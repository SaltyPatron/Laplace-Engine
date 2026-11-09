/* laplace model: decompose a transformer checkpoint (safetensors, single or sharded) into significant entity-pair
 * survivors above each circuit's noise floor (Sequence 24: Models as Food) and optionally record them into PostgreSQL
 * under AIModelProbe (0.50) with zero raw weight bytes retained.
 *
 * Circuits, each a set of rows a against candidates b:
 *   embed      token a vs token b          cosine of the input embeddings
 *   direct     token a vs next token b     final-norm(embedding a) against the unembedding (lm_head)
 *   Lk.Hh.qk   token a attends to token b  layer-k head-h query of a against key of b (positions ignored)
 *   Lk.Hh.ov   attending to a promotes b   head-h value of a through its output projection into the unembedding
 *   Lk.ffn     token a through SwiGLU FFN  connected gate+up+down MLP output of a into the unembedding
 *   Lk.ffn.dir neuron n promotes token b   the neuron's output direction into the unembedding
 *
 * Usage: laplace model model_dir [-d conninfo] [--ingest] [--witness name] [--layers N] [--z zmin] [--cap K] [--sample word ...]
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

/* ---- safetensors: single file or sharded via model.safetensors.index.json */
typedef struct { char path[4096]; const uint8_t *base; jdoc h; uint32_t root; } STShard;
typedef struct { char dir[4096]; STShard shard[64]; int nshard; char *idx_buf; jdoc idx; int64_t wmap; } ST;
typedef struct { const char *dtype; size_t shape[4]; int nd; const uint8_t *data; size_t bytes; } Tensor;

static STShard *st_open_shard(ST *s, const char *rel){
    char full[4352];
    if (rel[0] == '/' || (rel[0] && rel[1] == ':')) snprintf(full, sizeof full, "%s", rel);
    else snprintf(full, sizeof full, "%s/%s", s->dir, rel);
    for (int i = 0; i < s->nshard; i++) if (!strcmp(s->shard[i].path, full)) return &s->shard[i];
    if (s->nshard >= 64) { fprintf(stderr, "too many safetensors shards\n"); exit(1); }
    STShard *sh = &s->shard[s->nshard++];
    snprintf(sh->path, sizeof sh->path, "%s", full);
    size_t size = 0;
    if (!(sh->base = os_map_read(full, &size)) || size < 8) { perror(full); exit(1); }
    uint64_t hl; memcpy(&hl, sh->base, 8); sh->h = j_parse((const char *)sh->base + 8, hl); sh->root = 0;
    sh->base += 8 + hl; return sh;
}
static ST st_open_dir(const char *dir){
    ST s = { 0 }; snprintf(s.dir, sizeof s.dir, "%s", dir);
    char idxp[4352]; snprintf(idxp, sizeof idxp, "%s/model.safetensors.index.json", dir);
    size_t len = 0; s.idx_buf = slurp(idxp, &len);
    if (s.idx_buf) { s.idx = j_parse(s.idx_buf, len); s.wmap = j_get(&s.idx, 0, "weight_map"); }
    else { s.wmap = -1; st_open_shard(&s, "model.safetensors"); }
    return s;
}
static STShard *st_shard_for(ST *s, const char *name, int64_t *kout){
    if (s->wmap >= 0) {
        int64_t fn = j_get(&s->idx, (uint32_t)s->wmap, name);
        if (fn < 0 || !s->idx.v[fn].str) { *kout = -1; return NULL; }
        STShard *sh = st_open_shard(s, s->idx.v[fn].str);
        *kout = j_get(&sh->h, sh->root, name); return sh;
    }
    STShard *sh = &s->shard[0]; *kout = j_get(&sh->h, sh->root, name); return sh;
}
static int st_has(ST *s, const char *name){ int64_t k = -1; st_shard_for(s, name, &k); return k >= 0; }
static Tensor st_get(ST *s, const char *name){
    Tensor t = { 0 }; int64_t k = -1; STShard *sh = st_shard_for(s, name, &k);
    if (!sh || k < 0) { fprintf(stderr, "tensor %s missing\n", name); exit(1); }
    t.dtype = sh->h.v[j_get(&sh->h, (uint32_t)k, "dtype")].str;
    const jnode *sv = &sh->h.v[j_get(&sh->h, (uint32_t)k, "shape")]; t.nd = (int)sv->n;
    for (uint32_t i = 0; i < sv->n; i++) t.shape[i] = (size_t)sh->h.v[sh->h.kids[sv->first + i]].num;
    const jnode *off = &sh->h.v[j_get(&sh->h, (uint32_t)k, "data_offsets")];
    size_t o0 = (size_t)sh->h.v[sh->h.kids[off->first]].num, o1 = (size_t)sh->h.v[sh->h.kids[off->first + 1]].num;
    t.data = sh->base + o0; t.bytes = o1 - o0; return t;
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
static void token_id(const char *s, int do_ingest, lp_id *out, Ref *rout){
    size_t sl = strlen(s);
    if (do_ingest && sl > 0) { *rout = text_ref(CTX[0], (const uint8_t *)s, sl); *out = rout->id; }
    else { *out = lp_text_decompose(TX, (const uint8_t *)s, sl, NULL, NULL).id; rout->id = *out; }
}
static Tok *load_vocab(const char *dir, int do_ingest, size_t *nv, Ref *vocab_trunk){
    char p[4096]; snprintf(p, sizeof p, "%s/tokenizer.json", dir); size_t len; char *b = slurp(p, &len);
    if (!b) { perror(p); exit(1); }
    jdoc d = j_parse(b, len); int64_t model = j_get(&d, 0, "model"), vocab = j_get(&d, model, "vocab");
    const jnode *vo = &d.v[vocab]; size_t maxid = 0;
    for (uint32_t i = 0; i < vo->n; i += 2) { size_t id = (size_t)d.v[d.kids[vo->first + i + 1]].num; if (id > maxid) maxid = id; }
    Tok *t = calloc(maxid + 1, sizeof(Tok)); *nv = maxid + 1;
    for (uint32_t i = 0; i < vo->n; i += 2) {
        const char *s = d.v[d.kids[vo->first + i]].str; size_t id = (size_t)d.v[d.kids[vo->first + i + 1]].num;
        size_t L = strlen(s); char *o = malloc(L + 1), *q = o;
        for (size_t k = 0; k < L; ) {
            /* SentencePiece U+2581 (0xE2 0x96 0x81) or GPT-2/BPE U+0120 'Ġ' (0xC4 0xA0) -> ' '; U+010A 'Ċ' (0xC4 0x8A) -> '\n' */
            if (k + 2 < L && (unsigned char)s[k] == 0xE2 && (unsigned char)s[k + 1] == 0x96 && (unsigned char)s[k + 2] == 0x81) { *q++ = ' '; k += 3; }
            else if (k + 1 < L && (unsigned char)s[k] == 0xC4 && (unsigned char)s[k + 1] == 0xA0) { *q++ = ' '; k += 2; }
            else if (k + 1 < L && (unsigned char)s[k] == 0xC4 && (unsigned char)s[k + 1] == 0x8A) { *q++ = '\n'; k += 2; }
            else *q++ = s[k++];
        }
        *q = 0; t[id].text = o;
        if (L == 6 && s[0] == '<' && s[1] == '0' && s[2] == 'x' && s[5] == '>') t[id].kind = 1;
        else if (L > 2 && ((s[0] == '<' && s[L - 1] == '>') || (s[0] == '[' && s[L - 1] == ']'))) t[id].kind = 2;
        else if (!o[0]) t[id].kind = 2;
        else token_id(o, do_ingest, &t[id].id, &t[id].ref);
    }
    if (do_ingest && vocab_trunk) {
        Ref *vr = malloc(sizeof(Ref) * (*nv ? *nv : 1)); size_t nvr = 0;
        for (size_t i = 0; i < *nv; i++) if (t[i].text && t[i].kind == 0) { vr[nvr] = t[i].ref; vr[nvr++].said = 0; }
        *vocab_trunk = nvr ? blocks_of(vr, nvr) : (Ref){ 0 };
        free(vr);
    }
    free(b); return t;
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

/* ---- reporting & optional ingestion into PostgreSQL (Sequence 24.4-24.10) */
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

static Ref record_circuit_hits(File *mf, const char *cname, const lp_rowsig_hit *h, size_t nh, float zmin){
    Ref rel = text_ref(CTX[0], (const uint8_t *)cname, strlen(cname));
    Ref *claims = malloc(sizeof(Ref) * (nh ? nh : 1)); size_t nc = 0;
    for (size_t i = 0; i < nh; i++) {
        uint32_t r = h[i].row, c = h[i].col;
        if (r >= NV || c >= NV || r == c || !V[r].text || !V[c].text || V[r].kind != 0 || V[c].kind != 0) continue;
        if (lp_id_eq(&V[r].ref.id, &V[c].ref.id)) continue;
        Ref p3[3] = { V[r].ref, rel, V[c].ref }; p3[0].said = p3[1].said = p3[2].said = 0;
        Ref cl = said_claim(compose(p3, 3, ref_above(p3, 3)));
        double sc = 0.5 + 0.5 * erf((double)(h[i].z - zmin) / sqrt(2.0));
        if (sc < 0.51) sc = 0.51; if (sc > 0.999) sc = 0.999;
        cl.outcome = (uint8_t)lp_outcome_of(sc, &cl.spare);
        Event ev = { .claim = cl.id, .witnessed = cl.id, .score = (float)sc, .enter_rating = 1500.0f, .kind = EV_CLAIM };
        ev_push(&mf->ev, &ev);
        claims[nc++] = cl;
    }
    Ref out = nc ? said_record(blocks_of(claims, nc)) : rel;
    free(claims);
    return out;
}

/* Connected SwiGLU FFN: Y = (SiLU(X W_gate^T) * (X W_up^T)) W_down^T, tiled over token rows */
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
        /* Wd has shape (D x FF), so G (b x FF) . Wd^T (FF x D) = cblas_sgemm(NoTrans, Trans) */
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)b, (MKL_INT)D, (MKL_INT)FF, 1.0f, G, (MKL_INT)FF, Wd, (MKL_INT)FF, 0.0f, Yb, (MKL_INT)D);
    }
    free(G); free(U);
}

int cmd_model(int argc, char **argv){
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    if (argc < 2) {
        fprintf(stderr, "usage: laplace model model_dir [-d conninfo] [--ingest] [--witness name] [--layers N] [--z zmin] [--cap K] [--sample word ...]\n");
        return 2;
    }
    const char *dir = argv[1], *conninfo = laplace_db(), *wname = NULL;
    int layers = -1, do_ingest = 0; float zmin = 0.0f; uint32_t cap = 64; const char *samples[16]; int nsamp = 0;
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "--ingest")) do_ingest = 1;
        else if (!strcmp(argv[a], "--witness") && a + 1 < argc) wname = argv[++a];
        else if (!strcmp(argv[a], "--layers") && a + 1 < argc) layers = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--z") && a + 1 < argc) zmin = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "--cap") && a + 1 < argc) cap = (uint32_t)atoi(argv[++a]);
        else if (!strcmp(argv[a], "--sample")) while (a + 1 < argc && argv[a + 1][0] != '-' && nsamp < 16) samples[nsamp++] = argv[++a];
    }
    if (!nsamp) { static const char *d[] = { " king", " dog", " Paris", " happy", " run" }; for (int i = 0; i < 5; i++) samples[nsamp++] = d[i]; }
    double T = now(), t = now();
    if (!lp_mkl_reproducible()) { fprintf(stderr, "laplace model: MKL refused reproducible mode (CNR AVX2, strict)\n"); return 1; }
    tier0_open(NULL); TX = lp_text_new(T0);
    if (do_ingest) { table_init(); ctx_open(1); }
    printf("laplace model %s\ncpu: %s; MKL threads %d\n", dir, lp_cpu_describe(lp_cpu_features()), mkl_get_max_threads());

    char p[4096]; size_t len; snprintf(p, sizeof p, "%s/config.json", dir); char *cb = slurp(p, &len);
    if (!cb) { perror(p); return 1; }
    jdoc cfg = j_parse(cb, len);
    size_t D = (size_t)j_num(&cfg, 0, "hidden_size", 0), NH = (size_t)j_num(&cfg, 0, "num_attention_heads", 0),
           NKV = (size_t)j_num(&cfg, 0, "num_key_value_heads", NH), NL = (size_t)j_num(&cfg, 0, "num_hidden_layers", 0),
           FF = (size_t)j_num(&cfg, 0, "intermediate_size", 0); double eps = j_num(&cfg, 0, "rms_norm_eps", 1e-5);
    size_t HD = D / NH; if (layers < 0 || (size_t)layers > NL) layers = (int)NL;
    Ref vocab_trunk = { 0 };
    V = load_vocab(dir, do_ingest, &NV, &vocab_trunk);
    size_t ntext = 0, nbyte = 0, nspec = 0; for (size_t i = 0; i < NV; i++) { if (!V[i].text) continue; if (V[i].kind == 0) ntext++; else if (V[i].kind == 1) nbyte++; else nspec++; }
    for (int q = 0; q < nsamp; q++) for (size_t i = 0; i < NV; i++) if (V[i].text && !strcmp(V[i].text, samples[q])) { sample_rows[nsample++] = (int)i; break; }

    ST s = st_open_dir(dir);
    Tensor te = st_get(&s, "model.embed_tokens.weight");
    Tensor tu = st_has(&s, "lm_head.weight") ? st_get(&s, "lm_head.weight") : te;
    Tensor tn = st_get(&s, "model.norm.weight");
    float *E = to_f32(&te), *U = to_f32(&tu), *gf = to_f32(&tn); size_t NT = te.shape[0];
    if (zmin <= 0.0f) zmin = do_ingest ? (float)sqrt(2.0 * log((double)(NT > 2 ? NT : 2))) : 3.0f;
    printf("config: hidden %zu, heads %zu (kv %zu, dim %zu), layers %zu (running %d), ffn %zu; vocabulary %zu: text %zu, byte %zu, special %zu; zmin %.3f\n",
           D, NH, NKV, HD, NL, layers, FF, NV, ntext, nbyte, nspec, zmin);
    printf("loaded embeddings and unembedding (%s, %s) in %.2f s\n\n", te.dtype, st_has(&s, "lm_head.weight") ? "untied" : "tied", now() - t);

    File mf; memset(&mf, 0, sizeof mf);
    Ref *crecs = NULL; size_t ncrecs = 0, ccap = 0;
    if (do_ingest) {
        if (!wname || !*wname) { const char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\'); if (bs && (!sl || bs > sl)) sl = bs; wname = (sl && sl[1]) ? sl + 1 : dir; }
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

    size_t hcap = NT * cap; lp_rowsig_hit *hits = malloc(sizeof(lp_rowsig_hit) * hcap); lp_rowsig_stats st;
    float *X = malloc(sizeof(float) * NT * D), *Ug = malloc(sizeof(float) * NT * D);
    /* unembedding with the final norm's gain folded in: logits = (x / rms(x)) . (g * u) */
    #pragma omp parallel for
    for (size_t i = 0; i < NT; i++) for (size_t k = 0; k < D; k++) Ug[i * D + k] = U[i * D + k] * gf[k];

    printf("== circuits (z = score's distance above its row's mean, in the row's standard deviations)\n");
    memcpy(X, E, sizeof(float) * NT * D); unit_rows(X, NT, D);
    t = now(); size_t nh = lp_rowsig(X, NT, X, NT, D, 1.0f, zmin, cap, hits, hcap, &st); report("embed", &st, now() - t, hits, nh, 1);
    if (do_ingest) crecs[ncrecs++] = record_circuit_hits(&mf, "embed", hits, nh, zmin);
    rmsnorm_rows(E, X, NT, D, NULL, eps);
    t = now(); nh = lp_rowsig(X, NT, Ug, NT, D, 1.0f, zmin, cap, hits, hcap, &st); report("direct", &st, now() - t, hits, nh, 1);
    if (do_ingest) crecs[ncrecs++] = record_circuit_hits(&mf, "direct", hits, nh, zmin);

    float *Q = malloc(sizeof(float) * NT * D), *K = malloc(sizeof(float) * NT * NKV * HD), *Vv = malloc(sizeof(float) * NT * NKV * HD);
    float *Ah = malloc(sizeof(float) * NT * HD), *Bh = malloc(sizeof(float) * NT * HD), *Mh = malloc(sizeof(float) * NT * HD);
    float *Wo_h = malloc(sizeof(float) * D * HD), *WdT = malloc(sizeof(float) * FF * D);
    lp_rowsig_hit *fh = malloc(sizeof(lp_rowsig_hit) * FF * cap);
    for (int L = 0; L < layers; L++) {
        char nm[256], cname[64]; double tl = now();
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
            snprintf(cname, sizeof cname, "L%d.H%zu.qk", L, h);
            if (L == 0 && h < 2) { report(cname, &st, now() - t, hits, nh, 1); FLOPS_TOTAL -= st.flops; SECS_TOTAL -= now() - t; }
            if (do_ingest && nh) crecs[ncrecs++] = record_circuit_hits(&mf, cname, hits, nh, zmin);
            /* OV: attending to token a promotes b. Low rank: (V_a W_O,h) . Ug_b = V_a . (Ug_b W_O,h), both of width HD. */
            cols(Vv, NT, NKV * HD, kv * HD, HD, Ah); cols(Wo, D, D, h * HD, HD, Wo_h);
            cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, (MKL_INT)NT, (MKL_INT)HD, (MKL_INT)D, 1.0f, Ug, (MKL_INT)D, Wo_h, (MKL_INT)HD, 0.0f, Mh, (MKL_INT)HD);
            t = now(); nh = lp_rowsig(Ah, NT, Mh, NT, HD, 1.0f, zmin, cap, hits, hcap, &st); so += now() - t; fo += st.flops; a4o += st.above[1];
            snprintf(cname, sizeof cname, "L%d.H%zu.ov", L, h);
            if (L == 0 && h < 2) { report(cname, &st, now() - t, hits, nh, 1); FLOPS_TOTAL -= st.flops; SECS_TOTAL -= now() - t; }
            if (do_ingest && nh) crecs[ncrecs++] = record_circuit_hits(&mf, cname, hits, nh, zmin);
        }
        FLOPS_TOTAL += fq + fo; SECS_TOTAL += sq + so;
        free(Wq); free(Wk); free(Wv); free(Wo);
        /* FFN: connected SwiGLU (gate_proj + up_proj + down_proj) token-to-token + neuron direction readout */
        snprintf(nm, sizeof nm, "model.layers.%d.mlp.down_proj.weight", L); Tensor td = st_get(&s, nm); float *Wd = to_f32(&td);
        snprintf(nm, sizeof nm, "model.layers.%d.post_attention_layernorm.weight", L);
        char ng_nm[256], nu_nm[256];
        snprintf(ng_nm, sizeof ng_nm, "model.layers.%d.mlp.gate_proj.weight", L);
        snprintf(nu_nm, sizeof nu_nm, "model.layers.%d.mlp.up_proj.weight", L);
        if (st_has(&s, nm) && st_has(&s, ng_nm) && st_has(&s, nu_nm)) {
            Tensor tpn = st_get(&s, nm), twg = st_get(&s, ng_nm), twu = st_get(&s, nu_nm);
            float *g_post = to_f32(&tpn), *Wg = to_f32(&twg), *Wu = to_f32(&twu);
            rmsnorm_rows(E, X, NT, D, g_post, eps); free(g_post);
            ffn_connected(X, Wg, Wu, Wd, Q, NT, D, FF);
            free(Wg); free(Wu);
            t = now(); nh = lp_rowsig(Q, NT, Ug, NT, D, 1.0f, zmin, cap, hits, hcap, &st);
            FLOPS_TOTAL += st.flops; SECS_TOTAL += now() - t;
            snprintf(cname, sizeof cname, "L%d.ffn", L);
            if (L == 0) { report(cname, &st, now() - t, hits, nh, 1); FLOPS_TOTAL -= st.flops; SECS_TOTAL -= now() - t; }
            if (do_ingest && nh) crecs[ncrecs++] = record_circuit_hits(&mf, cname, hits, nh, zmin);
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
        if (rc_ || load_register(conninfo, 2, &lw, 1, &mf, 1) || merge(conninfo, 2, &ls)) { free(crecs); free(mf.ev.e); return 1; }
        char wid[33]; lp_id_hex(&mf.trunk.id, wid);
        printf("ingested witness %s (%s, trust %.2f): %zu claims attested, %llu entities new, raw_weight_bytes_retained = 0   (%.2f s)\n",
               wid, wname, mf.trust, mf.ev.n, (unsigned long long)ls.ent_rows, now() - ti);
        free(crecs); free(mf.ev.e);
    }
    return 0;
}
