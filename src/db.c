/* Writing to PostgreSQL: trunk-to-leaf deduplication, binary COPY straight into each leaf partition on its own
 * connection, and the semantics (witnesses, attestation, consensus). SQL only fetches and writes. */
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
static void cf_f32(Copy *c, float v){ uint32_t u; memcpy(&u, &v, 4); c32(c, 4); c32(c, u); }
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
 * physicality, attestation and consensus, in the schema stage under the leaf's own name: what is staged writes no WAL.
 * Lookups during an ingest read the leaf and its stage table both. A crash empties every stage table (unlogged) and
 * leaves the real tables as they were before the run: the run is begun again. */
static int stage_ready;
static int stage_open(PGconn *pg){
    if (stage_ready) return 1;
    /* made once: every leaf has its stage table already when the stage holds as many tables as the real tables have
     * leaves (measured: the CREATE ... IF NOT EXISTS of all of them took 7.3 s each time a process asked) */
    { PGresult *q = PQexec(pg, "SELECT (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'stage' AND c.relkind = 'r') "
                               "= (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'public' AND c.relkind = 'r' "
                               "AND c.relispartition AND c.relname ~ '^(entity|physicality|attestation|consensus)_')");
      int made = PQresultStatus(q) == PGRES_TUPLES_OK && PQntuples(q) && PQgetvalue(q, 0, 0)[0] == 't'; PQclear(q);
      if (made) return stage_ready = 1; }
    const char *sql =
        "CREATE SCHEMA IF NOT EXISTS stage;"
        "DO $$ DECLARE r record; BEGIN"
        "  FOR r IN SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace"
        "           WHERE n.nspname = 'public' AND c.relkind = 'r' AND c.relispartition"
        "             AND c.relname ~ '^(entity|physicality|attestation|consensus)_' LOOP"
        "    EXECUTE format('CREATE UNLOGGED TABLE IF NOT EXISTS stage.%I (LIKE public.%I INCLUDING DEFAULTS)', r.relname, r.relname);"
        "    IF r.relname LIKE 'entity%' THEN EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON stage.%I (id)', r.relname || '_id', r.relname);"
        "    ELSIF r.relname LIKE 'attestation%' THEN EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON stage.%I (claim, witness)', r.relname || '_cw', r.relname);"
        "    ELSIF r.relname LIKE 'consensus%' THEN EXECUTE format('CREATE UNIQUE INDEX IF NOT EXISTS %I ON stage.%I (claim)', r.relname || '_claim', r.relname);"
        "    END IF;"
        "  END LOOP; END $$";
    PGresult *r = PQexec(pg, sql); int ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    if (!ok) fprintf(stderr, "the stage: %s", PQerrorMessage(pg)); PQclear(r);
    return stage_ready = ok;
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
        snprintf(sql, sizeof sql, "SELECT u.i FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i) WHERE EXISTS (SELECT 1 FROM public.%s e WHERE e.id = u.id)"
                                  " OR EXISTS (SELECT 1 FROM stage.%s e WHERE e.id = u.id)", tn, tn);     /* recorded, or staged by this run */
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
        PGresult *r = PQexecParams(c, sql, 1, NULL, v, l, f, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "dedup: %s", PQerrorMessage(c)); exit(1); }
        for (int x = 0; x < PQntuples(r); x++) hit[at[job[j].lo + (uint64_t)atoll(PQgetvalue(r, x, 0)) - 1]] = 1;
        PQclear(r); free(ab); free(part);
    }
    free(at); free(job); free(cnt); free(fill);
    return hit;
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
/* Rows lo..hi of a partition's bucket, which is in Hilbert order already: a stretch of the partition, so that a
 * partition is written by as many connections as it has stretches. A codepoint is never written here: tier 0 is the
 * recorded once by the source that records it (tier0_write). */
static void write_node_rows(PGconn *pg, int p, uint64_t lo, uint64_t hi, uint64_t *rows_e, uint64_t *rows_p, int own_txn){
    char tn[64]; Copy c = { 0 }; char sql[160]; uint8_t geo[64 * 1024];
    lp_id *ids = NULL; uint64_t *runs = NULL; size_t idc = 0;
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
        if (x->nv > idc) { idc = x->nv * 2; ids = xrealloc(ids, idc * sizeof(lp_id)); runs = xrealloc(runs, idc * 8); }
        for (uint32_t v = 0; v < x->nv; v++) { ids[v] = VTX[x->voff + v].id; runs[v] = VTX[x->voff + v].m; }
        size_t gl = lp_ewkb_runs(ids, runs, x->nv, NULL, 0); uint8_t *gp = gl > sizeof geo ? malloc(gl) : geo;
        lp_ewkb_runs(ids, runs, x->nv, gp, gl);
        /* the row's own bank, kind: what the row is. What its constituents are (a part of speech, a dependency relation)
         * is their banks', on the rows those banks describe (manifest/banks.tsv), never this row's */
        memset(mask + 4, 0, 32); for (int b = 0; b < 8; b++) if (x->kind & (1u << b)) mask[4 + (b >> 3)] |= (uint8_t)(0x80 >> (b & 7));
        c16(&c, 5); cfield(&c, x->id.b, 16); cf_i16(&c, x->tier); cf_i64(&c, hsigned(bucket[p][b].h)); cfield(&c, gp, (uint32_t)gl); cfield(&c, mask, 36); c.rows++;
        if (gp != geo) free(gp);
    }
    copy_end(&c); *rows_p = c.rows;
    if (own_txn) { PGresult *e = PQexec(pg, "COMMIT"); if (PQresultStatus(e) != PGRES_COMMAND_OK) { fprintf(stderr, "commit: %s", PQerrorMessage(pg)); exit(1); } PQclear(e); }
    free(c.b); free(ids); free(runs);
}

/* ---- standings: a map from claim ID to its slot */
typedef struct { lp_id id; lp_rating r; uint32_t matches, m0; uint8_t had, entered; } Standing;     /* m0: the matches it was recorded with */
static Standing *stand; static lp_idmap *smap; static uint64_t sn;          /* the standings in play, in the order met; found by claim ID */
/* The stock default a claim enters at: Glicko-2's rating for the unrated, and the uncertainty of the witness that brings
 * it (the deviation its trust plays with), unless the recipe gives this kind of statement its own. */
static double entry_deviation(const Event *e, double trust){
    if (e->enter_deviation > 0) return e->enter_deviation;
    double t = trust < 0 ? -trust : trust; if (t == 0.0) return 350.0;
    double d = lp_trust_deviation(t); return d < 30.0 ? 30.0 : d;
}
static Standing *stand_get(const lp_id *id, const Event *add, double trust){
    if (!add) { int64_t i = lp_idmap_find(smap, id); return i < 0 ? NULL : &stand[i]; }
    bool fresh; size_t i = lp_idmap_put(smap, id, &fresh); if (!fresh) return &stand[i];
    stand[i] = (Standing){ *id, { add->enter_rating, entry_deviation(add, trust), 0.06 }, 0, 0, 0 };  /* the stock default for its level of attestation */
    sn = lp_idmap_count(smap); return &stand[i];
}

/* What attestation already holds, by the witnessed thing and the witness that witnessed it: the same witness attesting
 * the same thing again, from other content, adds games to that row (part_write). Content already recorded attests
 * nothing again (load: a file whose content tree is recorded), so a retried job does not multiply the same witnessing. */
typedef struct { lp_id w, by; } Recorded;
static Recorded *ldg; static uint8_t *ldg_used; static uint64_t ldg_cap, ldg_n;
static uint64_t ldg_slot(const lp_id *w, const lp_id *by){ uint64_t a, b; memcpy(&a, w->b, 8); memcpy(&b, by->b + 8, 8); return (a ^ b * 0x9E3779B97F4A7C15ull) & (ldg_cap - 1); }
static void ldg_put(const lp_id *w, const lp_id *by){
    if ((ldg_n + 1) * 2 > ldg_cap) {                                         /* half full: twice the room */
        Recorded *o = ldg; uint8_t *ou = ldg_used; uint64_t oc = ldg_cap; ldg_cap = oc ? oc * 2 : 1 << 16;
        ldg = malloc(sizeof(Recorded) * ldg_cap); ldg_used = calloc(ldg_cap, 1);
        for (uint64_t i = 0; i < oc; i++) if (ou[i]) { uint64_t k = ldg_slot(&o[i].w, &o[i].by); while (ldg_used[k]) k = (k + 1) & (ldg_cap - 1); ldg_used[k] = 1; ldg[k] = o[i]; }
        free(o); free(ou); }
    uint64_t k = ldg_slot(w, by);
    while (ldg_used[k]) { if (!memcmp(&ldg[k].w, w, 16) && !memcmp(&ldg[k].by, by, 16)) return; k = (k + 1) & (ldg_cap - 1); }
    ldg_used[k] = 1; ldg[k].w = *w; ldg[k].by = *by; ldg_n++;
}
static int ldg_has(const lp_id *w, const lp_id *by){
    if (!ldg_cap) return 0; uint64_t k = ldg_slot(w, by);
    while (ldg_used[k]) { if (!memcmp(&ldg[k].w, w, 16) && !memcmp(&ldg[k].by, by, 16)) return 1; k = (k + 1) & (ldg_cap - 1); }
    return 0;
}
/* One partition of the semantics, on one connection: the attestations whose witnessed thing's ID begins with h, in
 * reading order (its order is the order of play), the new standings and the recorded ones updated. Each partition is
 * written by one connection, inside that connection's part of the batch's transaction (load: prepared, then committed
 * as one). Returns 0, or 1 when the database refused. */
static int part_write(PGconn *pg, int h, File *files, int nfiles, uint64_t *led, uint64_t *nnew, uint64_t *nupd){
    /* Each (witnessed, witness) is one attestation: its games the times this witness attested it, its score the series'
     * score, the mean over its games (a claim is a game series: games plus a score). A pair already recorded gains this
     * batch's games in one statement; a new pair is copied. */
    typedef struct { lp_id w, by; uint32_t games, position; double sum; } Series;
    Series *ser = NULL; uint64_t ns = 0, cs = 0; lp_idmap *mine = lp_idmap_new(); char sql[1024];
    for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i];
        if (e->kind == EV_MEMBER || (e->witnessed.b[0] >> 4) != h) continue;                /* witnessed within its record: the record's row */
        const lp_id *by = e->own_witness ? &e->witness : &files[fi].witness.id;
        lp_id pair; for (int b = 0; b < 16; b++) pair.b[b] = e->witnessed.b[b] ^ by->b[(b + 7) & 15];   /* both are hashes: their mix names the pair */
        bool fresh; size_t at = lp_idmap_put(mine, &pair, &fresh);
        if (fresh) { if (ns == cs) { cs = cs ? cs * 2 : 4096; ser = xrealloc(ser, sizeof(Series) * cs); } ser[ns++] = (Series){ e->witnessed, *by, 0, e->position, 0 }; }
        ser[at].games++; ser[at].sum += e->score; }
    lp_idmap_free(mine);
    Copy lc = { 0 }; snprintf(sql, sizeof sql, "COPY stage.attestation_%x (claim, witness, score, position, games) FROM STDIN (FORMAT binary)", h);
    copy_begin(&lc, pg, sql); uint64_t nold = 0;
    for (uint64_t j = 0; j < ns; j++) { const Series *x = &ser[j];
        if (ldg_has(&x->w, &x->by)) { ser[nold++] = *x; continue; }                         /* recorded already: gains its games below */
        c16(&lc, 5); cfield(&lc, x->w.b, 16); cfield(&lc, x->by.b, 16); cf_f32(&lc, (float)(x->sum / x->games));
        if (x->position) cf_i32(&lc, (int32_t)x->position); else c32(&lc, 0xFFFFFFFFu);
        cf_i32(&lc, (int32_t)x->games); lc.rows++; }
    copy_end(&lc); *led += lc.rows; free(lc.b);
    for (uint64_t j0 = 0; j0 < nold; j0 += 100000) {                             /* recorded series: one statement a chunk */
        uint32_t n = (uint32_t)(nold - j0 < 100000 ? nold - j0 : 100000); static const uint32_t oid[4] = { 0, 0, 23, 701 }; static const int w[4] = { 16, 16, 4, 8 };
        uint8_t *arr[4]; int alen[4];
        for (int f = 0; f < 4; f++) {
            arr[f] = malloc(20 + (size_t)n * (4 + w[f])); uint8_t *q = arr[f] + 20;
            uint32_t hdr[5] = { htonl(1), htonl(0), htonl(f < 2 ? id_oid : oid[f]), htonl(n), htonl(1) }; memcpy(arr[f], hdr, 20);
            for (uint32_t j = 0; j < n; j++) { const Series *x = &ser[j0 + j]; uint32_t l = htonl((uint32_t)w[f]); memcpy(q, &l, 4); q += 4;
                if (f == 0) memcpy(q, x->w.b, 16); else if (f == 1) memcpy(q, x->by.b, 16);
                else if (f == 2) { uint32_t g = htonl(x->games); memcpy(q, &g, 4); }
                else { uint64_t u; memcpy(&u, &x->sum, 8); for (int y = 0; y < 8; y++) q[y] = (uint8_t)(u >> (56 - 8 * y)); }
                q += w[f]; }
            alen[f] = (int)(q - arr[f]); }
        const char *v[4] = { (char *)arr[0], (char *)arr[1], (char *)arr[2], (char *)arr[3] }; int fm[4] = { 1, 1, 1, 1 };
        /* a pair staged already gains the games where it is staged; a pair only recorded is staged with what it will
         * be, its recorded games and these (merge puts the staged row in place of the recorded one) */
        snprintf(sql, sizeof sql, "WITH u AS (SELECT * FROM unnest($1::blake3[], $2::blake3[], $3::int[], $4::float8[]) AS u(c, w, g, s)), "
            "st AS (UPDATE stage.attestation_%x a SET score = (a.score * a.games + u.s) / (a.games + u.g), games = a.games + u.g "
            "FROM u WHERE a.claim = u.c AND a.witness = u.w RETURNING a.claim, a.witness) "
            "INSERT INTO stage.attestation_%x (claim, witness, score, position, games) "
            "SELECT a.claim, a.witness, (a.score * a.games + u.s) / (a.games + u.g), a.position, a.games + u.g "
            "FROM public.attestation_%x a JOIN u ON a.claim = u.c AND a.witness = u.w "
            "WHERE NOT EXISTS (SELECT 1 FROM st WHERE st.claim = u.c AND st.witness = u.w)", h, h, h);
        PGresult *u = PQexecParams(pg, sql, 4, NULL, v, alen, fm, 0); int bad = PQresultStatus(u) != PGRES_COMMAND_OK;
        if (bad) fprintf(stderr, "games: %s", PQerrorMessage(pg));
        PQclear(u); for (int f = 0; f < 4; f++) free(arr[f]); if (bad) { free(ser); return 1; } }
    free(ser);
    /* The standings: new ones, and recorded or staged ones this batch's matchups moved, staged as they now stand. A
     * standing staged before is taken out of the stage first; merge puts a staged standing in place of the recorded one. */
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
    /* The files first, by their trunks: a file whose trunk is recorded is recorded, with everything under it and
     * everything it attested, and nothing of it is looked for, played or written again. */
    { lp_id *trunk = malloc(sizeof(lp_id) * (size_t)(nfiles + 1)); int *of = malloc(sizeof(int) * (size_t)(nfiles + 1)); uint64_t nt = 0;
      for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { of[nt] = fi; trunk[nt++] = files[fi].file.id; }
      if (nt) { uint8_t *tt = malloc(nt); for (uint64_t i = 0; i < nt; i++) { Node *x = table_find(&trunk[i]); tt[i] = x ? x->tier : files[of[i]].file.tier; }
                uint8_t *hit = recorded(pg, npg, trunk, tt, nt); free(tt); st->checked += nt;
                for (uint64_t i = 0; i < nt; i++) if (hit[i]) { files[of[i]].known = 1; st->known++; st->found++; Node *x = table_find(&trunk[i]); if (x) x->keep = 2; }
                free(hit); }
      /* A file whose trunk is new but whose content tree is recorded (its OS record changed, its content did not):
       * what its content attests was attested when that content was recorded, and is not attested again. */
      nt = 0; for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file && files[fi].ev.n) { of[nt] = fi; trunk[nt++] = files[fi].trunk.id; }
      if (nt) { uint8_t *tt = malloc(nt); for (uint64_t i = 0; i < nt; i++) { Node *x = table_find(&trunk[i]); tt[i] = x ? x->tier : files[of[i]].trunk.tier; }
                uint8_t *hit = recorded(pg, npg, trunk, tt, nt); free(tt); st->checked += nt;
                for (uint64_t i = 0; i < nt; i++) if (hit[i]) { files[of[i]].ev.n = 0; st->content_known++; }
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

    /* ---- semantics: witnesses, attestations, and standings played in reading order */
    t = now(); double tp = t; uint64_t nev = 0;      /* tp: where each part of it began */
    for (int fi = 0; fi < nfiles; fi++) { if (files[fi].known) { free(files[fi].ev.e); memset(&files[fi].ev, 0, sizeof files[fi].ev); } nev += files[fi].ev.n; }
    /* What the files attested goes in the same transaction as their nodes: all of it is recorded, or none. */
    if (nev) {
        smap = lp_idmap_new(); stand = malloc(sizeof(Standing) * (nev + 1)); sn = 0;
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) if (files[fi].ev.e[i].kind != EV_RECORD) stand_get(&files[fi].ev.e[i].claim, &files[fi].ev.e[i], files[fi].trust);
        /* claims already recorded start from their recorded standing */
        /* the statistics are partitioned by the claim's first hex digit: each read goes to its one partition */
        lp_id *old = malloc(sizeof(lp_id) * (sn + 1)); uint64_t nold = 0, ocnt[17] = { 0 };
        for (uint64_t i = 0; i < sn; i++) { Node *x = table_find(&stand[i].id); if (!x || x->keep != 1) { old[nold++] = stand[i].id; ocnt[(stand[i].id.b[0] >> 4) + 1]++; } }
        for (int h = 0; h < 16; h++) ocnt[h + 1] += ocnt[h];
        { lp_id *by = malloc(sizeof(lp_id) * (nold + 1)); uint64_t fill[16]; memcpy(fill, ocnt, sizeof fill); for (uint64_t i = 0; i < nold; i++) by[fill[old[i].b[0] >> 4]++] = old[i]; free(old); old = by; }
        const uint64_t CH = 100000; typedef struct { int h; uint64_t lo, n; } OJob; OJob *oj = malloc(sizeof(OJob) * (nold / CH + 17)); uint64_t noj = 0;
        for (int h = 0; h < 16; h++) for (uint64_t lo = ocnt[h]; lo < ocnt[h + 1]; lo += CH) oj[noj++] = (OJob){ h, lo, ocnt[h + 1] - lo < CH ? ocnt[h + 1] - lo : CH };
        #pragma omp parallel for num_threads(npg) schedule(dynamic)
        for (uint64_t j = 0; j < noj; j++) {
            uint32_t k = (uint32_t)oj[j].n; uint8_t *ab = malloc(20 + 20 * (size_t)k);
            size_t len = ids_param(ab, old + oj[j].lo, k); PGconn *c = pg[omp_get_thread_num()];
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
            for (int from = 0; from < 2; from++) {                            /* recorded, then staged: a staged standing is the later one */
                char sql[160]; snprintf(sql, sizeof sql, "SELECT claim, rating, deviation, volatility, matches FROM %s.consensus_%x WHERE claim = ANY($1::blake3[])", from ? "stage" : "public", oj[j].h);
                PGresult *q = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
                if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "standing: %s", PQerrorMessage(c)); exit(1); }
                #pragma omp critical
                for (int j = 0; j < PQntuples(q); j++) {
                    lp_id id; memcpy(id.b, PQgetvalue(q, j, 0), 16); Standing *s = stand_get(&id, NULL, 0); if (!s) continue;
                    double d[3]; for (int z = 0; z < 3; z++) { uint64_t u = 0; const uint8_t *b = (const uint8_t *)PQgetvalue(q, j, 1 + z); for (int y = 0; y < 8; y++) u = u << 8 | b[y]; memcpy(&d[z], &u, 8); }
                    const uint8_t *mb = (const uint8_t *)PQgetvalue(q, j, 4);
                    s->r = (lp_rating){ d[0], d[1], d[2] }; s->matches = s->m0 = (uint32_t)mb[0] << 24 | mb[1] << 16 | mb[2] << 8 | mb[3]; s->had = 1;
                }
                PQclear(q); }
            free(ab);
        }
        free(old); free(oj);
        /* What was witnessed plays once per lineage: a copy of it is an attestation and nothing more. What this
         * lineage witnessed before is read from attestation; what it witnesses in this run is kept here. */
        typedef struct { lp_id witnessed, lin; uint8_t used; } Seen;
        uint64_t nrec = 0; for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) nrec += files[fi].ev.e[i].kind != EV_MEMBER;
        lp_id *wold = malloc(sizeof(lp_id) * (nrec + 1)); uint64_t nwold = 0;
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i]; if (e->kind == EV_MEMBER) continue;
            Node *x = table_find(&e->witnessed); if (!x || x->keep != 1) wold[nwold++] = e->witnessed; }
        uint64_t lin_at[17] = { 0 }; uint64_t pcap = 1024; while (pcap < nrec * 3) pcap <<= 1; Seen *seen = calloc(pcap, sizeof(Seen)); uint64_t nseen = 0;
        #define SEEN_AT(w, l, found) do { uint64_t h_; memcpy(&h_, (w)->b, 8); uint64_t c_; memcpy(&c_, (l)->b + 8, 8); h_ ^= c_ * 0x9E3779B97F4A7C15ull; k_ = h_ & (pcap - 1); found = 0; \
            while (seen[k_].used) { if (!memcmp(seen[k_].witnessed.b, (w)->b, 16) && !memcmp(seen[k_].lin.b, (l)->b, 16)) { found = 1; break; } k_ = (k_ + 1) & (pcap - 1); } } while (0)
        /* half full: twice the room. It is sized by the batch's records, and attestation adds every lineage it already
         * holds for them, as many as there are: a full table probed for ever */
        #define SEEN_PUT(w, l) do { if ((nseen + 1) * 2 > pcap) { uint64_t oc_ = pcap; Seen *o_ = seen; pcap <<= 1; seen = calloc(pcap, sizeof(Seen)); \
                for (uint64_t q_ = 0; q_ < oc_; q_++) if (o_[q_].used) { uint64_t k_; int f_; SEEN_AT(&o_[q_].witnessed, &o_[q_].lin, f_); (void)f_; seen[k_] = o_[q_]; } free(o_); } \
            uint64_t k_; int f_; SEEN_AT(w, l, f_); if (!f_) { seen[k_].used = 1; seen[k_].witnessed = *(w); seen[k_].lin = *(l); nseen++; } } while (0)
        /* each claim read from the one partition of attestation it is in (its ID's first hex digit): asked of the whole
         * table, every one of the 16 partitions probes every claim of the batch */
        { lp_id *by = malloc(sizeof(lp_id) * (nwold + 1)); uint64_t at[17] = { 0 };
          for (uint64_t i = 0; i < nwold; i++) at[(wold[i].b[0] >> 4) + 1]++;
          for (int h = 0; h < 16; h++) at[h + 1] += at[h];
          uint64_t put[16]; memcpy(put, at, sizeof put); for (uint64_t i = 0; i < nwold; i++) by[put[wold[i].b[0] >> 4]++] = wold[i];
          free(wold); wold = by; memcpy(lin_at, at, sizeof at); }
        OJob *lj = malloc(sizeof(OJob) * (nwold / CH + 17)); uint64_t nlj = 0;
        for (int h = 0; h < 16; h++) for (uint64_t lo = lin_at[h]; lo < lin_at[h + 1]; lo += CH) lj[nlj++] = (OJob){ h, lo, lin_at[h + 1] - lo < CH ? lin_at[h + 1] - lo : CH };
        #pragma omp parallel for num_threads(npg) schedule(dynamic)
        for (uint64_t jx = 0; jx < nlj; jx++) {
            uint64_t i0 = lj[jx].lo; uint32_t k = (uint32_t)lj[jx].n; uint8_t *ab = malloc(20 + 20 * (size_t)k);
            size_t len = ids_param(ab, wold + i0, k); PGconn *c = pg[omp_get_thread_num()];
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 }; char sql[512];
            snprintf(sql, sizeof sql, "SELECT a.claim, w.id, w.lineage FROM (SELECT claim, witness FROM public.attestation_%x WHERE claim = ANY($1::blake3[]) "
                                      "UNION SELECT claim, witness FROM stage.attestation_%x WHERE claim = ANY($1::blake3[])) a JOIN witness w ON w.id = a.witness", lj[jx].h, lj[jx].h);
            PGresult *q = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "lineage: %s", PQerrorMessage(c)); exit(1); }
            #pragma omp critical
            for (int j = 0; j < PQntuples(q); j++) {
                lp_id wit, wid, lin; memcpy(wit.b, PQgetvalue(q, j, 0), 16); memcpy(wid.b, PQgetvalue(q, j, 1), 16);
                if (PQgetisnull(q, j, 2)) lin = wid; else memcpy(lin.b, PQgetvalue(q, j, 2), 16);
                SEEN_PUT(&wit, &lin);
                ldg_put(&wit, &wid);
            }
            PQclear(q); free(ab);
        }
        free(wold); free(lj);
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
                    uint64_t k_; SEEN_AT(&e->witnessed, lin, copy);
                    if (!copy) SEEN_PUT(&e->witnessed, lin);
                    if (e->kind == EV_RECORD) continue;
                }
                if (!copy) play[x] = (uint8_t)(e->claim.b[7] % NP);
            } } }
        free(seen);
        #pragma omp parallel for schedule(dynamic, 1)
        for (int p = 0; p < NP; p++) { uint64_t x = 0;
            for (int fi = 0; fi < nfiles; fi++) { double trust = files[fi].trust;
                for (uint64_t i = 0; i < files[fi].ev.n; i++, x++) { if (play[x] != p) continue; const Event *e = &files[fi].ev.e[i];
                    Standing *s = stand_get(&e->claim, NULL, 0);
                    if (!s->had && !s->entered) s->entered = 1;
                    lp_attest(&s->r, trust, e->score, e->enter_rating, 0.5, 30.0); s->matches++; } } }
        free(play);
        st->t_play += now() - tp; tp = now();
        Copy c = { 0 };
        /* witnesses: each once, and only those the database does not know yet */
        lp_id *wid = malloc(sizeof(lp_id) * (size_t)nfiles); int *wfile = malloc(sizeof(int) * (size_t)nfiles), nw = 0; lp_idmap *wmap = lp_idmap_new();
        for (int fi = 0; fi < nfiles; fi++) if (files[fi].ev.n) {                  /* the map's places are the order of first meeting: wid's */
            bool fresh; lp_idmap_put(wmap, &files[fi].witness.id, &fresh);
            if (fresh) { wid[nw] = files[fi].witness.id; wfile[nw++] = fi; }
        }
        /* witnesses a source names statement by statement: each is its own lineage, and plays at the source's trust */
        lp_id *own = NULL; double *owntrust = NULL; uint64_t nown = 0, cown = 0; lp_idmap *oseen = lp_idmap_new();
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i]; if (!e->own_witness) continue;
            bool fresh; lp_idmap_put(oseen, &e->witness, &fresh); if (!fresh) continue;
            if (nown == cown) { cown = cown ? cown * 2 : 4096; own = xrealloc(own, cown * sizeof(lp_id)); owntrust = xrealloc(owntrust, cown * 8); }
            own[nown] = e->witness; owntrust[nown] = files[fi].trust; nown++; }
        uint8_t *oknown = calloc(nown ? nown : 1, 1);
        for (uint64_t i0 = 0; i0 < nown; i0 += 50000) { uint32_t k = (uint32_t)(nown - i0 < 50000 ? nown - i0 : 50000); uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t len = ids_param(ab, own + i0, k);
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
            PGresult *q = PQexecParams(pg[0], "SELECT u.i FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i) JOIN witness w ON w.id = u.id", 1, NULL, v, l, f, 0);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "witnesses: %s", PQerrorMessage(pg[0])); return 1; }
            for (int j = 0; j < PQntuples(q); j++) oknown[i0 + (uint64_t)atoll(PQgetvalue(q, j, 0)) - 1] = 1;
            PQclear(q); free(ab); }
        uint8_t *known = calloc((size_t)(nw ? nw : 1), 1);
        { uint8_t *ab = malloc(20 + 20 * (size_t)nw); size_t len = ids_param(ab, wid, (uint32_t)nw);
          const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
          PGresult *q = PQexecParams(pg[0], "SELECT id FROM witness WHERE id = ANY($1::blake3[])", 1, NULL, v, l, f, 1);
          if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "witnesses: %s", PQerrorMessage(pg[0])); return 1; }
          for (int j = 0; j < PQntuples(q); j++) { lp_id x; memcpy(x.b, PQgetvalue(q, j, 0), 16); int64_t k = lp_idmap_find(wmap, &x); if (k >= 0) known[k] = 1; }
          PQclear(q); free(ab); }
        copy_begin(&c, pg[0], "COPY witness (id, lineage, trust) FROM STDIN (FORMAT binary)");
        for (int k = 0; k < nw; k++) if (!known[k]) {
            int fi = wfile[k];
            c16(&c, 3); cfield(&c, files[fi].witness.id.b, 16);
            if (files[fi].has_lineage) cfield(&c, files[fi].lineage.id.b, 16); else c32(&c, 0xFFFFFFFFu);
            cf_f64(&c, files[fi].trust);
        }
        for (uint64_t i = 0; i < nown; i++) if (!oknown[i]) { if (lp_idmap_find(wmap, &own[i]) >= 0) continue;
            c16(&c, 3); cfield(&c, own[i].b, 16); c32(&c, 0xFFFFFFFFu); cf_f64(&c, owntrust[i]); }
        copy_end(&c); free(wid); free(wfile); free(known); free(own); free(owntrust); lp_idmap_free(oseen); free(oknown); lp_idmap_free(wmap);
        st->t_wit += now() - tp; tp = now();
        /* attestations and standings, a partition a connection at a time on every connection: each connection's
         * part of the batch's transaction (prepared and committed as one below) */
        uint64_t led = 0, nnew = 0, nupd = 0; int bad = 0;
        #pragma omp parallel for num_threads(nparts) schedule(static, 1) reduction(+:led, nnew, nupd, bad)
        for (int j = 0; j < nparts; j++) for (int h = j; h < 16; h += nparts) { uint64_t a = 0, b = 0, c = 0; bad += part_write(pg[j], h, files, nfiles, &a, &b, &c); led += a; nnew += b; nupd += c; }
        if (bad) return 1;
        st->led += led; st->std_new += nnew; st->std_upd += nupd;
        free(ldg); free(ldg_used); ldg = NULL; ldg_used = NULL; ldg_cap = ldg_n = 0;
        st->t_led += now() - tp; tp = now();
    }
    if (nev) { free(stand); lp_idmap_free(smap); stand = NULL; smap = NULL; }
    st->t_sem += now() - t;

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

/* ---- merge: what the runs staged goes into the real tables, as one transaction in parts, one a connection (prepared,
 * then committed, as a batch's was). A leaf partition with rows staged for it takes them one of two ways, whichever
 * writes less:
 *   rewritten: the leaf is truncated and copied back whole, what it held and what was staged, in its order (Hilbert for
 *     entity and physicality, the claim for attestation and consensus), FREEZE. A truncated table's new files are
 *     written without WAL when wal_level is minimal (they are synced when the transaction is prepared instead), indexes
 *     with it, and frozen rows leave no hint bits for a later read to log (data_checksums is on). TRUNCATE locks that
 *     leaf alone, so the connections never wait on each other.
 *   appended: the staged rows are inserted, logged as any write is. A leaf that takes a few rows takes them this way.
 * What an append logs: the staged rows, and an image of every index page they land on (one a page per checkpoint,
 * up to all of the index's pages). What a rewrite costs: the leaf read and written once, nothing logged. With
 * wal_level above minimal a rewrite is logged whole, so every leaf is appended.
 * An entity leaf is always appended: a physicality's entity is a foreign key to it, and a table a foreign key points
 * at cannot be truncated alone. An entity leaf and the physicality leaf of the same range go to one connection, the
 * entities first, so the paths it copies find their entities in its own part of the transaction. */
typedef struct { char name[64]; int table; double live, staged_rows, staged_bytes, append_cost, rewrite_cost; int rewrite, worker; } MLeaf;
enum { M_ENTITY, M_PHYS, M_ATT, M_CONS };
static const char *M_ORDER[4] = { "hilbert", "hilbert", "claim, witness", "claim" };
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
    const char *same = L->table == M_ATT ? "s.claim = l.claim AND s.witness = l.witness" : "s.claim = l.claim";   /* a staged row in place of the recorded one */
    if (L->rewrite) {
        /* read whole, before the leaf is truncated: what it held (less what the stage holds in its place) and what was staged */
        if (L->table >= M_ATT) snprintf(sql, sl, "COPY (SELECT %s FROM public.%s l WHERE NOT EXISTS (SELECT 1 FROM stage.%s s WHERE %s) UNION ALL SELECT %s FROM stage.%s ORDER BY %s) TO STDOUT (FORMAT binary)",
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
        if (L->table >= M_ATT) {
            const char *set = L->table == M_ATT ? "score = s.score, games = s.games" : "rating = s.rating, deviation = s.deviation, volatility = s.volatility, matches = s.matches";
            snprintf(sql, sl, "UPDATE public.%s l SET %s FROM stage.%s s WHERE %s", L->name, set, L->name, same); bad = !must(w, sql);
            if (!bad) { snprintf(sql, sl, "INSERT INTO public.%s (%s) SELECT %s FROM stage.%s s WHERE NOT EXISTS (SELECT 1 FROM public.%s l WHERE %s) ORDER BY %s",
                                 L->name, cols, cols, L->name, L->name, same, M_ORDER[L->table]); bad = !must(w, sql); } }
        else { snprintf(sql, sl, "INSERT INTO public.%s (%s) SELECT %s FROM stage.%s ORDER BY %s", L->name, cols, cols, L->name, M_ORDER[L->table]); bad = !must(w, sql); }
        *rows += (uint64_t)L->staged_rows;
    }
    /* the container index takes what it was handed into its own list: merged here, inside the transaction, so no
     * lookup reads a pending list through (in a rewritten leaf this too is written without WAL) */
    if (!bad && L->table == M_PHYS) {
        const char *v[1] = { L->name };
        PGresult *q = PQexecParams(w, "SELECT gin_clean_pending_list(i.indexrelid::regclass) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam "
                                      "WHERE i.indrelid = ('public.' || quote_ident($1))::regclass AND a.amname = 'gin'", 1, NULL, v, NULL, NULL, 0);
        if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "merge %s, container index: %s", L->name, PQerrorMessage(w)); bad = 1; } PQclear(q); }
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
    r = PQexec(pg0, "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'stage' AND c.relkind = 'r' ORDER BY 1");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "merge: %s", PQerrorMessage(pg0)); return 1; }
    int nl = PQntuples(r); MLeaf *L = calloc((size_t)nl + 1, sizeof(MLeaf));
    for (int i = 0; i < nl; i++) { snprintf(L[i].name, sizeof L[i].name, "%s", PQgetvalue(r, i, 0));
        L[i].table = !strncmp(L[i].name, "entity", 6) ? M_ENTITY : !strncmp(L[i].name, "physicality", 11) ? M_PHYS : !strncmp(L[i].name, "attestation", 11) ? M_ATT : M_CONS; }
    PQclear(r);
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg), **rd = malloc(sizeof(PGconn *) * (size_t)npg);
    for (int i = 0; i < npg; i++) { pg[i] = db_connect(conninfo); rd[i] = db_connect(conninfo); PQclear(PQexec(pg[i], "SET synchronous_commit = off")); }
    int bad = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic, 1) reduction(+:bad)
    for (int i = 0; i < nl; i++) {
        PGconn *c = rd[omp_get_thread_num()]; const char *v[1] = { L[i].name }; char sql[512];
        snprintf(sql, sizeof sql, "SELECT (SELECT count(*) FROM stage.%s), pg_relation_size('stage.%s'), pg_relation_size('public.%s'), pg_total_relation_size('public.%s')",
                 L[i].name, L[i].name, L[i].name, L[i].name);
        PGresult *q = PQexec(c, sql);
        if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "merge %s: %s", L[i].name, PQerrorMessage(c)); bad++; PQclear(q); continue; }
        L[i].staged_rows = atof(PQgetvalue(q, 0, 0)); L[i].staged_bytes = atof(PQgetvalue(q, 0, 1)); double total = atof(PQgetvalue(q, 0, 3));
        L[i].live = total; PQclear(q);
        if (!L[i].staged_rows) continue;
        char rows[32]; snprintf(rows, sizeof rows, "%.0f", L[i].staged_rows); const char *v2[2] = { v[0], rows };
        q = PQexecParams(c, "SELECT coalesce(sum(least($2::float8, c.relpages::float8)), 0) * 8192 FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid "
                            "WHERE i.indrelid = ('public.' || quote_ident($1))::regclass", 2, NULL, v2, NULL, NULL, 0);
        double images = PQresultStatus(q) == PGRES_TUPLES_OK ? atof(PQgetvalue(q, 0, 0)) : 0; PQclear(q);
        L[i].append_cost = L[i].staged_bytes + images;
        L[i].rewrite_cost = 2.0 * (total + L[i].staged_bytes);
        L[i].rewrite = minimal && L[i].table != M_ENTITY && L[i].rewrite_cost < L[i].append_cost;
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
    char xid0[32] = ""; for (int j = 0; j < npg; j++) if (!must(pg[j], "BEGIN")) return 1;
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
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg); for (int i = 0; i < npg; i++) { pg[i] = db_connect(conninfo); PQclear(PQexec(pg[i], "SET synchronous_commit = off")); }
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
