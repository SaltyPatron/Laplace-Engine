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
PGresult *db_ask(PGconn *, const char *sql, int n, const char *const *v, const int *l, const int *f);   /* planned once for the connection; sql: a literal */

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
int cmd_replay(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_sources(int argc, char **argv);
int cmd_tier0(int argc, char **argv);
int cmd_flags(int argc, char **argv);
int cmd_bench(int argc, char **argv);
int cmd_model(int argc, char **argv);

/* ---- references: an entity's ID, its real coordinate, and its tier */
typedef lp_ref Ref;

/* ---- the node table: compositions keyed by ID, sharded by the ID's first byte, each shard behind its own lock */
typedef struct { lp_id id; uint32_t run; } __attribute__((packed)) Vtx;      /* run: M as it is written: the run, and above it what the vertex is */
#define VRUN(m) ((m) & ((1u << LP_M_RUN_BITS) - 1))
static inline Ref said_claim(Ref r){ r.said = LP_SAID_CLAIM; return r; }
static inline Ref said_tuple(Ref r){ r.said = LP_SAID_TUPLE; return r; }
static inline Ref said_record(Ref r){ if (r.said != LP_SAID_CLAIM && r.said != LP_SAID_TUPLE) r.said = LP_SAID_RECORD; return r; }
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
/* A block of a recipe's patterns: a map (key to value, read before anything is attested), or the claims one kind of
 * statement attests, with the stock default such claims enter at. */
typedef struct {
    int is_map; char name[32];
    char predicate[64];                               /* the claims' predicate, when the source states it by position */
    float enter_rating, enter_deviation;             /* the stock default for this level of attestation */
    int ordered, distinct;                            /* record each claim's position in its record; subject and object differ */
    char subj[8][96]; int nsubj;                      /* in a table: further columns of the subject, which is then the path of them all */
    int rest;                                         /* in a table: the fields after the named columns: 1 pairs of predicate and object, 2 objects */
    int row_tuple;                                    /* in a table: the row itself is the claim, the path of its fields in order */
    char field_pair;                                  /* in a table: every field is written A, this character, B, and is the pair [A, B] */
    char line[512]; void *line_re;                    /* grammar lines: the pattern a line must match to say something, its parts between parentheses */
    char witness_in[64];                              /* in a table: the column that names who says the row */
    double score_from, score_to; int score_mapped;    /* the score is written on a scale of its own: from the lowest to the highest */
    int itself;                                       /* what the row attests is its subject itself */
    char json[64];                                    /* in a table: the column whose field is a JSON object that speaks of the row's claim */
    char witnesses[8][64]; int nwitnesses;            /* in that object: the path of keys to who witnessed the claim */
    char subject_kind[64];                            /* in a table: the subject's name stands only among things of this kind ({dir}: within the file's directory) */
    int together;                                     /* in a table: what a row says it says together: the row is one record */
    int pair;                                         /* the claims are pairs: the source writes no predicate between the two */
    char voices[16][64]; int nvoices;                 /* in a table: columns that are each a witness, named by the column, saying its field of the subject */
    char attest[64][64]; int nattest;                 /* in a table: columns that are each a predicate, by the name the table gives them */
    char name_after, name_before;                     /* the predicate is in the file's name, between these two characters */
    char from[512]; const TSLanguage *lang;           /* a map read from another file, with that file's grammar */
    void *cache;                                      /* that map, read once */
    char in[6][96];                                   /* in a table: the column each part is in (subject, predicate, object, key, value, score), with its resolvers */
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
    struct { char col[64], kind[64]; } kinds[16]; int nkinds;   /* a table: columns whose values name things of a kind */
    int quoted;                                       /* a table whose fields may stand between double quotes (a quote inside is doubled) */
    int skip;                                         /* a table: lines at its head that are not rows */
    struct { char col[64], sep; } list[16]; int nlist;  /* a table: columns whose field is several values, and what parts them (* every column) */
    char remark;                                      /* in a table's row: what follows this character is not the row */
    char separator, comment; int header;              /* a table: what parts its fields, what begins a line that is not a row, whether its first row names its columns */
    char column[64][64]; int ncolumn;                 /* a table's columns, when no row names them */
    char predicate[64];
    char witness[128];                                /* the witness's name, recorded as content */
    struct { char el[64], attr[64], as[64]; int res, within, child, kind, own; } identity[48]; int nidentity;   /* within: the name stands only within the thing it is inside; child: the name is the text of an element inside it */     /* XML: the elements that are things, and the attribute that names each (res: 1 a codepoint, 2 codepoints) */
    struct { char el[64], attr[64], kind[64]; } refer[48]; int nrefer;                  /* XML: attributes whose value names a thing of a kind */
    struct { char el[64], start[32], end[32], text[64]; int inclusive; } stretch[8]; int nstretch;   /* XML: elements that speak of a stretch of a text */
    struct { char rec[64], word[8][64]; int nword; } words[4]; int nwords;      /* XML: an element that is a record of words, and the elements inside it that are its words */
    struct { char el[64], pred[64], obj[64], kind[64]; } link[16]; int nlink;             /* XML: elements that are relations of what they are inside */
    char codepoints[32][32]; int ncodepoints;          /* XML: attributes whose values are codepoints written in hex */
    char about_line[512]; void *about_re;              /* grammar lines: the pattern of the line that names what the file is about */
    char named_key[64], named[8][64]; int nnamed;       /* JSON: an object that holds named_key is the thing these members name together, in this order */
    int kinds_own, voices_file;                        /* a table: a kind stands within the source, [witness, kind, value]; a voice within the file */
    char escaped;                                      /* a table: the character after this one is itself, a line's end included */
    char path_sep, path_join;                          /* a value that begins with path_sep is a path of parts, a part's words joined by path_join */
    int specifics; char claims_under[8][64]; int nclaims_under;   /* what is held with a claim is its specifics: pairs, witnessed with it; but under these keys, claims of their own */
    int linkage, tuples;                               /* JSON: what a thing inside another says, it says of being there; a list of values inside a list is one tuple */
    int keys_things, members;                          /* JSON: the keys of an object inside nothing are things; read natively (members.c) */
    void *empty_like;                                 /* a table: a field that matches this is one the source leaves empty */
    char like[64];                                    /* the recipe it reads as: that recipe's grammar and statements */
    char lineage[128];                                /* the witness this one derives from; copies of it are one consensus */
    char subject_attr[3][48];                         /* subject from a sibling attribute: codepoint, first, last */
    /* a table whose rows come in records (a treebank's sentences): see records.c */
    int record_blank;                                 /* rows up to an empty line are one record */
    char record_line[16], field_is[8];                /* fields: the line that parts records; what parts a field's key from its value */
    char empty[8];                                    /* what the source writes in a field it leaves empty */
    char note_is[8];                                  /* what parts a note's key from its value (a comment line "KEY = VALUE") */
    char about[64];                                   /* the note that holds what the record is about */
    char word[64], number[64];                        /* the column that holds each row's word; the column that numbers the rows */
    char span;                                        /* in the numbering column, what parts the first and last row of a span */
    struct { char col[64], part, is, list; } pairs[8]; int npairs;          /* a field of KEY is VALUE parts */
    struct { char rel[64], head[64]; } relation;      /* each row's relation to the row its head column numbers */
    struct { char col[64], part, is; } relations[4]; int nrelations;       /* a field of HEAD is RELATION parts */
    int source;                                       /* the source it belongs to, or -1: a format any file may be read as */
    int broken; char file[512];                       /* it did not load: it stops the source it belongs to, and no other */
    TSQuery *query;                                   /* set when the recipe attests: a curated source */
    Block *block; int nblock;
} Recipe;
/* A source: a body of content with one identity, however many files it comes in. Its recipes say how each kind of
 * its files reads; the source says who the witness is, where the source is kept, and which sources it comes after. */
typedef struct {
    char name[64];
    char witness[128], lineage[128]; double trust;
    char root[8][512]; int nroot;                     /* where it may be kept: the first that exists, newest of a pattern */
    double room;                                      /* what it takes in the database, in times what its files hold, as measured (0: not measured) */
    char after[16][64]; int nafter;                   /* the sources it comes after */
    char except[8][128]; int nexcept;                 /* files of its roots that are not the source */
    char files[8][512]; int nfiles;                   /* the files it is, by pattern, when it is not everything under a root */
    char reads[8][64]; int nreads;                    /* formats its files are read as (recipes that belong to no source) */
    char found[1024];                                 /* where it is */
} Source;
int     recipes_load(const char *dir, Recipe **out);
Source *sources_loaded(int *n);                                       /* in the order they go in */
Recipe *recipe_for(Recipe *r, int n, const char *path, const Source *of);   /* of: among that source's recipes only */
int     recipes_broken(const Recipe *r, int n, const Source *of);             /* how many of a source's recipes (NULL: of the formats) did not load; each is said */

/* ---- attestation events, in reading order within each file */
/* What was witnessed is the claim with its specifics; the claim is its main components, and holds the standing. A
 * statement that stands alone is both at once. A record (a sentence with what is said of it) is witnessed once, and
 * every claim in it is witnessed in it. */
enum { EV_CLAIM = 0, EV_RECORD = 1, EV_MEMBER = 2 };      /* a claim that is its own record; a record; a claim within the record before it */
typedef struct { lp_id claim, witnessed; float score, enter_rating, enter_deviation; uint32_t position; uint8_t kind, own_witness; lp_id witness; } Event;   /* witness: who witnessed it, when the source names one for this statement and not for the whole file */
typedef struct { Event *e; uint64_t n, cap; } Events;

/* A file decomposed: its trunk, and what its recipe's queries attested. */
typedef struct { const char *path; Recipe *recipe; Ref trunk, witness, lineage; int has_lineage; uint64_t bytes, tokens, incomplete; uint8_t sha[32]; int exact, skipped, known; Events ev;
                 int partial;                          /* more of it is still to come: it is not yet a recorded source */
                 void *columns; int ncolumns;          /* a table read a stretch at a time: its columns, from its head */
                 uint64_t records; } File;             /* records read so far: a stretch's positions go on from the last */
void decompose_file(Ctx *, File *);
void decompose_bytes(Ctx *, File *, uint8_t *src, size_t n, int first);     /* a stretch of a file that is read a stretch at a time */
int  reads_in_stretches(const Recipe *, char *boundary);                    /* whether a file of this recipe can be: 1 at a line's end, 2 at an empty line */
void table_reset(void);                                                      /* what was decomposed is recorded: the table is emptied for what comes next */
Ref  string_ref(const uint8_t *s, size_t n);                          /* text as its entity, remembered per thread */
void ev_push(Events *, const Event *);
void attest_records(const Recipe *, File *, const uint8_t *src, size_t n);      /* records.c */
void attest_fields(const Recipe *, File *, const uint8_t *src, size_t n);
Ref path_ref(const uint8_t *p, size_t n, char sep, char join);
void attest_lines(const Recipe *, File *, const uint8_t *src, size_t n);
void attest_members(const Recipe *, File *, const uint8_t *src, size_t n);       /* members.c */
/* A JSON value that speaks of a thing: every claim it makes of it, and the values at the path of keys wpath. */
typedef struct { Ref *c; int n, cap; } RefList;
int  json_said_of(const Recipe *, Ctx *, const uint8_t *p, size_t n, Ref thing, RefList *claims, const Ref *wpath, int nwpath, RefList *found);
void attest_elements(const Recipe *, File *, void *root_node, const uint8_t *src, size_t n);
void dir_of(const char *path, char *out, size_t cap);                 /* the name of the directory a file is in */      /* elements.c */

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
