#ifndef LAPLACE_GUTENBERG_DECODER_H
#define LAPLACE_GUTENBERG_DECODER_H

/* Input decoders for Project Gutenberg text editions:
 *   "gutenberg"     raw UTF-8 work body with PG preamble, START/END markers,
 *                   and license postamble stripped cleanly.
 *   "gutenberg-tsv" unescaped UTF-8 TSV ("title\tauthor\tlanguage\trelease_date\tebook\tparagraph")
 *                   separating header metadata from blank-line-delimited work paragraphs.
 * Included directly by grammars.c without introducing new platform headers. */
#include "engine.h"
#include "laplace/os.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    char title[512];
    char author[512];
    char language[128];
    char release_date[128];
    char ebook[64];
} PgMeta;

typedef struct {
    uint8_t *raw;
    uint8_t *buf;
    size_t len, pos;
    int err;
} PgStream;

extern const InputDecoder gutenberg_decoder, gutenberg_tsv_decoder;

static int pg_ends_gz(const char *path){
    size_t n = strlen(path);
    return n > 3 && !strcasecmp(path + n - 3, ".gz");
}

static uint8_t *pg_slurp(const char *path, size_t *out_n){
    *out_n = 0;
    size_t cap = 65536, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;
    if (pg_ends_gz(path)) {
        gzFile gz = gzopen(path, "rb");
        if (!gz) { free(buf); return NULL; }
        gzbuffer(gz, 1 << 16);
        for (;;) {
            if (cap - n < 32768) {
                if (cap > SIZE_MAX / 2) { gzclose(gz); free(buf); return NULL; }
                cap *= 2;
                uint8_t *nb = realloc(buf, cap);
                if (!nb) { gzclose(gz); free(buf); return NULL; }
                buf = nb;
            }
            unsigned chunk = (cap - n) > (1u << 20) ? (1u << 20) : (unsigned)(cap - n);
            int got = gzread(gz, buf + n, chunk);
            if (got < 0) { gzclose(gz); free(buf); return NULL; }
            if (got == 0) break;
            n += (size_t)got;
        }
        if (gzclose(gz) != Z_OK) { free(buf); return NULL; }
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) { free(buf); return NULL; }
        for (;;) {
            if (cap - n < 32768) {
                if (cap > SIZE_MAX / 2) { fclose(f); free(buf); return NULL; }
                cap *= 2;
                uint8_t *nb = realloc(buf, cap);
                if (!nb) { fclose(f); free(buf); return NULL; }
                buf = nb;
            }
            size_t got = fread(buf + n, 1, cap - n, f);
            if (got) n += got;
            if (got < cap - n) {
                if (ferror(f)) { fclose(f); free(buf); return NULL; }
                break;
            }
        }
        fclose(f);
    }
    *out_n = n;
    return buf;
}

static char pg_low(uint8_t c){
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : (char)c;
}

static const uint8_t *pg_ci_find(const uint8_t *hay, size_t hn, const char *needle){
    size_t nn = strlen(needle);
    if (!nn) return hay;
    if (hn < nn) return NULL;
    char first = pg_low((uint8_t)needle[0]);
    for (size_t i = 0; i <= hn - nn; i++) {
        if (pg_low(hay[i]) != first) continue;
        size_t j = 1;
        while (j < nn && pg_low(hay[i + j]) == pg_low((uint8_t)needle[j])) j++;
        if (j == nn) return hay + i;
    }
    return NULL;
}

static void pg_next_line(const uint8_t *buf, size_t n, size_t pos,
                         size_t *ls, size_t *le, size_t *next_pos){
    *ls = pos;
    const uint8_t *nl = memchr(buf + pos, '\n', n - pos);
    if (!nl) {
        *le = n;
        *next_pos = n;
    } else {
        *le = (size_t)(nl - buf);
        *next_pos = *le + 1;
    }
    if (*le > *ls && buf[*le - 1] == '\r') (*le)--;
}

static void pg_trim_bounds(const uint8_t *buf, size_t *s, size_t *e){
    while (*s < *e && (buf[*s] == ' ' || buf[*s] == '\t' || buf[*s] == '\r')) (*s)++;
    while (*e > *s && (buf[*e - 1] == ' ' || buf[*e - 1] == '\t' || buf[*e - 1] == '\r')) (*e)--;
}

static int pg_line_has_stars(const uint8_t *buf, size_t s, size_t e){
    pg_trim_bounds(buf, &s, &e);
    return e - s >= 3 && buf[e - 3] == '*' && buf[e - 2] == '*' && buf[e - 1] == '*';
}

static int pg_is_blank(const uint8_t *buf, size_t ls, size_t le){
    for (size_t i = ls; i < le; i++) {
        if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r') return 0;
    }
    return 1;
}

static int pg_is_start_marker(const uint8_t *buf, size_t ls, size_t le){
    pg_trim_bounds(buf, &ls, &le);
    size_t len = le - ls;
    if (len < 16) return 0;
    const uint8_t *p = buf + ls;
    if (!(p[0] == '*' && p[1] == '*' && p[2] == '*')) return 0;
    const uint8_t *st = pg_ci_find(p + 3, len - 3, "start of");
    if (!st) return 0;
    return pg_ci_find(st, (size_t)(p + len - st), "project gutenberg") != NULL;
}

static int pg_is_end_marker(const uint8_t *buf, size_t ls, size_t le){
    pg_trim_bounds(buf, &ls, &le);
    size_t len = le - ls;
    const uint8_t *p = buf + ls;
    if (len >= 14 && p[0] == '*' && p[1] == '*' && p[2] == '*') {
        const uint8_t *en = pg_ci_find(p + 3, len - 3, "end of");
        if (en && pg_ci_find(en, (size_t)(p + len - en), "project gutenberg")) return 1;
    }
    if (len >= 24 && !strncasecmp((const char *)p, "end of project gutenberg", 24)) return 1;
    if (len >= 28 && !strncasecmp((const char *)p, "end of the project gutenberg", 28)) return 1;
    return 0;
}

static int pg_starts_with_ci(const uint8_t *buf, size_t ls, size_t le, const char *lit){
    pg_trim_bounds(buf, &ls, &le);
    size_t n = strlen(lit);
    return le - ls >= n && !strncasecmp((const char *)(buf + ls), lit, n);
}

static void pg_find_body_span(const uint8_t *buf, size_t n,
                              size_t *body_start_out, size_t *body_end_out,
                              size_t *meta_end_out, size_t *start_line_start_out,
                              size_t *start_line_end_out){
    size_t off0 = (n >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) ? 3 : 0;
    size_t scan_limit = off0 + 65536;
    if (scan_limit > n) scan_limit = n;

    size_t pos = off0;
    size_t body_start = off0;
    size_t meta_end = off0;
    size_t start_ls = 0, start_le = 0;
    int found_start = 0;
    int saw_latex_verbatim = 0;

    while (pos < scan_limit) {
        size_t ls, le, next_pos;
        pg_next_line(buf, n, pos, &ls, &le, &next_pos);
        if (pg_starts_with_ci(buf, ls, le, "\\begin{verbatim}")) saw_latex_verbatim = 1;
        if (pg_is_start_marker(buf, ls, le)) {
            found_start = 1;
            meta_end = ls;
            start_ls = ls;
            start_le = le;
            pos = next_pos;
            for (int wrap = 0; wrap < 4 && !pg_line_has_stars(buf, ls, le) && pos < scan_limit; wrap++) {
                pg_next_line(buf, n, pos, &ls, &le, &next_pos);
                if (pg_is_blank(buf, ls, le)) break;
                start_le = le;
                pos = next_pos;
            }
            if (saw_latex_verbatim && pos < n) {
                size_t tls, tle, tnext;
                pg_next_line(buf, n, pos, &tls, &tle, &tnext);
                if (pg_starts_with_ci(buf, tls, tle, "\\end{verbatim}")) {
                    pos = tnext;
                    if (pos < n) {
                        pg_next_line(buf, n, pos, &tls, &tle, &tnext);
                        if (pg_starts_with_ci(buf, tls, tle, "\\normalsize")) pos = tnext;
                    }
                }
            }
            body_start = pos;
            break;
        }
        pos = next_pos;
    }

    if (!found_start) {
        size_t head_limit = off0 + 8192;
        if (head_limit > n) head_limit = n;
        if (pg_ci_find(buf + off0, head_limit - off0, "this ebook is for the use of anyone")) {
            size_t p = off0;
            while (p < head_limit) {
                size_t ls, le, next_pos;
                pg_next_line(buf, n, p, &ls, &le, &next_pos);
                size_t ts = ls, te = le;
                pg_trim_bounds(buf, &ts, &te);
                if (te - ts >= 7 && !strncasecmp((const char *)(buf + ts), "credits", 7)) {
                    if (te - ts == 7 || buf[ts + 7] == ':') {
                        size_t after = next_pos;
                        if (te - ts == 7 && after < head_limit) {
                            size_t ls2, le2, next2;
                            pg_next_line(buf, n, after, &ls2, &le2, &next2);
                            size_t ts2 = ls2, te2 = le2;
                            pg_trim_bounds(buf, &ts2, &te2);
                            if (te2 > ts2 && buf[ts2] == ':') after = next2;
                        }
                        body_start = after;
                        meta_end = after;
                        break;
                    }
                }
                p = next_pos;
            }
        }
    }

    while (body_start < n) {
        size_t ls, le, next_pos;
        pg_next_line(buf, n, body_start, &ls, &le, &next_pos);
        if (!pg_is_blank(buf, ls, le)) break;
        body_start = next_pos;
    }

    size_t body_end = n;
    size_t tail_from = n > 131072 ? n - 131072 : body_start;
    if (tail_from < body_start) tail_from = body_start;
    if (tail_from > body_start) {
        while (tail_from < n && buf[tail_from - 1] != '\n') tail_from++;
    }

    int found_end = 0;
    size_t prev_ls = tail_from, prev_le = tail_from;
    size_t prev_prev_ls = tail_from, prev_prev_le = tail_from;
    pos = tail_from;
    while (pos < n) {
        size_t ls, le, next_pos;
        pg_next_line(buf, n, pos, &ls, &le, &next_pos);
        if (pg_is_end_marker(buf, ls, le)) {
            body_end = ls;
            if (prev_ls < ls && pg_starts_with_ci(buf, prev_ls, prev_le, "\\begin{verbatim}")) {
                body_end = prev_ls;
                if (prev_prev_ls < prev_ls && pg_starts_with_ci(buf, prev_prev_ls, prev_prev_le, "\\small")) {
                    body_end = prev_prev_ls;
                }
            }
            found_end = 1;
            break;
        }
        prev_prev_ls = prev_ls;
        prev_prev_le = prev_le;
        prev_ls = ls;
        prev_le = le;
        pos = next_pos;
    }

    if (!found_end) {
        pos = tail_from;
        while (pos < n) {
            size_t ls, le, next_pos;
            pg_next_line(buf, n, pos, &ls, &le, &next_pos);
            if (pg_starts_with_ci(buf, ls, le, "updated editions will replace the previous one")) {
                size_t probe = n - next_pos > 4096 ? 4096 : n - next_pos;
                if (pg_ci_find(buf + next_pos, probe, "START: FULL LICENSE")) {
                    body_end = ls;
                    break;
                }
            }
            pos = next_pos;
        }
    }

    if (body_end < body_start) body_end = body_start;
    while (body_end > body_start) {
        size_t nl = body_end;
        while (nl > body_start && buf[nl - 1] != '\n') nl--;
        size_t ls = nl, le = body_end;
        if (le > ls && buf[le - 1] == '\n') le--;
        if (le > ls && buf[le - 1] == '\r') le--;
        if (!pg_is_blank(buf, ls, le)) break;
        body_end = nl;
    }

    *body_start_out = body_start;
    *body_end_out = body_end;
    *meta_end_out = meta_end;
    *start_line_start_out = start_ls;
    *start_line_end_out = start_le;
}

static void pg_append_norm(char *dst, size_t cap, const uint8_t *src, size_t len, const char *sep){
    size_t cur = strlen(dst);
    char clean[512];
    size_t u = 0;
    int space = 0;
    for (size_t i = 0; i < len && u + 1 < sizeof clean; i++) {
        uint8_t c = src[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (u) space = 1;
            continue;
        }
        if (space && u + 1 < sizeof clean) clean[u++] = ' ';
        space = 0;
        if (u + 1 < sizeof clean) clean[u++] = (char)c;
    }
    clean[u] = 0;
    if (!u || cur + 1 >= cap) return;
    if (cur && sep && *sep) snprintf(dst + cur, cap - cur, "%s%s", sep, clean);
    else snprintf(dst + cur, cap - cur, "%s", clean);
}

static int pg_match_header_key(const uint8_t *buf, size_t ts, size_t te, const char *key, size_t *val_off){
    size_t kl = strlen(key);
    if (te - ts < kl || strncasecmp((const char *)(buf + ts), key, kl)) return 0;
    if (te - ts == kl) { *val_off = te; return 1; }
    if (buf[ts + kl] == ':') {
        size_t v = ts + kl + 1;
        while (v < te && (buf[v] == ' ' || buf[v] == '\t')) v++;
        *val_off = v;
        return 2;
    }
    return 0;
}

static int pg_is_any_header_label(const uint8_t *buf, size_t ts, size_t te){
    static const char *const LABELS[] = {
        "title", "subtitle", "author", "contributor", "translator", "illustrator",
        "release date", "most recently updated", "language", "character set encoding",
        "original publication", "other information", "credits", "posting date"
    };
    size_t z;
    for (size_t i = 0; i < sizeof LABELS / sizeof LABELS[0]; i++) {
        if (pg_match_header_key(buf, ts, te, LABELS[i], &z)) return 1;
    }
    return 0;
}

static void pg_extract_ebook_id(const char *src, char out[64]){
    if (out[0]) return;
    const uint8_t *p = pg_ci_find((const uint8_t *)src, strlen(src), "ebook");
    if (!p) return;
    p += 5;
    while (*p == ' ' || *p == '#' || *p == '\t') p++;
    size_t n = 0;
    while (p[n] >= '0' && p[n] <= '9' && n + 1 < 64) n++;
    if (n) { memcpy(out, p, n); out[n] = 0; }
}

static void pg_extract_metadata(const uint8_t *buf, size_t off0, size_t meta_end,
                                size_t start_ls, size_t start_le, PgMeta *meta){
    memset(meta, 0, sizeof *meta);
    static const struct { const char *name; int field; } KEYS[] = {
        {"title", 0}, {"author", 1}, {"language", 2}, {"release date", 3}
    };
    size_t pos = off0;
    while (pos < meta_end) {
        size_t ls, le, next_pos;
        pg_next_line(buf, meta_end, pos, &ls, &le, &next_pos);
        size_t ts = ls, te = le;
        pg_trim_bounds(buf, &ts, &te);
        pos = next_pos;
        if (te <= ts) continue;

        int matched = -1;
        size_t val_off = te;
        int mode = 0;
        for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) {
            int m = pg_match_header_key(buf, ts, te, KEYS[k].name, &val_off);
            if (m) { matched = KEYS[k].field; mode = m; break; }
        }
        if (matched < 0) continue;

        char *dst = matched == 0 ? meta->title : matched == 1 ? meta->author :
                    matched == 2 ? meta->language : meta->release_date;
        size_t cap = matched <= 1 ? sizeof meta->title : sizeof meta->language;
        if (mode == 2 && val_off < te) {
            pg_append_norm(dst, cap, buf + val_off, te - val_off, matched == 1 ? "; " : " ");
        }
        int saw_colon_line = (mode == 2);
        while (pos < meta_end) {
            size_t cls, cle, cnext;
            pg_next_line(buf, meta_end, pos, &cls, &cle, &cnext);
            size_t cts = cls, cte = cle;
            pg_trim_bounds(buf, &cts, &cte);
            if (cte <= cts || buf[cts] == '[' || pg_is_any_header_label(buf, cts, cte)) break;
            if (!saw_colon_line && buf[cts] == ':') {
                cts++;
                while (cts < cte && (buf[cts] == ' ' || buf[cts] == '\t')) cts++;
                saw_colon_line = 1;
                pg_append_norm(dst, cap, buf + cts, cte - cts, matched == 1 ? "; " : " ");
            } else if (saw_colon_line) {
                pg_append_norm(dst, cap, buf + cts, cte - cts, " ");
            } else break;
            pos = cnext;
        }
    }
    if (meta->release_date[0]) {
        pg_extract_ebook_id(meta->release_date, meta->ebook);
        char *br = strchr(meta->release_date, '[');
        if (br) {
            while (br > meta->release_date && br[-1] == ' ') br--;
            *br = 0;
        }
    }
    if (!meta->ebook[0] && start_le > start_ls) {
        size_t n = start_le - start_ls;
        char line[512];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, buf + start_ls, n);
        line[n] = 0;
        pg_extract_ebook_id(line, meta->ebook);
    }
}

static int pg_buf_append(uint8_t **buf, size_t *len, size_t *cap, const void *src, size_t n){
    if (!n) return 1;
    if (*len > SIZE_MAX - n) return 0;
    size_t need = *len + n;
    if (need > *cap) {
        size_t ncap = *cap ? *cap : 65536;
        while (ncap < need) {
            if (ncap > SIZE_MAX / 2) return 0;
            ncap *= 2;
        }
        uint8_t *nb = realloc(*buf, ncap);
        if (!nb) return 0;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, src, n);
    *len = need;
    return 1;
}

static int pg_append_tsv_norm(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *src, size_t n){
    size_t i = 0;
    while (i < n && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n')) i++;
    while (n > i && (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\r' || src[n - 1] == '\n')) n--;
    int space = 0;
    for (; i < n; i++) {
        uint8_t c = src[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            space = 1;
            continue;
        }
        if (space) {
            if (!pg_buf_append(buf, len, cap, " ", 1)) return 0;
            space = 0;
        }
        size_t start = i;
        while (i + 1 < n) {
            uint8_t nxt = src[i + 1];
            if (nxt == '\r' || nxt == '\n' || nxt == '\t' || nxt == ' ') break;
            i++;
        }
        if (!pg_buf_append(buf, len, cap, src + start, i - start + 1)) return 0;
    }
    return 1;
}

static void *pg_open_raw(const char *path){
    size_t n = 0;
    uint8_t *raw = pg_slurp(path, &n);
    if (!raw) return NULL;
    size_t bs = 0, be = 0, me = 0, sls = 0, sle = 0;
    pg_find_body_span(raw, n, &bs, &be, &me, &sls, &sle);
    PgStream *s = calloc(1, sizeof *s);
    if (!s) { free(raw); return NULL; }
    s->raw = raw;
    s->buf = raw + bs;
    s->len = be - bs;
    return s;
}

static void *pg_open_tsv(const char *path){
    size_t n = 0;
    uint8_t *raw = pg_slurp(path, &n);
    if (!raw) return NULL;
    size_t off0 = (n >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) ? 3 : 0;
    size_t bs = 0, be = 0, me = 0, sls = 0, sle = 0;
    pg_find_body_span(raw, n, &bs, &be, &me, &sls, &sle);
    PgMeta meta;
    pg_extract_metadata(raw, off0, me, sls, sle, &meta);

    uint8_t *out = NULL;
    size_t out_n = 0, out_cap = 0;
    static const char HEADER[] = "title\tauthor\tlanguage\trelease_date\tebook\tparagraph\n";
    if (!pg_buf_append(&out, &out_n, &out_cap, HEADER, sizeof HEADER - 1)) { free(raw); free(out); return NULL; }

    if (meta.title[0] || meta.author[0] || meta.language[0] || meta.release_date[0] || meta.ebook[0]) {
        int ok = pg_append_tsv_norm(&out, &out_n, &out_cap, (const uint8_t *)meta.title, strlen(meta.title)) &&
                 pg_buf_append(&out, &out_n, &out_cap, "\t", 1) &&
                 pg_append_tsv_norm(&out, &out_n, &out_cap, (const uint8_t *)meta.author, strlen(meta.author)) &&
                 pg_buf_append(&out, &out_n, &out_cap, "\t", 1) &&
                 pg_append_tsv_norm(&out, &out_n, &out_cap, (const uint8_t *)meta.language, strlen(meta.language)) &&
                 pg_buf_append(&out, &out_n, &out_cap, "\t", 1) &&
                 pg_append_tsv_norm(&out, &out_n, &out_cap, (const uint8_t *)meta.release_date, strlen(meta.release_date)) &&
                 pg_buf_append(&out, &out_n, &out_cap, "\t", 1) &&
                 pg_append_tsv_norm(&out, &out_n, &out_cap, (const uint8_t *)meta.ebook, strlen(meta.ebook)) &&
                 pg_buf_append(&out, &out_n, &out_cap, "\t\n", 2);
        if (!ok) { free(raw); free(out); return NULL; }
    }

    size_t pos = bs, para_start = bs;
    int in_para = 0;
    while (pos < be) {
        size_t ls, le, next_pos;
        pg_next_line(raw, be, pos, &ls, &le, &next_pos);
        if (pg_is_blank(raw, ls, le)) {
            if (in_para && ls > para_start) {
                if (!pg_buf_append(&out, &out_n, &out_cap, "\t\t\t\t\t", 5) ||
                    !pg_append_tsv_norm(&out, &out_n, &out_cap, raw + para_start, ls - para_start) ||
                    !pg_buf_append(&out, &out_n, &out_cap, "\n", 1)) {
                    free(raw); free(out); return NULL;
                }
            }
            in_para = 0;
        } else if (!in_para) {
            para_start = ls;
            in_para = 1;
        }
        pos = next_pos;
    }
    if (in_para && be > para_start) {
        if (!pg_buf_append(&out, &out_n, &out_cap, "\t\t\t\t\t", 5) ||
            !pg_append_tsv_norm(&out, &out_n, &out_cap, raw + para_start, be - para_start) ||
            !pg_buf_append(&out, &out_n, &out_cap, "\n", 1)) {
            free(raw); free(out); return NULL;
        }
    }
    free(raw);

    PgStream *s = calloc(1, sizeof *s);
    if (!s) { free(out); return NULL; }
    s->raw = out;
    s->buf = out;
    s->len = out_n;
    return s;
}

static size_t pg_read_chunk(void *ctx, uint8_t *out, size_t cap){
    PgStream *s = ctx;
    if (!s || s->err || s->pos >= s->len || !cap) return 0;
    size_t rem = s->len - s->pos;
    if (rem > cap) rem = cap;
    memcpy(out, s->buf + s->pos, rem);
    s->pos += rem;
    return rem;
}

static int pg_stream_error(void *ctx){
    const PgStream *s = ctx;
    return s ? s->err : 1;
}

static void pg_stream_close(void *ctx){
    PgStream *s = ctx;
    if (!s) return;
    free(s->raw);
    free(s);
}

const InputDecoder gutenberg_decoder = {
    .name = "gutenberg",
    .open = pg_open_raw,
    .read = pg_read_chunk,
    .error = pg_stream_error,
    .close = pg_stream_close
};

const InputDecoder gutenberg_tsv_decoder = {
    .name = "gutenberg-tsv",
    .open = pg_open_tsv,
    .read = pg_read_chunk,
    .error = pg_stream_error,
    .close = pg_stream_close
};

#endif
