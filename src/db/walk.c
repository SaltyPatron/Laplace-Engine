#define _GNU_SOURCE
/* The witnesses of claims, read from the attestation records (laplace_attested, laplace_witness_trust). Standings and
 * the testimony under a trunk are folded inside PostgreSQL by the Native law (lp_dag_multiplicity, lp_tally_edge,
 * lp_series); nothing here walks containment or counts. */
#include "engine.h"
#include "walk.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- the witnesses: a trunk in the attestation table, and its trust (laplace_witness_trust: one index probe each) */
void witnesses_known(PGconn *pg, Root *r, size_t n){
    lp_id *ids = malloc(sizeof(lp_id) * (n + 1)); for (size_t i = 0; i < n; i++) ids[i] = r[i].id;
    Args q = { 0 }; arg_ids(&q, ids, n); PGresult *w = db_call(pg, LP_SQL_WITNESS_TRUST, &q);
    for (int j = 0; j < PQntuples(w); j++) for (size_t k = 0; k < n; k++) if (!memcmp(col_id(w, j, 0), &r[k].id, 16)) { r[k].witness = 1; r[k].trust = col_f64(w, j, 1); }
    PQclear(w); args_free(&q); free(ids);
}

/* Who said these claims: one row a claim and the witness trunk above it, read from the attestation records. Every
 * supported extension has laplace_attested, so there is no probe and no containment walk-up fallback: a probe cached for
 * the process outlived an extension upgrade and sent every read up the containment DAG instead. */
Attested *attested(PGconn *pg, const lp_id *claims, int n, int *nout, uint64_t *rows){
    *nout = 0; if (n <= 0) return NULL;
    Args q = { 0 }; arg_ids(&q, claims, (size_t)n);
    PGresult *r = db_call(pg, LP_SQL_ATTESTED, &q);
    args_free(&q);
    int nr = PQntuples(r); if (rows) *rows += (uint64_t)nr;
    Attested *out = calloc((size_t)(nr ? nr : 1), sizeof(Attested));
    for (int i = 0; i < nr; i++) {
        out[i].claim    = *col_id(r, i, 0);
        out[i].trunk    = *col_id(r, i, 1);
        out[i].games    = col_f64(r, i, 2);
        out[i].tokens   = col_f64(r, i, 3);
        out[i].score    = col_f64(r, i, 4);
        out[i].position = (uint32_t)col_i32(r, i, 5);
        out[i].trust    = col_f64(r, i, 6);
        out[i].witness  = col_bool(r, i, 7);
    }
    PQclear(r);
    *nout = nr;
    return out;
}

typedef struct { lp_id witness; double info; double trust; uint64_t games; } WitAcc;

static int att_ptr_cmp(const void *a, const void *b){
    const Attested *xa = *(const Attested *const *)a, *xb = *(const Attested *const *)b;
    int c = lp_id_cmp(&xa->claim, &xb->claim);
    return c ? c : lp_id_cmp(&xa->trunk, &xb->trunk);
}
static int share_info_cmp(const void *a, const void *b){
    const LpWitnessShare *x = a, *y = b;
    if (x->fisher_info != y->fisher_info) return x->fisher_info > y->fisher_info ? -1 : 1;
    return lp_id_cmp(&x->witness, &y->witness);
}

int witness_attribution_bp(const Attested *att, int natt, const lp_id *witness_scope, int nscope, int scoped, LpWitnessShare *out, int cap){
    if (!out || cap <= 0 || !att || natt <= 0) return 0;
    if (scoped && (!witness_scope || nscope <= 0)) return 0;

    const Attested **ord = malloc(sizeof(*ord) * (size_t)natt);
    for (int i = 0; i < natt; i++) ord[i] = &att[i];
    qsort(ord, (size_t)natt, sizeof(*ord), att_ptr_cmp);

    lp_authority_scope sc = {
        .witnesses = witness_scope,
        .witness_count = (size_t)(witness_scope && nscope > 0 ? nscope : 0),
        .scoped = (scoped != 0),
        .capabilities = LP_CAP_ALL,
        .disposition = LP_DISPOSE_ALLOW,
        .min_trust = 0.0
    };

    lp_idmap *acc = lp_idmap_sized(sizeof(WitAcc));
    lp_witness_series *ws = malloc(sizeof *ws * (size_t)natt);
    lp_series_information *info = malloc(sizeof *info * (size_t)natt);

    for (int i = 0; i < natt;) {
        int j = i + 1;
        while (j < natt && lp_id_eq(&ord[j]->claim, &ord[i]->claim)) j++;
        size_t m = 0;
        for (int k = i; k < j; k++) {
            const Attested *a = ord[k];
            double g = floor(a->games + 0.5);
            if (g < 1.0 || g > 9007199254740992.0) continue;
            double wins = a->score;
            if (wins < 0.0) wins = 0.0;
            if (wins > g) wins = g;
            lp_series_entry e;
            if (!lp_series_attested(a->trunk, g, wins, a->trust, &e)) continue;
            ws[m].entry = e;
            ws[m].witness = a->trunk;
            ws[m].trust = a->trust;
            m++;
        }
        if (m > 0) {
            lp_standing st;
            size_t written = 0;
            if (lp_attest_series_scoped(NULL, ws, m, &sc, &st, info, (size_t)natt, &written)) {
                for (size_t u = 0; u < written; u++) {
                    bool fresh;
                    size_t at = lp_idmap_put(acc, &info[u].dependence_root, &fresh);
                    WitAcc *w = lp_idmap_at(acc, at);
                    if (fresh) {
                        w->witness = info[u].dependence_root;
                        w->info = 0.0;
                        w->trust = 0.0;
                        w->games = 0;
                    }
                    w->info += info[u].information;
                    w->games += info[u].games;
                    for (size_t x = 0; x < m; x++) {
                        if (lp_id_eq(&ws[x].witness, &info[u].dependence_root)) { w->trust = ws[x].trust; break; }
                    }
                }
            }
        }
        i = j;
    }

    size_t nw = lp_idmap_count(acc);
    int nout = 0;
    if (nw > 0) {
        LpWitnessShare *all = calloc(nw, sizeof *all);
        for (size_t u = 0; u < nw; u++) {
            const WitAcc *w = lp_idmap_at(acc, u);
            all[u].witness = w->witness;
            all[u].trust = w->trust;
            all[u].fisher_info = w->info;
            all[u].games = w->games;
        }
        qsort(all, nw, sizeof *all, share_info_cmp);
        nout = (int)(nw < (size_t)cap ? nw : (size_t)cap);
        double *weights = malloc(sizeof(double) * (size_t)nout);
        lp_id *ids = malloc(sizeof(lp_id) * (size_t)nout);
        int32_t *bp = malloc(sizeof(int32_t) * (size_t)nout);
        for (int u = 0; u < nout; u++) {
            weights[u] = all[u].fisher_info > 0.0 ? all[u].fisher_info : 0.0;
            ids[u] = all[u].witness;
        }
        if (lp_fisher_shares_bp(weights, ids, (size_t)nout, bp)) {
            for (int u = 0; u < nout; u++) {
                out[u] = all[u];
                out[u].share_bp = bp[u];
            }
        } else {
            nout = 0;
        }
        free(bp);
        free(ids);
        free(weights);
        free(all);
    }

    free(info);
    free(ws);
    lp_idmap_free(acc);
    free(ord);
    return nout;
}
