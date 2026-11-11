/* The witnesses of claims (walk.c), for the readers that name them. */
#ifndef LAPLACE_WALK_H
#define LAPLACE_WALK_H
#include "engine.h"

typedef struct { lp_id id; double trust; int witness, rank; } Root;
void    witnesses_known(PGconn *, Root *, size_t n);                        /* witness and trust, of each that is in the attestation table */
#endif
