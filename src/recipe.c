/* Sources, their recipes, and the decomposition they drive.
 *
 * A source is a directory of the recipes directory: a file named "source", and a recipe for each kind of file the
 * source comes in. The source file:
 *   name NAME
 *   witness NAME...               who testifies when this source attests, named as content
 *   lineage NAME...               the witness it derives from
 *   class NAME                    the witness's trust class, one the registry declares (Laplace-Native's
 *                                 manifest/trust_classes.toml): its prior is the trust the witness's claims play at.
 *                                 A trust written as a number is refused
 *   root PATH                     where the source is kept ($NAME from the environment; * for the newest of several).
 *                                 Several roots may be given: the first that exists is the source
 *   files PATTERN                 the files it is, when it is not everything under a root (several may be given)
 *   except PATTERN...             files under its root that are not the source
 *   room N                        what it takes in the database, in times what its files hold, as it was measured
 *   called NAME                   the source's record, where its witness is named file by file: its trunk is
 *                                 [record, its files' trunks] (file.c)
 *   after SOURCE...               the sources it comes after. The order of all the sources is the file "order" in
 *                                 the recipes directory: their names, one on a line
 *   reads FORMAT...               its files are ordinary content, read as these formats
 * A recipe of a source takes the source's witness, lineage and trust unless it names its own.
 *
 * A recipe is configuration. There is one decomposer (structure.c) and one reading of what a file's parts are
 * (say.c); a recipe says which files it reads and what their parts are, and nothing in the code names a format or a
 * source. A recipe file:
 *   name NAME
 *   match GLOB...                 files it applies to, by file name; a GLOB with a directory in it (annotated/train-*)
 *                                 by the end of the path
 *   grammar text | NAME           UAX #29 text, or a tree-sitter grammar loaded from $LAPLACE_GRAMMARS
 *   format NAME                   the grammar and what each kind of its nodes is, kept once for every recipe of that
 *                                 format (recipes/formats/NAME.format)
 *   tier, names, part, ...        the file laid out in tiers, outermost first (structure.h)
 *   content, key, refer, type, metadata, omit, own, thing, value, attest, relate, pair, holds, itself, voices, together,
 *   score, where, when, ...       what each named part of the file's tree is, and what the file attests (say.c)
 *   like RECIPE                   it reads as that recipe does, under its own name, witness, lineage and trust
 *   witness NAME...               the source as witness, named as content ({dir}: the directory the file is in;
 *                                 {name}: the file's own name, without what follows its last dot;
 *                                 {first NAME}: what the file first writes as NAME="...")
 *   lineage NAME...               the witness this one derives from, named as content. Copies of one lineage play one
 *                                 matchup per claim; each copy is still an attestation
 *   class NAME                    this recipe's witness's trust class, when it is not the source's
 * A recipe that says what a file's parts are is a curated source: what is recorded is the file's tree as its recipe
 * reads it, over its metadata tree, and what it attests. A recipe that says only a grammar records the file as its
 * syntax tree: each node the composition of its children with the bytes between them kept as text, so it recomposes
 * byte for byte. Leaves are text, decomposed by UAX #29. */
#define _GNU_SOURCE
#include "engine.h"
#include "os.h"
#include <tree_sitter/api.h>
#include <omp.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Ctx *ctx_here(void){ return CTX[omp_get_thread_num()]; }

/* A witness's trust is its class's prior (Sequence 6.5): the class is the only statement of it. A label the registry
 * does not declare, or a trust written as a number, stops the load: nothing is trusted by a figure someone typed. */
static double class_trust(const char *path, const char *label){
    const lp_trust_class *c = lp_trust_class_named(label);
    if (!c) { fprintf(stderr, "%s: class %s is not a trust class the registry declares (Laplace-Native/manifest/trust_classes.toml)\n", path, label ? label : "(none)"); exit(2); }
    return c->prior;
}
static void trust_by_number(const char *path, const char *directive){
    fprintf(stderr, "%s: %s: a witness's trust is its class's (class NAME, from Laplace-Native/manifest/trust_classes.toml), never a number\n", path, directive); exit(2);
}

/* ---- recipes */
static const TSLanguage *grammar_load(const char *name){
    const char *dir = laplace_grammars();
    char p[1024], sym[128]; snprintf(p, sizeof p, "%s/libtree-sitter-%s" OS_DLL_SUFFIX, dir, name); snprintf(sym, sizeof sym, "tree_sitter_%s", name);
    void *h = os_dl_open(p); if (!h) { fprintf(stderr, "grammar %s: %s\n", name, os_dl_error()); return NULL; }
    const TSLanguage *(*f)(void) = (const TSLanguage *(*)(void))os_dl_sym(h, sym);
    return f ? f() : NULL;
}
/* A path with $NAME read from the environment. */
static void path_expand(const char *in, char *out, size_t cap){
    size_t k = 0;
    for (const char *c = in; *c && k + 1 < cap; ) {
        if (*c != '$') { out[k++] = *c++; continue; }
        char name[64]; size_t l = 0; c++; while ((*c == '_' || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) && l + 1 < sizeof name) name[l++] = *c++; name[l] = 0;
        const char *v = getenv(name); if (!v && !strcmp(name, "LAPLACE_DATA")) v = "/vault/Data";
        if (!v && !strcmp(name, "LAPLACE_MODELS")) v = "/vault/models";
        if (v) k += (size_t)snprintf(out + k, cap - k, "%s", v);
    }
    out[k < cap ? k : cap - 1] = 0;
    os_slashes(out);                                                       /* one separator, whatever the environment wrote */
}
/* A recipe's comment: # where a line begins or after a space, outside a name written between double quotes ("#ISO"). */
void comment_off(char *line){
    int q = 0; for (char *c = line; *c; c++) { if (*c == '"') q = !q; else if (*c == '#' && !q && (c == line || c[-1] == ' ' || c[-1] == '\t')) { *c = 0; return; } }
}
static void rest_of(char *into, size_t cap){ char *rest = strtok(NULL, "\r\n"); if (!rest) return; while (*rest == ' ' || *rest == '\t') rest++; snprintf(into, cap, "%s", rest); }
static int recipe_parse(const char *path, Recipe *r){
    FILE *f = fopen(path, "r"); if (!f) return 0;
    memset(r, 0, sizeof *r); char line[4096];
    while (fgets(line, sizeof line, f)) {
        comment_off(line);
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "name")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->name, sizeof r->name, "%s", tok); }
        else if (!strcmp(tok, "match")) while ((tok = strtok(NULL, " \t\r\n")) && r->nmatch < 16) snprintf(r->match[r->nmatch++], 128, "%s", tok);
        else if (!strcmp(tok, "grammar")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->grammar, sizeof r->grammar, "%s", tok); }
        else if (!strcmp(tok, "class")) r->trust = class_trust(path, strtok(NULL, " \t\r\n"));
        else if (!strcmp(tok, "trust") || !strcmp(tok, "deviation")) trust_by_number(path, tok);
        else if (!strcmp(tok, "like")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->like, sizeof r->like, "%s", tok); }
        else if (!strcmp(tok, "witness")) rest_of(r->witness, sizeof r->witness);
        else if (!strcmp(tok, "lineage")) rest_of(r->lineage, sizeof r->lineage);
        else { int sv = say_says(r, path, tok);                             /* the file's layout and the disposition of its parts */
            if (!sv) fprintf(stderr, "%s: \"%s\" is not something a recipe says\n", path, tok);
            if (sv <= 0) { fclose(f); return 0; } }
    }
    fclose(f);
    if (r->like[0]) return 1;
    if (say_lays(r)) {                                                       /* configured: a grammar named gives the tree, else the layout's tiers do */
        if (r->grammar[0] && strcmp(r->grammar, "layout")) { r->lang = grammar_load(r->grammar); if (!r->lang) return 0; } else snprintf(r->grammar, sizeof r->grammar, "layout");
        r->curated = 1; return 1; }
    if (r->say && !say_only_disposes(r)) { fprintf(stderr, "%s: it says what parts are, but lays out no tree for them to be parts of (tier, format or node)\n", path); return 0; }
    if (!r->grammar[0]) snprintf(r->grammar, sizeof r->grammar, "text");
    if (strcmp(r->grammar, "text")) { r->lang = grammar_load(r->grammar); if (!r->lang) return 0; }
    return 1;
}
static Source *sources; static int nsources;
static int source_parse(const char *path, Source *s){
    FILE *f = fopen(path, "r"); if (!f) return 0;
    memset(s, 0, sizeof *s); char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h && (h == line || h[-1] == ' ' || h[-1] == '\t')) *h = 0;
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "name")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(s->name, sizeof s->name, "%s", tok); }
        else if (!strcmp(tok, "witness")) rest_of(s->witness, sizeof s->witness);
        else if (!strcmp(tok, "lineage")) rest_of(s->lineage, sizeof s->lineage);
        else if (!strcmp(tok, "class")) s->trust = class_trust(path, strtok(NULL, " \t\r\n"));
        else if (!strcmp(tok, "trust") || !strcmp(tok, "deviation")) trust_by_number(path, tok);
        else if (!strcmp(tok, "root")) { tok = strtok(NULL, " \t\r\n"); if (tok && s->nroot < 8) path_expand(tok, s->root[s->nroot++], 512); }
        else if (!strcmp(tok, "files")) { tok = strtok(NULL, " \t\r\n"); if (tok && s->nfiles < 8) path_expand(tok, s->files[s->nfiles++], 512); }
        else if (!strcmp(tok, "room")) { tok = strtok(NULL, " \t\r\n"); if (tok) s->room = atof(tok); }
        else if (!strcmp(tok, "tier0")) s->tier0 = 1;                       /* it records every codepoint, tier 0, as its ingest's first act */
        else if (!strcmp(tok, "called")) { char *v = strtok(NULL, "\r\n"); while (v && (*v == ' ' || *v == '\t')) v++; if (v) snprintf(s->called, sizeof s->called, "%s", v); }
        else if (!strcmp(tok, "after")) while ((tok = strtok(NULL, " \t\r\n")) && s->nafter < 16) snprintf(s->after[s->nafter++], 64, "%s", tok);
        else if (!strcmp(tok, "except")) while ((tok = strtok(NULL, " \t\r\n")) && s->nexcept < 8) snprintf(s->except[s->nexcept++], 128, "%s", tok);
        else if (!strcmp(tok, "reads")) while ((tok = strtok(NULL, " \t\r\n")) && s->nreads < 8) snprintf(s->reads[s->nreads++], 64, "%s", tok);
        else { fprintf(stderr, "%s: \"%s\" is not something a source says\n", path, tok); fclose(f); return 0; }
    }
    fclose(f);
    if (s->nfiles) { os_list g; os_glob(s->files[0], &g); if (g.n) snprintf(s->found, sizeof s->found, "%s", s->files[0]); os_list_free(&g); }
    for (int i = 0; i < s->nroot && !s->found[0]; i++) {                   /* the first root that exists; of a pattern, the newest */
        os_list g; os_glob(s->root[i], &g); if (g.n) snprintf(s->found, sizeof s->found, "%s", g.item[g.n - 1]);
        os_list_free(&g);
    }
    return s->name[0] != 0;
}
static int by_name(const void *a, const void *b){ return strcmp(*(char *const *)a, *(char *const *)b); }
static int recipes_in(const char *dir, int source, Recipe **out, int n){
    os_list ls; if (os_list_dir(dir, &ls)) { perror(dir); return n; }
    char **names = ls.item; int nn = (int)ls.n;
    qsort(names, (size_t)nn, sizeof(char *), by_name);
    for (int i = 0; i < nn; i++) {
        size_t l = strlen(names[i]); char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (l >= 8 && !strcmp(names[i] + l - 7, ".recipe")) {
            *out = xrealloc(*out, sizeof(Recipe) * (size_t)(n + 1)); Recipe *r = &(*out)[n];
            if (!recipe_parse(p, r)) { r->broken = 1; r->curated = 0; r->lang = NULL; if (!r->name[0]) snprintf(r->name, sizeof r->name, "%.*s", (int)(l - 7 < 63 ? l - 7 : 63), names[i]); }
            snprintf(r->file, sizeof r->file, "%s", p);
            r->source = source;
            if (source >= 0) { const Source *s = &sources[source];
                if (!r->witness[0]) snprintf(r->witness, sizeof r->witness, "%s", s->witness);
                if (!r->lineage[0]) snprintf(r->lineage, sizeof r->lineage, "%s", s->lineage);
                if (r->trust == 0.0) r->trust = s->trust; }
            n++;
        }
        else if (source < 0 && names[i][0] != '.') {                           /* a source: a directory with a source file */
            char sp[2100]; snprintf(sp, sizeof sp, "%s/source", p);
            if (!os_exists(sp)) { free(names[i]); continue; }
            sources = xrealloc(sources, sizeof(Source) * (size_t)(nsources + 1));
            if (!source_parse(sp, &sources[nsources])) { fprintf(stderr, "source %s does not load\n", sp); exit(2); }
            n = recipes_in(p, nsources++, out, n);
        }
        free(names[i]);
    }
    free(names); return n;
}
/* Sources in the order they go in. The file "order" in the recipes directory names them, one on a line, in the
 * order that grows what is known: what everything is written in, then languages, concepts, words, what is said
 * between them, and only then whole sentences and texts. A source still comes after every source it names
 * (after ...), and one the file does not name comes after those it does. */
static char order_dir[4096];
static void sources_order(void){
    int n = nsources ? nsources : 1;
    Source *o = malloc(sizeof(Source) * (size_t)n); int *at = malloc(sizeof(int) * (size_t)n), *done = calloc((size_t)n, sizeof(int)), *rank = malloc(sizeof(int) * (size_t)n), k = 0;
    for (int i = 0; i < nsources; i++) rank[i] = 1 << 20;
    { char p[4200], line[256]; snprintf(p, sizeof p, "%s/order", order_dir); FILE *f = fopen(p, "r"); int r = 0;
      while (f && fgets(line, sizeof line, f)) { char *c = line; while (*c == ' ' || *c == '\t') c++; if (*c == '#' || *c == '\n' || !*c) continue;
          char *e = c; while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++; *e = 0;
          int j = 0; while (j < nsources && strcmp(sources[j].name, c)) j++;
          if (j == nsources) { fprintf(stderr, "%s names %s, which is not a source\n", p, c); exit(2); }
          if (rank[j] == 1 << 20) rank[j] = r++; }
      if (f) fclose(f); }
    while (k < nsources) {
        int best = -1;
        for (int i = 0; i < nsources; i++) {
            if (done[i]) continue; int ready = 1;
            for (int a = 0; a < sources[i].nafter && ready; a++) { int j = 0; while (j < nsources && strcmp(sources[j].name, sources[i].after[a])) j++;
                if (j == nsources) { fprintf(stderr, "source %s comes after %s, which is not a source\n", sources[i].name, sources[i].after[a]); exit(2); }
                ready = done[j]; }
            if (ready && (best < 0 || rank[i] < rank[best])) best = i;
        }
        if (best < 0) { fprintf(stderr, "sources come after one another in a circle\n"); exit(2); }
        at[best] = k; o[k++] = sources[best]; done[best] = 1;
    }
    memcpy(sources, o, sizeof(Source) * (size_t)nsources); free(o); free(done); free(rank);
    extern Recipe *recipes_now; extern int nrecipes_now;
    for (int i = 0; i < nrecipes_now; i++) if (recipes_now[i].source >= 0) recipes_now[i].source = at[recipes_now[i].source];
    free(at);
}
Recipe *recipes_now; int nrecipes_now;
int recipes_load(const char *dir, Recipe **out){
    if (recipes_now) { *out = recipes_now; return nrecipes_now; }
    Recipe *r = NULL; int n = recipes_in(dir, -1, &r, 0);
    for (int i = 0; i < n; i++) if (r[i].like[0]) {                          /* it reads as another recipe does */
        int j = 0; while (j < n && strcmp(r[j].name, r[i].like)) j++;
        if (j == n || r[j].broken) { fprintf(stderr, "recipe %s reads like %s, which %s\n", r[i].name, r[i].like, j == n ? "is not a recipe" : "did not load"); r[i].broken = 1; r[i].like[0] = 0; continue; }
        Recipe me = r[i]; r[i] = r[j]; snprintf(r[i].file, sizeof r[i].file, "%s", me.file);
        snprintf(r[i].name, sizeof r[i].name, "%s", me.name); memcpy(r[i].match, me.match, sizeof me.match); r[i].nmatch = me.nmatch; r[i].source = me.source;
        memcpy(r[i].witness, me.witness, sizeof me.witness); memcpy(r[i].lineage, me.lineage, sizeof me.lineage); r[i].trust = me.trust; r[i].like[0] = 0;
    }
    recipes_now = r; nrecipes_now = n; snprintf(order_dir, sizeof order_dir, "%s", dir); sources_order();
    *out = r; return n;
}
Source *sources_loaded(int *n){ *n = nsources; return sources; }
static int reads(const Source *s, const Recipe *r){ for (int i = 0; i < s->nreads; i++) if (!strcmp(s->reads[i], r->name)) return 1; return 0; }
Recipe *recipe_for(Recipe *r, int n, const char *path, const Source *of){
    const char *b0 = strrchr(path, '/'); b0 = b0 ? b0 + 1 : path;
    char base[1024]; snprintf(base, sizeof base, "%s", b0);
    size_t bl = strlen(base); if (bl > 3 && !strcmp(base + bl - 3, ".gz")) base[bl - 3] = 0;   /* matched by what it holds */
    Recipe *best = NULL; size_t best_lit = 0;                              /* the most specific pattern wins */
    for (int i = 0; of && i < of->nexcept; i++) if (!os_fnmatch(of->except[i], path)) return NULL;
    for (int i = 0; i < n; i++) {
        if (r[i].broken) continue;
        if (of && !(r[i].source >= 0 && &sources[r[i].source] == of) && !(r[i].source < 0 && reads(of, &r[i]))) continue;
        for (int j = 0; j < r[i].nmatch; j++) {
            if (strchr(r[i].match[j], '/')) {                               /* a pattern with a directory in it: matched against the end of the path */
                char pat[300], whole[1024]; snprintf(pat, sizeof pat, "*/%s", r[i].match[j]); snprintf(whole, sizeof whole, "%s", path);
                size_t wl = strlen(whole); if (wl > 3 && !strcmp(whole + wl - 3, ".gz")) whole[wl - 3] = 0;
                if (os_fnmatch(pat, whole)) continue; }
            else if (os_fnmatch(r[i].match[j], base)) continue;
            size_t lit = 0; for (const char *c = r[i].match[j]; *c; c++) lit += !strchr("*?[]", *c);
            if (!best || lit > best_lit) { best = &r[i]; best_lit = lit; }
        }
    }
    return best;
}

/* A recipe that did not load stops the source it belongs to, so its files are never read as something else; it stops
 * no other source. */
int recipes_broken(const Recipe *r, int n, const Source *of){
    int k = 0;
    for (int i = 0; i < n; i++) { if (!r[i].broken) continue;
        int its = of ? ((r[i].source >= 0 && &sources[r[i].source] == of) || (r[i].source < 0 && reads(of, &r[i]))) : r[i].source < 0;
        if (its) { fprintf(stderr, "recipe %s did not load: %s is not read without it\n", r[i].file, of ? of->name : "a file"); k++; } }
    return k;
}

/* ---- strings to entities, decomposed once per thread: a string map from the text to its entity, emptied with the table */
typedef struct { lp_strmap *m; uint64_t epoch; } SCache;
static __thread SCache sc;
static uint64_t strings_epoch;                                           /* raised when the node table is emptied: what was remembered is of the table before */
void strings_forget(void){ __atomic_add_fetch(&strings_epoch, 1, __ATOMIC_RELAXED); }
Ref string_ref(const uint8_t *s, size_t n){
    if (!sc.m || sc.epoch != strings_epoch) { lp_strmap_free(sc.m); sc.m = lp_strmap_sized(sizeof(Ref)); sc.epoch = strings_epoch; }
    bool fresh; Ref *r = lp_strmap_get(sc.m, s, n, &fresh);
    if (fresh) *r = text_ref(ctx_here(), s, n);
    return *r;
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
    uint32_t k = 0;
    for (uint32_t i = 0; i <= 2 * nc; i++) if (has[i]) kids[k++] = kids[i];
    Ref r = compose(kids, k, ref_above(kids, k));
    free(kids); free(cs); free(has); free(cn);
    return r;
}

/* ---- what a text writes for what it cannot write plainly: XML references resolved */
size_t xml_unescape(const uint8_t *s, size_t n, uint8_t *o){
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
        k += lp_utf8_put(cp, o + k);
        i = j + 1;
    }
    return k;
}
void ev_push(Events *e, const Event *x){                         /* by address: a copy per call, in a loop, is stack that is never given back */
    /* From 64, doubling. Every file keeps its list until its batch is written, and a list begun at 65,536 events was
     * 4.7 MB a file however few it held: untouched pages cost nothing on Linux, but Windows charges them to the
     * commit limit, and PropBank's 7,568 files held 32 GB of it (FrameNet's 14,930 ran out: realloc: Not enough
     * space, 2026-10-06). */
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 64; e->e = xrealloc(e->e, e->cap * sizeof(Event)); }
    e->e[e->n++] = *x;
}

/* ---- a source's keys across its files: what a key column holds, resolved to its row's subject, for the rows of the
 * source's other files that point at it (refer COLUMN RECIPE). Process-wide, in stripes: a source is read in one process,
 * and the files that refer are read after the files referred to (ingest orders them). The first row to define a key keeps it:
 * first in the order of the files and of the rows in them (KeyRank), not in the order the threads reach them. */
/* A stripe is a string map, its key the recipe's name, a NUL, and the key's bytes; the stripe is the key's hash's top bits. */
typedef struct { pthread_mutex_t mu; lp_strmap *m; } KStripe;
static KStripe kstripe[64] = { [0 ... 63] = { PTHREAD_MUTEX_INITIALIZER, NULL } };
static KStripe *key_at(const char *recipe, const uint8_t *k, size_t n, uint8_t **key, size_t *len, uint8_t *stack, size_t cap){
    size_t rl = strlen(recipe); *len = rl + 1 + n; *key = *len <= cap ? stack : malloc(*len);
    memcpy(*key, recipe, rl + 1); memcpy(*key + rl + 1, k, n);
    return &kstripe[(lp_hash_bytes(*key, *len) >> 58) & 63];
}
typedef struct { Ref x; KeyRank r; } KeyHeld;
static int rank_before(const KeyRank *a, const KeyRank *b){ return a->file != b->file ? a->file < b->file : a->at != b->at ? a->at < b->at : a->unit < b->unit; }
void keys_put(const char *recipe, const uint8_t *k, size_t n, Ref x, const KeyRank *rank){
    uint8_t stack[512], *key; size_t len; KStripe *s = key_at(recipe, k, n, &key, &len, stack, sizeof stack);
    pthread_mutex_lock(&s->mu); if (!s->m) s->m = lp_strmap_sized(sizeof(KeyHeld));
    bool fresh; KeyHeld *h = lp_strmap_get(s->m, key, len, &fresh); if (fresh || rank_before(rank, &h->r)) { h->x = x; h->r = *rank; }     /* the first row to define a key keeps it */
    pthread_mutex_unlock(&s->mu); if (key != stack) free(key);
}
int keys_get(const char *recipe, const uint8_t *k, size_t n, Ref *out){
    uint8_t stack[512], *key; size_t len; KStripe *s = key_at(recipe, k, n, &key, &len, stack, sizeof stack);
    pthread_mutex_lock(&s->mu); const KeyHeld *r = s->m ? lp_strmap_lookup(s->m, key, len) : NULL; if (r) *out = r->x;
    pthread_mutex_unlock(&s->mu); if (key != stack) free(key); return r != NULL;
}
uint64_t keys_held(void){ uint64_t n = 0; for (int i = 0; i < 64; i++) n += lp_strmap_count(kstripe[i].m); return n; }

/* ---- one file */
/* A file's bytes; gzip is read through zlib, so a recipe sees what the container holds. */
static uint8_t *read_all(const char *path, size_t *n){
    size_t l = strlen(path);
    if (l > 3 && !strcmp(path + l - 3, ".gz")) {
        gzFile g = gzopen(path, "rb"); if (!g) return NULL; gzbuffer(g, 1 << 20);
        size_t cap = 1 << 24, k = 0; uint8_t *b = malloc(cap); int r;
        while ((r = gzread(g, b + k, (unsigned)(cap - k))) > 0) { k += (size_t)r; if (k == cap) { cap *= 2; b = xrealloc(b, cap); } }
        gzclose(g); if (r < 0) { free(b); return NULL; } *n = k; return b;
    }
    FILE *fp = fopen(path, "rb"); if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); size_t m = (size_t)ftell(fp); rewind(fp);
    uint8_t *b = malloc(m + 1); if (fread(b, 1, m, fp) != m) { fclose(fp); free(b); return NULL; }
    fclose(fp); *n = m; return b;
}
/* A witness's name, with {dir} as the directory the file is in. */
static void named_for(const char *name, const char *path, const uint8_t *src, size_t n, char *out, size_t cap){
    const char *fa = strstr(name, "{first ");
    if (fa) {                                                              /* what the file first writes as NAME="..." */
        const char *fe = strchr(fa, '}'); char key[72]; int kl = snprintf(key, sizeof key, " %.*s=\"", fe ? (int)(fe - fa - 7) : 0, fa + 7);
        const uint8_t *v = fe ? memmem(src, n, key, (size_t)kl) : NULL, *q = v ? memchr(v + kl, '"', n - (size_t)(v + kl - src)) : NULL;
        uint8_t val[400]; size_t vl = 0; if (q && (size_t)(q - v - kl) < 200) vl = xml_unescape(v + kl, (size_t)(q - v - kl), val);
        snprintf(out, cap, "%.*s%.*s%s", (int)(fa - name), name, (int)vl, (const char *)val, fe ? fe + 1 : ""); return;
    }
    const char *nm = strstr(name, "{name}");
    if (nm) {                                                              /* the file's own name, without what follows its last dot */
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path; const char *d = strrchr(b, '.'); size_t l = d && d > b ? (size_t)(d - b) : strlen(b);
        snprintf(out, cap, "%.*s%.*s%s", (int)(nm - name), name, (int)l, b, nm + 6); return;
    }
    const char *at = strstr(name, "{dir}"); if (!at) { snprintf(out, cap, "%s", name); return; }
    const char *e = strrchr(path, '/'), *s = e; while (s && s > path && s[-1] != '/') s--;
    snprintf(out, cap, "%.*s%.*s%s", (int)(at - name), name, e ? (int)(e - s) : 0, e ? s : "", at + 5);
}
/* A file read for the highway: what its recipe says of its types (laplace highway). */
void highway_file(Ctx *c, File *f, Hw *hw){
    size_t n; uint8_t *src0 = read_all(f->path, &n); if (!src0) { perror(f->path); return; } const Recipe *r = f->recipe; uint8_t *src = src0;
    if (n >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) { src += 3; n -= 3; }
    char w[512]; named_for(r->witness[0] ? r->witness : r->name, f->path, src, n, w, sizeof w); f->witness = text_ref(c, (const uint8_t *)w, strlen(w));
    say_highway(r, f, src, n, hw); free(src0);
}
int reads_in_stretches(const Recipe *r, char *boundary){
    if (!r || !r->curated) return 0;
    int b = say_stretches(r); if (!b) return 0; *boundary = (char)b; return 1;      /* by its outermost tier */
}
void decompose_file(Ctx *c, File *f){
    size_t n; uint8_t *src0 = read_all(f->path, &n); if (!src0) { f->skipped = 1; return; }
    f->bytes = n; decompose_bytes(c, f, src0, n, 1); free(src0);
}
void decompose_bytes(Ctx *c, File *f, uint8_t *src, size_t n, int first){
    const Recipe *r = f->recipe;
    if (r && r->curated) {
        if (first && n >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) { src += 3; n -= 3; }   /* a curated source's byte order mark is how it was written down, not what it says */
        if (first) {                                                         /* the witness, named as content */
            char w[512], l[512]; named_for(r->witness[0] ? r->witness : r->name, f->path, src, n, w, sizeof w); named_for(r->lineage, f->path, src, n, l, sizeof l);
            f->witness = text_ref(c, (const uint8_t *)w, strlen(w)); f->trunk = f->witness;
            if (l[0]) { f->lineage = text_ref(c, (const uint8_t *)l, strlen(l)); f->has_lineage = 1; } }
        attest_layout(r, f, src, n); return; }
    if (!r || !r->lang) { f->trunk = text_ref(c, src, n); return; }        /* text: UAX #29 */
    TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, r->lang);     /* content: the file as its syntax tree, byte for byte */
    TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
    f->trunk = ast_node(ts_tree_root_node(t), src, 0, (uint32_t)n, 0);
    ts_tree_delete(t); ts_parser_delete(ps);
}

/* ---- laplace tree: a file's syntax tree as its recipe's grammar reads it, for writing recipes
 *   laplace tree [-r recipes] [-g grammar] [-n nodes] file */
static void tree_print(TSTreeCursor *cur, const uint8_t *src, int depth, long *left){
    do {
        if (*left <= 0) return;
        TSNode nd = ts_tree_cursor_current_node(cur); const char *field = ts_tree_cursor_current_field_name(cur);
        uint32_t a = ts_node_start_byte(nd), b = ts_node_end_byte(nd);
        if (ts_node_is_named(nd) || ts_node_child_count(nd)) {
            (*left)--; printf("%*s", depth * 2, ""); if (field) printf("%s: ", field);
            printf("(%s) [%u, %u]", ts_node_type(nd), a, b);
            if (!ts_node_child_count(nd)) { printf("  "); fwrite(src + a, 1, b - a > 60 ? 60 : b - a, stdout); if (b - a > 60) printf("\xE2\x80\xA6"); }
            putchar('\n');
        }
        if (ts_tree_cursor_goto_first_child(cur)) { tree_print(cur, src, depth + 1, left); ts_tree_cursor_goto_parent(cur); }
    } while (ts_tree_cursor_goto_next_sibling(cur));
}
int cmd_tree(int argc, char **argv){
    const char *rdir = laplace_recipes(), *grammar = NULL; long long nodes_opt = 400;
    int a = opts(argc, argv, (const Opt[]){ { "-r", 's', &rdir }, { "-g", 's', &grammar }, { "-n", 'l', &nodes_opt }, { NULL } });
    long nodes = (long)nodes_opt;
    if (a >= argc) { fprintf(stderr, "usage: laplace tree [-r recipes] [-g grammar] [-n nodes] file\n"); return 2; }
    const TSLanguage *lang = NULL;
    if (grammar) lang = grammar_load(grammar);
    else { Recipe *rec = NULL; int n = recipes_load(rdir, &rec); Recipe *r = recipe_for(rec, n, argv[a], NULL);
           if (r) { lang = r->lang; printf("recipe %s, grammar %s\n", r->name, r->grammar); } }
    if (!lang) { fprintf(stderr, "no tree-sitter grammar reads %s (name one with -g)\n", argv[a]); return 1; }
    size_t n; uint8_t *src = read_all(argv[a], &n); if (!src) { perror(argv[a]); return 1; }
    TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, lang);
    TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
    TSTreeCursor cur = ts_tree_cursor_new(ts_tree_root_node(t));
    tree_print(&cur, src, 0, &nodes);
    if (nodes <= 0) printf("\xE2\x80\xA6 (more: -n)\n");
    ts_tree_cursor_delete(&cur); ts_tree_delete(t); ts_parser_delete(ps); free(src);
    return 0;
}
