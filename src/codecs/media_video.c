#include "codecs.h"
#include <blake3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_be32(const uint8_t *p){
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd_be64(const uint8_t *p){
    return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}
static uint32_t rd_le32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_le16(const uint8_t *p){
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static lp_ref video_compose_sink(void *sink, const lp_ref *ch, uint32_t n, uint8_t tier){
    (void)sink;
    return compose(ch, n, tier);
}

typedef struct {
    char     container[16];
    char     codec[32];
    uint32_t width, height;
    uint32_t timescale;
    uint64_t duration;
    uint64_t frame_count;
} VideoMeta;

typedef struct {
    int mode;
} VideoRecipe;

static int video_says(Recipe *r, const char *path, char *tok){
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(VideoRecipe));
    VideoRecipe *vr = r->native_recipe;
    if (!strcmp(tok, "records")) {
        char *v = strtok(NULL, " \t\r\n");
        if (!v || (strcmp(v, "media") && strcmp(v, "video") && strcmp(v, "frames"))) {
            fprintf(stderr, "%s: records media | video | frames\n", path);
            return -1;
        }
        vr->mode = 1;
        return 1;
    }
    fprintf(stderr, "%s: \"%s\" is not something a video recipe says\n", path, tok);
    return -1;
}

static int video_done(Recipe *r, const char *path){
    (void)path;
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(VideoRecipe));
    r->curated = 1;
    return 1;
}

typedef struct {
    uint64_t pts;
    uint32_t bytes;
    lp_id    digest;
} FrameDesc;

typedef lp_vec(FrameDesc) FrameVec;

static void parse_isobmff_boxes(const uint8_t *buf, size_t len, VideoMeta *meta, FrameVec *frames, int depth){
    if (depth > 8) return;
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint64_t sz = rd_be32(buf + pos);
        const char *typ = (const char *)(buf + pos + 4);
        size_t hdr = 8;
        if (sz == 1 && pos + 16 <= len) {
            sz = rd_be64(buf + pos + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = len - pos;
        }
        if (sz < hdr || sz > len - pos) break;
        const uint8_t *pay = buf + pos + hdr;
        size_t plen = (size_t)sz - hdr;

        if (!memcmp(typ, "ftyp", 4) && plen >= 4) {
            snprintf(meta->container, sizeof meta->container, "isobmff");
        } else if (!memcmp(typ, "mvhd", 4) && plen >= 20) {
            uint8_t ver = pay[0];
            if (ver == 0 && plen >= 20) {
                meta->timescale = rd_be32(pay + 12);
                meta->duration = rd_be32(pay + 16);
            } else if (ver == 1 && plen >= 32) {
                meta->timescale = rd_be32(pay + 20);
                meta->duration = rd_be64(pay + 24);
            }
        } else if (!memcmp(typ, "tkhd", 4) && plen >= 84) {
            uint8_t ver = pay[0];
            size_t wo = (ver == 1) ? 88 : 76;
            if (wo + 8 <= plen) {
                uint32_t w = rd_be32(pay + wo) >> 16;
                uint32_t h = rd_be32(pay + wo + 4) >> 16;
                if (w > meta->width) meta->width = w;
                if (h > meta->height) meta->height = h;
            }
        } else if (!memcmp(typ, "stsd", 4) && plen >= 16) {
            uint32_t entries = rd_be32(pay + 4);
            if (entries > 0 && plen >= 16) {
                snprintf(meta->codec, sizeof meta->codec, "%c%c%c%c",
                         pay[12], pay[13], pay[14], pay[15]);
            }
        } else if (!memcmp(typ, "stsz", 4) && plen >= 12) {
            uint32_t sample_size = rd_be32(pay + 4);
            uint32_t count = rd_be32(pay + 8);
            meta->frame_count += count;
            if (sample_size > 0) {
                for (uint32_t i = 0; i < count && frames->n < 4096; i++) {
                    FrameDesc fd = { .pts = i, .bytes = sample_size };
                    lp_push(frames, fd);
                }
            } else if (plen >= 12 + (size_t)count * 4u) {
                for (uint32_t i = 0; i < count && frames->n < 4096; i++) {
                    FrameDesc fd = { .pts = i, .bytes = rd_be32(pay + 12 + (size_t)i * 4u) };
                    lp_push(frames, fd);
                }
            }
        } else if (!memcmp(typ, "moov", 4) || !memcmp(typ, "trak", 4)
                || !memcmp(typ, "mdia", 4) || !memcmp(typ, "minf", 4)
                || !memcmp(typ, "stbl", 4)) {
            parse_isobmff_boxes(pay, plen, meta, frames, depth + 1);
        }
        pos += (size_t)sz;
    }
}

static int parse_video_container(const uint8_t *src, size_t n, VideoMeta *meta, FrameVec *frames){
    memset(meta, 0, sizeof *meta);
    if (n >= 32 && !memcmp(src, "DKIF", 4)) {
        snprintf(meta->container, sizeof meta->container, "ivf");
        snprintf(meta->codec, sizeof meta->codec, "%c%c%c%c", src[8], src[9], src[10], src[11]);
        meta->width = rd_le16(src + 12);
        meta->height = rd_le16(src + 14);
        meta->timescale = rd_le32(src + 16);
        meta->frame_count = rd_le32(src + 24);
        size_t hdr_len = rd_le16(src + 6);
        if (hdr_len < 32) hdr_len = 32;
        size_t pos = hdr_len;
        while (pos + 12 <= n) {
            uint32_t fsz = rd_le32(src + pos);
            uint64_t pts = (uint64_t)rd_le32(src + pos + 4) | ((uint64_t)rd_le32(src + pos + 8) << 32);
            pos += 12;
            if (fsz > n - pos) break;
            FrameDesc fd = { .pts = pts, .bytes = fsz };
            blake3_hasher bh; blake3_hasher_init(&bh);
            blake3_hasher_update(&bh, src + pos, fsz);
            blake3_hasher_finalize(&bh, fd.digest.b, 16);
            lp_push(frames, fd);
            pos += fsz;
        }
        if (!meta->frame_count) meta->frame_count = frames->n;
        return meta->width > 0 && meta->height > 0;
    }

    if (n >= 12 && !memcmp(src + 4, "ftyp", 4)) {
        parse_isobmff_boxes(src, n, meta, frames, 0);
        return meta->container[0] != 0;
    }

    if (n >= 12 && !memcmp(src, "RIFF", 4) && !memcmp(src + 8, "AVI ", 4)) {
        snprintf(meta->container, sizeof meta->container, "avi");
        size_t pos = 12;
        while (pos + 8 <= n) {
            const char *ck = (const char *)(src + pos);
            uint32_t sz = rd_le32(src + pos + 4);
            pos += 8;
            if (sz > n - pos) break;
            if (!memcmp(ck, "avih", 4) && sz >= 40) {
                meta->frame_count = rd_le32(src + pos + 16);
                meta->width = rd_le32(src + pos + 32);
                meta->height = rd_le32(src + pos + 36);
            } else if (!memcmp(ck, "LIST", 4) && sz >= 4 && !memcmp(src + pos, "hdrl", 4)) {
                size_t lpos = pos + 4, lend = pos + sz;
                while (lpos + 8 <= lend) {
                    const char *lck = (const char *)(src + lpos);
                    uint32_t lsz = rd_le32(src + lpos + 4);
                    lpos += 8;
                    if (lsz > lend - lpos) break;
                    if (!memcmp(lck, "avih", 4) && lsz >= 40) {
                        meta->frame_count = rd_le32(src + lpos + 16);
                        meta->width = rd_le32(src + lpos + 32);
                        meta->height = rd_le32(src + lpos + 36);
                    }
                    lpos += lsz + (lsz & 1u);
                }
            }
            pos += sz + (sz & 1u);
        }
        return 1;
    }

    if (n >= 4 && src[0] == 0x1A && src[1] == 0x45 && src[2] == 0xDF && src[3] == 0xA3) {
        snprintf(meta->container, sizeof meta->container, "ebml");
        return 1;
    }
    return 0;
}

static Ref meta_kv(const char *k, Ref val){
    Ref pair[2] = { text_ref(CTX[0], (const uint8_t *)k, strlen(k)), val };
    return said_tuple(compose(pair, 2, ref_above(pair, 2)));
}

static void video_read(const Recipe *r, File *f, const uint8_t *src, size_t n){
    (void)r;
    VideoMeta meta;
    FrameVec frames = { 0 };
    if (!parse_video_container(src, n, &meta, &frames)) {
        lp_vec_free(&frames);
        f->invalid = 1;
        return;
    }

    Ref hdr_parts[5];
    size_t nh = 0;
    hdr_parts[nh++] = meta_kv("container", text_ref(CTX[0], (const uint8_t *)meta.container, strlen(meta.container)));
    if (meta.codec[0]) hdr_parts[nh++] = meta_kv("codec", text_ref(CTX[0], (const uint8_t *)meta.codec, strlen(meta.codec)));
    hdr_parts[nh++] = meta_kv("width", lp_ref_u64(T0, meta.width, video_compose_sink, NULL));
    hdr_parts[nh++] = meta_kv("height", lp_ref_u64(T0, meta.height, video_compose_sink, NULL));
    hdr_parts[nh++] = meta_kv("frames", lp_ref_u64(T0, meta.frame_count, video_compose_sink, NULL));
    Ref track = compose(hdr_parts, (uint32_t)nh, ref_above(hdr_parts, nh));

    Ref content = track;
    if (frames.n > 0) {
        size_t nf = frames.n < 4096 ? frames.n : 4096;
        Ref *frefs = malloc(sizeof *frefs * nf);
        for (size_t i = 0; i < nf; i++) {
            Ref fp[2] = {
                lp_ref_u64(T0, frames.v[i].pts, video_compose_sink, NULL),
                lp_ref_u64(T0, frames.v[i].bytes, video_compose_sink, NULL)
            };
            frefs[i] = compose(fp, 2, ref_above(fp, 2));
        }
        Ref frame_seq = compose(frefs, (uint32_t)nf, ref_above(frefs, nf));
        free(frefs);
        Ref tr[2] = { track, frame_seq };
        content = compose(tr, 2, ref_above(tr, 2));
    }
    lp_vec_free(&frames);

    Ref vclaim_p[3] = {
        content,
        text_ref(CTX[0], (const uint8_t *)"video_track", 11),
        track
    };
    Ref vc = compose(vclaim_p, 3, ref_above(vclaim_p, 3));
    lp_codec_emit_claim_record(f, &content, &vc, 1);

    f->trunk = content;
    f->laid = 0;
    f->records += 1;
}

const NativeGrammar video_grammar = {
    "video", video_says, video_done, video_read, NULL, NULL, NULL
};
