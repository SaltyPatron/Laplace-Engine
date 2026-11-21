/* Forgetting removes witness authority and only the unreferenced part of its
 * structural closure. All discovery, sweeping and Native series updates share
 * one database transaction; no partition is exported to client RAM. */
#include "engine.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_sweep(int argc,char **argv){
    const char *conninfo=laplace_db();int limit=0,dry=0;
    int a=opts(argc,argv,(const Opt[]){{.flag="-d",.kind='s',.at=&conninfo},{.flag="-j",.kind='i',.at=&limit},
        {.flag="--dry",.kind='b',.at=&dry},{0}});
    if(a<argc||limit<0){fputs("usage: laplace sweep [-d conninfo] [-j connection-cap] [--dry]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);
    if(db_ddl_begin_maintenance(pg)!=0){PQfinish(pg);return 1;}
    Args args={0};arg_null(&args);
    PGresult *q=db_call(pg,LP_SQL_SWEEP_ORPHANS,&args);args_free(&args);
    uint64_t n=(uint64_t)col_i64(q,0,0);PQclear(q);
    db_tx(pg,dry?LP_SQL_TX_ROLLBACK:LP_SQL_TX_COMMIT);
    printf("laplace sweep   %s   %llu unreferenced entities %s\n",PQdb(pg),(unsigned long long)n,dry?"would be removed":"removed");
    PQfinish(pg);return 0;
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
    lp_text_free(ctx);PGconn *pg=db_connect(conninfo);
    if(db_ddl_begin_maintenance(pg)!=0){free(ids);free(records);PQfinish(pg);return 1;}
    Args fa={0};arg_ids(&fa,ids,n);arg_ids(&fa,records,n);arg_bool(&fa,except!=0);arg_null(&fa);
    free(ids);free(records);
    PGresult *q=db_call(pg,LP_SQL_FORGET_WITNESSES,&fa);args_free(&fa);
    uint64_t wit=(uint64_t)col_i64(q,0,0),ent=(uint64_t)col_i64(q,0,1),
        standing=(uint64_t)col_i64(q,0,2),orphaned=(uint64_t)col_i64(q,0,3);
    PQclear(q);
    int result=wit?0:1;
    if(!result)
        printf("  %llu witnesses removed; %llu unreferenced entities removed; %llu remaining standings recomputed; %llu claims without a witness\n",
            (unsigned long long)wit,(unsigned long long)ent,(unsigned long long)standing,(unsigned long long)orphaned);
    db_tx(pg,result?LP_SQL_TX_ROLLBACK:LP_SQL_TX_COMMIT);
    if(result)fputs("laplace forget: no registered source trunk matched\n",stderr);
    PQfinish(pg);return result;
}
int forget_superseded(const char *conninfo,int connection_cap,const lp_id *trunk){
    (void)connection_cap;PGconn *pg=db_connect(conninfo);
    if(db_ddl_begin_maintenance(pg)!=0){PQfinish(pg);return 1;}
    Reader *reader=reader_new(pg);lp_id first[2];size_t n=reader_parts(reader,trunk,first,2);reader_free(reader);
    if(!n){db_tx(pg,LP_SQL_TX_ROLLBACK);PQfinish(pg);return 1;}
    Args fa={0};arg_null(&fa);arg_ids(&fa,first,1);arg_bool(&fa,false);arg_id(&fa,trunk);
    PGresult *q=db_call(pg,LP_SQL_FORGET_WITNESSES,&fa);args_free(&fa);
    uint64_t wit=(uint64_t)col_i64(q,0,0),ent=(uint64_t)col_i64(q,0,1),
        standing=(uint64_t)col_i64(q,0,2),orphaned=(uint64_t)col_i64(q,0,3);
    PQclear(q);
    if(wit)
        printf("  %llu witnesses removed; %llu unreferenced entities removed; %llu remaining standings recomputed; %llu claims without a witness\n",
            (unsigned long long)wit,(unsigned long long)ent,(unsigned long long)standing,(unsigned long long)orphaned);
    db_tx(pg,LP_SQL_TX_COMMIT);PQfinish(pg);return 0;
}
