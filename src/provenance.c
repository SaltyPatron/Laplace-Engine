/* Provenance by containment (Engine#22; Semantics: Attestations, Witnesses and Outcomes). Who said a claim, how many
 * times and how, is not a row: a record is a path over the claims it says, in its file's content tree, under the file's
 * trunk, under the source's trunk, which is the witness (say.c, records_of). Every read of provenance is a walk:
 *
 *   up      from claims, one set read a level through the container index (every leaf at once), to the records that
 *           say them, the files and trunks above; a trunk in the witness table is a witness, with its trust. The count
 *           is read off the tree: games, the records under the trunk that say the claim (a record repeated is a game
 *           each time); tokens, with the times each says it; the outcome and position each record gave it, from its
 *           vertex's M (lp_m_outcome, lp_m_position).
 *   down    from a trunk, to every claim its records say, the same counts (replay, forget).
 *
 *   laplace held [-d conninfo] [--files] [--voices] [--tsv FILE] PART...
 *       The claims whose parts are these (each PART a text, LIST:KEY for a type of the highway's LIST, ? for a part
 *       left open), found as one set read over the claims, and who said each: a walk up. --files: the count under each
 *       file too; --voices: under each voice (who in a record says it). --tsv: one line a claim and trunk (file:NAME,
 *       voice:ID): claim, who, games, tokens, the sum of the scores, the least position.
 *
 *   laplace replay [-d conninfo] [--write] [--tsv FILE]
 *       Every witness's trunk, in the order the sources go in (recipes/order), walked down to the claims its records
 *       say; each claim's series played as the ingest plays it (once a source, at the witness's trust, from the stock
 *       default), and the standings compared bit for bit with the ones recorded. --write puts the replayed standings in
 *       place. --tsv: claim, trunk, games, tokens, the sum of the scores, the least position.
 *
 * SQL fetches sets of paths; the walks, the counts and the plays are here. */
#include "engine.h"
#include "walk.h"
#include <locale.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ---- the order the sources go in, by the trunk's record */
static int rank_of(Reader *rd, const lp_id *trunk){
    static Source *src; static int nsrc = -1; if (nsrc < 0) { Recipe *rec = NULL; recipes_load(laplace_recipes(), &rec); src = sources_loaded(&nsrc); }
    lp_id first[2]; size_t m = reader_parts(rd, trunk, first, 2); if (!m) return 1 << 30;
    char *name = reader_text(rd, &first[0], 200); int r = 1 << 30;
    for (int k = 0; k < nsrc && r == 1 << 30; k++) { const char *c = source_called(&src[k]); if (c && !strcmp(c, name)) r = k; }
    free(name); return r;
}
static int by_rank(const void *a, const void *b){ const Root *x = a, *y = b; return x->rank != y->rank ? (x->rank < y->rank ? -1 : 1) : memcmp(&x->id, &y->id, 16); }
static void roots_known(PGconn *pg, Root *r, size_t n, Reader *rd){
    witnesses_known(pg, r, n);
    for (size_t k = 0; k < n; k++) r[k].rank = rd && r[k].witness ? rank_of(rd, &r[k].id) : 1 << 30;
}


/* ---- the strands named */
static lp_id *strands_named(int argc, char **argv, int a, int *ns, Hold **hs, int *nhs){
    Ctx *c = lp_text_new(T0); if (!HW) HW = lp_highway_map(NULL);
    int np = argc - a; lp_id part[MAXPARTS]; int have[MAXPARTS], nk = 0; lp_id key[MAXPARTS];
    for (int i = 0; i < np && i < MAXPARTS; i++) { const char *p = argv[a + i]; have[i] = strcmp(p, "?") != 0; if (!have[i]) continue;
        const char *colon = strchr(p, ':'); int typed = 0;
        if (colon && colon > p && colon[1]) { char list[64]; snprintf(list, sizeof list, "%.*s", (int)(colon - p), p); Ref r = highway_typed(list, (const uint8_t *)colon + 1, strlen(colon + 1), &typed); if (typed) part[i] = r.id; }
        if (!typed) part[i] = lp_text_decompose(c, (const uint8_t *)p, strlen(p), NULL, NULL).id;
        key[nk++] = part[i];
        char hx[33]; id_text(&part[i], hx); printf("  part %d   %-24s %s%s\n", i + 1, p, hx, typed ? "   (a type of the highway)" : ""); }
    int nh = 0; Hold *h = holds_above(key, nk, -1, 0, 1, &nh); int w = 0; Ids ids = { 0 };
    for (int j = 0; j < nh; j++) { if (!h[j].claim) continue; lp_path p = lp_path_of(h[j].path, (size_t)h[j].path_len); size_t n = path_into(&ids, p);
        if ((int)n != np) continue; int fits = 1; for (int i = 0; i < np && fits; i++) if (have[i] && memcmp(&ids.v[i], &part[i], 16)) fits = 0;
        if (fits) { Hold x = h[j]; h[j] = h[w]; h[w++] = x; } }
    lp_vec_free(&ids);
    lp_id *out = malloc(sizeof(lp_id) * (size_t)(w ? w : 1)); for (int j = 0; j < w; j++) out[j] = h[j].entity;
    *ns = w; *hs = h; *nhs = nh; return out;
}
static int by_hold_id(const void *a, const void *b){ return memcmp(&((const Hold *)a)->entity, &((const Hold *)b)->entity, 16); }
/* A file's path, from the OS's record in its metadata tree: [pathname, its folders], or [filename, NAME] */
static char *file_name(Reader *rd, const lp_id *file){
    lp_id at = *file, kid[64];
    for (int depth = 0; depth < 8; depth++) {
        size_t n = reader_parts(rd, &at, kid, 64); if (!n) break;
        for (size_t i = 0; i < n && i < 64; i++) { lp_id two[4]; size_t m = reader_parts(rd, &kid[i], two, 4); if (m != 2) continue;
            char *k = reader_text(rd, &two[0], 32); int path = !strcmp(k, "pathname"), name = !strcmp(k, "filename"); free(k);
            if (name) return reader_text(rd, &two[1], 300);
            if (path) { lp_id seg[128]; size_t ns = reader_parts(rd, &two[1], seg, 128); char *o = calloc(1, 4096); size_t ol = 0;
                for (size_t z = 0; z < ns && z < 128; z++) { char *t = reader_text(rd, &seg[z], 300); ol += (size_t)snprintf(o + ol, ol < 4096 ? 4096 - ol : 0, "%s%s", z ? "/" : "", t); free(t); if (ol >= 4095) break; }
                return o; } }
        at = kid[0]; }
    return strdup("(a file)");
}
static void tsv_row(FILE *out, const lp_id *claim, const char *who, const Tally *t){
    char ch[33]; id_text(claim, ch); fprintf(out, "%s\t%s\t%.0f\t%.0f\t%.17g\t%u\n", ch, who, t->games, t->tokens, t->score, t->position);
}
/* What the claims under one node were said, as TSV rows (one a claim, who; and one a claim and voice, voice:ID): those
 * of `only` where it is given. The games and tokens added to tg, tt. */
static void rows_under(const Dag *d, size_t top, const char *who, const lp_idmap *only, int voices, FILE *out, double *tg, double *tt){
    double *cnt = down_from(d, top); lp_idmap *pc = lp_idmap_sized(sizeof(Tally)), *pv = voices ? lp_idmap_sized(sizeof(VTally)) : NULL; tally_under(d, cnt, pc, pv);
    for (size_t i = 0; i < lp_idmap_count(pc); i++) { const lp_id *cl = &lp_idmap_keys(pc)[i]; const Tally *t = lp_idmap_at(pc, i);
        if (t->games <= 0 || (only && lp_idmap_find((lp_idmap *)only, cl) < 0)) continue;
        if (tg) *tg += t->games; if (tt) *tt += t->tokens; if (out) tsv_row(out, cl, who, t); }
    if (pv) for (size_t i = 0; i < lp_idmap_count(pv); i++) { const VTally *v = lp_idmap_at(pv, i);
        if (v->t.games <= 0 || (only && lp_idmap_find((lp_idmap *)only, &v->claim) < 0)) continue;
        char vh[48], vx[33]; id_text(&v->voice, vx); snprintf(vh, sizeof vh, "voice:%s", vx); if (out) tsv_row(out, &v->claim, vh, &v->t); }
    lp_idmap_free(pc); if (pv) lp_idmap_free(pv); free(cnt);
}

int cmd_held(int argc, char **argv){
    const char *conninfo = laplace_db(), *tsv = NULL; int files = 0, voices = 0, byid = 0;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "--files", 'b', &files }, { "--voices", 'b', &voices }, { "--tsv", 's', &tsv }, { "--id", 'b', &byid }, { NULL } });
    if (a >= argc) { fprintf(stderr, "usage: laplace held [-d conninfo] [--files] [--voices] [--tsv FILE] PART...   (LIST:KEY a highway type, ? open)\n"); return 2; }
    setlocale(LC_NUMERIC, "en_US.UTF-8"); tier0_open(NULL);
    PGconn *pg = db_connect(conninfo); double T = now();
    printf("laplace held   %s   a walk up the container index, from the claims to the trunks that hold them\n", PQdb(pg));
    int ns = 0, nhs = 0; Hold *hs = NULL; lp_id *s;
    if (byid) { ns = argc - a; nhs = ns; s = malloc(sizeof(lp_id) * (size_t)(ns ? ns : 1)); hs = calloc((size_t)(ns ? ns : 1), sizeof(Hold));      /* the claims by their IDs */
        for (int i = 0; i < ns; i++) { if (!id_parse(argv[a + i], &s[i])) { fprintf(stderr, "%s: not an ID\n", argv[a + i]); return 2; } hs[i].entity = s[i]; } }
    else s = strands_named(argc, argv, a, &ns, &hs, &nhs);
    qsort(hs, (size_t)ns, sizeof(Hold), by_hold_id); for (int i = 0; i < ns; i++) s[i] = hs[i].entity;
    printf("  %'d claims of that shape, found as one set read over the claims (%.1f ms)\n", ns, (now() - T) * 1000);
    FILE *out = tsv ? fopen(tsv, "w") : NULL; if (tsv && !out) { perror(tsv); return 1; }
    Reader *rd = reader_new(pg); int show = ns <= 8;
    double t0 = now(); uint64_t rows = 0; Dag d = { 0 }; int levels = walk_up(&d, s, (size_t)ns, &rows); double t_walk = now() - t0;
    if (show) for (size_t k = 0; k < d.ne; k++) { const Edge *e = &d.e[k]; char ph[33], ch[33]; id_text(&d.n[e->p].id, ph); id_text(&d.n[e->c].id, ch);
        printf("  said in %s (%s, %d holders)   claim %.12s   times %llu   outcome %u   position %u%s\n", ph, e->rec ? "its own record" : "a record", d.n[e->p].nparents, ch, (unsigned long long)e->times, e->outcome, e->position, e->voiced ? "   voiced" : ""); }
    lp_vec(Root) roots = { 0 }; lp_vec(size_t) rat = { 0 }, fl = { 0 };
    for (size_t i = (size_t)ns; i < d.nn; i++) { if (!d.n[i].nparents) { lp_push(&roots, (Root){ d.n[i].id, 0, 0, 0 }); lp_push(&rat, i); } else if (d.n[i].metadata) lp_push(&fl, i); }
    roots_known(pg, roots.v, roots.n, rd);
    double tg = 0, tt = 0; lp_idmap *idx = lp_idmap_new(); for (int i = 0; i < ns; i++) lp_idmap_put(idx, &s[i], NULL);
    for (size_t r = 0; r < roots.n; r++) {
        char hx[33]; id_text(&roots.v[r].id, hx); lp_id first[2]; size_t m = reader_parts(rd, &roots.v[r].id, first, 2); char *tx = m ? reader_text(rd, &first[0], 80) : strdup("?");
        if (show || roots.n <= 6) { printf("  trunk %s   \"%s\"   %s", hx, tx, roots.v[r].witness ? "the witness" : "NOT a witness (no row in the witness table)"); if (roots.v[r].witness) printf(", trust %.3f", roots.v[r].trust); printf("\n"); }
        free(tx);
        rows_under(&d, rat.v[r], hx, idx, voices, out, &tg, &tt);
        if (show) { double *cnt = down_from(&d, rat.v[r]); lp_idmap *pc = lp_idmap_sized(sizeof(Tally)), *pv = lp_idmap_sized(sizeof(VTally)); tally_under(&d, cnt, pc, pv);
            for (int i = 0; i < ns; i++) { const Tally *t = lp_idmap_lookup(pc, &s[i]); if (!t || t->games <= 0) continue; char *ct = reader_text(rd, &s[i], 120);
                printf("\n  %s\n    standing %.6f / %.6f / %.6f, %d matches\n    by the trunk %.12s…   games %.0f (records that say it)   tokens %.0f   score %.6g   position ",
                       ct, hs[i].r.rating, hs[i].r.deviation, hs[i].r.volatility, hs[i].matches, hx, t->games, t->tokens, t->score / t->games); if (t->position) printf("%u\n", t->position); else printf("none\n"); free(ct);
                for (size_t k = 0; k < lp_idmap_count(pv); k++) { const VTally *v = lp_idmap_at(pv, k); if (memcmp(&v->claim, &s[i], 16) || v->t.games <= 0) continue; char *vt = reader_text(rd, &v->voice, 80);
                    printf("      said by the voice %-40s games %.0f   tokens %.0f\n", vt, v->t.games, v->t.tokens); free(vt); } }
            lp_idmap_free(pc); lp_idmap_free(pv); free(cnt); }
    }
    if (files) for (size_t f = 0; f < fl.n; f++) {
        char *fname = file_name(rd, &d.n[fl.v[f]].id); char *who = malloc(strlen(fname) + 8); sprintf(who, "file:%s", fname);
        rows_under(&d, fl.v[f], who, idx, 0, out, NULL, NULL);
        if (show) { double *cnt = down_from(&d, fl.v[f]); lp_idmap *pc = lp_idmap_sized(sizeof(Tally)); tally_under(&d, cnt, pc, NULL);
            for (int i = 0; i < ns; i++) { const Tally *t = lp_idmap_lookup(pc, &s[i]); if (t && t->games > 0) printf("      under %-60s games %.0f   tokens %.0f\n", fname, t->games, t->tokens); }
            lp_idmap_free(pc); free(cnt); }
        free(fname); free(who); }
    printf("\n  walked up %d levels: %'zu nodes, %'zu edges, %'llu holder rows read, %zu trunks at the top (%.1f ms)\n", levels, d.nn, d.ne, (unsigned long long)rows, roots.n, t_walk * 1000);
    printf("  %'d claims: %'.0f games, %'.0f tokens   %.1f ms in all\n", ns, tg, tt, (now() - T) * 1000);
    if (out) fclose(out);
    lp_idmap_free(idx); lp_vec_free(&roots); lp_vec_free(&rat); lp_vec_free(&fl); dag_free(&d);
    reader_free(rd); holds_free(hs, nhs); free(s); PQfinish(pg);
    return 0;
}

/* ---- the standings, played again from containment. A claim's standing is the stock default moved by each witness's
 * series in the order the sources go in, as the ingest moves it: once a source, at the witness's trust. */
typedef struct { lp_rating r; uint32_t matches; } Stand;
static void play(lp_idmap *stand, const lp_id *claim, double trust, const Tally *t){
    bool fresh; Stand *s = lp_idmap_get(stand, claim, &fresh); if (fresh) s->r = lp_rating_stock();
    lp_attest_series(&s->r, trust, (uint32_t)t->games, t->score / t->games, LP_ATTEST_FLOOR); s->matches += (uint32_t)t->games;
}
/* The replayed standings against the recorded, bit for bit; --write puts them in place. */
static int stands_compare(PGconn *pg, lp_idmap *stand, int write, uint64_t *same, uint64_t *differ, uint64_t *missing){
    size_t n = lp_idmap_count(stand); const lp_id *ids = lp_idmap_keys(stand);
    for (size_t i0 = 0; i0 < n; i0 += 50000) { size_t k = n - i0 < 50000 ? n - i0 : 50000;
        Args q = { 0 }; arg_ids(&q, ids + i0, k); PGresult *r = ask_once(pg, "SELECT claim, rating, deviation, volatility, matches FROM consensus WHERE claim = ANY($1::blake3[])", &q);
        lp_idmap *got = lp_idmap_new();
        for (int j = 0; j < PQntuples(r); j++) { const lp_id *cl = col_id(r, j, 0); lp_idmap_put(got, cl, NULL); const Stand *s = lp_idmap_lookup(stand, cl); if (!s) continue;
            double a3[3] = { col_f64(r, j, 1), col_f64(r, j, 2), col_f64(r, j, 3) };
            if (!memcmp(&a3[0], &s->r.rating, 8) && !memcmp(&a3[1], &s->r.deviation, 8) && !memcmp(&a3[2], &s->r.volatility, 8) && (uint32_t)col_int(r, j, 4) == s->matches) (*same)++; else (*differ)++; }
        for (size_t i = i0; i < i0 + k; i++) if (lp_idmap_find(got, &ids[i]) < 0) (*missing)++;
        PQclear(r); args_free(&q); lp_idmap_free(got); }
    if (!write || !n) return 0;
    exec(pg, "BEGIN"); exec(pg, "CREATE TEMP TABLE replayed (claim blake3, rating float8, deviation float8, volatility float8, matches int) ON COMMIT DROP");
    lp_copy c; copy_open(&c, pg, "COPY replayed FROM STDIN (FORMAT binary)");
    for (size_t i = 0; i < n; i++) { const Stand *s = lp_idmap_at(stand, i);
        lp_copy_row(&c, 5); lp_copy_id(&c, &ids[i]); lp_copy_f64(&c, s->r.rating); lp_copy_f64(&c, s->r.deviation); lp_copy_f64(&c, s->r.volatility); lp_copy_i32(&c, (int32_t)s->matches); }
    copy_close(&c);
    exec(pg, "UPDATE consensus p SET rating = r.rating, deviation = r.deviation, volatility = r.volatility, matches = r.matches FROM replayed r WHERE p.claim = r.claim");
    exec(pg, "INSERT INTO consensus SELECT r.* FROM replayed r WHERE NOT EXISTS (SELECT 1 FROM consensus p WHERE p.claim = r.claim)");
    exec(pg, "COMMIT"); return 0;
}
int cmd_replay(int argc, char **argv){
    const char *conninfo = laplace_db(), *tsv = NULL; int write = 0, voices = 0, files = 0;
    opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "--write", 'b', &write }, { "--tsv", 's', &tsv }, { "--voices", 'b', &voices }, { "--files", 'b', &files }, { NULL } });
    setlocale(LC_NUMERIC, "en_US.UTF-8"); tier0_open(NULL);
    PGconn *pg = db_connect(conninfo); double T = now(); Reader *rd = reader_new(pg);
    printf("laplace replay   %s   standings from containment alone: each witness's trunk walked down to the claims its records say\n", PQdb(pg));
    PGresult *wr = ask_once(pg, "SELECT id, trust FROM witness", NULL); size_t nw = (size_t)PQntuples(wr);
    Root *w = calloc(nw + 1, sizeof(Root)); for (size_t i = 0; i < nw; i++) { w[i].id = *col_id(wr, (int)i, 0); w[i].trust = col_f64(wr, (int)i, 1); w[i].witness = 1; w[i].rank = rank_of(rd, &w[i].id); }
    PQclear(wr); qsort(w, nw, sizeof(Root), by_rank);
    FILE *out = tsv ? fopen(tsv, "w") : NULL; if (tsv && !out) { perror(tsv); return 1; }
    lp_idmap *stand = lp_idmap_sized(sizeof(Stand)); uint64_t fetched = 0, played = 0; double tg = 0, tt = 0;
    for (size_t i = 0; i < nw; i++) {
        double t = now(); Dag d = { 0 }; int levels = walk_down(pg, &w[i].id, &d, &fetched);
        double *cnt = down_from(&d, (size_t)lp_idmap_find(d.m, &w[i].id)); lp_idmap *pc = lp_idmap_sized(sizeof(Tally)), *pv = voices ? lp_idmap_sized(sizeof(VTally)) : NULL; tally_under(&d, cnt, pc, pv);
        char hx[33]; id_text(&w[i].id, hx); lp_id first[2]; size_t m = reader_parts(rd, &w[i].id, first, 2); char *name = m ? reader_text(rd, &first[0], 60) : strdup("?");
        for (size_t k = 0; k < lp_idmap_count(pc); k++) { const Tally *x = lp_idmap_at(pc, k); if (x->games <= 0) continue;
            play(stand, &lp_idmap_keys(pc)[k], w[i].trust, x); played++; tg += x->games; tt += x->tokens; if (out) tsv_row(out, &lp_idmap_keys(pc)[k], hx, x); }
        if (pv && out) for (size_t k = 0; k < lp_idmap_count(pv); k++) { const VTally *v = lp_idmap_at(pv, k); if (v->t.games <= 0) continue; char vh[48], vx[33]; id_text(&v->voice, vx); snprintf(vh, sizeof vh, "voice:%s", vx); tsv_row(out, &v->claim, vh, &v->t); }
        if (files && out) for (size_t k = 0; k < d.nn; k++) if (d.n[k].metadata && d.n[k].nparents) { char *fname = file_name(rd, &d.n[k].id); char *who = malloc(strlen(fname) + 8); sprintf(who, "file:%s", fname); rows_under(&d, k, who, NULL, 0, out, NULL, NULL); free(fname); free(who); }
        printf("  %-40s %s   trust %.3f   %d levels down, %'zu nodes, %'zu claims said   (%.1f s)\n", name, hx, w[i].trust, levels, d.nn, lp_idmap_count(pc), now() - t);
        free(name); free(cnt); lp_idmap_free(pc); if (pv) lp_idmap_free(pv); dag_free(&d); }
    uint64_t same = 0, differ = 0, missing = 0; stands_compare(pg, stand, write, &same, &differ, &missing);
    printf("\n  %'llu claim-witness series played from %'llu paths fetched; %'.0f games, %'.0f tokens\n", (unsigned long long)played, (unsigned long long)fetched, tg, tt);
    printf("  replayed against recorded: %'llu identical bit for bit, %'llu different, %'llu with no standing recorded%s\n", (unsigned long long)same, (unsigned long long)differ, (unsigned long long)missing, write ? "   (written)" : "");
    printf("  %.1f s\n", now() - T);
    if (out) fclose(out);
    lp_idmap_free(stand); free(w); reader_free(rd); PQfinish(pg);
    return differ || missing ? 1 : 0;
}

/* ---- for forget: the standings of claims played again from the witnesses that still hold them (a walk up), in the
 * order the sources go in. */
int standings_replay(PGconn *pg, const lp_id *claims, size_t n, uint64_t *played, uint64_t *orphaned){
    Reader *rd = reader_new(pg); lp_idmap *stand = lp_idmap_sized(sizeof(Stand)); lp_idmap *wrank = lp_idmap_sized(sizeof(int));
    for (size_t i0 = 0; i0 < n; i0 += 20000) { int k = (int)(n - i0 < 20000 ? n - i0 : 20000), na = 0;
        Attested *a = attested(pg, claims + i0, k, &na, NULL);
        for (int j = 0; j < na; j++) if (a[j].witness) { bool f; int *r = lp_idmap_get(wrank, &a[j].trunk, &f); if (f) *r = rank_of(rd, &a[j].trunk); }
        /* each claim's series in the order its witnesses' sources go in */
        for (int x = 1; x < na; x++) { Attested y = a[x]; int z = x; int ry = y.witness ? *(int *)lp_idmap_lookup(wrank, &y.trunk) : 1 << 30;
            while (z > 0) { const Attested *p = &a[z - 1]; int c = memcmp(&p->claim, &y.claim, 16); int rp = p->witness ? *(int *)lp_idmap_lookup(wrank, &p->trunk) : 1 << 30;
                if (c < 0 || (c == 0 && (rp < ry || (rp == ry && memcmp(&p->trunk, &y.trunk, 16) <= 0)))) break; a[z] = a[z - 1]; z--; }
            a[z] = y; }
        for (int j = 0; j < na; j++) if (a[j].witness && a[j].games > 0) { Tally t = { a[j].games, a[j].tokens, a[j].score, a[j].position }; play(stand, &a[j].claim, a[j].trust, &t); (*played)++; }
        free(a); }
    uint64_t same = 0, differ = 0, missing = 0; stands_compare(pg, stand, 1, &same, &differ, &missing);
    /* a claim no witness holds any more has no standing */
    lp_vec(lp_id) gone = { 0 }; for (size_t i = 0; i < n; i++) if (lp_idmap_find(stand, &claims[i]) < 0) lp_push(&gone, claims[i]);
    for (size_t i0 = 0; i0 < gone.n; i0 += 50000) { size_t k = gone.n - i0 < 50000 ? gone.n - i0 : 50000; Args q = { 0 }; arg_ids(&q, gone.v + i0, k);
        PQclear(ask_once(pg, "DELETE FROM consensus WHERE claim = ANY($1::blake3[])", &q)); args_free(&q); }
    *orphaned = gone.n; lp_vec_free(&gone);
    lp_idmap_free(stand); lp_idmap_free(wrank); reader_free(rd); return 0;
}
