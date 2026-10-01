/* JSON read as what it says, natively: one value, or a value on every line.
 *
 * The recipe names the members that say what an object is: identity KEY... (the first of them an object holds, with
 * a text or a number for its value, names the thing the object is). With "keys things", the keys of an object that
 * is inside nothing are themselves things, and each one's value speaks of it.
 * A claim is the path from a thing to a value, every key and value as written:
 *   [thing, key, value]   [thing, key, key, value]   ...   and, where the path ends at another thing, [thing, key, thing]
 * An array says each of its values under the same path. null, and an empty text, say nothing. A member the recipe
 * names as a key (key MEMBER...) is how the source points at its things, an identifier: it is never recorded.
 * An object can instead be a row (subject in KEY, predicate in KEY, object in KEY, score in KEY): those members are the
 * claim, the score the one the row gives it, and every other member is said of the claim itself.
 * Everything a top-level value says it says together: it is one record, the path of its claims and of the records of
 * the objects inside it, witnessed once, and its claims within it. */
#define _GNU_SOURCE
#include "engine.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { Ref *c; int n, cap; } Refs;
typedef struct { const Recipe *r; const uint8_t *p, *e; uint8_t *buf; size_t bcap; int bad; Refs claims; Ctx *ctx; const Ref *wpath; int nwp; Refs found; } JP;
#define MAXPATH 24

static void refs_push(Refs *a, Ref x){ if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 64; a->c = xrealloc(a->c, sizeof(Ref) * (size_t)a->cap); } a->c[a->n++] = x; }
static uint8_t tier_of(const Ref *r, int n){ uint8_t t = 0; for (int i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }
static void ws(JP *j){ while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static int hex4(const uint8_t *p){ int v = 0; for (int i = 0; i < 4; i++) { int c = p[i]; v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1000000); } return v; }
/* A string, as the text it is. The cursor is on its opening quote. */
static int string(JP *j, const uint8_t **s, size_t *n){
    const uint8_t *p = j->p + 1, *q = p; int plain = 1;
    while (q < j->e && *q != '"') { if (*q == '\\') { plain = 0; q++; } q++; }
    if (q >= j->e) { j->bad = 1; return 0; }
    if (plain) { *s = p; *n = (size_t)(q - p); j->p = q + 1; return 1; }
    size_t need = (size_t)(q - p) + 8; if (need > j->bcap) { j->bcap = need * 2; j->buf = xrealloc(j->buf, j->bcap); }
    size_t k = 0;
    for (const uint8_t *c = p; c < q; c++) {
        if (*c != '\\') { j->buf[k++] = *c; continue; }
        c++;
        switch (*c) {
            case 'n': j->buf[k++] = '\n'; break; case 't': j->buf[k++] = '\t'; break; case 'r': j->buf[k++] = '\r'; break;
            case 'b': j->buf[k++] = '\b'; break; case 'f': j->buf[k++] = '\f'; break;
            case 'u': { if (c + 4 >= q + 1) { j->bad = 1; return 0; } int v = hex4(c + 1); c += 4; if (v < 0) { j->bad = 1; return 0; }
                        if (v >= 0xD800 && v < 0xDC00 && c + 6 < q + 1 && c[1] == '\\' && c[2] == 'u') { int lo = hex4(c + 3); if (lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); c += 6; } }
                        k += lp_utf8_put((uint32_t)v, j->buf + k); break; }
            default: j->buf[k++] = *c;
        }
    }
    *s = j->buf; *n = k; j->p = q + 1; return 1;
}
static Ref text_of(JP *j, const uint8_t *s, size_t n){ if (j->r->path_sep && n > 1 && s[0] == (uint8_t)j->r->path_sep) return path_ref(s, n, j->r->path_sep, j->r->path_join); return n > 256 ? text_ref(j->ctx, s, n) : string_ref(s, n); }
static void skip(JP *j);
static void value(JP *j, Ref *path, int np, Refs *items);
/* A member that is a source's key: how it points at its things (an entry's number, a sense's id). Never recorded. */
static int is_key(const JP *j, const uint8_t *k, size_t kn){ for (int i = 0; i < j->r->nkey; i++) if (strlen(j->r->key[i]) == kn && !memcmp(j->r->key[i], k, kn)) return 1; return 0; }

/* What an object is: the thing the members the recipe names say it is. The cursor is on its opening brace, and
 * stays. Gives which identity named it (-2: the members that name it together), or -1 when nothing does. */
static int thing_of(JP *j, const Ref *path, int np, Ref *out){
    const Recipe *r = j->r; if (!r->nidentity && !r->nnamed) return -1;
    const uint8_t *save = j->p; int best = -1, holds = 0; Ref found = { 0 }, part[8]; uint8_t have[8] = { 0 };
    j->p++; ws(j);
    while (j->p < j->e && *j->p == '"' && !j->bad) {
        const uint8_t *k; size_t kn; if (!string(j, &k, &kn)) break;
        int which = -1, nm = -1;
        for (int i = 0; i < r->nidentity; i++) if (strlen(r->identity[i].attr) == kn && !memcmp(r->identity[i].attr, k, kn)) {
            const char *el = r->identity[i].el; if (el[0] && strcmp(el, "*")) { if (np < 1) continue; Ref u = string_ref((const uint8_t *)el, strlen(el)); if (memcmp(&u.id, &path[np - 1].id, 16)) continue; }
            which = i; break; }
        for (int i = 0; i < r->nnamed; i++) if (strlen(r->named[i]) == kn && !memcmp(r->named[i], k, kn)) { nm = i; break; }
        if (r->nnamed && strlen(r->named_key) == kn && !memcmp(r->named_key, k, kn)) holds = 1;
        ws(j); if (j->p < j->e && *j->p == ':') j->p++; ws(j);
        int wanted = (which >= 0 && (best < 0 || which < best)) || nm >= 0; Ref v = { 0 }; int got = 0;
        if (wanted && j->p < j->e && *j->p == '"') { const uint8_t *t; size_t tn; if (string(j, &t, &tn) && tn) { v = text_of(j, t, tn); got = 1; } }
        else if (wanted && j->p < j->e && ((*j->p >= '0' && *j->p <= '9') || *j->p == '-')) { const uint8_t *t = j->p; skip(j); v = string_ref(t, (size_t)(j->p - t)); got = 1; }
        else if (wanted && nm < 0 && j->p < j->e && *j->p == '[') {                 /* a list of texts names it together */
            const uint8_t *at = j->p; Ref t[8]; int nt = 0, plain = 1; j->p++; ws(j);
            while (j->p < j->e && *j->p == '"' && nt < 8) { const uint8_t *x; size_t xn; if (!string(j, &x, &xn)) { plain = 0; break; } if (xn) t[nt++] = text_of(j, x, xn); ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break; }
            if (plain && nt && j->p < j->e && *j->p == ']') { j->p++; v = nt == 1 ? t[0] : said_tuple(compose(t, (uint32_t)nt, tier_of(t, nt))); got = 1; }
            else { j->p = at; j->bad = 0; skip(j); } }
        else skip(j);
        if (got && nm >= 0) { part[nm] = v; have[nm] = 1; }
        if (got && which >= 0 && (best < 0 || which < best)) { found = v; best = which; }
        ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break;
    }
    j->p = save; j->bad = 0;
    if (holds) { Ref t[8]; int nt = 0; for (int i = 0; i < r->nnamed; i++) if (have[i]) { t[nt] = part[i]; t[nt].said = t[nt].said == LP_SAID_TUPLE ? LP_SAID_TUPLE : 0; nt++; }
        if (nt > 1) { *out = said_tuple(compose(t, (uint32_t)nt, tier_of(t, nt))); return -2; }
        if (nt == 1) { *out = t[0]; return -2; } }
    if (best < 0) return -1; *out = found; return best;
}
static void skip(JP *j){
    ws(j); if (j->p >= j->e) return;
    if (*j->p == '"') { const uint8_t *s; size_t n; string(j, &s, &n); return; }
    if (*j->p == '{' || *j->p == '[') { int depth = 0;
        while (j->p < j->e) { uint8_t c = *j->p;
            if (c == '"') { const uint8_t *s; size_t n; if (!string(j, &s, &n)) return; continue; }
            if (c == '{' || c == '[') depth++; else if (c == '}' || c == ']') { depth--; if (!depth) { j->p++; return; } }
            j->p++; }
        j->bad = 1; return; }
    while (j->p < j->e && *j->p != ',' && *j->p != '}' && *j->p != ']' && *j->p != ' ' && *j->p != '\n' && *j->p != '\r' && *j->p != '\t') j->p++;
}
static Ref say(JP *j, Ref *path, int np, Ref v, Refs *items){
    if (np < 1) return (Ref){ 0 };                                                       /* nothing it would be said of */
    if (j->nwp && np - 1 == j->nwp) { int same = 1; for (int i = 0; i < j->nwp && same; i++) same = !memcmp(&path[i + 1].id, &j->wpath[i].id, 16); if (same) refs_push(&j->found, v); }
    Ref t[MAXPATH + 2]; memcpy(t, path, sizeof(Ref) * (size_t)np); for (int i = 0; i < np; i++) if (t[i].said == LP_SAID_CLAIM) t[i].said = LP_SAID_TUPLE; t[np] = v;       /* a claim that is spoken of is a thing like any other */
    Ref c = said_claim(compose(t, (uint32_t)np + 1, tier_of(t, np + 1)));
    refs_push(&j->claims, c); if (items) refs_push(items, c);
    return c;
}
/* ---- specifics: what an object holds, each a pair of its key and its value */
static Ref tuple_refs(Ref *t, int n){ for (int i = 0; i < n; i++) if (t[i].said != LP_SAID_TUPLE) t[i].said = 0; return n == 1 ? t[0] : said_tuple(compose(t, (uint32_t)n, tier_of(t, n))); }
static Ref pair_of(Ref k, Ref v){ Ref t[2] = { k, v }; return tuple_refs(t, 2); }
static void pairs(JP *j, int by, Ref X, Ref *kp, int nk, Refs *out);
static void held_one(JP *j, Ref key, Ref *kp, int nk, Ref v, Refs *out){
    if (j->nwp && nk + 1 == j->nwp) { int same = !memcmp(&key.id, &j->wpath[nk].id, 16); for (int i = 0; i < nk && same; i++) same = !memcmp(&kp[i].id, &j->wpath[i].id, 16); if (same) refs_push(&j->found, v); }
    refs_push(out, pair_of(key, v));
}
static int plain_text(JP *j, Ref *v){                                       /* a text or a plain value at the cursor; 0 when it says nothing */
    if (*j->p == '"') { const uint8_t *s; size_t n; if (!string(j, &s, &n)) return 0;
        while (n && (s[0] == ' ' || s[0] == '\n' || s[0] == '\t')) { s++; n--; } while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
        if (!n) return 0; *v = text_of(j, s, n); return 1; }
    const uint8_t *at = j->p; skip(j); size_t n = (size_t)(j->p - at); if (!n) { j->bad = 1; return 0; }
    if (n == 4 && !memcmp(at, "null", 4)) return 0; *v = string_ref(at, n); return 1;
}
static void held(JP *j, Ref key, Ref *kp, int nk, Refs *out){
    ws(j); if (j->p >= j->e || j->bad) return;
    uint8_t c = *j->p;
    if (c == '{') { Ref one[1] = { key }, X = { 0 }; int by = thing_of(j, one, 1, &X); Refs mine = { 0 }; if (by != -1) refs_push(&mine, X);
        Ref kq[9]; int nq = 0; if (nk < 8) { memcpy(kq, kp, sizeof(Ref) * (size_t)nk); kq[nk] = key; nq = nk + 1; }
        pairs(j, by, X, kq, nq, &mine);
        if (mine.n) refs_push(out, pair_of(key, tuple_refs(mine.c, mine.n))); else if (!j->bad) refs_push(out, key);    /* it holds nothing: its key is what is said */
        free(mine.c); }
    else if (c == '[') { j->p++; ws(j);
        while (j->p < j->e && *j->p != ']' && !j->bad) {
            if (*j->p == '[' && j->r->tuples) { const uint8_t *at = j->p; Ref t[16]; int nt = 0, plain = 1; j->p++; ws(j);
                while (j->p < j->e && *j->p != ']' && nt < 16) { if (*j->p == '{' || *j->p == '[') { plain = 0; break; } Ref v; if (plain_text(j, &v)) t[nt++] = v; if (j->bad) { plain = 0; break; } ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break; }
                if (plain && j->p < j->e && *j->p == ']') { j->p++; if (nt) held_one(j, key, kp, nk, tuple_refs(t, nt), out); }
                else { j->p = at; j->bad = 0; int was = j->r->tuples; (void)was; j->p++; ws(j); while (j->p < j->e && *j->p != ']' && !j->bad) { held(j, key, kp, nk, out); ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break; } if (j->p < j->e && *j->p == ']') j->p++; else j->bad = 1; } }
            else held(j, key, kp, nk, out);
            ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break; }
        if (j->p < j->e && *j->p == ']') j->p++; else j->bad = 1; }
    else { Ref v; if (plain_text(j, &v)) held_one(j, key, kp, nk, v, out); }
}
/* The pairs an object holds. The cursor is on its opening brace. by, X: what named it, as thing_of gave them. */
static void pairs(JP *j, int by, Ref X, Ref *kp, int nk, Refs *out){
    j->p++; ws(j);
    while (j->p < j->e && *j->p == '"' && !j->bad) {
        const uint8_t *k; size_t kn; if (!string(j, &k, &kn)) break;
        Ref key = kn ? string_ref(k, kn) : (Ref){ 0 }; int is_id = 0;
        if (by >= 0) is_id = strlen(j->r->identity[by].attr) == kn && !memcmp(j->r->identity[by].attr, k, kn);
        else if (by == -2) for (int i = 0; i < j->r->nnamed && !is_id; i++) is_id = strlen(j->r->named[i]) == kn && !memcmp(j->r->named[i], k, kn);
        ws(j); if (j->p < j->e && *j->p == ':') j->p++; ws(j);
        if (!kn || is_key(j, k, kn) || (is_id && by >= 0 && j->p < j->e && *j->p != '{')) skip(j);           /* a key, or the member that names it: said already */
        else if (is_id && j->p < j->e && *j->p != '{' && *j->p != '[') { Ref v; if (plain_text(j, &v) && memcmp(&v.id, &X.id, 16)) held_one(j, key, kp, nk, v, out); }
        else held(j, key, kp, nk, out);
        ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break;
    }
    if (j->p < j->e && *j->p == '}') j->p++; else j->bad = 1;
}
static int claims_under(JP *j, const Ref *path, int np){
    for (int i = 0; np >= 1 && i < j->r->nclaims_under; i++) { Ref u = string_ref((const uint8_t *)j->r->claims_under[i], strlen(j->r->claims_under[i])); if (!memcmp(&u.id, &path[np - 1].id, 16)) return 1; }
    return 0;
}
static void object(JP *j, Ref *path, int np, Refs *items){
    Ref X; int by = thing_of(j, path, np, &X), thing = by != -1; Refs mine = { 0 };
    if (j->r->specifics && np >= 1 && !(thing && claims_under(j, path, np))) {
        if (thing) { Ref c = say(j, path, np, X, items); c.said = LP_SAID_TUPLE; refs_push(&mine, c); pairs(j, by, X, NULL, 0, &mine);
            if (mine.n > 1 && items && !j->bad) refs_push(items, said_record(compose(mine.c, (uint32_t)mine.n, tier_of(mine.c, mine.n)))); }
        else { pairs(j, -1, X, NULL, 0, &mine);
            if (mine.n && !j->bad) say(j, path, np, tuple_refs(mine.c, mine.n), items); else if (!j->bad && np >= 2) say(j, path, np - 1, path[np - 1], items); }
        free(mine.c); return;
    }
    Ref sub[MAXPATH + 2]; int ns;
    if (thing) { sub[0] = X; ns = 1; if (np >= 1) { Ref c = say(j, path, np, X, items); if (j->r->linkage) sub[0] = c; } }
    else { memcpy(sub, path, sizeof(Ref) * (size_t)np); ns = np; }
    int keys_things = !thing && np == 0 && j->r->keys_things;
    j->p++; ws(j);
    while (j->p < j->e && *j->p == '"' && !j->bad) {
        const uint8_t *k; size_t kn; if (!string(j, &k, &kn)) break;
        Ref key = kn ? string_ref(k, kn) : (Ref){ 0 }; int is_id = 0;
        if (by >= 0) is_id = strlen(j->r->identity[by].attr) == kn && !memcmp(j->r->identity[by].attr, k, kn);
        else if (by == -2) for (int i = 0; i < j->r->nnamed && !is_id; i++) is_id = strlen(j->r->named[i]) == kn && !memcmp(j->r->named[i], k, kn);
        ws(j); if (j->p < j->e && *j->p == ':') j->p++; ws(j);
        if (!kn || ns >= MAXPATH || is_key(j, k, kn)) skip(j);
        else if (keys_things) { Ref one[1] = { key }; value(j, one, 1, &mine); }
        else if (is_id && j->p < j->e && *j->p == '[') skip(j);                   /* the texts that name it together: said already */
        else if (is_id && j->p < j->e && *j->p != '{') {        /* the member that names it: said already, unless another names it first */
            const uint8_t *at = j->p; Ref v; int same = 0;
            if (*j->p == '"') { const uint8_t *s; size_t n; if (string(j, &s, &n) && n) { v = text_of(j, s, n); same = !memcmp(&v.id, &X.id, 16); if (!same) { sub[ns] = key; say(j, sub, ns + 1, v, &mine); } } }
            else { skip(j); v = string_ref(at, (size_t)(j->p - at)); same = !memcmp(&v.id, &X.id, 16); if (!same) { sub[ns] = key; say(j, sub, ns + 1, v, &mine); } }
        }
        else { sub[ns] = key; value(j, sub, ns + 1, &mine); }
        ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break;
    }
    if (j->p < j->e && *j->p == '}') j->p++; else j->bad = 1;
    if (!thing && !mine.n && np >= 2 && !j->bad) say(j, path, np - 1, path[np - 1], items);    /* it holds nothing: the path to it is what is said */
    if (items && mine.n) refs_push(items, mine.n == 1 ? mine.c[0] : said_record(compose(mine.c, (uint32_t)mine.n, tier_of(mine.c, mine.n))));
    free(mine.c);
}
static void value(JP *j, Ref *path, int np, Refs *items){
    ws(j); if (j->p >= j->e || j->bad) return;
    uint8_t c = *j->p;
    if (c == '{') object(j, path, np, items);
    else if (c == '[') { j->p++; ws(j);
        while (j->p < j->e && *j->p != ']' && !j->bad) {
            if (j->r->tuples && *j->p == '[') {                                     /* a list of plain values inside a list: one tuple */
                const uint8_t *at = j->p; Ref t[16]; int nt = 0, plain = 1; j->p++; ws(j);
                while (j->p < j->e && *j->p != ']' && nt < 16) {
                    if (*j->p == '"') { const uint8_t *x; size_t xn; if (!string(j, &x, &xn)) { plain = 0; break; } if (xn) t[nt++] = text_of(j, x, xn); }
                    else if (*j->p == '{' || *j->p == '[') { plain = 0; break; }
                    else { const uint8_t *x = j->p; skip(j); size_t xn = (size_t)(j->p - x); if (!xn) { plain = 0; break; } if (!(xn == 4 && !memcmp(x, "null", 4))) t[nt++] = string_ref(x, xn); }
                    ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break; }
                if (plain && j->p < j->e && *j->p == ']') { j->p++; if (nt) say(j, path, np, nt == 1 ? t[0] : said_tuple(compose(t, (uint32_t)nt, tier_of(t, nt))), items); }
                else { j->p = at; j->bad = 0; value(j, path, np, items); }
            }
            else value(j, path, np, items); ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break; }
        if (j->p < j->e && *j->p == ']') j->p++; else j->bad = 1; }
    else if (c == '"') { const uint8_t *s; size_t n; if (string(j, &s, &n) && n) { while (n && (s[0] == ' ' || s[0] == '\n' || s[0] == '\t')) { s++; n--; } while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--; if (n) say(j, path, np, text_of(j, s, n), items); } }
    else { const uint8_t *at = j->p; skip(j); size_t n = (size_t)(j->p - at); if (!n) { j->bad = 1; return; }
           if (!(n == 4 && !memcmp(at, "null", 4))) say(j, path, np, string_ref(at, n), items); }
}
/* One top-level value: a record, and its claims within it. */
static int record(JP *j, Ref *path, int np, float er, float ed, Events *ev){
    j->claims.n = 0; Refs items = { 0 };
    ws(j); if (np == 0 && (j->p >= j->e || (*j->p != '{' && *j->p != '['))) { j->bad = 1; return 0; }
    value(j, path, np, &items);
    int ok = !j->bad && j->claims.n > 0;
    if (ok) {
        Ref node = items.n == 1 ? items.c[0] : compose(items.c, (uint32_t)items.n, tier_of(items.c, items.n));
        if (j->claims.n == 1) { Event x = { j->claims.c[0].id, j->claims.c[0].id, 1.0f, er, ed, 0, EV_CLAIM }; ev_push(ev, &x); }
        else { Event x = { node.id, node.id, 1.0f, er, ed, 0, EV_RECORD }; ev_push(ev, &x);
               /* said twice in one record: witnessed in it once */
               uint64_t cap = 64; while (cap < (uint64_t)j->claims.n * 2) cap <<= 1; uint32_t *slot = calloc(cap, 4);
               for (int i = 0; i < j->claims.n; i++) { uint64_t h; memcpy(&h, j->claims.c[i].id.b, 8); uint64_t k = h & (cap - 1); int dup = 0;
                   while (slot[k]) { if (!memcmp(&j->claims.c[slot[k] - 1].id, &j->claims.c[i].id, 16)) { dup = 1; break; } k = (k + 1) & (cap - 1); }
                   if (dup) continue; slot[k] = (uint32_t)i + 1;
                   Event m = { j->claims.c[i].id, node.id, 1.0f, er, ed, 0, EV_MEMBER }; ev_push(ev, &m); }
               free(slot); }
    }
    free(items.c); return ok;
}
/* An object that is a row: three of its members (or two) are the claim, and its other members are said of the claim. */
static int member_text(JP *j, Ref *out){
    ws(j); if (j->p >= j->e) return 0;
    if (*j->p == '"') { const uint8_t *s; size_t n; if (!string(j, &s, &n) || !n) return 0; *out = text_of(j, s, n); return 1; }
    if (*j->p == '{' || *j->p == '[') { skip(j); return 0; }
    const uint8_t *at = j->p; skip(j); size_t n = (size_t)(j->p - at); if (!n || (n == 4 && !memcmp(at, "null", 4))) return 0; *out = string_ref(at, n); return 1;
}
static int row(JP *j, const Block *b, float er, float ed, Events *ev){
    j->claims.n = 0; ws(j); if (j->p >= j->e || *j->p != '{') { j->bad = 1; return 0; }
    const uint8_t *start = j->p; Ref part[3]; int have[3] = { 0, 0, 0 }; float score = 1.0f;
    j->p++; ws(j);
    while (j->p < j->e && *j->p == '"' && !j->bad) { const uint8_t *k; size_t kn; if (!string(j, &k, &kn)) break; char key[96]; snprintf(key, sizeof key, "%.*s", (int)(kn < 95 ? kn : 95), k);
        ws(j); if (j->p < j->e && *j->p == ':') j->p++; ws(j); int role = -1; for (int i = 0; i < 3; i++) if (b->in[i][0] && !strcmp(b->in[i], key)) role = i;
        if (role >= 0) have[role] = member_text(j, &part[role]);
        else if (b->in[5][0] && !strcmp(b->in[5], key)) { const uint8_t *at = j->p; skip(j); char z[32]; size_t n = (size_t)(j->p - at); if (n && n < 32) { memcpy(z, at, n); z[n] = 0; char *e; double v = strtod(z, &e); if (e != z && v >= 0.0 && v <= 1.0) score = (float)v; } }
        else skip(j);
        ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break; }
    if (j->bad || !have[0] || !have[2] || (!have[1] && !b->pair)) return 0;
    Ref tp[3] = { part[0], have[1] ? part[1] : part[2], part[2] }; int n = have[1] ? 3 : 2;
    Ref claim = said_claim(compose(tp, (uint32_t)n, tier_of(tp, n))); refs_push(&j->claims, claim);
    Refs items = { 0 }; refs_push(&items, claim);
    j->p = start + 1; ws(j);                                                  /* again, for what is said of the claim */
    while (j->p < j->e && *j->p == '"' && !j->bad) { const uint8_t *k; size_t kn; if (!string(j, &k, &kn)) break; char key[96]; snprintf(key, sizeof key, "%.*s", (int)(kn < 95 ? kn : 95), k); Ref kr = kn ? string_ref(k, kn) : (Ref){ 0 };
        ws(j); if (j->p < j->e && *j->p == ':') j->p++; ws(j); int is_part = 0; for (int i = 0; i < 3; i++) if (b->in[i][0] && !strcmp(b->in[i], key)) is_part = 1;
        if (is_part || !kn) skip(j); else { Ref path[2] = { claim, kr }; value(j, path, 2, &items); }
        ws(j); if (j->p < j->e && *j->p == ',') { j->p++; ws(j); } else break; }
    if (j->claims.n == 1) { Event x = { claim.id, claim.id, score, er, ed, 0, EV_CLAIM }; ev_push(ev, &x); }
    else { Ref node = said_record(compose(items.c, (uint32_t)items.n, tier_of(items.c, items.n)));
        Event x = { node.id, node.id, score, er, ed, 0, EV_RECORD }; ev_push(ev, &x);
        for (int i = 0; i < j->claims.n; i++) { int dup = 0; for (int y = 0; y < i && !dup; y++) dup = !memcmp(&j->claims.c[y].id, &j->claims.c[i].id, 16);
            if (!dup) { Event m = { j->claims.c[i].id, node.id, i ? 1.0f : score, er, ed, 0, EV_MEMBER }; ev_push(ev, &m); } } }
    free(items.c); return 1;
}
int json_said_of(const Recipe *r, Ctx *ctx, const uint8_t *p, size_t n, Ref thing, RefList *claims, const Ref *wpath, int nwpath, RefList *found){
    JP j = { r, p, p + n, NULL, 0, 0, { 0 }, ctx, wpath, nwpath, { 0 } }; Refs items = { 0 };
    ws(&j); if (j.p >= j.e || *j.p != '{') return 0;
    Ref path[1] = { thing };
    if (r->specifics) pairs(&j, -1, thing, NULL, 0, &j.claims); else value(&j, path, 1, &items);
    claims->c = j.claims.c; claims->n = j.claims.n; claims->cap = j.claims.cap; found->c = j.found.c; found->n = j.found.n; found->cap = j.found.cap;
    free(items.c); free(j.buf); return !j.bad;
}
void attest_members(const Recipe *r, File *f, const uint8_t *src, size_t n){
    const Block *b = NULL; for (int k = 0; k < r->nblock && !b; k++) if (!r->block[k].is_map) b = &r->block[k];
    float er = b ? b->enter_rating : 1500.0f, ed = b ? b->enter_deviation : 0.0f;
    /* a value on every line: stretches of lines on every core, their attestations joined in order */
    size_t ncut = r->records ? n / (4u << 20) + 1 : 1; size_t *cut = malloc(sizeof(size_t) * (ncut + 2)); size_t k = 1; cut[0] = 0;
    for (size_t i = 1; i < ncut; i++) { size_t c = n / ncut * i; while (c < n && src[c - 1] != '\n') c++; if (c > cut[k - 1] && c < n) cut[k++] = c; }
    cut[k] = n; Events *pe = calloc(k, sizeof(Events)); uint64_t bad = 0;
    #pragma omp taskloop grainsize(1) reduction(+:bad)
    for (size_t t = 0; t < k; t++) {
        JP j = { r, src + cut[t], src + cut[t + 1], NULL, 0, 0, { 0 }, CTX[omp_get_thread_num()], NULL, 0, { 0 } };
        int rows = b && b->in[0][0] && b->in[2][0];                              /* every value is a row: a claim, and what is said of it */
        if (!r->records && r->keys_things) {                                   /* every key a thing, and what its value says a record */
            ws(&j); if (j.p < j.e && *j.p == '{') { j.p++; ws(&j);
                while (j.p < j.e && *j.p == '"' && !j.bad) { const uint8_t *ks; size_t kn; if (!string(&j, &ks, &kn)) break; Ref key[1]; if (kn) key[0] = string_ref(ks, kn);
                    ws(&j); if (j.p < j.e && *j.p == ':') j.p++;
                    if (kn) record(&j, key, 1, er, ed, &pe[t]); else skip(&j);
                    ws(&j); if (j.p < j.e && *j.p == ',') { j.p++; ws(&j); } else break; } }
            if (j.bad) bad++;
        }
        else if (!r->records) { if (!(rows ? row(&j, b, er, ed, &pe[t]) : record(&j, NULL, 0, er, ed, &pe[t])) && j.bad) bad++; }
        else while (j.p < j.e) {
            const uint8_t *nl = memchr(j.p, '\n', (size_t)(j.e - j.p)), *end = nl ? nl : j.e, *whole = j.e;
            j.e = end; j.bad = 0; ws(&j); if (j.p < j.e) { if (!(rows ? row(&j, b, er, ed, &pe[t]) : record(&j, NULL, 0, er, ed, &pe[t])) && j.bad) bad++; }
            j.e = whole; j.p = nl ? nl + 1 : whole;
        }
        free(j.buf); free(j.claims.c);
    }
    uint32_t ordinal = (uint32_t)f->records;
    for (size_t t = 0; t < k; t++) { for (uint64_t i = 0; i < pe[t].n; i++) { Event x; memcpy(&x, &pe[t].e[i], sizeof x); if (x.kind != EV_MEMBER) x.position = ++ordinal; ev_push(&f->ev, &x); } free(pe[t].e); }
    f->records = ordinal;
    if (bad) { fprintf(stderr, "\n  %s: %llu values of %s are not JSON and say nothing\n", r->name, (unsigned long long)bad, f->path); f->incomplete += bad; }
    free(pe); free(cut);
}
