#include "engine.h"
#include "laplace/os.h"
#include "libpq-events.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LP_OID_BYTEA  = 17,
    LP_OID_INT8   = 20,
    LP_OID_INT2   = 21,
    LP_OID_INT4   = 23,
    LP_OID_TEXT   = 25,
    LP_OID_OID    = 26,
    LP_OID_FLOAT8 = 701
};

typedef struct {
    const char *name;
    int arity;
    const char *sql;
} SqlMeta;

static const SqlMeta SQL_META[LP_SQL_COUNT] = {
#define LP_SQL(name, arity, sql) { #name, arity, sql },
#include "sql_catalog.def"
#undef LP_SQL
};

typedef struct {
    int ready;
    char *expanded;
} ContractPlan;

typedef struct {
    PGconn *pg;
    int has_binding;
    DbBinding binding;
    ContractPlan plans[LP_SQL_COUNT];
} ContractConn;

typedef struct {
    PGconn *pg;
    ContractConn *state;
} ContractEntry;

static pthread_mutex_t contract_mu = PTHREAD_MUTEX_INITIALIZER;
static ContractEntry *contract_conns;
static size_t contract_n, contract_cap;

const char *lp_sql_name(LpSqlId id) {
    return (id >= 0 && id < LP_SQL_COUNT) ? SQL_META[id].name : "INVALID";
}

int lp_sql_arity(LpSqlId id) {
    return (id >= 0 && id < LP_SQL_COUNT) ? SQL_META[id].arity : -1;
}

const char *lp_sql_template(LpSqlId id) {
    return (id >= 0 && id < LP_SQL_COUNT) ? SQL_META[id].sql : "";
}

static void put_be16(lp_buf *b, uint16_t v) {
    uint8_t p[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    lp_buf_put(b, p, sizeof p);
}

static void put_be32(lp_buf *b, uint32_t v) {
    uint8_t p[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    lp_buf_put(b, p, sizeof p);
}

static void put_be64(lp_buf *b, uint64_t v) {
    uint8_t p[8] = {
        (uint8_t)(v >> 56), (uint8_t)(v >> 48), (uint8_t)(v >> 40), (uint8_t)(v >> 32),
        (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),  (uint8_t)v
    };
    lp_buf_put(b, p, sizeof p);
}

static SqlArg *arg_slot(Args *a, int binary) {
    if (a->n >= ARGS_MAX) {
        fputs("postgres contract: too many query parameters\n", stderr);
        abort();
    }
    SqlArg *s = &a->a[a->n++];
    s->b.n = 0;
    s->binary = binary;
    return s;
}

static void pg_array_head(lp_buf *b, uint32_t elem_oid, size_t n) {
    put_be32(b, n ? 1u : 0u);
    put_be32(b, 0u);
    put_be32(b, elem_oid);
    if (n) {
        put_be32(b, (uint32_t)n);
        put_be32(b, 1u);
    }
}

void arg_null(Args *a) {
    (void)arg_slot(a, SQL_NULL);
}

void arg_bool(Args *a, bool v) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    uint8_t byte = v ? 1u : 0u;
    lp_buf_put(&s->b, &byte, 1);
}

void arg_i16(Args *a, int16_t v) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    put_be16(&s->b, (uint16_t)v);
}

void arg_i32(Args *a, int32_t v) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    put_be32(&s->b, (uint32_t)v);
}

void arg_i64(Args *a, int64_t v) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    put_be64(&s->b, (uint64_t)v);
}

void arg_f64_bin(Args *a, double v) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    put_be64(&s->b, u);
}

void arg_f64_array(Args *a, const double *vals, size_t n) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    pg_array_head(&s->b, LP_OID_FLOAT8, n);
    for (size_t i = 0; i < n; i++) {
        uint64_t u;
        memcpy(&u, &vals[i], sizeof u);
        put_be32(&s->b, 8u);
        put_be64(&s->b, u);
    }
}

void arg_oid(Args *a, uint32_t oid) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    put_be32(&s->b, oid);
}

void arg_id(Args *a, const lp_id *id) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    lp_buf_put(&s->b, id->b, sizeof id->b);
}

void arg_text_bin(Args *a, const char *str, size_t len) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    if (len) lp_buf_put(&s->b, str, len);
    else lp_buf_put(&s->b, "", 0);
}

void arg_bytea_array(Args *a, const uint8_t *const *items, const size_t *lens, size_t n) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    pg_array_head(&s->b, LP_OID_BYTEA, n);
    for (size_t i = 0; i < n; i++) {
        put_be32(&s->b, (uint32_t)lens[i]);
        if (lens[i]) lp_buf_put(&s->b, items[i], lens[i]);
    }
}

void arg_i16_array(Args *a, const int16_t *vals, size_t n) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    pg_array_head(&s->b, LP_OID_INT2, n);
    for (size_t i = 0; i < n; i++) {
        put_be32(&s->b, 2u);
        put_be16(&s->b, (uint16_t)vals[i]);
    }
}

void arg_i32_array(Args *a, const int32_t *vals, size_t n) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    pg_array_head(&s->b, LP_OID_INT4, n);
    for (size_t i = 0; i < n; i++) {
        put_be32(&s->b, 4u);
        put_be32(&s->b, (uint32_t)vals[i]);
    }
}

void arg_i64_array(Args *a, const int64_t *vals, size_t n) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    pg_array_head(&s->b, LP_OID_INT8, n);
    for (size_t i = 0; i < n; i++) {
        put_be32(&s->b, 8u);
        put_be64(&s->b, (uint64_t)vals[i]);
    }
}

void arg_oid_array(Args *a, const uint32_t *vals, size_t n) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    pg_array_head(&s->b, LP_OID_OID, n);
    for (size_t i = 0; i < n; i++) {
        put_be32(&s->b, 4u);
        put_be32(&s->b, vals[i]);
    }
}

void arg_text_array(Args *a, const char *const *items, size_t n) {
    SqlArg *s = arg_slot(a, SQL_BIN);
    pg_array_head(&s->b, LP_OID_TEXT, n);
    for (size_t i = 0; i < n; i++) {
        if (!items[i]) {
            put_be32(&s->b, 0xffffffffu);
        } else {
            size_t len = strlen(items[i]);
            put_be32(&s->b, (uint32_t)len);
            if (len) lp_buf_put(&s->b, items[i], len);
        }
    }
}

static void conn_clear_locked(ContractConn *s) {
    for (int i = 0; i < LP_SQL_COUNT; i++) {
        s->plans[i].ready = 0;
        free(s->plans[i].expanded);
        s->plans[i].expanded = NULL;
    }
    if (s->binding.native_schema) PQfreemem((void *)s->binding.native_schema);
    if (s->binding.postgis_schema) PQfreemem((void *)s->binding.postgis_schema);
    s->binding = (DbBinding){ 0 };
    s->has_binding = 0;
}

static int contract_event_proc(PGEventId id, void *info, void *pass) {
    ContractConn *s = pass;
    if (!s) return 1;
    if (id == PGEVT_CONNRESET) {
        (void)info;
        pthread_mutex_lock(&contract_mu);
        conn_clear_locked(s);
        pthread_mutex_unlock(&contract_mu);
    } else if (id == PGEVT_CONNDESTROY) {
        pthread_mutex_lock(&contract_mu);
        for (size_t i = 0; i < contract_n; i++) {
            if (contract_conns[i].pg == s->pg) {
                contract_conns[i] = contract_conns[--contract_n];
                break;
            }
        }
        conn_clear_locked(s);
        free(s);
        pthread_mutex_unlock(&contract_mu);
    }
    return 1;
}

static ContractConn *contract_conn(PGconn *pg) {
    pthread_mutex_lock(&contract_mu);
    for (size_t i = 0; i < contract_n; i++) {
        if (contract_conns[i].pg == pg) {
            ContractConn *st = contract_conns[i].state;
            pthread_mutex_unlock(&contract_mu);
            return st;
        }
    }
    ContractConn *st = calloc(1, sizeof *st);
    if (!st) {
        pthread_mutex_unlock(&contract_mu);
        perror("contract connection");
        abort();
    }
    st->pg = pg;
    if (!PQregisterEventProc(pg, contract_event_proc, "laplace_contract", st)) {
        free(st);
        pthread_mutex_unlock(&contract_mu);
        fputs("postgres contract: cannot register libpq event proc\n", stderr);
        abort();
    }
    if (contract_n == contract_cap) {
        contract_cap = contract_cap ? contract_cap * 2 : 16;
        ContractEntry *grown = realloc(contract_conns, contract_cap * sizeof *grown);
        if (!grown) {
            pthread_mutex_unlock(&contract_mu);
            perror("contract registry");
            abort();
        }
        contract_conns = grown;
    }
    contract_conns[contract_n++] = (ContractEntry){ pg, st };
    pthread_mutex_unlock(&contract_mu);
    return st;
}

static void copy_err(char *err, size_t errsz, const char *msg) {
    if (err && errsz) {
        snprintf(err, errsz, "%s", msg && *msg ? msg : "unknown PostgreSQL error");
    }
}

static int needs_extension_binding(const char *sql) {
    return strstr(sql, "@extschema@") != NULL || strstr(sql, "@postgis@") != NULL;
}

static int has_sql_keyword(const char *s, const char *kw) {
    size_t klen = strlen(kw);
    for (size_t i = 0; i < klen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c != (unsigned char)kw[i]) return 0;
    }
    unsigned char next = (unsigned char)s[klen];
    return next == '\0' || next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '(';
}

/* Zero-arity utility commands (BEGIN, COMMIT, ROLLBACK, SET, SHOW, ANALYZE)
 * must not go through PQprepare, and ROLLBACK must use simple query execution
 * so PQTRANS_INERROR aborted transactions exit cleanly. */
static int is_utility_sql(const char *s) {
    if (!s) return 1;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (has_sql_keyword(s, "SELECT") || has_sql_keyword(s, "WITH") ||
        has_sql_keyword(s, "INSERT") || has_sql_keyword(s, "UPDATE") ||
        has_sql_keyword(s, "DELETE")) {
        return 0;
    }
    return 1;
}

const DbBinding *db_contract_binding(PGconn *pg, char *err, size_t errsz) {
    ContractConn *st = contract_conn(pg);
    if (st->has_binding) return &st->binding;

    PGresult *r = PQexecParams(pg, SQL_META[LP_SQL_EXT_BINDING].sql, 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        copy_err(err, errsz, r ? PQerrorMessage(pg) : "extension binding failed");
        PQclear(r);
        return NULL;
    }
    st->binding.native_schema = PQescapeIdentifier(pg, PQgetvalue(r, 0, 0), (size_t)PQgetlength(r, 0, 0));
    st->binding.postgis_schema = PQescapeIdentifier(pg, PQgetvalue(r, 0, 1), (size_t)PQgetlength(r, 0, 1));
    st->binding.id_type = col_oid(r, 0, 2);
    st->binding.id_array = col_oid(r, 0, 3);
    PQclear(r);
    if (!st->binding.native_schema || !st->binding.postgis_schema || !st->binding.id_type) {
        if (st->binding.native_schema) PQfreemem((void *)st->binding.native_schema);
        if (st->binding.postgis_schema) PQfreemem((void *)st->binding.postgis_schema);
        st->binding = (DbBinding){ 0 };
        copy_err(err, errsz, "invalid installed Laplace/PostGIS extension catalog");
        return NULL;
    }
    st->has_binding = 1;
    return &st->binding;
}

static char *expand_sql(const char *sql, const DbBinding *b) {
    lp_buf out = { 0 };
    bool literal = false;
    while (*sql) {
        const char *rep = NULL;
        size_t tok = 0;
        if (b && !strncmp(sql, "@extschema@", 11)) {
            rep = b->native_schema;
            tok = 11;
        } else if (b && !strncmp(sql, "@postgis@", 9)) {
            rep = b->postgis_schema;
            tok = 9;
        }
        if (rep) {
            for (const char *p = rep; *p; p++) {
                lp_buf_put(&out, p, 1);
                if (literal && *p == '\'') lp_buf_put(&out, p, 1);
            }
            sql += tok;
        } else {
            if (*sql == '\'') {
                if (literal && sql[1] == '\'') {
                    lp_buf_put(&out, sql, 2);
                    sql += 2;
                    continue;
                }
                literal = !literal;
            }
            lp_buf_put(&out, sql++, 1);
        }
    }
    lp_buf_put(&out, "", 1);
    return (char *)out.b;
}

static int prepare_contract_stmt(PGconn *pg, ContractConn *st, LpSqlId id, char *stmt_name, size_t nsz, char *err, size_t errsz) {
    if (id < 0 || id >= LP_SQL_COUNT) {
        copy_err(err, errsz, "invalid SQL catalog identifier");
        return -1;
    }
    snprintf(stmt_name, nsz, "lp_c%03d", (int)id);
    ContractPlan *plan = &st->plans[id];
    if (plan->ready) return 0;

    const char *tmpl = SQL_META[id].sql;
    if (!plan->expanded) {
        const DbBinding *b = NULL;
        if (needs_extension_binding(tmpl)) {
            b = db_contract_binding(pg, err, errsz);
            if (!b) return -1;
        }
        plan->expanded = expand_sql(tmpl, b);
    }
    PGresult *p = PQprepare(pg, stmt_name, plan->expanded, SQL_META[id].arity, NULL);
    if (!p || PQresultStatus(p) != PGRES_COMMAND_OK) {
        copy_err(err, errsz, PQerrorMessage(pg));
        PQclear(p);
        return -1;
    }
    PQclear(p);
    plan->ready = 1;
    return 0;
}

PGresult *db_call_try(PGconn *pg, LpSqlId id, Args *a, char *err, size_t errsz) {
    if (!pg || PQstatus(pg) != CONNECTION_OK) {
        copy_err(err, errsz, pg ? PQerrorMessage(pg) : "no database connection");
        return NULL;
    }
    if (id < 0 || id >= LP_SQL_COUNT) {
        copy_err(err, errsz, "invalid SQL catalog identifier");
        return NULL;
    }
    int n = a ? a->n : 0;
    if (n != SQL_META[id].arity) {
        if (err && errsz) snprintf(err, errsz, "SQL contract %s expects %d args, got %d", SQL_META[id].name, SQL_META[id].arity, n);
        return NULL;
    }

    ContractConn *st = contract_conn(pg);
    if (SQL_META[id].arity == 0 && is_utility_sql(SQL_META[id].sql)) {
        ContractPlan *plan = &st->plans[id];
        if (!plan->expanded) {
            const DbBinding *b = NULL;
            if (needs_extension_binding(SQL_META[id].sql)) {
                b = db_contract_binding(pg, err, errsz);
                if (!b) return NULL;
            }
            plan->expanded = expand_sql(SQL_META[id].sql, b);
        }
        PGresult *r = PQexec(pg, plan->expanded);
        db_traffic_count_command();
        ExecStatusType est = r ? PQresultStatus(r) : PGRES_FATAL_ERROR;
        if (est != PGRES_TUPLES_OK && est != PGRES_COMMAND_OK) {
            copy_err(err, errsz, PQerrorMessage(pg));
            PQclear(r);
            return NULL;
        }
        return r;
    }

    char stmt_name[24];
    if (prepare_contract_stmt(pg, st, id, stmt_name, sizeof stmt_name, err, errsz) != 0) return NULL;

    const char *vals[ARGS_MAX] = { 0 };
    int lens[ARGS_MAX] = { 0 }, fmts[ARGS_MAX] = { 0 };
    uint32_t id_oid = 0;
    for (int i = 0; i < n; i++) {
        SqlArg *s = &a->a[i];
        if (s->binary == SQL_NULL) {
            vals[i] = NULL;
            lens[i] = 0;
            fmts[i] = 1;
        } else if (s->binary == SQL_TEXT) {
            vals[i] = (const char *)s->b.b;
            lens[i] = 0;
            fmts[i] = 0;
        } else {
            if (s->binary == SQL_ID_ARRAY && s->b.n >= 12) {
                if (!id_oid) {
                    const DbBinding *b = db_contract_binding(pg, err, errsz);
                    if (!b) return NULL;
                    id_oid = b->id_type;
                }
                uint8_t *p = s->b.b + 8;
                p[0] = (uint8_t)(id_oid >> 24);
                p[1] = (uint8_t)(id_oid >> 16);
                p[2] = (uint8_t)(id_oid >> 8);
                p[3] = (uint8_t)id_oid;
            }
            vals[i] = s->b.b ? (const char *)s->b.b : "";
            lens[i] = (int)s->b.n;
            fmts[i] = 1;
        }
    }

    PGresult *r = PQexecPrepared(pg, stmt_name, n, vals, lens, fmts, 1);
    db_traffic_count_command();
    ExecStatusType est = r ? PQresultStatus(r) : PGRES_FATAL_ERROR;
    if (est != PGRES_TUPLES_OK && est != PGRES_COMMAND_OK) {
        copy_err(err, errsz, PQerrorMessage(pg));
        PQclear(r);
        return NULL;
    }
    return r;
}

PGresult *db_call(PGconn *pg, LpSqlId id, Args *a) {
    char err[512];
    PGresult *r = db_call_try(pg, id, a, err, sizeof err);
    if (!r) {
        fprintf(stderr, "postgres contract %s: %s\n", lp_sql_name(id), err);
        exit(1);
    }
    return r;
}

int db_exec_contract_try(PGconn *pg, LpSqlId id, Args *a, char *err, size_t errsz) {
    PGresult *r = db_call_try(pg, id, a, err, errsz);
    if (!r) return -1;
    PQclear(r);
    return 0;
}

int db_exec_contract(PGconn *pg, LpSqlId id, Args *a) {
    PGresult *r = db_call(pg, id, a);
    PQclear(r);
    return 0;
}

int db_tx(PGconn *pg, LpSqlId id) {
    return db_exec_contract(pg, id, NULL);
}

int db_tx_try(PGconn *pg, LpSqlId id, char *err, size_t errsz) {
    return db_exec_contract_try(pg, id, NULL, err, errsz);
}

static int valid_ident_part(const char *s, size_t n) {
    if (!n || n > 63) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static int format_escaped_rel(PGconn *pg, const char *rel, char *out, size_t osz) {
    if (!rel || !*rel) return -1;
    const char *dot = strchr(rel, '.');
    if (dot) {
        if (strchr(dot + 1, '.')) return -1;
        size_t sn = (size_t)(dot - rel), tn = strlen(dot + 1);
        if (!valid_ident_part(rel, sn) || !valid_ident_part(dot + 1, tn)) return -1;
        char *qs = PQescapeIdentifier(pg, rel, sn), *qt = PQescapeIdentifier(pg, dot + 1, tn);
        if (!qs || !qt) { PQfreemem(qs); PQfreemem(qt); return -1; }
        int w = snprintf(out, osz, "%s.%s", qs, qt);
        PQfreemem(qs);
        PQfreemem(qt);
        return (w > 0 && (size_t)w < osz) ? 0 : -1;
    }
    size_t tn = strlen(rel);
    if (!valid_ident_part(rel, tn)) return -1;
    char *qt = PQescapeIdentifier(pg, rel, tn);
    if (!qt) return -1;
    int w = snprintf(out, osz, "%s", qt);
    PQfreemem(qt);
    return (w > 0 && (size_t)w < osz) ? 0 : -1;
}

static int exec_direct(PGconn *pg, const char *sql) {
    PGresult *r = PQexec(pg, sql);
    db_traffic_count_command();
    ExecStatusType st = r ? PQresultStatus(r) : PGRES_FATAL_ERROR;
    int ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
    if (!ok) fprintf(stderr, "%s", PQerrorMessage(pg));
    PQclear(r);
    return ok ? 0 : -1;
}

int db_stage_drop(PGconn *pg, const char *const *tables, size_t n) {
    if (!n) return 0;
    lp_buf sql = { 0 };
    lp_buf_put(&sql, "DROP TABLE ", 11);
    for (size_t i = 0; i < n; i++) {
        char q[200];
        if (format_escaped_rel(pg, tables[i], q, sizeof q) != 0) {
            lp_buf_free(&sql);
            return -1;
        }
        if (i) lp_buf_put(&sql, ",", 1);
        lp_buf_put(&sql, q, strlen(q));
    }
    lp_buf_put(&sql, "", 1);
    int rc = exec_direct(pg, (const char *)sql.b);
    lp_buf_free(&sql);
    return rc;
}

int db_stage_create_pair(PGconn *pg, const char *entity_tbl, const char *path_tbl) {
    char qe[200], qp[200], sql[1152];
    if (format_escaped_rel(pg, entity_tbl, qe, sizeof qe) != 0 ||
        format_escaped_rel(pg, path_tbl, qp, sizeof qp) != 0)
        return -1;
    int n = snprintf(sql, sizeof sql,
        "CREATE UNLOGGED TABLE %s (LIKE stage.entity INCLUDING DEFAULTS, present boolean NOT NULL) WITH (autovacuum_enabled = false);"
        "ALTER TABLE %s ALTER coord DROP NOT NULL, ALTER hilbert DROP NOT NULL;"
        "CREATE UNLOGGED TABLE %s (LIKE stage.physicality INCLUDING DEFAULTS, present boolean NOT NULL) WITH (autovacuum_enabled = false);"
        "ALTER TABLE %s ALTER path DROP NOT NULL",
        qe, qe, qp, qp);
    if (n <= 0 || (size_t)n >= sizeof sql) return -1;
    return exec_direct(pg, sql);
}

int db_stage_create_series(PGconn *pg, const char *delta_tbl, const char *pending_tbl, const char *wit_tbl, const char *affected_tbl) {
    const DbBinding *b = db_contract_binding(pg, NULL, 0);
    if (!b) return -1;
    char qd[200], qp[200], qw[200], qa[200], sql[2304];
    if (format_escaped_rel(pg, delta_tbl, qd, sizeof qd) != 0 ||
        format_escaped_rel(pg, pending_tbl, qp, sizeof qp) != 0 ||
        format_escaped_rel(pg, wit_tbl, qw, sizeof qw) != 0 ||
        format_escaped_rel(pg, affected_tbl, qa, sizeof qa) != 0)
        return -1;
    int n = snprintf(sql, sizeof sql,
        "CREATE UNLOGGED TABLE %s (LIKE stage.attestation INCLUDING DEFAULTS, wins double precision NOT NULL) WITH (autovacuum_enabled = false);"
        "CREATE UNLOGGED TABLE %s (LIKE stage.attestation INCLUDING DEFAULTS, wins double precision NOT NULL,"
        " CHECK(games > 0 AND games <= 9007199254740992 AND wins >= 0 AND wins <= games)) WITH (autovacuum_enabled = false);"
        "CREATE UNIQUE INDEX ON %s (claim, witness, lineage) NULLS NOT DISTINCT;"
        "CREATE UNLOGGED TABLE %s (id %s.blake3 PRIMARY KEY, lineage %s.blake3, trust double precision NOT NULL) WITH (autovacuum_enabled = false);"
        "CREATE UNLOGGED TABLE %s (claim %s.blake3 PRIMARY KEY) WITH (autovacuum_enabled = false)",
        qd, qp, qp, qw, b->native_schema, b->native_schema, qa, b->native_schema);
    if (n <= 0 || (size_t)n >= sizeof sql) return -1;
    return exec_direct(pg, sql);
}

int db_stage_create_attestation(PGconn *pg, const char *tbl) {
    char qt[200], sql[448];
    if (format_escaped_rel(pg, tbl, qt, sizeof qt) != 0) return -1;
    snprintf(sql, sizeof sql, "CREATE UNLOGGED TABLE %s (LIKE stage.attestation INCLUDING DEFAULTS) WITH (autovacuum_enabled = false)", qt);
    return exec_direct(pg, sql);
}

int db_ddl_begin_maintenance(PGconn *pg) {
    const DbBinding *b = db_contract_binding(pg, NULL, 0);
    if (!b) return -1;
    char sql[256];
    snprintf(sql, sizeof sql,
        "BEGIN ISOLATION LEVEL REPEATABLE READ;"
        " SELECT pg_catalog.pg_advisory_xact_lock(1279348803, -2);"
        " LOCK TABLE %s.attestation IN SHARE ROW EXCLUSIVE MODE",
        b->native_schema);
    return exec_direct(pg, sql);
}

int db_ddl_create_db(PGconn *pg, const char *dbname) {
    char *id = PQescapeIdentifier(pg, dbname, strlen(dbname)), sql[512];
    if (!id) return -1;
    snprintf(sql, sizeof sql, "CREATE DATABASE %s", id);
    PQfreemem(id);
    return exec_direct(pg, sql);
}

int db_ddl_drop_db(PGconn *pg, const char *dbname) {
    char *id = PQescapeIdentifier(pg, dbname, strlen(dbname)), sql[512];
    if (!id) return -1;
    snprintf(sql, sizeof sql, "DROP DATABASE %s WITH (FORCE)", id);
    PQfreemem(id);
    return exec_direct(pg, sql);
}

int db_ddl_create_schema(PGconn *pg, const char *schema) {
    char *id = PQescapeIdentifier(pg, schema, strlen(schema)), sql[512];
    if (!id) return -1;
    snprintf(sql, sizeof sql, "CREATE SCHEMA IF NOT EXISTS %s", id);
    PQfreemem(id);
    return exec_direct(pg, sql);
}

int db_ddl_create_postgis(PGconn *pg) {
    return exec_direct(pg, "CREATE EXTENSION IF NOT EXISTS postgis SCHEMA public");
}

int db_ddl_create_laplace(PGconn *pg, const char *schema) {
    char *id = PQescapeIdentifier(pg, schema, strlen(schema)), sql[512];
    if (!id) return -1;
    snprintf(sql, sizeof sql, "CREATE EXTENSION IF NOT EXISTS laplace SCHEMA %s", id);
    PQfreemem(id);
    return exec_direct(pg, sql);
}

int db_ddl_update_laplace(PGconn *pg, const char *version) {
    char *lit = PQescapeLiteral(pg, version, strlen(version)), sql[512];
    if (!lit) return -1;
    snprintf(sql, sizeof sql, "SET lock_timeout = '20s'; ALTER EXTENSION laplace UPDATE TO %s", lit);
    PQfreemem(lit);
    return exec_direct(pg, sql);
}

int db_ddl_alter_db_setting(PGconn *pg, const char *dbname, const char *setting, const char *value, int quote_text) {
    if (!valid_ident_part(setting, strlen(setting)) && !strchr(setting, '.')) return -1;
    char qset[160];
    if (format_escaped_rel(pg, setting, qset, sizeof qset) != 0) return -1;
    char *qdb = PQescapeIdentifier(pg, dbname, strlen(dbname));
    char *qval = quote_text ? PQescapeLiteral(pg, value, strlen(value)) : NULL;
    if (!qdb || (quote_text && !qval)) {
        PQfreemem(qdb);
        if (qval) PQfreemem(qval);
        return -1;
    }
    lp_buf sql = { 0 };
    char head[512];
    int n = snprintf(head, sizeof head, "ALTER DATABASE %s SET %s = ", qdb, qset);
    lp_buf_put(&sql, head, (size_t)n);
    const char *v = quote_text ? qval : value;
    lp_buf_put(&sql, v, strlen(v));
    lp_buf_put(&sql, "", 1);
    int rc = exec_direct(pg, (const char *)sql.b);
    lp_buf_free(&sql);
    PQfreemem(qdb);
    if (qval) PQfreemem(qval);
    return rc;
}

int db_ddl_install_restore_bindings(PGconn *pg, const char *module_path) {
    const DbBinding *b = db_contract_binding(pg, NULL, 0);
    if (!b) return -1;
    char *lit = PQescapeLiteral(pg, module_path, strlen(module_path));
    if (!lit) return -1;
    lp_buf sql = { 0 };
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
        "CREATE OR REPLACE FUNCTION %s.laplace_restore_function_bindings() RETURNS void"
        " AS %s, 'laplace_restore_function_bindings' LANGUAGE c STRICT",
        b->native_schema, lit);
    PQfreemem(lit);
    if (n <= 0) { lp_buf_free(&sql); return -1; }
    lp_buf_put(&sql, buf, (size_t)n + 1);
    int rc = exec_direct(pg, (const char *)sql.b);
    lp_buf_free(&sql);
    return rc;
}

void db_copy_open_stage_pair(lp_copy *c, PGconn *pg, const char *entity_tbl, const char *path_tbl) {
    char qe[200], qp[200], sql[512];
    if (format_escaped_rel(pg, entity_tbl, qe, sizeof qe) != 0 ||
        format_escaped_rel(pg, path_tbl, qp, sizeof qp) != 0) {
        fprintf(stderr, "invalid stage pair table identifiers\n");
        exit(1);
    }
    snprintf(sql, sizeof sql,
        "BEGIN;COPY %s (id,tier,coord,hilbert,mask,present) FROM STDIN (FORMAT binary);"
        "COPY %s (entity,tier,path,mask,present) FROM STDIN (FORMAT binary);COMMIT",
        qe, qp);
    copy_open(c, pg, sql);
}

void db_copy_open_stage_delta(lp_copy *c, PGconn *pg, const char *delta_tbl) {
    char qd[200], sql[384];
    if (format_escaped_rel(pg, delta_tbl, qd, sizeof qd) != 0) {
        fprintf(stderr, "invalid stage delta table identifier\n");
        exit(1);
    }
    snprintf(sql, sizeof sql,
        "BEGIN;COPY %s (claim,witness,lineage,games,tokens,score,\"position\",trust,wins) FROM STDIN (FORMAT binary)",
        qd);
    copy_open(c, pg, sql);
}

void db_copy_open_stage_witnesses(lp_copy *c, PGconn *pg, const char *wit_tbl) {
    char qw[200], sql[256];
    if (format_escaped_rel(pg, wit_tbl, qw, sizeof qw) != 0) {
        fprintf(stderr, "invalid stage witness table identifier\n");
        exit(1);
    }
    snprintf(sql, sizeof sql, "COPY %s (id,lineage,trust) FROM STDIN (FORMAT binary)", qw);
    copy_open(c, pg, sql);
}
