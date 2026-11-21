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
        for (uint32_t i = 0; i < req->nmessages; i++) {
            const LpEngineChatMessage *m = &req->messages[i];
            if (!m->role || !m->content || !*m->content) continue;
            if (!strcmp(m->role, "system") && !sys_prefix) { sys_prefix = m->content; continue; }
            if (!strcmp(m->role, "user")) raw_prompt = m->content;
            if (ndisc < MAXOCC) {
                size_t got = 0;
                lp_id tmp[64];
                text_occurrences(CTX[0], (const uint8_t *)m->content, strlen(m->content), tmp, 64, &got);
                for (size_t k = 0; k < got && k < 64 && ndisc < MAXOCC; k++) disc[ndisc++] = tmp[k];
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
        .ndiscourse = ndisc
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

    uint32_t max_vocab = req->max_vocab ? req->max_vocab : 1024u;
    if (max_vocab < 16u) max_vocab = 16u;
    uint32_t hidden_dim = req->hidden_dim >= 16u ? req->hidden_dim : 64u;
    uint32_t n_layers = req->n_layers ? req->n_layers : 4u;
    uint32_t n_heads = req->n_heads ? req->n_heads : 4u;

    lp_idmap *vmap = lp_idmap_new();
    size_t cap_e = 512, n_edges = 0;
    int *rows = malloc(sizeof(int) * cap_e);
    int *cols = malloc(sizeof(int) * cap_e);
    double *weights = malloc(sizeof(double) * cap_e);
    char **wlabels = malloc(sizeof(char *) * cap_e);

    if (h) {
        pthread_mutex_lock(&h->mu);
        PGconn *pg = ensure_pg_locked(h);
        if (pg) {
            Args a = { 0 }; arg_bool(&a, false);
            PGresult *q = db_call_try(pg, LP_SQL_REPLAY_EXPORT, &a, out->error, sizeof out->error);
            args_free(&a);
            if (q) {
                int nr = PQntuples(q);
                int lim = nr < 256 ? nr : 256;
                lp_id *cids = malloc(sizeof(lp_id) * (size_t)(lim ? lim : 1));
                lp_id *wids = malloc(sizeof(lp_id) * (size_t)(lim ? lim : 1));
                double *scores = malloc(sizeof(double) * (size_t)(lim ? lim : 1));
                for (int r = 0; r < lim; r++) {
                    cids[r] = *col_id(q, r, 0);
                    wids[r] = *col_id(q, r, 1);
                    scores[r] = col_f64(q, r, 4) > 0.0 ? col_f64(q, r, 4) : 1.0;
                }
                PQclear(q);
                int nh = 0;
                Hold *hh = lim ? paths_of(pg, cids, lim, &nh) : NULL;
                Ids pts = { 0 };
                for (int i = 0; i < nh && n_edges < cap_e; i++) {
                    path_into(&pts, lp_path_of(hh[i].path, (size_t)hh[i].path_len));
                    if (pts.n < 2) continue;
                    size_t i0 = lp_idmap_put(vmap, &pts.v[0], NULL);
                    size_t i1 = lp_idmap_put(vmap, &pts.v[pts.n - 1], NULL);
                    if (lp_idmap_count(vmap) >= max_vocab) break;
                    rows[n_edges] = (int)i0;
                    cols[n_edges] = (int)i1;
                    int src = hh[i].src >= 0 && hh[i].src < lim ? hh[i].src : 0;
                    weights[n_edges] = scores[src];
                    wlabels[n_edges] = malloc(33);
                    lp_id_hex(&wids[src], wlabels[n_edges]);
                    n_edges++;
                }
                lp_vec_free(&pts);
                holds_free(hh, nh);
                free(cids); free(wids); free(scores);
            }
        }
        pthread_mutex_unlock(&h->mu);
    }

    /* Realize DB vocabulary tokens or seed deterministic Tier-0 vocabulary when empty */
    size_t nv = lp_idmap_count(vmap);
    char **tokens = NULL;
    lp_id *token_ids = NULL;
    if (nv >= 8 && n_edges > 0) {
        token_ids = malloc(sizeof(lp_id) * nv);
        tokens = malloc(sizeof(char *) * nv);
        memcpy(token_ids, lp_idmap_keys(vmap), sizeof(lp_id) * nv);
        if (h) {
            pthread_mutex_lock(&h->mu);
            PGconn *pg = ensure_pg_locked(h);
            Reader *rd = pg ? reader_new(pg) : NULL;
            if (rd) {
                for (size_t i = 0; i < nv; i++) reader_want(rd, &token_ids[i]);
                for (size_t i = 0; i < nv; i++) tokens[i] = reader_text(rd, &token_ids[i], 64);
                reader_free(rd);
            } else {
                for (size_t i = 0; i < nv; i++) { tokens[i] = malloc(33); lp_id_hex(&token_ids[i], tokens[i]); }
            }
            pthread_mutex_unlock(&h->mu);
        } else {
            for (size_t i = 0; i < nv; i++) { tokens[i] = malloc(33); lp_id_hex(&token_ids[i], tokens[i]); }
        }
    } else {
        static const char *FALLBACK[16] = {
            "<bos>", "<eos>", "laplace", "salience", "geometry", "manifold", "curve", "witness",
            "consensus", "token", "merkle", "blake3", "ooda", "verify", "proof", "synthesis"
        };
        for (size_t i = 0; i < n_edges; i++) free(wlabels[i]);
        nv = 16; n_edges = 0;
        tokens = malloc(sizeof(char *) * nv);
        for (size_t i = 0; i < nv; i++) tokens[i] = strdup(FALLBACK[i]);
        for (size_t i = 2; i + 1 < nv && n_edges < cap_e; i++) {
            rows[n_edges] = (int)i;
            cols[n_edges] = (int)(i + 1);
            weights[n_edges] = 1.0;
            wlabels[n_edges] = strdup("laplace-seed");
            n_edges++;
        }
    }
    lp_idmap_free(vmap);

    LpMoldSpec spec = {
        .name = (req->model_name && *req->model_name) ? req->model_name : "laplace-molded",
        .out_path = req->out_path,
        .n_vocab = nv,
        .tokens = (const char **)tokens,
        .token_ids = token_ids,
        .bos_id = 0,
        .eos_id = nv > 1 ? 1 : 0,
        .n_edges = n_edges,
        .rows = rows,
        .cols = cols,
        .weights = weights,
        .edge_witnesses = (const char **)wlabels,
        .hidden_dim = hidden_dim,
        .n_layers = n_layers,
        .n_heads = n_heads,
        .n_kv_heads = n_heads,
        .ffn_dim = hidden_dim * 4u,
        .rms_eps = 1e-5,
        .rope_base = 10000.0,
        .f16_weights = req->f16_weights
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
