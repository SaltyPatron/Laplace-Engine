/* A file in the DAG.
 *
 * A file has a trunk, with children for the file's metadata and for the file's content (Storage: Compositions). The
 * trunk, the metadata and the content are compositions like any other: an ID is the BLAKE3 hash of its constituents
 * (Storage: Identity), so a file's ID comes from what it is made of and from nothing else. The bytes of a file are
 * never hashed to name it.
 *
 *   file      [metadata, content]
 *   metadata  what is said of the file that is not its content, a tree of its own. M of its vertex says so
 *             (LP_SAID_METADATA). Of any file, that is its name, as it is written; what its recipe says is said of the
 *             file itself joins it there (say.c).
 *   content   the tree the file decomposes to: a text, a syntax tree, or the parts its recipe lays it out in.
 *
 * A file its recipe lays out has its trunk made where it is read (say.c); this closes any other file. Whether a file is
 * recorded is what deduplication already answers: its trunk is looked for, trunk to leaf (Storage: Ingestion). The
 * trunk is written last of everything a file is and attests, so a trunk that is recorded has all of it recorded. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t above(const Ref *r, uint64_t n){ uint8_t t = 0; for (uint64_t i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }

/* The name a file has: where it is under its source's root, or its own name when it was given by itself. Where the
 * source is kept on this machine is no part of it. */
static const char *name_of(const File *f){
    const Source *s = f->source; size_t l = s ? strlen(s->found) : 0;
    if (s && l && !strncmp(f->path, s->found, l) && f->path[l] == '/') return f->path + l + 1;
    const char *b = strrchr(f->path, '/'); return b ? b + 1 : f->path;
}

/* The file is whole: its trunk, over its metadata and its content. */
void file_close(File *f){
    if (f->laid) return;                                                   /* its trees came from the recipe's layout */
    const char *name = name_of(f);
    Ref part[2] = { said_metadata(text_ref(CTX[0], (const uint8_t *)name, strlen(name))), f->trunk }; part[1].said = 0;
    f->file = compose(part, 2, above(part, 2)); f->file.said = 0; f->has_file = 1;
}
