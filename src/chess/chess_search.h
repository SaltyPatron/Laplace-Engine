#ifndef LAPLACE_CHESS_SEARCH_H
#define LAPLACE_CHESS_SEARCH_H

#include "chess_rules.h"
#include "reader.h"

typedef struct LpChessSearchResult {
    char     best_uci[8];
    char     best_san[16];
    lp_id    position_id;
    lp_id    move_id;
    lp_id    next_position_id;
    lp_id    claim_id;
    int32_t  score_cp;
    int32_t  depth_reached;
    uint64_t nodes_searched;
    uint64_t wall_us;
    uint64_t db_probes;
    int32_t  syzygy_wdl;
    int      from_syzygy;
    int      from_book;
    char     source[24];
} LpChessSearchResult;

int lp_chess_move_uci(const Mv *m, const Bd *b, char out[8]);
int lp_chess_search(PGconn *pg, const char *fen, const char *variant,
                    const lp_id *witnesses, int nwitnesses, int scoped,
                    const LpChessSearchPolicy *policy_override, LpChessSearchResult *out);

#endif
