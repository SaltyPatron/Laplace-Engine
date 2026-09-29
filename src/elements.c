/* XML read as what it says, natively, for sources whose statements are their elements and attributes.
 *
 * The recipe names which elements are things, and by which attribute: identity ELEMENT ATTRIBUTE (* for any element
 * that carries the attribute). Everything else follows from how the source wrote it, every name and value as written:
 *   an element that is a thing X        [X, attribute, value] for each of its other attributes, [X, ELEMENT, text] for
 *                                       its own text, and, inside a thing S, [S, ELEMENT, X]: said together, one record.
 *                                       The things inside it are records of their own
 *   any other element, inside a thing S  it speaks of S: [S, attribute, value] for each attribute, [S, ELEMENT, text]
 *                                       for its text. What it says it says together: the element is one record, the
 *                                       path of its name, its claims, and the records of the elements inside it.
 *                                       An element that says nothing itself only holds the ones inside it
 *   link ELEMENT A B                    such an element is a relation: [S, value of A, value of B]
 * A record is witnessed once and its claims within it; a claim that stands alone is its own record. */
#define _GNU_SOURCE
#include "engine.h"
#include <tree_sitter/api.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t xml_unescape(const uint8_t *s, size_t n, uint8_t *o);
typedef struct { Ref *c; int n, cap; } Refs;
typedef struct { const Recipe *r; const uint8_t *src; float er, ed; } EW;

static void refs_push(Refs *a, Ref x){ if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->c = xrealloc(a->c, sizeof(Ref) * (size_t)a->cap); } a->c[a->n++] = x; }
static uint8_t tier_of(const Ref *r, int n){ uint8_t t = 0; for (int i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }
static int is(TSNode n, const char *t){ return !strcmp(ts_node_type(n), t); }
static int named(const EW *w, TSNode n, const char *s){ uint32_t a = ts_node_start_byte(n), b = ts_node_end_byte(n); size_t l = strlen(s); return b - a == l && !memcmp(w->src + a, s, l); }
static int listed(char (*list)[32], int n, const EW *w, TSNode name){ for (int i = 0; i < n; i++) if (named(w, name, list[i])) return 1; return 0; }

/* A value as the text it is: quotes off, references resolved; codepoints written in hex as the text they are. */
static int value_ref(const EW *w, TSNode val, int cps, long scp, Ref *out){
    const uint8_t *p = w->src + ts_node_start_byte(val); size_t n = ts_node_end_byte(val) - ts_node_start_byte(val);
    if (n >= 2 && (p[0] == '"' || p[0] == '\'') && p[n - 1] == p[0]) { p++; n -= 2; }
    while (n && (p[0] == ' ' || p[0] == '\t' || p[0] == '\n' || p[0] == '\r')) { p++; n--; }
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
    if (!n) return 0;
    uint8_t stack[1024], *buf = n * 2 + 16 <= sizeof stack ? stack : malloc(n * 2 + 16); int ok = 1;
    if (cps) {
        size_t k = 0; const uint8_t *c = p, *e = p + n;
        while (c < e && ok) {
            while (c < e && *c == ' ') c++; if (c >= e) break;
            const uint8_t *t0 = c; while (c < e && *c != ' ') c++; long cp;
            if (c - t0 == 1 && w->r->itself && *t0 == (uint8_t)w->r->itself) cp = scp;
            else { char h[16]; size_t o = (size_t)(c - t0); if (o > 8) { ok = 0; break; } memcpy(h, t0, o); h[o] = 0; char *end; cp = strtol(h, &end, 16); if (*end) ok = 0; }
            if (cp < 0 || cp >= (long)LP_NCP) ok = 0;
            if (ok) { if (k + 4 > n * 2 + 16) { ok = 0; break; } k += lp_utf8_put((uint32_t)cp, buf + k); }
        }
        if (ok && k) *out = string_ref(buf, k); else ok = 0;
    } else {
        size_t l = xml_unescape(p, n, buf);
        if (l && w->r->itself && scp >= 0 && memchr(buf, w->r->itself, l)) {          /* the codepoint itself, written out */
            char hex[16]; int hl = snprintf(hex, sizeof hex, "%04lX", scp); uint8_t *o = malloc(l * 8 + 8); size_t k = 0;
            for (size_t i = 0; i < l; i++) { if (buf[i] == (uint8_t)w->r->itself) { memcpy(o + k, hex, (size_t)hl); k += (size_t)hl; } else o[k++] = buf[i]; }
            *out = string_ref(o, k); free(o);
        }
        else if (l) *out = l > 256 ? text_ref(CTX[omp_get_thread_num()], buf, l) : string_ref(buf, l); else ok = 0;
    }
    if (buf != stack) free(buf);
    return ok;
}
static Ref claim3(Ref s, Ref p, Ref o){ Ref t[3] = { s, p, o }; return said_claim(compose(t, 3, tier_of(t, 3))); }
static void alone(const EW *w, Events *ev, Ref c){ Event x = { c.id, c.id, 1.0f, w->er, w->ed, 0, EV_CLAIM }; ev_push(ev, &x); }

static void element(const EW *w, TSNode el, const Ref *S, long scp, Refs *rec, Ref *node, int *has_node, Events *ev);
static __thread uint32_t skip_child = UINT32_MAX;                             /* the element whose text named the thing being read: said already */
/* The elements inside a node, each read; many of them, on every core, their attestations joined in order. */
static void inside(const EW *w, TSNode content, const Ref *S, long scp, Refs *rec, Refs *items, Events *ev){
    uint32_t nc = ts_node_child_count(content); if (!nc) return;
    TSNode *kid = malloc(sizeof(TSNode) * nc); uint32_t k = 0;
    TSTreeCursor cur = ts_tree_cursor_new(content);
    uint32_t skip = skip_child; skip_child = UINT32_MAX;
    if (ts_tree_cursor_goto_first_child(&cur)) do { TSNode c = ts_tree_cursor_current_node(&cur); if (is(c, "element") && ts_node_start_byte(c) != skip) kid[k++] = c; } while (ts_tree_cursor_goto_next_sibling(&cur));
    ts_tree_cursor_delete(&cur);
    if (!rec && k >= 256 && ts_node_end_byte(content) - ts_node_start_byte(content) > (1u << 20)) {
        int nt = omp_get_num_threads() * 8; if (nt > (int)k) nt = (int)k; Events *pe = calloc((size_t)nt, sizeof(Events));
        #pragma omp taskloop grainsize(1)
        for (int t = 0; t < nt; t++)
            for (uint32_t i = (uint32_t)((uint64_t)k * t / nt); i < (uint32_t)((uint64_t)k * (t + 1) / nt); i++) { Ref n_; int h_; element(w, kid[i], S, scp, NULL, &n_, &h_, &pe[t]); }
        for (int t = 0; t < nt; t++) { for (uint64_t j = 0; j < pe[t].n; j++) ev_push(ev, &pe[t].e[j]); free(pe[t].e); }
        free(pe); free(kid); return;
    }
    for (uint32_t i = 0; i < k; i++) { Ref n_; int h_ = 0; element(w, kid[i], S, scp, rec, &n_, &h_, ev); if (h_ && items) refs_push(items, n_); }
    free(kid);
}
static void element(const EW *w, TSNode el, const Ref *S, long scp, Refs *rec, Ref *node, int *has_node, Events *ev){
    const Recipe *r = w->r; *has_node = 0;
    TSNode tag = ts_node_child(el, 0), content = { 0 }; int has_content = 0;
    if (ts_node_is_null(tag) || (!is(tag, "STag") && !is(tag, "EmptyElemTag"))) return;
    { uint32_t nc = ts_node_child_count(el); for (uint32_t i = 1; i < nc; i++) { TSNode c = ts_node_child(el, i); if (is(c, "content")) { content = c; has_content = 1; break; } } }
    TSNode name = { 0 }, an[256], av[256]; int na = 0, has_name = 0;
    { uint32_t nc = ts_node_named_child_count(tag);
      for (uint32_t i = 0; i < nc; i++) { TSNode c = ts_node_named_child(tag, i);
          if (is(c, "Name") && !has_name) { name = c; has_name = 1; }
          else if (is(c, "Attribute") && na < 256 && ts_node_named_child_count(c) >= 2) { an[na] = ts_node_named_child(c, 0); av[na] = ts_node_named_child(c, 1); na++; } } }
    if (!has_name) return;
    Ref nref = string_ref(w->src + ts_node_start_byte(name), ts_node_end_byte(name) - ts_node_start_byte(name));

    /* is it a thing, and what names it: an attribute, or the text of an element inside it */
    int idat = -1, idres = 0, idwithin = 0, have_x = 0; Ref X; long xcp = -1; TSNode idchild = { 0 }; int has_idchild = 0;
    for (int i = 0; i < r->nidentity && !have_x; i++) {
        if (strcmp(r->identity[i].el, "*") && !named(w, name, r->identity[i].el)) continue;
        if (r->identity[i].child) {
            if (!has_content) continue; uint32_t nc = ts_node_child_count(content);
            for (uint32_t c = 0; c < nc && !have_x; c++) { TSNode ch = ts_node_child(content, c); if (!is(ch, "element")) continue;
                TSNode t2 = ts_node_child(ch, 0); if (ts_node_is_null(t2) || !is(t2, "STag")) continue; TSNode n2 = ts_node_named_child(t2, 0);
                if (ts_node_is_null(n2) || !named(w, n2, r->identity[i].attr)) continue;
                uint32_t n3 = ts_node_child_count(ch); for (uint32_t z = 1; z < n3; z++) { TSNode cc = ts_node_child(ch, z); if (is(cc, "content") && value_ref(w, cc, 0, scp, &X)) { have_x = 1; idchild = ch; has_idchild = 1; idwithin = r->identity[i].within; break; } } }
            continue;
        }
        for (int a = 0; a < na && !have_x; a++) if (named(w, an[a], r->identity[i].attr)) {
            idres = r->identity[i].res; int ok;
            if (idres == 1) { const uint8_t *p = w->src + ts_node_start_byte(av[a]); size_t n = ts_node_end_byte(av[a]) - ts_node_start_byte(av[a]);
                if (n >= 2 && (p[0] == '"' || p[0] == '\'')) { p++; n -= 2; } char h[16]; ok = n && n <= 8; if (ok) { memcpy(h, p, n); h[n] = 0; char *e; unsigned long cp = strtoul(h, &e, 16); ok = !*e && cp < LP_NCP; if (ok) { X = atom((uint32_t)cp); xcp = (long)cp; } } }
            else ok = value_ref(w, av[a], idres == 2, scp, &X);
            if (ok) { have_x = 1; idat = a; idwithin = r->identity[i].within; }
        }
    }
    if (have_x && idwithin && S) { Ref in[2] = { *S, X }; in[0].said = 0; X = said_tuple(compose(in, 2, tier_of(in, 2))); xcp = -1; }     /* a name that stands only within what it is inside */
    if (have_x) {
        /* what it says itself it says together: its place in what it is inside, its attributes, its own text */
        Refs own = { 0 };
        if (S) refs_push(&own, claim3(*S, nref, X));
        for (int a = 0; a < na; a++) { if (a == idat) continue;
            Ref key = string_ref(w->src + ts_node_start_byte(an[a]), ts_node_end_byte(an[a]) - ts_node_start_byte(an[a]));
            char ls = 0; for (int z = 0; z < r->nlist; z++) if (named(w, an[a], r->list[z].col)) ls = r->list[z].sep;
            if (ls) { const uint8_t *p = w->src + ts_node_start_byte(av[a]), *e = w->src + ts_node_end_byte(av[a]); if (e - p >= 2 && (p[0] == '"' || p[0] == '\'')) { p++; e--; }
                while (p < e) { const uint8_t *q = memchr(p, ls, (size_t)(e - p)); if (!q) q = e; const uint8_t *vp = p; size_t vn = (size_t)(q - p);
                    while (vn && (vp[0] == ' ' || vp[0] == '\n' || vp[0] == '\t')) { vp++; vn--; } while (vn && (vp[vn - 1] == ' ' || vp[vn - 1] == '\n' || vp[vn - 1] == '\t')) vn--;
                    if (vn) { uint8_t *ub = malloc(vn * 2 + 16); size_t ul = xml_unescape(vp, vn, ub); if (ul) refs_push(&own, claim3(X, key, string_ref(ub, ul))); free(ub); }
                    p = q + 1; }
                continue; }
            Ref v; if (!value_ref(w, av[a], listed((char (*)[32])r->codepoints, r->ncodepoints, w, an[a]), xcp, &v)) continue;
            if (!memcmp(&v.id, &X.id, 16)) continue;                            /* what it says of itself is itself: nothing further */
            refs_push(&own, claim3(X, key, v)); }
        if (has_content) { int kids = 0; uint32_t nc = ts_node_child_count(content); for (uint32_t i = 0; i < nc && !kids; i++) kids = is(ts_node_child(content, i), "element");
            if (!kids) { Ref v; if (value_ref(w, content, 0, xcp, &v) && memcmp(&v.id, &X.id, 16)) refs_push(&own, claim3(X, nref, v)); } }
        if (rec) { for (int i = 0; i < own.n; i++) refs_push(rec, own.c[i]); }
        else if (own.n == 1) alone(w, ev, own.c[0]);
        else if (own.n > 1) {
            Ref *path = malloc(sizeof(Ref) * (size_t)(own.n + 1)); path[0] = nref; memcpy(path + 1, own.c, sizeof(Ref) * (size_t)own.n);
            Ref node_ = said_record(compose(path, (uint32_t)own.n + 1, tier_of(path, own.n + 1))); free(path);
            Event x = { node_.id, node_.id, 1.0f, w->er, w->ed, 0, EV_RECORD }; ev_push(ev, &x);
            for (int i = 0; i < own.n; i++) { int dup = 0; for (int j = 0; j < i && !dup; j++) dup = !memcmp(&own.c[j].id, &own.c[i].id, 16);
                if (!dup) { Event m = { own.c[i].id, node_.id, 1.0f, w->er, w->ed, 0, EV_MEMBER }; ev_push(ev, &m); } }
        }
        free(own.c);
        if (has_content) { skip_child = has_idchild ? ts_node_start_byte(idchild) : UINT32_MAX; inside(w, content, &X, xcp, NULL, NULL, ev); skip_child = UINT32_MAX; }
        *node = X; *has_node = 1; return;
    }
    if (!S) { if (has_content) inside(w, content, NULL, -1, NULL, NULL, ev); return; }   /* it speaks of nothing: what is inside it may */
    for (int i = 0; i < r->nlink; i++) if (named(w, name, r->link[i].el)) {              /* a relation of S */
        int pa = -1, oa = -1; for (int a = 0; a < na; a++) { if (named(w, an[a], r->link[i].pred)) pa = a; if (named(w, an[a], r->link[i].obj)) oa = a; }
        Ref p, o; if (pa < 0 || oa < 0 || !value_ref(w, av[pa], 0, scp, &p) || !value_ref(w, av[oa], 0, scp, &o)) return;
        Ref c = claim3(*S, p, o); if (rec) refs_push(rec, c); else alone(w, ev, c);
        *node = c; *has_node = 1; return;
    }
    if (!rec && !na && has_content) {                                           /* it says nothing itself: what is inside it speaks for itself */
        int kids = 0; uint32_t nc = ts_node_child_count(content); for (uint32_t i = 0; i < nc && !kids; i++) kids = is(ts_node_child(content, i), "element");
        if (kids) { inside(w, content, S, scp, NULL, NULL, ev); return; }
    }
    Refs mine = { 0 }, items = { 0 }, *claims = rec ? rec : &mine; int before = claims->n;
    for (int a = 0; a < na; a++) { Ref v; Ref key = string_ref(w->src + ts_node_start_byte(an[a]), ts_node_end_byte(an[a]) - ts_node_start_byte(an[a]));
        char ls = 0; for (int z = 0; z < r->nlist; z++) if (named(w, an[a], r->list[z].col)) ls = r->list[z].sep;
        if (ls) { const uint8_t *p = w->src + ts_node_start_byte(av[a]), *e = w->src + ts_node_end_byte(av[a]); if (e - p >= 2 && (p[0] == '"' || p[0] == '\'')) { p++; e--; }
            while (p < e) { const uint8_t *q = memchr(p, ls, (size_t)(e - p)); if (!q) q = e; const uint8_t *vp = p; size_t vn = (size_t)(q - p);
                while (vn && (vp[0] == ' ' || vp[0] == '\n' || vp[0] == '\t')) { vp++; vn--; } while (vn && (vp[vn - 1] == ' ' || vp[vn - 1] == '\n' || vp[vn - 1] == '\t')) vn--;
                if (vn) { uint8_t *ub = malloc(vn * 2 + 16); size_t ul = xml_unescape(vp, vn, ub); if (ul) { Ref c = claim3(*S, key, string_ref(ub, ul)); refs_push(claims, c); refs_push(&items, c); } free(ub); }
                p = q + 1; }
            continue; }
        if (!value_ref(w, av[a], listed((char (*)[32])r->codepoints, r->ncodepoints, w, an[a]), scp, &v)) continue;
        Ref c = claim3(*S, key, v);
        refs_push(claims, c); refs_push(&items, c); }
    if (has_content) {
        int kids = 0; uint32_t nc = ts_node_child_count(content); for (uint32_t i = 0; i < nc && !kids; i++) kids = is(ts_node_child(content, i), "element");
        if (!kids) { Ref v; if (value_ref(w, content, 0, scp, &v)) { Ref c = claim3(*S, nref, v); refs_push(claims, c); refs_push(&items, c); } }
        else inside(w, content, S, scp, claims, &items, ev);
    }
    if (items.n == 1) { *node = items.c[0]; *has_node = 1; }
    else if (items.n > 1) {
        Ref *path = malloc(sizeof(Ref) * (size_t)(items.n + 1)); path[0] = nref; memcpy(path + 1, items.c, sizeof(Ref) * (size_t)items.n);
        *node = said_record(compose(path, (uint32_t)items.n + 1, tier_of(path, items.n + 1))); *has_node = 1; free(path);
    }
    if (!rec && *has_node) {                                                     /* the record, and its claims within it */
        if (claims->n - before == 1 && items.n == 1) alone(w, ev, claims->c[before]);
        else { Event x = { node->id, node->id, 1.0f, w->er, w->ed, 0, EV_RECORD }; ev_push(ev, &x);
               for (int i = before; i < claims->n; i++) { int dup = 0; for (int j = before; j < i && !dup; j++) dup = !memcmp(&claims->c[j].id, &claims->c[i].id, 16);
                   if (!dup) { Event m = { claims->c[i].id, node->id, 1.0f, w->er, w->ed, 0, EV_MEMBER }; ev_push(ev, &m); } } }
    }
    free(mine.c); free(items.c);
}
void attest_elements(const Recipe *r, File *f, void *root_node, const uint8_t *src, size_t n){
    (void)n; EW w = { r, src, 1500.0f, 0.0f }; for (int k = 0; k < r->nblock; k++) if (!r->block[k].is_map) { w.er = r->block[k].enter_rating; w.ed = r->block[k].enter_deviation; break; }
    inside(&w, *(TSNode *)root_node, NULL, -1, NULL, NULL, &f->ev);
}
