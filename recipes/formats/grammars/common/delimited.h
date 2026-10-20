/* Empty fields are legal only at actual container boundaries. A nullable
 * lexical regex can otherwise emit empty tokens forever at a malformed quote.
 * Separator/end tokens reset the state, so EOF can yield an empty field once. */
#include <tree_sitter/parser.h>
#include <stdlib.h>
#ifndef LAPLACE_DELIMITED_REMARK
#define LAPLACE_DELIMITED_REMARK 0
#endif

#define LP_JOIN_(a, b) a##b
#define LP_JOIN(a, b) LP_JOIN_(a, b)
#define LP_SCANNER(name) LP_JOIN(LP_JOIN(tree_sitter_, LAPLACE_DELIMITED_LANGUAGE), name)
enum { LP_EMPTY_FIELD, LP_SEPARATOR, LP_RECORD_END };
typedef struct { bool empty; } LPDelimitedScanner;

void *LP_SCANNER(_external_scanner_create)(void){ return calloc(1, sizeof(LPDelimitedScanner)); }
void LP_SCANNER(_external_scanner_destroy)(void *payload){ free(payload); }
unsigned LP_SCANNER(_external_scanner_serialize)(void *payload, char *out){
    out[0] = ((LPDelimitedScanner *)payload)->empty; return 1;
}
void LP_SCANNER(_external_scanner_deserialize)(void *payload, const char *in, unsigned length){
    ((LPDelimitedScanner *)payload)->empty = length && in[0];
}
bool LP_SCANNER(_external_scanner_scan)(void *payload, TSLexer *lexer, const bool *valid){
    LPDelimitedScanner *state = payload;
    if (valid[LP_SEPARATOR] && lexer->lookahead == LAPLACE_DELIMITED_SEPARATOR) {
        lexer->advance(lexer, false); lexer->mark_end(lexer);
        state->empty = false; lexer->result_symbol = LP_SEPARATOR; return true;
    }
    if (valid[LP_RECORD_END] && (lexer->lookahead == '\r' || lexer->lookahead == '\n')) {
        bool cr = lexer->lookahead == '\r'; lexer->advance(lexer, false);
        if (cr && lexer->lookahead == '\n') lexer->advance(lexer, false);
        lexer->mark_end(lexer); state->empty = false; lexer->result_symbol = LP_RECORD_END; return true;
    }
    if (valid[LP_EMPTY_FIELD] && !state->empty &&
        (lexer->lookahead == LAPLACE_DELIMITED_SEPARATOR || lexer->lookahead == '\r' || lexer->lookahead == '\n' ||
         (LAPLACE_DELIMITED_REMARK && lexer->lookahead == LAPLACE_DELIMITED_REMARK) || lexer->eof(lexer))) {
        lexer->mark_end(lexer); state->empty = true; lexer->result_symbol = LP_EMPTY_FIELD; return true;
    }
    return false;
}
