/* Connection-owned containment projections over the shared Native indexed reader. */
#include "engine.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t ids_param(PGconn *pg,uint8_t *out, const lp_id *ids, uint32_t n){
    lp_buf encoded={.b=out,.cap=20+20*(size_t)n};
    lp_pg_ids(&encoded,db_binding(pg)->id_type,ids,(size_t)n);
    return encoded.n;
}
void id_text(const lp_id *id, char out[33]){ lp_id_hex(id, out); }

/* A trajectory's constituents in order, runs written out, from a path as the database sends it. What a record says, its
 * claims and the voices that say them (LP_SAID_CLAIM, LP_SAID_VOICE), is no part of its content's order: they are left
 * out, and a record reads as its thing. */
Run run_of(const uint8_t *ewkb, size_t len){
    lp_path p = lp_path_of(ewkb, len); size_t n = lp_path_len(p); Run r = { malloc(sizeof(lp_id) * (n ? n : 1)), 0 };
    for (size_t i = 0; i < p.n; i++) { double m; memcpy(&m, p.v + i * 32 + 24, 8); uint32_t sd = lp_m_said(m); if (sd == LP_SAID_CLAIM || sd == LP_SAID_VOICE) continue;
        lp_id id = lp_path_id(p, i); uint32_t run = lp_path_run(p, i); for (uint32_t k = 0; k < run; k++) r.id[r.n++] = id; }
    return r;
}


/* Duplicate holders share one standing lookup; request occurrences keep their
 * own path buffers and source positions. No partition names cross this API. */
static void standings_of(PGconn *pg,Hold *holds,int count){
    lp_idmap *wanted=lp_idmap_sized(sizeof(int));
    int *links=malloc(sizeof(int)*(size_t)(count?count:1));
    for(int i=count-1;i>=0;i--){
        bool fresh;size_t at=lp_idmap_put(wanted,&holds[i].entity,&fresh);
        int *head=lp_idmap_at(wanted,at);if(fresh)*head=-1;links[i]=*head;*head=i;
    }
    Args args={0};size_t n=lp_idmap_count(wanted);
    for(size_t begin=0;begin<n;){
        size_t amount=n-begin;if(amount>DB_ID_WINDOW)amount=DB_ID_WINDOW;
        args_reset(&args);arg_ids(&args,lp_idmap_keys(wanted)+begin,amount);
        PGresult *rows=ask(pg,"SELECT ordinal,rating,deviation,volatility,matches FROM @extschema@.laplace_standings($1::@extschema@.blake3[])",&args);
        for(int row=0;row<PQntuples(rows);row++){
            size_t at=begin+(size_t)col_int(rows,row,0)-1;
            for(int i=*(int *)lp_idmap_at(wanted,at);i>=0;i=links[i]){
                holds[i].r=col_rating(rows,row,1);holds[i].matches=col_int(rows,row,4);holds[i].stood=1;
            }
        }
        PQclear(rows);begin+=amount;
    }
    args_free(&args);free(links);lp_idmap_free(wanted);
}
static Hold *holders(PGconn *pg,const lp_id *keys,int nkeys,int floor,int each,int standing,
                     const lp_id *required,int cap,int kinds,int *hub,int *nout){
    *nout=0;if(!nkeys)return NULL;
    Args args={0};arg_ids(&args,keys,(size_t)nkeys);
    arg_ids(&args,required,required?1:0);
    PGresult *rows;
    if(!each){
        arg_int(&args,floor);arg_int(&args,kinds);
        rows=ask(pg,"SELECT 0::bigint,entity,path,tier,mask,CASE WHEN mask OPERATOR(@extschema@.?) 0::smallint THEN 1 ELSE 0 END::smallint,false"
            " FROM @extschema@.laplace_containers($1::@extschema@.blake3[]||$2::@extschema@.blake3[],'{}'::smallint[])"
            " WHERE tier>$3::smallint AND (($4::smallint & CASE WHEN mask OPERATOR(@extschema@.?) 0::smallint THEN 1 ELSE 2 END)<>0)",&args);
    }else{
        arg_int(&args,each==1);arg_int(&args,floor);arg_int(&args,cap);arg_int(&args,kinds);
        rows=ask(pg,"SELECT ordinal,entity,path,tier,mask,kind,hub FROM @extschema@.laplace_holders("
            "$1::@extschema@.blake3[],$2::@extschema@.blake3[],$3::integer<>0,$4::smallint,$5::bigint,$6::smallint)",&args);
    }
    lp_vec(Hold) out={0};
    for(int row=0;row<PQntuples(rows);row++){
        int ordinal=(int)col_int(rows,row,0),claim=(int)col_int(rows,row,5);
        if(PQgetvalue(rows,row,6)[0]){
            if(hub && ordinal>0 && ordinal<=nkeys)hub[2*(ordinal-1)+(claim?0:1)]=1;
            continue;
        }
        Hold hold={.src=each==1?ordinal-1:each==2?-1:0,.entity=*col_id(rows,row,1),
            .tier=(int16_t)col_int(rows,row,3),.claim=(uint8_t)claim,.path_len=PQgetlength(rows,row,2)};
        hold.path=malloc((size_t)(hold.path_len?hold.path_len:1));
        memcpy(hold.path,PQgetvalue(rows,row,2),(size_t)hold.path_len);lp_push(&out,hold);
    }
    PQclear(rows);args_free(&args);
    if(out.n>1)qsort(out.v,out.n,sizeof(Hold),hold_order);
    if(standing && out.n)standings_of(pg,out.v,(int)out.n);
    *nout=(int)out.n;return out.v;
}
Hold *holds_above(PGconn *pg,const lp_id *keys,int n,int floor,int each,int standing,int *nout){
    return holders(pg,keys,n,floor,each,standing,NULL,-1,3,NULL,nout);
}
Hold *holds_pair(PGconn *pg,const lp_id *keys,int n,const lp_id *required,int standing,int *nout){
    return holders(pg,keys,n,-1,2,standing,required,-1,3,NULL,nout);
}
Hold *holds_both(PGconn *pg,const lp_id *keys,int n,const lp_id *required,int *nout){
    return holders(pg,keys,n,-1,1,1,required,-1,3,NULL,nout);
}
Hold *holds_any(PGconn *pg,const lp_id *keys,int n,int *nout){
    return holders(pg,keys,n,-1,2,0,NULL,-1,3,NULL,nout);
}
Hold *holds_capped(PGconn *pg,const lp_id *keys,int n,int cap,int standing,int kinds,int *hub,int *nout){
    memset(hub,0,sizeof(int)*2*(size_t)n);
    return holders(pg,keys,n,-1,1,standing,NULL,cap,kinds,hub,nout);
}
long holds_count(PGconn *pg,const lp_id *keys,int n,int kind){
    if(!n)return 0;Args args={0};arg_ids(&args,keys,(size_t)n);arg_int(&args,kind);
    PGresult *rows=ask(pg,"SELECT count(*) FROM @extschema@.laplace_containers($1::@extschema@.blake3[],'{}'::smallint[])"
        " WHERE $2::smallint<0 OR (mask OPERATOR(@extschema@.?) 0::smallint)=($2::smallint=1)",&args);
    long count=(long)col_int(rows,0,0);PQclear(rows);args_free(&args);return count;
}
