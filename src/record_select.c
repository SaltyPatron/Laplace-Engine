/* Integer-slot field bindings, compiled selector execution and predicates.
 * No composition, witness accounting or source scheduling belongs here. */
#define _GNU_SOURCE
#include "record_select.h"
#include "file_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread RecordSelectionFrame selected;
RecordSelectionFrame record_selection_enter(const SemanticRecord *record, int16_t *memo, const Say *program, const BindingMatrix *bindings){
    RecordSelectionFrame previous=selected;
    selected=(RecordSelectionFrame){record,program,bindings,memo};
    return previous;
}
void record_selection_leave(const RecordSelectionFrame *previous){ selected=*previous; }

const PathCode *record_selector_code(const char *name){ return recipe_program_path(selected.program, name); }

int record_field_named(const SemanticValue *value, const char *name, size_t length){
    return value->nlen == length && (!length || !memcmp(value->name, name, length));
}

static size_t binding_bucket(uint64_t key, size_t capacity){
    key ^= key >> 30; key *= UINT64_C(0xbf58476d1ce4e5b9); key ^= key >> 27;
    return (key ^ (key >> 31)) & (capacity - 1);
}

const BindingBucket *record_binding_find(const BindingMatrix *matrix, uint32_t scope, uint32_t slot){
    if (!matrix || !matrix->capacity || scope == UINT32_MAX) return NULL;
    uint64_t key = ((uint64_t)scope + 1) << 32 | slot; size_t at = binding_bucket(key, matrix->capacity);
    while (matrix->buckets[at].key) {
        if (matrix->buckets[at].key == key) return &matrix->buckets[at];
        at = (at + 1) & (matrix->capacity - 1);
    }
    return NULL;
}

int32_t record_field_first(const SemanticValue *value){
    const SemanticRecord *record = value->owner;
    const BindingBucket *bucket = record_binding_find(record->bindings, (uint32_t)(value - record->n), BIND_INPUTS);
    return bucket ? (int32_t)record->bindings->links[bucket->first - 1].value : -1;
}

int32_t record_field_last(const SemanticValue *value){
    const SemanticRecord *record = value->owner;
    const BindingBucket *bucket = record_binding_find(record->bindings, (uint32_t)(value - record->n), BIND_INPUTS);
    return bucket ? (int32_t)record->bindings->links[bucket->last - 1].value : -1;
}

int32_t record_field_next(const SemanticValue *value){
    if (!value->operand) return -1;
    const BindingMatrix *matrix = value->owner->bindings;
    uint32_t next = matrix->links[value->operand - 1].next;
    return next ? (int32_t)matrix->links[next - 1].value : -1;
}

int32_t record_slot_first(const SemanticRecord *record, int32_t scope, uint32_t slot){
    if (scope < 0) return -1;
    const BindingBucket *bucket = record_binding_find(record->bindings, (uint32_t)scope, slot);
    return bucket ? (int32_t)record->bindings->links[bucket->first - 1].value : -1;
}

int32_t record_field_child(const SemanticRecord *record, int32_t scope, const char *name, int32_t after){
    const BindingBucket *bucket = record_binding_find(record->bindings, (uint32_t)scope, record_selector_code(name)->whole.slot);
    int ready = after < 0;
    for (uint32_t link = bucket ? bucket->first : 0; link; link = record->bindings->links[link - 1].next) {
        uint32_t value = record->bindings->links[link - 1].value;
        if (ready) return (int32_t)value;
        if (value == (uint32_t)after) ready = 1;
    }
    return -1;
}

static void bindings_grow(BindingMatrix *matrix){
    size_t capacity = matrix->capacity ? matrix->capacity * 2 : 128;
    if (capacity < matrix->capacity || capacity > SIZE_MAX / sizeof *matrix->buckets) { fputs("binding index exceeds the record arena\n", stderr); abort(); }
    BindingBucket *buckets = scratch_alloc(capacity * sizeof *buckets); memset(buckets, 0, capacity * sizeof *buckets);
    for (size_t i = 0; i < matrix->capacity; i++) if (matrix->buckets[i].key) {
        size_t at = binding_bucket(matrix->buckets[i].key, capacity);
        while (buckets[at].key) at = (at + 1) & (capacity - 1);
        buckets[at] = matrix->buckets[i];
    }
    matrix->buckets = buckets; matrix->capacity = capacity;
}

uint32_t record_binding_add(BindingMatrix *matrix, uint32_t scope, uint32_t slot, uint32_t value){
    if (scope == UINT32_MAX) { fputs("too many semantic scopes in one record\n", stderr); abort(); }
    if ((matrix->count + 1) * 2 >= matrix->capacity) bindings_grow(matrix);
    uint64_t key = ((uint64_t)scope + 1) << 32 | slot; size_t at = binding_bucket(key, matrix->capacity);
    while (matrix->buckets[at].key && matrix->buckets[at].key != key) at = (at + 1) & (matrix->capacity - 1);
    if (matrix->link_count == matrix->link_capacity) {
        size_t capacity = matrix->link_capacity ? matrix->link_capacity * 2 : 128;
        if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof *matrix->links) { fputs("too many record bindings\n", stderr); abort(); }
        BindingLink *links = scratch_alloc(capacity * sizeof *links);
        if (matrix->link_count) memcpy(links, matrix->links, matrix->link_count * sizeof *links);
        matrix->links = links; matrix->link_capacity = capacity;
    }
    uint32_t link = (uint32_t)++matrix->link_count; matrix->links[link - 1] = (BindingLink){value, 0};
    BindingBucket *bucket = &matrix->buckets[at];
    if (!bucket->key) { *bucket = (BindingBucket){key, link, link}; matrix->count++; }
    else { matrix->links[bucket->last - 1].next = link; bucket->last = link; }
    return link;
}

static int bind_name_matches(BindingMatrix *matrix, const PathProgram *program, uint32_t scope, uint32_t value,
                              const uint8_t *name, uint32_t length, int note, uint64_t hash, unsigned wildcard){
    int matches = 0;
    for (unsigned is_note = 0; is_note <= (unsigned)note; is_note++) {
        uint64_t key = disposition_key(hash, 0, is_note, wildcard, 0);
        size_t bucket = (key ^ (key >> 32)) & (program->name_bucket_count - 1);
        for (uint32_t at = program->name_buckets[bucket]; at; at = program->names[at - 1].next) {
            const NamePattern *pattern = &program->names[at - 1];
            if (pattern->hash == key && pattern->name.length == length && pattern->name.wildcard == wildcard &&
                pattern->name.note == is_note && (!length || !memcmp(pattern->name.bytes, name, length))) {
                if (matrix) record_binding_add(matrix, scope, at, value);
                matches++;
            }
        }
    }
    return matches;
}

static int bind_field_name(BindingMatrix *matrix, const PathProgram *program, uint32_t scope, uint32_t value,
                            const uint8_t *name, uint32_t length, int note){
    int matches = 0;
    uint64_t hash = NAME_HASH_START;
    if (program->has_positions) { if (matrix) record_binding_add(matrix, scope, 0, value); matches++; }
    if (program->wildcard_lengths[0] & 1) matches += bind_name_matches(matrix, program, scope, value, name, 0, note, hash, 1);
    uint32_t limit = length < program->max_name_length ? length : program->max_name_length;
    for (uint32_t i = 0; i < limit; i++) {
        hash = (hash ^ name[i]) * NAME_HASH_PRIME; uint32_t prefix = i + 1;
        if (prefix < 192 && (program->wildcard_lengths[prefix >> 6] & (UINT64_C(1) << (prefix & 63))))
            matches += bind_name_matches(matrix, program, scope, value, name, prefix, note, hash, 1);
    }
    if (length <= program->max_name_length) matches += bind_name_matches(matrix, program, scope, value, name, length, note, hash, 0);
    return matches;
}

int record_bind_field(BindingMatrix *matrix, const PathProgram *program, uint32_t scope, uint32_t value, const SField *field){
    const SNameBindings *compiled = field->kind == S_NOTE ? NULL : field->bindings;
    if (!compiled) return bind_field_name(matrix, program, scope, value, field->name, field->name_length, field->kind == S_NOTE);
    if (matrix) for (uint32_t i = 0; i < compiled->count; i++) record_binding_add(matrix, scope, compiled->slots[i], value);
    return (int)compiled->count;
}

int record_name_matches(const SemanticValue *node, const NameCode *name){
    if (name->note && node->kind != S_NOTE) return 0;
    if (name->wildcard ? node->nlen < name->length : node->nlen != name->length) return 0;
    return !name->length || !memcmp(node->name, name->bytes, name->length);
}

int record_field_matches(const SemanticValue *x, const char *name){
    const NameCode *code = &record_selector_code(name)->whole;
    return record_name_matches(x, code->slot ? &selected.program->paths->names[code->slot - 1].name : code);
}

static void disposition_candidates(const DisProgram *program, uint64_t hash, uint64_t parent, unsigned note,
                                   unsigned wildcard, unsigned qualified, uint64_t candidates[2][4]){
    uint64_t key = disposition_key(hash, parent, note, wildcard, qualified);
    size_t bucket = (key ^ (key >> 32)) & (program->capacity - 1);
    for (uint16_t at = program->buckets[bucket]; at; at = program->entries[at - 1].next) {
        const DisEntry *entry = &program->entries[at - 1];
        if (entry->key == key && entry->qualified == qualified)
            candidates[qualified][entry->rule >> 6] |= UINT64_C(1) << (entry->rule & 63);
    }
}

static void disposition_lookup(const DisProgram *program, const SemanticValue *node, uint64_t parent_hash, int has_parent,
                               uint64_t hash, unsigned wildcard, uint64_t candidates[2][4]){
    for (unsigned note = 0; note <= (unsigned)(node->kind == S_NOTE); note++) {
        disposition_candidates(program, hash, 0, note, wildcard, 0, candidates);
        if (has_parent) disposition_candidates(program, hash, parent_hash, note, wildcard, 1, candidates);
    }
}

static const Dis *dispatch_dis(const Say *say, const Dis *rules, int count, const SemanticValue *node, int resolve_conditions){
    if (!count) return NULL;
    const DisProgram *program = rules == say->dis ? say->dis_program : say->value_program;
    if (!program) { fputs("dispositions were not compiled at recipe load\n", stderr); abort(); }
    int32_t root = selected.record && node >= selected.record->n && node < selected.record->n + selected.record->count && node->parent >= 0 ? node->parent : 0;
    const SemanticValue *parent = NULL;
    if (selected.record && node->parent >= 0) {
        parent = &selected.record->n[node->parent];
        if (parent->nlen == node->nlen && (!node->nlen || !memcmp(parent->name, node->name, node->nlen)) && parent->parent >= 0) {
            parent = &selected.record->n[parent->parent]; root = (int32_t)(parent - selected.record->n);
        }
        if (parent->nlen > 63) parent = NULL;
    }
    uint64_t candidates[2][4] = {{0}}, hash = NAME_HASH_START;
    uint64_t parent_hash = parent ? selector_hash(parent->name, parent->nlen) : 0;
    if (program->wildcard_lengths & 1) disposition_lookup(program, node, parent_hash, parent != NULL, hash, 1, candidates);
    uint32_t length = node->nlen < 63 ? node->nlen : 63;
    for (uint32_t i = 0; i < length; i++) {
        hash = (hash ^ node->name[i]) * NAME_HASH_PRIME;
        if (program->wildcard_lengths & (UINT64_C(1) << (i + 1))) disposition_lookup(program, node, parent_hash, parent != NULL, hash, 1, candidates);
    }
    if (node->nlen <= 63) disposition_lookup(program, node, parent_hash, parent != NULL, hash, 0, candidates);
    for (int qualified = 1; qualified >= 0; qualified--) for (unsigned word = 0; word < 4; word++) {
        uint64_t bits = candidates[qualified][word];
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll(bits), index = word * 64 + bit; bits &= bits - 1;
            const Dis *rule = &rules[index];
            if (qualified) {
                if (!parent || !rule->parent_slot || !record_name_matches(parent, &say->paths->names[rule->parent_slot - 1].name) ||
                    !record_name_matches(node, &say->paths->names[rule->part_slot - 1].name)) continue;
            } else if (!record_name_matches(node, &say->paths->names[rule->name_slot - 1].name)) continue;
            if (rule->when) {
                if (!resolve_conditions) return NULL;
                if (!selected.record || !record_conditions_match(say, selected.record, root, rule->when)) continue;
            }
            return rule;
        }
    }
    return NULL;
}

static const Dis *dis_in(const Say *say, const Dis *rules, int count, const SemanticValue *node){ return dispatch_dis(say, rules, count, node, 1); }

static const Dis *match_dis(const Say *s, const SemanticValue *x){ return dis_in(s, s->dis, s->ndis, x); }

const Dis *record_value_meaning(const Say *s, const SemanticValue *x){ return s->nval && x->nlen ? dis_in(s, s->val, s->nval, x) : NULL; }

const Dis *record_field_disposition(const Say *s, const SemanticValue *x){
    if (!selected.dispositions || !selected.record || x < selected.record->n || x >= selected.record->n + selected.record->count) return match_dis(s, x);
    size_t i = (size_t)(x - selected.record->n); int16_t m = __atomic_load_n(&selected.dispositions[i], __ATOMIC_RELAXED); if (m) return m > 0 ? &s->dis[m - 1] : NULL;
    const Dis *d = match_dis(s, x); __atomic_store_n(&selected.dispositions[i], (int16_t)(d ? d - s->dis + 1 : -1), __ATOMIC_RELAXED); return d;
}

static int path_filter(const SemanticRecord *tree, int32_t node, const PathStep *step){
    if (!step->filter) return 1;
    int32_t value = record_path_first(tree, node, step->filter, NULL);
    if (value >= 0 && tree->n[value].kind == S_GROUP && record_field_first(&tree->n[value]) >= 0 &&
        record_field_next(&tree->n[record_field_first(&tree->n[value])]) < 0) value = record_field_first(&tree->n[value]);
    return value >= 0 && tree->n[value].vlen == step->filter_length &&
           (!step->filter_length || !memcmp(tree->n[value].val, step->filter_value, step->filter_length));
}

static int32_t path_single(const SemanticRecord *tree, int32_t root, const PathStep *step){
    if (step->operation == PATH_SELF) return root;
    if (step->operation == PATH_PARENT) return tree->n[root].parent;
    if (step->operation == PATH_ANCESTOR) {
        for (int32_t up = tree->n[root].parent; up >= 0; up = tree->n[up].parent)
            if (record_name_matches(&tree->n[up], &step->name)) return up;
        return -1;
    }
    if (!selected.bindings) { fputs("semantic bindings were not prepared before selector execution\n", stderr); abort(); }
    const BindingBucket *bucket = record_binding_find(selected.bindings, (uint32_t)root, step->name.slot);
    for (uint32_t link = bucket ? bucket->first : 0; link; link = selected.bindings->links[link - 1].next) {
        int32_t child = (int32_t)selected.bindings->links[link - 1].value;
        if (path_filter(tree, child, step)) return child;
    }
    if (step->operation == PATH_POSITION && step->position) {
        uint32_t position = step->position; bucket = record_binding_find(selected.bindings, (uint32_t)root, 0);
        for (uint32_t link = bucket ? bucket->first : 0; link; link = selected.bindings->links[link - 1].next)
            if (!--position) return (int32_t)selected.bindings->links[link - 1].value;
    }
    return -1;
}

int32_t record_path_first(const SemanticRecord *tree, int32_t root, const PathStep *step, const PathStep *stop){
    while (root >= 0 && step && step != stop) {
        int32_t next = path_single(tree, root, step);
        if (next < 0 && step->alternative) return record_path_first(tree, root, step->alternative, stop);
        root = next; step = step->next;
    }
    return root;
}

static int path_all(const SemanticRecord *tree, int32_t root, const PathStep *step, const PathStep *stop, int32_t *out, int capacity){
    if (root < 0 || capacity <= 0) return 0;
    if (!step || step == stop) { out[0] = root; return 1; }
    if (step->operation == PATH_PARENT || step->operation == PATH_ANCESTOR || step->operation == PATH_SELF)
        return path_all(tree, path_single(tree, root, step), step->next, stop, out, capacity);
    int count = 0, named = 0;
    if (!selected.bindings) { fputs("semantic bindings were not prepared before path enumeration\n", stderr); abort(); }
    const BindingBucket *bucket = record_binding_find(selected.bindings, (uint32_t)root, step->name.slot);
    for (uint32_t link = bucket ? bucket->first : 0; link && count < capacity; link = selected.bindings->links[link - 1].next) {
        int32_t child = (int32_t)selected.bindings->links[link - 1].value;
        if (!path_filter(tree, child, step)) continue;
        named = 1; count += path_all(tree, child, step->next, stop, out + count, capacity - count);
    }
    if (!named && step->operation == PATH_POSITION)
        count = path_all(tree, path_single(tree, root, step), step->next, stop, out, capacity);
    if (!count && step->alternative) return path_all(tree, root, step->alternative, stop, out, capacity);
    return count;
}

int32_t record_select_first(const SemanticRecord *tree, int32_t root, const char *name){
    return record_path_first(tree, root, record_selector_code(name)->first, NULL);
}

int record_select_all(const SemanticRecord *tree, int32_t root, const char *name, int32_t *out, int capacity){
    return path_all(tree, root, record_selector_code(name)->first, NULL, out, capacity);
}

int record_conditions_match(const Say *s, const SemanticRecord *t, int32_t root, uint32_t which){
    for (int i = 0; i < s->nwhere; i++) { if (!(which & (1u << i))) continue; const Where *w = &s->where[i]; int32_t c = record_select_first(t, root, w->name);
        if (c >= 0 && t->n[c].kind == S_GROUP && record_field_first(&t->n[c]) >= 0 && record_field_next(&t->n[record_field_first(&t->n[c])]) < 0 && t->n[record_field_first(&t->n[c])].kind != S_GROUP) c = record_field_first(&t->n[c]);     /* a part that holds only its text */
        const uint8_t *v = c >= 0 ? t->n[c].val : (const uint8_t *)""; size_t n = c >= 0 ? t->n[c].vlen : 0;
        if (w->op == 2) { size_t mark = scratch_mark(); char stack[512], *z = n < sizeof stack ? stack : scratch_alloc(n + 1); memcpy(z, v, n); z[n] = 0; int hit = !regexec(&w->re, z, 0, NULL, 0); scratch_restore(mark); if (!hit) return 0; }
        else { int same = n == strlen(w->val) && !memcmp(v, w->val, n); if (same == (w->op == 1)) return 0; } }
    return 1;
}

FileMetadataPolicy recipe_file_metadata_policy(const Recipe *recipe, const char *name){
    const Say *s = recipe ? recipe->say : NULL;
    if (!s) return FILE_METADATA_UNSET; SemanticValue x; memset(&x, 0, sizeof x); x.kind = S_VALUE; x.name = (const uint8_t *)name; x.nlen = (uint32_t)strlen(name); x.parent = -1;
    RecordSelectionFrame prior = record_selection_enter(NULL, NULL, s, NULL); const Dis *d = match_dis(s, &x); record_selection_leave(&prior);
    /* A corpus-wide content default describes its product fields. It must not
     * erase the stock OS metadata that identifies the unopened file. Explicit
     * metadata/omit wildcards still configure the packaging record itself. */
    if (!d || (d->whole.wild && d->what != D_METADATA && d->what != D_OMIT)) return FILE_METADATA_UNSET;
    return d->what == D_METADATA ? FILE_METADATA_KEEP : d->what == D_OMIT ? FILE_METADATA_OMIT : FILE_METADATA_CONTENT;
}

const Dis *record_unconditional_disposition(const Say *program, const SemanticValue *value){ return dispatch_dis(program,program->dis,program->ndis,value,0); }
