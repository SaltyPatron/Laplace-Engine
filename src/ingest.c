#define _GNU_SOURCE
/* laplace ingest: files through their recipes.
 *   laplace ingest [-d conninfo] [-t tier0.bin] [-r recipes/] [-j threads] [--no-load] [--plan] file...
 * Files already recorded byte for byte are skipped by one query over their BLAKE3-256 hashes. The rest decompose on
 * every core; each is recomposed from the node table and compared with its bytes; then new nodes, claims and
 * standings are written. Live counters go to stderr, phase times to stdout. */
#include "engine.h"
#include "blake3.h"
#include <arpa/inet.h>
#include <locale.h>
#include <ftw.h>
#include <sys/stat.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- recomposition: an entity back to its bytes, from the node table and tier 0 */
typedef struct { uint8_t *b; size_t n, cap; } Buf;
static int expand(const lp_id *id, Buf *o){
    int64_t cp = lp_tier0_codepoint(T0, id);
    if (cp >= 0) {
        if (o->n + 4 > o->cap) { o->cap = (o->n + 4) * 2; o->b = xrealloc(o->b, o->cap); }
        o->n += lp_utf8_put((uint32_t)cp, o->b + o->n);
        return 1;
    }
    Node *x = table_find(id); if (!x) return 0;
    Shard *s = &shard[id->b[0]];
    for (uint32_t v = 0; v < x->nv; v++) for (uint32_t r = 0; r < s->vtx[x->voff + v].run; r++) if (!expand(&s->vtx[x->voff + v].id, o)) return 0;
    return 1;
}

/* Directories are walked for every file under them. */
static char **paths; static int npaths, cpaths;
static void add_path(const char *p){ if (npaths == cpaths) { cpaths = cpaths ? cpaths * 2 : 1024; paths = xrealloc(paths, sizeof(char *) * cpaths); } paths[npaths++] = strdup(p); }
static int walk_cb(const char *p, const struct stat *st, int type, struct FTW *fw){
    const char *b = p + fw->base;
    if (type == FTW_D && b[0] == '.' && fw->level > 0) return FTW_SKIP_SUBTREE;          /* hidden directories */
    if (type == FTW_F && b[0] != '.' && st->st_size > 0) add_path(p);
    return FTW_CONTINUE;
}

int cmd_ingest(int argc, char **argv){
    const char *conninfo = laplace_db(), *t0p = NULL, *rdir = laplace_recipes();
    int threads = 0, do_load = 1, a = 1, show_claims = 0;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) t0p = argv[++a];
        else if (!strcmp(argv[a], "-r") && a + 1 < argc) rdir = argv[++a];
        else if (!strcmp(argv[a], "-j") && a + 1 < argc) threads = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--no-load")) do_load = 0;
        else if (!strcmp(argv[a], "--plan")) do_load = -1;
        else if (!strcmp(argv[a], "--claims")) { show_claims = 1; do_load = 0; }     /* what the recipes attest, as text; nothing is loaded */
        else { fprintf(stderr, "usage: laplace ingest [-d conninfo] [-t tier0.bin] [-r recipes] [-j threads] [--no-load] [--plan] [--claims] file...\n"); return 2; }
    }
    for (int i = a; i < argc; i++) { struct stat st; if (!stat(argv[i], &st) && S_ISDIR(st.st_mode)) nftw(argv[i], walk_cb, 64, FTW_PHYS | FTW_ACTIONRETVAL); else add_path(argv[i]); }
    int nfiles = npaths; if (nfiles <= 0) { fprintf(stderr, "no files\n"); return 2; }
    if (threads <= 0) threads = omp_get_num_procs();
    omp_set_num_threads(threads); omp_set_max_active_levels(1);
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    double T = now(), t;

    tier0_open(t0p);
    table_init();
    Recipe *rec = NULL; int nrec = recipes_load(rdir, &rec);
    printf("laplace ingest   %d threads   cpu: %s   dispatch: %s   %d recipes\n", threads, lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()), nrec);

    File *files = calloc(nfiles, sizeof(File));
    int uncovered = 0;
    for (int i = 0; i < nfiles; i++) { files[i].path = paths[i]; files[i].recipe = recipe_for(rec, nrec, files[i].path); if (!files[i].recipe) { files[i].skipped = 1; uncovered++; } }
    if (uncovered) {                                                                  /* what no recipe covers yet, by extension */
        typedef struct { char ext[16]; int n; } Ext; Ext ex[512]; int ne = 0;
        for (int i = 0; i < nfiles; i++) if (!files[i].recipe) {
            const char *d = strrchr(files[i].path, '.'), *sl = strrchr(files[i].path, '/'); char e[16] = "(none)";
            if (d && (!sl || d > sl) && strlen(d) < sizeof e) snprintf(e, sizeof e, "%s", d);
            int k = 0; while (k < ne && strcmp(ex[k].ext, e)) k++; if (k == ne && ne < 512) { snprintf(ex[ne].ext, 16, "%s", e); ex[ne++].n = 0; } if (k < ne) ex[k].n++;
        }
        for (int x = 0; x < ne; x++) for (int y = x + 1; y < ne; y++) if (ex[y].n > ex[x].n) { Ext tmp = ex[x]; ex[x] = ex[y]; ex[y] = tmp; }
        printf("files no recipe covers yet: %d of %d:", uncovered, nfiles);
        for (int k = 0; k < ne && k < 40; k++) printf(" %s %d", ex[k].ext, ex[k].n); printf("\n");
    }
    if (do_load < 0) {                                                                /* the plan: which recipe takes what */
        for (int k = 0; k < nrec; k++) { int n = 0; for (int i = 0; i < nfiles; i++) n += files[i].recipe == &rec[k]; if (n) printf("  %-24s %d files\n", rec[k].name, n); }
        return 0;
    }

    /* ---- files already recorded, by their bytes */
    t = now();
    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < nfiles; i++) {
        FILE *f = fopen(files[i].path, "rb"); if (!f) { files[i].skipped = 1; continue; }
        blake3_hasher h; blake3_hasher_init(&h); uint8_t buf[1 << 16]; size_t k;
        while ((k = fread(buf, 1, sizeof buf, f))) blake3_hasher_update(&h, buf, k);
        fclose(f); blake3_hasher_finalize(&h, files[i].sha, 32);
    }
    int nknown = 0;
    if (do_load) {
        PGconn *pg = db_connect(conninfo);
        uint8_t *ab = malloc(20 + 36 * (size_t)nfiles), *q = ab + 20;
        for (int i = 0; i < nfiles; i++) { uint32_t l = htonl(32); memcpy(q, &l, 4); memcpy(q + 4, files[i].sha, 32); q += 36; }
        uint32_t hdr[5] = { htonl(1), htonl(0), htonl(17), htonl((uint32_t)nfiles), htonl(1) }; memcpy(ab, hdr, 20);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)(q - ab) }, fm[1] = { 1 };
        PGresult *r = PQexecParams(pg, "SELECT content FROM source WHERE content = ANY($1::bytea[])", 1, NULL, v, l, fm, 1);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "%s", PQerrorMessage(pg)); return 1; }
        for (int j = 0; j < PQntuples(r); j++) for (int i = 0; i < nfiles; i++) if (!files[i].known && !memcmp(files[i].sha, PQgetvalue(r, j, 0), 32)) { files[i].known = 1; nknown++; }
        PQclear(r); PQfinish(pg); free(ab);
    }
    printf("files already recorded byte for byte: %d of %d (%.1f ms)\n", nknown, nfiles, (now() - t) * 1000);

    /* ---- decompose: files are tasks; large files split their syntax trees into further tasks */
    t = now(); ctx_open(threads);
    uint64_t bytes = 0; int done = 0;
    #pragma omp parallel
    #pragma omp single
    for (int i = 0; i < nfiles; i++) {
        if (files[i].known || files[i].skipped) continue;
        #pragma omp task firstprivate(i)
        {
            decompose_file(CTX[omp_get_thread_num()], &files[i]);
            #pragma omp critical
            {
                bytes += files[i].bytes; done++;
                fprintf(stderr, "\r  %d/%d files  %.1f MB  %.1f MB/s  %'llu nodes   ", done, nfiles - nknown, bytes / 1e6, bytes / 1e6 / (now() - t), (unsigned long long)table_count());
            }
        }
    }
    fputc('\n', stderr);
    double t_dec = now() - t;

    /* ---- every file recomposed from the node table and compared with its bytes */
    t = now(); int exact = 0, mism = 0; uint64_t nev = 0;
    #pragma omp parallel for schedule(dynamic) reduction(+:exact, mism)
    for (int i = 0; i < nfiles; i++) {
        if (files[i].known || files[i].skipped) continue;
        if (files[i].recipe && files[i].recipe->query) { exact++; continue; }      /* a curated source is not kept as a file */
        if (files[i].tokens) { exact++; continue; }                                  /* a vocabulary is the text its tokens stand for */
        FILE *f = fopen(files[i].path, "rb"); uint8_t *src = malloc(files[i].bytes + 1);
        size_t got = fread(src, 1, files[i].bytes, f); fclose(f);
        Buf o = { 0 }; int ok = expand(&files[i].trunk.id, &o) && got == files[i].bytes && o.n == got && !memcmp(o.b, src, got);
        if (ok) exact++; else { mism++; fprintf(stderr, "  %s: does not recompose\n", files[i].path); }
        if (getenv("LAPLACE_TRUNKS")) { const uint8_t *b = files[i].trunk.id.b; fprintf(stderr, "  trunk %02x%02x%02x%02x%02x%02x%02x%02x  %s\n", b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7], files[i].path); }
        free(o.b); free(src);
    }
    for (int i = 0; i < nfiles; i++) nev += files[i].ev.n;
    double t_rec = now() - t;
    if (show_claims) for (int i = 0; i < nfiles; i++) for (uint64_t e = 0; e < files[i].ev.n; e++) {
        Node *c = table_find(&files[i].ev.e[e].claim); if (!c) continue;
        Shard *sh = &shard[c->id.b[0]]; int first = 1; putchar('[');
        for (uint32_t v = 0; v < c->nv; v++) for (uint32_t r = 0; r < sh->vtx[c->voff + v].run; r++) {
            Buf o = { 0 }; expand(&sh->vtx[c->voff + v].id, &o);
            if (!first) printf(", "); first = 0; fwrite(o.b, 1, o.n, stdout); free(o.b);
        }
        printf("]\n");
    }
    uint64_t hits = 0; for (int s = 0; s < NSHARD; s++) hits += shard[s].hits;
    printf("\n== decomposition: %d files, %.1f MB, content recomposed byte for byte or curated %d, mismatched %d\n", nfiles - nknown, bytes / 1e6, exact, mism);
    printf("   %'llu distinct compositions, %'llu reused; %'llu attestations\n", (unsigned long long)table_count(), (unsigned long long)hits, (unsigned long long)nev);
    { uint64_t inc = 0; for (int i = 0; i < nfiles; i++) inc += files[i].incomplete;
      if (inc) { printf("   %'llu units held more partial matches than a query keeps: their attestations are incomplete (see above)\n", (unsigned long long)inc); mism++; } }
    printf("\n== phases\n");
    printf("  %-44s %8.2f s   %8.1f MB/s\n", "decompose (all threads)", t_dec, bytes / 1e6 / t_dec);
    printf("  %-44s %8.2f s\n", "recompose and compare", t_rec);
    if (!do_load) return mism ? 1 : 0;

    LoadStats st = { 0 };
    if (load(conninfo, threads, files, nfiles, &st)) return 1;
    printf("  %-44s %8.2f s   %'llu IDs checked in %llu rounds, %'llu subtrees already recorded\n", "deduplication, trunk to leaf", st.t_dedup,
           (unsigned long long)st.checked, (unsigned long long)st.rounds, (unsigned long long)st.found);
    printf("  %-44s %8.2f s   %'llu entities, %'llu paths (%'.0f rows/s)\n", "COPY into every partition", st.t_copy,
           (unsigned long long)st.ent_rows, (unsigned long long)st.phy_rows, (st.ent_rows + st.phy_rows) / (st.t_copy > 0 ? st.t_copy : 1));
    if (nev) printf("  %-44s %8.2f s   %'llu attestations; standings %'llu new, %'llu updated\n", "witnesses, ledger, standings", st.t_sem,
                    (unsigned long long)st.led, (unsigned long long)st.std_new, (unsigned long long)st.std_upd);
    printf("\n== total %.1f s\n", now() - T);
    return mism ? 1 : 0;
}
