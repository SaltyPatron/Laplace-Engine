/* Writing to PostgreSQL: trunk-to-leaf deduplication, binary COPY straight into each leaf partition on its own
 * connection, and the semantics (witnesses, the ledger, the consensus). SQL only fetches and writes. */
#define _GNU_SOURCE
#include "engine.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- binary COPY */

/* ---- partitions: a tier each, tiers deeper than 15 in the default; the largest tiers split again 16 ways by the
 * ID's first hex digit. Which tiers are split is the schema's to say: it is read from the database, never assumed. */
static uint8_t split[17];
static void parts_plan(PGconn *pg){
    PGresult *r = PQexec(pg, "SELECT c.relname FROM pg_class c WHERE c.relkind = 'r' AND c.relname ~ '^entity_t([0-9]+|x)_[0-9a-f]$'");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "partitions: %s", PQerrorMessage(pg)); exit(1); }
    memset(split, 0, sizeof split);
    for (int j = 0; j < PQntuples(r); j++) { const char *n = PQgetvalue(r, j, 0) + 8; int t = *n == 'x' ? 16 : atoi(n); if (t >= 0 && t <= 16) split[t] = 1; }
    PQclear(r);
}
static int part_of(const lp_id *id, uint8_t tier){ int t = tier < 16 ? tier : 16; return t * 16 + (split[t] ? id->b[0] >> 4 : 0); }
static void part_name(int p, const char *table, char *out, size_t cap){
    int t = p / 16, k = p % 16; char tier[8]; if (t == 16) snprintf(tier, sizeof tier, "x"); else snprintf(tier, sizeof tier, "%d", t);
    if (split[t]) snprintf(out, cap, "%s_t%s_%x", table, tier, k); else snprintf(out, cap, "%s_t%s", table, tier);
}
#define NPART (17 * 16)

/* "I have these IDs: which do you already have?" The client composed every node, so it knows each ID's tier; an ID's
 * tier and first hex digit name the one partition it can be in. The IDs of a partition go to that partition as one
 * sorted set and come back as the ones it holds: a set question answered from the ID index, never a search of the
 * tiers. What comes back is left out of what is written, with everything under it (Ingestion: Deduplication). */
typedef struct { const uint8_t *tiers; uint8_t *hit; } Probe;
static int probe_group(const lp_id *ids, uint64_t i, void *ctx){ return part_of(&ids[i], ((Probe *)ctx)->tiers[i]); }
static void probe_table(int g, int t, char *out, size_t cap, void *ctx){ (void)t; (void)ctx; part_name(g, "entity", out, cap); }
static void probe_hit(const PGresult *q, int r, const uint64_t *place, void *ctx){ ((Probe *)ctx)->hit[place[col_int(q, r, 0) - 1]] = 1; }
static uint8_t *recorded(PGconn **pg, int npg, const lp_id *ids, const uint8_t *tiers, uint64_t n){
    Probe pr = { tiers, calloc(n ? n : 1, 1) }; Groups by = { NPART, probe_group, NULL, probe_table, &pr };
    over_ids(pg, npg, ids, n, &by, 500000, "SELECT u.i FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i) WHERE EXISTS (SELECT 1 FROM %s e WHERE e.id = u.id)", probe_hit, &pr);
    return pr.hit;
}
static uint8_t *tiers_of(const lp_id *ids, uint64_t n){                       /* the tier the client composed each of them at */
    uint8_t *t = malloc(n ? n : 1); for (uint64_t i = 0; i < n; i++) { Node *x = table_find(&ids[i]); t[i] = x ? x->tier : 0; } return t;
}



typedef struct { uint64_t idx, h; } NRef;                                /* h: the node's Hilbert value, computed once */
static NRef *bucket[NPART]; static uint64_t nbucket[NPART];
static NRef nref(const Node *x){ lp_coord co; memcpy(co.m, x->m, 32); return (NRef){ (uint64_t)(x - NODE), lp_hilbert4(&co) }; }
static int by_hilbert(const void *a, const void *b){ uint64_t x = ((const NRef *)a)->h, y = ((const NRef *)b)->h; return x < y ? -1 : x > y; }
/* A partition's new rows go in Hilbert order (Atoms: the Hilbert value is for locality, ordering, and indexing; it is
 * never what partitions go by):
 * rows near each other in the 4-ball are written together, so the coordinate and Hilbert indexes take a run of
 * neighbours on the same pages instead of one row per page in hash order. */
static void write_node_rows(PGconn *pg, int p, uint64_t *rows_e, uint64_t *rows_p, int atoms_needed, int own_txn){
    char tn[64]; lp_copy c = { 0 }; char sql[160]; uint8_t geo[64 * 1024];
    lp_id *ids = NULL; uint64_t *runs = NULL; size_t idc = 0;
    qsort(bucket[p], nbucket[p], sizeof(NRef), by_hilbert);
    /* a partition's entities and their paths are one transaction: a load cut off between the two leaves no entity
     * without a physicality (Physicality: the counts match, or the system is wrong). The file trunks are written
     * inside the transaction that holds what they attested, which is already open: that one is not begun or ended here. */
    if (own_txn) { PGresult *b = PQexec(pg, "BEGIN"); if (PQresultStatus(b) != PGRES_COMMAND_OK) { fprintf(stderr, "begin: %s", PQerrorMessage(pg)); exit(1); } PQclear(b); }
    part_name(p, "entity", tn, sizeof tn); snprintf(sql, sizeof sql, "COPY %s (id, tier, coord, hilbert) FROM STDIN (FORMAT binary)", tn);
    copy_open(&c, pg, sql);
    if (atoms_needed && p / 16 == 0)
        for (uint32_t cp = 0; cp < LP_NCP; cp++) {
            if (part_of(&T0[cp].id, 0) != p) continue;
            double x[4]; for (int d = 0; d < 4; d++) x[d] = (double)T0[cp].m[d] / LP_FIXED_ONE;
            size_t gl = lp_ewkb_point4(x, geo, sizeof geo);
            lp_copy_row(&c, 4); lp_copy_field(&c, T0[cp].id.b, 16); lp_copy_i16(&c, 0); lp_copy_field(&c, geo, (uint32_t)gl); lp_copy_i64(&c, lp_hilbert_key(T0[cp].hilbert));
        }
    for (uint64_t b = 0; b < nbucket[p]; b++) {
        Node *x = &NODE[bucket[p][b].idx];
        double xm[4]; for (int d = 0; d < 4; d++) xm[d] = (double)x->m[d] / LP_FIXED_ONE;
        size_t gl = lp_ewkb_point4(xm, geo, sizeof geo);
        lp_copy_row(&c, 4); lp_copy_field(&c, x->id.b, 16); lp_copy_i16(&c, x->tier); lp_copy_field(&c, geo, (uint32_t)gl); lp_copy_i64(&c, lp_hilbert_key(bucket[p][b].h));
    }
    copy_close(&c); *rows_e = c.rows;
    part_name(p, "physicality", tn, sizeof tn); snprintf(sql, sizeof sql, "COPY %s (entity, tier, hilbert, path, mask) FROM STDIN (FORMAT binary)", tn);
    uint8_t mask[4 + 32]; lp_put_be(mask, 256, 4);         /* bit varying, binary: its length in bits, then its bytes, first bit first */
    copy_open(&c, pg, sql);
    if (atoms_needed && p / 16 == 0)
        for (uint32_t cp = 0; cp < LP_NCP; cp++) {
            if (part_of(&T0[cp].id, 0) != p) continue;
            uint64_t one = 1; size_t gl = lp_ewkb_runs(&T0[cp].id, &one, 1, geo, sizeof geo); memset(mask + 4, 0, 32);
            lp_copy_row(&c, 5); lp_copy_field(&c, T0[cp].id.b, 16); lp_copy_i16(&c, 0); lp_copy_i64(&c, lp_hilbert_key(T0[cp].hilbert)); lp_copy_field(&c, geo, (uint32_t)gl); lp_copy_field(&c, mask, 36);
        }
    for (uint64_t b = 0; b < nbucket[p]; b++) {
        Node *x = &NODE[bucket[p][b].idx];
        if (x->nv > idc) { idc = x->nv * 2; ids = xrealloc(ids, idc * sizeof(lp_id)); runs = xrealloc(runs, idc * 8); }
        for (uint32_t v = 0; v < x->nv; v++) { ids[v] = VTX[x->voff + v].id; runs[v] = VTX[x->voff + v].m; }
        size_t gl = lp_ewkb_runs(ids, runs, x->nv, NULL, 0); uint8_t *gp = gl > sizeof geo ? malloc(gl) : geo;
        lp_ewkb_runs(ids, runs, x->nv, gp, gl);
        /* the mask: what the row is, and the types it holds (each constituent that is a type of a mask field) */
        memset(mask + 4, 0, 32); for (int b = 0; b < 8; b++) if (x->kind & (1u << b)) mask[4 + (b >> 3)] |= (uint8_t)(0x80 >> (b & 7));
        if (HW) for (uint32_t v = 0; v < x->nv; v++) { int32_t b = lp_highway_mask_bit(HW, &ids[v]); if (b >= 0 && b < 256) mask[4 + (b >> 3)] |= (uint8_t)(0x80 >> (b & 7)); }
        lp_copy_row(&c, 5); lp_copy_field(&c, x->id.b, 16); lp_copy_i16(&c, x->tier); lp_copy_i64(&c, lp_hilbert_key(bucket[p][b].h)); lp_copy_field(&c, gp, (uint32_t)gl); lp_copy_field(&c, mask, 36);
        if (gp != geo) free(gp);
    }
    copy_close(&c); *rows_p = c.rows;
    if (own_txn) { PGresult *e = PQexec(pg, "COMMIT"); if (PQresultStatus(e) != PGRES_COMMAND_OK) { fprintf(stderr, "commit: %s", PQerrorMessage(pg)); exit(1); } PQclear(e); }
    lp_buf_free(&c.buf); free(ids); free(runs);
}

/* ---- standings: a map from claim ID to its slot */
typedef struct { lp_id id; lp_rating r; uint32_t matches, m0; uint8_t had, entered; } Standing;     /* m0: the matches it was recorded with */
static Standing *stand; static lp_idmap *smap; static uint64_t sn;          /* the standings in play, in the order met; found by claim ID */
/* The stock default a claim enters at: Glicko-2's rating for the unrated, and the uncertainty of the witness that brings
 * it (the deviation its trust plays with), unless the recipe gives this kind of statement its own. */
static Standing *stand_get(const lp_id *id, const Event *add, double trust){
    if (!add) { int64_t i = lp_idmap_find(smap, id); return i < 0 ? NULL : &stand[i]; }
    bool fresh; size_t i = lp_idmap_put(smap, id, &fresh); if (!fresh) return &stand[i];
    stand[i] = (Standing){ *id, { add->enter_rating, add->enter_deviation > 0 ? add->enter_deviation : lp_entry_deviation(trust), LP_GLICKO_VOLATILITY }, 0, 0, 0, 0 };  /* the stock default for its level of attestation */
    sn = lp_idmap_count(smap); return &stand[i];
}

/* What the ledger already holds, by the witnessed thing and the witness that witnessed it: the same witness witnessing
 * the same thing again is the observation it made before, read again (Content 11.12: a retried job does not multiply
 * the same witnessing), and is not written again. Another witness of the same lineage is a copy, and is. */
/* A pair of IDs as one key: side by side, 32 bytes. */
static void pair_put(lp_strmap *m, const lp_id *a, const lp_id *b, bool *fresh){ lp_id k[2] = { *a, *b }; lp_strmap_put(m, k, sizeof k, fresh); }
static bool pair_has(const lp_strmap *m, const lp_id *a, const lp_id *b){ lp_id k[2] = { *a, *b }; return m && lp_strmap_find(m, k, sizeof k) >= 0; }
static lp_strmap *ldg;
static void ldg_put(const lp_id *w, const lp_id *by){ if (!ldg) ldg = lp_strmap_sized(0); pair_put(ldg, w, by, NULL); }
static int ldg_has(const lp_id *w, const lp_id *by){ return pair_has(ldg, w, by); }
/* What a recorded standing says, read back into the one in play: m0, what it was recorded with. */
static void standing_row(const PGresult *q, int r, const uint64_t *place, void *ctx){
    (void)place; (void)ctx; Standing *s = stand_get(col_id(q, r, 0), NULL, 0); if (!s) return;
    s->r = col_rating(q, r, 1); s->matches = s->m0 = (uint32_t)col_int(q, r, 4); s->had = 1;
}
/* What the ledger says was witnessed, by which witness and lineage: once per lineage, and the pair ledgered. */
static void lineage_row(const PGresult *q, int r, const uint64_t *place, void *seen){
    (void)place; const lp_id *wit = col_id(q, r, 0), *wid = col_id(q, r, 1);
    pair_put(seen, wit, PQgetisnull(q, r, 2) ? wid : col_id(q, r, 2), NULL); ldg_put(wit, wid);
}
typedef struct { double trust; uint8_t known; } Own;                       /* a witness a source names statement by statement */
static void known_row(const PGresult *q, int r, const uint64_t *place, void *own){ (void)place; Own *o = lp_idmap_lookup(own, col_id(q, r, 0)); if (o) o->known = 1; }
/* One partition of the semantics, on one connection: the ledger's rows whose witnessed thing's ID begins with h, in
 * reading order (its order is the order of play), the new standings and the recorded ones updated. Each partition is
 * written by one connection, inside that connection's part of the batch's transaction (load: prepared, then committed
 * as one). Returns 0, or 1 when the database refused. */
static int part_write(PGconn *pg, int h, File *files, int nfiles, uint64_t *led, uint64_t *nnew, uint64_t *nupd){
    lp_copy lc = { 0 }; char sql[300]; snprintf(sql, sizeof sql, "COPY attestation_%x (claim, witness, score, position) FROM STDIN (FORMAT binary)", h);
    copy_open(&lc, pg, sql);
    for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i];
        if (e->kind == EV_MEMBER || (e->witnessed.b[0] >> 4) != h) continue;                /* witnessed within its record: the record's row */
        if (ldg_has(&e->witnessed, e->own_witness ? &e->witness : &files[fi].witness.id)) continue;      /* this witness's, already in the ledger */
        lp_copy_row(&lc, 4); lp_copy_field(&lc, e->witnessed.b, 16); lp_copy_field(&lc, e->own_witness ? e->witness.b : files[fi].witness.id.b, 16); lp_copy_f32(&lc, e->score);
        if (e->position) lp_copy_i32(&lc, (int32_t)e->position); else lp_copy_null(&lc); }
    copy_close(&lc); *led += lc.rows; lp_buf_free(&lc.buf);
    lp_copy c = { 0 }; snprintf(sql, sizeof sql, "COPY consensus_%x (claim, rating, deviation, volatility, matches) FROM STDIN (FORMAT binary)", h);
    copy_open(&c, pg, sql);
    for (uint64_t i = 0; i < sn; i++) { const Standing *s = &stand[i]; if (s->had || (s->id.b[0] >> 4) != h) continue;
        lp_copy_row(&c, 5); lp_copy_field(&c, s->id.b, 16); lp_copy_f64(&c, s->r.rating); lp_copy_f64(&c, s->r.deviation); lp_copy_f64(&c, s->r.volatility); lp_copy_i32(&c, (int32_t)s->matches); }
    copy_close(&c); *nnew += c.rows; lp_buf_free(&c.buf);
    Args ua = { 0 }; lp_vec(const Standing *) chunk = { 0 }; int bad = 0;
    for (uint64_t i0 = 0; i0 < sn && !bad; ) {                               /* recorded standings: set-based updates, five arrays side by side */
        chunk.n = 0; for (; i0 < sn && chunk.n < 100000; i0++) if (stand[i0].had && stand[i0].matches != stand[i0].m0 && (stand[i0].id.b[0] >> 4) == h) lp_push(&chunk, &stand[i0]);     /* moved by this batch's matchups */
        if (!chunk.n) continue;
        lp_buf b[5] = { { 0 } }; args_reset(&ua);
        lp_pg_array(&b[0], id_oid, chunk.n); for (int f = 1; f < 4; f++) lp_pg_array(&b[f], 701, chunk.n); lp_pg_array(&b[4], 23, chunk.n);
        for (size_t j = 0; j < chunk.n; j++) { const Standing *x = chunk.v[j];
            lp_pg_elem(&b[0], x->id.b, 16);
            lp_buf_be(&b[1], 8, 4); lp_buf_be_f64(&b[1], x->r.rating); lp_buf_be(&b[2], 8, 4); lp_buf_be_f64(&b[2], x->r.deviation); lp_buf_be(&b[3], 8, 4); lp_buf_be_f64(&b[3], x->r.volatility);
            lp_buf_be(&b[4], 4, 4); lp_buf_be(&b[4], x->matches, 4); }
        for (int f = 0; f < 5; f++) { arg_raw(&ua, b[f].b, b[f].n); lp_buf_free(&b[f]); }
        snprintf(sql, sizeof sql, "UPDATE consensus_%x s SET rating = u.r, deviation = u.d, volatility = u.v, matches = u.m "
            "FROM unnest($1::blake3[], $2::float8[], $3::float8[], $4::float8[], $5::int[]) AS u(c, r, d, v, m) WHERE s.claim = u.c", h);
        PGresult *u = ask_try(pg, sql, &ua);
        if (!u) { fprintf(stderr, "standing update: %s", PQerrorMessage(pg)); bad = 1; } else { PQclear(u); *nupd += chunk.n; }
    }
    args_free(&ua); lp_vec_free(&chunk); if (bad) return 1;
    return 0;
}
/* A statement that must succeed. */
static int must(PGconn *pg, const char *sql){
    PGresult *r = PQexec(pg, sql); int ok = PQresultStatus(r) == PGRES_COMMAND_OK || PQresultStatus(r) == PGRES_TUPLES_OK;
    if (!ok) fprintf(stderr, "%s: %s", sql, PQerrorMessage(pg)); PQclear(r); return ok;
}
/* A batch's transaction is in parts, one a connection, prepared and then committed (two-phase commit): part 0, which
 * holds the witnesses and the files' trunks, is prepared last and committed first, so its commit is the decision. A
 * part named 'laplace X j' is of the batch whose part 0 is transaction X. Left over after a stop: committed where X
 * committed, rolled back where X did not, part 0 itself committed where it was prepared (every part was). */
static int resolve_parts(PGconn *pg){
    for (int round = 0; round < 2; round++) {
        PGresult *r = PQexec(pg, "SELECT gid, split_part(gid, ' ', 3), pg_xact_status(split_part(gid, ' ', 2)::xid8) FROM pg_prepared_xacts "
                                 "WHERE gid LIKE 'laplace %' AND database = current_database() AND prepared < now() - interval '2 minutes' ORDER BY split_part(gid, ' ', 3)::int");     /* a load still committing its parts takes seconds: those it leaves alone */
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "prepared parts: %s", PQerrorMessage(pg)); PQclear(r); return 0; }
        for (int i = 0; i < PQntuples(r); i++) { const char *gid = PQgetvalue(r, i, 0), *part = PQgetvalue(r, i, 1), *st = PQgetvalue(r, i, 2); char sql[160];
            if (round == 0 && strcmp(part, "0")) continue;                       /* part 0 first: it decides */
            if (!strcmp(part, "0") || !strcmp(st, "committed")) snprintf(sql, sizeof sql, "COMMIT PREPARED '%s'", gid);
            else if (!strcmp(st, "aborted")) snprintf(sql, sizeof sql, "ROLLBACK PREPARED '%s'", gid);
            else { fprintf(stderr, "prepared part %s: its batch's decision is unknown (%s); left as it is\n", gid, st); continue; }
            if (!must(pg, sql)) { PQclear(r); return 0; }
            fprintf(stderr, "  %s, left from a batch that stopped\n", sql); }
        PQclear(r); }
    return 1;
}
int load_whole;
int load(const char *conninfo, int npg, File *files, int nfiles, LoadStats *st){
    table_kinds();                                                           /* what each child is said to be: on the child, for its mask */
    /* what a witness attested is a claim, whatever holds it or nothing does: its row says so, so every read finds it */
    for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { if (files[fi].ev.e[i].kind == EV_RECORD) continue;
        Node *c = table_find(&files[fi].ev.e[i].claim); if (c) c->kind |= (uint8_t)(1u << LP_KIND_CLAIM); }
    PGconn **pg = malloc(sizeof(PGconn *) * npg);
    for (int i = 0; i < npg; i++) {
        pg[i] = db_connect(conninfo);
        PQclear(PQexec(pg[i], "SET synchronous_commit = off"));
    }
    parts_plan(pg[0]);
    if (!resolve_parts(pg[0])) return 1;                                       /* a batch that stopped between its parts' commits, finished first */
    PGresult *r = PQexec(pg[0], "SELECT count(*) FROM entity WHERE tier = 0");
    int atoms_needed = !(PQresultStatus(r) == PGRES_TUPLES_OK && atoll(PQgetvalue(r, 0, 0)) == (long long)LP_NCP); PQclear(r);

    /* ---- trunk to leaf: a recorded node means its whole subtree is recorded, so nothing below it is checked */
    double t = now();
    uint64_t cap = 1 << 20, nf = 0; lp_id *front = malloc(cap * sizeof(lp_id));
    #define FPUSH(x) do { if (nf == cap) { cap *= 2; front = xrealloc(front, cap * sizeof(lp_id)); } front[nf++] = (x); } while (0)
    if (load_whole)                                                          /* every node is looked for: nothing is taken to be recorded because what holds it is */
        TABLE_EACH(x) if (!x->keep) { x->keep = 3; FPUSH(x->id); }
    /* The files first, by their trunks: a file whose trunk is recorded is recorded, with everything under it and
     * everything it attested, and nothing of it is looked for, played or written again. */
    { lp_id *trunk = malloc(sizeof(lp_id) * (size_t)(nfiles + 1)); int *of = malloc(sizeof(int) * (size_t)(nfiles + 1)); uint64_t nt = 0;
      for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { of[nt] = fi; trunk[nt++] = files[fi].file.id; }
      if (nt) { uint8_t *tt = malloc(nt); for (uint64_t i = 0; i < nt; i++) { Node *x = table_find(&trunk[i]); tt[i] = x ? x->tier : files[of[i]].file.tier; }
                uint8_t *hit = recorded(pg, npg, trunk, tt, nt); free(tt); st->checked += nt;
                for (uint64_t i = 0; i < nt; i++) if (hit[i]) { files[of[i]].known = 1; st->known++; st->found++; Node *x = table_find(&trunk[i]); if (x) x->keep = 2; }
                free(hit); }
      free(trunk); free(of); }
    for (int fi = 0; fi < nfiles; fi++) {
        if (files[fi].known || files[fi].skipped) continue;
        if (files[fi].has_file) { Node *x = table_find(&files[fi].file.id); if (x && !x->keep) { x->keep = 3; FPUSH(x->id); } }
        Node *x = table_find(&files[fi].trunk.id); if (x && !x->keep) { x->keep = 3; FPUSH(x->id); }
        if (files[fi].ev.n) { Node *w = table_find(&files[fi].witness.id); if (w && !w->keep) { w->keep = 3; FPUSH(w->id); } }
        if (files[fi].has_lineage) { Node *lin = table_find(&files[fi].lineage.id); if (lin && !lin->keep) { lin->keep = 3; FPUSH(lin->id); } }
        for (uint64_t j = 0; j < files[fi].nsaid; j++) { Node *x = table_find(&files[fi].said[j].id); if (x && !x->keep) { x->keep = 3; FPUSH(x->id); } }   /* a long file's parts, a stretch at a time: its trunk, written after its last, holds them, and a recorded trunk has all it holds recorded */
        for (uint64_t i = 0; i < files[fi].ev.n; i++) { Node *c = table_find(&files[fi].ev.e[i].claim); if (c && !c->keep) { c->keep = 3; FPUSH(c->id); }     /* a record's claims are under it */
            if (files[fi].ev.e[i].kind == EV_RECORD) { Node *w = table_find(&files[fi].ev.e[i].witnessed); if (w && !w->keep) { w->keep = 3; FPUSH(w->id); } }
            if (files[fi].ev.e[i].own_witness) { Node *w = table_find(&files[fi].ev.e[i].witness); if (w && !w->keep) { w->keep = 3; FPUSH(w->id); } } }
    }
    while (nf) {
        st->rounds++; st->checked += nf;
        uint8_t *ft = tiers_of(front, nf); uint8_t *hit = recorded(pg, npg, front, ft, nf); free(ft);
        uint64_t nn = 0; lp_id *next = malloc((nf + 1) * sizeof(lp_id)); uint64_t ncap = nf + 1;
        for (uint64_t i = 0; i < nf; i++) {
            Node *x = table_find(&front[i]);
            if (hit[i]) { x->keep = 2; st->found++; continue; }
            x->keep = 1; st->new_nodes++;
            for (uint32_t v = 0; v < x->nv; v++) {
                Node *ch = table_find(&VTX[x->voff + v].id);
                if (ch && !ch->keep) { ch->keep = 3; if (nn == ncap) { ncap *= 2; next = xrealloc(next, ncap * sizeof(lp_id)); } next[nn++] = ch->id; }
            }
        }
        free(hit); free(front); front = next; nf = nn; cap = ncap;
        fprintf(stderr, "\r  dedup round %llu: %llu new so far, %llu subtrees already recorded   ", (unsigned long long)st->rounds,
                (unsigned long long)st->new_nodes, (unsigned long long)st->found);
    }
    free(front); fputc('\n', stderr);
    /* a file's trunk is written last, after what the file attested: it is held back from the writing below */
    uint64_t nlast = 0;
    for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { Node *x = table_find(&files[fi].file.id); if (x && x->keep == 1) { x->keep = 5; nlast++; } }
    st->t_dedup += now() - t;

    /* ---- every leaf partition on its own connection; new nodes bucketed by partition once */
    t = now();
    { uint64_t cnt[NPART] = { 0 };
      TABLE_EACH(x) if (x->keep == 1) cnt[part_of(&x->id, x->tier)]++;
      for (int p = 0; p < NPART; p++) { bucket[p] = malloc(sizeof(NRef) * (cnt[p] + 1)); nbucket[p] = 0; }
      TABLE_EACH(x) if (x->keep == 1) {
          int p = part_of(&x->id, x->tier); bucket[p][nbucket[p]++] = nref(x); } } uint64_t re[NPART] = { 0 }, rp[NPART] = { 0 };
    /* A tier at a time, from the lowest: what a composition is made of is always of a lower tier than it, so whatever
     * is recorded has everything under it recorded, even if the writing is cut off. Trunk-to-leaf deduplication
     * rests on that. */
    for (int tier = 0; tier <= 16; tier++) {
        #pragma omp parallel for num_threads(npg) schedule(dynamic)
        for (int p = tier * 16; p < tier * 16 + 16; p++) {
            if (!nbucket[p] && !(atoms_needed && p / 16 == 0)) continue;         /* nothing new for this partition */
            write_node_rows(pg[omp_get_thread_num()], p, &re[p], &rp[p], atoms_needed, 1);
        }
    }
    for (int p = 0; p < NPART; p++) { st->ent_rows += re[p]; st->phy_rows += rp[p]; free(bucket[p]); bucket[p] = NULL; }
    st->t_copy += now() - t;

    /* ---- semantics: witnesses, the ledger, and standings played in reading order */
    t = now(); double tp = t; uint64_t nev = 0;      /* tp: where each part of it began */
    for (int fi = 0; fi < nfiles; fi++) { if (files[fi].known) { free(files[fi].ev.e); memset(&files[fi].ev, 0, sizeof files[fi].ev); } nev += files[fi].ev.n; }
    /* What the files attested and the files' trunks are written as one: either all of it is recorded or none is. */
    { PGresult *b = PQexec(pg[0], "BEGIN"); if (PQresultStatus(b) != PGRES_COMMAND_OK) { fprintf(stderr, "begin: %s", PQerrorMessage(pg[0])); return 1; } PQclear(b); }
    int nparts = npg < 16 ? npg : 16, begun = 1; char xid0[32] = "";      /* the parts of the batch's transaction; part 0's transaction ID names them */
    { PGresult *x = PQexec(pg[0], "SELECT pg_current_xact_id()"); if (PQresultStatus(x) != PGRES_TUPLES_OK) { fprintf(stderr, "transaction: %s", PQerrorMessage(pg[0])); return 1; }
      snprintf(xid0, sizeof xid0, "%s", PQgetvalue(x, 0, 0)); PQclear(x); }
    if (nev) {
        smap = lp_idmap_new(); stand = malloc(sizeof(Standing) * (nev + 1)); sn = 0;
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) if (files[fi].ev.e[i].kind != EV_RECORD) stand_get(&files[fi].ev.e[i].claim, &files[fi].ev.e[i], files[fi].trust);
        /* claims already recorded start from their recorded standing */
        /* the statistics are partitioned by the claim's first hex digit: each read goes to its one partition */
        Ids old = { 0 }; for (uint64_t i = 0; i < sn; i++) { Node *x = table_find(&stand[i].id); if (!x || x->keep != 1) lp_push(&old, stand[i].id); }
        Groups cons = by_digit("consensus"), ledger = by_digit("attestation");
        over_ids(pg, npg, old.v, old.n, &cons, 100000, "SELECT claim, rating, deviation, volatility, matches FROM %s WHERE claim = ANY($1::blake3[])", standing_row, NULL);
        lp_vec_free(&old);
        /* What was witnessed plays once per lineage: a copy of it is a row in the ledger and nothing more. What this
         * lineage witnessed before is read from the ledger, each claim from the one partition it is in (its ID's first
         * hex digit); what it witnesses in this run is kept here. */
        lp_strmap *seen = lp_strmap_sized(0); Ids wold = { 0 };
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i]; if (e->kind == EV_MEMBER) continue;
            Node *x = table_find(&e->witnessed); if (!x || x->keep != 1) lp_push(&wold, e->witnessed); }
        over_ids(pg, npg, wold.v, wold.n, &ledger, 100000, "SELECT a.claim, w.id, w.lineage FROM %s a JOIN witness w ON w.id = a.witness WHERE a.claim = ANY($1::blake3[])", lineage_row, seen);
        lp_vec_free(&wold);
        st->t_read += now() - tp; tp = now();
        /* The matchups, first in, first out: each attestation is played as one Glicko-2 matchup at the witness's trust.
         * A claim entering for the first time enters at its stock default and plays its first attestation from there;
         * every attestation plays the witness at the rating its record would enter at, with the deviation its trust
         * gives, and the outcome it attests. */
        /* Which attestations play, in reading order (what is witnessed plays once per lineage; a claim within a record
         * plays as its record does); then the plays, a claim's in its reading order, the claims on every core: a
         * standing is moved by its own claim's matchups and by nothing else, so the standings are those of one
         * reading in order. play: the core that plays each attestation, or none. */
        const int NP = 64; uint64_t ne = 0; for (int fi = 0; fi < nfiles; fi++) ne += files[fi].ev.n;
        uint8_t *play = malloc(ne ? ne : 1);
        { uint64_t x = 0;
          for (int fi = 0; fi < nfiles; fi++) {
            int copy = 0; const lp_id *flin = files[fi].has_lineage ? &files[fi].lineage.id : &files[fi].witness.id;
            for (uint64_t i = 0; i < files[fi].ev.n; i++, x++) {
                const Event *e = &files[fi].ev.e[i]; const lp_id *lin = e->own_witness ? &e->witness : flin; play[x] = 255;
                if (e->kind != EV_MEMBER) {                                  /* what is witnessed: once per lineage */
                    bool fresh; pair_put(seen, &e->witnessed, lin, &fresh); copy = !fresh;
                    if (e->kind == EV_RECORD) continue;
                }
                if (!copy) play[x] = (uint8_t)(e->claim.b[7] % NP);
            } } }
        lp_strmap_free(seen);
        #pragma omp parallel for schedule(dynamic, 1)
        for (int p = 0; p < NP; p++) { uint64_t x = 0;
            for (int fi = 0; fi < nfiles; fi++) { double trust = files[fi].trust;
                for (uint64_t i = 0; i < files[fi].ev.n; i++, x++) { if (play[x] != p) continue; const Event *e = &files[fi].ev.e[i];
                    Standing *s = stand_get(&e->claim, NULL, 0);
                    if (!s->had && !s->entered) s->entered = 1;
                    lp_attest(&s->r, trust, e->score, e->enter_rating, LP_ATTEST_TAU, LP_ATTEST_FLOOR); s->matches++; } } }
        free(play);
        st->t_play += now() - tp; tp = now();
        lp_copy c = { 0 };
        /* witnesses: each once, and only those the database does not know yet */
        lp_id *wid = malloc(sizeof(lp_id) * (size_t)nfiles); int *wfile = malloc(sizeof(int) * (size_t)nfiles), nw = 0;
        for (int fi = 0; fi < nfiles; fi++) if (files[fi].ev.n) {
            int k = 0; while (k < nw && memcmp(&wid[k], &files[fi].witness.id, 16)) k++;
            if (k == nw) { wid[nw] = files[fi].witness.id; wfile[nw++] = fi; }
        }
        /* witnesses a source names statement by statement: each is its own lineage, and plays at the source's trust */
        lp_idmap *own = lp_idmap_sized(sizeof(Own));
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i]; if (!e->own_witness) continue;
            bool fresh; Own *o = lp_idmap_get(own, &e->witness, &fresh); if (fresh) o->trust = files[fi].trust; }
        { Groups one = whole(); over_ids(pg, 1, lp_idmap_keys(own), lp_idmap_count(own), &one, 50000, "SELECT id FROM witness WHERE id = ANY($1::blake3[])", known_row, own); }
        uint8_t *known = calloc((size_t)(nw ? nw : 1), 1); lp_idmap *wat = lp_idmap_new(); for (int k = 0; k < nw; k++) *(uint32_t *)lp_idmap_get(wat, &wid[k], NULL) = (uint32_t)k;
        { Args wa = { 0 }; arg_ids(&wa, wid, (size_t)nw); PGresult *q = ask_try(pg[0], "SELECT id FROM witness WHERE id = ANY($1::blake3[])", &wa); args_free(&wa);
          if (!q) { fprintf(stderr, "witnesses: %s", PQerrorMessage(pg[0])); return 1; }
          for (int j = 0; j < PQntuples(q); j++) { int64_t k = lp_idmap_find(wat, col_id(q, j, 0)); if (k >= 0) known[k] = 1; }
          PQclear(q); }
        copy_open(&c, pg[0], "COPY witness (id, lineage, trust) FROM STDIN (FORMAT binary)");
        for (int k = 0; k < nw; k++) if (!known[k]) {
            int fi = wfile[k];
            lp_copy_row(&c, 3); lp_copy_field(&c, files[fi].witness.id.b, 16);
            if (files[fi].has_lineage) lp_copy_field(&c, files[fi].lineage.id.b, 16); else lp_copy_null(&c);
            lp_copy_f64(&c, files[fi].trust);
        }
        for (size_t i = 0; i < lp_idmap_count(own); i++) { const Own *o = lp_idmap_at(own, i); if (o->known || lp_idmap_find(wat, lp_idmap_key(own, i)) >= 0) continue;
            lp_copy_row(&c, 3); lp_copy_id(&c, lp_idmap_key(own, i)); lp_copy_null(&c); lp_copy_f64(&c, o->trust); }
        copy_close(&c); free(wid); free(wfile); free(known); lp_idmap_free(own); lp_idmap_free(wat);
        st->t_wit += now() - tp; tp = now();
        /* the ledger and the standings, a partition a connection at a time on every connection: each connection's
         * part of the batch's transaction (prepared and committed as one below) */
        uint64_t led = 0, nnew = 0, nupd = 0; int bad = 0;
        for (int j = 1; j < nparts; j++) if (!must(pg[j], "BEGIN")) return 1;
        begun = nparts;
        #pragma omp parallel for num_threads(nparts) schedule(static, 1) reduction(+:led, nnew, nupd, bad)
        for (int j = 0; j < nparts; j++) for (int h = j; h < 16; h += nparts) { uint64_t a = 0, b = 0, c = 0; bad += part_write(pg[j], h, files, nfiles, &a, &b, &c); led += a; nnew += b; nupd += c; }
        if (bad) return 1;
        st->led += led; st->std_new += nnew; st->std_upd += nupd;
        lp_strmap_free(ldg); ldg = NULL;
        st->t_led += now() - tp; tp = now();
    }
    if (nev) { free(stand); lp_idmap_free(smap); stand = NULL; smap = NULL; }
    st->t_sem += now() - t;

    /* the files' trunks: last */
    if (nlast) {
        uint64_t cnt[NPART] = { 0 };
        for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { Node *x = table_find(&files[fi].file.id); if (x && x->keep == 5) cnt[part_of(&x->id, x->tier)]++; }
        for (int p = 0; p < NPART; p++) { bucket[p] = malloc(sizeof(NRef) * (cnt[p] + 1)); nbucket[p] = 0; }
        for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { Node *x = table_find(&files[fi].file.id); if (!x || x->keep != 5) continue;
            int p = part_of(&x->id, x->tier); bucket[p][nbucket[p]++] = nref(x); x->keep = 1; }
        for (int p = 0; p < NPART; p++) { uint64_t e = 0, ph = 0; if (nbucket[p]) { write_node_rows(pg[0], p, &e, &ph, 0, 0); st->ent_rows += e; st->phy_rows += ph; } free(bucket[p]); bucket[p] = NULL; }
    }
    /* one transaction in parts: each prepared, part 0 last; then part 0 committed, which decides, and the rest */
    if (begun == 1) { if (!must(pg[0], "COMMIT")) return 1; }
    else { char sql[96];
        for (int j = 1; j < begun; j++) { snprintf(sql, sizeof sql, "PREPARE TRANSACTION 'laplace %s %d'", xid0, j); if (!must(pg[j], sql)) return 1; }
        snprintf(sql, sizeof sql, "PREPARE TRANSACTION 'laplace %s 0'", xid0); if (!must(pg[0], sql)) return 1;
        for (int j = 0; j < begun; j++) { snprintf(sql, sizeof sql, "COMMIT PREPARED 'laplace %s %d'", xid0, j); if (!must(pg[0], sql)) return 1; } }
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}

/* Whether every one of these compositions is recorded: one set question per partition. What is recorded has everything
 * under it recorded, so one composition of a file's content that is not means the file's trunk is not. */
int db_all_recorded(const char *conninfo, const lp_id *ids, const uint8_t *tiers, uint64_t n){
    if (!n) return 1;
    PGconn *pg = db_connect(conninfo); parts_plan(pg);
    uint8_t *hit = recorded(&pg, 1, ids, tiers, n); uint64_t got = 0; for (uint64_t i = 0; i < n; i++) got += hit[i];
    free(hit); PQfinish(pg); return got == n;
}
