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
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void show_claim(Reader *rd, const Claim *c, const char *lead){
    printf("%s%10.3f %8.0f %6.0f %8d   [", lead, c->conf, c->r.rating, c->r.deviation, c->matches);
    for (int p = 0; p < c->np; p++) { char *tx = reader_text(rd, &c->part[p], p == c->np - 1 ? 96 : 48); printf("%s%s", p ? ", " : "", tx); free(tx); }
    printf("]\n");
}
int cmd_pull(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; int a = 1; unsigned seed = 0; int seeded = 0;
    for (; a < argc - 1 && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "--firmware") && a + 1 < argc) fwp = argv[++a];
        else if (!strcmp(argv[a], "--seed") && a + 1 < argc) { seed = (unsigned)strtoul(argv[++a], NULL, 10); seeded = 1; }
    }
    if (a >= argc) { fprintf(stderr, "usage: laplace pull [-d conninfo] [--firmware FILE] [--seed N] prompt\n"); return 2; }
    double T = now(); if (!seeded) seed = (unsigned)(T * 1e6);
    Firmware fw = firmware_for(fwp, FW_PULL);
    tier0_open(NULL); table_init(); ctx_open(1); lp_text *c = lp_text_new(T0);
    const char *prompt = argv[a];

    /* ---- the prompt: broken down, and its trunk */
    Ref pr = text_ref(CTX[0], (const uint8_t *)prompt, strlen(prompt));
    Node *pn = table_find(&pr.id); lp_id *ph; int np = 0;
    if (!pn || pr.tier <= 2) { ph = malloc(sizeof(lp_id)); ph[0] = pr.id; np = 1; }      /* a word is one constituent of what holds it: itself */
    else { ph = malloc(sizeof(lp_id) * pn->len); np = (int)table_parts(&pr.id, ph, pn->len); }
    char idt[33]; id_text(&pr.id, idt);
    firmware_say(&fw, FW_PULL);
    printf("prompt     %s   tier %d   %d constituent%s", idt, pr.tier, np, np == 1 ? "" : "s");
    { int words = 0; for (int i = 0; i < np; i++) words += table_find(&ph[i]) != NULL; printf(", %d of them compositions\n", words); }

    PGconn *pg = db_connect(conninfo); Reader *rd = reader_new(pg);
    /* every segment of the prompt at once (laplace_forward): "The", "The dog", "The dog barked" and every other run of it,
     * the observations that hold each, how many as a run, and what follows the run in them, counted. The whole pass is one
     * set; a segment that is only tier-0 atoms (a space, a letter) is a hub and is not shown. */
    { double t = now(); uint8_t *ab = malloc(20 + 20 * (size_t)np); size_t al = ids_param(ab, ph, (uint32_t)np); char fan[24]; snprintf(fan, sizeof fan, "%d", fw.fan);
      const char *v[2] = { (const char *)ab, fan }; int l[2] = { (int)al, (int)strlen(fan) }, f[2] = { 1, 0 };
      /* the prefixes first, as the pass walks them ("The", "The dog", ...), then every other segment by how many hold it */
      PGresult *q = db_ask(pg, "SELECT i, j, paths, runs, next, times FROM laplace_forward($1::blake3[], $2::bigint) ORDER BY (i > 1), CASE WHEN i = 1 THEN j END, paths DESC, (j - i) DESC, i, times DESC NULLS LAST", 2, v, l, f);
      if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "forward: %s", PQerrorMessage(pg)); return 1; }
      int nr = PQntuples(q), shown = 0, segments = 0;
      for (int r = 0; r < nr; r++) if (!PQgetisnull(q, r, 4)) { lp_id x; memcpy(x.b, PQgetvalue(q, r, 4), 16); reader_want(rd, &x); }
      for (int i = 0; i < np; i++) reader_want(rd, &ph[i]);
      for (int r = 0; r < nr; ) { int i = (int)lp_be(PQgetvalue(q, r, 0), 4), j = (int)lp_be(PQgetvalue(q, r, 1), 4); long paths = (long)lp_be(PQgetvalue(q, r, 2), 8), runs = (long)lp_be(PQgetvalue(q, r, 3), 8); int r0 = r;
          while (r < nr && (int)lp_be(PQgetvalue(q, r, 0), 4) == i && (int)lp_be(PQgetvalue(q, r, 1), 4) == j) r++;
          int words = 0; for (int k = i - 1; k < j; k++) words += table_find(&ph[k]) != NULL; if (!words || !paths) continue;
          segments++; if (shown >= 32) continue; shown++;
          printf("%-10s \"", shown == 1 ? "segments" : i == 1 ? "" : "   also"); for (int k = i - 1; k < j; k++) { char *tx = reader_text(rd, &ph[k], 0); printf("%s", tx); free(tx); }
          printf("\"   %ld held, %ld as a run", paths, runs); if (paths >= fw.fan) printf(" (the fan)");
          int shownext = 0; for (int z = r0; z < r && shownext < 6; z++) { if (PQgetisnull(q, z, 4)) continue; long times = (long)lp_be(PQgetvalue(q, z, 5), 8); if (times < 2 && shownext) break;
              lp_id x; memcpy(x.b, PQgetvalue(q, z, 4), 16); char *tx = reader_text(rd, &x, 0); printf("%s %s×%ld", shownext ? "" : "  then", strcmp(tx, " ") ? tx : "␠", times); free(tx); shownext++; }
          printf("\n"); }
      printf("%-10s %d segments observed of %d   (%.1f ms)\n", "", segments, np * (np + 1) / 2, (now() - t) * 1000);
      /* the fold: every observation that holds a run of two or more of the prompt's words is tugged at once. The claims
       * those observations sit in are the strands; the entity at the other end of each strand (the subject when the
       * observation is the object, the object when it is the subject, never the predicate) pulls back as hard as the
       * strand's standing, summed over every strand that reaches it. What pulls back hardest is what the prompt is about. */
      t = now(); typedef struct { lp_id id; int16_t tier; } Ob; Ob *obs = NULL; int nobs = 0, cobs = 0;
      for (int r = 0; r < nr; ) { int i = (int)lp_be(PQgetvalue(q, r, 0), 4), j = (int)lp_be(PQgetvalue(q, r, 1), 4); long runs = (long)lp_be(PQgetvalue(q, r, 3), 8);
          while (r < nr && (int)lp_be(PQgetvalue(q, r, 0), 4) == i && (int)lp_be(PQgetvalue(q, r, 1), 4) == j) r++;
          int words = 0; for (int k = i - 1; k < j; k++) words += table_find(&ph[k]) != NULL; if (words < 2 || !runs || nobs >= 2048) continue;
          int floor = 0; for (int k = i - 1; k < j; k++) { Node *nd = table_find(&ph[k]); if (nd && nd->tier > floor) floor = nd->tier; }
          int nh = 0; Hold *hh = holds_above(ph + i - 1, j - i + 1, floor, 0, 0, &nh);
          for (int z = 0; z < nh && nobs < 2048; z++) { if (hh[z].claim) continue; int dup = 0;
              for (int u = 0; u < nobs && !dup; u++) dup = !memcmp(&obs[u].id, &hh[z].entity, 16); if (dup) continue;
              if (nobs == cobs) { cobs = cobs ? cobs * 2 : 256; obs = xrealloc(obs, sizeof(Ob) * (size_t)cobs); } obs[nobs++] = (Ob){ hh[z].entity, hh[z].tier }; }
          holds_free(hh, nh); }
      typedef struct { lp_id id; double pull; int strands; } Tug; Tug *tug = NULL; int ntug = 0, ctug = 0, nstr = 0, nref = 0; double t_obs = now() - t; t = now();
      lp_id wids[FW_WEIGHS]; weights_named(c, &fw, wids);
      lp_id refuse[FW_NAMES]; for (int z = 0; z < fw.nrefuse_predicate; z++) refuse[z] = entity_named(c, fw.refuse_predicate[z], NULL, 0, NULL).id;     /* the firmware's refusals: strands of a kind it does not navigate */
      if (nobs) {
          /* one tier at a time: a tier-4 observation is not looked for in tier 3. Every leaf of the tiers above, on every core. */
          uint8_t seen[256]; memset(seen, 0, sizeof seen);
          for (int s = 0; s < nobs; s++) { int tr = obs[s].tier; if (tr < 0 || tr > 255 || seen[tr]) continue; seen[tr] = 1;
              lp_id *g = malloc(sizeof(lp_id) * (size_t)nobs); int *map = malloc(sizeof(int) * (size_t)nobs); int ng = 0;
              for (int u = 0; u < nobs; u++) if (obs[u].tier == tr) { g[ng] = obs[u].id; map[ng++] = u; }
              int nh = 0; Hold *hh = holds_above(g, ng, tr, 1, 1, &nh);
              for (int z = 0; z < nh; z++) { int oi = hh[z].src; if (oi < 0 || oi >= ng || !hh[z].claim || !hh[z].stood) continue; oi = map[oi];
                  Run rn = run_of(hh[z].path, (size_t)hh[z].path_len); lp_rating rt = hh[z].r;
                  int at = -1; for (int k = 0; k < rn.n && at < 0; k++) if (!memcmp(&rn.id[k], &obs[oi].id, 16)) at = k;
                  int other = rn.n == 2 ? 1 - at : rn.n >= 3 && at == 0 ? rn.n - 1 : rn.n >= 3 && at == rn.n - 1 ? 0 : -1;        /* the other end; an observation that is the predicate pulls nothing */
                  int refused_ = 0; for (int k = 1; k + 1 < rn.n && !refused_; k++) for (int y = 0; y < fw.nrefuse_predicate; y++) if (!memcmp(&rn.id[k], &refuse[y], 16)) refused_ = 1;
                  if (refused_) { nref++; other = -1; }
                  if (at >= 0 && other >= 0) { nstr++; double c_ = lp_confidence(&rt, fw.k) * strand_weight(&fw, wids, rn.id, rn.n); int found = -1; for (int u = 0; u < ntug; u++) if (!memcmp(&tug[u].id, &rn.id[other], 16)) { found = u; break; }
                      if (found < 0) { if (ntug == ctug) { ctug = ctug ? ctug * 2 : 256; tug = xrealloc(tug, sizeof(Tug) * (size_t)ctug); } tug[ntug] = (Tug){ rn.id[other], 0, 0 }; found = ntug++; }
                      tug[found].pull += c_; tug[found].strands++; }
                  free(rn.id); }
              holds_free(hh, nh); free(g); free(map); } }
      for (int u = 0; u < ntug; u++) for (int k = 0; k < np; k++) if (!memcmp(&tug[u].id, &ph[k], 16)) tug[u].pull = 0;     /* the prompt's own words pull on nothing */
      for (int u = 1; u < ntug; u++) { Tug x = tug[u]; int y = u; while (y > 0 && tug[y - 1].pull < x.pull) { tug[y] = tug[y - 1]; y--; } tug[y] = x; }
      for (int u = 0; u < ntug && u < 12; u++) reader_want(rd, &tug[u].id);
      printf("\npulls back %d strands of %d observations, through what holds the prompt's runs; %d refused by the firmware   (%.1f ms to find them, %.1f ms to tug)\n", nstr, nobs, nref, t_obs * 1000, (now() - t) * 1000);
      for (int u = 0; u < ntug && u < 12 && tug[u].pull > 0; u++) { char *tx = reader_text(rd, &tug[u].id, 110); printf("%10.2f %8d   %s\n", tug[u].pull, tug[u].strands, tx); free(tx); }
      free(tug); free(obs);
      #undef BE
      PQclear(q); free(ab); }
    Claim *mine = NULL; int nmine = -1, capped = 0;                           /* what is attested of the prompt itself, fetched once */
    #define MINE() do { if (nmine < 0) { mine = claims_of(pg, &pr.id, fw.fan, fw.k, &nmine, &capped); nmine = refused(pg, c, &fw, mine, nmine); weighed(c, &fw, mine, nmine); } } while (0)
    int held_back = 0, took = 0;

    for (int s = 0; s < fw.ntake && !held_back; s++) {
        double t = now();
        if (fw.take[s].what == FW_TAKE_FACT) {
            /* one member, curated by a witness the firmware trusts that far, is returned as a fact: the rest is held back */
            MINE(); if (!nmine || fw.fact > 1.0) continue;
            lp_id *ids = malloc(sizeof(lp_id) * (size_t)nmine); for (int i = 0; i < nmine; i++) ids[i] = mine[i].id;
            uint8_t *ab = malloc(20 + 20 * (size_t)nmine); size_t al = ids_param(ab, ids, (uint32_t)nmine);
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
            PGresult *q = db_ask(pg, "SELECT claim, trust FROM laplace_attested($1::blake3[])", 1, v, l, f);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "witnesses: %s", PQerrorMessage(pg)); return 1; }
            int best = -1; double bt = -2;
            for (int j = 0; j < PQntuples(q); j++) { uint64_t u = 0; const uint8_t *b = (const uint8_t *)PQgetvalue(q, j, 1); for (int y = 0; y < 8; y++) u = u << 8 | b[y]; double tr; memcpy(&tr, &u, 8);
                if (tr < fw.fact) continue;
                for (int i = 0; i < nmine; i++) if (!memcmp(mine[i].id.b, PQgetvalue(q, j, 0), 16)) { if (tr > bt || (tr == bt && best >= 0 && mine[i].conf > mine[best].conf)) { bt = tr; best = i; } break; } }
            PQclear(q); free(ab); free(ids);
            if (best >= 0) { printf("\nfact       curated at trust %g; the other %d strands of the set are held back   (%.1f ms)\n", bt, nmine - 1, (now() - t) * 1000);
                             printf("%10s %8s %6s %8s   %s\n", "confidence", "rating", "dev", "matches", "claim"); show_claim(rd, &mine[best], ""); held_back = 1; took++; }
        }
        else if (fw.take[s].what == FW_TAKE_SEGMENT) {
            /* the rest of the branch the prompt is a run of: every trajectory that holds the run, followed along what
             * they go on to, constituent by constituent, for as long as more than one of them goes the same way, and
             * then along the one that is left to its end */
            lp_id *keys = malloc(sizeof(lp_id) * (size_t)np); int nk = 0, floor = 0;
            for (int i = 0; i < np; i++) { Node *nd = table_find(&ph[i]); if (!nd) continue; keys[nk++] = ph[i]; if (nd->tier > floor) floor = nd->tier; }
            if (!nk) { memcpy(keys, ph, sizeof(lp_id) * (size_t)np); nk = np; }
            int nh = 0; Hold *hh = holds_above(keys, nk, floor, 0, 0, &nh);
            typedef struct { lp_id *id; int n; lp_id in; } Rest; Rest *rest = malloc(sizeof(Rest) * (size_t)(nh + 1)); int nrest = 0, holders = 0;
            for (int j = 0; j < nh; j++) {
                Run r = run_of(hh[j].path, (size_t)hh[j].path_len); int found = 0;
                for (int i = 0; i + np <= r.n; i++) if (!memcmp(&r.id[i], ph, sizeof(lp_id) * (size_t)np)) {          /* each place it holds the run */
                    Rest x = { malloc(sizeof(lp_id) * (size_t)(r.n - i - np + 1)), r.n - i - np }; memcpy(x.id, &r.id[i + np], sizeof(lp_id) * (size_t)x.n); memcpy(x.in.b, hh[j].entity.b, 16);
                    rest = xrealloc(rest, sizeof(Rest) * (size_t)(nrest + 2)); rest[nrest++] = x; found = 1; }
                holders += found; free(r.id);
            }
            holds_free(hh, nh); free(keys);
            printf("\nobserved   %d trajector%s hold the prompt as a run, in %d places   (%.1f ms)\n", holders, holders == 1 ? "y" : "ies", nrest, (now() - t) * 1000);
            lp_id *seg = malloc(sizeof(lp_id) * 4096); int nseg = 0; uint8_t *alive = malloc((size_t)(nrest ? nrest : 1)); memset(alive, 1, (size_t)(nrest ? nrest : 1)); int nalive = nrest, shared = 0;
            for (int at = 0; nalive && nseg < 4096; at++) {
                /* what the trajectories still followed go on to here, counted */
                typedef struct { lp_id id; int n; } Tally; Tally *tl = malloc(sizeof(Tally) * (size_t)nalive); int nt = 0, ended = 0;
                for (int i = 0; i < nrest; i++) { if (!alive[i]) continue; if (rest[i].n <= at) { ended++; continue; }
                    int k = 0; while (k < nt && memcmp(&tl[k].id, &rest[i].id[at], 16)) k++; if (k == nt) { tl[nt].id = rest[i].id[at]; tl[nt++].n = 0; } tl[k].n++; }
                if (!nt) { free(tl); break; }                                                    /* every one of them ends here */
                int top = 0; for (int k = 1; k < nt; k++) if (tl[k].n > tl[top].n) top = k;
                if (tl[top].n < ended) { free(tl); break; }                                      /* more of them end here than go on */
                if (tl[top].n == 1 && nalive - ended > 1) { free(tl); break; }                   /* no two go the same way: nothing observed is shared from here */
                if (fw.top_within > 0) { int tied[64], ntied = 0; for (int k = 0; k < nt && ntied < 64; k++) if ((double)(tl[top].n - tl[k].n) / (double)tl[top].n <= fw.top_within) tied[ntied++] = k; top = tied[rand_r(&seed) % (unsigned)ntied]; }
                if (tl[top].n > 1) shared = at + 1;
                seg[nseg++] = tl[top].id;
                for (int i = 0; i < nrest; i++) if (alive[i] && (rest[i].n <= at || memcmp(&rest[i].id[at], &tl[top].id, 16))) { alive[i] = 0; nalive--; }
                free(tl);
            }
            if (nseg) { for (int i = 0; i < nseg; i++) reader_want(rd, &seg[i]);
                printf("segment    \""); for (int i = 0; i < nseg; i++) { char *tx = reader_text(rd, &seg[i], 0); printf("%s", tx); free(tx); } printf("\"\n");
                printf("           %d constituents along the branch; the first %d are what more than one trajectory goes on to\n", nseg, shared); took++; }
            else printf("segment    nothing follows it in what was observed\n");
            for (int i = 0; i < nrest; i++) free(rest[i].id); free(rest); free(seg); free(alive);
        }
        else if (fw.take[s].what == FW_TAKE_ATTESTATIONS) {
            MINE(); take_top(mine, nmine, fw.take[s].n, &fw, &seed);
            printf("\nattested   of the prompt: %d strand%s%s   (%.1f ms)\n", nmine, nmine == 1 ? "" : "s", capped ? " (more exist than the fan reads)" : "", (now() - t) * 1000);
            if (nmine) printf("%10s %8s %6s %8s   %s\n", "confidence", "rating", "dev", "matches", "claim");
            for (int i = 0; i < nmine && i < fw.take[s].n; i++) { show_claim(rd, &mine[i], ""); took++; }
        }
        else if (fw.take[s].what == FW_TAKE_CONSTITUENTS) {
            lp_id *seen = malloc(sizeof(lp_id) * (size_t)(np ? np : 1)); int nseen = 0, head = 0;
            { lp_idmap *sm = lp_idmap_new();
              for (int i = 0; i < np; i++) { if (!table_find(&ph[i]) || !memcmp(&ph[i], &pr.id, 16)) continue;    /* a composition, and not the prompt over again */
                  bool fresh; lp_idmap_put(sm, &ph[i], &fresh); if (fresh) seen[nseen++] = ph[i]; }
              lp_idmap_free(sm); }
            int nall, *src; Claim *all = claims_each(pg, seen, nseen, NULL, 0, -1, fw.k, &nall, &src);      /* every constituent's claims, one set */
            for (int i = 0, at = 0; i < nseen; i++) {
                int n = 0; while (at + n < nall && src[at + n] == i) n++;
                Claim *cl = malloc(sizeof(Claim) * (size_t)(n ? n : 1)); memcpy(cl, all + at, sizeof(Claim) * (size_t)n); at += n;
                if (fw.fan >= 0 && n > fw.fan) n = fw.fan;                                          /* the fan: its hardest pulling */
                n = refused(pg, c, &fw, cl, n); weighed(c, &fw, cl, n); take_top(cl, n, fw.take[s].n, &fw, &seed);
                if (!head) { printf("\nattested   of its constituents\n%10s %8s %6s %8s   %s\n", "confidence", "rating", "dev", "matches", "claim"); head = 1; }
                if (!n) { char *tx = reader_text(rd, &seen[i], 48); printf("%10s %8s %6s %8s   %s: nothing is attested of it\n", "", "", "", "", tx); free(tx); }
                for (int k = 0; k < n && k < fw.take[s].n; k++) { show_claim(rd, &cl[k], ""); took++; }
                free(cl);
            }
            free(all); free(src); free(seen);
            if (head) printf("           (%.1f ms)\n", (now() - t) * 1000);
        }
        else if (fw.take[s].what == FW_TAKE_CHAIN) {
            /* The word that pulls hardest, and the relations followed from it. How hard a word pulls is its role: what is
             * attested of it under the firmware's role kind (its part of speech), read through the firmware's weights, so
             * "dog" carries "What is a dog?" and "What", "is" and "a" do not. From that word each relation of the chain is
             * followed in turn: the claims that hold where the pull stands (or, past the first step, one of the things it
             * is made of), in the order their witness gave them, then by standing; the top is taken, and the pull stands
             * at what that claim says. Where it ends is what is returned. */
            typedef struct { lp_id id; double w; int at; } Puller; Puller *pl = malloc(sizeof(Puller) * (size_t)(np ? np : 1)); int npl = 0;
            lp_id by; memset(&by, 0, sizeof by); if (fw.role_by[0]) by = entity_named(c, fw.role_by, NULL, 0, NULL).id;
            lp_id rid[FW_WEIGHS]; for (int z = 0; z < fw.nrole; z++) rid[z] = entity_named(c, fw.role_name[z], NULL, 0, NULL).id;
            { lp_idmap *sm = lp_idmap_new();
              for (int i = 0; i < np; i++) { if (!table_find(&ph[i]) || (np > 1 && !memcmp(&ph[i], &pr.id, 16))) continue;
                  bool fresh; lp_idmap_put(sm, &ph[i], &fresh); if (fresh) pl[npl++] = (Puller){ ph[i], fw.role_by[0] ? 0.5 : 1.0, i }; }     /* a word nothing says the role of pulls half */
              lp_idmap_free(sm); }
            if (fw.role_by[0] && npl) {                                                  /* every word's role, one set */
                lp_id *ws = malloc(sizeof(lp_id) * (size_t)npl); for (int i = 0; i < npl; i++) ws[i] = pl[i].id;
                int n, *src; Claim *cl = claims_each(pg, ws, npl, &by, 1, fw.fan, fw.k, &n, &src); uint8_t *done = calloc((size_t)npl, 1);
                for (int q = 0; q < n; q++) { if (done[src[q]]) continue;
                    for (int z = 0; z < fw.nrole; z++) if (!memcmp(&cl[q].part[cl[q].np - 1], &rid[z], 16)) { pl[src[q]].w = fw.role[z]; done[src[q]] = 1; break; } }
                free(cl); free(src); free(done); free(ws); }
            for (int a_ = 1; a_ < npl; a_++) { Puller x = pl[a_]; int b_ = a_; while (b_ > 0 && pl[b_ - 1].w < x.w) { pl[b_] = pl[b_ - 1]; b_--; } pl[b_] = x; }
            for (int u = 0; u < npl && u < fw.take[s].n; u++) {
                if (pl[u].w <= 0) break;
                lp_id cur = pl[u].id; Claim kept[FW_CHAIN]; int nk = 0, alt = 0;
                for (alt = 0; alt < fw.nalt; alt++) { cur = pl[u].id; nk = 0;                /* each chain the firmware names, until one reaches its end */
                for (int z = 0; z < fw.nchain[alt]; z++) {
                    lp_id pred = entity_named(c, fw.chain[alt][z], NULL, 0, NULL).id, tryv[66]; int nt = 0; tryv[nt++] = cur;
                    if (z > 0 && lp_tier0_codepoint(T0, &cur) < 0) {                       /* what it is made of, the last first, the puller itself left out */
                        uint8_t ab[40]; size_t al = ids_param(ab, &cur, 1); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
                        PGresult *q = db_ask(pg, "SELECT path FROM laplace_paths($1::blake3[]) LIMIT 1", 1, v, l, f);
                        if (PQresultStatus(q) == PGRES_TUPLES_OK && PQntuples(q)) { Run rn = run_of((const uint8_t *)PQgetvalue(q, 0, 0), (size_t)PQgetlength(q, 0, 0));
                            for (int k = rn.n - 1; k >= 0 && nt < 66; k--) if (lp_tier0_codepoint(T0, &rn.id[k]) < 0 && memcmp(&rn.id[k], &pl[u].id, 16)) tryv[nt++] = rn.id[k];
                            free(rn.id); }
                        PQclear(q);
                        if (nt < 66) tryv[nt++] = pl[u].id; }                                    /* a synset of one member is that word: what is said of it is said of the word */
                    Claim *cl = NULL; int n = 0, cap2;
                    for (int t2 = 0; t2 < nt && !n; t2++) { lp_id part[3] = { tryv[t2], pred, pred }; int have[3] = { 2, 2, 0 };
                        cl = claims_like(pg, part, have, fw.fan, fw.k, &n, &cap2); if (!n) { free(cl); cl = NULL; } }     /* a relation the firmware names to follow is not one it refuses */
                    if (!n) break;
                    positions_of(pg, cl, n); if (fw.order_witness) qsort(cl, (size_t)n, sizeof(Claim), claim_by_position);
                    take_top(cl, n, 1, &fw, &seed);
                    kept[nk++] = cl[0]; cur = cl[0].part[cl[0].np - 1]; free(cl);
                }
                if (nk == fw.nchain[alt]) break; }
                if (alt == fw.nalt) alt = fw.nalt - 1;
                char *who = reader_text(rd, &pl[u].id, 64);
                if (nk == fw.nchain[alt]) { char *tx = reader_text(rd, &cur, 600);
                    printf("\nanswer     %s: %s\n", who, tx); free(tx);
                    printf("           through"); for (int z = 0; z < nk; z++) printf(" %s", fw.chain[alt][z]); printf(", %s pulling at %.2f", who, pl[u].w);
                    printf("; standing of the last strand %.0f +/- %.0f, %d matches   (%.1f ms)\n", kept[nk - 1].r.rating, kept[nk - 1].r.deviation, kept[nk - 1].matches, (now() - t) * 1000); took++; }
                else printf("\nanswer     %s: the chain stops after %d of %d relations: nothing is attested there   (%.1f ms)\n", who, nk, fw.nchain[alt], (now() - t) * 1000);
                free(who);
            }
            free(pl);
        }
    }
    printf("\n%d taken in %d step%s   %llu round trips for text   total %.1f ms\n", took, fw.ntake, fw.ntake == 1 ? "" : "s", (unsigned long long)reader_trips(rd), (now() - T) * 1000);
    free(mine); free(ph); reader_free(rd); PQfinish(pg);
    return 0;
}
