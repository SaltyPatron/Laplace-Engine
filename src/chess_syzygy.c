/* Syzygy endgame tablebases as a source: the endgame trajectories the tables give from where recorded games stand
 * (input decode syzygy, recipes/syzygy). Sequence 27.1-27.2: a game is P0 -> M0 -> P1 ...; tablebases are endings,
 * trajectories that converge with the games' and with each other's.
 *
 * A table's verdicts are not its content to record: a verdict is a deterministic read of the tables
 * (chess_tablebase.c: laplace tablebase, and Laplace-MCP's tablebase read), so no position is recorded for being in a
 * table. What the source records is what the tables add to what the database holds: for every position of the tables'
 * material that a recorded line reaches (a game, an opening, any record whose content holds a line), the best-play
 * line from it to its end, a mate, through each zeroing move into the smaller table and on; written as the PGN a game
 * of that line is ([FEN "P0"], its SAN, the result under the 50-move rule), so chess.c composes it as it composes every
 * game: the line [P0, M0, P1, ..., Pn], and for each move the [P, M, P'] a game that plays it claims, scored for the
 * side that moves as a game's result scores it, attested by the source's witness. A line another reached position's
 * line already holds from that position on, with the same result, is not written again: it is that line's tail.
 *
 * The reached positions are read from the database the ingest loads into (ingest_database), through the Engine's
 * reader, a level a round trip: the trunks of the sources whose recipes read chess (grammar pgn, or a part read by
 * it), down to their records (said RECORD); from each record to the line it holds (a composition of an odd number of
 * constituents, at least three, whose first and last are positions), its ends alone read (ends_of); the line's last
 * position decoded, and for a line that ends within the tables' pieces every position of it. Positions are decoded from
 * their constituents by the IDs chess_rules composes ([piece, square], the side, the castling masks, the en passant
 * square), each checked by composing its ID again. Positions with castling rights are in no table.
 *
 * Every table file of the source is read by the recipe (*.rtbw): the lines that begin in its material, either colouring,
 * in their positions' ID order. The work is done once a run, at the first table opened; each table's text is then
 * written out from it. */
#include "engine.h"
#include "chess_tablebase.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_PLIES 2048
#define LEVEL_CHUNK 262144

/* ---- a position's constituents, decoded by the IDs chess_rules gives them */
typedef struct { lp_idmap *pair, *square; lp_id white, black, none, castle_none; } Decode;
static void decode_init(Decode *d){
    d->pair = lp_idmap_sized(sizeof(int)); d->square = lp_idmap_sized(sizeof(int));
    for (int s = 0; s < 64; s++) { lp_id k[2]; lp_id_codepoint((uint32_t)('a' + (s & 7)), &k[0]); lp_id_codepoint((uint32_t)('1' + (s >> 3)), &k[1]);
        lp_id sq; lp_id_compose(k, 2, &sq); *(int *)lp_idmap_get(d->square, &sq, NULL) = s;
        for (int p = -KING; p <= KING; p++) { if (!p) continue; lp_id two[2]; lp_id_codepoint(piece_cp((int8_t)p), &two[0]); two[1] = sq;
            lp_id pr; lp_id_compose(two, 2, &pr); *(int *)lp_idmap_get(d->pair, &pr, NULL) = (p + 6) * 64 + s; } }
    lp_id_codepoint('w', &d->white); lp_id_codepoint('b', &d->black); lp_id_codepoint('-', &d->none);
    lp_id z[2]; lp_id_codepoint('0', &z[0]); z[1] = z[0]; lp_id_compose(z, 2, &d->castle_none);
}
static void decode_free(Decode *d){ lp_idmap_free(d->pair); lp_idmap_free(d->square); }
/* A position's four parts: 1 where they are a position the tables can hold (no castling); its board's ID in board. */
static int position_parts(const Decode *d, const lp_id *k, size_t n, Bd *b, lp_id *board){
    if (n != 4) return 0;
    memset(b, 0, sizeof *b); b->ep = -1;
    if (lp_id_eq(&k[1], &d->white)) b->side = 0; else if (lp_id_eq(&k[1], &d->black)) b->side = 1; else return 0;
    if (!lp_id_eq(&k[2], &d->castle_none)) return 0;
    if (!lp_id_eq(&k[3], &d->none)) { int64_t s = lp_idmap_find(d->square, &k[3]); if (s < 0) return 0; b->ep = (int8_t)*(int *)lp_idmap_at(d->square, (size_t)s); }
    *board = k[0]; return 1;
}
static int board_parts(const Decode *d, const lp_id *k, size_t n, Bd *b){
    for (size_t i = 0; i < n; i++) { int64_t at = lp_idmap_find(d->pair, &k[i]); if (at < 0) return 0;
        int v = *(int *)lp_idmap_at(d->pair, (size_t)at); b->sq[v % 64] = (int8_t)(v / 64 - 6); }
    return king_sq(b, 0) >= 0 && king_sq(b, 1) >= 0;
}

/* ---- the reached positions */
typedef struct { lp_id id; Bd b; int wdl, plies, covered, failed; Mv *line; char result[8]; } Start;
typedef struct { lp_idmap *at; Start *s; size_t n, cap; int pieces; uint64_t records, lines, ending, positions; } Reach;
static void reach_add(Reach *r, const lp_id *id, const Bd *b){
    bool fresh; size_t i = lp_idmap_put(r->at, id, &fresh); if (!fresh) return;
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 1024; r->s = realloc(r->s, sizeof(Start) * r->cap); }
    Start *s = &r->s[r->n++]; memset(s, 0, sizeof *s); s->id = *id; s->b = *b; (void)i;
}
typedef lp_vec(lp_id) IdList;
static void ids_unique(IdList *v){                                           /* in place, first occurrence kept */
    lp_idmap *m = lp_idmap_new(); size_t k = 0;
    for (size_t i = 0; i < v->n; i++) { bool fresh; lp_idmap_put(m, &v->v[i], &fresh); if (fresh) v->v[k++] = v->v[i]; }
    v->n = k; lp_idmap_free(m);
}
/* Positions decoded: each ID's position where it is one the tables can hold, at most r->pieces pieces, checked by its
 * ID composed again. out[i] set and ok[i] 1 where it is. Two round trips: the positions' parts, then their boards'. */
static void positions_decode(PGconn *pg, const Decode *d, int pieces, const lp_id *ids, size_t n, Bd *out, uint8_t *ok){
    memset(ok, 0, n);
    for (size_t at = 0; at < n; at += LEVEL_CHUNK) {
        size_t k = n - at < LEVEL_CHUNK ? n - at : LEVEL_CHUNK; int nh = 0;
        Hold *h = paths_of(pg, ids + at, (int)k, &nh);
        lp_id *board = calloc(k, sizeof(lp_id)); uint8_t *has = calloc(k, 1); Ids parts = { 0 };
        for (int j = 0; j < nh; j++) { size_t i = (size_t)h[j].src; if (i >= k) continue;
            path_into(&parts, lp_path_of(h[j].path, (size_t)h[j].path_len));
            if (position_parts(d, parts.v, parts.n, &out[at + i], &board[i])) has[i] = 1; }
        holds_free(h, nh);
        /* the boards: their lengths first (a board is its pieces), then the paths of those within the tables */
        IdList want = { 0 }; for (size_t i = 0; i < k; i++) if (has[i]) lp_push(&want, board[i]);
        size_t ne = 0; Ends *e = ends_of(pg, want.v, want.n, &ne); lp_idmap *small = lp_idmap_new();
        for (size_t j = 0; j < ne; j++) if (e[j].length <= pieces) lp_idmap_put(small, &e[j].entity, NULL);
        free(e); want.n = 0;
        for (size_t j = 0; j < lp_idmap_count(small); j++) lp_push(&want, lp_idmap_keys(small)[j]);
        int nb = 0; Hold *bh = paths_of(pg, want.v, (int)want.n, &nb);
        lp_idmap *bset = lp_idmap_sized(sizeof(int));
        for (int j = 0; j < nb; j++) *(int *)lp_idmap_get(bset, &bh[j].entity, NULL) = j;
        for (size_t i = 0; i < k; i++) { if (!has[i]) continue; int64_t at2 = lp_idmap_find(bset, &board[i]); if (at2 < 0) continue;
            const Hold *x = &bh[*(int *)lp_idmap_at(bset, (size_t)at2)];
            path_into(&parts, lp_path_of(x->path, (size_t)x->path_len));
            Bd *b = &out[at + i]; if (!board_parts(d, parts.v, parts.n, b)) continue;
            lp_id again; id_position(b, &again); ok[at + i] = lp_id_eq(&again, &ids[at + i]); }
        holds_free(bh, nb); lp_idmap_free(bset); lp_idmap_free(small); lp_vec_free(&want); lp_vec_free(&parts); free(board); free(has); }
}
/* Whether each ID is a position at all (its parts are four, the side w or b): for telling a line from other content. */
static void positions_are(PGconn *pg, const Decode *d, const lp_id *ids, size_t n, uint8_t *ok){
    memset(ok, 0, n);
    for (size_t at = 0; at < n; at += LEVEL_CHUNK) {
        size_t k = n - at < LEVEL_CHUNK ? n - at : LEVEL_CHUNK; size_t ne = 0;
        Ends *e = ends_of(pg, ids + at, k, &ne); IdList want = { 0 }; lp_idmap *four = lp_idmap_new();
        for (size_t j = 0; j < ne; j++) if (e[j].length == 4) lp_idmap_put(four, &e[j].entity, NULL);
        for (size_t j = 0; j < lp_idmap_count(four); j++) lp_push(&want, lp_idmap_keys(four)[j]);
        int nh = 0; Hold *h = paths_of(pg, want.v, (int)want.n, &nh); lp_idmap *pos = lp_idmap_new(); Ids parts = { 0 };
        for (int j = 0; j < nh; j++) { path_into(&parts, lp_path_of(h[j].path, (size_t)h[j].path_len));
            if (parts.n == 4 && (lp_id_eq(&parts.v[1], &d->white) || lp_id_eq(&parts.v[1], &d->black))) lp_idmap_put(pos, &h[j].entity, NULL); }
        for (size_t i = 0; i < k; i++) ok[at + i] = lp_idmap_find(pos, &ids[at + i]) >= 0;
        holds_free(h, nh); lp_idmap_free(pos); lp_idmap_free(four); lp_vec_free(&want); lp_vec_free(&parts); free(e); }
}
/* The witnesses' trunks of the sources whose recipes read chess, but this one (whose recipe decodes its input). */
static void chess_trunks(PGconn *pg, IdList *trunks){
    int nsrc, nrec; Source *src = sources_loaded(&nsrc); Recipe *rec = NULL; nrec = recipes_load(recipes_directory(), &rec);
    const NativeGrammar *pgn = native_grammar("pgn"); lp_vec(const char *) names = { 0 };
    for (int i = 0; i < nsrc; i++) { int chess = 0;
        for (int k = 0; k < nrec; k++) if (rec[k].source == i && !rec[k].decoder && !rec[k].broken && ((rec[k].native == pgn) || say_part_native(&rec[k], pgn))) chess = 1;
        const char *w = chess ? source_called(&src[i]) : NULL; if (w) lp_push(&names, w); }
    Args a = { 0 }; PGresult *q = ask_try(pg, "SELECT witness FROM @extschema@.laplace_witness_trust()", &a); args_free(&a);
    if (!q) return;
    Reader *rd = reader_new(pg);
    for (int r = 0; r < PQntuples(q); r++) { const lp_id *w = col_id(q, r, 0); lp_id first[2]; char *nm = reader_parts(rd, w, first, 2) ? reader_text(rd, &first[0], 256) : NULL;
        for (size_t i = 0; nm && i < names.n; i++) if (!strcmp(nm, names.v[i])) { lp_push(trunks, *w); fprintf(stderr, "  syzygy: the lines recorded under %s\n", nm); break; }
        free(nm); }
    reader_free(rd); PQclear(q); lp_vec_free(&names);
}
/* Down each trunk to its records, a level a round trip (Laplace-MCP's chess games read walks the same): below a trunk
 * its files' trunks, below them a vertex said HOLDS holds records, one said RECORD is a record as its file holds it. */
static void records_under(PGconn *pg, const IdList *trunks, IdList *records){
    IdList level = { 0 }; lp_idmap *seen = lp_idmap_new();
    for (size_t i = 0; i < trunks->n; i++) { bool fresh; lp_idmap_put(seen, &trunks->v[i], &fresh); if (fresh) lp_push(&level, trunks->v[i]); }
    for (int depth = 0; depth < 12 && level.n; depth++) {
        IdList next = { 0 };
        for (size_t at = 0; at < level.n; at += LEVEL_CHUNK) {
            size_t k = level.n - at < LEVEL_CHUNK ? level.n - at : LEVEL_CHUNK; int nh = 0; Hold *h = paths_of(pg, level.v + at, (int)k, &nh);
            for (int j = 0; j < nh; j++) { lp_path p = lp_path_of(h[j].path, (size_t)h[j].path_len);
                for (size_t v = 0; v < p.n; v++) { uint32_t said = lp_path_said(p, v);
                    if (said != LP_SAID_RECORD && said != LP_SAID_HOLDS && !(said == 0 && depth < 2 && v > 0)) continue;
                    lp_id id = lp_path_id(p, v); bool fresh; lp_idmap_put(seen, &id, &fresh); if (!fresh) continue;
                    if (said == LP_SAID_RECORD) lp_push(records, id); else lp_push(&next, id); } }
            holds_free(h, nh); }
        lp_vec_free(&level); level = next; }
    lp_vec_free(&level); lp_idmap_free(seen);
}
/* From records to the lines they hold, at most three levels down (a record held as [record, its claims...], a record
 * [line, tags, layer], a row whose field is the line): a line is a composition of an odd number of constituents, three
 * or more, whose first and last are positions. Below a composition its first constituent is looked at, and its last
 * where it has at most three (a row's last field); a record whose line of more than one move is found is looked at no
 * further. A claim [P, M, P'] is a line too, of the one move it is: what it holds is in its game's line as well. */
typedef struct { lp_id id; size_t rec; } Cand;
static void lines_of(PGconn *pg, const Decode *d, const lp_id *records, size_t n, IdList *lines, IdList *ends){
    lp_vec(Cand) cand = { 0 }; for (size_t i = 0; i < n; i++) { Cand c = { records[i], i }; lp_push(&cand, c); }
    uint8_t *done = calloc(n ? n : 1, 1); IdList ids = { 0 };
    for (int depth = 0; depth < 4 && cand.n; depth++) {
        ids.n = 0; for (size_t i = 0; i < cand.n; i++) lp_push(&ids, cand.v[i].id);
        size_t ne = 0; Ends *e = ends_of(pg, ids.v, ids.n, &ne);
        IdList check = { 0 }; for (size_t j = 0; j < ne; j++) if (e[j].length >= 3 && (e[j].length & 1)) { lp_push(&check, e[j].first); lp_push(&check, e[j].last); }
        uint8_t *pos = malloc(check.n ? check.n : 1), *line = calloc(ne ? ne : 1, 1); positions_are(pg, d, check.v, check.n, pos);
        for (size_t j = 0, c = 0; j < ne; j++) if (e[j].length >= 3 && (e[j].length & 1)) { line[j] = pos[c] && pos[c + 1]; c += 2;
            if (!line[j]) continue;
            lp_push(lines, e[j].entity); lp_push(ends, e[j].last);
            if (e[j].length > 3) done[cand.v[e[j].src].rec] = 1; }
        lp_vec(Cand) next = { 0 };
        for (size_t j = 0; j < ne; j++) { size_t rec = cand.v[e[j].src].rec; if (line[j] || done[rec]) continue;
            Cand f = { e[j].first, rec }; lp_push(&next, f);
            if (e[j].length <= 3 && !lp_id_eq(&e[j].last, &e[j].first)) { Cand l = { e[j].last, rec }; lp_push(&next, l); } }
        free(pos); free(line); free(e); lp_vec_free(&check); lp_vec_free(&cand); cand.v = next.v; cand.n = next.n; cand.cap = next.cap; }
    lp_vec_free(&cand); lp_vec_free(&ids); free(done);
}
/* Every reached position: the lines under the chess sources' records whose last position is within the tables' pieces,
 * each of their positions that is. */
static void reach_read(PGconn *pg, const Decode *d, Reach *r){
    IdList trunks = { 0 }, records = { 0 }; double t = now();
    chess_trunks(pg, &trunks); records_under(pg, &trunks, &records); r->records = records.n;
    fprintf(stderr, "  syzygy: %zu trunks, %zu records (%.1f s)\n", trunks.n, records.n, now() - t);
    for (size_t at = 0; at < records.n; at += LEVEL_CHUNK) {
        size_t k = records.n - at < LEVEL_CHUNK ? records.n - at : LEVEL_CHUNK;
        IdList lines = { 0 }, ends = { 0 }; lines_of(pg, d, records.v + at, k, &lines, &ends); r->lines += lines.n;
        Bd *b = malloc(sizeof(Bd) * (ends.n ? ends.n : 1)); uint8_t *ok = malloc(ends.n ? ends.n : 1);
        positions_decode(pg, d, r->pieces, ends.v, ends.n, b, ok);
        IdList ending = { 0 }; for (size_t i = 0; i < lines.n; i++) if (ok[i]) lp_push(&ending, lines.v[i]);
        free(b); free(ok); ids_unique(&ending); r->ending += ending.n;
        /* their positions, every one: the paths of the lines that end within the tables */
        IdList positions = { 0 }; Ids parts = { 0 };
        for (size_t j = 0; j < ending.n; j += 4096) { int nh = 0; size_t m = ending.n - j < 4096 ? ending.n - j : 4096; Hold *h = paths_of(pg, ending.v + j, (int)m, &nh);
            for (int x = 0; x < nh; x++) { path_into(&parts, lp_path_of(h[x].path, (size_t)h[x].path_len)); for (size_t v = 0; v < parts.n; v += 2) lp_push(&positions, parts.v[v]); }
            holds_free(h, nh); }
        ids_unique(&positions); r->positions += positions.n;
        b = malloc(sizeof(Bd) * (positions.n ? positions.n : 1)); ok = malloc(positions.n ? positions.n : 1);
        positions_decode(pg, d, r->pieces, positions.v, positions.n, b, ok);
        for (size_t i = 0; i < positions.n; i++) if (ok[i]) reach_add(r, &positions.v[i], &b[i]);
        free(b); free(ok); lp_vec_free(&parts); lp_vec_free(&positions); lp_vec_free(&ending); lp_vec_free(&lines); lp_vec_free(&ends); }
    fprintf(stderr, "  syzygy: %llu lines, %llu ending within %d pieces, %llu of their positions read, %zu within the tables (%.1f s)\n",
            (unsigned long long)r->lines, (unsigned long long)r->ending, r->pieces, (unsigned long long)r->positions, r->n, now() - t);
    lp_vec_free(&trunks); lp_vec_free(&records);
}
/* Each reached position's best-play line, on every core; then a line another one holds from its start on, with the
 * same result, is that line's tail and is not written again. */
static void reach_lines(LpTablebase *tb, Reach *r){
    double t = now();
    #pragma omp parallel for schedule(dynamic, 16)
    for (int64_t i = 0; i < (int64_t)r->n; i++) { Start *s = &r->s[i]; Mv *mv = malloc(sizeof(Mv) * LINE_PLIES);
        int n = lp_tablebase_line(tb, &s->b, mv, LINE_PLIES, s->result);
        if (n < 0) { s->failed = 1; free(mv); continue; }
        s->plies = n; if (!n) { free(mv); continue; }
        s->line = realloc(mv, sizeof(Mv) * (size_t)n); }
    /* tails: along each line, a reached position (not its start) with the same result is covered by it */
    #pragma omp parallel for schedule(dynamic, 16)
    for (int64_t i = 0; i < (int64_t)r->n; i++) { const Start *s = &r->s[i]; Bd cur = s->b;
        for (int p = 0; p < s->plies; p++) { make(&cur, &s->line[p]); lp_id id; id_position(&cur, &id); int64_t j = lp_idmap_find(r->at, &id);
            if (j >= 0 && j != i && r->s[j].plies && !strcmp(r->s[j].result, s->result)) __atomic_store_n(&r->s[j].covered, 1, __ATOMIC_RELAXED); } }
    uint64_t draws = 0, failed = 0, covered = 0, written = 0, plies = 0;
    for (size_t i = 0; i < r->n; i++) { const Start *s = &r->s[i]; if (s->failed) failed++; else if (!s->plies) draws++; else if (s->covered) covered++; else { written++; plies += (uint64_t)s->plies; } }
    fprintf(stderr, "  syzygy: %zu reached positions: %llu with no line (a draw, or mate already), %llu a tail of another's line, %llu lines of %llu plies, %llu the tables do not cover (%.1f s)\n",
            r->n, (unsigned long long)draws, (unsigned long long)covered, (unsigned long long)written, (unsigned long long)plies, (unsigned long long)failed, now() - t);
}

/* ---- once a run: the tables, the reached positions and their lines */
static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
static int once_state;                                                       /* 0 not yet, 1 ready, -1 failed */
static Reach REACH; static LpTablebase *TB;
static int reach_once(const char *table_path){
    pthread_mutex_lock(&once_mu);
    if (!once_state) {
        once_state = -1;
        /* the tables: those under the directory above the one this table is in (the source's root: syzygy/3-4-5/) */
        char root[1100]; snprintf(root, sizeof root, "%s", table_path); os_slashes(root);
        for (int up = 0; up < 2; up++) { char *s = strrchr(root, '/'); if (s) *s = 0; else snprintf(root, sizeof root, "."); }
        if (!*root) snprintf(root, sizeof root, "/");
        TB = lp_tablebase_open(root, 0);
        const char *conninfo = ingest_database(); PGconn *pg = TB ? PQconnectdb(conninfo) : NULL;
        if (TB && PQstatus(pg) != CONNECTION_OK) fprintf(stderr, "syzygy: the tables' lines begin where recorded games stand, read from the database: %s", PQerrorMessage(pg));
        else if (TB) {
            LpTablebaseStats st; lp_tablebase_stats(TB, &st); int pieces = st.pieces;          /* the most the tables hold: 5 of the 3-4-5 set */
            fprintf(stderr, "\n  syzygy: %d tables under %s; positions of up to %d pieces\n", st.tables, root, pieces);
            Decode d; decode_init(&d); REACH.at = lp_idmap_new(); REACH.pieces = pieces;
            reach_read(pg, &d, &REACH); decode_free(&d);
            reach_lines(TB, &REACH);
            lp_tablebase_stats(TB, &st);
            fprintf(stderr, "  syzygy: tables mapped %llu times, unmapped %llu, %d at once at most (bound %d)\n", (unsigned long long)st.maps, (unsigned long long)st.unmaps, st.peak, st.max_open);
            once_state = 1; }
        if (pg) PQfinish(pg); }
    int ok = once_state > 0; pthread_mutex_unlock(&once_mu); return ok;
}

/* ---- the decoder: a table file's lines, those that begin in its material */
typedef struct { lp_buf out; size_t at; int failed; } Decoder;
static int material_counts(const char *name, size_t n, int w[7], int k[7]){
    memset(w, 0, sizeof(int) * 7); memset(k, 0, sizeof(int) * 7); int *x = w, c = 0; static const char L[] = " PNBRQK";
    for (size_t i = 0; i < n; i++) { if (name[i] == 'v') { if (x == k) return 0; x = k; continue; } const char *p = strchr(L + 1, name[i]); if (!p || !name[i]) return 0; x[p - L]++; c++; }
    return c >= 3 && w[KING] == 1 && k[KING] == 1;
}
static int start_order(const void *a, const void *b){ return memcmp(&(*(const Start *const *)a)->id, &(*(const Start *const *)b)->id, sizeof(lp_id)); }
static void *syzygy_open(const char *path){
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\'); if (back > slash) slash = back;
    const char *base = slash ? slash + 1 : path, *dot = strrchr(base, '.'); int w[7], k[7];
    if (!dot || strcmp(dot, ".rtbw") || !material_counts(base, (size_t)(dot - base), w, k)) { fprintf(stderr, "%s: a Syzygy table is NAME.rtbw, its material\n", path); return NULL; }
    if (!reach_once(path)) return NULL;
    Decoder *d = calloc(1, sizeof *d);
    lp_vec(Start *) mine = { 0 };
    for (size_t i = 0; i < REACH.n; i++) { Start *s = &REACH.s[i]; if (!s->plies || s->covered) continue;
        int sw[7], sk[7]; memset(sw, 0, sizeof sw); memset(sk, 0, sizeof sk);
        for (int q = 0; q < 64; q++) { int8_t p = s->b.sq[q]; if (p > 0) sw[p]++; else if (p < 0) sk[-p]++; }
        if (!memcmp(sw, w, sizeof sw) && !memcmp(sk, k, sizeof sk)) lp_push(&mine, s);
        else if (!memcmp(sw, k, sizeof sw) && !memcmp(sk, w, sizeof sk)) lp_push(&mine, s); }
    if (mine.n > 1) qsort(mine.v, mine.n, sizeof(Start *), start_order);
    uint64_t plies = 0;
    for (size_t i = 0; i < mine.n; i++) { const Start *s = mine.v[i]; uint8_t *o = lp_buf_room(&d->out, 128 + 16 * (size_t)s->plies);
        size_t n = lp_tablebase_pgn(&s->b, s->line, s->plies, s->result, (char *)o); d->out.n -= 128 + 16 * (size_t)s->plies - n; plies += (uint64_t)s->plies; }
    fprintf(stderr, "\n  syzygy %.*s: %zu lines, %llu plies\n", (int)(dot - base), base, mine.n, (unsigned long long)plies);
    lp_vec_free(&mine); return d;
}
static size_t syzygy_read(void *context, uint8_t *out, size_t capacity){
    Decoder *d = context; size_t n = d->out.n - d->at; if (n > capacity) n = capacity;
    memcpy(out, d->out.b + d->at, n); d->at += n; return n;
}
static int syzygy_error(void *context){ Decoder *d = context; return d->failed; }
static void syzygy_close(void *context){ Decoder *d = context; lp_buf_free(&d->out); free(d); }
const InputDecoder syzygy_decoder = { "syzygy", syzygy_open, syzygy_read, syzygy_error, syzygy_close };

/* ---- laplace tablebase: a position's verdict, its moves ranked and its best-play line, from the tables alone */
static const char *const WDL_WORD[5] = { "loss", "blessed loss", "draw", "cursed win", "win" };
static int tablebase_one(LpTablebase *tb, const char *fen, size_t n, int line, int pgn){
    if (pgn) {
        Bd b; if (!board_fen(&b, fen, n, 0) || king_sq(&b, 0) < 0 || king_sq(&b, 1) < 0) { fprintf(stderr, "%.*s: no FEN\n", (int)n, fen); return 2; }
        Mv *mv = malloc(sizeof(Mv) * LINE_PLIES); char result[8]; int plies = lp_tablebase_line(tb, &b, mv, LINE_PLIES, result);
        if (plies < 0) { fprintf(stderr, "%.*s: the tables do not cover it\n", (int)n, fen); free(mv); return 1; }
        if (plies) { char *o = malloc(128 + 16 * (size_t)plies); size_t k = lp_tablebase_pgn(&b, mv, plies, result, o); fwrite(o, 1, k, stdout); free(o); }
        free(mv); return 0; }
    LpTbProbe *p = malloc(sizeof *p); int got = lp_tablebase_probe_fen(tb, fen, n, p); char hex[33];
    if (got < 0) { fprintf(stderr, "%.*s: no FEN\n", (int)n, fen); free(p); return 2; }
    id_text(&p->position, hex);
    if (got) printf("position\t%s\t%s\t%d\n", hex, WDL_WORD[p->wdl + 2], p->dtz); else printf("position\t%s\tnone\t\n", hex);
    for (int i = 0; i < p->nmoves; i++) { const LpTbMove *m = &p->move[i]; char ph[33], ch[33]; id_text(&m->position, ph); id_text(&m->claim, ch);
        if (got) printf("move\t%s\t%s\t%s\t%d\t%d\t%s\t%s\n", m->san, m->uci, WDL_WORD[m->wdl + 2], m->dtz, m->value, ph, ch);
        else printf("move\t%s\t%s\t\t\t\t%s\t%s\n", m->san, m->uci, ph, ch); }
    if (line && got) {
        LpTbPly *ply = malloc(sizeof(LpTbPly) * LINE_PLIES); lp_id lid; char result[8] = "";
        int k = lp_tablebase_line_fen(tb, fen, n, ply, LINE_PLIES, &lid, result); id_text(&lid, hex);
        if (k < 0) printf("line\t\t\t-1\n");
        else { printf("line\t%s\t%s\t%d\t", hex, result, k); for (int i = 0; i < k; i++) printf("%s%s", i ? " " : "", ply[i].san); printf("\n");
            for (int i = 0; i < k; i++) { char ch[33]; id_text(&ply[i].claim, ch); printf("claim\t%d\t%s\t%s\n", i + 1, ply[i].san, ch); } }
        free(ply); }
    free(p); return got ? 0 : 1;
}
int cmd_tablebase(int argc, char **argv){
    const char *root = NULL; int open = 0, line = 0, pgn = 0;
    int a = opts(argc, argv, (const Opt[]){ { .flag = "-T", .kind = 's', .at = &root }, { .flag = "-n", .kind = 'i', .at = &open }, { .flag = "--line", .kind = 'b', .at = &line }, { .flag = "--pgn", .kind = 'b', .at = &pgn }, { 0 } });
    if (a < argc && argv[a][0] == '-') { fputs("usage: laplace tablebase [-T tables] [-n open] [--line | --pgn] [FEN...]   (none: a FEN a line of standard input)\n", stderr); return 2; }
    LpTablebase *tb = lp_tablebase_open(root, open); if (!tb) return 1;
    int rc = 0;
    if (a < argc) for (int i = a; i < argc; i++) { int r = tablebase_one(tb, argv[i], strlen(argv[i]), line, pgn); if (r > rc) rc = r; }
    else { char buf[512]; while (fgets(buf, sizeof buf, stdin)) { size_t n = strcspn(buf, "\r\n"); if (!n) continue; int r = tablebase_one(tb, buf, n, line, pgn); if (r > rc) rc = r; } }
    LpTablebaseStats st; lp_tablebase_stats(tb, &st);
    fprintf(stderr, "tablebase: %d tables; mapped %llu times, unmapped %llu, %d at once at most (bound %d)\n", st.tables, (unsigned long long)st.maps, (unsigned long long)st.unmaps, st.peak, st.max_open);
    lp_tablebase_close(tb); fflush(stdout); return rc;
}
