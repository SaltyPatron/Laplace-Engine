/* The client composes and prunes; PostgreSQL merges whole COPY windows.
 * Structural and observation windows commit privately as they flow. Only
 * canonical witness registration and authoritative standings share the final
 * publication transaction; parsing never retains consensus bucket locks. */
#include "db_internal.h"
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
         * on first admission; only a recorded trunk can prune that closure. */
        x->keep=lp_idmap_find(writer.known,&x->id)>=0 || lp_tier0_contains(T0,&x->id) ? 2 : 0;
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
static void carrier_bytes(lp_mask value,uint8_t out[12]){
    memset(out,0,12);out[3]=64;
    for(unsigned bit=0;bit<64;bit++) if(value&(UINT64_C(1)<<bit)) out[4+(bit>>3)]|=(uint8_t)(0x80u>>(bit&7));
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
                    PGresult *children=ask_once(pg,"SELECT ordinal,id,@postgis@.ST_AsEWKB(coord) FROM @extschema@.laplace_entities($1)",&input);
                    const Node *next=NULL;unsigned changed=0;lp_coord_sum local_sum={{0},0},recorded_sum={{0},0};
                    for(int i=0;i<PQntuples(children);i++){
                        uint64_t ordinal=(uint64_t)col_int(children,i,0);if(!ordinal || ordinal>node->nv)continue;
                        int known=0;Ref local=ref_of_id(col_id(children,i,1),&known);lp_coord recorded;lp_ewkb_info info;
                        if(!known || PQgetisnull(children,i,2) || !lp_ewkb_visit((const uint8_t *)PQgetvalue(children,i,2),
                            (size_t)PQgetlength(children,i,2),binding_point,&recorded,&info))continue;
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
    char sql[8192];
    int length=snprintf(sql,sizeof sql,
        "WITH rejected(reason,id,detail) AS ("
        " SELECT 'duplicate entity',id,'rows='||count(*) FROM %s GROUP BY id HAVING count(*)<>1"
        " UNION ALL SELECT 'duplicate path',entity,'rows='||count(*) FROM %s GROUP BY entity HAVING count(*)<>1"
        " UNION ALL SELECT 'invalid entity carrier/tier',id,'tier='||tier||' mask='||mask FROM %s"
        " WHERE tier NOT BETWEEN 1 AND 255 OR NOT @extschema@.laplace_mask_valid(mask)"
        " UNION ALL SELECT 'invalid path carrier/tier',entity,'tier='||tier||' mask='||mask FROM %s"
        " WHERE tier NOT BETWEEN 1 AND 255 OR NOT @extschema@.laplace_mask_valid(mask)"
        " UNION ALL SELECT 'entity identity conflict',s.id,"
        " 'coord_equal='||(@postgis@.ST_AsEWKB(s.coord)=@postgis@.ST_AsEWKB(d.coord))||' hilbert_equal='||(s.hilbert=d.hilbert)"
        " ||' staged='||@postgis@.ST_AsEWKT(s.coord)||' recorded='||@postgis@.ST_AsEWKT(d.coord)"
        " FROM (SELECT * FROM %s WHERE coord IS NOT NULL OR hilbert IS NOT NULL) s"
        " CROSS JOIN LATERAL @extschema@.laplace_entities(ARRAY[s.id]) d"
        " WHERE @postgis@.ST_AsEWKB(s.coord)<>@postgis@.ST_AsEWKB(d.coord) OR s.hilbert<>d.hilbert"
        " UNION ALL SELECT 'path identity conflict',s.entity,"
        " 'ids_equal='||(@extschema@.laplace_vertex_ids(s.path)=@extschema@.laplace_vertex_ids(d.path))"
        " ||' hilbert_equal='||(s.hilbert=d.hilbert)"
        " ||' staged_vertices='||@extschema@.laplace_path_length(s.path)||' recorded_vertices='||@extschema@.laplace_path_length(d.path)"
        " ||' staged_prefix='||encode(substring(@postgis@.ST_AsEWKB(s.path) from 1 for 160),'hex')"
        " ||' recorded_prefix='||encode(substring(@postgis@.ST_AsEWKB(d.path) from 1 for 160),'hex')"
        " FROM (SELECT * FROM %s WHERE path IS NOT NULL OR hilbert IS NOT NULL) s"
        " CROSS JOIN LATERAL @extschema@.laplace_physicalities(ARRAY[s.entity]) d"
        " WHERE (@postgis@.ST_AsEWKB(s.path)<>@postgis@.ST_AsEWKB(d.path) AND @extschema@.laplace_path_enrich(d.path,s.path) IS NULL) OR s.hilbert<>d.hilbert"
        " UNION ALL SELECT 'entity without path',s.id,'' FROM (SELECT * FROM %s WHERE NOT present) s"
        " LEFT JOIN %s batch ON batch.entity OPERATOR(@extschema@.=) s.id LEFT JOIN @extschema@.physicality d ON d.entity OPERATOR(@extschema@.=) s.id"
        " WHERE NOT s.present AND batch.entity IS NULL AND d.entity IS NULL"
        " UNION ALL SELECT 'path without entity',s.entity,'' FROM (SELECT * FROM %s WHERE NOT present) s"
        " LEFT JOIN %s batch ON batch.id OPERATOR(@extschema@.=) s.entity LEFT JOIN @extschema@.entity d ON d.id OPERATOR(@extschema@.=) s.entity"
        " WHERE NOT s.present AND batch.id IS NULL AND d.id IS NULL)"
        ",sample AS MATERIALIZED (SELECT reason,id,detail FROM rejected ORDER BY reason,id LIMIT 8)"
        " SELECT s.reason,s.id::text,s.detail,@postgis@.ST_AsEWKB(p.path) FROM sample s"
        " LEFT JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) s.id ORDER BY s.reason,s.id",
        entities,paths,entities,paths,entities,paths,entities,paths,paths,entities);
    if(length<0 || (size_t)length>=sizeof sql){fputs("stage rejection diagnostic exceeded its bound\n",stderr);return;}
    PGresult *detail=db_exec(pg,sql);
    if(PQresultStatus(detail)==PGRES_TUPLES_OK){
        for(int row=0;row<PQntuples(detail);row++){
            fprintf(stderr,"  stage rejection: %s id=%s %s\n",PQgetvalue(detail,row,0),PQgetvalue(detail,row,1),PQgetvalue(detail,row,2));
            lp_id id; if(!id_parse(PQgetvalue(detail,row,1),&id)) continue;
            const Node *node=table_find(&id); if(!node) continue;
            fprintf(stderr,"    staged decomposition: tier=%u parts=%u vertices=%u\n",node->tier,node->len,node->nv);
            for(uint32_t i=0;i<node->nv && i<8;i++){
                const Vtx *v=&VTX[node->voff+i]; char child[33]; id_text(&v->id,child);
                fprintf(stderr,"    child=%s run=%u\n",child,VRUN(v->m));
            }
            if(!strcmp(PQgetvalue(detail,row,0),"entity identity conflict") && node->nv)coordinate_failure_chain(pg,node,0);
            if(!PQgetisnull(detail,row,3))
                fprintf(stderr,"    recorded path prefix=%.322s\n",PQgetvalue(detail,row,3));
        }
    }else fprintf(stderr,"stage rejection diagnostic: %s",PQerrorMessage(pg));
    PQclear(detail);
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
    lp_copy copy={0}; char sql[512]; uint8_t geometry[128],mask[12];
    snprintf(sql,sizeof sql,"BEGIN;COPY %s (id,tier,coord,hilbert,mask,present) FROM STDIN (FORMAT binary);"
        "COPY %s (entity,tier,hilbert,path,mask,present) FROM STDIN (FORMAT binary);COMMIT",writer.entity[lane],writer.path[lane]);
    copy_open(&copy,pg,sql);
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
        carrier_bytes(rows[i].lexical,mask);
        lp_copy_field(&copy,mask,sizeof mask);lp_copy_field(&copy,&present,1);
    }
    copy_next(&copy);
    PathEncoding path={0};
    for(size_t i=lo;i<hi;i++){
        const Node *x=rows[i].node;uint8_t present=x->keep==2;
        carrier_bytes(x->kind,mask);
        lp_copy_row(&copy,6);lp_copy_id(&copy,&x->id);lp_copy_i16(&copy,x->tier);
        if(present)lp_copy_null(&copy);else lp_copy_i64(&copy,(int64_t)(rows[i].hilbert^UINT64_C(0x8000000000000000)));
        if(present && !rows[i].annotations)lp_copy_null(&copy);
        else{size_t size=encode_node_path(x,&path);lp_copy_field(&copy,path.bytes,(uint32_t)size);}
        lp_copy_field(&copy,mask,sizeof mask);lp_copy_field(&copy,&present,1);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);
    free(path.ids);free(path.runs);free(path.spare);free(path.bytes);
}
typedef lp_vec(uint32_t) IndexPartitions;
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
static void maintain_window(PGconn *pg,IndexPartitions *partitions){
    if(!partitions->n)return;
    qsort(partitions->v,partitions->n,sizeof *partitions->v,partition_order);
    size_t n=0;
    for(size_t i=0;i<partitions->n;i++)
        if(!i || partitions->v[i]!=partitions->v[i-1])partitions->v[n++]=partitions->v[i];
    lp_buf ids={0};lp_pg_array(&ids,26,n);
    for(size_t i=0;i<n;i++){lp_buf_be(&ids,4,4);lp_buf_be(&ids,partitions->v[i],4);}
    Args input={0};arg_raw(&input,ids.b,ids.n);
    PQclear(ask_once(pg,"SELECT @extschema@.laplace_clean_partitions($1::oid[])",&input));
    args_free(&input);lp_buf_free(&ids);
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
            char sql[512];snprintf(sql,sizeof sql,
                "SELECT * FROM @extschema@.laplace_merge_stage('%s'::regclass,'%s'::regclass%s)",
                writer.entity[lane],writer.path[lane],writer.window_index?",true":"");
            PGresult *q=db_command_binary(writer.copy[lane],sql);
            if(PQresultStatus(q)!=PGRES_TUPLES_OK){
                merge_failure(writer.copy[lane],writer.entity[lane],writer.path[lane],PQresultErrorMessage(q));
                PQclear(q);exit(1);
            }
            if(PQntuples(q)!=1 || PQnfields(q)!=(writer.window_index?4:3)
                || PQgetisnull(q,0,0) || PQgetisnull(q,0,1)){
                fputs("publication: invalid Native merge result\n",stderr);PQclear(q);exit(1);
            }
            entities+=(uint64_t)col_int(q,0,0);
            paths+=(uint64_t)col_int(q,0,1);
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
        double phase=now();
        #pragma omp parallel for num_threads(lanes) schedule(static,1)
        for(unsigned lane=0;lane<lanes;lane++)maintain_window(writer.copy[lane],maintenance+lane);
        double elapsed=now()-phase;st->t_index+=elapsed;st->t_publish+=elapsed;
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
    statement(writer.series,"BEGIN ISOLATION LEVEL READ COMMITTED");
    if(!writer.registered)writer.registered=lp_idmap_new();
    if(!writer.new_witnesses)writer.new_witnesses=lp_idmap_new();
    lp_id *ids=malloc(count*sizeof *ids);
    if(!ids){perror("witness measurement roots");return 1;}
    for(size_t i=0;i<count;i++){ids[i]=roots[i].id;lp_idmap_put(writer.registered,ids+i,NULL);}
    Args existing_args={0};arg_ids(&existing_args,ids,count);free(ids);
    PGresult *existing=ask(writer.series,"SELECT DISTINCT witness FROM @extschema@.attestation WHERE witness OPERATOR(@extschema@.=) ANY($1)",&existing_args);
    lp_idmap *known_witnesses=lp_idmap_new();
    for(int i=0;i<PQntuples(existing);i++)lp_idmap_put(known_witnesses,col_id(existing,i,0),NULL);
    for(size_t i=0;i<count;i++)if(lp_idmap_find(known_witnesses,&roots[i].id)<0){
        lp_idmap_put(writer.new_witnesses,&roots[i].id,NULL);
        writer.witness_changed=1;
    }
    lp_idmap_free(known_witnesses);PQclear(existing);args_free(&existing_args);
    lp_copy copy={0};char sql[2400];snprintf(sql,sizeof sql,"COPY %s (id,lineage,trust) FROM STDIN (FORMAT binary)",writer.witnesses);
    copy_open(&copy,writer.series,sql);
    for(size_t i=0;i<count;i++){
        if(!isfinite(roots[i].trust) || roots[i].trust < -1 || roots[i].trust > 1){fprintf(stderr,"invalid witness trust\n");exit(1);}
        lp_copy_row(&copy,3);lp_copy_id(&copy,&roots[i].id);
        if(roots[i].has_lineage)lp_copy_id(&copy,&roots[i].lineage);else lp_copy_null(&copy);
        lp_copy_f64(&copy,roots[i].trust);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);
    if(count==1){
        /* Rewrite provisional source/model witness ID in writer.pending to the final trunk ID. */
        snprintf(sql,sizeof sql,
            "UPDATE %s SET witness=$1::@extschema@.blake3 WHERE witness OPERATOR(@extschema@.<>) $1::@extschema@.blake3",
            writer.pending);
        Args rw={0};arg_raw(&rw,roots[0].id.b,16);PQclear(ask_once(writer.series,sql,&rw));args_free(&rw);
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
            int own=PQtransactionStatus(writer.series)==PQTRANS_IDLE; if(own)statement(writer.series,"BEGIN");
            if(count==1){
                snprintf(sql,sizeof sql,
                    "WITH restored AS ("
                    "  SELECT c.claim, $2::@extschema@.blake3 AS witness,"
                    "         coalesce(c.dependence_root, w.lineage) AS lineage,"
                    "         sum(c.games)::bigint AS games, greatest(sum(c.tokens)::bigint, sum(c.games)::bigint) AS tokens,"
                    "         sum(c.score) AS wins, min(nullif(c.\"position\",0)) AS \"position\","
                    "         coalesce(max(c.dependence_trust), w.trust) AS trust"
                    "  FROM @extschema@.laplace_contained_series($1::@extschema@.blake3[]) c"
                    "  CROSS JOIN %s w"
                    "  WHERE c.games > 0"
                    "  GROUP BY c.claim, coalesce(c.dependence_root, w.lineage), w.trust"
                    ") INSERT INTO %s (claim,witness,lineage,games,tokens,score,\"position\",trust,wins)"
                    " SELECT claim,witness,lineage,games,tokens,least(1.0,greatest(0.0,wins/games)),\"position\",trust,wins FROM restored",
                    writer.witnesses,writer.delta);
                Args args={0};arg_ids(&args,lp_idmap_keys(reused),lp_idmap_count(reused));arg_raw(&args,roots[0].id.b,16);
                PQclear(ask_once(writer.series,sql,&args));args_free(&args);
            }else{
                snprintf(sql,sizeof sql,
                    "WITH restored AS ("
                    "  SELECT c.claim, c.trunk AS witness,"
                    "         coalesce(c.dependence_root, w.lineage) AS lineage,"
                    "         c.games::bigint AS games, greatest(c.tokens::bigint, c.games::bigint) AS tokens,"
                    "         c.score AS wins, nullif(c.\"position\",0) AS \"position\","
                    "         coalesce(c.dependence_trust, w.trust) AS trust"
                    "  FROM @extschema@.laplace_contained_series($1::@extschema@.blake3[]) c"
                    "  JOIN %s w ON w.id OPERATOR(@extschema@.=) c.trunk"
                    "  WHERE c.games > 0"
                    ") INSERT INTO %s (claim,witness,lineage,games,tokens,score,\"position\",trust,wins)"
                    " SELECT claim,witness,lineage,games,tokens,least(1.0,greatest(0.0,wins/games)),\"position\",trust,wins FROM restored",
                    writer.witnesses,writer.delta);
                Args args={0};arg_ids(&args,lp_idmap_keys(reused),lp_idmap_count(reused));
                PQclear(ask_once(writer.series,sql,&args));args_free(&args);
            }
            series_delta_apply(); if(own)statement(writer.series,"COMMIT");
            writer.series_changed=1;
        }
        lp_idmap_free(reused);
    }
    return 0;
}
int merge(const char *conninfo,int npg,LoadStats *stats){
    (void)conninfo;(void)npg;if(!writer.series)return 0;
    if(PQtransactionStatus(writer.series)==PQTRANS_IDLE)statement(writer.series,"BEGIN ISOLATION LEVEL READ COMMITTED");
    char sql[2400];uint64_t admitted=0;
    if(lp_idmap_count(writer.registered) && (writer.series_changed || writer.witness_changed || writer.counts_unknown)){
        snprintf(sql,sizeof sql,
            "INSERT INTO @extschema@.attestation (claim,witness,lineage,games,tokens,score,\"position\",trust)"
            " SELECT claim,witness,lineage,games,greatest(tokens,games),score,\"position\",trust FROM %s"
            " ON CONFLICT (claim,witness,lineage) DO UPDATE"
            "   SET games=excluded.games,tokens=excluded.tokens,score=excluded.score,"
            "       \"position\"=excluded.\"position\",trust=excluded.trust",
            writer.pending);
        statement(writer.series,sql);
        Args roots={0};arg_ids(&roots,lp_idmap_keys(writer.registered),lp_idmap_count(writer.registered));
        snprintf(sql,sizeof sql,
            "SELECT witness,count(DISTINCT claim)::bigint FROM @extschema@.attestation"
            " WHERE witness OPERATOR(@extschema@.=) ANY($1) GROUP BY witness");
        PGresult *measured=ask(writer.series,sql,&roots);
        for(int i=0;i<PQntuples(measured);i++)if(lp_idmap_find(writer.new_witnesses,col_id(measured,i,0))>=0){
            uint64_t n=(uint64_t)col_int(measured,i,1);
            if(n>UINT64_MAX-admitted){fprintf(stderr,"attestation counter overflow\n");return 1;}
            admitted+=n;
        }
        PQclear(measured);args_free(&roots);
    }
    /* Resolve only the affected claims through the indexed Native series
     * snapshot after the real trunk is registered in @extschema@.attestation. */
    snprintf(sql,sizeof sql,
        "SELECT @extschema@.laplace_attest_stage('%s'::regclass,'%s'::regclass)",
        writer.pending,writer.affected);
    /* An unchanged registered source contributes no new evidence on replay.
     * Recompute only for new series or changed witness trust/containment. */
    /* A window of a source not yet registered inserts no attestation, so folding it publishes nothing; it would only
     * hold every consensus bucket lock of its claims for the window (observation-windows). */
    if(lp_idmap_count(writer.registered) && (writer.series_changed || writer.witness_changed)) statement(writer.series,sql);
    for(int lane=0;lane<writer.ncopy;lane++){
        snprintf(sql,sizeof sql,"DROP TABLE %s,%s",writer.entity[lane],writer.path[lane]);
        statement(writer.series,sql);
    }
    snprintf(sql,sizeof sql,"DROP TABLE %s,%s,%s,%s",writer.delta,writer.pending,writer.witnesses,writer.affected);
    statement(writer.series,sql);statement(writer.series,"COMMIT");
    if(stats){stats->attestations+=admitted;stats->attestations_measured=1;}
    PQfinish(writer.series);for(int i=0;i<writer.ncopy;i++)PQfinish(writer.copy[i]);
    free(writer.copy);free(writer.entity);free(writer.path);lp_idmap_free(writer.known);
    lp_idmap_free(writer.registered);lp_idmap_free(writer.new_witnesses);writer=(Loader){0};return 0;
}
int cmd_merge(int argc,char **argv){
    const char *conninfo=laplace_db();
    int at=opts(argc,argv,(const Opt[]){ {.flag="-d",.kind='s',.at=&conninfo}, {0} });
    if(at<argc){fputs("usage: laplace merge [-d conninfo]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);statement(pg,"SELECT @extschema@.laplace_ensure_stage()");
    PGresult *q=db_exec(pg,"SELECT * FROM @extschema@.laplace_merge_stage('stage.entity','stage.physicality')");
    if(PQresultStatus(q)!=PGRES_TUPLES_OK){merge_failure(pg,"stage.entity","stage.physicality",PQresultErrorMessage(q));PQclear(q);PQfinish(pg);return 1;}
    printf("merged %s entities, %s paths, %s existing rows updated\n",PQgetvalue(q,0,0),PQgetvalue(q,0,1),PQgetvalue(q,0,2));
    PQclear(q);PQfinish(pg);return 0;
}
int cmd_projection(int argc,char **argv){
    const char *conninfo=laplace_db(); int apply=0;
    int at=opts(argc,argv,(const Opt[]){
        {.flag="-d",.kind='s',.at=&conninfo}, {.flag="--apply",.kind='b',.at=&apply}, {0}});
    if(at<argc){fputs("usage: laplace projection [-d conninfo] [--apply]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);
    const char *arguments[]={apply?"true":"false"};
    PGresult *result=db_exec_params(pg,"SELECT * FROM @extschema@.laplace_recoordinate($1::boolean)",1,NULL,arguments,NULL,NULL,0);
    if(PQresultStatus(result)!=PGRES_TUPLES_OK || PQntuples(result)!=1 || PQnfields(result)!=2){
        fprintf(stderr,"projection %s: %s",apply?"repair":"audit",PQerrorMessage(pg));
        PQclear(result);PQfinish(pg);return 1;
    }
    printf("projection %s: %s entity coordinates, %s path Hilbert keys %s\n",
        apply?"applied":"audit",PQgetvalue(result,0,0),PQgetvalue(result,0,1),apply?"corrected":"need correction");
    PQclear(result);PQfinish(pg);return 0;
}
