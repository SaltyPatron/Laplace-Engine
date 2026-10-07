/* The catalog: every parameter defined once, every operation declared by the parameters it takes, its effects and
 * what it gives (op.h). A default left NULL is the operation's own and is not repeated here. */
#include "op.h"
#include "engine.h"
#include <string.h>

/* ---- the parameters. Each says only what the program itself says of it (its comments and usage). */
static const Param
    CONNINFO = { "conninfo", "-d", ARG_CONNINFO, "LAPLACE_CONNINFO", "the database, as a libpq connection string" },
    JOBS     = { "jobs", "-j", ARG_INT, NULL, "connections" },
    THREADS  = { "threads", "-j", ARG_INT, NULL, "threads" },
    LIMIT    = { "limit", "-n", ARG_INT, NULL, "N" },
    NODES    = { "nodes", "-n", ARG_INT, NULL, "nodes" },
    TIER0    = { "tier0", "-t", ARG_PATH, "LAPLACE_TIER0", "the tier-0 perf-cache" },
    RECIPES  = { "recipes", "-r", ARG_PATH, "LAPLACE_RECIPES", "the recipes directory, holding order and one directory per source" },
    GRAMMAR  = { "grammar", "-g", ARG_TEXT, NULL, "grammar" },
    UCD      = { "ucd", "-u", ARG_PATH, "LAPLACE_UCD", "UCD_ROOT: the Unicode data" },
    OUT_T0   = { "output", "-o", ARG_PATH, "LAPLACE_TIER0", "tier0.bin" },
    OUT_FL   = { "output", "-o", ARG_PATH, "LAPLACE_FLAGS, else tier 0's path with .flags", "tier0.flags" },
    OUT_HW   = { "output", "-o", ARG_PATH, "LAPLACE_HIGHWAY, else tier 0's path with .highway", "highway.bin" },
    FIRMWARE = { "firmware", "--firmware", ARG_PATH, "LAPLACE_FIRMWARE", "the firmware: how a standing is read, how far a search walks, how many claims are read and what is refused" },
    SEED     = { "seed", "--seed", ARG_INT, "the clock", "N: the seed that breaks ties between claims taken" },
    FAN      = { "fan", "--fan", ARG_INT, "the firmware's", "to measure against the firmware: over the firmware's fan" },
    K        = { "k", "--k", ARG_REAL, "the firmware's", "to measure against the firmware: over the firmware's k" },
    HOPS     = { "hops", "--hops", ARG_INT, "the firmware's", "H: to measure against the firmware" },
    PER_HOP  = { "per_hop", "--per-hop", ARG_REAL, "the firmware's", "C: to measure against the firmware" },
    BATCH    = { "batch", "--batch", ARG_INT, NULL, "B" },
    DRY      = { "dry", "--dry", ARG_FLAG, NULL, "dry" },
    EXCEPT   = { "except", "--except", ARG_FLAG, NULL, "every witness but these" },
    SOURCE   = { "source", "-s", ARG_TEXT, NULL, "the files named are this source's: a part of it at a time" },
    WHOLE    = { "whole", "--whole", ARG_FLAG, NULL, "after a run that was cut off: every node is looked for" },
    NO_LOAD  = { "no_load", "--no-load", ARG_FLAG, NULL, "no-load" },
    PLAN     = { "plan", "--plan", ARG_FLAG, NULL, "plan" },
    CLAIMS   = { "claims", "--claims", ARG_FLAG, NULL, "what the recipes attest, as text; nothing is loaded" },
    ENTITIES = { "entities", "--entities", ARG_FLAG, NULL, "every composition a sample makes, its tier, parts and text, new or recorded; nothing is loaded" },
    AS       = { "as", "--as", ARG_TEXT, "USER", "USER" },
    SESSION  = { "session", "--session", ARG_TEXT, NULL, "NAME" },
    READ     = { "read", "--read", ARG_FLAG, NULL, "a read: nothing is witnessed" },
    LAYERS   = { "layers", "--layers", ARG_INT, "every layer", "N" },
    ZMIN     = { "z", "--z", ARG_REAL, "3", "zmin" },
    CAP      = { "cap", "--cap", ARG_INT, "64", "K" },
    SAMPLE   = { "sample", "--sample", ARG_TEXT, "\" king\" \" dog\" \" Paris\" \" happy\" \" run\"", "word ..." },
    TEXT     = { "text", NULL, ARG_TEXT, NULL, "text" },
    PROMPT   = { "prompt", NULL, ARG_TEXT, NULL, "prompt" },
    PHRASE   = { "phrase", NULL, ARG_TEXT, NULL, "phrase" },
    FILE_    = { "file", NULL, ARG_PATH, NULL, "file" },
    WHAT     = { "what", NULL, ARG_TEXT, "every source in recipes/order", "a source by its name, or files" },
    WITNESS  = { "witness", NULL, ARG_TEXT, NULL, "witness" },
    WORD     = { "word", NULL, ARG_TEXT, NULL, "word" },
    LANG     = { "language", NULL, ARG_TEXT, NULL, "languages as the resources write them: en de fr ja" },
    FROM     = { "from", NULL, ARG_TEXT, NULL, "from" },
    TO       = { "to", NULL, ARG_TEXT, NULL, "to" },
    LAYOUT   = { "layout", NULL, ARG_PATH, NULL, "LAYOUT" },
    OUT_PGN  = { "output", "-o", ARG_PATH, "standard output", "file.pgn" },
    VERIFY   = { "verify", "--verify", ARG_FLAG, NULL, "every ID under them recomputed from its constituents with BLAKE3 alone, every leaf a codepoint" },
    ROOT     = { "id", NULL, ARG_TEXT, NULL, "a file's, a record's or a line's ID" },
    MODEL    = { "model_dir", NULL, ARG_PATH, NULL, "model_dir" };

/* ---- the operations */
#define END { NULL, 0, NULL, NULL }
#define O(p) { &(p), 0, NULL, NULL }
static const Arg A_NONE[] = { END };
static const Arg A_TIER0[] = { O(UCD), O(OUT_T0), END };
static const Arg A_FLAGS[] = { O(UCD), O(OUT_FL), END };
static const Arg A_HIGHWAY[] = { O(OUT_HW), END };
static const Arg A_INGEST[] = { O(CONNINFO), O(TIER0), O(RECIPES), O(THREADS), O(SOURCE), O(WHOLE), O(NO_LOAD), O(PLAN), O(CLAIMS), O(ENTITIES),
                                { &WHAT, ARG_MANY, NULL, NULL }, END };
static const Arg A_MERGE[] = { O(CONNINFO), { &JOBS, 0, NULL, "every processor" }, END };
static const Arg A_FORGET[] = { O(CONNINFO), O(JOBS), O(EXCEPT), { &WITNESS, ARG_REQUIRED | ARG_MANY, NULL, NULL }, END };
static const Arg A_SWEEP[] = { O(CONNINFO), O(JOBS), O(DRY), END };
static const Arg A_STRUCTURE[] = { { &NODES, 0, NULL, "60" }, { &LAYOUT, ARG_REQUIRED, NULL, NULL }, { &FILE_, ARG_REQUIRED, NULL, NULL }, END };
static const Arg A_TREE[] = { O(RECIPES), O(GRAMMAR), { &NODES, 0, NULL, "400" }, { &FILE_, ARG_REQUIRED, NULL, NULL }, END };
static const Arg A_TEXT[] = { { &TEXT, ARG_REQUIRED, NULL, NULL }, END };
static const Arg A_PULL[] = { O(CONNINFO), O(FIRMWARE), O(SEED), { &PROMPT, ARG_REQUIRED, NULL, NULL }, END };
static const Arg A_TURN[] = { O(CONNINFO), O(FIRMWARE), O(AS), O(SESSION), O(SEED), O(READ), { &PROMPT, ARG_REQUIRED, NULL, NULL }, END };
static const Arg A_HOP[] = { O(CONNINFO), { &LIMIT, 0, NULL, "24" }, O(FIRMWARE), O(FAN), O(K),
                             { &TEXT, ARG_REQUIRED | ARG_MANY, "text | subject predicate object (? for a part left open)", NULL }, END };
static const Arg A_TRANSLATE[] = { O(CONNINFO), { &LIMIT, 0, "concepts", "4" }, O(FIRMWARE), { &WORD, ARG_REQUIRED, NULL, NULL },
                                   { &LANG, ARG_REQUIRED, "from", NULL }, { &LANG, ARG_REQUIRED | ARG_MANY, "to...", NULL }, END };
static const Arg A_DEGREES[] = { O(CONNINFO), O(FIRMWARE), { &LIMIT, 0, NULL, "24" }, O(FAN), O(HOPS), { &BATCH, 0, NULL, "64" }, O(K), O(PER_HOP),
                                 { &FROM, ARG_REQUIRED, NULL, NULL }, { &TO, 0, NULL, NULL }, END };
static const Arg A_FILLS[] = { O(CONNINFO), { &LIMIT, 0, NULL, "12" }, O(TIER0), { &PHRASE, ARG_REQUIRED, NULL, NULL }, END };
static const Arg A_MODEL[] = { { &MODEL, ARG_REQUIRED, NULL, NULL }, O(LAYERS), O(ZMIN), O(CAP), { &SAMPLE, ARG_MANY, NULL, NULL }, END };
static const Arg A_PGN[] = { O(CONNINFO), O(TIER0), O(OUT_PGN), O(VERIFY), { &ROOT, ARG_REQUIRED | ARG_MANY, NULL, NULL }, END };

#define R_DB EFFECT_READS_DB
#define W_DB (EFFECT_READS_DB | EFFECT_WRITES_DB)
#define R_F EFFECT_READS_FILES
#define W_F (EFFECT_READS_FILES | EFFECT_WRITES_FILES)
const Op OPS[] = {
    { "tier0",     cmd_tier0,     "generate tier 0 from the Unicode data", A_TIER0, W_F, "tier 0's fingerprint" },
    { "flags",     cmd_flags,     "generate the flags that go with tier 0, from the standard's own lists", A_FLAGS, W_F, "text" },
    { "highway",   cmd_highway,   "generate the highway: the types, from the resources that list them, and the mappings between them", A_HIGHWAY, W_F, "text" },
    { "deploy",    cmd_deploy,    "make a database a Laplace database", A_NONE, W_DB | R_F, "text" },
    { "sources",   cmd_sources,   "the sources there are recipes for, in the order they go in", A_NONE, R_DB | R_F, "text" },
    { "ingest",    cmd_ingest,    "a source by its name, or files, through their recipes", A_INGEST, W_DB | R_F, "text" },
    { "merge",     cmd_merge,     "what the ingest staged, into the real tables at once", A_MERGE, W_DB, "text" },
    { "forget",    cmd_forget,    "what one witness attested, taken back out", A_FORGET, W_DB | EFFECT_DESTROYS, "text" },
    { "sweep",     cmd_sweep,     "whatever nothing holds, removed", A_SWEEP, W_DB | EFFECT_DESTROYS, "text" },
    { "index",     cmd_index,     "the indexes, if one was dropped: deploy makes them", A_NONE, W_DB, "text" },
    { "structure", cmd_structure, "a file's tree, as a layout parts it", A_STRUCTURE, R_F, "text" },
    { "tree",      cmd_tree,      "a file's syntax tree, as its recipe's grammar reads it", A_TREE, R_F, "text" },
    { "text",      cmd_text,      "a text's ID, coordinate and constituents, computed here", A_TEXT, R_F, "text" },
    { "pull",      cmd_pull,      "the forward pass: a prompt, and the segments and strands its firmware takes", A_PULL, R_DB | R_F, "text" },
    { "turn",      cmd_turn,      "a turn of a session: the one forward program, RESOLVE to WITNESS, each emitted constituent changing the next", A_TURN, W_DB | R_F, "text" },
    { "hop",       cmd_hop,       "everything attested about an entity", A_HOP, R_DB | R_F, "text" },
    { "translate", cmd_translate, "a word up to its concepts and down into other languages", A_TRANSLATE, R_DB | R_F, "text" },
    { "degrees",   cmd_degrees,   "how far one entity is from another, over rated claims", A_DEGREES, R_DB | R_F, "text" },
    { "fills",     cmd_fills,     "what follows a phrase", A_FILLS, R_DB | R_F, "text" },
    { "status",    cmd_status,    "what a database holds", A_NONE, R_DB, "text" },
    { "bench",     cmd_bench,     "every native operation, measured", A_NONE, R_F, "text" },
    { "model",     cmd_model,     "a transformer checkpoint read as testimony", A_MODEL, R_F, "text" },
    { "pgn",       cmd_pgn,       "a PGN file's games written again from the database alone: the read edge of chess", A_PGN, R_DB | R_F, "PGN" },
};
const size_t NOPS = sizeof OPS / sizeof *OPS;

const Op *op_named(const char *name){
    for (size_t i = 0; i < NOPS; i++) if (!strcmp(OPS[i].name, name)) return &OPS[i];
    return NULL;
}

static const char *type_name(ArgType t){
    static const char *const n[] = { "text", "integer", "number", "flag", "path", "conninfo" };
    return n[t];
}

void op_usage(FILE *out, const Op *op){
    fprintf(out, "laplace %s", op->name);
    for (const Arg *a = op->args; a->p; a++) {
        const Param *p = a->p; const char *many = a->use & ARG_MANY ? "..." : "";
        if (p->flag && p->type == ARG_FLAG) fprintf(out, " [%s]", p->flag);
        else if (p->flag) fprintf(out, " [%s %s%s]", p->flag, p->name, many);
        else if (a->use & ARG_REQUIRED) fprintf(out, " %s%s", p->name, many);
        else fprintf(out, " [%s%s]", p->name, many);
    }
    fprintf(out, "\n");
}

static void json_str(FILE *out, const char *s){
    if (!s) { fputs("null", out); return; }
    fputc('"', out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

void op_describe(FILE *out){
    static const struct { unsigned bit; const char *name; } effects[] = {
        { EFFECT_READS_DB, "reads_database" }, { EFFECT_WRITES_DB, "writes_database" }, { EFFECT_DESTROYS, "destroys" },
        { EFFECT_READS_FILES, "reads_files" }, { EFFECT_WRITES_FILES, "writes_files" } };
    fputs("{\"object\":\"laplace.operations\",\"operations\":[", out);
    for (size_t i = 0; i < NOPS; i++) {
        const Op *op = &OPS[i];
        fputs(i ? ",{" : "{", out);
        fputs("\"name\":", out); json_str(out, op->name);
        fputs(",\"what\":", out); json_str(out, op->what);
        fputs(",\"parameters\":[", out);
        for (const Arg *a = op->args; a->p; a++) {
            const Param *p = a->p;
            fputs(a == op->args ? "{" : ",{", out);
            fputs("\"name\":", out); json_str(out, p->name);
            fputs(",\"flag\":", out); json_str(out, p->flag);
            fputs(",\"type\":", out); json_str(out, type_name(p->type));
            fprintf(out, ",\"required\":%s,\"many\":%s", a->use & ARG_REQUIRED ? "true" : "false", a->use & ARG_MANY ? "true" : "false");
            fputs(",\"default\":", out); json_str(out, a->dflt ? a->dflt : p->dflt);
            fputs(",\"what\":", out); json_str(out, a->what ? a->what : p->what);
            fputc('}', out);
        }
        fputs("],\"effects\":[", out);
        int first = 1;
        for (size_t e = 0; e < sizeof effects / sizeof *effects; e++)
            if (op->effects & effects[e].bit) { if (!first) fputc(',', out); json_str(out, effects[e].name); first = 0; }
        fputs("],\"gives\":", out); json_str(out, op->gives);
        fputc('}', out);
    }
    fputs("]}\n", out);
}
