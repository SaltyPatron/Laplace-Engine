/* The reader's chess (src/chess_rules.c, built into laplace_engine_reader, the one Laplace-MCP links): the IDs it gives a
 * FEN, and a line of movetext, read by the engine's own grammar (LAPLACE_GRAMMARS: pgn_moves). One input line a query:
 *   fen FEN                    -> the position's ID
 *   MOVETEXT[\tNAME\tECO]      -> line ID \t final position ID [\t record ID]
 *   moves FEN                  -> position ID, then \t SAN \t position ID a legal move (lp_chess_successors)
 *   law NAME\tFEN              -> the ID of the position a FEN gives under the law NAME (LAPLACE_CHESS_FIRMWARE)
 *   names                      -> every name a law goes by (lp_chess_law_name), one a line, then .
 *   perft NAME\tDEPTH\tFEN     -> the count of legal move sequences DEPTH plies long from the FEN under the law NAME
 *   game PATH                  -> the ID of the position a PGN file's game ends in, its plies and its result by its
 *                                 law alone (lp_chess_result; * while play goes on), under its Variant's law
 * The record ID is what an opening book row of those three fields is (recipes/openings): [line, [P, "opening name",
 * NAME], [P, "Encyclopaedia of Chess Openings code", ECO]], P the position the line ends in; its texts composed by
 * Native's text (LAPLACE_TIER0). tests/opening-lines.sh compares each with the ingest's. */
#include "chess_rules.h"
#include "laplace/os.h"
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
static uint64_t perft(const Bd *b, int depth){
    if (depth <= 0) return 1; Mv all[LP_CHESS_MOVES]; int n = lp_chess_gen(b, all); if (depth == 1) return (uint64_t)n;
    uint64_t total = 0; for (int i = 0; i < n; i++) { Bd t = *b; lp_chess_make(&t, &all[i]); total += perft(&t, depth - 1); }
    return total;
}
static void text_id(lp_text *text, const char *s, lp_id *out){ if (!lp_text_id(text, s, strlen(s), out)) { fprintf(stderr, "chess-positions: no text ID for %s\n", s); exit(2); } }

int main(void){
    const void *moves = language("pgn_moves"), *games = NULL; lp_text *text = NULL; char input[8192];
    while (fgets(input, sizeof input, stdin)) {
        size_t n = strcspn(input, "\r\n"); input[n] = 0; char hex[33];
        if (!strcmp(input, "names")) {                                      /* names: every name a law goes by, one a line */
            for (int i = 0; lp_chess_law_name(i); i++) puts(lp_chess_law_name(i));
            puts("."); continue; }
        if (!strncmp(input, "law ", 4)) {                                      /* law NAME\tFEN: the position a FEN gives under that law */
            char *fen = strchr(input + 4, '\t'); Bd b; lp_id p; const LpChessLaw *law = fen ? lp_chess_law_named(input + 4, (size_t)(fen - input - 4)) : NULL;
            if (!law || !lp_chess_board_law_fen(&b, fen + 1, strlen(fen + 1), law)) { puts("-"); continue; }
            lp_chess_id_position(&b, &p); lp_id_hex(&p, hex); puts(hex); continue; }
        if (!strncmp(input, "perft ", 6)) {                                    /* perft NAME\tDEPTH\tFEN: the leaves of the legal-move tree */
            char *d = strchr(input + 6, '\t'), *fen = d ? strchr(d + 1, '\t') : NULL; Bd b;
            const LpChessLaw *law = d ? lp_chess_law_named(input + 6, (size_t)(d - input - 6)) : NULL;
            if (!law || !fen || !lp_chess_board_law_fen(&b, fen + 1, strlen(fen + 1), law)) { puts("-"); continue; }
            printf("%llu\n", (unsigned long long)perft(&b, atoi(d + 1))); continue; }
        if (!strncmp(input, "game ", 5)) {                                     /* game PATH: the position a PGN file's first game ends in */
            if (!games) games = language("pgn");
            FILE *f = fopen(input + 5, "rb"); if (!f) { puts("-"); continue; }
            static char pgn[1 << 20]; size_t got = fread(pgn, 1, sizeof pgn - 1, f); fclose(f); pgn[got] = 0;
            static lp_id positions[4096]; Bd end; size_t count = lp_chess_pgn_end(games, pgn, got, positions, 4096, &end);
            if (!count) { puts("-"); continue; }
            static const char *const RESULT[] = { "*", "1-0", "0-1", "1/2-1/2" };
            lp_id_hex(&positions[count - 1], hex); printf("%s\t%zu\t%s\n", hex, count - 1, RESULT[lp_chess_result(&end)]); continue; }
        if (!strncmp(input, "moves ", 6)) {
            LpChessSuccessor next[256]; lp_id p; int k = lp_chess_successors(input + 6, n - 6, &p, next, 256); if (k < 0) { puts("-"); continue; }
            lp_id_hex(&p, hex); fputs(hex, stdout); for (int i = 0; i < k; i++) { lp_id_hex(&next[i].position, hex); printf("\t%s\t%s", next[i].san, hex); } putchar('\n'); continue; }
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
