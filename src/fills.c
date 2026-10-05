/* What follows a phrase, counted by occurrence across every source. Part of the pull, in the engine; the database only
 * fetches paths.
 *   1. The phrase is decomposed on the client into the IDs of its constituents (UAX #29, tier 0 perf-cache).
 *   2. Every path holding them is fetched through the container index and scanned (SIMD) for what follows the run.
 *   3. Leaf to trunk: the parents of those paths, then theirs, one set-based fetch per level, each node once.
 *   4. Trunk to leaf: a node occurs as often as the sum over its parents of the parent's occurrences times the times
 *      the parent holds it, runs included; a sentence repeated in fifty books counts fifty times.
 *   5. Every continuation is weighted by the occurrences of the path it came from. */
#include "engine.h"
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int parent; uint32_t times; } Up;
typedef struct { lp_id id; Up *up; int nup, cup; double occ; int8_t state; } DNode;
typedef struct { lp_id *id; int *idx; uint64_t cap, n; } IdMap;               /* ID → node index */

static uint64_t k64(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 3, 8); return k; }
static int map_get(IdMap *m, const lp_id *id, int add, DNode **nodes, int *nn, int *cn){
    if (add && (m->n + 1) * 2 > m->cap) {
        uint64_t oc = m->cap; lp_id *oi = m->id; int *ox = m->idx; m->cap = oc ? oc * 2 : 4096;
        m->id = calloc(m->cap, sizeof(lp_id)); m->idx = malloc(sizeof(int) * m->cap); for (uint64_t i = 0; i < m->cap; i++) m->idx[i] = -1;
        for (uint64_t i = 0; i < oc; i++) if (ox[i] >= 0) { uint64_t k = k64(&oi[i]) & (m->cap - 1); while (m->idx[k] >= 0) k = (k + 1) & (m->cap - 1); m->id[k] = oi[i]; m->idx[k] = ox[i]; }
        free(oi); free(ox);
    }
    if (!m->cap) return -1;
    uint64_t k = k64(id) & (m->cap - 1);
    while (m->idx[k] >= 0) { if (!memcmp(&m->id[k], id, 16)) return m->idx[k]; k = (k + 1) & (m->cap - 1); }
    if (!add) return -1;
    if (*nn == *cn) { *cn = *cn ? *cn * 2 : 4096; *nodes = xrealloc(*nodes, sizeof(DNode) * *cn); }
    memset(&(*nodes)[*nn], 0, sizeof(DNode)); (*nodes)[*nn].id = *id;
    m->id[k] = *id; m->idx[k] = (*nn)++; m->n++; return m->idx[k];
}
static double occ_of(DNode *d, int i){
    if (d[i].state == 2) return d[i].occ;
    d[i].state = 1; double o = d[i].nup ? 0 : 1;                                  /* no parent: a trunk */
    for (int k = 0; k < d[i].nup; k++) o += occ_of(d, d[i].up[k].parent) * d[i].up[k].times;
    d[i].occ = o; d[i].state = 2; return o;
}
/* EWKB of a path (PostGIS binary output) → its vertices. */


typedef struct { lp_id id; double occ; uint64_t paths; } Acc;

static int fills(const char *conninfo, Ctx *ctx, const char *phrase_text, int limit);
int cmd_fills(int argc, char **argv){
    const char *conninfo = laplace_db(), *t0p = NULL; int limit = 12, a = 1;
    for (; a < argc - 1 && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) t0p = argv[++a];
    }
    if (a >= argc) { fprintf(stderr, "usage: laplace fills [-d conninfo] [-n N] phrase\n"); return 2; }
    tier0_open(t0p); table_init(); ctx_open(1);
    return fills(conninfo, CTX[0], argv[a], limit);
}

static int fills(const char *conninfo, Ctx *ctx, const char *phrase_text, int limit){
    double T = now();
    /* 1. the phrase's constituents, computed on the client */
    Ref pr = text_ref(ctx, (const uint8_t *)phrase_text, strlen(phrase_text));
    Node *pn = table_find(&pr.id); lp_id *ph; int np;
    if (!pn) { ph = &pr.id; np = 1; }
    else { ph = malloc(sizeof(lp_id) * pn->len); np = (int)table_parts(&pr.id, ph, pn->len); }

    PGconn *pg = db_connect(conninfo);
    DNode *d = NULL; int nn = 0, cn = 0; IdMap map = { 0 };
    Acc *acc = NULL; uint64_t nacc = 0, cacc = 0; IdMap amap = { 0 }; DNode *adummy = NULL; int an = 0, ac = 0;
    typedef struct { int node; lp_id *f; size_t nf; } Cont; Cont *conts = NULL; int ncont = 0, ccont = 0;

    /* 2. containers and what follows the phrase in each */
    double t = now();
    /* index keys: the phrase's compositions, never its separators (a lone codepoint such as ' ' is in nearly every
     * path); the SIMD scan below still checks the exact run, separators included */
    lp_id *keys = malloc(sizeof(lp_id) * np); int nk = 0;
    for (int i = 0; i < np; i++) if (table_find(&ph[i])) keys[nk++] = ph[i];
    if (!nk) { memcpy(keys, ph, sizeof(lp_id) * np); nk = np; }
    uint8_t *ab = malloc(20 + 20 * (size_t)nk); size_t al = ids_param(ab, keys, (uint32_t)nk);
    const char *v1[1] = { (const char *)ab }; int l1[1] = { (int)al }, f1[1] = { 1 };
    PGresult *r = PQexecParams(pg, "SELECT entity, path, tier FROM laplace_containers($1::blake3[], '{}'::smallint[])", 1, NULL, v1, l1, f1, 1);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "containers: %s", PQerrorMessage(pg)); return 1; }
    lp_id *front = malloc(sizeof(lp_id) * (PQntuples(r) + 1)); int nf = 0;
    for (int i = 0; i < PQntuples(r); i++) {
        const uint8_t *e = (const uint8_t *)PQgetvalue(r, i, 1); size_t el = (size_t)PQgetlength(r, i, 1);
        const uint8_t *vx; size_t nv = lp_ewkb_vertices(e, el, &vx); size_t cap = nv * 4 + 16; lp_id *out = malloc(sizeof(lp_id) * cap);
        size_t k = lp_follows(e, el, ph, (size_t)np, out, cap);
        if (!k) { free(out); continue; }
        lp_id eid; memcpy(eid.b, PQgetvalue(r, i, 0), 16);
        int idx = map_get(&map, &eid, 1, &d, &nn, &cn);
        if (ncont == ccont) { ccont = ccont ? ccont * 2 : 256; conts = xrealloc(conts, sizeof(Cont) * ccont); }
        conts[ncont++] = (Cont){ idx, out, k < cap ? k : cap }; front[nf++] = eid;
    }
    int direct = ncont; PQclear(r); double t_cont = now() - t;

    /* 3. leaf to trunk */
    t = now(); int levels = 0; uint64_t fetched = 0;
    while (nf) {
        levels++; lp_id *next = malloc(sizeof(lp_id) * 1024); int nx = 0, cx = 1024;
        for (int i0 = 0; i0 < nf; i0 += 20000) {
            int k = nf - i0 < 20000 ? nf - i0 : 20000;
            uint8_t *pb = malloc(20 + 20 * (size_t)k); size_t pl = ids_param(pb, front + i0, (uint32_t)k);
            const char *v[1] = { (const char *)pb }; int l[1] = { (int)pl }, f[1] = { 1 };
            PGresult *q = PQexecParams(pg, "SELECT entity, id, times, tier FROM laplace_fills($1::blake3[])", 1, NULL, v, l, f, 1);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "parents: %s", PQerrorMessage(pg)); return 1; }
            fetched += (uint64_t)PQntuples(q);
            for (int j = 0; j < PQntuples(q); j++) {
                lp_id pid; memcpy(pid.b, PQgetvalue(q, j, 0), 16);
                int known = map_get(&map, &pid, 0, &d, &nn, &cn) >= 0, pidx = map_get(&map, &pid, 1, &d, &nn, &cn);
                if (!known) { if (nx == cx) { cx *= 2; next = xrealloc(next, sizeof(lp_id) * cx); } next[nx++] = pid;
 }
                lp_id cid; memcpy(cid.b, PQgetvalue(q, j, 1), 16);                /* the child it holds, and how often */
                uint64_t tbe; memcpy(&tbe, PQgetvalue(q, j, 2), 8); uint32_t times = (uint32_t)__builtin_bswap64(tbe);
                int ci = map_get(&map, &cid, 0, &d, &nn, &cn); if (ci < 0 || ci == pidx) continue;
                DNode *c = &d[ci]; int merged = 0;
                for (int u = 0; u < c->nup; u++) if (c->up[u].parent == pidx) { c->up[u].times += times; merged = 1; break; }
                if (!merged) { if (c->nup == c->cup) { c->cup = c->cup ? c->cup * 2 : 4; c->up = xrealloc(c->up, sizeof(Up) * c->cup); } c->up[c->nup++] = (Up){ pidx, times }; }
            }
            PQclear(q); free(pb);
        }
        fprintf(stderr, "  level %d: %d nodes looked up, %d new parents, %.1f ms\n", levels, nf, nx, (now() - t) * 1000);
        free(front); front = next; nf = nx;
    }
    double t_up = now() - t;

    /* 4 and 5 */
    t = now();
    for (int i = 0; i < ncont; i++) {
        double w = occ_of(d, conts[i].node);
        for (size_t j = 0; j < conts[i].nf; j++) {
            int ai = map_get(&amap, &conts[i].f[j], 0, &adummy, &an, &ac);
            if (ai < 0) { ai = map_get(&amap, &conts[i].f[j], 1, &adummy, &an, &ac);
                          if ((uint64_t)ai >= cacc) { cacc = cacc ? cacc * 2 : 1024; acc = xrealloc(acc, sizeof(Acc) * cacc); }
                          acc[ai] = (Acc){ conts[i].f[j], 0, 0 }; nacc++; }
            acc[ai].occ += w; acc[ai].paths++;
        }
    }
    for (uint64_t i = 1; i < nacc; i++) { Acc x = acc[i]; uint64_t j = i; while (j > 0 && acc[j - 1].occ < x.occ) { acc[j] = acc[j - 1]; j--; } acc[j] = x; }
    double t_w = now() - t;

    /* the top continuations as text: their paths fetched together, a level at a time */
    Reader *rd = reader_new(pg);
    for (uint64_t i = 0; i < nacc && (int)i < limit; i++) reader_want(rd, &acc[i].id);
    printf("%-24s %14s %10s\n", "follows", "occurrences", "paths");
    for (uint64_t i = 0; i < nacc && (int)i < limit; i++) {
        char *tx = reader_text(rd, &acc[i].id, 60);
        printf("%-24s %14.0f %10llu\n", tx, acc[i].occ, (unsigned long long)acc[i].paths); free(tx);
    }
    reader_free(rd);
    printf("\n%d paths hold the phrase; %d levels up to the trunks, %d nodes, %llu parent paths fetched\n", direct, levels, nn, (unsigned long long)fetched);
    printf("containers %.1f ms   leaf to trunk %.1f ms   weighting %.2f ms   total %.1f ms\n", t_cont * 1000, t_up * 1000, t_w * 1000, (now() - T) * 1000);
    PQfinish(pg); return 0;
}
