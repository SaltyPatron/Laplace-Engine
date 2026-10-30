#ifndef LAPLACE_RECORD_VALUES_H
#define LAPLACE_RECORD_VALUES_H

/* Canonical value/identity composition and projected key resolution.
 * Record caches and recursion state stay within the active worker frame. */
#include "record_select.h"

typedef struct { uint64_t unknown; int nunk; int32_t at[2]; uint8_t had[2]; } UnkMark;
typedef struct { char k[16][160]; int n; } Keys;
typedef struct { int base, depth; long codepoint; char marker; } RecordValueFrame;
RecordValueFrame record_values_enter(void);
void record_values_leave(const RecordValueFrame *);
Ref record_text(const uint8_t *p, size_t n);
int record_value(RecordWorker *k, const SemanticRecord *t, int32_t c, Ref *out, int depth);
int record_thing(RecordWorker *k, const SemanticRecord *t, int32_t g, Ref *out, int depth);
int record_subject(RecordWorker *k, const SemanticRecord *t, int32_t g, Ref *out);
void record_index_keys(RecordWorker *sink, const SemanticRecord *tree);
int record_value_empty(const Say *s, const SemanticValue *x);
int record_names_identity(RecordWorker *k, const SemanticRecord *t, int32_t g, const SemanticValue *y);
int record_entity(RecordWorker *k, const SemanticRecord *t, int32_t c, Ref *out);
void record_publish_keys(RecordWorker *k, const SemanticRecord *t, int32_t root, int reserve);
void record_highway_values(RecordWorker *k, const SemanticRecord *t, int32_t root);
void record_vocabulary_evidence(RecordWorker *, const SemanticRecord *);
int record_admit_references(RecordWorker *, const SemanticRecord *, int32_t root, int count);

#endif
