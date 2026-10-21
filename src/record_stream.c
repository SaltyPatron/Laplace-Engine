/* Source-level record scheduling, dependency barriers and disposable journals.
 * Semantic evaluation is delegated to the record worker API. */
#define _GNU_SOURCE
#include "record_worker.h"
#include <fcntl.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

/* What a file holds, in its order, as one composition. A few thousand parts are one path. More are factored into
 * blocks from the content alone (Storage: repeated blocks are factored from the content alone): a block ends after a
 * part whose own ID says so (one in BLOCK, by its bits), never at a count or a position, so the same parts always part
 * the same way, a part put in or taken out moves only its own block, and a run of parts that repeats is one block
 * wherever it stands. The blocks are composed the same way, level by level, until one holds them all: no path is
 * longer than a row can hold, and a file of millions of records is a tree of its own content. */
#define BLOCK 4096
/* A block holding records, or blocks that hold them, is said to hold them (LP_SAID_HOLDS): a walk down from a trunk
 * to what its records say goes through it, and past content that says nothing. A part standing alone in its block keeps
 * what it is said to be, and how. */
static int any_records(const Ref *r, size_t n){ for (size_t i = 0; i < n; i++) if (r[i].said == LP_SAID_RECORD || r[i].said == LP_SAID_HOLDS) return 1; return 0; }
static Ref block(Ref *r, size_t n){
    if (n == 1) { Ref x = r[0]; if (x.said != LP_SAID_TUPLE && x.said != LP_SAID_CLAIM && x.said != LP_SAID_RECORD && x.said != LP_SAID_HOLDS) x.said = 0; return x; }
    Ref x = compose(r, (uint32_t)n, ref_above(r, n)); x.said = any_records(r, n) ? LP_SAID_HOLDS : 0; return x;
}
static Ref blocks_of(Ref *r, size_t n){
    if (n == 1) return r[0];
    if (n <= BLOCK) return block(r, n);
    Ref *up = malloc(sizeof(Ref) * n); size_t m = 0, from = 0;
    for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, r[i].id.b + 8, 8);
        if ((b & (BLOCK - 1)) == 0 || i == n - 1) { up[m++] = block(r + from, i - from + 1); from = i + 1; } }
    Ref out = m == n ? block(r, n) : blocks_of(up, m); free(up); return out;
}

enum { RECORD_WINDOW = 1024, RECORDS_PER_LANE = 16, MAX_RECORD_LANES = RECORD_WINDOW / RECORDS_PER_LANE };
typedef struct DeferredJob {
    struct DeferredJob *next;
    int64_t operands, products; uint64_t tried; uint8_t ready, key_gap;
} DeferredJob;
typedef struct {
    FILE *operands, *products; DeferredJob *first, *last, *free;
    SayScratch jobs, read;
} DeferredRecords;
typedef struct RecordPipeline {
    const Recipe *recipe; File *file; const Say *say; const uint8_t *source; size_t length; const SInput *input;
    RecordWorker *result, *workers, *evaluators; uint32_t worker_count;
    Ref filename, stem, about; int has_about, failed; uint32_t key_gaps;
    DeferredRecords deferred;
    struct RecordPipeline *next;
} RecordPipeline;
static RecordPipeline *source_pending;
static int source_active;
void say_source_begin(void){
    if (source_active || source_pending) { fputs("a semantic source session is already active\n", stderr); exit(2); }
    source_active = 1;
}

/* Private, host-local journals retain compiled operands and native products,
 * never packaging buffers or parser nodes. Files disappear on close, including an
 * unsuccessful ingest. Large dependency spans therefore do not retain all
 * intervening records or native closures in RAM. */
static void journal_error(void){ perror("deferred record journal"); exit(2); }
#ifdef _WIN32
static void journal_windows_error(const char *operation, DWORD code){
    fprintf(stderr, "deferred record journal: %s failed (Win32 %lu)\n", operation, (unsigned long)code); exit(2);
}
#endif
static FILE *record_journal(void){
#ifdef _WIN32
    wchar_t directory[MAX_PATH + 1], path[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, directory);
    if (!n) journal_windows_error("GetTempPathW", GetLastError());
    if (n > MAX_PATH) { fputs("deferred record journal: temporary directory exceeds GetTempFileNameW's path limit\n", stderr); exit(2); }
    if (!GetTempFileNameW(directory, L"lpr", 0, path)) journal_windows_error("GetTempFileNameW", GetLastError());
    HANDLE handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (handle == INVALID_HANDLE_VALUE) { DWORD code = GetLastError(); DeleteFileW(path); journal_windows_error("CreateFileW", code); }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (fd < 0) { CloseHandle(handle); journal_error(); }
    FILE *file = _fdopen(fd, "w+b"); if (!file) { _close(fd); journal_error(); }
#else
    FILE *file = tmpfile(); if (!file) journal_error();
#endif
    return file;
}
static int64_t journal_seek(FILE *file, int64_t offset, int origin){
#ifdef _WIN32
    if (_fseeki64(file, offset, origin)) journal_error();
    int64_t at = _ftelli64(file);
#else
    if (fseeko(file, (off_t)offset, origin)) journal_error();
    int64_t at = (int64_t)ftello(file);
#endif
    if (at < 0) journal_error(); return at;
}
static void journal_write(FILE *file, const void *bytes, size_t length){
    if (length && fwrite(bytes, 1, length, file) != length) journal_error();
}
static void journal_read(FILE *file, void *bytes, size_t length){
    if (length && fread(bytes, 1, length, file) != length) journal_error();
}
typedef struct {
    KeyRank rank; uint64_t ordinal, buckets, bucket_count, links;
    uint32_t values, dropped, dropped_buckets;
} OperandHeader;
static int64_t journal_operands(DeferredRecords *deferred, const PreparedRecord *prepared){
    if (!deferred->operands) deferred->operands = record_journal();
    FILE *file = deferred->operands; int64_t offset = journal_seek(file, 0, SEEK_END);
    const SemanticRecord *record = &prepared->record; const BindingMatrix *bindings = &prepared->bindings;
    OperandHeader header = {.rank=prepared->rank, .ordinal=prepared->ordinal, .buckets=bindings->capacity,
        .bucket_count=bindings->count, .links=bindings->link_count, .values=record->count,
        .dropped=record->dropped_count, .dropped_buckets=record->dropped_bucket_count};
    journal_write(file, &header, sizeof header);
    journal_write(file, bindings->buckets, bindings->capacity * sizeof(BindingBucket));
    journal_write(file, bindings->links, bindings->link_count * sizeof(BindingLink));
    journal_write(file, record->dropped_buckets, (size_t)record->dropped_bucket_count * sizeof(uint32_t));
    for (uint32_t i = 0; i < record->count; i++) {
        const SemanticValue *value = &record->n[i];
        if (value->text) { fputs("unfinished text entered the deferred operand journal\n", stderr); exit(2); }
        SemanticValue saved = *value; saved.owner = NULL; saved.name = saved.val = NULL;
        journal_write(file, &saved, sizeof saved);
        journal_write(file, value->name, value->nlen); journal_write(file, value->val, value->vlen);
    }
    for (uint32_t i = 0; i < record->dropped_count; i++) {
        DroppedField saved = record->dropped[i]; saved.name = NULL; saved.seen = 0;
        journal_write(file, &saved, sizeof saved); journal_write(file, record->dropped[i].name, saved.length);
    }
    return offset;
}
static PreparedRecord *journal_restore(DeferredRecords *deferred, const DeferredJob *job){
    FILE *file = deferred->operands; journal_seek(file, job->operands, SEEK_SET);
    OperandHeader header; journal_read(file, &header, sizeof header);
    PreparedRecord *prepared = scratch_alloc(sizeof *prepared); *prepared = (PreparedRecord){.rank=header.rank, .ordinal=header.ordinal};
    SemanticRecord *record = &prepared->record; BindingMatrix *bindings = &prepared->bindings;
    *bindings = (BindingMatrix){.capacity=(size_t)header.buckets, .count=(size_t)header.bucket_count,
        .link_count=(size_t)header.links, .link_capacity=(size_t)header.links};
    bindings->buckets = scratch_alloc(bindings->capacity * sizeof(BindingBucket));
    bindings->links = scratch_alloc(bindings->link_count * sizeof(BindingLink));
    journal_read(file, bindings->buckets, bindings->capacity * sizeof(BindingBucket));
    journal_read(file, bindings->links, bindings->link_count * sizeof(BindingLink));
    *record = (SemanticRecord){.count=header.values, .capacity=header.values, .bindings=bindings,
        .dropped_count=header.dropped, .dropped_capacity=header.dropped, .dropped_bucket_count=header.dropped_buckets};
    record->dropped_buckets = scratch_alloc((size_t)header.dropped_buckets * sizeof(uint32_t));
    journal_read(file, record->dropped_buckets, (size_t)header.dropped_buckets * sizeof(uint32_t));
    record->n = scratch_alloc((size_t)header.values * sizeof(SemanticValue));
    for (uint32_t i = 0; i < header.values; i++) {
        SemanticValue *value = &record->n[i]; journal_read(file, value, sizeof *value); value->owner = record;
        uint8_t *name = scratch_alloc(value->nlen), *text = scratch_alloc(value->vlen);
        journal_read(file, name, value->nlen); journal_read(file, text, value->vlen); value->name = name; value->val = text;
    }
    record->dropped = scratch_alloc((size_t)header.dropped * sizeof(DroppedField));
    for (uint32_t i = 0; i < header.dropped; i++) {
        DroppedField *field = &record->dropped[i]; journal_read(file, field, sizeof *field);
        uint8_t *name = scratch_alloc(field->length); journal_read(file, name, field->length); field->name = name;
    }
    return prepared;
}
static DeferredJob *deferred_job(DeferredRecords *records){
    DeferredJob *job = records->free;
    if (job) records->free = job->next; else job = arena_alloc(&records->jobs, sizeof *job);
    *job = (DeferredJob){.operands=-1, .products=-1};
    if (records->last) records->last->next = job; else records->first = job;
    records->last = job; return job;
}
static void journal_products(DeferredRecords *records, DeferredJob *job, const Ref *things, size_t n, const Ref *metadata, size_t nm){
    if (!records->products) records->products = record_journal();
    job->products = journal_seek(records->products, 0, SEEK_END); job->ready = 1;
    uint64_t counts[2] = {n, nm}; journal_write(records->products, counts, sizeof counts);
    journal_write(records->products, things, n * sizeof(Ref)); journal_write(records->products, metadata, nm * sizeof(Ref));
    for (size_t i = 0; i < n; i++) table_pin(&things[i].id);
    for (size_t i = 0; i < nm; i++) table_pin(&metadata[i].id);
}
static void deferred_drain(RecordPipeline *pipeline){
    DeferredRecords *records = &pipeline->deferred;
    while (records->first && records->first->ready) {
        DeferredJob *job = records->first; journal_seek(records->products, job->products, SEEK_SET);
        uint64_t counts[2]; journal_read(records->products, counts, sizeof counts);
        for (int side = 0; side < 2; side++) for (uint64_t i = 0; i < counts[side]; i++) {
            Ref product; journal_read(records->products, &product, sizeof product);
            record_refs_push(side ? &pipeline->result->meta : &pipeline->result->things, &product);
        }
        records->first = job->next; if (!records->first) records->last = NULL;
        job->next = records->free; records->free = job;
    }
    if (!records->first) {
        if (records->operands && fclose(records->operands)) journal_error();
        if (records->products && fclose(records->products)) journal_error();
        records->operands = records->products = NULL;
    }
}
static void reset_record_worker(RecordPipeline *pipeline, RecordWorker *worker){
    SayScratch arena = worker->output; arena.used = arena.stack_used = 0;
    *worker = (RecordWorker){.r=pipeline->recipe, .s=pipeline->say, .er=1500.0f, .ed=0.0f,
        .fw=pipeline->file->witness, .fname=pipeline->filename, .fstem=pipeline->stem,
        .fabout=pipeline->about, .has_fabout=pipeline->has_about, .hw=pipeline->result->hw, .output=arena};
    worker->reserve_declarations = pipeline->key_gaps != 0;
    record_worker_outputs(worker);
}
static void deferred_bind(RecordPipeline *pipeline, int final){
    DeferredRecords *records = &pipeline->deferred; RecordWorker *worker = &pipeline->evaluators[0];
    for (DeferredJob *job = records->first; job; job = job->next) if (!job->ready && job->operands >= 0) {
        records->read.used = records->read.stack_used = 0;
        SayScratch previous = scratch_enter(&records->read);
        PreparedRecord *record = journal_restore(records, job);
        reset_record_worker(pipeline, worker); record_bind_keys(worker, record, final);
        scratch_leave(&records->read, previous);
    }
}
static void deferred_finish(RecordPipeline *pipeline, int final){
    DeferredRecords *records = &pipeline->deferred; RecordWorker *worker = &pipeline->evaluators[0];
    if (final) {
        uint64_t before;
        do { before = keys_revision(); deferred_bind(pipeline, 1); } while (keys_revision() != before);
    }
    uint64_t revision = keys_revision();
    for (DeferredJob *job = records->first; job; job = job->next) if (!pipeline->key_gaps && !job->ready && job->operands >= 0 && (final || job->tried != revision)) {
        records->read.used = records->read.stack_used = 0;
        SayScratch previous = scratch_enter(&records->read);
        PreparedRecord *record = journal_restore(records, job);
        reset_record_worker(pipeline, worker); worker->defer_missing = (uint8_t)!final;
        worker->kr = record->rank; worker->bindings = record->bindings;
        record_execute(worker, &record->record, 0, record->ordinal, 0); job->tried = revision;
        if (!worker->blocked || final) {
            journal_products(records, job, worker->things.c, worker->things.n, worker->meta.c, worker->meta.n);
            worker->things.n = worker->meta.n = 0; record_merge(pipeline->result, worker);
        }
        scratch_leave(&records->read, previous);
    }
    deferred_drain(pipeline);
}
static void deferred_close(DeferredRecords *records){
    if (records->operands && fclose(records->operands)) journal_error();
    if (records->products && fclose(records->products)) journal_error();
    arena_release(&records->jobs); arena_release(&records->read); *records = (DeferredRecords){0};
}
static void pending_outputs(RecordPipeline *pipeline){
    RecordWorker *result = pipeline->result; File *file = pipeline->file;
    table_pin(&pipeline->filename.id); table_pin(&pipeline->stem.id); table_pin(&file->witness.id);
    if (file->has_lineage) table_pin(&file->lineage.id);
    for (size_t i = 0; i < result->things.n; i++) table_pin(&result->things.c[i].id);
    for (size_t i = 0; i < result->meta.n; i++) table_pin(&result->meta.c[i].id);
    for (uint64_t i = 0; i < result->ev.n; i++) ev_push(&file->ev, &result->ev.e[i]);
    result->ev.n = 0;
}
static void page_about(RecordPipeline *pipeline, const SRecords *records){
    if (!pipeline->say->has_about) return;
    SRecords view = *records;
    view.tree = NULL; view.file_layout = NULL; view.started = view.done = view.failed = view.header_pending = view.descend = 0; view.ordinal = 0;
    view.cursor = lp_tls_cursor(ts_tree_root_node(records->tree));
    if (!view.cursor) { pipeline->failed = 1; return; }
    SRecord record;
    while (!pipeline->has_about && s_record_next(&view, &record)) {
        uint32_t length = record.range.end_byte - record.range.start_byte;
        if (!length || length >= 4096) continue;
        char text[4096]; memcpy(text, pipeline->source + record.range.start_byte, length); text[length] = 0;
        regmatch_t match[2];
        if (!regexec(&pipeline->say->about_re, text, 2, match, 0) && match[1].rm_so >= 0 && match[1].rm_eo > match[1].rm_so) {
            pipeline->about = string_ref((const uint8_t *)text + match[1].rm_so, (size_t)(match[1].rm_eo - match[1].rm_so));
            pipeline->has_about = 1;
        }
    }
    if (view.failed) pipeline->failed = 1;
    s_records_close(&view);
}
static void read_record_window(RecordPipeline *pipeline, SRecords *records, uint64_t byte_at){
    SRecord batch[RECORD_WINDOW];
    struct { SayScratch arena; PreparedRecord *record; uint32_t count; } prepared[MAX_RECORD_LANES] = {0};
    int bind_keys = pipeline->say->nkey != 0 || records->stream != NULL;
    while (!pipeline->failed) {
        uint32_t count = 0;
        while (count < RECORD_WINDOW && s_record_next(records, &batch[count])) {
            int boundary = batch[count].state != S_RECORD_VALUE; count++;
            if (boundary) break;
        }
        if (records->failed) { pipeline->failed = 1; break; }
        if (!count) break;
        uint32_t lanes = (count + RECORDS_PER_LANE - 1) / RECORDS_PER_LANE;
        if (lanes > pipeline->worker_count) lanes = pipeline->worker_count;
        const Layout *layout = records->layout;
        #pragma omp taskloop grainsize(1) shared(batch, prepared)
        for (uint32_t lane = 0; lane < lanes; lane++) {
            RecordWorker *worker = &pipeline->workers[lane]; reset_record_worker(pipeline, worker);
            uint32_t begin = (uint32_t)((uint64_t)count * lane / lanes), end = (uint32_t)((uint64_t)count * (lane + 1) / lanes);
            SayScratch previous = {0};
            if (bind_keys) {
                previous = scratch_enter(&prepared[lane].arena);
                prepared[lane].count = end - begin;
                prepared[lane].record = scratch_alloc((end - begin) * sizeof(PreparedRecord));
                memset(prepared[lane].record, 0, (end - begin) * sizeof(PreparedRecord));
            }
            for (uint32_t i = begin; i < end && !__atomic_load_n(&pipeline->failed, __ATOMIC_ACQUIRE); i++) {
                size_t mark = scratch_mark(); worker->bindings = (BindingMatrix){0};
                worker->kr.file = pipeline->file->order;
                worker->kr.at = pipeline->file->at + (records->stream ? batch[i].source_offset : byte_at + batch[i].range.start_byte);
                RecordCollector collector = {.sink=worker, .layout=layout, .ordinal=batch[i].ordinal,
                    .prepared=bind_keys ? &prepared[lane].record[i - begin] : NULL};
                if (collector.prepared) collector.prepared->reserve = batch[i].state == S_RECORD_RESERVE;
                int valid = s_cursor_record(layout, &batch[i], pipeline->source, pipeline->length, record_collect_fields, &collector);
                if (!bind_keys) scratch_restore(mark);
                if (!valid) __atomic_store_n(&pipeline->failed, 1, __ATOMIC_RELEASE);
            }
            if (bind_keys) scratch_leave(&prepared[lane].arena, previous);
        }
        if (pipeline->failed) break;
        if (bind_keys) {
            /* Resolve declarations to a fixed point before producing claims.
             * The native table publishes only monotone improvements at a
             * fixed source rank, so cycles stop when no declaration changes.
             * This revisits compiled operands, never source bytes or ASTs. */
            uint64_t before;
            do {
                before = keys_revision();
                #pragma omp taskloop grainsize(1) shared(prepared)
                for (uint32_t lane = 0; lane < lanes; lane++) {
                    RecordWorker *worker = &pipeline->workers[lane]; reset_record_worker(pipeline, worker);
                    SayScratch previous = scratch_enter(&prepared[lane].arena);
                    for (uint32_t i = 0; i < prepared[lane].count; i++)
                        if (prepared[lane].record[i].record.count) record_bind_declarations(worker, &prepared[lane].record[i]);
                    scratch_leave(&prepared[lane].arena, previous);
                }
                deferred_bind(pipeline, 0);
            } while (keys_revision() != before);
            /* All declarations in this bounded window are now published.
             * Evaluate the compiled operands after that dependency barrier;
             * the source bytes and each native AST were visited only once. */
            #pragma omp taskloop grainsize(1) shared(prepared)
            for (uint32_t lane = 0; lane < lanes; lane++) {
                RecordWorker *worker = &pipeline->workers[lane]; reset_record_worker(pipeline, worker);
                RecordWorker *evaluation = &pipeline->evaluators[lane];
                SayScratch previous = scratch_enter(&prepared[lane].arena);
                for (uint32_t i = 0; i < prepared[lane].count; i++) {
                    PreparedRecord *record = &prepared[lane].record[i];
                    if (!record->record.count || record->reserve || pipeline->key_gaps) continue;
                    reset_record_worker(pipeline, evaluation); evaluation->defer_missing = 1;
                    evaluation->kr = record->rank; evaluation->bindings = record->bindings;
                    record_execute(evaluation, &record->record, 0, record->ordinal, 0);
                    record->ready = evaluation->blocked == 0;
                    record->things_at = worker->things.n; record->meta_at = worker->meta.n;
                    if (record->ready) record_merge(worker, evaluation);
                    record->things_count = worker->things.n - record->things_at;
                    record->meta_count = worker->meta.n - record->meta_at;
                    record_prepared_clear(record);
                }
                scratch_leave(&prepared[lane].arena, previous);
            }
        }
        /* The taskgroup has joined: append complete lane results in source
         * order before reusing their arenas for the next bounded window. */
        for (uint32_t lane = 0; lane < lanes; lane++) {
            RecordWorker *worker = &pipeline->workers[lane];
            if (bind_keys) {
                DeferredRecords *deferred = &pipeline->deferred;
                uint32_t begin = (uint32_t)((uint64_t)count * lane / lanes);
                for (uint32_t i = 0; i < prepared[lane].count; i++) {
                    PreparedRecord *record = &prepared[lane].record[i];
                    const SRecord *framed = &batch[begin + i];
                    if (framed->state == S_RECORD_RESERVE) {
                        DeferredJob *job = deferred_job(deferred); *framed->continuation = job;
                        job->key_gap = record_has_unbound_key(pipeline->say, record);
                        pipeline->key_gaps += job->key_gap; continue;
                    }
                    const Ref *things = record->things_count ? worker->things.c + record->things_at : NULL;
                    const Ref *metadata = record->meta_count ? worker->meta.c + record->meta_at : NULL;
                    DeferredJob *reserved = framed->state == S_RECORD_COMPLETE ? *framed->continuation : NULL;
                    if (framed->state == S_RECORD_COMPLETE && !reserved) { fputs("scope header has no reserved source position\n", stderr); exit(2); }
                    if (reserved && reserved->key_gap) { pipeline->key_gaps--; reserved->key_gap = 0; }
                    if (!record->ready) {
                        DeferredJob *job = reserved ? reserved : deferred_job(deferred);
                        job->operands = journal_operands(deferred, record); job->tried = keys_revision();
                    } else if (reserved || deferred->first) {
                        DeferredJob *job = reserved ? reserved : deferred_job(deferred);
                        journal_products(deferred, job, things, record->things_count, metadata, record->meta_count);
                    } else {
                        for (size_t n = 0; n < record->things_count; n++) record_refs_push(&pipeline->result->things, &things[n]);
                        for (size_t n = 0; n < record->meta_count; n++) record_refs_push(&pipeline->result->meta, &metadata[n]);
                    }
                }
                worker->things.n = worker->meta.n = 0;
            }
            record_merge(pipeline->result, worker);
        }
        if (bind_keys) deferred_finish(pipeline, 0);
        for (uint32_t lane = 0; lane < lanes; lane++) prepared[lane].arena.used = prepared[lane].arena.stack_used = 0;
    }
    for (uint32_t lane = 0; lane < MAX_RECORD_LANES; lane++) arena_release(&prepared[lane].arena);
}

typedef struct { const uint8_t *bytes; size_t length, at; } MemoryInput;
static size_t memory_input_read(void *context, uint8_t *out, size_t capacity){
    MemoryInput *input = context; size_t n = input->length - input->at;
    if (n > capacity) n = capacity;
    if (n) memcpy(out, input->bytes + input->at, n);
    input->at += n; return n;
}
static int input_fill(const SInput *input, lp_buf *buffer, size_t target, size_t quantum, int *ended){
    while (!*ended && buffer->n < target) {
        size_t room = target - buffer->n; if (room > quantum) room = quantum;
        size_t before = buffer->n; uint8_t *out = lp_buf_room(buffer, room);
        size_t n = input->read(input->context, out, room);
        if (n > room) { fputs("record input exceeded its supplied buffer\n", stderr); return 0; }
        buffer->n = before + n;
        if (input->error && input->error(input->context)) return 0;
        if (!n) *ended = 1;
    }
    return 1;
}
int say_record_streamable(const Recipe *recipe){
    const Say *say = recipe ? recipe->say : NULL;
    return say && say->lay.g.compiled && !say->has_about && (say->lay.g.scope_count ||
        (say->lay.g.record_count && !say->lay.g.record_name_count && !say->lay.g.has_context));
}
static void read_records(RecordPipeline *pipeline){
    const uint8_t *original_source = pipeline->source; size_t original_length = pipeline->length;
    MemoryInput memory = {.bytes=original_source, .length=original_length};
    SInput memory_input = {.context=&memory, .read=memory_input_read};
    const SInput *input = pipeline->input ? pipeline->input : &memory_input;
    Layout *layout = malloc(sizeof *layout);
    if (!layout) { perror("record window layout"); pipeline->failed = 1; return; }
    pipeline->evaluators = calloc(pipeline->worker_count, sizeof *pipeline->evaluators);
    if (!pipeline->evaluators) { perror("record evaluators"); free(layout); pipeline->failed = 1; return; }
    *layout = pipeline->say->lay;
    int independent = say_record_streamable(pipeline->recipe), ended = 0;
    SScopeStream *scopes = independent && layout->g.scope_count ? s_scope_stream_open(layout) : NULL;
    if (independent && layout->g.scope_count && !scopes) { fputs("cannot create packaging scope stream\n", stderr); pipeline->failed = 1; }
    size_t quantum = (size_t)8 << 20;
    const char *configured = getenv("LAPLACE_RECORD_WINDOW_BYTES");
    if (configured && *configured) {
        char *end; unsigned long long value = strtoull(configured, &end, 10);
        if (*end || !value || value > UINT32_MAX) { fputs("LAPLACE_RECORD_WINDOW_BYTES must be in 1..UINT32_MAX\n", stderr); exit(2); }
        quantum = (size_t)value;
    }
    uint64_t ordinal = 0, source_row = 0, byte_at = 0;
    size_t header_mark = 0; int has_header = 0, first = 1;
    SayScratch headers = {0}; lp_buf buffer = {0};
    do {
        size_t target = independent ? quantum : UINT32_MAX;
        if (scopes && buffer.n >= target) {
            if (buffer.n > UINT32_MAX - quantum) { pipeline->failed = 1; break; }
            target = buffer.n + quantum;
        }
        if (first && target < 3) target = 3;
        if (!input_fill(input, &buffer, target, quantum, &ended)) { pipeline->failed = 1; break; }
        if (first && pipeline->input && buffer.n >= 3 && buffer.b[0] == 0xef && buffer.b[1] == 0xbb && buffer.b[2] == 0xbf) {
            memmove(buffer.b, buffer.b + 3, buffer.n - 3); buffer.n -= 3; byte_at = 3;
        }
        first = 0; size_t consumed = 0; SRecords records; int status;
        layout->source_row = source_row;
        for (;;) {
            status = scopes ? s_scope_stream_window(&records, scopes, layout, buffer.b, buffer.n, byte_at, ended) :
                independent ? s_records_window(&records, layout, buffer.b, buffer.n, ended, &consumed, &headers) :
                s_records_open(&records, layout, buffer.b, buffer.n);
            if (status || ended) break;
            if (buffer.n > UINT32_MAX - quantum) { status = -1; break; }
            if (!input_fill(input, &buffer, buffer.n + quantum, quantum, &ended)) { status = -1; break; }
        }
        if (status <= 0) {
            fprintf(stderr, "\n  %s: %s does not parse completely with its compiled packaging grammar at byte %llu\n",
                    pipeline->recipe->name, pipeline->file->path, (unsigned long long)byte_at);
            pipeline->failed = 1; break;
        }
        if (!independent) consumed = buffer.n;
        pipeline->source = buffer.b; pipeline->length = buffer.n;
        records.ordinal = ordinal; page_about(pipeline, &records);
        if (omp_in_parallel()) read_record_window(pipeline, &records, byte_at);
        else {
            #pragma omp parallel
            {
                #pragma omp single
                read_record_window(pipeline, &records, byte_at);
            }
        }
        ordinal = records.ordinal;
        if (scopes) consumed = s_scope_stream_consumed(&records);
        if (records.file_layout) {
            Layout *next = records.file_layout; records.file_layout = NULL;
            if (!has_header) { header_mark = records.header_mark; has_header = 1; }
            free(layout); layout = next;
        }
        if (layout->ntier) { layout->tier[0].header = 0; layout->tier[0].skip = 0; }
        s_records_close(&records);
        if (!pipeline->failed && ended && consumed == buffer.n) deferred_finish(pipeline, !source_active || pipeline->result->hw != NULL);
        if (pipeline->file->emit && !pipeline->failed) {
            RecordWorker *result = pipeline->result;
            if (pipeline->file->emit(pipeline->file->emit_context, result->things.c, result->things.n,
                    result->meta.c, result->meta.n, &result->ev)) pipeline->failed = 1;
            result->things.n = result->meta.n = result->ev.n = 0;
        }
        for (size_t i = 0; i < consumed; i++) source_row += buffer.b[i] == '\n';
        byte_at += consumed;
        memmove(buffer.b, buffer.b + consumed, buffer.n - consumed); buffer.n -= consumed;
        if (pipeline->failed || (ended && !buffer.n)) break;
    } while (1);
    if (has_header && !independent) scratch_restore(header_mark);
    if (!pipeline->failed && pipeline->deferred.first && (!source_active || pipeline->result->hw)) {
        fputs("deferred records reached file closure before they were finalized\n", stderr); exit(2);
    }
    if (pipeline->failed || !pipeline->deferred.first) deferred_close(&pipeline->deferred);
    record_workers_free(pipeline->evaluators, pipeline->worker_count); pipeline->evaluators = NULL;
    s_scope_stream_close(scopes); arena_release(&headers); lp_buf_free(&buffer); free(layout);
    pipeline->source = original_source; pipeline->length = original_length;
}

static void finish_laid(RecordPipeline *pipeline){
    const Recipe *r = pipeline->recipe; File *f = pipeline->file; const Say *sy = pipeline->say;
    RecordWorker *part = pipeline->result; size_t nc = 1; Hw *hw = part[0].hw;
    Refs things = {.arena=&part[0].output}, meta = {.arena=&part[0].output}; uint64_t open = 0;
    for (size_t i = 0; i < nc; i++) {
        for (uint64_t j = 0; j < part[i].ev.n; j++) ev_push(&f->ev, &part[i].ev.e[j]);
        for (size_t j = 0; j < part[i].things.n; j++) record_refs_push(&things, &part[i].things.c[j]);
        for (size_t j = 0; j < part[i].meta.n; j++) record_refs_push(&meta, &part[i].meta.c[j]);
        open += part[i].open[0]; record_collect_diagnostics(r->name, &part[i]);
    }
    if (open) { fprintf(stderr, "\n  %s: %llu named parts of %s have no disposition in the recipe: they are left open, not read:", r->name, (unsigned long long)open, f->path);
        char seen[64][64]; int ns = 0;
        for (size_t i = 0; i < nc; i++) for (int j = 0; j < part[i].nopen; j++) { int dup = 0; for (int z = 0; z < ns && !dup; z++) dup = !strcmp(seen[z], part[i].opennm[j]); if (dup || ns == 64) continue; memcpy(seen[ns++], part[i].opennm[j], 64); fprintf(stderr, " %s", part[i].opennm[j]); }
        fputc('\n', stderr); f->incomplete += open; }
    { uint64_t unk = 0; for (size_t i = 0; i < nc; i++) unk += part[i].unknown;
      if (unk) { fprintf(stderr, "\n  %s: %llu keys in %s name a type no list of the highway holds, and say nothing; for instance:", r->name, (unsigned long long)unk, f->path);
          for (size_t i = 0, shown = 0; i < nc && shown < 3; i++) for (int j = 0; j < part[i].nunk && shown < 3; j++, shown++) fprintf(stderr, " [%s]", part[i].unknm[j]); fputc('\n', stderr); } }
    if (f->emit) { file_stream_finish(f); record_workers_free(part, nc); free(pipeline); return; }
    if (hw) {                                                                /* for the highway: what the file says of its types, in the file's order */
        for (size_t i = 0; i < nc; i++) { for (size_t j = 0; j < part[i].nhr; j++) { HwRec *x = &part[i].hr[j]; const HwLine *h = &sy->hw[x->line];
                if (x->what == 1) hw_type(hw, h->a.list, h->say, x->thing);
                else if (x->what == 2) hw_key(hw, h->a.list, x->thing, x->a);
                else if (x->what == 3) hw_alias(hw, h->a.list, x->a, x->b);
                else hw_edge(hw, h->a.list, x->a, h->b.list, x->b);
            }
        }
        record_workers_free(part, nc); free(pipeline); return; }
    /* the trunk: [metadata, content]. The metadata tree: the file's name, and what its parts say of the file itself.
     * The content tree: the things of its outermost tier, in the file's order. */
    /* A file read a stretch at a time gathers what each stretch held, and its trunk is made when the last is read. */
    f->laid = 1; f->has_file = 0;
    { uint64_t need = f->nsaid + things.n + meta.n; if (need > f->csaid) { f->csaid = need * 2 + 1024; f->said = xrealloc(f->said, f->csaid * sizeof(Ref)); }
      for (size_t j = 0; j < meta.n; j++) { Ref x = meta.c[j]; x.said = LP_SAID_METADATA; f->said[f->nsaid++] = x; }
      for (size_t j = 0; j < things.n; j++) { Ref x = things.c[j]; if (x.said == LP_SAID_METADATA) x.said = 0; f->said[f->nsaid++] = x; } }
    if (f->partial) { record_workers_free(part, nc); free(pipeline); return; }
    things.n = meta.n = 0;
    for (uint64_t j = 0; j < f->nsaid; j++) { Ref x = f->said[j]; if (x.said == LP_SAID_METADATA) { x.said = LP_SAID_TUPLE; record_refs_push(&meta, &x); } else record_refs_push(&things, &x); }
    free(f->said); f->said = NULL; f->nsaid = f->csaid = 0;
    Ref *m = malloc(sizeof(Ref) * (meta.n + 1)); size_t nr = (size_t)file_os(r, f, m);          /* the OS's record of it, one node, first */
    memcpy(m + nr, meta.c, sizeof(Ref) * meta.n);
    size_t nm = nr + meta.n; Ref metadata; memset(&metadata, 0, sizeof metadata); if (nm) metadata = blocks_of(m, nm); free(m);
    if (things.n) { Ref content = blocks_of(things.c, things.n); if (content.said != LP_SAID_HOLDS && content.said != LP_SAID_RECORD) content.said = 0;     /* what holds records says so in the file's path */
        if (nm) { Ref two[2] = { said_metadata(metadata), content }; f->file = compose(two, 2, ref_above(two, 2)); } else f->file = content;
        if (nm) { f->file.said = 0; f->file.outcome = 0; f->file.position = 0; f->file.spare = 0; }   /* a file with no metadata is its content: it says in its source's trunk what its content is (records, or a record) */
        f->has_file = 1; f->trunk = content; }
    else if (f->ev.n && nm) { Ref two[1] = { said_metadata(metadata) }; f->file = two[0]; f->file.said = 0; f->has_file = 0; }
    record_workers_free(part, nc); free(pipeline);
}
static void read_laid(const Recipe *r, File *f, const uint8_t *src, size_t n, Hw *hw, const SInput *input){
    /* The compiled recipe is immutable. File-specific headers and lexical
     * operands already have owned storage; no per-file recipe copy is needed. */
    const Say *sy = r->say;
    if (!sy->lay.g.compiled || sy->lay.g.unresolved) {
        fprintf(stderr, "%s: semantic grammar was not compiled before record execution\n", r->name); exit(2);
    }
    const char *base = strrchr(f->path, '/'); base = base ? base + 1 : f->path;
    size_t bl = strlen(base); if (bl > 3 && !strcmp(base + bl - 3, ".gz")) bl -= 3;
    const char *sb = base;
    if (sy->stemfrom) { const char *from = NULL; for (const char *c = base; c < base + bl; c++) if (*c == sy->stemfrom) from = c; if (from) sb = from + 1; }
    const char *dot = memchr(sb, sy->stem ? sy->stem : '.', (size_t)(base + bl - sb));
    Ref fname = string_ref((const uint8_t *)base, bl), fstem = string_ref((const uint8_t *)sb, dot ? (size_t)(dot - sb) : (size_t)(base + bl - sb));
    if (f->emit) {
        table_pin(&fname.id); table_pin(&fstem.id); table_pin(&f->witness.id);
        if (f->has_lineage) table_pin(&f->lineage.id);
    }
    uint32_t workers = (uint32_t)omp_get_max_threads(); if (!workers) workers = 1;
    if (workers > MAX_RECORD_LANES) workers = MAX_RECORD_LANES;
    RecordWorker *part = calloc(1, sizeof *part), *lanes = calloc(workers, sizeof *lanes);
    RecordPipeline *pipeline = calloc(1, sizeof *pipeline);
    if (!part || !lanes || !pipeline) { perror("record workers"); exit(2); }
    *part = (RecordWorker){.r=r, .s=sy, .hw=hw}; record_worker_outputs(part);
    *pipeline = (RecordPipeline){.recipe=r, .file=f, .say=sy, .source=src, .length=n, .input=input,
        .result=part, .workers=lanes, .worker_count=workers, .filename=fname, .stem=fstem};
    if (f->emit || omp_in_parallel()) read_records(pipeline);
    else {
        #pragma omp parallel
        {
            #pragma omp master
            read_records(pipeline);
        }
    }
    record_workers_free(lanes, workers); pipeline->workers = NULL;
    if (pipeline->failed) { f->incomplete++; deferred_close(&pipeline->deferred); record_workers_free(part, 1); free(pipeline); exit(2); }
    if (pipeline->deferred.first) {
        f->partial = 1; pipeline->source = NULL; pipeline->length = 0; pipeline->input = NULL;
        if (!f->emit) pending_outputs(pipeline);
        RecordPipeline *head = __atomic_load_n(&source_pending, __ATOMIC_ACQUIRE);
        do { pipeline->next = head; } while (!__atomic_compare_exchange_n(&source_pending, &head, pipeline, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
        return;
    }
    finish_laid(pipeline);
}
static int pending_file_order(const void *a, const void *b){
    const RecordPipeline *x = *(RecordPipeline *const *)a, *y = *(RecordPipeline *const *)b;
    return x->file->order < y->file->order ? -1 : x->file->order > y->file->order;
}
static int source_drain(source_file_ready_fn ready, void *context, int final){
    RecordPipeline *head = __atomic_exchange_n(&source_pending, NULL, __ATOMIC_ACQ_REL);
    size_t count = 0; for (RecordPipeline *p = head; p; p = p->next) count++;
    RecordPipeline **files = count ? malloc(count * sizeof *files) : NULL;
    RecordWorker *evaluation = count ? calloc(1, sizeof *evaluation) : NULL;
    if (count && (!files || !evaluation)) { perror("source continuations"); exit(2); }
    size_t at = 0; for (RecordPipeline *p = head; p; p = p->next) { files[at++] = p; p->evaluators = evaluation; }
    if (count) qsort(files, count, sizeof *files, pending_file_order);
    uint64_t before;
    do {
        before = keys_revision();
        for (size_t i = 0; i < count; i++) deferred_bind(files[i], final);
    } while (keys_revision() != before);
    int failed = 0;
    for (size_t i = 0; i < count; i++) {
        RecordPipeline *pipeline = files[i]; File *file = pipeline->file;
        deferred_finish(pipeline, final);
        if (file->emit) {
            RecordWorker *result = pipeline->result;
            if (file->emit(file->emit_context, result->things.c, result->things.n,
                           result->meta.c, result->meta.n, &result->ev)) failed = 1;
            result->things.n = result->meta.n = result->ev.n = 0;
        }
        if (pipeline->deferred.first) {
            if (!file->emit) {
                pending_outputs(pipeline);
                if (!failed && ready && ready(context, file)) failed = 1;
                free(file->ev.e); file->ev = (Events){0};
            }
            pipeline->evaluators = NULL;
            RecordPipeline *pending = __atomic_load_n(&source_pending, __ATOMIC_ACQUIRE);
            do { pipeline->next = pending; } while (!__atomic_compare_exchange_n(&source_pending, &pending, pipeline, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
            continue;
        }
        if (!file->emit) file->partial = 0;
        deferred_close(&pipeline->deferred); finish_laid(pipeline);
        if (!failed && ready && ready(context, file)) failed = 1;
    }
    if (evaluation) record_workers_free(evaluation, 1);
    free(files); return failed;
}
int say_source_poll(source_file_ready_fn ready, void *context){ return source_drain(ready, context, 0); }
int say_source_end(source_file_ready_fn ready, void *context){
    int failed = source_drain(ready, context, 1); source_active = 0; return failed;
}
void attest_layout(const Recipe *r, File *f, const uint8_t *src, size_t n){ read_laid(r, f, src, n, NULL, NULL); }
void attest_input(const Recipe *r, File *f, const SInput *input){ read_laid(r, f, NULL, 0, NULL, input); }
void say_highway(const Recipe *r, File *f, const uint8_t *src, size_t n, Hw *hw){ read_laid(r, f, src, n, hw, NULL); }
/* What the recipe says a part states (relation "TEXT" NAME): TEXT, or NULL where it declares no relation of it. */
/* The first way the recipe names a TIER by parts of which two are A and B (each part by the last step of its path): how
 * many parts it names it by, and where A and B stand among them; -1 where no way does. */
