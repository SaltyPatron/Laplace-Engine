/* laplace forget: what witnesses attested, taken back out.
 *   laplace forget [-d conninfo] [-j connections] witness...
 *   laplace forget --except witness...        every witness but these
 * Their rows leave the ledger. What no other witness witnessed goes, with the consensus on it; what others witnessed
 * stays. Then, level by level down the DAG, whatever nothing holds any more goes too: an entity stays while any path
 * holds it, or while it is a file, a witness, or witnessed. A file whose content is what it witnessed stays while any
 * of that is still witnessed. Atoms always stay.
 * SQL fetches and deletes sets of rows; what to delete is decided here.
 *
 * laplace sweep: whatever nothing holds, removed.
 *   laplace sweep [-d conninfo] [-j connections] [--dry]
 * One pass over every path counts, for every entity, the places that hold it. An entity no path holds goes, unless it is
 * a file, a witness, or something the ledger says was witnessed; the consensus on it, if there is one, goes with it.
 * What it held is counted down, and goes in turn when its count reaches nothing. */
#include "engine.h"
#include <locale.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A set of IDs is an ID map with nothing beside each key: its keys, in the order added, are the set as an array. */
typedef lp_idmap Set;
static Set *set_new(void){ return lp_idmap_sized(0); }
static void set_add(Set *s, const lp_id *id){ lp_idmap_put(s, id, NULL); }
static bool set_has(const Set *s, const lp_id *id){ return lp_idmap_find(s, id) >= 0; }

static void must(PGconn *pg, PGresult *r, ExecStatusType want, const char *what){
    if (PQresultStatus(r) != want) { fprintf(stderr, "%s: %s", what, PQerrorMessage(pg)); exit(1); }
}
#define CHUNK 50000
/* One statement over a set of IDs, in chunks, on every connection at once (over_ids). Rows of a fetch go to `each`. */
static uint64_t over(PGconn **pg, int npg, const lp_id *ids, uint64_t n, const char *sql, Each each, void *into, const char *what){
    (void)what; Groups one = whole(); return over_ids(pg, npg, ids, n, &one, CHUNK, sql, each, into);
}
/* The partitions of a table that hold anything, by the first hex digit of the IDs they hold. An ID is looked for, or
 * removed, in the partitions its first digit names, by name: never through the partitioned table, which would probe
 * every partition of every tier for every ID. */
typedef struct { char name[32][40]; int n; } Parts;
static void parts_of(PGconn *pg, const char *table, Parts out[16]){
    char q[256]; snprintf(q, sizeof q, "SELECT c.relname FROM pg_class c WHERE c.relkind = 'r' AND c.relname ~ '^%s_t([0-9]+|x)(_[0-9a-f])?$' ORDER BY 1", table);
    PGresult *r = PQexec(pg, q); must(pg, r, PGRES_TUPLES_OK, "partitions"); memset(out, 0, sizeof(Parts) * 16);
    for (int j = 0; j < PQntuples(r); j++) {
        const char *name = PQgetvalue(r, j, 0); snprintf(q, sizeof q, "SELECT 1 FROM %s LIMIT 1", name);
        PGresult *e = PQexec(pg, q); int holds = PQresultStatus(e) == PGRES_TUPLES_OK && PQntuples(e) > 0; PQclear(e); if (!holds) continue;
        size_t nl = strlen(name); int whole = !(nl > 2 && name[nl - 2] == '_');        /* a tier that is one partition holds IDs of every first digit */
        char hx = name[nl - 1]; int only = hx <= '9' ? hx - '0' : hx - 'a' + 10;
        for (int h = whole ? 0 : only; h < (whole ? 16 : only + 1); h++) if (out[h].n < 32) snprintf(out[h].name[out[h].n++], 40, "%s", name);
    }
    PQclear(r);
}
/* One statement per partition over the IDs that partition could hold; %s in the statement is the partition. */
static int parts_targets(int g, void *parts){ return ((const Parts *)parts)[g].n; }
static void parts_target(int g, int t, char *out, size_t cap, void *parts){ snprintf(out, cap, "%s", ((const Parts *)parts)[g].name[t]); }
static int digit(const lp_id *ids, uint64_t i, void *ctx){ (void)ctx; return ids[i].b[0] >> 4; }
static uint64_t over_parts(PGconn **pg, int npg, const Parts parts[16], const lp_id *ids, uint64_t n, const char *sql, Each each, void *into, const char *what){
    (void)what; Groups by = { 16, digit, parts_targets, parts_target, (void *)parts }; return over_ids(pg, npg, ids, n, &by, CHUNK, sql, each, into);
}

/* ---- the sweep */
typedef struct { lp_id id; uint32_t held; uint8_t entity, root, file; } Count;
/* A file, and the content it is a trunk over: a file whose content is what it witnessed is kept by that content. */
typedef struct { lp_id file, content; uint8_t curated; } Kept;
static lp_vec(Kept) kept; static pthread_mutex_t kept_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { pthread_mutex_t mu; lp_idmap *m; } CShard;                /* the counts, by ID: each a Count */
static CShard cs[256];
static Count *count_of(CShard *s, const lp_id *id, int add){                /* the shard's lock is held */
    if (!add) return s->m ? lp_idmap_lookup(s->m, id) : NULL;
    if (!s->m) s->m = lp_idmap_sized(sizeof(Count)); bool fresh; Count *c = lp_idmap_get(s->m, id, &fresh); if (fresh) c->id = *id; return c;
}
/* A thread's pending counts, by shard, applied under that shard's lock a batch at a time. what: 0 held once more,
 * 1 it is an entity, 2 it is a root, 3 it is a file. */
typedef struct { lp_id id; uint8_t what; } Pend;
typedef struct { Pend *p[256]; uint32_t n[256]; } Batch;
#define BATCH 2048
static void batch_flush(Batch *b, int sh){
    CShard *s = &cs[sh]; pthread_mutex_lock(&s->mu);
    for (uint32_t i = 0; i < b->n[sh]; i++) { Count *c = count_of(s, &b->p[sh][i].id, 1); uint8_t w = b->p[sh][i].what; if (w == 0) c->held++; else if (w == 1) c->entity = 1; else if (w == 3) c->file = 1; else c->root = 1; }
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
/* A path's vertices, decoded into the thread's own array: kept and refilled row after row. */
static __thread lp_vec(lp_vertex) vx;
static lp_vertex *vertices(lp_path p){ lp_vec_reserve(&vx, p.n ? p.n : 1); lp_path_decode(p, vx.v, p.n); return vx.v; }
static void row_path(PGresult *r, Batch *b){
    lp_id e = *col_id(r, 0, 0); batch_put(b, &e, 1);
    lp_path p = col_path(r, 0, 1); lp_vertex *vt = vertices(p);
    int file = 0; lp_id content; uint8_t curated = 0; memset(&content, 0, sizeof content);
    for (size_t i = 0; i < p.n; i++) { batch_put(b, &vt[i].id, 0);
        uint32_t said = vt[i].said; if (said == LP_SAID_METADATA) file = 1; else if (file) { content = vt[i].id; curated = said == LP_SAID_RECORD || said == LP_SAID_CLAIM; } }
    if (file) {                                                                /* a file: a trunk over its metadata and its content */
        batch_put(b, &e, 3);
        pthread_mutex_lock(&kept_mu); lp_push(&kept, (Kept){ e, content, curated }); pthread_mutex_unlock(&kept_mu);
    }
}
static void row_root(PGresult *r, Batch *b){ for (int f = 0; f < PQnfields(r); f++) if (!PQgetisnull(r, 0, f)) batch_put(b, col_id(r, 0, f), 2); }
/* A curated file's content: whether any of what it witnessed is still witnessed. */
static void each_content(const PGresult *r, int j, const uint64_t *place, void *into){
    (void)place;
    lp_path p = col_path(r, j, 1); lp_vertex *vt = vertices(p); int still = 0;
    for (size_t i = 0; i < p.n && !still; i++) { if (!vt[i].said) continue;
        Count *c = count_of(&cs[vt[i].id.b[0]], &vt[i].id, 0); still = c && c->root; }
    if (still) set_add(into, col_id(r, j, 0));
}
/* What an entity that is going held: each counted down; whatever reaches nothing goes next. */
static void each_release(const PGresult *r, int j, const uint64_t *place, void *into){
    (void)place;
    lp_path p = col_path(r, j, 0); lp_vertex *vt = vertices(p);
    for (size_t i = 0; i < p.n; i++) {
        Count *c = count_of(&cs[vt[i].id.b[0]], &vt[i].id, 0); if (!c || !c->held) continue;
        if (!--c->held && c->entity && !c->root) set_add(into, &vt[i].id);
    }
}
static uint64_t sweep(PGconn **pg, int npg, int dry){
    double T = now(), t = now(); for (int i = 0; i < 256; i++) pthread_mutex_init(&cs[i].mu, NULL);
    static Parts eparts[16], pparts[16]; parts_of(pg[0], "entity", eparts); parts_of(pg[0], "physicality", pparts);
    /* every partition of paths above tier 0, each on a connection */
    PGresult *r = PQexec(pg[0], "SELECT c.relname FROM pg_class c JOIN pg_inherits i ON i.inhrelid = c.oid WHERE c.relkind = 'r' "
                                "AND c.relname ~ '^physicality_t([1-9][0-9]*|x)(_[0-9a-f])?$' ORDER BY c.reltuples DESC");
    must(pg[0], r, PGRES_TUPLES_OK, "partitions"); int nparts = PQntuples(r); uint64_t paths = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic) reduction(+:paths)
    for (int p = 0; p < nparts; p++) {
        Batch b = { 0 }; char sql[160]; snprintf(sql, sizeof sql, "SELECT entity, path FROM %s", PQgetvalue(r, p, 0));
        paths += stream(pg[omp_get_thread_num()], sql, row_path, &b); batch_done(&b);
    }
    PQclear(r);
    printf("  %-52s %'12llu   (%.1f s)\n", "paths read, every holder counted", (unsigned long long)paths, now() - t); fflush(stdout);
    t = now(); { Batch b = { 0 };
      stream(pg[0], "SELECT claim FROM attestation", row_root, &b);
      stream(pg[0], "SELECT id, lineage FROM witness", row_root, &b); batch_done(&b); }
    /* files: one whose content is its own stays; one whose content is what it witnessed stays while any of that is
     * witnessed still */
    { Set *live = set_new(), *ask = set_new();
      for (size_t i = 0; i < kept.n; i++) if (kept.v[i].curated) { Count *c = count_of(&cs[kept.v[i].content.b[0]], &kept.v[i].content, 0); set_add(c && c->root ? live : ask, &kept.v[i].content); }
      if (lp_idmap_count(ask)) over_parts(pg, npg, pparts, lp_idmap_keys(ask), lp_idmap_count(ask), "SELECT entity, path FROM %s WHERE entity = ANY($1::blake3[])", each_content, live, "what files witnessed");
      for (size_t i = 0; i < kept.n; i++) { Count *c = count_of(&cs[kept.v[i].file.b[0]], &kept.v[i].file, 0); if (!c) continue;
          if (!kept.v[i].curated || set_has(live, &kept.v[i].content)) c->root = 1; }
      lp_idmap_free(live); lp_idmap_free(ask); lp_vec_free(&kept); }
    Set *going = set_new(); uint64_t entities = 0, roots = 0;
    for (int sh = 0; sh < 256; sh++) for (size_t i = 0; i < lp_idmap_count(cs[sh].m); i++) {
        Count *c = lp_idmap_at(cs[sh].m, i); entities += c->entity; roots += c->root && c->entity;
        if (c->entity && !c->held && !c->root) set_add(going, &c->id);
    }
    printf("  %-52s %'12llu   of them witnessed, files and witnesses %'llu   (%.1f s)\n", "entities above tier 0", (unsigned long long)entities, (unsigned long long)roots, now() - t);
    uint64_t total = 0; int level = 0;
    for (uint64_t n; (n = lp_idmap_count(going)); ) {
        t = now(); level++; Set *next = set_new(); const lp_id *ids = lp_idmap_keys(going);
        if (level == 1) {                                                   /* what is going, shown before it goes */
            Reader *rd = reader_new(pg[0]); printf("  held by nothing, for example:");
            for (uint64_t i = 0; i < n && i < 12; i++) { char *tx = reader_text(rd, &ids[i * (n / 12 ? n / 12 : 1) % n], 40); printf("%s \"%s\"", i ? "," : "", tx); free(tx); }
            printf("\n"); reader_free(rd);
        }
        over_parts(pg, npg, pparts, ids, n, "SELECT path FROM %s WHERE entity = ANY($1::blake3[])", each_release, next, "what they held");
        if (!dry) { over(pg, npg, ids, n, "DELETE FROM consensus WHERE claim = ANY($1::blake3[])", NULL, NULL, "the consensus on them");
                    over_parts(pg, npg, pparts, ids, n, "DELETE FROM %s WHERE entity = ANY($1::blake3[])", NULL, NULL, "paths");
                    over_parts(pg, npg, eparts, ids, n, "DELETE FROM %s WHERE id = ANY($1::blake3[])", NULL, NULL, "entities"); }
        total += n;
        printf("  level %d: %'12llu held by nothing%s   (%.1f s)\n", level, (unsigned long long)n, dry ? "" : ", removed", now() - t); fflush(stdout);
        lp_idmap_free(going); going = next;
    }
    lp_idmap_free(going);
    printf("  %-52s %'12llu%s   (%.1f s)\n", "entities nothing held", (unsigned long long)total, dry ? "   (dry: nothing was removed)" : "", now() - T);
    for (int sh = 0; sh < 256; sh++) { lp_idmap_free(cs[sh].m); cs[sh].m = NULL; }
    return total;
}
int cmd_sweep(int argc, char **argv){
    const char *conninfo = laplace_db(); int npg = 0, dry = 0;
    if (opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "-j", 'i', &npg }, { "--dry", 'b', &dry }, { NULL } }) < argc) {
        fprintf(stderr, "usage: laplace sweep [-d conninfo] [-j connections] [--dry]\n"); return 2; }
    if (npg <= 0) npg = omp_get_num_procs();
    setlocale(LC_NUMERIC, "en_US.UTF-8"); tier0_open(NULL);
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg); for (int i = 0; i < npg; i++) pg[i] = db_connect(conninfo);
    printf("laplace sweep   %s%s\n", PQdb(pg[0]), dry ? "   (dry)" : "");
    sweep(pg, npg, dry);
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}

int cmd_forget(int argc, char **argv){
    const char *conninfo = laplace_db(); int npg = 0, except = 0;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "-j", 'i', &npg }, { "--except", 'b', &except }, { NULL } });
    if (a >= argc && !except) { fprintf(stderr, "usage: laplace forget [-d conninfo] [-j connections] witness...\n       laplace forget --except witness...   (every witness but these)\n"); return 2; }
    if (npg <= 0) npg = omp_get_num_procs();
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    double T = now(), t; tier0_open(NULL); Ctx *c = lp_text_new(T0);
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg); for (int i = 0; i < npg; i++) pg[i] = db_connect(conninfo);
    Set *named = set_new(), *going = named;
    for (int i = a; i < argc; i++) { lp_id id = lp_text_decompose(c, (const uint8_t *)argv[i], strlen(argv[i]), NULL, NULL).id; set_add(named, &id); }
    if (except) {                                                           /* every witness but the ones named */
        going = set_new(); PGresult *r = ask_once(pg[0], "SELECT id FROM witness", NULL);
        for (int j = 0; j < PQntuples(r); j++) if (!set_has(named, col_id(r, j, 0))) set_add(going, col_id(r, j, 0));
        PQclear(r);
    }
    uint64_t ng = lp_idmap_count(going); const lp_id *gid = lp_idmap_keys(going);
    printf("laplace forget   %s   %llu witness%s\n", PQdb(pg[0]), (unsigned long long)ng, ng == 1 ? "" : "es");
    if (!ng) { printf("  nothing to forget\n"); return 1; }
    { Reader *rd = reader_new(pg[0]); for (uint64_t i = 0; i < ng; i++) { char *tx = reader_text(rd, &gid[i], 80); printf("  %s\n", tx); free(tx); } reader_free(rd); }

    /* what they witnessed leaves the ledger; whatever nothing holds or witnesses any more goes with the sweep */
    t = now();
    uint64_t nl = over(pg, 1, gid, ng, "DELETE FROM attestation WHERE witness = ANY($1::blake3[])", NULL, NULL, "the ledger");
    over(pg, 1, gid, ng, "DELETE FROM witness WHERE id = ANY($1::blake3[])", NULL, NULL, "the witnesses");
    printf("  %-52s %'12llu   (%.1f s)\n\n", "ledger rows", (unsigned long long)nl, now() - t); fflush(stdout);
    sweep(pg, npg, 0);
    printf("\n== total %.1f s\n", now() - T);
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}
