#ifndef LAPLACE_RECORD_SELECT_H
#define LAPLACE_RECORD_SELECT_H

/* Compiled selector execution and field bindings. Selection frames are scoped
 * to one thread/evaluation and are restored when a dependent reading nests. */
#include "record_worker.h"

int record_name_matches(const SemanticValue *, const NameCode *);
const Dis *record_unconditional_disposition(const Say *, const SemanticValue *);

typedef struct {
    const SemanticRecord *record;
    const Say *program;
    const BindingMatrix *bindings;
    int16_t *dispositions;
} RecordSelectionFrame;
RecordSelectionFrame record_selection_enter(const SemanticRecord *, int16_t *, const Say *, const BindingMatrix *);
void record_selection_leave(const RecordSelectionFrame *);
const PathCode *record_selector_code(const char *name);
int record_field_named(const SemanticValue *value, const char *name, size_t length);
const BindingBucket *record_binding_find(const BindingMatrix *matrix, uint32_t scope, uint32_t slot);
int32_t record_field_first(const SemanticValue *value);
int32_t record_field_last(const SemanticValue *value);
int32_t record_field_next(const SemanticValue *value);
int32_t record_slot_first(const SemanticRecord *record, int32_t scope, uint32_t slot);
int32_t record_field_child(const SemanticRecord *record, int32_t scope, const char *name, int32_t after);
uint32_t record_binding_add(BindingMatrix *matrix, uint32_t scope, uint32_t slot, uint32_t value);
int record_bind_field(BindingMatrix *matrix, const PathProgram *program, uint32_t scope, uint32_t value, const SField *field);
int record_field_matches(const SemanticValue *x, const char *name);
const Dis *record_value_meaning(const Say *s, const SemanticValue *x);
const Dis *record_field_disposition(const Say *s, const SemanticValue *x);
int record_field_defaulted(const Say *, const SemanticRecord *, int32_t field);
int32_t record_path_first(const SemanticRecord *tree, int32_t root, const PathStep *step, const PathStep *stop);
int32_t record_select_first(const SemanticRecord *tree, int32_t root, const char *name);
int record_select_all(const SemanticRecord *tree, int32_t root, const char *name, int32_t *out, int capacity);
int record_conditions_match(const Say *s, const SemanticRecord *t, int32_t root, uint32_t which);

#endif
