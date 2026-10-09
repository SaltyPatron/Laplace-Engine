/* Writing to PostgreSQL: trunk-to-leaf deduplication, binary COPY straight into each leaf partition on its own
 * connection, and the semantics (witnesses and consensus; provenance is containment). SQL only fetches and writes. */
#define _GNU_SOURCE
#include "engine.h"
#include <arpa/inet.h>
#include <locale.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- binary COPY */
typedef struct { PGconn *pg; uint8_t *b; size_t n, cap; uint64_t rows, bytes; } Copy;
static void cflush(Copy *c){ if (c->n && PQputCopyData(c->pg, (const char *)c->b, (int)c->n) != 1) { fprintf(stderr, "COPY: %s", PQerrorMessage(c->pg)); exit(1); } c->bytes += c->n; c->n = 0; }
static void cput(Copy *c, const void *p, size_t n){ if (c->n + n > c->cap) cflush(c); if (n > c->cap) { c->cap = n * 2; c->b = xrealloc(c->b, c->cap); } memcpy(c->b + c->n, p, n); c->n += n; }
static void c16(Copy *c, uint16_t v){ uint8_t b[2] = { v >> 8, v }; cput(c, b, 2); }
static void c32(Copy *c, uint32_t v){ uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; cput(c, b, 4); }
static void c64(Copy *c, uint64_t v){ uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i)); cput(c, b, 8); }
static void cfield(Copy *c, const void *p, uint32_t n){ c32(c, n); cput(c, p, n); }
static void cf_i16(Copy *c, int16_t v){ c32(c, 2); c16(c, (uint16_t)v); }
static void cf_i32(Copy *c, int32_t v){ c32(c, 4); c32(c, (uint32_t)v); }
static void cf_i64(Copy *c, int64_t v){ c32(c, 8); c64(c, (uint64_t)v); }
static void cf_f64(Copy *c, double v){ uint64_t u; memcpy(&u, &v, 8); c32(c, 8); c64(c, u); }
static void copy_begin(Copy *c, PGconn *pg, const char *sql){
    c->pg = pg; if (!c->b) { c->cap = 1 << 22; c->b = malloc(c->cap); } c->n = 0; c->rows = 0;
    PGresult *r = PQexec(pg, sql); if (PQresultStatus(r) != PGRES_COPY_IN) { fprintf(stderr, "%s: %s", sql, PQerrorMessage(pg)); exit(1); } PQclear(r);
    static const uint8_t hdr[19] = { 'P','G','C','O','P','Y','\n',0xFF,'\r','\n',0, 0,0,0,0, 0,0,0,0 }; cput(c, hdr, 19);
}
static void copy_end(Copy *c){
    c16(c, 0xFFFF); cflush(c);
    if (PQputCopyEnd(c->pg, NULL) != 1) { fprintf(stderr, "COPY end: %s", PQerrorMessage(c->pg)); exit(1); }
    PGresult *r; while ((r = PQgetResult(c->pg))) { if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "COPY: %s", PQerrorMessage(c->pg)); exit(1); } PQclear(r); }
}
static int64_t hsigned(uint64_t h){ return (int64_t)(h ^ 0x8000000000000000ull); }      /* bigint order = Hilbert order */

/* ---- partitions: an entity is its ID, so entity and physicality partition by the ID alone, 256 ways by its first byte
 * (entity_00 .. entity_ff, physicality_00 .. physicality_ff, the same ranges). A node's tier is a column of its row,
 * never where it is looked for: the same content is one row however it is composed. */
static void parts_plan(PGconn *pg){ (void)pg; }
static int part_of(const lp_id *id, uint8_t tier){ (void)tier; return id->b[0]; }
static void part_name(int p, const char *table, char *out, size_t cap){ snprintf(out, cap, "%s_%02x", table, p); }
#define NPART 256

/* ---- the stage: what an ingest has deduplicated and played, held until it goes into the real tables at once (merge).
 * The inventor: "decompose and stage all the records necessary... deduplicated and all of that but just the records...
 * and then we batch that into the real database". A stage table is UNLOGGED, one for each leaf partition of entity,
 * physicality and consensus, in the schema stage under the leaf's own name: what is staged writes no WAL.
 * Lookups during an ingest read the leaf and its stage table both. A crash empties every stage table (unlogged) and
 * leaves the real tables as they were before the run: the run is begun again. */
static int stage_ready;
/* A run begins with an empty stage. A merge that did not go through leaves the stage as it was, so the run can be
 * begun again; the run begun again stages everything again, and what the stage held from before would go in twice
 * (measured: a second unicode run's merge failed on its own duplicate ids). So what a run that did not finish left
 * is emptied first, and said. */
static int stage_emptied(PGconn *pg){
    PGresult *r = PQexec(pg, "DO $$ DECLARE r record; e boolean; n int := 0; BEGIN"
                             "  FOR r IN SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'stage' AND c.relkind = 'r' LOOP"
                             "    EXECUTE format('SELECT EXISTS (SELECT 1 FROM stage.%I)', r.relname) INTO e;"
                             "    IF e THEN EXECUTE format('TRUNCATE stage.%I', r.relname); n := n + 1; END IF;"
                             "  END LOOP;"
                             "  IF n > 0 THEN RAISE NOTICE 'the stage held rows in % tables from a run that did not finish: emptied', n; END IF;"
                             "END $$");
    int ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    if (!ok) fprintf(stderr, "the stage: %s", PQerrorMessage(pg)); PQclear(r);
    return ok;
}
static int stage_open(PGconn *pg){
    if (stage_ready) return 1;
    /* made once: every leaf has its stage table already when the stage holds as many tables as the real tables have
     * leaves (measured: the CREATE ... IF NOT EXISTS of all of them took 7.3 s each time a process asked) */
    { PGresult *q = PQexec(pg, "SELECT (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'stage' AND c.relkind = 'r' AND c.relname ~ '^(entity|physicality|consensus)_') "
                               "= (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'public' AND c.relkind = 'r' "
                               "AND c.relispartition AND c.relname ~ '^(entity|physicality|consensus)_')");
      int made = PQresultStatus(q) == PGRES_TUPLES_OK && PQntuples(q) && PQgetvalue(q, 0, 0)[0] == 't'; PQclear(q);
      if (made) return stage_ready = stage_emptied(pg); }
    const char *sql =
        "CREATE SCHEMA IF NOT EXISTS stage;"
        "DO $$ DECLARE r record; BEGIN"
        "  FOR r IN SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace"
        "           WHERE n.nspname = 'public' AND c.relkind = 'r' AND c.relispartition"
        "             AND c.relname ~ '^(entity|physicality|consensus)_' LOOP"
        "    EXECUTE format('CREATE UNLOGGED TABLE IF NOT EXISTS stage.%I (LIKE public.%I INCLUDING DEFAULTS)', r.relname, r.relname);"
        "    IF r.relname LIKE 'consensus%' THEN EXECUTE format('CREATE UNIQUE INDEX IF NOT EXISTS %I ON stage.%I (claim)', r.relname || '_claim', r.relname);"
        "    END IF;"
        "  END LOOP; END $$";
    PGresult *r = PQexec(pg, sql); int ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    if (!ok) fprintf(stderr, "the stage: %s", PQerrorMessage(pg)); PQclear(r);
    return stage_ready = ok && stage_emptied(pg);
}

/* "I have these IDs: which do you already have?" The client composed every node, so it knows each ID's tier; an ID's
 * tier and first hex digit name the one partition it can be in. The IDs of a partition go to that partition as one
 * sorted set and come back as the ones it holds: a set question answered from the ID index, never a search of the
 * tiers. What comes back is left out of what is written, with everything under it (Ingestion: Deduplication). */
static int by_id(const void *a, const void *b, void *ids){ return memcmp(&((const lp_id *)ids)[*(const uint64_t *)a], &((const lp_id *)ids)[*(const uint64_t *)b], 16); }
static uint8_t *recorded(PGconn **pg, int npg, const lp_id *ids, const uint8_t *tiers, uint64_t n){
    uint8_t *hit = calloc(n ? n : 1, 1); const uint64_t CH = 500000;
    uint64_t *cnt = calloc(NPART + 1, 8); for (uint64_t i = 0; i < n; i++) cnt[part_of(&ids[i], tiers[i]) + 1]++;
    for (int p = 0; p < NPART; p++) cnt[p + 1] += cnt[p];
    uint64_t *at = malloc(sizeof(uint64_t) * (n + 1)), *fill = malloc(sizeof(uint64_t) * NPART); memcpy(fill, cnt, sizeof(uint64_t) * NPART);
    for (uint64_t i = 0; i < n; i++) at[fill[part_of(&ids[i], tiers[i])]++] = i;       /* the IDs, by the partition each can be in */
    typedef struct { int p; uint64_t lo, n; } Job; Job *job = malloc(sizeof(Job) * (n / CH + NPART + 1)); uint64_t nj = 0;
    for (int p = 0; p < NPART; p++) { if (cnt[p + 1] == cnt[p]) continue;
        qsort_r(at + cnt[p], cnt[p + 1] - cnt[p], sizeof(uint64_t), by_id, (void *)ids);                    /* in ID order: the index is read along, not hopped across */
        for (uint64_t lo = cnt[p]; lo < cnt[p + 1]; lo += CH) job[nj++] = (Job){ p, lo, cnt[p + 1] - lo < CH ? cnt[p + 1] - lo : CH }; }
    #pragma omp parallel for num_threads(npg) schedule(dynamic)
    for (uint64_t j = 0; j < nj; j++) {
        uint32_t k = (uint32_t)job[j].n; lp_id *part = malloc(sizeof(lp_id) * k); for (uint32_t i = 0; i < k; i++) part[i] = ids[at[job[j].lo + i]];
        uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t len = ids_param(ab, part, k); PGconn *c = pg[omp_get_thread_num()];
        char tn[64], sql[256]; part_name(job[j].p, "entity", tn, sizeof tn);
        snprintf(sql, sizeof sql, "SELECT u.i FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i) WHERE EXISTS (SELECT 1 FROM public.%s e WHERE e.id = u.id)", tn);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
        PGresult *r = PQexecParams(c, sql, 1, NULL, v, l, f, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "dedup: %s", PQerrorMessage(c)); exit(1); }
        for (int x = 0; x < PQntuples(r); x++) hit[at[job[j].lo + (uint64_t)atoll(PQgetvalue(r, x, 0)) - 1]] = 1;
        PQclear(r); free(ab); free(part);
    }
    free(at); free(job); free(cnt); free(fill);
    return hit;
}



typedef struct { uint64_t idx, h; } NRef;                                /* h: the node's Hilbert value, computed once */
static NRef *bucket[NPART]; static uint64_t nbucket[NPART];
static NRef nref(const Node *x){ lp_coord co; memcpy(co.m, x->m, 32); return (NRef){ (uint64_t)(x - NODE), lp_hilbert4(&co) }; }
static int by_hilbert(const void *a, const void *b){ uint64_t x = ((const NRef *)a)->h, y = ((const NRef *)b)->h; return x < y ? -1 : x > y; }
/* A partition's new rows go in Hilbert order (Atoms: the Hilbert value is for locality, ordering, and indexing; it is
 * never what partitions go by):
 * rows near each other in the 4-ball are written together, so the coordinate and Hilbert indexes take a run of
 * neighbours on the same pages instead of one row per page in hash order. */
/* Rows lo..hi of a partition's bucket, which is in Hilbert order already: a stretch of the partition, so that a
 * partition is written by as many connections as it has stretches. A codepoint is never written here: tier 0 is the
 * recorded once by the source that records it (tier0_write). */
static void write_node_rows(PGconn *pg, int p, uint64_t lo, uint64_t hi, uint64_t *rows_e, uint64_t *rows_p, int own_txn){
    char tn[64]; Copy c = { 0 }; char sql[160]; uint8_t geo[64 * 1024];
    lp_id *ids = NULL; uint64_t *runs = NULL; uint32_t *spare = NULL; size_t idc = 0;
    /* a stretch's entities and their paths are one transaction: a load cut off between the two leaves no entity
     * without a physicality (Physicality: the counts match, or the system is wrong). The file trunks are written
     * inside the transaction that holds what they attested, which is already open: that one is not begun or ended here. */
    if (own_txn) { PGresult *b = PQexec(pg, "BEGIN"); if (PQresultStatus(b) != PGRES_COMMAND_OK) { fprintf(stderr, "begin: %s", PQerrorMessage(pg)); exit(1); } PQclear(b); }
    part_name(p, "entity", tn, sizeof tn); snprintf(sql, sizeof sql, "COPY stage.%s (id, tier, coord, hilbert) FROM STDIN (FORMAT binary)", tn);
    copy_begin(&c, pg, sql);
    for (uint64_t b = lo; b < hi; b++) {
        Node *x = &NODE[bucket[p][b].idx];
        double xm[4]; for (int d = 0; d < 4; d++) xm[d] = (double)x->m[d] / LP_FIXED_ONE;
        size_t gl = lp_ewkb_point4(xm, geo, sizeof geo);
        c16(&c, 4); cfield(&c, x->id.b, 16); cf_i16(&c, x->tier); cfield(&c, geo, (uint32_t)gl); cf_i64(&c, hsigned(bucket[p][b].h)); c.rows++;
    }
    copy_end(&c); *rows_e = c.rows;
    part_name(p, "physicality", tn, sizeof tn); snprintf(sql, sizeof sql, "COPY stage.%s (entity, tier, hilbert, path, mask) FROM STDIN (FORMAT binary)", tn);
    uint8_t mask[4 + 32]; { uint32_t bl = htonl(256); memcpy(mask, &bl, 4); }         /* bit varying, binary: its length in bits, then its bytes, first bit first */
    copy_begin(&c, pg, sql);
    for (uint64_t b = lo; b < hi; b++) {
        Node *x = &NODE[bucket[p][b].idx];
        if (x->nv > idc) { idc = x->nv * 2; ids = xrealloc(ids, idc * sizeof(lp_id)); runs = xrealloc(runs, idc * 8); spare = xrealloc(spare, idc * 4); }
        for (uint32_t v = 0; v < x->nv; v++) { ids[v] = VTX[x->voff + v].id; runs[v] = VTX[x->voff + v].m; spare[v] = VTX[x->voff + v].spare; }
        size_t gl = lp_ewkb_runs_spare(ids, runs, spare, x->nv, NULL, 0); uint8_t *gp = gl > sizeof geo ? malloc(gl) : geo;
        lp_ewkb_runs_spare(ids, runs, spare, x->nv, gp, gl);
        /* the row's own bank, kind: what the row is. What its constituents are (a part of speech, a dependency relation)
         * is their banks', on the rows those banks describe (manifest/banks.tsv), never this row's */
        memset(mask + 4, 0, 32); for (int b = 0; b < 8; b++) if (x->kind & (1u << b)) mask[4 + (b >> 3)] |= (uint8_t)(0x80 >> (b & 7));
        c16(&c, 5); cfield(&c, x->id.b, 16); cf_i16(&c, x->tier); cf_i64(&c, hsigned(bucket[p][b].h)); cfield(&c, gp, (uint32_t)gl); cfield(&c, mask, 36); c.rows++;
        if (gp != geo) free(gp);
    }
    copy_end(&c); *rows_p = c.rows;
    if (own_txn) { PGresult *e = PQexec(pg, "COMMIT"); if (PQresultStatus(e) != PGRES_COMMAND_OK) { fprintf(stderr, "commit: %s", PQerrorMessage(pg)); exit(1); } PQclear(e); }
    free(c.b); free(ids); free(runs); free(spare);
}

/* ---- standings: a map from claim ID to its slot */
typedef struct { lp_id id; lp_rating r; uint32_t matches, m0; uint8_t had, entered; } Standing;     /* m0: the matches it was recorded with */
static Standing *stand; static lp_idmap *smap; static uint64_t sn;          /* the standings in play, in the order met; found by claim ID */
/* A claim enters at the stock default, Glicko-2's unrated (LP_GLICKO_RATING, LP_GLICKO_DEVIATION), whoever brings it:
 * what its witnesses say moves it from there (entering at the witness's deviation ranked a claim higher the lower its
 * witness's trust). matches: the games played into it. */
static Standing *stand_get(const lp_id *id, bool add){
    if (!add) { int64_t i = lp_idmap_find(smap, id); return i < 0 ? NULL : &stand[i]; }
    bool fresh; size_t i = lp_idmap_put(smap, id, &fresh); if (!fresh) return &stand[i];
    stand[i] = (Standing){ *id, lp_rating_stock(), 0, 0, 0, 0 };
    sn = lp_idmap_count(smap); return &stand[i];
}

/* One partition of the standings, on one connection: the new standings and the recorded or staged ones this source's
 * series moved, staged as they now stand. A standing staged before is taken out of the stage first; merge puts a staged
 * standing in place of the recorded one. Returns 0, or 1 when the database refused. */
static int standings_part(PGconn *pg, int h, uint64_t *nnew, uint64_t *nupd){
    char sql[256];
    uint64_t *idx = malloc(sizeof(uint64_t) * (sn + 1)), n = 0, nmoved = 0;
    for (uint64_t i = 0; i < sn; i++) { const Standing *s = &stand[i]; if ((s->id.b[0] >> 4) != h) continue;
        if (!s->had) idx[n++] = i; else if (s->matches != s->m0) { idx[n++] = i; nmoved++; } }
    for (uint64_t j0 = 0; j0 < n; j0 += 100000) {                             /* out of the stage: one statement a chunk */
        uint32_t k = (uint32_t)(n - j0 < 100000 ? n - j0 : 100000); lp_id *ids = malloc(sizeof(lp_id) * k);
        for (uint32_t j = 0; j < k; j++) ids[j] = stand[idx[j0 + j]].id;
        uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t len = ids_param(ab, ids, k); free(ids);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
        snprintf(sql, sizeof sql, "DELETE FROM stage.consensus_%x WHERE claim = ANY($1::blake3[])", h);
        PGresult *u = PQexecParams(pg, sql, 1, NULL, v, l, f, 0); int bad = PQresultStatus(u) != PGRES_COMMAND_OK;
        if (bad) fprintf(stderr, "standings staged: %s", PQerrorMessage(pg));
        PQclear(u); free(ab); if (bad) { free(idx); return 1; } }
    Copy c = { 0 }; snprintf(sql, sizeof sql, "COPY stage.consensus_%x (claim, rating, deviation, volatility, matches) FROM STDIN (FORMAT binary)", h);
    copy_begin(&c, pg, sql);
    for (uint64_t j = 0; j < n; j++) { const Standing *s = &stand[idx[j]];
        c16(&c, 5); cfield(&c, s->id.b, 16); cf_f64(&c, s->r.rating); cf_f64(&c, s->r.deviation); cf_f64(&c, s->r.volatility); cf_i32(&c, (int32_t)s->matches); c.rows++; }
    copy_end(&c); free(c.b); free(idx);
    *nnew += n - nmoved; *nupd += nmoved;
    return 0;
}

/* ---- what a source says, folded over all of it: each claim's games (the records that say it) and the sum of their
 * scores, as the records' vertices carry them (lp_score_carried). The client folds a source's repeats before the
 * database sees anything: one series a claim, played once, at the end of the source (standings_write). */
struct Fold { lp_idmap *m; };                                              /* claim -> Said */
typedef struct { uint32_t games; double sum; } Said;
Fold *fold_new(void){ Fold *f = calloc(1, sizeof *f); f->m = lp_idmap_sized(sizeof(Said)); return f; }
void fold_free(Fold *f){ if (!f) return; lp_idmap_free(f->m); free(f); }
uint64_t fold_count(const Fold *f){ return f ? lp_idmap_count(f->m) : 0; }
void fold_events(Fold *f, const File *files, int nfiles){
    for (int fi = 0; fi < nfiles; fi++) { if (files[fi].known || files[fi].skipped) continue;
        for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i]; if (e->kind != EV_CLAIM) continue;
            bool fresh; Said *s = lp_idmap_get(f->m, &e->claim, &fresh); s->games++; s->sum += lp_score_carried(e->score); } }
}
/* The series, played: each claim's standing, from what is recorded (or staged) of it, moved once by the source's series
 * at the witness's trust (lp_attest_series: each outcome pulled toward a draw by the trust, the deviation never below
 * what one witness of that trust can give), and staged for merge. The claims on every core: a standing is moved by its
 * own claim's series and by nothing else. Returns 0, or 1 when the database refused. */
int standings_write(const char *conninfo, int npg, Fold *fold, double trust, LoadStats *st){
    uint64_t nc = fold_count(fold); if (!nc) return 0;
    double t = now(), tp = t;
    PGconn **pg = malloc(sizeof(PGconn *) * npg); db_connect_many(conninfo, npg, pg);
    for (int i = 0; i < npg; i++) PQclear(PQexec(pg[i], "SET synchronous_commit = off"));
    if (!stage_open(pg[0])) return 1;
    const lp_id *ids = lp_idmap_keys(fold->m);
    smap = lp_idmap_new(); stand = malloc(sizeof(Standing) * (nc + 1)); sn = 0;
    for (uint64_t i = 0; i < nc; i++) stand_get(&ids[i], true);
    /* the recorded standings, then the staged ones: a staged standing is the later one; each read from its one partition */
    uint64_t ocnt[17] = { 0 }; lp_id *old = malloc(sizeof(lp_id) * (nc + 1));
    for (uint64_t i = 0; i < nc; i++) ocnt[(ids[i].b[0] >> 4) + 1]++;
    for (int h = 0; h < 16; h++) ocnt[h + 1] += ocnt[h];
    { uint64_t fill[16]; memcpy(fill, ocnt, sizeof fill); for (uint64_t i = 0; i < nc; i++) old[fill[ids[i].b[0] >> 4]++] = ids[i]; }
    const uint64_t CH = 100000; typedef struct { int h; uint64_t lo, n; } OJob; OJob *oj = malloc(sizeof(OJob) * (nc / CH + 17)); uint64_t noj = 0;
    for (int h = 0; h < 16; h++) for (uint64_t lo = ocnt[h]; lo < ocnt[h + 1]; lo += CH) oj[noj++] = (OJob){ h, lo, ocnt[h + 1] - lo < CH ? ocnt[h + 1] - lo : CH };
    int bad = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic) reduction(+:bad)
    for (uint64_t j = 0; j < noj; j++) {
        uint32_t k = (uint32_t)oj[j].n; uint8_t *ab = malloc(20 + 20 * (size_t)k);
        size_t len = ids_param(ab, old + oj[j].lo, k); PGconn *c = pg[omp_get_thread_num()];
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
        for (int from = 0; from < 2; from++) {
            char sql[160]; snprintf(sql, sizeof sql, "SELECT claim, rating, deviation, volatility, matches FROM %s.consensus_%x WHERE claim = ANY($1::blake3[])", from ? "stage" : "public", oj[j].h);
            PGresult *q = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "standing: %s", PQerrorMessage(c)); bad++; PQclear(q); break; }
            #pragma omp critical
            for (int r = 0; r < PQntuples(q); r++) {
                lp_id id; memcpy(id.b, PQgetvalue(q, r, 0), 16); Standing *s = stand_get(&id, false); if (!s) continue;
                s->r = (lp_rating){ lp_be_f64(PQgetvalue(q, r, 1)), lp_be_f64(PQgetvalue(q, r, 2)), lp_be_f64(PQgetvalue(q, r, 3)) };
                const uint8_t *mb = (const uint8_t *)PQgetvalue(q, r, 4); s->matches = s->m0 = (uint32_t)mb[0] << 24 | mb[1] << 16 | mb[2] << 8 | mb[3]; s->had = 1; }
            PQclear(q); }
        free(ab);
    }
    free(old); free(oj); if (bad) return 1;
    st->t_read += now() - tp; tp = now();
    #pragma omp parallel for schedule(dynamic, 4096)
    for (uint64_t i = 0; i < nc; i++) { const Said *q = lp_idmap_at(fold->m, i); Standing *s = &stand[i];      /* the map's places are the order of first meeting: stand's */
        lp_attest_series(&s->r, trust, q->games, q->sum / q->games, LP_ATTEST_FLOOR); s->matches += q->games; }
    st->t_play += now() - tp; tp = now();
    uint64_t nnew = 0, nupd = 0; int nparts = npg < 16 ? npg : 16;
    #pragma omp parallel for num_threads(nparts) schedule(static, 1) reduction(+:nnew, nupd, bad)
    for (int j = 0; j < nparts; j++) {
        PGconn *c = pg[j]; PQclear(PQexec(c, "BEGIN")); int mine = 0;
        for (int h = j; h < 16; h += nparts) { uint64_t a = 0, b = 0; mine += standings_part(c, h, &a, &b); nnew += a; nupd += b; }
        PGresult *e = PQexec(c, mine ? "ROLLBACK" : "COMMIT"); if (PQresultStatus(e) != PGRES_COMMAND_OK) { fprintf(stderr, "standings: %s", PQerrorMessage(c)); mine++; } PQclear(e); bad += mine; }
    st->std_new += nnew; st->std_upd += nupd; st->t_led += now() - tp; st->t_sem += now() - t;
    free(stand); lp_idmap_free(smap); stand = NULL; smap = NULL;
    for (int i = 0; i < npg; i++) PQfinish(pg[i]); free(pg);
    return bad ? 1 : 0;
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
/* The files of a batch whose trunks are recorded: each is recorded, with everything under it and everything it says,
 * and nothing of it is looked for, played or written again. Asked by ingest before it folds what the batch says (the
 * batch may then be written by a child of its own), and by load again, which finds the same. A file whose trunk is new
 * holds what it says under that trunk even where its content tree is recorded under another: its source's trunk is
 * another witness, and the walk up from what it says finds both (provenance by containment). */
int files_known(PGconn **pg, int npg, File *files, int nfiles, LoadStats *st){
    lp_id *trunk = malloc(sizeof(lp_id) * (size_t)(nfiles + 1)); int *of = malloc(sizeof(int) * (size_t)(nfiles + 1)); uint64_t nt = 0; int n = 0;
    for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { of[nt] = fi; trunk[nt++] = files[fi].file.id; }
    if (nt) { uint8_t *tt = malloc(nt); for (uint64_t i = 0; i < nt; i++) { Node *x = table_find(&trunk[i]); tt[i] = x ? x->tier : files[of[i]].file.tier; }
              uint8_t *hit = recorded(pg, npg, trunk, tt, nt); free(tt); if (st) st->checked += nt;
              for (uint64_t i = 0; i < nt; i++) if (hit[i]) { files[of[i]].known = 1; n++; if (st) { st->known++; st->found++; } Node *x = table_find(&trunk[i]); if (x) x->keep = 2; }
              free(hit); }
    free(trunk); free(of); return n;
}
int ingest_known(const char *conninfo, File *files, int nfiles){
    PGconn *pg = db_connect(conninfo); int n = files_known(&pg, 1, files, nfiles, NULL); PQfinish(pg); return n;
}
int load_whole;
int load(const char *conninfo, int npg, File *files, int nfiles, LoadStats *st){
    table_kinds();                                                           /* what each child is said to be: on the child, for its mask */
    /* what a witness attested is a claim, and what it attested together is a record, whatever holds it or nothing does:
     * its row says so, so every read finds it (a record is held by nothing, so no holder ever says it) */
    for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) {
        Node *c = table_find(&files[fi].ev.e[i].claim); if (c) c->kind |= (uint8_t)(1u << (files[fi].ev.e[i].kind == EV_RECORD ? LP_KIND_RECORD : LP_KIND_CLAIM)); }
    PGconn **pg = malloc(sizeof(PGconn *) * npg); db_connect_many(conninfo, npg, pg);
    for (int i = 0; i < npg; i++) PQclear(PQexec(pg[i], "SET synchronous_commit = off"));
    parts_plan(pg[0]);
    if (!stage_open(pg[0])) return 1;
    if (!resolve_parts(pg[0])) return 1;                                       /* a batch that stopped between its parts' commits, finished first */
    /* Tier 0 is the perf-cache's: a load never asks for, counts or writes a codepoint. Its rows are
     * written once by the source that records them (tier0_write); a codepoint is no node of the table, so the descent below
     * never reaches one. */

    /* ---- trunk to leaf: a recorded node means its whole subtree is recorded, so nothing below it is checked */
    double t = now();
    uint64_t cap = 1 << 20, nf = 0; lp_id *front = malloc(cap * sizeof(lp_id));
    #define FPUSH(x) do { if (nf == cap) { cap *= 2; front = xrealloc(front, cap * sizeof(lp_id)); } front[nf++] = (x); } while (0)
    if (load_whole)                                                          /* every node is looked for: nothing is taken to be recorded because what holds it is */
        TABLE_EACH(x) if (!x->keep) { x->keep = 3; FPUSH(x->id); }
    files_known(pg, npg, files, nfiles, st);
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
    /* Trunk to leaf, one set a partition a round: the nodes of a round are asked for together ("I have these IDs: which
     * do you already have?"), and a node the real tables hold is left out with everything under it. What this source
     * staged in an earlier batch the client knows (keep 2, before the batch: ingest.c), so the stage is never asked;
     * the real tables answer for every source recorded before this one. */
    while (nf) {
        st->rounds++; st->checked += nf;
        uint8_t *ft = malloc(nf); for (uint64_t i = 0; i < nf; i++) ft[i] = table_find(&front[i])->tier;
        uint8_t *hit = recorded(pg, npg, front, ft, nf); free(ft);
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
    }
    free(front); fprintf(stderr, "  staged: %'llu nodes, %llu tiers deep, %'llu subtrees recorded already\n", (unsigned long long)st->new_nodes,
                         (unsigned long long)st->rounds, (unsigned long long)st->found);
    st->t_dedup += now() - t;

    /* ---- the batch is one transaction, in parts, one a connection, prepared and committed together below: what is
     * recorded of it is all of it, so its nodes need no order among themselves (leaf to trunk holds by the commit).
     * Each part copies its share of the new nodes into entity and physicality: two statements a connection. */
    t = now();
    int nparts = npg < 16 ? npg : 16, begun = nparts; char xid0[32] = "";      /* part 0's transaction ID names the parts */
    for (int j = 0; j < nparts; j++) if (!must(pg[j], "BEGIN")) return 1;
    { PGresult *x = PQexec(pg[0], "SELECT pg_current_xact_id()"); if (PQresultStatus(x) != PGRES_TUPLES_OK) { fprintf(stderr, "transaction: %s", PQerrorMessage(pg[0])); return 1; }
      snprintf(xid0, sizeof xid0, "%s", PQgetvalue(x, 0, 0)); PQclear(x); }
    /* Each leaf partition (its tier, and the range of IDs the leaf holds) is one connection's alone, its new rows
     * copied in Hilbert order: no two connections write one leaf's pages, and a leaf's heap fills in order. */
    { uint64_t cnt[NPART] = { 0 };
      TABLE_EACH(x) if (x->keep == 1) cnt[part_of(&x->id, x->tier)]++;
      for (int p = 0; p < NPART; p++) { bucket[p] = malloc(sizeof(NRef) * (cnt[p] + 1)); nbucket[p] = 0; }
      TABLE_EACH(x) if (x->keep == 1) { int p = part_of(&x->id, x->tier); bucket[p][nbucket[p]++] = nref(x); } }
    { uint64_t te = 0, tp = 0;
      #pragma omp parallel for num_threads(nparts) schedule(static, 1) reduction(+:te, tp)
      for (int j = 0; j < nparts; j++) for (int p = j; p < NPART; p += nparts) {
          if (!nbucket[p]) continue;
          qsort(bucket[p], nbucket[p], sizeof(NRef), by_hilbert);
          uint64_t e = 0, ph = 0; write_node_rows(pg[j], p, 0, nbucket[p], &e, &ph, 0); te += e; tp += ph; }
      st->ent_rows += te; st->phy_rows += tp; }
    for (int p = 0; p < NPART; p++) { free(bucket[p]); bucket[p] = NULL; }
    st->t_copy += now() - t;

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
    PGconn *pg = db_connect(conninfo); parts_plan(pg); if (!stage_open(pg)) exit(1);
    uint8_t *hit = recorded(&pg, 1, ids, tiers, n); uint64_t got = 0; for (uint64_t i = 0; i < n; i++) got += hit[i];
    free(hit); PQfinish(pg); return got == n;
}
/* ---- merge: what a source staged goes into the real tables, in one order, as one transaction in parts, one a connection
 * (prepared, then committed). The stage holds each node of the source once, and only nodes the real tables did not
 * hold when it was staged (the descent in load asked them, trunk to leaf; the client staged nothing twice). A leaf:
 *   a leaf that holds nothing yet is loaded: truncated and copied FREEZE, in its order (Hilbert for entity and
 *     physicality, the claim for consensus). A truncated table's files are new, written without WAL at
 *     wal_level minimal and synced when the transaction is prepared, its indexes with it; frozen rows leave no hint
 *     bits for a later read to log (data_checksums is on). TRUNCATE locks that leaf alone.
 *   a leaf that holds rows takes the staged rows, one INSERT ... SELECT (its primary key refuses a row it holds);
 *     consensus puts a staged standing in place of the recorded one (as the source left it).
 * An entity leaf is always appended: a physicality's entity is a foreign key to it, and a table a foreign key points
 * at cannot be truncated alone. An entity leaf and the physicality leaf of the same range go to one connection, the
 * entities first, so the paths it copies find their entities in its own part of the transaction. */
typedef struct { char name[64]; int table; double live, staged_rows, staged_bytes; int rewrite, worker; } MLeaf;
enum { M_ENTITY, M_PHYS, M_CONS };
static const char *M_ORDER[3] = { "hilbert", "hilbert", "claim" };
static int mleaf_by_table(const void *a, const void *b){ return ((const MLeaf *)a)->table - ((const MLeaf *)b)->table; }   /* entities before their paths */
static int leaf_range(const char *name){ const char *u = strrchr(name, '_'); return u ? (int)strtol(u + 1, NULL, 16) : 0; }    /* entity_ab, physicality_ab: 0xab */
static char *cols_of(PGconn *pg, const char *leaf){                          /* the leaf's columns, in order */
    const char *v[1] = { leaf }; PGresult *r = PQexecParams(pg, "SELECT string_agg(quote_ident(attname), ', ' ORDER BY attnum) FROM pg_attribute "
        "WHERE attrelid = ('public.' || quote_ident($1))::regclass AND attnum > 0 AND NOT attisdropped", 1, NULL, v, NULL, NULL, 0);
    char *c = PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) ? strdup(PQgetvalue(r, 0, 0)) : NULL; PQclear(r); return c;
}
/* One leaf, on its writer's transaction w; r reads, outside any transaction. Returns 0, or 1 when the database refused. */
static int merge_leaf(PGconn *w, PGconn *rd, const MLeaf *L, uint64_t *rows){
    char *cols = cols_of(w, L->name); if (!cols) { fprintf(stderr, "merge %s: no columns\n", L->name); return 1; }
    size_t sl = strlen(cols) * 3 + 1024; char *sql = malloc(sl); int bad = 0;
    const char *same = "s.claim = l.claim";                                  /* a staged standing in place of the recorded one */
    if (L->rewrite) {
        /* read whole, before the leaf is truncated: what it held (less what the stage holds in its place) and what was staged */
        if (L->table == M_CONS) snprintf(sql, sl, "COPY (SELECT %s FROM public.%s l WHERE NOT EXISTS (SELECT 1 FROM stage.%s s WHERE %s) UNION ALL SELECT %s FROM stage.%s ORDER BY %s) TO STDOUT (FORMAT binary)",
                                        cols, L->name, L->name, same, cols, L->name, M_ORDER[L->table]);
        else snprintf(sql, sl, "COPY (SELECT %s FROM public.%s UNION ALL SELECT %s FROM stage.%s ORDER BY %s) TO STDOUT (FORMAT binary)", cols, L->name, cols, L->name, M_ORDER[L->table]);
        PGresult *r = PQexec(rd, sql);
        if (PQresultStatus(r) != PGRES_COPY_OUT) { fprintf(stderr, "merge %s, read: %s", L->name, PQerrorMessage(rd)); PQclear(r); free(sql); free(cols); return 1; }
        PQclear(r);
        size_t cap = 1 << 24, n = 0; uint8_t *buf = malloc(cap); char *chunk; int got;
        while ((got = PQgetCopyData(rd, &chunk, 0)) > 0) { if (n + (size_t)got > cap) { while (n + (size_t)got > cap) cap *= 2; buf = xrealloc(buf, cap); } memcpy(buf + n, chunk, (size_t)got); n += (size_t)got; PQfreemem(chunk); }
        while ((r = PQgetResult(rd))) { if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "merge %s, read: %s", L->name, PQerrorMessage(rd)); bad = 1; }
                                        else *rows += (uint64_t)atoll(PQcmdTuples(r)); PQclear(r); }
        if (!bad) { snprintf(sql, sl, "TRUNCATE public.%s", L->name); bad = !must(w, sql); }
        if (!bad) { snprintf(sql, sl, "COPY public.%s (%s) FROM STDIN (FORMAT binary, FREEZE)", L->name, cols);
            r = PQexec(w, sql);
            if (PQresultStatus(r) != PGRES_COPY_IN) { fprintf(stderr, "merge %s, write: %s", L->name, PQerrorMessage(w)); bad = 1; }
            PQclear(r);
            for (size_t o = 0; !bad && o < n; o += 1 << 22) { size_t k = n - o < (1u << 22) ? n - o : 1u << 22; if (PQputCopyData(w, (const char *)buf + o, (int)k) != 1) { fprintf(stderr, "merge %s: %s", L->name, PQerrorMessage(w)); bad = 1; } }
            if (!bad && PQputCopyEnd(w, NULL) != 1) { fprintf(stderr, "merge %s: %s", L->name, PQerrorMessage(w)); bad = 1; }
            while ((r = PQgetResult(w))) { if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "merge %s, write: %s", L->name, PQerrorMessage(w)); bad = 1; } PQclear(r); } }
        free(buf);
    } else {
        if (L->table == M_CONS) {
            /* A staged row is new, or takes the recorded one's place: two set operations along the primary key, the
             * recorded rows updated by a join and the new ones inserted by an anti-join, each one statement over the
             * leaf (Ingestion: Deduplication: set-based, never per-row conflict handling). ON CONFLICT arbitrated every
             * row through the index: 2.3 s a leaf-call on 13 sources (pg_stat_statements, 2026-10-05). */
            const char *key = "p.claim = s.claim";
            const char *set = "rating = s.rating, deviation = s.deviation, volatility = s.volatility, matches = s.matches";
            snprintf(sql, sl, "UPDATE public.%s p SET %s FROM stage.%s s WHERE %s", L->name, set, L->name, key);
            bad = !must(w, sql);
            if (!bad) { snprintf(sql, sl, "INSERT INTO public.%s (%s) SELECT %s FROM stage.%s s WHERE NOT EXISTS (SELECT 1 FROM public.%s p WHERE %s) ORDER BY %s",
                                 L->name, cols, cols, L->name, L->name, key, M_ORDER[L->table]); bad = !must(w, sql); } }
        else { snprintf(sql, sl, "INSERT INTO public.%s (%s) SELECT %s FROM stage.%s ORDER BY %s", L->name, cols, cols, L->name, M_ORDER[L->table]); bad = !must(w, sql); }   /* new by the descent: the primary key refuses anything that is not */
        *rows += (uint64_t)L->staged_rows;
    }
    /* the container index takes what it was handed into its own list: merged here, inside the transaction, so no
     * lookup reads a pending list through (in a rewritten leaf this too is written without WAL) */
    if (!bad && L->table == M_PHYS) {
        const char *v[1] = { L->name };
        PGresult *q = PQexecParams(w, "SELECT gin_clean_pending_list(i.indexrelid::regclass) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam "
                                      "WHERE i.indrelid = ('public.' || quote_ident($1))::regclass AND a.amname = 'gin'", 1, NULL, v, NULL, NULL, 0);
        if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "merge %s, container index: %s", L->name, PQerrorMessage(w)); bad = 1; } PQclear(q); }
    /* The reference a path makes to its entity, checked as one set over what this leaf took (the stage still holds it),
     * not row by row: with the referential triggers out of the writers' transaction, this is what keeps the ordering
     * honest. A path is partitioned by its entity, so the entity is in the entity leaf of the same range, written by
     * this writer before any of its paths (entities first) and visible to it. A path without an entity fails the
     * merge, which rolls back whole. The trigger also held each entity against a delete until the commit; the set
     * check reads a snapshot, so the entity leaf is held as a whole instead (forget deletes entities by leaf): the
     * writer that holds the range takes nothing from itself, and a delete waits for the merge. */
    if (!bad && L->table == M_PHYS) {
        const char *range = strrchr(L->name, '_') + 1;
        snprintf(sql, sl, "LOCK TABLE public.entity_%s IN SHARE ROW EXCLUSIVE MODE", range); bad = !must(w, sql);
        if (!bad) {
            snprintf(sql, sl, "SELECT count(*) FROM stage.%s s WHERE NOT EXISTS (SELECT 1 FROM public.entity_%s e WHERE e.id = s.entity)", L->name, range);
            PGresult *q = PQexec(w, sql);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "merge %s, paths and their entities: %s", L->name, PQerrorMessage(w)); bad = 1; }
            else if (strcmp(PQgetvalue(q, 0, 0), "0")) { fprintf(stderr, "merge %s: %s paths name an entity that is not recorded: the system is wrong\n", L->name, PQgetvalue(q, 0, 0)); bad = 1; }
            PQclear(q); } }
    if (!bad) { snprintf(sql, sl, "TRUNCATE stage.%s", L->name); bad = !must(w, sql); }
    free(sql); free(cols); return bad;
}
int merge(const char *conninfo, int npg){
    double T = now(); setlocale(LC_NUMERIC, "en_US.UTF-8");
    PGconn *pg0 = db_connect(conninfo); parts_plan(pg0); if (!stage_open(pg0)) return 1;
    if (!resolve_parts(pg0)) return 1;
    PGresult *r = PQexec(pg0, "SELECT current_setting('wal_level'), wal_bytes, wal_fpi FROM pg_stat_wal");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "merge: %s", PQerrorMessage(pg0)); return 1; }
    int minimal = !strcmp(PQgetvalue(r, 0, 0), "minimal"); double wal0 = atof(PQgetvalue(r, 0, 1)), fpi0 = atof(PQgetvalue(r, 0, 2)); PQclear(r);
    /* what is staged, leaf by leaf, and what each way would write */
    r = PQexec(pg0, "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'stage' AND c.relkind = 'r' AND c.relname ~ '^(entity|physicality|consensus)_' ORDER BY 1");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "merge: %s", PQerrorMessage(pg0)); return 1; }
    int nl = PQntuples(r); MLeaf *L = calloc((size_t)nl + 1, sizeof(MLeaf));
    for (int i = 0; i < nl; i++) { snprintf(L[i].name, sizeof L[i].name, "%s", PQgetvalue(r, i, 0));
        L[i].table = !strncmp(L[i].name, "entity", 6) ? M_ENTITY : !strncmp(L[i].name, "physicality", 11) ? M_PHYS : M_CONS; }
    PQclear(r);
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg), **rd = malloc(sizeof(PGconn *) * (size_t)npg);
    db_connect_many(conninfo, npg, pg); db_connect_many(conninfo, npg, rd);
    for (int i = 0; i < npg; i++) PQclear(PQexec(pg[i], "SET synchronous_commit = off"));
    int bad = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic, 1) reduction(+:bad)
    for (int i = 0; i < nl; i++) {
        PGconn *c = rd[omp_get_thread_num()]; char sql[512];
        snprintf(sql, sizeof sql, "SELECT (SELECT count(*) FROM stage.%s), pg_relation_size('stage.%s'), pg_relation_size('public.%s'), pg_total_relation_size('public.%s')",
                 L[i].name, L[i].name, L[i].name, L[i].name);
        PGresult *q = PQexec(c, sql);
        if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "merge %s: %s", L[i].name, PQerrorMessage(c)); bad++; PQclear(q); continue; }
        L[i].staged_rows = atof(PQgetvalue(q, 0, 0)); L[i].staged_bytes = atof(PQgetvalue(q, 0, 1)); double total = atof(PQgetvalue(q, 0, 3));
        L[i].live = total; PQclear(q);
        if (!L[i].staged_rows) continue;
        /* A leaf that holds nothing yet is loaded, unlogged: truncated, so its files are new and written without WAL at
         * wal_level minimal, and copied FREEZE. A leaf that holds rows takes what it does not hold, as one set. No
         * estimate decides: what the leaf is does. An entity leaf takes the set always (a path's entity is a foreign key
         * to it, and a table a foreign key points at cannot be truncated alone). */
        snprintf(sql, sizeof sql, "SELECT EXISTS (SELECT 1 FROM public.%s)", L[i].name); q = PQexec(c, sql);
        int holds = PQresultStatus(q) != PGRES_TUPLES_OK || PQgetvalue(q, 0, 0)[0] == 't'; PQclear(q);
        L[i].rewrite = minimal && L[i].table != M_ENTITY && !holds;
    }
    if (bad) return 1;
    int nm = 0; for (int i = 0; i < nl; i++) if (L[i].staged_rows) L[nm++] = L[i];
    if (!nm) { printf("  nothing staged\n"); for (int i = 0; i < npg; i++) { PQfinish(pg[i]); PQfinish(rd[i]); } PQfinish(pg0); free(L); return 0; }
    /* a range's leaves to one writer (an ID is a hash: the ranges are alike in size), entities first */
    qsort(L, (size_t)nm, sizeof(MLeaf), mleaf_by_table);
    for (int i = 0; i < nm; i++) L[i].worker = leaf_range(L[i].name) % npg;
    int nrw = 0; double rw_bytes = 0, ap_rows = 0; for (int i = 0; i < nm; i++) { if (L[i].rewrite) { nrw++; rw_bytes += L[i].live; } else ap_rows += L[i].staged_rows; }
    printf("  %d leaves have rows staged: %d rewritten (%.1f GB held, written without WAL), %d appended (%'.0f rows, logged)%s\n",
           nm, nrw, rw_bytes / 1e9, nm - nrw, ap_rows, minimal ? "" : "; wal_level is not minimal, so none is rewritten");
    /* one transaction in parts: each writer's part prepared, part 0 last, then part 0 committed, which decides */
    /* The writers' transactions. The foreign key from a path to its entity holds by construction: entities go in
     * first, and the descent stages no path whose entity is not staged or recorded (Ingestion: Deduplication). Checked
     * row by row it cost 3,829,527 lookups and 261 s of backend time in a 333 s run of unicode and iso-639 (SELECT 1
     * FROM entity WHERE id = $1 FOR KEY SHARE, pg_stat_statements, 2026-10-05), a 35 s floor under the smallest source;
     * replica mode leaves the referential triggers out for this transaction alone. */
    char xid0[32] = ""; for (int j = 0; j < npg; j++) if (!must(pg[j], "BEGIN") || !must(pg[j], "SET LOCAL session_replication_role = replica")) return 1;
    r = PQexec(pg[0], "SELECT pg_current_xact_id()"); if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "merge: %s", PQerrorMessage(pg[0])); return 1; }
    snprintf(xid0, sizeof xid0, "%s", PQgetvalue(r, 0, 0)); PQclear(r);
    uint64_t rows = 0;
    #pragma omp parallel for num_threads(npg) schedule(static, 1) reduction(+:bad, rows)
    for (int j = 0; j < npg; j++) for (int i = 0; i < nm && !bad; i++) if (L[i].worker == j) { uint64_t k = 0; bad += merge_leaf(pg[j], rd[j], &L[i], &k); rows += k; }
    if (bad) { for (int j = 0; j < npg; j++) PQclear(PQexec(pg[j], "ROLLBACK")); fprintf(stderr, "merge: rolled back, the stage is as it was\n"); return 1; }
    { char sql[96];
      for (int j = 1; j < npg; j++) { snprintf(sql, sizeof sql, "PREPARE TRANSACTION 'laplace %s %d'", xid0, j); if (!must(pg[j], sql)) return 1; }
      snprintf(sql, sizeof sql, "PREPARE TRANSACTION 'laplace %s 0'", xid0); if (!must(pg[0], sql)) return 1;
      for (int j = 0; j < npg; j++) { snprintf(sql, sizeof sql, "COMMIT PREPARED 'laplace %s %d'", xid0, j); if (!must(pg[0], sql)) return 1; } }
    double tc = now() - T;
    /* the planner's statistics for what was rewritten */
    #pragma omp parallel for num_threads(npg) schedule(dynamic, 1)
    for (int i = 0; i < nm; i++) if (L[i].rewrite) { char sql[96]; snprintf(sql, sizeof sql, "ANALYZE public.%s", L[i].name); PQclear(PQexec(pg[omp_get_thread_num()], sql)); }
    r = PQexec(pg0, "SELECT wal_bytes, wal_fpi FROM pg_stat_wal");
    double wal1 = PQresultStatus(r) == PGRES_TUPLES_OK ? atof(PQgetvalue(r, 0, 0)) : wal0, fpi1 = PQresultStatus(r) == PGRES_TUPLES_OK ? atof(PQgetvalue(r, 0, 1)) : fpi0; PQclear(r);
    printf("  merged %'llu rows in %.1f s (committed at %.1f s); WAL written by the merge: %.2f GB, %'.0f page images\n",
           (unsigned long long)rows, now() - T, tc, (wal1 - wal0) / 1e9, fpi1 - fpi0);
    for (int i = 0; i < npg; i++) { PQfinish(pg[i]); PQfinish(rd[i]); } PQfinish(pg0); free(L);
    return 0;
}
int cmd_merge(int argc, char **argv){
    const char *conninfo = laplace_db(); int threads = 0;
    for (int a = 1; a < argc; a++) { if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a]; else if (!strcmp(argv[a], "-j") && a + 1 < argc) threads = atoi(argv[++a]);
                                     else { fprintf(stderr, "usage: laplace merge [-d conninfo] [-j connections]\n"); return 2; } }
    if (threads <= 0) threads = omp_get_num_procs();
    printf("laplace merge   %d connections\n", threads);
    return merge(conninfo, threads);
}

/* ---- tier 0: every codepoint, recorded once, from the perf-cache (Atoms: "Tier 0 is still recorded to the database, but
 * function calls never need to read it from there"). The source that records it (tier0 in its source file: the Unicode source,
 * first in recipes/order) writes it as its ingest begins; no load asks for, counts or writes
 * a codepoint. Each range's codepoints go into the real tables on one connection, entities then their paths (a path's
 * entity is a foreign key), one transaction a range. A range that holds all of its codepoints is left as it is; a range
 * that holds some is told which it holds. Returns 0, or 1 when the database refused. */
int tier0_write(const char *conninfo, int npg){
    double T = now(); uint32_t want[NPART] = { 0 }; for (uint32_t cp = 0; cp < LP_NCP; cp++) want[part_of(&T0[cp].id, 0)]++;
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg); db_connect_many(conninfo, npg, pg); for (int i = 0; i < npg; i++) PQclear(PQexec(pg[i], "SET synchronous_commit = off"));
    uint64_t wrote = 0; int bad = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic, 1) reduction(+:wrote, bad)
    for (int p = 0; p < NPART; p++) {
        PGconn *c = pg[omp_get_thread_num()]; char tn[64], sql[192]; part_name(p, "entity", tn, sizeof tn);
        snprintf(sql, sizeof sql, "SELECT id FROM public.%s WHERE tier = 0", tn);
        PGresult *r = PQexecParams(c, sql, 0, NULL, NULL, NULL, NULL, 1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "tier 0, %s: %s", tn, PQerrorMessage(c)); PQclear(r); bad++; continue; }
        int have = PQntuples(r); if ((uint32_t)have >= want[p]) { PQclear(r); continue; }
        lp_idmap *held = lp_idmap_new(); for (int i = 0; i < have; i++) { lp_id id; memcpy(id.b, PQgetvalue(r, i, 0), 16); bool f; lp_idmap_put(held, &id, &f); }
        PQclear(r);
        if (!must(c, "BEGIN")) { bad++; lp_idmap_free(held); continue; }
        Copy e = { 0 }; uint8_t geo[256]; uint64_t n = 0;
        snprintf(sql, sizeof sql, "COPY public.%s (id, tier, coord, hilbert) FROM STDIN (FORMAT binary)", tn); copy_begin(&e, c, sql);
        for (uint32_t cp = 0; cp < LP_NCP; cp++) { if (part_of(&T0[cp].id, 0) != p || lp_idmap_find(held, &T0[cp].id) >= 0) continue;
            double x[4]; for (int d = 0; d < 4; d++) x[d] = (double)T0[cp].m[d] / LP_FIXED_ONE; size_t gl = lp_ewkb_point4(x, geo, sizeof geo);
            c16(&e, 4); cfield(&e, T0[cp].id.b, 16); cf_i16(&e, 0); cfield(&e, geo, (uint32_t)gl); cf_i64(&e, hsigned(T0[cp].hilbert)); n++; }
        copy_end(&e);
        part_name(p, "physicality", tn, sizeof tn); uint8_t mask[4 + 32]; { uint32_t bl = htonl(256); memcpy(mask, &bl, 4); memset(mask + 4, 0, 32); }
        snprintf(sql, sizeof sql, "COPY public.%s (entity, tier, hilbert, path, mask) FROM STDIN (FORMAT binary)", tn); copy_begin(&e, c, sql);
        for (uint32_t cp = 0; cp < LP_NCP; cp++) { if (part_of(&T0[cp].id, 0) != p || lp_idmap_find(held, &T0[cp].id) >= 0) continue;
            uint64_t one = 1; size_t gl = lp_ewkb_runs(&T0[cp].id, &one, 1, geo, sizeof geo);
            c16(&e, 5); cfield(&e, T0[cp].id.b, 16); cf_i16(&e, 0); cf_i64(&e, hsigned(T0[cp].hilbert)); cfield(&e, geo, (uint32_t)gl); cfield(&e, mask, 36); }
        copy_end(&e); free(e.b); lp_idmap_free(held);
        if (!must(c, "COMMIT")) { bad++; continue; }
        wrote += n;
    }
    for (int i = 0; i < npg; i++) PQfinish(pg[i]); free(pg);
    if (!bad) printf("  %-52s %'9llu written, from the perf-cache   %.1f s\n", "tier 0: every codepoint, once", (unsigned long long)wrote, now() - T);
    return bad ? 1 : 0;
}

/* Which of these compositions are recorded, in the real tables or the stage: one set question per partition, nothing
 * written (laplace ingest --entities asks it of a sample). The caller frees what it returns. */
uint8_t *db_recorded(const char *conninfo, const lp_id *ids, uint64_t n){
    PGconn *pg = db_connect(conninfo); parts_plan(pg); if (!stage_open(pg)) exit(1);
    uint8_t *t = calloc(n ? n : 1, 1), *hit = recorded(&pg, 1, ids, t, n); free(t); PQfinish(pg); return hit;
}
