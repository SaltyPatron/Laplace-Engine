/* laplace model: decompose a transformer checkpoint (Safetensors single/sharded OR GGUF quantized/float)
 * into significant two-sided entity-pair survivors (|z| >= zmin: WIN for z > 0, LOSS for z < 0) and optionally
 * record them into PostgreSQL under AIModelProbe (0.50) with zero raw weight bytes retained, or export a clean
 * laplace-blake3-v1 GGUF via CPU Mold-a-Model (--export out.gguf). */

#include "engine.h"
#include "codecs.h"
#include "laplace/json.h"
#include "laplace/os.h"
#include "laplace/tensor.h"
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mkl.h>

static char *slurp(const char *path, size_t *len){
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    int64_t sz = os_fsize(f);
    if (sz < 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)sz + 1u);
    if (!b || (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz)) { fclose(f); free(b); return NULL; }
    fclose(f);
    b[sz] = 0;
    *len = (size_t)sz;
    return b;
}

typedef struct {
    char            path[4096];
    const uint8_t  *base;
    size_t          size;
    lp_safetensors *st;
} STShard;

typedef struct {
    char      dir[4096];
    int       is_gguf;
    lp_gguf  *gguf;
    const uint8_t *gguf_base;
    size_t    gguf_size;
    STShard   shard[64];
    int       nshard;
    char     *idx_buf;
    lp_json  *idx;
    uint32_t  wmap;
} Ckpt;

typedef struct {
    lp_dtype       dtype;
    size_t         shape[LP_TENSOR_MAX_DIMS];
    uint32_t       nd;
    const uint8_t *data;
    size_t         n_elements;
} Tensor;

static double jnum(const lp_json *j, uint32_t node, const char *key, double dflt){
    double v; return lp_json_number(j, lp_json_get(j, node, key), &v) ? v : dflt;
}
static double jat(const lp_json *j, uint32_t array, uint32_t i){
    double v = 0; lp_json_number(j, lp_json_item(j, array, i), &v); return v;
}

static int find_gguf_in_dir(const char *dir, char *out, size_t cap){
    size_t dl = strlen(dir);
    if (dl > 5 && !strcasecmp(dir + dl - 5, ".gguf") && os_exists(dir)) {
        snprintf(out, cap, "%s", dir);
        return 1;
    }
    os_list ls = { 0 };
    if (os_list_dir(dir, &ls) != 0) return 0;
    int found = 0;
    for (size_t i = 0; i < ls.n; i++) {
        size_t l = strlen(ls.item[i]);
        if (l > 5 && !strcasecmp(ls.item[i] + l - 5, ".gguf")) {
            snprintf(out, cap, "%s/%s", dir, ls.item[i]);
            found = 1;
            break;
        }
    }
    os_list_free(&ls);
    return found;
}

static STShard *st_open_shard(Ckpt *c, const char *rel){
    char full[4352];
    if (rel[0] == '/' || (rel[0] && rel[1] == ':')) snprintf(full, sizeof full, "%s", rel);
    else snprintf(full, sizeof full, "%s/%s", c->dir, rel);
    for (int i = 0; i < c->nshard; i++) if (!strcmp(c->shard[i].path, full)) return &c->shard[i];
    if (c->nshard >= 64) { fprintf(stderr, "too many safetensors shards\n"); exit(1); }
    STShard *sh = &c->shard[c->nshard++];
    snprintf(sh->path, sizeof sh->path, "%s", full);
    sh->base = lp_map_file(full, 0, &sh->size);
    sh->st = sh->base ? lp_safetensors_open(sh->base, sh->size) : NULL;
    if (!sh->st) { fprintf(stderr, "%s: failed to open safetensors\n", full); exit(1); }
    return sh;
}

static Ckpt ckpt_open(const char *path){
    Ckpt c = { 0 };
    snprintf(c.dir, sizeof c.dir, "%s", path);
    c.wmap = LP_JSON_NONE;
    char gpath[4352];
    if (find_gguf_in_dir(path, gpath, sizeof gpath)) {
        c.is_gguf = 1;
        c.gguf_base = lp_map_file(gpath, 0, &c.gguf_size);
        c.gguf = c.gguf_base ? lp_gguf_open(c.gguf_base, c.gguf_size) : NULL;
        if (!c.gguf) { fprintf(stderr, "%s: failed to open GGUF checkpoint\n", gpath); exit(1); }
        return c;
    }
    char idxp[4352]; snprintf(idxp, sizeof idxp, "%s/model.safetensors.index.json", path);
    size_t len = 0; c.idx_buf = slurp(idxp, &len);
    if (c.idx_buf) {
        if (!(c.idx = lp_json_parse(c.idx_buf, len))) { fprintf(stderr, "%s: malformed\n", idxp); exit(1); }
        c.wmap = lp_json_get(c.idx, LP_JSON_ROOT, "weight_map");
    } else {
        st_open_shard(&c, "model.safetensors");
    }
    return c;
}

static void ckpt_close(Ckpt *c){
    if (!c) return;
    if (c->is_gguf) {
        if (c->gguf) lp_gguf_close(c->gguf);
        if (c->gguf_base) lp_unmap_file(c->gguf_base, c->gguf_size);
        c->gguf = NULL; c->gguf_base = NULL; c->gguf_size = 0;
        return;
    }
    for (int i = 0; i < c->nshard; i++) {
        if (c->shard[i].st) lp_safetensors_close(c->shard[i].st);
        if (c->shard[i].base) lp_unmap_file(c->shard[i].base, c->shard[i].size);
    }
    if (c->idx) lp_json_free(c->idx);
    free(c->idx_buf);
    c->idx = NULL; c->idx_buf = NULL; c->nshard = 0;
}

static int ckpt_find_tensor(Ckpt *c, const char *hf_name, Tensor *out){
    if (c->is_gguf) {
        char ggml[256];
        int64_t ti = lp_gguf_find_tensor(c->gguf, hf_name);
        if (ti < 0 && lp_gguf_hf_to_ggml_name(hf_name, ggml, sizeof ggml) > 0) {
            ti = lp_gguf_find_tensor(c->gguf, ggml);
        }
        if (ti < 0) return 0;
        memset(out, 0, sizeof *out);
        out->dtype = lp_gguf_tensor_dtype(c->gguf, (size_t)ti);
        uint64_t dims[LP_TENSOR_MAX_DIMS];
        out->nd = lp_gguf_tensor_dims(c->gguf, (size_t)ti, dims, LP_TENSOR_MAX_DIMS);
        /* GGUF stores innermost dim in dims[0] and rows in dims[1]; match row-major (rows, cols). */
        if (out->nd == 2) {
            out->shape[0] = (size_t)dims[1];
            out->shape[1] = (size_t)dims[0];
        } else {
            for (uint32_t i = 0; i < out->nd; i++) out->shape[i] = (size_t)dims[i];
        }
        size_t nel = 1u;
        for (uint32_t i = 0; i < out->nd; i++) nel *= out->shape[i];
        out->n_elements = nel;
        uint64_t off = lp_gguf_data_offset(c->gguf) + lp_gguf_tensor_offset(c->gguf, (size_t)ti);
        out->data = c->gguf_base + off;
        return 1;
    }

    STShard *sh = NULL;
    int64_t si = -1;
    if (c->wmap != LP_JSON_NONE) {
        const char *file = lp_json_string(c->idx, lp_json_get(c->idx, c->wmap, hf_name), NULL);
        if (!file) return 0;
        sh = st_open_shard(c, file);
        si = lp_safetensors_find(sh->st, hf_name);
    } else {
        sh = &c->shard[0];
        si = lp_safetensors_find(sh->st, hf_name);
    }
    if (!sh || si < 0) return 0;
    memset(out, 0, sizeof *out);
    out->dtype = lp_safetensors_dtype(sh->st, (size_t)si);
    uint64_t dims[LP_TENSOR_MAX_DIMS];
    out->nd = lp_safetensors_dims(sh->st, (size_t)si, dims, LP_TENSOR_MAX_DIMS);
    size_t nel = 1u;
    for (uint32_t i = 0; i < out->nd; i++) {
        out->shape[i] = (size_t)dims[i];
        nel *= out->shape[i];
    }
    out->n_elements = nel;
    uint64_t s0 = 0, s1 = 0;
    lp_safetensors_span(sh->st, (size_t)si, &s0, &s1);
    out->data = sh->base + lp_safetensors_data_offset(sh->st) + s0;
    return 1;
}

static Tensor ckpt_get(Ckpt *c, const char *hf_name){
    Tensor t;
    if (!ckpt_find_tensor(c, hf_name, &t)) {
        fprintf(stderr, "tensor %s missing\n", hf_name);
        exit(1);
    }
    return t;
}

static float *decode_f32(const Tensor *t){
    float *o = malloc(sizeof(float) * (t->n_elements ? t->n_elements : 1u));
    if (!o) { perror("malloc"); exit(1); }
    if (lp_tensor_decode_f32(t->data, t->n_elements, t->dtype, o) != 0) {
        fprintf(stderr, "tensor dtype %s decode failed\n", lp_dtype_name(t->dtype));
        exit(1);
    }
    return o;
}

/* ---- tokens: the model's vocabulary as text, and each token's content ID & Ref */
#define M_BLOCK 4096
static int any_rec(const Ref *r, size_t n){ for (size_t i = 0; i < n; i++) if (r[i].said == LP_SAID_RECORD || r[i].said == LP_SAID_HOLDS) return 1; return 0; }
static Ref m_block(Ref *r, size_t n){
    if (n == 1) return r[0];
    Ref x = compose(r, (uint32_t)n, ref_above(r, n)); x.said = any_rec(r, n) ? LP_SAID_HOLDS : 0; return x;
}
static Ref blocks_of(Ref *r, size_t n){
    if (n == 1) return r[0];
    if (n <= M_BLOCK) return m_block(r, n);
    Ref *up = malloc(sizeof(Ref) * n); size_t m = 0, from = 0;
    for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, r[i].id.b + 8, 8);
        if ((b & (M_BLOCK - 1)) == 0 || i == n - 1) { up[m++] = m_block(r + from, i - from + 1); from = i + 1; } }
    Ref out = m == n ? m_block(r, n) : blocks_of(up, m); free(up); return out;
}

typedef struct { char *text; int kind; lp_id id; Ref ref; } Tok;       /* kind: 0 text, 1 byte, 2 special */
static lp_text *TX;

static void token_id(const char *s, size_t sl, int do_ingest, lp_id *out, Ref *rout){
    if (do_ingest && sl > 0) { *rout = text_ref(CTX[0], (const uint8_t *)s, sl); *out = rout->id; }
    else { *out = lp_text_decompose(TX, (const uint8_t *)s, sl, NULL, NULL).id; rout->id = *out; }
}

static char *decode_vocab_piece(const char *s, size_t L){
    char *o = malloc(L + 1u), *q = o;
    for (size_t k = 0; k < L; ) {
        if (k + 2 < L && (unsigned char)s[k] == 0xE2 && (unsigned char)s[k + 1] == 0x96 && (unsigned char)s[k + 2] == 0x81) { *q++ = ' '; k += 3; }
        else if (k + 1 < L && (unsigned char)s[k] == 0xC4 && (unsigned char)s[k + 1] == 0xA0) { *q++ = ' '; k += 2; }
        else if (k + 1 < L && (unsigned char)s[k] == 0xC4 && (unsigned char)s[k + 1] == 0x8A) { *q++ = '\n'; k += 2; }
        else *q++ = s[k++];
    }
    *q = 0;
    return o;
}

static Tok *load_vocab(const Ckpt *c, const char *dir, int do_ingest, size_t *nv, Ref *vocab_trunk){
    Tok *t = NULL;
    if (c->is_gguf) {
        int64_t ti = lp_gguf_find_kv(c->gguf, "tokenizer.ggml.tokens");
        if (ti < 0) { fprintf(stderr, "GGUF missing tokenizer.ggml.tokens\n"); exit(1); }
        size_t nvocab = lp_gguf_kv_array_len(c->gguf, (size_t)ti);
        *nv = nvocab;
        t = calloc(nvocab ? nvocab : 1u, sizeof(Tok));
        for (size_t i = 0; i < nvocab; i++) {
            size_t L = 0;
            const char *s = lp_gguf_kv_array_str(c->gguf, (size_t)ti, i, &L);
            if (!s) { t[i].kind = 2; continue; }
            t[i].text = decode_vocab_piece(s, L);
            if (L == 6 && s[0] == '<' && s[1] == '0' && s[2] == 'x' && s[5] == '>') t[i].kind = 1;
            else if (L > 2 && ((s[0] == '<' && s[L - 1] == '>') || (s[0] == '[' && s[L - 1] == ']'))) t[i].kind = 2;
            else if (!t[i].text[0]) t[i].kind = 2;
            else token_id(t[i].text, strlen(t[i].text), do_ingest, &t[i].id, &t[i].ref);
        }
    } else {
        char p[4096]; snprintf(p, sizeof p, "%s/tokenizer.json", dir); size_t len; char *b = slurp(p, &len);
        if (!b) { perror(p); exit(1); }
        lp_json *d = lp_json_parse(b, len);
        if (!d) { fprintf(stderr, "%s: malformed\n", p); exit(1); }
        uint32_t vocab = lp_json_get(d, lp_json_get(d, LP_JSON_ROOT, "model"), "vocab"), nvocab = lp_json_count(d, vocab);
        size_t maxid = 0;
        for (uint32_t i = 0; i < nvocab; i++) { size_t id = (size_t)jat(d, vocab, i); if (id > maxid) maxid = id; }
        t = calloc(maxid + 1, sizeof(Tok)); *nv = maxid + 1;
        for (uint32_t i = 0; i < nvocab; i++) {
            const char *s = lp_json_key(d, vocab, i); size_t id = (size_t)jat(d, vocab, i);
            size_t L = strlen(s);
            t[id].text = decode_vocab_piece(s, L);
            if (L == 6 && s[0] == '<' && s[1] == '0' && s[2] == 'x' && s[5] == '>') t[id].kind = 1;
            else if (L > 2 && ((s[0] == '<' && s[L - 1] == '>') || (s[0] == '[' && s[L - 1] == ']'))) t[id].kind = 2;
            else if (!t[id].text[0]) t[id].kind = 2;
            else token_id(t[id].text, strlen(t[id].text), do_ingest, &t[id].id, &t[id].ref);
        }
        lp_json_free(d); free(b);
    }

    if (do_ingest && vocab_trunk) {
        Ref *vr = malloc(sizeof(Ref) * (*nv ? *nv : 1)); size_t nvr = 0;
        for (size_t i = 0; i < *nv; i++) if (t[i].text && t[i].kind == 0) { vr[nvr] = t[i].ref; vr[nvr++].said = 0; }
        *vocab_trunk = nvr ? blocks_of(vr, nvr) : (Ref){ 0 };
        free(vr);
    }
    return t;
}

/* ---- linear algebra helpers */
static void rmsnorm_rows(const float *x, float *y, size_t m, size_t d, const float *g, double eps){
    #pragma omp parallel for
    for (size_t i = 0; i < m; i++) {
        const float *r = x + i * d; float *o = y + i * d; double s = 0;
        for (size_t k = 0; k < d; k++) s += (double)r[k] * r[k];
        float inv = (float)(1.0 / sqrt(s / d + eps));
        for (size_t k = 0; k < d; k++) o[k] = r[k] * inv * (g ? g[k] : 1.0f);
    }
}
static void unit_rows(float *x, size_t m, size_t d){
    #pragma omp parallel for
    for (size_t i = 0; i < m; i++) { double s = 0; for (size_t k = 0; k < d; k++) s += (double)x[i * d + k] * x[i * d + k];
        float inv = s > 0 ? (float)(1.0 / sqrt(s)) : 0; for (size_t k = 0; k < d; k++) x[i * d + k] *= inv; }
}
static void mm_abt(const float *A, const float *B, float *C, size_t m, size_t n, size_t k){
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)m, (MKL_INT)n, (MKL_INT)k, 1.0f, A, (MKL_INT)k, B, (MKL_INT)k, 0.0f, C, (MKL_INT)n);
}
static void cols(const float *src, size_t m, size_t ld, size_t c0, size_t w, float *dst){
    #pragma omp parallel for
    for (size_t i = 0; i < m; i++) memcpy(dst + i * w, src + i * ld + c0, w * 4);
}

/* ---- two-sided |z| >= zmin reporting & ingestion (Sequence 24) */
static Tok *V; static size_t NV; static double FLOPS_TOTAL, SECS_TOTAL; static uint64_t KEPT_TOTAL, ROWS_TOTAL;
static int sample_rows[16]; static int nsample;

static void report(const char *name, const lp_rowsig_stats *s, double secs, const lp_rowsig_hit *h, size_t nh, int rows_are_tokens){
    FLOPS_TOTAL += s->flops; SECS_TOTAL += secs; KEPT_TOTAL += nh; ROWS_TOTAL += s->rows;
    printf("  %-12s %7.2f s %7.1f GFLOP/s   rows %'7llu   per row above z3 %7.2f  z4 %6.2f  z5 %6.2f   rows with none %5.1f%%\n", name, secs,
           secs > 0 ? s->flops / secs / 1e9 : 0.0, (unsigned long long)s->rows, (double)s->above[0] / s->rows, (double)s->above[1] / s->rows,
           (double)s->above[2] / s->rows, 100.0 * s->rows_without / s->rows);
    if (!rows_are_tokens) return;
    for (int q = 0; q < nsample; q++) {
        int row = sample_rows[q]; printf("      %-10s ->", V[row].text);
        int shown = 0;
        for (size_t i = 0; i < nh && shown < 10; i++) if ((int)h[i].row == row && h[i].z >= 4) { printf(" %s(%.1f)", V[h[i].col].text, h[i].z); shown++; }
        putchar('\n');
    }
}

static Ref record_scalar_fact(File *mf, Ref subj, const char *key, size_t val){
    char vb[32]; int vl = snprintf(vb, sizeof vb, "%zu", val);
    Ref kr = text_ref(CTX[0], (const uint8_t *)key, strlen(key));
    Ref vr = text_ref(CTX[0], (const uint8_t *)vb, (size_t)vl);
    Ref p3[3] = { subj, kr, vr }; p3[0].said = p3[1].said = p3[2].said = 0;
    Ref cl = said_claim(compose(p3, 3, ref_above(p3, 3)));
    cl.outcome = (uint8_t)lp_outcome_of(1.0, &cl.spare);
    Event ev = { .claim = cl.id, .witnessed = cl.id, .score = 1.0f, .enter_rating = 1500.0f, .kind = EV_CLAIM };
    ev_push(&mf->ev, &ev);
    return cl;
}

typedef struct {
    int   *rows;
    int   *cols;
    double *weights;
    size_t n, cap;
} ExportEdges;

static void push_export_edge(ExportEdges *ee, int r, int c, double w){
    if (!ee || r == c || w <= 0.0) return;
    if (ee->n == ee->cap) {
        ee->cap = ee->cap ? ee->cap * 2u : 1024u;
        ee->rows = xrealloc(ee->rows, sizeof *ee->rows * ee->cap);
        ee->cols = xrealloc(ee->cols, sizeof *ee->cols * ee->cap);
        ee->weights = xrealloc(ee->weights, sizeof *ee->weights * ee->cap);
    }
    ee->rows[ee->n] = r;
    ee->cols[ee->n] = c;
    ee->weights[ee->n] = w;
    ee->n++;
}

static Ref record_circuit_polarity(File *mf, const char *cname,
                                   const lp_rowsig_hit *pos, size_t npos,
                                   const lp_rowsig_hit *neg, size_t nneg,
                                   float zmin, ExportEdges *ee){
    Ref rel = text_ref(CTX[0], (const uint8_t *)cname, strlen(cname));
    size_t total = npos + nneg;
    Ref *claims = malloc(sizeof(Ref) * (total ? total : 1u)); size_t nc = 0;
    for (int side = 0; side < 2; side++) {
        const lp_rowsig_hit *h = side == 0 ? pos : neg;
        size_t nh = side == 0 ? npos : nneg;
        for (size_t i = 0; i < nh; i++) {
            uint32_t r = h[i].row, c = h[i].col;
            if (r >= NV || c >= NV || r == c || !V[r].text || !V[c].text || V[r].kind != 0 || V[c].kind != 0) continue;
            if (lp_id_eq(&V[r].ref.id, &V[c].ref.id)) continue;
            double mag = 0.5 * erf((double)(h[i].z - zmin) / sqrt(2.0));
            double sc = side == 0 ? (0.5 + mag) : (0.5 - mag);
            if (side == 0) {
                if (sc < 0.51) sc = 0.51;
                if (sc > 0.999) sc = 0.999;
                if (ee) push_export_edge(ee, (int)r, (int)c, sc);
            } else {
                if (sc > 0.49) sc = 0.49;
                if (sc < 0.001) sc = 0.001;
            }
            if (!mf) continue;
            Ref p3[3] = { V[r].ref, rel, V[c].ref }; p3[0].said = p3[1].said = p3[2].said = 0;
            Ref cl = said_claim(compose(p3, 3, ref_above(p3, 3)));
            cl.outcome = (uint8_t)lp_outcome_of(sc, &cl.spare);
            Event ev = { .claim = cl.id, .witnessed = cl.id, .score = (float)sc, .enter_rating = 1500.0f, .kind = EV_CLAIM };
            ev_push(&mf->ev, &ev);
            claims[nc++] = cl;
        }
    }
    Ref out = (mf && nc) ? said_record(blocks_of(claims, nc)) : rel;
    free(claims);
    return out;
}

static size_t rowsig_two_sided(const float *A, size_t m, const float *B, size_t n, size_t r,
                               float scale, float zmin, uint32_t cap,
                               lp_rowsig_hit *pos, size_t hcap, lp_rowsig_stats *pst,
                               lp_rowsig_hit *neg, size_t *nneg_out){
    size_t npos = lp_rowsig(A, m, B, n, r, scale, zmin, cap, pos, hcap, pst);
    *nneg_out = neg ? lp_rowsig(A, m, B, n, r, -scale, zmin, cap, neg, hcap, NULL) : 0;
    return npos;
}

static void ffn_connected(const float *X, const float *Wg, const float *Wu, const float *Wd, float *Y, size_t NT, size_t D, size_t FF){
    size_t tile = 2048; if (tile > NT) tile = NT;
    float *G = malloc(sizeof(float) * tile * FF), *U = malloc(sizeof(float) * tile * FF);
    for (size_t r0 = 0; r0 < NT; r0 += tile) {
        size_t b = r0 + tile <= NT ? tile : NT - r0;
        const float *Xb = X + r0 * D; float *Yb = Y + r0 * D;
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)b, (MKL_INT)FF, (MKL_INT)D, 1.0f, Xb, (MKL_INT)D, Wg, (MKL_INT)D, 0.0f, G, (MKL_INT)FF);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)b, (MKL_INT)FF, (MKL_INT)D, 1.0f, Xb, (MKL_INT)D, Wu, (MKL_INT)D, 0.0f, U, (MKL_INT)FF);
        size_t n = b * FF;
        #pragma omp parallel for
        for (size_t i = 0; i < n; i++) { float g = G[i]; G[i] = (g / (1.0f + expf(-g))) * U[i]; }
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)b, (MKL_INT)D, (MKL_INT)FF, 1.0f, G, (MKL_INT)FF, Wd, (MKL_INT)FF, 0.0f, Yb, (MKL_INT)D);
    }
    free(G); free(U);
}

static size_t gguf_arch_u64(const lp_gguf *g, const char *arch, const char *suffix, uint64_t dflt){
    char k[128]; uint64_t v = 0;
    snprintf(k, sizeof k, "%s.%s", arch, suffix);
    int64_t idx = lp_gguf_find_kv(g, k);
    if (idx >= 0 && lp_gguf_kv_u64(g, (size_t)idx, &v)) return (size_t)v;
    return (size_t)dflt;
}

static double gguf_arch_f64(const lp_gguf *g, const char *arch, const char *suffix, double dflt){
    char k[128]; double v = 0;
    snprintf(k, sizeof k, "%s.%s", arch, suffix);
    int64_t idx = lp_gguf_find_kv(g, k);
    if (idx >= 0 && lp_gguf_kv_f64(g, (size_t)idx, &v)) return v;
    return dflt;
}

int cmd_model(int argc, char **argv){
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    if (argc < 2) {
        fprintf(stderr, "usage: laplace model model_dir_or_gguf [-d conninfo] [--ingest] [--export out.gguf] [--witness name] [--layers N] [--z zmin] [--cap K] [--sample word ...]\n");
        return 2;
    }
    const char *dir = argv[1], *conninfo = laplace_db(), *wname = NULL, *export_path = NULL;
    int layers = -1, do_ingest = 0; float zmin = 0.0f; uint32_t cap = 64; const char *samples[16]; int nsamp = 0;
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "--ingest")) do_ingest = 1;
        else if (!strcmp(argv[a], "--export") && a + 1 < argc) export_path = argv[++a];
        else if (!strcmp(argv[a], "--witness") && a + 1 < argc) wname = argv[++a];
        else if (!strcmp(argv[a], "--layers") && a + 1 < argc) layers = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--z") && a + 1 < argc) zmin = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "--cap") && a + 1 < argc) cap = (uint32_t)atoi(argv[++a]);
        else if (!strcmp(argv[a], "--sample")) while (a + 1 < argc && argv[a + 1][0] != '-' && nsamp < 16) samples[nsamp++] = argv[++a];
    }
    if (!nsamp) { static const char *d[] = { " king", " dog", " Paris", " happy", " run" }; for (int i = 0; i < 5; i++) samples[nsamp++] = d[i]; }
    V = NULL; NV = 0; nsample = 0;
    FLOPS_TOTAL = 0; SECS_TOTAL = 0; KEPT_TOTAL = 0; ROWS_TOTAL = 0;

    double T = now(), t = now();
    if (!lp_mkl_reproducible()) { fprintf(stderr, "laplace model: MKL refused reproducible mode (CNR AVX2, strict)\n"); return 1; }
    tier0_open(NULL); TX = lp_text_new(T0);
    if (do_ingest) { table_init(); ctx_open(1); }
    printf("laplace model %s\ncpu: %s; MKL threads %d\n", dir, lp_cpu_describe(lp_cpu_features()), mkl_get_max_threads());

    Ckpt ckpt = ckpt_open(dir);
    size_t D = 0, NH = 0, NKV = 0, NL = 0, FF = 0; double eps = 1e-5;
    if (ckpt.is_gguf) {
        int64_t ai = lp_gguf_find_kv(ckpt.gguf, "general.architecture");
        const char *arch = ai >= 0 ? lp_gguf_kv_str(ckpt.gguf, (size_t)ai, NULL) : NULL;
        char abuf[64] = "llama";
        if (arch) { size_t al = 0; const char *as = lp_gguf_kv_str(ckpt.gguf, (size_t)ai, &al); snprintf(abuf, sizeof abuf, "%.*s", (int)al, as); }
        D = gguf_arch_u64(ckpt.gguf, abuf, "embedding_length", 0);
        NH = gguf_arch_u64(ckpt.gguf, abuf, "attention.head_count", 0);
        NKV = gguf_arch_u64(ckpt.gguf, abuf, "attention.head_count_kv", NH);
        NL = gguf_arch_u64(ckpt.gguf, abuf, "block_count", 0);
        FF = gguf_arch_u64(ckpt.gguf, abuf, "feed_forward_length", 0);
        eps = gguf_arch_f64(ckpt.gguf, abuf, "attention.layer_norm_rms_epsilon", 1e-5);
    } else {
        char p[4096]; size_t len; snprintf(p, sizeof p, "%s/config.json", dir); char *cb = slurp(p, &len);
        if (!cb) { perror(p); ckpt_close(&ckpt); lp_text_free(TX); TX = NULL; return 1; }
        lp_json *cfg = lp_json_parse(cb, len);
        if (!cfg) { fprintf(stderr, "%s: malformed\n", p); free(cb); ckpt_close(&ckpt); lp_text_free(TX); TX = NULL; return 1; }
        D = (size_t)jnum(cfg, LP_JSON_ROOT, "hidden_size", 0);
        NH = (size_t)jnum(cfg, LP_JSON_ROOT, "num_attention_heads", 0);
        NKV = (size_t)jnum(cfg, LP_JSON_ROOT, "num_key_value_heads", (double)NH);
        NL = (size_t)jnum(cfg, LP_JSON_ROOT, "num_hidden_layers", 0);
        FF = (size_t)jnum(cfg, LP_JSON_ROOT, "intermediate_size", 0);
        eps = jnum(cfg, LP_JSON_ROOT, "rms_norm_eps", 1e-5);
        lp_json_free(cfg);
        free(cb);
    }
    if (!NH || !D) { fprintf(stderr, "%s: missing model geometry\n", dir); ckpt_close(&ckpt); lp_text_free(TX); TX = NULL; return 1; }
    size_t HD = D / NH; if (layers < 0 || (size_t)layers > NL) layers = (int)NL;

    Ref vocab_trunk = { 0 };
    V = load_vocab(&ckpt, dir, do_ingest, &NV, &vocab_trunk);
    size_t ntext = 0, nbyte = 0, nspec = 0; for (size_t i = 0; i < NV; i++) { if (!V[i].text) continue; if (V[i].kind == 0) ntext++; else if (V[i].kind == 1) nbyte++; else nspec++; }
    for (int q = 0; q < nsamp; q++) for (size_t i = 0; i < NV; i++) if (V[i].text && !strcmp(V[i].text, samples[q])) { sample_rows[nsample++] = (int)i; break; }

    Tensor te = ckpt_get(&ckpt, "model.embed_tokens.weight");
    Tensor tu_tmp;
    int has_lm_head = ckpt_find_tensor(&ckpt, "lm_head.weight", &tu_tmp);
    Tensor tu = has_lm_head ? tu_tmp : te;
    Tensor tn = ckpt_get(&ckpt, "model.norm.weight");
    float *E = decode_f32(&te), *U = decode_f32(&tu), *gf = decode_f32(&tn); size_t NT = te.shape[0];
    if (zmin <= 0.0f) zmin = do_ingest ? (float)sqrt(2.0 * log((double)(NT > 2 ? NT : 2))) : 3.0f;
    printf("config: hidden %zu, heads %zu (kv %zu, dim %zu), layers %zu (running %d), ffn %zu; vocabulary %zu: text %zu, byte %zu, special %zu; zmin %.3f\n",
           D, NH, NKV, HD, NL, layers, FF, NV, ntext, nbyte, nspec, zmin);
    printf("loaded embeddings and unembedding (%s, %s) in %.2f s\n\n", lp_dtype_name(te.dtype), has_lm_head ? "untied" : "tied", now() - t);

    File mf; memset(&mf, 0, sizeof mf);
    Ref *crecs = NULL; size_t ncrecs = 0, ccap = 0;
    if (!wname || !*wname) { const char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\'); if (bs && (!sl || bs > sl)) sl = bs; wname = (sl && sl[1]) ? sl + 1 : dir; }
    if (do_ingest) {
        const lp_trust_class *tc = lp_trust_class_named("AIModelProbe");
        mf.path = dir; mf.witness = text_ref(CTX[0], (const uint8_t *)wname, strlen(wname)); mf.trust = tc ? tc->prior : 0.50;
        ccap = (size_t)layers * (2 * NH + 2) + 16; crecs = malloc(sizeof(Ref) * ccap);
        if (vocab_trunk.tier) crecs[ncrecs++] = vocab_trunk;
        Ref cf[6];
        cf[0] = record_scalar_fact(&mf, mf.witness, "hidden_size", D);
        cf[1] = record_scalar_fact(&mf, mf.witness, "num_attention_heads", NH);
        cf[2] = record_scalar_fact(&mf, mf.witness, "num_key_value_heads", NKV);
        cf[3] = record_scalar_fact(&mf, mf.witness, "num_hidden_layers", NL);
        cf[4] = record_scalar_fact(&mf, mf.witness, "intermediate_size", FF);
        cf[5] = record_scalar_fact(&mf, mf.witness, "vocab_size", NT);
        crecs[ncrecs++] = said_record(blocks_of(cf, 6));
    }

    size_t hcap = NT * cap;
    lp_rowsig_hit *hits = malloc(sizeof(lp_rowsig_hit) * hcap);
    lp_rowsig_hit *nhits = malloc(sizeof(lp_rowsig_hit) * hcap);
    lp_rowsig_stats st; size_t nneg = 0;
    ExportEdges ee = { 0 };
    ExportEdges *eep = export_path ? &ee : NULL;
    File *mfp = do_ingest ? &mf : NULL;

    float *X = malloc(sizeof(float) * NT * D), *Ug = malloc(sizeof(float) * NT * D);
    #pragma omp parallel for
    for (size_t i = 0; i < NT; i++) for (size_t k = 0; k < D; k++) Ug[i * D + k] = U[i * D + k] * gf[k];

    printf("== circuits (two-sided |z| >= zmin: WIN positive z, LOSS negative z)\n");
    memcpy(X, E, sizeof(float) * NT * D); unit_rows(X, NT, D);
    t = now(); size_t nh = rowsig_two_sided(X, NT, X, NT, D, 1.0f, zmin, cap, hits, hcap, &st, nhits, &nneg);
    report("embed", &st, now() - t, hits, nh, 1);
    if (do_ingest || export_path) {
        Ref r = record_circuit_polarity(mfp, "embed", hits, nh, nhits, nneg, zmin, eep);
        if (do_ingest) crecs[ncrecs++] = r;
    }

    rmsnorm_rows(E, X, NT, D, NULL, eps);
    t = now(); nh = rowsig_two_sided(X, NT, Ug, NT, D, 1.0f, zmin, cap, hits, hcap, &st, nhits, &nneg);
    report("direct", &st, now() - t, hits, nh, 1);
    if (do_ingest || export_path) {
        Ref r = record_circuit_polarity(mfp, "direct", hits, nh, nhits, nneg, zmin, eep);
        if (do_ingest) crecs[ncrecs++] = r;
    }

    float *Q = malloc(sizeof(float) * NT * D), *K = malloc(sizeof(float) * NT * NKV * HD), *Vv = malloc(sizeof(float) * NT * NKV * HD);
    float *Ah = malloc(sizeof(float) * NT * HD), *Bh = malloc(sizeof(float) * NT * HD), *Mh = malloc(sizeof(float) * NT * HD);
    float *Wo_h = malloc(sizeof(float) * D * HD), *WdT = malloc(sizeof(float) * FF * D);
    lp_rowsig_hit *fh = malloc(sizeof(lp_rowsig_hit) * FF * cap);
    for (int L = 0; L < layers; L++) {
        char nm[256], cname[64]; double tl = now();
        snprintf(nm, sizeof nm, "model.layers.%d.input_layernorm.weight", L); Tensor tg = ckpt_get(&ckpt, nm); float *g = decode_f32(&tg);
        rmsnorm_rows(E, X, NT, D, g, eps); free(g);
        snprintf(nm, sizeof nm, "model.layers.%d.self_attn.q_proj.weight", L); Tensor tq = ckpt_get(&ckpt, nm); float *Wq = decode_f32(&tq);
        snprintf(nm, sizeof nm, "model.layers.%d.self_attn.k_proj.weight", L); Tensor tk = ckpt_get(&ckpt, nm); float *Wk = decode_f32(&tk);
        snprintf(nm, sizeof nm, "model.layers.%d.self_attn.v_proj.weight", L); Tensor tv = ckpt_get(&ckpt, nm); float *Wv = decode_f32(&tv);
        snprintf(nm, sizeof nm, "model.layers.%d.self_attn.o_proj.weight", L); Tensor to = ckpt_get(&ckpt, nm); float *Wo = decode_f32(&to);
        mm_abt(X, Wq, Q, NT, D, D); mm_abt(X, Wk, K, NT, NKV * HD, D); mm_abt(X, Wv, Vv, NT, NKV * HD, D);
        double tproj = now() - tl; uint64_t a4q = 0, a4o = 0, rq = 0; double sq = 0, so = 0, fq = 0, fo = 0;
        for (size_t h = 0; h < NH; h++) {
            size_t kv = h / (NH / NKV);
            cols(Q, NT, D, h * HD, HD, Ah); cols(K, NT, NKV * HD, kv * HD, HD, Bh);
            t = now(); nh = rowsig_two_sided(Ah, NT, Bh, NT, HD, 1.0f / sqrtf((float)HD), zmin, cap, hits, hcap, &st, nhits, &nneg);
            sq += now() - t; fq += st.flops; a4q += st.above[1]; rq += st.rows;
            snprintf(cname, sizeof cname, "L%d.H%zu.qk", L, h);
            if (L == 0 && h < 2) { report(cname, &st, now() - t, hits, nh, 1); FLOPS_TOTAL -= st.flops; SECS_TOTAL -= now() - t; }
            if ((do_ingest || export_path) && (nh || nneg)) {
                Ref r = record_circuit_polarity(mfp, cname, hits, nh, nhits, nneg, zmin, eep);
                if (do_ingest) crecs[ncrecs++] = r;
            }
            cols(Vv, NT, NKV * HD, kv * HD, HD, Ah); cols(Wo, D, D, h * HD, HD, Wo_h);
            cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, (MKL_INT)NT, (MKL_INT)HD, (MKL_INT)D, 1.0f, Ug, (MKL_INT)D, Wo_h, (MKL_INT)HD, 0.0f, Mh, (MKL_INT)HD);
            t = now(); nh = rowsig_two_sided(Ah, NT, Mh, NT, HD, 1.0f, zmin, cap, hits, hcap, &st, nhits, &nneg);
            so += now() - t; fo += st.flops; a4o += st.above[1];
            snprintf(cname, sizeof cname, "L%d.H%zu.ov", L, h);
            if (L == 0 && h < 2) { report(cname, &st, now() - t, hits, nh, 1); FLOPS_TOTAL -= st.flops; SECS_TOTAL -= now() - t; }
            if ((do_ingest || export_path) && (nh || nneg)) {
                Ref r = record_circuit_polarity(mfp, cname, hits, nh, nhits, nneg, zmin, eep);
                if (do_ingest) crecs[ncrecs++] = r;
            }
        }
        FLOPS_TOTAL += fq + fo; SECS_TOTAL += sq + so;
        free(Wq); free(Wk); free(Wv); free(Wo);

        snprintf(nm, sizeof nm, "model.layers.%d.mlp.down_proj.weight", L); Tensor td = ckpt_get(&ckpt, nm); float *Wd = decode_f32(&td);
        snprintf(nm, sizeof nm, "model.layers.%d.post_attention_layernorm.weight", L);
        char ng_nm[256], nu_nm[256];
        snprintf(ng_nm, sizeof ng_nm, "model.layers.%d.mlp.gate_proj.weight", L);
        snprintf(nu_nm, sizeof nu_nm, "model.layers.%d.mlp.up_proj.weight", L);
        Tensor tpn, twg, twu;
        if (ckpt_find_tensor(&ckpt, nm, &tpn) && ckpt_find_tensor(&ckpt, ng_nm, &twg) && ckpt_find_tensor(&ckpt, nu_nm, &twu)) {
            float *g_post = decode_f32(&tpn), *Wg = decode_f32(&twg), *Wu = decode_f32(&twu);
            rmsnorm_rows(E, X, NT, D, g_post, eps); free(g_post);
            ffn_connected(X, Wg, Wu, Wd, Q, NT, D, FF);
            free(Wg); free(Wu);
            t = now(); nh = rowsig_two_sided(Q, NT, Ug, NT, D, 1.0f, zmin, cap, hits, hcap, &st, nhits, &nneg);
            FLOPS_TOTAL += st.flops; SECS_TOTAL += now() - t;
            snprintf(cname, sizeof cname, "L%d.ffn", L);
            if (L == 0) { report(cname, &st, now() - t, hits, nh, 1); FLOPS_TOTAL -= st.flops; SECS_TOTAL -= now() - t; }
            if ((do_ingest || export_path) && (nh || nneg)) {
                Ref r = record_circuit_polarity(mfp, cname, hits, nh, nhits, nneg, zmin, eep);
                if (do_ingest) crecs[ncrecs++] = r;
            }
        }
        #pragma omp parallel for
        for (size_t n = 0; n < FF; n++) for (size_t k = 0; k < D; k++) WdT[n * D + k] = Wd[k * FF + n];
        free(Wd);
        t = now(); size_t nf = lp_rowsig(WdT, FF, Ug, NT, D, 1.0f, zmin, cap, fh, FF * cap, &st); double sf = now() - t;
        FLOPS_TOTAL += st.flops; SECS_TOTAL += sf;
        printf("  layer %-2d  projections %5.2f s | %zu heads qk %6.2f s, ov %6.2f s: per row above z4 %.2f (qk) %.2f (ov) | ffn %zu neurons %5.2f s, above z4 %.2f, none %4.1f%% | %.1f s\n",
               L, tproj, NH, sq, so, (double)a4q / rq, (double)a4o / rq, FF, sf, (double)st.above[1] / st.rows, 100.0 * st.rows_without / st.rows, now() - tl);
        (void)nf; fflush(stdout);
    }
    printf("\n== total: %.1f s wall, kernel %.1f s at %.1f GFLOP/s (%.1f TFLOP)\n", now() - T, SECS_TOTAL, SECS_TOTAL > 0 ? FLOPS_TOTAL / SECS_TOTAL / 1e9 : 0.0, FLOPS_TOTAL / 1e12);

    if (do_ingest) {
        double ti = now();
        Ref body = ncrecs ? blocks_of(crecs, ncrecs) : mf.witness;
        Ref tp[2] = { said_metadata(mf.witness), body };
        mf.trunk = compose(tp, 2, ref_above(tp, 2));
        mf.file = mf.trunk; mf.has_file = 1;
        Fold *fold = fold_new(); fold_events(fold, &mf, 1);
        LoadStats ls = { 0 };
        LoadWitness lw = { .id = mf.trunk.id, .trust = mf.trust };
        int rc_ = load(conninfo, 2, &mf, 1, &ls); fold_free(fold);
        if (rc_ || load_register(conninfo, 2, &lw, 1, &mf, 1) || merge(conninfo, 2, &ls)) {
            free(crecs); free(mf.ev.e);
            ckpt_close(&ckpt);
            for (size_t i = 0; i < NV; i++) free(V[i].text);
            free(V); V = NULL; NV = 0;
            lp_text_free(TX); TX = NULL;
            return 1;
        }
        char wid[33]; lp_id_hex(&mf.trunk.id, wid);
        printf("ingested witness %s (%s, trust %.2f): %zu claims attested, %llu entities new, raw_weight_bytes_retained = 0   (%.2f s)\n",
               wid, wname, mf.trust, mf.ev.n, (unsigned long long)ls.ent_rows, now() - ti);
        free(crecs); free(mf.ev.e);
    }

    int export_rc = 0;
    if (export_path) {
        const char **tok_strs = calloc(NV, sizeof(char *));
        lp_id *tok_ids = calloc(NV, sizeof(lp_id));
        const char **ew = calloc(ee.n ? ee.n : 1u, sizeof(char *));
        for (size_t i = 0; i < NV; i++) {
            tok_strs[i] = V[i].text ? V[i].text : "<unk>";
            tok_ids[i] = V[i].id;
        }
        for (size_t i = 0; i < ee.n; i++) ew[i] = wname;
        LpMoldSpec sp = {
            .name = wname,
            .out_path = export_path,
            .n_vocab = NV,
            .tokens = tok_strs,
            .token_ids = tok_ids,
            .bos_id = 0,
            .eos_id = NV > 1 ? 1 : 0,
            .n_edges = ee.n,
            .rows = ee.rows,
            .cols = ee.cols,
            .weights = ee.weights,
            .edge_witnesses = ew,
            .edge_lineages = ew,
            .hidden_dim = D > 64 ? 64 : D,
            .n_layers = layers > 0 ? (size_t)layers : 1u,
            .n_heads = NH,
            .n_kv_heads = NKV,
            .ffn_dim = FF > 256 ? 256 : FF,
            .rms_eps = eps,
            .rope_base = 10000.0,
            .f16_weights = 0
        };
        export_rc = lp_mold_export(&sp);
        free((void *)ew);
        free(tok_ids);
        free((void *)tok_strs);
    }

    ckpt_close(&ckpt);
    for (size_t i = 0; i < NV; i++) free(V[i].text);
    free(V); V = NULL; NV = 0; nsample = 0;
    lp_text_free(TX); TX = NULL;
    free(ee.rows); free(ee.cols); free(ee.weights);
    free(Q); free(K); free(Vv); free(Ah); free(Bh); free(Mh); free(Wo_h); free(WdT); free(fh);
    free(hits); free(nhits); free(X); free(Ug); free(E); free(U); free(gf);
    return export_rc;
}
