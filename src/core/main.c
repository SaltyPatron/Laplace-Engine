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
const char *laplace_recipes(void){ return env_or("LAPLACE_RECIPES", LAPLACE_RECIPES_DEFAULT); }
const char *laplace_grammars(void){ return env_or("LAPLACE_GRAMMARS", LAPLACE_GRAMMARS_DEFAULT); }
const char *laplace_ucd(void){ return env_or("LAPLACE_UCD", LAPLACE_UCD_DEFAULT); }

int main(int argc, char **argv){
    db_conninfo_default(LAPLACE_CONNINFO_DEFAULT);       /* laplace_db(): LAPLACE_CONNINFO, else this */
    os_stdio_linebuf(stdout);
    os_stdio_linebuf(stderr);
    if (argc > 1 && !strcmp(argv[1], "describe")) { op_describe(stdout); return 0; }          /* the catalog, for every other surface */
    if (argc > 2 && !strcmp(argv[1], "help") && op_named(argv[2])) { op_usage(stdout, op_named(argv[2])); return 0; }
    const Op *op = argc > 1 ? op_named(argv[1]) : NULL;
    if (op) return op->run(argc - 1, argv + 1);
    fprintf(stderr, "laplace <operation> [options]       laplace help <operation>       laplace describe (every operation, as JSON)\n\n");
    for (size_t i = 0; i < NOPS; i++) fprintf(stderr, "  %-10s %s\n", OPS[i].name, OPS[i].what);
    fprintf(stderr, "\n  database  %s\n  tier 0    %s\n  recipes   %s\n  grammars  %s\n  firmware  %s\n", laplace_db(), lp_tier0_path(), laplace_recipes(), laplace_grammars(), firmware_path());
    return 2;
}

