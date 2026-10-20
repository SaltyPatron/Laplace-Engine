/* Compile packaging rules against native grammars, then project their cursors
 * into semantic fields. Only active ancestor frames live in scratch; completed
 * fields leave through the visitor. Injected parsers retain original byte ranges
 * while decoding the enclosing container's quoting and escapes. */
#define _GNU_SOURCE
#include "engine.h"
#include "structure.h"
#include "os.h"
#include "laplace_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#endif

/* ---- Stable-address, thread-local scratch. Reserve virtual address space
 * once; only touched pages consume memory. A reallocating bump buffer would
 * invalidate every live parent frame when a larger child exhausted it. ---- */
#define SCRATCH_RESERVE (UINT64_C(16) << 30)
static __thread SayScratch tls_scratch;
#ifdef _WIN32
static DWORD scratch_key = FLS_OUT_OF_INDEXES;
static INIT_ONCE scratch_once = INIT_ONCE_STATIC_INIT;
static void CALLBACK scratch_dispose(void *buffer){ VirtualFree(buffer, 0, MEM_RELEASE); }
static BOOL CALLBACK scratch_key_init(PINIT_ONCE once, PVOID argument, PVOID *context){
    (void)once; (void)argument; (void)context;
    scratch_key = FlsAlloc(scratch_dispose); return scratch_key != FLS_OUT_OF_INDEXES;
}
#else
static pthread_key_t scratch_key;
static pthread_once_t scratch_once = PTHREAD_ONCE_INIT;
static void scratch_dispose(void *buffer){
    munmap(buffer, (size_t)SCRATCH_RESERVE);
}
static void scratch_key_init(void){
    if (pthread_key_create(&scratch_key, scratch_dispose)) { fputs("scratch: cannot create thread lifetime key\n", stderr); abort(); }
}
#endif

static void arena_ready(SayScratch *arena){
    if (!arena->buf) {
        _Static_assert(sizeof(size_t) >= 8, "record arenas require a 64-bit address space");
#ifdef _WIN32
        arena->buf = VirtualAlloc(NULL, (size_t)SCRATCH_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
#else
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
        flags |= MAP_NORESERVE;
#endif
        void *buffer = mmap(NULL, (size_t)SCRATCH_RESERVE, PROT_READ | PROT_WRITE, flags, -1, 0);
        arena->buf = buffer == MAP_FAILED ? NULL : buffer;
        arena->committed = (size_t)SCRATCH_RESERVE;
#endif
        if (!arena->buf) { errno = ENOMEM; perror("scratch reserve"); abort(); }
        arena->cap = (size_t)SCRATCH_RESERVE;
    }
}
void arena_release(SayScratch *arena){
    if (!arena->buf) return;
#ifdef _WIN32
    VirtualFree(arena->buf, 0, MEM_RELEASE);
#else
    munmap(arena->buf, arena->cap);
#endif
    *arena = (SayScratch){0};
}
static void scratch_ready(void){
    if (!tls_scratch.buf) {
        arena_ready(&tls_scratch);
#ifdef _WIN32
        if (!InitOnceExecuteOnce(&scratch_once, scratch_key_init, NULL, NULL) || !FlsSetValue(scratch_key, tls_scratch.buf)) {
            fputs("scratch: cannot bind thread lifetime\n", stderr); abort();
        }
#else
        pthread_once(&scratch_once, scratch_key_init);
        if (pthread_setspecific(scratch_key, tls_scratch.buf)) { fputs("scratch: cannot bind thread lifetime\n", stderr); abort(); }
#endif
    }
}

void *arena_alloc(SayScratch *arena, size_t bytes){
    if (bytes > SIZE_MAX - 15) { errno = ENOMEM; perror("scratch_alloc"); abort(); }
    bytes = (bytes + 15u) & ~(size_t)15u; arena_ready(arena);
    if (bytes > arena->cap - arena->used - arena->stack_used) { errno = ENOMEM; perror("record exceeds scratch address space"); abort(); }
#ifdef _WIN32
    size_t end = arena->used + bytes;
    if (end > arena->committed) {
        size_t committed = (end + 65535u) & ~(size_t)65535u;
        if (!VirtualAlloc(arena->buf + arena->committed, committed - arena->committed, MEM_COMMIT, PAGE_READWRITE)) {
            errno = ENOMEM; perror("scratch commit"); abort();
        }
        arena->committed = committed;
    }
#endif
    void *p = arena->buf + arena->used;
    arena->used += bytes;
    return p;
}
void *scratch_alloc(size_t bytes){ scratch_ready(); return arena_alloc(&tls_scratch, bytes); }

/* Transient cursor frames grow from the other end of the same reservation.
 * Releasing an injected parser's stack cannot invalidate decoded field values
 * that the enclosing record still needs. No per-field heap or retained frame
 * vector is required, even for millions of sibling injections. */
static void *scratch_stack_alloc(size_t bytes){
    if (bytes > SIZE_MAX - 15) { errno = ENOMEM; perror("scratch stack"); abort(); }
    bytes = (bytes + 15u) & ~(size_t)15u; scratch_ready();
    if (bytes > tls_scratch.cap - tls_scratch.used - tls_scratch.stack_used) { errno = ENOMEM; perror("record exceeds scratch address space"); abort(); }
    size_t used = tls_scratch.stack_used + bytes;
#ifdef _WIN32
    if (used > tls_scratch.stack_committed) {
        size_t committed = (used + 65535u) & ~(size_t)65535u;
        if (!VirtualAlloc(tls_scratch.buf + tls_scratch.cap - committed, committed - tls_scratch.stack_committed, MEM_COMMIT, PAGE_READWRITE)) {
            errno = ENOMEM; perror("scratch stack commit"); abort();
        }
        tls_scratch.stack_committed = committed;
    }
#endif
    tls_scratch.stack_used = used;
    return tls_scratch.buf + tls_scratch.cap - used;
}

size_t scratch_mark(void){ return tls_scratch.used; }
void   scratch_restore(size_t mark){ if (mark <= tls_scratch.used) tls_scratch.used = mark; }

static void str_copy(char *dst, size_t cap, const char *src, size_t len){
    if (!cap) return;
    if (len >= cap) len = cap - 1;
    if (len) memcpy(dst, src, len);
    dst[len] = 0;
}
static void str_set(char *dst, size_t cap, const char *src){
    str_copy(dst, cap, src ? src : "", src ? strlen(src) : 0);
}
static size_t u64_digits(uint64_t v, char out[24]){
    char tmp[24]; size_t n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}
static int sep_read(const char *w, char *out, size_t cap){                 /* \n \t \\, and space, tab, hash by name (a recipe's own comments begin with the hash); anything else as written */
    size_t k = 0; if (!w) return 0; if (!strcmp(w, "space")) { out[0] = ' '; out[1] = 0; return 1; } if (!strcmp(w, "tab")) { out[0] = '\t'; out[1] = 0; return 1; } if (!strcmp(w, "hash")) { out[0] = '#'; out[1] = 0; return 1; }
    for (; *w && k + 1 < cap; w++) { if (*w == '\\' && w[1]) { w++; out[k++] = *w == 'n' ? '\n' : *w == 't' ? '\t' : *w == 'r' ? '\r' : *w; } else out[k++] = *w; }
    out[k] = 0; return (int)k;
}
static char *name_next(char **at, char *out, size_t cap){                  /* a name of several words is written between double quotes */
    char *p = *at; if (!p) return NULL; while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\r' || *p == '\n') return NULL;
    char *e; if (*p == '"') { p++; e = strchr(p, '"'); if (!e) return NULL; str_copy(out, cap, p, (size_t)(e - p)); *at = e + 1; }
    else { e = p; while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++; str_copy(out, cap, p, (size_t)(e - p)); *at = e; }
    return out;
}
int layout_says(Layout *l, const char *path, char *tok){
    STier *t = l->ntier ? &l->tier[l->ntier - 1] : NULL;
    if (!strcmp(tok, "tier")) { char *nm = strtok(NULL, " \t\r\n"), *by = strtok(NULL, " \t\r\n"), *sp = strtok(NULL, " \t\r\n");
        if (!nm || !by || strcmp(by, "by") || !sp || l->ntier == S_TIERS) { fprintf(stderr, "%s: tier NAME by SEPARATOR (at most %d tiers)\n", path, S_TIERS); return -1; }
        t = &l->tier[l->ntier++]; memset(t, 0, sizeof *t); str_set(t->name, sizeof t->name, nm); t->seplen = sep_read(sp, t->sep, sizeof t->sep);
        if (!t->seplen) { fprintf(stderr, "%s: tier %s is parted by nothing\n", path, nm); return -1; } return 1; }
    if (!strcmp(tok, "layout-prefix")) {
        GMap *grammar = &l->g;
        if (grammar->prefix_count == 4) { fprintf(stderr, "%s: at most four layout prefixes\n", path); return -1; }
        unsigned prefix = grammar->prefix_count++; char *name;
        while ((name = strtok(NULL, " \t\r\n"))) {
            if (grammar->prefix_length[prefix] == 8) { fprintf(stderr, "%s: at most eight names in a layout prefix\n", path); return -1; }
            str_set(grammar->layout_prefix[prefix][grammar->prefix_length[prefix]++], 64, name);
        }
        return grammar->prefix_length[prefix] ? 1 : -1;
    }
    if (!strcmp(tok, "part")) { char *pa = strtok(NULL, " \t\r\n"), *by = strtok(NULL, " \t\r\n"), *sp = strtok(NULL, " \t\r\n"), *w;
        if (!pa || !by || (strcmp(by, "by") && strcmp(by, "grammar")) || !sp || l->npart == 32) { fprintf(stderr, "%s: part NAME by SEPARATOR [is IS] [pieces N] [space CHAR] [padded] [grammar NAME]\n", path); return -1; }
        SPart *p = &l->part[l->npart++]; memset(p, 0, sizeof *p); str_set(p->path, sizeof p->path, pa);
        if (!strcmp(by, "grammar")) { str_set(p->grammar, sizeof p->grammar, sp); p->child_grammar_id = grammar_id_for(sp); }
        else p->seplen = sep_read(sp, p->sep, sizeof p->sep);
        while ((w = strtok(NULL, " \t\r\n"))) { if (!strcmp(w, "padded")) { p->padded = 1; continue; } char *v = strtok(NULL, " \t\r\n"), b[8];
            if (v && !strcmp(w, "is")) p->islen = sep_read(v, p->is, sizeof p->is);
            else if (v && !strcmp(w, "pieces") && atoi(v) > 0) p->pieces = atoi(v);
            else if (v && !strcmp(w, "space") && sep_read(v, b, sizeof b)) p->space = b[0];
            else if (v && !strcmp(w, "grammar")) { str_set(p->grammar, sizeof p->grammar, v); p->child_grammar_id = grammar_id_for(v); }
            else { fprintf(stderr, "%s: part NAME by SEPARATOR [is IS] [pieces N] [space CHAR] [padded] [grammar NAME]\n", path); return -1; } }
        return 1; }
    if (!strcmp(tok, "trim")) { l->g.trim = 1; return 1; }
    if (!strcmp(tok, "remark-content")) {
        char *group = strtok(NULL, " \t\r\n"), *text = strtok(NULL, " \t\r\n");
        if (!group || !text || strtok(NULL, " \t\r\n")) return -1;
        str_set(l->g.remark_group, sizeof l->g.remark_group, group); str_set(l->g.remark_text, sizeof l->g.remark_text, text);
        return 1;
    }
    if (!strcmp(tok, "counted-shape")) {
        char *count = strtok(NULL, " \t\r\n"), *group = strtok(NULL, " \t\r\n"), *member; unsigned radix = 10;
        if (group && !strcmp(group, "radix")) {
            char *number = strtok(NULL, " \t\r\n"), *end = NULL;
            radix = number ? (unsigned)strtoul(number, &end, 10) : 0;
            if (!number || !*number || *end || radix < 2 || radix > 36) return -1;
            group = strtok(NULL, " \t\r\n");
        }
        if (!count || !group || l->g.count_shape_count == 8) return -1;
        GCountShape *shape = &l->g.count_shapes[l->g.count_shape_count++];
        str_set(shape->count, sizeof shape->count, count); str_set(shape->group, sizeof shape->group, group); shape->radix = (uint8_t)radix;
        while ((member = strtok(NULL, " \t\r\n"))) {
            if (shape->nmember == 8) return -1;
            str_set(shape->member[shape->nmember++], sizeof shape->member[0], member);
        }
        if (!shape->nmember) return -1;
        return 1;
    }
    if (!strcmp(tok, "record-names")) {
        char *name;
        while ((name = strtok(NULL, " \t\r\n"))) {
            if (l->g.record_name_count == 16 || strlen(name) >= sizeof l->g.record_names[0].name) {
                fprintf(stderr, "%s: record-names accepts at most sixteen names of at most 63 bytes\n", path); return -1;
            }
            GRecordName *record = &l->g.record_names[l->g.record_name_count++];
            str_set(record->name, sizeof record->name, name); record->length = (uint16_t)strlen(name);
        }
        return l->g.record_name_count ? 1 : -1;
    }
    if (!strcmp(tok, "quotes")) {
        char *type;
        while ((type = strtok(NULL, " \t\r\n"))) {
            if (l->g.quote_count == 16) { fprintf(stderr, "%s: too many quoted node types\n", path); return -1; }
            str_set(l->g.quote_types[l->g.quote_count++], 48, type);
        }
        if (!l->g.quote_count) { fprintf(stderr, "%s: quotes requires a node type\n", path); return -1; }
        return 1;
    }
    if (!strcmp(tok, "separator")) {
        char *type = strtok(NULL, " \t\r\n"), *literal = strtok(NULL, " \t\r\n");
        if (!type || !literal || l->g.separator_count == 32) { fprintf(stderr, "%s: separator TYPE LITERAL\n", path); return -1; }
        GSeparator *separator = &l->g.separators[l->g.separator_count++]; str_set(separator->type, sizeof separator->type, type);
        separator->length = (uint8_t)sep_read(literal, separator->literal, sizeof separator->literal);
        if (!separator->length) return -1;
        return 1;
    }
    if (!strcmp(tok, "records")) {
        char *name;
        while ((name = strtok(NULL, " \t\r\n"))) {
            if (l->g.record_count == 16) { fprintf(stderr, "%s: too many record symbols\n", path); return -1; }
            str_set(l->g.record_types[l->g.record_count++], 48, name);
        }
        if (!l->g.record_count) { fprintf(stderr, "%s: records requires a node type\n", path); return -1; }
        return 1;
    }
    if (!strcmp(tok, "columns")) {
        char *word; int fields = 0;
        while ((word = strtok(NULL, " \t\r\n"))) {
            if (!strcmp(word, "under")) { word = strtok(NULL, " \t\r\n"); if (!word) return -1; str_set(l->g.column_parent, 48, word); }
            else if (!strcmp(word, "tier")) {
                word = strtok(NULL, " \t\r\n"); char *end = NULL; unsigned long index = word ? strtoul(word, &end, 10) : S_TIERS;
                if (!word || !*word || *end || index >= S_TIERS) { fprintf(stderr, "%s: invalid column tier\n", path); return -1; }
                l->g.column_tier = (uint8_t)index;
            } else if (!strcmp(word, "fields")) fields = 1;
            else if (fields) {
                if (l->g.named_column_count == S_NAMES) return -1;
                str_set(l->g.named_columns[l->g.named_column_count++], 48, word);
            } else {
                if (l->g.column_type_count == 8) return -1;
                str_set(l->g.column_types[l->g.column_type_count++], 48, word);
            }
        }
        if (!l->g.column_type_count || !l->g.column_parent[0]) { fprintf(stderr, "%s: columns NODE... under PARENT [tier N] [fields FIELD...]\n", path); return -1; }
        return 1;
    }
    if (!strcmp(tok, "levels")) { char *v; while ((v = strtok(NULL, " \t\r\n")) && l->nlevels < 8) {
            str_set(l->levels[l->nlevels], 32, v); l->level_lengths[l->nlevels] = (uint8_t)strlen(l->levels[l->nlevels]); l->nlevels++; }
        if (!l->nlevels) { fprintf(stderr, "%s: levels NAME...\n", path); return -1; } return 1; }
    if (!strcmp(tok, "resolve")) { char *v = strtok(NULL, " \t\r\n"); l->g.resolve = v && !strcmp(v, "xml") ? G_XML : v && !strcmp(v, "json") ? G_JSON : v && !strcmp(v, "turtle") ? G_TURTLE : v && !strcmp(v, "csv") ? G_CSV : v && !strcmp(v, "delimited") ? G_DELIMITED : v && !strcmp(v, "lines") ? G_LINES : 0;
        if (!l->g.resolve) { fprintf(stderr, "%s: resolve xml | json | turtle | csv | lines | delimited [quoted] [escaped CHAR]\n", path); return -1; }
        char *option;
        while ((option = strtok(NULL, " \t\r\n"))) {
            if (l->g.resolve == G_DELIMITED && !strcmp(option, "quoted")) l->g.quoted = 1;
            else if (l->g.resolve == G_DELIMITED && !strcmp(option, "padded")) l->g.padded = 1;
            else if (l->g.resolve == G_DELIMITED && !strcmp(option, "escaped")) {
                char *escape = strtok(NULL, " \t\r\n");
                if (!escape || !escape[0] || escape[1]) { fprintf(stderr, "%s: escaped requires one byte\n", path); return -1; }
                l->g.escaped = (uint8_t)escape[0];
            } else { fprintf(stderr, "%s: unknown resolve option %s\n", path, option); return -1; }
        }
        return 1; }
    if (!strcmp(tok, "node")) { char *ty = strtok(NULL, " \t\r\n"), *wh = strtok(NULL, " \t\r\n"), *w;
        if (!ty || !wh || l->g.n == 48) { fprintf(stderr, "%s: node TYPE group | value | text | member | note | skip ...\n", path); return -1; }
        GRule *r = &l->g.rule[l->g.n++]; memset(r, 0, sizeof *r); str_set(r->type, sizeof r->type, ty);
        r->what = !strcmp(wh, "group") ? G_GROUP : !strcmp(wh, "value") ? G_VALUE : !strcmp(wh, "text") ? G_TEXT : !strcmp(wh, "member") ? G_MEMBER : !strcmp(wh, "skip") ? G_SKIP : !strcmp(wh, "note") ? G_NOTE : 0;
        if (!r->what) { fprintf(stderr, "%s: node %s %s: group | value | text | member | note | skip\n", path, ty, wh); return -1; }
        r->compiled.kind = (GNodeKind)r->what;
        int into = 0;                                                       /* names, text path, injection, field, parent */
        while ((w = strtok(NULL, " \t\r\n"))) {
            if (into == 3) { str_set(r->child_grammar, sizeof r->child_grammar, w); r->compiled.child_grammar_id = grammar_id_for(w); into = 0; continue; }
            if (into == 4) { str_set(r->field, sizeof r->field, w); into = 0; continue; }
            if (into == 5) { str_set(r->parent, sizeof r->parent, w); into = 0; continue; }
            if (into == 6) { str_set(r->alias, sizeof r->alias, w); into = 0; continue; }
            if (into == 7) {
                char *end; unsigned long index = strtoul(w, &end, 10);
                if (!*w || *end || index >= S_TIERS) { fprintf(stderr, "%s: invalid alias tier\n", path); return -1; }
                r->alias_tier = (uint8_t)(index + 1); into = 0; continue;
            }
            if (!strcmp(w, "context")) { into = 8; continue; }
            if (into == 8) {
                if (r->ncontext == 4) { fprintf(stderr, "%s: at most four context paths per group\n", path); return -1; }
                str_set(r->context[r->ncontext++], 64, w); continue;
            }
            if (into == 2 && !r->text[0]) { str_set(r->text, sizeof r->text, w); continue; }
            if (into == 1 && !r->nname) { str_set(r->name[r->nname++], 64, w); continue; }
            if (!strcmp(w, "name")) into = 1; else if (!strcmp(w, "text") || !strcmp(w, "value")) into = 2; else if (!strcmp(w, "grammar")) into = 3;
            else if (!strcmp(w, "field")) into = 4; else if (!strcmp(w, "under")) into = 5;
            else if (!strcmp(w, "as")) into = 6;
            else if (!strcmp(w, "tier")) into = 7;
            else if (!strcmp(w, "same-tier")) r->compiled.flags |= 1u;
            else if (!strcmp(w, "raw")) r->raw = 1; else if (!strcmp(w, "join")) r->join = 1; else if (!strcmp(w, "list")) r->list = 1; else if (!strcmp(w, "kind")) r->kind = 1; else if (!strcmp(w, "trim")) r->trim = 1;
            else if (into == 1 && r->nname < 4) str_set(r->name[r->nname++], 64, w); else if (into == 2) str_set(r->text, sizeof r->text, w);
            else { fprintf(stderr, "%s: node %s: %s is not something a node's line says\n", path, ty, w); return -1; } }
        if ((into >= 3 && into != 8) || (into == 8 && !r->ncontext) || (into == 1 && !r->nname) || (into == 2 && !r->text[0])) { fprintf(stderr, "%s: node %s: missing rule argument\n", path, ty); return -1; }
        if (r->ncontext && r->what != G_GROUP) { fprintf(stderr, "%s: context paths require a group\n", path); return -1; }
        if ((r->what == G_VALUE || r->what == G_MEMBER || r->what == G_NOTE) && (!r->nname || !r->text[0])) { fprintf(stderr, "%s: node %s %s name PATH %s PATH\n", path, ty, wh, r->what == G_MEMBER ? "value" : "text"); return -1; }
        if ((r->alias[0] || r->alias_tier) && r->what != G_GROUP && r->what != G_TEXT) { fprintf(stderr, "%s: node %s: an alias applies to a group or text rule\n", path, ty); return -1; }
        if (r->alias[0] && r->alias_tier) { fprintf(stderr, "%s: node %s: use an explicit alias or a tier alias\n", path, ty); return -1; }
        return 1; }
    if (!strcmp(tok, "empty")) { char *v; int any = 0;
        while ((v = strtok(NULL, " \t\r\n")) && l->nempty < 16) { l->emptylen[l->nempty] = sep_read(v, l->empty[l->nempty], sizeof l->empty[0]); l->nempty++; any = 1; }
        if (!any) { fprintf(stderr, "%s: empty TEXT...\n", path); return -1; } return 1; }
    if (!t) return 0;                                                       /* what follows is said of the tier written last */
    if (!strcmp(tok, "names")) { char *at = strtok(NULL, "\r\n"), nm[64]; while (t->nnames < S_NAMES && name_next(&at, nm, sizeof nm)) str_set(t->names[t->nnames++], 64, nm);
        if (t->nnames) { char *z = t->names[t->nnames - 1]; size_t zl = strlen(z); if (zl > 3 && !strcmp(z + zl - 3, "...")) { z[zl - 3] = 0; t->rest = 1; } }     /* NAME...: every further part is NAME too, in order */
        return 1; }
    if (!strcmp(tok, "header")) { t->header = 1; return 1; }
    if (!strcmp(tok, "quoted")) { t->quoted = 1; return 1; }
    if (!strcmp(tok, "padded")) { t->padded = 1; return 1; }
    if (!strcmp(tok, "continued")) { t->continued = 1; return 1; }
    if (!strcmp(tok, "numbered")) { char *v = strtok(NULL, " \t\r\n"); if (!v) { fprintf(stderr, "%s: numbered NAME\n", path); return -1; } str_set(t->numbered, sizeof t->numbered, v); return 1; }
    if (!strcmp(tok, "is")) { char *v = strtok(NULL, " \t\r\n"); if (!v || !(t->kvlen = sep_read(v, t->kv, sizeof t->kv))) { fprintf(stderr, "%s: is SEPARATOR\n", path); return -1; } return 1; }
    if (!strcmp(tok, "skip")) { char *v = strtok(NULL, " \t\r\n"); t->skip = v ? atoi(v) : 0; return 1; }
    if (!strcmp(tok, "note")) { char *p = strtok(NULL, " \t\r\n"), *is = strtok(NULL, " \t\r\n"); if (!p) { fprintf(stderr, "%s: note PREFIX [IS]\n", path); return -1; }
        t->notelen = sep_read(p, t->note, sizeof t->note); t->islen = is ? sep_read(is, t->note_is, sizeof t->note_is) : 0; return 1; }
    if (!strcmp(tok, "comment")) { char *p = strtok(NULL, " \t\r\n"); if (!p) { fprintf(stderr, "%s: comment PREFIX\n", path); return -1; } t->commentlen = sep_read(p, t->comment, sizeof t->comment); return 1; }
    if (!strcmp(tok, "remark")) { char *p = strtok(NULL, " \t\r\n"), *kept = strtok(NULL, " \t\r\n"), b[8]; if (!p || !sep_read(p, b, sizeof b)) { fprintf(stderr, "%s: remark CHAR [KEPT]\n", path); return -1; }
        t->remark = b[0]; if (kept) str_set(t->remark_kept, sizeof t->remark_kept, kept); return 1; }
    if (!strcmp(tok, "counted")) { char *f = strtok(NULL, " \t\r\n"), *w = strtok(NULL, " \t\r\n"); int radix = 10;
        if (w && !strcmp(w, "radix")) { char *v = strtok(NULL, " \t\r\n"); radix = v ? atoi(v) : 0; w = strtok(NULL, " \t\r\n"); }
        else if (w && !strcmp(w, "hex")) { radix = 16; w = strtok(NULL, " \t\r\n"); }
        if (w && !strcmp(w, "of")) w = strtok(NULL, " \t\r\n");
        if (!f || !w || radix < 2 || radix > 36 || t->ncount == 4) { fprintf(stderr, "%s: counted NAME [radix N | hex] [of] GROUP MEMBER... (radix 2 to 36; at most 4 to a tier)\n", path); return -1; }
        int c = t->ncount; str_set(t->count[c].field, sizeof t->count[c].field, f); str_set(t->count[c].group, sizeof t->count[c].group, w); t->count[c].radix = radix; t->count[c].nmember = 0;
        char *m; while ((m = strtok(NULL, " \t\r\n")) && t->count[c].nmember < 8) str_set(t->count[c].member[t->count[c].nmember++], 32, m);
        if (!t->count[c].nmember) { fprintf(stderr, "%s: counted %s %s: a group is parts, named MEMBER...\n", path, f, w); return -1; }
        t->ncount++; return 1; }
    if (!strcmp(tok, "escaped")) { char *p = strtok(NULL, " \t\r\n"); if (!p) { fprintf(stderr, "%s: escaped CHAR\n", path); return -1; } t->escaped = p[0]; return 1; }
    return 0;
}

static int grammar_path_compile(const TSLanguage *lang, const char *path, GPath *out){
    memset(out, 0, sizeof *out);
    for (const char *at = path; *at;) {
        const char *end = strchr(at, '/'); uint32_t len = (uint32_t)(end ? (size_t)(end - at) : strlen(at));
        if (len) {
            if (out->n == G_PATH_STEPS) return 0;
            GPathStep *step = &out->step[out->n++];
            step->field = ts_language_field_id_for_name(lang, at, len);
            step->symbol = ts_language_symbol_for_name(lang, at, len, true);
            if (!step->symbol) step->symbol = ts_language_symbol_for_name(lang, at, len, false);
            if (step->field) step->flags |= 1;
            if (step->symbol || (len == 5 && !memcmp(at, "ERROR", 5))) step->flags |= 2;
            if (!step->flags) return 0;
        }
        at = end ? end + 1 : at + len;
    }
    return 1;
}

static Layout *inner_layouts[512];
static uint8_t inner_state[512];
static uint64_t part_hash(const uint8_t *bytes, size_t length){
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}
static FILE *format_open(const char *name, char *path, size_t capacity){
    snprintf(path, capacity, "%s/formats/%s.format", laplace_recipes(), name);
    FILE *file = fopen(path, "r");
    if (!file && errno == ENOENT && strcmp(laplace_recipes(), LAPLACE_RECIPES_DEFAULT)) {
        snprintf(path, capacity, "%s/formats/%s.format", LAPLACE_RECIPES_DEFAULT, name);
        file = fopen(path, "r");
    }
    if (!file) perror(path);
    return file;
}
static const Layout *inner_layout(const char *name){
    uint16_t id = grammar_id_for(name);
    if (!id || id >= 512) return NULL;
    if (inner_state[id] == 2) return inner_layouts[id];
    if (inner_state[id]) { fprintf(stderr, "grammar %s: cyclic or failed injected format\n", name); return NULL; }
    inner_state[id] = 1;
    char path[4400]; FILE *file = format_open(name, path, sizeof path);
    if (!file) { inner_state[id] = 3; return NULL; }
    Layout *layout = calloc(1, sizeof *layout); if (!layout) { perror("injected format"); abort(); }
    char line[2048]; int valid = 1, declared = 0;
    while (fgets(line, sizeof line, file)) {
        comment_off(line); char *token = strtok(line, " \t\r\n"); if (!token) continue;
        if (!strcmp(token, "grammar")) { char *language = strtok(NULL, " \t\r\n"); declared = language && !strcmp(language, name); if (!declared) { valid = 0; break; } }
        else if (layout_says(layout, path, token) != 1) { valid = 0; break; }
    }
    fclose(file);
    if (!valid || !declared || !layout_compile_grammar(layout, name)) { inner_state[id] = 3; return NULL; }
    inner_layouts[id] = layout; inner_state[id] = 2; return layout;
}
int layout_compile_parts(Layout *layout){
    if (layout->parts_compiled) return !layout->part_errors;
    layout->parts_compiled = 1;
    int valid = 1;
    for (int i = 0; i < layout->npart; i++) {
        SPart *part = &layout->part[i]; const char *name = part->grammar;
        if (!*name && ((part->seplen == 6 && !memcmp(part->sep, "object", 6)) || (part->seplen == 4 && !memcmp(part->sep, "json", 4)))) name = "json";
        if (*name) { part->inner = inner_layout(name); if (!part->inner) valid = 0; else part->child_grammar_id = part->inner->g.grammar_id; }
        else {
            part->syntax = inner_layout("lp_parts");
            if (!part->syntax) valid = 0;
            else {
                const GMap *grammar = &part->syntax->g;
                for (uint8_t s = 0; s < grammar->separator_count; s++) {
                    const GSeparator *separator = &grammar->separators[s];
                    if (part->seplen == separator->length && !memcmp(part->sep, separator->literal, separator->length)) part->separator = separator->symbol;
                    if (part->islen == separator->length && !memcmp(part->is, separator->literal, separator->length)) part->assignment = separator->symbol;
                }
                if (!part->separator || (part->islen && !part->assignment)) { fprintf(stderr, "part %s: separator is not declared by its compiled grammar\n", part->path); valid = 0; }
            }
        }
        part->name_length = (uint16_t)strlen(part->path);
        if (part->name_length && part->path[part->name_length - 1] == '*') { part->wildcard = 1; part->name_length--; layout->part_prefix_lengths |= UINT64_C(1) << part->name_length; }
        part->name_hash = part_hash((const uint8_t *)part->path, part->name_length);
        size_t bucket = (part->name_hash ^ (part->name_hash >> 32)) & 63u;
        part->next = layout->part_buckets[bucket]; layout->part_buckets[bucket] = (uint16_t)(i + 1);
    }
    layout->part_errors = (uint8_t)!valid;
    return valid;
}
static int compile_symbol_mask(const TSLanguage *language, uint32_t count, const char *name, uint8_t *mask){
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *symbol = ts_language_symbol_name(language, (TSSymbol)i);
        if (symbol && !strcmp(symbol, name)) { mask[i] = 1; found = 1; }
    }
    return found;
}
static int use_format(Layout *layout, const char *name){
    char path[4400]; FILE *file = format_open(name, path, sizeof path); if (!file) return 0;
    char line[2048]; int valid = 1, declared = 0;
    while (fgets(line, sizeof line, file)) {
        comment_off(line); char *token = strtok(line, " \t\r\n"); if (!token) continue;
        if (!strcmp(token, "grammar")) {
            char *grammar = strtok(NULL, " \t\r\n"); declared = grammar && !strcmp(grammar, name);
            if (!declared) { valid = 0; break; }
        } else if (layout_says(layout, path, token) != 1) { valid = 0; break; }
    }
    fclose(file); return valid && declared;
}
static int separated_by(const STier *tier, const char *separator){
    size_t length = strlen(separator);
    return (size_t)tier->seplen == length && !memcmp(tier->sep, separator, length);
}
static int native_counted_layout(const Layout *layout){
    static Layout *format;
    if (!format) {
        format = calloc(1, sizeof *format); if (!format) { perror("counted grammar schema"); abort(); }
        if (!use_format(format, "wndb")) { free(format); format = NULL; return -1; }
    }
    const STier *row = &layout->tier[0];
    for (unsigned p = 0; p < format->g.prefix_count; p++) {
        unsigned n = format->g.prefix_length[p]; if (n > (unsigned)row->nnames) continue;
        unsigned i = 0; while (i < n && !strcmp(row->names[i], format->g.layout_prefix[p][i])) i++;
        if (i == n) return 1;
    }
    return 0;
}
static int compile_conllu_records(Layout *layout, char *grammar, size_t capacity){
    Layout *inner = malloc(sizeof *inner); if (!inner) { perror("record product grammar"); return 0; }
    *inner = *layout;
    if ((!inner->g.n && !use_format(inner, "conllu")) || !layout_compile_grammar(inner, "conllu")) { free(inner); return 0; }
    layout->g = (GMap){0}; layout->npart = layout->parts_compiled = layout->part_errors = 0;
    memset(layout->part_buckets, 0, sizeof layout->part_buckets); layout->part_prefix_lengths = 0; layout->nlevels = 0;
    str_set(grammar, capacity, "lp_conllu_records");
    if (!use_format(layout, grammar)) { free(inner); return 0; }
    layout->record_inner = inner;
    for (int i = 0; i < layout->g.n; i++) if (layout->g.rule[i].what == G_TEXT) {
        layout->g.rule[i].compiled.inner = inner; layout->g.rule[i].compiled.child_grammar_id = inner->g.grammar_id;
        layout->g.rule[i].compiled.flags |= 2u;
    }
    return layout_compile_grammar(layout, grammar);
}
int layout_compile_tiers(Layout *layout, char *grammar, size_t capacity){
    if (!layout->ntier || layout->g.n || layout->g.compiled) return 0;
    const char *name = NULL;
    if (layout->ntier == 1 && separated_by(&layout->tier[0], "\n")) name = "lp_records";
    else if (layout->ntier == 2 && separated_by(&layout->tier[0], "\n%%\n") && separated_by(&layout->tier[1], "\n") &&
             layout->tier[1].kvlen == 1 && layout->tier[1].kv[0] == ':' && layout->tier[1].continued) name = "lp_registry";
    else if (layout->ntier == 2 && separated_by(&layout->tier[0], "\n\n") && separated_by(&layout->tier[1], "\n")) name = "lp_paragraphs";
    else if (layout->ntier == 3 && separated_by(&layout->tier[0], "\n\n") &&
             separated_by(&layout->tier[1], "\n") && separated_by(&layout->tier[2], "\t")) return compile_conllu_records(layout, grammar, capacity);
    else if (layout->ntier == 2 && separated_by(&layout->tier[0], "\n")) {
        const STier *fields = &layout->tier[1];
        if (layout->tier[0].ncount && separated_by(fields, " ")) {
            int native = native_counted_layout(layout); if (native < 0) return 0;
            name = native ? "wndb" : "lp_words";
        } else if (separated_by(fields, " ")) name = "lp_words";
        else {
            const char *kind = separated_by(fields, ",") ? "csv" : separated_by(fields, "\t") ? "tsv" :
                               separated_by(fields, "|") ? "psv" : separated_by(fields, ";") ? "semicolon" : NULL;
            char escape = fields->escaped ? fields->escaped : layout->tier[0].escaped;
            int quoted = fields->quoted || layout->tier[0].quoted;
            int padded = layout->tier[0].padded;
            char remark = layout->tier[0].remark;
            if (kind && (!escape || escape == '\\') &&
                (!fields->escaped || !layout->tier[0].escaped || fields->escaped == layout->tier[0].escaped) && (!remark || remark == '#')) {
                snprintf(grammar, capacity, "lp_%s%s%s%s%s", kind, quoted ? "_quoted" : "", escape ? "_escaped" : "",
                         quoted && padded ? "_padded" : "", remark ? "_hash" : "");
                name = grammar;
            }
        }
    }
    if (!name) { fputs("tier layout has no matching compiled packaging grammar\n", stderr); return 0; }
    if (name != grammar) str_set(grammar, capacity, name);
    if (!use_format(layout, grammar)) return 0;
    for (int i = 0; i + 1 < layout->ntier; i++) if (layout->tier[i].padded) layout->g.padded = 1;
    return layout_compile_grammar(layout, grammar);
}
int layout_compile_recipe(Layout *layout, char *grammar, size_t capacity){
    if (layout->g.compiled) return !layout->g.unresolved;
    if (!layout->g.n) return layout_compile_tiers(layout, grammar, capacity);
    if (layout->ntier == 3 && !strcmp(grammar, "conllu") && separated_by(&layout->tier[0], "\n\n") &&
        separated_by(&layout->tier[1], "\n") && separated_by(&layout->tier[2], "\t")) return compile_conllu_records(layout, grammar, capacity);
    /* JSON Lines has a physical record container and an independent JSON
     * product grammar. Keep the recipe's own levels and part instructions on
     * the child, rather than replacing them with a generic JSON format. */
    if (layout->ntier == 1 && separated_by(&layout->tier[0], "\n") && !strcmp(grammar, "json")) {
        Layout *inner = malloc(sizeof *inner); if (!inner) { perror("record injection grammar"); return 0; }
        *inner = *layout; inner->ntier = 0;
        if (!layout_compile_grammar(inner, grammar)) { free(inner); return 0; }
        layout->g = (GMap){0}; layout->npart = layout->parts_compiled = layout->part_errors = 0;
        memset(layout->part_buckets, 0, sizeof layout->part_buckets); layout->part_prefix_lengths = 0; layout->nlevels = 0;
        str_set(grammar, capacity, "lp_records");
        if (!use_format(layout, grammar)) { free(inner); return 0; }
        layout->record_inner = inner;
        for (int i = 0; i < layout->g.n; i++) if (layout->g.rule[i].what == G_TEXT) {
            layout->g.rule[i].compiled.inner = inner;
            layout->g.rule[i].compiled.child_grammar_id = inner->g.grammar_id;
        }
    }
    return layout_compile_grammar(layout, grammar);
}
static int map_count_columns(GMap *grammar, int pending_header){
    memset(grammar->count_by_column, 0, sizeof grammar->count_by_column);
    for (unsigned c = 0; c < grammar->count_count; c++) {
        const GColumn *trigger = &grammar->counts[c].trigger; unsigned found = 0;
        for (uint32_t column = 0; column < grammar->column_count && column < S_NAMES; column++) {
            const GColumn *name = &grammar->columns[column];
            if (name->length != trigger->length || memcmp(name->name, trigger->name, name->length)) continue;
            if (found || grammar->count_by_column[column]) { fputs("counted field has ambiguous column bindings\n", stderr); return 0; }
            grammar->count_by_column[column] = (uint8_t)(c + 1); found = 1;
        }
        if (!found && !pending_header) { fprintf(stderr, "counted %.*s: no named count column\n", (int)trigger->length, trigger->name); return 0; }
    }
    return 1;
}
void layout_bind_names(Layout *layout, s_compile_name_fn compile, const void *program){
    layout->compile_name = compile; layout->name_program = program;
    if (!compile) return;
    if (layout->record_inner) layout_bind_names(layout->record_inner, compile, program);
    for (uint32_t i = 0; i < layout->g.column_count; i++) {
        GColumn *column = &layout->g.columns[i]; column->bindings = compile(program, column->name, column->length, NULL);
    }
    for (unsigned i = 0; i < layout->g.count_count; i++) {
        GCountProgram *count = &layout->g.counts[i];
        count->group.bindings = compile(program, count->group.name, count->group.length, NULL);
        for (unsigned j = 0; j < count->arity; j++) {
            GColumn *column = &count->member[j]; column->bindings = compile(program, column->name, column->length, NULL);
        }
    }
}
static int compile_count_shapes(Layout *layout){
    GMap *grammar = &layout->g;
    for (int tier = 0; tier < layout->ntier; tier++) for (int c = 0; c < layout->tier[tier].ncount; c++) {
        const STier *source = &layout->tier[tier]; const GCountShape *shape = NULL;
        if (!grammar->count_shape_count) {
            if (tier || layout->ntier != 2 || !grammar->column_type_count || grammar->column_tier || grammar->count_count == 4) {
                fputs("counted fields require a compiled row/column grammar\n", stderr); return 0;
            }
            GCountProgram *program = &grammar->counts[grammar->count_count++];
            program->trigger = (GColumn){.name = (const uint8_t *)source->count[c].field, .length = (uint32_t)strlen(source->count[c].field)};
            program->group = (GColumn){.name = (const uint8_t *)source->count[c].group, .length = (uint32_t)strlen(source->count[c].group)};
            program->arity = (uint8_t)source->count[c].nmember; program->radix = (uint8_t)source->count[c].radix;
            for (unsigned member = 0; member < program->arity; member++)
                program->member[member] = (GColumn){.name = (const uint8_t *)source->count[c].member[member],
                    .length = (uint32_t)strlen(source->count[c].member[member]), .flags = S_FIELD_ALIAS | S_FIELD_SAME_TIER};
            continue;
        }
        for (uint8_t i = 0; i < grammar->count_shape_count; i++) {
            const GCountShape *candidate = &grammar->count_shapes[i];
            if (!strcmp(candidate->count, source->count[c].field) && candidate->nmember == source->count[c].nmember &&
                candidate->radix == source->count[c].radix) { shape = candidate; break; }
        }
        if (!shape) { fprintf(stderr, "counted %s: no compiled grammar shape of this radix and arity\n", source->count[c].field); return 0; }
        GRule *group = NULL;
        for (int i = 0; i < grammar->n; i++) if (grammar->rule[i].what == G_GROUP && !strcmp(grammar->rule[i].type, shape->group)) { group = &grammar->rule[i]; break; }
        if (!group) { fprintf(stderr, "counted %s: grammar has no group rule %s\n", shape->count, shape->group); return 0; }
        str_set(group->alias, sizeof group->alias, source->count[c].group);
        for (uint8_t member = 0; member < shape->nmember; member++) {
            if (grammar->n == 48) { fputs("counted shape exceeds the compiled rule limit\n", stderr); return 0; }
            GRule *rule = &grammar->rule[grammar->n++]; *rule = (GRule){.what=G_TEXT};
            rule->compiled.flags |= 1u;
            str_set(rule->type, sizeof rule->type, "field"); str_set(rule->field, sizeof rule->field, shape->member[member]);
            str_set(rule->parent, sizeof rule->parent, shape->group);
            str_set(rule->alias, sizeof rule->alias, source->count[c].member[member]);
        }
    }
    return 1;
}
static int compile_remarks(Layout *layout){
    if (!layout->ntier || !*layout->tier[0].remark_kept) return 1;
    GMap *grammar = &layout->g;
    if (!*grammar->remark_text) { fputs("kept remark has no compiled grammar content rule\n", stderr); return 0; }
    for (int i = 0; i < grammar->n; i++) if (grammar->rule[i].what == G_SKIP && !strcmp(grammar->rule[i].type, grammar->remark_group)) {
        memmove(grammar->rule + i, grammar->rule + i + 1, (size_t)(grammar->n - i - 1) * sizeof *grammar->rule); grammar->n--; break;
    }
    GRule *rule = NULL;
    for (int i = 0; i < grammar->n; i++) if (grammar->rule[i].what == G_TEXT && !strcmp(grammar->rule[i].type, grammar->remark_text)) { rule = &grammar->rule[i]; break; }
    if (!rule) {
        if (grammar->n == 48) return 0;
        rule = &grammar->rule[grammar->n++]; *rule = (GRule){.what=G_TEXT};
        str_set(rule->type, sizeof rule->type, grammar->remark_text);
    }
    rule->raw = rule->trim = 1; str_set(rule->alias, sizeof rule->alias, layout->tier[0].remark_kept);
    return 1;
}
int layout_compile_grammar(Layout *l, const char *grammar_name){
    GMap *g = &l->g;
    if (g->compiled) return !g->unresolved;
    g->compiled = 1; g->grammar_id = grammar_id_for(grammar_name);
    if (l->ntier) {
        g->numbered_length = (uint16_t)strlen(l->tier[0].numbered);
        g->root_name_length = (uint16_t)strlen(l->tier[0].name);
    }
    if (!compile_count_shapes(l) || !compile_remarks(l)) { g->unresolved++; return 0; }
    const TSLanguage *lang = grammar_lang(g->grammar_id);
    if (!lang) { g->unresolved++; fprintf(stderr, "grammar %s: no compiled language\n", grammar_name); return 0; }
    g->symbol_count = ts_language_symbol_count(lang);
    g->field_count = ts_language_field_count(lang);
    g->field_names = calloc((size_t)g->field_count + 1, sizeof(char *));
    g->field_lengths = calloc((size_t)g->field_count + 1, sizeof(uint32_t));
    if (!g->field_names || !g->field_lengths) { perror("grammar fields"); abort(); }
    for (uint32_t i = 1; i <= g->field_count; i++) {
        g->field_names[i] = ts_language_field_name_for_id(lang, (TSFieldId)i);
        if (g->field_names[i]) g->field_lengths[i] = (uint32_t)strlen(g->field_names[i]);
    }
    if (g->record_count) {
        g->record_symbols = calloc(g->symbol_count, 1); if (!g->record_symbols) { perror("record symbol dispatch"); abort(); }
        for (uint8_t i = 0; i < g->record_count; i++) if (!compile_symbol_mask(lang, g->symbol_count, g->record_types[i], g->record_symbols)) {
            fprintf(stderr, "grammar %s: unresolved record symbol %s\n", grammar_name, g->record_types[i]); g->unresolved++;
        }
    }
    for (uint8_t i = 0; i < g->record_name_count; i++) {
        GRecordName *record = &g->record_names[i];
        record->length = (uint16_t)strlen(record->name); record->hash = part_hash((const uint8_t *)record->name, record->length);
        size_t bucket = (record->hash ^ (record->hash >> 32)) & 31u;
        record->next = g->record_name_buckets[bucket]; g->record_name_buckets[bucket] = (uint8_t)(i + 1);
    }
    if (g->quote_count) {
        g->quoted_symbols = calloc(g->symbol_count, 1); if (!g->quoted_symbols) { perror("quoted symbol dispatch"); abort(); }
        for (uint8_t i = 0; i < g->quote_count; i++) if (!compile_symbol_mask(lang, g->symbol_count, g->quote_types[i], g->quoted_symbols)) {
            fprintf(stderr, "grammar %s: unresolved quoted symbol %s\n", grammar_name, g->quote_types[i]); g->unresolved++;
        }
    }
    if (g->column_type_count) {
        g->column_symbols = calloc(g->symbol_count, 1); g->column_parents = calloc(g->symbol_count, 1);
        g->column_by_field = calloc((size_t)g->field_count + 1, sizeof *g->column_by_field);
        if (!g->column_symbols || !g->column_parents || !g->column_by_field) { perror("column dispatch"); abort(); }
        for (uint8_t i = 0; i < g->column_type_count; i++) if (!compile_symbol_mask(lang, g->symbol_count, g->column_types[i], g->column_symbols)) {
            fprintf(stderr, "grammar %s: unresolved column symbol %s\n", grammar_name, g->column_types[i]); g->unresolved++;
        }
        if (!compile_symbol_mask(lang, g->symbol_count, g->column_parent, g->column_parents)) {
            fprintf(stderr, "grammar %s: unresolved column parent %s\n", grammar_name, g->column_parent); g->unresolved++;
        }
        g->column_parent_symbol = ts_language_symbol_for_name(lang, g->column_parent, (uint32_t)strlen(g->column_parent), true);
        for (uint16_t i = 0; i < g->named_column_count; i++) {
            TSFieldId field = ts_language_field_id_for_name(lang, g->named_columns[i], (uint32_t)strlen(g->named_columns[i]));
            if (!field) { fprintf(stderr, "grammar %s: unresolved column field %s\n", grammar_name, g->named_columns[i]); g->unresolved++; }
            else g->column_by_field[field] = (uint16_t)(i + 1);
        }
        if (g->column_tier < l->ntier) {
            const STier *tier = &l->tier[g->column_tier]; g->column_count = (uint32_t)tier->nnames;
            if (g->column_count) {
                g->columns = calloc(g->column_count, sizeof *g->columns); if (!g->columns) { perror("column names"); abort(); }
                for (uint32_t i = 0; i < g->column_count; i++) g->columns[i] = (GColumn){.name = (const uint8_t *)tier->names[i], .length = (uint32_t)strlen(tier->names[i]), .flags = S_FIELD_ALIAS};
            }
        }
    }
    if (!map_count_columns(g, l->ntier && l->tier[0].header)) g->unresolved++;
    for (uint8_t i = 0; i < g->separator_count; i++) {
        GSeparator *separator = &g->separators[i];
        separator->symbol = ts_language_symbol_for_name(lang, separator->type, (uint32_t)strlen(separator->type), true);
        if (!separator->symbol) { fprintf(stderr, "grammar %s: unresolved separator symbol %s\n", grammar_name, separator->type); g->unresolved++; }
    }
    if (g->symbol_count > UINT16_MAX + 1u) { g->unresolved++; return 0; }
    g->sym_rule_offset = calloc((size_t)g->symbol_count + 1, sizeof(uint32_t));
    if (!g->sym_rule_offset) { perror("grammar dispatch"); abort(); }
    uint8_t found[48] = {0};
    for (uint32_t sym = 0; sym < g->symbol_count; sym++) {
        const char *name = ts_language_symbol_name(lang, (TSSymbol)sym);
        if (name) for (int i = 0; i < g->n; i++) if (!strcmp(g->rule[i].type, name)) {
            g->sym_rule_offset[sym + 1]++; found[i] = 1;
        }
    }
    for (uint32_t sym = 0; sym < g->symbol_count; sym++) g->sym_rule_offset[sym + 1] += g->sym_rule_offset[sym];
    uint32_t total = g->sym_rule_offset[g->symbol_count];
    g->sym_rules = malloc(sizeof(uint16_t) * (total ? total : 1));
    if (!g->sym_rules) { perror("grammar rules"); abort(); }
    for (uint32_t sym = 0; sym < g->symbol_count; sym++) {
        const char *name = ts_language_symbol_name(lang, (TSSymbol)sym); uint32_t at = g->sym_rule_offset[sym];
        if (name) for (int i = 0; i < g->n; i++) if (!strcmp(g->rule[i].type, name)) g->sym_rules[at++] = (uint16_t)i;
    }
    for (int i = 0; i < g->n; i++) {
        GRule *r = &g->rule[i]; CompiledRule *c = &r->compiled;
        if (r->alias_tier && r->alias_tier <= l->ntier) str_set(r->alias, sizeof r->alias, l->tier[r->alias_tier - 1].name);
        c->kind = (GNodeKind)r->what; c->type_length = (uint16_t)strlen(r->type);
        c->alias_length = (uint16_t)strlen(r->alias);
        c->sym_id = ts_language_symbol_for_name(lang, r->type, c->type_length, true);
        if (!found[i]) { fprintf(stderr, "grammar %s: unresolved node %s\n", grammar_name, r->type); g->unresolved++; }
        if (r->field[0]) {
            c->field_id = ts_language_field_id_for_name(lang, r->field, (uint32_t)strlen(r->field));
            if (!c->field_id) { fprintf(stderr, "grammar %s: unresolved field %s on %s\n", grammar_name, r->field, r->type); g->unresolved++; }
        }
        if (r->parent[0]) {
            c->parent_sym = ts_language_symbol_for_name(lang, r->parent, (uint32_t)strlen(r->parent), true);
            if (!c->parent_sym) { fprintf(stderr, "grammar %s: unresolved parent %s on %s\n", grammar_name, r->parent, r->type); g->unresolved++; }
        }
        for (int j = 0; j < r->nname; j++) if (!grammar_path_compile(lang, r->name[j], &c->name[j])) {
            fprintf(stderr, "grammar %s: unresolved name path %s on %s\n", grammar_name, r->name[j], r->type); g->unresolved++;
        }
        for (unsigned j = 0; j < r->ncontext; j++) if (!grammar_path_compile(lang, r->context[j], &c->context[j])) {
            fprintf(stderr, "grammar %s: unresolved context path %s on %s\n", grammar_name, r->context[j], r->type); g->unresolved++;
        }
        g->has_context |= r->ncontext != 0;
        if (r->text[0] && !grammar_path_compile(lang, r->text, &c->text)) {
            fprintf(stderr, "grammar %s: unresolved text path %s on %s\n", grammar_name, r->text, r->type); g->unresolved++;
        }
        if (r->child_grammar[0]) {
            c->inner = inner_layout(r->child_grammar);
            if (!c->inner) { fprintf(stderr, "grammar %s: unresolved injection %s\n", grammar_name, r->child_grammar); g->unresolved++; }
            else c->child_grammar_id = c->inner->g.grammar_id;
        }
    }
    if (!layout_compile_parts(l)) g->unresolved++;
    return !g->unresolved;
}

/* ---- parting */
/* Where the part of tier k that begins at i ends: at the tier's separator, a quoted or escaped separator passed over
 * where the layout says the file writes them. *next: where the one after it begins. A carriage return before a line's
 * end belongs to the line's end. */

static const uint8_t *trimmed(const uint8_t *p, size_t *n){ while (*n && (p[0] == ' ' || p[0] == '\t')) { p++; (*n)--; } while (*n && (p[*n - 1] == ' ' || p[*n - 1] == '\t' || p[*n - 1] == '\r')) (*n)--; return p; }
/* A text as the file means it: its quotes off and a doubled quote one; an escaped character itself. */

/* A JSON value written inside a part (part NAME by object), read into the part via chained tree-sitter-json on TSRange:
 * an object's members each named by its key, a list's values each named by the list, a text with its escapes resolved,
 * a number or true or false as written; null says nothing. */
#include <tree_sitter/api.h>
size_t lp_utf8_put(uint32_t cp, uint8_t out[4]);
/* A decoder changes the lexer's view of the parent encoding, not its backing
 * bytes. Tree-sitter keeps offsets into the original record through every
 * injection, including CSV doubled quotes and XML/JSON escaped containers. */
#define G_DECODE_DEPTH 8
typedef struct { uint8_t mode[G_DECODE_DEPTH], n, escaped[G_DECODE_DEPTH], quoted[G_DECODE_DEPTH]; } GDecodePlan;
typedef struct {
    const uint8_t *src;
    uint32_t length;
    TSRange range;
    GDecodePlan decode;
    uint32_t physical_base, physical_end, *physical_lines, physical_line_count;
    TSPoint physical_origin;
    uint8_t physical_indexed;
} GInput;
/* Custom decoders change the lexer's logical line/column view, while byte
 * offsets still address the original file. Provenance uses this independent
 * physical source map, shared by every nested injection of the field. */
static int index_input(GInput *input){
    if (input->physical_indexed) return 1;
    if (input->decode.n) { fputs("decoded input has no original source map\n", stderr); return 0; }
    input->physical_indexed = 1; input->physical_base = input->range.start_byte; input->physical_end = input->range.end_byte;
    input->physical_origin = input->range.start_point;
    size_t capacity = 0; const uint8_t *at = input->src + input->range.start_byte, *end = input->src + input->range.end_byte;
    while (at < end) {
        const uint8_t *newline = memchr(at, '\n', (size_t)(end - at)); if (!newline) break;
        if (input->physical_line_count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            if (next > SIZE_MAX / sizeof(uint32_t) || next > UINT32_MAX) return 0;
            uint32_t *lines = scratch_alloc(next * sizeof *lines);
            if (capacity) memcpy(lines, input->physical_lines, capacity * sizeof *lines);
            input->physical_lines = lines; capacity = next;
        }
        input->physical_lines[input->physical_line_count++] = (uint32_t)(newline - input->src) + 1;
        at = newline + 1;
    }
    return 1;
}
static TSPoint physical_point(const GInput *input, uint32_t byte){
    if (byte < input->physical_base || byte > input->physical_end) { fputs("injected node is outside its original source field\n", stderr); abort(); }
    uint32_t lo = 0, hi = input->physical_line_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (input->physical_lines[mid] <= byte) lo = mid + 1; else hi = mid;
    }
    return (TSPoint){input->physical_origin.row + lo,
        lo ? byte - input->physical_lines[lo - 1] : input->physical_origin.column + byte - input->physical_base};
}
static TSRange source_range(const GInput *input, TSRange range){
    if (input->physical_indexed) { range.start_point = physical_point(input, range.start_byte); range.end_point = physical_point(input, range.end_byte); }
    return range;
}
typedef struct { uint32_t bytes; int32_t cp; uint8_t folded; } GUnit;
static GUnit g_unit(uint32_t bytes, int32_t cp){ return (GUnit){.bytes=bytes, .cp=cp}; }
static GUnit decode_unit(const GDecodePlan *plan, unsigned depth, const uint8_t *src, uint32_t length);
static GUnit decode_more(const GDecodePlan *plan, unsigned depth, const uint8_t *src, uint32_t length, uint32_t *at){
    if (*at >= length) return g_unit(0, -1);
    GUnit unit = decode_unit(plan, depth, src + *at, length - *at);
    *at += unit.bytes; return unit;
}
static int hex_digit(int32_t c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int scalar(uint32_t cp){ return cp < LP_NCP && (cp < 0xd800 || cp > 0xdfff); }
static GUnit decode_unit(const GDecodePlan *plan, unsigned depth, const uint8_t *src, uint32_t length){
    if (!length) return g_unit(0, -1);
    if (!depth) {
        size_t at = 0; uint32_t cp;
        if (!lp_utf8_next(src, length, &at, &cp) || !scalar(cp) ||
            (at == 2 && cp < 0x80) || (at == 3 && cp < 0x800) || (at == 4 && cp < 0x10000)) return g_unit(1, -1);
        return g_unit((uint32_t)at, (int32_t)cp);
    }
    GUnit first = decode_unit(plan, depth - 1, src, length);
    if (first.cp < 0) return first;
    unsigned mode = plan->mode[depth - 1]; uint32_t at = first.bytes;
    if (mode == G_LINES && (first.cp == '\r' || first.cp == '\n')) {
        if (first.cp == '\r') {
            uint32_t next_at = at; GUnit next = decode_more(plan, depth - 1, src, length, &next_at);
            if (next.cp == '\n') at = next_at;
        }
        while (at < length) {
            uint32_t next_at = at; GUnit next = decode_more(plan, depth - 1, src, length, &next_at);
            if (next.cp != ' ' && next.cp != '\t') break;
            at = next_at;
        }
        return (GUnit){.bytes=at, .cp=' ', .folded=1};
    }
    if (mode == G_DELIMITED) {
        if (plan->escaped[depth - 1] && first.cp == plan->escaped[depth - 1]) {
            GUnit next = decode_more(plan, depth - 1, src, length, &at);
            return next.bytes ? g_unit(at, next.cp) : first;
        }
        if (plan->quoted[depth - 1] && first.cp == '"') {
            GUnit next = decode_more(plan, depth - 1, src, length, &at);
            return next.cp == '"' ? g_unit(at, '"') : first;
        }
        return first;
    }
    if (mode == G_CSV && first.cp == '"') {
        GUnit next = decode_more(plan, depth - 1, src, length, &at);
        return next.cp == '"' ? g_unit(at, '"') : first;
    }
    if (mode == G_XML && first.cp == '&') {
        uint8_t name[16]; unsigned n = 0; int ended = 0;
        while (n < sizeof name && at < length) {
            GUnit next = decode_more(plan, depth - 1, src, length, &at);
            if (next.cp == ';') { ended = 1; break; }
            if (next.cp < 0 || next.cp > 127) return first;
            name[n++] = (uint8_t)next.cp;
        }
        uint32_t cp = UINT32_MAX;
        if (ended) {
            if (n == 2 && !memcmp(name, "lt", 2)) cp = '<';
            else if (n == 2 && !memcmp(name, "gt", 2)) cp = '>';
            else if (n == 3 && !memcmp(name, "amp", 3)) cp = '&';
            else if (n == 4 && !memcmp(name, "quot", 4)) cp = '"';
            else if (n == 4 && !memcmp(name, "apos", 4)) cp = '\'';
            else if (n > 1 && name[0] == '#') {
                unsigned i = 1, base = 10; cp = 0;
                if (i < n && name[i] == 'x') { base = 16; i++; }
                if (i == n) return g_unit(at, -1);
                for (; i < n; i++) { int d = hex_digit(name[i]); if (d < 0 || (unsigned)d >= base || cp > (UINT32_MAX - (unsigned)d) / base) return g_unit(at, -1); cp = cp * base + (unsigned)d; }
            }
        }
        if (cp == UINT32_MAX) return first; /* Unexpanded named DTD reference. */
        return g_unit(at, scalar(cp) ? (int32_t)cp : -1);
    }
    if ((mode == G_JSON || mode == G_TURTLE) && first.cp == '\\') {
        GUnit next = decode_more(plan, depth - 1, src, length, &at);
        int32_t cp = next.cp;
        switch (cp) {
            case '"': case '\\': case '/': return g_unit(at, cp);
            case '\'': return g_unit(at, mode == G_TURTLE ? cp : -1);
            case '~': case '.': case '-': case '!': case '$': case '&': case '(':
            case ')': case '*': case '+': case ',': case ';': case '=': case '?':
            case '#': case '@': case '%': case '_': return g_unit(at, mode == G_TURTLE ? cp : -1);
            case 'b': return g_unit(at, '\b');
            case 'f': return g_unit(at, '\f');
            case 'n': return g_unit(at, '\n');
            case 'r': return g_unit(at, '\r');
            case 't': return g_unit(at, '\t');
            case 'u': case 'U': {
                unsigned digits = cp == 'u' ? 4 : 8;
                if (cp == 'U' && mode != G_TURTLE) return g_unit(at, -1);
                uint32_t value = 0;
                for (unsigned i = 0; i < digits; i++) {
                    GUnit unit = decode_more(plan, depth - 1, src, length, &at); int d = hex_digit(unit.cp);
                    if (d < 0) return g_unit(at, -1); value = (value << 4) | (unsigned)d;
                }
                if (value >= 0xd800 && value <= 0xdbff && digits == 4) {
                    GUnit slash = decode_more(plan, depth - 1, src, length, &at), u = decode_more(plan, depth - 1, src, length, &at);
                    if (slash.cp != '\\' || u.cp != 'u') return g_unit(at, -1);
                    uint32_t low = 0;
                    for (unsigned i = 0; i < 4; i++) { GUnit unit = decode_more(plan, depth - 1, src, length, &at); int d = hex_digit(unit.cp); if (d < 0) return g_unit(at, -1); low = (low << 4) | (unsigned)d; }
                    if (low < 0xdc00 || low > 0xdfff) return g_unit(at, -1);
                    value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
                }
                return g_unit(at, scalar(value) ? (int32_t)value : -1);
            }
            default: return g_unit(at, -1);
        }
    }
    return first;
}
static __thread const GDecodePlan *active_decode;
static uint32_t decode_input(const uint8_t *src, uint32_t length, int32_t *cp){
    GUnit unit = decode_unit(active_decode, active_decode->n, src, length); *cp = unit.cp; return unit.bytes;
}
static const char *read_input(void *payload, uint32_t byte, TSPoint position, uint32_t *bytes){
    const GInput *input = payload; (void)position;
    if (byte >= input->range.end_byte) { *bytes = 0; return NULL; }
    *bytes = input->range.end_byte - byte;
    return (const char *)input->src + byte;
}
static const uint8_t *decode_text(const GDecodePlan *plan, const uint8_t *src, uint32_t *length){
    if (!plan->n || !*length) return src;
    uint8_t *out = scratch_alloc(*length); uint32_t at = 0, used = 0;
    while (at < *length) {
        GUnit unit = decode_more(plan, plan->n, src, *length, &at);
        if (unit.cp < 0 || !unit.bytes) { fputs("invalid escaped grammar input\n", stderr); exit(2); }
        if (unit.folded) while (used && (out[used - 1] == ' ' || out[used - 1] == '\t')) used--;
        used += (uint32_t)lp_utf8_put((uint32_t)unit.cp, out + used);
    }
    *length = used; return out;
}
static TSTree *parse_input_tree(const Layout *layout, const GInput *input){
    TSParser *parser = lp_tls_parser(layout->g.grammar_id);
    if (!parser || !ts_parser_set_included_ranges(parser, &input->range, 1)) return NULL;
    const GDecodePlan *saved = active_decode; active_decode = &input->decode;
    TSInput stream = {(void *)input, read_input, input->decode.n ? TSInputEncodingCustom : TSInputEncodingUTF8, decode_input};
    TSTree *parsed = ts_parser_parse(parser, NULL, stream);
    active_decode = saved;
    ts_parser_set_included_ranges(parser, NULL, 0);
    if (parsed && ts_node_has_error(ts_tree_root_node(parsed))) { ts_tree_delete(parsed); return NULL; }
    return parsed;
}


/* ---- a grammar's tree as the file's tree, by what the recipe says each kind of node is */
size_t xml_unescape(const uint8_t *s, size_t n, uint8_t *o);
typedef struct { const Layout *l; const uint8_t *src; const GInput *input; } GW;
/* The node a path names under a node: A, or A/B (the B in its A); a field of the node by its name where it has one. */
static int at_path(TSNode nd, const GPath *path, TSNode *out){
    TSNode cur = nd;
    for (uint8_t i = 0; i < path->n; i++) {
        const GPathStep *step = &path->step[i]; TSNode selected = {0};
        if (step->flags & 1) selected = ts_node_child_by_field_id(cur, step->field);
        if (ts_node_is_null(selected) && (step->flags & 2)) {
            TSTreeCursor *cursor = lp_tls_cursor(cur); if (!cursor) return 0;
            if (ts_tree_cursor_goto_first_child(cursor)) do {
                TSNode child = ts_tree_cursor_current_node(cursor);
                if (ts_node_symbol(child) == step->symbol) { selected = child; break; }
            } while (ts_tree_cursor_goto_next_sibling(cursor));
            lp_tls_cursor_release(cursor);
        }
        if (ts_node_is_null(selected)) return 0;
        cur = selected;
    }
    *out = cur; return 1;
}
/* The rule of a node, by its symbol: O(1) lookup via precompiled sym_rule_offset jump table. */
static const GRule *rule_at(GW *w, TSNode nd, TSFieldId field, TSSymbol parent){
    TSSymbol sy = ts_node_symbol(nd);
    const GMap *g = &w->l->g;
    if (!g->sym_rule_offset || sy >= g->symbol_count) return NULL;
    const GRule *best = NULL; unsigned specificity = 0;
    for (uint32_t at = g->sym_rule_offset[sy]; at < g->sym_rule_offset[sy + 1]; at++) {
        const GRule *rule = &g->rule[g->sym_rules[at]];
        const CompiledRule *c = &rule->compiled;
        if ((c->field_id && c->field_id != field) || (c->parent_sym && c->parent_sym != parent)) continue;
        unsigned score = (c->field_id ? 2u : 0u) + (c->parent_sym ? 1u : 0u);
        if (!best || score > specificity) { best = rule; specificity = score; }
    }
    return best;
}
static int grammar_quote(const GMap *grammar, int32_t quote){
    if (quote == '\'') return grammar->resolve == G_XML || grammar->resolve == G_TURTLE;
    return quote == '"' && (grammar->resolve == G_XML || grammar->resolve == G_JSON || grammar->resolve == G_TURTLE ||
                            grammar->resolve == G_CSV || (grammar->resolve == G_DELIMITED && grammar->quoted));
}
static int quoted_node(const GMap *grammar, TSNode node){
    TSSymbol symbol = ts_node_symbol(node);
    return grammar->quoted_symbols && symbol < grammar->symbol_count && grammar->quoted_symbols[symbol];
}
static const uint8_t *padding_off(const uint8_t *bytes, uint32_t *length, uint8_t escaped){
    uint32_t begin = 0, end = *length;
    while (begin < end && (bytes[begin] == ' ' || bytes[begin] == '\t')) begin++;
    while (end > begin && (bytes[end - 1] == ' ' || bytes[end - 1] == '\t' || bytes[end - 1] == '\r')) {
        uint32_t at = end - 1, escapes = 0;
        while (escaped && at > begin && bytes[at - 1] == escaped) { escapes++; at--; }
        if (escapes & 1) break;
        end--;
    }
    *length = end - begin; return bytes + begin;
}
/* A node's text as it means: its quotes off, and what the grammar's texts write for what they cannot write plainly
 * resolved (an XML reference, a JSON escape). */
static void g_text(GW *w, TSNode nd, int raw, const uint8_t **p, uint32_t *n){
    int padded = raw & 2, trim = raw & 4; raw &= 1;
    uint32_t len = ts_node_end_byte(nd) - ts_node_start_byte(nd);
    const uint8_t *s = decode_text(&w->input->decode, w->src + ts_node_start_byte(nd), &len);
    if (padded) s = padding_off(s, &len, w->l->g.escaped);
    int wrapped = 0, quoted = 0;
    if (!raw && w->l->g.resolve == G_LINES) { size_t n = len; s = trimmed(s, &n); len = (uint32_t)n; }
    if (!raw && w->l->g.resolve == G_TURTLE) {                                 /* Turtle: an IRI without its angle brackets, a language tag without its @, a text without its quotes (one or three) */
        if (len >= 2 && s[0] == '<' && s[len - 1] == '>') { s++; len -= 2; wrapped = 1; } else if (len >= 1 && s[0] == '@') { s++; len--; wrapped = 1; }
        else if (quoted_node(&w->l->g, nd) && len >= 6 && (s[0] == '"' || s[0] == '\'') && s[1] == s[0] && s[2] == s[0] && s[len - 1] == s[0] && s[len - 2] == s[0] && s[len - 3] == s[0]) { s += 3; len -= 6; wrapped = quoted = 1; } }
    if (!raw && !wrapped && quoted_node(&w->l->g, nd) && len >= 2 && grammar_quote(&w->l->g, s[0]) && s[len - 1] == s[0]) { s++; len -= 2; quoted = 1; }
    if (!raw && s_empty(w->l, s, len)) { *p = s; *n = len; return; }
    if (!raw && w->l->g.resolve && (w->l->g.resolve != G_CSV || quoted) &&
        (w->l->g.resolve != G_DELIMITED || w->l->g.escaped || quoted)) {
        GDecodePlan plan = {.mode={(uint8_t)w->l->g.resolve}, .n=1, .escaped={w->l->g.escaped}, .quoted={(uint8_t)quoted}};
        s = decode_text(&plan, s, &len);
    }
    if (!raw && w->l->g.resolve == G_LINES) { size_t n = len; s = trimmed(s, &n); len = (uint32_t)n; }
    if (trim) { size_t n = len; s = trimmed(s, &n); len = (uint32_t)n; }
    *p = s; *n = (uint32_t)len;
}
static TSPoint advance_point(TSPoint point, GUnit unit){
    if (unit.cp == '\n') { point.row++; point.column = 0; }
    else point.column += unit.bytes;
    return point;
}
static int input_padding_off(GInput *input, uint8_t escaped){
    uint32_t begin = input->range.end_byte, end = begin, escapes = 0;
    TSPoint begin_point = input->range.end_point, end_point = begin_point, point = input->range.start_point;
    int started = 0;
    for (uint32_t at = input->range.start_byte; at < input->range.end_byte;) {
        GUnit unit = decode_unit(&input->decode, input->decode.n, input->src + at, input->range.end_byte - at);
        if (unit.cp < 0 || !unit.bytes) return 0;
        TSPoint next = advance_point(point, unit);
        if (!started && unit.cp != ' ' && unit.cp != '\t') { begin = end = at; begin_point = end_point = point; started = 1; }
        if (started && ((unit.cp != ' ' && unit.cp != '\t' && unit.cp != '\r') || (escapes & 1))) { end = at + unit.bytes; end_point = next; }
        escapes = escaped && unit.cp == escaped ? escapes + 1 : 0;
        at += unit.bytes; point = next;
    }
    input->range = (TSRange){begin_point, end_point, begin, end}; return 1;
}
static int injected_input(GW *w, TSNode payload, int raw, GInput *result){
    int padded = raw & 2, trim = raw & 4; raw &= 1;
    GInput input = *w->input;
    input.range = (TSRange){ts_node_start_point(payload), ts_node_end_point(payload), ts_node_start_byte(payload), ts_node_end_byte(payload)};
    if ((input.decode.n || (!raw && w->l->g.resolve)) && !index_input(&input)) return 0;
    if (padded && !input_padding_off(&input, w->l->g.escaped)) return 0;
    uint32_t begin = input.range.start_byte, end = input.range.end_byte;
    GUnit first[3] = {{0}}, last[3] = {{0}}; TSPoint last_point[3]; uint32_t last_byte[3], count = 0;
    TSPoint point = input.range.start_point;
    for (uint32_t at = begin; at < end;) {
        GUnit unit = decode_unit(&input.decode, input.decode.n, input.src + at, end - at);
        if (unit.cp < 0 || !unit.bytes) return 0;
        if (count < 3) first[count] = unit;
        unsigned slot = count % 3; last[slot] = unit; last_byte[slot] = at; last_point[slot] = point;
        point = advance_point(point, unit); at += unit.bytes; count++;
    }
    if (!count) { *result = input; return 1; }
    unsigned head = 0, tail = 0, encoding = 0, quoting = 0;
    if (!raw) {
        int quoted = quoted_node(&w->l->g, payload) && count >= 2 && grammar_quote(&w->l->g, first[0].cp) && last[(count - 1) % 3].cp == first[0].cp;
        if (quoted) {
            quoting = 1;
            head = tail = 1;
            if (w->l->g.resolve == G_TURTLE && count >= 6 && first[1].cp == first[0].cp && first[2].cp == first[0].cp &&
                last[(count - 2) % 3].cp == first[0].cp && last[(count - 3) % 3].cp == first[0].cp) head = tail = 3;
        } else if (w->l->g.resolve == G_TURTLE && count >= 2 && first[0].cp == '<' && last[(count - 1) % 3].cp == '>') head = tail = 1;
        else if (w->l->g.resolve == G_TURTLE && first[0].cp == '@') head = 1;
        if (w->l->g.resolve == G_XML || w->l->g.resolve == G_TURTLE || w->l->g.resolve == G_LINES || quoted ||
            (w->l->g.resolve == G_DELIMITED && w->l->g.escaped)) encoding = (unsigned)w->l->g.resolve;
    }
    for (unsigned i = 0; i < head; i++) { input.range.start_byte += first[i].bytes; input.range.start_point = advance_point(input.range.start_point, first[i]); }
    if (tail) { input.range.end_byte = last_byte[(count - tail) % 3]; input.range.end_point = last_point[(count - tail) % 3]; }
    if (encoding) {
        if (input.decode.n == G_DECODE_DEPTH) { fputs("injected grammar encoding depth exceeds eight\n", stderr); return 0; }
        unsigned layer = input.decode.n++;
        input.decode.mode[layer] = (uint8_t)encoding; input.decode.escaped[layer] = w->l->g.escaped; input.decode.quoted[layer] = (uint8_t)quoting;
    }
    if (trim && !input_padding_off(&input, 0)) return 0;
    *result = input; return 1;
}

static const GColumn *column_alias(const Layout *layout, TSSymbol parent, TSSymbol child, TSFieldId field, uint32_t *ordinal){
    const GMap *grammar = &layout->g;
    if (!grammar->column_symbols || !grammar->column_parents || parent >= grammar->symbol_count || child >= grammar->symbol_count ||
        !grammar->column_parents[parent] || !grammar->column_symbols[child]) return NULL;
    uint32_t position = (*ordinal)++;
    if (grammar->named_column_count) {
        uint16_t slot = field <= grammar->field_count ? grammar->column_by_field[field] : 0;
        if (!slot) return NULL;
        position = slot - 1u;
    }
    if (position >= grammar->column_count) {
        if (!grammar->column_count || grammar->column_tier >= layout->ntier || !layout->tier[grammar->column_tier].rest) return NULL;
        position = grammar->column_count - 1;
    }
    return &grammar->columns[position];
}
/* Native cursor projection. Frames describe only the active ancestor chain;
 * completed fields leave this walker immediately through the visitor. */
typedef struct {
    const GCountProgram *program;
    SField closing;
    uint32_t remaining;
    uint8_t member, skip;
} CountFrame;
typedef struct {
    TSNode node, selected;
    size_t value_mark;
    const uint8_t *name, *context_name, *selected_name;
    const SNameBindings *bindings;
    uint32_t name_length, context_length, selected_length, column;
    TSFieldId field;
    TSSymbol parent;
    unsigned level;
    uint8_t context_list, close_scope, route, name_flags, unnamed;
    SField closing;
    CountFrame *counts;
    uint8_t count_input, count_member, count_omitted;
} CursorFrame;

static int cursor_project(const Layout *layout, TSNode root, const GInput *input, s_visit_fn visit, void *sink,
                          const uint8_t *name, uint32_t name_length, int list, int flatten);

static TSRange node_range(TSNode node){
    return (TSRange){ts_node_start_point(node), ts_node_end_point(node), ts_node_start_byte(node), ts_node_end_byte(node)};
}

int s_records_open(SRecords *records, const Layout *layout, const uint8_t *source, size_t length){
    *records = (SRecords){0};
    if (!layout->g.compiled || layout->g.unresolved || length > UINT32_MAX) return 0;
    TSParser *parser = lp_tls_parser(layout->g.grammar_id);
    if (!parser || !ts_parser_set_included_ranges(parser, NULL, 0)) return 0;
    TSTree *tree = ts_parser_parse_string(parser, NULL, (const char *)source, (uint32_t)length);
    if (!tree || ts_node_has_error(ts_tree_root_node(tree))) { if (tree) ts_tree_delete(tree); return 0; }
    TSTreeCursor *cursor = lp_tls_cursor(ts_tree_root_node(tree));
    if (!cursor) { ts_tree_delete(tree); return 0; }
    records->layout = layout; records->source = source; records->length = (uint32_t)length;
    records->tree = tree; records->cursor = cursor;
    records->header_pending = (uint8_t)(layout->ntier && layout->tier[0].header);
    return 1;
}
static int record_policy(const SRecords *records, TSNode node){
    const Layout *layout = records->layout;
    if (ts_node_start_byte(node) == ts_node_end_byte(node)) return 0;
    if (layout->ntier) {
        const STier *tier = &layout->tier[0];
        if (tier->skip > 0 && ts_node_start_point(node).row < (uint32_t)tier->skip) return 0;
        uint32_t start = ts_node_start_byte(node), length = ts_node_end_byte(node) - start;
        if (tier->commentlen && length >= (uint32_t)tier->commentlen &&
            !memcmp(records->source + start, tier->comment, (size_t)tier->commentlen)) return 0;
    }
    GInput input = {.src=records->source, .length=records->length}; input.range.end_byte = records->length;
    GW worker = {.l=layout, .src=records->source, .input=&input};
    TSNode parent = ts_node_parent(node);
    const GRule *rule = rule_at(&worker, node, ts_tree_cursor_current_field_id(records->cursor),
                               ts_node_is_null(parent) ? 0 : ts_node_symbol(parent));
    return !rule || rule->what != G_SKIP;
}
static int record_header(SRecords *records, TSNode node){
    const Layout *layout = records->layout; const GMap *grammar = &layout->g;
    TSSymbol parent = ts_node_symbol(node);
    if (!grammar->column_parents || parent >= grammar->symbol_count || !grammar->column_parents[parent]) {
        fputs("header is not a column record in the compiled grammar\n", stderr); return 0;
    }
    Layout *file_layout = malloc(sizeof *file_layout);
    if (!file_layout) { perror("file column layout"); return 0; }
    *file_layout = *layout; records->header_mark = scratch_mark(); records->file_layout = file_layout;
    GColumn *columns = scratch_alloc(S_NAMES * sizeof *columns); uint32_t count = 0;
    GInput input = {.src=records->source, .length=records->length}; input.range.end_byte = records->length;
    GW worker = {.l=layout, .src=records->source, .input=&input};
    TSTreeCursor *cursor = lp_tls_cursor(node);
    if (!cursor) return 0;
    int valid = 1;
    if (ts_tree_cursor_goto_first_child(cursor)) do {
        TSNode child = ts_tree_cursor_current_node(cursor); TSSymbol symbol = ts_node_symbol(child);
        if (!ts_node_is_named(child) || symbol >= grammar->symbol_count || !grammar->column_symbols[symbol]) continue;
        if (count == S_NAMES) { fputs("header exceeds the compiled column limit\n", stderr); valid = 0; break; }
        const uint8_t *name; uint32_t length; g_text(&worker, child, grammar->padded ? 2 : 0, &name, &length);
        if (grammar->trim) { size_t n = length; name = trimmed(name, &n); length = (uint32_t)n; }
        columns[count++] = (GColumn){.name = name, .length = length, .flags = S_FIELD_ALIAS};
        if (layout->compile_name) columns[count - 1].bindings = layout->compile_name(layout->name_program, name, length, &tls_scratch);
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    lp_tls_cursor_release(cursor);
    if (!valid || !count) return 0;
    file_layout->g.columns = columns; file_layout->g.column_count = count; records->layout = file_layout;
    if (!map_count_columns(&file_layout->g, 0)) return 0;
    records->header_pending = 0; return 1;
}
static int record_matches(const SRecords *records, TSNode node){
    const GMap *grammar = &records->layout->g;
    if (!grammar->record_name_count) return 1;
    GInput input = {.src=records->source, .length=records->length}; input.range.end_byte = records->length;
    GW worker = {.l=records->layout, .src=records->source, .input=&input};
    TSNode parent = ts_node_parent(node);
    const GRule *rule = rule_at(&worker, node, ts_tree_cursor_current_field_id(records->cursor),
                               ts_node_is_null(parent) ? 0 : ts_node_symbol(parent));
    if (!rule || rule->what != G_GROUP) return 0;
    size_t mark = scratch_mark();
    const uint8_t *name = NULL; uint32_t length = 0;
    if (rule->compiled.alias_length) { name = (const uint8_t *)rule->alias; length = rule->compiled.alias_length; }
    else if (rule->kind) { name = (const uint8_t *)rule->type; length = rule->compiled.type_length; }
    else for (int i = 0; i < rule->nname; i++) {
        TSNode key;
        if (at_path(node, &rule->compiled.name[i], &key)) { g_text(&worker, key, 0, &name, &length); break; }
    }
    if (!name) { scratch_restore(mark); return 0; }
    uint64_t hash = part_hash(name, length);
    size_t bucket = (hash ^ (hash >> 32)) & 31u; int matched = 0;
    for (uint8_t i = grammar->record_name_buckets[bucket]; i; i = grammar->record_names[i - 1].next) {
        const GRecordName *record = &grammar->record_names[i - 1];
        if (record->hash == hash && record->length == length && !memcmp(record->name, name, length)) { matched = 1; break; }
    }
    scratch_restore(mark); return matched;
}
static int record_advance(SRecords *records, int descend){
    if (descend && ts_tree_cursor_goto_first_child(records->cursor)) return 1;
    do { if (ts_tree_cursor_goto_next_sibling(records->cursor)) return 1; }
    while (ts_tree_cursor_goto_parent(records->cursor));
    records->done = 1; return 0;
}
int s_record_next(SRecords *records, SRecord *record){
    if (!records->cursor || records->done) return 0;
    if (records->started && !record_advance(records, records->descend)) return 0;
    records->started = 1;
    const GMap *grammar = &records->layout->g;
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(records->cursor); TSSymbol symbol = ts_node_symbol(node);
        int selected = ts_node_is_named(node) && (!grammar->record_symbols ||
            (symbol < grammar->symbol_count && grammar->record_symbols[symbol])) && record_matches(records, node);
        int header = 0;
        if (!selected && grammar->record_name_count && ts_node_is_named(node)) {
            GInput input = {.src=records->source, .length=records->length}; input.range.end_byte = records->length;
            GW worker = {.l=records->layout, .src=records->source, .input=&input};
            TSNode parent = ts_node_parent(node);
            const GRule *rule = rule_at(&worker, node, ts_tree_cursor_current_field_id(records->cursor), ts_node_is_null(parent) ? 0 : ts_node_symbol(parent));
            header = rule && rule->what == G_GROUP && rule->ncontext;
        }
        if (selected || header) {
            if (!record_policy(records, node)) { if (!record_advance(records, 0)) return 0; continue; }
            if (records->header_pending) {
                if (!record_header(records, node)) { records->done = records->failed = 1; return 0; }
                if (!record_advance(records, 0)) return 0;
                continue;
            }
            records->descend = (uint8_t)header;
            *record = (SRecord){.node=node, .range=node_range(node), .ordinal=records->ordinal++, .header=(uint8_t)header}; return 1;
        }
        if (!record_advance(records, 1)) return 0;
    }
}
void s_records_close(SRecords *records){
    if (records->cursor) lp_tls_cursor_release(records->cursor);
    if (records->tree) ts_tree_delete(records->tree);
    if (records->file_layout) { free(records->file_layout); scratch_restore(records->header_mark); }
    *records = (SRecords){0};
}

/* Part names are compiled at load time. Probe exact/prefix hash buckets and
 * preserve source-order precedence even when a wildcard overlaps an exact rule. */
static const SPart *part_at(const Layout *layout, const uint8_t *name, uint32_t length){
    if (!layout->npart) return NULL;
    const SPart *found = NULL;
    uint32_t limit = length < 63 ? length : 63;
    for (uint32_t n = 0; n <= limit; n++) {
        if (n != length && !(layout->part_prefix_lengths & (UINT64_C(1) << n))) continue;
        uint64_t hash = part_hash(name, n);
        size_t bucket = (hash ^ (hash >> 32)) & 63u;
        for (uint16_t i = layout->part_buckets[bucket]; i; i = layout->part[i - 1].next) {
            const SPart *part = &layout->part[i - 1];
            if ((!found || part < found) && part->name_hash == hash && part->name_length == n &&
                (part->wildcard || n == length) && (!n || !memcmp(name, part->path, n))) found = part;
        }
    }
    return found;
}

static int close_field(s_visit_fn visit, void *sink, const SField *field){
    int result = visit(sink, S_CLOSE, field);
    if (!(result & S_VISIT_OK)) return 0;
    if (result & S_VISIT_RELEASE) scratch_restore(field->arena_begin);
    return 1;
}

static int cursor_injection(GW *worker, TSNode payload, const Layout *inner, int raw, SField field,
                            s_visit_fn visit, void *sink){
    GInput input;
    if (!injected_input(worker, payload, raw, &input)) return 0;
    if (field.flags & S_FIELD_RECORD) {
        TSTree *tree = parse_input_tree(inner, &input);
        if (!tree) { fputs("record does not parse completely with its product grammar\n", stderr); return 0; }
        int valid = cursor_project(inner, ts_tree_root_node(tree), &input, visit, sink, NULL, 0, 0, 1);
        ts_tree_delete(tree); return valid;
    }
    field.kind = S_GROUP; field.value = NULL; field.value_length = 0; field.flags |= S_FIELD_LIST;
    int action = visit(sink, S_OPEN, &field);
    if (!(action & S_VISIT_OK)) return 0;
    if (action & S_VISIT_SKIP) return close_field(visit, sink, &field);
    if (input.range.start_byte != input.range.end_byte) {
        TSTree *tree = parse_input_tree(inner, &input);
        if (!tree) { fputs("injected field does not parse completely\n", stderr); return 0; }
        int valid = cursor_project(inner, ts_tree_root_node(tree), &input, visit, sink,
                                   field.name, field.name_length, 1, 1);
        ts_tree_delete(tree);
        if (!valid) return 0;
    }
    return close_field(visit, sink, &field);
}

static int part_piece(const SPart *part, const GInput *input, SField field, TSRange range, TSNode assignment,
                      s_visit_fn visit, void *sink){
    field.range = source_range(input, range); field.kind = S_TEXT;
    field.flags = (uint8_t)((field.flags & ~(S_FIELD_LIST | S_FIELD_PARTS | S_FIELD_JOIN | S_FIELD_TRIM)) | S_FIELD_SAME_TIER);
    if (!ts_node_is_null(assignment)) {
        uint32_t length = ts_node_start_byte(assignment) - range.start_byte;
        field.name = decode_text(&input->decode, input->src + range.start_byte, &length);
        field.bindings = NULL;
        field.name_length = length; field.kind = S_VALUE;
        range.start_byte = ts_node_end_byte(assignment); range.start_point = ts_node_end_point(assignment);
    }
    uint32_t length = range.end_byte - range.start_byte;
    field.value = decode_text(&input->decode, input->src + range.start_byte, &length); field.value_length = length;
    if (part->padded) {
        size_t n = field.value_length; field.value = trimmed(field.value, &n); field.value_length = (uint32_t)n;
        if (field.kind == S_VALUE) { n = field.name_length; field.name = trimmed(field.name, &n); field.name_length = (uint32_t)n; }
    }
    if (part->space && memchr(field.value, part->space, field.value_length)) {
        uint8_t *value = scratch_alloc(field.value_length);
        for (uint32_t i = 0; i < field.value_length; i++) value[i] = field.value[i] == (uint8_t)part->space ? ' ' : field.value[i];
        field.value = value;
    }
    return visit(sink, S_FIELD, &field) != 0;
}

/* The delimiter grammar identifies every boundary. The semantic instruction
 * selects symbol IDs; it never scans the field again to find delimiters. */
static int cursor_parts(GW *worker, TSNode payload, const SPart *part, int raw, SField field,
                        s_visit_fn visit, void *sink){
    if (part->inner) return cursor_injection(worker, payload, part->inner, raw, field, visit, sink);
    GInput input;
    if (!injected_input(worker, payload, raw, &input)) return 0;
    TSTree *tree = parse_input_tree(part->syntax, &input);
    if (!tree) { fputs("part field does not parse completely\n", stderr); return 0; }
    TSTreeCursor *cursor = lp_tls_cursor(ts_tree_root_node(tree));
    if (!cursor) { ts_tree_delete(tree); return 0; }
    field.kind = S_GROUP; field.value = NULL; field.value_length = 0; field.flags |= S_FIELD_LIST | S_FIELD_PARTS;
    int action = visit(sink, S_OPEN, &field), valid = (action & S_VISIT_OK) != 0;
    if (valid && (action & S_VISIT_SKIP)) {
        lp_tls_cursor_release(cursor); ts_tree_delete(tree); return close_field(visit, sink, &field);
    }
    TSRange piece = input.range; TSNode assignment = {0}; uint32_t made = 0;
    if (valid && ts_tree_cursor_goto_first_child(cursor)) do {
        TSNode token = ts_tree_cursor_current_node(cursor); TSSymbol symbol = ts_node_symbol(token);
        if (symbol == part->separator && (!part->pieces || made < (uint32_t)part->pieces - 1)) {
            piece.end_byte = ts_node_start_byte(token); piece.end_point = ts_node_start_point(token);
            if (piece.end_byte > piece.start_byte) {
                if (!part_piece(part, &input, field, piece, assignment, visit, sink)) { valid = 0; break; }
                made++;
            }
            piece.start_byte = ts_node_end_byte(token); piece.start_point = ts_node_end_point(token); assignment = (TSNode){0};
        } else if (part->assignment && symbol == part->assignment && ts_node_is_null(assignment)) assignment = token;
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    piece.end_byte = input.range.end_byte; piece.end_point = input.range.end_point;
    if (valid && piece.end_byte > piece.start_byte) valid = part_piece(part, &input, field, piece, assignment, visit, sink);
    lp_tls_cursor_release(cursor); ts_tree_delete(tree);
    return valid && close_field(visit, sink, &field);
}

static int cursor_child(CursorFrame *frame, TSTreeCursor *cursor){
    do {
        TSNode node = ts_tree_cursor_current_node(cursor);
        if (!ts_node_is_named(node)) continue;
        if (frame->selected.id && (ts_node_start_byte(node) > ts_node_start_byte(frame->selected) ||
                                  ts_node_end_byte(node) < ts_node_end_byte(frame->selected))) continue;
        return 1;
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    return 0;
}

/* Counts are values of native field nodes, not a second lexer over the row.
 * Column and member names were bound when the layout compiled. */
static int count_number(const GCountProgram *program, const uint8_t *text, uint32_t length, uint32_t *out){
    uint32_t value = 0; *out = 0;
    for (uint32_t i = 0; i < length; i++) {
        unsigned c = text[i], digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 36;
        if (digit >= program->radix) return 1; /* a non-number counts no groups */
        if (value > (UINT32_MAX - digit) / program->radix) return 0;
        value = value * program->radix + digit;
    }
    *out = value; return 1;
}
static int count_group_range(const Layout *layout, TSNode parent, TSNode first, unsigned arity, TSRange *range){
    TSTreeCursor *cursor = lp_tls_cursor(parent); if (!cursor) return 0;
    unsigned members = 0; *range = node_range(first);
    int positioned = ts_tree_cursor_goto_first_child_for_byte(cursor, range->start_byte) >= 0;
    if (positioned && !ts_node_eq(first, ts_tree_cursor_current_node(cursor)))
        positioned = range->start_byte == range->end_byte && ts_tree_cursor_goto_previous_sibling(cursor) &&
            ts_node_eq(first, ts_tree_cursor_current_node(cursor));
    if (positioned) do {
        TSNode node = ts_tree_cursor_current_node(cursor); TSSymbol symbol = ts_node_symbol(node);
        if (symbol >= layout->g.symbol_count || !layout->g.column_symbols[symbol]) continue;
        range->end_byte = ts_node_end_byte(node); range->end_point = ts_node_end_point(node);
        if (++members == arity) break;
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    lp_tls_cursor_release(cursor); return members == arity;
}

static int cursor_project(const Layout *layout, TSNode root, const GInput *input, s_visit_fn visit, void *sink,
                          const uint8_t *name, uint32_t name_length, int list, int flatten){
    GW worker = {.l=layout, .src=input->src, .input=input};
    const void *omit = NULL;
    if (flatten) {
        const GRule *rule = rule_at(&worker, root, 0, 0);
        while (!rule && ts_node_named_child_count(root) == 1) {
            root = ts_node_named_child(root, 0); rule = rule_at(&worker, root, 0, 0);
        }
        if (rule && rule->what == G_GROUP && !rule->kind && !rule->nname && !rule->compiled.alias_length) { omit = root.id; list = rule->list; }
    }
    TSTreeCursor *cursor = lp_tls_cursor(root);
    if (!cursor) return 0;
    size_t stack_mark = tls_scratch.stack_used, capacity = 32, count = 1;
    CursorFrame *frames = scratch_stack_alloc(capacity * sizeof *frames);
    frames[0] = (CursorFrame){.node=root, .value_mark=scratch_mark(), .context_name=name, .context_length=name_length, .context_list=(uint8_t)list};
    int valid = 1;
    while (count) {
        CursorFrame *frame = &frames[count - 1];
        const GRule *rule = frame->route || frame->node.id == omit ? NULL : rule_at(&worker, frame->node, frame->field, frame->parent);
        int children = 1;
        SField field = {.source=frame->node, .range=source_range(input, node_range(frame->node)), .name=frame->name, .name_length=frame->name_length,
                        .bindings=frame->bindings,
                        .arena_begin=frame->value_mark, .grammar=layout->g.grammar_id,
                        .flags=frame->name_flags};
        if (frame->count_omitted) { children = 0; goto after_rule; }
        if (rule && (rule->trim || (rule->what == G_TEXT && layout->g.trim))) field.flags |= S_FIELD_TRIM;
        if (rule && (rule->compiled.flags & 1u)) field.flags |= S_FIELD_SAME_TIER;
        if (rule && (rule->compiled.flags & 2u)) field.flags |= S_FIELD_RECORD;
        if (rule && rule->alias_tier && rule->alias_tier <= layout->ntier) { field.tier = rule->alias_tier - 1u; field.flags |= S_FIELD_TIER; }
        if (rule && rule->compiled.alias_length) {
            field.bindings = NULL;
            field.name = (const uint8_t *)rule->alias; field.name_length = rule->compiled.alias_length; field.flags |= S_FIELD_ALIAS;
        }
        if (rule && rule->what == G_SKIP) children = 0;
        else if (rule && rule->what == G_MEMBER) {
            field.bindings = NULL;
            TSNode key, value;
            if (!at_path(frame->node, &rule->compiled.name[0], &key) || !at_path(frame->node, &rule->compiled.text, &value) || value.id == frame->node.id) { valid = 0; break; }
            g_text(&worker, key, 0, &frame->selected_name, &frame->selected_length);
            frame->selected = value;
            if (frame->level < (unsigned)layout->nlevels) {
                field.name = (const uint8_t *)layout->levels[frame->level];
                field.name_length = layout->level_lengths[frame->level]; field.kind = S_GROUP;
                int action = visit(sink, S_OPEN, &field);
                if (!(action & S_VISIT_OK)) { valid = 0; break; }
                frame->closing = field; frame->close_scope = 1; frame->level++;
                if (action & S_VISIT_SKIP) { children = 0; goto after_rule; }
                frame->context_name = field.name; frame->context_length = field.name_length; frame->context_list = 0;
                SField key_field = {.source=key, .range=source_range(input, node_range(key)), .name=(const uint8_t *)"key", .name_length=3,
                                    .value=frame->selected_name, .value_length=frame->selected_length,
                                    .grammar=layout->g.grammar_id, .kind=S_VALUE};
                if (!visit(sink, S_FIELD, &key_field)) { valid = 0; break; }
                frame->selected_name = (const uint8_t *)"value"; frame->selected_length = 5;
            }
        } else if (rule && (rule->what == G_VALUE || rule->what == G_TEXT || rule->what == G_NOTE)) {
            TSNode payload = frame->node;
            int text_mode = rule->raw | (rule->what == G_TEXT && layout->g.padded ? 2 : 0) |
                            (!rule->join && (rule->trim || (rule->what == G_TEXT && layout->g.trim)) ? 4 : 0);
            field.kind = S_TEXT;
            if (rule->what == G_NOTE) {
                field.bindings = NULL;
                TSNode key;
                int has_key = at_path(frame->node, &rule->compiled.name[0], &key);
                int has_value = at_path(frame->node, &rule->compiled.text, &payload);
                if (has_key && has_value) {
                    g_text(&worker, key, 0, &field.name, &field.name_length);
                    size_t n = field.name_length; field.name = trimmed(field.name, &n); field.name_length = (uint32_t)n;
                } else {
                    field.name = NULL; field.name_length = 0;
                    if (has_key) payload = key;
                    else if (!has_value) { children = 0; goto after_rule; }
                }
                field.kind = S_NOTE;
            } else if (rule->what == G_VALUE) {
                field.bindings = NULL;
                TSNode key;
                if (!at_path(frame->node, &rule->compiled.name[0], &key) || !at_path(frame->node, &rule->compiled.text, &payload)) { valid = 0; break; }
                g_text(&worker, key, 1, &field.name, &field.name_length); field.kind = S_VALUE;
            } else if (!field.name && rule->kind) { field.name = (const uint8_t *)rule->type; field.name_length = rule->compiled.type_length; }
            if (frame->count_input) {
                g_text(&worker, payload, text_mode, &field.value, &field.value_length);
                const GCountProgram *program = &layout->g.counts[frame->count_input - 1]; uint32_t groups;
                if (count < 2 || !count_number(program, field.value, field.value_length, &groups)) { valid = 0; break; }
                CursorFrame *parent = &frames[count - 2];
                if (groups && !parent->counts) parent->counts = scratch_stack_alloc(sizeof *parent->counts);
                if (parent->counts) *parent->counts = (CountFrame){.program=groups ? program : NULL, .remaining=groups};
            }
            if (rule->compiled.inner) {
                if (!cursor_injection(&worker, payload, rule->compiled.inner, text_mode, field, visit, sink)) { valid = 0; break; }
            } else {
                if (!frame->count_input) g_text(&worker, payload, text_mode, &field.value, &field.value_length);
                field.range = source_range(input, node_range(payload));
                if (rule->what == G_NOTE) {
                    size_t n = field.value_length; field.value = trimmed(field.value, &n); field.value_length = (uint32_t)n;
                } else if (field.name && !(rule->alias_tier && rule->alias_tier <= layout->ntier)) field.kind = S_VALUE;
                else if (!field.name && !frame->unnamed) { field.name = frame->context_name; field.name_length = frame->context_length; }
                if (rule->join) field.flags |= S_FIELD_JOIN;
                const SPart *part = field.name ? part_at(layout, field.name, field.name_length) : NULL;
                if (part && !s_empty(layout, field.value, field.value_length)) {
                    if (!cursor_parts(&worker, payload, part, text_mode, field, visit, sink)) { valid = 0; break; }
                } else if (!visit(sink, S_FIELD, &field)) { valid = 0; break; }
            }
            children = 0;
        } else if (rule && rule->what == G_GROUP) {
            if (!field.name && frame->context_list) { field.name = frame->context_name; field.name_length = frame->context_length; }
            if (!field.name && rule->kind) { field.name = (const uint8_t *)rule->type; field.name_length = rule->compiled.type_length; }
            if (!field.name) for (int i = 0; i < rule->nname; i++) {
                TSNode key; if (at_path(frame->node, &rule->compiled.name[i], &key)) { g_text(&worker, key, 0, &field.name, &field.name_length); break; }
            }
            field.kind = S_GROUP; if (rule->list) field.flags |= S_FIELD_LIST;
            int action = visit(sink, S_OPEN, &field);
            if (!(action & S_VISIT_OK)) { valid = 0; break; }
            frame->closing = field; frame->close_scope = 1;
            if (action & S_VISIT_SKIP) { children = 0; goto after_rule; }
            frame->context_name = field.name; frame->context_length = field.name_length; frame->context_list = (uint8_t)rule->list;
            if (!ts_node_named_child_count(frame->node)) {
                g_text(&worker, frame->node, 0, &field.value, &field.value_length); field.kind = S_TEXT;
                if (field.value_length && !visit(sink, S_FIELD, &field)) { valid = 0; break; }
            }
        }
after_rule:
        if (children && ts_tree_cursor_goto_first_child(cursor)) {
            if (cursor_child(frame, cursor)) goto push_child;
            ts_tree_cursor_goto_parent(cursor);
        }
        for (;;) {
            frame = &frames[count - 1];
            if (frame->counts && frame->counts->remaining) { valid = 0; break; }
            if (frame->close_scope && count == 1 && layout->g.numbered_length && layout->ntier > 1) {
                char text[24]; size_t length = u64_digits((uint64_t)ts_node_start_point(root).row + 1, text);
                uint8_t *value = scratch_alloc(length); memcpy(value, text, length);
                TSPoint point = ts_node_start_point(root); uint32_t byte = ts_node_start_byte(root);
                SField number = {.range={point, point, byte, byte}, .name=(const uint8_t *)layout->tier[0].numbered,
                    .value=value, .name_length=layout->g.numbered_length, .value_length=(uint32_t)length,
                    .grammar=layout->g.grammar_id, .kind=S_VALUE, .flags=S_FIELD_DERIVED};
                if (!visit(sink, S_FIELD, &number)) { valid = 0; break; }
            }
            if (frame->close_scope && !close_field(visit, sink, &frame->closing)) { valid = 0; break; }
            int counted_member = frame->count_member;
            if (!--count) break;
            frame = &frames[count - 1];
            if (counted_member) {
                CountFrame *state = frame->counts;
                if (!state || !state->program || !state->remaining) { valid = 0; break; }
                if (++state->member == state->program->arity) {
                    if (!close_field(visit, sink, &state->closing)) { valid = 0; break; }
                    state->member = state->skip = 0;
                    if (!--state->remaining) state->program = NULL;
                }
            }
            /* A member has one selected value; its key and bookkeeping siblings
             * have no disposition and are never walked as independent fields. */
            if (!frame->selected.id && ts_tree_cursor_goto_next_sibling(cursor) && cursor_child(frame, cursor)) goto push_child;
            ts_tree_cursor_goto_parent(cursor);
        }
        if (!valid || !count) break;
        continue;
push_child:;
        TSNode child = ts_tree_cursor_current_node(cursor);
        CursorFrame next = {.node=child, .context_name=frame->context_name, .context_length=frame->context_length,
                            .value_mark=frame->selected.id ? frame->value_mark : scratch_mark(),
                            .context_list=frame->context_list, .level=frame->level,
                            .field=ts_tree_cursor_current_field_id(cursor), .parent=ts_node_symbol(frame->node)};
        if (frame->selected.id) {
            if (child.id == frame->selected.id) { next.name = frame->selected_name; next.name_length = frame->selected_length; }
            else { next.route = 1; next.selected = frame->selected; next.selected_name = frame->selected_name; next.selected_length = frame->selected_length; }
        } else {
            const GRule *child_rule = rule_at(&worker, child, next.field, next.parent);
            if (!(child_rule && (child_rule->what == G_GROUP || child_rule->what == G_SKIP) && (child_rule->nname || child_rule->kind))) {
                next.name = (const uint8_t *)layout->g.field_names[next.field]; next.name_length = layout->g.field_lengths[next.field];
            }
            TSSymbol symbol = ts_node_symbol(child);
            int column = layout->g.column_parents && layout->g.column_symbols && next.parent < layout->g.symbol_count &&
                symbol < layout->g.symbol_count && layout->g.column_parents[next.parent] && layout->g.column_symbols[symbol];
            uint32_t position = frame->column;
            const GColumn *alias;
            if (column && frame->counts && frame->counts->program) {
                CountFrame *state = frame->counts; const GCountProgram *program = state->program;
                if (!state->member) {
                    TSRange range;
                    if (!count_group_range(layout, frame->node, child, program->arity, &range)) { valid = 0; break; }
                    state->closing = (SField){.source=frame->node, .range=source_range(input, range),
                        .name=program->group.name, .name_length=program->group.length, .kind=S_GROUP,
                        .bindings=program->group.bindings,
                        .grammar=layout->g.grammar_id, .arena_begin=scratch_mark()};
                    int action = visit(sink, S_OPEN, &state->closing);
                    if (!(action & S_VISIT_OK)) { valid = 0; break; }
                    state->skip = (uint8_t)((action & S_VISIT_SKIP) != 0);
                }
                alias = &program->member[state->member]; next.count_member = 1; next.count_omitted = state->skip;
            } else {
                alias = column_alias(layout, next.parent, symbol, next.field, &frame->column);
                if (column && position < S_NAMES) next.count_input = layout->g.count_by_column[position];
            }
            if (alias) { next.name = alias->name; next.name_length = alias->length; next.name_flags = alias->flags; next.bindings = alias->bindings; }
            else if (!next.name && layout->g.column_parents && layout->g.column_symbols &&
                     next.parent < layout->g.symbol_count && ts_node_symbol(child) < layout->g.symbol_count &&
                     layout->g.column_parents[next.parent] && layout->g.column_symbols[ts_node_symbol(child)]) next.unnamed = 1;
        }
        if (count == capacity) {
            if (capacity > SIZE_MAX / (2 * sizeof *frames)) { valid = 0; break; }
            CursorFrame *grown = scratch_stack_alloc(capacity * 2 * sizeof *grown); memcpy(grown, frames, count * sizeof *grown); frames = grown; capacity *= 2;
        }
        frames[count++] = next;
    }
    lp_tls_cursor_release(cursor); tls_scratch.stack_used = stack_mark; return valid;
}

typedef struct { s_visit_fn visit; void *sink; } ContextVisitor;
static int context_visit(void *sink, int event, const SField *field){
    ContextVisitor *context = sink; SField inherited = *field;
    inherited.flags |= S_FIELD_CONTEXT;
    return context->visit(context->sink, event, &inherited);
}
static TSFieldId node_field(TSNode node, TSNode parent){
    if (ts_node_is_null(parent)) return 0;
    TSTreeCursor *cursor = lp_tls_cursor(parent);
    if (!cursor) { fputs("ancestor context: cursor pool exhausted\n", stderr); abort(); }
    TSFieldId field = 0;
    if (ts_tree_cursor_goto_first_child_for_byte(cursor, ts_node_start_byte(node)) >= 0 &&
        ts_node_eq(node, ts_tree_cursor_current_node(cursor))) field = ts_tree_cursor_current_field_id(cursor);
    lp_tls_cursor_release(cursor); return field;
}
/* Record workers inherit only the ancestor header paths declared by their
 * grammar. Native parent links find those headers without visiting sibling
 * records, copying their bytes, or projecting a second whole-file tree. */
static int record_context(const Layout *layout, TSNode root, const GInput *input, s_visit_fn visit, void *sink,
                          SField **opened, uint32_t *count){
    *opened = NULL; *count = 0;
    if (!layout->g.has_context) return 1;
    uint32_t depth = 0;
    for (TSNode node = ts_node_parent(root); !ts_node_is_null(node); node = ts_node_parent(node)) depth++;
    if (!depth) return 1;
    TSNode *ancestors = scratch_alloc((size_t)depth * sizeof *ancestors);
    SField *fields = scratch_alloc((size_t)depth * sizeof *fields);
    TSNode node = ts_node_parent(root);
    for (uint32_t i = depth; i; i--) { ancestors[i - 1] = node; node = ts_node_parent(node); }
    GW worker = {.l=layout, .src=input->src, .input=input}; ContextVisitor context = {visit, sink};
    for (uint32_t i = 0; i < depth; i++) {
        node = ancestors[i]; TSNode parent = ts_node_parent(node);
        const GRule *rule = rule_at(&worker, node, node_field(node, parent), ts_node_is_null(parent) ? 0 : ts_node_symbol(parent));
        if (!rule || rule->what != G_GROUP || !rule->ncontext) continue;
        SField field = {.source=node, .range=source_range(input, node_range(node)), .grammar=layout->g.grammar_id,
                        .kind=S_GROUP, .flags=S_FIELD_CONTEXT};
        if (rule->compiled.alias_length) { field.name = (const uint8_t *)rule->alias; field.name_length = rule->compiled.alias_length; }
        else if (rule->kind) { field.name = (const uint8_t *)rule->type; field.name_length = rule->compiled.type_length; }
        else for (int k = 0; k < rule->nname; k++) {
            TSNode name;
            if (at_path(node, &rule->compiled.name[k], &name)) { g_text(&worker, name, 0, &field.name, &field.name_length); break; }
        }
        if (!(visit(sink, S_OPEN, &field) & S_VISIT_OK)) return 0;
        fields[(*count)++] = field;
        for (unsigned k = 0; k < rule->ncontext; k++) {
            TSNode header;
            if (at_path(node, &rule->compiled.context[k], &header) &&
                !cursor_project(layout, header, input, context_visit, &context, NULL, 0, 0, 0)) return 0;
        }
    }
    *opened = fields; return 1;
}
typedef struct { s_visit_fn visit; void *sink; uint8_t opened, skipped; } HeaderVisitor;
static int header_visit(void *sink, int event, const SField *field){
    HeaderVisitor *header = sink;
    if (event == S_OPEN) {
        if (header->opened) { header->skipped = 1; return S_VISIT_OK | S_VISIT_SKIP; }
        header->opened = 1;
    } else if (event == S_CLOSE && header->skipped) { header->skipped = 0; return S_VISIT_OK; }
    return header->visit(header->sink, event, field);
}
static int cursor_record(const Layout *layout, TSNode root, int header, const uint8_t *source, size_t length, s_visit_fn visit, void *sink){
    if (!layout->g.compiled || layout->g.unresolved || length > UINT32_MAX || !visit || ts_node_is_null(root) || ts_node_has_error(root)) return 0;
    GInput input = {0}; input.src = source; input.length = (uint32_t)length; input.range.end_byte = (uint32_t)length;
    size_t mark = scratch_mark();
    SField *context; uint32_t contexts;
    int valid = record_context(layout, root, &input, visit, sink, &context, &contexts);
    HeaderVisitor visitor = {visit, sink, 0, 0};
    if (valid) valid = cursor_project(layout, root, &input, header ? header_visit : visit, header ? &visitor : sink, NULL, 0, 0, 0);
    while (valid && contexts) valid = (visit(sink, S_CLOSE, &context[--contexts]) & S_VISIT_OK) != 0;
    if (valid) {
        SField end = {.source=root, .range=node_range(root), .arena_begin=mark, .grammar=layout->g.grammar_id};
        valid = (visit(sink, S_FINISH, &end) & S_VISIT_OK) != 0;
    }
    scratch_restore(mark); return valid;
}
int s_cursor(const Layout *layout, TSNode root, const uint8_t *source, size_t length, s_visit_fn visit, void *sink){
    return cursor_record(layout, root, 0, source, length, visit, sink);
}
int s_cursor_record(const Layout *layout, const SRecord *record, const uint8_t *source, size_t length, s_visit_fn visit, void *sink){
    return cursor_record(layout, record->node, record->header, source, length, visit, sink);
}

/* ---- laplace structure LAYOUT FILE [-n NODES]: a file's tree as a layout parts it, for writing recipes. LAYOUT is a
 * file of layout lines (a recipe's own lines; whatever is not layout is passed over). */
typedef struct { long left; uint64_t shown; unsigned depth; uint8_t spans; } Show;
static int show_field(void *sink, int event, const SField *field){
    Show *show = sink;
    if (event == S_FINISH) return S_VISIT_OK;
    if (event == S_CLOSE) { if (show->depth) show->depth--; return 1; }
    if (show->left > 0) {
        show->left--;
        printf("%*s%s", (int)show->depth * 2, "", event == S_OPEN ? "group" : field->kind == S_VALUE ? "value" : field->kind == S_NOTE ? "note" : "text");
        if (field->name_length) printf(" %.*s", (int)field->name_length, field->name);
        if (show->spans) printf(" [%u:%u @%u:%u-%u:%u]", field->range.start_byte, field->range.end_byte,
            field->range.start_point.row + 1, field->range.start_point.column + 1,
            field->range.end_point.row + 1, field->range.end_point.column + 1);
        if (event == S_FIELD) {
            fputs("  \"", stdout);
            for (uint32_t i = 0; i < field->value_length && i < 80; i++) {
                uint8_t c = field->value[i];
                if (c == '\n') fputs("\\n", stdout); else if (c == '\t') fputs("\\t", stdout); else putchar(c);
            }
            fputs(field->value_length > 80 ? "\xE2\x80\xA6\"" : "\"", stdout);
        }
        putchar('\n');
    }
    if (event == S_OPEN) show->depth++;
    return 1;
}
int cmd_structure(int argc, char **argv){
    long nodes = 60; const char *lay = NULL, *file = NULL; int spans = 0;
    for (int a = 1; a < argc; a++) { if (!strcmp(argv[a], "-n") && a + 1 < argc) nodes = atol(argv[++a]); else if (!strcmp(argv[a], "--spans")) spans = 1; else if (!lay) lay = argv[a]; else file = argv[a]; }
    if (!lay || !file) { fprintf(stderr, "usage: laplace structure LAYOUT FILE [-n nodes] [--spans]\n"); return 2; }
    Layout *l = calloc(1, sizeof *l); FILE *f = fopen(lay, "r"); if (!f) { perror(lay); return 1; } char line[4096], grammar[64] = {0};
    while (fgets(line, sizeof line, f)) {
        comment_off(line); char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "grammar")) { char *name = strtok(NULL, " \t\r\n"); if (!name) { fclose(f); free(l); return 2; } str_set(grammar, sizeof grammar, name); }
        else if (!strcmp(tok, "format")) {
            char *name = strtok(NULL, " \t\r\n"), format[64];
            if (!name || *grammar || l->g.n) { fclose(f); free(l); return 2; }
            str_set(format, sizeof format, name); str_set(grammar, sizeof grammar, format);
            if (!use_format(l, format)) { fclose(f); free(l); return 2; }
        }
        else if (layout_says(l, lay, tok) < 0) { fclose(f); free(l); return 2; }
    }
    fclose(f);
    if (!layout_compile_recipe(l, grammar, sizeof grammar)) { free(l); return 2; }
    size_t n = 0; const uint8_t *src = os_map_read(file, &n);
    if (!src) { perror(file); free(l); return 1; }
    Show s = {.left=nodes, .spans=(uint8_t)spans}; uint64_t units = 0;
    if (*grammar) {
        if (n > UINT32_MAX) { fputs("structure: input exceeds the Tree-sitter byte range\n", stderr); free(l); return 2; }
        SRecords records;
        if (!s_records_open(&records, l, src, n)) {
            fprintf(stderr, "structure: %s does not parse completely with %s\n", file, grammar);
            free(l); return 2;
        }
        SRecord record; int valid = 1; units = 0;
        while (s_record_next(&records, &record)) {
            if (s.left > 0) printf("-- %llu\n", (unsigned long long)record.ordinal);
            if (!s_cursor_record(records.layout, &record, src, n, show_field, &s)) { valid = 0; break; }
            units++;
        }
        if (records.failed) valid = 0;
        s_records_close(&records);
        if (!valid) { fputs("structure: compiled cursor projection failed\n", stderr); free(l); return 2; }
    }
    printf("%llu parts of the outermost tier in %zu bytes\n", (unsigned long long)units, n); free(l); return 0;
}
