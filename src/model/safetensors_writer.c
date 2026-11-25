#include "safetensors_writer.h"
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

static int buf_grow(char **buf, size_t *cap, size_t need){
    if (need <= *cap) return 0;
    size_t nc = *cap ? *cap : 4096u;
    while (nc < need) nc *= 2u;
    char *nb = realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

static int buf_puts(char **buf, size_t *len, size_t *cap, const char *s, size_t n){
    if (buf_grow(buf, cap, *len + n + 1u) != 0) return -1;
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
    return 0;
}

static int buf_cstr(char **buf, size_t *len, size_t *cap, const char *s){
    return buf_puts(buf, len, cap, s, strlen(s));
}

static int buf_json_str(char **buf, size_t *len, size_t *cap, const char *s){
    if (buf_cstr(buf, len, cap, "\"") != 0) return -1;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            char esc[2] = { '\\', (char)c };
            if (buf_puts(buf, len, cap, esc, 2) != 0) return -1;
        } else if (c == '\b') {
            if (buf_cstr(buf, len, cap, "\\b") != 0) return -1;
        } else if (c == '\f') {
            if (buf_cstr(buf, len, cap, "\\f") != 0) return -1;
        } else if (c == '\n') {
            if (buf_cstr(buf, len, cap, "\\n") != 0) return -1;
        } else if (c == '\r') {
            if (buf_cstr(buf, len, cap, "\\r") != 0) return -1;
        } else if (c == '\t') {
            if (buf_cstr(buf, len, cap, "\\t") != 0) return -1;
        } else if (c < 0x20u) {
            char u[7];
            snprintf(u, sizeof u, "\\u%04x", (unsigned)c);
            if (buf_cstr(buf, len, cap, u) != 0) return -1;
        } else {
            char ch = (char)c;
            if (buf_puts(buf, len, cap, &ch, 1) != 0) return -1;
        }
    }
    return buf_cstr(buf, len, cap, "\"");
}

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
    char *hdr = NULL;
    size_t hlen = 0, hcap = 0;
    if (buf_cstr(&hdr, &hlen, &hcap, "{") != 0) { free(hdr); return -1; }

    int first = 1;
    if (w->nmeta > 0) {
        first = 0;
        if (buf_cstr(&hdr, &hlen, &hcap, "\"__metadata__\":{") != 0) { free(hdr); return -1; }
        for (size_t i = 0; i < w->nmeta; i++) {
            if (i > 0 && buf_cstr(&hdr, &hlen, &hcap, ",") != 0) { free(hdr); return -1; }
            if (buf_json_str(&hdr, &hlen, &hcap, w->meta[i].key) != 0 ||
                buf_cstr(&hdr, &hlen, &hcap, ":") != 0 ||
                buf_json_str(&hdr, &hlen, &hcap, w->meta[i].val) != 0) {
                free(hdr);
                return -1;
            }
        }
        if (buf_cstr(&hdr, &hlen, &hcap, "}") != 0) { free(hdr); return -1; }
    }

    for (size_t i = 0; i < w->ntensors; i++) {
        const StTensor *t = &w->tensors[i];
        const char *dname = NULL;
        st_dtype_size(t->dtype, &dname);
        if (!first && buf_cstr(&hdr, &hlen, &hcap, ",") != 0) { free(hdr); return -1; }
        first = 0;

        char num[128];
        if (buf_json_str(&hdr, &hlen, &hcap, t->name) != 0 ||
            buf_cstr(&hdr, &hlen, &hcap, ":{\"dtype\":\"") != 0 ||
            buf_cstr(&hdr, &hlen, &hcap, dname) != 0 ||
            buf_cstr(&hdr, &hlen, &hcap, "\",\"shape\":[") != 0) {
            free(hdr);
            return -1;
        }
        for (uint32_t d = 0; d < t->ndim; d++) {
            snprintf(num, sizeof num, "%s%llu", d ? "," : "", (unsigned long long)t->dims[d]);
            if (buf_cstr(&hdr, &hlen, &hcap, num) != 0) { free(hdr); return -1; }
        }
        snprintf(num, sizeof num, "],\"data_offsets\":[%llu,%llu]}",
                 (unsigned long long)t->offset_start,
                 (unsigned long long)t->offset_end);
        if (buf_cstr(&hdr, &hlen, &hcap, num) != 0) { free(hdr); return -1; }
    }

    if (buf_cstr(&hdr, &hlen, &hcap, "}") != 0) { free(hdr); return -1; }

    /* Pad JSON header with spaces so (8 + hlen) aligns cleanly to 8 bytes */
    while ((hlen % 8u) != 0) {
        if (buf_puts(&hdr, &hlen, &hcap, " ", 1u) != 0) { free(hdr); return -1; }
    }

    FILE *f = fopen(w->path, "wb");
    if (!f) { free(hdr); return -1; }

    uint64_t le_len = (uint64_t)hlen;
    uint8_t hdr8[8];
    for (int k = 0; k < 8; k++) hdr8[k] = (uint8_t)((le_len >> (k * 8)) & 0xFFu);

    int err = 0;
    if (fwrite(hdr8, 1u, 8u, f) != 8u) err = 1;
    if (!err && fwrite(hdr, 1u, hlen, f) != hlen) err = 1;
    for (size_t i = 0; i < w->ntensors && !err; i++) {
        if (w->tensors[i].nbytes > 0 &&
            fwrite(w->tensors[i].data, 1u, w->tensors[i].nbytes, f) != w->tensors[i].nbytes) {
            err = 1;
        }
    }
    if (fclose(f) != 0) err = 1;
    free(hdr);
    return err ? -1 : 0;
}
