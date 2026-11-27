/* The client composes and prunes; PostgreSQL merges whole COPY windows.
 * Structural and observation windows commit privately as they flow. Only
 * canonical witness registration and authoritative standings share the final
 * publication transaction; parsing never retains consensus bucket locks. */
#include "db_internal.h"
#include "chess_cache.h"
typedef struct { Node **item; size_t n,cap; } Nodes;
static void add_node(Nodes *v,Node *node){
    if(v->n==v->cap){v->cap=v->cap?v->cap*2:256;v->item=xrealloc(v->item,v->cap*sizeof *v->item);}
    v->item[v->n++]=node;
}
static void known_tree(Node *node){
    Nodes todo={0}; add_node(&todo,node);
    while(todo.n){
        Node *x=todo.item[--todo.n]; x->keep=2;
        for(uint32_t i=0;i<x->nv;i++){
            Node *child=table_find(&VTX[x->voff+i].id);
            if(child && child->keep!=2){child->keep=2;add_node(&todo,child);}
        }
    }
    free(todo.item);
}
static void queue_node(Nodes *frontier,Node *node){
    if(node && !node->keep){node->keep=3;add_node(&frontier[node->tier],node);}
}
static void queue_root(Nodes *frontier,const lp_id *id){
    queue_node(frontier,table_find(id));
}
static void prune(File *files,int nfiles,LoadStats *st){
    /* Tiers are uint8_t. Queue a selected node once, in its actual tier,
     * instead of scanning the complete working table at every altitude. */
    Nodes frontier[UINT8_MAX+1]={0};
    TABLE_EACH(x) {
        /* Tier-0 is the database realizer's storage-free leaf floor. Highway
         * membership supplies an identity/coordinate, not proof that its path
         * is readable from PostgreSQL. Persist its canonical logical closure
         * on first admission; only a recorded trunk can prune that closure.
         * A chess perf-cache record is likewise no proof by membership: it is
         * known here only when this load's database was found to hold it. */
        x->keep=lp_idmap_find(writer.known,&x->id)>=0 || lp_tier0_contains(T0,&x->id) || chess_cache_admitted(&x->id) ? 2 : 0;
    }
    TABLE_EACH(x) if(x->keep==2) known_tree(x);
    TABLE_EACH(x) if(x->pin) queue_node(frontier,x);
    for(int i=0;i<nfiles;i++){
        File *f=&files[i]; if(f->known || f->skipped) continue;
        if(f->has_file) queue_root(frontier,&f->file.id); else {
            queue_root(frontier,&f->trunk.id);
            for(uint64_t j=0;j<f->nsaid;j++) queue_root(frontier,&f->said[j].id);
        }
        if(f->has_lineage) queue_root(frontier,&f->lineage.id);
    }
    if(load_whole) TABLE_EACH(x) queue_node(frontier,x);
    for(int tier=UINT8_MAX;tier>0;tier--){
        Nodes *candidates=&frontier[tier];
        while(candidates->n){
        /* Logical paths already bypass Native's binary hash spines. Explicit
         * record-block spines are traversed immediately, never queried. New
         * same-tier children append to this queue and join the same pass. */
        size_t count=0;
        for(size_t i=0;i<candidates->n;i++){
            Node *x=candidates->item[i];if(x->keep!=3)continue;
            if(x->spine && !x->kind){
                x->keep=1;st->new_nodes++;
                for(uint32_t j=0;j<x->nv;j++) queue_root(frontier,&VTX[x->voff+j].id);
            }else candidates->item[count++]=x;
        }
        candidates->n=count;
        if(count){
            lp_id *ids=malloc(count*sizeof *ids);
            for(size_t i=0;i<count;i++) ids[i]=candidates->item[i]->id;
            uint8_t *hit=recorded(writer.copy,writer.ncopy,ids,(uint64_t)count);
            st->rounds++;st->checked+=(uint64_t)count;
            for(size_t i=0;i<count;i++){
                Node *x=candidates->item[i]; if(x->keep==2) continue;
                if(hit[i]){known_tree(x);st->found++;}
                else{x->keep=1;st->new_nodes++;for(uint32_t j=0;j<x->nv;j++)queue_root(frontier,&VTX[x->voff+j].id);}
            }
            free(hit);free(ids);
        }
        /* Saturated tiers may have same-tier logical children as well. Keep
         * their frontier rather than losing it after the parent's probe. */
        size_t next=candidates->n-count;
        if(next)memmove(candidates->item,candidates->item+count,next*sizeof *candidates->item);
        candidates->n=next;
        }
    }
    for(unsigned tier=0;tier<=UINT8_MAX;tier++)free(frontier[tier].item);
}

typedef struct { Node *node; uint64_t hilbert; lp_mask lexical; uint32_t wave; uint16_t lane; uint8_t annotations; } Row;
/* A dependency edge is about the newly admitted in-memory DAG, not domain tier.
 * Recorded descendants and perf-cache leaves need no publication dependency.
 * Repeated path vertices are visited as stored; their runs are never expanded. */
static int publication_child(void *context,size_t parent,size_t *cursor,size_t *out,size_t *distance){
    unsigned lanes=*(const unsigned *)context;
    Node *node=NODE+parent;
    if(!node->live || node->keep!=1)return 1;
    while(*cursor<node->nv){
        const Vtx *vertex=VTX+node->voff+(*cursor)++;
        Node *child=table_find(&vertex->id);
        if(child && child->keep==1){
            *out=(size_t)(child-NODE);
            *distance=((unsigned)node->id.b[0]*lanes/256u != (unsigned)child->id.b[0]*lanes/256u);
            return 0;
        }
    }
    return 1;
}
static int path_annotations(const Node *node){
    for(uint32_t i=0;i<node->nv;i++)if(VTX[node->voff+i].spare)return 1;
    return 0;
}
/* A tuple containing an explicit frozen lexical class carries that class.
 * This caches its own constituents, not a claim's inferred subject standing.
 * Conflicting classes have no single enum summary; retain the full path. */
static lp_mask tuple_mask(const Node *node){
    lp_mask mask=0,ambiguous=0;
    if (!HW || !(node->kind&(1u<<LP_KIND_TUPLE))) return 0;
    for (uint32_t i=0;i<node->nv;i++) {
        int32_t slot; const lp_bank *bank=lp_highway_bank_of(HW,&VTX[node->voff+i].id,&slot);
        if (!bank || strcmp(bank->carrier,"entity") || slot<0) continue;
        lp_mask field=lp_mask_field(bank);
        if (ambiguous&field) continue;
        if (!lp_mask_add_id(HW,&mask,&VTX[node->voff+i].id)) {
            mask&=~field; ambiguous|=field;
        }
    }
    return mask;
}
static int hilbert_order(const void *a,const void *b){
    const Row *x=a,*y=b;
    if(x->wave!=y->wave)return x->wave<y->wave?-1:1;
    if(x->lane!=y->lane)return x->lane<y->lane?-1:1;
    return x->hilbert<y->hilbert?-1:x->hilbert>y->hilbert?1:memcmp(x->node->id.b,y->node->id.b,16);
}
/* Rejected COPY input is retained. Identify the actual violated invariant
 * without weakening validation or dumping the batch/content into the log.
 * This set query runs only after a failed merge, outside its rolled-back
 * statement; it never changes either the caller's staging or public rows. */
static void coordinate_failure_chain(PGconn *pg,const Node *node,unsigned depth){
    char id[33];id_text(&node->id,id);
    fprintf(stderr,"    coordinate trace depth=%u id=%s tier=%u parts=%u vertices=%u\n",depth,id,node->tier,node->len,node->nv);
                lp_id *ids=malloc((size_t)node->nv*sizeof *ids);
                if(ids){
                    for(uint32_t i=0;i<node->nv;i++)ids[i]=VTX[node->voff+i].id;
                    Args input={0};arg_ids(&input,ids,node->nv);free(ids);
                    PGresult *children=db_call(pg,LP_SQL_ENTITY_COORDS,&input);
                    const Node *next=NULL;unsigned changed=0;lp_coord_sum local_sum={{0},0},recorded_sum={{0},0};
                    for(int i=0;i<PQntuples(children);i++){
                        uint64_t ordinal=(uint64_t)col_i64(children,i,0);if(!ordinal || ordinal>node->nv)continue;
                        int known=0;Ref local=ref_of_id(col_id(children,i,1),&known);lp_coord recorded;
                        if(!known || PQgetisnull(children,i,3) || PQgetisnull(children,i,4) || PQgetisnull(children,i,5) || PQgetisnull(children,i,6))continue;
                        int valid_pt=1;
                        for(int d=0;d<4;d++){
                            double v=col_f64(children,i,3+d);
                            if(!isfinite(v) || fabs(v)>1.0){valid_pt=0;break;}
                            recorded.m[d]=(int64_t)llround(v*LP_FIXED_ONE);
                        }
                        if(!valid_pt)continue;
                        uint64_t run=VRUN(VTX[node->voff+ordinal-1].m);
                        for(int d=0;d<4;d++){local_sum.s[d]+=(__int128)local.c.m[d]*run;recorded_sum.s[d]+=(__int128)recorded.m[d]*run;}
                        local_sum.n+=run;recorded_sum.n+=run;
                        if(memcmp(local.c.m,recorded.m,sizeof recorded.m) && changed++<8){
                            if(!next)next=table_find(&local.id);
                            char child[33];id_text(&local.id,child);
                            fprintf(stderr,"    child coordinate conflict id=%s local_tier=%u local=(%.17g %.17g %.17g %.17g) recorded=(%.17g %.17g %.17g %.17g)\n",child,local.tier,
                                local.c.m[0]/LP_FIXED_ONE,local.c.m[1]/LP_FIXED_ONE,local.c.m[2]/LP_FIXED_ONE,local.c.m[3]/LP_FIXED_ONE,
                                recorded.m[0]/LP_FIXED_ONE,recorded.m[1]/LP_FIXED_ONE,recorded.m[2]/LP_FIXED_ONE,recorded.m[3]/LP_FIXED_ONE);
                        }
                    }
                    lp_coord local,recorded;lp_coord_mean(&local_sum,&local);lp_coord_mean(&recorded_sum,&recorded);
                    fprintf(stderr,"    child coordinate conflicts=%u measured_runs=%llu local_mean=(%.17g %.17g %.17g %.17g) recorded_mean=(%.17g %.17g %.17g %.17g)\n",changed,(unsigned long long)local_sum.n,
                        local.m[0]/LP_FIXED_ONE,local.m[1]/LP_FIXED_ONE,local.m[2]/LP_FIXED_ONE,local.m[3]/LP_FIXED_ONE,
                        recorded.m[0]/LP_FIXED_ONE,recorded.m[1]/LP_FIXED_ONE,recorded.m[2]/LP_FIXED_ONE,recorded.m[3]/LP_FIXED_ONE);
                    PQclear(children);args_free(&input);
                    if(next && depth<12)coordinate_failure_chain(pg,next,depth+1);
                }
}
static void merge_failure(PGconn *pg, const char *entities, const char *paths, const char *error){
    fprintf(stderr,"stage merge: %s",error);
    Args args={0};arg_text(&args,entities);arg_text(&args,paths);
    char err[512];PGresult *detail=db_call_try(pg,LP_SQL_MERGE_DIAGNOSTICS,&args,err,sizeof err);
    args_free(&args);
    if(detail){
        for(int row=0;row<PQntuples(detail);row++){
            const char *reason=PQgetisnull(detail,row,0)?"":col_str(detail,row,0);
            const char *msg=PQgetisnull(detail,row,2)?"":col_str(detail,row,2);
            lp_id id={0};char idhex[33]="";
            int has_id=!PQgetisnull(detail,row,1) && PQgetlength(detail,row,1)==16;
            if(has_id){id=*col_id(detail,row,1);id_text(&id,idhex);}
            fprintf(stderr,"  stage rejection: %s id=%s %s\n",reason,idhex,msg);
            if(!has_id)continue;
            const Node *node=table_find(&id);if(!node)continue;
            fprintf(stderr,"    staged decomposition: tier=%u parts=%u vertices=%u\n",node->tier,node->len,node->nv);
            for(uint32_t i=0;i<node->nv && i<8;i++){
                const Vtx *v=&VTX[node->voff+i];char child[33];id_text(&v->id,child);
                fprintf(stderr,"    child=%s run=%u\n",child,VRUN(v->m));
            }
            if(!strcmp(reason,"entity identity conflict") && node->nv)coordinate_failure_chain(pg,node,0);
            if(!PQgetisnull(detail,row,3)){
                size_t len=(size_t)PQgetlength(detail,row,3);
                if(len>161)len=161;
                char hex[323];lp_hex((const uint8_t *)PQgetvalue(detail,row,3),len,hex);
                fprintf(stderr,"    recorded path prefix=%s\n",hex);
            }
        }
        PQclear(detail);
    }else fprintf(stderr,"stage rejection diagnostic: %s\n",err);
}
typedef struct { lp_id *ids;uint64_t *runs;uint32_t *spare;uint8_t *bytes;size_t capacity,byte_capacity; } PathEncoding;
static size_t encode_node_path(const Node *x,PathEncoding *path){
    if(x->nv>path->capacity){path->capacity=x->nv;path->ids=xrealloc(path->ids,path->capacity*sizeof *path->ids);
        path->runs=xrealloc(path->runs,path->capacity*sizeof *path->runs);path->spare=xrealloc(path->spare,path->capacity*sizeof *path->spare);}
    for(uint32_t j=0;j<x->nv;j++){const Vtx *v=&VTX[x->voff+j];path->ids[j]=v->id;path->runs[j]=v->m;path->spare[j]=v->spare;}
    size_t size=lp_ewkb_runs_spare(path->ids,path->runs,path->spare,x->nv,NULL,0);
    if(!size){
        char id[33];id_text(&x->id,id);fprintf(stderr,"invalid native path before COPY: entity=%s vertices=%u\n",id,x->nv);
        for(uint32_t j=0;j<x->nv;j++)if(path->runs[j]>(UINT64_C(1)<<53)-1 || (path->spare[j]>>LP_SPARE_BITS))
            fprintf(stderr,"  vertex=%u M=%llu spare=%u\n",j,(unsigned long long)path->runs[j],path->spare[j]);
        exit(1);
    }
    if(size>path->byte_capacity){path->byte_capacity=size;path->bytes=xrealloc(path->bytes,size);}
    if(lp_ewkb_runs_spare(path->ids,path->runs,path->spare,x->nv,path->bytes,size)!=size){fputs("native path size changed during COPY\n",stderr);exit(1);}
    return size;
}
static void copy_rows(PGconn *pg,int lane,const Row *rows,size_t lo,size_t hi){
    lp_copy copy={0}; uint8_t geometry[128];
    db_copy_open_stage_pair(&copy,pg,writer.entity[lane],writer.path[lane]);
    for(size_t i=lo;i<hi;i++){
        const Node *x=rows[i].node;uint8_t present=x->keep==2;
        lp_copy_row(&copy,6);lp_copy_id(&copy,&x->id);lp_copy_i16(&copy,x->tier);
        if(present){lp_copy_null(&copy);lp_copy_null(&copy);}
        else{
            double point[4];for(int axis=0;axis<4;axis++)point[axis]=(double)x->m[axis]/LP_FIXED_ONE;
            size_t size=lp_ewkb_point4(point,geometry,sizeof geometry);
            lp_copy_field(&copy,geometry,(uint32_t)size);
            lp_copy_i64(&copy,(int64_t)(rows[i].hilbert^UINT64_C(0x8000000000000000)));
        }
        lp_copy_i64(&copy,(int64_t)rows[i].lexical);lp_copy_field(&copy,&present,1);
    }
    copy_next(&copy);
    PathEncoding path={0};
    for(size_t i=lo;i<hi;i++){
        const Node *x=rows[i].node;uint8_t present=x->keep==2;
        lp_copy_row(&copy,5);lp_copy_id(&copy,&x->id);lp_copy_i16(&copy,x->tier);
        if(present && !rows[i].annotations)lp_copy_null(&copy);
        else{size_t size=encode_node_path(x,&path);lp_copy_field(&copy,path.bytes,(uint32_t)size);}
        lp_copy_i64(&copy,(int64_t)x->kind);lp_copy_field(&copy,&present,1);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);
    free(path.ids);free(path.runs);free(path.spare);free(path.bytes);
}
static void maintenance_partitions(IndexPartitions *out,const PGresult *q){
    size_t bytes=(size_t)PQgetlength(q,0,3);const uint8_t *p=(const uint8_t *)PQgetvalue(q,0,3);
    if(PQgetisnull(q,0,3) || bytes<12 || lp_be(p+4,4) || lp_be(p+8,4)!=26)goto invalid;
    uint64_t dimensions=lp_be(p,4);
    if(!dimensions && bytes==12)return;
    if(dimensions!=1 || bytes<20 || lp_be(p+16,4)!=1)goto invalid;
    uint64_t n=lp_be(p+12,4);
    if(n!=(bytes-20)/8 || (bytes-20)%8)goto invalid;
    for(size_t i=0;i<(size_t)n;i++){
        const uint8_t *element=p+20+i*8;
        if(lp_be(element,4)!=4)goto invalid;
        uint32_t oid=(uint32_t)lp_be(element+4,4);if(!oid)goto invalid;
        lp_push(out,oid);
    }
    return;
invalid:
    fputs("publication: invalid Native index-maintenance partition array\n",stderr);exit(1);
}
static int partition_order(const void *a,const void *b){
    uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;return (x>y)-(x<y);
}
static void deduplicate_partitions(IndexPartitions *partitions){
    if(partitions->n<=1)return;
    qsort(partitions->v,partitions->n,sizeof *partitions->v,partition_order);
    size_t n=0;
    for(size_t i=0;i<partitions->n;i++)
        if(!i || partitions->v[i]!=partitions->v[i-1])partitions->v[n++]=partitions->v[i];
    partitions->n=n;
}
/* Clean each touched physicality partition's GIN pending list ONCE at the end
 * of the load, with disjoint partition slices across at most n_unique backends
 * (<= 9 tier partitions, zero duplicate OIDs across connections). Intermediate
 * windows rely on PostgreSQL's gin_pending_list_limit = 32 MB per partition. */
static void flush_touched_partitions(LoadStats *stats){
    deduplicate_partitions(&writer.touched_partitions);
    size_t total=writer.touched_partitions.n;
    if(!total || writer.ncopy<=0)return;
    unsigned workers=total<(size_t)writer.ncopy?(unsigned)total:(unsigned)writer.ncopy;
    double phase=now();
    #pragma omp parallel for num_threads(workers) schedule(static,1)
    for(unsigned w=0;w<workers;w++){
        size_t lo=(total*w)/workers, hi=(total*(w+1u))/workers;
        if(lo<hi){
            Args input={0};arg_oid_array(&input,writer.touched_partitions.v+lo,hi-lo);
            db_exec_contract(writer.copy[w],LP_SQL_CLEAN_PARTITIONS,&input);
            args_free(&input);
        }
    }
    writer.touched_partitions.n=0;
    if(stats){double elapsed=now()-phase;stats->t_index+=elapsed;stats->t_publish+=elapsed;}
}
int load(const char *conninfo,int npg,File *files,int nfiles,LoadStats *st){
    start_loader(conninfo,npg);table_kinds();
    DbTraffic before=db_traffic();uint64_t tier_rounds=st->rounds;
    for(int i=0;i<nfiles;i++)for(uint64_t j=0;j<files[i].ev.n;j++){
        const Event *event=&files[i].ev.e[j];Node *node=table_find(&event->claim);
        if(node)node->kind|=(uint8_t)(1u<<(event->kind==EV_RECORD?LP_KIND_RECORD:node->len>=2?LP_KIND_CLAIM:LP_KIND_TUPLE));
    }
    TABLE_EACH(x) if(x->len<2)x->kind&=(uint8_t)~(1u<<LP_KIND_CLAIM);
    double begin=now();prune(files,nfiles,st);st->t_dedup+=now()-begin;begin=now();
    lp_vec(Row) selected={0};
    TABLE_EACH(x)if(x->keep==1 || x->keep==2){
        lp_mask lexical=tuple_mask(x);uint8_t annotations=x->keep==2?path_annotations(x):0;
        if(x->keep==2){
            if(!x->kind && !lexical && !annotations)continue;
            const Published *seen=lp_idmap_lookup(writer.known,&x->id);
            if(!annotations && seen && seen->metadata && seen->tier<=x->tier
                && (seen->kind & x->kind)==x->kind && (!lexical || seen->lexical==lexical))continue;
        }
        uint64_t hilbert=0;
        if(x->keep==1){lp_coord c;memcpy(c.m,x->m,sizeof c.m);hilbert=lp_hilbert4(&c);}
        lp_push(&selected,((Row){.node=x,.hilbert=hilbert,.lexical=lexical,.annotations=annotations}));
    }
    size_t count=selected.n;Row *rows=selected.v;
    double persistence_begin=begin;
    size_t complete=0,metadata=0,overlays=0;
    for(size_t i=0;i<count;i++)if(rows[i].node->keep==1)complete++;
        else{metadata++;overlays+=rows[i].annotations!=0;}
    fprintf(stderr,"batch transport: %zu complete payloads, %zu existing metadata IDs, %zu annotation overlays\n",complete,metadata,overlays);
    /* A lane needs a useful transport window. Reuse COPY's existing 4 MiB
     * grain instead of opening parallel applies for a handful of tiny rows.
     * This sizes concurrency only; it never excludes input or changes identity. */
    uint64_t payload=0;
    for(size_t i=0;i<count;i++){
        const Row *row=rows+i;
        payload+=row->node->keep==1 ? 176u+(uint64_t)32*row->node->nv
            : 114u+(row->annotations?9u+(uint64_t)32*row->node->nv:0);
    }
    /* LAPLACE_PUBLISH_GRAIN_BYTES sizes that window (as the record and working-set windows are sized); 4 MiB by default. */
    uint64_t grain=(uint64_t)1<<22;const char *grain_setting=getenv("LAPLACE_PUBLISH_GRAIN_BYTES");
    if(grain_setting && *grain_setting){char *end;unsigned long long g=strtoull(grain_setting,&end,10);if(!*end && g)grain=g;}
    uint64_t wanted=payload/grain;
    unsigned lanes=wanted<(uint64_t)writer.ncopy?(unsigned)wanted:(unsigned)writer.ncopy;
    if(!lanes)lanes=1;
    if(st->publish_lanes<lanes)st->publish_lanes=lanes;
    size_t nodes=(size_t)table_end();
    if(nodes>SIZE_MAX/sizeof(size_t)){fputs("publication graph exceeds addressable memory\n",stderr);exit(1);}
    size_t *levels=count?malloc(nodes*sizeof *levels):NULL;
    if(count && (!levels || !lp_dag_levels(nodes,publication_child,&lanes,levels))){
        fprintf(stderr,"invalid Native publication DAG: %s\n",strerror(errno));exit(1);
    }
    for(size_t i=0;i<count;i++){
        size_t depth=rows[i].node->keep==1?levels[rows[i].node-NODE]:1;
        if(depth>UINT32_MAX){fputs("publication wave exceeds addressable row metadata\n",stderr);exit(1);}
        rows[i].wave=(uint32_t)depth;
        /* Prefix ownership keeps concurrent writers on disjoint canonical
         * range leaves, while every lane's actual rows remain Hilbert ordered. */
        rows[i].lane=(uint16_t)((unsigned)rows[i].node->id.b[0]*lanes/256u);
    }
    free(levels);
    qsort(rows,count,sizeof *rows,hilbert_order);
    st->t_prepare+=now()-begin;
    size_t *bounds=malloc(((size_t)lanes+1)*sizeof *bounds);
    if(!bounds){perror("publication lane bounds");exit(1);}
    IndexPartitions *maintenance=calloc(lanes,sizeof *maintenance);
    if(!maintenance){perror("publication maintenance lanes");exit(1);}
    uint64_t waves=0;
    for(size_t lo=0;lo<count;){
        size_t hi=lo+1;while(hi<count && rows[hi].wave==rows[lo].wave)hi++;
        size_t at=lo;
        for(unsigned lane=0;lane<lanes;lane++){
            bounds[lane]=at;
            while(at<hi && rows[at].lane==(uint16_t)lane)at++;
        }
        bounds[lanes]=hi;
        double phase=now();
        #pragma omp parallel for num_threads(lanes) schedule(static,1)
        for(unsigned lane=0;lane<lanes;lane++)if(bounds[lane]<bounds[lane+1])
            copy_rows(writer.copy[lane],lane,rows,bounds[lane],bounds[lane+1]);
        st->t_transfer+=now()-phase;phase=now();
        uint64_t entities=0,paths=0;
        #pragma omp parallel for num_threads(lanes) schedule(static,1) reduction(+:entities,paths)
        for(unsigned lane=0;lane<lanes;lane++)if(bounds[lane]<bounds[lane+1]){
            Args ma={0};arg_text(&ma,writer.entity[lane]);arg_text(&ma,writer.path[lane]);
            if(writer.window_index)arg_bool(&ma,true);
            char err[512];
            PGresult *q=db_call_try(writer.copy[lane],
                writer.window_index?LP_SQL_MERGE_STAGE:LP_SQL_MERGE_STAGE_DEFAULT,&ma,err,sizeof err);
            args_free(&ma);
            if(!q){
                merge_failure(writer.copy[lane],writer.entity[lane],writer.path[lane],err);
                exit(1);
            }
            if(PQntuples(q)!=1 || PQnfields(q)!=(writer.window_index?4:3)
                || PQgetisnull(q,0,0) || PQgetisnull(q,0,1)){
                fputs("publication: invalid Native merge result\n",stderr);PQclear(q);exit(1);
            }
            entities+=(uint64_t)col_i64(q,0,0);
            paths+=(uint64_t)col_i64(q,0,1);
            if(writer.window_index)maintenance_partitions(maintenance+lane,q);
            PQclear(q);
        }
        /* A parent wave starts only after EVERY child wave transaction has
         * committed. A failed/cancelled lane can leave complete branches, never
         * a visible trunk whose missing subtree a later replay would prune. */
        st->ent_rows+=entities;st->phy_rows+=paths;
        st->t_publish+=now()-phase;waves++;lo=hi;
    }
    free(bounds);st->publish_waves+=waves;
    if(writer.window_index){
        for(unsigned lane=0;lane<lanes;lane++)
            for(size_t k=0;k<maintenance[lane].n;k++)
                lp_push(&writer.touched_partitions,maintenance[lane].v[k]);
        deduplicate_partitions(&writer.touched_partitions);
    }
    for(unsigned lane=0;lane<lanes;lane++)lp_vec_free(maintenance+lane);
    free(maintenance);
    for(size_t i=0;i<count;i++){
        const Row *row=rows+i;Published *seen=lp_idmap_get(writer.known,&row->node->id,NULL);
        if(!seen->metadata || row->node->tier<seen->tier)seen->tier=row->node->tier;
        if(row->lexical)seen->lexical=row->lexical;
        seen->kind|=row->node->kind;seen->metadata=1;
    }
    TABLE_EACH(x)if(x->keep==1 || x->keep==2){
        Published *seen=lp_idmap_get(writer.known,&x->id,NULL);
        if(!seen->metadata || x->tier<seen->tier)seen->tier=x->tier;
        lp_mask lexical=tuple_mask(x);
        if(lexical)seen->lexical=lexical;
        seen->kind|=x->kind;seen->metadata=1;
    }
    lp_vec_free(&selected);
    st->t_copy+=now()-persistence_begin;begin=now();flush_series(active_fold);st->t_sem+=now()-begin;
    DbTraffic after=db_traffic();
    fprintf(stderr,"batch database: %llu tier rounds, %llu SQL commands, %llu COPY streams; startup and source registration separate\n",
        (unsigned long long)(st->rounds-tier_rounds),(unsigned long long)(after.commands-before.commands),
        (unsigned long long)(after.copy_streams-before.copy_streams));
    if(!writer.from_ingest)return merge(conninfo,npg,st);
    return 0;
}

int load_register(const char *conninfo,int npg,const LoadWitness *roots,size_t count,const File *files,int nfiles){
    if(!count)return 0;start_loader(conninfo,npg);
    db_tx(writer.series,LP_SQL_TX_BEGIN_RC);
    if(!writer.registered)writer.registered=lp_idmap_new();
    if(!writer.new_witnesses)writer.new_witnesses=lp_idmap_new();
    lp_id *ids=malloc(count*sizeof *ids);
    if(!ids){perror("witness measurement roots");return 1;}
    for(size_t i=0;i<count;i++){ids[i]=roots[i].id;lp_idmap_put(writer.registered,ids+i,NULL);}
    Args existing_args={0};arg_ids(&existing_args,ids,count);free(ids);
    PGresult *existing=db_call(writer.series,LP_SQL_WITNESSES_EXISTING,&existing_args);
    lp_idmap *known_witnesses=lp_idmap_new();
    for(int i=0;i<PQntuples(existing);i++)lp_idmap_put(known_witnesses,col_id(existing,i,0),NULL);
    for(size_t i=0;i<count;i++)if(lp_idmap_find(known_witnesses,&roots[i].id)<0){
        lp_idmap_put(writer.new_witnesses,&roots[i].id,NULL);
        writer.witness_changed=1;
    }
    lp_idmap_free(known_witnesses);PQclear(existing);args_free(&existing_args);
    lp_copy copy={0};
    db_copy_open_stage_witnesses(&copy,writer.series,writer.witnesses);
    for(size_t i=0;i<count;i++){
        if(!isfinite(roots[i].trust) || roots[i].trust < -1 || roots[i].trust > 1){fprintf(stderr,"invalid witness trust\n");exit(1);}
        lp_copy_row(&copy,3);lp_copy_id(&copy,&roots[i].id);
        if(roots[i].has_lineage)lp_copy_id(&copy,&roots[i].lineage);else lp_copy_null(&copy);
        lp_copy_f64(&copy,roots[i].trust);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);
    if(count==1){
        /* Rewrite provisional source/model witness ID in writer.pending to the final trunk ID. */
        Args rw={0};arg_text(&rw,writer.pending);arg_id(&rw,&roots[0].id);
        db_exec_contract(writer.series,LP_SQL_STAGE_REWRITE_WITNESS,&rw);
        args_free(&rw);
    }
    if(writer.series_changed || writer.witness_changed || writer.counts_unknown){
        /* New assertions already supplied their exact claim IDs in each
         * committed window. Only pruned files need their stored testimony
         * restored; never walk the newly ingested corpus down to text again. */
        lp_idmap *reused=lp_idmap_new();
        for(int i=0;i<nfiles;i++)if(files[i].known && !files[i].skipped && files[i].has_file)
            lp_idmap_put(reused,&files[i].file.id,NULL);
        if(lp_idmap_count(reused)){
            /* a recorded file's testimony, restored into the delta and folded by the one accumulation (series_delta_apply) */
            int own=PQtransactionStatus(writer.series)==PQTRANS_IDLE;if(own)db_tx(writer.series,LP_SQL_TX_BEGIN);
            Args args={0};
            arg_ids(&args,lp_idmap_keys(reused),lp_idmap_count(reused));
            arg_text(&args,writer.witnesses);arg_text(&args,writer.delta);
            if(count==1)arg_id(&args,&roots[0].id);else arg_null(&args);
            db_exec_contract(writer.series,LP_SQL_STAGE_RESTORE_PRUNED,&args);
            args_free(&args);
            series_delta_apply();if(own)db_tx(writer.series,LP_SQL_TX_COMMIT);
            writer.series_changed=1;
        }
        lp_idmap_free(reused);
    }
    return 0;
}
int merge(const char *conninfo,int npg,LoadStats *stats){
    (void)conninfo;(void)npg;if(!writer.series)return 0;
    if(writer.window_index && writer.touched_partitions.n){
        if(PQtransactionStatus(writer.series)!=PQTRANS_IDLE)db_tx(writer.series,LP_SQL_TX_COMMIT);
        flush_touched_partitions(stats);
    }
    if(PQtransactionStatus(writer.series)==PQTRANS_IDLE)db_tx(writer.series,LP_SQL_TX_BEGIN_RC);
    uint64_t admitted=0;
    if(lp_idmap_count(writer.registered) && (writer.series_changed || writer.witness_changed || writer.counts_unknown)){
        int use_affected = (writer.series_changed || writer.witness_changed);
        size_t nnew = lp_idmap_count(writer.new_witnesses);
        const lp_id *new_ids = nnew ? lp_idmap_keys(writer.new_witnesses) : NULL;
        int lanes = (writer.bucket_publish && writer.ncopy > 1) ? (writer.ncopy < 16 ? writer.ncopy : 16) : 1;
        if(lanes > 1){
            /* Commit pending/affected visibility on writer.series so all parallel
             * backend lanes see the finalized stage tables and fold disjoint
             * claim.b[0]>>4 range partitions (attestation + consensus) concurrently. */
            if(PQtransactionStatus(writer.series)!=PQTRANS_IDLE)db_tx(writer.series,LP_SQL_TX_COMMIT);
            int lane_failed=0;
            #pragma omp parallel for num_threads(lanes) schedule(static,1) reduction(+:admitted) reduction(|:lane_failed)
            for(int lane=0;lane<lanes;lane++){
                int b_lo = (16 * lane) / lanes, b_hi = (16 * (lane + 1)) / lanes;
                int32_t mask = 0;
                for(int b=b_lo;b<b_hi;b++) mask |= (1 << b);
                if(!mask) continue;
                Args pa={0}; arg_text(&pa,writer.pending);
                if(use_affected) arg_text(&pa,writer.affected); else arg_null(&pa);
                if(nnew) arg_ids(&pa,new_ids,nnew); else arg_null(&pa);
                arg_i32(&pa,mask);
                PGresult *pub=db_call(writer.copy[lane],LP_SQL_STAGE_PUBLISH_ATTESTATIONS_BUCKETS,&pa);
                args_free(&pa);
                if(!pub || PQntuples(pub)!=1 || PQgetisnull(pub,0,0)){
                    fputs("attestation publication: invalid Native bucket result\n",stderr);
                    if(pub)PQclear(pub);
                    lane_failed=1;
                }else{
                    admitted+=(uint64_t)col_i64(pub,0,0);
                    PQclear(pub);
                }
            }
            if(lane_failed) return 1;
            db_tx(writer.series,LP_SQL_TX_BEGIN_RC);
        }else{
            Args pa={0};arg_text(&pa,writer.pending);
            if(use_affected)arg_text(&pa,writer.affected);else arg_null(&pa);
            if(nnew) arg_ids(&pa,new_ids,nnew); else arg_null(&pa);
            PGresult *pub=db_call(writer.series,LP_SQL_STAGE_PUBLISH_ATTESTATIONS,&pa);
            args_free(&pa);
            if(PQntuples(pub)!=1 || PQgetisnull(pub,0,0)){
                fputs("attestation publication: invalid Native result\n",stderr);PQclear(pub);return 1;
            }
            admitted=(uint64_t)col_i64(pub,0,0);
            PQclear(pub);
        }
    }
    size_t ndrop=(size_t)writer.ncopy*2u+4u;
    const char **drops=malloc(ndrop*sizeof *drops);
    if(!drops){perror("stage drop");return 1;}
    size_t dk=0;
    for(int lane=0;lane<writer.ncopy;lane++){drops[dk++]=writer.entity[lane];drops[dk++]=writer.path[lane];}
    drops[dk++]=writer.delta;drops[dk++]=writer.pending;drops[dk++]=writer.witnesses;drops[dk++]=writer.affected;
    if(db_stage_drop(writer.series,drops,ndrop)!=0){free(drops);return 1;}
    free(drops);
    db_tx(writer.series,LP_SQL_TX_COMMIT);
    if(stats){stats->attestations+=admitted;stats->attestations_measured=1;}
    PQfinish(writer.series);for(int i=0;i<writer.ncopy;i++)PQfinish(writer.copy[i]);
    free(writer.copy);free(writer.entity);free(writer.path);lp_idmap_free(writer.known);
    lp_idmap_free(writer.registered);lp_idmap_free(writer.new_witnesses);
    lp_vec_free(&writer.touched_partitions);writer=(Loader){0};return 0;
}
/* Single-connection interactive turn publisher: reuses an idle/read-ended PGconn without spawning extra worker connections. */
int lp_stage_publish_fast(PGconn *pg,Fold *f,const LoadWitness *roots,size_t nroots,File *files,int nfiles,LoadStats *st){
    if(!pg)return 1;
    if(!db_read_end(pg))return 1;
    if(PQtransactionStatus(pg)!=PQTRANS_IDLE){
        db_tx_try(pg,LP_SQL_TX_ROLLBACK,NULL,0);
        if(PQtransactionStatus(pg)!=PQTRANS_IDLE)return 1;
    }
    PGconn *lane_conn=pg;
    char ent_tbl[96],path_tbl[96];
    memset(&writer,0,sizeof writer);
    writer.from_ingest=1;
    writer.ncopy=1;
    writer.copy=&lane_conn;
    writer.series=pg;
    writer.known=lp_idmap_sized(sizeof(Published));
    db_exec_contract(pg,LP_SQL_ENSURE_STAGE,NULL);
    PGresult *xq=db_call(pg,LP_SQL_CURRENT_XACT,NULL);
    if(!xq || PQntuples(xq)!=1 || PQgetisnull(xq,0,0)){if(xq)PQclear(xq);lp_idmap_free(writer.known);writer=(Loader){0};return 1;}
    int64_t xid=col_i64(xq,0,0);PQclear(xq);
    snprintf(ent_tbl,sizeof ent_tbl,"stage.lp_entity_%lld_0",(long long)xid);
    snprintf(path_tbl,sizeof path_tbl,"stage.lp_path_%lld_0",(long long)xid);
    writer.entity=(char (*)[96])ent_tbl;
    writer.path=(char (*)[96])path_tbl;
    snprintf(writer.delta,sizeof writer.delta,"stage.lp_delta_%lld",(long long)xid);
    snprintf(writer.pending,sizeof writer.pending,"stage.lp_pending_%lld",(long long)xid);
    snprintf(writer.witnesses,sizeof writer.witnesses,"stage.lp_witness_%lld",(long long)xid);
    snprintf(writer.affected,sizeof writer.affected,"stage.lp_affected_%lld",(long long)xid);

    db_tx(pg,LP_SQL_TX_BEGIN);
    if(db_stage_create_pair(pg,ent_tbl,path_tbl)!=0 ||
       db_stage_create_series(pg,writer.delta,writer.pending,writer.witnesses,writer.affected)!=0){
        db_tx_try(pg,LP_SQL_TX_ROLLBACK,NULL,0);lp_idmap_free(writer.known);writer=(Loader){0};return 1;
    }
    const char *owned[6]={ent_tbl,path_tbl,writer.delta,writer.pending,writer.witnesses,writer.affected};
    Args ownership={0};arg_i64(&ownership,xid);arg_text_array(&ownership,owned,6);
    db_exec_contract(pg,LP_SQL_STAGE_OWN,&ownership);args_free(&ownership);
    db_tx(pg,LP_SQL_TX_COMMIT);

    table_kinds();
    for(int i=0;i<nfiles;i++)for(uint64_t j=0;j<files[i].ev.n;j++){
        const Event *event=&files[i].ev.e[j];Node *node=table_find(&event->claim);
        if(node)node->kind|=(uint8_t)(1u<<(event->kind==EV_RECORD?LP_KIND_RECORD:node->len>=2?LP_KIND_CLAIM:LP_KIND_TUPLE));
    }
    TABLE_EACH(x) if(x->len<2)x->kind&=(uint8_t)~(1u<<LP_KIND_CLAIM);
    LoadStats local={0};if(!st)st=&local;
    prune(files,nfiles,st);
    lp_vec(Row) selected={0};
    TABLE_EACH(x)if(x->keep==1 || x->keep==2){
        lp_mask lexical=tuple_mask(x);uint8_t annotations=x->keep==2?path_annotations(x):0;
        if(x->keep==2 && !x->kind && !lexical && !annotations)continue;
        uint64_t hilbert=0;
        if(x->keep==1){lp_coord c;memcpy(c.m,x->m,sizeof c.m);hilbert=lp_hilbert4(&c);}
        lp_push(&selected,((Row){.node=x,.hilbert=hilbert,.lexical=lexical,.annotations=annotations}));
    }
    size_t count=selected.n;Row *rows=selected.v;
    if(count){
        unsigned lanes=1;
        size_t nodes=(size_t)table_end();
        size_t *levels=malloc(nodes*sizeof *levels);
        if(!levels || !lp_dag_levels(nodes,publication_child,&lanes,levels)){
            free(levels);lp_vec_free(&selected);lp_idmap_free(writer.known);writer=(Loader){0};return 1;
        }
        for(size_t i=0;i<count;i++){
            size_t depth=rows[i].node->keep==1?levels[rows[i].node-NODE]:1;
            rows[i].wave=(uint32_t)depth;
            rows[i].lane=0;
        }
        free(levels);
        qsort(rows,count,sizeof *rows,hilbert_order);
        for(size_t lo=0;lo<count;){
            size_t hi=lo+1;while(hi<count && rows[hi].wave==rows[lo].wave)hi++;
            copy_rows(pg,0,rows,lo,hi);
            Args ma={0};arg_text(&ma,ent_tbl);arg_text(&ma,path_tbl);
            char err[512];PGresult *q=db_call_try(pg,LP_SQL_MERGE_STAGE_DEFAULT,&ma,err,sizeof err);args_free(&ma);
            if(!q){merge_failure(pg,ent_tbl,path_tbl,err);lp_vec_free(&selected);lp_idmap_free(writer.known);writer=(Loader){0};return 1;}
            st->ent_rows+=(uint64_t)col_i64(q,0,0);st->phy_rows+=(uint64_t)col_i64(q,0,1);
            PQclear(q);lo=hi;
        }
    }
    lp_vec_free(&selected);
    flush_series(f);

    uint64_t admitted=0;
    if(nroots>0 && roots){
        db_tx(pg,LP_SQL_TX_BEGIN_RC);
        writer.registered=lp_idmap_new();writer.new_witnesses=lp_idmap_new();
        lp_id *wids=malloc(nroots*sizeof *wids);
        for(size_t i=0;i<nroots;i++){wids[i]=roots[i].id;lp_idmap_put(writer.registered,wids+i,NULL);}
        Args ea={0};arg_ids(&ea,wids,nroots);free(wids);
        PGresult *eq=db_call(pg,LP_SQL_WITNESSES_EXISTING,&ea);
        lp_idmap *known_w=lp_idmap_new();
        for(int r=0;r<PQntuples(eq);r++)lp_idmap_put(known_w,col_id(eq,r,0),NULL);
        for(size_t i=0;i<nroots;i++)if(lp_idmap_find(known_w,&roots[i].id)<0){
            lp_idmap_put(writer.new_witnesses,&roots[i].id,NULL);writer.witness_changed=1;
        }
        lp_idmap_free(known_w);PQclear(eq);args_free(&ea);
        lp_copy wcopy={0};db_copy_open_stage_witnesses(&wcopy,pg,writer.witnesses);
        for(size_t i=0;i<nroots;i++){
            lp_copy_row(&wcopy,3);lp_copy_id(&wcopy,&roots[i].id);
            if(roots[i].has_lineage)lp_copy_id(&wcopy,&roots[i].lineage);else lp_copy_null(&wcopy);
            lp_copy_f64(&wcopy,roots[i].trust);
        }
        copy_close(&wcopy);lp_buf_free(&wcopy.buf);
        if(writer.series_changed || writer.witness_changed){
            Args pa={0};arg_text(&pa,writer.pending);
            arg_text(&pa,writer.affected);
            if(lp_idmap_count(writer.new_witnesses))
                arg_ids(&pa,lp_idmap_keys(writer.new_witnesses),lp_idmap_count(writer.new_witnesses));
            else arg_null(&pa);
            PGresult *pub=db_call(pg,LP_SQL_STAGE_PUBLISH_ATTESTATIONS,&pa);args_free(&pa);
            if(!pub || PQntuples(pub)!=1 || PQgetisnull(pub,0,0)){
                if(pub)PQclear(pub);db_tx_try(pg,LP_SQL_TX_ROLLBACK,NULL,0);
                lp_idmap_free(writer.known);lp_idmap_free(writer.registered);lp_idmap_free(writer.new_witnesses);
                writer=(Loader){0};return 1;
            }
            admitted=(uint64_t)col_i64(pub,0,0);PQclear(pub);
        }
        db_tx(pg,LP_SQL_TX_COMMIT);
    }
    db_tx(pg,LP_SQL_TX_BEGIN_RC);
    db_stage_drop(pg,owned,6);
    db_tx(pg,LP_SQL_TX_COMMIT);
    st->attestations+=admitted;st->attestations_measured=1;
    lp_idmap_free(writer.known);
    if(writer.registered)lp_idmap_free(writer.registered);
    if(writer.new_witnesses)lp_idmap_free(writer.new_witnesses);
    writer=(Loader){0};
    TABLE_EACH(x) x->keep=0;
    return 0;
}
int cmd_merge(int argc,char **argv){
    const char *conninfo=laplace_db();
    int at=opts(argc,argv,(const Opt[]){ {.flag="-d",.kind='s',.at=&conninfo}, {0} });
    if(at<argc){fputs("usage: laplace merge [-d conninfo]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);db_exec_contract(pg,LP_SQL_ENSURE_STAGE,NULL);
    Args ma={0};arg_text(&ma,"stage.entity");arg_text(&ma,"stage.physicality");
    char err[512];PGresult *q=db_call_try(pg,LP_SQL_MERGE_STAGE_DEFAULT,&ma,err,sizeof err);args_free(&ma);
    if(!q){merge_failure(pg,"stage.entity","stage.physicality",err);PQfinish(pg);return 1;}
    printf("merged %lld entities, %lld paths, %lld existing rows updated\n",
        (long long)col_i64(q,0,0),(long long)col_i64(q,0,1),(long long)col_i64(q,0,2));
    PQclear(q);PQfinish(pg);return 0;
}
int cmd_projection(int argc,char **argv){
    const char *conninfo=laplace_db(); int apply=0;
    int at=opts(argc,argv,(const Opt[]){
        {.flag="-d",.kind='s',.at=&conninfo}, {.flag="--apply",.kind='b',.at=&apply}, {0}});
    if(at<argc){fputs("usage: laplace projection [-d conninfo] [--apply]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);
    Args ra={0};arg_bool(&ra,apply!=0);
    char err[512];PGresult *result=db_call_try(pg,LP_SQL_RECOORDINATE,&ra,err,sizeof err);args_free(&ra);
    if(!result || PQntuples(result)!=1 || PQnfields(result)!=2){
        fprintf(stderr,"projection %s: %s\n",apply?"repair":"audit",result?"invalid result":err);
        PQclear(result);PQfinish(pg);return 1;
    }
    printf("projection %s: %lld entity coordinates, %lld path Hilbert keys %s\n",
        apply?"applied":"audit",(long long)col_i64(result,0,0),(long long)col_i64(result,0,1),apply?"corrected":"need correction");
    PQclear(result);PQfinish(pg);return 0;
}
