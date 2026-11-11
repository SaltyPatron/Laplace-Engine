#define _GNU_SOURCE
#include "ingest_internal.h"
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/statvfs.h>
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
void show_entities(const char *conninfo, int ask){
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
char **paths; const Source **path_of; int npaths; static int cpaths; const Source *walking;
void add_path(const char *p){
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
void walk_dir(const char *dir){ ig_reset(); nftw(dir, walk_cb, 64, FTW_PHYS | FTW_ACTIONRETVAL); ig_reset(); }
void source_walk(const Source *sc){
    if (!sc->nfiles) { walk_dir(sc->found); return; }
    for (int z = 0; z < sc->nfiles; z++) { glob_t g; if (glob(sc->files[z], 0, NULL, &g)) continue;
        for (size_t y = 0; y < g.gl_pathc; y++) {
            if (os_is_dir(g.gl_pathv[y])) walk_dir(g.gl_pathv[y]); else add_path(g.gl_pathv[y]); }
        globfree(&g); }
}

/* Claims render composed text directly; cache-only references render their
 * list and canonical identifier from the same immutable mapping. */
int tsv_claims;                                                        /* --claims-tsv: a claim a line, its parts between tabs */
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
void tsv_escaped(const uint8_t *b, size_t n){
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
void tsv_record(const lp_id *id, unsigned position){
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
void show_record_events(const File *file){
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
        if (event->run > 1) printf("   games %u", (unsigned)event->run);
        putchar('\n');
    }
}
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
typedef struct { char *path;Recipe *recipe; } OrderedSourceFile;
static int by_depth(const void *x,const void *y){
    const OrderedSourceFile *a=x,*b=y;size_t da=a->recipe->dependency_level,db=b->recipe->dependency_level;
    return da!=db?(da<db?-1:1):strcmp(a->path,b->path);
}
int source_files(const Source *sc, Recipe *rec, int nrec, char ***out, Recipe ***of){
    int from = npaths; const Source *was = walking; walking = sc;
    source_walk(sc);
    int n=0;OrderedSourceFile *selected=malloc((size_t)(npaths-from+1)*sizeof *selected);
    for(int i=from;i<npaths;i++){
        Recipe *recipe=recipe_for(rec,nrec,paths[i],sc);
        if(recipe)selected[n++]=(OrderedSourceFile){paths[i],recipe};else free(paths[i]);
    }
    npaths=from;qsort(selected,(size_t)n,sizeof *selected,by_depth);
    char **p=malloc((size_t)(n+1)*sizeof *p);Recipe **r=malloc((size_t)(n+1)*sizeof *r);
    for(int i=0;i<n;i++){p[i]=selected[i].path;r[i]=selected[i].recipe;}p[n]=NULL;r[n]=NULL;free(selected);
    walking = was; *out = p; *of = r; return n;
}
/* Probe recorded file identities before reading file content. A file's metadata tree begins with the OS's record of
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
    if(known){if(tier>out->tier)out->tier=tier;return 1;}
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
    uint8_t floor=ref_above(scratch->children,n);if(tier<floor)tier=floor;
    if(!lp_ref_compose_runs(scratch->children,scratch->runs,n,tier,out)||!lp_id_eq(id,&out->id))return -1;
    out->tier=tier;return 1;
}
typedef struct { lp_id id; int file; } FileProbe;
typedef lp_vec(FileProbe) FileProbes;
/* A shared metadata node can belong to several selected files. Identity
 * deduplicates database reads; (identity,file) deduplicates traversal without
 * throwing away an occurrence's owner or imposing an ancestry limit. */
static void file_probe_add(FileProbes *frontier,lp_strmap *seen,const lp_id *id,int file){
    uint8_t key[sizeof *id+sizeof file];bool fresh;
    memcpy(key,id,sizeof *id);memcpy(key+sizeof *id,&file,sizeof file);
    lp_strmap_put(seen,key,sizeof key,&fresh);
    if(fresh)lp_push(frontier,((FileProbe){.id=*id,.file=file}));
}
int files_recorded(const char *conninfo, File *files, int nfiles){
    File *candidate=NULL;int *owner=NULL,ncandidate=0,ccandidate=0,failed=0;
    uint8_t *local=NULL;FileRefScratch scratch={0};
    FileProbes frontier={0};lp_strmap *seen=lp_strmap_sized(1);int found=0;
    for(int i=0;i<nfiles;i++){
        if(files[i].skipped || files[i].known)continue;
        Ref os;if(file_os(files[i].recipe,&files[i],&os))file_probe_add(&frontier,seen,&os.id,i);
    }
    if(frontier.n){
        /* A recorded parent owns its complete structural closure. An absent
         * metadata ID therefore cannot have a recorded file above it. */
        lp_idmap *ids=lp_idmap_new();
        for(size_t i=0;i<frontier.n;i++)lp_idmap_put(ids,&frontier.v[i].id,NULL);
        uint8_t *recorded=db_recorded(conninfo,lp_idmap_keys(ids),(uint64_t)lp_idmap_count(ids));size_t kept=0;
        for(size_t i=0;i<frontier.n;i++)if(recorded[lp_idmap_find(ids,&frontier.v[i].id)])frontier.v[kept++]=frontier.v[i];
        lp_idmap_free(ids);free(recorded);frontier.n=kept;
    }
    PGconn *pg=frontier.n?db_connect(conninfo):NULL;
    while(frontier.n && !failed){
        typedef struct { size_t head; } ProbeOwners;
        lp_idmap *at=lp_idmap_sized(sizeof(ProbeOwners));size_t *links=malloc(frontier.n*sizeof *links);
        for(size_t k=0;k<frontier.n;k++){
            bool fresh;ProbeOwners *origins=lp_idmap_get(at,&frontier.v[k].id,&fresh);
            links[k]=fresh?SIZE_MAX:origins->head;origins->head=k;
        }
        FileProbes next={0};const lp_id *ids=lp_idmap_keys(at);size_t count=lp_idmap_count(at);
        for(size_t begin=0;begin<count && !failed;){
            size_t take=count-begin;if(take>DB_ID_WINDOW)take=DB_ID_WINDOW;
            Args args={0};arg_ids(&args,ids+begin,take);
            PGresult *r=ask_try(pg,"SELECT wanted.id,c.entity,@postgis@.ST_AsEWKB(c.path),c.tier"
                " FROM unnest($1::@extschema@.blake3[]) wanted(id) JOIN @extschema@.physicality c"
                " ON c.path OPERATOR(@extschema@.@>) ARRAY[wanted.id]::@extschema@.blake3[]"
                " WHERE @extschema@.laplace_path_starts(c.path,wanted.id)",&args);
            args_free(&args);begin+=take;
            if(!r){fprintf(stderr,"which files are recorded: %s",PQerrorMessage(pg));failed=1;break;}
            for (int j = 0; j < PQntuples(r); j++) {
                lp_id child,c;memcpy(child.b,PQgetvalue(r,j,0),sizeof child.b);memcpy(c.b,PQgetvalue(r,j,1),sizeof c.b);
                ProbeOwners *origins=lp_idmap_lookup(at,&child);if(!origins)continue;
                lp_vertex vx[2];size_t nv=lp_path_vertices((const uint8_t *)PQgetvalue(r,j,2),(size_t)PQgetlength(r,j,2),vx,2);
                if(!nv || !lp_id_eq(&vx[0].id,&child))continue;
                uint16_t tier;memcpy(&tier,PQgetvalue(r,j,3),sizeof tier);tier=ntohs(tier);
                for(size_t k=origins->head;k!=SIZE_MAX;k=links[k]){
                    int file=frontier.v[k].file;
                    if(vx[0].said!=LP_SAID_METADATA || nv<2){file_probe_add(&next,seen,&c,file);continue;}
                    if(tier>UINT8_MAX){fputs("file precheck: stored tier exceeds Native reference width\n",stderr);failed=1;break;}
                    if(ncandidate==ccandidate){ccandidate=ccandidate?ccandidate*2:128;candidate=xrealloc(candidate,(size_t)ccandidate*sizeof *candidate);owner=xrealloc(owner,(size_t)ccandidate*sizeof *owner);local=xrealloc(local,(size_t)ccandidate);}
                    owner[ncandidate]=file;File *fl=&candidate[ncandidate];*fl=files[file];
                    int have=ingest_local_file_ref(&c,(const uint8_t *)PQgetvalue(r,j,2),(size_t)PQgetlength(r,j,2),(uint8_t)tier,&scratch,&fl->file);
                    if(have<0){fputs("file precheck: stored path disagrees with canonical Native composition\n",stderr);failed=1;break;}
                    local[ncandidate++]=(uint8_t)have;
                    if(!have){memset(&fl->file,0,sizeof fl->file);fl->file.id=c;fl->file.tier=(uint8_t)tier;}
                    fl->file.said = 0;
                    fl->recorded_metadata = vx[0].id;
                    fl->has_file = 1; fl->known = 1;
                }
                if(failed)break;
            }
            PQclear(r);
        }
        lp_idmap_free(at);free(links);lp_vec_free(&frontier);frontier=next;
    }
    lp_vec_free(&frontier);lp_strmap_free(seen);free(scratch.vertices);free(scratch.children);free(scratch.runs);
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
        uint8_t *ab=malloc(20+20*(size_t)count);size_t al=ids_param(pg,ab,missing+begin,(uint32_t)count);
        const char *v[]={ (const char *)ab };int lengths[]={ (int)al },formats[]={1};
        PGresult *r=db_exec_params(pg,"SELECT wanted.id,"
            "(SELECT @postgis@.st_asewkb(e.coord) FROM @extschema@.entity e WHERE e.id OPERATOR(@extschema@.=) wanted.id) "
            "FROM unnest($1::@extschema@.blake3[]) wanted(id)",1,NULL,v,lengths,formats,1);
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
/* Explicit admission of an existing artifact. Generation and deployment never call this. */
int ingest_highway(const char *conninfo, const char *t0p, int threads, int do_load){
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
        if (merge(conninfo, threads, &st)) return 1;              /* what the load staged, into the real tables */
        printf("  %-52s %'llu types, %'llu compositions, %'llu entities new, %'llu not as written%s   %.1f s\n", "the highway's contents, as entities", (unsigned long long)types, (unsigned long long)n, (unsigned long long)st.ent_rows, (unsigned long long)wrong, "", now() - t);
    return 0;
}

/* Every source there is, in the order they go in, each in a process of its own with the options given, its output
 * kept in a log of its own. A source none of whose files is here is said so and passed over; a source that fails
 * stops the run, since what comes after it counts on it. What is already recorded is passed over by its bytes, so
 * a run that was cut off is taken up by running it again. */
/* The room left where the database keeps its data, or -1 when that cannot be seen from here (another machine). */
static double room_left(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo); double gb = -1;
    if (PQstatus(pg) == CONNECTION_OK) { PGresult *r = db_exec(pg, "SHOW data_directory");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r)) { struct statvfs v; if (!statvfs(PQgetvalue(r, 0, 0), &v)) gb = (double)v.f_bavail * (double)v.f_frsize / 1e9; }
        PQclear(r); }
    PQfinish(pg); return gb;
}
/* What a source took, measured as it went in: the database's growth over what its files hold, kept in the work
 * directory (room.tsv: source, times), one line a source, the last measure kept. It is a measure, not a forecast:
 * what is recorded once is not recorded again, so no later source is held to it. */
static double db_bytes(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo); double b = -1;
    if (PQstatus(pg) == CONNECTION_OK) { PGresult *r = db_exec(pg, "SELECT pg_database_size(current_database())"); if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r)) b = atof(PQgetvalue(r, 0, 0)); PQclear(r); }
    PQfinish(pg); return b;
}
static void room_keep(const char *path, const char *source, double times){
    char tmp[4200]; snprintf(tmp, sizeof tmp, "%s.new", path); FILE *o = fopen(tmp, "w"); if (!o) return;
    FILE *f = fopen(path, "r"); char line[512]; size_t n = strlen(source);
    while (f && fgets(line, sizeof line, f)) if (strncmp(line, source, n) || line[n] != '\t') fputs(line, o);
    if (f) fclose(f); fprintf(o, "%s\t%.3f\n", source, times); fclose(o); os_rename(tmp, path);
}
int ingest_every(int argc, char **argv, Source *src, int nsrc, Recipe *rec, int nrec, const int *want){
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
            { PGconn *pg = PQconnectdb(conninfo); if (PQstatus(pg) == CONNECTION_OK) { PGresult *r = db_exec(pg, "SHOW data_directory"); if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) && !statvfs(PQgetvalue(r, 0, 0), &v)) whole = (double)v.f_blocks * (double)v.f_frsize / 1e9; PQclear(r); } PQfinish(pg); }
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
