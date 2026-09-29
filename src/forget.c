/* laplace forget: what one witness attested, taken back out.
 *   laplace forget [-d conninfo] [-j connections] witness
 * The witness's rows leave the ledger. A claim no other witness attests goes with its standing; a claim others attest
 * stays (its standing is theirs to replay). Then, level by level down the DAG, whatever those claims related that
 * nothing holds any more goes too: an entity stays while any path holds it, or while it is a source's trunk or a
 * witness. Atoms always stay. SQL fetches and deletes sets of rows; what to delete is decided here.
 *
 * laplace sweep: whatever nothing holds, removed.
 *   laplace sweep [-d conninfo] [-j connections] [--dry]
 * One pass over every path counts, for every entity, the places that hold it. An entity no path holds goes, unless it is
 * a source's trunk, a witness, or a claim with a standing; what it held is counted down, and goes in turn when its count
 * reaches nothing. */
#include "engine.h"
#include <locale.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { lp_id *id; uint64_t n, cap; uint32_t *slot; uint64_t scap; } Set;
static uint64_t key(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 6, 8); return k; }
static int set_add(Set *s, const lp_id *id){                              /* 1 if it was not there */
    if ((s->n + 1) * 2 > s->scap) {
        free(s->slot); s->scap = s->scap ? s->scap * 2 : 1 << 16; s->slot = calloc(s->scap, 4);
        for (uint64_t i = 0; i < s->n; i++) { uint64_t k = key(&s->id[i]) & (s->scap - 1); while (s->slot[k]) k = (k + 1) & (s->scap - 1); s->slot[k] = (uint32_t)i + 1; }
    }
    uint64_t k = key(id) & (s->scap - 1);
    while (s->slot[k]) { if (!memcmp(&s->id[s->slot[k] - 1], id, 16)) return 0; k = (k + 1) & (s->scap - 1); }
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1 << 16; s->id = xrealloc(s->id, s->cap * sizeof(lp_id)); }
    s->id[s->n] = *id; s->slot[k] = (uint32_t)++s->n; return 1;
}
static void set_free(Set *s){ free(s->id); free(s->slot); memset(s, 0, sizeof *s); }

static void must(PGconn *pg, PGresult *r, ExecStatusType want, const char *what){
    if (PQresultStatus(r) != want) { fprintf(stderr, "%s: %s", what, PQerrorMessage(pg)); exit(1); }
}
#define CHUNK 50000
/* One statement over a set of IDs, in chunks, on every connection at once. Rows of a fetch go to `each`. */
typedef void (*Each)(PGresult *, int row, void *into);
static uint64_t over(PGconn **pg, int npg, const lp_id *ids, uint64_t n, const char *sql, Each each, void *into, const char *what){
    uint64_t rows = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic) reduction(+:rows)
    for (uint64_t i0 = 0; i0 < n; i0 += CHUNK) {
        uint32_t k = (uint32_t)(n - i0 < CHUNK ? n - i0 : CHUNK); uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t al = uuid_param(ab, ids + i0, k);
        PGconn *c = pg[omp_get_thread_num()]; const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGresult *r = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
        must(c, r, each ? PGRES_TUPLES_OK : PGRES_COMMAND_OK, what);
        if (each) {
            #pragma omp critical(forget_rows)
            for (int j = 0; j < PQntuples(r); j++) each(r, j, into);
            rows += (uint64_t)PQntuples(r);
        } else rows += strtoull(PQcmdTuples(r), NULL, 10);
        PQclear(r); free(ab);
    }
    return rows;
}
static void each_id(PGresult *r, int j, void *into){ lp_id id; memcpy(id.b, PQgetvalue(r, j, 0), 16); set_add(into, &id); }
/* The constituents of a fetched path that are not atoms. */
static void each_part(PGresult *r, int j, void *into){
    const uint8_t *vx; size_t nv = lp_ewkb_vertices((const uint8_t *)PQgetvalue(r, j, 0), (size_t)PQgetlength(r, j, 0), &vx);
    for (size_t i = 0; i < nv; i++) { double xyz[3]; memcpy(xyz, vx + 32 * i, 24); lp_id id; lp_xyz_to_id(xyz, &id); if (lp_tier0_codepoint(T0, &id) < 0) set_add(into, &id); }
}

/* ---- the sweep */
typedef struct { lp_id id; uint32_t held; uint8_t entity, root; } Count;
typedef struct { pthread_mutex_t mu; Count *c; uint64_t n, cap; uint32_t *slot; uint64_t scap; } CShard;
static CShard cs[256];
static Count *count_of(CShard *s, const lp_id *id, int add){                /* the shard's lock is held */
    if (add && (s->n + 1) * 2 > s->scap) {
        free(s->slot); s->scap = s->scap ? s->scap * 2 : 1 << 14; s->slot = calloc(s->scap, 4);
        for (uint64_t i = 0; i < s->n; i++) { uint64_t k = key(&s->c[i].id) & (s->scap - 1); while (s->slot[k]) k = (k + 1) & (s->scap - 1); s->slot[k] = (uint32_t)i + 1; }
    }
    if (!s->scap) return NULL;
    uint64_t k = key(id) & (s->scap - 1);
    while (s->slot[k]) { if (!memcmp(&s->c[s->slot[k] - 1].id, id, 16)) return &s->c[s->slot[k] - 1]; k = (k + 1) & (s->scap - 1); }
    if (!add) return NULL;
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1 << 14; s->c = xrealloc(s->c, s->cap * sizeof(Count)); }
    Count *c = &s->c[s->n]; memset(c, 0, sizeof *c); c->id = *id; s->slot[k] = (uint32_t)++s->n; return c;
}
/* A thread's pending counts, by shard, applied under that shard's lock a batch at a time. what: 0 held once more,
 * 1 it is an entity, 2 it is a root. */
typedef struct { lp_id id; uint8_t what; } Pend;
typedef struct { Pend *p[256]; uint32_t n[256]; } Batch;
#define BATCH 2048
static void batch_flush(Batch *b, int sh){
    CShard *s = &cs[sh]; pthread_mutex_lock(&s->mu);
    for (uint32_t i = 0; i < b->n[sh]; i++) { Count *c = count_of(s, &b->p[sh][i].id, 1); if (b->p[sh][i].what == 0) c->held++; else if (b->p[sh][i].what == 1) c->entity = 1; else c->root = 1; }
    pthread_mutex_unlock(&s->mu); b->n[sh] = 0;
}
static void batch_put(Batch *b, const lp_id *id, uint8_t what){
    int sh = id->b[0]; if (!b->p[sh]) b->p[sh] = malloc(sizeof(Pend) * BATCH);
    b->p[sh][b->n[sh]++] = (Pend){ *id, what }; if (b->n[sh] == BATCH) batch_flush(b, sh);
}
static void batch_done(Batch *b){ for (int sh = 0; sh < 256; sh++) { if (b->n[sh]) batch_flush(b, sh); free(b->p[sh]); } }
/* Every row of a statement, one at a time as it arrives. */
static uint64_t stream(PGconn *c, const char *sql, void (*row)(PGresult *, Batch *), Batch *b){
    if (!PQsendQueryParams(c, sql, 0, NULL, NULL, NULL, NULL, 1) || !PQsetSingleRowMode(c)) { fprintf(stderr, "%s: %s", sql, PQerrorMessage(c)); exit(1); }
    PGresult *r; uint64_t n = 0;
    while ((r = PQgetResult(c))) {
        if (PQresultStatus(r) == PGRES_SINGLE_TUPLE) { row(r, b); n++; }
        else if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "%s: %s", sql, PQerrorMessage(c)); exit(1); }
        PQclear(r);
    }
    return n;
}
static void row_path(PGresult *r, Batch *b){
    lp_id e; memcpy(e.b, PQgetvalue(r, 0, 0), 16); batch_put(b, &e, 1);
    const uint8_t *vx; size_t nv = lp_ewkb_vertices((const uint8_t *)PQgetvalue(r, 0, 1), (size_t)PQgetlength(r, 0, 1), &vx);
    for (size_t i = 0; i < nv; i++) { double xyz[3]; memcpy(xyz, vx + 32 * i, 24); lp_id id; lp_xyz_to_id(xyz, &id); batch_put(b, &id, 0); }
}
static void row_root(PGresult *r, Batch *b){ for (int f = 0; f < PQnfields(r); f++) if (!PQgetisnull(r, 0, f)) { lp_id id; memcpy(id.b, PQgetvalue(r, 0, f), 16); batch_put(b, &id, 2); } }
/* What an entity that is going held: each counted down; whatever reaches nothing goes next. */
static void each_release(PGresult *r, int j, void *into){
    const uint8_t *vx; size_t nv = lp_ewkb_vertices((const uint8_t *)PQgetvalue(r, j, 0), (size_t)PQgetlength(r, j, 0), &vx);
    for (size_t i = 0; i < nv; i++) {
        double xyz[3]; memcpy(xyz, vx + 32 * i, 24); lp_id id; lp_xyz_to_id(xyz, &id);
        Count *c = count_of(&cs[id.b[0]], &id, 0); if (!c || !c->held) continue;
        if (!--c->held && c->entity && !c->root) set_add(into, &id);
    }
}
static uint64_t sweep(PGconn **pg, int npg, int dry){
    double T = now(), t = now(); for (int i = 0; i < 256; i++) pthread_mutex_init(&cs[i].mu, NULL);
    /* every partition of paths above tier 0, each on a connection */
    PGresult *r = PQexec(pg[0], "SELECT c.relname FROM pg_class c JOIN pg_inherits i ON i.inhrelid = c.oid WHERE c.relkind = 'r' "
                                "AND c.relname ~ '^physicality_t([1-9][0-9]*|x)_[0-9a-f]$' ORDER BY c.reltuples DESC");
    must(pg[0], r, PGRES_TUPLES_OK, "partitions"); int nparts = PQntuples(r); uint64_t paths = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic) reduction(+:paths)
    for (int p = 0; p < nparts; p++) {
        Batch b = { 0 }; char sql[160]; snprintf(sql, sizeof sql, "SELECT entity, path FROM %s", PQgetvalue(r, p, 0));
        paths += stream(pg[omp_get_thread_num()], sql, row_path, &b); batch_done(&b);
    }
    PQclear(r);
    printf("  %-52s %'12llu   (%.1f s)\n", "paths read, every holder counted", (unsigned long long)paths, now() - t); fflush(stdout);
    t = now(); { Batch b = { 0 };
      stream(pg[0], "SELECT claim FROM standing", row_root, &b); stream(pg[0], "SELECT trunk FROM source", row_root, &b);
      stream(pg[0], "SELECT id, lineage FROM witness", row_root, &b); batch_done(&b); }
    Set going = { 0 }; uint64_t entities = 0, roots = 0;
    for (int sh = 0; sh < 256; sh++) for (uint64_t i = 0; i < cs[sh].n; i++) {
        Count *c = &cs[sh].c[i]; entities += c->entity; roots += c->root && c->entity;
        if (c->entity && !c->held && !c->root) set_add(&going, &c->id);
    }
    printf("  %-52s %'12llu   of them claims, trunks and witnesses %'llu   (%.1f s)\n", "entities above tier 0", (unsigned long long)entities, (unsigned long long)roots, now() - t);
    uint64_t total = 0; int level = 0;
    while (going.n) {
        t = now(); level++; Set next = { 0 };
        if (level == 1) {                                                   /* what is going, shown before it goes */
            Reader *rd = reader_new(pg[0]); printf("  held by nothing, for example:");
            for (uint64_t i = 0; i < going.n && i < 12; i++) { char *tx = reader_text(rd, &going.id[i * (going.n / 12 ? going.n / 12 : 1) % going.n], 40); printf("%s \"%s\"", i ? "," : "", tx); free(tx); }
            printf("\n"); reader_free(rd);
        }
        over(pg, npg, going.id, going.n, "SELECT path FROM physicality WHERE entity = ANY($1::uuid[])", each_release, &next, "what they held");
        if (!dry) { over(pg, npg, going.id, going.n, "DELETE FROM physicality WHERE entity = ANY($1::uuid[])", NULL, NULL, "paths");
                    over(pg, npg, going.id, going.n, "DELETE FROM entity WHERE id = ANY($1::uuid[])", NULL, NULL, "entities"); }
        total += going.n;
        printf("  level %d: %'12llu held by nothing%s   (%.1f s)\n", level, (unsigned long long)going.n, dry ? "" : ", removed", now() - t); fflush(stdout);
        set_free(&going); going = next;
    }
    printf("  %-52s %'12llu%s   (%.1f s)\n", "entities nothing held", (unsigned long long)total, dry ? "   (dry: nothing was removed)" : "", now() - T);
    for (int sh = 0; sh < 256; sh++) { free(cs[sh].c); free(cs[sh].slot); memset(&cs[sh], 0, sizeof cs[sh]); }
    return total;
}
int cmd_sweep(int argc, char **argv){
    const char *conninfo = laplace_db(); int npg = 0, dry = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-j") && a + 1 < argc) npg = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--dry")) dry = 1;
        else { fprintf(stderr, "usage: laplace sweep [-d conninfo] [-j connections] [--dry]\n"); return 2; }
    }
    if (npg <= 0) npg = omp_get_num_procs();
    setlocale(LC_NUMERIC, "en_US.UTF-8"); tier0_open(NULL);
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg); for (int i = 0; i < npg; i++) pg[i] = db_connect(conninfo);
    printf("laplace sweep   %s%s\n", PQdb(pg[0]), dry ? "   (dry)" : "");
    sweep(pg, npg, dry);
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}

int cmd_forget(int argc, char **argv){
    const char *conninfo = laplace_db(); int npg = 0, a = 1;
    for (; a < argc - 1 && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-j") && a + 1 < argc) npg = atoi(argv[++a]);
    }
    if (a >= argc) { fprintf(stderr, "usage: laplace forget [-d conninfo] [-j connections] witness\n"); return 2; }
    if (npg <= 0) npg = omp_get_num_procs();
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    double T = now(), t; tier0_open(NULL); Ctx *c = lp_text_new(T0);
    lp_id wid = lp_text_decompose(c, (const uint8_t *)argv[a], strlen(argv[a]), NULL, NULL).id; char wt[37]; id_text(&wid, wt);
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg); for (int i = 0; i < npg; i++) pg[i] = db_connect(conninfo);
    printf("laplace forget   %s   witness %s   %s\n", PQdb(pg[0]), argv[a], wt);

    /* what it attested */
    t = now(); Set claims = { 0 }, shared = { 0 }, gone = { 0 }, parts = { 0 };
    { const char *v[1] = { wt }; PGresult *r = PQexecParams(pg[0], "SELECT DISTINCT claim FROM attestation WHERE witness = $1::uuid", 1, NULL, v, NULL, NULL, 1);
      must(pg[0], r, PGRES_TUPLES_OK, "its claims"); for (int j = 0; j < PQntuples(r); j++) each_id(r, j, &claims); PQclear(r); }
    if (!claims.n) { printf("  it attested nothing here\n"); return 1; }
    { char sql[256]; snprintf(sql, sizeof sql, "SELECT DISTINCT claim FROM attestation WHERE claim = ANY($1::uuid[]) AND witness <> '%s'::uuid", wt);
      over(pg, npg, claims.id, claims.n, sql, each_id, &shared, "claims others attest"); }
    for (uint64_t i = 0; i < claims.n; i++) {
        uint64_t k = shared.scap ? key(&claims.id[i]) & (shared.scap - 1) : 0; int held = 0;
        while (shared.scap && shared.slot[k]) { if (!memcmp(&shared.id[shared.slot[k] - 1], &claims.id[i], 16)) { held = 1; break; } k = (k + 1) & (shared.scap - 1); }
        if (!held) set_add(&gone, &claims.id[i]);
    }
    printf("  %-52s %'12llu   (%.1f s)\n", "claims it attested", (unsigned long long)claims.n, now() - t);
    printf("  %-52s %'12llu\n", "of them, attested by others too: they stay", (unsigned long long)shared.n);

    /* the ledger, the standings, and the claims themselves */
    t = now();
    over(pg, npg, gone.id, gone.n, "SELECT path FROM physicality WHERE entity = ANY($1::uuid[])", each_part, &parts, "the claims' parts");
    { const char *v[1] = { wt }; PGresult *r = PQexecParams(pg[0], "DELETE FROM attestation WHERE witness = $1::uuid", 1, NULL, v, NULL, NULL, 0);
      must(pg[0], r, PGRES_COMMAND_OK, "the ledger"); printf("  %-52s %12s   ", "ledger rows", PQcmdTuples(r)); PQclear(r); }
    uint64_t ns = over(pg, npg, gone.id, gone.n, "DELETE FROM standing WHERE claim = ANY($1::uuid[])", NULL, NULL, "standings");
    uint64_t np = over(pg, npg, gone.id, gone.n, "DELETE FROM physicality WHERE entity = ANY($1::uuid[])", NULL, NULL, "claim paths");
    uint64_t ne = over(pg, npg, gone.id, gone.n, "DELETE FROM entity WHERE id = ANY($1::uuid[])", NULL, NULL, "claim entities");
    printf("(%.1f s)\n  %-52s %'12llu\n  %-52s %'12llu entities, %'llu paths\n", now() - t, "standings", (unsigned long long)ns, "claims", (unsigned long long)ne, (unsigned long long)np);
    { const char *v[1] = { wt };
      PGresult *r = PQexecParams(pg[0], "DELETE FROM source WHERE trunk = $1::uuid", 1, NULL, v, NULL, NULL, 0); must(pg[0], r, PGRES_COMMAND_OK, "its source"); PQclear(r);
      r = PQexecParams(pg[0], "DELETE FROM witness WHERE id = $1::uuid", 1, NULL, v, NULL, NULL, 0); must(pg[0], r, PGRES_COMMAND_OK, "the witness"); PQclear(r); }
    set_add(&parts, &wid);

    set_free(&parts); set_free(&claims); set_free(&shared); set_free(&gone);
    printf("\n"); sweep(pg, npg, 0);
    printf("\n== total %.1f s\n", now() - T);
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}
