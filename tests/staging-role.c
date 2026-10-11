/* Execute the actual ingestion staging initializer with the caller's PG identity.
 * Unused Engine functions are removed by function-section GC in the test build. */
#include "../src/db.c"

int main(void) {
    const char *connection = getenv("LAPLACE_TEST_CONNINFO");
    if (!connection) return 2;
    PGconn *pg = PQconnectdb(connection);
    if (PQstatus(pg) != CONNECTION_OK) return 2;
    const char *role = getenv("LAPLACE_TEST_ROLE");
    if (!role || strlen(role) > 63) return 2;
    char *quoted = PQescapeIdentifier(pg, role, strlen(role));
    char command[256];
    snprintf(command, sizeof command, "SET SESSION AUTHORIZATION %s", quoted);
    PQfreemem(quoted);
    PGresult *identity = PQexec(pg, command);
    if (PQresultStatus(identity) != PGRES_COMMAND_OK) return 2;
    PQclear(identity);
    int ok = stage_open(pg);
    PQfinish(pg);
    return ok ? 0 : 1;
}
