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

/* Up the container index to the files (engine.h, held_up). The front is the nodes whose keys changed at the level
 * before; each holder of a front node takes the keys of every front node it holds (by content, or by its record), and
 * is in the next front when it is new or its keys grew, unless it is a file. Keys only grow, so a node re-enters the
 * front at most once a key. */
static size_t held_node(HeldUp *out,lp_idmap *at,size_t *cap,const lp_id *id,bool *fresh){
    size_t i=lp_idmap_put(at,id,fresh);
    if(*fresh){lp_reserve((void **)&out->node,cap,out->n+1,sizeof *out->node);out->node[out->n++]=(Held){.id=*id};}
    return i;
}
void held_up(PGconn *pg,const lp_id *keys,int nkeys,int by,int cap,size_t max_nodes,HeldUp *out){
    memset(out,0,sizeof *out);
    if(nkeys<=0)return;
    lp_idmap *at=lp_idmap_new(),*front=lp_idmap_new();
    lp_strmap *pairs=lp_strmap_sized(1);
    size_t ncap=0,ecap=0;lp_vec(lp_vertex) vx={0};
    for(int k=0;k<nkeys;k++){
        bool fresh;size_t i=held_node(out,at,&ncap,keys+k,&fresh);
        if(k<64)out->node[i].keys|=1ull<<k;
        lp_idmap_put(front,keys+k,NULL);
    }
    while(lp_idmap_count(front) && out->levels<128 && !out->cut){
        int nh=0;Hold *h;
        if(out->levels==0 && cap>=0){
            int *hub=calloc((size_t)nkeys*2,sizeof *hub);
            h=holds_capped(pg,lp_idmap_keys(front),(int)lp_idmap_count(front),cap,0,3,hub,&nh);
            for(int k=0;k<nkeys && k<64;k++)if(hub[2*k] || hub[2*k+1])out->hub|=1ull<<k;
            free(hub);
        }else h=holds_any(pg,lp_idmap_keys(front),(int)lp_idmap_count(front),&nh);
        lp_idmap *next=lp_idmap_new();
        for(int j=0;j<nh && !out->cut;j++){
            size_t nv=lp_path_vertices(h[j].path,(size_t)h[j].path_len,NULL,0);if(!nv)continue;
            lp_vec_reserve(&vx,nv);lp_path_vertices(h[j].path,(size_t)h[j].path_len,vx.v,nv);
            int file=vx.v[0].said==LP_SAID_METADATA;size_t parent=SIZE_MAX;uint64_t before=0;bool grew=false;
            /* by name, a file is reached through its record (its first vertex) and nothing else that is a file counts */
            if(by==HELD_NAME && file && lp_idmap_find(front,&vx.v[0].id)<0)continue;
            for(size_t v=0;v<nv;v++){
                if(lp_idmap_find(front,&vx.v[v].id)<0)continue;
                if(by==HELD_CONTENT ? vx.v[v].said==LP_SAID_METADATA : file && v>0)continue;
                int64_t child=lp_idmap_find(at,&vx.v[v].id);if(child<0)continue;
                if(parent==SIZE_MAX){
                    bool fresh;
                    if(lp_idmap_find(at,&h[j].entity)<0 && out->n>=max_nodes){out->cut=1;break;}
                    parent=held_node(out,at,&ncap,&h[j].entity,&fresh);
                    out->node[parent].file|=(uint8_t)file;grew=fresh;before=out->node[parent].keys;
                }
                out->node[parent].keys|=out->node[child].keys;
                uint64_t key=((uint64_t)parent<<32)|(uint64_t)child;bool added;
                lp_strmap_put(pairs,&key,sizeof key,&added);
                if(added){lp_reserve((void **)&out->edge,&ecap,out->ne+1,sizeof *out->edge);
                    out->edge[out->ne++]=(HeldEdge){(uint32_t)parent,(uint32_t)child,(uint8_t)vx.v[v].said};}
            }
            if(parent!=SIZE_MAX && !file && (grew || out->node[parent].keys!=before))
                lp_idmap_put(next,&out->node[parent].id,NULL);
        }
        holds_free(h,nh);lp_idmap_free(front);front=next;out->levels++;
    }
    lp_vec_free(&vx);lp_idmap_free(front);lp_idmap_free(at);lp_strmap_free(pairs);
}
void held_up_free(HeldUp *u){free(u->node);free(u->edge);memset(u,0,sizeof *u);}
