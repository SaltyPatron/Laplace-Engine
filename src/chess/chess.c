/* Chess: PGN & FEN reader (Sequence 27.1-27.3, Corpora: Games, Conflicts K2).
 *
 * Pure state-to-state trajectories + altitude-scoped occurrences (zero KVP tag bags, zero stored FEN/[%clk] strings):
 *   square       [file, rank]                          two codepoints: e4 is [e, 4]
 *   piece        a codepoint, FEN's letter: K Q R B N P for White, k q r b n p for Black
 *   board        [[piece, square], ...]                the pieces in square order, a1 b1 ... h8
 *   castling     [White's mask, Black's mask]          each one 8-bit mask (27.1): bit 7 - f for file f; decimal digits
 *   position     [board, side, castling, en passant]   side w or b; en passant the square a pawn can take on legally
 *                                                      this move, else -; followed when non-empty by variant state
 *                                                      (hand, promoted squares, checks +W+B). Rules live in
 *                                                      firmware/chess.firmware, never in position hashes.
 *   line         [P0, P1, P2, ..., Pn]                 ordered trajectory of canonical board states (P0 alone when n=0)
 *   transition   [P_k, P_{k+1}]                        witnessed state transition scored by mover outcome (1, 1/2, 0)
 *   fide player  ["FIDE", digits(FideId)]              shared highway node across FIDE, PGN FideId tags, and Wikidata
 *   seat         [name, elo|-] or [fide, name, elo|-]  elo as compositional digits; positional seats = [seat_0, seat_1]
 *   meta         [seats, result, date, event, site,    10 positional game-altitude slots ('-' / '*' when absent, RLE-
 *                 round, eco, tc, variant, counters]   compressed in physicality.path); omitted whole when all default
 *   clocks       [c_0, c_1, ..., c_{n-1}]              integer deciseconds per ply ('-' where absent), omitted if none
 *   notes        [[ply_idx, item...], ...]             commentary, NAGs ([P_k, P_{k+1}, NAG]), and RAV variations
 *   record       line | [line, meta(, clocks, notes)]  occurrence record; unreplayable games keep [meta, raw_movetext]
 *
 * SAN, UCI, FEN, and PGN tag headers are synthesized at the read edge (chess_pgn.c, Laplace-MCP api_chess_games.c). */
#include "chess_internal.h"
#include "chess_cache.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rules_load(void){
    static int loaded; if (__atomic_load_n(&loaded, __ATOMIC_ACQUIRE)) return;
    char p[4300]; const char *e = getenv("LAPLACE_CHESS_FIRMWARE");
    if (e && *e) snprintf(p, sizeof p, "%s", e);
    else { snprintf(p, sizeof p, "%s", firmware_path()); char *s = strrchr(p, '/'), *b = strrchr(p, '\\'); if (b > s) s = b; if (s) snprintf(s + 1, sizeof p - (size_t)(s + 1 - p), "chess.firmware"); }
    lp_chess_laws_load(p); __atomic_store_n(&loaded, 1, __ATOMIC_RELEASE);
}
const Rules *rules_named(const char *v, size_t l){ rules_load(); return lp_chess_law_named(v, l); }

/* ---- vocabulary & scalar helpers ---- */
typedef struct {
    Ref ps[13][64]; uint8_t has[13][64];
    Ref sq[64]; uint8_t hsq[64];
    Ref fide_lit, fide_name_rel; uint8_t hfide;
} RefCache;

static Ref r_sq(RefCache *rc, int s){
    if (!rc->hsq[s]) {
        if (!chess_square(s, &rc->sq[s])) {
            Ref k[2] = { atom((uint32_t)('a' + (s & 7))), atom((uint32_t)('1' + (s >> 3))) };
            rc->sq[s] = compose(k, 2, ref_above(k, 2));
        }
        rc->hsq[s] = 1;
    }
    return rc->sq[s];
}
static Ref r_number(unsigned v){
    char d[16]; int l = snprintf(d, sizeof d, "%u", v); Ref k[16];
    for (int i = 0; i < l; i++) k[i] = atom((uint32_t)d[i]);
    Ref r = l == 1 ? k[0] : compose(k, (uint32_t)l, ref_above(k, (size_t)l));
    r.said = 0;
    return r;
}
static Ref r_text(const uint8_t *s, size_t n){
    Ref r = n > 256 ? text_ref(CTX[omp_get_thread_num()], s, n) : string_ref(s, n);
    r.said = 0;
    return r;
}
static void ensure_fide_refs(RefCache *rc){
    if (rc->hfide) return;
    static const char flit[] = "FIDE", frel[] = "FIDE registered player name";
    rc->fide_lit = r_text((const uint8_t *)flit, sizeof flit - 1);
    rc->fide_name_rel = r_text((const uint8_t *)frel, sizeof frel - 1);
    rc->hfide = 1;
}
static Ref r_position(RefCache *rc, const Bd *b){
    Ref ps[64]; uint32_t n = 0;
    for (int s = 0; s < 64; s++) if (b->sq[s]) {
        int pi = b->sq[s] + 6;
        if (!rc->has[pi][s]) {
            if (!chess_piece_square(b->sq[s], s, &rc->ps[pi][s])) {
                Ref two[2] = { atom(piece_cp(b->sq[s])), r_sq(rc, s) };
                rc->ps[pi][s] = compose(two, 2, ref_above(two, 2));
            }
            rc->has[pi][s] = 1;
        }
        ps[n++] = rc->ps[pi][s];
    }
    Ref part[4];
    part[0] = compose(ps, n, ref_above(ps, n));
    part[1] = atom(b->side ? 'b' : 'w');
    unsigned wm = castle_mask(b, 0), bm = castle_mask(b, 1);
    if (!chess_castling(wm, bm, &part[2])) { Ref cm[2] = { r_number(wm), r_number(bm) }; part[2] = compose(cm, 2, ref_above(cm, 2)); }
    int ep = ep_square(b);
    part[3] = ep >= 0 ? r_sq(rc, ep) : atom('-');
    Ref full[7]; uint32_t np = 4; memcpy(full, part, sizeof part);
    LpChessExtra x; lp_chess_extra(b, &x); Ref k[64];
    if (x.nhand) { for (int i = 0; i < x.nhand; i++) k[i] = atom(x.hand[i]); full[np++] = x.nhand == 1 ? k[0] : compose(k, (uint32_t)x.nhand, ref_above(k, (size_t)x.nhand)); }
    if (x.npromoted) { for (int i = 0; i < x.npromoted; i++) k[i] = r_sq(rc, x.promoted[i]); full[np++] = x.npromoted == 1 ? k[0] : compose(k, (uint32_t)x.npromoted, ref_above(k, (size_t)x.npromoted)); }
    if (x.nchecks) { for (int i = 0; i < x.nchecks; i++) k[i] = atom(x.checks[i]); full[np++] = compose(k, (uint32_t)x.nchecks, ref_above(k, (size_t)x.nchecks)); }
    for (uint32_t i = 0; i < np; i++) full[i].said = 0;
    Ref p = compose(full, np, ref_above(full, np)); p.said = 0; return p;
}
static Ref r_move(RefCache *rc, const Bd *b, const Mv *m){
    { Ref r; if (chess_move(b, m, &r)) { r.said = 0; return r; } }
    if (m->drop) { Ref d[3] = { atom(piece_cp((int8_t)(b->side ? -m->drop : m->drop))), atom('@'), r_sq(rc, m->to) }; Ref r = compose(d, 3, ref_above(d, 3)); r.said = 0; return r; }
    Ref k[4]; uint32_t n = 3; k[0] = atom(piece_cp(b->sq[m->from])); k[1] = r_sq(rc, m->from); k[2] = r_sq(rc, m->to);
    if (m->promo) { k[3] = atom(piece_cp((int8_t)(b->side ? -m->promo : m->promo))); n = 4; }
    Ref r = compose(k, n, ref_above(k, n)); r.said = 0; return r;
}

/* ---- compiled Tree-sitter PGN reader ---- */
enum { PG_TAG=1, PG_SAN, PG_NAG, PG_ANNOTATION, PG_VARIATION, PG_COMMENT, PG_LINE_COMMENT, PG_RESULT };
enum { K_PLAYED = 0, K_STUDY, K_PUZZLE };
typedef struct { int records; const Layout *layout; uint8_t *dispatch; uint32_t symbols; TSFieldId name, value; } ChessRecipe;
static __thread SayScratch chess_arena;
static void *chess_grow(const void *old, size_t bytes, size_t next){
    void *p = arena_alloc(&chess_arena, next); if (bytes) memcpy(p, old, bytes); return p;
}
typedef struct Line Line;
typedef struct { uint8_t kind; const char *t; uint32_t n; Line *var; } Ann;         /* kind: 1 comment, 2 NAG, 3 variation */
typedef struct { const char *san; uint32_t n; Ann *ann; int nann, cann; } Ply;
struct Line { Ply *p; int n, cap; Ann *lead; int nlead, clead; };
typedef struct { const char *name, *val; uint32_t nl, vl; } Tag;
typedef struct { Tag *tag; int ntag, ctag; Line main; const char *mt; size_t mtn; char result[8]; int has_result; int bad; } Game;

static void ann_push(Ann **a, int *n, int *c, Ann x){
    if (*n == *c) { int next = *c ? *c * 2 : 4; *a = chess_grow(*a, sizeof(Ann) * (size_t)*n, sizeof(Ann) * (size_t)next); *c = next; }
    (*a)[(*n)++] = x;
}
static uint8_t pgn_role(const ChessRecipe *recipe, TSNode node){
    TSSymbol symbol = ts_node_symbol(node); return symbol < recipe->symbols ? recipe->dispatch[symbol] : 0;
}
static void pgn_annotation(Line *line, Ann annotation){
    if (line->n) { Ply *ply = &line->p[line->n - 1]; ann_push(&ply->ann, &ply->nann, &ply->cann, annotation); }
    else ann_push(&line->lead, &line->nlead, &line->clead, annotation);
}
static void pgn_tag(const ChessRecipe *recipe, TSNode node, const char *source, Game *game){
    TSNode name = ts_node_child_by_field_id(node, recipe->name), value = ts_node_child_by_field_id(node, recipe->value);
    if (ts_node_is_null(name) || ts_node_is_null(value)) { game->bad = 1; return; }
    if (game->ntag == game->ctag) { int next = game->ctag ? game->ctag * 2 : 16; game->tag = chess_grow(game->tag, sizeof(Tag) * (size_t)game->ntag, sizeof(Tag) * (size_t)next); game->ctag = next; }
    Tag *tag = &game->tag[game->ntag++]; memset(tag, 0, sizeof *tag);
    tag->name = source + ts_node_start_byte(name); tag->nl = ts_node_end_byte(name) - ts_node_start_byte(name);
    uint32_t start = ts_node_start_byte(value) + 1, end = ts_node_end_byte(value) - 1;
    char *decoded = arena_alloc(&chess_arena, (size_t)(end - start) + 1); uint32_t count = 0;
    for (uint32_t j = start; j < end; j++) { if (source[j] == '\\' && j + 1 < end) j++; decoded[count++] = source[j]; }
    decoded[count] = 0; tag->val = decoded; tag->vl = count; game->mt = source + ts_node_end_byte(node);
}
static void pgn_line(const ChessRecipe *recipe, TSNode node, const char *source, Game *game, Line *line, unsigned depth){
    if (depth > 64) { game->bad = 1; return; }
    TSTreeCursor *cursor = lp_tls_cursor(node); int more = ts_tree_cursor_goto_first_child(cursor);
    while (more) {
        TSNode child = ts_tree_cursor_current_node(cursor); uint8_t role = pgn_role(recipe, child);
        const char *text = source + ts_node_start_byte(child); uint32_t length = ts_node_end_byte(child) - ts_node_start_byte(child);
        if (role == PG_TAG && !depth) pgn_tag(recipe, child, source, game);
        else if (role == PG_SAN) {
            uint32_t san = length; while (san && (text[san - 1] == '!' || text[san - 1] == '?')) san--;
            if (line->n == line->cap) { int next = line->cap ? line->cap * 2 : 128; line->p = chess_grow(line->p, sizeof(Ply) * (size_t)line->n, sizeof(Ply) * (size_t)next); line->cap = next; }
            line->p[line->n++] = (Ply){ .san = text, .n = san };
            if (san < length) pgn_annotation(line, (Ann){ 2, text + san, length - san, NULL });
        } else if (role == PG_COMMENT || role == PG_LINE_COMMENT) {
            uint32_t trim = role == PG_COMMENT ? 2 : 1;
            if (length >= trim) pgn_annotation(line, (Ann){ 1, text + 1, length - trim, NULL });
        } else if (role == PG_NAG || role == PG_ANNOTATION) pgn_annotation(line, (Ann){ 2, text, length, NULL });
        else if (role == PG_VARIATION) {
            Line *variation = arena_alloc(&chess_arena, sizeof *variation); memset(variation, 0, sizeof *variation);
            if (!line->n) game->bad = 1;
            pgn_annotation(line, (Ann){ 3, NULL, 0, variation }); pgn_line(recipe, child, source, game, variation, depth + 1);
        } else if (role == PG_RESULT && !depth) {
            if (length >= sizeof game->result) game->bad = 1;
            else { memcpy(game->result, text, length); game->result[length] = 0; game->has_result = 1; }
        }
        more = ts_tree_cursor_goto_next_sibling(cursor);
    }
    lp_tls_cursor_release(cursor);
}
static void pgn_game(const ChessRecipe *recipe, TSNode node, const char *source, Game *game){
    memset(game, 0, sizeof *game); game->mt = source + ts_node_start_byte(node);
    pgn_line(recipe, node, source, game, &game->main, 0);
    game->mtn = (size_t)(source + ts_node_end_byte(node) - game->mt);
}

/* ---- structured scalar parsers (clock deciseconds, dates, ratings, time controls) ---- */
static int parse_uint_span(const char *s, size_t n, unsigned *out){
    if (!n || n > 9) return 0;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10u + (unsigned)(s[i] - '0');
    }
    *out = v;
    return 1;
}
/* Parse [%clk H:MM:SS(.d)] or [%clk M:SS(.d)] into integer deciseconds, stripping [%clk ...] and [%emt ...]
 * machine tags and returning any remaining human comment prose in *rem / *rem_n. */
static int parse_clk_token(const char *c, size_t len, unsigned *ds_out){
    unsigned part[3] = { 0, 0, 0 }; int np = 0; size_t i = 0;
    while (i < len && c[i] == ' ') i++;
    while (i < len && np < 3) {
        size_t s = i; while (i < len && c[i] >= '0' && c[i] <= '9') i++;
        if (i == s || !parse_uint_span(c + s, i - s, &part[np++])) return 0;
        if (i < len && c[i] == ':') { i++; continue; }
        break;
    }
    if (np < 2) return 0;
    unsigned tenths = 0;
    if (i < len && c[i] == '.') {
        i++;
        if (i < len && c[i] >= '0' && c[i] <= '9') tenths = (unsigned)(c[i++] - '0');
        while (i < len && c[i] >= '0' && c[i] <= '9') i++;
    }
    while (i < len && c[i] == ' ') i++;
    if (i != len) return 0;
    unsigned secs = np == 3 ? (part[0] * 3600u + part[1] * 60u + part[2]) : (part[0] * 60u + part[1]);
    *ds_out = secs * 10u + tenths;
    return 1;
}
static int split_comment(const char *s, uint32_t n, int *has_clk, unsigned *clk_ds, const char **rem, uint32_t *rem_n){
    char *clean = arena_alloc(&chess_arena, (size_t)n + 1);
    uint32_t out = 0, i = 0;
    while (i < n) {
        if (i + 6 < n && (!memcmp(s + i, "[%clk ", 6) || !memcmp(s + i, "[%emt ", 6))) {
            int is_clk = s[i + 2] == 'c';
            uint32_t close = i + 6;
            while (close < n && s[close] != ']') close++;
            if (close < n) {
                unsigned ds = 0;
                if (is_clk && !*has_clk && parse_clk_token(s + i + 6, close - (i + 6), &ds)) {
                    *has_clk = 1;
                    *clk_ds = ds;
                }
                i = close + 1;
                continue;
            }
        }
        clean[out++] = s[i++];
    }
    uint32_t a = 0, b = out;
    while (a < b && (clean[a] == ' ' || clean[a] == '\t' || clean[a] == '\r' || clean[a] == '\n')) a++;
    while (b > a && (clean[b - 1] == ' ' || clean[b - 1] == '\t' || clean[b - 1] == '\r' || clean[b - 1] == '\n')) b--;
    *rem = clean + a;
    *rem_n = b - a;
    return *rem_n > 0;
}
static int parse_date_part(const char *s, size_t n, unsigned *out){
    int all_q = 1;
    for (size_t i = 0; i < n; i++) if (s[i] != '?') { all_q = 0; break; }
    if (all_q) { *out = 0; return 1; }
    return parse_uint_span(s, n, out);
}
static int encode_date(const char *s, uint32_t n, Ref *out){
    if (!n || (n == 1 && s[0] == '?')) { *out = atom('-'); return 0; }
    const char *d1 = memchr(s, '.', n);
    if (d1) {
        size_t l1 = (size_t)(d1 - s);
        const char *d2 = memchr(d1 + 1, '.', n - l1 - 1);
        if (d2) {
            size_t l2 = (size_t)(d2 - (d1 + 1)), l3 = n - l1 - l2 - 2;
            unsigned y = 0, m = 0, d = 0;
            if (l1 == 4 && l2 == 2 && l3 == 2 &&
                parse_date_part(s, l1, &y) && parse_date_part(d1 + 1, l2, &m) && parse_date_part(d2 + 1, l3, &d)) {
                if (!y && !m && !d) { *out = atom('-'); return 0; }
                Ref k[3] = { r_number(y), r_number(m), r_number(d) };
                *out = compose(k, 3, ref_above(k, 3));
                out->said = LP_SAID_TUPLE;
                return 1;
            }
        }
    }
    *out = r_text((const uint8_t *)s, n);
    return 1;
}
static int encode_tc(const char *s, uint32_t n, Ref *out){
    if (!n || (n == 1 && (s[0] == '?' || s[0] == '-'))) { *out = atom('-'); return 0; }
    unsigned base = 0, inc = 0;
    if (parse_uint_span(s, n, &base)) { *out = r_number(base); return 1; }
    const char *plus = memchr(s, '+', n);
    if (plus && parse_uint_span(s, (size_t)(plus - s), &base) &&
        parse_uint_span(plus + 1, n - (size_t)(plus - s) - 1, &inc)) {
        Ref k[2] = { r_number(base), r_number(inc) };
        *out = compose(k, 2, ref_above(k, 2));
        out->said = LP_SAID_TUPLE;
        return 1;
    }
    *out = r_text((const uint8_t *)s, n);
    return 1;
}
static int is_omitted_site(const char *s, uint32_t n){
    if (!n || (n == 1 && (s[0] == '?' || s[0] == '-'))) return 1;
    if ((n == 9 && !memcmp(s, "Chess.com", 9)) || (n == 7 && !memcmp(s, "Lichess", 7)) || (n == 11 && !memcmp(s, "lichess.org", 11))) return 1;
    if ((n >= 7 && !memcmp(s, "http://", 7)) || (n >= 8 && !memcmp(s, "https://", 8))) return 1;
    return 0;
}

/* ---- game & trajectory composition ---- */
typedef struct { Ref *c; size_t n, cap; } RV;
static void rv(RV *a, Ref x){
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 64; a->c = chess_grow(a->c, sizeof(Ref) * a->n, sizeof(Ref) * a->cap); }
    a->c[a->n++] = x;
}
static Ref comp(const Ref *k, size_t n){ Ref x = n == 1 ? k[0] : compose(k, (uint32_t)n, ref_above(k, n)); x.said = 0; return x; }
static Ref comp_tuple(const Ref *k, size_t n){ Ref x = compose(k, (uint32_t)n, ref_above(k, n)); x.said = LP_SAID_TUPLE; return x; }

typedef struct { RefCache *rc; RV nags, fide_claims; long errs; char err[160]; uint64_t plies, castles; } Rd;

static int replay(Rd *r, const Line *L, const Bd *b0, int want_clocks,
                  Ref *line, Ref *clocks, int *has_clocks, Ref *notes, int *has_notes, Ref *end){
    Bd b = *b0;
    RV traj = { 0 }, note_entries = { 0 };
    Ref P = r_position(r->rc, &b);
    rv(&traj, P);
    *has_clocks = 0;
    *has_notes = 0;

    if (L->nlead) {
        RV items = { 0 };
        rv(&items, r_number(0));
        for (int j = 0; j < L->nlead; j++) {
            const Ann *x = &L->lead[j];
            if (x->kind == 1) {
                int hc = 0; unsigned ds = 0; const char *rem = NULL; uint32_t rem_n = 0;
                if (split_comment(x->t, x->n, &hc, &ds, &rem, &rem_n)) rv(&items, r_text((const uint8_t *)rem, rem_n));
            } else if (x->kind == 2) {
                rv(&items, r_text((const uint8_t *)x->t, x->n));
            }
        }
        if (items.n > 1) rv(&note_entries, comp_tuple(items.c, items.n));
    }

    unsigned *clk_ds = want_clocks && L->n ? arena_alloc(&chess_arena, sizeof(unsigned) * (size_t)L->n) : NULL;
    uint8_t *clk_has = want_clocks && L->n ? arena_alloc(&chess_arena, (size_t)L->n) : NULL;
    if (clk_has) memset(clk_has, 0, (size_t)L->n);
    int any_clk = 0;

    for (int i = 0; i < L->n; i++) {
        const Ply *p = &L->p[i];
        Mv all[LP_CHESS_MOVES]; int na = gen(&b, all); int k = san_match(&b, p->san, p->n, all, na);
        if (k < 0) { snprintf(r->err, sizeof r->err, "%.*s is no legal move at ply %d", (int)p->n, p->san, i + 1); return 0; }
        Bd before = b; Ref Pb = P;
        Ref M = chess_capture ? r_move(r->rc, &b, &all[k]) : (Ref){ 0 };
        make(&b, &all[k]);
        P = r_position(r->rc, &b);
        rv(&traj, P);
        r->plies++;
        r->castles += all[k].castle;
        if (chess_capture) chess_capture(&before, &all[k], &b, &Pb, &M, &P);

        if (!p->nann) continue;
        RV items = { 0 };
        rv(&items, r_number((unsigned)(i + 1)));
        int hc = 0; unsigned ds = 0;
        for (int j = 0; j < p->nann; j++) {
            const Ann *x = &p->ann[j];
            if (x->kind == 1) {
                const char *rem = NULL; uint32_t rem_n = 0;
                if (split_comment(x->t, x->n, &hc, &ds, &rem, &rem_n)) rv(&items, r_text((const uint8_t *)rem, rem_n));
            } else if (x->kind == 2) {
                Ref nag = r_text((const uint8_t *)x->t, x->n);
                rv(&items, nag);
                rv(&r->nags, Pb); rv(&r->nags, P); rv(&r->nags, nag);
            } else if (x->var && x->var->n) {
                Ref vl, vc, vn; int hvc = 0, hvn = 0;
                if (!replay(r, x->var, &before, 0, &vl, &vc, &hvc, &vn, &hvn, NULL)) return 0;
                if (hvn) { Ref two[2] = { vl, vn }; rv(&items, comp_tuple(two, 2)); }
                else rv(&items, vl);
            }
        }
        if (hc && clk_has) { clk_has[i] = 1; clk_ds[i] = ds; any_clk = 1; }
        if (items.n > 1) rv(&note_entries, comp_tuple(items.c, items.n));
    }

    *line = comp(traj.c, traj.n);
    if (any_clk && L->n > 0) {
        Ref *cv = arena_alloc(&chess_arena, sizeof(Ref) * (size_t)L->n);
        for (int i = 0; i < L->n; i++) cv[i] = clk_has[i] ? r_number(clk_ds[i]) : atom('-');
        *clocks = L->n == 1 ? cv[0] : compose(cv, (uint32_t)L->n, ref_above(cv, (size_t)L->n));
        clocks->said = LP_SAID_TUPLE;
        *has_clocks = 1;
    }
    if (note_entries.n > 0) {
        *notes = note_entries.n == 1 ? note_entries.c[0] : comp_tuple(note_entries.c, note_entries.n);
        notes->said = LP_SAID_TUPLE;
        *has_notes = 1;
    }
    if (end) *end = P;
    return 1;
}

typedef struct {
    const char *variant, *fen, *result;
    size_t vl, fl, rl;
    const Rules *rules;
    Bd b0;
    Ref line, end;
    int ok;
} Start;

static int tag_eq(const Tag *t, const char *s){
    size_t l = strlen(s);
    return t->nl == l && !memcmp(t->name, s, l);
}
static int non_empty_val(const Tag *t){
    return t->vl > 0 && !(t->vl == 1 && t->val[0] == '?');
}
static void parse_fen_counters(const char *fen, size_t fl, unsigned *half_out, unsigned *full_out){
    *half_out = 0; *full_out = 1;
    const char *p = fen, *end = fen + fl;
    for (int f = 0; f < 4 && p < end; f++) {
        while (p < end && *p != ' ') p++;
        while (p < end && *p == ' ') p++;
    }
    if (p >= end) return;
    const char *s1 = p; while (p < end && *p != ' ') p++;
    unsigned hv = 0, fv = 1;
    if (parse_uint_span(s1, (size_t)(p - s1), &hv)) *half_out = hv;
    while (p < end && *p == ' ') p++;
    if (p < end) {
        const char *s2 = p; while (p < end && *p != ' ') p++;
        if (parse_uint_span(s2, (size_t)(p - s2), &fv) && fv >= 1) *full_out = fv;
    }
}

static Ref make_seat(Rd *rd, const Tag *name_tag, unsigned fide_id, unsigned elo){
    int has_name = name_tag && non_empty_val(name_tag);
    Ref nm = has_name ? r_text((const uint8_t *)name_tag->val, name_tag->vl) : atom('?');
    Ref el = elo > 0 ? r_number(elo) : atom('-');
    if (fide_id > 0) {
        ensure_fide_refs(rd->rc);
        Ref fk[2] = { rd->rc->fide_lit, r_number(fide_id) };
        Ref fn = compose(fk, 2, ref_above(fk, 2)); fn.said = 0;
        if (has_name) {
            int dup = 0;
            for (size_t i = 0; i + 2 < rd->fide_claims.n; i += 3)
                if (lp_id_eq(&rd->fide_claims.c[i].id, &fn.id) && lp_id_eq(&rd->fide_claims.c[i + 2].id, &nm.id)) { dup = 1; break; }
            if (!dup) { rv(&rd->fide_claims, fn); rv(&rd->fide_claims, rd->rc->fide_name_rel); rv(&rd->fide_claims, nm); }
        }
        Ref sk[3] = { fn, nm, el };
        return comp_tuple(sk, 3);
    }
    Ref sk[2] = { nm, el };
    return comp_tuple(sk, 2);
}

static int game_content(Rd *rd, const Game *g, Start *st, Ref *rec){
    memset(st, 0, sizeof *st);
    rd->nags.n = rd->fide_claims.n = 0;

    const Tag *t_white = NULL, *t_black = NULL, *t_date = NULL, *t_event = NULL,
              *t_site = NULL, *t_round = NULL, *t_eco = NULL, *t_tc = NULL;
    unsigned w_elo = 0, b_elo = 0, w_fide = 0, b_fide = 0;

    for (int i = 0; i < g->ntag; i++) {
        const Tag *t = &g->tag[i];
        if (tag_eq(t, "Variant")) { st->variant = t->val; st->vl = t->vl; }
        else if (tag_eq(t, "FEN")) { st->fen = t->val; st->fl = t->vl; }
        else if (tag_eq(t, "Result")) { st->result = t->val; st->rl = t->vl; }
        else if (tag_eq(t, "White")) t_white = t;
        else if (tag_eq(t, "Black")) t_black = t;
        else if (tag_eq(t, "WhiteElo")) (void)parse_uint_span(t->val, t->vl, &w_elo);
        else if (tag_eq(t, "BlackElo")) (void)parse_uint_span(t->val, t->vl, &b_elo);
        else if (tag_eq(t, "WhiteFideId")) (void)parse_uint_span(t->val, t->vl, &w_fide);
        else if (tag_eq(t, "BlackFideId")) (void)parse_uint_span(t->val, t->vl, &b_fide);
        else if (tag_eq(t, "Date") && !t_date) t_date = t;
        else if (tag_eq(t, "UTCDate") && !t_date) t_date = t;
        else if (tag_eq(t, "Event")) t_event = t;
        else if (tag_eq(t, "Site")) t_site = t;
        else if (tag_eq(t, "Round")) t_round = t;
        else if (tag_eq(t, "ECO")) t_eco = t;
        else if (tag_eq(t, "TimeControl")) t_tc = t;
    }

    const char *res = st->result; size_t rl = st->rl;
    if (!res && g->has_result) { res = g->result; rl = strlen(g->result); st->result = res; st->rl = rl; }
    uint32_t res_cp = '*';
    if (res) {
        if (rl == 3 && !memcmp(res, "1-0", 3)) res_cp = '1';
        else if (rl == 3 && !memcmp(res, "0-1", 3)) res_cp = '0';
        else if (rl == 7 && !memcmp(res, "1/2-1/2", 7)) res_cp = '=';
    }

    Ref dash = atom('-');
    Ref slots[10];
    int has_meta = 0;

    int has_players = (t_white && non_empty_val(t_white)) || (t_black && non_empty_val(t_black)) || w_fide > 0 || b_fide > 0;
    if (has_players) {
        Ref s2[2] = { make_seat(rd, t_white, w_fide, w_elo), make_seat(rd, t_black, b_fide, b_elo) };
        slots[0] = comp_tuple(s2, 2);
        has_meta = 1;
    } else slots[0] = dash;

    slots[1] = atom(res_cp);
    if (res_cp != '*') has_meta = 1;

    if (t_date && encode_date(t_date->val, t_date->vl, &slots[2])) has_meta = 1; else slots[2] = dash;
    if (t_event && non_empty_val(t_event)) { slots[3] = r_text((const uint8_t *)t_event->val, t_event->vl); has_meta = 1; } else slots[3] = dash;
    if (t_site && !is_omitted_site(t_site->val, t_site->vl)) { slots[4] = r_text((const uint8_t *)t_site->val, t_site->vl); has_meta = 1; } else slots[4] = dash;
    if (t_round && non_empty_val(t_round) && !(t_round->vl == 1 && t_round->val[0] == '-')) {
        unsigned rnum = 0;
        slots[5] = parse_uint_span(t_round->val, t_round->vl, &rnum) ? r_number(rnum) : r_text((const uint8_t *)t_round->val, t_round->vl);
        has_meta = 1;
    } else slots[5] = dash;
    if (t_eco && non_empty_val(t_eco) && !(t_eco->vl == 1 && t_eco->val[0] == '-')) {
        slots[6] = r_text((const uint8_t *)t_eco->val, t_eco->vl);
        has_meta = 1;
    } else slots[6] = dash;
    if (t_tc && encode_tc(t_tc->val, t_tc->vl, &slots[7])) has_meta = 1; else slots[7] = dash;

    if (st->variant && st->vl && !(st->vl == 1 && st->variant[0] == '?') &&
        !(st->vl == 8 && (!memcmp(st->variant, "Standard", 8) || !memcmp(st->variant, "standard", 8))) &&
        !(st->vl == 5 && !memcmp(st->variant, "chess", 5))) {
        slots[8] = r_text((const uint8_t *)st->variant, st->vl);
        has_meta = 1;
    } else slots[8] = dash;

    slots[9] = dash;
    if (st->fen && st->fl) {
        unsigned half = 0, full = 1;
        parse_fen_counters(st->fen, st->fl, &half, &full);
        if (half != 0 || full != 1) {
            Ref ck[2] = { r_number(half), r_number(full) };
            slots[9] = comp_tuple(ck, 2);
            has_meta = 1;
        }
    }

    st->rules = rules_named(st->variant, st->vl);
    st->ok = st->rules != NULL;
    if (st->ok) st->ok = st->fen ? board_law_fen(&st->b0, st->fen, st->fl, st->rules) : board_law_start(&st->b0, st->rules);

    Ref clocks = dash, notes = dash;
    int has_clocks = 0, has_notes = 0;
    if (st->ok && !g->bad) {
        st->ok = replay(rd, &g->main, &st->b0, 1, &st->line, &clocks, &has_clocks, &notes, &has_notes, &st->end);
    } else if (st->ok) {
        st->ok = 0; snprintf(rd->err, sizeof rd->err, "its variations are not closed");
    } else if (!st->rules) {
        snprintf(rd->err, sizeof rd->err, "its variant %.*s is in no rules (firmware/chess.firmware)", (int)st->vl, st->variant);
    } else {
        snprintf(rd->err, sizeof rd->err, "its FEN is no position");
    }

    RV part = { 0 };
    if (st->ok) {
        rv(&part, st->line);
        if (has_meta || has_clocks || has_notes) {
            rv(&part, comp_tuple(slots, 10));
            if (has_clocks || has_notes) rv(&part, has_clocks ? clocks : dash);
            if (has_notes) rv(&part, notes);
        }
    } else {
        if (st->fen && st->fl) { slots[9] = r_text((const uint8_t *)st->fen, st->fl); has_meta = 1; }
        if (has_meta) rv(&part, comp_tuple(slots, 10));
        if (g->mtn) rv(&part, r_text((const uint8_t *)g->mt, g->mtn));
    }
    if (!part.n) return 0;
    *rec = comp(part.c, part.n);
    return 1;
}

/* ---- the recipe: grammar pgn ---- */
static int chess_says(Recipe *r, const char *path, char *tok){
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(ChessRecipe));
    ChessRecipe *c = r->native_recipe;
    if (!strcmp(tok, "records")) {
        char *v = strtok(NULL, " \t\r\n");
        if (v && !strcmp(v, "played")) c->records = K_PLAYED;
        else if (v && !strcmp(v, "study")) c->records = K_STUDY;
        else if (v && !strcmp(v, "puzzle")) c->records = K_PUZZLE;
        else { fprintf(stderr, "%s: records played | study | puzzle\n", path); return -1; }
        return 1;
    }
    fprintf(stderr, "%s: \"%s\" is not something a PGN recipe says (records)\n", path, tok); return -1;
}
static int chess_recipe(Recipe *r, const char *path){
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(ChessRecipe));
    ChessRecipe *recipe = r->native_recipe; recipe->layout = layout_format("pgn");
    if (!recipe->layout) { fprintf(stderr, "%s: PGN packaging format is unavailable\n", path); return 0; }
    const TSLanguage *language = grammar_lang(recipe->layout->g.grammar_id); r->lang = language;
    recipe->symbols = ts_language_symbol_count(language); recipe->dispatch = calloc(recipe->symbols, 1);
    const char *names[] = {"tag_pair","san","nag","annotation","variation","comment","line_comment","result"};
    for (uint8_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        TSSymbol symbol = ts_language_symbol_for_name(language, names[i], (uint32_t)strlen(names[i]), true);
        if (!symbol) { fprintf(stderr, "%s: unresolved PGN symbol %s\n", path, names[i]); return 0; }
        recipe->dispatch[symbol] = (uint8_t)(i + 1);
    }
    recipe->name = ts_language_field_id_for_name(language, "name", 4);
    recipe->value = ts_language_field_id_for_name(language, "value", 5);
    if (!recipe->name || !recipe->value) return 0;
    r->curated = 1; return 1;
}

#define BLOCK 4096
static Ref blocks(Ref *r, size_t n){
    if (n == 1) return r[0];
    if (n <= BLOCK) return compose(r, (uint32_t)n, ref_above(r, n));
    Ref *up = malloc(sizeof(Ref) * n); size_t m = 0, from = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t b; memcpy(&b, r[i].id.b + 8, 8);
        if ((b & (BLOCK - 1)) == 0 || i == n - 1) {
            Ref x = i - from + 1 == 1 ? r[from] : compose(r + from, (uint32_t)(i - from + 1), ref_above(r + from, i - from + 1));
            if (x.said != LP_SAID_TUPLE && x.said != LP_SAID_CLAIM) x.said = 0;
            up[m++] = x; from = i + 1;
        }
    }
    Ref out = m == n ? compose(r, (uint32_t)n, ref_above(r, n)) : blocks(up, m); free(up); return out;
}
static void member(File *f, Ref rec, Ref *part, int n, uint32_t *pos){
    for (int i = 0; i < n; i++) if (part[i].said != LP_SAID_TUPLE) part[i].said = 0;
    Ref c = n == 1 ? part[0] : compose(part, (uint32_t)n, ref_above(part, (size_t)n)); c = said_claim(c);
    Event e = { .claim = c.id, .witnessed = rec.id, .score = 1.0f, .enter_rating = 1500.0f, .position = ++*pos, .kind = EV_CLAIM, .run = 1 };
    ev_push(&f->ev, &e);
}

static void chess_records(const Recipe *rcp, File *f, const uint8_t *src, SRecords *records){
    size_t mark = chess_arena.used;
    const ChessRecipe *cr = rcp->native_recipe; RefCache *rc = calloc(1, sizeof(RefCache)); Rd rd = { .rc = rc };
    RV recs = { 0 }; uint64_t games = 0, played = 0, study = 0, puzzle = 0, unread = 0, unfinished = 0, c960 = 0, zero = 0, nomarker = 0, mismatch = 0;
    Game g; uint32_t ordinal = (uint32_t)f->records; SRecord record;
    while (s_record_next(records, &record)) {
        pgn_game(cr, record.node, (const char *)src, &g);
        games++; ordinal++;
        Start st; Ref rec; int held_any = game_content(&rd, &g, &st, &rec);
        const char *result = st.result; size_t rl = st.rl; int ok = st.ok; Ref line = st.line;
        int result_mismatch = g.has_result && result && (rl != strlen(g.result) || memcmp(result, g.result, rl));
        if (!g.has_result) nomarker++; else if (result_mismatch) mismatch++;
        if (!result && g.has_result) { result = g.result; rl = strlen(g.result); }
        /* Annotated played games retain their played status and outcome; NAGs/notes/variations are also preserved. */
        int kind = cr->records;
        if (st.variant && st.rules && st.rules->c960) c960++;
        if (!ok) { unread++; if (unread <= 5) fprintf(stderr, "\n  %s: game %u is kept as its text and not replayed: %s", f->path, ordinal, rd.err); }
        if (!held_any) continue;
        uint64_t event_begin = f->ev.n;
        Event e = { .claim = rec.id, .witnessed = rec.id, .score = 1.0f, .enter_rating = 1500.0f, .position = ordinal, .kind = EV_RECORD };
        ev_push(&f->ev, &e);
        uint32_t pos = 0;
        int outcome = result && !result_mismatch &&
            ((rl == 3 && (!memcmp(result, "1-0", 3) || !memcmp(result, "0-1", 3))) || (rl == 7 && !memcmp(result, "1/2-1/2", 7)));
        if (ok && (!g.main.n)) zero++;
        if (!outcome) unfinished++;
        for (size_t i = 0; i + 2 < rd.fide_claims.n; i += 3) member(f, rec, &rd.fide_claims.c[i], 3, &pos);
        if (ok && kind == K_PLAYED) played++;
        /* Game-altitude outcome: attest the pure board-state trajectory [P0..Pn] once with the game's result
         * (1-0 -> 1.0, 1/2-1/2 -> 0.5, 0-1 -> 0.0), never synthetic per-move [P_k, P_{k+1}] win/loss claims. */
        if (ok && kind == K_PLAYED && outcome && g.main.n > 0) {
            float score = rl == 7 ? 0.5f : (result[0] == '1' ? 1.0f : 0.0f);
            Ref p1[1] = { line };
            member(f, rec, p1, 1, &pos);
            f->ev.e[f->ev.n - 1].score = score;
        }
        if (ok && kind == K_PUZZLE) { Ref p1[1] = { line }; member(f, rec, p1, 1, &pos); puzzle++; }
        if (ok && kind == K_STUDY) study++;
        if (ok) for (size_t i = 0; i + 2 < rd.nags.n; i += 3) member(f, rec, &rd.nags.c[i], 3, &pos);

        RV provenance = { 0 };
        uint64_t nclaims = f->ev.n - (event_begin + 1);
        if (nclaims == 1 && lp_id_eq(&f->ev.e[event_begin + 1].claim, &rec.id)) {
            Ref claim;
            if (!record_claim_vertex(&f->ev.e[event_begin + 1], &claim)) { fputs("chess claim has no product\n", stderr); exit(2); }
            claim.said = LP_SAID_RECORD;
            f->ev.e[event_begin].claim = f->ev.e[event_begin].witnessed = claim.id;
            rv(&recs, claim);
        } else {
            rv(&provenance, rec);
            for (uint64_t i = event_begin; i < f->ev.n; i++) if (f->ev.e[i].kind == EV_CLAIM) {
                Ref claim;
                if (!record_claim_vertex(&f->ev.e[i], &claim)) { fputs("chess claim has no product\n", stderr); exit(2); }
                rv(&provenance, claim);
            }
            Ref held = comp(provenance.c, provenance.n);
            held.said = LP_SAID_RECORD;
            f->ev.e[event_begin].claim = f->ev.e[event_begin].witnessed = held.id;
            rv(&recs, held);
        }
    }
    f->records += games; f->incomplete += unread;
    if (f->emit) {
        if (f->emit(f->emit_context, recs.c, recs.n, NULL, 0, &f->ev)) { fputs("PGN output window failed\n", stderr); exit(2); }
        f->ev.n = 0;
    } else {
        if (recs.n) { Ref content = blocks(recs.c, recs.n); content.said = 0; f->trunk = content; }
        f->laid = 0;
    }
    fprintf(stderr, "\n  %s: window of %llu games: %llu played, %llu studies, %llu puzzles, %llu kept as text; %llu without an outcome (*), %llu of no moves, %llu Chess960; %llu plies, %llu castlings; %llu without a termination marker, %llu whose marker is not their Result\n",
            f->path, (unsigned long long)games, (unsigned long long)played, (unsigned long long)study, (unsigned long long)puzzle, (unsigned long long)unread, (unsigned long long)unfinished,
            (unsigned long long)zero, (unsigned long long)c960, (unsigned long long)rd.plies, (unsigned long long)rd.castles, (unsigned long long)nomarker, (unsigned long long)mismatch);
    free(rc); chess_arena.used = mark;
}

static void chess_read(const Recipe *recipe, File *file, const uint8_t *source, size_t length){
    const ChessRecipe *compiled = recipe->native_recipe; SRecords records;
    if (!s_records_open(&records, compiled->layout, source, length)) { fprintf(stderr, "%s: invalid PGN packaging\n", file->path); exit(2); }
    chess_records(recipe, file, source, &records); s_records_close(&records);
}
static void chess_input(const Recipe *recipe, File *file, const SInput *input){
    const ChessRecipe *compiled = recipe->native_recipe; lp_buf buffer = { 0 }; SayScratch headers = { 0 };
    size_t quantum = (size_t)8 << 20; const char *configured = getenv("LAPLACE_RECORD_WINDOW_BYTES");
    if (configured && *configured) {
        char *end; unsigned long long value = strtoull(configured, &end, 10);
        if (*end || !value || value > UINT32_MAX) { fputs("invalid PGN record window\n", stderr); exit(2); }
        quantum = (size_t)value;
    }
    int ended = 0; uint64_t offset = 0;
    table_pin(&file->witness.id); if (file->has_lineage) table_pin(&file->lineage.id);
    while (!ended || buffer.n) {
        SRecords records; size_t consumed = 0; int status = 0;
        while (!status) {
            if (!ended) {
                if (buffer.n > UINT32_MAX - quantum) { fputs("PGN game exceeds parser byte range\n", stderr); exit(2); }
                lp_reserve((void **)&buffer.b, &buffer.cap, buffer.n + quantum, 1);
                size_t got = input->read(input->context, buffer.b + buffer.n, quantum); buffer.n += got;
                if (!got) { ended = 1; if (input->error && input->error(input->context)) { fputs("PGN input read failed\n", stderr); exit(2); } }
            }
            if (!buffer.n && ended) break;
            status = s_records_window(&records, compiled->layout, buffer.b, buffer.n, ended, &consumed, &headers);
            if (status < 0) { fprintf(stderr, "%s: invalid PGN packaging at byte %llu\n", file->path, (unsigned long long)offset); exit(2); }
        }
        if (!status) break;
        chess_records(recipe, file, buffer.b, &records); s_records_close(&records);
        if (!consumed) { fputs("PGN framing made no progress\n", stderr); exit(2); }
        memmove(buffer.b, buffer.b + consumed, buffer.n - consumed); buffer.n -= consumed; offset += consumed; file->at = offset;
    }
    lp_buf_free(&buffer); arena_release(&headers); arena_release(&chess_arena);
    if (file->emit) file_stream_finish(file);
}

/* ---- bare movetext field (part NAME grammar pgn, e.g. recipes/openings) ---- */
static ChessRecipe movetext; static int movetext_state;
static pthread_mutex_t movetext_mu = PTHREAD_MUTEX_INITIALIZER;
static int movetext_ready(void){
    pthread_mutex_lock(&movetext_mu);
    if (!movetext_state) {
        movetext_state = -1; const Layout *layout = layout_format("pgn_moves");
        const TSLanguage *language = layout ? grammar_lang(layout->g.grammar_id) : NULL;
        if (language) {
            movetext.layout = layout; movetext.symbols = ts_language_symbol_count(language); movetext.dispatch = calloc(movetext.symbols, 1);
            const char *names[] = {"tag_pair","san","nag","annotation","variation","comment","line_comment","result"};
            int san = 0;
            for (uint8_t i = 0; i < sizeof names / sizeof names[0]; i++) {
                TSSymbol symbol = ts_language_symbol_for_name(language, names[i], (uint32_t)strlen(names[i]), true);
                if (symbol && symbol < movetext.symbols) { movetext.dispatch[symbol] = (uint8_t)(i + 1); san |= i == 1; }
            }
            if (san) movetext_state = 1;
        }
        if (movetext_state < 0) fputs("chess: the movetext grammar (formats/pgn_moves.format) is unavailable\n", stderr);
    }
    int ready = movetext_state > 0; pthread_mutex_unlock(&movetext_mu); return ready;
}
static const char *const chess_products[] = { "position", NULL };
static int chess_field(const uint8_t *text, size_t n, unsigned product, Ref *out){
    if (!text) return movetext_ready();
    if (__atomic_load_n(&movetext_state, __ATOMIC_ACQUIRE) <= 0 || n > UINT32_MAX || product > 1) return 0;
    TSParser *parser = lp_tls_parser(movetext.layout->g.grammar_id); if (!parser) return 0;
    TSTree *tree = ts_parser_parse_string(parser, NULL, (const char *)text, (uint32_t)n); if (!tree) return 0;
    TSNode root = ts_tree_root_node(tree); static int told; int held = 0;
    if (ts_node_has_error(root)) {
        if (!product && __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 5)
            fprintf(stderr, "\n  chess: \"%.*s\" is no movetext\n", (int)(n < 80 ? n : 80), (const char *)text);
    } else {
        size_t mark = chess_arena.used; Game g; pgn_game(&movetext, root, (const char *)text, &g);
        RefCache *rc = arena_alloc(&chess_arena, sizeof *rc); memset(rc, 0, sizeof *rc); Rd rd = { .rc = rc }; Start st;
        Ref content; held = game_content(&rd, &g, &st, &content);
        if (!product) { if (held) *out = content; } else { held = held && st.ok; if (held) *out = st.end; }
        if (!product && held && !st.ok && __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 5)
            fprintf(stderr, "\n  chess: \"%.*s\" is kept as its text and not replayed: %s\n", (int)(n < 80 ? n : 80), (const char *)text, rd.err);
        chess_arena.used = mark;
    }
    ts_tree_delete(tree); return held;
}

const NativeGrammar chess_pgn = { "pgn", chess_says, chess_recipe, chess_read, chess_input, chess_field, chess_products };

/* ---- FEN field (part NAME grammar fen) ---- */
static __thread RefCache fen_cache;
static int fen_field(const uint8_t *text, size_t n, unsigned product, Ref *out){
    if (!text) return 1;
    Bd b; if (product || !board_fen(&b, (const char *)text, n, 0) || king_sq(&b, 0) < 0 || king_sq(&b, 1) < 0) return 0;
    memset(fen_cache.has, 0, sizeof fen_cache.has); memset(fen_cache.hsq, 0, sizeof fen_cache.hsq);
    *out = r_position(&fen_cache, &b); return 1;
}
static int fen_says(Recipe *r, const char *path, char *tok){ (void)r; fprintf(stderr, "%s: \"%s\": grammar fen reads a field (part NAME grammar fen), not a file\n", path, tok); return -1; }
static int fen_done(Recipe *r, const char *path){ (void)r; fprintf(stderr, "%s: grammar fen reads a field, not a file\n", path); return 0; }
const NativeGrammar chess_fen = { "fen", fen_says, fen_done, NULL, NULL, fen_field, NULL };
