/* The reader's chess (src/chess_rules.c, built into laplace_engine_reader, the one Laplace-MCP links): the IDs it gives a
 * FEN, and a line of movetext, read by the engine's own grammar (LAPLACE_GRAMMARS: pgn_moves). One input line a query:
 *   fen FEN                    -> the position's ID
 *   MOVETEXT[\tNAME\tECO]      -> line ID \t final position ID [\t record ID]
 * The record ID is what an opening book row of those three fields is (recipes/openings): [line, [P, "opening name",
 * NAME], [P, "Encyclopaedia of Chess Openings code", ECO]], P the position the line ends in; its texts composed by
 * Native's text (LAPLACE_TIER0). tests/opening-lines.sh compares each with the ingest's. */
#include "chess_rules.h"
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LAPLACE_GRAMMARS_DEFAULT
#define LAPLACE_GRAMMARS_DEFAULT ""
#endif

static const void *language(const char *name){
    const char *dir = getenv("LAPLACE_GRAMMARS"); if (!dir || !*dir) dir = LAPLACE_GRAMMARS_DEFAULT;
    char path[1024], symbol[128]; snprintf(path, sizeof path, "%s/libtree-sitter-%s" OS_DLL_SUFFIX, dir, name);
    snprintf(symbol, sizeof symbol, "tree_sitter_%s", name);
    void *library = os_dl_open(path); if (!library) { fprintf(stderr, "chess-positions: %s: %s\n", path, os_dl_error()); exit(2); }
    const void *(*entry)(void) = (const void *(*)(void))os_dl_sym(library, symbol); if (!entry) { fprintf(stderr, "chess-positions: no %s\n", symbol); exit(2); }
    return entry();
}
static void text_id(lp_text *text, const char *s, lp_id *out){ if (!lp_text_id(text, s, strlen(s), out)) { fprintf(stderr, "chess-positions: no text ID for %s\n", s); exit(2); } }

int main(void){
    const void *moves = language("pgn_moves"); lp_text *text = NULL; char input[8192];
    while (fgets(input, sizeof input, stdin)) {
        size_t n = strcspn(input, "\r\n"); input[n] = 0; char hex[33];
        if (!strncmp(input, "fen ", 4)) { lp_id p; if (!lp_chess_fen_position(input + 4, n - 4, &p)) { puts("-"); continue; } lp_id_hex(&p, hex); puts(hex); continue; }
        char *name = strchr(input, '\t'), *eco = NULL; if (name) { *name++ = 0; eco = strchr(name, '\t'); if (eco) *eco++ = 0; }
        lp_id positions[1024], line; size_t count = lp_chess_pgn_positions(moves, input, strlen(input), positions, 1024, &line);
        if (!count) { puts("-"); continue; }
        lp_id_hex(&line, hex); fputs(hex, stdout); lp_id_hex(&positions[count - 1], hex); printf("\t%s", hex);
        if (name && eco) {
            if (!text) { const lp_tier0_record *t0 = lp_tier0_map(NULL); if (!t0 || !(text = lp_text_new(t0))) { fputs("chess-positions: no tier 0\n", stderr); return 2; } }
            lp_id claim[3], record[3];
            claim[0] = positions[count - 1]; text_id(text, "opening name", &claim[1]); text_id(text, name, &claim[2]); lp_id_compose(claim, 3, &record[1]);
            text_id(text, "Encyclopaedia of Chess Openings code", &claim[1]); text_id(text, eco, &claim[2]); lp_id_compose(claim, 3, &record[2]);
            record[0] = line; lp_id r; lp_id_compose(record, 3, &r); lp_id_hex(&r, hex); printf("\t%s", hex); }
        putchar('\n');
    }
    if (text) lp_text_free(text);
    return 0;
}
