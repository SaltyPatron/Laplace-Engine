/* The grammars the engine reads itself, beside the tree-sitter grammars it loads from $LAPLACE_GRAMMARS: each found by
 * the name a recipe's grammar line gives, so a recipe names one as it names any grammar and nothing in recipe.c names
 * a format. A grammar of this kind is a declarative reader of its own: its recipe lines are its, and what each part of
 * a file becomes is said where the grammar is (chess.c: PGN, Laplace-Engine#46). */
#include "engine.h"
#include <string.h>

extern const NativeGrammar chess_pgn;
static const NativeGrammar *const NATIVE[] = { &chess_pgn };

const NativeGrammar *native_grammar(const char *name){
    for (size_t i = 0; i < sizeof NATIVE / sizeof NATIVE[0]; i++) if (!strcmp(NATIVE[i]->name, name)) return NATIVE[i];
    return NULL;
}
