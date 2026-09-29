/* Laplace-Engine internals: one program and one pipeline for every source. A recipe names how a file decomposes
 * (UAX #29 text, a tree-sitter grammar loaded at run time, or a vocabulary) and which parts of it attest what.
 * Decomposition runs on every core into a table of nodes sharded by ID; claims are compositions like any other; the
 * database receives binary COPY and set-based statements only. Everything shared with the database extension is
 * Laplace-Native's: the engine keeps no copy of it. */
#ifndef LAPLACE_ENGINE_H
#define LAPLACE_ENGINE_H

#include "laplace/laplace.h"
#include <libpq-fe.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* ---- where things are: the environment (laplace.env), else what the engine was built with */
const char *laplace_db(void);                        /* LAPLACE_CONNINFO: a libpq connection string */
const char *laplace_recipes(void);                   /* LAPLACE_RECIPES */
const char *laplace_grammars(void);                  /* LAPLACE_GRAMMARS */
const char *laplace_sql(void);                       /* LAPLACE_SQL: Laplace-postgres's sql directory */
const char *laplace_ucd(void);                       /* LAPLACE_UCD */
PGconn *db_connect(const char *conninfo);            /* exits with the server's message if it cannot */

/* ---- commands */
int cmd_ingest(int argc, char **argv);
int cmd_fills(int argc, char **argv);
int cmd_hop(int argc, char **argv);
int cmd_pull(int argc, char **argv);
int cmd_text(int argc, char **argv);
int cmd_tree(int argc, char **argv);
int cmd_deploy(int argc, char **argv);
int cmd_index(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_tier0(int argc, char **argv);
int cmd_bench(int argc, char **argv);
int cmd_model(int argc, char **argv);

/* ---- references: an entity's ID, its real coordinate, and its tier */
typedef lp_ref Ref;

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

void   tier0_open(const char *path);                                  /* maps tier 0 or exits */
void   table_init(void);
Ref    atom(uint32_t cp);
Ref    compose(const Ref *ch, uint32_t n, uint8_t tier);             /* one child is that child */
Node  *table_find(const lp_id *id);                                   /* NULL for atoms and unknown IDs */
uint64_t table_count(void);

/* ---- decomposition (each thread keeps its own working state) */
typedef lp_text Ctx;
extern Ctx **CTX;                                                     /* one per thread */
void   ctx_open(int threads);
Ref    text_ref(Ctx *, const uint8_t *s, size_t n);                  /* UAX #29, recorded in the node table */
Ref    notation_ref(Ctx *, const uint8_t *bytes, size_t n);          /* bytes that are not text: <0xAB> of each, composed */
Ref    vocabulary_ref(Ctx *, const uint8_t *src, size_t n, uint64_t *tokens, uint64_t *byte_tokens);   /* a tokenizer's vocabulary */

/* ---- recipes */
typedef struct TSLanguage TSLanguage;
typedef struct TSQuery TSQuery;
typedef struct {
    char name[64];
    char match[16][128]; int nmatch;                 /* filename globs */
    char grammar[64];                                 /* "text", "vocabulary", or a tree-sitter grammar name */
    const TSLanguage *lang;
    double trust;                                    /* the witness's trust, -1 .. 1 */
    int records;
    uint32_t unit;                                   /* queries run inside parts of the tree no larger than this */
    char predicate[64];
    char witness[128];                                /* the witness's name, recorded as content */
    char lineage[128];                                /* the witness this one derives from; copies of it are one consensus */
    char subject_attr[3][48];                         /* subject from a sibling attribute: codepoint, first, last */
    char *query_src; TSQuery *query;                  /* captures: subject, predicate, object; suffix .cp .text .xml .node */
} Recipe;
int     recipes_load(const char *dir, Recipe **out);
Recipe *recipe_for(Recipe *r, int n, const char *path);

/* ---- attestation events, in reading order within each file */
typedef struct { lp_id claim; float score; } Event;
typedef struct { Event *e; uint64_t n, cap; } Events;

/* A file decomposed: its trunk, and what its recipe's queries attested. */
typedef struct { const char *path; Recipe *recipe; Ref trunk, witness, lineage; int has_lineage; uint64_t bytes, tokens, incomplete; uint8_t sha[32]; int exact, skipped, known; Events ev; } File;
void decompose_file(Ctx *, File *);

typedef struct { uint64_t checked, found, rounds, new_nodes, ent_rows, phy_rows, led, std_new, std_upd; double t_dedup, t_copy, t_sem; } LoadStats;
int load(const char *conninfo, int npg, File *files, int nfiles, LoadStats *st);

/* ---- reading the database: set-based fetches, decoded here */
size_t uuid_param(uint8_t *out, const lp_id *ids, uint32_t n);        /* a binary uuid[] parameter; out holds 20 + 20 n bytes */
void   id_text(const lp_id *id, char out[37]);                        /* as PostgreSQL writes a uuid */

/* Entities back to their text: paths fetched one level at a time for every entity at once, expanded here down to tier 0. */
typedef struct Reader Reader;
Reader *reader_new(PGconn *pg);
void    reader_free(Reader *);
void    reader_want(Reader *, const lp_id *id);                       /* queue an entity; fetched by the next reader_text */
char   *reader_text(Reader *, const lp_id *id, size_t limit);         /* its text (malloc'd), cut at limit bytes with an ellipsis */
uint64_t reader_trips(const Reader *);                                /* round trips made */

double now(void);
void  *xrealloc(void *, size_t);

#endif
