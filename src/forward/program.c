/* The forward program: one stateful program answers a turn of a session (Sequence: Forward, Sessions). */
#include "program_internal.h"
#include "ooda.h"
#include "laplace/highway.h"
#include <unicode/uchar.h>
#include <unicode/uscript.h>

static int owed(const State *st, const Bits *support, const Bits *open){
    double s = 0;
    for (int w = 0; w < MAXOCC / 64; w++) {
        uint64_t m = support->w[w] & open->w[w];
        while (m) {
            int b = __builtin_ctzll(m), idx = w * 64 + b;
            s += (idx < MAXOCC && st->role[idx] > 0.0) ? st->role[idx] : 1.0;
            m &= m - 1;
        }
    }
    return (int)(s * 1000 + 0.5);
}

static const char *RK[R_KINDS] = { "strand", "follows", "discourse", "reached", "containment", "a later layer" };

static int reading_of(const lp_highway *h, const lp_id *id, int *list, uint32_t *slot){
    for (size_t l = 0; l < h->nlists; l++) {
        int bank = 0;
        for (size_t k = 0; k < h->nbanks; k++) bank |= h->bank[k].list == &h->list[l];
        if (bank) continue;
        int64_t s = lp_highway_slot(h, &h->list[l], id);
        if (s >= 0) { *list = (int)l; *slot = (uint32_t)s; return 1; }
    }
    return 0;
}

/* ---- SCAN: best-first A* from centres across rated strands & highway crosswalks with admissible 4D angle4 heuristic */
static void scan(State *st, Field *fd, const lp_id *centre, int nc){
    const Firmware *fw = st->fw; const lp_highway *hw = lp_highway_map(NULL);
    lp_frontier *fr = lp_frontier_new(); Ids rn = { 0 };
    double goal_c[4] = { 0 }; int has_goal = 0;
    {
        double pc[MAXOCC * 4]; uint8_t hc[MAXOCC];
        int nq = st->nocc > 0 ? st->nocc : 0;
        if (nq > 0) {
            coords_of(st->pg, st->occ, nq, pc, hc);
            double wsum = 0.0;
            for (int i = 0; i < nq; i++) {
                if (!hc[i] || !st->composed[i]) continue;
                double w = st->salience[i] > 0.0 ? st->salience[i] : st->role[i];
                if (w > 0.0) { for (int d = 0; d < 4; d++) goal_c[d] += w * pc[4 * i + d]; wsum += w; }
            }
            if (wsum > 0.0) { for (int d = 0; d < 4; d++) goal_c[d] /= wsum; has_goal = 1; }
        }
    }

    for (int i = 0; i < nc; i++) {
        const Cell *cc = cell_find(fd, &centre[i]);
        uint32_t base_h = cc ? (uint32_t)(cc->layer + 1) : 0u;
        lp_frontier_reach(fr, &centre[i], NULL, NULL, 0.0, 0.0, base_h);
    }
    int rounds = 0; const lp_reached *x;
    while (rounds++ < fw->hops && !envelope_hit(st)) {
        st->work.hops++;
        lp_reached batch[16]; int n = 0;
        while (n < 16 && (x = lp_frontier_next(fr))) batch[n++] = *x;
        if (!n) break;
        lp_id ids[64]; int m = 0, who[64];
        for (int i = 0; i < n; i++) if ((int)batch[i].hops < fw->hops) { ids[m] = batch[i].id; who[m++] = i; }
        if (!m) break;

        if (hw) {
            for (int e = 0; e < m; e++) {
                const lp_reached *at = &batch[who[e]]; int list; uint32_t slot;
                if (!reading_of(hw, &at->id, &list, &slot)) continue;
                for (size_t b = 0; b < hw->nlists; b++) {
                    if ((int)b == list) continue;
                    int bank = 0; for (size_t k = 0; k < hw->nbanks; k++) bank |= hw->bank[k].list == &hw->list[b];
                    if (bank) continue;
                    const lp_edge *edges; size_t ne = lp_highway_edges(hw, hw->list[list].name, slot, hw->list[b].name, &edges);
                    for (size_t k = 0; k < ne; k++) {
                        const lp_tier0_record *rec = lp_highway_at(hw, &hw->list[b], edges[k].to);
                        if (!rec) continue;
                        double cost = at->cost + fw->lambda;
                        double h_est = 0.0;
                        if (has_goal) {
                            lp_coord coord; double pt[4];
                            memcpy(coord.m, rec->m, sizeof coord.m);
                            lp_coord_xyzm(&coord, pt);
                            double ang = lp_angle4(pt, goal_c);
                            if (isfinite(ang) && ang > 0.0) {
                                if (ang > 3.14159265358979323846) ang = 3.14159265358979323846;
                                h_est = fw->lambda * (ang / 3.14159265358979323846);
                            }
                        }
                        lp_frontier_reach(fr, &rec->id, &at->id, &at->id, cost, h_est, at->hops + 1);
                        const Cell *ca = cell_find(fd, &at->id);
                        Bits sup = ca ? ca->support : (Bits){ 0 };
                        Bits dsup = ca ? ca->direct_support : (Bits){ 0 };
                        int had_r = ca ? ca->has_r : 0; lp_rating cr = ca ? ca->r : lp_rating_stock();
                        Cell *cx = cell(fd, &rec->id); cx->routes[R_SCAN]++;
                        for (int w = 0; w < MAXOCC / 64; w++) { cx->support.w[w] |= sup.w[w]; cx->direct_support.w[w] |= dsup.w[w]; }
                        if (cost < cx->cost) { cx->cost = cost; if (!cx->has_r && had_r) { cx->r = cr; cx->has_r = 1; cx->via = at->id; cx->rel = at->id; } }
                    }
                }
            }
        }

        if (envelope_hit(st)) break;
        int hub[128], nh = 0;
        Hold *hh = holds_capped_scoped(st->pg, ids, m, fw->fan, 1, 1,
            st->witness_scope, st->nwitness_scope, st->witness_scoped, hub, &nh);
        st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nh;

        typedef struct { lp_id target, from, entity, via, rel; lp_rating r; double g; Bits sup, nsup, dsup; int flip, is_bridge, lat, tier; uint32_t hops; } ScanPush;
        ScanPush *pushes = malloc(sizeof(ScanPush) * (size_t)(nh ? nh : 1));
        lp_id *tids = malloc(sizeof(lp_id) * (size_t)(nh ? nh : 1));
        int npush = 0;
        for (int r = 0; r < nh; r++) {
            int e = hh[r].src; if (e < 0 || e >= m || hub[2 * e] || !hh[r].claim || !hh[r].stood) continue;
            if (lp_is_internal_metadata_path(st->c, hh[r].path, (size_t)hh[r].path_len)) continue;
            const lp_reached *at = &batch[who[e]];
            path_into(&rn, lp_path_of(hh[r].path, (size_t)hh[r].path_len));
            if (!head_read(fw, rn.v, rn.n)) { st->refused++; continue; }
            int other = lp_tuple_other(rn.v, rn.n, &at->id); if (other < 0) continue;
            if (rn.n == 2 && (lp_id_eq(&rn.v[0], &st->handle) || lp_id_eq(&rn.v[1], &st->handle) ||
                              lp_id_eq(&rn.v[0], &st->prompt) || lp_id_eq(&rn.v[1], &st->prompt))) continue;
            if (lp_id_eq(&rn.v[other], &st->prompt) || lp_id_eq(&rn.v[other], &st->handle) || (hw && lp_highway_bank_of(hw, &rn.v[other], NULL))) continue;
            int own_occ = 0;
            for (int oi = 0; oi < st->nocc && !own_occ; oi++) own_occ = lp_id_eq(&rn.v[other], &st->occ[oi]);
            if (own_occ) continue;
            lp_rating rt = hh[r].r;
            double sw = strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n); if (sw <= 0.0) continue;
            double cost = at->cost + lp_cost(&rt, fw->k, fw->lambda) - log(sw);
            const Cell *ca = cell_find(fd, &at->id);
            lp_id rel = rn.n >= 3 ? rn.v[1] : at->id;
            int flip = 0;
            for (size_t p = 1; p + 1 < rn.n && !flip; p++)
                for (int ni = 0; ni < fw->nnegate_relation && !flip; ni++) flip = lp_id_eq(&fw->id.negate_relation[ni], &rn.v[p]);
            int is_bridge = (rn.n >= 3 && is_lexical_sense_rel(st, &rel))
                ? (((size_t)other == rn.n - 1) ? 1 : (other == 0 ? -1 : 0))
                : 0;
            int lat = (ca && ca->lateral_hop) || (rn.n == 2) || is_lateral_hop_rel(st, &rel) || (is_bridge < 0);
            tids[npush] = rn.v[other];
            pushes[npush++] = (ScanPush){
                rn.v[other], at->id, hh[r].entity, at->id, rel, rt, cost,
                ca ? ca->support : (Bits){ 0 }, ca ? ca->neg_support : (Bits){ 0 }, ca ? ca->direct_support : (Bits){ 0 },
                flip, is_bridge, lat, hh[r].tier, at->hops + 1
            };
        }
        double *pts = (has_goal && npush > 0) ? malloc(sizeof(double) * 4 * (size_t)npush) : NULL;
        uint8_t *has = (has_goal && npush > 0) ? malloc((size_t)npush) : NULL;
        if (has_goal && npush > 0) coords_of(st->pg, tids, npush, pts, has);
        for (int i = 0; i < npush; i++) {
            const ScanPush *sp = &pushes[i];
            double h_est = 0.0;
            if (has_goal && has && has[i]) {
                double ang = lp_angle4(pts + 4 * (size_t)i, goal_c);
                if (isfinite(ang) && ang > 0.0) {
                    if (ang > 3.14159265358979323846) ang = 3.14159265358979323846;
                    h_est = fw->lambda * (ang / 3.14159265358979323846);
                }
            }
            lp_frontier_reach(fr, &sp->target, &sp->from, &sp->entity, sp->g, h_est, sp->hops);
            int before_cx = fd->n;
            Cell *cx = cell(fd, &sp->target); cx->routes[R_SCAN]++;
            int is_new_cx = (fd->n > before_cx);
            int first_claim_cx = (!cx->has_r && !cx->routes[R_CLAIM] && !cx->routes[R_LAYER]);
            if (is_new_cx) { cx->lateral_hop = sp->lat; cx->layer = (int)sp->hops; }
            else {
                if (first_claim_cx && sp->lat) cx->lateral_hop = 1;
                else if (!sp->lat) cx->lateral_hop = 0;
                if ((!sp->lat || cx->lateral_hop) && (int)sp->hops < cx->layer) cx->layer = (int)sp->hops;
            }
            if (sp->is_bridge > 0) cx->bridge = 1;
            else if (sp->is_bridge < 0) cx->bridge = 0;
            for (int w = 0; w < MAXOCC / 64; w++) {
                if (sp->flip) {
                    cx->support.w[w] |= sp->nsup.w[w];
                    cx->neg_support.w[w] |= sp->sup.w[w];
                } else {
                    if (is_new_cx || !sp->lat || cx->lateral_hop) cx->support.w[w] |= sp->sup.w[w];
                    if (!sp->lat) cx->direct_support.w[w] |= sp->dsup.w[w];
                    cx->neg_support.w[w] |= sp->nsup.w[w];
                }
            }
            if (sp->g < cx->cost) { cx->cost = sp->g; if (!cx->has_r) { cx->r = sp->r; cx->has_r = 1; cx->via = sp->via; cx->rel = sp->rel; } }
        }
        free(pts); free(has); free(tids); free(pushes);
        holds_free(hh, nh);
    }
    lp_frontier_free(fr); lp_vec_free(&rn);
}

/* ---- proposals & election ---- */
enum { P_FOLLOW, P_CHAIN, P_SHAPE };
typedef struct { lp_id id; int kind, grounds, cont; double conf, hub, force; long times; int occ; int agree; long cooc, walk; Bits by[3]; double shape; int heads, tier, inlang; } Prop;
enum { CH_STRAND, CH_WALK, CH_BESIDE };

static int cell_matches_prompt_lang(const State *st, const Field *fd, const Cell *x){
    if (!st || !x || st->nlang <= 0 || !st->rd || st->lexical.np <= 0) return 0;
    const Lexicalization *lx = &st->lexical;
    if (lx->lang < 0 || lx->lang >= lx->np) return 0;
    /* Only inspect cells that are themselves lexicalization bridges or reached directly from a bridge cell;
     * avoids firing hundreds of uncached reader_parts SQL round trips across arbitrary prose/claim entities. */
    const Cell *via_c = (fd && !lp_id_eq(&x->via, &(lp_id){{0}})) ? cell_find(fd, &x->via) : NULL;
    const lp_id *probe[2] = { x->bridge ? &x->id : NULL, (via_c && via_c->bridge) ? &x->via : NULL };
    lp_id parts[16];
    for (int p = 0; p < 2; p++) {
        if (!probe[p]) continue;
        size_t np = reader_parts(st->rd, probe[p], parts, 16);
        if ((int)np == lx->np) {
            for (int y = 0; y < st->nlang; y++) {
                if (lp_id_eq(&parts[lx->lang], &st->lang[y])) return 1;
            }
        }
    }
    return 0;
}

static void weigh_prop(const State *st, const Field *fd, const lp_highway *hw, const Bits *content_mask, int multi_anc_prompt, Prop *p){
    (void)hw; (void)multi_anc_prompt;
    const Cell *x = cell_find(fd, &p->id);
    memset(p->by, 0, sizeof p->by); p->cooc = p->walk = 0; p->heads = x ? x->nheads : 0;
    p->tier = x ? x->tier : 0;
    p->force = x ? x->force : 0.0;
    p->inlang = cell_matches_prompt_lang(st, fd, x);
    int atom = lp_tier0_codepoint(T0, &p->id) >= 0;
    int is_neg_word = 0;
    for (int ni = 0; ni < st->fw->nnegate && !is_neg_word; ni++) is_neg_word = lp_id_eq(&p->id, &st->fw->id.negate[ni]);
    Bits contra; memset(&contra, 0, sizeof contra);
    for (int j = 0; j < st->npos; j++) {
        if (lp_id_eq(&st->occ[j], &p->id)) continue;
        int is_content_j = (j >= st->nocc) || !content_mask || ((content_mask->w[j >> 6] >> (j & 63)) & 1);
        int v = is_content_j ? visits(st, &p->id, j) : 0;
        int b = is_content_j ? beside(st, j, &p->id) : 0;
        if (v >= 2) p->walk += v;
        p->cooc += b;
        if (atom || j >= st->nocc || !((st->open.w[j >> 6] >> (j & 63)) & 1)) continue;
        int is_neg_occ = st->polarity[j] < 0;
        int has_pos_strand = x && ((x->support.w[j >> 6] >> (j & 63)) & 1);
        int has_neg_strand = x && ((x->neg_support.w[j >> 6] >> (j & 63)) & 1);
        if (has_pos_strand) bit_set(&p->by[CH_STRAND], j);
        if (has_neg_strand) bit_set(&contra, j);
        if (is_content_j && v >= 2) {
            if (is_neg_occ && !has_pos_strand && !is_neg_word) bit_set(&contra, j);
            else if (!has_neg_strand) bit_set(&p->by[CH_WALK], j);
        }
        if (is_content_j && beside_grounds(st, j, &p->id)) {
            if (is_neg_occ && !has_pos_strand && !is_neg_word) bit_set(&contra, j);
            else if (!has_neg_strand) bit_set(&p->by[CH_BESIDE], j);
        }
    }
    Bits u; memset(&u, 0, sizeof u); p->agree = 0;
    for (int c = 0; c < 3; c++) {
        int any = 0;
        for (int w = 0; w < MAXOCC / 64; w++) { u.w[w] |= p->by[c].w[w]; any |= p->by[c].w[w] != 0; }
        p->agree += any;
    }
    int pos_g = 0;
    if (x) {
        double gsum = 0.0;
        for (int w = 0; w < MAXOCC / 64; w++) {
            uint64_t m = u.w[w] & st->open.w[w];
            while (m) {
                int b = __builtin_ctzll(m), idx = w * 64 + b;
                double rw = (idx < MAXOCC && st->role[idx] > 0.0) ? st->role[idx] : 1.0;
                int direct = !x->lateral_hop && (
                             ((x->direct_support.w[w] >> b) & 1) ||
                             ((p->by[CH_WALK].w[w] >> b) & 1) ||
                             ((p->by[CH_BESIDE].w[w] >> b) & 1));
                gsum += direct ? rw : (0.25 * rw);
                m &= m - 1;
            }
        }
        double hop_decay = 1.0;
        if (x->layer > 1 && st->fw->lambda > 0.0) {
            double step_keep = 1.0 - 4.0 * st->fw->lambda;
            if (step_keep < 0.5) step_keep = 0.5;
            hop_decay = pow(step_keep, (double)(x->layer - 1));
        }
        pos_g = (int)(gsum * hop_decay * 1000.0 + 0.5);
    } else {
        pos_g = owed(st, &u, &st->open);
    }
    int neg_g = owed(st, &contra, &st->open);
    p->grounds = pos_g > neg_g ? pos_g - neg_g : 0;

    int trusted_multi = x && (x->has_r || x->routes[R_CLAIM] > 0 || x->routes[R_SCAN] > 0 || x->routes[R_LAYER] > 0 ||
                              x->routes[R_DISCOURSE] > 0 || p->agree >= 2 || bit_count_and(&x->support, &st->open) >= 2);
    if (st->fw->multi_tier && p->tier >= 2 && p->grounds > 0 && trusted_multi)
        p->cont += 1;
}

static _Thread_local const Firmware *ELECT;
static _Thread_local const Field *CAND_FD;

static int key_cmp(const Prop *x, const Prop *y, int k){
    switch (k) {
    case FW_E_GROUNDS:    return x->grounds != y->grounds ? (x->grounds > y->grounds ? -1 : 1) : 0;
    case FW_E_CONTINUITY: return x->cont != y->cont ? (x->cont > y->cont ? -1 : 1) : 0;
    case FW_E_AGREE:      return x->agree != y->agree ? (x->agree > y->agree ? -1 : 1) : 0;
    case FW_E_COOCCUR:    return x->cooc != y->cooc ? (x->cooc > y->cooc ? -1 : 1) : 0;
    case FW_E_WALKS:      return x->walk != y->walk ? (x->walk > y->walk ? -1 : 1) : 0;
    case FW_E_SHAPE:      return x->shape != y->shape ? (x->shape < y->shape ? -1 : 1) : 0;
    case FW_E_HEADS:      return x->heads != y->heads ? (x->heads > y->heads ? -1 : 1) : 0;
    case FW_E_CONFIDENCE: return x->conf != y->conf ? (x->conf > y->conf ? -1 : 1) : 0;
    case FW_E_SHARED: {
        int bx = x->hub > 64.0 ? (int)log2(x->hub) : 0;
        int by = y->hub > 64.0 ? (int)log2(y->hub) : 0;
        return bx != by ? (bx < by ? -1 : 1) : 0;
    }
    }
    return 0;
}
static int elect(const void *a, const void *b){
    const Prop *x = a, *y = b;
    for (int i = 0; i < ELECT->nelect; i++) {
        int c = key_cmp(x, y, ELECT->elect[i]);
        if (c) return c;
        if (ELECT->elect[i] == FW_E_GROUNDS && x->inlang != y->inlang)
            return x->inlang > y->inlang ? -1 : 1;
    }
    if (x->inlang != y->inlang) return x->inlang > y->inlang ? -1 : 1;
    if (x->force != y->force) return x->force > y->force ? -1 : 1;
    return memcmp(&x->id, &y->id, 16);
}
static int cannot_tell(const Prop *x, const Prop *y, const Firmware *fw){
    double temp = fw->top_within;
    for (int i = 0; i < fw->nelect; i++) {
        int k = fw->elect[i];
        if (k == FW_E_WALKS) { if (fabs((double)x->walk - (double)y->walk) > fw->sure * sqrt((double)x->walk + (double)y->walk)) return 0; continue; }
        if (k == FW_E_CONFIDENCE) { if (fabs(x->conf - y->conf) > temp) return 0; continue; }
        if (key_cmp(x, y, k)) return 0;
    }
    return 1;
}

/* ---- ORIENT ---- */
typedef struct { int cell, occ, list; uint32_t slot; } Cand;
typedef struct { int ncand, choice, level, capped; double score; } Bind;
typedef struct { const lp_highway *h; Cand *cand; int nc, *first; } Readings;

static int mapped(const lp_highway *h, const Cand *a, const Cand *b){
    const lp_edge *e; size_t n = lp_highway_edges(h, h->list[a->list].name, a->slot, h->list[b->list].name, &e);
    for (size_t k = 0; k < n; k++) if (e[k].to == b->slot) return 1;
    return 0;
}
static int holds_with(const lp_highway *h, const Cand *a, const Cand *b){
    return (a->list == b->list && a->slot == b->slot) || mapped(h, a, b) || mapped(h, b, a);
}
static double reading_compat(const State *st, const Field *fd, const Readings *R, const Cand *a, const Cand *b){
    if (holds_with(R->h, a, b)) return 1.0;
    const Cell *ca = &fd->c[a->cell], *cb = &fd->c[b->cell];
    if (((ca->support.w[b->occ >> 6] >> (b->occ & 63)) & 1) || ((cb->support.w[a->occ >> 6] >> (a->occ & 63)) & 1)) return 0.85;
    int v_ab = visits(st, &cb->id, a->occ) + visits(st, &ca->id, b->occ);
    int c_ab = beside_grounds(st, a->occ, &cb->id) + beside_grounds(st, b->occ, &ca->id);
    if (v_ab >= 2 && c_ab > 0) return 0.75;
    if (v_ab >= 2) return 0.60;
    int shared_other = 0;
    for (int w = 0; w < MAXOCC / 64 && !shared_other; w++) {
        uint64_t m = ca->support.w[w] & cb->support.w[w];
        if (w == (a->occ >> 6)) m &= ~(1ull << (a->occ & 63));
        if (w == (b->occ >> 6)) m &= ~(1ull << (b->occ & 63));
        if (m) shared_other = 1;
    }
    return (c_ab > 0 || shared_other) ? 0.45 : 0.0;
}
static int cand_by_id(const void *a, const void *b){
    return memcmp(&CAND_FD->c[((const Cand *)a)->cell].id, &CAND_FD->c[((const Cand *)b)->cell].id, 16);
}
static void reach_of(const State *st, const Cell *x, int i, int upto, double *walks, int *strands){
    *walks = 0; *strands = 0;
    for (int j = 0; j < upto; j++) {
        if (j == i || !st->composed[j] || st->role[j] <= 0.0) continue;
        *walks += st->role[j] * visits(st, &x->id, j);
        *strands += (int)((x->support.w[j >> 6] >> (j & 63)) & 1);
    }
}
static void readings_build(State *st, Field *fd, Bind *bind, Readings *R){
    R->h = lp_highway_map(NULL); R->cand = NULL; R->nc = 0; R->first = calloc((size_t)st->nocc + 1, sizeof(int));
    int cc = 0;
    for (int i = 0; i < st->nocc; i++) { memset(&bind[i], 0, sizeof bind[i]); bind[i].choice = -1; }
    if (!R->h) return;
    for (int i = 0; i < st->nocc; i++) {
        R->first[i] = R->nc;
        if (!st->composed[i] || st->role[i] <= 0.0) continue;
        for (int z = 0; z < fd->n; z++) {
            const Cell *x = &fd->c[z]; int list; uint32_t slot;
            if ((!x->routes[R_CLAIM] && !x->routes[R_LAYER]) || !((x->support.w[i >> 6] >> (i & 63)) & 1) || !reading_of(R->h, &x->id, &list, &slot)) continue;
            if (lp_highway_bank_of(R->h, &x->rel, NULL)) continue;
            size_t c_ = (size_t)cc; lp_reserve((void **)&R->cand, &c_, (size_t)R->nc + 1, sizeof(Cand)); cc = (int)c_;
            R->cand[R->nc++] = (Cand){ z, i, list, slot };
        }
        bind[i].ncand = R->nc - R->first[i];
        CAND_FD = fd;
        if (bind[i].ncand > 1) qsort(R->cand + R->first[i], (size_t)bind[i].ncand, sizeof(Cand), cand_by_id);
    }
    R->first[st->nocc] = R->nc;
}
static int reading_for(const State *st, const Field *fd, const Bind *bind, const Readings *R, int i, int upto, double *score, int *level){
    double best = 0.0, bw = 0.0; int pick = -1, lvl = 0, bs = 0;
    for (int a = R->first[i]; a < R->first[i] + bind[i].ncand; a++) {
        double s = 0.0;
        for (int j = 0; j < upto; j++) {
            if (j == i || !bind[j].ncand) continue;
            double hold = 0.0;
            if (bind[j].choice >= 0) hold = reading_compat(st, fd, R, &R->cand[a], &R->cand[bind[j].choice]);
            else for (int c = R->first[j]; c < R->first[j] + bind[j].ncand; c++) {
                double h = reading_compat(st, fd, R, &R->cand[a], &R->cand[c]);
                if (h > hold) { hold = h; if (hold >= 1.0) break; }
            }
            s += hold * st->role[j];
        }
        double w; int sn; reach_of(st, &fd->c[R->cand[a].cell], i, upto, &w, &sn);
        if (s > best + 1e-9) { best = s; pick = a; lvl = 0; bw = w; bs = sn; }
        else if (fabs(s - best) <= 1e-9 && best > 0.0) {
            double tol = st->fw->sure * sqrt(w + bw);
            if (w > bw + tol || (!(bw > w + tol) && sn > bs)) { pick = a; lvl = 0; bw = w; bs = sn; }
            else if (!(bw > w + tol) && sn == bs) lvl = 1;
        }
    }
    *score = best; *level = lvl; return pick;
}
static int orient(State *st, Field *fd, Bind *bind, const Readings *R, int *nambig){
    *nambig = 0; if (!R->h) return 0;
    for (int round = 0; round < 8; round++) {
        int changed = 0;
        for (int i = 0; i < st->nocc; i++) {
            if (!bind[i].ncand) continue;
            double sc; int lv; int pick = reading_for(st, fd, bind, R, i, st->nocc, &sc, &lv);
            if (pick != bind[i].choice) { bind[i].choice = pick; changed = 1; }
            bind[i].score = sc; bind[i].level = lv;
        }
        if (!changed) break;
    }
    int bound = 0;
    for (int i = 0; i < st->nocc; i++) {
        int owes = (st->open.w[i >> 6] >> (i & 63)) & 1;
        if (bind[i].choice >= 0) {
            bound += owes;
            if (bind[i].level && owes) (*nambig)++;
            bind[i].choice = R->cand[bind[i].choice].cell;
        }
    }
    return bound;
}

/* ---- session helpers ---- */
static Ref session_handle(const char *user, const char *session, Ref *who){
    *who = text_ref(CTX[0], (const uint8_t *)user, strlen(user));
    Ref sname = text_ref(CTX[0], (const uint8_t *)session, strlen(session));
    Ref hp[2] = { *who, sname }; hp[0].said = hp[1].said = 0;
    return said_tuple(compose(hp, 2, (uint8_t)((who->tier > sname.tier ? who->tier : sname.tier) + 1)));
}
static Claim *session_turns(PGconn *pg, const Firmware *fw, const lp_id *handle, int *nturn, int *ordinal){
    int have[3] = { 2, 0, 0 }, n = 0, cap = 0, kept = 0;
    lp_id part[3] = { *handle, *handle, *handle };
    Claim *tc = claims_like(pg, part, have, fw->fan, fw->k, &n, &cap);
    positions_of(pg, tc, n); lp_sort(tc, (size_t)n, sizeof(Claim), claim_by_position);
    *ordinal = 1;
    for (int i = 0; i < n; i++) {
        if (tc[i].np != 2 || memcmp(&tc[i].part[0], handle, 16)) continue;
        if (tc[i].position >= *ordinal) *ordinal = tc[i].position + 1;
        tc[kept++] = tc[i];
    }
    *nturn = kept; return tc;
}
static int turn_witness(PGconn *pg, const char *conninfo, Ref who, Ref by, Ref handle, Ref pr, Ref resp, int ordinal, LoadStats *ls, lp_id *turn_id){
    Ref tp[2] = { pr, resp }; tp[0].said = tp[1].said = 0;
    Ref turn = compose(tp, 2, ref_above(tp, 2));
    const lp_trust_class *uc = lp_trust_class_named("UserPromptContent"), *rc = lp_trust_class_named("ResponseContent");
    File f[2]; memset(f, 0, sizeof f);
    Ref p1[2] = { handle, turn }; p1[0].said = LP_SAID_TUPLE; p1[1].said = 0;
    Ref c1 = said_claim(compose(p1, 2, ref_above(p1, 2)));
    c1.outcome = (uint8_t)lp_outcome_of(1.0, &c1.spare); c1.position = (uint32_t)ordinal;
    Ref p2[2] = { resp, pr }; p2[0].said = p2[1].said = 0;
    Ref c2 = said_claim(compose(p2, 2, ref_above(p2, 2)));
    c2.outcome = (uint8_t)lp_outcome_of(1.0, &c2.spare);
    Ref u_tr[2] = { said_metadata(who), c1 }; Ref u_trunk = compose(u_tr, 2, ref_above(u_tr, 2));
    Ref r_tr[2] = { said_metadata(by), c2 }; Ref r_trunk = compose(r_tr, 2, ref_above(r_tr, 2));
    f[0].path = "the prompt"; f[0].witness = who; f[0].lineage = who; f[0].has_lineage = 1; f[0].trust = uc ? uc->prior : 0.3; f[0].trunk = u_trunk; f[0].file = u_trunk; f[0].has_file = 1;
    Event e1 = { .claim = c1.id, .witnessed = c1.id, .score = 1.0f, .enter_rating = 1500.0f, .position = (uint32_t)ordinal, .kind = EV_CLAIM }; ev_push(&f[0].ev, &e1);
    f[1].path = "the response"; f[1].witness = by; f[1].lineage = by; f[1].has_lineage = 1; f[1].trust = rc ? rc->prior : 0.2; f[1].trunk = r_trunk; f[1].file = r_trunk; f[1].has_file = 1;
    Event e2 = { .claim = c2.id, .witnessed = c2.id, .score = 1.0f, .enter_rating = 1500.0f, .kind = EV_CLAIM }; ev_push(&f[1].ev, &e2);
    Fold *fold = fold_new(); fold_events(fold, f, 2);
    LoadWitness lw[2] = { { .id = u_trunk.id, .lineage = who.id, .trust = f[0].trust, .has_lineage = 1 }, { .id = r_trunk.id, .lineage = by.id, .trust = f[1].trust, .has_lineage = 1 } };
    int rc_ = (pg && lp_stage_publish_fast(pg, fold, lw, 2, f, 2, ls) == 0) ? 0
        : (load(conninfo, 1, f, 2, ls) || load_register(conninfo, 1, lw, 2, f, 2) || merge(conninfo, 1, ls));
    fold_free(fold);
    free(f[0].ev.e); free(f[1].ev.e);
    if (rc_) return 1;
    *turn_id = turn.id; return 0;
}

/* ---- receipt helpers ---- */
static void receipt_id(FILE *R, const char *key, const lp_id *id){ char h[33]; lp_id_hex(id, h); fprintf(R, ",\"%s\":\"%s\"", key, h); }
static void receipt_end(FILE *R){ fputs("}\n", R); fflush(R); }
static void receipt_resolve(FILE *R, const lp_id *firmware_id, const lp_id *program_id, const lp_id *prompt, const lp_id *response, const char *user, const char *session, int ordinal, int nturn){
    if (!R) return;
    fputs("{\"event\":\"resolve\"", R);
    if (firmware_id) receipt_id(R, "firmware_id", firmware_id);
    if (program_id) receipt_id(R, "program_id", program_id);
    receipt_id(R, "prompt", prompt); if (response) receipt_id(R, "response", response);
    fputs(",\"user\":", R); lp_fput_json(R, user); fputs(",\"session\":", R); lp_fput_json(R, session);
    fprintf(R, ",\"ordinal\":%d,\"turns_before\":%d", ordinal, nturn); receipt_end(R);
}
static void receipt_step(FILE *R, int step, const char *kind, const lp_id *id, const char *text, double grounds, int continuity, double confidence){
    if (!R) return;
    fprintf(R, "{\"event\":\"step\",\"step\":%d,\"kind\":", step); lp_fput_json(R, kind); receipt_id(R, "id", id);
    fputs(",\"text\":", R); lp_fput_json(R, text);
    fprintf(R, ",\"grounds\":%.6g,\"continuity\":%d,\"confidence\":%.6g", grounds, continuity, confidence); receipt_end(R);
}
static void receipt_realize(FILE *R, const lp_id *firmware_id, const lp_id *program_id, const lp_id *decision_id, const char *text, const char *disposition){
    if (!R) return;
    fputs("{\"event\":\"realize\",\"text\":", R); lp_fput_json(R, text); fputs(",\"disposition\":", R); lp_fput_json(R, disposition);
    if (firmware_id) receipt_id(R, "firmware_id", firmware_id);
    if (program_id) receipt_id(R, "program_id", program_id);
    if (decision_id) receipt_id(R, "decision_id", decision_id);
    receipt_end(R);
}
static void receipt_witnessed(FILE *R, const lp_id *turn, const lp_id *response, int ordinal, unsigned long long entities){
    if (!R) return;
    fputs("{\"event\":\"witness\",\"witnessed\":true", R); receipt_id(R, "turn", turn); receipt_id(R, "response", response);
    fprintf(R, ",\"ordinal\":%d,\"entities\":%llu", ordinal, entities); receipt_end(R);
}
static void receipt_unwitnessed(FILE *R, const lp_id *turn, int held, const char *reason){
    if (!R) return;
    fputs("{\"event\":\"witness\",\"witnessed\":false", R);
    if (held) fprintf(R, ",\"held\":%d", held > 0 ? held : 0); else fputs(",\"held\":null", R);
    if (turn) receipt_id(R, "turn", turn); else fputs(",\"turn\":null", R);
    fputs(",\"reason\":", R); lp_fput_json(R, reason); receipt_end(R);
}
static lp_id compute_program_id(lp_text *c, const lp_id *prompt, const lp_id *firmware_id, const lp_id *handle, int ordinal){
    char ord[16]; int ol = snprintf(ord, sizeof ord, "%d", ordinal);
    lp_id ord_id = lp_text_decompose(c, (const uint8_t *)ord, (size_t)ol, NULL, NULL).id;
    const lp_id parts[4] = { *prompt, *firmware_id, *handle, ord_id };
    lp_id out; lp_id_compose(parts, 4, &out); return out;
}

static uint8_t *said_text(const char *arg, int files, const char *what, size_t *n){
    uint8_t *text;
    if (!files) { *n = strlen(arg); text = malloc(*n + 1); if (text) memcpy(text, arg, *n + 1); }
    else {
        FILE *f = fopen(arg, "rb"); if (!f) { fprintf(stderr, "said: the %s's file %s: %s\n", what, arg, strerror(errno)); return NULL; }
        size_t cap = 1 << 16; text = malloc(cap); *n = 0; size_t k;
        while (text && (k = fread(text + *n, 1, cap - *n, f)) > 0) { *n += k; if (*n == cap) { cap *= 2; text = xrealloc(text, cap); } }
        fclose(f);
    }
    if (!text) { perror("said"); return NULL; }
    if (!*n) { fprintf(stderr, "said: the %s is empty: no turn\n", what); free(text); return NULL; }
    for (size_t i = 0; i < *n; ) { uint32_t cp; if (!text[i] || !lp_utf8_next(text, *n, &i, &cp)) { fprintf(stderr, "said: the %s is not UTF-8 text\n", what); free(text); return NULL; } }
    return text;
}

int cmd_said(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL, *user = getenv("USER"), *session = NULL, *by = "Laplace", *receipt = NULL; int files = 0;
    int a = opts(argc, argv, (const Opt[]){ { .flag = "-d", .kind = 's', .at = &conninfo }, { .flag = "--firmware", .kind = 's', .at = &fwp }, { .flag = "--as", .kind = 's', .at = &user }, { .flag = "--session", .kind = 's', .at = &session },
                                            { .flag = "--by", .kind = 's', .at = &by }, { .flag = "--files", .kind = 'b', .at = &files }, { .flag = "--receipt", .kind = 's', .at = &receipt }, { 0 } });
    if (a + 2 != argc) { fprintf(stderr, "usage: laplace said [-d conninfo] [--firmware FILE] [--as USER] [--session NAME] [--by NAME] [--files] [--receipt FILE] prompt response\n"); return 2; }
    if (!user || !*user) user = "user"; if (!session) session = "session";
    if (!*session || !*by) { fprintf(stderr, "said: a session and a responder have names\n"); return 2; }
    FILE *rcp = receipt ? fopen(receipt, "wb") : NULL;
    if (receipt && !rcp) { fprintf(stderr, "the receipt %s: %s\n", receipt, strerror(errno)); return 1; }
    double T = now();
    size_t np, nr; uint8_t *prompt = said_text(argv[a], files, "prompt", &np); if (!prompt) { if (rcp) fclose(rcp); return 1; }
    uint8_t *response = said_text(argv[a + 1], files, "response", &nr); if (!response) { free(prompt); if (rcp) fclose(rcp); return 1; }
    Firmware fw = firmware_for(fwp, FW_PULL);
    tier0_open(NULL); firmware_ids(&fw); table_init(); ctx_open(1);
    Ref pr = text_ref(CTX[0], prompt, np), resp = text_ref(CTX[0], response, nr);
    Ref who, handle = session_handle(user, session, &who), speaker = text_ref(CTX[0], (const uint8_t *)by, strlen(by));
    Ref tp[2] = { pr, resp }; tp[0].said = tp[1].said = 0; Ref turn = compose(tp, 2, ref_above(tp, 2));
    PGconn *pg = db_read(conninfo); int nturn, ordinal, held = 0;
    Claim *tc = session_turns(pg, &fw, &handle.id, &nturn, &ordinal);
    for (int i = 0; i < nturn && !held; i++) if (lp_id_eq(&tc[i].part[1], &turn.id)) held = tc[i].position ? tc[i].position : -1;
    free(tc); PQfinish(pg);
    lp_id program_id = compute_program_id(CTX[0], &pr.id, &fw.id.image_id, &handle.id, held > 0 ? held : held ? 0 : ordinal);
    char ti[33], pi[33], ri[33]; lp_id_hex(&turn.id, ti); lp_id_hex(&pr.id, pi); lp_id_hex(&resp.id, ri);
    printf("RESOLVE    prompt %s, tier %d; response %s, tier %d; session \"%s\" of %s, %d turn%s before it\n", pi, pr.tier, ri, resp.tier, session, user, nturn, nturn == 1 ? "" : "s");
    receipt_resolve(rcp, &fw.id.image_id, &program_id, &pr.id, &resp.id, user, session, held > 0 ? held : held ? 0 : ordinal, nturn);
    if (held) {
        if (held > 0) printf("WITNESS    turn %s is held by the session, at %d: nothing is witnessed again\n", ti, held);
        else printf("WITNESS    turn %s is held by the session: nothing is witnessed again\n", ti);
        receipt_unwitnessed(rcp, &turn.id, held, "the turn is held by the session: nothing is witnessed again");
    } else {
        LoadStats ls = { 0 }; lp_id id;
        if (turn_witness(NULL, conninfo, who, speaker, handle, pr, resp, ordinal, &ls, &id)) { if (rcp) fclose(rcp); free(prompt); free(response); return 1; }
        printf("WITNESS    turn %s, at %d in the session: %llu entities new, the user's [session, turn] and %s's [response, prompt]\n", ti, ordinal, (unsigned long long)ls.ent_rows, by);
        receipt_witnessed(rcp, &id, &resp.id, ordinal, (unsigned long long)ls.ent_rows);
    }
    printf("\ntotal %.1f ms\n", (now() - T) * 1000);
    if (rcp) fclose(rcp); free(prompt); free(response); return 0;
}

static int is_unspaced_cp(UChar32 cp){
    if (u_hasBinaryProperty(cp, UCHAR_IDEOGRAPHIC)) return 1;
    int eaw = u_getIntPropertyValue(cp, UCHAR_EAST_ASIAN_WIDTH);
    if (u_ispunct(cp) && (eaw == U_EA_WIDE || eaw == U_EA_FULLWIDTH)) return 1;
    UErrorCode ec = U_ZERO_ERROR;
    UScriptCode sc = uscript_getScript(cp, &ec);
    if (U_FAILURE(ec)) return 0;
    return sc == USCRIPT_HAN || sc == USCRIPT_HIRAGANA || sc == USCRIPT_KATAKANA ||
           sc == USCRIPT_THAI || sc == USCRIPT_LAO || sc == USCRIPT_KHMER || sc == USCRIPT_MYANMAR;
}

static int utf8_last_cp(const char *s, size_t n, UChar32 *out){
    size_t pos = 0;
    uint32_t last = 0;
    int any = 0;
    while (pos < n) {
        uint32_t cp = 0;
        if (!lp_utf8_next((const uint8_t *)s, n, &pos, &cp)) break;
        last = cp;
        any = 1;
    }
    if (any && out) *out = (UChar32)last;
    return any;
}

static int utf8_first_cp(const char *s, size_t n, UChar32 *out){
    size_t pos = 0;
    uint32_t cp = 0;
    if (!n || !lp_utf8_next((const uint8_t *)s, n, &pos, &cp)) return 0;
    if (out) *out = (UChar32)cp;
    return 1;
}

void lp_program_append_out(char **out, size_t *outn, const char *tx, int is_follow, int combine_clauses){
    size_t tl = tx ? strlen(tx) : 0;
    if (!tl) return;
    int need_dot = 0, need_space = 0;
    if (!is_follow && *outn && *out) {
        UChar32 prev_cp = 0, next_cp = 0;
        int has_prev = utf8_last_cp(*out, *outn, &prev_cp);
        int has_next = utf8_first_cp(tx, tl, &next_cp);
        if (has_prev && has_next) {
            int prev_ws = u_isUWhiteSpace(prev_cp) || prev_cp == '\n' || prev_cp == '\r';
            int next_ws = u_isUWhiteSpace(next_cp) || next_cp == '\n' || next_cp == '\r';
            int prev_term = u_hasBinaryProperty(prev_cp, UCHAR_TERMINAL_PUNCTUATION) || prev_cp == '\n';
            int next_punct = u_ispunct(next_cp);
            int both_unspaced = is_unspaced_cp(prev_cp) && is_unspaced_cp(next_cp);
            if (combine_clauses && !prev_term && !prev_ws && !next_punct && !both_unspaced)
                need_dot = 1;
            if (!prev_ws && !next_ws && !next_punct && !both_unspaced)
                need_space = 1;
        }
    }
    *out = xrealloc(*out, *outn + tl + (size_t)need_dot + (size_t)need_space + 1);
    if (need_dot) (*out)[(*outn)++] = '.';
    if (need_space) (*out)[(*outn)++] = ' ';
    memcpy(*out + *outn, tx, tl);
    *outn += tl;
    (*out)[*outn] = 0;
}

static size_t utf8_decode_lower_trimmed(const uint8_t *s, size_t n, UChar32 *buf, size_t cap){
    size_t pos = 0, len = 0;
    while (pos < n && len < cap) {
        uint32_t cp = 0;
        if (!lp_utf8_next(s, n, &pos, &cp)) break;
        buf[len++] = u_tolower((UChar32)cp);
    }
    while (len > 0 && (u_ispunct(buf[len - 1]) || u_isUWhiteSpace(buf[len - 1]))) len--;
    return len;
}

int lp_program_is_prompt_echo(const uint8_t *prompt_text, size_t prompt_len, const char *tx, size_t tl, int is_follow){
    if (!prompt_text || !prompt_len || !tx || !tl || prompt_len >= 1024 || tl >= 1024) return 0;
    UChar32 pcp[512], tcp[512];
    size_t pl = utf8_decode_lower_trimmed(prompt_text, prompt_len, pcp, 512);
    size_t ctl = utf8_decode_lower_trimmed((const uint8_t *)tx, tl, tcp, 512);
    if (!pl || !ctl) return 0;

    if (ctl >= pl) {
        int prefix_eq = 1;
        for (size_t k = 0; k < pl; k++) {
            if (tcp[k] != pcp[k]) { prefix_eq = 0; break; }
        }
        if (prefix_eq && (ctl == pl || u_isUWhiteSpace(tcp[pl]) || u_ispunct(tcp[pl]) || is_unspaced_cp(tcp[pl])))
            return 1;
    }

    if (!is_follow && ctl < pl && (ctl >= 3 || (ctl >= 1 && is_unspaced_cp(tcp[0])))) {
        for (size_t pos = 0; pos + ctl <= pl; pos++) {
            int sub_eq = 1;
            for (size_t k = 0; k < ctl; k++) {
                if (tcp[k] != pcp[pos + k]) { sub_eq = 0; break; }
            }
            if (!sub_eq) continue;
            int prev_alnum = (pos > 0 && !is_unspaced_cp(pcp[pos - 1]) && u_isalnum(pcp[pos - 1]));
            int next_alnum = (pos + ctl < pl && !is_unspaced_cp(pcp[pos + ctl]) && u_isalnum(pcp[pos + ctl]));
            if (!prev_alnum && !next_alnum) return 1;
        }
    }
    return 0;
}

int lp_program_should_stop_enough(int sel_is_follow, int combine_clauses, int rem, int owed0, double enough, int open_anchors){
    if (sel_is_follow) return 0;
    if (combine_clauses && open_anchors > 0) return 0;
    return rem <= (int)(enough * owed0 + 0.5);
}

/* Format a bare multi-clause structured claim [via, rel, val] when val is a compact entity (not already a sentence/gloss). */
int lp_program_format_claim_clause(const char *via_tx, const char *rel_tx, const char *val_tx, char *buf, size_t cap){
    if (!via_tx || !*via_tx || !rel_tx || !*rel_tx || !val_tx || !*val_tx || !buf || cap < 8) return 0;
    size_t vl = strlen(val_tx), sl = strlen(via_tx), rl = strlen(rel_tx);
    if (vl > 64 || sl > 64 || rl > 64) return 0;
    if (strstr(val_tx, via_tx) != NULL) return 0;
    UChar32 last_cp = 0;
    if (utf8_last_cp(val_tx, vl, &last_cp) && u_hasBinaryProperty(last_cp, UCHAR_TERMINAL_PUNCTUATION)) return 0;
    int n = snprintf(buf, cap, "%s (%s): %s", via_tx, rel_tx, val_tx);
    return (n > 0 && (size_t)n < cap) ? 1 : 0;
}

static void append_out(char **out, size_t *outn, const char *tx, int is_follow, int combine_clauses){
    lp_program_append_out(out, outn, tx, is_follow, combine_clauses);
}

static int is_intermediate_chain_rel(const Firmware *fw, const lp_id *rel){
    for (int a = 0; a < fw->nalt; a++) {
        for (int j = 0; j + 1 < fw->nchain[a]; j++) {
            if (lp_id_eq(rel, &fw->id.chain[a][j])) return 1;
        }
    }
    return 0;
}

static int is_realizable_id(const State *st, const lp_highway *hw, const lp_id *id){
    if (!st || !id) return 0;
    if (lp_id_eq(id, &st->prompt) || lp_id_eq(id, &st->handle)) return 0;
    if (hw && lp_highway_bank_of(hw, id, NULL)) return 0;
    int list; uint32_t slot;
    if (hw && reading_of(hw, id, &list, &slot)) return 0;
    return 1;
}

static int is_raw_uri_artifact(const char *tx, size_t tl, const uint8_t *prompt_text, size_t prompt_len){
    if (!tx || tl < 8) return 0;
    int is_uri = (!strncmp(tx, "http://", 7) || !strncmp(tx, "https://", 8) ||
                  !strncmp(tx, "ftp://", 6) || !strncmp(tx, "file://", 7));
    if (!is_uri) return 0;
    if (prompt_text && prompt_len >= 4) {
        /* Allow URI answers when the prompt itself contains a URI scheme or asks for a link/url. */
        for (size_t i = 0; i + 3 <= prompt_len; i++) {
            if (prompt_text[i] == ':' && prompt_text[i + 1] == '/' && prompt_text[i + 2] == '/') return 0;
        }
    }
    return 1;
}

static int is_realizable_answer_cell(const State *st, const lp_highway *hw, const Cell *x){
    if (!x || x->hub || (x->segment && !x->has_r && !x->routes[R_CLAIM] && !x->routes[R_LAYER] && !x->routes[R_SCAN])) return 0;
    if (x->bridge || (x->has_r && is_intermediate_chain_rel(st->fw, &x->rel))) return 0;
    for (int i = 0; i < st->nocc; i++) if (lp_id_eq(&st->occ[i], &x->id)) return 0;
    for (int k = 0; k < st->nspans; k++) if (lp_id_eq(&st->spans[k], &x->id)) return 0;
    return is_realizable_id(st, hw, &x->id);
}

static int is_lexical_tuple_id(const State *st, const lp_highway *hw, const lp_id *id){
    if (!st || !st->rd || !id) return 0;
    lp_id parts[16];
    size_t np = reader_parts(st->rd, id, parts, 16);
    if (np < 2) return 0;
    if (np == 2 && (lp_id_eq(&parts[0], &st->prompt) || lp_id_eq(&parts[1], &st->prompt) ||
                    lp_id_eq(&parts[0], &st->handle) || lp_id_eq(&parts[1], &st->handle))) return 1;
    if (st->lexical.np > 0 && np == (size_t)st->lexical.np) {
        if (!hw) return 1;
        int list; uint32_t slot;
        if (st->lexical.concept >= 0 && (size_t)st->lexical.concept < np && reading_of(hw, &parts[st->lexical.concept], &list, &slot)) return 1;
        if (st->lexical.lang >= 0 && (size_t)st->lexical.lang < np && lp_highway_bank_of(hw, &parts[st->lexical.lang], NULL)) return 1;
    }
    if (hw && np <= 3) {
        for (size_t k = 0; k < np; k++) {
            int list; uint32_t slot;
            if (lp_highway_bank_of(hw, &parts[k], NULL)) return 1;
            if (reading_of(hw, &parts[k], &list, &slot) && strcmp(hw->list[list].name, "language") != 0) return 1;
        }
    }
    return 0;
}

int lp_forward_execute(const LpForwardRequest *req, LpForwardResult *out){
    if (!req || !out || !req->fw || !req->prompt_text || !req->prompt_len) return 2;
    memset(out, 0, sizeof *out);
    double T = now();

    Firmware fw = *req->fw;
    if (req->temperature >= 0.0) fw.top_within = req->temperature;
    int max_emit_eff = req->max_emit > 0 ? req->max_emit : fw.emit;
    if (max_emit_eff <= 0) max_emit_eff = 32;
    if (req->envelope && req->envelope->max_tokens_out > 0 && (int)req->envelope->max_tokens_out < max_emit_eff)
        max_emit_eff = (int)req->envelope->max_tokens_out;
    if (req->envelope && req->envelope->max_rows_scanned > 0) {
        int min_fan = (req->envelope->preset == LP_ENVELOPE_FAST) ? 512 : 1024;
        int env_fan = (int)(req->envelope->max_rows_scanned / 16ull);
        if (env_fan < min_fan) env_fan = min_fan;
        if (fw.fan > env_fan) fw.fan = env_fan;
        if (fw.walk_fan > 128) fw.walk_fan = 128;
        if (fw.nearest > 64) fw.nearest = 64;
    }
    if (req->envelope && req->envelope->max_hops > 0 && fw.hops > (int)req->envelope->max_hops)
        fw.hops = (int)req->envelope->max_hops;

    char err[512] = "";
    int fwrc = firmware_ids_checked(&fw, err, sizeof err);
    if (fwrc) { snprintf(out->error, sizeof out->error, "%s", err); out->exit_code = fwrc; return fwrc; }
    refuse_named(&fw);
    ELECT = &fw;

    tier0_open(NULL); table_init(); ctx_open(1);
    lp_text *c = lp_text_new(T0);
    if (!c) { snprintf(out->error, sizeof out->error, "cannot open ICU's break iterators\n"); out->exit_code = 1; return 1; }

    State *st = calloc(1, sizeof(State));
    st->fw = &fw; st->c = c; st->t_start_s = T; st->trace_out = req->trace_out;
    st->witness_scope = req->witness_scope;
    st->nwitness_scope = req->nwitness_scope;
    st->witness_scoped = req->witness_scoped;
    st->envelope = req->envelope;
    if (fw.lexical[0][0] && !st->lexical.np) lexicalization_of(st->c, &fw, &st->lexical);

    size_t occurrences = 0;
    Ref pr = text_occurrences(CTX[0], req->prompt_text, req->prompt_len, st->occ, MAXOCC, &occurrences);
    st->prompt = pr.id;
    out->prompt_id = pr.id;
    out->firmware_id = fw.id.image_id;
    if (occurrences > MAXOCC) {
        snprintf(out->error, sizeof out->error, "turn: prompt has %zu occurrences; this execution window supports %d\n", occurrences, MAXOCC);
        state_free(st); lp_text_free(c);
        out->exit_code = 2; return 2;
    }
    st->nocc = (int)occurrences;

    if (st->trace_out) firmware_say(&fw, FW_PULL);

    const char *conninfo = req->conninfo ? req->conninfo : laplace_db();
    int owns_pg = 0;
    PGconn *pg = req->pg;
    if (!pg) { pg = db_read(conninfo); owns_pg = 1; }
    st->pg = pg;
    st->rd = reader_new(pg);
    st->t_start_s = now();

    const char *user = (req->user && *req->user) ? req->user : "user";
    const char *session = (req->session && *req->session) ? req->session : "session";
    const char *responder = (req->responder && *req->responder) ? req->responder : "Laplace";
    Ref who, handle = session_handle(user, session, &who);
    st->handle = handle.id;

    int nturn = 0, ordinal = 1;
    if (req->discourse && req->ndiscourse > 0) {
        int nd = req->ndiscourse < MAXOCC ? req->ndiscourse : MAXOCC;
        memcpy(st->disc, req->discourse, sizeof(lp_id) * (size_t)nd);
        st->ndisc = nd;
    } else if (!req->read_only || req->trace_out || (req->session && *req->session && strcmp(req->session, "stateless"))) {
        Claim *tc = session_turns(st->pg, &fw, &handle.id, &nturn, &ordinal);
        st->trips++; st->work.db_probes++;
        for (int i = 0; i < nturn; i++) {
            size_t remaining = (size_t)(MAXOCC - st->ndisc);
            size_t parts = reader_parts(st->rd, &tc[i].part[1], st->disc + st->ndisc, remaining);
            st->trips++;
            if (parts > remaining) {
                snprintf(out->error, sizeof out->error, "turn: session discourse exceeds this execution window of %d constituents\n", MAXOCC);
                free(tc); reader_free(st->rd); if (owns_pg) PQfinish(pg); state_free(st); lp_text_free(c);
                out->exit_code = 2; return 2;
            }
            st->ndisc += (int)parts;
        }
        free(tc);
    }

    st->program_id = compute_program_id(st->c, &pr.id, &fw.id.image_id, &handle.id, ordinal);
    out->program_id = st->program_id;
    out->ordinal = ordinal;
    out->turns_before = nturn;
    {
        blake3_hasher h; blake3_hasher_init(&h);
        if (req->seed_value >= 0) { int64_t sv = req->seed_value; blake3_hasher_update(&h, &sv, sizeof sv); }
        else {
            blake3_hasher_update(&h, &pr.id, 16); blake3_hasher_update(&h, fw.id.image_id.b, 16);
            if (fw.seed_session) { blake3_hasher_update(&h, &handle.id, 16); int32_t od = ordinal; blake3_hasher_update(&h, &od, sizeof od); }
        }
        blake3_hasher_finalize(&h, st->seed, 32);
    }

    /* Probe 1: microsecond trajectory salience over prompt occurrences */
    st->prompt_text = req->prompt_text;
    st->prompt_len = req->prompt_len;
    resolve_salience(st);
    out->top_salience = st->max_salience;
    for (int i = 0; i < st->nocc; i++) {
        if (st->salience[i] >= st->max_salience && st->max_salience > 0.0) { out->top_salience_id = st->occ[i]; break; }
    }
    resolve_roles(st);
    resolve_polarity(st);
    memcpy(st->traj, st->occ, sizeof(lp_id) * (size_t)st->nocc);
    st->ntraj = st->nocc;

    lp_id emitted[512]; int nemit = 0;
    lp_id skipped[64]; int nskipped = 0;
    char *out_buf = calloc(1, 1); size_t outn = 0;
    const char *disposition = "nothing responds";
    Field fd = { 0 }; Curve near[8]; int nnear = 0;

    /* Probe 2: Single batched COUPLE over deduplicated (occ + case-variants + spans + prompt + disc) */
    {
        double tc = now();
        lp_idmap *uniq = lp_idmap_new();
        lp_id bkeys[MAXOCC * 3 + 2];
        int bocc[MAXOCC * 3 + 2];
        int nb = 0;
        int n_anchor = 0;
        for (int i = 0; i < st->nocc; i++) {
            if (!st->composed[i]) continue;
            if (!st->witness_scoped && st->df_ready && st->df[i] == 0) continue;
            if (st->df_ready && st->df[i] > fw.fan && st->max_salience > 0.0 &&
                st->salience[i] < 0.15 * st->max_salience) continue;
            n_anchor++;
        }
        for (int i = 0; i < st->nocc; i++) {
            if (!st->composed[i]) continue;
            if (!st->witness_scoped && st->df_ready && st->df[i] == 0) {
                st->ctx_hub[i] = 1;
                continue;
            }
            int is_salient_anchor = (st->max_salience > 0.0 && st->salience[i] >= 0.40 * st->max_salience);
            if (n_anchor > 0 && !is_salient_anchor && st->df_ready && st->df[i] > fw.fan &&
                ((!st->witness_scoped) || (st->envelope && st->max_salience > 0.0 && st->salience[i] < 0.15 * st->max_salience))) {
                st->ctx_hub[i] = 1;
                continue;
            }
            if (st->witness_scoped || !st->df_ready || st->df_exact[i] > 0 || !st->has_occ_lower[i]) {
                bool fresh; lp_idmap_put(uniq, &st->occ[i], &fresh);
                if (fresh && nb < MAXOCC * 3) { bkeys[nb] = st->occ[i]; bocc[nb] = i; nb++; }
            }
            if (st->has_occ_lower[i] && nb < MAXOCC * 3) {
                bool fresh; lp_idmap_put(uniq, &st->occ_lower[i], &fresh);
                if (fresh) { bkeys[nb] = st->occ_lower[i]; bocc[nb] = -(1000 + i); nb++; }
            }
        }
        for (int i = 0; i < st->nspans && nb < MAXOCC * 3; i++) {
            bool fresh; lp_idmap_put(uniq, &st->spans[i], &fresh);
            if (fresh) { bkeys[nb] = st->spans[i]; bocc[nb] = -3 - i; nb++; }
        }
        {
            bool fresh; lp_idmap_put(uniq, &st->prompt, &fresh);
            if (fresh && nb < MAXOCC * 3) { bkeys[nb] = st->prompt; bocc[nb] = -1; nb++; }
        }
        for (int i = 0; i < st->ndisc && nb < MAXOCC * 3 + 1; i++) {
            if (lp_tier0_codepoint(T0, &st->disc[i]) >= 0) continue;
            bool fresh; lp_idmap_put(uniq, &st->disc[i], &fresh);
            if (fresh) { bkeys[nb] = st->disc[i]; bocc[nb] = -2; nb++; }
        }
        lp_idmap_free(uniq);

        int *hub = calloc((size_t)(nb ? nb * 2 : 2), sizeof(int)), nh = 0;
        Hold *hh = nb ? holds_capped_scoped(st->pg, bkeys, nb, fw.fan, 1, 3,
            st->witness_scope, st->nwitness_scope, st->witness_scoped, hub, &nh) : NULL;
        if (nb) { st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nh; }

        int *seq_of = malloc(sizeof(int) * (size_t)(nb ? nb : 1));
        for (int i = 0; i < nb; i++) seq_of[i] = i;
        couple_ingest_holds(st, &fd, hh, nh, hub, bkeys, seq_of, nb, bocc, R_CLAIM);
        if (st->nocc >= 2 && fw.nearest > 0 && can_spend_probes(st, 1) && !getenv("LAPLACE_COUPLE_SQL")) {
            int p_occ[MAXOCC]; for (int i = 0; i < st->nocc; i++) p_occ[i] = i;
            nnear = shape_measured(st, st->occ, st->nocc, p_occ, hh, nh, near, 8);
        } else if (st->nocc >= 2 && fw.nearest > 0 && can_spend_probes(st, 2)) {
            nnear = shape_near(st, st->occ, st->nocc, near, 8);
        }
        free(seq_of); holds_free(hh, nh); free(hub);
        for (int i = 0; i < st->nocc; i++) { Cell *x = cell_find(&fd, &st->occ[i]); if (x) x->force = 0.0; }
        contextual_salience_lift(st, &fd);
        if (can_spend_probes(st, 3) && (!st->envelope || st->envelope->preset > LP_ENVELOPE_FAST))
            resolve_prompt_langs(st, &fd);

        if (st->trace_out) {
            double couple_ms = (now() - tc) * 1000.0;
            double t_tr = now();
            char idt[33]; lp_id_hex(&pr.id, idt);
            fprintf(st->trace_out, "RESOLVE    prompt %s, tier %d, %d occurrences; session \"%s\" of %s, turn %d (%d before it, %d discourse entities)\n",
                    idt, pr.tier, st->nocc, session, user, ordinal, nturn, st->ndisc);
            receipt_resolve(req->receipt_out, &fw.id.image_id, &st->program_id, &pr.id, NULL, user, session, ordinal, nturn);
            lp_id pref[MAXOCC]; size_t npref = 0;
            for (int i = 0; i < st->nocc; i++) if (st->composed[i]) pref[npref++] = st->occ[i];
            if (npref) reader_prefetch(st->rd, pref, npref, 32);
            fprintf(st->trace_out, "           obligations:");
            for (int i = 0; i < st->nocc; i++) {
                if (st->composed[i]) {
                    if (st->prompt_text && st->occ_len[i] > 0 && st->occ_off[i] + st->occ_len[i] <= st->prompt_len) {
                        fprintf(st->trace_out, " %.*s%s(%.2f)", (int)st->occ_len[i],
                                (const char *)(st->prompt_text + st->occ_off[i]),
                                (st->open.w[i >> 6] >> (i & 63)) & 1 ? "" : "~", st->role[i]);
                    } else {
                        char *tx = reader_text(st->rd, &st->occ[i], 32);
                        fprintf(st->trace_out, " %s%s(%.2f)", tx, (st->open.w[i >> 6] >> (i & 63)) & 1 ? "" : "~", st->role[i]);
                        free(tx);
                    }
                }
            }
            fprintf(st->trace_out, "\n");
            st->t_start_s += now() - t_tr;
            int routes[R_KINDS] = { 0 };
            for (int z = 0; z < fd.n; z++) for (int k = 0; k < R_KINDS; k++) routes[k] += fd.c[z].routes[k];
            fprintf(st->trace_out, "COUPLE     %d entities respond:", fd.n);
            for (int k = 0; k < R_KINDS; k++) if (routes[k]) fprintf(st->trace_out, " %d by %s", routes[k], RK[k]);
            fprintf(st->trace_out, "   (%.1f ms)\n", couple_ms);
        }
        for (int round = 1; round < fw.rounds && can_spend_probes(st, 2); round++) {
            lp_idmap *hm = lp_idmap_sized(sizeof(HeadStat));
            int fresh_n = layer_round(st, &fd, round, hm);
            if (st->trace_out) heads_say(st, hm, "heads");
            lp_idmap_free(hm);
            if (!fresh_n) break;
        }
        st->npos = st->nocc;
        if (fw.walks > 0 && fw.steps > 0 && can_spend_probes(st, 3))
            walk(st, 0, st->nocc);
        if (can_spend_probes(st, 2))
            context(st, 0, st->nocc);
    }

    Bind *bind = calloc((size_t)st->nocc + 1, sizeof(Bind));
    Readings R; readings_build(st, &fd, bind, &R);
    Next nx0[64]; int nn0 = 0;

    /* Probe 3: FORWARD_SUFFIX(_SCOPED) for sequence continuations */
    if (can_spend_probes(st, 1) && st->nocc > 0 && !(st->witness_scoped && st->nwitness_scope <= 0)) {
        nn0 = follows(st, nx0, 64);
        for (int p = 0; p < st->nocc; p++) {
            if (bind[p].ncand) {
                double sc; int lv; int pick = reading_for(st, &fd, bind, &R, p, p, &sc, &lv);
                bind[p].choice = pick; bind[p].level = lv;
            }
            pass_add(st, &st->occ[p]);
            if (bind[p].choice >= 0) pass_add(st, &fd.c[R.cand[bind[p].choice].cell].id);
            for (int j = 0; j < p; j++) {
                if (!bind[j].ncand) continue;
                double sc; int lv; int np_ = reading_for(st, &fd, bind, &R, j, p + 1, &sc, &lv);
                if (np_ >= 0 && np_ != bind[j].choice) { bind[j].choice = np_; bind[j].level = lv; }
            }
        }
    }

    if (nnear) {
        Bits slots; memset(&slots, 0, sizeof slots); int nslot = 0, nframe = 0;
        for (int i = 0; i < st->nocc; i++) {
            if (!((st->open.w[i >> 6] >> (i & 63)) & 1)) continue;
            int is_anchor = (st->composed[i] && st->role[i] >= 0.20) || (st->max_salience > 0.0 && st->salience[i] >= 0.50 * st->max_salience);
            int held = 0;
            if (!is_anchor) {
                for (int k = 0; k < near[0].nv && !held; k++) held = lp_id_eq(&near[0].v[k], &st->occ[i]);
            }
            if (held) nframe++; else { bit_set(&slots, i); nslot++; }
        }
        if (nframe && nslot) for (int w = 0; w < MAXOCC / 64; w++) st->open.w[w] &= slots.w[w];
    }
    for (int i = 0; i < nnear; i++) free(near[i].v);

    int nambig = 0, nbound = orient(st, &fd, bind, &R, &nambig), ncentre = 0, capped = 0;
    lp_id centre[MAXOCC];
    for (int i = 0; i < st->nocc; i++) {
        capped |= bind[i].capped;
        if (bind[i].choice >= 0 && ((st->open.w[i >> 6] >> (i & 63)) & 1)) centre[ncentre++] = fd.c[bind[i].choice].id;
    }
    free(R.cand); free(R.first);

    /* Fallback centres from highest-salience obligations when ORIENT did not lock highway readings. */
    if (!ncentre) {
        for (int i = 0; i < st->nocc && ncentre < 8; i++) {
            if (((st->open.w[i >> 6] >> (i & 63)) & 1) && st->role[i] > 0.0) centre[ncentre++] = st->occ[i];
        }
    }
    if (st->trace_out) fprintf(st->trace_out, "ROUTE      hops %d, fan %d, k %g, lambda %g, emit at most %d, from %d centre%s\n", fw.hops, fw.fan, fw.k, fw.lambda, max_emit_eff, ncentre, ncentre == 1 ? "" : "s");
    if (can_spend_probes(st, 4) && ncentre > 0)
        scan(st, &fd, centre, ncentre);
    if (st->trace_out) fprintf(st->trace_out, "COMPOSE    a frontier of %d\n", fd.n);

    disposition = (nbound || fd.n) ? "unresolved" : (st->refused && !fd.n) ? "refused: blocked by active firmware policy" : capped ? "resource-bounded: no joint binding among the candidates weighed" : "unresolved: nothing responds in scope";
    /* Never mute non-trivial queries solely because nambig > 0; tie policy governs clarification/draw/first. */
    int budget = max_emit_eff;
    int owed0 = owed(st, &st->open, &st->open);
    Bits content_mask0 = { 0 };
    for (int i = 0; i < st->nocc; i++) {
        if (st->composed[i] && ((st->open.w[i >> 6] >> (i & 63)) & 1) && st->role[i] >= 0.20)
            bit_set(&content_mask0, i);
    }
    int multi_anc_prompt0 = bit_count_and(&content_mask0, &content_mask0) >= 2;
    double conf_sum = 0.0;
    const lp_highway *hw = lp_highway_map(NULL);

    for (int step = 0; step < budget; step++) {
        if (step == 0 && (fd.n > 0 || nn0 > 0)) {
            lp_id pref[MAXOCC * 2]; size_t npref = 0;
            size_t pref_cap = (st->trace_out || (st->envelope && (st->envelope->preset <= LP_ENVELOPE_STANDARD || (st->envelope->max_wall_us > 0 && st->envelope->max_wall_us <= 500000)))) ? 16 : (MAXOCC * 2);
            for (int i = 0; i < nn0 && npref < pref_cap; i++) {
                if (!lp_id_eq(&nx0[i].id, &st->prompt) && !lp_id_eq(&nx0[i].id, &st->handle)) pref[npref++] = nx0[i].id;
            }
            for (int z = 0; z < fd.n && npref < pref_cap; z++) {
                const Cell *x = &fd.c[z];
                if (!x->hub && !lp_id_eq(&x->id, &st->prompt) && !lp_id_eq(&x->id, &st->handle)) pref[npref++] = x->id;
            }
            if (npref) {
                double t_pr = now();
                uint64_t b = reader_trips(st->rd);
                reader_prefetch(st->rd, pref, npref, pref_cap <= 16 ? 160 : 400);
                st->trips += reader_trips(st->rd) - b;
                if (st->trace_out) st->t_start_s += now() - t_pr;
            }
        }
        if (step > 0 && envelope_hit(st)) break;
        Bits anchor_mask = { 0 };
        Bits content_mask = { 0 };
        for (int i = 0; i < st->nocc; i++) {
            if (!st->composed[i] || !((st->open.w[i >> 6] >> (i & 63)) & 1)) continue;
            if (st->role[i] >= 0.75) bit_set(&anchor_mask, i);
            if (st->role[i] >= 0.20) bit_set(&content_mask, i);
        }
        if (!bit_count_and(&anchor_mask, &anchor_mask)) {
            for (int i = 0; i < st->nocc; i++) {
                if (st->composed[i] && ((st->open.w[i >> 6] >> (i & 63)) & 1)) bit_set(&anchor_mask, i);
            }
        }
        if (!bit_count_and(&content_mask, &content_mask)) content_mask = anchor_mask;
        int multi_anc_prompt = bit_count_and(&content_mask, &content_mask) >= 2;

        size_t pcap = (size_t)fd.n + 192;
        Prop *pp = malloc(sizeof(Prop) * pcap); int np = 0;
        Next nx[64];
        int nn = (step == 0) ? nn0 : (can_spend_probes(st, 1) ? follows(st, nx, 64) : 0);
        if (step == 0 && nn0) memcpy(nx, nx0, sizeof(Next) * (size_t)nn0);
        for (int i = 0; i < nn && np < (int)pcap; i++) {
            if (!is_realizable_id(st, hw, &nx[i].id)) continue;
            int was_skipped = 0;
            for (int k = 0; k < nskipped && !was_skipped; k++) was_skipped = lp_id_eq(&skipped[k], &nx[i].id);
            if (was_skipped) continue;
            Prop p; memset(&p, 0, sizeof p);
            p.id = nx[i].id; p.kind = P_FOLLOW; p.cont = nx[i].len; p.times = nx[i].times; p.occ = -1; p.shape = INFINITY;
            const Cell *x = cell_find(&fd, &nx[i].id);
            if (x) { p.conf = x->has_r ? lp_confidence(&x->r, fw.k) : 0.0; p.hub = x->hub; }
            weigh_prop(st, &fd, hw, &content_mask, multi_anc_prompt, &p); pp[np++] = p;
        }
        int have_attested_claim = 0;
        for (int z = 0; z < fd.n && !have_attested_claim; z++) {
            const Cell *x = &fd.c[z];
            if (!x->hub && bit_count_and(&x->support, &st->open) &&
                (x->has_r || x->routes[R_CLAIM] > 0 || x->routes[R_SCAN] > 0 || x->routes[R_LAYER] > 0 || x->routes[R_DISCOURSE] > 0))
                have_attested_claim = 1;
        }
        for (int z = 0; z < fd.n && np < (int)pcap; z++) {
            const Cell *x = &fd.c[z];
            if (!bit_count_and(&x->support, &st->open)) continue;
            if (have_attested_claim && !x->has_r && !x->routes[R_CLAIM] && !x->routes[R_SCAN] && !x->routes[R_LAYER] && !x->routes[R_DISCOURSE])
                continue;
            if (!is_realizable_answer_cell(st, hw, x)) continue;
            int already = 0;
            for (int k = 0; k < nemit && !already; k++) already = lp_id_eq(&emitted[k], &x->id);
            for (int k = 0; k < nskipped && !already; k++) already = lp_id_eq(&skipped[k], &x->id);
            for (int k = 0; k < np && !already; k++) already = lp_id_eq(&pp[k].id, &x->id);
            if (already) continue;
            Prop fp; memset(&fp, 0, sizeof fp); fp.id = x->id; fp.kind = P_CHAIN; fp.occ = -1; fp.shape = INFINITY;
            fp.conf = x->has_r ? lp_confidence(&x->r, fw.k) : 0.0; fp.hub = x->hub;
            weigh_prop(st, &fd, hw, &content_mask, multi_anc_prompt, &fp);
            if (fp.grounds > 0) pp[np++] = fp;
        }
        /* Follow firmware chain only if COUPLE/FOLLOWS/WALK/CONTEXT/SCAN produced zero proposals. */
        int need_chain = (np == 0);
        int have_attested_prop = 0;
        for (int k = 0; k < np && !have_attested_prop; k++) {
            if (pp[k].conf > 0.0 && pp[k].grounds > 0) have_attested_prop = 1;
        }
        int allow_chain = !envelope_hit(st) || (!have_attested_prop && (!st->envelope || !st->envelope->max_db_probes || st->work.db_probes < st->envelope->max_db_probes));
        if (step == 0 && need_chain && fw.nalt > 0 && st->nocc > 0 && allow_chain) {
            int cand_occ[MAXOCC]; double cand_sc[MAXOCC]; int ncand_occ = 0;
            for (int i = 0; i < st->nocc; i++) {
                if (!st->composed[i] || !((st->open.w[i >> 6] >> (i & 63)) & 1)) continue;
                int is_pref = (st->max_salience > 0.0 && st->salience[i] >= 0.40 * st->max_salience) || st->role[i] >= 0.75;
                if (!is_pref) continue;
                int dup = 0;
                for (int k = 0; k < ncand_occ && !dup; k++) dup = lp_id_eq(&st->occ[cand_occ[k]], &st->occ[i]);
                if (!dup) {
                    cand_occ[ncand_occ] = i;
                    cand_sc[ncand_occ] = (st->role[i] > 0.0 ? st->role[i] : 0.1) * (st->salience[i] > 0.0 ? st->salience[i] : 1.0);
                    ncand_occ++;
                }
            }
            if (ncand_occ == 0) {
                int best_occ = -1; double best_r = -1.0;
                for (int i = 0; i < st->nocc; i++) {
                    if (st->composed[i] && ((st->open.w[i >> 6] >> (i & 63)) & 1) && st->role[i] > best_r) {
                        best_r = st->role[i]; best_occ = i;
                    }
                }
                if (best_occ >= 0) { cand_occ[0] = best_occ; cand_sc[0] = best_r; ncand_occ = 1; }
            }
            for (int a = 1; a < ncand_occ; a++) {
                int ci = cand_occ[a]; double cs = cand_sc[a]; int p = a;
                while (p > 0 && cand_sc[p - 1] < cs) { cand_occ[p] = cand_occ[p - 1]; cand_sc[p] = cand_sc[p - 1]; p--; }
                cand_occ[p] = ci; cand_sc[p] = cs;
            }
            int max_try = ncand_occ < 3 ? ncand_occ : 3;
            int max_ok = fw.combine_clauses ? 2 : 1;
            int got_ok = 0;
            for (int ci = 0; ci < max_try && got_ok < max_ok && np < (int)pcap; ci++) {
                int idx = cand_occ[ci];
                lp_id ans; lp_rating rt;
                const lp_id *rd_id = (bind[idx].choice >= 0) ? &fd.c[bind[idx].choice].id : NULL;
                if (chain_from(st, &st->occ[idx], rd_id, &ans, &rt)) {
                    int already = 0;
                    for (int k = 0; k < nemit && !already; k++) already = lp_id_eq(&emitted[k], &ans);
                    for (int k = 0; k < nskipped && !already; k++) already = lp_id_eq(&skipped[k], &ans);
                    for (int k = 0; k < st->nocc && !already; k++) already = lp_id_eq(&st->occ[k], &ans);
                    for (int k = 0; k < np && !already; k++) already = lp_id_eq(&pp[k].id, &ans);
                    if (already) continue;
                    Prop cp; memset(&cp, 0, sizeof cp); cp.id = ans; cp.kind = P_CHAIN; cp.occ = idx; cp.shape = INFINITY;
                    cp.conf = lp_confidence(&rt, fw.k); bit_set(&cp.by[CH_STRAND], idx); cp.agree = 1;
                    cp.cont = 0; cp.tier = 0;
                    cp.grounds = owed(st, &cp.by[CH_STRAND], &st->open);
                    if (cp.grounds == 0) cp.grounds = (int)(st->role[idx] * 1000.0 + 0.5);
                    pp[np++] = cp;
                    got_ok++;
                }
            }
        }
        if (step > 0 && st->npass >= 2 && can_spend_probes(st, 2) && np < (int)pcap) {
            int wn = st->npass < 8 ? st->npass : 8;
            const lp_id *win = st->pass + st->npass - wn;
            Curve an[8]; int na = shape_near(st, win, wn, an, 8);
            for (int c_ = 0; c_ < na; c_++) {
                int meet = -1;
                for (int k = an[c_].nv - 1; k >= 0 && meet < 0; k--)
                    for (int z = 0; z < wn; z++) if (lp_id_eq(&an[c_].v[k], &win[z])) { meet = k; break; }
                int nxt = -1;
                for (int k = meet + 1; meet >= 0 && k < an[c_].nv; k++)
                    if (lp_tier0_codepoint(T0, &an[c_].v[k]) < 0) { nxt = k; break; }
                if (nxt < 0) continue;
                const lp_id *id = &an[c_].v[nxt];
                if (!is_realizable_id(st, hw, id)) continue;
                int was_skipped = 0;
                for (int k = 0; k < nskipped && !was_skipped; k++) was_skipped = lp_id_eq(&skipped[k], id);
                if (was_skipped) continue;
                int have = -1; for (int z = 0; z < np; z++) if (lp_id_eq(&pp[z].id, id)) have = z;
                if (have >= 0) { if (an[c_].d < pp[have].shape) pp[have].shape = an[c_].d; continue; }
                if (np >= (int)pcap) continue;
                Prop sp; memset(&sp, 0, sizeof sp); sp.id = *id; sp.kind = P_SHAPE; sp.occ = -1; sp.shape = an[c_].d;
                const Cell *x = cell_find(&fd, id); if (x) { sp.conf = x->has_r ? lp_confidence(&x->r, fw.k) : 0.0; sp.hub = x->hub; }
                weigh_prop(st, &fd, hw, &content_mask, multi_anc_prompt, &sp); pp[np++] = sp;
            }
            for (int c_ = 0; c_ < na; c_++) free(an[c_].v);
        }

        {
            int has_attested_valid = 0;
            for (int z = 0; z < np; z++) {
                if (pp[z].conf <= 0.0) {
                    Bits sup_all = { 0 };
                    for (int c_ = 0; c_ < 3; c_++)
                        for (int w = 0; w < MAXOCC / 64; w++) sup_all.w[w] |= pp[z].by[c_].w[w];
                    const Cell *cx = cell_find(&fd, &pp[z].id);
                    if (cx) for (int w = 0; w < MAXOCC / 64; w++) sup_all.w[w] |= cx->support.w[w];
                    if (!bit_count_and(&sup_all, &anchor_mask)) pp[z].grounds = 0;
                }
                if (pp[z].conf > 0.0 && pp[z].grounds > 0) has_attested_valid = 1;
            }
            if (has_attested_valid) {
                int keep_n = 0;
                for (int z = 0; z < np; z++) {
                    if (pp[z].conf > 0.0) pp[keep_n++] = pp[z];
                }
                np = keep_n;
            }
        }

        if (!np) {
            free(pp);
            if (!nemit && nambig > 0 && fw.tie == FW_TIE_ASK) {
                char *clarify = strdup("Multiple interpretations match the prompt; please specify the intended sense.");
                append_out(&out_buf, &outn, clarify, 0, fw.combine_clauses);
                free(clarify);
                disposition = "ambiguous: clarified with prompt";
            } else if (!nemit) {
                disposition = ncentre ? "unresolved: nothing follows and nothing grounds what is open" : disposition;
            }
            break;
        }
        qsort(pp, (size_t)np, sizeof(Prop), elect);
        if (np > 0 && st->rd) {
            size_t npre = (size_t)(np < 48 ? np : 48);
            lp_id top_ids[48];
            for (size_t z = 0; z < npre; z++) { top_ids[z] = pp[z].id; reader_want(st->rd, &top_ids[z]); }
            reader_prefetch(st->rd, top_ids, npre, 400);

            /* True Merkle-DAG multi-tier check (zero extra SQL): a Tier >= 2 composition has >= 2 non-codepoint
             * children (phrase/clause/sentence/AST node), unlike a Tier-1 single word whose children are Tier-0 codepoints. */
            if (fw.multi_tier) {
                for (size_t z = 0; z < npre; z++) {
                    if (pp[z].tier >= 2 || pp[z].grounds <= 0) continue;
                    lp_id tparts[4];
                    size_t tnp = reader_parts(st->rd, &pp[z].id, tparts, 4);
                    int non_cp = 0;
                    for (size_t ti = 0; ti < tnp; ti++) {
                        if (lp_tier0_codepoint(T0, &tparts[ti]) < 0) non_cp++;
                    }
                    if (non_cp >= 2) {
                        pp[z].tier = 2;
                        const Cell *txc = cell_find(&fd, &pp[z].id);
                        int trusted_multi = pp[z].conf > 0.0 || (txc && (txc->has_r || txc->routes[R_CLAIM] > 0 ||
                            txc->routes[R_SCAN] > 0 || txc->routes[R_LAYER] > 0 || txc->routes[R_DISCOURSE] > 0 ||
                            pp[z].agree >= 2 || bit_count_and(&txc->support, &st->open) >= 2));
                        if (trusted_multi) pp[z].cont += 1;
                    }
                }
            }

            /* Universal candidate specificity via GIN self-information (laplace_salience):
             * populates FW_E_SHARED (p.hub = df) and demotes global schema/type hubs (df > fw.fan,
             * e.g. universal class nodes with 100k+ holders) with zero corpus or language hardcoding. */
            if (st->pg && npre > 1 && can_spend_probes(st, 2) &&
                (!st->envelope || st->envelope->preset > LP_ENVELOPE_FAST)) {
                int ncsal = 0;
                st->work.db_probes++;
                LpSalienceRow *csal = salience_of(st->pg, top_ids, (int)npre,
                    st->witness_scoped ? st->witness_scope : NULL,
                    st->witness_scoped ? st->nwitness_scope : 0, &ncsal);
                st->trips++;
                st->work.rows_scanned += (uint64_t)ncsal;
                int any_specific = 0;
                for (int r = 0; r < ncsal; r++) {
                    for (size_t z = 0; z < npre; z++) {
                        if (!lp_id_eq(&pp[z].id, &csal[r].id)) continue;
                        pp[z].hub = (double)csal[r].df;
                        if (csal[r].df <= (int64_t)fw.fan) any_specific = 1;
                    }
                }
                if (any_specific) {
                    for (size_t z = 0; z < npre; z++) {
                        if (pp[z].hub > (double)fw.fan) pp[z].grounds = 0;
                    }
                }
                free(csal);
            }

            int keep_n = 0;
            for (int z = 0; z < np; z++) {
                if ((size_t)z < npre && is_lexical_tuple_id(st, hw, &pp[z].id)) {
                    if (nskipped < 64) skipped[nskipped++] = pp[z].id;
                    continue;
                }
                if ((size_t)z < npre && pp[z].grounds == 0 && pp[z].hub > (double)fw.fan) {
                    if (nskipped < 64) skipped[nskipped++] = pp[z].id;
                    continue;
                }
                pp[keep_n++] = pp[z];
            }
            np = keep_n;
            if (!np) {
                free(pp);
                if (!nemit) disposition = ncentre ? "unresolved: nothing follows and nothing grounds what is open" : disposition;
                break;
            }
            qsort(pp, (size_t)np, sizeof(Prop), elect);
            if (st->trace_out && step == 0) {
                for (int dz = 0; dz < np && dz < 5; dz++) {
                    char *dt = reader_text(st->rd, &pp[dz].id, 48);
                    fprintf(st->trace_out, "           cand %d: \"%s\" g=%d cont=%d agr=%d hds=%d cooc=%ld wk=%ld conf=%.3f hub=%.0f f=%.5f inl=%d\n",
                            dz + 1, dt ? dt : "", pp[dz].grounds, pp[dz].cont, pp[dz].agree, pp[dz].heads,
                            pp[dz].cooc, pp[dz].walk, pp[dz].conf, pp[dz].hub, pp[dz].force, pp[dz].inlang);
                    free(dt);
                }
            }
        }
        if (nemit > 0 && pp[0].conf <= 0.0) {
            free(pp);
            disposition = bit_count_and(&st->open, &st->open)
                ? "complete: what is still owed is within what the firmware leaves open"
                : "complete: every obligation is grounded";
            break;
        }
        if (nemit > 0) {
            const Cell *cx0 = cell_find(&fd, &pp[0].id);
            int direct_open_anchor = cx0 && !cx0->lateral_hop && bit_count_and(&cx0->direct_support, &anchor_mask) > 0;
            if (!direct_open_anchor && pp[0].kind != P_FOLLOW && pp[0].grounds < 500) {
                free(pp);
                disposition = bit_count_and(&st->open, &st->open)
                    ? "complete: what is still owed is within what the firmware leaves open"
                    : "complete: every obligation is grounded";
                break;
            }
        }
        if (!pp[0].grounds && (nemit == 0 || bit_count_and(&st->open, &st->open))) {
            free(pp); if (!nemit) disposition = "unresolved: what follows grounds nothing that is owed"; break;
        }

        int tied[192], nt = 0;
        for (int z = 0; z < np && nt < 192; z++) if (!z || cannot_tell(&pp[0], &pp[z], &fw)) tied[nt++] = z;
        int pick = 0;
        if (nt > 1) {
            if (fw.tie == FW_TIE_DRAW) pick = tied[(int)(draw(st, 0x5e1ec7u, (uint32_t)step, 0) * nt)];
            else if (fw.tie == FW_TIE_ASK && !nemit) {
                reader_want(st->rd, &pp[tied[0]].id);
                reader_want(st->rd, &pp[tied[1]].id);
                char *t0 = reader_text(st->rd, &pp[tied[0]].id, 64);
                char *t1 = reader_text(st->rd, &pp[tied[1]].id, 64);
                char askbuf[256];
                snprintf(askbuf, sizeof askbuf, "Multiple equally supported readings found (%s vs %s); which did you intend?", t0, t1);
                free(t0); free(t1); free(pp);
                append_out(&out_buf, &outn, askbuf, 0, fw.combine_clauses);
                disposition = "ambiguous: proposals the evidence cannot tell apart, and the firmware asks";
                break;
            }
        }
        Prop sel = pp[pick]; free(pp);

        lp_id realize_id = sel.id;
        {
            lp_id rparts[4];
            size_t rnp = reader_parts(st->rd, &sel.id, rparts, 4);
            int rl0 = 0, rl1 = 0; uint32_t rs0 = 0, rs1 = 0;
            int tail_is_tag = hw && rnp == 2 && (lp_highway_bank_of(hw, &rparts[1], NULL) || reading_of(hw, &rparts[1], &rl1, &rs1));
            int head_is_tag = hw && rnp == 2 && (lp_highway_bank_of(hw, &rparts[0], NULL) || reading_of(hw, &rparts[0], &rl0, &rs0));
            if (rnp == 2 && tail_is_tag && !head_is_tag) {
                realize_id = rparts[0];
            }
        }
        reader_want(st->rd, &realize_id);
        char *tx = reader_text(st->rd, &realize_id, 400);
        size_t tl = tx ? strlen(tx) : 0;
        int echo_skip = lp_program_is_prompt_echo(req->prompt_text, req->prompt_len, tx, tl, sel.kind == P_FOLLOW);
        if (!echo_skip && is_lexical_tuple_id(st, hw, &sel.id)) echo_skip = 1;
        if (!echo_skip && is_raw_uri_artifact(tx, tl, req->prompt_text, req->prompt_len)) echo_skip = 1;
        if (echo_skip) {
            /* Skip turn/prompt wrapper, internal lexicalization tuple, or prompt-contained span without clearing open obligations */
            free(tx);
            if (nskipped < 64) { skipped[nskipped++] = sel.id; if (step >= 0 && nskipped < 32) step--; }
            continue;
        }
        /* Multi-clause compositional binding: on multi-anchor prompts, ground each clause with its prompt anchor
         * when the realized clause does not already name it, preserving exact [anchor: proposition] structure. */
        if (multi_anc_prompt0 && fw.combine_clauses && sel.kind == P_CHAIN && tx && tl > 0) {
            const Cell *cx = cell_find(&fd, &sel.id);
            int bound_occ = -1;
            double best_role = -1.0;
            for (int oi = 0; oi < st->nocc; oi++) {
                if (!((content_mask0.w[oi >> 6] >> (oi & 63)) & 1)) continue;
                int grounds_oi = ((sel.by[CH_STRAND].w[oi >> 6] >> (oi & 63)) & 1) ||
                                 (cx && ((cx->support.w[oi >> 6] >> (oi & 63)) & 1));
                if (grounds_oi && st->role[oi] > best_role) { best_role = st->role[oi]; bound_occ = oi; }
            }
            if (bound_occ >= 0 && st->prompt_text && st->occ_len[bound_occ] > 0 &&
                st->occ_off[bound_occ] + st->occ_len[bound_occ] <= st->prompt_len && st->occ_len[bound_occ] < 64) {
                char anc_txt[64];
                memcpy(anc_txt, st->prompt_text + st->occ_off[bound_occ], st->occ_len[bound_occ]);
                anc_txt[st->occ_len[bound_occ]] = 0;
                if (strstr(tx, anc_txt) == NULL) {
                    size_t ncap = st->occ_len[bound_occ] + tl + 4;
                    char *prefixed = malloc(ncap);
                    if (prefixed) {
                        snprintf(prefixed, ncap, "%s: %s", anc_txt, tx);
                        free(tx);
                        tx = prefixed;
                        tl = strlen(tx);
                    }
                }
            }
        }
        /* Multi-clause structured proposition realization: preserve [via, rel, target] binding on secondary clauses. */
        if (nemit > 0 && fw.combine_clauses && sel.kind == P_CHAIN && tx && tl > 0) {
            const Cell *cx = cell_find(&fd, &sel.id);
            int via_l = 0; uint32_t via_s = 0;
            if (cx && cx->has_r && !is_gloss_rel(st, &cx->rel) && !is_lexical_sense_rel(st, &cx->rel) &&
                !lp_id_eq(&cx->via, &st->prompt) && !lp_id_eq(&cx->via, &cx->rel) &&
                !(hw && (lp_highway_bank_of(hw, &cx->via, NULL) || reading_of(hw, &cx->via, &via_l, &via_s))) &&
                !is_lexical_tuple_id(st, hw, &cx->via)) {
                reader_want(st->rd, &cx->via);
                reader_want(st->rd, &cx->rel);
                char *via_tx = reader_text(st->rd, &cx->via, 64);
                char *rel_tx = reader_text(st->rd, &cx->rel, 64);
                char clause_buf[256];
                if (lp_program_format_claim_clause(via_tx, rel_tx, tx, clause_buf, sizeof clause_buf)) {
                    free(tx);
                    tx = strdup(clause_buf);
                }
                free(via_tx);
                free(rel_tx);
            }
        }
        size_t at = outn;
        append_out(&out_buf, &outn, tx, sel.kind == P_FOLLOW, fw.combine_clauses);
        const char *skind = sel.kind == P_FOLLOW ? "follows" : sel.kind == P_SHAPE ? "like" : "answers";
        if (st->trace_out) fprintf(st->trace_out, "STEP %-3d   %s \"%s\"   grounds %.2f, continuity %d, confidence %.3f\n",
                                   step + 1, skind, tx, sel.grounds / 1000.0, sel.cont, sel.conf);
        receipt_step(req->receipt_out, step + 1, skind, &sel.id, out_buf + at, sel.grounds / 1000.0, sel.cont, sel.conf);
        if (req->on_step) req->on_step(req->on_step_ctx, step + 1, skind, &sel.id, out_buf + at, sel.grounds / 1000.0, sel.cont, sel.conf);
        free(tx);

        conf_sum += sel.conf;
        if (nemit < 512) emitted[nemit++] = sel.id;
        if (st->ntraj < 4096) st->traj[st->ntraj++] = sel.id;
        pass_add(st, &sel.id);
        st->work.tokens_out = (uint32_t)nemit;

        /* Check explicit stop_sequences from OpenAI API caller */
        int stopped = 0;
        for (int s = 0; s < req->nstop_sequences && !stopped; s++) {
            const char *ss = req->stop_sequences[s];
            if (!ss || !*ss) continue;
            char *hit = strstr(out_buf, ss);
            if (hit) { *hit = 0; outn = (size_t)(hit - out_buf); stopped = 1; }
        }
        if (stopped) { disposition = "stopped: stop sequence reached"; break; }

        { const Cell *x = cell_find(&fd, &sel.id); if (x) bit_clear(&st->open, &x->support); }
        for (int c_ = 0; c_ < 3; c_++) bit_clear(&st->open, &sel.by[c_]);

        int rem = owed(st, &st->open, &st->open);
        int open_anchors = bit_count_and(&st->open, &anchor_mask);
        int can_recouple = !st->envelope || (st->envelope->preset > LP_ENVELOPE_FAST &&
                           (!st->envelope->max_db_probes || st->work.db_probes + 3 < st->envelope->max_db_probes));
        if (lp_program_should_stop_enough(sel.kind == P_FOLLOW, fw.combine_clauses && can_recouple, rem, owed0, fw.enough, open_anchors)) {
            disposition = bit_count_and(&st->open, &st->open)
                ? "complete: what is still owed is within what the firmware leaves open"
                : "complete: every obligation is grounded";
            break;
        }
        if (!fw.combine_clauses && nemit >= 1 && !bit_count_and(&st->open, &st->open)) {
            disposition = "complete: every obligation is grounded";
            break;
        }

        if (step + 1 < budget && can_spend_probes(st, 2) && bit_count_and(&st->open, &st->open) > 0) {
            int emit_occ = -2;
            int before_n = fd.n;
            couple(st, &fd, &sel.id, 1, &emit_occ, R_LAYER, NULL, 0);
            if (st->trace_out)
                fprintf(st->trace_out, "RECOUPLE   step %d -> frontier %d (%+d)\n", step + 1, fd.n, fd.n - before_n);
        }
        disposition = bit_count_and(&st->open, &st->open) ? "open: obligations remain" : "complete: every obligation is grounded";
    }
    if (nemit >= max_emit_eff) disposition = "the emission budget is spent";
    if ((nemit == 0 || nemit < max_emit_eff) && strstr(disposition, "open") == disposition && envelope_hit(st)) {
        st->work.truncated = 1;
        st->work.exhausted_reason = lp_work_exceeded(st->envelope, &st->work);
        disposition = "truncated: compute envelope exhausted";
    } else if (nemit == 0 && envelope_hit(st)) {
        st->work.truncated = 1;
        st->work.exhausted_reason = lp_work_exceeded(st->envelope, &st->work);
        disposition = "truncated: compute envelope exhausted";
    }
    if (st->trace_out) fprintf(st->trace_out, "REALIZE    \"%s\"\n           %s\n", out_buf, disposition);

    {
        lp_id parts[514]; size_t np = 0;
        parts[np++] = st->program_id;
        for (int i = 0; i < nemit; i++) parts[np++] = emitted[i];
        parts[np++] = entity_named(st->c, disposition, NULL, 0, NULL).id;
        lp_id decision_id; lp_id_compose(parts, np, &decision_id);
        out->decision_id = decision_id;
        receipt_realize(req->receipt_out, &fw.id.image_id, &st->program_id, &decision_id, out_buf, disposition);
    }

    if (!req->read_only && outn && (st->pg || conninfo)) {
        Ref resp = text_ref(CTX[0], (const uint8_t *)out_buf, outn);
        Ref lap = text_ref(CTX[0], (const uint8_t *)responder, strlen(responder));
        LoadStats ls = { 0 }; lp_id turn = { 0 };
        if (turn_witness(st->pg, conninfo, who, lap, handle, pr, resp, ordinal, &ls, &turn)) {
            free(out_buf); free(bind); field_free(&fd); reader_free(st->rd);
            if (owns_pg) PQfinish(pg); state_free(st); lp_text_free(c);
            out->exit_code = 1; return 1;
        }
        TABLE_EACH(x) x->keep = 0;
        st->admitted = ls.ent_rows;
        out->response_id = resp.id;
        out->turn_id = turn;
        if (st->trace_out) {
            char ti[33]; lp_id_hex(&turn, ti);
            fprintf(st->trace_out, "WITNESS    turn %s, at %d in the session: %llu entities new, the user's [session, turn] and %s's [response, prompt]\n",
                    ti, ordinal, (unsigned long long)ls.ent_rows, responder);
        }
        receipt_witnessed(req->receipt_out, &turn, &resp.id, ordinal, (unsigned long long)ls.ent_rows);
    } else if (!req->read_only && !outn && (st->pg || conninfo)) {
        File pf; memset(&pf, 0, sizeof pf);
        pf.path = "the prompt"; pf.witness = who; pf.trust = 0.0; pf.trunk = pr;
        LoadStats ls = { 0 };
        int prc = (st->pg && lp_stage_publish_fast(st->pg, NULL, NULL, 0, &pf, 1, &ls) == 0) ? 0
            : (load(conninfo, 1, &pf, 1, &ls) || merge(conninfo, 1, &ls));
        if (prc) {
            free(out_buf); free(bind); field_free(&fd); reader_free(st->rd);
            if (owns_pg) PQfinish(pg); state_free(st); lp_text_free(c);
            out->exit_code = 1; return 1;
        }
        TABLE_EACH(x) x->keep = 0;
        st->admitted = ls.ent_rows;
        if (st->trace_out) fprintf(st->trace_out, "WITNESS    nothing emitted: prompt admitted (%llu entities new)\n", (unsigned long long)ls.ent_rows);
        receipt_unwitnessed(req->receipt_out, NULL, 0, "nothing emitted: nothing to witness");
    } else if (st->trace_out || req->receipt_out) {
        const char *why = req->read_only ? "a read: nothing is witnessed" : "nothing emitted: nothing to witness";
        if (st->trace_out) fprintf(st->trace_out, "WITNESS    %s\n", why);
        receipt_unwitnessed(req->receipt_out, NULL, 0, why);
    }

    st->work.wall_us = (uint64_t)((now() - T) * 1e6);
    out->work = st->work;
    out->text = out_buf;
    out->text_len = outn;
    snprintf(out->disposition, sizeof out->disposition, "%s", disposition);
    if (nemit > 0) {
        out->emitted = malloc(sizeof(lp_id) * (size_t)nemit);
        memcpy(out->emitted, emitted, sizeof(lp_id) * (size_t)nemit);
        out->n_emitted = nemit;
        out->mean_confidence = conf_sum / nemit;
    }

    if (st->trace_out) {
        fprintf(st->trace_out, "\n%llu round trips, %llu for text   total %.1f ms\n",
                (unsigned long long)st->trips, (unsigned long long)reader_trips(st->rd), (now() - T) * 1000);
    }

    field_free(&fd); free(bind); reader_free(st->rd);
    if (owns_pg) PQfinish(pg);
    state_free(st); lp_text_free(c);
    out->exit_code = 0;
    return 0;
}

void lp_forward_result_free(LpForwardResult *out){
    if (!out) return;
    free(out->text);
    free(out->emitted);
    memset(out, 0, sizeof *out);
}

int cmd_turn(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL, *user = getenv("USER"), *session = NULL, *receipt = NULL;
    const char *code_file = NULL, *code_lang = "c", *verify_cmd = NULL;
    int read_only = 0, files = 0, write_back = 0; long long seedv = -1;
    int a = opts(argc, argv, (const Opt[]){
        { .flag = "-d", .kind = 's', .at = &conninfo },
        { .flag = "--firmware", .kind = 's', .at = &fwp },
        { .flag = "--as", .kind = 's', .at = &user },
        { .flag = "--session", .kind = 's', .at = &session },
        { .flag = "--seed", .kind = 'l', .at = &seedv },
        { .flag = "--read", .kind = 'b', .at = &read_only },
        { .flag = "--files", .kind = 'b', .at = &files },
        { .flag = "--receipt", .kind = 's', .at = &receipt },
        { .flag = "--code-file", .kind = 's', .at = &code_file },
        { .flag = "--lang", .kind = 's', .at = &code_lang },
        { .flag = "--verify-cmd", .kind = 's', .at = &verify_cmd },
        { .flag = "--write", .kind = 'b', .at = &write_back },
        { 0 }
    });
    if (a + 1 != argc) {
        fprintf(stderr, "usage: laplace turn [-d conninfo] [--firmware FILE] [--as USER] [--session NAME] [--seed N] [--read] [--files] [--receipt FILE] [--code-file FILE] [--lang LANG] [--verify-cmd CMD] [--write] prompt\n");
        return 2;
    }
    if (!user || !*user) user = "user";
    if (!session) session = "session";
    size_t prompt_n; uint8_t *prompt_text = said_text(argv[a], files, "prompt", &prompt_n);
    if (!prompt_text) return 1;

    Firmware fw = firmware_for(fwp, FW_PULL);
    if (code_file || verify_cmd) {
        char err[512] = "";
        int frc = firmware_ids_checked(&fw, err, sizeof err);
        if (frc) { fputs(err, stderr); free(prompt_text); return frc; }
        LpOodaRequest oreq = {
            .conninfo = conninfo,
            .fw = &fw,
            .prompt = (const char *)prompt_text,
            .code_path = code_file,
            .lang = code_lang,
            .verify_cmd = verify_cmd,
            .session = session,
            .user = user,
            .write_file = write_back,
            .read_only = read_only
        };
        LpOodaResult ores = { 0 };
        int orc = lp_ooda_run(&oreq, &ores);
        printf("OODA       rounds %d   verified %d   %s\n", ores.rounds_run, ores.verified, ores.disposition);
        lp_ooda_result_free(&ores);
        free(prompt_text);
        return orc;
    }
    FILE *rcp = receipt ? fopen(receipt, "wb") : NULL;
    if (receipt && !rcp) { fprintf(stderr, "the receipt %s: %s\n", receipt, strerror(errno)); free(prompt_text); return 1; }
    if (getenv("LAPLACE_TIMES")) setvbuf(stdout, NULL, _IONBF, 0);

    lp_envelope cli_env = lp_envelope_preset(LP_ENVELOPE_STANDARD);
    cli_env.max_wall_us = 10000000;
    if (fw.emit > (int)cli_env.max_tokens_out) cli_env.max_tokens_out = (uint32_t)fw.emit;
    const char *unbounded = getenv("LAPLACE_UNBOUNDED");
    LpForwardRequest req = {
        .conninfo = conninfo,
        .fw = &fw,
        .prompt_text = prompt_text,
        .prompt_len = prompt_n,
        .user = user,
        .session = session,
        .responder = "Laplace",
        .seed_value = seedv,
        .read_only = read_only,
        .temperature = -1.0,
        .envelope = (unbounded && *unbounded) ? NULL : &cli_env,
        .trace_out = stdout,
        .receipt_out = rcp
    };
    LpForwardResult res;
    int rc = lp_forward_execute(&req, &res);
    if (rc && res.error[0]) fputs(res.error, stderr);
    if (rcp) fclose(rcp);
    lp_forward_result_free(&res);
    free(prompt_text);
    return rc;
}
