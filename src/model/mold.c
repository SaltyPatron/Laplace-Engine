#include "codecs.h"
#include "gguf_writer.h"
#include "safetensors_writer.h"
#include "laplace/linalg.h"
#include "laplace/os.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mkl.h>
#include <mkl_lapacke.h>

/* ---- CPU Mold-a-Model (Sequence 25 / 26):
 * Synthesizes clean Llama-architecture GGUF or SafeTensors checkpoints (laplace-blake3-v1) without gradient descent:
 *   1. Open Merkle BLAKE3-128 vocabulary (laplace.vocab.blake3_ids + tokenizer.ggml.tokens)
 *   2. Integer-conserved Fisher attribution (SUM(share_bp) == 10000 via Hamilton apportionment, lineage dedup)
 *   3. Full-rank multi-plane spectral embeddings (4D canonical coords + directed PMI SVD + normalized Laplacian
 *      eigenmaps + ordered tier0.bin UTF-8 codepoint moments + Householder QR + Procrustes, strictly rank > 4)
 *   4. Per-layer, per-head deterministic SVD/least-squares attention (Q/K/V/O) + SwiGLU FFN and log-target lm_head. */

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

/* Full-rank multi-plane embedding: 4D coords + directed PMI SVD + Laplacian eigenmaps + ordered codepoint moments. */
static void build_spectral_embedding(const LpMoldSpec *spec, const lp_tier0_record *t0,
                                     size_t n, size_t D, const double *anchors4d,
                                     const uint8_t *ids16, float *E_out, size_t *rank_out){
    size_t target_rank = D - 2u;
    if (target_rank > n) target_rank = n;
    double *vecs = calloc(target_rank * n, sizeof(double));

    /* Plane 1 (axes 0..3): deterministic 4D canonical spatial coordinates */
    for (size_t c = 0; c < 4 && c < target_rank; c++) {
        for (size_t i = 0; i < n; i++) vecs[c * n + i] = anchors4d[i * 4u + c];
    }
    size_t filled = target_rank < 4u ? target_rank : 4u;

    /* Plane 2: asymmetric directed trajectory context SVD (left U + right V singular modes of PMI(P)) */
    if (filled < target_rank && n <= 1024u && n >= 4u && spec->n_edges > 0) {
        double *P = calloc(n * n, sizeof(double));
        double *row_sum = calloc(n, sizeof(double));
        double *col_p = calloc(n, sizeof(double));
        for (size_t e = 0; e < spec->n_edges; e++) {
            int r = spec->rows[e], c = spec->cols[e];
            double w = spec->weights ? spec->weights[e] : 1.0;
            if (r >= 0 && c >= 0 && (size_t)r < n && (size_t)c < n && w > 0.0) {
                P[(size_t)r * n + (size_t)c] += w;
                row_sum[r] += w;
            }
        }
        for (size_t r = 0; r < n; r++) {
            double denom = row_sum[r] > 0.0 ? row_sum[r] : (double)n;
            for (size_t c = 0; c < n; c++) {
                double p = row_sum[r] > 0.0 ? (P[r * n + c] / denom) : (1.0 / (double)n);
                P[r * n + c] = p;
                col_p[c] += p / (double)n;
            }
        }
        for (size_t r = 0; r < n; r++) {
            for (size_t c = 0; c < n; c++) {
                P[r * n + c] = log((P[r * n + c] + 1e-4) / (col_p[c] + 1e-4));
            }
        }
        size_t want_pmi = (target_rank - filled) / 2u;
        if (want_pmi == 0 && (target_rank - filled) > 0) want_pmi = 1u;
        double *U = malloc(sizeof(double) * n * n);
        double *S = malloc(sizeof(double) * n);
        double *Vt = malloc(sizeof(double) * n * n);
        if (LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'S', (lapack_int)n, (lapack_int)n,
                           P, (lapack_int)n, S, U, (lapack_int)n, Vt, (lapack_int)n) == 0) {
            size_t added = 0;
            double tol = S[0] * 1e-9;
            for (size_t k = 0; k < n && added < want_pmi && filled < target_rank && S[k] > tol; k++) {
                for (size_t i = 0; i < n; i++) vecs[filled * n + i] = U[i * n + k];
                filled++; added++;
                if (added < want_pmi && filled < target_rank) {
                    for (size_t i = 0; i < n; i++) vecs[filled * n + i] = Vt[k * n + i];
                    filled++; added++;
                }
            }
        }
        free(U); free(S); free(Vt);
        free(P); free(row_sum); free(col_p);
    }

    /* Plane 3: symmetric relational Laplacian eigenmaps */
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

    /* Plane 4: ordered tier0.bin UTF-8 codepoint trajectory moments (separates anagrams deterministically) */
    if (filled < target_rank && t0) {
        lp_text *cp_tx = lp_text_new(t0);
        size_t mcap = 16;
        double *cp_c4 = malloc(sizeof(double) * 4u * mcap);
        for (size_t i = 0; i < n; i++) {
            const char *s = (spec->tokens && spec->tokens[i]) ? spec->tokens[i] : "";
            size_t slen = strlen(s), pos = 0, m = 0;
            while (pos < slen) {
                size_t p0 = pos;
                uint32_t cp = 0;
                if (!lp_utf8_next((const uint8_t *)s, slen, &pos, &cp)) break;
                if (m == mcap) { mcap *= 2u; cp_c4 = xrealloc(cp_c4, sizeof(double) * 4u * mcap); }
                lp_ref cr = lp_text_decompose(cp_tx, (const uint8_t *)(s + p0), pos - p0, NULL, NULL);
                lp_coord_xyzm(&cr.c, cp_c4 + m * 4u);
                m++;
            }
            if (m == 0) {
                cp_c4[0] = anchors4d[i * 4u + 0]; cp_c4[1] = anchors4d[i * 4u + 1];
                cp_c4[2] = anchors4d[i * 4u + 2]; cp_c4[3] = anchors4d[i * 4u + 3];
                m = 1;
            }
            for (size_t ax = filled; ax < target_rank; ax++) {
                size_t rel_ax = ax - filled;
                double val = 0.0;
                if (rel_ax < 16u) {
                    /* First 16 axes: positional power moments (r=1..4) across 4D S^3 codepoint coordinates */
                    size_t r = 1u + (rel_ax / 4u);
                    size_t d = rel_ax % 4u;
                    for (size_t p = 0; p < m; p++) {
                        double t = (2.0 * (double)p + 1.0) / (2.0 * (double)m);
                        val += pow(t, (double)r) * cp_c4[p * 4u + d];
                    }
                    val /= (double)m;
                } else if (rel_ax < 28u && m >= 2u) {
                    /* Next 12 axes: ordered consecutive codepoint bigram cross-moments */
                    size_t bi = rel_ax - 16u;
                    size_t r = bi / 4u;
                    size_t d = bi % 4u;
                    for (size_t p = 0; p + 1u < m; p++) {
                        double t = (2.0 * (double)p + 1.0) / (2.0 * (double)m);
                        val += (r ? pow(t, (double)r) : 1.0) * cp_c4[p * 4u + d] * cp_c4[(p + 1u) * 4u + ((d + 1u) % 4u)];
                    }
                    val /= (double)(m - 1u);
                } else {
                    /* Higher dimensions: deterministic BLAKE3-128 content hash bit planes (never row index!) */
                    size_t bit_idx = (rel_ax - 16u) % 128u;
                    int bit = (ids16[i * 16u + (bit_idx >> 3)] >> (bit_idx & 7u)) & 1;
                    val = bit ? 1.0 : -1.0;
                }
                vecs[ax * n + i] = val;
            }
        }
        filled = target_rank;
        free(cp_c4);
        if (cp_tx) lp_text_free(cp_tx);
    }

    /* Householder QR orthonormalization across rows (columns of embedding E) */
    if (lp_gram_schmidt(vecs, target_rank, n) != 0) {
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

    /* Pack (n x D) with RMS normalization slack in dims D-2, D-1 */
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

static int emit_tensor(lp_gguf_writer *gw, lp_st_writer *sw, const char *name,
                       const uint64_t *gguf_dims, uint32_t ndim,
                       const float *f32, int use_f16){
    size_t nel = 1u;
    for (uint32_t i = 0; i < ndim; i++) nel *= (size_t)gguf_dims[i];

    uint64_t st_dims[LP_TENSOR_MAX_DIMS];
    for (uint32_t i = 0; i < ndim; i++) st_dims[i] = gguf_dims[ndim - 1u - i];

    const void *payload = f32;
    uint16_t *f16 = NULL;
    lp_dtype dt = LP_DTYPE_F32;
    if (use_f16) {
        dt = LP_DTYPE_F16;
        f16 = malloc(sizeof(uint16_t) * nel);
        for (size_t i = 0; i < nel; i++) f16[i] = lp_f32_to_f16(f32[i]);
        payload = f16;
    }

    int rc = 0;
    if (gw) rc = lp_gguf_writer_add_tensor(gw, name, dt, gguf_dims, ndim, payload);
    else if (sw) rc = lp_st_writer_add_tensor(sw, name, dt, st_dims, ndim, payload);
    else rc = -1;

    free(f16);
    return rc;
}

/* Compute B_L = X^T * M_eff * X (D x D) and decompose via SVD into U_L * diag(S_L) * Vt_L. */
static void compile_layer_operators(size_t L, size_t NL, size_t n, size_t D, size_t NH, size_t NKV, size_t HD, size_t FF,
                                    const double *Xd, const double *M_eff,
                                    float *wq, float *wk, float *wv, float *wo,
                                    float *fg, float *fu, float *fd){
    double *MX = malloc(sizeof(double) * n * D);
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                (MKL_INT)n, (MKL_INT)D, (MKL_INT)n,
                1.0, M_eff, (MKL_INT)n, Xd, (MKL_INT)D, 0.0, MX, (MKL_INT)D);

    /* 1. Bilinear affinity B_L = X^T * MX */
    double *BL = malloc(sizeof(double) * D * D);
    cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                (MKL_INT)D, (MKL_INT)D, (MKL_INT)n,
                1.0, Xd, (MKL_INT)D, MX, (MKL_INT)D, 0.0, BL, (MKL_INT)D);
    for (size_t c = 0; c < D; c++) BL[c * D + c] += 0.05 / (double)(L + 1u);

    /* 2. Value-transport operator T_L = X^T * (M_eff - I) * X */
    double *TMX = malloc(sizeof(double) * n * D);
    for (size_t i = 0; i < n * D; i++) TMX[i] = MX[i] - Xd[i];
    double *TL = malloc(sizeof(double) * D * D);
    cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                (MKL_INT)D, (MKL_INT)D, (MKL_INT)n,
                1.0, Xd, (MKL_INT)D, TMX, (MKL_INT)D, 0.0, TL, (MKL_INT)D);
    for (size_t c = 0; c < D; c++) TL[c * D + c] += 1e-4 / (double)(L + 1u);
    free(TMX);

    /* 3. FFN residual operator R_L = MX^T * MX - X^T * MX + eps*I */
    double *RL = malloc(sizeof(double) * D * D);
    cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                (MKL_INT)D, (MKL_INT)D, (MKL_INT)n,
                1.0, MX, (MKL_INT)D, MX, (MKL_INT)D, 0.0, RL, (MKL_INT)D);
    for (size_t i = 0; i < D * D; i++) RL[i] -= (BL[i] - 0.05 / (double)(L + 1u) * 0.0);
    for (size_t c = 0; c < D; c++) RL[c * D + c] += 1e-4 / (double)(L + 1u);
    free(MX);

    double *UL = malloc(sizeof(double) * D * D), *SL = malloc(sizeof(double) * D), *VtL = malloc(sizeof(double) * D * D);
    double *Ut = malloc(sizeof(double) * D * D), *St = malloc(sizeof(double) * D), *Vtt = malloc(sizeof(double) * D * D);
    double *Uf = malloc(sizeof(double) * D * D), *Sf = malloc(sizeof(double) * D), *Vtf = malloc(sizeof(double) * D * D);

    int rcB = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'A', (lapack_int)D, (lapack_int)D, BL, (lapack_int)D, SL, UL, (lapack_int)D, VtL, (lapack_int)D);
    int rcT = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'A', (lapack_int)D, (lapack_int)D, TL, (lapack_int)D, St, Ut, (lapack_int)D, Vtt, (lapack_int)D);
    int rcR = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'A', (lapack_int)D, (lapack_int)D, RL, (lapack_int)D, Sf, Uf, (lapack_int)D, Vtf, (lapack_int)D);

    double split = pow((double)NL, -0.25);
    size_t qdim = NH * HD, kdim = NKV * HD;
    size_t l_off = (L * HD) % D;

    if (rcB == 0) {
        for (size_t h = 0; h < NH; h++) {
            for (size_t j = 0; j < HD; j++) {
                size_t r = h * HD + j;
                size_t mode = (l_off + r) % D;
                double sgn = sqrt(SL[mode] > 1e-12 ? SL[mode] : 1e-12) * 0.25 * split;
                for (size_t c = 0; c < D; c++) wq[r * D + c] = (float)(sgn * UL[c * D + mode]);
            }
        }
        size_t group = NH / NKV;
        for (size_t kv = 0; kv < NKV; kv++) {
            for (size_t j = 0; j < HD; j++) {
                size_t r = kv * HD + j;
                size_t h_rep = kv * group;
                size_t mode = (l_off + h_rep * HD + j) % D;
                double sgn = sqrt(SL[mode] > 1e-12 ? SL[mode] : 1e-12) * 0.25 * split;
                for (size_t c = 0; c < D; c++) wk[r * D + c] = (float)(sgn * VtL[mode * D + c]);
            }
        }
    } else {
        memset(wq, 0, sizeof(float) * qdim * D);
        memset(wk, 0, sizeof(float) * kdim * D);
        for (size_t c = 0; c < qdim && c < D; c++) wq[c * D + ((c + L) % D)] = (float)(0.25 * split);
        for (size_t c = 0; c < kdim && c < D; c++) wk[c * D + ((c + L) % D)] = (float)(0.25 * split);
    }

    if (rcT == 0) {
        size_t group = NH / NKV;
        for (size_t kv = 0; kv < NKV; kv++) {
            for (size_t j = 0; j < HD; j++) {
                size_t r = kv * HD + j;
                size_t mode = (l_off + (kv * group) * HD + j) % D;
                double sgn = sqrt(St[mode] > 1e-12 ? St[mode] : 1e-12) * 0.15 * split;
                for (size_t c = 0; c < D; c++) wv[r * D + c] = (float)(sgn * Vtt[mode * D + c]);
            }
        }
        for (size_t h = 0; h < NH; h++) {
            for (size_t j = 0; j < HD; j++) {
                size_t col = h * HD + j;
                size_t mode = (l_off + col) % D;
                double sgn = sqrt(St[mode] > 1e-12 ? St[mode] : 1e-12) * 0.15 * split;
                for (size_t c = 0; c < D; c++) wo[c * qdim + col] = (float)(sgn * Ut[c * D + mode]);
            }
        }
    } else {
        memset(wv, 0, sizeof(float) * kdim * D);
        memset(wo, 0, sizeof(float) * D * qdim);
        for (size_t c = 0; c < kdim && c < D; c++) wv[c * D + ((c + L) % D)] = (float)(0.15 * split);
        for (size_t c = 0; c < qdim && c < D; c++) wo[((c + L) % D) * qdim + c] = (float)(0.15 * split);
    }

    if (rcR == 0 && rcB == 0) {
        for (size_t z = 0; z < FF; z++) {
            size_t mode = (z + L * 3u) % D;
            double sf = sqrt(Sf[mode] > 1e-12 ? Sf[mode] : 1e-12) * 0.15 * split;
            for (size_t c = 0; c < D; c++) {
                fu[z * D + c] = (float)(sf * Vtf[mode * D + c]);
                fd[c * FF + z] = (float)(sf * Uf[c * D + mode]);
                fg[z * D + c] = (float)(0.5 * UL[c * D + mode]);
            }
            fg[z * D + (D - 1u)] += (float)(0.25 + 0.10 * (double)(L + 1u));
        }
    } else {
        memset(fg, 0, sizeof(float) * FF * D);
        memset(fu, 0, sizeof(float) * FF * D);
        memset(fd, 0, sizeof(float) * D * FF);
        for (size_t z = 0; z < FF; z++) {
            size_t col = (z + L) % (D - 1u > 0 ? D - 1u : 1u);
            fg[z * D + col] = 0.5f;
            fg[z * D + (D - 1u)] = (float)(0.25 + 0.10 * (double)(L + 1u));
            fu[z * D + col] = (float)(0.1 * split);
            fd[col * FF + z] = (float)(0.1 * split);
        }
    }

    free(BL); free(TL); free(RL);
    free(UL); free(SL); free(VtL);
    free(Ut); free(St); free(Vtt);
    free(Uf); free(Sf); free(Vtf);
}

int lp_mold_export(const LpMoldSpec *spec){
    if (!spec || !spec->out_path || !spec->tokens || spec->n_vocab < 4u
        || spec->hidden_dim < 8u || !spec->n_layers || !spec->n_heads || !spec->n_kv_heads
        || (spec->n_heads % spec->n_kv_heads) != 0
        || (spec->hidden_dim % spec->n_heads) != 0
        || ((spec->hidden_dim / spec->n_heads) % 2u) != 0
        || (spec->format != 0 && spec->format != 1)) {
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
    build_spectral_embedding(spec, t0, n, D, anchors, ids16, E, &rank);
    rows_rmsnorm_f32(E, X, n, D, eps);
    synthesize_readout(spec, X, n, D, head);
    free(anchors);

    lp_gguf_writer *gw = NULL;
    lp_st_writer *sw = NULL;
    if (spec->format == 0) {
        gw = lp_gguf_writer_create(spec->out_path);
        if (!gw) {
            free(ids16); free(E); free(X); free(head);
            for (size_t i = 0; i < n_witness; i++) free(witnesses[i]);
            free(witnesses); free(share_bp);
            return -1;
        }
    } else {
        sw = lp_st_writer_create(spec->out_path);
        if (!sw) {
            free(ids16); free(E); free(X); free(head);
            for (size_t i = 0; i < n_witness; i++) free(witnesses[i]);
            free(witnesses); free(share_bp);
            return -1;
        }
    }

    const char *mname = (spec->name && *spec->name) ? spec->name : "laplace-molded";
    int rc = 0;

    if (gw) {
        const char **ggml_tokens = malloc(sizeof(char *) * n);
        char **owned_tokens = calloc(n, sizeof(char *));
        float *scores = malloc(sizeof(float) * n);
        int32_t *tok_types = malloc(sizeof(int32_t) * n);
        lp_strmap *seen_tok = lp_strmap_sized(sizeof(uint8_t));
        uint32_t unk_id = (spec->bos_id != 0) ? 0u : (uint32_t)spec->eos_id;
        int found_unk = 0;
        for (size_t i = 0; i < n; i++) {
            const char *raw = (spec->tokens && spec->tokens[i] && *spec->tokens[i]) ? spec->tokens[i] : NULL;
            const char *chosen = raw;
            if (!chosen) {
                char tmp[48];
                uint64_t hi = 0;
                memcpy(&hi, ids16 + i * 16u, sizeof hi);
                snprintf(tmp, sizeof tmp, "<lp:%016llx>", (unsigned long long)hi);
                owned_tokens[i] = strdup(tmp);
                chosen = owned_tokens[i];
            }
            bool fresh = false;
            lp_strmap_get(seen_tok, chosen, strlen(chosen), &fresh);
            if (!fresh) {
                char uniq[96];
                snprintf(uniq, sizeof uniq, "%s#%zu", chosen, i);
                for (size_t salt = 0; ; salt++) {
                    bool ufresh = false;
                    lp_strmap_get(seen_tok, uniq, strlen(uniq), &ufresh);
                    if (ufresh) break;
                    snprintf(uniq, sizeof uniq, "<lp:%zu:%zu>", i, salt);
                }
                free(owned_tokens[i]);
                owned_tokens[i] = strdup(uniq);
                chosen = owned_tokens[i];
            }
            ggml_tokens[i] = chosen;
            size_t clen = strlen(chosen);
            int is_byte = (clen == 6 && chosen[0] == '<' && chosen[1] == '0' && chosen[2] == 'x' && chosen[5] == '>');
            int is_ctrl = (!strcmp(chosen, "<s>") || !strcmp(chosen, "</s>")
                           || !strcmp(chosen, "<bos>") || !strcmp(chosen, "<eos>")
                           || i == (size_t)spec->bos_id || i == (size_t)spec->eos_id);
            int is_unk = (!strcmp(chosen, "<unk>") || (spec->bos_id != 0 && i == 0u && !is_ctrl));
            if (!found_unk && !strcmp(chosen, "<unk>")) {
                unk_id = (uint32_t)i;
                found_unk = 1;
            }
            if (is_unk) {
                tok_types[i] = 2; scores[i] = 0.0f;
            } else if (is_ctrl) {
                tok_types[i] = 3; scores[i] = 0.0f;
            } else if (is_byte) {
                tok_types[i] = 6; scores[i] = 0.0f;
            } else {
                tok_types[i] = 1; scores[i] = -(float)i;
            }
        }
        lp_strmap_free(seen_tok);

        rc |= lp_gguf_writer_add_str(gw, "general.architecture", "llama");
        rc |= lp_gguf_writer_add_str(gw, "general.name", mname);
        rc |= lp_gguf_writer_add_u32(gw, "general.file_type", spec->f16_weights ? 1u : 0u);
        rc |= lp_gguf_writer_add_str(gw, "tokenizer.ggml.model", "llama");
        rc |= lp_gguf_writer_add_str_array(gw, "tokenizer.ggml.tokens", ggml_tokens, n);
        rc |= lp_gguf_writer_add_f32_array(gw, "tokenizer.ggml.scores", scores, n);
        rc |= lp_gguf_writer_add_i32_array(gw, "tokenizer.ggml.token_type", tok_types, n);
        rc |= lp_gguf_writer_add_u32(gw, "tokenizer.ggml.unk_token_id", unk_id);
        rc |= lp_gguf_writer_add_u32(gw, "tokenizer.ggml.bos_token_id", (uint32_t)spec->bos_id);
        rc |= lp_gguf_writer_add_u32(gw, "tokenizer.ggml.eos_token_id", (uint32_t)spec->eos_id);
        rc |= lp_gguf_writer_add_u32(gw, "tokenizer.ggml.padding_token_id", (uint32_t)spec->eos_id);
        rc |= lp_gguf_writer_add_bool(gw, "tokenizer.ggml.add_space_prefix", 0);
        rc |= lp_gguf_writer_add_str(gw, "laplace.tokenizer.scheme", "laplace-blake3-v1");
        rc |= lp_gguf_writer_add_u32(gw, "laplace.vocab.size", (uint32_t)n);
        rc |= lp_gguf_writer_add_u8_array(gw, "laplace.vocab.blake3_ids", ids16, 16u * n);
        for (size_t i = 0; i < n; i++) free(owned_tokens[i]);
        free(owned_tokens); free(ggml_tokens); free(scores); free(tok_types);
        rc |= lp_gguf_writer_add_u32(gw, "laplace.mold.embedding_rank", (uint32_t)rank);
        rc |= lp_gguf_writer_add_u32(gw, "laplace.mold.fisher_total_bp", 10000u);
        rc |= lp_gguf_writer_add_str_array(gw, "laplace.mold.witnesses", (const char *const *)witnesses, n_witness);
        rc |= lp_gguf_writer_add_i32_array(gw, "laplace.mold.share_bp", share_bp, n_witness);
        rc |= lp_gguf_writer_add_u64(gw, "laplace.mold.raw_weight_bytes_retained", 0u);
        rc |= lp_gguf_writer_add_bool(gw, "laplace.mold.tied_embeddings", spec->tied_embeddings ? 1 : 0);
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
    } else if (sw) {
        char numbuf[64];
        rc |= lp_st_writer_add_meta(sw, "format", "pt");
        rc |= lp_st_writer_add_meta(sw, "general.architecture", "llama");
        rc |= lp_st_writer_add_meta(sw, "general.name", mname);
        rc |= lp_st_writer_add_meta(sw, "laplace.tokenizer.scheme", "laplace-blake3-v1");
        snprintf(numbuf, sizeof numbuf, "%zu", n);
        rc |= lp_st_writer_add_meta(sw, "laplace.vocab.size", numbuf);
        snprintf(numbuf, sizeof numbuf, "%zu", rank);
        rc |= lp_st_writer_add_meta(sw, "laplace.mold.embedding_rank", numbuf);
        rc |= lp_st_writer_add_meta(sw, "laplace.mold.fisher_total_bp", "10000");
        rc |= lp_st_writer_add_meta(sw, "laplace.mold.raw_weight_bytes_retained", "0");
        rc |= lp_st_writer_add_meta(sw, "laplace.mold.tied_embeddings", spec->tied_embeddings ? "true" : "false");

        size_t wlen_est = 16;
        for (size_t i = 0; i < n_witness; i++) wlen_est += strlen(witnesses[i]) + 24u;
        char *wlist = calloc(1, wlen_est), *slist = calloc(1, n_witness * 24u + 16u);
        for (size_t i = 0; i < n_witness; i++) {
            if (i > 0) { strcat(wlist, ","); strcat(slist, ","); }
            strcat(wlist, witnesses[i]);
            char sb[24]; snprintf(sb, sizeof sb, "%d", share_bp[i]);
            strcat(slist, sb);
        }
        rc |= lp_st_writer_add_meta(sw, "laplace.mold.witnesses", wlist);
        rc |= lp_st_writer_add_meta(sw, "laplace.mold.share_bp", slist);
        free(wlist); free(slist);
    }

    uint64_t emb_dims[2] = { (uint64_t)D, (uint64_t)n };
    rc |= emit_tensor(gw, sw, gw ? "token_embd.weight" : "model.embed_tokens.weight",
                      emb_dims, 2, E, spec->f16_weights);

    /* Build directed row-normalized transition operator P (n x n) and normalized Xd (n x D) */
    double *P = calloc(n * n, sizeof(double));
    double *row_sum = calloc(n, sizeof(double));
    for (size_t e = 0; e < spec->n_edges; e++) {
        int r = spec->rows[e], c = spec->cols[e];
        double w = spec->weights ? spec->weights[e] : 1.0;
        if (r >= 0 && c >= 0 && (size_t)r < n && (size_t)c < n && w > 0.0) {
            P[(size_t)r * n + (size_t)c] += w;
            row_sum[r] += w;
        }
    }
    for (size_t r = 0; r < n; r++) {
        if (row_sum[r] <= 0.0) P[r * n + (size_t)spec->eos_id] = 1.0;
        double denom = 0.0;
        for (size_t c = 0; c < n; c++) { P[r * n + c] += 0.05 / (double)n; denom += P[r * n + c]; }
        for (size_t c = 0; c < n; c++) P[r * n + c] /= denom;
    }
    free(row_sum);

    double *Xd = malloc(sizeof(double) * n * D);
    for (size_t i = 0; i < n * D; i++) Xd[i] = (double)X[i];

    double *ML = malloc(sizeof(double) * n * n);
    double *Mnext = malloc(sizeof(double) * n * n);
    double *Meff = malloc(sizeof(double) * n * n);
    memcpy(ML, P, sizeof(double) * n * n);

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

    for (size_t L = 0; L < NL; L++) {
        if (L > 0) {
            cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                        (MKL_INT)n, (MKL_INT)n, (MKL_INT)n,
                        1.0, ML, (MKL_INT)n, P, (MKL_INT)n, 0.0, Mnext, (MKL_INT)n);
            for (size_t r = 0; r < n; r++) {
                double s = 0.0; for (size_t c = 0; c < n; c++) s += Mnext[r * n + c];
                double inv = s > 0.0 ? 1.0 / s : 0.0;
                for (size_t c = 0; c < n; c++) ML[r * n + c] = Mnext[r * n + c] * inv;
            }
        }
        if (L + 1u == NL && NL > 1u) {
            for (size_t i = 0; i < n * n; i++) Meff[i] = 0.6 * ML[i] + 0.4 * P[i];
        } else {
            memcpy(Meff, ML, sizeof(double) * n * n);
        }

        compile_layer_operators(L, NL, n, D, NH, NKV, HD, FF, Xd, Meff, wq, wk, wv, wo, fg, fu, fd);

        char tname[160];
        uint64_t norm_d[1] = { (uint64_t)D };
        uint64_t qd[2] = { (uint64_t)D, (uint64_t)qdim };
        uint64_t kd[2] = { (uint64_t)D, (uint64_t)kdim };
        uint64_t od[2] = { (uint64_t)qdim, (uint64_t)D };
        uint64_t fgd[2] = { (uint64_t)D, (uint64_t)FF };
        uint64_t fdd[2] = { (uint64_t)FF, (uint64_t)D };

        if (gw) {
            snprintf(tname, sizeof tname, "blk.%zu.attn_norm.weight", L);
            rc |= emit_tensor(gw, NULL, tname, norm_d, 1, ones, 0);
            snprintf(tname, sizeof tname, "blk.%zu.attn_q.weight", L);
            rc |= emit_tensor(gw, NULL, tname, qd, 2, wq, spec->f16_weights);
            snprintf(tname, sizeof tname, "blk.%zu.attn_k.weight", L);
            rc |= emit_tensor(gw, NULL, tname, kd, 2, wk, spec->f16_weights);
            snprintf(tname, sizeof tname, "blk.%zu.attn_v.weight", L);
            rc |= emit_tensor(gw, NULL, tname, kd, 2, wv, spec->f16_weights);
            snprintf(tname, sizeof tname, "blk.%zu.attn_output.weight", L);
            rc |= emit_tensor(gw, NULL, tname, od, 2, wo, spec->f16_weights);
            snprintf(tname, sizeof tname, "blk.%zu.ffn_norm.weight", L);
            rc |= emit_tensor(gw, NULL, tname, norm_d, 1, ones, 0);
            snprintf(tname, sizeof tname, "blk.%zu.ffn_gate.weight", L);
            rc |= emit_tensor(gw, NULL, tname, fgd, 2, fg, spec->f16_weights);
            snprintf(tname, sizeof tname, "blk.%zu.ffn_up.weight", L);
            rc |= emit_tensor(gw, NULL, tname, fgd, 2, fu, spec->f16_weights);
            snprintf(tname, sizeof tname, "blk.%zu.ffn_down.weight", L);
            rc |= emit_tensor(gw, NULL, tname, fdd, 2, fd, spec->f16_weights);
        } else {
            snprintf(tname, sizeof tname, "model.layers.%zu.input_layernorm.weight", L);
            rc |= emit_tensor(NULL, sw, tname, norm_d, 1, ones, 0);
            snprintf(tname, sizeof tname, "model.layers.%zu.self_attn.q_proj.weight", L);
            rc |= emit_tensor(NULL, sw, tname, qd, 2, wq, spec->f16_weights);
            snprintf(tname, sizeof tname, "model.layers.%zu.self_attn.k_proj.weight", L);
            rc |= emit_tensor(NULL, sw, tname, kd, 2, wk, spec->f16_weights);
            snprintf(tname, sizeof tname, "model.layers.%zu.self_attn.v_proj.weight", L);
            rc |= emit_tensor(NULL, sw, tname, kd, 2, wv, spec->f16_weights);
            snprintf(tname, sizeof tname, "model.layers.%zu.self_attn.o_proj.weight", L);
            rc |= emit_tensor(NULL, sw, tname, od, 2, wo, spec->f16_weights);
            snprintf(tname, sizeof tname, "model.layers.%zu.post_attention_layernorm.weight", L);
            rc |= emit_tensor(NULL, sw, tname, norm_d, 1, ones, 0);
            snprintf(tname, sizeof tname, "model.layers.%zu.mlp.gate_proj.weight", L);
            rc |= emit_tensor(NULL, sw, tname, fgd, 2, fg, spec->f16_weights);
            snprintf(tname, sizeof tname, "model.layers.%zu.mlp.up_proj.weight", L);
            rc |= emit_tensor(NULL, sw, tname, fgd, 2, fu, spec->f16_weights);
            snprintf(tname, sizeof tname, "model.layers.%zu.mlp.down_proj.weight", L);
            rc |= emit_tensor(NULL, sw, tname, fdd, 2, fd, spec->f16_weights);
        }
    }

    uint64_t out_norm_d[1] = { (uint64_t)D };
    rc |= emit_tensor(gw, sw, gw ? "output_norm.weight" : "model.norm.weight", out_norm_d, 1, ones, 0);
    if (!spec->tied_embeddings) {
        rc |= emit_tensor(gw, sw, gw ? "output.weight" : "lm_head.weight", emb_dims, 2, head, spec->f16_weights);
    }

    if (gw) { rc |= lp_gguf_writer_finalize(gw); lp_gguf_writer_free(gw); }
    if (sw) { rc |= lp_st_writer_finalize(sw); lp_st_writer_free(sw); }

    printf("exported %s (%s): vocab %zu, dim %zu, rank %zu, fisher_bp 10000 (%zu witnesses), raw_weight_bytes_retained = 0\n",
           spec->out_path, spec->format == 1 ? "safetensors" : "gguf", n, D, rank, n_witness);

    free(P); free(Xd); free(ML); free(Mnext); free(Meff);
    free(ones); free(wq); free(wk); free(wv); free(wo); free(fg); free(fu); free(fd);
    free(ids16); free(E); free(X); free(head);
    for (size_t i = 0; i < n_witness; i++) free(witnesses[i]);
    free(witnesses); free(share_bp);
    return rc ? 1 : 0;
}
