#ifndef LAPLACE_LITERAL_SQL_H
#define LAPLACE_LITERAL_SQL_H
#include <stdint.h>

/* Offsets into an already lexed SQL token, never statement framing. */
typedef struct { uint32_t begin, end; uint8_t quote, escaped; } SqlLiteral;
int sql_literal_bounds(const uint8_t *, uint32_t, SqlLiteral *);
#endif
