#define _GNU_SOURCE
/* laplace ingest: files through their recipes.
 *   laplace ingest [-d conninfo] [-t tier0.bin] [-r recipes/] [-j threads] [-s source] [--whole] [--no-load] [--plan] file...
 *   laplace ingest [options]          with nothing named: every source, in the order they go in
 * Files decompose on every core; each is recomposed from the node table and compared with its bytes. A file is a
 * trunk in the DAG, over its metadata and its content (file.c); a file whose trunk is recorded already is recorded,
 * and nothing of it is written again. Then new nodes are written, and what was attested is played and recorded. Live counters go to stderr, phase times to stdout. */
#include "ingest_internal.h"
#include "chess_cache.h"
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include "ingest_progress.h"
/* A file is a semantic object, not a persistence boundary. Keep the ordinary
 * grammar stream and file assembly, but share their Native working window
 * across files. The caller's resource plan supplies a resident-byte budget;
 * the standalone CLI retains its existing batch budget as a fallback. */
typedef struct {
    const char *conninfo; int threads; Fold *fold; LoadStats *stats;
    uint64_t budget, flushes;
    Ref *roots; size_t nroots, croots;
    Events events;
    File **complete; size_t ncomplete, ccomplete;
} StreamWindow;
static int stream_window_flush(StreamWindow *window){
    if (!window->nroots && !window->events.n && !window->ncomplete) return 0;
    File batch = {.said=window->roots, .nsaid=window->nroots, .ev=window->events};
    if (window->ncomplete) {                                          /* recorded trunks, asked once for the window before it loads */
        lp_id *ids = malloc(window->ncomplete * sizeof *ids); size_t n = 0;
        if (!ids) { perror("recorded file trunks"); return 1; }
        for (size_t i=0; i<window->ncomplete; i++) if (window->complete[i]->has_file) ids[n++] = window->complete[i]->file.id;
        if (n) { uint8_t *recorded = db_recorded(window->conninfo, ids, (uint64_t)n);
            for (size_t i=0; i<n; i++) window->stats->known += recorded[i] != 0;
            free(recorded); }
        free(ids);
    }
    if (load(window->conninfo, window->threads, &batch, 1, window->stats)) return 1;
    /* A composed-file receipt means its Native closure has been committed,
     * even when other files shared that apply. Never report it early. */
    for (size_t i=0; i<window->ncomplete; i++) progress_file("file-composed", window->complete[i]);
    window->nroots=window->events.n=window->ncomplete=0;
    uint64_t buffered=window->croots*sizeof *window->roots + window->events.cap*sizeof *window->events.e
        + window->ccomplete*sizeof *window->complete;
    if (buffered >= window->budget) {
        free(window->roots); free(window->events.e); free(window->complete);
        window->roots=NULL; window->croots=0; window->events=(Events){0}; window->complete=NULL; window->ccomplete=0;
    }
    window->flushes++; table_reset(); return 0;
}
static void stream_window_root(StreamWindow *window, const Ref *root){
    static const lp_id absent={{0}};
    if (lp_id_eq(&root->id,&absent)) return;
    table_annotate_roots(root, 1);
    if (window->nroots == window->croots) {
        window->croots = window->croots ? window->croots * 2 : 1;
        window->roots = xrealloc(window->roots, window->croots * sizeof *window->roots);
    }
    window->roots[window->nroots++] = *root;
}
static int stream_window_add(StreamWindow *window, File *file){
    if (file->known || file->skipped) return 0;
    fold_events(window->fold, file, 1);
    for (uint64_t i=0; i<file->ev.n; i++) ev_push(&window->events, &file->ev.e[i]);
    if (file->has_file) stream_window_root(window, &file->file);
    else {
        stream_window_root(window, &file->trunk);
        for (uint64_t i=0; i<file->nsaid; i++) stream_window_root(window, &file->said[i]);
    }
    if (file->has_lineage) stream_window_root(window, &file->lineage);
    if (!file->partial && !file->incomplete && !file->invalid) {
        if (window->ncomplete == window->ccomplete) {
            window->ccomplete = window->ccomplete ? window->ccomplete * 2 : 1;
            window->complete = xrealloc(window->complete, window->ccomplete * sizeof *window->complete);
        }
        window->complete[window->ncomplete++] = file;
    }
    uint64_t resident = table_resident_bytes() + window->croots * sizeof *window->roots
        + window->events.cap * sizeof *window->events.e + window->ccomplete * sizeof *window->complete;
    return resident >= window->budget ? stream_window_flush(window) : 0;
}
typedef struct {
    const char *conninfo; int threads, show, loading; File *file; Fold *fold; LoadStats *stats;
    uint64_t *events; double database_seconds, *source_seconds; StreamWindow *window;
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
        result = stream_window_add(stream->window, file);
    }
    double elapsed = now() - begin; stream->database_seconds += elapsed;
    if (stream->source_seconds) *stream->source_seconds += elapsed;
    file->ev = previous; file->said = said; file->nsaid = nsaid; file->csaid = csaid;
    if (!result) {
        if (!stream->loading) table_reset();
        progress_file("progress", file);
        const char *name=strrchr(file->path,'/');name=name?name+1:file->path;
        if (progress_console() && stream->loading) fprintf(stderr, "\r  %s: %'llu records read; %'llu new entities, %'llu new paths committed   ",
                name,(unsigned long long)file->records,(unsigned long long)stream->stats->ent_rows,(unsigned long long)stream->stats->phy_rows);
        else if(progress_console()) fprintf(stderr, "\r  %s: %'llu records inspected   ",name,(unsigned long long)file->records);
    }
    return result;
}
typedef struct {
    const char *conninfo; int threads, show, loading; Fold *fold; LoadStats *stats;
    uint64_t *events; double database_seconds; StreamWindow *window;
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
            if (stream_window_add(source->window, file)) return 1;
        }
    }
    if (source->loading && !streamed) source->database_seconds += now() - start;
    if (!source->loading) progress_file("file-composed", file);
    if (!file->partial && getenv("LAPLACE_TRUNKS")) { char id[33]; id_text(&file->trunk.id, id); fprintf(stderr, "  trunk %s  %s\n", id, file->path); }
    return 0;
}
static const uint64_t *by_size_of;                                    /* the batch's files, longest first, ties in the order given */
static int by_size(const void *a, const void *b){ int i = *(const int *)a, j = *(const int *)b; uint64_t x = by_size_of[i], y = by_size_of[j]; return x < y ? 1 : x > y ? -1 : i - j; }
static size_t file_dependency(const File *file){return file->recipe?file->recipe->dependency_level:0;}
static int file_reading_order(const void *a,const void *b){
    const File *x=a,*y=b;size_t xd=file_dependency(x),yd=file_dependency(y);
    if(xd!=yd)return xd<yd?-1:1;
    if(x->trust!=y->trust)return x->trust>y->trust?-1:1;
    return x->order<y->order?-1:x->order>y->order;
}
static lp_id witness_trunk, witness_lineage; static double witness_trust; static int has_witness_trunk, has_witness_lineage, witness_supersedes;   /* the source's trunk, the witness: its trust and lineage keyed by it */
/* ---- laplace witness: what a witness named at run time states (feedback on an answer, an attested fact, an outcome, a
 * measured cost), recorded as a source is. The witness is no directory of recipes: its name, its trust class and its
 * lineage are given, and the files are what it states, each read by the recipe any file of its kind is read by (a
 * .statement by recipes/statement.recipe: a claim a line). Everything else is cmd_ingest, unchanged: the files
 * decompose through the record VM, the trunk is [the witness's name, its files' trunks in path order], and that trunk is
 * the witness its attestations are keyed by, at its class's prior. A file's pathname is from the directory its files
 * share, or from --root DIR, which every file is under: an application that keeps what a witness states at its own
 * relative paths (an upload's folders) has those paths recorded as the files' pathnames. */
typedef struct { const char *as, *class_label, *lineage; char root[4096]; } RuntimeWitness;
static const RuntimeWitness *runtime_witness;
int cmd_witness(int argc, char **argv){
    const char *conninfo = NULL, *t0p = NULL, *rdir = NULL, *threads = NULL, *root = NULL; int no_load = 0, claims = 0;
    RuntimeWitness w = {0};
    int a = opts(argc, argv, (const Opt[]){ { .flag = "-d", .kind = 's', .at = &conninfo }, { .flag = "-t", .kind = 's', .at = &t0p }, { .flag = "-r", .kind = 's', .at = &rdir }, { .flag = "-j", .kind = 's', .at = &threads },
        { .flag = "--as", .kind = 's', .at = &w.as }, { .flag = "--class", .kind = 's', .at = &w.class_label }, { .flag = "--lineage", .kind = 's', .at = &w.lineage },
        { .flag = "--root", .kind = 's', .at = &root },
        { .flag = "--no-load", .kind = 'b', .at = &no_load }, { .flag = "--claims", .kind = 'b', .at = &claims }, { 0 } });
    if (a >= argc || argv[a][0] == '-' || !w.as || !*w.as || !w.class_label || !*w.class_label) {
        fputs("usage: laplace witness [-d conninfo] [-t tier0.bin] [-r recipes] [-j threads] --as WITNESS --class CLASS [--lineage NAME] [--root DIR] [--no-load] [--claims] file...\n", stderr); return 2; }
    if (root) {                                                               /* the directory the files' pathnames are from, as the OS resolves it */
        struct stat st; char real[4096];
        if (!realpath(root, real) || stat(real, &st) || !S_ISDIR(st.st_mode) || strlen(real) >= sizeof w.root) { fprintf(stderr, "witness: --root %s is not a directory\n", root); return 2; }
        for (char *q = real; *q; q++) if (*q == '\\') *q = '/';
        size_t k = strlen(real); while (k > 1 && real[k - 1] == '/') real[--k] = 0;
        memcpy(w.root, real, k + 1);
    }
    if (strlen(w.as) >= sizeof ((Source *)0)->witness || strchr(w.as, '{') || strchr(w.as, '\n') || (w.lineage && (strlen(w.lineage) >= sizeof ((Source *)0)->lineage || strchr(w.lineage, '\n')))) {
        fputs("witness: a witness and a lineage are names of at most 127 bytes on one line, a witness without {\n", stderr); return 2; }
    if (!w.lineage) w.lineage = w.as;                                          /* what one witness states at different times is one lineage: its repeats are no independent consensus */
    /* The files as the OS resolves them, and the directory they share: a file's pathname is from there, never the host's. */
    char **av = calloc((size_t)argc + 12, sizeof *av); int n = 0; if (!av) { perror("witness"); return 1; }
    av[n++] = "ingest";
    if (conninfo) { av[n++] = "-d"; av[n++] = (char *)conninfo; }
    if (t0p) { av[n++] = "-t"; av[n++] = (char *)t0p; }
    if (rdir) { av[n++] = "-r"; av[n++] = (char *)rdir; }
    if (threads) { av[n++] = "-j"; av[n++] = (char *)threads; }
    if (no_load) av[n++] = "--no-load";
    if (claims) av[n++] = "--claims";
    for (int i = a; i < argc; i++) {
        struct stat st; char *real = malloc(4096);
        if (!real || !realpath(argv[i], real) || stat(real, &st) || !S_ISREG(st.st_mode)) { fprintf(stderr, "witness: %s is not a file\n", argv[i]); return 2; }
        for (char *q = real; *q; q++) if (*q == '\\') *q = '/';
        if (root) {                                                           /* a file is under the root it is named from, or it is refused */
            size_t rl = strlen(w.root);
            if (strncmp(real, w.root, rl) || real[rl] != '/') { fprintf(stderr, "witness: %s is not under --root %s\n", argv[i], w.root); return 2; }
            av[n++] = real; continue;
        }
        char *slash = strrchr(real, '/'); size_t dir = slash ? (size_t)(slash - real) : 0;
        if (i == a) { memcpy(w.root, real, dir); w.root[dir] = 0; }
        else { size_t k = 0; while (w.root[k] && k < dir && w.root[k] == real[k]) k++;
               if (w.root[k] || (real[k] != '/' && k != dir)) { while (k && w.root[k - 1] != '/') k--; if (k) k--; }
               w.root[k] = 0; }
        av[n++] = real;
    }
    runtime_witness = &w; int rc = cmd_ingest(n, av); runtime_witness = NULL;
    return rc;
}
static const char *ingesting;
const char *ingest_database(void){ return ingesting ? ingesting : laplace_db(); }
int cmd_ingest(int argc, char **argv){
    const char *conninfo = laplace_db(), *t0p = NULL, *rdir = laplace_recipes();
    int highway = 0, threads = 0, do_load = 1, show_claims = 0, entities = 0, catalog = 0, list = 0; const char *of = NULL; extern int load_whole;
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
    ingesting = conninfo;
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
    /* An explicit no-load preview is entirely local. Plain --entities may
     * annotate the preview with database presence, but must not override it. */
    int asking = do_load > 0;
    if (entities || tsv_claims) show_claims = 1;
    if (show_claims) do_load = 0;
    if (a < argc && argv[a][0] == '-') { fprintf(stderr, "usage: laplace ingest [-d conninfo] [-t tier0.bin] [-r recipes] [-j threads] [--no-load] [--plan] [--claims] [--claims-tsv] [--entities] source|file...\n       laplace ingest [--catalog | --list-sources] [-r recipes]\n"); return 2; }
    /* what a recipe would say is shown of files: of nothing named it is no ingest of every source, and --entities, which
     * asks the database of every composition, is of files only, never a source by its name */
    if (show_claims && a >= argc) { fprintf(stderr, "--claims and --entities show what files would record: name the files\n"); return 2; }
    Recipe *rec = NULL; int nrec = recipes_load(rdir, &rec), nsrc; Source *src = sources_loaded(&nsrc);
    if (runtime_witness) {                                                    /* the files named are the witness's, and no name among them is a source's */
        walking = source_runtime(runtime_witness->as, runtime_witness->class_label, runtime_witness->lineage, runtime_witness->root);
        src = sources_loaded(&nsrc); nsrc = 0;
    }
    if (entities) for (int i = a; i < argc; i++) { struct stat st_; if (stat(argv[i], &st_)) { fprintf(stderr, "--entities shows what files would record: %s is no file (a sample of a source: -s SOURCE FILE)\n", argv[i]); return 2; } }
    /* Validate the complete selection before a named source can open a load.
     * A misspelled source is not an empty, successfully ingested file. */
    for (int i=a; i<argc; i++) {
        int k=0; while(k<nsrc && !source_named(&src[k],argv[i])) k++;
        if(k<nsrc) continue;
        struct stat input;
        if(stat(argv[i],&input)) {
            fprintf(stderr,"ingest: %s is neither a declared source nor an accessible file or directory: %s\n",argv[i],strerror(errno));
            return 2;
        }
        if(!S_ISREG(input.st_mode) && !S_ISDIR(input.st_mode)) {
            fprintf(stderr,"ingest: %s is not a regular file or directory\n",argv[i]);
            return 2;
        }
    }
    for (int k = 0; k < nrec; k++) if (!strcmp(rec[k].name, "file")) file_record_stock(&rec[k]);   /* what the OS keeps of every file: the stock recipe file */
    if (of) {
        int k = 0; while (k < nsrc && !source_named(&src[k], of)) k++;
        if (k == nsrc) { fprintf(stderr, "%s is not a source\n", of); return 2; }
        walking = &src[k];
        /* The registered root is the complete source, whether an operator names
         * it or supplies that same directory to an installed client. Only a
         * genuine slice suppresses the corpus witness. Preserve explicit file
         * selection for claim inspection, which requires file input. */
        char selected[4096], declared[4096];
        if (!show_claims && a + 1 == argc && src[k].found[0]
            && realpath(argv[a], selected) && realpath(src[k].found, declared)
            && !strcmp(selected, declared)) {
            argv[a] = src[k].name; of = NULL; walking = NULL;
        }
    }
    /* A bare declared source keeps its meaning when the current directory has
     * the same name. Explicit ./ or absolute paths still select file input. */
    { int named = 0; for (int i = a; i < argc; i++) { int k = 0; while (k < nsrc && !source_named(&src[k], argv[i])) k++; named += k < nsrc; }
      if (named && named == argc - a && !of && !getenv("LAPLACE_INGEST_ONE")) {   /* sources by name: each its own run, in recipes/order, after everything it comes after */
          int *want = calloc((size_t)nsrc, sizeof(int)); for (int i = a; i < argc; i++) for (int k = 0; k < nsrc; k++) if (source_named(&src[k], argv[i])) want[k] = 1;
          for (int k = nsrc - 1; k >= 0; k--) if (want[k]) for (int x = 0; x < src[k].nafter; x++) for (int j = 0; j < k; j++) if (!strcmp(src[j].name, src[k].after[x]) && !want[j]) want[j] = 2;   /* sources is in order: what one comes after is before it; 2: wanted only as what another comes after (ingest_every passes it over when it is recorded) */
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
    for (int i = 0; i < nfiles; i++) { files[i].path = paths[i]; files[i].recipe = recipe_select(rec, nrec, files[i].path, path_of[i], &files[i].coverage); files[i].source = path_of[i]; files[i].order=(uint32_t)i;
                                       if (files[i].recipe) files[i].trust = path_of[i] && files[i].recipe->source < 0 ? path_of[i]->trust : files[i].recipe->trust;
                                       if (!files[i].recipe) {
                                           files[i].skipped = 1; uncovered += files[i].coverage == RECIPE_UNCOVERED;
                                           if (do_load < 0) {
                                               printf("%s\t",files[i].coverage == RECIPE_OMITTED ? "omitted" : "uncovered");
                                               tsv_escaped((const uint8_t *)files[i].path,strlen(files[i].path)); putchar('\n');
                                           }
                                       } }     /* a source takes the files its recipes name */
    /* Providers first, then stated witness trust, then original declaration
     * order. One stable ordering replaces per-file recursive graph walks and
     * two quadratic insertion sorts. Cyclic recipe groups share a level. */
    qsort(files,(size_t)nfiles,sizeof *files,file_reading_order);
    size_t *depth=calloc((size_t)nfiles,sizeof *depth);
    for(int i=0;i<nfiles;i++)depth[i]=file_dependency(files+i);
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

    /* Every file is read: a file is known by its trunk, which is content alone, so an unchanged file is found only once it is
     * decomposed, and pruned there trunk to leaf with everything beneath it (files_known). */
    fflush(stdout); /* Keep catalog/coverage output before any worker's failure in redirected logs. */
    ctx_open(threads);

    /* ---- a batch at a time: decomposed on every core, recomposed and compared, recorded, and the table emptied for
     * the next. A file too long for one batch is read a stretch at a time, if its records can be parted. */
    t = now();
    uint64_t batch = (uint64_t)(getenv("LAPLACE_BATCH_MB") ? atoll(getenv("LAPLACE_BATCH_MB")) : 1024) << 20;
    uint64_t bytes = 0, nev = 0; int done = 0, exact = 0, mism = 0, batches = 0; double t_dec = 0, t_rec = 0; LoadStats st = { 0 };
    Fold *fold = fold_new();                                                  /* cumulative evidence, updated in PostgreSQL as each batch flows */
    uint64_t working_bytes = batch;
    const char *working_setting = getenv("LAPLACE_WORKING_SET_BYTES");
    if (working_setting && *working_setting) {
        char *end; errno=0; unsigned long long value=strtoull(working_setting,&end,10);
        if (errno || *end || *working_setting=='-' || !value) { fputs("LAPLACE_WORKING_SET_BYTES must be a positive byte budget\n",stderr); return 2; }
        working_bytes=(uint64_t)value;
    }
    StreamWindow window = {.conninfo=conninfo, .threads=threads, .fold=fold, .stats=&st, .budget=working_bytes};
    StreamLoad *streams = calloc((size_t)nfiles, sizeof *streams);
    if (nfiles && !streams) { perror("source stream loaders"); return 1; }
    SourceCompletion completion = {.conninfo=conninfo, .threads=threads, .show=show_claims, .loading=do_load,
        .fold=fold, .stats=&st, .events=&nev, .window=&window};
    /* Inspection has no persisted backing for projected key products. Only
     * sources without declarations or references can release each window;
     * entity dumps also need their complete requested graph. */
    int preview_streaming = show_claims && !entities;
    for (int i=0; i<nfiles && preview_streaming; i++)
        if (!files[i].skipped && (say_has_keys(files[i].recipe) || say_refers(files[i].recipe,0))) preview_streaming=0;
    say_source_begin();
    uint64_t *size = calloc((size_t)nfiles, 8);
    for (int i = 0; i < nfiles; i++) { struct stat sb; if (files[i].known || files[i].skipped || stat(files[i].path, &sb)) continue;
        size_t l = strlen(files[i].path); size[i] = (uint64_t)sb.st_size * (l > 3 && !strcmp(files[i].path + l - 3, ".gz") ? 8 : 1); }
    if (progress_open(files, nfiles, &st)) return 1;
    if(do_load){load_begin(conninfo,threads);chess_cache_database(conninfo);}   /* what of the chess perf-cache it holds: asked once, when chess first wants it */
    #define SHOW(F) do { if (show_claims) show_record_events(F); } while (0)
    /* What was read of a file is kept for its content; a file read to its end gets its trunk. */
    #define WHOLE(F) do { File *f_ = (F); if (!f_->partial) file_close(f_); } while (0)
    /* Keep the recorded-key reader and its Native backend caches alive for
     * this source. Opening a backend for each known file discards those
     * caches and repeats connection/library initialization on every replay. */
    for (int a0 = 0; a0 < nfiles; ) {
        int b0 = a0; uint64_t sum = 0;
        int streaming = (do_load || preview_streaming) && !files[a0].known && !files[a0].skipped && recipe_record_streamable(files[a0].recipe);
        /* Non-streaming providers retain their existing batch path. Drain any
         * earlier stream before that path loads or resets the shared table. */
        if (!streaming && do_load && stream_window_flush(&window)) return 1;
        if (streaming) {
            b0 = a0 + 1;
            sum = getenv("LAPLACE_RECORD_WINDOW_BYTES") ? strtoull(getenv("LAPLACE_RECORD_WINDOW_BYTES"), NULL, 10) : (UINT64_C(8) << 20);
        } else while (b0 < nfiles && (b0 == a0 || (sum + size[b0] <= batch && depth[b0] == depth[a0] &&
                !((do_load || preview_streaming) && !files[b0].known && !files[b0].skipped && recipe_record_streamable(files[b0].recipe))))) { sum += size[b0]; b0++; }
        table_size(sum);                                                                                                  /* the table is empty here: room for what this batch makes */
        double td = now();
        StreamLoad *stream = &streams[a0];
        *stream = (StreamLoad){.conninfo=conninfo, .threads=threads, .show=show_claims, .loading=do_load, .file=&files[a0],
            .fold=fold, .stats=&st, .events=&nev, .source_seconds=&completion.database_seconds, .window=&window};
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
                if (files[i].known || files[i].skipped) continue;
                progress_file("file-start", &files[i]);
                decompose_file(CTX[0], &files[i]); if (!do_load) progress_file("file-composed", &files[i]); bytes += files[i].bytes; done++;
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
                if (!do_load) progress_file("file-composed", &files[i]);
                #pragma omp critical
                {
                    bytes += files[i].bytes; done++;
                    if(progress_console()) fprintf(stderr, "\r  %d/%d files  %.1f MB  %.1f MB/s  %'llu nodes   ", done, nfiles, bytes / 1e6, bytes / 1e6 / (now() - t), (unsigned long long)table_count());
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
            if (streaming) {
                if (stream_window_add(&window, &files[a0])) return 1;
            } else {
                st.known += (uint64_t)ingest_known(conninfo, files + a0, b0 - a0);   /* recorded trunks: pruned whole, said nothing again */
                fold_events(fold, files + a0, b0 - a0);
                if (load(conninfo, threads, files + a0, b0 - a0, &st)) return 1;
            }
        }
        if (!do_load || !streaming) for (int i = a0; i < b0; i++) progress_file("file-composed", &files[i]);
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
            if (do_load && !streaming) { if (stream_window_flush(&window)) return 1; table_reset(); }
        }
        a0 = b0;
    }
    double source_finish = now(), database_before = completion.database_seconds;
    if (say_source_end(source_file_ready, &completion)) return 1;
    t_dec += now() - source_finish - (completion.database_seconds - database_before);
    if (do_load && stream_window_flush(&window)) return 1;
    if (do_load) printf("\n  streaming persistence: %'llu shared working windows, resident budget %'llu bytes\n",
        (unsigned long long)window.flushes, (unsigned long long)window.budget);
    fflush(stdout);
    free(window.roots); free(window.events.e); free(window.complete);
    free(streams);
    #undef SHOW
    #undef WHOLE
    fputc('\n', stderr);
    extern uint64_t table_total(void), table_hits(void);
    printf("\n== decomposition: %d files, %.1f MB, content recomposed byte for byte or curated %d, mismatched %d%s\n", nfiles, bytes / 1e6, exact, mism, batches > 1 ? ", a batch at a time" : "");
    if (do_load) printf("   files whose trunk was already recorded, which say nothing again: %'llu\n", (unsigned long long)st.known);
    printf("   %'llu compositions, %'llu reused; %'llu claims said by records\n",(unsigned long long)table_total(), (unsigned long long)table_hits(), (unsigned long long)nev);
    uint64_t incomplete=0; for (int i=0; i<nfiles; i++) incomplete+=files[i].incomplete;
    uint64_t omissions=say_obligations(stdout);
    extern uint64_t records_differ(void), say_unplaced(void), say_refused(void);
    uint64_t conflicts=records_differ(), unplaced=say_unplaced(), refused=say_refused();
    if (incomplete || omissions || conflicts || unplaced || refused || mism) {
        fprintf(stderr,"ingest rejected: incomplete_parts=%llu semantic_omissions=%llu metadata_conflicts=%llu unplaced_claims=%llu refused_parts=%llu byte_mismatches=%d; source and observation windows were not published\n",
            (unsigned long long)incomplete,(unsigned long long)omissions,(unsigned long long)conflicts,(unsigned long long)unplaced,(unsigned long long)refused,mism);
        fold_free(fold); progress_end("failed",NULL); return 1;
    }
    printf("\n== phases\n");
    printf("  %-44s %8.2f s   %8.1f MB/s\n", "decompose (all threads)", t_dec, bytes / 1e6 / (t_dec > 0 ? t_dec : 1));
    printf("  %-44s %8.2f s\n", "recompose and compare", t_rec);
    if (!do_load) { fold_free(fold); return mism ? 1 : 0; }
    printf("  %-44s %8.2f s   %'llu IDs checked in %llu rounds, %'llu subtrees already recorded\n", "deduplication, trunk to leaf", st.t_dedup,
           (unsigned long long)st.checked, (unsigned long long)st.rounds, (unsigned long long)st.found);
    printf("  %-44s %8.2f s   %'llu entities, %'llu paths (%'.0f rows/s)\n", "structure persistence", st.t_copy,
           (unsigned long long)st.ent_rows, (unsigned long long)st.phy_rows, (st.ent_rows + st.phy_rows) / (st.t_copy > 0 ? st.t_copy : 1));
    printf("    %-42s %8.2f s\n", "Native selection and dependency order", st.t_prepare);
    printf("    %-42s %8.2f s\n", "parallel staging COPY", st.t_transfer);
    printf("    %-42s %8.2f s   %llu closed dependency waves, up to %llu writers\n", "parallel paired publication", st.t_publish, (unsigned long long)st.publish_waves, (unsigned long long)st.publish_lanes);
    if(st.t_index)printf("      %-40s %8.2f s   included in paired publication\n", "GIN maintenance per working window", st.t_index);
    printf("  %-44s %8.2f s\n", "private observation windows", st.t_sem);
    fflush(stdout);
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
              /* Known files were not decomposed and carry no transient
               * lineage Ref. The source declaration owns this metadata,
               * including on an unchanged replay. Stage its entity too. */
              if (one->lineage[0]) {
                  sf.lineage=text_ref(CTX[0],(const uint8_t *)one->lineage,strlen(one->lineage));
                  sf.has_lineage=1;
              }
              progress_phase("source-trunk", &trunk.id);
              LoadStats ss = { 0 }; double ts = now(); if (load(conninfo, threads, &sf, 1, &ss)) return 1;
              printf("  %-44s %8.2f s   %s%s\n", "the source's trunk", now() - ts, source_called(one), ss.ent_rows ? "" : ": already recorded");
              st.ent_rows += ss.ent_rows; st.phy_rows += ss.phy_rows;fflush(stdout); 
              { char hx[33]; id_text(&trunk.id, hx); printf("  %-44s %s\n", "its ID, the witness", hx); }
              witness_trunk = trunk.id; witness_trust = trust; has_witness_trunk = 1; witness_supersedes = one->supersede;
              witness_lineage=sf.lineage.id; has_witness_lineage=sf.has_lineage; }
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
                if (files[i].trust == 0.0) continue;   /* no class declared: content, not a witness */
                bool fresh; lp_idmap_put(seen, &files[i].file.id, &fresh); if (!fresh) continue;
                roots[nr++] = (LoadWitness){.id=files[i].file.id, .lineage=files[i].lineage.id,
                    .trust=files[i].trust, .has_lineage=files[i].has_lineage};
            }
            lp_idmap_free(seen);
        }
        double publication_started=now();
        progress_phase("source-publication", has_witness_trunk ? &witness_trunk : NULL);
        if (load_register(conninfo, threads, roots, nr, files, nfiles) || merge(conninfo, threads, &st)) { free(roots); return 1; }
        printf("  %-44s %8.2f s\n", "source publication", now()-publication_started);
        free(roots);
        if (has_witness_trunk && witness_supersedes && forget_superseded(conninfo, threads, &witness_trunk)) return 1;
    }
    printf("\n== total %.1f s\n", now() - T);
    progress_end("complete",has_witness_trunk ? &witness_trunk : NULL);
    return mism ? 1 : 0;
}
