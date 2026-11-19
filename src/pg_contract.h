/* Enum-indexed prepared binary SQL contract shared by Laplace-Engine and Laplace-MCP. */
#ifndef LAPLACE_PG_CONTRACT_H
#define LAPLACE_PG_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <libpq-fe.h>

typedef enum {
    LP_SQL_INVALID = -1,
#define LP_SQL(name, arity, sql) LP_SQL_##name,
#include "sql_catalog.def"
#undef LP_SQL
    LP_SQL_COUNT
} LpSqlId;

typedef struct Args Args;

const char *lp_sql_name(LpSqlId id);
int lp_sql_arity(LpSqlId id);
const char *lp_sql_template(LpSqlId id);

/* Typed binary parameter builders sharing Args (declared in engine.h). */
void arg_null(Args *a);
void arg_bool(Args *a, bool v);
void arg_i16(Args *a, int16_t v);
void arg_i32(Args *a, int32_t v);
void arg_i64(Args *a, int64_t v);
void arg_f64_bin(Args *a, double v);
void arg_f64_array(Args *a, const double *vals, size_t n);
void arg_oid(Args *a, uint32_t oid);
void arg_id(Args *a, const lp_id *id);
void arg_text_bin(Args *a, const char *s, size_t len);
void arg_bytea_array(Args *a, const uint8_t *const *items, const size_t *lens, size_t n);
void arg_i16_array(Args *a, const int16_t *vals, size_t n);
void arg_i32_array(Args *a, const int32_t *vals, size_t n);
void arg_i64_array(Args *a, const int64_t *vals, size_t n);
void arg_oid_array(Args *a, const uint32_t *vals, size_t n);

/* Thread-safe binding resolution and per-connection prepared binary execution. */
const DbBinding *db_contract_binding(PGconn *pg, char *err, size_t errsz);
PGresult *db_call(PGconn *pg, LpSqlId id, Args *a);
PGresult *db_call_try(PGconn *pg, LpSqlId id, Args *a, char *err, size_t errsz);
int db_exec_contract(PGconn *pg, LpSqlId id, Args *a);
int db_exec_contract_try(PGconn *pg, LpSqlId id, Args *a, char *err, size_t errsz);
int db_tx(PGconn *pg, LpSqlId id);
int db_tx_try(PGconn *pg, LpSqlId id, char *err, size_t errsz);

/* Traffic & binary array parameter builders (pg.c + pg_contract.c). */
void db_traffic_count_command(void);
void arg_text_array(Args *a, const char *const *items, size_t n);

/* Validated DDL helpers (all identifiers checked/escaped here; zero raw SQL in callers). */
int db_stage_drop(PGconn *pg, const char *const *tables, size_t n);
int db_stage_create_pair(PGconn *pg, const char *entity_tbl, const char *path_tbl);
int db_stage_create_series(PGconn *pg, const char *delta_tbl, const char *pending_tbl, const char *wit_tbl, const char *affected_tbl);
int db_stage_create_attestation(PGconn *pg, const char *tbl);
int db_ddl_begin_maintenance(PGconn *pg);
int db_ddl_create_db(PGconn *pg, const char *dbname);
int db_ddl_drop_db(PGconn *pg, const char *dbname);
int db_ddl_create_schema(PGconn *pg, const char *schema);
int db_ddl_create_postgis(PGconn *pg);
int db_ddl_create_laplace(PGconn *pg, const char *schema);
int db_ddl_update_laplace(PGconn *pg, const char *version);
int db_ddl_alter_db_setting(PGconn *pg, const char *dbname, const char *setting, const char *value, int quote_text);
int db_ddl_install_restore_bindings(PGconn *pg, const char *module_path);
void db_copy_open_stage_pair(lp_copy *c, PGconn *pg, const char *entity_tbl, const char *path_tbl);
void db_copy_open_stage_delta(lp_copy *c, PGconn *pg, const char *delta_tbl);
void db_copy_open_stage_witnesses(lp_copy *c, PGconn *pg, const char *wit_tbl);

/* Binary PostgreSQL wire column decoders (requires resultFormat = 1). */
static inline bool col_bool(const PGresult *r, int row, int col) {
    return PQgetlength(r, row, col) >= 1 && ((const uint8_t *)PQgetvalue(r, row, col))[0] != 0;
}
static inline int16_t col_i16(const PGresult *r, int row, int col) {
    const uint8_t *p = (const uint8_t *)PQgetvalue(r, row, col);
    return PQgetlength(r, row, col) >= 2 ? (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]) : 0;
}
static inline int32_t col_i32(const PGresult *r, int row, int col) {
    const uint8_t *p = (const uint8_t *)PQgetvalue(r, row, col);
    return PQgetlength(r, row, col) >= 4
        ? (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3])
        : 0;
}
static inline uint32_t col_oid(const PGresult *r, int row, int col) {
    return (uint32_t)col_i32(r, row, col);
}
static inline int64_t col_i64(const PGresult *r, int row, int col) {
    const uint8_t *p = (const uint8_t *)PQgetvalue(r, row, col);
    if (PQgetlength(r, row, col) < 8) return 0;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u = (u << 8) | (uint64_t)p[i];
    return (int64_t)u;
}
static inline bool col_is_claim(const PGresult *r, int row, int col) {
    return PQgetlength(r, row, col) >= 5 && (((const uint8_t *)PQgetvalue(r, row, col))[4] & 0x80u) != 0;
}
static inline const char *col_str(const PGresult *r, int row, int col) {
    return PQgetisnull(r, row, col) ? "" : PQgetvalue(r, row, col);
}
static inline size_t col_i32_array(const PGresult *r, int row, int col, int32_t *out, size_t cap) {
    if (PQgetisnull(r, row, col)) return 0;
    const uint8_t *p = (const uint8_t *)PQgetvalue(r, row, col);
    size_t len = (size_t)PQgetlength(r, row, col);
    if (len < 12) return 0;
    int32_t ndim = (int32_t)lp_be(p, 4);
    if (ndim <= 0) return 0;
    size_t pos = 12 + (size_t)(ndim - 1) * 8;
    if (len < pos + 8) return 0;
    uint32_t n = (uint32_t)lp_be(p + 12, 4);
    pos = 12 + (size_t)ndim * 8;
    size_t count = n;
    size_t written = 0;
    for (size_t i = 0; i < count && pos + 4 <= len; i++) {
        int32_t elen = (int32_t)lp_be(p + pos, 4);
        pos += 4;
        if (elen < 0) {
            if (written < cap) out[written] = 0;
        } else {
            if (pos + (size_t)elen > len) break;
            if (written < cap && elen >= 4) out[written] = (int32_t)lp_be(p + pos, 4);
            pos += (size_t)elen;
        }
        written++;
    }
    return count;
}
static inline size_t col_i16_array(const PGresult *r, int row, int col, int16_t *out, size_t cap) {
    if (PQgetisnull(r, row, col)) return 0;
    const uint8_t *p = (const uint8_t *)PQgetvalue(r, row, col);
    size_t len = (size_t)PQgetlength(r, row, col);
    if (len < 12) return 0;
    int32_t ndim = (int32_t)lp_be(p, 4);
    if (ndim <= 0) return 0;
    if (len < 12 + (size_t)ndim * 8) return 0;
    uint32_t n = (uint32_t)lp_be(p + 12, 4);
    size_t pos = 12 + (size_t)ndim * 8;
    size_t written = 0;
    for (size_t i = 0; i < (size_t)n && pos + 4 <= len; i++) {
        int32_t elen = (int32_t)lp_be(p + pos, 4);
        pos += 4;
        if (elen < 0) {
            if (written < cap) out[written] = 0;
        } else {
            if (pos + (size_t)elen > len) break;
            if (written < cap && elen >= 2) out[written] = (int16_t)lp_be(p + pos, 2);
            pos += (size_t)elen;
        }
        written++;
    }
    return (size_t)n;
}

#endif
