/* laplace highway: the types, generated natively from the resources that list them, as a perf-cache beside tier 0.
 *
 * What a curated resource says in its enumerations is a type, not content (Semantics: Claims): a part of speech, a
 * dependency relation, a lexicographer file, an interlingual concept, a VerbNet class or role, a FrameNet frame,
 * frame element or lexical unit, a PropBank roleset, a VerbAtlas frame. Each list is the resource's own, in the order
 * it writes it; each type's record is the ID and coordinate of its content, computed here from tier 0 (a type's
 * content is what the resource writes for it: NOUN; adj.all; a concept's definition; a frame's name); and the
 * mappings the highway resources draw between the lists (PredicateMatrix, SemLink, VerbAtlas, PropBank's links,
 * VerbNet's members, FrameNet's units) are edges between slots. The layout is written beside the records, with the
 * lists small enough to be mask fields (Semantics: Claims, Masks), and the records and edges have a fingerprint.
 * Usage: laplace highway [-o highway.bin]; it reads the sources by their names from the recipes directory. */
#define _GNU_SOURCE
#include "engine.h"
#include "json_min.h"
#include "blake3.h"
#include <dirent.h>
#include <glob.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[32], say[64]; uint32_t first, count; } List;
typedef struct { char a[32], b[32]; uint32_t from, to; } Edge;
static lp_tier0_record *rec; static size_t nrec, crec;
static List *lists; static int nlists; static List *cur;
static Edge *edges; static size_t nedges, cedges;
static Ctx *ctx;
static uint32_t *slot_map; static size_t slot_cap;                              /* ID -> record index + 1, over every list */

static uint64_t hkey(const lp_id *id){ uint64_t k; memcpy(&k, id->b, 8); return k; }
static void slot_grow(void){
    size_t nc = slot_cap ? slot_cap * 2 : 1 << 16; uint32_t *t = calloc(nc, 4);
    for (size_t i = 0; i < nrec; i++) { uint64_t k = hkey(&rec[i].id) & (nc - 1); while (t[k]) k = (k + 1) & (nc - 1); t[k] = (uint32_t)i + 1; }
    free(slot_map); slot_map = t; slot_cap = nc;
}
/* The slot of a type in the current list, added if it is not there: its record is its content's ID and coordinate. */
static int64_t slot_of(Ref r){
    if ((nrec + 1) * 2 > slot_cap) slot_grow();
    uint64_t k = hkey(&r.id) & (slot_cap - 1);
    while (slot_map[k]) { size_t i = slot_map[k] - 1; if (!memcmp(&rec[i].id, &r.id, 16)) return i >= cur->first ? (int64_t)(i - cur->first) : -1; k = (k + 1) & (slot_cap - 1); }
    if (nrec == crec) { crec = crec ? crec * 2 : 4096; rec = xrealloc(rec, crec * sizeof(lp_tier0_record)); }
    lp_tier0_record *x = &rec[nrec]; memset(x, 0, sizeof *x); x->id = r.id; memcpy(x->m, r.c.m, 32); x->hilbert = lp_hilbert4(&r.c); x->rank = (uint32_t)(nrec - cur->first); x->pad = r.tier;
    slot_map[k] = (uint32_t)nrec + 1; nrec++; cur->count++;
    return (int64_t)x->rank;
}
static Ref text(const char *s, size_t n){ return lp_text_decompose(ctx, (const uint8_t *)s, n, NULL, NULL); }
static Ref pair(Ref a, Ref b){ Ref t[2] = { a, b }; return lp_ref_compose(t, 2, (uint8_t)((a.tier > b.tier ? a.tier : b.tier) + 1)); }
static List *list_begin(const char *name, const char *say){
    lists = xrealloc(lists, sizeof(List) * (size_t)(nlists + 1)); cur = &lists[nlists++]; memset(cur, 0, sizeof *cur);
    snprintf(cur->name, sizeof cur->name, "%s", name); snprintf(cur->say, sizeof cur->say, "%s", say); cur->first = (uint32_t)nrec; return cur;
}
static List *list_named(const char *name){ for (int i = 0; i < nlists; i++) if (!strcmp(lists[i].name, name)) return &lists[i]; return NULL; }
static int64_t slot_in(const char *list, Ref r){
    List *l = list_named(list); if (!l || !slot_cap) return -1;
    uint64_t k = hkey(&r.id) & (slot_cap - 1);
    while (slot_map[k]) { size_t i = slot_map[k] - 1; if (!memcmp(&rec[i].id, &r.id, 16) && i >= l->first && i < (size_t)l->first + l->count) return (int64_t)(i - l->first); k = (k + 1) & (slot_cap - 1); }
    return -1;
}
static void edge(const char *a, int64_t from, const char *b, int64_t to){
    if (from < 0 || to < 0) return;
    if (nedges == cedges) { cedges = cedges ? cedges * 2 : 65536; edges = xrealloc(edges, cedges * sizeof(Edge)); }
    Edge *e = &edges[nedges++]; snprintf(e->a, sizeof e->a, "%s", a); snprintf(e->b, sizeof e->b, "%s", b); e->from = (uint32_t)from; e->to = (uint32_t)to;
}
static int edge_cmp(const void *x, const void *y){ const Edge *a = x, *b = y; int c = strcmp(a->a, b->a); if (c) return c; c = strcmp(a->b, b->b); if (c) return c;
    return a->from < b->from ? -1 : a->from > b->from ? 1 : a->to < b->to ? -1 : a->to > b->to; }

/* ---- reading the resources */
static uint8_t *read_file(const char *path, size_t *n){ FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1); *n = fread(b, 1, (size_t)l, f); b[*n] = 0; fclose(f); return b; }
static const Source *source_named(Source *s, int n, const char *name){ for (int i = 0; i < n; i++) if (!strcmp(s[i].name, name)) return &s[i]; return NULL; }
/* An attribute's value in an element's text, as written (entities left as they are: the resources write names plainly). */
static int attr(const char *el, const char *name, char *out, size_t cap){
    size_t nl = strlen(name); const char *p = el;
    while ((p = strstr(p, name))) { if ((p == el || p[-1] == ' ' || p[-1] == '\t') && p[nl] == '=' && p[nl + 1] == '"') { const char *v = p + nl + 2, *e = strchr(v, '"'); if (!e) return 0; snprintf(out, cap, "%.*s", (int)(e - v), v); return 1; } p += nl; }
    return 0;
}
/* Each line that begins an element of the name, with the element's tag text up to its '>' */
#define EACH_TAG(src, tag, line) for (const char *line = strstr((const char *)(src), "<" tag " "); line; line = strstr(line + 1, "<" tag " "))
static uint64_t nslots_of(const char *name){ List *l = list_named(name); return l ? l->count : 0; }

/* ---- WordNet 3.0's sense keys and offsets, resolved to ILI through CILI's map: a resolution table, never recorded */
typedef struct { char key[64]; int64_t ili; } Sense;              /* a resource's key, and the slot it points at (the list is the key's prefix) */
static Sense *senses; static size_t nsenses, csenses; static uint32_t *sense_map; static size_t sense_cap;
static uint64_t skey(const char *s){ uint64_t h = 1469598103934665603ull; for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull; return h; }
static void sense_put(const char *key, int64_t ili){
    if ((nsenses + 1) * 2 > sense_cap) { size_t nc = sense_cap ? sense_cap * 2 : 1 << 18; uint32_t *t = calloc(nc, 4); for (size_t i = 0; i < nsenses; i++) { uint64_t k = skey(senses[i].key) & (nc - 1); while (t[k]) k = (k + 1) & (nc - 1); t[k] = (uint32_t)i + 1; } free(sense_map); sense_map = t; sense_cap = nc; }
    if (nsenses == csenses) { csenses = csenses ? csenses * 2 : 65536; senses = xrealloc(senses, csenses * sizeof(Sense)); }
    Sense *s = &senses[nsenses]; snprintf(s->key, sizeof s->key, "%s", key); s->ili = ili;
    uint64_t k = skey(key) & (sense_cap - 1); while (sense_map[k]) k = (k + 1) & (sense_cap - 1); sense_map[k] = (uint32_t)nsenses + 1; nsenses++;
}
static int64_t sense_get(const char *key){
    if (!sense_cap) return -1; uint64_t k = skey(key) & (sense_cap - 1);
    while (sense_map[k]) { Sense *s = &senses[sense_map[k] - 1]; if (!strcmp(s->key, key)) return s->ili; k = (k + 1) & (sense_cap - 1); }
    return -1;
}
/* ILI slots by CILI's own number: i46360 is slot 46359 only if the file lists every number; it is looked up by ID */
static int64_t ili_by_number(const char *inum){ char key[32]; snprintf(key, sizeof key, "ili:%s", inum); return sense_get(key); }

int cmd_highway(int argc, char **argv){
    const char *outp = lp_highway_path();
    for (int a = 1; a < argc; a++) { if (!strcmp(argv[a], "-o") && a + 1 < argc) outp = argv[++a]; else { fprintf(stderr, "usage: laplace highway [-o highway.bin]\n"); return 2; } }
    setlocale(LC_NUMERIC, "en_US.UTF-8"); double T = now();
    tier0_open(NULL); ctx = lp_text_new(T0); if (!ctx) { fprintf(stderr, "cannot open ICU's break iterators\n"); return 1; }
    Recipe *rc = NULL; recipes_load(laplace_recipes(), &rc); int ns; Source *src = sources_loaded(&ns);
    printf("laplace highway   the types, from the resources that list them\n");
    #define NEED(var, name) const Source *var = source_named(src, ns, name); if (!var || !var->found[0]) { printf("  %-28s not at any of its roots: its types are left out\n", name); }
    NEED(ud, "universal-dependencies-tools"); NEED(wn, "princeton-wordnet"); NEED(cili, "cili"); NEED(vn, "verbnet"); NEED(fn, "framenet"); NEED(pb, "propbank");
    NEED(pm, "predicate-matrix"); NEED(sl, "semlink"); NEED(va, "verbatlas");
    char p[4300]; size_t n; uint8_t *s;

    /* UD: parts of speech and relations, in the validator's order */
    if (ud && ud->found[0]) {
        static const struct { const char *file, *key, *list, *say; } L[] = { { "upos.json", "upos", "upos", "Universal part of speech" }, { "udeprels.json", "udeprels", "deprel", "Universal dependency relation" } };
        for (int i = 0; i < 2; i++) { snprintf(p, sizeof p, "%s/%s", ud->found, L[i].file); s = read_file(p, &n); if (!s) { perror(p); continue; }
            jdoc d = j_parse((const char *)s, n); int64_t arr = j_get(&d, 0, L[i].key); list_begin(L[i].list, L[i].say);
            if (arr >= 0) for (uint32_t k = 0; k < d.v[arr].n; k++) { const jnode *v = &d.v[d.kids[d.v[arr].first + k]]; if (v->t == J_STR) slot_of(text(v->str, strlen(v->str))); }
            free(s); printf("  %-28s %'llu\n", L[i].say, (unsigned long long)cur->count); }
    }
    /* WordNet: the lexicographer files; and its sense keys resolved to offsets (a table, not a list) */
    if (wn && wn->found[0]) {
        snprintf(p, sizeof p, "%s/dict/lexnames", wn->found); s = read_file(p, &n); list_begin("lexfile", "WordNet lexicographer file");
        if (s) { for (char *line = strtok((char *)s, "\n"); line; line = strtok(NULL, "\n")) { char *t = strchr(line, '\t'); if (!t) continue; char *e = strchr(t + 1, '\t'); slot_of(text(t + 1, e ? (size_t)(e - t - 1) : strlen(t + 1))); } free(s); }
        printf("  %-28s %'llu\n", "WordNet lexicographer files", (unsigned long long)cur->count);
    }
    /* CILI: every concept, its content the definition it gives; its number a key, resolved here */
    if (cili && cili->found[0]) {
        snprintf(p, sizeof p, "%s/ili.ttl", cili->found); s = read_file(p, &n); list_begin("ili", "Interlingual index concept");
        if (s) { char *cp = (char *)s;
            while ((cp = strstr(cp, "\n<i"))) { char *id0 = cp + 2, *ide = strchr(id0, '>'); if (!ide) break; char inum[24]; snprintf(inum, sizeof inum, "%.*s", (int)(ide - id0), id0);
                char *def = strstr(ide, "skos:definition"); char *nx = strstr(ide, "\n<i"); if (def && (!nx || def < nx)) { char *q = strchr(def, '"'); char *qe = q ? strchr(q + 1, '"') : NULL;
                    if (q && qe) { int64_t sl = slot_of(text(q + 1, (size_t)(qe - q - 1))); char key[32]; snprintf(key, sizeof key, "ili:%s", inum); sense_put(key, sl); } }
                cp = ide; }
            free(s); }
        printf("  %-28s %'llu\n", "CILI concepts", (unsigned long long)cur->count);
        snprintf(p, sizeof p, "%s/ili-map-pwn30.tab", cili->found); s = read_file(p, &n); uint64_t mapped = 0;
        if (s) { for (char *line = strtok((char *)s, "\n"); line; line = strtok(NULL, "\n")) { char *t = strchr(line, '\t'); if (!t) continue; *t = 0; int64_t sl = ili_by_number(line); if (sl < 0) continue;
                char key[40]; snprintf(key, sizeof key, "wn30:%s", t + 1); char *cr = strchr(key, '\r'); if (cr) *cr = 0; sense_put(key, sl); mapped++; } free(s); }
        if (wn && wn->found[0]) { snprintf(p, sizeof p, "%s/dict/index.sense", wn->found); s = read_file(p, &n); uint64_t keys = 0;
            if (s) { for (char *line = strtok((char *)s, "\n"); line; line = strtok(NULL, "\n")) { char *sp = strchr(line, ' '); if (!sp) continue; *sp = 0; char *off = sp + 1, *sp2 = strchr(off, ' '); if (sp2) *sp2 = 0;
                    const char *pc = strchr(line, '%'); if (!pc) continue; char pos = pc[1] == '1' ? 'n' : pc[1] == '2' ? 'v' : pc[1] == '3' ? 'a' : pc[1] == '4' ? 'r' : 's';
                    char key[48]; snprintf(key, sizeof key, "wn30:%s-%c", off, pos); int64_t sl = sense_get(key); if (sl < 0 && pos == 's') { snprintf(key, sizeof key, "wn30:%s-a", off); sl = sense_get(key); }
                    if (sl >= 0) { char sk[64]; snprintf(sk, sizeof sk, "sense:%s", line); sense_put(sk, sl); keys++; } } free(s); }
            printf("  %-28s %'llu synsets mapped, %'llu sense keys resolved\n", "WordNet 3.0 to ILI", (unsigned long long)mapped, (unsigned long long)keys); }
    }
    /* VerbNet: classes and subclasses by their IDs as written, thematic roles; members to ILI and FrameNet */
    if (vn && vn->found[0]) {
        list_begin("vnclass", "VerbNet class"); snprintf(p, sizeof p, "%s/*.xml", vn->found); glob_t g; if (glob(p, 0, NULL, &g)) g.gl_pathc = 0;
        typedef struct { char id[64]; int64_t slot; } Cls; Cls *cls = NULL; size_t ncls = 0;
        for (size_t i = 0; i < g.gl_pathc; i++) { s = read_file(g.gl_pathv[i], &n); if (!s) continue; char v[128];
            EACH_TAG(s, "VNCLASS", ln) if (attr(ln, "ID", v, sizeof v)) { cls = xrealloc(cls, sizeof(Cls) * (ncls + 1)); snprintf(cls[ncls].id, 64, "%s", v); cls[ncls++].slot = slot_of(text(v, strlen(v))); }
            EACH_TAG(s, "VNSUBCLASS", ln) if (attr(ln, "ID", v, sizeof v)) { cls = xrealloc(cls, sizeof(Cls) * (ncls + 1)); snprintf(cls[ncls].id, 64, "%s", v); cls[ncls++].slot = slot_of(text(v, strlen(v))); }
            free(s); }
        printf("  %-28s %'llu\n", "VerbNet classes", (unsigned long long)cur->count);
        list_begin("vnrole", "VerbNet thematic role");
        for (size_t i = 0; i < g.gl_pathc; i++) { s = read_file(g.gl_pathv[i], &n); if (!s) continue; char v[128]; EACH_TAG(s, "THEMROLE", ln) if (attr(ln, "type", v, sizeof v)) slot_of(text(v, strlen(v))); free(s); }
        printf("  %-28s %'llu\n", "VerbNet thematic roles", (unsigned long long)cur->count);
        /* a class number, as PredicateMatrix and SemLink write it (51.2), names the class whose ID ends in it */
        for (size_t i = 0; i < g.gl_pathc; i++) { s = read_file(g.gl_pathv[i], &n); if (!s) continue; char v[512], cid[64] = ""; int64_t cslot = -1;
            for (const char *ln = (const char *)s; (ln = strchr(ln, '<')); ln++) {
                if (!strncmp(ln, "<VNCLASS ", 9) || !strncmp(ln, "<VNSUBCLASS ", 12)) { if (attr(ln, "ID", cid, sizeof cid)) cslot = slot_in("vnclass", text(cid, strlen(cid))); }
                else if (!strncmp(ln, "<MEMBER ", 8) && cslot >= 0) {
                    if (attr(ln, "wn", v, sizeof v)) for (char *k = strtok(v, " "); k; k = strtok(NULL, " ")) { char sk[80]; snprintf(sk, sizeof sk, "sense:%s", k); int64_t il = sense_get(sk); if (il < 0) { snprintf(sk, sizeof sk, "sense:%s::", k); il = sense_get(sk); } edge("vnclass", cslot, "ili", il); }
                } }
            free(s); }
        for (size_t i = 0; i < ncls; i++) { const char *d = cls[i].id; while (*d && !(*d == '-' && d[1] >= '0' && d[1] <= '9')) d++; if (*d) { char key[80]; snprintf(key, sizeof key, "vn:%s", d + 1); sense_put(key, cls[i].slot); } }
        globfree(&g); free(cls);
    }
    /* FrameNet: frames, frame elements, lexical units, as its indexes list them */
    if (fn && fn->found[0]) {
        list_begin("fnframe", "FrameNet frame"); snprintf(p, sizeof p, "%s/frameIndex.xml", fn->found); s = read_file(p, &n); char v[256], w[256];
        if (s) { EACH_TAG(s, "frame", ln) if (attr(ln, "name", v, sizeof v)) { int64_t sl = slot_of(text(v, strlen(v))); char key[300]; snprintf(key, sizeof key, "fn:%s", v); sense_put(key, sl); } free(s); }
        printf("  %-28s %'llu\n", "FrameNet frames", (unsigned long long)cur->count);
        list_begin("fnfe", "FrameNet frame element"); snprintf(p, sizeof p, "%s/frame/*.xml", fn->found); glob_t g; if (glob(p, 0, NULL, &g)) g.gl_pathc = 0;
        for (size_t i = 0; i < g.gl_pathc; i++) { s = read_file(g.gl_pathv[i], &n); if (!s) continue; const char *fr = strstr((const char *)s, "<frame "); if (!fr || !attr(fr, "name", w, sizeof w)) { free(s); continue; }
            Ref frame = text(w, strlen(w)); int64_t fslot = slot_in("fnframe", frame);
            EACH_TAG(s, "FE", ln) if (attr(ln, "name", v, sizeof v)) { int64_t sl = slot_of(pair(frame, text(v, strlen(v)))); edge("fnfe", sl, "fnframe", fslot); }
            free(s); }
        globfree(&g); printf("  %-28s %'llu\n", "FrameNet frame elements", (unsigned long long)cur->count);
        list_begin("fnlu", "FrameNet lexical unit"); snprintf(p, sizeof p, "%s/luIndex.xml", fn->found); s = read_file(p, &n);
        if (s) { EACH_TAG(s, "lu", ln) if (attr(ln, "name", v, sizeof v) && attr(ln, "frameName", w, sizeof w)) { Ref frame = text(w, strlen(w)); int64_t sl = slot_of(pair(frame, text(v, strlen(v)))); edge("fnlu", sl, "fnframe", slot_in("fnframe", frame)); } free(s); }
        printf("  %-28s %'llu\n", "FrameNet lexical units", (unsigned long long)cur->count);
    }
    /* VerbNet members' FrameNet mappings, now that frames are listed */
    if (vn && vn->found[0] && fn && fn->found[0]) { snprintf(p, sizeof p, "%s/*.xml", vn->found); glob_t g; if (glob(p, 0, NULL, &g)) g.gl_pathc = 0; size_t before = nedges;
        for (size_t i = 0; i < g.gl_pathc; i++) { s = read_file(g.gl_pathv[i], &n); if (!s) continue; char v[512], cid[64]; int64_t cslot = -1;
            for (const char *ln = (const char *)s; (ln = strchr(ln, '<')); ln++) {
                if (!strncmp(ln, "<VNCLASS ", 9) || !strncmp(ln, "<VNSUBCLASS ", 12)) { if (attr(ln, "ID", cid, sizeof cid)) cslot = slot_in("vnclass", text(cid, strlen(cid))); }
                else if (!strncmp(ln, "<MEMBER ", 8) && cslot >= 0 && attr(ln, "fn_mapping", v, sizeof v) && strcmp(v, "None")) { char key[300]; snprintf(key, sizeof key, "fn:%s", v); edge("vnclass", cslot, "fnframe", sense_get(key)); } }
            free(s); }
        globfree(&g); printf("  %-28s %'llu edges\n", "VerbNet members to FrameNet", (unsigned long long)(nedges - before)); }
    /* PropBank: rolesets, and their links to VerbNet classes and FrameNet frames */
    if (pb && pb->found[0]) {
        list_begin("pbroleset", "PropBank roleset"); snprintf(p, sizeof p, "%s/*.xml", pb->found); glob_t g; if (glob(p, 0, NULL, &g)) g.gl_pathc = 0; size_t before = nedges;
        for (size_t i = 0; i < g.gl_pathc; i++) { s = read_file(g.gl_pathv[i], &n); if (!s) continue; char v[256], cl[256], rs[128]; int64_t rslot = -1;
            for (const char *ln = (const char *)s; (ln = strchr(ln, '<')); ln++) {
                if (!strncmp(ln, "<roleset ", 9)) { if (attr(ln, "id", rs, sizeof rs)) { rslot = slot_of(text(rs, strlen(rs))); char key[160]; snprintf(key, sizeof key, "pb:%s", rs); sense_put(key, rslot); } }
                else if ((!strncmp(ln, "<rolelink ", 10) || !strncmp(ln, "<lexlink ", 9)) && rslot >= 0 && attr(ln, "resource", v, sizeof v) && attr(ln, "class", cl, sizeof cl)) {
                    if (!strcmp(v, "VerbNet")) { const char *d = cl; while (*d && !(*d == '-' && d[1] >= '0' && d[1] <= '9')) d++; char key[300]; snprintf(key, sizeof key, "vn:%s", *d ? d + 1 : cl); edge("pbroleset", rslot, "vnclass", sense_get(key)); }
                    else if (!strcmp(v, "FrameNet")) { char key[300]; snprintf(key, sizeof key, "fn:%s", cl); edge("pbroleset", rslot, "fnframe", sense_get(key)); } } }
            free(s); }
        globfree(&g); printf("  %-28s %'llu, %'llu edges to VerbNet and FrameNet\n", "PropBank rolesets", (unsigned long long)cur->count, (unsigned long long)(nedges - before));
    }
    /* VerbAtlas: its frames, and BabelNet's bridge from them to WordNet 3.0, so to ILI; PropBank rolesets to them */
    if (va && va->found[0]) {
        list_begin("vaframe", "VerbAtlas frame"); glob_t g; snprintf(p, sizeof p, "%s/*/VA_frame_info.tsv", va->found); if (glob(p, 0, NULL, &g) || !g.gl_pathc) { snprintf(p, sizeof p, "%s/VA_frame_info.tsv", va->found); globfree(&g); glob(p, 0, NULL, &g); }
        char dir[4096] = ""; if (g.gl_pathc) { snprintf(dir, sizeof dir, "%s", g.gl_pathv[0]); char *sl = strrchr(dir, '/'); if (sl) *sl = 0; } globfree(&g);
        if (dir[0]) {
            snprintf(p, sizeof p, "%s/VA_frame_info.tsv", dir); s = read_file(p, &n);
            if (s) { for (char *line = strtok((char *)s, "\n"); line; line = strtok(NULL, "\n")) { if (strncmp(line, "va:", 3)) continue; char *t = strchr(line, '\t'); if (!t) continue; *t = 0; char *e = strchr(t + 1, '\t'); if (e) *e = 0;
                        int64_t sl = slot_of(text(t + 1, strlen(t + 1))); char key[40]; snprintf(key, sizeof key, "va:%s", line + 3); sense_put(key, sl); } free(s); }
            printf("  %-28s %'llu\n", "VerbAtlas frames", (unsigned long long)cur->count);
            size_t before = nedges;
            snprintf(p, sizeof p, "%s/bn2wn.tsv", dir); s = read_file(p, &n);                    /* bn -> wn30 offset+pos: a table */
            if (s) { for (char *line = strtok((char *)s, "\n"); line; line = strtok(NULL, "\n")) { if (strncmp(line, "bn:", 3)) continue; char *t = strchr(line, '\t'); if (!t) continue; *t = 0; char *w = t + 1; if (strncmp(w, "wn:", 3)) continue; w += 3; size_t l = strlen(w); if (w[l - 1] == '\r') w[--l] = 0;
                        char key[48]; snprintf(key, sizeof key, "wn30:%.*s-%c", (int)l - 1, w, w[l - 1]); int64_t il = sense_get(key); if (il < 0 && w[l - 1] == 's') { snprintf(key, sizeof key, "wn30:%.*s-a", (int)l - 1, w); il = sense_get(key); }
                        if (il >= 0) { snprintf(key, sizeof key, "%s", line); sense_put(key, il); } } free(s); }
            snprintf(p, sizeof p, "%s/VA_bn2va.tsv", dir); s = read_file(p, &n);
            if (s) { for (char *line = strtok((char *)s, "\n"); line; line = strtok(NULL, "\n")) { if (strncmp(line, "bn:", 3)) continue; char *t = strchr(line, '\t'); if (!t) continue; *t = 0; char *v = t + 1; if (strncmp(v, "va:", 3)) continue; char *cr = strchr(v, '\r'); if (cr) *cr = 0;
                        char key[40]; snprintf(key, sizeof key, "va:%s", v + 3); edge("ili", sense_get(line), "vaframe", sense_get(key)); } free(s); }
            snprintf(p, sizeof p, "%s/pb2va.tsv", dir); s = read_file(p, &n);
            if (s) { for (char *line = strtok((char *)s, "\n"); line; line = strtok(NULL, "\n")) { char *gt = strstr(line, ">va:"); if (!gt) continue; *gt = 0; char *v = gt + 1, *t = strchr(v, '\t'); if (t) *t = 0;
                        char k1[160], k2[40]; snprintf(k1, sizeof k1, "pb:%s", line); snprintf(k2, sizeof k2, "va:%s", v + 3); edge("pbroleset", sense_get(k1), "vaframe", sense_get(k2)); } free(s); }
            printf("  %-28s %'llu edges\n", "VerbAtlas mappings", (unsigned long long)(nedges - before));
        }
    }
    /* SemLink: rolesets to classes, classes to frames */
    if (sl && sl->found[0]) { size_t before = nedges;
        snprintf(p, sizeof p, "%s/instances/pb-vn2.json", sl->found); s = read_file(p, &n);
        if (s) { jdoc d = j_parse((const char *)s, n); const jnode *o = &d.v[0];
            if (o->t == J_OBJ) for (uint32_t i = 0; i + 1 < o->n; i += 2) { const char *rs = d.v[d.kids[o->first + i]].str; const jnode *cl = &d.v[d.kids[o->first + i + 1]]; char k1[160]; snprintf(k1, sizeof k1, "pb:%s", rs);
                if (cl->t == J_OBJ) for (uint32_t j = 0; j + 1 < cl->n; j += 2) { char k2[80]; snprintf(k2, sizeof k2, "vn:%s", d.v[d.kids[cl->first + j]].str); edge("pbroleset", sense_get(k1), "vnclass", sense_get(k2)); } }
            free(s); }
        snprintf(p, sizeof p, "%s/instances/vn-fn2.json", sl->found); s = read_file(p, &n);
        if (s) { jdoc d = j_parse((const char *)s, n); const jnode *o = &d.v[0];
            if (o->t == J_OBJ) for (uint32_t i = 0; i + 1 < o->n; i += 2) { const char *cv = d.v[d.kids[o->first + i]].str; char num[80]; snprintf(num, sizeof num, "%s", cv); char *dash = strrchr(num, '-'); if (dash && !(dash[1] >= '0' && dash[1] <= '9')) *dash = 0;
                char k1[100]; snprintf(k1, sizeof k1, "vn:%s", num); const jnode *fr = &d.v[d.kids[o->first + i + 1]];
                if (fr->t == J_ARR) for (uint32_t j = 0; j < fr->n; j++) { const jnode *f = &d.v[d.kids[fr->first + j]]; if (f->t != J_STR) continue; char k2[300]; snprintf(k2, sizeof k2, "fn:%s", f->str); edge("vnclass", sense_get(k1), "fnframe", sense_get(k2)); } }
            free(s); }
        printf("  %-28s %'llu edges\n", "SemLink mappings", (unsigned long long)(nedges - before)); }
    /* PredicateMatrix: every row ties a WordNet sense, an ILI, a VerbNet class, a FrameNet frame and a PropBank roleset */
    if (pm && pm->found[0]) { size_t before = nedges; snprintf(p, sizeof p, "%s/PredicateMatrix.v1.3.txt", pm->found); s = read_file(p, &n);
        if (s) { int first = 1; for (char *line = strtok((char *)s, "\n"); line; line = strtok(NULL, "\n")) { if (first) { first = 0; continue; }
                char *f[28]; int nf = 0; for (char *t = line; t && nf < 28; ) { f[nf++] = t; t = strchr(t, '\t'); if (t) *t++ = 0; } if (nf < 17) continue;
                int64_t il = -1, vc = -1, ff = -1, pr = -1; char key[300];
                if (!strncmp(f[11], "mcr:ili-30-", 11)) { snprintf(key, sizeof key, "wn30:%s", f[11] + 11); il = sense_get(key); }
                if (il < 0 && !strncmp(f[10], "wn:", 3)) { snprintf(key, sizeof key, "sense:%s::", f[10] + 3); il = sense_get(key); }
                if (!strncmp(f[4], "vn:", 3) && strcmp(f[4] + 3, "NULL")) { snprintf(key, sizeof key, "vn:%s", f[4] + 3); vc = sense_get(key); }
                if (!strncmp(f[12], "fn:", 3) && strcmp(f[12] + 3, "NULL")) { snprintf(key, sizeof key, "fn:%s", f[12] + 3); ff = sense_get(key); }
                if (!strncmp(f[15], "pb:", 3) && strcmp(f[15] + 3, "NULL")) { snprintf(key, sizeof key, "pb:%s", f[15] + 3); pr = sense_get(key); }
                edge("ili", il, "vnclass", vc); edge("ili", il, "fnframe", ff); edge("ili", il, "pbroleset", pr); edge("vnclass", vc, "fnframe", ff); edge("pbroleset", pr, "vnclass", vc); edge("pbroleset", pr, "fnframe", ff); }
            free(s); }
        printf("  %-28s %'llu edges\n", "PredicateMatrix", (unsigned long long)(nedges - before)); }

    /* the edges, sorted by their lists and their from-slot, each once; then the records and the layout */
    qsort(edges, nedges, sizeof(Edge), edge_cmp); size_t m = 0;
    for (size_t i = 0; i < nedges; i++) if (!m || edge_cmp(&edges[i], &edges[m - 1])) edges[m++] = edges[i];
    nedges = m;
    FILE *o = fopen(outp, "wb"); if (!o) { perror(outp); return 1; }
    if (fwrite(rec, sizeof(lp_tier0_record), nrec, o) != nrec) { perror(outp); return 1; }
    for (size_t i = 0; i < nedges; i++) { uint32_t pr[2] = { edges[i].from, edges[i].to }; fwrite(pr, 4, 2, o); }
    fclose(o);
    char lay[4300]; snprintf(lay, sizeof lay, "%s.layout", outp); o = fopen(lay, "w"); if (!o) { perror(lay); return 1; }
    fprintf(o, "# The highway: one record per type (as a tier-0 record, its rank the slot), the lists in the resources' order, then the edges as pairs of slots.\n");
    fprintf(o, "records\t%zu\nedges-count\t%zu\n", nrec, nedges);
    for (int i = 0; i < nlists; i++) fprintf(o, "list\t%s\t%s\t%u\t%u\n", lists[i].name, lists[i].say, lists[i].first, lists[i].count);
    for (size_t i = 0; i < nedges; ) { size_t j = i; while (j < nedges && !strcmp(edges[j].a, edges[i].a) && !strcmp(edges[j].b, edges[i].b)) j++; fprintf(o, "edges\t%s\t%s\t%zu\t%zu\n", edges[i].a, edges[i].b, i, j - i); i = j; }
    /* the mask fields: bits 0 to 7 say what a row is (a claim, a record, a tuple, a file); the lists small enough follow, each as wide as it is */
    int bit = 8; fprintf(o, "mask\tkind\t0\t8\n");
    for (int i = 0; i < nlists; i++) if (lists[i].count && lists[i].count <= 64 && bit + (int)lists[i].count <= 256) { fprintf(o, "mask\t%s\t%d\t%u\n", lists[i].name, bit, lists[i].count); bit += (int)lists[i].count; }
    fclose(o);
    /* the keys the resources point at their types with, beside the highway: resolved by readers, recorded nowhere */
    static const struct { const char *prefix, *list; } K[] = { { "ili:", "ili" }, { "vn:", "vnclass" }, { "fn:", "fnframe" }, { "pb:", "pbroleset" }, { "va:", "vaframe" } };
    snprintf(lay, sizeof lay, "%s.keys", outp); o = fopen(lay, "w"); if (!o) { perror(lay); return 1; } size_t nkeys = 0;
    fprintf(o, "# The keys the resources point at their types with: list, the key as the resource writes it, the slot. Resolved by readers, recorded nowhere.\n");
    for (size_t i = 0; i < nsenses; i++) for (size_t j = 0; j < sizeof K / sizeof *K; j++) { size_t pl = strlen(K[j].prefix);
        if (!strncmp(senses[i].key, K[j].prefix, pl) && senses[i].ili >= 0) { fprintf(o, "%s\t%s\t%lld\n", K[j].list, senses[i].key + pl, (long long)senses[i].ili); nkeys++; } }
    fclose(o);
    uint8_t fp[32]; blake3_hasher hs; blake3_hasher_init(&hs); blake3_hasher_update(&hs, rec, nrec * sizeof(lp_tier0_record)); for (size_t i = 0; i < nedges; i++) { uint32_t pr[2] = { edges[i].from, edges[i].to }; blake3_hasher_update(&hs, pr, 8); } blake3_hasher_finalize(&hs, fp, 32);
    printf("\n%s: %'zu types in %d lists, %'zu edges, %'zu keys beside them; %d of 256 mask bits   (%.1f s)\nfingerprint ", outp, nrec, nlists, nedges, nkeys, bit, now() - T);
    for (int i = 0; i < 32; i++) printf("%02x", fp[i]); printf("\n");
    (void)nslots_of;
    return 0;
}
