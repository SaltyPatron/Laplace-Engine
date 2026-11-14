/* Chess's rules and the IDs of its content (chess_rules.h): the board, legal moves under a law, SAN, FEN, and a
 * position's, move's and line's IDs computed from the codepoints alone (Native's lp_id_*), the same bytes the node table
 * composes when a file is read (chess.c). Built into laplace_engine_reader, so every reader names a position as the
 * ingest does.
 *
 * One implementation of every law firmware/chess.firmware declares: a law is its axes (chess_rules.h LpChessLaw), never
 * a name this code branches on. Standard is the law of all axes off but castling; a NULL law is Standard. */
#include "chess_rules.h"
#include "os.h"
#include <stdio.h>
#include <string.h>
#include <tree_sitter/api.h>

/* ---- the board */
static const int KN[8][2] = { {1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2} }, KG[8][2] = { {1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1} };
static const int DIAG[4][2] = { {1,1},{1,-1},{-1,1},{-1,-1} }, ORTH[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };
static int on(int f, int r){ return f >= 0 && f < 8 && r >= 0 && r < 8; }
static uint64_t bit(int s){ return (uint64_t)1 << s; }
static int castles(const LpChessLaw *w){ return !w || w->castles; }
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
static int adjacent(int a, int b){ return abs((a & 7) - (b & 7)) <= 1 && abs((a >> 3) - (b >> 3)) <= 1; }
/* Whether colour c's king is in check: attacked where it stands. A law whose king is a piece as any other has no check;
 * where captures explode, kings side by side are not in check (neither can take the other). */
int in_check(const Bd *b, int c){
    const LpChessLaw *w = b->law; if (w && w->unroyal) return 0;
    int k = king_sq(b, c); if (k < 0) return 0;
    if (w && w->explodes) { int o = king_sq(b, !c); if (o >= 0 && adjacent(k, o)) return 0; }
    return attacked(b, k, !c);
}
/* Castling rights only where their rook still stands on its file of the back rank, under a king that stands there. */
static void rights_held(Bd *b){
    for (int c = 0; c < 2; c++) { if (!b->rights[c]) continue; int back = c ? 56 : 0, ks = king_sq(b, c); int8_t rk = c ? -ROOK : ROOK;
        if (ks < 0 || (ks & ~7) != back) { b->rights[c] = 0; continue; }
        for (int f = 0; f < 8; f++) if ((b->rights[c] & (1u << f)) && b->sq[back + f] != rk) b->rights[c] &= (uint8_t)~(1u << f); }
}
void make(Bd *b, const Mv *m){
    const LpChessLaw *w = b->law; int c = b->side, back = c ? 56 : 0;
    if (m->drop) {
        b->sq[m->to] = (int8_t)(c ? -m->drop : m->drop); if (b->hand[c][m->drop]) b->hand[c][m->drop]--; b->promoted &= ~bit(m->to);
        b->ep = -1; b->side = (uint8_t)!c;
    } else if (m->castle) {
        int ks = m->from, rs = m->to, kingside = (rs & 7) > (ks & 7); int8_t k = b->sq[ks], rk = b->sq[rs];
        b->sq[ks] = 0; b->sq[rs] = 0; b->sq[back + (kingside ? 6 : 2)] = k; b->sq[back + (kingside ? 5 : 3)] = rk;
        b->promoted &= ~(bit(ks) | bit(rs));
        b->rights[c] = 0; b->ep = -1; b->side = (uint8_t)!c;
    } else {
        int8_t p = b->sq[m->from]; int capsq = m->ep ? (m->from & ~7) | (m->to & 7) : m->to; int8_t cap = b->sq[capsq];
        if (cap && w && w->drops) { int kind = (b->promoted & bit(capsq)) ? PAWN : abs(cap); if (kind != KING && b->hand[c][kind] < 255) b->hand[c][kind]++; }
        int promoted = (b->promoted & bit(m->from)) != 0 || m->promo;
        b->promoted &= ~(bit(m->from) | bit(capsq) | bit(m->to));
        if ((p == KING || p == -KING) && castles(w)) b->rights[c] = 0;
        b->sq[m->to] = m->promo ? (int8_t)(c ? -m->promo : m->promo) : p; b->sq[m->from] = 0;
        if (m->ep) b->sq[capsq] = 0;
        if (promoted && w && w->drops) b->promoted |= bit(m->to);
        if (cap && w && w->explodes) {                                         /* the taker goes with what it took, and every piece but a pawn beside */
            int f = m->to & 7, r = m->to >> 3; b->sq[m->to] = 0;
            for (int i = 0; i < 8; i++) { int ff = f + KG[i][0], rr = r + KG[i][1]; if (!on(ff, rr)) continue; int8_t q = b->sq[rr * 8 + ff]; if (q && abs(q) != PAWN) b->sq[rr * 8 + ff] = 0; } }
        b->ep = -1; if ((p == PAWN || p == -PAWN) && abs((m->to >> 3) - (m->from >> 3)) == 2) b->ep = (int8_t)((m->from + m->to) / 2);
        b->side = (uint8_t)!c;
    }
    rights_held(b);
    if (w && w->checks && in_check(b, b->side) && b->checks[c] < 255) b->checks[c]++;
}
static int capture_of(const Bd *b, const Mv *m){ return !m->castle && !m->drop && (b->sq[m->to] || m->ep); }
static int on_goal(const Bd *b, int c){ int k = king_sq(b, c); return k >= 0 && b->law && (b->law->goal & bit(k)); }
/* Every move by the side to move's pieces and hand, before whether it leaves its own king attacked. */
static int pseudo(const Bd *b, Mv *ps){
    const LpChessLaw *w = b->law; int n = 0, c = b->side, sg = c ? -1 : 1;
    #define ADD(F, T, PR, CA, EP, DR) do { if (n < LP_CHESS_MOVES) { ps[n].from = (uint8_t)(F); ps[n].to = (uint8_t)(T); ps[n].promo = (uint8_t)(PR); ps[n].castle = (uint8_t)(CA); ps[n].ep = (uint8_t)(EP); ps[n].drop = (uint8_t)(DR); n++; } } while (0)
    int top = w && w->king_promotes ? KING : QUEEN;
    for (int s = 0; s < 64; s++) { int8_t p = b->sq[s]; if (!p || colour_of(p) != c) continue; int a = abs(p), f = s & 7, r = s >> 3;
        if (a == PAWN) { int dr = c ? -1 : 1, last = c ? 0 : 7, start = c ? 6 : 1, first = c ? 7 : 0;
            if (on(f, r + dr) && !b->sq[(r + dr) * 8 + f]) {
                if (r + dr == last) for (int pr = KNIGHT; pr <= top; pr++) ADD(s, (r + dr) * 8 + f, pr, 0, 0, 0); else ADD(s, (r + dr) * 8 + f, 0, 0, 0, 0);
                if ((r == start || (r == first && w && w->first_double)) && on(f, r + 2 * dr) && !b->sq[(r + 2 * dr) * 8 + f]) ADD(s, (r + 2 * dr) * 8 + f, 0, 0, 0, 0); }
            for (int df = -1; df <= 1; df += 2) { if (!on(f + df, r + dr)) continue; int t = (r + dr) * 8 + f + df;
                if (b->sq[t] && colour_of(b->sq[t]) != c) { if (r + dr == last) for (int pr = KNIGHT; pr <= top; pr++) ADD(s, t, pr, 0, 0, 0); else ADD(s, t, 0, 0, 0, 0); }
                else if (t == b->ep && !b->sq[t] && b->sq[(r * 8) + f + df] == -sg * PAWN) ADD(s, t, 0, 0, 1, 0); } }
        else if (a == KNIGHT || a == KING) { const int (*d)[2] = a == KNIGHT ? KN : KG;
            for (int i = 0; i < 8; i++) { int ff = f + d[i][0], rr = r + d[i][1]; if (!on(ff, rr)) continue; int8_t q = b->sq[rr * 8 + ff]; if (q && colour_of(q) == c) continue; ADD(s, rr * 8 + ff, 0, 0, 0, 0); } }
        else { for (int k = 0; k < 2; k++) { if ((k == 0 && a == ROOK) || (k == 1 && a == BISHOP)) continue; const int (*d)[2] = k ? ORTH : DIAG;
                for (int i = 0; i < 4; i++) for (int ff = f + d[i][0], rr = r + d[i][1]; on(ff, rr); ff += d[i][0], rr += d[i][1]) { int8_t q = b->sq[rr * 8 + ff];
                    if (q && colour_of(q) == c) break; ADD(s, rr * 8 + ff, 0, 0, 0, 0); if (q) break; } } } }
    /* castling: the king and its rook on the back rank, the squares each crosses empty but for the two of them, and
     * the king in check on none of the squares from where it stands to where it ends */
    int back = c ? 56 : 0, ks = king_sq(b, c);
    if (castles(w) && ks >= 0 && (ks & ~7) == back && b->rights[c] && !in_check(b, c))
        for (int rf = 0; rf < 8; rf++) { if (!(b->rights[c] & (1u << rf))) continue; int rs = back + rf; if (b->sq[rs] != sg * ROOK) continue;
            int kingside = rf > (ks & 7), kd = back + (kingside ? 6 : 2), rd = back + (kingside ? 5 : 3), ok = 1;
            int lo = ks < kd ? ks : kd, hi = ks < kd ? kd : ks; for (int s = lo; s <= hi && ok; s++) if (s != ks && s != rs && b->sq[s]) ok = 0;
            lo = rs < rd ? rs : rd; hi = rs < rd ? rd : rs; for (int s = lo; s <= hi && ok; s++) if (s != ks && s != rs && b->sq[s]) ok = 0;
            lo = ks < kd ? ks : kd; hi = ks < kd ? kd : ks;
            for (int s = lo; s <= hi && ok; s++) { Bd t = *b; t.sq[ks] = 0; t.sq[rs] = 0; t.sq[s] = (int8_t)(sg * KING); if (in_check(&t, c)) ok = 0; }
            if (ok) ADD(ks, rs, 0, 1, 0, 0); }
    /* drops: a piece in hand onto an empty square, a pawn never onto its first or last rank */
    if (w && w->drops) for (int kind = PAWN; kind <= QUEEN; kind++) { if (!b->hand[c][kind]) continue;
        for (int s = 0; s < 64; s++) if (!b->sq[s] && (kind != PAWN || ((s >> 3) != 0 && (s >> 3) != 7))) ADD(s, s, 0, 0, 0, kind); }
    #undef ADD
    return n;
}
/* The legal moves, before whether the game has ended. */
static int legal(const Bd *b, Mv *out){
    const LpChessLaw *w = b->law; Mv ps[LP_CHESS_MOVES]; int n = pseudo(b, ps), c = b->side, k = 0;
    for (int i = 0; i < n; i++) {
        if (!(w && w->unroyal)) { Bd t = *b; make(&t, &ps[i]);
            if (w && w->explodes) { if (king_sq(b, c) >= 0 && king_sq(&t, c) < 0) continue; if (king_sq(&t, !c) >= 0 && in_check(&t, c)) continue; }
            else if (in_check(&t, c)) continue;
            if (w && w->no_check && in_check(&t, !c)) continue; }
        out[k++] = ps[i]; }
    if (w && w->forced) { int any = 0; for (int i = 0; i < k && !any; i++) any = capture_of(b, &out[i]);
        if (any) { int j = 0; for (int i = 0; i < k; i++) if (capture_of(b, &out[i])) out[j++] = out[i]; k = j; } }
    return k;
}
/* Whether the game has ended by its law before any move: checks given, a king exploded, a king on the goal. */
static int ended(const Bd *b){
    const LpChessLaw *w = b->law; if (!w) return 0;
    if (w->checks && (b->checks[0] >= w->checks || b->checks[1] >= w->checks)) return 1;
    if (w->explodes && ((king_sq(b, 0) < 0) != (king_sq(b, 1) < 0))) return 1;
    if (w->goal) { int g0 = on_goal(b, 0), g1 = on_goal(b, 1);
        if (!w->goal_answered) return g0 || g1;
        if (g1) return 1;
        if (g0) { if (b->side == 0) return 1; Mv m[LP_CHESS_MOVES]; int n = legal(b, m);       /* Black answers if its king can reach the goal */
            for (int i = 0; i < n; i++) if (abs(b->sq[m[i].from]) == KING && !m[i].drop && (w->goal & bit(m[i].to))) return 0;
            return 1; } }
    return 0;
}
/* Every legal move, castling as each rook the side may castle with (Chess960's rule holds Standard's). */
int gen(const Bd *b, Mv *out){ return ended(b) ? 0 : legal(b, out); }
int lp_chess_pseudo(const Bd *b, Mv *out){ return ended(b) ? 0 : pseudo(b, out); }
static int pieces_of(const Bd *b, int c){ int n = 0; for (int s = 0; s < 64; s++) n += b->sq[s] && colour_of(b->sq[s]) == c; for (int k = PAWN; k <= QUEEN; k++) n += b->hand[c][k]; return n; }
int lp_chess_result(const Bd *b){
    const LpChessLaw *w = b->law; int c = b->side;
    if (ended(b)) {
        if (w->checks && b->checks[0] >= w->checks) return LP_CHESS_WHITE;
        if (w->checks && b->checks[1] >= w->checks) return LP_CHESS_BLACK;
        if (w->explodes && king_sq(b, 0) < 0) return LP_CHESS_BLACK;
        if (w->explodes && king_sq(b, 1) < 0) return LP_CHESS_WHITE;
        int g0 = on_goal(b, 0), g1 = on_goal(b, 1);
        if (g0 && g1) return LP_CHESS_DRAW;
        return g1 ? LP_CHESS_BLACK : LP_CHESS_WHITE; }
    Mv m[LP_CHESS_MOVES]; if (legal(b, m)) return LP_CHESS_ON;
    if (w && w->unroyal) return c ? LP_CHESS_BLACK : LP_CHESS_WHITE;               /* no move left: the side to move has won */
    if (!pieces_of(b, c)) return c ? LP_CHESS_WHITE : LP_CHESS_BLACK;              /* nothing left to move (Horde's White) */
    if (in_check(b, c)) return c ? LP_CHESS_WHITE : LP_CHESS_BLACK;                /* mate */
    return LP_CHESS_DRAW;                                                          /* stalemate */
}
/* SAN, as the PGN standard writes it (8.2.3), its check or mate mark with it when check is set; a drop as P@e4. */
void san_of(const Bd *b, const Mv *m, const Mv *all, int nall, int check, char *out){
    char *o = out; int8_t p = b->sq[m->from]; int a = abs(p);
    if (m->drop) { *o++ = PIECES[m->drop]; *o++ = '@'; *o++ = (char)('a' + (m->to & 7)); *o++ = (char)('1' + (m->to >> 3)); }
    else if (m->castle) o += sprintf(o, (m->to & 7) > (m->from & 7) ? "O-O" : "O-O-O");
    else {
        int cap = b->sq[m->to] != 0 || m->ep;
        if (a == PAWN) { if (cap) { *o++ = (char)('a' + (m->from & 7)); *o++ = 'x'; } }
        else { *o++ = PIECES[a]; int same = 0, file = 0, rank = 0;
            for (int i = 0; i < nall; i++) { const Mv *x = &all[i]; if (x->castle || x->drop || x->to != m->to || x->from == m->from || abs(b->sq[x->from]) != a) continue;
                same = 1; if ((x->from & 7) == (m->from & 7)) file = 1; if ((x->from >> 3) == (m->from >> 3)) rank = 1; }
            if (same) { if (!file) *o++ = (char)('a' + (m->from & 7)); else if (!rank) *o++ = (char)('1' + (m->from >> 3)); else { *o++ = (char)('a' + (m->from & 7)); *o++ = (char)('1' + (m->from >> 3)); } }
            if (cap) *o++ = 'x'; }
        *o++ = (char)('a' + (m->to & 7)); *o++ = (char)('1' + (m->to >> 3));
        if (m->promo) { *o++ = '='; *o++ = PIECES[m->promo]; } }
    if (check) { Bd t = *b; make(&t, m); if (in_check(&t, t.side)) { Mv r[LP_CHESS_MOVES]; *o++ = gen(&t, r) ? '+' : '#'; } }
    *o = 0;
}
/* The legal move a SAN token names, or -1: its suffix annotations and check or mate mark taken off as written. */
int san_match(const Bd *b, const char *written, size_t n, const Mv *all, int nall){
    while (n && (written[n - 1] == '!' || written[n - 1] == '?')) n--;
    while (n && (written[n - 1] == '+' || written[n - 1] == '#')) n--;
    if (!n || n > 31) return -1;
    char tok[32]; memcpy(tok, written, n); tok[n] = 0;
    char t[32]; size_t l = 0; for (const char *c = tok; *c && l < 31; c++) t[l++] = *c == '0' ? 'O' : *c; t[l] = 0;     /* 0-0: the PGN standard's O-O written with zeros */
    if (!strchr(tok, '-')) for (size_t i = 0; i < l; i++) if (t[i] == 'O') t[i] = '0';
    char s[32];
    for (int i = 0; i < nall; i++) { san_of(b, &all[i], all, nall, 0, s); if (!strcmp(s, t)) return i; }
    /* a drop with its pawn's letter left out (@e4), as the import format of some sites writes it */
    const char *at = strchr(t, '@');
    if (at) { int kind = PAWN; if (at != t) { const char *w = at == t + 1 && t[0] != '.' ? strchr(PIECES, t[0]) : NULL; if (!w || !*w) return -1; kind = (int)(w - PIECES); }
        if (strlen(at + 1) != 2 || at[1] < 'a' || at[1] > 'h' || at[2] < '1' || at[2] > '8') return -1; int to = (at[1] - 'a') + 8 * (at[2] - '1');
        for (int i = 0; i < nall; i++) if (all[i].drop == kind && all[i].to == to) return i;
        return -1; }
    /* as the import format allows: the piece's square over-specified, or a promotion without its = */
    int pc = PAWN, ff = -1, fr = -1, to = -1, pr = 0; const char *c = t;
    if (strchr("NBRQK", *c) && *c) { pc = (int)(strchr(PIECES, *c) - PIECES); c++; }
    char sq[8]; int ns = 0; for (const char *d = c; *d && ns < 6; d++) if ((*d >= 'a' && *d <= 'h') || (*d >= '1' && *d <= '8')) sq[ns++] = *d; else if (*d == '=' || *d == 'x' || *d == '-') continue; else if (strchr("NBRQK", *d)) { pr = (int)(strchr(PIECES, *d) - PIECES); break; } else return -1;
    if (ns < 2) return -1; to = (sq[ns - 2] - 'a') + 8 * (sq[ns - 1] - '1'); if (to < 0 || to > 63) return -1;
    for (int k = 0; k < ns - 2; k++) { if (sq[k] >= 'a' && sq[k] <= 'h') ff = sq[k] - 'a'; else fr = sq[k] - '1'; }
    int hit = -1;
    for (int i = 0; i < nall; i++) { const Mv *m = &all[i]; if (m->castle || m->drop || m->to != to || abs(b->sq[m->from]) != pc || m->promo != pr) continue;
        if (ff >= 0 && (m->from & 7) != ff) continue; if (fr >= 0 && (m->from >> 3) != fr) continue; if (hit >= 0) return -1; hit = i; }
    return hit;
}
void board_start(Bd *b){
    memset(b, 0, sizeof *b); const char *back = "RNBQKBNR";
    for (int f = 0; f < 8; f++) { int8_t p = (int8_t)(strchr(PIECES, back[f]) - PIECES); b->sq[f] = p; b->sq[56 + f] = (int8_t)-p; b->sq[8 + f] = PAWN; b->sq[48 + f] = -PAWN; }
    b->rights[0] = b->rights[1] = (1u << 0) | (1u << 7); b->ep = -1;
}
int board_law_start(Bd *b, const LpChessLaw *law){
    if (law && law->start[0]) return board_law_fen(b, law->start, strlen(law->start), law);
    board_start(b); b->law = law; b->c960 = (uint8_t)(law && law->c960); if (!castles(law)) b->rights[0] = b->rights[1] = 0;
    return 1;
}
static int digits(const char *s, size_t n, unsigned *v){ if (!n || n > 3) return 0; *v = 0; for (size_t i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return 0; *v = *v * 10 + (unsigned)(s[i] - '0'); } return 1; }
/* A FEN under a law (chess_rules.h). KQkq name the outermost rook on that side of the king, a file letter (Shredder-FEN)
 * that file's rook; under Standard's castling only a king on e and rooks on a and h castle. */
int board_law_fen(Bd *b, const char *fen, size_t n, const LpChessLaw *law){
    memset(b, 0, sizeof *b); b->ep = -1; b->law = law; int c960 = law && law->c960; b->c960 = (uint8_t)c960; size_t i = 0; int r = 7, f = 0, last = -1;
    for (; i < n && fen[i] != ' ' && fen[i] != '['; i++) { char ch = fen[i];
        if (r < 0) { const char *pc = strchr(PIECES, toupper((unsigned char)ch)); if (!pc || ch == '.' || pc - PIECES == KING) return 0;   /* a ninth rank: the pieces in hand */
            b->hand[isupper((unsigned char)ch) ? 0 : 1][pc - PIECES]++; continue; }
        if (ch == '/') { if (f != 8) return 0; r--; f = 0; continue; }
        if (ch == '~') { if (last < 0) return 0; b->promoted |= bit(last); continue; }
        if (ch >= '1' && ch <= '8') { f += ch - '0'; if (f > 8) return 0; last = -1; continue; }
        const char *pc = strchr(PIECES, toupper((unsigned char)ch)); if (!pc || ch == '.' || f > 7) return 0;
        last = r * 8 + f; b->sq[r * 8 + f++] = (int8_t)((pc - PIECES) * (isupper((unsigned char)ch) ? 1 : -1)); }
    if (!(r == 0 && f == 8) && !(r == -1 && f == 0)) return 0;
    if (i < n && fen[i] == '[') { for (i++; i < n && fen[i] != ']'; i++) { const char *pc = strchr(PIECES, toupper((unsigned char)fen[i])); if (fen[i] == '-') continue;
            if (!pc || fen[i] == '.' || pc - PIECES == KING) return 0; b->hand[isupper((unsigned char)fen[i]) ? 0 : 1][pc - PIECES]++; }
        if (i >= n) return 0; i++; }
    while (i < n && fen[i] == ' ') i++; if (i >= n) return 0; b->side = fen[i] == 'b'; i++;
    while (i < n && fen[i] == ' ') i++;
    for (; i < n && fen[i] != ' '; i++) { char ch = fen[i]; if (ch == '-' || !castles(law)) continue; int c = islower((unsigned char)ch) ? 1 : 0, back = c ? 56 : 0, ks = king_sq(b, c); if (ks < 0 || (ks & ~7) != back) continue;
        int8_t rk = c ? -ROOK : ROOK; char u = (char)toupper((unsigned char)ch);
        if (u == 'K' || u == 'Q') { int found = -1;
            if (!c960) { int rf = u == 'K' ? 7 : 0; if ((ks & 7) == 4 && b->sq[back + rf] == rk) found = rf; }
            else if (u == 'K') { for (int x = 7; x > (ks & 7); x--) if (b->sq[back + x] == rk) { found = x; break; } }
            else { for (int x = 0; x < (ks & 7); x++) if (b->sq[back + x] == rk) { found = x; break; } }
            if (found >= 0) b->rights[c] |= (uint8_t)(1u << found); }
        else if (u >= 'A' && u <= 'H') { int x = u - 'A'; if (b->sq[back + x] == rk && x != (ks & 7)) b->rights[c] |= (uint8_t)(1u << x); } }
    while (i < n && fen[i] == ' ') i++;
    if (i < n && fen[i] >= 'a' && fen[i] <= 'h' && i + 1 < n && fen[i + 1] >= '1' && fen[i + 1] <= '8') b->ep = (int8_t)((fen[i] - 'a') + 8 * (fen[i + 1] - '1'));
    /* the fields after: the clocks, and the checks given (+W+B) or still to give (W+B) */
    while (i < n && fen[i] != ' ') i++;
    while (i < n) { while (i < n && fen[i] == ' ') i++; size_t s = i; while (i < n && fen[i] != ' ') i++; size_t l = i - s; if (!l) break;
        const char *x = fen + s, *plus = memchr(x + 1, '+', l - 1); unsigned wv, bv;
        if (x[0] == '+' && plus && digits(x + 1, (size_t)(plus - x - 1), &wv) && digits(plus + 1, l - (size_t)(plus - x) - 1, &bv)) { b->checks[0] = (uint8_t)wv; b->checks[1] = (uint8_t)bv; }
        else if (x[0] != '+' && plus && digits(x, (size_t)(plus - x), &wv) && digits(plus + 1, l - (size_t)(plus - x) - 1, &bv) && law && law->checks) {
            b->checks[0] = (uint8_t)(wv < law->checks ? law->checks - wv : 0); b->checks[1] = (uint8_t)(bv < law->checks ? law->checks - bv : 0); } }
    if (!(law && law->drops)) b->promoted = 0;
    return 1;
}
static const LpChessLaw CHESS960 = { .name = "Chess960", .castles = 1, .c960 = 1 };
/* A FEN under Standard's laws, or Chess960's castling (c960). Returns 0 when it is no FEN. */
int board_fen(Bd *b, const char *fen, size_t n, int c960){ return board_law_fen(b, fen, n, c960 ? &CHESS960 : NULL); }
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
    if (b->ep < 0) return -1; Mv m[LP_CHESS_MOVES]; int n = gen(b, m);
    for (int i = 0; i < n; i++) if (m[i].ep) return b->ep;
    return -1;
}
void lp_chess_extra(const Bd *b, LpChessExtra *x){
    memset(x, 0, sizeof *x); static const char order[] = "QRBNP";
    for (int c = 0; c < 2; c++) for (int i = 0; order[i]; i++) { int kind = (int)(strchr(PIECES, order[i]) - PIECES);
        for (int k = 0; k < b->hand[c][kind] && x->nhand < 64; k++) x->hand[x->nhand++] = (uint32_t)(c ? tolower((unsigned char)order[i]) : order[i]); }
    for (int s = 0; s < 64; s++) if ((b->promoted & bit(s)) && b->sq[s]) x->promoted[x->npromoted++] = (uint8_t)s;
    if (b->checks[0] || b->checks[1]) { char t[16]; int l = snprintf(t, sizeof t, "+%u+%u", b->checks[0], b->checks[1]); for (int i = 0; i < l; i++) x->checks[x->nchecks++] = (uint32_t)t[i]; }
}

/* ---- the laws: firmware/chess.firmware */
static LpChessLaw LAWS[32]; static int NLAWS = -1; static char LDEFAULT[64] = "Standard";
static pthread_mutex_t laws_mu = PTHREAD_MUTEX_INITIALIZER;
/* The next word of a firmware line, "Two Words" quoted; 0 at its end. */
static int word(char **at, char *out, size_t cap){
    char *c = *at; while (*c == ' ' || *c == '\t') c++; if (!*c || *c == '\r' || *c == '\n') { *at = c; return 0; }
    char *e; if (*c == '"') { c++; e = strchr(c, '"'); if (!e) return 0; snprintf(out, cap, "%.*s", (int)(e - c), c); *at = e + 1; return 1; }
    e = c + strcspn(c, " \t\r\n"); snprintf(out, cap, "%.*s", (int)(e - c), c); *at = e; return 1;
}
static int square_word(const char *w){ return strlen(w) == 2 && w[0] >= 'a' && w[0] <= 'h' && w[1] >= '1' && w[1] <= '8'; }
int lp_chess_laws_load(const char *path){
    pthread_mutex_lock(&laws_mu);
    if (NLAWS >= 0) { pthread_mutex_unlock(&laws_mu); return NLAWS; }
    FILE *f = path ? fopen(path, "r") : NULL; int n = 0;
    if (!f) { if (path) fprintf(stderr, "chess: %s: no laws (LAPLACE_CHESS_FIRMWARE names them): records are read under Standard and Chess960 alone\n", path);
        memset(LAWS, 0, sizeof LAWS[0] * 2); snprintf(LAWS[0].name, sizeof LAWS[0].name, "Standard"); LAWS[0].castles = 1; LAWS[1] = CHESS960;
        NLAWS = 2; pthread_mutex_unlock(&laws_mu); return NLAWS; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0; char *c = line, w[128];
        if (!word(&c, w, sizeof w)) continue;
        if (!strcmp(w, "default")) { if (word(&c, w, sizeof w)) snprintf(LDEFAULT, sizeof LDEFAULT, "%s", w); continue; }
        if (strcmp(w, "variant")) { fprintf(stderr, "chess: %s: \"%s\" is not something the chess firmware says\n", path, w); continue; }
        if (n >= 32) { fprintf(stderr, "chess: %s: more than 32 variants\n", path); break; }
        LpChessLaw x; memset(&x, 0, sizeof x); x.castles = 1; int bad = 0;
        if (!word(&c, x.name, sizeof x.name)) continue;
        while (!bad && word(&c, w, sizeof w)) {
            char v[128] = "";
            if (!strcmp(w, "castling")) { if (!word(&c, v, sizeof v)) bad = 1; else if (!strcmp(v, "standard")) { x.castles = 1; x.c960 = 0; } else if (!strcmp(v, "chess960")) { x.castles = 1; x.c960 = 1; } else if (!strcmp(v, "none")) x.castles = 0; else bad = 1; }
            else if (!strcmp(w, "drops")) x.drops = 1;
            else if (!strcmp(w, "explodes")) x.explodes = 1;
            else if (!strcmp(w, "captures")) { if (!word(&c, v, sizeof v) || strcmp(v, "forced")) bad = 1; else x.forced = 1; }
            else if (!strcmp(w, "king")) { if (!word(&c, v, sizeof v)) bad = 1; else if (!strcmp(v, "unroyal")) x.unroyal = 1; else if (!strcmp(v, "promotes")) x.king_promotes = 1; else bad = 1; }
            else if (!strcmp(w, "pawns")) { char v2[32]; if (!word(&c, v, sizeof v) || !word(&c, v2, sizeof v2) || strcmp(v, "double") || strcmp(v2, "first")) bad = 1; else x.first_double = 1; }
            else if (!strcmp(w, "check")) { if (!word(&c, v, sizeof v) || strcmp(v, "forbidden")) bad = 1; else x.no_check = 1; }
            else if (!strcmp(w, "checks")) { unsigned k; if (!word(&c, v, sizeof v) || !digits(v, strlen(v), &k) || !k || k > 255) bad = 1; else x.checks = (uint8_t)k; }
            else if (!strcmp(w, "goal")) { char *back = c; int any = 0;
                while (word(&c, v, sizeof v)) { if (!strcmp(v, "answered")) { x.goal_answered = 1; back = c; continue; } if (!square_word(v)) { c = back; break; } x.goal |= bit((v[0] - 'a') + 8 * (v[1] - '1')); any = 1; back = c; }
                if (!any) bad = 1; }
            else if (!strcmp(w, "start")) { if (!word(&c, x.start, sizeof x.start)) bad = 1; }
            else bad = 1; }
        if (bad) { fprintf(stderr, "chess: %s: variant %s: \"%s\" is not something a variant says\n", path, x.name, w); continue; }
        if (x.start[0]) { Bd t; if (!board_law_fen(&t, x.start, strlen(x.start), &x)) { fprintf(stderr, "chess: %s: variant %s: its start is no FEN\n", path, x.name); continue; } }
        LAWS[n++] = x; }
    fclose(f); NLAWS = n; pthread_mutex_unlock(&laws_mu); return n;
}
const LpChessLaw *lp_chess_law_named(const char *v, size_t l){
    if (__atomic_load_n(&NLAWS, __ATOMIC_ACQUIRE) < 0) {                       /* a reader's: LAPLACE_CHESS_FIRMWARE, else chess.firmware beside LAPLACE_FIRMWARE */
        const char *e = getenv("LAPLACE_CHESS_FIRMWARE"), *pf = getenv("LAPLACE_FIRMWARE"); char p[4300] = "";
        if (e && *e) snprintf(p, sizeof p, "%s", e);
        else if (pf && *pf) { snprintf(p, sizeof p, "%s", pf); char *s = strrchr(p, '/'), *b = strrchr(p, '\\'); if (b > s) s = b; if (s) snprintf(s + 1, sizeof p - (size_t)(s + 1 - p), "chess.firmware"); else p[0] = 0; }
        lp_chess_laws_load(p[0] ? p : NULL); }
    if (!v) { v = LDEFAULT; l = strlen(v); }
    for (int i = 0; i < NLAWS; i++) if (strlen(LAWS[i].name) == l && !memcmp(LAWS[i].name, v, l)) return &LAWS[i];
    return NULL;
}

/* ---- chess as content: the IDs a client computes from tier 0 alone (Native's lp_id_*), and the same as Refs in the
 * node table when a file is read (compose) */
static void id_cp(uint32_t cp, lp_id *out){ lp_id_codepoint(cp, out); }
static void id_number(unsigned v, lp_id *out){ char d[16]; int l = snprintf(d, sizeof d, "%u", v); lp_id k[16]; for (int i = 0; i < l; i++) id_cp((uint32_t)d[i], &k[i]); lp_id_compose(k, (size_t)l, out); }
static void id_square(int s, lp_id *out){ lp_id k[2]; id_cp((uint32_t)('a' + (s & 7)), &k[0]); id_cp((uint32_t)('1' + (s >> 3)), &k[1]); lp_id_compose(k, 2, out); }
/* The parts of a position, each the same bytes compose() hashes (src/chess.c gives the layout). */
void lp_chess_id_square(int s, lp_id *out){ id_square(s, out); }
void lp_chess_id_number(unsigned v, lp_id *out){ id_number(v, out); }
void lp_chess_id_piece_square(int8_t p, int s, lp_id *out){ lp_id two[2]; id_cp(piece_cp(p), &two[0]); id_square(s, &two[1]); lp_id_compose(two, 2, out); }
void lp_chess_id_castling(unsigned white, unsigned black, lp_id *out){ lp_id two[2]; id_number(white, &two[0]); id_number(black, &two[1]); lp_id_compose(two, 2, out); }
void lp_chess_id_en_passant(int ep, lp_id *out){ if (ep >= 0) id_square(ep, out); else id_cp('-', out); }
void lp_chess_id_board(const Bd *b, lp_id *out){
    lp_id ps[64]; int n = 0;
    for (int s = 0; s < 64; s++) if (b->sq[s]) lp_chess_id_piece_square(b->sq[s], s, &ps[n++]);
    lp_id_compose(ps, (size_t)n, out);
}
/* A position's ID, computed here without the node table: the same bytes compose() hashes. */
void id_position(const Bd *b, lp_id *out){
    lp_id part[7]; size_t n = 4; lp_chess_id_board(b, &part[0]); id_cp(b->side ? 'b' : 'w', &part[1]);
    lp_chess_id_castling(castle_mask(b, 0), castle_mask(b, 1), &part[2]); lp_chess_id_en_passant(ep_square(b), &part[3]);
    LpChessExtra x; lp_chess_extra(b, &x); lp_id k[64];
    if (x.nhand) { for (int i = 0; i < x.nhand; i++) id_cp(x.hand[i], &k[i]); lp_id_compose(k, (size_t)x.nhand, &part[n++]); }
    if (x.npromoted) { for (int i = 0; i < x.npromoted; i++) id_square(x.promoted[i], &k[i]); lp_id_compose(k, (size_t)x.npromoted, &part[n++]); }
    if (x.nchecks) { for (int i = 0; i < x.nchecks; i++) id_cp(x.checks[i], &k[i]); lp_id_compose(k, (size_t)x.nchecks, &part[n++]); }
    lp_id_compose(part, n, out);
}
/* A move's ID: [piece, from, to], [piece, from, to, promoted piece], or a drop [piece, @, to]. */
void id_move(const Bd *b, const Mv *m, lp_id *out){
    lp_id k[4]; int n = 3;
    if (m->drop) { id_cp(piece_cp((int8_t)(b->side ? -m->drop : m->drop)), &k[0]); id_cp('@', &k[1]); id_square(m->to, &k[2]); lp_id_compose(k, 3, out); return; }
    id_cp(piece_cp(b->sq[m->from]), &k[0]); id_square(m->from, &k[1]); id_square(m->to, &k[2]);
    if (m->promo) { id_cp(piece_cp((int8_t)(b->side ? -m->promo : m->promo)), &k[3]); n = 4; }
    lp_id_compose(k, (size_t)n, out);
}

size_t lp_chess_fen_of(const Bd *b, char *out){
    char *o = out;
    for (int r = 7; r >= 0; r--) { int empty = 0;
        for (int f = 0; f < 8; f++) { int8_t p = b->sq[r * 8 + f]; if (!p) { empty++; continue; } if (empty) *o++ = (char)('0' + empty); empty = 0; *o++ = (char)piece_cp(p); if (b->promoted & bit(r * 8 + f)) *o++ = '~'; }
        if (empty) *o++ = (char)('0' + empty); if (r) *o++ = '/'; }
    if (b->law && b->law->drops) { LpChessExtra x; lp_chess_extra(b, &x); *o++ = '['; for (int i = 0; i < x.nhand; i++) *o++ = (char)x.hand[i]; *o++ = ']'; }
    *o++ = ' '; *o++ = b->side ? 'b' : 'w'; *o++ = ' '; char *rights = o;
    for (int c = 0; c < 2; c++) { int ks = king_sq(b, c);                     /* K Q where Standard's rooks castle, else the rook's file (Shredder-FEN) */
        for (int f = 7; f >= 0; f--) { if (!(b->rights[c] & (1u << f))) continue;
            char letter = ks >= 0 && (ks & 7) == 4 && (f == 7 || f == 0) ? (f == 7 ? 'K' : 'Q') : (char)('A' + f);
            *o++ = c ? (char)tolower((unsigned char)letter) : letter; } }
    if (o == rights) *o++ = '-';
    *o++ = ' '; int ep = ep_square(b);
    if (ep >= 0) { *o++ = (char)('a' + (ep & 7)); *o++ = (char)('1' + (ep >> 3)); } else *o++ = '-';
    *o = 0; return (size_t)(o - out);
}

/* ---- for readers */
int lp_chess_successors(const char *fen, size_t n, lp_id *position, LpChessSuccessor *out, int cap){
    Bd b; if (!fen || !board_fen(&b, fen, n, 0) || king_sq(&b, 0) < 0 || king_sq(&b, 1) < 0) return -1;
    if (position) id_position(&b, position);
    Mv all[LP_CHESS_MOVES]; int na = gen(&b, all), k = 0;
    for (int i = 0; i < na && k < cap; i++, k++) { san_of(&b, &all[i], all, na, 1, out[k].san); out[k].zeroing = (uint8_t)(!all[i].castle && (b.sq[all[i].to] || all[i].ep || abs(b.sq[all[i].from]) == PAWN)); Bd t = b; make(&t, &all[i]); id_position(&t, &out[k].position); }
    return k;
}
int lp_chess_fen_position(const char *fen, size_t n, lp_id *out){
    Bd b; if (!fen || !board_fen(&b, fen, n, 0) || king_sq(&b, 0) < 0 || king_sq(&b, 1) < 0) return 0;
    id_position(&b, out); return 1;
}
/* The main line of the first game the text holds: its tag pairs (FEN, Variant) and its SAN in order. The grammar's own
 * symbols are found by name in the language given, as chess.c's recipe finds them; nothing here scans the bytes. */
static size_t pgn_replay(const void *ts_language, const char *text, size_t n, lp_id *positions, size_t cap, lp_id *line, Bd *end){
    const TSLanguage *language = ts_language; if (!language || !text || !cap || n > UINT32_MAX) return 0;
    TSSymbol game = ts_language_symbol_for_name(language, "game", 4, true), tag = ts_language_symbol_for_name(language, "tag_pair", 8, true),
             san = ts_language_symbol_for_name(language, "san", 3, true);
    TSFieldId name = ts_language_field_id_for_name(language, "name", 4), value = ts_language_field_id_for_name(language, "value", 5);
    if (!san) return 0;
    TSParser *parser = ts_parser_new(); if (!parser) return 0;
    if (!ts_parser_set_language(parser, language)) { ts_parser_delete(parser); return 0; }
    TSTree *tree = ts_parser_parse_string(parser, NULL, text, (uint32_t)n); ts_parser_delete(parser); if (!tree) return 0;
    TSNode root = ts_tree_root_node(tree), node = root; size_t written = 0;
    if (ts_node_has_error(root)) goto done;
    if (game) for (uint32_t i = 0; i < ts_node_named_child_count(root); i++) { TSNode c = ts_node_named_child(root, i); if (ts_node_symbol(c) == game) { node = c; break; } }
    Bd b; const char *fen = NULL, *variant = NULL; size_t fl = 0, vl = 0;
    for (uint32_t i = 0; tag && i < ts_node_named_child_count(node); i++) { TSNode c = ts_node_named_child(node, i); if (ts_node_symbol(c) != tag) continue;
        TSNode nm = ts_node_child_by_field_id(c, name), v = ts_node_child_by_field_id(c, value); if (ts_node_is_null(nm) || ts_node_is_null(v)) goto done;
        const char *k = text + ts_node_start_byte(nm); size_t kl = ts_node_end_byte(nm) - ts_node_start_byte(nm);
        const char *vv = text + ts_node_start_byte(v) + 1; size_t vn = ts_node_end_byte(v) - ts_node_start_byte(v); vn = vn >= 2 ? vn - 2 : 0;     /* the tag's string, its quotes off */
        if (kl == 3 && !memcmp(k, "FEN", 3)) { fen = vv; fl = vn; }
        else if (kl == 7 && !memcmp(k, "Variant", 7)) { variant = vv; vl = vn; } }
    const LpChessLaw *law = lp_chess_law_named(variant, vl); if (!law) goto done;
    if (fen ? !board_law_fen(&b, fen, fl, law) : !board_law_start(&b, law)) goto done;
    lp_id *traj = malloc(sizeof(lp_id) * (2 * cap)); if (!traj) goto done;
    size_t nt = 0; id_position(&b, &positions[0]); traj[nt++] = positions[0]; written = 1;
    for (uint32_t i = 0; i < ts_node_named_child_count(node) && written < cap; i++) { TSNode c = ts_node_named_child(node, i); if (ts_node_symbol(c) != san) continue;
        Mv all[LP_CHESS_MOVES]; int na = gen(&b, all);
        int k = san_match(&b, text + ts_node_start_byte(c), ts_node_end_byte(c) - ts_node_start_byte(c), all, na);
        if (k < 0) { written = 0; break; }
        id_move(&b, &all[k], &traj[nt++]); make(&b, &all[k]); id_position(&b, &positions[written]); traj[nt++] = positions[written++]; }
    if (written && line) lp_id_compose(traj, nt, line);
    if (written && end) *end = b;
    free(traj);
done:
    ts_tree_delete(tree); return written;
}
size_t lp_chess_pgn_positions(const void *ts_language, const char *text, size_t n, lp_id *positions, size_t cap, lp_id *line){ return pgn_replay(ts_language, text, n, positions, cap, line, NULL); }
size_t lp_chess_pgn_end(const void *ts_language, const char *text, size_t n, lp_id *positions, size_t cap, Bd *end){ return pgn_replay(ts_language, text, n, positions, cap, NULL, end); }
