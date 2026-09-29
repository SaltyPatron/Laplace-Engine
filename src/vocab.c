#define _GNU_SOURCE
/* Tokenizer vocabularies. Each token is the text it stands for, decomposed like any other text; the vocabulary is the
 * path of its tokens in index order. Byte-level BPE characters map back to bytes (GPT-2's table); SentencePiece's U+2581
 * is a space; WordPiece's ## continues a word; a byte token, or a byte-level token that is not valid UTF-8 by itself, is
 * the notation <0xAB> of each byte. The token list itself is kept as the model wrote it: the file is content too. */
#include "engine.h"
#include "json_min.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t gpt2_byte[0x180]; static int gpt2_ready;
static void gpt2_table(void){
    int bs[256], nb = 0, n = 0;
    for (int b = '!'; b <= '~'; b++) bs[nb++] = b;
    for (int b = 0xA1; b <= 0xAC; b++) bs[nb++] = b;
    for (int b = 0xAE; b <= 0xFF; b++) bs[nb++] = b;
    int is[256] = { 0 }; for (int i = 0; i < nb; i++) is[bs[i]] = 1;
    for (int i = 0; i < nb; i++) gpt2_byte[bs[i]] = (uint8_t)bs[i];
    for (int b = 0; b < 256; b++) if (!is[b]) gpt2_byte[256 + n++] = (uint8_t)b;
    gpt2_ready = 1;
}
static int utf8_valid(const uint8_t *s, size_t n){ size_t i = 0; uint32_t c; while (i < n) if (!lp_utf8_next(s, n, &i, &c)) return 0; return 1; }

Ref vocabulary_ref(Ctx *c, const uint8_t *src, size_t len, uint64_t *ntok, uint64_t *nbyte){
    Ref none; memset(&none, 0, sizeof none); *ntok = 0; *nbyte = 0;
    jdoc d = j_parse((const char *)src, len);
    int64_t model = d.n ? j_get(&d, 0, "model") : -1, voc = model >= 0 ? j_get(&d, (uint32_t)model, "vocab") : -1;
    if (voc < 0 || d.v[voc].t != J_OBJ) return none;
    int64_t mt = j_get(&d, (uint32_t)model, "type"); const char *mtype = mt >= 0 && d.v[mt].t == J_STR ? d.v[mt].str : "";
    int wordpiece = !strcmp(mtype, "WordPiece"), bytelevel = !wordpiece && memmem(src, len, "\"ByteLevel\"", 11) != NULL;
    #pragma omp critical(gpt2)
    if (bytelevel && !gpt2_ready) gpt2_table();
    size_t maxid = 0; const jnode *vo = &d.v[voc];
    for (uint32_t i = 0; i < vo->n; i += 2) { size_t id = (size_t)d.v[d.kids[vo->first + i + 1]].num; if (id > maxid) maxid = id; }
    int64_t added = j_get(&d, 0, "added_tokens");
    if (added >= 0) for (uint32_t i = 0; i < d.v[added].n; i++) { uint32_t o = d.kids[d.v[added].first + i]; size_t id = (size_t)j_num(&d, o, "id", 0); if (id > maxid) maxid = id; }
    const char **tok = calloc(maxid + 1, sizeof(char *));
    for (uint32_t i = 0; i < vo->n; i += 2) tok[(size_t)d.v[d.kids[vo->first + i + 1]].num] = d.v[d.kids[vo->first + i]].str;
    if (added >= 0) for (uint32_t i = 0; i < d.v[added].n; i++) { uint32_t o = d.kids[d.v[added].first + i]; int64_t ct = j_get(&d, o, "content");
        if (ct >= 0) tok[(size_t)j_num(&d, o, "id", 0)] = d.v[ct].str; }
    Ref *refs = malloc(sizeof(Ref) * (maxid + 1)); size_t nr = 0; size_t bcap = 1024; uint8_t *b = malloc(bcap);
    for (size_t id = 0; id <= maxid; id++) {
        const char *s = tok[id]; if (!s || !*s) continue;
        size_t L = strlen(s), n = 0; Ref r;
        if (L + 4 > bcap) { bcap = (L + 4) * 2; b = xrealloc(b, bcap); }
        if (L == 6 && s[0] == '<' && s[1] == '0' && s[2] == 'x' && s[5] == '>') {     /* SentencePiece byte token */
            uint8_t v = (uint8_t)strtol(s + 3, NULL, 16); r = notation_ref(c, &v, 1); (*nbyte)++;
        } else if (bytelevel) {
            int ok = 1; size_t i = 0;
            while (i < L) {                                                               /* characters back to bytes */
                size_t i0 = i; uint32_t cp; if (!lp_utf8_next((const uint8_t *)s, L, &i, &cp)) { ok = 0; break; }
                if (cp < 0x180 && (cp >= 256 || ((cp >= '!' && cp <= '~') || (cp >= 0xA1 && cp <= 0xAC) || (cp >= 0xAE && cp <= 0xFF))))
                    b[n++] = gpt2_byte[cp];
                else { memcpy(b + n, s + i0, i - i0); n += i - i0; }                       /* added tokens: literal text */
            }
            if (!ok) continue;
            if (utf8_valid(b, n)) r = text_ref(c, b, n); else { r = notation_ref(c, b, n); (*nbyte)++; }
        } else {
            for (size_t i = 0; i < L; ) {
                if ((unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x96 && (unsigned char)s[i + 2] == 0x81) { b[n++] = ' '; i += 3; }
                else b[n++] = (uint8_t)s[i++];
            }
            if (wordpiece && n > 2 && b[0] == '#' && b[1] == '#') { memmove(b, b + 2, n - 2); n -= 2; }
            r = text_ref(c, b, n);
        }
        refs[nr++] = r; (*ntok)++;
    }
    Ref trunk = nr ? compose(refs, (uint32_t)nr, 5) : none;
    free(refs); free(b); free(tok);
    for (uint32_t i = 0; i < d.n; i++) if (d.v[i].t == J_STR) free(d.v[i].str);
    free(d.v); free(d.kids);
    return trunk;
}
