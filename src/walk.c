/* The witnesses of claims, read from the attestation records (laplace_attested, laplace_witness_trust). Standings and
 * the testimony under a trunk are folded inside PostgreSQL by the Native law (lp_dag_multiplicity, lp_tally_edge,
 * lp_series); nothing here walks containment or counts. */
#include "engine.h"
#include "walk.h"
#include <stdlib.h>
#include <string.h>

/* ---- the witnesses: a trunk in the attestation table, and its trust (laplace_witness_trust: one index probe each) */
void witnesses_known(PGconn *pg, Root *r, size_t n){
    lp_id *ids = malloc(sizeof(lp_id) * (n + 1)); for (size_t i = 0; i < n; i++) ids[i] = r[i].id;
    Args q = { 0 }; arg_ids(&q, ids, n); PGresult *w = ask_once(pg, "SELECT witness, trust FROM @extschema@.laplace_witness_trust($1::@extschema@.blake3[])", &q);
    for (int j = 0; j < PQntuples(w); j++) for (size_t k = 0; k < n; k++) if (!memcmp(col_id(w, j, 0), &r[k].id, 16)) { r[k].witness = 1; r[k].trust = col_f64(w, j, 1); }
    PQclear(w); args_free(&q); free(ids);
}

/* Who said these claims: one row a claim and the witness trunk above it, read from the attestation records. Every
 * supported extension has laplace_attested, so there is no probe and no containment walk-up fallback: a probe cached for
 * the process outlived an extension upgrade and sent every read up the containment DAG instead. */
Attested *attested(PGconn *pg, const lp_id *claims, int n, int *nout, uint64_t *rows){
    *nout = 0; if (n <= 0) return NULL;
    Args q = { 0 }; arg_ids(&q, claims, (size_t)n);
    PGresult *r = ask_once(pg, "SELECT claim, trunk, games, tokens, score, \"position\", trust, witness FROM @extschema@.laplace_attested($1::@extschema@.blake3[])", &q);
    args_free(&q);
    int nr = PQntuples(r); if (rows) *rows += (uint64_t)nr;
    Attested *out = calloc((size_t)(nr ? nr : 1), sizeof(Attested));
    for (int i = 0; i < nr; i++) {
        out[i].claim    = *col_id(r, i, 0);
        out[i].trunk    = *col_id(r, i, 1);
        out[i].games    = col_f64(r, i, 2);
        out[i].tokens   = col_f64(r, i, 3);
        out[i].score    = col_f64(r, i, 4);
        out[i].position = (uint32_t)col_int(r, i, 5);
        out[i].trust    = col_f64(r, i, 6);
        out[i].witness  = PQgetvalue(r, i, 7)[0] != 0;
    }
    PQclear(r);
    *nout = nr;
    return out;
}
