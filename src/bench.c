/* laplace bench: throughput of every Laplace-Native operation on this machine, with each SIMD kernel measured next
 * to its scalar form on the same data. Prints one line per operation: rate, time per operation, and bandwidth where
 * it applies. */
#include "engine.h"
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile uint64_t sink;
static void row(const char *what, double ops, double secs, double bytes){
    double rate = ops / secs;
    const char *u = rate >= 1e9 ? "G" : rate >= 1e6 ? "M" : rate >= 1e3 ? "k" : "";
    double s = rate >= 1e9 ? 1e9 : rate >= 1e6 ? 1e6 : rate >= 1e3 ? 1e3 : 1;
    printf("  %-52s %9.2f %s/s   %9.1f ns/op", what, rate / s, u, secs / ops * 1e9);
    if (bytes > 0) printf("   %7.2f GB/s", bytes / secs / 1e9);
    putchar('\n');
}
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void){ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int cmd_bench(int argc, char **argv){
    uint32_t f = lp_cpu_features();
    printf("laplace bench   cpu: %s   dispatch: %s\n\n", lp_cpu_describe(f), lp_cpu_describe(lp_cpu_active()));
    double t;

    printf("identity\n");
    lp_id *ids = malloc(sizeof(lp_id) * LP_NCP);
    t = now(); for (uint32_t cp = 0; cp < LP_NCP; cp++) lp_id_codepoint(cp, &ids[cp]); row("codepoint ID (BLAKE3-128 over UTF-8)", LP_NCP, now() - t, 0);
    lp_id out; size_t N = 2000000;
    for (size_t k = 6; k <= 64; k *= 4) {
        char label[64]; snprintf(label, sizeof label, "composition ID, %zu children", k);
        t = now(); for (size_t i = 0; i < N / k * 6; i++) { lp_id_compose(ids + (i * 7) % (LP_NCP - 64), k, &out); sink += out.b[0]; }
        row(label, N / k * 6, now() - t, (double)(N / k * 6) * k * 16);
    }

    printf("\ngeometry\n");
    double xyz[3];
    t = now(); for (size_t i = 0; i < LP_NCP; i++) { lp_id_to_xyz(&ids[i], xyz); sink += (uint64_t)xyz[0]; } row("ID into X/Y/Z mantissas", LP_NCP, now() - t, 0);
    lp_id back; t = now(); for (size_t i = 0; i < LP_NCP; i++) { lp_id_to_xyz(&ids[i], xyz); lp_xyz_to_id(xyz, &back); sink += back.b[3]; }
    row("ID round trip", LP_NCP, now() - t, 0);
    uint8_t *buf = malloc(64 * 1024 * 1024); size_t pn = 1000000;
    lp_id *path = malloc(sizeof(lp_id) * pn); for (size_t i = 0; i < pn; i++) path[i] = ids[next() % 64];
    t = now(); size_t len = lp_ewkb_path(path, pn, buf, 64 * 1024 * 1024); row("EWKB path, 1M children (per child)", pn, now() - t, len);

    printf("\ncoordinates\n");
    lp_coord *c = malloc(sizeof(lp_coord) * LP_NCP);
    for (size_t i = 0; i < LP_NCP; i++) for (int d = 0; d < 4; d++) c[i].m[d] = (int64_t)(next() >> 11) - (1ll << 52);
    t = now(); for (size_t i = 0; i < LP_NCP; i++) sink += lp_hilbert4(&c[i]); row("4D Hilbert value", LP_NCP, now() - t, 0);
    t = now(); for (size_t i = 0; i < LP_NCP; i++) sink += lp_coord_inside(&c[i]); row("wall check (exact, 128-bit)", LP_NCP, now() - t, 0);
    lp_coord cc; t = now(); for (size_t i = 0; i + 16 < LP_NCP; i += 16) { lp_coord_centroid(c + i, 16, &cc); sink += cc.m[0]; }
    row("exact centroid, 16 points (per point)", LP_NCP, now() - t, 0);

    printf("\ncontinuations: vertex scan over a 1M-vertex path (no match, full pass)\n");
    size_t nv = lp_ewkb_vertices(buf, len, (const uint8_t **)&path) ; const uint8_t *v; nv = lp_ewkb_vertices(buf, len, &v);
    uint8_t key[24]; memset(key, 0x5A, 24); int reps = 20;
    t = now(); for (int r = 0; r < reps; r++) sink += lp_scan_scalar(v, 0, nv, key); row("scalar", (double)nv * reps, now() - t, (double)nv * reps * 32);
    if (f & LP_CPU_AVX2) { t = now(); for (int r = 0; r < reps; r++) sink += lp_scan_avx2(v, 0, nv, key); row("AVX2", (double)nv * reps, now() - t, (double)nv * reps * 32); }
    else printf("  %-52s not supported on this CPU\n", "AVX2");
    if (f & LP_CPU_AVX512) { t = now(); for (int r = 0; r < reps; r++) sink += lp_scan_avx512(v, 0, nv, key); row("AVX-512", (double)nv * reps, now() - t, (double)nv * reps * 32); }
    else printf("  %-52s not supported on this CPU\n", "AVX-512");

    printf("\n4D geometry\n");
    size_t m = 4096; double *bx = malloc(sizeof(double) * m * 5), *by = bx + m, *bz = by + m, *bm = bz + m, *o = bm + m, p[4] = { .1, .2, .3, .4 };
    for (size_t j = 0; j < 4 * m; j++) bx[j] = (double)(next() >> 11) / 9007199254740992.0;
    reps = 2000;
    t = now(); for (int r = 0; r < reps; r++) { lp_row_d2_scalar(p, bx, by, bz, bm, 0, m, o); sink += (uint64_t)o[r % m]; } row("squared distances, scalar (per pair)", (double)m * reps, now() - t, 0);
    if (f & LP_CPU_AVX2) { t = now(); for (int r = 0; r < reps; r++) { lp_row_d2_avx2(p, bx, by, bz, bm, 0, m, o); sink += (uint64_t)o[r % m]; } row("squared distances, AVX2 (per pair)", (double)m * reps, now() - t, 0); }
    for (size_t n = 100; n <= 1000; n *= 10) {
        double *a = malloc(sizeof(double) * 4 * n), *b = malloc(sizeof(double) * 4 * n);
        for (size_t j = 0; j < 4 * n; j++) { a[j] = (double)(next() >> 11) / 9007199254740992.0; b[j] = (double)(next() >> 11) / 9007199254740992.0; }
        char label[64]; snprintf(label, sizeof label, "discrete Frechet, %zu x %zu vertices (per call)", n, n);
        int k = n == 100 ? 2000 : 20; t = now(); for (int r = 0; r < k; r++) sink += (uint64_t)(lp_frechet4(a, n, b, n) * 1e6);
        row(label, k, now() - t, 0); free(a); free(b);
    }

    printf("\nshape measures, 1,000 x 1,000 vertices (per call)\n");
    { size_t n = 1000; double *a = malloc(sizeof(double) * 4 * n), *b = malloc(sizeof(double) * 4 * n);
      for (size_t j = 0; j < 4 * n; j++) { a[j] = (double)(next() >> 11) / 9007199254740992.0; b[j] = (double)(next() >> 11) / 9007199254740992.0; }
      t = now(); for (int r = 0; r < 20; r++) sink += (uint64_t)(lp_dtw4(a, n, b, n, NULL) * 1e6); row("DTW", 20, now() - t, 0);
      t = now(); for (int r = 0; r < 20; r++) sink += lp_edr4(a, n, b, n, 0.1); row("EDR", 20, now() - t, 0);
      t = now(); for (int r = 0; r < 5; r++) sink += (uint64_t)(lp_frechet4_outliers(a, n, b, n, 1) * 1e6); row("Frechet, 1 outlier skipped", 5, now() - t, 0);
      free(a); free(b); }

    printf("\ntext and the pull\n");
    { const lp_tier0_record *t0 = lp_tier0_map(NULL);
      if (!t0) printf("  tier 0 is not at %s: text is not measured\n", lp_tier0_path());
      else {
          lp_text *tx = lp_text_new(t0); static const char para[] = "It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in want of a wife. However little known the feelings or views of such a man may be on his first entering a neighbourhood, this truth is so well fixed in the minds of the surrounding families, that he is considered the rightful property of some one or other of their daughters.";
          size_t len = sizeof para - 1; int reps = 20000;
          t = now(); for (int r = 0; r < reps; r++) sink += lp_text_decompose(tx, (const uint8_t *)para, len, NULL, NULL).id.b[0];
          row("text to its trunk: ID and coordinate (per call)", reps, now() - t, (double)len * reps);
          t = now(); for (uint32_t cp = 0; cp < LP_NCP; cp++) sink += (uint64_t)lp_tier0_codepoint(t0, &ids[cp]); row("codepoint of an ID", LP_NCP, now() - t, 0);
          lp_text_free(tx);
      }
      lp_frontier *f = lp_frontier_new(); size_t nf = 1000000; lp_rating rr = { 1700, 80, 0.06 };
      t = now(); for (size_t i = 0; i < nf; i++) lp_frontier_reach(f, &ids[i], NULL, NULL, (double)(next() >> 40) + lp_cost(&rr, 2, 0.05), 0, 1);
      while (lp_frontier_next(f)) sink++;
      row("frontier: reach and take, 1M entities (per entity)", nf, now() - t, 0); lp_frontier_free(f); }

    printf("\nconsensus\n");
    lp_rating rt = { 1500, 350, 0.06 }; N = 1000000;
    t = now(); for (size_t i = 0; i < N; i++) { lp_attest(&rt, 0.9, (double)(i & 1), 1500, 0.5, 30); } row("Glicko-2 attestation (one matchup)", N, now() - t, 0);
    sink += (uint64_t)rt.rating;
    (void)argc; (void)argv;
    return 0;
}
