/* Arena-backed compiled operands collected from packaging grammar events.
 * This module binds fields and owns their lifetime; evaluation is delegated
 * through record_execute after the complete record or declaration is ready. */
#define _GNU_SOURCE
#include "record_select.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t semantic_variable(SemanticRecord *record, const PathProgram *program, int32_t parent, const SField *field){
    if (record->count == UINT32_MAX) { fputs("too many semantic variables in a record\n", stderr); abort(); }
    if (record->count == record->capacity) {
        uint32_t capacity = record->capacity ? record->capacity * 2 : 64;
        if (capacity < record->capacity) { fputs("semantic record exceeds its arena\n", stderr); abort(); }
        SemanticValue *values = scratch_alloc((size_t)capacity * sizeof *values);
        if (record->count) memcpy(values, record->n, (size_t)record->count * sizeof *values);
        record->n = values; record->capacity = capacity;
    }
    uint32_t id = record->count++;
    unsigned tier = field->flags & S_FIELD_TIER ? field->tier : parent >= 0 ?
        record->n[parent].tier + ((field->flags & S_FIELD_SAME_TIER) ? 0 : 1) : 0;
    record->n[id] = (SemanticValue){.owner=record, .name=field->name, .val=field->value,
        .nlen=field->name_length, .vlen=field->value_length, .extent=id, .parent=parent,
        .kind=field->kind, .tier=(uint8_t)(tier < 255 ? tier : 255),
        .join=(uint8_t)((field->flags & S_FIELD_LIST) ? S_PIECES : (field->flags & S_FIELD_JOIN) != 0),
        .trim=(uint8_t)((field->flags & S_FIELD_TRIM) != 0), .context=(uint8_t)((field->flags & S_FIELD_CONTEXT) != 0)};
    if (parent >= 0) {
        record->n[id].operand = record_binding_add(record->bindings, (uint32_t)parent, BIND_INPUTS, id);
        record->n[parent].arity++;
        record_bind_field(record->bindings, program, (uint32_t)parent, id, field);
    }
    return id;
}

static void dropped_rehash(SemanticRecord *record){
    uint32_t capacity = record->dropped_bucket_count ? record->dropped_bucket_count * 2 : 128;
    if (capacity < record->dropped_bucket_count) { fputs("diagnostic index exceeds its arena\n", stderr); abort(); }
    uint32_t *buckets = scratch_alloc((size_t)capacity * sizeof *buckets); memset(buckets, 0, (size_t)capacity * sizeof *buckets);
    for (uint32_t i = 0; i < record->dropped_count; i++) {
        DroppedField *field = &record->dropped[i]; size_t at = (field->hash ^ (field->hash >> 32)) & (capacity - 1);
        field->next = buckets[at]; buckets[at] = i + 1;
    }
    record->dropped_buckets = buckets; record->dropped_bucket_count = capacity;
}

static void drop_field(SemanticRecord *record, int32_t parent, const SField *source, const Layout *layout){
    if (parent < 0) { fputs("an unbound field has no semantic diagnostic scope\n", stderr); abort(); }
    if (!record->dropped_bucket_count || (record->dropped_count + 1) * 2 >= record->dropped_bucket_count) dropped_rehash(record);
    uint64_t hash = selector_hash(source->name, source->name_length) ^ ((uint64_t)(uint32_t)parent * UINT64_C(0x9e3779b97f4a7c15));
    size_t bucket = (hash ^ (hash >> 32)) & (record->dropped_bucket_count - 1);
    for (uint32_t at = record->dropped_buckets[bucket]; at; at = record->dropped[at - 1].next) {
        DroppedField *field = &record->dropped[at - 1];
        if (field->hash == hash && field->parent == (uint32_t)parent && field->length == source->name_length &&
            (!field->length || !memcmp(field->name, source->name, field->length))) {
            if (field->all == UINT32_MAX) { fputs("diagnostic count exceeds one record\n", stderr); abort(); }
            field->all++; field->nonempty += !s_empty(layout, source->value, source->value_length); return;
        }
    }
    if (record->dropped_count == record->dropped_capacity) {
        uint32_t capacity = record->dropped_capacity ? record->dropped_capacity * 2 : 32;
        if (capacity < record->dropped_capacity) { fputs("record diagnostics exceed the arena\n", stderr); abort(); }
        DroppedField *fields = scratch_alloc((size_t)capacity * sizeof *fields);
        if (record->dropped_count) memcpy(fields, record->dropped, (size_t)record->dropped_count * sizeof *fields);
        record->dropped = fields; record->dropped_capacity = capacity;
    }
    uint32_t id = record->dropped_count++;
    record->dropped[id] = (DroppedField){.name=source->name, .hash=hash, .length=source->name_length, .parent=(uint32_t)parent,
        .all=1, .nonempty=(uint32_t)!s_empty(layout, source->value, source->value_length),
        .next=record->dropped_buckets[bucket], .scope_next=record->n[parent].dropped_first};
    record->dropped_buckets[bucket] = id + 1; record->n[parent].dropped_first = id + 1;
}

static void text_fragment(TextRun *run, const uint8_t *bytes, uint32_t length){
    if (length > UINT32_MAX - run->length) { fputs("joined text exceeds the grammar byte limit\n", stderr); abort(); }
    TextFragment *fragment = scratch_alloc(sizeof *fragment); *fragment = (TextFragment){bytes, length, NULL};
    if (run->last) run->last->next = fragment; else run->first = fragment;
    run->last = fragment; run->length += length;
}

static void join_text(SemanticValue *value, const SField *field){
    if (!value->text) {
        value->text = scratch_alloc(sizeof *value->text); *value->text = (TextRun){0};
        text_fragment(value->text, value->val, value->vlen);
    }
    text_fragment(value->text, field->value, field->value_length);
    value->trim |= (uint8_t)((field->flags & S_FIELD_TRIM) != 0);
}

static void finish_text(RecordWorker *sink, SemanticValue *value){
    (void)sink;
    if (value->kind == S_GROUP) return;
    if (value->text) {
        uint8_t *text = scratch_alloc(value->text->length); uint32_t used = 0;
        for (const TextFragment *part = value->text->first; part; part = part->next) {
            if (part->length) memcpy(text + used, part->bytes, part->length);
            used += part->length;
        }
        value->val = text; value->vlen = used; value->text = NULL;
    }
    if (value->trim) {
        while (value->vlen && (value->val[0] == ' ' || value->val[0] == '\t')) { value->val++; value->vlen--; }
        while (value->vlen && (value->val[value->vlen - 1] == ' ' || value->val[value->vlen - 1] == '\t' || value->val[value->vlen - 1] == '\r')) value->vlen--;
    }
    value->trim = 0;

}

void record_prepared_clear(PreparedRecord *record){
    for (uint32_t i = 0; i < record->record.dropped_count; i++) record->record.dropped[i].seen = 0;
}

void record_bind_keys(RecordWorker *sink, PreparedRecord *record, int final){
    sink->kr = record->rank; sink->bindings = record->bindings;
    record_execute(sink, &record->record, 0, record->ordinal, record->reserve || sink->reserve_declarations ? 3 : final ? 2 : 1);
    /* Diagnostic marks belong to this evaluation, not later barrier rounds. */
    record_prepared_clear(record);
}

void record_bind_declarations(RecordWorker *sink, PreparedRecord *record){ record_bind_keys(sink, record, 0); }

static void collector_root(RecordCollector *collector){
    if (collector->record.count) return;
    SField root = {.kind=S_GROUP};
    if (collector->layout->ntier) { root.name = (const uint8_t *)collector->layout->tier[0].name; root.name_length = collector->layout->g.root_name_length; }
    collector->record.bindings = &collector->sink->bindings;
    semantic_variable(&collector->record, collector->sink->s->paths, -1, &root);
}

int record_collect_fields(void *sink, int event, const SField *field){
    RecordCollector *collector = sink; RecordWorker *output = collector->sink; collector_root(collector);
    SemanticRecord *record = &collector->record;
    if (field->flags & S_FIELD_CONTEXT) {
        if (event == S_CLOSE) {
            if (!collector->context_depth) return 0;
            uint32_t scope = collector->context_scopes[--collector->context_depth].scope;
            record->n[scope].extent = record->count - 1;
            RecordSelectionFrame previous = record_selection_enter(record, NULL, output->s, record->bindings);
            for (int32_t value = record_field_first(&record->n[scope]); value >= 0; value = record_field_next(&record->n[value])) finish_text(output, &record->n[value]);
            record_selection_leave(&previous); return S_VISIT_OK;
        }
        int32_t parent = collector->context_depth ? (int32_t)collector->context_scopes[collector->context_depth - 1].scope : -1;
        if (event == S_FIELD && field->name_length &&
            !record_bind_field(NULL, output->s->paths, (uint32_t)parent, 0, field)) return S_VISIT_OK;
        uint32_t variable = semantic_variable(record, output->s->paths, parent, field);
        if (event == S_OPEN) {
            if (collector->context_depth == collector->context_capacity) {
                uint32_t capacity = collector->context_capacity ? collector->context_capacity * 2 : 8;
                if (capacity < collector->context_capacity) return 0;
                SemanticScope *scopes = scratch_alloc((size_t)capacity * sizeof *scopes);
                if (collector->context_depth) memcpy(scopes, collector->context_scopes, (size_t)collector->context_depth * sizeof *scopes);
                collector->context_scopes = scopes; collector->context_capacity = capacity;
            }
            collector->context_scopes[collector->context_depth++] = (SemanticScope){variable};
        }
        return S_VISIT_OK;
    }
    if (!collector->parent_fixed && event != S_FINISH) {
        if (collector->context_depth) record->n[0].parent = (int32_t)collector->context_scopes[collector->context_depth - 1].scope;
        collector->parent_fixed = 1;
    }
    if (event == S_FINISH) {
        if (collector->depth || collector->context_depth) return 0;
        record->n[0].extent = record->count - 1;
        RecordSelectionFrame previous = record_selection_enter(record, NULL, output->s, record->bindings);
        for (int32_t value = record_field_first(&record->n[0]); value >= 0; value = record_field_next(&record->n[value])) finish_text(output, &record->n[value]);
        if (record->n[0].kind != S_GROUP) finish_text(output, &record->n[0]);
        record_selection_leave(&previous);
        if (collector->prepared) {
            PreparedRecord *prepared = collector->prepared;
            prepared->record = *record; prepared->bindings = output->bindings;
            prepared->record.bindings = &prepared->bindings;
            prepared->rank = output->kr; prepared->ordinal = collector->ordinal;
            for (uint32_t i = 0; i < prepared->record.count; i++) prepared->record.n[i].owner = &prepared->record;
            record_bind_declarations(output, prepared);
        } else record_execute(output, record, 0, collector->ordinal, 0);
        return S_VISIT_OK | (collector->prepared ? S_VISIT_RETAIN : 0);
    }
    if (event == S_CLOSE) {
        if (!collector->depth) return 0;
        uint32_t scope = collector->scopes[--collector->depth].scope;
        RecordSelectionFrame previous = record_selection_enter(record, NULL, output->s, record->bindings);
        for (int32_t value = record_field_first(&record->n[scope]); value >= 0; value = record_field_next(&record->n[value])) finish_text(output, &record->n[value]);
        record_selection_leave(&previous);
        record->n[scope].extent = record->count - 1;
        return S_VISIT_OK;
    }
    int32_t parent = collector->depth ? (int32_t)collector->scopes[collector->depth - 1].scope : 0;
    int merge_root = !collector->merged_root && !collector->depth && record->count == 1 &&
                     (field->flags & S_FIELD_TIER) && !field->tier;
    uint32_t variable;
    if (merge_root) {
        SemanticValue *root = &record->n[0];
        root->name = field->name; root->nlen = field->name_length; root->val = field->value; root->vlen = field->value_length;
        root->kind = field->kind; root->join = (uint8_t)((field->flags & S_FIELD_LIST) ? S_PIECES : (field->flags & S_FIELD_JOIN) != 0);
        root->trim = (uint8_t)((field->flags & S_FIELD_TRIM) != 0); variable = 0; collector->merged_root = 1;
    } else if (event == S_FIELD && field->kind == S_TEXT && (field->flags & S_FIELD_JOIN)) {
        int32_t previous = record_field_last(&record->n[parent]);
        if (previous >= 0 && record->n[previous].kind == S_TEXT && record->n[previous].join == 1) {
            join_text(&record->n[previous], field); return S_VISIT_OK;
        }
        variable = semantic_variable(record, output->s->paths, parent, field);
    } else {
        int required = event == S_OPEN || field->kind != S_VALUE || !field->name_length ||
            (field->flags & S_FIELD_SAME_TIER) ||
            record_bind_field(NULL, output->s->paths, (uint32_t)parent, 0, field);
        if (!required) { drop_field(record, parent, field, collector->layout); return S_VISIT_OK; }
        variable = semantic_variable(record, output->s->paths, parent, field);
    }
    if (parent == 0 && variable && record->n[0].parent >= 0) {
        /* The synthetic reduction root owns composition operands, but it is
         * not an ancestor in the source grammar. Parent selectors see the
         * actual inherited scope directly. */
        record->n[variable].parent = record->n[0].parent;
        record_bind_field(record->bindings, output->s->paths, (uint32_t)record->n[0].parent, variable, field);
    }
    if (event == S_OPEN) {
        if (collector->depth == collector->capacity) {
            uint32_t capacity = collector->capacity ? collector->capacity * 2 : 32;
            if (capacity < collector->capacity) return 0;
            SemanticScope *scopes = scratch_alloc((size_t)capacity * sizeof *scopes);
            if (collector->depth) memcpy(scopes, collector->scopes, (size_t)collector->depth * sizeof *scopes);
            collector->scopes = scopes; collector->capacity = capacity;
        }
        collector->scopes[collector->depth++] = (SemanticScope){variable};
        RecordSelectionFrame previous = record_selection_enter(record, NULL, output->s, record->bindings);
        const Dis *rule = record_unconditional_disposition(output->s, &record->n[variable]);
        int skip = rule && rule->what == D_OMIT;
        record_selection_leave(&previous);
        if (skip) return S_VISIT_OK | S_VISIT_SKIP;
    }
    return S_VISIT_OK;
}

int record_has_unbound_key(const Say *program, const PreparedRecord *record){
    int gap=0;
                        const SemanticRecord *semantic = &record->record;
                        RecordSelectionFrame previous = record_selection_enter(semantic, NULL, program, &record->bindings);
                        for (uint32_t g = 0; g < semantic->count && !gap; g++) {
                            const SemanticValue *value = &semantic->n[g];
                            if (value->context || value->kind != S_GROUP || !value->nlen) continue;
                            for (int key = 0; key < program->nkey; key++) {
                                const Pair *declaration = &program->key[key];
                                if (!record_field_named(value, declaration->tier, declaration->tier_length)) continue;
                                if (record_field_child(semantic, (int32_t)g, declaration->name, -1) < 0) { gap = 1; break; }
                            }
                        }
                        record_selection_leave(&previous);
    return gap;
}
