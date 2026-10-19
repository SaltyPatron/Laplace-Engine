/* The grammars the engine reads itself, beside the tree-sitter grammars it loads from $LAPLACE_GRAMMARS: each found by
 * the name a recipe's grammar line gives, so a recipe names one as it names any grammar and nothing in recipe.c names
 * a format. A grammar of this kind is a declarative reader of its own: its recipe lines are its, and what each part of
 * a file becomes is said where the grammar is (chess.c: PGN, Laplace-Engine#46). */
#include "engine.h"
#include "structure.h"
#include "os.h"
#include <tree_sitter/api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const NativeGrammar chess_pgn;
static const NativeGrammar *const NATIVE[] = { &chess_pgn };

const NativeGrammar *native_grammar(const char *name){
    for (size_t i = 0; i < sizeof NATIVE / sizeof NATIVE[0]; i++) if (!strcmp(NATIVE[i]->name, name)) return NATIVE[i];
    return NULL;
}

/* ---- Dynamic Tree-sitter grammar registry & thread-local parser pool ---- */
#define MAX_GRAMMARS 512
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[64]; const TSLanguage *lang; } g_reg[MAX_GRAMMARS];
static uint16_t g_nreg = 0;

static const TSLanguage *dl_load_grammar(const char *name){
    const char *dir = laplace_grammars();
    char p[1024], sym[128];
    snprintf(p, sizeof p, "%s/libtree-sitter-%s" OS_DLL_SUFFIX, dir, name);
    snprintf(sym, sizeof sym, "tree_sitter_%s", name);
    void *h = os_dl_open(p);
    if (!h) { fprintf(stderr, "grammar %s: %s\n", name, os_dl_error()); return NULL; }
    const TSLanguage *(*f)(void) = (const TSLanguage *(*)(void))os_dl_sym(h, sym);
    return f ? f() : NULL;
}

uint16_t grammar_id_for(const char *name){
    if (!name || !*name || !strcmp(name, "text") || !strcmp(name, "layout") || native_grammar(name)) return 0;
    pthread_mutex_lock(&g_mu);
    for (uint16_t i = 0; i < g_nreg; i++) {
        if (!strcmp(g_reg[i].name, name)) {
            const TSLanguage *l = g_reg[i].lang;
            pthread_mutex_unlock(&g_mu);
            return l ? (uint16_t)(i + 1) : 0;
        }
    }
    if (g_nreg >= MAX_GRAMMARS - 1) { pthread_mutex_unlock(&g_mu); return 0; }
    uint16_t idx = g_nreg++;
    snprintf(g_reg[idx].name, sizeof g_reg[idx].name, "%s", name);
    g_reg[idx].lang = dl_load_grammar(name);
    const TSLanguage *l = g_reg[idx].lang;
    pthread_mutex_unlock(&g_mu);
    return l ? (uint16_t)(idx + 1) : 0;
}

const TSLanguage *grammar_lang(uint16_t id){
    if (!id || id > g_nreg) return NULL;
    return g_reg[id - 1].lang;
}

static __thread TSParser *tls_parsers[MAX_GRAMMARS];

TSParser *lp_tls_parser(uint16_t grammar_id){
    const TSLanguage *lang = grammar_lang(grammar_id);
    if (!lang) return NULL;
    TSParser *ps = tls_parsers[grammar_id];
    if (!ps) {
        ps = ts_parser_new();
        ts_parser_set_language(ps, lang);
        tls_parsers[grammar_id] = ps;
    } else {
        ts_parser_reset(ps);
        ts_parser_set_included_ranges(ps, NULL, 0);
    }
    return ps;
}
