/* laplace: Laplace itself, one program.
 *
 *   laplace tier0   generate tier 0 from the Unicode data, and print its fingerprint
 *   laplace highway generate the highway: the types the resources list, and the mappings between them
 *   laplace deploy  make a database a Laplace database: extensions, schema, semantics, settings
 *   laplace ingest  files through their recipes: decompose, deduplicate trunk to leaf, record, attest
 *   laplace forget  what one witness attested, taken back out
 *   laplace sweep   whatever nothing holds, removed
 *   laplace index   build the indexes after a bulk load
 *   laplace tree    a file's syntax tree as its recipe's grammar reads it, for writing recipes
 *   laplace text    a text's ID, tier, coordinate and constituents, computed here without the database
 *   laplace hop     everything attested about an entity, by how hard each strand tugs back
 *   laplace translate  a word up to its concepts and down into other languages
 *   laplace degrees how far one entity is from another over rated claims, or what is nearest one
 *   laplace fills   what follows a phrase, counted across every source
 *   laplace status  what a database holds
 *   laplace bench   every native operation measured on this machine
 *   laplace model   a transformer checkpoint read as "b beats c given a"
 *
 * Where things are comes from the environment (laplace.env), else from what the engine was built with. */
#include "engine.h"
#include "op.h"
#include "laplace_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

static const char *env_or(const char *name, const char *dflt){ const char *v = getenv(name); return v && *v ? v : dflt; }
const char *laplace_db(void){ return env_or("LAPLACE_CONNINFO", LAPLACE_CONNINFO_DEFAULT); }
const char *laplace_recipes(void){ return env_or("LAPLACE_RECIPES", LAPLACE_RECIPES_DEFAULT); }
const char *laplace_grammars(void){ return env_or("LAPLACE_GRAMMARS", LAPLACE_GRAMMARS_DEFAULT); }
const char *laplace_ucd(void){ return env_or("LAPLACE_UCD", LAPLACE_UCD_DEFAULT); }

uint32_t id_oid;
static char noted_conn[8192];
const char *db_noted(void){ return noted_conn; }
static PGconn *connect_one(const char *conninfo);
PGconn *db_connect(const char *conninfo){
    if (!conninfo) conninfo = "";
    /* pool_open passes db_noted(), which is this buffer: copying it onto itself is undefined */
    if (conninfo != noted_conn) snprintf(noted_conn, sizeof noted_conn, "%s", conninfo);
    return connect_one(conninfo);
}
/* Many connections at once. A backend takes its time to start (measured on Windows, where each is a new process that
 * loads the preloaded libraries again: about 1.5 s under load, 0.5 s idle), so 32 opened one after another were 16 s
 * under every batch and under the source's trunk, and 49 were half the merge's floor. The first is opened alone (it
 * notes the conninfo and the id type's oid); the rest open together. */
void db_connect_many(const char *conninfo, int n, PGconn **out){
    if (n <= 0) return;
    out[0] = db_connect(conninfo);
    #pragma omp parallel for schedule(static, 1)
    for (int i = 1; i < n; i++) out[i] = connect_one(db_noted());
}
static PGconn *connect_one(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo);
    if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "%s (LAPLACE_CONNINFO: %s)\n", PQerrorMessage(pg), conninfo); exit(1); }
    PQclear(PQexec(pg, "SET client_min_messages = warning"));
    /* A named leaf keeps parallel_workers at 0, so a one-partition lookup stays one process.
     * A statement on the parent is many leaves: Gather uses the workers setup.sh already sized.
     * The database default stays off (a 1 ms lookup must not start workers). Measured on
     * laplace_containers of Sherlock Holmes: 3780 ms with it off, 573 ms with it on, 101 rows either way. */
    PQclear(PQexec(pg, "SET enable_parallel_append = on"));
    if (!id_oid && !omp_in_parallel()) { PGresult *r = PQexec(pg, "SELECT 'blake3'::regtype::oid");          /* a database not deployed yet has no such type */
                   if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r)) id_oid = (uint32_t)strtoul(PQgetvalue(r, 0, 0), NULL, 10); PQclear(r); }
    return pg;
}
/* A query that is asked again and again is planned once for the connection: over the partitions of entity and
 * physicality, planning a lookup takes several times what answering it does. Results come in binary. */
PGresult *db_ask(PGconn *pg, const char *sql, int n, const char *const *v, const int *l, const int *f){
    static struct { PGconn *pg; const char *sql; } known[64]; static int nknown; int k = 0;
    #pragma omp critical(db_ask)
    {
        while (k < nknown && !(known[k].pg == pg && known[k].sql == sql)) k++;
        if (k == nknown && nknown < 64) {
            char name[16]; snprintf(name, sizeof name, "q%d", k);
            if (!nknown || known[nknown - 1].pg != pg) { int had = 0; for (int i = 0; i < nknown; i++) had |= known[i].pg == pg; if (!had) PQclear(PQexec(pg, "SET plan_cache_mode = force_generic_plan")); }
            PGresult *r = PQprepare(pg, name, sql, n, NULL);
            if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "prepare: %s", PQerrorMessage(pg)); exit(1); }
            PQclear(r); known[nknown].pg = pg; known[nknown++].sql = sql;
        }
    }
    if (k >= 64) return PQexecParams(pg, sql, n, NULL, v, l, f, 1);
    char name[16]; snprintf(name, sizeof name, "q%d", k);
    return PQexecPrepared(pg, name, n, v, l, f, 1);
}

int main(int argc, char **argv){
    if (argc > 1 && !strcmp(argv[1], "describe")) { op_describe(stdout); return 0; }          /* the catalog, for every other surface */
    if (argc > 2 && !strcmp(argv[1], "help") && op_named(argv[2])) { op_usage(stdout, op_named(argv[2])); return 0; }
    const Op *op = argc > 1 ? op_named(argv[1]) : NULL;
    if (op) return op->run(argc - 1, argv + 1);
    fprintf(stderr, "laplace <operation> [options]       laplace help <operation>       laplace describe (every operation, as JSON)\n\n");
    for (size_t i = 0; i < NOPS; i++) fprintf(stderr, "  %-10s %s\n", OPS[i].name, OPS[i].what);
    fprintf(stderr, "\n  database  %s\n  tier 0    %s\n  recipes   %s\n  grammars  %s\n  firmware  %s\n", laplace_db(), lp_tier0_path(), laplace_recipes(), laplace_grammars(), firmware_path());
    return 2;
}

