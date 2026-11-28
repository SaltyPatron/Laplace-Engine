/* O(log N) + O(k) Multi-Channel Recipe-Driven Substrate Extraction for Mold-a-Model (Export.md §25.2-25.7).
 * Queries physicality.path (GIN), typed attestation/consensus relations, unary properties, and 4D entity.coord/curves
 * per recipe slot; zero O(n^2) all-pairs loops and zero fabricated fallbacks. */
#include "forward/engine_abi.h"
#include "forward/program_internal.h"
#include "codecs/codecs.h"
#include "laplace/highway.h"
#include "laplace/os.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Minimal strict JSON cursor for laplace.recipe parsing (zero external deps) ---- */
typedef struct { const char *p; } JCur;

static void j_ws(JCur *c){ while (*c->p && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) c->p++; }

static int j_str(JCur *c, char *out, size_t cap){
    j_ws(c);
    if (*c->p != '"') return 0;
    c->p++;
    size_t n = 0;
    while (*c->p && *c->p != '"') {
        char ch = *c->p++;
        if (ch == '\\' && *c->p) {
            char e = *c->p++;
            if (e == 'n') ch = '\n';
            else if (e == 't') ch = '\t';
            else if (e == 'r') ch = '\r';
            else ch = e;
        }
        if (out && n + 1 < cap) out[n++] = ch;
    }
    if (*c->p == '"') c->p++;
    if (out && cap) out[n] = 0;
    return 1;
}

static int j_num(JCur *c, double *out){
    j_ws(c);
    char *end = NULL;
    double v = strtod(c->p, &end);
    if (end == c->p) return 0;
    c->p = end;
    if (out) *out = v;
    return 1;
}

static int j_bool(JCur *c, int *out){
    j_ws(c);
    if (!strncmp(c->p, "true", 4)) { c->p += 4; if (out) *out = 1; return 1; }
    if (!strncmp(c->p, "false", 5)) { c->p += 5; if (out) *out = 0; return 1; }
    return 0;
}

static void j_skip(JCur *c){
    j_ws(c);
    char ch = *c->p;
    if (!ch) return;
    if (ch == '"') { j_str(c, NULL, 0); return; }
    if (ch == '{') {
        c->p++;
        while (*c->p) {
            j_ws(c);
            if (*c->p == '}') { c->p++; return; }
            if (!j_str(c, NULL, 0)) { c->p++; continue; }
            j_ws(c); if (*c->p == ':') c->p++;
            j_skip(c);
            j_ws(c); if (*c->p == ',') c->p++;
        }
        return;
    }
    if (ch == '[') {
        c->p++;
        while (*c->p) {
            j_ws(c);
            if (*c->p == ']') { c->p++; return; }
            j_skip(c);
            j_ws(c); if (*c->p == ',') c->p++;
        }
        return;
    }
    while (*c->p && *c->p != ',' && *c->p != '}' && *c->p != ']' && !isspace((unsigned char)*c->p)) c->p++;
}

/* ---- Parsed Recipe Operator Schedule ---- */
#define MAX_MOLD_PLANES 32
#define MAX_MOLD_LAYERS 64
#define MAX_MOLD_HEADS  64
#define MAX_MOLD_SEEDS  64

typedef struct {
    LpMoldPlaneKind kind;
    char            tag[96];
    char            subtype[64]; /* relation type ("IS_A", "hypernym", "*") or metric ("angular", "frechet") */
    int             is_inhibitory;
} PlaneDef;

typedef struct {
    char     name[128];
    uint32_t hidden_dim;
    uint32_t n_layers;
    uint32_t n_heads;
    uint32_t n_kv_heads;
    uint32_t ffn_dim;
    int      tied_embeddings;
    uint32_t vocab_size;
    int      vocab_hops;
    int      vocab_fanout;
    char     seeds[MAX_MOLD_SEEDS][96];
    size_t   n_seeds;

    PlaneDef planes[MAX_MOLD_PLANES];
    size_t   n_planes;
    size_t   layer_heads[MAX_MOLD_LAYERS][MAX_MOLD_HEADS];
    size_t   layer_nheads[MAX_MOLD_LAYERS];
    size_t   layer_nkv[MAX_MOLD_LAYERS];
    size_t   layer_ffn[MAX_MOLD_LAYERS];
    size_t   lm_head_plane;
    int      lm_head_set_mode;
    double   lm_head_max_outside_prob;
    int      has_explicit_layers;
} RecipeSchedule;

static size_t schedule_add_plane(RecipeSchedule *rs, LpMoldPlaneKind kind, const char *subtype){
    char tag[96];
    const char *sub = (subtype && *subtype) ? subtype : "";
    if (kind == LP_MOLD_PLANE_TRAJECTORY) snprintf(tag, sizeof tag, "trajectory");
    else if (kind == LP_MOLD_PLANE_UNARY) snprintf(tag, sizeof tag, "unary");
    else if (kind == LP_MOLD_PLANE_METRIC) snprintf(tag, sizeof tag, "metric:%s", *sub ? sub : "angular");
    else snprintf(tag, sizeof tag, "relation:%s", *sub ? sub : "*");

    for (size_t i = 0; i < rs->n_planes; i++) {
        if (rs->planes[i].kind == kind && strcmp(rs->planes[i].tag, tag) == 0) return i;
    }
    if (rs->n_planes >= MAX_MOLD_PLANES) return 0;
    size_t idx = rs->n_planes++;
    PlaneDef *pd = &rs->planes[idx];
    memset(pd, 0, sizeof *pd);
    pd->kind = kind;
    snprintf(pd->tag, sizeof pd->tag, "%s", tag);
    snprintf(pd->subtype, sizeof pd->subtype, "%s", sub);
    if (kind == LP_MOLD_PLANE_RELATION &&
        (!strcmp(sub, "antonym") || !strcmp(sub, "IS_ANTONYM_OF") || !strcmp(sub, "opposite") || !strcmp(sub, "contradicts")))
        pd->is_inhibitory = 1;
    return idx;
}

static size_t parse_op_object(JCur *c, RecipeSchedule *rs, int is_lm_head){
    char op[64] = "", rtype[64] = "", metric[64] = "", scoring[32] = "";
    double max_out = 0.0;
    j_ws(c);
    if (*c->p == '{') {
        c->p++;
        while (*c->p) {
            j_ws(c);
            if (*c->p == '}') { c->p++; break; }
            char k[64] = "";
            if (!j_str(c, k, sizeof k)) { j_skip(c); continue; }
            j_ws(c); if (*c->p == ':') c->p++;
            j_ws(c);
            if (!strcmp(k, "op")) j_str(c, op, sizeof op);
            else if (!strcmp(k, "type") || !strcmp(k, "relation") || !strcmp(k, "predicate")) j_str(c, rtype, sizeof rtype);
            else if (!strcmp(k, "metric")) j_str(c, metric, sizeof metric);
            else if (!strcmp(k, "scoring")) j_str(c, scoring, sizeof scoring);
            else if (!strcmp(k, "max_outside_probability")) j_num(c, &max_out);
            else j_skip(c);
            j_ws(c); if (*c->p == ',') c->p++;
        }
    } else {
        j_skip(c);
    }

    if (is_lm_head) {
        if (!strcmp(scoring, "set")) rs->lm_head_set_mode = 1;
        if (max_out > 0.0 && max_out < 1.0) rs->lm_head_max_outside_prob = max_out;
    }

    if (!strcmp(op, "relation") || !strcmp(op, "highway") || !strcmp(op, "couple")) {
        return schedule_add_plane(rs, LP_MOLD_PLANE_RELATION, *rtype ? rtype : "*");
    }
    if (!strcmp(op, "metric") || !strcmp(op, "frechet") || !strcmp(op, "angular") || !strcmp(op, "hausdorff")) {
        const char *m = *metric ? metric : (!strcmp(op, "metric") ? "angular" : op);
        return schedule_add_plane(rs, LP_MOLD_PLANE_METRIC, m);
    }
    if (!strcmp(op, "unary") || !strcmp(op, "standing") || !strcmp(op, "consensus") || !strcmp(op, "ffn") || !strcmp(op, "swiglu")) {
        return schedule_add_plane(rs, LP_MOLD_PLANE_UNARY, "unary");
    }
    return schedule_add_plane(rs, LP_MOLD_PLANE_TRAJECTORY, "trajectory");
}

static void parse_mold_recipe(const char *json, RecipeSchedule *rs){
    memset(rs, 0, sizeof *rs);
    rs->vocab_hops = 2;
    rs->vocab_fanout = 64;
    if (!json || !*json) return;
    JCur c = { json };
    j_ws(&c);
    if (*c.p != '{') return;
    c.p++;
    while (*c.p) {
        j_ws(&c);
        if (*c.p == '}') { c.p++; break; }
        char k[64] = "";
        if (!j_str(&c, k, sizeof k)) { j_skip(&c); continue; }
        j_ws(&c); if (*c.p == ':') c.p++;
        j_ws(&c);
        if (!strcmp(k, "name")) {
            j_str(&c, rs->name, sizeof rs->name);
        } else if (!strcmp(k, "hidden_size")) {
            double v = 0; if (j_num(&c, &v) && v > 0) rs->hidden_dim = (uint32_t)v; else j_skip(&c);
        } else if (!strcmp(k, "num_layers")) {
            double v = 0; if (j_num(&c, &v) && v > 0) rs->n_layers = (uint32_t)v; else j_skip(&c);
        } else if (!strcmp(k, "intermediate_size")) {
            double v = 0; if (j_num(&c, &v) && v > 0) rs->ffn_dim = (uint32_t)v; else j_skip(&c);
        } else if (!strcmp(k, "tie_embeddings")) {
            int b = 0; if (j_bool(&c, &b)) rs->tied_embeddings = b; else j_skip(&c);
        } else if (!strcmp(k, "lm_head")) {
            rs->lm_head_plane = parse_op_object(&c, rs, 1);
        } else if (!strcmp(k, "vocab") && *c.p == '{') {
            c.p++;
            while (*c.p) {
                j_ws(&c);
                if (*c.p == '}') { c.p++; break; }
                char vk[64] = "";
                if (!j_str(&c, vk, sizeof vk)) { j_skip(&c); continue; }
                j_ws(&c); if (*c.p == ':') c.p++;
                j_ws(&c);
                if (!strcmp(vk, "size")) {
                    double v = 0; if (j_num(&c, &v) && v > 0) rs->vocab_size = (uint32_t)v; else j_skip(&c);
                } else if (!strcmp(vk, "hops")) {
                    double v = 0; if (j_num(&c, &v) && v > 0) rs->vocab_hops = (int)v; else j_skip(&c);
                } else if (!strcmp(vk, "fanout")) {
                    double v = 0; if (j_num(&c, &v) && v > 0) rs->vocab_fanout = (int)v; else j_skip(&c);
                } else if (!strcmp(vk, "seeds") && *c.p == '[') {
                    c.p++;
                    while (*c.p) {
                        j_ws(&c);
                        if (*c.p == ']') { c.p++; break; }
                        char sv[96] = "";
                        if (j_str(&c, sv, sizeof sv) && *sv && rs->n_seeds < MAX_MOLD_SEEDS)
                            snprintf(rs->seeds[rs->n_seeds++], 96, "%s", sv);
                        else j_skip(&c);
                        j_ws(&c); if (*c.p == ',') c.p++;
                    }
                } else {
                    j_skip(&c);
                }
                j_ws(&c); if (*c.p == ',') c.p++;
            }
        } else if (!strcmp(k, "layers") && *c.p == '[') {
            c.p++;
            size_t li = 0;
            while (*c.p) {
                j_ws(&c);
                if (*c.p == ']') { c.p++; break; }
                if (*c.p == '{' && li < MAX_MOLD_LAYERS) {
                    c.p++;
                    size_t nkv = 1, nh = 0;
                    size_t ffn_p = 0;
                    int has_ffn = 0;
                    while (*c.p) {
                        j_ws(&c);
                        if (*c.p == '}') { c.p++; break; }
                        char lk[64] = "";
                        if (!j_str(&c, lk, sizeof lk)) { j_skip(&c); continue; }
                        j_ws(&c); if (*c.p == ':') c.p++;
                        j_ws(&c);
                        if (!strcmp(lk, "kv_heads")) {
                            double v = 1; if (j_num(&c, &v) && v >= 1) nkv = (size_t)v; else j_skip(&c);
                        } else if (!strcmp(lk, "ffn")) {
                            ffn_p = parse_op_object(&c, rs, 0);
                            has_ffn = 1;
                        } else if (!strcmp(lk, "heads") && *c.p == '[') {
                            c.p++;
                            while (*c.p) {
                                j_ws(&c);
                                if (*c.p == ']') { c.p++; break; }
                                size_t hp = parse_op_object(&c, rs, 0);
                                if (nh < MAX_MOLD_HEADS) rs->layer_heads[li][nh++] = hp;
                                j_ws(&c); if (*c.p == ',') c.p++;
                            }
                        } else {
                            j_skip(&c);
                        }
                        j_ws(&c); if (*c.p == ',') c.p++;
                    }
                    if (!has_ffn) ffn_p = schedule_add_plane(rs, LP_MOLD_PLANE_UNARY, "unary");
                    rs->layer_nheads[li] = nh;
                    rs->layer_nkv[li] = nkv;
                    rs->layer_ffn[li] = ffn_p;
                    if (li == 0 && nh > 0) {
                        rs->n_heads = (uint32_t)nh;
                        rs->n_kv_heads = (uint32_t)nkv;
                    }
                    li++;
                    rs->has_explicit_layers = 1;
                } else {
                    j_skip(&c);
                }
                j_ws(&c); if (*c.p == ',') c.p++;
            }
            if (li > 0) rs->n_layers = (uint32_t)li;
        } else {
            j_skip(&c);
        }
        j_ws(&c); if (*c.p == ',') c.p++;
    }
}

/* ---- Dynamic sparse edge buffer per substrate plane ---- */
typedef struct {
    int    *rows;
    int    *cols;
    double *weights;
    char  **witnesses;
    lp_id  *claim_ids;
    size_t  n;
    size_t  cap;
    size_t  n_features;
} EdgeBuf;

static void eb_push(EdgeBuf *eb, int r, int c, double w, const lp_id *claim_id, const char *wit){
    if (eb->n == eb->cap) {
        eb->cap = eb->cap ? eb->cap * 2u : 256u;
        eb->rows = xrealloc(eb->rows, sizeof(int) * eb->cap);
        eb->cols = xrealloc(eb->cols, sizeof(int) * eb->cap);
        eb->weights = xrealloc(eb->weights, sizeof(double) * eb->cap);
        eb->witnesses = xrealloc(eb->witnesses, sizeof(char *) * eb->cap);
        eb->claim_ids = xrealloc(eb->claim_ids, sizeof(lp_id) * eb->cap);
    }
    eb->rows[eb->n] = r;
    eb->cols[eb->n] = c;
    eb->weights[eb->n] = w;
    eb->witnesses[eb->n] = wit ? strdup(wit) : NULL;
    if (claim_id) eb->claim_ids[eb->n] = *claim_id;
    else memset(&eb->claim_ids[eb->n], 0, sizeof(lp_id));
    eb->n++;
}

static void eb_free(EdgeBuf *eb){
    if (!eb) return;
    for (size_t i = 0; i < eb->n; i++) free(eb->witnesses[i]);
    free(eb->rows); free(eb->cols); free(eb->weights); free(eb->witnesses); free(eb->claim_ids);
    memset(eb, 0, sizeof *eb);
}

/* Map semantic relation aliases from recipe-schema.md onto canonical substrate predicates (pure BLAKE3). */
static size_t resolve_relation_predicates(lp_text *tx, const char *subtype, lp_id out[8]){
    static const struct { const char *alias; const char *preds[4]; } MAP[] = {
        { "IS_A",           { "hypernym", "instance_hypernym", "IS_A", NULL } },
        { "HYPERNYM",       { "hypernym", "instance_hypernym", NULL, NULL } },
        { "IS_SYNONYM_OF",  { "similar", "also", "IS_SYNONYM_OF", NULL } },
        { "HAS_PART",       { "mero_part", "holo_part", "mero_member", "HAS_PART" } },
        { "HAS_DEFINITION", { "definition of the synset", "English definition", "standard definition of the synset", "HAS_DEFINITION" } },
        { "ANTONYM",        { "antonym", "opposite", "excludes", NULL } },
        { "IS_ANTONYM_OF",  { "antonym", "opposite", "excludes", NULL } }
    };
    if (!subtype || !*subtype || !strcmp(subtype, "*")) return 0;
    size_t n = 0;
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
        if (!strcmp(subtype, MAP[i].alias)) {
            for (int k = 0; k < 4 && MAP[i].preds[k] && n < 8; k++) {
                out[n++] = lp_text_decompose(tx, (const uint8_t *)MAP[i].preds[k], strlen(MAP[i].preds[k]), NULL, NULL).id;
            }
            return n;
        }
    }
    out[0] = lp_text_decompose(tx, (const uint8_t *)subtype, strlen(subtype), NULL, NULL).id;
    return 1;
}

static int is_valid_vocab_entity(const lp_tier0_record *t0, const lp_highway *hw, const lp_id *id){
    if (lp_tier0_codepoint(t0, id) >= 0) return 0;
    if (hw && lp_highway_contains(hw, id)) return 0;
    return 1;
}

static int is_refused_predicate(const Firmware *fw, int has_fw, const lp_id *pred){
    if (!has_fw) return 0;
    for (int i = 0; i < fw->nrefuse_predicate; i++) {
        if (lp_id_eq(pred, &fw->id.refuse[i])) return 1;
    }
    return 0;
}

/* Hilbert-windowed O(n log n + n * W) 4D metric k-NN (angular or unpacked tier0.bin Fréchet) — ZERO n x n matrix! */
typedef struct { size_t idx; uint64_t hkey; } HilbSlot;
static int cmp_hilb_slot(const void *a, const void *b){
    uint64_t x = ((const HilbSlot *)a)->hkey, y = ((const HilbSlot *)b)->hkey;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void build_metric_plane(EdgeBuf *eb, const char *subtype, const lp_tier0_record *t0,
                               const char *const *tokens, const double *coords4d,
                               size_t seeded_count, size_t nv){
    if (nv <= seeded_count + 1u) return;
    size_t nc = nv - seeded_count;
    int use_frechet = (subtype && (!strcmp(subtype, "frechet") || !strcmp(subtype, "hausdorff")));

    HilbSlot *order = malloc(sizeof(HilbSlot) * nc);
    for (size_t i = 0; i < nc; i++) {
        size_t ti = seeded_count + i;
        lp_coord c;
        lp_coord_trunc(coords4d + ti * 4u, &c);
        order[i].idx = ti;
        order[i].hkey = lp_hilbert4(&c);
    }
    qsort(order, nc, sizeof(HilbSlot), cmp_hilb_slot);

    double **curves = NULL;
    size_t *clens = NULL;
    if (use_frechet && t0) {
        curves = calloc(nv, sizeof(double *));
        clens = calloc(nv, sizeof(size_t));
        lp_text *tx = lp_text_new(t0);
        for (size_t i = seeded_count; i < nv; i++) {
            const char *s = tokens[i] ? tokens[i] : "";
            size_t slen = strlen(s), pos = 0, m = 0, cap = 16;
            double *pts = malloc(sizeof(double) * 4u * cap);
            while (pos < slen && m < 64) {
                size_t p0 = pos; uint32_t cp = 0;
                if (!lp_utf8_next((const uint8_t *)s, slen, &pos, &cp)) break;
                if (m == cap) { cap *= 2u; pts = xrealloc(pts, sizeof(double) * 4u * cap); }
                lp_ref cr = lp_text_decompose(tx, (const uint8_t *)(s + p0), pos - p0, NULL, NULL);
                lp_coord_xyzm(&cr.c, pts + m * 4u);
                m++;
            }
            if (m == 0) { memcpy(pts, coords4d + i * 4u, sizeof(double) * 4u); m = 1; }
            curves[i] = pts;
            clens[i] = m;
        }
        lp_text_free(tx);
    }

    const size_t WIN = 24u, TOPK = 6u;
    for (size_t p = 0; p < nc; p++) {
        size_t i = order[p].idx;
        size_t lo = p > WIN ? p - WIN : 0;
        size_t hi = p + WIN + 1u < nc ? p + WIN + 1u : nc;
        int best_j[8]; double best_d[8]; size_t nb = 0;
        for (size_t q = lo; q < hi; q++) {
            if (q == p) continue;
            size_t j = order[q].idx;
            double d = use_frechet && curves
                ? lp_frechet4(curves[i], clens[i], curves[j], clens[j])
                : lp_angle4(coords4d + i * 4u, coords4d + j * 4u);
            if (!isfinite(d) || d < 0.0) continue;
            if (nb < TOPK) {
                best_j[nb] = (int)j; best_d[nb] = d; nb++;
            } else {
                size_t worst = 0;
                for (size_t k = 1; k < nb; k++) if (best_d[k] > best_d[worst]) worst = k;
                if (d < best_d[worst]) { best_j[worst] = (int)j; best_d[worst] = d; }
            }
        }
        for (size_t k = 0; k < nb; k++) {
            double w = 1.0 / (1.0 + best_d[k]);
            eb_push(eb, (int)i, best_j[k], w, NULL, "geometry-4d");
        }
    }

    if (curves) {
        for (size_t i = seeded_count; i < nv; i++) free(curves[i]);
        free(curves); free(clens);
    }
    free(order);
}

int lp_engine_mold_execute(PGconn *pg, const LpEngineMoldRequest *req, LpEngineMoldResponse *out){
    if (!pg) {
        out->status_code = 1;
        snprintf(out->error, sizeof out->error, "mold export requires live PostgreSQL substrate connection");
        return 1;
    }

    const lp_tier0_record *t0 = T0 ? T0 : lp_tier0_map(NULL);
    const lp_highway *hw = lp_highway_map(NULL);
    lp_text *tx = t0 ? lp_text_new(t0) : NULL;
    if (!tx) {
        out->status_code = 1;
        snprintf(out->error, sizeof out->error, "failed to initialize Tier-0 Unicode context");
        return 1;
    }

    /* Resolve recipe JSON (inline or stored substrate entity via recipe_id_prefix) */
    char *loaded_recipe = NULL;
    const char *recipe_src = (req->recipe_json && *req->recipe_json) ? req->recipe_json : NULL;
    if (!recipe_src && req->recipe_id_prefix && *req->recipe_id_prefix) {
        lp_id rid = { { 0 } };
        if (strlen(req->recipe_id_prefix) == 32 && lp_id_unhex(req->recipe_id_prefix, &rid)) {
            Reader *rrd = reader_new(pg);
            if (rrd) {
                reader_want(rrd, &rid);
                loaded_recipe = reader_text(rrd, &rid, 65536);
                reader_free(rrd);
                if (loaded_recipe && *loaded_recipe == '{') recipe_src = loaded_recipe;
            }
        }
        if (!recipe_src) {
            free(loaded_recipe);
            lp_text_free(tx);
            out->status_code = 2;
            snprintf(out->error, sizeof out->error, "stored recipe_id_prefix '%s' not found in substrate", req->recipe_id_prefix);
            return 2;
        }
    }

    RecipeSchedule rs;
    parse_mold_recipe(recipe_src, &rs);

    uint32_t max_vocab = req->max_vocab ? req->max_vocab : (rs.vocab_size ? rs.vocab_size : 1024u);
    if (max_vocab < 16u) max_vocab = 16u;
    uint32_t n_layers = req->n_layers ? req->n_layers : (rs.n_layers ? rs.n_layers : 4u);
    uint32_t n_heads = req->n_heads ? req->n_heads : (rs.n_heads ? rs.n_heads : 4u);
    uint32_t n_kv_heads = req->n_kv_heads ? req->n_kv_heads : (rs.n_kv_heads ? rs.n_kv_heads : n_heads);
    uint32_t hidden_dim = req->hidden_dim >= 16u ? req->hidden_dim : (rs.hidden_dim >= 16u ? rs.hidden_dim : 0u);
    int auto_hidden = (hidden_dim == 0u);
    if (auto_hidden) hidden_dim = 128u;
    if ((n_heads % n_kv_heads) != 0) {
        free(loaded_recipe); lp_text_free(tx);
        out->status_code = 2;
        snprintf(out->error, sizeof out->error, "n_heads (%u) must be divisible by n_kv_heads (%u)", n_heads, n_kv_heads);
        return 2;
    }
    if ((hidden_dim % n_heads) != 0 || ((hidden_dim / n_heads) % 2u) != 0) {
        free(loaded_recipe); lp_text_free(tx);
        out->status_code = 2;
        snprintf(out->error, sizeof out->error, "hidden_dim (%u) must be divisible by 2 * n_heads (%u)", hidden_dim, n_heads);
        return 2;
    }
    uint32_t ffn_dim = req->ffn_dim ? req->ffn_dim : (rs.ffn_dim ? rs.ffn_dim : (hidden_dim * 4u));
    int tied_emb = req->tied_embeddings ? 1 : rs.tied_embeddings;

    /* Default multi-plane schedule when no explicit layers[] were passed (Export.md §25.3). */
    if (!rs.has_explicit_layers) {
        size_t p_traj = schedule_add_plane(&rs, LP_MOLD_PLANE_TRAJECTORY, "trajectory");
        size_t p_ang  = schedule_add_plane(&rs, LP_MOLD_PLANE_METRIC, "angular");
        size_t p_syn  = schedule_add_plane(&rs, LP_MOLD_PLANE_RELATION, "IS_SYNONYM_OF");
        size_t p_isa  = schedule_add_plane(&rs, LP_MOLD_PLANE_RELATION, "IS_A");
        size_t p_part = schedule_add_plane(&rs, LP_MOLD_PLANE_RELATION, "HAS_PART");
        size_t p_def  = schedule_add_plane(&rs, LP_MOLD_PLANE_RELATION, "HAS_DEFINITION");
        size_t p_rel  = schedule_add_plane(&rs, LP_MOLD_PLANE_RELATION, "*");
        size_t p_uni  = schedule_add_plane(&rs, LP_MOLD_PLANE_UNARY, "unary");
        size_t p_frec = schedule_add_plane(&rs, LP_MOLD_PLANE_METRIC, "frechet");
        for (uint32_t L = 0; L < n_layers && L < MAX_MOLD_LAYERS; L++) {
            rs.layer_nheads[L] = n_heads;
            rs.layer_nkv[L] = n_kv_heads;
            for (uint32_t h = 0; h < n_heads && h < MAX_MOLD_HEADS; h++) {
                if (L == 0) {
                    size_t early[4] = { p_traj, p_ang, p_syn, p_isa };
                    rs.layer_heads[L][h] = early[h % 4];
                } else if (L + 1u == n_layers) {
                    size_t late[4] = { p_traj, p_rel, p_frec, p_def };
                    rs.layer_heads[L][h] = late[h % 4];
                } else {
                    size_t mid[4] = { p_isa, p_part, p_def, p_traj };
                    rs.layer_heads[L][h] = mid[h % 4];
                }
            }
            rs.layer_ffn[L] = (L + 1u == n_layers) ? p_traj : p_uni;
        }
        rs.lm_head_plane = p_traj;
    }

    Firmware mold_fw;
    memset(&mold_fw, 0, sizeof mold_fw);
    char dummy_err[256] = "";
    int has_fw = (firmware_load_checked(NULL, FW_PULL, &mold_fw, dummy_err, sizeof dummy_err) == 0 &&
                  firmware_ids_checked(&mold_fw, dummy_err, sizeof dummy_err) == 0);
    if (has_fw) refuse_named(&mold_fw);

    const lp_id *wscope = (req->authority && req->authority->scoped) ? req->authority->witnesses : NULL;
    int nwscope = (req->authority && req->authority->scoped) ? (int)req->authority->nwitnesses : 0;
    int scoped = (req->authority && req->authority->scoped) ? 1 : 0;

    /* 1. Seed Open Merkle Vocabulary (special tokens + byte fallbacks + canonical content seeds). */
    lp_idmap *vmap = lp_idmap_new();
    size_t seeded_count = (max_vocab >= 320u) ? 259u : 3u;
    char **seeded_strs = calloc(seeded_count, sizeof(char *));
    for (size_t si = 0; si < seeded_count; si++) {
        char buf[16];
        if (si == 0) snprintf(buf, sizeof buf, "<unk>");
        else if (si == 1) snprintf(buf, sizeof buf, "<s>");
        else if (si == 2) snprintf(buf, sizeof buf, "</s>");
        else snprintf(buf, sizeof buf, "<0x%02X>", (unsigned)(si - 3u));
        seeded_strs[si] = strdup(buf);
        lp_ref sr = lp_text_decompose(tx, (const uint8_t *)buf, strlen(buf), NULL, NULL);
        lp_idmap_put(vmap, &sr.id, NULL);
    }

    static const char *const DEFAULT_SEEDS[] = {
        "king", "queen", "monarch", "water", "light", "earth", "time", "science",
        "music", "city", "language", "number", "animal", "plant", "person", "energy",
        "history", "France", "Paris", "photosynthesis", "dog", "cat", "tree", "sun",
        "book", "work", "life", "world", "system", "state", "force", "mind"
    };
    size_t n_seed_in = rs.n_seeds > 0 ? rs.n_seeds : (sizeof DEFAULT_SEEDS / sizeof DEFAULT_SEEDS[0]);
    for (size_t i = 0; i < n_seed_in && lp_idmap_count(vmap) < max_vocab; i++) {
        const char *sw = rs.n_seeds > 0 ? rs.seeds[i] : DEFAULT_SEEDS[i];
        lp_ref r = lp_text_decompose(tx, (const uint8_t *)sw, strlen(sw), NULL, NULL);
        lp_idmap_put(vmap, &r.id, NULL);
    }

    /* 2. Allocate one EdgeBuf per declared substrate plane. */
    EdgeBuf *ebufs = calloc(rs.n_planes ? rs.n_planes : 1u, sizeof(EdgeBuf));
    lp_idmap *unary_feats = lp_idmap_new();
    lp_idmap *touched_claims = lp_idmap_new();

    int need_traj = 0, need_unary = 0;
    size_t traj_plane_idx = 0, unary_plane_idx = 0;
    for (size_t p = 0; p < rs.n_planes; p++) {
        if (rs.planes[p].kind == LP_MOLD_PLANE_TRAJECTORY) { need_traj = 1; traj_plane_idx = p; }
        if (rs.planes[p].kind == LP_MOLD_PLANE_UNARY) { need_unary = 1; unary_plane_idx = p; }
    }

    /* 3. Multi-hop O(log N) + O(k) GIN expansion across declared channels (trajectories + typed relations). */
    int hops = rs.vocab_hops > 0 ? (rs.vocab_hops > 3 ? 3 : rs.vocab_hops) : 2;
    int fan = rs.vocab_fanout > 0 ? rs.vocab_fanout : 64;
    if (fan > 256) fan = 256;
    size_t expanded_upto = seeded_count;

    /* Precompute lexical-sense / synset highway bridge predicates from firmware for 2-hop concept relation resolution. */
    Lexicalization mold_lx;
    memset(&mold_lx, 0, sizeof mold_lx);
    int has_lx = has_fw && mold_fw.lexical[0][0] && lexicalization_of(tx, &mold_fw, &mold_lx);
    lp_id synset_rel_id = lp_text_decompose(tx, (const uint8_t *)"reference", 9, NULL, NULL).id;
    lp_id synset_rel_id2 = lp_text_decompose(tx, (const uint8_t *)"synset", 6, NULL, NULL).id;
    lp_id sense_rel_id = has_lx ? mold_lx.sense : lp_text_decompose(tx, (const uint8_t *)"lexical sense", 13, NULL, NULL).id;

    for (int hop = 0; hop < hops; hop++) {
        size_t cur_nv = lp_idmap_count(vmap);
        if (cur_nv <= expanded_upto) break;
        size_t batch_n = cur_nv - expanded_upto;
        if (batch_n > 192u) batch_n = 192u;
        const lp_id *all_keys = lp_idmap_keys(vmap);
        lp_id *bkeys = malloc(sizeof(lp_id) * batch_n);
        memcpy(bkeys, all_keys + expanded_upto, sizeof(lp_id) * batch_n);
        size_t base_vidx = expanded_upto;
        expanded_upto += batch_n;

        /* Pre-resolve word -> synset bridges once per hop when any typed RELATION plane is present. */
        int any_typed_rel = 0;
        for (size_t p = 0; p < rs.n_planes; p++) {
            if (rs.planes[p].kind == LP_MOLD_PLANE_RELATION && rs.planes[p].subtype[0] && strcmp(rs.planes[p].subtype, "*") != 0)
                any_typed_rel = 1;
        }
        lp_id *syn_keys = NULL;
        int *syn_src_vidx = NULL;
        size_t n_syn = 0;
        if (any_typed_rel) {
            int ns1 = 0;
            Hold *hs1 = holds_both_scoped(pg, bkeys, (int)batch_n, &sense_rel_id, wscope, nwscope, scoped, &ns1);
            lp_id *sense_ids = malloc(sizeof(lp_id) * (size_t)(ns1 ? ns1 : 1));
            int *sense_vidx = malloc(sizeof(int) * (size_t)(ns1 ? ns1 : 1));
            size_t n_senses = 0;
            Ids pts_s = { 0 };
            for (int r = 0; r < ns1 && n_senses < 512u; r++) {
                if (!hs1[r].claim || !hs1[r].stood) continue;
                int s = hs1[r].src;
                if (s < 0 || (size_t)s >= batch_n) continue;
                path_into(&pts_s, lp_path_of(hs1[r].path, (size_t)hs1[r].path_len));
                if (pts_s.n < 3) continue;
                int o = lp_tuple_other(pts_s.v, pts_s.n, &bkeys[s]);
                if (o < 0) continue;
                sense_ids[n_senses] = pts_s.v[o];
                sense_vidx[n_senses] = (int)(base_vidx + (size_t)s);
                n_senses++;
            }
            holds_free(hs1, ns1);
            if (n_senses > 0) {
                /* Direct canonical path: Sense = [lemma, language, synset/ILI],
                 * so its last constituent IS the synset (i* or unmapped oewn-*).
                 * Keep legacy [sense, reference, synset] fallback if present. */
                int nps0 = 0;
                Hold *sp0 = paths_of(pg, sense_ids, (int)n_senses, &nps0);
                int ns2 = 0;
                Hold *hs2 = holds_both_scoped(pg, sense_ids, (int)n_senses, &synset_rel_id, wscope, nwscope, scoped, &ns2);
                if (ns2 == 0) {
                    holds_free(hs2, ns2);
                    hs2 = holds_both_scoped(pg, sense_ids, (int)n_senses, &synset_rel_id2, wscope, nwscope, scoped, &ns2);
                }
                size_t cap_syn = (size_t)(nps0 + ns2);
                syn_keys = malloc(sizeof(lp_id) * (cap_syn ? cap_syn : 1));
                syn_src_vidx = malloc(sizeof(int) * (cap_syn ? cap_syn : 1));
                lp_idmap *syn_seen = lp_idmap_new();
                for (int r = 0; r < nps0 && n_syn < 512u; r++) {
                    int s = sp0[r].src;
                    if (s < 0 || (size_t)s >= n_senses) continue;
                    path_into(&pts_s, lp_path_of(sp0[r].path, (size_t)sp0[r].path_len));
                    if (pts_s.n < 3) continue;
                    const lp_id *cid = &pts_s.v[pts_s.n - 1];
                    bool fresh = false;
                    lp_idmap_put(syn_seen, cid, &fresh);
                    if (fresh) {
                        syn_keys[n_syn] = *cid;
                        syn_src_vidx[n_syn] = sense_vidx[s];
                        n_syn++;
                    }
                }
                holds_free(sp0, nps0);
                for (int r = 0; r < ns2 && n_syn < 512u; r++) {
                    if (!hs2[r].claim || !hs2[r].stood) continue;
                    int s = hs2[r].src;
                    if (s < 0 || (size_t)s >= n_senses) continue;
                    path_into(&pts_s, lp_path_of(hs2[r].path, (size_t)hs2[r].path_len));
                    if (pts_s.n < 3) continue;
                    int o = lp_tuple_other(pts_s.v, pts_s.n, &sense_ids[s]);
                    if (o < 0) continue;
                    bool fresh = false;
                    lp_idmap_put(syn_seen, &pts_s.v[o], &fresh);
                    if (fresh) {
                        syn_keys[n_syn] = pts_s.v[o];
                        syn_src_vidx[n_syn] = sense_vidx[s];
                        n_syn++;
                    }
                }
                lp_idmap_free(syn_seen);
                holds_free(hs2, ns2);
            }
            lp_vec_free(&pts_s);
            free(sense_ids); free(sense_vidx);
        }

        /* 3a. Query physicality.path observations/usages/examples (GIN kinds=2) for precedes/contains/co-occurs. */
        if (need_traj) {
            int *hub = calloc(batch_n * 2u, sizeof(int));
            int nh = 0;
            Hold *h = holds_capped_scoped(pg, bkeys, (int)batch_n, fan, 0, 2, wscope, nwscope, scoped, hub, &nh);
            Ids pts = { 0 };
            for (int r = 0; r < nh; r++) {
                if (h[r].claim || lp_is_internal_metadata_path(tx, h[r].path, (size_t)h[r].path_len)) continue;
                lp_path pp = lp_path_of(h[r].path, (size_t)h[r].path_len);
                if (pp.n < 2u || pp.n > 512u) continue;

                /* Filter out record/claim/metadata packaging paths (LP_SAID_*) and refused predicates/highway IDs,
                 * keeping pure natural-language/usage trajectories (handles gap-1 space separators naturally). */
                int has_schema = 0;
                for (size_t k = 0; k < pp.n && !has_schema; k++) {
                    uint32_t said = lp_path_said(pp, k);
                    if (said == LP_SAID_CLAIM || said == LP_SAID_TUPLE || said == LP_SAID_METADATA || said == LP_SAID_RECORD) {
                        has_schema = 1;
                        break;
                    }
                    lp_id vid = lp_path_id(pp, k);
                    if (is_refused_predicate(&mold_fw, has_fw, &vid)) has_schema = 1;
                    if (hw && lp_highway_contains(hw, &vid)) has_schema = 1;
                }
                if (has_schema) continue;
                size_t nv_p = lp_path_ids(h[r].path, (size_t)h[r].path_len, NULL, 0);
                if (nv_p < 2u || nv_p > 512u) continue;
                lp_vec_reserve(&pts, nv_p);
                nv_p = lp_path_ids(h[r].path, (size_t)h[r].path_len, pts.v, nv_p);
                lp_id words[128];
                size_t nw = 0;
                for (size_t k = 0; k < nv_p && nw < 128u; k++) {
                    if (is_valid_vocab_entity(t0, hw, &pts.v[k])) words[nw++] = pts.v[k];
                }
                for (size_t k = 0; k + 1u < nw; k++) {
                    if (lp_id_eq(&words[k], &words[k + 1u])) continue;
                    int64_t i0 = lp_idmap_find(vmap, &words[k]);
                    if (i0 < 0 && lp_idmap_count(vmap) < max_vocab) i0 = (int64_t)lp_idmap_put(vmap, &words[k], NULL);
                    int64_t i1 = lp_idmap_find(vmap, &words[k + 1u]);
                    if (i1 < 0 && lp_idmap_count(vmap) < max_vocab) i1 = (int64_t)lp_idmap_put(vmap, &words[k + 1u], NULL);
                    if (i0 >= 0 && i1 >= 0) {
                        eb_push(&ebufs[traj_plane_idx], (int)i0, (int)i1, 1.0, &h[r].entity, NULL);
                        if (k + 2u < nw && !lp_id_eq(&words[k], &words[k + 2u])) {
                            int64_t i2 = lp_idmap_find(vmap, &words[k + 2u]);
                            if (i2 >= 0) eb_push(&ebufs[traj_plane_idx], (int)i0, (int)i2, 0.35, &h[r].entity, NULL);
                        }
                    }
                }
            }
            lp_vec_free(&pts);
            holds_free(h, nh);
            free(hub);

            /* Direct GIN suffix continuations via LP_SQL_FORWARD_SUFFIX(_SCOPED) */
            Args fa = { 0 };
            arg_ids(&fa, bkeys, batch_n);
            arg_i64(&fa, (int64_t)fan);
            if (scoped) arg_ids(&fa, wscope, (size_t)nwscope);
            PGresult *fq = db_call_try(pg, scoped ? LP_SQL_FORWARD_SUFFIX_SCOPED : LP_SQL_FORWARD_SUFFIX, &fa, NULL, 0);
            if (fq) {
                int fnr = PQntuples(fq);
                for (int r = 0; r < fnr; r++) {
                    int idx_j = col_i32(fq, r, 1) - 1;
                    const lp_id *nxt = col_id(fq, r, 2);
                    int64_t times = col_i64(fq, r, 3);
                    if (idx_j < 0 || (size_t)idx_j >= batch_n || times <= 0 || !is_valid_vocab_entity(t0, hw, nxt)) continue;
                    int64_t i0 = (int64_t)(base_vidx + (size_t)idx_j);
                    int64_t i1 = lp_idmap_find(vmap, nxt);
                    if (i1 < 0 && lp_idmap_count(vmap) < max_vocab) i1 = (int64_t)lp_idmap_put(vmap, nxt, NULL);
                    if (i1 >= 0 && i0 != i1) {
                        eb_push(&ebufs[traj_plane_idx], (int)i0, (int)i1, log1p((double)times), NULL, "trajectory");
                    }
                }
                PQclear(fq);
            }
            args_free(&fa);
        }

        /* 3b. Query each declared RELATION plane using 2-key GIN holds_both_scoped (or general holds_capped_scoped). */
        for (size_t p = 0; p < rs.n_planes; p++) {
            if (rs.planes[p].kind != LP_MOLD_PLANE_RELATION) continue;
            lp_id preds[8];
            size_t npreds = resolve_relation_predicates(tx, rs.planes[p].subtype, preds);
            Ids pts = { 0 };
            if (npreds > 0) {
                for (size_t pi = 0; pi < npreds; pi++) {
                    int nh = 0;
                    Hold *h = holds_both_scoped(pg, bkeys, (int)batch_n, &preds[pi], wscope, nwscope, scoped, &nh);
                    for (int r = 0; r < nh; r++) {
                        if (!h[r].claim || !h[r].stood) continue;
                        int s = h[r].src;
                        if (s < 0 || (size_t)s >= batch_n) continue;
                        path_into(&pts, lp_path_of(h[r].path, (size_t)h[r].path_len));
                        if (pts.n < 3) continue;
                        int other = lp_tuple_other(pts.v, pts.n, &bkeys[s]);
                        if (other < 0 || lp_id_eq(&pts.v[other], &bkeys[s])) continue;
                        if (!is_valid_vocab_entity(t0, hw, &pts.v[other])) continue;

                        double conf = lp_confidence(&h[r].r, 2.0);
                        if (!(conf > 0.0)) conf = 0.25;
                        double w = conf * (1.0 + fabs(h[r].r.rating - 1500.0) / 400.0);
                        if (h[r].r.rating < 1500.0 || rs.planes[p].is_inhibitory) w = -w;

                        int64_t i0 = (int64_t)(base_vidx + (size_t)s);
                        int64_t i1 = lp_idmap_find(vmap, &pts.v[other]);
                        if (i1 < 0 && lp_idmap_count(vmap) < max_vocab) i1 = (int64_t)lp_idmap_put(vmap, &pts.v[other], NULL);
                        if (i1 >= 0 && i0 != i1) {
                            eb_push(&ebufs[p], (int)i0, (int)i1, w, &h[r].entity, NULL);
                            lp_idmap_put(touched_claims, &h[r].entity, NULL);
                        }
                    }
                    holds_free(h, nh);

                    /* 2-hop concept/synset bridge: word -> sense -> synset --[pred]--> target synset/gloss -> lemma/words. */
                    if (n_syn > 0) {
                        int nsh = 0;
                        Hold *sh = holds_both_scoped(pg, syn_keys, (int)n_syn, &preds[pi], wscope, nwscope, scoped, &nsh);
                        lp_id *tgt_syns = malloc(sizeof(lp_id) * (size_t)(nsh ? nsh : 1));
                        int *tgt_src = malloc(sizeof(int) * (size_t)(nsh ? nsh : 1));
                        double *tgt_w = malloc(sizeof(double) * (size_t)(nsh ? nsh : 1));
                        lp_id *tgt_cl = malloc(sizeof(lp_id) * (size_t)(nsh ? nsh : 1));
                        size_t n_tsyn = 0;
                        for (int r = 0; r < nsh; r++) {
                            if (!sh[r].claim || !sh[r].stood) continue;
                            int s = sh[r].src;
                            if (s < 0 || (size_t)s >= n_syn) continue;
                            path_into(&pts, lp_path_of(sh[r].path, (size_t)sh[r].path_len));
                            if (pts.n < 3) continue;
                            /* Preserve direction on asymmetric relations like hypernym: [child_syn, hypernym, parent_syn] */
                            if (!lp_id_eq(&pts.v[0], &syn_keys[s])) continue;
                            const lp_id *tgt = &pts.v[pts.n - 1];
                            if (lp_id_eq(tgt, &syn_keys[s])) continue;
                            double conf = lp_confidence(&sh[r].r, 2.0);
                            if (!(conf > 0.0)) conf = 0.25;
                            double w = conf * (1.0 + fabs(sh[r].r.rating - 1500.0) / 400.0);
                            if (sh[r].r.rating < 1500.0 || rs.planes[p].is_inhibitory) w = -w;

                            if (hw && lp_highway_contains(hw, tgt)) {
                                if (n_tsyn < 512u) {
                                    tgt_syns[n_tsyn] = *tgt;
                                    tgt_src[n_tsyn] = syn_src_vidx[s];
                                    tgt_w[n_tsyn] = w;
                                    tgt_cl[n_tsyn] = sh[r].entity;
                                    n_tsyn++;
                                }
                            } else if (is_valid_vocab_entity(t0, hw, tgt)) {
                                /* Direct text target (e.g. HAS_DEFINITION gloss) */
                                int i0 = syn_src_vidx[s];
                                int64_t i1 = lp_idmap_find(vmap, tgt);
                                if (i1 < 0 && lp_idmap_count(vmap) < max_vocab) i1 = (int64_t)lp_idmap_put(vmap, tgt, NULL);
                                if (i1 >= 0 && i0 != (int)i1) {
                                    eb_push(&ebufs[p], i0, (int)i1, w, &sh[r].entity, NULL);
                                    lp_idmap_put(touched_claims, &sh[r].entity, NULL);
                                }
                            }
                        }
                        holds_free(sh, nsh);

                        if (n_tsyn > 0) {
                            /* Resolve target synsets directly to lexicalization tuples [lemma, lang, synset] -> lemma
                             * (with fallback to legacy [sense, reference, synset] bridges). */
                            int nhb = 0;
                            Hold *hb = holds_any(pg, tgt_syns, (int)n_tsyn, &nhb);
                            for (int r = 0; r < nhb; r++) {
                                int ts = hb[r].src;
                                if (ts < 0 || (size_t)ts >= n_tsyn) continue;
                                path_into(&pts, lp_path_of(hb[r].path, (size_t)hb[r].path_len));
                                if (pts.n != 3 || !lp_id_eq(&pts.v[2], &tgt_syns[ts])) continue;
                                const lp_id *lemma = &pts.v[0];
                                if (!is_valid_vocab_entity(t0, hw, lemma)) continue;
                                int i0 = tgt_src[ts];
                                int64_t i1 = lp_idmap_find(vmap, lemma);
                                if (i1 < 0 && lp_idmap_count(vmap) < max_vocab) i1 = (int64_t)lp_idmap_put(vmap, lemma, NULL);
                                if (i1 >= 0 && i0 != (int)i1) {
                                    eb_push(&ebufs[p], i0, (int)i1, tgt_w[ts], &tgt_cl[ts], NULL);
                                    lp_idmap_put(touched_claims, &tgt_cl[ts], NULL);
                                }
                            }
                            holds_free(hb, nhb);
                            int nbk = 0;
                            Hold *bk = holds_both_scoped(pg, tgt_syns, (int)n_tsyn, &synset_rel_id, wscope, nwscope, scoped, &nbk);
                            if (nbk == 0) {
                                holds_free(bk, nbk);
                                bk = holds_both_scoped(pg, tgt_syns, (int)n_tsyn, &synset_rel_id2, wscope, nwscope, scoped, &nbk);
                            }
                            lp_id *bk_senses = malloc(sizeof(lp_id) * (size_t)(nbk ? nbk : 1));
                            int *bk_idx = malloc(sizeof(int) * (size_t)(nbk ? nbk : 1));
                            size_t n_bks = 0;
                            for (int r = 0; r < nbk && n_bks < 1024u; r++) {
                                if (!bk[r].claim || !bk[r].stood) continue;
                                int s = bk[r].src;
                                if (s < 0 || (size_t)s >= n_tsyn) continue;
                                path_into(&pts, lp_path_of(bk[r].path, (size_t)bk[r].path_len));
                                if (pts.n < 3) continue;
                                int o = lp_tuple_other(pts.v, pts.n, &tgt_syns[s]);
                                if (o < 0) continue;
                                bk_senses[n_bks] = pts.v[o];
                                bk_idx[n_bks] = s;
                                n_bks++;
                            }
                            holds_free(bk, nbk);
                            if (n_bks > 0) {
                                int nps = 0;
                                Hold *sp = paths_of(pg, bk_senses, (int)n_bks, &nps);
                                for (int r = 0; r < nps; r++) {
                                    int s = sp[r].src;
                                    if (s < 0 || (size_t)s >= n_bks) continue;
                                    path_into(&pts, lp_path_of(sp[r].path, (size_t)sp[r].path_len));
                                    if (pts.n < 2) continue;
                                    const lp_id *lemma = &pts.v[0];
                                    if (!is_valid_vocab_entity(t0, hw, lemma)) continue;
                                    int ts = bk_idx[s];
                                    int i0 = tgt_src[ts];
                                    int64_t i1 = lp_idmap_find(vmap, lemma);
                                    if (i1 < 0 && lp_idmap_count(vmap) < max_vocab) i1 = (int64_t)lp_idmap_put(vmap, lemma, NULL);
                                    if (i1 >= 0 && i0 != (int)i1) {
                                        eb_push(&ebufs[p], i0, (int)i1, tgt_w[ts], &tgt_cl[ts], NULL);
                                        lp_idmap_put(touched_claims, &tgt_cl[ts], NULL);
                                    }
                                }
                                holds_free(sp, nps);
                            }
                            free(bk_senses); free(bk_idx);
                        }
                        free(tgt_syns); free(tgt_src); free(tgt_w); free(tgt_cl);
                    }
                }
            } else {
                int *hub = calloc(batch_n * 2u, sizeof(int));
                int nh = 0;
                Hold *h = holds_capped_scoped(pg, bkeys, (int)batch_n, fan, 0, 1, wscope, nwscope, scoped, hub, &nh);
                for (int r = 0; r < nh; r++) {
                    if (!h[r].claim || !h[r].stood) continue;
                    int s = h[r].src;
                    if (s < 0 || (size_t)s >= batch_n) continue;
                    if (lp_is_internal_metadata_path(tx, h[r].path, (size_t)h[r].path_len)) continue;
                    path_into(&pts, lp_path_of(h[r].path, (size_t)h[r].path_len));
                    if (pts.n < 2) continue;
                    if (pts.n >= 3 && is_refused_predicate(&mold_fw, has_fw, &pts.v[1])) continue;
                    int other = lp_tuple_other(pts.v, pts.n, &bkeys[s]);
                    if (other < 0 || lp_id_eq(&pts.v[other], &bkeys[s])) continue;

                    double conf = lp_confidence(&h[r].r, 2.0);
                    if (!(conf > 0.0)) conf = 0.25;
                    double w = conf * (1.0 + fabs(h[r].r.rating - 1500.0) / 400.0);
                    if (h[r].r.rating < 1500.0) w = -w;

                    int64_t i0 = (int64_t)(base_vidx + (size_t)s);
                    if (pts.n == 2 || (hw && lp_highway_bank_of(hw, &pts.v[other], NULL))) {
                        if (need_unary) {
                            size_t fidx = lp_idmap_put(unary_feats, &pts.v[other], NULL);
                            eb_push(&ebufs[unary_plane_idx], (int)i0, (int)fidx, w, &h[r].entity, NULL);
                            lp_idmap_put(touched_claims, &h[r].entity, NULL);
                        }
                        continue;
                    }
                    if (!is_valid_vocab_entity(t0, hw, &pts.v[other])) continue;
                    int64_t i1 = lp_idmap_find(vmap, &pts.v[other]);
                    if (i1 < 0 && lp_idmap_count(vmap) < max_vocab) i1 = (int64_t)lp_idmap_put(vmap, &pts.v[other], NULL);
                    if (i1 >= 0 && i0 != i1) {
                        eb_push(&ebufs[p], (int)i0, (int)i1, w, &h[r].entity, NULL);
                        lp_idmap_put(touched_claims, &h[r].entity, NULL);
                    }
                }
                holds_free(h, nh);
                free(hub);
            }
            lp_vec_free(&pts);
        }

        /* 3c. Dedicated UNARY pass if no wildcard RELATION plane already populated unary_feats. */
        if (need_unary && ebufs[unary_plane_idx].n == 0) {
            int *hub = calloc(batch_n * 2u, sizeof(int));
            int nh = 0;
            Hold *h = holds_capped_scoped(pg, bkeys, (int)batch_n, fan, 0, 1, wscope, nwscope, scoped, hub, &nh);
            Ids pts = { 0 };
            for (int r = 0; r < nh; r++) {
                if (!h[r].claim || !h[r].stood) continue;
                int s = h[r].src;
                if (s < 0 || (size_t)s >= batch_n) continue;
                path_into(&pts, lp_path_of(h[r].path, (size_t)h[r].path_len));
                if (pts.n < 2) continue;
                int other = lp_tuple_other(pts.v, pts.n, &bkeys[s]);
                if (other < 0 || lp_id_eq(&pts.v[other], &bkeys[s])) continue;
                double conf = lp_confidence(&h[r].r, 2.0);
                if (!(conf > 0.0)) conf = 0.25;
                double w = (h[r].r.rating < 1500.0 ? -conf : conf);
                size_t fidx = lp_idmap_put(unary_feats, &pts.v[other], NULL);
                eb_push(&ebufs[unary_plane_idx], (int)(base_vidx + (size_t)s), (int)fidx, w, &h[r].entity, NULL);
                lp_idmap_put(touched_claims, &h[r].entity, NULL);
            }
            lp_vec_free(&pts);
            holds_free(h, nh);
            free(hub);
        }

        free(syn_keys); free(syn_src_vidx);
        free(bkeys);
    }

    size_t nv = lp_idmap_count(vmap);
    if (nv <= seeded_count) {
        for (size_t p = 0; p < rs.n_planes; p++) eb_free(&ebufs[p]);
        free(ebufs); lp_idmap_free(unary_feats); lp_idmap_free(touched_claims);
        for (size_t si = 0; si < seeded_count; si++) free(seeded_strs[si]);
        free(seeded_strs); lp_idmap_free(vmap); free(loaded_recipe); lp_text_free(tx);
        out->status_code = 1;
        snprintf(out->error, sizeof out->error, "mold export found zero vocabulary entities in scope");
        return 1;
    }

    /* 4. Batched realization of token surface strings + canonical 4D entity.coord anchors. */
    lp_id *token_ids = malloc(sizeof(lp_id) * nv);
    char **tokens = calloc(nv, sizeof(char *));
    memcpy(token_ids, lp_idmap_keys(vmap), sizeof(lp_id) * nv);
    for (size_t i = 0; i < seeded_count && i < nv; i++) tokens[i] = strdup(seeded_strs[i]);
    Reader *rd = reader_new(pg);
    if (rd) {
        if (nv > seeded_count) reader_prefetch(rd, token_ids + seeded_count, nv - seeded_count, 96);
        for (size_t i = seeded_count; i < nv; i++) tokens[i] = reader_text(rd, &token_ids[i], 96);
        reader_free(rd);
    } else {
        for (size_t i = seeded_count; i < nv; i++) { tokens[i] = malloc(33); lp_id_hex(&token_ids[i], tokens[i]); }
    }

    double *coords4d = calloc(nv * 4u, sizeof(double));
    uint8_t *has_c = calloc(nv, 1);
    coords_of(pg, token_ids, (int)nv, coords4d, has_c);
    for (size_t i = 0; i < nv; i++) {
        if (!has_c[i]) {
            const char *ts = tokens[i] ? tokens[i] : "";
            lp_ref tr = lp_text_decompose(tx, (const uint8_t *)ts, strlen(ts), NULL, NULL);
            lp_coord_xyzm(&tr.c, coords4d + i * 4u);
        }
    }
    free(has_c);

    /* 5. Build any declared 4D METRIC planes (angular / frechet) in O(n log n + n*W). */
    for (size_t p = 0; p < rs.n_planes; p++) {
        if (rs.planes[p].kind == LP_MOLD_PLANE_METRIC) {
            build_metric_plane(&ebufs[p], rs.planes[p].subtype, t0, (const char *const *)tokens, coords4d, seeded_count, nv);
        }
        if (rs.planes[p].kind == LP_MOLD_PLANE_UNARY) {
            ebufs[p].n_features = lp_idmap_count(unary_feats);
        }
    }

    /* 6. Resolve real witness trunk IDs for touched claims via batched attested() for exact Fisher share_bp. */
    size_t n_tc = lp_idmap_count(touched_claims);
    if (n_tc > 0) {
        size_t qn = n_tc > 4096u ? 4096u : n_tc;
        int natt = 0;
        Attested *att = attested(pg, lp_idmap_keys(touched_claims), (int)qn, &natt, NULL);
        if (att && natt > 0) {
            lp_idmap *c2w = lp_idmap_sized(sizeof(lp_id));
            for (int i = 0; i < natt; i++) {
                if (!att[i].witness) continue;
                bool fresh = false;
                size_t slot = lp_idmap_put(c2w, &att[i].claim, &fresh);
                if (fresh) *(lp_id *)lp_idmap_at(c2w, slot) = att[i].trunk;
            }
            static const lp_id zero_id = { { 0 } };
            for (size_t p = 0; p < rs.n_planes; p++) {
                for (size_t e = 0; e < ebufs[p].n; e++) {
                    if (ebufs[p].witnesses[e] || lp_id_eq(&ebufs[p].claim_ids[e], &zero_id)) continue;
                    int64_t slot = lp_idmap_find(c2w, &ebufs[p].claim_ids[e]);
                    if (slot >= 0) {
                        char hex[33];
                        lp_id_hex((const lp_id *)lp_idmap_at(c2w, (size_t)slot), hex);
                        ebufs[p].witnesses[e] = strdup(hex);
                    }
                }
            }
            lp_idmap_free(c2w);
        }
        free(att);
    }

    size_t total_edges = 0;
    for (size_t p = 0; p < rs.n_planes; p++) total_edges += ebufs[p].n;
    if (total_edges == 0) {
        for (size_t p = 0; p < rs.n_planes; p++) eb_free(&ebufs[p]);
        free(ebufs); lp_idmap_free(unary_feats); lp_idmap_free(touched_claims);
        for (size_t i = 0; i < nv; i++) free(tokens[i]);
        free(tokens); free(token_ids); free(coords4d);
        for (size_t si = 0; si < seeded_count; si++) free(seeded_strs[si]);
        free(seeded_strs); lp_idmap_free(vmap); free(loaded_recipe); lp_text_free(tx);
        out->status_code = 1;
        snprintf(out->error, sizeof out->error, "mold export found zero substrate edges across %zu planes in scope", rs.n_planes);
        return 1;
    }

    /* 7. Assemble LpMoldPlane[] and LpMoldLayerSpec[] and call lp_mold_export(). */
    LpMoldPlane *mplanes = calloc(rs.n_planes, sizeof(LpMoldPlane));
    for (size_t p = 0; p < rs.n_planes; p++) {
        mplanes[p].kind = rs.planes[p].kind;
        mplanes[p].tag = rs.planes[p].tag;
        mplanes[p].n_edges = ebufs[p].n;
        mplanes[p].rows = ebufs[p].rows;
        mplanes[p].cols = ebufs[p].cols;
        mplanes[p].weights = ebufs[p].weights;
        mplanes[p].witnesses = (const char **)ebufs[p].witnesses;
        mplanes[p].n_features = ebufs[p].n_features;
    }

    LpMoldLayerSpec *mlayers = calloc(n_layers, sizeof(LpMoldLayerSpec));
    for (uint32_t L = 0; L < n_layers; L++) {
        size_t srcL = L < MAX_MOLD_LAYERS ? L : (MAX_MOLD_LAYERS - 1u);
        mlayers[L].n_heads = n_heads;
        mlayers[L].n_kv_heads = n_kv_heads;
        mlayers[L].head_planes = rs.layer_heads[srcL];
        mlayers[L].ffn_plane = rs.layer_ffn[srcL];
    }

    if (auto_hidden) {
        size_t active_n = nv > seeded_count ? (nv - seeded_count) : 64u;
        uint32_t align = n_heads * 2u;
        if (align < 8u) align = 8u;
        uint32_t want_d = (uint32_t)(((active_n + align - 1u) / align) * align);
        if (want_d < 64u) want_d = 64u;
        if (want_d > 512u) want_d = 512u;
        hidden_dim = want_d;
        if (!req->ffn_dim && !rs.ffn_dim) ffn_dim = hidden_dim * 4u;
    }

    const char *mname = (req->model_name && *req->model_name) ? req->model_name : (rs.name[0] ? rs.name : "laplace-molded");
    LpMoldSpec spec = {
        .name = mname,
        .out_path = req->out_path,
        .n_vocab = nv,
        .tokens = (const char **)tokens,
        .token_ids = token_ids,
        .coords4d = coords4d,
        .bos_id = 1,
        .eos_id = nv > 2 ? 2 : 0,
        .hidden_dim = hidden_dim,
        .n_layers = n_layers,
        .n_heads = n_heads,
        .n_kv_heads = n_kv_heads,
        .ffn_dim = ffn_dim,
        .rms_eps = 1e-5,
        .rope_base = 10000.0,
        .f16_weights = req->f16_weights,
        .format = req->format,
        .tied_embeddings = tied_emb,
        .n_planes = rs.n_planes,
        .planes = mplanes,
        .layer_specs = mlayers,
        .lm_head_plane = rs.lm_head_plane,
        .lm_head_set_mode = rs.lm_head_set_mode,
        .lm_head_max_outside_prob = rs.lm_head_max_outside_prob
    };

    int rc = lp_mold_export(&spec);

    free(mlayers);
    free(mplanes);
    for (size_t p = 0; p < rs.n_planes; p++) eb_free(&ebufs[p]);
    free(ebufs);
    lp_idmap_free(unary_feats);
    lp_idmap_free(touched_claims);
    for (size_t i = 0; i < nv; i++) free(tokens[i]);
    free(tokens); free(token_ids); free(coords4d);
    for (size_t si = 0; si < seeded_count; si++) free(seeded_strs[si]);
    free(seeded_strs);
    lp_idmap_free(vmap);
    free(loaded_recipe);
    lp_text_free(tx);

    out->status_code = rc;
    out->n_vocab = (uint32_t)nv;
    out->n_edges = (uint32_t)total_edges;
    out->hidden_dim = hidden_dim;
    out->raw_weight_bytes_retained = 0;
    snprintf(out->out_path, sizeof out->out_path, "%s", req->out_path);
    if (rc) snprintf(out->error, sizeof out->error, "lp_mold_export failed (rc=%d)", rc);
    return rc;
}
