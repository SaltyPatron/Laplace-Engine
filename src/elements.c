/* XML read as what it says, natively, for sources whose statements are their elements and attributes.
 *
 * The recipe names which elements are things, and what names each:
 *   identity ELEMENT ATTRIBUTE          the thing is what the attribute names (* : any element that carries it)
 *   identity ELEMENT FIRST..LAST        the thing is a range of codepoints, written in hex as its first and its last:
 *                                       the path of the two. What is said of a range is said of the range
 *   identity ELEMENT >CHILD             the thing is what the text of the element CHILD inside it names;
 *                                       >CHILD.ATTRIBUTE: what that element's attribute names
 *   identity ELEMENT NAME within        the name stands only within the thing the element is inside: the thing is the
 *                                       path of that thing and the name
 *   identity ELEMENT NAME kind          the name stands only among elements of its kind (a source that numbers each
 *                                       kind from one): the thing is the path of the element's name and the name
 *   refer ATTRIBUTE KIND                the attribute's value names a thing of that kind: it is recorded as that thing
 * Everything else follows from how the source wrote it, every name and value as written:
 *   an element that is a thing X        [X, attribute, value] for each of its other attributes, [X, ELEMENT, text] for
 *                                       its own text, and, inside a thing S, [S, ELEMENT, X]: said together, one record.
 *                                       The things inside it are records of their own
 *   any other element, inside a thing S  it speaks of S: [S, attribute, value] for each attribute, [S, ELEMENT, text]
 *                                       for its text. What it says it says together: the element is one record, the
 *                                       path of its name, its claims, and the records of the elements inside it.
 *                                       One that says nothing itself and holds things is the record of their places
 *                                       in S; one that says nothing itself and holds no things only holds
 *   link ELEMENT A B                    such an element is a relation: [S, value of A, value of B]; B written >CHILD
 *                                       is the text of each element CHILD inside it
 *   span ELEMENT START END TEXT [inclusive]
 *                                       such an element speaks of a stretch of text: of the text of the element TEXT
 *                                       inside the thing it is in, from the character START to the character END.
 *                                       What it says is said of that stretch, which is content like any other; START
 *                                       and END are where it is, not what is said. TEXT is recorded exactly as written
 *   list ATTRIBUTE CHAR                 an attribute whose value is several values
 *   codepoints ATTRIBUTE...             attributes whose values are codepoints written in hex
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
typedef struct { const Recipe *r; const uint8_t *src; float er, ed; char dir[256]; } EW;
static Ref kind_of(const EW *w, const char *kind){ const char *k = !strcmp(kind, "{dir}") ? w->dir : kind; return string_ref((const uint8_t *)k, strlen(k)); }
typedef struct { uint8_t *p; size_t n; } Txt;                                 /* the text a span speaks of, as written */
typedef struct { TSNode name, an[256], av[256], content; int na, has_content; } Tag;

static void refs_push(Refs *a, Ref x){ if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->c = xrealloc(a->c, sizeof(Ref) * (size_t)a->cap); } a->c[a->n++] = x; }
static uint8_t tier_of(const Ref *r, int n){ uint8_t t = 0; for (int i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }
static int is(TSNode n, const char *t){ return !strcmp(ts_node_type(n), t); }
static int named(const EW *w, TSNode n, const char *s){ uint32_t a = ts_node_start_byte(n), b = ts_node_end_byte(n); size_t l = strlen(s); return b - a == l && !memcmp(w->src + a, s, l); }
static int listed(char (*list)[32], int n, const EW *w, TSNode name){ for (int i = 0; i < n; i++) if (named(w, name, list[i])) return 1; return 0; }
static Ref text_of(const uint8_t *s, size_t n){ return n > 256 ? text_ref(CTX[omp_get_thread_num()], s, n) : string_ref(s, n); }
static Ref name_of(const EW *w, TSNode n){ return string_ref(w->src + ts_node_start_byte(n), ts_node_end_byte(n) - ts_node_start_byte(n)); }
static Ref pair_of(Ref a, Ref b){ Ref t[2] = { a, b }; t[0].said = t[1].said = 0; return said_tuple(compose(t, 2, tier_of(t, 2))); }

static int tag_of(TSNode el, Tag *t){
    t->na = 0; t->has_content = 0; int has_name = 0, has_tag = 0; TSNode tag = { 0 };
    TSTreeCursor cur = ts_tree_cursor_new(el);                               /* children by cursor: asking for the i-th child costs i */
    if (ts_tree_cursor_goto_first_child(&cur)) {
        tag = ts_tree_cursor_current_node(&cur); has_tag = is(tag, "STag") || is(tag, "EmptyElemTag");
        if (has_tag) while (ts_tree_cursor_goto_next_sibling(&cur)) { TSNode c = ts_tree_cursor_current_node(&cur); if (is(c, "content")) { t->content = c; t->has_content = 1; break; } }
    }
    if (has_tag) { ts_tree_cursor_reset(&cur, tag);
        if (ts_tree_cursor_goto_first_child(&cur)) do { TSNode c = ts_tree_cursor_current_node(&cur);
            if (is(c, "Name") && !has_name) { t->name = c; has_name = 1; }
            else if (is(c, "Attribute") && t->na < 256) { TSNode n0 = ts_node_named_child(c, 0), n1 = ts_node_named_child(c, 1); if (!ts_node_is_null(n0) && !ts_node_is_null(n1)) { t->an[t->na] = n0; t->av[t->na] = n1; t->na++; } }
        } while (ts_tree_cursor_goto_next_sibling(&cur)); }
    ts_tree_cursor_delete(&cur);
    return has_tag && has_name;
}
static int holds_elements(TSNode content){
    TSTreeCursor cur = ts_tree_cursor_new(content); int yes = 0;
    if (ts_tree_cursor_goto_first_child(&cur)) do yes = is(ts_tree_cursor_current_node(&cur), "element"); while (!yes && ts_tree_cursor_goto_next_sibling(&cur));
    ts_tree_cursor_delete(&cur); return yes;
}
/* A value's bytes: quotes off; trimmed unless it is wanted exactly as written. */
static int raw_of(const EW *w, TSNode val, int exact, const uint8_t **p, size_t *n){
    *p = w->src + ts_node_start_byte(val); *n = ts_node_end_byte(val) - ts_node_start_byte(val);
    if (*n >= 2 && ((*p)[0] == '"' || (*p)[0] == '\'') && (*p)[*n - 1] == (*p)[0]) { (*p)++; *n -= 2; }
    const uint8_t *q = *p; size_t m = *n;
    while (m && (q[0] == ' ' || q[0] == '\t' || q[0] == '\n' || q[0] == '\r')) { q++; m--; }
    while (m && (q[m - 1] == ' ' || q[m - 1] == '\t' || q[m - 1] == '\n' || q[m - 1] == '\r')) m--;
    if (!m) return 0;
    if (!exact) { *p = q; *n = m; }
    return 1;
}
/* A value as the text it is: references resolved; codepoints written in hex as the text they are. */
static int value_ref(const EW *w, TSNode val, int cps, int exact, long scp, Ref *out){
    const uint8_t *p; size_t n; if (!raw_of(w, val, exact, &p, &n)) return 0;
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
        else if (l) *out = text_of(buf, l); else ok = 0;
    }
    if (buf != stack) free(buf);
    return ok;
}
static Ref claim3(Ref s, Ref p, Ref o){ Ref t[3] = { s, p, o }; return said_claim(compose(t, 3, tier_of(t, 3))); }
static void alone(const EW *w, Events *ev, Ref c){ Event x = { c.id, c.id, 1.0f, w->er, w->ed, 0, EV_CLAIM }; ev_push(ev, &x); }
static void record_of(const EW *w, Events *ev, Ref node, const Ref *claim, int n){
    Event x = { node.id, node.id, 1.0f, w->er, w->ed, 0, EV_RECORD }; ev_push(ev, &x);
    for (int i = 0; i < n; i++) { int dup = 0; for (int j = 0; j < i && !dup; j++) dup = !memcmp(&claim[j].id, &claim[i].id, 16);
        if (!dup) { Event m = { claim[i].id, node.id, 1.0f, w->er, w->ed, 0, EV_MEMBER }; ev_push(ev, &m); } }
}

/* What an element is, if it is a thing. */
typedef struct { Ref X; long cp; int attr, attr2; uint32_t child; } Thing;
static int thing_of(const EW *w, const Tag *t, const Ref *S, long scp, Thing *th){
    const Recipe *r = w->r; th->cp = -1; th->attr = th->attr2 = -1; th->child = UINT32_MAX;
    for (int i = 0; i < r->nidentity; i++) {
        if (strcmp(r->identity[i].el, "*") && !named(w, t->name, r->identity[i].el)) continue;
        int found = 0;
        if (r->identity[i].own) found = t->has_content && !holds_elements(t->content) && value_ref(w, t->content, 0, 0, scp, &th->X), th->attr = found ? -2 : -1;
        else if (r->identity[i].child) {
            if (!t->has_content) continue; uint32_t nc = ts_node_child_count(t->content);
            char cn[64]; snprintf(cn, sizeof cn, "%s", r->identity[i].attr); char *ca = strchr(cn, '.'); if (ca) *ca++ = 0;       /* CHILD, or CHILD.ATTRIBUTE */
            TSTreeCursor cur = ts_tree_cursor_new(t->content);
            if (ts_tree_cursor_goto_first_child(&cur)) do { TSNode ch = ts_tree_cursor_current_node(&cur); Tag ct; if (!is(ch, "element") || !tag_of(ch, &ct) || !named(w, ct.name, cn)) continue;
                if (ca) { for (int a = 0; a < ct.na && !found; a++) if (named(w, ct.an[a], ca)) found = value_ref(w, ct.av[a], 0, 0, scp, &th->X); }
                else if (ct.has_content && !holds_elements(ct.content) && value_ref(w, ct.content, 0, 0, scp, &th->X)) { found = 1; th->child = ts_node_start_byte(ch); }
            } while (!found && ts_tree_cursor_goto_next_sibling(&cur));
            ts_tree_cursor_delete(&cur); (void)nc;
        }
        else if (strstr(r->identity[i].attr, "..")) {                          /* a range of codepoints, written as its first and its last: the path of the two */
            char a1[64], *a2; snprintf(a1, sizeof a1, "%s", r->identity[i].attr); a2 = strstr(a1, ".."); *a2 = 0; a2 += 2; long lo = -1, hi = -1;
            for (int a = 0; a < t->na; a++) { int which = named(w, t->an[a], a1) ? 1 : named(w, t->an[a], a2) ? 2 : 0; const uint8_t *p; size_t n; char h[16];
                if (which && raw_of(w, t->av[a], 0, &p, &n) && n <= 8) { memcpy(h, p, n); h[n] = 0; char *e; unsigned long cp = strtoul(h, &e, 16); if (!*e && cp < LP_NCP) { if (which == 1) { lo = (long)cp; th->attr = a; } else { hi = (long)cp; th->attr2 = a; } } } }
            if (lo >= 0 && hi >= 0) { Ref two[2] = { atom((uint32_t)lo), atom((uint32_t)hi) }; th->X = said_tuple(compose(two, 2, 1)); found = 1; } else th->attr = th->attr2 = -1;
        }
        else for (int a = 0; a < t->na && !found; a++) if (named(w, t->an[a], r->identity[i].attr)) {
            if (r->identity[i].res == 1) { const uint8_t *p; size_t n; char h[16];
                if (raw_of(w, t->av[a], 0, &p, &n) && n <= 8) { memcpy(h, p, n); h[n] = 0; char *e; unsigned long cp = strtoul(h, &e, 16); if (!*e && cp < LP_NCP) { th->X = atom((uint32_t)cp); th->cp = (long)cp; found = 1; } } }
            else found = value_ref(w, t->av[a], r->identity[i].res == 2, 0, scp, &th->X);
            if (found) th->attr = a;
        }
        if (!found) continue;
        if (r->identity[i].kind) { th->X = pair_of(r->identity[i].as[0] ? kind_of(w, r->identity[i].as) : name_of(w, t->name), th->X); th->cp = -1; }
        else if (r->identity[i].within && S) { th->X = pair_of(*S, th->X); th->cp = -1; }
        return 1;
    }
    return 0;
}
/* What an attribute says: its value, as the thing it names if the recipe says it names one; several, if it is a list. */
static void said_by(const EW *w, const Tag *t, int a, Ref of, long scp, Refs *into, Refs *also){
    const Recipe *r = w->r; Ref key = name_of(w, t->an[a]); const char *kind = NULL; char ls = 0;
    for (int z = 0; z < r->nrefer; z++) if (named(w, t->an[a], r->refer[z].attr) && (!r->refer[z].el[0] || named(w, t->name, r->refer[z].el))) kind = r->refer[z].kind;
    for (int z = 0; z < r->nlist; z++) if (named(w, t->an[a], r->list[z].col)) ls = r->list[z].sep;
    if (!ls) { Ref v; if (!value_ref(w, t->av[a], listed((char (*)[32])r->codepoints, r->ncodepoints, w, t->an[a]), 0, scp, &v)) return;
        if (kind) v = pair_of(kind_of(w, kind), v);
        Ref c = claim3(of, key, v); refs_push(into, c); if (also) refs_push(also, c); return; }
    const uint8_t *p, *e; size_t n; if (!raw_of(w, t->av[a], 0, &p, &n)) return; e = p + n;
    while (p < e) { const uint8_t *q = memchr(p, ls, (size_t)(e - p)); if (!q) q = e; const uint8_t *vp = p; size_t vn = (size_t)(q - p);
        while (vn && (vp[0] == ' ' || vp[0] == '\n' || vp[0] == '\t')) { vp++; vn--; } while (vn && (vp[vn - 1] == ' ' || vp[vn - 1] == '\n' || vp[vn - 1] == '\t')) vn--;
        if (vn) { uint8_t *ub = malloc(vn * 2 + 16); size_t ul = xml_unescape(vp, vn, ub);
            if (ul) { Ref v = string_ref(ub, ul); if (kind) v = pair_of(kind_of(w, kind), v); Ref c = claim3(of, key, v); refs_push(into, c); if (also) refs_push(also, c); }
            free(ub); }
        p = q + 1; }
}
/* The text a thing's spans speak of: the text of the element inside it that the recipe names, exactly as written. */
static int text_in(const EW *w, const Tag *t, Txt *out){
    const Recipe *r = w->r; if (!r->nstretch || !t->has_content) return 0; uint32_t nc = ts_node_child_count(t->content);
    for (uint32_t c = 0; c < nc; c++) { TSNode ch = ts_node_child(t->content, c); Tag ct; if (!is(ch, "element") || !tag_of(ch, &ct) || !ct.has_content || holds_elements(ct.content)) continue;
        for (int i = 0; i < r->nstretch; i++) if (named(w, ct.name, r->stretch[i].text)) {
            const uint8_t *p = w->src + ts_node_start_byte(ct.content); size_t n = ts_node_end_byte(ct.content) - ts_node_start_byte(ct.content);
            out->p = malloc(n + 8); out->n = xml_unescape(p, n, out->p); return out->n > 0; } }
    return 0;
}
static int is_span_text(const EW *w, TSNode name){ for (int i = 0; i < w->r->nstretch; i++) if (named(w, name, w->r->stretch[i].text)) return 1; return 0; }
/* The stretch of a text between two characters, counted as the source counts them. */
static int stretch(const Txt *tx, long a, long b, int inclusive, Ref *out){
    if (!tx || a < 0 || b < a) return 0; if (inclusive) b++;
    size_t i = 0; long ch = 0; size_t lo = 0, hi = 0; int have_lo = 0;
    for (; i <= tx->n; ch++) {
        if (ch == a) { lo = i; have_lo = 1; } if (ch == b) { hi = i; break; }
        if (i == tx->n) return 0;
        uint8_t c = tx->p[i]; i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    }
    if (!have_lo || hi <= lo || hi > tx->n) return 0;
    *out = text_of(tx->p + lo, hi - lo); return 1;
}

static void element(const EW *w, TSNode el, const Ref *S, long scp, const Txt *tx, Refs *rec, Ref *node, int *has_node, Events *ev);
/* The elements inside a node, each read; many of them, on every core, their attestations joined in order. */
static void inside(const EW *w, TSNode content, const Ref *S, long scp, const Txt *tx, uint32_t skip, Refs *rec, Refs *items, Events *ev){
    uint32_t nc = ts_node_child_count(content); if (!nc) return;
    TSNode *kid = malloc(sizeof(TSNode) * nc); uint32_t k = 0;
    TSTreeCursor cur = ts_tree_cursor_new(content);
    if (ts_tree_cursor_goto_first_child(&cur)) do { TSNode c = ts_tree_cursor_current_node(&cur); if (is(c, "element") && ts_node_start_byte(c) != skip) kid[k++] = c; } while (ts_tree_cursor_goto_next_sibling(&cur));
    ts_tree_cursor_delete(&cur);
    if (!rec && k >= 256 && ts_node_end_byte(content) - ts_node_start_byte(content) > (1u << 20)) {
        int nt = omp_get_num_threads() * 8; if (nt > (int)k) nt = (int)k; Events *pe = calloc((size_t)nt, sizeof(Events));
        #pragma omp taskloop grainsize(1)
        for (int t = 0; t < nt; t++)
            for (uint32_t i = (uint32_t)((uint64_t)k * t / nt); i < (uint32_t)((uint64_t)k * (t + 1) / nt); i++) { Ref n_; int h_; element(w, kid[i], S, scp, tx, NULL, &n_, &h_, &pe[t]); }
        for (int t = 0; t < nt; t++) { for (uint64_t j = 0; j < pe[t].n; j++) ev_push(ev, &pe[t].e[j]); free(pe[t].e); }
        free(pe); free(kid); return;
    }
    for (uint32_t i = 0; i < k; i++) { Ref n_; int h_ = 0; element(w, kid[i], S, scp, tx, rec, &n_, &h_, ev); if (h_ && items) refs_push(items, n_); }
    free(kid);
}
static void element(const EW *w, TSNode el, const Ref *S, long scp, const Txt *tx, Refs *rec, Ref *node, int *has_node, Events *ev){
    const Recipe *r = w->r; *has_node = 0; Tag t; if (!tag_of(el, &t)) return;
    Ref nref = name_of(w, t.name); Thing th;
    for (int i = 0; i < r->nwords; i++) if (named(w, t.name, r->words[i].rec) && t.has_content) {
        /* a record of words: what it is about is the path of its words; what is said of a word is said within it */
        int isthing = thing_of(w, &t, S, scp, &th); Refs word = { 0 }, nodes = { 0 }, claims = { 0 };
        if (isthing) { if (S) refs_push(&claims, claim3(*S, nref, th.X)); for (int a = 0; a < t.na; a++) if (a != th.attr) said_by(w, &t, a, th.X, -1, &claims, NULL); }
        uint32_t nc = ts_node_child_count(t.content);
        for (uint32_t c = 0; c < nc; c++) { TSNode ch = ts_node_child(t.content, c); Tag ct; if (!is(ch, "element") || !tag_of(ch, &ct)) continue;
            int isword = 0; for (int z = 0; z < r->words[i].nword; z++) isword |= named(w, ct.name, r->words[i].word[z]);
            Ref wd; if (!isword || !ct.has_content || holds_elements(ct.content) || !value_ref(w, ct.content, 0, 0, -1, &wd)) continue;
            refs_push(&word, wd); Refs wc = { 0 }; for (int a = 0; a < ct.na; a++) said_by(w, &ct, a, wd, -1, &wc, NULL);
            for (int a = 0; a < wc.n; a++) refs_push(&claims, wc.c[a]);
            refs_push(&nodes, !wc.n ? wd : wc.n == 1 ? wc.c[0] : said_record(compose(wc.c, (uint32_t)wc.n, tier_of(wc.c, wc.n)))); free(wc.c); }
        if (word.n) {
            for (int a = 0; a < word.n; a++) word.c[a].said = 0;
            Ref about = word.n == 1 ? word.c[0] : compose(word.c, (uint32_t)word.n, tier_of(word.c, word.n));
            if (isthing) refs_push(&claims, claim3(th.X, nref, about));
            Ref *path = malloc(sizeof(Ref) * (size_t)(nodes.n + 1)); path[0] = about; memcpy(path + 1, nodes.c, sizeof(Ref) * (size_t)nodes.n);
            Ref rn = said_record(compose(path, (uint32_t)nodes.n + 1, tier_of(path, nodes.n + 1))); free(path);
            if (claims.n) record_of(w, ev, rn, claims.c, claims.n);
        }
        free(word.c); free(nodes.c); free(claims.c); return;
    }
    if (thing_of(w, &t, S, scp, &th)) {
        /* what it says itself it says together: its place in what it is inside, its attributes, its own text */
        Refs own = { 0 }; Ref place; int has_place = 0;
        if (S) { place = claim3(*S, nref, th.X); has_place = 1; if (!rec) refs_push(&own, place); }
        for (int a = 0; a < t.na; a++) if (a != th.attr && a != th.attr2) said_by(w, &t, a, th.X, th.cp, &own, NULL);
        if (th.attr != -2 && t.has_content && !holds_elements(t.content)) { Ref v; if (value_ref(w, t.content, 0, is_span_text(w, t.name), th.cp, &v)) refs_push(&own, claim3(th.X, nref, v)); }
        if (own.n == 1) alone(w, ev, own.c[0]);
        else if (own.n > 1) {
            Ref *path = malloc(sizeof(Ref) * (size_t)(own.n + 1)); path[0] = nref; memcpy(path + 1, own.c, sizeof(Ref) * (size_t)own.n);
            record_of(w, ev, said_record(compose(path, (uint32_t)own.n + 1, tier_of(path, own.n + 1))), own.c, own.n); free(path);
        }
        free(own.c);
        if (t.has_content && holds_elements(t.content)) { Txt mine; int has = text_in(w, &t, &mine);
            inside(w, t.content, &th.X, th.cp, has ? &mine : tx, th.child, NULL, NULL, ev); if (has) free(mine.p); }
        if (rec && has_place) { refs_push(rec, place); *node = place; *has_node = 1; }   /* inside a record: its place there is what the record says of it */
        return;
    }
    if (!S) { if (t.has_content) inside(w, t.content, NULL, -1, NULL, UINT32_MAX, NULL, NULL, ev); return; }   /* it speaks of nothing: what is inside it may */
    for (int i = 0; i < r->nlink; i++) if (named(w, t.name, r->link[i].el)) {              /* a relation of S */
        int pa = -1, oa = -1; for (int a = 0; a < t.na; a++) { if (named(w, t.an[a], r->link[i].pred)) pa = a; if (named(w, t.an[a], r->link[i].obj)) oa = a; }
        Ref p; if (pa < 0 || !value_ref(w, t.av[pa], 0, 0, scp, &p)) continue;               /* not written as the relation: read as any other element */
        Refs said = { 0 };
        if (r->link[i].obj[0] == '>') { if (t.has_content) { uint32_t nc = ts_node_child_count(t.content);
            for (uint32_t c = 0; c < nc; c++) { TSNode ch = ts_node_child(t.content, c); Tag ct; Ref o;
                if (is(ch, "element") && tag_of(ch, &ct) && named(w, ct.name, r->link[i].obj + 1) && ct.has_content && value_ref(w, ct.content, 0, 0, scp, &o)) {
                    if (r->link[i].kind[0]) o = pair_of(kind_of(w, r->link[i].kind), o); refs_push(&said, claim3(*S, p, o)); } } } }
        else { Ref o; if (oa >= 0 && value_ref(w, t.av[oa], 0, 0, scp, &o)) { if (r->link[i].kind[0]) o = pair_of(kind_of(w, r->link[i].kind), o); refs_push(&said, claim3(*S, p, o)); } }
        for (int j = 0; j < said.n; j++) { if (rec) refs_push(rec, said.c[j]); else alone(w, ev, said.c[j]); }
        if (said.n) { *node = said.n == 1 ? said.c[0] : said_record(compose(said.c, (uint32_t)said.n, tier_of(said.c, said.n))); *has_node = 1; }
        free(said.c); return;
    }
    /* one that speaks of a stretch of text speaks of that stretch */
    Ref of = *S; long ofcp = scp; int sa = -1, sb = -1, spanned = 0; Ref covered;
    for (int i = 0; i < r->nstretch && !spanned; i++) if (named(w, t.name, r->stretch[i].el)) {
        for (int a = 0; a < t.na; a++) { if (named(w, t.an[a], r->stretch[i].start)) sa = a; if (named(w, t.an[a], r->stretch[i].end)) sb = a; }
        const uint8_t *p, *q; size_t pn, qn;
        if (sa >= 0 && sb >= 0 && raw_of(w, t.av[sa], 0, &p, &pn) && raw_of(w, t.av[sb], 0, &q, &qn) && pn < 12 && qn < 12) {
            char x[16], y[16]; memcpy(x, p, pn); x[pn] = 0; memcpy(y, q, qn); y[qn] = 0; char *e1, *e2; long a0 = strtol(x, &e1, 10), b0 = strtol(y, &e2, 10);
            if (!*e1 && !*e2 && stretch(tx, a0, b0, r->stretch[i].inclusive, &covered)) { spanned = 1; of = covered; ofcp = -1; } }
        if (!spanned) sa = sb = -1;
    }
    if (!rec && !spanned && !t.na && t.has_content && holds_elements(t.content)) {
        /* it says nothing itself. If it holds things, it is the record of their places; if not, it only holds */
        int things = 0; uint32_t nc = ts_node_child_count(t.content);
        for (uint32_t c = 0; c < nc && !things; c++) { TSNode ch = ts_node_child(t.content, c); Tag ct; Thing x; if (is(ch, "element") && tag_of(ch, &ct)) things = thing_of(w, &ct, S, scp, &x); }
        if (!things) { inside(w, t.content, S, scp, tx, UINT32_MAX, NULL, NULL, ev); return; }
    }
    Refs mine = { 0 }, items = { 0 }, *claims = rec ? rec : &mine; int before = claims->n;
    if (spanned) refs_push(&items, covered);
    for (int a = 0; a < t.na; a++) if (a != sa && a != sb) said_by(w, &t, a, of, ofcp, claims, &items);
    if (t.has_content) {
        if (!holds_elements(t.content)) { Ref v; if (value_ref(w, t.content, 0, is_span_text(w, t.name), ofcp, &v)) { Ref c = claim3(of, nref, v); refs_push(claims, c); refs_push(&items, c); } }
        else inside(w, t.content, spanned ? &of : S, ofcp, tx, UINT32_MAX, claims, &items, ev);
    }
    if (items.n == 1) { *node = items.c[0]; *has_node = 1; }
    else if (items.n > 1) {
        Ref *path = malloc(sizeof(Ref) * (size_t)(items.n + 1)); path[0] = nref; memcpy(path + 1, items.c, sizeof(Ref) * (size_t)items.n);
        *node = said_record(compose(path, (uint32_t)items.n + 1, tier_of(path, items.n + 1))); *has_node = 1; free(path);
    }
    if (!rec && *has_node && claims->n > before) {                               /* the record, and its claims within it */
        if (claims->n - before == 1 && items.n == 1) alone(w, ev, claims->c[before]);
        else record_of(w, ev, *node, claims->c + before, claims->n - before);
    }
    free(mine.c); free(items.c);
}
/* Down through whatever is not an element (the document, a stretch the parser could not read) to the elements. */
static void below(const EW *w, TSNode nd, Events *ev){
    if (holds_elements(nd)) inside(w, nd, NULL, -1, NULL, UINT32_MAX, NULL, NULL, ev);
    TSTreeCursor cur = ts_tree_cursor_new(nd);
    if (ts_tree_cursor_goto_first_child(&cur)) do { TSNode c = ts_tree_cursor_current_node(&cur); if (!is(c, "element") && ts_node_child_count(c)) below(w, c, ev); } while (ts_tree_cursor_goto_next_sibling(&cur));
    ts_tree_cursor_delete(&cur);
}
void attest_elements(const Recipe *r, File *f, void *root_node, const uint8_t *src, size_t n){
    EW w = { r, src, 1500.0f, 0.0f, "" }; dir_of(f->path, w.dir, sizeof w.dir); for (int k = 0; k < r->nblock; k++) if (!r->block[k].is_map) { w.er = r->block[k].enter_rating; w.ed = r->block[k].enter_deviation; break; }
    if (root_node) { inside(&w, *(TSNode *)root_node, NULL, -1, NULL, UINT32_MAX, NULL, NULL, &f->ev); return; }
    /* a long file of records, each a thing on lines of its own. The run of them, from the first line that begins one
     * to the end of the last, is parted before lines that begin one; the parts are parsed and read on every core.
     * What stands before and after the run is read together, as the one document it is without the run. */
    char open[80], close[80]; int ol = snprintf(open, sizeof open, "<%s ", r->identity[0].el), cl = snprintf(close, sizeof close, "</%s>", r->identity[0].el);
    #define BEGINS(c) ({ size_t b_ = (c); while (b_ < n && (src[b_] == ' ' || src[b_] == '\t')) b_++; b_ + (size_t)ol < n && !memcmp(src + b_, open, (size_t)ol); })
    size_t lo = n, hi = 0, last = n;
    for (size_t c = 0; c < n; ) { if (BEGINS(c)) { if (lo == n) lo = c; last = c; } const uint8_t *nl = memchr(src + c, '\n', n - c); c = nl ? (size_t)(nl - src) + 1 : n; }
    if (lo == n) { TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, r->lang); TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
                   below(&w, ts_tree_root_node(t), &f->ev); ts_tree_delete(t); ts_parser_delete(ps); return; }
    { const uint8_t *nl = memchr(src + last, '\n', n - last); size_t e = nl ? (size_t)(nl - src) : n, x = e; while (x > last && (src[x - 1] == '\r' || src[x - 1] == ' ')) x--;
      if (x >= 2 && src[x - 2] == '/' && src[x - 1] == '>') hi = nl ? e + 1 : n;                       /* the last one ends on its own line */
      else { const uint8_t *c2 = memmem(src + last, n - last, close, (size_t)cl); const uint8_t *n2 = c2 ? memchr(c2, '\n', n - (size_t)(c2 - src)) : NULL; hi = n2 ? (size_t)(n2 - src) + 1 : n; } }
    int np = omp_get_num_threads() * 4; if (np < 1) np = 1; size_t *cut = malloc(sizeof(size_t) * (size_t)(np + 1)); cut[0] = lo; int k = 1;
    for (int i = 1; i < np; i++) { size_t c = lo + (hi - lo) / (size_t)np * (size_t)i; int found = 0;
        while (c < hi && !found) { while (c < hi && src[c - 1] != '\n') c++; if (c < hi && BEGINS(c)) found = 1; else c++; }
        if (found && c > cut[k - 1] && c < hi) cut[k++] = c; }
    cut[k] = hi; Events *pe = calloc((size_t)k + 1, sizeof(Events));
    #undef BEGINS
    /* what stands around the run, parsed first: the things the run is inside are found in it, where the run was */
    uint8_t *around = malloc(lo + (n - hi) + 1); memcpy(around, src, lo); memcpy(around + lo, src + hi, n - hi);
    TSParser *aps = ts_parser_new(); ts_parser_set_language(aps, r->lang); TSTree *at = ts_parser_parse_string(aps, NULL, (const char *)around, (uint32_t)(lo + n - hi));
    EW wa = w; wa.src = around; Ref S; int has_S = 0; long scp = -1;
    { TSNode chain[64]; int nc = 0; TSNode nd = ts_node_descendant_for_byte_range(ts_tree_root_node(at), (uint32_t)(lo ? lo - 1 : 0), (uint32_t)lo);
      for (; !ts_node_is_null(nd) && nc < 64; nd = ts_node_parent(nd)) if (is(nd, "element") && ts_node_start_byte(nd) < lo && ts_node_end_byte(nd) > lo) chain[nc++] = nd;
      for (int i = nc - 1; i >= 0; i--) { Tag t; Thing th; if (tag_of(chain[i], &t) && thing_of(&wa, &t, has_S ? &S : NULL, scp, &th)) { S = th.X; scp = th.cp; has_S = 1; } } }
    #pragma omp taskloop grainsize(1)
    for (int i = 0; i <= k; i++) {
        if (i == k) { below(&wa, ts_tree_root_node(at), &pe[i]); continue; }
        /* a part, made a document of its own by an element around it that says nothing */
        size_t len = cut[i + 1] - cut[i]; uint8_t *doc = malloc(len + 16); memcpy(doc, "<_>\n", 4); memcpy(doc + 4, src + cut[i], len); memcpy(doc + 4 + len, "\n</_>", 6);
        TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, r->lang); TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)doc, (uint32_t)(len + 9));
        EW wi = w; wi.src = doc; TSNode root = ts_tree_root_node(t), top = { 0 }; int has_top = 0;
        { TSTreeCursor cur = ts_tree_cursor_new(root); if (ts_tree_cursor_goto_first_child(&cur)) do { TSNode c = ts_tree_cursor_current_node(&cur); if (is(c, "element")) { top = c; has_top = 1; } } while (!has_top && ts_tree_cursor_goto_next_sibling(&cur)); ts_tree_cursor_delete(&cur); }
        Tag tt; if (has_top && !ts_node_has_error(root) && tag_of(top, &tt) && tt.has_content) inside(&wi, tt.content, has_S ? &S : NULL, scp, NULL, UINT32_MAX, NULL, NULL, &pe[i]);
        else {
            #pragma omp atomic
            f->incomplete++;
            fprintf(stderr, "\n  %s: the part of %s from byte %zu to byte %zu does not parse as whole records; what it holds was not read\n", r->name, f->path, cut[i], cut[i + 1]);
        }
        ts_tree_delete(t); ts_parser_delete(ps); free(doc);
    }
    ts_tree_delete(at); ts_parser_delete(aps); free(around);
    k++;
    for (int i = 0; i < k; i++) { for (uint64_t j = 0; j < pe[i].n; j++) ev_push(&f->ev, &pe[i].e[j]); free(pe[i].e); }
    free(pe); free(cut);
}
