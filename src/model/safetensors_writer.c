#include "safetensors_writer.h"
#include "laplace/laplace.h"
#include "laplace/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;
    char *val;
} StMeta;

typedef struct {
    char *name;
    lp_dtype dtype;
    uint32_t ndim;
    uint64_t dims[LP_TENSOR_MAX_DIMS];
    uint64_t offset_start;
    uint64_t offset_end;
    void *data;
    size_t nbytes;
} StTensor;

struct lp_st_writer {
    char *path;
    StMeta *meta;
    size_t nmeta;
    size_t cap_meta;
    StTensor *tensors;
    size_t ntensors;
    size_t cap_tensors;
    uint64_t data_cursor;
};

lp_st_writer *lp_st_writer_create(const char *output_path){
    if (!output_path || !*output_path) return NULL;
    lp_st_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->path = strdup(output_path);
    if (!w->path) { free(w); return NULL; }
    return w;
}

void lp_st_writer_free(lp_st_writer *w){
    if (!w) return;
    free(w->path);
    for (size_t i = 0; i < w->nmeta; i++) {
        free(w->meta[i].key);
        free(w->meta[i].val);
    }
    free(w->meta);
    for (size_t i = 0; i < w->ntensors; i++) {
        free(w->tensors[i].name);
        free(w->tensors[i].data);
    }
    free(w->tensors);
    free(w);
}

int lp_st_writer_add_meta(lp_st_writer *w, const char *key, const char *value){
    if (!w || !key || !value) return -1;
    if (w->nmeta == w->cap_meta) {
        size_t nc = w->cap_meta ? w->cap_meta * 2u : 16u;
        StMeta *nm = realloc(w->meta, nc * sizeof *nm);
        if (!nm) return -1;
        w->meta = nm;
        w->cap_meta = nc;
    }
    w->meta[w->nmeta].key = strdup(key);
    w->meta[w->nmeta].val = strdup(value);
    if (!w->meta[w->nmeta].key || !w->meta[w->nmeta].val) return -1;
    w->nmeta++;
    return 0;
}

static size_t st_dtype_size(lp_dtype dtype, const char **name_out){
    switch (dtype) {
    case LP_DTYPE_F32: *name_out = "F32"; return 4u;
    case LP_DTYPE_F16: *name_out = "F16"; return 2u;
    default: return 0u;
    }
}

int lp_st_writer_add_tensor(lp_st_writer *w,
                            const char *name,
                            lp_dtype dtype,
                            const uint64_t *dims,
                            uint32_t ndim,
                            const void *data){
    if (!w || !name || !dims || ndim == 0 || ndim > LP_TENSOR_MAX_DIMS || !data) return -1;
    const char *dname = NULL;
    size_t esz = st_dtype_size(dtype, &dname);
    if (esz == 0) return -1;

    size_t nel = 1u;
    for (uint32_t i = 0; i < ndim; i++) {
        if (dims[i] == 0) return -1;
        nel *= (size_t)dims[i];
    }
    size_t nbytes = nel * esz;

    if (w->ntensors == w->cap_tensors) {
        size_t nc = w->cap_tensors ? w->cap_tensors * 2u : 32u;
        StTensor *nt = realloc(w->tensors, nc * sizeof *nt);
        if (!nt) return -1;
        w->tensors = nt;
        w->cap_tensors = nc;
    }

    void *copy = malloc(nbytes);
    if (!copy) return -1;
    memcpy(copy, data, nbytes);

    StTensor *t = &w->tensors[w->ntensors++];
    memset(t, 0, sizeof *t);
    t->name = strdup(name);
    t->dtype = dtype;
    t->ndim = ndim;
    for (uint32_t i = 0; i < ndim; i++) t->dims[i] = dims[i];
    t->offset_start = w->data_cursor;
    t->offset_end = w->data_cursor + (uint64_t)nbytes;
    t->data = copy;
    t->nbytes = nbytes;
    w->data_cursor = t->offset_end;
    return t->name ? 0 : -1;
}

int lp_st_writer_finalize(lp_st_writer *w){
    if (!w || !w->path) return -1;
    lp_buf hdr = { 0 };
    lp_buf_cstr(&hdr, "{");

    int first = 1;
    if (w->nmeta > 0) {
        first = 0;
        lp_buf_cstr(&hdr, "\"__metadata__\":{");
        for (size_t i = 0; i < w->nmeta; i++) {
            if (i > 0) lp_buf_cstr(&hdr, ",");
            const char *mk = w->meta[i].key ? w->meta[i].key : "";
            const char *mv = w->meta[i].val ? w->meta[i].val : "";
            lp_json_str(&hdr, mk, strlen(mk));
            lp_buf_cstr(&hdr, ":");
            lp_json_str(&hdr, mv, strlen(mv));
        }
        lp_buf_cstr(&hdr, "}");
    }

    for (size_t i = 0; i < w->ntensors; i++) {
        const StTensor *t = &w->tensors[i];
        const char *dname = NULL;
        st_dtype_size(t->dtype, &dname);
        if (!first) lp_buf_cstr(&hdr, ",");
        first = 0;

        const char *tn = t->name ? t->name : "";
        lp_json_str(&hdr, tn, strlen(tn));
        lp_buf_cstr(&hdr, ":{\"dtype\":\"");
        lp_buf_cstr(&hdr, dname);
        lp_buf_cstr(&hdr, "\",\"shape\":[");
        for (uint32_t d = 0; d < t->ndim; d++) {
            if (d) lp_buf_cstr(&hdr, ",");
            lp_buf_u64(&hdr, t->dims[d]);
        }
        lp_buf_cstr(&hdr, "],\"data_offsets\":[");
        lp_buf_u64(&hdr, t->offset_start);
        lp_buf_cstr(&hdr, ",");
        lp_buf_u64(&hdr, t->offset_end);
        lp_buf_cstr(&hdr, "]}");
    }

    lp_buf_cstr(&hdr, "}");

    /* Pad JSON header with spaces so (8 + hdr.n) aligns cleanly to 8 bytes */
    while ((hdr.n % 8u) != 0) lp_buf_byte(&hdr, ' ');

    FILE *f = fopen(w->path, "wb");
    if (!f) { lp_buf_free(&hdr); return -1; }

    uint64_t le_len = (uint64_t)hdr.n;
    uint8_t hdr8[8];
    for (int k = 0; k < 8; k++) hdr8[k] = (uint8_t)((le_len >> (k * 8)) & 0xFFu);

    int err = 0;
    if (fwrite(hdr8, 1u, 8u, f) != 8u) err = 1;
    if (!err && fwrite(hdr.b, 1u, hdr.n, f) != hdr.n) err = 1;
    for (size_t i = 0; i < w->ntensors && !err; i++) {
        if (w->tensors[i].nbytes > 0 &&
            fwrite(w->tensors[i].data, 1u, w->tensors[i].nbytes, f) != w->tensors[i].nbytes) {
            err = 1;
        }
    }
    if (fclose(f) != 0) err = 1;
    lp_buf_free(&hdr);
    return err ? -1 : 0;
}
