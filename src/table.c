/* The node table: every composition decomposed in this run, keyed by ID. Threads insert concurrently; a shard is
 * chosen by the ID's first byte (a BLAKE3 hash, so shards fill evenly) and locked only while one node is looked up or
 * added. */
#include "engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

Shard shard[NSHARD];
const lp_tier0_record *T0;

double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
void *xrealloc(void *p, size_t n){ p = realloc(p, n ? n : 1); if (!p) { perror("realloc"); exit(1); } return p; }

void table_init(void){ for (int i = 0; i < NSHARD; i++) pthread_mutex_init(&shard[i].mu, NULL); }

void tier0_open(const char *path){
    T0 = lp_tier0_map(path);
    if (!T0) { fprintf(stderr, "cannot map tier 0 at %s (LAPLACE_TIER0; generate it with: laplace tier0)\n", path && *path ? path : lp_tier0_path()); exit(1); }
}

Ref atom(uint32_t cp){ Ref r = lp_ref_atom(T0, cp); r.said = 0; return r; }

static uint64_t hkey(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 1, 8); return k; }
static void grow(Shard *s){
    uint64_t oc = s->scap; uint32_t *old = s->slot; s->scap = oc ? oc * 2 : 1024; s->slot = calloc(s->scap, 4);
    for (uint64_t i = 0; i < oc; i++) if (old[i]) {
        uint64_t k = hkey(&s->node[old[i] - 1].id) & (s->scap - 1);
        while (s->slot[k]) k = (k + 1) & (s->scap - 1);
        s->slot[k] = old[i];
    }
    free(old);
}

/* A composition, recorded: Laplace-Native gives its ID and coordinate; the table keeps it, once, with its path. */
Ref compose(const Ref *ch, uint32_t n, uint8_t tier){
    if (n == 1) return ch[0];
    Ref r = lp_ref_compose(ch, n, tier); r.said = 0;
    Shard *s = &shard[r.id.b[0]];
    pthread_mutex_lock(&s->mu);
    if ((s->n + 1) * 2 > s->scap) grow(s);
    uint64_t k = hkey(&r.id) & (s->scap - 1);
    while (s->slot[k]) {
        Node *x = &s->node[s->slot[k] - 1];
        if (!memcmp(&x->id, &r.id, 16)) { s->hits++; pthread_mutex_unlock(&s->mu); return r; }
        k = (k + 1) & (s->scap - 1);
    }
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 4096; s->node = xrealloc(s->node, s->cap * sizeof(Node)); }
    Node *x = &s->node[s->n]; x->id = r.id; memcpy(x->m, r.c.m, 32); x->tier = tier; x->len = n; x->voff = s->nv; x->nv = 0; x->keep = 0; x->kind = 0;
    for (uint32_t i = 0; i < n; i++) {                                                  /* runs of the same child; what each is within this path, above the run */
        uint64_t said = (uint64_t)(ch[i].said & LP_M_SAID_MASK) << LP_M_RUN_BITS;
        if (x->nv && !memcmp(&s->vtx[s->nv - 1].id, &ch[i].id, 16) && (s->vtx[s->nv - 1].m & (LP_M_SAID_MASK << LP_M_RUN_BITS)) == said
                  && VRUN(s->vtx[s->nv - 1].m) < (1u << LP_M_RUN_BITS) - 1) { s->vtx[s->nv - 1].m++; continue; }
        if (s->nv == s->vcap) { s->vcap = s->vcap ? s->vcap * 2 : 16384; s->vtx = xrealloc(s->vtx, s->vcap * sizeof(Vtx)); }
        s->vtx[s->nv].id = ch[i].id; s->vtx[s->nv].m = 1ull | said; s->nv++; x->nv++;
    }
    s->slot[k] = (uint32_t)++s->n;
    pthread_mutex_unlock(&s->mu);
    /* what each child is said to be here, kept on the child: a claim, a record, a tuple, a file's metadata (its trunk is a file) */
    for (uint32_t i = 0; i < n; i++) if (ch[i].said) { Node *c = table_find(&ch[i].id); uint8_t bit = ch[i].said == LP_SAID_CLAIM ? LP_KIND_CLAIM : ch[i].said == LP_SAID_RECORD ? LP_KIND_RECORD : ch[i].said == LP_SAID_TUPLE ? LP_KIND_TUPLE : 255;
        if (c && bit != 255) __atomic_fetch_or(&c->kind, (uint8_t)(1u << bit), __ATOMIC_RELAXED);
        if (ch[i].said == LP_SAID_METADATA) { Node *me = table_find(&r.id); if (me) __atomic_fetch_or(&me->kind, (uint8_t)(1u << LP_KIND_FILE), __ATOMIC_RELAXED); } }
    return r;
}
static lp_ref compose_sink(void *sink, const lp_ref *ch, uint32_t n, uint8_t tier){ (void)sink; return compose(ch, n, tier); }

/* Text, decomposed by Laplace-Native and recorded here. */
Ctx **CTX; const lp_highway *HW;
Ref highway_typed(const char *list, const uint8_t *kv, size_t kn, int *has){
    *has = 0; Ref x; memset(&x, 0, sizeof x); if (!HW || !kn || kn >= 128) return x; const lp_list *l = lp_highway_list(HW, list); if (!l) return x;
    char key[128]; memcpy(key, kv, kn); key[kn] = 0; int64_t slot = lp_highway_key(HW, l, key); if (slot < 0) return x;
    const lp_tier0_record *rec = lp_highway_at(HW, l, (uint32_t)slot); if (!rec) return x;
    x.id = rec->id; memcpy(x.c.m, rec->m, 32); x.tier = (uint8_t)rec->pad; *has = 1; return x;
}
void ctx_open(int threads){
    if (!HW) HW = lp_highway_map(NULL);                                    /* the types' mask bits, when the highway is there */
    CTX = malloc(sizeof(Ctx *) * (size_t)threads);
    for (int i = 0; i < threads; i++) { CTX[i] = lp_text_new(T0); if (!CTX[i]) { fprintf(stderr, "cannot open ICU's break iterators\n"); exit(1); } }
}
Ref text_ref(Ctx *c, const uint8_t *s, size_t n){ Ref r = lp_text_decompose(c, s, n, compose_sink, NULL); r.said = 0; return r; }

/* Bytes that are not text by themselves: each byte as its notation, <0xAB>, composed. */
Ref notation_ref(Ctx *c, const uint8_t *b, size_t n){
    Ref stack[64], *r = n <= 64 ? stack : malloc(sizeof(Ref) * n); char tmp[8];
    for (size_t i = 0; i < n; i++) { snprintf(tmp, sizeof tmp, "<0x%02X>", b[i]); r[i] = text_ref(c, (const uint8_t *)tmp, 6); }
    Ref out = compose(r, (uint32_t)n, 3);
    if (r != stack) free(r);
    return out;
}

static uint64_t nodes_before, hits_before;
void strings_forget(void);
void table_reset(void){
    strings_forget();
    for (int i = 0; i < NSHARD; i++) { Shard *s = &shard[i]; nodes_before += s->n; hits_before += s->hits;
        free(s->node); free(s->slot); free(s->vtx); s->node = NULL; s->slot = NULL; s->vtx = NULL; s->n = s->cap = s->scap = s->nv = s->vcap = s->hits = 0; }
}
uint64_t table_total(void){ uint64_t n = nodes_before; for (int i = 0; i < NSHARD; i++) n += shard[i].n; return n; }
uint64_t table_hits(void){ uint64_t n = hits_before; for (int i = 0; i < NSHARD; i++) n += shard[i].hits; return n; }

/* Lookups run after decomposition, when nothing inserts: no lock. */
Node *table_find(const lp_id *id){
    Shard *s = &shard[id->b[0]]; if (!s->scap) return NULL;
    uint64_t k = hkey(id) & (s->scap - 1);
    while (s->slot[k]) { Node *x = &s->node[s->slot[k] - 1]; if (!memcmp(&x->id, id, 16)) return x; k = (k + 1) & (s->scap - 1); }
    return NULL;
}
uint64_t table_count(void){ uint64_t n = 0; for (int i = 0; i < NSHARD; i++) n += shard[i].n; return n; }
