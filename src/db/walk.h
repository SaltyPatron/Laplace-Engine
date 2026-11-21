/* The witnesses of claims (walk.c), for the readers that name them. */
#ifndef LAPLACE_WALK_H
#define LAPLACE_WALK_H
#include "reader.h"

typedef struct { lp_id id; double trust; int witness, rank; } Root;
void    witnesses_known(PGconn *, Root *, size_t n);                        /* witness and trust, of each that is in the attestation table */
/* A claim as its witnesses hold it, by a walk up the containment index (walk.c): one row a claim and trunk (games,
 * tokens, the sum of their scores, the least position, the trunk's trust; witness: the trunk attests). */
typedef struct { lp_id claim, trunk; double games, tokens, score; uint32_t position; double trust; int witness; } Attested;
Attested *attested(PGconn *pg, const lp_id *claims, int n, int *nout, uint64_t *rows);
typedef struct { lp_id witness; double trust; double fisher_info; uint64_t games; int32_t share_bp; } LpWitnessShare;
int witness_attribution_bp(const Attested *att, int natt, const lp_id *witness_scope, int nscope, int scoped, LpWitnessShare *out, int cap);
#endif
