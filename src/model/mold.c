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

/* ---- CPU Mold-a-Model (Sequence 25 / Export.md §25.2-25.7):
 * Synthesizes clean Llama-architecture GGUF or SafeTensors checkpoints (laplace-blake3-v1) without gradient descent,
 * zero O(n^2) dense n x n matrices, and zero fabricated hash-bit padding:
 *   1. Open Merkle BLAKE3-128 vocabulary (laplace.vocab.blake3_ids + tokenizer.ggml.tokens)
 *   2. Integer-conserved Fisher attribution across ALL planes (SUM(share_bp) == 10000 via Hamilton apportionment)
 *   3. Full-rank multi-plane embeddings in O(nnz * D):
 *        - Plane 1: canonical 4D entity.coord anchors on S^3 / 4-ball
 *        - Plane 2: ordered tier0.bin UTF-8 codepoint 4D curve moments (deterministically separates anagrams)
 *        - Plane 3: sparse trajectory subspace iteration (physicality.path left/right continuation/co-occurrence)
 *        - Plane 4: sparse relational & unary eigenmodes (attestation/consensus + unary feature Gram U(U^T Q))
 *        - Higher orthogonal axes (if any remain): continuous Chebyshev 4D codepoint curve harmonics
 *   4. Per-layer, per-head operator compilation: each head h fills ONLY its own [h*HD..(h+1)*HD) rows from its
 *      declared substrate plane (TRAJECTORY, RELATION[type], METRIC, UNARY); each layer FFN compiles from its own
 *      ffn_plane; lm_head calibrates against exact post-NL residual states in weighted or bounded-set mode. */

typedef struct {
    char  *key;
    double mass;
} LineageMass;

typedef struct { int r, c; double w; } MoldEdge;

typedef struct {
    LpMoldPlaneKind kind;
    const char     *tag;
    MoldEdge       *ed;
    size_t          ne;
    double         *row_abs;   /* sum of |w| per row (length n_vocab) */
    double         *row_pos;   /* sum of max(w, 0) per row (length n_vocab) */
    size_t          n_features;/* for UNARY: max(col) + 1 */
} CompactPlane;

static int cmp_mold_edge(const void *a, const void *b){
    const MoldEdge *x = a, *y = b;
    if (x->r != y->r) return x->r < y->r ? -1 : 1;
    return x->c < y->c ? -1 : (x->c > y->c ? 1 : 0);
}

static CompactPlane compact_plane_from_raw(LpMoldPlaneKind kind, const char *tag,
                                           const int *rows, const int *cols, const double *weights,
                                           size_t n_edges, size_t n_vocab, size_t n_features){
    CompactPlane cp;
    memset(&cp, 0, sizeof cp);
    cp.kind = kind;
    cp.tag = tag ? tag : "trajectory";
    cp.row_abs = calloc(n_vocab, sizeof(double));
    cp.row_pos = calloc(n_vocab, sizeof(double));
    cp.n_features = (kind == LP_MOLD_PLANE_UNARY) ? n_features : n_vocab;

    MoldEdge *ed = n_edges ? malloc(sizeof(MoldEdge) * n_edges) : NULL;
    size_t ne = 0;
    size_t max_col = cp.n_features;
    for (size_t e = 0; e < n_edges; e++) {
        int r = rows[e], c = cols[e];
        double w = weights ? weights[e] : 1.0;
        if (!isfinite(w) || w == 0.0) continue;
        if (r < 0 || (size_t)r >= n_vocab || c < 0) continue;
        if (kind != LP_MOLD_PLANE_UNARY && (size_t)c >= n_vocab) continue;
        if (kind == LP_MOLD_PLANE_UNARY && (size_t)c >= max_col) max_col = (size_t)c + 1u;
        ed[ne++] = (MoldEdge){ r, c, w };
    }
    cp.n_features = max_col;
    if (ne > 1u) {
        qsort(ed, ne, sizeof(MoldEdge), cmp_mold_edge);
        size_t out = 0;
        for (size_t i = 0; i < ne; i++) {
            if (out > 0 && ed[out - 1u].r == ed[i].r && ed[out - 1u].c == ed[i].c) {
                ed[out - 1u].w += ed[i].w;
            } else {
                ed[out++] = ed[i];
            }
        }
        ne = out;
    }
    for (size_t i = 0; i < ne; i++) {
        double aw = fabs(ed[i].w);
        cp.row_abs[ed[i].r] += aw;
        if (ed[i].w > 0.0) cp.row_pos[ed[i].r] += ed[i].w;
    }
    cp.ed = ed;
    cp.ne = ne;
    return cp;
}

static void compact_plane_free(CompactPlane *cp){
    if (!cp) return;
    free(cp->ed);
    free(cp->row_abs);
    free(cp->row_pos);
    memset(cp, 0, sizeof *cp);
}

/* Hamilton (largest-remainder) apportionment across ALL planes guaranteeing exact SUM(share_bp) == 10000. */
static void compute_fisher_bp(const LpMoldSpec *spec, char ***witnesses_out,
                              int32_t **bp_out, size_t *count_out){
    size_t nl = 0, cap = 8;
    LineageMass *lm = malloc(sizeof *lm * cap);
    lp_strmap *seen = lp_strmap_sized(sizeof(size_t));

    if (spec->n_planes > 0 && spec->planes) {
        for (size_t p = 0; p < spec->n_planes; p++) {
            const LpMoldPlane *pl = &spec->planes[p];
            if (!pl->n_edges || !pl->witnesses) continue;
            for (size_t i = 0; i < pl->n_edges; i++) {
                const char *k = (pl->lineages && pl->lineages[i])
                    ? pl->lineages[i]
                    : (pl->witnesses[i] ? pl->witnesses[i] : "Laplace");
                double w = pl->weights ? fabs(pl->weights[i]) : 1.0;
                if (!(w > 0.0)) continue;
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
    }
    if (nl == 0 && spec->n_edges > 0 && spec->edge_witnesses) {
        for (size_t i = 0; i < spec->n_edges; i++) {
            const char *k = (spec->edge_lineages && spec->edge_lineages[i])
                ? spec->edge_lineages[i]
                : (spec->edge_witnesses[i] ? spec->edge_witnesses[i] : "Laplace");
            double w = spec->weights ? fabs(spec->weights[i]) : 1.0;
            if (!(w > 0.0)) continue;
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
    if (nl == 0) {
        lm[0] = (LineageMass){ strdup(spec->name ? spec->name : "Laplace"), 1.0 };
        nl = 1;
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

/* Sparse O(nnz * D) operator application: Y_out = O_plane(Y_in). Zero n x n matrix allocation. */
static void sparse_apply_plane(const CompactPlane *pl, size_t eos_id,
                               const double *Y_in, double *Y_out, size_t n, size_t D, int transpose){
    if (!pl || pl->ne == 0) {
        memcpy(Y_out, Y_in, sizeof(double) * n * D);
        return;
    }
    if (pl->kind == LP_MOLD_PLANE_UNARY) {
        /* Sparse feature Gram: F = U^T * Y_in (n_feat x D), then Y_out = D_row^-1 * U * F. */
        size_t nf = pl->n_features ? pl->n_features : 1u;
        double *F = calloc(nf * D, sizeof(double));
        double *fdeg = calloc(nf, sizeof(double));
        for (size_t e = 0; e < pl->ne; e++) {
            size_t r = (size_t)pl->ed[e].r, f = (size_t)pl->ed[e].c;
            double w = pl->ed[e].w;
            if (r >= n || f >= nf) continue;
            fdeg[f] += fabs(w);
            const double *in = Y_in + r * D;
            double *fo = F + f * D;
            for (size_t d = 0; d < D; d++) fo[d] += w * in[d];
        }
        for (size_t f = 0; f < nf; f++) {
            if (fdeg[f] > 0.0) {
                double inv = 1.0 / (fdeg[f] + 0.05);
                double *fo = F + f * D;
                for (size_t d = 0; d < D; d++) fo[d] *= inv;
            }
        }
        memset(Y_out, 0, sizeof(double) * n * D);
        for (size_t e = 0; e < pl->ne; e++) {
            size_t r = (size_t)pl->ed[e].r, f = (size_t)pl->ed[e].c;
            double w = pl->ed[e].w;
            if (r >= n || f >= nf) continue;
            double coeff = w / (pl->row_abs[r] + 0.05);
            double *out = Y_out + r * D;
            const double *fo = F + f * D;
            for (size_t d = 0; d < D; d++) out[d] += coeff * fo[d];
        }
        for (size_t r = 0; r < n; r++) {
            if (pl->row_abs[r] <= 0.0) memcpy(Y_out + r * D, Y_in + r * D, sizeof(double) * D);
        }
        free(F);
        free(fdeg);
        return;
    }

    if (transpose) {
        double *col_abs = calloc(n, sizeof(double));
        for (size_t e = 0; e < pl->ne; e++) {
            size_t c = (size_t)pl->ed[e].c;
            if (c < n) col_abs[c] += fabs(pl->ed[e].w);
        }
        memset(Y_out, 0, sizeof(double) * n * D);
        for (size_t e = 0; e < pl->ne; e++) {
            size_t r = (size_t)pl->ed[e].r, c = (size_t)pl->ed[e].c;
            if (r >= n || c >= n) continue;
            double coeff = pl->ed[e].w / (col_abs[c] + 0.05);
            double *out = Y_out + c * D;
            const double *in = Y_in + r * D;
            for (size_t d = 0; d < D; d++) out[d] += coeff * in[d];
        }
        free(col_abs);
        return;
    }

    /* Forward sparse propagation supporting both positive (excitatory) and negative (inhibitory) edges. */
    double *col_mean = calloc(D, sizeof(double));
    for (size_t i = 0; i < n; i++) {
        const double *row = Y_in + i * D;
        for (size_t d = 0; d < D; d++) col_mean[d] += row[d];
    }
    double smooth_scale = 0.05 / (double)n;
    for (size_t d = 0; d < D; d++) col_mean[d] *= smooth_scale;

    for (size_t r = 0; r < n; r++) {
        double *out = Y_out + r * D;
        if (pl->row_abs[r] <= 0.0) {
            if (pl->kind == LP_MOLD_PLANE_TRAJECTORY) {
                double inv = 1.0 / 1.05;
                const double *eos_row = Y_in + eos_id * D;
                for (size_t d = 0; d < D; d++) out[d] = (eos_row[d] + col_mean[d]) * inv;
            } else {
                memcpy(out, Y_in + r * D, sizeof(double) * D);
            }
        } else {
            double inv = 1.0 / (pl->row_abs[r] + 0.05);
            for (size_t d = 0; d < D; d++) out[d] = col_mean[d] * inv;
        }
    }
    for (size_t e = 0; e < pl->ne; e++) {
        size_t r = (size_t)pl->ed[e].r, c = (size_t)pl->ed[e].c;
        if (r >= n || c >= n) continue;
        double coeff = pl->ed[e].w / (pl->row_abs[r] + 0.05);
        double *out = Y_out + r * D;
        const double *in = Y_in + c * D;
        for (size_t d = 0; d < D; d++) out[d] += coeff * in[d];
    }
    free(col_mean);
}

/* Compute dense SVD rank of M (rows x cols, row-major f32) via M^T M (D x D) when rows > cols (O(n * D^2), zero n x n). */
static size_t measure_matrix_rank(const float *M, size_t rows, size_t cols){
    size_t k = rows < cols ? rows : cols;
    if (!k) return 0;
    if (rows >= cols) {
        double *Md = malloc(sizeof(double) * rows * cols);
        double *G = calloc(cols * cols, sizeof(double));
        double *s = malloc(sizeof(double) * cols);
        for (size_t i = 0; i < rows * cols; i++) Md[i] = (double)M[i];
        cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    (MKL_INT)cols, (MKL_INT)cols, (MKL_INT)rows,
                    1.0, Md, (MKL_INT)cols, Md, (MKL_INT)cols, 0.0, G, (MKL_INT)cols);
        int rc = LAPACKE_dsyevd(LAPACK_ROW_MAJOR, 'N', 'U', (lapack_int)cols, G, (lapack_int)cols, s);
        size_t rank = 0;
        if (rc == 0 && s[cols - 1u] > 0.0) {
            double tol = s[cols - 1u] * 1e-12;
            for (size_t i = 0; i < cols; i++) if (s[i] > tol) rank++;
        }
        free(Md); free(G); free(s);
        return rank;
    }
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

/* Orthonormalize column-major (k x n) rows into row-major (n x k) Q for sparse subspace iteration. */
static void orthonormalize_rows_inplace(double *Q_kn, size_t k, size_t n){
    if (lp_gram_schmidt(Q_kn, k, n) != 0) {
        for (size_t c = 0; c < k; c++) {
            double ss = 0.0;
            for (size_t i = 0; i < n; i++) ss += Q_kn[c * n + i] * Q_kn[c * n + i];
            if (ss > 1e-18) {
                double inv = 1.0 / sqrt(ss);
                for (size_t i = 0; i < n; i++) Q_kn[c * n + i] *= inv;
            }
        }
    }
}

/* Compute exact Chebyshev harmonic T_m(u) on u in [-1, 1] over real 4D S^3 codepoint trajectory (zero hash noise). */
static double chebyshev_t(size_t m, double u){
    if (m == 0) return 1.0;
    if (m == 1) return u;
    double t0 = 1.0, t1 = u;
    for (size_t k = 2; k <= m; k++) {
        double t2 = 2.0 * u * t1 - t0;
        t0 = t1; t1 = t2;
    }
    return t1;
}

/* Full-rank multi-plane embedding in O(nnz * D): ZERO n x n dense matrices, ZERO fabricated BLAKE3 hash padding. */
static void build_multiplane_embedding(const LpMoldSpec *spec, const CompactPlane *planes, size_t n_planes,
                                       const lp_tier0_record *t0, size_t n, size_t D,
                                       const double *anchors4d, float *E_out, size_t *rank_out){
    size_t target_rank = D - 2u;
    if (target_rank > n) target_rank = n;
    double *vecs = calloc(target_rank * n, sizeof(double));

    /* Precompute real ordered tier0.bin 4D S^3 codepoint curves for every token (deterministic geometry, separates anagrams). */
    double **tok_curves = calloc(n, sizeof(double *));
    size_t *tok_lens = calloc(n, sizeof(size_t));
    if (t0) {
        lp_text *cp_tx = lp_text_new(t0);
        for (size_t i = 0; i < n; i++) {
            const char *s = (spec->tokens && spec->tokens[i]) ? spec->tokens[i] : "";
            size_t slen = strlen(s), pos = 0, m = 0, mcap = 16;
            double *c4 = malloc(sizeof(double) * 4u * mcap);
            while (pos < slen) {
                size_t p0 = pos;
                uint32_t cp = 0;
                if (!lp_utf8_next((const uint8_t *)s, slen, &pos, &cp)) break;
                if (m == mcap) { mcap *= 2u; c4 = xrealloc(c4, sizeof(double) * 4u * mcap); }
                lp_ref cr = lp_text_decompose(cp_tx, (const uint8_t *)(s + p0), pos - p0, NULL, NULL);
                lp_coord_xyzm(&cr.c, c4 + m * 4u);
                m++;
            }
            if (m == 0) {
                memcpy(c4, anchors4d + i * 4u, sizeof(double) * 4u);
                m = 1;
            }
            tok_curves[i] = c4;
            tok_lens[i] = m;
        }
        if (cp_tx) lp_text_free(cp_tx);
    } else {
        for (size_t i = 0; i < n; i++) {
            tok_curves[i] = malloc(sizeof(double) * 4u);
            memcpy(tok_curves[i], anchors4d + i * 4u, sizeof(double) * 4u);
            tok_lens[i] = 1;
        }
    }

    /* Plane 1 (axes 0..3): canonical 4D S^3 / 4-ball entity.coord anchors. */
    for (size_t c = 0; c < 4 && c < target_rank; c++) {
        for (size_t i = 0; i < n; i++) vecs[c * n + i] = anchors4d[i * 4u + c];
    }
    size_t filled = target_rank < 4u ? target_rank : 4u;

    /* Plane 2 (up to 16 axes): ordered tier0.bin 4D codepoint trajectory positional & bigram cross-moments. */
    size_t curve_budget = target_rank > filled ? (target_rank - filled) : 0;
    if (curve_budget > 16u) curve_budget = 16u;
    for (size_t ax = 0; ax < curve_budget && filled < target_rank; ax++) {
        for (size_t i = 0; i < n; i++) {
            const double *c4 = tok_curves[i];
            size_t m = tok_lens[i];
            double val = 0.0;
            if (ax < 8u) {
                size_t r = 1u + (ax / 4u);
                size_t d = ax % 4u;
                for (size_t p = 0; p < m; p++) {
                    double t = (2.0 * (double)p + 1.0) / (2.0 * (double)m);
                    val += pow(t, (double)r) * c4[p * 4u + d];
                }
                val /= (double)m;
            } else if (m >= 2u) {
                size_t bi = ax - 8u;
                size_t r = bi / 4u;
                size_t d = bi % 4u;
                for (size_t p = 0; p + 1u < m; p++) {
                    double t = (2.0 * (double)p + 1.0) / (2.0 * (double)m);
                    val += (r ? t : 1.0) * c4[p * 4u + d] * c4[(p + 1u) * 4u + ((d + 1u) % 4u)];
                }
                val /= (double)(m - 1u);
            } else {
                val = c4[ax % 4u] * c4[(ax + 1u) % 4u];
            }
            vecs[filled * n + i] = val;
        }
        filled++;
    }

    /* Plane 3 & 4: Sparse Subspace Iteration (O(nnz * k), ZERO n x n matrix) across ALL real substrate planes
     * (Trajectory left/right continuation + Typed Relations + Unary feature Gram + 4D Metric curves). */
    size_t active_planes = 0;
    for (size_t p = 0; p < n_planes; p++) if (planes[p].ne > 0) active_planes++;
    if (filled < target_rank && active_planes > 0) {
        size_t rem_axes = target_rank - filled;
        size_t per_plane = (rem_axes + active_planes - 1u) / active_planes;
        if (per_plane < 2u) per_plane = 2u;

        double *Q = malloc(sizeof(double) * n * per_plane);
        double *Z = malloc(sizeof(double) * n * per_plane);
        double *Qt = malloc(sizeof(double) * per_plane * n);

        for (size_t p = 0; p < n_planes && filled < target_rank; p++) {
            if (planes[p].ne == 0) continue;
            size_t want = target_rank - filled;
            if (want > per_plane) want = per_plane;

            /* Deterministic seed basis from continuous Chebyshev projections of real 4D codepoint trajectories. */
            for (size_t k = 0; k < want; k++) {
                size_t deg = 1u + ((k + p * 3u) / 4u);
                size_t d = (k + p) % 4u;
                for (size_t i = 0; i < n; i++) {
                    const double *c4 = tok_curves[i];
                    size_t m = tok_lens[i];
                    double acc = 0.0;
                    for (size_t pos = 0; pos < m; pos++) {
                        double u = m > 1u ? (2.0 * (double)pos / (double)(m - 1u) - 1.0) : 0.0;
                        acc += chebyshev_t(deg, u) * c4[pos * 4u + d];
                    }
                    Qt[k * n + i] = acc / (double)m;
                }
            }
            orthonormalize_rows_inplace(Qt, want, n);
            for (size_t i = 0; i < n; i++) for (size_t k = 0; k < want; k++) Q[i * want + k] = Qt[k * n + i];

            /* 3 sparse orthogonal subspace iterations: Z = O(Q) and O^T(O(Q)) for asymmetric trajectory/relation planes. */
            for (int it = 0; it < 3; it++) {
                sparse_apply_plane(&planes[p], (size_t)spec->eos_id, Q, Z, n, want, 0);
                if (planes[p].kind == LP_MOLD_PLANE_TRAJECTORY || planes[p].kind == LP_MOLD_PLANE_RELATION) {
                    /* Keep left/right asymmetry: alternate forward and transpose-mixed propagation */
                    if (it == 1) {
                        double *Zt = malloc(sizeof(double) * n * want);
                        sparse_apply_plane(&planes[p], (size_t)spec->eos_id, Z, Zt, n, want, 1);
                        for (size_t idx = 0; idx < n * want; idx++) Z[idx] = 0.7 * Z[idx] + 0.3 * Zt[idx];
                        free(Zt);
                    }
                }
                for (size_t i = 0; i < n; i++) for (size_t k = 0; k < want; k++) Qt[k * n + i] = Z[i * want + k];
                orthonormalize_rows_inplace(Qt, want, n);
                for (size_t i = 0; i < n; i++) for (size_t k = 0; k < want; k++) Q[i * want + k] = Qt[k * n + i];
            }

            /* Append non-degenerate singular modes to vecs */
            for (size_t k = 0; k < want && filled < target_rank; k++) {
                double norm2 = 0.0;
                for (size_t i = 0; i < n; i++) norm2 += Qt[k * n + i] * Qt[k * n + i];
                if (norm2 > 1e-12) {
                    memcpy(vecs + filled * n, Qt + k * n, sizeof(double) * n);
                    filled++;
                }
            }
        }
        free(Q); free(Z); free(Qt);
    }

    /* Any remaining axes (e.g. very high D on tiny test vocabs): continuous Chebyshev 4D curve harmonics (zero hash bits). */
    size_t harm_idx = 0;
    while (filled < target_rank) {
        size_t deg = 3u + (harm_idx / 4u);
        size_t d = harm_idx % 4u;
        size_t d2 = (harm_idx + 1u) % 4u;
        for (size_t i = 0; i < n; i++) {
            const double *c4 = tok_curves[i];
            size_t m = tok_lens[i];
            double acc = 0.0;
            for (size_t pos = 0; pos < m; pos++) {
                double u = m > 1u ? (2.0 * (double)pos / (double)(m - 1u) - 1.0) : 0.0;
                double w = chebyshev_t(deg, u);
                acc += w * (c4[pos * 4u + d] + 0.5 * c4[pos * 4u + d2] * (double)(pos + 1u) / (double)m);
            }
            vecs[filled * n + i] = acc / (double)m;
        }
        filled++;
        harm_idx++;
    }

    for (size_t i = 0; i < n; i++) free(tok_curves[i]);
    free(tok_curves); free(tok_lens);

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

    /* Pack (n x D) with exact RMS normalization slack in dims D-2, D-1 */
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

/* Compile lm_head (V x D) from the declared lm_head_plane in O(n_active*D^2 + nnz*D) (zero n x n matrix).
 * Fits ONLY rows that have real substrate evidence (active sources or terminal targets), so unused byte-fallback
 * slots (<0x00>..<0xFF>) never drown the rank-D readout with spurious EOS votes! */
static void synthesize_readout(const LpMoldSpec *spec, const CompactPlane *lm_plane,
                               const float *X, size_t n, size_t D, float *head_out){
    double delta = (spec->lm_head_max_outside_prob > 0.0 && spec->lm_head_max_outside_prob < 1.0)
        ? spec->lm_head_max_outside_prob : 1e-3;
    double floor_log = log(delta / (double)n);

    size_t *row_k = calloc(n, sizeof(size_t));
    uint8_t *is_target = calloc(n, 1);
    for (size_t e = 0; e < lm_plane->ne; e++) {
        size_t r = (size_t)lm_plane->ed[e].r, c = (size_t)lm_plane->ed[e].c;
        if (r < n && c < n && lm_plane->ed[e].w > 0.0) {
            row_k[r]++;
            is_target[c] = 1;
        }
    }

    /* Collect active rows: tokens with outgoing edges, plus terminal targets (for clean EOS stall on hypernym roots). */
    size_t *active = malloc(sizeof(size_t) * n);
    size_t *row_to_act = malloc(sizeof(size_t) * n);
    size_t na = 0;
    for (size_t i = 0; i < n; i++) {
        row_to_act[i] = (size_t)-1;
        if (lm_plane->row_pos[i] > 0.0 || is_target[i]) {
            row_to_act[i] = na;
            active[na++] = i;
        }
    }
    if (na == 0) {
        memcpy(head_out, X, sizeof(float) * n * D);
        free(row_k); free(is_target); free(active); free(row_to_act);
        return;
    }

    size_t k = na < D ? na : D;
    double *A = malloc(sizeof(double) * na * D);
    double *U = malloc(sizeof(double) * na * k);
    double *S = malloc(sizeof(double) * k);
    double *Vt = malloc(sizeof(double) * k * D);
    for (size_t ai = 0; ai < na; ai++) {
        size_t r = active[ai];
        for (size_t c = 0; c < D; c++) A[ai * D + c] = (double)X[r * D + c];
    }

    int rc = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'S', (lapack_int)na, (lapack_int)D,
                            A, (lapack_int)D, S, U, (lapack_int)k, Vt, (lapack_int)D);
    if (rc == 0) {
        double *u_sum = calloc(k, sizeof(double));
        for (size_t ai = 0; ai < na; ai++) {
            size_t r = active[ai];
            const double *ur = U + ai * k;
            double row_floor = floor_log;
            if (spec->lm_head_set_mode && row_k[r] > 0 && row_k[r] < n) {
                double vk = (double)(n - row_k[r]);
                row_floor = -log((vk * (1.0 - delta)) / ((double)row_k[r] * delta));
            }
            for (size_t z = 0; z < k; z++) u_sum[z] += ur[z] * row_floor;
        }
        double *UtT = calloc(k * n, sizeof(double));
        for (size_t z = 0; z < k; z++) {
            double base = u_sum[z];
            double *row = UtT + z * n;
            for (size_t j = 0; j < n; j++) row[j] = base;
        }
        for (size_t ai = 0; ai < na; ai++) {
            size_t r = active[ai];
            if (lm_plane->row_pos[r] <= 0.0) {
                /* Terminal target node with no outgoing continuation stalls cleanly at EOS. */
                double target = spec->lm_head_set_mode ? 0.0 : 1.0;
                double dlt = target - floor_log;
                const double *ur = U + ai * k;
                for (size_t z = 0; z < k; z++) UtT[z * n + (size_t)spec->eos_id] += ur[z] * dlt;
            }
        }
        for (size_t e = 0; e < lm_plane->ne; e++) {
            size_t r = (size_t)lm_plane->ed[e].r, c = (size_t)lm_plane->ed[e].c;
            if (r >= n || c >= n) continue;
            size_t ai = row_to_act[r];
            if (ai == (size_t)-1) continue;
            double w = lm_plane->ed[e].w;
            double row_floor = floor_log;
            if (spec->lm_head_set_mode && row_k[r] > 0 && row_k[r] < n) {
                double vk = (double)(n - row_k[r]);
                row_floor = -log((vk * (1.0 - delta)) / ((double)row_k[r] * delta));
            }
            double target = row_floor;
            if (w > 0.0 && lm_plane->row_pos[r] > 0.0) {
                target = spec->lm_head_set_mode ? 0.0 : (log(w / lm_plane->row_pos[r] * (double)n) + 2.0);
            } else if (w < 0.0) {
                target = row_floor - log1p(fabs(w));
            }
            double dlt = target - row_floor;
            if (dlt == 0.0) continue;
            const double *ur = U + ai * k;
            for (size_t z = 0; z < k; z++) UtT[z * n + c] += ur[z] * dlt;
        }
        free(u_sum);
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
    free(row_k); free(is_target); free(active); free(row_to_act);
    free(A); free(U); free(S); free(Vt);
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

/* Compile layer L: EACH head h fills ONLY its own [h*HD, (h+1)*HD) rows from its declared substrate plane!
 * FFN compiles from this layer's declared ffn_plane! Zero n x n matrices, zero tiling of one matrix across heads. */
static void compile_layer_per_head(size_t L, size_t NL, size_t n, size_t D, size_t NH, size_t NKV, size_t HD, size_t FF,
                                   size_t eos_id, const double *Xd,
                                   const CompactPlane *planes, size_t n_planes,
                                   const size_t *head_planes, size_t ffn_plane,
                                   float *wq, float *wk, float *wv, float *wo,
                                   float *fg, float *fu, float *fd){
    size_t qdim = NH * HD, kdim = NKV * HD;
    double split = pow((double)NL, -0.25);
    double reg = 0.05 / (double)(L + 1u);

    memset(wq, 0, sizeof(float) * qdim * D);
    memset(wk, 0, sizeof(float) * kdim * D);
    memset(wv, 0, sizeof(float) * kdim * D);
    memset(wo, 0, sizeof(float) * D * qdim);

    double *MX = malloc(sizeof(double) * n * D);
    double *MX2 = malloc(sizeof(double) * n * D);
    double *TMX = malloc(sizeof(double) * n * D);
    double *B = malloc(sizeof(double) * D * D);
    double *T = malloc(sizeof(double) * D * D);
    double *UB = malloc(sizeof(double) * D * D), *SB = malloc(sizeof(double) * D), *VtB = malloc(sizeof(double) * D * D);
    double *UT = malloc(sizeof(double) * D * D), *ST = malloc(sizeof(double) * D), *VtT = malloc(sizeof(double) * D * D);

    double *wk_acc = calloc(kdim * D, sizeof(double));
    double *wv_acc = calloc(kdim * D, sizeof(double));
    size_t group = NH / NKV;
    double inv_g = 1.0 / (double)group;

    /* Track how many heads in this layer share the same plane so distinct heads on the same plane take
     * disjoint spectral bands [occ*HD..(occ+1)*HD) rather than duplicating modes. */
    size_t *plane_occ = calloc(n_planes ? n_planes : 1u, sizeof(size_t));

    for (size_t h = 0; h < NH; h++) {
        size_t pi = head_planes ? (head_planes[h] % n_planes) : (h % n_planes);
        size_t occ_idx = plane_occ[pi]++;
        const CompactPlane *pl = &planes[pi];

        /* Apply head h's OWN sparse operator (depth-modulated for multi-layer composition on the same plane). */
        sparse_apply_plane(pl, eos_id, Xd, MX, n, D, 0);
        if (L > 0 && pl->kind == LP_MOLD_PLANE_TRAJECTORY) {
            sparse_apply_plane(pl, eos_id, MX, MX2, n, D, 0);
            for (size_t i = 0; i < n * D; i++) MX[i] = 0.65 * MX[i] + 0.35 * MX2[i];
        }

        /* Head h bilinear QK affinity: B_h = X^T * MX */
        cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    (MKL_INT)D, (MKL_INT)D, (MKL_INT)n,
                    1.0, Xd, (MKL_INT)D, MX, (MKL_INT)D, 0.0, B, (MKL_INT)D);
        for (size_t c = 0; c < D; c++) B[c * D + c] += reg;

        /* Head h value-transport OV operator: T_h = X^T * (MX - X) */
        for (size_t i = 0; i < n * D; i++) TMX[i] = MX[i] - Xd[i];
        cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    (MKL_INT)D, (MKL_INT)D, (MKL_INT)n,
                    1.0, Xd, (MKL_INT)D, TMX, (MKL_INT)D, 0.0, T, (MKL_INT)D);
        for (size_t c = 0; c < D; c++) T[c * D + c] += 1e-4 / (double)(L + 1u);

        int rcB = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'A', (lapack_int)D, (lapack_int)D, B, (lapack_int)D, SB, UB, (lapack_int)D, VtB, (lapack_int)D);
        int rcT = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'A', (lapack_int)D, (lapack_int)D, T, (lapack_int)D, ST, UT, (lapack_int)D, VtT, (lapack_int)D);

        size_t kv = h / group;
        size_t mode_base = (occ_idx * HD) % D;

        for (size_t j = 0; j < HD; j++) {
            size_t qrow = h * HD + j;
            size_t krow = kv * HD + j;
            size_t mode = (mode_base + j) % D;
            if (rcB == 0) {
                double sgn = sqrt(SB[mode] > 1e-12 ? SB[mode] : 1e-12) * 0.25 * split;
                for (size_t c = 0; c < D; c++) {
                    wq[qrow * D + c] = (float)(sgn * UB[c * D + mode]);
                    wk_acc[krow * D + c] += sgn * VtB[mode * D + c] * inv_g;
                }
            }
            if (rcT == 0) {
                double sgn = sqrt(ST[mode] > 1e-12 ? ST[mode] : 1e-12) * 0.15 * split;
                for (size_t c = 0; c < D; c++) {
                    wv_acc[krow * D + c] += sgn * VtT[mode * D + c] * inv_g;
                    wo[c * qdim + qrow] = (float)(sgn * UT[c * D + mode]);
                }
            }
        }
    }
    for (size_t i = 0; i < kdim * D; i++) {
        wk[i] = (float)wk_acc[i];
        wv[i] = (float)wv_acc[i];
    }

    /* Layer L FFN: compile SwiGLU (gate/up/down) from this layer's declared ffn_plane! */
    const CompactPlane *fpl = &planes[ffn_plane % n_planes];
    sparse_apply_plane(fpl, eos_id, Xd, MX, n, D, 0);
    cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                (MKL_INT)D, (MKL_INT)D, (MKL_INT)n,
                1.0, Xd, (MKL_INT)D, MX, (MKL_INT)D, 0.0, B, (MKL_INT)D);
    cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                (MKL_INT)D, (MKL_INT)D, (MKL_INT)n,
                1.0, MX, (MKL_INT)D, MX, (MKL_INT)D, 0.0, T, (MKL_INT)D);
    for (size_t i = 0; i < D * D; i++) T[i] -= B[i];
    for (size_t c = 0; c < D; c++) {
        B[c * D + c] += reg;
        T[c * D + c] += reg + 1e-4 / (double)(L + 1u);
    }

    int rcFB = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'A', (lapack_int)D, (lapack_int)D, B, (lapack_int)D, SB, UB, (lapack_int)D, VtB, (lapack_int)D);
    int rcFR = LAPACKE_dgesdd(LAPACK_ROW_MAJOR, 'A', (lapack_int)D, (lapack_int)D, T, (lapack_int)D, ST, UT, (lapack_int)D, VtT, (lapack_int)D);

    memset(fg, 0, sizeof(float) * FF * D);
    memset(fu, 0, sizeof(float) * FF * D);
    memset(fd, 0, sizeof(float) * D * FF);
    if (rcFR == 0 && rcFB == 0) {
        for (size_t z = 0; z < FF; z++) {
            size_t mode = z % D;
            double band_scale = 1.0 / (double)(1u + z / D);
            double sf = sqrt(ST[mode] > 1e-12 ? ST[mode] : 1e-12) * 0.15 * split * band_scale;
            for (size_t c = 0; c < D; c++) {
                fu[z * D + c] = (float)(sf * VtT[mode * D + c]);
                fd[c * FF + z] = (float)(sf * UT[c * D + mode]);
                fg[z * D + c] = (float)(0.5 * UB[c * D + mode] * band_scale);
            }
            fg[z * D + (D - 1u)] += (float)(0.25 + 0.10 * (double)(L + 1u));
        }
    }

    free(plane_occ);
    free(wk_acc); free(wv_acc);
    free(MX); free(MX2); free(TMX);
    free(B); free(T);
    free(UB); free(SB); free(VtB);
    free(UT); free(ST); free(VtT);
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

    /* Compact all declared substrate planes (or single legacy edge set when n_planes == 0). */
    size_t n_planes = (spec->n_planes > 0 && spec->planes) ? spec->n_planes : 1u;
    CompactPlane *cplanes = calloc(n_planes, sizeof(CompactPlane));
    if (spec->n_planes > 0 && spec->planes) {
        for (size_t p = 0; p < n_planes; p++) {
            const LpMoldPlane *pl = &spec->planes[p];
            cplanes[p] = compact_plane_from_raw(pl->kind, pl->tag, pl->rows, pl->cols, pl->weights,
                                                pl->n_edges, n, pl->n_features);
        }
    } else {
        cplanes[0] = compact_plane_from_raw(LP_MOLD_PLANE_TRAJECTORY, "trajectory",
                                            spec->rows, spec->cols, spec->weights,
                                            spec->n_edges, n, n);
    }
    size_t lm_pi = (spec->lm_head_plane < n_planes) ? spec->lm_head_plane : 0u;

    char **witnesses = NULL;
    int32_t *share_bp = NULL;
    size_t n_witness = 0;
    compute_fisher_bp(spec, &witnesses, &share_bp, &n_witness);

    float *E = calloc(n * D, sizeof(float));
    float *X = calloc(n * D, sizeof(float));
    float *H_cal = calloc(n * D, sizeof(float));
    float *H_norm = calloc(n * D, sizeof(float));
    float *head = calloc(n * D, sizeof(float));
    size_t rank = 0;
    build_multiplane_embedding(spec, cplanes, n_planes, t0, n, D, anchors, E, &rank);
    rows_rmsnorm_f32(E, X, n, D, eps);
    memcpy(H_cal, E, sizeof(float) * n * D);
    free(anchors);

    lp_gguf_writer *gw = NULL;
    lp_st_writer *sw = NULL;
    if (spec->format == 0) {
        gw = lp_gguf_writer_create(spec->out_path);
        if (!gw) goto fail_writers;
    } else {
        sw = lp_st_writer_create(spec->out_path);
        if (!sw) goto fail_writers;
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
            if (!is_unk && !is_ctrl && !is_byte && chosen[0] != '<' && !(clen >= 3 && (uint8_t)chosen[0] == 0xE2 && (uint8_t)chosen[1] == 0x96 && (uint8_t)chosen[2] == 0x81)) {
                /* Prefix normal word tokens with SentencePiece U+2581 ('▁') so llama.cpp renders clean word spacing. */
                char *sp = malloc(clen + 4u);
                sp[0] = (char)0xE2; sp[1] = (char)0x96; sp[2] = (char)0x81;
                memcpy(sp + 3, chosen, clen + 1u);
                free(owned_tokens[i]);
                owned_tokens[i] = sp;
                chosen = sp;
                ggml_tokens[i] = chosen;
            }
            if (!found_unk && !strcmp(chosen, "<unk>")) {
                unk_id = (uint32_t)i;
                found_unk = 1;
            }
            if (is_unk) { tok_types[i] = 2; scores[i] = -1000.0f; }
            else if (is_ctrl) { tok_types[i] = 3; scores[i] = 0.0f; }
            else if (is_byte) { tok_types[i] = 6; scores[i] = -100.0f; }
            else { tok_types[i] = 1; scores[i] = -1.0f - (float)i / (float)(n + 1u); }
        }
        lp_strmap_free(seen_tok);

        const char **ptags = malloc(sizeof(char *) * n_planes);
        for (size_t p = 0; p < n_planes; p++) ptags[p] = cplanes[p].tag;

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
        rc |= lp_gguf_writer_add_bool(gw, "tokenizer.ggml.add_space_prefix", 1);
        rc |= lp_gguf_writer_add_str(gw, "laplace.tokenizer.scheme", "laplace-blake3-v1");
        rc |= lp_gguf_writer_add_u32(gw, "laplace.vocab.size", (uint32_t)n);
        rc |= lp_gguf_writer_add_u8_array(gw, "laplace.vocab.blake3_ids", ids16, 16u * n);
        rc |= lp_gguf_writer_add_str_array(gw, "laplace.mold.planes", ptags, n_planes);
        rc |= lp_gguf_writer_add_str(gw, "laplace.mold.lm_head_plane", cplanes[lm_pi].tag);
        free(ptags);
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
        rc |= lp_st_writer_add_meta(sw, "laplace.mold.lm_head_plane", cplanes[lm_pi].tag);

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

    double *Xd = malloc(sizeof(double) * n * D);
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
    float *cal_v = malloc(sizeof(float) * n * kdim);
    float *cal_vfull = malloc(sizeof(float) * n * qdim);
    float *cal_out = malloc(sizeof(float) * n * D);
    float *cal_g = malloc(sizeof(float) * n * FF);
    float *cal_u = malloc(sizeof(float) * n * FF);

    for (size_t L = 0; L < NL; L++) {
        /* Input state entering layer L is exact RMSNorm(H_cal) from prior layer's residual stream. */
        rows_rmsnorm_f32(H_cal, H_norm, n, D, eps);
        for (size_t i = 0; i < n * D; i++) Xd[i] = (double)H_norm[i];

        const size_t *hp = (spec->layer_specs && spec->layer_specs[L].head_planes) ? spec->layer_specs[L].head_planes : NULL;
        size_t fp = spec->layer_specs ? spec->layer_specs[L].ffn_plane : (L + 1u == NL ? lm_pi : (L % n_planes));

        compile_layer_per_head(L, NL, n, D, NH, NKV, HD, FF, (size_t)spec->eos_id,
                               Xd, cplanes, n_planes, hp, fp,
                               wq, wk, wv, wo, fg, fu, fd);

        /* Propagate exact Llama residual block on H_cal so next layer & lm_head calibrate to post-layer states. */
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)n, (MKL_INT)kdim, (MKL_INT)D,
                    1.0f, H_norm, (MKL_INT)D, wv, (MKL_INT)D, 0.0f, cal_v, (MKL_INT)kdim);
        size_t gqa_g = NH / NKV;
        for (size_t i = 0; i < n; i++) {
            for (size_t h = 0; h < NH; h++) {
                size_t kv = h / gqa_g;
                memcpy(cal_vfull + i * qdim + h * HD, cal_v + i * kdim + kv * HD, sizeof(float) * HD);
            }
        }
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)n, (MKL_INT)D, (MKL_INT)qdim,
                    1.0f, cal_vfull, (MKL_INT)qdim, wo, (MKL_INT)qdim, 0.0f, cal_out, (MKL_INT)D);
        for (size_t i = 0; i < n * D; i++) H_cal[i] += cal_out[i];

        rows_rmsnorm_f32(H_cal, H_norm, n, D, eps);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)n, (MKL_INT)FF, (MKL_INT)D,
                    1.0f, H_norm, (MKL_INT)D, fg, (MKL_INT)D, 0.0f, cal_g, (MKL_INT)FF);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)n, (MKL_INT)FF, (MKL_INT)D,
                    1.0f, H_norm, (MKL_INT)D, fu, (MKL_INT)D, 0.0f, cal_u, (MKL_INT)FF);
        for (size_t i = 0; i < n * FF; i++) {
            double gv = (double)cal_g[i];
            double silu = gv / (1.0 + exp(-gv));
            cal_g[i] = (float)(silu * (double)cal_u[i]);
        }
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)n, (MKL_INT)D, (MKL_INT)FF,
                    1.0f, cal_g, (MKL_INT)FF, fd, (MKL_INT)FF, 0.0f, cal_out, (MKL_INT)D);
        for (size_t i = 0; i < n * D; i++) H_cal[i] += cal_out[i];

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

    /* Calibrate readout head against exact post-NL RMSNormed residual states arriving at output.weight. */
    rows_rmsnorm_f32(H_cal, H_norm, n, D, eps);
    synthesize_readout(spec, &cplanes[lm_pi], H_norm, n, D, head);

    uint64_t out_norm_d[1] = { (uint64_t)D };
    rc |= emit_tensor(gw, sw, gw ? "output_norm.weight" : "model.norm.weight", out_norm_d, 1, ones, 0);
    if (!spec->tied_embeddings) {
        rc |= emit_tensor(gw, sw, gw ? "output.weight" : "lm_head.weight", emb_dims, 2, head, spec->f16_weights);
    }

    if (gw) { rc |= lp_gguf_writer_finalize(gw); lp_gguf_writer_free(gw); }
    if (sw) {
        rc |= lp_st_writer_finalize(sw);
        lp_st_writer_free(sw);
        if (rc == 0) {
            const char *sep = strrchr(spec->out_path, '/');
            const char *bsep = strrchr(spec->out_path, '\\');
            if (bsep && (!sep || bsep > sep)) sep = bsep;
            size_t dlen = sep ? (size_t)(sep - spec->out_path + 1) : 0u;
            char cfg_path[4096], tok_path[4096];
            if (dlen + 32u < sizeof cfg_path) {
                memcpy(cfg_path, spec->out_path, dlen); strcpy(cfg_path + dlen, "config.json");
                memcpy(tok_path, spec->out_path, dlen); strcpy(tok_path + dlen, "tokenizer.json");
                FILE *cf = fopen(cfg_path, "wb");
                if (cf) {
                    fprintf(cf,
                            "{\n"
                            "  \"architectures\": [\"LlamaForCausalLM\"],\n"
                            "  \"model_type\": \"llama\",\n"
                            "  \"vocab_size\": %zu,\n"
                            "  \"hidden_size\": %zu,\n"
                            "  \"intermediate_size\": %zu,\n"
                            "  \"num_hidden_layers\": %zu,\n"
                            "  \"num_attention_heads\": %zu,\n"
                            "  \"num_key_value_heads\": %zu,\n"
                            "  \"rms_norm_eps\": %.17g,\n"
                            "  \"rope_theta\": %.17g,\n"
                            "  \"bos_token_id\": %u,\n"
                            "  \"eos_token_id\": %u,\n"
                            "  \"tie_word_embeddings\": %s,\n"
                            "  \"laplace_tokenizer_scheme\": \"laplace-blake3-v1\",\n"
                            "  \"laplace_raw_weight_bytes_retained\": 0\n"
                            "}\n",
                            n, D, FF, NL, NH, NKV, eps, rope_base,
                            spec->bos_id, spec->eos_id,
                            spec->tied_embeddings ? "true" : "false");
                    fclose(cf);
                }
                FILE *tf = fopen(tok_path, "wb");
                if (tf) {
                    fputs("{\"version\":\"1.0\",\"model\":{\"type\":\"WordLevel\",\"vocab\":{", tf);
                    for (size_t i = 0; i < n; i++) {
                        if (i > 0) fputc(',', tf);
                        lp_fput_json(tf, spec->tokens[i] ? spec->tokens[i] : "<unk>");
                        fprintf(tf, ":%zu", i);
                    }
                    fputs("},\"unk_token\":\"<unk>\"}}", tf);
                    fclose(tf);
                }
            }
        }
    }

    printf("exported %s (%s): vocab %zu, dim %zu, rank %zu, planes %zu, fisher_bp 10000 (%zu witnesses), raw_weight_bytes_retained = 0\n",
           spec->out_path, spec->format == 1 ? "safetensors" : "gguf", n, D, rank, n_planes, n_witness);

    free(Xd); free(ones); free(wq); free(wk); free(wv); free(wo); free(fg); free(fu); free(fd);
    free(cal_v); free(cal_vfull); free(cal_out); free(cal_g); free(cal_u);
    for (size_t p = 0; p < n_planes; p++) compact_plane_free(&cplanes[p]);
    free(cplanes);
    free(ids16); free(E); free(X); free(H_cal); free(H_norm); free(head);
    for (size_t i = 0; i < n_witness; i++) free(witnesses[i]);
    free(witnesses); free(share_bp);
    return rc ? 1 : 0;

fail_writers:
    for (size_t p = 0; p < n_planes; p++) compact_plane_free(&cplanes[p]);
    free(cplanes);
    free(ids16); free(E); free(X); free(H_cal); free(H_norm); free(head);
    for (size_t i = 0; i < n_witness; i++) free(witnesses[i]);
    free(witnesses); free(share_bp);
    return -1;
}
