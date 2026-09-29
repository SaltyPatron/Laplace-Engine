/* laplace: Laplace itself, one program.
 *
 *   laplace tier0   generate tier 0 from the Unicode data, and print its fingerprint
 *   laplace deploy  make a database a Laplace database: extensions, schema, semantics, settings
 *   laplace ingest  files through their recipes: decompose, deduplicate trunk to leaf, record, attest
 *   laplace index   build the indexes after a bulk load
 *   laplace tree    a file's syntax tree as its recipe's grammar reads it, for writing recipes
 *   laplace text    a text's ID, tier, coordinate and constituents, computed here without the database
 *   laplace hop     everything attested about an entity, by how hard each strand tugs back
 *   laplace pull    fan out from an entity over rated claims, or find the chain between two
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
const char *laplace_sql(void){ return env_or("LAPLACE_SQL", LAPLACE_SQL_DEFAULT); }
const char *laplace_ucd(void){ return env_or("LAPLACE_UCD", LAPLACE_UCD_DEFAULT); }

PGconn *db_connect(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo);
    if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "%s (LAPLACE_CONNINFO: %s)\n", PQerrorMessage(pg), conninfo); exit(1); }
    PQclear(PQexec(pg, "SET client_min_messages = warning"));
    return pg;
}

static const struct { const char *name; int (*run)(int, char **); const char *what; } CMD[] = {
    { "tier0",  cmd_tier0,  "generate tier 0 from the Unicode data" },
    { "deploy", cmd_deploy, "make a database a Laplace database" },
    { "ingest", cmd_ingest, "files through their recipes" },
    { "index",  cmd_index,  "build the indexes after a bulk load" },
    { "tree",   cmd_tree,   "a file's syntax tree, as its recipe's grammar reads it" },
    { "text",   cmd_text,   "a text's ID, coordinate and constituents, computed here" },
    { "hop",    cmd_hop,    "everything attested about an entity" },
    { "pull",   cmd_pull,   "fan out over rated claims, or the chain between two entities" },
    { "fills",  cmd_fills,  "what follows a phrase" },
    { "status", cmd_status, "what a database holds" },
    { "bench",  cmd_bench,  "every native operation, measured" },
    { "model",  cmd_model,  "a transformer checkpoint read as testimony" },
};

int main(int argc, char **argv){
    if (argc > 1) for (size_t i = 0; i < sizeof CMD / sizeof *CMD; i++) if (!strcmp(argv[1], CMD[i].name)) return CMD[i].run(argc - 1, argv + 1);
    fprintf(stderr, "laplace <command> [options]\n\n");
    for (size_t i = 0; i < sizeof CMD / sizeof *CMD; i++) fprintf(stderr, "  %-8s %s\n", CMD[i].name, CMD[i].what);
    fprintf(stderr, "\n  database  %s\n  tier 0    %s\n  recipes   %s\n  grammars  %s\n", laplace_db(), lp_tier0_path(), laplace_recipes(), laplace_grammars());
    return 2;
}
