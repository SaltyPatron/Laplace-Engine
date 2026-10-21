/* File packaging identities from OS metadata and the compiled reading contract. */
#define _GNU_SOURCE
#include "file_metadata.h"
#include <blake3.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const Recipe *stock_file;
void file_record_stock(const Recipe *recipe){ stock_file = recipe; }

static uint64_t record_open;                                                  /* the parts no recipe disposed of, said once */
/* Whether a part of the OS's record is kept in the metadata tree. Asked before its value is made: a value is composed
 * into the node table as it is made, and one the recipe omits (the access time, which reading the file changes) would
 * leave nodes of its own there, different on every read. */
static int record_kept(const Recipe *r, int k, const char *name){
    FileMetadataPolicy policy = recipe_file_metadata_policy(r, name);
    if (policy == FILE_METADATA_UNSET && stock_file) policy = recipe_file_metadata_policy(stock_file, name);
    if (policy == FILE_METADATA_UNSET) { if (!(__atomic_fetch_or(&record_open, 1ull << k, __ATOMIC_RELAXED) & (1ull << k))) fprintf(stderr, "  the OS's record of a file: %s is disposed of by no recipe (metadata or omit, in the file recipe)\n", name); return 0; }
    return policy == FILE_METADATA_KEEP;
}
static void record_part(const Recipe *r, Ref *out, size_t *n, size_t cap, int k, const char *name, Ref value){
    (void)r; (void)k; if (*n >= cap) return;
    Ref p[2] = { string_ref((const uint8_t *)name, strlen(name)), value }; p[0].said = p[1].said = 0;
    out[(*n)++] = said_tuple(compose(p, 2, ref_above(p, 2)));
}
static size_t fmt_u64(uint64_t v, char *out){
    char tmp[24]; int k = 0;
    do { tmp[k++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    return (size_t)k;
}
static Ref number_ref(uint64_t v){ char b[24]; size_t l = fmt_u64(v, b); return string_ref((const uint8_t *)b, l); }
static Ref when_ref(const struct statx_timestamp *t){
    char b[40]; size_t k = 0; long long sec = (long long)t->tv_sec;
    if (sec < 0) { b[k++] = '-'; sec = -sec; }
    k += fmt_u64((uint64_t)sec, b + k);
    b[k++] = '.';
    uint32_t ns = t->tv_nsec;
    for (int i = 8; i >= 0; i--) { b[k + (size_t)i] = (char)('0' + (ns % 10)); ns /= 10; }
    k += 9;
    return string_ref((const uint8_t *)b, k);
}
size_t file_record(const Recipe *r, const File *f, Ref *out, size_t cap){
    size_t n = 0; int k = 0; Ref seg[256]; uint32_t ns = 0; const char *p = f->path;
    while (*p && ns < 256) { const char *e = strchr(p, '/'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l) { seg[ns] = string_ref((const uint8_t *)p, l); seg[ns].said = 0; ns++; } p += l; if (*p == '/') p++; }
    if (ns) { if (record_kept(r, k, "pathname")) { Ref path = compose(seg, ns, ref_above(seg, ns)); path.said = 0; record_part(r, out, &n, cap, k, "pathname", path); } k++;
              if (record_kept(r, k, "filename")) record_part(r, out, &n, cap, k, "filename", seg[ns - 1]); k++; }
    struct statx x; if (statx(AT_FDCWD, f->path, 0, STATX_BASIC_STATS | STATX_BTIME, &x)) return n;
    #define NUM(F, M) do { if ((!(M) || (x.stx_mask & (M))) && record_kept(r, k, #F)) record_part(r, out, &n, cap, k, #F, number_ref((uint64_t)x.F)); k++; } while (0)
    #define WHEN(F, M) do { if ((x.stx_mask & (M)) && record_kept(r, k, #F)) record_part(r, out, &n, cap, k, #F, when_ref(&x.F)); k++; } while (0)
    NUM(stx_mode, STATX_MODE); NUM(stx_uid, STATX_UID); NUM(stx_gid, STATX_GID); NUM(stx_nlink, STATX_NLINK); NUM(stx_ino, STATX_INO);
    NUM(stx_size, STATX_SIZE); NUM(stx_blocks, STATX_BLOCKS); NUM(stx_blksize, 0); NUM(stx_attributes, 0);
    NUM(stx_dev_major, 0); NUM(stx_dev_minor, 0); NUM(stx_rdev_major, 0); NUM(stx_rdev_minor, 0);
    WHEN(stx_atime, STATX_ATIME); WHEN(stx_btime, STATX_BTIME); WHEN(stx_ctime, STATX_CTIME); WHEN(stx_mtime, STATX_MTIME);
    #undef NUM
    #undef WHEN
    return n;
}
/* The OS's record of a file as one node, the first part of its metadata tree: computed from stat alone, before a byte
 * of the file is read, so a file whose trunk is recorded is found by it (ingest.c, files_recorded). 0: none. */
int file_os(const Recipe *r, const File *f, Ref *out){
    Ref m[32]; size_t n = file_record(r, f, m, 32); if (!n) return 0;
    uint8_t environment[32], program[32], stock[32], digest[32];
    program_environment_hash(environment); recipe_reading_hash(r, program); recipe_reading_hash(stock_file, stock);
    blake3_hasher hash; blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, environment, sizeof environment); blake3_hasher_update(&hash, program, sizeof program);
    blake3_hasher_update(&hash, stock, sizeof stock); blake3_hasher_finalize(&hash, digest, sizeof digest);
    static const char hex[] = "0123456789abcdef"; uint8_t text[64];
    for (unsigned i = 0; i < sizeof digest; i++) { text[2 * i] = hex[digest[i] >> 4]; text[2 * i + 1] = hex[digest[i] & 15]; }
    if (n == 32) { fputs("file metadata has no room for its reading contract\n", stderr); exit(1); }
    Ref contract[2] = {string_ref((const uint8_t *)"reading contract", 16), string_ref(text, sizeof text)};
    m[n++] = said_tuple(compose(contract, 2, ref_above(contract, 2)));
    *out = compose(m, (uint32_t)n, ref_above(m, n)); out->said = 0; return 1;
}
/* The file, read: every part of its outermost tier on every core, joined in the file's order; then its trunk, over
 * its metadata tree and its content tree. */
