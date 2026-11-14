/* Chess's rules and its content identities, with nothing of the node table, a recipe or a database (chess_rules.c):
 * the board, its legal moves, SAN, FEN, and the IDs a position, a move and a line are (src/chess.c gives the layout).
 * The ingest (chess.c), the read edge (chess_pgn.c) and every reader that links laplace_engine_reader (Laplace-MCP)
 * use this one implementation, so a position named anywhere has the ID its ingest gave it. */
#ifndef LAPLACE_CHESS_RULES_H
#define LAPLACE_CHESS_RULES_H

#include "laplace/laplace.h"
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum { PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
/* The most legal moves a position has under any law: its pieces' moves and, where pieces are dropped, every piece in
 * hand onto every empty square (5 kinds onto at most 62 squares). Every move list is this long. */
#define LP_CHESS_MOVES 1024
/* A law: the rules a game is played under, as firmware/chess.firmware declares it (Sequence 28, Games). A law governs
 * which moves are legal and what a move does; it is in no position's ID. Standard is all zero but castles. */
typedef struct LpChessLaw {
    char name[64];                 /* as a Variant tag writes it */
    uint8_t castles;               /* castling at all */
    uint8_t c960;                  /* castling as Chess960's rule (which holds Standard's) */
    uint8_t drops;                 /* a piece taken goes to its taker's hand (a promoted piece as a pawn) and is dropped as a move */
    uint8_t explodes;              /* a capture removes the taker and every piece but a pawn next to where it took */
    uint8_t forced;                /* a side that can capture must */
    uint8_t unroyal;               /* the king is a piece as any other: it is never in check */
    uint8_t king_promotes;         /* a pawn may become a king */
    uint8_t first_double;          /* a pawn on its side's first rank steps one or two */
    uint8_t no_check;              /* a move that gives check is not legal */
    uint8_t checks;                /* the checks given that end the game; 0 none */
    uint8_t goal_answered;         /* a White king on the goal ends the game only once Black cannot answer with its own */
    uint64_t goal;                 /* bit s: a king of either side on square s ends the game */
    char start[100];               /* the position it starts from, a FEN; empty: Standard's */
} LpChessLaw;
/* A position: the board, the side to move, castling rights (bit f: the rook on file f), the en passant square, and what
 * a law adds: the pieces in each side's hand (by kind), the squares whose piece was promoted (drops: taken, it goes to
 * hand as a pawn) and the checks each side has given. law: the rules it is played under; NULL is Standard. */
typedef struct { int8_t sq[64]; uint8_t side, rights[2]; int8_t ep; uint8_t c960; uint8_t hand[2][7]; uint8_t checks[2]; uint64_t promoted; const LpChessLaw *law; } Bd;
/* A move: from and to, the piece promoted to, a castling (the king onto its own rook), an en passant capture, or a drop
 * (the kind of piece put from hand onto to; from is to). */
typedef struct { uint8_t from, to, promo, castle, ep, drop; } Mv;

static const char PIECES[] = ".PNBRQK";
static inline int colour_of(int8_t p){ return p > 0 ? 0 : 1; }
static inline int king_sq(const Bd *b, int c){ int8_t k = c ? -KING : KING; for (int s = 0; s < 64; s++) if (b->sq[s] == k) return s; return -1; }
static inline uint32_t piece_cp(int8_t p){ char c = PIECES[abs(p)]; return (uint32_t)(p < 0 ? tolower((unsigned char)c) : c); }

void lp_chess_make(Bd *b, const Mv *m);
int lp_chess_gen(const Bd *b, Mv *out);
void lp_chess_san_of(const Bd *b, const Mv *m, const Mv *all, int nall, int check, char *out);
/* The legal move of all (gen's) a SAN token names, its check or mate mark and suffix annotations (! ?) taken off as
 * written, or -1. The import format's over-specified squares and promotions without = are read too. */
int lp_chess_san_match(const Bd *b, const char *tok, size_t n, const Mv *all, int nall);
void lp_chess_board_start(Bd *b);
int lp_chess_board_fen(Bd *b, const char *fen, size_t n, int c960);
/* The position a law starts from (its start FEN, else Standard's), and a FEN read under a law: its board (a piece
 * marked ~ promoted; the pieces in hand after the board, [inside brackets] or as a ninth rank), side, castling, en
 * passant square, and the checks given (+W+B) or still to give (W+B). 0: it is no FEN. law NULL: Standard. */
int lp_chess_board_law_start(Bd *b, const LpChessLaw *law);
int lp_chess_board_law_fen(Bd *b, const char *fen, size_t n, const LpChessLaw *law);
/* The laws firmware/chess.firmware declares (path), read once; the law a Variant tag names (as written), or the
 * default law when v is NULL; NULL when no law has that name. Thread-safe. A reader that loads none reads
 * LAPLACE_CHESS_FIRMWARE, else chess.firmware beside LAPLACE_FIRMWARE, else knows Standard and Chess960 alone. */
int lp_chess_laws_load(const char *path);
const LpChessLaw *lp_chess_law_named(const char *v, size_t n);
/* What a position holds beyond [board, side, castling, en passant], each present only when there is any: the pieces in
 * hand as FEN writes them (White's Q R B N P, then Black's), the promoted squares in square order, and the checks
 * given as +W+B. A position's ID is [board, side, castling, en passant, these...]. */
typedef struct { uint32_t hand[64]; int nhand; uint8_t promoted[64]; int npromoted; uint32_t checks[16]; int nchecks; } LpChessExtra;
void lp_chess_extra(const Bd *b, LpChessExtra *out);
unsigned lp_chess_castle_mask(const Bd *b, int c);
int lp_chess_ep_square(const Bd *b);
void lp_chess_id_position(const Bd *b, lp_id *out);
void lp_chess_id_move(const Bd *b, const Mv *m, lp_id *out);
/* A position's parts: a square [file, rank]; a castling mask's number, its decimal digits; a piece on a square
 * [piece, square]; the castling [White's mask, Black's mask]; the en passant field, a square or - (ep < 0); the board,
 * its [piece, square]s in square order. id_position is [board, side, castling, en passant] of these. */
void lp_chess_id_square(int s, lp_id *out);
void lp_chess_id_number(unsigned v, lp_id *out);
void lp_chess_id_piece_square(int8_t p, int s, lp_id *out);
void lp_chess_id_castling(unsigned white, unsigned black, lp_id *out);
void lp_chess_id_en_passant(int ep, lp_id *out);
void lp_chess_id_board(const Bd *b, lp_id *out);
/* Whether colour c's king is attacked where it stands. */
int lp_chess_in_check(const Bd *b, int c);
/* A position's FEN, its first four fields (board, side, castling, the en passant square only where a legal capture
 * takes on it, as a position's ID has it), with a promoted piece marked ~ and the hands [inside brackets] where pieces
 * are dropped: what board_law_fen reads back to the same position. out: at least LP_CHESS_FEN bytes. */
#define LP_CHESS_FEN 160
size_t lp_chess_fen_of(const Bd *b, char *out);

/* ---- for readers: a position named by a FEN, and the positions a game plays */
/* The ID of the position a FEN's first four fields give (Standard castling: KQkq the a and h rooks of a king on e).
 * 1, or 0 when it is no FEN. */
int lp_chess_fen_position(const char *fen, size_t n, lp_id *out);
/* Every legal move of the position a FEN gives, in gen's order: its SAN (check and mate marked) and the ID of the
 * position it makes. The moves written (at most cap), or -1 when it is no FEN; 0 is a position with no legal move. */
typedef struct { char san[16]; lp_id position; uint8_t zeroing; } LpChessSuccessor;      /* zeroing: a capture or a pawn move (the 50-move rule starts again) */
int lp_chess_successors(const char *fen, size_t n, lp_id *position, LpChessSuccessor *out, int cap);
/* A game as PGN (tag pairs, movetext, a result) or movetext alone (an opening book's line), parsed by the tree-sitter
 * language given (the engine's grammars pgn or pgn_moves: LAPLACE_GRAMMARS), its main line replayed under the law its
 * Variant tag names (lp_chess_law_named; no tag: the default law, Standard when no laws are loaded) from its FEN tag's
 * position or the law's start: positions[i] the position after ply i (positions[0] the
 * one it starts in), and line the ID of [P0, M0, P1, ..., Pn] (a line of no moves is its position). Variations,
 * comments and NAGs are annotations, not the line. Returns n + 1, the positions written (at most cap; a longer game
 * stops there), or 0 when the text does not parse or a move is not legal where it is played. */
size_t lp_chess_pgn_positions(const void *ts_language, const char *text, size_t n, lp_id *positions, size_t cap, lp_id *line);

#define make lp_chess_make
#define gen lp_chess_gen
#define san_of lp_chess_san_of
#define san_match lp_chess_san_match
#define board_start lp_chess_board_start
#define board_fen lp_chess_board_fen
#define board_law_fen lp_chess_board_law_fen
#define board_law_start lp_chess_board_law_start
#define castle_mask lp_chess_castle_mask
#define ep_square lp_chess_ep_square
#define id_position lp_chess_id_position
#define id_move lp_chess_id_move
#define in_check lp_chess_in_check

#endif
