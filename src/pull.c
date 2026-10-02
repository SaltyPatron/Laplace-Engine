/* Reading the web of claims. An entity is named by its text and found by the ID computed here; everything attested
 * about it is the claims whose paths hold it, fetched through the container index with their standings, one set-based
 * fetch per step: O(log N) to find them, O(K) to read them. How hard a strand tugs back is its claim's confidence.
 * Nothing here changes a standing: querying reads scores, it does not move them.
 *
 * These are the lookups the forward program is made of: what is attested about an entity (hop), a claim with a part
 * left open (hop a b ?), a word up to its concepts and down into another language (translate, along the relations the
 * firmware names), and how far one entity is from another (degrees).
 *
 *   laplace text  text
 *   laplace hop   [-d conninfo] [-n N] [--firmware FILE] text | subject predicate object (? for a part left open)
 *   laplace translate [-d conninfo] [-n N] [--firmware FILE] word from to...
 *   laplace degrees [-d conninfo] [-n N] [--firmware FILE] [--batch B] from [to]
 * How a standing is read, how far a search walks, how many claims are read and what is refused are the firmware's
 * decisions (firmware.c), not the program's; --k, --fan, --hops and --per-hop are there to measure against it. */
#include "engine.h"
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
    char id[33]; lp_id_hex(&r->id, id); double x[4]; lp_coord_xyzm(&r->c, x);
    printf("%-10s %s   tier %d   (%.6f, %.6f, %.6f, %.6f)   hilbert %016llx   depth %.6f\n", label, id, r->tier, x[0], x[1], x[2], x[3],
           (unsigned long long)lp_hilbert4(&r->c),
           1.0 - sqrt(((double)r->c.m[0] * r->c.m[0] + (double)r->c.m[1] * r->c.m[1] + (double)r->c.m[2] * r->c.m[2] + (double)r->c.m[3] * r->c.m[3])) / LP_FIXED_ONE);
}

int cmd_text(int argc, char **argv){
    if (argc < 2) { fprintf(stderr, "usage: laplace text text\n"); return 2; }
    double t = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0);
    lp_ref parts[64]; size_t np; lp_ref r = entity_named(c, argv[1], parts, 64, &np);
    double ms = (now() - t) * 1000;
    uint8_t fp[32]; char hex[65]; lp_tier0_fingerprint(T0, fp); lp_hex(fp, 32, hex);
    show_ref("entity", &r);
    printf("%-10s %s\n", "wall", lp_coord_inside(&r.c) ? "inside" : "OUTSIDE");
    printf("%-10s %zu\n", "parts", np);
    for (size_t i = 0; i < np && i < 64; i++) { char label[16]; snprintf(label, sizeof label, "  [%zu]", i); show_ref(label, &parts[i]); }
    printf("%-10s %s\n", "tier 0", hex);
    printf("computed in %.2f ms, mapping tier 0 included; no database\n", ms);
    return 0;
}

/* ---- the claims that hold an entity */
int claim_by_conf(const void *a, const void *b){ double x = ((const Claim *)a)->conf, y = ((const Claim *)b)->conf; return x < y ? 1 : x > y ? -1 : memcmp(a, b, 16); }

/* The predicates the pass's firmware refuses, as the blake3[] every claim read passes: a restriction is applied before
 * the sort (Sequence 15.5), so a refused strand never takes a place the fan leaves. The firmware names them when it is
 * loaded; their IDs are computed once, on the first read. */
static Firmware pass;
void refuse_named(const Firmware *fw){ pass = *fw; pass.id.ready = 0; }
void arg_refused(Args *a){ firmware_ids(&pass); arg_ids(a, pass.id.refuse, (size_t)pass.nrefuse_predicate); }

/* A claim as a claim read returns it: its ID, its parts (at most MAXPARTS), its standing and how many matchups made it.
 * Columns: entity, path, rating, deviation, volatility[, matches]. */
static void claim_read(const PGresult *q, int j, int matches_col, Claim *x){
    memset(x, 0, sizeof *x); x->id = *col_id(q, j, 0);
    size_t np = lp_path_expand(col_path(q, j, 1), x->part, MAXPARTS); x->np = np < MAXPARTS ? (int)np : MAXPARTS;
    x->r = col_rating(q, j, 2); if (matches_col >= 0) x->matches = (int)col_int(q, j, matches_col);
}
/* In its place: the first part given is the claim's first, the last its last, and the middle one between them. */
static int in_place(const Claim *x, const lp_id *part, const int *have){
    int last = x->np - 1;
    if (have[0] > 1 && !lp_id_eq(&x->part[0], &part[0])) return 0;
    if (have[2] > 1 && !lp_id_eq(&x->part[last], &part[2])) return 0;
    if (have[1] > 1) { int in = 0; for (int i = 1; i < last || (i == 1 && x->np == 2 && i <= last); i++) if (lp_id_eq(&x->part[i], &part[1])) in = 1; if (!in) return 0; }
    return 1;
}
/* The claims that hold the given parts in their places (a part not given is open): at most fan of them; *capped says
 * there were more. k: how many deviations below its rating a claim is read at. */
Claim *claims_like(PGconn *pg, const lp_id *part, const int *have, int fan, double k, int *n, int *capped){
    lp_id keys[3]; size_t nk = 0; for (int i = 0; i < 3; i++) if (have[i]) keys[nk++] = part[i];
    Args a = { 0 }; arg_ids(&a, keys, nk); arg_int(&a, fan + 1); arg_text(&a, CLAIM_BITS); arg_refused(&a);
    PGresult *q = ask(pg, "SELECT entity, path, rating, deviation, volatility, matches FROM laplace_claims($1::blake3[], $2::bigint, $3::smallint[], $4::blake3[])", &a);
    int rows = PQntuples(q); *capped = rows > fan; if (rows > fan) rows = fan;
    Claim *c = malloc(sizeof(Claim) * (size_t)(rows ? rows : 1)); int m = 0;
    for (int j = 0; j < rows; j++) {
        claim_read(q, j, 5, &c[m]);
        if (c[m].np < 2 || !in_place(&c[m], part, have)) continue;
        c[m].conf = lp_confidence(&c[m].r, k); m++;
    }
    PQclear(q); args_free(&a); *n = m;
    lp_sort(c, (size_t)m, sizeof(Claim), claim_by_conf);
    return c;
}
/* Every claim that holds an entity, wherever in it. */
Claim *claims_of(PGconn *pg, const lp_id *e, int fan, double k, int *n, int *capped){
    lp_id part[3] = { *e, *e, *e }; int have[3] = { 1, 0, 0 };
    return claims_like(pg, part, have, fan, k, n, capped);
}
/* The claims of a set, by ID: the place each holds in it. */
static lp_idmap *claims_by_id(const Claim *c, int n){ lp_idmap *m = lp_idmap_new(); for (int i = 0; i < n; i++) lp_idmap_put(m, &c[i].id, NULL); return m; }
static void claim_ids(const Claim *c, int n, Ids *ids){ ids->n = 0; for (int i = 0; i < n; i++) lp_push(ids, c[i].id); }

/* The position each claim was given by the witnesses that gave one: the least, as recorded in the ledger. */
void positions_of(PGconn *pg, Claim *c, int n){
    if (!n) return;
    Ids ids = { 0 }; claim_ids(c, n, &ids); lp_idmap *at = claims_by_id(c, n);
    Args a = { 0 }; arg_ids(&a, ids.v, ids.n);
    PGresult *q = ask(pg, "SELECT claim, position FROM laplace_attested($1::blake3[]) WHERE position IS NOT NULL", &a);
    for (int j = 0; j < PQntuples(q); j++) {
        int64_t i = lp_idmap_find(at, col_id(q, j, 0)); int pos = (int)col_int(q, j, 1);
        if (i >= 0 && (!c[i].position || pos < c[i].position)) c[i].position = pos;
    }
    PQclear(q);
    /* A claim said within a record has its place in the record's own trajectory (Physicality: the trajectory records
     * the order of the constituents, so no ordinal is needed): the entry lists its senses in the order its witness
     * gave them. For the claims the ledger gave no place, the records that hold each are fetched as one set, and the
     * claim's place is where it stands in the path, the least when several records hold it. */
    int need = 0; for (int i = 0; i < n; i++) need += !c[i].position;
    if (need) {
        PGresult *r = ask(pg, "SELECT u.i, k.path FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i), LATERAL laplace_containers(ARRAY[u.id], '{1}'::smallint[]) k", &a);
        lp_vec(lp_vertex) vt = { 0 };
        for (int j = 0; j < PQntuples(r); j++) {
            int i = (int)col_int(r, j, 0) - 1; if (i < 0 || i >= n) continue;
            lp_path p = col_path(r, j, 1); if (!p.n) continue;
            lp_vec_reserve(&vt, p.n); lp_path_decode(p, vt.v, p.n);
            if (!lp_id_eq(&vt.v[0].id, &c[i].part[0])) continue;                  /* the trajectory under the claim's own subject: the order its witness gave */
            int place = 0, pos = 0; for (size_t z = 0; z < p.n && !place; z++) { if (lp_id_eq(&vt.v[z].id, &c[i].id)) place = pos + 1; pos += (int)vt.v[z].run; }
            if (place && (!c[i].position || place < c[i].position)) c[i].position = place;
        }
        PQclear(r); lp_vec_free(&vt);
    }
    args_free(&a); lp_vec_free(&ids); lp_idmap_free(at);
}
/* As given first, then by how hard the strand tugs back. */
int claim_by_position(const void *a, const void *b){
    const Claim *x = a, *y = b; int px = x->position ? x->position : 1 << 30, py = y->position ? y->position : 1 << 30;
    return px != py ? (px < py ? -1 : 1) : claim_by_conf(a, b);
}

/* The kinds of strand a firmware refuses, taken out of a set before it is used: a claim that holds a refused
 * predicate in its middle, and a claim only refused witnesses attested. Returns how many are left. */
int refused(PGconn *pg, Ctx *c, const Firmware *fwc, Claim *cl, int n){
    (void)c; Firmware *fw = (Firmware *)fwc;
    if (!n || (!fw->nrefuse_predicate && !fw->nrefuse_witness)) return n;
    firmware_ids(fw); uint8_t *out = calloc((size_t)n, 1);
    for (int i = 0; i < n; i++) {                                              /* a pair's middle is its second part */
        const lp_id *mid = cl[i].part + 1; size_t nm = cl[i].np == 2 ? 1 : (size_t)(cl[i].np - 2);
        for (size_t p = 0; p < nm && !out[i]; p++) for (int z = 0; z < fw->nrefuse_predicate; z++) if (lp_id_eq(&mid[p], &fw->id.refuse[z])) out[i] = 1;
    }
    if (fw->nrefuse_witness) {
        Ids ids = { 0 }; claim_ids(cl, n, &ids); lp_idmap *at = claims_by_id(cl, n);
        Args a = { 0 }; arg_ids(&a, ids.v, ids.n);
        PGresult *q = ask(pg, "SELECT claim, witness FROM laplace_attested($1::blake3[])", &a);
        uint8_t *other = calloc((size_t)n, 1), *theirs = calloc((size_t)n, 1);
        for (int j = 0; j < PQntuples(q); j++) {
            int64_t i = lp_idmap_find(at, col_id(q, j, 0)); if (i < 0) continue;
            int is = 0; for (int z = 0; z < fw->nrefuse_witness; z++) is |= lp_id_eq(col_id(q, j, 1), &fw->id.refuse_witness[z]);
            if (is) theirs[i] = 1; else other[i] = 1;
        }
        for (int i = 0; i < n; i++) if (theirs[i] && !other[i]) out[i] = 1;
        PQclear(q); args_free(&a); lp_vec_free(&ids); lp_idmap_free(at); free(other); free(theirs);
    }
    int m = 0; for (int i = 0; i < n; i++) if (!out[i]) cl[m++] = cl[i];
    free(out); return m;
}
/* Role weights (Semantics: Personality firmware, "Weights that are not standings"): a weight on which kind of strand is
 * allowed to pull. It is the firmware's, never a column of the claim: the standing is read as it is, and the reading is
 * multiplied by the weight the firmware gives the claim's kind (what stands between its first part and its last). */
void weights_named(Ctx *c, const Firmware *fw, lp_id *ids){ (void)c; firmware_ids((Firmware *)fw); memcpy(ids, fw->id.weigh, sizeof(lp_id) * (size_t)fw->nweigh); }
double strand_weight(const Firmware *fw, const lp_id *ids, const lp_id *part, int np){
    double w = 1.0;
    for (int p = 1; p < np - 1; p++) for (int z = 0; z < fw->nweigh; z++) if (lp_id_eq(&part[p], &ids[z]) && fw->weigh[z] < w) w = fw->weigh[z];
    return w;
}
void weighed(Ctx *c, const Firmware *fw, Claim *cl, int n){
    (void)c; if (!n || !fw->nweigh) return;
    firmware_ids((Firmware *)fw);
    for (int i = 0; i < n; i++) cl[i].conf *= strand_weight(fw, fw->id.weigh, cl[i].part, cl[i].np);
    lp_sort(cl, (size_t)n, sizeof(Claim), claim_by_conf);
}
int cmd_hop(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; int limit = 24, fan = -1; double k = -1;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "-n", 'i', &limit }, { "--firmware", 's', &fwp },
                                            { "--fan", 'i', &fan }, { "--k", 'd', &k }, { NULL } });       /* --fan, --k: to measure, over the firmware's */
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
    positions_of(pg, cl, n); if (!whole && fw.order_witness) lp_sort(cl, (size_t)n, sizeof(Claim), claim_by_position);
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
    t = now(); Args ca = { 0 }; arg_ids(&ca, &e.id, 1);
    PGresult *q = ask(pg, "SELECT tier FROM laplace_containers($1::blake3[], '{}'::smallint[]) WHERE NOT (mask ? 0::smallint)", &ca);
    uint64_t by_tier[256] = { 0 }; int any = 0;
    for (int j = 0; j < PQntuples(q); j++) { by_tier[col_int(q, j, 0) & 255]++; any = 1; }
    PQclear(q); args_free(&ca);
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
typedef struct { PGconn *pg; Reader *rd; const Firmware *fw; lp_idmap *lang; int lookups; } Translation;
typedef struct { char code[24]; } Lang;                                       /* by what holds an entity under the firmware's HELD */
/* The claims [first, middle, last] with whichever end is not given left open. */
static Claim *one_open(Translation *t, const lp_id *first, const lp_id *middle, const lp_id *last, int fan, int *n){
    lp_id p[3] = { first ? *first : *middle, *middle, last ? *last : *middle }; int h[3] = { first ? 2 : 0, 2, last ? 2 : 0 }, capped;
    t->lookups++; return claims_like(t->pg, p, h, fan, t->fw->k, n, &capped);
}
static const char *language_of(Translation *t, const lp_id *x){
    int n; Claim *lx = one_open(t, NULL, &t->fw->id.language[0], x, 8, &n); t->lookups--; if (!n) { free(lx); return ""; }
    lp_id h = lx[0].part[0]; free(lx);
    bool fresh; Lang *k = lp_idmap_get(t->lang, &h, &fresh); if (!fresh) return k->code;
    Claim *lg = one_open(t, &h, &t->fw->id.language[1], NULL, 8, &n); t->lookups--;
    if (n) { char *tx = reader_text(t->rd, &lg[0].part[lg[0].np - 1], 20); snprintf(k->code, sizeof k->code, "%s", tx); free(tx); }
    free(lg); return k->code;
}
/* Down from x along the steps up[from..0], in the other direction: the words at the bottom, at most cap of them. */
static int down(Translation *t, const lp_id *x, int from, int cap, int any){
    if (from < 0) { char *tx = reader_text(t->rd, x, 40); printf("%s%s", any ? ", " : " ", tx); free(tx); return 1; }
    int n, shown = 0; Claim *cl = one_open(t, NULL, &t->fw->id.up[from], x, from ? 256 : 8, &n);
    for (int i = 0; i < n && any + shown < cap; i++) shown += down(t, &cl[i].part[0], from - 1, cap, any + shown);
    free(cl); return shown;
}
int cmd_translate(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; int limit = 4;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "-n", 'i', &limit }, { "--firmware", 's', &fwp }, { NULL } });
    Firmware fw = firmware_for(fwp, FW_TRANSLATE); int fan = fw.fan;
    if (argc - a < 3) { fprintf(stderr, "usage: laplace translate [-d conninfo] [-n concepts] word from to...\n       languages as the resources write them: en de fr ja\n"); return 2; }
    if (fw.nup < 2 || !fw.language[0][0]) { fprintf(stderr, "%s: for translate, the firmware names no way up to a concept and back down (up RELATION..., language HELD SAYS)\n", fw.path); return 2; }
    double T = now(); tier0_open(NULL); Ctx *c = lp_text_new(T0); firmware_ids(&fw);
    Translation t = { db_connect(conninfo), NULL, &fw, lp_idmap_sized(sizeof(Lang)), 0 }; t.rd = reader_new(t.pg);
    lp_id word = entity_named(c, argv[a], NULL, 0, NULL).id; const char *from = argv[a + 1]; int n, shown = 0, last = fw.nup - 1;
    Claim *first = one_open(&t, &word, &fw.id.up[0], NULL, fan, &n); positions_of(t.pg, first, n); lp_sort(first, (size_t)n, sizeof(Claim), claim_by_position);
    lp_idmap *seen = lp_idmap_new();
    for (int i = 0; i < n && shown < limit; i++) {
        lp_id x = first[i].part[first[i].np - 1]; int ok = 1;
        for (int s = 1; s < last && ok; s++) { int ny; Claim *st = one_open(&t, &x, &fw.id.up[s], NULL, 8, &ny); if (ny) x = st[0].part[st[0].np - 1]; else ok = 0; free(st); }
        if (!ok || strcmp(language_of(&t, &x), from)) continue;
        int ni; Claim *il = one_open(&t, &x, &fw.id.up[last], NULL, 8, &ni); t.lookups += 2; if (!ni) { free(il); continue; }
        lp_id concept = il[0].part[il[0].np - 1]; lp_rating ir = il[0].r; free(il);
        bool fresh; lp_idmap_put(seen, &concept, &fresh); if (!fresh) continue;
        int nd = 0; Claim *df = fw.gloss[0] ? one_open(&t, &x, &fw.id.gloss, NULL, 8, &nd) : NULL;
        char *it = reader_text(t.rd, &concept, 60), *dt = nd ? reader_text(t.rd, &df[0].part[df[0].np - 1], 100) : strdup(""); free(df);
        printf("\n  %4.0f \xC2\xB1 %-3.0f  %s\n            %s\n", ir.rating, ir.deviation, it, dt); free(it); free(dt); shown++;
        int no; Claim *others = one_open(&t, NULL, &fw.id.up[last], &concept, fan, &no);
        for (int g = a + 2; g < argc; g++) {
            printf("    %-6s", argv[g]); int any = 0;
            for (int o = 0; o < no && any < 8; o++) { lp_id y = others[o].part[0];
                if (strcmp(language_of(&t, &y), argv[g])) continue;
                any += down(&t, &y, last - 1, 8, any); }
            if (!any) printf(" (nothing attested)");
            printf("\n");
        }
        free(others);
    }
    if (!shown) printf("%s: no concept of it is attested in a language written %s\n", argv[a], from);
    printf("\n%d lookups   total %.1f ms\n", t.lookups, (now() - T) * 1000);
    free(first); lp_idmap_free(seen); lp_idmap_free(t.lang); reader_free(t.rd); PQfinish(t.pg);
    return 0;
}

/* ---- degrees of separation: how far anything is from anything else
 * Best-first over claims, a batch of the nearest open entities per round trip: each entity in the batch gets its claims
 * through the container index, read at most K (--fan) of them, which is what keeps a step O(K). An entity that holds
 * more than K claims (a part of speech, a language) is reached and not read through. Between two entities the search
 * runs from both ends and stops when no open strand can beat the best chain found. */
typedef struct { lp_frontier *f; lp_id origin; } Side;
typedef struct { uint64_t trips, expanded, claims, hubs; } Work;

/* Close up to batch of the side's nearest open entities and reach across their claims. Returns how many it closed;
 * closed[] receives them. */
static int expand(PGconn *pg, Side *sd, int batch, int fan, int hops, double k, double per_hop, lp_reached *closed, uint8_t *hub, Work *w, const Firmware *way, const lp_id *wids){
    int n = 0; const lp_reached *x;
    while (n < batch && (x = lp_frontier_next(sd->f))) { hub[n] = 0; closed[n++] = *x; }
    Ids ids = { 0 }; lp_vec(int) who = { 0 };
    for (int i = 0; i < n; i++) if ((int)closed[i].hops < hops) { lp_push(&ids, closed[i].id); lp_push(&who, i); }
    if (!ids.n) return n;
    int m = (int)ids.n; Args a = { 0 }; arg_ids(&a, ids.v, ids.n); arg_int(&a, fan + 1); arg_text(&a, CLAIM_BITS); arg_refused(&a);
    PGresult *q = ask(pg, "SELECT i, entity, path, rating, deviation, volatility FROM laplace_claims_each($1::blake3[], $2::bigint, $3::smallint[], $4::blake3[])", &a);
    w->trips++; w->expanded += (uint64_t)m;
    int rows = PQntuples(q), *held = calloc((size_t)m, sizeof(int));
    for (int j = 0; j < rows; j++) held[col_int(q, j, 0) - 1]++;
    for (int j = 0; j < rows; j++) {
        int e = (int)col_int(q, j, 0) - 1; const lp_reached *at = &closed[who.v[e]];
        if (held[e] > fan && !lp_id_eq(&at->id, &sd->origin)) continue;            /* a hub: reached, not crossed */
        lp_path p = col_path(q, j, 2); lp_id part[3]; if (lp_path_len(p) != 3) continue;
        lp_path_expand(p, part, 3);
        int other = lp_tuple_other(part, 3, &at->id); if (other < 0) continue;      /* held as the predicate: it names the tie */
        lp_rating r = col_rating(q, j, 3); w->claims++;
        double sw = strand_weight(way, wids, part, 3); if (sw <= 0) continue;        /* a kind weighed at nothing is not crossed */
        lp_frontier_reach(sd->f, &part[other], &at->id, col_id(q, j, 1), at->cost + lp_cost(&r, k, per_hop) - log(sw), 0, at->hops + 1);
    }
    for (int e = 0; e < m; e++) if (held[e] > fan && !lp_id_eq(&ids.v[e], &sd->origin)) { w->hubs++; hub[who.v[e]] = 1; }
    PQclear(q); args_free(&a); lp_vec_free(&ids); lp_vec_free(&who); free(held);
    return n;
}

/* The tie between two neighbours of a chain, as text: the claim's predicate, read from its subject to its object. */
static void show_tie(Reader *rd, const lp_id *claim, const lp_id *left){
    lp_id part[4]; size_t np = reader_parts(rd, claim, part, 4);
    char *pt = np == 3 ? reader_text(rd, &part[1], 32) : strdup("?");
    printf(np == 3 && lp_id_eq(&part[0], left) ? " -[%s]-> " : " <-[%s]- ", pt); free(pt);
}
/* The chain from a side's origin to an entity, origin first, and the claim each was reached through. */
static int chain_to(const lp_frontier *f, const lp_id *id, lp_id *chain, lp_id *via, int cap){
    int n = 0; const lp_reached *r = lp_frontier_find(f, id);
    while (r && n < cap) { chain[n] = r->id; via[n] = r->claim; n++; if (!r->hops) break; r = lp_frontier_find(f, &r->from); }
    for (int i = 0; i < n / 2; i++) { lp_id t = chain[i]; chain[i] = chain[n - 1 - i]; chain[n - 1 - i] = t; t = via[i]; via[i] = via[n - 1 - i]; via[n - 1 - i] = t; }
    return n;                                                                     /* via[i]: the claim chain[i] was reached through from chain[i - 1] */
}
static void show_chain(Reader *rd, const lp_id *chain, const lp_id *via, int n, double cost, double per_hop){
    for (int i = 0; i < n; i++) reader_want(rd, &chain[i]);
    printf("%10.3g %5d   ", exp(-(cost - per_hop * (n - 1))), n - 1);
    for (int i = 0; i < n; i++) {
        if (i) show_tie(rd, &via[i], &chain[i - 1]);
        char *tx = reader_text(rd, &chain[i], 48); printf("%s", tx); free(tx);
    }
    printf("\n");
}
static int by_cost(const void *a, const void *b){ double x = ((const lp_reached *)a)->cost, y = ((const lp_reached *)b)->cost; return x < y ? -1 : x > y; }

int cmd_degrees(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL; int limit = 24, fan = -1, hops = -1, batch = 64; double k = -1, per_hop = -1;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "--firmware", 's', &fwp }, { "-n", 'i', &limit }, { "--fan", 'i', &fan },
                                            { "--hops", 'i', &hops }, { "--batch", 'i', &batch }, { "--k", 'd', &k }, { "--per-hop", 'd', &per_hop }, { NULL } });
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
        lp_vec(lp_reached) near = { 0 };
        for (;;) {
            if (near.n >= (size_t)limit) {                                    /* the N nearest are known once nothing open is nearer */
                lp_sort(near.v, near.n, sizeof *near.v, by_cost);
                if (lp_frontier_least(fw.f) >= near.v[limit - 1].cost) break;
            }
            int n = expand(pg, &fw, batch, fan, hops, k, per_hop, closed, hub, &w, &way, wids); if (!n) break;
            for (int i = 0; i < n; i++) if (!lp_id_eq(&closed[i].id, &from.id)) lp_push(&near, closed[i]);
        }
        double t_search = (now() - T) * 1000;
        lp_sort(near.v, near.n, sizeof *near.v, by_cost);
        printf("\n%10s %5s   %s\n", "confidence", "hops", "reached");
        for (size_t i = 0; i < near.n && i < (size_t)limit; i++) { lp_id chain[66], via[66]; int n = chain_to(fw.f, &near.v[i].id, chain, via, 66); show_chain(rd, chain, via, n, near.v[i].cost, per_hop); }
        printf("\n%llu entities expanded in %llu round trips, %llu claims crossed, %zu entities reached, %llu not read through; search %.1f ms, total %.1f ms\n",
               (unsigned long long)w.expanded, (unsigned long long)w.trips, (unsigned long long)w.claims, lp_frontier_count(fw.f), (unsigned long long)w.hubs, t_search, (now() - T) * 1000);
        lp_vec_free(&near);
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
            printf("\n%10s %5s   %s\n", "confidence", "hops", "chain"); show_chain(rd, chain, via, n, best, per_hop);
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
/* How hard a word pulls (its role): what is attested of it under the firmware's role kind ([word, role_by, x], read
 * in the claims' order), the weight the firmware gives the first x it names. -1 when nothing it names is attested. */
double role_of(PGconn *pg, Firmware *fw, const lp_id *word){
    if (!fw->role_by[0]) return -1;
    firmware_ids(fw); lp_id part[3] = { *word, fw->id.role_by, fw->id.role_by }; int have[3] = { 2, 2, 0 }, n, capped; double w = -1;
    Claim *cl = claims_like(pg, part, have, fw->fan, fw->k, &n, &capped);
    for (int q = 0; q < n && w < 0; q++) for (int z = 0; z < fw->nrole; z++) if (lp_id_eq(&cl[q].part[cl[q].np - 1], &fw->id.role[z])) { w = fw->role[z]; break; }
    free(cl); return w;
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
int chain_follow(PGconn *pg, Reader *rd, Firmware *fw, const lp_id *word, const lp_id *reading, unsigned *seed, lp_id *answer, Claim *last, int *alt){
    firmware_ids(fw); int steps = 0;
    for (*alt = 0; *alt < fw->nalt; (*alt)++) {
        lp_id cur = *word; steps = 0;
        for (int z = 0; z < fw->nchain[*alt]; z++) {
            lp_id tryv[66]; int nt = 0; tryv[nt++] = cur;
            if (z > 0 && lp_tier0_codepoint(T0, &cur) < 0) {
                lp_id parts[64]; size_t np = reader_parts(rd, &cur, parts, 64);
                for (size_t k = np; k-- > 0 && nt < 65; ) if (lp_tier0_codepoint(T0, &parts[k]) < 0 && !lp_id_eq(&parts[k], word)) tryv[nt++] = parts[k];
                tryv[nt++] = *word;                                                  /* a synset of one member is that word: what is said of it is said of the word */
            }
            Claim *cl = NULL; int n = 0, capped;
            for (int t = 0; t < nt && !n; t++) { lp_id part[3] = { tryv[t], fw->id.chain[*alt][z], fw->id.chain[*alt][z] }; int have[3] = { 2, 2, 0 };
                cl = claims_like(pg, part, have, fw->fan, fw->k, &n, &capped); if (!n) { free(cl); cl = NULL; } }     /* a relation the firmware names to follow is not one it refuses */
            if (!n) break;
            positions_of(pg, cl, n); if (fw->order_witness) lp_sort(cl, (size_t)n, sizeof(Claim), claim_by_position);
            int take = -1;
            if (z == 0 && reading) for (int k = 0; k < n; k++) if (lp_id_eq(&cl[k].part[cl[k].np - 1], reading)) { take = k; break; }
            if (take < 0) { take_top(cl, n, 1, fw, seed); take = 0; }
            *last = cl[take]; cur = cl[take].part[cl[take].np - 1]; steps++; free(cl);
        }
        if (steps == fw->nchain[*alt]) { *answer = cur; return steps; }
    }
    *alt = -1; return steps;
}
