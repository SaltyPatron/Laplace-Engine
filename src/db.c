/* The client composes and prunes; PostgreSQL merges whole COPY windows.
 * Structural batches commit as entity/path pairs. Consensus and witness
 * registration share a separate source transaction, so a failed source cannot
 * publish a standing based on an unregistered witness. */
#include "engine.h"
#include "laplace/tier0.h"
#include "laplace/highway.h"
#include <omp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { lp_id claim, witness, lineage; uint64_t games; double sum, trust; uint8_t has_lineage, dirty; } Series;
struct Fold { lp_strmap *series; };
static Fold *active_fold;
typedef struct {
    PGconn **copy, *series; int ncopy, from_ingest, witness_changed;
    char entity[96], path[96], delta[96], pending[96], witnesses[96], affected[96];
    lp_idmap *known, *changed;
} Loader;
static Loader writer;
int load_whole;

static void statement(PGconn *pg, const char *sql){ exec(pg, sql); }
static void start_loader(const char *conninfo, int requested){
    if (writer.series) return;
    writer.from_ingest = active_fold != NULL;
    PGconn *control = db_connect(conninfo);
    PGresult *q = PQexec(control, "SELECT greatest(1, current_setting('max_connections')::int"
        " - current_setting('superuser_reserved_connections')::int"
        " - current_setting('reserved_connections')::int - (SELECT count(*) FROM pg_stat_activity) - 4)");
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "ingest connections: %s", PQerrorMessage(control)); exit(1); }
    int available = atoi(PQgetvalue(q, 0, 0)); PQclear(q);
    int cores = omp_get_max_threads();
    if (requested <= 0 || requested > cores) requested = cores;
    const char *pool = getenv("LAPLACE_DB_POOL_SIZE");
    if (pool && atoi(pool) > 0 && requested > atoi(pool)) requested = atoi(pool);
    writer.ncopy = requested < available ? requested : available;
    if (writer.ncopy < 1) writer.ncopy = 1;
    writer.copy = calloc((size_t)writer.ncopy, sizeof *writer.copy);
    writer.copy[0] = control;
    for (int i = 1; i < writer.ncopy; i++) writer.copy[i] = db_connect(conninfo);
    statement(control, "SELECT laplace_ensure_stage()");
    q = PQexec(control, "SELECT pg_current_xact_id()::text");
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "ingest identity: %s", PQerrorMessage(control)); exit(1); }
    char suffix[40]; snprintf(suffix, sizeof suffix, "%s", PQgetvalue(q, 0, 0)); PQclear(q);
    snprintf(writer.entity, sizeof writer.entity, "stage.lp_entity_%s", suffix);
    snprintf(writer.path, sizeof writer.path, "stage.lp_path_%s", suffix);
    snprintf(writer.delta, sizeof writer.delta, "stage.lp_delta_%s", suffix);
    snprintf(writer.pending, sizeof writer.pending, "stage.lp_pending_%s", suffix);
    snprintf(writer.witnesses, sizeof writer.witnesses, "stage.lp_witness_%s", suffix);
    snprintf(writer.affected, sizeof writer.affected, "stage.lp_affected_%s", suffix);
    char sql[1800];
    snprintf(sql, sizeof sql,
        "CREATE UNLOGGED TABLE %s (LIKE stage.entity INCLUDING DEFAULTS);"
        "CREATE UNLOGGED TABLE %s (LIKE stage.physicality INCLUDING DEFAULTS);"
        "CREATE UNLOGGED TABLE %s (LIKE stage.attestation INCLUDING DEFAULTS);"
        "CREATE UNLOGGED TABLE %s (LIKE stage.attestation INCLUDING DEFAULTS);"
        "CREATE UNLOGGED TABLE %s (LIKE public.witness INCLUDING DEFAULTS);"
        "CREATE UNLOGGED TABLE %s (claim blake3 NOT NULL)",
        writer.entity, writer.path, writer.delta, writer.pending, writer.witnesses, writer.affected);
    statement(control, sql);
    writer.known = lp_idmap_new(); writer.changed = lp_idmap_new();
    writer.series = db_connect(conninfo);
    statement(writer.series, "BEGIN");
}

/* One parent-table set query per bounded ID window, across every tier. */
static uint8_t *recorded(PGconn **pg, int npg, const lp_id *ids, uint64_t n){
    uint8_t *hit = calloc(n ? (size_t)n : 1, 1);
    const uint64_t window = 65536;
    #pragma omp parallel for num_threads(npg) schedule(dynamic)
    for (uint64_t begin = 0; begin < n; begin += window) {
        uint64_t count = n - begin < window ? n - begin : window;
        lp_idmap *places = lp_idmap_sized(sizeof(uint8_t));
        for (uint64_t i = 0; i < count; i++) { bool fresh; lp_idmap_put(places, &ids[begin + i], &fresh); }
        Args a = {0}; arg_ids(&a, ids + begin, (size_t)count);
        PGresult *q = ask_once(pg[omp_get_thread_num()], "SELECT id FROM public.entity WHERE id = ANY($1::blake3[])", &a);
        for (int row = 0; row < PQntuples(q); row++) {
            lp_id id; memcpy(id.b, PQgetvalue(q, row, 0), 16);
            int64_t at = lp_idmap_find(places, &id);
            if (at >= 0) *(uint8_t *)lp_idmap_at(places, (size_t)at) = 1;
        }
        for (uint64_t i = 0; i < count; i++) { int64_t at = lp_idmap_find(places, &ids[begin + i]); hit[begin + i] = *(const uint8_t *)lp_idmap_at(places, (size_t)at); }
        PQclear(q); args_free(&a); lp_idmap_free(places);
    }
    return hit;
}
uint8_t *db_recorded(const char *conninfo, const lp_id *ids, uint64_t n){
    PGconn *pg = db_connect(conninfo); uint8_t *hit = recorded(&pg, 1, ids, n); PQfinish(pg); return hit;
}
int db_all_recorded(const char *conninfo, const lp_id *ids, const uint8_t *tiers, uint64_t n){
    (void)tiers; uint8_t *hit = db_recorded(conninfo, ids, n); int all = 1;
    for (uint64_t i = 0; i < n; i++) all &= hit[i] != 0;
    free(hit); return all;
}

/* Existence of a structural trunk alone is insufficient: an interrupted source
 * may have copied it before publishing its witness. Only published containment
 * permits skipping the file's bytes and evidence. */
int files_known(PGconn **pg, int npg, File *files, int nfiles, LoadStats *st){
    (void)npg;
    lp_id *ids = malloc(sizeof *ids * (size_t)(nfiles ? nfiles : 1));
    int *of = malloc(sizeof *of * (size_t)(nfiles ? nfiles : 1)), count = 0, found = 0;
    for (int i = 0; i < nfiles; i++) if (!files[i].skipped && files[i].has_file) {
        ids[count] = files[i].file.id; of[count++] = i; files[i].known = 0;
    }
    if (count) {
        Args a = {0}; arg_ids(&a, ids, (size_t)count);
        PGresult *q = ask_once(pg[0],
            "WITH RECURSIVE above(origin,id) AS (SELECT id,id FROM unnest($1::blake3[]) id"
            " UNION SELECT a.origin,p.entity FROM above a JOIN public.physicality p"
            " ON p.path @> ARRAY[a.id]::blake3[])"
            " SELECT DISTINCT a.origin FROM above a JOIN public.witness w ON w.id=a.id", &a);
        lp_idmap *published = lp_idmap_new();
        for (int row = 0; row < PQntuples(q); row++) { lp_id id; bool fresh; memcpy(id.b, PQgetvalue(q,row,0),16); lp_idmap_put(published,&id,&fresh); }
        for (int i = 0; i < count; i++) if (lp_idmap_find(published, &ids[i]) >= 0) { files[of[i]].known = 1; found++; }
        PQclear(q); args_free(&a); lp_idmap_free(published);
    }
    if (st) st->known += (uint64_t)found;
    free(ids); free(of); return found;
}
int ingest_known(const char *conninfo, File *files, int nfiles){
    PGconn *pg = db_connect(conninfo); int count = files_known(&pg,1,files,nfiles,NULL); PQfinish(pg); return count;
}

Fold *fold_new(void){
    Fold *f = calloc(1,sizeof *f); f->series = lp_strmap_sized(sizeof(Series)); active_fold = f; return f;
}
void fold_free(Fold *f){
    if (!f) return;
    if (active_fold == f) active_fold = NULL;
    lp_strmap_free(f->series); free(f);
}
uint64_t fold_count(const Fold *f){ return f ? (uint64_t)lp_strmap_count(f->series) : 0; }
void fold_events(Fold *f, const File *files, int nfiles){
    for (int fi = 0; fi < nfiles; fi++) {
        const File *file = &files[fi]; if (file->known || file->skipped) continue;
        Ref source = file->has_file ? file->file : file->witness;
        double trust = file->trust;
        if (file->source) {
            const char *name = source_called(file->source);
            if (!name) { fprintf(stderr,"source %s has no declared source record\n",file->source->name); exit(1); }
            source = text_ref(CTX[0],(const uint8_t *)name,strlen(name)); trust = file->source->trust;
        }
        if (!isfinite(trust) || trust < -1 || trust > 1) { fprintf(stderr,"invalid declared witness trust\n"); exit(1); }
        for (uint64_t i=0;i<file->ev.n;i++) {
            const Event *event=&file->ev.e[i]; Node *node=table_find(&event->claim);
            if (event->kind!=EV_CLAIM || !node || node->len!=3) continue;
            uint8_t key[32]; memcpy(key,event->claim.b,16); memcpy(key+16,source.id.b,16);
            bool fresh; Series *s=lp_strmap_get(f->series,key,sizeof key,&fresh);
            if (fresh) *s=(Series){.claim=event->claim,.witness=source.id,.lineage=file->lineage.id,
                .trust=trust,.has_lineage=(uint8_t)file->has_lineage};
            if (s->games==UINT64_C(9007199254740992)) { fprintf(stderr,"series exceeds exact Native game range\n"); exit(1); }
            s->games++; s->sum+=lp_score_carried(event->score); s->dirty=1;
        }
    }
}
static void flush_series(Fold *f, LoadStats *st){
    if (!f) return;
    lp_idmap *changed=lp_idmap_new(); lp_copy copy={0}; char sql[2200];
    snprintf(sql,sizeof sql,"COPY %s (claim,witness,lineage,games,score,trust) FROM STDIN (FORMAT binary)",writer.delta);
    copy_open(&copy,writer.series,sql);
    for (size_t i=0;i<lp_strmap_count(f->series);i++) {
        Series *s=lp_strmap_at(f->series,i); if (!s->dirty) continue;
        lp_copy_row(&copy,6); lp_copy_id(&copy,&s->claim); lp_copy_id(&copy,&s->witness);
        if(s->has_lineage) lp_copy_id(&copy,&s->lineage); else lp_copy_null(&copy);
        lp_copy_i64(&copy,(int64_t)s->games); lp_copy_f64(&copy,s->sum/(double)s->games); lp_copy_f64(&copy,s->trust);
        bool fresh; lp_idmap_put(changed,&s->claim,&fresh); lp_idmap_put(writer.changed,&s->claim,&fresh); s->dirty=0;
    }
    copy_close(&copy); lp_buf_free(&copy.buf);
    size_t n=lp_idmap_count(changed);
    if(n){
        snprintf(sql,sizeof sql,
            "UPDATE %s p SET games=d.games,score=d.score,trust=d.trust,lineage=d.lineage FROM %s d"
            " WHERE p.claim=d.claim AND p.witness=d.witness;"
            "INSERT INTO %s SELECT d.* FROM %s d WHERE NOT EXISTS"
            " (SELECT FROM %s p WHERE p.claim=d.claim AND p.witness=d.witness);TRUNCATE %s",
            writer.pending,writer.delta,writer.pending,writer.delta,writer.pending,writer.delta);
        statement(writer.series,sql);
        Args a={0}; arg_text(&a,writer.pending); arg_ids(&a,lp_idmap_keys(changed),n);
        PGresult *q=ask_once(writer.series,"SELECT laplace_attest_series($1::regclass,$2::blake3[])",&a);
        st->std_upd+=lp_be(PQgetvalue(q,0,0),8); PQclear(q); args_free(&a);
    }
    lp_idmap_free(changed);
}

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
static void queue_root(const lp_id *id){
    Node *node=table_find(id); if(node && !node->keep) node->keep=3;
}
static void prune(File *files,int nfiles,LoadStats *st){
    TABLE_EACH(x) {
        /* Tier-0 is the database realizer's storage-free leaf floor. Highway
         * membership supplies an identity/coordinate, not proof that its path
         * is readable from PostgreSQL. Persist its canonical logical closure
         * on first admission; only a recorded trunk can prune that closure. */
        x->keep=lp_idmap_find(writer.known,&x->id)>=0 || lp_tier0_contains(T0,&x->id) ? 2 : 0;
    }
    TABLE_EACH(x) if(x->keep==2) known_tree(x);
    TABLE_EACH(x) if(x->pin && !x->keep) x->keep=3;
    for(int i=0;i<nfiles;i++){
        File *f=&files[i]; if(f->known || f->skipped) continue;
        if(f->has_file) queue_root(&f->file.id); else {
            queue_root(&f->trunk.id);
            for(uint64_t j=0;j<f->nsaid;j++) queue_root(&f->said[j].id);
        }
        if(f->has_lineage) queue_root(&f->lineage.id);
    }
    if(load_whole) TABLE_EACH(x) if(!x->keep) x->keep=3;
    int highest = 0; TABLE_EACH(x) if(x->tier > highest) highest = x->tier;
    for(int tier=highest;tier>0;tier--){
        Nodes candidates={0};
        /* Logical paths already bypass Native's binary hash spines. Explicit
         * record-block spines are traversed immediately, never queried. */
        int more;
        do {
            more=0;
            TABLE_EACH(x) if(x->keep==3 && x->tier==(uint8_t)tier && x->spine && !x->kind){
                x->keep=1;st->new_nodes++;
                for(uint32_t i=0;i<x->nv;i++) queue_root(&VTX[x->voff+i].id);
                more=1;
            }
        }while(more);
        TABLE_EACH(x) if(x->keep==3 && x->tier==(uint8_t)tier) add_node(&candidates,x);
        if(candidates.n){
            lp_id *ids=malloc(candidates.n*sizeof *ids);
            for(size_t i=0;i<candidates.n;i++) ids[i]=candidates.item[i]->id;
            uint8_t *hit=recorded(writer.copy,writer.ncopy,ids,(uint64_t)candidates.n);
            st->rounds++;st->checked+=(uint64_t)candidates.n;
            for(size_t i=0;i<candidates.n;i++){
                Node *x=candidates.item[i]; if(x->keep==2) continue;
                if(hit[i]){known_tree(x);st->found++;}
                else{x->keep=1;st->new_nodes++;for(uint32_t j=0;j<x->nv;j++)queue_root(&VTX[x->voff+j].id);}
            }
            free(hit);free(ids);
        }
        free(candidates.item);
    }
}

typedef struct { Node *node; uint64_t hilbert; } Row;
static int hilbert_order(const void *a,const void *b){
    const Row *x=a,*y=b;return x->hilbert<y->hilbert?-1:x->hilbert>y->hilbert?1:memcmp(x->node->id.b,y->node->id.b,16);
}
static void copy_rows(PGconn *pg,const Row *rows,size_t lo,size_t hi){
    lp_copy copy={0}; char sql[256]; uint8_t geometry[128],zero[12]={0}; zero[3]=64;
    statement(pg,"BEGIN");
    snprintf(sql,sizeof sql,"COPY %s (id,tier,coord,hilbert,mask) FROM STDIN (FORMAT binary)",writer.entity);
    copy_open(&copy,pg,sql);
    for(size_t i=lo;i<hi;i++){
        const Node *x=rows[i].node; double point[4];
        for(int axis=0;axis<4;axis++)point[axis]=(double)x->m[axis]/LP_FIXED_ONE;
        size_t size=lp_ewkb_point4(point,geometry,sizeof geometry);
        lp_copy_row(&copy,5);lp_copy_id(&copy,&x->id);lp_copy_i16(&copy,x->tier);
        lp_copy_field(&copy,geometry,(uint32_t)size);lp_copy_i64(&copy,(int64_t)(rows[i].hilbert^UINT64_C(0x8000000000000000)));lp_copy_field(&copy,zero,sizeof zero);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);copy=(lp_copy){0};
    snprintf(sql,sizeof sql,"COPY %s (entity,tier,hilbert,path,mask) FROM STDIN (FORMAT binary)",writer.path);
    copy_open(&copy,pg,sql);
    lp_id *ids=NULL;uint64_t *runs=NULL;uint32_t *spare=NULL;uint8_t *path=NULL;size_t capacity=0,path_capacity=0;
    for(size_t i=lo;i<hi;i++){
        const Node *x=rows[i].node;
        if(x->nv>capacity){capacity=x->nv;ids=xrealloc(ids,capacity*sizeof *ids);runs=xrealloc(runs,capacity*sizeof *runs);spare=xrealloc(spare,capacity*sizeof *spare);}
        for(uint32_t j=0;j<x->nv;j++){const Vtx *v=&VTX[x->voff+j];ids[j]=v->id;runs[j]=v->m;spare[j]=v->spare;}
        size_t size=lp_ewkb_runs_spare(ids,runs,spare,x->nv,NULL,0);
        if(size>path_capacity){path_capacity=size;path=xrealloc(path,path_capacity);}
        lp_ewkb_runs_spare(ids,runs,spare,x->nv,path,size);
        uint8_t mask[12]={0};mask[3]=64;
        for(int bit=0;bit<8;bit++)if(x->kind&(1u<<bit))mask[4+(bit>>3)]|=(uint8_t)(0x80u>>(bit&7));
        lp_copy_row(&copy,5);lp_copy_id(&copy,&x->id);lp_copy_i16(&copy,x->tier);
        lp_copy_i64(&copy,(int64_t)(rows[i].hilbert^UINT64_C(0x8000000000000000)));
        lp_copy_field(&copy,path,(uint32_t)size);lp_copy_field(&copy,mask,sizeof mask);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);
    free(ids);free(runs);free(spare);free(path);statement(pg,"COMMIT");
}
int load(const char *conninfo,int npg,File *files,int nfiles,LoadStats *st){
    start_loader(conninfo,npg);table_kinds();
    for(int i=0;i<nfiles;i++)for(uint64_t j=0;j<files[i].ev.n;j++){
        const Event *event=&files[i].ev.e[j];Node *node=table_find(&event->claim);
        if(node)node->kind|=(uint8_t)(1u<<(event->kind==EV_RECORD?LP_KIND_RECORD:node->len==3?LP_KIND_CLAIM:LP_KIND_TUPLE));
    }
    TABLE_EACH(x) if(x->len!=3)x->kind&=(uint8_t)~(1u<<LP_KIND_CLAIM);
    double begin=now();prune(files,nfiles,st);st->t_dedup+=now()-begin;begin=now();
    size_t count=0;TABLE_EACH(x)if(x->keep==1 || (x->keep==2 && x->kind))count++;
    Row *rows=malloc((count?count:1)*sizeof *rows);size_t at=0;
    TABLE_EACH(x)if(x->keep==1 || (x->keep==2 && x->kind)){lp_coord c;memcpy(c.m,x->m,sizeof c.m);rows[at++]=(Row){x,lp_hilbert4(&c)};}
    qsort(rows,count,sizeof *rows,hilbert_order);
    if(count){
        int lanes=writer.ncopy;if((size_t)lanes>count)lanes=(int)count;
        #pragma omp parallel for num_threads(lanes) schedule(static,1)
        for(int lane=0;lane<lanes;lane++)copy_rows(writer.copy[lane],rows,count*(size_t)lane/(size_t)lanes,count*(size_t)(lane+1)/(size_t)lanes);
        char sql[320];snprintf(sql,sizeof sql,"SELECT * FROM laplace_merge_stage('%s'::regclass,'%s'::regclass)",writer.entity,writer.path);
        PGresult *q=PQexec(writer.copy[0],sql);
        if(PQresultStatus(q)!=PGRES_TUPLES_OK){fprintf(stderr,"stage merge: %s",PQerrorMessage(writer.copy[0]));exit(1);}
        st->ent_rows+=strtoull(PQgetvalue(q,0,0),NULL,10);st->phy_rows+=strtoull(PQgetvalue(q,0,1),NULL,10);PQclear(q);
    }
    free(rows);TABLE_EACH(x)if(x->keep==1 || x->keep==2){bool fresh;lp_idmap_put(writer.known,&x->id,&fresh);}
    st->t_copy+=now()-begin;begin=now();flush_series(active_fold,st);st->t_sem+=now()-begin;
    if(!writer.from_ingest)return merge(conninfo,npg);
    return 0;
}

int load_register(const char *conninfo,int npg,const LoadWitness *roots,size_t count){
    if(!count)return 0;start_loader(conninfo,npg);
    lp_copy copy={0};char sql[1600];snprintf(sql,sizeof sql,"COPY %s (id,lineage,trust) FROM STDIN (FORMAT binary)",writer.witnesses);
    copy_open(&copy,writer.series,sql);
    for(size_t i=0;i<count;i++){
        if(!isfinite(roots[i].trust) || roots[i].trust < -1 || roots[i].trust > 1){fprintf(stderr,"invalid witness trust\n");exit(1);}
        lp_copy_row(&copy,3);lp_copy_id(&copy,&roots[i].id);
        if(roots[i].has_lineage)lp_copy_id(&copy,&roots[i].lineage);else lp_copy_null(&copy);
        lp_copy_f64(&copy,roots[i].trust);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);
    snprintf(sql,sizeof sql,
        "WITH updated AS (UPDATE public.witness w SET lineage=s.lineage,trust=s.trust FROM %s s WHERE w.id=s.id"
        " AND (w.lineage IS DISTINCT FROM s.lineage OR w.trust IS DISTINCT FROM s.trust) RETURNING w.id),"
        " inserted AS (INSERT INTO public.witness SELECT DISTINCT s.* FROM %s s"
        " WHERE NOT EXISTS(SELECT FROM public.witness w WHERE w.id=s.id) RETURNING id)"
        " SELECT (SELECT count(*) FROM updated)+(SELECT count(*) FROM inserted)",writer.witnesses,writer.witnesses);
    PGresult *result=PQexec(writer.series,sql);
    if(PQresultStatus(result)!=PGRES_TUPLES_OK){fprintf(stderr,"witness registration: %s",PQerrorMessage(writer.series));exit(1);}
    writer.witness_changed |= strtoull(PQgetvalue(result,0,0),NULL,10) != 0;
    PQclear(result);return 0;
}
int merge(const char *conninfo,int npg){
    (void)conninfo;(void)npg;if(!writer.series)return 0;
    lp_copy copy={0};char sql[2400];snprintf(sql,sizeof sql,"COPY %s (claim) FROM STDIN (FORMAT binary)",writer.affected);
    copy_open(&copy,writer.series,sql);
    const lp_id *changed=lp_idmap_keys(writer.changed);
    for(size_t i=0;i<lp_idmap_count(writer.changed);i++){lp_copy_row(&copy,1);lp_copy_id(&copy,&changed[i]);}
    copy_close(&copy);lp_buf_free(&copy.buf);
    /* The final registered trunk includes unchanged files too. Replace pending
     * provisional evidence with the complete containment estate inside the same
     * transaction as witness registration. No standings are read by the client. */
    snprintf(sql,sizeof sql,
        "DELETE FROM %s;"
        "WITH RECURSIVE below(id) AS (SELECT id FROM %s UNION"
        " SELECT child FROM below b JOIN public.physicality p ON p.entity=b.id"
        " CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) child),"
        " claims AS (SELECT claim FROM %s UNION SELECT b.id FROM below b"
        " JOIN public.physicality p ON p.entity=b.id WHERE laplace_mask_has_value(p.mask,'kind','claim')"
        " AND laplace_path_length(p.path)=3)"
        " SELECT laplace_attest_series('%s'::regclass,array_agg(claim)) FROM claims",
        writer.pending,writer.witnesses,writer.affected,writer.pending);
    /* An unchanged registered source contributes no new evidence on replay.
     * Recompute only for new series or changed witness trust/containment. */
    if(lp_idmap_count(writer.changed) || writer.witness_changed) statement(writer.series,sql);
    snprintf(sql,sizeof sql,"DROP TABLE %s,%s,%s,%s,%s,%s",writer.entity,writer.path,writer.delta,writer.pending,writer.witnesses,writer.affected);
    statement(writer.series,sql);statement(writer.series,"COMMIT");
    PQfinish(writer.series);for(int i=0;i<writer.ncopy;i++)PQfinish(writer.copy[i]);
    free(writer.copy);lp_idmap_free(writer.known);lp_idmap_free(writer.changed);writer=(Loader){0};return 0;
}
int cmd_merge(int argc,char **argv){
    const char *conninfo=laplace_db();
    int at=opts(argc,argv,(const Opt[]){ {.flag="-d",.kind='s',.at=&conninfo}, {0} });
    if(at<argc){fputs("usage: laplace merge [-d conninfo]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);statement(pg,"SELECT laplace_ensure_stage()");
    PGresult *q=PQexec(pg,"SELECT * FROM laplace_merge_stage('stage.entity','stage.physicality')");
    if(PQresultStatus(q)!=PGRES_TUPLES_OK){fprintf(stderr,"stage merge: %s",PQerrorMessage(pg));PQclear(q);PQfinish(pg);return 1;}
    printf("merged %s entities, %s paths, %s existing rows updated\n",PQgetvalue(q,0,0),PQgetvalue(q,0,1),PQgetvalue(q,0,2));
    PQclear(q);PQfinish(pg);return 0;
}
