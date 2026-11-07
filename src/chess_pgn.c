/* ---- laplace pgn: the read edge. Everything under the IDs named, fetched a level at a time, then written as PGN */
#include "chess_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { lp_id id; uint32_t first, n; uint8_t mask0, got; } DNode;            /* got: 1 fetched, 2 not recorded */
typedef struct { DNode *v; size_t n, cap; lp_idmap *m; lp_id *kid; size_t nk, ck; } Db;
static int64_t db_find(Db *d, const lp_id *id){ return d->m ? lp_idmap_find(d->m, id) : -1; }
static int64_t db_add(Db *d, const lp_id *id){ if (!d->m) d->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(d->m, id, &fresh); if (!fresh) return (int64_t)i;
    lp_reserve((void **)&d->v, &d->cap, d->n + 1, sizeof(DNode)); DNode *x = &d->v[d->n++]; memset(x, 0, sizeof *x); x->id = *id; return (int64_t)i; }
static int is_cp(const lp_id *id){ return lp_tier0_codepoint(T0, id) >= 0; }
/* Every node under the roots, a level a round trip: its constituents, runs written out, and its mask's first byte. */
static void db_fetch(PGconn *pg, Db *d, const lp_id *roots, size_t nr){
    lp_vec(lp_id) want = { 0 };
    for (size_t i = 0; i < nr; i++) if (!is_cp(&roots[i]) && db_find(d, &roots[i]) < 0) { db_add(d, &roots[i]); lp_push(&want, roots[i]); }
    while (want.n) {
        lp_vec(lp_id) next = { 0 };
        for (size_t o = 0; o < want.n; o += 20000) { size_t k = want.n - o < 20000 ? want.n - o : 20000; Args a = { 0 }; arg_ids(&a, want.v + o, k);
            PGresult *q = ask(pg, "SELECT entity, path, mask FROM @extschema@.physicality WHERE entity OPERATOR(@extschema@.=) ANY($1::@extschema@.blake3[])", &a);
            for (size_t j = 0; j < k; j++) d->v[db_find(d, &want.v[o + j])].got = 2;
            for (int r = 0; r < PQntuples(q); r++) { int64_t at = db_find(d, col_id(q, r, 0)); if (at < 0) continue;
                lp_path p = col_path(q, r, 1); size_t nv = p.n; lp_vertex *vx = malloc(sizeof(lp_vertex) * (nv ? nv : 1)); lp_path_decode(p, vx, nv);
                size_t tot = 0; for (size_t v = 0; v < nv; v++) tot += vx[v].run;
                lp_reserve((void **)&d->kid, &d->ck, d->nk + tot, sizeof(lp_id));
                d->v[at].first = (uint32_t)d->nk; d->v[at].n = (uint32_t)tot; d->v[at].got = 1;
                const uint8_t *mk = (const uint8_t *)PQgetvalue(q, r, 2); d->v[at].mask0 = PQgetlength(q, r, 2) > 4 ? mk[4] : 0;
                for (size_t v = 0; v < nv; v++) for (uint32_t z = 0; z < vx[v].run; z++) d->kid[d->nk++] = vx[v].id;
                for (size_t v = 0; v < nv; v++) if (!is_cp(&vx[v].id) && db_find(d, &vx[v].id) < 0) { db_add(d, &vx[v].id); lp_push(&next, vx[v].id); }
                free(vx); }
            PQclear(q); args_free(&a); }
        lp_vec_free(&want); want.v = next.v; want.n = next.n; want.cap = next.cap; }
    lp_vec_free(&want);
}
static const DNode *dn(Db *d, const lp_id *id){ int64_t i = db_find(d, id); return i < 0 || d->v[i].got != 1 ? NULL : &d->v[i]; }
static size_t kids(Db *d, const lp_id *id, const lp_id **out){ const DNode *x = dn(d, id); if (!x) { *out = NULL; return 0; } *out = d->kid + x->first; return x->n; }
typedef struct { char *b; size_t n, cap; } Str;
static void sput(Str *s, const char *t, size_t n){ lp_reserve((void **)&s->b, &s->cap, s->n + n + 1, 1); memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0; }
static void sputs(Str *s, const char *t){ sput(s, t, strlen(t)); }
static int text_into(Db *d, const lp_id *id, Str *o, int depth){
    int64_t cp = lp_tier0_codepoint(T0, id); if (cp >= 0) { uint8_t u[4]; size_t l = lp_utf8_put((uint32_t)cp, u); sput(o, (const char *)u, l); return 1; }
    const lp_id *k; size_t n = kids(d, id, &k); if (!n || depth > 64) return 0;
    for (size_t i = 0; i < n; i++) if (!text_into(d, &k[i], o, depth + 1)) return 0;
    return 1;
}
/* a text of one word: a codepoint, or a composition of codepoints alone (a tag's name) */
static int one_word(Db *d, const lp_id *id){ if (is_cp(id)) return 1; const lp_id *k; size_t n = kids(d, id, &k); if (!n) return 0; for (size_t i = 0; i < n; i++) if (!is_cp(&k[i])) return 0; return 1; }
/* A position from its content; 0 when it is none. */
static int pos_decode(Db *d, const lp_id *id, Bd *b){
    const lp_id *k; if (kids(d, id, &k) != 4) return 0; int64_t side = lp_tier0_codepoint(T0, &k[1]); if (side != 'w' && side != 'b') return 0;
    memset(b, 0, sizeof *b); b->side = side == 'b'; b->ep = -1;
    const lp_id *ps; size_t np = kids(d, &k[0], &ps); if (np < 2) return 0;
    for (size_t i = 0; i < np; i++) { const lp_id *two, *sq; if (kids(d, &ps[i], &two) != 2 || kids(d, &two[1], &sq) != 2) return 0; int64_t pc = lp_tier0_codepoint(T0, &two[0]);
        int64_t fch = lp_tier0_codepoint(T0, &sq[0]), rch = lp_tier0_codepoint(T0, &sq[1]); const char *w = pc > 0 && pc < 128 && pc != '.' ? strchr(PIECES, toupper((int)pc)) : NULL;
        if (!w || fch < 'a' || fch > 'h' || rch < '1' || rch > '8') return 0;
        b->sq[(rch - '1') * 8 + (fch - 'a')] = (int8_t)((w - PIECES) * (isupper((int)pc) ? 1 : -1)); }
    const lp_id *cm; if (kids(d, &k[2], &cm) != 2) return 0;
    for (int c = 0; c < 2; c++) { Str t = { 0 }; if (!text_into(d, &cm[c], &t, 0)) { free(t.b); return 0; } unsigned m = (unsigned)atoi(t.b); free(t.b);
        int ks = king_sq(b, c); for (int f = 0; f < 8; f++) if ((m & (0x80u >> f)) && !(ks >= 0 && (ks & 7) == f)) b->rights[c] |= (uint8_t)(1u << f); }
    const lp_id *e; if (kids(d, &k[3], &e) == 2) b->ep = (int8_t)((lp_tier0_codepoint(T0, &e[0]) - 'a') + 8 * (lp_tier0_codepoint(T0, &e[1]) - '1'));
    return 1;
}
/* A line's positions and moves: P0 alone for a line of no moves (it is its start position). */
static size_t line_of(Db *d, const lp_id *id, const lp_id **L, const lp_id **P0){
    Bd x; const lp_id *k; size_t n = kids(d, id, &k);
    if (n == 4 && pos_decode(d, id, &x)) { *L = id; *P0 = id; return 0; }
    *L = k; *P0 = k; return n >= 3 && n % 2 == 1 ? (n - 1) / 2 : 0;
}
static int is_line(Db *d, const lp_id *id){ Bd x; const lp_id *k; size_t n = kids(d, id, &k); if (n == 4 && pos_decode(d, id, &x)) return 1; return n >= 3 && n % 2 == 1 && pos_decode(d, &k[0], &x); }
typedef struct { uint64_t records, zero, plies, replay_bad, ids, id_bad, leaves, leaf_bad, dangling, c960, c960_start, c960_castles, castles, positions, arrivals, transposed, multi, splits, text_kept, nags, comments, variations; } Stat;
static int nag_text(const char *s){ if (*s == '$') { for (s++; *s; s++) if (!isdigit((unsigned char)*s)) return 0; return 1; } if (!*s) return 0; for (; *s; s++) if (*s != '!' && *s != '?') return 0; return 1; }
typedef struct { lp_idmap *first, *edge; lp_strmap *key; } Seen;     /* first: each position's first way in; edge: each way in; key: each position's FEN-like key, to its ID */
static void movetext(Db *d, const lp_id *lineid, const lp_id *layerid, Bd b, int fullmove, Str *o, Stat *st, Seen *sn);
/* A ply's annotations, or a line's before its first move: comments, NAGs and variations as written. Returns whether a
 * comment or a variation stands last (Black's next move is then numbered again). */
static int anns(Db *d, const lp_id *k, size_t n, const lp_id *Pbefore, const Bd *before, int fullmove, Str *o, Stat *st, Seen *sn){
    (void)sn;
    int brk = 0;
    for (size_t j = 0; j < n; j++) {
        if (is_line(d, &k[j])) { const lp_id *L, *P0; line_of(d, &k[j], &L, &P0);
            if (lp_id_eq(P0, Pbefore)) { sputs(o, " ("); movetext(d, &k[j], NULL, *before, fullmove, o, st, NULL); sputs(o, ")"); brk = 1; st->variations++; continue; } }
        const lp_id *vk; size_t vn = kids(d, &k[j], &vk);
        if (vn == 2 && is_line(d, &vk[0])) { const lp_id *L, *P0; line_of(d, &vk[0], &L, &P0);
            if (lp_id_eq(P0, Pbefore)) { sputs(o, " ("); movetext(d, &vk[0], &vk[1], *before, fullmove, o, st, NULL); sputs(o, ")"); brk = 1; st->variations++; continue; } }
        Str t = { 0 }; text_into(d, &k[j], &t, 0); if (!t.b) { sputs(o, " {}"); brk = 1; continue; }
        if (nag_text(t.b)) { if (*t.b == '$') sputs(o, " "); sputs(o, t.b); st->nags++; }
        else { sputs(o, " {"); sputs(o, t.b); sputs(o, "}"); brk = 1; st->comments++; }
        free(t.b); }
    return brk;
}
static void pos_key(const Bd *b, char *out){
    char *o = out; for (int s = 0; s < 64; s++) *o++ = b->sq[s] ? (char)piece_cp(b->sq[s]) : '.';
    o += sprintf(o, " %c %u %u %d", b->side ? 'b' : 'w', castle_mask(b, 0), castle_mask(b, 1), ep_square(b));
}
/* A line's movetext, its layer's annotations in their places; the line's positions checked against what each move
 * makes of the one before. */
static void movetext(Db *d, const lp_id *lineid, const lp_id *layerid, Bd b, int fullmove, Str *o, Stat *st, Seen *sn){
    const lp_id *L, *P0; size_t nply = line_of(d, lineid, &L, &P0);
    /* the layer: an element a ply, with one before them for what stands before the first move; one element is itself */
    const lp_id *Y = NULL; size_t ny = 0; int lead = 0;
    if (layerid) { const lp_id *yk; size_t yn = kids(d, layerid, &yk);
        if (nply == 0) { Y = layerid; ny = 1; lead = 1; }
        else if (nply == 1) { const lp_id *e0; size_t n0 = yn ? kids(d, &yk[0], &e0) : 0;
            if (yn == 2 && n0 && lp_id_eq(&e0[0], P0)) { Y = yk; ny = 2; lead = 1; } else { Y = layerid; ny = 1; } }
        else { Y = yk; ny = yn; lead = yn == nply + 1; } }
    int brk = 1, first = 1;
    if (lead) { const lp_id *e; size_t ne = kids(d, &Y[0], &e); if (ne > 1) { Str t = { 0 }; anns(d, e + 1, ne - 1, P0, &b, fullmove, &t, st, NULL);
            if (t.n) { size_t sk = t.b[0] == ' '; sput(o, t.b + sk, t.n - sk); first = 0; } free(t.b); } }
    for (size_t i = 0; i < nply; i++) {
        const lp_id *Pb = &L[2 * i], *M = &L[2 * i + 1], *Pa = &L[2 * i + 2];
        const lp_id *mk; size_t nm = kids(d, M, &mk); Mv all[256]; int na = gen(&b, all), hit = -1;
        if (nm >= 3) { const lp_id *f2, *t2; if (kids(d, &mk[1], &f2) == 2 && kids(d, &mk[2], &t2) == 2) {
            int from = (int)(lp_tier0_codepoint(T0, &f2[0]) - 'a') + 8 * (int)(lp_tier0_codepoint(T0, &f2[1]) - '1'), to = (int)(lp_tier0_codepoint(T0, &t2[0]) - 'a') + 8 * (int)(lp_tier0_codepoint(T0, &t2[1]) - '1');
            int promo = 0; if (nm == 4) { int64_t pc = lp_tier0_codepoint(T0, &mk[3]); const char *w = pc > 0 && pc < 128 ? strchr(PIECES, toupper((int)pc)) : NULL; promo = w ? (int)(w - PIECES) : 0; }
            for (int z = 0; z < na; z++) if (all[z].from == from && all[z].to == to && all[z].promo == promo) { hit = z; break; } } }
        if (hit < 0) { st->replay_bad++; sputs(o, " ?"); return; }
        lp_id want; id_move(&b, &all[hit], &want); if (!lp_id_eq(&want, M)) st->replay_bad++;
        char san[16], num[32]; san_of(&b, &all[hit], all, na, 1, san);
        if (b.side == 0) snprintf(num, sizeof num, "%s%d. ", first ? "" : " ", fullmove);
        else if (brk) snprintf(num, sizeof num, "%s%d... ", first ? "" : " ", fullmove);
        else snprintf(num, sizeof num, " ");
        sputs(o, num); sputs(o, san); first = 0; st->plies++;
        if (all[hit].castle) { st->castles++; if (b.c960) st->c960_castles++; }
        Bd before = b; int fm_before = fullmove; make(&b, &all[hit]); if (b.side == 0) fullmove++;
        lp_id pa; id_position(&b, &pa); if (!lp_id_eq(&pa, Pa)) st->replay_bad++;
        if (sn) { lp_id e2[2] = { *Pb, *M }, edge; lp_id_compose(e2, 2, &edge); st->arrivals++;
            bool fresh; lp_id *fst = lp_idmap_get(sn->first, Pa, &fresh);
            if (fresh) { *fst = edge; st->positions++; char key[96]; pos_key(&b, key); bool kf; lp_id *kid = lp_strmap_get(sn->key, key, strlen(key), &kf); if (kf) *kid = *Pa; else if (!lp_id_eq(kid, Pa)) st->splits++; }
            else if (!lp_id_eq(fst, &edge)) { st->transposed++; lp_id pe[2] = { *Pa, edge }, key2; lp_id_compose(pe, 2, &key2); bool f2; lp_idmap_put(sn->edge, &key2, &f2); if (f2) st->multi++; } }
        brk = 0;
        if (Y && i + (size_t)lead >= ny) st->replay_bad++;                     /* a layer shorter than its line */
        else if (Y) { const lp_id *Ai = &Y[i + (size_t)lead]; if (!lp_id_eq(Ai, M)) { const lp_id *e; size_t ne = kids(d, Ai, &e); if (ne > 1 && lp_id_eq(&e[0], M)) brk = anns(d, e + 1, ne - 1, Pb, &before, fm_before, o, st, NULL); else st->replay_bad++; } } }
}
/* A record's tags: [[name, value], ...], one tag its pair, a tag of no value its name alone. */
static void tags_pgn(Db *d, const lp_id *tags, Str *o, char *result, char *fen, char *variant){
    const lp_id *t; size_t nt = kids(d, tags, &t); const lp_id *list = t; size_t nl = nt; lp_id self = *tags;
    if (one_word(d, tags)) { list = &self; nl = 1; }                                   /* one tag, of no value */
    else if (nt == 2 && one_word(d, &t[0])) { list = &self; nl = 1; }                 /* one tag: its pair [name, value] */
    for (size_t i = 0; i < nl; i++) { Str nm = { 0 }, vv = { 0 };
        if (one_word(d, &list[i])) text_into(d, &list[i], &nm, 0);
        else { const lp_id *pv; if (kids(d, &list[i], &pv) == 2) { text_into(d, &pv[0], &nm, 0); text_into(d, &pv[1], &vv, 0); } }
        sputs(o, "["); if (nm.b) sputs(o, nm.b); sputs(o, " \"");
        for (size_t z = 0; z < vv.n; z++) { if (vv.b[z] == '"' || vv.b[z] == '\\') sputs(o, "\\"); sput(o, vv.b + z, 1); }
        sputs(o, "\"]\n");
        if (nm.b && vv.b) { if (!strcmp(nm.b, "Result")) snprintf(result, 16, "%s", vv.b); else if (!strcmp(nm.b, "FEN")) snprintf(fen, 128, "%s", vv.b); else if (!strcmp(nm.b, "Variant")) snprintf(variant, 64, "%s", vv.b); }
        free(nm.b); free(vv.b); }
}
/* A record as PGN: its tags, a blank line, its movetext and its result. */
static void record_pgn(Db *d, const lp_id *rec, Str *o, Stat *st, Seen *sn){
    const lp_id *k; size_t n = kids(d, rec, &k); if (!n) return;
    int envelope=n>1;
    for(size_t i=1;i<n && envelope;i++){const DNode *claim=dn(d,&k[i]);if(!claim || !(claim->mask0&(0x80u>>LP_KIND_CLAIM)))envelope=0;}
    if(envelope){record_pgn(d,&k[0],o,st,sn);return;}
    st->records++;
    const lp_id *line = NULL, *tags = NULL, *layer = NULL, *text = NULL;
    if (is_line(d, rec)) line = rec;                                                     /* a record of no tags and no annotations is its line */
    else if (is_line(d, &k[0])) { line = &k[0]; if (n > 1) tags = &k[1]; if (n > 2) layer = &k[2]; }
    else { tags = &k[0]; if (n > 1) text = &k[1]; st->text_kept++; }
    char result[16] = "*", fen[128] = "", variant[64] = "";
    if (tags) tags_pgn(d, tags, o, result, fen, variant);
    sputs(o, "\n");
    if (text) { Str t = { 0 }; text_into(d, text, &t, 0); if (t.b) sput(o, t.b, t.n); free(t.b); sputs(o, "\n\n"); return; }
    const Rules *ru = rules_named(variant[0] ? variant : NULL, strlen(variant)); Bd b; int fullmove = 1, c960 = ru && ru->c960;
    if (fen[0]) { board_fen(&b, fen, strlen(fen), c960); const char *sp = fen; for (int f = 0; f < 5 && sp; f++) { sp = strchr(sp, ' '); if (sp) sp++; } if (sp) fullmove = atoi(sp); if (fullmove < 1) fullmove = 1; }
    else board_start(&b);
    b.c960 = (uint8_t)c960;
    const lp_id *L, *P0; size_t nply = line_of(d, line, &L, &P0);
    { lp_id want; id_position(&b, &want); if (!lp_id_eq(&want, P0)) st->replay_bad++; else if (c960 && fen[0]) st->c960_start++; if (c960) st->c960++; }
    if (!nply) st->zero++;
    Str m = { 0 }; movetext(d, line, layer, b, fullmove, &m, st, sn); if (m.b) sput(o, m.b, m.n);
    if (m.n) sputs(o, " "); sputs(o, result); sputs(o, "\n\n"); free(m.b);
}
typedef lp_vec(lp_id) IdVec;
/* The records under a node, in its order: a record is what its mask says one is (LP_KIND_RECORD). */
static void records_in(Db *d, const lp_id *id, IdVec *out, int depth){
    const DNode *x = dn(d, id); if (!x || depth > 40) return;
    if (x->mask0 & (0x80u >> LP_KIND_RECORD)) { lp_push(out, *id); return; }
    const lp_id *k; size_t n = kids(d, id, &k);
    for (size_t i = 0; i < n; i++) records_in(d, &k[i], out, depth + 1);
}
/* --verify: every composition under the roots recomputed with BLAKE3 alone from its constituents' IDs, and every leaf
 * a codepoint whose ID is BLAKE3 of its UTF-8 bytes: nothing else is in any ID. */
static void verify_ids(Db *d, Stat *st){
    for (size_t i = 0; i < d->n; i++) { const DNode *x = &d->v[i]; if (x->got != 1) { st->dangling++; continue; } st->ids++;
        lp_id h; blake3_hasher bh; blake3_hasher_init(&bh); blake3_hasher_update(&bh, d->kid + x->first, 16 * (size_t)x->n); blake3_hasher_finalize(&bh, h.b, 16);
        if (x->n < 2 || !lp_id_eq(&h, &x->id)) st->id_bad++;                          /* a one-child composition is its child: never a node of its own */
        for (uint32_t k = 0; k < x->n; k++) { const lp_id *c = &d->kid[x->first + k]; int64_t cp = lp_tier0_codepoint(T0, c); if (cp < 0) continue; st->leaves++;
            uint8_t u[4]; size_t l = lp_utf8_put((uint32_t)cp, u); blake3_hasher_init(&bh); blake3_hasher_update(&bh, u, l); blake3_hasher_finalize(&bh, h.b, 16); if (!lp_id_eq(&h, c)) st->leaf_bad++; } }
}
int cmd_pgn(int argc, char **argv){
    const char *conninfo = laplace_db(), *outp = NULL, *t0p = NULL; int verify = 0;
    int a = opts(argc, argv, (const Opt[]){ { .flag = "-d", .kind = 's', .at = &conninfo }, { .flag = "-t", .kind = 's', .at = &t0p }, { .flag = "-o", .kind = 's', .at = &outp }, { .flag = "--verify", .kind = 'b', .at = &verify }, { 0 } });
    if (a >= argc) { fprintf(stderr, "usage: laplace pgn [-d conninfo] [-o file] [--verify] ID...   (a file's, a record's or a line's ID)\n"); return 2; }
    tier0_open(t0p); rules_load();
    IdVec roots = { 0 }; for (int i = a; i < argc; i++) { lp_id x; if (!id_parse(argv[i], &x)) { fprintf(stderr, "%s is no ID\n", argv[i]); return 2; } lp_push(&roots, x); }
    PGconn *pg = db_read(conninfo); double t = now(); Db d = { 0 };
    fprintf(stderr, "laplace pgn: database %s\n", PQdb(pg));
    db_fetch(pg, &d, roots.v, roots.n);
    fprintf(stderr, "  %zu nodes fetched in %.2f s\n", d.n, now() - t);
    Stat st = { 0 }; Seen sn = { lp_idmap_sized(sizeof(lp_id)), lp_idmap_new(), lp_strmap_sized(sizeof(lp_id)) };
    FILE *out = outp ? fopen(outp, "wb") : stdout; if (!out) { perror(outp); return 1; }
    for (size_t i = 0; i < roots.n; i++) { IdVec recs = { 0 }; records_in(&d, &roots.v[i], &recs, 0);
        if (!recs.n && dn(&d, &roots.v[i])) lp_push(&recs, roots.v[i]);
        for (size_t r = 0; r < recs.n; r++) { Str o = { 0 }; record_pgn(&d, &recs.v[r], &o, &st, &sn); if (o.n) fwrite(o.b, 1, o.n, out); free(o.b); }
        lp_vec_free(&recs); }
    if (outp) fclose(out);
    if (verify) verify_ids(&d, &st);
    fprintf(stderr, "  %llu records, %llu plies, %llu castlings; %llu records of no moves; %llu kept as text; %llu comments, %llu NAGs, %llu variations\n", (unsigned long long)st.records, (unsigned long long)st.plies, (unsigned long long)st.castles,
            (unsigned long long)st.zero, (unsigned long long)st.text_kept, (unsigned long long)st.comments, (unsigned long long)st.nags, (unsigned long long)st.variations);
    fprintf(stderr, "  replay: %llu moves, positions or annotations that are not what the position before makes of them\n", (unsigned long long)st.replay_bad);
    fprintf(stderr, "  Chess960: %llu records, %llu starting at their FEN's position, %llu castlings\n", (unsigned long long)st.c960, (unsigned long long)st.c960_start, (unsigned long long)st.c960_castles);
    fprintf(stderr, "  positions after a move: %llu distinct of %llu arrivals; %llu arrivals by another way than a position's first (%llu distinct ways), on one node each; %llu positions split over two IDs\n",
            (unsigned long long)st.positions, (unsigned long long)st.arrivals, (unsigned long long)st.transposed, (unsigned long long)st.multi, (unsigned long long)st.splits);
    if (verify) fprintf(stderr, "  verify: %llu compositions, %llu whose ID is not BLAKE3 of its constituents' IDs; %llu leaves, %llu not BLAKE3 of their codepoint's UTF-8; %llu constituents not recorded\n",
                        (unsigned long long)st.ids, (unsigned long long)st.id_bad, (unsigned long long)st.leaves, (unsigned long long)st.leaf_bad, (unsigned long long)st.dangling);
    PQfinish(pg); return (st.replay_bad || st.id_bad || st.leaf_bad || st.dangling || st.splits) ? 1 : 0;
}
