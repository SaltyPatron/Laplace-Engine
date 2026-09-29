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
int cmd_translate(int argc, char **argv);
int cmd_degrees(int argc, char **argv);
int cmd_text(int argc, char **argv);
int cmd_tree(int argc, char **argv);
int cmd_deploy(int argc, char **argv);
int cmd_index(int argc, char **argv);
int cmd_forget(int argc, char **argv);
int cmd_sweep(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_sources(int argc, char **argv);
int cmd_tier0(int argc, char **argv);
int cmd_flags(int argc, char **argv);
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
typedef struct { uint8_t role; char from[64], to[64]; } Say;        /* a part's text as the source writes it, and as it is said */
/* A block of a recipe's patterns: a map (key to value, read before anything is attested), or the claims one kind of
 * statement attests, with the stock default such claims enter at. */
typedef struct {
    int is_map; char name[32];
    char predicate[64];                               /* the claims' predicate, when the source states it by position */
    float enter_rating, enter_deviation;             /* the stock default for this level of attestation */
    int ordered, distinct;                            /* record each claim's position in its record; subject and object differ */
    char spaces[5];                                   /* per part: a character the source writes for a space */
    Say *say; int nsay;
    Say *ending; int nending;                         /* per part: an ending the source writes, and the one it stands for */
    char name_after, name_before;                     /* the predicate is in the file's name, between these two characters */
    char from[512]; const TSLanguage *lang;           /* a map read from another file, with that file's grammar */
    void *cache;                                      /* that map, read once */
    char in[5][96];                                   /* in a table: the column each part is in (subject, predicate, object, key, value), with its resolvers */
    struct { char col[64]; int op; char val[128]; void *re; } where[8]; int nwhere;   /* in a table: the rows it speaks of */
    char column[64][64]; int ncolumn; char separator; /* a map kept in another table: its columns */
    char *query_src; TSQuery *query;
} Block;
typedef struct {
    char name[64];
    char match[16][128]; int nmatch;                 /* filename globs */
    char grammar[64];                                 /* "text", "vocabulary", or a tree-sitter grammar name */
    const TSLanguage *lang;
    double trust;                                    /* the witness's trust, -1 .. 1 */
    int records;
    uint32_t unit;                                   /* queries run inside parts of the tree no larger than this */
    char itself;                                      /* a character that, in an object, stands for the subject's codepoint */
    char separator, comment; int header;              /* a table: what parts its fields, what begins a line that is not a row, whether its first row names its columns */
    char column[64][64]; int ncolumn;                 /* a table's columns, when no row names them */
    char predicate[64];
    char witness[128];                                /* the witness's name, recorded as content */
    char lineage[128];                                /* the witness this one derives from; copies of it are one consensus */
    char subject_attr[3][48];                         /* subject from a sibling attribute: codepoint, first, last */
    int source;                                       /* the source it belongs to, or -1: a format any file may be read as */
    TSQuery *query;                                   /* set when the recipe attests: a curated source */
    Block *block; int nblock;
} Recipe;
/* A source: a body of content with one identity, however many files it comes in. Its recipes say how each kind of
 * its files reads; the source says who the witness is, where the source is kept, and which sources it comes after. */
typedef struct {
    char name[64];
    char witness[128], lineage[128]; double trust;
    char root[8][512]; int nroot;                     /* where it may be kept: the first that exists, newest of a pattern */
    char after[16][64]; int nafter;                   /* the sources it comes after */
    char except[8][128]; int nexcept;                 /* files of its roots that are not the source */
    char reads[8][64]; int nreads;                    /* formats its files are read as (recipes that belong to no source) */
    char found[1024];                                 /* where it is */
} Source;
int     recipes_load(const char *dir, Recipe **out);
Source *sources_loaded(int *n);                                       /* in the order they go in */
Recipe *recipe_for(Recipe *r, int n, const char *path, const Source *of);   /* of: among that source's recipes only */

/* ---- attestation events, in reading order within each file */
typedef struct { lp_id claim; float score, enter_rating, enter_deviation; uint32_t position; } Event;
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
