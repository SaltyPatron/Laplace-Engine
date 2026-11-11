/* Chess's rules and the IDs of its content (chess_rules.h): the board, legal moves, SAN, FEN, and a position's, move's
 * and line's IDs computed from the codepoints alone (Native's lp_id_*), the same bytes the node table composes when a
 * file is read (chess.c). Built into laplace_engine_reader, so every reader names a position as the ingest does. */
#include "chess_rules.h"
#include <stdio.h>
#include <string.h>
#include <tree_sitter/api.h>

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

/* ---- for readers */
int lp_chess_fen_position(const char *fen, size_t n, lp_id *out){
    Bd b; if (!fen || !board_fen(&b, fen, n, 0) || king_sq(&b, 0) < 0 || king_sq(&b, 1) < 0) return 0;
    id_position(&b, out); return 1;
}
/* The main line of the first game the text holds: its tag pairs (FEN, Variant) and its SAN in order. The grammar's own
 * symbols are found by name in the language given, as chess.c's recipe finds them; nothing here scans the bytes. */
size_t lp_chess_pgn_positions(const void *ts_language, const char *text, size_t n, lp_id *positions, size_t cap, lp_id *line){
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
    Bd b; board_start(&b); int c960 = 0; const char *fen = NULL; size_t fl = 0;
    for (uint32_t i = 0; tag && i < ts_node_named_child_count(node); i++) { TSNode c = ts_node_named_child(node, i); if (ts_node_symbol(c) != tag) continue;
        TSNode nm = ts_node_child_by_field_id(c, name), v = ts_node_child_by_field_id(c, value); if (ts_node_is_null(nm) || ts_node_is_null(v)) goto done;
        const char *k = text + ts_node_start_byte(nm); size_t kl = ts_node_end_byte(nm) - ts_node_start_byte(nm);
        const char *vv = text + ts_node_start_byte(v) + 1; size_t vl = ts_node_end_byte(v) - ts_node_start_byte(v); vl = vl >= 2 ? vl - 2 : 0;     /* the tag's string, its quotes off */
        if (kl == 3 && !memcmp(k, "FEN", 3)) { fen = vv; fl = vl; }
        else if (kl == 7 && !memcmp(k, "Variant", 7)) { if (vl >= 3 && !memcmp(vv + vl - 3, "960", 3)) c960 = 1; else if (!(vl == 8 && !memcmp(vv, "Standard", 8))) goto done; } }
    if (fen && !board_fen(&b, fen, fl, c960)) goto done;
    lp_id *traj = malloc(sizeof(lp_id) * (2 * cap)); if (!traj) goto done;
    size_t nt = 0; id_position(&b, &positions[0]); traj[nt++] = positions[0]; written = 1;
    for (uint32_t i = 0; i < ts_node_named_child_count(node) && written < cap; i++) { TSNode c = ts_node_named_child(node, i); if (ts_node_symbol(c) != san) continue;
        Mv all[256]; int na = gen(&b, all);
        int k = san_match(&b, text + ts_node_start_byte(c), ts_node_end_byte(c) - ts_node_start_byte(c), all, na);
        if (k < 0) { written = 0; break; }
        id_move(&b, &all[k], &traj[nt++]); make(&b, &all[k]); id_position(&b, &positions[written]); traj[nt++] = positions[written++]; }
    if (written && line) lp_id_compose(traj, nt, line);
    free(traj);
done:
    ts_tree_delete(tree); return written;
}
