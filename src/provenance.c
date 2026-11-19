/* Containment is read by the PostgreSQL Native operator. Rebuilding standings
 * uses exactly the same set-based Native solver as ingestion, in a transaction;
 * the CLI never ranks witnesses or computes ratings. */
#include "engine.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static lp_id *strands_named(PGconn *pg,int argc, char **argv, int a, int *ns, Hold **hs, int *nhs){
    Ctx *c = lp_text_new(T0); if (!HW) HW = lp_highway_map(NULL);
    int np=argc-a,nk=0;size_t capacity=(size_t)(np?np:1);
    lp_id *part=malloc(capacity*sizeof *part),*key=malloc(capacity*sizeof *key);int *have=calloc(capacity,sizeof *have);
    if(!part || !key || !have){perror("claim selectors");exit(1);}
    for (int i = 0; i < np; i++) { const char *p = argv[a + i]; have[i] = strcmp(p, "?") != 0; if (!have[i]) continue;
        const char *colon = strchr(p, ':'); int typed = 0;
        if (colon && colon > p && colon[1]) { char list[64]; snprintf(list, sizeof list, "%.*s", (int)(colon - p), p); Ref r = highway_typed(list, (const uint8_t *)colon + 1, strlen(colon + 1), &typed); if (typed) part[i] = r.id; }
        if (typed<0) { fprintf(stderr,"%s: ambiguous highway pointer; name its canonical target explicitly\n",p); exit(2); }
        if (!typed) part[i] = lp_text_decompose(c, (const uint8_t *)p, strlen(p), NULL, NULL).id;
        key[nk++] = part[i];
        char hx[33]; id_text(&part[i], hx); printf("  part %d   %-24s %s%s\n", i + 1, p, hx, typed ? "   (a type of the highway)" : ""); }
    int nh = 0; Hold *h = holds_above(pg,key, nk, -1, 0, 1, &nh); int w = 0; Ids ids = { 0 };
    for (int j = 0; j < nh; j++) { if (!h[j].claim) continue; lp_path p = lp_path_of(h[j].path, (size_t)h[j].path_len); size_t n = path_into(&ids, p);
        if ((int)n != np) continue; int fits = 1; for (int i = 0; i < np && fits; i++) if (have[i] && memcmp(&ids.v[i], &part[i], 16)) fits = 0;
        if (fits) { Hold x = h[j]; h[j] = h[w]; h[w++] = x; } }
    lp_vec_free(&ids);free(part);free(key);free(have);
    lp_id *out = malloc(sizeof(lp_id) * (size_t)(w ? w : 1)); for (int j = 0; j < w; j++) out[j] = h[j].entity;
    *ns = w; *hs = h; *nhs = nh; lp_text_free(c); return out;
}

Attested *lp_provenance_of(PGconn *pg,const lp_id *claims,size_t n,int files,int *nout){
    *nout=0;if(!n)return NULL;
    Args args={0};arg_ids(&args,claims,n);arg_bool(&args,files!=0);
    PGresult *q=db_call(pg,LP_SQL_ATTESTED_FILTERED,&args);
    int count=PQntuples(q);Attested *rows=calloc((size_t)(count?count:1),sizeof *rows);
    if(!rows){perror("provenance rows");exit(1);}
    for(int i=0;i<count;i++)rows[i]=(Attested){*col_id(q,i,0),*col_id(q,i,1),col_f64(q,i,2),col_f64(q,i,3),
        col_f64(q,i,4),PQgetisnull(q,i,5)?0u:(uint32_t)col_i32(q,i,5),col_f64(q,i,6),col_bool(q,i,7)};
    *nout=count;PQclear(q);args_free(&args);return rows;
}
static void print_rows(PGconn *pg,const lp_id *claims,size_t n,int files,FILE *out){
    int count=0;Attested *rows=lp_provenance_of(pg,claims,n,files,&count);
    for(int i=0;i<count;i++){
        const Attested *r=&rows[i];char claim[33],trunk[33];id_text(&r->claim,claim);id_text(&r->trunk,trunk);
        if(out)fprintf(out,"%s\t%s\t%.0f\t%.0f\t%.17g\t%u\n",claim,trunk,r->games,r->tokens,r->score,r->position);
        else printf("  %s  %s %s  games %.0f  tokens %.0f  mean %.9g  trust %.3f\n",
            claim,r->witness?"witness":"container",trunk,r->games,r->tokens,r->games?r->score/r->games:0,r->trust);
    }
    free(rows);
}
int cmd_held(int argc,char **argv){
    const char *conninfo=laplace_db(),*tsv=NULL;int files=0,byid=0;
    int a=opts(argc,argv,(const Opt[]){{.flag="-d",.kind='s',.at=&conninfo},{.flag="--files",.kind='b',.at=&files},
        {.flag="--tsv",.kind='s',.at=&tsv},{.flag="--id",.kind='b',.at=&byid},{0}});
    if(a>=argc){fputs("usage: laplace held [-d conninfo] [--files] [--tsv FILE] [--id] PART...\n",stderr);return 2;}
    tier0_open(NULL);PGconn *pg=db_read(conninfo);lp_id *claims=NULL;Hold *holds=NULL;int n=0,nh=0;
    if(byid){n=argc-a;claims=malloc((size_t)n*sizeof *claims);if(!claims){perror("claim IDs");exit(1);}
        for(int i=0;i<n;i++)if(!id_parse(argv[a+i],&claims[i])){fprintf(stderr,"%s: not an ID\n",argv[a+i]);free(claims);PQfinish(pg);return 2;}}
    else claims=strands_named(pg,argc,argv,a,&n,&holds,&nh);
    FILE *out=tsv?fopen(tsv,"w"):NULL;if(tsv&&!out){perror(tsv);free(claims);holds_free(holds,nh);PQfinish(pg);return 1;}
    printf("laplace held   %s   %d claims; Native GIN containment\n",PQdb(pg),n);
    print_rows(pg,claims,(size_t)n,files,out);
    if(out)fclose(out);free(claims);holds_free(holds,nh);PQfinish(pg);return 0;
}
static void replay_open(PGconn *pg,char table[96]){
    db_exec_contract(pg,LP_SQL_ENSURE_STAGE,NULL);
    db_exec_contract(pg,LP_SQL_STAGE_RETIRE,NULL);
    PGresult *q=db_call(pg,LP_SQL_CURRENT_XACT,NULL);
    if(PQntuples(q)!=1||PQgetisnull(q,0,0)){fputs("invalid replay transaction identity\n",stderr);PQclear(q);exit(1);}
    int64_t xid=col_i64(q,0,0);PQclear(q);
    snprintf(table,96,"stage.lp_replay_%lld",(long long)xid);
    if(db_stage_create_attestation(pg,table)!=0)exit(1);
    const char *owned[1]={table};
    Args own={0};arg_i64(&own,xid);arg_text_array(&own,owned,1);
    db_exec_contract(pg,LP_SQL_STAGE_OWN,&own);args_free(&own);
}
static void replay_close(PGconn *pg,const char *table){
    const char *tbl[1]={table};
    if(db_stage_drop(pg,tbl,1)!=0)exit(1);
}
static int replay_export(PGconn *pg,int files,FILE *out){
    Args args={0};arg_bool(&args,files!=0);
    char err[512];PGresult *q=db_call_try(pg,LP_SQL_REPLAY_EXPORT,&args,err,sizeof err);
    args_free(&args);
    if(!q){fprintf(stderr,"replay export: %s\n",err);return 1;}
    for(int i=0;i<PQntuples(q);i++){
        char claim[33],trunk[33];
        id_text(col_id(q,i,0),claim);id_text(col_id(q,i,1),trunk);
        if(PQgetisnull(q,i,5))
            fprintf(out,"%s\t%s\t%.0f\t%.0f\t%.17g\t\\N\n",
                claim,trunk,col_f64(q,i,2),col_f64(q,i,3),col_f64(q,i,4));
        else
            fprintf(out,"%s\t%s\t%.0f\t%.0f\t%.17g\t%d\n",
                claim,trunk,col_f64(q,i,2),col_f64(q,i,3),col_f64(q,i,4),col_i32(q,i,5));
    }
    PQclear(q);
    return ferror(out)?1:0;
}
int standings_replay(PGconn *pg,const lp_id *claims,size_t n,uint64_t *played,uint64_t *orphaned){
    *played=*orphaned=0;if(!n)return 0;
    int owns=PQtransactionStatus(pg)==PQTRANS_IDLE;if(owns)db_tx(pg,LP_SQL_TX_BEGIN);
    char table[96];replay_open(pg,table);
    Args selected={0};arg_text(&selected,table);arg_ids(&selected,claims,n);
    db_exec_contract(pg,LP_SQL_ATTEST_SERIES,&selected);args_free(&selected);
    Args args={0};arg_ids(&args,claims,n);
    PGresult *q=db_call(pg,LP_SQL_WITNESS_COUNT_MATCHES,&args);
    *played=(uint64_t)col_i64(q,0,0);*orphaned=(uint64_t)col_i64(q,0,1);
    PQclear(q);args_free(&args);replay_close(pg,table);if(owns)db_tx(pg,LP_SQL_TX_COMMIT);return 0;
}
int cmd_replay(int argc,char **argv){
    const char *conninfo=laplace_db(),*tsv=NULL;int write=0,files=0;
    int a=opts(argc,argv,(const Opt[]){{.flag="-d",.kind='s',.at=&conninfo},{.flag="--write",.kind='b',.at=&write},
        {.flag="--tsv",.kind='s',.at=&tsv},{.flag="--files",.kind='b',.at=&files},{0}});
    if(a<argc){fputs("usage: laplace replay [-d conninfo] [--write] [--files] [--tsv FILE]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);FILE *out=tsv?fopen(tsv,"w"):NULL;
    if(tsv&&!out){perror(tsv);PQfinish(pg);return 1;}
    db_tx(pg,LP_SQL_TX_BEGIN_RR);
    if(out){
        if(replay_export(pg,files,out)){fclose(out);PQfinish(pg);return 1;}fclose(out);
    }
    Args ra={0};arg_bool(&ra,write!=0);
    PGresult *q=db_call(pg,LP_SQL_REPLAY_VERIFY,&ra);args_free(&ra);
    uint64_t same=(uint64_t)col_i64(q,0,0),different=(uint64_t)col_i64(q,0,1);PQclear(q);
    db_tx(pg,write?LP_SQL_TX_COMMIT:LP_SQL_TX_ROLLBACK);
    printf("laplace replay   %s   %llu identical bit for bit, %llu different%s\n",PQdb(pg),
        (unsigned long long)same,(unsigned long long)different,write?"; rebuilt and committed":"; verification rolled back");
    PQfinish(pg);return !write&&different?1:0;
}
