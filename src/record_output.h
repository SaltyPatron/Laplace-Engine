#ifndef LAPLACE_RECORD_OUTPUT_H
#define LAPLACE_RECORD_OUTPUT_H

/* Arena-owned products, claim events and containment provenance paths. */
#include "record_worker.h"

void record_emit_event(RecordWorker *, const lp_id *, float score);

void record_emit_claim(RecordWorker *k, Ref *part, int n);
void record_group_claims(RecordWorker *k, uint64_t e0, const Ref *about);
void record_build_provenance(RecordWorker *k, const SemanticRecord *t, int32_t root, uint64_t ev0, const Ref *T);

#endif
