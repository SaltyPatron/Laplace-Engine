/* A file in the DAG.
 *
 * A file has a trunk, with children for the file's metadata and for the file's content (Storage: Compositions). The
 * trunk, the metadata and the content are compositions like any other: an ID is the BLAKE3 hash of its constituents
 * (Storage: Identity), so a file's ID comes from what it is made of and from nothing else. The bytes of a file are
 * never hashed to name it.
 *
 *   file      [metadata, content]
 *   metadata  what is said of the file that is not its content, a tree of its own. M of its vertex says so
 *             (LP_SAID_METADATA): the OS's record of the file, as its recipes dispose of it, and what its recipe says
 *             is said of the file itself (say.c, file_record).
 *   content   the tree the file decomposes to: a text, a syntax tree, or the parts its recipe lays it out in.
 *
 * A file its recipe lays out has its trunk made where it is read (say.c); this closes any other file. Whether a file is
 * recorded is what deduplication already answers: its trunk is looked for, trunk to leaf (Storage: Ingestion). The
 * trunk is written last of everything a file is and attests, so a trunk that is recorded has all of it recorded. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compare the DAG's inverse with the still-live input buffer. This does not
 * reopen a source or allocate another complete copy of its bytes. */
static int matches_node(const lp_id *id, const uint8_t *bytes, size_t length, size_t *at){
    int64_t cp = lp_tier0_codepoint(T0, id);
    if (cp >= 0) {
        uint8_t encoded[4]; size_t n = lp_utf8_put((uint32_t)cp, encoded);
        if (*at > length || n > length - *at || memcmp(bytes + *at, encoded, n)) return 0;
        *at += n; return 1;
    }
    const Node *node = table_find(id); if (!node) return 0;
    for (uint32_t v = 0; v < node->nv; v++) {
        const Vtx *vertex = &VTX[node->voff + v];
        for (uint32_t run = 0; run < VRUN(vertex->m); run++)
            if (!matches_node(&vertex->id, bytes, length, at)) return 0;
    }
    return 1;
}
int file_matches_bytes(const lp_id *id, const uint8_t *bytes, size_t length){
    const lp_id absent={{0}};
    if (!length && lp_id_eq(id,&absent)) return 1;
    size_t at = 0; return matches_node(id, bytes, length, &at) && at == length;
}

/* Online form of the recipe's content-defined blocks. The first 4096 parts
 * remain flat, exactly as blocks_of; larger sequences cut only where an ID
 * says to cut. No file/read/window size becomes an entity boundary. */
#define FILE_BLOCK 4096
typedef struct FileBlock {
    Ref *part; size_t n, cap; uint64_t total; int split, multiple;
    struct FileBlock *up;
} FileBlock;
struct FileAssembly { FileBlock content, metadata; Ref *roots; size_t capacity; uint64_t events; };
static int file_cut(const Ref *part){ uint64_t bits; memcpy(&bits, part->id.b + 8, 8); return !(bits & (FILE_BLOCK - 1)); }
static Ref file_block(Ref *part, size_t n){
    if (n == 1) { Ref x = part[0]; if (x.said != LP_SAID_TUPLE && x.said != LP_SAID_CLAIM && x.said != LP_SAID_RECORD && x.said != LP_SAID_HOLDS) x.said = 0; return x; }
    if (n > UINT32_MAX) { fputs("file content block exceeds the Native path cardinality\n", stderr); exit(2); }
    int records = 0; for (size_t i = 0; i < n; i++) records |= part[i].said == LP_SAID_RECORD || part[i].said == LP_SAID_HOLDS;
    Ref x = compose(part, (uint32_t)n, ref_above(part, n)); x.said = records ? LP_SAID_HOLDS : 0; return x;
}
static void file_block_push(FileBlock *block, Ref part);
static void file_block_emit(FileBlock *block, Ref *parts, size_t n){
    if (!block->up) { block->up = calloc(1, sizeof *block->up); if (!block->up) { perror("file block level"); exit(1); } }
    file_block_push(block->up, file_block(parts, n));
}
static void file_block_push(FileBlock *block, Ref part){
    if (block->n && !file_cut(&block->part[block->n - 1])) block->multiple = 1;
    if (block->n == block->cap) { block->cap = block->cap ? block->cap * 2 : 64; block->part = xrealloc(block->part, block->cap * sizeof *block->part); }
    block->part[block->n++] = part; block->total++;
    if (!block->split && block->total > FILE_BLOCK && block->multiple) {
        block->split = 1; size_t from = 0;
        for (size_t i = 0; i < block->n; i++) if (file_cut(&block->part[i])) { file_block_emit(block, block->part + from, i - from + 1); from = i + 1; }
        block->n -= from; memmove(block->part, block->part + from, block->n * sizeof *block->part);
    } else if (block->split && file_cut(&part)) { file_block_emit(block, block->part, block->n); block->n = 0; }
}
static Ref file_block_finish(FileBlock *block){
    if (!block->split) return block->total == 1 ? block->part[0] : file_block(block->part, block->n);
    if (block->n) { file_block_emit(block, block->part, block->n); block->n = 0; }
    return file_block_finish(block->up);
}
static void file_block_free(FileBlock *block){
    if (block->up) { file_block_free(block->up); free(block->up); }
    free(block->part);
}
static void binding_part(void *context, const Ref *part){ file_block_push(context, *part); }
Ref file_context_value(const File *file){
    static const char hex[]="0123456789abcdef"; uint8_t text[64];
    for (unsigned i=0;i<32;i++) { text[2*i]=hex[file->reading_context[i]>>4]; text[2*i+1]=hex[file->reading_context[i]&15]; }
    return text_ref(CTX[0],text,sizeof text);
}
static int file_binding_metadata(File *file, Ref *out){
    if (file->bindings_attached) return 0;
    file->bindings_attached = 1;
    FileBlock block = {0};
    size_t count=say_has_keys(file->recipe)?keys_file_export(file->order,binding_part,&block):0;
    if (keys_file_reads(file->order)) {
        Ref parts[2]={text_ref(CTX[0],(const uint8_t *)KEY_CONTEXT_TAG,sizeof KEY_CONTEXT_TAG-1),file_context_value(file)};
        Ref context=compose(parts,2,ref_above(parts,2)); context.said=0; file_block_push(&block,context); count++;
    }
    if (!count) { file_block_free(&block); return 0; }
    *out = file_block_finish(&block); out->said = 0; file_block_free(&block); return 1;
}
void file_stream_begin(File *file){
    file->assembly = calloc(1, sizeof *file->assembly); if (!file->assembly) { perror("file assembly"); exit(1); }
    file->partial = file->laid = 1; Ref os;
    if (file_os(file->recipe, file, &os)) file_block_push(&file->assembly->metadata, os);
}
void file_stream_add(File *file, const Ref *things, size_t n, const Ref *metadata, size_t nm, uint64_t events){
    for (size_t i = 0; i < n; i++) file_block_push(&file->assembly->content, things[i]);
    for (size_t i = 0; i < nm; i++) { Ref x = metadata[i]; x.said = LP_SAID_TUPLE; file_block_push(&file->assembly->metadata, x); }
    file->assembly->events += events;
}
const Ref *file_stream_roots(File *file, size_t *count){
    struct FileAssembly *assembly = file->assembly; size_t n = 0;
    for (int side = 0; side < 2; side++) for (FileBlock *block = side ? &assembly->metadata : &assembly->content; block; block = block->up) {
        if (block->n > SIZE_MAX - n) { fputs("file assembly root overflow\n", stderr); exit(1); }
        if (n + block->n > assembly->capacity) { assembly->capacity = n + block->n + 1024; assembly->roots = xrealloc(assembly->roots, assembly->capacity * sizeof *assembly->roots); }
        if (block->n) memcpy(assembly->roots + n, block->part, block->n * sizeof *block->part);
        n += block->n;
    }
    *count = n; return assembly->roots;
}
void file_stream_finish(File *file){
    struct FileAssembly *assembly = file->assembly;
    Ref bindings; if (file_binding_metadata(file, &bindings)) file_block_push(&assembly->metadata, bindings);
    Ref metadata = {0}; int has_metadata = assembly->metadata.total != 0;
    if (has_metadata) metadata = file_block_finish(&assembly->metadata);
    if (assembly->content.total) {
        Ref content = file_block_finish(&assembly->content);
        if (content.said != LP_SAID_HOLDS && content.said != LP_SAID_RECORD) content.said = 0;
        if (has_metadata) {
            Ref parts[2] = {said_metadata(metadata), content}; file->file = compose(parts, 2, ref_above(parts, 2));
            file->file.said = 0; file->file.outcome = 0; file->file.position = 0; file->file.spare = 0;
        } else file->file = content;
        file->has_file = 1; file->trunk = content;
    } else if (assembly->events && has_metadata) { file->file = metadata; file->file.said = 0; file->has_file = 0; }
    file->partial = 0; file_block_free(&assembly->content); file_block_free(&assembly->metadata);
    free(assembly->roots); free(assembly); file->assembly = NULL;
}


static Ref recs_of(Ref *r, size_t n){
    if (n == 1) return r[0];
    if (n <= 64) { Ref x = compose(r, (uint32_t)n, ref_above(r, n)); x.said = LP_SAID_HOLDS; return x; }
    size_t m = (n + 63) / 64; Ref *up = malloc(m * sizeof *up);
    for (size_t i = 0; i < m; i++) { size_t a = i * 64, k = n - a < 64 ? n - a : 64; up[i] = recs_of(r + a, k); }
    Ref out = recs_of(up, m); free(up); return out;
}

/* The file is whole: its trunk, over its metadata, its content, and any records its structure attests. */
void file_close(File *f){
    if (f->laid) {
        /* The provider owns the content. Compiled lookup state belongs beside
         * it in file metadata, so it can be restored after trunk pruning. */
        if (!f->has_file || f->partial) return;
        Ref bindings; if (!file_binding_metadata(f, &bindings)) return;
        FileBlock metadata = {0}; Ref os;
        if (file_os(f->recipe, f, &os)) file_block_push(&metadata, os);
        for (uint64_t i = 0; i < f->nsaid; i++) if (f->said[i].said == LP_SAID_METADATA) {
            Ref part = f->said[i]; part.said = LP_SAID_TUPLE; file_block_push(&metadata, part);
        }
        file_block_push(&metadata, bindings);
        Ref parts[2] = {said_metadata(file_block_finish(&metadata)), f->trunk};
        f->file = compose(parts, 2, ref_above(parts, 2)); f->file.said = 0;
        f->file.outcome = 0; f->file.position = f->file.spare = 0;
        file_block_free(&metadata); return;
    }
    Ref os; int has_os = file_os(f->recipe, f, &os);                       /* the OS's record of it, one node (say.c) */
    if (!has_os && !f->nsaid) { f->file = f->trunk; f->file.said = 0; f->has_file = 1; return; }
    Ref part[3]; uint32_t np = 0;
    if (f->has_packaging) {
        Ref metadata[2]; uint32_t count = 0;
        if (has_os) metadata[count++] = os;
        const lp_id absent={{0}};
        if (!lp_id_eq(&f->packaging.id,&absent)) {
            Ref original[2] = {string_ref((const uint8_t *)"packaging", 9), f->packaging};
            metadata[count++] = said_tuple(compose(original, 2, ref_above(original, 2)));
        }
        part[np++] = said_metadata(compose(metadata, count, ref_above(metadata, count)));
    } else if (has_os) part[np++] = said_metadata(os);
    const lp_id absent={{0}};
    if (!f->has_packaging || !lp_id_eq(&f->trunk.id,&absent)) { part[np] = f->trunk; part[np++].said = 0; }
    if (f->nsaid) { part[np++] = recs_of(f->said, f->nsaid); free(f->said); f->said = NULL; f->nsaid = 0; }
    f->file = compose(part, np, ref_above(part, np)); f->file.said = 0; f->has_file = 1;
    if (!f->source) f->witness = f->file;
}

/* A source's trunk: [its record, its files' trunks in the order of their paths]. The record is the source file's
 * witness, or its called line where the witness is named file by file. Its ID follows from its constituents and is
 * looked for like any other. A user's prompt has none: it has its turn, session and user. */
const char *source_called(const Source *s){ return s->witness[0] && !strchr(s->witness, '{') ? s->witness : s->called[0] ? s->called : NULL; }
static const File *by_path_of;
static int by_path(const void *a, const void *b){ return strcmp(by_path_of[*(const int *)a].path, by_path_of[*(const int *)b].path); }
int source_trunk(const Source *s, const File *files, int nfiles, Ref *out){
    const char *what = source_called(s); if (!what) return 0;
    int *at = malloc(sizeof(int) * (size_t)(nfiles ? nfiles : 1)), n = 0;
    for (int i = 0; i < nfiles; i++) if (files[i].source == s && files[i].has_file && !files[i].skipped) at[n++] = i;
    if (!n) { free(at); return 0; }
    by_path_of = files; qsort(at, (size_t)n, sizeof(int), by_path);
    Ref *t = malloc(sizeof(Ref) * (size_t)n); for (int k = 0; k < n; k++) { t[k] = files[at[k]].file; if (t[k].said != LP_SAID_HOLDS && t[k].said != LP_SAID_RECORD) { t[k].said = 0; t[k].outcome = 0; t[k].position = 0; t[k].spare = 0; } }     /* a file is said nothing; one with no metadata, what its content is */
    Ref content = compose(t, (uint32_t)n, ref_above(t, (uint64_t)n)); content.said = 0;
    Ref two[2] = { said_metadata(text_ref(CTX[0], (const uint8_t *)what, strlen(what))), content }; two[1].said = 0;
    *out = compose(two, 2, ref_above(two, 2)); out->said = 0; free(t); free(at); return 1;
}
