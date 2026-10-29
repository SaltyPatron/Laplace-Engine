/* laplace: Laplace itself, one program.
 *
 *   laplace tier0   generate tier 0 from the Unicode data, and print its fingerprint
 *   laplace highway generate the highway: the types the resources list, and the mappings between them
 *   laplace deploy  make a database a Laplace database: extensions, schema, semantics, settings
 *   laplace ingest  files through their recipes: decompose, deduplicate trunk to leaf, record, attest
 *   laplace forget  what one witness said, taken back out
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
#include "query.h"
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
    if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "Database connection failed: %s", PQerrorMessage(pg)); exit(1); }
    db_notices(pg);
    return pg;
}
/* A read sees one world. Ingestion folds standings as it goes, and a read is many statements on many connections (the
 * pool reads every leaf at once), each of which would otherwise see the database as it was when that statement began:
 * hop 1 and hop 5 of one pass could read different standings. A read command opens one repeatable-read, read-only
 * transaction and exports its snapshot; every pooled connection takes the same snapshot (db_snapshot, read.c), so every
 * statement of the command reads the database as it was at one instant. */
static char snapshot[64];
const char *db_snapshot(void){ return snapshot[0] ? snapshot : NULL; }
PGconn *db_read(const char *conninfo){
    PGconn *pg = db_connect(conninfo);
    PGresult *r = db_exec(pg, "BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY");
    if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "read: %s", PQerrorMessage(pg)); exit(1); } PQclear(r);
    r = db_exec(pg, "SELECT pg_export_snapshot()");
    if (PQresultStatus(r) != PGRES_TUPLES_OK || !PQntuples(r)) { fprintf(stderr, "snapshot: %s", PQerrorMessage(pg)); exit(1); }
    snprintf(snapshot, sizeof snapshot, "%s", PQgetvalue(r, 0, 0)); PQclear(r);
    return pg;
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

