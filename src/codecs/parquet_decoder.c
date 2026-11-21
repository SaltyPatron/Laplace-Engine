#include "codecs.h"
#include "laplace/os.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <zstd.h>

/* ---- Apache Parquet -> backslash-escaped TSV InputDecoder
 * Supports Thrift Compact Protocol FileMetaData + PageHeader (v1/v2),
 * physical types BOOLEAN, INT32, INT64, INT96, FLOAT, DOUBLE, BYTE_ARRAY, FIXED_LEN_BYTE_ARRAY,
 * codecs UNCOMPRESSED (0), SNAPPY (1), GZIP (2), ZSTD (3),
 * and encodings PLAIN (0), PLAIN_DICTIONARY (2), RLE (3), RLE_DICTIONARY (8). */

enum {
    PQ_BOOL = 0, PQ_I32 = 1, PQ_I64 = 2, PQ_I96 = 3,
    PQ_F32 = 4, PQ_F64 = 5, PQ_BYTES = 6, PQ_FIXED = 7
};
enum {
    PQ_ENC_PLAIN = 0, PQ_ENC_PLAIN_DICT = 2, PQ_ENC_RLE = 3,
    PQ_ENC_BIT_PACKED = 4, PQ_ENC_RLE_DICT = 8
};
enum {
    PQ_CODEC_NONE = 0, PQ_CODEC_SNAPPY = 1, PQ_CODEC_GZIP = 2, PQ_CODEC_ZSTD = 3
};

static uint16_t rd_le16(const uint8_t *p){ return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd_le32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_le64(const uint8_t *p){
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

/* ---- Pure-C Snappy block decompressor ---- */
static int snappy_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf, size_t *out_len){
    size_t pos = 0;
    uint32_t ulen = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (pos >= src_len) return 0;
        uint8_t b = src[pos++];
        ulen |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) break;
        if (shift >= 28) return 0;
    }
    uint8_t *dst = malloc(ulen ? ulen : 1u);
    if (!dst) return 0;
    size_t dpos = 0;
    while (pos < src_len) {
        uint8_t tag = src[pos++];
        uint8_t kind = tag & 3u;
        if (kind == 0u) {
            uint32_t len = (tag >> 2) & 0x3Fu;
            if (len < 60u) {
                len += 1u;
            } else {
                uint32_t nb = len - 59u;
                if (nb > 4u || pos + nb > src_len) { free(dst); return 0; }
                len = 0;
                for (uint32_t k = 0; k < nb; k++) len |= (uint32_t)src[pos++] << (8u * k);
                len += 1u;
            }
            if (len > src_len - pos || len > (size_t)ulen - dpos) { free(dst); return 0; }
            memcpy(dst + dpos, src + pos, len);
            pos += len;
            dpos += len;
        } else {
            uint32_t len = 0, off = 0;
            if (kind == 1u) {
                if (pos >= src_len) { free(dst); return 0; }
                len = ((uint32_t)(tag >> 2) & 7u) + 4u;
                off = (((uint32_t)tag & 0xE0u) << 3) | src[pos++];
            } else if (kind == 2u) {
                if (pos + 2u > src_len) { free(dst); return 0; }
                len = (uint32_t)(tag >> 2) + 1u;
                off = rd_le16(src + pos);
                pos += 2u;
            } else {
                if (pos + 4u > src_len) { free(dst); return 0; }
                len = (uint32_t)(tag >> 2) + 1u;
                off = rd_le32(src + pos);
                pos += 4u;
            }
            if (off == 0u || off > dpos || len > (size_t)ulen - dpos) { free(dst); return 0; }
            for (uint32_t k = 0; k < len; k++) dst[dpos + k] = dst[dpos - off + k];
            dpos += len;
        }
    }
    if (dpos != ulen) { free(dst); return 0; }
    *out_buf = dst;
    *out_len = ulen;
    return 1;
}

static int decompress_page(int codec, const uint8_t *src, size_t slen, size_t expected_ulen,
                           uint8_t **out, size_t *out_len){
    *out = NULL;
    *out_len = 0;
    if (codec == PQ_CODEC_NONE) {
        uint8_t *b = malloc(slen ? slen : 1u);
        if (!b) return 0;
        if (slen) memcpy(b, src, slen);
        *out = b; *out_len = slen;
        return 1;
    }
    if (codec == PQ_CODEC_SNAPPY) return snappy_decompress(src, slen, out, out_len);
    if (codec == PQ_CODEC_ZSTD) {
        uint8_t *b = malloc(expected_ulen ? expected_ulen : 1u);
        if (!b) return 0;
        size_t got = ZSTD_decompress(b, expected_ulen, src, slen);
        if (ZSTD_isError(got)) { free(b); return 0; }
        *out = b; *out_len = got;
        return 1;
    }
    if (codec == PQ_CODEC_GZIP) {
        size_t cap = expected_ulen ? expected_ulen : (slen * 4u + 64u);
        uint8_t *b = malloc(cap);
        if (!b) return 0;
        z_stream zs = { 0 };
        if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) { free(b); return 0; }
        zs.next_in = (Bytef *)(uintptr_t)src;
        zs.avail_in = (uInt)slen;
        for (;;) {
            if (zs.total_out >= cap) {
                cap *= 2u;
                b = xrealloc(b, cap);
            }
            zs.next_out = b + zs.total_out;
            zs.avail_out = (uInt)(cap - zs.total_out);
            int rc = inflate(&zs, Z_NO_FLUSH);
            if (rc == Z_STREAM_END) break;
            if (rc != Z_OK) { inflateEnd(&zs); free(b); return 0; }
        }
        *out = b;
        *out_len = zs.total_out;
        inflateEnd(&zs);
        return 1;
    }
    return 0;
}

/* ---- Thrift Compact Protocol reader ---- */
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
} TCur;

static int t_uvarint(TCur *c, uint64_t *out){
    uint64_t r = 0;
    for (int sh = 0; sh < 64; sh += 7) {
        if (c->pos >= c->len) return 0;
        uint8_t b = c->p[c->pos++];
        r |= (uint64_t)(b & 0x7Fu) << sh;
        if (!(b & 0x80u)) { *out = r; return 1; }
    }
    return 0;
}

static int t_zigzag(uint64_t u, int64_t *out){
    *out = (int64_t)(u >> 1) ^ -(int64_t)(u & 1u);
    return 1;
}

static int t_i32(TCur *c, int32_t *out){
    uint64_t u = 0; int64_t v = 0;
    if (!t_uvarint(c, &u)) return 0;
    t_zigzag(u, &v);
    *out = (int32_t)v;
    return 1;
}

static int t_i64(TCur *c, int64_t *out){
    uint64_t u = 0;
    if (!t_uvarint(c, &u)) return 0;
    return t_zigzag(u, out);
}

static int t_field(TCur *c, int16_t *last_id, int16_t *fid, uint8_t *ftype){
    if (c->pos >= c->len) return 0;
    uint8_t b = c->p[c->pos++];
    if (b == 0u) { *ftype = 0; return 1; }
    uint8_t delta = (b >> 4) & 0x0Fu;
    *ftype = b & 0x0Fu;
    if (delta) *fid = (int16_t)(*last_id + delta);
    else {
        int32_t id32 = 0;
        if (!t_i32(c, &id32)) return 0;
        *fid = (int16_t)id32;
    }
    *last_id = *fid;
    return 1;
}

static int t_list_hdr(TCur *c, uint8_t *etype, uint32_t *count){
    if (c->pos >= c->len) return 0;
    uint8_t b = c->p[c->pos++];
    *etype = b & 0x0Fu;
    uint32_t sz = (b >> 4) & 0x0Fu;
    if (sz == 15u) {
        uint64_t u = 0;
        if (!t_uvarint(c, &u)) return 0;
        sz = (uint32_t)u;
    }
    *count = sz;
    return 1;
}

static int t_skip(TCur *c, uint8_t type, int depth){
    if (depth > 32) return 0;
    switch (type) {
    case 1: case 2: return 1;
    case 3: if (c->pos >= c->len) return 0; c->pos++; return 1;
    case 4: case 5: case 6: { uint64_t dummy; return t_uvarint(c, &dummy); }
    case 7: if (8u > c->len - c->pos) return 0; c->pos += 8u; return 1;
    case 8: {
        uint64_t n = 0;
        if (!t_uvarint(c, &n) || n > c->len - c->pos) return 0;
        c->pos += (size_t)n;
        return 1;
    }
    case 9: case 10: {
        uint8_t et = 0; uint32_t cnt = 0;
        if (!t_list_hdr(c, &et, &cnt)) return 0;
        for (uint32_t i = 0; i < cnt; i++) if (!t_skip(c, et, depth + 1)) return 0;
        return 1;
    }
    case 11: {
        uint64_t sz = 0;
        if (!t_uvarint(c, &sz)) return 0;
        if (!sz) return 1;
        if (c->pos >= c->len) return 0;
        uint8_t kv = c->p[c->pos++];
        uint8_t kt = (kv >> 4) & 0x0Fu, vt = kv & 0x0Fu;
        for (uint64_t i = 0; i < sz; i++) {
            if (!t_skip(c, kt, depth + 1) || !t_skip(c, vt, depth + 1)) return 0;
        }
        return 1;
    }
    case 12: {
        int16_t last = 0, fid = 0; uint8_t ft = 0;
        while (t_field(c, &last, &fid, &ft) && ft != 0) {
            if (!t_skip(c, ft, depth + 1)) return 0;
        }
        return ft == 0;
    }
    default: return 0;
    }
}

/* ---- Parquet metadata & page structures ---- */
typedef struct {
    char   *name;
    int32_t type;
    int32_t type_len;
    int32_t rep_type;
    int32_t num_children;
    int32_t has_type;
} PqSchema;

typedef struct {
    char   *name;
    int32_t phys_type;
    int32_t type_len;
    int32_t max_def;
    int32_t max_rep;
} PqLeaf;

typedef struct {
    int32_t type;
    int32_t codec;
    int64_t num_values;
    int64_t total_compressed_size;
    int64_t data_page_offset;
    int64_t dict_page_offset;
    int     has_dict;
    char   *path_col;
} PqColChunk;

typedef struct {
    int64_t     num_rows;
    PqColChunk *cols;
    size_t      ncols;
} PqRowGroup;

typedef struct {
    int32_t   type;
    int32_t   uncomp_sz;
    int32_t   comp_sz;
    int32_t   num_values;
    int32_t   encoding;
    int32_t   def_enc;
    int32_t   rep_enc;
    int32_t   def_bytes_v2;
    int32_t   rep_bytes_v2;
    int       is_compressed;
} PqPageHdr;

static int parse_schema_elem(TCur *c, PqSchema *s){
    memset(s, 0, sizeof *s);
    s->type = -1;
    s->rep_type = -1;
    int16_t last = 0, fid = 0; uint8_t ft = 0;
    while (t_field(c, &last, &fid, &ft) && ft != 0) {
        if (fid == 1 && (ft == 5 || ft == 6)) { int32_t v; if (!t_i32(c, &v)) return 0; s->type = v; s->has_type = 1; }
        else if (fid == 2 && (ft == 5 || ft == 6)) { int32_t v; if (!t_i32(c, &v)) return 0; s->type_len = v; }
        else if (fid == 3 && (ft == 5 || ft == 6)) { int32_t v; if (!t_i32(c, &v)) return 0; s->rep_type = v; }
        else if (fid == 4 && ft == 8) {
            uint64_t n = 0; if (!t_uvarint(c, &n) || n > c->len - c->pos) return 0;
            s->name = malloc((size_t)n + 1u);
            memcpy(s->name, c->p + c->pos, (size_t)n); s->name[n] = 0;
            c->pos += (size_t)n;
        }
        else if (fid == 5 && (ft == 5 || ft == 6)) { int32_t v; if (!t_i32(c, &v)) return 0; s->num_children = v; }
        else if (!t_skip(c, ft, 1)) return 0;
    }
    return ft == 0;
}

static int parse_col_meta(TCur *c, PqColChunk *cc){
    memset(cc, 0, sizeof *cc);
    int16_t last = 0, fid = 0; uint8_t ft = 0;
    while (t_field(c, &last, &fid, &ft) && ft != 0) {
        if (fid == 1 && (ft == 5 || ft == 6)) { if (!t_i32(c, &cc->type)) return 0; }
        else if (fid == 3 && ft == 9) {
            uint8_t et = 0; uint32_t cnt = 0;
            if (!t_list_hdr(c, &et, &cnt)) return 0;
            lp_buf pb = { 0 };
            for (uint32_t i = 0; i < cnt; i++) {
                uint64_t n = 0;
                if (et != 8 || !t_uvarint(c, &n) || n > c->len - c->pos) { lp_buf_free(&pb); return 0; }
                if (i) lp_buf_byte(&pb, '.');
                lp_buf_put(&pb, c->p + c->pos, (size_t)n);
                c->pos += (size_t)n;
            }
            lp_buf_byte(&pb, 0);
            cc->path_col = (char *)lp_buf_take(&pb);
        }
        else if (fid == 4 && (ft == 5 || ft == 6)) { if (!t_i32(c, &cc->codec)) return 0; }
        else if (fid == 5 && (ft == 5 || ft == 6)) { if (!t_i64(c, &cc->num_values)) return 0; }
        else if (fid == 7 && (ft == 5 || ft == 6)) { if (!t_i64(c, &cc->total_compressed_size)) return 0; }
        else if (fid == 9 && (ft == 5 || ft == 6)) { if (!t_i64(c, &cc->data_page_offset)) return 0; }
        else if (fid == 11 && (ft == 5 || ft == 6)) { if (!t_i64(c, &cc->dict_page_offset)) return 0; cc->has_dict = 1; }
        else if (!t_skip(c, ft, 1)) return 0;
    }
    return ft == 0;
}

static int parse_col_chunk(TCur *c, PqColChunk *cc){
    memset(cc, 0, sizeof *cc);
    int16_t last = 0, fid = 0; uint8_t ft = 0;
    while (t_field(c, &last, &fid, &ft) && ft != 0) {
        if (fid == 3 && ft == 12) { if (!parse_col_meta(c, cc)) return 0; }
        else if (!t_skip(c, ft, 1)) return 0;
    }
    return ft == 0;
}

static int parse_row_group(TCur *c, PqRowGroup *rg){
    memset(rg, 0, sizeof *rg);
    int16_t last = 0, fid = 0; uint8_t ft = 0;
    while (t_field(c, &last, &fid, &ft) && ft != 0) {
        if (fid == 1 && ft == 9) {
            uint8_t et = 0; uint32_t cnt = 0;
            if (!t_list_hdr(c, &et, &cnt) || et != 12) return 0;
            rg->cols = calloc(cnt ? cnt : 1u, sizeof *rg->cols);
            rg->ncols = cnt;
            for (uint32_t i = 0; i < cnt; i++) if (!parse_col_chunk(c, &rg->cols[i])) return 0;
        }
        else if (fid == 3 && (ft == 5 || ft == 6)) { if (!t_i64(c, &rg->num_rows)) return 0; }
        else if (!t_skip(c, ft, 1)) return 0;
    }
    return ft == 0;
}

static int parse_page_header(TCur *c, PqPageHdr *h){
    memset(h, 0, sizeof *h);
    h->is_compressed = 1;
    int16_t last = 0, fid = 0; uint8_t ft = 0;
    while (t_field(c, &last, &fid, &ft) && ft != 0) {
        if (fid == 1 && (ft == 5 || ft == 6)) { if (!t_i32(c, &h->type)) return 0; }
        else if (fid == 2 && (ft == 5 || ft == 6)) { if (!t_i32(c, &h->uncomp_sz)) return 0; }
        else if (fid == 3 && (ft == 5 || ft == 6)) { if (!t_i32(c, &h->comp_sz)) return 0; }
        else if (fid == 5 && ft == 12) {
            int16_t l2 = 0, f2 = 0; uint8_t t2 = 0;
            while (t_field(c, &l2, &f2, &t2) && t2 != 0) {
                if (f2 == 1) { if (!t_i32(c, &h->num_values)) return 0; }
                else if (f2 == 2) { if (!t_i32(c, &h->encoding)) return 0; }
                else if (f2 == 3) { if (!t_i32(c, &h->def_enc)) return 0; }
                else if (f2 == 4) { if (!t_i32(c, &h->rep_enc)) return 0; }
                else if (!t_skip(c, t2, 2)) return 0;
            }
        } else if (fid == 7 && ft == 12) {
            int16_t l2 = 0, f2 = 0; uint8_t t2 = 0;
            while (t_field(c, &l2, &f2, &t2) && t2 != 0) {
                if (f2 == 1) { if (!t_i32(c, &h->num_values)) return 0; }
                else if (f2 == 2) { if (!t_i32(c, &h->encoding)) return 0; }
                else if (!t_skip(c, t2, 2)) return 0;
            }
        } else if (fid == 8 && ft == 12) {
            int16_t l2 = 0, f2 = 0; uint8_t t2 = 0;
            while (t_field(c, &l2, &f2, &t2) && t2 != 0) {
                if (f2 == 1) { if (!t_i32(c, &h->num_values)) return 0; }
                else if (f2 == 4) { if (!t_i32(c, &h->encoding)) return 0; }
                else if (f2 == 5) { if (!t_i32(c, &h->def_bytes_v2)) return 0; }
                else if (f2 == 6) { if (!t_i32(c, &h->rep_bytes_v2)) return 0; }
                else if (f2 == 7 && (t2 == 1 || t2 == 2)) h->is_compressed = (t2 == 1);
                else if (!t_skip(c, t2, 2)) return 0;
            }
        } else if (!t_skip(c, ft, 1)) return 0;
    }
    return ft == 0;
}

/* ---- Hybrid RLE/Bit-Packing decoder ---- */
static size_t rle_bitpack_decode(const uint8_t *src, size_t len, int bw,
                                 uint32_t *out, size_t n){
    if (bw <= 0) {
        for (size_t i = 0; i < n; i++) out[i] = 0;
        return n;
    }
    size_t pos = 0, got = 0;
    size_t vbytes = (size_t)(bw + 7) / 8u;
    uint32_t mask = bw >= 32 ? 0xFFFFFFFFu : ((1u << bw) - 1u);
    while (got < n && pos < len) {
        uint64_t hdr = 0;
        TCur tc = { src, len, pos };
        if (!t_uvarint(&tc, &hdr)) break;
        pos = tc.pos;
        if (hdr & 1u) {
            size_t cnt = (size_t)(hdr >> 1) * 8u;
            uint64_t acc = 0;
            int acc_bits = 0;
            for (size_t i = 0; i < cnt; i++) {
                while (acc_bits < bw && pos < len) {
                    acc |= (uint64_t)src[pos++] << acc_bits;
                    acc_bits += 8;
                }
                if (got < n) out[got++] = (uint32_t)(acc & mask);
                acc >>= bw;
                acc_bits = acc_bits > bw ? acc_bits - bw : 0;
            }
        } else {
            size_t cnt = (size_t)(hdr >> 1);
            if (pos + vbytes > len) break;
            uint32_t val = 0;
            for (size_t b = 0; b < vbytes; b++) val |= (uint32_t)src[pos++] << (8u * b);
            val &= mask;
            for (size_t i = 0; i < cnt && got < n; i++) out[got++] = val;
        }
    }
    while (got < n) out[got++] = 0;
    return got;
}

typedef struct {
    char **cells;
    size_t n;
} ColData;

static void col_data_free(ColData *cd){
    if (!cd) return;
    for (size_t i = 0; i < cd->n; i++) free(cd->cells[i]);
    free(cd->cells);
    cd->cells = NULL;
    cd->n = 0;
}

static char *dup_fmt(const char *s, size_t n){
    char *r = malloc(n + 1u);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static int decode_plain_values(const uint8_t *src, size_t slen, int ptype, int type_len,
                               size_t nvals, ColData *out, size_t start_idx, int is_dict){
    size_t pos = 0;
    char tmp[128];
    if (ptype == PQ_BOOL) {
        for (size_t i = 0; i < nvals; i++) {
            size_t byte_i = i >> 3, bit_i = i & 7u;
            int b = byte_i < slen ? ((src[byte_i] >> bit_i) & 1) : 0;
            out->cells[start_idx + i] = dup_fmt(b ? "true" : "false", b ? 4 : 5);
        }
        return 1;
    }
    for (size_t i = 0; i < nvals; i++) {
        char *cell = NULL;
        switch (ptype) {
        case PQ_I32: {
            if (pos + 4 > slen) goto truncated;
            int32_t v = (int32_t)rd_le32(src + pos); pos += 4;
            int k = snprintf(tmp, sizeof tmp, "%d", v);
            cell = dup_fmt(tmp, (size_t)k);
            break;
        }
        case PQ_I64: {
            if (pos + 8 > slen) goto truncated;
            int64_t v = (int64_t)rd_le64(src + pos); pos += 8;
            int k = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
            cell = dup_fmt(tmp, (size_t)k);
            break;
        }
        case PQ_F32: {
            if (pos + 4 > slen) goto truncated;
            uint32_t u = rd_le32(src + pos); pos += 4;
            float fv; memcpy(&fv, &u, 4);
            int k = snprintf(tmp, sizeof tmp, "%.9g", (double)fv);
            cell = dup_fmt(tmp, (size_t)k);
            break;
        }
        case PQ_F64: {
            if (pos + 8 > slen) goto truncated;
            uint64_t u = rd_le64(src + pos); pos += 8;
            double dv; memcpy(&dv, &u, 8);
            int k = snprintf(tmp, sizeof tmp, "%.17g", dv);
            cell = dup_fmt(tmp, (size_t)k);
            break;
        }
        case PQ_I96: {
            if (pos + 12 > slen) goto truncated;
            int64_t ns = (int64_t)rd_le64(src + pos);
            uint32_t jd = rd_le32(src + pos + 8);
            pos += 12;
            int k = snprintf(tmp, sizeof tmp, "%u:%lld", jd, (long long)ns);
            cell = dup_fmt(tmp, (size_t)k);
            break;
        }
        case PQ_BYTES: {
            if (pos + 4 > slen) goto truncated;
            uint32_t bl = rd_le32(src + pos); pos += 4;
            if (bl > slen - pos) goto truncated;
            cell = dup_fmt((const char *)(src + pos), bl);
            pos += bl;
            break;
        }
        case PQ_FIXED: {
            size_t fl = type_len > 0 ? (size_t)type_len : 0;
            if (fl > slen - pos) goto truncated;
            cell = dup_fmt((const char *)(src + pos), fl);
            pos += fl;
            break;
        }
        default:
            cell = dup_fmt("", 0);
            break;
        }
        out->cells[start_idx + i] = cell;
        continue;
    truncated:
        if (is_dict) break;
        out->cells[start_idx + i] = dup_fmt("", 0);
    }
    return 1;
}

static void emit_tsv_escaped(lp_buf *out, const char *s){
    if (!s) return;
    for (const char *p = s; *p; p++) {
        if (*p == '\\') lp_buf_cstr(out, "\\\\");
        else if (*p == '\t') lp_buf_cstr(out, "\\t");
        else if (*p == '\n') lp_buf_cstr(out, "\\n");
        else if (*p == '\r') lp_buf_cstr(out, "\\r");
        else lp_buf_byte(out, (uint8_t)*p);
    }
}

typedef struct {
    FILE        *fp;
    int64_t      file_size;
    PqLeaf      *leaves;
    size_t       nleaves;
    PqRowGroup  *groups;
    size_t       ngroups;
    size_t       cur_group;
    int          header_emitted;
    lp_buf       out;
    size_t       out_at;
    int          failed;
} ParquetDecoder;

static void walk_schema_leaves(const PqSchema *sc, size_t nsc, size_t *idx,
                               int def_lvl, int rep_lvl, int depth,
                               PqLeaf **leaves, size_t *nl, size_t *cl){
    if (*idx >= nsc) return;
    const PqSchema *s = &sc[(*idx)++];
    int ndef = def_lvl, nrep = rep_lvl;
    if (depth > 0 && s->rep_type >= 0) {
        if (s->rep_type != 0) ndef++;
        if (s->rep_type == 2) nrep++;
    }
    if (s->num_children > 0) {
        for (int32_t c = 0; c < s->num_children; c++) {
            walk_schema_leaves(sc, nsc, idx, ndef, nrep, depth + 1, leaves, nl, cl);
        }
    } else if (depth > 0) {
        if (*nl == *cl) {
            *cl = *cl ? *cl * 2u : 16u;
            *leaves = xrealloc(*leaves, sizeof **leaves * *cl);
        }
        (*leaves)[*nl] = (PqLeaf){
            .name = strdup(s->name ? s->name : "col"),
            .phys_type = s->has_type ? s->type : PQ_BYTES,
            .type_len = s->type_len,
            .max_def = ndef,
            .max_rep = nrep
        };
        (*nl)++;
    }
}

static int decode_column_chunk(FILE *fp, int64_t file_size, const PqLeaf *leaf,
                               const PqColChunk *cc, size_t expected_rows, ColData *col_out){
    col_out->cells = calloc(expected_rows ? expected_rows : 1u, sizeof(char *));
    col_out->n = 0;

    int64_t start = cc->has_dict && cc->dict_page_offset > 0 && cc->dict_page_offset < cc->data_page_offset
        ? cc->dict_page_offset : cc->data_page_offset;
    if (start <= 0 || cc->total_compressed_size <= 0 || start + cc->total_compressed_size > file_size) {
        return 0;
    }
    size_t csz = (size_t)cc->total_compressed_size;
    uint8_t *raw = malloc(csz);
    if (!raw) return 0;
    if (os_fseek64(fp, start, SEEK_SET) != 0 || fread(raw, 1, csz, fp) != csz) {
        free(raw);
        return 0;
    }

    ColData dict = { 0 };
    size_t pos = 0;
    int ok = 1;
    while (pos < csz && col_out->n < expected_rows) {
        TCur hcur = { raw, csz, pos };
        PqPageHdr ph;
        if (!parse_page_header(&hcur, &ph) || ph.comp_sz < 0 || (size_t)ph.comp_sz > csz - hcur.pos) {
            ok = 0; break;
        }
        pos = hcur.pos;
        const uint8_t *payload = raw + pos;
        size_t plen = (size_t)ph.comp_sz;
        pos += plen;

        uint8_t *uncomp = NULL;
        size_t ulen = 0;
        const uint8_t *data_ptr = NULL;
        size_t data_len = 0;
        size_t rep_v2 = (ph.type == 3 && ph.rep_bytes_v2 > 0) ? (size_t)ph.rep_bytes_v2 : 0;
        size_t def_v2 = (ph.type == 3 && ph.def_bytes_v2 > 0) ? (size_t)ph.def_bytes_v2 : 0;

        if (ph.type == 3) {
            if (rep_v2 + def_v2 > plen) { ok = 0; break; }
            size_t comp_off = rep_v2 + def_v2;
            size_t u_v2 = ph.uncomp_sz > (int32_t)comp_off ? (size_t)ph.uncomp_sz - comp_off : 0;
            int codec = ph.is_compressed ? cc->codec : PQ_CODEC_NONE;
            if (!decompress_page(codec, payload + comp_off, plen - comp_off, u_v2, &uncomp, &ulen)) {
                ok = 0; break;
            }
            data_ptr = uncomp;
            data_len = ulen;
        } else {
            if (!decompress_page(cc->codec, payload, plen, ph.uncomp_sz > 0 ? (size_t)ph.uncomp_sz : 0, &uncomp, &ulen)) {
                ok = 0; break;
            }
            data_ptr = uncomp;
            data_len = ulen;
        }

        if (ph.type == 2) { /* DICTIONARY_PAGE */
            col_data_free(&dict);
            dict.n = ph.num_values > 0 ? (size_t)ph.num_values : 0;
            dict.cells = calloc(dict.n ? dict.n : 1u, sizeof(char *));
            decode_plain_values(data_ptr, data_len, leaf->phys_type, leaf->type_len, dict.n, &dict, 0, 1);
            free(uncomp);
            continue;
        }

        /* DataPage v1 or v2 */
        size_t nv = ph.num_values > 0 ? (size_t)ph.num_values : 0;
        if (nv == 0) { free(uncomp); continue; }
        size_t dpos = 0;

        uint32_t *rep_lvls = NULL, *def_lvls = NULL;
        if (leaf->max_rep > 0) {
            rep_lvls = calloc(nv, sizeof(uint32_t));
            int rbw = 0; while ((1 << rbw) <= leaf->max_rep) rbw++;
            if (ph.type == 3) {
                rle_bitpack_decode(payload, rep_v2, rbw, rep_lvls, nv);
            } else if (dpos + 4 <= data_len) {
                uint32_t rl = rd_le32(data_ptr + dpos); dpos += 4;
                if (rl > data_len - dpos) rl = (uint32_t)(data_len - dpos);
                rle_bitpack_decode(data_ptr + dpos, rl, rbw, rep_lvls, nv);
                dpos += rl;
            }
        }
        size_t non_null = nv;
        if (leaf->max_def > 0) {
            def_lvls = calloc(nv, sizeof(uint32_t));
            int dbw = 0; while ((1 << dbw) <= leaf->max_def) dbw++;
            if (ph.type == 3) {
                rle_bitpack_decode(payload + rep_v2, def_v2, dbw, def_lvls, nv);
            } else if (dpos + 4 <= data_len) {
                uint32_t dl = rd_le32(data_ptr + dpos); dpos += 4;
                if (dl > data_len - dpos) dl = (uint32_t)(data_len - dpos);
                rle_bitpack_decode(data_ptr + dpos, dl, dbw, def_lvls, nv);
                dpos += dl;
            }
            non_null = 0;
            for (size_t i = 0; i < nv; i++) if ((int32_t)def_lvls[i] == leaf->max_def) non_null++;
        }

        ColData vals = { calloc(non_null ? non_null : 1u, sizeof(char *)), non_null };
        const uint8_t *vp = data_ptr + dpos;
        size_t vlen = data_len - dpos;

        if (ph.encoding == PQ_ENC_RLE_DICT || ph.encoding == PQ_ENC_PLAIN_DICT) {
            if (vlen >= 1) {
                int bw = vp[0];
                uint32_t *idx = calloc(non_null ? non_null : 1u, sizeof(uint32_t));
                rle_bitpack_decode(vp + 1, vlen - 1, bw, idx, non_null);
                for (size_t i = 0; i < non_null; i++) {
                    vals.cells[i] = idx[i] < dict.n && dict.cells[idx[i]]
                        ? strdup(dict.cells[idx[i]]) : dup_fmt("", 0);
                }
                free(idx);
            }
        } else if (ph.encoding == PQ_ENC_RLE && leaf->phys_type == PQ_BOOL) {
            uint32_t *bv = calloc(non_null ? non_null : 1u, sizeof(uint32_t));
            size_t rpos = 0;
            if (vlen >= 4) {
                size_t rlen = rd_le32(vp); rpos = 4;
                if (rlen > vlen - 4) rlen = vlen - 4;
                rle_bitpack_decode(vp + rpos, rlen, 1, bv, non_null);
            } else {
                rle_bitpack_decode(vp, vlen, 1, bv, non_null);
            }
            for (size_t i = 0; i < non_null; i++) vals.cells[i] = dup_fmt(bv[i] ? "true" : "false", bv[i] ? 4 : 5);
            free(bv);
        } else {
            decode_plain_values(vp, vlen, leaf->phys_type, leaf->type_len, non_null, &vals, 0, 0);
        }

        size_t vi = 0;
        for (size_t i = 0; i < nv && col_out->n < expected_rows; i++) {
            int is_null = def_lvls && (int32_t)def_lvls[i] < leaf->max_def;
            if (rep_lvls && i > 0 && rep_lvls[i] > 0) {
                /* Nested repeated elements append to previous row value */
                size_t prev = col_out->n - 1;
                if (!is_null && vi < vals.n) {
                    char *a = col_out->cells[prev], *b = vals.cells[vi];
                    size_t la = a ? strlen(a) : 0, lb = b ? strlen(b) : 0;
                    char *joined = malloc(la + lb + 2);
                    if (la) memcpy(joined, a, la);
                    joined[la] = '|';
                    if (lb) memcpy(joined + la + 1, b, lb);
                    joined[la + 1 + lb] = 0;
                    free(col_out->cells[prev]);
                    col_out->cells[prev] = joined;
                    free(vals.cells[vi]); vals.cells[vi] = NULL;
                    vi++;
                }
                continue;
            }
            if (is_null) {
                col_out->cells[col_out->n++] = NULL;
            } else {
                col_out->cells[col_out->n++] = vi < vals.n ? vals.cells[vi] : NULL;
                if (vi < vals.n) vals.cells[vi] = NULL;
                vi++;
            }
        }
        col_data_free(&vals);
        free(def_lvls);
        free(rep_lvls);
        free(uncomp);
    }
    col_data_free(&dict);
    free(raw);
    return ok;
}

static void *parquet_open(const char *path){
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }
    int64_t fsz = os_fsize(fp);
    if (fsz < 12) { fclose(fp); fprintf(stderr, "%s: not a Parquet file\n", path); return NULL; }

    uint8_t head[4], tail[8];
    if (fread(head, 1, 4, fp) != 4 || memcmp(head, "PAR1", 4) != 0
        || os_fseek64(fp, fsz - 8, SEEK_SET) != 0 || fread(tail, 1, 8, fp) != 8
        || memcmp(tail + 4, "PAR1", 4) != 0) {
        fclose(fp);
        fprintf(stderr, "%s: invalid Parquet magic bytes\n", path);
        return NULL;
    }

    uint32_t flen = rd_le32(tail);
    if ((int64_t)flen + 12 > fsz || flen > (256u << 20)) {
        fclose(fp);
        fprintf(stderr, "%s: invalid Parquet footer size\n", path);
        return NULL;
    }
    uint8_t *fbuf = malloc(flen);
    if (!fbuf || os_fseek64(fp, fsz - 8 - (int64_t)flen, SEEK_SET) != 0 || fread(fbuf, 1, flen, fp) != flen) {
        free(fbuf); fclose(fp); return NULL;
    }

    PqSchema *sc = NULL; size_t nsc = 0;
    PqRowGroup *rgs = NULL; size_t nrgs = 0;
    TCur fc = { fbuf, flen, 0 };
    int16_t last = 0, fid = 0; uint8_t ft = 0;
    int ok = 1;
    while (t_field(&fc, &last, &fid, &ft) && ft != 0) {
        if (fid == 2 && ft == 9) {
            uint8_t et = 0; uint32_t cnt = 0;
            if (!t_list_hdr(&fc, &et, &cnt) || et != 12) { ok = 0; break; }
            sc = calloc(cnt ? cnt : 1u, sizeof *sc); nsc = cnt;
            for (uint32_t i = 0; i < cnt; i++) if (!parse_schema_elem(&fc, &sc[i])) { ok = 0; break; }
            if (!ok) break;
        } else if (fid == 4 && ft == 9) {
            uint8_t et = 0; uint32_t cnt = 0;
            if (!t_list_hdr(&fc, &et, &cnt) || et != 12) { ok = 0; break; }
            rgs = calloc(cnt ? cnt : 1u, sizeof *rgs); nrgs = cnt;
            for (uint32_t i = 0; i < cnt; i++) if (!parse_row_group(&fc, &rgs[i])) { ok = 0; break; }
            if (!ok) break;
        } else if (!t_skip(&fc, ft, 0)) {
            ok = 0; break;
        }
    }
    free(fbuf);

    PqLeaf *leaves = NULL; size_t nl = 0, cl = 0, sidx = 0;
    if (ok && nsc > 0) walk_schema_leaves(sc, nsc, &sidx, 0, 0, 0, &leaves, &nl, &cl);
    for (size_t i = 0; i < nsc; i++) free(sc[i].name);
    free(sc);

    if (!ok || !nl) {
        for (size_t i = 0; i < nl; i++) free(leaves[i].name);
        free(leaves);
        for (size_t g = 0; g < nrgs; g++) {
            for (size_t c = 0; c < rgs[g].ncols; c++) free(rgs[g].cols[c].path_col);
            free(rgs[g].cols);
        }
        free(rgs);
        fclose(fp);
        fprintf(stderr, "%s: failed to parse Parquet metadata\n", path);
        return NULL;
    }

    ParquetDecoder *pd = calloc(1, sizeof *pd);
    pd->fp = fp;
    pd->file_size = fsz;
    pd->leaves = leaves;
    pd->nleaves = nl;
    pd->groups = rgs;
    pd->ngroups = nrgs;
    return pd;
}

static size_t parquet_read(void *ctx, uint8_t *out, size_t capacity){
    ParquetDecoder *pd = ctx;
    if (pd->failed || !capacity) return 0;

    if (!pd->header_emitted) {
        for (size_t c = 0; c < pd->nleaves; c++) {
            if (c) lp_buf_byte(&pd->out, '\t');
            emit_tsv_escaped(&pd->out, pd->leaves[c].name);
        }
        lp_buf_byte(&pd->out, '\n');
        pd->header_emitted = 1;
    }

    while (pd->out_at >= pd->out.n && pd->cur_group < pd->ngroups) {
        lp_buf_clear(&pd->out);
        pd->out_at = 0;
        const PqRowGroup *rg = &pd->groups[pd->cur_group++];
        size_t nrows = rg->num_rows > 0 ? (size_t)rg->num_rows : 0;
        if (!nrows) continue;

        ColData *cols = calloc(pd->nleaves, sizeof(ColData));
        for (size_t c = 0; c < pd->nleaves; c++) {
            const PqColChunk *cc = NULL;
            if (c < rg->ncols && rg->cols[c].path_col && !strcmp(rg->cols[c].path_col, pd->leaves[c].name)) {
                cc = &rg->cols[c];
            } else {
                for (size_t k = 0; k < rg->ncols; k++) {
                    if (rg->cols[k].path_col && !strcmp(rg->cols[k].path_col, pd->leaves[c].name)) {
                        cc = &rg->cols[k]; break;
                    }
                }
                if (!cc && c < rg->ncols) cc = &rg->cols[c];
            }
            if (!cc || !decode_column_chunk(pd->fp, pd->file_size, &pd->leaves[c], cc, nrows, &cols[c])) {
                pd->failed = 1;
                break;
            }
        }

        if (!pd->failed) {
            for (size_t r = 0; r < nrows; r++) {
                for (size_t c = 0; c < pd->nleaves; c++) {
                    if (c) lp_buf_byte(&pd->out, '\t');
                    if (r < cols[c].n && cols[c].cells[r]) emit_tsv_escaped(&pd->out, cols[c].cells[r]);
                }
                lp_buf_byte(&pd->out, '\n');
            }
        }
        for (size_t c = 0; c < pd->nleaves; c++) col_data_free(&cols[c]);
        free(cols);
        if (pd->failed) return 0;
    }

    size_t avail = pd->out.n - pd->out_at;
    size_t take = avail < capacity ? avail : capacity;
    if (take) {
        memcpy(out, pd->out.b + pd->out_at, take);
        pd->out_at += take;
    }
    return take;
}

static int parquet_error(void *ctx){
    ParquetDecoder *pd = ctx;
    return pd ? pd->failed : 1;
}

static void parquet_close(void *ctx){
    ParquetDecoder *pd = ctx;
    if (!pd) return;
    if (pd->fp) fclose(pd->fp);
    for (size_t i = 0; i < pd->nleaves; i++) free(pd->leaves[i].name);
    free(pd->leaves);
    for (size_t g = 0; g < pd->ngroups; g++) {
        for (size_t c = 0; c < pd->groups[g].ncols; c++) free(pd->groups[g].cols[c].path_col);
        free(pd->groups[g].cols);
    }
    free(pd->groups);
    lp_buf_free(&pd->out);
    free(pd);
}

const InputDecoder parquet_decoder = {
    "parquet", parquet_open, parquet_read, parquet_error, parquet_close
};
