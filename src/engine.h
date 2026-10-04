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
const char *laplace_ucd(void);                       /* LAPLACE_UCD */
PGconn *db_connect(const char *conninfo);            /* exits with the server's message if it cannot */
const char *db_noted(void);                          /* the conninfo of the last db_connect */
PGresult *db_ask(PGconn *, const char *sql, int n, const char *const *v, const int *l, const int *f);   /* planned once for the connection; sql: a literal */

/* ---- the personality firmware: a pull's decisions, read from a file, never from the records (firmware.c) */
enum { FW_HOP, FW_SEARCH, FW_TRANSLATE, FW_FOLLOWS, FW_PULL, FW_OPS };
enum { FW_FRECHET, FW_OUTLIERS, FW_DTW, FW_EDR };
enum { FW_TAKE_FACT, FW_TAKE_SEGMENT, FW_TAKE_ATTESTATIONS, FW_TAKE_CONSTITUENTS, FW_TAKE_CHAIN };
#define FW_CHAIN 8
#define FW_ALTS 4
#define FW_NAMES 32
#define FW_TAKES 16
#define FW_WEIGHS 128
typedef struct {
    char path[4200];
    double k, lambda;                                 /* how far below its rating a standing must hold; the tax on a hop */
    int fan, hops;
    int emit;                                         /* how many constituents a turn may emit (the emission budget STEER is given) */
    double enough;                                    /* a turn is complete when no more than this of what its obligations owed is still owed (0: all of it answered) */
    double top_within;                                /* 0: the top every time; else how near a tie has to be */
    double fact;                                      /* the trust at which a curated member is returned as one fact; above 1: never */
    int order_witness;                                /* on an open claim, the witness's own order before the standing */
    int shape; double shape_n;
    char refuse_predicate[FW_NAMES][96], refuse_witness[FW_NAMES][96]; int nrefuse_predicate, nrefuse_witness;
    struct { int what, n; } take[FW_TAKES]; int ntake; /* a pull's steps, in order */
    char weigh_name[FW_WEIGHS][96]; double weigh[FW_WEIGHS]; int nweigh;
    char role_by[96], role_name[FW_WEIGHS][96]; double role[FW_WEIGHS]; int nrole;      /* how hard a word pulls, by what is attested of it under role_by (its part of speech) */
    char chain[FW_ALTS][FW_CHAIN][96]; int nchain[FW_ALTS], nalt;                                             /* the relations a pull follows from the word that pulls hardest, in order */
    char up[FW_CHAIN][96]; int nup; char language[2][96], gloss[96];                                          /* translation: the relations from a word up to its concept (followed back down in another language); how what stands below the concept says its language (what holds it under the first, and what that says under the second); what is shown of a concept */
} Firmware;
/* ---- the lookups the forward pass is made of (pull.c) */
#define MAXPARTS 12
typedef struct { lp_id id, part[MAXPARTS]; int np; lp_rating r; int matches; double conf; int position; } Claim;   /* a claim is a tuple: a pair, three parts, or a longer path */
const char *firmware_path(void);                     /* LAPLACE_FIRMWARE, or firmware/program.firmware beside the recipes */
Firmware firmware_for(const char *path, int op);
void firmware_say(const Firmware *, int op);
lp_ref entity_named(lp_text *, const char *text, lp_ref *parts, size_t cap, size_t *np);      /* a text's trunk, computed here */
Claim *claims_like(PGconn *, const lp_id *part, const int *have, int fan, double k, int *n, int *capped);
Claim *claims_of(PGconn *, const lp_id *e, int fan, double k, int *n, int *capped);    /* every claim that holds an entity */
void   positions_of(PGconn *, Claim *, int n);
int    claim_by_position(const void *, const void *);
int    claim_by_conf(const void *, const void *);
int    refused(PGconn *, lp_text *, const Firmware *, Claim *, int n);
void   weights_named(lp_text *, const Firmware *, lp_id *ids);                           /* the kinds a firmware weighs, as entities */
double strand_weight(const Firmware *, const lp_id *ids, const lp_id *part, int np);     /* how hard a claim of these parts is allowed to pull */
void   weighed(lp_text *, const Firmware *, Claim *, int n);                             /* each claim's confidence by its weight, and the set in that order */                  /* what the firmware refuses, taken out */

/* ---- commands */
int cmd_ingest(int argc, char **argv);
int cmd_fills(int argc, char **argv);
int cmd_hop(int argc, char **argv);
int cmd_translate(int argc, char **argv);
int cmd_degrees(int argc, char **argv);
int cmd_pull(int argc, char **argv);
int cmd_turn(int argc, char **argv);
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
int cmd_highway(int argc, char **argv);
int cmd_bench(int argc, char **argv);
int cmd_model(int argc, char **argv);
int cmd_structure(int argc, char **argv);

/* ---- the bits a row must have (physicality.mask): what it is, by LP_KIND_*; as the text of a smallint[] parameter */
#define CLAIM_BITS "{0}"
/* ---- references: an entity's ID, its real coordinate, and its tier */
typedef lp_ref Ref;

/* ---- the node table: compositions keyed by ID, sharded by the ID's first byte, each shard behind its own lock */
typedef struct { lp_id id; uint64_t m; } __attribute__((packed)) Vtx;        /* m: M as it is written: the run, and above it what the vertex is (lp_m_of) */
#define VRUN(m) ((uint32_t)((m) & ((1ull << LP_M_RUN_BITS) - 1)))
#define VSAID(m) ((uint32_t)(((m) >> LP_M_RUN_BITS) & LP_M_SAID_MASK))
static inline Ref said_claim(Ref r){ r.said = LP_SAID_CLAIM; return r; }
static inline Ref said_tuple(Ref r){ r.said = LP_SAID_TUPLE; return r; }
static inline Ref said_metadata(Ref r){ r.said = LP_SAID_METADATA; return r; }
static inline Ref said_record(Ref r){ if (r.said != LP_SAID_CLAIM && r.said != LP_SAID_TUPLE) r.said = LP_SAID_RECORD; return r; }
typedef struct { lp_id id; int64_t m[4]; uint64_t voff; uint32_t nv, len; uint8_t tier, keep, kind, live; } Node;   /* kind: bits of what it is said to be (LP_KIND_*), by whatever holds it; live: a node, not a chunk's unused tail */
extern Node *NODE;                                                    /* every node of the table: NODE[0, table_end()), each with live set */
extern Vtx *VTX;                                                      /* a node's vertices: VTX[voff, voff + nv) */
uint64_t table_end(void);
#define TABLE_EACH(x) for (Node *x = NODE, *x##_end = NODE + table_end(); x < x##_end; x++) if (x->live)
extern const lp_tier0_record *T0;
extern const lp_highway *HW;                                          /* the highway, when it is there: types and their mask bits */
Ref    highway_typed(const char *list, const uint8_t *key, size_t n, int *has);   /* a type by a source's key of it: the highway's record as a reference; has = 0 when the list does not know the key */

void   tier0_open(const char *path);                                  /* maps tier 0 or exits */
void   table_init(void);
void   table_kinds(void);                                             /* after a decomposition: what each child is said to be, on the child */
Ref    atom(uint32_t cp);
Ref    compose(const Ref *ch, uint32_t n, uint8_t tier);             /* one child is that child */
Node  *table_find(const lp_id *id);                                   /* NULL for atoms and unknown IDs */
size_t table_parts(const lp_id *id, lp_id *out, size_t cap);         /* a composition's constituents in order, its runs written out; 0 when it is not in the table */
void   table_size(uint64_t bytes);                                    /* while the table is empty: room for what a batch of this many bytes makes */
uint64_t table_count(void);

/* ---- decomposition (each thread keeps its own working state) */
typedef lp_text Ctx;
extern Ctx **CTX;                                                     /* one per thread */
void   ctx_open(int threads);
Ref    text_ref(Ctx *, const uint8_t *s, size_t n);                  /* UAX #29, recorded in the node table */

/* ---- recipes */
typedef struct TSLanguage TSLanguage;
/* A recipe: which files it reads (match), the grammar or layout that gives each file its tree, and what every named
 * part of that tree is (say.c). Configuration only: there is one decomposer, and no code for a format or a source. */
typedef struct {
    char name[64];
    char match[16][128]; int nmatch;                 /* filename globs */
    char grammar[64];                                 /* "text" (UAX #29), "layout" (the recipe's tiers), or a tree-sitter grammar name */
    const TSLanguage *lang;
    double trust;                                    /* the witness's trust, -1 .. 1 */
    char witness[128];                                /* the witness's name, recorded as content */
    char lineage[128];                                /* the witness this one derives from; copies of it are one consensus */
    char like[64];                                    /* the recipe it reads as: that recipe's layout and dispositions */
    int source;                                       /* the source it belongs to, or -1: a format any file may be read as */
    int broken; char file[512];                       /* it did not load: it stops the source it belongs to, and no other */
    int curated;                                      /* it attests: a curated source, mined for what it says, not kept byte for byte */
    void *say;                                        /* the file's layout and the disposition of its parts, as the recipe configures them (say.c) */
} Recipe;
/* A source: a body of content with one identity, however many files it comes in. Its recipes say how each kind of
 * its files reads; the source says who the witness is, where the source is kept, and which sources it comes after. */
typedef struct {
    char name[64];
    char witness[128], lineage[128]; double trust;
    char called[128];                                 /* what the source is, where its witness is named file by file (called NAME) */
    char root[8][512]; int nroot;                     /* where it may be kept: the first that exists, newest of a pattern */
    double room;                                      /* what it takes in the database, in times what its files hold, as measured (0: not measured) */
    int tier0;                                        /* it records every codepoint, tier 0 (its source file says tier0): the Unicode source, first */
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
typedef struct { lp_id claim, witnessed; float score, enter_rating, enter_deviation; uint32_t position; uint8_t kind, own_witness; lp_id witness; uint8_t inner; } Event;   /* witness: who witnessed it, when the source names one for this statement and not for the whole file */
typedef struct { Event *e; uint64_t n, cap; } Events;

/* A file decomposed: its trunk, and what its recipe's queries attested. */
typedef struct { const char *path; Recipe *recipe; Ref trunk, witness, lineage; int has_lineage; uint64_t bytes, incomplete; int exact, skipped, known; Events ev;
                 const Source *source;                 /* the source it is a file of, or NULL for a file given by itself */
                 double trust;                         /* how far its witness is trusted */
                 Ref file; int has_file;               /* the file in the DAG: its trunk, over its metadata and its content */
                 Ref *said; uint64_t nsaid, csaid;     /* what it witnessed, in the order it was read: a curated file's content */
                 int partial;                          /* more of it is still to come: it is not yet a recorded source */
                 uint64_t records;
                 int laid; } File;                     /* laid: its trunk, metadata tree and content tree came from the recipe's layout (say.c) */
int  say_says(Recipe *, const char *path, char *tok);                       /* a recipe's line, if it lays the file out or disposes of a part */
int  say_lays(const Recipe *);
int  say_stretches(const Recipe *);                                         /* 1: a file it lays out can be read a line at a time; 2: a record ending at an empty line at a time; 0: whole */
void comment_off(char *line);                                               /* a recipe line without its comment */
const char *say_refers(const Recipe *, int i);                              /* the i-th recipe whose rows' keys this one's parts refer to, or NULL */
/* A source's keys across its files: what a key names, kept for the files read after (recipe.c). */
void keys_put(const char *recipe, const uint8_t *k, size_t n, Ref x);
int  keys_get(const char *recipe, const uint8_t *k, size_t n, Ref *out);
void attest_layout(const Recipe *, File *, const uint8_t *src, size_t n);             /* records read so far: a stretch's positions go on from the last */
/* The highway, read from the resources by their recipes (types, keyed, alias, maps lines): what a file says of its
 * types, handed over in the file's order (highway.c keeps them). */
typedef struct Hw Hw;
void say_highway(const Recipe *, File *, const uint8_t *src, size_t n, Hw *);
int  say_has_highway(const Recipe *);
void hw_type(Hw *, const char *list, const char *say, Ref thing);                 /* a type of the list: the thing it is */
void hw_key(Hw *, const char *list, Ref thing, const char *key);                  /* a resource's key of that type */
void hw_alias(Hw *, const char *list, const char *key, const char *to);           /* a key that names the type another key names */
void hw_edge(Hw *, const char *la, const char *ka, const char *lb, const char *kb);  /* a mapping: the type one key names to the type another names */
void highway_file(Ctx *, File *, Hw *);                                             /* a file read for the highway (recipe.c) */
int  source_files(const Source *, Recipe *rec, int nrec, char ***paths, Recipe ***of);   /* a source's files its recipes read, in the order they are read (ingest.c) */
void decompose_file(Ctx *, File *);
void decompose_bytes(Ctx *, File *, uint8_t *src, size_t n, int first);     /* a stretch of a file that is read a stretch at a time */
int  reads_in_stretches(const Recipe *, char *boundary);                    /* whether a file of this recipe can be: 1 at a line's end, 2 at an empty line */
void table_reset(void);                                                      /* what was decomposed is recorded: the table is emptied for what comes next */
Ref  string_ref(const uint8_t *s, size_t n);                          /* text as its entity, remembered per thread */
void ev_push(Events *, const Event *);

typedef struct { uint64_t checked, found, rounds, new_nodes, ent_rows, phy_rows, led, std_new, std_upd, known, content_known; double t_dedup, t_copy, t_sem, t_read, t_play, t_wit, t_led; } LoadStats;
int load(const char *conninfo, int npg, File *files, int nfiles, LoadStats *st);
int db_all_recorded(const char *conninfo, const lp_id *ids, const uint8_t *tiers, uint64_t n);   /* every one of them recorded (in its tier's partition) */
int merge(const char *conninfo, int npg);            /* what the runs staged, into the real tables at once (db.c) */
int tier0_write(const char *conninfo, int npg);       /* every codepoint, once, from the perf-cache, into the real tables (laplace deploy) */
int cmd_merge(int argc, char **argv);

/* ---- reading the database: set-based fetches, decoded here */
extern uint32_t id_oid;                                               /* the database's own number for the type of an ID, blake3 */
size_t ids_param(uint8_t *out, const lp_id *ids, uint32_t n);         /* a binary blake3[] parameter; out holds 20 + 20 n bytes */
typedef struct { lp_id *id; int n; } Run;                               /* a trajectory's constituents in order, runs written out */
Run run_of(const uint8_t *ewkb, size_t len);                            /* ... from a path as the database sends it (lp_path_ids) */
/* A path that holds the keys, read from the leaf partitions above floor: one statement a leaf, every core at once.
 * each = 0: the path holds every key (src is 0). each = 1: the path holds one key, src is that key's place.
 * standing = 1: the consensus on the path's entity, one partition a core, after the paths. claim is mask bit 0.
 * floor < 0 reads every tier. The caller frees the paths. */
typedef struct {
    lp_id entity; int src; int16_t tier; uint8_t claim, stood;
    uint8_t *path; int path_len; lp_rating r; int matches;
} Hold;
Hold *holds_above(const lp_id *keys, int nkeys, int floor, int each, int standing, int *nout);
void holds_free(Hold *h, int n);
int  tier_max(const lp_id *ids, int n);                                 /* the highest tier these IDs are recorded at; -1 when none are */
void   refuse_named(const Firmware *fw);                                 /* the predicates this pass's firmware refuses */
const char *refuse_param(int *len);                                      /* ... as the blake3[] every claim read passes */
void   id_text(const lp_id *id, char out[33]);                        /* 32 hexadecimal digits */
int    id_parse(const char *s, lp_id *out);                         /* the 32 hexadecimal digits back to the ID; 0 when they are not */

/* ---- a file in the DAG (file.c) */
void file_close(File *);
int  source_trunk(const Source *, const File *, int nfiles, Ref *out);  /* the source's trunk: [its record, its files' trunks] */
const char *source_called(const Source *);                            /* its record: its witness, or its called line */
size_t file_record(const Recipe *, const File *, Ref *out, size_t cap);   /* the OS's record of a file, its parts as its recipes dispose of them */
int  file_os(const Recipe *, const File *, Ref *out);                   /* the OS's record of a file as one node, from stat alone */
void file_record_stock(const Recipe *);                                /* the stock recipe that disposes of them where a file's own does not */
int  say_only_disposes(const Recipe *);                                              /* the file is whole: its metadata, its content, its trunk */

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
