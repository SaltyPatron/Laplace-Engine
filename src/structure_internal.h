#ifndef LAPLACE_STRUCTURE_INTERNAL_H
#define LAPLACE_STRUCTURE_INTERNAL_H

#include "engine.h"
#include "structure.h"
#include "context_stream.h"
#include "os.h"
#include "laplace_config.h"
#include "literal_sql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <blake3.h>
#include <tree_sitter/api.h>

extern __thread SayScratch tls_scratch;
void *scratch_stack_alloc(size_t bytes);

static inline void str_copy(char *dst, size_t cap, const char *src, size_t len){
    if (!cap) return;
    if (len >= cap) len = cap - 1;
    if (len) memcpy(dst, src, len);
    dst[len] = 0;
}
static inline void str_set(char *dst, size_t cap, const char *src){
    str_copy(dst, cap, src ? src : "", src ? strlen(src) : 0);
}
static inline size_t u64_digits(uint64_t v, char out[24]){
    char tmp[24]; size_t n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}
static inline uint64_t part_hash(const uint8_t *bytes, size_t length){
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}
static inline const uint8_t *trimmed(const uint8_t *p, size_t *n){
    while (*n && (p[0] == ' ' || p[0] == '\t')) { p++; (*n)--; }
    while (*n && (p[*n - 1] == ' ' || p[*n - 1] == '\t' || p[*n - 1] == '\r')) (*n)--;
    return p;
}
static inline TSRange node_range(TSNode node){
    return (TSRange){ts_node_start_point(node), ts_node_end_point(node), ts_node_start_byte(node), ts_node_end_byte(node)};
}
static inline int close_field(s_visit_fn visit, void *sink, const SField *field){
    int result = visit(sink, S_CLOSE, field);
    if (!(result & S_VISIT_OK)) return 0;
    if (result & S_VISIT_RELEASE) scratch_restore(field->arena_begin);
    return 1;
}

int use_format(Layout *layout, const char *name);
int map_count_columns(GMap *grammar, int pending_header);

size_t lp_utf8_put(uint32_t cp, uint8_t out[4]);
size_t xml_unescape(const uint8_t *s, size_t n, uint8_t *o);

#define G_DECODE_DEPTH 8
typedef struct { uint8_t mode[G_DECODE_DEPTH], n, escaped[G_DECODE_DEPTH], quoted[G_DECODE_DEPTH]; } GDecodePlan;
typedef struct {
    const uint8_t *src;
    const Layout *parts;
    const SPart *active_part;
    uint32_t length;
    TSRange range;
    GDecodePlan decode;
    uint32_t physical_base, physical_end, *physical_lines, physical_line_count;
    TSPoint physical_origin;
    uint64_t source_row;
    uint8_t physical_indexed;
} GInput;
typedef struct { const Layout *l; const uint8_t *src; const GInput *input; } GW;
typedef struct { TSNode node; uint8_t kind; } ScopeFrame;
struct SScopeState { ScopeFrame *frames; uint32_t count, capacity; };
typedef struct { s_visit_fn visit; void *sink; TSNode node; TSRange range; SField field; int action; uint8_t header_only; } ScopeOpening;
typedef struct { s_visit_fn visit; void *sink; } ContextVisitor;

int index_input(GInput *input);
TSRange source_range(const GInput *input, TSRange range);
const char *read_input(void *payload, uint32_t byte, TSPoint position, uint32_t *bytes);
const uint8_t *decode_text(const GDecodePlan *plan, const uint8_t *src, uint32_t *length);
bool reject_recovery(TSParseState *state);
TSTree *parse_input_tree(const Layout *layout, const GInput *input);
int at_path(TSNode nd, const GPath *path, TSNode *out);
const GRule *rule_at(GW *w, TSNode nd, TSFieldId field, TSSymbol parent);
void g_text(GW *w, TSNode nd, int raw, const uint8_t **p, uint32_t *n);
int injected_input(GW *w, TSNode payload, int raw, GInput *result);
uint8_t scope_role(const GMap *grammar, TSNode node);
int scope_equal(const Layout *layout, uint8_t kind, TSNode open, TSNode close, const GInput *input);
void scope_push(SRecords *records, TSNode node, uint8_t kind);
int scope_validate(const Layout *layout, TSNode root, const GInput *input);
int scope_extent(const GMap *grammar, TSNode start, TSRange *range);
const SPart *part_at(const Layout *layout, const uint8_t *name, uint32_t length);
int cursor_injection(GW *worker, TSNode payload, const Layout *inner, const SPart *part, int raw, SField field,
                     s_visit_fn visit, void *sink);
int cursor_parts(GW *worker, TSNode payload, const SPart *part, int raw, SField field,
                 s_visit_fn visit, void *sink);

int cursor_project(const Layout *layout, TSNode root, const GInput *input, s_visit_fn visit, void *sink,
                   const uint8_t *name, uint32_t name_length, int list, int flatten);
int scope_opening_visit(void *sink, int event, const SField *field);
int context_visit(void *sink, int event, const SField *field);
int record_context(const Layout *layout, TSNode root, const GInput *input, s_visit_fn visit, void *sink,
                   SField **opened, uint32_t *count);

#endif
