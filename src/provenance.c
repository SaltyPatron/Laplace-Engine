/* Containment is read by the PostgreSQL Native operator. Rebuilding standings
 * uses exactly the same set-based Native solver as ingestion, in a transaction;
 * the CLI never ranks witnesses or computes ratings. */
#include "engine.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lp_id *strands_named(int argc, char **argv, int a, int *ns, Hold **hs, int *nhs){
    Ctx *c = lp_text_new(T0); if (!HW) HW = lp_highway_map(NULL);
    int np = argc - a; lp_id part[MAXPARTS]; int have[MAXPARTS], nk = 0; lp_id key[MAXPARTS];
    for (int i = 0; i < np && i < MAXPARTS; i++) { const char *p = argv[a + i]; have[i] = strcmp(p, "?") != 0; if (!have[i]) continue;
        const char *colon = strchr(p, ':'); int typed = 0;
        if (colon && colon > p && colon[1]) { char list[64]; snprintf(list, sizeof list, "%.*s", (int)(colon - p), p); Ref r = highway_typed(list, (const uint8_t *)colon + 1, strlen(colon + 1), &typed); if (typed) part[i] = r.id; }
        if (!typed) part[i] = lp_text_decompose(c, (const uint8_t *)p, strlen(p), NULL, NULL).id;
        key[nk++] = part[i];
        char hx[33]; id_text(&part[i], hx); printf("  part %d   %-24s %s%s\n", i + 1, p, hx, typed ? "   (a type of the highway)" : ""); }
    int nh = 0; Hold *h = holds_above(key, nk, -1, 0, 1, &nh); int w = 0; Ids ids = { 0 };
    for (int j = 0; j < nh; j++) { if (!h[j].claim) continue; lp_path p = lp_path_of(h[j].path, (size_t)h[j].path_len); size_t n = path_into(&ids, p);
        if ((int)n != np) continue; int fits = 1; for (int i = 0; i < np && fits; i++) if (have[i] && memcmp(&ids.v[i], &part[i], 16)) fits = 0;
        if (fits) { Hold x = h[j]; h[j] = h[w]; h[w++] = x; } }
    lp_vec_free(&ids);
    lp_id *out = malloc(sizeof(lp_id) * (size_t)(w ? w : 1)); for (int j = 0; j < w; j++) out[j] = h[j].entity;
    *ns = w; *hs = h; *nhs = nh; lp_text_free(c); return out;
}

Attested *lp_provenance_of(PGconn *pg,const lp_id *claims,size_t n,int files,int *nout){
    *nout=0;if(!n)return NULL;
    Args args={0};arg_ids(&args,claims,n);uint8_t flag=files!=0;arg_raw(&args,&flag,1);
    PGresult *q=ask_once(pg,"SELECT claim,trunk,games,tokens,score,\"position\",trust,witness"
        " FROM laplace_attested($1::blake3[],$2::boolean) WHERE witness OR $2 ORDER BY claim,trunk",&args);
    int count=PQntuples(q);Attested *rows=calloc((size_t)(count?count:1),sizeof *rows);
    if(!rows){perror("provenance rows");exit(1);}
    for(int i=0;i<count;i++)rows[i]=(Attested){*col_id(q,i,0),*col_id(q,i,1),col_f64(q,i,2),col_f64(q,i,3),
        col_f64(q,i,4),(uint32_t)col_int(q,i,5),col_f64(q,i,6),PQgetvalue(q,i,7)[0]!=0};
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
    tier0_open(NULL);PGconn *pg=db_connect(conninfo);lp_id *claims=NULL;Hold *holds=NULL;int n=0,nh=0;
    if(byid){n=argc-a;claims=malloc((size_t)n*sizeof *claims);if(!claims){perror("claim IDs");exit(1);}
        for(int i=0;i<n;i++)if(!id_parse(argv[a+i],&claims[i])){fprintf(stderr,"%s: not an ID\n",argv[a+i]);free(claims);PQfinish(pg);return 2;}}
    else claims=strands_named(argc,argv,a,&n,&holds,&nh);
    FILE *out=tsv?fopen(tsv,"w"):NULL;if(tsv&&!out){perror(tsv);free(claims);holds_free(holds,nh);PQfinish(pg);return 1;}
    printf("laplace held   %s   %d claims; Native GIN containment\n",PQdb(pg),n);
    print_rows(pg,claims,(size_t)n,files,out);
    if(out)fclose(out);free(claims);holds_free(holds,nh);PQfinish(pg);return 0;
}
static void replay_open(PGconn *pg,char table[96]){
    exec(pg,"SELECT laplace_ensure_stage()");
    PGresult *q=ask_once(pg,"SELECT pg_current_xact_id()::text",NULL);const char *id=PQgetvalue(q,0,0);
    if(!*id||strspn(id,"0123456789")!=strlen(id)){fputs("invalid replay transaction identity\n",stderr);exit(1);}
    snprintf(table,96,"stage.lp_replay_%s",id);PQclear(q);
    char sql[256];snprintf(sql,sizeof sql,"CREATE UNLOGGED TABLE %s (LIKE stage.attestation INCLUDING DEFAULTS)",table);exec(pg,sql);
}
static void replay_close(PGconn *pg,const char *table){char sql[160];snprintf(sql,sizeof sql,"DROP TABLE %s",table);exec(pg,sql);}
int standings_replay(PGconn *pg,const lp_id *claims,size_t n,uint64_t *played,uint64_t *orphaned){
    *played=*orphaned=0;if(!n)return 0;
    int owns=PQtransactionStatus(pg)==PQTRANS_IDLE;if(owns)exec(pg,"BEGIN");
    char table[96];replay_open(pg,table);
    Args args={0};arg_text(&args,table);arg_ids(&args,claims,n);
    PQclear(ask_once(pg,"SELECT laplace_attest_series($1::regclass,$2::blake3[])",&args));args_free(&args);
    args=(Args){0};arg_ids(&args,claims,n);
    PGresult *q=ask_once(pg,"SELECT count(*),"
        " (SELECT count(DISTINCT id) FROM unnest($1::blake3[]) id)-count(*)"
        " FROM public.consensus WHERE claim=ANY($1::blake3[])",&args);
    *played=lp_be(PQgetvalue(q,0,0),8);*orphaned=lp_be(PQgetvalue(q,0,1),8);
    PQclear(q);args_free(&args);replay_close(pg,table);if(owns)exec(pg,"COMMIT");return 0;
}
int cmd_replay(int argc,char **argv){
    const char *conninfo=laplace_db(),*tsv=NULL;int write=0,files=0;
    int a=opts(argc,argv,(const Opt[]){{.flag="-d",.kind='s',.at=&conninfo},{.flag="--write",.kind='b',.at=&write},
        {.flag="--tsv",.kind='s',.at=&tsv},{.flag="--files",.kind='b',.at=&files},{0}});
    if(a<argc){fputs("usage: laplace replay [-d conninfo] [--write] [--files] [--tsv FILE]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);FILE *out=tsv?fopen(tsv,"w"):NULL;
    if(tsv&&!out){perror(tsv);PQfinish(pg);return 1;}
    exec(pg,"BEGIN ISOLATION LEVEL REPEATABLE READ");
    exec(pg,"CREATE TEMP TABLE replay_claims(claim blake3 PRIMARY KEY) ON COMMIT DROP;"
        " INSERT INTO replay_claims SELECT entity FROM public.physicality"
        " WHERE laplace_mask_has_value(mask,'kind','claim') AND laplace_path_length(path)>=2"
        " UNION SELECT claim FROM public.consensus;"
        " CREATE TEMP TABLE replay_before ON COMMIT DROP AS SELECT * FROM public.consensus");
    if(out){
        exec(pg,"DECLARE replay_export NO SCROLL CURSOR FOR SELECT claim FROM replay_claims ORDER BY claim");
        for(;;){
            PGresult *q=ask_once(pg,"FETCH 8192 FROM replay_export",NULL);size_t n=(size_t)PQntuples(q);
            if(!n){PQclear(q);break;}lp_id *ids=malloc(n*sizeof *ids);if(!ids){perror("replay export IDs");exit(1);}
            for(size_t i=0;i<n;i++)ids[i]=*col_id(q,(int)i,0);PQclear(q);print_rows(pg,ids,n,files,out);free(ids);
        }
        exec(pg,"CLOSE replay_export");fclose(out);
    }
    exec(pg,"DELETE FROM public.consensus c WHERE NOT EXISTS(SELECT FROM public.physicality p"
        " WHERE p.entity=c.claim AND laplace_mask_has_value(p.mask,'kind','claim') AND laplace_path_length(p.path)>=2);"
        " DELETE FROM replay_claims c WHERE NOT EXISTS(SELECT FROM public.physicality p"
        " WHERE p.entity=c.claim AND laplace_mask_has_value(p.mask,'kind','claim') AND laplace_path_length(p.path)>=2)");
    char table[96],sql[1800];replay_open(pg,table);
    snprintf(sql,sizeof sql,
        "DO $rebuild$ DECLARE batch blake3[]; last_id blake3; BEGIN LOOP"
        " SELECT array_agg(claim ORDER BY claim) INTO batch FROM"
        " (SELECT claim FROM replay_claims WHERE last_id IS NULL OR claim>last_id ORDER BY claim LIMIT 8192) selected;"
        " EXIT WHEN batch IS NULL; PERFORM laplace_attest_series('%s'::regclass,batch);"
        " last_id:=batch[cardinality(batch)]; END LOOP; END $rebuild$",table);
    exec(pg,sql);replay_close(pg,table);
    PGresult *q=ask_once(pg,
        "SELECT count(*) FILTER(WHERE b.claim IS NOT NULL AND a.claim IS NOT NULL AND"
        " ROW(float8send(b.rating),float8send(b.deviation),float8send(b.volatility),b.matches)"
        " IS NOT DISTINCT FROM ROW(float8send(a.rating),float8send(a.deviation),float8send(a.volatility),a.matches)),"
        " count(*) FILTER(WHERE b.claim IS NULL OR a.claim IS NULL OR"
        " ROW(float8send(b.rating),float8send(b.deviation),float8send(b.volatility),b.matches)"
        " IS DISTINCT FROM ROW(float8send(a.rating),float8send(a.deviation),float8send(a.volatility),a.matches))"
        " FROM replay_before b FULL JOIN public.consensus a USING(claim)",NULL);
    uint64_t same=lp_be(PQgetvalue(q,0,0),8),different=lp_be(PQgetvalue(q,0,1),8);PQclear(q);
    exec(pg,write?"COMMIT":"ROLLBACK");
    printf("laplace replay   %s   %llu identical bit for bit, %llu different%s\n",PQdb(pg),
        (unsigned long long)same,(unsigned long long)different,write?"; rebuilt and committed":"; verification rolled back");
    PQfinish(pg);return !write&&different?1:0;
}
