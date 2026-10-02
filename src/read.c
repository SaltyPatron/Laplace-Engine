/* Reading the database. SQL only fetches: paths come back for a whole set of entities at once, one level of the DAG per
 * round trip, and are decoded and expanded here. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
