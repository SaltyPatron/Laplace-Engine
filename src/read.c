/* Reading the database. SQL only fetches: paths come back for a whole set of entities at once, one level of the DAG per
 * round trip, and are decoded and expanded here. */
#include "engine.h"
#include <arpa/inet.h>
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
struct Reader { PGconn *pg; Ent *e; size_t n, cap; uint32_t *slot; size_t scap; uint64_t trips; };

Reader *reader_new(PGconn *pg){ Reader *r = calloc(1, sizeof *r); r->pg = pg; return r; }
void reader_free(Reader *r){ if (!r) return; for (size_t i = 0; i < r->n; i++) { free(r->e[i].kid); free(r->e[i].run); } free(r->e); free(r->slot); free(r); }
uint64_t reader_trips(const Reader *r){ return r->trips; }

static uint64_t key(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 5, 8); return k; }
static Ent *ent(Reader *r, const lp_id *id, int add){
    if (add && (r->n + 1) * 2 > r->scap) {
        free(r->slot); r->scap = r->scap ? r->scap * 2 : 1024; r->slot = calloc(r->scap, 4);
        for (size_t i = 0; i < r->n; i++) { size_t k = key(&r->e[i].id) & (r->scap - 1); while (r->slot[k]) k = (k + 1) & (r->scap - 1); r->slot[k] = (uint32_t)i + 1; }
    }
    if (!r->scap) return NULL;
    size_t k = key(id) & (r->scap - 1);
    while (r->slot[k]) { Ent *x = &r->e[r->slot[k] - 1]; if (!memcmp(&x->id, id, 16)) return x; k = (k + 1) & (r->scap - 1); }
    if (!add) return NULL;
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 1024; r->e = xrealloc(r->e, r->cap * sizeof(Ent)); }
    Ent *x = &r->e[r->n]; memset(x, 0, sizeof *x); x->id = *id; r->slot[k] = (uint32_t)++r->n;
    return x;
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
