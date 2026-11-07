/* Forward program: COUPLE, LAYER, FOLLOWS, SHAPE, WALK, CONTEXT, and CHAIN planes. */
#include "program_internal.h"
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

void resolve_roles(State *st){
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
    { double *pc = malloc(sizeof(double) * 4 * (size_t)n); uint8_t *hc = malloc((size_t)n); coords_of(st->pg, ids, n, pc, hc); double cen[4] = { 0 }; int m = 0;
      for (int k = 0; k < n; k++) if (hc[k]) { for (int d = 0; d < 4; d++) cen[d] += pc[4 * k + d]; m++; }
      if (m >= 2) { for (int d = 0; d < 4; d++) cen[d] /= m; uint8_t pt[64]; size_t pl = lp_ewkb_point4(cen, pt, sizeof pt);
          Args a = { 0 }; arg_raw(&a, pt, pl); arg_int(&a, fw->nearest);
          double tk = now(); PGresult *q = ask_st(st, "SELECT e.id FROM @extschema@.entity e WHERE e.tier >= 3 ORDER BY e.coord OPERATOR(@extschema@.<~>) @postgis@.ST_GeomFromEWKB($1::bytea), e.id LIMIT $2", &a);
          int nq = PQntuples(q); lp_id *gid = malloc(sizeof(lp_id) * (size_t)(nq ? nq : 1)); for (int r = 0; r < nq; r++) memcpy(gid[r].b, PQgetvalue(q, r, 0), 16); PQclear(q); args_free(&a);
          double tp = now(); int ng = 0; Hold *g = paths_of(st->pg, gid, nq, &ng); for (int i = 0; i < ng && nh < nh0 + fw->fan + 1; i++) { h[nh++] = g[i]; extra++; } free(g); free(gid);
          if (getenv("LAPLACE_TIMES")) fprintf(stderr, "times: the GiST's %d nearest in %.1f ms, their paths in %.1f ms\n", nq, (tp - tk) * 1000, (now() - tp) * 1000); }
      free(pc); free(hc); }
    lp_idmap *seen = lp_idmap_new(); int *pick = malloc(sizeof(int) * (size_t)(nh ? nh : 1)), np_ = 0; lp_idmap *verts = lp_idmap_new();
    for (int i = 0; i < nh; i++) { if (h[i].claim || lp_id_eq(&h[i].entity, &st->prompt)) continue; bool f; lp_idmap_put(seen, &h[i].entity, &f); if (!f) continue; pick[np_++] = i; }
    lp_id **cv = malloc(sizeof(lp_id *) * (size_t)(np_ ? np_ : 1)); int *cn = malloc(sizeof(int) * (size_t)(np_ ? np_ : 1));
    for (int c = 0; c < np_; c++) { const Hold *x = &h[pick[c]]; size_t nv = lp_path_ids(x->path, (size_t)x->path_len, NULL, 0); cv[c] = malloc(sizeof(lp_id) * (nv ? nv : 1)); cn[c] = (int)lp_path_ids(x->path, (size_t)x->path_len, cv[c], nv);
        for (int k = 0; k < cn[c]; k++) { bool f; lp_idmap_put(verts, &cv[c][k], &f); } }
    for (int k = 0; k < n; k++) { bool f; lp_idmap_put(verts, &ids[k], &f); }
    size_t nv = lp_idmap_count(verts); double *xyz = malloc(sizeof(double) * 4 * (nv ? nv : 1)); uint8_t *has = malloc(nv ? nv : 1);
    coords_of(st->pg, lp_idmap_keys(verts), (int)nv, xyz, has);
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
lp_idmap *HEADS;                                                     /* every relation whose strands respond, with how many, how hard, from which words */
static void head_note(Cell *x, const lp_id *rel, double conf, int occ){
    int have = 0; for (int k = 0; k < x->nheads && k < CELL_HEADS; k++) have |= lp_id_eq(&x->heads[k], rel);
    if (!have) { if (x->nheads < CELL_HEADS) x->heads[x->nheads] = *rel; x->nheads++; }
    if (!HEADS) HEADS = lp_idmap_sized(sizeof(HeadStat)); bool f; HeadStat *s = lp_idmap_at(HEADS, lp_idmap_put(HEADS, rel, &f)); if (f) memset(s, 0, sizeof *s);
    s->strands++; s->force += conf; if (occ >= 0) bit_set(&s->support, occ);
}
/* ---- a layer (Forward 20.2 and the correspondence: "one routed processing and fold round over the enabled planes"):
 * the entities that responded hardest in the round before, not the prompt's own words, not a hub, are coupled in turn,
 * their strands one Native frontier; what each reaches inherits the words it was reached from (its support) and is
 * pulled as hard as the strand's confidence times the force that reached its source, shared out. So a deeper entity
 * grounds an obligation through composition, as a later layer's attention reads an earlier layer's output. */
static int by_force_layer(const void *a, const void *b){ const Cell *x = *(const Cell *const *)a, *y = *(const Cell *const *)b; if (x->force != y->force) return x->force > y->force ? -1 : 1; return memcmp(&x->id, &y->id, 16); }
int layer_round(State *st, Field *fd, int round, lp_idmap *heads_out){
    const Firmware *fw = st->fw; Cell **c = malloc(sizeof(Cell *) * (size_t)(fd->n ? fd->n : 1)); int m = 0;
    for (int z = 0; z < fd->n; z++) { Cell *x = &fd->c[z]; if (x->layer != round - 1 || x->hub || !(x->force > 0) || x->segment) continue;
        int own = 0; for (int i = 0; i < st->nocc && !own; i++) own = lp_id_eq(&st->occ[i], &x->id); if (own || lp_tier0_codepoint(T0, &x->id) >= 0) continue; c[m++] = x; }
    if (!m) { free(c); return 0; }
    qsort(c, (size_t)m, sizeof(Cell *), by_force_layer); if (m > fw->breadth) m = fw->breadth;
    lp_id *keys = malloc(sizeof(lp_id) * (size_t)m); Bits *sup = malloc(sizeof(Bits) * (size_t)m); double *fo = malloc(sizeof(double) * (size_t)m);
    for (int i = 0; i < m; i++) { keys[i] = c[i]->id; sup[i] = c[i]->support; fo[i] = c[i]->force; }
    free(c);
    int *hub = malloc(sizeof(int) * 2 * (size_t)m), nh = 0; Hold *h = holds_capped(st->pg,keys, m, fw->fan, 1, 1, hub, &nh); st->trips++;
    int *outdeg = calloc((size_t)m, sizeof(int)); Ids rn = { 0 };
    for (int r = 0; r < nh; r++) { int s = h[r].src; if (s >= 0 && s < m && h[r].claim && h[r].stood) outdeg[s]++; }
    int fresh_n = 0;
    for (int r = 0; r < nh; r++) { int s = h[r].src; if (s < 0 || s >= m || hub[2 * s] || !h[r].claim || !h[r].stood) continue;
        path_into(&rn, lp_path_of(h[r].path, (size_t)h[r].path_len)); if (rn.n < 2 || !head_read(fw, rn.v, rn.n)) continue;
        int other = lp_tuple_other(rn.v, rn.n, &keys[s]); if (other < 0 || lp_id_eq(&rn.v[other], &keys[s]) || lp_id_eq(&rn.v[other], &st->prompt)) continue;
        lp_id rel = rn.n >= 3 ? rn.v[1] : keys[s];
        double conf = lp_confidence(&h[r].r, fw->k) * strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n) * fo[s] / (outdeg[s] ? outdeg[s] : 1);
        int before = fd->n; Cell *x = cell(fd, &rn.v[other]); if (fd->n > before) { x->layer = round; fresh_n++; }
        x->force += conf; x->routes[R_LAYER]++; for (int w = 0; w < MAXOCC / 64; w++) x->support.w[w] |= sup[s].w[w];
        { int have = 0; for (int k = 0; k < x->nheads && k < CELL_HEADS; k++) have |= lp_id_eq(&x->heads[k], &rel); if (!have) { if (x->nheads < CELL_HEADS) x->heads[x->nheads] = rel; x->nheads++; } }
        if (heads_out) { bool f; HeadStat *hs = lp_idmap_at(heads_out, lp_idmap_put(heads_out, &rel, &f)); if (f) memset(hs, 0, sizeof *hs); hs->strands++; hs->force += conf; for (int w = 0; w < MAXOCC / 64; w++) hs->support.w[w] |= sup[s].w[w]; }
        if (!x->has_r) { x->r = h[r].r; x->has_r = 1; x->via = keys[s]; x->rel = rel; } }
    holds_free(h, nh); free(hub); free(outdeg); free(keys); free(sup); free(fo); lp_vec_free(&rn);
    return fresh_n;
}
void heads_say(State *st, lp_idmap *hm, const char *label){
    size_t nhd = lp_idmap_count(hm); int *ord = malloc(sizeof(int) * (nhd ? nhd : 1)); for (size_t i = 0; i < nhd; i++) ord[i] = (int)i;
    for (size_t i = 1; i < nhd; i++) { int v = ord[i]; size_t j = i; const HeadStat *a = lp_idmap_at(hm, (size_t)v);
        while (j > 0) { const HeadStat *b = lp_idmap_at(hm, (size_t)ord[j - 1]); if (b->force > a->force || (b->force == a->force && memcmp(lp_idmap_key(hm, (size_t)ord[j - 1]), lp_idmap_key(hm, (size_t)v), 16) < 0)) break; ord[j] = ord[j - 1]; j--; } ord[j] = v; }
    printf("           %s: %zu relations respond; the strongest:", label, nhd);
    for (size_t i = 0; i < nhd && i < 8; i++) { const HeadStat *a = lp_idmap_at(hm, (size_t)ord[i]); reader_want(st->rd, lp_idmap_key(hm, (size_t)ord[i])); char *tn = reader_text(st->rd, lp_idmap_key(hm, (size_t)ord[i]), 24);
        printf(" %s(%ld strands, %d words, %.2f)", tn, a->strands, bit_count_and(&a->support, &a->support), a->force); free(tn); }
    printf("\n"); free(ord);
}
/* COUPLE through the shared Native containment reader (holds_capped, one Native frontier): for each entity, the claims
 * that hold it (its strands, the other end answering with the strand's standing) and the observations that hold it
 * (containment), a hub's never read; and the shape, measured here. */
int couple(State *st, Field *fd, const lp_id *ids, int n, const int *occ_of, int kind, Curve *near, int keep){
    if (!n) return 0; const Firmware *fw = st->fw;
    if (!getenv("LAPLACE_COUPLE_SQL")) {
        lp_id *keys = malloc(sizeof(lp_id) * (size_t)n); int *of = malloc(sizeof(int) * (size_t)n), nk = 0;
        for (int o = 0; o < n; o++) { keys[nk] = ids[o]; of[nk++] = o; }
        int *hub = malloc(sizeof(int) * 2 * (size_t)(nk ? nk : 1)), nh = 0; Hold *h = holds_capped(st->pg,keys, nk, fw->fan, 1, 3, hub, &nh); st->trips++;
        Ids rn = { 0 };
        for (int i = 0; i < nh; i++) { int s = h[i].src; if (s < 0 || s >= nk) continue; int o = of[s];
            int occ = occ_of ? occ_of[o] : -1; double pull = occ >= 0 && occ < st->nocc ? st->role[occ] : kind == R_CLAIM ? 1.0 : 0.5;
            if (lp_id_eq(&h[i].entity, &st->prompt)) continue;
            if (h[i].claim) { if (!h[i].stood) continue; path_into(&rn, lp_path_of(h[i].path, (size_t)h[i].path_len)); if (rn.n < 2) continue;
                if (!head_read(fw, rn.v, rn.n)) continue;
                int other = lp_tuple_other(rn.v, rn.n, &ids[o]); if (other < 0 || lp_id_eq(&rn.v[other], &st->prompt)) continue;
                lp_id p3[3] = { ids[o], rn.n >= 3 ? rn.v[1] : ids[o], rn.v[other] };
                double conf = lp_confidence(&h[i].r, fw->k) * strand_weight(fw, fw->id.weigh, p3, 3);
                Cell *x = cell(fd, &rn.v[other]); x->force += conf * pull; x->routes[kind]++; if (occ >= 0) bit_set(&x->support, occ); if (pull > 0) head_note(x, &p3[1], conf * pull, occ);      /* a head is what pulls through it: a space or a mark pulls nothing */
                if (!x->has_r || conf > lp_confidence(&x->r, fw->k)) { x->r = h[i].r; x->has_r = 1; x->via = p3[0]; x->rel = p3[1]; } }
            else { Cell *x = cell(fd, &h[i].entity); x->segment = 1; x->tier = h[i].tier; x->force += pull; x->routes[R_CONTAIN]++; if (occ >= 0) bit_set(&x->support, occ); } }
        /* a word whose strands are more than the fan is reached, not crossed: its on-ramp is read instead, the word and
         * each relation the firmware bubbles up by, together (Semantics: a word bubbles up to its highway node) */
        if (fw->nup) { lp_id hk[MAXOCC]; int hof[MAXOCC], nhk = 0; for (int k = 0; k < nk && nhk < MAXOCC; k++) if (hub[2 * k] && lp_tier0_codepoint(T0, &keys[k]) < 0) { hk[nhk] = keys[k]; hof[nhk++] = of[k]; }
            for (int u = 0; u < fw->nup && nhk; u++) { int n2 = 0; Hold *h2 = holds_both(st->pg,hk, nhk, &fw->id.up[u], &n2); st->trips++; int read_ = 0;
                for (int i = 0; i < n2; i++) { int s = h2[i].src; if (s < 0 || s >= nhk || !h2[i].claim) continue; int o = hof[s]; int occ = occ_of ? occ_of[o] : -1; double pull = occ >= 0 && occ < st->nocc ? st->role[occ] : 1.0;
                    path_into(&rn, lp_path_of(h2[i].path, (size_t)h2[i].path_len)); if (rn.n < 2 || !head_read(fw, rn.v, rn.n)) continue;
                    int other = lp_tuple_other(rn.v, rn.n, &hk[s]); if (other < 0) continue;
                    if (!h2[i].stood) continue; lp_rating rt = h2[i].r;
                    lp_id p3[3] = { hk[s], rn.n >= 3 ? rn.v[1] : hk[s], rn.v[other] }; double conf = lp_confidence(&rt, fw->k);
                    Cell *x = cell(fd, &rn.v[other]); x->force += conf * pull; x->routes[kind]++; if (occ >= 0) bit_set(&x->support, occ); if (pull > 0) head_note(x, &p3[1], conf * pull, occ);
                    if (!x->has_r) { x->r = rt; x->has_r = 1; x->via = p3[0]; x->rel = p3[1]; } read_++; }
                holds_free(h2, n2); (void)read_; } }
        lp_vec_free(&rn);
        int nn = keep > 0 ? shape_measured(st, ids, n, h, nh, near, keep) : 0;
        holds_free(h, nh); free(hub); free(keys); free(of); return nn;
    }
    Args a = { 0 }; arg_ids(&a, ids, (size_t)n); arg_int(&a, fw->fan); arg_refused(&a); arg_int(&a, keep > 0 ? fw->shape : -1); arg_f64(&a, fw->shape_n); arg_int(&a, keep > 0 ? keep + 1 : keep);
    PGresult *q = ask_st(st, "SELECT entity, occ, route, rating, deviation, volatility, via, rel, tier, distance, vertices FROM @extschema@.laplace_couple($1::@extschema@.blake3[], $2::bigint, $3::@extschema@.blake3[], $4::smallint, $5::float8, $6::integer)", &a);
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
int follows(State *st, Next *out, int cap){
    int from = st->ntraj > 24 ? st->ntraj - 24 : 0, n = st->ntraj - from; if (n <= 0) return 0;
    Args a = { 0 }; arg_ids(&a, st->traj + from, (size_t)n); arg_int(&a, st->fw->fan);
    /* every run that ends the trajectory, not only the longest: as attention reads every position, a word that follows
     * only the last two is still a proposal, at the run it follows (its continuity) */
    PGresult *q = ask_st(st, "SELECT i, j, next, times FROM @extschema@.laplace_forward($1::@extschema@.blake3[], $2::bigint) WHERE j = array_length($1::@extschema@.blake3[], 1) AND next IS NOT NULL ORDER BY (j - i) DESC, times DESC, next", &a);
    int m = 0; lp_idmap *seen = lp_idmap_new();
    for (int r = 0; r < PQntuples(q) && m < cap; r++) { int len = (int)(col_int(q, r, 1) - col_int(q, r, 0) + 1); bool fresh; lp_idmap_put(seen, col_id(q, r, 2), &fresh); if (!fresh) continue;
        out[m].id = *col_id(q, r, 2); out[m].times = (long)col_int(q, r, 3); out[m].len = len; m++; }
    lp_idmap_free(seen); PQclear(q); args_free(&a); return m;
}
static int curve_order(const void *a, const void *b){ const Curve *x = a, *y = b; return x->d < y->d ? -1 : x->d > y->d ? 1 : memcmp(&x->id, &y->id, 16); }
/* The observed curves nearest a run of entities by the firmware's measure, through the extension's native shape
 * operator (laplace_couple's shape route: the GIN's containers of the run and the GiST's nearest to its centroid,
 * realized and measured natively); only the shape is read here. In content order: distance, then ID. */
int shape_near(State *st, const lp_id *ids, int n, Curve *near, int keep){
    if (n < 2 || keep <= 0) return 0; const Firmware *fw = st->fw;
    if (!getenv("LAPLACE_COUPLE_SQL")) { int *hub = malloc(sizeof(int) * 2 * (size_t)n), nh = 0; Hold *h = holds_capped(st->pg,ids, n, fw->fan, 0, 2, hub, &nh); st->trips++;
        int k = shape_measured(st, ids, n, h, nh, near, keep); holds_free(h, nh); free(hub); return k; }
    Args a = { 0 }; arg_ids(&a, ids, (size_t)n); arg_int(&a, fw->fan); arg_refused(&a); arg_int(&a, fw->shape); arg_f64(&a, fw->shape_n); arg_int(&a, keep + 4);
    PGresult *q = ask_st(st, "SELECT entity, occ, route, rating, deviation, volatility, via, rel, tier, distance, vertices FROM @extschema@.laplace_couple($1::@extschema@.blake3[], $2::bigint, $3::@extschema@.blake3[], $4::smallint, $5::float8, $6::integer) WHERE route = 2", &a);
    int m = PQntuples(q), nn = 0; Curve *all = malloc(sizeof(Curve) * (size_t)(m ? m : 1));
    for (int r = 0; r < m; r++) { const lp_id *id = col_id(q, r, 0); if (lp_id_eq(id, &st->prompt)) continue; Curve *c = &all[nn++]; c->id = *id; c->d = col_f64(q, r, 9);
        const uint8_t *v = (const uint8_t *)PQgetvalue(q, r, 10); size_t vl = (size_t)PQgetlength(q, r, 10), na = lp_pg_ids_read(v, vl, NULL, 0);
        c->v = malloc(sizeof(lp_id) * (na ? na : 1)); c->nv = (int)lp_pg_ids_read(v, vl, c->v, na); }
    qsort(all, (size_t)nn, sizeof(Curve), curve_order);
    int k = nn < keep ? nn : keep; for (int i = 0; i < k; i++) near[i] = all[i]; for (int i = k; i < nn; i++) free(all[i].v);
    free(all); PQclear(q); args_free(&a); return k;
}
typedef struct { lp_id to; double w; } Hop_;
typedef struct Adj_ { Hop_ *h; int n; size_t cap; int hub; } Adj;
static int hop_cmp(const void *a, const void *b){ const Hop_ *x = a, *y = b; int c = memcmp(&x->to, &y->to, 16); return c ? c : (x->w < y->w ? -1 : x->w > y->w); }
void walk(State *st, int from, int to){                                 /* the walkers of positions [from, to) */
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
        if (nn) {                                                             /* the strands of every place not stood on before: one set, one Native frontier, a hub's never read */
            int *hub = malloc(sizeof(int) * 2 * (size_t)nn), nh = 0; Hold *hh = holds_capped(st->pg,need, nn, fw->walk_fan, 1, 1, hub, &nh); st->trips++;
            int *held = calloc((size_t)nn, sizeof(int)); for (int e = 0; e < nn; e++) held[e] = hub[2 * e] ? fw->walk_fan + 1 : 0;
            for (int r = 0; r < nh; r++) { int e = hh[r].src; if (e < 0 || e >= nn || held[e] > fw->walk_fan || !hh[r].claim || !hh[r].stood) continue;
                path_into(&rn, lp_path_of(hh[r].path, (size_t)hh[r].path_len));
                if (!head_read(fw, rn.v, rn.n)) continue;
                int other = lp_tuple_other(rn.v, rn.n, &need[e]); if (other < 0 || lp_id_eq(&rn.v[other], &need[e])) continue;
                lp_rating rt = hh[r].r; double w = lp_confidence(&rt, fw->k) * strand_weight(fw, fw->id.weigh, rn.v, (int)rn.n); if (!(w > 0)) continue;
                Adj *x = &adj[lp_idmap_find(am, &need[e])]; lp_reserve((void **)&x->h, &x->cap, (size_t)x->n + 1, sizeof(Hop_)); x->h[x->n++] = (Hop_){ rn.v[other], w }; }
            for (int e = 0; e < nn; e++) { Adj *x = &adj[lp_idmap_find(am, &need[e])]; x->hub = held[e] > fw->walk_fan; }
            /* a hub is walked through its on-ramp: the place and each relation the firmware bubbles up by, together */
            if (fw->nup) { lp_id *hk = malloc(sizeof(lp_id) * (size_t)nn); int nhk = 0; for (int e = 0; e < nn; e++) if (held[e] > fw->walk_fan) hk[nhk++] = need[e];
                for (int u = 0; u < fw->nup && nhk; u++) { int n2 = 0; Hold *h2 = holds_both(st->pg,hk, nhk, &fw->id.up[u], &n2); st->trips++;
                    for (int r2 = 0; r2 < n2; r2++) { int s_ = h2[r2].src; if (s_ < 0 || s_ >= nhk || !h2[r2].claim || !h2[r2].stood) continue; path_into(&rn, lp_path_of(h2[r2].path, (size_t)h2[r2].path_len));
                        if (rn.n < 2 || !head_read(fw, rn.v, rn.n)) continue; int other = lp_tuple_other(rn.v, rn.n, &hk[s_]); if (other < 0 || lp_id_eq(&rn.v[other], &hk[s_])) continue;
                        double w = lp_confidence(&h2[r2].r, fw->k); if (!(w > 0)) continue;
                        Adj *x = &adj[lp_idmap_find(am, &hk[s_])]; lp_reserve((void **)&x->h, &x->cap, (size_t)x->n + 1, sizeof(Hop_)); x->h[x->n++] = (Hop_){ rn.v[other], w }; x->hub = 0; }
                    holds_free(h2, n2); }
                free(hk); }
            for (int e = 0; e < nn; e++) { Adj *x = &adj[lp_idmap_find(am, &need[e])]; if (x->n > 1) qsort(x->h, (size_t)x->n, sizeof(Hop_), hop_cmp); }
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
void context(State *st, int from, int to){                              /* the context of positions [from, to) */
    if (!CTX_OBS) CTX_OBS = lp_idmap_new(); if (!st->ctx_all) st->ctx_all = lp_idmap_new(); lp_idmap *obs = CTX_OBS; Ids ids = { 0 };
    /* every position's observations in one set, one Native frontier; a position more than the fan holds is a hub, its
     * observations never read. The same word again shares its context. */
    lp_id keys[MAXOCC]; int at[MAXOCC], nk = 0;
    for (int i = from; i < to; i++) { st->ctx[i] = NULL; st->ctx_n[i] = 0; st->ctx_hub[i] = 0; at[i - from] = -1;
        if (!st->composed[i] || st->role[i] <= 0) continue;
        int dup = -1; for (int j = 0; j < from && dup < 0; j++) if (lp_id_eq(&st->occ[j], &st->occ[i]) && (st->ctx[j] || st->ctx_hub[j])) dup = j;
        if (dup >= 0) { st->ctx[i] = st->ctx[dup]; st->ctx_n[i] = st->ctx_n[dup]; st->ctx_hub[i] = st->ctx_hub[dup]; continue; }
        int k = -1; for (int z = 0; z < nk; z++) if (lp_id_eq(&keys[z], &st->occ[i])) k = z;
        if (k < 0) { keys[nk] = st->occ[i]; k = nk++; } at[i - from] = k; }
    if (!nk) { lp_vec_free(&ids); return; }
    int *hub = malloc(sizeof(int) * 2 * (size_t)nk), nh = 0; Hold *h = holds_capped(st->pg,keys, nk, st->fw->fan, 0, 2, hub, &nh); st->trips++;
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
int beside_grounds(const State *st, int i, const lp_id *id){
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
/* A text's languages from recipe-declared lexicalization parts, in two sets. */
int langs_of(State *st, const lp_id *w, int n, lp_id (*out)[LANGS], int *nout){
    const Firmware *fw = st->fw; memset(nout, 0, sizeof(int) * (size_t)n); if (!n || !fw->lexical[0][0]) return 0;
    if (!st->lexical.np && !lexicalization_of(st->c, fw, &st->lexical)) exit(2);
    const Lexicalization *lx = &st->lexical;
    int nc = 0, *src = NULL; Claim *cl = claims_each(st->pg, w, n, &lx->sense, CLAIM_FIRST, fw->fan, fw->k, &nc, &src); st->trips++;
    lp_id *sense = malloc(sizeof(lp_id) * (size_t)(nc ? nc : 1));
    for (int entry = 0; entry < nc; entry++) sense[entry] = cl[entry].part[cl[entry].np - 1];
    int nh = 0; Hold *h = paths_of(st->pg, sense, nc, &nh); st->trips += nc > 0; Ids parts = { 0 };
    for (int row = 0; row < nh; row++) { int entry = h[row].src; if (entry < 0 || entry >= nc) continue;
        int word = src[entry]; if (word < 0 || word >= n) continue;
        path_into(&parts, lp_path_of(h[row].path, (size_t)h[row].path_len)); if (parts.n != (size_t)lx->np) continue;
        const lp_id *language = &parts.v[lx->lang]; int duplicate = 0;
        for (int index = 0; index < nout[word]; index++) duplicate |= lp_id_eq(&out[word][index], language);
        if (!duplicate && nout[word] < LANGS) out[word][nout[word]++] = *language;
    }
    int known = 0; for (int word = 0; word < n; word++) known += nout[word] > 0;
    lp_vec_free(&parts); holds_free(h, nh); free(sense); free(src); free(cl); return known;
}
typedef struct { State *st; const lp_id *word; int last_hold, last_n; } ForkCtx;
static int fork_choice(void *vc, const Claim *cl, int n){
    ForkCtx *fc = vc; State *st = fc->st; const Firmware *fw = st->fw;
    lp_id *ends = malloc(sizeof(lp_id) * (size_t)n); for (int k = 0; k < n; k++) ends[k] = cl[k].part[cl[k].np - 1];
    lp_idmap **rec = calloc((size_t)n, sizeof *rec); for (int k = 0; k < n; k++) { bool f; rec[k] = lp_idmap_new(); lp_idmap_put(rec[k], &ends[k], &f); }
    /* one hop out, one Native frontier; a hub (a relation, a part of speech, a language: more strands than the fan) is reached and its strands never read */
    /* a branch is read from itself and its own parts: a sense is the word with its synset, so its synset (and the
     * synset's definitions and examples) is one step down from it before it is one step out; bubble down, then out */
    lp_id *k1 = malloc(sizeof(lp_id) * (size_t)n * 17); int *b1 = malloc(sizeof(int) * (size_t)n * 17), nk1 = 0;
    for (int k = 0; k < n; k++) { k1[nk1] = ends[k]; b1[nk1++] = k; reader_want(st->rd, &ends[k]); }
    for (int k = 0; k < n; k++) { lp_id pp[16]; size_t np = reader_parts(st->rd, &ends[k], pp, 16); if (np > 16) np = 16;
        for (size_t v = 0; v < np; v++) { if (lp_tier0_codepoint(T0, &pp[v]) >= 0 || lp_id_eq(&pp[v], fc->word) || table_find(&pp[v])) continue;
            bool f; lp_idmap_put(rec[k], &pp[v], &f); if (f) { k1[nk1] = pp[v]; b1[nk1++] = k; } } }
    int *hb = malloc(sizeof(int) * 2 * (size_t)nk1), m1 = 0; Hold *c1 = holds_capped(st->pg,k1, nk1, fw->fan, 0, 1, hb, &m1); st->trips++; Ids rv = { 0 };
    for (int r = 0; r < m1; r++) { int s1_ = c1[r].src; if (s1_ < 0 || s1_ >= nk1 || !c1[r].claim) continue; int b = b1[s1_]; path_into(&rv, lp_path_of(c1[r].path, (size_t)c1[r].path_len));
        int o = lp_tuple_other(rv.v, rv.n, &k1[s1_]); if (o < 0) continue;     /* the strand's other end; its middle (a relation) is not the branch's record */
        const lp_id *x = &rv.v[o]; bool f; lp_idmap_put(rec[b], x, &f); }
    holds_free(c1, m1); free(hb); free(k1); free(b1);
    /* one hop out is the record (Research: Trust): from the branch and its synset, its definitions, its examples and its
     * relation neighbours; the neighbours' members and the observations' words come from opening them below */
    lp_vec_free(&rv);
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
    /* how much each context word informs: how many observations hold it, counted by the index (a hub is counted, never
     * read), against the commonest of the prompt's words */
    if (!st->df_ready) { st->df_max = 0; for (int j = 0; j < st->npos; j++) { st->df[j] = 0; if (!st->composed[j]) continue; int seen = -1; for (int i = 0; i < j && seen < 0; i++) if (lp_id_eq(&st->occ[i], &st->occ[j])) seen = i;
            st->df[j] = seen >= 0 ? st->df[seen] : holds_count(st->pg,&st->occ[j], 1, 0); if (seen < 0) st->trips++; if (st->df[j] > st->df_max) st->df_max = st->df[j]; }
        st->df_ready = st->npos; }
    double *hold = calloc((size_t)n, sizeof(double)), *walks = calloc((size_t)n, sizeof(double)), *score = calloc((size_t)n, sizeof(double));
    for (int k = 0; k < n; k++) { size_t cnt = lp_idmap_count(rec[k]); double norm = sqrt((double)(cnt ? cnt : 1));
        for (int j = 0; j < st->npos; j++) { if (!st->composed[j] || st->role[j] <= 0 || lp_id_eq(&st->occ[j], fc->word)) continue;
            int seen = 0; for (int i = 0; i < j && !seen; i++) seen = lp_id_eq(&st->occ[i], &st->occ[j]); if (seen) continue;
            double info = log((1.0 + st->df_max) / (1.0 + st->df[j]));          /* against the prompt's commonest word: rare informs, common does not */
            if (info > 0 && lp_idmap_find(rec[k], &st->occ[j]) >= 0) hold[k] += st->role[j] * info;
            for (size_t i = 0; i < cnt; i++) walks[k] += st->role[j] * visits(st, lp_idmap_key(rec[k], i), j); }
        hold[k] /= norm; walks[k] /= norm;
        int rank = cl[k].position > 0 ? cl[k].position : k + 1; score[k] = -log((double)rank) + hold[k]; }
    /* the prior, where a tagged corpus counts the word's concepts: how often [word, prevalence, concept] was played, for
     * the concept each branch is one step down to (a sense is the word with its synset) */
    double *prev = calloc((size_t)n, sizeof(double)); double ptotal = 0;
    if (st->lexical.has_usage) { long long *uses = calloc((size_t)n, sizeof(long long));
        st->trips += lexicalization_counts(st->pg, st->rd, &st->lexical, ends, n, fw->fan, fw->k, uses);
        for (int branch = 0; branch < n; branch++) { prev[branch] = (double)uses[branch]; ptotal += prev[branch]; } free(uses);
    } else if (fw->prevalence[0]) { lp_id *cc = malloc(sizeof(lp_id) * (size_t)n * 4); int *cof = malloc(sizeof(int) * (size_t)n * 4), ncc = 0;
        for (int k = 0; k < n; k++) { lp_id pp[8]; size_t np = reader_parts(st->rd, &ends[k], pp, 8); if (np > 8) np = 8;
            for (size_t v = 0; v < np && ncc < n * 4; v++) if (!lp_id_eq(&pp[v], fc->word) && lp_tier0_codepoint(T0, &pp[v]) < 0) { cc[ncc] = pp[v]; cof[ncc++] = k; } }
        int n2 = 0; Hold *h2 = ncc ? holds_both(st->pg,cc, ncc, fc->word, &n2) : NULL; if (ncc) st->trips++; Ids pv = { 0 };
        for (int r = 0; r < n2; r++) { int s_ = h2[r].src; if (s_ < 0 || s_ >= ncc || !h2[r].claim || !h2[r].stood) continue; path_into(&pv, lp_path_of(h2[r].path, (size_t)h2[r].path_len));
            int as_pair = fw->prevalence_pair && pv.n == 2 && lp_id_eq(&pv.v[0], fc->word) && lp_id_eq(&pv.v[1], &cc[s_]);       /* [word, concept]: the statement a tagged corpus makes */
            int as_rel = !fw->prevalence_pair && pv.n == 3 && lp_id_eq(&pv.v[0], fc->word) && lp_id_eq(&pv.v[1], &fw->id.prevalence) && lp_id_eq(&pv.v[2], &cc[s_]);
            if (!as_pair && !as_rel) continue;
            prev[cof[s_]] += h2[r].matches; ptotal += h2[r].matches; }
        holds_free(h2, n2); lp_vec_free(&pv); free(cc); free(cof); }
    if (ptotal > 0) for (int k = 0; k < n; k++) score[k] = log((1.0 + prev[k]) / (1.0 + ptotal)) + hold[k];
    /* a rendering in the prompt's language first: the share of a branch's own words lexicalized in it */
    double *inlang = calloc((size_t)n, sizeof(double));
    if (st->nlang) { lp_id (*bw)[64] = calloc((size_t)n, sizeof *bw); int *nbw = calloc((size_t)n, sizeof(int)); lp_id all[4096]; int na = 0;
        for (int k = 0; k < n; k++) { lp_id pp[256]; size_t np = reader_parts(st->rd, &ends[k], pp, 256); if (np > 256) np = 256;
            for (size_t v = 0; v < np; v++) { if (lp_tier0_codepoint(T0, &pp[v]) >= 0) continue; lp_id sub_[64]; size_t ns = reader_parts(st->rd, &pp[v], sub_, 64); size_t atoms = 0; for (size_t q = 0; q < ns && q < 64; q++) atoms += lp_tier0_codepoint(T0, &sub_[q]) >= 0;
                if (ns && atoms == ns && nbw[k] < 64 && na < 4096) { bw[k][nbw[k]++] = pp[v]; all[na++] = pp[v]; } } }
        lp_id (*lg)[LANGS] = calloc((size_t)(na ? na : 1), sizeof *lg); int *nlg = calloc((size_t)(na ? na : 1), sizeof(int)); langs_of(st, all, na, lg, nlg);
        int at = 0; for (int k = 0; k < n; k++) { int known = 0, ours = 0;
            for (int v = 0; v < nbw[k]; v++, at++) { if (!nlg[at]) continue; known++; int hit = 0; for (int z = 0; z < nlg[at] && z < LANGS && !hit; z++) for (int y = 0; y < st->nlang; y++) hit |= lp_id_eq(&lg[at][z], &st->lang[y]); ours += hit; }
            inlang[k] = known ? (double)ours / known : 0; }
        free(bw); free(nbw); free(lg); free(nlg); }
    int best = 0; for (int k = 1; k < n; k++) { double tol = fw->sure * sqrt((walks[k] + walks[best]) / 4.0);
        if (inlang[k] > inlang[best] || (inlang[k] == inlang[best] && (score[k] > score[best] || (score[k] == score[best] && walks[k] > walks[best] + tol)))) best = k; }
    int told = 0; for (int k = 0; k < n; k++) if (k != best && (inlang[best] > inlang[k] || (inlang[best] == inlang[k] && (score[best] > score[k] || walks[best] > walks[k] + fw->sure * sqrt((walks[k] + walks[best]) / 4.0))))) told++;
    int pick = told == n - 1 ? best : -1;
    if (getenv("LAPLACE_TIMES")) for (int k = 0; k < n; k++) { char *tx = reader_text(st->rd, &ends[k], 50); fprintf(stderr, "fork: %s%-50s in language %.2f, record %zu, holds %.3f, counted %.0f, score %.3f, walks %.2f\n", k == pick ? "* " : "  ", tx, inlang[k], lp_idmap_count(rec[k]), hold[k], prev[k], score[k], walks[k]); free(tx); }
    fc->last_hold = (int)(hold[best] * 1000 + 0.5); fc->last_n = n;
    for (int k = 0; k < n; k++) lp_idmap_free(rec[k]);
    free(rec); free(ends); free(hold); free(walks); free(score); free(inlang); free(prev);
    return pick;
}
/* A chain the firmware names, followed from a word (chain_follow), the oriented reading taken where the chain passes
 * through it, and at every other fork the branch the context supports; *rating: the standing of its last strand. */
int chain_from(State *st, const lp_id *word, const lp_id *reading, lp_id *answer, lp_rating *rating){
    ClaimStanding last; int alt; ForkCtx fc = { st, word, 0, 0 }; chain_follow(st->pg, st->rd, (Firmware *)st->fw, word, reading, NULL, fork_choice, &fc, answer, &last, &alt);
    if (alt < 0) return 0;
    *rating = last.r; return 1;
}
