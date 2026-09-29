/* Sources, their recipes, and the decomposition they drive.
 *
 * A source is a directory of the recipes directory: a file named "source", and a recipe for each kind of file the
 * source comes in. The source file:
 *   name NAME
 *   witness NAME...               who testifies when this source attests, named as content
 *   lineage NAME...               the witness it derives from
 *   trust T | deviation D         how far the witness is trusted
 *   root PATH                     where the source is kept ($NAME from the environment; * for the newest of several).
 *                                 Several roots may be given: the first that exists is the source
 *   except PATTERN...             files under its root that are not the source
 *   after SOURCE...               the sources it comes after
 *   reads FORMAT...               its files are ordinary content, read as these formats
 * A recipe of a source takes the source's witness, lineage and trust unless it names its own.
 *
 * A recipe file:
 *   name NAME
 *   match GLOB...                 files it applies to (by file name)
 *   grammar text | vocabulary | table | NAME
 *                                 UAX #29 text; a tokenizer's vocabulary (each token as the text it stands for, the
 *                                 vocabulary the path of its tokens in index order); a table of rows and fields; or
 *                                 a tree-sitter grammar loaded from $LAPLACE_GRAMMARS
 *   A table:
 *   separator tab | CHAR          what parts a row's fields (tab unless said)
 *   header                        the first row names the columns
 *   columns NAME...               the columns, when no row names them
 *   comment CHAR                  a line that begins with it is not a row
 *   and in a table's claims and maps, in place of a query:
 *     subject in COLUMN[.resolver...]    predicate in COLUMN    object in COLUMN    key in COLUMN    value in COLUMN
 *     where COLUMN is VALUE | is-not VALUE | matches PATTERN
 *   An empty field attests nothing.
 *   trust T                       the source's trust as a witness, -1 .. 1
 *   deviation D                   the same, given as the deviation the witness plays with (trust is g of it)
 *   map NAME [from FILE GRAMMAR]  patterns up to "end", read over the whole file (or over FILE, by its own grammar)
 *   ...                             before anything is attested: each match binds @key to @value, so a part
 *   end                             written as one identifier can be recorded as what that identifier stands for
 *   claims                        a kind of statement the source makes. What follows applies to it:
 *     predicate TEXT                the claims' predicate, when the source states it by position rather than by name
 *     predicate-in-name A B         the claims' predicate, when the file's name states it: between its last A and the
 *                                   B that follows
 *     enter RATING DEVIATION        the stock default a claim of this kind enters at
 *     ordered                       each claim's position among this kind's claims in its record is recorded
 *     distinct                      a claim whose subject is its object is not one
 *     spaces PART CHAR              a character the source writes where it means a space
 *     say PART FROM = TO            a part the source abbreviates, as it is said
 *     ending PART FROM TO           an ending the source writes, and the one it stands for (before any map)
 *     query                         tree-sitter query patterns, up to a line "end"; every match attests one claim:
 *     ...                             @subject, @predicate, @object, each with resolvers after a dot, in order:
 *     end                             .cp   a codepoint written in hex (quotes stripped)
 *                                     .text the node's text (quotes and surrounding spaces stripped)
 *                                     .head the node's text before its first colon
 *                                     .iri  an identifier written between angle brackets, without them
 *                                     .cps  codepoints written in hex, as the text they are
 *                                     .xml  the node's text with XML references resolved (quotes stripped)
 *                                     .node the node itself, as recorded
 *                                     .NAME then looked up in map NAME; a part no map holds attests nothing
 *                                   predicates: #eq? #not-eq? #any-of? #not-any-of? #match? #not-match?
 *   query                         the same, for a recipe with one kind of statement
 *   witness NAME...               the source as witness, named as content ({dir}: the directory the file is in). A recipe with a query is a curated source:
 *                                 what is recorded is what it attests — the claims and the entities they relate — never
 *                                 the file's own syntax (rows, tags, delimiters)
 *   lineage NAME...               the witness this one derives from, named as content. Copies of one lineage play one
 *                                 matchup per claim; each copy is still a row in the ledger.
 *   predicate TEXT                the claims' predicate, when the source states it by position rather than by name
 *   records                       the file is a flat sequence of line-terminated records: large files are split at
 *                                 line boundaries and the pieces parsed on every core, then joined under one root
 *   unit BYTES                    queries run on the parts of the syntax tree no larger than this (default 65536), in
 *                                 reading order: a pattern matches inside one record, never across a whole file,
 *                                 so the work is bounded by the record however many records there are
 *   itself CHAR                   a character the source writes, in an object, for the subject's own codepoint
 *   subject-attribute A [F L]     for @subject.attr: the subject is the codepoint in sibling attribute A of the captured
 *                                 node's parent, or, when the element carries F and L instead, the element itself
 *                                 (a range is attested at its element, never copied to each codepoint)
 * With a grammar, a file is recorded as its syntax tree: each node the composition of its children with the bytes
 * between them kept as text, so it recomposes byte for byte. Leaves are text, decomposed by UAX #29. */
#define _GNU_SOURCE
#include "engine.h"
#include <tree_sitter/api.h>
#include <math.h>
#include <dirent.h>
#include <dlfcn.h>
#include <glob.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include <omp.h>
#include <regex.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Ctx *ctx_here(void){ return CTX[omp_get_thread_num()]; }

/* ---- recipes */
static const TSLanguage *grammar_load(const char *name){
    const char *dir = laplace_grammars();
    char p[1024], sym[128]; snprintf(p, sizeof p, "%s/libtree-sitter-%s.so", dir, name); snprintf(sym, sizeof sym, "tree_sitter_%s", name);
    void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL); if (!h) { fprintf(stderr, "grammar %s: %s\n", name, dlerror()); return NULL; }
    const TSLanguage *(*f)(void) = (const TSLanguage *(*)(void))dlsym(h, sym);
    return f ? f() : NULL;
}
static Block *block_new(Recipe *r, int is_map){
    r->block = xrealloc(r->block, sizeof(Block) * (size_t)(r->nblock + 1)); Block *b = &r->block[r->nblock++];
    memset(b, 0, sizeof *b); b->is_map = is_map; b->enter_rating = 1500.0f; b->enter_deviation = 350.0f;
    snprintf(b->predicate, sizeof b->predicate, "%s", r->predicate);
    return b;
}
static int part_named(const char *s){ return !s ? -1 : !strcmp(s, "subject") ? 0 : !strcmp(s, "predicate") ? 1 : !strcmp(s, "object") ? 2 : !strcmp(s, "key") ? 3 : !strcmp(s, "value") ? 4 : -1; }
/* A path with $NAME read from the environment. */
static void path_expand(const char *in, char *out, size_t cap){
    size_t k = 0;
    for (const char *c = in; *c && k + 1 < cap; ) {
        if (*c != '$') { out[k++] = *c++; continue; }
        char name[64]; size_t l = 0; c++; while ((*c == '_' || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) && l + 1 < sizeof name) name[l++] = *c++; name[l] = 0;
        const char *v = getenv(name); if (!v && !strcmp(name, "LAPLACE_DATA")) v = "/vault/Data";
        if (v) k += (size_t)snprintf(out + k, cap - k, "%s", v);
    }
    out[k < cap ? k : cap - 1] = 0;
}
static void rest_of(char *into, size_t cap);
static char sep_named(const char *t){ return !t ? 0 : !strcmp(t, "tab") ? '\t' : !strcmp(t, "space") ? ' ' : t[0]; }
/* What a table's block says of its rows. Returns 0 if the line is not one of those. */
static int table_says(const char *path, Block *b, char *tok){
    int role = part_named(tok);
    if (role >= 0) { char *rest = strtok(NULL, "\r\n"); if (!rest) return 0;
                     while (*rest == ' ' || *rest == '\t') rest++; size_t l = strlen(rest); while (l && (rest[l - 1] == ' ' || rest[l - 1] == '\t')) rest[--l] = 0;
                     if (!strncmp(rest, "in ", 3)) { rest += 3; while (*rest == ' ') rest++; snprintf(b->in[role], 96, "%s", rest); return 1; }
                     if (role == 1) { snprintf(b->predicate, sizeof b->predicate, "%s", rest); return 1; }      /* the predicate, said outright */
                     fprintf(stderr, "%s: %s in COLUMN\n", path, tok); exit(2); }
    if (!strcmp(tok, "where") && b->nwhere < 8) {
        char *col = strtok(NULL, " \t\r\n"), *op = strtok(NULL, " \t\r\n"), *val = strtok(NULL, "\r\n"); if (!col || !op) return 0;
        while (val && *val == ' ') val++;
        snprintf(b->where[b->nwhere].col, 64, "%s", col); snprintf(b->where[b->nwhere].val, 128, "%s", val ? val : "");
        b->where[b->nwhere].op = !strcmp(op, "is") ? 0 : !strcmp(op, "is-not") ? 1 : !strcmp(op, "matches") ? 2 : -1;
        if (b->where[b->nwhere].op < 0) { fprintf(stderr, "%s: where COLUMN is | is-not | matches\n", path); exit(2); }
        if (b->where[b->nwhere].op == 2) { regex_t *re = malloc(sizeof *re); if (regcomp(re, b->where[b->nwhere].val, REG_EXTENDED | REG_NOSUB)) { fprintf(stderr, "%s: pattern does not compile: %s\n", path, val); exit(2); } b->where[b->nwhere].re = re; }
        b->nwhere++; return 1;
    }
    if (!strcmp(tok, "columns")) { while ((tok = strtok(NULL, " \t\r\n")) && b->ncolumn < 64) snprintf(b->column[b->ncolumn++], 64, "%s", tok); return 1; }
    if (!strcmp(tok, "separator")) { b->separator = sep_named(strtok(NULL, " \t\r\n")); return 1; }
    return 0;
}
static int recipe_parse(const char *path, Recipe *r){
    FILE *f = fopen(path, "r"); if (!f) return 0;
    memset(r, 0, sizeof *r); char line[4096]; int inq = 0, intable = 0; size_t ql = 0; Block *b = NULL;
    while (fgets(line, sizeof line, f)) {
        if (intable) {                                                      /* a map kept in a table: what it says of its rows, up to "end" */
            char *h = strchr(line, '#'); if (h && (h == line || h[-1] == ' ' || h[-1] == '\t')) *h = 0;
            char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
            if (!strcmp(tok, "end")) { intable = 0; b = NULL; continue; }
            if (!table_says(path, b, tok)) { fprintf(stderr, "%s: \"%s\" is not something a table's map says\n", path, tok); fclose(f); return 0; }
            continue;
        }
        if (inq) {
            if (!strncmp(line, "end", 3) && (line[3] == '\n' || line[3] == '\r' || !line[3])) { inq = 0; if (b->is_map) b = NULL; continue; }
            size_t l = strlen(line); b->query_src = xrealloc(b->query_src, ql + l + 1); memcpy(b->query_src + ql, line, l + 1); ql += l; continue;
        }
        if (!strncmp(line, "itself ", 7) && line[7] && line[7] != '\n') { r->itself = line[7]; continue; }
        if (!strncmp(line, "comment ", 8) && line[8] && line[8] != '\n') { r->comment = line[8]; continue; }     /* the character may be the one that begins a comment */
        char *h = strchr(line, '#'); if (h && (h == line || h[-1] == ' ' || h[-1] == '\t')) *h = 0;
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "name")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->name, sizeof r->name, "%s", tok); }
        else if (!strcmp(tok, "match")) while ((tok = strtok(NULL, " \t\r\n")) && r->nmatch < 16) snprintf(r->match[r->nmatch++], 128, "%s", tok);
        else if (!strcmp(tok, "grammar")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(r->grammar, sizeof r->grammar, "%s", tok); }
        else if (!strcmp(tok, "trust")) { tok = strtok(NULL, " \t\r\n"); if (tok) r->trust = atof(tok); }
        else if (!strcmp(tok, "deviation")) { tok = strtok(NULL, " \t\r\n"); if (tok) { double phi = atof(tok) / LP_GLICKO_SCALE; r->trust = 1.0 / sqrt(1.0 + 3.0 * phi * phi / (M_PI * M_PI)); } }
        else if (!strcmp(tok, "map")) {
            tok = strtok(NULL, " \t\r\n"); b = block_new(r, 1); if (tok) snprintf(b->name, sizeof b->name, "%s", tok); inq = 1; ql = 0;
            char *from = strtok(NULL, " \t\r\n"), *file = from && !strcmp(from, "from") ? strtok(NULL, " \t\r\n") : NULL, *gr = file ? strtok(NULL, " \t\r\n") : NULL;
            if (from && !gr) { fprintf(stderr, "%s: map NAME from FILE GRAMMAR\n", path); fclose(f); return 0; }
            if (gr) { path_expand(file, b->from, sizeof b->from);
                      if (!strcmp(gr, "table")) { inq = 0; intable = 1; b->separator = '\t'; snprintf(b->predicate, sizeof b->predicate, "table"); }
                      else { b->lang = grammar_load(gr); if (!b->lang) { fclose(f); return 0; } } }
            else if (!strcmp(r->grammar, "table")) { inq = 0; intable = 1; }
        }
        else if (!strcmp(tok, "separator")) r->separator = sep_named(strtok(NULL, " \t\r\n"));
        else if (!strcmp(tok, "header")) r->header = 1;
        else if (!strcmp(tok, "comment")) { tok = strtok(NULL, " \t\r\n"); r->comment = tok ? tok[0] : '#'; }
        else if (!strcmp(tok, "columns") && !b) { while ((tok = strtok(NULL, " \t\r\n")) && r->ncolumn < 64) snprintf(r->column[r->ncolumn++], 64, "%s", tok); }
        else if (b && !strcmp(r->grammar, "table") && (part_named(tok) >= 0 || !strcmp(tok, "where")) && table_says(path, b, tok)) { }
        else if (!strcmp(tok, "claims")) b = block_new(r, 0);
        else if (!strcmp(tok, "query")) { if (!b || b->query_src) b = block_new(r, 0); inq = 1; ql = 0; }
        else if (!strcmp(tok, "enter") && b) { tok = strtok(NULL, " \t\r\n"); char *d = strtok(NULL, " \t\r\n"); if (tok && d) { b->enter_rating = (float)atof(tok); b->enter_deviation = (float)atof(d); } }
        else if (!strcmp(tok, "ordered") && b) b->ordered = 1;
        else if (!strcmp(tok, "distinct") && b) b->distinct = 1;
        else if (!strcmp(tok, "spaces") && b) { int role = part_named(strtok(NULL, " \t\r\n")); tok = strtok(NULL, " \t\r\n"); if (role >= 0 && tok) b->spaces[role] = tok[0]; }
        else if (!strcmp(tok, "predicate-in-name") && b) { char *x = strtok(NULL, " \t\r\n"), *y = strtok(NULL, " \t\r\n"); if (x && y) { b->name_after = x[0]; b->name_before = y[0]; } }
        else if (!strcmp(tok, "ending") && b) {
            int role = part_named(strtok(NULL, " \t\r\n")); char *from = strtok(NULL, " \t\r\n"), *to = strtok(NULL, " \t\r\n");
            if (role < 0 || !from || !to) { fprintf(stderr, "%s: ending PART FROM TO\n", path); fclose(f); return 0; }
            b->ending = xrealloc(b->ending, sizeof(Say) * (size_t)(b->nending + 1)); Say *y = &b->ending[b->nending++]; y->role = (uint8_t)role;
            snprintf(y->from, sizeof y->from, "%s", from); snprintf(y->to, sizeof y->to, "%s", to);
        }
        else if (!strcmp(tok, "say") && b) {
            int role = part_named(strtok(NULL, " \t\r\n")); char *from = strtok(NULL, " \t\r\n"), *eq = strtok(NULL, " \t\r\n"), *to = strtok(NULL, "\r\n");
            if (role < 0 || !from || !eq || strcmp(eq, "=") || !to) { fprintf(stderr, "%s: say PART FROM = TO\n", path); fclose(f); return 0; }
            while (*to == ' ' || *to == '\t') to++; size_t tl = strlen(to); while (tl && (to[tl - 1] == ' ' || to[tl - 1] == '\t')) to[--tl] = 0;
            b->say = xrealloc(b->say, sizeof(Say) * (size_t)(b->nsay + 1)); Say *y = &b->say[b->nsay++]; y->role = (uint8_t)role;
            snprintf(y->from, sizeof y->from, "%s", from); snprintf(y->to, sizeof y->to, "%s", to);
        }
        else if (!strcmp(tok, "records")) r->records = 1;
        else if (!strcmp(tok, "unit")) { tok = strtok(NULL, " \t\r\n"); if (tok) r->unit = (uint32_t)strtoul(tok, NULL, 10); }
        else if (!strcmp(tok, "witness")) { char *rest = strtok(NULL, "\r\n"); if (rest) { while (*rest == ' ' || *rest == '\t') rest++; snprintf(r->witness, sizeof r->witness, "%s", rest); } }
        else if (!strcmp(tok, "lineage")) { char *rest = strtok(NULL, "\r\n"); if (rest) { while (*rest == ' ' || *rest == '\t') rest++; snprintf(r->lineage, sizeof r->lineage, "%s", rest); } }
        else if (!strcmp(tok, "predicate")) { char said[64] = ""; rest_of(said, sizeof said); size_t l = strlen(said); while (l && (said[l - 1] == ' ' || said[l - 1] == '\t')) said[--l] = 0;
                                              if (said[0]) snprintf(b ? b->predicate : r->predicate, 64, "%s", said); }
        else if (!strcmp(tok, "subject-attribute")) for (int k = 0; k < 3 && (tok = strtok(NULL, " \t\r\n")); k++) snprintf(r->subject_attr[k], 48, "%s", tok);
        else { fprintf(stderr, "%s: \"%s\" is not something a recipe says\n", path, tok); fclose(f); return 0; }
    }
    fclose(f);
    if (!r->grammar[0]) snprintf(r->grammar, sizeof r->grammar, "text");
    if (!r->unit) r->unit = 65536;
    if (!r->separator) r->separator = '\t';
    if (!strcmp(r->grammar, "table")) { for (int k = 0; k < r->nblock; k++) if (!r->block[k].is_map) r->query = (TSQuery *)r; }   /* it attests: a curated source */
    else if (strcmp(r->grammar, "text") && strcmp(r->grammar, "vocabulary")) {
        r->lang = grammar_load(r->grammar); if (!r->lang) return 0;
        for (int k = 0; k < r->nblock; k++) {
            Block *x = &r->block[k]; if (!x->query_src || (x->is_map && x->from[0] && !x->lang)) continue;
            if (!x->is_map && !x->predicate[0] && r->predicate[0]) snprintf(x->predicate, sizeof x->predicate, "%s", r->predicate);
            uint32_t off; TSQueryError err;
            x->query = ts_query_new(x->lang ? x->lang : r->lang, x->query_src, (uint32_t)strlen(x->query_src), &off, &err);
            if (!x->query) { fprintf(stderr, "%s: query error %d at byte %u of block %d\n", path, err, off, k + 1); return 0; }
            if (!x->is_map) r->query = x->query;
        }
    }
    return 1;
}
static Source *sources; static int nsources;
static void rest_of(char *into, size_t cap){ char *rest = strtok(NULL, "\r\n"); if (!rest) return; while (*rest == ' ' || *rest == '\t') rest++; snprintf(into, cap, "%s", rest); }
static int source_parse(const char *path, Source *s){
    FILE *f = fopen(path, "r"); if (!f) return 0;
    memset(s, 0, sizeof *s); char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h && (h == line || h[-1] == ' ' || h[-1] == '\t')) *h = 0;
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "name")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(s->name, sizeof s->name, "%s", tok); }
        else if (!strcmp(tok, "witness")) rest_of(s->witness, sizeof s->witness);
        else if (!strcmp(tok, "lineage")) rest_of(s->lineage, sizeof s->lineage);
        else if (!strcmp(tok, "trust")) { tok = strtok(NULL, " \t\r\n"); if (tok) s->trust = atof(tok); }
        else if (!strcmp(tok, "deviation")) { tok = strtok(NULL, " \t\r\n"); if (tok) { double phi = atof(tok) / LP_GLICKO_SCALE; s->trust = 1.0 / sqrt(1.0 + 3.0 * phi * phi / (M_PI * M_PI)); } }
        else if (!strcmp(tok, "root")) { tok = strtok(NULL, " \t\r\n"); if (tok && s->nroot < 8) path_expand(tok, s->root[s->nroot++], 512); }
        else if (!strcmp(tok, "after")) while ((tok = strtok(NULL, " \t\r\n")) && s->nafter < 16) snprintf(s->after[s->nafter++], 64, "%s", tok);
        else if (!strcmp(tok, "except")) while ((tok = strtok(NULL, " \t\r\n")) && s->nexcept < 8) snprintf(s->except[s->nexcept++], 128, "%s", tok);
        else if (!strcmp(tok, "reads")) while ((tok = strtok(NULL, " \t\r\n")) && s->nreads < 8) snprintf(s->reads[s->nreads++], 64, "%s", tok);
        else { fprintf(stderr, "%s: \"%s\" is not something a source says\n", path, tok); fclose(f); return 0; }
    }
    fclose(f);
    for (int i = 0; i < s->nroot && !s->found[0]; i++) {                   /* the first root that exists; of a pattern, the newest */
        glob_t g; if (!glob(s->root[i], 0, NULL, &g) && g.gl_pathc) snprintf(s->found, sizeof s->found, "%s", g.gl_pathv[g.gl_pathc - 1]);
        globfree(&g);
    }
    return s->name[0] != 0;
}
static int by_name(const void *a, const void *b){ return strcmp(*(char *const *)a, *(char *const *)b); }
static int recipes_in(const char *dir, int source, Recipe **out, int n){
    DIR *d = opendir(dir); if (!d) { perror(dir); return n; }
    struct dirent *de; char **names = NULL; int nn = 0;
    while ((de = readdir(d))) { names = xrealloc(names, sizeof(char *) * (size_t)(nn + 1)); names[nn++] = strdup(de->d_name); }
    closedir(d); qsort(names, (size_t)nn, sizeof(char *), by_name);
    for (int i = 0; i < nn; i++) {
        size_t l = strlen(names[i]); char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (l >= 8 && !strcmp(names[i] + l - 7, ".recipe")) {
            *out = xrealloc(*out, sizeof(Recipe) * (size_t)(n + 1)); Recipe *r = &(*out)[n];
            if (!recipe_parse(p, r)) { fprintf(stderr, "recipe %s does not load; stopping rather than ingesting without it\n", p); exit(2); }
            r->source = source;
            if (source >= 0) { const Source *s = &sources[source];
                if (!r->witness[0]) snprintf(r->witness, sizeof r->witness, "%s", s->witness);
                if (!r->lineage[0]) snprintf(r->lineage, sizeof r->lineage, "%s", s->lineage);
                if (r->trust == 0.0) r->trust = s->trust; }
            n++;
        }
        else if (source < 0 && names[i][0] != '.') {                           /* a source: a directory with a source file */
            char sp[2100]; snprintf(sp, sizeof sp, "%s/source", p); struct stat st;
            if (stat(sp, &st)) continue;
            sources = xrealloc(sources, sizeof(Source) * (size_t)(nsources + 1));
            if (!source_parse(sp, &sources[nsources])) { fprintf(stderr, "source %s does not load\n", sp); exit(2); }
            n = recipes_in(p, nsources++, out, n);
        }
        free(names[i]);
    }
    free(names); return n;
}
/* Sources in the order they go in: each after the sources it names. */
static void sources_order(void){
    Source *o = malloc(sizeof(Source) * (size_t)(nsources ? nsources : 1)); int *at = malloc(sizeof(int) * (size_t)(nsources ? nsources : 1)), *done = calloc((size_t)(nsources ? nsources : 1), sizeof(int)), k = 0;
    while (k < nsources) {
        int moved = 0;
        for (int i = 0; i < nsources; i++) {
            if (done[i]) continue; int ready = 1;
            for (int a = 0; a < sources[i].nafter && ready; a++) { int j = 0; while (j < nsources && strcmp(sources[j].name, sources[i].after[a])) j++;
                if (j == nsources) { fprintf(stderr, "source %s comes after %s, which is not a source\n", sources[i].name, sources[i].after[a]); exit(2); }
                ready = done[j]; }
            if (ready) { at[i] = k; o[k++] = sources[i]; done[i] = 1; moved = 1; }
        }
        if (!moved) { fprintf(stderr, "sources come after one another in a circle\n"); exit(2); }
    }
    memcpy(sources, o, sizeof(Source) * (size_t)nsources); free(o); free(done);
    extern Recipe *recipes_now; extern int nrecipes_now;
    for (int i = 0; i < nrecipes_now; i++) if (recipes_now[i].source >= 0) recipes_now[i].source = at[recipes_now[i].source];
    free(at);
}
Recipe *recipes_now; int nrecipes_now;
int recipes_load(const char *dir, Recipe **out){
    if (recipes_now) { *out = recipes_now; return nrecipes_now; }
    Recipe *r = NULL; int n = recipes_in(dir, -1, &r, 0);
    recipes_now = r; nrecipes_now = n; sources_order();
    *out = r; return n;
}
Source *sources_loaded(int *n){ *n = nsources; return sources; }
static int reads(const Source *s, const Recipe *r){ for (int i = 0; i < s->nreads; i++) if (!strcmp(s->reads[i], r->name)) return 1; return 0; }
Recipe *recipe_for(Recipe *r, int n, const char *path, const Source *of){
    const char *b0 = strrchr(path, '/'); b0 = b0 ? b0 + 1 : path;
    char base[1024]; snprintf(base, sizeof base, "%s", b0);
    size_t bl = strlen(base); if (bl > 3 && !strcmp(base + bl - 3, ".gz")) base[bl - 3] = 0;   /* matched by what it holds */
    Recipe *best = NULL; size_t best_lit = 0;                              /* the most specific pattern wins */
    for (int i = 0; of && i < of->nexcept; i++) if (!fnmatch(of->except[i], path, 0)) return NULL;
    for (int i = 0; i < n; i++) {
        if (of && !(r[i].source >= 0 && &sources[r[i].source] == of) && !(r[i].source < 0 && reads(of, &r[i]))) continue;
        for (int j = 0; j < r[i].nmatch; j++) {
            if (fnmatch(r[i].match[j], base, 0)) continue;
            size_t lit = 0; for (const char *c = r[i].match[j]; *c; c++) lit += !strchr("*?[]", *c);
            if (!best || lit > best_lit) { best = &r[i]; best_lit = lit; }
        }
    }
    return best;
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
        k += lp_utf8_put(cp, o + k);
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
/* #match?: POSIX extended regular expressions, each compiled once per thread. */
typedef struct { const TSQuery *q; uint32_t id; regex_t re; } Rx;
static __thread Rx *rxs; static __thread int nrx;
static int pattern_matches(const TSQuery *q, uint32_t id, const uint8_t *p, size_t n){
    Rx *x = NULL; for (int i = 0; i < nrx; i++) if (rxs[i].q == q && rxs[i].id == id) { x = &rxs[i]; break; }
    if (!x) {
        uint32_t l; const char *pat = ts_query_string_value_for_id(q, id, &l);
        rxs = xrealloc(rxs, sizeof(Rx) * (size_t)(nrx + 1)); x = &rxs[nrx++]; x->q = q; x->id = id;
        if (regcomp(&x->re, pat, REG_EXTENDED | REG_NOSUB)) { fprintf(stderr, "a recipe's #match? pattern does not compile: %s\n", pat); exit(2); }
    }
    char stack[256], *z = n < sizeof stack ? stack : malloc(n + 1); memcpy(z, p, n); z[n] = 0;
    int hit = !regexec(&x->re, z, 0, NULL, 0);
    if (z != stack) free(z);
    return hit;
}
static int predicates_hold(const TSQuery *q, const TSQueryMatch *m, const uint8_t *src){
    uint32_t ns; const TSQueryPredicateStep *st = ts_query_predicates_for_pattern(q, m->pattern_index, &ns);
    for (uint32_t i = 0; i < ns; ) {
        uint32_t j = i; while (j < ns && st[j].type != TSQueryPredicateStepTypeDone) j++;
        if (j > i && st[i].type == TSQueryPredicateStepTypeString) {
            uint32_t l; const char *op = ts_query_string_value_for_id(q, st[i].value_id, &l);
            int neg = !strncmp(op, "not-", 4), any = strstr(op, "any-of") != NULL, rx = strstr(op, "match") != NULL;
            if (j - i >= 3 && st[i + 1].type == TSQueryPredicateStepTypeCapture) {
                TSNode cn = { 0 }; int found = 0;
                for (uint16_t c = 0; c < m->capture_count; c++) if (m->captures[c].index == st[i + 1].value_id) { cn = m->captures[c].node; found = 1; break; }
                if (found && rx && st[i + 2].type == TSQueryPredicateStepTypeString) {
                    const uint8_t *p = src + ts_node_start_byte(cn); size_t n = ts_node_end_byte(cn) - ts_node_start_byte(cn); unquote(&p, &n);
                    if (pattern_matches(q, st[i + 2].value_id, p, n) == neg) return 0;
                }
                else if (found) {
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
static void ev_push(Events *e, const Event *x){                         /* by address: a copy per call, in a loop, is stack that is never given back */
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 65536; e->e = xrealloc(e->e, e->cap * sizeof(Event)); }
    e->e[e->n++] = *x;
}

/* ---- maps: what an identifier of the source stands for, read before anything is attested */
typedef struct { uint64_t h; uint32_t koff, klen, voff, vlen; } MEnt;
typedef struct { MEnt *t; uint64_t cap, n; uint8_t *pool; size_t pn, pcap; } Map;
static void map_put(Map *m, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl){
    if ((m->n + 1) * 2 > m->cap) {
        uint64_t oc = m->cap; MEnt *old = m->t; m->cap = oc ? oc * 2 : 4096; m->t = calloc(m->cap, sizeof(MEnt));
        for (uint64_t i = 0; i < oc; i++) if (old[i].h) { uint64_t x = old[i].h & (m->cap - 1); while (m->t[x].h) x = (x + 1) & (m->cap - 1); m->t[x] = old[i]; }
        free(old);
    }
    uint64_t h = fnv(k, kl), x = h & (m->cap - 1);
    while (m->t[x].h) { if (m->t[x].h == h && m->t[x].klen == kl && !memcmp(m->pool + m->t[x].koff, k, kl)) return; x = (x + 1) & (m->cap - 1); }   /* the first binding holds */
    if (m->pn + kl + vl > m->pcap) { m->pcap = (m->pn + kl + vl) * 2 + 65536; m->pool = xrealloc(m->pool, m->pcap); }
    memcpy(m->pool + m->pn, k, kl); memcpy(m->pool + m->pn + kl, v, vl);
    m->t[x] = (MEnt){ h, (uint32_t)m->pn, (uint32_t)kl, (uint32_t)(m->pn + kl), (uint32_t)vl }; m->pn += kl + vl; m->n++;
}
static int map_get(const Map *m, const uint8_t *k, size_t kl, const uint8_t **v, size_t *vl){
    if (!m->cap) return 0;
    uint64_t h = fnv(k, kl), x = h & (m->cap - 1);
    while (m->t[x].h) { if (m->t[x].h == h && m->t[x].klen == kl && !memcmp(m->pool + m->t[x].koff, k, kl)) { *v = m->pool + m->t[x].voff; *vl = m->t[x].vlen; return 1; } x = (x + 1) & (m->cap - 1); }
    return 0;
}
/* What a file is read with: its recipe, and the maps its recipe's map blocks filled (one per block). */
typedef struct { const Recipe *r; Map *map; char (*predicate)[64]; } Reading;
static __thread long subject_cp = -1;                                  /* the codepoint the claim being read is about, when it is one */

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
/* A captured node as the text it stands for: the first resolver reads it, each further one looks it up in a map.
 * Returns 0 when it is empty or a map does not hold it. */
static int part_said(const Reading *rd, const Block *b, int role, const char *suffix, uint8_t *buf, const uint8_t **p, size_t *n);
static int part_text(const Reading *rd, const Block *b, int role, const char *suffix, TSNode nd, const uint8_t *src, uint8_t *buf, const uint8_t **p, size_t *n){
    *p = src + ts_node_start_byte(nd); *n = ts_node_end_byte(nd) - ts_node_start_byte(nd);
    unquote(p, n);
    return part_said(rd, b, role, suffix, buf, p, n);
}
static int part_said(const Reading *rd, const Block *b, int role, const char *suffix, uint8_t *buf, const uint8_t **p, size_t *n){
    while (*n && ((*p)[0] == ' ' || (*p)[0] == '\t' || (*p)[0] == '\r' || (*p)[0] == '\n')) { (*p)++; (*n)--; }
    while (*n && ((*p)[*n - 1] == ' ' || (*p)[*n - 1] == '\t' || (*p)[*n - 1] == '\r' || (*p)[*n - 1] == '\n')) (*n)--;
    if (!*n) return 0;                                                  /* an empty value is no content */
    char chain[64]; snprintf(chain, sizeof chain, "%s", suffix); char *save = NULL, *res = strtok_r(chain, ".", &save);
    if (res && !strcmp(res, "xml")) { size_t l = xml_unescape(*p, *n, buf); if (!l) return 0; *p = buf; *n = l; }
    else if (res && !strcmp(res, "cps")) {                              /* codepoints in hex, as the text they are */
        size_t k = 0; const uint8_t *c = *p, *e = *p + *n; uint8_t tmp[512];
        while (c < e && k + 4 < sizeof tmp) {
            while (c < e && *c == ' ') c++; if (c >= e) break;
            const uint8_t *t0 = c; while (c < e && *c != ' ') c++;
            long cp; if (c - t0 == 1 && rd->r->itself && *t0 == (uint8_t)rd->r->itself) cp = subject_cp;
            else { char h[16]; if (c - t0 > 8) return 0; size_t o = (size_t)(c - t0); const uint8_t *hx = t0; if (o > 2 && hx[0] == 'U' && hx[1] == '+') { hx += 2; o -= 2; }
                   memcpy(h, hx, o); h[o] = 0; char *end; cp = strtol(h, &end, 16); if (*end) return 0; }
            if (cp < 0 || cp >= (long)LP_NCP) return 0;
            k += lp_utf8_put((uint32_t)cp, tmp + k);
        }
        if (!k) return 0; memcpy(buf, tmp, k); *p = buf; *n = k;
    }
    else if (res && !strcmp(res, "iri")) { if (*n < 3 || (*p)[0] != '<' || (*p)[*n - 1] != '>') return 0; (*p)++; *n -= 2; }
    else if (res && !strcmp(res, "head")) { const uint8_t *c = memchr(*p, ':', *n); if (!c || c == *p) return 0; *n = (size_t)(c - *p); }
    else if (res && strcmp(res, "text")) { save = NULL; snprintf(chain, sizeof chain, "%s", suffix); res = NULL; }     /* the first is already a map */
    if (role == 2 && rd->r->itself && subject_cp >= 0 && res && strcmp(res, "cps") && memchr(*p, rd->r->itself, *n)) {   /* the codepoint, written out */
        char hex[16]; int hl = snprintf(hex, sizeof hex, "%04lX", subject_cp); size_t k = 0; uint8_t *o = malloc(*n * 8 + 8);
        for (size_t i = 0; i < *n; i++) { if ((*p)[i] == (uint8_t)rd->r->itself) { memcpy(o + k, hex, (size_t)hl); k += (size_t)hl; } else o[k++] = (*p)[i]; }
        memcpy(buf, o, k); free(o); *p = buf; *n = k;
    }
    for (int i = 0; i < b->nending; i++) {
        const Say *e = &b->ending[i]; size_t fl = strlen(e->from), tl = strlen(e->to);
        if (e->role != role || *n < fl || memcmp(*p + *n - fl, e->from, fl)) continue;
        if (*p != buf) memmove(buf, *p, *n);
        memcpy(buf + *n - fl, e->to, tl); *p = buf; *n = *n - fl + tl; break;
    }
    for (char *name = res ? strtok_r(NULL, ".", &save) : strtok_r(chain, ".", &save); name; name = strtok_r(NULL, ".", &save)) {
        int k = 0; while (k < rd->r->nblock && !(rd->r->block[k].is_map && !strcmp(rd->r->block[k].name, name))) k++;
        if (k == rd->r->nblock) { fprintf(stderr, "%s: no map named %s\n", rd->r->name, name); exit(2); }
        if (!map_get(&rd->map[k], *p, *n, p, n)) return 0;
    }
    return *n > 0;
}
/* A part as the source means it: the character it writes for a space, and what it abbreviates. */
static Ref part_ref(const Block *b, int role, const uint8_t *p, size_t n){
    for (int i = 0; i < b->nsay; i++) if (b->say[i].role == role && strlen(b->say[i].from) == n && !memcmp(b->say[i].from, p, n))
        return string_ref((const uint8_t *)b->say[i].to, strlen(b->say[i].to));
    if (b->spaces[role] && memchr(p, b->spaces[role], n)) {
        uint8_t stack[256], *z = n <= sizeof stack ? stack : malloc(n); memcpy(z, p, n);
        for (size_t i = 0; i < n; i++) if (z[i] == (uint8_t)b->spaces[role]) z[i] = ' ';
        Ref r = string_ref(z, n); if (z != stack) free(z); return r;
    }
    return string_ref(p, n);
}
static int capture_role(const char *name, uint32_t l, const char *what, char *suf, size_t cap){
    size_t wl = strlen(what); if (l < wl || memcmp(name, what, wl) || (l > wl && name[wl] != '.')) return 0;
    snprintf(suf, cap, "%.*s", l > wl ? (int)(l - wl - 1) : 4, l > wl ? name + wl + 1 : "text");
    return 1;
}

/* Every match of a map's patterns inside one node binds a key to a value. */
static void map_node(const Reading *rd, int k, TSQueryCursor *qc, TSNode nd, const uint8_t *src, uint8_t *buf){
    const Block *b = &rd->r->block[k]; ts_query_cursor_exec(qc, b->query, nd); TSQueryMatch m; uint8_t *kb = NULL; size_t kcap = 0;
    while (ts_query_cursor_next_match(qc, &m)) {
        if (!predicates_hold(b->query, &m, src)) continue;
        const uint8_t *kp = NULL, *vp = NULL; size_t kn = 0, vn = 0; int ok = 1;
        for (uint16_t c = 0; c < m.capture_count && ok; c++) {
            uint32_t l; const char *name = ts_query_capture_name_for_id(b->query, m.captures[c].index, &l); char suf[64];
            if (capture_role(name, l, "key", suf, sizeof suf)) {
                ok = part_text(rd, b, 3, suf, m.captures[c].node, src, buf, &kp, &kn);
                if (ok) { if (kn > kcap) { kcap = kn * 2; kb = xrealloc(kb, kcap); } memcpy(kb, kp, kn); kp = kb; }       /* buf is used again for the value */
            }
            else if (capture_role(name, l, "value", suf, sizeof suf)) ok = part_text(rd, b, 4, suf, m.captures[c].node, src, buf, &vp, &vn);
        }
        if (ok && kp && vp) map_put(&rd->map[k], kp, kn, vp, vn);
    }
    free(kb);
}
/* Every match of a kind's patterns inside one node attests one claim. */
static void attest_node(const Reading *rd, int k, TSQueryCursor *qc, TSNode nd, const uint8_t *src, uint8_t *buf, Events *ev){
    const Recipe *r = rd->r; const Block *b = &r->block[k]; uint32_t position = 0;
    ts_query_cursor_exec(qc, b->query, nd); TSQueryMatch m;
    while (ts_query_cursor_next_match(qc, &m)) {
        if (!predicates_hold(b->query, &m, src)) continue;
        Ref part[3]; int have[3] = { 0, 0, 0 }, void_match = 0; subject_cp = -1;
        for (int pass = 0; pass < 2 && !void_match; pass++)                     /* the subject first: the rest may speak of it */
        for (uint16_t c = 0; c < m.capture_count && !void_match; c++) {
            uint32_t l; const char *name = ts_query_capture_name_for_id(b->query, m.captures[c].index, &l); char suf[64];
            int role = capture_role(name, l, "subject", suf, sizeof suf) ? 0 : capture_role(name, l, "predicate", suf, sizeof suf) ? 1
                     : capture_role(name, l, "object", suf, sizeof suf) ? 2 : -1;
            if (role < 0 || (role == 0) != (pass == 0)) continue;
            TSNode cn = m.captures[c].node;
            if (!strcmp(suf, "attr")) {                                          /* the subject named by a sibling attribute */
                TSNode an = cn; if (!strcmp(ts_node_type(an), "Attribute")) an = ts_node_named_child(an, 0);
                uint32_t nl; const uint8_t *np = src + ts_node_start_byte(an); nl = ts_node_end_byte(an) - ts_node_start_byte(an);
                int own = 0; for (int z = 0; z < 3 && !own; z++) own = r->subject_attr[z][0] && strlen(r->subject_attr[z]) == nl && !memcmp(r->subject_attr[z], np, nl);
                if (own) { void_match = 1; break; }                            /* the subject's own attributes attest nothing */
                have[role] = resolve_subject_attr(r, cn, src, &part[role]);
                if (have[role] && part[role].tier == 0) subject_cp = (long)lp_tier0_codepoint(T0, &part[role].id);
                continue;
            }
            if (!strcmp(suf, "node")) { part[role] = ast_node(cn, src, ts_node_start_byte(cn), ts_node_end_byte(cn), 0); have[role] = 1; continue; }
            if (!strcmp(suf, "cp")) {
                const uint8_t *p = src + ts_node_start_byte(cn); size_t n = ts_node_end_byte(cn) - ts_node_start_byte(cn); unquote(&p, &n);
                char h[16]; if (!n || n > 8) continue; memcpy(h, p, n); h[n] = 0; char *e; unsigned long v = strtoul(h, &e, 16);
                if (*e || v >= LP_NCP) continue; part[role] = atom((uint32_t)v); have[role] = 1; continue;
            }
            const uint8_t *p; size_t n;
            if (part_text(rd, b, role, suf, cn, src, buf, &p, &n)) { part[role] = part_ref(b, role, p, n); have[role] = 1; }
        }
        if (void_match) continue;
        if (!have[1] && rd->predicate[k][0]) { part[1] = string_ref((const uint8_t *)rd->predicate[k], strlen(rd->predicate[k])); have[1] = 1; }
        if (!have[0] || !have[1] || !have[2]) continue;
        if (b->distinct && !memcmp(&part[0].id, &part[2].id, 16)) continue;
        uint8_t t = 0; for (int i = 0; i < 3; i++) if (part[i].tier > t) t = part[i].tier;
        Ref claim = compose(part, 3, (uint8_t)(t + 1));
        Event x = { claim.id, 1.0f, b->enter_rating, b->enter_deviation, b->ordered ? ++position : 0 };
        ev_push(ev, &x);                                                     /* the source asserts it: a win */
    }
}
/* The tree in reading order, down to the parts no larger than the recipe's unit; the patterns run inside each. A query
 * over a whole file would pair every record with every other before any predicate could tell them apart. */
static void read_units(const Reading *rd, int maps, TSQueryCursor *qc, TSNode nd, const uint8_t *src, uint8_t *buf, Events *ev, uint64_t *units){
    const Recipe *r = rd->r;
    if (ts_node_end_byte(nd) - ts_node_start_byte(nd) <= r->unit || ts_node_child_count(nd) == 0) {
        double t = now(); int over = 0;
        for (int k = 0; k < r->nblock; k++) {
            if (!r->block[k].query || r->block[k].is_map != maps || r->block[k].from[0]) continue;
            if (maps) map_node(rd, k, qc, nd, src, buf); else attest_node(rd, k, qc, nd, src, buf, ev);
            over |= ts_query_cursor_did_exceed_match_limit(qc);
        }
        units[0]++; t = now() - t;
        if (over && !units[1]++)                                            /* matches were dropped: say so, never silently */
            fprintf(stderr, "\n  %s: a unit of %u bytes at byte %u (%s) holds more partial matches than a query keeps: a pattern pairs "
                            "siblings it should name by position; what it attests is incomplete\n", r->name,
                    ts_node_end_byte(nd) - ts_node_start_byte(nd), ts_node_start_byte(nd), ts_node_type(nd));
        if (t > 0.25) fprintf(stderr, "\n  %s: a unit of %u bytes at byte %u (%s, %u children) took %.2f s\n", r->name, ts_node_end_byte(nd) - ts_node_start_byte(nd),
                              ts_node_start_byte(nd), ts_node_type(nd), ts_node_child_count(nd), t);
        return;
    }
    uint32_t nc = ts_node_child_count(nd); TSNode *kid = malloc(sizeof(TSNode) * nc); uint32_t k = 0;
    TSTreeCursor cur = ts_tree_cursor_new(nd);
    if (ts_tree_cursor_goto_first_child(&cur)) do kid[k++] = ts_tree_cursor_current_node(&cur); while (k < nc && ts_tree_cursor_goto_next_sibling(&cur));
    ts_tree_cursor_delete(&cur);
    if (!maps && k >= 256 && ts_node_end_byte(nd) - ts_node_start_byte(nd) > (4u << 20)) {
        /* many records: read on every core, each stretch with its own cursor, and their attestations joined in order */
        int nt = omp_get_num_threads() * 8; if (nt > (int)k) nt = (int)k; if (nt < 1) nt = 1;
        Events *pe = calloc((size_t)nt, sizeof(Events)); uint64_t (*pu)[2] = calloc((size_t)nt, sizeof *pu); size_t blen = (size_t)rd->r->unit * 8 + 68;                 /* a part of one unit, written out at its longest */
        #pragma omp taskloop grainsize(1)
        for (int t = 0; t < nt; t++) {
            TSQueryCursor *q2 = ts_query_cursor_new(); ts_query_cursor_set_match_limit(q2, 1u << 14); uint8_t *b2 = malloc(blen);
            for (uint32_t i = (uint32_t)((uint64_t)k * t / nt); i < (uint32_t)((uint64_t)k * (t + 1) / nt); i++) read_units(rd, maps, q2, kid[i], src, b2, &pe[t], pu[t]);
            free(b2); ts_query_cursor_delete(q2);
        }
        for (int t = 0; t < nt; t++) { for (uint64_t j = 0; j < pe[t].n; j++) ev_push(ev, &pe[t].e[j]); free(pe[t].e); units[0] += pu[t][0]; units[1] += pu[t][1]; }
        free(pe); free(pu); free(kid); return;
    }
    for (uint32_t i = 0; i < k; i++) read_units(rd, maps, qc, kid[i], src, buf, ev, units);
    free(kid);
}
static uint8_t *read_all(const char *path, size_t *n);

/* ---- tables: rows of fields, read natively. Columns are named by the first row or by the recipe. */
typedef struct { const uint8_t *p; size_t n; } Cell;
typedef struct { int in[5], where[8]; } Cols;                           /* per block: where each part and condition is, or -1 */
static int column_named(char (*name)[64], int n, const char *spec){
    size_t l = strcspn(spec, ".");
    for (int i = 0; i < n; i++) if (strlen(name[i]) == l && !memcmp(name[i], spec, l)) return i;
    return -1;
}
static Cols *cols_for(const Recipe *r, char (*name)[64], int n, int only){
    Cols *c = calloc((size_t)(r->nblock ? r->nblock : 1), sizeof(Cols));
    for (int k = 0; k < r->nblock; k++) {
        const Block *b = &r->block[k]; if (only >= 0 && k != only) continue;
        for (int i = 0; i < 5; i++) { c[k].in[i] = b->in[i][0] ? column_named(name, n, b->in[i]) : -1;
            if (b->in[i][0] && c[k].in[i] < 0 && !(b->is_map && b->from[0] && only < 0)) { fprintf(stderr, "%s: there is no column %s\n", r->name, b->in[i]); exit(2); } }
        for (int i = 0; i < b->nwhere; i++) { c[k].where[i] = column_named(name, n, b->where[i].col);
            if (c[k].where[i] < 0 && !(b->is_map && b->from[0] && only < 0)) { fprintf(stderr, "%s: there is no column %s\n", r->name, b->where[i].col); exit(2); } }
    }
    return c;
}
static const char *resolvers_of(const char *spec){ const char *d = strchr(spec, '.'); return d ? d + 1 : "text"; }
static int row_is_spoken_of(const Block *b, const Cols *c, const Cell *cell, int ncell){
    for (int i = 0; i < b->nwhere; i++) {
        Cell x = c->where[i] < ncell ? cell[c->where[i]] : (Cell){ NULL, 0 }; size_t vl = strlen(b->where[i].val);
        int same = x.n == vl && !memcmp(x.p, b->where[i].val, vl);
        if (b->where[i].op == 0 && !same) return 0;
        if (b->where[i].op == 1 && same) return 0;
        if (b->where[i].op == 2) { char st[256], *z = x.n < sizeof st ? st : malloc(x.n + 1); memcpy(z, x.p, x.n); z[x.n] = 0; int hit = !regexec(b->where[i].re, z, 0, NULL, 0); if (z != st) free(z); if (!hit) return 0; }
    }
    return 1;
}
/* The rows between two offsets: for a map, each binds a key to a value; otherwise each attests what its blocks say. */
static void table_rows(const Reading *rd, const Cols *cols, int maps, int only, char sep, char comment, const uint8_t *src, size_t lo, size_t hi, Events *ev){
    const Recipe *r = rd->r; Cell cell[64]; uint8_t *buf = malloc((size_t)r->unit * 8 + 68), *kb = malloc((size_t)r->unit * 8 + 68);
    for (size_t at = lo; at < hi; ) {
        const uint8_t *nl = memchr(src + at, '\n', hi - at); size_t e = nl ? (size_t)(nl - src) : hi, next = e + 1; if (e > at && src[e - 1] == '\r') e--;
        if (e == at || (comment && src[at] == (uint8_t)comment) || e - at > r->unit) { at = next; continue; }
        int nc = 0; for (size_t i = at, f0 = at; i <= e && nc < 64; i++) if (i == e || src[i] == (uint8_t)sep) { cell[nc++] = (Cell){ src + f0, i - f0 }; f0 = i + 1; }
        for (int k = 0; k < r->nblock; k++) {
            const Block *b = &r->block[k]; if ((only >= 0 && k != only) || b->is_map != maps || (only < 0 && b->is_map && b->from[0])) continue;
            if (!row_is_spoken_of(b, &cols[k], cell, nc)) continue;
            if (maps) {
                const uint8_t *kp, *vp; size_t kn, vn; int ki = cols[k].in[3], vi = cols[k].in[4]; if (ki < 0 || vi < 0 || ki >= nc || vi >= nc) continue;
                kp = cell[ki].p; kn = cell[ki].n; if (!part_said(rd, b, 3, resolvers_of(b->in[3]), kb, &kp, &kn)) continue;
                if (kp != kb) { memcpy(kb, kp, kn); kp = kb; }
                vp = cell[vi].p; vn = cell[vi].n; if (!part_said(rd, b, 4, resolvers_of(b->in[4]), buf, &vp, &vn)) continue;
                map_put(&rd->map[k], kp, kn, vp, vn); continue;
            }
            Ref part[3]; int have[3] = { 0, 0, 0 }; subject_cp = -1;
            for (int role = 0; role < 3; role++) {
                int ci = cols[k].in[role]; if (ci < 0 || ci >= nc) continue;
                const uint8_t *p = cell[ci].p; size_t n = cell[ci].n;
                if (part_said(rd, b, role, resolvers_of(b->in[role]), buf, &p, &n)) { part[role] = part_ref(b, role, p, n); have[role] = 1;
                    if (role == 0 && part[0].tier == 0) subject_cp = (long)lp_tier0_codepoint(T0, &part[0].id); }
            }
            if (!have[1] && rd->predicate[k][0]) { part[1] = string_ref((const uint8_t *)rd->predicate[k], strlen(rd->predicate[k])); have[1] = 1; }
            if (!have[0] || !have[1] || !have[2]) continue;
            if (b->distinct && !memcmp(&part[0].id, &part[2].id, 16)) continue;
            uint8_t t = 0; for (int i = 0; i < 3; i++) if (part[i].tier > t) t = part[i].tier;
            Ref claim = compose(part, 3, (uint8_t)(t + 1));
            Event x = { claim.id, 1.0f, b->enter_rating, b->enter_deviation, 0 }; ev_push(ev, &x);
        }
        at = next;
    }
    free(buf); free(kb);
}
/* A table's columns: the recipe's, or the block's, or its first row's. Returns where its rows begin. */
static size_t table_columns(const uint8_t *src, size_t n, char sep, char comment, int header, char (*given)[64], int ngiven, char (*name)[64], int *nn){
    size_t at = 0; *nn = 0;
    for (int i = 0; i < ngiven; i++) snprintf(name[(*nn)++], 64, "%s", given[i]);
    if (!header || ngiven) return 0;
    while (at < n && comment && src[at] == (uint8_t)comment) { const uint8_t *nl = memchr(src + at, '\n', n - at); at = nl ? (size_t)(nl - src) + 1 : n; }
    const uint8_t *nl = memchr(src + at, '\n', n - at); size_t e = nl ? (size_t)(nl - src) : n, next = e + 1; if (e > at && src[e - 1] == '\r') e--;
    for (size_t i = at, f0 = at; i <= e && *nn < 64; i++) if (i == e || src[i] == (uint8_t)sep) { snprintf(name[(*nn)++], 64, "%.*s", (int)(i - f0 > 63 ? 63 : i - f0), src + f0); f0 = i + 1; }
    return next < n ? next : n;
}
static void attest_table(const Recipe *r, const char *path, const uint8_t *src, size_t n, Events *ev);

/* A map kept in another file, read once with that file's grammar. */
static const Map *map_from(const Recipe *r, int k){
    Block *b = &r->block[k];
    #pragma omp critical(map_from)
    if (!b->cache) {
        if (strchr(b->from, '*')) { glob_t g; if (!glob(b->from, 0, NULL, &g) && g.gl_pathc) snprintf(b->from, sizeof b->from, "%s", g.gl_pathv[g.gl_pathc - 1]); globfree(&g); }   /* the newest */
        size_t n; uint8_t *src = read_all(b->from, &n); if (!src) { perror(b->from); fprintf(stderr, "%s: map %s cannot be read\n", r->name, b->name); exit(2); }
        if (!b->lang) {                                                         /* a table */
            char name[64][64]; int nn; size_t at = table_columns(src, n, b->separator ? b->separator : '\t', r->comment, !b->ncolumn, b->column, b->ncolumn, name, &nn);
            Reading rd = { r, calloc((size_t)r->nblock, sizeof(Map)), NULL }; Cols *cols = cols_for(r, name, nn, k);
            table_rows(&rd, cols, 1, k, b->separator ? b->separator : '\t', r->comment, src, at, n, NULL);
            Map *m = malloc(sizeof *m); *m = rd.map[k]; b->cache = m; free(rd.map); free(cols); free(src);
        } else {
        TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, b->lang);
        TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
        Recipe whole = *r; whole.unit = r->unit; Reading rd = { &whole, calloc((size_t)r->nblock, sizeof(Map)), NULL };
        TSQueryCursor *qc = ts_query_cursor_new(); uint8_t *buf = malloc(n + 4); ts_query_cursor_set_match_limit(qc, 1u << 14);
        TSTreeCursor cur = ts_tree_cursor_new(ts_tree_root_node(t));           /* its records, one at a time */
        if (ts_tree_cursor_goto_first_child(&cur)) do map_node(&rd, k, qc, ts_tree_cursor_current_node(&cur), src, buf); while (ts_tree_cursor_goto_next_sibling(&cur));
        ts_tree_cursor_delete(&cur);
        Map *m = malloc(sizeof *m); *m = rd.map[k]; b->cache = m;
        free(rd.map); free(buf); ts_query_cursor_delete(qc); ts_tree_delete(t); ts_parser_delete(ps); free(src);
        }
    }
    return b->cache;
}
static void attest_tree(const Recipe *r, const char *path, TSNode root, const uint8_t *src, size_t n, Events *ev, uint64_t *incomplete){
    Reading rd = { r, calloc((size_t)(r->nblock ? r->nblock : 1), sizeof(Map)), calloc((size_t)(r->nblock ? r->nblock : 1), 64) }; int has_maps = 0;
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    for (int k = 0; k < r->nblock; k++) {
        const Block *b = &r->block[k]; snprintf(rd.predicate[k], 64, "%s", b->predicate);
        if (b->name_after) { const char *x = strrchr(base, b->name_after), *y = x ? strchr(x + 1, b->name_before) : NULL; if (x && y) snprintf(rd.predicate[k], 64, "%.*s", (int)(y - x - 1), x + 1); }
        if (b->is_map && b->from[0]) rd.map[k] = *map_from(r, k); else has_maps |= b->is_map;
    }
    TSQueryCursor *qc = ts_query_cursor_new(); uint8_t *buf = malloc(n * 2 + 68); uint64_t units[2] = { 0, 0 };
    ts_query_cursor_set_match_limit(qc, 1u << 14);
    if (has_maps) read_units(&rd, 1, qc, root, src, buf, ev, units);
    read_units(&rd, 0, qc, root, src, buf, ev, units);
    #pragma omp atomic
    *incomplete += units[1];
    for (int k = 0; k < r->nblock; k++) if (!r->block[k].from[0]) { free(rd.map[k].t); free(rd.map[k].pool); }
    free(rd.map); free(rd.predicate); free(buf); ts_query_cursor_delete(qc);
}

static void attest_table(const Recipe *r, const char *path, const uint8_t *src, size_t n, Events *ev){
    Reading rd = { r, calloc((size_t)(r->nblock ? r->nblock : 1), sizeof(Map)), calloc((size_t)(r->nblock ? r->nblock : 1), 64) }; int has_maps = 0;
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    for (int k = 0; k < r->nblock; k++) {
        const Block *b = &r->block[k]; snprintf(rd.predicate[k], 64, "%s", b->predicate);
        if (b->name_after) { const char *x = strrchr(base, b->name_after), *y = x ? strchr(x + 1, b->name_before) : NULL; if (x && y) snprintf(rd.predicate[k], 64, "%.*s", (int)(y - x - 1), x + 1); }
        if (b->is_map && b->from[0]) rd.map[k] = *map_from(r, k); else has_maps |= b->is_map;
    }
    char name[64][64]; int nn; size_t at = table_columns(src, n, r->separator, r->comment, r->header, (char (*)[64])r->column, r->ncolumn, name, &nn);
    Cols *cols = cols_for(r, name, nn, -1);
    if (has_maps) table_rows(&rd, cols, 1, -1, r->separator, r->comment, src, at, n, NULL);
    int nt = omp_get_num_threads() * 8; if ((size_t)nt > (n - at) / 65536 + 1) nt = (int)((n - at) / 65536 + 1);
    size_t *cut = malloc(sizeof(size_t) * (size_t)(nt + 1)); cut[0] = at; int k = 1;
    for (int i = 1; i < nt; i++) { size_t c = at + (n - at) / (size_t)nt * (size_t)i; while (c < n && src[c - 1] != '\n') c++; if (c > cut[k - 1] && c < n) cut[k++] = c; }
    cut[k] = n; Events *pe = calloc((size_t)k, sizeof(Events));
    #pragma omp taskloop grainsize(1)
    for (int t = 0; t < k; t++) table_rows(&rd, cols, 0, -1, r->separator, r->comment, src, cut[t], cut[t + 1], &pe[t]);
    for (int t = 0; t < k; t++) { for (uint64_t j = 0; j < pe[t].n; j++) ev_push(ev, &pe[t].e[j]); free(pe[t].e); }   /* in reading order */
    for (int i = 0; i < r->nblock; i++) if (!r->block[i].from[0]) { free(rd.map[i].t); free(rd.map[i].pool); }
    free(pe); free(cut); free(cols); free(rd.map); free(rd.predicate);
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
static void named_for(const char *name, const char *path, char *out, size_t cap){
    const char *at = strstr(name, "{dir}"); if (!at) { snprintf(out, cap, "%s", name); return; }
    const char *e = strrchr(path, '/'), *s = e; while (s && s > path && s[-1] != '/') s--;
    snprintf(out, cap, "%.*s%.*s%s", (int)(at - name), name, e ? (int)(e - s) : 0, e ? s : "", at + 5);
}
void decompose_file(Ctx *c, File *f){
    size_t n; uint8_t *src = read_all(f->path, &n); if (!src) { f->skipped = 1; return; }
    f->bytes = n;
    const Recipe *r = f->recipe;
    if (r && r->query) {                                                        /* the witness, named as content */
        char w[512], l[512]; named_for(r->witness[0] ? r->witness : r->name, f->path, w, sizeof w); named_for(r->lineage, f->path, l, sizeof l);
        f->witness = text_ref(c, (const uint8_t *)w, strlen(w)); f->trunk = f->witness;
        if (l[0]) { f->lineage = text_ref(c, (const uint8_t *)l, strlen(l)); f->has_lineage = 1; }
    }
    if (r && !strcmp(r->grammar, "vocabulary")) {
        uint64_t nb; f->trunk = vocabulary_ref(c, src, n, &f->tokens, &nb);
        if (!f->tokens) f->skipped = 1;                                          /* not a tokenizer's file after all */
    }
    else if (r && !strcmp(r->grammar, "table")) attest_table(r, f->path, src, n, &f->ev);
    else if (!r || !r->lang) f->trunk = text_ref(c, src, n);
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
            if (r->query) attest_tree(r, f->path, root, base, len, &part[i].ev, &f->incomplete);         /* this piece's attestations, in order */
            ts_tree_delete(t); ts_parser_delete(ps);
        }
        size_t tot = 0; uint8_t tm = 0; for (int i = 0; i < k; i++) { tot += part[i].n; if (part[i].t > tm) tm = part[i].t; }
        Ref *all = malloc(sizeof(Ref) * (tot + 1)); size_t m = 0;
        for (int i = 0; i < k; i++) {
            memcpy(all + m, part[i].v, sizeof(Ref) * part[i].n); m += part[i].n; free(part[i].v);
            for (uint64_t j = 0; j < part[i].ev.n; j++) ev_push(&f->ev, &part[i].ev.e[j]);   /* reading order */
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
        else attest_tree(r, f->path, root, src, n, &f->ev, &f->incomplete);                       /* a curated source: what it attests */
        ts_tree_delete(t); ts_parser_delete(ps);
    }
    free(src);
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
    const char *rdir = laplace_recipes(), *grammar = NULL; long nodes = 400; int a = 1;
    for (; a < argc - 1 && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-r") && a + 1 < argc) rdir = argv[++a];
        else if (!strcmp(argv[a], "-g") && a + 1 < argc) grammar = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) nodes = atol(argv[++a]);
    }
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
