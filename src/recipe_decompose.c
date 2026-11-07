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
static Ctx *ctx_here(void){ return CTX[omp_get_thread_num()]; }
static const TSLanguage *grammar_load(const char *name){ return grammar_lang(grammar_id_for(name)); }
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
static uint8_t *read_recipe_input(const Recipe *recipe, const char *path, size_t *length){
    return recipe && recipe->input_frame.width ? input_frame_read(&recipe->input_frame, path, length) : read_all(path, length);
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
    const Recipe *r = f->recipe;
    size_t n; uint8_t *src0 = read_recipe_input(r, f->path, &n); if (!src0) { perror(f->path); return; } uint8_t *src = src0;
    if (r && r->input_frame.width) f->at = r->input_frame.width;
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
    return recipe && !recipe->input_frame.width && recipe->curated && (recipe->native ? recipe->native->input != NULL : say_record_streamable(recipe));
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
    size_t n; uint8_t *src0 = read_recipe_input(r, f->path, &n); if (!src0) { perror(f->path); f->exact = -1; exit(2); }
    if (r && r->input_frame.width) f->at = r->input_frame.width;
    f->bytes = (uint64_t)n + (r ? r->input_frame.width : 0); decompose_bytes(c, f, src0, n, 1);
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
    const TSLanguage *lang = NULL; const Recipe *selected = NULL;
    if (grammar) lang = grammar_load(grammar);
    else { Recipe *rec = NULL; int n = recipes_load(rdir, &rec); Recipe *r = recipe_for(rec, n, argv[a], NULL);
           if (r) { selected = r; lang = r->lang; printf("recipe %s, grammar %s\n", r->name, r->grammar); } }
    if (!lang) { fprintf(stderr, "no tree-sitter grammar reads %s (name one with -g)\n", argv[a]); return 1; }
    size_t n; uint8_t *src = read_recipe_input(selected, argv[a], &n); if (!src) { perror(argv[a]); return 1; }
    if (n > UINT32_MAX) { fprintf(stderr, "%s: Tree-sitter input exceeds its 32-bit byte range\n", argv[a]); free(src); return 1; }
    if (selected && selected->input_frame.width) printf("input section: offset %u, bytes %zu\n", selected->input_frame.width, n);
    TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, lang);
    TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
    TSNode root = ts_tree_root_node(t); int malformed = ts_node_has_error(root);
    TSTreeCursor cur = ts_tree_cursor_new(root);
    tree_print(&cur, src, 0, &nodes);
    if (nodes <= 0) printf("\xE2\x80\xA6 (more: -n)\n");
    ts_tree_cursor_delete(&cur); ts_tree_delete(t); ts_parser_delete(ps); free(src);
    return malformed ? 1 : 0;
}
