/* Laplace-Engine internals: one pipeline for every source. A recipe names how a file decomposes (UAX #29 text, or a
 * tree-sitter grammar loaded at run time) and which parts of it attest what. Decomposition runs on every core into a
 * table of nodes sharded by ID; claims are compositions like any other; the database receives binary COPY and
 * set-based statements only. */
#ifndef LAPLACE_ENGINE_H
#define LAPLACE_ENGINE_H

#include "laplace/laplace.h"
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* ---- references: an entity's ID, its real coordinate, and its tier */
typedef struct { lp_id id; int64_t m[4]; uint8_t tier; } Ref;

/* ---- the node table: compositions keyed by ID, sharded by the ID's first byte, each shard behind its own lock */
typedef struct { lp_id id; uint32_t run; } __attribute__((packed)) Vtx;
typedef struct { lp_id id; int64_t m[4]; uint64_t voff; uint32_t nv, len; uint8_t tier, keep; } Node;
typedef struct {
    pthread_mutex_t mu;
    Node *node; uint64_t n, cap;
    uint32_t *slot; uint64_t scap;                   /* open addressing: node index + 1 */
    Vtx *vtx; uint64_t nv, vcap;
    uint64_t hits;
} Shard;
#define NSHARD 256
extern Shard shard[NSHARD];
extern const lp_tier0_record *T0;

void   table_init(void);
Ref    atom(uint32_t cp);
Ref    compose(const Ref *ch, uint32_t n, uint8_t tier);             /* one child is that child */
Node  *table_find(const lp_id *id);                                   /* NULL for atoms and unknown IDs */
uint64_t table_count(void);

/* ---- decomposition (each thread keeps its own working state) */
typedef struct Ctx Ctx;
Ctx   *ctx_new(void);
void   ctx_free(Ctx *);
Ref    text_ref(Ctx *, const uint8_t *s, size_t n);                  /* UAX #29: codepoint, grapheme, word, sentence, paragraph, file */

/* ---- recipes */
typedef struct TSLanguage TSLanguage;
typedef struct TSQuery TSQuery;
typedef struct {
    char name[64];
    char match[16][128]; int nmatch;                 /* filename globs */
    char grammar[64];                                 /* "text", or a tree-sitter grammar name */
    const TSLanguage *lang;
    double trust;                                    /* the witness's trust, -1 .. 1 */
    int records;                                      /* the file is a sequence of line records: parse chunks in parallel */
    char subject_attr[3][48];                         /* subject from a sibling attribute: codepoint, first, last */
    char *query_src; TSQuery *query;                  /* captures: subject, predicate, object; suffix .cp .text .xml .node */
} Recipe;
int     recipes_load(const char *dir, Recipe **out);
Recipe *recipe_for(Recipe *r, int n, const char *path);

/* ---- attestation events, in reading order within each file */
typedef struct { lp_id claim; float score; } Event;
typedef struct { Event *e; uint64_t n, cap; } Events;

/* A file decomposed: its trunk, and what its recipe's queries attested. */
typedef struct { const char *path; Recipe *recipe; Ref trunk; uint64_t bytes; uint8_t sha[32]; int exact, skipped, known; Events ev; } File;
void decompose_file(Ctx *, File *);

typedef struct { uint64_t checked, found, rounds, new_nodes, ent_rows, phy_rows, led, std_new, std_upd; double t_dedup, t_copy, t_sem; } LoadStats;
int load(const char *conninfo, int npg, File *files, int nfiles, LoadStats *st);

int fills(const char *conninfo, Ctx *, const char *phrase, int limit);

double now(void);
void  *xrealloc(void *, size_t);

#endif
