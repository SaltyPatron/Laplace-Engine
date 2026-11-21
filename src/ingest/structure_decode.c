/* Input source maps, container escape/quote decoders, injected sub-parsers, and lexical scope validation. */
#define _GNU_SOURCE
#include "structure_internal.h"
int index_input(GInput *input){
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
TSRange source_range(const GInput *input, TSRange range){
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
static int sql_byte_escape(const GDecodePlan *plan, unsigned depth, const uint8_t *src, uint32_t length, uint32_t *used, uint8_t *value){
    uint32_t at = *used;
    if (decode_more(plan, depth, src, length, &at).cp != '\\') return 0;
    int32_t tag = decode_more(plan, depth, src, length, &at).cp;
    unsigned result = 0, digits = 0, base = tag == 'x' ? 16u : 8u, limit = tag == 'x' ? 2u : 3u;
    if (tag >= '0' && tag <= '7') { result = (unsigned)(tag - '0'); digits = 1; }
    else if (tag != 'x') return 0;
    while (digits < limit && at < length) {
        uint32_t next_at = at; int d = hex_digit(decode_more(plan, depth, src, length, &next_at).cp);
        if (d < 0 || (unsigned)d >= base) break;
        result = result * base + (unsigned)d; at = next_at; digits++;
    }
    if (!digits) return 0;
    *used = at; *value = (uint8_t)result; return 1;
}
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
    if ((mode == G_SQL || mode == G_SQL_ESCAPE) && plan->quoted[depth - 1] && first.cp == plan->quoted[depth - 1]) {
        GUnit next = decode_more(plan, depth - 1, src, length, &at);
        return next.cp == first.cp ? g_unit(at, first.cp) : first;
    }
    if (mode == G_SQL_ESCAPE && first.cp == '\\') {
        uint8_t bytes[4]; uint32_t used = 0;
        if (sql_byte_escape(plan, depth - 1, src, length, &used, &bytes[0])) {
            unsigned count = bytes[0] < 0x80 ? 1u : bytes[0] >= 0xc2 && bytes[0] <= 0xdf ? 2u :
                bytes[0] >= 0xe0 && bytes[0] <= 0xef ? 3u : bytes[0] >= 0xf0 && bytes[0] <= 0xf4 ? 4u : 0u;
            if (!count) return g_unit(used, -1);
            for (unsigned i = 1; i < count; i++)
                if (!sql_byte_escape(plan, depth - 1, src, length, &used, &bytes[i]) || (bytes[i] & 0xc0) != 0x80) return g_unit(used, -1);
            size_t offset = 0; uint32_t cp;
            if (!lp_utf8_next(bytes, count, &offset, &cp) || offset != count || !scalar(cp) ||
                (count == 3 && cp < 0x800) || (count == 4 && cp < 0x10000)) return g_unit(used, -1);
            return g_unit(used, (int32_t)cp);
        }
    }
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
    if ((mode == G_JSON || mode == G_TURTLE || mode == G_SQL_ESCAPE) && first.cp == '\\') {
        GUnit next = decode_more(plan, depth - 1, src, length, &at);
        int32_t cp = next.cp;
        switch (cp) {
            case '"': case '\\': case '/': return g_unit(at, cp);
            case '\'': return g_unit(at, mode == G_TURTLE || mode == G_SQL_ESCAPE ? cp : -1);
            case '~': case '.': case '-': case '!': case '$': case '&': case '(':
            case ')': case '*': case '+': case ',': case ';': case '=': case '?':
            case '#': case '@': case '%': case '_': return g_unit(at, mode == G_TURTLE || mode == G_SQL_ESCAPE ? cp : -1);
            case 'b': return g_unit(at, '\b');
            case 'f': return g_unit(at, '\f');
            case 'n': return g_unit(at, '\n');
            case 'r': return g_unit(at, '\r');
            case 't': return g_unit(at, '\t');
            case 'u': case 'U': {
                unsigned digits = cp == 'u' ? 4 : 8;
                if (cp == 'U' && mode != G_TURTLE && mode != G_SQL_ESCAPE) return g_unit(at, -1);
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
            default: return g_unit(at, mode == G_SQL_ESCAPE ? cp : -1);
        }
    }
    return first;
}
static __thread const GDecodePlan *active_decode;
static uint32_t decode_input(const uint8_t *src, uint32_t length, int32_t *cp){
    GUnit unit = decode_unit(active_decode, active_decode->n, src, length); *cp = unit.cp; return unit.bytes;
}
const char *read_input(void *payload, uint32_t byte, TSPoint position, uint32_t *bytes){
    const GInput *input = payload; (void)position;
    if (byte >= input->range.end_byte) { *bytes = 0; return NULL; }
    *bytes = input->range.end_byte - byte;
    return (const char *)input->src + byte;
}
const uint8_t *decode_text(const GDecodePlan *plan, const uint8_t *src, uint32_t *length){
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
/* Strict curated admission never consumes a recovered syntax tree. Stop when every
 * parser stack has entered error recovery, rather than spending unbounded work
 * trying to repair a malformed record that this pipeline would reject anyway. */
bool reject_recovery(TSParseState *state){ return state->has_error; }
TSTree *parse_input_tree(const Layout *layout, const GInput *input){
    TSParser *parser = lp_tls_parser(layout->g.grammar_id);
    if (!parser || !ts_parser_set_included_ranges(parser, &input->range, 1)) return NULL;
    const GDecodePlan *saved = active_decode; active_decode = &input->decode;
    TSInput stream = {(void *)input, read_input, input->decode.n ? TSInputEncodingCustom : TSInputEncodingUTF8, decode_input};
    TSTree *parsed = ts_parser_parse_with_options(parser, NULL, stream, (TSParseOptions){.progress_callback=reject_recovery});
    active_decode = saved;
    ts_parser_set_included_ranges(parser, NULL, 0);
    if (parsed && ts_node_has_error(ts_tree_root_node(parsed))) { ts_tree_delete(parsed); return NULL; }
    return parsed;
}
int at_path(TSNode nd, const GPath *path, TSNode *out){
    TSNode cur = nd;
    for (uint8_t i = 0; i < path->n; i++) {
        const GPathStep *step = &path->step[i]; TSNode selected = {0};
        if (step->flags & 4) {
            for (TSNode ancestor = cur; !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor))
                if (ts_node_symbol(ancestor) == step->symbol) { selected = ancestor; break; }
        } else if (step->flags & 8) {
            TSTreeCursor *cursor = lp_tls_cursor(cur); if (!cursor) return 0;
            if (ts_tree_cursor_goto_first_child(cursor)) for (;;) {
                TSNode child = ts_tree_cursor_current_node(cursor);
                if (((step->flags & 1) && ts_tree_cursor_current_field_id(cursor) == step->field) ||
                    (!(step->flags & 1) && ts_node_symbol(child) == step->symbol)) { selected = child; break; }
                if (ts_tree_cursor_goto_first_child(cursor)) continue;
                while (!ts_tree_cursor_goto_next_sibling(cursor)) {
                    if (!ts_tree_cursor_goto_parent(cursor) || ts_node_eq(ts_tree_cursor_current_node(cursor), cur)) goto descendant_done;
                }
            }
descendant_done:
            lp_tls_cursor_release(cursor);
        } else if (step->flags & 1) selected = ts_node_child_by_field_id(cur, step->field);
        if (!(step->flags & 12) && ts_node_is_null(selected) && (step->flags & 2)) {
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
const GRule *rule_at(GW *w, TSNode nd, TSFieldId field, TSSymbol parent){
    TSSymbol sy = ts_node_symbol(nd);
    const GMap *g = &w->l->g;
    if (!g->sym_rule_offset || sy >= g->symbol_count) return NULL;
    const GRule *best = NULL; unsigned specificity = 0;
    for (uint32_t at = g->sym_rule_offset[sy]; at < g->sym_rule_offset[sy + 1]; at++) {
        const GRule *rule = &g->rule[g->sym_rules[at]];
        const CompiledRule *c = &rule->compiled;
        if ((c->field_id && c->field_id != field) || (c->parent_sym && c->parent_sym != parent)) continue;
        if (rule->condition_mode) {
            TSNode value; int found = at_path(nd, &c->condition, &value);
            if (rule->condition_mode == 2) { if (found) continue; }
            else {
                const uint8_t *text; uint32_t length;
                if (!found) continue;
                g_text(w, value, 0, &text, &length);
                if (length != rule->equals_length) continue;
                uint32_t i = 0;
                for (; i < length; i++) {
                    unsigned a = text[i], b = (uint8_t)rule->equals[i];
                    if (rule->casefold) { if (a >= 'A' && a <= 'Z') a += 'a' - 'A'; if (b >= 'A' && b <= 'Z') b += 'a' - 'A'; }
                    if (a != b) break;
                }
                if (i != length) continue;
            }
        }
        unsigned score = (c->field_id ? 2u : 0u) + (c->parent_sym ? 1u : 0u);
        if (rule->condition_mode) score += 4u;
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
void g_text(GW *w, TSNode nd, int raw, const uint8_t **p, uint32_t *n){
    int padded = raw & 2, trim = raw & 4; raw &= 1;
    uint32_t len = ts_node_end_byte(nd) - ts_node_start_byte(nd);
    const uint8_t *s = decode_text(&w->input->decode, w->src + ts_node_start_byte(nd), &len);
    if (!raw && w->l->g.resolve == G_HTML) {
        uint8_t *decoded = scratch_alloc((size_t)len*2 + 8);
        size_t length = html_unescape(s, len, decoded);
        if (length > UINT32_MAX) { fputs("HTML text exceeds parser byte range\n", stderr); exit(2); }
        *p=decoded; *n=(uint32_t)length; return;
    }
    if (padded) s = padding_off(s, &len, w->l->g.escaped);
    if (!raw && w->l->g.resolve == G_SQL && quoted_node(&w->l->g, nd)) {
        SqlLiteral literal;
        if (!sql_literal_bounds(s, len, &literal)) { fputs("invalid SQL literal boundaries\n", stderr); exit(2); }
        s += literal.begin; len = literal.end - literal.begin;
        if (literal.quote) {
            GDecodePlan plan = {.mode={literal.escaped ? G_SQL_ESCAPE : G_SQL}, .n=1, .quoted={literal.quote}};
            s = decode_text(&plan, s, &len);
        }
        *p = s; *n = len; return;
    }
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
int injected_input(GW *w, TSNode payload, int raw, GInput *result){
    int padded = raw & 2, trim = raw & 4; raw &= 1;
    GInput input = *w->input;
    input.range = (TSRange){ts_node_start_point(payload), ts_node_end_point(payload), ts_node_start_byte(payload), ts_node_end_byte(payload)};
    if ((input.decode.n || (!raw && w->l->g.resolve)) && !index_input(&input)) return 0;
    if (padded && !input_padding_off(&input, w->l->g.escaped)) return 0;
    if (!raw && w->l->g.resolve == G_SQL && quoted_node(&w->l->g, payload)) {
        uint32_t length = input.range.end_byte - input.range.start_byte;
        const uint8_t *text = decode_text(&input.decode, input.src + input.range.start_byte, &length);
        SqlLiteral literal; if (!sql_literal_bounds(text, length, &literal)) return 0;
        TSRange range = input.range; TSPoint point = range.start_point; uint32_t logical = 0;
        for (uint32_t at = range.start_byte;;) {
            if (logical == literal.begin) { input.range.start_byte = at; input.range.start_point = point; }
            if (logical == literal.end) { input.range.end_byte = at; input.range.end_point = point; break; }
            if (at == range.end_byte) return 0;
            GUnit unit = decode_unit(&input.decode, input.decode.n, input.src + at, range.end_byte - at);
            if (unit.cp < 0 || !unit.bytes) return 0;
            uint8_t encoded[4]; logical += (uint32_t)lp_utf8_put((uint32_t)unit.cp, encoded);
            at += unit.bytes; point = advance_point(point, unit);
        }
        if (literal.quote) {
            if (input.decode.n == G_DECODE_DEPTH) return 0;
            unsigned layer = input.decode.n++;
            input.decode.mode[layer] = literal.escaped ? G_SQL_ESCAPE : G_SQL;
            input.decode.quoted[layer] = literal.quote;
        }
        if (trim && !input_padding_off(&input, 0)) return 0;
        *result = input; return 1;
    }
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
uint8_t scope_role(const GMap *grammar, TSNode node){
    TSSymbol symbol = ts_node_symbol(node);
    return symbol < grammar->symbol_count ? grammar->scope_roles[symbol] : 0;
}
int scope_equal(const Layout *layout, uint8_t kind, TSNode open, TSNode close, const GInput *input){
    TSNode a, b;
    const GMap *grammar = &layout->g;
    if (!at_path(open, &grammar->scopes[kind - 1].identity, &a) ||
        !at_path(close, &grammar->scopes[kind - 1].identity, &b)) return 0;
    size_t mark = scratch_mark(); const uint8_t *left, *right; uint32_t nl, nr;
    GW worker = {.l=layout, .src=input->src, .input=input};
    g_text(&worker, a, 0, &left, &nl); g_text(&worker, b, 0, &right, &nr);
    int equal = nl == nr && !memcmp(left, right, nl); scratch_restore(mark); return equal;
}
void scope_push(SRecords *records, TSNode node, uint8_t kind){
    if (!records->scopes) {
        records->scopes = arena_alloc(&records->context_arena, sizeof *records->scopes);
        *records->scopes = (SScopeState){0};
    }
    SScopeState *state = records->scopes;
    if (state->count == state->capacity) {
        uint32_t capacity = state->capacity ? state->capacity * 2 : 16;
        if (capacity < state->capacity) { fputs("scope depth overflow\n", stderr); abort(); }
        ScopeFrame *frames = arena_alloc(&records->context_arena, (size_t)capacity * sizeof *frames);
        if (state->count) memcpy(frames, state->frames, (size_t)state->count * sizeof *frames);
        state->frames = frames; state->capacity = capacity;
    }
    state->frames[state->count++] = (ScopeFrame){node, kind};
}
/* Validate lexical events before exposing records. Name bytes are source data,
 * not runtime grammar dispatch. Only the active nesting chain is retained. */
int scope_validate(const Layout *layout, TSNode root, const GInput *input){
    const GMap *grammar = &layout->g;
    TSTreeCursor *cursor = lp_tls_cursor(root); if (!cursor) return 0;
    size_t mark = tls_scratch.stack_used, count = 0, capacity = 16; unsigned roots = 0;
    ScopeFrame *frames = scratch_stack_alloc(capacity * sizeof *frames); int valid = 1;
    if (ts_tree_cursor_goto_first_child(cursor)) do {
        TSNode node = ts_tree_cursor_current_node(cursor); if (!ts_node_is_named(node)) continue;
        uint8_t role = scope_role(grammar, node);
        if (role && role <= 8) {
            if (!count && ++roots != 1) { valid = 0; break; }
            if (count == capacity) {
                if (capacity > SIZE_MAX / 2 / sizeof *frames) { valid = 0; break; }
                ScopeFrame *grown = scratch_stack_alloc(capacity * 2 * sizeof *grown);
                memcpy(grown, frames, count * sizeof *grown); frames = grown; capacity *= 2;
            }
            frames[count++] = (ScopeFrame){node, role};
        } else if (role >= 9 && role <= 16) {
            if (!count || frames[count - 1].kind != role - 8 ||
                !scope_equal(layout, role - 8, frames[count - 1].node, node, input)) { valid = 0; break; }
            count--;
        } else if (role >= 17 && role <= 24) {
            if (!count && ++roots != 1) { valid = 0; break; }
        } else if (!count && role != 255 && role != 253 && role != 252) {
            if (role != 254) { valid = 0; break; }
            size_t value_mark = scratch_mark(); const uint8_t *text; uint32_t length;
            GW worker = {.l=layout, .src=input->src, .input=input}; g_text(&worker, node, 0, &text, &length);
            for (uint32_t i = 0; i < length; i++)
                if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') { valid = 0; break; }
            scratch_restore(value_mark);
            if (!valid) break;
        }
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    if (count || roots != 1) valid = 0;
    if (!valid) fprintf(stderr, "packaging scopes do not form one matched document near byte %u\n", ts_node_start_byte(ts_tree_cursor_current_node(cursor)));
    lp_tls_cursor_release(cursor); tls_scratch.stack_used = mark; return valid;
}
int scope_extent(const GMap *grammar, TSNode start, TSRange *range){
    *range = node_range(start); uint8_t role = scope_role(grammar, start);
    if (role >= 17 && role <= 24) return 1;
    TSTreeCursor *cursor = lp_tls_cursor(ts_node_parent(start)); if (!cursor) return 0;
    int depth = 0, valid = 0;
    if (ts_tree_cursor_goto_first_child_for_byte(cursor, range->start_byte) >= 0) do {
        TSNode node = ts_tree_cursor_current_node(cursor); role = scope_role(grammar, node);
        if (role && role <= 8) depth++;
        else if (role >= 9 && role <= 16 && !--depth) {
            range->end_byte = ts_node_end_byte(node); range->end_point = ts_node_end_point(node); valid = 1; break;
        }
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    lp_tls_cursor_release(cursor); return valid;
}

const SPart *part_at(const Layout *layout, const uint8_t *name, uint32_t length){
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
int cursor_injection(GW *worker, TSNode payload, const Layout *inner, const SPart *part, int raw, SField field,
                            s_visit_fn visit, void *sink){
    GInput input;
    if (!injected_input(worker, payload, raw, &input)) return 0;
    if (part) input.active_part=part;
    if (field.flags & S_FIELD_RECORD) {
        TSTree *tree = parse_input_tree(inner, &input);
        if (!tree) { fputs("record does not parse completely with its product grammar\n", stderr); return 0; }
        int valid = cursor_project(inner, ts_tree_root_node(tree), &input, visit, sink, NULL, 0, 0, 1);
        ts_tree_delete(tree); return valid;
    }
    /* A typed identifier still denotes the complete decoded field. Its inner
     * grammar exposes structure without discarding separators from identity. */
    g_text(worker,payload,raw,&field.value,&field.value_length);
    field.kind = S_GROUP; field.flags |= S_FIELD_LIST | S_FIELD_PAYLOAD;
    int action = visit(sink, S_OPEN, &field);
    if (!(action & S_VISIT_OK)) return 0;
    if (action & S_VISIT_SKIP) return close_field(visit, sink, &field);
    if (input.range.start_byte != input.range.end_byte || (raw & 8)) {
        TSTree *tree = parse_input_tree(inner, &input);
        if (!tree) {
            fprintf(stderr, "injected field %.*s at bytes %u..%u does not parse completely\n",
                    (int)field.name_length, field.name, input.range.start_byte, input.range.end_byte);
            return 0;
        }
        /* A grammar-declared list exposes its decoded members. Keeping the
         * encoded container as a scalar would attest JSON syntax as one value. */
        TSNode root=ts_tree_root_node(tree);
        GW nested={.l=inner,.src=input.src,.input=&input};
        const GRule *shape=rule_at(&nested,root,0,0);
        while (!shape && ts_node_named_child_count(root)==1) {
            root=ts_node_named_child(root,0); shape=rule_at(&nested,root,0,0);
        }
        if (shape && shape->what==G_GROUP && shape->list) field.flags &= (uint16_t)~S_FIELD_PAYLOAD;
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
    field.flags = (uint16_t)((field.flags & ~(S_FIELD_LIST | S_FIELD_PARTS | S_FIELD_JOIN | S_FIELD_TRIM)) | S_FIELD_SAME_TIER);
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
int cursor_parts(GW *worker, TSNode payload, const SPart *part, int raw, SField field,
                        s_visit_fn visit, void *sink){
    if (part->inner) return cursor_injection(worker, payload, part->inner, part, raw, field, visit, sink);
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
