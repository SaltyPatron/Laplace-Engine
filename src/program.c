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
#include "program_internal.h"
static const double *ROLE;
static int owed(const Bits *support, const Bits *open){ double s = 0; for (int w = 0; w < MAXOCC / 64; w++) { uint64_t m = support->w[w] & open->w[w]; while (m) { int b = __builtin_ctzll(m); s += ROLE ? ROLE[w * 64 + b] : 1; m &= m - 1; } } return (int)(s * 1000 + 0.5); }
static const char *RK[R_KINDS] = { "strand", "follows", "discourse", "reached", "containment", "a later layer" };
static int reading_of(const lp_highway *h, const lp_id *id, int *list, uint32_t *slot){
    for (size_t l = 0; l < h->nlists; l++) { int bank = 0; for (size_t k = 0; k < h->nbanks; k++) bank |= h->bank[k].list == &h->list[l];
        if (bank) continue;                                                  /* a bank's value (a part of speech, a relation): a feature, not a reading */
        int64_t s = lp_highway_slot(h, &h->list[l], id); if (s >= 0) { *list = (int)l; *slot = (uint32_t)s; return 1; } }
    return 0;
}
/* ---- SCAN: best-first from the centres, across rated strands and highway crosswalk edges */
static void scan(State *st, Field *fd, const lp_id *centre, int nc){
    const Firmware *fw = st->fw; const lp_highway *hw = lp_highway_map(NULL);
    lp_frontier *fr = lp_frontier_new(); Ids rn = { 0 }; Args a = { 0 };
    for (int i = 0; i < nc; i++) lp_frontier_reach(fr, &centre[i], NULL, NULL, 0, 0, 0);
    int rounds = 0; const lp_reached *x;
    while (rounds++ < fw->hops) {
        lp_reached batch[16]; int n = 0; while (n < 16 && (x = lp_frontier_next(fr))) batch[n++] = *x; if (!n) break;      /* the nearest sixteen a round */
        lp_id ids[64]; int m = 0, who[64]; for (int i = 0; i < n; i++) if ((int)batch[i].hops < fw->hops) { ids[m] = batch[i].id; who[m++] = i; }
        if (!m) break;
        if (hw) {
            for (int e = 0; e < m; e++) {
                const lp_reached *at = &batch[who[e]]; int list; uint32_t slot;
                if (!reading_of(hw, &at->id, &list, &slot)) continue;
                for (size_t b = 0; b < hw->nlists; b++) {
                    if ((int)b == list) continue;
                    int bank = 0; for (size_t k = 0; k < hw->nbanks; k++) bank |= hw->bank[k].list == &hw->list[b];
                    if (bank) continue;
                    const lp_edge *edges; size_t ne = lp_highway_edges(hw, hw->list[list].name, slot, hw->list[b].name, &edges);
                    for (size_t k = 0; k < ne; k++) {
                        const lp_tier0_record *rec = lp_highway_at(hw, &hw->list[b], edges[k].to); if (!rec) continue;
                        double cost = at->cost + fw->lambda;
                        lp_frontier_reach(fr, &rec->id, &at->id, &at->id, cost, 0, at->hops + 1);
                        const Cell *ca = cell_find(fd, &at->id); Bits sup = ca ? ca->support : (Bits){ 0 };
                        int had_r = ca ? ca->has_r : 0; lp_rating cr = ca ? ca->r : (lp_rating){ 1500, 350, 0.06 };
                        Cell *cx = cell(fd, &rec->id); cx->routes[R_SCAN]++;
                        for (int w = 0; w < MAXOCC / 64; w++) cx->support.w[w] |= sup.w[w];
                        if (cost < cx->cost) { cx->cost = cost; if (!cx->has_r && had_r) { cx->r = cr; cx->has_r = 1; cx->via = at->id; cx->rel = at->id; } }
                    }
                }
            }
        }
        int hub[128], nh = 0; Hold *hh = holds_capped(st->pg,ids, m, fw->fan, 1, 1, hub, &nh); st->trips++;      /* one Native frontier, in content order, a hub's never read */
        for (int r = 0; r < nh; r++) { int e = hh[r].src; if (e < 0 || e >= m || hub[2 * e] || !hh[r].claim || !hh[r].stood) continue;
            const lp_reached *at = &batch[who[e]]; path_into(&rn, lp_path_of(hh[r].path, (size_t)hh[r].path_len));
            if (!head_read(fw, rn.v, rn.n)) { st->refused++; continue; }
            int other = lp_tuple_other(rn.v, rn.n, &at->id); if (other < 0) continue;
            lp_rating rt = hh[r].r;
            double sw = strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n); if (sw <= 0) continue;
            double cost = at->cost + lp_cost(&rt, fw->k, fw->lambda) - log(sw);
            lp_frontier_reach(fr, &rn.v[other], &at->id, &hh[r].entity, cost, 0, at->hops + 1);
            const Cell *ca = cell_find(fd, &at->id); Bits sup = ca ? ca->support : (Bits){ 0 };
            Cell *cx = cell(fd, &rn.v[other]); cx->routes[R_SCAN]++;
            for (int w = 0; w < MAXOCC / 64; w++) cx->support.w[w] |= sup.w[w];
            if (cost < cx->cost) { cx->cost = cost; if (!cx->has_r) { cx->r = rt; cx->has_r = 1; cx->via = at->id; cx->rel = rn.n >= 3 ? rn.v[1] : at->id; } }
        }
        holds_free(hh, nh);
    }
    lp_frontier_free(fr); lp_vec_free(&rn); args_free(&a);
}

/* ---- the proposals of a step, and their election */
enum { P_FOLLOW, P_CHAIN, P_SHAPE };
typedef struct { lp_id id; int kind, grounds, cont; double conf, hub; long times; int occ; int agree; long cooc, walk; Bits by[3]; double shape; int heads; } Prop;     /* shape: the distance of the nearest analogous curve that proposes it (INFINITY: none) */     /* occ: the occurrence a chain answers, or -1; by: which obligations each channel grounds */
enum { CH_STRAND, CH_WALK, CH_BESIDE };
/* A proposal against every word still owed, through every channel (the attention of a transformer, read from the
 * records): a strand of the word reaches it (the field's support), the word's walkers stand on it, it is observed beside
 * the word more than its base rate. grounds: what the union owes, weighed by role; agree: how many channels ground
 * anything; cooc, walk: how often, over the words owed. */
static void weigh_prop(const State *st, const Field *fd, Prop *p){
    const Cell *x = cell_find(fd, &p->id); memset(p->by, 0, sizeof p->by); p->cooc = p->walk = 0; p->heads = x ? x->nheads : 0;      /* independent relation heads converging on it */
    int atom = lp_tier0_codepoint(T0, &p->id) >= 0;                          /* a letter, a space, a mark: it may continue a run, it grounds nothing */
    for (int j = 0; j < st->npos; j++) { if (lp_id_eq(&st->occ[j], &p->id)) continue;
        int v = visits(st, &p->id, j), b = beside(st, j, &p->id); p->walk += v; p->cooc += b;      /* how the whole trajectory so far, prompt and emitted, attends to it */
        if (atom || j >= st->nocc || !((st->open.w[j >> 6] >> (j & 63)) & 1)) continue;            /* grounding: only what is still owed, and only by a composition */
        if (x && ((x->support.w[j >> 6] >> (j & 63)) & 1)) bit_set(&p->by[CH_STRAND], j);
        if (v >= 2) bit_set(&p->by[CH_WALK], j);
        if (beside_grounds(st, j, &p->id)) bit_set(&p->by[CH_BESIDE], j); }
    Bits u; memset(&u, 0, sizeof u); p->agree = 0;
    for (int c = 0; c < 3; c++) { int any = 0; for (int w = 0; w < MAXOCC / 64; w++) { u.w[w] |= p->by[c].w[w]; any |= p->by[c].w[w] != 0; } p->agree += any; }
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
    case FW_E_HEADS:      return x->heads != y->heads ? (x->heads > y->heads ? -1 : 1) : 0;
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
    int a = opts(argc, argv, (const Opt[]){ { .flag = "-d", .kind = 's', .at = &conninfo }, { .flag = "--firmware", .kind = 's', .at = &fwp }, { .flag = "--as", .kind = 's', .at = &user }, { .flag = "--session", .kind = 's', .at = &session },
                                            { .flag = "--seed", .kind = 'l', .at = &seedv }, { .flag = "--read", .kind = 'b', .at = &read_only }, { 0 } });      /* --read: a read, nothing is witnessed */
    if (a >= argc) { fprintf(stderr, "usage: laplace turn [-d conninfo] [--firmware FILE] [--as USER] [--session NAME] [--seed N] [--read] prompt\n"); return 2; }
    if (!user || !*user) user = "user"; if (!session) session = "session";
    double T = now(); if (getenv("LAPLACE_TIMES")) setvbuf(stdout, NULL, _IONBF, 0);      /* each line as it is reached, to be timed */
    Firmware fw = firmware_for(fwp, FW_PULL); ELECT = &fw;
    tier0_open(NULL); table_init(); ctx_open(1); lp_text *c = lp_text_new(T0);
    State *st = calloc(1, sizeof(State)); st->fw = &fw; st->c = c;
    firmware_ids(&fw);
    firmware_say(&fw, FW_PULL);

    /* ---- RESOLVE: the prompt as content; the session from the record */
    const char *prompt = argv[a];
    size_t occurrences;
    Ref pr = text_occurrences(CTX[0], (const uint8_t *)prompt, strlen(prompt), st->occ, MAXOCC, &occurrences); st->prompt = pr.id;
    if (occurrences > MAXOCC) { fprintf(stderr, "turn: prompt has %zu occurrences; this execution window supports %d\n", occurrences, MAXOCC); return 2; }
    st->nocc = (int)occurrences;
    st->pg = db_read(conninfo); st->rd = reader_new(st->pg);
    Ref who = text_ref(CTX[0], (const uint8_t *)user, strlen(user)), sname = text_ref(CTX[0], (const uint8_t *)session, strlen(session));
    Ref hp[2] = { who, sname }; hp[0].said = hp[1].said = 0; Ref handle = said_tuple(compose(hp, 2, (uint8_t)((who.tier > sname.tier ? who.tier : sname.tier) + 1)));    /* the session: its name within its user's */
    int nturn = 0, ordinal = 1;
    { int have[3] = { 2, 0, 0 }, n, cap; lp_id part[3] = { handle.id, handle.id, handle.id }; Claim *tc = claims_like(st->pg, part, have, fw.fan, fw.k, &n, &cap); st->trips++;
      positions_of(st->pg, tc, n); lp_sort(tc, (size_t)n, sizeof(Claim), claim_by_position);
      for (int i = 0; i < n; i++) { if (tc[i].np != 2 || memcmp(&tc[i].part[0], &handle.id, 16)) continue; nturn++;
          if (tc[i].position >= ordinal) ordinal = tc[i].position + 1;
          size_t remaining = (size_t)(MAXOCC - st->ndisc), parts = reader_parts(st->rd, &tc[i].part[1], st->disc + st->ndisc, remaining); st->trips++;
          if (parts > remaining) { fprintf(stderr, "turn: session discourse exceeds this execution window of %d constituents\n", MAXOCC); free(tc); return 2; }
          st->ndisc += (int)parts; }
      free(tc); }
    if (!read_only) {                                                         /* the prompt, admitted as content (Forward 20.1, Sessions 21.3): what follows reads a substrate that holds it */
        const lp_trust_class *pc = lp_trust_class_named("UserPromptContent"); File pf; memset(&pf, 0, sizeof pf);
        pf.path = "the prompt"; pf.witness = who; pf.trust = pc ? pc->prior : 0.3; pf.trunk = pr;
        LoadStats ls = { 0 }; if (load(conninfo, 2, &pf, 1, &ls) || merge(conninfo, 2, &ls)) return 1; st->admitted = ls.ent_rows;
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
    { lp_id w[MAXOCC]; int nw = 0; for (int i = 0; i < st->nocc; i++) if (st->composed[i]) { int dup = 0; for (int z = 0; z < nw && !dup; z++) dup = lp_id_eq(&w[z], &st->occ[i]); if (!dup) w[nw++] = st->occ[i]; }   /* each word once: a word said twice is one vote */
      lp_id (*lg)[LANGS] = calloc((size_t)(nw ? nw : 1), sizeof *lg); int *nlg = calloc((size_t)(nw ? nw : 1), sizeof(int)); langs_of(st, w, nw, lg, nlg);
      /* each word gives each language it is lexicalized in a share of one vote: a word in one language says which; a
       * word lexicalized in many (a homograph of every language's "a") says little */
      lp_id cand[64]; double cnt[64]; int nc = 0;
      for (int i = 0; i < nw; i++) for (int z = 0; z < nlg[i] && z < LANGS; z++) { int k = -1; for (int y = 0; y < nc; y++) if (lp_id_eq(&cand[y], &lg[i][z])) k = y; if (k < 0 && nc < 64) { cand[nc] = lg[i][z]; cnt[nc] = 0; k = nc++; } if (k >= 0) cnt[k] += 1.0 / nlg[i]; }
      double top = 0; for (int y = 0; y < nc; y++) if (cnt[y] > top) top = cnt[y];
      for (int y = 0; y < nc && st->nlang < 8; y++) if (cnt[y] == top && top > 0) st->lang[st->nlang++] = cand[y];
      free(lg); free(nlg); }
    memcpy(st->traj, st->occ, sizeof(lp_id) * (size_t)st->nocc); st->ntraj = st->nocc;
    for (int i = 0; i < st->nocc; i++) reader_want(st->rd, &st->occ[i]);
    char idt[33]; lp_id_hex(&pr.id, idt);
    printf("RESOLVE    prompt %s, tier %d, %d occurrences; session \"%s\" of %s, turn %d (%d before it, %d discourse entities)\n", idt, pr.tier, st->nocc, session, user, ordinal, nturn, st->ndisc);
    if (!read_only) printf("           admitted as content, the user witnessing it: %llu entities new\n", (unsigned long long)st->admitted);
    printf("           language:"); for (int y = 0; y < st->nlang; y++) { reader_want(st->rd, &st->lang[y]); char *tl = reader_text(st->rd, &st->lang[y], 16); printf(" %s", tl); free(tl); } if (!st->nlang) printf(" not known"); printf("\n");
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
      if (HEADS) { size_t nhd = lp_idmap_count(HEADS); int *ord = malloc(sizeof(int) * (nhd ? nhd : 1)); for (size_t i = 0; i < nhd; i++) ord[i] = (int)i;
          for (size_t i = 1; i < nhd; i++) { int v = ord[i]; size_t j = i; const HeadStat *a = lp_idmap_at(HEADS, (size_t)v);
              while (j > 0) { const HeadStat *b = lp_idmap_at(HEADS, (size_t)ord[j - 1]); if (b->force > a->force || (b->force == a->force && memcmp(lp_idmap_key(HEADS, (size_t)ord[j - 1]), lp_idmap_key(HEADS, (size_t)v), 16) < 0)) break; ord[j] = ord[j - 1]; j--; } ord[j] = v; }
          printf("           heads: %zu relations respond; the strongest:", nhd);
          for (size_t i = 0; i < nhd && i < 8; i++) { const HeadStat *a = lp_idmap_at(HEADS, (size_t)ord[i]); reader_want(st->rd, lp_idmap_key(HEADS, (size_t)ord[i])); char *tn = reader_text(st->rd, lp_idmap_key(HEADS, (size_t)ord[i]), 24);
              printf(" %s(%ld strands, %d words, %.1f)", tn, a->strands, bit_count_and(&a->support, &a->support), a->force); free(tn); }
          printf("\n"); free(ord); }
      for (int round = 1; round < fw.rounds; round++) { double tl = now(); lp_idmap *hm = lp_idmap_sized(sizeof(HeadStat)); int fresh_n = layer_round(st, &fd, round, hm);
          printf("LAYER %-3d  %d entities new, coupled from the hardest of the round before   (%.1f ms)\n", round + 1, fresh_n, (now() - tl) * 1000);
          char lab[16]; snprintf(lab, sizeof lab, "heads"); heads_say(st, hm, lab); lp_idmap_free(hm); if (!fresh_n) break; }
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
    for (int i = 0; i < R.nc; i++) reader_want(st->rd, &fd.c[R.cand[i].cell].id);
    for (int i = 0; i < nnear && i < 5; i++) reader_want(st->rd, &near[i].id);
    Next nx0[64]; int nn0 = 0;
    { double t = now(); Args ra = { 0 }; arg_ids(&ra, st->occ, (size_t)st->nocc); arg_int(&ra, fw.fan);
      PGresult *fq = ask_st(st, "SELECT i, j, paths, next, times FROM @extschema@.laplace_forward($1::@extschema@.blake3[], $2::bigint) ORDER BY j, i, times DESC, next", &ra);
      { lp_idmap *seen0 = lp_idmap_new();
        for (int r = 0; r < PQntuples(fq) && nn0 < 64; r++) {
            if ((int)col_int(fq, r, 1) != st->nocc || PQgetisnull(fq, r, 3)) continue;
            bool fresh; lp_idmap_put(seen0, col_id(fq, r, 3), &fresh); if (!fresh) continue;
            nx0[nn0].id = *col_id(fq, r, 3); nx0[nn0].times = (long)col_int(fq, r, 4);
            nx0[nn0].len = (int)(col_int(fq, r, 1) - col_int(fq, r, 0) + 1); nn0++;
        }
        lp_idmap_free(seen0); }
      int expected = 0, surprised = 0, revised = 0;
      for (int p = 0; p < st->nocc; p++) {
          char *w = reader_text(st->rd, &st->occ[p], 24); printf("READ %-3d   %-12s", p + 1, w); free(w);
          /* expectation: the longest run ending at p - 1 that something follows, and whether it is followed by this */
          if (p > 0) { int seg = -1; long times = -1, total = 0;
              for (int r = 0; r < PQntuples(fq); r++) { int i_ = (int)col_int(fq, r, 0), j_ = (int)col_int(fq, r, 1); if (j_ != p || PQgetisnull(fq, r, 3) || col_int(fq, r, 2) > fw.fan) continue;
                  if (seg < 0 || i_ < seg) { seg = i_; times = -1; total = 0; } if (i_ != seg) continue;
                  long tm = (long)col_int(fq, r, 4); total += tm; if (lp_id_eq(col_id(fq, r, 3), &st->occ[p])) times = tm; }
              /* a longer run before it that more than the fan hold is a hub: its rows are not crossed, but the index
               * counts it: how many observations hold the run, and how many hold the run and this one too */
              int hubseg = -1; for (int r = 0; r < PQntuples(fq); r++) { int i_ = (int)col_int(fq, r, 0), j_ = (int)col_int(fq, r, 1); if (j_ == p && col_int(fq, r, 2) > fw.fan && (hubseg < 0 || i_ < hubseg)) hubseg = i_; }
              if (hubseg > 0 && (seg < 0 || hubseg < seg)) { int len = p - hubseg + 1; long c_run = holds_count(st->pg,st->occ + hubseg - 1, len, 0), c_with = holds_count(st->pg,st->occ + hubseg - 1, len + 1, 0); st->trips += 2;
                  if (c_with > 0) { expected++; printf(" held with the %d before it in %ld of the %ld observations that hold those", len, c_with, c_run); }
                  else { surprised++; printf(" held with the %d before it in none of the %ld observations that hold those", len, c_run); } }
              else if (seg < 0) printf(" nothing observed before it predicts what follows");
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
      printf("ORIENT     %s", !nbound ? (st->refused && !fd.n ? "refused: blocked by active firmware policy" : capped ? "resource-bounded: no joint binding among the candidates weighed" : "inconsistent: no reading of one word is compatible with a reading of another")
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
    disposition = ambiguous ? "ambiguous: more than one reading survives, and an ambiguous orientation emits nothing" : (nbound || fd.n) ? "unresolved" : (st->refused && !fd.n) ? "refused: blocked by active firmware policy" : capped ? "resource-bounded: no joint binding among the candidates weighed" : "inconsistent: no word's reading holds together with another's";
    int budget = ambiguous ? 0 : fw.emit;                                   /* an ambiguous orientation finalizes and emits nothing (Sequence 18.12) */
    int owed0 = owed(&st->open, &st->open);                                 /* what the obligations owe at the start of the turn */

    for (int step = 0; step < budget; step++) {
        /* PROPOSE: what follows the active trajectory as a run in what was observed, what the coupled field
         * responds with across strands/layers/scans, and what the nearest analogous curves do next. */
        Prop *pp = malloc(sizeof(Prop) * (size_t)(160)); int np = 0;
        Next nx[64]; int nn = step == 0 ? nn0 : follows(st, nx, 64);
        if (step == 0 && nn0) memcpy(nx, nx0, sizeof(Next) * (size_t)nn0);
        const lp_highway *hw = lp_highway_map(NULL);
        for (int i = 0; i < nn; i++) { if (hw && lp_highway_bank_of(hw, &nx[i].id, NULL)) continue;   /* a bank's value (a part of speech, a relation) is a feature, never what is said */
            Prop p; memset(&p, 0, sizeof p); p.id = nx[i].id; p.kind = P_FOLLOW; p.cont = nx[i].len; p.times = nx[i].times; p.occ = -1; p.shape = INFINITY;
            { const Cell *x = cell_find(&fd, &nx[i].id); if (x) { p.conf = x->has_r ? lp_confidence(&x->r, fw.k) : 0; p.hub = x->hub; } }
            weigh_prop(st, &fd, &p); pp[np++] = p; }
        /* Propose from the coupled field (strands, layers, scan) for entities that ground open obligations. */
        for (int z = 0; z < fd.n && np < 128; z++) {
            const Cell *x = &fd.c[z];
            if (x->hub || !bit_count_and(&x->support, &st->open)) continue;
            if (hw && lp_highway_bank_of(hw, &x->id, NULL)) continue;
            int list; uint32_t slot;
            if (hw && reading_of(hw, &x->id, &list, &slot)) continue;
            int already = 0;
            for (int k = 0; k < nemit && !already; k++) already = lp_id_eq(&emitted[k], &x->id);
            for (int k = 0; k < st->nocc && !already; k++) already = lp_id_eq(&st->occ[k], &x->id);
            for (int k = 0; k < np && !already; k++) already = lp_id_eq(&pp[k].id, &x->id);
            if (already) continue;
            Prop fp; memset(&fp, 0, sizeof fp); fp.id = x->id; fp.kind = P_CHAIN; fp.occ = -1; fp.shape = INFINITY;
            fp.conf = x->has_r ? lp_confidence(&x->r, fw.k) : 0; fp.hub = x->hub;
            weigh_prop(st, &fd, &fp);
            if (fp.grounds > 0) pp[np++] = fp;
        }
        /* the pass's own shape: the observed curves nearest its recent window, and what each does next after the last
         * place it meets the window; a constituent proposed this way that a run also proposes takes the nearer shape */
        { int wn = st->npass < 8 ? st->npass : 8; const lp_id *win = st->pass + st->npass - wn; Curve an[8]; int na = shape_near(st, win, wn, an, 8);
          for (int c = 0; c < na; c++) { int meet = -1;
              for (int k = an[c].nv - 1; k >= 0 && meet < 0; k--) for (int z = 0; z < wn; z++) if (lp_id_eq(&an[c].v[k], &win[z])) { meet = k; break; }
              int nxt = -1; for (int k = meet + 1; meet >= 0 && k < an[c].nv; k++) if (lp_tier0_codepoint(T0, &an[c].v[k]) < 0) { nxt = k; break; }
              if (c < 2) { reader_want(st->rd, &an[c].id); char *tc = reader_text(st->rd, &an[c].id, 70); printf("           like the pass: %.4f \"%s\"%s\n", an[c].d, tc, nxt < 0 ? ", nothing after where it meets the pass" : ""); free(tc); }
              if (nxt < 0) continue; const lp_id *id = &an[c].v[nxt]; int have = -1; for (int z = 0; z < np; z++) if (lp_id_eq(&pp[z].id, id)) have = z;
              if (have >= 0) { if (an[c].d < pp[have].shape) pp[have].shape = an[c].d; continue; }
              if (np >= 160 || (hw && lp_highway_bank_of(hw, id, NULL))) continue;
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
        for (int z = 0; z < np && z < 3; z++) reader_want(st->rd, &pp[z].id);
        for (int z = 0; z < np && z < 3; z++) { char *tz = reader_text(st->rd, &pp[z].id, 40);
            printf("           %s \"%s\": grounds %.2f by %d channel%s (strand %d, walks %d, beside %d words), continuity %d, beside %ld, walks %ld, shape %.4f, confidence %.3f\n", z ? "  then" : "elect", tz,
                   pp[z].grounds / 1000.0, pp[z].agree, pp[z].agree == 1 ? "" : "s", bit_count_and(&pp[z].by[CH_STRAND], &pp[z].by[CH_STRAND]), bit_count_and(&pp[z].by[CH_WALK], &pp[z].by[CH_WALK]),
                   bit_count_and(&pp[z].by[CH_BESIDE], &pp[z].by[CH_BESIDE]), pp[z].cont, pp[z].cooc, pp[z].walk, isfinite(pp[z].shape) ? pp[z].shape : -1.0, pp[z].conf); free(tz); }
        int tied[160], nt = 0; for (int z = 0; z < np && nt < 160; z++) if (!z || cannot_tell(&pp[0], &pp[z], &fw)) tied[nt++] = z;
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
        for (int c = 0; c < 3; c++) bit_clear(&st->open, &sel.by[c]);
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
        Ref resp = text_ref(CTX[0], (const uint8_t *)out, outn); Ref tp[2] = { pr, resp }; tp[0].said = tp[1].said = 0; Ref turn = compose(tp, 2, ref_above(tp, 2));
        const lp_trust_class *uc = lp_trust_class_named("UserPromptContent"), *rc = lp_trust_class_named("ResponseContent");
        File f[2]; memset(f, 0, sizeof f);
        Ref p1[2] = { handle, turn }; p1[0].said = LP_SAID_TUPLE; p1[1].said = 0; Ref c1 = said_claim(compose(p1, 2, ref_above(p1, 2)));
        c1.outcome = (uint8_t)lp_outcome_of(1.0, &c1.spare); c1.position = (uint32_t)ordinal;
        Ref p2[2] = { resp, pr }; p2[0].said = p2[1].said = 0; Ref c2 = said_claim(compose(p2, 2, ref_above(p2, 2)));
        c2.outcome = (uint8_t)lp_outcome_of(1.0, &c2.spare);
        Ref lap = text_ref(CTX[0], (const uint8_t *)"Laplace", 7);
        Ref u_tr[2] = { said_metadata(who), c1 }; Ref u_trunk = compose(u_tr, 2, ref_above(u_tr, 2));
        Ref r_tr[2] = { said_metadata(lap), c2 }; Ref r_trunk = compose(r_tr, 2, ref_above(r_tr, 2));
        f[0].path = "the prompt"; f[0].witness = who; f[0].trust = uc ? uc->prior : 0.3; f[0].trunk = u_trunk; f[0].file = u_trunk; f[0].has_file = 1;
        Event e1 = { .claim = c1.id, .witnessed = c1.id, .score = 1.0f, .enter_rating = 1500.0f, .position = (uint32_t)ordinal, .kind = EV_CLAIM }; ev_push(&f[0].ev, &e1);
        f[1].path = "the response"; f[1].witness = lap; f[1].trust = rc ? rc->prior : 0.2; f[1].trunk = r_trunk; f[1].file = r_trunk; f[1].has_file = 1;
        Event e2 = { .claim = c2.id, .witnessed = c2.id, .score = 1.0f, .enter_rating = 1500.0f, .kind = EV_CLAIM }; ev_push(&f[1].ev, &e2);
        Fold *fold = fold_new(); fold_events(fold, f, 2);
        LoadStats ls = { 0 };
        LoadWitness lw[2] = { { .id = u_trunk.id, .trust = f[0].trust }, { .id = r_trunk.id, .trust = f[1].trust } };
        int rc_ = load(conninfo, 2, f, 2, &ls); fold_free(fold);
        if (rc_ || load_register(conninfo, 2, lw, 2, f, 2) || merge(conninfo, 2, &ls)) return 1;
        char ti[33]; lp_id_hex(&turn.id, ti);
        printf("WITNESS    turn %s, at %d in the session: %llu entities new, the user's [session, turn] and Laplace's [response, prompt]\n", ti, ordinal, (unsigned long long)ls.ent_rows);
        free(f[0].ev.e); free(f[1].ev.e);
    } else printf("WITNESS    %s\n", read_only ? "a read: nothing is witnessed" : "nothing emitted: nothing to witness");
    printf("\n%llu round trips, %llu for text   total %.1f ms\n", (unsigned long long)st->trips, (unsigned long long)reader_trips(st->rd), (now() - T) * 1000);
    field_free(&fd); free(bind); free(out); reader_free(st->rd); PQfinish(st->pg); free(st);
    return 0;
}
