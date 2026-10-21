#ifndef LAPLACE_RECORD_DIAGNOSTICS_H
#define LAPLACE_RECORD_DIAGNOSTICS_H

/* Bounded per-worker obligations and source-level diagnostic totals. */
#include "record_worker.h"

enum { DIAG_ATTEST, DIAG_HOLDS, DIAG_RELATE, DIAG_LINE_PREDICATE, DIAG_LINE, DIAG_NO_LIST, DIAG_SELF_VALUE, DIAG_UNRESOLVED_KEY, DIAG_COUNT };
extern const DiagnosticLabel record_diagnostic_labels[DIAG_COUNT];
void record_note_open_name(RecordWorker *, const uint8_t *, uint32_t, uint64_t);
void record_note_unhandled(RecordWorker *sink, const SemanticValue *value);
void record_note_dropped(RecordWorker *sink, const SemanticRecord *record, uint32_t scope, int all);
int record_field_unmentioned(const Say *s, const SemanticValue *x);
void record_obligation_add(RecordWorker *sink, char what, const DiagnosticLabel *label, const uint8_t *a, size_t a_length,
                           const uint8_t *b, size_t b_length, uint64_t count);
void record_obligation_note(RecordWorker *sink, char what, const DiagnosticLabel *label, const uint8_t *a, size_t a_length,
                            const uint8_t *b, size_t b_length);
void record_obligation_node(RecordWorker *sink, const DiagnosticLabel *label, const SemanticValue *node);
void record_note_road_value(RecordWorker *k, const Dis *m, const Ref *v);
void record_statement_count(RecordWorker *k, const Dis *m);

void record_merge_diagnostics(RecordWorker *, const RecordWorker *);

#endif
