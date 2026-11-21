#ifndef LAPLACE_CHESS_INTERNAL_H
#define LAPLACE_CHESS_INTERNAL_H

#include "query.h"
#include "structure.h"
#include "record_output.h"
#include "blake3.h"
#include "chess_rules.h"

/* A ruleset is a law (chess_rules.h): firmware/chess.firmware declares each, chess_rules.c holds them. */
typedef LpChessLaw Rules;

void lp_chess_rules_load(void);
const Rules *lp_chess_rules_named(const char *v, size_t l);

#define rules_load lp_chess_rules_load
#define rules_named lp_chess_rules_named

#endif
