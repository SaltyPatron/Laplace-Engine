/* A minimal JSON reader for configuration, tokenizer, and safetensors headers: the whole document is parsed into a flat
 * node array; objects and arrays keep their children as consecutive runs of node indices. */
#ifndef LP_JSON_MIN_H
#define LP_JSON_MIN_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;
typedef struct { jtype t; double num; char *str; uint32_t n, first; } jnode;     /* for objects: children alternate key, value */
typedef struct { jnode *v; uint32_t n, cap; uint32_t *kids; uint32_t nk, capk; const char *p, *end; int err; } jdoc;

static uint32_t j_new(jdoc *d, jtype t){
    if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 1024; d->v = realloc(d->v, d->cap * sizeof(jnode)); }
    memset(&d->v[d->n], 0, sizeof(jnode)); d->v[d->n].t = t; return d->n++;
}
static void j_ws(jdoc *d){ while (d->p < d->end && (*d->p == ' ' || *d->p == '\n' || *d->p == '\r' || *d->p == '\t')) d->p++; }
static uint32_t j_hex4(const char *s){ uint32_t v = 0; for (int i = 0; i < 4; i++) { char c = s[i]; v = v * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10); } return v; }
static char *j_string(jdoc *d){
    d->p++; const char *s = d->p; size_t cap = 16; char *o = malloc(cap); size_t n = 0;
    while (d->p < d->end && *d->p != '"') {
        if (n + 8 > cap) { cap *= 2; o = realloc(o, cap); }
        if (*d->p == '\\') {
            d->p++; char e = *d->p++;
            if (e == 'u') {
                uint32_t c = j_hex4(d->p); d->p += 4;
                if (c >= 0xD800 && c < 0xDC00 && d->p[0] == '\\' && d->p[1] == 'u') { uint32_t lo = j_hex4(d->p + 2); d->p += 6; c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); }
                n += lp_utf8_put(c, (uint8_t *)o + n);
            } else o[n++] = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
        } else o[n++] = *d->p++;
    }
    (void)s; d->p++; o[n] = 0; return o;
}
static uint32_t j_value(jdoc *d);
static uint32_t j_value(jdoc *d){
    j_ws(d); if (d->p >= d->end) { d->err = 1; return j_new(d, J_NULL); }
    char c = *d->p;
    if (c == '{' || c == '[') {
        int obj = c == '{'; uint32_t me = j_new(d, obj ? J_OBJ : J_ARR); d->p++;
        uint32_t *tmp = NULL; uint32_t tn = 0, tc = 0;
        for (;;) {
            j_ws(d); if (*d->p == (obj ? '}' : ']')) { d->p++; break; }
            if (obj) { uint32_t k = j_new(d, J_STR); d->v[k].str = j_string(d); j_ws(d); d->p++;
                       if (tn + 2 > tc) { tc = tc ? tc * 2 : 16; tmp = realloc(tmp, tc * 4); } tmp[tn++] = k; }
            uint32_t v = j_value(d);
            if (tn + 1 > tc) { tc = tc ? tc * 2 : 16; tmp = realloc(tmp, tc * 4); } tmp[tn++] = v;
            j_ws(d); if (*d->p == ',') d->p++;
        }
        if (d->nk + tn > d->capk) { d->capk = (d->nk + tn) * 2; d->kids = realloc(d->kids, d->capk * 4); }
        d->v[me].first = d->nk; d->v[me].n = tn; memcpy(d->kids + d->nk, tmp, tn * 4); d->nk += tn; free(tmp);
        return me;
    }
    if (c == '"') { uint32_t me = j_new(d, J_STR); d->v[me].str = j_string(d); return me; }
    if (c == 't' || c == 'f') { uint32_t me = j_new(d, J_BOOL); d->v[me].num = c == 't'; d->p += c == 't' ? 4 : 5; return me; }
    if (c == 'n') { d->p += 4; return j_new(d, J_NULL); }
    uint32_t me = j_new(d, J_NUM); char *e; d->v[me].num = strtod(d->p, &e); d->p = e; return me;
}
static jdoc j_parse(const char *s, size_t len){ jdoc d = { 0 }; d.p = s; d.end = s + len; j_value(&d); return d; }
/* Member of an object by key, or -1. */
static int64_t j_get(const jdoc *d, uint32_t obj, const char *key){
    const jnode *o = &d->v[obj]; if (o->t != J_OBJ) return -1;
    for (uint32_t i = 0; i < o->n; i += 2) if (!strcmp(d->v[d->kids[o->first + i]].str, key)) return d->kids[o->first + i + 1];
    return -1;
}
static double j_num(const jdoc *d, uint32_t obj, const char *key, double dflt){ int64_t k = j_get(d, obj, key); return k < 0 ? dflt : d->v[k].num; }
#endif
