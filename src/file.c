/* A file in the DAG.
 *
 * A file has a trunk, with children for the file's metadata and for the file's content (Storage: Compositions). The
 * trunk, the metadata and the content are compositions like any other: an ID is the BLAKE3 hash of its constituents
 * (Storage: Identity), so a file's ID comes from what it is made of and from nothing else. The bytes of a file are
 * never hashed to name it.
 *
 *   file      [metadata, content]
 *   metadata  what is said of the file that is not its content. M of its vertex says so (LP_SAID_METADATA). Of any
 *             file, that is its name, as it is written; a format that carries metadata of its own (EXIF, headers)
 *             gives it through its recipe.
 *   content   what the file decomposes to: a text, a syntax tree, a vocabulary; or, of a curated file, what it
 *             witnessed, in the order it was read.
 *
 * Whether a file is recorded is what deduplication already answers: its trunk is looked for, trunk to leaf
 * (Storage: Ingestion). The trunk is written last of everything a file is and attests, so a trunk that is recorded
 * has all of it recorded. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t above(const Ref *r, uint64_t n){ uint8_t t = 0; for (uint64_t i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }

/* What was just read of the file, kept for its content: every record and claim it witnessed, in the order it was
 * read. Called for every stretch of a file, while what the stretch decomposed to is still at hand. */
void file_take(File *f){
    for (uint64_t i = 0; i < f->ev.n; i++) {
        const Event *e = &f->ev.e[i]; if (e->kind == EV_MEMBER || e->inner) continue;      /* within its record, or held by what it is inside: the tree above holds it */
        Node *x = table_find(&e->witnessed); if (!x) continue;
        if (f->nsaid == f->csaid) { f->csaid = f->csaid ? f->csaid * 2 : 1024; f->said = xrealloc(f->said, f->csaid * sizeof(Ref)); }
        Ref *r = &f->said[f->nsaid++]; memset(r, 0, sizeof *r); r->id = x->id; memcpy(r->c.m, x->m, sizeof r->c.m); r->tier = x->tier;
        r->said = e->kind == EV_RECORD ? LP_SAID_RECORD : LP_SAID_CLAIM;
    }
}

/* The name a file has: where it is under its source's root, or its own name when it was given by itself. Where the
 * source is kept on this machine is no part of it. */
static const char *name_of(const File *f){
    const Source *s = f->source; size_t l = s ? strlen(s->found) : 0;
    if (s && l && !strncmp(f->path, s->found, l) && f->path[l] == '/') return f->path + l + 1;
    const char *b = strrchr(f->path, '/'); return b ? b + 1 : f->path;
}

/* The file is whole: its trunk, over its metadata and its content. */
/* What a curated file witnessed, as one composition of bounded fan: blocks of FILE_FAN by position, level by level,
 * until one holds them all. Canonical from the content alone, as a text's paragraphs and sentences are; a flat
 * composition of millions of records would be one path of a gigabyte, which no row can hold and no read should decode. */
#define FILE_FAN 4096
static Ref tree_of(Ref *r, uint32_t n){
    if (n == 1) return r[0];
    if (n <= FILE_FAN) return compose(r, n, above(r, n));
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i += FILE_FAN) { uint32_t k = n - i < FILE_FAN ? n - i : FILE_FAN; Ref b = k == 1 ? r[i] : compose(r + i, k, above(r + i, k)); b.said = 0; r[m++] = b; }
    return tree_of(r, m);
}
void file_close(File *f){
    if (f->recipe && f->recipe->query && !f->nsaid) { f->has_file = 0; return; }  /* it witnessed nothing: there is nothing of it to record */
    const char *name = name_of(f);
    Ref part[2]; uint32_t n = 0;
    part[n++] = said_metadata(text_ref(CTX[0], (const uint8_t *)name, strlen(name)));
    if (f->recipe && f->recipe->query) {                                       /* curated: what it witnessed */
        if (f->nsaid) { part[n] = f->nsaid == 1 ? f->said[0] : said_record(tree_of(f->said, (uint32_t)f->nsaid)); n++; }
    }
    else { part[n] = f->trunk; part[n].said = 0; n++; }
    f->file = compose(part, n, above(part, n)); f->file.said = 0; f->has_file = 1;
    free(f->said); f->said = NULL; f->nsaid = f->csaid = 0;
}
