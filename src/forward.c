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

typedef struct { lp_id *id; int n; } Run;                                    /* a trajectory's constituents in order, runs written out */

static double be_f64(const char *p){ uint64_t u = 0; for (int i = 0; i < 8; i++) u = u << 8 | (uint8_t)p[i]; double d; memcpy(&d, &u, 8); return d; }
static Run run_of(const uint8_t *ewkb, size_t len){
    const uint8_t *vx; size_t nv = lp_ewkb_vertices(ewkb, len, &vx); int n = 0;
    for (size_t i = 0; i < nv; i++) { double m; memcpy(&m, vx + 32 * i + 24, 8); n += (int)lp_m_run(m); }
    Run r = { malloc(sizeof(lp_id) * (size_t)(n ? n : 1)), 0 };
    for (size_t i = 0; i < nv; i++) { double xyz[3], m; memcpy(xyz, vx + 32 * i, 24); memcpy(&m, vx + 32 * i + 24, 8); lp_id id; lp_xyz_to_id(xyz, &id);
        for (uint32_t k = 0; k < lp_m_run(m); k++) r.id[r.n++] = id; }
    return r;
}
static void show_claim(Reader *rd, const Claim *c, const char *lead){
    printf("%s%10.3f %8.0f %6.0f %8d   [", lead, c->conf, c->r.rating, c->r.deviation, c->matches);
    for (int p = 0; p < c->np; p++) { char *tx = reader_text(rd, &c->part[p], p == c->np - 1 ? 96 : 48); printf("%s%s", p ? ", " : "", tx); free(tx); }
    printf("]\n");
}
/* The strands of a set a firmware takes: the top n, or, when the firmware says how near a tie has to be, any strand
 * that near the one above it may be taken in its place. */
static void take_top(Claim *cl, int n, int want, const Firmware *fw, unsigned *seed){
    if (fw->top_within <= 0) return;
    for (int i = 0; i < n && i < want; i++) { int tied = i; while (tied + 1 < n && cl[i].conf - cl[tied + 1].conf <= fw->top_within) tied++;
        if (tied > i) { int pick = i + (int)(rand_r(seed) % (unsigned)(tied - i + 1)); Claim t = cl[i]; cl[i] = cl[pick]; cl[pick] = t; } }
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
    else { Shard *s = &shard[pr.id.b[0]]; for (uint32_t v = 0; v < pn->nv; v++) np += (int)VRUN(s->vtx[pn->voff + v].m);
           ph = malloc(sizeof(lp_id) * (size_t)np); int k = 0;
           for (uint32_t v = 0; v < pn->nv; v++)
               for (uint32_t r = 0; r < VRUN(s->vtx[pn->voff + v].m); r++) ph[k++] = s->vtx[pn->voff + v].id; }
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
      #define BE(p_, n_) ({ uint64_t u_ = 0; for (int y_ = 0; y_ < (n_); y_++) u_ = u_ << 8 | (uint8_t)(p_)[y_]; u_; })
      int nr = PQntuples(q), shown = 0, segments = 0;
      for (int r = 0; r < nr; r++) if (!PQgetisnull(q, r, 4)) { lp_id x; memcpy(x.b, PQgetvalue(q, r, 4), 16); reader_want(rd, &x); }
      for (int i = 0; i < np; i++) reader_want(rd, &ph[i]);
      for (int r = 0; r < nr; ) { int i = (int)BE(PQgetvalue(q, r, 0), 4), j = (int)BE(PQgetvalue(q, r, 1), 4); long paths = (long)BE(PQgetvalue(q, r, 2), 8), runs = (long)BE(PQgetvalue(q, r, 3), 8); int r0 = r;
          while (r < nr && (int)BE(PQgetvalue(q, r, 0), 4) == i && (int)BE(PQgetvalue(q, r, 1), 4) == j) r++;
          int words = 0; for (int k = i - 1; k < j; k++) words += table_find(&ph[k]) != NULL; if (!words || !paths) continue;
          segments++; if (shown >= 32) continue; shown++;
          printf("%-10s \"", shown == 1 ? "segments" : i == 1 ? "" : "   also"); for (int k = i - 1; k < j; k++) { char *tx = reader_text(rd, &ph[k], 0); printf("%s", tx); free(tx); }
          printf("\"   %ld held, %ld as a run", paths, runs); if (paths >= fw.fan) printf(" (the fan)");
          int shownext = 0; for (int z = r0; z < r && shownext < 6; z++) { if (PQgetisnull(q, z, 4)) continue; long times = (long)BE(PQgetvalue(q, z, 5), 8); if (times < 2 && shownext) break;
              lp_id x; memcpy(x.b, PQgetvalue(q, z, 4), 16); char *tx = reader_text(rd, &x, 0); printf("%s %s×%ld", shownext ? "" : "  then", strcmp(tx, " ") ? tx : "␠", times); free(tx); shownext++; }
          printf("\n"); }
      printf("%-10s %d segments observed of %d   (%.1f ms)\n", "", segments, np * (np + 1) / 2, (now() - t) * 1000);
      /* the fold: every observation that holds a run of two or more of the prompt's words is tugged at once. The claims
       * those observations sit in are the strands; the entity at the other end of each strand (the subject when the
       * observation is the object, the object when it is the subject, never the predicate) pulls back as hard as the
       * strand's standing, summed over every strand that reaches it. What pulls back hardest is what the prompt is about. */
      t = now(); lp_id *obs = NULL; int nobs = 0, cobs = 0;
      for (int r = 0; r < nr; ) { int i = (int)BE(PQgetvalue(q, r, 0), 4), j = (int)BE(PQgetvalue(q, r, 1), 4); long runs = (long)BE(PQgetvalue(q, r, 3), 8);
          while (r < nr && (int)BE(PQgetvalue(q, r, 0), 4) == i && (int)BE(PQgetvalue(q, r, 1), 4) == j) r++;
          int words = 0; for (int k = i - 1; k < j; k++) words += table_find(&ph[k]) != NULL; if (words < 2 || !runs || nobs >= 2048) continue;
          uint8_t *sb = malloc(20 + 20 * (size_t)(j - i + 1)); size_t sl = ids_param(sb, ph + i - 1, (uint32_t)(j - i + 1)); const char *sv[1] = { (const char *)sb }; int sll[1] = { (int)sl }, sf[1] = { 1 };
          PGresult *o = db_ask(pg, "SELECT entity FROM laplace_containers($1::blake3[], '{}'::smallint[]) WHERE NOT (mask ? 0::smallint) LIMIT 128", 1, sv, sll, sf);
          if (PQresultStatus(o) == PGRES_TUPLES_OK) for (int z = 0; z < PQntuples(o); z++) { lp_id ob; memcpy(ob.b, PQgetvalue(o, z, 0), 16); int dup = 0;
              for (int u = 0; u < nobs && !dup; u++) dup = !memcmp(&obs[u], &ob, 16); if (dup) continue;
              if (nobs == cobs) { cobs = cobs ? cobs * 2 : 256; obs = xrealloc(obs, sizeof(lp_id) * (size_t)cobs); } obs[nobs++] = ob; }
          PQclear(o); free(sb); }
      typedef struct { lp_id id; double pull; int strands; } Tug; Tug *tug = NULL; int ntug = 0, ctug = 0, nstr = 0;
      if (nobs) { uint8_t *ob = malloc(20 + 20 * (size_t)nobs); size_t ol = ids_param(ob, obs, (uint32_t)nobs); char fan2[24]; snprintf(fan2, sizeof fan2, "%d", 64);
          const char *ov[2] = { (const char *)ob, fan2 }; int oll[2] = { (int)ol, (int)strlen(fan2) }, of_[2] = { 1, 0 };
          PGresult *o = db_ask(pg, "SELECT i, path, rating, deviation, volatility FROM laplace_claims_each($1::blake3[], $2::bigint, '{0}'::smallint[])", 2, ov, oll, of_);     /* one call: every strand of every observation */
          if (PQresultStatus(o) != PGRES_TUPLES_OK) { fprintf(stderr, "fold: %s", PQerrorMessage(pg)); return 1; }
          for (int z = 0; z < PQntuples(o); z++) { int oi = (int)BE(PQgetvalue(o, z, 0), 8) - 1; if (oi < 0 || oi >= nobs) continue;
              Run rn = run_of((const uint8_t *)PQgetvalue(o, z, 1), (size_t)PQgetlength(o, z, 1)); lp_rating rt = { be_f64(PQgetvalue(o, z, 2)), be_f64(PQgetvalue(o, z, 3)), be_f64(PQgetvalue(o, z, 4)) };
              int at = -1; for (int k = 0; k < rn.n && at < 0; k++) if (!memcmp(&rn.id[k], &obs[oi], 16)) at = k;
              int other = rn.n == 2 ? 1 - at : rn.n >= 3 && at == 0 ? rn.n - 1 : rn.n >= 3 && at == rn.n - 1 ? 0 : -1;        /* the other end; an observation that is the predicate pulls nothing */
              if (at >= 0 && other >= 0) { nstr++; double c_ = lp_confidence(&rt, fw.k); int found = -1; for (int u = 0; u < ntug; u++) if (!memcmp(&tug[u].id, &rn.id[other], 16)) { found = u; break; }
                  if (found < 0) { if (ntug == ctug) { ctug = ctug ? ctug * 2 : 256; tug = xrealloc(tug, sizeof(Tug) * (size_t)ctug); } tug[ntug] = (Tug){ rn.id[other], 0, 0 }; found = ntug++; }
                  tug[found].pull += c_; tug[found].strands++; }
              free(rn.id); }
          PQclear(o); free(ob); }
      for (int u = 0; u < ntug; u++) for (int k = 0; k < np; k++) if (!memcmp(&tug[u].id, &ph[k], 16)) tug[u].pull = 0;     /* the prompt's own words pull on nothing */
      for (int u = 1; u < ntug; u++) { Tug x = tug[u]; int y = u; while (y > 0 && tug[y - 1].pull < x.pull) { tug[y] = tug[y - 1]; y--; } tug[y] = x; }
      for (int u = 0; u < ntug && u < 12; u++) reader_want(rd, &tug[u].id);
      printf("\npulls back %d strands of %d observations, through what holds the prompt's runs   (%.1f ms)\n", nstr, nobs, (now() - t) * 1000);
      for (int u = 0; u < ntug && u < 12 && tug[u].pull > 0; u++) { char *tx = reader_text(rd, &tug[u].id, 110); printf("%10.2f %8d   %s\n", tug[u].pull, tug[u].strands, tx); free(tx); }
      free(tug); free(obs);
      #undef BE
      PQclear(q); free(ab); }
    Claim *mine = NULL; int nmine = -1, capped = 0;                           /* what is attested of the prompt itself, fetched once */
    #define MINE() do { if (nmine < 0) { mine = claims_of(pg, &pr.id, fw.fan, fw.k, &nmine, &capped); nmine = refused(pg, c, &fw, mine, nmine); } } while (0)
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
            lp_id *keys = malloc(sizeof(lp_id) * (size_t)np); int nk = 0;
            for (int i = 0; i < np; i++) if (table_find(&ph[i])) keys[nk++] = ph[i];
            if (!nk) { memcpy(keys, ph, sizeof(lp_id) * (size_t)np); nk = np; }
            uint8_t *ab = malloc(20 + 20 * (size_t)nk); size_t al = ids_param(ab, keys, (uint32_t)nk);
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
            PGresult *q = db_ask(pg, "SELECT entity, path FROM laplace_containers($1::blake3[], '{}'::smallint[])", 1, v, l, f);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "containers: %s", PQerrorMessage(pg)); return 1; }
            typedef struct { lp_id *id; int n; lp_id in; } Rest; Rest *rest = malloc(sizeof(Rest) * (size_t)(PQntuples(q) + 1)); int nrest = 0, holders = 0;
            for (int j = 0; j < PQntuples(q); j++) {
                Run r = run_of((const uint8_t *)PQgetvalue(q, j, 1), (size_t)PQgetlength(q, j, 1)); int found = 0;
                for (int i = 0; i + np <= r.n; i++) if (!memcmp(&r.id[i], ph, sizeof(lp_id) * (size_t)np)) {          /* each place it holds the run */
                    Rest x = { malloc(sizeof(lp_id) * (size_t)(r.n - i - np + 1)), r.n - i - np }; memcpy(x.id, &r.id[i + np], sizeof(lp_id) * (size_t)x.n); memcpy(x.in.b, PQgetvalue(q, j, 0), 16);
                    rest = xrealloc(rest, sizeof(Rest) * (size_t)(nrest + 2)); rest[nrest++] = x; found = 1; }
                holders += found; free(r.id);
            }
            PQclear(q); free(ab); free(keys);
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
            lp_id seen[256]; int nseen = 0, head = 0;
            for (int i = 0; i < np && nseen < 256; i++) {
                if (!table_find(&ph[i]) || !memcmp(&ph[i], &pr.id, 16)) continue;                  /* a composition, and not the prompt over again */
                int dup = 0; for (int z = 0; z < nseen; z++) dup |= !memcmp(&seen[z], &ph[i], 16); if (dup) continue; seen[nseen++] = ph[i];
                int n, cap2; Claim *cl = claims_of(pg, &ph[i], fw.fan, fw.k, &n, &cap2); n = refused(pg, c, &fw, cl, n); take_top(cl, n, fw.take[s].n, &fw, &seed);
                if (!head) { printf("\nattested   of its constituents\n%10s %8s %6s %8s   %s\n", "confidence", "rating", "dev", "matches", "claim"); head = 1; }
                if (!n) { char *tx = reader_text(rd, &ph[i], 48); printf("%10s %8s %6s %8s   %s: nothing is attested of it\n", "", "", "", "", tx); free(tx); }
                for (int k = 0; k < n && k < fw.take[s].n; k++) { show_claim(rd, &cl[k], ""); took++; }
                free(cl);
            }
            if (head) printf("           (%.1f ms)\n", (now() - t) * 1000);
        }
    }
    printf("\n%d taken in %d step%s   %llu round trips for text   total %.1f ms\n", took, fw.ntake, fw.ntake == 1 ? "" : "s", (unsigned long long)reader_trips(rd), (now() - T) * 1000);
    free(mine); free(ph); reader_free(rd); PQfinish(pg);
    return 0;
}
