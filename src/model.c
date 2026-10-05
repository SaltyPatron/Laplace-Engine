/* laplace model: decompose a transformer checkpoint (safetensors, Llama architecture) into "b beats c given a" survivors
 * and measure it. Prototype stage: this reports timing, throughput, and how much survives each circuit's own noise
 * floor; recording the survivors as attestations comes after the numbers are understood.
 *
 * Circuits, each a set of rows a against candidates b:
 *   embed      token a vs token b          cosine of the input embeddings
 *   direct     token a vs next token b     final-norm(embedding a) against the unembedding (lm_head)
 *   Lk.Hh.qk   token a attends to token b  layer-k head-h query of a against key of b (positions ignored)
 *   Lk.Hh.ov   attending to a promotes b   head-h value of a through its output projection into the unembedding
 *   Lk.ffn     neuron n promotes token b   the neuron's output direction into the unembedding
 * Layer inputs are the embeddings through that layer's RMSNorm: exact for layer 0, an approximation deeper in, where
 * the real input is the residual stream.
 *
 * Usage: laplace model model_dir [--layers N] [--z zmin] [--cap K] [--sample word ...]
 */
#include "engine.h"
#include "json_min.h"
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mkl.h>
#include <immintrin.h>

static char *slurp(const char *path, size_t *len){
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); rewind(f); char *b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != *len) { fclose(f); free(b); return NULL; } fclose(f); b[*len] = 0; return b;
}

/* ---- safetensors: 8-byte header length, JSON header, tensor bytes */
typedef struct { const uint8_t *base; jdoc h; uint32_t root; } ST;
typedef struct { const char *dtype; size_t shape[4]; int nd; const uint8_t *data; size_t bytes; } Tensor;
static ST st_open(const char *path){
    ST s = { 0 }; size_t size;
    if (!(s.base = os_map_read(path, &size)) || size < 8) { perror(path); exit(1); }
    uint64_t hl; memcpy(&hl, s.base, 8); s.h = j_parse((const char *)s.base + 8, hl); s.root = 0;
    s.base += 8 + hl; return s;
}
static Tensor st_get(const ST *s, const char *name){
    Tensor t = { 0 }; int64_t k = j_get(&s->h, s->root, name);
    if (k < 0) { fprintf(stderr, "tensor %s missing\n", name); exit(1); }
    t.dtype = s->h.v[j_get(&s->h, k, "dtype")].str;
    const jnode *sh = &s->h.v[j_get(&s->h, k, "shape")]; t.nd = sh->n;
    for (uint32_t i = 0; i < sh->n; i++) t.shape[i] = (size_t)s->h.v[s->h.kids[sh->first + i]].num;
    const jnode *off = &s->h.v[j_get(&s->h, k, "data_offsets")];
    size_t o0 = (size_t)s->h.v[s->h.kids[off->first]].num, o1 = (size_t)s->h.v[s->h.kids[off->first + 1]].num;
    t.data = s->base + o0; t.bytes = o1 - o0; return t;
}
static float *to_f32(const Tensor *t){
    size_t n = 1; for (int i = 0; i < t->nd; i++) n *= t->shape[i];
    float *o = malloc(sizeof(float) * n);
    if (!strcmp(t->dtype, "F32")) memcpy(o, t->data, 4 * n);
    else if (!strcmp(t->dtype, "BF16")) {
        const uint16_t *u = (const uint16_t *)t->data;
        #pragma omp parallel for
        for (size_t i = 0; i < n; i++) { uint32_t b = (uint32_t)u[i] << 16; memcpy(&o[i], &b, 4); }
    } else if (!strcmp(t->dtype, "F16")) {
        const uint16_t *u = (const uint16_t *)t->data;
        #pragma omp parallel for
        for (size_t i = 0; i < n; i++) o[i] = _cvtsh_ss(u[i]);
    } else { fprintf(stderr, "dtype %s not handled yet\n", t->dtype); exit(1); }
    return o;
}

/* ---- tokens: the model's vocabulary as text, and each token's content ID */
typedef struct { char *text; int kind; lp_id id; } Tok;                /* kind: 0 text, 1 byte, 2 special */
/* A token's ID is the ID of the text it stands for, by the one decomposition of text. */
static lp_text *TX;
static void token_id(const char *s, lp_id *out){ *out = lp_text_decompose(TX, (const uint8_t *)s, strlen(s), NULL, NULL).id; }
static Tok *load_vocab(const char *dir, size_t *nv){
    char p[4096]; snprintf(p, sizeof p, "%s/tokenizer.json", dir); size_t len; char *b = slurp(p, &len);
    if (!b) { perror(p); exit(1); }
    jdoc d = j_parse(b, len); int64_t model = j_get(&d, 0, "model"), vocab = j_get(&d, model, "vocab");
    const jnode *vo = &d.v[vocab]; size_t n = vo->n / 2, maxid = 0;
    for (uint32_t i = 0; i < vo->n; i += 2) { size_t id = (size_t)d.v[d.kids[vo->first + i + 1]].num; if (id > maxid) maxid = id; }
    Tok *t = calloc(maxid + 1, sizeof(Tok)); *nv = maxid + 1;
    for (uint32_t i = 0; i < vo->n; i += 2) {
        const char *s = d.v[d.kids[vo->first + i]].str; size_t id = (size_t)d.v[d.kids[vo->first + i + 1]].num;
        size_t L = strlen(s); char *o = malloc(L + 1), *q = o;
        for (size_t k = 0; k < L; ) {                                        /* SentencePiece's U+2581 marks a space */
            if ((unsigned char)s[k] == 0xE2 && (unsigned char)s[k + 1] == 0x96 && (unsigned char)s[k + 2] == 0x81) { *q++ = ' '; k += 3; }
            else *q++ = s[k++];
        }
        *q = 0; t[id].text = o;
        if (L == 6 && s[0] == '<' && s[1] == '0' && s[2] == 'x' && s[5] == '>') t[id].kind = 1;
        else if (L > 2 && s[0] == '<' && s[L - 1] == '>') t[id].kind = 2;
        else token_id(o, &t[id].id);
    }
    (void)n; return t;
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
/* C (m x n) = A (m x k) . B^T where B is (n x k), all row-major */
static void mm_abt(const float *A, const float *B, float *C, size_t m, size_t n, size_t k){
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, m, n, k, 1.0f, A, k, B, k, 0.0f, C, n);
}
static void cols(const float *src, size_t m, size_t ld, size_t c0, size_t w, float *dst){      /* copy columns c0..c0+w */
    #pragma omp parallel for
    for (size_t i = 0; i < m; i++) memcpy(dst + i * w, src + i * ld + c0, w * 4);
}

/* ---- reporting */
static Tok *V; static size_t NV; static double FLOPS_TOTAL, SECS_TOTAL; static uint64_t KEPT_TOTAL, ROWS_TOTAL;
static int sample_rows[16]; static int nsample;
static void report(const char *name, const lp_rowsig_stats *s, double secs, const lp_rowsig_hit *h, size_t nh, int rows_are_tokens){
    FLOPS_TOTAL += s->flops; SECS_TOTAL += secs; KEPT_TOTAL += nh; ROWS_TOTAL += s->rows;
    printf("  %-12s %7.2f s %7.1f GFLOP/s   rows %'7llu   per row above z3 %7.2f  z4 %6.2f  z5 %6.2f   rows with none %5.1f%%\n", name, secs,
           s->flops / secs / 1e9, (unsigned long long)s->rows, (double)s->above[0] / s->rows, (double)s->above[1] / s->rows,
           (double)s->above[2] / s->rows, 100.0 * s->rows_without / s->rows);
    if (!rows_are_tokens) return;
    for (int q = 0; q < nsample; q++) {
        int row = sample_rows[q]; printf("      %-10s ->", V[row].text);
        int shown = 0;
        for (size_t i = 0; i < nh && shown < 10; i++) if ((int)h[i].row == row && h[i].z >= 4) { printf(" %s(%.1f)", V[h[i].col].text, h[i].z); shown++; }
        putchar('\n');
    }
}

int cmd_model(int argc, char **argv){
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    if (argc < 2) { fprintf(stderr, "usage: laplace model model_dir [--layers N] [--z zmin] [--cap K] [--sample word ...]\n"); return 2; }
    const char *dir = argv[1]; int layers = -1; float zmin = 3; uint32_t cap = 64; const char *samples[16]; int nsamp = 0;
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "--layers")) layers = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--z")) zmin = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "--cap")) cap = (uint32_t)atoi(argv[++a]);
        else if (!strcmp(argv[a], "--sample")) while (a + 1 < argc && argv[a + 1][0] != '-' && nsamp < 16) samples[nsamp++] = argv[++a];
    }
    if (!nsamp) { static const char *d[] = { " king", " dog", " Paris", " happy", " run" }; for (int i = 0; i < 5; i++) samples[nsamp++] = d[i]; }
    double T = now(), t = now();
    /* Every GEMM below in MKL's reproducible mode (CNR, AVX2, strict), the level both machines have: without it MKL
     * picks kernels by CPU, and the z-score candidates kept from the products could differ between them. */
    if (!lp_mkl_reproducible()) { fprintf(stderr, "laplace model: MKL refused reproducible mode (CNR AVX2, strict)\n"); return 1; }
    tier0_open(NULL); TX = lp_text_new(T0);
    printf("laplace model %s\ncpu: %s; MKL threads %d\n", dir, lp_cpu_describe(lp_cpu_features()), mkl_get_max_threads());

    char p[4096]; size_t len; snprintf(p, sizeof p, "%s/config.json", dir); char *cb = slurp(p, &len); jdoc cfg = j_parse(cb, len);
    size_t D = (size_t)j_num(&cfg, 0, "hidden_size", 0), NH = (size_t)j_num(&cfg, 0, "num_attention_heads", 0),
           NKV = (size_t)j_num(&cfg, 0, "num_key_value_heads", NH), NL = (size_t)j_num(&cfg, 0, "num_hidden_layers", 0),
           FF = (size_t)j_num(&cfg, 0, "intermediate_size", 0); double eps = j_num(&cfg, 0, "rms_norm_eps", 1e-5);
    size_t HD = D / NH; if (layers < 0 || (size_t)layers > NL) layers = (int)NL;
    V = load_vocab(dir, &NV);
    size_t ntext = 0, nbyte = 0, nspec = 0; for (size_t i = 0; i < NV; i++) { if (!V[i].text) continue; if (V[i].kind == 0) ntext++; else if (V[i].kind == 1) nbyte++; else nspec++; }
    for (int q = 0; q < nsamp; q++) for (size_t i = 0; i < NV; i++) if (V[i].text && !strcmp(V[i].text, samples[q])) { sample_rows[nsample++] = (int)i; break; }
    printf("config: hidden %zu, heads %zu (kv %zu, dim %zu), layers %zu (running %d), ffn %zu; vocabulary %zu: text %zu, byte %zu, special %zu\n",
           D, NH, NKV, HD, NL, layers, FF, NV, ntext, nbyte, nspec);

    snprintf(p, sizeof p, "%s/model.safetensors", dir); ST s = st_open(p);
    Tensor te = st_get(&s, "model.embed_tokens.weight"), tu = st_get(&s, "lm_head.weight"), tn = st_get(&s, "model.norm.weight");
    float *E = to_f32(&te), *U = to_f32(&tu), *gf = to_f32(&tn); size_t NT = te.shape[0];
    printf("loaded embeddings and unembedding (%s) in %.2f s\n\n", te.dtype, now() - t);

    size_t hcap = NT * cap; lp_rowsig_hit *hits = malloc(sizeof(lp_rowsig_hit) * hcap); lp_rowsig_stats st;
    float *X = malloc(sizeof(float) * NT * D), *Ug = malloc(sizeof(float) * NT * D);
    /* unembedding with the final norm's gain folded in: logits = (x / rms(x)) . (g * u) */
    #pragma omp parallel for
    for (size_t i = 0; i < NT; i++) for (size_t k = 0; k < D; k++) Ug[i * D + k] = U[i * D + k] * gf[k];

    printf("== circuits (z = score's distance above its row's mean, in the row's standard deviations)\n");
    memcpy(X, E, sizeof(float) * NT * D); unit_rows(X, NT, D);
    t = now(); size_t nh = lp_rowsig(X, NT, X, NT, D, 1.0f, zmin, cap, hits, hcap, &st); report("embed", &st, now() - t, hits, nh, 1);
    rmsnorm_rows(E, X, NT, D, NULL, eps);
    t = now(); nh = lp_rowsig(X, NT, Ug, NT, D, 1.0f, zmin, cap, hits, hcap, &st); report("direct", &st, now() - t, hits, nh, 1);

    float *Q = malloc(sizeof(float) * NT * D), *K = malloc(sizeof(float) * NT * NKV * HD), *Vv = malloc(sizeof(float) * NT * NKV * HD);
    float *Ah = malloc(sizeof(float) * NT * HD), *Bh = malloc(sizeof(float) * NT * HD), *Mh = malloc(sizeof(float) * NT * HD);
    float *Wo_h = malloc(sizeof(float) * D * HD), *WdT = malloc(sizeof(float) * FF * D);
    lp_rowsig_hit *fh = malloc(sizeof(lp_rowsig_hit) * FF * cap);
    for (int L = 0; L < layers; L++) {
        char nm[256]; double tl = now();
        snprintf(nm, sizeof nm, "model.layers.%d.input_layernorm.weight", L); Tensor tg = st_get(&s, nm); float *g = to_f32(&tg);
        rmsnorm_rows(E, X, NT, D, g, eps); free(g);
        snprintf(nm, sizeof nm, "model.layers.%d.self_attn.q_proj.weight", L); Tensor tq = st_get(&s, nm); float *Wq = to_f32(&tq);
        snprintf(nm, sizeof nm, "model.layers.%d.self_attn.k_proj.weight", L); Tensor tk = st_get(&s, nm); float *Wk = to_f32(&tk);
        snprintf(nm, sizeof nm, "model.layers.%d.self_attn.v_proj.weight", L); Tensor tv = st_get(&s, nm); float *Wv = to_f32(&tv);
        snprintf(nm, sizeof nm, "model.layers.%d.self_attn.o_proj.weight", L); Tensor to = st_get(&s, nm); float *Wo = to_f32(&to);
        mm_abt(X, Wq, Q, NT, D, D); mm_abt(X, Wk, K, NT, NKV * HD, D); mm_abt(X, Wv, Vv, NT, NKV * HD, D);
        double tproj = now() - tl; uint64_t a4q = 0, a4o = 0, rq = 0; double sq = 0, so = 0, fq = 0, fo = 0;
        for (size_t h = 0; h < NH; h++) {
            size_t kv = h / (NH / NKV);
            cols(Q, NT, D, h * HD, HD, Ah); cols(K, NT, NKV * HD, kv * HD, HD, Bh);
            t = now(); nh = lp_rowsig(Ah, NT, Bh, NT, HD, 1.0f / sqrtf((float)HD), zmin, cap, hits, hcap, &st); sq += now() - t; fq += st.flops;
            a4q += st.above[1]; rq += st.rows;
            if (L == 0 && h < 2) { char lab[32]; snprintf(lab, sizeof lab, "L%d.H%zu.qk", L, h); report(lab, &st, now() - t, hits, nh, 1); FLOPS_TOTAL -= st.flops; SECS_TOTAL -= now() - t; }
            /* OV: attending to token a promotes b. Low rank: (V_a W_O,h) . Ug_b = V_a . (Ug_b W_O,h), both of width HD. */
            cols(Vv, NT, NKV * HD, kv * HD, HD, Ah); cols(Wo, D, D, h * HD, HD, Wo_h);
            cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, NT, HD, D, 1.0f, Ug, D, Wo_h, HD, 0.0f, Mh, HD);
            t = now(); nh = lp_rowsig(Ah, NT, Mh, NT, HD, 1.0f, zmin, cap, hits, hcap, &st); so += now() - t; fo += st.flops; a4o += st.above[1];
            if (L == 0 && h < 2) { char lab[32]; snprintf(lab, sizeof lab, "L%d.H%zu.ov", L, h); report(lab, &st, now() - t, hits, nh, 1); FLOPS_TOTAL -= st.flops; SECS_TOTAL -= now() - t; }
        }
        FLOPS_TOTAL += fq + fo; SECS_TOTAL += sq + so;
        free(Wq); free(Wk); free(Wv); free(Wo);
        /* FFN: neuron n's output direction is column n of down_proj; it promotes the tokens it points at. */
        snprintf(nm, sizeof nm, "model.layers.%d.mlp.down_proj.weight", L); Tensor td = st_get(&s, nm); float *Wd = to_f32(&td);
        #pragma omp parallel for
        for (size_t n = 0; n < FF; n++) for (size_t k = 0; k < D; k++) WdT[n * D + k] = Wd[k * FF + n];
        free(Wd);
        t = now(); size_t nf = lp_rowsig(WdT, FF, Ug, NT, D, 1.0f, zmin, cap, fh, FF * cap, &st); double sf = now() - t;
        FLOPS_TOTAL += st.flops; SECS_TOTAL += sf;
        printf("  layer %-2d  projections %5.2f s | %zu heads qk %6.2f s, ov %6.2f s: per row above z4 %.2f (qk) %.2f (ov) | ffn %zu neurons %5.2f s, above z4 %.2f, none %4.1f%% | %.1f s\n",
               L, tproj, NH, sq, so, (double)a4q / rq, (double)a4o / rq, FF, sf, (double)st.above[1] / st.rows, 100.0 * st.rows_without / st.rows, now() - tl);
        (void)nf; fflush(stdout);
    }
    printf("\n== total: %.1f s wall, kernel %.1f s at %.1f GFLOP/s (%.1f TFLOP)\n", now() - T, SECS_TOTAL, FLOPS_TOTAL / SECS_TOTAL / 1e9, FLOPS_TOTAL / 1e12);
    return 0;
}
