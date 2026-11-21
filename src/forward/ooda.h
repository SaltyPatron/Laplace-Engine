#ifndef LAPLACE_OODA_H
#define LAPLACE_OODA_H

#include "query.h"

typedef struct {
    const char *conninfo;
    PGconn     *pg;
    Firmware   *fw;
    const char *prompt;
    const char *code_path;
    const char *code_text;
    size_t      code_len;
    const char *lang;
    const char *verify_cmd;
    const char *witness_name;
    const char *session;
    const char *user;
    int         max_rounds;
    int         write_file;
    int         read_only;
} LpOodaRequest;

typedef struct {
    lp_id    source_id;
    lp_id    patched_id;
    lp_id    culprit_id;
    double   culprit_contrast;
    uint32_t culprit_start;
    uint32_t culprit_end;
    int      rounds_run;
    int      verified;
    int      unverified_by_toolchain;
    int      exit_code;
    char    *patched_code;
    size_t   patched_len;
    char    *verifier_log;
    char     disposition[160];
} LpOodaResult;

int  lp_ooda_run(const LpOodaRequest *req, LpOodaResult *out);
void lp_ooda_result_free(LpOodaResult *out);

int  lp_ooda_decompose_ast(const char *src, size_t len, const char *lang,
                           Ref *root_out, Ref **nodes_out, uint32_t **starts_out, uint32_t **ends_out, size_t *count_out);
int  lp_ooda_splice(const char *src, size_t len, uint32_t start_byte, uint32_t end_byte,
                    const char *delta, size_t delta_len, char **out_buf, size_t *out_len);

#endif
