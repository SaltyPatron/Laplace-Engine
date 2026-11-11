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
 *   played    [P, M, P'] for each move, the position before it, the move and the position after: shared content, the same
 *             wherever any game plays it. The result is the score of the side that made the move (1 a win, 0 a loss, 1/2 a
 *             draw), outside the claim; a result of * (the game not finished) claims no outcome and so no move
 *   study     a record whose movetext has variations or NAGs, or every record of a recipe that says "records study":
 *             the annotator's testimony, not a game anyone played: each NAG as the annotator's judgment of a move in its
 *             position, [position, move, NAG]
 *   puzzle    every record of a recipe that says "records puzzle": the composer's claimed solution, the line itself, and
 *             the NAGs as a study's
 * A record is never the subject of a claim: its line, its tags (the players, their ratings, the clocks, the date, the
 * event, the result) and its layer are its content, the trajectory of the game, and are not claimed again as [record, field].
 * The rules a record is replayed under are firmware (Sequence 28, Games): firmware/chess.firmware names each variant
 * a Variant tag can name and how it castles; the variant governs which moves are legal and is in no position's ID.
 *
 *   laplace pgn [-d conninfo] [--verify] [-o file] ID...
 *                    a file's (or a record's) PGN written again from the database alone; --verify recomputes every
 *                    ID under it from its constituents with BLAKE3 alone, every leaf a codepoint, and replays every
 *                    line: each position the one its move makes of the one before */
#include "chess_internal.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the rules: firmware/chess.firmware */
static Rules RULES[32]; static int NRULES = -1; static char RDEFAULT[64] = "Standard";
static pthread_mutex_t rules_mu = PTHREAD_MUTEX_INITIALIZER;
void rules_load(void){
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
const Rules *rules_named(const char *v, size_t l){
    rules_load(); if (!v) { v = RDEFAULT; l = strlen(v); }
    for (int i = 0; i < NRULES; i++) if (strlen(RULES[i].name) == l && !memcmp(RULES[i].name, v, l)) return &RULES[i];
    return NULL;
}

/* ---- the board */
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
void make(Bd *b, const Mv *m){
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
int gen(const Bd *b, Mv *out){
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
void san_of(const Bd *b, const Mv *m, const Mv *all, int nall, int check, char *out){
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
void board_start(Bd *b){
    memset(b, 0, sizeof *b); const char *back = "RNBQKBNR";
    for (int f = 0; f < 8; f++) { int8_t p = (int8_t)(strchr(PIECES, back[f]) - PIECES); b->sq[f] = p; b->sq[56 + f] = (int8_t)-p; b->sq[8 + f] = PAWN; b->sq[48 + f] = -PAWN; }
    b->rights[0] = b->rights[1] = (1u << 0) | (1u << 7); b->ep = -1;
}
/* A FEN (its first four fields): KQkq the outermost rook on that side of the king, a file letter (Shredder-FEN) that
 * file's rook; under Standard's rules only a king on e and rooks on a and h castle. Returns 0 when it is no FEN. */
int board_fen(Bd *b, const char *fen, size_t n, int c960){
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
unsigned castle_mask(const Bd *b, int c){
    if (!b->rights[c]) return 0; int ks = king_sq(b, c); unsigned m = 0;
    if (ks >= 0) m |= 0x80u >> (ks & 7); for (int f = 0; f < 8; f++) if (b->rights[c] & (1u << f)) m |= 0x80u >> f;
    return m;
}
/* The en passant square of the position: only where a pawn can take on it by a legal move (otherwise the same
 * position would have two identities). */
int ep_square(const Bd *b){
    if (b->ep < 0) return -1; Mv m[256]; int n = gen(b, m);
    for (int i = 0; i < n; i++) if (m[i].ep) return b->ep;
    return -1;
}

/* ---- chess as content: the IDs a client computes from tier 0 alone (Native's lp_id_*), and the same as Refs in the
 * node table when a file is read (compose) */
static void id_cp(uint32_t cp, lp_id *out){ lp_id_codepoint(cp, out); }
static void id_number(unsigned v, lp_id *out){ char d[16]; int l = snprintf(d, sizeof d, "%u", v); lp_id k[16]; for (int i = 0; i < l; i++) id_cp((uint32_t)d[i], &k[i]); lp_id_compose(k, (size_t)l, out); }
static void id_square(int s, lp_id *out){ lp_id k[2]; id_cp((uint32_t)('a' + (s & 7)), &k[0]); id_cp((uint32_t)('1' + (s >> 3)), &k[1]); lp_id_compose(k, 2, out); }
/* A position's ID, computed here without the node table: the same bytes compose() hashes. */
void id_position(const Bd *b, lp_id *out){
    lp_id ps[64], two[2]; int n = 0;
    for (int s = 0; s < 64; s++) if (b->sq[s]) { id_cp(piece_cp(b->sq[s]), &two[0]); id_square(s, &two[1]); lp_id_compose(two, 2, &ps[n++]); }
    lp_id part[4]; lp_id_compose(ps, (size_t)n, &part[0]); id_cp(b->side ? 'b' : 'w', &part[1]);
    id_number(castle_mask(b, 0), &two[0]); id_number(castle_mask(b, 1), &two[1]); lp_id_compose(two, 2, &part[2]);
    int ep = ep_square(b); if (ep >= 0) id_square(ep, &part[3]); else id_cp('-', &part[3]);
    lp_id_compose(part, 4, out);
}
void id_move(const Bd *b, const Mv *m, lp_id *out){
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

/* Compiled PGN symbols feed the chess state machine; no byte scanner parses
 * packaging. The game work arena is reused for each bounded input window. */
enum { PG_TAG=1, PG_SAN, PG_NAG, PG_ANNOTATION, PG_VARIATION, PG_COMMENT, PG_LINE_COMMENT, PG_RESULT };
enum { K_PLAYED = 0, K_STUDY, K_PUZZLE };
typedef struct { int records; const Layout *layout; uint8_t *dispatch; uint32_t symbols; TSFieldId name, value; } ChessRecipe;
static __thread SayScratch chess_arena;
static void *chess_grow(const void *old,size_t bytes,size_t next){
    void *p=arena_alloc(&chess_arena,next); if(bytes)memcpy(p,old,bytes); return p;
}
typedef struct Line Line;
typedef struct { uint8_t kind; const char *t; uint32_t n; Line *var; } Ann;         /* kind: 1 a comment, 2 a NAG, 3 a variation */
typedef struct { const char *san; uint32_t n; Ann *ann; int nann, cann; } Ply;
struct Line { Ply *p; int n, cap; Ann *lead; int nlead, clead; };
typedef struct { const char *name, *val; uint32_t nl, vl; char *unesc; } Tag;
typedef struct { Tag *tag; int ntag, ctag; Line main; const char *mt; size_t mtn; char result[8]; int has_result; int bad; } Game;
static void ann_push(Ann **a, int *n, int *c, Ann x){ if (*n == *c) { int next=*c?*c*2:4; *a=chess_grow(*a,sizeof(Ann)*(size_t)*n,sizeof(Ann)*(size_t)next);*c=next; } (*a)[(*n)++] = x; }
static uint8_t pgn_role(const ChessRecipe *recipe,TSNode node){
    TSSymbol symbol=ts_node_symbol(node); return symbol<recipe->symbols?recipe->dispatch[symbol]:0;
}
static void pgn_annotation(Line *line,Ann annotation){
    if(line->n){Ply *ply=&line->p[line->n-1];ann_push(&ply->ann,&ply->nann,&ply->cann,annotation);}
    else ann_push(&line->lead,&line->nlead,&line->clead,annotation);
}
static void pgn_tag(const ChessRecipe *recipe,TSNode node,const char *source,Game *game){
    TSNode name=ts_node_child_by_field_id(node,recipe->name),value=ts_node_child_by_field_id(node,recipe->value);
    if(ts_node_is_null(name) || ts_node_is_null(value)){game->bad=1;return;}
    if(game->ntag==game->ctag){int next=game->ctag?game->ctag*2:16;game->tag=chess_grow(game->tag,sizeof(Tag)*(size_t)game->ntag,sizeof(Tag)*(size_t)next);game->ctag=next;}
    Tag *tag=&game->tag[game->ntag++];memset(tag,0,sizeof *tag);
    tag->name=source+ts_node_start_byte(name);tag->nl=ts_node_end_byte(name)-ts_node_start_byte(name);
    uint32_t start=ts_node_start_byte(value)+1,end=ts_node_end_byte(value)-1;
    char *decoded=arena_alloc(&chess_arena,(size_t)(end-start)+1);uint32_t count=0;
    for(uint32_t j=start;j<end;j++){if(source[j]=='\\' && j+1<end)j++;decoded[count++]=source[j];}
    decoded[count]=0;tag->val=decoded;tag->vl=count;game->mt=source+ts_node_end_byte(node);
}
static void pgn_line(const ChessRecipe *recipe,TSNode node,const char *source,Game *game,Line *line,unsigned depth){
    if(depth>64){game->bad=1;return;}
    TSTreeCursor *cursor=lp_tls_cursor(node);int more=ts_tree_cursor_goto_first_child(cursor);
    while(more){
        TSNode child=ts_tree_cursor_current_node(cursor);uint8_t role=pgn_role(recipe,child);
        const char *text=source+ts_node_start_byte(child);uint32_t length=ts_node_end_byte(child)-ts_node_start_byte(child);
        if(role==PG_TAG && !depth)pgn_tag(recipe,child,source,game);
        else if(role==PG_SAN){
            uint32_t san=length;while(san && (text[san-1]=='!' || text[san-1]=='?'))san--;
            if(line->n==line->cap){int next=line->cap?line->cap*2:128;line->p=chess_grow(line->p,sizeof(Ply)*(size_t)line->n,sizeof(Ply)*(size_t)next);line->cap=next;}
            line->p[line->n++]=(Ply){.san=text,.n=san};
            if(san<length)pgn_annotation(line,(Ann){2,text+san,length-san,NULL});
        }else if(role==PG_COMMENT || role==PG_LINE_COMMENT){
            uint32_t trim=role==PG_COMMENT?2:1;
            if(length>=trim)pgn_annotation(line,(Ann){1,text+1,length-trim,NULL});
        }else if(role==PG_NAG || role==PG_ANNOTATION)pgn_annotation(line,(Ann){2,text,length,NULL});
        else if(role==PG_VARIATION){
            Line *variation=arena_alloc(&chess_arena,sizeof *variation);memset(variation,0,sizeof *variation);
            if(!line->n)game->bad=1;
            pgn_annotation(line,(Ann){3,NULL,0,variation});pgn_line(recipe,child,source,game,variation,depth+1);
        }else if(role==PG_RESULT && !depth){
            if(length>=sizeof game->result)game->bad=1;
            else {memcpy(game->result,text,length);game->result[length]=0;game->has_result=1;}
        }
        more=ts_tree_cursor_goto_next_sibling(cursor);
    }
    lp_tls_cursor_release(cursor);
}
static void pgn_game(const ChessRecipe *recipe,TSNode node,const char *source,Game *game){
    memset(game,0,sizeof *game);game->mt=source+ts_node_start_byte(node);
    pgn_line(recipe,node,source,game,&game->main,0);
    game->mtn=(size_t)(source+ts_node_end_byte(node)-game->mt);
}

/* ---- a game composed */
typedef struct { Ref *c; size_t n, cap; } RV;
static void rv(RV *a, Ref x){ if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 64; a->c = chess_grow(a->c, sizeof(Ref) * a->n, sizeof(Ref) * a->cap); } a->c[a->n++] = x; }
static Ref comp(const Ref *k, size_t n){ Ref x = n == 1 ? k[0] : compose(k, (uint32_t)n, ref_above(k, n)); x.said = 0; return x; }
typedef struct { RefCache *rc; RV nags, transitions; long errs; char err[160]; uint64_t plies, castles; } Rd;
/* A line replayed from b0: its trajectory, and its layer where it has annotations (has_layer). 0: a move that is not
 * legal there, said in r->err. */
static int replay(Rd *r, const Line *L, const Bd *b0, Ref *line, Ref *layer, int *has_layer){
    Bd b = *b0; RV traj = { 0 }, lay = { 0 }; int annotated = L->nlead > 0; Ref P = r_position(r->rc, &b); rv(&traj, P);
    if (L->nlead) { RV e = { 0 }; rv(&e, P);
        for (int j = 0; j < L->nlead; j++) { const Ann *x = &L->lead[j]; if (x->kind == 3) continue; rv(&e, r_text((const uint8_t *)x->t, x->n)); }
        rv(&lay, comp(e.c, e.n)); }
    for (int i = 0; i < L->n; i++) { const Ply *p = &L->p[i];
        Mv all[256]; int na = gen(&b, all); char tok[32]; size_t tl = p->n < 31 ? p->n : 31; memcpy(tok, p->san, tl); tok[tl] = 0;
        size_t z = tl; while (z && (tok[z - 1] == '+' || tok[z - 1] == '#')) tok[--z] = 0;
        int k = z ? san_match(&b, tok, all, na) : -1;
        if (k < 0) { snprintf(r->err, sizeof r->err, "%.*s is no legal move at ply %d", (int)p->n, p->san, i + 1); return 0; }
        Ref M = r_move(r->rc, &b, &all[k]); Bd before = b; Ref Pb = P; make(&b, &all[k]); P = r_position(r->rc, &b); rv(&traj, M); rv(&traj, P); r->plies++; r->castles += all[k].castle;
        rv(&r->transitions,Pb);rv(&r->transitions,M);rv(&r->transitions,P);
        if (!p->nann) { rv(&lay, M); continue; }
        annotated = 1; RV e = { 0 }; rv(&e, M);
        for (int j = 0; j < p->nann; j++) { const Ann *x = &p->ann[j];
            if (x->kind == 1) rv(&e, r_text((const uint8_t *)x->t, x->n));
            else if (x->kind == 2) { Ref nag = r_text((const uint8_t *)x->t, x->n); rv(&e, nag); rv(&r->nags, Pb); rv(&r->nags, M); rv(&r->nags, nag); }
            else if (x->var && x->var->n) { Ref vl, vy; int hv = 0; if (!replay(r, x->var, &before, &vl, &vy, &hv)) { return 0; }
                if (hv) { Ref two[2] = { vl, vy }; rv(&e, comp(two, 2)); } else rv(&e, vl); } }
        rv(&lay, comp(e.c, e.n)); }
    *line = comp(traj.c, traj.n); *has_layer = annotated; if (annotated) *layer = comp(lay.c, lay.n);
    return 1;
}
/* A game's content, however it is read (a file of games, or a field that holds one): where it replays under its rules,
 * [line, tags, layer], its tags only where it has any and its layer only where it is annotated, so a game of neither is
 * its line; where it does not, [tags, movetext] as written. 0: it holds nothing. */
typedef struct { const char *variant, *fen, *result; size_t vl, fl, rl; const Rules *rules; Bd b0; Ref line; int ok; } Start;
static int game_content(Rd *rd, const Game *g, Start *st, Ref *rec){
    memset(st, 0, sizeof *st);
    for (int i = 0; i < g->ntag; i++) { const Tag *t = &g->tag[i];
        if (t->nl == 7 && !memcmp(t->name, "Variant", 7)) { st->variant = t->val; st->vl = t->vl; }
        else if (t->nl == 3 && !memcmp(t->name, "FEN", 3)) { st->fen = t->val; st->fl = t->vl; }
        else if (t->nl == 6 && !memcmp(t->name, "Result", 6)) { st->result = t->val; st->rl = t->vl; } }
    /* the tags: each [name, value] as written; an empty value, the name alone */
    RV tags = { 0 };
    for (int i = 0; i < g->ntag; i++) { const Tag *t = &g->tag[i]; Ref nm = r_text((const uint8_t *)t->name, t->nl);
        if (!t->vl) { rv(&tags, nm); continue; }
        Ref two[2] = { nm, r_text((const uint8_t *)t->val, t->vl) }; Ref pr = compose(two, 2, ref_above(two, 2)); pr.said = LP_SAID_TUPLE; rv(&tags, pr); }
    Ref T = tags.n ? (tags.n == 1 ? tags.c[0] : compose(tags.c, (uint32_t)tags.n, ref_above(tags.c, tags.n))) : atom('-'); T.said = 0;
    st->rules = rules_named(st->variant, st->vl); st->ok = st->rules != NULL;
    if (st->ok) { if (st->fen) st->ok = board_fen(&st->b0, st->fen, st->fl, st->rules->c960); else board_start(&st->b0); }
    Ref layer; int has_layer = 0; rd->nags.n = rd->transitions.n = 0;
    if (st->ok && !g->bad) st->ok = replay(rd, &g->main, &st->b0, &st->line, &layer, &has_layer); else if (st->ok) { st->ok = 0; snprintf(rd->err, sizeof rd->err, "its variations are not closed"); }
    else if (!st->rules) snprintf(rd->err, sizeof rd->err, "its variant %.*s is in no rules (firmware/chess.firmware)", (int)st->vl, st->variant); else snprintf(rd->err, sizeof rd->err, "its FEN is no position");
    RV part = { 0 };
    if (st->ok) { rv(&part, st->line); if (tags.n) rv(&part, T); if (has_layer) rv(&part, layer); }
    else { if (tags.n) rv(&part, T); if (g->mtn) rv(&part, r_text((const uint8_t *)g->mt, g->mtn)); }
    if (!part.n) return 0;
    *rec = comp(part.c, part.n); return 1;
}
static int has_study_marks(const Line *L){
    for (int i = 0; i < L->n; i++) for (int j = 0; j < L->p[i].nann; j++) if (L->p[i].ann[j].kind != 1) return 1;
    for (int j = 0; j < L->nlead; j++) if (L->lead[j].kind != 1) return 1;
    return 0;
}

/* ---- the recipe: grammar pgn, and what its records are */

static int chess_says(Recipe *r, const char *path, char *tok){
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(ChessRecipe));
    ChessRecipe *c = r->native_recipe;
    if (!strcmp(tok, "records")) { char *v = strtok(NULL, " \t\r\n");
        if (v && !strcmp(v, "played")) c->records = K_PLAYED; else if (v && !strcmp(v, "study")) c->records = K_STUDY; else if (v && !strcmp(v, "puzzle")) c->records = K_PUZZLE;
        else { fprintf(stderr, "%s: records played | study | puzzle\n", path); return -1; }
        return 1; }
    fprintf(stderr, "%s: \"%s\" is not something a PGN recipe says (records)\n", path, tok); return -1;
}
static int chess_recipe(Recipe *r, const char *path){
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(ChessRecipe));
    ChessRecipe *recipe=r->native_recipe;recipe->layout=layout_format("pgn");
    if(!recipe->layout){fprintf(stderr,"%s: PGN packaging format is unavailable\n",path);return 0;}
    const TSLanguage *language=grammar_lang(recipe->layout->g.grammar_id);r->lang=language;
    recipe->symbols=ts_language_symbol_count(language);recipe->dispatch=calloc(recipe->symbols,1);
    const char *names[]={"tag_pair","san","nag","annotation","variation","comment","line_comment","result"};
    for(uint8_t i=0;i<sizeof names/sizeof names[0];i++){
        TSSymbol symbol=ts_language_symbol_for_name(language,names[i],(uint32_t)strlen(names[i]),true);
        if(!symbol){fprintf(stderr,"%s: unresolved PGN symbol %s\n",path,names[i]);return 0;}
        recipe->dispatch[symbol]=(uint8_t)(i+1);
    }
    recipe->name=ts_language_field_id_for_name(language,"name",4);recipe->value=ts_language_field_id_for_name(language,"value",5);
    if(!recipe->name || !recipe->value)return 0;
    r->curated=1;return 1;
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
    Event e = { .claim = c.id, .witnessed = rec.id, .score = 1.0f, .enter_rating = 1500.0f, .position = ++*pos, .kind = EV_CLAIM, .run=1 }; ev_push(&f->ev, &e);
}
/* A PGN file, read: every game a record, in the file's order; its trunk [the OS's record, its records]. */
static void chess_records(const Recipe *rcp, File *f, const uint8_t *src, SRecords *records){
    size_t mark=chess_arena.used;
    const ChessRecipe *cr = rcp->native_recipe; RefCache *rc = calloc(1, sizeof(RefCache)); Rd rd = {.rc=rc};
    RV recs = { 0 }; uint64_t games = 0, played = 0, study = 0, puzzle = 0, unread = 0, unfinished = 0, c960 = 0, zero = 0, nomarker = 0, mismatch = 0;
    Game g; uint32_t ordinal = (uint32_t)f->records;SRecord record;
    while (s_record_next(records,&record)) {
        pgn_game(cr,record.node,(const char *)src,&g);
        games++; ordinal++;
        Start st; Ref rec; int held_any = game_content(&rd, &g, &st, &rec);
        const char *result = st.result; size_t rl = st.rl; int ok = st.ok; const Bd b0 = st.b0; Ref line = st.line;
        int result_mismatch=g.has_result && result && (rl!=strlen(g.result) || memcmp(result,g.result,rl));
        if (!g.has_result) nomarker++;else if(result_mismatch)mismatch++;
        if(!result && g.has_result){result=g.result;rl=strlen(g.result);}
        int kind = cr->records == K_PLAYED && has_study_marks(&g.main) ? K_STUDY : cr->records;
        if (st.variant && st.rules && st.rules->c960) c960++;
        if (!ok) { unread++; if (unread <= 5) fprintf(stderr, "\n  %s: game %u is kept as its text and not replayed: %s", f->path, ordinal, rd.err); }
        if (!held_any) continue;
        uint64_t event_begin=f->ev.n;
        Event e = { .claim = rec.id, .witnessed = rec.id, .score = 1.0f, .enter_rating = 1500.0f, .position = ordinal, .kind = EV_RECORD }; ev_push(&f->ev, &e);
        uint32_t pos = 0; int outcome = result && !result_mismatch &&
            ((rl==3 && (!memcmp(result,"1-0",3) || !memcmp(result,"0-1",3))) || (rl==7 && !memcmp(result,"1/2-1/2",7)));
        if (ok && (!g.main.n)) zero++;
        if (!outcome) unfinished++;
        if (ok && kind == K_PLAYED) played++;                                  /* the record is its line and its tags: its content, not a claim of itself (the moves below are the claims) */
        if(ok && kind==K_PLAYED && outcome)for(size_t i=0;i+2<rd.transitions.n;i+=3){
            int side=(b0.side+(int)(i/3))&1;
            float score=rl==7?0.5f:((result[0]=='1')==(side==0)?1.0f:0.0f);
            member(f,rec,&rd.transitions.c[i],3,&pos);f->ev.e[f->ev.n-1].score=score;
        }
        if (ok && kind == K_PUZZLE) { Ref p1[1] = { line }; member(f, rec, p1, 1, &pos); puzzle++; }
        if (ok && kind == K_STUDY) study++;
        if (ok && kind != K_PLAYED) for (size_t i = 0; i + 2 < rd.nags.n; i += 3) member(f, rec, &rd.nags.c[i], 3, &pos);
        RV provenance={0};rv(&provenance,rec);
        for(uint64_t i=event_begin;i<f->ev.n;i++)if(f->ev.e[i].kind==EV_CLAIM){
            Ref claim;if(!record_claim_vertex(&f->ev.e[i],&claim)){fputs("chess claim has no product\n",stderr);exit(2);}rv(&provenance,claim);
        }
        Ref held=comp(provenance.c,provenance.n);held.said=LP_SAID_RECORD;
        f->ev.e[event_begin].claim=f->ev.e[event_begin].witnessed=held.id;
        rv(&recs,held); }
    f->records += games; f->incomplete += unread;
    if (f->emit) {
        if(f->emit(f->emit_context,recs.c,recs.n,NULL,0,&f->ev)){fputs("PGN output window failed\n",stderr);exit(2);}
        f->ev.n=0;
    }else{if (recs.n) { Ref content = blocks(recs.c, recs.n); content.said = 0; f->trunk = content; }f->laid = 0;}
    fprintf(stderr, "\n  %s: window of %llu games: %llu played, %llu studies, %llu puzzles, %llu kept as text; %llu without an outcome (*), %llu of no moves, %llu Chess960; %llu plies, %llu castlings; %llu without a termination marker, %llu whose marker is not their Result\n",
            f->path, (unsigned long long)games, (unsigned long long)played, (unsigned long long)study, (unsigned long long)puzzle, (unsigned long long)unread, (unsigned long long)unfinished,
            (unsigned long long)zero, (unsigned long long)c960, (unsigned long long)rd.plies, (unsigned long long)rd.castles, (unsigned long long)nomarker, (unsigned long long)mismatch);
    free(rc);chess_arena.used=mark;
}

static void chess_read(const Recipe *recipe,File *file,const uint8_t *source,size_t length){
    const ChessRecipe *compiled=recipe->native_recipe;SRecords records;
    if(!s_records_open(&records,compiled->layout,source,length)){fprintf(stderr,"%s: invalid PGN packaging\n",file->path);exit(2);}
    chess_records(recipe,file,source,&records);s_records_close(&records);
}
static void chess_input(const Recipe *recipe,File *file,const SInput *input){
    const ChessRecipe *compiled=recipe->native_recipe;lp_buf buffer={0};SayScratch headers={0};
    size_t quantum=(size_t)8<<20;const char *configured=getenv("LAPLACE_RECORD_WINDOW_BYTES");
    if(configured && *configured){char *end;unsigned long long value=strtoull(configured,&end,10);
        if(*end || !value || value>UINT32_MAX){fputs("invalid PGN record window\n",stderr);exit(2);}quantum=(size_t)value;}
    int ended=0;uint64_t offset=0;
    table_pin(&file->witness.id);if(file->has_lineage)table_pin(&file->lineage.id);
    while(!ended || buffer.n){
        SRecords records;size_t consumed=0;int status=0;
        while(!status){
            if(!ended){if(buffer.n>UINT32_MAX-quantum){fputs("PGN game exceeds parser byte range\n",stderr);exit(2);}
                lp_reserve((void **)&buffer.b,&buffer.cap,buffer.n+quantum,1);size_t got=input->read(input->context,buffer.b+buffer.n,quantum);buffer.n+=got;
                if(!got){ended=1;if(input->error && input->error(input->context)){fputs("PGN input read failed\n",stderr);exit(2);}}
            }
            if(!buffer.n && ended)break;
            status=s_records_window(&records,compiled->layout,buffer.b,buffer.n,ended,&consumed,&headers);
            if(status<0){fprintf(stderr,"%s: invalid PGN packaging at byte %llu\n",file->path,(unsigned long long)offset);exit(2);}
        }
        if(!status)break;
        chess_records(recipe,file,buffer.b,&records);s_records_close(&records);
        if(!consumed){fputs("PGN framing made no progress\n",stderr);exit(2);}
        memmove(buffer.b,buffer.b+consumed,buffer.n-consumed);buffer.n-=consumed;offset+=consumed;file->at=offset;
    }
    lp_buf_free(&buffer);arena_release(&headers);arena_release(&chess_arena);
    if(file->emit)file_stream_finish(file);
}

/* ---- a field of another file read as chess (part NAME grammar pgn): its text is movetext, the PGN standard's 8.2 with
 * no tag pairs and no termination marker (an opening book's line), parsed by the movetext grammar (formats/pgn_moves:
 * PGN's own rules, a document of elements) and composed by game_content as a game of a file is: under the default rules
 * from the standard start position. So a game of no tags and no annotations that plays the same moves is the same
 * content, its line; a field that does not replay is its text as written, said, as a game's would be. */
static ChessRecipe movetext; static int movetext_state;            /* 0 not compiled yet, 1 ready, -1 unavailable */
static pthread_mutex_t movetext_mu = PTHREAD_MUTEX_INITIALIZER;
static int movetext_ready(void){
    pthread_mutex_lock(&movetext_mu);
    if (!movetext_state) {
        movetext_state = -1; const Layout *layout = layout_format("pgn_moves");
        const TSLanguage *language = layout ? grammar_lang(layout->g.grammar_id) : NULL;
        if (language) {
            movetext.layout = layout; movetext.symbols = ts_language_symbol_count(language); movetext.dispatch = calloc(movetext.symbols, 1);
            const char *names[] = {"tag_pair","san","nag","annotation","variation","comment","line_comment","result"};    /* the roles of chess_recipe's dispatch */
            int san = 0;
            for (uint8_t i = 0; i < sizeof names / sizeof names[0]; i++) {
                TSSymbol symbol = ts_language_symbol_for_name(language, names[i], (uint32_t)strlen(names[i]), true);
                if (symbol && symbol < movetext.symbols) { movetext.dispatch[symbol] = (uint8_t)(i + 1); san |= i == 1; } }      /* movetext has no tag pairs and no result */
            if (san) movetext_state = 1; }
        if (movetext_state < 0) fputs("chess: the movetext grammar (formats/pgn_moves.format) is unavailable\n", stderr); }
    int ready = movetext_state > 0; pthread_mutex_unlock(&movetext_mu); return ready;
}
static int chess_field(const uint8_t *text, size_t n, Ref *out){
    if (!text) return movetext_ready();                                       /* at the recipe's load */
    if (__atomic_load_n(&movetext_state, __ATOMIC_ACQUIRE) <= 0 || n > UINT32_MAX) return 0;
    TSParser *parser = lp_tls_parser(movetext.layout->g.grammar_id); if (!parser) return 0;
    TSTree *tree = ts_parser_parse_string(parser, NULL, (const char *)text, (uint32_t)n); if (!tree) return 0;
    TSNode root = ts_tree_root_node(tree); static int told; int held = 0;
    if (ts_node_has_error(root)) { if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 5) fprintf(stderr, "\n  chess: \"%.*s\" is no movetext\n", (int)(n < 80 ? n : 80), (const char *)text); }
    else {
        size_t mark = chess_arena.used; Game g; pgn_game(&movetext, root, (const char *)text, &g);
        RefCache *rc = arena_alloc(&chess_arena, sizeof *rc); memset(rc, 0, sizeof *rc); Rd rd = { .rc = rc }; Start st;
        held = game_content(&rd, &g, &st, out);
        if (held && !st.ok && __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 5) fprintf(stderr, "\n  chess: \"%.*s\" is kept as its text and not replayed: %s\n", (int)(n < 80 ? n : 80), (const char *)text, rd.err);
        chess_arena.used = mark; }
    ts_tree_delete(tree); return held;
}

/* PGN packaging uses the shared compiled grammar/framer; C owns chess rules. */
const NativeGrammar chess_pgn = { "pgn", chess_says, chess_recipe, chess_read, chess_input, chess_field };
