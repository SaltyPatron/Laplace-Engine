/* The operations Laplace-Engine offers, declared once: what each takes, what it does to the world, and what it gives
 * back. The command line's usage is written from these declarations, and `laplace describe` prints them for every
 * other surface (MCP, HTTP, the web), which binds an operation by its name and implements none of its own. */
#ifndef LAPLACE_OP_H
#define LAPLACE_OP_H

#include <stddef.h>
#include <stdio.h>

typedef enum { ARG_TEXT, ARG_INT, ARG_REAL, ARG_FLAG, ARG_PATH, ARG_CONNINFO } ArgType;

/* A parameter: one meaning, defined once, and named by every operation that takes it. */
typedef struct {
    const char *name;                                  /* its name in a description */
    const char *flag;                                  /* its spelling on the command line; NULL: positional */
    ArgType type;
    const char *dflt;                                  /* where its value comes from when it is not given; NULL: the operation's own */
    const char *what;
} Param;

/* An operation's use of a parameter. */
enum { ARG_OPTIONAL = 0, ARG_REQUIRED = 1, ARG_MANY = 2 };
typedef struct {
    const Param *p; unsigned use;
    const char *what;                                  /* this operation's sense of it; NULL: the parameter's */
    const char *dflt;                                  /* this operation's default; NULL: the parameter's */
} Arg;

/* What an operation does to the world: a surface shows it, and gates what writes or destroys. */
enum { EFFECT_READS_DB = 1, EFFECT_WRITES_DB = 2, EFFECT_DESTROYS = 4, EFFECT_READS_FILES = 8, EFFECT_WRITES_FILES = 16 };

typedef struct {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *what;
    const Arg *args;                                   /* options, then positionals in order; ends at { NULL } */
    unsigned effects;
    const char *gives;                                 /* what it writes to standard output */
} Op;

extern const Op OPS[];
extern const size_t NOPS;
const Op *op_named(const char *name);
void op_usage(FILE *out, const Op *op);                /* "laplace NAME [-d conninfo] ... file...", from the declaration */
void op_describe(FILE *out);                           /* every operation and parameter, as JSON */

#endif
