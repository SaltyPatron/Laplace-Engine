/* Reading the database. SQL only fetches: paths come back for a whole set of entities at once, one level of the DAG per
 * round trip, and are decoded and expanded here. A containment is not one backend walking every partition: each leaf
 * is its own statement, and the leaves run on every core (the same shape as a load's writes). */
#include "engine.h"
#include <arpa/inet.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t ids_param(uint8_t *out, const lp_id *ids, uint32_t n){
    uint32_t hdr[5] = { htonl(1), htonl(0), htonl(id_oid), htonl(n), htonl(1) }; memcpy(out, hdr, 20); uint8_t *q = out + 20;
    for (uint32_t i = 0; i < n; i++) { uint32_t l = htonl(16); memcpy(q, &l, 4); memcpy(q + 4, ids[i].b, 16); q += 20; }
    return (size_t)(q - out);
}
void id_text(const lp_id *id, char out[33]){ lp_id_hex(id, out); }

typedef struct { lp_id id; lp_id *kid; uint32_t *run; uint32_t nv; uint8_t state; } Ent;      /* state: 0 wanted, 1 fetched, 2 not recorded */
struct Reader { PGconn *pg; Ent *e; size_t n, cap; lp_idmap *m; uint64_t trips; };

Reader *reader_new(PGconn *pg){ Reader *r = calloc(1, sizeof *r); r->pg = pg; return r; }
void reader_free(Reader *r){ if (!r) return; for (size_t i = 0; i < r->n; i++) { free(r->e[i].kid); free(r->e[i].run); } free(r->e); lp_idmap_free(r->m); free(r); }
uint64_t reader_trips(const Reader *r){ return r->trips; }

static Ent *ent(Reader *r, const lp_id *id, int add){
    if (!add) { int64_t i = lp_idmap_find(r->m, id); return i < 0 ? NULL : &r->e[i]; }
    if (!r->m) r->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(r->m, id, &fresh); if (!fresh) return &r->e[i];
    lp_reserve((void **)&r->e, &r->cap, r->n + 1, sizeof(Ent));
    Ent *x = &r->e[r->n++]; memset(x, 0, sizeof *x); x->id = *id; return x;
}
void reader_want(Reader *r, const lp_id *id){ if (lp_tier0_codepoint(T0, id) < 0) ent(r, id, 1); }

/* Every wanted entity's path, in one round trip. Returns how many were wanted. */
static size_t fetch(Reader *r){
    size_t nw = 0; for (size_t i = 0; i < r->n; i++) nw += r->e[i].state == 0;
    if (!nw) return 0;
    lp_vec(lp_id) ids = { 0 };
    for (size_t i = 0; i < r->n; i++) if (r->e[i].state == 0) { lp_push(&ids, r->e[i].id); r->e[i].state = 2; }
    Args a = { 0 }; arg_ids(&a, ids.v, ids.n);
    PGresult *q = ask(r->pg, "SELECT entity, path FROM laplace_paths($1::blake3[])", &a);
    r->trips++;
    for (int j = 0; j < PQntuples(q); j++) {
        Ent *x = ent(r, col_id(q, j, 0), 0); if (!x || x->state == 1) continue;
        lp_path p = col_path(q, j, 1);
        x->kid = malloc(sizeof(lp_id) * (p.n ? p.n : 1)); x->run = malloc(4 * (p.n ? p.n : 1)); x->nv = (uint32_t)p.n; x->state = 1;
        for (size_t i = 0; i < p.n; i++) { x->kid[i] = lp_path_id(p, i); x->run[i] = lp_path_run(p, i); }
    }
    PQclear(q); args_free(&a); lp_vec_free(&ids);
    return nw;
}

typedef struct { char *b; size_t n, cap, limit, wants; } Out;
static void expand(Reader *r, const lp_id *id, Out *o, int depth){
    if (o->n >= o->limit) return;
    int64_t cp = lp_tier0_codepoint(T0, id);
    if (cp >= 0) { lp_reserve((void **)&o->b, &o->cap, o->n + 4, 1); o->n += lp_utf8_put((uint32_t)cp, (uint8_t *)o->b + o->n); return; }
    Ent *x = ent(r, id, 0);
    if (!x) { if (o->wants < o->limit) { ent(r, id, 1); o->wants++; } return; }
    if (x->state == 0) { o->wants++; return; }
    if (x->state != 1 || depth > 64) return;
    size_t at = (size_t)(x - r->e);
    for (uint32_t v = 0; v < r->e[at].nv && o->n < o->limit; v++) {                 /* by index: the table may move */
        lp_id kid = r->e[at].kid[v]; uint32_t run = r->e[at].run[v];
        for (uint32_t k = 0; k < run && o->n < o->limit; k++) expand(r, &kid, o, depth + 1);
    }
}

char *reader_text(Reader *r, const lp_id *id, size_t limit){
    Out o = { 0 }; o.limit = limit ? limit : (size_t)-1; o.cap = 64; o.b = malloc(o.cap);
    reader_want(r, id);
    for (int pass = 0; pass < 64; pass++) {
        fetch(r);
        o.n = 0; o.wants = 0; expand(r, id, &o, 0);
        if (!o.wants) break;
    }
    Ent *x = ent(r, id, 0);
    if (!o.n && x && x->state == 2) { free(o.b); char t[33]; lp_id_hex(id, t); char *s = malloc(48); snprintf(s, 48, "{%s}", t); return s; }   /* not recorded */
    if (o.n >= o.limit) {                                   /* cut at a character, and say so */
        size_t n = o.n; if (n > o.limit) { n = o.limit; while (n > 0 && ((uint8_t)o.b[n] & 0xC0) == 0x80) n--; }
        o.b = xrealloc(o.b, n + 8); memcpy(o.b + n, "\xE2\x80\xA6", 4); return o.b;
    }
    o.b = xrealloc(o.b, o.n + 1); o.b[o.n] = 0;
    return o.b;
}
/* An entity's constituents, its runs written out, at most cap of them: fetched if the reader has not read it. 0 for an
 * atom, or for what is not recorded. */
size_t reader_parts(Reader *r, const lp_id *id, lp_id *out, size_t cap){
    if (lp_tier0_codepoint(T0, id) >= 0) return 0;
    Ent *x = ent(r, id, 1); if (x->state == 0) fetch(r);
    x = ent(r, id, 0); if (!x || x->state != 1) return 0;
    size_t n = 0; for (uint32_t v = 0; v < x->nv; v++) for (uint32_t k = 0; k < x->run[v]; k++, n++) if (n < cap) out[n] = x->kid[v];
    return n < cap ? n : cap;
}
/* A trajectory's constituents in order, runs written out, from a path as the database sends it. */
Run run_of(const uint8_t *ewkb, size_t len){
    lp_path p = lp_path_of(ewkb, len); size_t n = lp_path_len(p); Run r = { malloc(sizeof(lp_id) * (n ? n : 1)), (int)n };
    lp_path_expand(p, r.id, n); return r;
}

/* ---- every leaf at once -------------------------------------------------
 * physicality and entity are partitioned by tier, and the large tiers again by the first hex digit of the entity.
 * A path lookup cannot name one of those leaves from the ID it holds: the ID sits inside the path, so every leaf
 * above the ID's tier can hold it. Naming the parent makes one backend append those leaves and walk them in order.
 * Here each leaf is one statement and the statements run together. Mask and standing are not in that statement:
 * the mask bit is a filter on the rows the path index returned, and the standing is a second set, by the claim's
 * own partition. */
static PGconn **pool;
static int npool;
static int times_on(void){ static int on = -1; if (on < 0) { const char *e = getenv("LAPLACE_TIMES"); on = e && *e; } return on; }
static void pool_open(void){
    if (pool) return; double t0 = now();
    const char *ci = db_noted(); if (!ci || !*ci) ci = laplace_db();
    npool = omp_get_num_procs(); if (npool < 1) npool = 1;
    pool = calloc((size_t)npool, sizeof *pool);
    /* every connection at once: a backend starts in about 100 ms on Windows (a process each), so 32 opened in turn
     * cost three seconds before the first read; opened together they cost one */
    #pragma omp parallel for num_threads(npool) schedule(static, 1)
    for (int i = 0; i < npool; i++) { PGconn *c = PQconnectdb(ci);
        if (PQstatus(c) != CONNECTION_OK) { fprintf(stderr, "%s", PQerrorMessage(c)); exit(1); }
        PQclear(PQexec(c, "SET client_min_messages = warning")); PQclear(PQexec(c, "SET enable_parallel_append = on")); pool[i] = c; }
    if (times_on()) fprintf(stderr, "times: %d connections opened in %.1f ms\n", npool, (now() - t0) * 1000);
    const char *snap = db_snapshot();                                    /* a read: every leaf read at the instant the command's first statement saw */
    if (snap) for (int i = 0; i < npool; i++) {
        char q[160]; snprintf(q, sizeof q, "BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY; SET TRANSACTION SNAPSHOT '%s'", snap);
        PGresult *r = PQexec(pool[i], q);
        if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "snapshot: %s", PQerrorMessage(pool[i])); exit(1); } PQclear(r); }
}
typedef struct { char name[64]; int tier; } Leaf;
static Leaf *phy, *entleaves; static int nphy, nent;
static int tier_in(const char *name){
    const char *p = strstr(name, "_t"); if (!p) return -1; p += 2;
    if (*p == 'x') return 16;
    return atoi(p);
}
static void leaves_of(const char *kind, Leaf **out, int *n){
    if (*out) return;
    pool_open();
    char q[320]; snprintf(q, sizeof q, "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'public' AND c.relkind = 'r' AND c.relname ~ '^%s_[0-9a-f]{2}$' ORDER BY 1", kind);   /* the ID's ranges: a leaf holds every tier */
    PGresult *r = PQexec(pool[0], q);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "leaves: %s", PQerrorMessage(pool[0])); exit(1); }
    int m = PQntuples(r); *out = calloc((size_t)(m ? m : 1), sizeof(Leaf)); *n = m;
    for (int i = 0; i < m; i++) { snprintf((*out)[i].name, 64, "%s", PQgetvalue(r, i, 0)); (*out)[i].tier = tier_in((*out)[i].name); }
    PQclear(r);
}
static int mask_claim(const char *m, int len){ if (len < 5) return 0; return ((const uint8_t *)m)[4] & 0x80; }
static int16_t rd_i16(const char *p){ uint16_t u; memcpy(&u, p, 2); return (int16_t)ntohs(u); }

int tier_max(const lp_id *ids, int n){
    if (!n) return -1;
    leaves_of("entity", &entleaves, &nent);
    uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = ids_param(ab, ids, (uint32_t)n);
    int *got = calloc((size_t)nent, sizeof(int)); int16_t *tv = calloc((size_t)nent, sizeof(int16_t));
    #pragma omp parallel for num_threads(npool) schedule(dynamic)
    for (int i = 0; i < nent; i++) {
        char sql[160]; snprintf(sql, sizeof sql, "SELECT tier FROM %s WHERE id = ANY($1::blake3[])", entleaves[i].name);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGresult *r = PQexecParams(pool[omp_get_thread_num()], sql, 1, NULL, v, l, f, 1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "tier: %s", PQerrorMessage(pool[omp_get_thread_num()])); exit(1); }
        for (int row = 0; row < PQntuples(r); row++) { int16_t t = rd_i16(PQgetvalue(r, row, 0)); if (!got[i] || t > tv[i]) tv[i] = t; got[i] = 1; }
        PQclear(r);
    }
    int16_t mx = -1; for (int i = 0; i < nent; i++) if (got[i] && tv[i] > mx) mx = tv[i];
    free(got); free(tv); free(ab);
    return mx;
}

static int hold_order(const void *a, const void *b){
    const Hold *x = a, *y = b; int c = memcmp(x->entity.b, y->entity.b, 16);
    return c ? c : x->src != y->src ? (x->src < y->src ? -1 : 1) : 0;
}
typedef struct { Hold *h; int n, cap; } Bag;
static void bag_put(Bag *b, Hold x){
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 32; b->h = xrealloc(b->h, (size_t)b->cap * sizeof(Hold)); }
    b->h[b->n++] = x;
}
static Hold *holds_core_(const lp_id *keys, int nkeys, int floor, int each, int standing, const lp_id *with, int kind, int *nout);
static Hold *holds_core(const lp_id *keys, int nkeys, int floor, int each, int standing, const lp_id *with, int *nout){
    double t0 = now(); Hold *h = holds_core_(keys, nkeys, floor, each, standing, with, -1, nout);
    if (times_on()) fprintf(stderr, "times: holds of %d key%s over %d leaves: %d rows in %.1f ms\n", nkeys, nkeys == 1 ? "" : "s", nphy, *nout, (now() - t0) * 1000);
    return h;
}
static Hold *holds_core_(const lp_id *keys, int nkeys, int floor, int each, int standing, const lp_id *with, int kind, int *nout){
    *nout = 0; if (!nkeys) return NULL;
    leaves_of("physicality", &phy, &nphy);
    uint8_t *ab = malloc(20 + 20 * (size_t)nkeys); size_t al = ids_param(ab, keys, (uint32_t)nkeys);
    int *job = malloc(sizeof(int) * (size_t)nphy); int nj = 0;
    for (int i = 0; i < nphy; i++) job[nj++] = i;                        /* every range holds every tier: the tier is asked of the rows */
    Bag *bag = calloc((size_t)(nj ? nj : 1), sizeof(Bag));
    /* each = 1 without a second key is asked as each = 2, one overlap probe a leaf for the whole set, never a probe a
     * key: every path is given to the keys it holds, read from its own vertices through a map of the keys */
    int give = each == 1 && !with; if (give) each = 2;
    lp_idmap *km = NULL; int *khead = NULL, *klink = NULL;
    if (give) { km = lp_idmap_new(); khead = malloc(sizeof(int) * (size_t)nkeys); klink = malloc(sizeof(int) * (size_t)nkeys);
        for (int i = 0; i < nkeys; i++) khead[i] = -1;
        for (int i = nkeys - 1; i >= 0; i--) { bool fresh; size_t u = lp_idmap_put(km, &keys[i], &fresh); klink[i] = khead[u]; khead[u] = i; } }
    uint8_t wb[40]; size_t wl = with ? ids_param(wb, with, 1) : 0;          /* each key with this one too: a claim's key and its relation */
    #pragma omp parallel for num_threads(npool) schedule(dynamic)
    for (int j = 0; j < nj; j++) {
        char sql[384];
        if (each == 2) snprintf(sql, sizeof sql, "SELECT entity, path, tier, mask FROM %s WHERE path && $1::blake3[] AND tier > %d%s%s",     /* any key: one probe a leaf for the set; with is checked on the rows, never probed (a relation is a hub: its posting list is every claim of it) */
                                phy[job[j]].name, floor, with ? " AND (path @> $2::blake3[]) IS TRUE" : "", kind == 1 ? " AND mask ? 0::smallint" : kind == 0 ? " AND NOT (mask ? 0::smallint)" : "");
        else if (each) snprintf(sql, sizeof sql, "SELECT u.i, p.entity, p.path, p.tier, p.mask FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i) JOIN %s p ON p.path @> %s WHERE p.tier > %d",
                           phy[job[j]].name, with ? "(ARRAY[u.id] || $2::blake3[])" : "ARRAY[u.id]", floor);
        else snprintf(sql, sizeof sql, "SELECT entity, path, tier, mask FROM %s WHERE path @> $1::blake3[] AND tier > %d", phy[job[j]].name, floor);
        const char *v[2] = { (const char *)ab, (const char *)wb }; int l[2] = { (int)al, (int)wl }, f[2] = { 1, 1 };
        PGconn *c = pool[omp_get_thread_num()];
        PGresult *r = PQexecParams(c, sql, each && with ? 2 : 1, NULL, v, l, f, 1); int one = each == 1;
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "hold %s: %s", phy[job[j]].name, PQerrorMessage(c)); exit(1); }
        for (int row = 0; row < PQntuples(r); row++) {
            int col = one ? 1 : 0;
            Hold x; memset(&x, 0, sizeof x);
            if (one) { uint64_t o; memcpy(&o, PQgetvalue(r, row, 0), 8); x.src = (int)__builtin_bswap64(o) - 1; } else if (each) x.src = -1;     /* any key: the caller reads which from the path */
            memcpy(x.entity.b, PQgetvalue(r, row, col), 16);
            x.path_len = PQgetlength(r, row, col + 1); x.path = malloc((size_t)(x.path_len ? x.path_len : 1)); memcpy(x.path, PQgetvalue(r, row, col + 1), (size_t)x.path_len);
            x.tier = rd_i16(PQgetvalue(r, row, col + 2));
            if (x.tier <= floor) { free(x.path); continue; }
            x.claim = (uint8_t)mask_claim(PQgetvalue(r, row, col + 3), PQgetlength(r, row, col + 3));
            if (!give) { bag_put(&bag[j], x); continue; }
            size_t nv = lp_path_ids(x.path, (size_t)x.path_len, NULL, 0); lp_id *pv = malloc(sizeof(lp_id) * (nv ? nv : 1)); lp_path_ids(x.path, (size_t)x.path_len, pv, nv);
            int64_t seen[64]; int ns = 0, first = 1;
            for (size_t v = 0; v < nv; v++) { int64_t u = lp_idmap_find(km, &pv[v]); if (u < 0) continue;
                int dup = 0; for (int s = 0; s < ns; s++) dup |= seen[s] == u; if (dup) continue; if (ns < 64) seen[ns++] = u;      /* a path that holds a key twice holds it once */
                for (int i = khead[u]; i >= 0; i = klink[i]) { Hold y = x; y.src = i;
                    if (!first) { y.path = malloc((size_t)(x.path_len ? x.path_len : 1)); memcpy(y.path, x.path, (size_t)x.path_len); }
                    first = 0; bag_put(&bag[j], y); } }
            if (first) free(x.path); free(pv);
        }
        PQclear(r);
    }
    free(ab); free(job); if (km) { lp_idmap_free(km); free(khead); free(klink); }
    int n = 0; for (int j = 0; j < nj; j++) n += bag[j].n;
    Hold *h = calloc((size_t)(n ? n : 1), sizeof(Hold)); int w = 0;
    for (int j = 0; j < nj; j++) { memcpy(h + w, bag[j].h, (size_t)bag[j].n * sizeof(Hold)); w += bag[j].n; free(bag[j].h); }
    free(bag);
    /* In content order: rows come out of each leaf as its heap lies, which differs between two installs of the same
     * content and after a vacuum. Every caller that caps, sums or takes the first of a set would otherwise depend on it. */
    if (n > 1) qsort(h, (size_t)n, sizeof(Hold), hold_order);
    if (standing && n) {
        int *ix[16], nx[16]; memset(nx, 0, sizeof nx);
        for (int i = 0; i < n; i++) nx[h[i].entity.b[0] >> 4]++;
        int acc[16]; int run = 0; for (int k = 0; k < 16; k++) { acc[k] = run; run += nx[k]; nx[k] = 0; }
        int *at = malloc(sizeof(int) * (size_t)n);
        for (int i = 0; i < n; i++) { int k = h[i].entity.b[0] >> 4; at[acc[k] + nx[k]++] = i; }
        for (int k = 0; k < 16; k++) ix[k] = at + acc[k];
        #pragma omp parallel for num_threads(npool) schedule(dynamic)
        for (int k = 0; k < 16; k++) {
            if (!nx[k]) continue;
            /* each claim once, and the holds that are it chained under it: a standing finds its holds by its ID, not by a scan */
            lp_idmap *u = lp_idmap_new(); lp_id *ids = malloc(sizeof(lp_id) * (size_t)nx[k]); int *head = malloc(sizeof(int) * (size_t)nx[k]), *link = malloc(sizeof(int) * (size_t)nx[k]); int nu = 0;
            for (int i = 0; i < nx[k]; i++) { bool fresh; size_t at_ = lp_idmap_put(u, &h[ix[k][i]].entity, &fresh); if (fresh) { ids[nu] = h[ix[k][i]].entity; head[nu++] = -1; } link[i] = head[at_]; head[at_] = i; }
            uint8_t *bb = malloc(20 + 20 * (size_t)nu); size_t bl = ids_param(bb, ids, (uint32_t)nu);
            char sql[160]; snprintf(sql, sizeof sql, "SELECT claim, rating, deviation, volatility, matches FROM consensus_%x WHERE claim = ANY($1::blake3[])", k);
            const char *v[1] = { (const char *)bb }; int l[1] = { (int)bl }, f[1] = { 1 };
            PGconn *c = pool[omp_get_thread_num()];
            PGresult *r = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
            if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "standing: %s", PQerrorMessage(c)); exit(1); }
            for (int row = 0; row < PQntuples(r); row++) {
                lp_id id; memcpy(id.b, PQgetvalue(r, row, 0), 16);
                int64_t at_ = lp_idmap_find(u, &id); if (at_ < 0) continue;
                for (int i = head[at_]; i >= 0; i = link[i]) {
                    Hold *x = &h[ix[k][i]];
                    x->r.rating = lp_be_f64(PQgetvalue(r, row, 1)); x->r.deviation = lp_be_f64(PQgetvalue(r, row, 2)); x->r.volatility = lp_be_f64(PQgetvalue(r, row, 3));
                    uint32_t mb; memcpy(&mb, PQgetvalue(r, row, 4), 4); x->matches = (int)ntohl(mb); x->stood = 1;
                }
            }
            PQclear(r); free(bb); free(ids); free(head); free(link); lp_idmap_free(u);
        }
        free(at);
    }
    *nout = n; return h;
}
Hold *holds_above(const lp_id *keys, int nkeys, int floor, int each, int standing, int *nout){ return holds_core(keys, nkeys, floor, each, standing, NULL, nout); }
/* What holds each key, a hub's holders never read: first, every leaf at once, how many claims and how many
 * observations hold each key, counted at most cap a leaf (a count, never which rows, so the same on every install);
 * then the rows of the keys no more than cap hold, claims and observations apart, one overlap probe a leaf for each
 * set. hub[2k]: more than cap claims hold key k; hub[2k + 1]: more than cap observations. src is the key's place. */
Hold *holds_capped(const lp_id *keys, int nkeys, int cap, int standing, int kinds, int *hub, int *nout){
    *nout = 0; memset(hub, 0, sizeof(int) * 2 * (size_t)nkeys); if (!nkeys) return NULL; double t0 = now();
    leaves_of("physicality", &phy, &nphy);
    uint8_t *ab = malloc(20 + 20 * (size_t)nkeys); size_t al = ids_param(ab, keys, (uint32_t)nkeys);
    /* An ID is a hash, so what holds a key spreads evenly over the leaves: a first count stops at q + 1 a leaf. A key whose
     * capped counts already pass cap is a hub; one that saturated no leaf is counted exactly; only a leaf a key saturated
     * without the sum deciding is counted again, to cap + 1. Exact, and a hub costs at most q + 1 rows a leaf. */
    long *cnt = calloc((size_t)nphy * (size_t)nkeys * 2, sizeof(long)); int q = cap / nphy * 4 + 16; if (q > cap) q = cap;
    for (int pass = 0; pass < 2; pass++) {
    int *redo = NULL; if (pass) { redo = calloc((size_t)nkeys * 2, sizeof(int)); int any = 0;
        for (int k = 0; k < nkeys; k++) for (int t = 0; t < 2; t++) { long sum = 0; int sat = 0;
            for (int j = 0; j < nphy; j++) { long c = cnt[((size_t)j * (size_t)nkeys + (size_t)k) * 2 + (size_t)t]; sum += c; sat |= c > q; }
            if (sat && sum <= cap) { redo[2 * k + t] = 1; any = 1; } }
        if (!any) { free(redo); break; } }
    int lim = pass ? cap + 1 : q + 1;
    #pragma omp parallel for num_threads(npool) schedule(dynamic)
    for (int j = 0; j < nphy; j++) {
        if (pass) { int any = 0; for (int k = 0; k < nkeys && !any; k++) for (int t = 0; t < 2; t++) any |= redo[2 * k + t] && cnt[((size_t)j * (size_t)nkeys + (size_t)k) * 2 + (size_t)t] > q; if (!any) continue; }
        char sql[768]; snprintf(sql, sizeof sql, "SELECT u.i, (SELECT count(*) FROM (SELECT 1 FROM %s p WHERE p.path @> ARRAY[u.id] AND p.mask ? 0::smallint LIMIT %d) x), "
                                "(SELECT count(*) FROM (SELECT 1 FROM %s p WHERE p.path @> ARRAY[u.id] AND NOT (p.mask ? 0::smallint) LIMIT %d) y) FROM unnest($1::blake3[]) WITH ORDINALITY u(id, i)",
                                phy[j].name, lim, phy[j].name, lim);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGconn *c = pool[omp_get_thread_num()]; PGresult *r = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "count %s: %s", phy[j].name, PQerrorMessage(c)); exit(1); }
        for (int row = 0; row < PQntuples(r); row++) { uint64_t o, a, b; memcpy(&o, PQgetvalue(r, row, 0), 8); memcpy(&a, PQgetvalue(r, row, 1), 8); memcpy(&b, PQgetvalue(r, row, 2), 8);
            int k = (int)__builtin_bswap64(o) - 1; if (k < 0 || k >= nkeys) continue; size_t at = ((size_t)j * (size_t)nkeys + (size_t)k) * 2;
            if (!pass || redo[2 * k]) cnt[at] = (long)__builtin_bswap64(a); if (!pass || redo[2 * k + 1]) cnt[at + 1] = (long)__builtin_bswap64(b); }
        PQclear(r);
    }
    free(redo); }
    for (int k = 0; k < nkeys; k++) { long c = 0, o = 0; for (int j = 0; j < nphy; j++) { c += cnt[((size_t)j * (size_t)nkeys + (size_t)k) * 2]; o += cnt[((size_t)j * (size_t)nkeys + (size_t)k) * 2 + 1]; }
        hub[2 * k] = c > cap; hub[2 * k + 1] = o > cap; }
    free(cnt); free(ab);
    double t1 = now();
    lp_id *kk = malloc(sizeof(lp_id) * (size_t)nkeys); int *back = malloc(sizeof(int) * (size_t)nkeys); Hold *all = NULL; int na = 0;
    for (int kind = 1; kind >= 0; kind--) { int m = 0; if (!(kinds & (kind ? 1 : 2))) continue;
        for (int k = 0; k < nkeys; k++) if (!hub[2 * k + (kind ? 0 : 1)]) { kk[m] = keys[k]; back[m++] = k; }
        if (!m) continue; int nh = 0; Hold *h = holds_core_(kk, m, -1, 1, kind ? standing : 0, NULL, kind, &nh);
        for (int i = 0; i < nh; i++) if (h[i].src >= 0 && h[i].src < m) h[i].src = back[h[i].src];
        all = xrealloc(all, sizeof(Hold) * (size_t)(na + nh + 1)); memcpy(all + na, h, sizeof(Hold) * (size_t)nh); na += nh; free(h); }
    free(kk); free(back);
    if (na > 1) qsort(all, (size_t)na, sizeof(Hold), hold_order);
    if (times_on()) fprintf(stderr, "times: capped holds of %d key%s: counted in %.1f ms, %d rows read in %.1f ms\n", nkeys, nkeys == 1 ? "" : "s", (t1 - t0) * 1000, na, (now() - t1) * 1000);
    *nout = na; return all;
}
/* The paths of a set of entities, each from the one physicality partition its ID names, every partition at once:
 * claims flagged by their mask, the set's order kept (src: the place in ids). */
Hold *paths_of(const lp_id *ids, int n, int *nout){
    *nout = 0; if (!n) return NULL; leaves_of("physicality", &phy, &nphy);
    int *head = malloc(sizeof(int) * 256), *link = malloc(sizeof(int) * (size_t)n); for (int b = 0; b < 256; b++) head[b] = -1;
    for (int i = n - 1; i >= 0; i--) { int b = ids[i].b[0]; link[i] = head[b]; head[b] = i; }
    Bag *bag = calloc(256, sizeof(Bag));
    #pragma omp parallel for num_threads(npool) schedule(dynamic)
    for (int b = 0; b < 256; b++) {
        if (head[b] < 0) continue; char want[64]; snprintf(want, sizeof want, "physicality_%02x", b); int leaf = -1;
        for (int e = 0; e < nphy; e++) if (!strcmp(phy[e].name, want)) leaf = e;
        if (leaf < 0) continue;
        int m = 0; for (int i = head[b]; i >= 0; i = link[i]) m++; lp_id *q = malloc(sizeof(lp_id) * (size_t)m); m = 0; for (int i = head[b]; i >= 0; i = link[i]) q[m++] = ids[i];
        uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = ids_param(ab, q, (uint32_t)m);
        char sql[256]; snprintf(sql, sizeof sql, "SELECT entity, path, tier, mask FROM %s WHERE entity = ANY($1::blake3[])", phy[leaf].name);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGconn *c = pool[omp_get_thread_num()]; PGresult *r = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "paths: %s", PQerrorMessage(c)); exit(1); }
        for (int row = 0; row < PQntuples(r); row++) { Hold x; memset(&x, 0, sizeof x); memcpy(x.entity.b, PQgetvalue(r, row, 0), 16);
            x.path_len = PQgetlength(r, row, 1); x.path = malloc((size_t)(x.path_len ? x.path_len : 1)); memcpy(x.path, PQgetvalue(r, row, 1), (size_t)x.path_len);
            x.tier = rd_i16(PQgetvalue(r, row, 2)); x.claim = (uint8_t)mask_claim(PQgetvalue(r, row, 3), PQgetlength(r, row, 3)); x.src = -1;
            for (int i = head[b]; i >= 0; i = link[i]) if (!memcmp(ids[i].b, x.entity.b, 16)) { x.src = i; break; }
            bag_put(&bag[b], x); }
        PQclear(r); free(q); free(ab);
    }
    int total = 0; for (int b = 0; b < 256; b++) total += bag[b].n;
    Hold *h = malloc(sizeof(Hold) * (size_t)(total ? total : 1)); int w = 0; for (int b = 0; b < 256; b++) { memcpy(h + w, bag[b].h, sizeof(Hold) * (size_t)bag[b].n); w += bag[b].n; free(bag[b].h); }
    free(bag); free(head); free(link); if (w > 1) qsort(h, (size_t)w, sizeof(Hold), hold_order); *nout = w; return h;
}
/* The coordinate of each entity: an atom's from tier 0, the rest from its entity partition, every partition at once. */
void coords_of(const lp_id *ids, int n, double *out, uint8_t *has){
    memset(has, 0, (size_t)n); if (!n) return;
    leaves_of("entity", &entleaves, &nent);
    int *bucket = malloc(sizeof(int) * (size_t)n), *head = malloc(sizeof(int) * 256), *link = malloc(sizeof(int) * (size_t)n); for (int b = 0; b < 256; b++) head[b] = -1;
    for (int i = n - 1; i >= 0; i--) { int64_t cp = lp_tier0_codepoint(T0, &ids[i]);
        if (cp >= 0) { lp_ref a = lp_ref_atom(T0, (uint32_t)cp); lp_coord_xyzm(&a.c, out + 4 * (size_t)i); has[i] = 1; continue; }
        int b = ids[i].b[0]; bucket[i] = b; link[i] = head[b]; head[b] = i; }
    #pragma omp parallel for num_threads(npool) schedule(dynamic)
    for (int b = 0; b < 256; b++) {
        if (head[b] < 0) continue; int leaf = -1; char want[64]; snprintf(want, sizeof want, "entity_%02x", b);
        for (int e = 0; e < nent; e++) if (!strcmp(entleaves[e].name, want)) leaf = e;
        if (leaf < 0) continue;
        int m = 0; for (int i = head[b]; i >= 0; i = link[i]) m++;
        lp_id *q = malloc(sizeof(lp_id) * (size_t)m); m = 0; for (int i = head[b]; i >= 0; i = link[i]) q[m++] = ids[i];
        uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = ids_param(ab, q, (uint32_t)m);
        char sql[256]; snprintf(sql, sizeof sql, "SELECT id, ST_X(coord), ST_Y(coord), ST_Z(coord), ST_M(coord) FROM %s WHERE id = ANY($1::blake3[])", entleaves[leaf].name);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGconn *c = pool[omp_get_thread_num()]; PGresult *r = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "coords: %s", PQerrorMessage(c)); exit(1); }
        for (int row = 0; row < PQntuples(r); row++) { const char *id = PQgetvalue(r, row, 0); double x[4]; for (int d = 0; d < 4; d++) x[d] = lp_be_f64(PQgetvalue(r, row, 1 + d));
            for (int i = head[b]; i >= 0; i = link[i]) if (!memcmp(ids[i].b, id, 16)) { memcpy(out + 4 * (size_t)i, x, 32); has[i] = 1; } }
        PQclear(r); free(q); free(ab);
    }
    free(bucket); free(head); free(link);
}
Hold *holds_pair(const lp_id *keys, int nkeys, const lp_id *with, int standing, int *nout){ return holds_core(keys, nkeys, -1, 2, standing, with, nout); }
Hold *holds_any(const lp_id *keys, int nkeys, int *nout){ return holds_core(keys, nkeys, -1, 2, 0, NULL, nout); }
void holds_free(Hold *h, int n){ if (!h) return; for (int i = 0; i < n; i++) free(h[i].path); free(h); }
