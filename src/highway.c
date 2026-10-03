/* laplace highway: the types, as a perf-cache beside tier 0, read from the resources by their own recipes.
 *
 * What a curated resource says in its enumerations is a type, not content (Semantics: Claims): a part of speech, a
 * dependency relation, a lexicographer file, an interlingual concept, a VerbNet class or role, a FrameNet frame,
 * frame element or lexical unit, a PropBank roleset, a VerbAtlas frame. Which of a resource's things are the types of
 * which list, the keys the resources point at them with, and the mappings the highway resources draw between them, are
 * the resources' recipes' to say (say.c: types, keyed, alias, maps); this reads every source that says any, in the
 * order the sources go in, by the one decomposer, and keeps what they say. Nothing here names a resource.
 *
 * Each list is in the order its resources write it; a type's record is the ID and coordinate of its content, the very
 * thing its recipe composes for it when the source is ingested (never the number or id a resource points at it with,
 * which is a key, kept beside); the mappings are edges between slots. Every list's slots are frozen (manifest/slots),
 * and the banks (manifest/banks.tsv) say which lists are masks, of which semantic group, on which row. The layout is
 * written beside the records, and the records and edges have a fingerprint.
 * The types' contents are written beside them as the compositions they are, for laplace deploy to record as entities.
 * Usage: laplace highway [-o highway.bin] */
#define _GNU_SOURCE
#include "engine.h"
#include "blake3.h"
#include "laplace_config.h"
#include <locale.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct { char name[32], say[64]; Ref *t; size_t n, cap; lp_idmap *map; uint32_t first; } List;   /* map: a type's slot by its ID */
typedef struct { int list; char *key; lp_id id; } Key;
typedef struct { int list; char *key, *to; int done; } Alias;
typedef struct { int la, lb; char *ka, *kb; } PEdge;
typedef struct { int la, lb; uint32_t from, to; } Edge;
struct Hw { List *l; int nl; Key *k; size_t nk, ck; uint32_t *kmap; size_t kcap; Alias *a; size_t na, ca; PEdge *e; size_t ne, ce; };

static uint64_t strk(int list, const char *s){ uint64_t h = 1469598103934665603ull ^ (uint64_t)list; for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull; return h; }
static int list_of(Hw *h, const char *name, const char *say){
    for (int i = 0; i < h->nl; i++) if (!strcmp(h->l[i].name, name)) { if (say && !h->l[i].say[0]) snprintf(h->l[i].say, sizeof h->l[i].say, "%s", say); return i; }
    h->l = xrealloc(h->l, sizeof(List) * (size_t)(h->nl + 1)); List *l = &h->l[h->nl]; memset(l, 0, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name); if (say) snprintf(l->say, sizeof l->say, "%s", say); return h->nl++;
}
static int64_t slot_in(const List *l, const lp_id *id){ return lp_idmap_find(l->map, id); }
/* A type of a list: its slot is the next, unless its content is in the list already. */
void hw_type(Hw *h, const char *list, const char *say, Ref thing){
    int li = list_of(h, list, say); List *l = &h->l[li]; if (slot_in(l, &thing.id) >= 0) return;      /* the list first: naming a new one moves them all */
    if (!l->map) l->map = lp_idmap_new(); lp_idmap_put(l->map, &thing.id, NULL);
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 1024; l->t = xrealloc(l->t, sizeof(Ref) * l->cap); }
    l->t[l->n++] = thing;
}
static Key *key_find(Hw *h, int list, const char *key){
    if (!h->kcap) return NULL; uint64_t k = strk(list, key) & (h->kcap - 1);
    while (h->kmap[k]) { Key *x = &h->k[h->kmap[k] - 1]; if (x->list == list && !strcmp(x->key, key)) return x; k = (k + 1) & (h->kcap - 1); }
    return NULL;
}
static int key_put(Hw *h, int list, const char *key, const lp_id *id){
    if (key_find(h, list, key)) return 0;                                    /* the first that names a key keeps it */
    if ((h->nk + 1) * 2 > h->kcap) { size_t nc = h->kcap ? h->kcap * 2 : 1 << 16; uint32_t *m = calloc(nc, 4);
        for (size_t i = 0; i < h->nk; i++) { uint64_t k = strk(h->k[i].list, h->k[i].key) & (nc - 1); while (m[k]) k = (k + 1) & (nc - 1); m[k] = (uint32_t)i + 1; } free(h->kmap); h->kmap = m; h->kcap = nc; }
    if (h->nk == h->ck) { h->ck = h->ck ? h->ck * 2 : 1 << 16; h->k = xrealloc(h->k, sizeof(Key) * h->ck); }
    Key *x = &h->k[h->nk]; x->list = list; x->key = strdup(key); x->id = *id;
    uint64_t k = strk(list, key) & (h->kcap - 1); while (h->kmap[k]) k = (k + 1) & (h->kcap - 1); h->kmap[k] = (uint32_t)++h->nk; return 1;
}
void hw_key(Hw *h, const char *list, Ref thing, const char *key){ key_put(h, list_of(h, list, NULL), key, &thing.id); }
void hw_alias(Hw *h, const char *list, const char *key, const char *to){
    if (h->na == h->ca) { h->ca = h->ca ? h->ca * 2 : 1 << 16; h->a = xrealloc(h->a, sizeof(Alias) * h->ca); }
    h->a[h->na++] = (Alias){ list_of(h, list, NULL), strdup(key), strdup(to), 0 };
}
void hw_edge(Hw *h, const char *la, const char *ka, const char *lb, const char *kb){
    if (h->ne == h->ce) { h->ce = h->ce ? h->ce * 2 : 1 << 16; h->e = xrealloc(h->e, sizeof(PEdge) * h->ce); }
    h->e[h->ne++] = (PEdge){ list_of(h, la, NULL), list_of(h, lb, NULL), strdup(ka), strdup(kb) };
}
static Hw *H;
static int edge_cmp(const void *x, const void *y){ const Edge *a = x, *b = y; int c = strcmp(H->l[a->la].name, H->l[b->la].name); if (c) return c; c = strcmp(H->l[a->lb].name, H->l[b->lb].name); if (c) return c;
    return a->from < b->from ? -1 : a->from > b->from ? 1 : a->to < b->to ? -1 : a->to > b->to; }

/* ---- the types' contents, as the compositions they are: every node under a type, once, before what holds it */
typedef struct { lp_idmap *m; } Seen;
static int seen_add(Seen *s, const lp_id *id){ if (!s->m) s->m = lp_idmap_new(); bool fresh; lp_idmap_put(s->m, id, &fresh); return fresh; }
static void hexid(const lp_id *id, char out[33]){ id_text(id, out); }
static void node_write(FILE *o, Seen *s, const lp_id *id, int depth){
    if (lp_tier0_codepoint(T0, id) >= 0 || depth > 256) return; Node *x = table_find(id); if (!x || !seen_add(s, id)) return;
    for (uint32_t v = 0; v < x->nv; v++) node_write(o, s, &VTX[x->voff + v].id, depth + 1);
    char h[33]; hexid(id, h); fprintf(o, "N\t%s\t%u\t%u", h, x->tier, x->nv);
    for (uint32_t v = 0; v < x->nv; v++) { const Vtx *w = &VTX[x->voff + v]; int64_t cp = lp_tier0_codepoint(T0, &w->id); char c[33];
        if (cp >= 0) snprintf(c, sizeof c, "U%llX", (long long)cp); else hexid(&w->id, c);
        fprintf(o, "\t%s:%u:%u", c, VSAID(w->m), VRUN(w->m)); }
    fputc('\n', o);
}

/* ---- frozen slots (decision: enumerate first, then append forever). A type's slot is what a mask bit, a vertex
 * value or a stored reference to it means; at billions of rows it can never move. Each list's slots are a registry of
 * their own, versioned with the manifests (manifest/slots/LIST.tsv): a type the registry holds keeps its slot; a type
 * no resource lists any longer keeps its slot, retired (a tombstone: never reused); a new type takes the next slot.
 * The order the resources happen to be read in decides nothing. */
static const char *manifest_dir(void){ const char *e = getenv("LAPLACE_MANIFEST"); return e && *e ? e : LAPLACE_MANIFEST_DEFAULT; }
typedef struct { lp_id id; int retired; char key[96]; } Frozen;
static Frozen *frozen_read(const char *list, size_t *n){
    char p[4096]; snprintf(p, sizeof p, "%s/slots/%s.tsv", manifest_dir(), list); FILE *f = fopen(p, "r"); *n = 0; if (!f) return NULL;
    Frozen *fz = NULL; size_t cap = 0; char line[1024];
    while (fgets(line, sizeof line, f)) { if (line[0] == '#' || !strncmp(line, "slot\t", 5)) continue;
        char *save = NULL, *slot = strtok_r(line, "\t\n", &save), *id = strtok_r(NULL, "\t\n", &save), *st = strtok_r(NULL, "\t\n", &save), *key = strtok_r(NULL, "\n", &save);
        if (!slot || !id || !st) continue; size_t s = strtoull(slot, NULL, 10);
        if (s != *n) { fprintf(stderr, "%s: slot %zu where %zu was due: a slot registry is never edited by hand\n", p, s, *n); exit(1); }
        if (*n == cap) { cap = cap ? cap * 2 : 1024; fz = xrealloc(fz, sizeof(Frozen) * cap); }
        Frozen *z = &fz[(*n)++]; memset(z, 0, sizeof *z); if (!id_parse(id, &z->id)) { fprintf(stderr, "%s: slot %zu: not an ID\n", p, s); exit(1); }
        z->retired = !strcmp(st, "retired"); if (key) snprintf(z->key, sizeof z->key, "%s", key); }
    fclose(f); return fz;
}
/* Each list put in its frozen order; returns how many types were new, how many retired, across the lists. */
static void freeze(Hw *h, size_t *added, size_t *retired, size_t *kept){
    *added = *retired = *kept = 0;
    lp_idmap *first = lp_idmap_new(); size_t fcap = 0; const char **fkey = NULL;   /* a readable name for each type: the first key that names it */
    for (int li = 0; li < h->nl; li++) {
        List *l = &h->l[li]; size_t nz; Frozen *fz = frozen_read(l->name, &nz);
        lp_idmap_free(first); first = lp_idmap_new(); fcap = 0; free(fkey); fkey = NULL;
        for (size_t i = 0; i < h->nk; i++) if (h->k[i].list == li) { bool fresh; size_t at = lp_idmap_put(first, &h->k[i].id, &fresh);
            if (fresh) { if (at >= fcap) { fcap = (at + 1) * 2; fkey = xrealloc(fkey, sizeof(char *) * fcap); } fkey[at] = h->k[i].key; } }
        Ref *t = xrealloc(NULL, sizeof(Ref) * (nz + l->n + 1)); size_t n = 0; uint8_t *placed = calloc(l->n + 1, 1);
        for (size_t s = 0; s < nz; s++) { int64_t was = slot_in(l, &fz[s].id);
            if (was >= 0) { t[n] = l->t[was]; placed[was] = 1; (*kept)++; fz[s].retired = 0; }
            else { memset(&t[n], 0, sizeof(Ref)); t[n].id = fz[s].id; if (!fz[s].retired) (*retired)++; fz[s].retired = 1; }
            n++; }
        size_t nz_before = nz;
        for (size_t i = 0; i < l->n; i++) if (!placed[i]) { t[n++] = l->t[i]; (*added)++; }
        lp_idmap_free(l->map); l->map = lp_idmap_new(); for (size_t s = 0; s < n; s++) lp_idmap_put(l->map, &t[s].id, NULL);
        free(l->t); l->t = t; l->n = n; l->cap = n + 1; free(placed);
        /* the registry, written back: the slots it held, then the new ones */
        char d[4096], p[4200], tmp[4300]; snprintf(d, sizeof d, "%s/slots", manifest_dir()); mkdir(d, 0775);
        snprintf(p, sizeof p, "%s/%s.tsv", d, l->name); snprintf(tmp, sizeof tmp, "%s.tmp", p); FILE *o = fopen(tmp, "w"); if (!o) { perror(tmp); exit(1); }
        fprintf(o, "# The frozen slots of the highway list %s (%s). A slot never moves: a type no resource lists any longer is\n# retired and keeps its slot; a new type takes the next. Written by laplace highway; never edited by hand.\n", l->name, l->say);
        fprintf(o, "slot\tid\tstatus\tkey\n");
        for (size_t s = 0; s < n; s++) { char hx[33]; id_text(&t[s].id, hx); int64_t at = lp_idmap_find(first, &t[s].id);
            const char *key = at >= 0 && (size_t)at < fcap ? fkey[at] : s < nz_before ? fz[s].key : "";
            fprintf(o, "%zu\t%s\t%s\t%s\n", s, hx, s < nz_before && fz[s].retired ? "retired" : "live", key ? key : ""); }
        if (fclose(o) || rename(tmp, p)) { perror(p); exit(1); }
        free(fz);
    }
    lp_idmap_free(first); free(fkey);
}

int cmd_highway(int argc, char **argv){
    const char *outp = lp_highway_path();
    for (int a = 1; a < argc; a++) { if (!strcmp(argv[a], "-o") && a + 1 < argc) outp = argv[++a]; else { fprintf(stderr, "usage: laplace highway [-o highway.bin]\n"); return 2; } }
    setlocale(LC_NUMERIC, "en_US.UTF-8"); double T = now();
    int threads = omp_get_num_procs(); omp_set_num_threads(threads); omp_set_max_active_levels(1);
    tier0_open(NULL); table_init(); ctx_open(threads); HW = NULL;           /* the highway being made is read by nothing while it is made */
    Recipe *rc = NULL; int nrc = recipes_load(laplace_recipes(), &rc), ns; Source *src = sources_loaded(&ns);
    printf("laplace highway   the types, from the resources' recipes\n");
    Hw hw = { 0 }; H = &hw;
    for (int i = 0; i < ns; i++) {
        int says = 0; for (int k = 0; k < nrc; k++) says |= rc[k].source == i && !rc[k].broken && say_has_highway(&rc[k]);
        if (!says) continue;
        if (!src[i].found[0]) { printf("  %-28s not at any of its roots: what it says of the types is left out\n", src[i].name); continue; }
        if (recipes_broken(rc, nrc, &src[i])) return 2;
        char **paths; Recipe **of; int np = source_files(&src[i], rc, nrc, &paths, &of); size_t types0 = 0, keys0 = hw.nk, al0 = hw.na, e0 = hw.ne; for (int l = 0; l < hw.nl; l++) types0 += hw.l[l].n;
        for (int f = 0; f < np; f++) { if (say_has_highway(of[f])) { File one = { 0 }; one.path = paths[f]; one.recipe = of[f]; one.source = &src[i];
                #pragma omp parallel
                #pragma omp single
                highway_file(CTX[omp_get_thread_num()], &one, &hw); }
            free(paths[f]); }
        free(paths); free(of); size_t types1 = 0; for (int l = 0; l < hw.nl; l++) types1 += hw.l[l].n;
        printf("  %-28s %'zu types, %'zu keys, %'zu keys of other keys, %'zu mappings\n", src[i].name, types1 - types0, hw.nk - keys0, hw.na - al0, hw.ne - e0);
    }
    /* a key that names what another names: until no key is added */
    size_t added = 0; for (int more = 1; more; ) { more = 0;
        for (size_t i = 0; i < hw.na; i++) { Alias *a = &hw.a[i]; if (a->done) continue; Key *t = key_find(&hw, a->list, a->to); if (!t) continue; lp_id id = t->id; a->done = 1; if (key_put(&hw, a->list, a->key, &id)) { added++; more = 1; } } }
    /* every list in its frozen order: slots never move */
    size_t fz_new, fz_retired, fz_kept; freeze(&hw, &fz_new, &fz_retired, &fz_kept);
    printf("  slots: %'zu kept, %'zu new, %'zu retired (%s/slots)\n", fz_kept, fz_new, fz_retired, manifest_dir());
    /* the lists, one after another in the order they were first named; the edges by slot */
    uint32_t total = 0; for (int l = 0; l < hw.nl; l++) { hw.l[l].first = total; total += (uint32_t)hw.l[l].n; }
    Edge *ed = malloc(sizeof(Edge) * (hw.ne + 1)); size_t ne = 0, lost = 0;
    typedef struct { int la, lb; size_t n; char ex[2][96]; } Lost; Lost ls[64]; int nls = 0;          /* what was left out, by the lists, with one of each side that no list holds */
    for (size_t i = 0; i < hw.ne; i++) { const PEdge *p = &hw.e[i]; Key *a = key_find(&hw, p->la, p->ka), *b = key_find(&hw, p->lb, p->kb);
        int64_t from = a ? slot_in(&hw.l[p->la], &a->id) : -1, to = b ? slot_in(&hw.l[p->lb], &b->id) : -1;
        if (from < 0 || to < 0) { lost++; int q = 0; while (q < nls && !(ls[q].la == p->la && ls[q].lb == p->lb)) q++;
            if (q == nls && nls < 64) { memset(&ls[nls], 0, sizeof ls[0]); ls[nls].la = p->la; ls[nls].lb = p->lb; nls++; }
            if (q < nls) { ls[q].n++; if (from < 0 && !ls[q].ex[0][0]) snprintf(ls[q].ex[0], 96, "%s", p->ka); if (to < 0 && !ls[q].ex[1][0]) snprintf(ls[q].ex[1], 96, "%s", p->kb); }
            continue; }
        ed[ne++] = (Edge){ p->la, p->lb, (uint32_t)from, (uint32_t)to }; }
    qsort(ed, ne, sizeof(Edge), edge_cmp); size_t m = 0;
    for (size_t i = 0; i < ne; i++) if (!m || edge_cmp(&ed[i], &ed[m - 1])) ed[m++] = ed[i];
    ne = m;
    lp_tier0_record *rec = calloc(total + 1, sizeof(lp_tier0_record));
    for (int l = 0; l < hw.nl; l++) for (size_t s = 0; s < hw.l[l].n; s++) { lp_tier0_record *x = &rec[hw.l[l].first + s]; const Ref *r = &hw.l[l].t[s];
        x->id = r->id; memcpy(x->m, r->c.m, 32); x->hilbert = lp_hilbert4(&r->c); x->rank = (uint32_t)s; x->pad = r->tier; }
    FILE *o = fopen(outp, "wb"); if (!o) { perror(outp); return 1; }
    if (fwrite(rec, sizeof(lp_tier0_record), total, o) != total) { perror(outp); return 1; }
    for (size_t i = 0; i < ne; i++) { uint32_t pr[2] = { ed[i].from, ed[i].to }; fwrite(pr, 4, 2, o); }
    fclose(o);
    char lay[4300]; snprintf(lay, sizeof lay, "%s.layout", outp); o = fopen(lay, "w"); if (!o) { perror(lay); return 1; }
    fprintf(o, "# The highway: one record per type (as a tier-0 record, its rank the slot), the lists in the resources' order, then the edges as pairs of slots.\n");
    fprintf(o, "records\t%u\nedges-count\t%zu\n", total, ne);
    for (int i = 0; i < hw.nl; i++) fprintf(o, "list\t%s\t%s\t%u\t%zu\n", hw.l[i].name, hw.l[i].say, hw.l[i].first, hw.l[i].n);
    for (size_t i = 0; i < ne; ) { size_t j = i; while (j < ne && ed[j].la == ed[i].la && ed[j].lb == ed[i].lb) j++; fprintf(o, "edges\t%s\t%s\t%zu\t%zu\n", hw.l[ed[i].la].name, hw.l[ed[i].lb].name, i, j - i); i = j; }
    /* the banks (manifest/banks.tsv): one mask per semantic group, on the row its group describes; a value's bit in its
     * bank is its frozen slot, so nothing is packed and nothing moves. A list grown past its bank's width is refused:
     * the bank is widened in the manifest, never spilled into another */
    int nbank = 0;
    { char bp[4200]; snprintf(bp, sizeof bp, "%s/banks.tsv", manifest_dir()); FILE *bf = fopen(bp, "r"); if (!bf) { perror(bp); return 1; } char line[1024];
      while (fgets(line, sizeof line, bf)) { if (line[0] == '#' || !strncmp(line, "bank\t", 5)) continue;
          char *save = NULL, *nm = strtok_r(line, "\t\n", &save), *ls = strtok_r(NULL, "\t\n", &save), *gr = strtok_r(NULL, "\t\n", &save), *ca = strtok_r(NULL, "\t\n", &save), *wd = strtok_r(NULL, "\t\n", &save);
          if (!nm || !ls || !gr || !ca || !wd) { fprintf(stderr, "%s: a bank is: bank list group carrier width\n", bp); return 1; }
          size_t width = strtoul(wd, NULL, 10), have = 0; int li = -1;
          if (strcmp(ls, "-")) { for (int i = 0; i < hw.nl; i++) if (!strcmp(hw.l[i].name, ls)) li = i;
              if (li < 0) { fprintf(stderr, "%s: bank %s names the list %s, which no resource lists\n", bp, nm, ls); return 1; } have = hw.l[li].n; }
          if (have > width || width > 256) { fprintf(stderr, "%s: bank %s holds %zu values in %zu bits: widen it (at most 256)\n", bp, nm, have, width); return 1; }
          fprintf(o, "bank\t%s\t%s\t%s\t%s\t%zu\n", nm, ls, gr, ca, width); nbank++; }
      fclose(bf); }
    int bit = nbank;
    fclose(o);
    /* the keys the resources point at their types with, beside the highway: resolved by readers, recorded nowhere */
    snprintf(lay, sizeof lay, "%s.keys", outp); o = fopen(lay, "w"); if (!o) { perror(lay); return 1; } size_t nkeys = 0, stray = 0;
    fprintf(o, "# The keys the resources point at their types with: list, the key as the resource writes it, the slot. Resolved by readers, recorded nowhere.\n");
    for (size_t i = 0; i < hw.nk; i++) { int64_t s = slot_in(&hw.l[hw.k[i].list], &hw.k[i].id); if (s < 0) { stray++; continue; } fprintf(o, "%s\t%s\t%lld\n", hw.l[hw.k[i].list].name, hw.k[i].key, (long long)s); nkeys++; }
    fclose(o);
    /* the contents: every node under every type, before what holds it; then which type each list's slot is */
    snprintf(lay, sizeof lay, "%s.nodes", outp); o = fopen(lay, "w"); if (!o) { perror(lay); return 1; } Seen seen = { 0 };
    fprintf(o, "# The content of every type, as the composition it is. N: a node, its ID, its tier and its path (each child: its ID, or U and a code point; what it is said to be; its run). S: a list's slot and the ID of its content. Recorded by laplace deploy.\n");
    for (int l = 0; l < hw.nl; l++) for (size_t s = 0; s < hw.l[l].n; s++) node_write(o, &seen, &hw.l[l].t[s].id, 0);
    for (int l = 0; l < hw.nl; l++) for (size_t s = 0; s < hw.l[l].n; s++) { const lp_id *id = &hw.l[l].t[s].id; int64_t cp = lp_tier0_codepoint(T0, id); char c[33];
        if (cp >= 0) snprintf(c, sizeof c, "U%llX", (long long)cp); else hexid(id, c); fprintf(o, "S\t%s\t%zu\t%s\n", hw.l[l].name, s, c); }
    fclose(o);
    for (int l = 0; l < hw.nl; l++) printf("  %-28s %'zu\n", hw.l[l].say[0] ? hw.l[l].say : hw.l[l].name, hw.l[l].n);
    if (lost) { printf("  %'zu mappings name a key no list holds, or a type of another list: left out\n", lost);
        for (int q = 0; q < nls; q++) printf("    %-10s to %-10s %'9zu   for instance %s%s%s%s\n", hw.l[ls[q].la].name, hw.l[ls[q].lb].name, ls[q].n, ls[q].ex[0][0] ? ls[q].ex[0] : "", ls[q].ex[0][0] && ls[q].ex[1][0] ? ", " : "", ls[q].ex[1][0] ? ls[q].ex[1] : "", ""); }
    if (stray) printf("  %'zu keys name something that is not a type of their list: left out\n", stray);
    uint8_t fp[32]; blake3_hasher hs; blake3_hasher_init(&hs); blake3_hasher_update(&hs, rec, total * sizeof(lp_tier0_record)); for (size_t i = 0; i < ne; i++) { uint32_t pr[2] = { ed[i].from, ed[i].to }; blake3_hasher_update(&hs, pr, 8); } blake3_hasher_finalize(&hs, fp, 32);
    printf("\n%s: %'u types in %d lists, %'zu edges, %'zu keys beside them (%'zu by way of other keys); %d banks   (%.1f s)\nfingerprint ", outp, total, hw.nl, ne, nkeys, added, bit, now() - T);
    for (int i = 0; i < 32; i++) printf("%02x", fp[i]); printf("\n");
    (void)nrc; return 0;
}
