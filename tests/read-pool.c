/* The production pool must respect the operator's OpenMP allocation against
 * the disposable database provided by staging-role.py. */
#include "../src/read.c"

const char *db_noted(void) { return getenv("LAPLACE_TEST_CONNINFO"); }
const char *laplace_db(void) { return db_noted(); }
const char *db_snapshot(void) { return NULL; }

int main(void) {
    if (!db_noted() || !*db_noted()) {
        fprintf(stderr, "LAPLACE_TEST_CONNINFO must select the disposable database\n");
        return 1;
    }
    omp_set_num_threads(2);
    pool_open();
    if (npool != 2) {
        fprintf(stderr, "pool ignored configured threads: %d slots\n", npool);
        return 1;
    }
    for (int index = 0; index < npool; index++) {
        if (!pool[index] || PQstatus(pool[index]) != CONNECTION_OK) {
            fprintf(stderr, "pool connection %d is not ready\n", index);
            return 1;
        }
        PQfinish(pool[index]);
    }
    free(pool);
    return 0;
}
