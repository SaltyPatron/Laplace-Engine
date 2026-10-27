/* The Highway contains canonical identifier components, their Tier-0 composition,
 * frozen enum banks, lookup aliases and crosswalk edges in one mapped file.
 * Corpus prose belongs to source-attested claims, never to cache identity. */
#define _GNU_SOURCE
#include "engine.h"
#include "blake3.h"
#include "laplace_config.h"
#include "laplace/highway.h"
#include <locale.h>
#include <stdarg.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <glob.h>
#include <zlib.h>

typedef struct { char name[32], say[64]; Ref *t; size_t n, cap; lp_idmap *map; uint32_t first; int bank; } List;
typedef struct { int list; lp_id id; uint32_t more; } Key;                   /* a resource's key can name several existing concepts */
typedef struct { int list; char *key, *to; } Alias;
typedef struct { int la, lb; char *ka, *kb; } PEdge;
typedef struct { int la, lb; uint32_t from, to; } Edge;
struct Hw { List *l; int nl; lp_strmap *keys; lp_vec(Key) extra_keys; lp_vec(Alias) a; lp_vec(PEdge) e; };
static int list_of(Hw *h, const char *name, const char *say){
    for (int i = 0; i < h->nl; i++) if (!strcmp(h->l[i].name, name)) { if (say && !h->l[i].say[0]) snprintf(h->l[i].say, sizeof h->l[i].say, "%s", say); return i; }
    h->l = xrealloc(h->l, sizeof(List) * (size_t)(h->nl + 1)); List *l = &h->l[h->nl]; memset(l, 0, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name); if (say) snprintf(l->say, sizeof l->say, "%s", say); return h->nl++;
}
void hw_list(Hw *h, const char *list, const char *say){ list_of(h, list, say); }
static int64_t slot_in(const List *l, const lp_id *id){
    int64_t at=lp_idmap_find(l->map,id); return at<0 ? -1 : (int64_t)*lp_idmap_value(l->map,(size_t)at);
}
/* A type of a list: its slot is the next, unless its content is in the list already. */
void hw_type(Hw *h, const char *list, const char *say, Ref thing){
    int li = list_of(h, list, say); List *l = &h->l[li]; if (slot_in(l, &thing.id) >= 0) return;      /* the list first: naming a new one moves them all */
    if (!l->map) l->map = lp_idmap_new();
    *lp_idmap_value(l->map,lp_idmap_put(l->map,&thing.id,NULL))=(uint32_t)l->n;
    lp_reserve((void **)&l->t, &l->cap, l->n + 1, sizeof(Ref)); l->t[l->n++] = thing;
}
/* The map's key: the list's place, then the key's bytes. */
static size_t key_of(int list, const char *key, char *out){ size_t n = strlen(key); if (n > 1000) n = 1000; memcpy(out, &list, sizeof list); memcpy(out + sizeof list, key, n); return sizeof list + n; }
static Key *key_find(Hw *h, int list, const char *key){ char k[1024]; return h->keys ? lp_strmap_lookup(h->keys, k, key_of(list, key, k)) : NULL; }
static int key_put(Hw *h, int list, const char *key, const lp_id *id){
    if (!h->keys) h->keys = lp_strmap_sized(sizeof(Key));
    char k[1024]; bool fresh; Key *x = lp_strmap_get(h->keys, k, key_of(list, key, k), &fresh);
    if (!fresh) {
        for (const Key *target=x; target; target=target->more ? h->extra_keys.v+target->more-1 : NULL)
            if (lp_id_eq(&target->id,id)) return 0;
        if (h->extra_keys.n>=UINT32_MAX-1) { fputs("too many highway key targets\n",stderr); exit(1); }
        Key extra={list,*id,x->more}; lp_push(&h->extra_keys,extra); x->more=(uint32_t)h->extra_keys.n; return 1;
    }
    *x=(Key){list,*id,0}; return 1;
}
void hw_key(Hw *h, const char *list, Ref thing, const char *key){ key_put(h, list_of(h, list, NULL), key, &thing.id); }
void hw_alias(Hw *h, const char *list, const char *key, const char *to){
    lp_push(&h->a, (Alias){ list_of(h, list, NULL), strdup(key), strdup(to) });
}
void hw_edge(Hw *h, const char *la, const char *ka, const char *lb, const char *kb){
    lp_push(&h->e, (PEdge){ list_of(h, la, NULL), list_of(h, lb, NULL), strdup(ka), strdup(kb) });
}
static Hw *H;
static int edge_cmp(const void *x, const void *y){ const Edge *a = x, *b = y; int c = strcmp(H->l[a->la].name, H->l[b->la].name); if (c) return c; c = strcmp(H->l[a->lb].name, H->l[b->lb].name); if (c) return c;
    return a->from < b->from ? -1 : a->from > b->from ? 1 : a->to < b->to ? -1 : a->to > b->to; }

/* ---- frozen slots (decision: enumerate first, then append forever). A type's slot is what a mask bit, a vertex
 * value or a stored reference to it means; at billions of rows it can never move. Each list's slots are a registry of
 * their own, versioned with the manifests (manifest/slots/LIST.tsv): a type the registry holds keeps its slot; a type
 * no resource lists any longer keeps its slot, retired (a tombstone: never reused); a new type takes the next slot.
 * The order the resources happen to be read in decides nothing. */
static const char *manifest_dir(void){ const char *e = getenv("LAPLACE_MANIFEST"); return e && *e ? e : LAPLACE_MANIFEST_DEFAULT; }
static int lists_read(Hw *h){
    char path[4200],line[1024]; snprintf(path,sizeof path,"%s/slots/lists.tsv",manifest_dir());
    FILE *f=fopen(path,"r"); if (!f) { perror(path); return 0; }
    int ok=1;
    while (fgets(line,sizeof line,f)) {
        if (line[0]=='#' || !strncmp(line,"list\t",5)) continue;
        char *tab=strchr(line,'\t'); if (!tab) { ok=0; break; } *tab++=0;
        tab[strcspn(tab,"\r\n")]=0;
        if (!*line || strlen(line)>=32 || !*tab || strlen(tab)>=64) { ok=0; break; }
        list_of(h,line,tab);
    }
    ok=ok && !ferror(f); fclose(f); return ok && h->nl;
}
typedef struct { char name[32], list[32], group[16], carrier[16]; unsigned width, offset; int flags, li; } BankSpec;
static int banks_read(Hw *h, BankSpec banks[64]){
    char path[4200], line[1024]; snprintf(path, sizeof path, "%s/banks.tsv", manifest_dir());
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return -1; }
    int count = 0, bad = 0; uint64_t occupied = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || !strncmp(line, "bank\t", 5) || line[0] == '\n' || line[0] == '\r') continue;
        char *v[8], *save = NULL; int n = 0;
        for (char *p = strtok_r(line, "\t\r\n", &save); p && n < 8; p = strtok_r(NULL, "\t\r\n", &save)) v[n++] = p;
        if (n != 7 || count == 64 || strlen(v[0]) >= 32 || strlen(v[1]) >= 32 || strlen(v[2]) >= 16 || strlen(v[3]) >= 16) { bad = 1; break; }
        char *end; unsigned long width = strtoul(v[4], &end, 10); if (!*v[4] || *end || width < 1 || width > 64) { bad = 1; break; }
        unsigned long offset = strtoul(v[5], &end, 10); if (!*v[5] || *end || offset > 64 - width) { bad = 1; break; }
        if (strcmp(v[6], "enum") && strcmp(v[6], "flags")) { bad = 1; break; }
        lp_bank descriptor = { .width = (uint16_t)width, .offset = (uint16_t)offset };
        uint64_t field = lp_mask_field(&descriptor); if (occupied & field) { bad = 1; break; } occupied |= field;
        for (int i = 0; i < count; i++) if (!strcmp(banks[i].name, v[0]) || !strcmp(banks[i].list, v[1])) bad = 1;
        if (bad) break;
        BankSpec *b = banks + count++; memset(b, 0, sizeof *b);
        snprintf(b->name, sizeof b->name, "%s", v[0]); snprintf(b->list, sizeof b->list, "%s", v[1]);
        snprintf(b->group, sizeof b->group, "%s", v[2]); snprintf(b->carrier, sizeof b->carrier, "%s", v[3]);
        b->width = (unsigned)width; b->offset = (unsigned)offset; b->flags = !strcmp(v[6], "flags");
        b->li = list_of(h, b->list, b->name); h->l[b->li].bank = 1;
    }
    bad |= ferror(f) || !count; fclose(f);
    if (bad) { fprintf(stderr, "%s: expected non-overlapping bank/list/group/carrier/width/offset/encoding fields in 64 bits\n", path); return -1; }
    return count;
}

typedef struct { lp_id id; int retired; char key[96], p1[160], p2[160]; } Frozen;
static Frozen *frozen_read(const char *list, size_t *n){
    char p[4096]; snprintf(p, sizeof p, "%s/slots/%s.tsv", manifest_dir(), list); FILE *f = fopen(p, "r"); *n = 0; if (!f) return NULL;
    Frozen *fz = NULL; size_t cap = 0; char line[1024];
    while (fgets(line, sizeof line, f)) { if (line[0] == '#' || !strncmp(line, "slot\t", 5)) continue;
        char *save = NULL, *slot = strtok_r(line, "\t\r\n", &save), *id = strtok_r(NULL, "\t\r\n", &save), *st = strtok_r(NULL, "\t\r\n", &save),
             *key = strtok_r(NULL, "\t\r\n", &save), *p1 = strtok_r(NULL, "\t\r\n", &save), *p2 = strtok_r(NULL, "\t\r\n", &save);
        if (!slot || !id || !st) continue; size_t s = strtoull(slot, NULL, 10);
        if (s != *n) { fprintf(stderr, "%s: slot %zu where %zu was due: a slot registry is never edited by hand\n", p, s, *n); exit(1); }
        if (*n == cap) { cap = cap ? cap * 2 : 1024; fz = xrealloc(fz, sizeof(Frozen) * cap); }
        Frozen *z = &fz[(*n)++]; memset(z, 0, sizeof *z); if (!id_parse(id, &z->id)) { fprintf(stderr, "%s: slot %zu: not an ID\n", p, s); exit(1); }
        z->retired = !strcmp(st, "retired");
        if (key) snprintf(z->key, sizeof z->key, "%s", key);
        if (p1) snprintf(z->p1, sizeof z->p1, "%s", p1);
        if (p2) snprintf(z->p2, sizeof z->p2, "%s", p2); }
    fclose(f); return fz;
}
static unsigned frozen_parts(const Frozen *slot,const char *fallback,const char **parts){
    parts[0]=slot && slot->p1[0] ? slot->p1 : fallback;
    if (slot && slot->p2[0]) { parts[1]=slot->p2; return 2; }
    return parts[0] && *parts[0] ? 1 : 0;
}
static Ref identifier_ref(const char *const *parts,unsigned n){
    Ref child[2];
    for (unsigned i=0; i<n; i++) child[i]=text_ref(CTX[0],(const uint8_t *)parts[i],strlen(parts[i]));
    return n==1 ? child[0] : compose(child,n,ref_above(child,n));
}
/* A foundation change can be reviewed against the declared components without
 * changing a registry or building a cache. Slot positions and edges stay stable. */
static int identifier_report(Hw *h){
    puts("list\tslot\tstored_id\tcanonical_id");
    for (int li=0; li<h->nl; li++) {
        size_t count; Frozen *slots=frozen_read(h->l[li].name,&count);
        for (size_t i=0; i<count; i++) {
            const Frozen *slot=&slots[i]; lp_id canonical=slot->id;
            if (slot->key[0]) {
                const char *parts[2]; unsigned n=frozen_parts(slot,slot->key,parts);
                canonical=identifier_ref(parts,n).id;
            } else if (!slot->retired) { free(slots); return 1; }
            char stored[33],computed[33]; id_text(&slot->id,stored); id_text(&canonical,computed);
            printf("%s\t%zu\t%s\t%s\n",h->l[li].name,i,stored,computed);
        }
        free(slots);
    }
    return ferror(stdout) ? 1 : 0;
}
/* Slots retain their positions even when an old prose-derived ID is corrected.
 * Aliases are redirected in one pass, rather than rescanning all keys per slot. */
static void freeze(Hw *h, size_t *added, size_t *retired, size_t *kept){
    *added=*retired=*kept=0;
    for (int li=0; li<h->nl; li++) {
        List *l=h->l+li; size_t nz; Frozen *fz=frozen_read(l->name,&nz);
        lp_idmap *first=lp_idmap_sized(sizeof(char *)), *remap=lp_idmap_sized(sizeof(lp_id));
        for (size_t k=0; k<lp_strmap_count(h->keys); k++) {
            for (Key *entry=lp_strmap_at(h->keys,k); entry; entry=entry->more ? h->extra_keys.v+entry->more-1 : NULL) {
                if (entry->list!=li) continue;
                bool fresh; size_t at=lp_idmap_put(first,&entry->id,&fresh);
                if (fresh) { size_t len; const char *key=lp_strmap_key(h->keys,k,&len)+sizeof(int); *(const char **)lp_idmap_at(first,at)=key; }
            }
        }
        size_t capacity=nz+l->n+1; Ref *t=xrealloc(NULL,sizeof *t*capacity); size_t n=0;
        uint8_t *placed=calloc(l->n+1,1);
        for (size_t slot=0; slot<nz; slot++) {
            const char *key=fz[slot].key;
            if (!*key && fz[slot].retired && !l->bank) {
                memset(t+n,0,sizeof *t); t[n++].tier=UINT8_MAX; (*retired)++; continue;
            }
            if (!*key) { fprintf(stderr,"%s slot %zu has no identifier\n",l->name,slot); exit(1); }
            const char *parts[2]; unsigned np=frozen_parts(fz+slot,key,parts);
            Ref value=identifier_ref(parts,np); t[n++]=value;
            if (!lp_id_eq(&value.id,&fz[slot].id)) { fprintf(stderr,"%s slot %zu: frozen ID disagrees with its identifier components\n",l->name,slot); exit(1); }
            *(lp_id *)lp_idmap_at(remap,lp_idmap_put(remap,&fz[slot].id,NULL))=value.id;
            Key *entry=key_find(h,li,key);
            int64_t old=entry ? slot_in(l,&entry->id) : slot_in(l,&fz[slot].id);
            if (old>=0) {
                placed[old]=1;
                *(lp_id *)lp_idmap_at(remap,lp_idmap_put(remap,&l->t[old].id,NULL))=value.id;
            }
            (*kept)++; if (fz[slot].retired) (*retired)++;
        }
        for (size_t old=0; old<l->n; old++) if (!placed[old]) {
            int64_t at=lp_idmap_find(first,&l->t[old].id);
            if (at<0) { fprintf(stderr,"%s: an unkeyed identifier cannot be appended\n",l->name); exit(1); }
            const char *key=*(const char **)lp_idmap_at(first,(size_t)at);
            Ref value=text_ref(CTX[0],(const uint8_t *)key,strlen(key)); t[n++]=value;
            if (!lp_id_eq(&value.id,&l->t[old].id)) { fprintf(stderr,"%s: register the structured identifier components before appending key %s\n",l->name,key); exit(1); }
            *(lp_id *)lp_idmap_at(remap,lp_idmap_put(remap,&l->t[old].id,NULL))=value.id; (*added)++;
        }
        for (size_t k=0; k<lp_strmap_count(h->keys); k++) {
            for (Key *entry=lp_strmap_at(h->keys,k); entry; entry=entry->more ? h->extra_keys.v+entry->more-1 : NULL) {
                if (entry->list!=li) continue;
                int64_t at=lp_idmap_find(remap,&entry->id); if (at>=0) entry->id=*(lp_id *)lp_idmap_at(remap,(size_t)at);
            }
        }
        for (size_t slot=0; slot<nz; slot++) {
            if (t[slot].tier==UINT8_MAX) continue;
            Key *entry=key_find(h,li,fz[slot].key);
            if (entry) entry->id=t[slot].id; else key_put(h,li,fz[slot].key,&t[slot].id);
            if (fz[slot].p1[0] && !fz[slot].p2[0]) key_put(h,li,fz[slot].p1,&t[slot].id);
            if (!strcmp(l->name,"vnclass")) {
                const char *dash=strchr(fz[slot].key,'-');
                if (dash && dash[1]>='0' && dash[1]<='9') key_put(h,li,dash+1,&t[slot].id);
            }
            if (!strcmp(l->name,"lexfile")) { char number[16]; snprintf(number,sizeof number,"%02zu",slot); key_put(h,li,number,&t[slot].id); }
        }
        free(l->t); l->t=t; l->n=n; l->cap=capacity;
        lp_idmap_free(l->map); l->map=lp_idmap_new();
        for (size_t slot=0; slot<n; slot++) {
            if (t[slot].tier==UINT8_MAX) continue;
            bool fresh; size_t at=lp_idmap_put(l->map,&t[slot].id,&fresh);
            *lp_idmap_value(l->map,at)=(uint32_t)slot;
            if (!fresh) { fprintf(stderr,"%s: duplicate identifier at slot %zu\n",l->name,slot); exit(1); }
        }
        free(placed); free(fz); lp_idmap_free(first); lp_idmap_free(remap);
    }
}
static int frozen_keys(Hw *h){
    char path[4200],line[4096]; snprintf(path,sizeof path,"%s/slots/keys.tsv.gz",manifest_dir());
    gzFile file=gzopen(path,"rb"); if (!file) { perror(path); return 0; }
    int ok=1;
    while (gzgets(file,line,sizeof line)) {
        if (line[0]=='#') continue;
        line[strcspn(line,"\r\n")]=0;
        char *field[4]={line}; int nf=1;
        for (char *p=line; *p; p++) if (*p=='\t') {
            if (nf==4) { ok=0; break; } *p=0; field[nf++]=p+1;
        }
        if (!ok || (nf!=3 && nf!=4)) { ok=0; break; }
        char *space=field[0], *key=field[1], *target=nf==4 ? field[2] : space, *slot=field[nf-1];
        if (!*space || strlen(space)>=sizeof h->l[0].name || !*key || strlen(key)>1000 || *slot<'0' || *slot>'9') { ok=0; break; }
        char *end; unsigned long at=strtoul(slot,&end,10);
        int li=-1; for (int i=0; i<h->nl; i++) if (!strcmp(h->l[i].name,target)) li=i;
        if (*end || li<0 || at>=h->l[li].n || h->l[li].t[at].tier==UINT8_MAX) { ok=0; break; }
        lp_id id=h->l[li].t[at].id; int source=list_of(h,space,NULL);
        Key *existing=key_find(h,source,key);
        if (nf==3 && existing && !lp_id_eq(&existing->id,&id)) { ok=0; break; }
        key_put(h,source,key,&id);
    }
    if (!gzeof(file)) ok=0;
    if (gzclose(file)!=Z_OK) ok=0;
    if (!ok) fprintf(stderr,"%s: invalid frozen lookup key\n",path);
    return ok;
}
typedef struct { uint8_t *b; size_t n,cap; } HwBuffer;
static void append(HwBuffer *buf,const void *bytes,size_t size){
    if (size>SIZE_MAX-buf->n) { fputs("highway section too large\n",stderr); exit(1); }
    lp_reserve((void **)&buf->b,&buf->cap,buf->n+size+1,1);
    memcpy(buf->b+buf->n,bytes,size); buf->n+=size; buf->b[buf->n]=0;
}
static void line(HwBuffer *buf,const char *format,...){
    char text[4096]; va_list args; va_start(args,format); int size=vsnprintf(text,sizeof text,format,args); va_end(args);
    if (size<0 || (size_t)size>=sizeof text) { fputs("highway descriptor too long\n",stderr); exit(1); }
    append(buf,text,(size_t)size);
}
/* A file written fresh beside its old self and swapped in: table_init maps the highway that is there, and on Windows a
 * mapped file cannot be truncated in place, while it can be renamed away. */
static int swap_in(const char *path, const char *fresh){
    char old[4300]; snprintf(old, sizeof old, "%s.old", path); remove(old);
    os_rename(path, old);                                                    /* nothing to move aside is fine */
    if (os_rename(fresh, path)) { perror(path); return 1; }
    remove(old);                                                          /* still mapped: left for the next run */
    return 0;
}

int cmd_highway(int argc, char **argv){
    const char *outp = lp_highway_path();
    int fingerprint = 0, refresh = 0, identities = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-o") && a + 1 < argc) outp = argv[++a];
        else if (!strcmp(argv[a], "--fingerprint")) fingerprint = 1;
        else if (!strcmp(argv[a], "--refresh")) refresh = 1;
        else if (!strcmp(argv[a], "--identities")) identities = 1;
        else { fprintf(stderr, "usage: laplace highway [-o highway.bin] [--fingerprint] [--refresh] [--identities]\n"); return 2; }
    }
    if (identities && (fingerprint || refresh)) { fputs("--identities is an independent registry read\n",stderr); return 2; }
    if (fingerprint) {
        const lp_highway *h = lp_highway_map(outp);
        if (!h) { fprintf(stderr, "cannot map existing highway at %s\n", outp); return 1; }
        uint8_t bytes[32]; char hex[65]; lp_highway_fingerprint(h, bytes); lp_hex(bytes, 32, hex);
        printf("fingerprint %s\n", hex);
        return 0;
    }
    setlocale(LC_NUMERIC, "en_US.UTF-8"); double T = now();
    int threads = omp_get_num_procs(); omp_set_num_threads(threads); omp_set_max_active_levels(1);
    tier0_open(NULL); table_init(); ctx_open(threads); HW = NULL;           /* the highway being made is read by nothing while it is made */
    Recipe *rc = NULL; int ns=0,nrc=refresh ? recipes_load(laplace_recipes(), &rc) : 0;
    Source *src=refresh ? sources_loaded(&ns) : NULL;
    if (!identities) { printf("laplace highway   canonical identifiers from frozen manifests\n"); fflush(stdout); }
    Hw hw = { 0 }; H = &hw;
    if (!lists_read(&hw)) return 1;
    if (identities) return identifier_report(&hw);
    int any_absent = !refresh;
    for (int i = 0; i < ns; i++) {
        int says = 0; for (int k = 0; k < nrc; k++) says |= rc[k].source == i && !rc[k].broken && say_has_highway(&rc[k]);
        if (!says) continue;
        for (int k = 0; k < nrc; k++) if (rc[k].source == i && !rc[k].broken && say_has_highway(&rc[k])) say_highway_lists(&rc[k], &hw);
        if (!refresh) continue;
        if (src[i].unset[0]) { fprintf(stderr, "source %s: its root names $%s, which the environment does not set (Laplace-Operations laplace.env)\n", src[i].name, src[i].unset); return 2; }
        if (!src[i].found[0]) { any_absent = 1; printf("  %-28s not at any of its roots: frozen slots reconstructed from manifest/slots\n", src[i].name); fflush(stdout); continue; }
        if (recipes_broken(rc, nrc, &src[i])) return 2;
        char **paths; Recipe **of; int np = source_files(&src[i], rc, nrc, &paths, &of); size_t types0 = 0, keys0 = lp_strmap_count(hw.keys), al0 = hw.a.n, e0 = hw.e.n; for (int l = 0; l < hw.nl; l++) types0 += hw.l[l].n;
        for (int f = 0; f < np; f++) { if (say_has_highway(of[f])) { File one = { 0 }; one.path = paths[f]; one.recipe = of[f]; one.source = &src[i];
                #pragma omp parallel
                #pragma omp single
                highway_file(CTX[omp_get_thread_num()], &one, &hw); }
            free(paths[f]); }
        free(paths); free(of); size_t types1 = 0; for (int l = 0; l < hw.nl; l++) types1 += hw.l[l].n;
        printf("  %-28s %'zu types, %'zu keys, %'zu keys of other keys, %'zu mappings\n", src[i].name, types1 - types0, lp_strmap_count(hw.keys) - keys0, hw.a.n - al0, hw.e.n - e0);
        fflush(stdout);
    }
    /* a key that names what another names: until no key is added */
    size_t added = 0; for (int more = 1; more; ) { more = 0;
        for (size_t i = 0; i < hw.a.n; i++) { Alias *a = &hw.a.v[i]; Key *t = key_find(&hw, a->list, a->to); if (!t) continue;
            lp_id id=t->id; lp_vec(lp_id) alternatives={0};
            for (uint32_t next=t->more; next; next=hw.extra_keys.v[next-1].more) lp_push(&alternatives,hw.extra_keys.v[next-1].id);
            if (key_put(&hw,a->list,a->key,&id)) { added++; more=1; }
            for (size_t j=0; j<alternatives.n; j++) if (key_put(&hw,a->list,a->key,alternatives.v+j)) { added++; more=1; }
            lp_vec_free(&alternatives);
        } }
    BankSpec banks[64]; int nbank = banks_read(&hw, banks); if (nbank < 0) return 1;
    /* every list in its frozen order: slots never move */
    size_t fz_new, fz_retired, fz_kept; freeze(&hw, &fz_new, &fz_retired, &fz_kept);
    if (!frozen_keys(&hw)) return 1;
    for (int i = 0; i < nbank; i++) {
        const BankSpec *b = banks + i; size_t have = hw.l[b->li].n;
        uint64_t capacity = b->flags ? b->width : b->width == 64 ? UINT64_MAX : (UINT64_C(1) << b->width) - 1;
        if (!have || have > capacity) { fprintf(stderr, "bank %s: %zu slots do not fit its %u-bit field\n", b->name, have, b->width); return 1; }
    }
    printf("  slots: %'zu kept, %'zu new, %'zu retired (%s/slots)\n", fz_kept, fz_new, fz_retired, manifest_dir());
    /* the lists, one after another in the order they were first named; the edges by slot */
    uint32_t total = 0; for (int l = 0; l < hw.nl; l++) { hw.l[l].first = total; total += (uint32_t)hw.l[l].n; }
    size_t ecap = hw.e.n + 1; Edge *ed = malloc(sizeof(Edge) * ecap); size_t ne = 0, lost = 0;
    typedef struct { int la, lb; size_t n; char ex[2][96]; } Lost; Lost ls[64]; int nls = 0;          /* what was left out, by the lists, with one of each side that no list holds */
    for (size_t i = 0; i < hw.e.n; i++) { const PEdge *p = &hw.e.v[i]; Key *a = key_find(&hw, p->la, p->ka), *b = key_find(&hw, p->lb, p->kb);
        int64_t from = a && !a->more ? slot_in(&hw.l[p->la], &a->id) : -1, to = b && !b->more ? slot_in(&hw.l[p->lb], &b->id) : -1;
        if (from < 0 || to < 0) { lost++; int q = 0; while (q < nls && !(ls[q].la == p->la && ls[q].lb == p->lb)) q++;
            if (q == nls && nls < 64) { memset(&ls[nls], 0, sizeof ls[0]); ls[nls].la = p->la; ls[nls].lb = p->lb; nls++; }
            if (q < nls) { ls[q].n++; if (from < 0 && !ls[q].ex[0][0]) snprintf(ls[q].ex[0], 96, "%s", p->ka); if (to < 0 && !ls[q].ex[1][0]) snprintf(ls[q].ex[1], 96, "%s", p->kb); }
            continue; }
        ed[ne++] = (Edge){ p->la, p->lb, (uint32_t)from, (uint32_t)to }; }
    if (any_absent) {
        char ep[4200]; snprintf(ep, sizeof ep, "%s/slots/edges*.tsv", manifest_dir());
        glob_t paths={0}; if (glob(ep,0,NULL,&paths)) { fprintf(stderr,"%s: no frozen crosswalks\n",ep); return 1; }
        for (size_t file=0; file<paths.gl_pathc; file++) {
        FILE *ef = fopen(paths.gl_pathv[file], "r");
        if (ef) {
            char line[512];
            while (fgets(line, sizeof line, ef)) {
                if (line[0] == '#' || !strncmp(line, "from_list\t", 10)) continue;
                char *save = NULL, *la = strtok_r(line, "\t\r\n", &save), *fa = strtok_r(NULL, "\t\r\n", &save), *lb = strtok_r(NULL, "\t\r\n", &save), *tb = strtok_r(NULL, "\t\r\n", &save);
                if (!la || !fa || !lb || !tb) continue;
                int ia = list_of(&hw, la, NULL), ib = list_of(&hw, lb, NULL);
                if (ne == ecap) { ecap = ecap ? ecap * 2 : 8192; ed = xrealloc(ed, sizeof(Edge) * ecap); }
                ed[ne++] = (Edge){ ia, ib, (uint32_t)strtoul(fa, NULL, 10), (uint32_t)strtoul(tb, NULL, 10) };
            }
            fclose(ef);
        }
        }
        globfree(&paths);
    }
    /* Crosswalks support interchange in both directions. */
    size_t forward=ne;
    if (forward>SIZE_MAX/2/sizeof *ed) return 1;
    if (ecap<2*forward) { ecap=2*forward; ed=xrealloc(ed,sizeof *ed*ecap); }
    for (size_t i=0; i<forward; i++) ed[ne++]=(Edge){ed[i].lb,ed[i].la,ed[i].to,ed[i].from};
    qsort(ed, ne, sizeof(Edge), edge_cmp); size_t m = 0;
    for (size_t i = 0; i < ne; i++) if (!m || edge_cmp(&ed[i], &ed[m - 1])) ed[m++] = ed[i];
    ne = m;
    /* no resource at any of its roots writes nothing: the highway that is there stays (a run under the wrong LAPLACE_DATA
     * must not swap an empty one in) */
    if (!total) { fprintf(stderr, "laplace highway: no resource was found at any of its roots: %s is left as it is\n", outp); return 1; }
    lp_tier0_record *rec = calloc(total + 1, sizeof(lp_tier0_record));
    for (int l = 0; l < hw.nl; l++) for (size_t s = 0; s < hw.l[l].n; s++) { lp_tier0_record *x = &rec[hw.l[l].first + s]; const Ref *r = &hw.l[l].t[s];
        x->id = r->id; memcpy(x->m, r->c.m, 32); x->hilbert = lp_hilbert4(&r->c); x->rank = (uint32_t)s; x->pad = r->tier==UINT8_MAX ? LP_HIGHWAY_RETIRED : r->tier; }
    HwBuffer layout={0}, keys={0}, identifiers={0};
    line(&layout,"records\t%u\nedges-count\t%zu\n",total,ne);
    for (int i=0; i<hw.nl; i++) {
        const List *list=hw.l+i;
        if (!list->n) continue; /* key-only namespaces own no records or mask slots */
        line(&layout,"list\t%s\t%s\t%u\t%zu\n",list->name,list->say[0]?list->say:list->name,list->first,list->n);
        size_t nz; Frozen *frozen=frozen_read(list->name,&nz);
        for (size_t slot=0; slot<list->n; slot++) {
            const char *identifier=slot<nz ? frozen[slot].key : NULL;
            if (!identifier) for (size_t k=0; k<lp_strmap_count(hw.keys); k++) {
                Key *key=lp_strmap_at(hw.keys,k);
                if (key->list==i && lp_id_eq(&key->id,&list->t[slot].id)) { size_t bytes; identifier=lp_strmap_key(hw.keys,k,&bytes)+sizeof(int); break; }
            }
            if (!identifier || (!*identifier && list->t[slot].tier!=UINT8_MAX)) { fprintf(stderr,"%s slot %zu has no identifier\n",list->name,slot); return 1; }
            const char *parts[2]; unsigned np=frozen_parts(slot<nz ? frozen+slot : NULL,identifier,parts);
            uint8_t count[4]={(uint8_t)np,0,0,0}; append(&identifiers,count,sizeof count);
            for (unsigned p=0; p<np; p++) append(&identifiers,parts[p],strlen(parts[p])+1);
        }
        free(frozen);
    }
    for (size_t i=0; i<ne;) {
        size_t j=i; while (j<ne && ed[j].la==ed[i].la && ed[j].lb==ed[i].lb) j++;
        line(&layout,"edges\t%s\t%s\t%zu\t%zu\n",hw.l[ed[i].la].name,hw.l[ed[i].lb].name,i,j-i); i=j;
    }
    for (int i=0; i<nbank; i++) {
        const BankSpec *b=banks+i;
        line(&layout,"bank\t%s\t%s\t%s\t%s\t%u\t%u\t%s\n",b->name,b->list,b->group,b->carrier,b->width,b->offset,b->flags?"flags":"enum");
    }
    size_t nkeys=0,nreferences=0,stray=0; lp_idindex *reference_ids=NULL;
    for (size_t i=0; i<lp_strmap_count(hw.keys); i++) {
        size_t bytes; const char *text=lp_strmap_key(hw.keys,i,&bytes)+sizeof(int);
        const Key *first=lp_strmap_at(hw.keys,i); int multiple=first->more!=0;
        for (const Key *key=first; key; key=key->more ? hw.extra_keys.v+key->more-1 : NULL) {
        int64_t slot=slot_in(hw.l+key->list,&key->id);
        if (slot<0) {
            if (!reference_ids) { reference_ids=lp_idindex_build(rec,total,sizeof *rec); if (!reference_ids) return 1; }
            uint64_t probe=0; int64_t at;
            while ((at=lp_idindex_find(reference_ids,&key->id,&probe))>=0 && rec[at].pad==LP_HIGHWAY_RETIRED) {}
            if (at<0) { stray++; continue; }
            /* Lists have contiguous frozen ranges. Empty key namespaces can
             * share a boundary but never own the record at that boundary. */
            int lo=0,hi=hw.nl;
            while (lo<hi) { int mid=lo+(hi-lo)/2; if (hw.l[mid].first<=(uint64_t)at) lo=mid+1; else hi=mid; }
            const List *target=hw.l+lo-1;
            line(&keys,"%s\t%.*s\t%s\t%llu\n",hw.l[key->list].name,(int)(bytes-sizeof(int)),text,
                target->name,(unsigned long long)((uint64_t)at-target->first));
            nreferences++; continue;
        }
        if (multiple) {
            line(&keys,"%s\t%.*s\t%s\t%lld\n",hw.l[key->list].name,(int)(bytes-sizeof(int)),text,hw.l[key->list].name,(long long)slot); nreferences++;
        } else { line(&keys,"%s\t%.*s\t%lld\n",hw.l[key->list].name,(int)(bytes-sizeof(int)),text,(long long)slot); nkeys++; }
        }
    }
    lp_idindex_free(reference_ids);
    lp_edge *edges=xrealloc(NULL,sizeof *edges*(ne+1));
    for (size_t i=0; i<ne; i++) edges[i]=(lp_edge){ed[i].from,ed[i].to};
    char fresh[4300]; snprintf(fresh,sizeof fresh,"%s.new",outp);
    if (!lp_highway_write(fresh,rec,total,edges,ne,(const char *)layout.b,layout.n,
        (const char *)keys.b,keys.n,(const char *)identifiers.b,identifiers.n)) { perror(fresh); return 1; }
    const lp_highway *checked=lp_highway_map(fresh);
    if (!checked) { fprintf(stderr,"%s: generated highway failed validation\n",fresh); remove(fresh); return 1; }
    uint8_t fp[32]; lp_highway_fingerprint(checked,fp);
    if (swap_in(outp,fresh)) return 1;
    int bit=nbank;
    for (int l = 0; l < hw.nl; l++) printf("  %-28s %'zu\n", hw.l[l].say[0] ? hw.l[l].say : hw.l[l].name, hw.l[l].n);
    if (lost) { printf("  %'zu mappings name a key no list holds, or a type of another list: left out\n", lost);
        for (int q = 0; q < nls; q++) printf("    %-10s to %-10s %'9zu   for instance %s%s%s%s\n", hw.l[ls[q].la].name, hw.l[ls[q].lb].name, ls[q].n, ls[q].ex[0][0] ? ls[q].ex[0] : "", ls[q].ex[0][0] && ls[q].ex[1][0] ? ", " : "", ls[q].ex[1][0] ? ls[q].ex[1] : "", ""); }
    if (nreferences) printf("  %'zu source pointer keys resolve to existing records (embedded in the highway)\n",nreferences);
    if (stray) printf("  %'zu keys name no active highway record: left out\n", stray);
    printf("\n%s: %'u types in %d lists, %'zu edges, %'zu embedded keys (%'zu by way of other keys); %d banks   (%.1f s)\nfingerprint ", outp, total, hw.nl, ne, nkeys+nreferences, added, bit, now() - T);
    for (int i = 0; i < 32; i++) printf("%02x", fp[i]); printf("\n");
    (void)nrc; return 0;
}
