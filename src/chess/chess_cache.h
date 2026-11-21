/* The chess perf-cache (chess_cache.c): chess's finite vocabulary as a dense ROM, and the positions and moves the
 * sources named play as a sparse admitted estate, in one Native perf-cache module (laplace/perfcache.h). */
#ifndef LAPLACE_CHESS_CACHE_H
#define LAPLACE_CHESS_CACHE_H

#include "engine.h"
#include "laplace/perfcache.h"

/* Where it is: $LAPLACE_CHESS, else tier 0's path with .chess in place of its ending. */
const char *chess_cache_path(void);
/* The module at chess_cache_path, mapped once and bound to this process's tier 0; NULL when there is no file there. A
 * file that is there and does not load, is not this law's module, or was composed from another tier 0 stops the
 * program: it is never half used and never silently passed over. */
const lp_perfcache *chess_cache(void);
/* The load's database (NULL: the connection's defaults), before anything is decomposed. Which records it already
 * holds is asked once, set-wise, the first time a chess composition wants one; never per row. Not called: nothing is
 * held (a load-free run composes everything). */
void chess_cache_database(const char *conninfo);
/* Whether a composed record of the module, by ID, is held by the load's database, and its closure with it: the
 * top-down prune need not ask. 0 when there is no module or no load, or nothing chess was composed. */
int chess_cache_admitted(const lp_id *id);
/* The record of an already mapped module with this ID as a Ref (its coordinate and tier): 0 when none. */
int chess_cache_ref(const lp_id *id, Ref *out);
/* The vocabulary from the dense ROM by direct address, for a record the load's database holds (or a codepoint): 1 and
 * *out, or 0 to compose it (and so stage it on its first admission). */
int chess_square(int s, Ref *out);
int chess_piece_square(int8_t piece, int s, Ref *out);
int chess_castling(unsigned white, unsigned black, Ref *out);
int chess_en_passant(int ep, Ref *out);

#ifdef LAPLACE_CHESS_RULES_H              /* with the board (chess_rules.h): chess.c and the generator */
int chess_move(const Bd *b, const Mv *m, Ref *out);
/* A generator's tap on the decomposer: each move chess.c replays, the board before and after it, and the position,
 * move and position it composed. NULL but in laplace chess. */
typedef void (*ChessCapture)(const Bd *before, const Mv *m, const Bd *after, const Ref *p, const Ref *move, const Ref *next);
extern ChessCapture chess_capture;
#endif

#endif
