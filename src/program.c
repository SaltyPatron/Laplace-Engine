/* The forward program: one stateful program answers a turn of a session (Sequence: Forward, Sessions).
 *
 *   laplace turn [-d conninfo] [--firmware FILE] [--as USER] [--session NAME] [--seed N] [--read] prompt
 *
 * RESOLVE   the prompt is admitted as content, decomposed on the client; its occurrences are its constituents in
 *           order; the session is resolved from the record: its turns are the claims [session, turn] its user
 *           witnessed, in the order their ledger positions give, and their constituents are the discourse.
 * COUPLE    the whole observation perturbs the web at once: every segment of it, the observations that hold each run
 *           and what follows the run in them; every strand that holds an occurrence, the prompt or a discourse
 *           entity, read at the firmware's k. The field keeps its kinds apart: what responds, by which route, from
 *           which occurrences, with what standing.
 * ORIENT    the joint interpretation: an occurrence is an obligation unless what is attested of it under the
 *           firmware's role kind says it pulls nothing; the interpretation is the responding entities that ground the
 *           most obligations together; unique, ambiguous, or nothing responds.
 * ROUTE     the program: the firmware's hops, fan, tax and emission budget, over the oriented centres.
 * SCAN      best-first from the centres across rated strands, to the hop and fan limits: what is reached, by which
 *           strand, at what cost.
 * COMPOSE   the frontier: what was reached, folded by entity, its routes and supporting occurrences kept apart.
 * PROPOSE   the next constituents: what follows the active trajectory (the prompt and what has been emitted) as a run
 *           in what was observed; and what a result-bearing strand of an oriented entity says.
 * STEER     the firmware's election: grounded obligations first, then ordinal continuity, then the conservative
 *           bound (confidence at k), then the least shared; a hub never wins election.
 * SELECT    the top, or, as the firmware's temperature allows, a strand that near it.
 * REALIZE   its text.
 * WITNESS   the emitted constituent joins the active trajectory, the obligations it grounds close, and the coupling
 *           of the next step is recomputed from that state. At the end of the turn, unless --read, the prompt and the
 *           response are recorded as content, the turn [prompt, response] with them: the user witnesses the turn's
 *           place in the session ([session, turn], at its ordinal, UserPromptContent) and Laplace the response's
 *           dependence on the prompt ([response, prompt], ResponseContent).
 * Every stage's state is printed as the pass's trace. Nothing here changes a standing except by witnessing. */
#include "engine.h"
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXOCC 256
typedef struct { uint64_t w[MAXOCC / 64]; } Bits;
static void bit_set(Bits *b, int i){ if (i >= 0 && i < MAXOCC) b->w[i >> 6] |= 1ull << (i & 63); }
static int bit_count_and(const Bits *a, const Bits *b){ int n = 0; for (int i = 0; i < MAXOCC / 64; i++) n += __builtin_popcountll(a->w[i] & b->w[i]); return n; }
/* What a cell grounds, weighed: the sum, over the obligations it grounds, of how hard each word pulls (its role), in
 * thousandths: a word that pulls hardest owes the most, and glue grounded twice does not outweigh it. */
static const double *ROLE;
static int owed(const Bits *support, const Bits *open){ double s = 0; for (int w = 0; w < MAXOCC / 64; w++) { uint64_t m = support->w[w] & open->w[w]; while (m) { int b = __builtin_ctzll(m); s += ROLE ? ROLE[w * 64 + b] : 1; m &= m - 1; } } return (int)(s * 1000 + 0.5); }
static void bit_clear(Bits *a, const Bits *b){ for (int i = 0; i < MAXOCC / 64; i++) a->w[i] &= ~b->w[i]; }

enum { R_CLAIM, R_FOLLOWS, R_DISCOURSE, R_SCAN, R_CONTAIN, R_KINDS };
static const char *RK[R_KINDS] = { "strand", "follows", "discourse", "reached", "containment" };
typedef struct { lp_id id; double force, cost; Bits support; int routes[R_KINDS]; lp_id via, rel; lp_rating r; int has_r, hub, shared, segment, tier; } Cell;     /* shared: how many strands hold it, up to the fan (-1: not read) */
typedef struct { Cell *c; int n, cap; uint32_t *map; int mcap; } Field;
static uint64_t k64(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 5, 8); return k; }
static Cell *cell(Field *f, const lp_id *id){
    if ((f->n + 1) * 2 > f->mcap) { int nc = f->mcap ? f->mcap * 2 : 1024; uint32_t *m = calloc((size_t)nc, 4);
        for (int i = 0; i < f->n; i++) { uint64_t k = k64(&f->c[i].id) & (uint64_t)(nc - 1); while (m[k]) k = (k + 1) & (uint64_t)(nc - 1); m[k] = (uint32_t)i + 1; } free(f->map); f->map = m; f->mcap = nc; }
    uint64_t k = k64(id) & (uint64_t)(f->mcap - 1);
    while (f->map[k]) { Cell *x = &f->c[f->map[k] - 1]; if (!memcmp(&x->id, id, 16)) return x; k = (k + 1) & (uint64_t)(f->mcap - 1); }
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 1024; f->c = xrealloc(f->c, sizeof(Cell) * (size_t)f->cap); }
    Cell *x = &f->c[f->n]; memset(x, 0, sizeof *x); x->id = *id; x->cost = INFINITY; f->map[k] = (uint32_t)++f->n; return x;
}
static void field_free(Field *f){ free(f->c); free(f->map); memset(f, 0, sizeof *f); }

/* The constituents of a composition, in order, its runs written out; an atom or an unknown ID is itself. */
static int constituents(const lp_id *id, lp_id *out, int cap){
    Node *x = table_find(id); if (!x) { out[0] = *id; return 1; }
    Shard *s = &shard[id->b[0]]; int n = 0;
    for (uint32_t v = 0; v < x->nv && n < cap; v++) for (uint32_t r = 0; r < VRUN(s->vtx[x->voff + v].m) && n < cap; r++) out[n++] = s->vtx[x->voff + v].id;
    return n;
}

/* ---- the state a turn carries from one emitted constituent to the next */
typedef struct {
    const Firmware *fw; PGconn *pg; Reader *rd; lp_text *c; unsigned seed;
    lp_id prompt; lp_id occ[MAXOCC]; int nocc; double role[MAXOCC]; int composed[MAXOCC];        /* the occurrences, how hard each pulls, which are compositions */
    lp_id disc[MAXOCC]; int ndisc;                                                                 /* the discourse: what the session's earlier turns hold */
    lp_id traj[4096]; int ntraj;                                                                   /* the active trajectory: the occurrences, then what has been emitted */
    Bits open;                                                                                     /* the obligations still open */
    lp_id refuse[FW_NAMES], weigh[FW_WEIGHS];
    uint64_t trips;
} State;

static PGresult *ask(State *st, const char *sql, int n, const char **v, const int *l, const int *f){ st->trips++; return db_ask(st->pg, sql, n, v, l, f); }
static int refused_pred(const State *st, const lp_id *part, int np){
    for (int k = 1; k + 1 < np; k++) for (int z = 0; z < st->fw->nrefuse_predicate; z++) if (!memcmp(&part[k], &st->refuse[z], 16)) return 1;
    return 0;
}

/* ---- RESOLVE */
static void resolve_roles(State *st){
    const Firmware *fw = st->fw; lp_id by; memset(&by, 0, sizeof by); if (fw->role_by[0]) by = entity_named(st->c, fw->role_by, NULL, 0, NULL).id;
    lp_id rid[FW_WEIGHS]; for (int z = 0; z < fw->nrole; z++) rid[z] = entity_named(st->c, fw->role_name[z], NULL, 0, NULL).id;
    const lp_layout *fl = lp_flags_map(NULL); const lp_field *alpha = fl ? lp_flags_field(fl, "Alphabetic") : NULL, *gc = fl ? lp_flags_field(fl, "General_Category") : NULL; int32_t nd = gc ? lp_flags_value(fl, gc, "Nd") : -1;
    for (int i = 0; i < st->nocc; i++) { st->role[i] = 1.0; st->composed[i] = table_find(&st->occ[i]) != NULL;
        if (!st->composed[i]) { int64_t cp = lp_tier0_codepoint(T0, &st->occ[i]);      /* a word of one letter is a word: by its Unicode properties, Alphabetic or a decimal number */
            if (cp >= 0 && ((alpha && lp_flags_get(fl, (uint32_t)cp, alpha)) || (gc && nd >= 0 && lp_flags_get(fl, (uint32_t)cp, gc) == (uint32_t)nd))) st->composed[i] = 1; }
        if (!st->composed[i]) { st->role[i] = 0; continue; }                                   /* a space, a mark: no pull of its own, no obligation */
        if (!fw->role_by[0]) continue;
        lp_id part[3] = { st->occ[i], by, by }; int have[3] = { 2, 2, 0 }, n, cap; Claim *cl = claims_like(st->pg, part, have, fw->fan, fw->k, &n, &cap); st->trips++;
        for (int q = 0; q < n; q++) { int hit = 0; for (int z = 0; z < fw->nrole && !hit; z++) if (!memcmp(&cl[q].part[cl[q].np - 1], &rid[z], 16)) { st->role[i] = fw->role[z]; hit = 1; } if (hit) break; }
        free(cl); }
    memset(&st->open, 0, sizeof st->open);
    for (int i = 0; i < st->nocc; i++) if (st->composed[i] && st->role[i] > 0) bit_set(&st->open, i);   /* an obligation until what is attested of it says it pulls nothing */
}

/* ---- COUPLE: the strands of the occurrences, the prompt and the discourse; and what follows the active trajectory */
static void couple_strands(State *st, Field *fd, const lp_id *ids, int n, int occ0, int kind){
    if (!n) return; const Firmware *fw = st->fw;
    uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = ids_param(ab, ids, (uint32_t)n); char fan[24]; snprintf(fan, sizeof fan, "%d", fw->fan + 1);
    int rl; const char *v[4] = { (const char *)ab, fan, CLAIM_BITS, refuse_param(&rl) }; int l[4] = { (int)al, 0, 0, rl }, f[4] = { 1, 0, 0, 1 };
    PGresult *q = ask(st, "SELECT i, path, rating, deviation, volatility FROM laplace_claims_each($1::blake3[], $2::bigint, $3::smallint[], $4::blake3[])", 4, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "couple: %s", PQerrorMessage(st->pg)); exit(1); }
    int *held = calloc((size_t)n, sizeof(int)); for (int r = 0; r < PQntuples(q); r++) { int i = (int)lp_be(PQgetvalue(q, r, 0), 8) - 1; if (i >= 0 && i < n) held[i]++; }
    for (int r = 0; r < PQntuples(q); r++) { int i = (int)lp_be(PQgetvalue(q, r, 0), 8) - 1; if (i < 0 || i >= n) continue;
        Run rn = run_of((const uint8_t *)PQgetvalue(q, r, 1), (size_t)PQgetlength(q, r, 1)); if (rn.n < 2) { free(rn.id); continue; }
        lp_rating rt = { lp_be_f64(PQgetvalue(q, r, 2)), lp_be_f64(PQgetvalue(q, r, 3)), lp_be_f64(PQgetvalue(q, r, 4)) };
        int at = -1; for (int k = 0; k < rn.n && at < 0; k++) if (!memcmp(&rn.id[k], &ids[i], 16)) at = k;
        int other = rn.n == 2 ? 1 - at : at == 0 ? rn.n - 1 : at == rn.n - 1 ? 0 : -1;               /* held as the predicate: it names the tie, it reaches nothing */
        if (at < 0 || other < 0 || refused_pred(st, rn.id, rn.n)) { free(rn.id); continue; }
        int occ = kind == R_CLAIM ? occ0 + i : -1; double pull = occ >= 0 && occ < st->nocc ? st->role[occ] : kind == R_CLAIM ? 1.0 : 0.5;
        Cell *x = cell(fd, &rn.id[other]); double conf = lp_confidence(&rt, fw->k) * strand_weight(fw, st->weigh, rn.id, rn.n);
        x->force += conf * pull; x->routes[kind]++; if (occ >= 0) bit_set(&x->support, occ);
        if (held[i] > fw->fan) x->hub = 1;                                                         /* what so many strands hold is reached, not crossed */
        if (!x->has_r || conf > lp_confidence(&x->r, fw->k)) { x->r = rt; x->has_r = 1; x->via = ids[i]; x->rel = rn.n >= 3 ? rn.id[1] : ids[i]; }
        free(rn.id); }
    PQclear(q); free(ab); free(held);
}
/* COUPLE's containment channel (Sequence 19.3, 15.1): what holds each occurrence, the definitions, examples and texts it
 * stands in, through the container index, at most the fan of them an occurrence (one holding more is a hub word: its
 * containers are reached, the fan of them). A container is a segment the pass can return (Sequence 20.7); one holding
 * several of the prompt's occurrences is where they meet, and grounds them together. Claims are the other channel. */
static void couple_containers(State *st, Field *fd, const lp_id *ids, int n, const int *occ_of){
    if (!n) return; const Firmware *fw = st->fw;
    uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = ids_param(ab, ids, (uint32_t)n); char fan[24]; snprintf(fan, sizeof fan, "%d", fw->fan);
    const char *v[2] = { (const char *)ab, fan }; int l[2] = { (int)al, (int)strlen(fan) }, f[2] = { 1, 0 };
    PGresult *q = ask(st, "SELECT u.i, k.entity, k.tier FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i), "
                          "LATERAL (SELECT c.entity, c.tier FROM laplace_containers(ARRAY[u.id], '{}'::smallint[]) c WHERE NOT (c.mask ? 0::smallint) LIMIT $2::bigint) k", 2, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "containers: %s", PQerrorMessage(st->pg)); exit(1); }
    for (int r = 0; r < PQntuples(q); r++) { int i = (int)lp_be(PQgetvalue(q, r, 0), 8) - 1; if (i < 0 || i >= n) continue;
        int occ = occ_of ? occ_of[i] : -1; double pull = occ >= 0 && occ < st->nocc ? st->role[occ] : 0.5;
        lp_id id; memcpy(id.b, PQgetvalue(q, r, 1), 16); if (!memcmp(&id, &st->prompt, 16)) continue; Cell *x = cell(fd, &id);
        x->segment = 1; x->tier = (int)lp_be(PQgetvalue(q, r, 2), 2); x->force += pull; x->routes[R_CONTAIN]++; if (occ >= 0) bit_set(&x->support, occ); }
    PQclear(q); free(ab);
}

/* ---- the shape channel (Sequence 19.6, 19.7; firmware 18.11). A trajectory's realized curve is its vertices unpacked,
 * each child's real 4D coordinate in order, runs written out: a packed path's X/Y/Z are IDs, never positions. The
 * candidates are nominated by both indexes: the GIN (what holds the prompt's words: exact) and the GiST (what lies
 * nearest the prompt's own coordinate: approximate, it nominates and never decides); the firmware's shape measure
 * decides among them, natively. */
static int id_cmp(const void *a, const void *b){ return memcmp(a, b, 16); }
typedef struct { lp_id id; double xyzm[4]; int has; } Coord;
static int coord_cmp(const void *a, const void *b){ return memcmp(a, b, 16); }
static void coords_of(State *st, Coord *c, int n){                         /* every ID's real coordinate: one read */
    if (!n) return; qsort(c, (size_t)n, sizeof(Coord), coord_cmp);
    lp_id *ids = malloc(sizeof(lp_id) * (size_t)n); for (int i = 0; i < n; i++) ids[i] = c[i].id;
    for (int b0 = 0; b0 < n; b0 += 4096) { int m = n - b0 < 4096 ? n - b0 : 4096;
        uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = ids_param(ab, ids + b0, (uint32_t)m);
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGresult *q = ask(st, "SELECT e.id, ST_X(e.coord), ST_Y(e.coord), ST_Z(e.coord), ST_M(e.coord) FROM entity e WHERE e.id = ANY($1::blake3[])", 1, v, l, f);
        if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "coordinates: %s", PQerrorMessage(st->pg)); exit(1); }
        for (int r = 0; r < PQntuples(q); r++) { Coord key; memcpy(key.id.b, PQgetvalue(q, r, 0), 16); Coord *x = bsearch(&key, c, (size_t)n, sizeof(Coord), coord_cmp);
            if (x) { for (int k = 0; k < 4; k++) x->xyzm[k] = lp_be_f64(PQgetvalue(q, r, 1 + k)); x->has = 1; } }
        PQclear(q); free(ab); }
    free(ids);
}
static double shape_of(const Firmware *fw, const double *a, size_t na, const double *b, size_t nb){
    switch (fw->shape) {
    case FW_OUTLIERS: return lp_frechet4_outliers(a, na, b, nb, (unsigned)fw->shape_n);
    case FW_DTW: { size_t s; double d = lp_dtw4(a, na, b, nb, &s); return s ? d / (double)s : d; }
    case FW_EDR: return (double)lp_edr4(a, na, b, nb, fw->shape_n);
    default: return lp_frechet4(a, na, b, nb); }
}
typedef struct { lp_id id; double d; lp_id *v; int nv; } Curve;
static int curve_by_d(const void *a, const void *b){ double x = ((const Curve *)a)->d, y = ((const Curve *)b)->d; return x < y ? -1 : x > y; }
/* The curves nearest the prompt's: up to keep of them, nearest first. *nominated: how many the indexes nominated. */
static int shape_near(State *st, Field *fd, Curve *out, int keep, int *nominated){
    const Firmware *fw = st->fw; *nominated = 0;
    /* the prompt's curve, from its occurrences' real coordinates */
    Coord *pc = malloc(sizeof(Coord) * (size_t)st->nocc); for (int i = 0; i < st->nocc; i++) { memset(&pc[i], 0, sizeof pc[i]); pc[i].id = st->occ[i]; }
    coords_of(st, pc, st->nocc);
    double *pa = malloc(sizeof(double) * 4 * (size_t)st->nocc); int npa = 0;
    for (int i = 0; i < st->nocc; i++) { Coord key; key.id = st->occ[i]; Coord *x = bsearch(&key, pc, (size_t)st->nocc, sizeof(Coord), coord_cmp); if (x && x->has) memcpy(pa + 4 * npa++, x->xyzm, 32); }
    free(pc); if (npa < 2) { free(pa); return 0; }
    double cen[4] = { 0 }; for (int i = 0; i < npa; i++) for (int k = 0; k < 4; k++) cen[k] += pa[4 * i + k] / npa;

    /* nominated: the texts holding the prompt's words (GIN, read in COUPLE) and the entities nearest its centroid (GiST) */
    lp_id *cand = NULL; int nc = 0, cc = 0;
    for (int z = 0; z < fd->n; z++) if (fd->c[z].segment) { if (nc == cc) { cc = cc ? cc * 2 : 1024; cand = xrealloc(cand, sizeof(lp_id) * (size_t)cc); } cand[nc++] = fd->c[z].id; }
    { char pt[160]; snprintf(pt, sizeof pt, "SRID=0;POINT ZM(%.17g %.17g %.17g %.17g)", cen[0], cen[1], cen[2], cen[3]); char lim[16]; snprintf(lim, sizeof lim, "%d", fw->fan < 1024 ? fw->fan : 1024);
      const char *v[2] = { pt, lim }; int l[2] = { 0, 0 }, f[2] = { 0, 0 };
      PGresult *q = ask(st, "SELECT e.id FROM entity e WHERE e.tier >= 3 ORDER BY e.coord <<->> $1::geometry LIMIT $2::bigint", 2, v, l, f);
      if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "nearest: %s", PQerrorMessage(st->pg)); exit(1); }
      for (int r = 0; r < PQntuples(q); r++) { if (nc == cc) { cc = cc ? cc * 2 : 1024; cand = xrealloc(cand, sizeof(lp_id) * (size_t)cc); } memcpy(cand[nc++].b, PQgetvalue(q, r, 0), 16); }
      PQclear(q); }

    if (nc) { qsort(cand, (size_t)nc, 16, id_cmp); int u = 0; for (int i = 0; i < nc; i++) if (!u || memcmp(&cand[i], &cand[u - 1], 16)) cand[u++] = cand[i]; nc = u; }
    *nominated = nc;
    /* their trajectories, realized: the paths, then every child's coordinate in one read */
    Curve *cv = calloc((size_t)(nc ? nc : 1), sizeof(Curve)); int ncv = 0;
    for (int b0 = 0; b0 < nc; b0 += 1024) { int m = nc - b0 < 1024 ? nc - b0 : 1024;
        uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = ids_param(ab, cand + b0, (uint32_t)m); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
        PGresult *q = ask(st, "SELECT entity, path FROM laplace_paths($1::blake3[])", 1, v, l, f);
        if (PQresultStatus(q) == PGRES_TUPLES_OK) for (int r = 0; r < PQntuples(q); r++) {
            Run rn = run_of((const uint8_t *)PQgetvalue(q, r, 1), (size_t)PQgetlength(q, r, 1)); if (rn.n < 2 || rn.n > 4 * MAXOCC) { free(rn.id); continue; }
            memcpy(cv[ncv].id.b, PQgetvalue(q, r, 0), 16); cv[ncv].v = rn.id; cv[ncv].nv = rn.n; ncv++; }
        PQclear(q); free(ab); }
    free(cand);
    int nall = 0; for (int i = 0; i < ncv; i++) nall += cv[i].nv;
    Coord *cc_ = malloc(sizeof(Coord) * (size_t)(nall ? nall : 1)); int ncc = 0;
    for (int i = 0; i < ncv; i++) for (int k = 0; k < cv[i].nv; k++) { memset(&cc_[ncc], 0, sizeof(Coord)); cc_[ncc++].id = cv[i].v[k]; }
    qsort(cc_, (size_t)ncc, sizeof(Coord), coord_cmp); { int u = 0; for (int i = 0; i < ncc; i++) if (!u || memcmp(&cc_[i], &cc_[u - 1], 16)) cc_[u++] = cc_[i]; ncc = u; }
    coords_of(st, cc_, ncc);
    /* the measure, natively, on every core */
    #pragma omp parallel for schedule(dynamic, 16)
    for (int i = 0; i < ncv; i++) { double *b = malloc(sizeof(double) * 4 * (size_t)cv[i].nv); int nb = 0;
        for (int k = 0; k < cv[i].nv; k++) { Coord key; key.id = cv[i].v[k]; Coord *x = bsearch(&key, cc_, (size_t)ncc, sizeof(Coord), coord_cmp); if (x && x->has) memcpy(b + 4 * nb++, x->xyzm, 32); }
        cv[i].d = nb >= 2 ? shape_of(fw, pa, (size_t)npa, b, (size_t)nb) : INFINITY; free(b); }
    qsort(cv, (size_t)ncv, sizeof(Curve), curve_by_d);
    int m = ncv < keep ? ncv : keep; for (int i = 0; i < m; i++) out[i] = cv[i];
    for (int i = m; i < ncv; i++) free(cv[i].v); free(cv); free(cc_); free(pa);
    return m;
}

/* What follows the end of the active trajectory, as a run, in what was observed: the longest observed suffix first. */
typedef struct { lp_id id; long times; int len; } Next;
static int follows(State *st, Next *out, int cap){
    int from = st->ntraj > 24 ? st->ntraj - 24 : 0, n = st->ntraj - from; if (n <= 0) return 0;
    uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = ids_param(ab, st->traj + from, (uint32_t)n); char fan[24]; snprintf(fan, sizeof fan, "%d", st->fw->fan);
    const char *v[2] = { (const char *)ab, fan }; int l[2] = { (int)al, (int)strlen(fan) }, f[2] = { 1, 0 };
    PGresult *q = ask(st, "SELECT i, j, next, times FROM laplace_forward($1::blake3[], $2::bigint) WHERE j = array_length($1::blake3[], 1) AND next IS NOT NULL ORDER BY (j - i) DESC, times DESC", 2, v, l, f);
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "follows: %s", PQerrorMessage(st->pg)); exit(1); }
    int m = 0, best = -1;
    for (int r = 0; r < PQntuples(q) && m < cap; r++) { int i = (int)lp_be(PQgetvalue(q, r, 0), 4), j = (int)lp_be(PQgetvalue(q, r, 1), 4), len = j - i + 1;
        if (best < 0) best = len; if (len < best) break;                                           /* the longest run observed decides; shorter ones are not consulted once it answers */
        memcpy(out[m].id.b, PQgetvalue(q, r, 2), 16); out[m].times = (long)lp_be(PQgetvalue(q, r, 3), 8); out[m].len = len; m++; }
    PQclear(q); free(ab); return m;
}

/* How many strands hold each of these cells, up to the fan: what more than the fan holds is a hub, reached and never
 * crossed, and among the rest the least shared meets first. One set-based read. */
static void share(State *st, Field *fd, const int *idx, int n){
    if (!n) return; lp_id *ids = malloc(sizeof(lp_id) * (size_t)n); for (int i = 0; i < n; i++) { ids[i] = fd->c[idx[i]].id; fd->c[idx[i]].shared = 0; }
    uint8_t *ab = malloc(20 + 20 * (size_t)n); size_t al = ids_param(ab, ids, (uint32_t)n); char fan[24]; snprintf(fan, sizeof fan, "%d", st->fw->fan + 1);
    int rl; const char *v[4] = { (const char *)ab, fan, CLAIM_BITS, refuse_param(&rl) }; int l[4] = { (int)al, 0, 0, rl }, f[4] = { 1, 0, 0, 1 };
    PGresult *q = ask(st, "SELECT i FROM laplace_claims_each($1::blake3[], $2::bigint, $3::smallint[], $4::blake3[])", 4, v, l, f);
    if (PQresultStatus(q) == PGRES_TUPLES_OK) for (int r = 0; r < PQntuples(q); r++) { int i = (int)lp_be(PQgetvalue(q, r, 0), 8) - 1; if (i >= 0 && i < n) fd->c[idx[i]].shared++; }
    for (int i = 0; i < n; i++) if (fd->c[idx[i]].shared > st->fw->fan) fd->c[idx[i]].hub = 1;
    PQclear(q); free(ab); free(ids);
}
/* A chain the firmware names, followed from a word: each relation in turn, the strands that hold where the chain stands
 * (or, past the first step, one of the things it is made of) in their witness's order, then by standing; the top taken.
 * The first of the firmware's chains that reaches its end answers; *rating: the standing of its last strand. */
static int chain_from(State *st, const lp_id *word, const lp_id *reading, lp_id *answer, lp_rating *rating){
    const Firmware *fw = st->fw;
    for (int alt = 0; alt < fw->nalt; alt++) { lp_id cur = *word; int z = 0;
        for (; z < fw->nchain[alt]; z++) {
            lp_id pred = entity_named(st->c, fw->chain[alt][z], NULL, 0, NULL).id, tryv[66]; int nt = 0; tryv[nt++] = cur;
            if (z > 0 && lp_tier0_codepoint(T0, &cur) < 0) { uint8_t ab[40]; size_t al = ids_param(ab, &cur, 1); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
                PGresult *q = ask(st, "SELECT path FROM laplace_paths($1::blake3[]) LIMIT 1", 1, v, l, f);
                if (PQresultStatus(q) == PGRES_TUPLES_OK && PQntuples(q)) { Run rn = run_of((const uint8_t *)PQgetvalue(q, 0, 0), (size_t)PQgetlength(q, 0, 0));
                    for (int k = rn.n - 1; k >= 0 && nt < 65; k--) if (lp_tier0_codepoint(T0, &rn.id[k]) < 0 && memcmp(&rn.id[k], word, 16)) tryv[nt++] = rn.id[k]; free(rn.id); }
                PQclear(q); tryv[nt++] = *word; }
            Claim *cl = NULL; int n = 0, cap;
            for (int t = 0; t < nt && !n; t++) { lp_id part[3] = { tryv[t], pred, pred }; int have[3] = { 2, 2, 0 }; cl = claims_like(st->pg, part, have, fw->fan, fw->k, &n, &cap); st->trips++; if (!n) { free(cl); cl = NULL; } }
            if (!n) break;
            positions_of(st->pg, cl, n); if (fw->order_witness) qsort(cl, (size_t)n, sizeof(Claim), claim_by_position);
            int take = 0;                                                    /* the first step follows the word's oriented reading, or this chain does not answer it */
            if (z == 0 && reading) { take = -1; for (int k = 0; k < n && take < 0; k++) if (!memcmp(&cl[k].part[cl[k].np - 1], reading, 16)) take = k; }
            if (take < 0) { free(cl); break; }
            cur = cl[take].part[cl[take].np - 1]; *rating = cl[take].r; free(cl); }
        if (z == fw->nchain[alt]) { *answer = cur; return 1; } }
    return 0;
}
/* ---- SCAN: best-first from the centres, across rated strands */
static void scan(State *st, Field *fd, const lp_id *centre, int nc){
    const Firmware *fw = st->fw; lp_frontier *fr = lp_frontier_new();
    for (int i = 0; i < nc; i++) lp_frontier_reach(fr, &centre[i], NULL, NULL, 0, 0, 0);
    int rounds = 0; const lp_reached *x;
    while (rounds++ < fw->hops) {
        lp_reached batch[16]; int n = 0; while (n < 16 && (x = lp_frontier_next(fr))) batch[n++] = *x; if (!n) break;      /* the nearest sixteen a round */
        lp_id ids[64]; int m = 0, who[64]; for (int i = 0; i < n; i++) if ((int)batch[i].hops < fw->hops) { ids[m] = batch[i].id; who[m++] = i; }
        if (!m) break;
        uint8_t ab[20 + 20 * 64]; size_t al = ids_param(ab, ids, (uint32_t)m); char fan[24]; snprintf(fan, sizeof fan, "%d", fw->fan + 1);
        int rl; const char *v[4] = { (const char *)ab, fan, CLAIM_BITS, refuse_param(&rl) }; int l[4] = { (int)al, 0, 0, rl }, f[4] = { 1, 0, 0, 1 };
        PGresult *q = ask(st, "SELECT i, entity, path, rating, deviation, volatility FROM laplace_claims_each($1::blake3[], $2::bigint, $3::smallint[], $4::blake3[])", 4, v, l, f);
        if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "scan: %s", PQerrorMessage(st->pg)); exit(1); }
        int held[64] = { 0 }; for (int r = 0; r < PQntuples(q); r++) { int e = (int)lp_be(PQgetvalue(q, r, 0), 8) - 1; if (e >= 0 && e < m) held[e]++; }
        for (int r = 0; r < PQntuples(q); r++) { int e = (int)lp_be(PQgetvalue(q, r, 0), 8) - 1; if (e < 0 || e >= m || held[e] > fw->fan) continue;
            const lp_reached *at = &batch[who[e]]; Run rn = run_of((const uint8_t *)PQgetvalue(q, r, 2), (size_t)PQgetlength(q, r, 2));
            if (rn.n < 2 || refused_pred(st, rn.id, rn.n)) { free(rn.id); continue; }
            const lp_id *other = !memcmp(&rn.id[0], &at->id, 16) ? &rn.id[rn.n - 1] : !memcmp(&rn.id[rn.n - 1], &at->id, 16) ? &rn.id[0] : NULL;
            if (!other || !memcmp(other, &at->id, 16)) { free(rn.id); continue; }
            lp_rating rt = { lp_be_f64(PQgetvalue(q, r, 3)), lp_be_f64(PQgetvalue(q, r, 4)), lp_be_f64(PQgetvalue(q, r, 5)) };
            double sw = strand_weight(fw, st->weigh, rn.id, rn.n); if (sw <= 0) { free(rn.id); continue; }
            lp_id claim; memcpy(claim.b, PQgetvalue(q, r, 1), 16); double cost = at->cost + lp_cost(&rt, fw->k, fw->lambda) - log(sw);
            lp_frontier_reach(fr, other, &at->id, &claim, cost, 0, at->hops + 1);
            Cell *cx = cell(fd, other); cx->routes[R_SCAN]++; if (cost < cx->cost) { cx->cost = cost; if (!cx->has_r) { cx->r = rt; cx->has_r = 1; cx->via = at->id; cx->rel = rn.n >= 3 ? rn.id[1] : at->id; } }
            free(rn.id); }
        PQclear(q);
    }
    lp_frontier_free(fr);
}

/* ---- the proposals of a step, and their election */
enum { P_FOLLOW, P_CHAIN };
typedef struct { lp_id id; int kind, grounds, cont; double conf, hub; long times; int occ; } Prop;     /* occ: the occurrence a chain answers, or -1 */
static int elect(const void *a, const void *b){                               /* grounded obligations, continuity, the conservative bound, the least shared */
    const Prop *x = a, *y = b;
    if (x->grounds != y->grounds) return y->grounds - x->grounds;
    if (x->cont != y->cont) return y->cont - x->cont;
    if (x->conf != y->conf) return x->conf < y->conf ? 1 : -1;
    if (x->hub != y->hub) return x->hub > y->hub ? 1 : -1;
    return memcmp(&x->id, &y->id, 16);
}

/* ---- ORIENT: the joint interpretation (Sequence 20.3; INVENTION §7, "Joint interpretation before policy"). A winning
 * interpretation is a jointly compatible subgraph, not the definition with the largest global score: each occurrence's
 * candidates are what its own strands reach in the field, and the candidates of different occurrences constrain one
 * another through the web. Two are compatible when a strand ties them, or a strand of each meets in one entity that is
 * no hub (a hub is reached, never crossed). Each occurrence takes the candidate the others' choices support most, round
 * after round until no choice changes: a candidate in turn changes what makes sense for the other constituents. An
 * occurrence whose best two stand level is ambiguous; one no other occurrence's choice supports binds nothing. */
#define CAND 32
typedef struct { int cell, occ; lp_id *nb; int nnb; } Cand;
typedef struct { int ncand, choice, level, capped; double score; } Bind;      /* per occurrence */
static const Field *CF;
static int cand_by_force(const void *a, const void *b){ double x = CF->c[*(const int *)a].force, y = CF->c[*(const int *)b].force; return x < y ? 1 : x > y ? -1 : 0; }
static int has_id(const lp_id *s, int n, const lp_id *id){ return n && bsearch(id, s, (size_t)n, 16, id_cmp) != NULL; }
/* How strongly two candidates of different occurrences hold together: a strand that ties them counts twice, each
 * entity a strand of each meets in (no hub) once. */
static int compat(const Cand *a, const Cand *b, const lp_id *aid, const lp_id *bid, const lp_id *hubs, int nhubs){
    int s = 0; if (!memcmp(aid, bid, 16)) return 4;
    if (has_id(a->nb, a->nnb, bid) || has_id(b->nb, b->nnb, aid)) s += 2;
    for (int i = 0, j = 0; i < a->nnb && j < b->nnb; ) { int c = memcmp(&a->nb[i], &b->nb[j], 16);
        if (!c) { if (!has_id(hubs, nhubs, &a->nb[i])) s++; i++; j++; } else if (c < 0) i++; else j++; }
    return s;
}
static int orient(State *st, Field *fd, Bind *bind, int *nambig){
    const Firmware *fw = st->fw; Cand *cand = NULL; int nc = 0, cc = 0; *nambig = 0;
    int *byocc = malloc(sizeof(int) * (size_t)(fd->n ? fd->n : 1));
    for (int i = 0; i < st->nocc; i++) { memset(&bind[i], 0, sizeof bind[i]); bind[i].choice = -1;
        if (!((st->open.w[i >> 6] >> (i & 63)) & 1)) continue;                         /* an occurrence that owes nothing orients nothing */
        int m = 0; for (int z = 0; z < fd->n; z++) { const Cell *x = &fd->c[z];
            if (x->hub || x->segment || x->force <= 0 || !((x->support.w[i >> 6] >> (i & 63)) & 1)) continue; byocc[m++] = z; }    /* a segment is evidence, not a reading */
        CF = fd; qsort(byocc, (size_t)m, sizeof(int), cand_by_force);
        int lead = m < 4 * CAND ? m : 4 * CAND, keep = 0;                    /* a candidate more than the fan holds (a lexicon, a language) is a hub: reached, never a reading */
        for (int b0 = 0; b0 < lead; b0 += 512) share(st, fd, byocc + b0, lead - b0 < 512 ? lead - b0 : 512);
        for (int k = 0; k < lead; k++) if (!fd->c[byocc[k]].hub) byocc[keep++] = byocc[k];
        if (m > lead) bind[i].capped = 1; m = keep;
        if (m > CAND) { bind[i].capped = 1; m = CAND; }
        for (int k = 0; k < m; k++) { if (nc == cc) { cc = cc ? cc * 2 : 256; cand = xrealloc(cand, sizeof(Cand) * (size_t)cc); } cand[nc++] = (Cand){ byocc[k], i, NULL, 0 }; }
        bind[i].ncand = m; }
    free(byocc);
    /* what each candidate's strands reach: one set-based read a batch, refusals out before the fan */
    for (int b0 = 0; b0 < nc; b0 += 512) { int m = nc - b0 < 512 ? nc - b0 : 512; lp_id *ids = malloc(sizeof(lp_id) * (size_t)m);
        for (int k = 0; k < m; k++) ids[k] = fd->c[cand[b0 + k].cell].id;
        uint8_t *ab = malloc(20 + 20 * (size_t)m); size_t al = ids_param(ab, ids, (uint32_t)m); char fan[24]; snprintf(fan, sizeof fan, "%d", fw->fan + 1);
        int rl; const char *v[4] = { (const char *)ab, fan, CLAIM_BITS, refuse_param(&rl) }; int l[4] = { (int)al, 0, 0, rl }, f[4] = { 1, 0, 0, 1 };
        PGresult *q = ask(st, "SELECT i, path FROM laplace_claims_each($1::blake3[], $2::bigint, $3::smallint[], $4::blake3[])", 4, v, l, f);
        if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "orient: %s", PQerrorMessage(st->pg)); exit(1); }
        int *cap = calloc((size_t)m, sizeof(int));
        for (int r = 0; r < PQntuples(q); r++) { int k = (int)lp_be(PQgetvalue(q, r, 0), 8) - 1; if (k < 0 || k >= m) continue;
            Run rn = run_of((const uint8_t *)PQgetvalue(q, r, 1), (size_t)PQgetlength(q, r, 1)); Cand *x = &cand[b0 + k];
            const lp_id *other = rn.n >= 2 && !memcmp(&rn.id[0], &ids[k], 16) ? &rn.id[rn.n - 1] : rn.n >= 2 && !memcmp(&rn.id[rn.n - 1], &ids[k], 16) ? &rn.id[0] : NULL;
            if (other && memcmp(other, &ids[k], 16)) { if (x->nnb == cap[k]) { cap[k] = cap[k] ? cap[k] * 2 : 16; x->nb = xrealloc(x->nb, sizeof(lp_id) * (size_t)cap[k]); } x->nb[x->nnb++] = *other; }
            free(rn.id); }
        for (int k = 0; k < m; k++) { Cand *x = &cand[b0 + k]; if (x->nnb) qsort(x->nb, (size_t)x->nnb, 16, id_cmp); }
        PQclear(q); free(ab); free(ids); free(cap); }
    /* the entities candidates of two occurrences meet in: a hub among them is reached, never crossed */
    lp_id *meet = NULL; int nmeet = 0, cmeet = 0, nhubs = 0; lp_id *hubs = NULL;
    for (int a = 0; a < nc; a++) for (int b = a + 1; b < nc; b++) { if (cand[a].occ == cand[b].occ) continue;
        for (int i = 0, j = 0; i < cand[a].nnb && j < cand[b].nnb; ) { int c = memcmp(&cand[a].nb[i], &cand[b].nb[j], 16);
            if (!c) { if (nmeet == cmeet) { cmeet = cmeet ? cmeet * 2 : 256; meet = xrealloc(meet, sizeof(lp_id) * (size_t)cmeet); } if (nmeet < 1 << 16) meet[nmeet++] = cand[a].nb[i]; i++; j++; } else if (c < 0) i++; else j++; } }
    if (nmeet) { qsort(meet, (size_t)nmeet, 16, id_cmp); int u = 0; for (int i = 0; i < nmeet; i++) if (!u || memcmp(&meet[i], &meet[u - 1], 16)) meet[u++] = meet[i]; nmeet = u;
        Field mf = { 0 }; int *idx = malloc(sizeof(int) * (size_t)nmeet); for (int i = 0; i < nmeet; i++) { cell(&mf, &meet[i]); idx[i] = i; }
        for (int b0 = 0; b0 < nmeet; b0 += 512) share(st, &mf, idx + b0, nmeet - b0 < 512 ? nmeet - b0 : 512);
        hubs = malloc(sizeof(lp_id) * (size_t)nmeet); for (int i = 0; i < mf.n; i++) if (mf.c[i].hub) hubs[nhubs++] = mf.c[i].id;
        if (nhubs) qsort(hubs, (size_t)nhubs, 16, id_cmp); free(idx); field_free(&mf); }
    /* the joint interpretation: each occurrence's choice, given the others', until none changes */
    int *first = calloc((size_t)st->nocc + 1, sizeof(int)); for (int i = 0, at = 0; i < st->nocc; i++) { first[i] = at; at += bind[i].ncand; } first[st->nocc] = nc;
    for (int round = 0; round < 8; round++) { int changed = 0;
        for (int i = 0; i < st->nocc; i++) { if (!bind[i].ncand) continue; double best = 0, second = 0; int pick = -1;
            for (int a = first[i]; a < first[i] + bind[i].ncand; a++) { double s = 0;
                for (int j = 0; j < st->nocc; j++) { if (j == i || !bind[j].ncand) continue; int lo = first[j], hi = first[j] + bind[j].ncand, bj = bind[j].choice;
                    double w = st->role[j], top = 0;                              /* before the others have chosen, the best any of their candidates gives */
                    if (round && bj >= 0) top = compat(&cand[a], &cand[bj], &fd->c[cand[a].cell].id, &fd->c[cand[bj].cell].id, hubs, nhubs);
                    else if (!round) for (int b = lo; b < hi; b++) { int c = compat(&cand[a], &cand[b], &fd->c[cand[a].cell].id, &fd->c[cand[b].cell].id, hubs, nhubs); if (c > top) top = c; }
                    s += w * top; }
                if (s > best) { second = best; best = s; pick = a; } else if (s > second) second = s; }
            bind[i].level = pick >= 0 && second == best;
            if (pick != bind[i].choice) { bind[i].choice = pick; changed = 1; } bind[i].score = best; }
        if (round && !changed) break; }
    int bound = 0; for (int i = 0; i < st->nocc; i++) { if (bind[i].choice >= 0) { bound++; if (bind[i].level) (*nambig)++; bind[i].choice = cand[bind[i].choice].cell; } }
    for (int a = 0; a < nc; a++) free(cand[a].nb); free(cand); free(meet); free(hubs); free(first);
    return bound;
}
int cmd_turn(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL, *user = getenv("USER"), *session = NULL; int a = 1, seeded = 0, read_only = 0; unsigned seed = 0;
    for (; a < argc - 1 && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "--firmware") && a + 1 < argc) fwp = argv[++a];
        else if (!strcmp(argv[a], "--as") && a + 1 < argc) user = argv[++a];
        else if (!strcmp(argv[a], "--session") && a + 1 < argc) session = argv[++a];
        else if (!strcmp(argv[a], "--seed") && a + 1 < argc) { seed = (unsigned)strtoul(argv[++a], NULL, 10); seeded = 1; }
        else if (!strcmp(argv[a], "--read")) read_only = 1;                    /* a read: nothing is witnessed */
        else break;
    }
    if (a >= argc) { fprintf(stderr, "usage: laplace turn [-d conninfo] [--firmware FILE] [--as USER] [--session NAME] [--seed N] [--read] prompt\n"); return 2; }
    if (!user || !*user) user = "user"; if (!session) session = "session";
    double T = now(); if (!seeded) seed = (unsigned)(T * 1e6);
    Firmware fw = firmware_for(fwp, FW_PULL);
    tier0_open(NULL); table_init(); ctx_open(1); lp_text *c = lp_text_new(T0);
    State *st = calloc(1, sizeof(State)); st->fw = &fw; st->c = c; st->seed = seed;
    st->pg = db_connect(conninfo); st->rd = reader_new(st->pg);
    for (int z = 0; z < fw.nrefuse_predicate; z++) st->refuse[z] = entity_named(c, fw.refuse_predicate[z], NULL, 0, NULL).id;
    weights_named(c, &fw, st->weigh);
    firmware_say(&fw, FW_PULL);

    /* ---- RESOLVE: the prompt as content; the session from the record */
    const char *prompt = argv[a];
    Ref pr = text_ref(CTX[0], (const uint8_t *)prompt, strlen(prompt)); st->prompt = pr.id;
    st->nocc = pr.tier <= 2 ? 1 : constituents(&pr.id, st->occ, MAXOCC); if (pr.tier <= 2) st->occ[0] = pr.id;
    Ref who = text_ref(CTX[0], (const uint8_t *)user, strlen(user)), sname = text_ref(CTX[0], (const uint8_t *)session, strlen(session));
    Ref hp[2] = { who, sname }; hp[0].said = hp[1].said = 0; Ref handle = said_tuple(compose(hp, 2, (uint8_t)((who.tier > sname.tier ? who.tier : sname.tier) + 1)));    /* the session: its name within its user's */
    int nturn = 0, ordinal = 1;
    { int have[3] = { 2, 0, 0 }, n, cap; lp_id part[3] = { handle.id, handle.id, handle.id }; Claim *tc = claims_like(st->pg, part, have, fw.fan, fw.k, &n, &cap); st->trips++;
      positions_of(st->pg, tc, n); qsort(tc, (size_t)n, sizeof(Claim), claim_by_position);
      for (int i = 0; i < n; i++) { if (tc[i].np != 2 || memcmp(&tc[i].part[0], &handle.id, 16)) continue; nturn++;
          if (tc[i].position >= ordinal) ordinal = tc[i].position + 1;
          uint8_t ab[40]; size_t al = ids_param(ab, &tc[i].part[1], 1); const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
          PGresult *q = ask(st, "SELECT path FROM laplace_paths($1::blake3[]) LIMIT 1", 1, v, l, f);       /* the turn: [prompt, response]; their constituents are the discourse */
          if (PQresultStatus(q) == PGRES_TUPLES_OK && PQntuples(q)) { Run tr = run_of((const uint8_t *)PQgetvalue(q, 0, 0), (size_t)PQgetlength(q, 0, 0));
              for (int k = 0; k < tr.n && st->ndisc < MAXOCC; k++) st->disc[st->ndisc++] = tr.id[k]; free(tr.id); }
          PQclear(q); }
      free(tc); }
    resolve_roles(st); ROLE = st->role;
    memcpy(st->traj, st->occ, sizeof(lp_id) * (size_t)st->nocc); st->ntraj = st->nocc;
    for (int i = 0; i < st->nocc; i++) reader_want(st->rd, &st->occ[i]);
    char idt[33]; id_text(&pr.id, idt);
    printf("RESOLVE    prompt %s, tier %d, %d occurrences; session \"%s\" of %s, turn %d (%d before it, %d discourse entities)\n", idt, pr.tier, st->nocc, session, user, ordinal, nturn, st->ndisc);
    printf("           obligations:"); for (int i = 0; i < st->nocc; i++) if (st->composed[i]) { char *tx = reader_text(st->rd, &st->occ[i], 32); printf(" %s%s(%.2f)", tx, (st->open.w[i >> 6] >> (i & 63)) & 1 ? "" : "~", st->role[i]); free(tx); } printf("\n");

    /* ---- the loop: each emitted constituent changes the state the next is chosen from */
    lp_id emitted[512]; int nemit = 0; char *out = calloc(1, 1); size_t outn = 0; const char *disposition = "nothing responds";
    Field fd = { 0 };
    { /* COUPLE, once for the whole observation: the strands of every occurrence, of the prompt itself, and of the discourse */
      double t = now(); couple_strands(st, &fd, st->occ, st->nocc, 0, R_CLAIM); couple_strands(st, &fd, &st->prompt, 1, -1, R_CLAIM); couple_strands(st, &fd, st->disc, st->ndisc, 0, R_DISCOURSE);
      { lp_id cw[MAXOCC]; int co[MAXOCC], ncw = 0; for (int i = 0; i < st->nocc; i++) if (st->composed[i] && st->role[i] > 0) { cw[ncw] = st->occ[i]; co[ncw++] = i; } couple_containers(st, &fd, cw, ncw, co); }
      for (int i = 0; i < st->nocc; i++) { Cell *x = NULL; for (int z = 0; z < fd.n; z++) if (!memcmp(&fd.c[z].id, &st->occ[i], 16)) x = &fd.c[z]; if (x) x->force = 0; }      /* the prompt's own words are not what it is about */
      int routes[R_KINDS] = { 0 }; for (int z = 0; z < fd.n; z++) for (int k = 0; k < R_KINDS; k++) routes[k] += fd.c[z].routes[k];
      printf("COUPLE     %d entities respond:", fd.n); for (int k = 0; k < R_KINDS; k++) if (routes[k]) printf(" %d by %s", routes[k], RK[k]); printf("   (%.1f ms)\n", (now() - t) * 1000); }
    /* ORIENT, the frame: the observed curves nearest the prompt's under the firmware's shape measure. The prompt's words
     * the nearest holds are the frame the question is asked in; the words it does not hold, where the curves part, are
     * the slots, what the question is about. While the frame holds part of the prompt and leaves a slot, the slots are
     * what is owed. */
    Curve near[8]; int nnear = 0, nominated = 0;
    { double t = now(); nnear = shape_near(st, &fd, near, 8, &nominated);
      static const char *SH[] = { "frechet", "outliers", "dtw", "edr" };
      printf("ORIENT     the shape: %d curves nominated (GIN: what holds the words; GiST: what lies nearest), measured by %s   (%.1f ms)\n", nominated, SH[fw.shape], (now() - t) * 1000);
      for (int i = 0; i < nnear && i < 5; i++) { reader_want(st->rd, &near[i].id); char *tx = reader_text(st->rd, &near[i].id, 90); printf("           %8.4f   %s\n", near[i].d, tx); free(tx); }
      if (nnear) { Bits slots; memset(&slots, 0, sizeof slots); int nslot = 0, nframe = 0;
          for (int i = 0; i < st->nocc; i++) { if (!((st->open.w[i >> 6] >> (i & 63)) & 1)) continue; int held = 0;
              for (int k = 0; k < near[0].nv && !held; k++) held = !memcmp(&near[0].v[k], &st->occ[i], 16);
              if (held) nframe++; else { bit_set(&slots, i); nslot++; } }
          if (nframe && nslot) { for (int w = 0; w < MAXOCC / 64; w++) st->open.w[w] &= slots.w[w];
              printf("           the frame holds %d of the words; it is about", nframe);
              for (int i = 0; i < st->nocc; i++) if ((slots.w[i >> 6] >> (i & 63)) & 1) { char *tx = reader_text(st->rd, &st->occ[i], 24); printf(" %s", tx); free(tx); } printf("\n"); }
          else printf("           %s\n", nframe ? "the nearest holds every word owed" : "the nearest holds none of the words owed: every word is owed"); } }
    for (int i = 0; i < nnear; i++) free(near[i].v);
    /* ORIENT: the joint interpretation; its bindings are the centres SCAN starts from and what a chain answers from */
    Bind *bind = calloc((size_t)st->nocc + 1, sizeof(Bind)); int nambig = 0, nbound, ncentre = 0, capped = 0; lp_id centre[MAXOCC];
    { double t = now(); nbound = orient(st, &fd, bind, &nambig);
      for (int i = 0; i < st->nocc; i++) { capped |= bind[i].capped; if (bind[i].choice >= 0 && !bind[i].level) centre[ncentre++] = fd.c[bind[i].choice].id; }
      int ambiguous_ = nambig > 0;
      printf("ORIENT     %s", !nbound ? (capped ? "resource-bounded: no joint binding among the candidates weighed" : "inconsistent: no reading of one word is compatible with a reading of another")
                                    : ambiguous_ ? "ambiguous" : "unique enough to execute");
      printf("   (%.1f ms)\n", (now() - t) * 1000);
      for (int i = 0; i < st->nocc; i++) { if (!st->composed[i] || st->role[i] <= 0) continue; char *w = reader_text(st->rd, &st->occ[i], 32);
          if (bind[i].choice < 0) printf("           %-12s binds nothing: %d reading%s, none another word's reading holds together with\n", w, bind[i].ncand, bind[i].ncand == 1 ? "" : "s");
          else { reader_want(st->rd, &fd.c[bind[i].choice].id); char *b = reader_text(st->rd, &fd.c[bind[i].choice].id, 60);
                 printf("           %-12s %s %s   (held together %.2f, of %d reading%s)\n", w, bind[i].level ? "ambiguous, e.g." : "->", b, bind[i].score, bind[i].ncand, bind[i].ncand == 1 ? "" : "s"); free(b); }
          free(w); } }
    int ambiguous = nambig > 0;
    /* ROUTE */
    printf("ROUTE      hops %d, fan %d, k %g, lambda %g, emit at most %d, from %d centre%s\n", fw.hops, fw.fan, fw.k, fw.lambda, fw.emit, ncentre, ncentre == 1 ? "" : "s");
    /* SCAN, COMPOSE */
    { double t = now(); int before = fd.n; scan(st, &fd, centre, ncentre); printf("SCAN       %d reached beyond the field   (%.1f ms)\n", fd.n - before, (now() - t) * 1000); }
    printf("COMPOSE    a frontier of %d\n", fd.n);
    /* the chains the firmware names, followed from each word still owed, the hardest pulling first: an answer a
     * result-bearing relation establishes (a word's gloss through its sense), proposed while the word is owed */
    Prop chains[8]; int nchains = 0;
    { int byrole[MAXOCC], nb = 0; for (int i = 0; i < st->nocc; i++) if ((st->open.w[i >> 6] >> (i & 63)) & 1) byrole[nb++] = i;
      for (int x = 1; x < nb; x++) { int v = byrole[x], y = x; while (y > 0 && st->role[byrole[y - 1]] < st->role[v]) { byrole[y] = byrole[y - 1]; y--; } byrole[y] = v; }
      for (int x = 0; x < nb && nchains < 8 && fw.nalt; x++) { lp_id ans; lp_rating rt; int o_ = byrole[x]; if (bind[o_].choice < 0 || bind[o_].level) continue; if (!chain_from(st, &st->occ[o_], &fd.c[bind[o_].choice].id, &ans, &rt)) continue;
          chains[nchains++] = (Prop){ ans, P_CHAIN, (int)(st->role[byrole[x]] * 1000 + 0.5), 0, lp_confidence(&rt, fw.k), 0, 0, byrole[x] };
          reader_want(st->rd, &ans); char *w = reader_text(st->rd, &st->occ[byrole[x]], 32), *t = reader_text(st->rd, &ans, 80); printf("           chain from %s: %s\n", w, t); free(w); free(t); }
      printf("           %d chain%s of the firmware answer a word still owed\n", nchains, nchains == 1 ? "" : "s"); }
    disposition = ambiguous ? "ambiguous: more than one reading survives, and an ambiguous orientation emits nothing" : nbound ? "unresolved" : capped ? "resource-bounded: no joint binding among the candidates weighed" : "inconsistent: no word's reading holds together with another's";
    int budget = ambiguous ? 0 : fw.emit;                                   /* an ambiguous orientation finalizes and emits nothing (Sequence 18.12) */
    int owed0 = owed(&st->open, &st->open);                                 /* what the obligations owe at the start of the turn */

    for (int step = 0; step < budget; step++) {
        /* PROPOSE: what follows the active trajectory as a run in what was observed, and the answers the firmware's
         * result-bearing relations (its chains) give a word still owed. An entity the coupling merely reaches is routing
         * state: it orients and is scanned from, and is not output for being renderable. */
        Prop *pp = malloc(sizeof(Prop) * (size_t)(80)); int np = 0;
        Next nx[64]; int nn = follows(st, nx, 64);
        for (int i = 0; i < nn; i++) { Prop p = { nx[i].id, P_FOLLOW, 0, nx[i].len, 0, 0, nx[i].times, -1 };
            for (int z = 0; z < fd.n; z++) if (!memcmp(&fd.c[z].id, &nx[i].id, 16)) { p.grounds = owed(&fd.c[z].support, &st->open); p.conf = fd.c[z].has_r ? lp_confidence(&fd.c[z].r, fw.k) : 0; p.hub = fd.c[z].hub; }
            pp[np++] = p; }
        for (int q = 0; q < nchains; q++) { int o = chains[q].occ; if (!((st->open.w[o >> 6] >> (o & 63)) & 1)) continue; pp[np++] = chains[q]; }     /* a chain's answer, while its word is owed */
        if (!np) { free(pp); if (!nemit) disposition = ncentre ? "unresolved: nothing follows and nothing grounds what is open" : disposition; break; }
        /* STEER, SELECT */
        qsort(pp, (size_t)np, sizeof(Prop), elect);
        int pick = 0; if (fw.top_within > 0) { int tied = 0; while (tied + 1 < np && pp[tied + 1].grounds == pp[0].grounds && pp[tied + 1].cont == pp[0].cont && pp[0].conf - pp[tied + 1].conf <= fw.top_within) tied++; pick = (int)(rand_r(&st->seed) % (unsigned)(tied + 1)); }
        Prop sel = pp[pick]; free(pp);
        /* REALIZE */
        reader_want(st->rd, &sel.id); char *tx = reader_text(st->rd, &sel.id, 400);
        size_t tl = strlen(tx); int sep = sel.kind != P_FOLLOW && outn && out[outn - 1] != ' ' && tl && tx[0] != ' ';
        out = xrealloc(out, outn + tl + 2); if (sep) out[outn++] = ' '; memcpy(out + outn, tx, tl); outn += tl; out[outn] = 0;
        printf("STEP %-3d   %s \"%s\"   grounds %.2f, continuity %d, confidence %.3f\n", step + 1, sel.kind == P_FOLLOW ? "follows" : "answers", tx, sel.grounds / 1000.0, sel.cont, sel.conf);
        free(tx);
        /* WITNESS, within the pass: the constituent joins the trajectory; what it grounds closes; the next coupling sees it */
        if (nemit < 512) emitted[nemit++] = sel.id; if (st->ntraj < 4096) st->traj[st->ntraj++] = sel.id;
        for (int z = 0; z < fd.n; z++) if (!memcmp(&fd.c[z].id, &sel.id, 16)) bit_clear(&st->open, &fd.c[z].support);
        if (sel.kind == P_CHAIN && sel.occ >= 0) st->open.w[sel.occ >> 6] &= ~(1ull << (sel.occ & 63));        /* the word the chain answers is owed no longer */
        couple_strands(st, &fd, &sel.id, 1, -1, R_DISCOURSE);
        if (sel.kind != P_FOLLOW && owed(&st->open, &st->open) <= (int)(fw.enough * owed0 + 0.5)) { disposition = bit_count_and(&st->open, &st->open) ? "complete: what is still owed is within what the firmware leaves open" : "complete: every obligation is grounded"; break; }
        disposition = bit_count_and(&st->open, &st->open) ? "open: obligations remain" : "complete: every obligation is grounded";
    }
    if (nemit >= fw.emit) disposition = "the emission budget is spent";
    printf("REALIZE    \"%s\"\n           %s\n", out, disposition);

    /* ---- WITNESS: the turn, recorded */
    if (!read_only && outn) {
        Ref resp = text_ref(CTX[0], (const uint8_t *)out, outn); Ref tp[2] = { pr, resp }; tp[0].said = tp[1].said = 0; Ref turn = compose(tp, 2, (uint8_t)((pr.tier > resp.tier ? pr.tier : resp.tier) + 1));
        const lp_trust_class *uc = lp_trust_class_named("UserPromptContent"), *rc = lp_trust_class_named("ResponseContent");
        File f[2]; memset(f, 0, sizeof f);
        Ref p1[2] = { handle, turn }; p1[0].said = LP_SAID_TUPLE; p1[1].said = 0; Ref c1 = said_claim(compose(p1, 2, (uint8_t)((handle.tier > turn.tier ? handle.tier : turn.tier) + 1)));
        Ref p2[2] = { resp, pr }; p2[0].said = p2[1].said = 0; Ref c2 = said_claim(compose(p2, 2, (uint8_t)((resp.tier > pr.tier ? resp.tier : pr.tier) + 1)));
        Ref lap = text_ref(CTX[0], (const uint8_t *)"Laplace", 7);
        f[0].path = "the prompt"; f[0].witness = who; f[0].trust = uc ? uc->prior : 0.3; f[0].trunk = turn;
        Event e1 = { c1.id, c1.id, 1.0f, 1500.0f, 0.0f, (uint32_t)ordinal, EV_CLAIM, 0, { { 0 } }, 0 }; ev_push(&f[0].ev, &e1);
        f[1].path = "the response"; f[1].witness = lap; f[1].trust = rc ? rc->prior : 0.2; f[1].trunk = resp;
        Event e2 = { c2.id, c2.id, 1.0f, 1500.0f, 0.0f, 0, EV_CLAIM, 0, { { 0 } }, 0 }; ev_push(&f[1].ev, &e2);
        LoadStats ls = { 0 }; if (load(conninfo, 2, f, 2, &ls)) return 1;
        char ti[33]; id_text(&turn.id, ti);
        printf("WITNESS    turn %s, at %d in the session: %llu entities new, the user's [session, turn] and Laplace's [response, prompt]\n", ti, ordinal, (unsigned long long)ls.ent_rows);
        free(f[0].ev.e); free(f[1].ev.e);
    } else printf("WITNESS    %s\n", read_only ? "a read: nothing is witnessed" : "nothing emitted: nothing to witness");
    printf("\n%llu round trips, %llu for text   total %.1f ms\n", (unsigned long long)st->trips, (unsigned long long)reader_trips(st->rd), (now() - T) * 1000);
    field_free(&fd); free(bind); free(out); reader_free(st->rd); PQfinish(st->pg); free(st);
    return 0;
}
