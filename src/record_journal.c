/* Private, host-local journals for deferred compiled operands and native products. */
#define _GNU_SOURCE
#include "record_stream_internal.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

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
int64_t journal_operands(DeferredRecords *deferred, const PreparedRecord *prepared){
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
DeferredJob *deferred_job(DeferredRecords *records){
    DeferredJob *job = records->free;
    if (job) records->free = job->next; else job = arena_alloc(&records->jobs, sizeof *job);
    *job = (DeferredJob){.operands=-1, .products=-1};
    if (records->last) records->last->next = job; else records->first = job;
    records->last = job; return job;
}
void journal_products(DeferredRecords *records, DeferredJob *job, const Ref *things, size_t n, const Ref *metadata, size_t nm){
    if (!records->products) records->products = record_journal();
    if (!job->ready && job->operands >= 0) records->waiting--;
    job->products = journal_seek(records->products, 0, SEEK_END); job->ready = 1;
    uint64_t counts[2] = {n, nm}; journal_write(records->products, counts, sizeof counts);
    journal_write(records->products, things, n * sizeof(Ref)); journal_write(records->products, metadata, nm * sizeof(Ref));
    table_annotate_roots(things,n); table_annotate_roots(metadata,nm);
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
void reset_record_worker(RecordPipeline *pipeline, RecordWorker *worker){
    SayScratch arena = worker->output; arena.used = arena.stack_used = 0;
    *worker = (RecordWorker){.r=pipeline->recipe, .s=pipeline->say, .er=1500.0f, .ed=0.0f,
        .fw=pipeline->file->witness, .fname=pipeline->filename, .fstem=pipeline->stem, .file_scope=pipeline->file_scope,
        .fabout=pipeline->about, .has_fabout=pipeline->has_about, .hw=pipeline->result->hw, .output=arena};
    worker->reserve_declarations = pipeline->key_gaps != 0;
    record_worker_outputs(worker);
}
/* A lexical scope can be just one header. Replaying every deferred operand
 * after every such scope makes a forward-reference span quadratic, with idle
 * workers while one lane rereads its journals. Amortize that work against new
 * binding publications and the outstanding backlog. Source closure still runs
 * the full fixed point, including absent references and canonical duplicates. */
static int deferred_retry(DeferredRecords *records, uint64_t *last, int final){
    uint64_t revision = keys_revision();
    uint64_t interval = records->waiting > RECORD_WINDOW ? records->waiting : RECORD_WINDOW;
    if (!final && revision - *last < interval) return 0;
    *last = revision; return 1;
}
void deferred_bind(RecordPipeline *pipeline, int final){
    DeferredRecords *records = &pipeline->deferred; RecordWorker *worker = &pipeline->evaluators[0];
    if (!records->waiting || !deferred_retry(records, &records->bound_revision, final)) return;
    for (DeferredJob *job = records->first; job; job = job->next) if (!job->ready && job->operands >= 0) {
        records->read.used = records->read.stack_used = 0;
        SayScratch previous = scratch_enter(&records->read);
        PreparedRecord *record = journal_restore(records, job);
        reset_record_worker(pipeline, worker); record_bind_keys(worker, record, final);
        scratch_leave(&records->read, previous);
    }
}
void deferred_finish(RecordPipeline *pipeline, int final){
    DeferredRecords *records = &pipeline->deferred; RecordWorker *worker = &pipeline->evaluators[0];
    if (!final && (!records->waiting || !deferred_retry(records, &records->evaluated_revision, 0))) {
        deferred_drain(pipeline); return;
    }
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
void deferred_close(DeferredRecords *records){
    if (records->operands && fclose(records->operands)) journal_error();
    if (records->products && fclose(records->products)) journal_error();
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
