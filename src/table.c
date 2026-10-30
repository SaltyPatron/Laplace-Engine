/* The node table: every composition decomposed in this run, keyed by ID. One flat table of slots, open addressing by
 * linear probing, shared by every thread without a lock (Research/Performance.md, concurrent tables): a slot is a
 * word, empty, or the ID's tag over the node's index; a hit is plain loads and never writes; a miss claims an empty
 * slot by compare-and-swap, marked busy while this thread writes the node, then published. The nodes and their
 * vertices go in two arrays reserved once and filled a thread's chunk at a time, so nothing is reallocated while
 * another thread reads. 256 locked shards made every thread composing the same new node wait on one lock. */
#include "engine.h"
#include "laplace/highway.h"
#include <omp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <immintrin.h>
#include <blake3.h>
#include <errno.h>
#include <unistd.h>
#include "laplace/tier0.h"
#include "laplace_reader_source.h"

Node *NODE; Vtx *VTX;
const lp_tier0_record *T0;

static __thread const File *reading_file;
const File *table_reading_file(const File *file){
    const File *previous = reading_file; reading_file = file; return previous;
}
static void input_error_prefix(void){
    if (reading_file) fprintf(stderr, "%s: ", reading_file->path);
}

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

/* Source-lifetime key lookup, independent of the batch's NODE/VTX storage.
 * Published entries are immutable. Replacing a duplicate key appends an earlier
 * ranked version with a head CAS; readers never lock or observe a partial Ref.
 * No entry is reclaimed while a source worker can retain it. */
#define KEY_BUCKETS (UINT64_C(1) << 20)
#define KEY_CAPACITY (UINT64_C(1) << 29)
#define KEY_TEXT_CAPACITY (UINT64_C(8) << 30)
typedef struct { lp_id hash; KeyField field; KeyRank rank; uint64_t next, file_next; } KeyEntry;
typedef struct { uint64_t *head, *local_head, *file_head, top, count, revision, text_top; KeyEntry *entry; uint8_t *text; } KeyTable;
static KeyTable keys;
static uint8_t *key_readers;
static __thread uint64_t key_reader = UINT64_MAX;
void keys_reader_file(uint64_t file){ key_reader = file; }
int keys_file_reads(uint64_t file){ return file<=UINT32_MAX && __atomic_load_n(&key_readers[file],__ATOMIC_ACQUIRE); }

/* Reader-source/perf-cache provenance for a reading. This fingerprint is
 * file metadata, not content identity. Compute it once without opening any
 * corpus file; changing the reader must invalidate an OS-only pre-check.
 * Executable bytes are not a reading contract: identical source built into the
 * standalone and embedded runtimes must produce identical provenance trunks. */
static pthread_mutex_t program_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t program_environment[32]; static int program_ready;
void program_environment_hash(uint8_t out[32]){
    if (!__atomic_load_n(&program_ready, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&program_mutex);
        if (!program_ready) {
            if (!T0) { fputs("reading contract requires the mapped Tier-0 cache\n", stderr); exit(1); }
            uint8_t artifact[32]; blake3_hasher hash; blake3_hasher_init(&hash);
            static const char source[] = LAPLACE_READER_SOURCE_ID;
            blake3_hasher_update(&hash, source, sizeof source - 1);
            blake3_hasher cache; blake3_hasher_init(&cache);
            blake3_hasher_update(&cache, T0, (size_t)LP_TIER0_COUNT * sizeof *T0);
            blake3_hasher_finalize(&cache, artifact, sizeof artifact); blake3_hasher_update(&hash, artifact, sizeof artifact);
            int available = program_file_hash(lp_highway_path(), artifact);
            if (!available && errno != ENOENT) { perror(lp_highway_path()); exit(1); }
            uint8_t present = (uint8_t)available; blake3_hasher_update(&hash, &present, sizeof present);
            if (available) blake3_hasher_update(&hash, artifact, sizeof artifact);
            blake3_hasher_finalize(&hash, program_environment, sizeof program_environment);
            __atomic_store_n(&program_ready, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&program_mutex);
    }
    memcpy(out, program_environment, sizeof program_environment);
}

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
static inline uint64_t hkey(const lp_id *id);
static inline uint64_t tag_of(const lp_id *id);
uint64_t table_end(void);
void table_size(uint64_t bytes){
    const char *e = getenv("LAPLACE_NODES_PER_MB");     /* measured: Universal Dependencies made 9,100 a MB */
    uint64_t per = e ? strtoull(e, NULL, 10) : 32768;
    uint64_t want=2 * ((bytes >> 20) + 1) * per;
    uint64_t old=smask+1,end=slot ? table_end() : 0;
    if (want<2*end) want=2*end;
    table_size_slots(want);
    if (!end || old==smask+1) return;
    /* File metadata and lookup keys can already have composed nodes before
     * the first batch is sized. Changing the mask must preserve their slots.
     * Callers resize only while record workers are idle. */
    uint64_t *next=area[!side]; os_discard(next,(smask+1)*sizeof *next);
    for (uint64_t i=0; i<end; i++) {
        const Node *node=NODE+i; if (!node->live) continue;
        uint64_t at=hkey(&node->id)&smask;
        while (next[at]) at=(at+1)&smask;
        next[at]=tag_of(&node->id)|(i+1);
    }
    os_discard(slot,old*sizeof *slot); slot=next; side=!side;
}
void table_init(void){
    if (slot) return;
    area[0] = reserve(SLOTS_MAX * sizeof *slot); area[1] = reserve(SLOTS_MAX * sizeof *slot); slot = area[0]; side = 0;
    vtx_cap = SLOTS_MAX * 4;
    NODE = reserve(SLOTS_MAX * sizeof(Node)); VTX = reserve(vtx_cap * sizeof(Vtx));
    keys.head = reserve(KEY_BUCKETS * sizeof *keys.head);
    keys.local_head = reserve(KEY_BUCKETS * sizeof *keys.local_head);
    keys.file_head = reserve(KEY_BUCKETS * sizeof *keys.file_head);
    keys.entry = reserve(KEY_CAPACITY * sizeof *keys.entry);
    keys.text = reserve(KEY_TEXT_CAPACITY);
    key_readers = reserve(UINT64_C(1)<<32);
    table_size_slots(1ull << 26);
}

/* This hash indexes a recipe-local symbol, never mints a content entity ID. */
void keys_domain(const char *recipe, const char *tier, uint8_t key_namespace, lp_id *out){
    blake3_hasher hash; blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, recipe, strlen(recipe) + 1);
    blake3_hasher_update(&hash, tier, strlen(tier) + 1);
    blake3_hasher_update(&hash, &key_namespace, 1);
    blake3_hasher_finalize(&hash, out->b, sizeof out->b);
}
/* A compiled field selector has its own namespace underneath a declaration.
 * These hashes index bindings, never identify substrate content. */
void keys_field_domain(const lp_id *domain, const char *path, lp_id *out){
    blake3_hasher hash; blake3_hasher_init(&hash); uint8_t field = 1;
    blake3_hasher_update(&hash, domain->b, sizeof domain->b);
    blake3_hasher_update(&hash, &field, 1);
    blake3_hasher_update(&hash, path, strlen(path));
    blake3_hasher_finalize(&hash, out->b, sizeof out->b);
}
static lp_id key_hash(const lp_id *domain, const uint8_t *text, size_t length){
    blake3_hasher hash; lp_id id; blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, domain->b, sizeof domain->b);
    blake3_hasher_update(&hash, text, length);
    blake3_hasher_finalize(&hash, id.b, sizeof id.b); return id;
}
static int key_rank_before(const KeyRank *a, const KeyRank *b){
    return a->file != b->file ? a->file < b->file : a->at != b->at ? a->at < b->at :
        a->unit != b->unit ? a->unit < b->unit : a->member < b->member;
}
static void key_publish(const lp_id *hash, const KeyField *field, const KeyRank *rank, int local){
    uint64_t bucket; memcpy(&bucket, hash->b, sizeof bucket);
    if (local) bucket ^= rank->file;
    uint64_t *head = &(local ? keys.local_head : keys.head)[bucket & (KEY_BUCKETS - 1)], index = 0;
    uint64_t observed = __atomic_load_n(head, __ATOMIC_ACQUIRE);
    for (;;) {
        const KeyEntry *previous = NULL;
        for (uint64_t at = observed; at; at = keys.entry[at - 1].next)
            if (lp_id_eq(&keys.entry[at - 1].hash, hash) && (!local || keys.entry[at - 1].rank.file == rank->file)) { previous = &keys.entry[at - 1]; break; }
        if (previous && !key_rank_before(rank, &previous->rank)) {
            /* A declaration may acquire its native product after the key
             * barrier. A later duplicate must never supply a missing field. */
            int same = !key_rank_before(&previous->rank, rank);
            int improves = (!previous->field.has_value && field->has_value) || (previous->field.blocked && !field->blocked);
            if (!same || !improves) return;
        }
        if (!index) {
            index = __atomic_fetch_add(&keys.top, 1, __ATOMIC_RELAXED) + 1;
            if (index > KEY_CAPACITY) { fputs("source key table capacity exhausted\n", stderr); exit(1); }
            KeyField owned = *field;
            if (field->has_text && field->length) {
                uint64_t at = __atomic_fetch_add(&keys.text_top, field->length, __ATOMIC_RELAXED);
                if (at > KEY_TEXT_CAPACITY || field->length > KEY_TEXT_CAPACITY - at) {
                    fputs("source key field text capacity exhausted\n", stderr); exit(1);
                }
                memcpy(keys.text + at, field->text, field->length); owned.text = keys.text + at;
            } else owned.text = NULL;
            keys.entry[index - 1] = (KeyEntry){.hash=*hash, .field=owned, .rank=*rank};
        }
        keys.entry[index - 1].next = observed;
        if (__atomic_compare_exchange_n(head, &observed, index, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (local) {
                uint64_t *file = &keys.file_head[rank->file & (KEY_BUCKETS - 1)];
                uint64_t seen = __atomic_load_n(file, __ATOMIC_ACQUIRE);
                do { keys.entry[index - 1].file_next = seen; }
                while (!__atomic_compare_exchange_n(file, &seen, index, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
            } else {
                if (!previous) __atomic_fetch_add(&keys.count, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&keys.revision, 1, __ATOMIC_RELEASE);
            }
            return;
        }
    }
}
void keys_restore(const lp_id *hash, const KeyField *field, const KeyRank *rank){
    if (field->has_value) table_pin(&field->value.id);
    key_publish(hash, field, rank, 0);
}
void keys_field_put(const lp_id *domain, const uint8_t *text, size_t length, const KeyField *field, const KeyRank *rank){
    if (field->has_value) table_pin(&field->value.id);
    lp_id hash = key_hash(domain, text, length);
    key_publish(&hash, field, rank, 0); key_publish(&hash, field, rank, 1);
}
static int binding_order(const void *a, const void *b){
    const KeyEntry *x = *(KeyEntry *const *)a, *y = *(KeyEntry *const *)b;
    return memcmp(x->hash.b, y->hash.b, sizeof x->hash.b);
}
size_t keys_file_export(uint64_t file, key_binding_fn emit, void *context){
    KeyEntry **entries = NULL; size_t n = 0, capacity = 0;
    for (uint64_t at = __atomic_load_n(&keys.file_head[file & (KEY_BUCKETS - 1)], __ATOMIC_ACQUIRE); at; at = keys.entry[at - 1].file_next) {
        KeyEntry *entry = &keys.entry[at - 1]; if (entry->rank.file != file) continue;
        uint64_t bucket; memcpy(&bucket, entry->hash.b, sizeof bucket); bucket ^= file;
        uint64_t latest = __atomic_load_n(&keys.local_head[bucket & (KEY_BUCKETS - 1)], __ATOMIC_ACQUIRE);
        while (latest && (!lp_id_eq(&keys.entry[latest - 1].hash, &entry->hash) || keys.entry[latest - 1].rank.file != file)) latest = keys.entry[latest - 1].next;
        if (latest != at) continue;
        if (n == capacity) { capacity = capacity ? capacity * 2 : 128; entries = xrealloc(entries, capacity * sizeof *entries); }
        entries[n++] = entry;
    }
    if (!n) { free(entries); return 0; }
    qsort(entries, n, sizeof *entries, binding_order);
    Ref marker = text_ref(CTX[0], (const uint8_t *)KEY_BINDING_TAG, sizeof KEY_BINDING_TAG - 1);
    Ref absent = text_ref(CTX[0], (const uint8_t *)"absent", 6);
    for (size_t i = 0; i < n; i++) {
        const KeyEntry *entry = entries[i]; const KeyField *field = &entry->field;
        char key[33], header[256]; id_text(&entry->hash, key);
        unsigned flags = field->has_value | (field->has_text << 1) | (field->blocked << 2);
        int length = snprintf(header, sizeof header, "%llu:%llu:%llu:%u:%u:%u:%u:%u:%u",
            (unsigned long long)entry->rank.at, (unsigned long long)entry->rank.unit, (unsigned long long)entry->rank.member,
            flags, field->has_value ? field->value.said : 0, field->has_value ? field->value.outcome : 0,
            field->has_value ? field->value.position : 0, field->has_value ? field->value.spare : 0, field->has_text ? field->length : 0);
        if (length < 0 || (size_t)length >= sizeof header) { fputs("invalid compiled binding metadata\n", stderr); exit(1); }
        Ref parts[5] = {marker, text_ref(CTX[0], (const uint8_t *)key, 32), text_ref(CTX[0], (const uint8_t *)header, (size_t)length),
            field->has_value ? field->value : absent, field->has_text && field->length ? text_ref(CTX[0], field->text, field->length) : absent};
        for (unsigned p = 0; p < 5; p++) { parts[p].said = parts[p].outcome = 0; parts[p].position = parts[p].spare = 0; }
        Ref binding = compose(parts, 5, ref_above(parts, 5)); binding.said = 0; emit(context, &binding);
    }
    free(entries); return n;
}
const KeyField *keys_field_get(const lp_id *domain, const uint8_t *text, size_t length){
    if (key_reader<=UINT32_MAX) __atomic_store_n(&key_readers[key_reader],1,__ATOMIC_RELEASE);
    lp_id hash = key_hash(domain, text, length); uint64_t bucket; memcpy(&bucket, hash.b, sizeof bucket);
    for (uint64_t at = __atomic_load_n(&keys.head[bucket & (KEY_BUCKETS - 1)], __ATOMIC_ACQUIRE);
         at; at = keys.entry[at - 1].next)
        if (lp_id_eq(&keys.entry[at - 1].hash, &hash)) return &keys.entry[at - 1].field;
    return NULL;
}
void keys_put(const lp_id *domain, const uint8_t *text, size_t length, Ref value, const KeyRank *rank){
    KeyField field = {.value=value, .has_value=1}; keys_field_put(domain, text, length, &field, rank);
}
int keys_get(const lp_id *domain, const uint8_t *text, size_t length, Ref *out){
    const KeyField *field = keys_field_get(domain, text, length);
    if (!field || !field->has_value) return 0;
    *out = field->value; return 1;
}
uint64_t keys_held(void){ return __atomic_load_n(&keys.count, __ATOMIC_ACQUIRE); }
uint64_t keys_revision(void){ return __atomic_load_n(&keys.revision, __ATOMIC_ACQUIRE); }

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
uint8_t ref_above(const Ref *r, size_t n){ uint8_t t = 0; for (size_t i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; if (t == UINT8_MAX) { if(n==1)return t; fputs("composition exceeds the Native tier representation\n", stderr); exit(2); } return (uint8_t)(t + 1); }
/* What a child is within this path, above its run: what it is said to be and, a claim of a record, how the record said
 * it (lp_m_full); the spare bits carry a score. Zero above the run is a child said nothing of. */
static inline uint64_t m_above(const Ref *c){ return lp_m_full(1, c->said, c->outcome, c->position) & ~((1ull << LP_M_RUN_BITS) - 1); }
static uint64_t said_differently;                                       /* a record composed again whose claims were said otherwise (records_differ) */
uint64_t records_differ(void){ return __atomic_load_n(&said_differently, __ATOMIC_RELAXED); }
/* A composition met again whose children say how they are said: the node keeps what was said first, and a record whose
 * claims were said otherwise (another outcome or place) under the same ID is counted. Its ID is its content's either
 * way; what differs is metadata that ID cannot hold twice. */
static void said_same(const Ref *ch, uint32_t n, const lp_id *id){
    int any = 0; for (uint32_t i = 0; i < n && !any; i++) any = ch[i].outcome || ch[i].position || ch[i].spare;
    if (!any) return;
    Node *x = table_find(id); if (!x) return; uint32_t v = 0, r = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (v >= x->nv) { __atomic_fetch_add(&said_differently, 1, __ATOMIC_RELAXED); return; }
        const Vtx *w = &VTX[x->voff + v];
        if ((w->m & ~((1ull << LP_M_RUN_BITS) - 1)) != m_above(&ch[i]) || w->spare != ch[i].spare) { __atomic_fetch_add(&said_differently, 1, __ATOMIC_RELAXED); return; }
        if (++r == VRUN(w->m)) { v++; r = 0; } }
}
/* A composition, recorded: Laplace-Native gives its ID and coordinate; the table keeps it, once, with its path. */
Ref compose(const Ref *ch, uint32_t n, uint8_t tier){
    if (!n) return lp_ref_compose(ch, 0, tier);
    static const lp_id absent = {{0}};
    for (uint32_t i=0; i<n; i++) if (lp_id_eq(&ch[i].id,&absent)) {
        input_error_prefix();
        fprintf(stderr,"invalid scalar content or unresolved child at composition operand %u\n",i); exit(1);
    }
    if (n == 1) return ch[0];
    uint8_t floor = ref_above(ch, n); if (tier < floor) tier = floor;
    Ref r = lp_ref_compose(ch, n, tier); r.said = 0; r.outcome = 0; r.position = 0; r.spare = 0;
    Seen *c = seen_of(); uint64_t h = hkey(&r.id); uint32_t at = (uint32_t)(h >> 17) & (SEEN - 1);
    if (!memcmp(&c->id[at], &r.id, 16) && c->tier[at] <= tier) { c->hits++; said_same(ch, n, &r.id); return r; }   /* in the table, as this thread found, at this tier or lower */
    c->id[at] = r.id; c->tier[at] = tier;                               /* in the table once this returns, found or added, at this tier or lower */
    if (__atomic_load_n(&node_top, __ATOMIC_RELAXED) > grow_at) grow();
    enter(c);
    uint64_t tag = tag_of(&r.id), k = h & smask;
    for (;;) {
        uint64_t w = __atomic_load_n(&slot[k], __ATOMIC_ACQUIRE);
        if (!w) {
            if (!__atomic_compare_exchange_n(&slot[k], &w, tag | BUSY, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;  /* another thread took it: look again */
            Node *x = node_new(c); int own; uint64_t vo = vtx_room(c, n, &own); Vtx *vt = &VTX[vo]; uint32_t nv = 0;
            x->id = r.id; memcpy(x->m, r.c.m, 32); x->tier = tier; x->len = n; x->voff = vo; x->keep = 0; x->kind = 0; x->spine = x->pin = 0;
            for (uint32_t i = 0; i < n; i++) {                                          /* runs of the same child said the same way; what each is within this path, above the run */
                uint64_t above = m_above(&ch[i]);
                if (nv && !memcmp(&vt[nv - 1].id, &ch[i].id, 16) && (vt[nv - 1].m & ~((1ull << LP_M_RUN_BITS) - 1)) == above && vt[nv - 1].spare == ch[i].spare
                       && VRUN(vt[nv - 1].m) < (1u << LP_M_RUN_BITS) - 1) { vt[nv - 1].m++; continue; }
                vt[nv].id = ch[i].id; vt[nv].m = 1ull | above; vt[nv].spare = ch[i].spare; nv++;
            }
            x->nv = nv; x->live = 1; if (!own) c->v_at += nv;
            __atomic_store_n(&slot[k], tag | (uint64_t)(x - NODE + 1), __ATOMIC_RELEASE);
            leave(c); c->added++; return r;
        }
        if ((w & ~IDX_MASK) == tag) {
            if ((w & IDX_MASK) == BUSY) w = published(&slot[k]);
            if (!memcmp(&NODE[(w & IDX_MASK) - 1].id, &r.id, 16)) { tier_floor(&NODE[(w & IDX_MASK) - 1], tier); leave(c); c->hits++; said_same(ch, n, &r.id); return r; }
        }
        k = (k + 1) & smask;
    }
}
/* Caller holds an active table guard, so its slot area cannot be resized. */
static Node *table_find_active(const lp_id *id){
    Node *found = NULL;
    uint64_t tag = tag_of(id), k = hkey(id) & smask, w;
    while ((w = __atomic_load_n(&slot[k], __ATOMIC_ACQUIRE))) {
        if ((w & ~IDX_MASK) == tag) { if ((w & IDX_MASK) == BUSY) w = published(&slot[k]);
            Node *x = &NODE[(w & IDX_MASK) - 1]; if (!memcmp(&x->id, id, 16)) { found = x; break; } }
        k = (k + 1) & smask;
    }
    return found;
}
/* A node by its ID; safe while others insert (a slot being written is waited for). */
Node *table_find(const lp_id *id){
    if (!slot) return NULL;
    Seen *c=seen_of(); enter(c); Node *found=table_find_active(id); leave(c); return found;
}
void table_pin(const lp_id *id){
    Node *node = table_find(id); if (node) __atomic_store_n(&node->pin, 1, __ATOMIC_RELEASE);
}
size_t table_parts(const lp_id *id, lp_id *out, size_t cap){
    Node *x = table_find(id); if (!x) return 0; size_t n = 0;
    for (uint32_t v = 0; v < x->nv && n < cap; v++) for (uint32_t r = 0; r < VRUN(VTX[x->voff + v].m) && n < cap; r++) out[n++] = VTX[x->voff + v].id;
    return n;
}
uint64_t table_end(void){ uint64_t e = __atomic_load_n(&node_top, __ATOMIC_ACQUIRE); return e < node_cap ? e : node_cap; }
/* What each child of every node is said to be, kept on the child: a claim, a record, a tuple, a file's metadata (its
 * holder is a file). Read off the vertices' M after the decomposition, when nothing inserts. */
static void table_kind(const lp_id *id, uint64_t said, int active){
    uint8_t bit = said == LP_SAID_CLAIM ? LP_KIND_CLAIM : said == LP_SAID_RECORD ? LP_KIND_RECORD : said == LP_SAID_TUPLE ? LP_KIND_TUPLE : 255;
    if (said!=LP_SAID_HOLDS && bit==255) return;
    Node *child=active?table_find_active(id):table_find(id); if (!child) return;
    if (said==LP_SAID_HOLDS) __atomic_store_n(&child->spine,1,__ATOMIC_RELAXED);
    if (bit!=255) __atomic_fetch_or(&child->kind,(uint8_t)(1u<<bit),__ATOMIC_RELAXED);
}
void table_annotate_roots(const Ref *roots, size_t count){
    for(size_t i=0;i<count;i++)table_kind(&roots[i].id,roots[i].said,0);
}
void table_kinds(void){
    uint64_t end = table_end();
    #pragma omp parallel
    {
        /* This scan never composes. Protect its stable slot area once per
         * worker rather than issuing a resize fence for every marked edge. */
        Seen *c=seen_of(); enter(c);
        #pragma omp for schedule(dynamic, 65536)
        for (uint64_t i = 0; i < end; i++) { Node *x = &NODE[i]; if (!x->live) continue;
            for (uint32_t v = 0; v < x->nv; v++) { uint64_t said = VSAID(VTX[x->voff + v].m); if (!said) continue;
                table_kind(&VTX[x->voff + v].id, said,1);
                if (said == LP_SAID_METADATA) __atomic_fetch_or(&x->kind, (uint8_t)(1u << LP_KIND_FILE), __ATOMIC_RELAXED); } }
        leave(c);
    }
}
static lp_ref compose_sink(void *sink, const lp_ref *ch, uint32_t n, uint8_t tier){ (void)sink; return compose(ch, n, tier); }

/* Text, decomposed by Laplace-Native and recorded here. */
Ctx **CTX; const lp_highway *HW;
Ref highway_typed(const char *list, const uint8_t *kv, size_t kn, int *has){
    *has = 0; Ref x; memset(&x, 0, sizeof x); if (!HW || !kn || kn > 1000) return x;
    char key[1001]; memcpy(key, kv, kn); key[kn] = 0;
    const lp_list *typed=lp_highway_list(HW,list);
    const char *space=typed ? typed->name : list; uint32_t cursor=0;
    const lp_tier0_record *rec=lp_highway_resolve_next(HW,space,key,&cursor); if (!rec) return x;
    if (lp_highway_resolve_next(HW,space,key,&cursor)) { *has=-1; return x; }
    x.id = rec->id; memcpy(x.c.m, rec->m, 32); x.tier = (uint8_t)rec->pad; *has = 1; return x;
}
void ctx_open(int threads){
    if (!HW) HW = lp_highway_map(NULL);                                    /* the types' mask bits, when the highway is there */
    CTX = malloc(sizeof(Ctx *) * (size_t)threads);
    for (int i = 0; i < threads; i++) { CTX[i] = lp_text_new(T0); if (!CTX[i]) { fprintf(stderr, "cannot open ICU's break iterators\n"); exit(1); } }
}
typedef struct { lp_id *out; size_t cap, count; } TextOccurrences;
static void text_occurrence(void *sink, size_t offset, size_t length, const lp_ref *value){
    TextOccurrences *parts = sink; (void)offset; (void)length;
    if (parts->count < parts->cap) parts->out[parts->count] = value->id;
    parts->count++;
}
Ref text_occurrences(Ctx *c, const uint8_t *s, size_t n, lp_id *out, size_t cap, size_t *count){
    /* Native owns the canonical UAX #29 tree. A worker or byte-window boundary
     * must never introduce a second text decomposition or change its ID. */
    TextOccurrences parts = { out, cap, 0 };
    Ref r = lp_text_decompose_segments(c, s, n, compose_sink, NULL,
        count ? text_occurrence : NULL, &parts); r.said = 0;
    if (count) *count = parts.count;
    static const lp_id absent = {{0}};
    if (n && lp_id_eq(&r.id,&absent)) { input_error_prefix(); fputs("invalid scalar content has no canonical text entity\n",stderr); exit(1); }
    return r;
}
Ref text_ref(Ctx *c, const uint8_t *s, size_t n){
    return text_occurrences(c, s, n, NULL, 0, NULL);
}

static uint64_t nodes_before, hits_before;
static uint64_t seen_sum(int forget){
    uint64_t n = 0; pthread_mutex_lock(&seen_mu);
    for (Seen *c = seen_all; c; c = c->next) { n += forget == 2 ? c->added : c->hits; if (forget == 1) c->hits = 0; if (forget == 3) c->added = 0; }
    pthread_mutex_unlock(&seen_mu); return n;
}
uint64_t table_count(void){ return seen_sum(2); }
uint64_t table_resident_bytes(void){
    return (smask + 1) * sizeof *slot + table_end() * sizeof *NODE
        + __atomic_load_n(&vtx_top, __ATOMIC_ACQUIRE) * sizeof *VTX;
}
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
/* The active node table and immutable caches own canonical reference geometry. */
Ref ref_of_id(const lp_id *id, int *ok){
    Ref r; memset(&r, 0, sizeof r); *ok = 1;
    Node *x = table_find(id); if (x) { r.id = *id; memcpy(r.c.m, x->m, 32); r.tier = x->tier; return r; }
    int64_t cp = lp_tier0_codepoint(T0, id); if (cp >= 0) return atom((uint32_t)cp);
    const lp_tier0_record *cached=lp_highway_find(HW,id);
    if (cached) { r.id=*id; memcpy(r.c.m,cached->m,sizeof r.c.m); r.tier=(uint8_t)cached->pad; return r; }
    *ok = 0; r.id = *id; return r;
}
