/* Provenance by containment, the walks (Engine#22; Semantics: Attestations, Witnesses and Outcomes). A record is a path
 * over the claims it says, in its file's content tree, under the file's trunk, under the source's trunk, which is the
 * witness. Here are the two walks over that containment and the counts read off them: up from claims (walk_up,
 * attested), down from a trunk (walk_down, trunk_claims). They need the reader (read.c) and a connection (pg.c) and
 * nothing else of the engine, so a process that only reads links them as they are; the commands that print and replay
 * what they find are provenance.c. */
#include "engine.h"
#include "walk.h"
#include <stdlib.h>
#include <string.h>

static size_t dag_node(Dag *d, const lp_id *id, int *fresh_out){
    if (!d->m) d->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(d->m, id, &fresh);
    if (fresh) { lp_reserve((void **)&d->n, &d->cn, d->nn + 1, sizeof(WNode)); memset(&d->n[d->nn], 0, sizeof(WNode)); d->n[d->nn].id = *id; d->n[d->nn].in_head = NONE; d->nn++; }
    if (fresh_out) *fresh_out = fresh; return i;
}
static Edge *dag_edge(Dag *d, size_t p, size_t c, const lp_vertex *v){
    lp_reserve((void **)&d->e, &d->ce, d->ne + 1, sizeof(Edge)); Edge *e = &d->e[d->ne]; memset(e, 0, sizeof *e);
    e->p = p; e->c = c; e->times = v->run; e->outcome = v->outcome; e->spare = v->spare; e->position = v->position; e->said = (uint8_t)v->said;
    e->in_next = d->n[c].in_head; d->n[c].in_head = d->ne; d->n[c].nparents++; d->ne++; return e;
}
void dag_free(Dag *d){ lp_idmap_free(d->m); free(d->n); free(d->e); memset(d, 0, sizeof *d); }
/* How many times each node stands under `top`, top down in topological order. */
double *down_from(const Dag *d, size_t top){
    size_t n = d->nn; double *cnt = calloc(n ? n : 1, sizeof(double)); int *indeg = calloc(n ? n : 1, sizeof(int));
    size_t *head = malloc(sizeof(size_t) * (n ? n : 1)), *link = malloc(sizeof(size_t) * (d->ne ? d->ne : 1));
    for (size_t i = 0; i < n; i++) head[i] = NONE;
    for (size_t k = 0; k < d->ne; k++) { link[k] = head[d->e[k].p]; head[d->e[k].p] = k; indeg[d->e[k].c]++; }
    size_t *q = malloc(sizeof(size_t) * (n ? n : 1)), qh = 0, qt = 0; for (size_t i = 0; i < n; i++) if (!indeg[i]) q[qt++] = i;
    cnt[top] = 1;
    while (qh < qt) { size_t p = q[qh++]; for (size_t k = head[p]; k != NONE; k = link[k]) { size_t c = d->e[k].c; cnt[c] += cnt[p] * (double)d->e[k].times; if (!--indeg[c]) q[qt++] = c; } }
    free(indeg); free(head); free(link); free(q); return cnt;
}
/* What the claims under a node were said: per claim (and per claim and voice, where voices is given), games, tokens, the
 * sum of the scores and the least position. */
static void tally_add(Tally *t, double g, double tk, double s, uint32_t pos){ t->games += g; t->tokens += tk; t->score += s; if (pos && (!t->position || pos < t->position)) t->position = pos; }
void tally_under(const Dag *d, const double *cnt, lp_idmap *per_claim, lp_idmap *per_voice){
    for (size_t k = 0; k < d->ne; k++) { const Edge *e = &d->e[k]; if (!e->claim) continue; double c = cnt[e->p]; if (c <= 0) continue;
        double g = e->rec ? c * (double)e->times : c, tk = c * (double)e->times, s = g * lp_outcome_score(e->outcome, e->spare);
        bool f; tally_add(lp_idmap_get(per_claim, &d->n[e->c].id, &f), g, tk, s, e->position);
        if (per_voice && e->voiced) { lp_id key; for (int b = 0; b < 16; b++) key.b[b] = d->n[e->c].id.b[b] ^ e->voice.b[(b + 5) & 15];    /* a claim and its voice: their mix names the pair */
            VTally *v = lp_idmap_get(per_voice, &key, &f); if (f) { v->claim = d->n[e->c].id; v->voice = e->voice; } tally_add(&v->t, g, tk, s, e->position); } }
}

/* ---- up: from claims to the trunks that hold them. At the first level only a vertex said to be a claim, or a claim
 * standing as its own record, counts: the claim said, not the same content standing elsewhere. Above it, whatever
 * holds what was reached, but a file's metadata. */
int walk_up(Dag *d, const lp_id *claims, size_t n, uint64_t *rows){
    lp_idmap *front = lp_idmap_new(); for (size_t i = 0; i < n; i++) { lp_idmap_put(front, &claims[i], NULL); dag_node(d, &claims[i], NULL); }
    int level = 0; lp_vec(lp_vertex) vx = { 0 };
    while (lp_idmap_count(front) && level < 64) {
        int nh = 0; Hold *h = holds_any(lp_idmap_keys(front), (int)lp_idmap_count(front), &nh); *rows += (uint64_t)nh;
        lp_idmap *next = lp_idmap_new();
        for (int j = 0; j < nh; j++) {
            size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0); lp_vec_reserve(&vx, nv ? nv : 1); lp_path_vertices(h[j].path, (size_t)h[j].path_len, vx.v, nv);
            size_t pi = NONE; int fresh = 0;
            for (size_t v = 0; v < nv; v++) { if (lp_idmap_find(front, &vx.v[v].id) < 0) continue;
                uint32_t sd = vx.v[v].said;
                if (level == 0 && sd != LP_SAID_CLAIM && sd != LP_SAID_RECORD) continue;
                if (level > 0 && sd == LP_SAID_METADATA) continue;
                if (pi == NONE) { pi = dag_node(d, &h[j].entity, &fresh); d->n[pi].metadata = nv && vx.v[0].said == LP_SAID_METADATA; }
                Edge *e = dag_edge(d, pi, (size_t)lp_idmap_find(d->m, &vx.v[v].id), &vx.v[v]);
                if (level == 0) { e->claim = 1; e->rec = sd == LP_SAID_RECORD; if (v && vx.v[v - 1].said == LP_SAID_VOICE) { e->voiced = 1; e->voice = vx.v[v - 1].id; } } }
            if (pi != NONE && fresh) lp_idmap_put(next, &h[j].entity, NULL);
        }
        holds_free(h, nh); lp_idmap_free(front); front = next; level++;
    }
    lp_vec_free(&vx); lp_idmap_free(front); return level;
}

/* ---- the witnesses: a trunk in the witness table, and its trust */
void witnesses_known(PGconn *pg, Root *r, size_t n){
    lp_id *ids = malloc(sizeof(lp_id) * (n + 1)); for (size_t i = 0; i < n; i++) ids[i] = r[i].id;
    Args q = { 0 }; arg_ids(&q, ids, n); PGresult *w = ask_once(pg, "SELECT id, trust FROM witness WHERE id = ANY($1::blake3[])", &q);
    for (int j = 0; j < PQntuples(w); j++) for (size_t k = 0; k < n; k++) if (!memcmp(col_id(w, j, 0), &r[k].id, 16)) { r[k].witness = 1; r[k].trust = col_f64(w, j, 1); }
    PQclear(w); args_free(&q); free(ids);
}

/* Who said these claims: one row a claim and the trunk above it, with what the records under it said of the claim. */
Attested *attested(PGconn *pg, const lp_id *claims, int n, int *nout, uint64_t *rows){
    *nout = 0; if (n <= 0) return NULL;
    Dag d = { 0 }; uint64_t rd_rows = 0; walk_up(&d, claims, (size_t)n, &rd_rows); if (rows) *rows += rd_rows;
    lp_vec(Root) roots = { 0 }; lp_vec(size_t) at = { 0 };
    for (size_t i = (size_t)n; i < d.nn; i++) if (!d.n[i].nparents) { lp_push(&roots, (Root){ d.n[i].id, 0, 0, 0 }); lp_push(&at, i); }
    witnesses_known(pg, roots.v, roots.n);
    lp_vec(Attested) out = { 0 };
    for (size_t r = 0; r < roots.n; r++) {
        double *cnt = down_from(&d, at.v[r]); lp_idmap *pc = lp_idmap_sized(sizeof(Tally)); tally_under(&d, cnt, pc, NULL);
        for (size_t i = 0; i < lp_idmap_count(pc); i++) { const Tally *t = lp_idmap_at(pc, i); if (t->games <= 0) continue;
            Attested a = { lp_idmap_keys(pc)[i], roots.v[r].id, t->games, t->tokens, t->score, t->position, roots.v[r].trust, roots.v[r].witness }; lp_push(&out, a); }
        lp_idmap_free(pc); free(cnt); }
    lp_vec_free(&roots); lp_vec_free(&at); dag_free(&d);
    *nout = (int)out.n; return out.v;
}

/* ---- down: from a trunk to every claim its records say. What a node is follows from how it was reached:
 *   trunk     [its record (metadata), its content]: the content, or one file
 *   content   the source's files, each a trunk [metadata, content]
 *   file      [metadata, content]: the content where it holds records (or is one), else nothing that says anything
 *   block     the blocks and records it holds
 *   record    the claims it says (and the voice before each); a record holding no claim vertex is a claim said alone */
enum { C_TOP = 1, C_SRC, C_FILE, C_BLOCK, C_REC, C_LEAF };
typedef lp_vec(lp_id) IdVec;
static void child(Dag *d, size_t pi, const lp_vertex *v, uint8_t cls, IdVec *next){
    int fresh; size_t ci = dag_node(d, &v->id, &fresh); Edge *e = dag_edge(d, pi, ci, v);
    if (d->n[ci].alone && v->said == LP_SAID_RECORD) { e->claim = 1; e->rec = 1; }     /* a claim found standing alone already: this is another of its records */
    if (fresh || (cls == C_REC && d->n[ci].cls == C_LEAF)) { d->n[ci].cls = cls; if (cls != C_LEAF) lp_push(next, v->id); }
}
int walk_down(PGconn *pg, const lp_id *top, Dag *d, uint64_t *rows){
    size_t ti = dag_node(d, top, NULL); d->n[ti].cls = C_TOP;
    IdVec front = { 0 }; lp_push(&front, *top); int level = 0; lp_vec(lp_vertex) vx = { 0 };
    while (front.n && level < 64) {
        IdVec next = { 0 };
        for (size_t c0 = 0; c0 < front.n; c0 += 50000) { size_t k = front.n - c0 < 50000 ? front.n - c0 : 50000;
            Args q = { 0 }; arg_ids(&q, front.v + c0, k); PGresult *r = ask_once(pg, "SELECT entity, path FROM laplace_paths($1::blake3[])", &q); *rows += (uint64_t)PQntuples(r);
            for (int j = 0; j < PQntuples(r); j++) {
                size_t ei = (size_t)lp_idmap_find(d->m, col_id(r, j, 0)); uint8_t cls = d->n[ei].cls;
                size_t nv = lp_path_vertices((const uint8_t *)PQgetvalue(r, j, 1), (size_t)PQgetlength(r, j, 1), NULL, 0); lp_vec_reserve(&vx, nv ? nv : 1);
                lp_path_vertices((const uint8_t *)PQgetvalue(r, j, 1), (size_t)PQgetlength(r, j, 1), vx.v, nv);
                int meta = nv && vx.v[0].said == LP_SAID_METADATA; d->n[ei].metadata = (uint8_t)meta;
                if (cls == C_SRC && meta) cls = C_FILE;                       /* a source of one file: the file is its content */
                if (cls == C_FILE && !meta) cls = C_BLOCK;                    /* a file with no metadata is its content */
                if (cls == C_REC) {
                    int has = 0; for (size_t v = 0; v < nv && !has; v++) has = vx.v[v].said == LP_SAID_CLAIM;
                    if (!has) { d->n[ei].alone = 1; for (size_t k2 = d->n[ei].in_head; k2 != NONE; k2 = d->e[k2].in_next) if (d->e[k2].said == LP_SAID_RECORD) { d->e[k2].claim = 1; d->e[k2].rec = 1; } continue; }   /* a claim said alone: its own record where it stands as one (where a record says it, that record's vertex counts it) */
                    for (size_t v = 0; v < nv; v++) { if (vx.v[v].said != LP_SAID_CLAIM) continue;
                        int fresh; size_t ci = dag_node(d, &vx.v[v].id, &fresh); if (fresh) d->n[ci].cls = C_LEAF;
                        Edge *e = dag_edge(d, ei, ci, &vx.v[v]); e->claim = 1;
                        if (v && vx.v[v - 1].said == LP_SAID_VOICE) { e->voiced = 1; e->voice = vx.v[v - 1].id; } }
                    continue; }
                for (size_t v = 0; v < nv; v++) { uint32_t sd = vx.v[v].said;
                    if (sd == LP_SAID_METADATA) continue;
                    if (cls == C_TOP) child(d, ei, &vx.v[v], sd == LP_SAID_HOLDS ? C_BLOCK : sd == LP_SAID_RECORD ? C_REC : C_SRC, &next);
                    else if (cls == C_SRC) { if (sd == LP_SAID_HOLDS) child(d, ei, &vx.v[v], C_BLOCK, &next); else if (sd == LP_SAID_RECORD) child(d, ei, &vx.v[v], C_REC, &next); else child(d, ei, &vx.v[v], C_FILE, &next); }
                    else if (sd == LP_SAID_HOLDS) child(d, ei, &vx.v[v], C_BLOCK, &next);
                    else if (sd == LP_SAID_RECORD) child(d, ei, &vx.v[v], C_REC, &next);
                    /* said nothing: content that says nothing, below a file */ }
            }
            PQclear(r); args_free(&q); }
        lp_vec_free(&front); front.v = next.v; front.n = next.n; front.cap = next.cap; level++;
    }
    lp_vec_free(&vx); lp_vec_free(&front); return level;
}

/* ---- for forget: the claims a trunk's records say (a walk down) */
lp_id *trunk_claims(PGconn *pg, const lp_id *trunk, size_t *n){
    Dag d = { 0 }; uint64_t rows = 0; walk_down(pg, trunk, &d, &rows);
    lp_vec(lp_id) out = { 0 }; lp_idmap *seen = lp_idmap_new();
    for (size_t k = 0; k < d.ne; k++) if (d.e[k].claim) { bool f; lp_idmap_put(seen, &d.n[d.e[k].c].id, &f); if (f) lp_push(&out, d.n[d.e[k].c].id); }
    lp_idmap_free(seen); dag_free(&d); *n = out.n; return out.v;
}
