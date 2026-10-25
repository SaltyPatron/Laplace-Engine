/* The client composes and prunes; PostgreSQL merges whole COPY windows.
 * Structural batches commit as entity/path pairs. Consensus and witness
 * registration share a separate source transaction, so a failed source cannot
 * publish a standing based on an unregistered witness. */
#include "engine.h"
#include "laplace/tier0.h"
#include "laplace/highway.h"
#include "laplace/geometry.h"
#include <omp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { lp_id claim, witness, lineage; uint64_t games; double sum, trust; uint8_t has_lineage, dirty; } Series;
struct Fold { lp_strmap *series; lp_vec(size_t) dirty; };
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
        " - current_setting('reserved_connections')::int - (SELECT count(*) FROM pg_stat_activity) - 4),"
        " to_regprocedure('public.laplace_path_enrich(geometry,geometry)') IS NOT NULL");
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "ingest connections: %s", PQerrorMessage(control)); exit(1); }
    if(strcmp(PQgetvalue(q,0,1),"t")){fputs("ingest requires the installed laplace 1.25 annotation merge upgrade\n",stderr);PQclear(q);PQfinish(control);exit(1);}
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
        "CREATE UNIQUE INDEX ON %s (claim,witness,lineage) NULLS NOT DISTINCT;"
        "CREATE UNLOGGED TABLE %s (LIKE public.witness INCLUDING DEFAULTS);"
        "CREATE UNLOGGED TABLE %s (claim blake3 NOT NULL)",
        writer.entity, writer.path, writer.delta, writer.pending, writer.pending, writer.witnesses, writer.affected);
    statement(control, sql);
    writer.known = lp_idmap_new(); writer.changed = lp_idmap_new();
    writer.series = db_connect(conninfo);
    statement(writer.series, "BEGIN");
}

/* One parent-table set query per allocated connection, across every tier. */
static uint8_t *recorded(PGconn **pg, int npg, const lp_id *ids, uint64_t n){
    uint8_t *hit = calloc(n ? (size_t)n : 1, 1);
    if (!n) return hit;
    int lanes = (uint64_t)npg < n ? npg : (int)n;
    /* Partition the complete frontier over the actual connection allocation.
     * No fixed ID quota splits it into additional database round trips. */
    #pragma omp parallel for num_threads(lanes) schedule(static,1)
    for (int lane = 0; lane < lanes; lane++) {
        uint64_t begin = n * (uint64_t)lane / (uint64_t)lanes;
        uint64_t end = n * (uint64_t)(lane + 1) / (uint64_t)lanes;
        uint64_t count = end - begin;
        lp_idmap *places = lp_idmap_sized(sizeof(uint8_t));
        for (uint64_t i = 0; i < count; i++) { bool fresh; lp_idmap_put(places, &ids[begin + i], &fresh); }
        Args a = {0}; arg_ids(&a, ids + begin, (size_t)count);
        /* Each requested ID supplies a partition-pruning parameter. With an
         * array predicate, a generic plan repeats array processing in every
         * entity partition even when none of the IDs is stored. */
        PGresult *q = ask_once(pg[lane],
            "SELECT e.id FROM unnest($1::blake3[]) wanted(id) JOIN public.entity e ON e.id=wanted.id", &a);
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

/* A file trunk is persisted only after its complete structural closure. Its
 * existence therefore prunes the file independently of source publication.
 * A new source can attest that recorded subtree without reading its bytes. */
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
            "SELECT e.id FROM public.entity e JOIN public.physicality p ON p.entity=e.id"
            " WHERE e.id=ANY($1::blake3[])", &a);
        lp_idmap *recorded = lp_idmap_new();
        for (int row = 0; row < PQntuples(q); row++) { lp_id id; bool fresh; memcpy(id.b, PQgetvalue(q,row,0),16); lp_idmap_put(recorded,&id,&fresh); }
        for (int i = 0; i < count; i++) if (lp_idmap_find(recorded, &ids[i]) >= 0) { files[of[i]].known = 1; found++; }
        PQclear(q); args_free(&a); lp_idmap_free(recorded);
    }
    if (st) st->known += (uint64_t)found;
    free(ids); free(of); return found;
}
int ingest_known(const char *conninfo, File *files, int nfiles){
    PGconn *pg = db_connect(conninfo); int count = files_known(&pg,1,files,nfiles,NULL); PQfinish(pg); return count;
}

static bool binding_point(void *context, const lp_ewkb_span *span){
    double point[4]; if (span->count != 1 || !lp_ewkb_vertex(span, 0, point)) return false;
    lp_coord *coord = context;
    for (unsigned d = 0; d < 4; d++) {
        if (!isfinite(point[d]) || fabs(point[d]) > 1.0) return false;
        coord->m[d] = (int64_t)llrint(point[d] * (double)LP_FIXED_ONE);
    }
    return true;
}
void validate_file_contexts(const char *conninfo, File *files, int nfiles){
    size_t count=0;for(int i=0;i<nfiles;i++)if(files[i].known)count++;
    if(!count)return;
    lp_id *ids=malloc(count*2*sizeof *ids);if(!ids){perror("recorded reading contexts");exit(1);}
    size_t at=0;
    for(int i=0;i<nfiles;i++)if(files[i].known){ids[at]=files[i].file.id;ids[count+at]=file_context_value(&files[i]).id;at++;}
    Ref tag=text_ref(CTX[0],(const uint8_t *)KEY_CONTEXT_TAG,sizeof KEY_CONTEXT_TAG-1);
    Args args={0};arg_ids(&args,ids,count);arg_ids(&args,ids+count,count);arg_raw(&args,tag.id.b,sizeof tag.id.b);
    PGconn *pg=db_connect(conninfo);
    PGresult *result=ask_once(pg,
        "WITH RECURSIVE contexts(id,reading) AS ("
        " SELECT p.entity,k.id FROM public.physicality p"
        " CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) k(id)"
        " WHERE p.path @> ARRAY[$3::blake3] AND laplace_path_starts(p.path,$3::blake3)"
        " AND laplace_path_length(p.path)=2 AND k.id<>$3::blake3"
        " UNION SELECT parent.entity,c.reading FROM contexts c"
        " JOIN public.physicality child ON child.entity=c.id"
        " JOIN public.physicality parent ON parent.path @> ARRAY[c.id]::blake3[]"
        " WHERE NOT laplace_mask_has_value(child.mask,'kind','file'))"
        " SELECT wanted.id FROM unnest($1::blake3[],$2::blake3[]) wanted(id,reading)"
        " JOIN contexts c ON c.id=wanted.id GROUP BY wanted.id"
        " HAVING bool_or(c.reading<>wanted.reading)",&args);
    lp_idmap *invalid=lp_idmap_new();
    for(int row=0;row<PQntuples(result);row++){lp_id id;bool fresh;memcpy(id.b,PQgetvalue(result,row,0),16);lp_idmap_put(invalid,&id,&fresh);}
    for(int i=0;i<nfiles;i++)if(files[i].known && lp_idmap_find(invalid,&files[i].file.id)>=0){
        files[i].known=files[i].has_file=0;
    }
    lp_idmap_free(invalid);PQclear(result);PQfinish(pg);args_free(&args);free(ids);
}
/* Restore only the compiled index beneath a recorded file's metadata. Entry
 * vertices terminate the walk: their product targets are not exported into RAM.
 * One set read restores a provider; none of its source bytes are reopened. */
int load_file_keys(const char *conninfo, const File *file){
    if (!file->known || !file->has_file) return 0;
    Ref marker = text_ref(CTX[0], (const uint8_t *)KEY_BINDING_TAG, sizeof KEY_BINDING_TAG - 1);
    Args args = {0}; arg_raw(&args, file->recorded_metadata.b, sizeof file->recorded_metadata.b); arg_raw(&args, marker.id.b, sizeof marker.id.b);
    PGconn *pg = db_connect(conninfo);
    PGresult *result = ask_once(pg,
        "WITH RECURSIVE metadata(id) AS ("
        " SELECT $1::blake3"
        " UNION SELECT child FROM metadata m JOIN public.physicality p ON p.entity=m.id"
        " CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) child"
        " WHERE NOT laplace_path_starts(p.path,$2::blake3))"
        " SELECT st_asewkb(p.path) FROM metadata m JOIN public.physicality p ON p.entity=m.id"
        " WHERE laplace_path_starts(p.path,$2::blake3) AND laplace_path_length(p.path)=5", &args);
    /* vertex_ids is the GIN key SET. Decode ordered carriers in Native C;
     * treating the sorted/distinct index keys as an AST loses field order. */
    size_t count=(size_t)PQntuples(result); lp_id *ids=malloc((count?count:1)*4*sizeof *ids);
    if (!ids) { perror("recorded reference bindings"); exit(1); }
    int invalid=0;
    for (size_t row=0;row<count;row++) {
        lp_vertex vertex[5]; size_t n=lp_path_vertices((const uint8_t *)PQgetvalue(result,(int)row,0),(size_t)PQgetlength(result,(int)row,0),vertex,5);
        if (!n || n>5 || !lp_id_eq(&vertex[0].id,&marker.id)) { invalid=1; break; }
        size_t expanded=0;
        for (size_t v=0;v<n && !invalid;v++) {
            uint64_t run=vertex[v].run;
            if (!run || run>5-expanded) { invalid=1; break; }
            for (uint64_t r=0;r<run;r++,expanded++) if (expanded) ids[(expanded-1)*count+row]=vertex[v].id;
        }
        if (expanded!=5) { invalid=1; break; }
    }
    PQclear(result); args_free(&args); args=(Args){0};
    if (invalid || !count) {
        free(ids); PQfinish(pg);
        if (invalid) fprintf(stderr,"%s: invalid compiled reference entry path\n",file->path);
        else fprintf(stderr,"  restored 0 compiled reference bindings from recorded metadata: %s\n",file->path);
        return invalid;
    }
    for (unsigned col=0;col<4;col++) arg_ids(&args,ids+col*count,count);
    result=ask_once(pg,
        "SELECT laplace_text(b.hash),laplace_text(b.header),b.value,st_asewkb(e.coord),e.tier,laplace_text(b.text)"
        " FROM unnest($1::blake3[],$2::blake3[],$3::blake3[],$4::blake3[]) b(hash,header,value,text)"
        " LEFT JOIN public.entity e ON e.id=b.value",&args);
    free(ids);
    for (int row = 0; row < PQntuples(result); row++) {
        KeyRank rank = {.file=file->order}; KeyField field = {0}; lp_id hash;
        unsigned long long at, unit, member; unsigned flags, said, outcome, position, spare, length; int used = 0;
        if (PQgetisnull(result,row,0) || PQgetlength(result,row,0) != 32 || !id_parse(PQgetvalue(result,row,0),&hash) ||
            PQgetisnull(result,row,1) || sscanf(PQgetvalue(result,row,1), "%llu:%llu:%llu:%u:%u:%u:%u:%u:%u%n",
                &at,&unit,&member,&flags,&said,&outcome,&position,&spare,&length,&used) != 9 ||
            used != PQgetlength(result,row,1) || flags > 7 || said > LP_M_SAID_MASK || outcome > UINT8_MAX ||
            (!(flags & 2) && length)) { invalid = 1; break; }
        rank.at=at; rank.unit=unit; rank.member=member;
        field.has_value=(uint8_t)(flags&1); field.has_text=(uint8_t)((flags>>1)&1); field.blocked=(uint8_t)((flags>>2)&1);
        if (field.has_value) {
            if (PQgetisnull(result,row,2) || PQgetlength(result,row,2)!=16) { invalid=1; break; }
            memcpy(field.value.id.b,PQgetvalue(result,row,2),16);
            const lp_tier0_record *cached=lp_tier0_find(T0,&field.value.id); int floor=cached!=NULL;
            if (!cached) cached=lp_highway_find(HW,&field.value.id);
            if (cached) { memcpy(field.value.c.m,cached->m,sizeof field.value.c.m); field.value.tier=floor ? 0 : (uint8_t)cached->pad; }
            else {
                lp_ewkb_info info;
                if (PQgetisnull(result,row,3) || PQgetisnull(result,row,4) || PQgetlength(result,row,4)!=2 ||
                    !lp_ewkb_visit((const uint8_t *)PQgetvalue(result,row,3),(size_t)PQgetlength(result,row,3),binding_point,&field.value.c,&info) ||
                    info.type!=LP_POINT || info.vertices!=1) { invalid=1; break; }
                uint64_t tier=lp_be(PQgetvalue(result,row,4),2);
                if (tier>UINT8_MAX) { invalid=1; break; } field.value.tier=(uint8_t)tier;
            }
            field.value.said=(uint8_t)said; field.value.outcome=(uint8_t)outcome; field.value.position=position; field.value.spare=spare;
        }
        if (field.has_text && length) {
            if (PQgetisnull(result,row,5) || (uint64_t)PQgetlength(result,row,5)!=length) { invalid=1; break; }
            field.text=(const uint8_t *)PQgetvalue(result,row,5); field.length=length;
        }
        keys_restore(&hash,&field,&rank);
    }
    if (invalid) fprintf(stderr,"%s: recorded compiled reference metadata is invalid\n",file->path);
    else fprintf(stderr,"  restored %d compiled reference bindings from recorded metadata: %s\n",PQntuples(result),file->path);
    PQclear(result); args_free(&args); PQfinish(pg); return invalid;
}

Fold *fold_new(void){
    Fold *f = calloc(1,sizeof *f); f->series = lp_strmap_sized(sizeof(Series)); active_fold = f; return f;
}
void fold_free(Fold *f){
    if (!f) return;
    if (active_fold == f) active_fold = NULL;
    lp_strmap_free(f->series); lp_vec_free(&f->dirty); free(f);
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
            if (event->kind!=EV_CLAIM || !node || node->len<2) continue;
            lp_id lineage=file->lineage.id; uint8_t has_lineage=(uint8_t)file->has_lineage; double observation_trust=trust;
            if (event->has_dependence) { lineage=event->dependence; has_lineage=1; observation_trust=event->dependence_trust; }
            uint8_t key[48]={0}; memcpy(key,event->claim.b,16); memcpy(key+16,source.id.b,16);
            if (has_lineage) memcpy(key+32,lineage.b,16);
            bool fresh; size_t index=lp_strmap_put(f->series,key,sizeof key,&fresh);
            Series *s=lp_strmap_at(f->series,index);
            if (fresh) *s=(Series){.claim=event->claim,.witness=source.id,.lineage=lineage,
                .trust=observation_trust,.has_lineage=has_lineage};
            if (s->games==UINT64_C(9007199254740992)) { fprintf(stderr,"series exceeds exact Native game range\n"); exit(1); }
            /* Map slots are stable even when its backing storage grows. Queue
             * each changed series once per window, retaining cumulative games
             * without scanning the entire earlier source on every flush. */
            if(!s->dirty)lp_push(&f->dirty,index);
            s->games++; s->sum+=lp_score_carried(event->score); s->dirty=1;
        }
    }
}
static void flush_series(Fold *f, LoadStats *st){
    if (!f) return;
    lp_idmap *changed=lp_idmap_new(); lp_copy copy={0}; char sql[2200];
    snprintf(sql,sizeof sql,"COPY %s (claim,witness,lineage,games,score,trust) FROM STDIN (FORMAT binary)",writer.delta);
    copy_open(&copy,writer.series,sql);
    for (size_t i=0;i<f->dirty.n;i++) {
        Series *s=lp_strmap_at(f->series,f->dirty.v[i]);
        lp_copy_row(&copy,6); lp_copy_id(&copy,&s->claim); lp_copy_id(&copy,&s->witness);
        if(s->has_lineage) lp_copy_id(&copy,&s->lineage); else lp_copy_null(&copy);
        lp_copy_i64(&copy,(int64_t)s->games); lp_copy_f64(&copy,s->sum/(double)s->games); lp_copy_f64(&copy,s->trust);
        bool fresh; lp_idmap_put(changed,&s->claim,&fresh); lp_idmap_put(writer.changed,&s->claim,&fresh); s->dirty=0;
    }
    copy_close(&copy); lp_buf_free(&copy.buf);
    size_t n=lp_idmap_count(changed);
    if(n){
        snprintf(sql,sizeof sql,
            "INSERT INTO %s (claim,witness,lineage,games,score,trust)"
            " SELECT claim,witness,lineage,games,score,trust FROM %s"
            " ON CONFLICT(claim,witness,lineage) DO UPDATE"
            " SET games=EXCLUDED.games,score=EXCLUDED.score,trust=EXCLUDED.trust;TRUNCATE %s",
            writer.pending,writer.delta,writer.delta);
        statement(writer.series,sql);
        Args a={0}; arg_text(&a,writer.pending); arg_ids(&a,lp_idmap_keys(changed),n);
        PGresult *q=ask_once(writer.series,"SELECT laplace_attest_series($1::regclass,$2::blake3[])",&a);
        st->std_upd+=lp_be(PQgetvalue(q,0,0),8); PQclear(q); args_free(&a);
    }
    f->dirty.n=0;
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

typedef struct { Node *node; uint64_t hilbert; lp_mask lexical; } Row;
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
    const Row *x=a,*y=b;return x->hilbert<y->hilbert?-1:x->hilbert>y->hilbert?1:memcmp(x->node->id.b,y->node->id.b,16);
}
/* Rejected COPY input is retained. Identify the actual violated invariant
 * without weakening validation or dumping the batch/content into the log.
 * This set query runs only after a failed merge, outside its rolled-back
 * statement; it never changes either the caller's staging or public rows. */
static void merge_failure(PGconn *pg, const char *entities, const char *paths, const char *error){
    fprintf(stderr,"stage merge: %s",error);
    char sql[8192];
    int length=snprintf(sql,sizeof sql,
        "WITH rejected(reason,id,detail) AS ("
        " SELECT 'duplicate entity',id,'rows='||count(*) FROM %s GROUP BY id HAVING count(*)<>1"
        " UNION ALL SELECT 'duplicate path',entity,'rows='||count(*) FROM %s GROUP BY entity HAVING count(*)<>1"
        " UNION ALL SELECT 'invalid entity carrier/tier',id,'tier='||tier||' mask='||mask FROM %s"
        " WHERE tier NOT BETWEEN 1 AND 255 OR NOT laplace_mask_valid(mask)"
        " UNION ALL SELECT 'invalid path carrier/tier',entity,'tier='||tier||' mask='||mask FROM %s"
        " WHERE tier NOT BETWEEN 1 AND 255 OR NOT laplace_mask_valid(mask)"
        " UNION ALL SELECT 'entity identity conflict',s.id,"
        " 'coord_equal='||(ST_AsEWKB(s.coord)=ST_AsEWKB(d.coord))||' hilbert_equal='||(s.hilbert=d.hilbert)"
        " ||' staged='||ST_AsEWKT(s.coord)||' recorded='||ST_AsEWKT(d.coord)"
        " FROM %s s JOIN public.entity d USING(id)"
        " WHERE ST_AsEWKB(s.coord)<>ST_AsEWKB(d.coord) OR s.hilbert<>d.hilbert"
        " UNION ALL SELECT 'path identity conflict',s.entity,"
        " 'ids_equal='||(laplace_vertex_ids(s.path)=laplace_vertex_ids(d.path))"
        " ||' hilbert_equal='||(s.hilbert=d.hilbert)"
        " ||' staged_vertices='||laplace_path_length(s.path)||' recorded_vertices='||laplace_path_length(d.path)"
        " ||' staged_prefix='||encode(substring(ST_AsEWKB(s.path) from 1 for 160),'hex')"
        " ||' recorded_prefix='||encode(substring(ST_AsEWKB(d.path) from 1 for 160),'hex')"
        " FROM %s s JOIN public.physicality d ON d.entity=s.entity"
        " WHERE (ST_AsEWKB(s.path)<>ST_AsEWKB(d.path) AND laplace_path_enrich(d.path,s.path) IS NULL) OR s.hilbert<>d.hilbert"
        " UNION ALL SELECT 'entity without path',s.id,'' FROM %s s"
        " LEFT JOIN %s batch ON batch.entity=s.id LEFT JOIN public.physicality d ON d.entity=s.id"
        " WHERE batch.entity IS NULL AND d.entity IS NULL"
        " UNION ALL SELECT 'path without entity',s.entity,'' FROM %s s"
        " LEFT JOIN %s batch ON batch.id=s.entity LEFT JOIN public.entity d ON d.id=s.entity"
        " WHERE batch.id IS NULL AND d.id IS NULL)"
        ",sample AS MATERIALIZED (SELECT reason,id,detail FROM rejected ORDER BY reason,id LIMIT 8)"
        " SELECT s.reason,s.id::text,s.detail,ST_AsEWKB(p.path) FROM sample s"
        " LEFT JOIN public.physicality p ON p.entity=s.id ORDER BY s.reason,s.id",
        entities,paths,entities,paths,entities,paths,entities,paths,paths,entities);
    if(length<0 || (size_t)length>=sizeof sql){fputs("stage rejection diagnostic exceeded its bound\n",stderr);return;}
    PGresult *detail=PQexec(pg,sql);
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
static void copy_rows(PGconn *pg,const Row *rows,size_t lo,size_t hi){
    lp_copy copy={0}; char sql[256]; uint8_t geometry[128],mask[12];
    statement(pg,"BEGIN");
    snprintf(sql,sizeof sql,"COPY %s (id,tier,coord,hilbert,mask) FROM STDIN (FORMAT binary)",writer.entity);
    copy_open(&copy,pg,sql);
    for(size_t i=lo;i<hi;i++){
        const Node *x=rows[i].node; double point[4];
        for(int axis=0;axis<4;axis++)point[axis]=(double)x->m[axis]/LP_FIXED_ONE;
        size_t size=lp_ewkb_point4(point,geometry,sizeof geometry);
        lp_copy_row(&copy,5);lp_copy_id(&copy,&x->id);lp_copy_i16(&copy,x->tier);
        carrier_bytes(rows[i].lexical,mask);
        lp_copy_field(&copy,geometry,(uint32_t)size);lp_copy_i64(&copy,(int64_t)(rows[i].hilbert^UINT64_C(0x8000000000000000)));lp_copy_field(&copy,mask,sizeof mask);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);copy=(lp_copy){0};
    snprintf(sql,sizeof sql,"COPY %s (entity,tier,hilbert,path,mask) FROM STDIN (FORMAT binary)",writer.path);
    copy_open(&copy,pg,sql);
    PathEncoding path={0};
    for(size_t i=lo;i<hi;i++){
        const Node *x=rows[i].node;
        size_t size=encode_node_path(x,&path);
        carrier_bytes(x->kind,mask);
        lp_copy_row(&copy,5);lp_copy_id(&copy,&x->id);lp_copy_i16(&copy,x->tier);
        lp_copy_i64(&copy,(int64_t)(rows[i].hilbert^UINT64_C(0x8000000000000000)));
        lp_copy_field(&copy,path.bytes,(uint32_t)size);lp_copy_field(&copy,mask,sizeof mask);
    }
    copy_close(&copy);lp_buf_free(&copy.buf);
    free(path.ids);free(path.runs);free(path.spare);free(path.bytes);statement(pg,"COMMIT");
}
int load(const char *conninfo,int npg,File *files,int nfiles,LoadStats *st){
    start_loader(conninfo,npg);table_kinds();
    for(int i=0;i<nfiles;i++)for(uint64_t j=0;j<files[i].ev.n;j++){
        const Event *event=&files[i].ev.e[j];Node *node=table_find(&event->claim);
        if(node)node->kind|=(uint8_t)(1u<<(event->kind==EV_RECORD?LP_KIND_RECORD:node->len>=2?LP_KIND_CLAIM:LP_KIND_TUPLE));
    }
    TABLE_EACH(x) if(x->len<2)x->kind&=(uint8_t)~(1u<<LP_KIND_CLAIM);
    double begin=now();prune(files,nfiles,st);st->t_dedup+=now()-begin;begin=now();
    size_t count=0;TABLE_EACH(x)if(x->keep==1 || (x->keep==2 && x->kind))count++;
    Row *rows=malloc((count?count:1)*sizeof *rows);size_t at=0;
    TABLE_EACH(x)if(x->keep==1 || (x->keep==2 && x->kind)){lp_coord c;memcpy(c.m,x->m,sizeof c.m);rows[at++]=(Row){x,lp_hilbert4(&c),tuple_mask(x)};}
    qsort(rows,count,sizeof *rows,hilbert_order);
    if(count){
        int lanes=writer.ncopy;if((size_t)lanes>count)lanes=(int)count;
        #pragma omp parallel for num_threads(lanes) schedule(static,1)
        for(int lane=0;lane<lanes;lane++)copy_rows(writer.copy[lane],rows,count*(size_t)lane/(size_t)lanes,count*(size_t)(lane+1)/(size_t)lanes);
        /* Native has already computed the canonical references and paths for
         * these nodes. Send those proofs with mutable carrier declarations;
         * the checked merge validates identity and enriches only absent data.
         * No coordinate/path retrieval is needed for decomposed content. */
        char sql[1800];snprintf(sql,sizeof sql,
            "BEGIN;SELECT pg_advisory_xact_lock(1279348803,-2);"
            "SELECT * FROM laplace_merge_stage('%s'::regclass,'%s'::regclass)",
            writer.entity,writer.path);
        PGresult *q=PQexec(writer.copy[0],sql);
        if(PQresultStatus(q)!=PGRES_TUPLES_OK){
            PGresult *rollback=PQexec(writer.copy[0],"ROLLBACK");PQclear(rollback);
            merge_failure(writer.copy[0],writer.entity,writer.path,PQresultErrorMessage(q));PQclear(q);exit(1);
        }
        st->ent_rows+=strtoull(PQgetvalue(q,0,0),NULL,10);st->phy_rows+=strtoull(PQgetvalue(q,0,1),NULL,10);PQclear(q);
        statement(writer.copy[0],"COMMIT");
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
    /* The final registered trunk includes unchanged files too. Native owns
     * traversal and testimony reconstruction; SQL only combines affected IDs
     * and invokes the standing fold in the witness registration transaction. */
    snprintf(sql,sizeof sql,
        "DELETE FROM %s;"
        "WITH claims AS (SELECT claim FROM %s UNION"
        " SELECT claim FROM laplace_contained_series(ARRAY(SELECT id FROM %s)))"
        " SELECT laplace_attest_series('%s'::regclass,array_agg(claim)) FROM claims",
        writer.pending,writer.affected,writer.witnesses,writer.pending);
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
    PGresult *result=PQexecParams(pg,"SELECT * FROM laplace_recoordinate($1::boolean)",1,NULL,arguments,NULL,NULL,0);
    if(PQresultStatus(result)!=PGRES_TUPLES_OK || PQntuples(result)!=1 || PQnfields(result)!=2){
        fprintf(stderr,"projection %s: %s",apply?"repair":"audit",PQerrorMessage(pg));
        PQclear(result);PQfinish(pg);return 1;
    }
    printf("projection %s: %s entity coordinates, %s path Hilbert keys %s\n",
        apply?"applied":"audit",PQgetvalue(result,0,0),PQgetvalue(result,0,1),apply?"corrected":"need correction");
    PQclear(result);PQfinish(pg);return 0;
}
