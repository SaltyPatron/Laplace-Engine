#ifndef LAPLACE_PG_PLAN_H
#define LAPLACE_PG_PLAN_H
#include "libpq-fe.h"

/* Binary results; the caller owns PQclear. SQL text is copied into the
 * connection's plan registry. Reset/finish release that registry. One active
 * caller owns each PGconn, following libpq's concurrency contract. */
PGresult *db_ask(PGconn *,const char *sql,int n,const char *const *values,const int *lengths,const int *formats);
#endif
