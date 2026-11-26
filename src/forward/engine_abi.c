/* In-process fixed-width shared library ABI for Laplace-Engine (dotnet / MCP / native tests). */
#include "engine_abi.h"
#include "program_internal.h"
#include "ooda.h"
#include "codecs/codecs.h"
#include "chess/chess_search.h"
#include "laplace_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *abi_env_or(const char *name, const char *dflt){
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}
const char *laplace_recipes(void){ return abi_env_or("LAPLACE_RECIPES", LAPLACE_RECIPES_DEFAULT); }
const char *laplace_grammars(void){ return abi_env_or("LAPLACE_GRAMMARS", LAPLACE_GRAMMARS_DEFAULT); }
const char *laplace_ucd(void){ return abi_env_or("LAPLACE_UCD", LAPLACE_UCD_DEFAULT); }

struct LpEngineHandle {
    pthread_mutex_t mu;
    char           *conninfo;
    PGconn         *pg;
};

static int authority_preflight(const LpEngineAuthorityScope *auth, uint32_t need_cap, int want_write,
                               char *disp_out, size_t disp_cap, char *err_out, size_t err_cap){
    if (!auth) return 0;
    if (auth->disposition == LP_DISPOSE_REFUSE || auth->disposition == LP_DISPOSE_QUARANTINE) {
        snprintf(disp_out, disp_cap, "refused: authority disposition blocks execution");
        snprintf(err_out, err_cap, "%s", disp_out);
        return 2;
    }
    if ((auth->capabilities & need_cap) != need_cap) {
        snprintf(disp_out, disp_cap, "refused: insufficient authority capability (need 0x%x)", need_cap);
        snprintf(err_out, err_cap, "%s", disp_out);
        return 2;
    }
    if (want_write && !auth->allow_write) {
        snprintf(disp_out, disp_cap, "refused: authority scope forbids writes");
        snprintf(err_out, err_cap, "%s", disp_out);
        return 2;
    }
    if (auth->scoped && auth->nwitnesses == 0) {
        snprintf(disp_out, disp_cap, "refused: empty witness allowlist");
        snprintf(err_out, err_cap, "%s", disp_out);
        return 2;
    }
    return 0;
}

static void resolve_firmware_path(const LpEngineTurnRequest *req, char *buf, size_t cap){
    if (req->firmware_path && *req->firmware_path) {
        snprintf(buf, cap, "%s", req->firmware_path);
        return;
    }
    const char *base = firmware_path();
    if (req->model_name && *req->model_name &&
        (!strcmp(req->model_name, "coding") || !strcmp(req->model_name, "deductive") ||
         !strcmp(req->model_name, "inductive") || !strcmp(req->model_name, "abductive") ||
         !strcmp(req->model_name, "constructive") || !strcmp(req->model_name, "adversarial") ||
         !strcmp(req->model_name, "program"))) {
        snprintf(buf, cap, "%s", base);
        char *slash = strrchr(buf, '/'), *bslash = strrchr(buf, '\\');
        if (bslash > slash) slash = bslash;
        if (slash) {
            snprintf(slash + 1, cap - (size_t)(slash + 1 - buf), "%s.firmware", req->model_name);
            return;
        }
    }
    snprintf(buf, cap, "%s", base);
}

static PGconn *ensure_pg_locked(LpEngineHandle *h){
    if (!h || !h->conninfo || !*h->conninfo) return NULL;
    if (h->pg && PQstatus(h->pg) == CONNECTION_OK) return h->pg;
    if (h->pg) { PQreset(h->pg); if (PQstatus(h->pg) == CONNECTION_OK) return h->pg; PQfinish(h->pg); h->pg = NULL; }
    h->pg = db_try(h->conninfo);
    return h->pg;
}

LpEngineHandle *lp_engine_init(const char *conninfo, char *err, size_t err_cap){
    if (err && err_cap) err[0] = 0;
    db_conninfo_default(LAPLACE_CONNINFO_DEFAULT);
    tier0_open(NULL);
    table_init();
    ctx_open(1);

    LpEngineHandle *h = calloc(1, sizeof *h);
    if (!h) {
        if (err && err_cap) snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    pthread_mutex_init(&h->mu, NULL);
    if (conninfo && *conninfo) {
        h->conninfo = strdup(conninfo);
        char derr[256] = "";
        h->pg = db_try_err(conninfo, derr, sizeof derr);
        if (!h->pg) {
            if (err && err_cap) snprintf(err, err_cap, "%s", derr[0] ? derr : "failed to connect to PostgreSQL");
            free(h->conninfo);
            pthread_mutex_destroy(&h->mu);
            free(h);
            return NULL;
        }
        db_note(conninfo);
    }
    return h;
}

typedef struct {
    LpEngineTurnStep *items;
    uint32_t          count;
    uint32_t          cap;
} TurnStepCollector;

static void collect_turn_step(void *ctx, int step, const char *kind, const lp_id *id,
                              const char *text, double grounds, int continuity, double confidence){
    TurnStepCollector *c = (TurnStepCollector *)ctx;
    if (!c) return;
    if (c->count == c->cap) {
        uint32_t ncap = c->cap ? c->cap * 2u : 16u;
        LpEngineTurnStep *next = realloc(c->items, sizeof(LpEngineTurnStep) * (size_t)ncap);
        if (!next) return;
        c->items = next;
        c->cap = ncap;
    }
    LpEngineTurnStep *s = &c->items[c->count++];
    memset(s, 0, sizeof *s);
    s->step = step;
    s->continuity = continuity;
    if (id) s->entity_id = *id;
    s->grounds = grounds;
    s->confidence = confidence;
    if (kind && *kind) snprintf(s->kind, sizeof s->kind, "%s", kind);
    if (text && *text) {
        s->text_len = strlen(text);
        s->text = malloc(s->text_len + 1);
        if (s->text) memcpy(s->text, text, s->text_len + 1);
        else s->text_len = 0;
    }
}

int32_t lp_engine_turn(LpEngineHandle *h, const LpEngineTurnRequest *req, LpEngineTurnResponse *out){
    if (!h || !req || !out) return 2;
    memset(out, 0, sizeof *out);

    int32_t auth_rc = authority_preflight(req->authority, LP_CAP_READ | (req->read_only ? 0u : LP_CAP_ATTEST),
                                          !req->read_only, out->disposition, sizeof out->disposition, out->error, sizeof out->error);
    if (auth_rc) { out->status_code = auth_rc; return auth_rc; }

    char fw_path[4200];
    resolve_firmware_path(req, fw_path, sizeof fw_path);
    Firmware fw;
    char ferr[512] = "";
    int frc = firmware_load_checked(fw_path, FW_PULL, &fw, ferr, sizeof ferr);
    if (frc) {
        out->status_code = frc;
        snprintf(out->error, sizeof out->error, "%s", ferr);
        snprintf(out->disposition, sizeof out->disposition, "error: invalid firmware");
        return frc;
    }

    /* Unified OODA code mode when verify_cmd, code_path, or code_text is supplied */
    if (req->verify_cmd || req->code_path || req->code_text) {
        pthread_mutex_lock(&h->mu);
        PGconn *pg = ensure_pg_locked(h);
        LpOodaRequest oreq = {
            .conninfo = (!req->read_only && h->conninfo) ? h->conninfo : NULL,
            .pg = pg,
            .fw = &fw,
            .prompt = req->prompt,
            .code_path = req->code_path,
            .code_text = req->code_text,
            .code_len = req->code_len,
            .lang = req->code_lang,
            .verify_cmd = req->verify_cmd,
            .user = req->user,
            .session = req->session,
            .max_rounds = req->ooda_rounds,
            .write_file = req->write_file,
            .read_only = req->read_only
        };
        LpOodaResult ores = { 0 };
        int orc = lp_ooda_run(&oreq, &ores);
        pthread_mutex_unlock(&h->mu);

        out->status_code = orc;
        out->ooda_verified = ores.verified;
        out->ooda_unverified = ores.unverified_by_toolchain;
        out->ooda_rounds_run = ores.rounds_run;
        out->prompt_id = ores.source_id;
        out->response_id = ores.patched_id;
        out->culprit_id = ores.culprit_id;
        out->culprit_contrast = ores.culprit_contrast;
        out->patched_code = ores.patched_code;
        out->patched_len = ores.patched_len;
        ores.patched_code = NULL;
        out->text = ores.verifier_log ? ores.verifier_log : strdup(ores.disposition);
        out->text_len = strlen(out->text);
        ores.verifier_log = NULL;
        snprintf(out->disposition, sizeof out->disposition, "%s", ores.disposition);
        lp_ooda_result_free(&ores);
        return orc;
    }

    /* Extract prompt and multi-turn discourse from OpenAI messages[] when present */
    const char *raw_prompt = req->prompt;
    const char *sys_prefix = req->system_prompt;
    lp_id disc[MAXOCC]; int ndisc = 0;
    if (req->messages && req->nmessages > 0) {
        int last_user_idx = -1;
        for (uint32_t i = 0; i < req->nmessages; i++) {
            const LpEngineChatMessage *m = &req->messages[i];
            if (!m->role || !m->content || !*m->content) continue;
            if (!strcmp(m->role, "system") && !sys_prefix) { sys_prefix = m->content; continue; }
            if (!strcmp(m->role, "user")) { raw_prompt = m->content; last_user_idx = (int)i; }
        }
        for (uint32_t i = 0; i < req->nmessages && ndisc < MAXOCC; i++) {
            const LpEngineChatMessage *m = &req->messages[i];
            if (!m->role || !m->content || !*m->content) continue;
            if (!strcmp(m->role, "system") || (last_user_idx >= 0 && i == (uint32_t)last_user_idx)) continue;
            size_t got = 0;
            lp_id tmp[64];
            text_occurrences(CTX[0], (const uint8_t *)m->content, strlen(m->content), tmp, 64, &got);
            for (size_t k = 0; k < got && k < 64 && ndisc < MAXOCC; k++) {
                if (lp_tier0_codepoint(T0, &tmp[k]) < 0) disc[ndisc++] = tmp[k];
            }
        }
    }
    if (!raw_prompt || !*raw_prompt) {
        out->status_code = 2;
        snprintf(out->error, sizeof out->error, "turn request contains no prompt or user message");
        snprintf(out->disposition, sizeof out->disposition, "error: empty prompt");
        return 2;
    }

    char *combined_prompt = NULL;
    const char *eff_prompt = raw_prompt;
    if (sys_prefix && *sys_prefix && strlen(raw_prompt) < 128) {
        size_t sl = strlen(sys_prefix), pl = strlen(raw_prompt);
        combined_prompt = malloc(sl + pl + 4);
        snprintf(combined_prompt, sl + pl + 4, "%s %s", sys_prefix, raw_prompt);
        eff_prompt = combined_prompt;
    }

    lp_envelope env = req->envelope ? *req->envelope : lp_envelope_preset(LP_ENVELOPE_STANDARD);
    if (req->max_tokens > 0 && (env.max_tokens_out == 0 || (uint32_t)req->max_tokens < env.max_tokens_out))
        env.max_tokens_out = (uint32_t)req->max_tokens;

    pthread_mutex_lock(&h->mu);
    PGconn *pg = ensure_pg_locked(h);
    if (!pg) {
        pthread_mutex_unlock(&h->mu);
        free(combined_prompt);
        out->status_code = 1;
        snprintf(out->error, sizeof out->error, "no database connection");
        snprintf(out->disposition, sizeof out->disposition, "error: offline");
        return 1;
    }

    TurnStepCollector steps_col = { 0 };
    LpForwardRequest freq = {
        .pg = pg,
        .conninfo = (!req->read_only && h->conninfo) ? h->conninfo : NULL,
        .fw = &fw,
        .prompt_text = (const uint8_t *)eff_prompt,
        .prompt_len = strlen(eff_prompt),
        .user = req->user ? req->user : "user",
        .session = req->session ? req->session : "stateless",
        .responder = "Laplace",
        .seed_value = req->seed,
        .read_only = req->read_only || !(req->authority && req->authority->allow_write),
        .max_emit = req->max_tokens,
        .temperature = req->temperature,
        .stop_sequences = req->stop_sequences,
        .nstop_sequences = (int)req->nstop_sequences,
        .witness_scope = req->authority ? req->authority->witnesses : NULL,
        .nwitness_scope = req->authority ? (int)req->authority->nwitnesses : 0,
        .witness_scoped = req->authority ? req->authority->scoped : 0,
        .envelope = &env,
        .discourse = ndisc > 0 ? disc : NULL,
        .ndiscourse = ndisc,
        .on_step = collect_turn_step,
        .on_step_ctx = &steps_col
    };
    LpForwardResult fres = { 0 };
    int rc = lp_forward_execute(&freq, &fres);

    /* Integer-conserved Fisher attribution over emitted tokens when DB attested them */
    if (rc == 0 && fres.n_emitted > 0 && pg) {
        int natt = 0;
        Attested *att = attested(pg, fres.emitted, fres.n_emitted, &natt, NULL);
        if (att && natt > 0) {
            LpWitnessShare wsh[64];
            int nw = witness_attribution_bp(att, natt,
                req->authority ? req->authority->witnesses : NULL,
                req->authority ? (int)req->authority->nwitnesses : 0,
                req->authority ? req->authority->scoped : 0, wsh, 64);
            if (nw > 0) {
                out->shares = calloc((size_t)nw, sizeof(LpEngineWitnessShare));
                out->nshares = (uint32_t)nw;
                for (int i = 0; i < nw; i++) {
                    out->shares[i].witness = wsh[i].witness;
                    out->shares[i].trust = wsh[i].trust;
                    out->shares[i].fisher_info = wsh[i].fisher_info;
                    out->shares[i].games = wsh[i].games;
                    out->shares[i].share_bp = wsh[i].share_bp;
                }
            }
        }
        free(att);
    }
    pthread_mutex_unlock(&h->mu);

    free(combined_prompt);
    out->status_code = rc;
    out->firmware_id = fres.firmware_id;
    out->program_id = fres.program_id;
    out->decision_id = fres.decision_id;
    out->prompt_id = fres.prompt_id;
    out->response_id = fres.response_id;
    out->turn_id = fres.turn_id;
    out->top_salience = fres.top_salience;
    out->mean_confidence = fres.mean_confidence;
    out->work = fres.work;
    out->text = fres.text;
    out->text_len = fres.text_len;
    fres.text = NULL;
    out->ordinal = fres.ordinal > 0 ? fres.ordinal : 1;
    out->turns_before = fres.turns_before;
    out->steps = steps_col.items;
    out->nsteps = steps_col.count;
    snprintf(out->disposition, sizeof out->disposition, "%s", fres.disposition);
    snprintf(out->error, sizeof out->error, "%s", fres.error);
    lp_forward_result_free(&fres);
    return rc;
}

void lp_engine_turn_response_free(LpEngineTurnResponse *out){
    if (!out) return;
    free(out->text);
    free(out->patched_code);
    free(out->shares);
    if (out->steps) {
        for (uint32_t i = 0; i < out->nsteps; i++) free(out->steps[i].text);
        free(out->steps);
    }
    memset(out, 0, sizeof *out);
}

int32_t lp_engine_chess_play(LpEngineHandle *h, const LpEngineChessRequest *req, LpEngineChessResponse *out){
    if (!req || !out || !req->fen || !*req->fen) return 2;
    memset(out, 0, sizeof *out);

    char dummy_disp[160] = "";
    int32_t auth_rc = authority_preflight(req->authority, LP_CAP_READ, 0,
                                          dummy_disp, sizeof dummy_disp, out->error, sizeof out->error);
    if (auth_rc) { out->status_code = auth_rc; return auth_rc; }

    LpChessSearchPolicy pol = lp_chess_search_policy();
    if (req->max_depth > 0) pol.max_depth = req->max_depth;
    if (req->quiesce_depth > 0) pol.quiesce_depth = req->quiesce_depth;
    if (req->max_nodes > 0) pol.max_nodes = req->max_nodes;
    if (req->max_wall_us > 0) pol.max_wall_us = req->max_wall_us;

    PGconn *pg = NULL;
    if (h) { pthread_mutex_lock(&h->mu); pg = ensure_pg_locked(h); }
    LpChessSearchResult sr = { 0 };
    int rc = lp_chess_search(pg, req->fen, req->variant,
        req->authority ? req->authority->witnesses : NULL,
        req->authority ? (int)req->authority->nwitnesses : 0,
        req->authority ? req->authority->scoped : 0,
        &pol, &sr);
    if (h) pthread_mutex_unlock(&h->mu);

    out->status_code = rc;
    out->score_cp = sr.score_cp;
    out->depth_reached = sr.depth_reached;
    out->syzygy_wdl = sr.syzygy_wdl;
    out->from_syzygy = sr.from_syzygy;
    out->nodes_searched = sr.nodes_searched;
    out->wall_us = sr.wall_us;
    out->db_probes = sr.db_probes;
    out->position_id = sr.position_id;
    out->move_id = sr.move_id;
    out->next_position_id = sr.next_position_id;
    out->claim_id = sr.claim_id;
    memcpy(out->best_uci, sr.best_uci, sizeof out->best_uci);
    memcpy(out->best_san, sr.best_san, sizeof out->best_san);
    memcpy(out->source, sr.source, sizeof out->source);
    if (rc) snprintf(out->error, sizeof out->error, "chess search failed (rc=%d)", rc);
    return rc;
}

int32_t lp_engine_mold_checkpoint(LpEngineHandle *h, const LpEngineMoldRequest *req, LpEngineMoldResponse *out){
    if (!req || !out || !req->out_path || !*req->out_path) return 2;
    memset(out, 0, sizeof *out);

    char dummy_disp[160] = "";
    int32_t auth_rc = authority_preflight(req->authority, LP_CAP_READ | LP_CAP_EXPORT, 0,
                                          dummy_disp, sizeof dummy_disp, out->error, sizeof out->error);
    if (auth_rc) { out->status_code = auth_rc; return auth_rc; }

    if (req->format != 0 && req->format != 1) {
        out->status_code = 2;
        snprintf(out->error, sizeof out->error, "unsupported mold export format %d (expected 0=GGUF or 1=SafeTensors)", req->format);
        return 2;
    }

    uint32_t max_vocab = req->max_vocab ? req->max_vocab : 1024u;
    if (max_vocab < 16u) max_vocab = 16u;
    uint32_t hidden_dim = req->hidden_dim >= 16u ? req->hidden_dim : 64u;
    uint32_t n_layers = req->n_layers ? req->n_layers : 4u;
    uint32_t n_heads = req->n_heads ? req->n_heads : 4u;
    uint32_t n_kv_heads = req->n_kv_heads ? req->n_kv_heads : n_heads;
    if ((n_heads % n_kv_heads) != 0) {
        out->status_code = 2;
        snprintf(out->error, sizeof out->error, "n_heads (%u) must be divisible by n_kv_heads (%u)", n_heads, n_kv_heads);
        return 2;
    }
    uint32_t ffn_dim = req->ffn_dim ? req->ffn_dim : (hidden_dim * 4u);

    lp_idmap *vmap = lp_idmap_new();
    size_t seeded_count = (max_vocab >= 320u) ? 259u : 3u;
    char **seeded_strs = calloc(seeded_count, sizeof(char *));
    const lp_tier0_record *t0_seed = T0 ? T0 : lp_tier0_map(NULL);
    lp_text *tx_seed = t0_seed ? lp_text_new(t0_seed) : NULL;
    for (size_t si = 0; si < seeded_count; si++) {
        char buf[16];
        if (si == 0) snprintf(buf, sizeof buf, "<unk>");
        else if (si == 1) snprintf(buf, sizeof buf, "<s>");
        else if (si == 2) snprintf(buf, sizeof buf, "</s>");
        else snprintf(buf, sizeof buf, "<0x%02X>", (unsigned)(si - 3u));
        seeded_strs[si] = strdup(buf);
        lp_id sid;
        if (tx_seed) {
            lp_ref sr = lp_text_decompose(tx_seed, (const uint8_t *)buf, strlen(buf), NULL, NULL);
            sid = sr.id;
        } else {
            memset(&sid, 0, sizeof sid);
            sid.b[0] = (uint8_t)(si & 0xFFu);
            sid.b[1] = (uint8_t)(((si >> 8) & 0x7Fu) | 0x80u);
        }
        lp_idmap_put(vmap, &sid, NULL);
    }
    if (tx_seed) lp_text_free(tx_seed);

    size_t cap_e = (size_t)max_vocab * 4u, n_edges = 0;
    if (cap_e < 512u) cap_e = 512u;
    int *rows = malloc(sizeof(int) * cap_e);
    int *cols = malloc(sizeof(int) * cap_e);
    double *weights = malloc(sizeof(double) * cap_e);
    char **wlabels = malloc(sizeof(char *) * cap_e);

    if (h) {
        pthread_mutex_lock(&h->mu);
        PGconn *pg = ensure_pg_locked(h);
        if (pg) {
            Args a = { 0 };
            int32_t row_limit = (int32_t)(max_vocab < 2048u ? max_vocab : 2048u);
            arg_i32(&a, row_limit);
            int scoped = (req->authority && req->authority->scoped && req->authority->witnesses && req->authority->nwitnesses > 0);
            if (scoped) arg_ids(&a, req->authority->witnesses, (size_t)req->authority->nwitnesses);
            PGresult *q = db_call_try(pg, scoped ? LP_SQL_MOLD_CLAIMS_SCOPED : LP_SQL_MOLD_CLAIMS, &a, out->error, sizeof out->error);
            args_free(&a);
            if (q) {
                int nr = PQntuples(q);
                for (int r = 0; r < nr && n_edges < cap_e; r++) {
                    lp_path path = lp_path_of((const uint8_t *)PQgetvalue(q, r, 1), (size_t)PQgetlength(q, r, 1));
                    lp_rating lr = lp_rating_stock();
                    lr.rating = col_f64(q, r, 2);
                    lr.deviation = col_f64(q, r, 3);
                    double score = lp_confidence(&lr, 2.0);
                    if (!(score > 0.0)) score = 0.5;
                    const lp_id *wid = col_id(q, r, 4);
                    if (path.n < 2) continue;
                    Ids pts = { 0 };
                    path_into(&pts, path);
                    if (pts.n >= 2) {
                        for (size_t k = 0; k + 1 < pts.n && n_edges < cap_e; k++) {
                            if (lp_id_eq(&pts.v[k], &pts.v[k + 1])) continue;
                            if (lp_idmap_find(vmap, &pts.v[k]) < 0 && lp_idmap_count(vmap) >= max_vocab) continue;
                            size_t i0 = lp_idmap_put(vmap, &pts.v[k], NULL);
                            if (lp_idmap_find(vmap, &pts.v[k + 1]) < 0 && lp_idmap_count(vmap) >= max_vocab) continue;
                            size_t i1 = lp_idmap_put(vmap, &pts.v[k + 1], NULL);
                            rows[n_edges] = (int)i0;
                            cols[n_edges] = (int)i1;
                            weights[n_edges] = score;
                            wlabels[n_edges] = malloc(33);
                            lp_id_hex(wid, wlabels[n_edges]);
                            n_edges++;
                        }
                    }
                    lp_vec_free(&pts);
                    if (lp_idmap_count(vmap) >= max_vocab) break;
                }
                PQclear(q);
            }
        }
        pthread_mutex_unlock(&h->mu);
    }

    /* Realize DB vocabulary tokens or seed deterministic Tier-0 vocabulary when empty */
    size_t nv = lp_idmap_count(vmap);
    char **tokens = NULL;
    lp_id *token_ids = NULL;
    if (nv > seeded_count && n_edges > 0) {
        token_ids = malloc(sizeof(lp_id) * nv);
        tokens = malloc(sizeof(char *) * nv);
        memcpy(token_ids, lp_idmap_keys(vmap), sizeof(lp_id) * nv);
        for (size_t i = 0; i < seeded_count && i < nv; i++)
            tokens[i] = strdup(seeded_strs[i]);
        if (h) {
            pthread_mutex_lock(&h->mu);
            PGconn *pg = ensure_pg_locked(h);
            Reader *rd = pg ? reader_new(pg) : NULL;
            if (rd) {
                for (size_t i = seeded_count; i < nv; i++) reader_want(rd, &token_ids[i]);
                for (size_t i = seeded_count; i < nv; i++) tokens[i] = reader_text(rd, &token_ids[i], 64);
                reader_free(rd);
            } else {
                for (size_t i = seeded_count; i < nv; i++) { tokens[i] = malloc(33); lp_id_hex(&token_ids[i], tokens[i]); }
            }
            pthread_mutex_unlock(&h->mu);
        } else {
            for (size_t i = seeded_count; i < nv; i++) { tokens[i] = malloc(33); lp_id_hex(&token_ids[i], tokens[i]); }
        }
    } else {
        static const char *FALLBACK[17] = {
            "<unk>", "<s>", "</s>", "laplace", "salience", "geometry", "manifold", "curve", "witness",
            "consensus", "token", "merkle", "blake3", "ooda", "verify", "proof", "synthesis"
        };
        for (size_t i = 0; i < n_edges; i++) free(wlabels[i]);
        nv = 17; n_edges = 0;
        tokens = malloc(sizeof(char *) * nv);
        for (size_t i = 0; i < nv; i++) tokens[i] = strdup(FALLBACK[i]);
        for (size_t i = 3; i + 1 < nv && n_edges < cap_e; i++) {
            rows[n_edges] = (int)i;
            cols[n_edges] = (int)(i + 1);
            weights[n_edges] = 1.0;
            wlabels[n_edges] = strdup("laplace-seed");
            n_edges++;
        }
    }
    for (size_t si = 0; si < seeded_count; si++) free(seeded_strs[si]);
    free(seeded_strs);
    lp_idmap_free(vmap);

    LpMoldSpec spec = {
        .name = (req->model_name && *req->model_name) ? req->model_name : "laplace-molded",
        .out_path = req->out_path,
        .n_vocab = nv,
        .tokens = (const char **)tokens,
        .token_ids = token_ids,
        .bos_id = 1,
        .eos_id = nv > 2 ? 2 : 0,
        .n_edges = n_edges,
        .rows = rows,
        .cols = cols,
        .weights = weights,
        .edge_witnesses = (const char **)wlabels,
        .hidden_dim = hidden_dim,
        .n_layers = n_layers,
        .n_heads = n_heads,
        .n_kv_heads = n_kv_heads,
        .ffn_dim = ffn_dim,
        .rms_eps = 1e-5,
        .rope_base = 10000.0,
        .f16_weights = req->f16_weights,
        .format = req->format,
        .tied_embeddings = req->tied_embeddings ? 1 : 0
    };

    int rc = lp_mold_export(&spec);
    for (size_t i = 0; i < nv; i++) free(tokens[i]);
    for (size_t i = 0; i < n_edges; i++) free(wlabels[i]);
    free(tokens); free(token_ids); free(rows); free(cols); free(weights); free(wlabels);

    out->status_code = rc;
    out->n_vocab = (uint32_t)nv;
    out->n_edges = (uint32_t)n_edges;
    out->hidden_dim = hidden_dim;
    out->raw_weight_bytes_retained = 0;
    snprintf(out->out_path, sizeof out->out_path, "%s", req->out_path);
    if (rc) snprintf(out->error, sizeof out->error, "lp_mold_export failed (rc=%d)", rc);
    return rc;
}

void lp_engine_free(LpEngineHandle *h){
    if (!h) return;
    pthread_mutex_lock(&h->mu);
    if (h->pg) { PQfinish(h->pg); h->pg = NULL; }
    free(h->conninfo); h->conninfo = NULL;
    pthread_mutex_unlock(&h->mu);
    pthread_mutex_destroy(&h->mu);
    free(h);
}
