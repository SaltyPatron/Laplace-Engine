/* Reading the web of claims. An entity is named by its text and found by the ID computed here; everything attested
 * about it is the claims whose paths hold it, fetched through the container index with their standings, one set-based
 * fetch per step: O(log N) to find them, O(K) to read them. How hard a strand tugs back is its claim's confidence.
 * Nothing here changes a standing: querying reads scores, it does not move them.
 *
 * This is not yet the forward pass. It is the lookups the forward pass is made of: what is attested about an entity
 * (hop), a claim with a part left open (hop a b ?), a word up to its concepts and down into another language
 * (translate, along the relations the firmware names), and how far one entity is from another (degrees).
 *
 *   laplace text  text
 *   laplace hop   [-d conninfo] [-n N] [--firmware FILE] text | subject predicate object (? for a part left open)
 *   laplace translate [-d conninfo] [-n N] [--firmware FILE] word from to...
 *   laplace degrees [-d conninfo] [-n N] [--firmware FILE] [--batch B] from [to]
 * How a standing is read, how far a search walks, how many claims are read and what is refused are the firmware's
 * decisions (firmware.c), not the program's; --k, --fan, --hops and --per-hop are there to measure against it. */
#include "query.h"
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>
#include <string.h>

/* ---- naming an entity: the text's trunk, computed on the client */
lp_ref entity_named(Ctx *c, const char *text, lp_ref *parts, size_t cap, size_t *np){
    lp_ref p[1]; size_t n;
    return lp_text_parts(c, (const uint8_t *)text, strlen(text), parts ? parts : p, parts ? cap : 1, np ? np : &n);
}
void show_ref(const char *label, const lp_ref *r){
    char id[33]; id_text(&r->id, id);
    printf("%-10s %s   tier %d   (%.6f, %.6f, %.6f, %.6f)   hilbert %016llx   depth %.6f\n", label, id, r->tier,
           (double)r->c.m[0] / LP_FIXED_ONE, (double)r->c.m[1] / LP_FIXED_ONE, (double)r->c.m[2] / LP_FIXED_ONE, (double)r->c.m[3] / LP_FIXED_ONE,
           (unsigned long long)lp_hilbert4(&r->c),
           1.0 - sqrt(((double)r->c.m[0] * r->c.m[0] + (double)r->c.m[1] * r->c.m[1] + (double)r->c.m[2] * r->c.m[2] + (double)r->c.m[3] * r->c.m[3])) / LP_FIXED_ONE);
}

int cmd_text(int argc, char **argv){
    int segments = 0;
    int a = opts(argc, argv, (const Opt[]){ { .flag="--segments", .kind='b', .at=&segments }, {0} });
    if (a >= argc) { fprintf(stderr, "usage: laplace text [--segments] text\n"); return 2; }
    double t = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0);
    if (segments) {
        const uint8_t *bytes = (const uint8_t *)argv[a]; size_t length = strlen(argv[a]), count;
        lp_id *ids = malloc(sizeof(lp_id) * (length ? length : 1)); table_init();
        Ref root = text_occurrences(c, bytes, length, ids, length, &count); char id[33]; id_text(&root.id,id);
        printf("{\"entity\":\"%s\",\"segments\":[",id);
        for (size_t i=0; i<count; i++) { id_text(ids+i,id); printf("%s\"%s\"",i ? "," : "",id); }
        puts("]}"); free(ids); lp_text_free(c); return 0;
    }
    lp_ref parts[64]; size_t np; lp_ref r = entity_named(c, argv[a], parts, 64, &np);
    double ms = (now() - t) * 1000;
    uint8_t fp[32]; lp_tier0_fingerprint(T0, fp);
    show_ref("entity", &r);
    printf("%-10s %s\n", "wall", lp_coord_inside(&r.c) ? "inside" : "OUTSIDE");
    printf("%-10s %zu\n", "parts", np);
    for (size_t i = 0; i < np && i < 64; i++) {
        char label[16]; snprintf(label, sizeof label, "  [%zu]", i); show_ref(label, &parts[i]);
    }
    printf("%-10s ", "tier 0"); for (int i = 0; i < 32; i++) printf("%02x", fp[i]); printf("\n");
    printf("computed in %.2f ms, mapping tier 0 included; no database\n", ms);
    return 0;
}

/* ---- the claims that hold an entity */

int claim_by_conf(const void *a, const void *b){ double x = ((const Claim *)a)->conf, y = ((const Claim *)b)->conf; return x < y ? 1 : x > y ? -1 : memcmp(a, b, 16); }

/* The predicates the pass's firmware refuses, as the blake3[] every claim read passes: a restriction is applied before
 * the sort (Sequence 15.5), so a refused strand never takes a place the fan leaves. The firmware names them when it is
 * loaded; their IDs are computed here, once, on the first read. */
static char refuse_names[FW_NAMES][96]; static int nrefuse_names;
static uint64_t refuse_generation=1;
typedef struct { uint64_t generation; lp_id ids[FW_NAMES]; uint8_t wire[20+20*FW_NAMES]; } RefusalCache;
static _Thread_local RefusalCache refusal;
void refuse_named(const Firmware *fw){
    nrefuse_names=fw->nrefuse_predicate;memcpy(refuse_names,fw->refuse_predicate,sizeof refuse_names);refuse_generation++;
}
static RefusalCache *refused_ids(void){
    if(refusal.generation!=refuse_generation){
        if(nrefuse_names){
            lp_text *context=lp_text_new(T0);
            for(int i=0;i<nrefuse_names;i++)refusal.ids[i]=entity_named(context,refuse_names[i],NULL,0,NULL).id;
            lp_text_free(context);
        }
        refusal.generation=refuse_generation;
    }
    return &refusal;
}
const char *refuse_param(PGconn *pg,int *len){
    RefusalCache *cache=refused_ids();
    *len=(int)ids_param(pg,cache->wire,cache->ids,(uint32_t)nrefuse_names);
    return (const char *)cache->wire;
}
void arg_refused(Args *a){arg_ids(a,refused_ids()->ids,(size_t)nrefuse_names);}
/* Whether a claim holds a refused predicate between its first part and its last: the check the reads make in SQL. */
int middle_refused(const lp_id *part, int np){
    const lp_id *id=refused_ids()->ids;
    for (int p = 1; p + 1 < np; p++) for (int i = 0; i < nrefuse_names; i++) if (!memcmp(&part[p], &id[i], 16)) return 1;
    return 0;
}

/* The claims that hold the given parts in their places (a part not given is open): at most fan of them; *capped says
 * there were more. k: how many deviations below its rating a claim is read at. */
Claim *claims_like(PGconn *pg, const lp_id *part, const int *have, int fan, double k, int *n, int *capped){
    lp_id keys[3]; uint32_t nk = 0; for (int i = 0; i < 3; i++) if (have[i]) keys[nk++] = part[i];
    /* the tier is known once the IDs are: every leaf above it, together, then the fan keeps the hardest pulling */
    int nh = 0; Hold *h = holds_above(pg,keys, (int)nk, tier_max(pg, keys, (int)nk), 0, 1, &nh);
    int count=0;Claim *c=claims_from_holds(h,nh,k,&count);int m=0;
    for(int j=0;j<count;j++){
        Claim *x=&c[j];
        /* in its place: the first part given is the claim's first, the last its last, and the middle one between them */
        int fits = 1, last = x->np - 1;
        if (have[0] > 1 && memcmp(&x->part[0], &part[0], 16)) fits = 0;
        if (have[2] > 1 && memcmp(&x->part[last], &part[2], 16)) fits = 0;
        if (have[1] > 1) { int in = 0; for (int i = 1; i < last || (i == 1 && x->np == 2 && i <= last); i++) if (!memcmp(&x->part[i], &part[1], 16)) in = 1; if (!in) fits = 0; }
        if (!fits) continue;
        c[m++]=*x;
    }
    holds_free(h, nh); *n = m;
    qsort(c, (size_t)m, sizeof(Claim), claim_by_conf);
    *capped = fan >= 0 && m > fan; if (*capped) *n = fan;
    return c;
}
/* The claims of each of a set of entities, one set (holds_pair: one probe a leaf for all of them): with rel, the claims
 * whose middle holds rel; position selects any part, the first, or the last. Each comes back with its entity's place in
 * keys (src), ordered by place, then by how hard it tugs; at most fan a place. */
typedef struct { Claim c; int src; } SrcClaim;
static int by_src_conf(const void *a, const void *b){ const SrcClaim *x = a, *y = b; return x->src != y->src ? (x->src < y->src ? -1 : 1) : claim_by_conf(&x->c, &y->c); }
Claim *claims_each(PGconn *pg, const lp_id *keys, int nk, const lp_id *rel, ClaimPosition position, int fan, double k, int *n, int **src){
    (void)pg; *n = 0; *src = NULL; if (!nk) return NULL;
    lp_idmap *km = lp_idmap_new(); int *head = malloc(sizeof(int) * (size_t)nk), *link = malloc(sizeof(int) * (size_t)nk);
    for (int i = 0; i < nk; i++) head[i] = -1;
    for (int i = nk - 1; i >= 0; i--) { bool fresh; size_t u = lp_idmap_put(km, &keys[i], &fresh); link[i] = head[u]; head[u] = i; }
    int nh=0,nc=0;Hold *h=holds_pair(pg,keys,nk,rel,1,&nh);Claim *decoded=claims_from_holds(h,nh,k,&nc);
    holds_free(h,nh);
    SrcClaim *s=malloc(sizeof(SrcClaim)*(size_t)(nc?nc:1));int m=0,cap=nc?nc:1;
    uint64_t *seen=calloc(lp_idmap_count(km),sizeof *seen);
    for(int j=0;j<nc;j++){
        Claim x=decoded[j];int last=x.np-1;
        if(rel){int in=0;for(int i=1;i<last || (i==1 && x.np==2 && i<=last);i++)in|=lp_id_eq(&x.part[i],rel);if(!in)continue;}
        int begin=position==CLAIM_LAST?last:0,end=position==CLAIM_ANY?x.np:begin+1;
        for(int p=begin;p<end;p++){
            int64_t u=lp_idmap_find(km,&x.part[p]);if(u<0 || seen[u]==(uint64_t)j+1)continue;
            seen[u]=(uint64_t)j+1;
            for(int i=head[u];i>=0;i=link[i]){
                if(m==cap){cap*=2;s=xrealloc(s,sizeof(SrcClaim)*(size_t)cap);}s[m++]=(SrcClaim){x,i};
            }
        }
    }
    free(seen);lp_idmap_free(km);free(head);free(link);
    qsort(s, (size_t)m, sizeof(SrcClaim), by_src_conf);
    size_t operands=0;int kept=0,per=0;
    for(int j=0;j<m;j++){
        per=j && s[j].src==s[j-1].src?per+1:0;if(fan>=0 && per>=fan)continue;
        if((size_t)s[j].c.np>SIZE_MAX-operands){fputs("claim result exceeds addressable memory\n",stderr);exit(1);}
        operands+=(size_t)s[j].c.np;kept++;
    }
    Claim *c=claim_block((size_t)kept,operands);lp_id *parts=(lp_id *)(c+(kept?kept:1));
    int *sr=malloc(sizeof(int)*(size_t)(kept?kept:1)),w=0;per=0;
    for(int j=0;j<m;j++){
        per=j && s[j].src==s[j-1].src?per+1:0;if(fan>=0 && per>=fan)continue;
        c[w]=s[j].c;memcpy(parts,c[w].part,sizeof(lp_id)*(size_t)c[w].np);c[w].part=parts;parts+=c[w].np;sr[w++]=s[j].src;
    }
    free(s);free(decoded);*n=w;*src=sr;return c;
}
/* Every claim that holds an entity, wherever in it. */
Claim *claims_of(PGconn *pg, const lp_id *e, int fan, double k, int *n, int *capped){
    lp_id part[3] = { *e, *e, *e }; int have[3] = { 1, 0, 0 };
    return claims_like(pg, part, have, fan, k, n, capped);
}
/* The position each claim was given by the records that gave one: the least, as each record's vertex for it carries it
 * (provenance by containment: a walk up from the claims to the records and trunks that hold them). */
void positions_of(PGconn *pg, Claim *c, int n){
    if (!n) return;
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)n); lp_idmap *at = lp_idmap_sized(sizeof(int));
    for (int i = 0; i < n; i++) { ids[i] = c[i].id; bool f; int *x = lp_idmap_get(at, &c[i].id, &f); if (f) *x = i; }
    int na = 0; Attested *a = attested(pg, ids, n, &na, NULL);
    for (int j = 0; j < na; j++) { if (!a[j].position) continue; const int *x = lp_idmap_lookup(at, &a[j].claim); if (!x) continue;
        int i = *x, pos = (int)a[j].position; if (!c[i].position || pos < c[i].position) c[i].position = pos; }
    free(a); lp_idmap_free(at); uint8_t *ab = NULL;
    /* A claim said within a record has its place in the record's own trajectory (Physicality: the trajectory records
     * the order of the constituents, so no ordinal is needed): the entry lists its senses in the order its witness
     * gave them. For the claims no record gave a place, the paths that hold each are fetched as one set, and the
     * claim's place is where it stands in the path, the least when several records hold it. */
    int need = 0; for (int i = 0; i < n; i++) need += !c[i].position;
    if (need) {                                                              /* the paths that hold any of them, one set (holds_above gives each path to the claims it holds) */
        lp_id *want = malloc(sizeof(lp_id) * (size_t)need); int *of = malloc(sizeof(int) * (size_t)need), nw = 0;
        for (int i = 0; i < n; i++) if (!c[i].position) { want[nw] = c[i].id; of[nw++] = i; }
        int nh = 0; Hold *h = holds_above(pg,want, nw, -1, 1, 0, &nh);
        for (int j = 0; j < nh; j++) {
            if (h[j].src < 0 || h[j].src >= nw || h[j].claim) continue; int i = of[h[j].src];
            size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0); if (!nv) continue;
            lp_vertex *vt = malloc(sizeof(lp_vertex) * nv); lp_path_vertices(h[j].path, (size_t)h[j].path_len, vt, nv); int at = 0, place = 0;
            if (memcmp(&vt[0].id, &c[i].part[0], 16)) { free(vt); continue; }   /* the trajectory under the claim's own subject: the order its witness gave */
            for (size_t z = 0; z < nv && !place; z++) { if (!memcmp(&vt[z].id, &c[i].id, 16)) place = at + 1; at += (int)vt[z].run; }
            free(vt);
            if (place && (!c[i].position || place < c[i].position)) c[i].position = place;
        }
        holds_free(h, nh); free(want); free(of);
    }
    free(ab); free(ids);
}
/* As given first, then by how hard the strand tugs back. */
int claim_by_position(const void *a, const void *b){
    const Claim *x = a, *y = b; int px = x->position ? x->position : 1 << 30, py = y->position ? y->position : 1 << 30;
    return px != py ? (px < py ? -1 : 1) : claim_by_conf(a, b);
}

/* The kinds of strand a firmware refuses, taken out of a set before it is used: a claim that holds a refused
 * predicate in its middle, and a claim only refused witnesses hold (a walk up to the trunks over it). Returns how many are left. */
int refused(PGconn *pg, Ctx *c, const Firmware *fw, Claim *cl, int n){
    if (!n || (!fw->nrefuse_predicate && !fw->nrefuse_witness)) return n;
    lp_id pred[FW_NAMES], wit[FW_NAMES];
    for (int i = 0; i < fw->nrefuse_predicate; i++) pred[i] = entity_named(c, fw->refuse_predicate[i], NULL, 0, NULL).id;
    for (int i = 0; i < fw->nrefuse_witness; i++) wit[i] = entity_named(c, fw->refuse_witness[i], NULL, 0, NULL).id;
    uint8_t *out = calloc((size_t)n, 1);
    for (int i = 0; i < n; i++) for (int p = 1; p < cl[i].np - 1 || (p == 1 && cl[i].np == 2); p++) { if (p >= cl[i].np) break;
        for (int z = 0; z < fw->nrefuse_predicate; z++) if (!memcmp(&cl[i].part[p], &pred[z], 16)) out[i] = 1; }
    if (fw->nrefuse_witness) {                                               /* a witness by its name: the record of its source's trunk, [record, files] */
        lp_id *ids = malloc(sizeof(lp_id) * (size_t)n); lp_idmap *at = lp_idmap_sized(sizeof(int));
        for (int i = 0; i < n; i++) { ids[i] = cl[i].id; bool f; int *x = lp_idmap_get(at, &cl[i].id, &f); if (f) *x = i; }
        int na = 0; Attested *a = attested(pg, ids, n, &na, NULL); Reader *rd = reader_new(pg); lp_idmap *named = lp_idmap_sized(1);
        uint8_t *other = calloc((size_t)n, 1), *theirs = calloc((size_t)n, 1);
        for (int j = 0; j < na; j++) { if (!a[j].witness) continue; const int *x = lp_idmap_lookup(at, &a[j].claim); if (!x) continue;
            bool f; uint8_t *is = lp_idmap_get(named, &a[j].trunk, &f);
            if (f) { lp_id first[2]; size_t m = reader_parts(rd, &a[j].trunk, first, 2); *is = 1; for (int z = 0; m && z < fw->nrefuse_witness; z++) if (!memcmp(&first[0], &wit[z], 16)) *is = 2; }
            if (*is == 2) theirs[*x] = 1; else other[*x] = 1; }
        for (int i = 0; i < n; i++) if (theirs[i] && !other[i]) out[i] = 1;
        free(a); free(ids); free(other); free(theirs); lp_idmap_free(at); lp_idmap_free(named); reader_free(rd);
    }
    int m = 0; for (int i = 0; i < n; i++) if (!out[i]) cl[m++] = cl[i];
    free(out); return m;
}
/* Role weights (Semantics: Personality firmware, "Weights that are not standings"): a weight on which kind of strand is
 * allowed to pull. It is the firmware's, never a column of the claim: the standing is read as it is, and the reading is
 * multiplied by the weight the firmware gives the claim's kind (what stands between its first part and its last). */
void weights_named(Ctx *c, const Firmware *fw, lp_id *ids){ for (int i = 0; i < fw->nweigh; i++) ids[i] = entity_named(c, fw->weigh_name[i], NULL, 0, NULL).id; }
double strand_weight(const Firmware *fw, const lp_id *ids, const lp_id *part, int np){
    double w = 1.0;
    for (int p = 1; p < np - 1; p++) for (int z = 0; z < fw->nweigh; z++) if (!memcmp(&part[p], &ids[z], 16) && fw->weigh[z] < w) w = fw->weigh[z];
    return w;
}
void weighed(Ctx *c, const Firmware *fw, Claim *cl, int n){
    if (!n || !fw->nweigh) return;
    lp_id ids[FW_WEIGHS]; weights_named(c, fw, ids);
    for (int i = 0; i < n; i++) cl[i].conf *= strand_weight(fw, ids, cl[i].part, cl[i].np);
    qsort(cl, (size_t)n, sizeof(Claim), claim_by_conf);
}
int cmd_hop(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; int limit = 24, fan = -1, a = 1; double k = -1;
    for (; a < argc - 1 && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--firmware") && a + 1 < argc) fwp = argv[++a];
        else if (!strcmp(argv[a], "--fan") && a + 1 < argc) fan = atoi(argv[++a]);      /* to measure: over the firmware's */
        else if (!strcmp(argv[a], "--k") && a + 1 < argc) k = atof(argv[++a]);
    }
    Firmware fw = firmware_for(fwp, FW_HOP); if (fan < 0) fan = fw.fan; if (k < 0) k = fw.k;
    if (a >= argc || (argc - a != 1 && argc - a != 3)) {
        fprintf(stderr, "usage: laplace hop [-d conninfo] [-n N] [--firmware FILE] text\n"
                        "       laplace hop [...] first middle last             with ? for a part left open: laplace hop was UPOS ?\n"); return 2; }
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0); PGconn *pg = db_read(conninfo);
    int n, capped, whole = argc - a == 1; Claim *cl; lp_ref e; double t;
    if (whole) { e = entity_named(c, argv[a], NULL, 0, NULL); show_ref("entity", &e); t = now(); cl = claims_of(pg, &e.id, fan, k, &n, &capped); }
    else {
        lp_id part[3]; int have[3];
        for (int i = 0; i < 3; i++) { have[i] = strcmp(argv[a + i], "?") ? 2 : 0; if (have[i]) { lp_ref r = entity_named(c, argv[a + i], NULL, 0, NULL); part[i] = r.id; show_ref(i == 0 ? "subject" : i == 1 ? "predicate" : "object", &r); } }
        if (!have[0] && !have[1] && !have[2]) { fprintf(stderr, "every part is open\n"); return 2; }
        t = now(); cl = claims_like(pg, part, have, fan, k, &n, &capped);
    }
    double t_claims = (now() - t) * 1000;
    n = refused(pg, c, &fw, cl, n); weighed(c, &fw, cl, n);
    if (!whole && fw.order_witness) { positions_of(pg, cl, n); qsort(cl, (size_t)n, sizeof(Claim), claim_by_position); }
    else positions_of(pg, cl, n < limit ? n : limit);
    Reader *rd = reader_new(pg);
    lp_vec(lp_id) display={0};
    for (int i = 0; i < n && i < limit; i++) for (int p = 0; p < cl[i].np; p++) lp_push(&display,cl[i].part[p]);
    reader_prefetch(rd,display.v,display.n,72);lp_vec_free(&display);
    firmware_say(&fw, FW_HOP);
    printf("\nattested: %d claim%s%s\n", n, n == 1 ? "" : "s", capped ? " (more exist than the fan reads)" : "");
    if (n) printf("%10s %8s %6s %8s %6s   %s\n", "confidence", "rating", "dev", "matches", "given", "claim");
    for (int i = 0; i < n && i < limit; i++) {
        char pos[16] = ""; if (cl[i].position) snprintf(pos, sizeof pos, "%d", cl[i].position);
        printf("%10.3f %8.0f %6.0f %8" PRId64 " %6s   [", cl[i].conf, cl[i].r.rating, cl[i].r.deviation, cl[i].matches, pos);
        for (int p = 0; p < cl[i].np; p++) { char *tx = reader_text(rd, &cl[i].part[p], p == cl[i].np - 1 ? 72 : 48); printf("%s%s", p ? ", " : "", tx); free(tx); }
        printf("]\n");
    }
    if (!whole) { printf("\nclaims %.1f ms   %llu round trips for text   total %.1f ms\n", t_claims, (unsigned long long)reader_trips(rd), (now() - T) * 1000);
                  free(cl); reader_free(rd); PQfinish(pg); return 0; }

    /* observed: the content that holds it, which is not claims */
    t = now(); uint8_t ab[40]; size_t al = ids_param(pg,ab, &e.id, 1);
    const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = db_ask(pg, "SELECT tier FROM @extschema@.laplace_containers($1::@extschema@.blake3[], '{}'::smallint[]) WHERE NOT (mask OPERATOR(@extschema@.?) 0::smallint)", 1, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "containers: %s", PQerrorMessage(pg)); return 1; }
    uint64_t by_tier[256] = { 0 }; int any = 0;
    for (int j = 0; j < PQntuples(q); j++) { uint16_t tb; memcpy(&tb, PQgetvalue(q, j, 0), 2); by_tier[ntohs(tb) & 255]++; any = 1; }
    PQclear(q);
    printf("\nobserved: held by"); if (!any) printf(" nothing recorded");
    for (int tr = 0, first = 1; tr < 256; tr++) if (by_tier[tr]) { printf("%s %llu path%s of tier %d", first ? "" : ",", (unsigned long long)by_tier[tr], by_tier[tr] == 1 ? "" : "s", tr); first = 0; }
    printf("\n\nclaims %.1f ms   containers %.1f ms   %llu round trips for text   total %.1f ms\n", t_claims, (now() - t) * 1000,
           (unsigned long long)reader_trips(rd), (now() - T) * 1000);
    free(cl); reader_free(rd); PQfinish(pg);
    return 0;
}

/* ---- translation through the concepts: a word goes up to its concepts and down into other languages through its
 * lexicalizations, read as the recipe that reads the wordnets declares them (for translate: lexicalization, gloss), never
 * along a resource's names written here (Laplace-Engine#32):
 *   lexicalization RECIPE THING LANGUAGE CONCEPT
 *                          the relation RECIPE declares of THING (relation "TEXT" THING), [word, TEXT, lexicalization],
 *                          and the places of LANGUAGE and CONCEPT among the parts RECIPE names a THING by (thing THING
 *                          ...): the lexicalization [lemma, language, ILI] (recipes/BRIEF.md). Up: the word's
 *                          lexicalizations in the language asked, and their concepts; down: what holds each concept in
 *                          that place with a language asked in its place, and the word it is a lexicalization of
 *   gloss RECIPE NAME      what is shown of a concept: what holds it as a pair [x, concept] (a value said of x), and
 *                          [x, TEXT, text], TEXT the relation RECIPE declares of NAME
 *   laplace translate [-d conninfo] [-n N] [--firmware FILE] word from to...        laplace translate dog en de fr ja */
/* Every step is one set: the whole frontier is asked at once, its claims read from every leaf together (holds_above,
 * one statement a leaf), never one entity at a time. A step no claim carries is the structure's (Recipes: a thing named
 * by several parts together is the path of them, so a sense named by its synset and its entry holds its synset as a
 * constituent, recorded once and never said again as a claim): up, the constituents of x; down, what holds x. */
typedef struct { int from; lp_id to; lp_rating r; } Step;          /* from: the frontier entry it leaves; to: where it goes */
static double read_k = 2.0;                                                  /* the firmware's k, for the lookups below */
/* Read the complete tuple through the shared claim reader, then follow its
 * declared endpoint. dir 0: [key, ..., R, ..., x]; dir 1: [x, ..., R, ..., key]. */
static Step *open_each(PGconn *pg,const lp_id *keys,int nk,const lp_id *rel,int dir,int fan,int *ns){
    int n=0,*src=NULL;
    Claim *claims=claims_each(pg,keys,nk,rel,dir?CLAIM_LAST:CLAIM_FIRST,-1,read_k,&n,&src);
    Step *out=malloc(sizeof *out*(size_t)(n?n:1));int used=0;
    int *per=calloc((size_t)(nk?nk:1),sizeof *per);
    for(int j=0;j<n;j++){
        Claim *claim=claims+j;int key=src[j];
        if(claim->np<3 || (fan>=0 && per[key]>=fan))continue;
        out[used++]=(Step){key,claim->part[dir?0:claim->np-1],claim->r};per[key]++;
    }
    free(per);free(src);free(claims);*ns=used;return out;
}
/* The constituents of each entity, one set: out[i] for keys[i]. */
static Run *parts_each(PGconn *pg, const lp_id *keys, int nk){
    Run *out = calloc((size_t)(nk ? nk : 1), sizeof(Run)); if (!nk) return out;
    uint8_t *ab = malloc(20 + 20 * (size_t)nk); size_t al = ids_param(pg,ab, keys, (uint32_t)nk); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = db_ask(pg, "SELECT u.i, p.path FROM unnest($1::@extschema@.blake3[]) WITH ORDINALITY AS u(id, i) JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) u.id", 1, v, l, f);
    if (PQresultStatus(q) == PGRES_TUPLES_OK) for (int j = 0; j < PQntuples(q); j++) { uint64_t o; memcpy(&o, PQgetvalue(q, j, 0), 8); int i = (int)__builtin_bswap64(o) - 1;
        if (i >= 0 && i < nk && !out[i].id) out[i] = run_of((const uint8_t *)PQgetvalue(q, j, 1), (size_t)PQgetlength(q, j, 1)); }
    PQclear(q); free(ab); return out;
}
static Claim *one_open_claims(PGconn *pg, const lp_id *first, const lp_id *rel, int fan, int *n){
    lp_id p[3] = { *first, *rel, *first }; int h[3] = { 2, 2, 0 }, capped; return claims_like(pg, p, h, fan, read_k, n, &capped);
}
/* A word's lexicalizations as the recipe that reads the wordnets declares them: the relation that says a word has one,
 * and which of its parts are its language and its concept. Read from that recipe, never from a second copy of its
 * names, so a recipe that renames them fails here, at once, rather than leaving every translation empty. */
int lexicalization_of(Ctx *c, const Firmware *fw, Lexicalization *x){
    memset(x, 0, sizeof *x);
    if (!fw->lexical[0][0]) { fprintf(stderr, "%s: for translate, the firmware names no lexicalization (lexicalization RECIPE THING LANGUAGE CONCEPT)\n", fw->path); return 0; }
    Recipe *R; int nr = recipes_load(laplace_recipes(), &R); const Recipe *w = NULL, *g = NULL, *e = NULL, *u = NULL;
    for (int i = 0; i < nr; i++) if (!R[i].broken) { if (!strcmp(R[i].name, fw->lexical[0])) w = &R[i]; if (fw->gloss[0][0] && !strcmp(R[i].name, fw->gloss[0])) g = &R[i];
        if (fw->nequiv && !strcmp(R[i].name, fw->equiv[0])) e = &R[i]; if (fw->usages[0][0] && !strcmp(R[i].name, fw->usages[0])) u = &R[i]; }
    /* how often a sense is observed: the games the recipe that reads a usage count gives the sense it attests (cntlist's
     * tag_cnt), read from it so it cannot drift; a recipe that renames it fails here */
    if (fw->usages[0][0]) { int ur = u && say_games(u, fw->usages[1]);
        if (!ur) { fprintf(stderr, "%s: for translate, usages: the recipe %s %s\n", fw->path, fw->usages[0], u ? "declares no games of that name" : "is not a recipe that loads"); return 0; }
        x->has_usage = 1; }
    /* the codes of one language: the relations the recipe that reads a code table declares between a code and its
     * equivalents in the other code sets (ISO 639-3's Part1, Part2b, Part2t), read from it so they cannot drift */
    if (fw->nequiv && !e) { fprintf(stderr, "%s: for translate, equivalent names the recipe %s, which is not a recipe of %s that loads\n", fw->path, fw->equiv[0], laplace_recipes()); return 0; }
    for (int k = 0; k < fw->nequiv && k < 4; k++) { const char *er = say_relation(e, fw->equiv[k + 1]);
        if (!er) { fprintf(stderr, "%s: for translate, the recipe %s declares no relation of %s (relation \"TEXT\" %s)\n", fw->path, fw->equiv[0], fw->equiv[k + 1], fw->equiv[k + 1]); return 0; }
        x->eq[x->neq++] = entity_named(c, er, NULL, 0, NULL).id; }
    if (!w) { fprintf(stderr, "%s: for translate, lexicalization names the recipe %s, which is not a recipe of %s that loads\n", fw->path, fw->lexical[0], laplace_recipes()); return 0; }
    const char *rel = say_relation(w, fw->lexical[1]);
    if (!rel) { fprintf(stderr, "%s: for translate, the recipe %s declares no relation of %s (relation \"TEXT\" %s)\n", fw->path, fw->lexical[0], fw->lexical[1], fw->lexical[1]); return 0; }
    x->np = say_thing_parts(w, fw->lexical[1], fw->lexical[2], fw->lexical[3], &x->lang, &x->concept);
    if (x->np < 0) { fprintf(stderr, "%s: for translate, the recipe %s names no %s by parts that are its %s and its %s (thing %s ...)\n", fw->path, fw->lexical[0], fw->lexical[1], fw->lexical[2], fw->lexical[3], fw->lexical[1]); return 0; }
    x->sense = entity_named(c, rel, NULL, 0, NULL).id;
    if (fw->gloss[0][0]) { const char *gr = g ? say_relation(g, fw->gloss[1]) : NULL;
        if (!gr) { fprintf(stderr, "%s: for translate, gloss: the recipe %s %s\n", fw->path, fw->gloss[0], g ? "declares no relation of that name" : "is not a recipe that loads"); return 0; }
        x->gloss = entity_named(c, gr, NULL, 0, NULL).id; x->has_gloss = 1; }
    return 1;
}
/* A language as every code set writes it: the code asked, and each code a code table states is its equivalent, either
 * way ([eng, Equivalent 639-1 identifier, en] joins en and eng), and theirs in turn: one language's codes. */
typedef struct { lp_id id[16]; int n; } Codes;
static int codes_has(const Codes *s, const lp_id *x){ for (int i = 0; i < s->n; i++) if (!memcmp(&s->id[i], x, 16)) return 1; return 0; }
static void codes_of(PGconn *pg, const Lexicalization *lx, const lp_id *code, Codes *s, int *steps){
    s->n = 0; s->id[s->n++] = *code;
    for (int from = 0, round = 0; round < 2 && from < s->n; round++) { int to = s->n;
        for (int i = from; i < to; i++) for (int k = 0; k < lx->neq; k++) for (int side = 0; side < 2; side++) {
            lp_id p[3] = { s->id[i], lx->eq[k], s->id[i] }; int have[3] = { side ? 0 : 2, 2, side ? 2 : 0 }, n, capped;
            Claim *cl = claims_like(pg, p, have, 64, read_k, &n, &capped); (*steps)++;
            for (int j = 0; j < n; j++) { const lp_id *o = side ? &cl[j].part[0] : &cl[j].part[cl[j].np - 1]; if (s->n < 16 && !codes_has(s, o)) s->id[s->n++] = *o; }
            free(cl); }
        from = to; }
}
typedef struct { lp_id *id; int *of, n, cap; } Found;                      /* entities found, each with what it is found for */
int lexicalization_counts(PGconn *pg, Reader *rd, const Lexicalization *lx, const lp_id *ids, int n, int fan, double k, long long *uses){
    (void)rd; (void)fan; (void)k;
    if (!lx->has_usage || !n) return 0;
    /* the games of each lexicalization: the times a source observed it (a count the source states is the games of the sense
     * it attests, never a claim's value), one set */
    uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = ids_param(pg, ab, ids, (uint32_t)n); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = db_ask(pg, "SELECT u.i, sum(a.games)::bigint FROM unnest($1::@extschema@.blake3[]) WITH ORDINALITY AS u(id, i) JOIN @extschema@.attestation a ON a.claim OPERATOR(@extschema@.=) u.id GROUP BY u.i", 1, v, l, f);
    if (PQresultStatus(q) == PGRES_TUPLES_OK) for (int j = 0; j < PQntuples(q); j++) { uint64_t o, g; memcpy(&o, PQgetvalue(q, j, 0), 8); memcpy(&g, PQgetvalue(q, j, 1), 8);
        int entry = (int)__builtin_bswap64(o) - 1; long long value = (long long)__builtin_bswap64(g); if (entry >= 0 && entry < n && value > uses[entry]) uses[entry] = value; }
    PQclear(q); free(ab); return 1;
}
static void found_put(Found *f, const lp_id *id, int of){
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 64; f->id = xrealloc(f->id, sizeof(lp_id) * (size_t)f->cap); f->of = xrealloc(f->of, sizeof(int) * (size_t)f->cap); }
    f->id[f->n] = *id; f->of[f->n++] = of; }
int cmd_translate(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; int limit = 4, a = 1;
    for (; a < argc - 1 && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--firmware") && a + 1 < argc) fwp = argv[++a];
    }
    Firmware fw = firmware_for(fwp, FW_TRANSLATE); int fan = fw.fan; read_k = fw.k;
    if (argc - a < 3) { fprintf(stderr, "usage: laplace translate [-d conninfo] [-n concepts] word from to...\n       languages as the resources write them: en de fr ja\n"); return 2; }
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0);
    Lexicalization lx; if (!lexicalization_of(c, &fw, &lx)) return 2;
    PGconn *pg = db_read(conninfo); Reader *rd = reader_new(pg);
    const lp_id word = entity_named(c, argv[a], NULL, 0, NULL).id, from = entity_named(c, argv[a + 1], NULL, 0, NULL).id;
    int nto = argc - a - 2 < 64 ? argc - a - 2 : 64, steps = 0; lp_id to[64];
    for (int g = 0; g < nto; g++) to[g] = entity_named(c, argv[a + 2 + g], NULL, 0, NULL).id;
    Codes fromc, *toc = malloc(sizeof(Codes) * (size_t)nto); codes_of(pg, &lx, &from, &fromc, &steps); for (int g = 0; g < nto; g++) codes_of(pg, &lx, &to[g], &toc[g], &steps);

    /* up: the word's lexicalizations, in its witness's order, and of each its language and its concept */
    int n; Claim *first = one_open_claims(pg, &word, &lx.sense, fan, &n); steps++; positions_of(pg, first, n); qsort(first, (size_t)n, sizeof(Claim), claim_by_position);
    lp_id *ls = malloc(sizeof(lp_id) * (size_t)(n ? n : 1)); for (int i = 0; i < n; i++) ls[i] = first[i].part[first[i].np - 1];
    /* by observed usages, most first; then as above, the witness's order, then standing (Laplace-Wiki Sequence/Web.md
     * 15.6): the number the usage relation states of each lexicalization, one set */
    long long *uses = calloc((size_t)(n ? n : 1), sizeof(long long));
    if (lx.has_usage && n) { steps += lexicalization_counts(pg, rd, &lx, ls, n, fan, read_k, uses);
        int *ord = malloc(sizeof(int) * (size_t)n); for (int i = 0; i < n; i++) ord[i] = i;
        for (int i = 1; i < n; i++) { int v = ord[i], j = i; while (j > 0 && uses[ord[j - 1]] < uses[v]) { ord[j] = ord[j - 1]; j--; } ord[j] = v; }      /* stable: ties keep the order above */
        Claim *f2 = malloc(sizeof(Claim) * (size_t)n); lp_id *l2 = malloc(sizeof(lp_id) * (size_t)n); long long *u2 = malloc(sizeof(long long) * (size_t)n);
        for (int i = 0; i < n; i++) { f2[i] = first[ord[i]]; l2[i] = ls[ord[i]]; u2[i] = uses[ord[i]]; }
        memcpy(first,f2,sizeof(Claim)*(size_t)n);free(f2);free(ls);free(uses);free(ord);ls=l2;uses=u2; }
    Run *pr = parts_each(pg, ls, n); steps += n > 0;
    lp_id *cs = malloc(sizeof(lp_id) * (size_t)(n ? n : 1)); lp_rating *cr = malloc(sizeof(lp_rating) * (size_t)(n ? n : 1)); long long *cu = calloc((size_t)(n ? n : 1), sizeof(long long)); int nc = 0;
    for (int i = 0; i < n; i++) {
        if (nc < limit && pr[i].n == lx.np && codes_has(&fromc, &pr[i].id[lx.lang])) { const lp_id *k = &pr[i].id[lx.concept]; int dup = 0;
            for (int z = 0; z < nc; z++) dup |= !memcmp(&cs[z], k, 16); if (!dup) { cs[nc] = *k; cu[nc] = uses[i]; cr[nc++] = first[i].r; } }
        free(pr[i].id); }
    free(pr); free(ls);
    if (!nc) printf("%s: no lexicalization of it is attested in a language written %s\n", argv[a], argv[a + 1]);

    /* down: what holds each concept, one set: its lexicalizations in the languages asked, and what has it as its value
     * (a synset, whose definition is shown); then each lexicalization back to its word, along the same relation */
    int nh = 0; Hold *h = nc ? holds_any(pg,cs, nc, &nh) : NULL; steps += nc > 0;
    lp_idmap *cm = lp_idmap_new(); for (int z = 0; z < nc; z++) { bool fresh; lp_idmap_put(cm, &cs[z], &fresh); }
    Found lex = { 0 }, val = { 0 }; Ids parts={0};
    for (int j = 0; j < nh; j++) { size_t np=path_into(&parts,lp_path_of(h[j].path,(size_t)h[j].path_len));lp_id *p=parts.v;
        if ((int)np == lx.np) { int64_t z = lp_idmap_find(cm, &p[lx.concept]); if (z < 0) continue;
            for (int g = 0; g < nto; g++) if (codes_has(&toc[g], &p[lx.lang])) { found_put(&lex, &h[j].entity, (int)z * 64 + g); break; } }
        else if (np == 2 && lx.has_gloss) { int64_t z = lp_idmap_find(cm, &p[1]); if (z >= 0) found_put(&val, &p[0], (int)z); } }
    lp_vec_free(&parts); holds_free(h, nh); lp_idmap_free(cm);
    int nd = 0; Step *dn = open_each(pg,lex.id,lex.n,&lx.sense,1,fan,&nd); steps += lex.n > 0;
    int ng = 0; Step *gl = val.n ? open_each(pg,val.id,val.n,&lx.gloss,0,1,&ng) : NULL; steps += val.n > 0;
    for (int j = 0; j < nd; j++) reader_want(rd, &dn[j].to); for (int z = 0; z < nc; z++) reader_want(rd, &cs[z]); for (int j = 0; j < ng; j++) reader_want(rd, &gl[j].to);
    for (int z = 0; z < nc; z++) {
        char *it = reader_text(rd, &cs[z], 60), *dt = NULL;
        /* a synset is the words it lists (recipes/wn-lmf.recipe), so one synset can hold several of the concepts shown
         * (game's contest, single play and pastime are each [game]); its definitions then single none of them out, and
         * none is shown as that concept's */
        for (int j = 0; j < ng && !dt; j++) if (val.of[gl[j].from] == z) { int k = gl[j].from, shared = 0;
            for (int m = 0; m < val.n && !shared; m++) shared = val.of[m] != z && !memcmp(&val.id[m], &val.id[k], 16);
            if (!shared) dt = reader_text(rd, &gl[j].to, 100); }
        printf("\n  %4.0f \xC2\xB1 %-3.0f  %s", cr[z].rating, cr[z].deviation, it); if (cu[z] > 0) printf("   observed %lld times", cu[z]);
        printf("\n            %s\n", dt ? dt : ""); free(it); free(dt);
        for (int g = 0; g < nto; g++) {
            printf("    %-6s", argv[a + 2 + g]); int any = 0; lp_idmap *shown = lp_idmap_new();
            for (int j = 0; j < nd && any < 8; j++) { if (lex.of[dn[j].from] != z * 64 + g) continue; bool fresh; lp_idmap_put(shown, &dn[j].to, &fresh); if (!fresh) continue;
                char *tx = reader_text(rd, &dn[j].to, 40); printf("%s%s", any ? ", " : " ", tx); free(tx); any++; }
            lp_idmap_free(shown); if (!any) printf(" (nothing attested)"); printf("\n"); }
    }
    printf("\n%d steps, each one set   total %.1f ms\n", steps, (now() - T) * 1000);
    free(toc); free(uses); free(cu); free(first); free(cs); free(cr); free(lex.id); free(lex.of); free(val.id); free(val.of); free(dn); free(gl); reader_free(rd); PQfinish(pg);
    return 0;
}
