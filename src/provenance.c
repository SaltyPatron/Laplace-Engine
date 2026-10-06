/* Provenance by containment (Engine#22), read side by side with the attestation table.
 *
 *   laplace held [-d conninfo] [--table] [--files] [--tsv FILE] PART...
 *       The strands whose parts are these (each PART a text, LIST:KEY for a type of the highway's LIST, ? for a part
 *       left open), found as one set read over the claims, and who said each and how many times:
 *         by containment (the default): a walk up the container index from each strand to the records that hold it as
 *         a claim (a vertex of the record said to be one), and on up to the files and the trunks above them. A trunk
 *         that holds nothing above it is the witness; the count is read off the tree: games, the record occurrences
 *         under the trunk that assert the strand; tokens, the same with each record's run (how many times it says it).
 *         --files gives the count under each file as well.
 *         --table: the attestation table's rows for the same strands (witness, games, score, position), as built.
 *       --tsv FILE: one line a strand and witness, for comparing the two: claim, witness, games, score, position,
 *       rating, deviation, volatility, matches.
 *
 *   laplace replay [-d conninfo] [--write] [--tsv FILE]
 *       Every witness's trunk walked down to the strands its records assert, each counted off the tree; each strand
 *       played as the ingest plays it (one matchup per strand and witness, at the witness's trust, from the stock
 *       default), and the standing compared bit for bit with the one recorded. --write puts the replayed standings in
 *       place (after a forget: the standings the forgotten trunk touched). --tsv: claim, witness, games, tokens.
 *
 * SQL fetches sets of paths; the walk, the counts and the plays are here. */
#include "engine.h"
#include <locale.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a walked DAG: its nodes, and its edges parent -> child with the times the child stands in the parent */
typedef struct { lp_id parent, child; uint64_t times; } Edge;
typedef struct { lp_id id; int nparents; uint8_t first_said, has_path, file; } WNode;
typedef struct { lp_idmap *m; WNode *n; size_t nn, cn; Edge *e; size_t ne, ce; } Dag;
static size_t dag_node(Dag *d, const lp_id *id){
    if (!d->m) d->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(d->m, id, &fresh);
    if (fresh) { lp_reserve((void **)&d->n, &d->cn, d->nn + 1, sizeof(WNode)); memset(&d->n[d->nn], 0, sizeof(WNode)); d->n[d->nn].id = *id; d->nn++; }
    return i;
}
static void dag_edge(Dag *d, const lp_id *p, const lp_id *c, uint64_t times){
    lp_reserve((void **)&d->e, &d->ce, d->ne + 1, sizeof(Edge)); d->e[d->ne++] = (Edge){ *p, *c, times };
}
static void dag_free(Dag *d){ lp_idmap_free(d->m); free(d->n); free(d->e); memset(d, 0, sizeof *d); }

/* Up from the strands: one set read a level (every leaf of the container index at once), each holder an edge to the
 * keys of that level it holds. At the first level only a vertex said to be a claim counts: the strand asserted by a
 * record, not the same content standing elsewhere (a tree's [REL, word] is content, not an assertion). */
static int walk_up(Dag *d, const lp_id *strands, size_t ns, uint64_t *rows){
    lp_idmap *front = lp_idmap_new(); for (size_t i = 0; i < ns; i++) { lp_idmap_put(front, &strands[i], NULL); dag_node(d, &strands[i]); }
    int level = 0;
    while (lp_idmap_count(front) && level < 48) {
        int nh = 0; Hold *h = holds_any(lp_idmap_keys(front), (int)lp_idmap_count(front), &nh); *rows += (uint64_t)nh;
        lp_idmap *next = lp_idmap_new(); lp_vec(lp_vertex) vx = { 0 };
        for (int j = 0; j < nh; j++) {
            size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0); lp_vec_reserve(&vx, nv ? nv : 1); lp_path_vertices(h[j].path, (size_t)h[j].path_len, vx.v, nv);
            int any = 0;
            for (size_t v = 0; v < nv; v++) { if (lp_idmap_find(front, &vx.v[v].id) < 0) continue;
                if (level == 0 && vx.v[v].said != LP_SAID_CLAIM) continue;
                dag_edge(d, &h[j].entity, &vx.v[v].id, vx.v[v].run); any = 1;
                d->n[dag_node(d, &vx.v[v].id)].nparents++; }
            if (!any) continue;
            bool fresh_dag = lp_idmap_find(d->m, &h[j].entity) < 0; size_t pi = dag_node(d, &h[j].entity);
            if (!d->n[pi].has_path) { d->n[pi].has_path = 1; d->n[pi].first_said = nv ? (uint8_t)vx.v[0].said : 0; }
            if (fresh_dag) lp_idmap_put(next, &h[j].entity, NULL);
        }
        lp_vec_free(&vx); holds_free(h, nh); lp_idmap_free(front); front = next; level++;
    }
    lp_idmap_free(front);
    for (size_t i = 0; i < d->nn; i++) d->n[i].file = d->n[i].has_path && d->n[i].first_said == LP_SAID_METADATA && d->n[i].nparents;
    return level;
}
/* How many times each node stands under `top`, top down in topological order (edges parent -> child, times each). */
static double *down_from(const Dag *d, const lp_id *top){
    size_t n = d->nn; double *cnt = calloc(n ? n : 1, sizeof(double)); int *indeg = calloc(n ? n : 1, sizeof(int));
    size_t *head = malloc(sizeof(size_t) * (n ? n : 1)), *link = malloc(sizeof(size_t) * (d->ne ? d->ne : 1));
    for (size_t i = 0; i < n; i++) head[i] = (size_t)-1;
    for (size_t k = 0; k < d->ne; k++) { size_t p = (size_t)lp_idmap_find(d->m, &d->e[k].parent), c = (size_t)lp_idmap_find(d->m, &d->e[k].child); link[k] = head[p]; head[p] = k; indeg[c]++; }
    size_t *q = malloc(sizeof(size_t) * (n ? n : 1)), qh = 0, qt = 0; for (size_t i = 0; i < n; i++) if (!indeg[i]) q[qt++] = i;
    int64_t t = lp_idmap_find(d->m, top); if (t >= 0) cnt[t] = 1;
    while (qh < qt) { size_t p = q[qh++]; for (size_t k = head[p]; k != (size_t)-1; k = link[k]) { size_t c = (size_t)lp_idmap_find(d->m, &d->e[k].child); cnt[c] += cnt[p] * (double)d->e[k].times; if (!--indeg[c]) q[qt++] = c; } }
    free(indeg); free(head); free(link); free(q); return cnt;
}
/* games: the record occurrences under top that assert each node; tokens: with each one's run (indexed as the DAG's nodes) */
static void sums_under(const Dag *d, const double *cnt, double *games, double *tokens){
    memset(games, 0, sizeof(double) * d->nn); memset(tokens, 0, sizeof(double) * d->nn);
    for (size_t k = 0; k < d->ne; k++) { double c = cnt[lp_idmap_find(d->m, &d->e[k].parent)]; size_t ch = (size_t)lp_idmap_find(d->m, &d->e[k].child); games[ch] += c; tokens[ch] += c * (double)d->e[k].times; }
}
/* A file's name, from the OS's record in its metadata tree: [filename, NAME] */
static char *file_name(Reader *rd, const lp_id *file){
    lp_id at = *file, kid[64];
    for (int depth = 0; depth < 8; depth++) {
        size_t n = reader_parts(rd, &at, kid, 64); if (!n) break;
        for (size_t i = 0; i < n && i < 64; i++) { lp_id two[4]; size_t m = reader_parts(rd, &kid[i], two, 4);
            if (m == 2) { char *k = reader_text(rd, &two[0], 32); int is = !strcmp(k, "filename"); free(k); if (is) return reader_text(rd, &two[1], 200); } }
        at = kid[0]; }
    return strdup("(a file)");
}

/* ---- the strands named */
static lp_id *strands_named(PGconn *pg, int argc, char **argv, int a, int *ns, Hold **hs, int *nhs){
    Ctx *c = lp_text_new(T0); if (!HW) HW = lp_highway_map(NULL);
    int np = argc - a; lp_id part[MAXPARTS]; int have[MAXPARTS], nk = 0; lp_id key[MAXPARTS];
    for (int i = 0; i < np && i < MAXPARTS; i++) { const char *p = argv[a + i]; have[i] = strcmp(p, "?") != 0; if (!have[i]) continue;
        const char *colon = strchr(p, ':'); int typed = 0;
        if (colon && colon > p && colon[1]) { char list[64]; snprintf(list, sizeof list, "%.*s", (int)(colon - p), p); Ref r = highway_typed(list, (const uint8_t *)colon + 1, strlen(colon + 1), &typed); if (typed) part[i] = r.id; }
        if (!typed) part[i] = lp_text_decompose(c, (const uint8_t *)p, strlen(p), NULL, NULL).id;
        key[nk++] = part[i];
        char hx[33]; id_text(&part[i], hx); printf("  part %d   %-24s %s%s\n", i + 1, p, hx, typed ? "   (a type of the highway)" : ""); }
    (void)pg;
    int nh = 0; Hold *h = holds_above(key, nk, -1, 0, 1, &nh); int w = 0; Ids ids = { 0 };
    for (int j = 0; j < nh; j++) { if (!h[j].claim) continue; lp_path p = lp_path_of(h[j].path, (size_t)h[j].path_len); size_t n = path_into(&ids, p);
        if ((int)n != np) continue; int fits = 1; for (int i = 0; i < np && fits; i++) if (have[i] && memcmp(&ids.v[i], &part[i], 16)) fits = 0;
        if (fits) { Hold x = h[j]; h[j] = h[w]; h[w++] = x; } }
    lp_vec_free(&ids);
    lp_id *out = malloc(sizeof(lp_id) * (size_t)(w ? w : 1)); for (int j = 0; j < w; j++) out[j] = h[j].entity;
    *ns = w; *hs = h; *nhs = nh; return out;
}
static int by_hold_id(const void *a, const void *b){ return memcmp(&((const Hold *)a)->entity, &((const Hold *)b)->entity, 16); }

int cmd_held(int argc, char **argv){
    const char *conninfo = laplace_db(), *tsv = NULL; int table = 0, files = 0;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "--table", 'b', &table }, { "--files", 'b', &files }, { "--tsv", 's', &tsv }, { NULL } });
    if (a >= argc) { fprintf(stderr, "usage: laplace held [-d conninfo] [--table] [--files] [--tsv FILE] PART...   (LIST:KEY a highway type, ? open)\n"); return 2; }
    setlocale(LC_NUMERIC, "en_US.UTF-8"); tier0_open(NULL);
    PGconn *pg = db_connect(conninfo); double T = now();
    printf("laplace held   %s   %s\n", PQdb(pg), table ? "by the attestation table, as built" : "by containment: a walk up the container index");
    int ns = 0, nhs = 0; Hold *hs = NULL; lp_id *s = strands_named(pg, argc, argv, a, &ns, &hs, &nhs);
    qsort(hs, (size_t)ns, sizeof(Hold), by_hold_id); for (int i = 0; i < ns; i++) s[i] = hs[i].entity;
    double t_find = now() - T;
    printf("  %'d strands of that shape, found as one set read over the claims (%.1f ms)\n", ns, t_find * 1000);
    FILE *out = tsv ? fopen(tsv, "w") : NULL; if (tsv && !out) { perror(tsv); return 1; }
    Reader *rd = reader_new(pg); int show = ns <= 8;
    if (show) for (int i = 0; i < ns; i++) for (int p = 0; p < 1; p++) reader_want(rd, &s[i]);
    double tot_games = 0, tot_tokens = 0; uint64_t rows = 0; int levels = 0; double t0 = now();
    if (table) {
        Args q = { 0 }; arg_ids(&q, s, (size_t)ns);
        PGresult *r = ask_once(pg, "SELECT claim, witness, score, position, games FROM attestation WHERE claim = ANY($1::blake3[]) ORDER BY claim, witness", &q);
        rows = (uint64_t)PQntuples(r);
        lp_idmap *at = lp_idmap_new(); for (int i = 0; i < ns; i++) lp_idmap_put(at, &s[i], NULL);
        for (int j = 0; j < PQntuples(r); j++) {
            const lp_id *cl = col_id(r, j, 0), *wi = col_id(r, j, 1); int64_t i = lp_idmap_find(at, cl); const Hold *x = i >= 0 ? &hs[i] : NULL;
            float sc; { uint32_t u = (uint32_t)lp_be(PQgetvalue(r, j, 2), 4); memcpy(&sc, &u, 4); }
            int pos_null = PQgetisnull(r, j, 3); long long pos = pos_null ? 0 : (long long)col_int(r, j, 3), games = (long long)col_int(r, j, 4);
            tot_games += (double)games;
            char ch[33], wh[33]; id_text(cl, ch); id_text(wi, wh);
            char posb[24]; if (pos_null) snprintf(posb, sizeof posb, "\\N"); else snprintf(posb, sizeof posb, "%lld", pos);
            if (out) fprintf(out, "%s\t%s\t%lld\t%.9g\t%s\t%.17g\t%.17g\t%.17g\t%d\n", ch, wh, games, (double)sc, posb, x ? x->r.rating : 0, x ? x->r.deviation : 0, x ? x->r.volatility : 0, x ? x->matches : 0);
            if (show) { char *tx = reader_text(rd, cl, 120), *wt = reader_text(rd, wi, 80);
                printf("\n  %s   %s\n    witness %s   games %lld   score %.3g   position %s\n    standing %.6f / %.6f / %.6f, %d matches\n", tx, ch, wt, games, (double)sc, pos_null ? "none" : "given", x ? x->r.rating : 0, x ? x->r.deviation : 0, x ? x->r.volatility : 0, x ? x->matches : 0); free(tx); free(wt); }
        }
        PQclear(r); args_free(&q); lp_idmap_free(at);
    } else {
        Dag d = { 0 }; levels = walk_up(&d, s, (size_t)ns, &rows);
        lp_vec(size_t) roots = { 0 }, fl = { 0 }; for (size_t i = 0; i < d.nn; i++) { if (!d.n[i].nparents && i >= (size_t)ns) lp_push(&roots, i); if (d.n[i].file) lp_push(&fl, i); }
        /* the witnesses: the trunks nothing holds, as the witness table keys them */
        lp_id *rid = malloc(sizeof(lp_id) * (roots.n + 1)); for (size_t r = 0; r < roots.n; r++) rid[r] = d.n[roots.v[r]].id;
        double *trust = calloc(roots.n + 1, sizeof(double)); uint8_t *is_w = calloc(roots.n + 1, 1);
        { Args q = { 0 }; arg_ids(&q, rid, roots.n); PGresult *r = ask_once(pg, "SELECT id, trust FROM witness WHERE id = ANY($1::blake3[])", &q);
          for (int j = 0; j < PQntuples(r); j++) for (size_t k = 0; k < roots.n; k++) if (!memcmp(col_id(r, j, 0), &rid[k], 16)) { is_w[k] = 1; trust[k] = col_f64(r, j, 1); }
          PQclear(r); args_free(&q); }
        double **cnt = malloc(sizeof(double *) * (roots.n + 1)), **gs = malloc(sizeof(double *) * (roots.n + 1)), **ts = malloc(sizeof(double *) * (roots.n + 1));
        for (size_t r = 0; r < roots.n; r++) { cnt[r] = down_from(&d, &rid[r]); gs[r] = malloc(sizeof(double) * d.nn); ts[r] = malloc(sizeof(double) * d.nn); sums_under(&d, cnt[r], gs[r], ts[r]); }
        double **fcnt = NULL; char **fname = NULL;
        double **fg = NULL, **ft = NULL;
        if (files) { fcnt = malloc(sizeof(double *) * (fl.n + 1)); fg = malloc(sizeof(double *) * (fl.n + 1)); ft = malloc(sizeof(double *) * (fl.n + 1)); fname = malloc(sizeof(char *) * (fl.n + 1));
            for (size_t f = 0; f < fl.n; f++) { fcnt[f] = down_from(&d, &d.n[fl.v[f]].id); fg[f] = malloc(sizeof(double) * d.nn); ft[f] = malloc(sizeof(double) * d.nn); sums_under(&d, fcnt[f], fg[f], ft[f]); fname[f] = file_name(rd, &d.n[fl.v[f]].id); } }
        double t_walk = now() - t0;
        if (show || roots.n <= 4) for (size_t r = 0; r < roots.n; r++) { char hx[33]; id_text(&rid[r], hx); lp_id first[2]; size_t m = reader_parts(rd, &rid[r], first, 2); char *tx = m ? reader_text(rd, &first[0], 80) : strdup("?");
            printf("  trunk %s   \"%s\"   %s\n", hx, tx, is_w[r] ? "the witness (witness table: its trust)" : "NOT in the witness table"); free(tx); }
        for (int i = 0; i < ns; i++) {
            char ch[33]; id_text(&s[i], ch);
            if (show) { char *tx = reader_text(rd, &s[i], 120); printf("\n  %s   %s\n    standing %.6f / %.6f / %.6f, %d matches\n", tx, ch, hs[i].r.rating, hs[i].r.deviation, hs[i].r.volatility, hs[i].matches); free(tx); }
            for (size_t r = 0; r < roots.n; r++) { size_t si = (size_t)lp_idmap_find(d.m, &s[i]); double g = gs[r][si], tk = ts[r][si]; if (g <= 0) continue; tot_games += g; tot_tokens += tk;
                char wh[33]; id_text(&rid[r], wh);
                if (show) printf("    witness %.12s…   games %.0f (records that assert it)   tokens %.0f   score 1 (no outcome in M: an affirmation)   position none\n", wh, g, tk);
                if (out) fprintf(out, "%s\t%s\t%.0f\t1\t\\N\t%.17g\t%.17g\t%.17g\t%d\n", ch, wh, g, hs[i].r.rating, hs[i].r.deviation, hs[i].r.volatility, hs[i].matches); }
            if (files && show) for (size_t f = 0; f < fl.n; f++) { size_t si = (size_t)lp_idmap_find(d.m, &s[i]); double g = fg[f][si], tk = ft[f][si]; if (g > 0) printf("      under %-28s games %.0f   tokens %.0f\n", fname[f], g, tk); }
        }
        printf("\n  walked up %d levels: %'zu nodes, %'zu edges, %'llu holder rows read, %zu trunks at the top (%.1f ms)\n", levels, d.nn, d.ne, (unsigned long long)rows, roots.n, t_walk * 1000);
        for (size_t r = 0; r < roots.n; r++) { free(cnt[r]); free(gs[r]); free(ts[r]); } free(cnt); free(gs); free(ts); free(rid); free(trust); free(is_w);
        if (files) { for (size_t f = 0; f < fl.n; f++) { free(fcnt[f]); free(fg[f]); free(ft[f]); free(fname[f]); } free(fcnt); free(fg); free(ft); free(fname); }
        lp_vec_free(&roots); lp_vec_free(&fl); dag_free(&d);
    }
    printf("\n  %'d strands: %'.0f games%s; %'llu rows read   %.1f ms in all\n", ns, tot_games, table ? "" : "", (unsigned long long)rows, (now() - T) * 1000);
    if (!table) printf("  %'.0f tokens\n", tot_tokens);
    if (out) fclose(out);
    reader_free(rd); holds_free(hs, nhs); free(s); PQfinish(pg);
    return 0;
}

/* ---- replay: down from each trunk to what its records assert */
typedef struct { lp_id id; double games, tokens; } Count2;
int cmd_replay(int argc, char **argv){
    const char *conninfo = laplace_db(), *tsv = NULL; int write = 0;
    opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "--write", 'b', &write }, { "--tsv", 's', &tsv }, { NULL } });
    setlocale(LC_NUMERIC, "en_US.UTF-8"); tier0_open(NULL);
    PGconn *pg = db_connect(conninfo); double T = now();
    printf("laplace replay   %s   standings from containment: each trunk walked down to the strands its records assert\n", PQdb(pg));
    PGresult *wr = ask_once(pg, "SELECT id, trust FROM witness ORDER BY id", NULL); int nw = PQntuples(wr);
    FILE *out = tsv ? fopen(tsv, "w") : NULL;
    uint64_t same = 0, differ = 0, missing = 0, played = 0, fetched = 0; double tg = 0, tt = 0;
    lp_idmap *stand = lp_idmap_sized(sizeof(lp_rating) + sizeof(int)); lp_vec(lp_id) order = { 0 };
    for (int w = 0; w < nw; w++) {
        lp_id wid = *col_id(wr, w, 0); double trust = col_f64(wr, w, 1); char wh[33]; id_text(&wid, wh);
        /* the DAG down from the trunk: every path but a metadata tree's; below a record only what it says is a claim */
        Dag d = { 0 }; dag_node(&d, &wid); lp_vec(lp_id) front = { 0 }; lp_push(&front, wid); lp_idmap *kind = lp_idmap_sized(1);   /* 0 above records, 1 a record, 2 what a record asserts */
        { bool f; *(uint8_t *)lp_idmap_get(kind, &wid, &f) = 0; }
        lp_idmap *strands = lp_idmap_new(); int level = 0;
        while (front.n && level < 64) {
            Args q = { 0 }; arg_ids(&q, front.v, front.n); PGresult *r = ask_once(pg, "SELECT entity, path FROM laplace_paths($1::blake3[])", &q); fetched += (uint64_t)PQntuples(r);
            lp_vec(lp_id) next = { 0 }; lp_vec(lp_vertex) vx = { 0 };
            for (int j = 0; j < PQntuples(r); j++) {
                lp_id e = *col_id(r, j, 0); uint8_t k = *(uint8_t *)lp_idmap_lookup(kind, &e);
                size_t nv = lp_path_vertices((const uint8_t *)PQgetvalue(r, j, 1), (size_t)PQgetlength(r, j, 1), NULL, 0); lp_vec_reserve(&vx, nv ? nv : 1);
                lp_path_vertices((const uint8_t *)PQgetvalue(r, j, 1), (size_t)PQgetlength(r, j, 1), vx.v, nv);
                int all_claims = nv > 0; for (size_t v = 0; v < nv; v++) all_claims &= vx.v[v].said == LP_SAID_CLAIM;
                if (k == 2) {                                                 /* what a record asserts: a path of strands, or one strand */
                    if (all_claims) for (size_t v = 0; v < nv; v++) { dag_edge(&d, &e, &vx.v[v].id, vx.v[v].run); dag_node(&d, &vx.v[v].id); lp_idmap_put(strands, &vx.v[v].id, NULL); }
                    else lp_idmap_put(strands, &e, NULL);
                    continue; }
                for (size_t v = 0; v < nv; v++) { uint32_t sd = vx.v[v].said; uint8_t ck;
                    if (k == 0) { if (sd == LP_SAID_METADATA) continue; ck = sd == LP_SAID_RECORD ? 1 : 0; }
                    else { if (sd != LP_SAID_CLAIM) continue; ck = 2; }
                    dag_edge(&d, &e, &vx.v[v].id, vx.v[v].run);
                    bool fresh = lp_idmap_find(d.m, &vx.v[v].id) < 0; dag_node(&d, &vx.v[v].id);
                    if (fresh) { bool f; *(uint8_t *)lp_idmap_get(kind, &vx.v[v].id, &f) = ck; lp_push(&next, vx.v[v].id); } }
            }
            lp_vec_free(&vx); PQclear(r); args_free(&q); lp_vec_free(&front); front.v = next.v; front.n = next.n; front.cap = next.cap; level++;
        }
        lp_vec_free(&front); lp_idmap_free(kind);
        double *cnt = down_from(&d, &wid), *gsum = malloc(sizeof(double) * (d.nn + 1)), *tsum = malloc(sizeof(double) * (d.nn + 1)); sums_under(&d, cnt, gsum, tsum);
        size_t nsd = lp_idmap_count(strands); const lp_id *sid = lp_idmap_keys(strands);
        /* each strand: games off the tree, then its matchup, as the ingest plays it (db.c: once per strand and witness) */
        double dev0 = trust < 0 ? -trust : trust; dev0 = dev0 == 0.0 ? 350.0 : lp_trust_deviation(dev0); if (dev0 < 30.0) dev0 = 30.0;
        for (size_t i = 0; i < nsd; i++) {
            Count2 c = { sid[i], 0, 0 }; int64_t si = lp_idmap_find(d.m, &sid[i]);
            c.games = gsum[si]; c.tokens = tsum[si];                          /* each record occurrence that asserts it, and with its run */
            if (c.games <= 0) continue; tg += c.games; tt += c.tokens;
            bool fresh; uint8_t *slot = lp_idmap_get(stand, &sid[i], &fresh); lp_rating *rt = (lp_rating *)slot; int *m = (int *)(slot + sizeof(lp_rating));
            if (fresh) { *rt = (lp_rating){ 1500.0f, dev0, 0.06 }; *m = 0; lp_push(&order, sid[i]); }
            lp_attest(rt, trust, 1.0f, 1500.0f, 0.5, 30.0); (*m)++; played++;
            if (out) { char ch[33]; id_text(&sid[i], ch); fprintf(out, "%s\t%s\t%.0f\t%.0f\n", ch, wh, c.games, c.tokens); }
        }
        printf("  witness %s   trust %.3f   %d levels down, %'zu nodes, %'zu strands asserted\n", wh, trust, level, d.nn, nsd);
        free(cnt); free(gsum); free(tsum); lp_idmap_free(strands); dag_free(&d);
    }
    PQclear(wr);
    /* the replayed standings against the recorded ones, bit for bit */
    for (size_t i0 = 0; i0 < order.n; i0 += 50000) { size_t k = order.n - i0 < 50000 ? order.n - i0 : 50000;
        Args q = { 0 }; arg_ids(&q, order.v + i0, k);
        PGresult *r = ask_once(pg, "SELECT claim, rating, deviation, volatility, matches FROM consensus WHERE claim = ANY($1::blake3[])", &q);
        lp_idmap *got = lp_idmap_new();
        for (int j = 0; j < PQntuples(r); j++) { const lp_id *cl = col_id(r, j, 0); lp_idmap_put(got, cl, NULL); uint8_t *slot = lp_idmap_lookup(stand, cl); if (!slot) continue;
            lp_rating *rt = (lp_rating *)slot; int m = *(int *)(slot + sizeof(lp_rating)); double a3[3] = { col_f64(r, j, 1), col_f64(r, j, 2), col_f64(r, j, 3) };
            if (!memcmp(&a3[0], &rt->rating, 8) && !memcmp(&a3[1], &rt->deviation, 8) && !memcmp(&a3[2], &rt->volatility, 8) && col_int(r, j, 4) == m) same++; else differ++; }
        for (size_t i = i0; i < i0 + k; i++) if (lp_idmap_find(got, &order.v[i]) < 0) missing++;
        PQclear(r); args_free(&q); lp_idmap_free(got); }
    printf("\n  %'llu strand-witness matchups played from %'llu paths fetched; %'.0f games, %'.0f tokens\n", (unsigned long long)played, (unsigned long long)fetched, tg, tt);
    printf("  replayed against recorded: %'llu identical bit for bit, %'llu different, %'llu with no standing recorded\n", (unsigned long long)same, (unsigned long long)differ, (unsigned long long)missing);
    if (write && order.n) {
        exec(pg, "BEGIN"); exec(pg, "CREATE TEMP TABLE replayed (claim blake3, rating float8, deviation float8, volatility float8, matches int) ON COMMIT DROP");
        lp_copy c; copy_open(&c, pg, "COPY replayed FROM STDIN (FORMAT binary)");
        for (size_t i = 0; i < order.n; i++) { uint8_t *slot = lp_idmap_lookup(stand, &order.v[i]); lp_rating *rt = (lp_rating *)slot; int m = *(int *)(slot + sizeof(lp_rating));
            lp_copy_row(&c, 5); lp_copy_id(&c, &order.v[i]); lp_copy_f64(&c, rt->rating); lp_copy_f64(&c, rt->deviation); lp_copy_f64(&c, rt->volatility); lp_copy_i32(&c, m); }
        copy_close(&c);
        exec(pg, "INSERT INTO consensus SELECT * FROM replayed ON CONFLICT (claim) DO UPDATE SET rating = excluded.rating, deviation = excluded.deviation, volatility = excluded.volatility, matches = excluded.matches");
        exec(pg, "COMMIT"); printf("  written: %'zu standings\n", order.n); }
    printf("  %.1f s\n", now() - T);
    if (out) fclose(out);
    lp_idmap_free(stand); lp_vec_free(&order); PQfinish(pg);
    return differ || missing ? 1 : 0;
}
