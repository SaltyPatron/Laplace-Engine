/* Compile packaging rules against native grammars, then project their cursors
 * into semantic fields. Only active ancestor frames live in scratch; completed
 * fields leave through the visitor. Injected parsers retain original byte ranges
 * while decoding the enclosing container's quoting and escapes. */
#define _GNU_SOURCE
#include "structure_internal.h"
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
    uint32_t *ordinals;
    uint8_t count_input, count_member, count_omitted;
} CursorFrame;

static int cursor_gap(const Layout *layout, const GInput *input, uint32_t *at, TSPoint *point,
                      uint32_t end, TSPoint end_point, s_visit_fn visit, void *sink){
    if (!layout->g.gaps || end <= *at) return 1;
    SField field = {.value=input->src+*at, .value_length=end-*at, .kind=S_TEXT,
        .flags=S_FIELD_JOIN, .grammar=layout->g.grammar_id,
        .range=source_range(input, (TSRange){*point,end_point,*at,end})};
    if (layout->g.resolve==G_HTML) {
        uint8_t *decoded=scratch_alloc((size_t)field.value_length*2+8);
        field.value_length=(uint32_t)html_unescape(field.value,field.value_length,decoded); field.value=decoded;
    }
    *at=end; *point=end_point;
    return (visit(sink,S_FIELD,&field)&S_VISIT_OK)!=0;
}
/* Part names are compiled at load time. Probe exact/prefix hash buckets and
 * preserve source-order precedence even when a wildcard overlaps an exact rule. */
static int cursor_scope_project(const Layout *layout, TSNode root, TSRange range, const GInput *input, s_visit_fn visit, void *sink);
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

int cursor_project(const Layout *layout, TSNode root, const GInput *input, s_visit_fn visit, void *sink,
                          const uint8_t *name, uint32_t name_length, int list, int flatten){
    GInput scoped=*input; if (!scoped.parts) scoped.parts=layout; input=&scoped;
    if (layout->g.scope_count && ts_node_is_null(ts_node_parent(root))) {
        if (!scope_validate(layout, root, input)) return 0;
        return cursor_scope_project(layout, root, node_range(root), input, visit, sink);
    }
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
    int valid = 1; uint32_t surface_at=ts_node_start_byte(root); TSPoint surface_point=ts_node_start_point(root);
    while (count) {
        CursorFrame *frame = &frames[count - 1];
        if (!cursor_gap(layout,input,&surface_at,&surface_point,ts_node_start_byte(frame->node),ts_node_start_point(frame->node),visit,sink)) {valid=0;break;}
        const GRule *rule = frame->route || frame->node.id == omit ? NULL : rule_at(&worker, frame->node, frame->field, frame->parent);
        if(!rule && count==1 && layout->g.record_fallback.what){
            rule=&layout->g.record_fallback;
        }
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
            int text_mode = rule->raw | ((rule->compiled.flags & 4u) ? 8 : 0) | (rule->what == G_TEXT && layout->g.padded ? 2 : 0) |
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
                if (!cursor_injection(&worker, payload, rule->compiled.inner, NULL, text_mode, field, visit, sink)) { valid = 0; break; }
            } else {
                int inherited_name=0;
                if (!frame->count_input) g_text(&worker, payload, text_mode, &field.value, &field.value_length);
                field.range = source_range(input, node_range(payload));
                if (rule->what == G_NOTE) {
                    size_t n = field.value_length; field.value = trimmed(field.value, &n); field.value_length = (uint32_t)n;
                } else if (field.name && !(rule->alias_tier && rule->alias_tier <= layout->ntier)) field.kind = S_VALUE;
                else if (!field.name && !frame->unnamed) { field.name = frame->context_name; field.name_length = frame->context_length; inherited_name=1; }
                if (rule->join) field.flags |= S_FIELD_JOIN;
                const SPart *part = field.name ? part_at(input->parts, field.name, field.name_length) : NULL;
                if (!part && field.name && input->parts!=layout) part=part_at(layout,field.name,field.name_length);
                /* List members inherit their enclosing field's semantic name.
                 * They are already products of its injection, not another
                 * encoded container. Explicit inner member names still chain. */
                if (inherited_name && part==input->active_part) part=NULL;
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
            if(rule->capture[0]){
                SField captured={.source=frame->node,.range=field.range,.name=(const uint8_t *)rule->capture,
                    .name_length=rule->capture_length,
                    .bindings=rule->capture_bindings,.grammar=layout->g.grammar_id,.kind=S_TEXT,.flags=S_FIELD_CAPTURE};
                g_text(&worker, frame->node, 1, &captured.value, &captured.value_length);
                if(!visit(sink,S_FIELD,&captured)){valid=0;break;}
            }
            if (rule->numbered[0]) {
                if (count<2) { valid=0; break; }
                CursorFrame *parent=&frames[count-2];
                if (!parent->ordinals) { parent->ordinals=scratch_stack_alloc((size_t)layout->g.n*sizeof *parent->ordinals); memset(parent->ordinals,0,(size_t)layout->g.n*sizeof *parent->ordinals); }
                uint32_t *ordinal=&parent->ordinals[rule-layout->g.rule];
                if (*ordinal==UINT32_MAX) { valid=0; break; }
                uint32_t value=++*ordinal; unsigned radix=rule->number_radix ? rule->number_radix : 10;
                uint8_t text[64]; size_t length=0;
                do { unsigned digit=value%radix; text[length++]=(uint8_t)(digit<10 ? '0'+digit : 'a'+digit-10); value/=radix; } while (value);
                while (length<rule->number_width) text[length++]='0';
                uint8_t *bytes=scratch_alloc(length); for (size_t i=0;i<length;i++) bytes[i]=text[length-i-1];
                SField number={.source=frame->node,.range=field.range,.name=(const uint8_t *)rule->numbered,
                    .name_length=rule->number_length,.value=bytes,.value_length=(uint32_t)length,
                    .bindings=rule->number_bindings,.grammar=layout->g.grammar_id,.kind=S_VALUE,.flags=S_FIELD_DERIVED};
                if (!visit(sink,S_FIELD,&number)) { valid=0; break; }
            }
            frame->context_name = field.name; frame->context_length = field.name_length; frame->context_list = (uint8_t)rule->list;
            if (!rule->capture[0] && !ts_node_named_child_count(frame->node)) {
                g_text(&worker, frame->node, 0, &field.value, &field.value_length); field.kind = S_TEXT;
                if (field.value_length && !visit(sink, S_FIELD, &field)) { valid = 0; break; }
            }
        }
after_rule:
        if (layout->g.gaps && !children) { surface_at=ts_node_end_byte(frame->node); surface_point=ts_node_end_point(frame->node); }
        if (children && ts_tree_cursor_goto_first_child(cursor)) {
            if (cursor_child(frame, cursor)) goto push_child;
            ts_tree_cursor_goto_parent(cursor);
        }
        for (;;) {
            frame = &frames[count - 1];
            if (frame->counts && frame->counts->remaining) { valid = 0; break; }
            if (frame->close_scope && count == 1 && layout->g.numbered_length && layout->ntier > 1) {
                char text[24]; size_t length = u64_digits(input->source_row + (uint64_t)ts_node_start_point(root).row + 1, text);
                uint8_t *value = scratch_alloc(length); memcpy(value, text, length);
                TSPoint point = ts_node_start_point(root); uint32_t byte = ts_node_start_byte(root);
                SField number = {.range={point, point, byte, byte}, .name=(const uint8_t *)layout->tier[0].numbered,
                    .value=value, .name_length=layout->g.numbered_length, .value_length=(uint32_t)length,
                    .grammar=layout->g.grammar_id, .kind=S_VALUE, .flags=S_FIELD_DERIVED};
                if (!visit(sink, S_FIELD, &number)) { valid = 0; break; }
            }
            if (!cursor_gap(layout,input,&surface_at,&surface_point,ts_node_end_byte(frame->node),ts_node_end_point(frame->node),visit,sink)) {valid=0;break;}
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

int scope_opening_visit(void *sink, int event, const SField *field){
    ScopeOpening *opening = sink;
    if (field->source.id == opening->node.id) {
        if (event == S_CLOSE) return S_VISIT_OK;
        if (event == S_OPEN) {
            opening->field = *field;
            if (opening->range.end_byte) opening->field.range = opening->range;
            opening->action = opening->visit(opening->sink, event, &opening->field);
            return opening->action | (opening->header_only ? S_VISIT_SKIP : 0);
        }
    }
    return opening->visit(opening->sink, event, field);
}
/* A lexical event grammar and an ordinary recursive grammar emit exactly the
 * same semantic scopes. Retain open fields only until their matching close;
 * attributes and leaves use the existing compiled cursor and injection path. */
static int cursor_scope_project(const Layout *layout, TSNode root, TSRange range, const GInput *input, s_visit_fn visit, void *sink){
    const GMap *grammar = &layout->g;
    TSNode parent = ts_node_parent(root); if (ts_node_is_null(parent)) parent = root;
    TSTreeCursor *cursor = lp_tls_cursor(parent); if (!cursor) return 0;
    size_t mark = tls_scratch.stack_used, capacity = 16, count = 0;
    SField *fields = scratch_stack_alloc(capacity * sizeof *fields); unsigned skipped = 0; int valid = 1;
    if (ts_tree_cursor_goto_first_child_for_byte(cursor, range.start_byte) >= 0) do {
        TSNode node = ts_tree_cursor_current_node(cursor);
        if (ts_node_start_byte(node) >= range.end_byte) break;
        uint8_t role = scope_role(grammar, node);
        if (role && role <= 8) {
            if (skipped) { skipped++; continue; }
            ScopeOpening opening = {.visit=visit, .sink=sink, .node=node};
            if (!scope_extent(grammar, node, &opening.range)) { valid = 0; break; }
            if (!cursor_project(layout, node, input, scope_opening_visit, &opening, NULL, 0, 0, 0) ||
                !(opening.action & S_VISIT_OK)) { valid = 0; break; }
            if (count == capacity) {
                if (capacity > SIZE_MAX / 2 / sizeof *fields) { valid = 0; break; }
                SField *grown = scratch_stack_alloc(capacity * 2 * sizeof *grown);
                memcpy(grown, fields, count * sizeof *grown); fields = grown; capacity *= 2;
            }
            fields[count++] = opening.field;
            if (opening.action & S_VISIT_SKIP) skipped = 1;
        } else if (role >= 9 && role <= 16) {
            if (skipped && --skipped) continue;
            if (!count || scope_role(grammar, fields[count - 1].source) != role - 8 ||
                !scope_equal(layout, role - 8, fields[count - 1].source, node, input)) { valid = 0; break; }
            SField field = fields[--count]; field.range.end_byte = ts_node_end_byte(node); field.range.end_point = ts_node_end_point(node);
            if (!close_field(visit, sink, &field)) { valid = 0; break; }
        } else if (!skipped) {
            const uint8_t *name = count ? fields[count - 1].name : NULL;
            uint32_t length = count ? fields[count - 1].name_length : 0;
            int list = count && (fields[count - 1].flags & S_FIELD_LIST);
            if (!cursor_project(layout, node, input, visit, sink, name, length, list, 0)) { valid = 0; break; }
        }
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    if (count || skipped) valid = 0;
    lp_tls_cursor_release(cursor); tls_scratch.stack_used = mark; return valid;
}

int context_visit(void *sink, int event, const SField *field){
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
int record_context(const Layout *layout, TSNode root, const GInput *input, s_visit_fn visit, void *sink,
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
                        .kind=S_GROUP, .flags=S_FIELD_CONTEXT, .context_level=rule->context_level};
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
static int cursor_record(const Layout *layout, TSNode root, TSRange range, int header, const SContext *inherited, const SContext *projection, const uint8_t *source, size_t length, s_visit_fn visit, void *sink){
    if (!layout->g.compiled || layout->g.unresolved || length > UINT32_MAX || !visit ||
        (!projection && (ts_node_is_null(root) || ts_node_has_error(root)))) return 0;
    GInput input = {0}; input.src = source; input.length = (uint32_t)length; input.range.end_byte = (uint32_t)length;
    input.source_row = layout->source_row;
    size_t mark = scratch_mark();
    SField *context = NULL; uint32_t contexts = 0; int valid = 1;
    if (inherited) {
        uint32_t depth = 0, total = 0;
        for (const SContext *part = inherited; part; part = part->parent) { depth++; total += part->count; }
        const SContext **chain = scratch_alloc((size_t)depth * sizeof *chain); uint32_t at = depth;
        for (const SContext *part = inherited; part; part = part->parent) chain[--at] = part;
        context = scratch_alloc((size_t)total * sizeof *context);
        for (uint32_t i = 0; i < depth && valid; i++) {
            const SContext *part = chain[i];
            for (const ContextEvent *event = part->first; event && valid; event = event->next) {
                valid = (visit(sink, event->event, &event->field) & S_VISIT_OK) != 0;
                if (event == part->last) break;
            }
            if (part->count) memcpy(context + contexts, part->opened, (size_t)part->count * sizeof *context);
            contexts += part->count;
        }
    } else if (!layout->g.scope_count) valid = record_context(layout, root, &input, visit, sink, &context, &contexts);
    HeaderVisitor visitor = {visit, sink, 0, 0};
    if (valid && projection) {
        for (const ContextEvent *event = projection->first; event && valid; event = event->next) {
            SField field = event->field; field.flags &= (uint16_t)~S_FIELD_CONTEXT;
            valid = (visit(sink, event->event, &field) & S_VISIT_OK) != 0;
            if (event == projection->last) break;
        }
        if (valid && header == 2 && projection->first) {
            SField close = projection->first->field; close.flags &= (uint16_t)~S_FIELD_CONTEXT;
            valid = (visit(sink, S_CLOSE, &close) & S_VISIT_OK) != 0;
        }
    } else if (valid) valid = layout->g.scope_count ?
        cursor_scope_project(layout, root, range, &input, header ? header_visit : visit, header ? &visitor : sink) :
        cursor_project(layout, root, &input, header ? header_visit : visit, header ? &visitor : sink, NULL, 0, 0, 0);
    while (valid && contexts) valid = (visit(sink, S_CLOSE, &context[--contexts]) & S_VISIT_OK) != 0;
    int retained = 0;
    if (valid) {
        SField end = {.source=root, .range=range, .arena_begin=mark, .grammar=layout->g.grammar_id};
        if (layout->g.record_children && layout->g.record_fallback.capture[0]) {
            end.value = source + range.start_byte; end.value_length = range.end_byte-range.start_byte;
            end.flags |= S_FIELD_CAPTURE;
        }
        int result = visit(sink, S_FINISH, &end);
        valid = (result & S_VISIT_OK) != 0; retained = valid && (result & S_VISIT_RETAIN);
    }
    if (!retained) scratch_restore(mark);
    return valid;
}
int s_cursor(const Layout *layout, TSNode root, const uint8_t *source, size_t length, s_visit_fn visit, void *sink){
    if (layout->g.scope_count) {
        GInput input = {.src=source, .length=(uint32_t)length}; input.range.end_byte = (uint32_t)length;
        if (length > UINT32_MAX || !scope_validate(layout, root, &input)) return 0;
    }
    return cursor_record(layout, root, node_range(root), 0, NULL, NULL, source, length, visit, sink);
}
int s_cursor_record(const Layout *layout, const SRecord *record, const uint8_t *source, size_t length, s_visit_fn visit, void *sink){
    return cursor_record(layout, record->node, record->range, record->state == S_RECORD_RESERVE ? 2 : record->header, record->context, record->projection, source, length, visit, sink);
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
