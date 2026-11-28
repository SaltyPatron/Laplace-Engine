/* Unified closed-loop OODA (Talking = InternalMonologue 0.10 = ToolchainVerification 0.90 Coding). */
#include "ooda.h"
#include "codecs/codecs.h"
#include "ingest/structure.h"
#include "laplace/os.h"
#include <tree_sitter/api.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_whole_file(const char *path, size_t *len_out){
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = sz > 0 ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

static int write_whole_file(const char *path, const char *buf, size_t len){
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t put = len > 0 ? fwrite(buf, 1, len, f) : 0;
    int close_rc = fclose(f);
    return (put == len && close_rc == 0) ? 0 : -1;
}

int lp_ooda_splice(const char *src, size_t len, uint32_t start_byte, uint32_t end_byte,
                   const char *delta, size_t delta_len, char **out_buf, size_t *out_len){
    if (!src || !out_buf || !out_len) return 1;
    if (start_byte > len) start_byte = (uint32_t)len;
    if (end_byte > len) end_byte = (uint32_t)len;
    if (end_byte < start_byte) end_byte = start_byte;
    if (!delta) delta_len = 0;
    size_t next_len = (size_t)start_byte + delta_len + (len - (size_t)end_byte);
    char *next = malloc(next_len + 1);
    if (!next) return 1;
    if (start_byte > 0) memcpy(next, src, start_byte);
    if (delta_len > 0) memcpy(next + start_byte, delta, delta_len);
    if (end_byte < len) memcpy(next + start_byte + delta_len, src + end_byte, len - end_byte);
    next[next_len] = 0;
    *out_buf = next;
    *out_len = next_len;
    return 0;
}

static void collect_ts_nodes(TSNode node, const char *src, size_t src_len, int depth,
                             Ref **nodes, uint32_t **starts, uint32_t **ends, size_t *n, size_t *cap){
    if (ts_node_is_null(node) || depth > 8 || *n >= 128) return;
    uint32_t sb = ts_node_start_byte(node), eb = ts_node_end_byte(node);
    if (eb > sb && eb <= src_len) {
        const char *tp = ts_node_type(node);
        int keep = ts_node_is_named(node) && (
            !strcmp(tp, "ERROR") ||
            !strcmp(tp, "function_definition") ||
            !strcmp(tp, "declaration") ||
            !strcmp(tp, "return_statement") ||
            !strcmp(tp, "expression_statement") ||
            !strcmp(tp, "call_expression") ||
            !strcmp(tp, "assignment_expression") ||
            !strcmp(tp, "string_literal") ||
            !strcmp(tp, "number_literal") ||
            !strcmp(tp, "identifier") ||
            !strcmp(tp, "primitive_type") ||
            !strcmp(tp, "type_identifier"));
        if (keep && depth > 0) {
            if (*n == *cap) {
                *cap = *cap ? *cap * 2 : 32;
                *nodes = xrealloc(*nodes, *cap * sizeof(Ref));
                *starts = xrealloc(*starts, *cap * sizeof(uint32_t));
                *ends = xrealloc(*ends, *cap * sizeof(uint32_t));
            }
            (*nodes)[*n] = text_ref(CTX[0], (const uint8_t *)(src + sb), (size_t)(eb - sb));
            (*starts)[*n] = sb;
            (*ends)[*n] = eb;
            (*n)++;
        }
    }
    uint32_t nc = ts_node_child_count(node);
    for (uint32_t i = 0; i < nc && *n < 128; i++) {
        collect_ts_nodes(ts_node_child(node, i), src, src_len, depth + 1, nodes, starts, ends, n, cap);
    }
}

int lp_ooda_decompose_ast(const char *src, size_t len, const char *lang,
                          Ref *root_out, Ref **nodes_out, uint32_t **starts_out, uint32_t **ends_out, size_t *count_out){
    if (!src || !root_out || !nodes_out || !starts_out || !ends_out || !count_out) return 1;
    tier0_open(NULL); table_init(); ctx_open(1);

    Ref *nodes = NULL; uint32_t *starts = NULL, *ends = NULL; size_t n = 0, cap = 0;
    uint16_t gid = (lang && *lang) ? grammar_id_for(lang) : 0;
    TSParser *ps = gid ? lp_tls_parser(gid) : NULL;
    TSTree *tree = ps ? ts_parser_parse_string(ps, NULL, src, (uint32_t)len) : NULL;
    if (tree) {
        TSNode root = ts_tree_root_node(tree);
        collect_ts_nodes(root, src, len, 0, &nodes, &starts, &ends, &n, &cap);
        ts_tree_delete(tree);
    }
    if (n == 0 && len > 0) {
        size_t pos = 0;
        while (pos < len && n < 128) {
            while (pos < len && (src[pos] == '\n' || src[pos] == '\r')) pos++;
            if (pos >= len) break;
            size_t end = pos;
            while (end < len && src[end] != '\n' && src[end] != '\r') end++;
            if (end > pos) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 32;
                    nodes = xrealloc(nodes, cap * sizeof(Ref));
                    starts = xrealloc(starts, cap * sizeof(uint32_t));
                    ends = xrealloc(ends, cap * sizeof(uint32_t));
                }
                nodes[n] = text_ref(CTX[0], (const uint8_t *)(src + pos), end - pos);
                starts[n] = (uint32_t)pos;
                ends[n] = (uint32_t)end;
                n++;
            }
            pos = end + 1;
        }
    }

    *root_out = len > 0 ? text_ref(CTX[0], (const uint8_t *)src, len) : text_ref(CTX[0], (const uint8_t *)"", 0);
    *nodes_out = nodes;
    *starts_out = starts;
    *ends_out = ends;
    *count_out = n;
    return 0;
}

static int parse_verify_argv(const char *cmd, char *buf, size_t cap, char *argv[64]){
    if (!cmd || !*cmd) return 0;
    snprintf(buf, cap, "%s", cmd);
    int argc = 0; char *p = buf;
    while (*p && argc < 63) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            argv[argc++] = p;
            while (*p && *p != q) p++;
            if (*p) *p++ = 0;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = 0;
        }
    }
    argv[argc] = NULL;
    return argc;
}

static int resolve_exe_on_path(const char *exe, char *out, size_t cap){
    if (!exe || !*exe || !out || !cap) return 0;
    if (strchr(exe, '/') || strchr(exe, '\\')) {
        snprintf(out, cap, "%s", exe);
        os_slashes(out);
        return os_exists(out) && !os_is_dir(out);
    }
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
            if (!strstr(exe, ".exe")) {
                snprintf(out + dlen, cap - dlen, "%s.exe", exe);
                os_slashes(out);
                if (os_exists(out) && !os_is_dir(out)) return 1;
            }
        }
        p = q ? q + 1 : p + dlen;
    }
    return 0;
}

/* Parse 1-based line and optional 1-based column from compiler diagnostic log. */
static int extract_error_loc(const char *log, int *col_out){
    if (col_out) *col_out = -1;
    if (!log) return -1;
    const char *err = strstr(log, "error:");
    if (!err) err = strstr(log, "Error");
    if (!err) return -1;
    const char *scan = err;
    while (scan > log && *scan != '\n') scan--;
    if (*scan == '\n') scan++;
    for (const char *p = scan; p < err; p++) {
        if ((*p == ':' || *p == '(') && isdigit((unsigned char)p[1])) {
            char *endp = NULL;
            int ln = (int)strtol(p + 1, &endp, 10);
            if (ln > 0) {
                if (col_out && endp && (*endp == ':' || *endp == ',') && isdigit((unsigned char)endp[1])) {
                    int col = (int)strtol(endp + 1, NULL, 10);
                    if (col > 0) *col_out = col;
                }
                return ln;
            }
        }
    }
    return -1;
}

static void line_byte_range(const char *src, size_t len, int target_line, uint32_t *sb_out, uint32_t *eb_out){
    int cur = 1; size_t pos = 0;
    while (pos < len && cur < target_line) {
        if (src[pos] == '\n') cur++;
        pos++;
    }
    size_t end = pos;
    while (end < len && src[end] != '\n' && src[end] != '\r') end++;
    *sb_out = (uint32_t)pos;
    *eb_out = (uint32_t)end;
}

static void emit_monologue_witness(const char *conninfo, const Ref *subject, const char *note, uint32_t pos_idx){
    if (!conninfo || !*conninfo || !subject || !note) return;
    const lp_trust_class *mc = lp_trust_class_named("InternalMonologue");
    double trust = mc ? mc->prior : 0.10;
    Ref wref = text_ref(CTX[0], (const uint8_t *)"ooda-monologue", 14);
    Ref nref = text_ref(CTX[0], (const uint8_t *)note, strlen(note));
    Ref rel = text_ref(CTX[0], (const uint8_t *)"InternalMonologue", 17);
    Ref subj = *subject; subj.said = subj.outcome = subj.position = subj.spare = 0;
    Ref cp[3] = { subj, rel, nref };
    Ref obs = said_claim(compose(cp, 3, ref_above(cp, 3)));
    obs.position = pos_idx ? pos_idx : 1u;
    Ref tr[2] = { said_metadata(wref), obs };
    Ref trunk = compose(tr, 2, ref_above(tr, 2));
    File f; memset(&f, 0, sizeof f);
    f.path = "ooda-monologue"; f.witness = wref; f.lineage = wref; f.has_lineage = 1;
    f.trust = trust; f.trunk = trunk; f.file = trunk; f.has_file = 1;
    Event mev = {
        .claim = obs.id, .witnessed = obs.id,
        .score = 0.5f, .enter_rating = 1500.0f,
        .position = obs.position, .kind = EV_CLAIM, .run = 1
    };
    ev_push(&f.ev, &mev);
    Fold *fold = fold_new();
    fold_events(fold, &f, 1);
    LoadWitness lw = { .id = trunk.id, .lineage = wref.id, .trust = trust, .has_lineage = 1 };
    LoadStats ls = { 0 };
    if (load(conninfo, 1, &f, 1, &ls) == 0) {
        if (load_register(conninfo, 1, &lw, 1, &f, 1) == 0) merge(conninfo, 1, &ls);
    }
    fold_free(fold);
    free(f.ev.e);
}

static TSTree *parse_cst(const char *src, size_t len, const char *lang){
    if (!src || !lang || !*lang) return NULL;
    uint16_t gid = grammar_id_for(lang);
    TSParser *ps = gid ? lp_tls_parser(gid) : NULL;
    return ps ? ts_parser_parse_string(ps, NULL, src, (uint32_t)len) : NULL;
}

static int count_cst_errors(TSNode node){
    if (ts_node_is_null(node)) return 0;
    int err = (ts_node_is_error(node) || ts_node_is_missing(node)) ? 1 : 0;
    uint32_t nc = ts_node_child_count(node);
    for (uint32_t i = 0; i < nc; i++) {
        err += count_cst_errors(ts_node_child(node, i));
    }
    return err;
}

static int find_first_missing_token(TSNode node, uint32_t *byte_out, const char **tok_out){
    if (ts_node_is_null(node)) return 0;
    if (ts_node_is_missing(node) && !ts_node_is_named(node)) {
        const char *tp = ts_node_type(node);
        size_t tlen = tp ? strlen(tp) : 0;
        if (tlen >= 1 && tlen <= 8) {
            int ok = 1;
            for (size_t k = 0; k < tlen; k++) {
                unsigned char ch = (unsigned char)tp[k];
                if (ch < 0x20 || ch >= 0x7f) { ok = 0; break; }
            }
            if (ok) {
                *byte_out = ts_node_start_byte(node);
                *tok_out = tp;
                return 1;
            }
        }
    }
    uint32_t nc = ts_node_child_count(node);
    for (uint32_t i = 0; i < nc; i++) {
        if (find_first_missing_token(ts_node_child(node, i), byte_out, tok_out)) return 1;
    }
    return 0;
}

static void find_unclosed_delimiter_cst(TSNode node, const char *src, size_t len, char *stack, int *sp, int cap){
    if (ts_node_is_null(node)) return;
    const char *tp = ts_node_type(node);
    if (tp && (!strcmp(tp, "string_literal") || !strcmp(tp, "char_literal") ||
               !strcmp(tp, "comment") || !strcmp(tp, "raw_string_literal"))) {
        return;
    }
    uint32_t nc = ts_node_child_count(node);
    if (nc == 0 && !ts_node_is_missing(node)) {
        uint32_t sb = ts_node_start_byte(node), eb = ts_node_end_byte(node);
        if (eb == sb + 1 && eb <= len) {
            char ch = src[sb];
            if (ch == '(' || ch == '{' || ch == '[') {
                if (*sp < cap) stack[(*sp)++] = ch;
            } else if (ch == ')' || ch == '}' || ch == ']') {
                char want = (ch == ')') ? '(' : (ch == '}') ? '{' : '[';
                if (*sp > 0 && stack[*sp - 1] == want) (*sp)--;
            }
        }
        return;
    }
    for (uint32_t i = 0; i < nc; i++) {
        find_unclosed_delimiter_cst(ts_node_child(node, i), src, len, stack, sp, cap);
    }
}

static int candidate_is_new_and_valid(const char *cand, size_t cand_len, const char *lang,
                                      int max_allowed_errors, const lp_id *tried, size_t ntried){
    if (!cand) return 0;
    Ref cref = text_ref(CTX[0], (const uint8_t *)cand, cand_len);
    for (size_t i = 0; i < ntried; i++) {
        if (lp_id_eq(&cref.id, &tried[i])) return 0;
    }
    TSTree *tree = parse_cst(cand, cand_len, lang);
    if (tree) {
        int errs = count_cst_errors(ts_tree_root_node(tree));
        ts_tree_delete(tree);
        if (errs > max_allowed_errors) return 0;
    }
    return 1;
}

static void record_decl_ident(const char *src, size_t len, uint32_t sb, uint32_t eb, int is_fn,
                              char decls[128][64], int decl_is_fn[128], int *ndecls){
    if (*ndecls >= 128 || eb <= sb || eb > len) return;
    size_t n = (size_t)(eb - sb);
    if (n == 0 || n >= 64) return;
    for (int i = 0; i < *ndecls; i++) {
        if (strlen(decls[i]) == n && memcmp(decls[i], src + sb, n) == 0) {
            if (is_fn) decl_is_fn[i] = 1;
            return;
        }
    }
    memcpy(decls[*ndecls], src + sb, n);
    decls[*ndecls][n] = 0;
    decl_is_fn[*ndecls] = is_fn ? 1 : 0;
    (*ndecls)++;
}

static void collect_ast_declared_idents(TSNode node, const char *src, size_t len, int in_decl, int in_fn_decl,
                                        char decls[128][64], int decl_is_fn[128], int *ndecls){
    if (ts_node_is_null(node) || *ndecls >= 128) return;
    const char *tp = ts_node_type(node);
    if (!tp) return;
    if ((!strcmp(tp, "identifier") || !strcmp(tp, "field_identifier")) && in_decl) {
        record_decl_ident(src, len, ts_node_start_byte(node), ts_node_end_byte(node), in_fn_decl, decls, decl_is_fn, ndecls);
        return;
    }
    if (!strcmp(tp, "compound_statement") || !strcmp(tp, "argument_list")) {
        in_decl = 0;
        in_fn_decl = 0;
    }
    if (!strcmp(tp, "init_declarator")) {
        TSNode d = ts_node_child_by_field_name(node, "declarator", 10);
        TSNode v = ts_node_child_by_field_name(node, "value", 5);
        if (!ts_node_is_null(d)) collect_ast_declared_idents(d, src, len, 1, in_fn_decl, decls, decl_is_fn, ndecls);
        if (!ts_node_is_null(v)) collect_ast_declared_idents(v, src, len, 0, 0, decls, decl_is_fn, ndecls);
        return;
    }
    if (!strcmp(tp, "preproc_def")) {
        TSNode nm = ts_node_child_by_field_name(node, "name", 4);
        if (!ts_node_is_null(nm)) collect_ast_declared_idents(nm, src, len, 1, 0, decls, decl_is_fn, ndecls);
        return;
    }
    int next_decl = in_decl;
    int next_fn = in_fn_decl;
    if (!strcmp(tp, "function_declarator")) {
        TSNode fn = ts_node_child_by_field_name(node, "declarator", 10);
        TSNode pl = ts_node_child_by_field_name(node, "parameters", 10);
        if (!ts_node_is_null(fn)) collect_ast_declared_idents(fn, src, len, 1, 1, decls, decl_is_fn, ndecls);
        if (!ts_node_is_null(pl)) collect_ast_declared_idents(pl, src, len, 0, 0, decls, decl_is_fn, ndecls);
        return;
    }
    if (!strcmp(tp, "parameter_declaration") || !strcmp(tp, "field_declaration") ||
        !strcmp(tp, "declaration") || !strcmp(tp, "variable_declarator")) {
        next_decl = 1;
        next_fn = 0;
    }
    uint32_t nc = ts_node_child_count(node);
    for (uint32_t i = 0; i < nc && *ndecls < 128; i++) {
        collect_ast_declared_idents(ts_node_child(node, i), src, len, next_decl, next_fn, decls, decl_is_fn, ndecls);
    }
}

static void collect_span_idents(TSNode node, const char *src, size_t len, uint32_t sb, uint32_t eb, int is_callee,
                                char names[32][64], uint32_t starts[32], uint32_t ends[32], int callee_flags[32], int *n){
    if (ts_node_is_null(node) || *n >= 32) return;
    uint32_t nsb = ts_node_start_byte(node), neb = ts_node_end_byte(node);
    if (neb <= sb || nsb >= eb) return;
    const char *tp = ts_node_type(node);
    if (tp && (!strcmp(tp, "identifier") || !strcmp(tp, "type_identifier")) && neb > nsb && neb <= len && (neb - nsb) < 64) {
        size_t idlen = (size_t)(neb - nsb);
        memcpy(names[*n], src + nsb, idlen);
        names[*n][idlen] = 0;
        starts[*n] = nsb;
        ends[*n] = neb;
        callee_flags[*n] = is_callee ? 1 : 0;
        (*n)++;
        return;
    }
    if (tp && !strcmp(tp, "call_expression")) {
        TSNode fn = ts_node_child_by_field_name(node, "function", 8);
        TSNode args = ts_node_child_by_field_name(node, "arguments", 9);
        if (!ts_node_is_null(fn)) collect_span_idents(fn, src, len, sb, eb, 1, names, starts, ends, callee_flags, n);
        if (!ts_node_is_null(args)) collect_span_idents(args, src, len, sb, eb, 0, names, starts, ends, callee_flags, n);
        return;
    }
    uint32_t nc = ts_node_child_count(node);
    for (uint32_t i = 0; i < nc && *n < 32; i++) {
        collect_span_idents(ts_node_child(node, i), src, len, sb, eb, 0, names, starts, ends, callee_flags, n);
    }
}

static int levenshtein_bounded(const char *a, const char *b){
    size_t la = strlen(a), lb = strlen(b);
    if (la >= 64 || lb >= 64) return 99;
    int dp[64][64];
    for (size_t i = 0; i <= la; i++) dp[i][0] = (int)i;
    for (size_t j = 0; j <= lb; j++) dp[0][j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        for (size_t j = 1; j <= lb; j++) {
            int sub = dp[i - 1][j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            int ins = dp[i][j - 1] + 1;
            int del = dp[i - 1][j] + 1;
            int m = sub < ins ? sub : ins;
            dp[i][j] = m < del ? m : del;
        }
    }
    return dp[la][lb];
}

static char *ooda_file_basename(Reader *rd, const lp_id *file){
    lp_id at = *file, kid[64];
    for (int depth = 0; depth < 6; depth++) {
        size_t n = reader_parts(rd, &at, kid, 64);
        if (!n) break;
        for (size_t i = 0; i < n && i < 64; i++) {
            lp_id two[4];
            if (reader_parts(rd, &kid[i], two, 4) != 2) continue;
            char *k = reader_text(rd, &two[0], 32);
            int is_fn = k && !strcmp(k, "filename");
            free(k);
            if (is_fn) return reader_text(rd, &two[1], 256);
        }
        at = kid[0];
    }
    return NULL;
}

/* Stage C1: Substrate Workspace AST resolution — resolve undeclared symbols directly from ingested
 * `declared identifier` / `declaration signature` AST claims in PostgreSQL (zero hardcoded stdlib tables). */
static int substrate_ast_resolve_symbol(PGconn *pg, const char *lang, const char *src, size_t len,
                                        const char *sym, int orig_errors,
                                        const lp_id *tried, size_t ntried,
                                        char **patched_out, size_t *patched_len_out){
    if (!pg || !sym || !*sym || (strcmp(lang, "c") != 0 && strcmp(lang, "cpp") != 0)) return 1;
    Ref sym_ref = text_ref(CTX[0], (const uint8_t *)sym, strlen(sym));
    Ref rel_decl = text_ref(CTX[0], (const uint8_t *)"declared identifier", 19);
    int nh = 0;
    Hold *h = holds_both(pg, &sym_ref.id, 1, &rel_decl.id, &nh);

    Reader *rd = reader_new(pg);
    int resolved = 0;
    for (int i = 0; h && i < nh && i < 8 && !resolved; i++) {
        if (!h[i].claim) continue;
        lp_id parts[4];
        size_t np = lp_path_ids(h[i].path, (size_t)h[i].path_len, parts, 4);
        if (np != 3 || !lp_id_eq(&parts[1], &rel_decl.id) || !lp_id_eq(&parts[2], &sym_ref.id)) continue;

        /* Walk containment index up from the declaration claim to its declaring header file (*.h / *.hpp). */
        HeldUp up;
        held_up(pg, &h[i].entity, 1, HELD_CONTENT, 256, 2048, &up);
        for (size_t k = 0; k < up.n && !resolved; k++) {
            if (!up.node[k].file) continue;
            char *fname = ooda_file_basename(rd, &up.node[k].id);
            if (!fname) continue;
            size_t fl = strlen(fname);
            int is_hdr = (fl > 2 && !strcmp(fname + fl - 2, ".h")) || (fl > 4 && !strcmp(fname + fl - 4, ".hpp"));
            if (is_hdr && strstr(src, fname) == NULL) {
                char inc[320];
                snprintf(inc, sizeof inc, "#include \"%s\"\n", fname);
                char *cand = NULL; size_t cand_len = 0;
                if (lp_ooda_splice(src, len, 0u, 0u, inc, strlen(inc), &cand, &cand_len) == 0) {
                    if (candidate_is_new_and_valid(cand, cand_len, lang, orig_errors, tried, ntried)) {
                        *patched_out = cand; *patched_len_out = cand_len;
                        resolved = 1;
                    } else {
                        free(cand);
                    }
                }
            }
            free(fname);
        }
        held_up_free(&up);
    }
    if (!resolved) {
        /* Header prototypes (e.g. extern/API functions) live in containment even without a function_definition body. */
        HeldUp up;
        held_up(pg, &sym_ref.id, 1, HELD_CONTENT, 256, 4096, &up);
        for (size_t k = 0; k < up.n && !resolved; k++) {
            if (!up.node[k].file) continue;
            char *fname = ooda_file_basename(rd, &up.node[k].id);
            if (!fname) continue;
            size_t fl = strlen(fname);
            int is_hdr = (fl > 2 && !strcmp(fname + fl - 2, ".h")) || (fl > 4 && !strcmp(fname + fl - 4, ".hpp"));
            if (is_hdr && strstr(src, fname) == NULL) {
                char inc[320];
                snprintf(inc, sizeof inc, "#include \"%s\"\n", fname);
                char *cand = NULL; size_t cand_len = 0;
                if (lp_ooda_splice(src, len, 0u, 0u, inc, strlen(inc), &cand, &cand_len) == 0) {
                    if (candidate_is_new_and_valid(cand, cand_len, lang, orig_errors, tried, ntried)) {
                        *patched_out = cand; *patched_len_out = cand_len;
                        resolved = 1;
                    } else {
                        free(cand);
                    }
                }
            }
            free(fname);
        }
        held_up_free(&up);
    }
    reader_free(rd);
    holds_free(h, nh);
    return resolved ? 0 : 1;
}

/* Synthesize minimal targeted AST/scope/substrate patch Δ guided by loss_contrast culprit + CST + substrate. */
static int synthesize_delta(PGconn *pg, Firmware *fw, const char *prompt, const char *lang,
                            const char *src, size_t len, const char *log,
                            uint32_t culprit_sb, uint32_t culprit_eb,
                            const lp_id *tried, size_t ntried,
                            char **patched_out, size_t *patched_len_out){
    if (!log) log = "";
    int err_col = -1;
    int err_line = extract_error_loc(log, &err_col);
    uint32_t lsb = 0, leb = (uint32_t)len;
    if (err_line > 0) line_byte_range(src, len, err_line, &lsb, &leb);
    else if (culprit_eb > culprit_sb && culprit_eb <= len) { lsb = culprit_sb; leb = culprit_eb; }

    TSTree *tree = parse_cst(src, len, lang);
    int orig_errors = tree ? count_cst_errors(ts_tree_root_node(tree)) : 0;

    /* Stage A: Formal Grammar Repair (Tree-sitter MISSING token & CST delimiter balance) */
    if (tree && orig_errors > 0) {
        TSNode root = ts_tree_root_node(tree);
        uint32_t ins_byte = 0; const char *tok = NULL;
        if (find_first_missing_token(root, &ins_byte, &tok) && tok) {
            char *cand = NULL; size_t cand_len = 0;
            if (lp_ooda_splice(src, len, ins_byte, ins_byte, tok, strlen(tok), &cand, &cand_len) == 0) {
                if (candidate_is_new_and_valid(cand, cand_len, lang, orig_errors - 1, tried, ntried)) {
                    ts_tree_delete(tree);
                    *patched_out = cand; *patched_len_out = cand_len;
                    return 0;
                }
                free(cand);
            }
        }
        char stack[64]; int sp = 0;
        find_unclosed_delimiter_cst(root, src, len, stack, &sp, 64);
        if (sp > 0) {
            char open_ch = stack[sp - 1];
            const char *closer = (open_ch == '(') ? ")" : (open_ch == '[') ? "]" : "\n}\n";
            uint32_t pos = (open_ch == '{') ? (uint32_t)len : leb;
            if (open_ch != '{' && pos > lsb) {
                while (pos > lsb && (src[pos - 1] == ' ' || src[pos - 1] == ';' || src[pos - 1] == '\t')) pos--;
            }
            char *cand = NULL; size_t cand_len = 0;
            if (lp_ooda_splice(src, len, pos, pos, closer, strlen(closer), &cand, &cand_len) == 0) {
                if (candidate_is_new_and_valid(cand, cand_len, lang, orig_errors - 1, tried, ntried)) {
                    ts_tree_delete(tree);
                    *patched_out = cand; *patched_len_out = cand_len;
                    return 0;
                }
                free(cand);
            }
        }
    }

    /* Stage B: AST Scope Resolution (in-scope symbol table + bounded edit distance) */
    if (tree) {
        TSNode root = ts_tree_root_node(tree);
        char decls[128][64]; int decl_is_fn[128]; int ndecls = 0;
        collect_ast_declared_idents(root, src, len, 0, 0, decls, decl_is_fn, &ndecls);

        char span_names[32][64]; uint32_t s_sb[32], s_eb[32]; int callee_flags[32]; int nspan = 0;
        collect_span_idents(root, src, len, lsb, leb, 0, span_names, s_sb, s_eb, callee_flags, &nspan);

        for (int i = 0; i < nspan; i++) {
            int declared = 0;
            for (int d = 0; d < ndecls && !declared; d++) {
                if (!strcmp(span_names[i], decls[d])) declared = 1;
            }
            if (declared) continue;

            int ilen = (int)strlen(span_names[i]);
            int best_d = 99, best_idx = -1, ties = 0;
            for (int d = 0; d < ndecls; d++) {
                if (callee_flags[i] && !decl_is_fn[d]) continue;
                int dist = levenshtein_bounded(span_names[i], decls[d]);
                if (dist >= 1 && dist <= 2 && dist * 2 <= ilen) {
                    if (dist < best_d) { best_d = dist; best_idx = d; ties = 1; }
                    else if (dist == best_d) ties++;
                }
            }
            if (best_idx >= 0 && ties == 1) {
                char *cand = NULL; size_t cand_len = 0;
                if (lp_ooda_splice(src, len, s_sb[i], s_eb[i], decls[best_idx], strlen(decls[best_idx]), &cand, &cand_len) == 0) {
                    if (candidate_is_new_and_valid(cand, cand_len, lang, orig_errors, tried, ntried)) {
                        ts_tree_delete(tree);
                        *patched_out = cand; *patched_len_out = cand_len;
                        return 0;
                    }
                    free(cand);
                }
            }
            if (pg && substrate_ast_resolve_symbol(pg, lang, src, len, span_names[i], orig_errors,
                                                   tried, ntried, patched_out, patched_len_out) == 0) {
                ts_tree_delete(tree);
                return 0;
            }
        }
        ts_tree_delete(tree);
        tree = NULL;
    }
    if (tree) ts_tree_delete(tree);

    /* Stage C: Substrate-Coupled Repair via lp_forward_execute */
    if (pg && fw) {
        size_t cul_n = (culprit_eb > culprit_sb && culprit_eb <= len) ? (size_t)(culprit_eb - culprit_sb) : 0;
        if (cul_n > 96) cul_n = 96;
        char diag_prompt[512];
        snprintf(diag_prompt, sizeof diag_prompt, "%s %.*s %.220s",
                 prompt ? prompt : "",
                 (int)cul_n, cul_n ? (src + culprit_sb) : "",
                 log);
        lp_envelope env = lp_envelope_preset(LP_ENVELOPE_STANDARD);
        LpForwardRequest freq = {
            .pg = pg, .fw = fw,
            .prompt_text = (const uint8_t *)diag_prompt,
            .prompt_len = strlen(diag_prompt),
            .read_only = 1, .max_emit = 16, .temperature = -1.0,
            .envelope = &env
        };
        LpForwardResult fres = { 0 };
        if (lp_forward_execute(&freq, &fres) == 0 && fres.text && fres.text_len > 0) {
            int prepend = (!strncmp(fres.text, "#include", 8) || !strncmp(fres.text, "import ", 7) ||
                           !strncmp(fres.text, "using ", 6));
            uint32_t sp_sb = prepend ? 0u : (culprit_eb > culprit_sb && culprit_eb <= len ? culprit_sb : 0u);
            uint32_t sp_eb = prepend ? 0u : (culprit_eb > culprit_sb && culprit_eb <= len ? culprit_eb : 0u);
            if (prepend || culprit_eb > culprit_sb) {
                char *cand = NULL; size_t cand_len = 0;
                if (lp_ooda_splice(src, len, sp_sb, sp_eb, fres.text, fres.text_len, &cand, &cand_len) == 0) {
                    if (candidate_is_new_and_valid(cand, cand_len, lang, orig_errors, tried, ntried)) {
                        lp_forward_result_free(&fres);
                        *patched_out = cand; *patched_len_out = cand_len;
                        return 0;
                    }
                    free(cand);
                }
            }
        }
        lp_forward_result_free(&fres);
    }

    return 1;
}

int lp_ooda_run(const LpOodaRequest *req, LpOodaResult *out){
    if (!req || !out) return 2;
    memset(out, 0, sizeof *out);

    size_t cur_len = 0, orig_len = 0; char *cur_src = NULL, *orig_src = NULL;
    if (req->code_text) {
        cur_len = req->code_len ? req->code_len : strlen(req->code_text);
        cur_src = malloc(cur_len + 1);
        memcpy(cur_src, req->code_text, cur_len); cur_src[cur_len] = 0;
    } else if (req->code_path && *req->code_path) {
        cur_src = read_whole_file(req->code_path, &cur_len);
        if (cur_src) {
            orig_len = cur_len;
            orig_src = malloc(orig_len + 1);
            memcpy(orig_src, cur_src, orig_len + 1);
        }
    } else if (req->prompt && *req->prompt) {
        cur_len = strlen(req->prompt);
        cur_src = malloc(cur_len + 1);
        memcpy(cur_src, req->prompt, cur_len); cur_src[cur_len] = 0;
    }
    if (!cur_src) {
        snprintf(out->disposition, sizeof out->disposition, "error: missing or unreadable source");
        return 1;
    }

    const char *lang = (req->lang && *req->lang) ? req->lang : "c";
    Ref root0 = { 0 }; Ref *nodes = NULL; uint32_t *sb = NULL, *eb = NULL; size_t n_nodes = 0;
    lp_ooda_decompose_ast(cur_src, cur_len, lang, &root0, &nodes, &sb, &eb, &n_nodes);
    out->source_id = root0.id;
    out->patched_id = root0.id;
    free(nodes); free(sb); free(eb);

    /* Unverified mode: verify_cmd == NULL -> explicit unverified_by_toolchain + InternalMonologue hypothesis only */
    if (!req->verify_cmd || !*req->verify_cmd) {
        if (!req->read_only && req->conninfo && *req->conninfo)
            emit_monologue_witness(req->conninfo, &root0, req->prompt ? req->prompt : "unverified draft", 1u);
        free(orig_src);
        out->unverified_by_toolchain = 1;
        out->verified = 0;
        out->patched_code = cur_src;
        out->patched_len = cur_len;
        snprintf(out->disposition, sizeof out->disposition, "unverified_by_toolchain");
        return 0;
    }

    int max_rounds = req->max_rounds > 0 ? req->max_rounds : (req->fw && req->fw->ooda_rounds > 0 ? req->fw->ooda_rounds : 3);
    const char *witness = (req->witness_name && *req->witness_name) ? req->witness_name : "toolchain";
    int owns_pg = 0;
    PGconn *pg = req->pg;
    if (!pg && req->conninfo && *req->conninfo) { pg = db_try(req->conninfo); owns_pg = (pg != NULL); }

    lp_id tried_roots[64];
    size_t ntried = 0;

    for (int round = 0; round < max_rounds; round++) {
        out->rounds_run = round + 1;
        Ref cur_root = { 0 }; Ref *ast = NULL; uint32_t *as = NULL, *ae = NULL; size_t nast = 0;
        lp_ooda_decompose_ast(cur_src, cur_len, lang, &cur_root, &ast, &as, &ae, &nast);
        out->patched_id = cur_root.id;

        if (req->code_path && *req->code_path && round > 0)
            write_whole_file(req->code_path, cur_src, cur_len);

        char cmd_buf[2048]; char *argv[64]; char exe_resolved[1024];
        int argc = parse_verify_argv(req->verify_cmd, cmd_buf, sizeof cmd_buf, argv);
        if (argc > 0 && resolve_exe_on_path(argv[0], exe_resolved, sizeof exe_resolved))
            argv[0] = exe_resolved;
        char *vlog = NULL; int vcode = 127;
        int ran = argc > 0 ? os_run_captured(argv, 60, 65536, &vlog, &vcode) : 0;
        if (!ran) { free(vlog); vlog = strdup("failed to launch verifier command"); vcode = 127; }
        free(out->verifier_log);
        out->verifier_log = vlog;
        out->exit_code = vcode;

        int passed = (ran && vcode == 0);
        if (!req->read_only && req->conninfo && *req->conninfo) {
            LpToolchainReport rep = {
                .conninfo = req->conninfo,
                .witness_name = witness,
                .target_label = req->code_path ? req->code_path : lang,
                .subject = &cur_root,
                .constituents = ast,
                .nconstituents = nast,
                .passed = passed,
                .position = (uint32_t)(round + 1)
            };
            lp_toolchain_witness(&rep, NULL, NULL);
        }

        if (passed) {
            out->verified = 1;
            out->patched_code = cur_src;
            out->patched_len = cur_len;
            snprintf(out->disposition, sizeof out->disposition, "verified: toolchain passed (WIN)");
            if (orig_src && req->code_path && *req->code_path && !req->write_file)
                write_whole_file(req->code_path, orig_src, orig_len);
            free(orig_src);
            free(ast); free(as); free(ae);
            if (owns_pg) PQfinish(pg);
            return 0;
        }

        /* Causal localization via microsecond salience_of(loss_contrast), aligned with verifier error line. */
        uint32_t cul_sb = 0, cul_eb = 0; double best_lc = -1.0; lp_id cul_id = { 0 };
        double *node_lc = nast > 0 ? calloc(nast, sizeof(double)) : NULL;
        if (pg && nast > 0) {
            lp_id *aids = malloc(sizeof(lp_id) * nast);
            for (size_t k = 0; k < nast; k++) aids[k] = ast[k].id;
            int nsal = 0;
            LpSalienceRow *sal = salience_of(pg, aids, (int)nast, NULL, 0, &nsal);
            for (int r = 0; r < nsal; r++) {
                double score = sal[r].loss_contrast;
                if (sal[r].ord >= 1 && (size_t)sal[r].ord <= nast) {
                    size_t idx = (size_t)sal[r].ord - 1;
                    node_lc[idx] = score;
                    if (score > best_lc) {
                        best_lc = score; cul_id = sal[r].id;
                        cul_sb = as[idx]; cul_eb = ae[idx];
                    }
                }
            }
            free(sal); free(aids);
        }
        int err_line = extract_error_loc(out->verifier_log, NULL);
        if (err_line > 0 && nast > 0) {
            uint32_t lsb = 0, leb = 0;
            line_byte_range(cur_src, cur_len, err_line, &lsb, &leb);
            double line_best_lc = -2.0;
            int line_pick = -1;
            for (size_t k = 0; k < nast; k++) {
                if (as[k] >= lsb && ae[k] <= leb) {
                    double lc = node_lc ? node_lc[k] : 0.0;
                    if (line_pick < 0 || lc > line_best_lc) { line_best_lc = lc; line_pick = (int)k; }
                }
            }
            if (line_pick >= 0 && (best_lc <= 0.0 || line_best_lc >= best_lc * 0.5)) {
                cul_id = ast[line_pick].id; cul_sb = as[line_pick]; cul_eb = ae[line_pick];
                if (line_best_lc > best_lc) best_lc = line_best_lc;
            }
        }
        free(node_lc);
        out->culprit_id = cul_id;
        out->culprit_contrast = best_lc > 0.0 ? best_lc : 0.0;
        out->culprit_start = cul_sb;
        out->culprit_end = cul_eb;

        if (!req->read_only && req->conninfo && *req->conninfo)
            emit_monologue_witness(req->conninfo, &cur_root, out->verifier_log ? out->verifier_log : "repair hypothesis", (uint32_t)(round + 1));

        if (ntried < 64) tried_roots[ntried++] = cur_root.id;

        char *next_src = NULL; size_t next_len = 0;
        int patched = synthesize_delta(pg, req->fw, req->prompt, lang, cur_src, cur_len, out->verifier_log, cul_sb, cul_eb, tried_roots, ntried, &next_src, &next_len);
        free(ast); free(as); free(ae);

        if (patched != 0 || !next_src || (next_len == cur_len && memcmp(next_src, cur_src, cur_len) == 0)) {
            free(next_src);
            break;
        }
        free(cur_src);
        cur_src = next_src;
        cur_len = next_len;
    }

    if (orig_src && req->code_path && *req->code_path)
        write_whole_file(req->code_path, orig_src, orig_len);
    free(orig_src);
    if (owns_pg) PQfinish(pg);
    out->verified = 0;
    out->patched_code = cur_src;
    out->patched_len = cur_len;
    snprintf(out->disposition, sizeof out->disposition, "WHY_NOT: verifier exited %d after %d rounds (culprit %u..%u)", out->exit_code, out->rounds_run, out->culprit_start, out->culprit_end);
    return 1;
}

void lp_ooda_result_free(LpOodaResult *out){
    if (!out) return;
    free(out->patched_code);
    free(out->verifier_log);
    memset(out, 0, sizeof *out);
}
