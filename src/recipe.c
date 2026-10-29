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
 *   files PATTERN                 the files it is, when it is not everything under a root (several may be given); a
 *                                 directory a pattern names is every file under it
 *   ignore NAME                   files of this name found under what is walked (.gitignore) say, in gitignore's
 *                                 notation, which files beside and below them are not the source
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
 *   content, key, refer, type, metadata, omit, thing, value, attest, relate, pair, holds, itself, together,
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
#include "structure.h"
#include "os.h"
#include <tree_sitter/api.h>
#include <omp.h>
#include <zlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <blake3.h>

int program_file_hash(const char *path, uint8_t out[32]){
    FILE *file = fopen(path, "rb"); if (!file) return 0;
    blake3_hasher hash; blake3_hasher_init(&hash); uint8_t bytes[32768]; size_t n;
    while ((n = fread(bytes, 1, sizeof bytes, file))) blake3_hasher_update(&hash, bytes, n);
    int valid = !ferror(file); fclose(file);
    if (valid) blake3_hasher_finalize(&hash, out, 32); return valid;
}
static void program_size(blake3_hasher *hash, uint64_t value){
    uint8_t bytes[8]; for (unsigned i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (i * 8));
    blake3_hasher_update(hash, bytes, sizeof bytes);
}
static void program_text(blake3_hasher *hash, const char *text){
    size_t n = strlen(text); program_size(hash, n); blake3_hasher_update(hash, text, n);
}
/* A reading fingerprint is metadata, never an entity ID. Frame configuration
 * bytes before hashing them; no host path or ambiguous concatenation enters
 * the semantic program fingerprint. Include format lines in expansion order. */
void recipe_program_line(Recipe *recipe, const char *path, const char *line){
    if (!recipe->program_state) return;
    const char *name = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') name = p + 1;
    const char *begin = line; while (*begin == ' ' || *begin == '\t') begin++;
    const char *tail = begin; while (*tail && *tail != ' ' && *tail != '\t' && *tail != '\r' && *tail != '\n') tail++;
    size_t directive = (size_t)(tail - begin);
    /* These compiler directives consume the remaining bytes as a pattern;
     * native grammars also own their parameter syntax. Preserve those bytes
     * rather than treating significant spaces as token separators. */
    if (recipe->native || (directive == 4 && !memcmp(begin, "line", 4)) || (directive == 5 && !memcmp(begin, "about", 5))) {
        if (!directive) return;
        while (*tail == ' ' || *tail == '\t') tail++;
        size_t n = strcspn(tail, "\r\n"); blake3_hasher *hash = recipe->program_state;
        program_text(hash, name); program_size(hash, directive); blake3_hasher_update(hash, begin, directive);
        program_size(hash, n); blake3_hasher_update(hash, tail, n); return;
    }
    uint8_t canonical[4096]; size_t used = 0; int quoted = 0, space = 0;
    for (const char *p = line; *p; p++) {
        if (!quoted && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) { space = used != 0; continue; }
        if (used + (size_t)space >= sizeof canonical) { fputs("recipe contract line exceeds compiler input limit\n", stderr); abort(); }
        if (space) { canonical[used++] = ' '; space = 0; }
        canonical[used++] = (uint8_t)*p; if (*p == '"') quoted = !quoted;
    }
    if (!used) return;
    blake3_hasher *hash = recipe->program_state; program_text(hash, name);
    program_size(hash, used); blake3_hasher_update(hash, canonical, used);
}
void recipe_reading_hash(const Recipe *recipe, uint8_t out[32]){
    blake3_hasher hash; blake3_hasher_init(&hash);
    if (recipe) {
        blake3_hasher_update(&hash, recipe->program_hash, sizeof recipe->program_hash);
        program_text(&hash, recipe->name); program_text(&hash, recipe->grammar);
        program_text(&hash, recipe->witness); program_text(&hash, recipe->lineage);
        uint64_t trust; memcpy(&trust, &recipe->trust, sizeof trust); program_size(&hash, trust);
        program_size(&hash, recipe->curated != 0); program_size(&hash, recipe->native != NULL);
        uint8_t semantic[32]; say_program_hash(recipe, semantic); blake3_hasher_update(&hash, semantic, sizeof semantic);
        if (recipe->lang) {
            grammar_program_hash(grammar_id_for_language(recipe->lang), semantic); blake3_hasher_update(&hash, semantic, sizeof semantic);
        }
    }
    blake3_hasher_finalize(&hash, out, 32);
}

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
int say_compile(Recipe *r);
static const TSLanguage *grammar_load(const char *name){
    return grammar_lang(grammar_id_for(name));
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
static const char *metadata_root;
const char *recipes_directory(void){ return metadata_root ? metadata_root : laplace_recipes(); }
static int recipe_parse(const char *path, Recipe *r, int metadata){
    FILE *f = fopen(path, "r"); if (!f) return 0;
    memset(r, 0, sizeof *r); char line[4096];
    blake3_hasher program; blake3_hasher_init(&program); r->program_state = &program;
    while (fgets(line, sizeof line, f)) {
        comment_off(line); recipe_program_line(r, path, line); char whole[4096]; snprintf(whole, sizeof whole, "%s", line);
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "name")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->name, sizeof r->name, "%s", tok); }
        else if (!strcmp(tok, "match")) while ((tok = strtok(NULL, " \t\r\n"))) {
            if (r->nmatch == (int)(sizeof r->match / sizeof r->match[0]) || strlen(tok) >= sizeof r->match[0]) {
                fprintf(stderr, "%s: too many or overlong match patterns\n", path); fclose(f); r->program_state = NULL; return 0;
            }
            snprintf(r->match[r->nmatch++], sizeof r->match[0], "%s", tok);
        }
        else if (!strcmp(tok, "grammar")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->grammar, sizeof r->grammar, "%s", tok); r->native = native_grammar(r->grammar); }
        else if (!strcmp(tok, "class")) r->trust = class_trust(path, strtok(NULL, " \t\r\n"));
        else if (!strcmp(tok, "trust") || !strcmp(tok, "deviation")) trust_by_number(path, tok);
        else if (!strcmp(tok, "like")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->like, sizeof r->like, "%s", tok); }
        else if (!strcmp(tok, "witness")) rest_of(r->witness, sizeof r->witness);
        else if (!strcmp(tok, "lineage")) rest_of(r->lineage, sizeof r->lineage);
        else if (metadata && !strcmp(tok, "format")) {
            tok = strtok(NULL, " \t\r\n"); char fp[4096];
            if (!tok || strchr(tok, '/') || strchr(tok, '\\') || strstr(tok, "..")) { fclose(f); return 0; }
            snprintf(fp, sizeof fp, "%s/formats/%s.format", metadata_root, tok);
            FILE *format = fopen(fp, "r"); if (!format) { fprintf(stderr, "missing format: %s\n", fp); fclose(f); return 0; }
            char declaration[4096]; while (fgets(declaration, sizeof declaration, format)) {
                char key[64], value[128]; if (sscanf(declaration, "%63s %127s", key, value) == 2 && !strcmp(key, "grammar")) snprintf(r->grammar, sizeof r->grammar, "%s", value);
            }
            fclose(format);
        }
        else if (!strcmp(tok, "product-format")) {
            char name[64], *argument = strtok(NULL, " \t\r\n");
            if (!argument || strlen(argument) >= sizeof name || strtok(NULL, " \t\r\n")) { fclose(f); r->program_state = NULL; return 0; }
            snprintf(name, sizeof name, "%s", argument);
            if (!metadata && !recipe_product_format(r, name)) { fclose(f); r->program_state = NULL; return 0; }
        }
        else if (metadata) continue;                                    /* inventory needs no grammar, cache or database */
        else { int sv = r->native ? r->native->says(r, path, tok)           /* a line of the native grammar the recipe names */
                              : say_says(r, path, tok);                     /* the file's layout and the disposition of its parts */
            if (sv > 0 && !r->native) say_mentions(r, whole);              /* its words: a part named in none is left open */
            if (!sv) fprintf(stderr, "%s: \"%s\" is not something a recipe says\n", path, tok);
            if (sv <= 0) { fclose(f); r->program_state = NULL; return 0; } }
    }
    fclose(f);
    blake3_hasher_finalize(&program, r->program_hash, sizeof r->program_hash); r->program_state = NULL;
    if (metadata || r->like[0]) return 1;
    /* A grammar-only recipe records packaging bytes. Native semantic execution
     * requires its explicit directives, rather than silently turning a raw PGN
     * file into curated chess claims. */
    if (r->native && !r->native_recipe) r->native = NULL;
    if (r->native) return r->native->done(r, path);                          /* a native grammar reads its files itself */
    if (say_lays(r)) {                                                       /* configured: a grammar named gives the tree, else the layout's tiers do */
        if (r->grammar[0] && strcmp(r->grammar, "layout")) { r->lang = grammar_load(r->grammar); if (!r->lang) return 0; } else snprintf(r->grammar, sizeof r->grammar, "layout");
        r->curated = 1;
        if (!say_compile(r)) { fprintf(stderr, "%s: unresolved compiled grammar rules\n", path); return 0; }
        return 1; }
    if (r->say && !say_only_disposes(r)) { fprintf(stderr, "%s: it says what parts are, but lays out no tree for them to be parts of (tier, format or node)\n", path); return 0; }
    if (r->say && !say_compile(r)) { fprintf(stderr, "%s: unresolved compiled metadata rules\n", path); return 0; }
    if (!r->grammar[0]) snprintf(r->grammar, sizeof r->grammar, "text");
    if (strcmp(r->grammar, "text")) { r->lang = grammar_load(r->grammar); if (!r->lang) return 0; }
    return 1;
}
static Source *sources; static int nsources;
static int source_parse(const char *path, Source *s, int metadata){
    FILE *f = fopen(path, "r"); if (!f) return 0;
    memset(s, 0, sizeof *s); char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h && (h == line || h[-1] == ' ' || h[-1] == '\t')) *h = 0;
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "name")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(s->name, sizeof s->name, "%s", tok); }
        else if (!strcmp(tok, "aliases")) while ((tok = strtok(NULL, " \t\r\n"))) {
            if (s->naliases == 8 || strlen(tok) >= 64) { fprintf(stderr, "%s: too many or overlong aliases\n", path); fclose(f); return 0; }
            snprintf(s->aliases[s->naliases++], 64, "%s", tok);
        }
        else if (!strcmp(tok, "witness")) rest_of(s->witness, sizeof s->witness);
        else if (!strcmp(tok, "lineage")) rest_of(s->lineage, sizeof s->lineage);
        else if (!strcmp(tok, "class")) s->trust = class_trust(path, strtok(NULL, " \t\r\n"));
        else if (!strcmp(tok, "trust") || !strcmp(tok, "deviation")) trust_by_number(path, tok);
        else if (!strcmp(tok, "root")) { tok = strtok(NULL, " \t\r\n"); if (tok && s->nroot < 8) {
            if (metadata) snprintf(s->root[s->nroot++], 512, "%s", tok);
            else if (!path_expand(tok, s->root[s->nroot++], 512, s->unset, sizeof s->unset)) s->nroot--; } }
        else if (!strcmp(tok, "files")) { tok = strtok(NULL, " \t\r\n"); if (tok && s->nfiles < 16) {
            if (metadata) snprintf(s->files[s->nfiles++], 512, "%s", tok);
            else if (!path_expand(tok, s->files[s->nfiles++], 512, s->unset, sizeof s->unset)) s->nfiles--; } }
        else if (!strcmp(tok, "room")) { tok = strtok(NULL, " \t\r\n"); if (tok) s->room = atof(tok); }
        else if (!strcmp(tok, "supersede")) s->supersede = 1;               /* its newest whole reading is what it says: the earlier ones go */
        else if (!strcmp(tok, "tier0")) s->tier0 = 1;                       /* it records every codepoint, tier 0, as its ingest's first act */
        else if (!strcmp(tok, "held")) rest_of(s->held, sizeof s->held);
        else if (!strcmp(tok, "called")) { char *v = strtok(NULL, "\r\n"); while (v && (*v == ' ' || *v == '\t')) v++; if (v) snprintf(s->called, sizeof s->called, "%s", v); }
        else if (!strcmp(tok, "after")) while ((tok = strtok(NULL, " \t\r\n")) && s->nafter < 16) snprintf(s->after[s->nafter++], 64, "%s", tok);
        else if (!strcmp(tok, "except")) while ((tok = strtok(NULL, " \t\r\n")) && s->nexcept < 8) snprintf(s->except[s->nexcept++], 128, "%s", tok);
        else if (!strcmp(tok, "ignore")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(s->ignore, sizeof s->ignore, "%s", tok); }
        else if (!strcmp(tok, "reads")) while ((tok = strtok(NULL, " \t\r\n")) && s->nreads < 32) snprintf(s->reads[s->nreads++], 64, "%s", tok);
        else { fprintf(stderr, "%s: \"%s\" is not something a source says\n", path, tok); fclose(f); return 0; }
    }
    fclose(f);
    if (metadata) return s->name[0] != 0;
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
static int recipes_in(const char *dir, int source, Recipe **out, int n, int metadata){
    os_list ls; if (os_list_dir(dir, &ls)) { perror(dir); return n; }
    char **names = ls.item; int nn = (int)ls.n;
    qsort(names, (size_t)nn, sizeof(char *), by_name);
    for (int i = 0; i < nn; i++) {
        size_t l = strlen(names[i]); char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (l >= 8 && !strcmp(names[i] + l - 7, ".recipe")) {
            *out = xrealloc(*out, sizeof(Recipe) * (size_t)(n + 1)); Recipe *r = &(*out)[n];
            if (!recipe_parse(p, r, metadata)) { r->broken = 1; r->curated = 0; r->lang = NULL; if (!r->name[0]) snprintf(r->name, sizeof r->name, "%.*s", (int)(l - 7 < 63 ? l - 7 : 63), names[i]); }
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
            if (!source_parse(sp, &sources[nsources], metadata)) { fprintf(stderr, "source %s does not load\n", sp); exit(2); }
            n = recipes_in(p, nsources++, out, n, metadata);
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
          if (rank[j] != 1 << 20) { fprintf(stderr, "%s repeats source %s\n", p, c); exit(2); }
          rank[j] = r++; }
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
int source_named(const Source *s, const char *name){
    for (int a = -1; a < s->naliases; a++) {
        const unsigned char *p = (const unsigned char *)(a < 0 ? s->name : s->aliases[a]);
        const unsigned char *q = (const unsigned char *)name;
        while (*p && *q && tolower(*p) == tolower(*q)) { p++; q++; }
        if (!*p && !*q) return 1;
    }
    return 0;
}
typedef struct { size_t *offsets; const size_t *children; } RecipeDependencies;
static int recipe_dependency(void *context,size_t node,size_t *cursor,size_t *child,size_t *distance){
    const RecipeDependencies *graph=context;size_t begin=graph->offsets[node],end=graph->offsets[node+1];
    if(*cursor==end-begin)return 1;
    *child=graph->children[begin+(*cursor)++];*distance=1;return 0;
}
/* Compile foreign recipe references once. Native groups logical cycles; its
 * component order puts every external provider before its consumers. The
 * existing source continuation solver retains declaration order inside a
 * cyclic group. This graph schedules files, never canonical content nodes. */
static void recipe_dependencies(Recipe *recipes,size_t count){
    if(!count)return;
    lp_strmap *names=lp_strmap_sized(sizeof(size_t));lp_vec(size_t) children={0};
    size_t *offsets=calloc(count+1,sizeof *offsets),*components=malloc(count*sizeof *components);
    size_t *heads=malloc(count*sizeof *heads),*next=malloc(count*sizeof *next);
    if(!offsets || !components || !heads || !next){perror("recipe dependency graph");exit(2);}
    for(size_t i=0;i<count;i++){
        bool fresh;size_t *index=lp_strmap_get(names,recipes[i].name,strlen(recipes[i].name),&fresh);
        if(fresh)*index=i;
    }
    for(size_t i=0;i<count;i++){
        offsets[i]=children.n;const char *name;
        for(int k=0;(name=say_refers(recipes+i,k));k++){
            int64_t found=lp_strmap_find(names,name,strlen(name));
            if(found>=0)lp_push(&children,*(size_t *)lp_strmap_at(names,(size_t)found));
        }
    }
    offsets[count]=children.n;lp_strmap_free(names);
    RecipeDependencies graph={offsets,children.v};size_t groups;
    if(!lp_graph_components(count,recipe_dependency,&graph,components,&groups)){
        perror("recipe dependency graph");exit(2);
    }
    for(size_t i=0;i<groups;i++)heads[i]=SIZE_MAX;
    for(size_t i=0;i<count;i++){next[i]=heads[components[i]];heads[components[i]]=i;}
    for(size_t group=0;group<groups;group++){
        size_t level=0;
        for(size_t i=heads[group];i!=SIZE_MAX;i=next[i])for(size_t edge=offsets[i];edge<offsets[i+1];edge++){
            size_t child=children.v[edge];
            if(components[child]!=group && level<=recipes[child].dependency_level)level=recipes[child].dependency_level+1;
        }
        for(size_t i=heads[group];i!=SIZE_MAX;i=next[i])recipes[i].dependency_level=level;
    }
    free(offsets);free(components);free(heads);free(next);lp_vec_free(&children);
}
static int recipes_load_mode(const char *dir, Recipe **out, int metadata){
    if (recipes_now) { *out = recipes_now; return nrecipes_now; }
    metadata_root = dir;
    Recipe *r = NULL; int n = recipes_in(dir, -1, &r, 0, metadata);
    for (int i = 0; i < n; i++) if (r[i].like[0]) {                          /* it reads as another recipe does */
        int j = 0; while (j < n && strcmp(r[j].name, r[i].like)) j++;
        if (j == n || r[j].broken) { fprintf(stderr, "recipe %s reads like %s, which %s\n", r[i].name, r[i].like, j == n ? "is not a recipe" : "did not load"); r[i].broken = 1; r[i].like[0] = 0; continue; }
        Recipe me = r[i]; r[i] = r[j]; snprintf(r[i].file, sizeof r[i].file, "%s", me.file);
        blake3_hasher program; blake3_hasher_init(&program);
        blake3_hasher_update(&program, r[j].program_hash, sizeof r[j].program_hash);
        blake3_hasher_update(&program, me.program_hash, sizeof me.program_hash);
        blake3_hasher_finalize(&program, r[i].program_hash, sizeof r[i].program_hash);
        snprintf(r[i].name, sizeof r[i].name, "%s", me.name); memcpy(r[i].match, me.match, sizeof me.match); r[i].nmatch = me.nmatch; r[i].source = me.source;
        memcpy(r[i].witness, me.witness, sizeof me.witness); memcpy(r[i].lineage, me.lineage, sizeof me.lineage); r[i].trust = me.trust; r[i].like[0] = 0;
    }
    for (int i = 0; i < n; i++) if (r[i].source < 0 && r[i].trust == 0.0) r[i].trust = 0.60;
    for (int i = 0; i < nsources; i++) for (int a = -1; a < sources[i].naliases; a++) {
        const char *name = a < 0 ? sources[i].name : sources[i].aliases[a];
        for (const char *p = name; *p; p++) if (!(*p >= 'a' && *p <= 'z') && !isdigit((unsigned char)*p) && *p != '-') {
            fprintf(stderr, "source %s has invalid name or alias %s\n", sources[i].name, name); exit(2);
        }
        for (int j = 0; j < i; j++) if (source_named(&sources[j], name)) {
            fprintf(stderr, "source name or alias %s is shared by %s and %s\n", name, sources[j].name, sources[i].name); exit(2);
        }
        for (int b = -1; b < a; b++) if (!strcmp(name, b < 0 ? sources[i].name : sources[i].aliases[b])) {
            fprintf(stderr, "source %s repeats name or alias %s\n", sources[i].name, name); exit(2);
        }
    }
    recipe_dependencies(r,(size_t)n);
    recipes_now = r; nrecipes_now = n; snprintf(order_dir, sizeof order_dir, "%s", dir); sources_order();
    *out = r; return n;
}
int recipes_load(const char *dir, Recipe **out){ return recipes_load_mode(dir, out, 0); }
static void catalog_strings(FILE *out, const char *base, size_t stride, int n){
    fputc('[', out); for (int i = 0; i < n; i++) { if (i) fputc(',', out); json_str(out, base + stride * (size_t)i); } fputc(']', out);
}
static void catalog_recipe(FILE *out, const Recipe *r){
    fputs("{\"name\":", out); json_str(out, r->name);
    fputs(",\"grammar\":", out); json_str(out, r->grammar);
    fputs(",\"like\":", out); json_str(out, r->like);
    fputs(",\"match\":", out); catalog_strings(out, (const char *)r->match, sizeof r->match[0], r->nmatch); fputc('}', out);
}
int recipes_catalog(const char *dir, FILE *out, int json){
    char order[4096]; snprintf(order, sizeof order, "%s/order", dir);
    if (!os_exists(order)) { fprintf(stderr, "recipe estate has no order declaration: %s\n", dir); return 2; }
    Recipe *r = NULL; int n = recipes_load_mode(dir, &r, 1);
    if (!nsources) { fputs("recipe estate contains no sources\n", stderr); return 2; }
    for (int i = 0; i < n; i++) if (r[i].broken) { fprintf(stderr, "catalog recipe metadata did not load: %s\n", r[i].file); return 2; }
    if (!json) { for (int i = 0; i < nsources; i++) fprintf(out, "%s\n", sources[i].name); return ferror(out) ? 1 : 0; }
    fputs("{\"sources\":[", out);
    for (int i = 0; i < nsources; i++) {
        const Source *s = &sources[i]; if (i) fputc(',', out);
        fputs("{\"name\":", out); json_str(out, s->name);
        fputs(",\"aliases\":", out); catalog_strings(out, (const char *)s->aliases, sizeof s->aliases[0], s->naliases);
        fputs(",\"witness\":", out); json_str(out, s->witness);
        fputs(",\"lineage\":", out); json_str(out, s->lineage);
        fputs(",\"held\":", out); json_str(out, s->held);
        fputs(",\"roots\":", out); catalog_strings(out, (const char *)s->root, sizeof s->root[0], s->nroot);
        fputs(",\"files\":", out); catalog_strings(out, (const char *)s->files, sizeof s->files[0], s->nfiles);
        fputs(",\"after\":", out); catalog_strings(out, (const char *)s->after, sizeof s->after[0], s->nafter);
        fputs(",\"reads\":", out); catalog_strings(out, (const char *)s->reads, sizeof s->reads[0], s->nreads);
        fputs(",\"recipes\":[", out); int first = 1;
        for (int k = 0; k < n; k++) if (r[k].source == i) { if (!first) fputc(',', out); catalog_recipe(out, &r[k]); first = 0; }
        fputs("]}", out);
    }
    fputs("],\"formats\":[", out); int first = 1;
    for (int k = 0; k < n; k++) if (r[k].source < 0) { if (!first) fputc(',', out); catalog_recipe(out, &r[k]); first = 0; }
    fputs("]}\n", out); return ferror(out) ? 1 : 0;
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
Recipe *recipe_select(Recipe *r, int n, const char *path, const Source *of, RecipeCoverage *coverage){
    if (coverage) *coverage=RECIPE_UNCOVERED;
    const char *b0 = strrchr(path, '/'); b0 = b0 ? b0 + 1 : path;
    char base[1024]; snprintf(base, sizeof base, "%s", b0);
    size_t bl = strlen(base); if (bl > 3 && !strcmp(base + bl - 3, ".gz")) base[bl - 3] = 0;   /* matched by what it holds */
    if (!of) for (int s = 0; s < nsources; s++) if (under_source_root(&sources[s], path)) { of = &sources[s]; break; }
    Recipe *best = NULL; size_t best_lit = 0;                              /* the most specific pattern wins */
    for (int i = 0; of && i < of->nexcept; i++) if (!os_fnmatch(of->except[i], path)) {
        if (coverage) *coverage=RECIPE_OMITTED;
        return NULL;
    }
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
    if (best && !best->curated && best->grammar[0] && strcmp(best->grammar, "text") != 0 && !best->lang) {
        best->lang = grammar_load(best->grammar);
        if (!best->lang) { fprintf(stderr, "recipe %s: required grammar %s is unavailable for %s\n", best->name, best->grammar, path); exit(2); }
    }
    if (coverage && best) *coverage=RECIPE_COVERED;
    return best;
}
Recipe *recipe_for(Recipe *r, int n, const char *path, const Source *of){
    return recipe_select(r,n,path,of,NULL);
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

/* ---- syntax tree -> content, directly as the cursor ascends. Only the active
 * ancestor frames and their already composed children live in the arena. No
 * projected DOM, copied TSNode array per node, or depth-dependent text fallback.
 * Gaps and unnamed punctuation are retained so PostgreSQL can realize every byte. */
typedef struct AstFrame {
    struct AstFrame *parent;
    Ref *children;
    size_t mark;
    uint32_t end, at, count;
} AstFrame;

static Ref ast_cursor(TSNode root, const uint8_t *src, uint32_t lo, uint32_t hi);

/* Large sibling sets are independent composition work. The taskgroup implied by
 * taskloop keeps these arena slots alive until every result has been written. */
static Ref ast_parallel(TSNode root, const uint8_t *src, uint32_t lo, uint32_t hi, uint32_t count){
    size_t mark = scratch_mark();
    TSNode *nodes = scratch_alloc(sizeof(TSNode) * count);
    Ref *children = scratch_alloc(sizeof(Ref) * (2 * (size_t)count + 1));
    uint8_t *present = scratch_alloc(2 * (size_t)count + 1);
    memset(present, 0, 2 * (size_t)count + 1);
    TSTreeCursor *cursor = lp_tls_cursor(root);
    if (!cursor) { fputs("syntax composition: cursor pool exhausted\n", stderr); abort(); }
    uint32_t found = 0;
    if (ts_tree_cursor_goto_first_child(cursor)) do {
        nodes[found++] = ts_tree_cursor_current_node(cursor);
    } while (found < count && ts_tree_cursor_goto_next_sibling(cursor));
    lp_tls_cursor_release(cursor);
    #pragma omp taskloop num_tasks(omp_get_num_threads() * 8)
    for (uint32_t i = 0; i < found; i++) {
        uint32_t start = ts_node_start_byte(nodes[i]), end = ts_node_end_byte(nodes[i]);
        if (end > start) { children[2 * i + 1] = ast_cursor(nodes[i], src, start, end); present[2 * i + 1] = 1; }
    }
    uint32_t at = lo;
    for (uint32_t i = 0; i < found; i++) {
        uint32_t start = ts_node_start_byte(nodes[i]), end = ts_node_end_byte(nodes[i]);
        if (start > at) { children[2 * i] = string_ref(src + at, start - at); present[2 * i] = 1; }
        if (end > at) at = end;
    }
    if (hi > at) { children[2 * found] = string_ref(src + at, hi - at); present[2 * found] = 1; }
    uint32_t used = 0;
    for (uint32_t i = 0; i <= 2 * found; i++) if (present[i]) children[used++] = children[i];
    Ref result = compose(children, used, ref_above(children, used));
    scratch_restore(mark);
    return result;
}

static Ref ast_cursor(TSNode root, const uint8_t *src, uint32_t lo, uint32_t hi){
    TSTreeCursor *cursor = lp_tls_cursor(root);
    if (!cursor) { fputs("syntax composition: cursor pool exhausted\n", stderr); abort(); }
    AstFrame *frame = NULL;
    uint32_t start = lo, end = hi;
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(cursor);
        uint32_t count = ts_node_child_count(node);
        if (frame && start > frame->at) frame->children[frame->count++] = string_ref(src + frame->at, start - frame->at);
        Ref result;
        int present = end > start;
        if (present && count && end - start > (4u << 20) && count >= 16) {
            result = ast_parallel(node, src, start, end, count);
        } else if (present && count) {
            size_t mark = scratch_mark();
            AstFrame *next = scratch_alloc(sizeof *next);
            next->children = scratch_alloc(sizeof(Ref) * (2 * (size_t)count + 1));
            next->parent = frame; next->mark = mark; next->at = start; next->end = end; next->count = 0;
            frame = next;
            ts_tree_cursor_goto_first_child(cursor);
            node = ts_tree_cursor_current_node(cursor);
            start = ts_node_start_byte(node); end = ts_node_end_byte(node);
            continue;
        } else if (present || !frame) {
            result = string_ref(src + start, end - start);
            present = 1;
        }
        for (;;) {
            if (!frame) { lp_tls_cursor_release(cursor); return result; }
            if (present) frame->children[frame->count++] = result;
            if (end > frame->at) frame->at = end;
            if (ts_tree_cursor_goto_next_sibling(cursor)) {
                node = ts_tree_cursor_current_node(cursor);
                start = ts_node_start_byte(node); end = ts_node_end_byte(node);
                break;
            }
            ts_tree_cursor_goto_parent(cursor);
            if (frame->end > frame->at) frame->children[frame->count++] = string_ref(src + frame->at, frame->end - frame->at);
            result = compose(frame->children, frame->count, ref_above(frame->children, frame->count));
            end = frame->end;
            AstFrame *parent = frame->parent;
            size_t mark = frame->mark;
            scratch_restore(mark);
            frame = parent; present = 1;
        }
    }
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
typedef struct { FILE *plain; gzFile compressed; File *file; int failed; } RecipeInput;
int recipe_record_streamable(const Recipe *recipe){
    return recipe && recipe->curated && (recipe->native ? recipe->native->input != NULL : say_record_streamable(recipe));
}
static size_t recipe_input_read(void *context, uint8_t *out, size_t capacity){
    RecipeInput *input = context; size_t n;
    if (input->compressed) {
        unsigned take = capacity > INT_MAX ? INT_MAX : (unsigned)capacity;
        int got = gzread(input->compressed, out, take);
        if (got < 0) { input->failed = 1; return 0; }
        n = (size_t)got;
    } else n = fread(out, 1, capacity, input->plain);
    input->file->bytes += n; return n;
}
static int recipe_input_error(void *context){
    RecipeInput *input = context;
    if (input->compressed) {
        int status; const char *message = gzerror(input->compressed, &status);
        if (status != Z_OK && status != Z_STREAM_END) {
            fprintf(stderr, "%s: %s\n", input->file->path, message); input->failed = 1;
        }
    } else if (ferror(input->plain)) { perror(input->file->path); input->failed = 1; }
    return input->failed;
}
static void decompose_input(Ctx *c, File *f){
    const Recipe *r = f->recipe;
    if (recipe_record_streamable(r)) {
        RecipeInput reader = {.file=f}; size_t length = strlen(f->path);
        if (length > 3 && !strcmp(f->path + length - 3, ".gz")) {
            reader.compressed = gzopen(f->path, "rb");
            if (reader.compressed) gzbuffer(reader.compressed, 1 << 20);
        } else reader.plain = fopen(f->path, "rb");
        if (!reader.compressed && !reader.plain) { perror(f->path); exit(2); }
        const char *witness = f->source ? source_called(f->source) : r->witness[0] ? r->witness : r->name;
        const char *lineage = f->source ? f->source->lineage : r->lineage;
        if (!witness || strchr(witness, '{') || strchr(lineage, '{')) {
            fprintf(stderr, "%s: streaming records require a declared source record and lineage, without witness templates\n", f->path); exit(2);
        }
        f->witness = text_ref(c, (const uint8_t *)witness, strlen(witness)); f->trunk = f->witness;
        if (lineage[0]) { f->lineage = text_ref(c, (const uint8_t *)lineage, strlen(lineage)); f->has_lineage = 1; }
        SInput input = {.context=&reader, .read=recipe_input_read, .error=recipe_input_error};
        f->bytes = 0;
        if (r->native) r->native->input(r, f, &input); else attest_input(r, f, &input);
        if (reader.compressed) { if (gzclose(reader.compressed) != Z_OK) reader.failed = 1; }
        else if (fclose(reader.plain)) reader.failed = 1;
        if (reader.failed) { fprintf(stderr, "%s: input did not close cleanly\n", f->path); exit(2); }
        f->exact = 1; return;
    }
    size_t n; uint8_t *src0 = read_all(f->path, &n); if (!src0) { perror(f->path); f->exact = -1; exit(2); }
    f->bytes = n; decompose_bytes(c, f, src0, n, 1);
    if (f->recipe && f->recipe->curated) f->exact = 1;
    else {
        f->exact = file_matches_bytes(f->has_packaging ? &f->packaging.id : &f->trunk.id, src0, n);
    }
    free(src0);
}
void decompose_file(Ctx *c, File *f){
    const File *previous = table_reading_file(f);
    decompose_input(c, f);
    table_reading_file(previous);
}
typedef struct { uint8_t *bytes; size_t length, capacity; int boundary; } ProductText;
static void product_append(ProductText *text, const void *bytes, size_t length){
    if (length > SIZE_MAX - text->length) { fputs("product text exceeds address space\n", stderr); exit(2); }
    size_t needed = text->length + length;
    if (needed > text->capacity) {
        size_t capacity = needed > SIZE_MAX/2 ? needed : needed*2;
        text->bytes = xrealloc(text->bytes, capacity); text->capacity = capacity;
    }
    if (length) memcpy(text->bytes + text->length, bytes, length);
    text->length = needed;
}
static int product_text(void *sink, int event, const SField *field){
    ProductText *text = sink;
    if (event == S_OPEN || event == S_CLOSE) { if (text->length) text->boundary = 1; }
    else if (event == S_FIELD && field->kind == S_TEXT && field->value_length) {
        if (text->boundary && text->length && text->bytes[text->length - 1] != '\n') product_append(text, "\n", 1);
        text->boundary = 0; product_append(text, field->value, field->value_length);
    }
    return S_VISIT_OK;
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
    if (!r || !r->lang) { f->trunk = text_ref(c, src, n); return; }        /* text: UAX #29 */
    if (n > UINT32_MAX) { fprintf(stderr, "%s: Tree-sitter input exceeds its 32-bit byte range\n", f->path); exit(2); }
    uint16_t gid = grammar_id_for_language(r->lang);
    TSParser *ps = gid > 0 ? lp_tls_parser(gid) : NULL;
    if (!ps) { fprintf(stderr, "%s: cannot obtain the registered Tree-sitter parser\n", f->path); exit(2); }
    TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
    if (!t) { fprintf(stderr, "%s: Tree-sitter could not parse the input\n", f->path); exit(2); }
    TSNode root = ts_tree_root_node(t);
    f->trunk = ast_cursor(root, src, 0, (uint32_t)n);
    if (r->product) {
        ProductText product = {0};
        if (!s_cursor(r->product, root, src, n, product_text, &product)) { fprintf(stderr, "%s: product projection failed\n", f->path); exit(2); }
        f->packaging = f->trunk; f->has_packaging = 1;
        f->trunk = text_ref(c, product.bytes, product.length);
        free(product.bytes);
    }
    ts_tree_delete(t);
}

/* ---- laplace tree: a file's syntax tree as its recipe's grammar reads it, for writing recipes
 *   laplace tree [-r recipes] [-g grammar] [-n nodes] file */
static void tree_print(TSTreeCursor *cur, const uint8_t *src, int depth, long *left){
    do {
        if (*left <= 0) return;
        TSNode nd = ts_tree_cursor_current_node(cur); const char *field = ts_tree_cursor_current_field_name(cur);
        uint32_t a = ts_node_start_byte(nd), b = ts_node_end_byte(nd);
        if (ts_node_is_named(nd) || ts_node_is_missing(nd) || ts_node_is_error(nd) || ts_node_child_count(nd)) {
            (*left)--; printf("%*s", depth * 2, ""); if (field) printf("%s: ", field);
            printf("(%s%s) [%u, %u]", ts_node_is_missing(nd) ? "MISSING " : "", ts_node_type(nd), a, b);
            if (ts_node_has_error(nd) && !ts_node_is_error(nd) && !ts_node_is_missing(nd)) printf(" contains errors");
            if (!ts_node_child_count(nd)) { printf("  "); fwrite(src + a, 1, b - a > 60 ? 60 : b - a, stdout); if (b - a > 60) printf("\xE2\x80\xA6"); }
            putchar('\n');
        }
        if (ts_tree_cursor_goto_first_child(cur)) { tree_print(cur, src, depth + 1, left); ts_tree_cursor_goto_parent(cur); }
    } while (ts_tree_cursor_goto_next_sibling(cur));
}
int cmd_tree(int argc, char **argv){
    const char *rdir = laplace_recipes(), *grammar = NULL; long long nodes_opt = 400;
    int a = opts(argc, argv, (const Opt[]){ { "-r", 's', &rdir, 0 }, { "-g", 's', &grammar, 0 }, { "-n", 'l', &nodes_opt, 0 }, { 0 } });
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
    TSNode root = ts_tree_root_node(t); int malformed = ts_node_has_error(root);
    TSTreeCursor cur = ts_tree_cursor_new(root);
    tree_print(&cur, src, 0, &nodes);
    if (nodes <= 0) printf("\xE2\x80\xA6 (more: -n)\n");
    ts_tree_cursor_delete(&cur); ts_tree_delete(t); ts_parser_delete(ps); free(src);
    return malformed ? 1 : 0;
}
