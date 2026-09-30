/* A database as a Laplace database: deployed, indexed, and looked at. The SQL is Laplace-postgres's; the engine runs
 * it, so there is one way to stand a database up and one place that says what it holds.
 *
 *   laplace deploy [-d conninfo]     the database if it is not there, extensions, content schema, semantics, its tier 0
 *   laplace index  [-d conninfo]     the indexes again, after one was dropped
 *   laplace status [-d conninfo]     what it holds */
#include "engine.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *conn_arg(int argc, char **argv){
    const char *c = laplace_db();
    for (int a = 1; a + 1 < argc; a++) if (!strcmp(argv[a], "-d")) c = argv[a + 1];
    return c;
}
static int run(PGconn *pg, const char *sql, const char *what){
    double t = now(); PGresult *r = PQexec(pg, sql); int ok = PQresultStatus(r) == PGRES_COMMAND_OK || PQresultStatus(r) == PGRES_TUPLES_OK;
    if (!ok) fprintf(stderr, "%s: %s", what, PQerrorMessage(pg)); else printf("  %-52s %9.1f ms\n", what, (now() - t) * 1000);
    PQclear(r); fflush(stdout); return ok;
}
/* A file of SQL, without psql's own commands (lines that start with a backslash) and without comment lines. */
static char *sql_file(const char *name){
    char p[4096]; snprintf(p, sizeof p, "%s/%s", laplace_sql(), name);
    FILE *f = fopen(p, "r"); if (!f) { perror(p); fprintf(stderr, "LAPLACE_SQL is where Laplace-postgres's sql directory is\n"); exit(1); }
    size_t cap = 1 << 16, n = 0; char *b = malloc(cap), line[8192];
    while (fgets(line, sizeof line, f)) {
        const char *c = line; while (*c == ' ' || *c == '\t') c++;
        if (line[0] == '\\' || (c[0] == '-' && c[1] == '-')) continue;
        size_t l = strlen(line); if (n + l + 1 > cap) { cap = (n + l + 1) * 2; b = xrealloc(b, cap); }
        memcpy(b + n, line, l); n += l;
    }
    fclose(f); b[n] = 0; return b;
}
static char *one(PGconn *pg, const char *sql){
    PGresult *r = PQexec(pg, sql); char *v = NULL;
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) && !PQgetisnull(r, 0, 0)) v = strdup(PQgetvalue(r, 0, 0));
    PQclear(r); return v;
}

/* The database the connection string names, made if the server does not have it: asked of the server's own
 * database, "postgres", by the same role over the same connection. */
static void database_made(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo); int there = PQstatus(pg) == CONNECTION_OK; PQfinish(pg); if (there) return;
    char *err = NULL; PQconninfoOption *o = PQconninfoParse(conninfo, &err); if (!o) { fprintf(stderr, "%s", err ? err : "the connection string does not parse\n"); exit(1); }
    const char *kw[32], *vl[32], *db = NULL; int n = 0;
    for (PQconninfoOption *x = o; x->keyword && n < 30; x++) { if (!x->val) continue; if (!strcmp(x->keyword, "dbname")) { db = x->val; continue; } kw[n] = x->keyword; vl[n++] = x->val; }
    if (!db) { PQconninfoFree(o); return; }
    kw[n] = "dbname"; vl[n++] = "postgres"; kw[n] = NULL; vl[n] = NULL;
    pg = PQconnectdbParams(kw, vl, 0);
    if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "%s", PQerrorMessage(pg)); exit(1); }
    char *id = PQescapeIdentifier(pg, db, strlen(db)), q[512]; snprintf(q, sizeof q, "CREATE DATABASE %s", id);
    printf("laplace deploy   the server has no database %s: making it\n", db);
    if (!run(pg, q, "the database")) exit(1);
    PQfreemem(id); PQfinish(pg); PQconninfoFree(o);
}
int cmd_deploy(int argc, char **argv){
    database_made(conn_arg(argc, argv));
    PGconn *pg = db_connect(conn_arg(argc, argv)); tier0_open(NULL);
    printf("laplace deploy   %s\n", PQdb(pg));
    static const char *ext[] = { "postgis", "laplace", "pg_stat_statements", "pg_buffercache" };
    for (int i = 0; i < 4; i++) { char q[128], w[64]; snprintf(q, sizeof q, "CREATE EXTENSION IF NOT EXISTS %s", ext[i]); snprintf(w, sizeof w, "extension %s", ext[i]); if (!run(pg, q, w)) return 1; }
    if (!run(pg, "ALTER EXTENSION laplace UPDATE", "laplace extension at its newest version")) return 1;
    { char q[4400], *esc = PQescapeLiteral(pg, lp_tier0_path(), strlen(lp_tier0_path())), *db = PQescapeIdentifier(pg, PQdb(pg), strlen(PQdb(pg)));
      snprintf(q, sizeof q, "ALTER DATABASE %s SET laplace.tier0 = %s", db, esc); if (!run(pg, q, "the database's tier 0")) return 1;
      snprintf(q, sizeof q, "SET laplace.tier0 = %s", esc); PQclear(PQexec(pg, q)); PQfreemem(esc);
      esc = PQescapeLiteral(pg, lp_flags_path(), strlen(lp_flags_path()));
      snprintf(q, sizeof q, "ALTER DATABASE %s SET laplace.flags = %s", db, esc); if (!run(pg, q, "the flags that go with it")) return 1;
      PQfreemem(esc); PQfreemem(db); }
    char *have = one(pg, "SELECT 1 FROM pg_class WHERE relname = 'entity' AND relkind = 'p'");
    if (have) printf("  %-52s %9s\n", "content schema", "present"); else { char *s = sql_file("schema.sql"); if (!run(pg, s, "content schema: entity, physicality")) return 1; free(s); }
    free(have);
    { char *s = sql_file("semantics.sql"); if (!run(pg, s, "semantics: witness, attestation, consensus")) return 1; free(s); }
    { char *s = sql_file("lookup.sql"); if (!run(pg, s, "lookups ingestion needs: IDs")) return 1; free(s); }
    /* every index, from the start: a deployed database answers from its first row, and every load keeps them */
    { char *s = sql_file("indexes.sql"); if (!run(pg, s, "containers (GIN), 4D (GiST), Hilbert, the ledger")) return 1; free(s); }
    PQfinish(pg);
    return cmd_status(argc, argv);
}

int cmd_index(int argc, char **argv){
    PGconn *pg = db_connect(conn_arg(argc, argv));
    printf("laplace index   %s\n", PQdb(pg));
    char *s = sql_file("indexes.sql"), *p = s; int ok = 1;
    while (ok && *p) {                                                     /* statement by statement, each timed */
        char *e = strchr(p, ';'); if (!e) break; *e = 0;
        while (*p == '\n' || *p == ' ') p++;
        if (*p) { char what[64]; snprintf(what, sizeof what, "%.60s", p); for (char *c = what; *c; c++) if (*c == '\n') *c = ' '; ok = run(pg, p, what); }
        p = e + 1;
    }
    free(s); PQfinish(pg); return !ok;
}

int cmd_status(int argc, char **argv){
    PGconn *pg = db_connect(conn_arg(argc, argv)); setlocale(LC_NUMERIC, "en_US.UTF-8");
    char *ver = one(pg, "SELECT extversion FROM pg_extension WHERE extname = 'laplace'"), *isa = one(pg, "SELECT laplace_isa()"),
         *t0 = one(pg, "SELECT current_setting('laplace.tier0', true)"), *fp = one(pg, "SELECT laplace_fingerprint()"),
         *size = one(pg, "SELECT pg_size_pretty(pg_database_size(current_database()))"), *srv = one(pg, "SELECT split_part(version(), ',', 1)");
    printf("\ndatabase   %s, %s\nserver     %s\nextension  laplace %s; %s\n", PQdb(pg), size ? size : "?", srv ? srv : "?", ver ? ver : "(not installed)", isa ? isa : "");
    printf("tier 0     %s\n           %s\n", t0 && *t0 ? t0 : "(the extension's default)", fp ? fp : "(no fingerprint: extension older than 0.6)");
    const lp_tier0_record *mine = lp_tier0_map(NULL);
    if (mine && fp) { uint8_t h[32]; char hex[65]; lp_tier0_fingerprint(mine, h); for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
                      printf("           %s\n", strcmp(hex, fp) ? "DIFFERS from this engine's tier 0: the two would give the same content different coordinates" : "the same as this engine's"); }

    PGresult *r = PQexec(pg, "SELECT p.tier, sum(c.reltuples)::bigint, pg_size_pretty(sum(pg_total_relation_size(c.oid))) "
                             "FROM pg_class c JOIN LATERAL (SELECT (regexp_match(c.relname, '^entity_t([0-9]+|x)(_[0-9a-f])?$'))[1] AS tier) p ON p.tier IS NOT NULL "
                             "WHERE c.relkind = 'r' AND c.reltuples > 0 GROUP BY 1 ORDER BY CASE WHEN p.tier = 'x' THEN 99 ELSE p.tier::int END");
    printf("\nentities, by tier (the planner's counts)\n");
    for (int i = 0; PQresultStatus(r) == PGRES_TUPLES_OK && i < PQntuples(r); i++) printf("  tier %-3s %'16lld   %s\n", PQgetvalue(r, i, 0), atoll(PQgetvalue(r, i, 1)), PQgetvalue(r, i, 2));
    PQclear(r);
    r = PQexec(pg, "SELECT c.relname, c.reltuples::bigint, pg_size_pretty(pg_total_relation_size(c.oid)) FROM pg_class c "
                   "WHERE c.relname IN ('witness', 'attestation', 'consensus') AND c.relkind = 'r' ORDER BY 1");
    printf("\nsemantics\n");
    for (int i = 0; PQresultStatus(r) == PGRES_TUPLES_OK && i < PQntuples(r); i++) {
        if (atoll(PQgetvalue(r, i, 1)) < 0) printf("  %-24s %16s   %s\n", PQgetvalue(r, i, 0), "not counted yet", PQgetvalue(r, i, 2));
        else printf("  %-24s %'16lld   %s\n", PQgetvalue(r, i, 0), atoll(PQgetvalue(r, i, 1)), PQgetvalue(r, i, 2));
    }
    PQclear(r);
    r = PQexec(pg, "SELECT count(*) FILTER (WHERE indexdef LIKE '%USING gin%'), count(*) FILTER (WHERE indexdef LIKE '%USING gist%'), count(*) "
                   "FROM pg_indexes WHERE schemaname = 'public' AND tablename IN ('entity', 'physicality')");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) printf("\nindexes    container (GIN) %s, 4D (GiST) %s, of %s on entity and physicality\n", PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2));
    PQclear(r);
    free(ver); free(isa); free(t0); free(fp); free(size); free(srv); PQfinish(pg);
    return 0;
}

/* laplace sources: the sources there are recipes for, in the order they go in, where each is, and whether it is in. */
int cmd_sources(int argc, char **argv){
    PGconn *pg = db_connect(conn_arg(argc, argv)); tier0_open(NULL); lp_text *tx = lp_text_new(T0);
    Recipe *rec = NULL; int nrec = recipes_load(laplace_recipes(), &rec), n; Source *s = sources_loaded(&n);
    printf("%-4s %-28s %-8s %-9s %s\n", "", "source", "recipes", "in", "kept at");
    for (int i = 0; i < n; i++) {
        int mine = 0, in = 0;
        for (int k = 0; k < nrec; k++) mine += rec[k].source == i;
        if (s[i].witness[0] && !strchr(s[i].witness, '{')) {                   /* it is in when its witness is known */
            lp_id w = lp_text_decompose(tx, (const uint8_t *)s[i].witness, strlen(s[i].witness), NULL, NULL).id; uint8_t ab[40]; size_t al = ids_param(ab, &w, 1);
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)al }, f[1] = { 1 };
            PGresult *r = PQexecParams(pg, "SELECT 1 FROM witness WHERE id = ANY($1::blake3[])", 1, NULL, v, l, f, 0);
            in = PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0; PQclear(r);
        }
        printf("%-4d %-28s %-8d %-9s %s\n", i + 1, s[i].name, mine, in ? "yes" : "no", s[i].found[0] ? s[i].found : "(not at any of its roots)");
        if (s[i].nafter) { printf("     %-28s after", ""); for (int a = 0; a < s[i].nafter; a++) printf(" %s", s[i].after[a]); printf("\n"); }
    }
    PQfinish(pg);
    return 0;
}
