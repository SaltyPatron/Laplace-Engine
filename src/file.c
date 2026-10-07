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


/* The file is whole: its trunk, over its metadata and its content. */
void file_close(File *f){
    if (f->laid) return;                                                   /* its trees came from the recipe's layout */
    Ref os; if (!file_os(f->recipe, f, &os)) { f->file = f->trunk; f->file.said = 0; f->has_file = 1; return; }   /* the OS's record of it, one node (say.c) */
    Ref part[2] = { said_metadata(os), f->trunk }; part[1].said = 0;
    f->file = compose(part, 2, ref_above(part, 2)); f->file.said = 0; f->has_file = 1;
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
