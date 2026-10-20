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
#ifdef _WIN32
#include <windows.h>
#endif

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
#define LANGUAGE_SLOTS 1024
static struct { uintptr_t language; uint16_t id; } language_index[LANGUAGE_SLOTS];

static size_t language_slot(const TSLanguage *language){
    uintptr_t key = (uintptr_t)language;
    return (size_t)((key >> 4) ^ (key >> 16) ^ (key >> 32)) & (LANGUAGE_SLOTS - 1);
}
static void language_publish(const TSLanguage *language, uint16_t id){
    size_t slot = language_slot(language);
    for (;;) {
        uintptr_t key = __atomic_load_n(&language_index[slot].language, __ATOMIC_ACQUIRE);
        if (key == (uintptr_t)language) return;
        if (!key) {
            language_index[slot].id = id;
            __atomic_store_n(&language_index[slot].language, (uintptr_t)language, __ATOMIC_RELEASE);
            return;
        }
        slot = (slot + 1) & (LANGUAGE_SLOTS - 1);
    }
}
/* Languages are indexed once under g_mu. Record workers only compare pointer
 * keys; they neither acquire that mutex nor resolve grammar names again. */
uint16_t grammar_id_for_language(const TSLanguage *language){
    if (!language) return 0;
    size_t slot = language_slot(language);
    for (;;) {
        uintptr_t key = __atomic_load_n(&language_index[slot].language, __ATOMIC_ACQUIRE);
        if (!key) return 0;
        if (key == (uintptr_t)language) return language_index[slot].id;
        slot = (slot + 1) & (LANGUAGE_SLOTS - 1);
    }
}

static const TSLanguage *dl_load_grammar(const char *name){
    const char *dir = laplace_grammars();
    char p[1024], sym[128];
    snprintf(p, sizeof p, "%s/libtree-sitter-%s" OS_DLL_SUFFIX, dir, name);
    snprintf(sym, sizeof sym, "tree_sitter_%s", name);
    for (char *c = sym; *c; c++) if (*c == '-') *c = '_';
    void *h = os_dl_open(p);
    if (!h) { fprintf(stderr, "grammar %s: %s\n", name, os_dl_error()); return NULL; }
    const TSLanguage *(*f)(void) = (const TSLanguage *(*)(void))os_dl_sym(h, sym);
    if (!f) { fprintf(stderr, "grammar %s: missing entry point %s: %s\n", name, sym, os_dl_error()); return NULL; }
    return f();
}

uint16_t grammar_id_for(const char *name){
    if (!name || !*name || !strcmp(name, "text") || !strcmp(name, "layout")) return 0;
    pthread_mutex_lock(&g_mu);
    for (uint16_t i = 0; i < g_nreg; i++) {
        if (!strcmp(g_reg[i].name, name)) {
            const TSLanguage *l = g_reg[i].lang;
            pthread_mutex_unlock(&g_mu);
            return l ? (uint16_t)(i + 1) : 0;
        }
    }
    if (g_nreg >= MAX_GRAMMARS - 1) { pthread_mutex_unlock(&g_mu); return 0; }
    uint16_t idx = g_nreg;
    snprintf(g_reg[idx].name, sizeof g_reg[idx].name, "%s", name);
    g_reg[idx].lang = dl_load_grammar(name);
    const TSLanguage *l = g_reg[idx].lang;
    __atomic_store_n(&g_nreg, (uint16_t)(idx + 1), __ATOMIC_RELEASE);
    if (l) language_publish(l, (uint16_t)(idx + 1));
    pthread_mutex_unlock(&g_mu);
    return l ? (uint16_t)(idx + 1) : 0;
}

const TSLanguage *grammar_lang(uint16_t id){
    if (!id || id > __atomic_load_n(&g_nreg, __ATOMIC_ACQUIRE)) return NULL;
    return g_reg[id - 1].lang;
}

typedef struct {
    TSParser *parsers[MAX_GRAMMARS];
    TSTreeCursor cursors[MAX_GRAMMARS];
    uint8_t cursor_ready[MAX_GRAMMARS];
    uint16_t cursor_depth;
} ParserState;
static __thread ParserState tls_state;
#ifdef _WIN32
static DWORD parser_key = FLS_OUT_OF_INDEXES;
static INIT_ONCE parser_once = INIT_ONCE_STATIC_INIT;
static void CALLBACK parsers_dispose(void *value){
#else
static pthread_key_t parser_key;
static pthread_once_t parser_once = PTHREAD_ONCE_INIT;
static void parsers_dispose(void *value){
#endif
    ParserState *state = value;
    for (size_t i = 1; i < MAX_GRAMMARS; i++) if (state->parsers[i]) ts_parser_delete(state->parsers[i]);
    for (size_t i = 0; i < MAX_GRAMMARS; i++) if (state->cursor_ready[i]) ts_tree_cursor_delete(&state->cursors[i]);
}
#ifdef _WIN32
static BOOL CALLBACK parser_key_init(PINIT_ONCE once, PVOID argument, PVOID *context){
    (void)once; (void)argument; (void)context;
    parser_key = FlsAlloc(parsers_dispose); return parser_key != FLS_OUT_OF_INDEXES;
}
#else
static void parser_key_init(void){
    if (pthread_key_create(&parser_key, parsers_dispose)) { fputs("grammar: cannot create parser lifetime key\n", stderr); abort(); }
}
#endif

static int parser_lifetime(void){
#ifdef _WIN32
    return InitOnceExecuteOnce(&parser_once, parser_key_init, NULL, NULL) && FlsSetValue(parser_key, &tls_state);
#else
    pthread_once(&parser_once, parser_key_init);
    return !pthread_setspecific(parser_key, &tls_state);
#endif
}

TSTreeCursor *lp_tls_cursor(TSNode root){
    uint16_t slot = tls_state.cursor_depth;
    if (slot == MAX_GRAMMARS || !parser_lifetime()) return NULL;
    TSTreeCursor *cursor = &tls_state.cursors[slot];
    if (tls_state.cursor_ready[slot]) ts_tree_cursor_reset(cursor, root);
    else { *cursor = ts_tree_cursor_new(root); tls_state.cursor_ready[slot] = 1; }
    tls_state.cursor_depth++; return cursor;
}
void lp_tls_cursor_release(TSTreeCursor *cursor){
    if (!tls_state.cursor_depth || cursor != &tls_state.cursors[tls_state.cursor_depth - 1]) {
        fputs("grammar: cursor scopes released out of order\n", stderr); abort();
    }
    tls_state.cursor_depth--;
}

TSParser *lp_tls_parser(uint16_t grammar_id){
    const TSLanguage *lang = grammar_lang(grammar_id);
    if (!lang) return NULL;
    TSParser *ps = tls_state.parsers[grammar_id];
    if (!ps) {
        ps = ts_parser_new();
        if (!ps || !ts_parser_set_language(ps, lang)) {
            if (ps) ts_parser_delete(ps);
            fprintf(stderr, "grammar %u: incompatible Tree-sitter language or exhausted parser memory\n", grammar_id);
            return NULL;
        }
        if (!parser_lifetime()) { ts_parser_delete(ps); return NULL; }
        tls_state.parsers[grammar_id] = ps;
    } else {
        ts_parser_reset(ps);
        ts_parser_set_included_ranges(ps, NULL, 0);
    }
    return ps;
}
