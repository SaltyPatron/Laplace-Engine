#ifndef LAPLACE_RECORD_STREAM_INTERNAL_H
#define LAPLACE_RECORD_STREAM_INTERNAL_H

#include "record_worker.h"
#include "record_select.h"
#include "context_stream.h"
#include <stdio.h>

enum { RECORD_WINDOW = 1024, RECORDS_PER_LANE = 16, MAX_RECORD_LANES = RECORD_WINDOW / RECORDS_PER_LANE };

typedef struct DeferredJob {
    struct DeferredJob *next;
    int64_t operands, products; uint64_t tried; uint8_t ready, key_gap;
} DeferredJob;

typedef struct {
    FILE *operands, *products; DeferredJob *first, *last, *free;
    uint64_t waiting, bound_revision, evaluated_revision;
    SayScratch jobs, read;
} DeferredRecords;

typedef struct RecordPipeline {
    const Recipe *recipe; File *file; const Say *say; const uint8_t *source; size_t length; const SInput *input;
    RecordWorker *result, *workers, *evaluators; uint32_t worker_count;
    Ref filename, stem, file_scope, about; int has_about, failed; uint32_t key_gaps;
    DeferredRecords deferred;
    struct RecordPipeline *next;
} RecordPipeline;

int64_t lp_record_journal_operands(DeferredRecords *deferred, const PreparedRecord *prepared);
DeferredJob *lp_record_deferred_job(DeferredRecords *records);
void lp_record_journal_products(DeferredRecords *records, DeferredJob *job, const Ref *things, size_t n, const Ref *metadata, size_t nm);
void lp_record_reset_worker(RecordPipeline *pipeline, RecordWorker *worker);
void lp_record_deferred_bind(RecordPipeline *pipeline, int final);
void lp_record_deferred_finish(RecordPipeline *pipeline, int final);
void lp_record_deferred_close(DeferredRecords *records);
void lp_record_pending_outputs(RecordPipeline *pipeline);

#define journal_operands lp_record_journal_operands
#define deferred_job lp_record_deferred_job
#define journal_products lp_record_journal_products
#define reset_record_worker lp_record_reset_worker
#define deferred_bind lp_record_deferred_bind
#define deferred_finish lp_record_deferred_finish
#define deferred_close lp_record_deferred_close
#define pending_outputs lp_record_pending_outputs

#endif
