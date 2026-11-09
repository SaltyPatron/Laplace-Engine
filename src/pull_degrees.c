/* ---- degrees of separation and firmware chain traversal */
#include "query.h"
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
static int expand(PGconn *pg, Side *sd, int batch, int fan, int hops, double k, double per_hop, lp_reached *closed, uint8_t *hub, Work *w, const Firmware *way, const lp_id *wids){
    int n = 0; const lp_reached *x;
    while (n < batch && (x = lp_frontier_next(sd->f))) { hub[n] = 0; closed[n++] = *x; }
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)(n ? n : 1)); int *who = malloc(sizeof(int) * (size_t)(n ? n : 1)), m = 0;
    for (int i = 0; i < n; i++) if ((int)closed[i].hops < hops) { ids[m] = closed[i].id; who[m++] = i; }
    if (!m) { free(ids); free(who); return n; }
    uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = ids_param(pg,ab, ids, (uint32_t)m); char lim[16]; snprintf(lim, sizeof lim, "%d", fan + 1);
    int rl; const char *v[4] = { (const char *)ab, lim, CLAIM_BITS, refuse_param(pg,&rl) }; int l[4] = { (int)al, 0, 0, rl }, f[4] = { 1, 0, 0, 1 };
    PGresult *q = db_ask(pg,
        "SELECT i, entity, path, rating, deviation, volatility FROM @extschema@.laplace_claims_each($1::@extschema@.blake3[], $2::bigint, $3::smallint[], $4::@extschema@.blake3[])", 4, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "claims: %s", PQerrorMessage(pg)); exit(1); }
    w->trips++; w->expanded += (uint64_t)m;
    int rows = PQntuples(q), *held = calloc((size_t)m, sizeof(int));
    for (int j = 0; j < rows; j++) { uint64_t o; memcpy(&o, PQgetvalue(q, j, 0), 8); held[__builtin_bswap64(o) - 1]++; }
    for (int j = 0; j < rows; j++) {
        uint64_t o; memcpy(&o, PQgetvalue(q, j, 0), 8); int e = (int)(__builtin_bswap64(o) - 1); const lp_reached *at = &closed[who[e]];
        if (held[e] > fan) continue;                                                       /* a hub: reached, not crossed (the origin is read below) */
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
    /* The origin is crossed even when it is a hub: a search from a part of speech has to leave it. What the index gave
     * past the fan is whatever its scan met first, so the origin is read whole and the fan keeps its hardest pulling,
     * the same set on every install of the same content (claims_of: in content order, by confidence). */
    for (int e = 0; e < m; e++) if (held[e] > fan && !memcmp(&ids[e], &sd->origin, 16)) {
        const lp_reached *at = &closed[who[e]]; int nc, capped; Claim *cl = claims_of(pg, &ids[e], fan, k, &nc, &capped);
        for (int j = 0; j < nc; j++) {
            if (cl[j].np != 3 || middle_refused(cl[j].part, cl[j].np)) continue;
            const lp_id *part = cl[j].part, *other;
            if (!memcmp(&part[0], &at->id, 16)) other = &part[2]; else if (!memcmp(&part[2], &at->id, 16)) other = &part[0]; else continue;
            if (!memcmp(other, &at->id, 16)) continue;
            w->claims++; double sw = strand_weight(way, wids, part, 3); if (sw <= 0) continue;
            lp_frontier_reach(sd->f, other, &at->id, &cl[j].id, at->cost + lp_cost(&cl[j].r, k, per_hop) - log(sw), 0, at->hops + 1);
        }
        free(cl);
    }
    PQclear(q); free(ab); free(ids); free(who); free(held);
    return n;
}

/* The tie between two neighbours of a chain, as text: the claim's predicate, read from its subject to its object. */
static void show_tie(PGconn *pg, Reader *rd, const lp_id *claim, const lp_id *left){
    uint8_t ab[40]; size_t al = ids_param(pg,ab, claim, 1); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, fm[1] = { 1 };
    PGresult *q = db_ask(pg, "SELECT path FROM @extschema@.laplace_paths($1::@extschema@.blake3[]) LIMIT 1", 1, v, l, fm);
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

/* ---- what the forward pass reads through the firmware, one way for every command */
/* How hard each word in a set pulls (its role): what is attested of it under the firmware's role kind ([word, value]
 * for a highway bank, or [word, role_by, x] in the claims' order), the weight the firmware gives the strongest x it
 * names. -1 when nothing it names is attested. All words in the batch are resolved together in at most two round trips. */
void roles_of(PGconn *pg, Firmware *fw, const lp_id *words, int n, double *out, uint64_t *trips){
    for (int i = 0; i < n; i++) out[i] = -1.0;
    if (!n || !fw->role_by[0]) return;
    firmware_ids(fw);
    if (!fw->nrole) return;
    char bank_name[96]; size_t b = 0;
    for (; fw->role_by[b] && b + 1 < sizeof bank_name; b++) {
        unsigned char ch = (unsigned char)fw->role_by[b]; bank_name[b] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 'a' - 'A') : (char)ch;
    }
    bank_name[b] = 0; const lp_highway *highway = lp_highway_map(NULL);
    const lp_bank *bank = highway ? lp_highway_bank(highway, bank_name) : NULL;
    if (bank) {
        uint8_t *wb = malloc(20 + 20 * (size_t)n), *rb = malloc(20 + 20 * (size_t)fw->nrole);
        size_t wl = ids_param(pg, wb, words, (uint32_t)n), rl_role = ids_param(pg, rb, fw->id.role, (uint32_t)fw->nrole);
        int rl_ref = 0; const char *ref_p = refuse_param(pg, &rl_ref);
        const char *v[4] = { (const char *)wb, CLAIM_BITS, ref_p, (const char *)rb };
        int l[4] = { (int)wl, 0, rl_ref, (int)rl_role }, f[4] = { 1, 0, 1, 1 };
        PGresult *q = db_ask(pg,
            "SELECT i, path, rating, deviation, volatility FROM @extschema@.laplace_claims_each("
            "$1::@extschema@.blake3[], NULL::bigint, $2::smallint[], $3::@extschema@.blake3[], "
            "1::smallint, NULL::@extschema@.blake3[], 2::bigint, $4::@extschema@.blake3[])", 4, v, l, f);
        if (trips) (*trips)++;
        if (PQresultStatus(q) == PGRES_TUPLES_OK) {
            double *best = malloc(sizeof(double) * (size_t)n);
            int *best_z = malloc(sizeof(int) * (size_t)n);
            for (int i = 0; i < n; i++) { best[i] = -INFINITY; best_z[i] = fw->nrole; }
            int rows = PQntuples(q);
            for (int r = 0; r < rows; r++) {
                uint64_t o; memcpy(&o, PQgetvalue(q, r, 0), 8);
                int idx = (int)(__builtin_bswap64(o) - 1);
                if (idx < 0 || idx >= n) continue;
                lp_id pair[3]; size_t np = lp_path_ids((const uint8_t *)PQgetvalue(q, r, 1), (size_t)PQgetlength(q, r, 1), pair, 3);
                if (np != 2 || !lp_id_eq(&pair[0], &words[idx]) || lp_highway_slot(highway, bank->list, &pair[1]) < 0) continue;
                lp_rating rt = { lp_be_f64(PQgetvalue(q, r, 2)), lp_be_f64(PQgetvalue(q, r, 3)), lp_be_f64(PQgetvalue(q, r, 4)) };
                double conf = lp_confidence(&rt, fw->k);
                for (int z = 0; z < fw->nrole; z++) if (lp_id_eq(&pair[1], &fw->id.role[z])
                        && (conf > best[idx] || (conf == best[idx] && z < best_z[idx]))) {
                    best[idx] = conf; best_z[idx] = z; out[idx] = fw->role[z];
                }
            }
            free(best); free(best_z);
        }
        PQclear(q); free(wb); free(rb);
    }
    lp_id *rem = malloc(sizeof(lp_id) * (size_t)n); int *rem_idx = malloc(sizeof(int) * (size_t)n), nrem = 0;
    for (int i = 0; i < n; i++) if (out[i] < 0) { rem[nrem] = words[i]; rem_idx[nrem++] = i; }
    if (nrem) {
        int nc = 0, *src = NULL;
        Claim *cl = claims_each(pg, rem, nrem, &fw->id.role_by, CLAIM_FIRST, fw->fan, fw->k, &nc, &src);
        if (trips) (*trips)++;
        for (int q = 0; q < nc; q++) {
            int i = rem_idx[src[q]]; if (out[i] >= 0) continue;
            for (int z = 0; z < fw->nrole; z++) if (lp_id_eq(&cl[q].part[cl[q].np - 1], &fw->id.role[z])) { out[i] = fw->role[z]; break; }
        }
        free(cl); free(src);
    }
    free(rem); free(rem_idx);
}
double role_of(PGconn *pg, Firmware *fw, const lp_id *word){
    double w = -1.0; roles_of(pg, fw, word, 1, &w, NULL); return w;
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
    for (*alt = 0; *alt < fw->nalt; (*alt)++) {
        lp_id cur = *word; steps = 0;
        for (int z = 0; z < fw->nchain[*alt]; z++) {
            Ids candidates={0};lp_push(&candidates,cur);
            if(z>0 && lp_tier0_codepoint(T0,&cur)<0){
                size_t count=reader_parts(rd,&cur,NULL,0);Ids parts={0};lp_vec_reserve(&parts,count);
                reader_parts(rd,&cur,parts.v,count);
                for(size_t i=count;i-- >0;)if(lp_tier0_codepoint(T0,&parts.v[i])<0 && !lp_id_eq(&parts.v[i],word))lp_push(&candidates,parts.v[i]);
                lp_vec_free(&parts);lp_push(&candidates,*word);
            }
            if(candidates.n>INT_MAX){fputs("chain candidates exceed the reader's addressable range\n",stderr);exit(1);}
            int n=0,*source=NULL;
            Claim *cl=claims_each(pg,candidates.v,(int)candidates.n,&fw->id.chain[*alt][z],CLAIM_FIRST,fw->fan,fw->k,&n,&source);
            lp_vec_free(&candidates);
            /* The source order remains the firmware's priority. Read all
             * candidates together, then retain the first supported subject. */
            if(n){int first=source[0],keep=0;while(keep<n && source[keep]==first)keep++;n=keep;}
            free(source);
            if (!n){free(cl);break;}
            positions_of(pg, cl, n); if (fw->order_witness) lp_sort(cl, (size_t)n, sizeof(Claim), claim_by_position);
            int take = -1;
            if (z == 0 && reading) for (int k = 0; k < n; k++) if (lp_id_eq(&cl[k].part[cl[k].np - 1], reading)) { take = k; break; }
            if (take < 0 && fork && n > 1) take = fork(fork_ctx, cl, n);        /* what the context supports, when it tells the branches apart */
            if (take < 0) { take_top(cl, n, 1, fw, seed); take = 0; }
            *last = (ClaimStanding){cl[take].r,cl[take].matches}; cur = cl[take].part[cl[take].np - 1]; steps++; free(cl);
        }
        if (steps == fw->nchain[*alt]) { *answer = cur; return steps; }
    }
    *alt = -1; return steps;
}
