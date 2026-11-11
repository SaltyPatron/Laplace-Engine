/* Selected text and parts share an append-only Native node/child arena.
 * Each new frontier is fetched once; the walk back to text is Native's
 * (lp_realize): order, runs, what is no part of text, and the cycle check. */
#include "engine.h"
#include "laplace/highway.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {lp_id id;size_t first,nv;uint8_t state;} Ent;
struct Reader {PGconn *pg;Ent *e;size_t n,cap,fetched;lp_idmap *m;lp_vec(lp_realize_child) children;uint64_t trips;const lp_highway *highway;lp_text *text;lp_realizer *walk;};
Reader *reader_new(PGconn *pg){Reader *r=calloc(1,sizeof *r);r->pg=pg;r->highway=lp_highway_map(NULL);r->walk=lp_realizer_new();return r;}
void reader_free(Reader *r){if(!r)return;free(r->e);lp_vec_free(&r->children);lp_idmap_free(r->m);lp_text_free(r->text);lp_realizer_free(r->walk);free(r);}
uint64_t reader_trips(const Reader *r){return r->trips;}
static Ent *ent(Reader *r,const lp_id *id,int add){
    if(!add){int64_t i=lp_idmap_find(r->m,id);return i<0?NULL:r->e+i;}
    if(!r->m)r->m=lp_idmap_new();bool fresh;size_t i=lp_idmap_put(r->m,id,&fresh);if(!fresh)return r->e+i;
    lp_reserve((void **)&r->e,&r->cap,r->n+1,sizeof *r->e);
    Ent *x=r->e+r->n++;*x=(Ent){.id=*id};return x;
}
/* The immutable Highway stores the canonical identifier components, including
 * structured tuples. Native decomposes those bytes into this reader's shared
 * arena. Their content is available even when no database row exists. This
 * never supplies source assertions, occurrence annotations or standing. */
static lp_ref cached_compose(void *context,const lp_ref *children,uint32_t count,uint8_t tier){
    Reader *reader=context;lp_ref value=lp_ref_compose(children,count,tier);
    Ent *node=ent(reader,&value.id,1);
    if(node->state==1)return value;
    node->first=reader->children.n;node->nv=count;node->state=1;
    for(uint32_t i=0;i<count;i++)lp_push(&reader->children,((lp_realize_child){children[i].id,1,0}));
    return value;
}
static int cached_content(Reader *reader,const lp_id *id){
    if(!lp_highway_contains(reader->highway,id))return 0;
    if(!reader->text)reader->text=lp_text_new(T0);
    if(!reader->text){fputs("reader: cannot open Native text context\n",stderr);exit(1);}
    lp_ref root;
    if(!lp_highway_decompose(reader->highway,reader->text,id,cached_compose,reader,&root,NULL,0,NULL)){
        fputs("reader: Highway components disagree with canonical content\n",stderr);exit(1);
    }
    return 1;
}
/* Queued once; its content comes from the Highway or the database at the next fetch, so the arena a walk reads
 * never grows under it. */
void reader_want(Reader *r,const lp_id *id){
    if(lp_tier0_codepoint(T0,id)>=0)return;
    ent(r,id,1);
}
static size_t fetch(Reader *r){
    size_t begin=r->fetched,end=r->n;if(begin==end)return 0;r->fetched=end;
    lp_vec(lp_id) ids={0};Args args={0};
    for(size_t at=begin;at<end;){
        size_t amount=end-at;if(amount>DB_ID_WINDOW)amount=DB_ID_WINDOW;
        ids.n=0;
        for(size_t i=at;i<at+amount;i++){
            if(r->e[i].state || cached_content(r,&r->e[i].id))continue;
            lp_push(&ids,r->e[i].id);r->e[i].state=2;
        }
        if(!ids.n){at+=amount;continue;}
        args_reset(&args);arg_ids(&args,ids.v,ids.n);
        PGresult *rows=ask(r->pg,"SELECT entity,path FROM @extschema@.laplace_paths($1::@extschema@.blake3[])",&args);r->trips++;
        for(int row=0;row<PQntuples(rows);row++){
            Ent *node=ent(r,col_id(rows,row,0),0);if(!node || node->state==1)continue;
            lp_path path=col_path(rows,row,1);node->first=r->children.n;node->nv=path.n;node->state=1;
            if(path.n>SIZE_MAX-r->children.n){fputs("reader: child arena overflow\n",stderr);exit(1);}
            lp_vec_reserve(&r->children,r->children.n+path.n);
            for(size_t i=0;i<path.n;i++)
                lp_push(&r->children,((lp_realize_child){lp_path_id(path,i),lp_path_run(path,i),lp_path_said(path,i)}));
        }
        PQclear(rows);at+=amount;
    }
    args_free(&args);lp_vec_free(&ids);return end-begin;
}
/* One walk's output: bytes up to the limit, and the nodes it found unread (at most limit of them queued). */
typedef struct {Reader *r;char *b;size_t n,cap,limit,wants;} Out;
static int read_node(void *context,const lp_id *id,lp_realize_node *node){
    Out *out=context;Reader *r=out->r;Ent *x=ent(r,id,0);
    if(!x){
        if(out->wants>=out->limit)return LP_REALIZE_ABSENT;
        ent(r,id,1);out->wants++;return LP_REALIZE_PENDING;
    }
    if(x->state==0){out->wants++;return LP_REALIZE_PENDING;}
    if(x->state!=1)return LP_REALIZE_ABSENT;
    node->child=x->nv?r->children.v+x->first:NULL;node->n=x->nv;node->flags=0;
    return LP_REALIZE_KNOWN;
}
static int put(void *context,const uint8_t *bytes,size_t n){
    Out *out=context;
    lp_reserve((void **)&out->b,&out->cap,out->n+n,1);memcpy(out->b+out->n,bytes,n);out->n+=n;
    return out->n>=out->limit;
}
static void expand(Reader *r,const lp_id *id,Out *out){
    lp_realize_result result;out->r=r;
    if(!lp_realize(r->walk,T0,id,LP_REALIZE_TEXT,read_node,put,out,&result)){
        if(errno==ELOOP){char hex[33];lp_id_hex(&result.cycle,hex);fprintf(stderr,"reader: cyclic composition at %s\n",hex);}
        else fputs("reader: cannot realize text\n",stderr);
        exit(1);
    }
}
void reader_prefetch(Reader *r,const lp_id *ids,size_t count,size_t limit){
    if(!count)return;
    Out out={.limit=limit?limit:SIZE_MAX,.cap=64};out.b=malloc(out.cap);
    for(size_t i=0;i<count;i++)reader_want(r,ids+i);
    for(;;){
        fetch(r);bool pending=false;
        for(size_t i=0;i<count;i++){
            out.n=out.wants=0;expand(r,ids+i,&out);pending|=out.wants!=0;
        }
        if(!pending)break;
    }
    free(out.b);
}
char *reader_text(Reader *r,const lp_id *id,size_t limit){
    Out out={0};out.limit=limit?limit:SIZE_MAX;out.cap=64;out.b=malloc(out.cap);reader_want(r,id);
    for(;;){fetch(r);out.n=out.wants=0;expand(r,id,&out);if(!out.wants)break;}
    Ent *x=ent(r,id,0);
    if(!out.n && x && x->state==2){free(out.b);char hex[33];lp_id_hex(id,hex);char *text=malloc(48);snprintf(text,48,"{%s}",hex);return text;}
    if(out.n>=out.limit){
        size_t n=out.n;if(n>out.limit){n=out.limit;while(n>0 && ((uint8_t)out.b[n]&0xC0)==0x80)n--;}
        out.b=xrealloc(out.b,n+8);memcpy(out.b+n,"\xE2\x80\xA6",4);return out.b;
    }
    out.b=xrealloc(out.b,out.n+1);out.b[out.n]=0;return out.b;
}
/* What a record says, its claims and the voices that say them, is no part of its content: left out, as lp_realize
 * leaves them out of its text. */
size_t reader_parts(Reader *r,const lp_id *id,lp_id *out,size_t cap){
    if(!cap || lp_tier0_codepoint(T0,id)>=0)return 0;
    reader_want(r,id);Ent *x=ent(r,id,0);if(x && !x->state)fetch(r);x=ent(r,id,0);if(!x || x->state!=1)return 0;
    size_t n=0;
    for(size_t i=0;i<x->nv && n<cap;i++){
        lp_realize_child *child=r->children.v+x->first+i;
        if(child->said==LP_SAID_CLAIM || child->said==LP_SAID_VOICE)continue;
        size_t amount=child->run;
        if(amount>cap-n)amount=cap-n;
        for(size_t j=0;j<amount;j++)out[n++]=child->id;
    }
    return n;
}
