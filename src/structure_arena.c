/* Thread-local virtual-memory scratch arena and declarative layout directive parser. */
#define _GNU_SOURCE
#include "structure_internal.h"
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
__thread SayScratch tls_scratch;
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
SayScratch scratch_enter(SayScratch *arena){
    scratch_ready(); arena_ready(arena);
    SayScratch previous = tls_scratch; tls_scratch = *arena; return previous;
}
void scratch_leave(SayScratch *arena, SayScratch previous){
    *arena = tls_scratch; tls_scratch = previous;
}

/* Transient cursor frames grow from the other end of the same reservation.
 * Releasing an injected parser's stack cannot invalidate decoded field values
 * that the enclosing record still needs. No per-field heap or retained frame
 * vector is required, even for millions of sibling injections. */
void *scratch_stack_alloc(size_t bytes){
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
    if (!strcmp(tok, "scope")) {
        char *open = strtok(NULL, " \t\r\n"), *end = strtok(NULL, " \t\r\n"), *close = strtok(NULL, " \t\r\n"),
             *name = strtok(NULL, " \t\r\n"), *identity = strtok(NULL, " \t\r\n"), *empty = strtok(NULL, " \t\r\n"), *single = strtok(NULL, " \t\r\n");
        if (!open || !end || strcmp(end, "end") || !close || !name || strcmp(name, "name") || !identity ||
            !empty || strcmp(empty, "empty") || !single || strtok(NULL, " \t\r\n") || l->g.scope_count == 8) {
            fprintf(stderr, "%s: scope OPEN end CLOSE name PATH empty SINGLE\n", path); return -1;
        }
        GScope *scope = &l->g.scopes[l->g.scope_count++];
        str_set(scope->open, sizeof scope->open, open); str_set(scope->close, sizeof scope->close, close);
        str_set(scope->name, sizeof scope->name, identity); str_set(scope->empty, sizeof scope->empty, single); return 1;
    }
    if (!strcmp(tok, "scope-space") || !strcmp(tok, "scope-misc") || !strcmp(tok, "scope-prefix") || !strcmp(tok, "scope-first")) {
        int kind = !strcmp(tok, "scope-space") ? 0 : !strcmp(tok, "scope-misc") ? 1 : !strcmp(tok, "scope-prefix") ? 2 : 3;
        char *name; unsigned added = 0;
        while ((name = strtok(NULL, " \t\r\n"))) {
            uint8_t *count = kind == 0 ? &l->g.scope_space_count : kind == 1 ? &l->g.scope_misc_count : kind == 2 ? &l->g.scope_prefix_count : &l->g.scope_first_count;
            unsigned capacity = kind == 1 ? 16 : kind == 3 ? 4 : 8;
            if (*count == capacity || strlen(name) >= 48) return -1;
            char (*names)[48] = kind == 0 ? l->g.scope_space : kind == 1 ? l->g.scope_misc : kind == 2 ? l->g.scope_prefix : l->g.scope_first;
            str_set(names[(*count)++], 48, name); added++;
        }
        return added ? 1 : -1;
    }
    if (!strcmp(tok, "framing")) {
        char *name = strtok(NULL, " \t\r\n");
        if (!name || strlen(name) >= sizeof l->g.framing || strtok(NULL, " \t\r\n")) {
            fprintf(stderr, "%s: framing GRAMMAR\n", path); return -1;
        }
        str_set(l->g.framing, sizeof l->g.framing, name); return 1;
    }
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
        if (!strcmp(by, "grammar")) { str_set(p->grammar, sizeof p->grammar, sp); }
        else p->seplen = sep_read(sp, p->sep, sizeof p->sep);
        while ((w = strtok(NULL, " \t\r\n"))) { if (!strcmp(w, "padded")) { p->padded = 1; continue; } char *v = strtok(NULL, " \t\r\n"), b[8];
            if (v && !strcmp(w, "is")) p->islen = sep_read(v, p->is, sizeof p->is);
            else if (v && !strcmp(w, "pieces") && atoi(v) > 0) p->pieces = atoi(v);
            else if (v && !strcmp(w, "space") && sep_read(v, b, sizeof b)) p->space = b[0];
            else if (v && !strcmp(w, "grammar")) { str_set(p->grammar, sizeof p->grammar, v); }
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
            if(!strcmp(name,"children")){l->g.record_children=1;continue;}
            if (l->g.record_count == 16) { fprintf(stderr, "%s: too many record symbols\n", path); return -1; }
            str_set(l->g.record_types[l->g.record_count++], 48, name);
        }
        if (!l->g.record_count && !l->g.record_children) { fprintf(stderr, "%s: records requires a node type\n", path); return -1; }
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
    if (!strcmp(tok, "resolve")) { char *v = strtok(NULL, " \t\r\n"); l->g.resolve = v && !strcmp(v, "xml") ? G_XML : v && !strcmp(v, "json") ? G_JSON : v && !strcmp(v, "turtle") ? G_TURTLE : v && !strcmp(v, "csv") ? G_CSV : v && !strcmp(v, "delimited") ? G_DELIMITED : v && !strcmp(v, "lines") ? G_LINES : v && !strcmp(v, "sql") ? G_SQL : v && !strcmp(v, "html") ? G_HTML : 0;
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
    if (!strcmp(tok, "gaps")) {
        char *kind = strtok(NULL, " \t\r\n");
        if (!kind || strcmp(kind, "text") || strtok(NULL, " \t\r\n")) return -1;
        l->g.gaps = 1; return 1;
    }
    if (!strcmp(tok, "node")) { char *ty = strtok(NULL, " \t\r\n"), *wh = strtok(NULL, " \t\r\n"), *w;
        if (!ty || !wh || l->g.n == (int)(sizeof l->g.rule/sizeof *l->g.rule)) { fprintf(stderr, "%s: node TYPE group | value | text | member | note | skip ...\n", path); return -1; }
        GRule *r=!strcmp(ty,"@record")?&l->g.record_fallback:&l->g.rule[l->g.n++];
        memset(r, 0, sizeof *r); str_set(r->type, sizeof r->type, ty);
        r->what = !strcmp(wh, "group") ? G_GROUP : !strcmp(wh, "value") ? G_VALUE : !strcmp(wh, "text") ? G_TEXT : !strcmp(wh, "member") ? G_MEMBER : !strcmp(wh, "skip") ? G_SKIP : !strcmp(wh, "note") ? G_NOTE : 0;
        if (!r->what) { fprintf(stderr, "%s: node %s %s: group | value | text | member | note | skip\n", path, ty, wh); return -1; }
        r->compiled.kind = (GNodeKind)r->what;
        int into = 0;                                                       /* names, text path, injection, field, parent */
        while ((w = strtok(NULL, " \t\r\n"))) {
            if (into == 3) { str_set(r->child_grammar, sizeof r->child_grammar, w); into = 0; continue; }
            if (into == 4) { str_set(r->field, sizeof r->field, w); into = 0; continue; }
            if (into == 5) { str_set(r->parent, sizeof r->parent, w); into = 0; continue; }
            if (into == 6) { str_set(r->alias, sizeof r->alias, w); into = 0; continue; }
            if (into == 7) {
                char *end; unsigned long index = strtoul(w, &end, 10);
                if (!*w || *end || index >= S_TIERS) { fprintf(stderr, "%s: invalid alias tier\n", path); return -1; }
                r->alias_tier = (uint8_t)(index + 1); into = 0; continue;
            }
            if (into == 9) { str_set(r->numbered,sizeof r->numbered,w); r->number_length=(uint8_t)strlen(r->numbered); into=0; continue; }
            if (into == 12) { str_set(r->capture,sizeof r->capture,w); r->capture_length=(uint16_t)strlen(r->capture); into=0; continue; }
            if (into == 13) {
                char *end; unsigned long level = strtoul(w, &end, 10);
                if (!*w || *end || !level || level > UINT8_MAX) { fprintf(stderr, "%s: context-level requires 1..255\n", path); return -1; }
                r->context_level = (uint8_t)level; into = 0; continue;
            }
            if (into == 14) { str_set(r->condition, sizeof r->condition, w); into = 15; continue; }
            if (into == 15) {
                if (!strcmp(w, "missing")) { r->condition_mode = 2; into = 0; continue; }
                if (!strcmp(w, "is")) { r->condition_mode = 1; into = 16; continue; }
                fprintf(stderr, "%s: node condition requires is VALUE or missing\n", path); return -1;
            }
            if (into == 16) { str_set(r->equals, sizeof r->equals, w); r->equals_length = (uint8_t)strlen(r->equals); into = 0; continue; }
            if (!strcmp(w, "when")) { into = 14; continue; }
            if (!strcmp(w, "casefold")) { r->casefold = 1; continue; }
            if (into == 10 || into == 11) {
                char *end; unsigned long value=strtoul(w,&end,10);
                if (!*w || *end || (into==10 ? value<2 || value>36 : value<1 || value>64)) {
                    fprintf(stderr,"%s: numbered radix 2..36, width 1..64\n",path); return -1;
                }
                if (into==10) r->number_radix=(uint8_t)value; else r->number_width=(uint8_t)value;
                into=0; continue;
            }
            if (!strcmp(w, "context")) { into = 8; continue; }
            if (!strcmp(w, "context-level")) { into = 13; continue; }
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
            else if (!strcmp(w,"numbered")) into=9;
            else if (!strcmp(w,"radix")) into=10;
            else if (!strcmp(w,"width")) into=11;
            else if (!strcmp(w,"capture")) into=12;
            else if (!strcmp(w, "same-tier")) r->compiled.flags |= 1u;
            else if (!strcmp(w, "required")) r->compiled.flags |= 4u;
            else if (!strcmp(w, "raw")) r->raw = 1; else if (!strcmp(w, "join")) r->join = 1; else if (!strcmp(w, "list")) r->list = 1; else if (!strcmp(w, "kind")) r->kind = 1; else if (!strcmp(w, "trim")) r->trim = 1;
            else if (into == 1 && r->nname < 4) str_set(r->name[r->nname++], 64, w); else if (into == 2) str_set(r->text, sizeof r->text, w);
            else { fprintf(stderr, "%s: node %s: %s is not something a node's line says\n", path, ty, w); return -1; } }
        if ((into >= 3 && into != 8) || (into == 8 && !r->ncontext) || (into == 1 && !r->nname) || (into == 2 && !r->text[0])) { fprintf(stderr, "%s: node %s: missing rule argument\n", path, ty); return -1; }
        if (r->ncontext && r->what != G_GROUP) { fprintf(stderr, "%s: context paths require a group\n", path); return -1; }
        if (r->context_level && !r->ncontext) { fprintf(stderr, "%s: context-level requires context paths\n", path); return -1; }
        if (r->capture[0] && r->what!=G_GROUP) { fprintf(stderr,"%s: capture requires a group\n",path);return -1; }
        if ((r->what == G_VALUE || r->what == G_MEMBER || r->what == G_NOTE) && (!r->nname || !r->text[0])) { fprintf(stderr, "%s: node %s %s name PATH %s PATH\n", path, ty, wh, r->what == G_MEMBER ? "value" : "text"); return -1; }
        if ((r->alias[0] || r->alias_tier) && r->what != G_GROUP && r->what != G_TEXT) { fprintf(stderr, "%s: node %s: an alias applies to a group or text rule\n", path, ty); return -1; }
        if ((r->numbered[0] && r->what!=G_GROUP) || ((!r->numbered[0]) && (r->number_radix || r->number_width))) {
            fprintf(stderr,"%s: numbered applies to a group rule\n",path); return -1;
        }
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
