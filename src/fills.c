/* What follows a phrase, counted by occurrence across every source. Part of the pull, in the engine; the database only
 * fetches paths.
 *   1. The phrase is decomposed on the client into the IDs of its constituents (UAX #29, tier 0 perf-cache).
 *   2. Every path holding them is fetched through the container index and scanned (SIMD) for what follows the run.
 *   3. Leaf to trunk: the parents of those paths, then theirs, one set-based fetch per level, each node once.
 *   4. Trunk to leaf: a node occurs as often as the sum over its parents of the parent's occurrences times the times
 *      the parent holds it, runs included; a sentence repeated in fifty books counts fifty times.
 *   5. Every continuation is weighted by the occurrences of the path it came from. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The DAG above the phrase, by ID: each node its parents and how many times each holds it, and its occurrences once
 * they are counted. */
typedef struct { int parent; uint32_t times; } Up;
typedef struct { lp_vec(Up) up; double occ; int8_t state; } DNode;
static double occ_of(lp_idmap *d, int i){
    DNode *x = lp_idmap_at(d, (size_t)i);
    if (x->state == 2) return x->occ;
    x->state = 1; double o = x->up.n ? 0 : 1;                                   /* no parent: a trunk */
    for (size_t k = 0; k < x->up.n; k++) o += occ_of(d, x->up.v[k].parent) * x->up.v[k].times;
    x = lp_idmap_at(d, (size_t)i); x->occ = o; x->state = 2; return o;
}
typedef struct { double occ; uint64_t paths; } Acc;
typedef struct { lp_id id; Acc a; } Followed;
static int by_occ(const void *a, const void *b){ double x = ((const Followed *)a)->a.occ, y = ((const Followed *)b)->a.occ; return x < y ? 1 : x > y ? -1 : 0; }

static int fills(const char *conninfo, Ctx *ctx, const char *phrase_text, int limit);
int cmd_fills(int argc, char **argv){
    const char *conninfo = laplace_db(), *t0p = NULL; int limit = 12;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "-n", 'i', &limit }, { "-t", 's', &t0p }, { NULL } });
    if (a >= argc) { fprintf(stderr, "usage: laplace fills [-d conninfo] [-n N] phrase\n"); return 2; }
    tier0_open(t0p); table_init(); ctx_open(1);
    return fills(conninfo, CTX[0], argv[a], limit);
}

static int fills(const char *conninfo, Ctx *ctx, const char *phrase_text, int limit){
    double T = now();
    /* 1. the phrase's constituents, computed on the client */
    Ref pr = text_ref(ctx, (const uint8_t *)phrase_text, strlen(phrase_text));
    size_t cap = strlen(phrase_text) + 1; lp_id *ph = malloc(sizeof(lp_id) * cap); int np = (int)table_parts(&pr.id, ph, cap);
    if (!np) { ph[0] = pr.id; np = 1; }

    PGconn *pg = db_connect(conninfo);
    lp_idmap *d = lp_idmap_sized(sizeof(DNode)), *acc = lp_idmap_sized(sizeof(Acc));
    typedef struct { int node; lp_id *f; size_t nf; } Cont; lp_vec(Cont) conts = { 0 };

    /* 2. containers and what follows the phrase in each */
    double t = now();
    /* index keys: the phrase's compositions, never its separators (a lone codepoint such as ' ' is in nearly every
     * path); the SIMD scan below still checks the exact run, separators included */
    Ids keys = { 0 }; for (int i = 0; i < np; i++) if (table_find(&ph[i])) lp_push(&keys, ph[i]);
    if (!keys.n) for (int i = 0; i < np; i++) lp_push(&keys, ph[i]);
    Args a = { 0 }; arg_ids(&a, keys.v, keys.n);
    PGresult *r = ask(pg, "SELECT entity, path, tier FROM laplace_containers($1::blake3[], '{}'::smallint[])", &a);
    Ids front = { 0 };
    for (int i = 0; i < PQntuples(r); i++) {
        lp_path p = col_path(r, i, 1); size_t fc = p.n * 4 + 16; lp_id *out = malloc(sizeof(lp_id) * fc);
        size_t k = lp_path_follows(p, ph, (size_t)np, out, fc);
        if (!k) { free(out); continue; }
        lp_push(&conts, (Cont){ (int)lp_idmap_put(d, col_id(r, i, 0), NULL), out, k < fc ? k : fc }); lp_push(&front, *col_id(r, i, 0));
    }
    size_t direct = conts.n; PQclear(r); double t_cont = now() - t;

    /* 3. leaf to trunk */
    t = now(); int levels = 0; uint64_t fetched = 0;
    while (front.n) {
        levels++; Ids next = { 0 };
        for (size_t i0 = 0; i0 < front.n; i0 += 20000) {
            size_t k = front.n - i0 < 20000 ? front.n - i0 : 20000;
            args_reset(&a); arg_ids(&a, front.v + i0, k);
            PGresult *q = ask(pg, "SELECT entity, id, times, tier FROM laplace_fills($1::blake3[])", &a);
            fetched += (uint64_t)PQntuples(q);
            for (int j = 0; j < PQntuples(q); j++) {
                bool fresh; int pidx = (int)lp_idmap_put(d, col_id(q, j, 0), &fresh);
                if (fresh) lp_push(&next, *col_id(q, j, 0));
                int ci = (int)lp_idmap_find(d, col_id(q, j, 1)); if (ci < 0 || ci == pidx) continue;     /* the child it holds, and how often */
                uint32_t times = (uint32_t)col_int(q, j, 2); DNode *c = lp_idmap_at(d, (size_t)ci); size_t u = 0;
                while (u < c->up.n && c->up.v[u].parent != pidx) u++;
                if (u < c->up.n) c->up.v[u].times += times; else lp_push(&c->up, (Up){ pidx, times });
            }
            PQclear(q);
        }
        fprintf(stderr, "  level %d: %zu nodes looked up, %zu new parents, %.1f ms\n", levels, front.n, next.n, (now() - t) * 1000);
        lp_vec_free(&front); front = next;
    }
    double t_up = now() - t;

    /* 4 and 5 */
    t = now();
    for (size_t i = 0; i < conts.n; i++) {
        double w = occ_of(d, conts.v[i].node);
        for (size_t j = 0; j < conts.v[i].nf; j++) { Acc *x = lp_idmap_get(acc, &conts.v[i].f[j], NULL); x->occ += w; x->paths++; }
    }
    size_t nacc = lp_idmap_count(acc); Followed *f = malloc(sizeof(Followed) * (nacc + 1));
    for (size_t i = 0; i < nacc; i++) { f[i].id = *lp_idmap_key(acc, i); f[i].a = *(Acc *)lp_idmap_at(acc, i); }
    lp_sort(f, nacc, sizeof(Followed), by_occ);
    double t_w = now() - t;

    /* the top continuations as text: their paths fetched together, a level at a time */
    Reader *rd = reader_new(pg);
    for (size_t i = 0; i < nacc && (int)i < limit; i++) reader_want(rd, &f[i].id);
    printf("%-24s %14s %10s\n", "follows", "occurrences", "paths");
    for (size_t i = 0; i < nacc && (int)i < limit; i++) {
        char *tx = reader_text(rd, &f[i].id, 60);
        printf("%-24s %14.0f %10llu\n", tx, f[i].a.occ, (unsigned long long)f[i].a.paths); free(tx);
    }
    reader_free(rd);
    printf("\n%zu paths hold the phrase; %d levels up to the trunks, %zu nodes, %llu parent paths fetched\n", direct, levels, lp_idmap_count(d), (unsigned long long)fetched);
    printf("containers %.1f ms   leaf to trunk %.1f ms   weighting %.2f ms   total %.1f ms\n", t_cont * 1000, t_up * 1000, t_w * 1000, (now() - T) * 1000);
    for (size_t i = 0; i < lp_idmap_count(d); i++) lp_vec_free(&((DNode *)lp_idmap_at(d, i))->up);
    for (size_t i = 0; i < conts.n; i++) free(conts.v[i].f);
    lp_vec_free(&conts); lp_vec_free(&keys); lp_vec_free(&front); args_free(&a); lp_idmap_free(d); lp_idmap_free(acc); free(f); free(ph);
    PQfinish(pg); return 0;
}
