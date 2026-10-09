/* Chess: a PGN file read as what it is (Sequence 27.1-27.3, Corpora: Games, Laplace-Engine#46).
 *
 * A recipe that says "grammar pgn" has its files read here: the PGN grammar (the PGN standard's sections 8.1 and 8.2,
 * tag pairs and movetext) is a declarative reader of its own, and what each part becomes is chess's content, composed
 * as every content is (Storage: Identity): a codepoint's ID is BLAKE3 of its UTF-8 bytes, a composition's ID BLAKE3
 * over its children's IDs in order, a one-child composition its child, and nothing else in any hash.
 *
 *   square       [file, rank]                          two codepoints: e4 is [e, 4]
 *   piece        a codepoint, FEN's letter: K Q R B N P for White, k q r b n p for Black
 *   board        [[piece, square], ...]                the pieces in square order, a1 b1 ... h8
 *   castling     [White's mask, Black's mask]          each one 8-bit mask (27.1): bit 7 - f for file f, so its
 *                                                      binary reads a to h; the king's file is its centre bit and
 *                                                      the rooks' files on either side are where it castles; a
 *                                                      side that cannot castle is 0. A mask is a number, its
 *                                                      decimal digits (137 is a, e and h: [1, 3, 7])
 *   position     [board, side, castling, en passant]   side w or b; en passant the square a pawn can be taken on
 *                                                      by a legal capture this move, else -. Nothing that happened
 *                                                      before the position (its clocks, its move number, how it was
 *                                                      reached) is part of it, and no ruleset is
 *   move         [piece, from, to] or [piece, from, to, promoted piece]; a castling is [king, king's square, its
 *                rook's square], the king onto its own rook, so a Chess960 castling is never a king's step
 *   line         [P0, M0, P1, M1, ..., Pn]             the trajectory, shared by every record that plays it. A line
 *                                                      of no moves is its start position: a one-child composition is
 *                                                      its child, so a game of no moves holds a position, and
 *                                                      nothing makes that position a game (a type is an observation)
 *   record       [line, tags, layer]                   one PGN game as its source wrote it, the occurrence. tags:
 *                                                      [[name, value], ...] in the source's order, each tag a
 *                                                      statement, its value content as written (a player's name is
 *                                                      never folded, nothing is dropped, nothing is hashed into an
 *                                                      identity). layer: the movetext's annotations aligned to the
 *                                                      plies, there only when there are any: for each ply its move,
 *                                                      or [move, annotation...] in the order written, an annotation
 *                                                      a comment's text ([%clk 0:02:59.9]), a NAG as written ($1),
 *                                                      or a variation, the line from the position before the move
 *                                                      (with its own layer: [line, layer]); before the first move,
 *                                                      [P0, comment...]
 * A record whose movetext cannot be replayed under its rules (a variant the rules do not hold, an illegal move) is
 * [tags, movetext], the movetext its text as written: it is kept, and said, never dropped. SAN and PGN are not
 * recorded: they are written again from the positions at the read edge (laplace pgn).
 *
 * What the file attests, its witness being the source's: each record, once (its attestation, position its place in
 * the file); within it, by the kind of record it is:
 *   played    [record, line] and [record, [tag, value]] for each tag, and [record, layer]; a result of * (the game
 *             not finished) claims no outcome
 *   study     a record whose movetext has variations or NAGs, or every record of a recipe that says "records study":
 *             the annotator's testimony, not a game anyone played: [record, [tag, value]], and each NAG as the
 *             annotator's judgment of a move in its position, [position, move, NAG]; no [record, line]
 *   puzzle    every record of a recipe that says "records puzzle": the composer's claimed solution, the line itself,
 *             with [record, [tag, value]] and the NAGs as a study's
 * The rules a record is replayed under are firmware (Sequence 28, Games): firmware/chess.firmware names each variant
 * a Variant tag can name and how it castles; the variant governs which moves are legal and is in no position's ID.
 *
 *   laplace pgn [-d conninfo] [--verify] [-o file] ID...
 *                    a file's (or a record's) PGN written again from the database alone; --verify recomputes every
 *                    ID under it from its constituents with BLAKE3 alone, every leaf a codepoint, and replays every
 *                    line: each position the one its move makes of the one before */
#include "engine.h"
#include "blake3.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- the rules: firmware/chess.firmware */
typedef struct { char name[64]; int c960; } Rules;
static Rules RULES[32]; static int NRULES = -1; static char RDEFAULT[64] = "Standard";
static pthread_mutex_t rules_mu = PTHREAD_MUTEX_INITIALIZER;
static void rules_load(void){
    pthread_mutex_lock(&rules_mu);
    if (NRULES >= 0) { pthread_mutex_unlock(&rules_mu); return; }
    char p[4300]; const char *e = getenv("LAPLACE_CHESS_FIRMWARE");
    if (e && *e) snprintf(p, sizeof p, "%s", e);
    else { snprintf(p, sizeof p, "%s", firmware_path()); char *s = strrchr(p, '/'), *b = strrchr(p, '\\'); if (b > s) s = b; if (s) snprintf(s + 1, sizeof p - (size_t)(s + 1 - p), "chess.firmware"); }
    FILE *f = fopen(p, "r"); int n = 0;
    if (!f) { fprintf(stderr, "chess: %s: no rules (LAPLACE_CHESS_FIRMWARE names them): every record is read under Standard alone\n", p); snprintf(RULES[0].name, 64, "Standard"); NRULES = 1; pthread_mutex_unlock(&rules_mu); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) { comment_off(line); char *c = line; while (*c == ' ' || *c == '\t') c++; if (!*c || *c == '\n' || *c == '\r') continue;
        char name[64] = "", *q;
        if (!strncmp(c, "default", 7)) { c += 7; while (*c == ' ') c++; if (*c == '"') { c++; q = strchr(c, '"'); } else q = c + strcspn(c, " \t\r\n"); if (q) snprintf(RDEFAULT, sizeof RDEFAULT, "%.*s", (int)(q - c), c); continue; }
        if (strncmp(c, "variant", 7)) { fprintf(stderr, "chess: %s: \"%s\" is not something the chess firmware says\n", p, c); continue; }
        c += 7; while (*c == ' ' || *c == '\t') c++;
        if (*c == '"') { c++; q = strchr(c, '"'); if (!q) continue; snprintf(name, sizeof name, "%.*s", (int)(q - c), c); c = q + 1; }
        else { q = c + strcspn(c, " \t\r\n"); snprintf(name, sizeof name, "%.*s", (int)(q - c), c); c = q; }
        int c960 = strstr(c, "castling chess960") != NULL;
        if (n < 32) { snprintf(RULES[n].name, 64, "%s", name); RULES[n++].c960 = c960; } }
    fclose(f); NRULES = n; pthread_mutex_unlock(&rules_mu);
}
static const Rules *rules_named(const char *v, size_t l){
    rules_load(); if (!v) { v = RDEFAULT; l = strlen(v); }
    for (int i = 0; i < NRULES; i++) if (strlen(RULES[i].name) == l && !memcmp(RULES[i].name, v, l)) return &RULES[i];
    return NULL;
}

/* ---- the board */
enum { PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
typedef struct { int8_t sq[64]; uint8_t side, rights[2]; int8_t ep; uint8_t c960; } Bd;   /* sq: +piece White, -piece Black; rights: the files of the rooks each side may castle with; ep: the square a double step passed, or -1 */
typedef struct { uint8_t from, to, promo, castle, ep; } Mv;                                  /* castle: to is the rook's square */
static const char PIECES[] = ".PNBRQK";
static int colour_of(int8_t p){ return p > 0 ? 0 : 1; }
static int king_sq(const Bd *b, int c){ int8_t k = c ? -KING : KING; for (int s = 0; s < 64; s++) if (b->sq[s] == k) return s; return -1; }
static const int KN[8][2] = { {1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2} }, KG[8][2] = { {1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1} };
static const int DIAG[4][2] = { {1,1},{1,-1},{-1,1},{-1,-1} }, ORTH[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };
static int on(int f, int r){ return f >= 0 && f < 8 && r >= 0 && r < 8; }
/* Whether colour by attacks square s. */
static int attacked(const Bd *b, int s, int by){
    int f = s & 7, r = s >> 3, sg = by ? -1 : 1;
    int pr = r - (by ? -1 : 1);                                               /* a pawn of by attacks s from the rank behind it */
    for (int df = -1; df <= 1; df += 2) if (on(f + df, pr) && b->sq[pr * 8 + f + df] == sg * PAWN) return 1;
    for (int i = 0; i < 8; i++) { int ff = f + KN[i][0], rr = r + KN[i][1]; if (on(ff, rr) && b->sq[rr * 8 + ff] == sg * KNIGHT) return 1; }
    for (int i = 0; i < 8; i++) { int ff = f + KG[i][0], rr = r + KG[i][1]; if (on(ff, rr) && b->sq[rr * 8 + ff] == sg * KING) return 1; }
    for (int i = 0; i < 4; i++) for (int ff = f + DIAG[i][0], rr = r + DIAG[i][1]; on(ff, rr); ff += DIAG[i][0], rr += DIAG[i][1]) { int8_t p = b->sq[rr * 8 + ff]; if (!p) continue; if (p == sg * BISHOP || p == sg * QUEEN) return 1; break; }
    for (int i = 0; i < 4; i++) for (int ff = f + ORTH[i][0], rr = r + ORTH[i][1]; on(ff, rr); ff += ORTH[i][0], rr += ORTH[i][1]) { int8_t p = b->sq[rr * 8 + ff]; if (!p) continue; if (p == sg * ROOK || p == sg * QUEEN) return 1; break; }
    return 0;
}
static int in_check(const Bd *b, int c){ int k = king_sq(b, c); return k >= 0 && attacked(b, k, !c); }
static void make(Bd *b, const Mv *m){
    int c = b->side, back = c ? 56 : 0; int8_t p = b->sq[m->from];
    if (m->castle) {
        int ks = m->from, rs = m->to, kingside = (rs & 7) > (ks & 7); int8_t k = b->sq[ks], rk = b->sq[rs];
        b->sq[ks] = 0; b->sq[rs] = 0; b->sq[back + (kingside ? 6 : 2)] = k; b->sq[back + (kingside ? 5 : 3)] = rk;
        b->rights[c] = 0; b->ep = -1; b->side = (uint8_t)!c; return; }
    int8_t cap = b->sq[m->to];
    if (cap && (cap == ROOK || cap == -ROOK) && (m->to >> 3) == (c ? 0 : 7)) b->rights[!c] &= (uint8_t)~(1u << (m->to & 7));   /* a rook taken where it may still castle */
    if (p == KING || p == -KING) b->rights[c] = 0;
    if ((p == ROOK || p == -ROOK) && (m->from & ~7) == back) b->rights[c] &= (uint8_t)~(1u << (m->from & 7));
    b->sq[m->to] = m->promo ? (int8_t)(c ? -m->promo : m->promo) : p; b->sq[m->from] = 0;
    if (m->ep) b->sq[(m->from & ~7) | (m->to & 7)] = 0;
    b->ep = -1; if ((p == PAWN || p == -PAWN) && abs((m->to >> 3) - (m->from >> 3)) == 2) b->ep = (int8_t)((m->from + m->to) / 2);
    b->side = (uint8_t)!c;
}
/* Every legal move, castling as each rook the side may castle with (Chess960's rule holds Standard's). */
static int gen(const Bd *b, Mv *out){
    Mv ps[256]; int n = 0, c = b->side, sg = c ? -1 : 1;
    #define ADD(F, T, PR, CA, EP) do { ps[n].from = (uint8_t)(F); ps[n].to = (uint8_t)(T); ps[n].promo = (uint8_t)(PR); ps[n].castle = (uint8_t)(CA); ps[n].ep = (uint8_t)(EP); n++; } while (0)
    for (int s = 0; s < 64; s++) { int8_t p = b->sq[s]; if (!p || colour_of(p) != c) continue; int a = abs(p), f = s & 7, r = s >> 3;
        if (a == PAWN) { int dr = c ? -1 : 1, last = c ? 0 : 7, start = c ? 6 : 1;
            if (on(f, r + dr) && !b->sq[(r + dr) * 8 + f]) {
                if (r + dr == last) for (int pr = KNIGHT; pr <= QUEEN; pr++) ADD(s, (r + dr) * 8 + f, pr, 0, 0); else ADD(s, (r + dr) * 8 + f, 0, 0, 0);
                if (r == start && !b->sq[(r + 2 * dr) * 8 + f]) ADD(s, (r + 2 * dr) * 8 + f, 0, 0, 0); }
            for (int df = -1; df <= 1; df += 2) { if (!on(f + df, r + dr)) continue; int t = (r + dr) * 8 + f + df;
                if (b->sq[t] && colour_of(b->sq[t]) != c) { if (r + dr == last) for (int pr = KNIGHT; pr <= QUEEN; pr++) ADD(s, t, pr, 0, 0); else ADD(s, t, 0, 0, 0); }
                else if (t == b->ep && !b->sq[t]) ADD(s, t, 0, 0, 1); } }
        else if (a == KNIGHT || a == KING) { const int (*d)[2] = a == KNIGHT ? KN : KG;
            for (int i = 0; i < 8; i++) { int ff = f + d[i][0], rr = r + d[i][1]; if (!on(ff, rr)) continue; int8_t q = b->sq[rr * 8 + ff]; if (q && colour_of(q) == c) continue; ADD(s, rr * 8 + ff, 0, 0, 0); } }
        else { for (int k = 0; k < 2; k++) { if ((k == 0 && a == ROOK) || (k == 1 && a == BISHOP)) continue; const int (*d)[2] = k ? ORTH : DIAG;
                for (int i = 0; i < 4; i++) for (int ff = f + d[i][0], rr = r + d[i][1]; on(ff, rr); ff += d[i][0], rr += d[i][1]) { int8_t q = b->sq[rr * 8 + ff];
                    if (q && colour_of(q) == c) break; ADD(s, rr * 8 + ff, 0, 0, 0); if (q) break; } } } }
    /* castling: the king and its rook on the back rank, the squares each crosses empty but for the two of them, and
     * the king in check on none of the squares from where it stands to where it ends */
    int back = c ? 56 : 0, ks = king_sq(b, c);
    if (ks >= 0 && (ks & ~7) == back && b->rights[c] && !attacked(b, ks, !c))
        for (int rf = 0; rf < 8; rf++) { if (!(b->rights[c] & (1u << rf))) continue; int rs = back + rf; if (b->sq[rs] != sg * ROOK) continue;
            int kingside = rf > (ks & 7), kd = back + (kingside ? 6 : 2), rd = back + (kingside ? 5 : 3), ok = 1;
            int lo = ks < kd ? ks : kd, hi = ks < kd ? kd : ks; for (int s = lo; s <= hi && ok; s++) if (s != ks && s != rs && b->sq[s]) ok = 0;
            lo = rs < rd ? rs : rd; hi = rs < rd ? rd : rs; for (int s = lo; s <= hi && ok; s++) if (s != ks && s != rs && b->sq[s]) ok = 0;
            if (!ok) continue;
            Bd t = *b; t.sq[ks] = 0; t.sq[rs] = 0; lo = ks < kd ? ks : kd; hi = ks < kd ? kd : ks;
            for (int s = lo; s <= hi && ok; s++) if (attacked(&t, s, !c)) ok = 0;
            if (ok) ADD(ks, rs, 0, 1, 0); }
    #undef ADD
    int k = 0; for (int i = 0; i < n; i++) { Bd t = *b; make(&t, &ps[i]); if (!in_check(&t, c)) out[k++] = ps[i]; }
    return k;
}
/* SAN, as the PGN standard writes it (8.2.3), its check or mate mark with it when check is set. */
static void san_of(const Bd *b, const Mv *m, const Mv *all, int nall, int check, char *out){
    char *o = out; int8_t p = b->sq[m->from]; int a = abs(p);
    if (m->castle) o += sprintf(o, (m->to & 7) > (m->from & 7) ? "O-O" : "O-O-O");
    else {
        int cap = b->sq[m->to] != 0 || m->ep;
        if (a == PAWN) { if (cap) { *o++ = (char)('a' + (m->from & 7)); *o++ = 'x'; } }
        else { *o++ = PIECES[a]; int same = 0, file = 0, rank = 0;
            for (int i = 0; i < nall; i++) { const Mv *x = &all[i]; if (x->castle || x->to != m->to || x->from == m->from || abs(b->sq[x->from]) != a) continue;
                same = 1; if ((x->from & 7) == (m->from & 7)) file = 1; if ((x->from >> 3) == (m->from >> 3)) rank = 1; }
            if (same) { if (!file) *o++ = (char)('a' + (m->from & 7)); else if (!rank) *o++ = (char)('1' + (m->from >> 3)); else { *o++ = (char)('a' + (m->from & 7)); *o++ = (char)('1' + (m->from >> 3)); } }
            if (cap) *o++ = 'x'; }
        *o++ = (char)('a' + (m->to & 7)); *o++ = (char)('1' + (m->to >> 3));
        if (m->promo) { *o++ = '='; *o++ = PIECES[m->promo]; } }
    if (check) { Bd t = *b; make(&t, m); if (in_check(&t, t.side)) { Mv r[256]; *o++ = gen(&t, r) ? '+' : '#'; } }
    *o = 0;
}
/* The legal move a SAN token names (its check mark and suffix annotations taken off already), or -1. */
static int san_match(const Bd *b, const char *tok, const Mv *all, int nall){
    char t[32]; size_t l = 0; for (const char *c = tok; *c && l < 31; c++) t[l++] = *c == '0' ? 'O' : *c; t[l] = 0;     /* 0-0: the PGN standard's O-O written with zeros */
    if (!strchr(tok, '-')) for (size_t i = 0; i < l; i++) if (t[i] == 'O') t[i] = '0';
    char s[32];
    for (int i = 0; i < nall; i++) { san_of(b, &all[i], all, nall, 0, s); if (!strcmp(s, t)) return i; }
    /* as the import format allows: the piece's square over-specified, or a promotion without its = */
    int pc = PAWN, ff = -1, fr = -1, to = -1, pr = 0; const char *c = t;
    if (strchr("NBRQK", *c) && *c) { pc = (int)(strchr(PIECES, *c) - PIECES); c++; }
    char sq[8]; int ns = 0; for (const char *d = c; *d && ns < 6; d++) if ((*d >= 'a' && *d <= 'h') || (*d >= '1' && *d <= '8')) sq[ns++] = *d; else if (*d == '=' || *d == 'x' || *d == '-') continue; else if (strchr("NBRQ", *d)) { pr = (int)(strchr(PIECES, *d) - PIECES); break; } else return -1;
    if (ns < 2) return -1; to = (sq[ns - 2] - 'a') + 8 * (sq[ns - 1] - '1'); if (to < 0 || to > 63) return -1;
    for (int k = 0; k < ns - 2; k++) { if (sq[k] >= 'a' && sq[k] <= 'h') ff = sq[k] - 'a'; else fr = sq[k] - '1'; }
    int hit = -1;
    for (int i = 0; i < nall; i++) { const Mv *m = &all[i]; if (m->castle || m->to != to || abs(b->sq[m->from]) != pc || m->promo != pr) continue;
        if (ff >= 0 && (m->from & 7) != ff) continue; if (fr >= 0 && (m->from >> 3) != fr) continue; if (hit >= 0) return -1; hit = i; }
    return hit;
}
static void board_start(Bd *b){
    memset(b, 0, sizeof *b); const char *back = "RNBQKBNR";
    for (int f = 0; f < 8; f++) { int8_t p = (int8_t)(strchr(PIECES, back[f]) - PIECES); b->sq[f] = p; b->sq[56 + f] = (int8_t)-p; b->sq[8 + f] = PAWN; b->sq[48 + f] = -PAWN; }
    b->rights[0] = b->rights[1] = (1u << 0) | (1u << 7); b->ep = -1;
}
/* A FEN (its first four fields): KQkq the outermost rook on that side of the king, a file letter (Shredder-FEN) that
 * file's rook; under Standard's rules only a king on e and rooks on a and h castle. Returns 0 when it is no FEN. */
static int board_fen(Bd *b, const char *fen, size_t n, int c960){
    memset(b, 0, sizeof *b); b->ep = -1; size_t i = 0; int r = 7, f = 0;
    for (; i < n && fen[i] != ' '; i++) { char ch = fen[i];
        if (ch == '/') { if (f != 8) return 0; r--; f = 0; continue; }
        if (ch >= '1' && ch <= '8') { f += ch - '0'; if (f > 8) return 0; continue; }
        const char *pc = strchr(PIECES, toupper((unsigned char)ch)); if (!pc || ch == '.' || f > 7 || r < 0) return 0;
        b->sq[r * 8 + f++] = (int8_t)((pc - PIECES) * (isupper((unsigned char)ch) ? 1 : -1)); }
    if (r != 0 || f != 8) return 0;
    while (i < n && fen[i] == ' ') i++; if (i >= n) return 0; b->side = fen[i] == 'b'; i++;
    while (i < n && fen[i] == ' ') i++;
    for (; i < n && fen[i] != ' '; i++) { char ch = fen[i]; if (ch == '-') continue; int c = islower((unsigned char)ch) ? 1 : 0, back = c ? 56 : 0, ks = king_sq(b, c); if (ks < 0 || (ks & ~7) != back) continue;
        int8_t rk = c ? -ROOK : ROOK; char u = (char)toupper((unsigned char)ch);
        if (u == 'K' || u == 'Q') { int found = -1;
            if (!c960) { int rf = u == 'K' ? 7 : 0; if ((ks & 7) == 4 && b->sq[back + rf] == rk) found = rf; }
            else if (u == 'K') { for (int x = 7; x > (ks & 7); x--) if (b->sq[back + x] == rk) { found = x; break; } }
            else { for (int x = 0; x < (ks & 7); x++) if (b->sq[back + x] == rk) { found = x; break; } }
            if (found >= 0) b->rights[c] |= (uint8_t)(1u << found); }
        else if (u >= 'A' && u <= 'H') { int x = u - 'A'; if (b->sq[back + x] == rk && x != (ks & 7)) b->rights[c] |= (uint8_t)(1u << x); } }
    while (i < n && fen[i] == ' ') i++;
    if (i < n && fen[i] >= 'a' && fen[i] <= 'h' && i + 1 < n && fen[i + 1] >= '1' && fen[i + 1] <= '8') b->ep = (int8_t)((fen[i] - 'a') + 8 * (fen[i + 1] - '1'));
    return 1;
}
/* The castling mask of 27.1: the king's file its centre bit, the rooks' files it castles with on either side; file f
 * is bit 7 - f, so the mask in binary reads a to h. 0: it does not castle. */
static unsigned castle_mask(const Bd *b, int c){
    if (!b->rights[c]) return 0; int ks = king_sq(b, c); unsigned m = 0;
    if (ks >= 0) m |= 0x80u >> (ks & 7); for (int f = 0; f < 8; f++) if (b->rights[c] & (1u << f)) m |= 0x80u >> f;
    return m;
}
/* The en passant square of the position: only where a pawn can take on it by a legal move (otherwise the same
 * position would have two identities). */
static int ep_square(const Bd *b){
    if (b->ep < 0) return -1; Mv m[256]; int n = gen(b, m);
    for (int i = 0; i < n; i++) if (m[i].ep) return b->ep;
    return -1;
}

/* ---- chess as content: the IDs a client computes from tier 0 alone (Native's lp_id_*), and the same as Refs in the
 * node table when a file is read (compose) */
typedef struct { lp_id id[16]; int n; } Kids;
static void id_cp(uint32_t cp, lp_id *out){ lp_id_codepoint(cp, out); }
static void id_number(unsigned v, lp_id *out){ char d[16]; int l = snprintf(d, sizeof d, "%u", v); lp_id k[16]; for (int i = 0; i < l; i++) id_cp((uint32_t)d[i], &k[i]); lp_id_compose(k, (size_t)l, out); }
static void id_square(int s, lp_id *out){ lp_id k[2]; id_cp((uint32_t)('a' + (s & 7)), &k[0]); id_cp((uint32_t)('1' + (s >> 3)), &k[1]); lp_id_compose(k, 2, out); }
static uint32_t piece_cp(int8_t p){ char c = PIECES[abs(p)]; return (uint32_t)(p < 0 ? tolower((unsigned char)c) : c); }
/* A position's ID, computed here without the node table: the same bytes compose() hashes. */
static void id_position(const Bd *b, lp_id *out){
    lp_id ps[64], two[2]; int n = 0;
    for (int s = 0; s < 64; s++) if (b->sq[s]) { id_cp(piece_cp(b->sq[s]), &two[0]); id_square(s, &two[1]); lp_id_compose(two, 2, &ps[n++]); }
    lp_id part[4]; lp_id_compose(ps, (size_t)n, &part[0]); id_cp(b->side ? 'b' : 'w', &part[1]);
    id_number(castle_mask(b, 0), &two[0]); id_number(castle_mask(b, 1), &two[1]); lp_id_compose(two, 2, &part[2]);
    int ep = ep_square(b); if (ep >= 0) id_square(ep, &part[3]); else id_cp('-', &part[3]);
    lp_id_compose(part, 4, out);
}
static void id_move(const Bd *b, const Mv *m, lp_id *out){
    lp_id k[4]; int n = 3; id_cp(piece_cp(b->sq[m->from]), &k[0]); id_square(m->from, &k[1]); id_square(m->to, &k[2]);
    if (m->promo) { id_cp(piece_cp((int8_t)(b->side ? -m->promo : m->promo)), &k[3]); n = 4; }
    lp_id_compose(k, (size_t)n, out);
}

/* ---- reading a file: the node table */
typedef struct { Ref ps[13][64]; uint8_t has[13][64]; Ref sq[64]; uint8_t hsq[64]; } RefCache;      /* within one file's reading: what a batch's node table holds */
static Ref r_sq(RefCache *rc, int s){ if (!rc->hsq[s]) { Ref k[2] = { atom((uint32_t)('a' + (s & 7))), atom((uint32_t)('1' + (s >> 3))) }; rc->sq[s] = compose(k, 2, ref_above(k, 2)); rc->hsq[s] = 1; } return rc->sq[s]; }
static Ref r_number(unsigned v){ char d[16]; int l = snprintf(d, sizeof d, "%u", v); Ref k[16]; for (int i = 0; i < l; i++) k[i] = atom((uint32_t)d[i]); return l == 1 ? k[0] : compose(k, (uint32_t)l, ref_above(k, (size_t)l)); }
static Ref r_position(RefCache *rc, const Bd *b){
    Ref ps[64]; uint32_t n = 0;
    for (int s = 0; s < 64; s++) if (b->sq[s]) { int pi = b->sq[s] + 6; if (!rc->has[pi][s]) { Ref two[2] = { atom(piece_cp(b->sq[s])), r_sq(rc, s) }; rc->ps[pi][s] = compose(two, 2, ref_above(two, 2)); rc->has[pi][s] = 1; } ps[n++] = rc->ps[pi][s]; }
    Ref part[4]; part[0] = compose(ps, n, ref_above(ps, n)); part[1] = atom(b->side ? 'b' : 'w');
    Ref cm[2] = { r_number(castle_mask(b, 0)), r_number(castle_mask(b, 1)) }; part[2] = compose(cm, 2, ref_above(cm, 2));
    int ep = ep_square(b); part[3] = ep >= 0 ? r_sq(rc, ep) : atom('-');
    for (int i = 0; i < 4; i++) part[i].said = 0;
    Ref p = compose(part, 4, ref_above(part, 4)); p.said = 0; return p;
}
static Ref r_move(RefCache *rc, const Bd *b, const Mv *m){
    Ref k[4]; uint32_t n = 3; k[0] = atom(piece_cp(b->sq[m->from])); k[1] = r_sq(rc, m->from); k[2] = r_sq(rc, m->to);
    if (m->promo) { k[3] = atom(piece_cp((int8_t)(b->side ? -m->promo : m->promo))); n = 4; }
    Ref r = compose(k, n, ref_above(k, n)); r.said = 0; return r;
}
static Ref r_text(const uint8_t *s, size_t n){ Ref r = n > 256 ? text_ref(CTX[omp_get_thread_num()], s, n) : string_ref(s, n); r.said = 0; return r; }

/* ---- the PGN grammar: a game's tags and its movetext, the movetext a tree of lines */
typedef struct Line Line;
typedef struct { uint8_t kind; const char *t; uint32_t n; Line *var; } Ann;         /* kind: 1 a comment, 2 a NAG, 3 a variation */
typedef struct { const char *san; uint32_t n; Ann *ann; int nann, cann; } Ply;
struct Line { Ply *p; int n, cap; Ann *lead; int nlead, clead; };
typedef struct { const char *name, *val; uint32_t nl, vl; char *unesc; } Tag;
typedef struct { Tag *tag; int ntag, ctag; Line main; const char *mt; size_t mtn; char result[8]; int has_result; int bad; } Game;
static void ann_push(Ann **a, int *n, int *c, Ann x){ if (*n == *c) { *c = *c ? *c * 2 : 4; *a = xrealloc(*a, sizeof(Ann) * (size_t)*c); } (*a)[(*n)++] = x; }
static void line_free(Line *l){
    for (int i = 0; i < l->n; i++) { for (int j = 0; j < l->p[i].nann; j++) if (l->p[i].ann[j].var) { line_free(l->p[i].ann[j].var); free(l->p[i].ann[j].var); } free(l->p[i].ann); }
    for (int j = 0; j < l->nlead; j++) if (l->lead[j].var) { line_free(l->lead[j].var); free(l->lead[j].var); }
    free(l->p); free(l->lead); memset(l, 0, sizeof *l);
}
static void game_free(Game *g){ for (int i = 0; i < g->ntag; i++) free(g->tag[i].unesc); free(g->tag); line_free(&g->main); memset(g, 0, sizeof *g); }
static int is_result(const char *s, size_t n){ return (n == 3 && (!memcmp(s, "1-0", 3) || !memcmp(s, "0-1", 3))) || (n == 7 && !memcmp(s, "1/2-1/2", 7)) || (n == 1 && *s == '*'); }
/* One game from s[*at]: its tag pairs, then its movetext up to its termination marker. 0: nothing more. */
static int pgn_game(const char *s, size_t n, size_t *at, Game *g){
    memset(g, 0, sizeof *g); size_t i = *at;
    for (;;) {                                                                /* the tag pairs, and what stands around them */
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i < n && s[i] == '%' && (i == 0 || s[i - 1] == '\n')) { while (i < n && s[i] != '\n') i++; continue; }      /* an escaped line (8.2.6): not the game's */
        if (i >= n || s[i] != '[') break;
        size_t a = i + 1; while (a < n && (s[a] == ' ' || s[a] == '\t')) a++; size_t nb = a; while (nb < n && s[nb] != ' ' && s[nb] != '\t' && s[nb] != '"' && s[nb] != ']') nb++;
        size_t q = nb; while (q < n && s[q] != '"' && s[q] != ']' && s[q] != '\n') q++;
        Tag t = { s + a, NULL, (uint32_t)(nb - a), 0, NULL };
        if (q < n && s[q] == '"') { size_t v = q + 1, e = v; int esc = 0; while (e < n && s[e] != '"' && s[e] != '\n') { if (s[e] == '\\' && e + 1 < n) { esc = 1; e++; } e++; }
            if (esc) { t.unesc = malloc(e - v + 1); size_t k = 0; for (size_t z = v; z < e; z++) { if (s[z] == '\\' && z + 1 < e) z++; t.unesc[k++] = s[z]; } t.val = t.unesc; t.vl = (uint32_t)k; }
            else { t.val = s + v; t.vl = (uint32_t)(e - v); }
            q = e; }
        while (q < n && s[q] != ']' && s[q] != '\n') q++; if (q < n && s[q] == ']') q++;
        if (g->ntag == g->ctag) { g->ctag = g->ctag ? g->ctag * 2 : 32; g->tag = xrealloc(g->tag, sizeof(Tag) * (size_t)g->ctag); } g->tag[g->ntag++] = t;
        i = q; }
    /* the movetext: tokens to the termination marker at the outermost level */
    Line *stack[64]; int depth = 0; stack[0] = &g->main; size_t m0 = i; int any = g->ntag > 0;
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '%' && (i == 0 || s[i - 1] == '\n')) { while (i < n && s[i] != '\n') i++; continue; }
        if (c == '[' && depth == 0 && (i == 0 || s[i - 1] == '\n')) break;               /* the next game's tags: this one had no marker */
        any = 1; Line *L = stack[depth];
        if (c == '{' || c == ';') { size_t a = i + 1, e = a; if (c == '{') { while (e < n && s[e] != '}') e++; } else { while (e < n && s[e] != '\n' && s[e] != '\r') e++; }
            Ann x = { 1, s + a, (uint32_t)(e - a), NULL }; if (L->n) ann_push(&L->p[L->n - 1].ann, &L->p[L->n - 1].nann, &L->p[L->n - 1].cann, x); else ann_push(&L->lead, &L->nlead, &L->clead, x);
            i = c == '{' && e < n ? e + 1 : e; continue; }
        if (c == '(') { Line *v = calloc(1, sizeof(Line)); Ann x = { 3, NULL, 0, v };
            if (L->n) ann_push(&L->p[L->n - 1].ann, &L->p[L->n - 1].nann, &L->p[L->n - 1].cann, x); else { g->bad = 1; ann_push(&L->lead, &L->nlead, &L->clead, x); }
            if (depth < 63) stack[++depth] = v; else g->bad = 1; i++; continue; }
        if (c == ')') { if (depth) depth--; else g->bad = 1; i++; continue; }
        size_t a = i; while (i < n && !strchr(" \t\r\n{}();[]", s[i])) i++;
        const char *t = s + a; size_t tl = i - a;
        if (!tl) { i++; continue; }                                            /* a character no token begins with (a stray brace): passed over */
        if (depth == 0 && is_result(t, tl)) { memcpy(g->result, t, tl); g->result[tl] = 0; g->has_result = 1; break; }
        if (*t == '$') { Ann x = { 2, t, (uint32_t)tl, NULL }; if (L->n) ann_push(&L->p[L->n - 1].ann, &L->p[L->n - 1].nann, &L->p[L->n - 1].cann, x); else ann_push(&L->lead, &L->nlead, &L->clead, x); continue; }
        size_t d = 0; while (d < tl && isdigit((unsigned char)t[d])) d++;
        if (d) { size_t e = d; while (e < tl && t[e] == '.') e++; if (e == tl && e > d) continue; if (e > d) { t += e; tl -= e; } else if (d == tl) continue; }   /* a move number, with the move written against it or not */
        while (tl && *t == '.') { t++; tl--; } if (!tl) continue;
        size_t sl = tl; while (sl && (t[sl - 1] == '!' || t[sl - 1] == '?')) sl--;          /* suffix annotations (8.2.3.8): an annotation of the move */
        if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 128; L->p = xrealloc(L->p, sizeof(Ply) * (size_t)L->cap); }
        Ply *p = &L->p[L->n++]; memset(p, 0, sizeof *p); p->san = t; p->n = (uint32_t)sl;
        if (sl < tl) { Ann x = { 2, t + sl, (uint32_t)(tl - sl), NULL }; ann_push(&p->ann, &p->nann, &p->cann, x); } }
    g->mt = s + m0; g->mtn = (i > m0 ? i : m0) - m0;
    if (depth) g->bad = 1;
    *at = i; return any;
}

/* ---- a game composed */
typedef struct { Ref *c; size_t n, cap; } RV;
static void rv(RV *a, Ref x){ if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 64; a->c = xrealloc(a->c, sizeof(Ref) * a->cap); } a->c[a->n++] = x; }
static Ref comp(const Ref *k, size_t n){ Ref x = n == 1 ? k[0] : compose(k, (uint32_t)n, ref_above(k, n)); x.said = 0; return x; }
typedef struct { RefCache *rc; RV nags; long errs; char err[160]; uint64_t plies, castles; } Rd;     /* nags: [position, move, NAG], three Refs each */
/* A line replayed from b0: its trajectory, and its layer where it has annotations (has_layer). 0: a move that is not
 * legal there, said in r->err. */
static int replay(Rd *r, const Line *L, const Bd *b0, Ref *line, Ref *layer, int *has_layer){
    Bd b = *b0; RV traj = { 0 }, lay = { 0 }; int annotated = L->nlead > 0; Ref P = r_position(r->rc, &b); rv(&traj, P);
    if (L->nlead) { RV e = { 0 }; rv(&e, P);
        for (int j = 0; j < L->nlead; j++) { const Ann *x = &L->lead[j]; if (x->kind == 3) continue; rv(&e, r_text((const uint8_t *)x->t, x->n)); }
        rv(&lay, comp(e.c, e.n)); free(e.c); }
    for (int i = 0; i < L->n; i++) { const Ply *p = &L->p[i];
        Mv all[256]; int na = gen(&b, all); char tok[32]; size_t tl = p->n < 31 ? p->n : 31; memcpy(tok, p->san, tl); tok[tl] = 0;
        size_t z = tl; while (z && (tok[z - 1] == '+' || tok[z - 1] == '#')) tok[--z] = 0;
        int k = z ? san_match(&b, tok, all, na) : -1;
        if (k < 0) { snprintf(r->err, sizeof r->err, "%.*s is no legal move at ply %d", (int)p->n, p->san, i + 1); free(traj.c); free(lay.c); return 0; }
        Ref M = r_move(r->rc, &b, &all[k]); Bd before = b; Ref Pb = P; make(&b, &all[k]); P = r_position(r->rc, &b); rv(&traj, M); rv(&traj, P); r->plies++; r->castles += all[k].castle;
        if (!p->nann) { rv(&lay, M); continue; }
        annotated = 1; RV e = { 0 }; rv(&e, M);
        for (int j = 0; j < p->nann; j++) { const Ann *x = &p->ann[j];
            if (x->kind == 1) rv(&e, r_text((const uint8_t *)x->t, x->n));
            else if (x->kind == 2) { Ref nag = r_text((const uint8_t *)x->t, x->n); rv(&e, nag); rv(&r->nags, Pb); rv(&r->nags, M); rv(&r->nags, nag); }
            else if (x->var && x->var->n) { Ref vl, vy; int hv = 0; if (!replay(r, x->var, &before, &vl, &vy, &hv)) { free(e.c); free(traj.c); free(lay.c); return 0; }
                if (hv) { Ref two[2] = { vl, vy }; rv(&e, comp(two, 2)); } else rv(&e, vl); } }
        rv(&lay, comp(e.c, e.n)); free(e.c); }
    *line = comp(traj.c, traj.n); *has_layer = annotated; if (annotated) *layer = comp(lay.c, lay.n);
    free(traj.c); free(lay.c); return 1;
}
static int has_study_marks(const Line *L){
    for (int i = 0; i < L->n; i++) for (int j = 0; j < L->p[i].nann; j++) if (L->p[i].ann[j].kind != 1) return 1;
    for (int j = 0; j < L->nlead; j++) if (L->lead[j].kind != 1) return 1;
    return 0;
}

/* ---- the recipe: grammar pgn, and what its records are */
enum { K_PLAYED = 0, K_STUDY, K_PUZZLE };
typedef struct { int records; } ChessRecipe;
int chess_says(Recipe *r, const char *path, char *tok){
    if (strcmp(r->grammar, "pgn")) return 0;
    if (!r->chess) r->chess = calloc(1, sizeof(ChessRecipe));
    ChessRecipe *c = r->chess;
    if (!strcmp(tok, "records")) { char *v = strtok(NULL, " \t\r\n");
        if (v && !strcmp(v, "played")) c->records = K_PLAYED; else if (v && !strcmp(v, "study")) c->records = K_STUDY; else if (v && !strcmp(v, "puzzle")) c->records = K_PUZZLE;
        else { fprintf(stderr, "%s: records played | study | puzzle\n", path); return -1; }
        return 1; }
    fprintf(stderr, "%s: \"%s\" is not something a PGN recipe says (records)\n", path, tok); return -1;
}
int chess_recipe(Recipe *r, const char *path){
    (void)path; if (!r->chess) r->chess = calloc(1, sizeof(ChessRecipe));
    r->curated = 1; r->lang = NULL; return 1;
}

/* The content a file holds, in its order: blocks factored from the content alone, as say.c's blocks_of does. */
#define BLOCK 4096
static Ref blocks(Ref *r, size_t n){
    if (n == 1) return r[0];
    if (n <= BLOCK) return compose(r, (uint32_t)n, ref_above(r, n));
    Ref *up = malloc(sizeof(Ref) * n); size_t m = 0, from = 0;
    for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, r[i].id.b + 8, 8);
        if ((b & (BLOCK - 1)) == 0 || i == n - 1) { Ref x = i - from + 1 == 1 ? r[from] : compose(r + from, (uint32_t)(i - from + 1), ref_above(r + from, i - from + 1)); if (x.said != LP_SAID_TUPLE && x.said != LP_SAID_CLAIM) x.said = 0; up[m++] = x; from = i + 1; } }
    Ref out = m == n ? compose(r, (uint32_t)n, ref_above(r, n)) : blocks(up, m); free(up); return out;
}
static void member(File *f, Ref rec, Ref *part, int n, uint32_t *pos){
    for (int i = 0; i < n; i++) if (part[i].said != LP_SAID_TUPLE) part[i].said = 0;
    Ref c = n == 1 ? part[0] : compose(part, (uint32_t)n, ref_above(part, (size_t)n)); c = said_claim(c);
    Event e = { c.id, rec.id, 1.0f, 1500.0f, 0.0f, ++*pos, EV_MEMBER, 0, { { 0 } }, 0 }; ev_push(&f->ev, &e);
}
/* A PGN file, read: every game a record, in the file's order; its trunk [the OS's record, its records]. */
void chess_read(const Recipe *rcp, File *f, const uint8_t *src, size_t n){
    const ChessRecipe *cr = rcp->chess; RefCache *rc = calloc(1, sizeof(RefCache)); Rd rd = { rc, { 0 }, 0, "", 0, 0 };
    RV recs = { 0 }; size_t at = 0; uint64_t games = 0, played = 0, study = 0, puzzle = 0, unread = 0, unfinished = 0, c960 = 0, zero = 0, nomarker = 0, mismatch = 0;
    Game g; uint32_t ordinal = 0;
    while (pgn_game((const char *)src, n, &at, &g)) {
        games++; ordinal++;
        const char *variant = NULL, *fen = NULL, *result = NULL; size_t vl = 0, fl = 0, rl = 0;
        for (int i = 0; i < g.ntag; i++) { const Tag *t = &g.tag[i];
            if (t->nl == 7 && !memcmp(t->name, "Variant", 7)) { variant = t->val; vl = t->vl; }
            else if (t->nl == 3 && !memcmp(t->name, "FEN", 3)) { fen = t->val; fl = t->vl; }
            else if (t->nl == 6 && !memcmp(t->name, "Result", 6)) { result = t->val; rl = t->vl; } }
        if (!g.has_result) nomarker++; else if (result && (rl != strlen(g.result) || memcmp(result, g.result, rl))) mismatch++;
        /* the tags: each [name, value] as written; an empty value, the name alone */
        RV tags = { 0 };
        for (int i = 0; i < g.ntag; i++) { const Tag *t = &g.tag[i]; Ref nm = r_text((const uint8_t *)t->name, t->nl);
            if (!t->vl) { rv(&tags, nm); continue; }
            Ref two[2] = { nm, r_text((const uint8_t *)t->val, t->vl) }; Ref pr = compose(two, 2, ref_above(two, 2)); pr.said = LP_SAID_TUPLE; rv(&tags, pr); }
        Ref T = tags.n ? (tags.n == 1 ? tags.c[0] : compose(tags.c, (uint32_t)tags.n, ref_above(tags.c, tags.n))) : atom('-'); T.said = 0;
        const Rules *rules = rules_named(variant, vl);
        Bd b0; int ok = rules != NULL, kind = cr->records == K_PLAYED && has_study_marks(&g.main) ? K_STUDY : cr->records;
        if (ok) { if (fen) ok = board_fen(&b0, fen, fl, rules->c960); else board_start(&b0); }
        if (variant && rules && rules->c960) c960++;
        Ref line, layer; int has_layer = 0; rd.nags.n = 0;
        if (ok && !g.bad) ok = replay(&rd, &g.main, &b0, &line, &layer, &has_layer); else if (ok) { ok = 0; snprintf(rd.err, sizeof rd.err, "its variations are not closed"); }
        else if (!rules) snprintf(rd.err, sizeof rd.err, "its variant %.*s is in no rules (firmware/chess.firmware)", (int)vl, variant); else snprintf(rd.err, sizeof rd.err, "its FEN is no position");
        RV part = { 0 }; Ref rec;
        if (ok) { rv(&part, line); if (tags.n) rv(&part, T); if (has_layer) rv(&part, layer); }
        else { if (tags.n) rv(&part, T); if (g.mtn) rv(&part, r_text((const uint8_t *)g.mt, g.mtn));
               unread++; if (unread <= 5) fprintf(stderr, "\n  %s: game %u is kept as its text and not replayed: %s", f->path, ordinal, rd.err); }
        if (!part.n) { free(part.c); free(tags.c); game_free(&g); continue; }
        rec = comp(part.c, part.n); free(part.c);
        Event e = { rec.id, rec.id, 1.0f, 1500.0f, 0.0f, ordinal, EV_RECORD, 0, { { 0 } }, 0 }; ev_push(&f->ev, &e);
        uint32_t pos = 0; int outcome = !result || !(rl == 1 && *result == '*');
        if (ok && (!g.main.n)) zero++;
        if (!outcome) unfinished++;
        if (ok && kind == K_PLAYED) { Ref p2[2] = { rec, line }; member(f, rec, p2, 2, &pos); played++; }
        if (ok && kind == K_PUZZLE) { Ref p1[1] = { line }; member(f, rec, p1, 1, &pos); puzzle++; }
        if (ok && kind == K_STUDY) study++;
        for (size_t i = 0; i < tags.n; i++) { const Tag *t = &g.tag[i];
            if (!t->vl) continue;                                               /* what a source leaves empty attests nothing */
            if (t->nl == 6 && !memcmp(t->name, "Result", 6) && !outcome) continue;      /* *: no outcome is claimed */
            Ref p2[2] = { rec, tags.c[i] }; member(f, rec, p2, 2, &pos); }
        if (ok && has_layer && kind == K_PLAYED) { Ref p2[2] = { rec, layer }; member(f, rec, p2, 2, &pos); }
        if (ok && kind != K_PLAYED) for (size_t i = 0; i + 2 < rd.nags.n; i += 3) member(f, rec, &rd.nags.c[i], 3, &pos);
        rec.said = LP_SAID_RECORD; rv(&recs, rec); free(tags.c); game_free(&g); }
    f->records = games; f->incomplete += unread;
    if (recs.n) { Ref content = blocks(recs.c, recs.n); content.said = 0; f->trunk = content; }
    f->laid = 0;
    char hex[33] = ""; if (recs.n) lp_id_hex(&f->trunk.id, hex);
    fprintf(stderr, "\n  %s: %llu games: %llu played, %llu studies, %llu puzzles, %llu kept as text; %llu without an outcome (*), %llu of no moves, %llu Chess960; %llu plies, %llu castlings; %llu without a termination marker, %llu whose marker is not their Result; content %s\n",
            f->path, (unsigned long long)games, (unsigned long long)played, (unsigned long long)study, (unsigned long long)puzzle, (unsigned long long)unread, (unsigned long long)unfinished,
            (unsigned long long)zero, (unsigned long long)c960, (unsigned long long)rd.plies, (unsigned long long)rd.castles, (unsigned long long)nomarker, (unsigned long long)mismatch, hex);
    free(recs.c); free(rd.nags.c); free(rc);
}

/* ---- laplace pgn: the read edge. Everything under the IDs named, fetched a level at a time, then written as PGN */
typedef struct { lp_id id; uint32_t first, n; uint8_t mask0, got; } DNode;            /* got: 1 fetched, 2 not recorded */
typedef struct { DNode *v; size_t n, cap; lp_idmap *m; lp_id *kid; size_t nk, ck; } Db;
static int64_t db_find(Db *d, const lp_id *id){ return d->m ? lp_idmap_find(d->m, id) : -1; }
static int64_t db_add(Db *d, const lp_id *id){ if (!d->m) d->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(d->m, id, &fresh); if (!fresh) return (int64_t)i;
    lp_reserve((void **)&d->v, &d->cap, d->n + 1, sizeof(DNode)); DNode *x = &d->v[d->n++]; memset(x, 0, sizeof *x); x->id = *id; return (int64_t)i; }
static int is_cp(const lp_id *id){ return lp_tier0_codepoint(T0, id) >= 0; }
/* Every node under the roots, a level a round trip: its constituents, runs written out, and its mask's first byte. */
static void db_fetch(PGconn *pg, Db *d, const lp_id *roots, size_t nr){
    lp_vec(lp_id) want = { 0 };
    for (size_t i = 0; i < nr; i++) if (!is_cp(&roots[i]) && db_find(d, &roots[i]) < 0) { db_add(d, &roots[i]); lp_push(&want, roots[i]); }
    while (want.n) {
        lp_vec(lp_id) next = { 0 };
        for (size_t o = 0; o < want.n; o += 20000) { size_t k = want.n - o < 20000 ? want.n - o : 20000; Args a = { 0 }; arg_ids(&a, want.v + o, k);
            PGresult *q = ask(pg, "SELECT entity, path, mask FROM physicality WHERE entity = ANY($1::blake3[])", &a);
            for (size_t j = 0; j < k; j++) d->v[db_find(d, &want.v[o + j])].got = 2;
            for (int r = 0; r < PQntuples(q); r++) { int64_t at = db_find(d, col_id(q, r, 0)); if (at < 0) continue;
                lp_path p = col_path(q, r, 1); size_t nv = p.n; lp_vertex *vx = malloc(sizeof(lp_vertex) * (nv ? nv : 1)); lp_path_decode(p, vx, nv);
                size_t tot = 0; for (size_t v = 0; v < nv; v++) tot += vx[v].run;
                lp_reserve((void **)&d->kid, &d->ck, d->nk + tot, sizeof(lp_id));
                d->v[at].first = (uint32_t)d->nk; d->v[at].n = (uint32_t)tot; d->v[at].got = 1;
                const uint8_t *mk = (const uint8_t *)PQgetvalue(q, r, 2); d->v[at].mask0 = PQgetlength(q, r, 2) > 4 ? mk[4] : 0;
                for (size_t v = 0; v < nv; v++) for (uint32_t z = 0; z < vx[v].run; z++) d->kid[d->nk++] = vx[v].id;
                for (size_t v = 0; v < nv; v++) if (!is_cp(&vx[v].id) && db_find(d, &vx[v].id) < 0) { db_add(d, &vx[v].id); lp_push(&next, vx[v].id); }
                free(vx); }
            PQclear(q); args_free(&a); }
        lp_vec_free(&want); want.v = next.v; want.n = next.n; want.cap = next.cap; }
    lp_vec_free(&want);
}
static const DNode *dn(Db *d, const lp_id *id){ int64_t i = db_find(d, id); return i < 0 || d->v[i].got != 1 ? NULL : &d->v[i]; }
static size_t kids(Db *d, const lp_id *id, const lp_id **out){ const DNode *x = dn(d, id); if (!x) { *out = NULL; return 0; } *out = d->kid + x->first; return x->n; }
typedef struct { char *b; size_t n, cap; } Str;
static void sput(Str *s, const char *t, size_t n){ lp_reserve((void **)&s->b, &s->cap, s->n + n + 1, 1); memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0; }
static void sputs(Str *s, const char *t){ sput(s, t, strlen(t)); }
static int text_into(Db *d, const lp_id *id, Str *o, int depth){
    int64_t cp = lp_tier0_codepoint(T0, id); if (cp >= 0) { uint8_t u[4]; size_t l = lp_utf8_put((uint32_t)cp, u); sput(o, (const char *)u, l); return 1; }
    const lp_id *k; size_t n = kids(d, id, &k); if (!n || depth > 64) return 0;
    for (size_t i = 0; i < n; i++) if (!text_into(d, &k[i], o, depth + 1)) return 0;
    return 1;
}
/* a text of one word: a codepoint, or a composition of codepoints alone (a tag's name) */
static int one_word(Db *d, const lp_id *id){ if (is_cp(id)) return 1; const lp_id *k; size_t n = kids(d, id, &k); if (!n) return 0; for (size_t i = 0; i < n; i++) if (!is_cp(&k[i])) return 0; return 1; }
/* A position from its content; 0 when it is none. */
static int pos_decode(Db *d, const lp_id *id, Bd *b){
    const lp_id *k; if (kids(d, id, &k) != 4) return 0; int64_t side = lp_tier0_codepoint(T0, &k[1]); if (side != 'w' && side != 'b') return 0;
    memset(b, 0, sizeof *b); b->side = side == 'b'; b->ep = -1;
    const lp_id *ps; size_t np = kids(d, &k[0], &ps); if (np < 2) return 0;
    for (size_t i = 0; i < np; i++) { const lp_id *two, *sq; if (kids(d, &ps[i], &two) != 2 || kids(d, &two[1], &sq) != 2) return 0; int64_t pc = lp_tier0_codepoint(T0, &two[0]);
        int64_t fch = lp_tier0_codepoint(T0, &sq[0]), rch = lp_tier0_codepoint(T0, &sq[1]); const char *w = pc > 0 && pc < 128 && pc != '.' ? strchr(PIECES, toupper((int)pc)) : NULL;
        if (!w || fch < 'a' || fch > 'h' || rch < '1' || rch > '8') return 0;
        b->sq[(rch - '1') * 8 + (fch - 'a')] = (int8_t)((w - PIECES) * (isupper((int)pc) ? 1 : -1)); }
    const lp_id *cm; if (kids(d, &k[2], &cm) != 2) return 0;
    for (int c = 0; c < 2; c++) { Str t = { 0 }; if (!text_into(d, &cm[c], &t, 0)) { free(t.b); return 0; } unsigned m = (unsigned)atoi(t.b); free(t.b);
        int ks = king_sq(b, c); for (int f = 0; f < 8; f++) if ((m & (0x80u >> f)) && !(ks >= 0 && (ks & 7) == f)) b->rights[c] |= (uint8_t)(1u << f); }
    const lp_id *e; if (kids(d, &k[3], &e) == 2) b->ep = (int8_t)((lp_tier0_codepoint(T0, &e[0]) - 'a') + 8 * (lp_tier0_codepoint(T0, &e[1]) - '1'));
    return 1;
}
/* A line's positions and moves: P0 alone for a line of no moves (it is its start position). */
static size_t line_of(Db *d, const lp_id *id, const lp_id **L, const lp_id **P0){
    Bd x; const lp_id *k; size_t n = kids(d, id, &k);
    if (n == 4 && pos_decode(d, id, &x)) { *L = id; *P0 = id; return 0; }
    *L = k; *P0 = k; return n >= 3 && n % 2 == 1 ? (n - 1) / 2 : 0;
}
static int is_line(Db *d, const lp_id *id){ Bd x; const lp_id *k; size_t n = kids(d, id, &k); if (n == 4 && pos_decode(d, id, &x)) return 1; return n >= 3 && n % 2 == 1 && pos_decode(d, &k[0], &x); }
typedef struct { uint64_t records, zero, plies, replay_bad, ids, id_bad, leaves, leaf_bad, dangling, c960, c960_start, c960_castles, castles, positions, arrivals, transposed, multi, splits, text_kept, nags, comments, variations; } Stat;
static int nag_text(const char *s){ if (*s == '$') { for (s++; *s; s++) if (!isdigit((unsigned char)*s)) return 0; return 1; } if (!*s) return 0; for (; *s; s++) if (*s != '!' && *s != '?') return 0; return 1; }
typedef struct { lp_idmap *first, *edge; lp_strmap *key; } Seen;     /* first: each position's first way in; edge: each way in; key: each position's FEN-like key, to its ID */
static void movetext(Db *d, const lp_id *lineid, const lp_id *layerid, Bd b, int fullmove, Str *o, Stat *st, Seen *sn);
/* A ply's annotations, or a line's before its first move: comments, NAGs and variations as written. Returns whether a
 * comment or a variation stands last (Black's next move is then numbered again). */
static int anns(Db *d, const lp_id *k, size_t n, const lp_id *Pbefore, const Bd *before, int fullmove, Str *o, Stat *st, Seen *sn){
    int brk = 0;
    for (size_t j = 0; j < n; j++) {
        if (is_line(d, &k[j])) { const lp_id *L, *P0; line_of(d, &k[j], &L, &P0);
            if (lp_id_eq(P0, Pbefore)) { sputs(o, " ("); movetext(d, &k[j], NULL, *before, fullmove, o, st, NULL); sputs(o, ")"); brk = 1; st->variations++; continue; } }
        const lp_id *vk; size_t vn = kids(d, &k[j], &vk);
        if (vn == 2 && is_line(d, &vk[0])) { const lp_id *L, *P0; line_of(d, &vk[0], &L, &P0);
            if (lp_id_eq(P0, Pbefore)) { sputs(o, " ("); movetext(d, &vk[0], &vk[1], *before, fullmove, o, st, NULL); sputs(o, ")"); brk = 1; st->variations++; continue; } }
        Str t = { 0 }; text_into(d, &k[j], &t, 0); if (!t.b) { sputs(o, " {}"); brk = 1; continue; }
        if (nag_text(t.b)) { if (*t.b == '$') sputs(o, " "); sputs(o, t.b); st->nags++; }
        else { sputs(o, " {"); sputs(o, t.b); sputs(o, "}"); brk = 1; st->comments++; }
        free(t.b); }
    return brk;
}
static void pos_key(const Bd *b, char *out){
    char *o = out; for (int s = 0; s < 64; s++) *o++ = b->sq[s] ? (char)piece_cp(b->sq[s]) : '.';
    o += sprintf(o, " %c %u %u %d", b->side ? 'b' : 'w', castle_mask(b, 0), castle_mask(b, 1), ep_square(b));
}
/* A line's movetext, its layer's annotations in their places; the line's positions checked against what each move
 * makes of the one before. */
static void movetext(Db *d, const lp_id *lineid, const lp_id *layerid, Bd b, int fullmove, Str *o, Stat *st, Seen *sn){
    const lp_id *L, *P0; size_t nply = line_of(d, lineid, &L, &P0);
    /* the layer: an element a ply, with one before them for what stands before the first move; one element is itself */
    const lp_id *Y = NULL; size_t ny = 0; int lead = 0;
    if (layerid) { const lp_id *yk; size_t yn = kids(d, layerid, &yk);
        if (nply == 0) { Y = layerid; ny = 1; lead = 1; }
        else if (nply == 1) { const lp_id *e0; size_t n0 = yn ? kids(d, &yk[0], &e0) : 0;
            if (yn == 2 && n0 && lp_id_eq(&e0[0], P0)) { Y = yk; ny = 2; lead = 1; } else { Y = layerid; ny = 1; } }
        else { Y = yk; ny = yn; lead = yn == nply + 1; } }
    int brk = 1, first = 1;
    if (lead) { const lp_id *e; size_t ne = kids(d, &Y[0], &e); if (ne > 1) { Str t = { 0 }; anns(d, e + 1, ne - 1, P0, &b, fullmove, &t, st, NULL);
            if (t.n) { size_t sk = t.b[0] == ' '; sput(o, t.b + sk, t.n - sk); first = 0; } free(t.b); } }
    for (size_t i = 0; i < nply; i++) {
        const lp_id *Pb = &L[2 * i], *M = &L[2 * i + 1], *Pa = &L[2 * i + 2];
        const lp_id *mk; size_t nm = kids(d, M, &mk); Mv all[256]; int na = gen(&b, all), hit = -1;
        if (nm >= 3) { const lp_id *f2, *t2; if (kids(d, &mk[1], &f2) == 2 && kids(d, &mk[2], &t2) == 2) {
            int from = (int)(lp_tier0_codepoint(T0, &f2[0]) - 'a') + 8 * (int)(lp_tier0_codepoint(T0, &f2[1]) - '1'), to = (int)(lp_tier0_codepoint(T0, &t2[0]) - 'a') + 8 * (int)(lp_tier0_codepoint(T0, &t2[1]) - '1');
            int promo = 0; if (nm == 4) { int64_t pc = lp_tier0_codepoint(T0, &mk[3]); const char *w = pc > 0 && pc < 128 ? strchr(PIECES, toupper((int)pc)) : NULL; promo = w ? (int)(w - PIECES) : 0; }
            for (int z = 0; z < na; z++) if (all[z].from == from && all[z].to == to && all[z].promo == promo) { hit = z; break; } } }
        if (hit < 0) { st->replay_bad++; sputs(o, " ?"); return; }
        lp_id want; id_move(&b, &all[hit], &want); if (!lp_id_eq(&want, M)) st->replay_bad++;
        char san[16], num[32]; san_of(&b, &all[hit], all, na, 1, san);
        if (b.side == 0) snprintf(num, sizeof num, "%s%d. ", first ? "" : " ", fullmove);
        else if (brk) snprintf(num, sizeof num, "%s%d... ", first ? "" : " ", fullmove);
        else snprintf(num, sizeof num, " ");
        sputs(o, num); sputs(o, san); first = 0; st->plies++;
        if (all[hit].castle) { st->castles++; if (b.c960) st->c960_castles++; }
        Bd before = b; int fm_before = fullmove; make(&b, &all[hit]); if (b.side == 0) fullmove++;
        lp_id pa; id_position(&b, &pa); if (!lp_id_eq(&pa, Pa)) st->replay_bad++;
        if (sn) { lp_id e2[2] = { *Pb, *M }, edge; lp_id_compose(e2, 2, &edge); st->arrivals++;
            bool fresh; lp_id *fst = lp_idmap_get(sn->first, Pa, &fresh);
            if (fresh) { *fst = edge; st->positions++; char key[96]; pos_key(&b, key); bool kf; lp_id *kid = lp_strmap_get(sn->key, key, strlen(key), &kf); if (kf) *kid = *Pa; else if (!lp_id_eq(kid, Pa)) st->splits++; }
            else if (!lp_id_eq(fst, &edge)) { st->transposed++; lp_id pe[2] = { *Pa, edge }, key2; lp_id_compose(pe, 2, &key2); bool f2; lp_idmap_put(sn->edge, &key2, &f2); if (f2) st->multi++; } }
        brk = 0;
        if (Y && i + (size_t)lead >= ny) st->replay_bad++;                     /* a layer shorter than its line */
        else if (Y) { const lp_id *Ai = &Y[i + (size_t)lead]; if (!lp_id_eq(Ai, M)) { const lp_id *e; size_t ne = kids(d, Ai, &e); if (ne > 1 && lp_id_eq(&e[0], M)) brk = anns(d, e + 1, ne - 1, Pb, &before, fm_before, o, st, NULL); else st->replay_bad++; } } }
}
/* A record's tags: [[name, value], ...], one tag its pair, a tag of no value its name alone. */
static void tags_pgn(Db *d, const lp_id *tags, Str *o, char *result, char *fen, char *variant){
    const lp_id *t; size_t nt = kids(d, tags, &t); const lp_id *list = t; size_t nl = nt; lp_id self = *tags;
    if (one_word(d, tags)) { list = &self; nl = 1; }                                   /* one tag, of no value */
    else if (nt == 2 && one_word(d, &t[0])) { list = &self; nl = 1; }                 /* one tag: its pair [name, value] */
    for (size_t i = 0; i < nl; i++) { Str nm = { 0 }, vv = { 0 };
        if (one_word(d, &list[i])) text_into(d, &list[i], &nm, 0);
        else { const lp_id *pv; if (kids(d, &list[i], &pv) == 2) { text_into(d, &pv[0], &nm, 0); text_into(d, &pv[1], &vv, 0); } }
        sputs(o, "["); if (nm.b) sputs(o, nm.b); sputs(o, " \"");
        for (size_t z = 0; z < vv.n; z++) { if (vv.b[z] == '"' || vv.b[z] == '\\') sputs(o, "\\"); sput(o, vv.b + z, 1); }
        sputs(o, "\"]\n");
        if (nm.b && vv.b) { if (!strcmp(nm.b, "Result")) snprintf(result, 16, "%s", vv.b); else if (!strcmp(nm.b, "FEN")) snprintf(fen, 128, "%s", vv.b); else if (!strcmp(nm.b, "Variant")) snprintf(variant, 64, "%s", vv.b); }
        free(nm.b); free(vv.b); }
}
/* A record as PGN: its tags, a blank line, its movetext and its result. */
static void record_pgn(Db *d, const lp_id *rec, Str *o, Stat *st, Seen *sn){
    const lp_id *k; size_t n = kids(d, rec, &k); if (!n) return; st->records++;
    const lp_id *line = NULL, *tags = NULL, *layer = NULL, *text = NULL;
    if (is_line(d, rec)) line = rec;                                                     /* a record of no tags and no annotations is its line */
    else if (is_line(d, &k[0])) { line = &k[0]; if (n > 1) tags = &k[1]; if (n > 2) layer = &k[2]; }
    else { tags = &k[0]; if (n > 1) text = &k[1]; st->text_kept++; }
    char result[16] = "*", fen[128] = "", variant[64] = "";
    if (tags) tags_pgn(d, tags, o, result, fen, variant);
    sputs(o, "\n");
    if (text) { Str t = { 0 }; text_into(d, text, &t, 0); if (t.b) sput(o, t.b, t.n); free(t.b); sputs(o, "\n\n"); return; }
    const Rules *ru = rules_named(variant[0] ? variant : NULL, strlen(variant)); Bd b; int fullmove = 1, c960 = ru && ru->c960;
    if (fen[0]) { board_fen(&b, fen, strlen(fen), c960); const char *sp = fen; for (int f = 0; f < 5 && sp; f++) { sp = strchr(sp, ' '); if (sp) sp++; } if (sp) fullmove = atoi(sp); if (fullmove < 1) fullmove = 1; }
    else board_start(&b);
    b.c960 = (uint8_t)c960;
    const lp_id *L, *P0; size_t nply = line_of(d, line, &L, &P0);
    { lp_id want; id_position(&b, &want); if (!lp_id_eq(&want, P0)) st->replay_bad++; else if (c960 && fen[0]) st->c960_start++; if (c960) st->c960++; }
    if (!nply) st->zero++;
    Str m = { 0 }; movetext(d, line, layer, b, fullmove, &m, st, sn); if (m.b) sput(o, m.b, m.n);
    if (m.n) sputs(o, " "); sputs(o, result); sputs(o, "\n\n"); free(m.b);
}
typedef lp_vec(lp_id) IdVec;
/* The records under a node, in its order: a record is what its mask says one is (LP_KIND_RECORD). */
static void records_in(Db *d, const lp_id *id, IdVec *out, int depth){
    const DNode *x = dn(d, id); if (!x || depth > 40) return;
    if (x->mask0 & (0x80u >> LP_KIND_RECORD)) { lp_push(out, *id); return; }
    const lp_id *k; size_t n = kids(d, id, &k);
    for (size_t i = 0; i < n; i++) records_in(d, &k[i], out, depth + 1);
}
/* --verify: every composition under the roots recomputed with BLAKE3 alone from its constituents' IDs, and every leaf
 * a codepoint whose ID is BLAKE3 of its UTF-8 bytes: nothing else is in any ID. */
static void verify_ids(Db *d, Stat *st){
    for (size_t i = 0; i < d->n; i++) { const DNode *x = &d->v[i]; if (x->got != 1) { st->dangling++; continue; } st->ids++;
        lp_id h; blake3_hasher bh; blake3_hasher_init(&bh); blake3_hasher_update(&bh, d->kid + x->first, 16 * (size_t)x->n); blake3_hasher_finalize(&bh, h.b, 16);
        if (x->n < 2 || !lp_id_eq(&h, &x->id)) st->id_bad++;                          /* a one-child composition is its child: never a node of its own */
        for (uint32_t k = 0; k < x->n; k++) { const lp_id *c = &d->kid[x->first + k]; int64_t cp = lp_tier0_codepoint(T0, c); if (cp < 0) continue; st->leaves++;
            uint8_t u[4]; size_t l = lp_utf8_put((uint32_t)cp, u); blake3_hasher_init(&bh); blake3_hasher_update(&bh, u, l); blake3_hasher_finalize(&bh, h.b, 16); if (!lp_id_eq(&h, c)) st->leaf_bad++; } }
}
int cmd_pgn(int argc, char **argv){
    const char *conninfo = laplace_db(), *outp = NULL, *t0p = NULL; int verify = 0;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "-t", 's', &t0p }, { "-o", 's', &outp }, { "--verify", 'b', &verify }, { NULL } });
    if (a >= argc) { fprintf(stderr, "usage: laplace pgn [-d conninfo] [-o file] [--verify] ID...   (a file's, a record's or a line's ID)\n"); return 2; }
    tier0_open(t0p); rules_load();
    IdVec roots = { 0 }; for (int i = a; i < argc; i++) { lp_id x; if (!id_parse(argv[i], &x)) { fprintf(stderr, "%s is no ID\n", argv[i]); return 2; } lp_push(&roots, x); }
    PGconn *pg = db_read(conninfo); double t = now(); Db d = { 0 };
    fprintf(stderr, "laplace pgn: database %s\n", PQdb(pg));
    db_fetch(pg, &d, roots.v, roots.n);
    fprintf(stderr, "  %zu nodes fetched in %.2f s\n", d.n, now() - t);
    Stat st = { 0 }; Seen sn = { lp_idmap_sized(sizeof(lp_id)), lp_idmap_new(), lp_strmap_sized(sizeof(lp_id)) };
    FILE *out = outp ? fopen(outp, "wb") : stdout; if (!out) { perror(outp); return 1; }
    for (size_t i = 0; i < roots.n; i++) { IdVec recs = { 0 }; records_in(&d, &roots.v[i], &recs, 0);
        if (!recs.n && dn(&d, &roots.v[i])) lp_push(&recs, roots.v[i]);
        for (size_t r = 0; r < recs.n; r++) { Str o = { 0 }; record_pgn(&d, &recs.v[r], &o, &st, &sn); if (o.n) fwrite(o.b, 1, o.n, out); free(o.b); }
        lp_vec_free(&recs); }
    if (outp) fclose(out);
    if (verify) verify_ids(&d, &st);
    fprintf(stderr, "  %llu records, %llu plies, %llu castlings; %llu records of no moves; %llu kept as text; %llu comments, %llu NAGs, %llu variations\n", (unsigned long long)st.records, (unsigned long long)st.plies, (unsigned long long)st.castles,
            (unsigned long long)st.zero, (unsigned long long)st.text_kept, (unsigned long long)st.comments, (unsigned long long)st.nags, (unsigned long long)st.variations);
    fprintf(stderr, "  replay: %llu moves, positions or annotations that are not what the position before makes of them\n", (unsigned long long)st.replay_bad);
    fprintf(stderr, "  Chess960: %llu records, %llu starting at their FEN's position, %llu castlings\n", (unsigned long long)st.c960, (unsigned long long)st.c960_start, (unsigned long long)st.c960_castles);
    fprintf(stderr, "  positions after a move: %llu distinct of %llu arrivals; %llu arrivals by another way than a position's first (%llu distinct ways), on one node each; %llu positions split over two IDs\n",
            (unsigned long long)st.positions, (unsigned long long)st.arrivals, (unsigned long long)st.transposed, (unsigned long long)st.multi, (unsigned long long)st.splits);
    if (verify) fprintf(stderr, "  verify: %llu compositions, %llu whose ID is not BLAKE3 of its constituents' IDs; %llu leaves, %llu not BLAKE3 of their codepoint's UTF-8; %llu constituents not recorded\n",
                        (unsigned long long)st.ids, (unsigned long long)st.id_bad, (unsigned long long)st.leaves, (unsigned long long)st.leaf_bad, (unsigned long long)st.dangling);
    PQfinish(pg); return (st.replay_bad || st.id_bad || st.leaf_bad || st.dangling || st.splits) ? 1 : 0;
}
