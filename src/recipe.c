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
 *   held REASON                   a source its keeper has not supplied yet: when it is absent, a plan says so, with the reason, and does not fail
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
 *   witness NAME...               the source's record, named as content: its trunk [record, its files' trunks] is the
 *                                 witness. In a recipe, the name its voices are composed under ({dir}: the directory the
 *                                 file is in; {name}: the file's own name, without what follows its last dot;
 *                                 {first NAME}: what the file first writes as NAME="...")
 *   lineage NAME...               the witness this one derives from, named as content: the source's, kept beside its
 *                                 trunk in witness (a recipe's own is said and not used)
 *   class NAME                    a trust class; the source's is its trunk's, and every claim it says plays at it
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
/* A path with $NAME read from the environment. Where the data is kept is the machine's to say (Laplace-Operations'
 * laplace.env: LAPLACE_DATA, LAPLACE_MODELS), never the code's: a variable the environment does not set, or sets
 * empty, resolves to nothing, and its name is given back (unset) so what needs the path can say so and stop. */
static int path_expand(const char *in, char *out, size_t cap, char *unset, size_t ucap){
    size_t k = 0; int ok = 1;
    for (const char *c = in; *c && k + 1 < cap; ) {
        if (*c != '$') { out[k++] = *c++; continue; }
        char name[64]; size_t l = 0; c++; while ((*c == '_' || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) && l + 1 < sizeof name) name[l++] = *c++; name[l] = 0;
        const char *v = getenv(name);
        if (!v || !*v) { if (ok && unset) snprintf(unset, ucap, "%s", name); ok = 0; continue; }
        k += (size_t)snprintf(out + k, cap - k, "%s", v);
    }
    out[k < cap ? k : cap - 1] = 0;
    os_slashes(out);                                                       /* one separator, whatever the environment wrote */
    return ok;
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
        else if (!strcmp(tok, "grammar")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->grammar, sizeof r->grammar, "%s", tok); r->native = native_grammar(r->grammar); }
        else if (!strcmp(tok, "class")) r->trust = class_trust(path, strtok(NULL, " \t\r\n"));
        else if (!strcmp(tok, "trust") || !strcmp(tok, "deviation")) trust_by_number(path, tok);
        else if (!strcmp(tok, "like")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->like, sizeof r->like, "%s", tok); }
        else if (!strcmp(tok, "witness")) rest_of(r->witness, sizeof r->witness);
        else if (!strcmp(tok, "lineage")) rest_of(r->lineage, sizeof r->lineage);
        else { int sv = r->native ? r->native->says(r, path, tok)           /* a line of the native grammar the recipe names */
                              : say_says(r, path, tok);                     /* the file's layout and the disposition of its parts */
            if (!sv) fprintf(stderr, "%s: \"%s\" is not something a recipe says\n", path, tok);
            if (sv <= 0) { fclose(f); return 0; } }
    }
    fclose(f);
    if (r->like[0]) return 1;
    if (r->native) return r->native->done(r, path);                          /* a native grammar reads its files itself */
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
        else if (!strcmp(tok, "root")) { tok = strtok(NULL, " \t\r\n"); if (tok && s->nroot < 8 && !path_expand(tok, s->root[s->nroot++], 512, s->unset, sizeof s->unset)) s->nroot--; }
        else if (!strcmp(tok, "files")) { tok = strtok(NULL, " \t\r\n"); if (tok && s->nfiles < 8 && !path_expand(tok, s->files[s->nfiles++], 512, s->unset, sizeof s->unset)) s->nfiles--; }
        else if (!strcmp(tok, "room")) { tok = strtok(NULL, " \t\r\n"); if (tok) s->room = atof(tok); }
        else if (!strcmp(tok, "tier0")) s->tier0 = 1;                       /* it records every codepoint, tier 0, as its ingest's first act */
        else if (!strcmp(tok, "held")) rest_of(s->held, sizeof s->held);
        else if (!strcmp(tok, "called")) { char *v = strtok(NULL, "\r\n"); while (v && (*v == ' ' || *v == '\t')) v++; if (v) snprintf(s->called, sizeof s->called, "%s", v); }
        else if (!strcmp(tok, "after")) while ((tok = strtok(NULL, " \t\r\n")) && s->nafter < 16) snprintf(s->after[s->nafter++], 64, "%s", tok);
        else if (!strcmp(tok, "except")) while ((tok = strtok(NULL, " \t\r\n")) && s->nexcept < 8) snprintf(s->except[s->nexcept++], 128, "%s", tok);
        else if (!strcmp(tok, "reads")) while ((tok = strtok(NULL, " \t\r\n")) && s->nreads < 8) snprintf(s->reads[s->nreads++], 64, "%s", tok);
        else { fprintf(stderr, "%s: \"%s\" is not something a source says\n", path, tok); fclose(f); return 0; }
    }
    fclose(f);
    if (s->unset[0]) { s->nroot = s->nfiles = 0; return s->name[0] != 0; }   /* a root it names is not set: it is at none, and says why (source_unset) */
    if (s->nfiles) { os_list g; os_glob(s->files[0], &g); if (g.n) snprintf(s->found, sizeof s->found, "%s", s->files[0]); os_list_free(&g); }
    /* the first root that is there; of a pattern, the newest directory: an archive or a checksum kept beside the
     * directory it was opened into matches the pattern too, and is not where the source is */
    for (int i = 0; i < s->nroot && !s->found[0]; i++) {
        os_list g; os_glob(s->root[i], &g);
        for (size_t j = g.n; j > 0; j--) if (os_is_dir(g.item[j - 1]) && !s->nfound++) snprintf(s->found, sizeof s->found, "%s", g.item[j - 1]);
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
static const struct { const char *name, *grammar, *match[8]; } generic_specs[] = {
    { "csv",        "csv",        { "*.csv", NULL } },
    { "tsv",        "tsv",        { "*.tsv", "*.tab", NULL } },
    { "psv",        "psv",        { "*.psv", NULL } },
    { "html",       "html",       { "*.html", "*.htm", NULL } },
    { "c",          "c",          { "*.c", "*.h", NULL } },
    { "cpp",        "cpp",        { "*.cpp", "*.cc", "*.cxx", "*.hpp", "*.hh", "*.hxx", NULL } },
    { "c_sharp",    "c_sharp",    { "*.cs", NULL } },
    { "python",     "python",     { "*.py", "*.pyi", NULL } },
    { "javascript", "javascript", { "*.js", "*.mjs", "*.cjs", "*.jsx", NULL } },
    { "typescript", "typescript", { "*.ts", "*.mts", "*.cts", NULL } },
    { "tsx",        "tsx",        { "*.tsx", NULL } },
    { "rust",       "rust",       { "*.rs", NULL } },
    { "go",         "go",         { "*.go", NULL } },
    { "java",       "java",       { "*.java", NULL } },
    { "kotlin",     "kotlin",     { "*.kt", "*.kts", NULL } },
    { "ruby",       "ruby",       { "*.rb", NULL } },
    { "php",        "php",        { "*.php", NULL } },
    { "bash",       "bash",       { "*.sh", "*.bash", "*.zsh", NULL } },
    { "cmake",      "cmake",      { "*.cmake", "CMakeLists.txt", NULL } },
    { "cuda",       "cuda",       { "*.cu", "*.cuh", NULL } },
    { "fortran",    "fortran",    { "*.f", "*.f90", "*.f95", "*.for", NULL } },
    { "glsl",       "glsl",       { "*.glsl", "*.vert", "*.frag", "*.comp", NULL } },
    { "hlsl",       "hlsl",       { "*.hlsl", "*.fx", NULL } },
    { "ispc",       "ispc",       { "*.ispc", NULL } },
    { "julia",      "julia",      { "*.jl", NULL } },
    { "llvm",       "llvm",       { "*.ll", NULL } },
    { "mlir",       "mlir",       { "*.mlir", NULL } },
    { "nasm",       "nasm",       { "*.nasm", "*.asm", "*.s", NULL } },
    { "wgsl",       "wgsl",       { "*.wgsl", NULL } },
    { "zig",        "zig",        { "*.zig", NULL } },
    { "dtd",        "dtd",        { "*.dtd", NULL } },
};
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
    size_t ngen = sizeof(generic_specs) / sizeof(generic_specs[0]);
    for (size_t g = 0; g < ngen; g++) {
        int exists = 0; for (int i = 0; i < n && !exists; i++) if (r[i].source < 0 && !strcmp(r[i].name, generic_specs[g].name)) exists = 1;
        if (exists) continue;
        r = xrealloc(r, sizeof(Recipe) * (size_t)(n + 1)); Recipe *x = &r[n++]; memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", generic_specs[g].name);
        snprintf(x->file, sizeof x->file, "<generic:%s>", generic_specs[g].name);
        snprintf(x->grammar, sizeof x->grammar, "%s", generic_specs[g].grammar);
        for (int m = 0; generic_specs[g].match[m] && x->nmatch < 16; m++) snprintf(x->match[x->nmatch++], 256, "%s", generic_specs[g].match[m]);
        x->source = -1; x->trust = 0.60; x->curated = 0; x->lang = NULL;
    }
    for (int i = 0; i < n; i++) if (r[i].source < 0 && r[i].trust == 0.0) r[i].trust = 0.60;
    recipes_now = r; nrecipes_now = n; snprintf(order_dir, sizeof order_dir, "%s", dir); sources_order();
    *out = r; return n;
}
Source *sources_loaded(int *n){ *n = nsources; return sources; }
static int reads(const Source *s, const Recipe *r){ for (int i = 0; i < s->nreads; i++) if (!strcmp(s->reads[i], r->name)) return 1; return 0; }
static int under_source_root(const Source *s, const char *path){
    if (!s->found[0]) return 0;
    size_t fl = strlen(s->found);
    if (strncmp(path, s->found, fl) != 0) return 0;
    return path[fl] == 0 || path[fl] == '/' || path[fl] == '\\';
}
static Recipe *by_recipe_name(Recipe *r, int n, const char *name){
    for (int i = 0; i < n; i++) if (r[i].source < 0 && !r[i].broken && !strcmp(r[i].name, name)) return &r[i];
    return NULL;
}
static Recipe *sniff_generic_recipe(Recipe *r, int n, const char *path){
    FILE *fp = fopen(path, "rb"); if (!fp) return NULL;
    uint8_t buf[4096]; size_t m = fread(buf, 1, sizeof buf, fp); fclose(fp);
    if (!m) return by_recipe_name(r, n, "text");
    for (size_t i = 0; i < m; i++) if (buf[i] == 0) return NULL;             /* binary file with NUL bytes */
    size_t i = 0; if (m >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) i = 3;
    while (i < m && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
    if (i < m) {
        if (buf[i] == '{' || buf[i] == '[') return by_recipe_name(r, n, "json");
        if (m - i >= 5 && !strncasecmp((const char *)buf + i, "<?xml", 5)) return by_recipe_name(r, n, "xml");
        if (m - i >= 9 && !strncasecmp((const char *)buf + i, "<!doctype", 9)) return by_recipe_name(r, n, "html");
        if (m - i >= 5 && !strncasecmp((const char *)buf + i, "<html", 5)) return by_recipe_name(r, n, "html");
    }
    return by_recipe_name(r, n, "text");
}
Recipe *recipe_for(Recipe *r, int n, const char *path, const Source *of){
    const char *b0 = strrchr(path, '/'); b0 = b0 ? b0 + 1 : path;
    char base[1024]; snprintf(base, sizeof base, "%s", b0);
    size_t bl = strlen(base); if (bl > 3 && !strcmp(base + bl - 3, ".gz")) base[bl - 3] = 0;   /* matched by what it holds */
    if (!of) for (int s = 0; s < nsources; s++) if (under_source_root(&sources[s], path)) { of = &sources[s]; break; }
    Recipe *best = NULL; size_t best_lit = 0;                              /* the most specific pattern wins */
    for (int i = 0; of && i < of->nexcept; i++) if (!os_fnmatch(of->except[i], path)) return NULL;
    for (int i = 0; i < n; i++) {
        if (r[i].broken) continue;
        if (of) { if (!(r[i].source >= 0 && &sources[r[i].source] == of) && !(r[i].source < 0 && reads(of, &r[i]))) continue; }
        else if (r[i].source >= 0) continue;                               /* user file outside any seeded corpus root: generic recipes only */
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
    if (!best && !of) best = sniff_generic_recipe(r, n, path);
    if (best && !best->curated && best->grammar[0] && strcmp(best->grammar, "text") != 0 && !best->lang)
        if (strcmp(best->grammar, "csv") != 0 && strcmp(best->grammar, "tsv") != 0 && strcmp(best->grammar, "psv") != 0)
            best->lang = grammar_load(best->grammar);
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
/* ---- Generic structural attestation extraction for user uploads & non-curated files ---- */
typedef struct { Ref *claims; size_t n, cap; } GenRec;
static void gen_claim_add(GenRec *gr, File *f, const Ref *parts, size_t np){
    if (np < 2) return;
    Ref p[8]; if (np > 8) np = 8;
    for (size_t i = 0; i < np; i++) { if (!parts[i].tier) return; p[i] = parts[i]; p[i].said = 0; }
    uint32_t pos = gr->n < 254 ? (uint32_t)(gr->n + 1) : 255;
    Ref cl = compose(p, (uint32_t)np, ref_above(p, np));
    Event ev = { .claim = cl.id, .witnessed = cl.id, .score = 1.0f, .kind = EV_CLAIM, .run = 1, .position = pos };
    ev_push(&f->ev, &ev);
    cl.said = LP_SAID_CLAIM; cl.outcome = (uint8_t)lp_outcome_of(1.0, &cl.spare); cl.position = pos;
    if (gr->n == gr->cap) { gr->cap = gr->cap ? gr->cap * 2 : 16; gr->claims = xrealloc(gr->claims, gr->cap * sizeof(Ref)); }
    gr->claims[gr->n++] = cl;
}
static void gen_record_flush(GenRec *gr, File *f){
    if (!gr->n) return;
    Ref rec = gr->n == 1 ? gr->claims[0] : compose(gr->claims, (uint32_t)gr->n, ref_above(gr->claims, gr->n));
    rec.said = LP_SAID_RECORD; if (gr->n > 1) { rec.outcome = 0; rec.position = 0; rec.spare = 0; }
    if (f->nsaid == f->csaid) { f->csaid = f->csaid ? f->csaid * 2 : 64; f->said = xrealloc(f->said, f->csaid * sizeof(Ref)); }
    f->said[f->nsaid++] = rec;
    f->records++;
    gr->n = 0;
}
static Ref span_trim_ref(const uint8_t *s, size_t len, int strip_quotes){
    while (len > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n')) { s++; len--; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
    if (strip_quotes && len >= 2 && ((s[0] == '"' && s[len - 1] == '"') || (s[0] == '\'' && s[len - 1] == '\''))) { s++; len -= 2; }
    if (!len) return (Ref){0};
    return string_ref(s, len);
}
static int is_id_key(const uint8_t *s, size_t n){
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) { s++; n -= 2; }
    return (n == 2 && !strncasecmp((const char *)s, "id", 2)) ||
           (n == 3 && (!strncasecmp((const char *)s, "key", 3) || !strncasecmp((const char *)s, "uri", 3) || !strncasecmp((const char *)s, "uid", 3))) ||
           (n == 4 && (!strncasecmp((const char *)s, "name", 4) || !strncasecmp((const char *)s, "code", 4) || !strncasecmp((const char *)s, "slug", 4) || !strncasecmp((const char *)s, "uuid", 4))) ||
           (n == 5 && (!strncasecmp((const char *)s, "title", 5) || !strncasecmp((const char *)s, "label", 5) || !strncasecmp((const char *)s, "about", 5))) ||
           (n == 7 && !strncasecmp((const char *)s, "subject", 7));
}
static int json_scalar_ref(TSNode v, const uint8_t *src, Ref *out){
    if (ts_node_is_null(v)) return 0;
    const char *tp = ts_node_type(v);
    if (!strcmp(tp, "string") || !strcmp(tp, "number") || !strcmp(tp, "true") || !strcmp(tp, "false")) {
        uint32_t a = ts_node_start_byte(v), b = ts_node_end_byte(v);
        *out = span_trim_ref(src + a, b - a, 1);
        return out->tier != 0;
    }
    return 0;
}
static void attest_json_node(TSNode nd, const uint8_t *src, File *f, GenRec *gr){
    const char *tp = ts_node_type(nd);
    if (!strcmp(tp, "object")) {
        uint32_t nc = ts_node_named_child_count(nd);
        Ref subj = {0}; int has_subj = 0;
        for (uint32_t i = 0; i < nc && !has_subj; i++) {
            TSNode ch = ts_node_named_child(nd, i);
            if (strcmp(ts_node_type(ch), "pair")) continue;
            TSNode k = ts_node_child_by_field_name(ch, "key", 3);
            TSNode v = ts_node_child_by_field_name(ch, "value", 5);
            if (ts_node_is_null(k) || ts_node_is_null(v)) continue;
            uint32_t ka = ts_node_start_byte(k), kb = ts_node_end_byte(k);
            if (is_id_key(src + ka, kb - ka) && json_scalar_ref(v, src, &subj)) has_subj = 1;
        }
        gr->n = 0;
        for (uint32_t i = 0; i < nc; i++) {
            TSNode ch = ts_node_named_child(nd, i);
            if (strcmp(ts_node_type(ch), "pair")) continue;
            TSNode k = ts_node_child_by_field_name(ch, "key", 3);
            TSNode v = ts_node_child_by_field_name(ch, "value", 5);
            if (ts_node_is_null(k) || ts_node_is_null(v)) continue;
            Ref kr = span_trim_ref(src + ts_node_start_byte(k), ts_node_end_byte(k) - ts_node_start_byte(k), 1);
            Ref vr = {0};
            if (json_scalar_ref(v, src, &vr)) {
                if (has_subj && !lp_id_eq(&subj.id, &vr.id)) gen_claim_add(gr, f, (Ref[]){ subj, kr, vr }, 3);
                else gen_claim_add(gr, f, (Ref[]){ kr, vr }, 2);
            } else if (!strcmp(ts_node_type(v), "array")) {
                uint32_t ac = ts_node_named_child_count(v);
                for (uint32_t j = 0; j < ac; j++) {
                    Ref avr = {0};
                    if (json_scalar_ref(ts_node_named_child(v, j), src, &avr)) {
                        if (has_subj) gen_claim_add(gr, f, (Ref[]){ subj, kr, avr }, 3);
                        else gen_claim_add(gr, f, (Ref[]){ kr, avr }, 2);
                    }
                }
            }
        }
        gen_record_flush(gr, f);
    }
    uint32_t nc = ts_node_named_child_count(nd);
    for (uint32_t i = 0; i < nc; i++) attest_json_node(ts_node_named_child(nd, i), src, f, gr);
}
static void attest_xml_node(TSNode nd, const uint8_t *src, File *f, GenRec *gr, Ref parent_subj){
    const char *tp = ts_node_type(nd);
    Ref next_subj = parent_subj;
    if (!strcmp(tp, "element")) {
        uint32_t nc = ts_node_named_child_count(nd);
        TSNode tag_parent = {0}; Ref tag_ref = {0}, subj_ref = {0}; int has_subj = 0;
        for (uint32_t i = 0; i < nc; i++) {
            TSNode ch = ts_node_named_child(nd, i); const char *ctp = ts_node_type(ch);
            if (!strcmp(ctp, "STag") || !strcmp(ctp, "EmptyElemTag") || !strcmp(ctp, "start_tag") || !strcmp(ctp, "self_closing_tag")) { tag_parent = ch; break; }
        }
        if (!ts_node_is_null(tag_parent)) {
            uint32_t tc = ts_node_named_child_count(tag_parent);
            for (uint32_t i = 0; i < tc; i++) {
                TSNode ch = ts_node_named_child(tag_parent, i); const char *ctp = ts_node_type(ch);
                if (!tag_ref.tier && (!strcmp(ctp, "Name") || !strcmp(ctp, "tag_name")))
                    tag_ref = span_trim_ref(src + ts_node_start_byte(ch), ts_node_end_byte(ch) - ts_node_start_byte(ch), 0);
                else if (!strcmp(ctp, "Attribute") || !strcmp(ctp, "attribute")) {
                    uint32_t ac = ts_node_named_child_count(ch);
                    if (ac >= 2) {
                        TSNode ak = ts_node_named_child(ch, 0), av = ts_node_named_child(ch, 1);
                        uint32_t ka = ts_node_start_byte(ak), kb = ts_node_end_byte(ak);
                        if (!has_subj && is_id_key(src + ka, kb - ka)) {
                            subj_ref = span_trim_ref(src + ts_node_start_byte(av), ts_node_end_byte(av) - ts_node_start_byte(av), 1);
                            if (subj_ref.tier) has_subj = 1;
                        }
                    }
                }
            }
            if (has_subj) next_subj = subj_ref;
            gr->n = 0;
            Ref head = has_subj ? subj_ref : tag_ref;
            for (uint32_t i = 0; i < tc; i++) {
                TSNode ch = ts_node_named_child(tag_parent, i); const char *ctp = ts_node_type(ch);
                if (!strcmp(ctp, "Attribute") || !strcmp(ctp, "attribute")) {
                    uint32_t ac = ts_node_named_child_count(ch);
                    if (ac >= 2) {
                        TSNode ak = ts_node_named_child(ch, 0), av = ts_node_named_child(ch, 1);
                        Ref kr = span_trim_ref(src + ts_node_start_byte(ak), ts_node_end_byte(ak) - ts_node_start_byte(ak), 0);
                        Ref vr = span_trim_ref(src + ts_node_start_byte(av), ts_node_end_byte(av) - ts_node_start_byte(av), 1);
                        if (head.tier && kr.tier && vr.tier) {
                            if (has_subj && lp_id_eq(&head.id, &vr.id)) gen_claim_add(gr, f, (Ref[]){ kr, vr }, 2);
                            else gen_claim_add(gr, f, (Ref[]){ head, kr, vr }, 3);
                        }
                    }
                }
            }
            Ref owner = has_subj ? subj_ref : parent_subj;
            for (uint32_t i = 0; i < nc; i++) {
                TSNode ch = ts_node_named_child(nd, i); const char *ctp = ts_node_type(ch);
                if (!strcmp(ctp, "content")) {
                    uint32_t cc = ts_node_named_child_count(ch);
                    for (uint32_t j = 0; j < cc; j++) {
                        TSNode cch = ts_node_named_child(ch, j);
                        if (!strcmp(ts_node_type(cch), "CharData")) {
                            Ref tr = span_trim_ref(src + ts_node_start_byte(cch), ts_node_end_byte(cch) - ts_node_start_byte(cch), 0);
                            if (tr.tier && tag_ref.tier) {
                                if (owner.tier) gen_claim_add(gr, f, (Ref[]){ owner, tag_ref, tr }, 3);
                                else gen_claim_add(gr, f, (Ref[]){ tag_ref, tr }, 2);
                            }
                        }
                    }
                } else if (!strcmp(ctp, "text")) {
                    Ref tr = span_trim_ref(src + ts_node_start_byte(ch), ts_node_end_byte(ch) - ts_node_start_byte(ch), 0);
                    if (tr.tier && tag_ref.tier) {
                        if (owner.tier) gen_claim_add(gr, f, (Ref[]){ owner, tag_ref, tr }, 3);
                        else gen_claim_add(gr, f, (Ref[]){ tag_ref, tr }, 2);
                    }
                }
            }
            gen_record_flush(gr, f);
        }
    }
    uint32_t nc = ts_node_named_child_count(nd);
    for (uint32_t i = 0; i < nc; i++) attest_xml_node(ts_node_named_child(nd, i), src, f, gr, next_subj);
}
static TSNode find_ident(TSNode nd, int depth){
    if (ts_node_is_null(nd) || depth > 4) return (TSNode){0};
    const char *tp = ts_node_type(nd);
    if (!strcmp(tp, "identifier") || !strcmp(tp, "type_identifier") || !strcmp(tp, "field_identifier") || !strcmp(tp, "property_identifier"))
        return nd;
    TSNode nm = ts_node_child_by_field_name(nd, "name", 4);
    if (!ts_node_is_null(nm)) { TSNode r = find_ident(nm, depth + 1); if (!ts_node_is_null(r)) return r; }
    TSNode dc = ts_node_child_by_field_name(nd, "declarator", 10);
    if (!ts_node_is_null(dc)) { TSNode r = find_ident(dc, depth + 1); if (!ts_node_is_null(r)) return r; }
    return (TSNode){0};
}
static void attest_code_node(TSNode nd, const uint8_t *src, File *f, GenRec *gr){
    const char *tp = ts_node_type(nd);
    if (strstr(tp, "definition") || strstr(tp, "declaration") || strstr(tp, "specifier") || strstr(tp, "item") || !strcmp(tp, "method_declaration") || !strcmp(tp, "class_declaration") || !strcmp(tp, "interface_declaration") || !strcmp(tp, "struct_specifier") || !strcmp(tp, "function_item") || !strcmp(tp, "struct_item") || !strcmp(tp, "enum_item") || !strcmp(tp, "trait_item")) {
        TSNode id = find_ident(nd, 0);
        if (!ts_node_is_null(id)) {
            Ref kind_ref = string_ref((const uint8_t *)tp, strlen(tp));
            Ref sym_ref = span_trim_ref(src + ts_node_start_byte(id), ts_node_end_byte(id) - ts_node_start_byte(id), 0);
            if (kind_ref.tier && sym_ref.tier) gen_claim_add(gr, f, (Ref[]){ kind_ref, sym_ref }, 2);
        }
    }
    uint32_t nc = ts_node_named_child_count(nd);
    for (uint32_t i = 0; i < nc; i++) attest_code_node(ts_node_named_child(nd, i), src, f, gr);
}
static void attest_delimited(File *f, const uint8_t *src, size_t n, char sep){
    size_t pos = 0; if (n >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) pos = 3;
    Ref hdr[256]; int nhdr = 0, first_row = 1;
    GenRec gr = {0};
    while (pos < n) {
        size_t line_end = pos; while (line_end < n && src[line_end] != '\n') line_end++;
        size_t le = line_end; if (le > pos && src[le - 1] == '\r') le--;
        if (le > pos && src[pos] != '#') {
            Ref cells[256]; int nc = 0;
            size_t c = pos;
            while (c <= le && nc < 256) {
                size_t cs = c, ce = c;
                if (c < le && src[c] == '"') {
                    cs = ++c;
                    while (c < le) { if (src[c] == '"') { if (c + 1 < le && src[c + 1] == '"') { c += 2; continue; } break; } c++; }
                    ce = c; while (c < le && src[c] != sep) c++;
                } else {
                    while (c < le && src[c] != sep) c++;
                    ce = c;
                }
                cells[nc++] = span_trim_ref(src + cs, ce - cs, 1);
                if (c == le) break;
                c++;
            }
            if (first_row) {
                first_row = 0; nhdr = nc;
                for (int i = 0; i < nc; i++) hdr[i] = cells[i];
            } else if (nc > 0) {
                gr.n = 0;
                Ref subj = cells[0];
                for (int i = 0; i < nc; i++) {
                    if (!cells[i].tier) continue;
                    Ref col = (i < nhdr && hdr[i].tier) ? hdr[i] : (Ref){0};
                    if (subj.tier && i > 0 && col.tier) gen_claim_add(&gr, f, (Ref[]){ subj, col, cells[i] }, 3);
                    else if (col.tier) gen_claim_add(&gr, f, (Ref[]){ col, cells[i] }, 2);
                    else if (subj.tier && i > 0) gen_claim_add(&gr, f, (Ref[]){ subj, cells[i] }, 2);
                }
                gen_record_flush(&gr, f);
            }
        }
        pos = line_end + 1;
    }
    free(gr.claims);
}
void decompose_bytes(Ctx *c, File *f, uint8_t *src, size_t n, int first){
    const Recipe *r = f->recipe;
    if (r && r->curated) {
        if (first && n >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) { src += 3; n -= 3; }   /* a curated source's byte order mark is how it was written down, not what it says */
        if (first) {                                                         /* the witness, named as content */
            char w[512], l[512]; named_for(r->witness[0] ? r->witness : r->name, f->path, src, n, w, sizeof w); named_for(r->lineage, f->path, src, n, l, sizeof l);
            f->witness = text_ref(c, (const uint8_t *)w, strlen(w)); f->trunk = f->witness;
            if (l[0]) { f->lineage = text_ref(c, (const uint8_t *)l, strlen(l)); f->has_lineage = 1; } }
        if (r->native) { r->native->read(r, f, src, n); return; }           /* the native grammar the recipe names */
        attest_layout(r, f, src, n); return; }
    if (r && (!strcmp(r->grammar, "csv") || !strcmp(r->grammar, "tsv") || !strcmp(r->grammar, "psv"))) {
        f->trunk = text_ref(c, src, n);
        char sep = !strcmp(r->grammar, "tsv") ? '\t' : (!strcmp(r->grammar, "psv") ? '|' : ',');
        attest_delimited(f, src, n, sep);
        return;
    }
    if (!r || !r->lang) { f->trunk = text_ref(c, src, n); return; }        /* text: UAX #29 */
    TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, r->lang);     /* content: the file as its syntax tree, byte for byte */
    TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
    TSNode root = ts_tree_root_node(t);
    f->trunk = ast_node(root, src, 0, (uint32_t)n, 0);
    GenRec gr = {0};
    if (!strcmp(r->grammar, "json")) attest_json_node(root, src, f, &gr);
    else if (!strcmp(r->grammar, "xml") || !strcmp(r->grammar, "html")) attest_xml_node(root, src, f, &gr, (Ref){0});
    else if (!strcmp(r->grammar, "turtle")) {
        uint32_t nc = ts_node_named_child_count(root);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode st = ts_node_named_child(root, i);
            if (strcmp(ts_node_type(st), "statement")) continue;
            TSNode tr = ts_node_named_child(st, 0);
            if (ts_node_is_null(tr) || strcmp(ts_node_type(tr), "triples")) continue;
            TSNode s_nd = ts_node_named_child(tr, 0), pol = ts_node_named_child(tr, 1);
            if (ts_node_is_null(s_nd) || ts_node_is_null(pol)) continue;
            Ref sr = span_trim_ref(src + ts_node_start_byte(s_nd), ts_node_end_byte(s_nd) - ts_node_start_byte(s_nd), 1);
            gr.n = 0;
            uint32_t pc = ts_node_named_child_count(pol);
            for (uint32_t j = 0; j + 1 < pc; j += 2) {
                TSNode vb = ts_node_named_child(pol, j), ol = ts_node_named_child(pol, j + 1);
                Ref vr = span_trim_ref(src + ts_node_start_byte(vb), ts_node_end_byte(vb) - ts_node_start_byte(vb), 1);
                uint32_t oc = ts_node_named_child_count(ol);
                for (uint32_t k = 0; k < oc; k++) {
                    TSNode ob = ts_node_named_child(ol, k);
                    Ref or_ref = span_trim_ref(src + ts_node_start_byte(ob), ts_node_end_byte(ob) - ts_node_start_byte(ob), 1);
                    if (sr.tier && vr.tier && or_ref.tier) gen_claim_add(&gr, f, (Ref[]){ sr, vr, or_ref }, 3);
                }
            }
            gen_record_flush(&gr, f);
        }
    } else {
        attest_code_node(root, src, f, &gr);
        gen_record_flush(&gr, f);
    }
    free(gr.claims);
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
