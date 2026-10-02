/* The database, as the engine speaks to it: one way to send a statement its parameters, one way to read a column back,
 * one way to stream rows in by COPY. The bytes are Laplace-Native's (lp_pg_ids, lp_copy); this is only libpq around
 * them. Every result comes back in binary. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Arg *arg_next(Args *a){ if (a->n >= ARGS_MAX) { fprintf(stderr, "a statement of more than %d parameters\n", ARGS_MAX); exit(1); } Arg *x = &a->a[a->n++]; x->b.n = 0; return x; }
void arg_ids(Args *a, const lp_id *ids, size_t n){ Arg *x = arg_next(a); lp_pg_ids(&x->b, id_oid, ids, n); x->binary = 1; }
void arg_text(Args *a, const char *s){ Arg *x = arg_next(a); lp_buf_put(&x->b, s, strlen(s) + 1); x->b.n--; x->binary = 0; }
void arg_int(Args *a, long long v){ char t[32]; snprintf(t, sizeof t, "%lld", v); arg_text(a, t); }
void arg_f64(Args *a, double v){ char t[40]; snprintf(t, sizeof t, "%.17g", v); arg_text(a, t); }
void arg_raw(Args *a, const void *bytes, size_t n){ Arg *x = arg_next(a); lp_buf_put(&x->b, bytes, n); x->binary = 1; }
void args_reset(Args *a){ a->n = 0; }
void args_free(Args *a){ for (int i = 0; i < ARGS_MAX; i++) lp_buf_free(&a->a[i].b); a->n = 0; }

/* A text parameter is sent NUL-terminated, which its buffer is once it is complete. */
static void args_out(Args *a, const char **v, int *l, int *f){
    for (int i = 0; a && i < a->n; i++) {
        Arg *x = &a->a[i];
        if (!x->binary) { *lp_buf_room(&x->b, 1) = 0; x->b.n--; }
        v[i] = (const char *)x->b.b; l[i] = (int)x->b.n; f[i] = x->binary;
    }
}
static PGresult *checked(PGconn *pg, PGresult *r, const char *sql){
    ExecStatusType s = PQresultStatus(r);
    if (s != PGRES_TUPLES_OK && s != PGRES_COMMAND_OK) { fprintf(stderr, "%.60s...: %s", sql, PQerrorMessage(pg)); exit(1); }
    return r;
}
PGresult *ask(PGconn *pg, const char *sql, Args *a){
    const char *v[ARGS_MAX]; int l[ARGS_MAX], f[ARGS_MAX]; args_out(a, v, l, f);
    return checked(pg, db_ask(pg, sql, a ? a->n : 0, v, l, f), sql);
}
PGresult *ask_once(PGconn *pg, const char *sql, Args *a){
    const char *v[ARGS_MAX]; int l[ARGS_MAX], f[ARGS_MAX]; args_out(a, v, l, f);
    return checked(pg, PQexecParams(pg, sql, a ? a->n : 0, NULL, v, l, f, 1), sql);
}
PGresult *ask_try(PGconn *pg, const char *sql, Args *a){
    const char *v[ARGS_MAX]; int l[ARGS_MAX], f[ARGS_MAX]; args_out(a, v, l, f);
    PGresult *r = PQexecParams(pg, sql, a ? a->n : 0, NULL, v, l, f, 1); ExecStatusType s = PQresultStatus(r);
    if (s == PGRES_TUPLES_OK || s == PGRES_COMMAND_OK) return r;
    PQclear(r); return NULL;
}
void exec(PGconn *pg, const char *sql){ PQclear(checked(pg, PQexec(pg, sql), sql)); }

/* ---- COPY: rows flushed to the server as the buffer fills */
static int put(void *pg, const uint8_t *b, size_t n){ return PQputCopyData((PGconn *)pg, (const char *)b, (int)n) == 1; }
void copy_open(lp_copy *c, PGconn *pg, const char *sql){
    PGresult *r = PQexec(pg, sql);
    if (PQresultStatus(r) != PGRES_COPY_IN) { fprintf(stderr, "%s: %s", sql, PQerrorMessage(pg)); exit(1); }
    PQclear(r); lp_copy_start(c, (size_t)1 << 22, put, pg);
}
void copy_close(lp_copy *c){
    PGconn *pg = c->ctx;
    if (!lp_copy_end(c) || PQputCopyEnd(pg, NULL) != 1) { fprintf(stderr, "COPY: %s", PQerrorMessage(pg)); exit(1); }
    PGresult *r; while ((r = PQgetResult(pg))) { if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "COPY: %s", PQerrorMessage(pg)); exit(1); } PQclear(r); }
}

/* ---- options: each the flag, what it holds, and where; the arguments that follow are the command's own */
int opts(int argc, char **argv, const Opt *o){
    int a = 1;
    for (; a < argc; a++) {
        const Opt *x = o; while (x->flag && strcmp(x->flag, argv[a])) x++;
        if (!x->flag) break;                                                   /* not an option: the arguments begin */
        if (x->kind == 'b') { *(int *)x->at = 1; continue; }
        if (a + 1 >= argc) break;                                              /* an option missing its value is an argument */
        const char *v = argv[++a];
        switch (x->kind) {
            case 's': *(const char **)x->at = v; break;
            case 'i': *(int *)x->at = atoi(v); break;
            case 'u': *(unsigned *)x->at = (unsigned)strtoul(v, NULL, 10); break;
            case 'd': *(double *)x->at = atof(v); break;
            case 'l': *(long long *)x->at = strtoll(v, NULL, 10); break;
        }
    }
    return a;
}

/* ---- one statement over a set of IDs, on every connection at once
 * The IDs go out grouped by where each can be (by->group gives the i-th ID's group, 0 .. by->ngroups - 1), each group
 * in ID order so an index is read along rather than hopped across, in chunks of at most chunk IDs; a chunk is sent to
 * every table its group names (by->targets, by->target), %s in the statement standing for the table. Every row comes
 * back through each(q, r, place, ctx) with every other connection's rows held off: place[k] is where the chunk's k-th
 * ID stands in ids (for a statement that answers by ordinality). Returns the rows fetched, or for a write the rows it
 * changed. */
#include <omp.h>
typedef struct { int g, t; uint64_t lo, n; } Job;
typedef struct { lp_id id; uint64_t at; } Placed;
static int by_placed(const void *a, const void *b){ return lp_id_cmp(a, b); }
uint64_t over_ids(PGconn **pg, int npg, const lp_id *ids, uint64_t n, const Groups *by, size_t chunk, const char *sql, Each each, void *ctx){
    int ng = by->ngroups; uint64_t *cnt = calloc((size_t)ng + 1, sizeof *cnt), *place = malloc(sizeof *place * (n + 1)), *fill = malloc(sizeof *fill * (size_t)(ng + 1));
    #define GROUP(i) (by->group ? by->group(ids, (i), by->ctx) : 0)
    for (uint64_t i = 0; i < n; i++) cnt[GROUP(i) + 1]++;
    for (int g = 0; g < ng; g++) cnt[g + 1] += cnt[g];
    memcpy(fill, cnt, sizeof *fill * (size_t)ng); for (uint64_t i = 0; i < n; i++) place[fill[GROUP(i)]++] = i;
    #undef GROUP
    lp_vec(Job) jobs = { 0 }; lp_vec(Placed) pl = { 0 };
    for (int g = 0; g < ng; g++) {
        if (cnt[g + 1] == cnt[g]) continue;
        uint64_t k = cnt[g + 1] - cnt[g]; lp_vec_reserve(&pl, k);                       /* the group in ID order */
        for (uint64_t i = 0; i < k; i++) pl.v[i] = (Placed){ ids[place[cnt[g] + i]], place[cnt[g] + i] };
        lp_sort(pl.v, k, sizeof *pl.v, by_placed); for (uint64_t i = 0; i < k; i++) place[cnt[g] + i] = pl.v[i].at;
        int nt = by->targets ? by->targets(g, by->ctx) : 1;
        for (uint64_t lo = cnt[g]; lo < cnt[g + 1]; lo += chunk) for (int t = 0; t < nt; t++) lp_push(&jobs, (Job){ g, t, lo, cnt[g + 1] - lo < chunk ? cnt[g + 1] - lo : chunk });
    }
    uint64_t rows = 0;
    #pragma omp parallel for num_threads(npg) schedule(dynamic) reduction(+:rows)
    for (size_t j = 0; j < jobs.n; j++) {
        const Job *x = &jobs.v[j]; lp_id *part = malloc(sizeof(lp_id) * x->n); for (uint64_t i = 0; i < x->n; i++) part[i] = ids[place[x->lo + i]];
        char table[96] = "", q[1024]; if (by->target) by->target(x->g, x->t, table, sizeof table, by->ctx); snprintf(q, sizeof q, sql, table);
        Args a = { 0 }; arg_ids(&a, part, x->n); PGresult *r = ask_once(pg[omp_get_thread_num()], q, &a); args_free(&a); free(part);
        if (each) {
            #pragma omp critical(over_ids)
            for (int k = 0; k < PQntuples(r); k++) each(r, k, place + x->lo, ctx);
            rows += (uint64_t)PQntuples(r);
        } else rows += strtoull(PQcmdTuples(r), NULL, 10);
        PQclear(r);
    }
    lp_vec_free(&jobs); lp_vec_free(&pl); free(cnt); free(place); free(fill);
    return rows;
}
/* The groups of the ID's first hex digit: the ledger's and the statistics' sixteen partitions. */
static int digit_group(const lp_id *ids, uint64_t i, void *ctx){ (void)ctx; return ids[i].b[0] >> 4; }
static void digit_table(int g, int t, char *out, size_t cap, void *ctx){ (void)t; snprintf(out, cap, "%s_%x", (const char *)ctx, g); }
Groups by_digit(const char *table){ Groups g = { 16, digit_group, NULL, digit_table, (void *)table }; return g; }
Groups whole(void){ Groups g = { 1, NULL, NULL, NULL, NULL }; return g; }
