/* The Engine's reader: the database as the engine speaks to it (pg.c), containment and identity reads (read.c,
 * read_ids.c), entities back to their text (read_text.c). The library Laplace::engine_reader, installed with the
 * Engine package (LaplaceEngine) for the readers that link it (Laplace-MCP); engine.h includes it. */
#ifndef LAPLACE_READER_H
#define LAPLACE_READER_H

#include "laplace/laplace.h"
#include "laplace/consensus.h"
#include <libpq-fe.h>
#include <stdint.h>
#include <stddef.h>
#include "os.h"
#include "pg_plan.h"

typedef struct Reader Reader;

/* Connections (pg.c), the one way a program that links the reader opens the database. */
void db_conninfo_default(const char *conninfo);      /* the program's built-in conninfo, set once at its start */
const char *laplace_db(void);                        /* LAPLACE_CONNINFO, else that default: a libpq connection string */
PGconn *db_connect(const char *conninfo);            /* exits with the server's message if it cannot */
void db_connect_many(const char *conninfo, int n, PGconn **out);   /* n of them, opened together */
const char *db_noted(void);                          /* the conninfo of the last db_connect (or db_note) */
void db_note(const char *conninfo);                  /* note a conninfo the program opened by its own means */
PGconn *db_read(const char *conninfo);               /* db_connect for a read: one world, a repeatable-read snapshot every connection of the command shares */
PGconn *db_try(const char *conninfo);                /* a connection, or NULL when there is none to be had: for what can do without */

/* ---- the database, as the engine speaks to it (pg.c): a statement's parameters, its columns read back, COPY */
void   id_text(const lp_id *id, char out[33]);                        /* 32 hexadecimal digits */
typedef struct { lp_id *id; int n; } Run;                               /* a trajectory's constituents in order, runs written out */
Run run_of(const uint8_t *ewkb, size_t len);                            /* ... from a path as the database sends it (lp_path_ids) */
/* Connection-owned indexed containment through the Native PostgreSQL owner.
 * each = 0: the path holds every key (src is 0). each = 1: the path holds one key, src is that key's place.
 * standing = 1: typed consensus, read by the shared indexed ID frontier. claim is mask bit 0.
 * floor < 0 reads every tier. The caller frees the paths. */
typedef struct {
    lp_id entity; int src; int16_t tier; uint8_t claim, stood;
    uint8_t *path; int path_len; lp_rating r; int64_t matches;
} Hold;
Hold *holds_above(PGconn *pg,const lp_id *keys, int nkeys, int floor, int each, int standing, int *nout);
/* Union of keys with an additional required member; src is -1. */
Hold *holds_pair(PGconn *pg,const lp_id *keys,int nkeys,const lp_id *with,int standing,int *nout);
/* Per-key holders with an additional required member; src is its input position. */
Hold *holds_both(PGconn *pg,const lp_id *keys,int nkeys,const lp_id *with,int *nout);
Hold *holds_any(PGconn *pg,const lp_id *keys,int nkeys,int *nout);
void holds_free(Hold *h,int n);
int hold_order(const void *,const void *);
/* Complete sets or hubs, separately for claims and observations. kinds: 1/2/3.
 * hub[2*k] is the claim overflow; hub[2*k+1] is the observation overflow. */
Hold *holds_capped(PGconn *pg,const lp_id *keys,int nkeys,int cap,int standing,int kinds,int *hub,int *nout);
void coords_of(PGconn *pg,const lp_id *ids,int n,double *out,uint8_t *has);
void tiers_of(PGconn *pg,const lp_id *ids,int n,int *tiers); /* -1 for absent IDs */
Hold *paths_of(PGconn *pg,const lp_id *ids,int n,int *nout);
/* Each recorded ID's first and last constituents and its length (runs written out), without its path: src its place in
 * ids. The caller frees the rows. */
typedef struct { lp_id entity, first, last; int64_t length; size_t src; } Ends;
Ends *ends_of(PGconn *pg,const lp_id *ids,size_t n,size_t *nout);
/* All-member count remains in PostgreSQL. kind: 0 observations, 1 claims, -1 both. */
long holds_count(PGconn *pg,const lp_id *keys,int nkeys,int kind);
/* Up the container index from keys to the files that hold them (read.c): one indexed frontier a level. A holder whose
 * first vertex is the OS's record of a file is that file, and the walk stops there. by = HELD_CONTENT: through every
 * path that holds a node as content, never through a file's record (a name held only as metadata, a file called so, is
 * no occurrence). by = HELD_NAME: through the record only: up from a name to the files whose record holds it, and a file
 * reached through its content is not one. Each node carries the keys below it (bit k: keys[k], for k < 64), propagated
 * up as they arrive, so the lowest node holding a set of keys is their common ancestor. node[k] is keys[k] (the keys are
 * distinct); an edge is parent and child by node index. cap >= 0: a key with more direct holders of a kind than cap is
 * a hub, its bit is set in hub, and its holders of that kind are not walked (cap < 0 walks every key). max_nodes bounds
 * the walk: cut says it stopped there. */
enum { HELD_CONTENT = 0, HELD_NAME = 1 };
typedef struct { lp_id id; uint64_t keys; uint8_t file; } Held;
typedef struct { uint32_t parent, child; uint8_t said; } HeldEdge;   /* said: the child's role in the parent's path (LP_SAID_*) */
typedef struct { Held *node; size_t n; HeldEdge *edge; size_t ne; uint64_t hub; int levels, cut; } HeldUp;
void held_up(PGconn *pg,const lp_id *keys,int nkeys,int by,int cap,size_t max_nodes,HeldUp *out);
void held_up_free(HeldUp *);
/* The same walk by content, read inside PostgreSQL in one call (laplace_held_files): the files that hold the keys,
 * each with the keys below it (bit k: keys[k], k < 64), under trunk when it is given (NULL: any file); and each file's
 * unit, the lowest node of its content holding all of them, with its path length (constituents, runs counted), its tier
 * (height: a whole file's path is only its record and its content) and how many times each key stands below it
 * (count[k], k < 64). cap and max_nodes as held_up's; *hubs, *levels and *cut as
 * it reports them. The caller frees the files. */
typedef struct { lp_id file, unit; uint64_t keys; int64_t length; int tier; uint64_t count[64]; } HeldFile;
HeldFile *held_in(PGconn *pg,const lp_id *trunk,const lp_id *keys,int nkeys,long cap,long max_nodes,
                  uint64_t *hubs,int *levels,int *cut,size_t *nout);
int tier_max(PGconn *pg,const lp_id *ids,int n);
#define ARGS_MAX 12
enum { SQL_TEXT=0, SQL_BIN=1, SQL_ID_ARRAY=2, SQL_NULL=3 }; /* binary payload whose element OID binds at execution */
typedef struct { lp_buf b; int binary; } SqlArg;
typedef struct Args { SqlArg a[ARGS_MAX]; int n; } Args;                    /* zeroed to begin; args_free when done */
void arg_ids(Args *, const lp_id *ids, size_t n);                     /* a blake3[], in binary */
void arg_text(Args *, const char *s);                                 /* a value as text: the server casts it */
void arg_raw(Args *, const void *bytes, size_t n);                    /* bytes already in the binary form */
void args_reset(Args *);                                              /* empty, its buffers kept for the next statement */
void args_free(Args *);
/* One transport grain, independent of admitted set size. Binary ID arrays
 * have a 20-byte header and one length word per fixed-width identity. */
enum { DB_TRANSPORT_BYTES=1u<<22, DB_ID_WINDOW=(DB_TRANSPORT_BYTES-20)/(sizeof(lp_id)+sizeof(uint32_t)) };
/* A column of a row, as the database sends it in binary. */
static inline const lp_id *col_id(const PGresult *q, int r, int c){ return (const lp_id *)PQgetvalue(q, r, c); }
static inline double col_f64(const PGresult *q, int r, int c){ return lp_be_f64(PQgetvalue(q, r, c)); }
static inline lp_path col_path(const PGresult *q, int r, int c){ return lp_path_of((const uint8_t *)PQgetvalue(q, r, c), (size_t)PQgetlength(q, r, c)); }
static inline lp_rating col_rating(const PGresult *q, int r, int c){ lp_rating x = { col_f64(q, r, c), col_f64(q, r, c + 1), col_f64(q, r, c + 2) }; return x; }
/* A COPY ... FROM STDIN (FORMAT binary) begun on the connection: rows with Native's lp_copy_*, flushed as it fills. */
void copy_open(lp_copy *, PGconn *, const char *sql);
void copy_next(lp_copy *);                                          /* finish this stream and enter the next COPY in the same command */
void copy_close(lp_copy *);                                           /* the trailer, the last flush, the server's answer: exits on a failure */
#include "pg_contract.h"

/* ---- a command's options: each its flag, what it holds ('s' a string, 'i' an int, 'u' unsigned, 'l' long long, 'd' a
 * double, 'b' a flag that is there or not, 'v' an int set to val) and where; the list ends with { NULL }. Returns where
 * the command's own arguments begin: at the first that is not one of its options. */
typedef struct { const char *flag; char kind; void *at; int val; } Opt;
int opts(int argc, char **argv, const Opt *o);
typedef lp_vec(lp_id) Ids;
/* A path's constituents in order, its runs written out, into ids: one array kept and refilled row after row. */
static inline size_t path_into(Ids *ids, lp_path p){ size_t n = lp_path_len(p); lp_vec_reserve(ids, n ? n : 1); return ids->n = lp_path_expand(p, ids->v, n); }
/* 32 hexadecimal digits back to the ID, ending the field: 0 when they are not. */
static inline int id_parse(const char *s, lp_id *out){ return lp_id_unhex(s, out) && (s[32] == 0 || s[32] == '\t' || s[32] == '\n'); }

/* Entities back to their text: paths fetched one level at a time for every entity at once, expanded here down to tier 0. */
Reader *reader_new(PGconn *pg);
void    reader_free(Reader *);
void    reader_want(Reader *, const lp_id *id);                       /* queue an entity; fetched by the next reader_text */
void    reader_prefetch(Reader *, const lp_id *ids, size_t count, size_t limit); /* selected display prefixes, one shared frontier at a time */
char   *reader_text(Reader *, const lp_id *id, size_t limit);         /* its text (malloc'd), cut at limit bytes with an ellipsis */
uint64_t reader_trips(const Reader *);                                /* round trips made */
size_t  reader_parts(Reader *, const lp_id *id, lp_id *out, size_t cap); /* an entity's constituents, runs written out; 0 for an atom or nothing recorded */

double now(void);
void  *xrealloc(void *, size_t);

#endif
