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
typedef struct { Cell *c; int n, cap; lp_idmap *m; } Field;                  /* the cells, in the order they responded; found by ID */
static Cell *cell(Field *f, const lp_id *id){
    if (!f->m) f->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(f->m, id, &fresh);
    if (!fresh) return &f->c[i];
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 1024; f->c = xrealloc(f->c, sizeof(Cell) * (size_t)f->cap); }
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
    const Firmware *fw; PGconn *pg; Reader *rd; lp_text *c; unsigned seed;
    lp_id prompt; lp_id occ[MAXOCC]; int nocc; double role[MAXOCC]; int composed[MAXOCC];        /* the occurrences, how hard each pulls, which are compositions */
    lp_id disc[MAXOCC]; int ndisc;                                                                 /* the discourse: what the session's earlier turns hold */
    lp_id traj[4096]; int ntraj;                                                                   /* the active trajectory: the occurrences, then what has been emitted */
    Bits open;                                                                                     /* the obligations still open */
    uint64_t trips;
    uint64_t admitted;                                                        /* the prompt's entities its admission recorded new */
} State;

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
static int couple(State *st, Field *fd, const lp_id *ids, int n, const int *occ_of, int kind, Curve *near, int keep){
    if (!n) return 0; const Firmware *fw = st->fw;
    Args a = { 0 }; arg_ids(&a, ids, (size_t)n); arg_int(&a, fw->fan); arg_refused(&a); arg_int(&a, keep > 0 ? fw->shape : -1); arg_f64(&a, fw->shape_n); arg_int(&a, keep > 0 ? keep + 1 : keep);
    PGresult *q = ask_st(st, "SELECT entity, occ, route, rating, deviation, volatility, via, rel, tier, distance, vertices FROM laplace_couple($1::blake3[], $2::bigint, $3::blake3[], $4::smallint, $5::float8, $6::integer)", &a);
    int *held = calloc((size_t)n + 1, sizeof(int)), nn = 0;
    for (int r = 0; r < PQntuples(q); r++) if (col_int(q, r, 2) == 0) { int o = (int)col_int(q, r, 1); if (o >= 1 && o <= n) held[o - 1]++; }
    for (int r = 0; r < PQntuples(q); r++) {
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
    PQclear(q); args_free(&a); free(held); return nn;
}
/* What follows the end of the active trajectory, as a run, in what was observed: the longest observed suffix first. */
typedef struct { lp_id id; long times; int len; } Next;
static int follows(State *st, Next *out, int cap){
    int from = st->ntraj > 24 ? st->ntraj - 24 : 0, n = st->ntraj - from; if (n <= 0) return 0;
    Args a = { 0 }; arg_ids(&a, st->traj + from, (size_t)n); arg_int(&a, st->fw->fan);
    PGresult *q = ask_st(st, "SELECT i, j, next, times FROM laplace_forward($1::blake3[], $2::bigint) WHERE j = array_length($1::blake3[], 1) AND next IS NOT NULL ORDER BY (j - i) DESC, times DESC", &a);
    int m = 0, best = -1;
    for (int r = 0; r < PQntuples(q) && m < cap; r++) { int len = (int)(col_int(q, r, 1) - col_int(q, r, 0) + 1);
        if (best < 0) best = len; if (len < best) break;                                           /* the longest run observed decides; shorter ones are not consulted once it answers */
        out[m].id = *col_id(q, r, 2); out[m].times = (long)col_int(q, r, 3); out[m].len = len; m++; }
    PQclear(q); args_free(&a); return m;
}

/* A chain the firmware names, followed from a word (chain_follow), the oriented reading taken where the chain passes
 * through it; *rating: the standing of its last strand. */
static int chain_from(State *st, const lp_id *word, const lp_id *reading, lp_id *answer, lp_rating *rating){
    Claim last; int alt; chain_follow(st->pg, st->rd, (Firmware *)st->fw, word, reading, NULL, answer, &last, &alt);
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
        args_reset(&a); arg_ids(&a, ids, (size_t)m); arg_int(&a, fw->fan + 1); arg_text(&a, CLAIM_BITS); arg_refused(&a);
        PGresult *q = ask_st(st, "SELECT i, entity, path, rating, deviation, volatility FROM laplace_claims_each($1::blake3[], $2::bigint, $3::smallint[], $4::blake3[])", &a);
        int held[64] = { 0 }; for (int r = 0; r < PQntuples(q); r++) { int e = (int)col_int(q, r, 0) - 1; if (e >= 0 && e < m) held[e]++; }
        for (int r = 0; r < PQntuples(q); r++) { int e = (int)col_int(q, r, 0) - 1; if (e < 0 || e >= m || held[e] > fw->fan) continue;
            const lp_reached *at = &batch[who[e]]; path_into(&rn, col_path(q, r, 2));
            if (lp_tuple_middle_any(rn.v, rn.n, fw->id.refuse, (size_t)fw->nrefuse_predicate)) continue;
            int other = lp_tuple_other(rn.v, rn.n, &at->id); if (other < 0) continue;
            lp_rating rt = col_rating(q, r, 3);
            double sw = strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n); if (sw <= 0) continue;
            double cost = at->cost + lp_cost(&rt, fw->k, fw->lambda) - log(sw);
            lp_frontier_reach(fr, &rn.v[other], &at->id, col_id(q, r, 1), cost, 0, at->hops + 1);
            Cell *cx = cell(fd, &rn.v[other]); cx->routes[R_SCAN]++; if (cost < cx->cost) { cx->cost = cost; if (!cx->has_r) { cx->r = rt; cx->has_r = 1; cx->via = at->id; cx->rel = rn.n >= 3 ? rn.v[1] : at->id; } }
        }
        PQclear(q);
    }
    lp_frontier_free(fr); lp_vec_free(&rn); args_free(&a);
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
 * interpretation is a jointly compatible subgraph, not the definition with the largest global score. A word's readings
 * are the types its own strands reach: a concept, a frame, a lexical unit, a roleset, a class, a role, whatever list of
 * the highway holds it. The lists small enough to be mask fields (a part of speech, a dependency relation, a
 * lexicographer file, a thematic role, a row's kind) are features every word shares, not readings: glue is known by its
 * type. Two readings of different words hold together when they are the same type, or when the Linguistic Super
 * Highway maps the one to the other (ILI to frame, roleset, class; roleset to frame and class; lexical unit to frame).
 * Each word takes the reading the others' choices hold together with most, round after round until no choice
 * changes: a reading in turn changes what makes sense for the other words. A word whose best readings stand level is
 * ambiguous; one whose readings nothing else holds together with binds nothing. All of it is the perf-cache: no read. */
typedef struct { int cell, occ, list; uint32_t slot; } Cand;
typedef struct { int ncand, choice, level, capped; double score; } Bind;      /* per occurrence */
static int reading_of(const lp_highway *h, const lp_id *id, int *list, uint32_t *slot){
    for (size_t l = 0; l < h->nlists; l++) { if (lp_highway_mask(h, h->list[l].name)) continue;        /* a feature, not a reading */
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
static int orient(State *st, Field *fd, Bind *bind, int *nambig){
    const lp_highway *h = lp_highway_map(NULL); *nambig = 0;
    for (int i = 0; i < st->nocc; i++) { memset(&bind[i], 0, sizeof bind[i]); bind[i].choice = -1; }
    if (!h) return 0;
    Cand *cand = NULL; int nc = 0, cc = 0, *first = calloc((size_t)st->nocc + 1, sizeof(int));
    for (int i = 0; i < st->nocc; i++) { first[i] = nc;
        if (!st->composed[i] || st->role[i] <= 0) continue;
        for (int z = 0; z < fd->n; z++) { const Cell *x = &fd->c[z]; int list; uint32_t slot;
            if (!x->routes[R_CLAIM] || !((x->support.w[i >> 6] >> (i & 63)) & 1) || !reading_of(h, &x->id, &list, &slot)) continue;
            if (lp_highway_mask_bit(h, &x->rel) >= 0) continue;                       /* reached through a feature (a dependency, a part of speech): syntax, not a reading */
            if (nc == cc) { cc = cc ? cc * 2 : 256; cand = xrealloc(cand, sizeof(Cand) * (size_t)cc); } cand[nc++] = (Cand){ z, i, list, slot }; }
        bind[i].ncand = nc - first[i]; }
    first[st->nocc] = nc;
    for (int round = 0; round < 8; round++) { int changed = 0;
        for (int i = 0; i < st->nocc; i++) { if (!bind[i].ncand) continue; double best = 0; int pick = -1, level = 0;
            for (int a = first[i]; a < first[i] + bind[i].ncand; a++) { double s = 0;
                for (int j = 0; j < st->nocc; j++) { if (j == i || !bind[j].ncand) continue; int hold = 0;
                    if (round && bind[j].choice >= 0) hold = holds_with(h, &cand[a], &cand[bind[j].choice]);
                    else if (!round) for (int b = first[j]; b < first[j] + bind[j].ncand && !hold; b++) hold = holds_with(h, &cand[a], &cand[b]);
                    s += hold * st->role[j]; }
                if (s > best) { best = s; pick = a; level = 0; } else if (s == best && s > 0) level = 1; }
            if (pick != bind[i].choice) { bind[i].choice = pick; changed = 1; } bind[i].score = best; bind[i].level = level; }
        if (round && !changed) break; }
    int bound = 0; for (int i = 0; i < st->nocc; i++) { int owes = (st->open.w[i >> 6] >> (i & 63)) & 1;
        if (bind[i].choice >= 0) { bound += owes; if (bind[i].level && owes) (*nambig)++; bind[i].choice = cand[bind[i].choice].cell; } }
    free(cand); free(first);
    return bound;
}
int cmd_turn(int argc, char **argv){
    const char *conninfo = laplace_db(), *fwp = NULL, *user = getenv("USER"), *session = NULL; int read_only = 0; long long seedv = -1;
    int a = opts(argc, argv, (const Opt[]){ { "-d", 's', &conninfo }, { "--firmware", 's', &fwp }, { "--as", 's', &user }, { "--session", 's', &session },
                                            { "--seed", 'l', &seedv }, { "--read", 'b', &read_only }, { NULL } });      /* --read: a read, nothing is witnessed */
    if (a >= argc) { fprintf(stderr, "usage: laplace turn [-d conninfo] [--firmware FILE] [--as USER] [--session NAME] [--seed N] [--read] prompt\n"); return 2; }
    if (!user || !*user) user = "user"; if (!session) session = "session";
    double T = now(); unsigned seed = seedv >= 0 ? (unsigned)seedv : (unsigned)(T * 1e6);
    Firmware fw = firmware_for(fwp, FW_PULL);
    tier0_open(NULL); table_init(); ctx_open(1); lp_text *c = lp_text_new(T0);
    State *st = calloc(1, sizeof(State)); st->fw = &fw; st->c = c; st->seed = seed;
    st->pg = db_connect(conninfo); st->rd = reader_new(st->pg);
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
      printf("COUPLE     %d entities respond:", fd.n); for (int k = 0; k < R_KINDS; k++) if (routes[k]) printf(" %d by %s", routes[k], RK[k]); printf("   (%.1f ms)\n", (now() - t) * 1000); }
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
    Bind *bind = calloc((size_t)st->nocc + 1, sizeof(Bind)); int nambig = 0, nbound, ncentre = 0, capped = 0; lp_id centre[MAXOCC];
    { double t = now(); nbound = orient(st, &fd, bind, &nambig);
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
            { const Cell *x = cell_find(&fd, &nx[i].id); if (x) { p.grounds = owed(&x->support, &st->open); p.conf = x->has_r ? lp_confidence(&x->r, fw.k) : 0; p.hub = x->hub; } }
            pp[np++] = p; }
        for (int q = 0; q < nchains; q++) { int o = chains[q].occ; if (!((st->open.w[o >> 6] >> (o & 63)) & 1)) continue; pp[np++] = chains[q]; }     /* a chain's answer, while its word is owed */
        if (!np) { free(pp); if (!nemit) disposition = ncentre ? "unresolved: nothing follows and nothing grounds what is open" : disposition; break; }
        /* STEER, SELECT */
        qsort(pp, (size_t)np, sizeof(Prop), elect);
        if (!pp[0].grounds && bit_count_and(&st->open, &st->open)) {            /* while obligations are open, what grounds none of them is not emitted (Sequence 20.8) */
            free(pp); if (!nemit) disposition = "unresolved: what follows grounds nothing that is owed"; break; }
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
        { const Cell *x = cell_find(&fd, &sel.id); if (x) bit_clear(&st->open, &x->support); }
        if (sel.kind == P_CHAIN && sel.occ >= 0) st->open.w[sel.occ >> 6] &= ~(1ull << (sel.occ & 63));        /* the word the chain answers is owed no longer */
        couple(st, &fd, &sel.id, 1, NULL, R_DISCOURSE, NULL, 0);
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
