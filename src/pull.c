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
#include "engine.h"
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- naming an entity: the text's trunk, computed on the client */
lp_ref entity_named(Ctx *c, const char *text, lp_ref *parts, size_t cap, size_t *np){
    lp_ref p[1]; size_t n;
    return lp_text_parts(c, (const uint8_t *)text, strlen(text), parts ? parts : p, parts ? cap : 1, np ? np : &n);
}
static void show_ref(const char *label, const lp_ref *r){
    char id[33]; id_text(&r->id, id);
    printf("%-10s %s   tier %d   (%.6f, %.6f, %.6f, %.6f)   hilbert %016llx   depth %.6f\n", label, id, r->tier,
           (double)r->c.m[0] / LP_FIXED_ONE, (double)r->c.m[1] / LP_FIXED_ONE, (double)r->c.m[2] / LP_FIXED_ONE, (double)r->c.m[3] / LP_FIXED_ONE,
           (unsigned long long)lp_hilbert4(&r->c),
           1.0 - sqrt(((double)r->c.m[0] * r->c.m[0] + (double)r->c.m[1] * r->c.m[1] + (double)r->c.m[2] * r->c.m[2] + (double)r->c.m[3] * r->c.m[3])) / LP_FIXED_ONE);
}

int cmd_text(int argc, char **argv){
    if (argc < 2) { fprintf(stderr, "usage: laplace text text\n"); return 2; }
    double t = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0);
    lp_ref parts[64]; size_t np; lp_ref r = entity_named(c, argv[1], parts, 64, &np);
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
/* Claim, MAXPARTS: engine.h */

int claim_by_conf(const void *a, const void *b){ double x = ((const Claim *)a)->conf, y = ((const Claim *)b)->conf; return x < y ? 1 : x > y ? -1 : memcmp(a, b, 16); }

/* The predicates the pass's firmware refuses, as the blake3[] every claim read passes: a restriction is applied before
 * the sort (Sequence 15.5), so a refused strand never takes a place the fan leaves. The firmware names them when it is
 * loaded; their IDs are computed here, once, on the first read. */
static char refuse_names[FW_NAMES][96]; static int nrefuse_names, refuse_len = -1; static uint8_t refuse_ab[20 + 20 * FW_NAMES];
void refuse_named(const Firmware *fw){ nrefuse_names = fw->nrefuse_predicate; memcpy(refuse_names, fw->refuse_predicate, sizeof refuse_names); refuse_len = -1; }
const char *refuse_param(int *len){
    if (refuse_len < 0) {
        if (!nrefuse_names) { uint32_t h[3] = { htonl(0), htonl(0), htonl(id_oid) }; memcpy(refuse_ab, h, 12); refuse_len = 12; }   /* '{}': nothing refused */
        else { lp_text *c = lp_text_new(T0); lp_id id[FW_NAMES];
               for (int i = 0; i < nrefuse_names; i++) id[i] = entity_named(c, refuse_names[i], NULL, 0, NULL).id;
               refuse_len = (int)ids_param(refuse_ab, id, (uint32_t)nrefuse_names); lp_text_free(c); }
    }
    *len = refuse_len; return (const char *)refuse_ab;
}

/* The claims that hold the given parts in their places (a part not given is open): at most fan of them; *capped says
 * there were more. k: how many deviations below its rating a claim is read at. */
Claim *claims_like(PGconn *pg, const lp_id *part, const int *have, int fan, double k, int *n, int *capped){
    (void)pg;
    lp_id keys[3]; uint32_t nk = 0; for (int i = 0; i < 3; i++) if (have[i]) keys[nk++] = part[i];
    /* the tier is known once the IDs are: every leaf above it, together, then the fan keeps the hardest pulling */
    int nh = 0; Hold *h = holds_above(keys, (int)nk, tier_max(keys, (int)nk), 0, 1, &nh);
    Claim *c = malloc(sizeof(Claim) * (size_t)(nh ? nh : 1)); int m = 0;
    for (int j = 0; j < nh; j++) {
        if (!h[j].claim || !h[j].stood) continue;
        Claim *x = &c[m]; memcpy(x->id.b, h[j].entity.b, 16); x->np = 0; x->position = 0;
        size_t np_ = lp_path_ids(h[j].path, (size_t)h[j].path_len, x->part, MAXPARTS); x->np = np_ < MAXPARTS ? (int)np_ : MAXPARTS;
        if (x->np < 2) continue;
        /* in its place: the first part given is the claim's first, the last its last, and the middle one between them */
        int fits = 1, last = x->np - 1;
        if (have[0] > 1 && memcmp(&x->part[0], &part[0], 16)) fits = 0;
        if (have[2] > 1 && memcmp(&x->part[last], &part[2], 16)) fits = 0;
        if (have[1] > 1) { int in = 0; for (int i = 1; i < last || (i == 1 && x->np == 2 && i <= last); i++) if (!memcmp(&x->part[i], &part[1], 16)) in = 1; if (!in) fits = 0; }
        if (!fits) continue;
        x->r = h[j].r; x->matches = h[j].matches; x->conf = lp_confidence(&x->r, k); m++;
    }
    holds_free(h, nh); *n = m;
    qsort(c, (size_t)m, sizeof(Claim), claim_by_conf);
    *capped = fan >= 0 && m > fan; if (*capped) *n = fan;
    return c;
}
/* Every claim that holds an entity, wherever in it. */
Claim *claims_of(PGconn *pg, const lp_id *e, int fan, double k, int *n, int *capped){
    lp_id part[3] = { *e, *e, *e }; int have[3] = { 1, 0, 0 };
    return claims_like(pg, part, have, fan, k, n, capped);
}
/* The position each claim was given by the witnesses that gave one: the least, as recorded in attestation. */
void positions_of(PGconn *pg, Claim *c, int n){
    if (!n) return;
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)n); for (int i = 0; i < n; i++) ids[i] = c[i].id;
    uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = ids_param(ab, ids, (uint32_t)n);
    const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = db_ask(pg, "SELECT claim, position FROM laplace_attested($1::blake3[]) WHERE position IS NOT NULL", 1, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "positions: %s", PQerrorMessage(pg)); exit(1); }
    for (int j = 0; j < PQntuples(q); j++) {
        uint32_t pb; memcpy(&pb, PQgetvalue(q, j, 1), 4); int pos = (int)ntohl(pb);
        for (int i = 0; i < n; i++) if (!memcmp(c[i].id.b, PQgetvalue(q, j, 0), 16)) { if (!c[i].position || pos < c[i].position) c[i].position = pos; break; }
    }
    PQclear(q);
    /* A claim said within a record has its place in the record's own trajectory (Physicality: the trajectory records
     * the order of the constituents, so no ordinal is needed): the entry lists its senses in the order its witness
     * gave them. For the claims attestation gave no place, the records that hold each are fetched as one set, and the
     * claim's place is where it stands in the path, the least when several records hold it. */
    int need = 0; for (int i = 0; i < n; i++) need += !c[i].position;
    if (need) {
        const char *v2[1] = { (const char *)ab }; int l2[1] = { (int)al }, f2[1] = { 1 };
        PGresult *r = db_ask(pg, "SELECT u.i, k.path FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i), LATERAL laplace_containers(ARRAY[u.id], '{1}'::smallint[]) k", 1, v2, l2, f2);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "records: %s", PQerrorMessage(pg)); exit(1); }
        for (int j = 0; j < PQntuples(r); j++) {
            uint64_t o; memcpy(&o, PQgetvalue(r, j, 0), 8); int i = (int)(__builtin_bswap64(o) - 1); if (i < 0 || i >= n || (c[i].position && c[i].position < (1 << 20))) { if (i < 0 || i >= n) continue; }
            const uint8_t *pb = (const uint8_t *)PQgetvalue(r, j, 1); size_t pl = (size_t)PQgetlength(r, j, 1), nv = lp_path_vertices(pb, pl, NULL, 0); int at = 0, place = 0;
            if (!nv) continue; lp_vertex *vt = malloc(sizeof(lp_vertex) * nv); lp_path_vertices(pb, pl, vt, nv);
            if (memcmp(&vt[0].id, &c[i].part[0], 16)) { free(vt); continue; }   /* the trajectory under the claim's own subject: the order its witness gave */
            for (size_t z = 0; z < nv && !place; z++) { if (!memcmp(&vt[z].id, &c[i].id, 16)) place = at + 1; at += (int)vt[z].run; }
            free(vt);
            if (place && (!c[i].position || place < c[i].position)) c[i].position = place;
        }
        PQclear(r);
    }
    free(ab); free(ids);
}
/* As given first, then by how hard the strand tugs back. */
int claim_by_position(const void *a, const void *b){
    const Claim *x = a, *y = b; int px = x->position ? x->position : 1 << 30, py = y->position ? y->position : 1 << 30;
    return px != py ? (px < py ? -1 : 1) : claim_by_conf(a, b);
}

/* The kinds of strand a firmware refuses, taken out of a set before it is used: a claim that holds a refused
 * predicate in its middle, and a claim only refused witnesses attested. Returns how many are left. */
int refused(PGconn *pg, Ctx *c, const Firmware *fw, Claim *cl, int n){
    if (!n || (!fw->nrefuse_predicate && !fw->nrefuse_witness)) return n;
    lp_id pred[FW_NAMES], wit[FW_NAMES];
    for (int i = 0; i < fw->nrefuse_predicate; i++) pred[i] = entity_named(c, fw->refuse_predicate[i], NULL, 0, NULL).id;
    for (int i = 0; i < fw->nrefuse_witness; i++) wit[i] = entity_named(c, fw->refuse_witness[i], NULL, 0, NULL).id;
    uint8_t *out = calloc((size_t)n, 1);
    for (int i = 0; i < n; i++) for (int p = 1; p < cl[i].np - 1 || (p == 1 && cl[i].np == 2); p++) { if (p >= cl[i].np) break;
        for (int z = 0; z < fw->nrefuse_predicate; z++) if (!memcmp(&cl[i].part[p], &pred[z], 16)) out[i] = 1; }
    if (fw->nrefuse_witness) {
        lp_id *ids = malloc(sizeof(lp_id) * (size_t)n); for (int i = 0; i < n; i++) ids[i] = cl[i].id;
        uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = ids_param(ab, ids, (uint32_t)n);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGresult *q = db_ask(pg, "SELECT claim, witness FROM laplace_attested($1::blake3[])", 1, v, l, f);
        if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "witnesses: %s", PQerrorMessage(pg)); exit(1); }
        uint8_t *other = calloc((size_t)n, 1), *theirs = calloc((size_t)n, 1);
        for (int j = 0; j < PQntuples(q); j++) { int is = 0; for (int z = 0; z < fw->nrefuse_witness; z++) is |= !memcmp(PQgetvalue(q, j, 1), wit[z].b, 16);
            for (int i = 0; i < n; i++) if (!memcmp(cl[i].id.b, PQgetvalue(q, j, 0), 16)) { if (is) theirs[i] = 1; else other[i] = 1; break; } }
        for (int i = 0; i < n; i++) if (theirs[i] && !other[i]) out[i] = 1;
        PQclear(q); free(ab); free(ids); free(other); free(theirs);
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
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0); PGconn *pg = db_connect(conninfo);
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
    positions_of(pg, cl, n); if (!whole && fw.order_witness) qsort(cl, (size_t)n, sizeof(Claim), claim_by_position);
    Reader *rd = reader_new(pg);
    for (int i = 0; i < n && i < limit; i++) for (int p = 0; p < cl[i].np; p++) reader_want(rd, &cl[i].part[p]);
    firmware_say(&fw, FW_HOP);
    printf("\nattested: %d claim%s%s\n", n, n == 1 ? "" : "s", capped ? " (more exist than the fan reads)" : "");
    if (n) printf("%10s %8s %6s %8s %6s   %s\n", "confidence", "rating", "dev", "matches", "given", "claim");
    for (int i = 0; i < n && i < limit; i++) {
        char pos[16] = ""; if (cl[i].position) snprintf(pos, sizeof pos, "%d", cl[i].position);
        printf("%10.3f %8.0f %6.0f %8d %6s   [", cl[i].conf, cl[i].r.rating, cl[i].r.deviation, cl[i].matches, pos);
        for (int p = 0; p < cl[i].np; p++) { char *tx = reader_text(rd, &cl[i].part[p], p == cl[i].np - 1 ? 72 : 48); printf("%s%s", p ? ", " : "", tx); free(tx); }
        printf("]\n");
    }
    if (!whole) { printf("\nclaims %.1f ms   %llu round trips for text   total %.1f ms\n", t_claims, (unsigned long long)reader_trips(rd), (now() - T) * 1000);
                  free(cl); reader_free(rd); PQfinish(pg); return 0; }

    /* observed: the content that holds it, which is not claims */
    t = now(); uint8_t ab[40]; size_t al = ids_param(ab, &e.id, 1);
    const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = db_ask(pg, "SELECT tier FROM laplace_containers($1::blake3[], '{}'::smallint[]) WHERE NOT (mask ? 0::smallint)", 1, v, l, f);
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

/* ---- translation through the concepts: a word bubbles up to its concepts and down into other languages, along the
 * relations the firmware names (for translate: up, language, gloss), never along a resource's names written here:
 *   up R1 R2 ... Rn        from a word, [word, R1, x1], [x1, R2, x2], ... [x(n-1), Rn, concept]; and back down from the
 *                          concept, in the other direction, to the words of another language
 *   language HELD SAYS     the language of x(n-1): what holds it under HELD, [h, HELD, x(n-1)], and what h says
 *                          under SAYS, [h, SAYS, code]
 *   gloss R                what is shown of a concept: [x(n-1), R, text]
 *   laplace translate [-d conninfo] [-n N] [--firmware FILE] word from to...        laplace translate dog en de fr ja */
/* Every step is one set: the whole frontier is asked at once, its claims read from every leaf together (holds_above,
 * one statement a leaf), never one entity at a time. A step no claim carries is the structure's (Recipes: a thing named
 * by several parts together is the path of them, so a sense named by its synset and its entry holds its synset as a
 * constituent, recorded once and never said again as a claim): up, the constituents of x; down, what holds x. */
typedef struct { int from; lp_id to; lp_rating r; } Step;          /* from: the frontier entry it leaves; to: where it goes */
static double read_k = 2.0;                                                  /* the firmware's k, for the lookups below */
/* dir 0: [key, R, x], x the far end; dir 1: [x, R, key]. claim 0: the paths that hold the key and are no claim. */
static Step *open_each(const lp_id *keys, int nk, const lp_id *rel, int dir, int claim, int fan, int *ns){
    int nh = 0; Hold *h = claim ? holds_pair(keys, nk, rel, 1, &nh) : holds_above(keys, nk, -1, 1, 0, &nh); Step *s = malloc(sizeof(Step) * (size_t)(nh ? nh : 1)); int m = 0;
    int *per = calloc((size_t)(nk ? nk : 1), sizeof(int));
    for (int j = 0; j < nh; j++) { if (h[j].src < 0 || h[j].src >= nk || per[h[j].src] >= fan) continue;
        if (!claim) { if (h[j].claim) continue; s[m++] = (Step){ h[j].src, h[j].entity, { 0, 0, 0 } }; per[h[j].src]++; continue; }
        if (!h[j].claim || !h[j].stood) continue;
        lp_id p[MAXPARTS]; size_t np = lp_path_ids(h[j].path, (size_t)h[j].path_len, p, MAXPARTS); if (np < 3 || np > MAXPARTS) continue;
        const lp_id *key = &keys[h[j].src]; size_t last = np - 1, end = dir ? last : 0;
        if (memcmp(&p[end], key, 16)) continue;
        int in = 0; for (size_t i = 1; i < last; i++) in |= !memcmp(&p[i], rel, 16); if (!in) continue;
        s[m++] = (Step){ h[j].src, p[dir ? 0 : last], h[j].r }; per[h[j].src]++; }
    holds_free(h, nh); free(per); *ns = m; return s;
}
/* The constituents of each entity, one set: out[i] for keys[i]. */
static Run *parts_each(PGconn *pg, const lp_id *keys, int nk){
    Run *out = calloc((size_t)(nk ? nk : 1), sizeof(Run)); if (!nk) return out;
    uint8_t *ab = malloc(20 + 20 * (size_t)nk); size_t al = ids_param(ab, keys, (uint32_t)nk); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = db_ask(pg, "SELECT u.i, p.path FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i) JOIN physicality p ON p.entity = u.id", 1, v, l, f);
    if (PQresultStatus(q) == PGRES_TUPLES_OK) for (int j = 0; j < PQntuples(q); j++) { uint64_t o; memcpy(&o, PQgetvalue(q, j, 0), 8); int i = (int)__builtin_bswap64(o) - 1;
        if (i >= 0 && i < nk && !out[i].id) out[i] = run_of((const uint8_t *)PQgetvalue(q, j, 1), (size_t)PQgetlength(q, j, 1)); }
    PQclear(q); free(ab); return out;
}
/* The language each entity is in, one set a step: [h, HELD, x], then [h, SAYS, code]; code[i] for keys[i] ("" none). */
static char (*language_each(Reader *rd, const lp_id *keys, int nk, const lp_id *held, const lp_id *says))[24] {
    char (*code)[24] = calloc((size_t)(nk ? nk : 1), 24); int nh, ns;
    Step *h = open_each(keys, nk, held, 1, 1, 8, &nh); int *hold = malloc(sizeof(int) * (size_t)(nk ? nk : 1)); for (int i = 0; i < nk; i++) hold[i] = -1;
    lp_idmap *m = lp_idmap_new(); lp_id *hs = malloc(sizeof(lp_id) * (size_t)(nh ? nh : 1)); int nu = 0;
    for (int j = 0; j < nh; j++) if (hold[h[j].from] < 0) { bool fresh; size_t at = lp_idmap_put(m, &h[j].to, &fresh); if (fresh) hs[nu++] = h[j].to; hold[h[j].from] = (int)at; }
    Step *sy = open_each(hs, nu, says, 0, 1, 1, &ns); char (*hc)[24] = calloc((size_t)(nu ? nu : 1), 24);
    for (int j = 0; j < ns; j++) reader_want(rd, &sy[j].to);
    for (int j = 0; j < ns; j++) if (!hc[sy[j].from][0]) { char *t = reader_text(rd, &sy[j].to, 20); snprintf(hc[sy[j].from], 24, "%s", t); free(t); }
    for (int i = 0; i < nk; i++) if (hold[i] >= 0) memcpy(code[i], hc[hold[i]], 24);
    free(h); free(hold); free(hs); free(sy); free(hc); lp_idmap_free(m); return code;
}
static Claim *one_open_claims(PGconn *pg, const lp_id *first, const lp_id *rel, int fan, int *n){
    lp_id p[3] = { *first, *rel, *first }; int h[3] = { 2, 2, 0 }, capped; return claims_like(pg, p, h, fan, read_k, n, &capped);
}
int cmd_translate(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; int limit = 4, a = 1;
    for (; a < argc - 1 && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--firmware") && a + 1 < argc) fwp = argv[++a];
    }
    Firmware fw = firmware_for(fwp, FW_TRANSLATE); int fan = fw.fan; read_k = fw.k;
    if (argc - a < 3) { fprintf(stderr, "usage: laplace translate [-d conninfo] [-n concepts] word from to...\n       languages as the resources write them: en de fr ja\n"); return 2; }
    if (fw.nup < 2 || !fw.language[0][0]) { fprintf(stderr, "%s: for translate, the firmware names no way up to a concept and back down (up RELATION..., language HELD SAYS)\n", fw.path); return 2; }
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0); PGconn *pg = db_connect(conninfo); Reader *rd = reader_new(pg);
    const lp_id word = entity_named(c, argv[a], NULL, 0, NULL).id; const char *from = argv[a + 1]; int last = fw.nup - 1, steps = 0;
    lp_id up[FW_CHAIN], held = entity_named(c, fw.language[0], NULL, 0, NULL).id, says = entity_named(c, fw.language[1], NULL, 0, NULL).id, gloss = { { 0 } };
    for (int s = 0; s <= last; s++) up[s] = entity_named(c, fw.up[s], NULL, 0, NULL).id; if (fw.gloss[0]) gloss = entity_named(c, fw.gloss, NULL, 0, NULL).id;

    /* up: the word's first step in its witness's order, then every further step for the whole frontier at once */
    int n; Claim *first = one_open_claims(pg, &word, &up[0], fan, &n); steps++; positions_of(pg, first, n); qsort(first, (size_t)n, sizeof(Claim), claim_by_position);
    int nf = n; lp_id *fr = malloc(sizeof(lp_id) * (size_t)(nf ? nf : 1)); int *orig = malloc(sizeof(int) * (size_t)(nf ? nf : 1));
    for (int i = 0; i < n; i++) { fr[i] = first[i].part[first[i].np - 1]; orig[i] = i; }
    lp_id *syn = calloc((size_t)(n ? n : 1), sizeof(lp_id)); lp_id *concept = calloc((size_t)(n ? n : 1), sizeof(lp_id)); lp_rating *cr = calloc((size_t)(n ? n : 1), sizeof(lp_rating)); uint8_t *got = calloc((size_t)(n ? n : 1), 1);
    for (int s = 1; s <= last && nf; s++) {
        int ns; Step *st = open_each(fr, nf, &up[s], 0, 1, s == last ? 1 : 8, &ns); steps++;
        uint8_t *had = calloc((size_t)nf, 1); for (int j = 0; j < ns; j++) had[st[j].from] = 1;
        if (s == last) { for (int j = 0; j < ns; j++) { int o = orig[st[j].from]; if (got[o]) continue; got[o] = 1; syn[o] = fr[st[j].from]; concept[o] = st[j].to; cr[o] = st[j].r; } free(had); free(st); break; }
        int nn = 0, cap = ns + 1; lp_id *nx = malloc(sizeof(lp_id) * (size_t)cap); int *no = malloc(sizeof(int) * (size_t)cap);
        #define PUSH(id, o) do { if (nn == cap) { cap *= 2; nx = xrealloc(nx, sizeof(lp_id) * (size_t)cap); no = xrealloc(no, sizeof(int) * (size_t)cap); } nx[nn] = (id); no[nn++] = (o); } while (0)
        for (int j = 0; j < ns; j++) PUSH(st[j].to, orig[st[j].from]);
        int nb = 0; lp_id *bare = malloc(sizeof(lp_id) * (size_t)nf); int *bo = malloc(sizeof(int) * (size_t)nf);
        for (int i = 0; i < nf; i++) if (!had[i]) { bare[nb] = fr[i]; bo[nb++] = orig[i]; }
        Run *pr = parts_each(pg, bare, nb); steps += nb > 0;                 /* no claim carries the step: the constituents, never back to the word */
        for (int i = 0; i < nb; i++) { for (int p = 0; p < pr[i].n; p++) if (memcmp(&pr[i].id[p], &word, 16)) PUSH(pr[i].id[p], bo[i]); free(pr[i].id); }
        free(pr); free(bare); free(bo); free(had); free(st); free(fr); free(orig); fr = nx; orig = no; nf = nn;
        #undef PUSH
    }
    free(fr); free(orig);
    /* the concepts reached from synsets in the language asked for, each once, in the word's order */
    char (*lang)[24] = language_each(rd, syn, n, &held, &says); steps += 2;
    int nc = 0; int *pick = malloc(sizeof(int) * (size_t)(n ? n : 1));
    for (int i = 0; i < n && nc < limit; i++) { if (!got[i] || strcmp(lang[i], from)) continue; int dup = 0; for (int z = 0; z < nc; z++) dup |= !memcmp(&concept[pick[z]], &concept[i], 16); if (!dup) pick[nc++] = i; }
    free(lang);
    if (!nc) { printf("%s: no concept of it is attested in a language written %s\n", argv[a], from); }
    /* down: every concept's synsets in one set, their languages in one, then each step back down for all of them */
    lp_id *cs = malloc(sizeof(lp_id) * (size_t)(nc ? nc : 1)), *ss = malloc(sizeof(lp_id) * (size_t)(nc ? nc : 1)); for (int z = 0; z < nc; z++) { cs[z] = concept[pick[z]]; ss[z] = syn[pick[z]]; }
    int ng = 0; Step *gl = fw.gloss[0] ? open_each(ss, nc, &gloss, 0, 1, 1, &ng) : NULL; steps += fw.gloss[0] != 0;
    int nd; Step *dn = open_each(cs, nc, &up[last], 1, 1, fan, &nd); steps++;
    lp_id *ys = malloc(sizeof(lp_id) * (size_t)(nd ? nd : 1)); for (int j = 0; j < nd; j++) ys[j] = dn[j].to;
    char (*yl)[24] = language_each(rd, ys, nd, &held, &says); steps += 2;
    int nf2 = 0; lp_id *f2 = malloc(sizeof(lp_id) * (size_t)(nd ? nd : 1)); int *o2 = malloc(sizeof(int) * (size_t)(nd ? nd : 1));   /* o2: concept * 64 + target language */
    for (int j = 0; j < nd; j++) for (int g = a + 2; g < argc && g - a - 2 < 64; g++) if (!strcmp(yl[j], argv[g])) { f2[nf2] = ys[j]; o2[nf2++] = dn[j].from * 64 + (g - a - 2); }
    free(yl); free(ys);
    for (int s = last - 1; s >= 0 && nf2; s--) {
        int ns; Step *st = open_each(f2, nf2, &up[s], 1, 1, fan, &ns); steps++;
        uint8_t *had = calloc((size_t)nf2, 1); for (int j = 0; j < ns; j++) had[st[j].from] = 1;
        int nn = 0, cap = ns + 1; lp_id *nx = malloc(sizeof(lp_id) * (size_t)cap); int *no = malloc(sizeof(int) * (size_t)cap);
        #define PUSH(id, o) do { if (nn == cap) { cap *= 2; nx = xrealloc(nx, sizeof(lp_id) * (size_t)cap); no = xrealloc(no, sizeof(int) * (size_t)cap); } nx[nn] = (id); no[nn++] = (o); } while (0)
        for (int j = 0; j < ns; j++) PUSH(st[j].to, o2[st[j].from]);
        if (s > 0) { int nb = 0; lp_id *bare = malloc(sizeof(lp_id) * (size_t)nf2); int *bo = malloc(sizeof(int) * (size_t)nf2);
            for (int i = 0; i < nf2; i++) if (!had[i]) { bare[nb] = f2[i]; bo[nb++] = o2[i]; }
            int nh; Step *hs = nb ? open_each(bare, nb, NULL, 0, 0, fan, &nh) : NULL; steps += nb > 0;     /* no claim carries the step: what holds it */
            for (int j = 0; nb && j < nh; j++) PUSH(hs[j].to, bo[hs[j].from]); free(hs); free(bare); free(bo); }
        free(had); free(st); free(f2); free(o2); f2 = nx; o2 = no; nf2 = nn;
        #undef PUSH
    }
    for (int j = 0; j < nf2; j++) reader_want(rd, &f2[j]); for (int z = 0; z < nc; z++) reader_want(rd, &cs[z]); for (int j = 0; j < ng; j++) reader_want(rd, &gl[j].to);
    for (int z = 0; z < nc; z++) {
        int i = pick[z]; char *it = reader_text(rd, &cs[z], 60), *dt = NULL;
        for (int j = 0; j < ng && !dt; j++) if (gl[j].from == z) dt = reader_text(rd, &gl[j].to, 100);
        printf("\n  %4.0f \xC2\xB1 %-3.0f  %s\n            %s\n", cr[i].rating, cr[i].deviation, it, dt ? dt : ""); free(it); free(dt);
        for (int g = a + 2; g < argc && g - a - 2 < 64; g++) {
            printf("    %-6s", argv[g]); int any = 0; lp_idmap *shown = lp_idmap_new();
            for (int j = 0; j < nf2 && any < 8; j++) { if (o2[j] != z * 64 + (g - a - 2)) continue; bool fresh; lp_idmap_put(shown, &f2[j], &fresh); if (!fresh) continue;
                char *tx = reader_text(rd, &f2[j], 40); printf("%s%s", any ? ", " : " ", tx); free(tx); any++; }
            lp_idmap_free(shown); if (!any) printf(" (nothing attested)"); printf("\n"); }
    }
    printf("\n%d steps, each one set   total %.1f ms\n", steps, (now() - T) * 1000);
    free(first); free(syn); free(concept); free(cr); free(got); free(pick); free(cs); free(ss); free(gl); free(dn); free(f2); free(o2); reader_free(rd); PQfinish(pg);
    return 0;
}

/* ---- degrees of separation: how far anything is from anything else
 * Best-first over claims, a batch of the nearest open entities per round trip: each entity in the batch gets its claims
 * through the container index, read at most K (--fan) of them, which is what keeps a step O(K). An entity that holds
 * more than K claims (a part of speech, a language) is reached and not read through. Between two entities the search
 * runs from both ends and stops when no open strand can beat the best chain found. */
typedef struct { lp_frontier *f; lp_id origin; } Side;
typedef struct { uint64_t trips, expanded, claims, hubs; } Work;

static int decode_claim(const char *path, int len, lp_id part[3]){ return (int)lp_path_ids((const uint8_t *)path, (size_t)len, part, 3); }

/* Close up to batch of the side's nearest open entities and reach across their claims. Returns how many it closed;
 * closed[] receives them. */
static int expand(PGconn *pg, Side *sd, int batch, int fan, int hops, double k, double per_hop, lp_reached *closed, uint8_t *hub, Work *w, const Firmware *way, const lp_id *wids){
    int n = 0; const lp_reached *x;
    while (n < batch && (x = lp_frontier_next(sd->f))) { hub[n] = 0; closed[n++] = *x; }
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)(n ? n : 1)); int *who = malloc(sizeof(int) * (size_t)(n ? n : 1)), m = 0;
    for (int i = 0; i < n; i++) if ((int)closed[i].hops < hops) { ids[m] = closed[i].id; who[m++] = i; }
    if (!m) { free(ids); free(who); return n; }
    uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = ids_param(ab, ids, (uint32_t)m); char lim[16]; snprintf(lim, sizeof lim, "%d", fan + 1);
    int rl; const char *v[4] = { (const char *)ab, lim, CLAIM_BITS, refuse_param(&rl) }; int l[4] = { (int)al, 0, 0, rl }, f[4] = { 1, 0, 0, 1 };
    PGresult *q = db_ask(pg,
        "SELECT i, entity, path, rating, deviation, volatility FROM laplace_claims_each($1::blake3[], $2::bigint, $3::smallint[], $4::blake3[])", 4, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "claims: %s", PQerrorMessage(pg)); exit(1); }
    w->trips++; w->expanded += (uint64_t)m;
    int rows = PQntuples(q), *held = calloc((size_t)m, sizeof(int));
    for (int j = 0; j < rows; j++) { uint64_t o; memcpy(&o, PQgetvalue(q, j, 0), 8); held[__builtin_bswap64(o) - 1]++; }
    for (int j = 0; j < rows; j++) {
        uint64_t o; memcpy(&o, PQgetvalue(q, j, 0), 8); int e = (int)(__builtin_bswap64(o) - 1); const lp_reached *at = &closed[who[e]];
        if (held[e] > fan && memcmp(&at->id, &sd->origin, 16)) continue;                  /* a hub: reached, not crossed */
        lp_id part[3], claim; if (decode_claim(PQgetvalue(q, j, 2), PQgetlength(q, j, 2), part) != 3) continue;
        const lp_id *other;                                                                /* a claim ties its subject to its object */
        if (!memcmp(&part[0], &at->id, 16)) other = &part[2];
        else if (!memcmp(&part[2], &at->id, 16)) other = &part[0];
        else continue;                                                                     /* held as the predicate: it names the tie */
        if (!memcmp(other, &at->id, 16)) continue;
        lp_rating r = { lp_be_f64(PQgetvalue(q, j, 3)), lp_be_f64(PQgetvalue(q, j, 4)), lp_be_f64(PQgetvalue(q, j, 5)) };
        memcpy(claim.b, PQgetvalue(q, j, 1), 16); w->claims++;
        double sw = strand_weight(way, wids, part, 3); if (sw <= 0) continue;                 /* a kind weighed at nothing is not crossed */
        lp_frontier_reach(sd->f, other, &at->id, &claim, at->cost + lp_cost(&r, k, per_hop) - log(sw), 0, at->hops + 1);
    }
    for (int e = 0; e < m; e++) if (held[e] > fan && memcmp(&ids[e], &sd->origin, 16)) { w->hubs++; hub[who[e]] = 1; }
    PQclear(q); free(ab); free(ids); free(who); free(held);
    return n;
}

/* The tie between two neighbours of a chain, as text: the claim's predicate, read from its subject to its object. */
static void show_tie(PGconn *pg, Reader *rd, const lp_id *claim, const lp_id *left){
    uint8_t ab[40]; size_t al = ids_param(ab, claim, 1); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, fm[1] = { 1 };
    PGresult *q = db_ask(pg, "SELECT path FROM laplace_paths($1::blake3[]) LIMIT 1", 1, v, l, fm);
    lp_id part[3]; int np = PQresultStatus(q) == PGRES_TUPLES_OK && PQntuples(q) ? decode_claim(PQgetvalue(q, 0, 0), PQgetlength(q, 0, 0), part) : 0;
    PQclear(q);
    char *pt = np == 3 ? reader_text(rd, &part[1], 32) : strdup("?");
    printf(np == 3 && !memcmp(&part[0], left, 16) ? " -[%s]-> " : " <-[%s]- ", pt); free(pt);
}
/* The chain from a side's origin to an entity, origin first. Returns its length. */
static int chain_to(const lp_frontier *f, const lp_id *id, lp_id *chain, lp_id *via, int cap){
    int n = 0; const lp_reached *r = lp_frontier_find(f, id);
    while (r && n < cap) { chain[n] = r->id; via[n] = r->claim; n++; if (!r->hops) break; r = lp_frontier_find(f, &r->from); }
    for (int i = 0; i < n / 2; i++) { lp_id t = chain[i]; chain[i] = chain[n - 1 - i]; chain[n - 1 - i] = t; }
    /* via[i] is the claim chain[i] was reached through, from chain[i - 1] */
    for (int i = 0; i < n / 2; i++) { lp_id t = via[i]; via[i] = via[n - 1 - i]; via[n - 1 - i] = t; }
    return n;
}
static void show_chain(PGconn *pg, Reader *rd, const lp_id *chain, const lp_id *via, int n, double cost, double per_hop){
    for (int i = 0; i < n; i++) reader_want(rd, &chain[i]);
    printf("%10.3g %5d   ", exp(-(cost - per_hop * (n - 1))), n - 1);
    for (int i = 0; i < n; i++) {
        if (i) show_tie(pg, rd, &via[i], &chain[i - 1]);
        char *tx = reader_text(rd, &chain[i], 48); printf("%s", tx); free(tx);
    }
    printf("\n");
}
static int by_cost(const void *a, const void *b){ double x = ((const lp_reached *)a)->cost, y = ((const lp_reached *)b)->cost; return x < y ? -1 : x > y; }

int cmd_degrees(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; int limit = 24, fan = -1, hops = -1, batch = 64, a = 1; double k = -1, per_hop = -1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "--firmware") && a + 1 < argc) fwp = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--fan") && a + 1 < argc) fan = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--hops") && a + 1 < argc) hops = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--batch") && a + 1 < argc) batch = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--k") && a + 1 < argc) k = atof(argv[++a]);
        else if (!strcmp(argv[a], "--per-hop") && a + 1 < argc) per_hop = atof(argv[++a]);
        else break;
    }
    if (a >= argc || batch < 1) { fprintf(stderr, "usage: laplace degrees [-d conninfo] [-n N] [--hops H] [--fan K] [--batch B] [--k K] [--per-hop C] from [to]\n"); return 2; }
    Firmware way = firmware_for(fwp, FW_SEARCH); if (fan < 0) fan = way.fan; if (hops < 0) hops = way.hops; if (k < 0) k = way.k; if (per_hop < 0) per_hop = way.lambda;
    firmware_say(&way, FW_SEARCH);
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0);
    lp_ref from = entity_named(c, argv[a], NULL, 0, NULL), to; int goal = a + 1 < argc;
    lp_id wids[FW_WEIGHS]; weights_named(c, &way, wids);
    show_ref("from", &from); if (goal) { to = entity_named(c, argv[a + 1], NULL, 0, NULL); show_ref("to", &to); }
    PGconn *pg = db_connect(conninfo); Reader *rd = reader_new(pg); Work w = { 0 };
    lp_reached *closed = malloc(sizeof(lp_reached) * (size_t)batch); uint8_t *hub = malloc((size_t)batch);
    Side fw = { lp_frontier_new(), from.id }; lp_frontier_reach(fw.f, &from.id, NULL, NULL, 0, 0, 0);

    if (!goal) {
        lp_reached *near = NULL; size_t nn = 0, cn = 0;
        for (;;) {
            if (nn >= (size_t)limit) {                                        /* the N nearest are known once nothing open is nearer */
                qsort(near, nn, sizeof *near, by_cost);
                if (lp_frontier_least(fw.f) >= near[limit - 1].cost) break;
            }
            int n = expand(pg, &fw, batch, fan, hops, k, per_hop, closed, hub, &w, &way, wids); if (!n) break;
            for (int i = 0; i < n; i++) if (memcmp(&closed[i].id, &from.id, 16)) {
                if (nn == cn) { cn = cn ? cn * 2 : 256; near = xrealloc(near, cn * sizeof *near); } near[nn++] = closed[i]; }
        }
        double t_search = (now() - T) * 1000;
        qsort(near, nn, sizeof *near, by_cost);
        printf("\n%10s %5s   %s\n", "confidence", "hops", "reached");
        for (size_t i = 0; i < nn && i < (size_t)limit; i++) { lp_id chain[66], via[66]; int n = chain_to(fw.f, &near[i].id, chain, via, 66); show_chain(pg, rd, chain, via, n, near[i].cost, per_hop); }
        printf("\n%llu entities expanded in %llu round trips, %llu claims crossed, %zu entities reached, %llu not read through; search %.1f ms, total %.1f ms\n",
               (unsigned long long)w.expanded, (unsigned long long)w.trips, (unsigned long long)w.claims, lp_frontier_count(fw.f), (unsigned long long)w.hubs, t_search, (now() - T) * 1000);
        free(near);
    } else {
        Side bw = { lp_frontier_new(), to.id }; lp_frontier_reach(bw.f, &to.id, NULL, NULL, 0, 0, 0);
        double best = INFINITY; lp_id meet; memset(&meet, 0, sizeof meet);
        for (;;) {
            double lf = lp_frontier_least(fw.f), lb = lp_frontier_least(bw.f);
            if (lf + lb >= best || (isinf(lf) && isinf(lb))) break;           /* no open strand can beat the chain found */
            Side *sd = lf <= lb ? &fw : &bw, *ot = sd == &fw ? &bw : &fw;
            int n = expand(pg, sd, batch, fan, (hops + 1) / 2, k, per_hop, closed, hub, &w, &way, wids); if (!n) break;
            for (int i = 0; i < n; i++) {                                     /* where the two sides meet; never at a hub */
                if (hub[i]) continue;
                const lp_reached *o = lp_frontier_find(ot->f, &closed[i].id);
                if (o && closed[i].cost + o->cost < best && (int)(closed[i].hops + o->hops) <= hops) { best = closed[i].cost + o->cost; meet = closed[i].id; }
            }
        }
        double t_search = (now() - T) * 1000;
        if (isinf(best)) printf("\nno chain of at most %d claims ties them: %zu entities reached from one, %zu from the other\n", hops, lp_frontier_count(fw.f), lp_frontier_count(bw.f));
        else {
            lp_id chain[140], via[140], back[70], bvia[70]; int n = chain_to(fw.f, &meet, chain, via, 66), m = chain_to(bw.f, &meet, back, bvia, 66);
            /* back runs from `to` to the meeting entity: append it reversed; the claim between back[i] and back[i + 1] is bvia[i + 1] */
            for (int i = m - 2; i >= 0; i--) { chain[n] = back[i]; via[n] = bvia[i + 1]; n++; }
            printf("\n%10s %5s   %s\n", "confidence", "hops", "chain"); show_chain(pg, rd, chain, via, n, best, per_hop);
        }
        printf("\n%llu entities expanded in %llu round trips, %llu claims crossed, %zu + %zu entities reached, %llu not read through; search %.1f ms, total %.1f ms\n",
               (unsigned long long)w.expanded, (unsigned long long)w.trips, (unsigned long long)w.claims, lp_frontier_count(fw.f), lp_frontier_count(bw.f),
               (unsigned long long)w.hubs, t_search, (now() - T) * 1000);
        lp_frontier_free(bw.f);
        if (isinf(best)) { free(closed); free(hub); reader_free(rd); lp_frontier_free(fw.f); PQfinish(pg); return 1; }
    }
    free(closed); free(hub); reader_free(rd); lp_frontier_free(fw.f); PQfinish(pg);
    return 0;
}
