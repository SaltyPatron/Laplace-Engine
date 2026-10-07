/* The forward program: one stateful program answers a turn of a session (Sequence: Forward, Sessions).
 *
 *   laplace turn [-d conninfo] [--firmware FILE] [--as USER] [--session NAME] [--seed N] [--read] prompt
 *
 * RESOLVE   the prompt is admitted as content, decomposed on the client; its occurrences are its constituents in
 *           order; the session is resolved from the record: its turns are the claims [session, turn] its user
 *           witnessed, in the order their positions in attestation give, and their constituents are the discourse.
 * COUPLE    the whole observation perturbs the web at once: every segment of it, the observations that hold each run
 *           and what follows the run in them; every strand that holds an occurrence, the prompt or a discourse
 *           entity, read at the firmware's k. The field keeps its kinds apart: what responds, by which route, from
 *           which occurrences, with what standing.
 *           Then the walks: from every word, the firmware's walkers step across rated strands, each strand taken as
 *           often as it tugs, going home as often as the firmware says; where they stand is how the rest of the
 *           prompt reaches an entity in more than one hop (a transformer's deeper layers). Every draw is BLAKE3 of the
 *           seed and the walker's place, so the walks replay bit for bit. And the context: the observations that hold
 *           each word and what else they hold, what the prompt's words are observed beside.
 * ORIENT    the joint interpretation: an occurrence is an obligation unless what is attested of it under the
 *           firmware's role kind says it pulls nothing; the interpretation is the responding entities that ground the
 *           most obligations together; two readings level there are told apart by how the other words' walkers reach
 *           them and how many of the other words' strands do; ambiguous only when nothing tells them apart.
 * ROUTE     the program: the firmware's hops, fan, tax and emission budget, over the oriented centres.
 * SCAN      best-first from the centres across rated strands, to the hop and fan limits: what is reached, by which
 *           strand, at what cost.
 * COMPOSE   the frontier: what was reached, folded by entity, its routes and supporting occurrences kept apart.
 * PROPOSE   the next constituents: what follows the active trajectory (the prompt and what has been emitted) as a run
 *           in what was observed; and what a result-bearing strand of an oriented entity says.
 * STEER     every proposal is weighed against every word still owed, through every channel: a strand of the word
 *           reaches it, the word's walkers reach it, it is observed beside the word more often than its base rate.
 *           The firmware's election order compares them key by key: the obligations they ground, continuity, how many
 *           channels agree, how often observed beside the words, how often the walkers reach it, confidence at k,
 *           the least shared. Typed, never one number; a hub never wins election.
 *           The pass's own work is a trajectory too (Sequence 20.12: the result is content, and the next step works
 *           from it): every position read, every reading chosen, every constituent emitted, in order. Its recent window
 *           is coupled by shape each step, the observed curves nearest it nominated by containment and by its centroid
 *           and measured by the firmware's measure; what each does next, after the place it meets the pass, is a
 *           proposal (continuation by analogy, not only by an exact suffix), its distance the shape key.
 * SELECT    the top. Proposals the evidence cannot tell apart, every key equal or within the walks' error, are a
 *           tie, and the firmware says what a tie gets: the first by ID, a draw, or a question.
 * REALIZE   its text.
 * WITNESS   the emitted constituent joins the active trajectory, the obligations it grounds close, and the coupling
 *           of the next step is recomputed from that state. At the end of the turn, unless --read, the prompt and the
 *           response are recorded as content, the turn [prompt, response] with them: the user witnesses the turn's
 *           place in the session ([session, turn], at its ordinal, UserPromptContent) and Laplace the response's
 *           dependence on the prompt ([response, prompt], ResponseContent).
 * Every stage's state is printed as the pass's trace. Nothing here changes a standing except by witnessing. */
#include "engine.h"
#include "blake3.h"
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
typedef struct { Cell *c; int n; size_t cap; lp_idmap *m; } Field;                  /* the cells, in the order they responded; found by ID */
static Cell *cell(Field *f, const lp_id *id){
    if (!f->m) f->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(f->m, id, &fresh);
    if (!fresh) return &f->c[i];
    lp_reserve((void **)&f->c, &f->cap, (size_t)f->n + 1, sizeof(Cell));
    Cell *x = &f->c[f->n++]; memset(x, 0, sizeof *x); x->id = *id; x->cost = INFINITY; return x;
}
static Cell *cell_find(const Field *f, const lp_id *id){ int64_t i = lp_idmap_find(f->m, id); return i < 0 ? NULL : &f->c[i]; }
static void field_free(Field *f){ free(f->c); lp_idmap_free(f->m); memset(f, 0, sizeof *f); }

/* The constituents of a composition, in order, its runs written out; an atom or an unknown ID is itself. */
static int constituents(const lp_id *id, lp_id *out, int cap){
    int n = (int)table_parts(id, out, (size_t)cap); if (!n) { out[0] = *id; return 1; }
    return n;
}

/* ---- the state a turn carries from one emitted constituent to the next */
typedef struct {
    const Firmware *fw; PGconn *pg; Reader *rd; lp_text *c;
    lp_id prompt; lp_id occ[MAXOCC]; int nocc; double role[MAXOCC]; int composed[MAXOCC];        /* the occurrences, how hard each pulls, which are compositions */
    lp_id disc[MAXOCC]; int ndisc;                                                                 /* the discourse: what the session's earlier turns hold */
    lp_id traj[4096]; int ntraj;                                                                   /* the active trajectory: the occurrences, then what has been emitted */
    Bits open;                                                                                     /* the obligations still open */
    uint64_t trips;
    uint64_t admitted;                                                        /* the prompt's entities its admission recorded new */
    uint8_t seed[32];                                                         /* every draw of the pass is BLAKE3 of this and the draw's place */
    int npos;                                                                 /* positions: the prompt's occurrences, then each word emitted (attention reads both) */
    lp_id pass[4096]; int npass;                                              /* the pass's own trajectory: what it read, chose and emitted, in order */
    lp_idmap *walk_at; int *walk_visits; size_t walk_cap; uint64_t walk_steps, walk_homes; int walk_read;    /* where each position's walkers stood, MAXOCC counts an entity */
    lp_idmap *walk_am; struct Adj_ *walk_adj; size_t walk_acap; int walk_nadj;  /* the strands of every place stood on, read once a pass */
    lp_idmap *ctx[MAXOCC]; int ctx_n[MAXOCC], ctx_hub[MAXOCC]; lp_idmap *ctx_all; int ctx_total;            /* what the observations holding each word hold */
} State;

/* A draw: uniform in [0, 1), from the pass's seed and where the draw is made. The same seed, the same draws, on every
 * machine: no generator state, nothing that depends on the order anything arrived in. */
static double draw(const State *st, uint32_t a, uint32_t b, uint32_t c){
    blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, st->seed, 32); uint32_t v[3] = { a, b, c }; blake3_hasher_update(&h, v, sizeof v);
    uint8_t o[8]; blake3_hasher_finalize(&h, o, 8); uint64_t x; memcpy(&x, o, 8); return (double)(x >> 11) * 0x1.0p-53;
}
/* Rows in content order: a result comes in the order the server's scans met its rows, which another install of the
 * same content meets differently; what is folded from it (a sum, the first of a level set) is folded in this order. */
static const PGresult *ORD_Q; static const int *ORD_C; static int ORD_N;
static int row_cmp(const void *a, const void *b){
    int x = *(const int *)a, y = *(const int *)b;
    for (int k = 0; k < ORD_N; k++) { int c = ORD_C[k], lx = PQgetlength(ORD_Q, x, c), ly = PQgetlength(ORD_Q, y, c), d = memcmp(PQgetvalue(ORD_Q, x, c), PQgetvalue(ORD_Q, y, c), (size_t)(lx < ly ? lx : ly));
        if (d) return d; if (lx != ly) return lx < ly ? -1 : 1; }
    return x < y ? -1 : x > y;
}
static int *rows_in_order(const PGresult *q, const int *cols, int ncols){
    int n = PQntuples(q), *o = malloc(sizeof(int) * (size_t)(n ? n : 1)); for (int i = 0; i < n; i++) o[i] = i;
    ORD_Q = q; ORD_C = cols; ORD_N = ncols; qsort(o, (size_t)n, sizeof(int), row_cmp); return o;
}

static PGresult *ask_st(State *st, const char *sql, Args *a){ st->trips++; return ask(st->pg, sql, a); }

/* ---- RESOLVE */
static void resolve_roles(State *st){
    Firmware *fw = (Firmware *)st->fw;
    const lp_layout *fl = lp_flags_map(NULL); const lp_field *alpha = fl ? lp_flags_field(fl, "Alphabetic") : NULL, *gc = fl ? lp_flags_field(fl, "General_Category") : NULL; int32_t nd = gc ? lp_flags_value(fl, gc, "Nd") : -1;
    for (int i = 0; i < st->nocc; i++) { st->role[i] = 1.0; st->composed[i] = table_find(&st->occ[i]) != NULL;
        if (!st->composed[i]) { int64_t cp = lp_tier0_codepoint(T0, &st->occ[i]);      /* a word of one letter is a word: by its Unicode properties, Alphabetic or a decimal number */
            if (cp >= 0 && ((alpha && lp_flags_get(fl, (uint32_t)cp, alpha)) || (gc && nd >= 0 && lp_flags_get(fl, (uint32_t)cp, gc) == (uint32_t)nd))) st->composed[i] = 1; }
        if (!st->composed[i]) { st->role[i] = 0; continue; }                                   /* a space, a mark: no pull of its own, no obligation */
        if (!fw->role_by[0]) continue;
        double w = role_of(st->pg, fw, &st->occ[i]); st->trips++; if (w >= 0) st->role[i] = w; }
    memset(&st->open, 0, sizeof st->open);
    for (int i = 0; i < st->nocc; i++) if (st->composed[i] && st->role[i] > 0) bit_set(&st->open, i);   /* an obligation until what is attested of it says it pulls nothing */
}

/* ---- COUPLE: the strands of the occurrences, the prompt and the discourse; and what follows the active trajectory */
typedef struct { lp_id id; double d; lp_id *v; int nv; } Curve;              /* an observed curve near the prompt's, and its vertices */
/* COUPLE through the extension's one native operator (Sequence 20.2): the strands, the containment and, when asked, the
 * shape of these entities, in one call. Each row keeps its route; here they are folded into the field by entity, the
 * routes and supporting occurrences kept apart. occ_of: the occurrence each entity is (-1: none); kind: the route a
 * strand counts as (R_CLAIM for the prompt, R_DISCOURSE for what was said before or emitted). Returns how many
 * nearest curves were kept in near. */
static int curve_order(const void *a, const void *b);
static double shape_measure(int shape, double shape_n, const double *a, size_t na, const double *b, size_t nb){
    switch (shape) { case FW_OUTLIERS: return lp_frechet4_outliers(a, na, b, nb, (unsigned)shape_n);
                     case FW_DTW: { size_t s; double d = lp_dtw4(a, na, b, nb, &s); return s ? d / (double)s : d; }
                     case FW_EDR: return (double)lp_edr4(a, na, b, nb, shape_n);
                     default: return lp_frechet4(a, na, b, nb); }
}
/* The shape: the curve of a run of entities against every observation nominated for it, each realized from its
 * constituents' coordinates and measured by the firmware's measure natively; the keep nearest, in content order. */
static int shape_measured(State *st, const lp_id *ids, int n, const Hold *h0, int nh0, Curve *near, int keep){
    if (n < 2 || keep <= 0) return 0; const Firmware *fw = st->fw;
    /* the GIN's nominations are the containers read already; the GiST's, the entities nearest the curve's centroid at a
     * tier that composes, the fan of them (as laplace_couple nominates) */
    Hold *h = malloc(sizeof(Hold) * (size_t)(nh0 + fw->fan + 1)); memcpy(h, h0, sizeof(Hold) * (size_t)nh0); int nh = nh0, extra = 0;
    { double *pc = malloc(sizeof(double) * 4 * (size_t)n); uint8_t *hc = malloc((size_t)n); coords_of(ids, n, pc, hc); double cen[4] = { 0 }; int m = 0;
      for (int k = 0; k < n; k++) if (hc[k]) { for (int d = 0; d < 4; d++) cen[d] += pc[4 * k + d]; m++; }
      if (m >= 2) { for (int d = 0; d < 4; d++) cen[d] /= m; uint8_t pt[64]; size_t pl = lp_ewkb_point4(cen, pt, sizeof pt);
          Args a = { 0 }; arg_raw(&a, pt, pl); arg_int(&a, fw->nearest);
          double tk = now(); PGresult *q = ask_st(st, "SELECT e.id FROM entity e WHERE e.tier >= 3 ORDER BY e.coord <~> ST_GeomFromEWKB($1::bytea), e.id LIMIT $2", &a);
          int nq = PQntuples(q); lp_id *gid = malloc(sizeof(lp_id) * (size_t)(nq ? nq : 1)); for (int r = 0; r < nq; r++) memcpy(gid[r].b, PQgetvalue(q, r, 0), 16); PQclear(q); args_free(&a);
          double tp = now(); int ng = 0; Hold *g = paths_of(gid, nq, &ng); for (int i = 0; i < ng && nh < nh0 + fw->fan + 1; i++) { h[nh++] = g[i]; extra++; } free(g); free(gid);
          if (getenv("LAPLACE_TIMES")) fprintf(stderr, "times: the GiST's %d nearest in %.1f ms, their paths in %.1f ms\n", nq, (tp - tk) * 1000, (now() - tp) * 1000); }
      free(pc); free(hc); }
    lp_idmap *seen = lp_idmap_new(); int *pick = malloc(sizeof(int) * (size_t)(nh ? nh : 1)), np_ = 0; lp_idmap *verts = lp_idmap_new();
    for (int i = 0; i < nh; i++) { if (h[i].claim || lp_id_eq(&h[i].entity, &st->prompt)) continue; bool f; lp_idmap_put(seen, &h[i].entity, &f); if (!f) continue; pick[np_++] = i; }
    lp_id **cv = malloc(sizeof(lp_id *) * (size_t)(np_ ? np_ : 1)); int *cn = malloc(sizeof(int) * (size_t)(np_ ? np_ : 1));
    for (int c = 0; c < np_; c++) { const Hold *x = &h[pick[c]]; size_t nv = lp_path_ids(x->path, (size_t)x->path_len, NULL, 0); cv[c] = malloc(sizeof(lp_id) * (nv ? nv : 1)); cn[c] = (int)lp_path_ids(x->path, (size_t)x->path_len, cv[c], nv);
        for (int k = 0; k < cn[c]; k++) { bool f; lp_idmap_put(verts, &cv[c][k], &f); } }
    for (int k = 0; k < n; k++) { bool f; lp_idmap_put(verts, &ids[k], &f); }
    size_t nv = lp_idmap_count(verts); double *xyz = malloc(sizeof(double) * 4 * (nv ? nv : 1)); uint8_t *has = malloc(nv ? nv : 1);
    coords_of(lp_idmap_keys(verts), (int)nv, xyz, has);
    double *pa = malloc(sizeof(double) * 4 * (size_t)n); size_t npa = 0;
    for (int k = 0; k < n; k++) { int64_t q = lp_idmap_find(verts, &ids[k]); if (q >= 0 && has[q]) { memcpy(pa + 4 * npa, xyz + 4 * q, 32); npa++; } }
    Curve *all = malloc(sizeof(Curve) * (size_t)(np_ ? np_ : 1)); int na = 0; double *b = malloc(sizeof(double) * 4 * 4096);
    for (int c = 0; c < np_ && npa >= 2; c++) { int m = 0; for (int k = 0; k < cn[c] && m < 4096; k++) { int64_t q = lp_idmap_find(verts, &cv[c][k]); if (q >= 0 && has[q]) { memcpy(b + 4 * m, xyz + 4 * q, 32); m++; } }
        if (m < 2) continue; all[na].id = h[pick[c]].entity; all[na].d = shape_measure(fw->shape, fw->shape_n, pa, npa, b, (size_t)m); all[na].v = cv[c]; all[na].nv = cn[c]; cv[c] = NULL; na++; }
    qsort(all, (size_t)na, sizeof(Curve), curve_order);
    int k = 0; for (int i = 0; i < na; i++) { if (k < keep && isfinite(all[i].d)) near[k++] = all[i]; else free(all[i].v); }
    for (int c = 0; c < np_; c++) free(cv[c]);
    for (int i = nh0; i < nh; i++) free(h[i].path);
    free(h); free(cv); free(cn); free(all); free(b); free(pa); free(xyz); free(has); free(pick); lp_idmap_free(seen); lp_idmap_free(verts);
    (void)extra; return k;
}
/* COUPLE through the Engine's leaf reads (holds_capped, every leaf at once on every core): for each entity, the claims
 * that hold it (its strands, the other end answering with the strand's standing) and the observations that hold it
 * (containment), a hub's never read; and the shape, measured here. */
static int couple(State *st, Field *fd, const lp_id *ids, int n, const int *occ_of, int kind, Curve *near, int keep){
    if (!n) return 0; const Firmware *fw = st->fw;
    if (!getenv("LAPLACE_COUPLE_SQL")) {
        lp_id *keys = malloc(sizeof(lp_id) * (size_t)n); int *of = malloc(sizeof(int) * (size_t)n), nk = 0;
        for (int o = 0; o < n; o++) { keys[nk] = ids[o]; of[nk++] = o; }
        int *hub = malloc(sizeof(int) * 2 * (size_t)(nk ? nk : 1)), nh = 0; Hold *h = holds_capped(keys, nk, fw->fan, 1, 3, hub, &nh); st->trips++;
        Ids rn = { 0 };
        for (int i = 0; i < nh; i++) { int s = h[i].src; if (s < 0 || s >= nk) continue; int o = of[s];
            int occ = occ_of ? occ_of[o] : -1; double pull = occ >= 0 && occ < st->nocc ? st->role[occ] : kind == R_CLAIM ? 1.0 : 0.5;
            if (lp_id_eq(&h[i].entity, &st->prompt)) continue;
            if (h[i].claim) { if (!h[i].stood) continue; path_into(&rn, lp_path_of(h[i].path, (size_t)h[i].path_len)); if (rn.n < 2) continue;
                if (lp_tuple_middle_any(rn.v, rn.n, fw->id.refuse, (size_t)fw->nrefuse_predicate)) continue;
                int other = lp_tuple_other(rn.v, rn.n, &ids[o]); if (other < 0 || lp_id_eq(&rn.v[other], &st->prompt)) continue;
                lp_id p3[3] = { ids[o], rn.n >= 3 ? rn.v[1] : ids[o], rn.v[other] };
                double conf = lp_confidence(&h[i].r, fw->k) * strand_weight(fw, fw->id.weigh, p3, 3);
                Cell *x = cell(fd, &rn.v[other]); x->force += conf * pull; x->routes[kind]++; if (occ >= 0) bit_set(&x->support, occ);
                if (!x->has_r || conf > lp_confidence(&x->r, fw->k)) { x->r = h[i].r; x->has_r = 1; x->via = p3[0]; x->rel = p3[1]; } }
            else { Cell *x = cell(fd, &h[i].entity); x->segment = 1; x->tier = h[i].tier; x->force += pull; x->routes[R_CONTAIN]++; if (occ >= 0) bit_set(&x->support, occ); } }
        lp_vec_free(&rn);
        int nn = keep > 0 ? shape_measured(st, ids, n, h, nh, near, keep) : 0;
        holds_free(h, nh); free(hub); free(keys); free(of); return nn;
    }
    Args a = { 0 }; arg_ids(&a, ids, (size_t)n); arg_int(&a, fw->fan); arg_refused(&a); arg_int(&a, keep > 0 ? fw->shape : -1); arg_f64(&a, fw->shape_n); arg_int(&a, keep > 0 ? keep + 1 : keep);
    PGresult *q = ask_st(st, "SELECT entity, occ, route, rating, deviation, volatility, via, rel, tier, distance, vertices FROM laplace_couple($1::blake3[], $2::bigint, $3::blake3[], $4::smallint, $5::float8, $6::integer)", &a);
    int *held = calloc((size_t)n + 1, sizeof(int)), nn = 0;
    for (int r = 0; r < PQntuples(q); r++) if (col_int(q, r, 2) == 0) { int o = (int)col_int(q, r, 1); if (o >= 1 && o <= n) held[o - 1]++; }
    static const int OC[] = { 2, 1, 9, 0, 6, 7 }; int *ord = rows_in_order(q, OC, 6);      /* route, occurrence, distance, entity, via, relation */
    for (int r_ = 0; r_ < PQntuples(q); r_++) { int r = ord[r_];
        const lp_id *id = col_id(q, r, 0); int o = (int)col_int(q, r, 1) - 1, route = (int)col_int(q, r, 2);
        if (lp_id_eq(id, &st->prompt)) continue;                            /* the prompt, admitted, is not its own response: not even its nearest curve */
        int occ = o >= 0 && o < n && occ_of ? occ_of[o] : -1; double pull = occ >= 0 && occ < st->nocc ? st->role[occ] : kind == R_CLAIM ? 1.0 : 0.5;
        if (route == 2) { if (nn < keep) { Curve *c = &near[nn++]; c->id = *id; c->d = col_f64(q, r, 9);
                const uint8_t *v = (const uint8_t *)PQgetvalue(q, r, 10); size_t vl = (size_t)PQgetlength(q, r, 10), na = lp_pg_ids_read(v, vl, NULL, 0);
                c->v = malloc(sizeof(lp_id) * (na ? na : 1)); c->nv = (int)lp_pg_ids_read(v, vl, c->v, na); }
            continue; }
        Cell *x = cell(fd, id);
        if (route == 1) { x->segment = 1; x->tier = (int)col_int(q, r, 8); x->force += pull; x->routes[R_CONTAIN]++; if (occ >= 0) bit_set(&x->support, occ); continue; }
        lp_rating rt = col_rating(q, r, 3); lp_id part[3] = { *col_id(q, r, 6), *col_id(q, r, 7), *id };
        double conf = lp_confidence(&rt, fw->k) * strand_weight(fw, fw->id.weigh, part, 3);
        x->force += conf * pull; x->routes[kind]++; if (occ >= 0) bit_set(&x->support, occ);
        if (o >= 0 && held[o] > fw->fan) x->hub = 1;                                               /* what so many strands hold is reached, not crossed */
        if (!x->has_r || conf > lp_confidence(&x->r, fw->k)) { x->r = rt; x->has_r = 1; x->via = part[0]; x->rel = part[1]; }
    }
    PQclear(q); args_free(&a); free(held); free(ord); return nn;
}
/* What follows the end of the active trajectory, as a run, in what was observed: the longest observed suffix first. */
typedef struct { lp_id id; long times; int len; } Next;
static int follows(State *st, Next *out, int cap){
    int from = st->ntraj > 24 ? st->ntraj - 24 : 0, n = st->ntraj - from; if (n <= 0) return 0;
    Args a = { 0 }; arg_ids(&a, st->traj + from, (size_t)n); arg_int(&a, st->fw->fan);
    /* every run that ends the trajectory, not only the longest: as attention reads every position, a word that follows
     * only the last two is still a proposal, at the run it follows (its continuity) */
    PGresult *q = ask_st(st, "SELECT i, j, next, times FROM laplace_forward($1::blake3[], $2::bigint) WHERE j = array_length($1::blake3[], 1) AND next IS NOT NULL ORDER BY (j - i) DESC, times DESC, next", &a);
    int m = 0; lp_idmap *seen = lp_idmap_new();
    for (int r = 0; r < PQntuples(q) && m < cap; r++) { int len = (int)(col_int(q, r, 1) - col_int(q, r, 0) + 1); bool fresh; lp_idmap_put(seen, col_id(q, r, 2), &fresh); if (!fresh) continue;
        out[m].id = *col_id(q, r, 2); out[m].times = (long)col_int(q, r, 3); out[m].len = len; m++; }
    lp_idmap_free(seen); PQclear(q); args_free(&a); return m;
}

/* ---- the pass's own trajectory: a composition is appended as the pass reaches it (an atom, a space or a mark, carries
 * no shape of its own), the same entity twice in a row once */
static void pass_add(State *st, const lp_id *id){
    if (st->npass >= 4096 || lp_tier0_codepoint(T0, id) >= 0) return;
    if (st->npass && lp_id_eq(&st->pass[st->npass - 1], id)) return;
    st->pass[st->npass++] = *id;
}
static int curve_order(const void *a, const void *b){ const Curve *x = a, *y = b; return x->d < y->d ? -1 : x->d > y->d ? 1 : memcmp(&x->id, &y->id, 16); }
/* The observed curves nearest a run of entities by the firmware's measure, through the extension's native shape
 * operator (laplace_couple's shape route: the GIN's containers of the run and the GiST's nearest to its centroid,
 * realized and measured natively); only the shape is read here. In content order: distance, then ID. */
static int shape_near(State *st, const lp_id *ids, int n, Curve *near, int keep){
    if (n < 2 || keep <= 0) return 0; const Firmware *fw = st->fw;
    if (!getenv("LAPLACE_COUPLE_SQL")) { int *hub = malloc(sizeof(int) * 2 * (size_t)n), nh = 0; Hold *h = holds_capped(ids, n, fw->fan, 0, 2, hub, &nh); st->trips++;
        int k = shape_measured(st, ids, n, h, nh, near, keep); holds_free(h, nh); free(hub); return k; }
    Args a = { 0 }; arg_ids(&a, ids, (size_t)n); arg_int(&a, fw->fan); arg_refused(&a); arg_int(&a, fw->shape); arg_f64(&a, fw->shape_n); arg_int(&a, keep + 4);
    PGresult *q = ask_st(st, "SELECT entity, occ, route, rating, deviation, volatility, via, rel, tier, distance, vertices FROM laplace_couple($1::blake3[], $2::bigint, $3::blake3[], $4::smallint, $5::float8, $6::integer) WHERE route = 2", &a);
    int m = PQntuples(q), nn = 0; Curve *all = malloc(sizeof(Curve) * (size_t)(m ? m : 1));
    for (int r = 0; r < m; r++) { const lp_id *id = col_id(q, r, 0); if (lp_id_eq(id, &st->prompt)) continue; Curve *c = &all[nn++]; c->id = *id; c->d = col_f64(q, r, 9);
        const uint8_t *v = (const uint8_t *)PQgetvalue(q, r, 10); size_t vl = (size_t)PQgetlength(q, r, 10), na = lp_pg_ids_read(v, vl, NULL, 0);
        c->v = malloc(sizeof(lp_id) * (na ? na : 1)); c->nv = (int)lp_pg_ids_read(v, vl, c->v, na); }
    qsort(all, (size_t)nn, sizeof(Curve), curve_order);
    int k = nn < keep ? nn : keep; for (int i = 0; i < k; i++) near[i] = all[i]; for (int i = k; i < nn; i++) free(all[i].v);
    free(all); PQclear(q); args_free(&a); return k;
}

/* ---- the walks (Monte Carlo over the web, deterministic): from every word, the firmware's walkers; a step takes a
 * strand of where the walker stands as often as that strand tugs (its confidence at k, by the firmware's weights), or
 * goes home; an entity more than the fan holds is reached and not crossed: a walker there goes home. The strands of
 * every place stood on are read once, a set a step, and kept in content order, so a draw picks the same strand on
 * every install. What is counted is how often each word's walkers stand on each entity. */
typedef struct { lp_id to; double w; } Hop_;
typedef struct Adj_ { Hop_ *h; int n; size_t cap; int hub; } Adj;
static int hop_cmp(const void *a, const void *b){ const Hop_ *x = a, *y = b; int c = memcmp(&x->to, &y->to, 16); return c ? c : (x->w < y->w ? -1 : x->w > y->w); }
static int visits(const State *st, const lp_id *id, int o){ if (!st->walk_at) return 0; int64_t i = lp_idmap_find(st->walk_at, id); return i < 0 ? 0 : st->walk_visits[(size_t)i * MAXOCC + (size_t)o]; }
static void walk(State *st, int from, int to){                                 /* the walkers of positions [from, to) */
    const Firmware *fw = st->fw; if (fw->walks <= 0 || fw->steps <= 0) return;
    int org[MAXOCC], no = 0; for (int i = from; i < to; i++) if (st->composed[i] && st->role[i] > 0) org[no++] = i;
    if (!no) return;
    int W = fw->walks, nw = no * W; lp_id *pos = malloc(sizeof(lp_id) * (size_t)nw), *need = malloc(sizeof(lp_id) * (size_t)nw);
    for (int o = 0; o < no; o++) for (int w = 0; w < W; w++) pos[o * W + w] = st->occ[org[o]];
    if (!st->walk_am) st->walk_am = lp_idmap_new(); if (!st->walk_at) st->walk_at = lp_idmap_new();
    lp_idmap *am = st->walk_am; Adj *adj = st->walk_adj; size_t acap = st->walk_acap; int nadj = st->walk_nadj; Ids rn = { 0 }; Args a = { 0 };
    for (int s = 0; s < fw->steps; s++) {
        int nn = 0;
        for (int i = 0; i < nw; i++) { bool fresh; lp_idmap_put(am, &pos[i], &fresh);
            if (fresh) { lp_reserve((void **)&adj, &acap, (size_t)nadj + 1, sizeof(Adj)); memset(&adj[nadj++], 0, sizeof(Adj)); need[nn++] = pos[i]; } }
        if (nn) {                                                             /* the strands of every place not stood on before: one set, every leaf at once, a hub's never read */
            int *hub = malloc(sizeof(int) * 2 * (size_t)nn), nh = 0; Hold *hh = holds_capped(need, nn, fw->walk_fan, 1, 1, hub, &nh); st->trips++;
            int *held = calloc((size_t)nn, sizeof(int)); for (int e = 0; e < nn; e++) held[e] = hub[2 * e] ? fw->walk_fan + 1 : 0;
            for (int r = 0; r < nh; r++) { int e = hh[r].src; if (e < 0 || e >= nn || held[e] > fw->walk_fan || !hh[r].claim || !hh[r].stood) continue;
                path_into(&rn, lp_path_of(hh[r].path, (size_t)hh[r].path_len));
                if (lp_tuple_middle_any(rn.v, rn.n, fw->id.refuse, (size_t)fw->nrefuse_predicate)) continue;
                int other = lp_tuple_other(rn.v, rn.n, &need[e]); if (other < 0 || lp_id_eq(&rn.v[other], &need[e])) continue;
                lp_rating rt = hh[r].r; double w = lp_confidence(&rt, fw->k) * strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n); if (!(w > 0)) continue;
                Adj *x = &adj[lp_idmap_find(am, &need[e])]; lp_reserve((void **)&x->h, &x->cap, (size_t)x->n + 1, sizeof(Hop_)); x->h[x->n++] = (Hop_){ rn.v[other], w }; }
            for (int e = 0; e < nn; e++) { Adj *x = &adj[lp_idmap_find(am, &need[e])]; x->hub = held[e] > fw->walk_fan; if (x->n > 1) qsort(x->h, (size_t)x->n, sizeof(Hop_), hop_cmp); }
            st->walk_read += nn; free(held); free(hub); holds_free(hh, nh);
        }
        for (int o = 0; o < no; o++) for (int w = 0; w < W; w++) { int i = o * W + w; const Adj *x = &adj[lp_idmap_find(am, &pos[i])];
            if (draw(st, (uint32_t)org[o], (uint32_t)w, (uint32_t)(2 * s)) < fw->restart || x->hub || !x->n) { pos[i] = st->occ[org[o]]; st->walk_homes++; continue; }
            double tot = 0; for (int k = 0; k < x->n; k++) tot += x->h[k].w;
            double u = draw(st, (uint32_t)org[o], (uint32_t)w, (uint32_t)(2 * s + 1)) * tot; int k = 0; while (k + 1 < x->n && u >= x->h[k].w) { u -= x->h[k].w; k++; }
            pos[i] = x->h[k].to; st->walk_steps++;
            bool fresh; size_t vi = lp_idmap_put(st->walk_at, &pos[i], &fresh);
            if (fresh) { size_t need_ = (vi + 1) * MAXOCC, old = st->walk_cap; lp_reserve((void **)&st->walk_visits, &st->walk_cap, need_, sizeof(int)); memset(st->walk_visits + old, 0, (st->walk_cap - old) * sizeof(int)); }
            st->walk_visits[vi * MAXOCC + (size_t)org[o]]++; }
        st->walk_adj = adj; st->walk_acap = acap; st->walk_nadj = nadj;     /* kept: a later position's walkers read only where no one has stood */
    }
    free(pos); free(need); lp_vec_free(&rn); args_free(&a);
}
/* ---- the context: for each word, the observations that hold it (not claims) and what else they hold, counted once an
 * observation. A word held by more than the fan is a hub and has none: reached, not crossed. ctx_all counts each
 * observation once however many of the words it holds: the base rate a word is observed at, in this context. */
static lp_idmap *CTX_OBS;                                                     /* every observation counted into ctx_all, once */
static void context(State *st, int from, int to){                              /* the context of positions [from, to) */
    if (!CTX_OBS) CTX_OBS = lp_idmap_new(); if (!st->ctx_all) st->ctx_all = lp_idmap_new(); lp_idmap *obs = CTX_OBS; Ids ids = { 0 };
    /* every position's observations in one set, every leaf at once; a position more than the fan holds is a hub, its
     * observations never read. The same word again shares its context. */
    lp_id keys[MAXOCC]; int at[MAXOCC], nk = 0;
    for (int i = from; i < to; i++) { st->ctx[i] = NULL; st->ctx_n[i] = 0; st->ctx_hub[i] = 0; at[i - from] = -1;
        if (!st->composed[i] || st->role[i] <= 0) continue;
        int dup = -1; for (int j = 0; j < from && dup < 0; j++) if (lp_id_eq(&st->occ[j], &st->occ[i]) && (st->ctx[j] || st->ctx_hub[j])) dup = j;
        if (dup >= 0) { st->ctx[i] = st->ctx[dup]; st->ctx_n[i] = st->ctx_n[dup]; st->ctx_hub[i] = st->ctx_hub[dup]; continue; }
        int k = -1; for (int z = 0; z < nk; z++) if (lp_id_eq(&keys[z], &st->occ[i])) k = z;
        if (k < 0) { keys[nk] = st->occ[i]; k = nk++; } at[i - from] = k; }
    if (!nk) { lp_vec_free(&ids); return; }
    int *hub = malloc(sizeof(int) * 2 * (size_t)nk), nh = 0; Hold *h = holds_capped(keys, nk, st->fw->fan, 0, 2, hub, &nh); st->trips++;
    lp_idmap **km = calloc((size_t)nk, sizeof *km); int *kn = calloc((size_t)nk, sizeof(int));
    for (int k = 0; k < nk; k++) if (!hub[2 * k + 1]) km[k] = lp_idmap_new();
    for (int z = 0; z < nh; z++) { int k = h[z].src; if (k < 0 || k >= nk || !km[k] || h[z].claim) continue; kn[k]++;
        bool once; lp_idmap_put(obs, &h[z].entity, &once);
        size_t nv = lp_path_ids(h[z].path, (size_t)h[z].path_len, NULL, 0); lp_vec_reserve(&ids, nv ? nv : 1); nv = lp_path_ids(h[z].path, (size_t)h[z].path_len, ids.v, nv);
        lp_idmap *here = lp_idmap_new();
        for (size_t v = 0; v < nv; v++) { bool f1; lp_idmap_put(here, &ids.v[v], &f1); if (!f1) continue;
            bool f; (*lp_idmap_value(km[k], lp_idmap_put(km[k], &ids.v[v], &f)))++;
            if (once) (*lp_idmap_value(st->ctx_all, lp_idmap_put(st->ctx_all, &ids.v[v], &f)))++; }
        lp_idmap_free(here); if (once) st->ctx_total++; }
    for (int i = from; i < to; i++) { int k = at[i - from]; if (k < 0) continue; if (hub[2 * k + 1]) { st->ctx_hub[i] = 1; continue; } st->ctx[i] = km[k]; st->ctx_n[i] = kn[k]; }
    holds_free(h, nh); free(hub); free(km); free(kn); lp_vec_free(&ids);
}
static int beside(const State *st, int i, const lp_id *id){ if (!st->ctx[i]) return 0; int64_t k = lp_idmap_find(st->ctx[i], id); return k < 0 ? 0 : (int)*lp_idmap_value(st->ctx[i], (size_t)k); }
/* Observed beside word i more often than its base rate in this context, by the firmware's lift, and more than once. */
static int beside_grounds(const State *st, int i, const lp_id *id){
    int c = beside(st, i, id); if (c < 2 || !st->ctx_n[i] || !st->ctx_total) return 0;
    int64_t k = lp_idmap_find(st->ctx_all, id); double base = k < 0 ? 0 : (double)*lp_idmap_value(st->ctx_all, (size_t)k) / st->ctx_total;
    return base > 0 && ((double)c / st->ctx_n[i]) >= st->fw->lift * base;
}

/* ---- a fork of a chain: which branch the rest of the prompt supports (a word's senses, a sense's synsets). A branch's
 * record set is what its strands reach in two hops (the sense, its synset, the synset's gloss and examples, its
 * relations) and the words the observations among them hold. The context is the prompt's other words. A branch holds a
 * word when its record set does, weighed by how hard the word pulls: the intersection Research: Trust measured at 66.2
 * on the WSD sets. Then how often the other words' walkers stood on the branch's record set, within the walks' error.
 * When neither tells the branches apart, -1: the chain keeps its own order (the witness's, a sense's frequency). */
typedef struct { State *st; const lp_id *word; int last_hold, last_n; } ForkCtx;
static int fork_choice(void *vc, const Claim *cl, int n){
    ForkCtx *fc = vc; State *st = fc->st; const Firmware *fw = st->fw;
    lp_id *ends = malloc(sizeof(lp_id) * (size_t)n); for (int k = 0; k < n; k++) ends[k] = cl[k].part[cl[k].np - 1];
    lp_idmap **rec = calloc((size_t)n, sizeof *rec); for (int k = 0; k < n; k++) { bool f; rec[k] = lp_idmap_new(); lp_idmap_put(rec[k], &ends[k], &f); }
    /* hop one: each branch's strands; hop two: the strands of what they reach, words left out (a word's strands are its own record, not the branch's) */
    /* both hops every leaf at once, a hub (a relation, a part of speech, a language: more strands than the fan) reached and its strands never read */
    int *hb = malloc(sizeof(int) * 2 * (size_t)n), m1 = 0; Hold *c1 = holds_capped(ends, n, fw->fan, 0, 1, hb, &m1); st->trips++; Ids rv = { 0 };
    lp_id *mid = malloc(sizeof(lp_id) * (size_t)(m1 + 1)); int *mof = malloc(sizeof(int) * (size_t)(m1 + 1)), nm = 0;
    for (int r = 0; r < m1; r++) { int b = c1[r].src; if (b < 0 || b >= n || !c1[r].claim) continue; path_into(&rv, lp_path_of(c1[r].path, (size_t)c1[r].path_len));
        int o = lp_tuple_other(rv.v, rv.n, &ends[b]); if (o < 0) continue;     /* the strand's other end; its middle (a relation) is not the branch's record */
        const lp_id *x = &rv.v[o]; bool f; lp_idmap_put(rec[b], x, &f);
        if (f && !lp_id_eq(x, fc->word) && !table_find(x)) { int dup = 0; for (int z = 0; z < nm && !dup; z++) dup = lp_id_eq(&mid[z], x) && mof[z] == b; if (!dup) { mid[nm] = *x; mof[nm++] = b; } } }
    holds_free(c1, m1); free(hb);
    int m2 = 0, *hb2 = malloc(sizeof(int) * 2 * (size_t)(nm ? nm : 1)); Hold *c2 = nm ? holds_capped(mid, nm, fw->fan, 0, 1, hb2, &m2) : NULL; if (nm) st->trips++;
    for (int r = 0; r < m2; r++) { int s_ = c2[r].src; if (s_ < 0 || s_ >= nm || !c2[r].claim) continue; path_into(&rv, lp_path_of(c2[r].path, (size_t)c2[r].path_len));
        int o = lp_tuple_other(rv.v, rv.n, &mid[s_]); if (o < 0) continue; bool f; lp_idmap_put(rec[mof[s_]], &rv.v[o], &f); }
    holds_free(c2, m2); free(hb2); lp_vec_free(&rv); Claim *c1_ = NULL, *c2_ = NULL; int *s1 = NULL, *s2 = NULL; (void)c1_; (void)c2_;
    /* the words of every observation a record set holds, its gloss and its examples: each composition above a word is
     * opened, level by level, down to the words (a composition of atoms alone is a word, and is not opened) */
    lp_id parts[256]; size_t *done = calloc((size_t)n, sizeof(size_t));
    for (int level = 0; level < 4; level++) {
        for (int k = 0; k < n; k++) for (size_t i = done[k]; i < lp_idmap_count(rec[k]); i++) reader_want(st->rd, lp_idmap_key(rec[k], i));
        int grew = 0;
        for (int k = 0; k < n; k++) { size_t cnt = lp_idmap_count(rec[k]);
            for (size_t i = done[k]; i < cnt; i++) { lp_id x = *lp_idmap_key(rec[k], i); size_t np = reader_parts(st->rd, &x, parts, 256); if (np > 256) np = 256;
                size_t atoms = 0; for (size_t v = 0; v < np; v++) atoms += lp_tier0_codepoint(T0, &parts[v]) >= 0;
                if (!np || atoms == np) continue;                                     /* a word: its letters are not the branch's record */
                for (size_t v = 0; v < np; v++) { if (lp_tier0_codepoint(T0, &parts[v]) >= 0) continue; bool f; lp_idmap_put(rec[k], &parts[v], &f); grew |= f; } }
            done[k] = cnt; }
        if (!grew) break; }
    free(done);
    /* the context: the prompt's other words, and what the pass has emitted, each once. As measured (Research: Trust,
     * role trust in word sense disambiguation): a shared word counts by how hard it pulls (its role) and by how much it
     * informs (its containers: a word held by few observations says more than one held by many; a hub, more than the
     * fan holds, says nothing and is reached, not crossed); the overlap is normalized by the size of the record set, as
     * a cosine normalizes a dot product, or a sense with more records shares something with every sentence; and it is
     * added to the branch's log prevalence, here the order its witness gives the branches (a sense's frequency order,
     * until its counts are read). Then the walks from the other words over the record set, normalized the same way,
     * within their standard error. */
    double *hold = calloc((size_t)n, sizeof(double)), *walks = calloc((size_t)n, sizeof(double)), *score = calloc((size_t)n, sizeof(double));
    for (int k = 0; k < n; k++) { size_t cnt = lp_idmap_count(rec[k]); double norm = sqrt((double)(cnt ? cnt : 1));
        for (int j = 0; j < st->npos; j++) { if (!st->composed[j] || st->role[j] <= 0 || lp_id_eq(&st->occ[j], fc->word)) continue;
            int seen = 0; for (int i = 0; i < j && !seen; i++) seen = lp_id_eq(&st->occ[i], &st->occ[j]); if (seen) continue;
            double info = st->ctx_hub[j] ? 0 : log((double)(fw->fan + 1) / (1.0 + st->ctx_n[j]));
            if (info > 0 && lp_idmap_find(rec[k], &st->occ[j]) >= 0) hold[k] += st->role[j] * info;
            for (size_t i = 0; i < cnt; i++) walks[k] += st->role[j] * visits(st, lp_idmap_key(rec[k], i), j); }
        hold[k] /= norm; walks[k] /= norm;
        int rank = cl[k].position > 0 ? cl[k].position : k + 1; score[k] = -log((double)rank) + hold[k]; }
    int best = 0; for (int k = 1; k < n; k++) { double tol = fw->sure * sqrt((walks[k] + walks[best]) / 4.0);
        if (score[k] > score[best] || (score[k] == score[best] && walks[k] > walks[best] + tol)) best = k; }
    int told = 0; for (int k = 0; k < n; k++) if (k != best && (score[best] > score[k] || walks[best] > walks[k] + fw->sure * sqrt((walks[k] + walks[best]) / 4.0))) told++;
    int pick = told == n - 1 ? best : -1;
    if (getenv("LAPLACE_TIMES")) for (int k = 0; k < n; k++) { char *tx = reader_text(st->rd, &ends[k], 50); fprintf(stderr, "fork: %s%-50s record %zu, holds %.3f, prior %.2f, score %.3f, walks %.2f\n", k == pick ? "* " : "  ", tx, lp_idmap_count(rec[k]), hold[k], -log((double)(cl[k].position > 0 ? cl[k].position : k + 1)), score[k], walks[k]); free(tx); }
    fc->last_hold = (int)(hold[best] * 1000 + 0.5); fc->last_n = n;
    for (int k = 0; k < n; k++) lp_idmap_free(rec[k]);
    free(rec); free(ends); free(s1); free(s2); free(mid); free(mof); free(hold); free(walks); free(score);
    return pick;
}
/* A chain the firmware names, followed from a word (chain_follow), the oriented reading taken where the chain passes
 * through it, and at every other fork the branch the context supports; *rating: the standing of its last strand. */
static int chain_from(State *st, const lp_id *word, const lp_id *reading, lp_id *answer, lp_rating *rating){
    Claim last; int alt; ForkCtx fc = { st, word, 0, 0 }; chain_follow(st->pg, st->rd, (Firmware *)st->fw, word, reading, NULL, fork_choice, &fc, answer, &last, &alt);
    if (alt < 0) return 0;
    *rating = last.r; return 1;
}
/* ---- SCAN: best-first from the centres, across rated strands */
static void scan(State *st, Field *fd, const lp_id *centre, int nc){
    const Firmware *fw = st->fw; lp_frontier *fr = lp_frontier_new(); Ids rn = { 0 }; Args a = { 0 };
    for (int i = 0; i < nc; i++) lp_frontier_reach(fr, &centre[i], NULL, NULL, 0, 0, 0);
    int rounds = 0; const lp_reached *x;
    while (rounds++ < fw->hops) {
        lp_reached batch[16]; int n = 0; while (n < 16 && (x = lp_frontier_next(fr))) batch[n++] = *x; if (!n) break;      /* the nearest sixteen a round */
        lp_id ids[64]; int m = 0, who[64]; for (int i = 0; i < n; i++) if ((int)batch[i].hops < fw->hops) { ids[m] = batch[i].id; who[m++] = i; }
        if (!m) break;
        int hub[128], nh = 0; Hold *hh = holds_capped(ids, m, fw->fan, 1, 1, hub, &nh); st->trips++;      /* every leaf at once, in content order, a hub's never read */
        for (int r = 0; r < nh; r++) { int e = hh[r].src; if (e < 0 || e >= m || hub[2 * e] || !hh[r].claim || !hh[r].stood) continue;
            const lp_reached *at = &batch[who[e]]; path_into(&rn, lp_path_of(hh[r].path, (size_t)hh[r].path_len));
            if (lp_tuple_middle_any(rn.v, rn.n, fw->id.refuse, (size_t)fw->nrefuse_predicate)) continue;
            int other = lp_tuple_other(rn.v, rn.n, &at->id); if (other < 0) continue;
            lp_rating rt = hh[r].r;
            double sw = strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n); if (sw <= 0) continue;
            double cost = at->cost + lp_cost(&rt, fw->k, fw->lambda) - log(sw);
            lp_frontier_reach(fr, &rn.v[other], &at->id, &hh[r].entity, cost, 0, at->hops + 1);
            Cell *cx = cell(fd, &rn.v[other]); cx->routes[R_SCAN]++; if (cost < cx->cost) { cx->cost = cost; if (!cx->has_r) { cx->r = rt; cx->has_r = 1; cx->via = at->id; cx->rel = rn.n >= 3 ? rn.v[1] : at->id; } }
        }
        holds_free(hh, nh);
    }
    lp_frontier_free(fr); lp_vec_free(&rn); args_free(&a);
}

/* ---- the proposals of a step, and their election */
enum { P_FOLLOW, P_CHAIN, P_SHAPE };
typedef struct { lp_id id; int kind, grounds, cont; double conf, hub; long times; int occ; int agree; long cooc, walk; Bits by[3]; double shape; } Prop;     /* shape: the distance of the nearest analogous curve that proposes it (INFINITY: none) */     /* occ: the occurrence a chain answers, or -1; by: which obligations each channel grounds */
enum { CH_STRAND, CH_WALK, CH_BESIDE };
/* A proposal against every word still owed, through every channel (the attention of a transformer, read from the
 * records): a strand of the word reaches it (the field's support), the word's walkers stand on it, it is observed beside
 * the word more than its base rate. grounds: what the union owes, weighed by role; agree: how many channels ground
 * anything; cooc, walk: how often, over the words owed. */
static void weigh_prop(const State *st, const Field *fd, Prop *p){
    const Cell *x = cell_find(fd, &p->id); memset(p->by, 0, sizeof p->by); p->cooc = p->walk = 0;
    int atom = lp_tier0_codepoint(T0, &p->id) >= 0;                          /* a letter, a space, a mark: it may continue a run, it grounds nothing */
    for (int j = 0; j < st->npos; j++) { if (lp_id_eq(&st->occ[j], &p->id)) continue;
        int v = visits(st, &p->id, j), b = beside(st, j, &p->id); p->walk += v; p->cooc += b;      /* how the whole trajectory so far, prompt and emitted, attends to it */
        if (atom || j >= st->nocc || !((st->open.w[j >> 6] >> (j & 63)) & 1)) continue;            /* grounding: only what is still owed, and only by a composition */
        if (x && ((x->support.w[j >> 6] >> (j & 63)) & 1)) bit_set(&p->by[CH_STRAND], j);
        if (v >= 2) bit_set(&p->by[CH_WALK], j);
        if (beside_grounds(st, j, &p->id)) bit_set(&p->by[CH_BESIDE], j); }
    Bits u; memset(&u, 0, sizeof u); p->agree = 0;
    for (int c = 0; c < 3; c++) { int any = 0; for (int w = 0; w < MAXOCC / 64; w++) { u.w[w] |= p->by[c].w[w]; any |= p->by[c].w[w] != 0; } p->agree += any; }
    if (p->kind == P_CHAIN && p->occ >= 0) bit_set(&u, p->occ);              /* a chain's answer grounds the word it answers */
    p->grounds = owed(&u, &st->open);
}
static const Firmware *ELECT;
static int key_cmp(const Prop *x, const Prop *y, int k){                      /* -1: x first */
    switch (k) {
    case FW_E_GROUNDS:    return x->grounds != y->grounds ? (x->grounds > y->grounds ? -1 : 1) : 0;
    case FW_E_CONTINUITY: return x->cont != y->cont ? (x->cont > y->cont ? -1 : 1) : 0;
    case FW_E_AGREE:      return x->agree != y->agree ? (x->agree > y->agree ? -1 : 1) : 0;
    case FW_E_COOCCUR:    return x->cooc != y->cooc ? (x->cooc > y->cooc ? -1 : 1) : 0;
    case FW_E_WALKS:      return x->walk != y->walk ? (x->walk > y->walk ? -1 : 1) : 0;
    case FW_E_SHAPE:      return x->shape != y->shape ? (x->shape < y->shape ? -1 : 1) : 0;
    case FW_E_CONFIDENCE: return x->conf != y->conf ? (x->conf > y->conf ? -1 : 1) : 0;
    case FW_E_SHARED:     return x->hub != y->hub ? (x->hub < y->hub ? -1 : 1) : 0;
    } return 0;
}
static int elect(const void *a, const void *b){                               /* the firmware's election order, key by key; then the ID */
    const Prop *x = a, *y = b;
    for (int i = 0; i < ELECT->nelect; i++) { int c = key_cmp(x, y, ELECT->elect[i]); if (c) return c; }
    return memcmp(&x->id, &y->id, 16);
}
/* Whether the evidence cannot tell y from x: every key of the election equal, the walks' counts within the firmware's
 * standard errors of each other (a count of n walkers' visits is uncertain by about its square root), and confidence
 * within the firmware's temperature. */
static int cannot_tell(const Prop *x, const Prop *y, const Firmware *fw){
    for (int i = 0; i < fw->nelect; i++) { int k = fw->elect[i];
        if (k == FW_E_WALKS) { if (fabs((double)x->walk - (double)y->walk) > fw->sure * sqrt((double)x->walk + (double)y->walk)) return 0; continue; }
        if (k == FW_E_CONFIDENCE) { if (fabs(x->conf - y->conf) > fw->top_within) return 0; continue; }
        if (key_cmp(x, y, k)) return 0; }
    return 1;
}
/* ---- ORIENT: the joint interpretation (Sequence 20.3; INVENTION §7, "Joint interpretation before policy"). A winning
 * interpretation is a jointly compatible subgraph, not the definition with the largest global score. A word's readings
 * are the types its own strands reach: a concept, a frame, a lexical unit, a roleset, a class, a role, whatever list of
 * the highway holds it. The lists that are banks (manifest/banks.tsv: a part of speech, a dependency relation, a
 * lexicographer file, a thematic role) are features every word shares, not readings: glue is known by its type. Two readings of different words hold together when they are the same type, or when the Linguistic Super
 * Highway maps the one to the other (ILI to frame, roleset, class; roleset to frame and class; lexical unit to frame).
 * Each word takes the reading the others' choices hold together with most, round after round until no choice
 * changes: a reading in turn changes what makes sense for the other words. A word whose best readings stand level is
 * ambiguous; one whose readings nothing else holds together with binds nothing. All of it is the perf-cache: no read. */
typedef struct { int cell, occ, list; uint32_t slot; } Cand;
typedef struct { int ncand, choice, level, capped; double score; } Bind;      /* per occurrence */
static int reading_of(const lp_highway *h, const lp_id *id, int *list, uint32_t *slot){
    for (size_t l = 0; l < h->nlists; l++) { int bank = 0; for (size_t k = 0; k < h->nbanks; k++) bank |= h->bank[k].list == &h->list[l];
        if (bank) continue;                                                  /* a bank's value (a part of speech, a relation): a feature, not a reading */
        int64_t s = lp_highway_slot(h, &h->list[l], id); if (s >= 0) { *list = (int)l; *slot = (uint32_t)s; return 1; } }
    return 0;
}
static int mapped(const lp_highway *h, const Cand *a, const Cand *b){        /* the highway maps a to b */
    const lp_edge *e; size_t n = lp_highway_edges(h, h->list[a->list].name, a->slot, h->list[b->list].name, &e);
    for (size_t k = 0; k < n; k++) if (e[k].to == b->slot) return 1;
    return 0;
}
static int holds_with(const lp_highway *h, const Cand *a, const Cand *b){
    return (a->list == b->list && a->slot == b->slot) || mapped(h, a, b) || mapped(h, b, a);
}
static const Field *CAND_FD;
static int cand_by_id(const void *a, const void *b){ return memcmp(&CAND_FD->c[((const Cand *)a)->cell].id, &CAND_FD->c[((const Cand *)b)->cell].id, 16); }
/* How the positions in [0, upto), other than i, reach a reading: their walkers' visits, weighed by how hard each pulls,
 * and how many of their strands reach it. */
static void reach_of(const State *st, const Cell *x, int i, int upto, double *walks, int *strands){
    *walks = 0; *strands = 0;
    for (int j = 0; j < upto; j++) { if (j == i || !st->composed[j] || st->role[j] <= 0) continue;
        *walks += st->role[j] * visits(st, &x->id, j); *strands += (int)((x->support.w[j >> 6] >> (j & 63)) & 1); }
}
/* Every position's readings, once: the types its own strands reach, in content order. */
typedef struct { const lp_highway *h; Cand *cand; int nc, *first; } Readings;
static void readings_build(State *st, Field *fd, Bind *bind, Readings *R){
    R->h = lp_highway_map(NULL); R->cand = NULL; R->nc = 0; R->first = calloc((size_t)st->nocc + 1, sizeof(int)); int cc = 0;
    for (int i = 0; i < st->nocc; i++) { memset(&bind[i], 0, sizeof bind[i]); bind[i].choice = -1; }
    if (!R->h) return;
    for (int i = 0; i < st->nocc; i++) { R->first[i] = R->nc;
        if (!st->composed[i] || st->role[i] <= 0) continue;
        for (int z = 0; z < fd->n; z++) { const Cell *x = &fd->c[z]; int list; uint32_t slot;
            if (!x->routes[R_CLAIM] || !((x->support.w[i >> 6] >> (i & 63)) & 1) || !reading_of(R->h, &x->id, &list, &slot)) continue;
            if (lp_highway_bank_of(R->h, &x->rel, NULL)) continue;                   /* reached through a feature (a dependency, a part of speech): syntax, not a reading */
            { size_t c_ = (size_t)cc; lp_reserve((void **)&R->cand, &c_, (size_t)R->nc + 1, sizeof(Cand)); cc = (int)c_; } R->cand[R->nc++] = (Cand){ z, i, list, slot }; }
        bind[i].ncand = R->nc - R->first[i];
        CAND_FD = fd; if (bind[i].ncand > 1) qsort(R->cand + R->first[i], (size_t)bind[i].ncand, sizeof(Cand), cand_by_id); }   /* in content order, not as the rows came */
    R->first[st->nocc] = R->nc;
}
/* The reading position i takes against the positions in [0, upto) other than i: how many of them hold together with it
 * (a position's chosen reading, or any of its readings while it has none chosen), weighed by how hard each pulls; two
 * level there told apart by what reaches each from those positions, the walks within their error. -1: none holds.
 * Returns a candidate's index in R. */
static int reading_for(const State *st, const Field *fd, const Bind *bind, const Readings *R, int i, int upto, double *score, int *level){
    double best = 0, bw = 0; int pick = -1, lvl = 0, bs = 0;
    for (int a = R->first[i]; a < R->first[i] + bind[i].ncand; a++) { double s = 0;
        for (int j = 0; j < upto; j++) { if (j == i || !bind[j].ncand) continue; int hold = 0;
            if (bind[j].choice >= 0) hold = holds_with(R->h, &R->cand[a], &R->cand[bind[j].choice]);
            else for (int c = R->first[j]; c < R->first[j] + bind[j].ncand && !hold; c++) hold = holds_with(R->h, &R->cand[a], &R->cand[c]);
            s += hold * st->role[j]; }
        double w; int sn; reach_of(st, &fd->c[R->cand[a].cell], i, upto, &w, &sn);
        if (s > best) { best = s; pick = a; lvl = 0; bw = w; bs = sn; }
        else if (s == best && s > 0) {
            double tol = st->fw->sure * sqrt(w + bw);
            if (w > bw + tol || (!(bw > w + tol) && sn > bs)) { pick = a; lvl = 0; bw = w; bs = sn; }
            else if (!(bw > w + tol) && sn == bs) lvl = 1; } }
    *score = best; *level = lvl; return pick;
}
/* ORIENT, consolidated: from the readings READ chose position by position, every position again against all the
 * others, round after round until no choice changes. The disposition: how many owed positions are bound, how many
 * stay level (ambiguous). bind[].choice becomes the reading's cell. */
static int orient(State *st, Field *fd, Bind *bind, const Readings *R, int *nambig){
    *nambig = 0; if (!R->h) return 0;
    for (int round = 0; round < 8; round++) { int changed = 0;
        for (int i = 0; i < st->nocc; i++) { if (!bind[i].ncand) continue; double sc; int lv;
            int pick = reading_for(st, fd, bind, R, i, st->nocc, &sc, &lv);
            if (pick != bind[i].choice) { bind[i].choice = pick; changed = 1; } bind[i].score = sc; bind[i].level = lv; }
        if (!changed) break; }
    int bound = 0; for (int i = 0; i < st->nocc; i++) { int owes = (st->open.w[i >> 6] >> (i & 63)) & 1;
        if (bind[i].choice >= 0) { bound += owes; if (bind[i].level && owes) (*nambig)++; bind[i].choice = R->cand[bind[i].choice].cell; } }
    return bound;
}
int cmd_turn(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL, *user = getenv("USER"), *session = NULL; int read_only = 0; long long seedv = -1;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "--firmware", 's', &fwp }, { "--as", 's', &user }, { "--session", 's', &session },
                                            { "--seed", 'l', &seedv }, { "--read", 'b', &read_only }, { NULL } });      /* --read: a read, nothing is witnessed */
    if (a >= argc) { fprintf(stderr, "usage: laplace turn [-d conninfo] [--firmware FILE] [--as USER] [--session NAME] [--seed N] [--read] prompt\n"); return 2; }
    if (!user || !*user) user = "user"; if (!session) session = "session";
    double T = now(); if (getenv("LAPLACE_TIMES")) setvbuf(stdout, NULL, _IONBF, 0);      /* each line as it is reached, to be timed */
    Firmware fw = firmware_for(fwp, FW_PULL); ELECT = &fw;
    tier0_open(NULL); table_init(); ctx_open(1); lp_text *c = lp_text_new(T0);
    State *st = calloc(1, sizeof(State)); st->fw = &fw; st->c = c;
    st->pg = db_read(conninfo); st->rd = reader_new(st->pg);
    firmware_ids(&fw);
    firmware_say(&fw, FW_PULL);

    /* ---- RESOLVE: the prompt as content; the session from the record */
    const char *prompt = argv[a];
    Ref pr = text_ref(CTX[0], (const uint8_t *)prompt, strlen(prompt)); st->prompt = pr.id;
    st->nocc = pr.tier <= 2 ? 1 : constituents(&pr.id, st->occ, MAXOCC); if (pr.tier <= 2) st->occ[0] = pr.id;
    Ref who = text_ref(CTX[0], (const uint8_t *)user, strlen(user)), sname = text_ref(CTX[0], (const uint8_t *)session, strlen(session));
    Ref hp[2] = { who, sname }; hp[0].said = hp[1].said = 0; Ref handle = said_tuple(compose(hp, 2, (uint8_t)((who.tier > sname.tier ? who.tier : sname.tier) + 1)));    /* the session: its name within its user's */
    int nturn = 0, ordinal = 1;
    { int have[3] = { 2, 0, 0 }, n, cap; lp_id part[3] = { handle.id, handle.id, handle.id }; Claim *tc = claims_like(st->pg, part, have, fw.fan, fw.k, &n, &cap); st->trips++;
      positions_of(st->pg, tc, n); lp_sort(tc, (size_t)n, sizeof(Claim), claim_by_position);
      for (int i = 0; i < n; i++) { if (tc[i].np != 2 || memcmp(&tc[i].part[0], &handle.id, 16)) continue; nturn++;
          if (tc[i].position >= ordinal) ordinal = tc[i].position + 1;
          st->ndisc += (int)reader_parts(st->rd, &tc[i].part[1], st->disc + st->ndisc, (size_t)(MAXOCC - st->ndisc)); st->trips++; }   /* the turn: [prompt, response]; its constituents are the discourse */
      free(tc); }
    if (!read_only) {                                                         /* the prompt, admitted as content (Forward 20.1, Sessions 21.3): what follows reads a substrate that holds it */
        const lp_trust_class *pc = lp_trust_class_named("UserPromptContent"); File pf; memset(&pf, 0, sizeof pf);
        pf.path = "the prompt"; pf.witness = who; pf.trust = pc ? pc->prior : 0.3; pf.trunk = pr;
        LoadStats ls = { 0 }; if (load(conninfo, 2, &pf, 1, &ls)) return 1; st->admitted = ls.ent_rows;
        TABLE_EACH(x) x->keep = 0;                                            /* recorded now: the turn's witnessing finds it, and writes it again nowhere */
    }
    /* the seed: BLAKE3 of the observation and the firmware's text (and, when the firmware says so, the session and the
     * turn), or of --seed: the same prompt under the same firmware draws the same, and the receipt can say from what */
    { blake3_hasher h; blake3_hasher_init(&h);
      if (seedv >= 0) { int64_t sv = seedv; blake3_hasher_update(&h, &sv, sizeof sv); }
      else { blake3_hasher_update(&h, &pr.id, 16); FILE *ff = fopen(fw.path, "rb"); if (ff) { char b[4096]; size_t k; while ((k = fread(b, 1, sizeof b, ff)) > 0) blake3_hasher_update(&h, b, k); fclose(ff); }
             if (fw.seed_session) { blake3_hasher_update(&h, &handle.id, 16); int32_t od = ordinal; blake3_hasher_update(&h, &od, sizeof od); } }
      blake3_hasher_finalize(&h, st->seed, 32); }
    resolve_roles(st); ROLE = st->role;
    memcpy(st->traj, st->occ, sizeof(lp_id) * (size_t)st->nocc); st->ntraj = st->nocc;
    for (int i = 0; i < st->nocc; i++) reader_want(st->rd, &st->occ[i]);
    char idt[33]; lp_id_hex(&pr.id, idt);
    printf("RESOLVE    prompt %s, tier %d, %d occurrences; session \"%s\" of %s, turn %d (%d before it, %d discourse entities)\n", idt, pr.tier, st->nocc, session, user, ordinal, nturn, st->ndisc);
    if (!read_only) printf("           admitted as content, the user witnessing it: %llu entities new\n", (unsigned long long)st->admitted);
    printf("           obligations:"); for (int i = 0; i < st->nocc; i++) if (st->composed[i]) { char *tx = reader_text(st->rd, &st->occ[i], 32); printf(" %s%s(%.2f)", tx, (st->open.w[i >> 6] >> (i & 63)) & 1 ? "" : "~", st->role[i]); free(tx); } printf("\n");

    /* ---- the loop: each emitted constituent changes the state the next is chosen from */
    lp_id emitted[512]; int nemit = 0; char *out = calloc(1, 1); size_t outn = 0; const char *disposition = "nothing responds";
    Field fd = { 0 }; Curve near[8]; int nnear = 0;
    { /* COUPLE, once for the whole observation: the strands of every occurrence, of the prompt itself, and of the discourse */
      double t = now(); int oo[MAXOCC], none = -1; for (int i = 0; i < st->nocc; i++) oo[i] = i;
      nnear = couple(st, &fd, st->occ, st->nocc, oo, R_CLAIM, near, 8); couple(st, &fd, &st->prompt, 1, &none, R_CLAIM, NULL, 0); couple(st, &fd, st->disc, st->ndisc, NULL, R_DISCOURSE, NULL, 0);
      for (int i = 0; i < st->nocc; i++) { Cell *x = cell_find(&fd, &st->occ[i]); if (x) x->force = 0; }      /* the prompt's own words are not what it is about */
      int routes[R_KINDS] = { 0 }; for (int z = 0; z < fd.n; z++) for (int k = 0; k < R_KINDS; k++) routes[k] += fd.c[z].routes[k];
      printf("COUPLE     %d entities respond:", fd.n); for (int k = 0; k < R_KINDS; k++) if (routes[k]) printf(" %d by %s", routes[k], RK[k]); printf("   (%.1f ms)\n", (now() - t) * 1000);
      st->npos = st->nocc; t = now(); walk(st, 0, st->nocc); char sd[17]; for (int k = 0; k < 8; k++) snprintf(sd + 2 * k, 3, "%02x", st->seed[k]);
      printf("           walks: %d from each word, %d steps, seed %s: %llu steps taken, %llu home, %zu entities stood on, %d read   (%.1f ms)\n", fw.walks, fw.steps, sd,
             (unsigned long long)st->walk_steps, (unsigned long long)st->walk_homes, st->walk_at ? lp_idmap_count(st->walk_at) : (size_t)0, st->walk_read, (now() - t) * 1000);
      t = now(); context(st, 0, st->nocc); int nctx = 0, nhub = 0; for (int i = 0; i < st->nocc; i++) { nctx += st->ctx_n[i]; nhub += st->ctx_hub[i]; }
      printf("           context: %d observations hold the words (%d distinct), %d word%s held by more than the fan   (%.1f ms)\n", nctx, st->ctx_total, nhub, nhub == 1 ? "" : "s", (now() - t) * 1000); }
    /* READ, position by position, in the prompt's order: what the fetches above hold is read the way the prompt is,
     * each position's state depending on the positions before it. For each position: whether what came before
     * predicted it (what follows the longest observed run ending just before it), its reading against the readings
     * chosen before it, and the earlier positions it makes read otherwise (reanalysis). */
    Bind *bind = calloc((size_t)st->nocc + 1, sizeof(Bind)); Readings R; readings_build(st, &fd, bind, &R);
    { double t = now(); Args ra = { 0 }; arg_ids(&ra, st->occ, (size_t)st->nocc); arg_int(&ra, fw.fan);
      PGresult *fq = ask_st(st, "SELECT i, j, paths, next, times FROM laplace_forward($1::blake3[], $2::bigint) ORDER BY j, i, times DESC, next", &ra);
      int expected = 0, surprised = 0, revised = 0;
      for (int p = 0; p < st->nocc; p++) {
          char *w = reader_text(st->rd, &st->occ[p], 24); printf("READ %-3d   %-12s", p + 1, w); free(w);
          /* expectation: the longest run ending at p - 1 that something follows, and whether it is followed by this */
          if (p > 0) { int seg = -1; long times = -1, total = 0;
              for (int r = 0; r < PQntuples(fq); r++) { int i_ = (int)col_int(fq, r, 0), j_ = (int)col_int(fq, r, 1); if (j_ != p || PQgetisnull(fq, r, 3) || col_int(fq, r, 2) > fw.fan) continue;
                  if (seg < 0 || i_ < seg) { seg = i_; times = -1; total = 0; } if (i_ != seg) continue;
                  long tm = (long)col_int(fq, r, 4); total += tm; if (lp_id_eq(col_id(fq, r, 3), &st->occ[p])) times = tm; }
              if (seg < 0) printf(" nothing observed before it predicts what follows");
              else if (times > 0) { expected++; printf(" expected after %d before it: %ld of %ld", p - seg + 1, times, total); }
              else { surprised++; printf(" not what %d before it is observed followed by (%ld continuations)", p - seg + 1, total); } }
          /* its reading, against the readings chosen before it; then the earlier positions it changes */
          if (bind[p].ncand) { double sc; int lv; int pick = reading_for(st, &fd, bind, &R, p, p, &sc, &lv); bind[p].choice = pick; bind[p].level = lv;
              if (pick >= 0) { reader_want(st->rd, &fd.c[R.cand[pick].cell].id); char *rt = reader_text(st->rd, &fd.c[R.cand[pick].cell].id, 40); printf("; reads %s%s (held with %.2f before it)", lv ? "level, e.g. " : "", rt, sc); free(rt); }
              else printf("; %d reading%s, none held with what came before", bind[p].ncand, bind[p].ncand == 1 ? "" : "s"); }
          printf("\n");
          pass_add(st, &st->occ[p]); if (bind[p].choice >= 0) pass_add(st, &fd.c[R.cand[bind[p].choice].cell].id);    /* the pass's trajectory: what it read, and how it read it */
          for (int j = 0; j < p; j++) { if (!bind[j].ncand) continue; double sc; int lv; int np_ = reading_for(st, &fd, bind, &R, j, p + 1, &sc, &lv);
              if (np_ >= 0 && np_ != bind[j].choice) { revised++; reader_want(st->rd, &fd.c[R.cand[np_].cell].id); char *wj = reader_text(st->rd, &st->occ[j], 24), *rt = reader_text(st->rd, &fd.c[R.cand[np_].cell].id, 40);
                  printf("           and %s now reads %s\n", wj, rt); free(wj); free(rt); bind[j].choice = np_; bind[j].level = lv; } } }
      printf("           read: %d positions expected, %d not, %d readings revised   (%.1f ms)\n", expected, surprised, revised, (now() - t) * 1000);
      PQclear(fq); args_free(&ra); }
    /* ORIENT, the frame: the observed curves nearest the prompt's under the firmware's shape measure. The prompt's words
     * the nearest holds are the frame the question is asked in; the words it does not hold, where the curves part, are
     * the slots, what the question is about. While the frame holds part of the prompt and leaves a slot, the slots are
     * what is owed. */
    {
      static const char *SH[] = { "frechet", "outliers", "dtw", "edr" };
      printf("ORIENT     the shape: the %d observed curves nearest the prompt's, nominated by the GIN and the GiST, measured by %s natively\n", nnear, SH[fw.shape]);
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
    int nambig = 0, nbound, ncentre = 0, capped = 0; lp_id centre[MAXOCC];
    { double t = now(); nbound = orient(st, &fd, bind, &R, &nambig); free(R.cand); free(R.first);
      for (int i = 0; i < st->nocc; i++) { capped |= bind[i].capped; if (bind[i].choice >= 0 && !bind[i].level && ((st->open.w[i >> 6] >> (i & 63)) & 1)) centre[ncentre++] = fd.c[bind[i].choice].id;    /* the program routes from what is owed (Sequence 20.4) */ }
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
          chains[nchains] = (Prop){ ans, P_CHAIN, (int)(st->role[byrole[x]] * 1000 + 0.5), 0, lp_confidence(&rt, fw.k), 0, 0, byrole[x] }; chains[nchains++].shape = INFINITY;
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
        const lp_highway *hw = lp_highway_map(NULL);
        for (int i = 0; i < nn; i++) { if (hw && lp_highway_bank_of(hw, &nx[i].id, NULL)) continue;   /* a bank's value (a part of speech, a relation) is a feature, never what is said */
            Prop p; memset(&p, 0, sizeof p); p.id = nx[i].id; p.kind = P_FOLLOW; p.cont = nx[i].len; p.times = nx[i].times; p.occ = -1; p.shape = INFINITY;
            { const Cell *x = cell_find(&fd, &nx[i].id); if (x) { p.conf = x->has_r ? lp_confidence(&x->r, fw.k) : 0; p.hub = x->hub; } }
            weigh_prop(st, &fd, &p); pp[np++] = p; }
        for (int q = 0; q < nchains; q++) { int o = chains[q].occ; if (!((st->open.w[o >> 6] >> (o & 63)) & 1)) continue; Prop p = chains[q]; weigh_prop(st, &fd, &p); pp[np++] = p; }     /* a chain's answer, while its word is owed */
        /* the pass's own shape: the observed curves nearest its recent window, and what each does next after the last
         * place it meets the window; a constituent proposed this way that a run also proposes takes the nearer shape */
        { int wn = st->npass < 8 ? st->npass : 8; const lp_id *win = st->pass + st->npass - wn; Curve an[8]; int na = shape_near(st, win, wn, an, 8);
          for (int c = 0; c < na; c++) { int meet = -1;
              for (int k = an[c].nv - 1; k >= 0 && meet < 0; k--) for (int z = 0; z < wn; z++) if (lp_id_eq(&an[c].v[k], &win[z])) { meet = k; break; }
              int nxt = -1; for (int k = meet + 1; meet >= 0 && k < an[c].nv; k++) if (lp_tier0_codepoint(T0, &an[c].v[k]) < 0) { nxt = k; break; }
              if (c < 2) { reader_want(st->rd, &an[c].id); char *tc = reader_text(st->rd, &an[c].id, 70); printf("           like the pass: %.4f \"%s\"%s\n", an[c].d, tc, nxt < 0 ? ", nothing after where it meets the pass" : ""); free(tc); }
              if (nxt < 0) continue; const lp_id *id = &an[c].v[nxt]; int have = -1; for (int z = 0; z < np; z++) if (lp_id_eq(&pp[z].id, id)) have = z;
              if (have >= 0) { if (an[c].d < pp[have].shape) pp[have].shape = an[c].d; continue; }
              if (np >= 80 || (hw && lp_highway_bank_of(hw, id, NULL))) continue;
              Prop sp; memset(&sp, 0, sizeof sp); sp.id = *id; sp.kind = P_SHAPE; sp.occ = -1; sp.shape = an[c].d;
              { const Cell *x = cell_find(&fd, id); if (x) { sp.conf = x->has_r ? lp_confidence(&x->r, fw.k) : 0; sp.hub = x->hub; } }
              weigh_prop(st, &fd, &sp); pp[np++] = sp; }
          for (int c = 0; c < na; c++) free(an[c].v); }
        if (!np) { free(pp); if (!nemit) disposition = ncentre ? "unresolved: nothing follows and nothing grounds what is open" : disposition; break; }
        /* STEER, SELECT */
        qsort(pp, (size_t)np, sizeof(Prop), elect);
        if (!pp[0].grounds && bit_count_and(&st->open, &st->open)) {            /* while obligations are open, what grounds none of them is not emitted (Sequence 20.8) */
            free(pp); if (!nemit) disposition = "unresolved: what follows grounds nothing that is owed"; break; }
        /* the evidence for the head of the election, as the trace's attention: each channel, the words it grounds */
        for (int z = 0; z < np && z < 3; z++) { reader_want(st->rd, &pp[z].id); char *tz = reader_text(st->rd, &pp[z].id, 40);
            printf("           %s \"%s\": grounds %.2f by %d channel%s (strand %d, walks %d, beside %d words), continuity %d, beside %ld, walks %ld, shape %.4f, confidence %.3f\n", z ? "  then" : "elect", tz,
                   pp[z].grounds / 1000.0, pp[z].agree, pp[z].agree == 1 ? "" : "s", bit_count_and(&pp[z].by[CH_STRAND], &pp[z].by[CH_STRAND]), bit_count_and(&pp[z].by[CH_WALK], &pp[z].by[CH_WALK]),
                   bit_count_and(&pp[z].by[CH_BESIDE], &pp[z].by[CH_BESIDE]), pp[z].cont, pp[z].cooc, pp[z].walk, isfinite(pp[z].shape) ? pp[z].shape : -1.0, pp[z].conf); free(tz); }
        int tied[80], nt = 0; for (int z = 0; z < np && nt < 80; z++) if (!z || cannot_tell(&pp[0], &pp[z], &fw)) tied[nt++] = z;
        int pick = 0;
        if (nt > 1) {                                                         /* nothing in the evidence tells these apart: the firmware's tie */
            printf("           a tie: %d proposals the evidence cannot tell apart; the firmware %s\n", nt, fw.tie == FW_TIE_DRAW ? "draws" : fw.tie == FW_TIE_ASK ? "asks" : "takes the first by ID");
            if (fw.tie == FW_TIE_DRAW) pick = tied[(int)(draw(st, 0x5e1ec7u, (uint32_t)step, 0) * nt)];
            else if (fw.tie == FW_TIE_ASK) { free(pp); if (!nemit) disposition = "ambiguous: proposals the evidence cannot tell apart, and the firmware asks"; break; } }
        Prop sel = pp[pick]; free(pp);
        /* REALIZE */
        reader_want(st->rd, &sel.id); char *tx = reader_text(st->rd, &sel.id, 400);
        size_t tl = strlen(tx); int sep = sel.kind != P_FOLLOW && outn && out[outn - 1] != ' ' && tl && tx[0] != ' ';
        out = xrealloc(out, outn + tl + 2); if (sep) out[outn++] = ' '; memcpy(out + outn, tx, tl); outn += tl; out[outn] = 0;
        printf("STEP %-3d   %s \"%s\"   grounds %.2f, continuity %d, confidence %.3f\n", step + 1, sel.kind == P_FOLLOW ? "follows" : sel.kind == P_SHAPE ? "like   " : "answers", tx, sel.grounds / 1000.0, sel.cont, sel.conf);
        free(tx);
        /* WITNESS, within the pass: the constituent joins the trajectory; what it grounds closes; the next coupling sees it */
        if (nemit < 512) emitted[nemit++] = sel.id; if (st->ntraj < 4096) st->traj[st->ntraj++] = sel.id; pass_add(st, &sel.id);
        { const Cell *x = cell_find(&fd, &sel.id); if (x) bit_clear(&st->open, &x->support); }
        if (sel.kind == P_CHAIN && sel.occ >= 0) st->open.w[sel.occ >> 6] &= ~(1ull << (sel.occ & 63));        /* the word the chain answers is owed no longer */
        couple(st, &fd, &sel.id, 1, NULL, R_DISCOURSE, NULL, 0);
        if (st->npos < MAXOCC && lp_tier0_codepoint(T0, &sel.id) < 0) {        /* a composition emitted: a position the next step attends to, its walkers and its context */
            int q = st->npos++; st->occ[q] = sel.id; st->composed[q] = 1; st->role[q] = 1.0; double tw = now(); uint64_t before = st->walk_steps;
            walk(st, q, q + 1); context(st, q, q + 1);
            printf("           it walks: %llu steps, %d observations beside it   (%.1f ms)\n", (unsigned long long)(st->walk_steps - before), st->ctx_n[q], (now() - tw) * 1000); }
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
        char ti[33]; lp_id_hex(&turn.id, ti);
        printf("WITNESS    turn %s, at %d in the session: %llu entities new, the user's [session, turn] and Laplace's [response, prompt]\n", ti, ordinal, (unsigned long long)ls.ent_rows);
        free(f[0].ev.e); free(f[1].ev.e);
    } else printf("WITNESS    %s\n", read_only ? "a read: nothing is witnessed" : "nothing emitted: nothing to witness");
    printf("\n%llu round trips, %llu for text   total %.1f ms\n", (unsigned long long)st->trips, (unsigned long long)reader_trips(st->rd), (now() - T) * 1000);
    field_free(&fd); free(bind); free(out); reader_free(st->rd); PQfinish(st->pg); free(st);
    return 0;
}
