/* Recipes and the decomposition they drive.
 *
 * A recipe file:
 *   name NAME
 *   match GLOB...                 files it applies to (by file name)
 *   grammar text | NAME           UAX #29 text, or a tree-sitter grammar loaded from $LAPLACE_GRAMMARS
 *   trust T                       the source's trust as a witness, -1 .. 1
 *   query                         tree-sitter query patterns, up to a line "end"; every match attests one claim:
 *   ...                             @subject, @predicate, @object, each with a resolver suffix:
 *   end                             .cp   a codepoint written in hex (quotes stripped)
 *                                   .text the node's text (quotes stripped)
 *                                   .xml  the node's text with XML references resolved (quotes stripped)
 *                                   .node the node itself, as recorded
 *                                 predicates: #eq? #not-eq? #any-of? #not-any-of?
 *   witness NAME...               the source as witness, named as content. A recipe with a query is a curated source:
 *                                 what is recorded is what it attests — the claims and the entities they relate — never
 *                                 the file's own syntax (rows, tags, delimiters)
 *   predicate TEXT                the claims' predicate, when the source states it by position rather than by name
 *   records                       the file is a flat sequence of line-terminated records: large files are split at
 *                                 line boundaries and the pieces parsed on every core, then joined under one root
 *   subject-attribute A [F L]     for @subject.attr: the subject is the codepoint in sibling attribute A of the captured
 *                                 node's parent, or, when the element carries F and L instead, the element itself
 *                                 (a range is attested at its element, never copied to each codepoint)
 * With a grammar, a file is recorded as its syntax tree: each node the composition of its children with the bytes
 * between them kept as text, so it recomposes byte for byte. Leaves are text, decomposed by UAX #29. */
#include "engine.h"
#include <tree_sitter/api.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Ctx **CTX;
static Ctx *ctx_here(void){ return CTX[omp_get_thread_num()]; }

/* ---- recipes */
static const TSLanguage *grammar_load(const char *name){
    const char *dir = getenv("LAPLACE_GRAMMARS"); if (!dir) dir = "/repos/build/grammars";
    char p[1024], sym[128]; snprintf(p, sizeof p, "%s/libtree-sitter-%s.so", dir, name); snprintf(sym, sizeof sym, "tree_sitter_%s", name);
    void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL); if (!h) { fprintf(stderr, "grammar %s: %s\n", name, dlerror()); return NULL; }
    const TSLanguage *(*f)(void) = (const TSLanguage *(*)(void))dlsym(h, sym);
    return f ? f() : NULL;
}
static int recipe_parse(const char *path, Recipe *r){
    FILE *f = fopen(path, "r"); if (!f) return 0;
    memset(r, 0, sizeof *r); char line[4096]; int inq = 0; size_t ql = 0;
    while (fgets(line, sizeof line, f)) {
        if (inq) {
            if (!strncmp(line, "end", 3) && (line[3] == '\n' || line[3] == '\r' || !line[3])) { inq = 0; continue; }
            size_t l = strlen(line); r->query_src = xrealloc(r->query_src, ql + l + 1); memcpy(r->query_src + ql, line, l + 1); ql += l; continue;
        }
        char *h = strchr(line, '#'); if (h && (h == line || h[-1] == ' ' || h[-1] == '\t')) *h = 0;
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "name")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->name, sizeof r->name, "%s", tok); }
        else if (!strcmp(tok, "match")) while ((tok = strtok(NULL, " \t\r\n")) && r->nmatch < 16) snprintf(r->match[r->nmatch++], 128, "%s", tok);
        else if (!strcmp(tok, "grammar")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->grammar, sizeof r->grammar, "%s", tok); }
        else if (!strcmp(tok, "trust")) { tok = strtok(NULL, " \t\r\n"); if (tok) r->trust = atof(tok); }
        else if (!strcmp(tok, "query")) inq = 1;
        else if (!strcmp(tok, "records")) r->records = 1;
        else if (!strcmp(tok, "witness")) { char *rest = strtok(NULL, "\r\n"); if (rest) { while (*rest == ' ' || *rest == '\t') rest++; snprintf(r->witness, sizeof r->witness, "%s", rest); } }
        else if (!strcmp(tok, "predicate")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->predicate, sizeof r->predicate, "%s", tok); }
        else if (!strcmp(tok, "subject-attribute")) for (int k = 0; k < 3 && (tok = strtok(NULL, " \t\r\n")); k++) snprintf(r->subject_attr[k], 48, "%s", tok);
    }
    fclose(f);
    if (!r->grammar[0]) snprintf(r->grammar, sizeof r->grammar, "text");
    if (strcmp(r->grammar, "text")) {
        r->lang = grammar_load(r->grammar); if (!r->lang) return 0;
        if (r->query_src) {
            uint32_t off; TSQueryError err;
            r->query = ts_query_new(r->lang, r->query_src, (uint32_t)strlen(r->query_src), &off, &err);
            if (!r->query) { fprintf(stderr, "%s: query error %d at byte %u\n", path, err, off); return 0; }
        }
    }
    return 1;
}
int recipes_load(const char *dir, Recipe **out){
    DIR *d = opendir(dir); if (!d) { perror(dir); return 0; }
    struct dirent *de; int n = 0; Recipe *r = NULL;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name); if (l < 8 || strcmp(de->d_name + l - 7, ".recipe")) continue;
        char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        r = xrealloc(r, sizeof(Recipe) * (n + 1));
        if (recipe_parse(p, &r[n])) n++; else fprintf(stderr, "recipe %s not loaded\n", p);
    }
    closedir(d); *out = r; return n;
}
Recipe *recipe_for(Recipe *r, int n, const char *path){
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    for (int pass = 0; pass < 2; pass++)                                    /* exact names before patterns */
        for (int i = 0; i < n; i++) for (int j = 0; j < r[i].nmatch; j++)
            if ((strpbrk(r[i].match[j], "*?[") != NULL) == pass && !fnmatch(r[i].match[j], base, 0)) return &r[i];
    return NULL;
}

/* ---- strings to entities, decomposed once per thread */
typedef struct { uint64_t h; Ref ref; uint32_t off, len; } SEnt;
typedef struct { SEnt *t; uint64_t cap, n; char *pool; size_t pn, pcap; } SCache;
static __thread SCache sc;
static uint64_t fnv(const uint8_t *s, size_t n){ uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; } return h ? h : 1; }
static Ref string_ref(const uint8_t *s, size_t n){
    if ((sc.n + 1) * 2 > sc.cap) {
        uint64_t oc = sc.cap; SEnt *old = sc.t; sc.cap = oc ? oc * 2 : 4096; sc.t = calloc(sc.cap, sizeof(SEnt));
        for (uint64_t i = 0; i < oc; i++) if (old[i].h) { uint64_t k = old[i].h & (sc.cap - 1); while (sc.t[k].h) k = (k + 1) & (sc.cap - 1); sc.t[k] = old[i]; }
        free(old);
    }
    uint64_t h = fnv(s, n), k = h & (sc.cap - 1);
    while (sc.t[k].h) { if (sc.t[k].h == h && sc.t[k].len == n && !memcmp(sc.pool + sc.t[k].off, s, n)) return sc.t[k].ref; k = (k + 1) & (sc.cap - 1); }
    if (sc.pn + n > sc.pcap) { sc.pcap = (sc.pn + n) * 2 + 4096; sc.pool = xrealloc(sc.pool, sc.pcap); }
    memcpy(sc.pool + sc.pn, s, n);
    sc.t[k] = (SEnt){ h, text_ref(ctx_here(), s, n), (uint32_t)sc.pn, (uint32_t)n }; sc.pn += n; sc.n++;
    return sc.t[k].ref;
}

/* ---- the syntax tree as content. Large nodes decompose their children as parallel tasks. */
static Ref ast_node(TSNode nd, const uint8_t *src, uint32_t lo, uint32_t hi, int depth){
    uint32_t nc = ts_node_child_count(nd);
    if (nc == 0 || depth > 200) return string_ref(src + lo, hi - lo);
    Ref *kids = malloc(sizeof(Ref) * (2 * (size_t)nc + 1)); uint32_t *cs = malloc(8 * (size_t)nc), *ce = cs + nc;
    TSNode *cn = malloc(sizeof(TSNode) * nc);                          /* children in one pass: ts_node_child(i) is O(i) */
    { TSTreeCursor cur = ts_tree_cursor_new(nd); uint32_t i = 0;
      if (ts_tree_cursor_goto_first_child(&cur)) do { cn[i] = ts_tree_cursor_current_node(&cur); cs[i] = ts_node_start_byte(cn[i]); ce[i] = ts_node_end_byte(cn[i]); i++; } while (i < nc && ts_tree_cursor_goto_next_sibling(&cur));
      ts_tree_cursor_delete(&cur); }
    /* slot 2i: the text before child i; slot 2i+1: child i; slot 2nc: the text after the last child */
    uint8_t *has = calloc(2 * (size_t)nc + 1, 1);
    if (hi - lo > (4u << 20) && nc >= 16) {
        #pragma omp taskloop num_tasks(omp_get_num_threads() * 8)
        for (uint32_t i = 0; i < nc; i++) if (ce[i] > cs[i]) { kids[2 * i + 1] = ast_node(cn[i], src, cs[i], ce[i], depth + 1); has[2 * i + 1] = 1; }
    } else
        for (uint32_t i = 0; i < nc; i++) if (ce[i] > cs[i]) { kids[2 * i + 1] = ast_node(cn[i], src, cs[i], ce[i], depth + 1); has[2 * i + 1] = 1; }
    uint32_t at = lo;
    for (uint32_t i = 0; i < nc; i++) { if (cs[i] > at) { kids[2 * i] = string_ref(src + at, cs[i] - at); has[2 * i] = 1; } if (ce[i] > at) at = ce[i]; }
    if (hi > at) { kids[2 * nc] = string_ref(src + at, hi - at); has[2 * nc] = 1; }
    uint32_t k = 0; uint8_t t = 0;
    for (uint32_t i = 0; i <= 2 * nc; i++) if (has[i]) { kids[k] = kids[i]; if (kids[k].tier > t) t = kids[k].tier; k++; }
    Ref r = compose(kids, k, (uint8_t)(t + 1));
    free(kids); free(cs); free(has); free(cn);
    return r;
}

/* ---- attestation queries */
static size_t xml_unescape(const uint8_t *s, size_t n, uint8_t *o){
    size_t k = 0;
    for (size_t i = 0; i < n; ) {
        if (s[i] != '&') { o[k++] = s[i++]; continue; }
        size_t j = i + 1; while (j < n && s[j] != ';' && j - i < 12) j++;
        if (j >= n || s[j] != ';') { o[k++] = s[i++]; continue; }
        const char *e = (const char *)s + i + 1; size_t el = j - i - 1; uint32_t cp = 0; int ok = 1;
        if (el == 2 && !memcmp(e, "lt", 2)) cp = '<'; else if (el == 2 && !memcmp(e, "gt", 2)) cp = '>';
        else if (el == 3 && !memcmp(e, "amp", 3)) cp = '&'; else if (el == 4 && !memcmp(e, "apos", 4)) cp = '\''; else if (el == 4 && !memcmp(e, "quot", 4)) cp = '"';
        else if (el > 1 && e[0] == '#') cp = (uint32_t)(e[1] == 'x' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10));
        else ok = 0;
        if (!ok || cp >= LP_NCP) { o[k++] = s[i++]; continue; }
        if (cp < 0x80) o[k++] = (uint8_t)cp;
        else if (cp < 0x800) { o[k++] = 0xC0 | cp >> 6; o[k++] = 0x80 | (cp & 0x3F); }
        else if (cp < 0x10000) { o[k++] = 0xE0 | cp >> 12; o[k++] = 0x80 | ((cp >> 6) & 0x3F); o[k++] = 0x80 | (cp & 0x3F); }
        else { o[k++] = 0xF0 | cp >> 18; o[k++] = 0x80 | ((cp >> 12) & 0x3F); o[k++] = 0x80 | ((cp >> 6) & 0x3F); o[k++] = 0x80 | (cp & 0x3F); }
        i = j + 1;
    }
    return k;
}
static void unquote(const uint8_t **p, size_t *n){
    if (*n >= 2 && ((*p)[0] == '"' || (*p)[0] == '\'') && (*p)[*n - 1] == (*p)[0]) { (*p)++; *n -= 2; }
}
static int text_is(const uint8_t *src, TSNode nd, const char *s, uint32_t l){
    uint32_t a = ts_node_start_byte(nd), b = ts_node_end_byte(nd); return b - a == l && !memcmp(src + a, s, l);
}
static int predicates_hold(const TSQuery *q, const TSQueryMatch *m, const uint8_t *src){
    uint32_t ns; const TSQueryPredicateStep *st = ts_query_predicates_for_pattern(q, m->pattern_index, &ns);
    for (uint32_t i = 0; i < ns; ) {
        uint32_t j = i; while (j < ns && st[j].type != TSQueryPredicateStepTypeDone) j++;
        if (j > i && st[i].type == TSQueryPredicateStepTypeString) {
            uint32_t l; const char *op = ts_query_string_value_for_id(q, st[i].value_id, &l);
            int neg = !strncmp(op, "not-", 4), any = strstr(op, "any-of") != NULL;
            if (j - i >= 3 && st[i + 1].type == TSQueryPredicateStepTypeCapture) {
                TSNode cn = { 0 }; int found = 0;
                for (uint16_t c = 0; c < m->capture_count; c++) if (m->captures[c].index == st[i + 1].value_id) { cn = m->captures[c].node; found = 1; break; }
                if (found) {
                    int hit = 0;
                    for (uint32_t k = i + 2; k < j && (any || k == i + 2); k++) {
                        if (st[k].type != TSQueryPredicateStepTypeString) continue;
                        uint32_t vl; const char *v = ts_query_string_value_for_id(q, st[k].value_id, &vl);
                        if (text_is(src, cn, v, vl)) hit = 1;
                        if (!any && !hit) {                          /* #eq? on a quoted value compares the unquoted text */
                            const uint8_t *p = src + ts_node_start_byte(cn); size_t n = ts_node_end_byte(cn) - ts_node_start_byte(cn);
                            unquote(&p, &n); hit = n == vl && !memcmp(p, v, vl);
                        }
                    }
                    if (hit == neg) return 0;
                }
            }
        }
        i = j + 1;
    }
    return 1;
}
static void ev_push(Events *e, lp_id claim, float score){
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 65536; e->e = xrealloc(e->e, e->cap * sizeof(Event)); }
    e->e[e->n++] = (Event){ claim, score };
}
static int attr_named(TSNode tag, const uint8_t *src, const char *name, TSNode *val){
    uint32_t nc = ts_node_named_child_count(tag), l = (uint32_t)strlen(name);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode a = ts_node_named_child(tag, i); if (strcmp(ts_node_type(a), "Attribute")) continue;
        if (text_is(src, ts_node_named_child(a, 0), name, l)) { *val = ts_node_named_child(a, 1); return 1; }
    }
    return 0;
}
static int resolve_subject_attr(const Recipe *r, TSNode nd, const uint8_t *src, Ref *out){
    TSNode tag = ts_node_parent(nd); TSNode v;
    if (r->subject_attr[0][0] && attr_named(tag, src, r->subject_attr[0], &v)) {
        const uint8_t *p = src + ts_node_start_byte(v); size_t n = ts_node_end_byte(v) - ts_node_start_byte(v); unquote(&p, &n);
        char h[16]; if (!n || n > 8) return 0; memcpy(h, p, n); h[n] = 0; char *e; unsigned long cp = strtoul(h, &e, 16);
        if (*e || cp >= LP_NCP) return 0; *out = atom((uint32_t)cp); return 1;
    }
    if (r->subject_attr[1][0] && attr_named(tag, src, r->subject_attr[1], &v)) {   /* a range: the element as recorded */
        *out = ast_node(tag, src, ts_node_start_byte(tag), ts_node_end_byte(tag), 0); return 1;
    }
    return 0;
}
static int resolve(const char *suffix, TSNode nd, const uint8_t *src, uint8_t *buf, Ref *out){
    const uint8_t *p = src + ts_node_start_byte(nd); size_t n = ts_node_end_byte(nd) - ts_node_start_byte(nd);
    if (!strcmp(suffix, "node")) { *out = ast_node(nd, src, ts_node_start_byte(nd), ts_node_end_byte(nd), 0); return 1; }
    unquote(&p, &n); if (!n) return 0;                                  /* an empty value is no content */
    if (!strcmp(suffix, "cp")) { char h[16]; if (n > 8) return 0; memcpy(h, p, n); h[n] = 0; char *e; unsigned long v = strtoul(h, &e, 16);
                                 if (*e || v >= LP_NCP) return 0; *out = atom((uint32_t)v); return 1; }
    if (!strcmp(suffix, "xml")) { size_t l = xml_unescape(p, n, buf); if (!l) return 0; *out = string_ref(buf, l); return 1; }
    *out = string_ref(p, n); return 1;
}
static void attest_tree(const Recipe *r, TSNode root, const uint8_t *src, size_t n, Events *ev){
    TSQueryCursor *qc = ts_query_cursor_new(); ts_query_cursor_exec(qc, r->query, root);
    uint8_t *buf = malloc(n + 4); TSQueryMatch m;
    while (ts_query_cursor_next_match(qc, &m)) {
        if (!predicates_hold(r->query, &m, src)) continue;
        Ref part[3]; int have[3] = { 0, 0, 0 };
        for (uint16_t c = 0; c < m.capture_count; c++) {
            uint32_t l; const char *name = ts_query_capture_name_for_id(r->query, m.captures[c].index, &l);
            int role = !strncmp(name, "subject", 7) ? 0 : !strncmp(name, "predicate", 9) ? 1 : !strncmp(name, "object", 6) ? 2 : -1;
            if (role < 0) continue;
            const char *dot = memchr(name, '.', l); char suf[16] = "text";
            if (dot) { size_t sl = l - (size_t)(dot + 1 - name); if (sl < sizeof suf) { memcpy(suf, dot + 1, sl); suf[sl] = 0; } }
            if (!strcmp(suf, "attr")) {                                          /* the subject named by a sibling attribute */
                TSNode an = m.captures[c].node; if (!strcmp(ts_node_type(an), "Attribute")) an = ts_node_named_child(an, 0);
                uint32_t nl; const uint8_t *np = src + ts_node_start_byte(an); nl = ts_node_end_byte(an) - ts_node_start_byte(an);
                int own = 0; for (int k = 0; k < 3 && !own; k++) own = r->subject_attr[k][0] && strlen(r->subject_attr[k]) == nl && !memcmp(r->subject_attr[k], np, nl);
                if (own) { have[0] = have[1] = have[2] = 0; break; }          /* the subject's own attributes attest nothing */
                have[role] = resolve_subject_attr(r, m.captures[c].node, src, &part[role]); continue;
            }
            have[role] = resolve(suf, m.captures[c].node, src, buf, &part[role]);
        }
        if (!have[1] && r->predicate[0]) { part[1] = string_ref((const uint8_t *)r->predicate, strlen(r->predicate)); have[1] = 1; }
        if (!have[0] || !have[1] || !have[2]) continue;
        uint8_t t = 0; for (int i = 0; i < 3; i++) if (part[i].tier > t) t = part[i].tier;
        Ref claim = compose(part, 3, (uint8_t)(t + 1));
        ev_push(ev, claim.id, 1.0f);                                         /* the source asserts it: a win */
    }
    free(buf); ts_query_cursor_delete(qc);
}

/* ---- one file */
void decompose_file(Ctx *c, File *f){
    FILE *fp = fopen(f->path, "rb"); if (!fp) { f->skipped = 1; return; }
    fseek(fp, 0, SEEK_END); size_t n = (size_t)ftell(fp); rewind(fp);
    uint8_t *src = malloc(n + 1); if (fread(src, 1, n, fp) != n) { fclose(fp); free(src); f->skipped = 1; return; }
    fclose(fp); f->bytes = n;
    const Recipe *r = f->recipe;
    if (r && r->query) {                                                        /* the witness, named as content */
        const char *w = r->witness[0] ? r->witness : r->name; f->witness = text_ref(c, (const uint8_t *)w, strlen(w)); f->trunk = f->witness;
    }
    if (!r || !r->lang) f->trunk = text_ref(c, src, n);
    else if (r->records && n > (64u << 20) && !getenv("LAPLACE_ONE_PARSE")) {
        /* line records: split after a line break, parse the pieces on every core, and join their top-level children
         * (with the bytes between them) under one root, exactly as one parse of the whole file would give */
        int np = omp_get_num_threads() * 4; if (np < 1) np = 1;
        size_t *cut = malloc(sizeof(size_t) * (np + 1)); cut[0] = 0; int k = 1;
        for (int i = 1; i < np; i++) { size_t c = n / np * i; while (c < n && src[c - 1] != '\n') c++; if (c > cut[k - 1] && c < n) cut[k++] = c; }
        cut[k] = n;
        typedef struct { Ref *v; size_t n; uint8_t t; Events ev; } Part; Part *part = calloc(k, sizeof(Part));
        #pragma omp taskloop grainsize(1)
        for (int i = 0; i < k; i++) {
            TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, r->lang);
            TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src + cut[i], (uint32_t)(cut[i + 1] - cut[i]));
            TSNode root = ts_tree_root_node(t); uint32_t nc = ts_node_child_count(root);
            Ref *v = malloc(sizeof(Ref) * (2 * (size_t)nc + 2)); size_t m = 0; uint32_t at = 0; uint8_t tm = 0;
            const uint8_t *base = src + cut[i]; uint32_t len = (uint32_t)(cut[i + 1] - cut[i]);
            TSTreeCursor cur = ts_tree_cursor_new(root);
            if (!r->query && ts_tree_cursor_goto_first_child(&cur)) do {
                TSNode c = ts_tree_cursor_current_node(&cur); uint32_t a = ts_node_start_byte(c), b = ts_node_end_byte(c);
                if (a > at) v[m++] = string_ref(base + at, a - at);
                if (b > a) v[m++] = ast_node(c, base, a, b, 1);
                if (b > at) at = b;
            } while (ts_tree_cursor_goto_next_sibling(&cur));
            ts_tree_cursor_delete(&cur);
            if (!r->query && len > at) v[m++] = string_ref(base + at, len - at);
            for (size_t j = 0; j < m; j++) if (v[j].tier > tm) tm = v[j].tier;
            part[i].v = v; part[i].n = m; part[i].t = tm;
            if (r->query) attest_tree(r, root, base, len, &part[i].ev);         /* this piece's attestations, in order */
            ts_tree_delete(t); ts_parser_delete(ps);
        }
        size_t tot = 0; uint8_t tm = 0; for (int i = 0; i < k; i++) { tot += part[i].n; if (part[i].t > tm) tm = part[i].t; }
        Ref *all = malloc(sizeof(Ref) * (tot + 1)); size_t m = 0;
        for (int i = 0; i < k; i++) {
            memcpy(all + m, part[i].v, sizeof(Ref) * part[i].n); m += part[i].n; free(part[i].v);
            for (uint64_t j = 0; j < part[i].ev.n; j++) ev_push(&f->ev, part[i].ev.e[j].claim, part[i].ev.e[j].score);   /* reading order */
            free(part[i].ev.e);
        }
        if (!r->query) f->trunk = compose(all, (uint32_t)m, (uint8_t)(tm + 1));
        free(all); free(part); free(cut);
    }
    else {
        TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, r->lang);
        TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
        TSNode root = ts_tree_root_node(t);
        if (!r->query) f->trunk = ast_node(root, src, 0, (uint32_t)n, 0);   /* content: the file as itself */
        else attest_tree(r, root, src, n, &f->ev);                       /* a curated source: what it attests */
        ts_tree_delete(t); ts_parser_delete(ps);
    }
    free(src);
}
