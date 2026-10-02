/* laplace tier0: generate tier 0 natively from the Unicode data.
 *
 * For every one of the 1,114,112 codepoints:
 *   rank        DUCET total order from uca/allkeys.txt (explicit entries; Hangul via jamo; @implicitweights, core
 *               and other Han, and the catch-all), non-ignorable L1|L2|L3 sort key, ties broken by NFD, then by
 *               codepoint;
 *   coordinate  Super-Fibonacci points (n = 1,114,112) walked in 4D Hilbert order; rank r takes the r-th point;
 *               fixed point m = rint(x * 2^53), moved toward zero one unit at a time until m.m <= 2^106 exactly;
 *   ID          BLAKE3-128 of the codepoint's UTF-8 bytes (lp_id_codepoint);
 *   hilbert     4D Hilbert value of the coordinate.
 * Writes 64-byte records indexed by codepoint, and prints the build fingerprint (BLAKE3-256 of the table and of the
 * order). Usage: laplace tier0 [-u UCD_ROOT] [-o tier0.bin]; by default it reads $LAPLACE_UCD and writes $LAPLACE_TIER0. */
#include "engine.h"
#include "blake3.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double cr_sin(double), cr_cos(double);          /* CORE-MATH: correctly rounded */

#define N LP_NCP
static double T_start;
static void say(const char *m){ printf("[%6.1fs] %s\n", now() - T_start, m); fflush(stdout); }
static void *xmalloc(size_t n){ void *p = calloc(1, n ? n : 1); if (!p) { perror("calloc"); exit(1); } return p; }
static FILE *open_in(const char *root, const char *rel){
    char p[4096]; snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "r"); if (!f) { perror(p); exit(1); } return f;
}
/* "XXXX" or "XXXX..YYYY" at the start of a UCD data line. */
static int range_of(const char *s, uint32_t *lo, uint32_t *hi){
    char *e; unsigned long a = strtoul(s, &e, 16); if (e == s) return 0;
    *lo = *hi = (uint32_t)a;
    if (e[0] == '.' && e[1] == '.') *hi = (uint32_t)strtoul(e + 2, NULL, 16);
    return 1;
}
static const char *field(const char *s){ const char *p = strchr(s, ';'); if (!p) return NULL; p++; while (*p == ' ') p++; return p; }

/* ---- UCD properties the collation needs */
static uint8_t *assigned;      /* general category other than Cn and Cs: a <char> in the UCD XML */
static uint8_t *uideo;         /* Unified_Ideograph */
static uint8_t *hancore;       /* in the CJK Unified Ideographs or CJK Compatibility Ideographs block */
static uint8_t *ccc;           /* canonical combining class */
static uint32_t *dec_off; static uint8_t *dec_len; static uint32_t *dec_pool; static size_t dec_n;   /* canonical decompositions */

static void load_ucd(const char *root){
    char line[8192]; uint32_t lo, hi; FILE *f;
    assigned = xmalloc(N); uideo = xmalloc(N); hancore = xmalloc(N); ccc = xmalloc(N);
    dec_off = xmalloc(4 * N); dec_len = xmalloc(N); dec_pool = xmalloc(4 * 64 * 1024);
    f = open_in(root, "ucd/extracted/DerivedGeneralCategory.txt");
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || !range_of(line, &lo, &hi)) continue;
        const char *v = field(line); if (!v) continue;
        int un = !strncmp(v, "Cn", 2) || !strncmp(v, "Cs", 2);
        for (uint32_t c = lo; c <= hi; c++) assigned[c] = !un;
    }
    fclose(f);
    f = open_in(root, "ucd/PropList.txt");
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || !range_of(line, &lo, &hi)) continue;
        const char *v = field(line); if (!v || strncmp(v, "Unified_Ideograph", 17) || (v[17] != ' ' && v[17] != '#')) continue;
        for (uint32_t c = lo; c <= hi; c++) uideo[c] = 1;
    }
    fclose(f);
    f = open_in(root, "ucd/Blocks.txt");
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || !range_of(line, &lo, &hi)) continue;
        const char *v = field(line); if (!v) continue;
        if (!strncmp(v, "CJK Unified Ideographs\n", 23) || !strncmp(v, "CJK Unified Ideographs\r", 23) ||
            !strncmp(v, "CJK Compatibility Ideographs\n", 29) || !strncmp(v, "CJK Compatibility Ideographs\r", 29))
            for (uint32_t c = lo; c <= hi; c++) hancore[c] = 1;
    }
    fclose(f);
    f = open_in(root, "ucd/UnicodeData.txt");
    while (fgets(line, sizeof line, f)) {
        char *fl[16]; int nf = 0; char *p = line; fl[nf++] = p;
        while ((p = strchr(p, ';')) && nf < 16) { *p++ = 0; fl[nf++] = p; }
        if (nf < 6) continue;
        uint32_t cp = (uint32_t)strtoul(fl[0], NULL, 16);
        ccc[cp] = (uint8_t)atoi(fl[3]);
        if (fl[5][0] && fl[5][0] != '<') {
            dec_off[cp] = (uint32_t)dec_n; char *q = fl[5], *e;
            for (;;) { unsigned long x = strtoul(q, &e, 16); if (e == q) break; dec_pool[dec_n++] = (uint32_t)x; dec_len[cp]++; q = e; }
        }
    }
    fclose(f);
}
/* Full canonical decomposition of one codepoint, with algorithmic Hangul, then canonical ordering. */
static int nfd_full(uint32_t cp, uint32_t *out){
    if (cp >= 0xAC00 && cp < 0xAC00 + 11172) {
        uint32_t s = cp - 0xAC00; int k = 0;
        out[k++] = 0x1100 + s / 588; out[k++] = 0x1161 + (s % 588) / 28;
        if (s % 28) out[k++] = 0x11A7 + s % 28;
        return k;
    }
    if (!dec_len[cp]) { out[0] = cp; return 1; }
    int k = 0;
    for (int i = 0; i < dec_len[cp]; i++) k += nfd_full(dec_pool[dec_off[cp] + i], out + k);
    return k;
}
static int nfd(uint32_t cp, uint32_t *out){
    int n = nfd_full(cp, out);
    for (int i = 0; i < n; ) {                                 /* stable sort of each run of non-starters by class */
        if (!ccc[out[i]]) { i++; continue; }
        int j = i; while (j < n && ccc[out[j]]) j++;
        for (int a = i + 1; a < j; a++) { uint32_t x = out[a]; int b = a; while (b > i && ccc[out[b - 1]] > ccc[x]) { out[b] = out[b - 1]; b--; } out[b] = x; }
        i = j;
    }
    return n;
}

/* ---- collation keys: per codepoint its L1, L2 and L3 sequences of nonzero weights, then its NFD */
typedef struct { uint32_t off; uint8_t n1, n2, n3, nd; } Key;
static Key *key; static uint16_t *kpool; static size_t kn, kcap;
static uint32_t *npool; static size_t nn;
static void kput(uint16_t w){ if (kn == kcap) { kcap = kcap ? kcap * 2 : (1u << 22); kpool = realloc(kpool, kcap * 2); } kpool[kn++] = w; }

typedef struct { uint16_t p, s, t; } CE;
static CE *single_ce; static uint32_t *single_off; static uint8_t *single_n; static size_t ce_n, ce_cap;
typedef struct { uint32_t lo, hi, base; } Implicit;
static Implicit imp[64]; static int nimp;

static void load_allkeys(const char *root){
    char line[8192]; FILE *f = open_in(root, "uca/allkeys.txt");
    single_off = xmalloc(4 * N); single_n = xmalloc(N);
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '\n' || *s == '\r') continue;
        if (!strncmp(s, "@implicitweights", 16)) {
            uint32_t lo, hi; char *p = s + 16; while (*p == ' ') p++;
            range_of(p, &lo, &hi); const char *v = field(p);
            imp[nimp++] = (Implicit){ lo, hi, (uint32_t)strtoul(v, NULL, 16) }; continue;
        }
        if (*s == '@') continue;
        char *semi = strchr(s, ';'); if (!semi) continue; *semi = 0;
        char *e; uint32_t cp = (uint32_t)strtoul(s, &e, 16);
        while (*e == ' ') e++;
        if (*e) continue;                                      /* contractions: more than one codepoint */
        single_off[cp] = (uint32_t)ce_n;
        for (char *q = semi + 1; (q = strchr(q, '[')); q++) {
            if (ce_n == ce_cap) { ce_cap = ce_cap ? ce_cap * 2 : 65536; single_ce = realloc(single_ce, ce_cap * sizeof(CE)); }
            unsigned p, sc, t; if (sscanf(q + 2, "%4x.%4x.%4x", &p, &sc, &t) != 3) continue;
            single_ce[ce_n++] = (CE){ (uint16_t)p, (uint16_t)sc, (uint16_t)t }; single_n[cp]++;
        }
        if (!single_n[cp]) single_n[cp] = 0xFF;                /* explicit, with no weights: completely ignorable */
    }
    fclose(f);
}
static int ces_of(uint32_t cp, CE *out){
    if (single_n[cp]) { int n = single_n[cp] == 0xFF ? 0 : single_n[cp]; memcpy(out, single_ce + single_off[cp], n * sizeof(CE)); return n; }
    if (cp >= 0xAC00 && cp <= 0xD7A3) {                        /* Hangul syllable: the weights of its jamo */
        uint32_t s = cp - 0xAC00, seq[3] = { 0x1100 + s / 588, 0x1161 + (s % 588) / 28, 0x11A7 + s % 28 }; int n = 0;
        for (int j = 0; j < (seq[2] != 0x11A7 ? 3 : 2); j++) { int m = single_n[seq[j]] == 0xFF ? 0 : single_n[seq[j]]; memcpy(out + n, single_ce + single_off[seq[j]], m * sizeof(CE)); n += m; }
        return n;
    }
    for (int i = 0; i < nimp; i++)                             /* siniform scripts with implicit weights */
        if (cp >= imp[i].lo && cp <= imp[i].hi && assigned[cp]) {
            uint32_t first = imp[i].lo; for (int j = 0; j < nimp; j++) if (imp[j].base == imp[i].base && imp[j].lo < first) first = imp[j].lo;
            out[0] = (CE){ (uint16_t)imp[i].base, 0x20, 2 }; out[1] = (CE){ (uint16_t)((cp - first) | 0x8000), 0, 0 }; return 2;
        }
    uint32_t aaaa = uideo[cp] ? (hancore[cp] ? 0xFB40 : 0xFB80) + (cp >> 15) : 0xFBC0 + (cp >> 15);
    out[0] = (CE){ (uint16_t)aaaa, 0x20, 2 }; out[1] = (CE){ (uint16_t)((cp & 0x7FFF) | 0x8000), 0, 0 }; return 2;
}
static void build_keys(void){
    key = xmalloc(sizeof(Key) * N); npool = xmalloc(4 * 8 * (size_t)N);
    CE ce[64]; uint32_t d[64];
    for (uint32_t cp = 0; cp < N; cp++) {
        int n = ces_of(cp, ce); Key *k = &key[cp]; k->off = (uint32_t)kn;
        for (int i = 0; i < n; i++) if (ce[i].p) { kput(ce[i].p); k->n1++; }
        for (int i = 0; i < n; i++) if (ce[i].s) { kput(ce[i].s); k->n2++; }
        for (int i = 0; i < n; i++) if (ce[i].t) { kput(ce[i].t); k->n3++; }
        int m = nfd(cp, d); k->nd = (uint8_t)m; memcpy(npool + nn, d, 4 * m); nn += m;
    }
}
static uint32_t *nfd_off;
static int seqcmp16(const uint16_t *a, int na, const uint16_t *b, int nb){
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return na < nb ? -1 : na > nb;
}
static int keycmp(const void *pa, const void *pb){
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb; const Key *x = &key[a], *y = &key[b]; int c;
    const uint16_t *xa = kpool + x->off, *ya = kpool + y->off;
    if ((c = seqcmp16(xa, x->n1, ya, y->n1))) return c;
    if ((c = seqcmp16(xa + x->n1, x->n2, ya + y->n1, y->n2))) return c;
    if ((c = seqcmp16(xa + x->n1 + x->n2, x->n3, ya + y->n1 + y->n2, y->n3))) return c;
    const uint32_t *dx = npool + nfd_off[a], *dy = npool + nfd_off[b]; int n = x->nd < y->nd ? x->nd : y->nd;
    for (int i = 0; i < n; i++) if (dx[i] != dy[i]) return dx[i] < dy[i] ? -1 : 1;
    if (x->nd != y->nd) return x->nd < y->nd ? -1 : 1;
    return a < b ? -1 : a > b;
}

/* ---- placement */
typedef struct { uint64_t h; uint32_t i; } HI;
static int hicmp(const void *pa, const void *pb){
    const HI *a = pa, *b = pb;
    if (a->h != b->h) return a->h < b->h ? -1 : 1;
    return a->i < b->i ? -1 : a->i > b->i;                     /* stable: equal values keep index order */
}
static uint64_t hilbert_x(const double x[4]){
    uint32_t g[4];
    for (int d = 0; d < 4; d++) { double v = (x[d] + 1.0) / 2.0 * 65536.0; g[d] = v < 0 ? 0 : v > 65535.0 ? 65535u : (uint32_t)v; }
    return lp_hilbert4_grid(g);
}

int cmd_tier0(int argc, char **argv){
    char dflt[4096]; snprintf(dflt, sizeof dflt, "%s/Public/UCD/latest", laplace_ucd());
    const char *root = dflt, *outp = lp_tier0_path();
    if (opts(argc, argv, (const Opt[]){ { "-u", 's', &root }, { "-o", 's', &outp }, { NULL } }) < argc) { fprintf(stderr, "usage: laplace tier0 [-u UCD_ROOT] [-o tier0.bin]\n"); return 2; }
    T_start = now();
    load_ucd(root); say("UCD properties and decompositions read");
    load_allkeys(root); say("allkeys.txt read");
    build_keys();
    nfd_off = xmalloc(4 * (size_t)N); { uint32_t o = 0; for (uint32_t c = 0; c < N; c++) { nfd_off[c] = o; o += key[c].nd; } }
    uint32_t *order = xmalloc(4 * (size_t)N); for (uint32_t c = 0; c < N; c++) order[c] = c;
    qsort(order, N, 4, keycmp);
    uint32_t *rank = xmalloc(4 * (size_t)N); for (uint32_t r = 0; r < N; r++) rank[order[r]] = r;
    say("DUCET total order built");

    /* Super-Fibonacci (Alexa, CVPR 2022), n = N, in the exact operation order of the specification's reference. */
    const double phi = sqrt(2.0), psi = 1.533751168755204288118041, twopi = 2.0 * 3.141592653589793;
    double (*S)[4] = xmalloc(sizeof(double[4]) * N); HI *hi = xmalloc(sizeof(HI) * N);
    for (uint32_t i = 0; i < N; i++) {
        double s = (double)i + 0.5, t = s / (double)N, r = sqrt(t), R = sqrt(1.0 - t);
        double al = twopi * s / phi, be = twopi * s / psi;
        S[i][0] = r * cr_sin(al); S[i][1] = r * cr_cos(al); S[i][2] = R * cr_sin(be); S[i][3] = R * cr_cos(be);
        hi[i] = (HI){ hilbert_x(S[i]), i };
    }
    qsort(hi, N, sizeof(HI), hicmp);                           /* the points in Hilbert order */
    say("Super-Fibonacci points walked in Hilbert order");

    lp_tier0_record *rec = xmalloc(sizeof(lp_tier0_record) * N); uint64_t nudged = 0;
    const __int128 LIMIT = (__int128)1 << 106;
    for (uint32_t cp = 0; cp < N; cp++) {
        const double *x = S[hi[rank[cp]].i]; int64_t m[4];
        for (int d = 0; d < 4; d++) m[d] = (int64_t)nearbyint(x[d] * LP_FIXED_ONE);
        for (;;) {
            __int128 q = 0; for (int d = 0; d < 4; d++) q += (__int128)m[d] * m[d];
            if (q <= LIMIT) break;
            int j = 0; for (int d = 1; d < 4; d++) if (llabs(m[d]) > llabs(m[j])) j = d;
            m[j] -= m[j] > 0 ? 1 : -1; nudged++;
        }
        lp_tier0_record *o = &rec[cp]; lp_id_codepoint(cp, &o->id); memcpy(o->m, m, 32);
        lp_coord c; memcpy(c.m, m, 32); o->hilbert = lp_hilbert4(&c); o->rank = rank[cp]; o->pad = 0;
    }
    char msg[128]; snprintf(msg, sizeof msg, "coordinates fixed to the exact grid (%llu units moved inward), IDs hashed", (unsigned long long)nudged); say(msg);

    FILE *f = fopen(outp, "wb"); if (!f || fwrite(rec, sizeof *rec, N, f) != N) { perror(outp); return 1; } fclose(f);
    uint8_t fp[32], of[32]; blake3_hasher h;
    blake3_hasher_init(&h); blake3_hasher_update(&h, rec, sizeof *rec * (size_t)N); blake3_hasher_finalize(&h, fp, 32);
    blake3_hasher_init(&h); for (uint32_t r = 0; r < N; r++) { uint8_t b[3] = { order[r] >> 16, order[r] >> 8, order[r] }; blake3_hasher_update(&h, b, 3); } blake3_hasher_finalize(&h, of, 32);
    printf("%s: %u records x %zu bytes\nfingerprint (BLAKE3-256 of the table) ", outp, N, sizeof *rec);
    for (int i = 0; i < 32; i++) printf("%02x", fp[i]);
    printf("\nDUCET order (BLAKE3-256)                 "); for (int i = 0; i < 32; i++) printf("%02x", of[i]);
    printf("\n"); say("done");
    return 0;
}
