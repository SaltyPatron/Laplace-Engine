#include "codecs.h"
#include "laplace/os.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations matching vendored dr_wav.h / dr_flac.h / dr_mp3.h / stb_vorbis.c */
#define DR_WAV_NO_STDIO
#include "third_party/dr_wav.h"
#define DR_FLAC_NO_STDIO
#include "third_party/dr_flac.h"
#define DR_MP3_NO_STDIO
#include "third_party/dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "third_party/stb_vorbis.c"

#define AUDIO_MAGIC      "lp-audio-v1"
#define AUDIO_FRAME_SIZE 1024u

static lp_ref audio_compose_sink(void *sink, const lp_ref *ch, uint32_t n, uint8_t tier){
    (void)sink;
    return compose(ch, n, tier);
}

typedef struct {
    int mode;
} AudioRecipe;

static int audio_says(Recipe *r, const char *path, char *tok){
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(AudioRecipe));
    AudioRecipe *ar = r->native_recipe;
    if (!strcmp(tok, "records")) {
        char *v = strtok(NULL, " \t\r\n");
        if (!v || (strcmp(v, "media") && strcmp(v, "audio") && strcmp(v, "samples"))) {
            fprintf(stderr, "%s: records media | audio | samples\n", path);
            return -1;
        }
        ar->mode = 1;
        return 1;
    }
    fprintf(stderr, "%s: \"%s\" is not something an audio recipe says\n", path, tok);
    return -1;
}

static int audio_done(Recipe *r, const char *path){
    (void)path;
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(AudioRecipe));
    r->curated = 1;
    return 1;
}

static int decode_audio_pcm16(const uint8_t *src, size_t n, int16_t **pcm_out,
                              uint32_t *sr_out, uint32_t *ch_out, uint64_t *frames_out,
                              const char **fmt_out){
    *pcm_out = NULL;
    *sr_out = 0;
    *ch_out = 0;
    *frames_out = 0;
    *fmt_out = "unknown";

    if (n >= 12 && (!memcmp(src, "RIFF", 4) || !memcmp(src, "RF64", 4))) {
        drwav wav;
        if (drwav_init_memory(&wav, src, n, NULL)) {
            uint64_t nf = wav.totalPCMFrameCount;
            uint32_t ch = wav.channels, sr = wav.sampleRate;
            if (ch > 0 && nf > 0 && nf <= SIZE_MAX / ch / sizeof(int16_t)) {
                int16_t *pcm = malloc((size_t)nf * ch * sizeof(int16_t));
                if (pcm) {
                    drwav_uint64 got = drwav_read_pcm_frames_s16(&wav, nf, pcm);
                    drwav_uninit(&wav);
                    *pcm_out = pcm; *sr_out = sr; *ch_out = ch; *frames_out = (uint64_t)got;
                    *fmt_out = "wav";
                    return got > 0;
                }
            }
            drwav_uninit(&wav);
        }
    }

    if (n >= 4 && !memcmp(src, "fLaC", 4)) {
        drflac *fl = drflac_open_memory(src, n, NULL);
        if (fl) {
            uint64_t nf = fl->totalPCMFrameCount;
            uint32_t ch = fl->channels, sr = fl->sampleRate;
            if (ch > 0 && nf > 0 && nf <= SIZE_MAX / ch / sizeof(int16_t)) {
                int16_t *pcm = malloc((size_t)nf * ch * sizeof(int16_t));
                if (pcm) {
                    drflac_uint64 got = drflac_read_pcm_frames_s16(fl, nf, pcm);
                    drflac_close(fl);
                    *pcm_out = pcm; *sr_out = sr; *ch_out = ch; *frames_out = (uint64_t)got;
                    *fmt_out = "flac";
                    return got > 0;
                }
            }
            drflac_close(fl);
        }
    }

    if (n >= 4 && !memcmp(src, "OggS", 4)) {
        int ch = 0, sr = 0;
        short *pcm = NULL;
        int got = stb_vorbis_decode_memory(src, n > (size_t)INT_MAX ? INT_MAX : (int)n, &ch, &sr, &pcm);
        if (got > 0 && pcm && ch > 0 && sr > 0) {
            *pcm_out = pcm; *sr_out = (uint32_t)sr; *ch_out = (uint32_t)ch; *frames_out = (uint64_t)got;
            *fmt_out = "ogg";
            return 1;
        }
        free(pcm);
    }

    drmp3 mp3;
    if (drmp3_init_memory(&mp3, src, n, NULL)) {
        drmp3_uint64 total = drmp3_get_pcm_frame_count(&mp3);
        uint32_t ch = mp3.channels, sr = mp3.sampleRate;
        if (ch > 0 && total > 0 && total <= SIZE_MAX / ch / sizeof(int16_t)) {
            int16_t *pcm = malloc((size_t)total * ch * sizeof(int16_t));
            if (pcm) {
                drmp3_uint64 got = drmp3_read_pcm_frames_s16(&mp3, total, pcm);
                drmp3_uninit(&mp3);
                *pcm_out = pcm; *sr_out = sr; *ch_out = ch; *frames_out = (uint64_t)got;
                *fmt_out = "mp3";
                return got > 0;
            }
        }
        drmp3_uninit(&mp3);
    }
    return 0;
}

static Ref audio_block_tree(Ref *items, size_t n){
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
    Ref res = audio_block_tree(up, nup);
    free(up);
    return res;
}

static void audio_read(const Recipe *r, File *f, const uint8_t *src, size_t n){
    (void)r;
    int16_t *pcm = NULL;
    uint32_t sr = 0, ch = 0;
    uint64_t nf = 0;
    const char *fmt = "unknown";
    if (!decode_audio_pcm16(src, n, &pcm, &sr, &ch, &nf, &fmt)) {
        free(pcm);
        f->invalid = 1;
        return;
    }

    lp_scalars *sc = lp_scalars_of(T0, audio_compose_sink, NULL);
    size_t nblocks = (size_t)((nf + AUDIO_FRAME_SIZE - 1u) / AUDIO_FRAME_SIZE);
    Ref *blocks = malloc(sizeof *blocks * (nblocks ? nblocks : 1u));
    Ref *srefs = malloc(sizeof *srefs * AUDIO_FRAME_SIZE * ch);

    for (size_t bi = 0; bi < nblocks; bi++) {
        uint64_t f0 = (uint64_t)bi * AUDIO_FRAME_SIZE;
        uint64_t fc = nf - f0 < AUDIO_FRAME_SIZE ? nf - f0 : AUDIO_FRAME_SIZE;
        size_t ns = (size_t)(fc * ch);
        const int16_t *sp = pcm + f0 * ch;
        for (size_t i = 0; i < ns; i++) srefs[i] = lp_scalar_i16(sc, sp[i]);
        blocks[bi] = compose(srefs, (uint32_t)ns, ref_above(srefs, ns));
    }
    free(srefs);
    free(pcm);

    Ref blocks_root = audio_block_tree(blocks, nblocks);
    free(blocks);

    Ref hdr_parts[4] = {
        text_ref(CTX[0], (const uint8_t *)AUDIO_MAGIC, sizeof AUDIO_MAGIC - 1),
        lp_ref_u64(T0, (uint64_t)sr, audio_compose_sink, NULL),
        lp_ref_u64(T0, (uint64_t)ch, audio_compose_sink, NULL),
        lp_ref_u64(T0, nf, audio_compose_sink, NULL)
    };
    Ref hdr = compose(hdr_parts, 4, ref_above(hdr_parts, 4));
    Ref root_parts[2] = { hdr, blocks_root };
    Ref content = compose(root_parts, 2, ref_above(root_parts, 2));

    Ref meta_parts[3] = {
        content,
        text_ref(CTX[0], (const uint8_t *)"audio_format", 12),
        text_ref(CTX[0], (const uint8_t *)fmt, strlen(fmt))
    };
    Ref mc = compose(meta_parts, 3, ref_above(meta_parts, 3));
    lp_codec_emit_claim_record(f, &content, &mc, 1);

    f->trunk = content;
    f->laid = 0;
    f->records += 1;
    lp_scalars_free(sc);
}

static int decode_u64_id(const lp_id *id, const lp_tier0_record *t0, uint64_t *val){
    char *s = NULL; size_t len = 0;
    if (!lp_codec_realize_text(id, t0, &s, &len) || !len || len > 20) { free(s); return 0; }
    for (size_t i = 0; i < len; i++) if (s[i] < '0' || s[i] > '9') { free(s); return 0; }
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    int ok = (end == s + len);
    free(s);
    if (!ok) return 0;
    *val = (uint64_t)v;
    return 1;
}

static int decode_i16_id(const lp_id *id, const lp_tier0_record *t0, int16_t *val){
    char *s = NULL; size_t len = 0;
    if (!lp_codec_realize_text(id, t0, &s, &len) || !len || len > 8) { free(s); return 0; }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    int ok = (end == s + len) && v >= -32768L && v <= 32767L;
    free(s);
    if (!ok) return 0;
    *val = (int16_t)v;
    return 1;
}

static int find_audio_content(const lp_id *id, const lp_tier0_record *t0, int depth,
                              uint32_t *sr_out, uint32_t *ch_out, uint64_t *nf_out,
                              lp_id *blocks_root_out){
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
                && ml == sizeof AUDIO_MAGIC - 1 && !memcmp(magic, AUDIO_MAGIC, ml)) {
                free(magic);
                uint64_t sr = 0, ch = 0, nf = 0;
                if (decode_u64_id(&VTX[hn->voff + 1].id, t0, &sr)
                    && decode_u64_id(&VTX[hn->voff + 2].id, t0, &ch)
                    && decode_u64_id(&VTX[hn->voff + 3].id, t0, &nf)
                    && sr > 0 && ch > 0 && nf > 0) {
                    *sr_out = (uint32_t)sr;
                    *ch_out = (uint32_t)ch;
                    *nf_out = nf;
                    *blocks_root_out = v1->id;
                    return 1;
                }
            }
            free(magic);
        }
    }
    for (uint32_t v = 0; v < x->nv; v++) {
        if (find_audio_content(&VTX[x->voff + v].id, t0, depth + 1, sr_out, ch_out, nf_out, blocks_root_out))
            return 1;
    }
    return 0;
}

static int collect_frame_samples(const lp_id *frame_id, const lp_tier0_record *t0,
                                 size_t total_samples, size_t *cursor, int16_t *pcm){
    Node *n = table_find(frame_id);
    if (!n) return 0;
    for (uint32_t v = 0; v < n->nv; v++) {
        const Vtx *vt = &VTX[n->voff + v];
        uint32_t run = VRUN(vt->m);
        int16_t s = 0;
        if (!decode_i16_id(&vt->id, t0, &s) || *cursor + run > total_samples) return 0;
        for (uint32_t k = 0; k < run; k++) pcm[(*cursor)++] = s;
    }
    return 1;
}

static int collect_audio_blocks(const lp_id *id, const lp_tier0_record *t0, int depth,
                                size_t total_samples, size_t *cursor, int16_t *pcm){
    if (depth == 0) return collect_frame_samples(id, t0, total_samples, cursor, pcm);
    Node *n = table_find(id);
    if (!n) return 0;
    for (uint32_t v = 0; v < n->nv; v++) {
        const Vtx *vt = &VTX[n->voff + v];
        uint32_t run = VRUN(vt->m);
        for (uint32_t k = 0; k < run; k++) {
            if (!collect_audio_blocks(&vt->id, t0, depth - 1, total_samples, cursor, pcm)) return 0;
        }
    }
    return 1;
}

static void put_le16(uint8_t *p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int lp_audio_realize_wav(const lp_id *root, const lp_tier0_record *t0,
                         uint8_t **wav_out, size_t *wav_len){
    *wav_out = NULL;
    *wav_len = 0;
    if (!root) return 0;
    const lp_tier0_record *tier0 = t0 ? t0 : T0;
    uint32_t sr = 0, ch = 0;
    uint64_t nf = 0;
    lp_id blocks_root = { 0 };
    if (!find_audio_content(root, tier0, 0, &sr, &ch, &nf, &blocks_root)) return 0;
    if (nf > (uint64_t)((SIZE_MAX - 44u) / ch / 2u)) return 0;

    size_t total_samples = (size_t)nf * ch;
    size_t data_bytes = total_samples * 2u;
    uint8_t *buf = malloc(44u + data_bytes);
    if (!buf) return 0;

    uint64_t nb = (nf + AUDIO_FRAME_SIZE - 1u) / AUDIO_FRAME_SIZE;
    int block_depth = 0;
    while (nb > 1u) { block_depth++; nb = (nb + 4095u) / 4096u; }

    int16_t *pcm = (int16_t *)(buf + 44u);
    size_t cursor = 0;
    if (!collect_audio_blocks(&blocks_root, tier0, block_depth, total_samples, &cursor, pcm) || cursor != total_samples) {
        free(buf);
        return 0;
    }

    memcpy(buf, "RIFF", 4);
    put_le32(buf + 4, (uint32_t)(36u + data_bytes));
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    put_le32(buf + 16, 16u);
    put_le16(buf + 20, 1u); /* PCM */
    put_le16(buf + 22, (uint16_t)ch);
    put_le32(buf + 24, sr);
    put_le32(buf + 28, sr * ch * 2u);
    put_le16(buf + 32, (uint16_t)(ch * 2u));
    put_le16(buf + 34, 16u);
    memcpy(buf + 36, "data", 4);
    put_le32(buf + 40, (uint32_t)data_bytes);

    *wav_out = buf;
    *wav_len = 44u + data_bytes;
    return 1;
}

const NativeGrammar audio_grammar = {
    "audio", audio_says, audio_done, audio_read, NULL, NULL, NULL
};
