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
void id_text(const lp_id *id, char out[33]){
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = hex[id->b[i] >> 4]; out[2 * i + 1] = hex[id->b[i] & 15]; } out[32] = 0;
}
int id_parse(const char *s, lp_id *out){
    for (int i = 0; i < 16; i++) { int v = 0; for (int j = 0; j < 2; j++) { char c = s[2 * i + j]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (d < 0) return 0; v = v * 16 + d; } out->b[i] = (uint8_t)v; }
    return s[32] == 0 || s[32] == '\t' || s[32] == '\n';
}

typedef struct { lp_id id; lp_id *kid; uint32_t *run; uint32_t nv; uint8_t state; } Ent;      /* state: 0 wanted, 1 fetched, 2 not recorded */
struct Reader { PGconn *pg; Ent *e; size_t n, cap; lp_idmap *m; uint64_t trips; };

Reader *reader_new(PGconn *pg){ Reader *r = calloc(1, sizeof *r); r->pg = pg; return r; }
void reader_free(Reader *r){ if (!r) return; for (size_t i = 0; i < r->n; i++) { free(r->e[i].kid); free(r->e[i].run); } free(r->e); lp_idmap_free(r->m); free(r); }
uint64_t reader_trips(const Reader *r){ return r->trips; }

static Ent *ent(Reader *r, const lp_id *id, int add){
    if (!add) { int64_t i = lp_idmap_find(r->m, id); return i < 0 ? NULL : &r->e[i]; }
    if (!r->m) r->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(r->m, id, &fresh); if (!fresh) return &r->e[i];
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 1024; r->e = xrealloc(r->e, r->cap * sizeof(Ent)); }
    Ent *x = &r->e[r->n++]; memset(x, 0, sizeof *x); x->id = *id; return x;
}
void reader_want(Reader *r, const lp_id *id){ if (lp_tier0_codepoint(T0, id) < 0) ent(r, id, 1); }

/* Every wanted entity's path, in one round trip. Returns how many were wanted. */
static size_t fetch(Reader *r){
    size_t nw = 0; for (size_t i = 0; i < r->n; i++) nw += r->e[i].state == 0;
    if (!nw) return 0;
    lp_id *ids = malloc(sizeof(lp_id) * nw); size_t k = 0;
    for (size_t i = 0; i < r->n; i++) if (r->e[i].state == 0) { ids[k++] = r->e[i].id; r->e[i].state = 2; }
    uint8_t *ab = malloc(20 + 20 * nw); size_t al = ids_param(ab, ids, (uint32_t)nw);
    const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = db_ask(r->pg, "SELECT entity, path FROM laplace_paths($1::blake3[])", 1, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "paths: %s", PQerrorMessage(r->pg)); exit(1); }
    r->trips++;
    for (int j = 0; j < PQntuples(q); j++) {
        lp_id id; memcpy(id.b, PQgetvalue(q, j, 0), 16); Ent *x = ent(r, &id, 0); if (!x || x->state == 1) continue;
        const uint8_t *pb = (const uint8_t *)PQgetvalue(q, j, 1); size_t pl = (size_t)PQgetlength(q, j, 1), nv = lp_path_vertices(pb, pl, NULL, 0);
        lp_vertex *vt = malloc(sizeof(lp_vertex) * (nv ? nv : 1)); lp_path_vertices(pb, pl, vt, nv);
        x->kid = malloc(sizeof(lp_id) * (nv ? nv : 1)); x->run = malloc(4 * (nv ? nv : 1)); x->nv = (uint32_t)nv; x->state = 1;
        for (size_t i = 0; i < nv; i++) { x->kid[i] = vt[i].id; x->run[i] = vt[i].run; }
        free(vt);
    }
    PQclear(q); free(ab); free(ids);
    return nw;
}

typedef struct { char *b; size_t n, cap, limit, wants; } Out;
static void expand(Reader *r, const lp_id *id, Out *o, int depth){
    if (o->n >= o->limit) return;
    int64_t cp = lp_tier0_codepoint(T0, id);
    if (cp >= 0) { if (o->n + 4 > o->cap) { o->cap = (o->n + 4) * 2; o->b = xrealloc(o->b, o->cap); } o->n += lp_utf8_put((uint32_t)cp, (uint8_t *)o->b + o->n); return; }
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
    if (!o.n && x && x->state == 2) { free(o.b); char t[33]; id_text(id, t); char *s = malloc(48); snprintf(s, 48, "{%s}", t); return s; }   /* not recorded */
    if (o.n >= o.limit) {                                   /* cut at a character, and say so */
        size_t n = o.n; if (n > o.limit) { n = o.limit; while (n > 0 && ((uint8_t)o.b[n] & 0xC0) == 0x80) n--; }
        o.b = xrealloc(o.b, n + 8); memcpy(o.b + n, "\xE2\x80\xA6", 4); return o.b;
    }
    o.b = xrealloc(o.b, o.n + 1); o.b[o.n] = 0;
    return o.b;
}
/* A trajectory's constituents in order, runs written out, from a path as the database sends it. */
Run run_of(const uint8_t *ewkb, size_t len){
    size_t n = lp_path_ids(ewkb, len, NULL, 0); Run r = { malloc(sizeof(lp_id) * (n ? n : 1)), (int)n };
    lp_path_ids(ewkb, len, r.id, n); return r;
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
static void pool_open(void){
    if (pool) return;
    const char *ci = db_noted(); if (!ci || !*ci) ci = laplace_db();
    npool = omp_get_num_procs(); if (npool < 1) npool = 1;
    pool = calloc((size_t)npool, sizeof *pool);
    for (int i = 0; i < npool; i++) pool[i] = db_connect(ci);
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

typedef struct { Hold *h; int n, cap; } Bag;
static void bag_put(Bag *b, Hold x){
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 32; b->h = xrealloc(b->h, (size_t)b->cap * sizeof(Hold)); }
    b->h[b->n++] = x;
}
static Hold *holds_core(const lp_id *keys, int nkeys, int floor, int each, int standing, const lp_id *with, int *nout){
    *nout = 0; if (!nkeys) return NULL;
    leaves_of("physicality", &phy, &nphy);
    uint8_t *ab = malloc(20 + 20 * (size_t)nkeys); size_t al = ids_param(ab, keys, (uint32_t)nkeys);
    int *job = malloc(sizeof(int) * (size_t)nphy); int nj = 0;
    for (int i = 0; i < nphy; i++) job[nj++] = i;                        /* every range holds every tier: the tier is asked of the rows */
    Bag *bag = calloc((size_t)(nj ? nj : 1), sizeof(Bag));
    uint8_t wb[40]; size_t wl = with ? ids_param(wb, with, 1) : 0;          /* each key with this one too: a claim's key and its relation */
    #pragma omp parallel for num_threads(npool) schedule(dynamic)
    for (int j = 0; j < nj; j++) {
        char sql[384];
        if (each == 2) snprintf(sql, sizeof sql, "SELECT entity, path, tier, mask FROM %s WHERE path && $1::blake3[] AND tier > %d%s",     /* any key: one probe a leaf for the set; with is checked on the rows, never probed (a relation is a hub: its posting list is every claim of it) */
                                phy[job[j]].name, floor, with ? " AND (path @> $2::blake3[]) IS TRUE" : "");
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
            bag_put(&bag[j], x);
        }
        PQclear(r);
    }
    free(ab); free(job);
    int n = 0; for (int j = 0; j < nj; j++) n += bag[j].n;
    Hold *h = calloc((size_t)(n ? n : 1), sizeof(Hold)); int w = 0;
    for (int j = 0; j < nj; j++) { memcpy(h + w, bag[j].h, (size_t)bag[j].n * sizeof(Hold)); w += bag[j].n; free(bag[j].h); }
    free(bag);
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
Hold *holds_pair(const lp_id *keys, int nkeys, const lp_id *with, int standing, int *nout){ return holds_core(keys, nkeys, -1, 2, standing, with, nout); }
Hold *holds_any(const lp_id *keys, int nkeys, int *nout){ return holds_core(keys, nkeys, -1, 2, 0, NULL, nout); }
void holds_free(Hold *h, int n){ if (!h) return; for (int i = 0; i < n; i++) free(h[i].path); free(h); }
