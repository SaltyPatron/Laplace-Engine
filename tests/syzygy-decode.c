/* The Syzygy decoder alone (src/chess_syzygy.c, input decode syzygy): a table decoded to the text a recipe reads, a line
 * a legal position of its material, FEN \t verdict, with no recipe, grammar or database between. The tables a probe
 * reaches are read from the table's own directory, as the ingest reads them.
 *   syzygy_decode TABLE           every line, then the decoder's error state as the exit status
 *   syzygy_decode TABLE LINES     the first LINES lines only (the table opened and its reach loaded all the same), for
 *                                 a table whose every position is too many to read in a test (tests/syzygy-pieces.sh)
 * Exit 0: decoded (every position probed, when read to its end); 1: the table did not open or a probe failed. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const InputDecoder syzygy_decoder;

int main(int argc, char **argv){
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: syzygy_decode TABLE.rtbw|TABLE.rtbz [LINES]\n"); return 2; }
    unsigned long long limit = argc == 3 ? strtoull(argv[2], NULL, 10) : 0, lines = 0;
    void *d = syzygy_decoder.open(argv[1]); if (!d) return 1;
    static uint8_t buf[1 << 16]; size_t n; int partial = 0;
    while (!partial && (n = syzygy_decoder.read(d, buf, sizeof buf)) > 0) {
        size_t keep = n;
        if (limit) for (size_t i = 0; i < n; i++) if (buf[i] == '\n' && ++lines == limit) { keep = i + 1; partial = 1; break; }
        if (fwrite(buf, 1, keep, stdout) != keep) { syzygy_decoder.close(d); return 1; }
    }
    int failed = !partial && syzygy_decoder.error(d);
    syzygy_decoder.close(d);
    return fflush(stdout) || failed;
}
