/* Selected text and parts share an append-only Native node/child arena.
 * Each new frontier is fetched once; ordered occurrences use an explicit stack. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {lp_id id;uint32_t run;} ReadChild;
typedef struct {lp_id id;size_t first,nv;uint8_t state,active;} Ent;
struct Reader {PGconn *pg;Ent *e;size_t n,cap,fetched;lp_idmap *m;lp_vec(ReadChild) children;uint64_t trips;};
Reader *reader_new(PGconn *pg){Reader *r=calloc(1,sizeof *r);r->pg=pg;return r;}
void reader_free(Reader *r){if(!r)return;free(r->e);lp_vec_free(&r->children);lp_idmap_free(r->m);free(r);}
uint64_t reader_trips(const Reader *r){return r->trips;}
static Ent *ent(Reader *r,const lp_id *id,int add){
    if(!add){int64_t i=lp_idmap_find(r->m,id);return i<0?NULL:r->e+i;}
    if(!r->m)r->m=lp_idmap_new();bool fresh;size_t i=lp_idmap_put(r->m,id,&fresh);if(!fresh)return r->e+i;
    lp_reserve((void **)&r->e,&r->cap,r->n+1,sizeof *r->e);
    Ent *x=r->e+r->n++;*x=(Ent){.id=*id};return x;
}
void reader_want(Reader *r,const lp_id *id){if(lp_tier0_codepoint(T0,id)<0)ent(r,id,1);}
static size_t fetch(Reader *r){
    size_t begin=r->fetched,end=r->n;if(begin==end)return 0;r->fetched=end;
    lp_vec(lp_id) ids={0};Args args={0};
    for(size_t at=begin;at<end;){
        size_t amount=end-at;if(amount>DB_ID_WINDOW)amount=DB_ID_WINDOW;
        ids.n=0;
        for(size_t i=at;i<at+amount;i++){lp_push(&ids,r->e[i].id);r->e[i].state=2;}
        args_reset(&args);arg_ids(&args,ids.v,ids.n);
        PGresult *rows=ask(r->pg,"SELECT entity,path FROM @extschema@.laplace_paths($1::@extschema@.blake3[])",&args);r->trips++;
        for(int row=0;row<PQntuples(rows);row++){
            Ent *node=ent(r,col_id(rows,row,0),0);if(!node || node->state==1)continue;
            lp_path path=col_path(rows,row,1);node->first=r->children.n;node->nv=path.n;node->state=1;
            if(path.n>SIZE_MAX-r->children.n){fputs("reader: child arena overflow\n",stderr);exit(1);}
            lp_vec_reserve(&r->children,r->children.n+path.n);
            for(size_t i=0;i<path.n;i++){
                double m;memcpy(&m,path.v+i*32+24,8);uint32_t said=lp_m_said(m);
                uint32_t run=(said==LP_SAID_CLAIM || said==LP_SAID_VOICE)?0:lp_path_run(path,i);
                lp_push(&r->children,((ReadChild){lp_path_id(path,i),run}));
            }
        }
        PQclear(rows);at+=amount;
    }
    args_free(&args);lp_vec_free(&ids);return end-begin;
}
typedef struct {char *b;size_t n,cap,limit,wants;} Out;
typedef struct {size_t node,child;uint32_t repeat;} ReadFrame;
static void enter(Reader *reader,size_t index){
    Ent *node=reader->e+index;
    if(node->active){char hex[33];lp_id_hex(&node->id,hex);fprintf(stderr,"reader: cyclic composition at %s\n",hex);exit(1);}
    node->active=1;
}
static void scalar(Out *out,uint32_t cp){
    lp_reserve((void **)&out->b,&out->cap,out->n+4,1);out->n+=lp_utf8_put(cp,(uint8_t *)out->b+out->n);
}
static void expand(Reader *r,const lp_id *id,Out *out){
    int64_t cp=lp_tier0_codepoint(T0,id);if(cp>=0){scalar(out,(uint32_t)cp);return;}
    Ent *root=ent(r,id,0);
    if(!root){if(out->wants<out->limit){ent(r,id,1);out->wants++;}return;}
    if(root->state==0){out->wants++;return;}if(root->state!=1)return;
    lp_vec(ReadFrame) stack={0};size_t first=(size_t)(root-r->e);enter(r,first);lp_push(&stack,((ReadFrame){.node=first}));
    while(stack.n && out->n<out->limit){
        ReadFrame *frame=stack.v+stack.n-1;Ent *node=r->e+frame->node;
        if(!frame->repeat && frame->child<node->nv){
            ReadChild *child=r->children.v+node->first+frame->child;frame->child++;frame->repeat=child->run;
        }
        if(frame->repeat){
            lp_id child=r->children.v[node->first+frame->child-1].id;frame->repeat--;
            cp=lp_tier0_codepoint(T0,&child);if(cp>=0){scalar(out,(uint32_t)cp);continue;}
            Ent *next=ent(r,&child,0);
            if(!next){if(out->wants<out->limit){ent(r,&child,1);out->wants++;}frame->repeat=0;continue;}
            if(next->state!=1){if(next->state==0)out->wants++;frame->repeat=0;continue;}
            size_t index=(size_t)(next-r->e);enter(r,index);lp_push(&stack,((ReadFrame){.node=index}));continue;
        }
        if(frame->child<node->nv)continue;
        node->active=0;stack.n--;
    }
    /* A display byte limit can stop inside a shared branch. Clear active-path
     * marks so another root or a later display can safely reuse the same DAG. */
    for(size_t i=0;i<stack.n;i++)r->e[stack.v[i].node].active=0;
    lp_vec_free(&stack);
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
size_t reader_parts(Reader *r,const lp_id *id,lp_id *out,size_t cap){
    if(!cap || lp_tier0_codepoint(T0,id)>=0)return 0;
    Ent *x=ent(r,id,1);if(!x->state)fetch(r);x=ent(r,id,0);if(!x || x->state!=1)return 0;
    size_t n=0;
    for(size_t i=0;i<x->nv && n<cap;i++){
        ReadChild *child=r->children.v+x->first+i;size_t amount=child->run;
        if(amount>cap-n)amount=cap-n;
        for(size_t j=0;j<amount;j++)out[n++]=child->id;
    }
    return n;
}
