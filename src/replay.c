/* laplace replay: every standing, played again from the ledger.
 *   laplace replay [-d conninfo] [-j connections] [--dry]
 * The ledger is read in the order it was written. What a row holds is what was witnessed: a claim, or a record, whose
 * claims are the vertices its path (and the paths of the records inside it) mark as claims. What one lineage witnessed
 * before plays once. A claim entering for the first time enters at its stock default; after that every record that
 * comes in plays the one recorded. Standings are a fold over the ledger, so taking a witness's rows out and playing
 * the rest again gives exactly the standings that would have been without it. */
#define _GNU_SOURCE
#include "engine.h"
#include <locale.h>
#include <omp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { lp_id id; lp_rating r; uint32_t matches; uint8_t entered; } St;
static St *st; static uint32_t *slot; static uint64_t ns, cs, scap;
static uint64_t key(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 5, 8); return k; }
static void grow(void){
    free(slot); scap = scap ? scap * 2 : 1u << 22; slot = calloc(scap, 4);
    for (uint64_t i = 0; i < ns; i++) { uint64_t k = key(&st[i].id) & (scap - 1); while (slot[k]) k = (k + 1) & (scap - 1); slot[k] = (uint32_t)i + 1; }
}
static St *find(const lp_id *id, int add){
    if (add && (ns + 1) * 2 > scap) grow();
    if (!scap) return NULL;
    uint64_t k = key(id) & (scap - 1);
    while (slot[k]) { if (!memcmp(&st[slot[k] - 1].id, id, 16)) return &st[slot[k] - 1]; k = (k + 1) & (scap - 1); }
    if (!add) return NULL;
    if (ns == cs) { cs = cs ? cs * 2 : 1u << 20; st = xrealloc(st, cs * sizeof(St)); }
    memset(&st[ns], 0, sizeof(St)); st[ns].id = *id; slot[k] = (uint32_t)++ns; return &st[ns - 1];
}
typedef struct { lp_id id, lin; double trust; } Wit;
typedef struct { lp_id w, lin; uint8_t used; } Seen;

static void must(PGconn *pg, PGresult *r, ExecStatusType want, const char *what){ if (PQresultStatus(r) != want) { fprintf(stderr, "%s: %s", what, PQerrorMessage(pg)); exit(1); } }
/* The paths of these entities, from the partitions their IDs say they could be in, and only those that hold anything. */
static char *path_sql[16];
static void path_plan(PGconn *pg){
    PGresult *r = PQexec(pg, "SELECT c.relname FROM pg_class c WHERE c.relkind = 'r' AND c.relname ~ '^physicality_t([1-9][0-9]*|x)_[0-9a-f]$' ORDER BY 1");
    must(pg, r, PGRES_TUPLES_OK, "partitions"); size_t cap[16] = { 0 }, len[16] = { 0 };
    for (int j = 0; j < PQntuples(r); j++) {
        const char *name = PQgetvalue(r, j, 0); char q[160]; snprintf(q, sizeof q, "SELECT 1 FROM %s LIMIT 1", name);
        PGresult *e = PQexec(pg, q); int holds = PQresultStatus(e) == PGRES_TUPLES_OK && PQntuples(e) > 0; PQclear(e); if (!holds) continue;
        char hx = name[strlen(name) - 1]; int h = hx <= '9' ? hx - '0' : hx - 'a' + 10;
        if (len[h] + 256 > cap[h]) { cap[h] = cap[h] * 2 + 4096; path_sql[h] = xrealloc(path_sql[h], cap[h]); }
        len[h] += (size_t)snprintf(path_sql[h] + len[h], cap[h] - len[h], "%sSELECT entity, path FROM %s WHERE entity = ANY($1::uuid[])", len[h] ? " UNION ALL " : "", name);
    }
    PQclear(r);
}
typedef struct { lp_id *id; uint32_t *root; uint64_t n, cap; } Front;
static void front_push(Front *f, const lp_id *id, uint32_t root){ if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 1u << 16; f->id = xrealloc(f->id, f->cap * sizeof(lp_id)); f->root = xrealloc(f->root, f->cap * 4); } f->id[f->n] = *id; f->root[f->n++] = root; }
typedef struct { uint32_t root, claim; } Member;

int cmd_replay(int argc, char **argv){
    const char *conninfo = laplace_db(); int npg = 0, dry = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-j") && a + 1 < argc) npg = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--dry")) dry = 1;
        else { fprintf(stderr, "usage: laplace replay [-d conninfo] [-j connections] [--dry]\n"); return 2; }
    }
    if (npg <= 0) npg = omp_get_num_procs();
    setlocale(LC_NUMERIC, "en_US.UTF-8"); double T = now(), t = now(); tier0_open(NULL);
    PGconn **pg = malloc(sizeof(PGconn *) * (size_t)npg); for (int i = 0; i < npg; i++) pg[i] = db_connect(conninfo);
    printf("laplace replay   %s%s\n", PQdb(pg[0]), dry ? "   (dry: standings are not written)" : "");
    path_plan(pg[0]);

    Wit *wit = NULL; int nw = 0;
    { PGresult *r = PQexecParams(pg[0], "SELECT id, lineage, trust FROM witness", 0, NULL, NULL, NULL, NULL, 1); must(pg[0], r, PGRES_TUPLES_OK, "witnesses");
      nw = PQntuples(r); wit = malloc(sizeof(Wit) * (size_t)(nw + 1));
      for (int j = 0; j < nw; j++) { memcpy(wit[j].id.b, PQgetvalue(r, j, 0), 16); if (PQgetisnull(r, j, 1)) wit[j].lin = wit[j].id; else memcpy(wit[j].lin.b, PQgetvalue(r, j, 1), 16);
          uint64_t u = 0; const uint8_t *b = (const uint8_t *)PQgetvalue(r, j, 2); for (int y = 0; y < 8; y++) u = u << 8 | b[y]; memcpy(&wit[j].trust, &u, 8); }
      PQclear(r); }
    /* every claim that has a standing: they are what a row or a vertex can be */
    { if (!PQsendQueryParams(pg[0], "SELECT claim FROM consensus", 0, NULL, NULL, NULL, NULL, 1) || !PQsetSingleRowMode(pg[0])) { fprintf(stderr, "%s", PQerrorMessage(pg[0])); return 1; }
      PGresult *r; while ((r = PQgetResult(pg[0]))) { if (PQresultStatus(r) == PGRES_SINGLE_TUPLE) { lp_id id; memcpy(id.b, PQgetvalue(r, 0, 0), 16); find(&id, 1); } else must(pg[0], r, PGRES_TUPLES_OK, "standings"); PQclear(r); } }
    printf("  %-52s %'12llu   %d witnesses   (%.1f s)\n", "claims with a standing", (unsigned long long)ns, nw, now() - t); fflush(stdout);

    /* the ledger, in the order it was written */
    t = now(); uint64_t rows = 0, records = 0, played = 0, entered = 0, copies = 0, unknown = 0;
    uint64_t pcap = 1u << 22, nseen = 0; Seen *seen = calloc(pcap, sizeof(Seen));
    PQclear(PQexec(pg[0], "SET max_parallel_workers_per_gather = 0")); PQclear(PQexec(pg[0], "SET synchronize_seqscans = off"));
    PQclear(PQexec(pg[0], "BEGIN")); must(pg[0], PQexec(pg[0], "DECLARE led NO SCROLL CURSOR FOR SELECT claim, witness, score FROM attestation"), PGRES_COMMAND_OK, "the ledger");
    const int B = 200000; lp_id *bc = malloc(sizeof(lp_id) * B), *bw = malloc(sizeof(lp_id) * B); float *bs = malloc(sizeof(float) * B);
    for (;;) {
        char q[64]; snprintf(q, sizeof q, "FETCH %d FROM led", B); PGresult *r = PQexecParams(pg[0], q, 0, NULL, NULL, NULL, NULL, 1); must(pg[0], r, PGRES_TUPLES_OK, "the ledger");
        int n = PQntuples(r); if (!n) { PQclear(r); break; }
        for (int j = 0; j < n; j++) { memcpy(bc[j].b, PQgetvalue(r, j, 0), 16); memcpy(bw[j].b, PQgetvalue(r, j, 1), 16); uint32_t u; memcpy(&u, PQgetvalue(r, j, 2), 4); u = __builtin_bswap32(u); memcpy(&bs[j], &u, 4); }
        PQclear(r); rows += (uint64_t)n;
        /* the records among them, down to their claims: one set of paths per level */
        Front f = { 0 }; Member *mem = NULL; uint64_t nm = 0, cm = 0;
        for (int j = 0; j < n; j++) if (!find(&bc[j], 0)) { front_push(&f, &bc[j], (uint32_t)j); records++; }
        while (f.n) {
            Front next = { 0 }; uint64_t cnt[17] = { 0 }; for (uint64_t i = 0; i < f.n; i++) cnt[(f.id[i].b[0] >> 4) + 1]++;
            for (int h = 0; h < 16; h++) cnt[h + 1] += cnt[h];
            uint64_t *at = malloc(8 * (f.n + 1)), fill[16]; memcpy(fill, cnt, sizeof fill); for (uint64_t i = 0; i < f.n; i++) at[fill[f.id[i].b[0] >> 4]++] = i;
            /* a root for each ID of this level (the first that asked for it) */
            uint64_t hc = 1024; while (hc < f.n * 2) hc <<= 1; uint32_t *hs = calloc(hc, 4);
            typedef struct { int h; uint64_t lo, n; } Job; const uint64_t CH = 20000; Job *job = malloc(sizeof(Job) * (f.n / CH + 17)); uint64_t nj = 0;
            for (int h = 0; h < 16; h++) { if (!path_sql[h]) continue; for (uint64_t lo = cnt[h]; lo < cnt[h + 1]; lo += CH) job[nj++] = (Job){ h, lo, cnt[h + 1] - lo < CH ? cnt[h + 1] - lo : CH }; }
            for (uint64_t i = 0; i < f.n; i++) { uint64_t k = key(&f.id[i]) & (hc - 1); int dup = 0; while (hs[k]) { if (!memcmp(&f.id[hs[k] - 1], &f.id[i], 16) && f.root[hs[k] - 1] == f.root[i]) { dup = 1; break; } k = (k + 1) & (hc - 1); } if (!dup) hs[k] = (uint32_t)i + 1; }
            #pragma omp parallel for num_threads(npg) schedule(dynamic)
            for (uint64_t jx = 0; jx < nj; jx++) {
                uint32_t k = (uint32_t)job[jx].n; lp_id *part = malloc(sizeof(lp_id) * k); for (uint32_t i = 0; i < k; i++) part[i] = f.id[at[job[jx].lo + i]];
                uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t len = uuid_param(ab, part, k); PGconn *c = pg[omp_get_thread_num()];
                const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, fm[1] = { 1 };
                PGresult *pr = PQexecParams(c, path_sql[job[jx].h], 1, NULL, v, l, fm, 1); must(c, pr, PGRES_TUPLES_OK, "paths");
                #pragma omp critical(replay_paths)
                for (int x = 0; x < PQntuples(pr); x++) {
                    lp_id e; memcpy(e.b, PQgetvalue(pr, x, 0), 16);
                    const uint8_t *vx; size_t nv = lp_ewkb_vertices((const uint8_t *)PQgetvalue(pr, x, 1), (size_t)PQgetlength(pr, x, 1), &vx);
                    /* every root that asked for this entity at this level */
                    for (uint32_t i = 0; i < k; i++) { uint64_t fi = at[job[jx].lo + i]; if (memcmp(&f.id[fi], &e, 16)) continue; uint32_t root = f.root[fi];
                        for (size_t y = 0; y < nv; y++) { double xyz[3], m; memcpy(xyz, vx + 32 * y, 24); memcpy(&m, vx + 32 * y + 24, 8); uint32_t said = lp_m_said(m); if (!said) continue;
                            lp_id id; lp_xyz_to_id(xyz, &id);
                            if (said & LP_SAID_CLAIM) { St *s = find(&id, 0); if (s) { if (nm == cm) { cm = cm ? cm * 2 : 1u << 20; mem = xrealloc(mem, cm * sizeof(Member)); } mem[nm++] = (Member){ root, (uint32_t)(s - st) }; } }
                            else front_push(&next, &id, root); } }
                }
                PQclear(pr); free(ab); free(part);
            }
            free(at); free(hs); free(job); free(f.id); free(f.root); f = next;
        }
        /* members by the row they belong to */
        uint64_t *first = calloc((size_t)n + 1, 8); for (uint64_t i = 0; i < nm; i++) first[mem[i].root + 1]++;
        for (int j = 0; j < n; j++) first[j + 1] += first[j];
        uint32_t *of = malloc(4 * (nm + 1)); { uint64_t *fl = malloc(8 * ((size_t)n + 1)); memcpy(fl, first, 8 * ((size_t)n + 1)); for (uint64_t i = 0; i < nm; i++) of[fl[mem[i].root]++] = mem[i].claim; free(fl); }
        for (int j = 0; j < n; j++) {
            int wi = 0; while (wi < nw && memcmp(&wit[wi].id, &bw[j], 16)) wi++; if (wi == nw) { unknown++; continue; }
            if ((nseen + 1) * 2 > pcap) { uint64_t oc = pcap; Seen *old = seen; pcap *= 2; seen = calloc(pcap, sizeof(Seen));
                for (uint64_t i = 0; i < oc; i++) if (old[i].used) { uint64_t h; memcpy(&h, old[i].w.b, 8); uint64_t c2; memcpy(&c2, old[i].lin.b + 8, 8); h ^= c2 * 0x9E3779B97F4A7C15ull; uint64_t k = h & (pcap - 1); while (seen[k].used) k = (k + 1) & (pcap - 1); seen[k] = old[i]; } free(old); }
            uint64_t h; memcpy(&h, bc[j].b, 8); uint64_t c2; memcpy(&c2, wit[wi].lin.b + 8, 8); h ^= c2 * 0x9E3779B97F4A7C15ull; uint64_t k = h & (pcap - 1); int copy = 0;
            while (seen[k].used) { if (!memcmp(&seen[k].w, &bc[j], 16) && !memcmp(&seen[k].lin, &wit[wi].lin, 16)) { copy = 1; break; } k = (k + 1) & (pcap - 1); }
            if (copy) { copies++; continue; }
            seen[k].used = 1; seen[k].w = bc[j]; seen[k].lin = wit[wi].lin; nseen++;
            double tr = wit[wi].trust, at_ = fabs(tr), dev = at_ == 0.0 ? 350.0 : lp_trust_deviation(at_); if (dev < 30.0) dev = 30.0;
            St *own = find(&bc[j], 0); uint64_t lo = own ? 0 : first[j], hi = own ? 1 : first[j + 1];
            for (uint64_t x = lo; x < hi; x++) { St *s = own ? own : &st[of[x]];
                if (!own) { int dup = 0; for (uint64_t y = lo; y < x && !dup; y++) dup = of[y] == of[x]; if (dup) continue; }
                if (!s->entered) { s->entered = 1; s->r = (lp_rating){ 1500.0, dev, 0.06 }; entered++; continue; }
                lp_attest(&s->r, tr, bs[j], 1500.0, 0.5, 30.0); s->matches++; played++; }
        }
        free(first); free(of); free(mem);
        fprintf(stderr, "\r  %'llu ledger rows, %'llu matchups   ", (unsigned long long)rows, (unsigned long long)played);
    }
    PQclear(PQexec(pg[0], "CLOSE led")); PQclear(PQexec(pg[0], "COMMIT")); fputc('\n', stderr);
    printf("  %-52s %'12llu   of them records %'llu, copies within a lineage %'llu   (%.1f s)\n", "ledger rows", (unsigned long long)rows, (unsigned long long)records, (unsigned long long)copies, now() - t);
    if (unknown) printf("  %-52s %'12llu\n", "rows of a witness the database does not know", (unsigned long long)unknown);
    uint64_t never = 0; for (uint64_t i = 0; i < ns; i++) never += !st[i].entered;
    printf("  %-52s %'12llu   matchups %'llu\n", "claims entered", (unsigned long long)entered, (unsigned long long)played);
    printf("  %-52s %'12llu\n", "standings the ledger no longer accounts for", (unsigned long long)never);
    if (!dry) {
        t = now(); must(pg[0], PQexec(pg[0], "BEGIN"), PGRES_COMMAND_OK, "begin"); must(pg[0], PQexec(pg[0], "TRUNCATE consensus"), PGRES_COMMAND_OK, "standings");
        PGresult *r = PQexec(pg[0], "COPY consensus (claim, rating, deviation, volatility, matches) FROM STDIN (FORMAT binary)"); must(pg[0], r, PGRES_COPY_IN, "standings"); PQclear(r);
        size_t cap = 1u << 22, k = 0; uint8_t *b = malloc(cap + 128); static const uint8_t hdr[19] = { 'P','G','C','O','P','Y','\n',0xFF,'\r','\n',0, 0,0,0,0, 0,0,0,0 }; memcpy(b, hdr, 19); k = 19;
        for (uint64_t i = 0; i < ns; i++) { St *s = &st[i]; if (!s->entered) continue;
            b[k++] = 0; b[k++] = 5; b[k++] = 0; b[k++] = 0; b[k++] = 0; b[k++] = 16; memcpy(b + k, s->id.b, 16); k += 16;
            double d[3] = { s->r.rating, s->r.deviation, s->r.volatility };
            for (int z = 0; z < 3; z++) { uint64_t u; memcpy(&u, &d[z], 8); b[k++] = 0; b[k++] = 0; b[k++] = 0; b[k++] = 8; for (int y = 0; y < 8; y++) b[k++] = (uint8_t)(u >> (56 - 8 * y)); }
            b[k++] = 0; b[k++] = 0; b[k++] = 0; b[k++] = 4; b[k++] = (uint8_t)(s->matches >> 24); b[k++] = (uint8_t)(s->matches >> 16); b[k++] = (uint8_t)(s->matches >> 8); b[k++] = (uint8_t)s->matches;
            if (k > cap) { if (PQputCopyData(pg[0], (const char *)b, (int)k) != 1) { fprintf(stderr, "COPY: %s", PQerrorMessage(pg[0])); return 1; } k = 0; } }
        b[k++] = 0xFF; b[k++] = 0xFF; if (PQputCopyData(pg[0], (const char *)b, (int)k) != 1 || PQputCopyEnd(pg[0], NULL) != 1) { fprintf(stderr, "COPY: %s", PQerrorMessage(pg[0])); return 1; }
        while ((r = PQgetResult(pg[0]))) { must(pg[0], r, PGRES_COMMAND_OK, "standings"); PQclear(r); }
        must(pg[0], PQexec(pg[0], "COMMIT"), PGRES_COMMAND_OK, "commit"); free(b);
        printf("  %-52s %'12llu   (%.1f s)\n", "standings written", (unsigned long long)(ns - never), now() - t);
    }
    printf("\n== total %.1f s\n", now() - T);
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}
