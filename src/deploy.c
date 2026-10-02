/* A database as a Laplace database: deployed, indexed, and looked at. The SQL is Laplace-postgres's; the engine runs
 * it, so there is one way to stand a database up and one place that says what it holds.
 *
 *   laplace deploy [-d conninfo]     the database if it is not there, extensions, content schema, semantics, its tier 0
 *   laplace index  [-d conninfo]     the indexes again, after one was dropped
 *   laplace status [-d conninfo]     what it holds */
#include "engine.h"
#include <locale.h>
#include <omp.h>
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
      PQfreemem(esc); esc = PQescapeLiteral(pg, lp_highway_path(), strlen(lp_highway_path()));
      snprintf(q, sizeof q, "ALTER DATABASE %s SET laplace.highway = %s", db, esc); if (!run(pg, q, "the highway: the types, and the mappings between them")) return 1;
      PQfreemem(esc); PQfreemem(db); }
    /* the schema is the extension's: the five tables, their partitions and every index, from CREATE EXTENSION. A
     * database that had them before the extension owned them keeps them, and they are made the extension's here. */
    { PGresult *r = PQexec(pg, "SELECT c.oid::regclass::text FROM pg_class c WHERE c.relkind IN ('r', 'p') AND (c.relname ~ '^(entity|physicality)(_t[0-9a-fx]+(_[0-9a-f])?)?$' OR c.relname IN ('witness', 'attestation', 'consensus')) "
                               "AND NOT EXISTS (SELECT 1 FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid WHERE d.classid = 'pg_class'::regclass AND d.objid = c.oid AND e.extname = 'laplace')");
      int n = PQresultStatus(r) == PGRES_TUPLES_OK ? PQntuples(r) : 0;
      for (int i = 0; i < n; i++) { char q[256]; snprintf(q, sizeof q, "ALTER EXTENSION laplace ADD TABLE %s", PQgetvalue(r, i, 0)); PGresult *a = PQexec(pg, q); PQclear(a); }
      if (n) printf("  %-52s %9d\n", "tables from before the extension owned them, adopted", n); PQclear(r); }
    if (!run(pg, "SELECT laplace_schema_indexes()", "every index, made where one is missing")) return 1;
    PQfinish(pg);
    /* the highway's contents as entities: a type's content (a definition, a frame's name, a lemma and a roleset's name)
     * is what a claim that holds the type renders and pulls through, whether or not any file wrote it as content. Each
     * is recomposed here as the composition laplace highway wrote beside the highway, and its ID checked */
    { char tp[4300]; snprintf(tp, sizeof tp, "%s.nodes", lp_highway_path()); FILE *f = fopen(tp, "r");
      if (!f) printf("  %-52s %s\n", "the highway's contents", "not beside the highway: laplace highway writes them");
      else { double t = now(); int threads = omp_get_num_procs(); table_init(); ctx_open(threads); char *line = NULL; size_t cap = 0; uint64_t n = 0, wrong = 0, types = 0, unknown = 0; const lp_highway *h = lp_highway_map(NULL);
        Ref *ch = NULL; size_t cch = 0;
        while (getline(&line, &cap, f) > 0) { if (line[0] == '#') continue; char *save = NULL, *kind = strtok_r(line, "\t\n", &save); if (!kind) continue;
            if (kind[0] == 'N') { char *hid = strtok_r(NULL, "\t\n", &save), *tr = strtok_r(NULL, "\t\n", &save), *nv = strtok_r(NULL, "\t\n", &save), *c; if (!hid || !tr || !nv) { wrong++; continue; }
                size_t k = 0; int ok = 1;
                while ((c = strtok_r(NULL, "\t\n", &save))) { char *s1 = strchr(c, ':'), *s2 = s1 ? strchr(s1 + 1, ':') : NULL; if (!s2) { ok = 0; break; } *s1 = 0;
                    Ref r; lp_id id; if (c[0] == 'U') r = atom((uint32_t)strtoul(c + 1, NULL, 16)); else { if (!id_parse(c, &id)) { ok = 0; break; } Node *x = table_find(&id); if (!x) { unknown++; ok = 0; break; } memset(&r, 0, sizeof r); r.id = x->id; memcpy(r.c.m, x->m, sizeof r.c.m); r.tier = x->tier; }
                    r.said = (uint8_t)atoi(s1 + 1); uint32_t run = (uint32_t)strtoul(s2 + 1, NULL, 10);
                    for (uint32_t q = 0; q < run; q++) { if (k == cch) { cch = cch ? cch * 2 : 4096; ch = xrealloc(ch, sizeof(Ref) * cch); } ch[k++] = r; } }
                lp_id want; if (!ok || !id_parse(hid, &want)) { wrong++; continue; }
                Ref r = compose(ch, (uint32_t)k, (uint8_t)atoi(tr)); if (memcmp(&r.id, &want, 16)) wrong++; n++; }
            else if (kind[0] == 'S') { char *ln = strtok_r(NULL, "\t\n", &save), *sl = strtok_r(NULL, "\t\n", &save), *c = strtok_r(NULL, "\t\n", &save); if (!ln || !sl || !c) { wrong++; continue; }
                lp_id id; if (c[0] == 'U') id = atom((uint32_t)strtoul(c + 1, NULL, 16)).id; else if (!id_parse(c, &id)) { wrong++; continue; }
                const lp_list *l = h ? lp_highway_list(h, ln) : NULL; const lp_tier0_record *x = l ? lp_highway_at(h, l, (uint32_t)strtoul(sl, NULL, 10)) : NULL; if (!x || memcmp(&x->id, &id, 16)) wrong++; types++; } }
        free(line); free(ch); fclose(f);
        extern int load_whole; load_whole = 1; File one = { 0 }; one.path = "the highway's contents"; LoadStats st = { 0 };
        if (load(conn_arg(argc, argv), threads, &one, 1, &st)) return 1;
        printf("  %-52s %'llu types, %'llu compositions, %'llu entities new, %'llu not as written%s   %.1f s\n", "the highway's contents, as entities", (unsigned long long)types, (unsigned long long)n, (unsigned long long)st.ent_rows, (unsigned long long)wrong, unknown ? ", some naming a node not written before them" : "", now() - t); }
    }
    pg = db_connect(conn_arg(argc, argv));
    { char *db = PQescapeIdentifier(pg, PQdb(pg), strlen(PQdb(pg))), q[256]; snprintf(q, sizeof q, "ALTER DATABASE %s SET enable_parallel_append = off", db); PQclear(PQexec(pg, q)); PQfreemem(db); }
    PQfinish(pg);
    return cmd_status(argc, argv);
}

int cmd_index(int argc, char **argv){
    PGconn *pg = db_connect(conn_arg(argc, argv));
    printf("laplace index   %s\n", PQdb(pg));
    int ok = run(pg, "SET maintenance_work_mem = '8GB'", "maintenance memory") && run(pg, "SELECT laplace_schema_indexes()", "every index, made where one is missing") && run(pg, "ANALYZE", "statistics");
    PQfinish(pg); return !ok;
}

int cmd_status(int argc, char **argv){
    PGconn *pg = db_connect(conn_arg(argc, argv)); setlocale(LC_NUMERIC, "en_US.UTF-8");
    char *ver = one(pg, "SELECT extversion FROM pg_extension WHERE extname = 'laplace'"), *isa = one(pg, "SELECT laplace_isa()"),
         *t0 = one(pg, "SELECT current_setting('laplace.tier0', true)"), *fp = one(pg, "SELECT laplace_fingerprint()"),
         *size = one(pg, "SELECT pg_size_pretty(pg_database_size(current_database()))"), *srv = one(pg, "SELECT split_part(version(), ',', 1)");
    printf("\ndatabase   %s, %s\nserver     %s\nextension  laplace %s; %s\n", PQdb(pg), size ? size : "?", srv ? srv : "?", ver ? ver : "(not installed)", isa ? isa : "");
    printf("tier 0     %s\n           %s\n", t0 && *t0 ? t0 : "(the extension's default)", fp ? fp : "(no fingerprint: extension older than 0.6)");
    const lp_tier0_record *mine = lp_tier0_map(NULL);
    if (mine && fp) { uint8_t h[32]; char hex[65]; lp_tier0_fingerprint(mine, h); lp_hex(h, 32, hex);
                      printf("           %s\n", strcmp(hex, fp) ? "DIFFERS from this engine's tier 0: the two would give the same content different coordinates" : "the same as this engine's"); }
    { char *hw = one(pg, "SELECT current_setting('laplace.highway', true)"), *hfp = one(pg, "SELECT laplace_highway_fingerprint()"); const lp_highway *h = lp_highway_map(NULL);
      printf("highway    %s\n           %s\n", hw && *hw ? hw : "(the extension's default)", hfp ? hfp : "(not generated: laplace highway)");
      if (h && hfp) { uint8_t b[32]; char hex[65]; lp_highway_fingerprint(h, b); lp_hex(b, 32, hex);
                      printf("           %s\n", strcmp(hex, hfp) ? "DIFFERS from this engine's highway: the two would give a type different slots" : "the same as this engine's"); }
      free(hw); free(hfp); }

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
            lp_id w = lp_text_decompose(tx, (const uint8_t *)s[i].witness, strlen(s[i].witness), NULL, NULL).id; Args a = { 0 }; arg_ids(&a, &w, 1);
            PGresult *r = ask_try(pg, "SELECT 1 FROM witness WHERE id = ANY($1::blake3[])", &a);
            in = r && PQntuples(r) > 0; PQclear(r); args_free(&a);
        }
        printf("%-4d %-28s %-8d %-9s %s\n", i + 1, s[i].name, mine, in ? "yes" : "no", s[i].found[0] ? s[i].found : "(not at any of its roots)");
        if (s[i].nafter) { printf("     %-28s after", ""); for (int a = 0; a < s[i].nafter; a++) printf(" %s", s[i].after[a]); printf("\n"); }
    }
    PQfinish(pg);
    return 0;
}
