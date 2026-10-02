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

static double be_f64(const char *p){ uint64_t u = 0; for (int i = 0; i < 8; i++) u = u << 8 | (uint8_t)p[i]; double d; memcpy(&d, &u, 8); return d; }
int claim_by_conf(const void *a, const void *b){ double x = ((const Claim *)a)->conf, y = ((const Claim *)b)->conf; return x < y ? 1 : x > y ? -1 : memcmp(a, b, 16); }

/* The claims that hold the given parts in their places (a part not given is open): at most fan of them; *capped says
 * there were more. k: how many deviations below its rating a claim is read at. */
Claim *claims_like(PGconn *pg, const lp_id *part, const int *have, int fan, double k, int *n, int *capped){
    lp_id keys[3]; uint32_t nk = 0; for (int i = 0; i < 3; i++) if (have[i]) keys[nk++] = part[i];
    uint8_t ab[80]; size_t al = ids_param(ab, keys, nk); char lim[16]; snprintf(lim, sizeof lim, "%d", fan + 1);
    const char *v[3] = { (const char *)ab, lim, CLAIM_BITS }; int l[3] = { (int)al, 0, 0 }, f[3] = { 1, 0, 0 };
    PGresult *q = db_ask(pg,
        "SELECT entity, path, rating, deviation, volatility, matches FROM laplace_claims($1::blake3[], $2::bigint, $3::smallint[])", 3, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "claims: %s", PQerrorMessage(pg)); exit(1); }
    int rows = PQntuples(q); *capped = rows > fan; if (rows > fan) rows = fan;
    Claim *c = malloc(sizeof(Claim) * (size_t)(rows ? rows : 1)); int m = 0;
    for (int j = 0; j < rows; j++) {
        Claim *x = &c[m]; memcpy(x->id.b, PQgetvalue(q, j, 0), 16); x->np = 0; x->position = 0;
        const uint8_t *vx; size_t nv = lp_ewkb_vertices((const uint8_t *)PQgetvalue(q, j, 1), (size_t)PQgetlength(q, j, 1), &vx);
        for (size_t i = 0; i < nv; i++) {
            double xyz[3], run; memcpy(xyz, vx + 32 * i, 24); memcpy(&run, vx + 32 * i + 24, 8); lp_id id; lp_xyz_to_id(xyz, &id);
            for (int r = 0; r < (int)lp_m_run(run) && x->np < MAXPARTS; r++) x->part[x->np++] = id;
        }
        if (x->np < 2) continue;
        /* in its place: the first part given is the claim's first, the last its last, and the middle one between them */
        int fits = 1, last = x->np - 1;
        if (have[0] > 1 && memcmp(&x->part[0], &part[0], 16)) fits = 0;
        if (have[2] > 1 && memcmp(&x->part[last], &part[2], 16)) fits = 0;
        if (have[1] > 1) { int in = 0; for (int i = 1; i < last || (i == 1 && x->np == 2 && i <= last); i++) if (!memcmp(&x->part[i], &part[1], 16)) in = 1; if (!in) fits = 0; }
        if (!fits) continue;
        x->r = (lp_rating){ be_f64(PQgetvalue(q, j, 2)), be_f64(PQgetvalue(q, j, 3)), be_f64(PQgetvalue(q, j, 4)) };
        uint32_t mb; memcpy(&mb, PQgetvalue(q, j, 5), 4); x->matches = (int)ntohl(mb);
        x->conf = lp_confidence(&x->r, k); m++;
    }
    PQclear(q); *n = m;
    qsort(c, (size_t)m, sizeof(Claim), claim_by_conf);
    return c;
}
/* Every claim that holds an entity, wherever in it. */
Claim *claims_of(PGconn *pg, const lp_id *e, int fan, double k, int *n, int *capped){
    lp_id part[3] = { *e, *e, *e }; int have[3] = { 1, 0, 0 };
    return claims_like(pg, part, have, fan, k, n, capped);
}
/* The position each claim was given by the witnesses that gave one: the least, as recorded in the ledger. */
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
     * gave them. For the claims the ledger gave no place, the records that hold each are fetched as one set, and the
     * claim's place is where it stands in the path, the least when several records hold it. */
    int need = 0; for (int i = 0; i < n; i++) need += !c[i].position;
    if (need) {
        const char *v2[1] = { (const char *)ab }; int l2[1] = { (int)al }, f2[1] = { 1 };
        PGresult *r = db_ask(pg, "SELECT u.i, k.path FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i), LATERAL laplace_containers(ARRAY[u.id], '{1}'::smallint[]) k", 1, v2, l2, f2);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "records: %s", PQerrorMessage(pg)); exit(1); }
        for (int j = 0; j < PQntuples(r); j++) {
            uint64_t o; memcpy(&o, PQgetvalue(r, j, 0), 8); int i = (int)(__builtin_bswap64(o) - 1); if (i < 0 || i >= n || (c[i].position && c[i].position < (1 << 20))) { if (i < 0 || i >= n) continue; }
            const uint8_t *vx; size_t nv = lp_ewkb_vertices((const uint8_t *)PQgetvalue(r, j, 1), (size_t)PQgetlength(r, j, 1), &vx); int at = 0, place = 0;
            { double h3[3]; lp_id head; if (!nv) continue; memcpy(h3, vx, 24); lp_xyz_to_id(h3, &head); if (memcmp(&head, &c[i].part[0], 16)) continue; }   /* the trajectory under the claim's own subject: the order its witness gave */
            for (size_t z = 0; z < nv && !place; z++) { double xyz[3], run; memcpy(xyz, vx + 32 * z, 24); memcpy(&run, vx + 32 * z + 24, 8); lp_id id; lp_xyz_to_id(xyz, &id);
                if (!memcmp(&id, &c[i].id, 16)) place = at + 1; at += (int)lp_m_run(run); }
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
static double read_k = 2.0;                                                  /* the firmware's k, for the lookups below */
static Claim *one_open(PGconn *pg, Ctx *c, const lp_id *first, const char *middle, const lp_id *last, int fan, int *n){
    lp_id p[3]; int h[3] = { first ? 2 : 0, 2, last ? 2 : 0 }, capped; if (first) p[0] = *first; if (last) p[2] = *last;
    p[1] = entity_named(c, middle, NULL, 0, NULL).id; return claims_like(pg, p, h, fan, read_k, n, &capped);
}
typedef struct { lp_id held; char code[24]; } Lang;
static const char *language_of(PGconn *pg, Ctx *c, Reader *rd, const Firmware *fw, const lp_id *x, Lang **known, int *nknown){
    int n; Claim *lx = one_open(pg, c, NULL, fw->language[0], x, 8, &n); if (!n) { free(lx); return ""; }
    lp_id h = lx[0].part[0]; free(lx);
    for (int i = 0; i < *nknown; i++) if (!memcmp(&(*known)[i].held, &h, 16)) return (*known)[i].code;
    Claim *lg = one_open(pg, c, &h, fw->language[1], NULL, 8, &n); *known = xrealloc(*known, sizeof(Lang) * (size_t)(*nknown + 1)); Lang *k = &(*known)[(*nknown)++]; k->held = h; k->code[0] = 0;
    if (n) { char *t = reader_text(rd, &lg[0].part[lg[0].np - 1], 20); snprintf(k->code, sizeof k->code, "%s", t); free(t); }
    free(lg); return k->code;
}
/* Down from x along the steps up[from..0], in the other direction: the words at the bottom, at most cap of them. */
static int down(PGconn *pg, Ctx *c, Reader *rd, const Firmware *fw, const lp_id *x, int from, int fan, int *lookups, int cap, int any){
    if (from < 0) { char *tx = reader_text(rd, x, 40); printf("%s%s", any ? ", " : " ", tx); free(tx); return 1; }
    int n, shown = 0; Claim *cl = one_open(pg, c, NULL, fw->up[from], x, from ? 256 : 8, &n); (*lookups)++;
    for (int i = 0; i < n && any + shown < cap; i++) shown += down(pg, c, rd, fw, &cl[i].part[0], from - 1, fan, lookups, cap, any + shown);
    free(cl); return shown;
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
    lp_id word = entity_named(c, argv[a], NULL, 0, NULL).id; const char *from = argv[a + 1]; Lang *known = NULL; int nknown = 0, n, lookups = 0, shown = 0, last = fw.nup - 1;
    Claim *first = one_open(pg, c, &word, fw.up[0], NULL, fan, &n); lookups++; positions_of(pg, first, n); qsort(first, (size_t)n, sizeof(Claim), claim_by_position);
    lp_id seen[64]; int nseen = 0;
    for (int i = 0; i < n && shown < limit; i++) {
        lp_id x = first[i].part[first[i].np - 1]; int ok = 1;
        for (int s = 1; s < last && ok; s++) { int ny; Claim *st = one_open(pg, c, &x, fw.up[s], NULL, 8, &ny); lookups++; if (ny) x = st[0].part[st[0].np - 1]; else ok = 0; free(st); }
        if (!ok || strcmp(language_of(pg, c, rd, &fw, &x, &known, &nknown), from)) continue;
        int ni; Claim *il = one_open(pg, c, &x, fw.up[last], NULL, 8, &ni); lookups += 3; if (!ni) { free(il); continue; }
        lp_id concept = il[0].part[il[0].np - 1]; lp_rating ir = il[0].r; free(il);
        int dup = 0; for (int z = 0; z < nseen; z++) dup |= !memcmp(&seen[z], &concept, 16); if (dup) continue; if (nseen < 64) seen[nseen++] = concept;
        int nd = 0; Claim *df = fw.gloss[0] ? one_open(pg, c, &x, fw.gloss, NULL, 8, &nd) : NULL; lookups += fw.gloss[0] != 0;
        char *it = reader_text(rd, &concept, 60), *dt = nd ? reader_text(rd, &df[0].part[df[0].np - 1], 100) : strdup(""); free(df);
        printf("\n  %4.0f \xC2\xB1 %-3.0f  %s\n            %s\n", ir.rating, ir.deviation, it, dt); free(it); free(dt); shown++;
        int no; Claim *others = one_open(pg, c, NULL, fw.up[last], &concept, fan, &no); lookups++;
        for (int g = a + 2; g < argc; g++) {
            printf("    %-6s", argv[g]); int any = 0;
            for (int o = 0; o < no && any < 8; o++) { lp_id y = others[o].part[0];
                if (strcmp(language_of(pg, c, rd, &fw, &y, &known, &nknown), argv[g])) continue;
                any += down(pg, c, rd, &fw, &y, last - 1, fan, &lookups, 8, any); }
            if (!any) printf(" (nothing attested)");
            printf("\n");
        }
        free(others);
    }
    if (!shown) printf("%s: no concept of it is attested in a language written %s\n", argv[a], from);
    printf("\n%d lookups   total %.1f ms\n", lookups, (now() - T) * 1000);
    free(first); free(known); reader_free(rd); PQfinish(pg);
    return 0;
}

/* ---- degrees of separation: how far anything is from anything else
 * Best-first over claims, a batch of the nearest open entities per round trip: each entity in the batch gets its claims
 * through the container index, read at most K (--fan) of them, which is what keeps a step O(K). An entity that holds
 * more than K claims (a part of speech, a language) is reached and not read through. Between two entities the search
 * runs from both ends and stops when no open strand can beat the best chain found. */
typedef struct { lp_frontier *f; lp_id origin; } Side;
typedef struct { uint64_t trips, expanded, claims, hubs; } Work;

static int decode_claim(const char *path, int len, lp_id part[3]){
    const uint8_t *vx; size_t nv = lp_ewkb_vertices((const uint8_t *)path, (size_t)len, &vx); int np = 0;
    for (size_t i = 0; i < nv; i++) {
        double xyz[3], run; memcpy(xyz, vx + 32 * i, 24); memcpy(&run, vx + 32 * i + 24, 8); lp_id id; lp_xyz_to_id(xyz, &id);
        for (int r = 0; r < (int)lp_m_run(run); r++) { if (np < 3) part[np] = id; np++; }
    }
    return np;
}

/* Close up to batch of the side's nearest open entities and reach across their claims. Returns how many it closed;
 * closed[] receives them. */
static int expand(PGconn *pg, Side *sd, int batch, int fan, int hops, double k, double per_hop, lp_reached *closed, uint8_t *hub, Work *w, const Firmware *way, const lp_id *wids){
    int n = 0; const lp_reached *x;
    while (n < batch && (x = lp_frontier_next(sd->f))) { hub[n] = 0; closed[n++] = *x; }
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)(n ? n : 1)); int *who = malloc(sizeof(int) * (size_t)(n ? n : 1)), m = 0;
    for (int i = 0; i < n; i++) if ((int)closed[i].hops < hops) { ids[m] = closed[i].id; who[m++] = i; }
    if (!m) { free(ids); free(who); return n; }
    uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = ids_param(ab, ids, (uint32_t)m); char lim[16]; snprintf(lim, sizeof lim, "%d", fan + 1);
    const char *v[3] = { (const char *)ab, lim, CLAIM_BITS }; int l[3] = { (int)al, 0, 0 }, f[3] = { 1, 0, 0 };
    PGresult *q = db_ask(pg,
        "SELECT i, entity, path, rating, deviation, volatility FROM laplace_claims_each($1::blake3[], $2::bigint, $3::smallint[])", 3, v, l, f);
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
        lp_rating r = { be_f64(PQgetvalue(q, j, 3)), be_f64(PQgetvalue(q, j, 4)), be_f64(PQgetvalue(q, j, 5)) };
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
