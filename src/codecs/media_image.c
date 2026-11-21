#include "codecs.h"
#include "laplace/os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Forward declarations matching vendored stb_image.h (compiled in media_impl.c) */
extern unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len,
                                            int *x, int *y, int *channels_in_file,
                                            int desired_channels);
extern void stbi_image_free(void *retval_from_stbi_load);

#define IMAGE_MAGIC "lp-image-v1"
#define AUDIO_BLOCK 1024u

static lp_ref codecs_compose_sink(void *sink, const lp_ref *ch, uint32_t n, uint8_t tier){
    (void)sink;
    return compose(ch, n, tier);
}

/* Note: returned pointer is valid within the active node-table epoch; callers that reset
 * the node table between batches should allocate a fresh lp_scalars table via codecs_scalars(). */
static pthread_mutex_t sc_mu = PTHREAD_MUTEX_INITIALIZER;
static lp_scalars *sc_cached;
static const lp_tier0_record *sc_t0;
lp_scalars *codecs_scalars(void){
    pthread_mutex_lock(&sc_mu);
    if (!sc_cached || sc_t0 != T0) {
        if (sc_cached) lp_scalars_free(sc_cached);
        sc_t0 = T0;
        sc_cached = lp_scalars_of(T0, codecs_compose_sink, NULL);
    }
    lp_scalars *s = sc_cached;
    pthread_mutex_unlock(&sc_mu);
    return s;
}

typedef struct {
    lp_buf              out;
    lp_realize_child  **allocs;
    size_t              nalloc, calloc_;
} CodecRealizeCtx;

static int codecs_table_reader(void *context, const lp_id *id, lp_realize_node *out){
    CodecRealizeCtx *c = context;
    Node *x = table_find(id);
    if (!x) return LP_REALIZE_ABSENT;
    lp_realize_child *ch = malloc(sizeof *ch * (x->nv ? x->nv : 1));
    if (!ch) return LP_REALIZE_ABSENT;
    for (uint32_t v = 0; v < x->nv; v++) {
        const Vtx *vt = &VTX[x->voff + v];
        ch[v].id = vt->id;
        ch[v].run = VRUN(vt->m);
        ch[v].said = VSAID(vt->m);
    }
    if (c->nalloc == c->calloc_) {
        c->calloc_ = c->calloc_ ? c->calloc_ * 2 : 64;
        c->allocs = xrealloc(c->allocs, sizeof *c->allocs * c->calloc_);
    }
    c->allocs[c->nalloc++] = ch;
    out->child = ch;
    out->n = x->nv;
    out->flags = 0;
    return LP_REALIZE_KNOWN;
}

static int codecs_realize_sink(void *context, const uint8_t *bytes, size_t n){
    CodecRealizeCtx *c = context;
    lp_buf_put(&c->out, bytes, n);
    return 0;
}

int lp_codec_realize_text(const lp_id *id, const lp_tier0_record *t0, char **out, size_t *len){
    *out = NULL;
    if (len) *len = 0;
    CodecRealizeCtx ctx = { 0 };
    lp_realizer *rl = lp_realizer_new();
    lp_realize_result res = { 0 };
    bool ok = lp_realize(rl, t0 ? t0 : T0, id, LP_REALIZE_TEXT, codecs_table_reader, codecs_realize_sink, &ctx, &res);
    for (size_t i = 0; i < ctx.nalloc; i++) free(ctx.allocs[i]);
    free(ctx.allocs);
    lp_realizer_free(rl);
    if (!ok || res.pending) {
        lp_buf_free(&ctx.out);
        return 0;
    }
    size_t n = ctx.out.n;
    lp_buf_byte(&ctx.out, 0);
    *out = (char *)lp_buf_take(&ctx.out);
    if (len) *len = n;
    return 1;
}

void lp_codec_emit_claim_record(File *f, const Ref *subject, const Ref *claims, size_t nclaims){
    if (!f || !subject || !claims || !nclaims) return;
    Ref *rparts = malloc((1u + nclaims) * sizeof *rparts);
    Ref subj = *subject;
    subj.said = subj.outcome = subj.position = subj.spare = 0;
    rparts[0] = subj;
    for (size_t i = 0; i < nclaims; i++) {
        Ref c = said_claim(claims[i]);
        c.outcome = (uint8_t)lp_outcome_of(1.0, &c.spare);
        c.position = (uint32_t)(i + 1u);
        Event ev = {
            .claim = c.id, .witnessed = c.id,
            .score = 1.0f, .enter_rating = 1500.0f,
            .position = c.position, .kind = EV_CLAIM, .run = 1
        };
        ev_push(&f->ev, &ev);
        rparts[1u + i] = c;
    }
    Ref R = compose(rparts, (uint32_t)(1u + nclaims), ref_above(rparts, 1u + nclaims));
    R.said = LP_SAID_RECORD;
    R.outcome = R.position = R.spare = 0;
    free(rparts);
    if (f->nsaid == f->csaid) {
        f->csaid = f->csaid ? f->csaid * 2u : 4u;
        f->said = xrealloc(f->said, f->csaid * sizeof *f->said);
    }
    f->said[f->nsaid++] = R;
}

static int decode_u64_node(const lp_id *id, const lp_tier0_record *t0, uint64_t *val){
    char *s = NULL; size_t len = 0;
    if (!lp_codec_realize_text(id, t0, &s, &len) || !len || len > 20) { free(s); return 0; }
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') { free(s); return 0; }
    }
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    int ok = (end == s + len);
    free(s);
    if (!ok) return 0;
    *val = (uint64_t)v;
    return 1;
}

static uint32_t be32(const uint8_t *p){
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(lp_buf *b, uint32_t v){
    uint8_t u[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    lp_buf_put(b, u, 4);
}

static void write_png_chunk(lp_buf *out, const char type[4], const uint8_t *data, uint32_t len){
    put_be32(out, len);
    lp_buf_put(out, type, 4);
    if (len && data) lp_buf_put(out, data, len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len && data) crc = crc32(crc, (const Bytef *)data, len);
    put_be32(out, (uint32_t)crc);
}

typedef struct {
    int mode;
} ImageRecipe;

static int image_says(Recipe *r, const char *path, char *tok){
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(ImageRecipe));
    ImageRecipe *ir = r->native_recipe;
    if (!strcmp(tok, "records")) {
        char *v = strtok(NULL, " \t\r\n");
        if (!v || (strcmp(v, "media") && strcmp(v, "pixels") && strcmp(v, "image"))) {
            fprintf(stderr, "%s: records media | pixels | image\n", path);
            return -1;
        }
        ir->mode = 1;
        return 1;
    }
    fprintf(stderr, "%s: \"%s\" is not something an image recipe says\n", path, tok);
    return -1;
}

static int image_done(Recipe *r, const char *path){
    (void)path;
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(ImageRecipe));
    r->curated = 1;
    return 1;
}

/* Balanced block tree matching file_block() so ultra-large height scanline sets stay shallow. */
static Ref codec_block_tree(Ref *items, size_t n){
    if (!n) return (Ref){ 0 };
    if (n == 1) return items[0];
    if (n <= 4096) return compose(items, (uint32_t)n, ref_above(items, n));
    size_t nup = (n + 4095) / 4096;
    Ref *up = malloc(sizeof *up * nup);
    for (size_t i = 0; i < nup; i++) {
        size_t a = i * 4096;
        size_t k = n - a < 4096 ? n - a : 4096;
        up[i] = k == 1 ? items[a] : compose(items + a, (uint32_t)k, ref_above(items + a, k));
    }
    Ref res = codec_block_tree(up, nup);
    free(up);
    return res;
}

static void image_read(const Recipe *r, File *f, const uint8_t *src, size_t n){
    (void)r;
    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(src, n > (size_t)0x7FFFFFFF ? 0x7FFFFFFF : (int)n, &w, &h, &comp, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        f->invalid = 1;
        return;
    }
    lp_scalars *sc = lp_scalars_of(T0, codecs_compose_sink, NULL);

    /* Extract any PNG tEXt / iTXt / zTXt metadata chunks alongside the pixel raster */
    lp_vec(Ref) meta = { 0 };
    if (n >= 8 && !memcmp(src, "\x89PNG\r\n\x1a\n", 8)) {
        size_t pos = 8;
        while (pos + 12 <= n) {
            uint32_t clen = be32(src + pos);
            const char *ctype = (const char *)(src + pos + 4);
            pos += 8;
            if (clen > n - pos - 4) break;
            if (!memcmp(ctype, "tEXt", 4)) {
                const uint8_t *nul = memchr(src + pos, 0, clen);
                if (nul) {
                    size_t kl = (size_t)(nul - (src + pos));
                    size_t vl = clen - kl - 1;
                    Ref kv[2] = {
                        text_ref(CTX[0], src + pos, kl),
                        vl ? text_ref(CTX[0], nul + 1, vl) : text_ref(CTX[0], (const uint8_t *)"", 0)
                    };
                    if (vl) lp_push(&meta, said_tuple(compose(kv, 2, ref_above(kv, 2))));
                }
            }
            pos += clen + 4;
            if (!memcmp(ctype, "IEND", 4)) break;
        }
    }

    Ref *rows = malloc(sizeof *rows * (size_t)h);
    Ref *row_pix = malloc(sizeof *row_pix * (size_t)w);
    for (int y = 0; y < h; y++) {
        const uint8_t *rp = rgba + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; x++) {
            const uint8_t *px = rp + (size_t)x * 4u;
            Ref ch[4] = {
                lp_scalar_u8(sc, px[0]),
                lp_scalar_u8(sc, px[1]),
                lp_scalar_u8(sc, px[2]),
                lp_scalar_u8(sc, px[3])
            };
            row_pix[x] = compose(ch, 4, ref_above(ch, 4));
        }
        rows[y] = compose(row_pix, (uint32_t)w, ref_above(row_pix, (size_t)w));
    }
    free(row_pix);
    stbi_image_free(rgba);

    Ref rows_root = codec_block_tree(rows, (size_t)h);
    free(rows);

    Ref hdr_parts[4] = {
        text_ref(CTX[0], (const uint8_t *)IMAGE_MAGIC, sizeof IMAGE_MAGIC - 1),
        lp_ref_u64(T0, (uint64_t)w, codecs_compose_sink, NULL),
        lp_ref_u64(T0, (uint64_t)h, codecs_compose_sink, NULL),
        lp_ref_u64(T0, (uint64_t)(comp > 0 ? comp : 4), codecs_compose_sink, NULL)
    };
    Ref hdr = compose(hdr_parts, 4, ref_above(hdr_parts, 4));
    Ref root_parts[2] = { hdr, rows_root };
    Ref content = compose(root_parts, 2, ref_above(root_parts, 2));

    lp_vec(Ref) claims = { 0 };
    Ref dim_parts[3] = {
        content,
        text_ref(CTX[0], (const uint8_t *)"dimensions", 10),
        hdr
    };
    lp_push(&claims, compose(dim_parts, 3, ref_above(dim_parts, 3)));
    for (size_t i = 0; i < meta.n; i++) {
        Ref p[2] = { content, meta.v[i] };
        lp_push(&claims, compose(p, 2, ref_above(p, 2)));
    }
    lp_vec_free(&meta);
    lp_codec_emit_claim_record(f, &content, claims.v, claims.n);
    lp_vec_free(&claims);

    f->trunk = content;
    f->laid = 0;
    f->records += 1;
    lp_scalars_free(sc);
}

static int find_image_content(const lp_id *id, const lp_tier0_record *t0, int depth,
                              uint64_t *w_out, uint64_t *h_out, lp_id *rows_root_out){
    if (depth > 4) return 0;
    Node *x = table_find(id);
    if (!x) return 0;
    if (x->nv == 2) {
        const Vtx *v0 = &VTX[x->voff];
        const Vtx *v1 = &VTX[x->voff + 1];
        Node *hn = table_find(&v0->id);
        if (hn && hn->nv == 4) {
            char *magic = NULL; size_t ml = 0;
            if (lp_codec_realize_text(&VTX[hn->voff].id, t0, &magic, &ml)
                && ml == sizeof IMAGE_MAGIC - 1 && !memcmp(magic, IMAGE_MAGIC, ml)) {
                free(magic);
                uint64_t w = 0, h = 0;
                if (decode_u64_node(&VTX[hn->voff + 1].id, t0, &w)
                    && decode_u64_node(&VTX[hn->voff + 2].id, t0, &h)
                    && w > 0 && h > 0) {
                    *w_out = w;
                    *h_out = h;
                    *rows_root_out = v1->id;
                    return 1;
                }
            }
            free(magic);
        }
    }
    for (uint32_t v = 0; v < x->nv; v++) {
        if (find_image_content(&VTX[x->voff + v].id, t0, depth + 1, w_out, h_out, rows_root_out))
            return 1;
    }
    return 0;
}

static int decode_pixel_rgba(const lp_id *px_id, const lp_tier0_record *t0, uint8_t rgba[4]){
    Node *p = table_find(px_id);
    if (!p) return 0;
    uint64_t ch[4];
    size_t nc = 0;
    for (uint32_t v = 0; v < p->nv; v++) {
        const Vtx *vt = &VTX[p->voff + v];
        uint64_t val = 0;
        if (!decode_u64_node(&vt->id, t0, &val) || val > 255) return 0;
        uint32_t run = VRUN(vt->m);
        for (uint32_t k = 0; k < run && nc < 4; k++) ch[nc++] = val;
    }
    if (nc != 4) return 0;
    for (size_t i = 0; i < 4; i++) rgba[i] = (uint8_t)ch[i];
    return 1;
}

static int emit_row_pixels(const lp_id *row_id, const lp_tier0_record *t0,
                           uint64_t w, uint8_t *dst_row){
    Node *rn = table_find(row_id);
    if (!rn) return 0;
    uint64_t col = 0;
    for (uint32_t v = 0; v < rn->nv; v++) {
        const Vtx *vt = &VTX[rn->voff + v];
        uint8_t rgba[4];
        if (!decode_pixel_rgba(&vt->id, t0, rgba)) return 0;
        uint32_t run = VRUN(vt->m);
        for (uint32_t k = 0; k < run; k++) {
            if (col >= w) return 0;
            memcpy(dst_row + col * 4u, rgba, 4);
            col++;
        }
    }
    return col == w;
}

static int emit_rows_tree(const lp_id *node_id, const lp_tier0_record *t0, int depth,
                          uint64_t w, uint64_t h, uint64_t *cur_y, uint8_t *scanlines){
    if (*cur_y >= h) return 0;
    if (depth == 0) {
        uint8_t *row_buf = scanlines + (*cur_y) * (1u + w * 4u);
        row_buf[0] = 0; /* PNG filter None */
        if (!emit_row_pixels(node_id, t0, w, row_buf + 1)) return 0;
        (*cur_y)++;
        return 1;
    }
    Node *n = table_find(node_id);
    if (!n) return 0;
    for (uint32_t v = 0; v < n->nv; v++) {
        const Vtx *vt = &VTX[n->voff + v];
        uint32_t run = VRUN(vt->m);
        for (uint32_t k = 0; k < run; k++) {
            if (!emit_rows_tree(&vt->id, t0, depth - 1, w, h, cur_y, scanlines)) return 0;
        }
    }
    return 1;
}

int lp_image_realize_png(const lp_id *root, const lp_tier0_record *t0,
                         uint8_t **png_out, size_t *png_len){
    *png_out = NULL;
    *png_len = 0;
    if (!root) return 0;
    const lp_tier0_record *tier0 = t0 ? t0 : T0;
    uint64_t w = 0, h = 0;
    lp_id rows_root = { 0 };
    if (!find_image_content(root, tier0, 0, &w, &h, &rows_root)) return 0;
    if (w > 65535u || h > 65535u || (h && w > (SIZE_MAX / 4u - 1u) / h)) return 0;

    size_t raw_len = (size_t)h * (1u + (size_t)w * 4u);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return 0;
    uint64_t nr = h; int row_depth = 0;
    while (nr > 1u) { row_depth++; nr = (nr + 4095u) / 4096u; }
    uint64_t cur_y = 0;
    if (!emit_rows_tree(&rows_root, tier0, row_depth, w, h, &cur_y, raw) || cur_y != h) {
        free(raw);
        return 0;
    }

    uLongf zcap = compressBound((uLong)raw_len);
    uint8_t *zbuf = malloc(zcap);
    if (!zbuf || compress2(zbuf, &zcap, raw, (uLong)raw_len, Z_DEFAULT_COMPRESSION) != Z_OK) {
        free(raw);
        free(zbuf);
        return 0;
    }
    free(raw);

    lp_buf out = { 0 };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    lp_buf_put(&out, sig, 8);
    uint8_t ihdr[13] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, 6, 0, 0, 0
    };
    write_png_chunk(&out, "IHDR", ihdr, 13);
    write_png_chunk(&out, "IDAT", zbuf, (uint32_t)zcap);
    free(zbuf);
    write_png_chunk(&out, "IEND", NULL, 0);

    *png_len = out.n;
    *png_out = (uint8_t *)lp_buf_take(&out);
    return 1;
}

const NativeGrammar image_grammar = {
    "image", image_says, image_done, image_read, NULL, NULL, NULL
};
