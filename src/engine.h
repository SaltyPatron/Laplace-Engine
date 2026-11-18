/* Laplace-Engine internals: one program and one pipeline for every source. A recipe names how a file decomposes
 * (UAX #29 text, a tree-sitter grammar loaded at run time, or a vocabulary) and which parts of it attest what.
 * Decomposition runs on every core into a table of nodes sharded by ID; claims are compositions like any other; the
 * database receives binary COPY and set-based statements only. Everything shared with the database extension is
 * Laplace-Native's: the engine keeps no copy of it. */
#ifndef LAPLACE_ENGINE_H
#define LAPLACE_ENGINE_H

#include "laplace/laplace.h"
#include "laplace/consensus.h"
#include "input_frame.h"
#include <libpq-fe.h>
#include <stdint.h>
#include <stddef.h>
#include "os.h"                                     /* pthread mutexes and the other POSIX calls, on Windows too */

/* ---- where things are: the environment (laplace.env), else what the engine was built with */
const char *laplace_recipes(void);                   /* LAPLACE_RECIPES */
const char *laplace_grammars(void);                  /* LAPLACE_GRAMMARS */
const char *laplace_ucd(void);                       /* LAPLACE_UCD */
#include "pg_plan.h"

#include "walk.h"                                   /* the reader: reader.h, walk.h (Laplace::engine_reader) */

/* ---- commands */
int cmd_ingest(int argc, char **argv);
int cmd_witness(int argc, char **argv);              /* ingest.c: what a witness named at run time states, through the one ingest */
int cmd_fills(int argc, char **argv);
int cmd_tree(int argc, char **argv);
int cmd_deploy(int argc, char **argv);
int cmd_index(int argc, char **argv);
int cmd_forget(int argc, char **argv);
/* What a source's earlier readings alone held, taken out once a whole new reading is its witness: every witness that
 * begins with the trunk's record (the source's name) but the trunk itself is forgotten as `laplace forget --trunk`. */
int forget_superseded(const char *conninfo, int npg, const lp_id *trunk);
int cmd_sweep(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_sources(int argc, char **argv);
int cmd_tier0(int argc, char **argv);
int cmd_flags(int argc, char **argv);
int cmd_highway(int argc, char **argv);
int cmd_bench(int argc, char **argv);
int cmd_model(int argc, char **argv);
int cmd_held(int argc, char **argv);                                  /* provenance.c: who said a claim, a walk up from it to the trunks that hold it */
int cmd_replay(int argc, char **argv);                                /* provenance.c: standings from containment alone, against the recorded */
int cmd_structure(int argc, char **argv);
int cmd_pgn(int argc, char **argv);                  /* chess.c: a PGN file's records written again from the database alone */
int cmd_tablebase(int argc, char **argv);            /* chess_syzygy.c: a position's tablebase verdict, its moves ranked, its best-play line */
int cmd_chess(int argc, char **argv);                /* chess_cache.c: the chess perf-cache generated, verified or fingerprinted */
const char *ingest_database(void);                   /* the database the ingest running loads into (its -d, else LAPLACE_CONNINFO) */

/* ---- the bits a row must have (physicality.mask): what it is, by LP_KIND_*; as the text of a smallint[] parameter */
#define CLAIM_BITS "{0}"
/* ---- references: an entity's ID, its real coordinate, and its tier */
typedef lp_ref Ref;

/* ---- the node table: compositions keyed by ID, sharded by the ID's first byte, each shard behind its own lock */
typedef struct { lp_id id; uint64_t m; uint32_t spare; } __attribute__((packed)) Vtx;   /* m: M as it is written: the run, and above it what the vertex is and how it was said (lp_m_full); spare: its spare bits (a score) */
#define VRUN(m) ((uint32_t)((m) & ((1ull << LP_M_RUN_BITS) - 1)))
#define VSAID(m) ((uint32_t)(((m) >> LP_M_RUN_BITS) & LP_M_SAID_MASK))
static inline Ref said_claim(Ref r){ r.said = LP_SAID_CLAIM; return r; }
static inline Ref said_tuple(Ref r){ r.said = LP_SAID_TUPLE; return r; }
static inline Ref said_metadata(Ref r){ r.said = LP_SAID_METADATA; return r; }
static inline Ref said_record(Ref r){ if (r.said != LP_SAID_CLAIM && r.said != LP_SAID_TUPLE) r.said = LP_SAID_RECORD; return r; }
typedef struct { lp_id id; int64_t m[4]; uint64_t voff; uint32_t nv, len; uint8_t tier, keep, kind, live, spine, pin; } Node;   /* spine: record-block factoring; pin: a reference surviving the current window */
extern Node *NODE;                                                    /* every node of the table: NODE[0, table_end()), each with live set */
extern Vtx *VTX;                                                      /* a node's vertices: VTX[voff, voff + nv) */
uint64_t table_end(void);
#define TABLE_EACH(x) for (Node *x = NODE, *x##_end = NODE + table_end(); x < x##_end; x++) if (x->live)
extern const lp_tier0_record *T0;
extern const lp_highway *HW;                                          /* the highway, when it is there: types and their mask bits */
Ref    highway_typed(const char *list, const uint8_t *key, size_t n, int *has);   /* existing canonical record: has=1 unique, 0 absent, -1 ambiguous */

void   tier0_open(const char *path);                                  /* maps tier 0 or exits */
void   table_init(void);
void   table_kinds(void);                                             /* after a decomposition: what each child is said to be, on the child */
void   table_annotate_roots(const Ref *, size_t);                    /* retain root roles before their parents can cross a persistence window */
Ref    atom(uint32_t cp);
Ref    compose(const Ref *ch, uint32_t n, uint8_t tier);             /* one child is that child */
uint8_t ref_above(const Ref *r, size_t n);                            /* the tier above the highest of them (255 stays 255) */
Node  *table_find(const lp_id *id);                                   /* NULL for atoms and unknown IDs */
Ref    ref_of_id(const lp_id *id, int *ok);                         /* node, Tier-0 or Highway geometry; ok = 0 when absent */
size_t table_parts(const lp_id *id, lp_id *out, size_t cap);         /* a composition's constituents in order, its runs written out; 0 when it is not in the table */
void   table_size(uint64_t bytes);                                    /* while the table is empty: room for what a batch of this many bytes makes */
uint64_t table_count(void);
uint64_t table_resident_bytes(void);                /* current node/vertex arenas and active hash slots, excluding reserved address space */

/* ---- decomposition (each thread keeps its own working state) */
typedef lp_text Ctx;
extern Ctx **CTX;                                                     /* one per thread */
void   ctx_open(int threads);
Ref    text_ref(Ctx *, const uint8_t *s, size_t n);                  /* UAX #29, recorded in the node table */
Ref    text_occurrences(Ctx *, const uint8_t *s, size_t n, lp_id *out, size_t cap, size_t *count);

/* ---- recipes */
typedef struct TSLanguage TSLanguage;
/* A recipe: which files it reads (match), the grammar or layout that gives each file its tree, and what every named
 * part of that tree is (say.c). Configuration only: there is one decomposer, and no code for a format or a source. */
typedef struct {
    char name[64];
    char match[64][256]; int nmatch;                 /* filename globs; fail explicitly if the declaration exceeds this capacity */
    char interpreter[8][32]; int ninterpreter;       /* the commands a script's #! line names to run what this recipe reads */
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
    void *product;                                    /* compiled format projection, independent of retained packaging */
    const struct NativeGrammar *native;               /* the grammar the recipe names, when it is a reading of the engine's own, not tree-sitter's */
    void *native_recipe;                              /* what that grammar's own lines said */
    InputFrame input_frame;                           /* recipe-declared byte section before grammar interpretation */
    const struct InputDecoder *decoder;               /* input decode NAME: the file is a format the engine writes out as text first */
    size_t dependency_level;                          /* compiled logical reference group, providers before consumers */
    uint8_t program_hash[32]; void *program_state;     /* effective recipe/format source; state exists only while compiling */
} Recipe;
int recipe_product_format(Recipe *, const char *);
void recipe_program_line(Recipe *recipe, const char *path, const char *line);
int program_file_hash(const char *path, uint8_t out[32]);
void grammar_program_hash(uint16_t grammar, uint8_t out[32]);
void say_program_hash(const Recipe *recipe, uint8_t out[32]);
/* A source: a body of content with one identity, however many files it comes in. Its recipes say how each kind of
 * its files reads; the source says who the witness is, where the source is kept, and which sources it comes after. */
typedef struct {
    char name[64];
    char aliases[8][64]; int naliases;                 /* accepted names declared beside the source */
    char witness[128], lineage[128]; double trust;
    char called[128];                                 /* what the source is, where its witness is named file by file (called NAME) */
    char held[256];                                   /* held REASON: a source not here yet, by its keeper's choice; absent, it is said so with the reason and fails no plan */
    char root[8][512]; int nroot;                     /* where it may be kept: the first that exists, newest of a pattern */
    double room;                                      /* what it takes in the database, in times what its files hold, as measured (0: not measured) */
    int tier0;                                        /* it records every codepoint, tier 0 (its source file says tier0): the Unicode source, first */
    char after[16][64]; int nafter;                   /* the sources it comes after */
    char except[8][128]; int nexcept;                 /* files of its roots that are not the source */
    struct { char preferred[128], fallback[128]; } prefer[8]; int nprefer; /* sibling representations, preferred when present */
    char files[16][512]; int nfiles;                  /* the files it is, by pattern, when it is not everything under a root; a directory is every file under it */
    int supersede;                                    /* supersede: a whole reading of it replaces the readings before it (a working tree, not an edition) */
    int sniff;                                        /* a file no pattern matches is read by what it holds, through the recipes it reads */
    char ignore[64];                                  /* ignore NAME: files of that name, found while walking, say in gitignore's notation which files under them are not the source */
    char reads[32][64]; int nreads;                   /* formats its files are read as (recipes that belong to no source) */
    char found[1024];                                 /* where it is */
    int nfound;                                       /* how many directories its root matched: found is the last of them by name */
    char unset[64];                                   /* a variable its root or files name that the environment does not set: it is nowhere until it is */
    int runtime;                                      /* a witness named at run time (laplace witness): no directory of recipes, its files read as any format reads them */
} Source;
int     recipes_load(const char *dir, Recipe **out);
const char *recipes_directory(void);                /* selected estate, including an explicit -r */
int     recipes_catalog(const char *dir, FILE *out, int json);
int     source_named(const Source *source, const char *name);
void    json_str(FILE *out, const char *s);
Source *sources_loaded(int *n);                                       /* in the order they go in */
/* A witness named at run time, after the recipes are loaded: a source of this run alone, its record the witness's name,
 * its trust its class's prior (a class the registry does not declare exits 2), its lineage as given, found at root, its
 * files read as the formats read them. It is in no order and has no directory. The sources are one array: what
 * sources_loaded gave before this is not kept. */
const Source *source_runtime(const char *witness, const char *class_label, const char *lineage, const char *root);
typedef enum { RECIPE_UNCOVERED, RECIPE_OMITTED, RECIPE_COVERED } RecipeCoverage;
Recipe *recipe_select(Recipe *r, int n, const char *path, const Source *of, RecipeCoverage *coverage);
Recipe *recipe_for(Recipe *r, int n, const char *path, const Source *of);   /* of: among that source's recipes only */
int     recipes_broken(const Recipe *r, int n, const Source *of);             /* how many of a source's recipes (NULL: of the formats) did not load; each is said */

/* ---- what a file says, in reading order within each file */
/* What was witnessed is the claim with its specifics; the claim is its main components, and holds the standing. A
 * statement that stands alone is both at once. Every claim is said within a record, and the record is a path over the
 * claims it says, in its file's content tree (say.c, record_of): its provenance is that containment, under the source's
 * trunk, which is the witness. run: how many times the record says it; rec: the node of the file's tree that is its
 * record; witness: who in the record says it (a voice), where the source names one for this statement. */
enum { EV_CLAIM = 0, EV_RECORD = 1, EV_MEMBER = 2 };      /* a claim that is its own record; a record; a claim within the record before it */
typedef struct { lp_id claim, witnessed; float score, enter_rating, enter_deviation; uint32_t position; uint8_t kind, own_witness; lp_id witness; uint32_t run; int32_t rec;
                 lp_id dependence; double dependence_trust; uint8_t has_dependence;
                 lp_id qualifier; uint8_t has_qualifier;
                 uint8_t games; } Event;     /* games: the run is a count the source states (games TIER NAME), so many records; else one record saying it run times */
typedef struct { Event *e; uint64_t n, cap; } Events;
typedef int (*record_batch_fn)(void *, const Ref *, size_t, const Ref *, size_t, const Events *);
struct FileAssembly;

/* A file decomposed: its trunk, and what its recipe's queries attested. */
typedef struct { const char *path; Recipe *recipe; Ref trunk, witness, lineage, packaging; int has_packaging, has_lineage; uint64_t bytes, incomplete, invalid; int exact, skipped, known; RecipeCoverage coverage; Events ev;
                 const Source *source;                 /* the source it is a file of, or NULL for a file given by itself */
                 double trust;                         /* how far its witness is trusted */
                 Ref file; int has_file;               /* the file in the DAG: its trunk, over its metadata and its content */
                 Ref *said; uint64_t nsaid, csaid;     /* what it witnessed, in the order it was read: a curated file's content */
                 int partial;                          /* more of it is still to come: it is not yet a recorded source */
                 uint64_t records;
                 uint32_t order;                       /* its place among the run's files, as ingest orders them */
                 uint64_t at;                          /* where the bytes being read begin in it: a stretch's offset */
                 int laid, bindings_attached;
                 record_batch_fn emit; void *emit_context; struct FileAssembly *assembly;
                 } File;                     /* laid: its trunk, metadata tree and content tree came from the recipe's layout (say.c) */
void file_stream_begin(File *);
void file_stream_add(File *, const Ref *, size_t, const Ref *, size_t, uint64_t);
const Ref *file_stream_roots(File *, size_t *);
void file_stream_finish(File *);
void table_pin(const lp_id *);
int  say_says(Recipe *, const char *path, char *tok);                       /* a recipe's line, if it lays the file out or disposes of a part */
int  say_lays(const Recipe *);
void say_mentions(Recipe *, const char *line);                              /* a recipe's line as written: the parts it names */
int  say_stretches(const Recipe *);                                         /* 1: a file it lays out can be read a line at a time; 2: a record ending at an empty line at a time; 0: whole */
void comment_off(char *line);                                               /* a recipe line without its comment */
const char *say_refers(const Recipe *, int i);                              /* the i-th recipe whose rows' keys this one's parts refer to, or NULL */
/* A source's keys across its files: what a key names, kept for the files read after (table.c). A key defined more than
 * once is kept as the first in the file's order defines it, whichever thread puts it first: its rank is the file's place
 * in the run, the offset of the part of it read, and the unit's place in that part. */
typedef struct { uint64_t file, at, unit, member; } KeyRank;
typedef struct { Ref value; const uint8_t *text; uint32_t length; uint8_t has_value, has_text, blocked; } KeyField;
void keys_domain(const char *recipe, const char *tier, uint8_t key_namespace, lp_id *out); /* complete symbol namespace byte, never an entity */
void keys_field_domain(const lp_id *domain, const char *path, lp_id *out);
void keys_put(const lp_id *domain, const uint8_t *k, size_t n, Ref x, const KeyRank *rank);
int  keys_get(const lp_id *domain, const uint8_t *k, size_t n, Ref *out);
void keys_field_put(const lp_id *domain, const uint8_t *k, size_t n, const KeyField *field, const KeyRank *rank);
const KeyField *keys_field_get(const lp_id *domain, const uint8_t *k, size_t n);
uint64_t keys_revision(void);
/* Compiled lookup metadata: hashes index recipe symbols, never mint content
 * IDs or retain the source's pointer spellings. Export each file's first local
 * declaration, including ones shadowed by another file in this reading. */
#define KEY_BINDING_TAG "compiled reference binding v1"
typedef void (*key_binding_fn)(void *, const Ref *);
size_t keys_file_export(uint64_t file, key_binding_fn, void *);
void keys_restore(const lp_id *hash, const KeyField *, const KeyRank *);
void keys_reader_file(uint64_t file);
void attest_layout(const Recipe *, File *, const uint8_t *src, size_t n);             /* records read so far: a stretch's positions go on from the last */
struct SInput;
int  say_record_streamable(const Recipe *);
int  recipe_record_streamable(const Recipe *);
void attest_input(const Recipe *, File *, const struct SInput *);
typedef int (*source_file_ready_fn)(void *, File *);
void say_source_begin(void);
int say_source_poll(source_file_ready_fn, void *);
int say_source_end(source_file_ready_fn, void *);
int say_has_keys(const Recipe *);
int say_reads_keys(const Recipe *);                                       /* any keyed reference, including this recipe's own declarations */
/* The highway, read from the resources by their recipes (types, keyed, alias, maps lines): what a file says of its
 * types, handed over in the file's order (highway.c keeps them). */
typedef struct Hw Hw;
void say_highway(const Recipe *, File *, const uint8_t *src, size_t n, Hw *);
int  say_has_highway(const Recipe *);
const char *say_relation(const Recipe *, const char *name);                  /* relation "TEXT" NAME: TEXT, or NULL where the recipe declares none of NAME */
int say_games(const Recipe *, const char *name);                             /* games TIER NAME: 1 where the recipe gives NAME as the games of what it attests */
int say_part_native(const Recipe *, const struct NativeGrammar *);           /* 1 where a part of its layout is read by that grammar of the engine's own (part NAME grammar G) */
int  say_thing_parts(const Recipe *, const char *tier, const char *a, const char *b, int *ia, int *ib);     /* the first way it names a TIER by parts A and B: its parts, and where A and B stand; -1 none */
void say_highway_lists(const Recipe *, Hw *);
uint64_t say_obligations(FILE *);                                            /* reports and returns semantic omissions before source publication */
void hw_list(Hw *, const char *list, const char *say);
void hw_type(Hw *, const char *list, const char *say, Ref thing);                 /* a type of the list: the thing it is */
void hw_key(Hw *, const char *list, Ref thing, const char *key);                  /* a resource's key of that type */
void hw_alias(Hw *, const char *list, const char *key, const char *to);           /* a key that names the type another key names */
void hw_edge(Hw *, const char *la, const char *ka, const char *lb, const char *kb);  /* a mapping: the type one key names to the type another names */
void highway_file(Ctx *, File *, Hw *);                                             /* a file read for the highway (recipe.c) */
int  source_files(const Source *, Recipe *rec, int nrec, char ***paths, Recipe ***of);   /* a source's files its recipes read, in the order they are read (ingest.c) */
void decompose_file(Ctx *, File *);
const File *table_reading_file(const File *);                          /* thread-local input context for composition diagnostics; returns previous */
void table_reading_invalid(void);                                    /* prevents a rejected reading becoming a cached file trunk */
void decompose_bytes(Ctx *, File *, uint8_t *src, size_t n, int first);     /* a stretch of a file that is read a stretch at a time */
/* ---- a grammar that is no tree-sitter grammar: a reading the engine holds of its own, found by the name a recipe's
 * grammar line gives (grammars.c lists them). The recipe names it as it names any grammar; recipe.c names none. */
typedef struct NativeGrammar {
    const char *name;                                                        /* what a recipe's grammar line says */
    int  (*says)(Recipe *, const char *path, char *tok);                     /* a line of a recipe of this grammar: 1 said, -1 written wrong */
    int  (*done)(Recipe *, const char *path);                                /* the recipe, read to its end: 1 it loads */
    void (*read)(const Recipe *, File *, const uint8_t *src, size_t n);    /* a file: its records, what they attest, its trunk */
    void (*input)(const Recipe *, File *, const struct SInput *);          /* grammar-bounded windows from a single input stream */
    /* A field of another grammar's file that a recipe parts by this one (part NAME grammar GRAMMAR): the field's text read
     * as this grammar's content, the same content a file of it would hold for that text. 1 read; 0 not this grammar's.
     * Called with no text and no out when the recipe loads: 1 when fields can be read. NULL: it reads files alone.
     * product 0 is that content; product i > 0 is products[i - 1], a part of it the grammar can name (a recipe's
     * thing TIER NAME product PRODUCT): the same reading, another of its products. 0: it has none of that product. */
    int  (*field)(const uint8_t *text, size_t n, unsigned product, Ref *out);
    const char *const *products;                                             /* NULL-terminated; NULL: the content alone */
} NativeGrammar;
/* ---- an input the engine decodes before a recipe's layout reads it (input decode NAME): a format that is not text, whose
 * content the decoder writes out as the text the recipe's tiers part, as zlib inflates a .gz. Found by name, as a
 * native grammar is (grammars.c); the decoder knows its format and nothing of what the recipe says of the parts. */
typedef struct InputDecoder {
    const char *name;
    void  *(*open)(const char *path);                        /* NULL: the file cannot be decoded (said on stderr) */
    size_t (*read)(void *, uint8_t *out, size_t capacity);   /* the text, in order, at most capacity bytes; 0 at its end */
    int    (*error)(void *);                                 /* nonzero: decoding failed (said on stderr) */
    void   (*close)(void *);
} InputDecoder;
const InputDecoder *input_decoder(const char *name);                         /* the decoder of that name, or NULL */
unsigned native_product(const NativeGrammar *, const char *name);            /* a product's number (field's product), or 0: none of that name */
const NativeGrammar *native_grammar(const char *name);                       /* the native grammar of that name, or NULL */
int  reads_in_stretches(const Recipe *, char *boundary);                    /* whether a file of this recipe can be: 1 at a line's end, 2 at an empty line */
void table_reset(void);                                                      /* what was decomposed is recorded: the table is emptied for what comes next */
Ref  string_ref(const uint8_t *s, size_t n);                          /* text as its entity, remembered per thread */
void ev_push(Events *, const Event *);

typedef struct { uint64_t checked, found, rounds, new_nodes, ent_rows, phy_rows, std_new, std_upd, known, publish_waves, publish_lanes, attestations; int attestations_measured; double t_prepare, t_transfer, t_publish, t_index, t_dedup, t_copy, t_sem, t_read, t_play, t_led; } LoadStats;
typedef struct { uint64_t commands, copy_streams; } DbTraffic;
DbTraffic db_traffic(void);
PGresult *db_command(PGconn *, const char *sql);
PGresult *db_command_binary(PGconn *, const char *sql);
int load(const char *conninfo, int npg, File *files, int nfiles, LoadStats *st);
typedef struct { lp_id id, lineage; double trust; int has_lineage; } LoadWitness;
void load_begin(const char *conninfo, int npg);
int load_register(const char *conninfo, int npg, const LoadWitness *, size_t count, const File *, int nfiles);
int files_known(PGconn **pg, int npg, File *files, int nfiles, LoadStats *st);   /* the files whose trunks are recorded: known, saying nothing again */
int ingest_known(const char *conninfo, File *files, int nfiles);                /* the same, on a connection of its own */
/* Window-local observations; PostgreSQL retains private cumulative series.
 * Publish authoritative standings atomically with the source witness (db.c). */
typedef struct Fold Fold;
Fold *fold_new(void);
void  fold_free(Fold *);
uint64_t fold_count(const Fold *);
void  fold_events(Fold *, const File *files, int nfiles);
/* ---- provenance by containment (provenance.c): who said a claim, how many times and how, by a walk up the container
 * index from it to the records, files and trunks that hold it; a trunk in the witness table is a witness. One row a
 * claim and trunk: games (the records under the trunk that say it), tokens (with the times each says it), the sum of
 * their scores, the least position any gave it (0 none), and the trunk's trust. */
Attested *lp_provenance_of(PGconn *, const lp_id *, size_t, int files, int *nout);
int standings_replay(PGconn *pg, const lp_id *claims, size_t n, uint64_t *played, uint64_t *orphaned);   /* their standings again, from the witnesses that hold them */
int db_all_recorded(const char *conninfo, const lp_id *ids, const uint8_t *tiers, uint64_t n);   /* every one of them recorded (in its tier's partition) */
int merge(const char *conninfo, int npg, LoadStats *stats); /* source publication and its measured assertions */
uint8_t *db_recorded(const char *conninfo, const lp_id *ids, uint64_t n);   /* which are recorded, real tables or stage: nothing written */
int cmd_merge(int argc, char **argv);
int cmd_projection(int argc, char **argv);

const char *refuse_param(PGconn *,int *len);                              /* ... as this database's binary blake3[] */
void   arg_refused(Args *);                                              /* ... the same, as a statement's parameter */

/* ---- a file in the DAG (file.c) */
void file_close(File *);                                              /* the file is whole: its metadata, its content, its trunk */
int file_matches_bytes(const lp_id *, const uint8_t *, size_t);
int  source_trunk(const Source *, const File *, int nfiles, Ref *out);  /* the source's trunk: [its record, its files' trunks] */
const char *source_called(const Source *);                            /* its record: its witness, or its called line */
size_t file_record(const Recipe *, const File *, Ref *out, size_t cap);   /* the OS's record of a file, its parts as its recipes dispose of them */
int  file_os(const Recipe *, const File *, Ref *out);                   /* the OS's record of a file as one node, from stat alone */
void file_record_stock(const Recipe *);                                /* the stock recipe that disposes of them where a file's own does not */
int  say_only_disposes(const Recipe *);


#endif
