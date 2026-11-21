/* Bounded coarse C chess search: legal gen + Syzygy + 1-shot DB standings prior + PVS/alpha-beta + quiescence. */
#include "chess_search.h"
#include "chess_tablebase.h"
#include "laplace/os.h"
#include <limits.h>
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
    for (int s = 0; s < 64; s++) {
        int8_t p = b->sq[s];
        if (!p) continue;
        int ap = abs(p), c = p < 0 ? 1 : 0, sg = c ? -1 : 1;
        int f = s & 7, r = s >> 3;
        int rank_rel = c ? 7 - r : r;
        int psq = 0;
        if (ap == PAWN) {
            psq = rank_rel * 8 + (f >= 2 && f <= 5 ? 6 : 0);
        } else if (ap == KNIGHT || ap == BISHOP) {
            int center = 3 - (abs(f * 2 - 7) + abs(r * 2 - 7)) / 4;
            psq = center * 12;
        } else if (ap == KING) {
            if (c == 0) kw = s; else kb = s;
        }
        score += sg * (MAT[ap <= KING ? ap : PAWN] + psq);
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
    return (int)((double)pov * scale);
}

enum { TT_SIZE = 1 << 15 };
typedef struct { uint64_t key; int16_t depth, score; uint8_t flag; Mv best; } TTEntry;
enum { TT_EXACT = 1, TT_ALPHA = 2, TT_BETA = 3 };

typedef struct {
    LpChessSearchPolicy pol;
    LpTablebase        *tb;
    uint64_t            nodes;
    double              start_s;
    int                 aborted;
    TTEntry            *tt;
} SearchCtx;

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
    sort_moves(b, moves, NULL, nm, has_ttm ? &ttm : NULL);

    int orig_alpha = alpha;
    int best_score = -SCORE_INF;
    Mv best_mv = moves[0];

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
        if (ctx->aborted) return 0;
        if (score > best_score) { best_score = score; best_mv = moves[i]; }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    te->key = key;
    te->depth = (int16_t)depth;
    te->score = (int16_t)(best_score > 30000 ? 30000 : best_score < -30000 ? -30000 : best_score);
    te->flag = (best_score <= orig_alpha) ? TT_ALPHA : (best_score >= beta) ? TT_BETA : TT_EXACT;
    te->best = best_mv;
    return best_score;
}

int lp_chess_search(PGconn *pg, const char *fen, const char *variant,
                    const lp_id *witnesses, int nwitnesses, int scoped,
                    const LpChessSearchPolicy *policy_override, LpChessSearchResult *out){
    if (!fen || !*fen || !out) return 2;
    memset(out, 0, sizeof *out);
    double t0 = now();

    LpChessSearchPolicy pol = policy_override ? *policy_override : lp_chess_search_policy();
    if (pol.max_depth <= 0) pol.max_depth = 6;
    if (pol.eval_scale <= 0.0) pol.eval_scale = 1.0;

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
            lp_id c3[3] = { out->position_id, out->move_id, out->next_position_id };
            lp_id_compose(c3, 3, &out->claim_id);
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

    /* Up to 1 batched DB probe for root [P, M, P'] standing priors; zero DB calls during tree recursion. */
    int root_priors[LP_CHESS_MOVES];
    memset(root_priors, 0, sizeof root_priors);
    if (pg && pol.book_weight > 0) {
        lp_id *cids = malloc(sizeof(lp_id) * (size_t)nm);
        lp_rating *rats = malloc(sizeof(lp_rating) * (size_t)nm);
        int64_t *matches = calloc((size_t)nm, sizeof(int64_t));
        uint8_t *has = calloc((size_t)nm, 1);
        for (int i = 0; i < nm; i++) {
            lp_id mid, pid;
            Bd nb = root; lp_chess_make(&nb, &legal[i]);
            lp_chess_id_move(&root, &legal[i], &mid);
            lp_chess_id_position(&nb, &pid);
            lp_id c3[3] = { out->position_id, mid, pid };
            lp_id_compose(c3, 3, &cids[i]);
        }
        standings_of_ids(pg, cids, nm, witnesses, nwitnesses, scoped, rats, matches, has);
        out->db_probes = 1;
        for (int i = 0; i < nm; i++) {
            if (has[i] && matches[i] > 0) {
                double delta = (rats[i].rating - 1500.0) * 0.1 * ((double)pol.book_weight / 100.0);
                root_priors[i] = (int)delta;
            }
        }
        free(cids); free(rats); free(matches); free(has);
    }

    SearchCtx ctx = {
        .pol = pol, .tb = tb, .nodes = 0, .start_s = t0, .aborted = 0,
        .tt = calloc(TT_SIZE, sizeof(TTEntry))
    };

    Mv best_mv = legal[0];
    int best_score = 0;
    int depth_done = 0;

    sort_moves(&root, legal, root_priors, nm, NULL);

    for (int d = 1; d <= pol.max_depth; d++) {
        int alpha = -SCORE_INF, beta = SCORE_INF;
        int cur_best = -SCORE_INF;
        Mv cur_mv = legal[0];
        for (int i = 0; i < nm; i++) {
            Bd next = root;
            lp_chess_make(&next, &legal[i]);
            int sc;
            if (i == 0) sc = -pvs(&ctx, &next, d - 1, -beta, -alpha, 1);
            else {
                sc = -pvs(&ctx, &next, d - 1, -alpha - 1, -alpha, 1);
                if (!ctx.aborted && sc > alpha && sc < beta) sc = -pvs(&ctx, &next, d - 1, -beta, -alpha, 1);
            }
            if (ctx.aborted) break;
            sc += root_priors[i];
            if (sc > cur_best) { cur_best = sc; cur_mv = legal[i]; }
            if (sc > alpha) alpha = sc;
        }
        if (ctx.aborted) break;
        best_mv = cur_mv;
        best_score = cur_best;
        depth_done = d;
        /* Keep completed-depth best move first in next iteration */
        for (int i = 1; i < nm; i++) {
            if (memcmp(&legal[i], &best_mv, sizeof(Mv)) == 0) {
                Mv tmp = legal[0]; legal[0] = legal[i]; legal[i] = tmp;
                int tp = root_priors[0]; root_priors[0] = root_priors[i]; root_priors[i] = tp;
                break;
            }
        }
        if (abs(best_score) >= SCORE_MATE - 100) break;
    }

    free(ctx.tt);

    Bd nb = root; lp_chess_make(&nb, &best_mv);
    lp_chess_move_uci(&best_mv, &root, out->best_uci);
    lp_chess_san_of(&root, &best_mv, legal, nm, 1, out->best_san);
    lp_chess_id_move(&root, &best_mv, &out->move_id);
    lp_chess_id_position(&nb, &out->next_position_id);
    lp_id c3[3] = { out->position_id, out->move_id, out->next_position_id };
    lp_id_compose(c3, 3, &out->claim_id);
    out->score_cp = best_score;
    out->depth_reached = depth_done;
    out->nodes_searched = ctx.nodes;
    out->wall_us = (uint64_t)((now() - t0) * 1e6);
    snprintf(out->source, sizeof out->source, depth_done > 0 ? "search" : "legal");
    return 0;
}
