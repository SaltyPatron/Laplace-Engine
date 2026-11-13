/* The Syzygy endgame tablebases as a read (chess_tablebase.c): a position's win/draw/loss and distance to zeroing, its
 * legal moves ranked by what each leaves, and the best-play line from it to its end. A verdict is a deterministic read
 * of the tables, never recorded per position; what an ingest records of the tables is a line (recipes/syzygy). Every
 * move, position and ID is chess_rules.c's, so a position probed here is the position a game stands in.
 *
 * The tables are found under a root: root/NAME.rtbw and root/DIR/NAME.rtbw, each with its .rtbz beside it. A table is
 * mapped (os_map_read) the first time a probe needs it, never read whole; at most max_open tables are mapped at once,
 * the least recently used one unmapped when another is needed. A handle is shared by threads. */
#ifndef LAPLACE_CHESS_TABLEBASE_H
#define LAPLACE_CHESS_TABLEBASE_H

#include "chess_rules.h"

typedef struct LpTablebase LpTablebase;

/* The tables under root (NULL: lp_tablebase_root's). NULL when no table is there (said on stderr). max_open <= 0: 64. */
LpTablebase *lp_tablebase_open(const char *root, int max_open);
void lp_tablebase_close(LpTablebase *);
/* The default root: $LAPLACE_SYZYGY, else $LAPLACE_DATA/Games/Chess/syzygy (the syzygy source's root). */
const char *lp_tablebase_root(char *out, size_t cap);
/* One handle for a process's reads (a reader such as Laplace-MCP), opened at the default root the first time. */
LpTablebase *lp_tablebase_shared(void);
typedef struct { int tables, pieces, mapped, peak, max_open; uint64_t maps, unmaps; } LpTablebaseStats;   /* pieces: the most any table holds; peak: the most mapped at once */
void lp_tablebase_stats(LpTablebase *, LpTablebaseStats *);

/* For the side to move: wdl -2 loss, -1 blessed loss (a loss the 50-move rule saves), 0 draw, 1 cursed win (a win it
 * draws), 2 win; dtz the distance to zeroing in plies, as Fathom's probe_dtz gives it (signed for the side to move, over
 * 100 where the 50-move rule turns the result, 0 for a draw). 1 probed; 0 the tables do not cover the position (more
 * pieces than they hold, castling rights, a table missing). */
int lp_tablebase_wdl(LpTablebase *, const Bd *b, int *wdl);
int lp_tablebase_dtz(LpTablebase *, const Bd *b, int *wdl, int *dtz);

/* A legal move as the tables rank it (Fathom's root rule): value is the mover's distance to zeroing through the move
 * (1 a mate or a zeroing move that keeps the win, the position's own distance one ply on otherwise; negative where the
 * mover loses, 0 a draw); wdl and dtz the verdict of the position it makes, for the mover. */
typedef struct { Mv move; int wdl, dtz, value; uint8_t zeroing, mate; } LpTbRanked;
/* Every legal move, best first: the mover's result, then a win's soonest zeroing, a loss's latest, then gen's order.
 * The moves written (at most cap), or -1 when the tables do not cover the position or a move's. */
int lp_tablebase_rank(LpTablebase *, const Bd *b, LpTbRanked *out, int cap);
/* The best-play line from b, the best move each ply (rank's first), to its end: a mate, or a position of the kings
 * alone. moves: written, at most cap. The plies, 0 for a draw (no line), -1 when the tables do not cover a position on
 * it or it does not end within cap. result: the PGN termination for b under the 50-move rule ("1-0", "0-1",
 * "1/2-1/2" for a cursed win or a blessed loss), when the line has plies. */
int lp_tablebase_line(LpTablebase *, const Bd *b, Mv *moves, int cap, char result[8]);
/* The line as PGN, the text the syzygy source records (recipes/syzygy): [FEN "b's four fields"], a blank line, its SAN
 * movetext and its termination, a blank line. out: at least 64 + 12 * plies bytes. The bytes written. */
size_t lp_tablebase_pgn(const Bd *b, const Mv *moves, int plies, const char *result, char *out);

/* ---- for readers: a FEN */
#define LP_TB_MOVES 256
typedef struct { char san[16], uci[8]; lp_id move, position, claim; int wdl, dtz, value; uint8_t zeroing, mate; } LpTbMove;
typedef struct { lp_id position; int covered, wdl, dtz, nmoves; LpTbMove move[LP_TB_MOVES]; } LpTbProbe;
/* The position a FEN gives, its verdict and its legal moves (ranked best first where covered, gen's order where not):
 * each its SAN, UCI, the move's and the position's IDs and the transition's [P, M, P'] claim ID, the ID a game that
 * plays it records. 1 covered, 0 not covered (the moves without verdicts), -1 no FEN. */
int lp_tablebase_probe_fen(LpTablebase *, const char *fen, size_t n, LpTbProbe *out);
typedef struct { char san[16], uci[8]; lp_id move, position, claim; } LpTbPly;
/* The best-play line from a FEN's position: its plies (at most cap), line the ID of [P0, M0, P1, ..., Pn] as a game's
 * line is composed, result its termination. The plies; 0 a draw (line is P0); -1 no FEN or not covered. */
int lp_tablebase_line_fen(LpTablebase *, const char *fen, size_t n, LpTbPly *out, int cap, lp_id *line, char result[8]);

#endif
