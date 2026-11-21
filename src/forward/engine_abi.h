#ifndef LAPLACE_ENGINE_ABI_H
#define LAPLACE_ENGINE_ABI_H

#include "laplace/laplace.h"
#include "laplace/consensus.h"
#include "laplace/envelope.h"
#include <stddef.h>
#include <stdint.h>

#ifndef LP_ENGINE_API
#  ifdef _WIN32
#    ifdef LP_ENGINE_ABI_EXPORTS
#      define LP_ENGINE_API __declspec(dllexport)
#    else
#      define LP_ENGINE_API __declspec(dllimport)
#    endif
#  else
#    define LP_ENGINE_API __attribute__((visibility("default")))
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LpEngineHandle LpEngineHandle;

typedef struct {
    const lp_id *witnesses;
    uint32_t     nwitnesses;
    int32_t      scoped;
    uint32_t     capabilities;
    int32_t      disposition;
    double       min_trust;
    int32_t      allow_write;
    int32_t      reserved;
} LpEngineAuthorityScope;

typedef struct {
    const char *role;
    const char *content;
} LpEngineChatMessage;

typedef struct {
    const char                   *firmware_path;
    const char                   *model_name;
    const char                   *prompt;
    const LpEngineChatMessage    *messages;
    uint32_t                      nmessages;
    const char                   *system_prompt;
    const char                   *user;
    const char                   *session;
    int32_t                       read_only;
    int32_t                       max_tokens;
    double                        temperature;
    const char *const            *stop_sequences;
    uint32_t                      nstop_sequences;
    int64_t                       seed;
    const LpEngineAuthorityScope *authority;
    const lp_envelope            *envelope;
    const char                   *code_path;
    const char                   *code_text;
    size_t                        code_len;
    const char                   *code_lang;
    const char                   *verify_cmd;
    int32_t                       ooda_rounds;
    int32_t                       write_file;
} LpEngineTurnRequest;

typedef struct {
    lp_id    witness;
    double   trust;
    double   fisher_info;
    uint64_t games;
    int32_t  share_bp;
    int32_t  reserved;
} LpEngineWitnessShare;

typedef struct {
    int32_t               status_code;
    int32_t               ooda_verified;
    int32_t               ooda_unverified;
    int32_t               ooda_rounds_run;
    lp_id                 firmware_id;
    lp_id                 program_id;
    lp_id                 decision_id;
    lp_id                 prompt_id;
    lp_id                 response_id;
    lp_id                 turn_id;
    lp_id                 culprit_id;
    double                culprit_contrast;
    double                top_salience;
    double                mean_confidence;
    lp_work_counters      work;
    char                 *text;
    size_t                text_len;
    char                 *patched_code;
    size_t                patched_len;
    char                  disposition[160];
    char                  error[512];
    LpEngineWitnessShare *shares;
    uint32_t              nshares;
    int32_t               reserved;
} LpEngineTurnResponse;

typedef struct {
    const char                   *fen;
    const char                   *variant;
    const LpEngineAuthorityScope *authority;
    int32_t                       max_depth;
    int32_t                       quiesce_depth;
    uint64_t                      max_nodes;
    uint64_t                      max_wall_us;
} LpEngineChessRequest;

typedef struct {
    int32_t  status_code;
    int32_t  score_cp;
    int32_t  depth_reached;
    int32_t  syzygy_wdl;
    int32_t  from_syzygy;
    int32_t  reserved;
    uint64_t nodes_searched;
    uint64_t wall_us;
    uint64_t db_probes;
    lp_id    position_id;
    lp_id    move_id;
    lp_id    next_position_id;
    lp_id    claim_id;
    char     best_uci[8];
    char     best_san[16];
    char     source[24];
    char     error[256];
} LpEngineChessResponse;

typedef struct {
    const char                   *out_path;
    const char                   *model_name;
    const LpEngineAuthorityScope *authority;
    uint32_t                      max_vocab;
    uint32_t                      hidden_dim;
    uint32_t                      n_layers;
    uint32_t                      n_heads;
    int32_t                       f16_weights;
    int32_t                       reserved;
} LpEngineMoldRequest;

typedef struct {
    int32_t  status_code;
    uint32_t n_vocab;
    uint32_t n_edges;
    uint32_t hidden_dim;
    uint64_t raw_weight_bytes_retained;
    char     out_path[512];
    char     error[256];
} LpEngineMoldResponse;

LP_ENGINE_API LpEngineHandle *lp_engine_init(const char *conninfo, char *err, size_t err_cap);
LP_ENGINE_API int32_t lp_engine_turn(LpEngineHandle *h, const LpEngineTurnRequest *req, LpEngineTurnResponse *out);
LP_ENGINE_API void    lp_engine_turn_response_free(LpEngineTurnResponse *out);
LP_ENGINE_API int32_t lp_engine_chess_play(LpEngineHandle *h, const LpEngineChessRequest *req, LpEngineChessResponse *out);
LP_ENGINE_API int32_t lp_engine_mold_checkpoint(LpEngineHandle *h, const LpEngineMoldRequest *req, LpEngineMoldResponse *out);
LP_ENGINE_API void    lp_engine_free(LpEngineHandle *h);

#ifdef __cplusplus
}
#endif
#endif
