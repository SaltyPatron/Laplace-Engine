#ifndef LAPLACE_DB_INTERNAL_H
#define LAPLACE_DB_INTERNAL_H

#include "engine.h"
#include "laplace/tier0.h"
#include "laplace/highway.h"
#include "laplace/geometry.h"
#include <omp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

typedef struct { lp_id claim, witness, lineage; uint64_t games, tokens; double sum, trust; uint32_t position; uint8_t has_lineage, dirty; } Series;
struct Fold { lp_strmap *series; lp_vec(size_t) dirty; };
typedef struct { lp_mask lexical; uint8_t tier,kind,metadata; } Published;
typedef struct {
    PGconn **copy, *series; int ncopy, from_ingest, witness_changed, series_changed, window_index;
    char (*entity)[96], (*path)[96];
    char delta[96], pending[96], witnesses[96], affected[96];
    lp_idmap *known;
    lp_idmap *registered, *new_witnesses;
    int counts_unknown;
} Loader;

static inline void statement(PGconn *pg, const char *sql){ exec(pg, sql); }
static inline bool binding_point(void *context, const lp_ewkb_span *span){
    double point[4]; if (span->count != 1 || !lp_ewkb_vertex(span, 0, point)) return false;
    lp_coord *coord = context;
    for (unsigned d = 0; d < 4; d++) {
        if (!isfinite(point[d]) || fabs(point[d]) > 1.0) return false;
        coord->m[d] = (int64_t)llrint(point[d] * (double)LP_FIXED_ONE);
    }
    return true;
}

#define active_fold lp_db_active_fold
#define writer lp_db_writer
#define start_loader lp_db_start_loader
#define recorded lp_db_recorded_ids
#define flush_series lp_db_flush_series

extern Fold *active_fold;
extern Loader writer;
extern int load_whole;

void start_loader(const char *conninfo, int requested);
uint8_t *recorded(PGconn **pg, int npg, const lp_id *ids, uint64_t n);
void flush_series(Fold *f);

#endif
