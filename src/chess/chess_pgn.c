/* ---- laplace pgn: the read edge. Everything under the IDs named, fetched a level at a time, then written as PGN */
#include "chess_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { lp_id id; uint32_t first, n; uint64_t mask; uint8_t got; } DNode;            /* got: 1 fetched, 2 not recorded */
typedef struct { DNode *v; size_t n, cap; lp_idmap *m; lp_id *kid; uint8_t *ksaid; size_t nk, ck; } Db;

static int64_t db_find(Db *d, const lp_id *id){ return d->m ? lp_idmap_find(d->m, id) : -1; }
static int64_t db_add(Db *d, const lp_id *id){
    if (!d->m) d->m = lp_idmap_new();
    bool fresh; size_t i = lp_idmap_put(d->m, id, &fresh); if (!fresh) return (int64_t)i;
    lp_reserve((void **)&d->v, &d->cap, d->n + 1, sizeof(DNode));
    DNode *x = &d->v[d->n++]; memset(x, 0, sizeof *x); x->id = *id; return (int64_t)i;
}
static int is_cp(const lp_id *id){ return lp_tier0_codepoint(T0, id) >= 0; }
static int cp_is(const lp_id *id, uint32_t c){ return lp_tier0_codepoint(T0, id) == (int64_t)c; }

static void db_fetch(PGconn *pg, Db *d, const lp_id *roots, size_t nr){
    lp_vec(lp_id) want = { 0 };
    for (size_t i = 0; i < nr; i++) if (!is_cp(&roots[i]) && db_find(d, &roots[i]) < 0) { db_add(d, &roots[i]); lp_push(&want, roots[i]); }
    while (want.n) {
        lp_vec(lp_id) next = { 0 };
        for (size_t o = 0; o < want.n; o += 20000) {
            size_t k = want.n - o < 20000 ? want.n - o : 20000; Args a = { 0 }; arg_ids(&a, want.v + o, k);
            PGresult *q = db_call(pg, LP_SQL_PHYSICALITY_BATCH, &a);
            for (size_t j = 0; j < k; j++) d->v[db_find(d, &want.v[o + j])].got = 2;
            for (int r = 0; r < PQntuples(q); r++) {
                int64_t at = db_find(d, col_id(q, r, 0)); if (at < 0) continue;
                lp_path p = col_path(q, r, 1); size_t nv = p.n;
                lp_vertex *vx = malloc(sizeof(lp_vertex) * (nv ? nv : 1)); lp_path_decode(p, vx, nv);
                size_t tot = 0; for (size_t v = 0; v < nv; v++) tot += vx[v].run;
                if (d->nk + tot > d->ck) {
                    size_t next_ck = d->ck ? d->ck * 2 : 1024;
                    while (next_ck < d->nk + tot) next_ck *= 2;
                    d->kid = realloc(d->kid, sizeof(lp_id) * next_ck);
                    d->ksaid = realloc(d->ksaid, next_ck);
                    d->ck = next_ck;
                }
                d->v[at].first = (uint32_t)d->nk; d->v[at].n = (uint32_t)tot; d->v[at].got = 1;
                d->v[at].mask = (uint64_t)col_i64(q, r, 2);
                for (size_t v = 0; v < nv; v++) for (uint32_t z = 0; z < vx[v].run; z++) {
                    d->ksaid[d->nk] = (uint8_t)vx[v].said;
                    d->kid[d->nk++] = vx[v].id;
                }
                for (size_t v = 0; v < nv; v++) if (!is_cp(&vx[v].id) && db_find(d, &vx[v].id) < 0) { db_add(d, &vx[v].id); lp_push(&next, vx[v].id); }
                free(vx);
            }
            PQclear(q); args_free(&a);
        }
        lp_vec_free(&want); want.v = next.v; want.n = next.n; want.cap = next.cap;
    }
    lp_vec_free(&want);
}

static const DNode *dn(Db *d, const lp_id *id){ int64_t i = db_find(d, id); return i < 0 || d->v[i].got != 1 ? NULL : &d->v[i]; }
static size_t kids(Db *d, const lp_id *id, const lp_id **out){ const DNode *x = dn(d, id); if (!x) { *out = NULL; return 0; } *out = d->kid + x->first; return x->n; }
static size_t kids_said(Db *d, const lp_id *id, const lp_id **out, const uint8_t **said){
    const DNode *x = dn(d, id);
    if (!x) { *out = NULL; *said = NULL; return 0; }
    *out = d->kid + x->first; *said = d->ksaid + x->first; return x->n;
}

typedef struct { char *b; size_t n, cap; } Str;
static void sput(Str *s, const char *t, size_t n){ lp_reserve((void **)&s->b, &s->cap, s->n + n + 1, 1); memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0; }
static void sputs(Str *s, const char *t){ sput(s, t, strlen(t)); }

static int text_into(Db *d, const lp_id *id, Str *o, int depth){
    int64_t cp = lp_tier0_codepoint(T0, id);
    if (cp >= 0) { uint8_t u[4]; size_t l = lp_utf8_put((uint32_t)cp, u); sput(o, (const char *)u, l); return 1; }
    const lp_id *k; size_t n = kids(d, id, &k); if (!n || depth > 64) return 0;
    for (size_t i = 0; i < n; i++) if (!text_into(d, &k[i], o, depth + 1)) return 0;
    return 1;
}
static int uint_of(Db *d, const lp_id *id, unsigned *out){
    Str t = { 0 };
    if (!text_into(d, id, &t, 0) || !t.n || t.n > 9) { free(t.b); return 0; }
    unsigned v = 0;
    for (size_t i = 0; i < t.n; i++) {
        if (t.b[i] < '0' || t.b[i] > '9') { free(t.b); return 0; }
        v = v * 10u + (unsigned)(t.b[i] - '0');
    }
    free(t.b); *out = v; return 1;
}

static int extra_decode(Db *d, const lp_id *id, Bd *b){
    Str t = { 0 }; if (!text_into(d, id, &t, 0) || !t.n) { free(t.b); return 0; }
    int ok = 1;
    if (t.b[0] == '+') { unsigned w, k; ok = sscanf(t.b, "+%u+%u", &w, &k) == 2 && w < 256 && k < 256; if (ok) { b->checks[0] = (uint8_t)w; b->checks[1] = (uint8_t)k; } }
    else if (strspn(t.b, "QRBNPqrbnp") == t.n) { for (size_t i = 0; i < t.n; i++) { int kind = (int)(strchr(PIECES, toupper((unsigned char)t.b[i])) - PIECES); b->hand[islower((unsigned char)t.b[i]) ? 1 : 0][kind]++; } }
    else { if (t.n % 2) ok = 0; for (size_t i = 0; ok && i < t.n; i += 2) { if (t.b[i] < 'a' || t.b[i] > 'h' || t.b[i + 1] < '1' || t.b[i + 1] > '8') ok = 0; else b->promoted |= (uint64_t)1 << ((t.b[i] - 'a') + 8 * (t.b[i + 1] - '1')); } }
    free(t.b); return ok;
}
static int pos_decode_law(Db *d, const lp_id *id, Bd *b, const LpChessLaw *law){
    const lp_id *k; size_t nk = kids(d, id, &k); if (nk < 4 || nk > 7) return 0;
    int64_t side = lp_tier0_codepoint(T0, &k[1]); if (side != 'w' && side != 'b') return 0;
    memset(b, 0, sizeof *b); b->side = side == 'b'; b->ep = -1; b->law = law; b->c960 = (uint8_t)(law && law->c960);
    for (size_t i = 4; i < nk; i++) if (!extra_decode(d, &k[i], b)) return 0;
    const lp_id *ps; size_t np = kids(d, &k[0], &ps); if (np < 2) return 0;
    for (size_t i = 0; i < np; i++) {
        const lp_id *two, *sq; if (kids(d, &ps[i], &two) != 2 || kids(d, &two[1], &sq) != 2) return 0;
        int64_t pc = lp_tier0_codepoint(T0, &two[0]), fch = lp_tier0_codepoint(T0, &sq[0]), rch = lp_tier0_codepoint(T0, &sq[1]);
        const char *w = pc > 0 && pc < 128 && pc != '.' ? strchr(PIECES, toupper((int)pc)) : NULL;
        if (!w || fch < 'a' || fch > 'h' || rch < '1' || rch > '8') return 0;
        b->sq[(rch - '1') * 8 + (fch - 'a')] = (int8_t)((w - PIECES) * (isupper((int)pc) ? 1 : -1));
    }
    const lp_id *cm; if (kids(d, &k[2], &cm) != 2) return 0;
    for (int c = 0; c < 2; c++) {
        unsigned m = 0; if (!uint_of(d, &cm[c], &m)) return 0;
        int ks = king_sq(b, c);
        for (int f = 0; f < 8; f++) if ((m & (0x80u >> f)) && !(ks >= 0 && (ks & 7) == f)) b->rights[c] |= (uint8_t)(1u << f);
    }
    const lp_id *e; if (kids(d, &k[3], &e) == 2) b->ep = (int8_t)((lp_tier0_codepoint(T0, &e[0]) - 'a') + 8 * (lp_tier0_codepoint(T0, &e[1]) - '1'));
    return 1;
}
static int pos_decode(Db *d, const lp_id *id, Bd *b){ return pos_decode_law(d, id, b, NULL); }

/* A line [P0, P1, ..., Pn] (or P0 alone for 0 plies): returns nply, sets *L = array of n+1 positions and *P0 = &L[0]. */
static size_t line_of(Db *d, const lp_id *id, const lp_id **L, const lp_id **P0){
    Bd x; const lp_id *k; const uint8_t *ks; size_t n = kids_said(d, id, &k, &ks);
    if (n >= 4 && n <= 7 && pos_decode(d, id, &x)) { *L = id; *P0 = id; return 0; }
    if (n >= 2 && ks[0] == 0 && ks[1] == 0 && pos_decode(d, &k[0], &x) && pos_decode(d, &k[1], &x)) {
        *L = k; *P0 = &k[0]; return n - 1;
    }
    *L = NULL; *P0 = NULL; return 0;
}
static int is_line(Db *d, const lp_id *id){
    const lp_id *L, *P0; (void)line_of(d, id, &L, &P0); return P0 != NULL;
}

typedef struct { uint64_t records, zero, plies, replay_bad, ids, id_bad, leaves, leaf_bad, dangling, c960, c960_start, c960_castles, castles, positions, arrivals, transposed, multi, splits, text_kept, nags, comments, variations; } Stat;
static int nag_text(const char *s){
    if (*s == '$') { for (s++; *s; s++) if (!isdigit((unsigned char)*s)) return 0; return 1; }
    if (!*s) return 0;
    for (; *s; s++) if (*s != '!' && *s != '?') return 0;
    return 1;
}
typedef struct { lp_idmap *first, *edge; lp_strmap *key; } Seen;

static void movetext(Db *d, const lp_id *lineid, const lp_id *clocksid, const lp_id *notesid, Bd b, int fullmove, Str *o, Stat *st, Seen *sn);

static int emit_note_items(Db *d, const lp_id *k, size_t n, const lp_id *Pbefore, const Bd *before, int fullmove, Str *o, Stat *st){
    int brk = 0;
    for (size_t j = 0; j < n; j++) {
        if (is_line(d, &k[j])) {
            const lp_id *L, *P0; line_of(d, &k[j], &L, &P0);
            if (P0 && lp_id_eq(P0, Pbefore)) { sputs(o, " ("); movetext(d, &k[j], NULL, NULL, *before, fullmove, o, st, NULL); sputs(o, ")"); brk = 1; st->variations++; continue; }
        }
        const lp_id *vk; size_t vn = kids(d, &k[j], &vk);
        if (vn == 2 && is_line(d, &vk[0])) {
            const lp_id *L, *P0; line_of(d, &vk[0], &L, &P0);
            if (P0 && lp_id_eq(P0, Pbefore)) { sputs(o, " ("); movetext(d, &vk[0], NULL, &vk[1], *before, fullmove, o, st, NULL); sputs(o, ")"); brk = 1; st->variations++; continue; }
        }
        Str t = { 0 }; text_into(d, &k[j], &t, 0);
        if (!t.b) { sputs(o, " {}"); brk = 1; continue; }
        if (nag_text(t.b)) { if (*t.b == '$') sputs(o, " "); sputs(o, t.b); st->nags++; }
        else { sputs(o, " {"); sputs(o, t.b); sputs(o, "}"); brk = 1; st->comments++; }
        free(t.b);
    }
    return brk;
}
static void format_clk(unsigned ds, char out[32]){
    unsigned secs = ds / 10u, tenths = ds % 10u;
    unsigned h = secs / 3600u, m = (secs % 3600u) / 60u, s = secs % 60u;
    if (tenths) snprintf(out, 32, "%u:%02u:%02u.%u", h, m, s, tenths);
    else snprintf(out, 32, "%u:%02u:%02u", h, m, s);
}
static void pos_key(const Bd *b, char *out){
    char *o = out; for (int s = 0; s < 64; s++) *o++ = b->sq[s] ? (char)piece_cp(b->sq[s]) : '.';
    o += sprintf(o, " %c %u %u %d", b->side ? 'b' : 'w', castle_mask(b, 0), castle_mask(b, 1), ep_square(b));
    LpChessExtra x; lp_chess_extra(b, &x); *o++ = ' '; for (int i = 0; i < x.nhand; i++) *o++ = (char)x.hand[i];
    for (int i = 0; i < x.npromoted; i++) o += sprintf(o, " %d", x.promoted[i]); *o++ = ' '; for (int i = 0; i < x.nchecks; i++) *o++ = (char)x.checks[i]; *o = 0;
}

static void unpack_notes(Db *d, const lp_id *notesid, const lp_id **entries, size_t *nentries, lp_id *single){
    *entries = NULL; *nentries = 0;
    if (!notesid || cp_is(notesid, '-')) return;
    const lp_id *k; const uint8_t *ks; size_t n = kids_said(d, notesid, &k, &ks);
    if (!n) return;
    if (ks[0] == LP_SAID_TUPLE) { *entries = k; *nentries = n; }
    else { *single = *notesid; *entries = single; *nentries = 1; }
}

static void movetext(Db *d, const lp_id *lineid, const lp_id *clocksid, const lp_id *notesid, Bd b, int fullmove, Str *o, Stat *st, Seen *sn){
    const lp_id *L, *P0; size_t nply = line_of(d, lineid, &L, &P0);
    if (!P0) { st->replay_bad++; return; }

    const lp_id *clk_arr = NULL; lp_id single_clk;
    if (clocksid && !cp_is(clocksid, '-') && nply > 0) {
        if (nply == 1) { single_clk = *clocksid; clk_arr = &single_clk; }
        else { size_t nc = kids(d, clocksid, &clk_arr); if (nc != nply) { st->replay_bad++; clk_arr = NULL; } }
    }

    const lp_id *nentries = NULL; size_t nn = 0; lp_id single_note;
    unpack_notes(d, notesid, &nentries, &nn, &single_note);
    size_t nat = 0;
    int brk = 1, first = 1;

    if (nat < nn) {
        const lp_id *ek; size_t en = kids(d, &nentries[nat], &ek); unsigned idx = 0;
        if (en > 1 && uint_of(d, &ek[0], &idx) && idx == 0) {
            Str t = { 0 };
            emit_note_items(d, ek + 1, en - 1, P0, &b, fullmove, &t, st);
            if (t.n) { size_t sk = t.b[0] == ' '; sput(o, t.b + sk, t.n - sk); first = 0; }
            free(t.b);
            nat++;
        }
    }

    for (size_t i = 0; i < nply; i++) {
        const lp_id *Pb = &L[i], *Pa = &L[i + 1];
        Mv all[LP_CHESS_MOVES]; int na = 0;
        int hit = lp_chess_find_transition(&b, Pa, all, &na);
        if (hit < 0) { st->replay_bad++; sputs(o, " ?"); return; }
        char san[16], num[32]; san_of(&b, &all[hit], all, na, 1, san);
        if (b.side == 0) snprintf(num, sizeof num, "%s%d. ", first ? "" : " ", fullmove);
        else if (brk) snprintf(num, sizeof num, "%s%d... ", first ? "" : " ", fullmove);
        else snprintf(num, sizeof num, " ");
        sputs(o, num); sputs(o, san); first = 0; st->plies++;
        if (all[hit].castle) { st->castles++; if (b.c960) st->c960_castles++; }

        Bd before = b; int fm_before = fullmove;
        make(&b, &all[hit]); if (b.side == 0) fullmove++;
        lp_id pa; id_position(&b, &pa); if (!lp_id_eq(&pa, Pa)) st->replay_bad++;

        if (sn) {
            lp_id e2[2] = { *Pb, *Pa }, edge; lp_id_compose(e2, 2, &edge); st->arrivals++;
            bool fresh; lp_id *fst = lp_idmap_get(sn->first, Pa, &fresh);
            if (fresh) {
                *fst = edge; st->positions++; char key[256]; pos_key(&b, key);
                bool kf; lp_id *kid = lp_strmap_get(sn->key, key, strlen(key), &kf);
                if (kf) *kid = *Pa; else if (!lp_id_eq(kid, Pa)) st->splits++;
            } else if (!lp_id_eq(fst, &edge)) {
                st->transposed++; lp_id pe[2] = { *Pa, edge }, key2; lp_id_compose(pe, 2, &key2);
                bool f2; lp_idmap_put(sn->edge, &key2, &f2); if (f2) st->multi++;
            }
        }

        brk = 0;
        if (clk_arr && !cp_is(&clk_arr[i], '-')) {
            unsigned ds = 0;
            if (uint_of(d, &clk_arr[i], &ds)) {
                char cb[32]; format_clk(ds, cb);
                sputs(o, " {[%clk "); sputs(o, cb); sputs(o, "]}");
                brk = 1; st->comments++;
            }
        }
        if (nat < nn) {
            const lp_id *ek; size_t en = kids(d, &nentries[nat], &ek); unsigned idx = 0;
            if (en > 1 && uint_of(d, &ek[0], &idx) && idx == (unsigned)(i + 1)) {
                if (emit_note_items(d, ek + 1, en - 1, Pb, &before, fm_before, o, st)) brk = 1;
                nat++;
            }
        }
    }
}

static void emit_tag_pair(Str *o, const char *name, const char *val, size_t vl){
    sputs(o, "["); sputs(o, name); sputs(o, " \"");
    for (size_t z = 0; z < vl; z++) { if (val[z] == '"' || val[z] == '\\') sputs(o, "\\"); sput(o, val + z, 1); }
    sputs(o, "\"]\n");
}
static void emit_tag_id(Db *d, Str *o, const char *name, const lp_id *val){
    if (!val || cp_is(val, '-')) return;
    Str vv = { 0 };
    if (text_into(d, val, &vv, 0) && vv.b) emit_tag_pair(o, name, vv.b, vv.n);
    free(vv.b);
}

static void record_pgn(Db *d, const lp_id *rec, Str *o, Stat *st, Seen *sn){
    const lp_id *k; size_t n = kids(d, rec, &k); if (!n) return;
    if (!is_line(d, rec) && n > 1) {
        int envelope = 1;
        for (size_t i = 1; i < n && envelope; i++) {
            const DNode *claim = dn(d, &k[i]);
            if (!claim || !(claim->mask & (UINT64_C(1) << LP_KIND_CLAIM))) envelope = 0;
        }
        if (envelope) { record_pgn(d, &k[0], o, st, sn); return; }
    }
    st->records++;

    const lp_id *line = NULL, *meta = NULL, *clocks = NULL, *notes = NULL, *raw_text = NULL;
    if (is_line(d, rec)) line = rec;
    else if (is_line(d, &k[0])) {
        line = &k[0];
        if (n > 1) meta = &k[1];
        if (n > 2 && !cp_is(&k[2], '-')) clocks = &k[2];
        if (n > 3 && !cp_is(&k[3], '-')) notes = &k[3];
    } else {
        const lp_id *mk;
        if (kids(d, &k[0], &mk) == 10) { meta = &k[0]; if (n > 1) raw_text = &k[1]; }
        else raw_text = rec;
        st->text_kept++;
    }

    char result[16] = "*", variant[64] = "", raw_fen[160] = "";
    unsigned start_half = 0, start_full = 1; int has_counters = 0;
    const lp_id *mk = NULL; size_t nmk = meta ? kids(d, meta, &mk) : 0;

    if (nmk == 10) {
        int64_t rcp = lp_tier0_codepoint(T0, &mk[1]);
        if (rcp == '1') snprintf(result, sizeof result, "1-0");
        else if (rcp == '0') snprintf(result, sizeof result, "0-1");
        else if (rcp == '=') snprintf(result, sizeof result, "1/2-1/2");

        emit_tag_id(d, o, "Event", &mk[3]);
        emit_tag_id(d, o, "Site", &mk[4]);

        if (!cp_is(&mk[2], '-')) {
            const lp_id *dk; unsigned y = 0, mo = 0, da = 0;
            if (kids(d, &mk[2], &dk) == 3 && uint_of(d, &dk[0], &y) && uint_of(d, &dk[1], &mo) && uint_of(d, &dk[2], &da)) {
                char ys[8], ms[4], ds[4], dbuf[20];
                if (y) snprintf(ys, sizeof ys, "%04u", y); else snprintf(ys, sizeof ys, "????");
                if (mo) snprintf(ms, sizeof ms, "%02u", mo); else snprintf(ms, sizeof ms, "??");
                if (da) snprintf(ds, sizeof ds, "%02u", da); else snprintf(ds, sizeof ds, "??");
                snprintf(dbuf, sizeof dbuf, "%s.%s.%s", ys, ms, ds);
                emit_tag_pair(o, "Date", dbuf, strlen(dbuf));
            } else emit_tag_id(d, o, "Date", &mk[2]);
        }

        emit_tag_id(d, o, "Round", &mk[5]);

        if (!cp_is(&mk[0], '-')) {
            const lp_id *sk;
            if (kids(d, &mk[0], &sk) == 2) {
                const char *const pnames[2] = { "White", "Black" };
                const char *const enames[2] = { "WhiteElo", "BlackElo" };
                const char *const fnames[2] = { "WhiteFideId", "BlackFideId" };
                const lp_id *elo_slot[2] = { NULL, NULL }, *fide_slot[2] = { NULL, NULL };
                for (int s = 0; s < 2; s++) {
                    const lp_id * pk; size_t npk = kids(d, &sk[s], &pk);
                    if (npk == 2) {
                        if (!cp_is(&pk[0], '?')) emit_tag_id(d, o, pnames[s], &pk[0]);
                        elo_slot[s] = &pk[1];
                    } else if (npk == 3) {
                        const lp_id *fk;
                        if (kids(d, &pk[0], &fk) == 2) fide_slot[s] = &fk[1];
                        if (!cp_is(&pk[1], '?')) emit_tag_id(d, o, pnames[s], &pk[1]);
                        elo_slot[s] = &pk[2];
                    }
                }
                emit_tag_pair(o, "Result", result, strlen(result));
                for (int s = 0; s < 2; s++) if (elo_slot[s]) emit_tag_id(d, o, enames[s], elo_slot[s]);
                for (int s = 0; s < 2; s++) if (fide_slot[s]) emit_tag_id(d, o, fnames[s], fide_slot[s]);
            } else emit_tag_pair(o, "Result", result, strlen(result));
        } else if (rcp != '*') {
            emit_tag_pair(o, "Result", result, strlen(result));
        }

        emit_tag_id(d, o, "ECO", &mk[6]);

        if (!cp_is(&mk[7], '-')) {
            const lp_id *tk; unsigned base = 0, inc = 0;
            if (kids(d, &mk[7], &tk) == 2 && uint_of(d, &tk[0], &base) && uint_of(d, &tk[1], &inc)) {
                char tbuf[32]; snprintf(tbuf, sizeof tbuf, "%u+%u", base, inc);
                emit_tag_pair(o, "TimeControl", tbuf, strlen(tbuf));
            } else emit_tag_id(d, o, "TimeControl", &mk[7]);
        }

        if (!cp_is(&mk[8], '-')) {
            Str vv = { 0 };
            if (text_into(d, &mk[8], &vv, 0) && vv.b) {
                snprintf(variant, sizeof variant, "%s", vv.b);
                emit_tag_pair(o, "Variant", vv.b, vv.n);
            }
            free(vv.b);
        }

        if (!cp_is(&mk[9], '-')) {
            const lp_id *ck;
            if (kids(d, &mk[9], &ck) == 2 && uint_of(d, &ck[0], &start_half) && uint_of(d, &ck[1], &start_full)) {
                has_counters = 1;
            } else {
                Str fv = { 0 };
                if (text_into(d, &mk[9], &fv, 0) && fv.b) snprintf(raw_fen, sizeof raw_fen, "%s", fv.b);
                free(fv.b);
            }
        }
    }

    if (raw_text) {
        if (raw_fen[0]) emit_tag_pair(o, "FEN", raw_fen, strlen(raw_fen));
        sputs(o, "\n");
        Str t = { 0 }; text_into(d, raw_text, &t, 0); if (t.b) sput(o, t.b, t.n); free(t.b);
        sputs(o, "\n\n");
        return;
    }

    const Rules *ru = rules_named(variant[0] ? variant : NULL, strlen(variant));
    if (!ru) { st->replay_bad++; return; }
    Bd b, std_b;
    board_law_start(&std_b, ru);
    lp_id std_id; id_position(&std_b, &std_id);

    const lp_id *L, *P0; size_t nply = line_of(d, line, &L, &P0);
    if (!P0 || !pos_decode_law(d, P0, &b, ru)) { st->replay_bad++; return; }
    { lp_id want; id_position(&b, &want); if (!lp_id_eq(&want, P0)) st->replay_bad++; }
    int c960 = ru->c960;
    if (c960) st->c960++;
    if (!lp_id_eq(P0, &std_id) || has_counters) {
        char fbuf[LP_CHESS_FEN + 32];
        size_t fl = lp_chess_fen_of(&b, fbuf);
        snprintf(fbuf + fl, sizeof fbuf - fl, " %u %u", start_half, start_full);
        emit_tag_pair(o, "SetUp", "1", 1);
        emit_tag_pair(o, "FEN", fbuf, strlen(fbuf));
        if (c960) st->c960_start++;
    }

    if (o->n) sputs(o, "\n");
    if (!nply) st->zero++;
    Str m = { 0 };
    movetext(d, line, clocks, notes, b, (int)start_full, &m, st, sn);
    if (m.b) sput(o, m.b, m.n);
    if (m.n) sputs(o, " ");
    sputs(o, result); sputs(o, "\n\n");
    free(m.b);
}

typedef lp_vec(lp_id) IdVec;
static void records_in(Db *d, const lp_id *id, IdVec *out, int depth){
    const DNode *x = dn(d, id); if (!x || depth > 40) return;
    if (x->mask & (UINT64_C(1) << LP_KIND_RECORD)) { lp_push(out, *id); return; }
    const lp_id *k; size_t n = kids(d, id, &k);
    for (size_t i = 0; i < n; i++) records_in(d, &k[i], out, depth + 1);
}

static void verify_ids(Db *d, Stat *st){
    for (size_t i = 0; i < d->n; i++) {
        const DNode *x = &d->v[i]; if (x->got != 1) { st->dangling++; continue; } st->ids++;
        lp_id h; blake3_hasher bh; blake3_hasher_init(&bh); blake3_hasher_update(&bh, d->kid + x->first, 16 * (size_t)x->n); blake3_hasher_finalize(&bh, h.b, 16);
        if (x->n < 2 || !lp_id_eq(&h, &x->id)) st->id_bad++;
        for (uint32_t k = 0; k < x->n; k++) {
            const lp_id *c = &d->kid[x->first + k]; int64_t cp = lp_tier0_codepoint(T0, c); if (cp < 0) continue; st->leaves++;
            uint8_t u[4]; size_t l = lp_utf8_put((uint32_t)cp, u);
            blake3_hasher_init(&bh); blake3_hasher_update(&bh, u, l); blake3_hasher_finalize(&bh, h.b, 16);
            if (!lp_id_eq(&h, c)) st->leaf_bad++;
        }
    }
}

int cmd_pgn(int argc, char **argv){
    const char *conninfo = laplace_db(), *outp = NULL, *t0p = NULL; int verify = 0;
    int a = opts(argc, argv, (const Opt[]){ { .flag = "-d", .kind = 's', .at = &conninfo }, { .flag = "-t", .kind = 's', .at = &t0p }, { .flag = "-o", .kind = 's', .at = &outp }, { .flag = "--verify", .kind = 'b', .at = &verify }, { 0 } });
    if (a >= argc) { fprintf(stderr, "usage: laplace pgn [-d conninfo] [-o file] [--verify] ID...   (a file's, a record's or a line's ID)\n"); return 2; }
    tier0_open(t0p); rules_load();
    IdVec roots = { 0 };
    for (int i = a; i < argc; i++) { lp_id x; if (!id_parse(argv[i], &x)) { fprintf(stderr, "%s is no ID\n", argv[i]); return 2; } lp_push(&roots, x); }
    PGconn *pg = db_read(conninfo); double t = now(); Db d = { 0 };
    fprintf(stderr, "laplace pgn: database %s\n", PQdb(pg));
    db_fetch(pg, &d, roots.v, roots.n);
    fprintf(stderr, "  %zu nodes fetched in %.2f s\n", d.n, now() - t);
    Stat st = { 0 }; Seen sn = { lp_idmap_sized(sizeof(lp_id)), lp_idmap_new(), lp_strmap_sized(sizeof(lp_id)) };
    FILE *out = outp ? fopen(outp, "wb") : stdout; if (!out) { perror(outp); return 1; }
    for (size_t i = 0; i < roots.n; i++) {
        IdVec recs = { 0 }; records_in(&d, &roots.v[i], &recs, 0);
        if (!recs.n && dn(&d, &roots.v[i])) lp_push(&recs, roots.v[i]);
        for (size_t r = 0; r < recs.n; r++) { Str o = { 0 }; record_pgn(&d, &recs.v[r], &o, &st, &sn); if (o.n) fwrite(o.b, 1, o.n, out); free(o.b); }
        lp_vec_free(&recs);
    }
    if (outp) fclose(out);
    if (verify) verify_ids(&d, &st);
    fprintf(stderr, "  %llu records, %llu plies, %llu castlings; %llu records of no moves; %llu kept as text; %llu comments, %llu NAGs, %llu variations\n",
            (unsigned long long)st.records, (unsigned long long)st.plies, (unsigned long long)st.castles,
            (unsigned long long)st.zero, (unsigned long long)st.text_kept, (unsigned long long)st.comments, (unsigned long long)st.nags, (unsigned long long)st.variations);
    fprintf(stderr, "  replay: %llu moves, positions or annotations that are not what the position before makes of them\n", (unsigned long long)st.replay_bad);
    fprintf(stderr, "  Chess960: %llu records, %llu starting at their FEN's position, %llu castlings\n", (unsigned long long)st.c960, (unsigned long long)st.c960_start, (unsigned long long)st.c960_castles);
    fprintf(stderr, "  positions after a move: %llu distinct of %llu arrivals; %llu arrivals by another way than a position's first (%llu distinct ways), on one node each; %llu positions split over two IDs\n",
            (unsigned long long)st.positions, (unsigned long long)st.arrivals, (unsigned long long)st.transposed, (unsigned long long)st.multi, (unsigned long long)st.splits);
    if (verify) fprintf(stderr, "  verify: %llu compositions, %llu whose ID is not BLAKE3 of its constituents' IDs; %llu leaves, %llu not BLAKE3 of their codepoint's UTF-8; %llu constituents not recorded\n",
                        (unsigned long long)st.ids, (unsigned long long)st.id_bad, (unsigned long long)st.leaves, (unsigned long long)st.leaf_bad, (unsigned long long)st.dangling);
    free(d.v); free(d.kid); free(d.ksaid); if (d.m) lp_idmap_free(d.m);
    lp_idmap_free(sn.first); lp_idmap_free(sn.edge); lp_strmap_free(sn.key); lp_vec_free(&roots);
    PQfinish(pg); return (st.replay_bad || st.id_bad || st.leaf_bad || st.dangling || st.splits) ? 1 : 0;
}
