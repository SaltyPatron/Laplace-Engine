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

Ref atom(uint32_t cp){ Ref r; r.id = T0[cp].id; memcpy(r.m, T0[cp].m, 32); r.tier = 0; return r; }

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

Ref compose(const Ref *ch, uint32_t n, uint8_t tier){
    if (n == 1) return ch[0];
    lp_id stackids[64], *ids = n <= 64 ? stackids : malloc(sizeof(lp_id) * n);
    __int128 sum[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n; i++) { ids[i] = ch[i].id; for (int d = 0; d < 4; d++) sum[d] += ch[i].m[d]; }
    Ref r; lp_id_compose(ids, n, &r.id); r.tier = tier;
    for (int d = 0; d < 4; d++) r.m[d] = (int64_t)(sum[d] / (__int128)n);           /* exact, truncated toward zero */
    Shard *s = &shard[r.id.b[0]];
    pthread_mutex_lock(&s->mu);
    if ((s->n + 1) * 2 > s->scap) grow(s);
    uint64_t k = hkey(&r.id) & (s->scap - 1);
    while (s->slot[k]) {
        Node *x = &s->node[s->slot[k] - 1];
        if (!memcmp(&x->id, &r.id, 16)) { s->hits++; pthread_mutex_unlock(&s->mu); if (ids != stackids) free(ids); return r; }
        k = (k + 1) & (s->scap - 1);
    }
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 4096; s->node = xrealloc(s->node, s->cap * sizeof(Node)); }
    Node *x = &s->node[s->n]; x->id = r.id; memcpy(x->m, r.m, 32); x->tier = tier; x->len = n; x->voff = s->nv; x->nv = 0; x->keep = 0;
    for (uint32_t i = 0; i < n; i++) {                                                  /* runs of the same child */
        if (x->nv && !memcmp(&s->vtx[s->nv - 1].id, &ids[i], 16)) { s->vtx[s->nv - 1].run++; continue; }
        if (s->nv == s->vcap) { s->vcap = s->vcap ? s->vcap * 2 : 16384; s->vtx = xrealloc(s->vtx, s->vcap * sizeof(Vtx)); }
        s->vtx[s->nv].id = ids[i]; s->vtx[s->nv].run = 1; s->nv++; x->nv++;
    }
    s->slot[k] = (uint32_t)++s->n;
    pthread_mutex_unlock(&s->mu);
    if (ids != stackids) free(ids);
    return r;
}

/* Lookups run after decomposition, when nothing inserts: no lock. */
Node *table_find(const lp_id *id){
    Shard *s = &shard[id->b[0]]; if (!s->scap) return NULL;
    uint64_t k = hkey(id) & (s->scap - 1);
    while (s->slot[k]) { Node *x = &s->node[s->slot[k] - 1]; if (!memcmp(&x->id, id, 16)) return x; k = (k + 1) & (s->scap - 1); }
    return NULL;
}
uint64_t table_count(void){ uint64_t n = 0; for (int i = 0; i < NSHARD; i++) n += shard[i].n; return n; }
