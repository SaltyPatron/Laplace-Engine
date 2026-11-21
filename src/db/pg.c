/* The database, as the engine speaks to it: one way to send a statement its parameters, one way to read a column back,
 * one way to stream rows in by COPY. The bytes are Laplace-Native's (lp_pg_ids, lp_copy); this is only libpq around
 * them. Every result comes back in binary. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static _Atomic uint64_t commands,copy_streams;
void db_traffic_count_command(void){
    atomic_fetch_add_explicit(&commands,1,memory_order_relaxed);
}
DbTraffic db_traffic(void){
    return (DbTraffic){atomic_load_explicit(&commands,memory_order_relaxed),
        atomic_load_explicit(&copy_streams,memory_order_relaxed)};
}

static SqlArg *arg_next(Args *a){ if (a->n >= ARGS_MAX) { fprintf(stderr, "a statement of more than %d parameters\n", ARGS_MAX); exit(1); } SqlArg *x = &a->a[a->n++]; x->b.n = 0; return x; }
void arg_ids(Args *a, const lp_id *ids, size_t n){ SqlArg *x = arg_next(a); lp_pg_ids(&x->b, 0, ids, n); x->binary = SQL_ID_ARRAY; }
void arg_text(Args *a, const char *s){ SqlArg *x = arg_next(a); lp_buf_put(&x->b, s, strlen(s) + 1); x->b.n--; x->binary = 0; }
void arg_raw(Args *a, const void *bytes, size_t n){ SqlArg *x = arg_next(a); lp_buf_put(&x->b, bytes, n); x->binary = 1; }
void args_reset(Args *a){ a->n = 0; }
void args_free(Args *a){ for (int i = 0; i < ARGS_MAX; i++) lp_buf_free(&a->a[i].b); a->n = 0; }

/* ---- COPY: rows flushed to the server as the buffer fills */
static int put(void *pg, const uint8_t *b, size_t n){ return PQputCopyData((PGconn *)pg, (const char *)b, (int)n) == 1; }
void copy_open(lp_copy *c, PGconn *pg, const char *sql){
    atomic_fetch_add_explicit(&commands,1,memory_order_relaxed);
    PGresult *r = PQexec(pg, sql);
    if (PQresultStatus(r) != PGRES_COPY_IN) { fprintf(stderr, "%s: %s", sql, PQerrorMessage(pg)); exit(1); }
    PQclear(r);atomic_fetch_add_explicit(&copy_streams,1,memory_order_relaxed);
    lp_copy_start(c, DB_TRANSPORT_BYTES, put, pg);
}
static PGresult *copy_finish(lp_copy *c){
    PGconn *pg = c->ctx;
    if (!lp_copy_end(c) || PQputCopyEnd(pg, NULL) != 1) { fprintf(stderr, "COPY: %s", PQerrorMessage(pg)); exit(1); }
    PGresult *r; while ((r = PQgetResult(pg))) {
        if (PQresultStatus(r) == PGRES_COPY_IN) return r;
        if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "COPY: %s", PQerrorMessage(pg)); exit(1); }
        PQclear(r);
    }
    return NULL;
}
void copy_next(lp_copy *c){
    PGconn *pg=c->ctx;PGresult *r=copy_finish(c);
    if(!r){fputs("COPY: expected the next stream in the batch\n",stderr);exit(1);}
    PQclear(r);lp_buf_free(&c->buf);*c=(lp_copy){0};
    atomic_fetch_add_explicit(&copy_streams,1,memory_order_relaxed);
    lp_copy_start(c,DB_TRANSPORT_BYTES,put,pg);
}
void copy_close(lp_copy *c){
    PGresult *r=copy_finish(c);
    if(r){PQclear(r);fputs("COPY: unconsumed stream in the batch\n",stderr);exit(1);}
}

/* ---- options: each the flag, what it holds, and where; the arguments that follow are the command's own */
int opts(int argc, char **argv, const Opt *o){
    int a = 1;
    for (; a < argc; a++) {
        const Opt *x = o; while (x->flag && strcmp(x->flag, argv[a])) x++;
        if (!x->flag) break;                                                   /* not an option: the arguments begin */
        if (x->kind == 'b') { *(int *)x->at = 1; continue; }
        if (x->kind == 'v') { *(int *)x->at = x->val; continue; }
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
/* ---- connections (reader.h): one way every program that links the reader opens the database. The conninfo is
 * LAPLACE_CONNINFO, else the default the program was built with (db_conninfo_default, set once at its start). */
static const char *conninfo_default = "";
static char noted_conn[8192];
void db_conninfo_default(const char *conninfo){ conninfo_default = conninfo ? conninfo : ""; }
const char *laplace_db(void){ const char *v = getenv("LAPLACE_CONNINFO"); return v && *v ? v : conninfo_default; }
const char *db_noted(void){ return noted_conn; }
void db_note(const char *conninfo){
    if (!conninfo) conninfo = "";
    /* pool_open passes db_noted(), which is this buffer: copying it onto itself is undefined */
    if (conninfo != noted_conn) snprintf(noted_conn, sizeof noted_conn, "%s", conninfo);
}
static PGconn *connect_one(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo);
    if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "Database connection failed: %s", PQerrorMessage(pg)); exit(1); }
    db_notices(pg);
    return pg;
}
PGconn *db_connect(const char *conninfo){ db_note(conninfo); return connect_one(db_noted()); }
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
/* Every statement in a read command uses its connection-owned, read-only
 * repeatable-read transaction. No containment pool or global snapshot owner. */
PGconn *db_read(const char *conninfo){
    PGconn *pg = db_connect(conninfo);
    if(!db_read_begin(pg,NULL)){fprintf(stderr,"read snapshot: %s",PQerrorMessage(pg));exit(1);}
    return pg;
}
PGconn *db_try_err(const char *conninfo, char *err, size_t err_cap){
    if (err && err_cap) err[0] = '\0';
    PGconn *pg = PQconnectdb(conninfo ? conninfo : "");
    if (PQstatus(pg) != CONNECTION_OK) {
        if (err && err_cap) snprintf(err, err_cap, "%s", PQerrorMessage(pg));
        PQfinish(pg);
        return NULL;
    }
    db_notices(pg);
    return pg;
}
PGconn *db_try(const char *conninfo){ return db_try_err(conninfo, NULL, 0); }
PGconn *db_connect_admin(const char *conninfo, const char *admin_db, char *dbname_out, size_t dbname_cap, char *err, size_t err_cap){
    if (err && err_cap) err[0] = '\0';
    if (dbname_out && dbname_cap) dbname_out[0] = '\0';
    char *perr = NULL;
    PQconninfoOption *o = PQconninfoParse(conninfo ? conninfo : "", &perr);
    if (!o) {
        if (err && err_cap) snprintf(err, err_cap, "%s", perr ? perr : "the connection string does not parse");
        if (perr) PQfreemem(perr);
        return NULL;
    }
    const char *kw[32], *vl[32], *db = NULL;
    int n = 0;
    for (PQconninfoOption *x = o; x->keyword && n < 30; x++) {
        if (!x->val) continue;
        if (!strcmp(x->keyword, "dbname")) { db = x->val; continue; }
        kw[n] = x->keyword;
        vl[n++] = x->val;
    }
    if (!db || !*db) {
        if (err && err_cap) snprintf(err, err_cap, "the connection string names no database");
        PQconninfoFree(o);
        return NULL;
    }
    if (dbname_out && dbname_cap) snprintf(dbname_out, dbname_cap, "%s", db);
    kw[n] = "dbname";
    vl[n++] = (admin_db && *admin_db) ? admin_db : "postgres";
    kw[n] = NULL;
    vl[n] = NULL;
    PGconn *pg = PQconnectdbParams(kw, vl, 0);
    PQconninfoFree(o);
    if (PQstatus(pg) != CONNECTION_OK) {
        if (err && err_cap) snprintf(err, err_cap, "%s", PQerrorMessage(pg));
        PQfinish(pg);
        return NULL;
    }
    db_notices(pg);
    return pg;
}
