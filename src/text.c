/* UAX #29 text: codepoint → grapheme → word segment → sentence → paragraph → file. Nothing is dropped: whitespace
 * and punctuation are constituents like everything else, so the file recomposes byte for byte. A single line break
 * inside a paragraph is read as a space for segmentation only; the bytes recorded are the original ones. */
#include "engine.h"
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int32_t *b; size_t n, cap; } Bounds;
typedef struct { Ref *v; size_t n, cap; } Refs;
struct Ctx {
    UBreakIterator *brk[3];                          /* sentence, word, character: opened once per thread */
    Bounds sb, wb, gb;
    Refs cps, g, segs, sents, paras;
    uint8_t *cpy; size_t ccap;
};

static void push(Refs *r, Ref x){ if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 256; r->v = xrealloc(r->v, r->cap * sizeof(Ref)); } r->v[r->n++] = x; }

Ctx *ctx_new(void){
    Ctx *c = calloc(1, sizeof *c); UErrorCode e = U_ZERO_ERROR;
    c->brk[0] = ubrk_open(UBRK_SENTENCE, "", NULL, 0, &e);
    c->brk[1] = ubrk_open(UBRK_WORD, "", NULL, 0, &e);
    c->brk[2] = ubrk_open(UBRK_CHARACTER, "", NULL, 0, &e);
    return c;
}
void ctx_free(Ctx *c){
    for (int i = 0; i < 3; i++) ubrk_close(c->brk[i]);
    free(c->sb.b); free(c->wb.b); free(c->gb.b); free(c->cps.v); free(c->g.v); free(c->segs.v); free(c->sents.v); free(c->paras.v); free(c->cpy); free(c);
}
static void bounds_of(UBreakIterator *bi, UText *ut, Bounds *out){
    UErrorCode e = U_ZERO_ERROR; ubrk_setUText(bi, ut, &e); out->n = 0;
    for (int32_t p = ubrk_first(bi); p != UBRK_DONE; p = ubrk_next(bi)) {
        if (out->n == out->cap) { out->cap = out->cap ? out->cap * 2 : 1024; out->b = xrealloc(out->b, out->cap * 4); }
        out->b[out->n++] = p;
    }
}
static int utf8_next(const uint8_t *s, size_t n, size_t *i, uint32_t *cp){
    uint8_t c = s[*i];
    if (c < 0x80) { *cp = c; *i += 1; return 1; }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (!len || *i + len > n) return 0;
    uint32_t v = c & (0x7F >> len);
    for (int k = 1; k < len; k++) { if ((s[*i + k] & 0xC0) != 0x80) return 0; v = (v << 6) | (s[*i + k] & 0x3F); }
    *cp = v; *i += len; return 1;
}

Ref text_ref(Ctx *c, const uint8_t *src, size_t n){
    if (n + 1 > c->ccap) { c->ccap = (n + 1) * 2; c->cpy = xrealloc(c->cpy, c->ccap); }
    uint8_t *cpy = c->cpy; memcpy(cpy, src, n);
    for (size_t i = 0; i < n; ) {                                   /* a lone line break inside a paragraph: a space */
        if (src[i] != '\n' && src[i] != '\r') { i++; continue; }
        size_t s0 = i, e0 = i + ((src[i] == '\r' && i + 1 < n && src[i + 1] == '\n') ? 2 : 1), j = e0;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        int next_blank = (j < n && (src[j] == '\n' || src[j] == '\r'));
        size_t k = s0; while (k > 0 && (src[k - 1] == ' ' || src[k - 1] == '\t')) k--;
        int prev_blank = (k == 0 || src[k - 1] == '\n' || src[k - 1] == '\r');
        if (!next_blank && !prev_blank) for (size_t z = s0; z < e0; z++) cpy[z] = ' ';
        i = e0;
    }
    UErrorCode e = U_ZERO_ERROR; UText *ut = utext_openUTF8(NULL, (const char *)cpy, (int64_t)n, &e);
    bounds_of(c->brk[0], ut, &c->sb); bounds_of(c->brk[1], ut, &c->wb); bounds_of(c->brk[2], ut, &c->gb);
    c->paras.n = 0; c->sents.n = 0; size_t wi = 0, gi = 0;
    for (size_t si = 0; si + 1 < c->sb.n; si++) {
        int32_t s0 = c->sb.b[si], s1 = c->sb.b[si + 1], w0 = s0; c->segs.n = 0;
        while (w0 < s1) {
            while (wi < c->wb.n && c->wb.b[wi] <= w0) wi++;
            int32_t w1 = (wi < c->wb.n && c->wb.b[wi] < s1) ? c->wb.b[wi] : s1, g0 = w0; c->g.n = 0;
            while (g0 < w1) {
                while (gi < c->gb.n && c->gb.b[gi] <= g0) gi++;
                int32_t g1 = (gi < c->gb.n && c->gb.b[gi] < w1) ? c->gb.b[gi] : w1; size_t i = (size_t)g0; uint32_t cp; c->cps.n = 0;
                while (i < (size_t)g1) { if (!utf8_next(src, n, &i, &cp)) { cp = 0xFFFD; i++; } push(&c->cps, atom(cp)); }
                push(&c->g, compose(c->cps.v, (uint32_t)c->cps.n, 1)); g0 = g1;
            }
            push(&c->segs, compose(c->g.v, (uint32_t)c->g.n, 2)); w0 = w1;
        }
        push(&c->sents, compose(c->segs.v, (uint32_t)c->segs.n, 3));
        int end_para = (si + 2 == c->sb.n);
        for (int32_t z = s1 - 1; z >= s0 && !end_para; z--) {
            if (src[z] != '\n' && src[z] != '\r' && src[z] != ' ' && src[z] != '\t') break;
            if ((src[z] == '\n' || src[z] == '\r') && cpy[z] != ' ') end_para = 1;
        }
        if (end_para) { push(&c->paras, compose(c->sents.v, (uint32_t)c->sents.n, 4)); c->sents.n = 0; }
    }
    if (c->sents.n) push(&c->paras, compose(c->sents.v, (uint32_t)c->sents.n, 4));
    utext_close(ut);
    return compose(c->paras.v, (uint32_t)c->paras.n, 5);
}
