/* Bounded coarse C chess search: legal gen + Syzygy + batched trajectory/standing/4D-Fréchet COUPLE + PVS/alpha-beta. */
#include "chess_search.h"
#include "chess_tablebase.h"
#include "laplace/os.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { SCORE_INF = 300000, SCORE_MATE = 200000 };
static const int MAT[7] = { 0, 100, 320, 330, 500, 900, 0 };

static uint64_t splitmix64(uint64_t x){
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t board_hash(const Bd *b){
    uint64_t h = splitmix64((uint64_t)b->side | ((uint64_t)b->rights[0] << 1) | ((uint64_t)b->rights[1] << 9) |
                            ((uint64_t)(uint8_t)b->ep << 17) | ((uint64_t)b->checks[0] << 25) | ((uint64_t)b->checks[1] << 33));
    for (int s = 0; s < 64; s++) {
        if (!b->sq[s]) continue;
        h ^= splitmix64(((uint64_t)(s + 1) << 8) ^ (uint64_t)(b->sq[s] + 16));
    }
    if (b->law && b->law->drops) {
        for (int c = 0; c < 2; c++) for (int p = 1; p <= 5; p++) {
            if (b->hand[c][p]) h ^= splitmix64(((uint64_t)(c * 8 + p) << 16) ^ b->hand[c][p]);
        }
    }
    return h;
}

int lp_chess_move_uci(const Mv *m, const Bd *b, char out[8]){
    if (!m || !out) return 1;
    if (m->drop) {
        out[0] = ".PNBRQK"[m->drop <= KING ? m->drop : PAWN];
        out[1] = '@';
        out[2] = (char)('a' + (m->to & 7));
        out[3] = (char)('1' + (m->to >> 3));
        out[4] = 0;
        return 0;
    }
    int to = m->to;
    if (m->castle && !(b && b->c960)) {
        to = ((m->to & 7) > (m->from & 7)) ? ((m->from & ~7) + 6) : ((m->from & ~7) + 2);
    }
    char *o = out;
    *o++ = (char)('a' + (m->from & 7));
    *o++ = (char)('1' + (m->from >> 3));
    *o++ = (char)('a' + (to & 7));
    *o++ = (char)('1' + (to >> 3));
    if (m->promo) *o++ = ".pnbrqk"[m->promo <= KING ? m->promo : QUEEN];
    *o = 0;
    return 0;
}

static int count_pieces(const Bd *b){
    int n = 0;
    for (int s = 0; s < 64; s++) if (b->sq[s]) n++;
    return n;
}

static int is_capture(const Bd *b, const Mv *m){
    return !m->castle && !m->drop && (b->sq[m->to] != 0 || m->ep);
}

static int cheb_dist(int a, int b){
    int df = abs((a & 7) - (b & 7)), dr = abs((a >> 3) - (b >> 3));
    return df > dr ? df : dr;
}

static int evaluate(const Bd *b, double scale){
    const LpChessLaw *w = b->law;
    int score = 0;
    int kw = -1, kb = -1;
    int wpawns[8] = { 0 }, bpawns[8] = { 0 };
    int wbish = 0, bbish = 0;
    int wpawn_sq[8], bpawn_sq[8], nwp = 0, nbp = 0;
    int wrook_f[8], brook_f[8], nwr = 0, nbr = 0;

    for (int s = 0; s < 64; s++) {
        int8_t p = b->sq[s];
        if (!p) continue;
        int ap = abs(p), c = p < 0 ? 1 : 0, sg = c ? -1 : 1;
        int f = s & 7, r = s >> 3;
        int rank_rel = c ? 7 - r : r;
        int psq = 0;
        if (ap == PAWN) {
            psq = rank_rel * 8 + (f >= 2 && f <= 5 ? 6 : 0);
            if (c == 0) { wpawns[f]++; if (nwp < 8) wpawn_sq[nwp++] = s; }
            else { bpawns[f]++; if (nbp < 8) bpawn_sq[nbp++] = s; }
        } else if (ap == KNIGHT || ap == BISHOP) {
            int center = 3 - (abs(f * 2 - 7) + abs(r * 2 - 7)) / 4;
            psq = center * 12;
            if (ap == BISHOP) { if (c == 0) wbish++; else bbish++; }
        } else if (ap == ROOK) {
            if (c == 0) { if (nwr < 8) wrook_f[nwr++] = f; }
            else { if (nbr < 8) brook_f[nbr++] = f; }
        } else if (ap == KING) {
            if (c == 0) kw = s; else kb = s;
        }
        score += sg * (MAT[ap <= KING ? ap : PAWN] + psq);
    }

    /* Deterministic structural planes: bishop pair, doubled/isolated/passed pawns, rook files, king shield */
    if (wbish >= 2) score += 30;
    if (bbish >= 2) score -= 30;
    for (int f = 0; f < 8; f++) {
        if (wpawns[f] > 1) score -= (wpawns[f] - 1) * 15;
        if (bpawns[f] > 1) score += (bpawns[f] - 1) * 15;
        int wadj = (f > 0 ? wpawns[f - 1] : 0) + (f < 7 ? wpawns[f + 1] : 0);
        int badj = (f > 0 ? bpawns[f - 1] : 0) + (f < 7 ? bpawns[f + 1] : 0);
        if (wpawns[f] > 0 && wadj == 0) score -= wpawns[f] * 12;
        if (bpawns[f] > 0 && badj == 0) score += bpawns[f] * 12;
    }
    for (int i = 0; i < nwp; i++) {
        int f = wpawn_sq[i] & 7, r = wpawn_sq[i] >> 3, passed = 1;
        for (int j = 0; j < nbp && passed; j++) {
            int bf = bpawn_sq[j] & 7, br = bpawn_sq[j] >> 3;
            if (abs(bf - f) <= 1 && br > r) passed = 0;
        }
        if (passed) score += 12 * r;
    }
    for (int i = 0; i < nbp; i++) {
        int f = bpawn_sq[i] & 7, r = bpawn_sq[i] >> 3, passed = 1;
        for (int j = 0; j < nwp && passed; j++) {
            int wf = wpawn_sq[j] & 7, wr = wpawn_sq[j] >> 3;
            if (abs(wf - f) <= 1 && wr < r) passed = 0;
        }
        if (passed) score -= 12 * (7 - r);
    }
    for (int i = 0; i < nwr; i++) {
        int f = wrook_f[i];
        if (wpawns[f] == 0 && bpawns[f] == 0) score += 20;
        else if (wpawns[f] == 0) score += 10;
    }
    for (int i = 0; i < nbr; i++) {
        int f = brook_f[i];
        if (wpawns[f] == 0 && bpawns[f] == 0) score -= 20;
        else if (bpawns[f] == 0) score -= 10;
    }
    if (!w || (!w->goal && !w->unroyal)) {
        if (kw >= 0 && (kw >> 3) + 1 <= 7) {
            int kf = kw & 7, sr = (kw >> 3) + 1;
            for (int df = -1; df <= 1; df++) {
                int f = kf + df;
                if (f >= 0 && f < 8 && b->sq[sr * 8 + f] == PAWN) score += 10;
            }
        }
        if (kb >= 0 && (kb >> 3) - 1 >= 0) {
            int kf = kb & 7, sr = (kb >> 3) - 1;
            for (int df = -1; df <= 1; df++) {
                int f = kf + df;
                if (f >= 0 && f < 8 && b->sq[sr * 8 + f] == -PAWN) score -= 10;
            }
        }
    }

    if (w && w->drops) {
        for (int p = PAWN; p <= QUEEN; p++) score += ((int)b->hand[0][p] - (int)b->hand[1][p]) * ((MAT[p] * 3) / 4);
    }
    if (w && w->checks) score += ((int)b->checks[0] - (int)b->checks[1]) * 350;
    if (w && w->goal) {
        int bdist = 10, wdist = 10;
        for (int g = 0; g < 64; g++) {
            if (!(w->goal & (1ULL << g))) continue;
            if (kw >= 0) { int d = cheb_dist(kw, g); if (d < wdist) wdist = d; }
            if (kb >= 0) { int d = cheb_dist(kb, g); if (d < bdist) bdist = d; }
        }
        if (wdist < 10) score += (6 - wdist) * 140;
        if (bdist < 10) score -= (6 - bdist) * 140;
        if (w->goal_answered) {
            if (kw >= 0) score += (kw >> 3) * 45;
            if (kb >= 0) score -= (kb >> 3) * 45;
        }
    }
    if (w && w->unroyal && w->forced) score = -score;
    int pov = b->side ? -score : score;
    if (scale <= 0.0) return 0;
    return (int)((double)pov * scale);
}

enum { TT_SIZE = 1 << 15 };
typedef struct { uint64_t key; int16_t depth, score; uint8_t flag; Mv best; } TTEntry;
enum { TT_EXACT = 1, TT_ALPHA = 2, TT_BETA = 3 };

typedef struct {
    Mv mv;
    int prior;
    int64_t traj;
    int stood;
    double rating;
} RootMove;

typedef struct {
    lp_id  pos_id;
    int    valid;
    int    eval_bonus;
    lp_id  best_child;
    int    has_best;
} SubPosCache;

enum { SUB_CACHE_SIZE = 256 };

typedef struct {
    LpChessSearchPolicy pol;
    LpTablebase        *tb;
    PGconn             *pg;
    const lp_id        *witnesses;
    int                 nwitnesses;
    int                 scoped;
    uint64_t           *db_probes;
    uint64_t            max_db_probes;
    double              root_coord[4];
    int                 has_root_coord;
    SubPosCache        *sub_cache;
    lp_id               line_ids[16];
    int                 line_len;
    uint64_t            nodes;
    double              start_s;
    int                 aborted;
    TTEntry            *tt;
} SearchCtx;

/* Wave 2B game-altitude trajectory standing probe:
 * Uses one batched GIN set-intersection query (LP_SQL_HOLDERS_WITH_STANDINGS[_SCOPED])
 * with parts=child_ids and required=[pid] to find attested game trajectories [P0..Pn]
 * containing the consecutive board-state transition pid -> child_ids[i], and aggregates
 * their game-altitude Glicko-2 standings signed for the side to move (White: +1, Black: -1). */
static void probe_game_altitude_standings(
    PGconn *pg, const lp_id *pid, int side, const lp_id *child_ids, int nm,
    const lp_id *witnesses, int nwitnesses, int scoped,
    double *ev_out, int64_t *matches_out, double *rating_out, uint8_t *has_out)
{
    if (!pg || !pid || !child_ids || nm <= 0) return;
    if (scoped && (!witnesses || nwitnesses <= 0)) return;

    Args a = { 0 };
    arg_ids(&a, child_ids, (size_t)nm);
    arg_ids(&a, pid, 1);
    arg_bool(&a, 1);           /* per_part = true */
    arg_i16(&a, -1);           /* floor = -1 */
    arg_i64(&a, 24);           /* cap = 24 attested game trajectories per candidate move */
    arg_i16(&a, 1);            /* kinds = 1 (claims: attested trajectories) */
    arg_null(&a);              /* bits = NULL */
    arg_i16(&a, 0);            /* endpoint = 0 */
    if (scoped) arg_ids(&a, witnesses, (size_t)nwitnesses);

    PGresult *q = db_call_try(pg, scoped ? LP_SQL_HOLDERS_WITH_STANDINGS_SCOPED : LP_SQL_HOLDERS_WITH_STANDINGS, &a, NULL, 0);
    if (!q) { args_free(&a); return; }

    double *wsum = calloc((size_t)nm, sizeof(double));
    double *rsum = calloc((size_t)nm, sizeof(double));
    double side_sign = (side == 0) ? 1.0 : -1.0;

    int nr = PQntuples(q);
    for (int r = 0; r < nr; r++) {
        if (col_bool(q, r, 6) || PQgetisnull(q, r, 7)) continue;
        int64_t ord = col_i64(q, r, 0);
        if (ord < 1 || ord > nm) continue;
        int idx = (int)(ord - 1);

        /* Confirm consecutive state transition pid -> child_ids[idx] in the game trajectory. */
        lp_path p = col_path(q, r, 2);
        if (p.n < 2 || p.n > 1024) continue;
        lp_vertex *vx = malloc(sizeof(lp_vertex) * p.n);
        if (!vx) continue;
        lp_path_decode(p, vx, p.n);
        int adjacent = 0;
        for (size_t k = 0; k + 1 < p.n; k++) {
            if (lp_id_eq(&vx[k].id, pid) && lp_id_eq(&vx[k + 1].id, &child_ids[idx])) {
                adjacent = 1;
                break;
            }
        }
        free(vx);
        if (!adjacent) continue;

        lp_rating rat = col_rating(q, r, 7);
        int64_t m = PQgetisnull(q, r, 10) ? 1 : col_i64(q, r, 10);
        if (m <= 0) m = 1;
        double conf = lp_confidence(&rat, 2.0);
        if (conf <= 0.0) conf = 0.5;
        double w = conf * log1p((double)m);
        wsum[idx] += w;
        rsum[idx] += w * rat.rating;
        matches_out[idx] += m;
        has_out[idx] = 1;
    }

    for (int i = 0; i < nm; i++) {
        if (has_out[i] && wsum[i] > 0.0) {
            double avg_white_rating = rsum[i] / wsum[i];
            /* Express rating from side-to-move perspective centered at 1500. */
            double pov_rating = 1500.0 + side_sign * (avg_white_rating - 1500.0);
            if (rating_out) rating_out[i] = pov_rating;
            if (ev_out) ev_out[i] = (pov_rating - 1500.0) * 0.14;
        }
    }

    free(wsum);
    free(rsum);
    PQclear(q);
    args_free(&a);
}

/* Multi-ply batched substrate probe for inner search nodes (ply 1..2):
 * queries game-altitude [P0..Pn] Glicko-2 standings + physicality.path FORWARD_SUFFIX continuations + 4D Fréchet curve
 * continuity from root, cached by position ID so transpositions never re-hit PostgreSQL. */
static int probe_substrate_node(SearchCtx *ctx, const Bd *b, const Mv *moves, int nm, int *move_priors){
    if (!ctx->pg || !ctx->sub_cache || ctx->pol.book_weight <= 0 || nm <= 0) return 0;
    if (ctx->scoped && (!ctx->witnesses || ctx->nwitnesses <= 0)) return 0;
    lp_id pid;
    lp_chess_id_position(b, &pid);
    uint64_t h = 0;
    memcpy(&h, pid.b, sizeof h);
    SubPosCache *slot = &ctx->sub_cache[h & (SUB_CACHE_SIZE - 1)];
    if (slot->valid && lp_id_eq(&slot->pos_id, &pid)) {
        if (move_priors && slot->has_best) {
            for (int i = 0; i < nm; i++) {
                Bd nb = *b; lp_chess_make(&nb, &moves[i]);
                lp_id cid; lp_chess_id_position(&nb, &cid);
                if (lp_id_eq(&cid, &slot->best_child)) { move_priors[i] += 2500; break; }
            }
        }
        return slot->eval_bonus;
    }
    if (ctx->db_probes && *ctx->db_probes >= ctx->max_db_probes) return 0;

    lp_id *child_ids = malloc(sizeof(lp_id) * (size_t)nm);
    for (int i = 0; i < nm; i++) {
        Bd nb = *b;
        lp_chess_make(&nb, &moves[i]);
        lp_chess_id_position(&nb, &child_ids[i]);
    }

    double *alt_ev = calloc((size_t)nm, sizeof(double));
    int64_t *matches = calloc((size_t)nm, sizeof(int64_t));
    uint8_t *has = calloc((size_t)nm, 1);
    probe_game_altitude_standings(ctx->pg, &pid, b->side, child_ids, nm,
                                  ctx->witnesses, ctx->nwitnesses, ctx->scoped,
                                  alt_ev, matches, NULL, has);
    if (ctx->db_probes) (*ctx->db_probes)++;

    int64_t *traj = calloc((size_t)nm, sizeof(int64_t));
    if (!ctx->db_probes || *ctx->db_probes < ctx->max_db_probes) {
        Args a = { 0 };
        arg_ids(&a, &pid, 1);
        arg_i64(&a, 128);
        if (ctx->scoped) arg_ids(&a, ctx->witnesses, (size_t)ctx->nwitnesses);
        PGresult *q = db_call_try(ctx->pg, ctx->scoped ? LP_SQL_FORWARD_SUFFIX_SCOPED : LP_SQL_FORWARD_SUFFIX, &a, NULL, 0);
        if (q) {
            int nr = PQntuples(q);
            for (int r = 0; r < nr; r++) {
                const lp_id *nid = col_id(q, r, 2);
                int64_t times = col_i64(q, r, 3);
                if (times <= 0) continue;
                for (int i = 0; i < nm; i++) {
                    if (lp_id_eq(nid, &child_ids[i])) { traj[i] += times; break; }
                }
            }
            PQclear(q);
        }
        args_free(&a);
        if (ctx->db_probes) (*ctx->db_probes)++;
    }

    double bw = (double)ctx->pol.book_weight / 100.0;
    int best_bonus = 0;
    int best_idx = -1;
    for (int i = 0; i < nm; i++) {
        double ev = 0.0;
        if (has[i] && matches[i] > 0) ev += alt_ev[i];
        if (traj[i] > 0) ev += log1p((double)traj[i]) * 30.0;
        int pr = (int)(ev * bw);
        if (move_priors) move_priors[i] = pr;
        if (pr > best_bonus) { best_bonus = pr; best_idx = i; }
    }

    /* Multi-ply unpacked 4D entity.coord Fréchet curve affinity over [P_0 .. P_k, P_{k+1}] */
    if (best_idx >= 0 && ctx->line_len >= 1 && (!ctx->db_probes || *ctx->db_probes < ctx->max_db_probes)) {
        int plen = ctx->line_len < 15 ? ctx->line_len : 15;
        int total_q = plen + nm;
        lp_id *qids = malloc(sizeof(lp_id) * (size_t)total_q);
        for (int k = 0; k < plen; k++) qids[k] = ctx->line_ids[k];
        for (int i = 0; i < nm; i++) qids[plen + i] = child_ids[i];
        double *coords = calloc((size_t)total_q * 4u, sizeof(double));
        uint8_t *has_c = calloc((size_t)total_q, 1);
        coords_of(ctx->pg, qids, total_q, coords, has_c);
        if (ctx->db_probes) (*ctx->db_probes)++;

        double ref_c[16 * 4];
        size_t rn = 0;
        for (int k = 0; k < plen; k++) {
            if (has_c[k]) { memcpy(ref_c + rn * 4u, coords + (size_t)k * 4u, 32); rn++; }
        }
        if (rn >= 1u && has_c[plen + best_idx]) {
            memcpy(ref_c + rn * 4u, coords + (size_t)(plen + best_idx) * 4u, 32);
            size_t ref_len = rn + 1u;
            double cand_c[16 * 4];
            memcpy(cand_c, ref_c, rn * 32u);
            for (int i = 0; i < nm; i++) {
                if (!has_c[plen + i]) continue;
                memcpy(cand_c + rn * 4u, coords + (size_t)(plen + i) * 4u, 32);
                double fd = lp_frechet4(cand_c, ref_len, ref_c, ref_len);
                double d = (isfinite(fd) && fd >= 0.0) ? fd : 10.0;
                int bonus = (int)((18.0 / (1.0 + d)) * bw);
                if (move_priors) move_priors[i] += bonus;
                if (move_priors && move_priors[i] > best_bonus) best_bonus = move_priors[i];
            }
        }
        free(qids); free(coords); free(has_c);
    }

    slot->pos_id = pid;
    slot->valid = 1;
    slot->eval_bonus = best_bonus;
    slot->has_best = (best_idx >= 0);
    if (best_idx >= 0) slot->best_child = child_ids[best_idx];

    free(child_ids); free(alt_ev); free(matches); free(has); free(traj);
    return best_bonus;
}

static int time_or_node_limit(SearchCtx *ctx){
    if (ctx->aborted) return 1;
    if (ctx->pol.max_nodes && ctx->nodes >= ctx->pol.max_nodes) { ctx->aborted = 1; return 1; }
    if (ctx->pol.max_wall_us && (ctx->nodes & 255ULL) == 0) {
        if ((uint64_t)((now() - ctx->start_s) * 1e6) >= ctx->pol.max_wall_us) { ctx->aborted = 1; return 1; }
    }
    return 0;
}

static int move_order_key(const Bd *b, const Mv *m, const Mv *ttm, int prior){
    if (ttm && memcmp(m, ttm, sizeof *m) == 0) return 10000000;
    int k = prior;
    if (is_capture(b, m)) {
        int cap = abs(b->sq[m->to] ? b->sq[m->to] : PAWN);
        int atk = abs(b->sq[m->from]);
        k += 100000 + MAT[cap <= KING ? cap : PAWN] * 16 - MAT[atk <= KING ? atk : PAWN];
    }
    if (m->promo) k += 80000 + MAT[m->promo <= KING ? m->promo : QUEEN];
    if (m->drop) k += 5000 + MAT[m->drop <= KING ? m->drop : PAWN];
    return k;
}

static void sort_moves(const Bd *b, Mv *moves, const int *priors, int n, const Mv *ttm){
    int keys[LP_CHESS_MOVES];
    for (int i = 0; i < n; i++) keys[i] = move_order_key(b, &moves[i], ttm, priors ? priors[i] : 0);
    for (int i = 1; i < n; i++) {
        Mv mv = moves[i]; int ky = keys[i]; int j = i - 1;
        while (j >= 0 && keys[j] < ky) { moves[j + 1] = moves[j]; keys[j + 1] = keys[j]; j--; }
        moves[j + 1] = mv; keys[j + 1] = ky;
    }
}

static void sort_root_moves(const Bd *b, RootMove *rm, int n){
    int keys[LP_CHESS_MOVES];
    for (int i = 0; i < n; i++) keys[i] = move_order_key(b, &rm[i].mv, NULL, rm[i].prior);
    for (int i = 1; i < n; i++) {
        RootMove cur = rm[i]; int ky = keys[i]; int j = i - 1;
        while (j >= 0 && keys[j] < ky) { rm[j + 1] = rm[j]; keys[j + 1] = keys[j]; j--; }
        rm[j + 1] = cur; keys[j + 1] = ky;
    }
}

static int quiesce(SearchCtx *ctx, const Bd *b, int alpha, int beta, int qply){
    ctx->nodes++;
    if (time_or_node_limit(ctx)) return 0;

    int term = lp_chess_result(b);
    if (term != LP_CHESS_ON) {
        if (term == LP_CHESS_DRAW) return 0;
        int winner = (term == LP_CHESS_WHITE) ? 0 : 1;
        return (winner == b->side) ? (SCORE_MATE - qply) : -(SCORE_MATE - qply);
    }

    int stand = evaluate(b, ctx->pol.eval_scale);
    if (qply >= ctx->pol.quiesce_depth) return stand;
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;

    Mv moves[LP_CHESS_MOVES];
    int nm = lp_chess_gen(b, moves);
    sort_moves(b, moves, NULL, nm, NULL);

    for (int i = 0; i < nm; i++) {
        if (!is_capture(b, &moves[i]) && !moves[i].promo) continue;
        Bd next = *b;
        lp_chess_make(&next, &moves[i]);
        int score = -quiesce(ctx, &next, -beta, -alpha, qply + 1);
        if (ctx->aborted) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

static int pvs(SearchCtx *ctx, const Bd *b, int depth, int alpha, int beta, int ply){
    ctx->nodes++;
    if (time_or_node_limit(ctx)) return 0;

    int term = lp_chess_result(b);
    if (term != LP_CHESS_ON) {
        if (term == LP_CHESS_DRAW) return 0;
        int winner = (term == LP_CHESS_WHITE) ? 0 : 1;
        return (winner == b->side) ? (SCORE_MATE - ply) : -(SCORE_MATE - ply);
    }

    if (depth <= 0) return quiesce(ctx, b, alpha, beta, 0);

    if (ctx->tb && ctx->pol.syzygy_pieces > 0 && ply > 0 &&
        (!b->law || (!b->law->drops && !b->law->explodes && !b->law->checks && !b->law->goal && !b->law->unroyal)) &&
        !b->rights[0] && !b->rights[1] && count_pieces(b) <= ctx->pol.syzygy_pieces) {
        int wdl = 0;
        if (lp_tablebase_wdl(ctx->tb, b, &wdl)) {
            if (wdl > 1) return SCORE_MATE - 100 - ply;
            if (wdl < -1) return -(SCORE_MATE - 100 - ply);
            return 0;
        }
    }

    uint64_t key = board_hash(b);
    TTEntry *te = &ctx->tt[key & (TT_SIZE - 1)];
    Mv ttm = { 0 }; int has_ttm = 0;
    if (te->key == key && te->flag) {
        ttm = te->best; has_ttm = 1;
        if (te->depth >= depth) {
            if (te->flag == TT_EXACT) return te->score;
            if (te->flag == TT_ALPHA && te->score <= alpha) return alpha;
            if (te->flag == TT_BETA && te->score >= beta) return beta;
        }
    }

    Mv moves[LP_CHESS_MOVES];
    int nm = lp_chess_gen(b, moves);
    if (nm <= 0) return evaluate(b, ctx->pol.eval_scale);
    int sub_priors[LP_CHESS_MOVES];
    int *priors_ptr = NULL;
    int sub_eval_bonus = 0;
    if (ctx->pg && ply <= 2 && depth >= 2 && ctx->pol.book_weight > 0) {
        memset(sub_priors, 0, sizeof(int) * (size_t)nm);
        sub_eval_bonus = probe_substrate_node(ctx, b, moves, nm, sub_priors);
        priors_ptr = sub_priors;
    }
    sort_moves(b, moves, priors_ptr, nm, has_ttm ? &ttm : NULL);

    int orig_alpha = alpha;
    int best_score = -SCORE_INF;
    Mv best_mv = moves[0];

    int pushed_line = 0;
    if (ctx->pg && ply > 0 && ctx->line_len < 15) {
        lp_chess_id_position(b, &ctx->line_ids[ctx->line_len]);
        ctx->line_len++;
        pushed_line = 1;
    }

    for (int i = 0; i < nm; i++) {
        Bd next = *b;
        lp_chess_make(&next, &moves[i]);
        int score;
        if (i == 0) {
            score = -pvs(ctx, &next, depth - 1, -beta, -alpha, ply + 1);
        } else {
            score = -pvs(ctx, &next, depth - 1, -alpha - 1, -alpha, ply + 1);
            if (!ctx->aborted && score > alpha && score < beta)
                score = -pvs(ctx, &next, depth - 1, -beta, -alpha, ply + 1);
        }
        if (ctx->aborted) { if (pushed_line) ctx->line_len--; return 0; }
        if (score > best_score) { best_score = score; best_mv = moves[i]; }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }
    if (pushed_line) ctx->line_len--;

    te->key = key;
    te->depth = (int16_t)depth;
    int final_score = best_score + (sub_eval_bonus / 4);
    te->score = (int16_t)(final_score > 30000 ? 30000 : final_score < -30000 ? -30000 : final_score);
    te->flag = (final_score <= orig_alpha) ? TT_ALPHA : (final_score >= beta) ? TT_BETA : TT_EXACT;
    te->best = best_mv;
    return final_score;
}

int lp_chess_search(PGconn *pg, const char *fen, const char *variant,
                    const lp_id *witnesses, int nwitnesses, int scoped,
                    const LpChessSearchPolicy *policy_override, LpChessSearchResult *out){
    if (!fen || !*fen || !out) return 2;
    memset(out, 0, sizeof *out);
    double t0 = now();

    LpChessSearchPolicy pol = policy_override ? *policy_override : lp_chess_search_policy();
    if (pol.max_depth <= 0) pol.max_depth = 6;
    if (pol.eval_scale < 0.0) pol.eval_scale = 1.0;

    const LpChessLaw *law = lp_chess_law_named(variant, variant ? strlen(variant) : 0);
    if (!law) return 2;

    Bd root;
    if (!lp_chess_board_law_fen(&root, fen, strlen(fen), law)) return 2;
    lp_chess_id_position(&root, &out->position_id);

    if (lp_chess_result(&root) != LP_CHESS_ON) return 1;

    Mv legal[LP_CHESS_MOVES];
    int nm = lp_chess_gen(&root, legal);
    if (nm <= 0) return 1;

    /* Root Syzygy tablebase check when law & piece count qualify. */
    LpTablebase *tb = (pol.syzygy_pieces > 0) ? lp_tablebase_shared() : NULL;
    if (tb && (!law->drops && !law->explodes && !law->checks && !law->goal && !law->unroyal) &&
        !root.rights[0] && !root.rights[1] && count_pieces(&root) <= pol.syzygy_pieces) {
        LpTbRanked ranked[256];
        int nr = lp_tablebase_rank(tb, &root, ranked, 256);
        int wdl = 0;
        if (nr > 0 && lp_tablebase_wdl(tb, &root, &wdl)) {
            Mv bm = ranked[0].move;
            Bd nb = root; lp_chess_make(&nb, &bm);
            lp_chess_move_uci(&bm, &root, out->best_uci);
            lp_chess_san_of(&root, &bm, legal, nm, 1, out->best_san);
            lp_chess_id_move(&root, &bm, &out->move_id);
            lp_chess_id_position(&nb, &out->next_position_id);
            lp_id c2[2] = { out->position_id, out->next_position_id };
            lp_id_compose(c2, 2, &out->claim_id);
            out->syzygy_wdl = wdl;
            out->from_syzygy = 1;
            out->score_cp = wdl > 0 ? 10000 - ranked[0].dtz : wdl < 0 ? -10000 - ranked[0].dtz : 0;
            out->depth_reached = 0;
            out->nodes_searched = 1;
            out->wall_us = (uint64_t)((now() - t0) * 1e6);
            snprintf(out->source, sizeof out->source, "syzygy");
            return 0;
        }
    }

    RootMove rm[LP_CHESS_MOVES];
    for (int i = 0; i < nm; i++) {
        rm[i] = (RootMove){ .mv = legal[i], .prior = 0, .traj = 0, .stood = 0, .rating = 0.0 };
    }

    /* Up to 3 batched Substrate COUPLE probes at root: trajectory suffixes + game-altitude standings + 4D Fréchet curves. */
    if (pg && pol.book_weight > 0 && !(scoped && (!witnesses || nwitnesses <= 0))) {
        lp_id *child_ids = malloc(sizeof(lp_id) * (size_t)nm);
        for (int i = 0; i < nm; i++) {
            Bd nb = root;
            lp_chess_make(&nb, &rm[i].mv);
            lp_chess_id_position(&nb, &child_ids[i]);
        }

        /* Probe 1: FORWARD_SUFFIX(_SCOPED) trajectory continuations */
        Args a = { 0 };
        arg_ids(&a, &out->position_id, 1);
        arg_i64(&a, 256);
        if (scoped) arg_ids(&a, witnesses, (size_t)nwitnesses);
        PGresult *q = db_call(pg, scoped ? LP_SQL_FORWARD_SUFFIX_SCOPED : LP_SQL_FORWARD_SUFFIX, &a);
        if (q) {
            int nr = PQntuples(q);
            for (int r = 0; r < nr; r++) {
                const lp_id *nid = col_id(q, r, 2);
                int64_t times = col_i64(q, r, 3);
                if (times <= 0) continue;
                for (int i = 0; i < nm; i++) {
                    if (lp_id_eq(nid, &child_ids[i])) {
                        rm[i].traj += times;
                        break;
                    }
                }
            }
            PQclear(q);
        }
        args_free(&a);
        out->db_probes++;

        /* Probe 2: Wave 2B game-altitude [P0..Pn] trajectory standings via GIN intersection */
        double *alt_ev = calloc((size_t)nm, sizeof(double));
        double *pov_rats = calloc((size_t)nm, sizeof(double));
        int64_t *matches = calloc((size_t)nm, sizeof(int64_t));
        uint8_t *has = calloc((size_t)nm, 1);
        probe_game_altitude_standings(pg, &out->position_id, root.side, child_ids, nm,
                                      witnesses, nwitnesses, scoped,
                                      alt_ev, matches, pov_rats, has);
        out->db_probes++;

        double bw = (double)pol.book_weight / 100.0;
        int best_ref_idx = -1;
        double best_ref_ev = 0.0;
        for (int i = 0; i < nm; i++) {
            double ev = 0.0;
            if (has[i] && matches[i] > 0) {
                rm[i].stood = 1;
                rm[i].rating = pov_rats[i];
                ev += alt_ev[i];
            }
            if (rm[i].traj > 0) {
                ev += log1p((double)rm[i].traj) * 35.0;
            }
            rm[i].prior = (int)(ev * bw);
            if ((rm[i].traj > 0 || (rm[i].stood && rm[i].rating >= 1500.0)) && ev > best_ref_ev) {
                best_ref_ev = ev;
                best_ref_idx = i;
            }
        }

        /* Probe 3: real 4D entity.coord Fréchet curve affinity against strongest attested trajectory branch */
        if (best_ref_idx >= 0) {
            lp_id *all_ids = malloc(sizeof(lp_id) * (size_t)(nm + 1));
            all_ids[0] = out->position_id;
            for (int i = 0; i < nm; i++) all_ids[i + 1] = child_ids[i];
            double *coords = calloc((size_t)(nm + 1) * 4u, sizeof(double));
            uint8_t *has_c = calloc((size_t)(nm + 1), 1);
            coords_of(pg, all_ids, nm + 1, coords, has_c);
            out->db_probes++;

            if (has_c[0] && has_c[best_ref_idx + 1]) {
                double ref_c[8];
                memcpy(ref_c, coords, sizeof(double) * 4u);
                memcpy(ref_c + 4, coords + (size_t)(best_ref_idx + 1) * 4u, sizeof(double) * 4u);
                for (int i = 0; i < nm; i++) {
                    if (!has_c[i + 1]) continue;
                    double cand_c[8];
                    memcpy(cand_c, coords, sizeof(double) * 4u);
                    memcpy(cand_c + 4, coords + (size_t)(i + 1) * 4u, sizeof(double) * 4u);
                    double fd = lp_frechet4(cand_c, 2u, ref_c, 2u);
                    double d = (isfinite(fd) && fd >= 0.0) ? fd : 10.0;
                    rm[i].prior += (int)((15.0 / (1.0 + d)) * bw);
                }
            }
            free(all_ids); free(coords); free(has_c);
        }

        free(child_ids); free(alt_ev); free(pov_rats); free(matches); free(has);
    }

    SearchCtx ctx = {
        .pol = pol, .tb = tb, .pg = pg,
        .witnesses = witnesses, .nwitnesses = nwitnesses, .scoped = scoped,
        .db_probes = &out->db_probes, .max_db_probes = 24u,
        .sub_cache = pg ? calloc(SUB_CACHE_SIZE, sizeof(SubPosCache)) : NULL,
        .line_len = 1,
        .nodes = 0, .start_s = t0, .aborted = 0,
        .tt = calloc(TT_SIZE, sizeof(TTEntry))
    };
    ctx.line_ids[0] = out->position_id;

    RootMove best_root = rm[0];
    int best_score = 0;
    int depth_done = 0;

    sort_root_moves(&root, rm, nm);

    for (int d = 1; d <= pol.max_depth; d++) {
        int alpha = -SCORE_INF, beta = SCORE_INF;
        int cur_best = -SCORE_INF;
        int cur_idx = 0;
        for (int i = 0; i < nm; i++) {
            Bd next = root;
            lp_chess_make(&next, &rm[i].mv);
            /* Shift PVS window by -prior so fail-hard cutoffs account for the substrate prior exactly. */
            int pr = rm[i].prior;
            int adj_alpha = alpha > -SCORE_INF + 5000 ? alpha - pr : -SCORE_INF;
            int adj_beta  = beta  <  SCORE_INF - 5000 ? beta  - pr :  SCORE_INF;
            int raw;
            if (i == 0) {
                raw = -pvs(&ctx, &next, d - 1, -adj_beta, -adj_alpha, 1);
            } else {
                raw = -pvs(&ctx, &next, d - 1, -adj_alpha - 1, -adj_alpha, 1);
                if (!ctx.aborted && raw > adj_alpha && raw < adj_beta)
                    raw = -pvs(&ctx, &next, d - 1, -adj_beta, -adj_alpha, 1);
            }
            if (ctx.aborted) break;
            int sc = raw + pr;
            if (sc > cur_best) { cur_best = sc; cur_idx = i; }
            if (sc > alpha) alpha = sc;
        }
        if (ctx.aborted) break;
        best_root = rm[cur_idx];
        best_score = cur_best;
        depth_done = d;
        if (cur_idx > 0) {
            RootMove tmp = rm[0]; rm[0] = rm[cur_idx]; rm[cur_idx] = tmp;
        }
        if (abs(best_score) >= SCORE_MATE - 100) break;
    }

    free(ctx.sub_cache);
    free(ctx.tt);

    Bd nb = root; lp_chess_make(&nb, &best_root.mv);
    lp_chess_move_uci(&best_root.mv, &root, out->best_uci);
    lp_chess_san_of(&root, &best_root.mv, legal, nm, 1, out->best_san);
    lp_chess_id_move(&root, &best_root.mv, &out->move_id);
    lp_chess_id_position(&nb, &out->next_position_id);
    lp_id c2[2] = { out->position_id, out->next_position_id };
    lp_id_compose(c2, 2, &out->claim_id);
    out->score_cp = best_score;
    out->depth_reached = depth_done;
    out->nodes_searched = ctx.nodes;
    out->wall_us = (uint64_t)((now() - t0) * 1e6);
    if (best_root.traj > 0) {
        out->from_book = 1;
        snprintf(out->source, sizeof out->source, "trajectory+search");
    } else if (best_root.stood && best_root.rating >= 1500.0) {
        out->from_book = 1;
        snprintf(out->source, sizeof out->source, "book+search");
    } else {
        out->from_book = 0;
        snprintf(out->source, sizeof out->source, depth_done > 0 ? "search" : "legal");
    }
    return 0;
}
