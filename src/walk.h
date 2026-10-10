/* The walks of provenance by containment (walk.c), as provenance.c reads what they found. */
#ifndef LAPLACE_WALK_H
#define LAPLACE_WALK_H
#include "engine.h"

/* ---- a walked DAG: its nodes, and its edges parent -> child with how the parent holds the child */
typedef struct { size_t p, c; uint64_t times; uint32_t outcome, spare, position; uint8_t claim, rec, voiced, said; lp_id voice; size_t in_next; } Edge;
/* claim: the child is a claim the parent says; rec: it stands alone as its own record (times: records), else within a
 * record (times: how many times the record says it) */
typedef struct { lp_id id; int nparents; uint8_t cls, metadata, alone; size_t in_head; } WNode;   /* metadata: its path begins with its metadata (a file, or a trunk) */
typedef struct { lp_idmap *m; WNode *n; size_t nn, cn; Edge *e; size_t ne, ce; } Dag;
#define NONE ((size_t)-1)
typedef struct { double games, tokens, score; uint32_t position; } Tally;
typedef struct { lp_id claim, voice; Tally t; } VTally;                     /* a claim as one voice in its records said it */
typedef struct { lp_id id; double trust; int witness, rank; } Root;
void    dag_free(Dag *);
double *down_from(const Dag *, size_t top);                                 /* how many times each node stands under top; the caller frees it */
void    tally_under(const Dag *, const double *cnt, lp_idmap *per_claim, lp_idmap *per_voice);   /* per_claim holds a Tally, per_voice (or NULL) a VTally */
int     walk_up(Dag *, const lp_id *claims, size_t n, uint64_t *rows);      /* the levels walked; the claims are the Dag's first n nodes */
int     walk_down(PGconn *, const lp_id *top, Dag *, uint64_t *rows);
void    witnesses_known(PGconn *, Root *, size_t n);                        /* witness and trust, of each that is in the witness table */
#endif
