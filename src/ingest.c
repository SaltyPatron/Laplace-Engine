#define _GNU_SOURCE
/* laplace ingest: files through their recipes.
 *   laplace ingest [-d conninfo] [-t tier0.bin] [-r recipes/] [-j threads] [-s source] [--whole] [--no-load] [--plan] file...
 *   laplace ingest [options]          with nothing named: every source, in the order they go in
 * Files decompose on every core; each is recomposed from the node table and compared with its bytes. A file is a
 * trunk in the DAG, over its metadata and its content (file.c); a file whose trunk is recorded already is recorded,
 * and nothing of it is written again. Then new nodes are written, and what was attested is played and recorded. Live counters go to stderr, phase times to stdout. */
#include "engine.h"
#include <arpa/inet.h>
#include <locale.h>
#include <ftw.h>
#include <sys/stat.h>
#include <omp.h>
#include <zlib.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many recipes a recipe's keys are reached through: 0 when it refers to none (refer NAME RECIPE), else one more
 * than the recipes it refers to. The files of a deeper recipe are read after the shallower ones' */
static int refer_depth(const Recipe *rec, int nrec, const Recipe *r, int guard){
    if (!r || guard > 16) return 0; int d = 0;
    const char *nm; for (int z = 0; (nm = say_refers(r, z)); z++) for (int k = 0; k < nrec; k++) if (!strcmp(rec[k].name, nm)) { int x = 1 + refer_depth(rec, nrec, &rec[k], guard + 1); if (x > d) d = x; }
    return d;
}
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
    for (uint32_t v = 0; v < x->nv; v++) for (uint32_t r = 0; r < VRUN(VTX[x->voff + v].m); r++) if (!expand(&VTX[x->voff + v].id, o)) return 0;
    return 1;
}

/* Every composition a batch made, by tier: whether the database holds it already (recorded, in the real tables or the
 * stage) or it would be new, how many parts it has, and its text (a claim or tuple as its parts). Nothing is written:
 * laplace ingest --entities, on a sample, shows what a recipe would record before anything is loaded. */
/* What this source has staged, every ID of it, kept here (the client knows what it wrote; the database is not asked):
 * a node an earlier batch staged is marked recorded before the next batch is loaded, so neither it nor anything under
 * it is staged again, and the stage holds each node of the source once. Kept by this process, which forks each batch's
 * writer: it is marked before the fork. */
static lp_idmap *staged_set; static uint64_t staged_again;
static void staged_mark(void){
    if (!staged_set) staged_set = lp_idmap_new();
    TABLE_EACH(x) { bool fresh; lp_idmap_put(staged_set, &x->id, &fresh); if (!fresh) { x->keep = 2; staged_again++; } }
}
static void show_tuple(const lp_id *id);
static void show_entities(const char *conninfo, int ask){
    uint64_t n = 0; TABLE_EACH(x) n++; if (!n) return;
    lp_id *ids = malloc(sizeof(lp_id) * n); Node **of = malloc(sizeof(Node *) * n); uint64_t k = 0; TABLE_EACH(x) { ids[k] = x->id; of[k++] = x; }
    if (ask) { PGconn *t = PQconnectdb(conninfo); ask = PQstatus(t) == CONNECTION_OK; PQfinish(t); }     /* no database: the entities alone */
    uint8_t *hit = ask ? db_recorded(conninfo, ids, n) : NULL;
    uint64_t by[17][2] = { { 0 } };
    for (int t = 0; t <= 16; t++) for (uint64_t i = 0; i < n; i++) { Node *x = of[i]; if ((x->tier < 16 ? x->tier : 16) != t) continue;
        int rec = hit && hit[i]; by[t][rec]++;
        printf("  tier %-2d %-9s %4u parts   ", x->tier, !hit ? "" : rec ? "recorded" : "new", x->len);
        if (x->kind & ((1u << LP_KIND_CLAIM) | (1u << LP_KIND_TUPLE))) show_tuple(&x->id);
        else { Buf o = { 0 }; expand(&x->id, &o); size_t m = o.n > 160 ? 160 : o.n; for (size_t j = 0; j < m; j++) putchar(o.b[j] == '\n' ? ' ' : o.b[j]); if (o.n > m) printf("..."); free(o.b); }
        putchar('\n'); }
    printf("  entities: %'llu", (unsigned long long)n);
    if (hit) { uint64_t nn = 0, nr = 0; for (int t = 0; t <= 16; t++) { nn += by[t][0]; nr += by[t][1]; } printf(", %'llu new, %'llu recorded", (unsigned long long)nn, (unsigned long long)nr); }
    printf("; by tier:"); for (int t = 0; t <= 16; t++) if (by[t][0] + by[t][1]) printf(" %d:%llu", t, (unsigned long long)(by[t][0] + by[t][1])); putchar('\n');
    free(ids); free(of); free(hit);
}

/* Directories are walked for every file under them. */
static char **paths; static const Source **path_of; static int npaths, cpaths; static const Source *walking;
static void add_path(const char *p){
    if (npaths == cpaths) { cpaths = cpaths ? cpaths * 2 : 1024; paths = xrealloc(paths, sizeof(char *) * cpaths); path_of = xrealloc(path_of, sizeof(Source *) * cpaths); }
    path_of[npaths] = walking; paths[npaths++] = strdup(p);
}
static int walk_cb(const char *p, const struct stat *st, int type, struct FTW *fw){
    const char *b = p + fw->base;
    if (type == FTW_D && b[0] == '.' && fw->level > 0) return FTW_SKIP_SUBTREE;          /* hidden directories */
    if (type == FTW_F && b[0] != '.' && st->st_size > 0) add_path(p);
    return FTW_CONTINUE;
}

/* A claim or a tuple as text: its parts between brackets, a part that is itself a tuple the same way. */
static void show_tuple(const lp_id *id){
    Node *c = table_find(id); if (!c) { Buf o = { 0 }; expand(id, &o); fwrite(o.b, 1, o.n, stdout); free(o.b); return; }
    int first = 1; putchar('[');
    for (uint32_t v = 0; v < c->nv; v++) for (uint32_t r = 0; r < VRUN(VTX[c->voff + v].m); r++) {
        if (!first) printf(", "); first = 0;
        if (VSAID(VTX[c->voff + v].m) == LP_SAID_TUPLE && table_find(&VTX[c->voff + v].id)) show_tuple(&VTX[c->voff + v].id);
        else { Buf o = { 0 }; expand(&VTX[c->voff + v].id, &o);
               if (!o.n && HW) { int64_t at = lp_highway_slot(HW, NULL, &VTX[c->voff + v].id); const char *ln = "type";
                   if (at >= 0) for (size_t i = 0; i < HW->nlists; i++) if ((size_t)at >= HW->list[i].first && (size_t)at < (size_t)HW->list[i].first + HW->list[i].count) ln = HW->list[i].name;
                   printf("<%s>", ln); }
               else fwrite(o.b, 1, o.n, stdout); free(o.b); } }
    putchar(']');
}
/* What a record holds besides its claims: the things and specifics said with them, and the records inside it. */
static void show_held(const lp_id *id, int depth){
    Node *c = table_find(id); if (!c || depth > 6) return;
    for (uint32_t v = 0; v < c->nv; v++) { uint64_t said = VSAID(VTX[c->voff + v].m);
        if (said == LP_SAID_TUPLE) { printf("%*s+ ", depth * 2 + 2, ""); show_tuple(&VTX[c->voff + v].id); putchar('\n'); }
        else if (said == LP_SAID_RECORD) { printf("%*s(\n", depth * 2 + 2, ""); show_held(&VTX[c->voff + v].id, depth + 1); printf("%*s)\n", depth * 2 + 2, ""); } }
}
/* Every source there is, in the order they go in, each in a process of its own with the options given, its output
 * kept in a log of its own. A source none of whose files is here is said so and passed over; a source that fails
 * stops the run, since what comes after it counts on it. What is already recorded is passed over by its bytes, so
 * a run that was cut off is taken up by running it again. */
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/statvfs.h>
/* How much a source's files hold, as its recipes would read them (what gzip holds is taken as eight times its size). */
static uint64_t source_bytes(const Source *sc, Recipe *rec, int nrec){
    int from = npaths; const Source *was = walking; walking = sc;
    if (sc->nfiles) for (int z = 0; z < sc->nfiles; z++) { glob_t g; if (!glob(sc->files[z], 0, NULL, &g)) for (size_t y = 0; y < g.gl_pathc; y++) add_path(g.gl_pathv[y]); globfree(&g); }
    else nftw(sc->found, walk_cb, 64, FTW_PHYS | FTW_ACTIONRETVAL);
    walking = was; uint64_t sum = 0;
    for (int i = from; i < npaths; i++) { struct stat st; if (recipe_for(rec, nrec, paths[i], sc) && !stat(paths[i], &st)) { size_t l = strlen(paths[i]); sum += (uint64_t)st.st_size * (l > 3 && !strcmp(paths[i] + l - 3, ".gz") ? 8 : 1); } free(paths[i]); }
    npaths = from; return sum;
}
/* A source's files its recipes read, in the order they are read: what refers to another recipe's keys after it, and
 * otherwise by name, so that whatever is made of them in order comes out the same on any machine. */
static const Recipe *rec_now; static int nrec_now;
static int by_depth(const void *x, const void *y){ const char *a = *(char *const *)x, *b = *(char *const *)y;
    int da = refer_depth(rec_now, nrec_now, recipe_for((Recipe *)rec_now, nrec_now, a, walking), 0), db = refer_depth(rec_now, nrec_now, recipe_for((Recipe *)rec_now, nrec_now, b, walking), 0);
    return da != db ? da - db : strcmp(a, b); }
int source_files(const Source *sc, Recipe *rec, int nrec, char ***out, Recipe ***of){
    int from = npaths; const Source *was = walking; walking = sc;
    if (sc->nfiles) for (int z = 0; z < sc->nfiles; z++) { glob_t g; if (!glob(sc->files[z], 0, NULL, &g)) for (size_t y = 0; y < g.gl_pathc; y++) add_path(g.gl_pathv[y]); globfree(&g); }
    else nftw(sc->found, walk_cb, 64, FTW_PHYS | FTW_ACTIONRETVAL);
    int n = 0; char **p = malloc(sizeof(char *) * (size_t)(npaths - from + 1));
    for (int i = from; i < npaths; i++) { if (recipe_for(rec, nrec, paths[i], sc)) p[n++] = paths[i]; else free(paths[i]); }
    npaths = from; rec_now = rec; nrec_now = nrec; qsort(p, (size_t)n, sizeof(char *), by_depth);
    Recipe **r = malloc(sizeof(Recipe *) * (size_t)(n + 1)); for (int i = 0; i < n; i++) r[i] = recipe_for(rec, nrec, p[i], sc);
    walking = was; *out = p; *of = r; return n;
}
/* The room left where the database keeps its data, or -1 when that cannot be seen from here (another machine). */
static double room_left(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo); double gb = -1;
    if (PQstatus(pg) == CONNECTION_OK) { PGresult *r = PQexec(pg, "SHOW data_directory");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r)) { struct statvfs v; if (!statvfs(PQgetvalue(r, 0, 0), &v)) gb = (double)v.f_bavail * (double)v.f_frsize / 1e9; }
        PQclear(r); }
    PQfinish(pg); return gb;
}
/* What a source took, measured as it went in: the database's growth over what its files hold, kept in the work
 * directory (room.tsv: source, times), one line a source, the last measure kept. The room check reads it before the
 * recipe's room N, which was measured once by hand. */
static double db_bytes(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo); double b = -1;
    if (PQstatus(pg) == CONNECTION_OK) { PGresult *r = PQexec(pg, "SELECT pg_database_size(current_database())"); if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r)) b = atof(PQgetvalue(r, 0, 0)); PQclear(r); }
    PQfinish(pg); return b;
}
static double room_measured(const char *path, const char *source){
    FILE *f = fopen(path, "r"); if (!f) return 0; char line[512]; double x = 0; size_t n = strlen(source);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, source, n) && line[n] == '\t') x = atof(line + n + 1);
    fclose(f); return x;
}
static void room_keep(const char *path, const char *source, double times){
    char tmp[4200]; snprintf(tmp, sizeof tmp, "%s.new", path); FILE *o = fopen(tmp, "w"); if (!o) return;
    FILE *f = fopen(path, "r"); char line[512]; size_t n = strlen(source);
    while (f && fgets(line, sizeof line, f)) if (strncmp(line, source, n) || line[n] != '\t') fputs(line, o);
    if (f) fclose(f); fprintf(o, "%s\t%.3f\n", source, times); fclose(o); rename(tmp, path);
}
static int ingest_every(int argc, char **argv, Source *src, int nsrc, Recipe *rec, int nrec, const int *want){
    char self[4096]; ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1); if (sl <= 0) { perror("/proc/self/exe"); return 1; } self[sl] = 0;
    const char *work = getenv("LAPLACE_WORK"); char dir[4096]; snprintf(dir, sizeof dir, "%s/logs/ingest", work && *work ? work : "."); 
    { char cmd[4200]; snprintf(cmd, sizeof cmd, "%s", dir); for (char *c = cmd + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(cmd, 0775); *c = '/'; } mkdir(cmd, 0775); }
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    const char *conninfo = laplace_db(); int loads = 1;
    for (int a = 1; a < argc; a++) { if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[a + 1]; if (!strcmp(argv[a], "--no-load") || !strcmp(argv[a], "--plan") || !strcmp(argv[a], "--claims")) loads = 0; }
    /* What a source takes in the database is what its source file says was measured (room N, in times what its files
     * hold), or 65 times, the most measured of any, when it says nothing (LAPLACE_ROOM_FACTOR). A source is not begun when the volume would be
     * left with less than a tenth of itself, and that is said. */
    double factor = getenv("LAPLACE_ROOM_FACTOR") ? atof(getenv("LAPLACE_ROOM_FACTOR")) : 65.0; int short_of_room = 0, short_of_order = 0;
    char roomf[4200]; snprintf(roomf, sizeof roomf, "%s/room.tsv", work && *work ? work : ".");
    int *wentin = calloc((size_t)nsrc, sizeof(int));                       /* 1: it went in in this run, or has nothing to go in */
    printf("laplace ingest   %s, in order   logs in %s\n\n", want ? "the sources named" : "every source", dir);
    printf("%-4s %-38s %-10s %10s   %s\n", "", "source", "", "seconds", "");
    double T = now(); int failed = 0, absent = 0, empty = 0, went = 0;
    for (int i = 0; i < nsrc && !failed; i++) {
        if (want && !want[i]) continue;
        { const char *b = NULL;                                              /* recipes/order: what it comes after went in first, in this run */
          for (int a = 0; a < src[i].nafter && !b; a++) for (int j = 0; j < i; j++) if (!strcmp(src[j].name, src[i].after[a]) && wentin[j] != 1) b = src[j].name;
          if (b) { printf("%-4d %-38s %-10s %10s   it comes after %s, which did not go in\n", i + 1, src[i].name, "not begun", "", b); short_of_order++; continue; } }
        int mine = 0; for (int k = 0; k < nrec; k++) mine += rec[k].source == i; mine += src[i].nreads;
        if (!mine) { printf("%-4d %-38s %-10s %10s   no recipe reads it yet\n", i + 1, src[i].name, "passed", ""); empty++; wentin[i] = 1; continue; }
        if (!src[i].found[0]) { printf("%-4d %-38s %-10s %10s   it is at none of its roots\n", i + 1, src[i].name, "absent", ""); absent++; continue; }
        double measured = room_measured(roomf, src[i].name), bytes_in = (double)source_bytes(&src[i], rec, nrec), before = loads ? db_bytes(conninfo) : -1;
        if (loads) { double have = room_left(conninfo), need = bytes_in * (measured > 0 ? measured : src[i].room > 0 ? src[i].room : factor) / 1e9; struct statvfs v; double whole = 0;
            { PGconn *pg = PQconnectdb(conninfo); if (PQstatus(pg) == CONNECTION_OK) { PGresult *r = PQexec(pg, "SHOW data_directory"); if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) && !statvfs(PQgetvalue(r, 0, 0), &v)) whole = (double)v.f_blocks * (double)v.f_frsize / 1e9; PQclear(r); } PQfinish(pg); }
            if (have >= 0 && have - need < whole / 10) { char why[128]; snprintf(why, sizeof why, "no room: it may take %.0f GB, and %.0f GB is left", need, have);
                printf("%-4d %-38s %-10s %10s   %s\n", i + 1, src[i].name, "not begun", "", why); short_of_room++; continue; } }
        char log[4300]; snprintf(log, sizeof log, "%s/%s.log", dir, src[i].name);
        printf("%-4d %-38s ", i + 1, src[i].name); fflush(stdout);
        double t = now(); pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (!pid) {
            int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0664); if (fd < 0) { perror(log); _exit(127); }
            dup2(fd, 1); dup2(fd, 2); close(fd); setenv("LAPLACE_INGEST_ONE", "1", 1);   /* the child takes this source alone: what it comes after went in before it */
            char **av = malloc(sizeof(char *) * (size_t)(argc + 3)); int n = 0; av[n++] = self; av[n++] = "ingest";
            for (int a = 1; a < argc; a++) av[n++] = argv[a];                            /* the options, as they were given */
            av[n++] = (char *)src[i].name; av[n] = NULL;
            execv(self, av); perror(self); _exit(127);
        }
        int st = 0; while (waitpid(pid, &st, 0) < 0) { }
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        char last[256] = ""; { FILE *f = fopen(log, "r"); char line[4096]; while (f && fgets(line, sizeof line, f)) { char *cr = strrchr(line, '\r'); const char *c = cr && cr[1] && cr[1] != '\n' ? cr + 1 : line; while (*c == ' ') c++;
              if (strstr(c, "attestations") || strstr(c, "already recorded") || (code && *c && *c != '\n')) { snprintf(last, sizeof last, "%.200s", c); char *nl = strchr(last, '\n'); if (nl) *nl = 0; } } if (f) fclose(f); }
        printf("%-10s %'10.1f   %s\n", code ? "FAILED" : "in", now() - t, last); fflush(stdout);
        if (code) { failed = 1; fprintf(stderr, "\n%s did not go in (exit %d); what it said is in %s\n", src[i].name, code, log); } else { went++; wentin[i] = 1; }
        if (!code && loads && before >= 0 && bytes_in > 0) { double after = db_bytes(conninfo); if (after > before) room_keep(roomf, src[i].name, (after - before) / bytes_in); }     /* what it took, for the next run's check */
    }
    printf("\n%d sources in, %d absent, %d without a recipe, %d not begun for want of room, %d not begun because what they come after is not in%s   %'.1f s\n", went, absent, empty, short_of_room, short_of_order, failed ? ", and one that failed: the run stops there" : "", now() - T);
    /* each source that went in merged its own stage as it ended (cmd_ingest); one that failed left its stage, and its
     * next run takes up from it (what is staged is looked for as what is recorded is) */
    free(wentin); return failed || short_of_order;
}
static const uint64_t *by_size_of;                                    /* the batch's files, longest first, ties in the order given */
static int by_size(const void *a, const void *b){ int i = *(const int *)a, j = *(const int *)b; uint64_t x = by_size_of[i], y = by_size_of[j]; return x < y ? 1 : x > y ? -1 : i - j; }
/* The child writing a batch: waited for, its counts added to the run's. Nonzero when it failed. */
static int loader_wait(pid_t *pid, int fd, LoadStats *st){
    LoadStats cs; memset(&cs, 0, sizeof cs); ssize_t got = read(fd, &cs, sizeof cs); close(fd);
    int status = 0; while (waitpid(*pid, &status, 0) < 0) { } *pid = 0;
    if (got != (ssize_t)sizeof cs || !WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr, "\n  a batch was not written (its writer exited %d)\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)); return 1; }
    uint64_t *a = (uint64_t *)st, *b = (uint64_t *)&cs; for (size_t k = 0; k < offsetof(LoadStats, t_dedup) / sizeof(uint64_t); k++) a[k] += b[k];
    double *x = &st->t_dedup, *y = &cs.t_dedup; for (size_t k = 0; k < (sizeof(LoadStats) - offsetof(LoadStats, t_dedup)) / sizeof(double); k++) x[k] += y[k];
    return 0;
}
/* Which files are recorded, before any is read (the inventor: "if you have the file's trunk node (and it's hash metadata
 * matches), you know you have everything in that file already"). A file's metadata tree begins with the OS's record of
 * it, one node computed from stat alone (say.c, file_os); the container index gives what holds that node: the file's
 * trunk, where it is the metadata, or the metadata tree that begins with it, whose container is the trunk. A file found
 * is recorded with everything under it, and is never decomposed. Returns how many were found. */
static int ingest_ewkb_point(const uint8_t *e, size_t n, lp_coord *out){
    if (n < 5 || e[0] != 1) return 0; uint32_t t; memcpy(&t, e + 1, 4); size_t off = 5; if (t & 0x20000000u) off += 4;
    if (n < off + 32) return 0; for (int d = 0; d < 4; d++) { double v; memcpy(&v, e + off + 8 * d, 8); out->m[d] = (int64_t)(v * (double)LP_FIXED_ONE); } return 1;
}
static int files_recorded(const char *conninfo, File *files, int nfiles){
    lp_id *want = malloc(sizeof(lp_id) * (size_t)(nfiles + 1)); int *of = malloc(sizeof(int) * (size_t)(nfiles + 1)), nw = 0, found = 0;
    for (int i = 0; i < nfiles; i++) { if (files[i].skipped || files[i].known || !files[i].recipe) continue; Ref os; if (file_os(files[i].recipe, &files[i], &os)) { want[nw] = os.id; of[nw++] = i; } }
    PGconn *pg = nw ? db_connect(conninfo) : NULL;
    for (int level = 0; level < 8 && nw; level++) {                          /* a metadata tree factored into blocks is a few levels deep */
        lp_idmap *at = lp_idmap_new(); for (int k = 0; k < nw; k++) { bool fresh; lp_idmap_put(at, &want[k], &fresh); }
        int ncap = nw, nn = 0; lp_id *next = malloc(sizeof(lp_id) * (size_t)ncap); int *nof = malloc(sizeof(int) * (size_t)ncap);   /* an ID can be held by many: grown as they come */
        for (int k0 = 0; k0 < nw; k0 += 10000) { int k1 = nw - k0 < 10000 ? nw : k0 + 10000; uint8_t *ab = malloc(20 + 20 * (size_t)(k1 - k0)); size_t al = ids_param(ab, want + k0, (uint32_t)(k1 - k0));
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
            PGresult *r = PQexecParams(pg, "SELECT c.entity, st_asewkb(c.path), c.tier, st_asewkb(e.coord) FROM laplace_containers($1::blake3[], '{}'::smallint[]) c JOIN entity e ON e.id = c.entity AND e.tier = c.tier", 1, NULL, v, l, f, 1);
            if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "which files are recorded: %s", PQerrorMessage(pg)); PQclear(r); free(ab); break; }
            for (int j = 0; j < PQntuples(r); j++) {
                lp_vertex vx[2]; size_t nv = lp_path_vertices((const uint8_t *)PQgetvalue(r, j, 1), (size_t)PQgetlength(r, j, 1), vx, 2); if (!nv) continue;
                int64_t k = lp_idmap_find(at, &vx[0].id); if (k < 0) continue;                    /* held, but not first: not its metadata */
                lp_id c; memcpy(c.b, PQgetvalue(r, j, 0), 16);
                if (vx[0].said == LP_SAID_METADATA && nv >= 2) {                                   /* the file's trunk */
                    File *fl = &files[of[k]]; if (fl->known) continue;
                    lp_coord co; if (!ingest_ewkb_point((const uint8_t *)PQgetvalue(r, j, 3), (size_t)PQgetlength(r, j, 3), &co)) continue;
                    uint16_t tier; memcpy(&tier, PQgetvalue(r, j, 2), 2); tier = ntohs(tier);
                    memset(&fl->file, 0, sizeof fl->file); fl->file.id = c; fl->file.c = co; fl->file.tier = (uint8_t)tier; fl->file.said = 0;
                    fl->has_file = 1; fl->known = 1; found++; }
                else { if (nn == ncap) { ncap *= 2; next = xrealloc(next, sizeof(lp_id) * (size_t)ncap); nof = xrealloc(nof, sizeof(int) * (size_t)ncap); } next[nn] = c; nof[nn++] = of[k]; } }                                         /* a metadata tree it begins: what holds that is next */
            PQclear(r); free(ab); }
        lp_idmap_free(at); free(want); free(of); want = next; of = nof; nw = 0;
        for (int k = 0; k < nn; k++) if (!files[of[k]].known) { want[nw] = want[k]; of[nw++] = of[k]; }
    }
    if (pg) PQfinish(pg); free(want); free(of); table_reset();              /* what was composed to ask is composed again for the files that are read */
    return found;
}
int cmd_ingest(int argc, char **argv){
    const char *conninfo = laplace_db(), *t0p = NULL, *rdir = laplace_recipes();
    int threads = 0, do_load = 1, a = 1, show_claims = 0, entities = 0, ask = 1; const char *of = NULL;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) t0p = argv[++a];
        else if (!strcmp(argv[a], "-r") && a + 1 < argc) rdir = argv[++a];
        else if (!strcmp(argv[a], "-j") && a + 1 < argc) threads = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-s") && a + 1 < argc) of = argv[++a];            /* the files named are this source's: a part of it at a time */
        else if (!strcmp(argv[a], "--whole")) { extern int load_whole; load_whole = 1; }   /* after a run that was cut off: every node is looked for */
        else if (!strcmp(argv[a], "--no-load")) do_load = 0;
        else if (!strcmp(argv[a], "--plan")) do_load = -1;
        else if (!strcmp(argv[a], "--claims")) { show_claims = 1; do_load = 0; }     /* what the recipes attest, as text; nothing is loaded */
        else if (!strcmp(argv[a], "--entities")) { entities = 1; show_claims = 1; do_load = 0; }   /* what the recipes would record, entities and claims; nothing is loaded */
        else { fprintf(stderr, "usage: laplace ingest [-d conninfo] [-t tier0.bin] [-r recipes] [-j threads] [--no-load] [--plan] [--claims] [--entities] file...\n"); return 2; }
    }
    /* what a recipe would say is shown of files: of nothing named it is no ingest of every source, and --entities, which
     * asks the database of every composition, is of files only, never a source by its name */
    if (show_claims && a >= argc) { fprintf(stderr, "--claims and --entities show what files would record: name the files\n"); return 2; }
    Recipe *rec = NULL; int nrec = recipes_load(rdir, &rec), nsrc; Source *src = sources_loaded(&nsrc);
    if (entities) for (int i = a; i < argc; i++) { struct stat st_; if (stat(argv[i], &st_)) { fprintf(stderr, "--entities shows what files would record: %s is no file (a sample of a source: -s SOURCE FILE)\n", argv[i]); return 2; } }
    for (int k = 0; k < nrec; k++) if (!strcmp(rec[k].name, "file")) file_record_stock(&rec[k]);   /* what the OS keeps of every file: the stock recipe file */
    if (of) { int k = 0; while (k < nsrc && strcmp(src[k].name, of)) k++; if (k == nsrc) { fprintf(stderr, "%s is not a source\n", of); return 2; } walking = &src[k]; }
    { int named = 0; for (int i = a; i < argc; i++) { struct stat st_; int k = 0; while (k < nsrc && strcmp(src[k].name, argv[i])) k++; named += k < nsrc && stat(argv[i], &st_); }
      if (named && named == argc - a && !of && !getenv("LAPLACE_INGEST_ONE")) {   /* sources by name: each its own run, in recipes/order, after everything it comes after */
          int *want = calloc((size_t)nsrc, sizeof(int)); for (int i = a; i < argc; i++) for (int k = 0; k < nsrc; k++) if (!strcmp(src[k].name, argv[i])) want[k] = 1;
          for (int k = nsrc - 1; k >= 0; k--) if (want[k]) for (int x = 0; x < src[k].nafter; x++) for (int j = 0; j < k; j++) if (!strcmp(src[j].name, src[k].after[x])) want[j] = 1;   /* sources is in order: what one comes after is before it */
          int rc = ingest_every(a, argv, src, nsrc, rec, nrec, want); free(want); return rc; } }
    for (int i = a; i < argc; i++) {                                         /* a source by its name, or files and directories */
        struct stat st; int k = 0; while (k < nsrc && strcmp(src[k].name, argv[i])) k++;
        if (k < nsrc && stat(argv[i], &st)) {
            if (!src[k].found[0]) { fprintf(stderr, "source %s is not at any of its roots\n", src[k].name); return 1; }
            const Source *was = walking; walking = &src[k];
            if (src[k].nfiles) for (int z = 0; z < src[k].nfiles; z++) { glob_t g; if (!glob(src[k].files[z], 0, NULL, &g)) for (size_t y = 0; y < g.gl_pathc; y++) add_path(g.gl_pathv[y]); globfree(&g); }
            else nftw(src[k].found, walk_cb, 64, FTW_PHYS | FTW_ACTIONRETVAL);
            walking = was;
        }
        else {                                                               /* a file or directory: under a source's root, it is that source's */
            const Source *was = walking; char real[4096]; if (!walking && realpath(argv[i], real))
                for (int z = 0; z < nsrc; z++) { size_t l = strlen(src[z].found); if (l && !strncmp(real, src[z].found, l) && (real[l] == '/' || !real[l])) { walking = &src[z]; break; } }
            if (!stat(argv[i], &st) && S_ISDIR(st.st_mode)) nftw(argv[i], walk_cb, 64, FTW_PHYS | FTW_ACTIONRETVAL);
            else add_path(argv[i]);
            walking = was;
        }
    }
    if (a >= argc && !of) return ingest_every(argc, argv, src, nsrc, rec, nrec, NULL);         /* nothing named: every source, in order */
    int nfiles = npaths; if (nfiles <= 0) { fprintf(stderr, "no files\n"); return 2; }
    { int stop = 0, direct = 0; const Source *seen[64]; int ns = 0;          /* a recipe that did not load stops its own source only */
      for (int i = 0; i < nfiles; i++) { if (!path_of[i]) { direct = 1; continue; } int k = 0; while (k < ns && seen[k] != path_of[i]) k++; if (k == ns && ns < 64) seen[ns++] = path_of[i]; }
      for (int k = 0; k < ns; k++) stop += recipes_broken(rec, nrec, seen[k]);
      if (direct) stop += recipes_broken(rec, nrec, NULL);
      if (stop) return 2; }
    if (threads <= 0) threads = omp_get_num_procs();
    omp_set_num_threads(threads); omp_set_max_active_levels(1);
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    double T = now(), t;

    tier0_open(t0p);
    table_init();
    printf("laplace ingest   %d threads   cpu: %s   dispatch: %s   %d recipes\n", threads, lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()), nrec);

    File *files = calloc(nfiles, sizeof(File));
    int uncovered = 0;
    for (int i = 0; i < nfiles; i++) { files[i].path = paths[i]; files[i].recipe = recipe_for(rec, nrec, files[i].path, path_of[i]); files[i].source = path_of[i];
                                       if (files[i].recipe) files[i].trust = path_of[i] && files[i].recipe->source < 0 ? path_of[i]->trust : files[i].recipe->trust;
                                       if (!files[i].recipe) { files[i].skipped = 1; uncovered += path_of[i] == NULL; } }     /* a source takes the files its recipes name */
    /* a source's files, most trusted witness first; among equals, as they were found */
    for (int i = 1; i < nfiles; i++) { File x = files[i]; int j = i;
        while (j > 0 && path_of[i] && x.recipe && files[j - 1].recipe && x.recipe->trust > files[j - 1].recipe->trust) { files[j] = files[j - 1]; j--; }
        files[j] = x; }
    /* a file whose recipe refers to the keys of another recipe's rows is read after that recipe's files (refer COLUMN RECIPE) */
    int *depth = calloc((size_t)nfiles, sizeof(int));
    for (int i = 0; i < nfiles; i++) depth[i] = refer_depth(rec, nrec, files[i].recipe, 0);
    for (int i = 1; i < nfiles; i++) { File x = files[i]; int d = depth[i], j = i;
        while (j > 0 && depth[j - 1] > d) { files[j] = files[j - 1]; depth[j] = depth[j - 1]; j--; }
        files[j] = x; depth[j] = d; }
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

    /* Whether a file is recorded is not asked of its bytes: it is decomposed, its trunk is computed here, and the
     * trunk is looked for, trunk to leaf, with everything else (load). */
    int nknown = 0; ctx_open(threads);
    /* A source that records tier 0 (the Unicode source, first in recipes/order) records every codepoint before anything
     * of its own: each codepoint's ID, coordinate and Hilbert value are tier 0's, computed once from the same data and
     * mapped, never asked of the database. A codepoint recorded already is left as it is. */
    if (do_load > 0) for (int i = 0; i < nfiles; i++) if (files[i].source && files[i].source->tier0) { if (tier0_write(conninfo, threads)) return 1; break; }
    if (do_load) { double tk = now(); nknown = files_recorded(conninfo, files, nfiles); if (nknown) fprintf(stderr, "  %d files recorded already, found by the OS's record of them, not read (%.2f s)\n", nknown, now() - tk); }

    /* ---- a batch at a time: decomposed on every core, recomposed and compared, recorded, and the table emptied for
     * the next. A file too long for one batch is read a stretch at a time, if its records can be parted. */
    t = now();
    uint64_t batch = (uint64_t)(getenv("LAPLACE_BATCH_MB") ? atoll(getenv("LAPLACE_BATCH_MB")) : 1024) << 20;
    uint64_t bytes = 0, nev = 0; int done = 0, exact = 0, mism = 0, batches = 0; double t_dec = 0, t_rec = 0; LoadStats st = { 0 };
    pid_t loader = 0; int loader_fd = -1;                                     /* the child writing the batch before, if one is */
    st.known += (uint64_t)nknown;                                            /* found by their OS record, never read */
    uint64_t *size = calloc((size_t)nfiles, 8);
    for (int i = 0; i < nfiles; i++) { struct stat sb; if (files[i].known || files[i].skipped || stat(files[i].path, &sb)) continue;
        size_t l = strlen(files[i].path); size[i] = (uint64_t)sb.st_size * (l > 3 && !strcmp(files[i].path + l - 3, ".gz") ? 8 : 1); }
    #define SHOW(F) do { if (show_claims) for (uint64_t e = 0; e < (F)->ev.n; e++) { const Event *x_ = &(F)->ev.e[e]; \
        if (x_->kind == EV_RECORD) { printf("-- record %u", x_->position); if (x_->own_witness) { printf("   by "); show_tuple(&x_->witness); } putchar('\n'); show_held(&x_->witnessed, 0); continue; } \
        if (!table_find(&x_->claim)) continue; \
        show_tuple(&x_->claim); \
        if (x_->position) printf("   #%u", x_->position); \
        if (x_->score != 1.0f) printf("   score %.3g", (double)x_->score); \
        if (x_->own_witness) { printf("   by "); show_tuple(&x_->witness); } \
        putchar('\n'); } } while (0)
    /* What was read of a file is kept for its content; a file read to its end gets its trunk. */
    #define WHOLE(F) do { File *f_ = (F); if (!f_->partial) file_close(f_); } while (0)
    for (int a0 = 0; a0 < nfiles; ) {
        int b0 = a0; uint64_t sum = 0; char boundary = 0;
        while (b0 < nfiles && (b0 == a0 || (sum + size[b0] <= batch && depth[b0] == depth[a0]))) { sum += size[b0]; b0++; }     /* a batch is a barrier: what refers waits for what is referred to */
        table_size(sum);                                                                                                  /* the table is empty here: room for what this batch makes */
        if (b0 == a0 + 1 && size[a0] > batch && reads_in_stretches(files[a0].recipe, &boundary)) {
            table_size(batch);                                                                                    /* a stretch at a time: room for one stretch */
            /* One long file, a stretch at a time. It is read twice: first for what it is, its trunk, with nothing
             * recorded; and, if that trunk is not recorded, again to record it. A file already recorded costs its
             * decomposition and one lookup, and nothing is written.  The first pass ends at the first stretch attesting a
             * claim that is not recorded: a trunk is written after every claim of every stretch, so that file's is not, and
             * a new file is read once more, not twice. (Its stretches' parts, the content under the trunk, are not what is
             * asked: a stretch writes what it attests, and its parts only where they are claims.) */
            File *f = &files[a0]; int failed = 0;
            int unrecorded = 0;                                               /* a stretch held something not recorded: the trunk is not looked for */
            for (int pass = do_load ? 0 : 1; pass < 2 && !failed && !f->known; pass++) {
                gzFile g = gzopen(f->path, "rb"); if (!g) { f->skipped = 1; break; } gzbuffer(g, 1 << 20);
                size_t cap = (size_t)batch / 2 + (64u << 20), have = 0; uint8_t *buf = malloc(cap + 1); int first = 1, last = 0;
                f->bytes = 0; f->records = 0; f->incomplete = 0; f->has_file = 0;
                while (!last) {
                    double td = now(); int got; while (have < cap - (1u << 20) && (got = gzread(g, buf + have, (unsigned)((cap - have) > (1u << 30) ? (1u << 30) : (cap - have)))) > 0) have += (size_t)got;
                    last = have < cap - (1u << 20); size_t end = have;
                    if (!last) { end = 0; for (size_t i = have; i > 1; i--) if (buf[i - 1] == '\n' && (boundary == 1 || (i >= 2 && buf[i - 2] == '\n') || (i >= 3 && buf[i - 2] == '\r' && buf[i - 3] == '\n'))) { end = i; break; }
                                 if (!end) { fprintf(stderr, "\n  %s: a record longer than a stretch (%zu MB): the file cannot be read in stretches of this length\n", f->path, cap >> 20); mism++; failed = 1; break; } }
                    f->partial = !last;
                    #pragma omp parallel
                    #pragma omp single
                    decompose_bytes(CTX[omp_get_thread_num()], f, buf, end, first);
                    first = 0; f->bytes += end; WHOLE(f); t_dec += now() - td;
                    if (!pass && !last && f->ev.n) {                                   /* a claim it attests not recorded: its trunk is not (a trunk is written after every claim of every stretch), and it is recorded now */
                        uint64_t k = 0, want = f->ev.n < 4096 ? f->ev.n : 4096; lp_id *ids = malloc(sizeof(lp_id) * want); uint8_t *tt = malloc(want);
                        for (uint64_t j = 0; j < f->ev.n && k < want; j++) if (f->ev.e[j].kind != EV_RECORD) { const Node *x = table_find(&f->ev.e[j].claim); if (!x) continue; ids[k] = x->id; tt[k++] = x->tier; }     /* at the tier it is recorded at: the lowest it is composed at */
                        if (!db_all_recorded(conninfo, ids, tt, k)) unrecorded = 1;
                        free(ids); free(tt); }
                    if (pass) { bytes += end; nev += f->ev.n; batches++; }
                    fprintf(stderr, "\r  %s: %s  %.1f MB read  %'llu nodes in this stretch   ", f->path, pass ? "recording" : "its trunk", f->bytes / 1e6, (unsigned long long)table_count());
                    if (pass) { SHOW(f); if (do_load) staged_mark(); if (do_load && load(conninfo, threads, f, 1, &st)) return 1; }
                    free(f->ev.e); memset(&f->ev, 0, sizeof f->ev); table_reset();
                    memmove(buf, buf + end, have - end); have -= end;
                    if (unrecorded && !pass) { fprintf(stderr, "\n  %s: what it holds is not all recorded: it is recorded now\n", f->path); break; }
                }
                gzclose(g); free(buf);
                if (unrecorded && !pass) { free(f->said); f->said = NULL; f->nsaid = f->csaid = 0; continue; }
                if (!pass && !failed && f->has_file) {                          /* is its trunk recorded */
                    PGconn *pg = db_connect(conninfo); uint8_t ab[40]; size_t al = ids_param(ab, &f->file.id, 1);
                    const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, fm[1] = { 1 };
                    char q[96]; snprintf(q, sizeof q, "SELECT 1 FROM entity WHERE id = ANY($1::blake3[])");   /* by its ID alone: one partition */
                    PGresult *r = PQexecParams(pg, q, 1, NULL, v, l, fm, 0);
                    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "%s", PQerrorMessage(pg)); return 1; }
                    if (PQntuples(r)) { f->known = 1; st.known++; }
                    PQclear(r); PQfinish(pg);
                }
            }
            done++; exact++; a0 = b0; continue;
        }
        double td = now();
        /* the longest first: a long file begun last is decomposed on one core while every other waits for it */
        int *order = malloc(sizeof(int) * (size_t)(b0 - a0)); for (int i = a0; i < b0; i++) order[i - a0] = i;
        by_size_of = size; qsort(order, (size_t)(b0 - a0), sizeof(int), by_size);
        #pragma omp parallel
        #pragma omp single
        for (int o = 0; o < b0 - a0; o++) {
            int i = order[o];
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
        free(order); t_dec += now() - td; td = now();
        /* every file recomposed from the node table and compared with its bytes */
        #pragma omp parallel for schedule(dynamic) reduction(+:exact, mism)
        for (int i = a0; i < b0; i++) {
            if (files[i].known || files[i].skipped) continue;
            if (files[i].recipe && files[i].recipe->curated) { exact++; continue; }      /* a curated source is not kept as a file */
            FILE *f = fopen(files[i].path, "rb"); uint8_t *src = malloc(files[i].bytes + 1);
            size_t got = fread(src, 1, files[i].bytes, f); fclose(f);
            Buf o = { 0 }; int ok = expand(&files[i].trunk.id, &o) && got == files[i].bytes && o.n == got && !memcmp(o.b, src, got);
            if (ok) exact++; else { mism++; fprintf(stderr, "  %s: does not recompose\n", files[i].path); }
            if (getenv("LAPLACE_TRUNKS")) { const uint8_t *b = files[i].trunk.id.b; fprintf(stderr, "  trunk %02x%02x%02x%02x%02x%02x%02x%02x  %s\n", b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7], files[i].path); }
            free(o.b); free(src);
        }
        t_rec += now() - td;
        for (int i = a0; i < b0; i++) { if (!files[i].known && !files[i].skipped) WHOLE(&files[i]); nev += files[i].ev.n; SHOW(&files[i]); }
        if (entities) show_entities(conninfo, ask);
        if (do_load) { staged_mark();                                        /* what an earlier batch staged is not staged again */
            /* The batch before is written and committed first, so this batch's lookups see it. When more batches follow,
             * this one is written by a child, from its copy of the node table, while this process empties the table and
             * decomposes the next on every core: the cores and the database work at the same time. */
            if (loader > 0 && loader_wait(&loader, loader_fd, &st)) return 1;
            if (b0 < nfiles) { int pfd[2]; if (pipe(pfd)) { perror("pipe"); return 1; } fflush(NULL); pid_t p = fork();
                if (p < 0) { perror("fork"); return 1; }
                if (!p) { close(pfd[0]); LoadStats cs; memset(&cs, 0, sizeof cs); int rc = load(conninfo, threads, files + a0, b0 - a0, &cs);
                          if (write(pfd[1], &cs, sizeof cs) != (ssize_t)sizeof cs) rc = 1; fflush(NULL); _exit(rc ? 1 : 0); }
                close(pfd[1]); loader = p; loader_fd = pfd[0]; }
            else if (load(conninfo, threads, files + a0, b0 - a0, &st)) return 1; }
        batches++;
        if (b0 < nfiles) { for (int i = a0; i < b0; i++) { free(files[i].ev.e); memset(&files[i].ev, 0, sizeof files[i].ev); } table_reset(); }
        a0 = b0;
    }
    #undef SHOW
    #undef WHOLE
    if (loader > 0 && loader_wait(&loader, loader_fd, &st)) return 1;        /* the last batch a child wrote */
    fputc('\n', stderr);
    extern uint64_t table_total(void), table_hits(void);
    printf("\n== decomposition: %d files, %.1f MB, content recomposed byte for byte or curated %d, mismatched %d%s\n", nfiles, bytes / 1e6, exact, mism, batches > 1 ? ", a batch at a time" : "");
    if (do_load) printf("   files whose trunk was already recorded: %'llu; whose content was, under another trunk, so it attests nothing again: %'llu\n", (unsigned long long)st.known, (unsigned long long)st.content_known);
    printf("   %'llu compositions, %'llu reused; %'llu attestations\n", (unsigned long long)table_total(), (unsigned long long)table_hits(), (unsigned long long)nev);
    { uint64_t inc = 0; for (int i = 0; i < nfiles; i++) inc += files[i].incomplete;
      if (inc) printf("   %'llu parts were not read whole: what parses is recorded, and what they attest is incomplete (see above); the source still goes in\n", (unsigned long long)inc); }
    printf("\n== phases\n");
    printf("  %-44s %8.2f s   %8.1f MB/s\n", "decompose (all threads)", t_dec, bytes / 1e6 / (t_dec > 0 ? t_dec : 1));
    printf("  %-44s %8.2f s\n", "recompose and compare", t_rec);
    if (!do_load) return mism ? 1 : 0;
    printf("  %-44s %8.2f s   %'llu IDs checked in %llu rounds, %'llu subtrees already recorded\n", "deduplication, trunk to leaf", st.t_dedup,
           (unsigned long long)st.checked, (unsigned long long)st.rounds, (unsigned long long)st.found);
    printf("  %-44s %8.2f s   %'llu entities, %'llu paths (%'.0f rows/s)\n", "COPY into every partition", st.t_copy,
           (unsigned long long)st.ent_rows, (unsigned long long)st.phy_rows, (st.ent_rows + st.phy_rows) / (st.t_copy > 0 ? st.t_copy : 1));
    if (nev) printf("  %-44s %8.2f s   %'llu attestations new, the rest games of those recorded; standings %'llu new, %'llu updated\n", "witnesses, attestations, standings", st.t_sem,
                    (unsigned long long)st.led, (unsigned long long)st.std_new, (unsigned long long)st.std_upd);
    if (nev) printf("    %-42s %8.2f s\n    %-42s %8.2f s\n    %-42s %8.2f s\n    %-42s %8.2f s\n",
                    "standings and lineages read", st.t_read, "matchups played", st.t_play, "witnesses", st.t_wit, "attestations and standings, every partition", st.t_led);
    { const Source *one = NULL; int many = 0; Ref trunk;                    /* one source, every file of it recorded: its trunk, last of all */
      for (int i = 0; i < nfiles; i++) { if (files[i].skipped) continue; if (!files[i].source) { many = 1; break; } if (!one) one = files[i].source; else if (one != files[i].source) many = 1; }
      if (one && !many && !mism && !of) { table_reset(); table_size(64u << 20);   /* the last batch is written: an empty table for the trunk alone */
          if (source_trunk(one, files, nfiles, &trunk)) { File sf; memset(&sf, 0, sizeof sf); sf.path = one->name; sf.source = one; sf.trunk = trunk; sf.file = trunk; sf.has_file = 1;
              LoadStats ss = { 0 }; double ts = now(); staged_mark(); if (load(conninfo, threads, &sf, 1, &ss)) return 1;
              printf("  %-44s %8.2f s   %s%s\n", "the source's trunk", now() - ts, source_called(one), ss.ent_rows ? "" : ": already recorded"); }
          else printf("  the source's trunk: none, since its source file names no record (witness or called)\n");
          table_reset(); } }
    /* What this ingest staged goes into the real tables now, before anything after it: a source is in once its records
     * are (the inventor: "decompose and stage all the records necessary... and then we batch that into the real
     * database"). The next source is read against them, a key a record breaks stops this source and no other, and
     * what is in can be read while the rest goes in. Each leaf takes what was staged for it by rewriting or by
     * appending, whichever writes less (merge): a small source appends. */
    if (do_load > 0 && !mism) { printf("\nmerge\n"); fflush(stdout); if (merge(conninfo, threads)) return 1; }
    printf("\n== total %.1f s\n", now() - T);
    return mism ? 1 : 0;
}
