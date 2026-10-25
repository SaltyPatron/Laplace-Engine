#define _GNU_SOURCE
/* laplace ingest: files through their recipes.
 *   laplace ingest [-d conninfo] [-t tier0.bin] [-r recipes/] [-j threads] [-s source] [--whole] [--no-load] [--plan] file...
 *   laplace ingest [options]          with nothing named: every source, in the order they go in
 * Files decompose on every core; each is recomposed from the node table and compared with its bytes. A file is a
 * trunk in the DAG, over its metadata and its content (file.c); a file whose trunk is recorded already is recorded,
 * and nothing of it is written again. Then new nodes are written, and what was attested is played and recorded. Live counters go to stderr, phase times to stdout. */
#include "engine.h"
#include "laplace/highway.h"
#include <blake3.h>
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
#include <limits.h>

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
    char *s = strdup(p); for (char *q = s; *q; q++) if (*q == '\\') *q = '/';
    path_of[npaths] = walking; paths[npaths++] = s;
}

/* ---- ignore files (a source's `ignore NAME`): what gitignore(5) says of the files beside and below one. A line is a
 * pattern; `#` begins a comment line and `!` turns a pattern into one that takes a file back; a pattern ending in `/`
 * is of directories only; one with a `/` before its end is matched against the path from the ignore file's directory,
 * one without against a file's own name at any depth. `*`, `?` and `[...]` stay within a path component, `**` crosses
 * them. The last pattern that matches decides, and the files of a deeper directory are decided after a shallower
 * one's. A directory that is not the source is not walked, so nothing under it is taken back. */
typedef struct { char *pat; int neg, dir, anchored; } IgLine;
typedef struct { char *base; size_t nb; IgLine *line; int n; } IgFile;
static IgFile *ig; static int nig;
static void ig_reset(void){ for (int i = 0; i < nig; i++) { for (int j = 0; j < ig[i].n; j++) free(ig[i].line[j].pat); free(ig[i].line); free(ig[i].base); } free(ig); ig = NULL; nig = 0; }
static void ig_load(const char *dir, const char *name){
    char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "r"); if (!f) return;
    ig = xrealloc(ig, sizeof *ig * (size_t)(nig + 1)); IgFile *g = &ig[nig++];
    g->base = strdup(dir); g->nb = strlen(dir); g->line = NULL; g->n = 0;
    for (char *q = g->base; *q; q++) if (*q == '\\') *q = '/';
    char ln[4096];
    while (fgets(ln, sizeof ln, f)) {
        size_t l = strcspn(ln, "\r\n"); ln[l] = 0;
        while (l && (ln[l - 1] == ' ' || ln[l - 1] == '\t') && !(l >= 2 && ln[l - 2] == '\\')) ln[--l] = 0;
        char *s = ln; if (!*s || *s == '#') continue;
        IgLine x = { 0 };
        if (*s == '!') { x.neg = 1; s++; } else if (*s == '\\' && (s[1] == '!' || s[1] == '#')) s++;
        l = strlen(s); if (l && s[l - 1] == '/') { x.dir = 1; s[--l] = 0; }
        if (!l) continue;
        x.anchored = strchr(s, '/') != NULL; if (*s == '/') s++;
        x.pat = strdup(s);
        g->line = xrealloc(g->line, sizeof *g->line * (size_t)(g->n + 1)); g->line[g->n++] = x;
    }
    fclose(f);
}
/* gitignore's wildcards: 0 when the string matches */
static int ig_set(const char **pp, char c){
    const char *p = *pp + 1; int neg = 0, hit = 0;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    for (int first = 1; *p && (*p != ']' || first); first = 0) {
        char lo = *p == '\\' && p[1] ? *++p : *p; p++; char hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') { p++; hi = *p == '\\' && p[1] ? *++p : *p; p++; }
        if (c >= lo && c <= hi) hit = 1;
    }
    if (*p != ']') return -1;
    *pp = p; return hit != neg;
}
static int ig_match(const char *p, const char *s){
    for (; *p; p++, s++) {
        if (*p == '*' && p[1] == '*' && (p[2] == '/' || !p[2])) {
            const char *q = p[2] ? p + 3 : p + 2;
            if (!*q) return 0;                                                 /* trailing **: everything below */
            for (const char *t = s; ; t++) { if ((t == s || t[-1] == '/') && !ig_match(q, t)) return 0; if (!*t) return 1; }
        }
        if (*p == '*') { for (const char *t = s; ; t++) { if (!ig_match(p + 1, t)) return 0; if (!*t || *t == '/') return 1; } }
        if (!*s) return 1;
        if (*p == '?') { if (*s == '/') return 1; continue; }
        if (*p == '[') { const char *q = p; int r = ig_set(&q, *s); if (r < 0) { if (*s != '[') return 1; continue; } if (!r || *s == '/') return 1; p = q; continue; }
        if (*p == '\\' && p[1]) p++;
        if (*p != *s) return 1;
    }
    return *s != 0;
}
static int ignored(const char *path, int is_dir){
    int out = 0;
    for (int i = 0; i < nig; i++) {
        IgFile *g = &ig[i];
        if (strncmp(path, g->base, g->nb) || path[g->nb] != '/') continue;
        const char *rel = path + g->nb + 1, *nm = strrchr(rel, '/'); nm = nm ? nm + 1 : rel;
        for (int j = 0; j < g->n; j++) { IgLine *x = &g->line[j];
            if (x->dir && !is_dir) continue;
            if (!ig_match(x->pat, x->anchored ? rel : nm)) out = !x->neg; }
    }
    return out;
}
static int walk_cb(const char *p, const struct stat *st, int type, struct FTW *fw){
    const char *b = p + fw->base;
    char path[4096]; snprintf(path, sizeof path, "%s", p); for (char *q = path; *q; q++) if (*q == '\\') *q = '/';
    size_t pl = strlen(path); while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
    if (type == FTW_D && b[0] == '.' && fw->level > 0) return FTW_SKIP_SUBTREE;          /* hidden directories */
    const char *ign = walking && walking->ignore[0] ? walking->ignore : NULL;
    if (type == FTW_D) {
        if (ign && fw->level > 0 && ignored(path, 1)) return FTW_SKIP_SUBTREE;
        if (ign) ig_load(path, ign);
        return FTW_CONTINUE;
    }
    if (type == FTW_F && b[0] != '.' && st->st_size > 0 && !(ign && ignored(path, 0))) add_path(p);
    return FTW_CONTINUE;
}
/* What a source is on disk: every file under where it was found, or the files its patterns name, a directory among
 * them walked for every file under it. */
static void walk_dir(const char *dir){ ig_reset(); nftw(dir, walk_cb, 64, FTW_PHYS | FTW_ACTIONRETVAL); ig_reset(); }
static void source_walk(const Source *sc){
    if (!sc->nfiles) { walk_dir(sc->found); return; }
    for (int z = 0; z < sc->nfiles; z++) { glob_t g; if (glob(sc->files[z], 0, NULL, &g)) continue;
        for (size_t y = 0; y < g.gl_pathc; y++) {
            if (os_is_dir(g.gl_pathv[y])) walk_dir(g.gl_pathv[y]); else add_path(g.gl_pathv[y]); }
        globfree(&g); }
}

/* Claims render composed text directly; cache-only references render their
 * list and canonical identifier from the same immutable mapping. */
static int tsv_claims;                                                        /* --claims-tsv: a claim a line, its parts between tabs */
static void put(Buf *o, const void *s, size_t n){ if (o->n + n > o->cap) { o->cap = (o->n + n) * 2 + 16; o->b = xrealloc(o->b, o->cap); } memcpy(o->b + o->n, s, n); o->n += n; }
static void part_text(const lp_id *id, Buf *o){
    Buf t = { 0 }; expand(id, &t);
    if (t.n || !HW) { if (t.n) put(o, t.b, t.n); free(t.b); return; }
    free(t.b);
    int64_t at = lp_highway_slot(HW, NULL, id); const char *ln = "type";
    if (at >= 0) for (size_t i = 0; i < HW->nlists; i++) if ((size_t)at >= HW->list[i].first && (size_t)at < (size_t)HW->list[i].first + HW->list[i].count) ln = HW->list[i].name;
    if (!tsv_claims) { put(o, "<", 1); put(o, ln, strlen(ln)); put(o, ">", 1); return; }
    const lp_list *list=at>=0 ? lp_highway_list(HW,ln) : NULL;
    const char *k=list ? lp_highway_identifier(HW,list,(uint32_t)at-list->first) : NULL;
    put(o, ln, strlen(ln)); put(o, ":", 1);
    if (k) put(o, k, strlen(k));
    else { char h[32]; int hn = snprintf(h, sizeof h, "#%lld", (long long)(at >= 0 ? at - (int64_t)lp_highway_list(HW, ln)->first : -1)); put(o, h, (size_t)hn); }
}
static void tuple_text(const lp_id *id, Buf *o){
    Node *c = table_find(id); if (!c) { expand(id, o); return; }
    int first = 1; put(o, "[", 1);
    for (uint32_t v = 0; v < c->nv; v++) for (uint32_t r = 0; r < VRUN(VTX[c->voff + v].m); r++) {
        if (!first) put(o, ", ", 2); first = 0;
        if (VSAID(VTX[c->voff + v].m) == LP_SAID_TUPLE && table_find(&VTX[c->voff + v].id)) tuple_text(&VTX[c->voff + v].id, o);
        else part_text(&VTX[c->voff + v].id, o); }
    put(o, "]", 1);
}
static void show_tuple(const lp_id *id){ Buf o = { 0 }; tuple_text(id, &o); fwrite(o.b, 1, o.n, stdout); free(o.b); }
/* --claims-tsv: one line a claim (claim) or a tuple a record holds besides (held): kind, position, score, then each part as
 * text (a tuple part bracketed as above); a tab, a line end or a backslash in a part is written \t, \n, \r, \\. Every
 * line of this kind begins with its kind and a tab. A record line includes its
 * native ID and display text, including records which state no claims. */
static void tsv_escaped(const uint8_t *b, size_t n){
    for (size_t i = 0; i < n; i++) switch (b[i]) { case '\t': fputs("\\t", stdout); break; case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break; case '\\': fputs("\\\\", stdout); break; default: putchar(b[i]); }
}
static void tsv_tuple(const char *kind, const lp_id *id, unsigned position, double score){
    Node *c = table_find(id); if (!c) return;
    printf("%s\t%u\t%.6g", kind, position, score);
    for (uint32_t v = 0; v < c->nv; v++) for (uint32_t r = 0; r < VRUN(VTX[c->voff + v].m); r++) {
        Buf o = { 0 };
        if (VSAID(VTX[c->voff + v].m) == LP_SAID_TUPLE && table_find(&VTX[c->voff + v].id)) tuple_text(&VTX[c->voff + v].id, &o);
        else part_text(&VTX[c->voff + v].id, &o);
        putchar('\t'); tsv_escaped(o.b, o.n); free(o.b); }
    putchar('\n');
}
static void tsv_held(const lp_id *id, int depth){
    Node *c = table_find(id); if (!c || depth > 6) return;
    for (uint32_t v = 0; v < c->nv; v++) { uint64_t said = VSAID(VTX[c->voff + v].m);
        if (said == LP_SAID_TUPLE) tsv_tuple("held", &VTX[c->voff + v].id, 0, 1.0);
        else if (said == LP_SAID_RECORD) tsv_held(&VTX[c->voff + v].id, depth + 1); }
}
static void tsv_record(const lp_id *id, unsigned position){
    char hex[33]; Buf display={0}; id_text(id,hex); part_text(id,&display);
    printf("record\t%u\t1\t%s\t",position,hex);
    tsv_escaped(display.b,display.n); putchar('\n'); free(display.b);
}
/* What a record holds besides its claims: the things and specifics said with them, and the records inside it. */
static void show_held(const lp_id *id, int depth){
    Node *c = table_find(id); if (!c || depth > 6) return;
    for (uint32_t v = 0; v < c->nv; v++) { uint64_t said = VSAID(VTX[c->voff + v].m);
        if (said == LP_SAID_TUPLE) { printf("%*s+ ", depth * 2 + 2, ""); show_tuple(&VTX[c->voff + v].id); putchar('\n'); }
        else if (said == LP_SAID_RECORD) { printf("%*s(\n", depth * 2 + 2, ""); show_held(&VTX[c->voff + v].id, depth + 1); printf("%*s)\n", depth * 2 + 2, ""); } }
}
static void show_record_events(const File *file){
    for (uint64_t i = 0; i < file->ev.n; i++) {
        const Event *event = &file->ev.e[i];
        if (tsv_claims) {
            if (event->kind == EV_RECORD) {
                tsv_record(&event->witnessed,event->position);
                tsv_held(&event->witnessed,0);
            } else tsv_tuple("claim",&event->claim,event->position,(double)event->score);
            continue;
        }
        if (event->kind == EV_RECORD) { printf("-- record %u\n", event->position); show_held(&event->witnessed, 0); continue; }
        if (!table_find(&event->claim)) continue;
        show_tuple(&event->claim);
        if (event->position) printf("   #%u", event->position);
        if (event->score != 1.0f) printf("   score %.3g", (double)event->score);
        putchar('\n');
    }
}
#include "ingest_progress.h"
typedef struct {
    const char *conninfo; int threads, show, loading; File *file; Fold *fold; LoadStats *stats;
    uint64_t *events, windows; double database_seconds, *source_seconds;
} StreamLoad;
static int stream_record_batch(void *context, const Ref *things, size_t n, const Ref *metadata, size_t nm, const Events *events){
    StreamLoad *stream = context; File *file = stream->file;
    size_t roots = 0; const Ref *root = NULL;
    if (file->assembly) {
        file_stream_add(file, things, n, metadata, nm, events->n);
        root = file_stream_roots(file, &roots);
    }
    Events previous = file->ev; Ref *said = file->said; uint64_t nsaid = file->nsaid, csaid = file->csaid;
    file->ev = *events; file->said = (Ref *)root; file->nsaid = roots; file->csaid = roots;
    /* Claim events cannot describe records which state no claims. Inspect the
     * actual product roots before this streaming window releases its DAG. */
    if (stream->show && tsv_claims) for (size_t i=0; i<n; i++) tsv_record(&things[i].id,0);
    if (stream->show) show_record_events(file);
    *stream->events += events->n;
    double begin = now(); int result = 0;
    if (stream->loading) {
        fold_events(stream->fold, file, 1);
        result = load(stream->conninfo, stream->threads, file, 1, stream->stats);
    }
    double elapsed = now() - begin; stream->database_seconds += elapsed;
    if (stream->source_seconds) *stream->source_seconds += elapsed;
    file->ev = previous; file->said = said; file->nsaid = nsaid; file->csaid = csaid;
    if (!result) {
        stream->windows++; table_reset();
        progress_file("progress", file);
        if (stream->loading) fprintf(stderr, "\r  %'llu record windows, %'llu entities and %'llu paths persisted   ",
                (unsigned long long)stream->windows, (unsigned long long)stream->stats->ent_rows, (unsigned long long)stream->stats->phy_rows);
        else fprintf(stderr, "\r  %'llu record windows inspected, %'llu claim events reported   ",
                (unsigned long long)stream->windows, (unsigned long long)*stream->events);
    }
    return result;
}
typedef struct {
    const char *conninfo; int threads, show, loading; Fold *fold; LoadStats *stats;
    uint64_t *events; double database_seconds;
} SourceCompletion;
static int source_file_ready(void *context, File *file){
    SourceCompletion *source = context; double start = now();
    if (!file->partial) file_close(file);
    int streamed = file->emit != NULL;
    if (file->emit) {
        Events empty = {0};
        if (file->emit(file->emit_context, NULL, 0, NULL, 0, &empty)) return 1;
        file->emit = NULL; file->emit_context = NULL;
    } else {
        *source->events += file->ev.n;
        if (source->show) show_record_events(file);
        if (source->loading) {
            fold_events(source->fold, file, 1);
            if (load(source->conninfo, source->threads, file, 1, source->stats)) return 1;
        }
    }
    if (source->loading && !streamed) source->database_seconds += now() - start;
    progress_file("file-composed", file);
    if (!file->partial && getenv("LAPLACE_TRUNKS")) { char id[33]; id_text(&file->trunk.id, id); fprintf(stderr, "  trunk %s  %s\n", id, file->path); }
    return 0;
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
    source_walk(sc);
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
    source_walk(sc);
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
 * directory (room.tsv: source, times), one line a source, the last measure kept. It is a measure, not a forecast:
 * what is recorded once is not recorded again, so no later source is held to it. */
static double db_bytes(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo); double b = -1;
    if (PQstatus(pg) == CONNECTION_OK) { PGresult *r = PQexec(pg, "SELECT pg_database_size(current_database())"); if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r)) b = atof(PQgetvalue(r, 0, 0)); PQclear(r); }
    PQfinish(pg); return b;
}
static void room_keep(const char *path, const char *source, double times){
    char tmp[4200]; snprintf(tmp, sizeof tmp, "%s.new", path); FILE *o = fopen(tmp, "w"); if (!o) return;
    FILE *f = fopen(path, "r"); char line[512]; size_t n = strlen(source);
    while (f && fgets(line, sizeof line, f)) if (strncmp(line, source, n) || line[n] != '\t') fputs(line, o);
    if (f) fclose(f); fprintf(o, "%s\t%.3f\n", source, times); fclose(o); os_rename(tmp, path);
}
static int ingest_every(int argc, char **argv, Source *src, int nsrc, Recipe *rec, int nrec, const int *want){
    char self[4096]; ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1); if (sl <= 0) { perror("/proc/self/exe"); return 1; } self[sl] = 0;
    const char *work = getenv("LAPLACE_WORK"); char dir[4096]; snprintf(dir, sizeof dir, "%s/logs/ingest", work && *work ? work : "."); 
    { char cmd[4200]; snprintf(cmd, sizeof cmd, "%s", dir); for (char *c = cmd + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(cmd, 0775); *c = '/'; } mkdir(cmd, 0775); }
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    const char *conninfo = laplace_db(); int loads = 1, plans = 0;
    for (int a = 1; a < argc; a++) { if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[a + 1]; if (!strcmp(argv[a], "--no-load") || !strcmp(argv[a], "--plan") || !strcmp(argv[a], "--claims") || !strcmp(argv[a], "--claims-tsv")) loads = 0; if (!strcmp(argv[a], "--plan")) plans = 1; }
    /* What a source takes is not its bytes times a ratio: content is recorded once, so what an earlier source measured
     * (bytes the database grew over bytes its files hold, room.tsv) falls as what is recorded covers more of what is read.
     * A source is not begun only when the volume is already left with less than a tenth of itself, and that is said. */
    int short_of_room = 0, short_of_order = 0;
    char roomf[4200]; snprintf(roomf, sizeof roomf, "%s/room.tsv", work && *work ? work : ".");
    { int unset = 0;                                                        /* where a source is kept is the environment's to say: nothing begins until it says it */
      for (int i = 0; i < nsrc; i++) if ((!want || want[i]) && src[i].unset[0]) { fprintf(stderr, "source %s: its root names $%s, which the environment does not set (Laplace-Operations laplace.env)\n", src[i].name, src[i].unset); unset++; }
      if (unset) { return 2; } }
    int *wentin = calloc((size_t)nsrc, sizeof(int));                       /* 1: it went in in this run, or has nothing to go in */
    printf("laplace ingest   %s, in order   logs in %s\n\n", want ? "the sources named" : "every source", dir);
    printf("%-4s %-38s %-10s %10s   %s\n", "", "source", "", "seconds", "");
    double T = now(); int failed = 0, absent = 0, held = 0, empty = 0, went = 0;
    for (int i = 0; i < nsrc && !failed; i++) {
        if (want && !want[i]) continue;
        { const char *b = NULL;                                              /* recipes/order: what it comes after went in first, in this run */
          for (int a = 0; a < src[i].nafter && !b; a++) for (int j = 0; j < i; j++) if (!strcmp(src[j].name, src[i].after[a]) && wentin[j] != 1) b = src[j].name;
          if (b) { printf("%-4d %-38s %-10s %10s   it comes after %s, which did not go in\n", i + 1, src[i].name, "not begun", "", b); short_of_order++; continue; } }
        int mine = 0; for (int k = 0; k < nrec; k++) mine += rec[k].source == i; mine += src[i].nreads;
        if (!mine) { printf("%-4d %-38s %-10s %10s   no recipe reads it yet\n", i + 1, src[i].name, "passed", ""); empty++; wentin[i] = 1; continue; }
        if (!src[i].found[0]) { printf("%-4d %-38s %-10s %10s   it is at none of its roots%s%s\n", i + 1, src[i].name, src[i].held[0] ? "held" : "absent", "", src[i].held[0] ? ": " : "", src[i].held); if (src[i].held[0]) held++; else absent++; continue; }
        double bytes_in = (double)source_bytes(&src[i], rec, nrec), before = loads ? db_bytes(conninfo) : -1;
        if (bytes_in <= 0) { printf("%-4d %-38s %-10s %10s   its root holds no files%s%s\n", i + 1, src[i].name, src[i].held[0] ? "held" : "absent", "", src[i].held[0] ? ": " : "", src[i].held); if (src[i].held[0]) held++; else absent++; continue; }
        if (loads) { double have = room_left(conninfo); struct statvfs v; double whole = 0;
            { PGconn *pg = PQconnectdb(conninfo); if (PQstatus(pg) == CONNECTION_OK) { PGresult *r = PQexec(pg, "SHOW data_directory"); if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) && !statvfs(PQgetvalue(r, 0, 0), &v)) whole = (double)v.f_blocks * (double)v.f_frsize / 1e9; PQclear(r); } PQfinish(pg); }
            if (have >= 0 && have < whole / 10) { char why[128]; snprintf(why, sizeof why, "no room: %.0f GB is left of %.0f GB", have, whole);
                printf("%-4d %-38s %-10s %10s   %s\n", i + 1, src[i].name, "not begun", "", why); short_of_room++; continue; } }
        char log[4300]; snprintf(log, sizeof log, "%s/%s.log", dir, src[i].name);
        printf("%-4d %-38s ", i + 1, src[i].name); fflush(stdout);
        double t = now();
#ifdef _WIN32
        /* No fork on Windows: the same child, started as a process of its own with its output in the log. */
        int code;
        { char **av = malloc(sizeof(char *) * (size_t)(argc + 3)); int n = 0; av[n++] = self; av[n++] = "ingest";
          for (int a = 1; a < argc; a++) av[n++] = argv[a];
          av[n++] = (char *)src[i].name; av[n] = NULL;
          code = os_run_logged(self, av, log, "LAPLACE_INGEST_ONE", "1"); free(av);
          if (code < 0) { perror(self); return 1; } }
#else
        pid_t pid = fork();
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
#endif
        char last[256] = ""; { FILE *f = fopen(log, "r"); char line[4096]; while (f && fgets(line, sizeof line, f)) { char *cr = strrchr(line, '\r'); const char *c = cr && cr[1] && cr[1] != '\n' ? cr + 1 : line; while (*c == ' ') c++;
              if (strstr(c, "attestations") || strstr(c, "already recorded") || (code && *c && *c != '\n')) { snprintf(last, sizeof last, "%.200s", c); char *nl = strchr(last, '\n'); if (nl) *nl = 0; } } if (f) fclose(f); }
        printf("%-10s %'10.1f   %s\n", code ? "FAILED" : "in", now() - t, last);
        if (src[i].nfound > 1) printf("%-4s %-38s %-10s %10s   its root matches %d directories: it takes the last by name, %s\n", "", "", "", "", src[i].nfound, src[i].found);
        fflush(stdout);
        if (code) {
            failed = 1; fprintf(stderr, "\n%s did not go in (exit %d); diagnostic: %s\n", src[i].name, code, log);
            FILE *f = fopen(log, "r");
            if (f) {
                /* Keep the actual child error visible in CLI/Actions output. Bound
                 * output even when a source produced a long progress log. */
                char tail[16384]; size_t used, begin = 0;
                if (!fseek(f, 0, SEEK_END)) { long size = ftell(f); if (size > (long)sizeof tail) { fseek(f, -(long)sizeof tail, SEEK_END); begin = 1; } else rewind(f); }
                used = fread(tail, 1, sizeof tail, f); fclose(f);
                if (begin) { while (begin < used && tail[begin] != '\n') begin++; if (begin < used) begin++; }
                fwrite(tail + begin, 1, used - begin, stderr);
                fputc('\n', stderr);
            }
        } else { went++; wentin[i] = 1; }
        if (!code && loads && before >= 0 && bytes_in > 0) { double after = db_bytes(conninfo); if (after > before) room_keep(roomf, src[i].name, (after - before) / bytes_in); }     /* what it took, for the next run's check */
    }
    printf("\n%d sources in, %d absent, %d held, %d without a recipe, %d not begun for want of room, %d not begun because what they come after is not in%s   %'.1f s\n", went, absent, held, empty, short_of_room, short_of_order, failed ? ", and one that failed: the run stops there" : "", now() - T);
    /* each source that went in merged its own stage as it ended (cmd_ingest); one that failed left its stage, and its
     * next run takes up from it (what is staged is looked for as what is recorded is) */
    /* a plan is the check that every source is where its recipe says (Laplace-Engine#60): one that is not fails it */
    free(wentin); return failed || short_of_order || (plans && absent);
}
static const uint64_t *by_size_of;                                    /* the batch's files, longest first, ties in the order given */
static int by_size(const void *a, const void *b){ int i = *(const int *)a, j = *(const int *)b; uint64_t x = by_size_of[i], y = by_size_of[j]; return x < y ? 1 : x > y ? -1 : i - j; }
/* Which files are recorded, before any is read (the inventor: "if you have the file's trunk node (and it's hash metadata
 * matches), you know you have everything in that file already"). A file's metadata tree begins with the OS's record of
 * it, one node computed from stat alone (say.c, file_os); the container index gives what holds that node: the file's
 * trunk, where it is the metadata, or the metadata tree that begins with it, whose container is the trunk. A file found
 * is recorded with everything under it, and is never decomposed. Returns how many were found. */
static int ingest_ewkb_point(const uint8_t *e, size_t n, lp_coord *out){
    if (n < 5 || e[0] != 1) return 0; uint32_t t; memcpy(&t, e + 1, 4); size_t off = 5; if (t & 0x20000000u) off += 4;
    if (n < off + 32) return 0; for (int d = 0; d < 4; d++) { double v; memcpy(&v, e + off + 8 * d, 8); out->m[d] = (int64_t)(v * (double)LP_FIXED_ONE); } return 1;
}
typedef struct { lp_vertex *vertices; Ref *children; uint64_t *runs; size_t capacity; } FileRefScratch;
/* Presence is an ID question. Reuse Native's local reference first; when its
 * children are local, Native can compose the reference without a DB read. */
static int ingest_local_file_ref(const lp_id *id, const uint8_t *path, size_t bytes,
                                 uint8_t tier, FileRefScratch *scratch, Ref *out){
    int known; *out=ref_of_id(id,&known);
    if(known){out->tier=tier;return 1;}
    size_t n=lp_path_vertices(path,bytes,NULL,0);
    if(!n)return 0;
    if(n>scratch->capacity){
        if(n>SIZE_MAX/sizeof(Ref)||n>SIZE_MAX/sizeof(lp_vertex)||n>SIZE_MAX/sizeof(uint64_t))return -1;
        scratch->vertices=xrealloc(scratch->vertices,n*sizeof *scratch->vertices);
        scratch->children=xrealloc(scratch->children,n*sizeof *scratch->children);
        scratch->runs=xrealloc(scratch->runs,n*sizeof *scratch->runs);scratch->capacity=n;
    }
    if(lp_path_vertices(path,bytes,scratch->vertices,n)!=n)return -1;
    for(size_t i=0;i<n;i++){
        scratch->children[i]=ref_of_id(&scratch->vertices[i].id,&known);
        if(!known)return 0;
        scratch->runs[i]=scratch->vertices[i].run;
    }
    if(!lp_ref_compose_runs(scratch->children,scratch->runs,n,tier,out)||!lp_id_eq(id,&out->id))return -1;
    out->tier=tier;return 1;
}
static int files_recorded(const char *conninfo, File *files, int nfiles){
    File *candidate=NULL;int *owner=NULL,ncandidate=0,ccandidate=0,failed=0;
    uint8_t *local=NULL;FileRefScratch scratch={0};
    lp_id *want = malloc(sizeof(lp_id) * (size_t)(nfiles + 1)); int *of = malloc(sizeof(int) * (size_t)(nfiles + 1)), nw = 0, found = 0;
    for (int i = 0; i < nfiles; i++) { if (files[i].skipped || files[i].known) continue; Ref os; if (file_os(files[i].recipe, &files[i], &os)) { want[nw] = os.id; of[nw++] = i; } }
    if(nw){
        /* A recorded parent owns its complete structural closure. An absent
         * metadata ID therefore cannot have a recorded file above it. */
        uint8_t *recorded=db_recorded(conninfo,want,(uint64_t)nw);int kept=0;
        for(int i=0;i<nw;i++)if(recorded[i]){want[kept]=want[i];of[kept++]=of[i];}
        free(recorded);nw=kept;
    }
    PGconn *pg = nw ? db_connect(conninfo) : NULL;
    for (int level = 0; level < 8 && nw; level++) {                          /* a metadata tree factored into blocks is a few levels deep */
        lp_idmap *at = lp_idmap_new(); for (int k = 0; k < nw; k++) { bool fresh; lp_idmap_put(at, &want[k], &fresh); }
        int ncap = nw, nn = 0; lp_id *next = malloc(sizeof(lp_id) * (size_t)ncap); int *nof = malloc(sizeof(int) * (size_t)ncap);   /* an ID can be held by many: grown as they come */
        for (int k0 = 0; k0 < nw && !failed; k0 += 10000) { int k1 = nw - k0 < 10000 ? nw : k0 + 10000; uint8_t *ab = malloc(20 + 20 * (size_t)(k1 - k0)); size_t al = ids_param(ab, want + k0, (uint32_t)(k1 - k0));
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
            PGresult *r = PQexecParams(pg, "SELECT c.entity, st_asewkb(c.path), c.tier"
                " FROM unnest($1::blake3[]) wanted(id) JOIN public.physicality c"
                " ON c.path @> ARRAY[wanted.id]::blake3[]",
                1, NULL, v, l, f, 1);
            if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "which files are recorded: %s", PQerrorMessage(pg)); PQclear(r); free(ab); failed=1; break; }
            for (int j = 0; j < PQntuples(r); j++) {
                lp_vertex vx[2]; size_t nv = lp_path_vertices((const uint8_t *)PQgetvalue(r, j, 1), (size_t)PQgetlength(r, j, 1), vx, 2); if (!nv) continue;
                int64_t k = lp_idmap_find(at, &vx[0].id); if (k < 0) continue;                    /* held, but not first: not its metadata */
                lp_id c; memcpy(c.b, PQgetvalue(r, j, 0), 16);
                if (vx[0].said == LP_SAID_METADATA && nv >= 2) {                                   /* the file's trunk */
                    uint16_t tier; memcpy(&tier, PQgetvalue(r, j, 2), 2); tier = ntohs(tier);
                    if(ncandidate==ccandidate){ccandidate=ccandidate?ccandidate*2:128;candidate=xrealloc(candidate,(size_t)ccandidate*sizeof *candidate);owner=xrealloc(owner,(size_t)ccandidate*sizeof *owner);local=xrealloc(local,(size_t)ccandidate);}
                    owner[ncandidate]=of[k];File *fl=&candidate[ncandidate];*fl=files[of[k]];
                    int have=ingest_local_file_ref(&c,(const uint8_t *)PQgetvalue(r,j,1),(size_t)PQgetlength(r,j,1),(uint8_t)tier,&scratch,&fl->file);
                    if(have<0){fputs("file precheck: stored path disagrees with canonical Native composition\n",stderr);failed=1;break;}
                    local[ncandidate++]=(uint8_t)have;
                    if(!have){memset(&fl->file,0,sizeof fl->file);fl->file.id=c;fl->file.tier=(uint8_t)tier;}
                    fl->file.said = 0;
                    fl->recorded_metadata = vx[0].id;
                    fl->has_file = 1; fl->known = 1; }
                else { if (nn == ncap) { ncap *= 2; next = xrealloc(next, sizeof(lp_id) * (size_t)ncap); nof = xrealloc(nof, sizeof(int) * (size_t)ncap); } next[nn] = c; nof[nn++] = of[k]; } }                                         /* a metadata tree it begins: what holds that is next */
            PQclear(r); free(ab); }
        lp_idmap_free(at); free(want); free(of); want = next; of = nof; nw = 0;
        if (failed) break;
        for (int k = 0; k < nn; k++) if (!files[of[k]].known) { want[nw] = want[k]; of[nw++] = of[k]; }
    }
    free(want); free(of);free(scratch.vertices);free(scratch.children);free(scratch.runs);
    if (failed) { if(pg)PQfinish(pg);free(candidate);free(owner);free(local);return -1; }
    validate_file_contexts(conninfo,candidate,ncandidate);
    /* Only accepted file IDs absent from local composition need a retrieval.
     * This is a bounded-key primary-index lookup, never a join to the estate. */
    typedef struct { lp_coord coord; int have; } StoredFileRef;
    lp_idmap *references=lp_idmap_sized(sizeof(StoredFileRef));
    lp_id *missing=malloc((size_t)(ncandidate?ncandidate:1)*sizeof *missing);int nmissing=0;
    for(int i=0;i<ncandidate;i++)if(candidate[i].known&&!local[i]){
        bool fresh;StoredFileRef *value=lp_idmap_get(references,&candidate[i].file.id,&fresh);
        if(fresh){memset(value,0,sizeof *value);missing[nmissing++]=candidate[i].file.id;}
    }
    /* libpq represents one binary parameter's byte length with a signed int. */
    const int parameter_ids=(INT_MAX-20)/20;
    for(int begin=0;begin<nmissing&&!failed;){
        int count=nmissing-begin;if(count>parameter_ids)count=parameter_ids;
        uint8_t *ab=malloc(20+20*(size_t)count);size_t al=ids_param(ab,missing+begin,(uint32_t)count);
        const char *v[]={ (const char *)ab };int lengths[]={ (int)al },formats[]={1};
        PGresult *r=PQexecParams(pg,"SELECT e.id,st_asewkb(e.coord) FROM public.entity e WHERE e.id=ANY($1::blake3[])",1,NULL,v,lengths,formats,1);
        if(PQresultStatus(r)!=PGRES_TUPLES_OK){fprintf(stderr,"file reference IDs: %s",PQerrorMessage(pg));failed=1;}
        else for(int j=0;j<PQntuples(r);j++){
            lp_id id;memcpy(id.b,PQgetvalue(r,j,0),sizeof id.b);int64_t at=lp_idmap_find(references,&id);
            if(at<0||PQgetisnull(r,j,1)||!ingest_ewkb_point((const uint8_t *)PQgetvalue(r,j,1),(size_t)PQgetlength(r,j,1),&((StoredFileRef *)lp_idmap_at(references,(size_t)at))->coord)){
                fputs("file reference IDs: invalid stored reference\n",stderr);failed=1;break;
            }
            ((StoredFileRef *)lp_idmap_at(references,(size_t)at))->have=1;
        }
        PQclear(r);free(ab);begin+=count;
    }
    for(int i=0;i<ncandidate&&!failed;i++)if(candidate[i].known&&!local[i]){
        int64_t at=lp_idmap_find(references,&candidate[i].file.id);
        StoredFileRef *value=at<0?NULL:lp_idmap_at(references,(size_t)at);
        if(!value||!value->have){fputs("file reference IDs: recorded file has no entity reference\n",stderr);failed=1;break;}
        candidate[i].file.c=value->coord;
    }
    free(missing);lp_idmap_free(references);if(pg)PQfinish(pg);free(local);
    if(failed){free(candidate);free(owner);return -1;}
    uint8_t *stale=calloc((size_t)nfiles,1);if(nfiles && !stale){perror("cached reading contexts");exit(1);}
    for(int i=0;i<ncandidate;i++)if(candidate[i].known && !files[owner[i]].known){
        File *fl=&files[owner[i]];fl->file=candidate[i].file;fl->recorded_metadata=candidate[i].recorded_metadata;fl->known=fl->has_file=1;
    }else if(!candidate[i].known)stale[owner[i]]=1;
    for(int i=0;i<nfiles;i++)if(stale[i] && !files[i].known)fprintf(stderr,"  source reference context changed; reinterpret %s\n",files[i].path);
    free(stale);free(candidate);free(owner);
    found = ingest_known(conninfo, files, nfiles); table_reset();              /* what was composed to ask is composed again for the files that are read */
    return found;
}
static lp_id witness_trunk, witness_lineage; static double witness_trust; static int has_witness_trunk, has_witness_lineage, witness_supersedes;   /* the source's trunk, the witness: its trust and lineage keyed by it */
/* Explicit admission of an existing artifact. Generation and deployment never call this. */
static int ingest_highway(const char *conninfo, const char *t0p, int threads, int do_load){
    double t=now(); tier0_open(t0p);
    if (threads<=0) threads=omp_get_max_threads();
    omp_set_num_threads(threads); table_init(); ctx_open(threads);
    const lp_highway *h=lp_highway_map(NULL);
    if (!h) { fprintf(stderr,"cannot map existing highway at %s; rejected before database access\n",lp_highway_path()); return 1; }
    uint64_t types=0,wrong=0;
    /* The canonical Native UAX tree reconstructs every identifier's closure.
     * No corpus file, private dump, or database is needed for validation. */
    for (size_t li=0; li<h->nlists; li++) {
        const lp_list *list=h->list+li;
        #pragma omp parallel for reduction(+:types,wrong) schedule(dynamic,256)
        for (uint32_t slot=0; slot<list->count; slot++) {
            const lp_tier0_record *expected=lp_highway_at(h,list,slot);
            if (!expected) continue; /* frozen, unassigned tombstone */
            const char *const *identifier; size_t np=lp_highway_identifier_parts(h,list,slot,&identifier);
            if (!np) { wrong++; continue; }
            Ref stack[8],*parts=np<=8 ? stack : malloc(np*sizeof *parts);
            if (!parts) { wrong++; continue; }
            for (size_t i=0; i<np; i++) parts[i]=text_ref(CTX[omp_get_thread_num()],(const uint8_t *)identifier[i],strlen(identifier[i]));
            Ref actual=np==1 ? parts[0] : compose(parts,(uint32_t)np,ref_above(parts,np));
            if (parts!=stack) free(parts);
            types++;
            if (!lp_id_eq(&actual.id,&expected->id) || memcmp(actual.c.m,expected->m,sizeof expected->m)
                || actual.tier!=expected->pad || lp_hilbert4(&actual.c)!=expected->hilbert) wrong++;
        }
    }
    uint64_t n=table_count();
    if (wrong || !types) { fprintf(stderr,"highway rejected before database access: %llu invalid identifiers\n",(unsigned long long)wrong); return 1; }
    if (!do_load) { printf("highway contents validated; no database access: %llu identifiers, %llu compositions\n",(unsigned long long)types,(unsigned long long)n); return 0; }
        extern int load_whole; load_whole = 1; File one = { 0 }; one.path = "the highway's contents"; LoadStats st = { 0 };
        if (load(conninfo, threads, &one, 1, &st)) return 1;
        if (merge(conninfo, threads)) return 1;                  /* what the load staged, into the real tables */
        printf("  %-52s %'llu types, %'llu compositions, %'llu entities new, %'llu not as written%s   %.1f s\n", "the highway's contents, as entities", (unsigned long long)types, (unsigned long long)n, (unsigned long long)st.ent_rows, (unsigned long long)wrong, "", now() - t);
    return 0;
}

int cmd_ingest(int argc, char **argv){
    const char *conninfo = laplace_db(), *t0p = NULL, *rdir = laplace_recipes();
    int highway = 0, threads = 0, do_load = 1, show_claims = 0, entities = 0, asking = 1, catalog = 0, list = 0; const char *of = NULL; extern int load_whole;
    int a = opts(argc, argv, (const Opt[]){ { .flag = "-d", .kind = 's', .at = &conninfo }, { .flag = "-t", .kind = 's', .at = &t0p }, { .flag = "-r", .kind = 's', .at = &rdir }, { .flag = "-j", .kind = 'i', .at = &threads },
        { .flag = "-s", .kind = 's', .at = &of },                                                       /* the files named are this source's: a part of it at a time */
        { .flag = "--highway", .kind = 'b', .at = &highway },
        { .flag = "--catalog", .kind = 'b', .at = &catalog },
        { .flag = "--list-sources", .kind = 'b', .at = &list },
        { .flag = "--whole", .kind = 'b', .at = &load_whole },                                          /* after a run that was cut off: every node is looked for */
        { .flag = "--no-load", .kind = 'v', .at = &do_load, .val = 0 }, { .flag = "--plan", .kind = 'v', .at = &do_load, .val = -1 },
        { .flag = "--claims", .kind = 'b', .at = &show_claims },                                        /* what the recipes attest, as text; nothing is loaded */
        { .flag = "--claims-tsv", .kind = 'b', .at = &tsv_claims },                                     /* --claims for a machine: a claim a line, its parts between tabs */
        { .flag = "--entities", .kind = 'b', .at = &entities },                                         /* what the recipes would record, entities and claims; nothing is loaded */
        { 0 } });
    if (catalog || list) {
        if (a < argc || of || highway || show_claims || tsv_claims || entities || do_load != 1 || (catalog && list)) {
            fputs("Use ingest --catalog or --list-sources with optional -r recipes.\n", stderr); return 2;
        }
        return recipes_catalog(rdir, stdout, catalog);
    }
    if (highway) {
        if (a < argc || of || show_claims || tsv_claims || entities || do_load < 0) { fprintf(stderr, "--highway admits only the existing highway contents; use -d, -t, -j or --no-load\n"); return 2; }
        return ingest_highway(conninfo, t0p, threads, do_load);
    }
    if (tsv_claims && entities) { fprintf(stderr, "--claims-tsv writes claims, not entities\n"); return 2; }
    if (entities || tsv_claims) show_claims = 1;
    if (show_claims) do_load = 0;
    if (a < argc && argv[a][0] == '-') { fprintf(stderr, "usage: laplace ingest [-d conninfo] [-t tier0.bin] [-r recipes] [-j threads] [--no-load] [--plan] [--claims] [--claims-tsv] [--entities] source|file...\n       laplace ingest [--catalog | --list-sources] [-r recipes]\n"); return 2; }
    /* what a recipe would say is shown of files: of nothing named it is no ingest of every source, and --entities, which
     * asks the database of every composition, is of files only, never a source by its name */
    if (show_claims && a >= argc) { fprintf(stderr, "--claims and --entities show what files would record: name the files\n"); return 2; }
    Recipe *rec = NULL; int nrec = recipes_load(rdir, &rec), nsrc; Source *src = sources_loaded(&nsrc);
    if (entities) for (int i = a; i < argc; i++) { struct stat st_; if (stat(argv[i], &st_)) { fprintf(stderr, "--entities shows what files would record: %s is no file (a sample of a source: -s SOURCE FILE)\n", argv[i]); return 2; } }
    for (int k = 0; k < nrec; k++) if (!strcmp(rec[k].name, "file")) file_record_stock(&rec[k]);   /* what the OS keeps of every file: the stock recipe file */
    if (of) { int k = 0; while (k < nsrc && !source_named(&src[k], of)) k++; if (k == nsrc) { fprintf(stderr, "%s is not a source\n", of); return 2; } walking = &src[k]; }
    /* A bare declared source keeps its meaning when the current directory has
     * the same name. Explicit ./ or absolute paths still select file input. */
    { int named = 0; for (int i = a; i < argc; i++) { int k = 0; while (k < nsrc && !source_named(&src[k], argv[i])) k++; named += k < nsrc; }
      if (named && named == argc - a && !of && !getenv("LAPLACE_INGEST_ONE")) {   /* sources by name: each its own run, in recipes/order, after everything it comes after */
          int *want = calloc((size_t)nsrc, sizeof(int)); for (int i = a; i < argc; i++) for (int k = 0; k < nsrc; k++) if (source_named(&src[k], argv[i])) want[k] = 1;
          for (int k = nsrc - 1; k >= 0; k--) if (want[k]) for (int x = 0; x < src[k].nafter; x++) for (int j = 0; j < k; j++) if (!strcmp(src[j].name, src[k].after[x])) want[j] = 1;   /* sources is in order: what one comes after is before it */
          int rc = ingest_every(a, argv, src, nsrc, rec, nrec, want); free(want); return rc; } }
    for (int i = a; i < argc; i++) {                                         /* a source by its name, or files and directories */
        struct stat st; int k = 0; while (k < nsrc && !source_named(&src[k], argv[i])) k++;
        if (k < nsrc) {
            if (src[k].unset[0]) { fprintf(stderr, "source %s: its root names $%s, which the environment does not set (Laplace-Operations laplace.env)\n", src[k].name, src[k].unset); return 2; }
            if (!src[k].found[0]) { fprintf(stderr, "source %s is not at any of its roots\n", src[k].name); return 1; }
            const Source *was = walking; walking = &src[k];
            source_walk(&src[k]);
            walking = was;
        }
        else {                                                               /* a file or directory: under a source's root, it is that source's */
            const Source *was = walking; char real[4096]; if (!walking && realpath(argv[i], real))
                for (int z = 0; z < nsrc; z++) { size_t l = strlen(src[z].found); if (l && !strncmp(real, src[z].found, l) && (real[l] == '/' || !real[l])) { walking = &src[z]; break; } }
            if (!stat(argv[i], &st) && S_ISDIR(st.st_mode)) walk_dir(argv[i]);
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
    if (threads <= 0) threads = omp_get_max_threads();
    omp_set_num_threads(threads); omp_set_max_active_levels(1);
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    double T = now(), t;

    tier0_open(t0p);
    table_init();
    printf("laplace ingest   %d threads   cpu: %s   dispatch: %s   %d recipes\n", threads, lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()), nrec);

    File *files = calloc(nfiles, sizeof(File));
    int uncovered = 0;
    for (int i = 0; i < nfiles; i++) { files[i].path = paths[i]; files[i].recipe = recipe_select(rec, nrec, files[i].path, path_of[i], &files[i].coverage); files[i].source = path_of[i];
                                       if (files[i].recipe) files[i].trust = path_of[i] && files[i].recipe->source < 0 ? path_of[i]->trust : files[i].recipe->trust;
                                       if (!files[i].recipe) {
                                           files[i].skipped = 1; uncovered += files[i].coverage == RECIPE_UNCOVERED;
                                           if (do_load < 0) {
                                               printf("%s\t",files[i].coverage == RECIPE_OMITTED ? "omitted" : "uncovered");
                                               tsv_escaped((const uint8_t *)files[i].path,strlen(files[i].path)); putchar('\n');
                                           }
                                       } }     /* a source takes the files its recipes name */
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
    for (int i = 0; i < nfiles; i++) files[i].order = (uint32_t)i;              /* the order a key defined in two files is kept by */
    if (uncovered) {                                                                  /* what no recipe covers yet, by extension */
        typedef struct { char ext[16]; int n; } Ext; Ext ex[512]; int ne = 0;
        for (int i = 0; i < nfiles; i++) if (files[i].coverage == RECIPE_UNCOVERED) {
            const char *d = strrchr(files[i].path, '.'), *sl = strrchr(files[i].path, '/'); char e[16] = "(none)";
            if (d && (!sl || d > sl) && strlen(d) < sizeof e) snprintf(e, sizeof e, "%s", d);
            int k = 0; while (k < ne && strcmp(ex[k].ext, e)) k++; if (k == ne && ne < 512) { snprintf(ex[ne].ext, 16, "%s", e); ex[ne++].n = 0; } if (k < ne) ex[k].n++;
        }
        for (int x = 0; x < ne; x++) for (int y = x + 1; y < ne; y++) if (ex[y].n > ex[x].n) { Ext tmp = ex[x]; ex[x] = ex[y]; ex[y] = tmp; }
        printf("files no recipe covers yet: %d of %d:", uncovered, nfiles);
        for (int k = 0; k < ne && k < 40; k++) printf(" %s %d", ex[k].ext, ex[k].n); printf("\n");
        printf("recipe coverage is partial: %d unsupported files were not admitted\n",uncovered);
    }
    if (do_load < 0) {                                                                /* the plan: which recipe takes what */
        for (int k = 0; k < nrec; k++) { int n = 0; for (int i = 0; i < nfiles; i++) n += files[i].recipe == &rec[k]; if (n) printf("  %-24s %d files\n", rec[k].name, n); }
        return 0;
    }

    /* Unchanged published files are found from stat metadata before opening
     * their contents. Novel files are subsequently pruned trunk to leaf. */
    int nknown = 0; ctx_open(threads);
    /* Reading a reference depends on the source's provider estate, not only
     * this file's bytes. Stat/reader identities suffice to invalidate that
     * context before any input is opened. Files that never perform a lookup
     * retain their ordinary file-only precheck. */
    lp_id *identities=calloc((size_t)nfiles,sizeof *identities);
    if(nfiles && !identities){perror("source reading context");return 1;}
    for(int i=0;i<nfiles;i++) if(!files[i].skipped){Ref os;if(file_os(files[i].recipe,&files[i],&os))identities[i]=os.id;}
    for(int i=0;i<nfiles;i++) {
        int first=0;while(first<i && files[first].source!=files[i].source)first++;
        if(first<i){memcpy(files[i].reading_context,files[first].reading_context,32);continue;}
        blake3_hasher hash;blake3_hasher_init(&hash);
        for(int j=0;j<nfiles;j++)if(!files[j].skipped && files[j].source==files[i].source)blake3_hasher_update(&hash,identities[j].b,sizeof identities[j].b);
        blake3_hasher_finalize(&hash,files[i].reading_context,32);
    }
    free(identities);
    if (do_load) { double tk = now(); nknown = files_recorded(conninfo, files, nfiles); if (nknown < 0) return 1; if (nknown) fprintf(stderr, "  %d files recorded already, found by the OS's record of them, not read (%.2f s)\n", nknown, now() - tk); }
    int restore_keys = 0;
    for (int i = 0; i < nfiles; i++) if (!files[i].known && !files[i].skipped) { restore_keys = 1; break; }

    /* ---- a batch at a time: decomposed on every core, recomposed and compared, recorded, and the table emptied for
     * the next. A file too long for one batch is read a stretch at a time, if its records can be parted. */
    t = now();
    uint64_t batch = (uint64_t)(getenv("LAPLACE_BATCH_MB") ? atoll(getenv("LAPLACE_BATCH_MB")) : 1024) << 20;
    uint64_t bytes = 0, nev = 0; int done = 0, exact = 0, mism = 0, batches = 0; double t_dec = 0, t_rec = 0; LoadStats st = { 0 };
    Fold *fold = fold_new();                                                  /* cumulative evidence, updated in PostgreSQL as each batch flows */
    StreamLoad *streams = calloc((size_t)nfiles, sizeof *streams);
    if (nfiles && !streams) { perror("source stream loaders"); return 1; }
    SourceCompletion completion = {.conninfo=conninfo, .threads=threads, .show=show_claims, .loading=do_load,
        .fold=fold, .stats=&st, .events=&nev};
    /* Inspection has no persisted backing for projected key products. Only
     * sources without declarations or references can release each window;
     * entity dumps also need their complete requested graph. */
    int preview_streaming = show_claims && !entities;
    for (int i=0; i<nfiles && preview_streaming; i++)
        if (!files[i].skipped && (say_has_keys(files[i].recipe) || say_refers(files[i].recipe,0))) preview_streaming=0;
    say_source_begin();
    st.known += (uint64_t)nknown;                                            /* found by their OS record, never read */
    uint64_t *size = calloc((size_t)nfiles, 8);
    for (int i = 0; i < nfiles; i++) { struct stat sb; if (files[i].known || files[i].skipped || stat(files[i].path, &sb)) continue;
        size_t l = strlen(files[i].path); size[i] = (uint64_t)sb.st_size * (l > 3 && !strcmp(files[i].path + l - 3, ".gz") ? 8 : 1); }
    if (do_load && progress_open(files, nfiles, &st)) return 1;
    #define SHOW(F) do { if (show_claims) show_record_events(F); } while (0)
    /* What was read of a file is kept for its content; a file read to its end gets its trunk. */
    #define WHOLE(F) do { File *f_ = (F); if (!f_->partial) file_close(f_); } while (0)
    for (int a0 = 0; a0 < nfiles; ) {
        int b0 = a0; uint64_t sum = 0;
        int streaming = (do_load || preview_streaming) && !files[a0].known && !files[a0].skipped && recipe_record_streamable(files[a0].recipe);
        if (streaming) {
            b0 = a0 + 1;
            sum = getenv("LAPLACE_RECORD_WINDOW_BYTES") ? strtoull(getenv("LAPLACE_RECORD_WINDOW_BYTES"), NULL, 10) : (UINT64_C(8) << 20);
        } else while (b0 < nfiles && (b0 == a0 || (sum + size[b0] <= batch && depth[b0] == depth[a0] &&
                !((do_load || preview_streaming) && !files[b0].known && !files[b0].skipped && recipe_record_streamable(files[b0].recipe))))) { sum += size[b0]; b0++; }
        table_size(sum);                                                                                                  /* the table is empty here: room for what this batch makes */
        double td = now();
        StreamLoad *stream = &streams[a0];
        *stream = (StreamLoad){.conninfo=conninfo, .threads=threads, .show=show_claims, .loading=do_load, .file=&files[a0],
            .fold=fold, .stats=&st, .events=&nev, .source_seconds=&completion.database_seconds};
        if (streaming) {
            File *file = &files[a0]; file->emit = stream_record_batch; file->emit_context = stream;
            progress_file("file-start", file);
            file_stream_begin(file); decompose_file(CTX[0], file);
            if (!file->partial) { file->emit = NULL; file->emit_context = NULL; }
            bytes += file->bytes; done++;
            t_dec += now() - td - stream->database_seconds;
        } else {
        int ordered = 0;
        for (int i = a0; i < b0; i++) ordered |= say_has_keys(files[i].recipe);
        if (ordered) {
            /* Source-rank declarations must arrive in canonical file order.
             * Each file's grammar records still run on all worker lanes. */
            for (int i = a0; i < b0; i++) {
                if (restore_keys && files[i].known && say_has_keys(files[i].recipe)) {
                    if (load_file_keys(conninfo, &files[i])) return 1;
                }
                if (files[i].known || files[i].skipped) continue;
                progress_file("file-start", &files[i]);
                decompose_file(CTX[0], &files[i]); bytes += files[i].bytes; done++;
            }
            t_dec += now() - td;
        } else {
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
                progress_file("file-start", &files[i]);
                decompose_file(CTX[omp_get_thread_num()], &files[i]);
                #pragma omp critical
                {
                    bytes += files[i].bytes; done++;
                    fprintf(stderr, "\r  %d/%d files  %.1f MB  %.1f MB/s  %'llu nodes   ", done, nfiles - nknown, bytes / 1e6, bytes / 1e6 / (now() - t), (unsigned long long)table_count());
                }
            }
        }
        free(order); t_dec += now() - td;
        }
        }
        td = now();
        /* The provider compared with the original buffer before releasing it.
         * Verification never opens and reads the source a second time. */
        for (int i = a0; i < b0; i++) {
            if (files[i].exact < 0) { mism++; continue; }
            if (files[i].known || files[i].skipped) continue;
            if (files[i].exact) exact++;
            else { mism++; fprintf(stderr, "  %s: does not recompose\n", files[i].path); }
            if (!files[i].partial && getenv("LAPLACE_TRUNKS")) { char id[33]; id_text(&files[i].trunk.id, id); fprintf(stderr, "  trunk %s  %s\n", id, files[i].path); }
        }
        t_rec += now() - td;
        if (mism) { fputs("ingest: source verification failed; no witness is published\n", stderr); return 1; }
        for (int i = a0; i < b0; i++) { if (!files[i].known && !files[i].skipped) WHOLE(&files[i]); nev += files[i].ev.n; SHOW(&files[i]); }
        if (entities) show_entities(conninfo, asking);
        if (do_load) {
            ingest_known(conninfo, files + a0, b0 - a0);
            fold_events(fold, files + a0, b0 - a0);
            if (load(conninfo, threads, files + a0, b0 - a0, &st)) return 1;
        }
        for (int i = a0; i < b0; i++) progress_file("file-composed", &files[i]);
        progress_file("progress", &files[a0]);
        for (int i = a0; i < b0; i++) if (files[i].partial) { free(files[i].ev.e); files[i].ev = (Events){0}; }
        td = now(); double database_before = completion.database_seconds;
        if (say_source_poll(source_file_ready, &completion)) return 1;
        t_dec += now() - td - (completion.database_seconds - database_before);
        batches++;
        if (b0 < nfiles) {
            for (int i = a0; i < b0; i++) { free(files[i].ev.e); memset(&files[i].ev, 0, sizeof files[i].ev); }
            /* No-load realization has no persisted backing for native key
             * products. Keep its source DAG until all continuations finish. */
            if (do_load) table_reset();
        }
        a0 = b0;
    }
    double source_finish = now(), database_before = completion.database_seconds;
    if (say_source_end(source_file_ready, &completion)) return 1;
    t_dec += now() - source_finish - (completion.database_seconds - database_before);
    free(streams);
    #undef SHOW
    #undef WHOLE
    fputc('\n', stderr);
    extern uint64_t table_total(void), table_hits(void);
    printf("\n== decomposition: %d files, %.1f MB, content recomposed byte for byte or curated %d, mismatched %d%s\n", nfiles, bytes / 1e6, exact, mism, batches > 1 ? ", a batch at a time" : "");
    if (do_load) printf("   files whose trunk was already recorded, which say nothing again: %'llu\n", (unsigned long long)st.known);
    printf("   %'llu compositions, %'llu reused; %'llu claims said by records\n",(unsigned long long)table_total(), (unsigned long long)table_hits(), (unsigned long long)nev);
    { uint64_t inc = 0; for (int i = 0; i < nfiles; i++) inc += files[i].incomplete;
      if (inc) printf("   %'llu parts were not read whole: what parses is recorded, and what they attest is incomplete (see above); the source still goes in\n", (unsigned long long)inc); }
    say_obligations(stdout);                                                 /* what the recipes left unresolved: no claim of it was made */
    printf("\n== phases\n");
    printf("  %-44s %8.2f s   %8.1f MB/s\n", "decompose (all threads)", t_dec, bytes / 1e6 / (t_dec > 0 ? t_dec : 1));
    printf("  %-44s %8.2f s\n", "recompose and compare", t_rec);
    if (!do_load) { fold_free(fold); return mism ? 1 : 0; }
    printf("  %-44s %8.2f s   %'llu IDs checked in %llu rounds, %'llu subtrees already recorded\n", "deduplication, trunk to leaf", st.t_dedup,
           (unsigned long long)st.checked, (unsigned long long)st.rounds, (unsigned long long)st.found);
    printf("  %-44s %8.2f s   %'llu entities, %'llu paths (%'.0f rows/s)\n", "COPY into every partition", st.t_copy,
           (unsigned long long)st.ent_rows, (unsigned long long)st.phy_rows, (st.ent_rows + st.phy_rows) / (st.t_copy > 0 ? st.t_copy : 1));
    { const Source *one = NULL; int many = 0; Ref trunk;                    /* one source, every file of it recorded: its trunk, last of all */
      for (int i = 0; i < nfiles; i++) { if (files[i].skipped) continue; if (!files[i].source) { many = 1; break; } if (!one) one = files[i].source; else if (one != files[i].source) many = 1; }
      /* The witness is the source's trunk (Semantics: Attestations, Witnesses): its trust and lineage are the source's,
       * keyed by it, and every claim the source says plays at that trust. A recipe that names a class or lineage of its
       * own under the source is said, and not used: a trunk has one of each. */
      double trust = one && !many ? one->trust : 0; int differs = 0;
      for (int i = 0; i < nfiles; i++) if (!files[i].skipped && !files[i].known) { if (!one || many) { trust = files[i].trust; break; } differs += files[i].trust != one->trust || files[i].has_lineage != !!one->lineage[0]; }
      if (differs && do_load > 0) printf("  %d files are of recipes naming a class or lineage of their own: the witness is the source's trunk, and its are the source's (trust %.3f)\n", differs, trust);
      { extern uint64_t records_differ(void), say_unplaced(void); uint64_t rd = records_differ(), un = say_unplaced();
        if (rd) printf("  %'llu records met again said otherwise (another outcome or place for a claim): the first is kept\n", (unsigned long long)rd);
        if (un) printf("  %'llu claims could be put in no record: their provenance is lost\n", (unsigned long long)un); }
      fold_free(fold); fold = NULL;
      if (one && !many && !mism && !of) { table_reset(); table_size(64u << 20);   /* the last batch is written: an empty table for the trunk alone */
          if (source_trunk(one, files, nfiles, &trunk)) { File sf; memset(&sf, 0, sizeof sf); sf.path = one->name; sf.source = one; sf.trunk = trunk; sf.file = trunk; sf.has_file = 1;
              LoadStats ss = { 0 }; double ts = now(); if (load(conninfo, threads, &sf, 1, &ss)) return 1;
              printf("  %-44s %8.2f s   %s%s\n", "the source's trunk", now() - ts, source_called(one), ss.ent_rows ? "" : ": already recorded");
              st.ent_rows += ss.ent_rows; st.phy_rows += ss.phy_rows;
              { char hx[33]; id_text(&trunk.id, hx); printf("  %-44s %s\n", "its ID, the witness", hx); }
              witness_trunk = trunk.id; witness_trust = trust; has_witness_trunk = 1; witness_supersedes = one->supersede;
              for (int i = 0; i < nfiles && !has_witness_lineage; i++) if (!files[i].skipped && files[i].has_lineage) { witness_lineage = files[i].lineage.id; has_witness_lineage = 1; } }
          else printf("  the source's trunk: none, since its source file names no record (witness or called): what it says is held by its files' trunks, and no witness holds them\n");
          table_reset(); }
      else if (do_load > 0 && nev) printf("  no source's trunk (%s): what these files say is held by their trunks, and no witness holds them\n", of ? "a part of a source" : many ? "files of several sources, or of none" : "a file did not recompose"); }
    if (do_load > 0 && !mism) {
        LoadWitness *roots = calloc((size_t)nfiles + 1, sizeof *roots); size_t nr = 0;
        if (has_witness_trunk) roots[nr++] = (LoadWitness){.id=witness_trunk, .lineage=witness_lineage,
            .trust=witness_trust, .has_lineage=has_witness_lineage};
        else if (!of) {
            lp_idmap *seen = lp_idmap_new();
            for (int i=0; i<nfiles; i++) {
                if (files[i].skipped || files[i].source || !files[i].has_file) continue;
                bool fresh; lp_idmap_put(seen, &files[i].file.id, &fresh); if (!fresh) continue;
                roots[nr++] = (LoadWitness){.id=files[i].file.id, .lineage=files[i].lineage.id,
                    .trust=files[i].trust, .has_lineage=files[i].has_lineage};
            }
            lp_idmap_free(seen);
        }
        if (load_register(conninfo, threads, roots, nr) || merge(conninfo, threads)) { free(roots); return 1; }
        free(roots);
        if (has_witness_trunk && witness_supersedes && forget_superseded(conninfo, threads, &witness_trunk)) return 1;
    }
    printf("\n== total %.1f s\n", now() - T);
    progress_close(has_witness_trunk ? &witness_trunk : NULL);
    return mism ? 1 : 0;
}
