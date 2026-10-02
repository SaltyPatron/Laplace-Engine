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

typedef struct { lp_id *id; uint64_t n, cap; lp_idmap *m; } Set;                           /* in the order added; membership by ID */
static int set_add(Set *s, const lp_id *id){                              /* 1 if it was not there */
    if (!s->m) s->m = lp_idmap_new(); bool fresh; lp_idmap_put(s->m, id, &fresh); if (!fresh) return 0;
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1 << 16; s->id = xrealloc(s->id, s->cap * sizeof(lp_id)); }
    s->id[s->n++] = *id; return 1;
}
static void set_free(Set *s){ free(s->id); lp_idmap_free(s->m); memset(s, 0, sizeof *s); }

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
        uint32_t k = (uint32_t)(n - i0 < CHUNK ? n - i0 : CHUNK); uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t al = ids_param(ab, ids + i0, k);
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
static uint64_t over_parts(PGconn **pg, int npg, const Parts parts[16], const lp_id *ids, uint64_t n, const char *sql, Each each, void *into, const char *what){
    uint64_t cnt[17] = { 0 }; for (uint64_t i = 0; i < n; i++) cnt[(ids[i].b[0] >> 4) + 1]++;
    for (int h = 0; h < 16; h++) cnt[h + 1] += cnt[h];
    lp_id *by = malloc(sizeof(lp_id) * (n + 1)); uint64_t fill[16]; memcpy(fill, cnt, sizeof fill); for (uint64_t i = 0; i < n; i++) by[fill[ids[i].b[0] >> 4]++] = ids[i];
    typedef struct { int h, p; uint64_t lo, n; } Job; uint64_t nj = 0, cj = 0; Job *job = NULL;
    for (int h = 0; h < 16; h++) for (uint64_t lo = cnt[h]; lo < cnt[h + 1]; lo += CHUNK) for (int p = 0; p < parts[h].n; p++) {
        if (nj == cj) { cj = cj ? cj * 2 : 1024; job = xrealloc(job, cj * sizeof(Job)); }
        job[nj++] = (Job){ h, p, lo, cnt[h + 1] - lo < CHUNK ? cnt[h + 1] - lo : CHUNK }; }
    uint64_t rows = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic) reduction(+:rows)
    for (uint64_t j = 0; j < nj; j++) {
        uint32_t k = (uint32_t)job[j].n; uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t al = ids_param(ab, by + job[j].lo, k);
        char q[512]; snprintf(q, sizeof q, sql, parts[job[j].h].name[job[j].p]);
        PGconn *c = pg[omp_get_thread_num()]; const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGresult *r = PQexecParams(c, q, 1, NULL, v, l, f, 1); must(c, r, each ? PGRES_TUPLES_OK : PGRES_COMMAND_OK, what);
        if (each) {
            #pragma omp critical(forget_rows)
            for (int x = 0; x < PQntuples(r); x++) each(r, x, into);
            rows += (uint64_t)PQntuples(r);
        } else rows += strtoull(PQcmdTuples(r), NULL, 10);
        PQclear(r); free(ab);
    }
    free(by); free(job);
    return rows;
}

/* ---- the sweep */
typedef struct { lp_id id; uint32_t held; uint8_t entity, root, file; } Count;
/* A file, and the content it is a trunk over: a file whose content is what it witnessed is kept by that content. */
typedef struct { lp_id file, content; uint8_t curated; } Kept;
static Kept *kept; static uint64_t nkept, ckept; static pthread_mutex_t kept_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { pthread_mutex_t mu; Count *c; uint64_t n, cap; lp_idmap *m; } CShard;
static CShard cs[256];
static Count *count_of(CShard *s, const lp_id *id, int add){                /* the shard's lock is held */
    if (!add) { int64_t i = lp_idmap_find(s->m, id); return i < 0 ? NULL : &s->c[i]; }
    if (!s->m) s->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(s->m, id, &fresh); if (!fresh) return &s->c[i];
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1 << 14; s->c = xrealloc(s->c, s->cap * sizeof(Count)); }
    Count *c = &s->c[s->n++]; memset(c, 0, sizeof *c); c->id = *id; return c;
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
static void row_path(PGresult *r, Batch *b){
    lp_id e; memcpy(e.b, PQgetvalue(r, 0, 0), 16); batch_put(b, &e, 1);
    const uint8_t *pb = (const uint8_t *)PQgetvalue(r, 0, 1); size_t pl = (size_t)PQgetlength(r, 0, 1), nv = lp_path_vertices(pb, pl, NULL, 0);
    lp_vertex *vt = malloc(sizeof(lp_vertex) * (nv ? nv : 1)); lp_path_vertices(pb, pl, vt, nv);
    int file = 0; lp_id content; uint8_t curated = 0; memset(&content, 0, sizeof content);
    for (size_t i = 0; i < nv; i++) { batch_put(b, &vt[i].id, 0);
        uint32_t said = vt[i].said; if (said == LP_SAID_METADATA) file = 1; else if (file) { content = vt[i].id; curated = said == LP_SAID_RECORD || said == LP_SAID_CLAIM; } }
    free(vt);
    if (file) {                                                                /* a file: a trunk over its metadata and its content */
        batch_put(b, &e, 3);
        pthread_mutex_lock(&kept_mu); if (nkept == ckept) { ckept = ckept ? ckept * 2 : 1024; kept = xrealloc(kept, ckept * sizeof(Kept)); }
        kept[nkept++] = (Kept){ e, content, curated }; pthread_mutex_unlock(&kept_mu);
    }
}
static void row_root(PGresult *r, Batch *b){ for (int f = 0; f < PQnfields(r); f++) if (!PQgetisnull(r, 0, f)) { lp_id id; memcpy(id.b, PQgetvalue(r, 0, f), 16); batch_put(b, &id, 2); } }
/* A curated file's content: whether any of what it witnessed is still witnessed. */
static void each_content(PGresult *r, int j, void *into){
    const uint8_t *pb = (const uint8_t *)PQgetvalue(r, j, 1); size_t pl = (size_t)PQgetlength(r, j, 1), nv = lp_path_vertices(pb, pl, NULL, 0); int still = 0;
    lp_vertex *vt = malloc(sizeof(lp_vertex) * (nv ? nv : 1)); lp_path_vertices(pb, pl, vt, nv);
    for (size_t i = 0; i < nv && !still; i++) { if (!vt[i].said) continue;
        Count *c = count_of(&cs[vt[i].id.b[0]], &vt[i].id, 0); still = c && c->root; }
    free(vt);
    if (still) { lp_id id; memcpy(id.b, PQgetvalue(r, j, 0), 16); set_add(into, &id); }
}
/* What an entity that is going held: each counted down; whatever reaches nothing goes next. */
static void each_release(PGresult *r, int j, void *into){
    const uint8_t *pb = (const uint8_t *)PQgetvalue(r, j, 0); size_t pl = (size_t)PQgetlength(r, j, 0), nv = lp_path_vertices(pb, pl, NULL, 0);
    lp_vertex *vt = malloc(sizeof(lp_vertex) * (nv ? nv : 1)); lp_path_vertices(pb, pl, vt, nv);
    for (size_t i = 0; i < nv; i++) {
        Count *c = count_of(&cs[vt[i].id.b[0]], &vt[i].id, 0); if (!c || !c->held) continue;
        if (!--c->held && c->entity && !c->root) set_add(into, &vt[i].id);
    }
    free(vt);
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
    { Set live = { 0 }, ask = { 0 };
      for (uint64_t i = 0; i < nkept; i++) if (kept[i].curated) { Count *c = count_of(&cs[kept[i].content.b[0]], &kept[i].content, 0); if (c && c->root) set_add(&live, &kept[i].content); else set_add(&ask, &kept[i].content); }
      if (ask.n) over_parts(pg, npg, pparts, ask.id, ask.n, "SELECT entity, path FROM %s WHERE entity = ANY($1::blake3[])", each_content, &live, "what files witnessed");
      for (uint64_t i = 0; i < nkept; i++) { Count *c = count_of(&cs[kept[i].file.b[0]], &kept[i].file, 0); if (!c) continue;
          int stays = 1; if (kept[i].curated) { stays = 0; for (uint64_t k = 0; k < live.n && !stays; k++) stays = !memcmp(&live.id[k], &kept[i].content, 16); }
          if (stays) c->root = 1; }
      set_free(&live); set_free(&ask); free(kept); kept = NULL; nkept = ckept = 0; }
    Set going = { 0 }; uint64_t entities = 0, roots = 0;
    for (int sh = 0; sh < 256; sh++) for (uint64_t i = 0; i < cs[sh].n; i++) {
        Count *c = &cs[sh].c[i]; entities += c->entity; roots += c->root && c->entity;
        if (c->entity && !c->held && !c->root) set_add(&going, &c->id);
    }
    printf("  %-52s %'12llu   of them witnessed, files and witnesses %'llu   (%.1f s)\n", "entities above tier 0", (unsigned long long)entities, (unsigned long long)roots, now() - t);
    uint64_t total = 0; int level = 0;
    while (going.n) {
        t = now(); level++; Set next = { 0 };
        if (level == 1) {                                                   /* what is going, shown before it goes */
            Reader *rd = reader_new(pg[0]); printf("  held by nothing, for example:");
            for (uint64_t i = 0; i < going.n && i < 12; i++) { char *tx = reader_text(rd, &going.id[i * (going.n / 12 ? going.n / 12 : 1) % going.n], 40); printf("%s \"%s\"", i ? "," : "", tx); free(tx); }
            printf("\n"); reader_free(rd);
        }
        over_parts(pg, npg, pparts, going.id, going.n, "SELECT path FROM %s WHERE entity = ANY($1::blake3[])", each_release, &next, "what they held");
        if (!dry) { over(pg, npg, going.id, going.n, "DELETE FROM consensus WHERE claim = ANY($1::blake3[])", NULL, NULL, "the consensus on them");
                    over_parts(pg, npg, pparts, going.id, going.n, "DELETE FROM %s WHERE entity = ANY($1::blake3[])", NULL, NULL, "paths");
                    over_parts(pg, npg, eparts, going.id, going.n, "DELETE FROM %s WHERE id = ANY($1::blake3[])", NULL, NULL, "entities"); }
        total += going.n;
        printf("  level %d: %'12llu held by nothing%s   (%.1f s)\n", level, (unsigned long long)going.n, dry ? "" : ", removed", now() - t); fflush(stdout);
        set_free(&going); going = next;
    }
    printf("  %-52s %'12llu%s   (%.1f s)\n", "entities nothing held", (unsigned long long)total, dry ? "   (dry: nothing was removed)" : "", now() - T);
    for (int sh = 0; sh < 256; sh++) { free(cs[sh].c); lp_idmap_free(cs[sh].m); memset(&cs[sh], 0, sizeof cs[sh]); }
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
    const char *conninfo = laplace_db(); int npg = 0, a = 1, except = 0;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-j") && a + 1 < argc) npg = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--except")) except = 1;
        else break;
    }
    if (a >= argc && !except) { fprintf(stderr, "usage: laplace forget [-d conninfo] [-j connections] witness...\n       laplace forget --except witness...   (every witness but these)\n"); return 2; }
    if (npg <= 0) npg = omp_get_num_procs();
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    double T = now(), t; tier0_open(NULL); Ctx *c = lp_text_new(T0);
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg); for (int i = 0; i < npg; i++) pg[i] = db_connect(conninfo);
    Set named = { 0 }, going = { 0 };
    for (int i = a; i < argc; i++) { lp_id id = lp_text_decompose(c, (const uint8_t *)argv[i], strlen(argv[i]), NULL, NULL).id; set_add(&named, &id); }
    if (!except) going = named;
    else {                                                                  /* every witness but the ones named */
        PGresult *r = PQexecParams(pg[0], "SELECT id FROM witness", 0, NULL, NULL, NULL, NULL, 1); must(pg[0], r, PGRES_TUPLES_OK, "witnesses");
        for (int j = 0; j < PQntuples(r); j++) { lp_id id; memcpy(id.b, PQgetvalue(r, j, 0), 16); int keep = 0;
            for (uint64_t k = 0; k < named.n; k++) keep |= !memcmp(&named.id[k], &id, 16);
            if (!keep) set_add(&going, &id); }
        PQclear(r);
    }
    printf("laplace forget   %s   %llu witness%s\n", PQdb(pg[0]), (unsigned long long)going.n, going.n == 1 ? "" : "es");
    if (!going.n) { printf("  nothing to forget\n"); return 1; }
    { Reader *rd = reader_new(pg[0]); for (uint64_t i = 0; i < going.n; i++) { char *tx = reader_text(rd, &going.id[i], 80); printf("  %s\n", tx); free(tx); } reader_free(rd); }

    /* what they witnessed leaves the ledger; whatever nothing holds or witnesses any more goes with the sweep */
    t = now();
    uint64_t nl = over(pg, 1, going.id, going.n, "DELETE FROM attestation WHERE witness = ANY($1::blake3[])", NULL, NULL, "the ledger");
    over(pg, 1, going.id, going.n, "DELETE FROM witness WHERE id = ANY($1::blake3[])", NULL, NULL, "the witnesses");
    printf("  %-52s %'12llu   (%.1f s)\n\n", "ledger rows", (unsigned long long)nl, now() - t); fflush(stdout);
    sweep(pg, npg, 0);
    printf("\n== total %.1f s\n", now() - T);
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}
