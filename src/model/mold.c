#include "codecs.h"
#include "gguf_writer.h"
#include "laplace/linalg.h"
#include "laplace/os.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mkl.h>
#include <mkl_lapacke.h>

/* ---- CPU Mold-a-Model (Sequence 25 / 26):
 * Synthesizes clean Llama-architecture GGUFs (laplace-blake3-v1) without gradient descent:
 *   1. Open Merkle BLAKE3-128 vocabulary (laplace.vocab.blake3_ids + tokenizer.ggml.tokens)
 *   2. Integer-conserved Fisher attribution (SUM(share_bp) == 10000 via Hamilton apportionment, lineage dedup)
 *   3. Full-rank multi-plane spectral embeddings (4D canonical coordinates + normalized Laplacian eigenmaps
 *      + Householder QR lp_gram_schmidt + orthogonal Procrustes lp_procrustes + RMS slack, strictly rank > 4)
 *   4. Deterministic SVD/least-squares attention (Q/K/V/O), SwiGLU FFN, and log-target lm_head readout. */

typedef struct {
    char  *key;
    double mass;
} LineageMass;

/* Hamilton (largest-remainder) apportionment guaranteeing exact SUM(share_bp) == 10000. */
static void compute_fisher_bp(const LpMoldSpec *spec, char ***witnesses_out,
                              int32_t **bp_out, size_t *count_out){
    size_t nl = 0, cap = 8;
    LineageMass *lm = malloc(sizeof *lm * cap);
    lp_strmap *seen = lp_strmap_sized(sizeof(size_t));

    if (spec->n_edges == 0 || !spec->edge_witnesses) {
        lm[0] = (LineageMass){ strdup(spec->name ? spec->name : "Laplace"), 1.0 };
        nl = 1;
    } else {
        for (size_t i = 0; i < spec->n_edges; i++) {
            const char *k = (spec->edge_lineages && spec->edge_lineages[i])
                ? spec->edge_lineages[i]
                : (spec->edge_witnesses[i] ? spec->edge_witnesses[i] : "Laplace");
            double w = spec->weights && spec->weights[i] > 0.0 ? spec->weights[i] : 0.0;
            bool fresh = false;
            size_t *slot = lp_strmap_get(seen, k, strlen(k), &fresh);
            if (fresh) {
                if (nl == cap) { cap *= 2u; lm = xrealloc(lm, sizeof *lm * cap); }
                *slot = nl;
                lm[nl++] = (LineageMass){ strdup(k), w };
            } else {
                lm[*slot].mass += w;
            }
        }
    }
    lp_strmap_free(seen);

    double total = 0.0;
    for (size_t i = 0; i < nl; i++) total += lm[i].mass;
    if (total <= 0.0) {
        for (size_t i = 0; i < nl; i++) lm[i].mass = 1.0;
        total = (double)nl;
    }

    int32_t *bp = calloc(nl, sizeof(int32_t));
    double *rem = malloc(sizeof(double) * nl);
    int32_t sum = 0;
    for (size_t i = 0; i < nl; i++) {
        double exact = 10000.0 * lm[i].mass / total;
        bp[i] = (int32_t)floor(exact);
        rem[i] = exact - (double)bp[i];
        sum += bp[i];
    }
    int32_t left = 10000 - sum;
    while (left > 0) {
        size_t best = 0;
        for (size_t i = 1; i < nl; i++) if (rem[i] > rem[best]) best = i;
        bp[best]++;
        rem[best] = -1.0;
        left--;
    }
    free(rem);

    char **names = malloc(sizeof(char *) * nl);
    for (size_t i = 0; i < nl; i++) names[i] = lm[i].key;
    free(lm);
    *witnesses_out = names;
    *bp_out = bp;
    *count_out = nl;
}

/* Compute dense SVD rank of M (rows x cols, row-major f32) for diagnostic verification. */
static size_t measure_matrix_rank(const float *M, size_t rows, size_t cols){
    size_t k = rows < cols ? rows : cols;
    if (!k) return 0;
    double *a = malloc(sizeof(double) * rows * cols);
    double *s = malloc(sizeof(double) * k);
    double *u = malloc(sizeof(double) * rows);
    double *vt = malloc(sizeof(double) * cols);
    for (size_t i = 0; i < rows * cols; i++) a[i] = (double)M[i];
    int rc = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'N', (lapack_int)rows, (lapack_int)cols,
                            a, (lapack_int)cols, s, u, 1, vt, (lapack_int)cols);
    size_t rank = 0;
    if (rc == 0 && s[0] > 0.0) {
        double tol = s[0] * 1e-6;
        for (size_t i = 0; i < k; i++) if (s[i] > tol) rank++;
    }
    free(a); free(s); free(u); free(vt);
    return rank;
}

static void build_spectral_embedding(const LpMoldSpec *spec, size_t n, size_t D,
                                     const double *anchors4d, float *E_out, size_t *rank_out){
    size_t target_rank = D - 2u;
    if (target_rank > n) target_rank = n;
    double *vecs = calloc(target_rank * n, sizeof(double)); /* target_rank rows of length n */

    /* Axes 0..3: deterministic 4D spatial coordinates */
    for (size_t c = 0; c < 4 && c < target_rank; c++) {
        for (size_t i = 0; i < n; i++) vecs[c * n + i] = anchors4d[i * 4u + c];
    }

    size_t filled = target_rank < 4u ? target_rank : 4u;

    /* Spectral eigenmap modes from symmetric positive trajectory graph (via Laplace::linalg) */
    if (filled < target_rank && n <= 1024u && n >= 4u && spec->n_edges > 0) {
        double *W = calloc(n * n, sizeof(double));
        for (size_t e = 0; e < spec->n_edges; e++) {
            int r = spec->rows[e], c = spec->cols[e];
            double w = spec->weights ? spec->weights[e] : 1.0;
            if (r >= 0 && c >= 0 && (size_t)r < n && (size_t)c < n && r != c && w > 0.0) {
                W[(size_t)r * n + (size_t)c] += w;
                W[(size_t)c * n + (size_t)r] += w;
            }
        }
        /* Ensure weak chain connectivity so Laplacian has zero isolated zero-degree collapse */
        for (size_t i = 0; i + 1 < n; i++) {
            W[i * n + (i + 1)] += 1e-3;
            W[(i + 1) * n + i] += 1e-3;
        }
        size_t want_k = target_rank - filled;
        if (want_k > n - 2u) want_k = n - 2u;
        if (want_k >= 1u) {
            double *Y = malloc(sizeof(double) * n * want_k);
            double *lam = malloc(sizeof(double) * want_k);
            if (lp_eigenmap(W, n, want_k, Y, lam) == 0) {
                for (size_t k = 0; k < want_k && filled < target_rank; k++) {
                    for (size_t i = 0; i < n; i++) vecs[filled * n + i] = Y[i * want_k + k];
                    filled++;
                }
            }
            free(Y); free(lam);
        }
        free(W);
    }

    /* Fill remaining feature planes from multi-plane structural Fourier/trajectory modes */
    while (filled < target_rank) {
        size_t mode = filled;
        for (size_t i = 0; i < n; i++) {
            double ph = 2.0 * 3.14159265358979323846 * (double)((i * (mode + 1u)) % n) / (double)n;
            vecs[filled * n + i] = (mode & 1u) ? cos(ph + (double)mode * 0.13) : sin(ph + (double)mode * 0.17);
        }
        filled++;
    }

    /* Householder QR orthonormalization across rows (columns of embedding E) */
    if (lp_gram_schmidt(vecs, target_rank, n) != 0) {
        /* Fallback deterministic orthonormal basis */
        for (size_t c = 0; c < target_rank; c++) {
            memset(vecs + c * n, 0, sizeof(double) * n);
            vecs[c * n + (c % n)] = 1.0;
        }
    }

    /* Orient the first 4 orthonormal axes against canonical 4D anchors via Procrustes */
    if (target_rank >= 4u) {
        double *A4 = malloc(sizeof(double) * n * 4u);
        for (size_t i = 0; i < n; i++) for (size_t c = 0; c < 4; c++) A4[i * 4u + c] = vecs[c * n + i];
        double R4[16], scale = 1.0;
        if (lp_procrustes(A4, anchors4d, n, 4u, R4, &scale) == 0) {
            double *oriented = malloc(sizeof(double) * n * 4u);
            cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                        (MKL_INT)n, 4, 4, 1.0, A4, 4, R4, 4, 0.0, oriented, 4);
            for (size_t c = 0; c < 4; c++) for (size_t i = 0; i < n; i++) vecs[c * n + i] = oriented[i * 4u + c];
            free(oriented);
        }
        free(A4);
    }

    /* Pack (n x D) with RMS normalization slack in dims D-2, D-1 (matching basis.cpp) */
    double *ed = calloc(n * D, sizeof(double));
    double max_ss = 0.0;
    for (size_t i = 0; i < n; i++) {
        double ss = 0.0;
        for (size_t c = 0; c < target_rank; c++) {
            double v = vecs[c * n + i];
            ed[i * D + c] = v;
            ss += v * v;
        }
        if (ss > max_ss) max_ss = ss;
    }
    double common = max_ss + 1e-12;
    double scale = sqrt((double)(D - 1u) / common);
    for (size_t i = 0; i < n; i++) {
        double ss = 0.0;
        for (size_t c = 0; c < target_rank; c++) {
            double v = ed[i * D + c];
            ss += v * v;
            E_out[i * D + c] = (float)(v * scale);
        }
        for (size_t c = target_rank; c + 2u < D; c++) E_out[i * D + c] = 0.0f;
        E_out[i * D + D - 2u] = (float)(sqrt(common > ss ? common - ss : 0.0) * scale);
        E_out[i * D + D - 1u] = 1.0f;
    }

    free(ed);
    free(vecs);
    *rank_out = measure_matrix_rank(E_out, n, D);
}

static void rows_rmsnorm_f32(const float *x, float *y, size_t n, size_t d, double eps){
    for (size_t i = 0; i < n; i++) {
        double ss = 0.0;
        for (size_t c = 0; c < d; c++) ss += (double)x[i * d + c] * x[i * d + c];
        double inv = 1.0 / sqrt(ss / (double)d + eps);
        for (size_t c = 0; c < d; c++) y[i * d + c] = (float)(x[i * d + c] * inv);
    }
}

/* Compute least-squares readout head W (V x D) fitting log-transition distribution over X (V x D). */
static void synthesize_readout(const LpMoldSpec *spec, const float *X, size_t n, size_t D, float *head_out){
    double *tgt = malloc(sizeof(double) * n * n);
    double *row_sum = calloc(n, sizeof(double));
    for (size_t e = 0; e < spec->n_edges; e++) {
        int r = spec->rows[e], c = spec->cols[e];
        double w = spec->weights ? spec->weights[e] : 1.0;
        if (r >= 0 && c >= 0 && (size_t)r < n && (size_t)c < n && w > 0.0) {
            row_sum[r] += w;
        }
    }
    double floor_log = log(1e-3 / (double)n);
    for (size_t i = 0; i < n * n; i++) tgt[i] = floor_log;
    for (size_t i = 0; i < n; i++) {
        if (row_sum[i] <= 0.0) tgt[i * n + (size_t)spec->eos_id] = 1.0;
    }
    for (size_t e = 0; e < spec->n_edges; e++) {
        int r = spec->rows[e], c = spec->cols[e];
        double w = spec->weights ? spec->weights[e] : 1.0;
        if (r >= 0 && c >= 0 && (size_t)r < n && (size_t)c < n && w > 0.0 && row_sum[r] > 0.0) {
            tgt[(size_t)r * n + (size_t)c] = log(w / row_sum[r] * (double)n) + 1.0;
        }
    }
    free(row_sum);

    /* Solve min ||X * W^T - tgt||_F via SVD of X (n x D) */
    size_t k = n < D ? n : D;
    double *A = malloc(sizeof(double) * n * D);
    double *U = malloc(sizeof(double) * n * k);
    double *S = malloc(sizeof(double) * k);
    double *Vt = malloc(sizeof(double) * k * D);
    for (size_t i = 0; i < n * D; i++) A[i] = (double)X[i];

    int rc = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'S', (lapack_int)n, (lapack_int)D,
                            A, (lapack_int)D, S, U, (lapack_int)k, Vt, (lapack_int)D);
    if (rc == 0) {
        double *UtT = calloc(k * n, sizeof(double));
        cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans, (MKL_INT)k, (MKL_INT)n, (MKL_INT)n,
                    1.0, U, (MKL_INT)k, tgt, (MKL_INT)n, 0.0, UtT, (MKL_INT)n);
        double tol = S[0] * 1e-6;
        for (size_t z = 0; z < k; z++) {
            double inv = S[z] > tol ? 1.0 / S[z] : 0.0;
            for (size_t j = 0; j < n; j++) UtT[z * n + j] *= inv;
        }
        double *Wd = calloc(D * n, sizeof(double));
        cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans, (MKL_INT)D, (MKL_INT)n, (MKL_INT)k,
                    1.0, Vt, (MKL_INT)D, UtT, (MKL_INT)n, 0.0, Wd, (MKL_INT)n);
        for (size_t i = 0; i < n; i++) for (size_t c = 0; c < D; c++) head_out[i * D + c] = (float)Wd[c * n + i];
        free(Wd); free(UtT);
    } else {
        memcpy(head_out, X, sizeof(float) * n * D);
    }
    free(A); free(U); free(S); free(Vt);
    free(tgt);
}

static int add_f32_or_f16(lp_gguf_writer *w, const char *name, const uint64_t *dims, uint32_t ndim,
                          const float *f32, int use_f16){
    if (!use_f16) return lp_gguf_writer_add_tensor(w, name, LP_DTYPE_F32, dims, ndim, f32);
    size_t nel = 1u;
    for (uint32_t i = 0; i < ndim; i++) nel *= (size_t)dims[i];
    uint16_t *f16 = malloc(sizeof(uint16_t) * nel);
    for (size_t i = 0; i < nel; i++) f16[i] = lp_f32_to_f16(f32[i]);
    int rc = lp_gguf_writer_add_tensor(w, name, LP_DTYPE_F16, dims, ndim, f16);
    free(f16);
    return rc;
}

int lp_mold_export(const LpMoldSpec *spec){
    if (!spec || !spec->out_path || !spec->tokens || spec->n_vocab < 4u
        || spec->hidden_dim < 8u || !spec->n_layers || !spec->n_heads || !spec->n_kv_heads
        || (spec->n_heads % spec->n_kv_heads) != 0
        || (spec->hidden_dim % spec->n_heads) != 0
        || ((spec->hidden_dim / spec->n_heads) % 2u) != 0) {
        return -1;
    }
    if (!lp_mkl_reproducible()) return -1;

    const size_t n = spec->n_vocab, D = spec->hidden_dim, NL = spec->n_layers;
    const size_t NH = spec->n_heads, NKV = spec->n_kv_heads, HD = D / NH;
    const size_t FF = spec->ffn_dim ? spec->ffn_dim : (D * 4u);
    const double eps = spec->rms_eps > 0.0 ? spec->rms_eps : 1e-5;
    const double rope_base = spec->rope_base > 0.0 ? spec->rope_base : 10000.0;

    const lp_tier0_record *t0 = T0 ? T0 : lp_tier0_map(NULL);
    if (!t0) return -1;
    lp_text *tx = lp_text_new(t0);
    if (!tx) return -1;

    uint8_t *ids16 = malloc(16u * n);
    double *anchors = malloc(sizeof(double) * 4u * n);
    for (size_t i = 0; i < n; i++) {
        const char *ts = spec->tokens[i] ? spec->tokens[i] : "";
        lp_ref tr = lp_text_decompose(tx, (const uint8_t *)ts, strlen(ts), NULL, NULL);
        if (spec->token_ids) {
            static const lp_id zero = { { 0 } };
            lp_id id = lp_id_eq(&spec->token_ids[i], &zero) ? tr.id : spec->token_ids[i];
            memcpy(ids16 + i * 16u, id.b, 16u);
        } else {
            memcpy(ids16 + i * 16u, tr.id.b, 16u);
        }
        if (spec->coords4d) {
            for (size_t c = 0; c < 4; c++) anchors[i * 4u + c] = spec->coords4d[i * 4u + c];
        } else {
            double c4[4];
            lp_coord_xyzm(&tr.c, c4);
            for (size_t c = 0; c < 4; c++) anchors[i * 4u + c] = c4[c];
        }
    }
    lp_text_free(tx);

    char **witnesses = NULL;
    int32_t *share_bp = NULL;
    size_t n_witness = 0;
    compute_fisher_bp(spec, &witnesses, &share_bp, &n_witness);

    float *E = calloc(n * D, sizeof(float));
    float *X = calloc(n * D, sizeof(float));
    float *head = calloc(n * D, sizeof(float));
    size_t rank = 0;
    build_spectral_embedding(spec, n, D, anchors, E, &rank);
    rows_rmsnorm_f32(E, X, n, D, eps);
    synthesize_readout(spec, X, n, D, head);
    free(anchors);

    lp_gguf_writer *gw = lp_gguf_writer_create(spec->out_path);
    if (!gw) {
        free(ids16); free(E); free(X); free(head);
        for (size_t i = 0; i < n_witness; i++) free(witnesses[i]);
        free(witnesses); free(share_bp);
        return -1;
    }

    const char *mname = (spec->name && *spec->name) ? spec->name : "laplace-molded";
    int rc = 0;
    rc |= lp_gguf_writer_add_str(gw, "general.architecture", "llama");
    rc |= lp_gguf_writer_add_str(gw, "general.name", mname);
    rc |= lp_gguf_writer_add_u32(gw, "general.file_type", spec->f16_weights ? 1u : 0u);
    rc |= lp_gguf_writer_add_str(gw, "tokenizer.ggml.model", "laplace-blake3-v1");
    rc |= lp_gguf_writer_add_str_array(gw, "tokenizer.ggml.tokens", spec->tokens, n);
    rc |= lp_gguf_writer_add_u32(gw, "tokenizer.ggml.bos_token_id", (uint32_t)spec->bos_id);
    rc |= lp_gguf_writer_add_u32(gw, "tokenizer.ggml.eos_token_id", (uint32_t)spec->eos_id);
    rc |= lp_gguf_writer_add_u8_array(gw, "laplace.vocab.blake3_ids", ids16, 16u * n);
    rc |= lp_gguf_writer_add_u32(gw, "laplace.mold.embedding_rank", (uint32_t)rank);
    rc |= lp_gguf_writer_add_u32(gw, "laplace.mold.fisher_total_bp", 10000u);
    rc |= lp_gguf_writer_add_str_array(gw, "laplace.mold.witnesses", (const char *const *)witnesses, n_witness);
    rc |= lp_gguf_writer_add_i32_array(gw, "laplace.mold.share_bp", share_bp, n_witness);
    rc |= lp_gguf_writer_add_u64(gw, "laplace.mold.raw_weight_bytes_retained", 0u);
    rc |= lp_gguf_writer_add_u32(gw, "llama.vocab_size", (uint32_t)n);
    rc |= lp_gguf_writer_add_u32(gw, "llama.embedding_length", (uint32_t)D);
    rc |= lp_gguf_writer_add_u32(gw, "llama.block_count", (uint32_t)NL);
    rc |= lp_gguf_writer_add_u32(gw, "llama.attention.head_count", (uint32_t)NH);
    rc |= lp_gguf_writer_add_u32(gw, "llama.attention.head_count_kv", (uint32_t)NKV);
    rc |= lp_gguf_writer_add_u32(gw, "llama.feed_forward_length", (uint32_t)FF);
    rc |= lp_gguf_writer_add_u32(gw, "llama.context_length", 2048u);
    rc |= lp_gguf_writer_add_u32(gw, "llama.rope.dimension_count", (uint32_t)HD);
    rc |= lp_gguf_writer_add_f32(gw, "llama.rope.freq_base", (float)rope_base);
    rc |= lp_gguf_writer_add_f32(gw, "llama.attention.layer_norm_rms_epsilon", (float)eps);

    uint64_t emb_dims[2] = { (uint64_t)D, (uint64_t)n };
    rc |= add_f32_or_f16(gw, "token_embd.weight", emb_dims, 2, E, spec->f16_weights);

    /* Precompute Layer Operator Q/K/V/O & SwiGLU matrices from normalized multi-plane geometry */
    float *ones = malloc(sizeof(float) * D);
    for (size_t c = 0; c < D; c++) ones[c] = 1.0f;

    size_t qdim = NH * HD, kdim = NKV * HD;
    float *wq = calloc(qdim * D, sizeof(float));
    float *wk = calloc(kdim * D, sizeof(float));
    float *wv = calloc(kdim * D, sizeof(float));
    float *wo = calloc(D * qdim, sizeof(float));
    float *fg = calloc(FF * D, sizeof(float));
    float *fu = calloc(FF * D, sizeof(float));
    float *fd = calloc(D * FF, sizeof(float));

    /* Compile affinity operator (X^T * A_norm * X) in D x D so attention tracks observed edges */
    double *op_score = calloc(D * D, sizeof(double));
    for (size_t e = 0; e < spec->n_edges; e++) {
        int r = spec->rows[e], c = spec->cols[e];
        double w = spec->weights ? spec->weights[e] : 0.0;
        if (r >= 0 && c >= 0 && (size_t)r < n && (size_t)c < n && w > 0.0) {
            double gain = w / (double)(spec->n_edges > 0 ? spec->n_edges : 1u);
            for (size_t i = 0; i < D; i++) {
                double xi = (double)X[(size_t)r * D + i];
                for (size_t j = 0; j < D; j++) op_score[i * D + j] += gain * xi * (double)X[(size_t)c * D + j];
            }
        }
    }
    for (size_t c = 0; c < D; c++) op_score[c * D + c] += 1.0 / (double)NL;

    double split = pow((double)NL, -0.25);
    for (size_t c = 0; c < qdim && c < D; c++) wq[c * D + c] = (float)(0.25 * split);
    for (size_t c = 0; c < kdim && c < D; c++) wk[c * D + c] = (float)(0.25 * split);
    for (size_t r = 0; r < kdim && r < D; r++) for (size_t c = 0; c < D; c++) wv[r * D + c] = (float)(0.05 * split * op_score[r * D + c]);
    for (size_t c = 0; c < D && c < qdim; c++) wo[c * qdim + c] = (float)(0.25 * split);

    /* Constant gate on bias channel D-1 + residual operator directions in SwiGLU */
    for (size_t z = 0; z < FF; z++) {
        fg[z * D + (D - 1u)] = 6.0f;
        size_t col = z % (D - 2u > 0 ? D - 2u : 1u);
        fu[z * D + col] = (float)(0.1 * split);
        fd[col * FF + z] = (float)(0.1 * split);
    }
    free(op_score);

    for (size_t L = 0; L < NL; L++) {
        char tname[128];
        uint64_t norm_d[1] = { (uint64_t)D };
        uint64_t qd[2] = { (uint64_t)D, (uint64_t)qdim };
        uint64_t kd[2] = { (uint64_t)D, (uint64_t)kdim };
        uint64_t od[2] = { (uint64_t)qdim, (uint64_t)D };
        uint64_t fgd[2] = { (uint64_t)D, (uint64_t)FF };
        uint64_t fdd[2] = { (uint64_t)FF, (uint64_t)D };

        snprintf(tname, sizeof tname, "blk.%zu.attn_norm.weight", L);
        rc |= lp_gguf_writer_add_tensor(gw, tname, LP_DTYPE_F32, norm_d, 1, ones);
        snprintf(tname, sizeof tname, "blk.%zu.attn_q.weight", L);
        rc |= add_f32_or_f16(gw, tname, qd, 2, wq, spec->f16_weights);
        snprintf(tname, sizeof tname, "blk.%zu.attn_k.weight", L);
        rc |= add_f32_or_f16(gw, tname, kd, 2, wk, spec->f16_weights);
        snprintf(tname, sizeof tname, "blk.%zu.attn_v.weight", L);
        rc |= add_f32_or_f16(gw, tname, kd, 2, wv, spec->f16_weights);
        snprintf(tname, sizeof tname, "blk.%zu.attn_output.weight", L);
        rc |= add_f32_or_f16(gw, tname, od, 2, wo, spec->f16_weights);
        snprintf(tname, sizeof tname, "blk.%zu.ffn_norm.weight", L);
        rc |= lp_gguf_writer_add_tensor(gw, tname, LP_DTYPE_F32, norm_d, 1, ones);
        snprintf(tname, sizeof tname, "blk.%zu.ffn_gate.weight", L);
        rc |= add_f32_or_f16(gw, tname, fgd, 2, fg, spec->f16_weights);
        snprintf(tname, sizeof tname, "blk.%zu.ffn_up.weight", L);
        rc |= add_f32_or_f16(gw, tname, fgd, 2, fu, spec->f16_weights);
        snprintf(tname, sizeof tname, "blk.%zu.ffn_down.weight", L);
        rc |= add_f32_or_f16(gw, tname, fdd, 2, fd, spec->f16_weights);
    }

    uint64_t out_norm_d[1] = { (uint64_t)D };
    rc |= lp_gguf_writer_add_tensor(gw, "output_norm.weight", LP_DTYPE_F32, out_norm_d, 1, ones);
    rc |= add_f32_or_f16(gw, "output.weight", emb_dims, 2, head, spec->f16_weights);

    rc |= lp_gguf_writer_finalize(gw);
    lp_gguf_writer_free(gw);

    printf("exported %s: vocab %zu, dim %zu, rank %zu, fisher_bp 10000 (%zu witnesses), raw_weight_bytes_retained = 0\n",
           spec->out_path, n, D, rank, n_witness);

    free(ones); free(wq); free(wk); free(wv); free(wo); free(fg); free(fu); free(fd);
    free(ids16); free(E); free(X); free(head);
    for (size_t i = 0; i < n_witness; i++) free(witnesses[i]);
    free(witnesses); free(share_bp);
    return rc ? 1 : 0;
}
