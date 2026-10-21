/* Forgetting removes witness authority and only the unreferenced part of its
 * structural closure. All discovery, sweeping and Native series updates share
 * one database transaction; no partition is exported to client RAM. */
#include "engine.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void candidates_open(PGconn *pg){
    exec(pg,"CREATE TEMP TABLE forget_candidates(id blake3 PRIMARY KEY) ON COMMIT DROP;"
        " CREATE TEMP TABLE forget_round(id blake3 PRIMARY KEY) ON COMMIT DROP;"
        " CREATE TEMP TABLE forget_stats(deleted bigint NOT NULL) ON COMMIT DROP;"
        " INSERT INTO forget_stats VALUES(0)");
}
static uint64_t sweep_candidates(PGconn *pg){
    exec(pg,"DO $sweep$ DECLARE n bigint; BEGIN LOOP"
        " TRUNCATE forget_round;"
        " INSERT INTO forget_round SELECT c.id FROM forget_candidates c"
        " WHERE NOT EXISTS(SELECT FROM public.witness w WHERE w.id=c.id OR w.lineage=c.id)"
        " AND NOT EXISTS(SELECT FROM public.physicality p WHERE p.path @> ARRAY[c.id]::blake3[] AND p.entity<>c.id);"
        " GET DIAGNOSTICS n=ROW_COUNT; EXIT WHEN n=0;"
        " DELETE FROM public.consensus c USING forget_round d WHERE c.claim=d.id;"
        " DELETE FROM public.physicality p USING forget_round d WHERE p.entity=d.id;"
        " DELETE FROM public.entity e USING forget_round d WHERE e.id=d.id;"
        " DELETE FROM forget_candidates c USING forget_round d WHERE c.id=d.id;"
        " UPDATE forget_stats SET deleted=deleted+n;"
        " END LOOP; END $sweep$");
    PGresult *q=ask_once(pg,"SELECT deleted FROM forget_stats",NULL);uint64_t n=lp_be(PQgetvalue(q,0,0),8);PQclear(q);return n;
}
static int forget_ids(PGconn *pg,const lp_id *roots,size_t n){
    if(!n)return 1;
    candidates_open(pg);Args args={0};arg_ids(&args,roots,n);
    PQclear(ask_once(pg,"WITH RECURSIVE below(id) AS (SELECT unnest($1::blake3[]) UNION"
        " SELECT child FROM below b JOIN public.physicality p ON p.entity=b.id"
        " CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) child)"
        " INSERT INTO forget_candidates SELECT b.id FROM below b JOIN public.entity e ON e.id=b.id",&args));
    PGresult *q=ask_once(pg,"SELECT p.entity FROM public.physicality p JOIN forget_candidates c ON c.id=p.entity"
        " WHERE laplace_mask_has_value(p.mask,'kind','claim') AND laplace_path_length(p.path)>=2"
        " UNION SELECT s.claim FROM public.consensus s JOIN forget_candidates c ON c.id=s.claim",NULL);
    size_t nc=(size_t)PQntuples(q);lp_id *claims=malloc((nc?nc:1)*sizeof *claims);
    if(!claims){perror("affected claim IDs");exit(1);}
    for(size_t i=0;i<nc;i++)claims[i]=*col_id(q,(int)i,0);PQclear(q);
    PQclear(ask_once(pg,"DELETE FROM public.witness WHERE id=ANY($1::blake3[])",&args));args_free(&args);
    /* The Native solver validates claim paths. Remove witness authority and
     * recompute while that closure is still present, then sweep its orphans. */
    uint64_t standing=0,orphaned=0;
    standings_replay(pg,claims,nc,&standing,&orphaned);free(claims);
    uint64_t removed=sweep_candidates(pg);
    printf("  %llu witnesses removed; %llu unreferenced entities removed; %llu remaining standings recomputed; %llu claims without a witness\n",
        (unsigned long long)n,(unsigned long long)removed,(unsigned long long)standing,(unsigned long long)orphaned);
    return 0;
}
static void begin_maintenance(PGconn *pg){
    exec(pg,"BEGIN ISOLATION LEVEL REPEATABLE READ;"
        " SELECT pg_advisory_xact_lock(1279348803,-2);"
        " LOCK TABLE public.witness IN SHARE ROW EXCLUSIVE MODE");
}
int cmd_sweep(int argc,char **argv){
    const char *conninfo=laplace_db();int limit=0,dry=0;
    int a=opts(argc,argv,(const Opt[]){{.flag="-d",.kind='s',.at=&conninfo},{.flag="-j",.kind='i',.at=&limit},
        {.flag="--dry",.kind='b',.at=&dry},{0}});
    if(a<argc||limit<0){fputs("usage: laplace sweep [-d conninfo] [-j connection-cap] [--dry]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);begin_maintenance(pg);candidates_open(pg);
    exec(pg,"INSERT INTO forget_candidates SELECT id FROM public.entity");
    uint64_t n=sweep_candidates(pg);exec(pg,dry?"ROLLBACK":"COMMIT");
    printf("laplace sweep   %s   %llu unreferenced entities %s\n",PQdb(pg),(unsigned long long)n,dry?"would be removed":"removed");
    PQfinish(pg);return 0;
}
static lp_id *selected_roots(PGconn *pg,const lp_id *ids,const lp_id *records,size_t n,int except,size_t *count){
    Args args={0};arg_ids(&args,ids,n);arg_ids(&args,records,n);uint8_t invert=except!=0;arg_raw(&args,&invert,1);
    PGresult *q=ask_once(pg,"SELECT w.id FROM public.witness w JOIN public.physicality p ON p.entity=w.id"
        " WHERE (w.id=ANY($1::blake3[]) OR EXISTS(SELECT FROM unnest($2::blake3[]) r(id)"
        " WHERE laplace_path_starts(p.path,r.id)))<>$3::boolean ORDER BY w.id",&args);
    size_t nr=(size_t)PQntuples(q);lp_id *roots=malloc((nr?nr:1)*sizeof *roots);if(!roots){perror("witness IDs");exit(1);}
    for(size_t i=0;i<nr;i++)roots[i]=*col_id(q,(int)i,0);*count=nr;PQclear(q);args_free(&args);return roots;
}
int cmd_forget(int argc,char **argv){
    const char *conninfo=laplace_db();int limit=0,except=0,trunk=0;
    int a=opts(argc,argv,(const Opt[]){{.flag="-d",.kind='s',.at=&conninfo},{.flag="-j",.kind='i',.at=&limit},
        {.flag="--except",.kind='b',.at=&except},{.flag="--trunk",.kind='b',.at=&trunk},{0}});
    if((a>=argc&&!except)||limit<0){fputs("usage: laplace forget [-d conninfo] [-j connection-cap] [--except] [--trunk] source-or-trunk...\n",stderr);return 2;}
    tier0_open(NULL);Ctx *ctx=lp_text_new(T0);size_t n=(size_t)(argc-a);
    lp_id *ids=calloc(n?n:1,sizeof *ids),*records=calloc(n?n:1,sizeof *records);
    if(!ids||!records){perror("source selectors");exit(1);}
    Recipe *recipes=NULL;recipes_load(laplace_recipes(),&recipes);int ns=0;Source *sources=sources_loaded(&ns);
    for(size_t i=0;i<n;i++){
        const char *name=argv[a+(int)i];int parsed=id_parse(name,&ids[i]);
        if(trunk&&!parsed){fprintf(stderr,"%s: not a trunk ID\n",name);free(ids);free(records);lp_text_free(ctx);return 2;}
        if(!trunk){
            for(int j=0;j<ns;j++)if(!strcmp(name,sources[j].name)){const char *called=source_called(&sources[j]);if(called)name=called;break;}
            records[i]=lp_text_decompose(ctx,(const uint8_t *)name,strlen(name),NULL,NULL).id;
        }
    }
    lp_text_free(ctx);PGconn *pg=db_connect(conninfo);begin_maintenance(pg);
    size_t nr=0;lp_id *roots=selected_roots(pg,ids,records,n,except,&nr);free(ids);free(records);
    int result=forget_ids(pg,roots,nr);free(roots);
    exec(pg,result?"ROLLBACK":"COMMIT");
    if(result)fputs("laplace forget: no registered source trunk matched\n",stderr);
    PQfinish(pg);return result;
}
int forget_superseded(const char *conninfo,int connection_cap,const lp_id *trunk){
    (void)connection_cap;PGconn *pg=db_connect(conninfo);begin_maintenance(pg);
    Reader *reader=reader_new(pg);lp_id first[2];size_t n=reader_parts(reader,trunk,first,2);reader_free(reader);
    if(!n){exec(pg,"ROLLBACK");PQfinish(pg);return 1;}
    lp_id unused={{0}};size_t nr=0;lp_id *all=selected_roots(pg,&unused,first,1,0,&nr);size_t kept=0;
    for(size_t i=0;i<nr;i++)if(!lp_id_eq(&all[i],trunk))all[kept++]=all[i];
    int result=kept?forget_ids(pg,all,kept):0;free(all);exec(pg,result?"ROLLBACK":"COMMIT");PQfinish(pg);return result;
}
