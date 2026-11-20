/* A database as a Laplace database: deployed, indexed, and looked at. The SQL is Laplace-postgres's; the engine runs
 * it, so there is one way to stand a database up and one place that says what it holds.
 *
 *   laplace deploy [-d conninfo]     the database if it is not there, extensions, content schema, semantics, its tier 0
 *   laplace index  [-d conninfo]     the indexes again, after one was dropped
 *   laplace status [-d conninfo]     what it holds */
#include "engine.h"
#include "chess_cache.h"
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
static void timed_ok(double t, const char *what){
    printf("  %-52s %9.1f ms\n", what, (now() - t) * 1000);
    fflush(stdout);
}
static int run_sql(PGconn *pg, LpSqlId id, Args *a, const char *what){
    double t = now(); char err[512];
    int ok = db_exec_contract_try(pg, id, a, err, sizeof err) == 0;
    if (!ok) fprintf(stderr, "%s: %s\n", what, err); else timed_ok(t, what);
    return ok;
}
static PGresult *status_query(PGconn *pg, LpSqlId id){
    char err[512]; PGresult *r = db_call_try(pg, id, NULL, err, sizeof err);
    if (!r) {
        fprintf(stderr, "laplace status: %s\n", err);
        PQfinish(pg); exit(1);
    }
    return r;
}
static char *dup_text_col(const PGresult *r, int col){
    return (PQntuples(r) && !PQgetisnull(r, 0, col)) ? strdup(col_str(r, 0, col)) : NULL;
}

/* The database the connection string names, made if the server does not have it: asked of the server's own
 * database, "postgres", by the same role over the same connection. */
static void database_made(const char *conninfo){
    PGconn *pg = PQconnectdb(conninfo); int there = PQstatus(pg) == CONNECTION_OK;
    char first[512]; snprintf(first, sizeof first, "%s", there ? "" : PQerrorMessage(pg)); PQfinish(pg); if (there) return;
    char *err = NULL; PQconninfoOption *o = PQconninfoParse(conninfo, &err); if (!o) { fprintf(stderr, "%s", err ? err : "the connection string does not parse\n"); exit(1); }
    const char *kw[32], *vl[32], *db = NULL; int n = 0;
    for (PQconninfoOption *x = o; x->keyword && n < 30; x++) { if (!x->val) continue; if (!strcmp(x->keyword, "dbname")) { db = x->val; continue; } kw[n] = x->keyword; vl[n++] = x->val; }
    if (!db) { PQconninfoFree(o); return; }
    kw[n] = "dbname"; vl[n++] = "postgres"; kw[n] = NULL; vl[n] = NULL;
    pg = PQconnectdbParams(kw, vl, 0);
    if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "cannot connect to %s: %scannot connect to postgres either: %s", db, first, PQerrorMessage(pg)); exit(1); }
    {   /* Only a database the server does not have is made: any other refusal (authentication, pg_hba, CONNECT
         * privilege, server down) is reported as it is. */
        Args a = {0}; arg_text(&a, db); char cerr[512];
        PGresult *r = db_call_try(pg, LP_SQL_DB_EXISTS, &a, cerr, sizeof cerr); args_free(&a);
        if (!r) { fprintf(stderr, "cannot check database %s: %s\n", db, cerr); PQfinish(pg); PQconninfoFree(o); exit(1); }
        int exists = PQntuples(r) == 1; PQclear(r);
        if (exists) { fprintf(stderr, "cannot connect to %s, which exists: %s", db, first); PQfinish(pg); PQconninfoFree(o); exit(1); }
    }
    printf("laplace deploy   the server has no database %s: making it\n", db);
    { double t = now(); if (db_ddl_create_db(pg, db) != 0) exit(1); timed_ok(t, "the database"); }
    PQfinish(pg); PQconninfoFree(o);
}
/* Used only when --recreate is explicitly passed and persistent storage table DDL is incompatible with in-place
 * laplace.reconcile.sql update: ends active sessions, drops the database, and recreates it over the same role. */
static void database_dropped(const char *conninfo){
    char *err = NULL; PQconninfoOption *o = PQconninfoParse(conninfo, &err); if (!o) { fprintf(stderr, "%s", err ? err : "the connection string does not parse\n"); exit(1); }
    const char *kw[32], *vl[32], *db = NULL; int n = 0;
    for (PQconninfoOption *x = o; x->keyword && n < 30; x++) { if (!x->val) continue; if (!strcmp(x->keyword, "dbname")) { db = x->val; continue; } kw[n] = x->keyword; vl[n++] = x->val; }
    if (!db) { fputs("laplace deploy: the connection string names no database to make again\n", stderr); exit(1); }
    kw[n] = "dbname"; vl[n++] = "postgres"; kw[n] = NULL; vl[n] = NULL;
    PGconn *pg = PQconnectdbParams(kw, vl, 0);
    if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "cannot connect to postgres to make %s again: %s", db, PQerrorMessage(pg)); exit(1); }
    { double t = now(); if (db_ddl_drop_db(pg, db) != 0) exit(1); timed_ok(t, "the database, dropped"); }
    PQfinish(pg); PQconninfoFree(o);
}
static int valid_version_token(const char *s){
    if(!s || !*s || strlen(s)>64) return 0;
    for(const char *p=s; *p; p++){
        if(*p=='/' || *p=='\\' || *p==':' || (*p=='.' && p[1]=='.')) return 0;
    }
    return 1;
}
static int prepare_laplace_transition(PGconn *pg, const char *installed, const char *avail, char *transition_path, size_t cap){
    transition_path[0]='\0';
    if(!valid_version_token(installed) || !valid_version_token(avail)) return 0;
    char cerr[512];
    PGresult *d=db_call_try(pg,LP_SQL_EXT_CONTROL_DIR,NULL,cerr,sizeof cerr);
    if(!d || PQntuples(d)!=1 || PQgetisnull(d,0,0)){ if(d) PQclear(d); return 0; }
    char dir[1024]; snprintf(dir,sizeof dir,"%s",col_str(d,0,0));
    PQclear(d);
    if(!*dir) return 0;
    char rec_path[1200];
    snprintf(rec_path,sizeof rec_path,"%s/laplace.reconcile.sql",dir);
    FILE *rf=fopen(rec_path,"rb");
    if(!rf) return 0;
    if(fseek(rf,0,SEEK_END)!=0){ fclose(rf); return 0; }
    long rlen=ftell(rf);
    if(rlen<=0 || rlen>16*1024*1024 || fseek(rf,0,SEEK_SET)!=0){ fclose(rf); return 0; }
    char *buf=malloc((size_t)rlen+1);
    if(!buf){ fclose(rf); return 0; }
    size_t got=fread(buf,1,(size_t)rlen,rf); fclose(rf);
    if(got!=(size_t)rlen){ free(buf); return 0; }
    buf[got]='\0';
    const char *tag=strstr(buf,"laplace-storage:");
    char stamp[64]="";
    if(tag){
        size_t i=0;
        const char *p=tag+strlen("laplace-storage:");
        while(i<sizeof(stamp)-1 && ((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')||(p[i]>='A'&&p[i]<='F'))){
            stamp[i]=p[i]; i++;
        }
        stamp[i]='\0';
        if(i>0){
            char full[80]; snprintf(full,sizeof full,"laplace-storage:%s",stamp);
            Args a={0}; arg_text(&a,full);
            PGresult *sc=db_call_try(pg,LP_SQL_EXT_STORAGE_COMPAT,&a,cerr,sizeof cerr);
            args_free(&a);
            int compat=sc && PQntuples(sc)==1 && !PQgetisnull(sc,0,0) && col_bool(sc,0,0);
            if(sc) PQclear(sc);
            if(!compat){ free(buf); return 0; }
        }
    }
    if(!*stamp){ free(buf); return 0; }
    if(snprintf(transition_path,cap,"%s/laplace--%s--%s.sql",dir,installed,avail)>=(int)cap){
        transition_path[0]='\0'; free(buf); return 0;
    }
    FILE *tf=fopen(transition_path,"wb");
    if(!tf){ transition_path[0]='\0'; free(buf); return 0; }
    size_t wrote=fwrite(buf,1,got,tf); fclose(tf); free(buf);
    if(wrote!=got){ remove(transition_path); transition_path[0]='\0'; return 0; }
    return 1;
}
int cmd_deploy(int argc, char **argv){
    int configure_cache=0,prepare_only=0,recreate=0;const char *schema=NULL;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-d") && i+1<argc){i++;continue;}
        if(!strcmp(argv[i],"--schema") && i+1<argc){schema=argv[++i];continue;}
        if(!strcmp(argv[i],"--configure-server-cache")){configure_cache=1;continue;}
        if(!strcmp(argv[i],"--prepare")){prepare_only=1;continue;}
        if(!strcmp(argv[i],"--recreate")){recreate=1;continue;}
        fputs("usage: laplace deploy [-d conninfo] [--schema name] [--configure-server-cache | --prepare] [--recreate]\n",stderr);return 2;
    }
    if(prepare_only && configure_cache){
        fputs("--prepare preserves server cache bindings; it cannot configure them\n",stderr);return 2;
    }
    database_made(conn_arg(argc, argv));
    PGconn *pg = db_connect(conn_arg(argc, argv)); tier0_open(NULL);
    printf("laplace deploy   %s\n", PQdb(pg));
    if(schema){
        char cerr[512];
        PGresult *r=db_call_try(pg,LP_SQL_EXT_INSTALLED_INFO,NULL,cerr,sizeof cerr);
        if(!r){fprintf(stderr,"extension namespace: %s\n",cerr);PQfinish(pg);return 1;}
        if(!PQgetisnull(r,0,0) && strcmp(col_str(r,0,0),schema)){
            fprintf(stderr,"laplace is installed in schema %s; --schema does not move an existing installation\n",col_str(r,0,0));
            PQclear(r);PQfinish(pg);return 1;
        }
        if(!*schema || strlen(schema)>(size_t)col_i32(r,0,1)){
            fputs("extension namespace exceeds PostgreSQL's identifier length or is empty\n",stderr);PQclear(r);PQfinish(pg);return 2;
        }
        PQclear(r);
        double t=now();
        if(db_ddl_create_schema(pg,schema)!=0){PQfinish(pg);return 1;}
        timed_ok(t,"Native namespace");
    }
    { double t=now(); if(db_ddl_create_postgis(pg)!=0){PQfinish(pg);return 1;} timed_ok(t,"extension postgis"); }
    { double t=now(); if(db_ddl_create_laplace(pg,schema?schema:"public")!=0){PQfinish(pg);return 1;} timed_ok(t,"extension laplace"); }
    {   /* Reconcile replaceable API/operation objects in place when storage DDL matches; recreate only with --recreate. */
        char cerr[512];
        PGresult *v = db_call_try(pg, LP_SQL_EXT_VERSIONS, NULL, cerr, sizeof cerr);
        if (!v || PQntuples(v) != 1) { fprintf(stderr, "laplace extension version: %s\n", v ? "not found" : cerr); PQclear(v); PQfinish(pg); return 1; }
        if (strcmp(col_str(v, 0, 0), col_str(v, 0, 1))) {
            char installed[128], avail[128];
            snprintf(installed,sizeof installed,"%s",col_str(v,0,0));
            snprintf(avail,sizeof avail,"%s",col_str(v,0,1));
            PQclear(v);
            int can_update=0;
            PGresult *up=db_call_try(pg,LP_SQL_EXT_UPDATE_PATH,NULL,cerr,sizeof cerr);
            if(up && PQntuples(up)==1 && !PQgetisnull(up,0,0) && col_i32(up,0,0)>0) can_update=1;
            if(up) PQclear(up);
            char transition_path[1400]="";
            if(!can_update) can_update=prepare_laplace_transition(pg,installed,avail,transition_path,sizeof transition_path);
            if(can_update){
                double t=now();
                int rc=db_ddl_update_laplace(pg,avail);
                if(transition_path[0]) remove(transition_path);
                if(rc!=0){ PQfinish(pg); return 1; }
                timed_ok(t,"extension laplace updated in place");
            }else if(recreate){
                printf("laplace deploy   %s holds laplace %s and the installed extension is %s: making it again (--recreate)\n",
                       PQdb(pg), installed, avail);
                PQfinish(pg); fflush(stdout);
                database_dropped(conn_arg(argc, argv));
                return cmd_deploy(argc, argv);
            }else{
                fprintf(stderr, "laplace deploy: %s carries incompatible storage schema %s (installed extension is %s); run deploy drop or --recreate to recreate deliberately\n",
                        PQdb(pg), installed, avail);
                PQfinish(pg);
                return 1;
            }
        } else {
            PQclear(v);
        }
    }
    {   /* The module generation the installed control names (its module_pathname), read where the server finds the
         * control: a module rebuilt under the same version is another generation, and the bindings are restored by
         * that generation's own restore, so every C function comes to it. A server whose control cannot be read here
         * keeps the generation its functions name, and says so. */
        char cerr[512];
        PGresult *m = db_call_try(pg, LP_SQL_EXT_MODULE_PATH, NULL, cerr, sizeof cerr);
        if (m && PQntuples(m) == 1 && !PQgetisnull(m, 0, 0)) {
            double t = now();
            if (db_ddl_install_restore_bindings(pg, col_str(m, 0, 0)) != 0) { PQclear(m); PQfinish(pg); return 1; }
            timed_ok(t, "the installed module generation");
        } else printf("  the installed control's module could not be read (%s): the functions keep the generation they name\n",
                      m ? "no control on extension_control_path" : cerr);
        PQclear(m);
    }
    if (!run_sql(pg, LP_SQL_RESTORE_FUNCTION_BINDINGS, NULL, "matching Native implementation bindings")) { PQfinish(pg); return 1; }
    /* Only a host installer may select server-local cache paths. A remote
     * extension install/upgrade preserves the server's existing configuration. */
    if(configure_cache){
        const char *names[]={"laplace.tier0","laplace.flags","laplace.highway","laplace.chess"};
        const char *paths[]={lp_tier0_path(),lp_flags_path(),lp_highway_path(),chess_cache_path()};
        if(!run_sql(pg,LP_SQL_TX_BEGIN,NULL,"server cache configuration")){PQfinish(pg);return 1;}
        for(size_t i=0;i<sizeof names/sizeof *names;i++){
            double t=now();
            if(db_ddl_alter_db_setting(pg,PQdb(pg),names[i],paths[i],1)!=0){PQfinish(pg);return 1;}
            timed_ok(t,names[i]);
        }
        if(!run_sql(pg,LP_SQL_TX_COMMIT,NULL,"server cache configuration")){PQfinish(pg);return 1;}
        PQfinish(pg);pg=db_connect(conn_arg(argc,argv));
    }
    if (!run_sql(pg, LP_SQL_SCHEMA_INDEXES, NULL, "every index, made where one is missing")) { PQfinish(pg); return 1; }
    PQfinish(pg);
    if(prepare_only){
        puts("Database schema prepared; runtime activation and compatibility readback remain required.");
        return 0;
    }
    return cmd_status(argc, argv);
}

int cmd_index(int argc, char **argv){
    PGconn *pg = db_connect(conn_arg(argc, argv));
    printf("laplace index   %s\n", PQdb(pg));
    int ok = run_sql(pg,LP_SQL_SCHEMA_INDEXES,NULL,"every index, made where one is missing") &&
        run_sql(pg,LP_SQL_ANALYZE_SUBSTRATE,NULL,"Native statistics");
    PQfinish(pg); return !ok;
}

int cmd_status(int argc, char **argv){
    int compatible=1;
    PGconn *pg = db_connect(conn_arg(argc, argv)); setlocale(LC_NUMERIC, "en_US.UTF-8");
    PGresult *meta = status_query(pg, LP_SQL_STATUS_META);
    char *ver = dup_text_col(meta, 0), *isa = dup_text_col(meta, 1),
         *t0 = dup_text_col(meta, 2), *fp = dup_text_col(meta, 3),
         *size = dup_text_col(meta, 4), *srv = dup_text_col(meta, 5),
         *hw = dup_text_col(meta, 6), *hfp = dup_text_col(meta, 7),
         *ch = dup_text_col(meta, 8), *cfp = dup_text_col(meta, 9);
    PQclear(meta);
    printf("\ndatabase   %s, %s\nserver     %s\nextension  laplace %s; %s\n", PQdb(pg), size ? size : "?", srv ? srv : "?", ver ? ver : "(not installed)", isa ? isa : "");
    printf("tier 0     %s\n           %s\n", t0 && *t0 ? t0 : "(the extension's default)", fp ? fp : "(fingerprint unavailable)");
    const lp_tier0_record *mine = lp_tier0_map(NULL);
    if (mine && !T0) T0 = mine;   /* the process's tier 0: the chess cache below is bound to it, never to none */
    if (mine && fp) { uint8_t h[32]; char hex[65]; lp_tier0_fingerprint(mine, h); lp_hex(h, 32, hex);
                      int differs=strcmp(hex,fp);compatible&=!differs;
                      printf("           %s\n", differs ? "DIFFERS from this engine's tier 0: the two would give the same content different coordinates" : "the same as this engine's"); }
    else {fputs("laplace status: client or database Tier-0 cache is unavailable\n",stderr);compatible=0;}
    { const lp_highway *h = lp_highway_map(NULL);
      printf("highway    %s\n           %s\n", hw && *hw ? hw : "(the extension's default)", hfp ? hfp : "(not generated: laplace highway)");
      if (h && hfp) { uint8_t b[32]; char hex[65]; lp_highway_fingerprint(h, b); lp_hex(b, 32, hex);
                      int differs=strcmp(hex,hfp);compatible&=!differs;
                      printf("           %s\n", differs ? "DIFFERS from this engine's highway: the two would give a type different slots" : "the same as this engine's"); }
      free(hw); free(hfp); }
    { const lp_perfcache *c = T0 ? chess_cache() : NULL;   /* without a tier 0 there is nothing to bind it to */
      printf("chess      %s\n           %s\n", ch && *ch ? ch : "(the extension's default)", cfp ? cfp : "(not generated: laplace chess)");
      if (c && cfp) { uint8_t b[32]; char hex[65]; lp_perfcache_fingerprint(c, b); lp_hex(b, 32, hex);
                      int differs=strcmp(hex,cfp);compatible&=!differs;
                      printf("           %s\n", differs ? "DIFFERS from this engine's chess perf-cache: the two hold other positions or another vocabulary" : "the same as this engine's"); }
      free(ch); free(cfp); }

    /* the tier is a column, not a partition: counted from a 1% sample of the rows, the size from the partitions */
    PGresult *r = status_query(pg, LP_SQL_STATUS_TIER_SAMPLE);
    printf("\nentities, by tier (from a 1%% sample)\n");
    for (int i = 0; i < PQntuples(r); i++) printf("  tier %-3d %'16lld\n", (int)col_i16(r, i, 0), (long long)col_i64(r, i, 1));
    PQclear(r);
    r = status_query(pg, LP_SQL_STATUS_ENTITY_PHYSICALITY);
    if (PQntuples(r)) {
        if(PQgetisnull(r,0,0) || col_i64(r,0,0)<0) printf("  entities and paths %14s   %s\n","not counted yet",col_str(r,0,1));
        else printf("  entities and paths %'14lld rows   %s\n",(long long)col_i64(r,0,0),col_str(r,0,1));
    }
    PQclear(r);
    /* the witnesses (each a source's trunk) and the standings, the partitions of a table summed: a partition the planner
     * has not counted yet (reltuples -1) makes the sum not counted */
    r = status_query(pg, LP_SQL_STATUS_PARTITIONS);
    printf("\nsemantics\n");
    for (int i = 0; i < PQntuples(r); i++) {
        int64_t rows = col_i64(r, i, 1);
        if (rows < 0) printf("  %-24s %16s   %s\n", col_str(r, i, 0), "not counted yet", col_str(r, i, 2));
        else printf("  %-24s %'16lld   %s\n", col_str(r, i, 0), (long long)rows, col_str(r, i, 2));
    }
    PQclear(r);
    r = status_query(pg, LP_SQL_STATUS_INDEXES);
    if (PQntuples(r)) printf("\nindexes    container (GIN) %lld, 4D (GiST) %lld, of %lld on entity and physicality\n",
        (long long)col_i64(r, 0, 0), (long long)col_i64(r, 0, 1), (long long)col_i64(r, 0, 2));
    PQclear(r);
    free(ver); free(isa); free(t0); free(fp); free(size); free(srv); PQfinish(pg);
    return !compatible;
}

/* laplace sources: the sources there are recipes for, in the order they go in, where each is, and whether it is in. */
int cmd_sources(int argc, char **argv){
    if (argc == 2 && !strcmp(argv[1], "--catalog")) return recipes_catalog(laplace_recipes(), stdout, 1);
    PGconn *pg = db_connect(conn_arg(argc, argv)); tier0_open(NULL); lp_text *tx = lp_text_new(T0);
    Recipe *rec = NULL; int nrec = recipes_load(laplace_recipes(), &rec), n; Source *s = sources_loaded(&n);
    printf("%-4s %-28s %-8s %-9s %s\n", "", "source", "recipes", "in", "kept at");
    /* a source is in when a witness is its trunk: [its record, its files' trunks], its record the source's name */
    lp_idmap *records = lp_idmap_new();
    { char cerr[512]; PGresult *r = db_call_try(pg, LP_SQL_WITNESS_TRUST_ALL, NULL, cerr, sizeof cerr); Reader *rd = reader_new(pg);
      for (int j = 0; r && j < PQntuples(r); j++) { lp_id first[2]; if (reader_parts(rd, col_id(r, j, 0), first, 2)) lp_idmap_put(records, &first[0], NULL); }
      PQclear(r); reader_free(rd); }
    for (int i = 0; i < n; i++) {
        int mine = 0, in = 0;
        for (int k = 0; k < nrec; k++) mine += rec[k].source == i;
        const char *called = source_called(&s[i]);
        if (called) { lp_id w = lp_text_decompose(tx, (const uint8_t *)called, strlen(called), NULL, NULL).id; in = lp_idmap_find(records, &w) >= 0; }
        char where[1200]; if (s[i].unset[0]) snprintf(where, sizeof where, "($%s is not set)", s[i].unset); else snprintf(where, sizeof where, "%s", s[i].found[0] ? s[i].found : "(not at any of its roots)");
        printf("%-4d %-28s %-8d %-9s %s\n", i + 1, s[i].name, mine, in ? "yes" : "no", where);
        if (s[i].nafter) { printf("     %-28s after", ""); for (int a = 0; a < s[i].nafter; a++) printf(" %s", s[i].after[a]); printf("\n"); }
    }
    lp_idmap_free(records); PQfinish(pg);
    return 0;
}
