/* ---- degrees of separation and firmware chain traversal */
#include "query.h"
#include "laplace/highway.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

/* Best-first over claims, a batch of the nearest open entities per round trip: each entity in the batch gets its claims
 * through the container index, read at most K (--fan) of them, which is what keeps a step O(K). An entity that holds
 * more than K claims (a part of speech, a language) is reached and not read through. Between two entities the search
 * runs from both ends and stops when no open strand can beat the best chain found. */
typedef struct { lp_frontier *f; lp_id origin; } Side;
typedef struct { uint64_t trips, expanded, claims, hubs; } Work;

static int decode_claim(const char *path, int len, lp_id part[3]){ return (int)lp_path_ids((const uint8_t *)path, (size_t)len, part, 3); }

/* Close up to batch of the side's nearest open entities and reach across their claims. Returns how many it closed;
 * closed[] receives them. */
static int expand(PGconn *pg, Side *sd, int batch, int fan, int hops, double k, double per_hop, const lp_coord *goal_coord, lp_reached *closed, uint8_t *hub, Work *w, const Firmware *way, const lp_id *wids){
    int n = 0; const lp_reached *x;
    while (n < batch && (x = lp_frontier_next(sd->f))) { hub[n] = 0; closed[n++] = *x; }
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)(n ? n : 1)); int *who = malloc(sizeof(int) * (size_t)(n ? n : 1)), m = 0;
    for (int i = 0; i < n; i++) if ((int)closed[i].hops < hops) { ids[m] = closed[i].id; who[m++] = i; }
    if (!m) { free(ids); free(who); return n; }
    Args ca = { 0 }; int16_t claim_bit = 0;
    arg_ids(&ca, ids, (size_t)m);
    arg_i64(&ca, (int64_t)(fan + 1));
    arg_i16_array(&ca, &claim_bit, 1);
    arg_refused(&ca);
    char err[512]; PGresult *q = db_call_try(pg, LP_SQL_CLAIMS_EACH, &ca, err, sizeof err);
    args_free(&ca);
    if (!q) { fprintf(stderr, "claims: %s\n", err); exit(1); }
    w->trips++; w->expanded += (uint64_t)m;
    int rows = PQntuples(q), *held = calloc((size_t)m, sizeof(int));
    typedef struct { lp_id target, from, claim; double g; uint32_t hops; } PendingEdge;
    PendingEdge *edges = malloc(sizeof(PendingEdge) * (size_t)(rows ? rows : 1));
    int nedges = 0;
    for (int j = 0; j < rows; j++) { held[col_i64(q, j, 0) - 1]++; }
    for (int j = 0; j < rows; j++) {
        int e = (int)(col_i64(q, j, 0) - 1); const lp_reached *at = &closed[who[e]];
        if (held[e] > fan) continue;                                                       /* a hub: reached, not crossed (the origin is read below) */
        lp_id part[64], claim; int npart = (int)lp_path_ids((const uint8_t *)PQgetvalue(q, j, 2), (size_t)PQgetlength(q, j, 2), part, 64);
        if (npart < 2 || npart > 64) continue;
        int other_idx = lp_tuple_other(part, (size_t)npart, &at->id);
        if (other_idx < 0) continue;
        const lp_id *other = &part[other_idx];
        if (!memcmp(other, &at->id, 16)) continue;
        lp_rating r = { col_f64(q, j, 3), col_f64(q, j, 4), col_f64(q, j, 5) };
        claim = *col_id(q, j, 1); w->claims++;
        double sw = strand_weight(way, wids, part, npart); if (sw <= 0) continue;           /* a kind weighed at nothing is not crossed */
        edges[nedges++] = (PendingEdge){ *other, at->id, claim, at->cost + lp_cost(&r, k, per_hop) - log(sw), at->hops + 1 };
    }
    for (int e = 0; e < m; e++) if (held[e] > fan && memcmp(&ids[e], &sd->origin, 16)) { w->hubs++; hub[who[e]] = 1; }
    /* The origin is crossed even when it is a hub: a search from a part of speech has to leave it. What the index gave
     * past the fan is whatever its scan met first, so the origin is read whole and the fan keeps its hardest pulling,
     * the same set on every install of the same content (claims_of: in content order, by confidence). */
    for (int e = 0; e < m; e++) if (held[e] > fan && !memcmp(&ids[e], &sd->origin, 16)) {
        const lp_reached *at = &closed[who[e]]; int nc, capped; Claim *cl = claims_of(pg, &ids[e], fan, k, &nc, &capped);
        if (nc > 0) edges = xrealloc(edges, sizeof(PendingEdge) * (size_t)(nedges + nc));
        for (int j = 0; j < nc; j++) {
            if (cl[j].np < 2 || middle_refused(cl[j].part, cl[j].np)) continue;
            const lp_id *part = cl[j].part;
            int other_idx = lp_tuple_other(part, (size_t)cl[j].np, &at->id);
            if (other_idx < 0) continue;
            const lp_id *other = &part[other_idx];
            if (!memcmp(other, &at->id, 16)) continue;
            w->claims++; double sw = strand_weight(way, wids, part, cl[j].np); if (sw <= 0) continue;
            edges[nedges++] = (PendingEdge){ *other, at->id, cl[j].id, at->cost + lp_cost(&cl[j].r, k, per_hop) - log(sw), at->hops + 1 };
        }
        free(cl);
    }
    if (nedges > 0) {
        double g4[4] = { 0 };
        double *pts = NULL; uint8_t *has = NULL;
        if (goal_coord) {
            lp_coord_xyzm(goal_coord, g4);
            lp_id *tids = malloc(sizeof(lp_id) * (size_t)nedges);
            pts = malloc(sizeof(double) * 4 * (size_t)nedges);
            has = malloc((size_t)nedges);
            for (int i = 0; i < nedges; i++) tids[i] = edges[i].target;
            coords_of(pg, tids, nedges, pts, has);
            free(tids);
        }
        for (int i = 0; i < nedges; i++) {
            double h_est = 0.0;
            if (goal_coord && has && has[i] && per_hop > 0.0) {
                double ang = lp_angle4(pts + 4 * (size_t)i, g4);
                if (isfinite(ang) && ang > 0.0) {
                    if (ang > 3.14159265358979323846) ang = 3.14159265358979323846;
                    h_est = 0.5 * per_hop * (ang / 3.14159265358979323846);
                }
            }
            lp_frontier_reach(sd->f, &edges[i].target, &edges[i].from, &edges[i].claim, edges[i].g, h_est, edges[i].hops);
        }
        free(pts); free(has);
    }
    free(edges); PQclear(q); free(ids); free(who); free(held);
    return n;
}

/* The tie between two neighbours of a chain, as text: the claim's predicate, read from its subject to its object. */
static void show_tie(PGconn *pg, Reader *rd, const lp_id *claim, const lp_id *left){
    Args ca = { 0 }; arg_ids(&ca, claim, 1);
    char err[512]; PGresult *q = db_call_try(pg, LP_SQL_PATH_FIRST, &ca, err, sizeof err);
    args_free(&ca);
    lp_id part[3]; int np = q && PQntuples(q) ? decode_claim(PQgetvalue(q, 0, 0), PQgetlength(q, 0, 0), part) : 0;
    if (q) PQclear(q);
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
    PGconn *pg = db_read(conninfo); Reader *rd = reader_new(pg); Work w = { 0 };
    lp_reached *closed = malloc(sizeof(lp_reached) * (size_t)batch); uint8_t *hub = malloc((size_t)batch);
    Side fw = { lp_frontier_new(), from.id }; lp_frontier_reach(fw.f, &from.id, NULL, NULL, 0, 0, 0);

    if (!goal) {
        lp_reached *near = NULL; size_t nn = 0, cn = 0;
        for (;;) {
            if (nn >= (size_t)limit) {                                        /* the N nearest are known once nothing open is nearer */
                qsort(near, nn, sizeof *near, by_cost);
                if (lp_frontier_least(fw.f) >= near[limit - 1].cost) break;
            }
            int n = expand(pg, &fw, batch, fan, hops, k, per_hop, NULL, closed, hub, &w, &way, wids); if (!n) break;
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
            const lp_coord *goal_c = sd == &fw ? &to.c : &from.c;
            int n = expand(pg, sd, batch, fan, (hops + 1) / 2, k, per_hop, goal_c, closed, hub, &w, &way, wids); if (!n) break;
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

/* ---- what the forward pass reads through the firmware, one way for every command */
/* How hard each word of a sequence pulls, by what it is where it stands (Forward 20.3 ORIENT: "Content obligations come
 * from an occurrence's own evidence"). A word's reading under the firmware's role kind is what the observations of the
 * sequence's run say at the word's place: the observations that hold the word and the word after it in the sequence
 * (their junction, read by intersection however common either word is; a junction more than the fan hold is reached,
 * not crossed), and, in each record that holds such an observation, a sibling layer of the same length at the same
 * place: a treebank lays its parts of speech along its words that way. A value of the role kind's bank there is one
 * reading; the reading most of them give is the word's, and the firmware's weight for it is how hard the word pulls.
 * What a word is said to be with no place (an exception list's noun, a tag claimed of the word) takes no part.
 * out[i] is -1 where no observation of the run gives one reading, or the firmware names no weight for it: the word
 * stays an obligation, at the weight its caller gives a word nothing says. reading/votes/seen (each may be NULL): the
 * reading taken, the observations that give it, and the observations that give any. Three round trips a sequence. */
typedef struct { size_t obs; int pos, occ; } RoleSpot;
typedef struct { size_t obs, sib; } RoleLink;
typedef struct { lp_id v; int n; } RoleTally;
static int spot_order(const void *a, const void *b){
    const RoleSpot *x = a, *y = b;
    if (x->occ != y->occ) return x->occ < y->occ ? -1 : 1;
    if (x->obs != y->obs) return x->obs < y->obs ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}
static int link_order(const void *a, const void *b){ const RoleLink *x = a, *y = b; return x->obs < y->obs ? -1 : x->obs > y->obs; }
static int is_word(const lp_id *id, const lp_layout *fl, const lp_field *alpha, const lp_field *gc, int32_t nd){
    int64_t cp = lp_tier0_codepoint(T0, id); if (cp < 0) return 1;            /* a composition is a word; a letter or digit is one */
    return (alpha && lp_flags_get(fl, (uint32_t)cp, alpha)) || (gc && nd >= 0 && lp_flags_get(fl, (uint32_t)cp, gc) == (uint32_t)nd);
}
void roles_of(PGconn *pg, Firmware *fw, const lp_id *seq, const uint8_t *word, int n, double *out, lp_id *reading, int *votes, int *seen, uint64_t *trips){
    for (int i = 0; i < n; i++) { out[i] = -1.0; if (reading) memset(&reading[i], 0, sizeof(lp_id)); if (votes) votes[i] = 0; if (seen) seen[i] = 0; }
    if (!n || !fw->role_by[0]) return;
    firmware_ids(fw);
    char bank_name[96]; size_t b = 0;
    for (; fw->role_by[b] && b + 1 < sizeof bank_name; b++) {
        unsigned char ch = (unsigned char)fw->role_by[b]; bank_name[b] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 'a' - 'A') : (char)ch;
    }
    bank_name[b] = 0; const lp_highway *highway = lp_highway_map(NULL);
    const lp_bank *bank = highway ? lp_highway_bank(highway, bank_name) : NULL;
    if (!bank) return;                                                        /* a role kind no bank types: nothing can be read as one of its values */
    const lp_layout *fl = lp_flags_map(NULL); const lp_field *alpha = fl ? lp_flags_field(fl, "Alphabetic") : NULL, *gc = fl ? lp_flags_field(fl, "General_Category") : NULL;
    int32_t nd = gc ? lp_flags_value(fl, gc, "Nd") : -1;
    int *at = malloc(sizeof(int) * (size_t)n), nw = 0; for (int i = 0; i < n; i++) if (word[i]) at[nw++] = i;
    if (!nw) { free(at); return; }
    /* the junctions: each word with the word after it; a word alone is its own */
    int np = nw > 1 ? nw - 1 : 1; lp_id *left = malloc(sizeof(lp_id) * (size_t)np), *right = malloc(sizeof(lp_id) * (size_t)np);
    for (int k = 0; k < np; k++) { left[k] = seq[at[k]]; right[k] = seq[at[nw > 1 ? k + 1 : 0]]; }
    Args q = { 0 }; arg_ids(&q, left, (size_t)np); arg_ids(&q, right, (size_t)np); arg_i64(&q, (int64_t)fw->fan);
    PGresult *r1 = db_call(pg, LP_SQL_ROLE_JUNCTIONS, &q);
    if (trips) (*trips)++;
    lp_idmap *obs = lp_idmap_new(); lp_vec(Run) runs = { 0 }; lp_vec(RoleSpot) spots = { 0 };
    for (int row = 0; row < PQntuples(r1); row++) {
        int k = (int)col_i64(r1, row, 0) - 1; if (k < 0 || k >= np) continue;
        bool fresh; size_t oi = lp_idmap_put(obs, col_id(r1, row, 1), &fresh);
        if (fresh) lp_push(&runs, run_of((const uint8_t *)PQgetvalue(r1, row, 2), (size_t)PQgetlength(r1, row, 2)));
        const Run *rn = &runs.v[oi];
        for (int p = 0; p < rn->n; p++) {
            if (!lp_id_eq(&rn->id[p], &left[k])) continue;
            if (nw == 1) { lp_push(&spots, (RoleSpot){ oi, p, at[0] }); continue; }
            int nx = p + 1; while (nx < rn->n && !is_word(&rn->id[nx], fl, alpha, gc, nd)) nx++;   /* the next word, past a space or a mark */
            if (nx < rn->n && lp_id_eq(&rn->id[nx], &right[k])) { lp_push(&spots, (RoleSpot){ oi, p, at[k] }); lp_push(&spots, (RoleSpot){ oi, nx, at[k + 1] }); }
        }
    }
    PQclear(r1); args_free(&q);
    /* the records that hold those observations (a record says claims of its own, so either kind), and the other layers they hold */
    lp_vec(RoleLink) links = { 0 }; lp_idmap *sib = lp_idmap_new(); size_t nobs = lp_idmap_count(obs);
    if (spots.n) {
        for (size_t begin = 0; begin < nobs; ) {
            size_t amount = nobs - begin; if (amount > DB_ID_WINDOW) amount = DB_ID_WINDOW;
            Args q2 = { 0 }; arg_ids(&q2, lp_idmap_keys(obs) + begin, amount); arg_i64(&q2, (int64_t)fw->fan);
            PGresult *r2 = db_call(pg, LP_SQL_ROLE_HOLDERS, &q2);
            if (trips) (*trips)++;
            for (int row = 0; row < PQntuples(r2); row++) {
                size_t oi = begin + (size_t)col_i64(r2, row, 0) - 1; if (oi >= nobs) continue;
                Run rec = run_of((const uint8_t *)PQgetvalue(r2, row, 1), (size_t)PQgetlength(r2, row, 1)); lp_idmap *once = lp_idmap_new();
                for (int c = 0; c < rec.n; c++) {
                    if (lp_id_eq(&rec.id[c], lp_idmap_key(obs, oi))) continue;
                    bool fresh; lp_idmap_put(once, &rec.id[c], &fresh); if (!fresh) continue;
                    lp_push(&links, (RoleLink){ oi, lp_idmap_put(sib, &rec.id[c], &fresh) });
                }
                lp_idmap_free(once); free(rec.id);
            }
            PQclear(r2); args_free(&q2); begin += amount;
        }
    }
    size_t nsib = lp_idmap_count(sib); Run *sruns = calloc(nsib ? nsib : 1, sizeof(Run));
    if (nsib) {
        int nh = 0; Hold *h = paths_of(pg, lp_idmap_keys(sib), (int)nsib, &nh); if (trips) (*trips)++;
        for (int i = 0; i < nh; i++) if (h[i].src >= 0 && (size_t)h[i].src < nsib && !sruns[h[i].src].id) sruns[h[i].src] = run_of(h[i].path, (size_t)h[i].path_len);
        holds_free(h, nh);
    }
    /* each observation of the run, at the word's place, in each layer laid along it: one reading */
    if (spots.n > 1) qsort(spots.v, spots.n, sizeof(RoleSpot), spot_order);
    if (links.n > 1) qsort(links.v, links.n, sizeof(RoleLink), link_order);
    RoleTally tally[16]; int nt = 0, occ = -1;
    for (size_t s = 0; s <= spots.n; s++) {
        if (s == spots.n || spots.v[s].occ != occ) {                          /* a word's readings are all in: take the one most give */
            if (occ >= 0 && nt) { int best = 0, tied = 0, all = 0;
                for (int t = 0; t < nt; t++) { all += tally[t].n; if (tally[t].n > tally[best].n) { best = t; tied = 0; } else if (t != best && tally[t].n == tally[best].n) tied = 1; }
                if (seen) seen[occ] = all;
                if (!tied) { if (reading) reading[occ] = tally[best].v; if (votes) votes[occ] = tally[best].n;
                    for (int z = 0; z < fw->nrole; z++) if (lp_id_eq(&tally[best].v, &fw->id.role[z])) { out[occ] = fw->role[z]; break; } } }
            if (s == spots.n) break;
            occ = spots.v[s].occ; nt = 0;
        }
        if (s > 0 && !spot_order(&spots.v[s], &spots.v[s - 1])) continue;    /* one place of one observation, reached from both its junctions */
        const RoleSpot *sp = &spots.v[s]; const Run *o = &runs.v[sp->obs];
        size_t lo = 0, hi = links.n; while (lo < hi) { size_t mid = (lo + hi) / 2; if (links.v[mid].obs < sp->obs) lo = mid + 1; else hi = mid; }
        for (size_t l = lo; l < links.n && links.v[l].obs == sp->obs; l++) {
            const Run *layer = &sruns[links.v[l].sib]; if (!layer->id || layer->n != o->n) continue;
            const lp_id *v = &layer->id[sp->pos]; if (lp_highway_slot(highway, bank->list, v) < 0) continue;
            int t = 0; while (t < nt && !lp_id_eq(&tally[t].v, v)) t++;
            if (t == nt) { if (nt == 16) continue; tally[nt++] = (RoleTally){ *v, 0 }; }
            tally[t].n++;
        }
    }
    for (size_t i = 0; i < runs.n; i++) free(runs.v[i].id); lp_vec_free(&runs);
    for (size_t i = 0; i < nsib; i++) free(sruns[i].id); free(sruns);
    lp_vec_free(&spots); lp_vec_free(&links); lp_idmap_free(obs); lp_idmap_free(sib); free(left); free(right); free(at);
}
/* The strands of a set a firmware takes: the top n, or, when the firmware says how near a tie has to be, any strand
 * that near the one above it may be taken in its place. */
void take_top(Claim *cl, int n, int want, const Firmware *fw, unsigned *seed){
    if (fw->top_within <= 0 || !seed) return;
    for (int i = 0; i < n && i < want; i++) { int tied = i; while (tied + 1 < n && cl[i].conf - cl[tied + 1].conf <= fw->top_within) tied++;
        if (tied > i) { int pick = i + (int)(rand_r(seed) % (unsigned)(tied - i + 1)); Claim t = cl[i]; cl[i] = cl[pick]; cl[pick] = t; } }
}
/* A chain the firmware names, followed from a word: each relation in turn, the strands that hold where the chain stands
 * (or, past the first step, one of the things it is made of, the last first, the word itself last) in their witness's
 * order, then by standing. At the first step the strand to the oriented reading is taken where the chain passes
 * through it; otherwise the top, as the firmware takes it (seed NULL: the top every time). The first of the firmware's
 * chains that reaches its end answers. Returns the steps the answering chain took, or, when none reached its end, the
 * most the last one tried took and -1 in *alt; *last is the last strand taken. */
int chain_follow(PGconn *pg, Reader *rd, Firmware *fw, const lp_id *word, const lp_id *reading, unsigned *seed, ChainFork fork, void *fork_ctx, lp_id *answer, ClaimStanding *last, int *alt){
    firmware_ids(fw); int steps = 0;
    const lp_highway *hw = lp_highway_map(NULL);
    const lp_list *lang_list = hw ? lp_highway_list(hw, "language") : NULL;
    for (*alt = 0; *alt < fw->nalt; (*alt)++) {
        lp_id cur = *word; steps = 0;
        for (int z = 0; z < fw->nchain[*alt]; z++) {
            int n = 0, *source = NULL;
            Claim *cl = claims_each(pg, &cur, 1, &fw->id.chain[*alt][z], CLAIM_FIRST, fw->fan, fw->k, &n, &source);
            if (!n && z > 0 && lp_tier0_codepoint(T0, &cur) < 0) {
                free(source); free(cl); cl = NULL; source = NULL;
                Ids candidates = { 0 };
                size_t count = reader_parts(rd, &cur, NULL, 0);
                Ids parts = { 0 };
                lp_vec_reserve(&parts, count);
                reader_parts(rd, &cur, parts.v, count);
                for (size_t i = count; i-- > 0; ) {
                    if (lp_tier0_codepoint(T0, &parts.v[i]) >= 0 || lp_id_eq(&parts.v[i], word)) continue;
                    if (hw && lp_highway_bank_of(hw, &parts.v[i], NULL)) continue;
                    if (lang_list && lp_highway_slot(hw, lang_list, &parts.v[i]) >= 0) continue;
                    lp_push(&candidates, parts.v[i]);
                }
                lp_vec_free(&parts);
                lp_push(&candidates, *word);
                if (candidates.n > INT_MAX) { fputs("chain candidates exceed the reader's addressable range\n", stderr); exit(1); }
                cl = claims_each(pg, candidates.v, (int)candidates.n, &fw->id.chain[*alt][z], CLAIM_FIRST, fw->fan, fw->k, &n, &source);
                lp_vec_free(&candidates);
            }
            /* The source order remains the firmware's priority. Read all
             * candidates together, then retain the first supported subject. */
            if (n) { int first = source[0], keep = 0; while (keep < n && source[keep] == first) keep++; n = keep; }
            free(source);
            if (!n) { free(cl); break; }
            if (n > 1) {
                positions_of(pg, cl, n);
                if (fw->order_witness) lp_sort(cl, (size_t)n, sizeof(Claim), claim_by_position);
            }
            int take = -1;
            if (z == 0 && reading) for (int k = 0; k < n; k++) if (lp_id_eq(&cl[k].part[cl[k].np - 1], reading)) { take = k; break; }
            if (take < 0 && fork && n > 1) take = fork(fork_ctx, cl, n);        /* what the context supports, when it tells the branches apart */
            if (take < 0) { take_top(cl, n, 1, fw, seed); take = 0; }
            *last = (ClaimStanding){ cl[take].r, cl[take].matches }; cur = cl[take].part[cl[take].np - 1]; steps++; free(cl);
        }
        if (steps == fw->nchain[*alt]) { *answer = cur; return steps; }
    }
    *alt = -1; return steps;
}
