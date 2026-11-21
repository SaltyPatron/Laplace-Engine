#include "codecs.h"
#include "laplace_config.h"
#include "laplace/os.h"
#include "laplace/tensor.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const char *env_or(const char *name, const char *dflt){ const char *v = getenv(name); return v && *v ? v : dflt; }
const char *laplace_recipes(void){ return env_or("LAPLACE_RECIPES", LAPLACE_RECIPES_DEFAULT); }
const char *laplace_grammars(void){ return env_or("LAPLACE_GRAMMARS", LAPLACE_GRAMMARS_DEFAULT); }
const char *laplace_ucd(void){ return env_or("LAPLACE_UCD", LAPLACE_UCD_DEFAULT); }

extern unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len,
                                            int *x, int *y, int *channels_in_file,
                                            int desired_channels);
extern void stbi_image_free(void *retval_from_stbi_load);

/* Defined in src/ingest/ingest.c for CLI error prefix; set harmlessly when testing codecs directly */
static void make_png(int w, int h, const uint8_t *rgba, const char *comment,
                     uint8_t **out_buf, size_t *out_len){
    size_t raw_len = (size_t)h * (1u + (size_t)w * 4u);
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (1u + (size_t)w * 4u)] = 0;
        memcpy(raw + (size_t)y * (1u + (size_t)w * 4u) + 1u,
               rgba + (size_t)y * (size_t)w * 4u, (size_t)w * 4u);
    }
    uLongf zcap = compressBound((uLong)raw_len);
    uint8_t *zbuf = malloc(zcap);
    assert(compress2(zbuf, &zcap, raw, (uLong)raw_len, Z_DEFAULT_COMPRESSION) == Z_OK);
    free(raw);

    lp_buf b = { 0 };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    lp_buf_put(&b, sig, 8);

    uint8_t ihdr[13] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, 6, 0, 0, 0
    };
    uint8_t len4[4];
    #define CHUNK(T, D, L) do { \
        len4[0] = (uint8_t)((L) >> 24); len4[1] = (uint8_t)((L) >> 16); \
        len4[2] = (uint8_t)((L) >> 8);  len4[3] = (uint8_t)(L); \
        lp_buf_put(&b, len4, 4); lp_buf_put(&b, T, 4); \
        if ((L) && (D)) lp_buf_put(&b, D, L); \
        uLong c = crc32(0L, Z_NULL, 0); c = crc32(c, (const Bytef *)(T), 4); \
        if ((L) && (D)) c = crc32(c, (const Bytef *)(D), (uInt)(L)); \
        len4[0] = (uint8_t)(c >> 24); len4[1] = (uint8_t)(c >> 16); \
        len4[2] = (uint8_t)(c >> 8);  len4[3] = (uint8_t)c; \
        lp_buf_put(&b, len4, 4); \
    } while (0)

    CHUNK("IHDR", ihdr, 13u);
    if (comment) {
        size_t cl = strlen(comment);
        uint8_t *tx = malloc(8u + cl);
        memcpy(tx, "Comment", 8);
        memcpy(tx + 8, comment, cl);
        CHUNK("tEXt", tx, 8u + (uint32_t)cl);
        free(tx);
    }
    CHUNK("IDAT", zbuf, (uint32_t)zcap);
    CHUNK("IEND", NULL, 0u);
    #undef CHUNK

    free(zbuf);
    *out_len = b.n;
    *out_buf = (uint8_t *)lp_buf_take(&b);
}

static int find_vrun_gt_1(const lp_id *id, int depth){
    if (depth > 8) return 0;
    Node *n = table_find(id);
    if (!n) return 0;
    for (uint32_t v = 0; v < n->nv; v++) {
        if (VRUN(VTX[n->voff + v].m) > 1u) return 1;
        if (find_vrun_gt_1(&VTX[n->voff + v].id, depth + 1)) return 1;
    }
    return 0;
}

static void test_image_codec(void){
    const int W = 16, H = 8;
    uint8_t *rgba = malloc((size_t)W * (size_t)H * 4u);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *p = rgba + ((size_t)y * W + x) * 4u;
            if (y == 0) {
                /* Solid white run collapses via VRUN > 1 at both pixel level (255x4) and row level */
                p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
            } else {
                p[0] = (uint8_t)(x * 16); p[1] = (uint8_t)(y * 32);
                p[2] = (uint8_t)((x ^ y) * 11); p[3] = 255;
            }
        }
    }
    uint8_t *png = NULL; size_t png_len = 0;
    make_png(W, H, rgba, "laplace-test", &png, &png_len);

    /* Parity check: decimal scalar 255 == UAX #29 "255" ID (zero ByteAtoms) */
    lp_scalars *sc = codecs_scalars();
    Ref u8_255 = lp_scalar_u8(sc, 255u);
    Ref tx_255 = text_ref(CTX[0], (const uint8_t *)"255", 3);
    assert(lp_id_eq(&u8_255.id, &tx_255.id));

    File f = { .path = "synthetic.png", .trust = 0.8 };
    image_grammar.read(NULL, &f, png, png_len);
    assert(!f.invalid && f.records == 1 && f.ev.n == 2 && f.nsaid == 1 && f.said[0].said == LP_SAID_RECORD);
    assert(find_vrun_gt_1(&f.trunk.id, 0));

    uint8_t *out_png = NULL; size_t out_len = 0;
    assert(lp_image_realize_png(&f.trunk.id, T0, &out_png, &out_len));
    int rw = 0, rh = 0, rcomp = 0;
    uint8_t *dec = stbi_load_from_memory(out_png, (int)out_len, &rw, &rh, &rcomp, 4);
    assert(dec && rw == W && rh == H);
    assert(memcmp(dec, rgba, (size_t)W * H * 4u) == 0);

    stbi_image_free(dec);
    free(out_png); free(png); free(rgba); free(f.ev.e); free(f.said);
    printf("[PASS] image_grammar PNG roundtrip + scalar '255' parity + VRUN>1\n");
}

static void put_le16(uint8_t *p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void test_audio_and_video(void){
    const uint32_t nf = 1500, sr = 8000, ch = 1;
    size_t wlen = 44u + (size_t)nf * 2u;
    uint8_t *wav = malloc(wlen);
    memcpy(wav, "RIFF", 4); put_le32(wav + 4, (uint32_t)(wlen - 8u));
    memcpy(wav + 8, "WAVEfmt ", 8); put_le32(wav + 16, 16u);
    put_le16(wav + 20, 1u); put_le16(wav + 22, (uint16_t)ch);
    put_le32(wav + 24, sr); put_le32(wav + 28, sr * 2u);
    put_le16(wav + 32, 2u); put_le16(wav + 34, 16u);
    memcpy(wav + 36, "data", 4); put_le32(wav + 40, nf * 2u);
    int16_t *pcm = (int16_t *)(wav + 44u);
    for (uint32_t i = 0; i < nf; i++) pcm[i] = i < 400 ? 0 : (int16_t)((int)(i % 64) * 512 - 16384);

    File fa = { .path = "synthetic.wav", .trust = 0.8 };
    audio_grammar.read(NULL, &fa, wav, wlen);
    assert(!fa.invalid && fa.records == 1 && fa.ev.n == 1 && fa.nsaid == 1 && fa.said[0].said == LP_SAID_RECORD);
    assert(find_vrun_gt_1(&fa.trunk.id, 0));

    uint8_t *rw = NULL; size_t rwl = 0;
    assert(lp_audio_realize_wav(&fa.trunk.id, T0, &rw, &rwl));
    assert(rwl == wlen && memcmp(rw, wav, wlen) == 0);
    free(rw); free(wav); free(fa.ev.e); free(fa.said);

    /* Minimal valid IVF video container (DKIF + 2 frames) */
    uint8_t ivf[32 + 2 * (12 + 4)];
    memset(ivf, 0, sizeof ivf);
    memcpy(ivf, "DKIF", 4); put_le16(ivf + 6, 32u); memcpy(ivf + 8, "VP80", 4);
    put_le16(ivf + 12, 64u); put_le16(ivf + 14, 48u);
    put_le32(ivf + 16, 30u); put_le32(ivf + 20, 1u); put_le32(ivf + 24, 2u);
    put_le32(ivf + 32, 4u); put_le32(ivf + 48, 4u);
    File fv = { .path = "synthetic.ivf", .trust = 0.8 };
    video_grammar.read(NULL, &fv, ivf, sizeof ivf);
    assert(!fv.invalid && fv.records == 1 && fv.ev.n == 1 && fv.nsaid == 1 && fv.said[0].said == LP_SAID_RECORD);
    free(fv.ev.e); free(fv.said);
    printf("[PASS] audio_grammar WAV roundtrip + VRUN>1 + video_grammar IVF\n");
}

/* Emit Compact Thrift helpers for portable Parquet fixture */
static void c_uvarint(lp_buf *b, uint64_t v){
    while (v >= 0x80u) { lp_buf_byte(b, (uint8_t)((v & 0x7Fu) | 0x80u)); v >>= 7; }
    lp_buf_byte(b, (uint8_t)v);
}
static void c_i32(lp_buf *b, int32_t v){
    c_uvarint(b, ((uint32_t)v << 1) ^ (uint32_t)(v >> 31));
}
static void c_i64(lp_buf *b, int64_t v){
    c_uvarint(b, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63));
}
static void c_field(lp_buf *b, int16_t *last, int16_t id, uint8_t ft){
    int16_t d = id - *last;
    if (d > 0 && d <= 15) lp_buf_byte(b, (uint8_t)(((uint8_t)d << 4) | ft));
    else { lp_buf_byte(b, ft); c_i32(b, id); }
    *last = id;
}

static void snappy_store_raw(lp_buf *out, const uint8_t *src, size_t n){
    c_uvarint(out, n);
    if (!n) return;
    if (n <= 60u) {
        lp_buf_byte(out, (uint8_t)((n - 1u) << 2));
    } else {
        lp_buf_byte(out, (uint8_t)(60u << 2));
        lp_buf_byte(out, (uint8_t)((n - 1u) & 0xFFu));
        lp_buf_byte(out, (uint8_t)(((n - 1u) >> 8) & 0xFFu));
    }
    lp_buf_put(out, src, n);
}

static size_t read_decoder_sample(const InputDecoder *dec, void *h, char *buf, size_t cap){
    size_t tot = 0;
    while (tot + 1 < cap) {
        size_t got = dec->read(h, (uint8_t *)buf + tot, cap - 1 - tot);
        if (!got) break;
        tot += got;
    }
    buf[tot] = 0;
    return tot;
}

static void test_parquet_and_shapefile(void){
    /* 1. Portable SNAPPY-compressed Parquet fixture (2 columns: id INT32, txt BYTE_ARRAY; 2 rows) */
    const char *pq_path = "laplace_codec_test.parquet";
    lp_buf pg0_raw = { 0 }, pg1_raw = { 0 };
    int32_t v0 = 42, v1 = 99;
    uint8_t le4[4];
    put_le32(le4, (uint32_t)v0); lp_buf_put(&pg0_raw, le4, 4);
    put_le32(le4, (uint32_t)v1); lp_buf_put(&pg0_raw, le4, 4);

    const char *s0 = "alpha", *s1 = "be\tta";
    put_le32(le4, (uint32_t)strlen(s0)); lp_buf_put(&pg1_raw, le4, 4); lp_buf_cstr(&pg1_raw, s0);
    put_le32(le4, (uint32_t)strlen(s1)); lp_buf_put(&pg1_raw, le4, 4); lp_buf_cstr(&pg1_raw, s1);

    lp_buf pg0_sn = { 0 }, pg1_sn = { 0 };
    snappy_store_raw(&pg0_sn, pg0_raw.b, pg0_raw.n);
    snappy_store_raw(&pg1_sn, pg1_raw.b, pg1_raw.n);

    lp_buf pq = { 0 };
    lp_buf_put(&pq, "PAR1", 4);

    size_t col0_off = pq.n;
    lp_buf h0 = { 0 }; int16_t l = 0;
    c_field(&h0, &l, 1, 5); c_i32(&h0, 0);
    c_field(&h0, &l, 2, 5); c_i32(&h0, (int32_t)pg0_raw.n);
    c_field(&h0, &l, 3, 5); c_i32(&h0, (int32_t)pg0_sn.n);
    c_field(&h0, &l, 5, 12);
    { int16_t l2 = 0; c_field(&h0, &l2, 1, 5); c_i32(&h0, 2); c_field(&h0, &l2, 2, 5); c_i32(&h0, 0); lp_buf_byte(&h0, 0); }
    lp_buf_byte(&h0, 0);
    lp_buf_put(&pq, h0.b, h0.n); lp_buf_put(&pq, pg0_sn.b, pg0_sn.n);
    size_t col0_sz = pq.n - col0_off;

    size_t col1_off = pq.n;
    lp_buf h1 = { 0 }; l = 0;
    c_field(&h1, &l, 1, 5); c_i32(&h1, 0);
    c_field(&h1, &l, 2, 5); c_i32(&h1, (int32_t)pg1_raw.n);
    c_field(&h1, &l, 3, 5); c_i32(&h1, (int32_t)pg1_sn.n);
    c_field(&h1, &l, 5, 12);
    { int16_t l2 = 0; c_field(&h1, &l2, 1, 5); c_i32(&h1, 2); c_field(&h1, &l2, 2, 5); c_i32(&h1, 0); lp_buf_byte(&h1, 0); }
    lp_buf_byte(&h1, 0);
    lp_buf_put(&pq, h1.b, h1.n); lp_buf_put(&pq, pg1_sn.b, pg1_sn.n);
    size_t col1_sz = pq.n - col1_off;

    lp_buf ftr = { 0 }; l = 0;
    c_field(&ftr, &l, 1, 5); c_i32(&ftr, 1);
    c_field(&ftr, &l, 2, 9); lp_buf_byte(&ftr, (3u << 4) | 12u); /* 3 SchemaElements */
    { int16_t ls = 0; c_field(&ftr, &ls, 4, 8); c_uvarint(&ftr, 6); lp_buf_cstr(&ftr, "schema"); c_field(&ftr, &ls, 5, 5); c_i32(&ftr, 2); lp_buf_byte(&ftr, 0); }
    { int16_t ls = 0; c_field(&ftr, &ls, 1, 5); c_i32(&ftr, 1); c_field(&ftr, &ls, 3, 5); c_i32(&ftr, 0); c_field(&ftr, &ls, 4, 8); c_uvarint(&ftr, 2); lp_buf_cstr(&ftr, "id"); lp_buf_byte(&ftr, 0); }
    { int16_t ls = 0; c_field(&ftr, &ls, 1, 5); c_i32(&ftr, 6); c_field(&ftr, &ls, 3, 5); c_i32(&ftr, 0); c_field(&ftr, &ls, 4, 8); c_uvarint(&ftr, 3); lp_buf_cstr(&ftr, "txt"); lp_buf_byte(&ftr, 0); }
    c_field(&ftr, &l, 3, 6); c_i64(&ftr, 2);
    c_field(&ftr, &l, 4, 9); lp_buf_byte(&ftr, (1u << 4) | 12u); /* 1 RowGroup */
    {
        int16_t lrg = 0;
        c_field(&ftr, &lrg, 1, 9); lp_buf_byte(&ftr, (2u << 4) | 12u);
        for (int ci = 0; ci < 2; ci++) {
            int16_t lcc = 0; c_field(&ftr, &lcc, 2, 6); c_i64(&ftr, ci ? (int64_t)col1_off : (int64_t)col0_off);
            c_field(&ftr, &lcc, 3, 12);
            {
                int16_t lm = 0;
                c_field(&ftr, &lm, 1, 5); c_i32(&ftr, ci ? 6 : 1);
                c_field(&ftr, &lm, 3, 9); lp_buf_byte(&ftr, (1u << 4) | 8u);
                const char *cn = ci ? "txt" : "id"; c_uvarint(&ftr, strlen(cn)); lp_buf_cstr(&ftr, cn);
                c_field(&ftr, &lm, 4, 5); c_i32(&ftr, 1); /* SNAPPY */
                c_field(&ftr, &lm, 5, 6); c_i64(&ftr, 2);
                c_field(&ftr, &lm, 7, 6); c_i64(&ftr, ci ? (int64_t)col1_sz : (int64_t)col0_sz);
                c_field(&ftr, &lm, 9, 6); c_i64(&ftr, ci ? (int64_t)col1_off : (int64_t)col0_off);
                lp_buf_byte(&ftr, 0);
            }
            lp_buf_byte(&ftr, 0);
        }
        c_field(&ftr, &lrg, 3, 6); c_i64(&ftr, 2);
        lp_buf_byte(&ftr, 0);
    }
    lp_buf_byte(&ftr, 0);

    lp_buf_put(&pq, ftr.b, ftr.n);
    put_le32(le4, (uint32_t)ftr.n);
    lp_buf_put(&pq, le4, 4);
    lp_buf_put(&pq, "PAR1", 4);

    FILE *fp = fopen(pq_path, "wb"); assert(fp);
    assert(fwrite(pq.b, 1, pq.n, fp) == pq.n); fclose(fp);

    void *pd = parquet_decoder.open(pq_path);
    assert(pd != NULL);
    char tsv[1024];
    read_decoder_sample(&parquet_decoder, pd, tsv, sizeof tsv);
    assert(!parquet_decoder.error(pd));
    assert(!strcmp(tsv, "id\ttxt\n42\talpha\n99\tbe\\tta\n"));
    parquet_decoder.close(pd);
    remove(pq_path);

    lp_buf_free(&pg0_raw); lp_buf_free(&pg1_raw); lp_buf_free(&pg0_sn); lp_buf_free(&pg1_sn);
    lp_buf_free(&h0); lp_buf_free(&h1); lp_buf_free(&ftr); lp_buf_free(&pq);

    /* 2. Real corpus MeasuringHateSpeech Parquet verification when present locally */
    const char *mhs = "E:/Data/Ingest/Safety/MeasuringHateSpeech/measuring-hate-speech-5468f6e118396646b02a2f691e771f6b6d9502ea/measuring-hate-speech.parquet";
    if (os_exists(mhs)) {
        void *rp = parquet_decoder.open(mhs);
        assert(rp != NULL);
        char *rb = malloc(65536);
        size_t rn = read_decoder_sample(&parquet_decoder, rp, rb, 65536);
        assert(!parquet_decoder.error(rp) && rn > 2000);
        assert(strstr(rb, "comment_id") && strstr(rb, "hate_speech_score"));
        char *nl = strchr(rb, '\n');
        assert(nl && nl[1] != 0);
        free(rb);
        parquet_decoder.close(rp);
        printf("[PASS] parquet_decoder portable SNAPPY fixture + real MeasuringHateSpeech Parquet (%zu sample bytes)\n", rn);
    } else {
        printf("[PASS] parquet_decoder portable SNAPPY fixture\n");
    }

    /* 3. Synthetic Shapefile (.shp + .dbf) verification */
    const char *shp_path = "laplace_codec_test.shp", *dbf_path = "laplace_codec_test.dbf";
    uint8_t shp[100 + 8 + 20];
    memset(shp, 0, sizeof shp);
    shp[2] = 0x27; shp[3] = 0x0A; /* BE 9994 */
    shp[27] = sizeof shp / 2;     /* BE words */
    put_le32(shp + 28, 1000u);
    put_le32(shp + 32, 1u);       /* Point */
    /* Record 1 */
    shp[103] = 1u; shp[107] = 10u; /* rec 1, 10 words */
    put_le32(shp + 108, 1u);
    double x = -97.7431, y = 30.2672;
    memcpy(shp + 112, &x, 8); memcpy(shp + 120, &y, 8);
    fp = fopen(shp_path, "wb"); assert(fp); assert(fwrite(shp, 1, sizeof shp, fp) == sizeof shp); fclose(fp);

    uint8_t dbf[32 + 32 + 1 + 11];
    memset(dbf, 0, sizeof dbf);
    dbf[0] = 3; put_le32(dbf + 4, 1u); put_le16(dbf + 8, 65u); put_le16(dbf + 10, 11u);
    memcpy(dbf + 32, "CITY", 4); dbf[32 + 11] = 'C'; dbf[32 + 16] = 10;
    dbf[64] = 0x0D;
    dbf[65] = ' '; memcpy(dbf + 66, "Austin    ", 10);
    fp = fopen(dbf_path, "wb"); assert(fp); assert(fwrite(dbf, 1, sizeof dbf, fp) == sizeof dbf); fclose(fp);

    void *sd = shapefile_decoder.open(shp_path);
    assert(sd != NULL);
    char sout[512];
    read_decoder_sample(&shapefile_decoder, sd, sout, sizeof sout);
    assert(!shapefile_decoder.error(sd));
    assert(strstr(sout, "shape_type\twkt\tCITY\nPoint\tPOINT (-97.7431 30.2672)\tAustin\n"));
    shapefile_decoder.close(sd);
    remove(shp_path); remove(dbf_path);
    printf("[PASS] shapefile_decoder .shp + .dbf -> WKT TSV\n");
}

static void test_git_and_toolchain(void){
    /* Loose zlib-compressed git blob passed to git_grammar.read */
    const char payload[] = "blob 5\0hello";
    uLongf zlen = compressBound(sizeof payload - 1);
    uint8_t *zbuf = malloc(zlen);
    assert(compress2(zbuf, &zlen, (const uint8_t *)payload, sizeof payload - 1, Z_DEFAULT_COMPRESSION) == Z_OK);
    File fg = { .path = "loose_obj", .trust = 0.8 };
    git_grammar.read(NULL, &fg, zbuf, zlen);
    Ref hello = text_ref(CTX[0], (const uint8_t *)"hello", 5);
    assert(lp_id_eq(&fg.trunk.id, &hello.id));
    free(zbuf); free(fg.ev.e);

    /* ToolchainVerification witness (0.90) offline emission check */
    const lp_trust_class *tc = lp_trust_class_named("ToolchainVerification");
    assert(tc && fabs(tc->prior - 0.90) < 1e-9);
    Ref subj = text_ref(CTX[0], (const uint8_t *)"fn_foo", 6);
    Ref dep = text_ref(CTX[0], (const uint8_t *)"datatype_X", 10);
    lp_id fail_id = { { 0 } }, pass_id = { { 0 } };
    LpToolchainReport r_fail = {
        .conninfo = NULL, .witness_name = "icx", .target_label = "compile",
        .subject = &subj, .constituents = &dep, .nconstituents = 1, .passed = 0, .position = 1
    };
    LpToolchainReport r_pass = r_fail;
    r_pass.passed = 1;
    assert(lp_toolchain_witness(&r_fail, &fail_id, NULL) == 0);
    assert(lp_toolchain_witness(&r_pass, &pass_id, NULL) == 0);
    assert(!lp_id_eq(&fail_id, &pass_id));
    printf("[PASS] git_grammar + lp_toolchain_witness (ToolchainVerification = 0.90)\n");
}

static void test_mold_gguf_export(void){
    const size_t V = 16;
    static const char *WORDS[16] = {
        "<s>", "</s>", "alpha", "beta", "gamma", "delta", "epsilon", "zeta",
        "eta", "theta", "iota", "kappa", "lambda", "mu", "nu", "xi"
    };
    static const int ROWS[12] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    static const int COLS[12] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 2 };
    static const double W[12] = { 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3 };
    static const char *EW[12] = { "W0","W0","W0","W1","W1","W1","W2","W2","W2","W1","W1","W0" };
    static const char *EL[12] = { "L_A","L_A","L_A","L_B","L_B","L_B","L_B","L_B","L_B","L_A","L_A","L_A" };

    const char *gguf_path = "laplace_mold_test.gguf";
    LpMoldSpec sp = {
        .name = "mold-test",
        .out_path = gguf_path,
        .n_vocab = V,
        .tokens = WORDS,
        .bos_id = 0,
        .eos_id = 1,
        .n_edges = 12,
        .rows = ROWS,
        .cols = COLS,
        .weights = W,
        .edge_witnesses = EW,
        .edge_lineages = EL,
        .hidden_dim = 16,
        .n_layers = 2,
        .n_heads = 4,
        .n_kv_heads = 2,
        .ffn_dim = 32,
        .rms_eps = 1e-5,
        .rope_base = 10000.0,
        .f16_weights = 0
    };
    assert(lp_mold_export(&sp) == 0);

    size_t gsz = 0;
    const uint8_t *gb = lp_map_file(gguf_path, 0, &gsz);
    assert(gb && gsz > 256);
    lp_gguf *g = lp_gguf_open(gb, gsz);
    assert(g != NULL);

    int64_t ti = lp_gguf_find_kv(g, "tokenizer.ggml.model");
    size_t tl = 0; const char *tm = lp_gguf_kv_str(g, (size_t)ti, &tl);
    assert(ti >= 0 && tm && tl == 17 && !memcmp(tm, "laplace-blake3-v1", 17));

    int64_t bi = lp_gguf_find_kv(g, "laplace.vocab.blake3_ids");
    assert(bi >= 0 && lp_gguf_kv_array_type(g, (size_t)bi) == LP_GGUF_TYPE_UINT8);
    assert(lp_gguf_kv_array_len(g, (size_t)bi) == 16u * V);

    int64_t ri = lp_gguf_find_kv(g, "laplace.mold.embedding_rank");
    uint64_t rank = 0; assert(ri >= 0 && lp_gguf_kv_u64(g, (size_t)ri, &rank) && rank > 4u);

    int64_t fi = lp_gguf_find_kv(g, "laplace.mold.fisher_total_bp");
    uint64_t fbp = 0; assert(fi >= 0 && lp_gguf_kv_u64(g, (size_t)fi, &fbp) && fbp == 10000u);

    int64_t sbi = lp_gguf_find_kv(g, "laplace.mold.share_bp");
    assert(sbi >= 0);
    size_t nsh = lp_gguf_kv_array_len(g, (size_t)sbi);
    int64_t sum_bp = 0;
    for (size_t k = 0; k < nsh; k++) {
        int32_t v = 0;
        assert(lp_gguf_kv_array_i32(g, (size_t)sbi, k, &v) && v >= 0);
        sum_bp += v;
    }
    assert(sum_bp == 10000);

    int64_t rwbi = lp_gguf_find_kv(g, "laplace.mold.raw_weight_bytes_retained");
    uint64_t rwb = 999; assert(rwbi >= 0 && lp_gguf_kv_u64(g, (size_t)rwbi, &rwb) && rwb == 0u);

    int64_t ei = lp_gguf_find_tensor(g, "token_embd.weight");
    assert(ei >= 0 && lp_gguf_tensor_dtype(g, (size_t)ei) == LP_DTYPE_F32);
    uint64_t edims[LP_TENSOR_MAX_DIMS];
    assert(lp_gguf_tensor_dims(g, (size_t)ei, edims, LP_TENSOR_MAX_DIMS) == 2 && edims[0] == 16 && edims[1] == V);

    float *ebuf = malloc(sizeof(float) * V * 16u);
    const uint8_t *ep = gb + lp_gguf_data_offset(g) + lp_gguf_tensor_offset(g, (size_t)ei);
    assert(lp_tensor_decode_f32(ep, V * 16u, LP_DTYPE_F32, ebuf) == 0);
    free(ebuf);

    lp_gguf_close(g);
    lp_unmap_file(gb, gsz);

    char *m_argv[] = { "model", (char *)gguf_path, "--cap", "8", "--z", "1.5", NULL };
    assert(cmd_model(6, m_argv) == 0);
    assert(remove(gguf_path) == 0);
    printf("[PASS] Mold-a-Model GGUF export (laplace-blake3-v1, rank %llu > 4, Fisher %zu lineages SUM == 10000) + cmd_model assimilation\n",
           (unsigned long long)rank, nsh);
}

int main(void){
    tier0_open(NULL);
    table_init();
    ctx_open(1);
    test_image_codec();
    test_audio_and_video();
    test_parquet_and_shapefile();
    test_git_and_toolchain();
    test_mold_gguf_export();
    return 0;
}
