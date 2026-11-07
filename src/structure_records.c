/* Single-pass grammar-bounded record streaming, context snapshots, and event scope streams. */
#define _GNU_SOURCE
#include "structure_internal.h"
static const SContext *record_context_snapshot(SRecords *records, TSNode node);
static int records_open(SRecords *records, const Layout *layout, const uint8_t *source, size_t length, int tail, int scopes){
    *records = (SRecords){0};
    if (!layout->g.compiled || layout->g.unresolved || length > UINT32_MAX) return 0;
    TSParser *parser = lp_tls_parser(layout->g.grammar_id);
    if (!parser || !ts_parser_set_included_ranges(parser, NULL, 0)) return 0;
    size_t prefix = length; TSTree *tree;
    /* Top-level AST records need the same grammar-boundary recovery as scope
     * events. Byte-halving a large declaration can repeatedly admit only its
     * tiny leading comment and reparse the remaining megabytes. */
    int framing = tail && (layout->g.scope_count || layout->g.record_children);
    for (;;) {
        ts_parser_reset(parser);
        GInput input = {.src=source, .length=(uint32_t)prefix}; input.range.end_byte = (uint32_t)prefix;
        TSInput stream = {&input, read_input, TSInputEncodingUTF8, NULL};
        tree = ts_parser_parse_with_options(parser, NULL, stream,
            (TSParseOptions){.progress_callback=framing ? NULL : reject_recovery});
        if (tree && !ts_node_has_error(ts_tree_root_node(tree))) break;
        if (framing) {
            /* A partial packaging event can end anywhere in a byte window.
             * Use recovery only to locate the leading complete grammar nodes,
             * then strictly reparse that prefix before admitting any record.
             * Halving arbitrary byte offsets can miss every legal boundary. */
            uint32_t complete = 0;
            if (tree) {
                TSNode root = ts_tree_root_node(tree);
                uint32_t count = ts_node_named_child_count(root);
                for (uint32_t i = 0; i < count; i++) {
                    TSNode child = ts_node_named_child(root, i);
                    if (ts_node_has_error(child) || ts_node_is_missing(child)) break;
                    complete = ts_node_end_byte(child);
                }
                ts_tree_delete(tree);
            }
            if (!complete || complete >= prefix) return 0;
            prefix = complete; framing = 0;
            continue;
        }
        if (tree) ts_tree_delete(tree);
        if (!tail || prefix < 2) return 0;
        /* A byte window may end inside a legal quoted field. Probe a smaller
         * grammar-valid prefix, then retain its last record as lookahead. No
         * format scanner guesses where a quote, newline or record ends. */
        prefix /= 2;
    }
    /* `levels` names successive mapping members. A syntactically valid scalar,
     * list, or missing document cannot satisfy that declared input contract.
     * Inspect the compiled node disposition once at the document boundary; no
     * corpus name, JSON scanner, or runtime node-name comparison is involved. */
    if (layout->nlevels) {
        GW worker = {.l=layout, .src=source}; TSNode root = ts_tree_root_node(tree);
        const GRule *rule = rule_at(&worker, root, 0, 0);
        while (!rule && ts_node_named_child_count(root) == 1) {
            root = ts_node_named_child(root, 0); rule = rule_at(&worker, root, 0, 0);
        }
        if (!rule || rule->what != G_GROUP || rule->list) {
            fputs("named mapping levels require an object document\n", stderr);
            ts_tree_delete(tree); return 0;
        }
    }
    GInput scope_input = {.src=source, .length=(uint32_t)prefix}; scope_input.range.end_byte = (uint32_t)prefix;
    if (scopes && layout->g.scope_count && !scope_validate(layout, ts_tree_root_node(tree), &scope_input)) { ts_tree_delete(tree); return 0; }
    TSTreeCursor *cursor = lp_tls_cursor(ts_tree_root_node(tree));
    if (!cursor) { ts_tree_delete(tree); return 0; }
    records->layout = layout; records->source = source; records->length = (uint32_t)prefix;
    records->tree = tree; records->cursor = cursor;
    records->limit = (uint32_t)prefix;
    records->header_pending = (uint8_t)(layout->ntier && layout->tier[0].header);
    return 1;
}
int s_records_open(SRecords *records, const Layout *layout, const uint8_t *source, size_t length){
    return records_open(records, layout, source, length, 0, 1);
}
static int record_policy(const SRecords *records, TSNode node){
    const Layout *layout = records->layout;
    if (ts_node_end_byte(node) <= records->skip_before) return 0;
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
static int record_header_fields(SRecords *records, TSNode node){
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
        uint8_t *owned_name = scratch_alloc(length); if (length) memcpy(owned_name, name, length); name = owned_name;
        columns[count++] = (GColumn){.name = name, .length = length, .flags = S_FIELD_ALIAS};
        if (layout->compile_name) columns[count - 1].bindings = layout->compile_name(layout->name_program, name, length, &tls_scratch);
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    lp_tls_cursor_release(cursor);
    if (!valid || !count) return 0;
    file_layout->g.columns = columns; file_layout->g.column_count = count; records->layout = file_layout;
    if (!map_count_columns(&file_layout->g, 0)) return 0;
    records->header_pending = 0; records->skip_before = ts_node_end_byte(node); return 1;
}
static int record_header(SRecords *records, TSNode node){
    if (!records->header_arena) return record_header_fields(records, node);
    SayScratch previous = scratch_enter(records->header_arena);
    int valid = record_header_fields(records, node);
    scratch_leave(records->header_arena, previous); return valid;
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
static int scope_record_next(SRecords *records, SRecord *record){
    if (records->done) return 0;
    int advanced = records->started ? ts_tree_cursor_goto_next_sibling(records->cursor) : ts_tree_cursor_goto_first_child(records->cursor);
    records->started = 1;
    const GMap *grammar = &records->layout->g;
    while (advanced) {
        TSNode node = ts_tree_cursor_current_node(records->cursor); uint8_t role = scope_role(grammar, node);
        if ((role && role <= 8) || (role >= 17 && role <= 24)) {
            int selected = record_matches(records, node), header = 0;
            if (!selected && grammar->has_context) {
                GInput input = {.src=records->source, .length=records->length};
                GW worker = {.l=records->layout, .src=records->source, .input=&input};
                const GRule *rule = rule_at(&worker, node, 0, ts_node_symbol(ts_node_parent(node)));
                header = rule && rule->what == G_GROUP && rule->ncontext;
            }
            if (selected || header) {
                TSRange range;
                if (!scope_extent(grammar, node, &range)) { records->failed = records->done = 1; return 0; }
                const SContext *context = grammar->has_context ? record_context_snapshot(records, node) : NULL;
                if (records->failed) return 0;
                if (selected && role <= 8) {
                    TSNode parent = ts_node_parent(node);
                    ts_tree_cursor_reset(records->cursor, parent);
                    if (ts_tree_cursor_goto_first_child_for_byte(records->cursor, range.end_byte - 1) < 0) { records->failed = records->done = 1; return 0; }
                } else if (role <= 8) scope_push(records, node, role);
                *record = (SRecord){.node=node, .range=range, .ordinal=records->ordinal++, .header=(uint8_t)header, .context=context};
                return 1;
            }
            if (role <= 8) scope_push(records, node, role);
        } else if (role >= 9 && role <= 16) {
            if (!records->scopes || !records->scopes->count) { records->failed = records->done = 1; return 0; }
            records->scopes->count--;
        }
        advanced = ts_tree_cursor_goto_next_sibling(records->cursor);
    }
    records->done = 1; return 0;
}
static int scope_stream_next(SRecords *records, SRecord *record);
int s_record_next(SRecords *records, SRecord *record){
    if (!records->cursor || records->done) return 0;
    if (records->stream) return scope_stream_next(records, record);
    if (records->layout->g.scope_count) return scope_record_next(records, record);
    if (records->started && !record_advance(records, records->descend)) return 0;
    records->started = 1;
    const GMap *grammar = &records->layout->g;
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(records->cursor); TSSymbol symbol = ts_node_symbol(node);
        if (ts_node_start_byte(node) >= records->limit) { records->done = 1; return 0; }
        int empty_surface = grammar->record_children && grammar->record_fallback.capture[0] &&
            ts_node_eq(node,ts_tree_root_node(records->tree)) && !ts_node_named_child_count(node);
        int selected = ts_node_is_named(node) && (empty_surface || !grammar->record_children || ts_node_eq(ts_node_parent(node),ts_tree_root_node(records->tree))) && (!grammar->record_symbols ||
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
            if (ts_node_end_byte(node) > records->limit || ts_node_has_error(node)) { records->done = 1; return 0; }
            if (!record_policy(records, node)) { if (!record_advance(records, 0)) return 0; continue; }
            if (records->header_pending) {
                if (!record_header(records, node)) { records->done = records->failed = 1; return 0; }
                if (!record_advance(records, 0)) return 0;
                continue;
            }
            records->descend = (uint8_t)header;
            const SContext *context = records->layout->g.has_context ? record_context_snapshot(records, node) : NULL;
            if (records->failed) return 0;
            records->last_context = context;
            TSRange range = node_range(node);
            if (grammar->record_children && grammar->record_fallback.capture[0]) {
                range.start_byte = records->surface_end; range.start_point = records->surface_point;
                TSNode next = ts_node_next_named_sibling(node);
                if (ts_node_is_null(next) || ts_node_start_byte(next) >= records->limit) {
                    for (uint32_t i = range.end_byte; i < records->limit; i++) {
                        if (records->source[i] == '\n') { range.end_point.row++; range.end_point.column=0; }
                        else range.end_point.column++;
                    }
                    range.end_byte = records->limit;
                }
                records->surface_end = range.end_byte; records->surface_point = range.end_point;
            }
            *record = (SRecord){.node=node, .range=range, .ordinal=records->ordinal++, .header=(uint8_t)header, .context=context}; return 1;
        }
        if (!record_advance(records, 1)) return 0;
    }
}
void s_records_close(SRecords *records){
    if (records->cursor) lp_tls_cursor_release(records->cursor);
    if (records->tree) ts_tree_delete(records->tree);
    if (records->file_layout) {
        free(records->file_layout);
        if (records->header_arena) records->header_arena->used = records->header_mark;
        else scratch_restore(records->header_mark);
    }
    arena_release(&records->context_arena);
    *records = (SRecords){0};
}
int s_records_context_window(SRecords *records, const Layout *layout, const uint8_t *source, size_t length,
                     int final, size_t *consumed, SayScratch *headers, SContextStream *context_stream){
    *consumed = 0;
    if (!records_open(records, layout, source, length, !final, 1)) return final ? -1 : 0;
    records->header_arena = headers;
    records->context_stream = context_stream;
    TSNode root = ts_tree_root_node(records->tree);
    SRecord record; uint32_t last = 0, previous = 0;
    while (s_record_next(records, &record)) { previous = last; last = record.range.end_byte; }
    uint32_t prefix = final ? (uint32_t)length : previous;
    if (records->failed || (!prefix && length)) {
        s_records_close(records); return final ? -1 : 0;
    }
    *consumed = prefix; records->limit = prefix;
    ts_tree_cursor_reset(records->cursor, root);
    records->started = records->done = records->failed = records->descend = 0; records->ordinal = 0;
    records->surface_end = 0; records->surface_point = (TSPoint){0,0};
    records->last_context = NULL;
    return 1;
}
int s_records_window(SRecords *records, const Layout *layout, const uint8_t *source, size_t length,
                     int final, size_t *consumed, SayScratch *headers){
    return s_records_context_window(records, layout, source, length, final, consumed, headers, NULL);
}
/* Immutable lexical context, independent of the packaging AST and input byte
 * buffer. A window projects each ancestor header once; record workers replay
 * compiled fields rather than parsing the same headers for every record. */
typedef struct ContextEntry {
    const void *owner; uint32_t start; SContext value; struct ContextEntry *next;
} ContextEntry;
struct SContextStore { ContextEntry **buckets; size_t capacity, count; };
typedef struct { SContext *context; SayScratch *arena; } ContextCapture;
static int capture_context(void *sink, int event, const SField *field){
    ContextCapture *capture = sink;
    ContextEvent *entry = arena_alloc(capture->arena, sizeof *entry);
    *entry = (ContextEvent){.field=s_context_field_copy(capture->arena, field), .event=event};
    if (capture->context->last) capture->context->last->next = entry;
    else capture->context->first = entry;
    capture->context->last = entry; return S_VISIT_OK;
}
static size_t context_bucket(const void *owner, uint32_t start, size_t capacity){
    uint64_t key = (uint64_t)(uintptr_t)owner ^ ((uint64_t)start << 32 | start);
    key ^= key >> 30; key *= UINT64_C(0xbf58476d1ce4e5b9); key ^= key >> 27;
    return (size_t)key & (capacity - 1);
}
static int scope_context_header(const Layout *layout, const GRule *rule, TSNode node, const GInput *input,
    ContextCapture *capture, SField *field){
    ContextVisitor inherited = {capture_context, capture};
    ScopeOpening opening = {.visit=context_visit, .sink=&inherited, .node=node, .header_only=1};
    if (!cursor_project(layout, node, input, scope_opening_visit, &opening, NULL, 0, 0, 0) || !(opening.action & S_VISIT_OK)) return 0;
    *field = opening.field;
    for (unsigned k = 0; k < rule->ncontext; k++) {
        TSNode header;
        if (!at_path(node, &rule->compiled.context[k], &header)) continue;
        if (header.id != node.id) {
            if (!cursor_project(layout, header, input, context_visit, &inherited, NULL, 0, 0, 0)) return 0;
        } else {
            TSTreeCursor *cursor = lp_tls_cursor(node); if (!cursor) return 0;
            int valid = 1;
            if (ts_tree_cursor_goto_first_child(cursor)) do {
                if (!cursor_project(layout, ts_tree_cursor_current_node(cursor), input, context_visit, &inherited, NULL, 0, 0, 0)) { valid = 0; break; }
            } while (ts_tree_cursor_goto_next_sibling(cursor));
            lp_tls_cursor_release(cursor); if (!valid) return 0;
        }
    }
    return 1;
}
static int scope_record_context(SRecords *records, const GInput *input, ContextCapture *capture, SField **opened, uint32_t *count){
    *opened = NULL; *count = 0;
    if (!records->scopes || !records->scopes->count) return 1;
    const Layout *layout = records->layout; SScopeState *state = records->scopes;
    SField *fields = scratch_alloc((size_t)state->count * sizeof *fields);
    GW worker = {.l=layout, .src=input->src, .input=input};
    for (uint32_t i = 0; i < state->count; i++) {
        TSNode node = state->frames[i].node;
        const GRule *rule = rule_at(&worker, node, 0, ts_node_symbol(ts_node_parent(node)));
        if (!rule || rule->what != G_GROUP || !rule->ncontext) continue;
        if (!scope_context_header(layout, rule, node, input, capture, &fields[*count])) return 0;
        (*count)++;
    }
    *opened = fields; return 1;
}
static const SContext *scope_stream_context(const SScopeStream *stream);
static const SContext *record_context_snapshot(SRecords *records, TSNode node){
    if (records->stream) return scope_stream_context(records->stream);
    SayScratch *arena = &records->context_arena;
    SContextStore *store = records->contexts;
    if (!store) { store = arena_alloc(arena, sizeof *store); *store = (SContextStore){0}; records->contexts = store; }
    TSNode parent = records->layout->g.scope_count ?
        (records->scopes && records->scopes->count ? records->scopes->frames[records->scopes->count - 1].node : (TSNode){0}) : ts_node_parent(node);
    const void *owner = parent.id; uint32_t start = ts_node_is_null(parent) ? 0 : ts_node_start_byte(parent);
    if (store->capacity) {
        for (ContextEntry *entry = store->buckets[context_bucket(owner, start, store->capacity)]; entry; entry = entry->next)
            if (entry->owner == owner && entry->start == start) return &entry->value;
    }
    if (store->count >= store->capacity / 2) {
        size_t capacity = store->capacity ? store->capacity * 2 : 128;
        if (capacity > SIZE_MAX / sizeof(ContextEntry *)) { fputs("ancestor context index overflow\n", stderr); abort(); }
        ContextEntry **buckets = arena_alloc(arena, capacity * sizeof *buckets); memset(buckets, 0, capacity * sizeof *buckets);
        for (size_t i = 0; i < store->capacity; i++) for (ContextEntry *entry = store->buckets[i], *next; entry; entry = next) {
            next = entry->next; size_t bucket = context_bucket(entry->owner, entry->start, capacity);
            entry->next = buckets[bucket]; buckets[bucket] = entry;
        }
        store->buckets = buckets; store->capacity = capacity;
    }
    ContextEntry *entry = arena_alloc(arena, sizeof *entry); *entry = (ContextEntry){.owner=owner, .start=start};
    ContextCapture capture = {&entry->value, arena};
    GInput input = {.src=records->source, .length=records->length}; input.range.end_byte = records->length;
    input.source_row = records->layout->source_row;
    size_t mark = scratch_mark(); SField *opened; uint32_t count;
    int valid = records->layout->g.scope_count ? scope_record_context(records, &input, &capture, &opened, &count) :
        record_context(records->layout, node, &input, capture_context, &capture, &opened, &count);
    if (!valid) {
        scratch_restore(mark); records->failed = 1; return NULL;
    }
    if (count) {
        entry->value.opened = arena_alloc(arena, (size_t)count * sizeof(SField)); entry->value.count = count;
        for (uint32_t i = 0; i < count; i++) entry->value.opened[i] = s_context_field_copy(arena, &opened[i]);
    }
    scratch_restore(mark);
    if (!s_context_stream_inherit(records->context_stream, &entry->value)) {
        fputs("context levels must increase through ancestor groups\n", stderr); records->failed = 1; return NULL;
    }
    size_t bucket = context_bucket(owner, start, store->capacity);
    entry->next = store->buckets[bucket]; store->buckets[bucket] = entry; store->count++;
    return &entry->value;
}

typedef struct ScopeStreamFrame {
    struct ScopeStreamFrame *parent, *next, *all;
    SayScratch *arena; size_t mark; SContext header, attributes;
    SField opening; const SContext *inherited;
    const uint8_t *name; uint32_t name_length;
    uint64_t ordinal, offset; uint8_t kind, emit;
    void *continuation;
} ScopeStreamFrame;
struct SScopeStream {
    SayScratch arena, headers; ScopeStreamFrame *top, *free, *all, *retired;
    uint8_t *prefix_seen; unsigned roots;
    uint64_t ordinal, offset, first_offset; size_t consumed; uint8_t final, begun;
};
SScopeStream *s_scope_stream_open(const Layout *layout){
    if (!layout->g.compiled || layout->g.unresolved || !layout->g.scope_count) return NULL;
    SScopeStream *stream = calloc(1, sizeof *stream); if (!stream) return NULL;
    stream->prefix_seen = arena_alloc(&stream->arena, layout->g.symbol_count);
    memset(stream->prefix_seen, 0, layout->g.symbol_count); return stream;
}
void s_scope_stream_close(SScopeStream *stream){
    if (!stream) return;
    arena_release(&stream->headers); arena_release(&stream->arena); free(stream);
}
static const SContext *scope_stream_context(const SScopeStream *stream){
    return stream->top ? (stream->top->emit ? &stream->top->attributes : stream->top->inherited) : NULL;
}
static ScopeStreamFrame *scope_stream_frame(SScopeStream *stream){
    ScopeStreamFrame *frame = stream->free;
    if (frame) stream->free = frame->next;
    else {
        frame = arena_alloc(&stream->arena, sizeof *frame); *frame = (ScopeStreamFrame){.all=stream->all}; stream->all = frame;
    }
    ScopeStreamFrame *all = frame->all;
    *frame = (ScopeStreamFrame){.arena=&stream->headers, .mark=stream->headers.used, .all=all,
        .parent=stream->top, .inherited=scope_stream_context(stream)};
    return frame;
}
static void scope_stream_recycle(SScopeStream *stream, ScopeStreamFrame *frame){
    stream->headers.used = frame->mark;
    frame->next = stream->free; stream->free = frame;
}
int s_scope_stream_window(SRecords *records, SScopeStream *stream, const Layout *layout,
    const uint8_t *source, size_t length, uint64_t offset, int final){
    if (!records_open(records, layout, source, length, !final, 0)) return final ? -1 : 0;
    TSNode root = ts_tree_root_node(records->tree);
    uint32_t last = 0, previous = 0;
    if (ts_tree_cursor_goto_first_child(records->cursor)) do {
        TSNode node = ts_tree_cursor_current_node(records->cursor);
        if (ts_node_is_named(node)) { previous = last; last = ts_node_end_byte(node); }
    } while (ts_tree_cursor_goto_next_sibling(records->cursor));
    uint32_t prefix = final ? (uint32_t)length : previous;
    if (!prefix && length) { s_records_close(records); return final ? -1 : 0; }
    records->limit = prefix; records->stream = stream; records->ordinal = stream->ordinal;
    if (!stream->begun) { stream->first_offset = offset; stream->begun = 1; }
    stream->offset = offset; stream->final = (uint8_t)final; stream->consumed = 0;
    ts_tree_cursor_reset(records->cursor, root); return 1;
}
size_t s_scope_stream_consumed(const SRecords *records){ return records->stream ? records->stream->consumed : 0; }
static int scope_stream_error(SRecords *records, TSNode node){
    fprintf(stderr, "invalid packaging scope at source byte %llu\n", (unsigned long long)(records->stream->offset + ts_node_start_byte(node)));
    records->failed = records->done = 1; return 0;
}
static int scope_stream_next(SRecords *records, SRecord *record){
    SScopeStream *stream = records->stream; const Layout *layout = records->layout; const GMap *grammar = &layout->g;
    if (records->done) return 0;
    /* A completion is a batch boundary: its readers finish before this frame
     * returns to the active-depth pool. No per-record allocation is needed. */
    if (stream->retired) { scope_stream_recycle(stream, stream->retired); stream->retired = NULL; }
    int advanced = records->started ? ts_tree_cursor_goto_next_sibling(records->cursor) : ts_tree_cursor_goto_first_child(records->cursor);
    records->started = 1;
    GInput input = {.src=records->source, .length=records->length}; input.range.end_byte = records->length; input.source_row = layout->source_row;
    GW worker = {.l=layout, .src=input.src, .input=&input};
    while (advanced) {
        TSNode node = ts_tree_cursor_current_node(records->cursor); uint8_t role = scope_role(grammar, node);
        if (ts_node_start_byte(node) >= records->limit) break;
        if (role == 252 || role == 253) {
            TSSymbol symbol = ts_node_symbol(node);
            if (stream->top || stream->roots || stream->prefix_seen[symbol] ||
                (role == 252 && stream->offset + ts_node_start_byte(node) != stream->first_offset)) return scope_stream_error(records, node);
            stream->prefix_seen[symbol] = 1;
        } else if ((role && role <= 8) || (role >= 17 && role <= 24)) {
            if (!stream->top && stream->roots) return scope_stream_error(records, node);
            int selected = record_matches(records, node);
            const GRule *rule = rule_at(&worker, node, 0, ts_node_symbol(ts_node_parent(node)));
            int header = !selected && rule && rule->what == G_GROUP && rule->ncontext;
            if (selected || role >= 17) {
                TSRange range;
                if (!scope_extent(grammar, node, &range) || range.end_byte > records->limit) {
                    if (stream->final) return scope_stream_error(records, node);
                    stream->consumed = ts_node_start_byte(node); records->done = 1; return 0;
                }
                if (!stream->top) stream->roots++;
                const SContext *context = scope_stream_context(stream);
                if (role <= 8) {
                    TSNode parent = ts_node_parent(node); ts_tree_cursor_reset(records->cursor, parent);
                    if (ts_tree_cursor_goto_first_child_for_byte(records->cursor, range.end_byte - 1) < 0) return scope_stream_error(records, node);
                }
                stream->consumed = range.end_byte;
                if (selected || header) {
                    *record = (SRecord){.node=node, .range=range, .ordinal=stream->ordinal++,
                        .source_offset=stream->offset + range.start_byte, .header=(uint8_t)header, .context=context};
                    records->ordinal = stream->ordinal; return 1;
                }
            } else {
                ScopeStreamFrame *frame = scope_stream_frame(stream); frame->kind = role; frame->emit = (uint8_t)header;
                frame->offset = stream->offset + ts_node_start_byte(node);
                TSNode identity;
                if (!at_path(node, &grammar->scopes[role - 1].identity, &identity)) return scope_stream_error(records, node);
                size_t mark = scratch_mark(); const uint8_t *name; uint32_t length;
                g_text(&worker, identity, 0, &name, &length);
                uint8_t *owned = arena_alloc(frame->arena, length); memcpy(owned, name, length); frame->name = owned; frame->name_length = length;
                scratch_restore(mark);
                if (!stream->top) stream->roots++;
                if (header) {
                    ContextCapture capture = {&frame->header, frame->arena};
                    ScopeOpening opening = {.visit=capture_context, .sink=&capture, .node=node};
                    mark = scratch_mark();
                    if (!cursor_project(layout, node, &input, scope_opening_visit, &opening, NULL, 0, 0, 0) || !(opening.action & S_VISIT_OK)) {
                        scratch_restore(mark); return scope_stream_error(records, node);
                    }
                    frame->opening = s_context_field_copy(frame->arena, &opening.field); scratch_restore(mark);
                    ContextCapture attributes = {&frame->attributes, frame->arena}; SField inherited;
                    mark = scratch_mark();
                    if (!scope_context_header(layout, rule, node, &input, &attributes, &inherited)) {
                        scratch_restore(mark); return scope_stream_error(records, node);
                    }
                    scratch_restore(mark); frame->attributes.parent = frame->inherited;
                    frame->attributes.opened = &frame->opening; frame->attributes.count = 1;
                    frame->ordinal = stream->ordinal++;
                }
                stream->top = frame; stream->consumed = ts_node_end_byte(node);
                if (header) {
                    *record = (SRecord){.node=node, .range=node_range(node), .ordinal=frame->ordinal, .source_offset=frame->offset,
                        .state=S_RECORD_RESERVE, .continuation=&frame->continuation, .context=frame->inherited, .projection=&frame->header};
                    records->ordinal = stream->ordinal; return 1;
                }
            }
        } else if (role >= 9 && role <= 16) {
            ScopeStreamFrame *frame = stream->top; TSNode identity;
            if (!frame || frame->kind != role - 8 || !at_path(node, &grammar->scopes[role - 9].identity, &identity)) return scope_stream_error(records, node);
            size_t mark = scratch_mark(); const uint8_t *name; uint32_t length;
            g_text(&worker, identity, 0, &name, &length);
            int matched = length == frame->name_length && !memcmp(name, frame->name, length); scratch_restore(mark);
            if (!matched) return scope_stream_error(records, node);
            stream->top = frame->parent; stream->consumed = ts_node_end_byte(node);
            if (frame->emit) {
                SField close = frame->opening; close.range.end_byte = ts_node_end_byte(node); close.range.end_point = ts_node_end_point(node);
                ContextCapture capture = {&frame->header, frame->arena}; capture_context(&capture, S_CLOSE, &close);
                stream->retired = frame;
                *record = (SRecord){.range=close.range, .ordinal=frame->ordinal, .source_offset=frame->offset,
                    .state=S_RECORD_COMPLETE, .header=1, .context=frame->inherited, .projection=&frame->header, .continuation=&frame->continuation};
                return 1;
            }
            scope_stream_recycle(stream, frame);
        } else if (!stream->top && role != 255) {
            if (role != 254) return scope_stream_error(records, node);
            size_t mark = scratch_mark(); const uint8_t *text; uint32_t length; g_text(&worker, node, 0, &text, &length);
            int valid = 1;
            for (uint32_t i = 0; i < length; i++) if (text[i] != ' ' && text[i] != '\t' && text[i] != '\n' && text[i] != '\r') { valid = 0; break; }
            scratch_restore(mark); if (!valid) return scope_stream_error(records, node);
        } else if (stream->top && stream->top->emit) {
            if (role == 254) {
                size_t mark = scratch_mark(); const uint8_t *text; uint32_t length; g_text(&worker, node, 0, &text, &length);
                int space = 1;
                for (uint32_t i = 0; i < length; i++) if (text[i] != ' ' && text[i] != '\t' && text[i] != '\n' && text[i] != '\r') { space = 0; break; }
                scratch_restore(mark);
                if (space) { advanced = ts_tree_cursor_goto_next_sibling(records->cursor); continue; }
            }
            ScopeStreamFrame *frame = stream->top; ContextCapture capture = {&frame->header, frame->arena};
            size_t mark = scratch_mark();
            if (!cursor_project(layout, node, &input, capture_context, &capture, frame->opening.name, frame->opening.name_length, 0, 0)) {
                scratch_restore(mark); return scope_stream_error(records, node);
            }
            scratch_restore(mark);
        }
        advanced = ts_tree_cursor_goto_next_sibling(records->cursor);
    }
    stream->consumed = records->limit; records->done = 1;
    if (stream->final && (stream->top || stream->roots != 1)) return scope_stream_error(records, ts_tree_root_node(records->tree));
    return 0;
}
