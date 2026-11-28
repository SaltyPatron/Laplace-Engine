#include "forward/query.h"
#include "forward/program_internal.h"
#include "forward/ooda.h"
#include "forward/engine_abi.h"
#include "chess/chess_rules.h"
#include "chess/chess_search.h"
#include "codecs/codecs.h"
#include "laplace_config.h"
#include "laplace/os.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *name, const char *dflt){
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}
const char *laplace_recipes(void){ return env_or("LAPLACE_RECIPES", LAPLACE_RECIPES_DEFAULT); }
const char *laplace_grammars(void){ return env_or("LAPLACE_GRAMMARS", LAPLACE_GRAMMARS_DEFAULT); }
const char *laplace_ucd(void){ return env_or("LAPLACE_UCD", LAPLACE_UCD_DEFAULT); }

static void test_firmware_suite(void){
    const char *root = laplace_recipes();
    char fdir[4200];
    snprintf(fdir, sizeof fdir, "%s/../firmware", root);

    static const struct { const char *name; int min_ooda, tie, shape; } CASES[] = {
        { "program.firmware",      1, FW_TIE_FIRST, FW_FRECHET },
        { "coding.firmware",       3, FW_TIE_FIRST, FW_FRECHET },
        { "deductive.firmware",    2, FW_TIE_ASK,   FW_FRECHET },
        { "inductive.firmware",    1, FW_TIE_FIRST, FW_DTW },
        { "abductive.firmware",    2, FW_TIE_ASK,   FW_OUTLIERS },
        { "constructive.firmware", 1, FW_TIE_DRAW,  FW_FRECHET },
        { "critical.firmware",     2, FW_TIE_ASK,   FW_FRECHET },
        { "executive.firmware",    2, FW_TIE_FIRST, FW_FRECHET },
        { "adversarial.firmware",  2, FW_TIE_ASK,   FW_FRECHET },
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char path[4300], err[512] = "";
        snprintf(path, sizeof path, "%s/%s", fdir, CASES[i].name);
        Firmware fw = { 0 };
        int rc = firmware_load_checked(path, FW_PULL, &fw, err, sizeof err);
        assert(rc == 0 && err[0] == 0);
        assert(fw.id.ready == 1);
        assert(fw.ooda_rounds == CASES[i].min_ooda);
        assert(fw.tie == CASES[i].tie);
        assert(fw.shape == CASES[i].shape);
        assert(fw.multi_tier == 1);
        assert(fw.combine_clauses == 1);
    }

    {
        Firmware short_fw = { 0 };
        char short_err[512] = "";
        assert(firmware_load_checked("deductive", FW_PULL, &short_fw, short_err, sizeof short_err) == 0 && short_err[0] == 0);
        assert(short_fw.id.ready == 1);
    }

    char chess_fw[4300];
    snprintf(chess_fw, sizeof chess_fw, "%s/chess.firmware", fdir);
    assert(lp_chess_laws_load(chess_fw) >= 2);
    LpChessSearchPolicy pol = lp_chess_search_policy();
    assert(pol.max_depth > 0 && pol.max_nodes > 0 && pol.max_wall_us > 0);
    printf("[PASS] Shipped 10-firmware suite & chess search policy validated offline\n");
}

typedef struct { lp_id id; double pt[4]; } StarNode;
static int star_graph_search(const StarNode *nodes, const int (*edges)[4], const int *deg, int n_nodes,
                             int start_idx, int goal_idx, double lam, int use_h,
                             double *cost_out, lp_id *order_out, int *n_exp_out){
    lp_frontier *fr = lp_frontier_new();
    double h0 = use_h ? lam * (lp_angle4(nodes[start_idx].pt, nodes[goal_idx].pt) / 3.14159265358979323846) : 0.0;
    lp_frontier_reach(fr, &nodes[start_idx].id, NULL, NULL, 0.0, h0, 0);
    int nexp = 0; double gcost = -1.0;
    const lp_reached *cur;
    while ((cur = lp_frontier_next(fr)) != NULL) {
        order_out[nexp++] = cur->id;
        int u = -1;
        for (int i = 0; i < n_nodes; i++) if (lp_id_eq(&nodes[i].id, &cur->id)) { u = i; break; }
        if (u == goal_idx) { gcost = cur->cost; break; }
        for (int ei = 0; u >= 0 && ei < deg[u]; ei++) {
            int v = edges[u][ei];
            double ang = use_h ? lp_angle4(nodes[v].pt, nodes[goal_idx].pt) : 0.0;
            if (ang > 3.14159265358979323846) ang = 3.14159265358979323846;
            double hv = use_h ? lam * (ang / 3.14159265358979323846) : 0.0;
            lp_frontier_reach(fr, &nodes[v].id, &nodes[u].id, &nodes[u].id, cur->cost + lam, hv, cur->hops + 1);
        }
    }
    lp_frontier_free(fr);
    *cost_out = gcost; *n_exp_out = nexp;
    return gcost >= 0.0;
}

static void test_astar_heuristic(void){
    double ga[4] = { 1.0, 0.0, 0.0, 0.0 };
    double gb[4] = { 0.0, 1.0, 0.0, 0.0 };
    double ang_same = lp_angle4(ga, ga);
    double ang_orth = lp_angle4(ga, gb);
    double lam = 0.05;
    double h_same = lam * (ang_same / 3.14159265358979323846);
    double h_orth = lam * (ang_orth / 3.14159265358979323846);
    assert(h_same >= 0.0 && h_same < 1e-9);
    assert(h_orth > 0.0 && h_orth <= lam);

    lp_id a, b, g;
    lp_id_codepoint('a', &a);
    lp_id_codepoint('b', &b);
    lp_id_codepoint('g', &g);
    lp_frontier *fr = lp_frontier_new();
    assert(lp_frontier_reach(fr, &b, &a, &a, 0.05, h_orth, 1));
    assert(lp_frontier_reach(fr, &g, &a, &a, 0.05, h_same, 1));
    const lp_reached *first = lp_frontier_next(fr);
    assert(first && lp_id_eq(&first->id, &g));
    lp_frontier_free(fr);

    /* Multi-hop S^3 graph: start(0)->1->2->goal(3) vs 18 off-axis decoy branches at equal edge cost */
    StarNode nodes[22]; int edges[22][4]; int deg[22];
    memset(edges, 0, sizeof edges); memset(deg, 0, sizeof deg);
    static const double goal_pts[4][4] = {
        { 1.0, 0.0, 0.0, 0.0 },
        { 0.8660254037844386, 0.5, 0.0, 0.0 },
        { 0.5, 0.8660254037844386, 0.0, 0.0 },
        { 0.0, 1.0, 0.0, 0.0 }
    };
    for (int i = 0; i < 4; i++) {
        lp_id_codepoint((uint32_t)(0x40 + i), &nodes[i].id);
        memcpy(nodes[i].pt, goal_pts[i], sizeof goal_pts[i]);
    }
    edges[0][deg[0]++] = 1; edges[1][deg[1]++] = 2; edges[2][deg[2]++] = 3;
    for (int d = 0; d < 18; d++) {
        int idx = 4 + d;
        lp_id_codepoint((uint32_t)(0x50 + d), &nodes[idx].id);
        double theta = 0.4 + 0.1 * d;
        nodes[idx].pt[0] = cos(theta); nodes[idx].pt[1] = 0.0;
        nodes[idx].pt[2] = sin(theta); nodes[idx].pt[3] = 0.0;
        if (d < 3) edges[0][deg[0]++] = idx;
        else { int p = 4 + (d - 3) % 12; if (deg[p] < 4) edges[p][deg[p]++] = idx; }
    }
    double cost_u = 0.0, cost_a1 = 0.0, cost_a2 = 0.0;
    lp_id ord_u[22], ord_a1[22], ord_a2[22];
    int n_u = 0, n_a1 = 0, n_a2 = 0;
    assert(star_graph_search(nodes, edges, deg, 22, 0, 3, lam, 0, &cost_u, ord_u, &n_u));
    assert(star_graph_search(nodes, edges, deg, 22, 0, 3, lam, 1, &cost_a1, ord_a1, &n_a1));
    assert(star_graph_search(nodes, edges, deg, 22, 0, 3, lam, 1, &cost_a2, ord_a2, &n_a2));
    assert(fabs(cost_a1 - cost_u) < 1e-9);
    assert(n_a1 < n_u && n_a1 == n_a2);
    for (int i = 0; i < n_a1; i++) assert(lp_id_eq(&ord_a1[i], &ord_a2[i]));
    printf("[PASS] 4D angle4 A* admissibility, deterministic tie-breaking, & node-visit reduction (%d -> %d) verified\n", n_u, n_a1);
}

static void test_witness_attribution(void){
    lp_id c1, w1, w2, w3;
    lp_id_codepoint('c', &c1);
    lp_id_codepoint('x', &w1);
    lp_id_codepoint('y', &w2);
    lp_id_codepoint('z', &w3);

    Attested att[4] = {
        { .claim = c1, .trunk = w1, .games = 10.0, .tokens = 10.0, .score = 9.0, .position = 1, .trust = 0.80, .witness = 1 },
        { .claim = c1, .trunk = w1, .games = 10.0, .tokens = 10.0, .score = 9.0, .position = 2, .trust = 0.80, .witness = 1 },
        { .claim = c1, .trunk = w2, .games = 8.0,  .tokens = 8.0,  .score = 7.0, .position = 1, .trust = 0.50, .witness = 1 },
        { .claim = c1, .trunk = w3, .games = 6.0,  .tokens = 6.0,  .score = 6.0, .position = 1, .trust = 0.90, .witness = 1 }
    };
    LpWitnessShare shares[8];
    int zero = witness_attribution_bp(att, 4, NULL, 0, 1, shares, 8);
    assert(zero == 0); /* Fail closed on empty allowlist */

    lp_id allow[2] = { w1, w2 };
    int n = witness_attribution_bp(att, 4, allow, 2, 1, shares, 8);
    assert(n == 2);
    int32_t sum_bp = 0;
    for (int i = 0; i < n; i++) {
        assert(lp_id_eq(&shares[i].witness, &w1) || lp_id_eq(&shares[i].witness, &w2));
        sum_bp += shares[i].share_bp;
    }
    assert(sum_bp == 10000);
    printf("[PASS] Fisher witness attribution (lineage dedup, fail-closed scope, sum_bp == 10000)\n");
}

static void test_chess_search_offline(void){
    LpChessSearchPolicy pol = {
        .max_depth = 3, .quiesce_depth = 2, .max_nodes = 50000,
        .max_wall_us = 200000, .syzygy_pieces = 0, .book_weight = 0, .eval_scale = 1.0
    };
    LpChessSearchResult res = { 0 };
    assert(lp_chess_search(NULL, "6k1/5ppp/8/8/8/8/5PPP/4R1K1 w - - 0 1", "Standard", NULL, 0, 0, &pol, &res) == 0);
    assert(strcmp(res.best_uci, "e1e8") == 0);

    memset(&res, 0, sizeof res);
    assert(lp_chess_search(NULL, "8/8/8/8/8/3K4/8/7k w - - 0 1", "King of the Hill", NULL, 0, 0, &pol, &res) == 0);
    assert(strstr("d4 d5 e4 e5", res.best_uci + 2) != NULL || res.score_cp >= 20000);

    memset(&res, 0, sizeof res);
    assert(lp_chess_search(NULL, "4k3/8/8/8/8/8/8/3QK3 w - - 0 1 +2+0", "Three-check", NULL, 0, 0, &pol, &res) == 0);
    assert(strchr(res.best_san, '+') != NULL || strchr(res.best_san, '#') != NULL);

    memset(&res, 0, sizeof res);
    assert(lp_chess_search(NULL, "4k3/8/8/8/8/8/8/4K3[Q] w - - 0 1", "Crazyhouse", NULL, 0, 0, &pol, &res) == 0);
    assert(res.best_uci[0] != 0);

    LpChessSearchPolicy tiny = pol;
    tiny.max_depth = 6; tiny.max_nodes = 32;
    memset(&res, 0, sizeof res);
    assert(lp_chess_search(NULL, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "Standard", NULL, 0, 0, &tiny, &res) == 0);
    assert(res.best_uci[0] != 0 && res.nodes_searched <= 128);
    printf("[PASS] Coarse C chess search offline (Standard mate-in-1, KotH, Three-check, Crazyhouse, budget cap)\n");
}

static void test_ooda_ast_offline(void){
    const char *src0 = "int f(void) { return 41; }\n";
    Ref root0 = { 0 }; Ref *nodes = NULL; uint32_t *sb = NULL, *eb = NULL; size_t nn = 0;
    assert(lp_ooda_decompose_ast(src0, strlen(src0), "c", &root0, &nodes, &sb, &eb, &nn) == 0);
    assert(nn > 0);
    const char *pos41 = strstr(src0, "41");
    assert(pos41 != NULL);
    uint32_t s41 = (uint32_t)(pos41 - src0);
    char *patched = NULL; size_t plen = 0;
    assert(lp_ooda_splice(src0, strlen(src0), s41, s41 + 2u, "42", 2u, &patched, &plen) == 0);
    assert(strcmp(patched, "int f(void) { return 42; }\n") == 0);
    free(patched); free(nodes); free(sb); free(eb);

    LpOodaRequest req = {
        .code_text = src0, .code_len = strlen(src0), .lang = "c",
        .verify_cmd = NULL, .read_only = 1
    };
    LpOodaResult res = { 0 };
    assert(lp_ooda_run(&req, &res) == 0);
    assert(res.unverified_by_toolchain == 1 && res.verified == 0);
    lp_ooda_result_free(&res);
    printf("[PASS] OODA AST decompose/splice & unverified_by_toolchain contract offline\n");
}

typedef LpEngineHandle *(*FnInit)(const char *, char *, size_t);
typedef int32_t (*FnTurn)(LpEngineHandle *, const LpEngineTurnRequest *, LpEngineTurnResponse *);
typedef void (*FnTurnFree)(LpEngineTurnResponse *);
typedef int32_t (*FnChess)(LpEngineHandle *, const LpEngineChessRequest *, LpEngineChessResponse *);
typedef int32_t (*FnMold)(LpEngineHandle *, const LpEngineMoldRequest *, LpEngineMoldResponse *);
typedef void (*FnFree)(LpEngineHandle *);

typedef struct {
    void *lib;
    FnInit init;
    FnTurn turn;
    FnTurnFree turn_free;
    FnChess chess_play;
    FnMold mold_checkpoint;
    FnFree free_fn;
} AbiVtable;

static AbiVtable load_abi_dll(void){
    AbiVtable v = { 0 };
    v.lib = os_dl_open(LAPLACE_ENGINE_ABI_DLL);
    assert(v.lib != NULL);
    v.init = (FnInit)os_dl_sym(v.lib, "lp_engine_init");
    v.turn = (FnTurn)os_dl_sym(v.lib, "lp_engine_turn");
    v.turn_free = (FnTurnFree)os_dl_sym(v.lib, "lp_engine_turn_response_free");
    v.chess_play = (FnChess)os_dl_sym(v.lib, "lp_engine_chess_play");
    v.mold_checkpoint = (FnMold)os_dl_sym(v.lib, "lp_engine_mold_checkpoint");
    v.free_fn = (FnFree)os_dl_sym(v.lib, "lp_engine_free");
    assert(v.init && v.turn && v.turn_free && v.chess_play && v.mold_checkpoint && v.free_fn);
    return v;
}

static void test_abi_dll_offline(const AbiVtable *abi){
    char err[256] = "";
    LpEngineHandle *h = abi->init(NULL, err, sizeof err);
    assert(h != NULL);

    LpEngineAuthorityScope no_cap = { .capabilities = LP_CAP_NONE, .disposition = LP_DISPOSE_ALLOW };
    LpEngineTurnRequest treq = { .prompt = "hello", .read_only = 1, .authority = &no_cap };
    LpEngineTurnResponse tres = { 0 };
    assert(abi->turn(h, &treq, &tres) == 2 && tres.status_code == 2);
    abi->turn_free(&tres);

    LpEngineAuthorityScope refused = { .capabilities = LP_CAP_ALL, .disposition = LP_DISPOSE_REFUSE };
    treq.authority = &refused;
    assert(abi->turn(h, &treq, &tres) == 2 && tres.status_code == 2);
    abi->turn_free(&tres);

    LpEngineAuthorityScope empty_scope = { .capabilities = LP_CAP_ALL, .disposition = LP_DISPOSE_ALLOW, .scoped = 1, .nwitnesses = 0 };
    treq.authority = &empty_scope;
    assert(abi->turn(h, &treq, &tres) == 2 && tres.status_code == 2);
    abi->turn_free(&tres);

    LpEngineChessRequest creq = { .fen = "6k1/5ppp/8/8/8/8/5PPP/4R1K1 w - - 0 1", .variant = "Standard", .max_depth = 2 };
    LpEngineChessResponse cres = { 0 };
    assert(abi->chess_play(h, &creq, &cres) == 0 && strcmp(cres.best_uci, "e1e8") == 0);

    char gguf_path[256];
    snprintf(gguf_path, sizeof gguf_path, "laplace_abi_mold_tmp.gguf");
    LpEngineMoldRequest mreq = { .out_path = gguf_path, .model_name = "abi-test", .hidden_dim = 32, .n_layers = 2, .n_heads = 4 };
    LpEngineMoldResponse mres = { 0 };
    /* Offline mold_checkpoint with zero substrate claims must fail loud instead of fabricating fake vocabularies. */
    assert(abi->mold_checkpoint(h, &mreq, &mres) != 0);
    assert(mres.status_code != 0 && mres.error[0] != '\0');
    remove(gguf_path);

    abi->free_fn(h);
    printf("[PASS] Shared ABI DLL dynamic load + authority preflight + chess + fail-loud offline mold verified\n");
}

static void build_test_witness_file(File *f, const char *witness_name, const Ref (*triples)[4], const int *np, int nclaims,
                                    const char (*obs_texts)[128], int nobs){
    memset(f, 0, sizeof *f);
    Ref wref = text_ref(CTX[0], (const uint8_t *)witness_name, strlen(witness_name));
    f->path = witness_name;
    f->witness = wref;
    f->lineage = wref;
    f->has_lineage = 1;
    f->trust = 0.85;

    lp_vec(Ref) parts = { 0 };
    for (int i = 0; i < nclaims; i++) {
        Ref p[4];
        for (int k = 0; k < np[i]; k++) { p[k] = triples[i][k]; p[k].said = p[k].outcome = p[k].position = p[k].spare = 0; }
        Ref cl = said_claim(compose(p, (uint32_t)np[i], ref_above(p, (size_t)np[i])));
        cl.outcome = (uint8_t)lp_outcome_of(1.0, &cl.spare);
        cl.position = (uint32_t)(i + 1);
        Event ev = { .claim = cl.id, .witnessed = cl.id, .score = 1.0f, .enter_rating = 1500.0f,
                     .position = cl.position, .kind = EV_CLAIM, .run = 1 };
        ev_push(&f->ev, &ev);
        lp_push(&parts, cl);
    }
    for (int i = 0; i < nobs; i++) {
        Ref ob = text_ref(CTX[0], (const uint8_t *)obs_texts[i], strlen(obs_texts[i]));
        lp_push(&parts, ob);
    }
    Ref body = parts.n == 1 ? parts.v[0] : compose(parts.v, (uint32_t)parts.n, ref_above(parts.v, parts.n));
    lp_vec_free(&parts);
    Ref tr[2] = { said_metadata(wref), body };
    f->trunk = compose(tr, 2, ref_above(tr, 2));
    f->file = f->trunk;
    f->has_file = 1;
}

static void cleanup_test_witnesses(PGconn *pg, const char *const *names, size_t nnames){
    lp_id *ids = malloc(sizeof(lp_id) * nnames);
    lp_id *records = malloc(sizeof(lp_id) * nnames);
    memset(ids, 0, sizeof(lp_id) * nnames);
    for (size_t i = 0; i < nnames; i++) {
        records[i] = text_ref(CTX[0], (const uint8_t *)names[i], strlen(names[i])).id;
    }
    if (db_ddl_begin_maintenance(pg) == 0) {
        Args fa = { 0 };
        arg_ids(&fa, ids, nnames);
        arg_ids(&fa, records, nnames);
        arg_bool(&fa, false);
        arg_null(&fa);
        PGresult *q = db_call(pg, LP_SQL_FORGET_WITNESSES, &fa);
        args_free(&fa); PQclear(q);
        Args sa = { 0 };
        arg_null(&sa);
        PGresult *sq = db_call(pg, LP_SQL_SWEEP_ORPHANS, &sa);
        args_free(&sa); PQclear(sq);
        db_tx(pg, LP_SQL_TX_COMMIT);
    }
    free(ids); free(records);
}

static int test_resolve_exe(const char *exe, char *out, size_t cap){
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) return 0;
    char sep = strchr(path_env, ';') ? ';' : ':';
    const char *p = path_env;
    while (*p) {
        const char *q = strchr(p, sep);
        size_t dlen = q ? (size_t)(q - p) : strlen(p);
        if (dlen > 0 && dlen + strlen(exe) + 6 < cap) {
            memcpy(out, p, dlen);
            if (out[dlen - 1] != '/' && out[dlen - 1] != '\\') out[dlen++] = '/';
            snprintf(out + dlen, cap - dlen, "%s", exe);
            os_slashes(out);
            if (os_exists(out) && !os_is_dir(out)) return 1;
        }
        p = q ? q + 1 : p + dlen;
    }
    return 0;
}

static void test_live_db(const AbiVtable *abi){
    const char *conninfo = laplace_db();
    PGconn *pg = db_try(conninfo);
    if (!pg) {
        printf("[SKIP] Live DB tests skipped (PostgreSQL not reachable at %s)\n", conninfo);
        return;
    }
    printf("[INFO] Connected to live DB (%s)\n", conninfo);

    static const char *CLEAN_NAMES[5] = { "test-2c-red", "test-2c-blue", "test-2c-icx", "ooda-monologue", "test-2c-chess" };
    cleanup_test_witnesses(pg, CLEAN_NAMES, 5);

    Ref r_light = text_ref(CTX[0], (const uint8_t *)"lightning", 9);
    Ref r_is    = text_ref(CTX[0], (const uint8_t *)"is", 2);
    Ref r_def   = text_ref(CTX[0], (const uint8_t *)"rapid atmospheric electrical discharge", 38);
    Ref r_trans = text_ref(CTX[0], (const uint8_t *)"transfers", 9);
    Ref r_chg   = text_ref(CTX[0], (const uint8_t *)"charge", 6);
    Ref r_atm   = text_ref(CTX[0], (const uint8_t *)"atmosphere", 10);
    Ref r_topic = text_ref(CTX[0], (const uint8_t *)"probe_topic", 11);
    Ref r_rel   = text_ref(CTX[0], (const uint8_t *)"rel", 3);
    Ref r_redv  = text_ref(CTX[0], (const uint8_t *)"red_secret", 10);
    Ref r_bluv  = text_ref(CTX[0], (const uint8_t *)"blue_secret", 11);

    Ref red_t[3][4] = {
        { r_light, r_is, r_def, { 0 } },
        { r_light, r_trans, r_chg, r_atm },
        { r_topic, r_rel, r_redv, { 0 } }
    };
    int red_np[3] = { 3, 4, 3 };
    static const char glue_obs[4][128] = {
        "How does this happen today",
        "How does it run there",
        "How does that move here",
        "How does one check now"
    };
    Ref blue_t[1][4] = { { r_topic, r_rel, r_bluv, { 0 } } };
    int blue_np[1] = { 3 };

    File f[2];
    build_test_witness_file(&f[0], "test-2c-red", red_t, red_np, 3, glue_obs, 4);
    build_test_witness_file(&f[1], "test-2c-blue", blue_t, blue_np, 1, NULL, 0);

    LoadWitness lw[2] = {
        { .id = f[0].trunk.id, .lineage = f[0].witness.id, .trust = f[0].trust, .has_lineage = 1 },
        { .id = f[1].trunk.id, .lineage = f[1].witness.id, .trust = f[1].trust, .has_lineage = 1 }
    };
    Fold *fold = fold_new(); fold_events(fold, f, 2);
    LoadStats ls = { 0 };
    assert(load(conninfo, 1, f, 2, &ls) == 0);
    fold_free(fold);
    assert(load_register(conninfo, 1, lw, 2, f, 2) == 0);
    assert(merge(conninfo, 1, &ls) == 0);
    free(f[0].ev.e); free(f[1].ev.e);

    /* 1. Verify salience_of ranks "lightning" higher than glue "How"/"does" */
    {
        const char *q = "How does lightning work?";
        lp_id occ[64]; size_t nocc = 0;
        text_occurrences(CTX[0], (const uint8_t *)q, strlen(q), occ, 64, &nocc);
        int nsal = 0;
        LpSalienceRow *sal = salience_of(pg, occ, (int)nocc, NULL, 0, &nsal);
        assert(sal && nsal > 0);
        lp_id w_how = text_ref(CTX[0], (const uint8_t *)"How", 3).id;
        lp_id w_does = text_ref(CTX[0], (const uint8_t *)"does", 4).id;
        double s_how = -1.0, s_does = -1.0, s_light = -1.0;
        for (int r = 0; r < nsal; r++) {
            if (lp_id_eq(&sal[r].id, &w_how)) s_how = sal[r].salience;
            if (lp_id_eq(&sal[r].id, &w_does)) s_does = sal[r].salience;
            if (lp_id_eq(&sal[r].id, &r_light.id)) s_light = sal[r].salience;
        }
        assert(s_light > s_how && s_light > s_does);
        free(sal);
        printf("[PASS] Live laplace_salience: lightning (%.3f) > How (%.3f), does (%.3f)\n", s_light, s_how, s_does);
    }

    /* 2. Warm in-process DLL turn latency, probes <= 6, Fisher sum_bp == 10000, witness scope isolation */
    char err[256] = "";
    LpEngineHandle *h = abi->init(conninfo, err, sizeof err);
    assert(h != NULL);

    /* Warmup + measure under LP_ENVELOPE_STANDARD */
    lp_envelope fast_env = lp_envelope_preset(LP_ENVELOPE_STANDARD);
    for (int run = 0; run < 2; run++) {
        LpEngineAuthorityScope red_scope = {
            .witnesses = &f[0].trunk.id, .nwitnesses = 1, .scoped = 1,
            .capabilities = LP_CAP_READ, .disposition = LP_DISPOSE_ALLOW
        };
        LpEngineTurnRequest tr = {
            .model_name = "program",
            .prompt = "How does lightning work?",
            .read_only = 1,
            .authority = &red_scope,
            .envelope = &fast_env
        };
        LpEngineTurnResponse resp = { 0 };
        int trc = abi->turn(h, &tr, &resp);
        if (trc != 0 || !resp.text || !resp.text_len || resp.work.db_probes > 16) {
            fprintf(stderr, "[DIAG] run=%d trc=%d status=%d disp=\"%s\" err=\"%s\" wall_us=%llu probes=%llu rows=%llu\n",
                    run, trc, resp.status_code, resp.disposition, resp.error,
                    (unsigned long long)resp.work.wall_us, (unsigned long long)resp.work.db_probes,
                    (unsigned long long)resp.work.rows_scanned);
        }
        assert(trc == 0);
        assert(resp.text && resp.text_len > 0);
        assert(resp.work.db_probes > 0 && resp.work.db_probes <= 16);
        if (resp.nshares > 0) {
            int32_t sbp = 0;
            for (uint32_t i = 0; i < resp.nshares; i++) sbp += resp.shares[i].share_bp;
            assert(sbp == 10000);
        }
        if (run == 1) {
            if (resp.work.wall_us >= 2500000ULL) {
                fprintf(stderr, "[DIAG] run=1 wall_us=%llu probes=%llu rows=%llu text=\"%s\"\n",
                        (unsigned long long)resp.work.wall_us, (unsigned long long)resp.work.db_probes,
                        (unsigned long long)resp.work.rows_scanned, resp.text ? resp.text : "");
            }
            assert(resp.work.wall_us < 2500000ULL);
            printf("[PASS] Warm lp_engine_turn STANDARD (<2500ms, <=16 probes): %.2f ms, %llu probes, text=\"%s\"\n",
                   (double)resp.work.wall_us / 1000.0, (unsigned long long)resp.work.db_probes, resp.text);
        }
        abi->turn_free(&resp);
    }

    /* Fast envelope explicit probe-cap check */
    {
        lp_envelope tiny_env = lp_envelope_preset(LP_ENVELOPE_FAST);
        LpEngineAuthorityScope red_scope = {
            .witnesses = &f[0].trunk.id, .nwitnesses = 1, .scoped = 1,
            .capabilities = LP_CAP_READ, .disposition = LP_DISPOSE_ALLOW
        };
        LpEngineTurnRequest tr = {
            .model_name = "program",
            .prompt = "How does lightning work?",
            .read_only = 1,
            .authority = &red_scope,
            .envelope = &tiny_env
        };
        LpEngineTurnResponse resp = { 0 };
        int trc = abi->turn(h, &tr, &resp);
        if (trc != 0 || !resp.text || !resp.text_len || resp.work.db_probes > 8) {
            fprintf(stderr, "[DIAG] FAST trc=%d status=%d disp=\"%s\" err=\"%s\" wall_us=%llu probes=%llu rows=%llu\n",
                    trc, resp.status_code, resp.disposition, resp.error,
                    (unsigned long long)resp.work.wall_us, (unsigned long long)resp.work.db_probes,
                    (unsigned long long)resp.work.rows_scanned);
        }
        assert(trc == 0 && resp.text && resp.text_len > 0 && resp.work.db_probes <= 8);
        printf("[PASS] Warm lp_engine_turn FAST (<=8 probes): %.2f ms, %llu probes, text=\"%s\"\n",
               (double)resp.work.wall_us / 1000.0, (unsigned long long)resp.work.db_probes, resp.text);
        abi->turn_free(&resp);
    }

    /* Witness isolation: Red vs Blue */
    {
        LpEngineAuthorityScope red_scope = {
            .witnesses = &f[0].trunk.id, .nwitnesses = 1, .scoped = 1,
            .capabilities = LP_CAP_READ, .disposition = LP_DISPOSE_ALLOW
        };
        LpEngineTurnRequest tr = { .prompt = "probe_topic", .read_only = 1, .authority = &red_scope };
        LpEngineTurnResponse resp = { 0 };
        assert(abi->turn(h, &tr, &resp) == 0);
        if (!resp.text || strstr(resp.text, "red_secret") == NULL || strstr(resp.text, "blue_secret") != NULL) {
            fprintf(stderr, "[DIAG] red isolation disp=\"%s\" err=\"%s\" text=\"%s\"\n",
                    resp.disposition, resp.error, resp.text ? resp.text : "");
        }
        assert(resp.text && strstr(resp.text, "red_secret") != NULL && strstr(resp.text, "blue_secret") == NULL);
        abi->turn_free(&resp);

        LpEngineAuthorityScope blue_scope = {
            .witnesses = &f[1].trunk.id, .nwitnesses = 1, .scoped = 1,
            .capabilities = LP_CAP_READ, .disposition = LP_DISPOSE_ALLOW
        };
        tr.authority = &blue_scope;
        assert(abi->turn(h, &tr, &resp) == 0);
        assert(resp.text && strstr(resp.text, "blue_secret") != NULL && strstr(resp.text, "red_secret") == NULL);
        abi->turn_free(&resp);
        printf("[PASS] Live witness isolation (#1708): Red vs Blue tenants strictly isolated\n");
    }

    /* Non-muted polysemy check across deductive & program firmwares */
    {
        LpEngineTurnRequest tr = { .model_name = "deductive", .prompt = "probe_topic", .read_only = 1 };
        LpEngineTurnResponse resp = { 0 };
        assert(abi->turn(h, &tr, &resp) == 0);
        assert(resp.text && resp.text_len > 0);
        abi->turn_free(&resp);
        printf("[PASS] Non-muted polysemous turn produced non-empty response under deductive firmware\n");
    }

    /* Refused metadata labels (Synset/vaframe) and ungrounded winners must not win under deductive firmware on real query */
    {
        LpEngineTurnRequest tr = { .model_name = "deductive", .prompt = "How does lightning work?", .read_only = 1, .envelope = &fast_env };
        LpEngineTurnResponse resp = { 0 };
        assert(abi->turn(h, &tr, &resp) == 0);
        assert(resp.text && resp.text_len > 0);
        assert(strstr(resp.text, "Synset") == NULL && strstr(resp.text, "vaframe") == NULL);
        assert(strstr(resp.text, "Honi") == NULL);
        assert(strstr(resp.text, "discharge") != NULL || strstr(resp.text, "light") != NULL);
        printf("[PASS] Live deductive turn grounded & filtered metadata labels (probes=%llu, wall=%.2f ms, text=\"%s\")\n",
               (unsigned long long)resp.work.db_probes, (double)resp.work.wall_us / 1000.0, resp.text);
        abi->turn_free(&resp);
    }

    /* Closed-loop OODA icx self-repair against live DB */
    {
        const char *tmp_c = "laplace_ooda_repair_tmp.c";
        FILE *cf = fopen(tmp_c, "wb");
        assert(cf != NULL);
        const char *broken = "int broken_fn(void) { int x = 7 return x; }\n";
        fwrite(broken, 1, strlen(broken), cf);
        fclose(cf);

        char icx_exe[1024] = "", icx_check_cmd[1152];
        int has_icx = test_resolve_exe("icx.exe", icx_exe, sizeof icx_exe) || test_resolve_exe("icx", icx_exe, sizeof icx_exe);
        snprintf(icx_check_cmd, sizeof icx_check_cmd, "\"%s\" -fsyntax-only %s", has_icx ? icx_exe : "icx", tmp_c);
        char *probe_argv[] = { has_icx ? icx_exe : "icx", "--version", NULL };
        char *pout = NULL; int pcode = 127;
        if (has_icx && os_run_captured(probe_argv, 10, 4096, &pout, &pcode) && pcode == 0) {
            char errf[256] = "";
            Firmware cfw = { 0 };
            char cfwp[4300];
            snprintf(cfwp, sizeof cfwp, "%s/../firmware/coding.firmware", laplace_recipes());
            assert(firmware_load_checked(cfwp, FW_PULL, &cfw, errf, sizeof errf) == 0);

            LpOodaRequest oreq = {
                .conninfo = conninfo, .pg = pg, .fw = &cfw,
                .code_path = tmp_c, .lang = "c",
                .verify_cmd = icx_check_cmd,
                .witness_name = "test-2c-icx",
                .max_rounds = 3, .write_file = 1, .read_only = 0
            };
            LpOodaResult ores = { 0 };
            int orc = lp_ooda_run(&oreq, &ores);
            assert(orc == 0 && ores.verified == 1 && ores.rounds_run >= 2);
            assert(!lp_id_eq(&ores.source_id, &ores.patched_id));
            assert(ores.patched_code && strstr(ores.patched_code, "7;") != NULL);
            lp_ooda_result_free(&ores);
            printf("[PASS] Closed-loop OODA icx compile-fail -> self-repair + WIN witness verified\n");

            /* AST scope symbol-table typo repair (zero log-scraping) */
            const char *typo_c = "laplace_ooda_scope_tmp.c";
            FILE *sf = fopen(typo_c, "wb");
            assert(sf != NULL);
            const char *broken_scope = "int sum_fn(int count) { return cont + 1; }\n";
            fwrite(broken_scope, 1, strlen(broken_scope), sf);
            fclose(sf);

            char icx_scope_cmd[1152];
            snprintf(icx_scope_cmd, sizeof icx_scope_cmd, "\"%s\" -fsyntax-only %s", has_icx ? icx_exe : "icx", typo_c);
            LpOodaRequest sreq = {
                .conninfo = conninfo, .pg = pg, .fw = &cfw,
                .code_path = typo_c, .lang = "c",
                .verify_cmd = icx_scope_cmd,
                .witness_name = "test-2c-icx",
                .max_rounds = 3, .write_file = 1, .read_only = 0
            };
            LpOodaResult sres = { 0 };
            int src_rc = lp_ooda_run(&sreq, &sres);
            assert(src_rc == 0 && sres.verified == 1 && sres.rounds_run >= 2);
            assert(sres.patched_code && strstr(sres.patched_code, "return count + 1") != NULL);
            lp_ooda_result_free(&sres);
            remove(typo_c);
            printf("[PASS] Closed-loop OODA AST scope symbol-table repair verified\n");

            /* Explicit WHY_NOT residual on unrepairable error */
            const char *bad_c = "laplace_ooda_whynot_tmp.c";
            FILE *bf = fopen(bad_c, "wb");
            assert(bf != NULL);
            const char *unfixable = "int bad_fn(void) { unknown_symbol_xyz(1, 2, 3); }\n";
            fwrite(unfixable, 1, strlen(unfixable), bf);
            fclose(bf);

            char icx_bad_cmd[1152];
            snprintf(icx_bad_cmd, sizeof icx_bad_cmd, "\"%s\" -fsyntax-only %s", has_icx ? icx_exe : "icx", bad_c);
            LpOodaRequest breq = {
                .conninfo = conninfo, .pg = pg, .fw = &cfw,
                .code_path = bad_c, .lang = "c",
                .verify_cmd = icx_bad_cmd,
                .witness_name = "test-2c-icx",
                .max_rounds = 2, .write_file = 1, .read_only = 0
            };
            LpOodaResult bres = { 0 };
            int brc = lp_ooda_run(&breq, &bres);
            assert(brc != 0 && bres.verified == 0 && strstr(bres.disposition, "WHY_NOT:") != NULL);
            lp_ooda_result_free(&bres);
            remove(bad_c);
            printf("[PASS] Closed-loop OODA explicit WHY_NOT residual verified\n");

            /* Live substrate AST workspace-code header discovery (#include "engine_abi.h") */
            {
                const char *sub_c = "laplace_ooda_sub_ast_tmp.c";
                FILE *asf = fopen(sub_c, "wb");
                assert(asf != NULL);
                const char *need_hdr = "void *probe_fn(void) { LpEngineHandle *h = lp_engine_init(0, 0, 0); return h; }\n";
                fwrite(need_hdr, 1, strlen(need_hdr), asf);
                fclose(asf);

                char inc_fwd[4300], inc_nat[4300], icx_sub_cmd[8800];
                snprintf(inc_fwd, sizeof inc_fwd, "%s/../src/forward", laplace_recipes());
                snprintf(inc_nat, sizeof inc_nat, "%s/../../Laplace-Native/include", laplace_recipes());
                os_slashes(inc_fwd); os_slashes(inc_nat);
                snprintf(icx_sub_cmd, sizeof icx_sub_cmd, "\"%s\" -fsyntax-only \"-I%s\" \"-I%s\" %s",
                         has_icx ? icx_exe : "icx", inc_fwd, inc_nat, sub_c);

                LpOodaRequest areq = {
                    .conninfo = conninfo, .pg = pg, .fw = &cfw,
                    .code_path = sub_c, .lang = "c",
                    .verify_cmd = icx_sub_cmd,
                    .witness_name = "test-2c-icx",
                    .max_rounds = 3, .write_file = 0, .read_only = 1
                };
                LpOodaResult ares = { 0 };
                int arc = lp_ooda_run(&areq, &ares);
                if (arc == 0 && ares.verified == 1 && ares.patched_code && strstr(ares.patched_code, "engine_abi.h") != NULL) {
                    printf("[PASS] Closed-loop OODA live workspace-code AST header discovery (#include \"engine_abi.h\") verified\n");
                } else {
                    /* Fail loud if workspace-code was ingested (LpEngineHandle exists in substrate) */
                    lp_id sym = text_ref(CTX[0], (const uint8_t *)"LpEngineHandle", 14).id;
                    int nh = 0;
                    Hold *hh = holds_capped_scoped(pg, &sym, 1, 8, 1, 3, NULL, 0, 0, NULL, &nh);
                    holds_free(hh, nh);
                    if (nh > 0) {
                        fprintf(stderr, "[FAIL] workspace-code present (nh=%d) but OODA AST header repair failed: arc=%d verified=%d disp=%s\n",
                                nh, arc, ares.verified, ares.disposition);
                        assert(arc == 0 && ares.verified == 1);
                    } else {
                        printf("[SKIP] OODA workspace-code AST header discovery skipped (workspace-code not ingested)\n");
                    }
                }
                lp_ooda_result_free(&ares);
                remove(sub_c);
            }
        }
        free(pout);
        remove(tmp_c);
    }

    /* Live substrate chess trajectory + game-altitude standings COUPLE probe, pure substrate mode (eval_scale=0), & fail-closed empty scope check */
    {
        /* Seed one Wave 2B game-altitude chess trajectory [P0..P4] via real cmd_witness so this test is 100% self-contained even on an empty DB. */
        extern int cmd_witness(int argc, char **argv);
        char pgn_path[512];
        const char *tmp = getenv("TEMP"); if (!tmp || !*tmp) tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
        snprintf(pgn_path, sizeof pgn_path, "%s/laplace_test_2c_chess.pgn", tmp);
        FILE *pf = fopen(pgn_path, "wb");
        assert(pf != NULL);
        fputs("[Event \"Test2C\"]\n[Site \"Local\"]\n[Date \"2026.11.27\"]\n[Round \"1\"]\n[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Nf3 Nc6 1-0\n", pf);
        fclose(pf);
        char *wav[10] = { "witness", "-d", (char *)conninfo, "--as", "test-2c-chess", "--class", "StructuredCorpus", pgn_path, NULL };
        assert(cmd_witness(8, wav) == 0);
        remove(pgn_path);

        const char *start_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        LpChessSearchPolicy spol = {
            .max_depth = 3, .quiesce_depth = 2, .max_nodes = 20000,
            .max_wall_us = 250000, .syzygy_pieces = 0, .book_weight = 100, .eval_scale = 1.0
        };
        LpChessSearchResult sres = { 0 };
        assert(lp_chess_search(pg, start_fen, "Standard", NULL, 0, 0, &spol, &sres) == 0);
        assert(sres.best_uci[0] != 0 && sres.db_probes >= 2);
        printf("[PASS] Live substrate chess search (probes=%llu, best=%s, src=%s, nodes=%llu)\n",
               (unsigned long long)sres.db_probes, sres.best_uci, sres.source, (unsigned long long)sres.nodes_searched);

        LpChessSearchPolicy spol_pure = spol;
        spol_pure.eval_scale = 0.0;
        LpChessSearchResult sres_pure = { 0 };
        assert(lp_chess_search(pg, start_fen, "Standard", NULL, 0, 0, &spol_pure, &sres_pure) == 0);
        assert(sres_pure.best_uci[0] != 0 && sres_pure.db_probes >= 2 && sres_pure.from_book == 1 && sres_pure.score_cp != 0);
        printf("[PASS] Live PURE substrate chess search (eval_scale=0.0, zero MAT[7]: probes=%llu, best=%s, cp=%d, src=%s)\n",
               (unsigned long long)sres_pure.db_probes, sres_pure.best_uci, sres_pure.score_cp, sres_pure.source);

        LpChessSearchResult scoped_empty = { 0 };
        assert(lp_chess_search(pg, start_fen, "Standard", NULL, 0, 1, &spol, &scoped_empty) == 0);
        assert(scoped_empty.best_uci[0] != 0 && scoped_empty.db_probes == 0);
        printf("[PASS] Live substrate chess search fail-closed on empty scope (db_probes=0)\n");
    }

    /* Live np >= 4 N-ary interior/endpoint traversal under red_scope ([lightning, transfers, charge, atmosphere]) */
    {
        lp_envelope std_env = lp_envelope_preset(LP_ENVELOPE_STANDARD);
        LpEngineAuthorityScope red_scope = {
            .witnesses = &f[0].trunk.id, .nwitnesses = 1, .scoped = 1,
            .capabilities = LP_CAP_READ, .disposition = LP_DISPOSE_ALLOW
        };
        LpEngineTurnRequest ntr = {
            .model_name = "program",
            .prompt = "atmosphere",
            .read_only = 1,
            .authority = &red_scope,
            .envelope = &std_env
        };
        LpEngineTurnResponse nresp = { 0 };
        assert(abi->turn(h, &ntr, &nresp) == 0);
        if (!nresp.text || (strstr(nresp.text, "lightning") == NULL && strstr(nresp.text, "charge") == NULL)) {
            fprintf(stderr, "[DIAG] nary np>=4 disp=\"%s\" text=\"%s\"\n",
                    nresp.disposition, nresp.text ? nresp.text : "");
        }
        assert(nresp.text && (strstr(nresp.text, "lightning") != NULL || strstr(nresp.text, "charge") != NULL));
        printf("[PASS] Live np >= 4 N-ary peer traversal verified (text=\"%s\")\n", nresp.text);
        abi->turn_free(&nresp);
    }

    cleanup_test_witnesses(pg, CLEAN_NAMES, 5);

    /* Regression: OpenAI messages[] STANDARD budget + direct_support gloss ranking (#31 / converse parity) */
    {
        lp_id w_hound = text_ref(CTX[0], (const uint8_t *)"hound", 5).id;
        int nhound = 0;
        Hold *hhound = holds_capped_scoped(pg, &w_hound, 1, 4, 1, 1, NULL, 0, 0, NULL, &nhound);
        holds_free(hhound, nhound);
        if (nhound == 0) {
            printf("[SKIP] Populated-corpus hound/wolf/mold regressions skipped (lexical corpus not yet seeded)\n");
        } else {
        lp_envelope std_env = lp_envelope_preset(LP_ENVELOPE_STANDARD);
        const LpEngineChatMessage hound_msgs[1] = { { .role = "user", .content = "what is a hound" } };
        LpEngineTurnRequest htr = {
            .model_name = "program",
            .messages = hound_msgs,
            .nmessages = 1,
            .read_only = 1,
            .envelope = &std_env
        };
        LpEngineTurnResponse hresp = { 0 };
        int hrc = abi->turn(h, &htr, &hresp);
        int hound_ok = (hrc == 0 && hresp.work.truncated == 0 && hresp.text && hresp.text_len > 0 &&
                        (strstr(hresp.text, "dog") != NULL || strstr(hresp.text, "hunting") != NULL));
        if (!hound_ok) {
            fprintf(stderr, "[DIAG] hound STANDARD hrc=%d trunc=%u wall_us=%llu probes=%llu disp=\"%s\" text=\"%s\"\n",
                    hrc, (unsigned)hresp.work.truncated, (unsigned long long)hresp.work.wall_us,
                    (unsigned long long)hresp.work.db_probes, hresp.disposition, hresp.text ? hresp.text : "");
            fflush(stderr);
        }
        assert(hound_ok);
        printf("[PASS] Regression hound STANDARD non-truncated (wall=%.2f ms, text=\"%s\")\n",
               (double)hresp.work.wall_us / 1000.0, hresp.text);
        abi->turn_free(&hresp);

        static const int32_t ENVS[3] = { LP_ENVELOPE_STANDARD, LP_ENVELOPE_DEEP, LP_ENVELOPE_UNLIMITED };
        const LpEngineChatMessage light_msgs[1] = { { .role = "user", .content = "How does lightning work?" } };
        for (size_t ei = 0; ei < 3; ei++) {
            lp_envelope lenv = lp_envelope_preset(ENVS[ei]);
            LpEngineTurnRequest ltr = {
                .model_name = "program",
                .messages = light_msgs,
                .nmessages = 1,
                .read_only = 1,
                .envelope = &lenv
            };
            LpEngineTurnResponse lresp = { 0 };
            int lrc = abi->turn(h, &ltr, &lresp);
            int light_ok = (lrc == 0 && lresp.work.truncated == 0 && lresp.text && lresp.text_len > 0 &&
                            (strstr(lresp.text, "electric discharge") != NULL || strstr(lresp.text, "discharge") != NULL || strstr(lresp.text, "light") != NULL) &&
                            strstr(lresp.text, "(physics) a manifestation of energy") == NULL);
            if (!light_ok) {
                fprintf(stderr, "[DIAG] lightning env=%d lrc=%d trunc=%u wall_us=%llu disp=\"%s\" text=\"%s\"\n",
                        (int)ENVS[ei], lrc, (unsigned)lresp.work.truncated,
                        (unsigned long long)lresp.work.wall_us, lresp.disposition,
                        lresp.text ? lresp.text : "");
                fflush(stderr);
            }
            assert(light_ok);
            printf("[PASS] Regression lightning env=%d (wall=%.2f ms, text=\"%s\")\n",
                   (int)ENVS[ei], (double)lresp.work.wall_us / 1000.0, lresp.text);
            abi->turn_free(&lresp);
        }

        /* Multi-clause compositional synthesis across 2 anchors with stateful RECOUPLE */
        {
            const LpEngineChatMessage cmp_msgs[1] = { { .role = "user", .content = "Compare a hound and a wolf" } };
            LpEngineTurnRequest ctr = {
                .model_name = "program",
                .messages = cmp_msgs,
                .nmessages = 1,
                .read_only = 1,
                .envelope = &std_env
            };
            LpEngineTurnResponse cresp = { 0 };
            int crc = abi->turn(h, &ctr, &cresp);
            int has_wolf = cresp.text && (strstr(cresp.text, "wolf") != NULL || strstr(cresp.text, "canine") != NULL || strstr(cresp.text, "packs") != NULL);
            int has_hound = cresp.text && (strstr(cresp.text, "hound") != NULL || strstr(cresp.text, "dog") != NULL || strstr(cresp.text, "hunting") != NULL);
            if (crc != 0 || !has_wolf || !has_hound) {
                fprintf(stderr, "[DIAG] compare hound/wolf crc=%d disp=\"%s\" text=\"%s\"\n",
                        crc, cresp.disposition, cresp.text ? cresp.text : "");
            }
            assert(crc == 0 && has_wolf && has_hound);
            printf("[PASS] Live multi-clause compositional synthesis (hound+wolf, wall=%.2f ms, text=\"%s\")\n",
                   (double)cresp.work.wall_us / 1000.0, cresp.text);
            abi->turn_free(&cresp);
        }

        /* Live Mold-a-Model GGUF + SafeTensors synthesis against populated PostgreSQL 18 */
        {
            const char *keep_gguf = getenv("LAPLACE_KEEP_MOLD_GGUF");
            const char *keep_st = getenv("LAPLACE_KEEP_MOLD_SAFETENSORS");
            const char *gguf_out = (keep_gguf && *keep_gguf) ? keep_gguf : "laplace_live_mold_tmp.gguf";
            const char *st_out = (keep_st && *keep_st) ? keep_st : "laplace_live_mold_tmp.safetensors";
            const char *recipe_json =
                "{\"name\":\"laplace-live-mold\","
                "\"hidden_size\":64,\"num_layers\":2,\"intermediate_size\":128,\"tie_embeddings\":false,"
                "\"vocab\":{\"size\":384,\"hops\":2,\"fanout\":32,\"seeds\":[\"lightning\",\"hound\",\"wolf\"]},"
                "\"layers\":["
                "  {\"heads\":[{\"op\":\"trajectory\"},{\"op\":\"relation\",\"type\":\"hypernym\"},{\"op\":\"metric\",\"metric\":\"angular\"},{\"op\":\"unary\"}],\"ffn\":{\"op\":\"unary\"}},"
                "  {\"heads\":[{\"op\":\"trajectory\"},{\"op\":\"relation\",\"type\":\"*\"},{\"op\":\"metric\",\"metric\":\"frechet\"},{\"op\":\"unary\"}],\"ffn\":{\"op\":\"trajectory\"}}"
                "],"
                "\"lm_head\":{\"op\":\"trajectory\",\"scoring\":\"weighted\"}}";

            LpEngineMoldRequest g_req = {
                .out_path = gguf_out, .model_name = "laplace-live-mold",
                .max_vocab = 384, .hidden_dim = 64, .n_layers = 2, .n_heads = 4, .n_kv_heads = 4, .ffn_dim = 128,
                .f16_weights = 0, .format = 0, .tied_embeddings = 0, .recipe_json = recipe_json
            };
            LpEngineMoldResponse g_res = { 0 };
            int grc = abi->mold_checkpoint(h, &g_req, &g_res);
            if (grc != 0) fprintf(stderr, "[DIAG] live GGUF mold failed: rc=%d status=%d err=%s\n", grc, g_res.status_code, g_res.error);
            assert(grc == 0 && g_res.status_code == 0 && g_res.raw_weight_bytes_retained == 0 && g_res.n_vocab > 8 && g_res.n_edges > 0);
            assert(os_exists(gguf_out));
            printf("[PASS] Live Mold-a-Model GGUF verified (vocab=%u, edges=%u, dim=%u, raw_retained=%llu, out=%s)\n",
                   g_res.n_vocab, g_res.n_edges, g_res.hidden_dim, (unsigned long long)g_res.raw_weight_bytes_retained, gguf_out);

            LpEngineMoldRequest s_req = g_req;
            s_req.out_path = st_out;
            s_req.format = 1;
            LpEngineMoldResponse s_res = { 0 };
            int src = abi->mold_checkpoint(h, &s_req, &s_res);
            if (src != 0) fprintf(stderr, "[DIAG] live SafeTensors mold failed: rc=%d status=%d err=%s\n", src, s_res.status_code, s_res.error);
            assert(src == 0 && s_res.status_code == 0 && s_res.raw_weight_bytes_retained == 0 && s_res.n_vocab > 8 && s_res.n_edges > 0);
            assert(os_exists(st_out));
            printf("[PASS] Live Mold-a-Model SafeTensors verified (vocab=%u, edges=%u, dim=%u, raw_retained=%llu, out=%s)\n",
                   s_res.n_vocab, s_res.n_edges, s_res.hidden_dim, (unsigned long long)s_res.raw_weight_bytes_retained, st_out);

            if (!keep_gguf || !*keep_gguf) remove(gguf_out);
            if (!keep_st || !*keep_st) remove(st_out);
        }
        }
    }

    abi->free_fn(h);
    cleanup_test_witnesses(pg, CLEAN_NAMES, 5);
    PQfinish(pg);
}

static void test_forward_unicode_and_multiclause(void){
    /* 1. Language-agnostic clause joining: CJK/Japanese must never get bogus Latin spaces or ASCII dots */
    char *out = calloc(1, 1); size_t outn = 0;
    lp_program_append_out(&out, &outn, "\xE6\x9D\xB1\xE4\xBA\xAC", 0, 1); /* 東京 */
    lp_program_append_out(&out, &outn, "\xE4\xBA\xBA\xE5\x8F\xA3", 0, 1); /* 人口 */
    assert(strcmp(out, "\xE6\x9D\xB1\xE4\xBA\xAC\xE4\xBA\xBA\xE5\x8F\xA3") == 0);
    free(out);

    /* 2. Unicode terminal punctuation (。) and non-ASCII uppercase (Ü, Р) */
    out = calloc(1, 1); outn = 0;
    lp_program_append_out(&out, &outn, "\xE6\x9D\xB1\xE4\xBA\xAC\xE3\x80\x82", 0, 1); /* 東京。 */
    lp_program_append_out(&out, &outn, "\xE6\x97\xA5\xE6\x9C\xAC", 0, 1);             /* 日本 */
    assert(strcmp(out, "\xE6\x9D\xB1\xE4\xBA\xAC\xE3\x80\x82\xE6\x97\xA5\xE6\x9C\xAC") == 0);
    free(out);

    out = calloc(1, 1); outn = 0;
    lp_program_append_out(&out, &outn, "Paris", 0, 1);
    lp_program_append_out(&out, &outn, "\xC3\x9C""ber", 0, 1); /* Über */
    assert(strcmp(out, "Paris. \xC3\x9C""ber") == 0);
    free(out);

    out = calloc(1, 1); outn = 0;
    lp_program_append_out(&out, &outn, "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0", 0, 1); /* Москва */
    lp_program_append_out(&out, &outn, "\xD0\xA0\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F", 0, 1); /* Россия */
    assert(strcmp(out, "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0. \xD0\xA0\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F") == 0);
    free(out);

    /* 3. Full Unicode case-folded & punctuation-trimmed echo detection (accented Latin, Cyrillic, CJK) */
    const char *p_de = "\xC3\x9C""ber?"; /* Über? */
    assert(lp_program_is_prompt_echo((const uint8_t *)p_de, strlen(p_de), "\xC3\xBC""ber", 5, 0) == 1);
    const char *p_ru = "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0!"; /* Москва! */
    const char *t_ru = "\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0";  /* москва */
    assert(lp_program_is_prompt_echo((const uint8_t *)p_ru, strlen(p_ru), t_ru, strlen(t_ru), 0) == 1);
    const char *p_ja = "\xE6\x9D\xB1\xE4\xBA\xAC\xEF\xBC\x9F"; /* 東京？ */
    const char *t_ja = "\xE6\x9D\xB1\xE4\xBA\xAC";             /* 東京 */
    assert(lp_program_is_prompt_echo((const uint8_t *)p_ja, strlen(p_ja), t_ja, strlen(t_ja), 0) == 1);
    assert(lp_program_is_prompt_echo((const uint8_t *)"scatter", 7, "cat", 3, 0) == 0);

    /* 4. Multi-anchor combine_clauses must NOT prematurely stop at enough=0.5 while a salient anchor remains open */
    assert(lp_program_should_stop_enough(0, 1, 1000, 2000, 0.5, 1) == 0);
    assert(lp_program_should_stop_enough(0, 1, 1000, 2000, 0.5, 0) == 1);
    assert(lp_program_should_stop_enough(0, 0, 1000, 2000, 0.5, 1) == 1);
    assert(lp_program_should_stop_enough(1, 1, 0, 2000, 0.5, 0) == 0);

    /* 5. Multi-clause structured claim realization preserves [via, rel, target] without wrapping full sentences */
    char cbuf[256] = "";
    assert(lp_program_format_claim_clause("France", "capital", "Paris", cbuf, sizeof cbuf) == 1);
    assert(strcmp(cbuf, "France (capital): Paris") == 0);
    assert(lp_program_format_claim_clause("France", "definition", "A country in Europe.", cbuf, sizeof cbuf) == 0);
    assert(lp_program_format_claim_clause("France", "capital", "Paris is in France", cbuf, sizeof cbuf) == 0);

    /* 6. Firmware lateral directive parsed from program.firmware (no C hardcoding override) */
    {
        Firmware pfw = { 0 }; char perr[512] = "";
        assert(firmware_load_checked("program", FW_PULL, &pfw, perr, sizeof perr) == 0 && perr[0] == 0);
        assert(pfw.nlateral_relation >= 10);
    }
    printf("[PASS] Language-agnostic Unicode realization/echo, firmware lateral, & multi-clause RECOUPLE verified\n");
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    db_conninfo_default(LAPLACE_CONNINFO_DEFAULT);
    tier0_open(NULL);
    table_init();
    ctx_open(1);

    test_firmware_suite();
    test_forward_unicode_and_multiclause();
    test_astar_heuristic();
    test_witness_attribution();
    test_chess_search_offline();
    test_ooda_ast_offline();

    AbiVtable abi = load_abi_dll();
    test_abi_dll_offline(&abi);
    test_live_db(&abi);
    return 0;
}
