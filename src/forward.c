/* The forward pass. A prompt is ingested as text, broken down, and given a trunk ID; what a step has to work from is
 * the observation (the trajectories that hold it), the attestations on that observation (the claims witnessed of the
 * same content), and its whole tree across tiers. A step does not have to emit one token: it takes any segment of
 * any branch, or a combination of them, as the firmware decides (Semantics: Pull, Personality firmware).
 *
 *   laplace pull [-d conninfo] [--firmware FILE] [--seed N] prompt
 *
 * The lookups are the ones hop and fills make: the container index finds what holds an entity, O(log N), and the set
 * is read, O(K). Nothing here changes a standing, and nothing here is a record: the choice is the firmware's. */
#include "engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void show_claim(Reader *rd, const Claim *c, const char *lead){
    printf("%s%10.3f %8.0f %6.0f %8d   [", lead, c->conf, c->r.rating, c->r.deviation, c->matches);
    for (int p = 0; p < c->np; p++) { char *tx = reader_text(rd, &c->part[p], p == c->np - 1 ? 96 : 48); printf("%s%s", p ? ", " : "", tx); free(tx); }
    printf("]\n");
}
/* The segments of a laplace_forward result: rows r0 .. r-1 are one segment [i..j], from r0. */
static int segment_end(const PGresult *q, int r0){
    int nr = PQntuples(q), i = (int)col_int(q, r0, 0), j = (int)col_int(q, r0, 1), r = r0;
    while (r < nr && (int)col_int(q, r, 0) == i && (int)col_int(q, r, 1) == j) r++;
    return r;
}
typedef struct { double pull; int strands; } Tug;
static int by_pull(const void *a, const void *b){ double x = ((const Tug *)a)->pull, y = ((const Tug *)b)->pull; return x < y ? 1 : x > y ? -1 : 0; }
typedef struct { lp_id id; Tug t; } Tugged;
static int by_tugged(const void *a, const void *b){ return by_pull(&((const Tugged *)a)->t, &((const Tugged *)b)->t); }

int cmd_pull(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; long long seedv = -1;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "--firmware", 's', &fwp }, { "--seed", 'l', &seedv }, { NULL } });
    if (a >= argc) { fprintf(stderr, "usage: laplace pull [-d conninfo] [--firmware FILE] [--seed N] prompt\n"); return 2; }
    double T = now(); unsigned seed = seedv >= 0 ? (unsigned)seedv : (unsigned)(T * 1e6);
    Firmware fw = firmware_for(fwp, FW_PULL);
    tier0_open(NULL); table_init(); ctx_open(1); lp_text *c = lp_text_new(T0); firmware_ids(&fw);
    const char *prompt = argv[a];

    /* ---- the prompt: broken down, and its trunk; a word is one constituent of what holds it: itself */
    Ref pr = text_ref(CTX[0], (const uint8_t *)prompt, strlen(prompt));
    lp_id *ph = malloc(sizeof(lp_id) * (strlen(prompt) + 1)); int np = pr.tier <= 2 ? 0 : (int)table_parts(&pr.id, ph, strlen(prompt) + 1);
    if (!np) { ph[0] = pr.id; np = 1; }
    char idt[33]; lp_id_hex(&pr.id, idt);
    firmware_say(&fw, FW_PULL);
    printf("prompt     %s   tier %d   %d constituent%s", idt, pr.tier, np, np == 1 ? "" : "s");
    uint8_t *word = malloc((size_t)np); int words = 0; for (int i = 0; i < np; i++) words += word[i] = table_find(&ph[i]) != NULL;
    printf(", %d of them compositions\n", words);

    PGconn *pg = db_connect(conninfo); Reader *rd = reader_new(pg);
    /* every segment of the prompt at once (laplace_forward): "The", "The dog", "The dog barked" and every other run of it,
     * the observations that hold each, how many as a run, and what follows the run in them, counted. The whole pass is one
     * set; a segment that is only tier-0 atoms (a space, a letter) is a hub and is not shown. */
    { double t = now(); Args sa = { 0 }; arg_ids(&sa, ph, (size_t)np); arg_int(&sa, fw.fan);
      /* the prefixes first, as the pass walks them ("The", "The dog", ...), then every other segment by how many hold it */
      PGresult *q = ask(pg, "SELECT i, j, paths, runs, next, times FROM laplace_forward($1::blake3[], $2::bigint) ORDER BY (i > 1), CASE WHEN i = 1 THEN j END, paths DESC, (j - i) DESC, i, times DESC NULLS LAST", &sa);
      int nr = PQntuples(q), shown = 0, segments = 0;
      for (int r = 0; r < nr; r++) if (!PQgetisnull(q, r, 4)) reader_want(rd, col_id(q, r, 4));
      for (int i = 0; i < np; i++) reader_want(rd, &ph[i]);
      for (int r0 = 0, r; r0 < nr; r0 = r) {
          r = segment_end(q, r0); int i = (int)col_int(q, r0, 0), j = (int)col_int(q, r0, 1); long paths = (long)col_int(q, r0, 2), runs = (long)col_int(q, r0, 3);
          int ws = 0; for (int k = i - 1; k < j; k++) ws += word[k]; if (!ws || !paths) continue;
          segments++; if (shown >= 32) continue; shown++;
          printf("%-10s \"", shown == 1 ? "segments" : i == 1 ? "" : "   also"); for (int k = i - 1; k < j; k++) { char *tx = reader_text(rd, &ph[k], 0); printf("%s", tx); free(tx); }
          printf("\"   %ld held, %ld as a run", paths, runs); if (paths >= fw.fan) printf(" (the fan)");
          int shownext = 0; for (int z = r0; z < r && shownext < 6; z++) { if (PQgetisnull(q, z, 4)) continue; long times = (long)col_int(q, z, 5); if (times < 2 && shownext) break;
              char *tx = reader_text(rd, col_id(q, z, 4), 0); printf("%s %s×%ld", shownext ? "" : "  then", strcmp(tx, " ") ? tx : "␠", times); free(tx); shownext++; }
          printf("\n"); }
      printf("%-10s %d segments observed of %d   (%.1f ms)\n", "", segments, np * (np + 1) / 2, (now() - t) * 1000);
      /* the fold: every observation that holds a run of two or more of the prompt's words is tugged at once. The claims
       * those observations sit in are the strands; the entity at the other end of each strand (the subject when the
       * observation is the object, the object when it is the subject, never the predicate) pulls back as hard as the
       * strand's standing, summed over every strand that reaches it. What pulls back hardest is what the prompt is about. */
      t = now(); lp_idmap *obs = lp_idmap_sized(0);
      for (int r0 = 0, r; r0 < nr; r0 = r) {
          r = segment_end(q, r0); int i = (int)col_int(q, r0, 0), j = (int)col_int(q, r0, 1); long runs = (long)col_int(q, r0, 3);
          int ws = 0; for (int k = i - 1; k < j; k++) ws += word[k]; if (ws < 2 || !runs || lp_idmap_count(obs) >= 2048) continue;
          Args oa = { 0 }; arg_ids(&oa, ph + i - 1, (size_t)(j - i + 1));
          PGresult *o = ask(pg, "SELECT entity FROM laplace_containers($1::blake3[], '{}'::smallint[]) WHERE NOT (mask ? 0::smallint) LIMIT 128", &oa);
          for (int z = 0; z < PQntuples(o); z++) lp_idmap_put(obs, col_id(o, z, 0), NULL);
          PQclear(o); args_free(&oa); }
      size_t nobs = lp_idmap_count(obs); lp_idmap *tug = lp_idmap_sized(sizeof(Tug)); int nstr = 0, nref = 0; double t_obs = now() - t; t = now();
      if (nobs) { Args fa = { 0 }; arg_ids(&fa, lp_idmap_keys(obs), nobs); arg_int(&fa, 64); arg_refused(&fa);
          PGresult *o = ask(pg, "SELECT i, path, rating, deviation, volatility FROM laplace_claims_each($1::blake3[], $2::bigint, '{0}'::smallint[], $3::blake3[])", &fa);     /* one call: every strand of every observation */
          Ids rn = { 0 };
          for (int z = 0; z < PQntuples(o); z++) { int64_t oi = col_int(o, z, 0) - 1; if (oi < 0 || (size_t)oi >= nobs) continue;
              path_into(&rn, col_path(o, z, 1)); lp_rating rt = col_rating(o, z, 2);
              int other = lp_tuple_other(rn.v, rn.n, lp_idmap_key(obs, (size_t)oi));     /* the other end; an observation that is the predicate pulls nothing */
              if (lp_tuple_middle_any(rn.v, rn.n, fw.id.refuse, (size_t)fw.nrefuse_predicate)) { nref++; continue; }      /* the firmware's refusals: strands of a kind it does not navigate */
              if (other < 0) continue;
              nstr++; Tug *x = lp_idmap_get(tug, &rn.v[other], NULL); x->pull += lp_confidence(&rt, fw.k) * strand_weight(&fw, fw.id.weigh, rn.v, (int)rn.n); x->strands++; }
          PQclear(o); args_free(&fa); lp_vec_free(&rn); }
      for (int k = 0; k < np; k++) { Tug *x = lp_idmap_lookup(tug, &ph[k]); if (x) x->pull = 0; }     /* the prompt's own words pull on nothing */
      size_t ntug = lp_idmap_count(tug); Tugged *tg = malloc(sizeof(Tugged) * (ntug + 1));
      for (size_t u = 0; u < ntug; u++) { tg[u].id = *lp_idmap_key(tug, u); tg[u].t = *(Tug *)lp_idmap_at(tug, u); }
      lp_sort(tg, ntug, sizeof(Tugged), by_tugged);
      for (size_t u = 0; u < ntug && u < 12; u++) reader_want(rd, &tg[u].id);
      printf("\npulls back %d strands of %zu observations, through what holds the prompt's runs; %d refused by the firmware   (%.1f ms to find them, %.1f ms to tug)\n", nstr, nobs, nref, t_obs * 1000, (now() - t) * 1000);
      for (size_t u = 0; u < ntug && u < 12 && tg[u].t.pull > 0; u++) { char *tx = reader_text(rd, &tg[u].id, 110); printf("%10.2f %8d   %s\n", tg[u].t.pull, tg[u].t.strands, tx); free(tx); }
      free(tg); lp_idmap_free(tug); lp_idmap_free(obs);
      PQclear(q); args_free(&sa); }
    Claim *mine = NULL; int nmine = -1, capped = 0;                           /* what is attested of the prompt itself, fetched once */
    #define MINE() do { if (nmine < 0) { mine = claims_of(pg, &pr.id, fw.fan, fw.k, &nmine, &capped); nmine = refused(pg, c, &fw, mine, nmine); weighed(c, &fw, mine, nmine); } } while (0)
    int held_back = 0, took = 0;

    for (int s = 0; s < fw.ntake && !held_back; s++) {
        double t = now();
        if (fw.take[s].what == FW_TAKE_FACT) {
            /* one member, curated by a witness the firmware trusts that far, is returned as a fact: the rest is held back */
            MINE(); if (!nmine || fw.fact > 1.0) continue;
            lp_idmap *at = lp_idmap_new(); Ids ids = { 0 }; for (int i = 0; i < nmine; i++) { lp_idmap_put(at, &mine[i].id, NULL); lp_push(&ids, mine[i].id); }
            Args ma = { 0 }; arg_ids(&ma, ids.v, ids.n);
            PGresult *q = ask(pg, "SELECT claim, trust FROM laplace_attested($1::blake3[])", &ma);
            int best = -1; double bt = -2;
            for (int j = 0; j < PQntuples(q); j++) { double tr = col_f64(q, j, 1); int64_t i = lp_idmap_find(at, col_id(q, j, 0));
                if (tr < fw.fact || i < 0) continue;
                if (tr > bt || (tr == bt && best >= 0 && mine[i].conf > mine[best].conf)) { bt = tr; best = (int)i; } }
            PQclear(q); args_free(&ma); lp_vec_free(&ids); lp_idmap_free(at);
            if (best >= 0) { printf("\nfact       curated at trust %g; the other %d strands of the set are held back   (%.1f ms)\n", bt, nmine - 1, (now() - t) * 1000);
                             printf("%10s %8s %6s %8s   %s\n", "confidence", "rating", "dev", "matches", "claim"); show_claim(rd, &mine[best], ""); held_back = 1; took++; }
        }
        else if (fw.take[s].what == FW_TAKE_SEGMENT) {
            /* the rest of the branch the prompt is a run of: every trajectory that holds the run, followed along what
             * they go on to, constituent by constituent, for as long as more than one of them goes the same way, and
             * then along the one that is left to its end */
            Ids keys = { 0 }; for (int i = 0; i < np; i++) if (word[i]) lp_push(&keys, ph[i]);
            if (!keys.n) for (int i = 0; i < np; i++) lp_push(&keys, ph[i]);
            Args ka = { 0 }; arg_ids(&ka, keys.v, keys.n);
            PGresult *q = ask(pg, "SELECT entity, path FROM laplace_containers($1::blake3[], '{}'::smallint[])", &ka);
            typedef struct { lp_id *id; int n; } Rest; lp_vec(Rest) rest = { 0 }; int holders = 0; Ids r = { 0 };
            for (int j = 0; j < PQntuples(q); j++) {
                path_into(&r, col_path(q, j, 1)); int found = 0;
                for (int i = 0; i + np <= (int)r.n; i++) if (!memcmp(&r.v[i], ph, sizeof(lp_id) * (size_t)np)) {          /* each place it holds the run */
                    Rest x = { malloc(sizeof(lp_id) * (r.n - (size_t)i - (size_t)np + 1)), (int)r.n - i - np }; memcpy(x.id, &r.v[i + np], sizeof(lp_id) * (size_t)x.n);
                    lp_push(&rest, x); found = 1; }
                holders += found;
            }
            PQclear(q); args_free(&ka); lp_vec_free(&keys); lp_vec_free(&r);
            printf("\nobserved   %d trajector%s hold the prompt as a run, in %zu places   (%.1f ms)\n", holders, holders == 1 ? "y" : "ies", rest.n, (now() - t) * 1000);
            Ids seg = { 0 }; uint8_t *alive = malloc(rest.n + 1); memset(alive, 1, rest.n + 1); size_t nalive = rest.n; int shared = 0;
            lp_idmap *tally = lp_idmap_new();                                    /* what the trajectories still followed go on to here, counted */
            for (int at = 0; nalive && seg.n < 4096; at++) {
                lp_idmap_clear(tally); size_t ended = 0;
                for (size_t i = 0; i < rest.n; i++) { if (!alive[i]) continue; if (rest.v[i].n <= at) { ended++; continue; } (*(uint32_t *)lp_idmap_get(tally, &rest.v[i].id[at], NULL))++; }
                size_t nt = lp_idmap_count(tally); if (!nt) break;                                  /* every one of them ends here */
                size_t top = 0; for (size_t k = 1; k < nt; k++) if (*lp_idmap_value(tally, k) > *lp_idmap_value(tally, top)) top = k;
                uint32_t topn = *lp_idmap_value(tally, top);
                if (topn < ended) break;                                                            /* more of them end here than go on */
                if (topn == 1 && nalive - ended > 1) break;                                         /* no two go the same way: nothing observed is shared from here */
                if (fw.top_within > 0) { size_t tied[64], ntied = 0; for (size_t k = 0; k < nt && ntied < 64; k++) if ((double)(topn - *lp_idmap_value(tally, k)) / (double)topn <= fw.top_within) tied[ntied++] = k; top = tied[rand_r(&seed) % (unsigned)ntied]; topn = *lp_idmap_value(tally, top); }
                if (topn > 1) shared = at + 1;
                lp_id step = *lp_idmap_key(tally, top); lp_push(&seg, step);
                for (size_t i = 0; i < rest.n; i++) if (alive[i] && (rest.v[i].n <= at || !lp_id_eq(&rest.v[i].id[at], &step))) { alive[i] = 0; nalive--; }
            }
            if (seg.n) { for (size_t i = 0; i < seg.n; i++) reader_want(rd, &seg.v[i]);
                printf("segment    \""); for (size_t i = 0; i < seg.n; i++) { char *tx = reader_text(rd, &seg.v[i], 0); printf("%s", tx); free(tx); } printf("\"\n");
                printf("           %zu constituents along the branch; the first %d are what more than one trajectory goes on to\n", seg.n, shared); took++; }
            else printf("segment    nothing follows it in what was observed\n");
            for (size_t i = 0; i < rest.n; i++) free(rest.v[i].id); lp_vec_free(&rest); lp_vec_free(&seg); free(alive); lp_idmap_free(tally);
        }
        else if (fw.take[s].what == FW_TAKE_ATTESTATIONS) {
            MINE(); take_top(mine, nmine, fw.take[s].n, &fw, &seed);
            printf("\nattested   of the prompt: %d strand%s%s   (%.1f ms)\n", nmine, nmine == 1 ? "" : "s", capped ? " (more exist than the fan reads)" : "", (now() - t) * 1000);
            if (nmine) printf("%10s %8s %6s %8s   %s\n", "confidence", "rating", "dev", "matches", "claim");
            for (int i = 0; i < nmine && i < fw.take[s].n; i++) { show_claim(rd, &mine[i], ""); took++; }
        }
        else if (fw.take[s].what == FW_TAKE_CONSTITUENTS) {
            lp_idmap *seen = lp_idmap_sized(0); int head = 0;
            for (int i = 0; i < np && lp_idmap_count(seen) < 256; i++) {
                if (!word[i] || lp_id_eq(&ph[i], &pr.id)) continue;                                 /* a composition, and not the prompt over again */
                bool fresh; lp_idmap_put(seen, &ph[i], &fresh); if (!fresh) continue;
                int n, cap2; Claim *cl = claims_of(pg, &ph[i], fw.fan, fw.k, &n, &cap2); n = refused(pg, c, &fw, cl, n); weighed(c, &fw, cl, n); take_top(cl, n, fw.take[s].n, &fw, &seed);
                if (!head) { printf("\nattested   of its constituents\n%10s %8s %6s %8s   %s\n", "confidence", "rating", "dev", "matches", "claim"); head = 1; }
                if (!n) { char *tx = reader_text(rd, &ph[i], 48); printf("%10s %8s %6s %8s   %s: nothing is attested of it\n", "", "", "", "", tx); free(tx); }
                for (int k = 0; k < n && k < fw.take[s].n; k++) { show_claim(rd, &cl[k], ""); took++; }
                free(cl);
            }
            if (head) printf("           (%.1f ms)\n", (now() - t) * 1000);
            lp_idmap_free(seen);
        }
        else if (fw.take[s].what == FW_TAKE_CHAIN && fw.nalt) {
            /* The word that pulls hardest, and the relations followed from it. How hard a word pulls is its role: what is
             * attested of it under the firmware's role kind (its part of speech), read through the firmware's weights, so
             * "dog" carries "What is a dog?" and "What", "is" and "a" do not. From that word each relation of the chain is
             * followed in turn (chain_follow); where it ends is what is returned. */
            typedef struct { lp_id id; double w; int at; } Puller; lp_vec(Puller) pl = { 0 }; lp_idmap *seen = lp_idmap_sized(0);
            for (int i = 0; i < np && pl.n < 64; i++) {
                if (!word[i] || (np > 1 && lp_id_eq(&ph[i], &pr.id))) continue;
                bool fresh; lp_idmap_put(seen, &ph[i], &fresh); if (!fresh) continue;
                double w = role_of(pg, &fw, &ph[i]); if (w < 0) w = fw.role_by[0] ? 0.5 : 1.0;             /* a word nothing says the role of pulls half */
                lp_push(&pl, (Puller){ ph[i], w, i });
            }
            lp_idmap_free(seen);
            for (size_t a_ = 1; a_ < pl.n; a_++) { Puller x = pl.v[a_]; size_t b_ = a_; while (b_ > 0 && pl.v[b_ - 1].w < x.w) { pl.v[b_] = pl.v[b_ - 1]; b_--; } pl.v[b_] = x; }
            for (size_t u = 0; u < pl.n && (int)u < fw.take[s].n; u++) {
                if (pl.v[u].w <= 0) break;
                lp_id ans; Claim last; int alt, steps = chain_follow(pg, rd, &fw, &pl.v[u].id, NULL, &seed, &ans, &last, &alt);
                char *who = reader_text(rd, &pl.v[u].id, 64);
                if (alt >= 0) { char *tx = reader_text(rd, &ans, 600);
                    printf("\nanswer     %s: %s\n", who, tx); free(tx);
                    printf("           through"); for (int z = 0; z < steps; z++) printf(" %s", fw.chain[alt][z]); printf(", %s pulling at %.2f", who, pl.v[u].w);
                    printf("; standing of the last strand %.0f +/- %.0f, %d matches   (%.1f ms)\n", last.r.rating, last.r.deviation, last.matches, (now() - t) * 1000); took++; }
                else printf("\nanswer     %s: the chain stops after %d of %d relations: nothing is attested there   (%.1f ms)\n", who, steps, fw.nchain[fw.nalt - 1], (now() - t) * 1000);
                free(who);
            }
            lp_vec_free(&pl);
        }
    }
    printf("\n%d taken in %d step%s   %llu round trips for text   total %.1f ms\n", took, fw.ntake, fw.ntake == 1 ? "" : "s", (unsigned long long)reader_trips(rd), (now() - T) * 1000);
    free(mine); free(ph); free(word); reader_free(rd); PQfinish(pg);
    return 0;
}
