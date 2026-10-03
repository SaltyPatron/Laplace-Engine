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
#include "laplace_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *name, const char *dflt){ const char *v = getenv(name); return v && *v ? v : dflt; }
const char *laplace_db(void){ return env_or("LAPLACE_CONNINFO", LAPLACE_CONNINFO_DEFAULT); }
const char *laplace_recipes(void){ return env_or("LAPLACE_RECIPES", LAPLACE_RECIPES_DEFAULT); }
const char *laplace_grammars(void){ return env_or("LAPLACE_GRAMMARS", LAPLACE_GRAMMARS_DEFAULT); }
const char *laplace_ucd(void){ return env_or("LAPLACE_UCD", LAPLACE_UCD_DEFAULT); }

uint32_t id_oid;
static char noted_conn[8192];
const char *db_noted(void){ return noted_conn; }
PGconn *db_connect(const char *conninfo){
    if (!conninfo) conninfo = "";
    /* pool_open passes db_noted(), which is this buffer: copying it onto itself is undefined */
    if (conninfo != noted_conn) snprintf(noted_conn, sizeof noted_conn, "%s", conninfo);
    PGconn *pg = PQconnectdb(conninfo);
    if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "%s (LAPLACE_CONNINFO: %s)\n", PQerrorMessage(pg), conninfo); exit(1); }
    PQclear(PQexec(pg, "SET client_min_messages = warning"));
    /* A named leaf keeps parallel_workers at 0, so a one-partition lookup stays one process.
     * A statement on the parent is many leaves: Gather uses the workers setup.sh already sized.
     * The database default stays off (a 1 ms lookup must not start workers). Measured on
     * laplace_containers of Sherlock Holmes: 3780 ms with it off, 573 ms with it on, 101 rows either way. */
    PQclear(PQexec(pg, "SET enable_parallel_append = on"));
    if (!id_oid) { PGresult *r = PQexec(pg, "SELECT 'blake3'::regtype::oid");          /* a database not deployed yet has no such type */
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

static const struct { const char *name; int (*run)(int, char **); const char *what; } CMD[] = {
    { "tier0",  cmd_tier0,  "generate tier 0 from the Unicode data" },
    { "flags",  cmd_flags,  "generate the flags that go with tier 0, from the standard's own lists" },
    { "highway", cmd_highway, "generate the highway: the types, from the resources that list them, and the mappings between them" },
    { "deploy", cmd_deploy, "make a database a Laplace database" },
    { "sources", cmd_sources, "the sources there are recipes for, in the order they go in" },
    { "ingest", cmd_ingest, "a source by its name, or files, through their recipes" },
    { "forget", cmd_forget, "what one witness attested, taken back out" },
    { "sweep",  cmd_sweep,  "whatever nothing holds, removed" },
    { "index",  cmd_index,  "the indexes, if one was dropped: deploy makes them" },
    { "structure", cmd_structure, "a file's tree, as a layout parts it" },
    { "tree",   cmd_tree,   "a file's syntax tree, as its recipe's grammar reads it" },
    { "text",   cmd_text,   "a text's ID, coordinate and constituents, computed here" },
    { "pull",   cmd_pull,   "the forward pass: a prompt, and the segments and strands its firmware takes" },
    { "turn",   cmd_turn,   "a turn of a session: the one forward program, RESOLVE to WITNESS, each emitted constituent changing the next" },
    { "hop",    cmd_hop,    "everything attested about an entity" },
    { "translate", cmd_translate, "a word up to its concepts and down into other languages" },
    { "degrees", cmd_degrees, "how far one entity is from another, over rated claims" },
    { "fills",  cmd_fills,  "what follows a phrase" },
    { "status", cmd_status, "what a database holds" },
    { "bench",  cmd_bench,  "every native operation, measured" },
    { "model",  cmd_model,  "a transformer checkpoint read as testimony" },
};

int main(int argc, char **argv){
    if (argc > 1) for (size_t i = 0; i < sizeof CMD / sizeof *CMD; i++) if (!strcmp(argv[1], CMD[i].name)) return CMD[i].run(argc - 1, argv + 1);
    fprintf(stderr, "laplace <command> [options]\n\n");
    for (size_t i = 0; i < sizeof CMD / sizeof *CMD; i++) fprintf(stderr, "  %-10s %s\n", CMD[i].name, CMD[i].what);
    fprintf(stderr, "\n  database  %s\n  tier 0    %s\n  recipes   %s\n  grammars  %s\n  firmware  %s\n", laplace_db(), lp_tier0_path(), laplace_recipes(), laplace_grammars(), firmware_path());
    return 2;
}
