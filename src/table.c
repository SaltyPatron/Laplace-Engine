/* The node table: every composition decomposed in this run, keyed by ID. One flat table of slots, open addressing by
 * linear probing, shared by every thread without a lock (Research/Performance.md, concurrent tables): a slot is a
 * word, empty, or the ID's tag over the node's index; a hit is plain loads and never writes; a miss claims an empty
 * slot by compare-and-swap, marked busy while this thread writes the node, then published. The nodes and their
 * vertices go in two arrays reserved once and filled a thread's chunk at a time, so nothing is reallocated while
 * another thread reads. 256 locked shards made every thread composing the same new node wait on one lock. */
#include "engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <immintrin.h>

Node *NODE; Vtx *VTX;
const lp_tier0_record *T0;

double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
void *xrealloc(void *p, size_t n){ p = realloc(p, n ? n : 1); if (!p) { perror("realloc"); exit(1); } return p; }

#define IDX_BITS 40
#define IDX_MASK ((1ull << IDX_BITS) - 1)
#define BUSY IDX_MASK                                   /* a slot claimed, its node still being written */
#define SLOTS_MAX (1ull << 32)                          /* the most a table can be: reserved, touched only as used */
#define NCHUNK 4096                                     /* nodes a thread takes at a time */
#define VCHUNK (1u << 16)                               /* vertices a thread takes at a time */
static uint64_t *slot, smask, node_top, vtx_top, node_cap, vtx_cap, grow_at;
static uint64_t *area[2]; static int side, resizing;          /* the slots alternate between two reserved areas as the table doubles */

static void *reserve(size_t bytes){
    void *p = os_reserve(bytes);
    if (!p) { perror("reserve"); exit(1); }
    return p;
}
/* The table sized for what one batch can make: twice the nodes it is expected to hold, so it stays at most half full.
 * Only while the table is empty. */
static void table_size_slots(uint64_t want){
    uint64_t n = 1u << 20; while (n < want && n < SLOTS_MAX) n <<= 1;
    smask = n - 1; node_cap = n - n / 8; grow_at = n / 2;  /* half full, it doubles; past seven eighths, probing is no longer a table */
}
void table_size(uint64_t bytes){
    const char *e = getenv("LAPLACE_NODES_PER_MB");     /* measured: Universal Dependencies made 9,100 a MB */
    uint64_t per = e ? strtoull(e, NULL, 10) : 32768;
    table_size_slots(2 * ((bytes >> 20) + 1) * per);
}
void table_init(void){
    if (slot) return;
    area[0] = reserve(SLOTS_MAX * sizeof *slot); area[1] = reserve(SLOTS_MAX * sizeof *slot); slot = area[0]; side = 0;
    vtx_cap = SLOTS_MAX * 4;
    NODE = reserve(SLOTS_MAX * sizeof(Node)); VTX = reserve(vtx_cap * sizeof(Vtx));
    table_size_slots(1ull << 26);
}

void tier0_open(const char *path){
    T0 = lp_tier0_map(path);
    if (!T0) { fprintf(stderr, "cannot map tier 0 at %s (LAPLACE_TIER0; generate it with: laplace tier0)\n", path && *path ? path : lp_tier0_path()); exit(1); }
}

Ref atom(uint32_t cp){ Ref r = lp_ref_atom(T0, cp); r.said = 0; return r; }

static inline uint64_t hkey(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 1, 8); return k; }
static inline uint64_t tag_of(const lp_id *id){ return ((uint64_t)id->b[9] | (uint64_t)id->b[10] << 8 | (uint64_t)id->b[11] << 16) << IDX_BITS; }
static inline uint64_t published(uint64_t *s){         /* a slot's word once its node is written */
    uint64_t w = __atomic_load_n(s, __ATOMIC_ACQUIRE);
    while ((w & IDX_MASK) == BUSY) { _mm_pause(); w = __atomic_load_n(s, __ATOMIC_ACQUIRE); }
    return w;
}
static void full(const char *what){
    fprintf(stderr, "\nthe node table is full of %s: a batch made more than LAPLACE_NODES_PER_MB allows (%llu slots); raise it or LAPLACE_BATCH_MB lower\n",
            what, (unsigned long long)(smask + 1));
    exit(1);
}

/* What this thread has already found in the table, by ID: a repeat (the same tag, the same empty field, the same small
 * tuple, row after row) is answered without touching the shared slots. Its chunks of nodes and vertices, and its
 * counts, are here too. The table emptied between batches empties these: a cache holds only what the table holds now
 * (its epoch). */
#define SEEN (1u << 15)
typedef struct Seen { uint64_t active, epoch, hits, added, n_at, n_end, v_at, v_end; struct Seen *next; lp_id id[SEEN]; uint8_t tier[SEEN]; } Seen;
static uint64_t epoch = 1; static Seen *seen_all; static pthread_mutex_t seen_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread Seen *seen_here;
static Seen *seen_of(void){
    Seen *c = seen_here;
    if (!c) { c = calloc(1, sizeof *c); if (!c) { perror("calloc"); exit(1); } pthread_mutex_lock(&seen_mu); c->next = seen_all; seen_all = c; pthread_mutex_unlock(&seen_mu); seen_here = c; }
    uint64_t e = __atomic_load_n(&epoch, __ATOMIC_ACQUIRE);
    if (c->epoch != e) { memset(c->id, 0, sizeof c->id); memset(c->tier, 0, sizeof c->tier); c->n_at = c->n_end = c->v_at = c->v_end = 0; c->epoch = e; }
    return c;
}
/* The table doubles when it is half full: what a batch makes is not known before it is decomposed (Unicode's small files
 * make far more nodes a byte than a corpus). A thread inside the table says so (active); the thread that doubles it waits
 * until none is, puts every node into slots twice as many, and lets them go on. Nothing is copied but the slots. */
static inline void enter(Seen *c){
    for (;;) {
        __atomic_store_n(&c->active, 1, __ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&resizing, __ATOMIC_SEQ_CST)) return;
        __atomic_store_n(&c->active, 0, __ATOMIC_RELEASE);
        while (__atomic_load_n(&resizing, __ATOMIC_ACQUIRE)) _mm_pause();
    }
}
static inline void leave(Seen *c){ __atomic_store_n(&c->active, 0, __ATOMIC_RELEASE); }
uint64_t table_end(void);
static void grow(void){
    int z = 0;
    if (!__atomic_compare_exchange_n(&resizing, &z, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { while (__atomic_load_n(&resizing, __ATOMIC_ACQUIRE)) _mm_pause(); return; }
    if (__atomic_load_n(&node_top, __ATOMIC_ACQUIRE) > grow_at && smask + 1 < SLOTS_MAX) {      /* not already doubled by another */
        pthread_mutex_lock(&seen_mu);
        for (Seen *s = seen_all; s; s = s->next) while (__atomic_load_n(&s->active, __ATOMIC_SEQ_CST)) _mm_pause();
        pthread_mutex_unlock(&seen_mu);
        uint64_t old = smask + 1, n = old * 2, m = n - 1, end = table_end(); uint64_t *ns = area[!side];
        for (uint64_t i = 0; i < end; i++) { const Node *x = &NODE[i]; if (!x->live) continue;
            uint64_t k = hkey(&x->id) & m; while (ns[k]) k = (k + 1) & m; ns[k] = tag_of(&x->id) | (i + 1); }
        os_discard(slot, old * sizeof *slot);                    /* the old slots, empty for the next time */
        slot = ns; side = !side; smask = m; node_cap = n - n / 8; grow_at = n / 2;
    }
    __atomic_store_n(&resizing, 0, __ATOMIC_SEQ_CST);
}
static Node *node_new(Seen *c){
    if (c->n_at == c->n_end) {
        uint64_t a = __atomic_fetch_add(&node_top, NCHUNK, __ATOMIC_RELAXED);
        if (a + NCHUNK > node_cap) full("nodes");
        c->n_at = a; c->n_end = a + NCHUNK;
    }
    return &NODE[c->n_at++];
}
/* Room for n vertices, in a run: from the thread's chunk, or a run of its own for a node too long for one. */
static uint64_t vtx_room(Seen *c, uint32_t n, int *own){
    *own = 0;
    if (c->v_end - c->v_at >= n) return c->v_at;
    if (n > VCHUNK / 4) { uint64_t a = __atomic_fetch_add(&vtx_top, n, __ATOMIC_RELAXED); if (a + n > vtx_cap) full("vertices"); *own = 1; return a; }
    uint64_t a = __atomic_fetch_add(&vtx_top, VCHUNK, __ATOMIC_RELAXED); if (a + VCHUNK > vtx_cap) full("vertices");
    c->v_at = a; c->v_end = a + VCHUNK; return a;
}

/* A node is recorded at the lowest tier it is composed at (Compositions: a node can fill a higher tier, never a lower
 * one): [a,n] repeated inside "banana" is a tier 1 block and the word "an" is tier 2; whichever thread composed it first
 * no longer decides. */
static inline void tier_floor(Node *x, uint8_t tier){
    uint8_t t = __atomic_load_n(&x->tier, __ATOMIC_RELAXED);
    while (tier < t && !__atomic_compare_exchange_n(&x->tier, &t, tier, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) ;
}
uint8_t ref_above(const Ref *r, size_t n){ uint8_t t = 0; for (size_t i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }
/* A composition, recorded: Laplace-Native gives its ID and coordinate; the table keeps it, once, with its path. */
Ref compose(const Ref *ch, uint32_t n, uint8_t tier){
    if (n == 1) return ch[0];
    Ref r = lp_ref_compose(ch, n, tier); r.said = 0;
    Seen *c = seen_of(); uint64_t h = hkey(&r.id); uint32_t at = (uint32_t)(h >> 17) & (SEEN - 1);
    if (!memcmp(&c->id[at], &r.id, 16) && c->tier[at] <= tier) { c->hits++; return r; }   /* in the table, as this thread found, at this tier or lower */
    c->id[at] = r.id; c->tier[at] = tier;                               /* in the table once this returns, found or added, at this tier or lower */
    if (__atomic_load_n(&node_top, __ATOMIC_RELAXED) > grow_at) grow();
    enter(c);
    uint64_t tag = tag_of(&r.id), k = h & smask;
    for (;;) {
        uint64_t w = __atomic_load_n(&slot[k], __ATOMIC_ACQUIRE);
        if (!w) {
            if (!__atomic_compare_exchange_n(&slot[k], &w, tag | BUSY, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;  /* another thread took it: look again */
            Node *x = node_new(c); int own; uint64_t vo = vtx_room(c, n, &own); Vtx *vt = &VTX[vo]; uint32_t nv = 0;
            x->id = r.id; memcpy(x->m, r.c.m, 32); x->tier = tier; x->len = n; x->voff = vo; x->keep = 0; x->kind = 0;
            for (uint32_t i = 0; i < n; i++) {                                          /* runs of the same child; what each is within this path, above the run */
                uint64_t said = (uint64_t)(ch[i].said & LP_M_SAID_MASK) << LP_M_RUN_BITS;
                if (nv && !memcmp(&vt[nv - 1].id, &ch[i].id, 16) && (vt[nv - 1].m & (LP_M_SAID_MASK << LP_M_RUN_BITS)) == said
                       && VRUN(vt[nv - 1].m) < (1u << LP_M_RUN_BITS) - 1) { vt[nv - 1].m++; continue; }
                vt[nv].id = ch[i].id; vt[nv].m = 1ull | said; nv++;
            }
            x->nv = nv; x->live = 1; if (!own) c->v_at += nv;
            __atomic_store_n(&slot[k], tag | (uint64_t)(x - NODE + 1), __ATOMIC_RELEASE);
            leave(c); c->added++; return r;
        }
        if ((w & ~IDX_MASK) == tag) {
            if ((w & IDX_MASK) == BUSY) w = published(&slot[k]);
            if (!memcmp(&NODE[(w & IDX_MASK) - 1].id, &r.id, 16)) { tier_floor(&NODE[(w & IDX_MASK) - 1], tier); leave(c); c->hits++; return r; }
        }
        k = (k + 1) & smask;
    }
}
/* A node by its ID; safe while others insert (a slot being written is waited for). */
Node *table_find(const lp_id *id){
    if (!slot) return NULL;
    Seen *c = seen_of(); enter(c); Node *found = NULL;
    uint64_t tag = tag_of(id), k = hkey(id) & smask, w;
    while ((w = __atomic_load_n(&slot[k], __ATOMIC_ACQUIRE))) {
        if ((w & ~IDX_MASK) == tag) { if ((w & IDX_MASK) == BUSY) w = published(&slot[k]);
            Node *x = &NODE[(w & IDX_MASK) - 1]; if (!memcmp(&x->id, id, 16)) { found = x; break; } }
        k = (k + 1) & smask;
    }
    leave(c); return found;
}
size_t table_parts(const lp_id *id, lp_id *out, size_t cap){
    Node *x = table_find(id); if (!x) return 0; size_t n = 0;
    for (uint32_t v = 0; v < x->nv && n < cap; v++) for (uint32_t r = 0; r < VRUN(VTX[x->voff + v].m) && n < cap; r++) out[n++] = VTX[x->voff + v].id;
    return n;
}
uint64_t table_end(void){ uint64_t e = __atomic_load_n(&node_top, __ATOMIC_ACQUIRE); return e < node_cap ? e : node_cap; }
/* What each child of every node is said to be, kept on the child: a claim, a record, a tuple, a file's metadata (its
 * holder is a file). Read off the vertices' M after the decomposition, when nothing inserts. */
void table_kinds(void){
    uint64_t end = table_end();
    #pragma omp parallel for schedule(dynamic, 65536)
    for (uint64_t i = 0; i < end; i++) { Node *x = &NODE[i]; if (!x->live) continue;
        for (uint32_t v = 0; v < x->nv; v++) { uint64_t said = VSAID(VTX[x->voff + v].m); if (!said) continue;
            uint8_t bit = said == LP_SAID_CLAIM ? LP_KIND_CLAIM : said == LP_SAID_RECORD ? LP_KIND_RECORD : said == LP_SAID_TUPLE ? LP_KIND_TUPLE : 255;
            if (bit != 255) { Node *c = table_find(&VTX[x->voff + v].id); if (c) __atomic_fetch_or(&c->kind, (uint8_t)(1u << bit), __ATOMIC_RELAXED); }
            if (said == LP_SAID_METADATA) __atomic_fetch_or(&x->kind, (uint8_t)(1u << LP_KIND_FILE), __ATOMIC_RELAXED); } }
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

static uint64_t nodes_before, hits_before;
static uint64_t seen_sum(int forget){
    uint64_t n = 0; pthread_mutex_lock(&seen_mu);
    for (Seen *c = seen_all; c; c = c->next) { n += forget == 2 ? c->added : c->hits; if (forget == 1) c->hits = 0; if (forget == 3) c->added = 0; }
    pthread_mutex_unlock(&seen_mu); return n;
}
uint64_t table_count(void){ return seen_sum(2); }
void strings_forget(void);
/* Emptied between batches: the pages handed back are zero when touched again, so the slots are empty and the nodes'
 * live marks clear without a pass over them. */
void table_reset(void){
    strings_forget();
    nodes_before += seen_sum(2); seen_sum(3); hits_before += seen_sum(0); seen_sum(1);
    uint64_t ne = table_end(), nv = __atomic_load_n(&vtx_top, __ATOMIC_ACQUIRE);
    os_discard(slot, (smask + 1) * sizeof *slot);
    os_discard(NODE, ne * sizeof(Node));
    os_discard(VTX, (nv < vtx_cap ? nv : vtx_cap) * sizeof(Vtx));
    node_top = vtx_top = 0;
    __atomic_fetch_add(&epoch, 1, __ATOMIC_RELEASE);                    /* every thread's cache and chunks are of a table that is gone */
}
uint64_t table_total(void){ return nodes_before + table_count(); }
uint64_t table_hits(void){ return hits_before + seen_sum(0); }
