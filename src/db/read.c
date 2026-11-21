/* Connection-owned containment projections over the shared Native indexed reader. */
#include "engine.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        PGresult *rows=db_call(pg,LP_SQL_STANDINGS,&args);
        for(int row=0;row<PQntuples(rows);row++){
            size_t at=begin+(size_t)col_i64(rows,row,0)-1;
            for(int i=*(int *)lp_idmap_at(wanted,at);i>=0;i=links[i]){
                holds[i].r=col_rating(rows,row,1);holds[i].matches=col_i64(rows,row,4);holds[i].stood=1;
            }
        }
        PQclear(rows);begin+=amount;
    }
    args_free(&args);free(links);lp_idmap_free(wanted);
}
static Hold *holders_scoped(PGconn *pg,const lp_id *keys,int nkeys,int floor,int each,int standing,
                            const lp_id *required,int cap,int kinds,int endpoint,
                            const lp_id *witnesses,int nwitnesses,int scoped,int *hub,int *nout){
    *nout=0;if(!nkeys)return NULL;
    if(scoped && (!witnesses || nwitnesses<=0)){
        if(hub)memset(hub,0,sizeof(int)*2*(size_t)nkeys);
        return NULL;
    }
    Args args={0};arg_ids(&args,keys,(size_t)nkeys);
    arg_ids(&args,required,required?1:0);
    PGresult *rows;
    if(!each){
        arg_i16(&args,(int16_t)floor);arg_i16(&args,(int16_t)kinds);
        rows=db_call(pg,LP_SQL_CONTAINERS_ABOVE,&args);
    }else{
        arg_bool(&args,each==1);arg_i16(&args,(int16_t)floor);arg_i64(&args,(int64_t)cap);
        arg_i16(&args,(int16_t)kinds);arg_null(&args);arg_i16(&args,(int16_t)endpoint);
        if(scoped){
            arg_ids(&args,witnesses,(size_t)nwitnesses);
            rows=db_call(pg,LP_SQL_HOLDERS_WITH_STANDINGS_SCOPED,&args);
        }else{
            rows=db_call(pg,standing?LP_SQL_HOLDERS_WITH_STANDINGS:LP_SQL_HOLDERS,&args);
        }
    }
    lp_vec(Hold) out={0};
    for(int row=0;row<PQntuples(rows);row++){
        int ordinal=each?(int)col_i64(rows,row,0):0;
        int col_base=each?1:0;
        int claim=(int)col_i16(rows,row,col_base+4);
        if(each && col_bool(rows,row,6)){
            if(hub && ordinal>0 && ordinal<=nkeys)hub[2*(ordinal-1)+(claim?0:1)]=1;
            continue;
        }
        Hold hold={.src=each==1?ordinal-1:each==2?-1:0,.entity=*col_id(rows,row,col_base),
            .tier=col_i16(rows,row,col_base+2),.claim=(uint8_t)claim,.path_len=PQgetlength(rows,row,col_base+1)};
        hold.path=malloc((size_t)(hold.path_len?hold.path_len:1));
        memcpy(hold.path,PQgetvalue(rows,row,col_base+1),(size_t)hold.path_len);
        if(each && standing && !PQgetisnull(rows,row,7)){
            hold.r=col_rating(rows,row,7);hold.matches=col_i64(rows,row,10);hold.stood=1;
        }
        lp_push(&out,hold);
    }
    PQclear(rows);args_free(&args);
    if(out.n>1)qsort(out.v,out.n,sizeof(Hold),hold_order);
    if(!each && standing && out.n)standings_of(pg,out.v,(int)out.n);
    *nout=(int)out.n;return out.v;
}
static Hold *holders(PGconn *pg,const lp_id *keys,int nkeys,int floor,int each,int standing,
                     const lp_id *required,int cap,int kinds,int *hub,int *nout){
    return holders_scoped(pg,keys,nkeys,floor,each,standing,required,cap,kinds,0,NULL,0,0,hub,nout);
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
    return holders_scoped(pg,keys,n,-1,1,standing,NULL,cap,kinds,0,NULL,0,0,hub,nout);
}
Hold *holds_endpoint(PGconn *pg,const lp_id *keys,int n,int cap,int standing,int kinds,int endpoint,int *hub,int *nout){
    if(hub)memset(hub,0,sizeof(int)*2*(size_t)n);
    return holders_scoped(pg,keys,n,-1,1,standing,NULL,cap,kinds,endpoint,NULL,0,0,hub,nout);
}
Hold *holds_capped_scoped(PGconn *pg,const lp_id *keys,int n,int cap,int standing,int kinds,
                          const lp_id *witnesses,int nwitnesses,int scoped,int *hub,int *nout){
    if(hub)memset(hub,0,sizeof(int)*2*(size_t)n);
    return holders_scoped(pg,keys,n,-1,1,standing,NULL,cap,kinds,0,witnesses,nwitnesses,scoped,hub,nout);
}
Hold *holds_both_scoped(PGconn *pg,const lp_id *keys,int n,const lp_id *with,
                        const lp_id *witnesses,int nwitnesses,int scoped,int *nout){
    return holders_scoped(pg,keys,n,-1,1,1,with,-1,3,0,witnesses,nwitnesses,scoped,NULL,nout);
}
LpSalienceRow *salience_of(PGconn *pg,const lp_id *occ,int nocc,const lp_id *scope,int nscope,int *nout){
    *nout=0;
    if(!pg || !occ || nocc<=0)return NULL;
    Args args={0};
    arg_ids(&args,occ,(size_t)nocc);
    arg_ids(&args,scope,(size_t)(scope && nscope>0?nscope:0));
    PGresult *rows=db_call(pg,LP_SQL_SALIENCE,&args);
    int nr=PQntuples(rows);
    LpSalienceRow *out=calloc((size_t)(nr?nr:1),sizeof *out);
    for(int r=0;r<nr;r++){
        out[r].ord=col_i32(rows,r,0);
        out[r].id=*col_id(rows,r,1);
        out[r].tier=col_i16(rows,r,2);
        out[r].df=col_i64(rows,r,3);
        out[r].total=col_i64(rows,r,4);
        out[r].info_bits=col_f64(rows,r,5);
        out[r].span_lift=col_f64(rows,r,6);
        out[r].loss_contrast=col_f64(rows,r,7);
        out[r].salience=col_f64(rows,r,8);
    }
    PQclear(rows);args_free(&args);
    *nout=nr;
    return out;
}
long holds_count(PGconn *pg,const lp_id *keys,int n,int kind){
    if(!n)return 0;Args args={0};arg_ids(&args,keys,(size_t)n);arg_i16(&args,(int16_t)kind);
    PGresult *rows=db_call(pg,LP_SQL_CONTAINERS_COUNT,&args);
    long count=(long)col_i64(rows,0,0);PQclear(rows);args_free(&args);return count;
}

/* Up the container index to the files inside PostgreSQL (laplace_held_closure).
 * Deduplicates (parent, child) edges in vertex order and projects seed hub flags
 * and node/edge arrays onto HeldUp. */
void held_up(PGconn *pg,const lp_id *keys,int nkeys,int by,int cap,size_t max_nodes,HeldUp *out){
    memset(out,0,sizeof *out);
    if(nkeys<=0)return;
    Args args={0};
    arg_ids(&args,keys,(size_t)nkeys);
    arg_i16(&args,(int16_t)by);
    arg_i64(&args,(int64_t)cap);
    arg_i32(&args,128);
    arg_i32(&args,(int32_t)(max_nodes>(size_t)INT32_MAX?(size_t)INT32_MAX:max_nodes));
    PGresult *rows=db_call(pg,LP_SQL_HELD_CLOSURE,&args);
    args_free(&args);
    int nr=PQntuples(rows);
    if(nr>0){
        out->n=(size_t)nr;
        out->node=calloc(out->n,sizeof *out->node);
        out->cut=col_bool(rows,0,13)?1:0;
        out->levels=col_i32(rows,0,14);
        lp_strmap *pairs=lp_strmap_sized(1);
        size_t ecap=0,ac=64;
        int32_t *cidx=malloc(ac*sizeof *cidx);
        int16_t *csaid=malloc(ac*sizeof *csaid);
        for(int row=0;row<nr;row++){
            int32_t pidx=col_i32(rows,row,0);
            if(pidx<0 || (size_t)pidx>=out->n)continue;
            out->node[pidx].id=*col_id(rows,row,1);
            out->node[pidx].keys=(uint64_t)col_i64(rows,row,3);
            out->node[pidx].file=col_bool(rows,row,4)?1:0;
            if(col_bool(rows,row,6) && col_i32(rows,row,8)==0 && pidx<64)
                out->hub|=1ull<<(uint32_t)pidx;
            size_t nc=col_i32_array(rows,row,9,cidx,ac);
            if(nc>ac){
                ac=nc;
                cidx=realloc(cidx,ac*sizeof *cidx);
                csaid=realloc(csaid,ac*sizeof *csaid);
                nc=col_i32_array(rows,row,9,cidx,ac);
            }
            col_i16_array(rows,row,10,csaid,ac);
            for(size_t k=0;k<nc;k++){
                if(cidx[k]<0 || (size_t)cidx[k]>=out->n)continue;
                uint64_t key=((uint64_t)(uint32_t)pidx<<32)|(uint64_t)(uint32_t)cidx[k];
                bool added;
                lp_strmap_put(pairs,&key,sizeof key,&added);
                if(added){
                    lp_reserve((void **)&out->edge,&ecap,out->ne+1,sizeof *out->edge);
                    out->edge[out->ne++]=(HeldEdge){(uint32_t)pidx,(uint32_t)cidx[k],(uint8_t)csaid[k]};
                }
            }
        }
        free(cidx);free(csaid);lp_strmap_free(pairs);
    }
    PQclear(rows);
}
void held_up_free(HeldUp *u){free(u->node);free(u->edge);memset(u,0,sizeof *u);}
HeldFile *held_in(PGconn *pg,const lp_id *trunk,const lp_id *keys,int nkeys,long cap,long max_nodes,
                  uint64_t *hubs,int *levels,int *cut,size_t *nout){
    *nout=0;*hubs=0;*levels=0;*cut=0;
    if(nkeys<=0)return NULL;
    Args args={0};arg_ids(&args,keys,(size_t)nkeys);
    if(trunk)arg_id(&args,trunk); else arg_null(&args);
    arg_i64(&args,(int64_t)cap);arg_i64(&args,(int64_t)max_nodes);
    PGresult *rows=db_call(pg,LP_SQL_HELD_FILES,&args);
    lp_vec(HeldFile) out={0};
    for(int row=0;row<PQntuples(rows);row++){
        *hubs=(uint64_t)col_i64(rows,row,2);*levels=(int)col_i64(rows,row,3);*cut=col_bool(rows,row,4)?1:0;
        if(PQgetisnull(rows,row,0))continue;
        HeldFile held={.file=*col_id(rows,row,0),.unit=*col_id(rows,row,5),.keys=(uint64_t)col_i64(rows,row,1),
                       .length=col_i64(rows,row,6),.tier=(int)col_i64(rows,row,8)};
        /* counts: a binary int8[] of one dimension, each element its length then its value */
        const uint8_t *a=(const uint8_t *)PQgetvalue(rows,row,7);int bytes=PQgetlength(rows,row,7);
        if(bytes>=20 && lp_be(a,4)==1){
            size_t elements=(size_t)lp_be(a+12,4),at=20;
            for(size_t k=0;k<elements && k<64 && at+12<=(size_t)bytes;k++,at+=12)held.count[k]=lp_be(a+at+4,8);
        }
        lp_push(&out,held);
    }
    PQclear(rows);args_free(&args);*nout=out.n;return out.v;
}
