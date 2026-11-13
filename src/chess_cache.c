/* The chess perf-cache (Sequence 7, Perfcaches; Repositories/Laplace/docs/guides/compositional-perfcache.md): the
 * lattice piece and square vocabulary -> positions -> transitions, one Native perf-cache module (laplace/perfcache.h)
 * that laplace chess writes, the ingest and every backend map (the extension's laplace.chess setting).
 *
 * Dense, complete, by direct address (record = section[address], O(1), no hash). Each record is the content chess.c
 * composes (src/chess.c gives the layout) and its ID is chess_rules.c's:
 *   square        64     s = file + 8 rank                      [file, rank]
 *   piece_square  768    64 piece + s, piece P N B R Q K p n b r q k   [piece, square]
 *   side          2      0 w, 1 b                               a codepoint
 *   number        256    the value                              its decimal digits (a castling mask)
 *   castling      7225   85 white + black, each the place (ascending) of its mask among the 85 a side can hold: none, a
 *                        king and one castling rook (two bits), or a king between two (three bits): what the rules
 *                        castle with, Standard and Chess960      [White's mask, Black's mask]
 *   en_passant    17     0 -, then a3..h3, a6..h6               a square or -
 *   move          every [piece, from, to] or [piece, from, to, promoted] a piece of either colour can be moved by on
 *                 some board: its steps, slides and jumps, a pawn's push, double push, capture and promotion from
 *                 ranks 2 to 7 of its side, and a king onto any square of its own back rank (castling is the king
 *                 onto its own rook). Its address is the enumeration (piece, from, to, promotion) in that order.
 * Sparse, admitted: the positions, their boards and the [P, M, P'] transitions of every move the sources named play,
 * as chess.c composes them when it reads them, sorted by ID (lookup O(1) expected through the module's ID index), and
 * the deterministic (P, M) -> P' as a pair table (O(log n) binary search on the full key). A miss is no absence: the
 * caller composes canonically.
 *
 * Use. The cache supplies identity and coordinate; it is not proof that a database holds a record. The ingest asks
 * its database once, set-wise, which records it holds (chess_cache_database); a held record is used from the ROM and
 * never composed into the node table, staged or asked about again, and the top-down prune takes a held sparse
 * record as recorded with its closure. A record the database does not hold yet is composed and staged as before,
 * on its first admission. */
#include "chess_internal.h"
#include "chess_cache.h"
#include "laplace/tier0.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- the vocabulary's law: its domains and their addresses */
/* A piece's index: P N B R Q K p n b r q k. */
static int8_t piece_at(int i){ return (int8_t)(i < 6 ? i + 1 : -(i - 5)); }
static int piece_index(int8_t p){ return p > 0 ? p - 1 : 5 - p; }
#define NMASKS 85
static int mask_place[256]; static unsigned mask_value[NMASKS];
static int popcount8(unsigned m){ int c = 0; for (; m; m &= m - 1) c++; return c; }
/* A promotion as its index: 0 none, 1..4 a knight, bishop, rook or queen. */
static int promo_index(uint8_t promo){ return promo >= KNIGHT && promo <= QUEEN ? promo - 1 : 0; }
static int move_shape(int pi, int from, int to, int pr){
    int8_t p = piece_at(pi); int a = abs(p), white = p > 0;
    if (from == to) return 0;
    int df = (to & 7) - (from & 7), dr = (to >> 3) - (from >> 3), adf = abs(df), adr = abs(dr);
    if (a != PAWN && pr) return 0;
    switch (a) {
        case KNIGHT: return (adf == 1 && adr == 2) || (adf == 2 && adr == 1);
        case BISHOP: return adf == adr;
        case ROOK:   return !df || !dr;
        case QUEEN:  return adf == adr || !df || !dr;
        case KING:   return (adf <= 1 && adr <= 1) || (!dr && (from >> 3) == (white ? 0 : 7));
        default: {
            int fr = from >> 3, fwd = white ? 1 : -1, last = white ? 7 : 0;
            if (fr == 0 || fr == 7) return 0;
            int ok = (!df && dr == fwd) || (!df && dr == 2 * fwd && fr == (white ? 1 : 6)) || (adf == 1 && dr == fwd);
            if (!ok) return 0;
            return (to >> 3) == last ? pr != 0 : pr == 0;
        }
    }
}
static uint16_t move_slot[12][64][64][5];         /* slot + 1; 0: no such move */
static uint32_t NMOVES;
typedef struct { int8_t piece; uint8_t from, to, promo; } MoveShape;
static MoveShape *MOVES;
/* Once, for every thread (pthread_once is not on every platform os.h serves): the flag read without the lock once set. */
#define ONCE(flag, mu, fn) do { if (!__atomic_load_n(&(flag), __ATOMIC_ACQUIRE)) { pthread_mutex_lock(&(mu)); if (!(flag)) { fn(); __atomic_store_n(&(flag), 1, __ATOMIC_RELEASE); } pthread_mutex_unlock(&(mu)); } } while (0)
static int law_done; static pthread_mutex_t law_mu = PTHREAD_MUTEX_INITIALIZER;
static void law_build(void){
    int n = 0; for (unsigned m = 0; m < 256; m++) { int c = popcount8(m); mask_place[m] = -1; if (c == 0 || c == 2 || c == 3) { mask_value[n] = m; mask_place[m] = n++; } }
    if (n != NMASKS) { fputs("chess cache: the castling domain is not 85 masks\n", stderr); exit(2); }
    uint32_t k = 0; MOVES = malloc(sizeof *MOVES * 12 * 64 * 64); if (!MOVES) { perror("chess cache"); exit(1); }
    for (int pi = 0; pi < 12; pi++) for (int f = 0; f < 64; f++) for (int t = 0; t < 64; t++) for (int pr = 0; pr < 5; pr++)
        if (move_shape(pi, f, t, pr)) { move_slot[pi][f][t][pr] = (uint16_t)(k + 1); MOVES[k++] = (MoveShape){ piece_at(pi), (uint8_t)f, (uint8_t)t, (uint8_t)(pr ? pr + 1 : 0) }; }
    NMOVES = k;
}
static void law(void){ ONCE(law_done, law_mu, law_build); }

enum { S_SQUARE, S_PIECE_SQUARE, S_SIDE, S_NUMBER, S_CASTLING, S_EN_PASSANT, S_MOVE, S_BOARD, S_POSITION, S_TRANSITION, S_NEXT, NSECTIONS };
static const char *const SECTION[NSECTIONS] = { "square", "piece_square", "side", "number", "castling", "en_passant", "move", "board", "position", "transition", "next" };
static uint32_t dense_count(int s){
    switch (s) { case S_SQUARE: return 64; case S_PIECE_SQUARE: return 768; case S_SIDE: return 2; case S_NUMBER: return 256;
                 case S_CASTLING: return NMASKS * NMASKS; case S_EN_PASSANT: return 17; case S_MOVE: return NMOVES; default: return 0; }
}
static int ep_slot(int ep){ if (ep < 0) return 0; int r = ep >> 3; return r == 2 ? 1 + (ep & 7) : r == 5 ? 9 + (ep & 7) : -1; }

/* ---- the vocabulary as content: each composed with its coordinate (Native's lp_ref_*) and its ID checked against
 * chess_rules.c's */
static Ref cp(uint32_t c){ return lp_ref_atom(T0, c); }
static Ref comp(const Ref *k, size_t n){ Ref r = n == 1 ? k[0] : lp_ref_compose(k, n, lp_tier_above(k, n)); r.said = 0; r.outcome = 0; r.position = 0; r.spare = 0; return r; }
static Ref square_ref(int s){ Ref k[2] = { cp((uint32_t)('a' + (s & 7))), cp((uint32_t)('1' + (s >> 3))) }; return comp(k, 2); }
static Ref number_ref(unsigned v){ char d[8]; int l = snprintf(d, sizeof d, "%u", v); Ref k[8]; for (int i = 0; i < l; i++) k[i] = cp((uint32_t)d[i]); return comp(k, (size_t)l); }
static Ref piece_square_ref(int8_t p, int s){ Ref k[2] = { cp(piece_cp(p)), square_ref(s) }; return comp(k, 2); }
static Ref castling_ref(unsigned w, unsigned b){ Ref k[2] = { number_ref(w), number_ref(b) }; return comp(k, 2); }
static Ref en_passant_ref(int ep){ return ep >= 0 ? square_ref(ep) : cp('-'); }
static Ref move_ref(int8_t p, int from, int to, uint8_t promo){
    Ref k[4] = { cp(piece_cp(p)), square_ref(from), square_ref(to) }; size_t n = 3;
    if (promo) { k[3] = cp(piece_cp((int8_t)(p > 0 ? promo : -(int)promo))); n = 4; }
    return comp(k, n);
}
static Ref board_ref(const Bd *b){ Ref ps[64]; size_t n = 0; for (int s = 0; s < 64; s++) if (b->sq[s]) ps[n++] = piece_square_ref(b->sq[s], s); return comp(ps, n); }
static Ref position_ref(const Bd *b, Ref *board){
    Ref part[4] = { board_ref(b), cp(b->side ? 'b' : 'w'), castling_ref(castle_mask(b, 0), castle_mask(b, 1)), en_passant_ref(ep_square(b)) };
    if (board) *board = part[0];
    return comp(part, 4);
}
static Bd lone(int8_t p, int s){ Bd b; memset(&b, 0, sizeof b); b.ep = -1; b.sq[s] = p; b.side = p > 0 ? 0 : 1; return b; }

static lp_tier0_record record_of(Ref r, uint32_t slot){
    lp_tier0_record x; memset(&x, 0, sizeof x);
    x.id = r.id; memcpy(x.m, r.c.m, sizeof x.m); x.hilbert = lp_hilbert4(&r.c); x.rank = slot; x.pad = r.tier; return x;
}
static Ref ref_of(const lp_tier0_record *x){ Ref r; memset(&r, 0, sizeof r); r.id = x->id; memcpy(r.c.m, x->m, sizeof r.c.m); r.tier = (uint8_t)x->pad; return r; }
static int same(const Ref *a, const Ref *b){ return lp_id_eq(&a->id, &b->id) && !memcmp(a->c.m, b->c.m, sizeof a->c.m) && a->tier == b->tier; }
static int record_is(const lp_tier0_record *x, const Ref *r){ Ref y = ref_of(x); return same(&y, r) && x->hilbert == lp_hilbert4(&r->c); }

/* The dense record at (section, slot), composed, and whether its ID is chess_rules.c's. */
static Ref dense_ref(int s, uint32_t slot, int *rules){
    lp_id want; Ref r; *rules = 1;
    switch (s) {
        case S_SQUARE: r = square_ref((int)slot); lp_chess_id_square((int)slot, &want); break;
        case S_PIECE_SQUARE: { int8_t p = piece_at((int)(slot / 64)); r = piece_square_ref(p, (int)(slot % 64)); lp_chess_id_piece_square(p, (int)(slot % 64), &want); break; }
        case S_SIDE: r = cp(slot ? 'b' : 'w'); lp_id_codepoint(slot ? 'b' : 'w', &want); break;
        case S_NUMBER: r = number_ref(slot); lp_chess_id_number(slot, &want); break;
        case S_CASTLING: { unsigned w = mask_value[slot / NMASKS], b = mask_value[slot % NMASKS]; r = castling_ref(w, b); lp_chess_id_castling(w, b, &want); break; }
        case S_EN_PASSANT: { int ep = !slot ? -1 : slot <= 8 ? 16 + (int)slot - 1 : 40 + (int)slot - 9; r = en_passant_ref(ep); lp_chess_id_en_passant(ep, &want); break; }
        default: { const MoveShape *m = &MOVES[slot]; r = move_ref(m->piece, m->from, m->to, m->promo);
                   Bd b = lone(m->piece, m->from); Mv mv = { m->from, m->to, m->promo, 0, 0 }; lp_chess_id_move(&b, &mv, &want); break; }
    }
    *rules = lp_id_eq(&want, &r.id); return r;
}

/* ---- the mapped module */
static const lp_perfcache *PC; static const lp_section *SEC[NSECTIONS];
static int mapped_done; static pthread_mutex_t map_mu = PTHREAD_MUTEX_INITIALIZER;
const char *chess_cache_path(void){ static char path[4096]; return lp_perfcache_beside_tier0("LAPLACE_CHESS", ".chess", path, sizeof path); }
/* The module's sections as the law declares them, or the reason it is not this law's module. */
static const char *conforms(const lp_perfcache *pc, const lp_section **sec){
    law();
    if (strcmp(pc->module, "chess")) return "it is not a chess module";
    for (int s = 0; s < NSECTIONS; s++) {
        sec[s] = lp_perfcache_section(pc, SECTION[s]); if (!sec[s]) return "a section is missing";
        uint32_t kind = s <= S_MOVE ? LP_PERFCACHE_DENSE : s == S_NEXT ? LP_PERFCACHE_PAIRS : LP_PERFCACHE_SPARSE;
        if (sec[s]->kind != kind || (s <= S_MOVE && sec[s]->count != dense_count(s))) return "a section is not the law's (another vocabulary)";
    }
    return NULL;
}
static void map_once_run(void){
    const char *path = chess_cache_path(); struct stat st;
    if (stat(path, &st)) { if (getenv("LAPLACE_CHESS") && *getenv("LAPLACE_CHESS")) { fprintf(stderr, "chess cache: LAPLACE_CHESS names %s, which is not there (laplace chess makes it)\n", path); exit(1); } return; }
    const lp_perfcache *pc = lp_perfcache_map(path);
    if (!pc) { fprintf(stderr, "chess cache: %s does not load (truncated, changed or not a perf-cache module); make it again: laplace chess\n", path); exit(1); }
    const char *why = conforms(pc, SEC);
    if (why) { fprintf(stderr, "chess cache: %s: %s; make it again: laplace chess\n", path, why); exit(1); }
    if (!lp_perfcache_bound(pc, T0)) { fprintf(stderr, "chess cache: %s was composed from another tier 0: it is stale; make it again: laplace chess\n", path); exit(1); }
    PC = pc;
}
const lp_perfcache *chess_cache(void){ ONCE(mapped_done, map_mu, map_once_run); return PC; }

/* ---- what the load's database holds */
static int loading; static char *load_conninfo;
static uint8_t *held;                                  /* per record: 1 held (or a codepoint), 0 not */
static int held_done; static pthread_mutex_t held_mu = PTHREAD_MUTEX_INITIALIZER;
void chess_cache_database(const char *conninfo){ loading = 1; free(load_conninfo); load_conninfo = conninfo ? strdup(conninfo) : NULL; }
static void held_run(void){
    if (!loading || !chess_cache()) return;
    uint8_t *h = calloc(PC->nrec ? PC->nrec : 1, 1); lp_idmap *ask = lp_idmap_new();
    for (size_t i = 0; i < PC->nrec; i++) if (PC->rec[i].pad) lp_idmap_put(ask, &PC->rec[i].id, NULL); else h[i] = 1;
    size_t n = lp_idmap_count(ask);
    uint8_t *hit = n ? db_recorded(load_conninfo, lp_idmap_keys(ask), (uint64_t)n) : NULL;
    size_t found = 0;
    for (size_t i = 0; i < PC->nrec; i++) if (PC->rec[i].pad) { int64_t at = lp_idmap_find(ask, &PC->rec[i].id); if (at >= 0 && hit[at]) { h[i] = 1; found++; } }
    fprintf(stderr, "\n  chess cache: the database holds %zu of its %zu composed records\n", found, PC->nrec);
    free(hit); lp_idmap_free(ask); held = h;
}
static const uint8_t *held_records(void){ ONCE(held_done, held_mu, held_run); return held; }
int chess_cache_admitted(const lp_id *id){
    if (!loading || !__atomic_load_n(&mapped_done, __ATOMIC_ACQUIRE) || !PC) return 0;
    const uint8_t *h = held_records(); if (!h) return 0;
    const lp_tier0_record *x = lp_perfcache_find(PC, id, NULL);
    return x && x->pad && h[x - PC->rec];
}
int chess_cache_ref(const lp_id *id, Ref *out){
    if (!__atomic_load_n(&mapped_done, __ATOMIC_ACQUIRE) || !PC) return 0;
    const lp_tier0_record *x = lp_perfcache_find(PC, id, NULL); if (!x) return 0;
    *out = ref_of(x); return 1;
}
static int vocabulary(int s, int64_t slot, Ref *out){
    if (!loading || slot < 0 || !chess_cache()) return 0;
    const uint8_t *h = held_records(); if (!h) return 0;
    const lp_tier0_record *x = lp_perfcache_at(PC, SEC[s], (uint32_t)slot);
    if (!x || !h[x - PC->rec]) return 0;
    *out = ref_of(x); return 1;
}
int chess_square(int s, Ref *out){ return s >= 0 && s < 64 && vocabulary(S_SQUARE, s, out); }
int chess_piece_square(int8_t p, int s, Ref *out){ return p && abs(p) <= KING && s >= 0 && s < 64 && vocabulary(S_PIECE_SQUARE, piece_index(p) * 64 + s, out); }
int chess_castling(unsigned w, unsigned b, Ref *out){
    law(); if (w > 255 || b > 255 || mask_place[w] < 0 || mask_place[b] < 0) return 0;
    return vocabulary(S_CASTLING, mask_place[w] * NMASKS + mask_place[b], out);
}
int chess_en_passant(int ep, Ref *out){ return ep < 64 && vocabulary(S_EN_PASSANT, ep_slot(ep), out); }
int chess_move(const Bd *b, const Mv *m, Ref *out){
    law(); int8_t p = b->sq[m->from]; if (!p || m->to > 63) return 0;
    uint16_t k = move_slot[piece_index(p)][m->from][m->to][promo_index(m->promo)];
    return k && vocabulary(S_MOVE, k - 1, out);
}
ChessCapture chess_capture;

/* ---- laplace chess: the generator */
typedef struct { Bd before, after; Mv m; Ref p, move, next; } Played;
static lp_idmap *played; static pthread_mutex_t played_mu = PTHREAD_MUTEX_INITIALIZER;
static void capture(const Bd *before, const Mv *m, const Bd *after, const Ref *p, const Ref *move, const Ref *next){
    lp_id t[3] = { p->id, move->id, next->id }, id; lp_id_compose(t, 3, &id);
    pthread_mutex_lock(&played_mu); bool fresh;
    size_t at = lp_idmap_put(played, &id, &fresh);
    if (fresh) { Played *x = lp_idmap_at(played, at); x->before = *before; x->after = *after; x->m = *m; x->p = *p; x->move = *move; x->next = *next; }
    pthread_mutex_unlock(&played_mu);
}
typedef lp_vec(lp_tier0_record) Records;
static int by_id(const void *a, const void *b){ return memcmp(((const lp_tier0_record *)a)->id.b, ((const lp_tier0_record *)b)->id.b, 16); }
static int by_key(const void *a, const void *b){ const lp_pair *x = a, *y = b; int c = memcmp(x->a.b, y->a.b, 16); return c ? c : memcmp(x->b.b, y->b.b, 16); }
/* An admitted section: sorted by ID, each ID once, rank its place. */
static void admitted(Records *r){
    if (!r->n) return;
    qsort(r->v, r->n, sizeof *r->v, by_id); size_t k = 0;
    for (size_t i = 0; i < r->n; i++) if (!k || !lp_id_eq(&r->v[k - 1].id, &r->v[i].id)) r->v[k++] = r->v[i];
    r->n = k; for (size_t i = 0; i < k; i++) r->v[i].rank = (uint32_t)i;
}
static int fail(const char *what, uint64_t n){ if (n) fprintf(stderr, "chess cache: %llu %s\n", (unsigned long long)n, what); return n != 0; }

/* Every record of a mapped module recomputed: the dense vocabulary slot by slot from the law, and every transition the
 * module holds replayed from the standard start position under the rules (chess_rules.c), each position, board, move
 * and transition record checked as the ingest composes it. A transition no line from the start reaches (a game begun
 * from a FEN) is reported, not guessed at. */
static int verify(const lp_perfcache *pc, const lp_section **sec){
    uint64_t bad = 0, dense = 0, positions = 0, transitions = 0;
    for (int s = 0; s <= S_MOVE; s++) for (uint32_t i = 0; i < sec[s]->count; i++) {
        int rules; Ref r = dense_ref(s, i, &rules); const lp_tier0_record *x = lp_perfcache_at(pc, sec[s], i);
        if (!rules || !x || !record_is(x, &r)) { if (bad < 5) fprintf(stderr, "chess cache: %s %u is not the law's composition\n", SECTION[s], i); bad++; } else dense++;
    }
    typedef struct { Bd b; Ref p; } Open;
    lp_vec(Open) open = { 0 }; lp_idmap *seen = lp_idmap_new(); Bd b; board_start(&b);
    { Ref board, p = position_ref(&b, &board); lp_idmap_put(seen, &p.id, NULL); lp_push(&open, ((Open){ b, p })); }
    for (size_t at = 0; at < open.n; at++) {
        Open o = open.v[at]; const lp_section *in = NULL; Ref board; Ref p = position_ref(&o.b, &board);
        const lp_tier0_record *xp = lp_perfcache_find(pc, &p.id, &in), *xb = lp_perfcache_find(pc, &board.id, NULL);
        if (xp && in == sec[S_POSITION]) {
            if (!record_is(xp, &p) || !xb || !record_is(xb, &board)) { bad++; continue; }
            positions++;
        } else if (at) { bad++; continue; }                     /* a position a held move makes is held */
        Mv all[256]; int n = gen(&o.b, all);
        for (int i = 0; i < n; i++) {
            Ref mv = move_ref(o.b.sq[all[i].from], all[i].from, all[i].to, all[i].promo);
            const lp_pair *pr = lp_perfcache_pair(pc, sec[S_NEXT], &p.id, &mv.id); if (!pr) continue;
            Bd t = o.b; make(&t, &all[i]); Ref next = position_ref(&t, NULL); lp_id rules_next, rules_move;
            id_position(&t, &rules_next); id_move(&o.b, &all[i], &rules_move);
            Ref tr[3] = { p, mv, next }; Ref transition = comp(tr, 3);
            const lp_tier0_record *xm = lp_perfcache_find(pc, &mv.id, NULL), *xt = lp_perfcache_find(pc, &transition.id, NULL);
            if (!lp_id_eq(&pr->c, &next.id) || !lp_id_eq(&rules_next, &next.id) || !lp_id_eq(&rules_move, &mv.id)
                || !xm || !record_is(xm, &mv) || !xt || !record_is(xt, &transition)) { if (bad < 5) fputs("chess cache: a transition is not the rules' move\n", stderr); bad++; continue; }
            transitions++; bool fresh; lp_idmap_put(seen, &next.id, &fresh); if (fresh) lp_push(&open, ((Open){ t, next }));
        }
    }
    printf("verified: %llu dense records (%u moves) the law's compositions with chess_rules' IDs; %llu of %u positions and %llu of %zu transitions replayed from the standard start\n",
           (unsigned long long)dense, sec[S_MOVE]->count, (unsigned long long)positions, sec[S_POSITION]->count, (unsigned long long)transitions, pc->npairs);
    if (transitions < pc->npairs) printf("          %llu transitions no line from the standard start reaches (games begun from a FEN): checked as records only\n", (unsigned long long)(pc->npairs - transitions));
    lp_vec_free(&open); lp_idmap_free(seen);
    return fail("records are not the law's", bad);
}

int cmd_chess(int argc, char **argv){
    const char *out = NULL, *t0p = NULL, *rdir = NULL, *threads = NULL, *source = NULL; int check = 0, print = 0;
    int a = opts(argc, argv, (const Opt[]){ { .flag = "-o", .kind = 's', .at = &out }, { .flag = "-t", .kind = 's', .at = &t0p },
        { .flag = "-r", .kind = 's', .at = &rdir }, { .flag = "-j", .kind = 's', .at = &threads }, { .flag = "-s", .kind = 's', .at = &source },
        { .flag = "--verify", .kind = 'b', .at = &check }, { .flag = "--fingerprint", .kind = 'b', .at = &print }, { 0 } });
    if ((a < argc && argv[a][0] == '-') || ((check || print) && a < argc)) {
        fputs("usage: laplace chess [-o chess.bin] [-t tier0.bin] [-r recipes] [-j threads] [-s source] [source|file...]\n"
              "       laplace chess [-o chess.bin] [-t tier0.bin] --verify | --fingerprint\n", stderr); return 2; }
    tier0_open(t0p); law();
    if (!out) out = chess_cache_path();
    if (check || print) {
        const lp_perfcache *pc = lp_perfcache_map(out); const lp_section *sec[NSECTIONS];
        if (!pc) { fprintf(stderr, "chess cache: %s does not load\n", out); return 1; }
        const char *why = conforms(pc, sec); if (why) { fprintf(stderr, "chess cache: %s: %s\n", out, why); return 1; }
        uint8_t fp[32]; char hex[65]; lp_perfcache_fingerprint(pc, fp); lp_hex(fp, 32, hex);
        printf("%s\nfingerprint (BLAKE3-256 of the module) %s\n%zu records, %zu transitions\n", out, hex, pc->nrec, pc->npairs);
        if (!lp_perfcache_bound(pc, T0)) { fputs("chess cache: composed from another tier 0: stale\n", stderr); return 1; }
        return check ? verify(pc, sec) : 0;
    }
    double started = now(); Records rec = { 0 }; lp_section sec[NSECTIONS]; memset(sec, 0, sizeof sec); uint64_t bad = 0;
    for (int s = 0; s <= S_MOVE; s++) {
        snprintf(sec[s].name, sizeof sec[s].name, "%s", SECTION[s]); sec[s].kind = LP_PERFCACHE_DENSE; sec[s].count = dense_count(s);
        for (uint32_t i = 0; i < sec[s].count; i++) { int rules; Ref r = dense_ref(s, i, &rules); bad += !rules; lp_push(&rec, record_of(r, i)); }
    }
    if (fail("vocabulary records whose ID is not chess_rules'", bad)) return 1;
    played = lp_idmap_sized(sizeof(Played));
    if (a < argc) {
        /* Read in this process, as laplace ingest --no-load reads them, so the decomposer that populates the database is
         * the one that composes what is cached: a source named is its files (its recipes' roots), read as that source's
         * (-s), never a child run of it and of what it comes after. */
        lp_vec(const char *) av = { 0 }; lp_vec(char *) owned = { 0 }; const char *as = source;
        if (!source) {
            Recipe *rc = NULL; int nrc = recipes_load(rdir ? rdir : laplace_recipes(), &rc), ns = 0; Source *src = sources_loaded(&ns);
            for (int i = 0; i < ns && a + 1 == argc; i++) if (source_named(&src[i], argv[a])) {
                if (src[i].unset[0] || !src[i].found[0]) { fprintf(stderr, "chess cache: source %s is not at any of its roots\n", src[i].name); return 1; }
                if (recipes_broken(rc, nrc, &src[i])) return 2;
                char **paths; Recipe **of; int np = source_files(&src[i], rc, nrc, &paths, &of);
                for (int f = 0; f < np; f++) lp_push(&owned, paths[f]);
                free(paths); free(of); as = src[i].name; a++; break;
            }
        }
        lp_push(&av, "ingest"); lp_push(&av, "--no-load");
        if (t0p) { lp_push(&av, "-t"); lp_push(&av, t0p); } if (rdir) { lp_push(&av, "-r"); lp_push(&av, rdir); }
        if (threads) { lp_push(&av, "-j"); lp_push(&av, threads); } if (as) { lp_push(&av, "-s"); lp_push(&av, as); }
        for (size_t f = 0; f < owned.n; f++) lp_push(&av, owned.v[f]);
        for (; a < argc; a++) lp_push(&av, argv[a]);
        lp_push(&av, NULL); chess_capture = capture; fflush(stdout);
        int rc = cmd_ingest((int)av.n - 1, (char **)av.v); chess_capture = NULL;
        for (size_t f = 0; f < owned.n; f++) free(owned.v[f]);
        lp_vec_free(&owned); lp_vec_free(&av);
        if (rc) { fputs("chess cache: the sources did not decompose\n", stderr); return 1; }
    }
    /* the admitted estate, each record recomposed by the law from the boards the decomposer replayed, and checked
     * against what chess.c composed and against chess_rules' IDs */
    Records board = { 0 }, position = { 0 }, transition = { 0 }; lp_vec(lp_pair) next = { 0 };
    for (size_t i = 0; i < lp_idmap_count(played); i++) {
        const Played *x = lp_idmap_at(played, i); Ref b0, b1; lp_id rp, rn, rm;
        Ref p = position_ref(&x->before, &b0), q = position_ref(&x->after, &b1), mv = move_ref(x->before.sq[x->m.from], x->m.from, x->m.to, x->m.promo);
        id_position(&x->before, &rp); id_position(&x->after, &rn); id_move(&x->before, &x->m, &rm);
        Ref tr[3] = { p, mv, q }; Ref t = comp(tr, 3);
        if (!same(&p, &x->p) || !same(&q, &x->next) || !same(&mv, &x->move) || !lp_id_eq(&rp, &p.id) || !lp_id_eq(&rn, &q.id) || !lp_id_eq(&rm, &mv.id)) { bad++; continue; }
        lp_push(&board, record_of(b0, 0)); lp_push(&board, record_of(b1, 0));
        lp_push(&position, record_of(p, 0)); lp_push(&position, record_of(q, 0));
        lp_push(&transition, record_of(t, 0)); lp_push(&next, ((lp_pair){ p.id, mv.id, q.id }));
    }
    if (fail("played moves the decomposer composed otherwise than the law and chess_rules", bad)) return 1;
    admitted(&board); admitted(&position); admitted(&transition);
    if (next.n) qsort(next.v, next.n, sizeof *next.v, by_key);
    Records *sparse[3] = { &board, &position, &transition };
    for (int s = S_BOARD; s <= S_TRANSITION; s++) {
        snprintf(sec[s].name, sizeof sec[s].name, "%s", SECTION[s]); sec[s].kind = LP_PERFCACHE_SPARSE; sec[s].count = (uint32_t)sparse[s - S_BOARD]->n;
        for (size_t i = 0; i < sparse[s - S_BOARD]->n; i++) lp_push(&rec, sparse[s - S_BOARD]->v[i]);
    }
    snprintf(sec[S_NEXT].name, sizeof sec[S_NEXT].name, "%s", SECTION[S_NEXT]); sec[S_NEXT].kind = LP_PERFCACHE_PAIRS; sec[S_NEXT].count = (uint32_t)next.n;
    uint8_t depends[32], fp[32]; char hex[65]; lp_tier0_fingerprint(T0, depends);
    if (!lp_perfcache_write(out, "chess", depends, sec, NSECTIONS, rec.v, rec.n, next.v, next.n, fp)) { fprintf(stderr, "chess cache: %s: %s\n", out, strerror(errno)); return 1; }
    lp_hex(fp, 32, hex);
    printf("%s: %zu records (%u vocabulary: %u squares, %u piece-squares, %u numbers, %u castlings, %u en passant fields, %u moves; %zu boards, %zu positions, %zu transitions), %zu (position, move) -> position\n"
           "fingerprint (BLAKE3-256 of the module) %s\nnative generation completed in %.2f s\n",
           out, rec.n, sec[S_SQUARE].count + sec[S_PIECE_SQUARE].count + sec[S_SIDE].count + sec[S_NUMBER].count + sec[S_CASTLING].count + sec[S_EN_PASSANT].count + sec[S_MOVE].count,
           sec[S_SQUARE].count, sec[S_PIECE_SQUARE].count, sec[S_NUMBER].count, sec[S_CASTLING].count, sec[S_EN_PASSANT].count, sec[S_MOVE].count,
           board.n, position.n, transition.n, next.n, hex, now() - started);
    lp_vec_free(&rec); lp_vec_free(&board); lp_vec_free(&position); lp_vec_free(&transition); lp_vec_free(&next); lp_idmap_free(played);
    return 0;
}
