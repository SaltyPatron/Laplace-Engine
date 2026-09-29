/* Reading the web of claims. An entity is named by its text and found by the ID computed here; everything attested
 * about it is the claims whose paths hold it, fetched through the container index with their standings, one set-based
 * fetch per step: O(log N) to find them, O(K) to read them. How hard a strand tugs back is its claim's confidence.
 * Nothing here changes a standing: querying reads scores, it does not move them.
 *
 * This is not yet the forward pass. It is the lookups the forward pass is made of: what is attested about an entity
 * (hop), a claim with a part left open (hop a b ?), a word up to its concepts and down into another language
 * (translate), and how far one entity is from another (degrees).
 *
 *   laplace text  text
 *   laplace hop   [-d conninfo] [-n N] [--fan K] text | subject predicate object (? for a part left open)
 *   laplace translate [-d conninfo] [-n N] word from to...
 *   laplace degrees [-d conninfo] [-n N] [--hops H] [--fan K] [--batch B] [--k K] [--per-hop C] from [to] */
#include "engine.h"
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- naming an entity: the text's trunk, computed on the client */
static lp_ref named(Ctx *c, const char *text, lp_ref *parts, size_t cap, size_t *np){
    lp_ref p[1]; size_t n;
    return lp_text_parts(c, (const uint8_t *)text, strlen(text), parts ? parts : p, parts ? cap : 1, np ? np : &n);
}
static void show_ref(const char *label, const lp_ref *r){
    char id[37]; id_text(&r->id, id);
    printf("%-10s %s   tier %d   (%.6f, %.6f, %.6f, %.6f)   hilbert %016llx   depth %.6f\n", label, id, r->tier,
           (double)r->c.m[0] / LP_FIXED_ONE, (double)r->c.m[1] / LP_FIXED_ONE, (double)r->c.m[2] / LP_FIXED_ONE, (double)r->c.m[3] / LP_FIXED_ONE,
           (unsigned long long)lp_hilbert4(&r->c),
           1.0 - sqrt(((double)r->c.m[0] * r->c.m[0] + (double)r->c.m[1] * r->c.m[1] + (double)r->c.m[2] * r->c.m[2] + (double)r->c.m[3] * r->c.m[3])) / LP_FIXED_ONE);
}

int cmd_text(int argc, char **argv){
    if (argc < 2) { fprintf(stderr, "usage: laplace text text\n"); return 2; }
    double t = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0);
    lp_ref parts[64]; size_t np; lp_ref r = named(c, argv[1], parts, 64, &np);
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
#define MAXPARTS 12
typedef struct { lp_id id, part[MAXPARTS]; int np; lp_rating r; int matches; double conf; int position; } Claim;   /* a claim is a tuple: a pair, three parts, or a longer path */

static double be_f64(const char *p){ uint64_t u = 0; for (int i = 0; i < 8; i++) u = u << 8 | (uint8_t)p[i]; double d; memcpy(&d, &u, 8); return d; }
static int claim_by_conf(const void *a, const void *b){ double x = ((const Claim *)a)->conf, y = ((const Claim *)b)->conf; return x < y ? 1 : x > y ? -1 : memcmp(a, b, 16); }

/* The claims that hold the given parts in their places (a part not given is open): at most fan of them; *capped says
 * there were more. k: how many deviations below its rating a claim is read at. */
static Claim *claims_like(PGconn *pg, const lp_id *part, const int *have, int fan, double k, int *n, int *capped){
    lp_id keys[3]; uint32_t nk = 0; for (int i = 0; i < 3; i++) if (have[i]) keys[nk++] = part[i];
    uint8_t ab[80]; size_t al = uuid_param(ab, keys, nk); char lim[16]; snprintf(lim, sizeof lim, "%d", fan + 1);
    const char *v[2] = { (const char *)ab, lim }; int l[2] = { (int)al, 0 }, f[2] = { 1, 0 };
    PGresult *q = PQexecParams(pg,
        "SELECT p.entity, p.path, s.rating, s.deviation, s.volatility, s.matches FROM physicality p JOIN standing s ON s.claim = p.entity "
        "WHERE p.path @> $1::uuid[] LIMIT $2::int", 2, NULL, v, l, f, 1);
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
static Claim *claims_of(PGconn *pg, const lp_id *e, int fan, double k, int *n, int *capped){
    lp_id part[3] = { *e, *e, *e }; int have[3] = { 1, 0, 0 };
    return claims_like(pg, part, have, fan, k, n, capped);
}
/* The position each claim was given by the witnesses that gave one: the least, as recorded in the ledger. */
static void positions_of(PGconn *pg, Claim *c, int n){
    if (!n) return;
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)n); for (int i = 0; i < n; i++) ids[i] = c[i].id;
    uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = uuid_param(ab, ids, (uint32_t)n);
    const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = PQexecParams(pg, "SELECT claim, position FROM attestation WHERE claim = ANY($1::uuid[]) AND position IS NOT NULL", 1, NULL, v, l, f, 1);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "positions: %s", PQerrorMessage(pg)); exit(1); }
    for (int j = 0; j < PQntuples(q); j++) {
        uint32_t pb; memcpy(&pb, PQgetvalue(q, j, 1), 4); int pos = (int)ntohl(pb);
        for (int i = 0; i < n; i++) if (!memcmp(c[i].id.b, PQgetvalue(q, j, 0), 16)) { if (!c[i].position || pos < c[i].position) c[i].position = pos; break; }
    }
    PQclear(q); free(ab); free(ids);
}
/* As given first, then by how hard the strand tugs back. */
static int claim_by_position(const void *a, const void *b){
    const Claim *x = a, *y = b; int px = x->position ? x->position : 1 << 30, py = y->position ? y->position : 1 << 30;
    return px != py ? (px < py ? -1 : 1) : claim_by_conf(a, b);
}

int cmd_hop(int argc, char **argv){
    const char *conninfo = laplace_db(); int limit = 24, fan = 4096, a = 1; double k = 2.0;
    for (; a < argc - 1 && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--fan") && a + 1 < argc) fan = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--k") && a + 1 < argc) k = atof(argv[++a]);
    }
    if (a >= argc || (argc - a != 1 && argc - a != 3)) {
        fprintf(stderr, "usage: laplace hop [-d conninfo] [-n N] [--fan K] [--k K] text\n"
                        "       laplace hop [...] first middle last             with ? for a part left open: laplace hop was UPOS ?\n"); return 2; }
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0); PGconn *pg = db_connect(conninfo);
    int n, capped, whole = argc - a == 1; Claim *cl; lp_ref e; double t;
    if (whole) { e = named(c, argv[a], NULL, 0, NULL); show_ref("entity", &e); t = now(); cl = claims_of(pg, &e.id, fan, k, &n, &capped); }
    else {
        lp_id part[3]; int have[3];
        for (int i = 0; i < 3; i++) { have[i] = strcmp(argv[a + i], "?") ? 2 : 0; if (have[i]) { lp_ref r = named(c, argv[a + i], NULL, 0, NULL); part[i] = r.id; show_ref(i == 0 ? "subject" : i == 1 ? "predicate" : "object", &r); } }
        if (!have[0] && !have[1] && !have[2]) { fprintf(stderr, "every part is open\n"); return 2; }
        t = now(); cl = claims_like(pg, part, have, fan, k, &n, &capped);
    }
    double t_claims = (now() - t) * 1000;
    positions_of(pg, cl, n); if (!whole) qsort(cl, (size_t)n, sizeof(Claim), claim_by_position);
    Reader *rd = reader_new(pg);
    for (int i = 0; i < n && i < limit; i++) for (int p = 0; p < cl[i].np; p++) reader_want(rd, &cl[i].part[p]);
    printf("\nattested: %d claim%s%s\n", n, n == 1 ? "" : "s", capped ? " (more exist: raise --fan)" : "");
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
    t = now(); uint8_t ab[40]; size_t al = uuid_param(ab, &e.id, 1);
    const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
    PGresult *q = PQexecParams(pg, "SELECT p.tier FROM physicality p WHERE p.path @> $1::uuid[] "
                                   "AND NOT EXISTS (SELECT 1 FROM standing s WHERE s.claim = p.entity)", 1, NULL, v, l, f, 1);
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

/* ---- translation through the ILI: a word bubbles up to its concepts and down into another language, two lookups
 *   laplace translate [-d conninfo] [-n N] word from to...        laplace translate dog eng deu fra jpn */
int cmd_translate(int argc, char **argv){
    const char *conninfo = laplace_db(); int limit = 4, fan = 4096, a = 1; double k = 2.0;
    for (; a < argc - 1 && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = atoi(argv[++a]);
    }
    if (argc - a < 3) { fprintf(stderr, "usage: laplace translate [-d conninfo] [-n concepts] word from to...\n"); return 2; }
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0); PGconn *pg = db_connect(conninfo); Reader *rd = reader_new(pg);
    lp_id word = named(c, argv[a], NULL, 0, NULL).id, from = named(c, argv[a + 1], NULL, 0, NULL).id, def = named(c, "definition", NULL, 0, NULL).id;
    int n, capped; lp_id up[3] = { word, from, word }; int hu[3] = { 2, 2, 0 };
    double t = now(); Claim *senses = claims_like(pg, up, hu, fan, k, &n, &capped); positions_of(pg, senses, n);
    qsort(senses, (size_t)n, sizeof(Claim), claim_by_position); double t_up = (now() - t) * 1000, t_down = 0;
    printf("%s, %s: %d concept%s, as given and then by standing (%.1f ms)\n", argv[a], argv[a + 1], n, n == 1 ? "" : "s", t_up);
    for (int i = 0; i < n && i < limit; i++) {
        lp_id ili = senses[i].part[2]; char *it = reader_text(rd, &ili, 24);
        int nd, cd; lp_id dp[3] = { ili, def, ili }; int hd[3] = { 2, 2, 0 }; Claim *d = claims_like(pg, dp, hd, 64, k, &nd, &cd);
        char *dt = nd ? reader_text(rd, &d[0].part[2], 96) : strdup("");
        printf("\n  %-10s %4.0f \xC2\xB1 %-3.0f  %s\n", it, senses[i].r.rating, senses[i].r.deviation, dt); free(it); free(dt); free(d);
        for (int g = a + 2; g < argc; g++) {
            lp_id lang = named(c, argv[g], NULL, 0, NULL).id, dn[3] = { ili, lang, ili }; int hn[3] = { 0, 2, 2 }, nl, cl2;
            t = now(); Claim *lex = claims_like(pg, dn, hn, fan, k, &nl, &cl2); t_down += (now() - t) * 1000;
            printf("    %-6s", argv[g]);
            for (int j = 0; j < nl && j < 8; j++) { char *w = reader_text(rd, &lex[j].part[0], 40); printf("%s%s %.0f\xC2\xB1%.0f", j ? ", " : " ", w, lex[j].r.rating, lex[j].r.deviation); free(w); }
            if (!nl) printf(" (nothing attested)");
            printf("\n"); free(lex);
        }
    }
    printf("\nup %.1f ms   down %.1f ms   total %.1f ms\n", t_up, t_down, (now() - T) * 1000);
    free(senses); reader_free(rd); PQfinish(pg);
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
static int expand(PGconn *pg, Side *sd, int batch, int fan, int hops, double k, double per_hop, lp_reached *closed, uint8_t *hub, Work *w){
    int n = 0; const lp_reached *x;
    while (n < batch && (x = lp_frontier_next(sd->f))) { hub[n] = 0; closed[n++] = *x; }
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)(n ? n : 1)); int *who = malloc(sizeof(int) * (size_t)(n ? n : 1)), m = 0;
    for (int i = 0; i < n; i++) if ((int)closed[i].hops < hops) { ids[m] = closed[i].id; who[m++] = i; }
    if (!m) { free(ids); free(who); return n; }
    uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = uuid_param(ab, ids, (uint32_t)m); char lim[16]; snprintf(lim, sizeof lim, "%d", fan + 1);
    const char *v[2] = { (const char *)ab, lim }; int l[2] = { (int)al, 0 }, f[2] = { 1, 0 };
    PGresult *q = PQexecParams(pg,
        "SELECT u.i, c.entity, c.path, c.rating, c.deviation, c.volatility FROM unnest($1::uuid[]) WITH ORDINALITY AS u(id, i) "
        "CROSS JOIN LATERAL (SELECT p.entity, p.path, s.rating, s.deviation, s.volatility FROM physicality p JOIN standing s ON s.claim = p.entity "
        "WHERE p.path @> ARRAY[u.id] LIMIT $2::int) c", 2, NULL, v, l, f, 1);
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
        lp_frontier_reach(sd->f, other, &at->id, &claim, at->cost + lp_cost(&r, k, per_hop), 0, at->hops + 1);
    }
    for (int e = 0; e < m; e++) if (held[e] > fan && memcmp(&ids[e], &sd->origin, 16)) { w->hubs++; hub[who[e]] = 1; }
    PQclear(q); free(ab); free(ids); free(who); free(held);
    return n;
}

/* The tie between two neighbours of a chain, as text: the claim's predicate, read from its subject to its object. */
static void show_tie(PGconn *pg, Reader *rd, const lp_id *claim, const lp_id *left){
    uint8_t ab[40]; size_t al = uuid_param(ab, claim, 1); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, fm[1] = { 1 };
    PGresult *q = PQexecParams(pg, "SELECT path FROM physicality WHERE entity = ANY($1::uuid[]) LIMIT 1", 1, NULL, v, l, fm, 1);
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
    const char *conninfo = laplace_db(); int limit = 24, fan = 512, hops = 8, batch = 64, a = 1; double k = 2.0, per_hop = 0.05;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) limit = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--fan") && a + 1 < argc) fan = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--hops") && a + 1 < argc) hops = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--batch") && a + 1 < argc) batch = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--k") && a + 1 < argc) k = atof(argv[++a]);
        else if (!strcmp(argv[a], "--per-hop") && a + 1 < argc) per_hop = atof(argv[++a]);
        else break;
    }
    if (a >= argc || batch < 1) { fprintf(stderr, "usage: laplace degrees [-d conninfo] [-n N] [--hops H] [--fan K] [--batch B] [--k K] [--per-hop C] from [to]\n"); return 2; }
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0);
    lp_ref from = named(c, argv[a], NULL, 0, NULL), to; int goal = a + 1 < argc;
    show_ref("from", &from); if (goal) { to = named(c, argv[a + 1], NULL, 0, NULL); show_ref("to", &to); }
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
            int n = expand(pg, &fw, batch, fan, hops, k, per_hop, closed, hub, &w); if (!n) break;
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
            int n = expand(pg, sd, batch, fan, (hops + 1) / 2, k, per_hop, closed, hub, &w); if (!n) break;
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
