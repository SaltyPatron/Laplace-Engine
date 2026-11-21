#ifndef LAPLACE_PG_PLAN_H
#define LAPLACE_PG_PLAN_H
#include "libpq-fe.h"
#include <stdint.h>

typedef struct {
    const char *native_schema, *postgis_schema; /* quoted installed identifiers */
    uint32_t id_type, id_array;
} DbBinding;
void db_notices(PGconn *);
/* A read scope owns one repeatable-read, read-only transaction. Import the
 * supplied snapshot, or export one when NULL. End releases only this scope. */
int db_read_begin(PGconn *,const char *snapshot);
const char *db_read_snapshot(PGconn *);
int db_read_end(PGconn *);
#endif
