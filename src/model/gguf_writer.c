#include "gguf_writer.h"
#include "laplace/os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GGUF_ALIGN 32u

typedef enum {
    KV_STR,
    KV_U32,
    KV_U64,
    KV_F32,
    KV_BOOL,
    KV_STR_ARRAY,
    KV_U8_ARRAY,
    KV_F32_ARRAY,
    KV_I32_ARRAY
} KvKind;

typedef struct {
    char    *key;
    KvKind   kind;
    char    *str_val;
    uint64_t u64_val;
    float    f32_val;
    size_t   count;
    lp_buf   blob;
} KvEntry;

typedef struct {
    char     *name;
    uint32_t  ggml_type;
    uint32_t  ndim;
    uint64_t  dims[LP_TENSOR_MAX_DIMS];
    size_t    bytes;
} TensorMeta;

struct lp_gguf_writer {
    char       *output_path;
    KvEntry    *kvs;
    size_t      nkvs, ckvs;
    TensorMeta *tensors;
    size_t      ntensors, ctensors;
    lp_strmap  *kv_seen;
    lp_strmap  *tensor_seen;
    FILE       *payload;
    int         failed;
    int         finalized;
};

static void put_u32(lp_buf *b, uint32_t v){
    uint8_t u[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    lp_buf_put(b, u, 4);
}
static void put_u64(lp_buf *b, uint64_t v){
    uint8_t u[8];
    for (int i = 0; i < 8; i++) u[i] = (uint8_t)(v >> (8 * i));
    lp_buf_put(b, u, 8);
}
static void put_gstr(lp_buf *b, const char *s, size_t len){
    put_u64(b, (uint64_t)len);
    if (len) lp_buf_put(b, s, len);
}
static size_t align_up(size_t v, size_t align){
    return (v + align - 1u) & ~(align - 1u);
}

lp_gguf_writer *lp_gguf_writer_create(const char *output_path){
    if (!output_path || !*output_path) return NULL;
    lp_gguf_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->output_path = strdup(output_path);
    w->kv_seen = lp_strmap_sized(1);
    w->tensor_seen = lp_strmap_sized(1);
    w->payload = os_tmpfile();
    if (!w->output_path || !w->kv_seen || !w->tensor_seen || !w->payload) {
        lp_gguf_writer_free(w);
        return NULL;
    }
    return w;
}

static KvEntry *push_kv(lp_gguf_writer *w, const char *key, KvKind kind){
    if (!w || w->failed || w->finalized || !key) return NULL;
    bool fresh = false;
    lp_strmap_put(w->kv_seen, key, strlen(key), &fresh);
    if (!fresh) return NULL;
    if (w->nkvs == w->ckvs) {
        w->ckvs = w->ckvs ? w->ckvs * 2u : 32u;
        w->kvs = xrealloc(w->kvs, sizeof *w->kvs * w->ckvs);
    }
    KvEntry *e = &w->kvs[w->nkvs++];
    memset(e, 0, sizeof *e);
    e->key = strdup(key);
    e->kind = kind;
    return e;
}

int lp_gguf_writer_add_str(lp_gguf_writer *w, const char *key, const char *value){
    if (!value) return -1;
    KvEntry *e = push_kv(w, key, KV_STR);
    if (!e) return -1;
    e->str_val = strdup(value);
    return e->str_val ? 0 : -1;
}

int lp_gguf_writer_add_u32(lp_gguf_writer *w, const char *key, uint32_t value){
    KvEntry *e = push_kv(w, key, KV_U32);
    if (!e) return -1;
    e->u64_val = value;
    return 0;
}

int lp_gguf_writer_add_u64(lp_gguf_writer *w, const char *key, uint64_t value){
    KvEntry *e = push_kv(w, key, KV_U64);
    if (!e) return -1;
    e->u64_val = value;
    return 0;
}

int lp_gguf_writer_add_f32(lp_gguf_writer *w, const char *key, float value){
    KvEntry *e = push_kv(w, key, KV_F32);
    if (!e) return -1;
    e->f32_val = value;
    return 0;
}

int lp_gguf_writer_add_bool(lp_gguf_writer *w, const char *key, int value){
    KvEntry *e = push_kv(w, key, KV_BOOL);
    if (!e) return -1;
    e->u64_val = value ? 1u : 0u;
    return 0;
}

int lp_gguf_writer_add_str_array(lp_gguf_writer *w, const char *key,
                                 const char *const *values, size_t count){
    if (count && !values) return -1;
    KvEntry *e = push_kv(w, key, KV_STR_ARRAY);
    if (!e) return -1;
    e->count = count;
    for (size_t i = 0; i < count; i++) {
        const char *s = values[i] ? values[i] : "";
        put_gstr(&e->blob, s, strlen(s));
    }
    return 0;
}

int lp_gguf_writer_add_u8_array(lp_gguf_writer *w, const char *key,
                                const uint8_t *values, size_t count){
    if (count && !values) return -1;
    KvEntry *e = push_kv(w, key, KV_U8_ARRAY);
    if (!e) return -1;
    e->count = count;
    if (count) lp_buf_put(&e->blob, values, count);
    return 0;
}

int lp_gguf_writer_add_f32_array(lp_gguf_writer *w, const char *key,
                                 const float *values, size_t count){
    if (count && !values) return -1;
    KvEntry *e = push_kv(w, key, KV_F32_ARRAY);
    if (!e) return -1;
    e->count = count;
    for (size_t i = 0; i < count; i++) {
        uint32_t bits; memcpy(&bits, &values[i], 4);
        put_u32(&e->blob, bits);
    }
    return 0;
}

int lp_gguf_writer_add_i32_array(lp_gguf_writer *w, const char *key,
                                 const int32_t *values, size_t count){
    if (count && !values) return -1;
    KvEntry *e = push_kv(w, key, KV_I32_ARRAY);
    if (!e) return -1;
    e->count = count;
    for (size_t i = 0; i < count; i++) put_u32(&e->blob, (uint32_t)values[i]);
    return 0;
}

int lp_gguf_writer_add_tensor(lp_gguf_writer *w,
                              const char *name,
                              lp_dtype dtype,
                              const uint64_t *dims,
                              uint32_t ndim,
                              const void *data){
    if (!w || w->failed || w->finalized || !name || !dims || !data || !ndim || ndim > LP_TENSOR_MAX_DIMS) {
        return -1;
    }
    int32_t ggml = lp_dtype_to_ggml(dtype);
    if (ggml < 0) return -1;

    bool fresh = false;
    lp_strmap_put(w->tensor_seen, name, strlen(name), &fresh);
    if (!fresh) return -1;

    size_t nel = 1u;
    for (uint32_t d = 0; d < ndim; d++) {
        if (!dims[d] || dims[d] > SIZE_MAX / nel) return -1;
        nel *= (size_t)dims[d];
    }
    size_t bytes = lp_tensor_nbytes(dtype, nel);
    if (!bytes) return -1;

    if (fwrite(data, 1, bytes, w->payload) != bytes) { w->failed = 1; return -1; }
    static const uint8_t zero[GGUF_ALIGN] = { 0 };
    size_t pad = align_up(bytes, GGUF_ALIGN) - bytes;
    if (pad && fwrite(zero, 1, pad, w->payload) != pad) { w->failed = 1; return -1; }

    if (w->ntensors == w->ctensors) {
        w->ctensors = w->ctensors ? w->ctensors * 2u : 64u;
        w->tensors = xrealloc(w->tensors, sizeof *w->tensors * w->ctensors);
    }
    TensorMeta *tm = &w->tensors[w->ntensors++];
    memset(tm, 0, sizeof *tm);
    tm->name = strdup(name);
    tm->ggml_type = (uint32_t)ggml;
    tm->ndim = ndim;
    for (uint32_t d = 0; d < ndim; d++) tm->dims[d] = dims[d];
    tm->bytes = bytes;
    return 0;
}

int lp_gguf_writer_finalize(lp_gguf_writer *w){
    if (!w || w->failed || w->finalized) return -1;

    lp_buf hdr = { 0 };
    lp_buf_put(&hdr, "GGUF", 4);
    put_u32(&hdr, 3u);
    put_u64(&hdr, (uint64_t)w->ntensors);
    put_u64(&hdr, (uint64_t)w->nkvs);

    for (size_t i = 0; i < w->nkvs; i++) {
        const KvEntry *e = &w->kvs[i];
        put_gstr(&hdr, e->key, strlen(e->key));
        switch (e->kind) {
        case KV_STR:
            put_u32(&hdr, LP_GGUF_TYPE_STRING);
            put_gstr(&hdr, e->str_val, strlen(e->str_val));
            break;
        case KV_U32:
            put_u32(&hdr, LP_GGUF_TYPE_UINT32);
            put_u32(&hdr, (uint32_t)e->u64_val);
            break;
        case KV_U64:
            put_u32(&hdr, LP_GGUF_TYPE_UINT64);
            put_u64(&hdr, e->u64_val);
            break;
        case KV_F32: {
            put_u32(&hdr, LP_GGUF_TYPE_FLOAT32);
            uint32_t bits; memcpy(&bits, &e->f32_val, 4);
            put_u32(&hdr, bits);
            break;
        }
        case KV_BOOL:
            put_u32(&hdr, LP_GGUF_TYPE_BOOL);
            lp_buf_byte(&hdr, e->u64_val ? 1u : 0u);
            break;
        case KV_STR_ARRAY:
            put_u32(&hdr, LP_GGUF_TYPE_ARRAY);
            put_u32(&hdr, LP_GGUF_TYPE_STRING);
            put_u64(&hdr, (uint64_t)e->count);
            if (e->blob.n) lp_buf_put(&hdr, e->blob.b, e->blob.n);
            break;
        case KV_U8_ARRAY:
            put_u32(&hdr, LP_GGUF_TYPE_ARRAY);
            put_u32(&hdr, LP_GGUF_TYPE_UINT8);
            put_u64(&hdr, (uint64_t)e->count);
            if (e->blob.n) lp_buf_put(&hdr, e->blob.b, e->blob.n);
            break;
        case KV_F32_ARRAY:
            put_u32(&hdr, LP_GGUF_TYPE_ARRAY);
            put_u32(&hdr, LP_GGUF_TYPE_FLOAT32);
            put_u64(&hdr, (uint64_t)e->count);
            if (e->blob.n) lp_buf_put(&hdr, e->blob.b, e->blob.n);
            break;
        case KV_I32_ARRAY:
            put_u32(&hdr, LP_GGUF_TYPE_ARRAY);
            put_u32(&hdr, LP_GGUF_TYPE_INT32);
            put_u64(&hdr, (uint64_t)e->count);
            if (e->blob.n) lp_buf_put(&hdr, e->blob.b, e->blob.n);
            break;
        }
    }

    uint64_t off = 0;
    for (size_t i = 0; i < w->ntensors; i++) {
        const TensorMeta *tm = &w->tensors[i];
        put_gstr(&hdr, tm->name, strlen(tm->name));
        put_u32(&hdr, tm->ndim);
        for (uint32_t d = 0; d < tm->ndim; d++) put_u64(&hdr, tm->dims[d]);
        put_u32(&hdr, tm->ggml_type);
        put_u64(&hdr, off);
        off += (uint64_t)align_up(tm->bytes, GGUF_ALIGN);
    }

    size_t pad = align_up(hdr.n, GGUF_ALIGN) - hdr.n;
    for (size_t i = 0; i < pad; i++) lp_buf_byte(&hdr, 0);

    size_t plen = strlen(w->output_path) + 16u;
    char *pending = malloc(plen);
    snprintf(pending, plen, "%s.partial", w->output_path);
    FILE *out = fopen(pending, "wb");
    if (!out) { lp_buf_free(&hdr); free(pending); return -1; }

    int ok = fwrite(hdr.b, 1, hdr.n, out) == hdr.n && fflush(w->payload) == 0;
    lp_buf_free(&hdr);
    rewind(w->payload);
    uint8_t blk[65536];
    while (ok) {
        size_t got = fread(blk, 1, sizeof blk, w->payload);
        if (got && fwrite(blk, 1, got, out) != got) { ok = 0; break; }
        if (got < sizeof blk) { if (ferror(w->payload)) ok = 0; break; }
    }
    if (fclose(out) != 0) ok = 0;
    if (!ok) {
        remove(pending);
        free(pending);
        w->failed = 1;
        return -1;
    }
    if (os_rename(pending, w->output_path) != 0) {
        remove(pending);
        free(pending);
        w->failed = 1;
        return -1;
    }
    free(pending);
    w->finalized = 1;
    return 0;
}

void lp_gguf_writer_free(lp_gguf_writer *w){
    if (!w) return;
    if (w->payload) fclose(w->payload);
    for (size_t i = 0; i < w->nkvs; i++) {
        free(w->kvs[i].key);
        free(w->kvs[i].str_val);
        lp_buf_free(&w->kvs[i].blob);
    }
    free(w->kvs);
    for (size_t i = 0; i < w->ntensors; i++) free(w->tensors[i].name);
    free(w->tensors);
    if (w->kv_seen) lp_strmap_free(w->kv_seen);
    if (w->tensor_seen) lp_strmap_free(w->tensor_seen);
    free(w->output_path);
    free(w);
}
