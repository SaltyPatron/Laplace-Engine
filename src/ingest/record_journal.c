/* Private, host-local in-memory journals for deferred compiled operands and native products. */
#include "record_stream_internal.h"
#include <omp.h>
#include <stdlib.h>
#include <string.h>

static void buf_read_at(const lp_buf *buf, size_t *off, void *bytes, size_t length){
    if (!length) return;
    if (!buf->b || *off > buf->n || length > buf->n - *off) {
        fputs("deferred record journal: read past buffer\n", stderr);
        exit(2);
    }
    memcpy(bytes, buf->b + *off, length);
    *off += length;
}
typedef struct {
    KeyRank rank; uint64_t ordinal, buckets, bucket_count, links;
    uint32_t values, dropped, dropped_buckets;
} OperandHeader;
int64_t journal_operands(DeferredRecords *deferred, const PreparedRecord *prepared){
    size_t offset = deferred->operands_buf.n;
    const SemanticRecord *record = &prepared->record; const BindingMatrix *bindings = &prepared->bindings;
    OperandHeader header = {.rank=prepared->rank, .ordinal=prepared->ordinal, .buckets=bindings->capacity,
        .bucket_count=bindings->count, .links=bindings->link_count, .values=record->count,
        .dropped=record->dropped_count, .dropped_buckets=record->dropped_bucket_count};
    lp_buf_put(&deferred->operands_buf, &header, sizeof header);
    lp_buf_put(&deferred->operands_buf, bindings->buckets, bindings->capacity * sizeof(BindingBucket));
    lp_buf_put(&deferred->operands_buf, bindings->links, bindings->link_count * sizeof(BindingLink));
    lp_buf_put(&deferred->operands_buf, record->dropped_buckets, (size_t)record->dropped_bucket_count * sizeof(uint32_t));
    for (uint32_t i = 0; i < record->count; i++) {
        const SemanticValue *value = &record->n[i];
        if (value->text) { fputs("unfinished text entered the deferred operand journal\n", stderr); exit(2); }
        SemanticValue saved = *value; saved.owner = NULL; saved.name = saved.val = NULL;
        lp_buf_put(&deferred->operands_buf, &saved, sizeof saved);
        lp_buf_put(&deferred->operands_buf, value->name, value->nlen);
        lp_buf_put(&deferred->operands_buf, value->val, value->vlen);
    }
    for (uint32_t i = 0; i < record->dropped_count; i++) {
        DroppedField saved = record->dropped[i]; saved.name = NULL; saved.seen = 0;
        lp_buf_put(&deferred->operands_buf, &saved, sizeof saved);
        lp_buf_put(&deferred->operands_buf, record->dropped[i].name, saved.length);
    }
    return (int64_t)offset;
}
static PreparedRecord *journal_restore(const DeferredRecords *deferred, const DeferredJob *job){
    size_t at = (size_t)job->operands;
    OperandHeader header; buf_read_at(&deferred->operands_buf, &at, &header, sizeof header);
    PreparedRecord *prepared = scratch_alloc(sizeof *prepared); *prepared = (PreparedRecord){.rank=header.rank, .ordinal=header.ordinal};
    SemanticRecord *record = &prepared->record; BindingMatrix *bindings = &prepared->bindings;
    *bindings = (BindingMatrix){.capacity=(size_t)header.buckets, .count=(size_t)header.bucket_count,
        .link_count=(size_t)header.links, .link_capacity=(size_t)header.links};
    bindings->buckets = scratch_alloc(bindings->capacity * sizeof(BindingBucket));
    bindings->links = scratch_alloc(bindings->link_count * sizeof(BindingLink));
    buf_read_at(&deferred->operands_buf, &at, bindings->buckets, bindings->capacity * sizeof(BindingBucket));
    buf_read_at(&deferred->operands_buf, &at, bindings->links, bindings->link_count * sizeof(BindingLink));
    *record = (SemanticRecord){.count=header.values, .capacity=header.values, .bindings=bindings,
        .dropped_count=header.dropped, .dropped_capacity=header.dropped, .dropped_bucket_count=header.dropped_buckets};
    record->dropped_buckets = scratch_alloc((size_t)header.dropped_buckets * sizeof(uint32_t));
    buf_read_at(&deferred->operands_buf, &at, record->dropped_buckets, (size_t)header.dropped_buckets * sizeof(uint32_t));
    record->n = scratch_alloc((size_t)header.values * sizeof(SemanticValue));
    for (uint32_t i = 0; i < header.values; i++) {
        SemanticValue *value = &record->n[i]; buf_read_at(&deferred->operands_buf, &at, value, sizeof *value); value->owner = record;
        uint8_t *name = scratch_alloc(value->nlen), *text = scratch_alloc(value->vlen);
        buf_read_at(&deferred->operands_buf, &at, name, value->nlen);
        buf_read_at(&deferred->operands_buf, &at, text, value->vlen);
        value->name = name; value->val = text;
    }
    record->dropped = scratch_alloc((size_t)header.dropped * sizeof(DroppedField));
    for (uint32_t i = 0; i < header.dropped; i++) {
        DroppedField *field = &record->dropped[i]; buf_read_at(&deferred->operands_buf, &at, field, sizeof *field);
        uint8_t *name = scratch_alloc(field->length); buf_read_at(&deferred->operands_buf, &at, name, field->length); field->name = name;
    }
    return prepared;
}
DeferredJob *deferred_job(DeferredRecords *records){
    DeferredJob *job = records->free;
    if (job) records->free = job->next; else job = arena_alloc(&records->jobs, sizeof *job);
    *job = (DeferredJob){.operands=-1, .products=-1};
    if (records->last) records->last->next = job; else records->first = job;
    records->last = job; return job;
}
void journal_products(DeferredRecords *records, DeferredJob *job, const Ref *things, size_t n, const Ref *metadata, size_t nm){
    if (!job->ready && job->operands >= 0) records->waiting--;
    job->products = (int64_t)records->products_buf.n; job->ready = 1;
    uint64_t counts[2] = {n, nm};
    lp_buf_put(&records->products_buf, counts, sizeof counts);
    lp_buf_put(&records->products_buf, things, n * sizeof(Ref));
    lp_buf_put(&records->products_buf, metadata, nm * sizeof(Ref));
    table_annotate_roots(things,n); table_annotate_roots(metadata,nm);
    for (size_t i = 0; i < n; i++) table_pin(&things[i].id);
    for (size_t i = 0; i < nm; i++) table_pin(&metadata[i].id);
}
static void deferred_drain(RecordPipeline *pipeline){
    DeferredRecords *records = &pipeline->deferred;
    while (records->first && records->first->ready) {
        DeferredJob *job = records->first; size_t at = (size_t)job->products;
        uint64_t counts[2]; buf_read_at(&records->products_buf, &at, counts, sizeof counts);
        for (int side = 0; side < 2; side++) for (uint64_t i = 0; i < counts[side]; i++) {
            Ref product; buf_read_at(&records->products_buf, &at, &product, sizeof product);
            record_refs_push(side ? &pipeline->result->meta : &pipeline->result->things, &product);
        }
        records->first = job->next; if (!records->first) records->last = NULL;
        job->next = records->free; records->free = job;
    }
    if (!records->first) {
        lp_buf_free(&records->operands_buf);
        lp_buf_free(&records->products_buf);
    }
}
void reset_record_worker(RecordPipeline *pipeline, RecordWorker *worker){
    SayScratch arena = worker->output; arena.used = arena.stack_used = 0;
    *worker = (RecordWorker){.r=pipeline->recipe, .s=pipeline->say, .er=1500.0f, .ed=0.0f,
        .fw=pipeline->file->witness, .fname=pipeline->filename, .fstem=pipeline->stem, .file_scope=pipeline->file_scope,
        .pair=pipeline->pair, .fabout=pipeline->about, .has_fabout=pipeline->has_about, .hw=pipeline->result->hw, .output=arena};
    worker->reserve_declarations = pipeline->key_gaps != 0;
    record_worker_outputs(worker);
}
/* A lexical scope can be just one header. Replaying every deferred operand
 * after every such scope makes a forward-reference span quadratic, with idle
 * workers while one lane rereads its journals. Amortize that work against new
 * binding publications and the outstanding backlog. Source closure still runs
 * the full fixed point, including absent references and canonical duplicates. */
static int deferred_retry(DeferredRecords *records, uint64_t *last, uint32_t backoff, int final){
    uint64_t revision = keys_revision();
    uint64_t base = records->waiting > RECORD_WINDOW ? records->waiting : RECORD_WINDOW;
    uint64_t mult = backoff > 1u ? (uint64_t)backoff : 1u;
    uint64_t interval = base * mult;
    if (!final && revision - *last < interval) return 0;
    *last = revision; return 1;
}
static void deferred_bind_lane(RecordPipeline *pipeline, DeferredRecords *records, DeferredJob **batch,
                               size_t nbatch, SayScratch *lane_reads, uint32_t lane, uint32_t lanes, int final){
    const File *previous_file = table_reading_file(pipeline->file);
    const PairBinding *prev_pair = record_selection_pair(pipeline->pair.active ? &pipeline->pair : NULL);
    RecordWorker *worker = &pipeline->evaluators[lane];
    size_t j0 = nbatch * lane / lanes, j1 = nbatch * (lane + 1) / lanes;
    for (size_t j = j0; j < j1; j++) {
        lane_reads[lane].used = lane_reads[lane].stack_used = 0;
        SayScratch previous = scratch_enter(&lane_reads[lane]);
        PreparedRecord *record = journal_restore(records, batch[j]);
        reset_record_worker(pipeline, worker); record_bind_keys(worker, record, final);
        scratch_leave(&lane_reads[lane], previous);
    }
    record_selection_pair(prev_pair);
    table_reading_file(previous_file);
}
void deferred_bind(RecordPipeline *pipeline, int final){
    DeferredRecords *records = &pipeline->deferred;
    if (!records->waiting || !deferred_retry(records, &records->bound_revision, records->bind_backoff, final)) return;
    uint64_t rev_before = keys_revision();
    size_t nbatch = 0;
    for (DeferredJob *job = records->first; job; job = job->next)
        if (!job->ready && job->operands >= 0) nbatch++;
    if (!nbatch) return;
    uint32_t avail = (pipeline->evaluators && pipeline->worker_count > 1) ? pipeline->worker_count : 1;
    uint32_t lanes = (uint32_t)((nbatch + RECORDS_PER_LANE - 1) / RECORDS_PER_LANE);
    if (lanes > avail) lanes = avail;
    if (lanes <= 1 || nbatch <= 1) {
        RecordWorker *worker = &pipeline->evaluators[0];
        for (DeferredJob *job = records->first; job; job = job->next) if (!job->ready && job->operands >= 0) {
            records->read.used = records->read.stack_used = 0;
            SayScratch previous = scratch_enter(&records->read);
            PreparedRecord *record = journal_restore(records, job);
            reset_record_worker(pipeline, worker); record_bind_keys(worker, record, final);
            scratch_leave(&records->read, previous);
        }
    } else {
        DeferredJob **batch = malloc(sizeof(*batch) * nbatch);
        if (!batch) { perror("deferred bind batch"); exit(2); }
        size_t bi = 0;
        for (DeferredJob *job = records->first; job; job = job->next)
            if (!job->ready && job->operands >= 0) batch[bi++] = job;
        SayScratch *lane_reads = calloc(lanes, sizeof(*lane_reads));
        if (!lane_reads) { perror("deferred bind arenas"); free(batch); exit(2); }
        if (omp_in_parallel()) {
            #pragma omp taskloop grainsize(1) shared(pipeline, records, batch, lane_reads)
            for (uint32_t lane = 0; lane < lanes; lane++)
                deferred_bind_lane(pipeline, records, batch, nbatch, lane_reads, lane, lanes, final);
        } else {
            #pragma omp parallel for num_threads(lanes) schedule(static,1)
            for (uint32_t lane = 0; lane < lanes; lane++)
                deferred_bind_lane(pipeline, records, batch, nbatch, lane_reads, lane, lanes, final);
        }
        for (uint32_t lane = 0; lane < lanes; lane++) arena_release(&lane_reads[lane]);
        free(lane_reads); free(batch);
    }
    if (!final) {
        if (keys_revision() == rev_before)
            records->bind_backoff = records->bind_backoff ? (records->bind_backoff < 32u ? records->bind_backoff * 2u : 32u) : 2u;
        else
            records->bind_backoff = 1u;
    } else {
        records->bind_backoff = 1u;
    }
}
typedef struct { size_t things_off, meta_off, things_n, meta_n; uint32_t lane; uint8_t done; } DeferredExecOut;
static void deferred_finish_lane(RecordPipeline *pipeline, DeferredRecords *records, DeferredJob **batch,
                                 DeferredExecOut *outs, RecordWorker *acc, SayScratch *lane_reads, lp_buf *lane_refs,
                                 size_t nbatch, uint32_t lane, uint32_t lanes, uint64_t revision, int final){
    const File *previous_file = table_reading_file(pipeline->file);
    const PairBinding *prev_pair = record_selection_pair(pipeline->pair.active ? &pipeline->pair : NULL);
    RecordWorker *worker = &pipeline->evaluators[lane];
    size_t j0 = nbatch * lane / lanes, j1 = nbatch * (lane + 1) / lanes;
    for (size_t j = j0; j < j1; j++) {
        lane_reads[lane].used = lane_reads[lane].stack_used = 0;
        SayScratch previous = scratch_enter(&lane_reads[lane]);
        PreparedRecord *record = journal_restore(records, batch[j]);
        reset_record_worker(pipeline, worker); worker->defer_missing = (uint8_t)!final;
        worker->kr = record->rank; worker->bindings = record->bindings;
        record_execute(worker, &record->record, 0, record->ordinal, 0); batch[j]->tried = revision;
        if (!worker->blocked || final) {
            outs[j].done = 1; outs[j].lane = lane;
            outs[j].things_n = worker->things.n; outs[j].things_off = lane_refs[lane].n;
            lp_buf_put(&lane_refs[lane], worker->things.c, worker->things.n * sizeof(Ref));
            outs[j].meta_n = worker->meta.n; outs[j].meta_off = lane_refs[lane].n;
            lp_buf_put(&lane_refs[lane], worker->meta.c, worker->meta.n * sizeof(Ref));
            worker->things.n = worker->meta.n = 0;
            record_merge(&acc[lane], worker);
        }
        scratch_leave(&lane_reads[lane], previous);
    }
    record_selection_pair(prev_pair);
    table_reading_file(previous_file);
}
void deferred_finish(RecordPipeline *pipeline, int final){
    DeferredRecords *records = &pipeline->deferred;
    if (!final && (!records->waiting || !deferred_retry(records, &records->evaluated_revision, records->eval_backoff, 0))) {
        deferred_drain(pipeline); return;
    }
    if (final) {
        uint64_t before;
        do { before = keys_revision(); deferred_bind(pipeline, 1); } while (keys_revision() != before);
    }
    uint64_t revision = keys_revision();
    size_t nbatch = 0;
    if (!pipeline->key_gaps) {
        for (DeferredJob *job = records->first; job; job = job->next)
            if (!job->ready && job->operands >= 0 && (final || job->tried != revision)) nbatch++;
    }
    if (!nbatch) { deferred_drain(pipeline); return; }
    uint32_t avail = (pipeline->evaluators && pipeline->worker_count > 1) ? pipeline->worker_count : 1;
    uint32_t lanes = (uint32_t)((nbatch + RECORDS_PER_LANE - 1) / RECORDS_PER_LANE);
    if (lanes > avail) lanes = avail;
    size_t resolved = 0;
    if (lanes <= 1 || nbatch <= 1) {
        RecordWorker *worker = &pipeline->evaluators[0];
        for (DeferredJob *job = records->first; job; job = job->next) if (!pipeline->key_gaps && !job->ready && job->operands >= 0 && (final || job->tried != revision)) {
            records->read.used = records->read.stack_used = 0;
            SayScratch previous = scratch_enter(&records->read);
            PreparedRecord *record = journal_restore(records, job);
            reset_record_worker(pipeline, worker); worker->defer_missing = (uint8_t)!final;
            worker->kr = record->rank; worker->bindings = record->bindings;
            record_execute(worker, &record->record, 0, record->ordinal, 0); job->tried = revision;
            if (!worker->blocked || final) {
                resolved++;
                journal_products(records, job, worker->things.c, worker->things.n, worker->meta.c, worker->meta.n);
                worker->things.n = worker->meta.n = 0; record_merge(pipeline->result, worker);
            }
            scratch_leave(&records->read, previous);
        }
    } else {
        DeferredJob **batch = malloc(sizeof(*batch) * nbatch);
        DeferredExecOut *outs = calloc(nbatch, sizeof(*outs));
        RecordWorker *acc = calloc(lanes, sizeof(*acc));
        SayScratch *lane_reads = calloc(lanes, sizeof(*lane_reads));
        lp_buf *lane_refs = calloc(lanes, sizeof(*lane_refs));
        if (!batch || !outs || !acc || !lane_reads || !lane_refs) { perror("deferred finish lanes"); exit(2); }
        for (uint32_t lane = 0; lane < lanes; lane++) {
            acc[lane] = (RecordWorker){.r=pipeline->recipe, .s=pipeline->say, .er=1500.0f, .ed=0.0f,
                .fw=pipeline->file->witness, .fname=pipeline->filename, .fstem=pipeline->stem, .file_scope=pipeline->file_scope,
                .pair=pipeline->pair, .fabout=pipeline->about, .has_fabout=pipeline->has_about, .hw=pipeline->result->hw};
            record_worker_outputs(&acc[lane]);
        }
        size_t bi = 0;
        for (DeferredJob *job = records->first; job; job = job->next)
            if (!job->ready && job->operands >= 0 && (final || job->tried != revision)) batch[bi++] = job;
        if (omp_in_parallel()) {
            #pragma omp taskloop grainsize(1) shared(pipeline, records, batch, outs, acc, lane_reads, lane_refs)
            for (uint32_t lane = 0; lane < lanes; lane++)
                deferred_finish_lane(pipeline, records, batch, outs, acc, lane_reads, lane_refs, nbatch, lane, lanes, revision, final);
        } else {
            #pragma omp parallel for num_threads(lanes) schedule(static,1)
            for (uint32_t lane = 0; lane < lanes; lane++)
                deferred_finish_lane(pipeline, records, batch, outs, acc, lane_reads, lane_refs, nbatch, lane, lanes, revision, final);
        }
        for (size_t j = 0; j < nbatch; j++) if (outs[j].done) {
            resolved++;
            const Ref *th = outs[j].things_n ? (const Ref *)(lane_refs[outs[j].lane].b + outs[j].things_off) : NULL;
            const Ref *mt = outs[j].meta_n ? (const Ref *)(lane_refs[outs[j].lane].b + outs[j].meta_off) : NULL;
            journal_products(records, batch[j], th, outs[j].things_n, mt, outs[j].meta_n);
        }
        for (uint32_t lane = 0; lane < lanes; lane++) {
            record_merge(pipeline->result, &acc[lane]);
            arena_release(&lane_reads[lane]);
            lp_buf_free(&lane_refs[lane]);
        }
        record_workers_free(acc, lanes);
        free(lane_reads); free(lane_refs); free(outs); free(batch);
    }
    if (!final) {
        if (resolved == 0)
            records->eval_backoff = records->eval_backoff ? (records->eval_backoff < 32u ? records->eval_backoff * 2u : 32u) : 2u;
        else
            records->eval_backoff = 1u;
    } else {
        records->eval_backoff = 1u;
    }
    deferred_drain(pipeline);
}
void deferred_close(DeferredRecords *records){
    lp_buf_free(&records->operands_buf);
    lp_buf_free(&records->products_buf);
    arena_release(&records->jobs); arena_release(&records->read); *records = (DeferredRecords){0};
}
void pending_outputs(RecordPipeline *pipeline){
    RecordWorker *result = pipeline->result; File *file = pipeline->file;
    if (pipeline->names_file) { table_pin(&pipeline->filename.id); table_pin(&pipeline->stem.id); } table_pin(&file->witness.id);
    if (pipeline->say->file_qualifiers) table_pin(&pipeline->file_scope.id);
    if (file->has_lineage) table_pin(&file->lineage.id);
    for (size_t i = 0; i < result->things.n; i++) table_pin(&result->things.c[i].id);
    for (size_t i = 0; i < result->meta.n; i++) table_pin(&result->meta.c[i].id);
    for (uint64_t i = 0; i < result->ev.n; i++) ev_push(&file->ev, &result->ev.e[i]);
    result->ev.n = 0;
}
