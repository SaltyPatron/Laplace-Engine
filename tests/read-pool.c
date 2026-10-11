/* The production pool must respect the operator's OpenMP allocation. Connections
 * are isolated here; the database staging test exercises real PostgreSQL rights. */
#include "../src/read.c"

static int connections;
const char *db_noted(void) { return "fixture"; }
const char *laplace_db(void) { return "fixture"; }
const char *db_snapshot(void) { return NULL; }
PGconn *db_connect(const char *ignored) { (void)ignored; connections++; return NULL; }

int main(void) {
    omp_set_num_threads(2);
    pool_open();
    if (connections != 2 || npool != 2) {
        fprintf(stderr, "pool ignored configured threads: %d connections, %d slots\n", connections, npool);
        return 1;
    }
    free(pool);
    return 0;
}
