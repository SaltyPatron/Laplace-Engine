#ifndef LAPLACE_PG_PLAN_H
#define LAPLACE_PG_PLAN_H
#include "libpq-fe.h"
#include <stdint.h>

typedef struct {
    const char *native_schema, *postgis_schema; /* quoted installed identifiers */
    uint32_t id_type, id_array;
} DbBinding;
const DbBinding *db_binding(PGconn *);
void db_notices(PGconn *);
/* Expand only explicit schema tokens, once per SQL template and connection. */
const char *db_sql(PGconn *,const char *sql);
PGresult *db_exec(PGconn *,const char *sql);
PGresult *db_exec_params(PGconn *,const char *sql,int n,const Oid *types,
                        const char *const *values,const int *lengths,const int *formats,int result_format);

/* Binary results; the caller owns PQclear. SQL text is copied into the
 * connection's plan registry. Reset/finish release that registry. One active
 * caller owns each PGconn, following libpq's concurrency contract. */
PGresult *db_ask(PGconn *,const char *sql,int n,const char *const *values,const int *lengths,const int *formats);
#endif
