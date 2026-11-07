#ifndef LAPLACE_CHESS_INTERNAL_H
#define LAPLACE_CHESS_INTERNAL_H

#include "query.h"
#include "structure.h"
#include "record_output.h"
#include "blake3.h"
#include <ctype.h>

typedef struct { char name[64]; int c960; } Rules;
enum { PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
typedef struct { int8_t sq[64]; uint8_t side, rights[2]; int8_t ep; uint8_t c960; } Bd;
typedef struct { uint8_t from, to, promo, castle, ep; } Mv;

static const char PIECES[] = ".PNBRQK";
static inline int colour_of(int8_t p){ return p > 0 ? 0 : 1; }
static inline int king_sq(const Bd *b, int c){ int8_t k = c ? -KING : KING; for (int s = 0; s < 64; s++) if (b->sq[s] == k) return s; return -1; }
static inline uint32_t piece_cp(int8_t p){ char c = PIECES[abs(p)]; return (uint32_t)(p < 0 ? tolower((unsigned char)c) : c); }

void lp_chess_rules_load(void);
const Rules *lp_chess_rules_named(const char *v, size_t l);
void lp_chess_make(Bd *b, const Mv *m);
int lp_chess_gen(const Bd *b, Mv *out);
void lp_chess_san_of(const Bd *b, const Mv *m, const Mv *all, int nall, int check, char *out);
void lp_chess_board_start(Bd *b);
int lp_chess_board_fen(Bd *b, const char *fen, size_t n, int c960);
unsigned lp_chess_castle_mask(const Bd *b, int c);
int lp_chess_ep_square(const Bd *b);
void lp_chess_id_position(const Bd *b, lp_id *out);
void lp_chess_id_move(const Bd *b, const Mv *m, lp_id *out);

#define rules_load lp_chess_rules_load
#define rules_named lp_chess_rules_named
#define make lp_chess_make
#define gen lp_chess_gen
#define san_of lp_chess_san_of
#define board_start lp_chess_board_start
#define board_fen lp_chess_board_fen
#define castle_mask lp_chess_castle_mask
#define ep_square lp_chess_ep_square
#define id_position lp_chess_id_position
#define id_move lp_chess_id_move

#endif
