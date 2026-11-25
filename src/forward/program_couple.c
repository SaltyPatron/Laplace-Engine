/* Forward program: SALIENCE, COUPLE, LAYER, FOLLOWS, SHAPE, WALK, CONTEXT, and CHAIN planes. */
#include "program_internal.h"
#include "laplace/highway.h"
#include <unicode/uchar.h>

/* Rows in content order: a result comes in the order the server's scans met its rows, which another install of the
 * same content meets differently; what is folded from it (a sum, the first of a level set) is folded in this order. */
static const PGresult *ORD_Q; static const int *ORD_C; static int ORD_N;
static int row_cmp(const void *a, const void *b){
    int x = *(const int *)a, y = *(const int *)b;
    for (int k = 0; k < ORD_N; k++) {
        int c = ORD_C[k], lx = PQgetlength(ORD_Q, x, c), ly = PQgetlength(ORD_Q, y, c);
        int d = memcmp(PQgetvalue(ORD_Q, x, c), PQgetvalue(ORD_Q, y, c), (size_t)(lx < ly ? lx : ly));
        if (d) return d;
        if (lx != ly) return lx < ly ? -1 : 1;
    }
    return x < y ? -1 : x > y;
}
static int *rows_in_order(const PGresult *q, const int *cols, int ncols){
    int n = PQntuples(q), *o = malloc(sizeof(int) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) o[i] = i;
    ORD_Q = q; ORD_C = cols; ORD_N = ncols;
    qsort(o, (size_t)n, sizeof(int), row_cmp);
    return o;
}

static void init_composed_words(State *st){
    const lp_layout *fl = lp_flags_map(NULL);
    const lp_field *alpha = fl ? lp_flags_field(fl, "Alphabetic") : NULL;
    const lp_field *gc = fl ? lp_flags_field(fl, "General_Category") : NULL;
    int32_t nd = gc ? lp_flags_value(fl, gc, "Nd") : -1;
    for (int i = 0; i < st->nocc; i++) {
        st->composed[i] = table_find(&st->occ[i]) != NULL;
        if (!st->composed[i]) {
            int64_t cp = lp_tier0_codepoint(T0, &st->occ[i]);
            if (cp >= 0 && ((alpha && lp_flags_get(fl, (uint32_t)cp, alpha)) ||
                            (gc && nd >= 0 && lp_flags_get(fl, (uint32_t)cp, gc) == (uint32_t)nd)))
                st->composed[i] = 1;
        }
    }
}

typedef struct { lp_id target; size_t offset, length; int found; } FirstSegFind;
static void first_seg_cb(void *sink, size_t offset, size_t length, const lp_ref *value){
    FirstSegFind *f = sink;
    if (!f->found && lp_id_eq(&value->id, &f->target)) {
        f->offset = offset;
        f->length = length;
        f->found = 1;
    }
}

void resolve_salience(State *st){
    free(st->salience_rows); st->salience_rows = NULL; st->nsalience = 0;
    st->max_salience = 0.0; st->nspans = 0;
    st->df_ready = 0; st->df_max = 0;
    memset(st->salience, 0, sizeof st->salience);
    memset(st->df, 0, sizeof st->df);
    if (!st->pg || st->nocc <= 0) return;

    init_composed_words(st);

    lp_id q_occ[MAXOCC + 1]; int nq = 0;
    for (int i = 0; i < st->nocc; i++) {
        if (!st->composed[i]) continue;
        int dup = 0;
        for (int k = 0; k < nq && !dup; k++) dup = lp_id_eq(&q_occ[k], &st->occ[i]);
        if (!dup) q_occ[nq++] = st->occ[i];
    }
    if (nq == 0) return;

    int has_lower_var = 0;
    lp_id lower_id = { { 0 } };
    if (st->composed[0] && st->prompt_text && st->prompt_len > 0) {
        FirstSegFind fs = { .target = st->occ[0], .offset = 0, .length = 0, .found = 0 };
        lp_text_segments(st->c, st->prompt_text, st->prompt_len, first_seg_cb, &fs);
        if (fs.found && fs.length > 0 && fs.offset + fs.length <= st->prompt_len) {
            const uint8_t *seg = st->prompt_text + fs.offset;
            size_t pos = 0; uint32_t cp0 = 0;
            if (lp_utf8_next(seg, fs.length, &pos, &cp0) && u_isupper((UChar32)cp0)) {
                int has_lower_tail = 0;
                for (size_t p2 = pos; p2 < fs.length; ) {
                    uint32_t cpk = 0;
                    if (!lp_utf8_next(seg, fs.length, &p2, &cpk)) break;
                    if (u_islower((UChar32)cpk)) { has_lower_tail = 1; break; }
                }
                if (has_lower_tail) {
                    uint8_t low_first[4];
                    size_t lfl = lp_utf8_put((uint32_t)u_tolower((UChar32)cp0), low_first);
                    size_t tail = fs.length - pos;
                    uint8_t *lbuf = malloc(lfl + tail);
                    if (lbuf) {
                        memcpy(lbuf, low_first, lfl);
                        if (tail) memcpy(lbuf + lfl, seg + pos, tail);
                        lower_id = lp_text_decompose(st->c, lbuf, lfl + tail, NULL, NULL).id;
                        free(lbuf);
                        if (!lp_id_eq(&lower_id, &st->occ[0])) {
                            has_lower_var = 1;
                            int dup = 0;
                            for (int k = 0; k < nq && !dup; k++) dup = lp_id_eq(&q_occ[k], &lower_id);
                            if (!dup && nq <= MAXOCC) q_occ[nq++] = lower_id;
                        }
                    }
                }
            }
        }
    }

    int nsal = 0;
    st->work.db_probes++;
    LpSalienceRow *rows = salience_of(st->pg, q_occ, nq,
        st->witness_scoped ? st->witness_scope : NULL,
        st->witness_scoped ? st->nwitness_scope : 0, &nsal);
    st->trips++;
    st->work.rows_scanned += (uint64_t)nsal;
    st->salience_rows = rows;
    st->nsalience = nsal;

    int found_lower = 0; long lower_df = 0; double lower_sal = 0.0;
    for (int r = 0; r < nsal; r++) {
        long row_df = rows[r].df > LONG_MAX ? LONG_MAX : (rows[r].df < 0 ? 0L : (long)rows[r].df);
        if (has_lower_var && lp_id_eq(&rows[r].id, &lower_id)) {
            found_lower = 1;
            lower_df = row_df;
            lower_sal = rows[r].salience;
        }
        for (int i = 0; i < st->nocc; i++) {
            if (!st->composed[i] || !lp_id_eq(&rows[r].id, &st->occ[i])) continue;
            st->salience[i] = rows[r].salience;
            st->df[i] = row_df;
            st->df_ready = 1;
        }
        if (st->fw->multi_tier && rows[r].tier >= 2 && rows[r].span_lift > 0.0 &&
            !lp_id_eq(&rows[r].id, &st->prompt) && st->nspans < MAXOCC) {
            int dup = 0;
            for (int i = 0; i < st->nocc && !dup; i++) dup = lp_id_eq(&st->occ[i], &rows[r].id);
            for (int i = 0; i < st->nspans && !dup; i++) dup = lp_id_eq(&st->spans[i], &rows[r].id);
            if (!dup) {
                memset(&st->span_bits[st->nspans], 0, sizeof(Bits));
                for (int i = 0; i < st->nocc; i++) if (st->composed[i]) bit_set(&st->span_bits[st->nspans], i);
                st->spans[st->nspans++] = rows[r].id;
            }
        }
    }
    if (has_lower_var && found_lower && lower_df > st->df[0]) {
        st->occ0_lower = lower_id;
        st->has_occ0_lower = 1;
        if (lower_sal <= st->salience[0]) {
            st->df[0] = lower_df;
            st->salience[0] = lower_sal;
        }
    }
    for (int i = 0; i < st->nocc; i++) {
        if (!st->composed[i]) continue;
        if (st->df[i] > st->df_max) st->df_max = st->df[i];
        if (st->salience[i] > st->max_salience) st->max_salience = st->salience[i];
    }
    /* Synthesize high-salience composed multi-word spans locally when multi_tier is enabled. */
    if (st->fw->multi_tier && st->max_salience > 0.0) {
        double thresh = st->fw->salience_floor > 0.0 ? st->fw->salience_floor : 0.25 * st->max_salience;
        for (int i = 0; i + 2 < st->nocc && st->nspans < MAXOCC; i++) {
            if (st->composed[i] && !st->composed[i + 1] && st->composed[i + 2] &&
                st->salience[i] >= thresh && st->salience[i + 2] >= thresh) {
                lp_id seg[3] = { st->occ[i], st->occ[i + 1], st->occ[i + 2] };
                lp_id span_id;
                lp_id_compose(seg, 3, &span_id);
                if (!lp_id_eq(&span_id, &st->prompt)) {
                    int dup = 0;
                    for (int k = 0; k < st->nspans && !dup; k++) dup = lp_id_eq(&st->spans[k], &span_id);
                    if (!dup) {
                        memset(&st->span_bits[st->nspans], 0, sizeof(Bits));
                        bit_set(&st->span_bits[st->nspans], i);
                        bit_set(&st->span_bits[st->nspans], i + 2);
                        st->spans[st->nspans++] = span_id;
                    }
                }
            }
        }
        for (int i = 0; i + 4 < st->nocc && st->nspans < MAXOCC; i++) {
            if (st->composed[i] && !st->composed[i + 1] && st->composed[i + 2] &&
                !st->composed[i + 3] && st->composed[i + 4] &&
                st->salience[i] >= thresh && st->salience[i + 4] >= thresh) {
                lp_id seg[5] = { st->occ[i], st->occ[i + 1], st->occ[i + 2], st->occ[i + 3], st->occ[i + 4] };
                lp_id span_id;
                lp_id_compose(seg, 5, &span_id);
                if (!lp_id_eq(&span_id, &st->prompt)) {
                    int dup = 0;
                    for (int k = 0; k < st->nspans && !dup; k++) dup = lp_id_eq(&st->spans[k], &span_id);
                    if (!dup) {
                        memset(&st->span_bits[st->nspans], 0, sizeof(Bits));
                        bit_set(&st->span_bits[st->nspans], i);
                        bit_set(&st->span_bits[st->nspans], i + 2);
                        bit_set(&st->span_bits[st->nspans], i + 4);
                        st->spans[st->nspans++] = span_id;
                    }
                }
            }
        }
    }
}

/* Apply salience-based anchor weighting over composed prompt tokens. */
static void apply_salience_roles(State *st, const double *best_upos_score){
    const Firmware *fw = st->fw;
    if (st->nsalience <= 0 || st->max_salience <= 0.0) return;
    double max_s = st->max_salience;
    int any_upos_anchor = 0;
    for (int i = 0; i < st->nocc; i++) {
        if (st->composed[i] && best_upos_score && best_upos_score[i] > 0.0 && st->role[i] >= 0.75) {
            any_upos_anchor = 1;
            break;
        }
    }
    for (int i = 0; i < st->nocc; i++) {
        if (!st->composed[i]) continue;
        double sn = st->salience[i] / max_s;
        if (sn < 0.0) sn = 0.0;
        if (sn > 1.0) sn = 1.0;
        int has_upos = (best_upos_score && best_upos_score[i] > 0.0);
        if (sn >= 0.50) {
            if (has_upos && st->role[i] < 0.75) {
                /* Respect explicit firmware UPOS role below anchor threshold (e.g. VERB/ADV/PRON/DET/AUX). */
            } else if (!has_upos && any_upos_anchor && st->df_ready && st->df[i] >= 50) {
                double damped = 0.25 * sn;
                if (st->polarity[i] < 0 && damped < 0.35) damped = 0.35;
                if (damped < st->role[i]) st->role[i] = damped;
            } else {
                double lift = 0.75 + 0.25 * sn;
                if (lift > st->role[i]) st->role[i] = lift;
            }
        } else if (st->role_votes[i] == 0 && !has_upos) {
            double damped = st->role[i] * (sn < 0.25 ? (0.10 + 0.50 * sn) : (0.20 + 0.80 * sn));
            if (st->polarity[i] < 0 && damped < 0.35) damped = 0.35;
            st->role[i] = damped;
        } else if (sn < 0.25 && st->salience[i] < fw->salience_floor && st->role[i] <= 0.55) {
            double damped = st->role[i] * (0.10 + 0.50 * sn);
            if (st->polarity[i] < 0 && damped < 0.35) damped = 0.35;
            st->role[i] = damped;
        }
        if (st->role[i] > 0.0) bit_set(&st->open, i);
    }
}

static int is_active_negate_operator(const State *st, int i){
    const Firmware *fw = st->fw;
    int is_neg = 0;
    for (int z = 0; z < fw->nnegate && !is_neg; z++) is_neg = lp_id_eq(&st->occ[i], &fw->id.negate[z]);
    if (!is_neg || !st->composed[i]) return 0;
    for (int k = i + 1; k < st->nocc; k++) {
        for (int s = 0; s < fw->nscope_stop; s++) if (lp_id_eq(&st->occ[k], &fw->id.scope_stop[s])) return 0;
        if (st->composed[k]) return 1;
    }
    return 0;
}

/* Fallback POS weighting directly from claims already fetched in COUPLE (no extra DB trip). */
static void roles_fallback_from_holds(State *st, const Hold *h, int nh, const lp_id *keys, const int *key_to_occ, int nk){
    const Firmware *fw = st->fw;
    const lp_highway *hw = lp_highway_map(NULL);
    const lp_bank *upos_bank = NULL;
    if (hw && fw->role_by[0]) {
        char bn[96]; size_t bi = 0;
        for (; fw->role_by[bi] && bi + 1 < sizeof bn; bi++) {
            unsigned char ch = (unsigned char)fw->role_by[bi];
            bn[bi] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 'a' - 'A') : (char)ch;
        }
        bn[bi] = 0;
        upos_bank = lp_highway_bank(hw, bn);
    }
    double best_score[MAXOCC];
    lp_id best_tag[MAXOCC];
    for (int i = 0; i < st->nocc; i++) best_score[i] = -1.0;

    Ids rn = { 0 };
    if (upos_bank && h && nh > 0) {
        for (int r = 0; r < nh; r++) {
            int s = h[r].src;
            if (!h[r].claim || !h[r].stood || s < 0 || s >= nk) continue;
            int occ = key_to_occ[s];
            if (occ < 0 || occ >= st->nocc || !st->composed[occ] || st->role_votes[occ] > 0) continue;
            path_into(&rn, lp_path_of(h[r].path, (size_t)h[r].path_len));
            if (rn.n < 2) continue;
            int other = lp_tuple_other(rn.v, rn.n, &keys[s]);
            if (other < 0 || lp_highway_slot(hw, upos_bank->list, &rn.v[other]) < 0) continue;
            double sc = lp_confidence(&h[r].r, fw->k) + (double)h[r].matches;
            if (sc > best_score[occ]) { best_score[occ] = sc; best_tag[occ] = rn.v[other]; }
        }
    }
    lp_vec_free(&rn);

    for (int i = 0; i < st->nocc; i++) {
        if (!st->composed[i]) continue;
        if (best_score[i] > 0.0) {
            st->role_read[i] = best_tag[i];
            for (int z = 0; z < fw->nrole; z++) {
                if (lp_id_eq(&best_tag[i], &fw->id.role[z])) { st->role[i] = fw->role[z]; break; }
            }
        }
    }
    apply_salience_roles(st, best_score);
    double max_s = st->max_salience > 0.0 ? st->max_salience : 1.0;
    for (int i = 0; i < st->nocc; i++) {
        if (is_active_negate_operator(st, i)) {
            if (st->role[i] > 0.15) st->role[i] = 0.15;
        } else if (st->polarity[i] < 0 && st->composed[i] && st->role[i] < 0.85 &&
                   !(best_score[i] > 0.0 && st->role[i] < 0.75) &&
                   (best_score[i] > 0.0 || st->salience[i] >= 0.25 * max_s)) {
            st->role[i] = 0.85;
            bit_set(&st->open, i);
        }
    }
}

void resolve_roles(State *st){
    Firmware *fw = (Firmware *)st->fw;
    init_composed_words(st);
    for (int i = 0; i < st->nocc; i++) {
        st->role[i] = fw->role_by[0] ? 0.5 : 1.0;
        if (!st->composed[i]) st->role[i] = 0.0;
    }
    memset(&st->open, 0, sizeof st->open);
    for (int i = 0; i < st->nocc; i++) if (st->composed[i] && st->role[i] > 0.0) bit_set(&st->open, i);
    apply_salience_roles(st, NULL);
}

void resolve_polarity(State *st){
    const Firmware *fw = st->fw;
    for (int i = 0; i < st->nocc; i++) st->polarity[i] = 1;
    for (int i = 0; i < st->nocc; i++) {
        int is_neg = 0;
        for (int z = 0; z < fw->nnegate && !is_neg; z++) is_neg = lp_id_eq(&st->occ[i], &fw->id.negate[z]);
        if (!is_neg) continue;
        st->polarity[i] = -1;
        if (st->composed[i] && st->role[i] > 0.0) {
            if (st->role[i] < 0.35) st->role[i] = 0.35;
            bit_set(&st->open, i);
        }
        int scoped = 0, best_k = -1;
        double best_sc = -1.0;
        for (int k = i + 1; k < st->nocc && scoped < 3; k++) {
            int stop = 0;
            for (int s = 0; s < fw->nscope_stop && !stop; s++) stop = lp_id_eq(&st->occ[k], &fw->id.scope_stop[s]);
            if (stop) break;
            if (st->composed[k]) {
                st->polarity[k] = -1;
                scoped++;
                double sc = st->salience[k] > 0.0 ? st->salience[k] : st->role[k];
                if (best_k < 0 || sc > best_sc) { best_k = k; best_sc = sc; }
            }
        }
        if (scoped > 0 && st->composed[i]) { st->role[i] = 0.15; bit_set(&st->open, i); }
        if (best_k >= 0 && st->role[best_k] < 0.85) { st->role[best_k] = 0.85; bit_set(&st->open, best_k); }
    }
}

static int is_neg_rel(const State *st, const lp_id *rel){
    const Firmware *fw = st->fw;
    for (int i = 0; i < fw->nnegate_relation; i++) if (lp_id_eq(&fw->id.negate_relation[i], rel)) return 1;
    return 0;
}

void contextual_salience_lift(State *st, const Field *fd){
    double max_s = st->max_salience > 0.0 ? st->max_salience : 1.0;
    for (int i = 0; i < st->nocc; i++) {
        if (!st->composed[i] || st->role[i] <= 0.0 || st->role[i] >= 0.75) continue;
        int multi = 0;
        for (int z = 0; z < fd->n && !multi; z++) {
            const Cell *x = &fd->c[z];
            if (x->segment && x->tier >= 2 && ((x->support.w[i >> 6] >> (i & 63)) & 1) &&
                bit_count_and(&x->support, &x->support) >= 2)
                multi = 1;
        }
        int disc = st->ctx_n[i] > 0 && !st->ctx_hub[i] && st->ctx_total > 0 &&
                   ((double)st->ctx_total / (double)st->ctx_n[i]) >= st->fw->lift;
        int sal = st->nsalience > 0 && (st->salience[i] / max_s) >= 0.40;
        if (multi || disc || sal || st->polarity[i] < 0) {
            double lifted = st->role[i] + 0.35 * (1.0 - st->role[i]);
            if (lifted > st->role[i]) { st->role[i] = lifted; bit_set(&st->open, i); }
        }
    }
}

static int curve_order(const void *a, const void *b){
    const Curve *x = a, *y = b;
    return x->d < y->d ? -1 : x->d > y->d ? 1 : memcmp(&x->id, &y->id, 16);
}
static double shape_measure(int shape, double shape_n, const double *a, size_t na, const double *b, size_t nb){
    switch (shape) {
    case FW_OUTLIERS: return lp_frechet4_outliers(a, na, b, nb, (unsigned)shape_n);
    case FW_DTW: { size_t s; double d = lp_dtw4(a, na, b, nb, &s); return s ? d / (double)s : d; }
    case FW_EDR: return (double)lp_edr4(a, na, b, nb, shape_n);
    default: return lp_frechet4(a, na, b, nb);
    }
}

/* Query-centroid salience weighting: keeps stored entity.coord deterministic while focusing GiST nomination on salient tokens. */
int shape_measured(State *st, const lp_id *ids, int n, const int *occ_of, const Hold *h0, int nh0, Curve *near, int keep){
    if (n < 2 || keep <= 0) return 0;
    const Firmware *fw = st->fw;
    const lp_layout *fl = lp_flags_map(NULL);
    const lp_field *alpha = fl ? lp_flags_field(fl, "Alphabetic") : NULL;
    const lp_field *gc = fl ? lp_flags_field(fl, "General_Category") : NULL;
    int32_t nd = gc ? lp_flags_value(fl, gc, "Nd") : -1;

    Hold *h = malloc(sizeof(Hold) * (size_t)(nh0 + fw->fan + 1));
    if (nh0 > 0) memcpy(h, h0, sizeof(Hold) * (size_t)nh0);
    int nh = nh0;
    int obs_in_h0 = 0;
    for (int i = 0; i < nh0; i++) {
        if (!h[i].claim && !lp_id_eq(&h[i].entity, &st->prompt) &&
            !lp_is_internal_metadata_path(st->c, h[i].path, (size_t)h[i].path_len))
            obs_in_h0++;
    }
    int allow_knn = fw->nearest > 0 && obs_in_h0 < 4 && !envelope_hit(st);
    if (allow_knn) {
        int knn_k = (st->envelope && fw->nearest > 64) ? 64 : fw->nearest;
        double *pc = malloc(sizeof(double) * 4 * (size_t)n);
        uint8_t *hc = malloc((size_t)n);
        coords_of(st->pg, ids, n, pc, hc);
        st->trips++; st->work.db_probes++;
        double cen[4] = { 0 }, raw_cen[4] = { 0 }, wsum = 0.0;
        int m = 0;
        for (int k = 0; k < n; k++) {
            if (!hc[k]) continue;
            m++;
            for (int d = 0; d < 4; d++) raw_cen[d] += pc[4 * k + d];
            double w = 0.0;
            int occ = occ_of ? occ_of[k] : -1;
            if (occ < 0) {
                for (int j = 0; j < st->nocc; j++) if (lp_id_eq(&ids[k], &st->occ[j])) { occ = j; break; }
            }
            if (occ >= 0 && occ < st->nocc) {
                if (!st->composed[occ]) w = 0.0;
                else if (st->nsalience > 0 && st->salience[occ] > 0.0) w = st->salience[occ];
                else w = st->role[occ] > 0.0 ? st->role[occ] : 0.1;
            } else {
                int64_t cp = lp_tier0_codepoint(T0, &ids[k]);
                if (cp < 0 || (alpha && lp_flags_get(fl, (uint32_t)cp, alpha)) ||
                    (gc && nd >= 0 && lp_flags_get(fl, (uint32_t)cp, gc) == (uint32_t)nd))
                    w = 1.0;
            }
            if (w > 0.0) { for (int d = 0; d < 4; d++) cen[d] += w * pc[4 * k + d]; wsum += w; }
        }
        if (m >= 2) {
            if (wsum > 0.0) { for (int d = 0; d < 4; d++) cen[d] /= wsum; }
            else { for (int d = 0; d < 4; d++) cen[d] = raw_cen[d] / m; }
            uint8_t pt[64]; size_t pl = lp_ewkb_point4(cen, pt, sizeof pt);
            Args a = { 0 }; arg_raw(&a, pt, pl); arg_i32(&a, (int32_t)knn_k);
            PGresult *q = call_st(st, LP_SQL_SHAPE_NEAREST_ENTITIES, &a);
            int nq = PQntuples(q);
            lp_id *gid = malloc(sizeof(lp_id) * (size_t)(nq ? nq : 1));
            for (int r = 0; r < nq; r++) memcpy(gid[r].b, PQgetvalue(q, r, 0), 16);
            PQclear(q); args_free(&a);
            int ng = 0; Hold *g = nq ? paths_of(st->pg, gid, nq, &ng) : NULL;
            if (nq) { st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)ng; }
            for (int i = 0; i < ng && nh < nh0 + fw->fan + 1; i++) h[nh++] = g[i];
            free(g); free(gid);
        }
        free(pc); free(hc);
    }

    lp_idmap *seen = lp_idmap_new();
    int *pick = malloc(sizeof(int) * (size_t)(nh ? nh : 1)), np_ = 0;
    lp_idmap *verts = lp_idmap_new();
    int max_cand = (st->envelope && fw->nearest > 0) ? fw->nearest : 256;
    for (int i = 0; i < nh && np_ < max_cand; i++) {
        if (h[i].claim || lp_id_eq(&h[i].entity, &st->prompt) ||
            lp_is_internal_metadata_path(st->c, h[i].path, (size_t)h[i].path_len))
            continue;
        bool f; lp_idmap_put(seen, &h[i].entity, &f);
        if (f) pick[np_++] = i;
    }
    if (np_ == 0) {
        for (int i = nh0; i < nh; i++) free(h[i].path);
        free(h); free(pick);
        lp_idmap_free(seen); lp_idmap_free(verts);
        return 0;
    }
    lp_id **cv = malloc(sizeof(lp_id *) * (size_t)np_);
    int *cn = malloc(sizeof(int) * (size_t)np_);
    for (int c = 0; c < np_; c++) {
        const Hold *x = &h[pick[c]];
        size_t nv = lp_path_ids(x->path, (size_t)x->path_len, NULL, 0);
        cv[c] = malloc(sizeof(lp_id) * (nv ? nv : 1));
        cn[c] = (int)lp_path_ids(x->path, (size_t)x->path_len, cv[c], nv);
        for (int k = 0; k < cn[c]; k++) { bool f; lp_idmap_put(verts, &cv[c][k], &f); }
    }
    for (int k = 0; k < n; k++) { bool f; lp_idmap_put(verts, &ids[k], &f); }
    size_t nv = lp_idmap_count(verts);
    double *xyz = malloc(sizeof(double) * 4 * (nv ? nv : 1));
    uint8_t *has = malloc(nv ? nv : 1);
    coords_of(st->pg, lp_idmap_keys(verts), (int)nv, xyz, has);
    if (nv > 0) { st->trips++; st->work.db_probes++; }
    double *pa = malloc(sizeof(double) * 4 * (size_t)n);
    size_t npa = 0;
    for (int k = 0; k < n; k++) {
        int64_t q = lp_idmap_find(verts, &ids[k]);
        if (q >= 0 && has[q]) { memcpy(pa + 4 * npa, xyz + 4 * q, 32); npa++; }
    }
    Curve *all = malloc(sizeof(Curve) * (size_t)(np_ ? np_ : 1));
    int na = 0;
    double *b = malloc(sizeof(double) * 4 * 4096);
    for (int c = 0; c < np_ && npa >= 2; c++) {
        int m = 0;
        for (int k = 0; k < cn[c] && m < 4096; k++) {
            int64_t q = lp_idmap_find(verts, &cv[c][k]);
            if (q >= 0 && has[q]) { memcpy(b + 4 * m, xyz + 4 * q, 32); m++; }
        }
        if (m < 2) continue;
        all[na].id = h[pick[c]].entity;
        all[na].d = shape_measure(fw->shape, fw->shape_n, pa, npa, b, (size_t)m);
        all[na].v = cv[c]; all[na].nv = cn[c]; cv[c] = NULL; na++;
    }
    qsort(all, (size_t)na, sizeof(Curve), curve_order);
    int k = 0;
    for (int i = 0; i < na; i++) {
        if (k < keep && isfinite(all[i].d)) near[k++] = all[i];
        else free(all[i].v);
    }
    for (int c = 0; c < np_; c++) free(cv[c]);
    for (int i = nh0; i < nh; i++) free(h[i].path);
    free(h); free(cv); free(cn); free(all); free(b); free(pa); free(xyz); free(has); free(pick);
    lp_idmap_free(seen); lp_idmap_free(verts);
    return k;
}

static void head_note(State *st, Cell *x, const lp_id *rel, double conf, int occ){
    int have = 0; for (int k = 0; k < x->nheads && k < CELL_HEADS; k++) have |= lp_id_eq(&x->heads[k], rel);
    if (!have) { if (x->nheads < CELL_HEADS) x->heads[x->nheads] = *rel; x->nheads++; }
    if (!st->heads) st->heads = lp_idmap_sized(sizeof(HeadStat));
    bool f; HeadStat *s = lp_idmap_at(st->heads, lp_idmap_put(st->heads, rel, &f));
    if (f) memset(s, 0, sizeof *s);
    s->strands++; s->force += conf;
    if (occ >= 0) bit_set(&s->support, occ);
}

static Bits LAYER_ANCHORS;

static int by_force_layer(const void *a, const void *b){
    const Cell *x = *(const Cell *const *)a, *y = *(const Cell *const *)b;
    int ax = bit_count_and(&x->support, &LAYER_ANCHORS) + bit_count_and(&x->neg_support, &LAYER_ANCHORS);
    int ay = bit_count_and(&y->support, &LAYER_ANCHORS) + bit_count_and(&y->neg_support, &LAYER_ANCHORS);
    if (ax != ay) return ax > ay ? -1 : 1;
    if (x->force != y->force) return x->force > y->force ? -1 : 1;
    return memcmp(&x->id, &y->id, 16);
}

int layer_round(State *st, Field *fd, int round, lp_idmap *heads_out){
    const Firmware *fw = st->fw;
    const lp_highway *hw = lp_highway_map(NULL);
    memset(&LAYER_ANCHORS, 0, sizeof LAYER_ANCHORS);
    for (int i = 0; i < st->nocc; i++) {
        if (!st->composed[i] || !((st->open.w[i >> 6] >> (i & 63)) & 1)) continue;
        if (st->role[i] >= 0.75) bit_set(&LAYER_ANCHORS, i);
    }
    if (!bit_count_and(&LAYER_ANCHORS, &LAYER_ANCHORS)) {
        for (int i = 0; i < st->nocc; i++) {
            if (st->composed[i] && ((st->open.w[i >> 6] >> (i & 63)) & 1))
                bit_set(&LAYER_ANCHORS, i);
        }
    }
    Cell **c = malloc(sizeof(Cell *) * (size_t)(fd->n ? fd->n : 1));
    int m = 0;
    for (int z = 0; z < fd->n; z++) {
        Cell *x = &fd->c[z];
        if (x->layer != round - 1 || x->hub || !(x->force > 0) ||
            (x->segment && !x->bridge && !x->routes[R_CLAIM] && !x->routes[R_LAYER] &&
             (bit_count_and(&x->support, &LAYER_ANCHORS) + bit_count_and(&x->neg_support, &LAYER_ANCHORS)) < 2)) continue;
        int own = 0; for (int i = 0; i < st->nocc && !own; i++) own = lp_id_eq(&st->occ[i], &x->id);
        if (own || lp_tier0_codepoint(T0, &x->id) >= 0) continue;
        c[m++] = x;
    }
    if (!m) { free(c); return 0; }
    qsort(c, (size_t)m, sizeof(Cell *), by_force_layer);
    if (m > fw->breadth) m = fw->breadth;
    if (st->envelope && st->envelope->preset <= LP_ENVELOPE_STANDARD && m > 32) m = 32;
    lp_id *keys = malloc(sizeof(lp_id) * (size_t)m);
    lp_id *vias = malloc(sizeof(lp_id) * (size_t)m);
    Bits *sup = malloc(sizeof(Bits) * (size_t)m), *nsup = malloc(sizeof(Bits) * (size_t)m);
    double *fo = malloc(sizeof(double) * (size_t)m);
    for (int i = 0; i < m; i++) { keys[i] = c[i]->id; vias[i] = c[i]->via; sup[i] = c[i]->support; nsup[i] = c[i]->neg_support; fo[i] = c[i]->force; }
    free(c);

    int *hub = malloc(sizeof(int) * 2 * (size_t)m), nh = 0;
    Hold *h = holds_capped_scoped(st->pg, keys, m, fw->fan, 1, 1,
        st->witness_scope, st->nwitness_scope, st->witness_scoped, hub, &nh);
    st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nh;
    int *outdeg = calloc((size_t)m, sizeof(int));
    Ids rn = { 0 };
    for (int r = 0; r < nh; r++) { int s = h[r].src; if (s >= 0 && s < m && h[r].claim && h[r].stood) outdeg[s]++; }
    int fresh_n = 0;
    for (int r = 0; r < nh; r++) {
        int s = h[r].src;
        if (s < 0 || s >= m || hub[2 * s] || !h[r].claim || !h[r].stood) continue;
        if (lp_is_internal_metadata_path(st->c, h[r].path, (size_t)h[r].path_len)) continue;
        path_into(&rn, lp_path_of(h[r].path, (size_t)h[r].path_len));
        if (rn.n < 2 || !head_read(fw, rn.v, rn.n)) continue;
        if (rn.n == 2 && (lp_id_eq(&rn.v[0], &st->handle) || lp_id_eq(&rn.v[1], &st->handle) ||
                          lp_id_eq(&rn.v[0], &st->prompt) || lp_id_eq(&rn.v[1], &st->prompt))) continue;
        int other = lp_tuple_other(rn.v, rn.n, &keys[s]);
        if (other < 0 || lp_id_eq(&rn.v[other], &keys[s]) || lp_id_eq(&rn.v[other], &st->prompt) || lp_id_eq(&rn.v[other], &st->handle) || lp_id_eq(&rn.v[other], &vias[s])) continue;
        lp_id rel = rn.n >= 3 ? rn.v[1] : keys[s];
        int flip = 0;
        for (size_t p = 1; p + 1 < rn.n && !flip; p++) flip = is_neg_rel(st, &rn.v[p]);
        double conf = lp_confidence(&h[r].r, fw->k) * strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n) * fo[s] / (outdeg[s] ? outdeg[s] : 1);
        int before = fd->n; Cell *x = cell(fd, &rn.v[other]);
        int is_new = (fd->n > before);
        if (!is_new && x->segment && !x->bridge && !x->routes[R_CLAIM] && x->layer == 0) continue;
        int is_sense_edge = (rn.n >= 3 && is_lexical_sense_rel(st, &rel));
        if (is_sense_edge && (size_t)other == rn.n - 1) x->bridge = 1;
        else if (is_sense_edge && other == 0) x->bridge = 0;
        int prev_anc = is_new ? 0 : bit_count_and(&x->support, &LAYER_ANCHORS);
        if (is_new) { x->layer = round; fresh_n++; }
        if (lp_tier0_codepoint(T0, &rn.v[other]) < 0 && !(hw && lp_highway_bank_of(hw, &rn.v[other], NULL)) && h[r].tier > x->tier)
            x->tier = h[r].tier;
        x->force += conf; x->routes[R_LAYER]++;
        for (int w = 0; w < MAXOCC / 64; w++) {
            x->support.w[w] |= (flip ? nsup[s].w[w] : sup[s].w[w]);
            x->neg_support.w[w] |= (flip ? sup[s].w[w] : nsup[s].w[w]);
        }
        if (!is_new && x->layer < round && bit_count_and(&x->support, &LAYER_ANCHORS) > prev_anc) {
            x->layer = round;
            fresh_n++;
        }
        int have = 0; for (int k = 0; k < x->nheads && k < CELL_HEADS; k++) have |= lp_id_eq(&x->heads[k], &rel);
        if (!have) { if (x->nheads < CELL_HEADS) x->heads[x->nheads] = rel; x->nheads++; }
        if (heads_out) {
            bool f; HeadStat *hs = lp_idmap_at(heads_out, lp_idmap_put(heads_out, &rel, &f));
            if (f) memset(hs, 0, sizeof *hs);
            hs->strands++; hs->force += conf;
            for (int w = 0; w < MAXOCC / 64; w++) hs->support.w[w] |= sup[s].w[w] | nsup[s].w[w];
        }
        if (!x->has_r) { x->r = h[r].r; x->has_r = 1; x->via = keys[s]; x->rel = rel; }

        /* Propagate interior arguments on N-ary strands (np >= 4). */
        for (size_t p = 0; p < rn.n; p++) {
            if ((int)p == other || p == 1 || lp_id_eq(&rn.v[p], &keys[s]) || lp_id_eq(&rn.v[p], &st->prompt)) continue;
            if (hw && lp_highway_bank_of(hw, &rn.v[p], NULL)) continue;
            int before2 = fd->n; Cell *xa = cell(fd, &rn.v[p]);
            if (fd->n > before2) { xa->layer = round; fresh_n++; }
            xa->force += 0.75 * conf; xa->routes[R_LAYER]++;
            for (int w = 0; w < MAXOCC / 64; w++) {
                xa->support.w[w] |= (flip ? nsup[s].w[w] : sup[s].w[w]);
                xa->neg_support.w[w] |= (flip ? sup[s].w[w] : nsup[s].w[w]);
            }
            if (!xa->has_r) { xa->r = h[r].r; xa->has_r = 1; xa->via = keys[s]; xa->rel = rel; }
        }
    }
    holds_free(h, nh); free(hub); free(outdeg); free(keys); free(vias); free(sup); free(nsup); free(fo); lp_vec_free(&rn);
    return fresh_n;
}

void heads_say(State *st, lp_idmap *hm, const char *label){
    size_t nhd = lp_idmap_count(hm);
    int *ord = malloc(sizeof(int) * (nhd ? nhd : 1));
    for (size_t i = 0; i < nhd; i++) ord[i] = (int)i;
    for (size_t i = 1; i < nhd; i++) {
        int v = ord[i]; size_t j = i; const HeadStat *a = lp_idmap_at(hm, (size_t)v);
        while (j > 0) {
            const HeadStat *b = lp_idmap_at(hm, (size_t)ord[j - 1]);
            if (b->force > a->force || (b->force == a->force && memcmp(lp_idmap_key(hm, (size_t)ord[j - 1]), lp_idmap_key(hm, (size_t)v), 16) < 0)) break;
            ord[j] = ord[j - 1]; j--;
        }
        ord[j] = v;
    }
    printf("           %s: %zu relations respond; the strongest:", label, nhd);
    lp_id pkeys[8]; size_t npkeys = nhd < 8 ? nhd : 8;
    for (size_t i = 0; i < npkeys; i++) pkeys[i] = *lp_idmap_key(hm, (size_t)ord[i]);
    if (npkeys) { uint64_t b = reader_trips(st->rd); reader_prefetch(st->rd, pkeys, npkeys, 24); st->trips += reader_trips(st->rd) - b; }
    for (size_t i = 0; i < npkeys; i++) {
        const HeadStat *a = lp_idmap_at(hm, (size_t)ord[i]);
        char *tn = reader_text(st->rd, &pkeys[i], 24);
        printf(" %s(%ld strands, %d words, %.2f)", tn, a->strands, bit_count_and(&a->support, &a->support), a->force);
        free(tn);
    }
    printf("\n"); free(ord);
}

static void context_fill(State *st, int from, int to, const int *at, int nk, const Hold *h, int nh, const int *hub, const lp_id *keys){
    if (!st->ctx_obs) st->ctx_obs = lp_idmap_new();
    if (!st->ctx_all) st->ctx_all = lp_idmap_new();
    lp_idmap *obs = st->ctx_obs; Ids ids = { 0 };
    lp_idmap **km = calloc((size_t)(nk ? nk : 1), sizeof *km);
    int *kn = calloc((size_t)(nk ? nk : 1), sizeof(int));
    for (int k = 0; k < nk; k++) {
        int active = 0; for (int i = from; i < to && !active; i++) active = (at[i - from] == k);
        if (active && !hub[2 * k + 1]) km[k] = lp_idmap_new();
    }
    for (int z = 0; z < nh; z++) {
        int k = h[z].src; if (k < 0 || k >= nk || !km[k] || h[z].claim) continue;
        if (lp_is_internal_metadata_path(st->c, h[z].path, (size_t)h[z].path_len)) continue;
        kn[k]++;
        bool once; lp_idmap_put(obs, &h[z].entity, &once);
        size_t nv = lp_path_ids(h[z].path, (size_t)h[z].path_len, NULL, 0);
        lp_vec_reserve(&ids, nv ? nv : 1);
        nv = lp_path_ids(h[z].path, (size_t)h[z].path_len, ids.v, nv);
        lp_idmap *here = lp_idmap_new();
        for (size_t v = 0; v < nv; v++) {
            bool f1; lp_idmap_put(here, &ids.v[v], &f1); if (!f1) continue;
            bool f; (*lp_idmap_value(km[k], lp_idmap_put(km[k], &ids.v[v], &f)))++;
            if (once) (*lp_idmap_value(st->ctx_all, lp_idmap_put(st->ctx_all, &ids.v[v], &f)))++;
        }
        lp_idmap_free(here); if (once) st->ctx_total++;
    }
    for (int i = from; i < to; i++) {
        int k = at[i - from]; if (k < 0) continue;
        if (hub[2 * k + 1]) { st->ctx_hub[i] = 1; continue; }
        st->ctx[i] = km[k]; st->ctx_n[i] = kn[k];
    }
    free(km); free(kn); lp_vec_free(&ids); (void)keys;
}

/* Fold a pre-fetched Hold[] batch directly into Field + Context + Roles + Walk step-0 adjacency. */
void couple_ingest_holds(State *st, Field *fd, const Hold *h, int nh, const int *hub, const lp_id *keys, const int *of, int nk, const int *occ_of, int kind){
    const Firmware *fw = st->fw;
    const lp_highway *hw = lp_highway_map(NULL);
    Ids rn = { 0 };
    int key_to_occ[MAXOCC];
    for (int k = 0; k < nk && k < MAXOCC; k++) key_to_occ[k] = -1;

    if (occ_of && nk > 0) {
        for (int s = 0; s < nk; s++) {
            int o = of[s], occ = occ_of[o];
            if (occ >= 0 && occ < st->nocc) key_to_occ[s] = occ;
            else if (occ == -1000) key_to_occ[s] = 0;
        }
        roles_fallback_from_holds(st, h, nh, keys, key_to_occ, nk);
    }

    for (int i = 0; i < nh; i++) {
        int s = h[i].src; if (s < 0 || s >= nk) continue;
        int o = of[s];
        int raw_occ = occ_of ? occ_of[o] : -1;
        if (raw_occ == -1000) continue;
        int occ = raw_occ >= 0 ? raw_occ : -1;
        int eff_kind = (raw_occ == -2) ? R_DISCOURSE : kind;
        Bits span_sup = { 0 };
        if (occ < 0 && raw_occ != -2) {
            if (raw_occ == -1 && occ_of && lp_id_eq(&keys[s], &st->prompt)) {
                for (int j = 0; j < st->nocc; j++) if (st->composed[j]) bit_set(&span_sup, j);
            } else if (raw_occ <= -3) {
                int si = -raw_occ - 3;
                if (si >= 0 && si < st->nspans) span_sup = st->span_bits[si];
            }
        }
        double pull = (occ >= 0 && occ < st->nocc) ? st->role[occ] : (eff_kind == R_CLAIM ? 1.0 : 0.5);
        if (lp_id_eq(&h[i].entity, &st->prompt)) continue;
        if (lp_is_internal_metadata_path(st->c, h[i].path, (size_t)h[i].path_len)) continue;
        if (h[i].claim) {
            if (!h[i].stood) continue;
            path_into(&rn, lp_path_of(h[i].path, (size_t)h[i].path_len));
            if (rn.n < 2 || !head_read(fw, rn.v, rn.n)) { st->refused += !head_read(fw, rn.v, rn.n); continue; }
            if (rn.n == 2 && (lp_id_eq(&rn.v[0], &st->handle) || lp_id_eq(&rn.v[1], &st->handle) ||
                              lp_id_eq(&rn.v[0], &st->prompt) || lp_id_eq(&rn.v[1], &st->prompt))) continue;
            int other = lp_tuple_other(rn.v, rn.n, &keys[s]);
            if (other < 0 || lp_id_eq(&rn.v[other], &st->prompt) || lp_id_eq(&rn.v[other], &st->handle)) continue;
            lp_id rel = rn.n >= 3 ? rn.v[1] : keys[s];
            int flip_occ = 0;
            for (size_t p = 1; p + 1 < rn.n && !flip_occ; p++) flip_occ = is_neg_rel(st, &rn.v[p]);
            double conf = lp_confidence(&h[i].r, fw->k) * strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n);
            Cell *x = cell(fd, &rn.v[other]);
            int is_sense_edge = (rn.n >= 3 && is_lexical_sense_rel(st, &rel));
            if (is_sense_edge && (size_t)other == rn.n - 1) x->bridge = 1;
            else if (is_sense_edge && other == 0) x->bridge = 0;
            x->force += conf * pull;
            x->routes[eff_kind]++;
            if (lp_tier0_codepoint(T0, &rn.v[other]) < 0 && !(hw && lp_highway_bank_of(hw, &rn.v[other], NULL)) && h[i].tier > x->tier)
                x->tier = h[i].tier;
            if (occ >= 0) {
                int neg = (occ < st->nocc && st->polarity[occ] < 0) ^ flip_occ;
                if (neg) bit_set(&x->neg_support, occ); else bit_set(&x->support, occ);
            } else if (bit_count_and(&span_sup, &span_sup) > 0) {
                Bits *dst = flip_occ ? &x->neg_support : &x->support;
                for (int w = 0; w < MAXOCC / 64; w++) dst->w[w] |= span_sup.w[w];
            }
            if (pull > 0.0) head_note(st, x, &rel, conf * pull, occ);
            if (!x->has_r || conf > lp_confidence(&x->r, fw->k)) { x->r = h[i].r; x->has_r = 1; x->via = keys[s]; x->rel = rel; }

            /* Propagate N-ary (np >= 4) interior arguments. */
            for (size_t p = 0; p < rn.n; p++) {
                if ((int)p == other || p == 1 || lp_id_eq(&rn.v[p], &keys[s]) || lp_id_eq(&rn.v[p], &st->prompt)) continue;
                if (hw && lp_highway_bank_of(hw, &rn.v[p], NULL)) continue;
                Cell *xa = cell(fd, &rn.v[p]);
                xa->force += 0.75 * conf * pull; xa->routes[eff_kind]++;
                if (lp_tier0_codepoint(T0, &rn.v[p]) < 0 && h[i].tier > xa->tier) xa->tier = h[i].tier;
                if (occ >= 0) {
                    int neg = (occ < st->nocc && st->polarity[occ] < 0) ^ flip_occ;
                    if (neg) bit_set(&xa->neg_support, occ); else bit_set(&xa->support, occ);
                } else if (bit_count_and(&span_sup, &span_sup) > 0) {
                    Bits *dst = flip_occ ? &xa->neg_support : &xa->support;
                    for (int w = 0; w < MAXOCC / 64; w++) dst->w[w] |= span_sup.w[w];
                }
                if (!xa->has_r || conf > lp_confidence(&xa->r, fw->k)) { xa->r = h[i].r; xa->has_r = 1; xa->via = keys[s]; xa->rel = rel; }
            }
        } else {
            Cell *x = cell(fd, &h[i].entity);
            x->segment = 1; x->tier = h[i].tier;
            x->force += pull; x->routes[R_CONTAIN]++;
            if (occ >= 0) {
                if (occ < st->nocc && st->polarity[occ] < 0) bit_set(&x->neg_support, occ);
                else bit_set(&x->support, occ);
            } else if (bit_count_and(&span_sup, &span_sup) > 0) {
                for (int w = 0; w < MAXOCC / 64; w++) x->support.w[w] |= span_sup.w[w];
            }
            if (fw->multi_tier && h[i].tier >= 2) {
                size_t nv = lp_path_ids(h[i].path, (size_t)h[i].path_len, NULL, 0);
                lp_vec_reserve(&rn, nv ? nv : 1);
                nv = lp_path_ids(h[i].path, (size_t)h[i].path_len, rn.v, nv);
                for (int j = 0; j < st->nocc; j++) {
                    if (!st->composed[j]) continue;
                    for (size_t q = 0; q < nv; q++) {
                        if (lp_id_eq(&rn.v[q], &st->occ[j])) {
                            if (st->polarity[j] < 0) bit_set(&x->neg_support, j);
                            else bit_set(&x->support, j);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (fw->nup) {
        lp_id hk[MAXOCC]; int hof[MAXOCC], nhk = 0;
        for (int k = 0; k < nk && nhk < MAXOCC; k++) {
            if (hub[2 * k] && lp_tier0_codepoint(T0, &keys[k]) < 0) { hk[nhk] = keys[k]; hof[nhk++] = of[k]; }
        }
        for (int u = 0; u < fw->nup && nhk; u++) {
            int n2 = 0;
            Hold *h2 = holds_both_scoped(st->pg, hk, nhk, &fw->id.up[u],
                st->witness_scope, st->nwitness_scope, st->witness_scoped, &n2);
            st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)n2;
            for (int i = 0; i < n2; i++) {
                int s = h2[i].src; if (s < 0 || s >= nhk || !h2[i].claim || !h2[i].stood) continue;
                int o = hof[s], occ = occ_of ? occ_of[o] : -1;
                int eff_kind = (occ == -2) ? R_DISCOURSE : kind;
                double pull = (occ >= 0 && occ < st->nocc) ? st->role[occ] : (eff_kind == R_CLAIM ? 1.0 : 0.5);
                path_into(&rn, lp_path_of(h2[i].path, (size_t)h2[i].path_len));
                if (rn.n < 2 || !head_read(fw, rn.v, rn.n)) continue;
                if (rn.n == 2 && (lp_id_eq(&rn.v[0], &st->handle) || lp_id_eq(&rn.v[1], &st->prompt))) continue;
                int other = lp_tuple_other(rn.v, rn.n, &hk[s]); if (other < 0) continue;
                lp_rating rt = h2[i].r; lp_id rel = rn.n >= 3 ? rn.v[1] : hk[s];
                double conf = lp_confidence(&rt, fw->k) * strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n);
                Cell *x = cell(fd, &rn.v[other]);
                int is_sense_edge = (rn.n >= 3 && is_lexical_sense_rel(st, &rel));
                if (is_sense_edge && (size_t)other == rn.n - 1) x->bridge = 1;
                else if (is_sense_edge && other == 0) x->bridge = 0;
                x->force += conf * pull; x->routes[eff_kind]++;
                if (occ >= 0) {
                    int neg = (occ < st->nocc && st->polarity[occ] < 0) ^ is_neg_rel(st, &rel);
                    if (neg) bit_set(&x->neg_support, occ); else bit_set(&x->support, occ);
                }
                if (pull > 0.0) head_note(st, x, &rel, conf * pull, occ);
                if (!x->has_r) { x->r = rt; x->has_r = 1; x->via = hk[s]; x->rel = rel; }
            }
            holds_free(h2, n2);
        }
    }
    lp_vec_free(&rn);

    if (occ_of && nk > 0) {
        int at[MAXOCC]; for (int i = 0; i < st->nocc; i++) at[i] = -1;
        for (int i = 0; i < st->nocc; i++) {
            if (!st->composed[i] || st->role[i] <= 0.0) continue;
            for (int k = 0; k < nk; k++) if (lp_id_eq(&keys[k], &st->occ[i])) { at[i] = k; break; }
        }
        context_fill(st, 0, st->nocc, at, nk, h, nh, hub, keys);
        walk_seed_holds(st, keys, nk, h, nh, hub);
    }
}

int couple(State *st, Field *fd, const lp_id *ids, int n, const int *occ_of, int kind, Curve *near, int keep){
    if (!n) return 0;
    const Firmware *fw = st->fw;
    if (!getenv("LAPLACE_COUPLE_SQL")) {
        lp_id *keys = malloc(sizeof(lp_id) * (size_t)n);
        int *of = malloc(sizeof(int) * (size_t)n), nk = 0;
        for (int o = 0; o < n; o++) {
            if (occ_of && occ_of[o] >= 0 && occ_of[o] < st->nocc && !st->composed[occ_of[o]]) continue;
            keys[nk] = ids[o]; of[nk++] = o;
        }
        int *hub = malloc(sizeof(int) * 2 * (size_t)(nk ? nk : 1)), nh = 0;
        Hold *h = nk ? holds_capped_scoped(st->pg, keys, nk, fw->fan, 1, 3,
            st->witness_scope, st->nwitness_scope, st->witness_scoped, hub, &nh) : NULL;
        if (nk) { st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nh; }
        couple_ingest_holds(st, fd, h, nh, hub, keys, of, nk, occ_of, kind);
        int nn = keep > 0 ? shape_measured(st, ids, n, occ_of, h, nh, near, keep) : 0;
        holds_free(h, nh); free(hub); free(keys); free(of);
        return nn;
    }
    if (st->witness_scoped && st->nwitness_scope <= 0) return 0;
    Args a = { 0 };
    arg_ids(&a, ids, (size_t)n); arg_i64(&a, (int64_t)fw->fan); arg_refused(&a);
    arg_i16(&a, (int16_t)(keep > 0 ? fw->shape : -1)); arg_f64_bin(&a, fw->shape_n);
    arg_i32(&a, (int32_t)(keep > 0 ? keep + 1 : keep));
    if (st->witness_scoped) arg_ids(&a, st->witness_scope, (size_t)st->nwitness_scope);
    PGresult *q = call_st(st, st->witness_scoped ? LP_SQL_COUPLE_SCOPED : LP_SQL_COUPLE, &a);
    int *held = calloc((size_t)n + 1, sizeof(int)), nn = 0;
    for (int r = 0; r < PQntuples(q); r++) if (col_i16(q, r, 2) == 0) { int o = col_i32(q, r, 1); if (o >= 1 && o <= n) held[o - 1]++; }
    static const int OC[] = { 2, 1, 9, 0, 6, 7 };
    int *ord = rows_in_order(q, OC, 6);
    for (int r_ = 0; r_ < PQntuples(q); r_++) {
        int r = ord[r_]; const lp_id *id = col_id(q, r, 0); int o = col_i32(q, r, 1) - 1, route = col_i16(q, r, 2);
        if (lp_id_eq(id, &st->prompt)) continue;
        int occ = o >= 0 && o < n && occ_of ? occ_of[o] : -1;
        double pull = occ >= 0 && occ < st->nocc ? st->role[occ] : kind == R_CLAIM ? 1.0 : 0.5;
        if (route == 2) {
            if (nn < keep) {
                Curve *c = &near[nn++]; c->id = *id; c->d = col_f64(q, r, 9);
                const uint8_t *v = (const uint8_t *)PQgetvalue(q, r, 10); size_t vl = (size_t)PQgetlength(q, r, 10), na = lp_pg_ids_read(v, vl, NULL, 0);
                c->v = malloc(sizeof(lp_id) * (na ? na : 1)); c->nv = (int)lp_pg_ids_read(v, vl, c->v, na);
            }
            continue;
        }
        Cell *x = cell(fd, id);
        if (route == 1) { x->segment = 1; x->tier = col_i16(q, r, 8); x->force += pull; x->routes[R_CONTAIN]++; if (occ >= 0) bit_set(&x->support, occ); continue; }
        lp_rating rt = col_rating(q, r, 3); lp_id part[3] = { *col_id(q, r, 6), *col_id(q, r, 7), *id };
        double conf = lp_confidence(&rt, fw->k) * strand_weight(fw, fw->id.weigh, part, 3);
        x->force += conf * pull; x->routes[kind]++; if (occ >= 0) bit_set(&x->support, occ);
        if (o >= 0 && held[o] > fw->fan) x->hub = 1;
        if (!x->has_r || conf > lp_confidence(&x->r, fw->k)) { x->r = rt; x->has_r = 1; x->via = part[0]; x->rel = part[1]; }
    }
    PQclear(q); args_free(&a); free(held); free(ord);
    return nn;
}

int follows(State *st, Next *out, int cap){
    if (st->witness_scoped && st->nwitness_scope <= 0) return 0;
    int from = st->ntraj > 24 ? st->ntraj - 24 : 0, n = st->ntraj - from;
    if (n <= 0) return 0;
    Args a = { 0 };
    arg_ids(&a, st->traj + from, (size_t)n);
    arg_i64(&a, (int64_t)st->fw->fan);
    if (st->witness_scoped) arg_ids(&a, st->witness_scope, (size_t)st->nwitness_scope);
    PGresult *q = call_st(st, st->witness_scoped ? LP_SQL_FORWARD_SUFFIX_SCOPED : LP_SQL_FORWARD_SUFFIX, &a);
    int m = 0; lp_idmap *seen = lp_idmap_new();
    for (int r = 0; r < PQntuples(q) && m < cap; r++) {
        int len = col_i32(q, r, 1) - col_i32(q, r, 0) + 1;
        bool fresh; lp_idmap_put(seen, col_id(q, r, 2), &fresh); if (!fresh) continue;
        out[m].id = *col_id(q, r, 2); out[m].times = (long)col_i64(q, r, 3); out[m].len = len; m++;
    }
    lp_idmap_free(seen); PQclear(q); args_free(&a);
    return m;
}

int shape_near(State *st, const lp_id *ids, int n, Curve *near, int keep){
    if (n < 2 || keep <= 0) return 0;
    const Firmware *fw = st->fw;
    if (!getenv("LAPLACE_COUPLE_SQL")) {
        lp_id *keys = malloc(sizeof(lp_id) * (size_t)n); int nk = 0;
        for (int i = 0; i < n; i++) if (lp_tier0_codepoint(T0, &ids[i]) < 0) keys[nk++] = ids[i];
        int *hub = malloc(sizeof(int) * 2 * (size_t)(nk ? nk : 1)), nh = 0;
        Hold *h = nk ? holds_capped_scoped(st->pg, keys, nk, fw->fan, 0, 2,
            st->witness_scope, st->nwitness_scope, st->witness_scoped, hub, &nh) : NULL;
        if (nk) { st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nh; }
        int k = shape_measured(st, ids, n, NULL, h, nh, near, keep);
        holds_free(h, nh); free(hub); free(keys);
        return k;
    }
    if (st->witness_scoped && st->nwitness_scope <= 0) return 0;
    Args a = { 0 }; arg_ids(&a, ids, (size_t)n); arg_i64(&a, (int64_t)fw->fan); arg_refused(&a);
    arg_i16(&a, (int16_t)fw->shape); arg_f64_bin(&a, fw->shape_n); arg_i32(&a, (int32_t)(keep + 4));
    if (st->witness_scoped) arg_ids(&a, st->witness_scope, (size_t)st->nwitness_scope);
    PGresult *q = call_st(st, st->witness_scoped ? LP_SQL_COUPLE_SCAN_SCOPED : LP_SQL_COUPLE_SCAN, &a);
    int m = PQntuples(q), nn = 0;
    Curve *all = malloc(sizeof(Curve) * (size_t)(m ? m : 1));
    for (int r = 0; r < m; r++) {
        const lp_id *id = col_id(q, r, 0); if (lp_id_eq(id, &st->prompt)) continue;
        Curve *c = &all[nn++]; c->id = *id; c->d = col_f64(q, r, 9);
        const uint8_t *v = (const uint8_t *)PQgetvalue(q, r, 10); size_t vl = (size_t)PQgetlength(q, r, 10), na = lp_pg_ids_read(v, vl, NULL, 0);
        c->v = malloc(sizeof(lp_id) * (na ? na : 1)); c->nv = (int)lp_pg_ids_read(v, vl, c->v, na);
    }
    qsort(all, (size_t)nn, sizeof(Curve), curve_order);
    int k = nn < keep ? nn : keep;
    for (int i = 0; i < k; i++) near[i] = all[i];
    for (int i = k; i < nn; i++) free(all[i].v);
    free(all); PQclear(q); args_free(&a);
    return k;
}

typedef struct { lp_id to; double w; } Hop_;
typedef struct Adj_ { Hop_ *h; int n; size_t cap; int hub; } Adj;
static int hop_cmp(const void *a, const void *b){
    const Hop_ *x = a, *y = b; int c = memcmp(&x->to, &y->to, 16);
    return c ? c : (x->w < y->w ? -1 : x->w > y->w);
}

/* Pre-seed step-0 walk adjacency from the initial COUPLE holds batch so walk(0, nocc) needs no step-0 DB trip. */
void walk_seed_holds(State *st, const lp_id *keys, int nk, const Hold *hh, int nh, const int *hub){
    const Firmware *fw = st->fw;
    if (fw->walks <= 0 || fw->steps <= 0 || !keys || nk <= 0) return;
    if (!st->walk_am) st->walk_am = lp_idmap_new();
    if (!st->walk_at) st->walk_at = lp_idmap_new();

    int *idx_of = malloc(sizeof(int) * (size_t)nk);
    int *counts = calloc((size_t)nk, sizeof(int));
    for (int e = 0; e < nk; e++) {
        bool fresh; size_t slot = lp_idmap_put(st->walk_am, &keys[e], &fresh);
        if (fresh) {
            lp_reserve((void **)&st->walk_adj, &st->walk_acap, (size_t)st->walk_nadj + 1, sizeof(Adj));
            memset(&st->walk_adj[st->walk_nadj], 0, sizeof(Adj));
            idx_of[e] = st->walk_nadj++;
        } else {
            idx_of[e] = (int)slot;
        }
    }
    Ids rn = { 0 };
    for (int r = 0; r < nh; r++) {
        int e = hh[r].src; if (e < 0 || e >= nk || hub[2 * e] || !hh[r].claim || !hh[r].stood) continue;
        if (lp_is_internal_metadata_path(st->c, hh[r].path, (size_t)hh[r].path_len)) continue;
        counts[e]++;
        if (counts[e] > fw->walk_fan) continue;
        path_into(&rn, lp_path_of(hh[r].path, (size_t)hh[r].path_len));
        if (!head_read(fw, rn.v, rn.n)) continue;
        int other = lp_tuple_other(rn.v, rn.n, &keys[e]);
        if (other < 0 || lp_id_eq(&rn.v[other], &keys[e])) continue;
        lp_rating rt = hh[r].r;
        double w = lp_confidence(&rt, fw->k) * strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n);
        if (!(w > 0.0)) continue;
        Adj *x = &st->walk_adj[idx_of[e]];
        lp_reserve((void **)&x->h, &x->cap, (size_t)x->n + 1, sizeof(Hop_));
        x->h[x->n++] = (Hop_){ rn.v[other], w };
    }
    for (int e = 0; e < nk; e++) {
        Adj *x = &st->walk_adj[idx_of[e]];
        x->hub = (hub[2 * e] || counts[e] > fw->walk_fan);
        if (x->n > 1) qsort(x->h, (size_t)x->n, sizeof(Hop_), hop_cmp);
    }
    st->walk_read += nk;
    lp_vec_free(&rn); free(counts); free(idx_of);
}

void walk(State *st, int from, int to){
    const Firmware *fw = st->fw; if (fw->walks <= 0 || fw->steps <= 0) return;
    int org[MAXOCC], no = 0;
    for (int i = from; i < to; i++) if (st->composed[i] && st->role[i] > 0.0) org[no++] = i;
    if (!no) return;
    int W = fw->walks, nw = no * W;
    lp_id *pos = malloc(sizeof(lp_id) * (size_t)nw), *need = malloc(sizeof(lp_id) * (size_t)nw);
    for (int o = 0; o < no; o++) for (int w = 0; w < W; w++) pos[o * W + w] = st->occ[org[o]];
    if (!st->walk_am) st->walk_am = lp_idmap_new();
    if (!st->walk_at) st->walk_at = lp_idmap_new();
    lp_idmap *am = st->walk_am; Adj *adj = st->walk_adj; size_t acap = st->walk_acap; int nadj = st->walk_nadj;
    Ids rn = { 0 }; Args a = { 0 };
    for (int s = 0; s < fw->steps; s++) {
        int nn = 0;
        for (int i = 0; i < nw; i++) {
            bool fresh; lp_idmap_put(am, &pos[i], &fresh);
            if (fresh) { lp_reserve((void **)&adj, &acap, (size_t)nadj + 1, sizeof(Adj)); memset(&adj[nadj++], 0, sizeof(Adj)); need[nn++] = pos[i]; }
        }
        if (nn && !envelope_hit(st)) {
            int *hub = malloc(sizeof(int) * 2 * (size_t)nn), nh = 0;
            Hold *hh = holds_capped_scoped(st->pg, need, nn, fw->walk_fan, 1, 1,
                st->witness_scope, st->nwitness_scope, st->witness_scoped, hub, &nh);
            st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nh;
            int *held = calloc((size_t)nn, sizeof(int));
            for (int e = 0; e < nn; e++) held[e] = hub[2 * e] ? fw->walk_fan + 1 : 0;
            for (int r = 0; r < nh; r++) {
                int e = hh[r].src; if (e < 0 || e >= nn || held[e] > fw->walk_fan || !hh[r].claim || !hh[r].stood) continue;
                if (lp_is_internal_metadata_path(st->c, hh[r].path, (size_t)hh[r].path_len)) continue;
                path_into(&rn, lp_path_of(hh[r].path, (size_t)hh[r].path_len));
                if (!head_read(fw, rn.v, rn.n)) continue;
                int other = lp_tuple_other(rn.v, rn.n, &need[e]);
                if (other < 0 || lp_id_eq(&rn.v[other], &need[e])) continue;
                lp_rating rt = hh[r].r;
                double w = lp_confidence(&rt, fw->k) * strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n);
                if (!(w > 0.0)) continue;
                Adj *x = &adj[lp_idmap_find(am, &need[e])];
                lp_reserve((void **)&x->h, &x->cap, (size_t)x->n + 1, sizeof(Hop_));
                x->h[x->n++] = (Hop_){ rn.v[other], w };
            }
            for (int e = 0; e < nn; e++) { Adj *x = &adj[lp_idmap_find(am, &need[e])]; x->hub = held[e] > fw->walk_fan; }
            if (fw->nup) {
                lp_id *hk = malloc(sizeof(lp_id) * (size_t)nn); int nhk = 0;
                for (int e = 0; e < nn; e++) if (held[e] > fw->walk_fan) hk[nhk++] = need[e];
                for (int u = 0; u < fw->nup && nhk; u++) {
                    int n2 = 0;
                    Hold *h2 = holds_both_scoped(st->pg, hk, nhk, &fw->id.up[u],
                        st->witness_scope, st->nwitness_scope, st->witness_scoped, &n2);
                    st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)n2;
                    for (int r2 = 0; r2 < n2; r2++) {
                        int s_ = h2[r2].src; if (s_ < 0 || s_ >= nhk || !h2[r2].claim || !h2[r2].stood) continue;
                        path_into(&rn, lp_path_of(h2[r2].path, (size_t)h2[r2].path_len));
                        if (rn.n < 2 || !head_read(fw, rn.v, rn.n)) continue;
                        int other = lp_tuple_other(rn.v, rn.n, &hk[s_]);
                        if (other < 0 || lp_id_eq(&rn.v[other], &hk[s_])) continue;
                        double w = lp_confidence(&h2[r2].r, fw->k); if (!(w > 0.0)) continue;
                        Adj *x = &adj[lp_idmap_find(am, &hk[s_])];
                        lp_reserve((void **)&x->h, &x->cap, (size_t)x->n + 1, sizeof(Hop_));
                        x->h[x->n++] = (Hop_){ rn.v[other], w }; x->hub = 0;
                    }
                    holds_free(h2, n2);
                }
                free(hk);
            }
            for (int e = 0; e < nn; e++) { Adj *x = &adj[lp_idmap_find(am, &need[e])]; if (x->n > 1) qsort(x->h, (size_t)x->n, sizeof(Hop_), hop_cmp); }
            st->walk_read += nn; free(held); free(hub); holds_free(hh, nh);
        }
        for (int o = 0; o < no; o++) for (int w = 0; w < W; w++) {
            int i = o * W + w; const Adj *x = &adj[lp_idmap_find(am, &pos[i])];
            if (draw(st, (uint32_t)org[o], (uint32_t)w, (uint32_t)(2 * s)) < fw->restart || x->hub || !x->n) {
                pos[i] = st->occ[org[o]]; st->walk_homes++; continue;
            }
            double tot = 0; for (int k = 0; k < x->n; k++) tot += x->h[k].w;
            double u = draw(st, (uint32_t)org[o], (uint32_t)w, (uint32_t)(2 * s + 1)) * tot;
            int k = 0; while (k + 1 < x->n && u >= x->h[k].w) { u -= x->h[k].w; k++; }
            pos[i] = x->h[k].to; st->walk_steps++;
            bool fresh; size_t vi = lp_idmap_put(st->walk_at, &pos[i], &fresh);
            if (fresh) {
                size_t need_ = (vi + 1) * MAXOCC, old = st->walk_cap;
                lp_reserve((void **)&st->walk_visits, &st->walk_cap, need_, sizeof(int));
                memset(st->walk_visits + old, 0, (st->walk_cap - old) * sizeof(int));
            }
            st->walk_visits[vi * MAXOCC + (size_t)org[o]]++;
        }
        st->walk_adj = adj; st->walk_acap = acap; st->walk_nadj = nadj;
    }
    free(pos); free(need); lp_vec_free(&rn); args_free(&a);
}

void context(State *st, int from, int to){
    lp_id keys[MAXOCC]; int at[MAXOCC], nk = 0;
    for (int i = from; i < to; i++) {
        if (st->ctx[i] || st->ctx_hub[i]) continue;
        st->ctx[i] = NULL; st->ctx_n[i] = 0; st->ctx_hub[i] = 0; at[i - from] = -1;
        if (!st->composed[i] || st->role[i] <= 0.0) continue;
        int dup = -1; for (int j = 0; j < from && dup < 0; j++) if (lp_id_eq(&st->occ[j], &st->occ[i]) && (st->ctx[j] || st->ctx_hub[j])) dup = j;
        if (dup >= 0) { st->ctx[i] = st->ctx[dup]; st->ctx_n[i] = st->ctx_n[dup]; st->ctx_hub[i] = st->ctx_hub[dup]; continue; }
        int k = -1; for (int z = 0; z < nk; z++) if (lp_id_eq(&keys[z], &st->occ[i])) k = z;
        if (k < 0) { keys[nk] = st->occ[i]; k = nk++; }
        at[i - from] = k;
    }
    if (!nk) return;
    int *hub = malloc(sizeof(int) * 2 * (size_t)nk), nh = 0;
    Hold *h = holds_capped_scoped(st->pg, keys, nk, st->fw->fan, 0, 2,
        st->witness_scope, st->nwitness_scope, st->witness_scoped, hub, &nh);
    st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nh;
    context_fill(st, from, to, at, nk, h, nh, hub, keys);
    holds_free(h, nh); free(hub);
}

int beside_grounds(const State *st, int i, const lp_id *id){
    int c = beside(st, i, id); if (c < 2 || !st->ctx_n[i] || !st->ctx_total) return 0;
    int64_t k = lp_idmap_find(st->ctx_all, id);
    double base = k < 0 ? 0.0 : (double)*lp_idmap_value(st->ctx_all, (size_t)k) / st->ctx_total;
    return base > 0.0 && ((double)c / st->ctx_n[i]) >= st->fw->lift * base;
}

int langs_of(State *st, const lp_id *w, int n, lp_id (*out)[LANGS], int *nout){
    const Firmware *fw = st->fw; memset(nout, 0, sizeof(int) * (size_t)n);
    if (!n || !fw->lexical[0][0]) return 0;
    if (!st->lexical.np && !lexicalization_of(st->c, fw, &st->lexical)) return 0;
    const Lexicalization *lx = &st->lexical;
    int nc = 0, *src = NULL;
    Claim *cl = claims_each(st->pg, w, n, &lx->sense, CLAIM_FIRST, fw->fan, fw->k, &nc, &src);
    st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nc;
    lp_id *sense = malloc(sizeof(lp_id) * (size_t)(nc ? nc : 1));
    for (int entry = 0; entry < nc; entry++) sense[entry] = cl[entry].part[cl[entry].np - 1];
    int nh = 0; Hold *h = nc ? paths_of(st->pg, sense, nc, &nh) : NULL;
    if (nc > 0) { st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)nh; }
    Ids parts = { 0 };
    for (int row = 0; row < nh; row++) {
        int entry = h[row].src; if (entry < 0 || entry >= nc) continue;
        int word = src[entry]; if (word < 0 || word >= n) continue;
        path_into(&parts, lp_path_of(h[row].path, (size_t)h[row].path_len));
        if (parts.n != (size_t)lx->np) continue;
        const lp_id *language = &parts.v[lx->lang]; int duplicate = 0;
        for (int index = 0; index < nout[word]; index++) duplicate |= lp_id_eq(&out[word][index], language);
        if (!duplicate && nout[word] < LANGS) out[word][nout[word]++] = *language;
    }
    int known = 0; for (int word = 0; word < n; word++) known += nout[word] > 0;
    lp_vec_free(&parts); holds_free(h, nh); free(sense); free(src); free(cl);
    return known;
}

typedef struct { State *st; const lp_id *word; int last_hold, last_n; } ForkCtx;
static int fork_choice(void *vc, const Claim *cl, int n){
    ForkCtx *fc = vc; State *st = fc->st; const Firmware *fw = st->fw;
    lp_id *ends = malloc(sizeof(lp_id) * (size_t)n);
    for (int k = 0; k < n; k++) ends[k] = cl[k].part[cl[k].np - 1];
    lp_idmap **rec = calloc((size_t)n, sizeof *rec);
    for (int k = 0; k < n; k++) { bool f; rec[k] = lp_idmap_new(); lp_idmap_put(rec[k], &ends[k], &f); }
    lp_id *k1 = malloc(sizeof(lp_id) * (size_t)n * 17); int *b1 = malloc(sizeof(int) * (size_t)n * 17), nk1 = 0;
    for (int k = 0; k < n; k++) { k1[nk1] = ends[k]; b1[nk1++] = k; reader_want(st->rd, &ends[k]); }
    for (int k = 0; k < n; k++) {
        lp_id pp[16]; size_t np = reader_parts(st->rd, &ends[k], pp, 16); if (np > 16) np = 16;
        for (size_t v = 0; v < np; v++) {
            if (lp_tier0_codepoint(T0, &pp[v]) >= 0 || lp_id_eq(&pp[v], fc->word) || table_find(&pp[v])) continue;
            bool f; lp_idmap_put(rec[k], &pp[v], &f); if (f) { k1[nk1] = pp[v]; b1[nk1++] = k; }
        }
    }
    int *hb = malloc(sizeof(int) * 2 * (size_t)nk1), m1 = 0;
    Hold *c1 = holds_capped_scoped(st->pg, k1, nk1, fw->fan, 0, 1,
        st->witness_scope, st->nwitness_scope, st->witness_scoped, hb, &m1);
    st->trips++; st->work.db_probes++; st->work.rows_scanned += (uint64_t)m1;
    Ids rv = { 0 };
    for (int r = 0; r < m1; r++) {
        int s1_ = c1[r].src; if (s1_ < 0 || s1_ >= nk1 || !c1[r].claim) continue;
        int b = b1[s1_]; path_into(&rv, lp_path_of(c1[r].path, (size_t)c1[r].path_len));
        int o = lp_tuple_other(rv.v, rv.n, &k1[s1_]); if (o < 0) continue;
        bool f; lp_idmap_put(rec[b], &rv.v[o], &f);
    }
    holds_free(c1, m1); free(hb); free(k1); free(b1); lp_vec_free(&rv);

    lp_id parts[256]; size_t *done = calloc((size_t)n, sizeof(size_t));
    for (int level = 0; level < 4; level++) {
        for (int k = 0; k < n; k++) for (size_t i = done[k]; i < lp_idmap_count(rec[k]); i++) reader_want(st->rd, lp_idmap_key(rec[k], i));
        int grew = 0;
        for (int k = 0; k < n; k++) {
            size_t cnt = lp_idmap_count(rec[k]);
            for (size_t i = done[k]; i < cnt; i++) {
                lp_id x = *lp_idmap_key(rec[k], i);
                size_t np = reader_parts(st->rd, &x, parts, 256); if (np > 256) np = 256;
                size_t atoms = 0; for (size_t v = 0; v < np; v++) atoms += lp_tier0_codepoint(T0, &parts[v]) >= 0;
                if (!np || atoms == np) continue;
                for (size_t v = 0; v < np; v++) {
                    if (lp_tier0_codepoint(T0, &parts[v]) >= 0) continue;
                    bool f; lp_idmap_put(rec[k], &parts[v], &f); grew |= f;
                }
            }
            done[k] = cnt;
        }
        if (!grew) break;
    }
    free(done);

    if (!st->df_ready) {
        st->df_max = 0;
        for (int j = 0; j < st->npos; j++) {
            st->df[j] = !st->composed[j] ? 0 : st->ctx_hub[j] ? st->fw->fan + 1 : st->ctx_n[j];
            if (st->df[j] > st->df_max) st->df_max = st->df[j];
        }
        st->df_ready = st->npos;
    }
    double *hold = calloc((size_t)n, sizeof(double)), *walks = calloc((size_t)n, sizeof(double)), *score = calloc((size_t)n, sizeof(double));
    for (int k = 0; k < n; k++) {
        size_t cnt = lp_idmap_count(rec[k]); double norm = sqrt((double)(cnt ? cnt : 1));
        for (int j = 0; j < st->npos; j++) {
            if (!st->composed[j] || st->ctx_hub[j] || st->role[j] <= 0.0 || lp_id_eq(&st->occ[j], fc->word)) continue;
            int seen = 0; for (int i = 0; i < j && !seen; i++) seen = lp_id_eq(&st->occ[i], &st->occ[j]); if (seen) continue;
            double info = log((1.0 + st->df_max) / (1.0 + st->df[j]));
            if (info > 0.0 && lp_idmap_find(rec[k], &st->occ[j]) >= 0) hold[k] += st->role[j] * info;
            for (size_t i = 0; i < cnt; i++) walks[k] += st->role[j] * visits(st, lp_idmap_key(rec[k], i), j);
        }
        hold[k] /= norm; walks[k] /= norm;
        int rank = cl[k].position > 0 ? cl[k].position : k + 1;
        score[k] = -log((double)rank) + hold[k];
    }

    double *prev = calloc((size_t)n, sizeof(double)); double ptotal = 0.0;
    if (st->lexical.has_usage) {
        long long *uses = calloc((size_t)n, sizeof(long long));
        st->trips += (uint64_t)lexicalization_counts(st->pg, st->rd, &st->lexical, ends, n, fw->fan, fw->k, uses);
        for (int branch = 0; branch < n; branch++) { prev[branch] = (double)uses[branch]; ptotal += prev[branch]; }
        free(uses);
    } else if (fw->prevalence[0]) {
        lp_id *cc = malloc(sizeof(lp_id) * (size_t)n * 4); int *cof = malloc(sizeof(int) * (size_t)n * 4), ncc = 0;
        for (int k = 0; k < n; k++) {
            lp_id pp[8]; size_t np = reader_parts(st->rd, &ends[k], pp, 8); if (np > 8) np = 8;
            for (size_t v = 0; v < np && ncc < n * 4; v++)
                if (!lp_id_eq(&pp[v], fc->word) && lp_tier0_codepoint(T0, &pp[v]) < 0) { cc[ncc] = pp[v]; cof[ncc++] = k; }
        }
        int n2 = 0;
        Hold *h2 = ncc ? holds_both_scoped(st->pg, cc, ncc, fc->word,
            st->witness_scope, st->nwitness_scope, st->witness_scoped, &n2) : NULL;
        if (ncc) st->trips++;
        Ids pv = { 0 };
        for (int r = 0; r < n2; r++) {
            int s_ = h2[r].src; if (s_ < 0 || s_ >= ncc || !h2[r].claim || !h2[r].stood) continue;
            path_into(&pv, lp_path_of(h2[r].path, (size_t)h2[r].path_len));
            int as_pair = fw->prevalence_pair && pv.n == 2 && lp_id_eq(&pv.v[0], fc->word) && lp_id_eq(&pv.v[1], &cc[s_]);
            int as_rel = !fw->prevalence_pair && pv.n == 3 && lp_id_eq(&pv.v[0], fc->word) && lp_id_eq(&pv.v[1], &fw->id.prevalence) && lp_id_eq(&pv.v[2], &cc[s_]);
            if (!as_pair && !as_rel) continue;
            prev[cof[s_]] += (double)h2[r].matches; ptotal += (double)h2[r].matches;
        }
        holds_free(h2, n2); lp_vec_free(&pv); free(cc); free(cof);
    }
    if (ptotal > 0.0) for (int k = 0; k < n; k++) score[k] = log((1.0 + prev[k]) / (1.0 + ptotal)) + hold[k];

    double *inlang = calloc((size_t)n, sizeof(double));
    if (st->nlang) {
        lp_id (*bw)[64] = calloc((size_t)n, sizeof *bw); int *nbw = calloc((size_t)n, sizeof(int));
        lp_id all[4096]; int na = 0;
        for (int k = 0; k < n; k++) {
            lp_id pp[256]; size_t np = reader_parts(st->rd, &ends[k], pp, 256); if (np > 256) np = 256;
            for (size_t v = 0; v < np; v++) {
                if (lp_tier0_codepoint(T0, &pp[v]) >= 0) continue;
                lp_id sub_[64]; size_t ns = reader_parts(st->rd, &pp[v], sub_, 64);
                size_t atoms = 0; for (size_t q = 0; q < ns && q < 64; q++) atoms += lp_tier0_codepoint(T0, &sub_[q]) >= 0;
                if (ns && atoms == ns && nbw[k] < 64 && na < 4096) { bw[k][nbw[k]++] = pp[v]; all[na++] = pp[v]; }
            }
        }
        lp_id (*lg)[LANGS] = calloc((size_t)(na ? na : 1), sizeof *lg); int *nlg = calloc((size_t)(na ? na : 1), sizeof(int));
        langs_of(st, all, na, lg, nlg);
        int at = 0;
        for (int k = 0; k < n; k++) {
            int known = 0, ours = 0;
            for (int v = 0; v < nbw[k]; v++, at++) {
                if (!nlg[at]) continue; known++;
                int hit = 0; for (int z = 0; z < nlg[at] && z < LANGS && !hit; z++) for (int y = 0; y < st->nlang; y++) hit |= lp_id_eq(&lg[at][z], &st->lang[y]);
                ours += hit;
            }
            inlang[k] = known ? (double)ours / known : 0.0;
        }
        free(bw); free(nbw); free(lg); free(nlg);
    }

    int best = 0;
    for (int k = 1; k < n; k++) {
        double tol = fw->sure * sqrt((walks[k] + walks[best]) / 4.0);
        if (inlang[k] > inlang[best] || (inlang[k] == inlang[best] && (score[k] > score[best] || (score[k] == score[best] && walks[k] > walks[best] + tol))))
            best = k;
    }
    int told = 0;
    for (int k = 0; k < n; k++) {
        if (k != best && (inlang[best] > inlang[k] || (inlang[best] == inlang[k] && (score[best] > score[k] || walks[best] > walks[k] + fw->sure * sqrt((walks[k] + walks[best]) / 4.0)))))
            told++;
    }
    int pick = told == n - 1 ? best : -1;
    fc->last_hold = (int)(hold[best] * 1000 + 0.5); fc->last_n = n;
    for (int k = 0; k < n; k++) lp_idmap_free(rec[k]);
    free(rec); free(ends); free(hold); free(walks); free(score); free(inlang); free(prev);
    return pick;
}

int chain_from(State *st, const lp_id *word, const lp_id *reading, lp_id *answer, lp_rating *rating){
    ClaimStanding last; int alt; ForkCtx fc = { st, word, 0, 0 };
    chain_follow(st->pg, st->rd, (Firmware *)st->fw, word, reading, NULL, fork_choice, &fc, answer, &last, &alt);
    if (alt < 0) return 0;
    *rating = last.r;
    return 1;
}

void state_free(State *st){
    if (!st) return;
    free(st->salience_rows);
    for (int i = 0; i < MAXOCC; i++) {
        if (!st->ctx[i]) continue;
        lp_idmap *m = st->ctx[i];
        lp_idmap_free(m);
        for (int j = i + 1; j < MAXOCC; j++) if (st->ctx[j] == m) st->ctx[j] = NULL;
        st->ctx[i] = NULL;
    }
    lp_idmap_free(st->ctx_all);
    lp_idmap_free(st->ctx_obs);
    lp_idmap_free(st->heads);
    lp_idmap_free(st->walk_at);
    free(st->walk_visits);
    lp_idmap_free(st->walk_am);
    for (int i = 0; i < st->walk_nadj; i++) free(st->walk_adj[i].h);
    free(st->walk_adj);
    free(st);
}
