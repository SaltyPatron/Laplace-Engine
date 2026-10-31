/* The client composes and prunes; PostgreSQL merges whole COPY windows.
 * Structural and observation windows commit privately as they flow. Only
 * canonical witness registration and authoritative standings share the final
 * publication transaction; parsing never retains consensus bucket locks. */
#include "engine.h"
#include "laplace/tier0.h"
#include "laplace/highway.h"
#include "laplace/geometry.h"
#include <omp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

typedef struct { lp_id claim, witness, lineage; uint64_t games; double sum, trust; uint8_t has_lineage, dirty; } Series;
struct Fold { lp_strmap *series; lp_vec(size_t) dirty; };
static Fold *active_fold;
typedef struct { lp_mask lexical; uint8_t tier,kind,metadata; } Published;
typedef struct {
    PGconn **copy, *series; int ncopy, from_ingest, witness_changed, series_changed, window_index;
    char (*entity)[96], (*path)[96];
    char delta[96], pending[96], witnesses[96], affected[96];
    lp_idmap *known;
    lp_idmap *registered, *new_witnesses;
    int counts_unknown;
} Loader;
static Loader writer;
int load_whole;

static void statement(PGconn *pg, const char *sql){ exec(pg, sql); }
static void start_loader(const char *conninfo, int requested){
    if (writer.series) return;
    writer.from_ingest = active_fold != NULL;
    PGconn *control = db_connect(conninfo);
    PGresult *q = db_exec(control, "SELECT greatest(1, current_setting('max_connections')::int"
        " - current_setting('superuser_reserved_connections')::int"
        " - current_setting('reserved_connections')::int - (SELECT count(*) FROM pg_stat_activity) - 4),"
        " to_regprocedure('@extschema@.laplace_stage_own(bigint,regclass[])') IS NOT NULL"
        " AND to_regprocedure('@extschema@.laplace_accumulate_series(regclass,regclass)') IS NOT NULL"
        " AND to_regprocedure('@extschema@.laplace_attest_stage(regclass,regclass)') IS NOT NULL"
        " AND to_regprocedure('@extschema@.laplace_present_stage(regclass,regclass,name,name)') IS NOT NULL,"
        " to_regprocedure('@extschema@.laplace_merge_stage(regclass,regclass,boolean)') IS NOT NULL"
        " AND to_regprocedure('@extschema@.laplace_stage_partitions(regclass)') IS NOT NULL"
        " AND to_regprocedure('@extschema@.laplace_clean_partitions(oid[])') IS NOT NULL");
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "ingest connections: %s", PQerrorMessage(control)); exit(1); }
    if(strcmp(PQgetvalue(q,0,1),"t")){fputs("ingest requires the installed Native observation-window API (laplace 1.44)\n",stderr);PQclear(q);PQfinish(control);exit(1);}
    writer.window_index=!strcmp(PQgetvalue(q,0,2),"t");
    int available = atoi(PQgetvalue(q, 0, 0)); PQclear(q);
    int cores = omp_get_max_threads();
    if (requested <= 0 || requested > cores) requested = cores;
    const char *pool = getenv("LAPLACE_DB_POOL_SIZE");
    if (pool && atoi(pool) > 0 && requested > atoi(pool)) requested = atoi(pool);
    writer.ncopy = requested < available ? requested : available;
    if (writer.ncopy < 1) writer.ncopy = 1;
    writer.copy = calloc((size_t)writer.ncopy, sizeof *writer.copy);
    writer.entity = calloc((size_t)writer.ncopy, sizeof *writer.entity);
    writer.path = calloc((size_t)writer.ncopy, sizeof *writer.path);
    if (!writer.copy || !writer.entity || !writer.path || writer.ncopy > UINT16_MAX) {
        fputs("ingest: cannot allocate bounded publication lanes\n", stderr); exit(1);
    }
    writer.copy[0] = control;
    for (int i = 1; i < writer.ncopy; i++) writer.copy[i] = db_connect(conninfo);
    statement(control, "SELECT @extschema@.laplace_ensure_stage()");
    statement(control, "SELECT * FROM @extschema@.laplace_stage_retire()");
    q = db_exec(control, "SELECT pg_current_xact_id()::text");
    if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "ingest identity: %s", PQerrorMessage(control)); exit(1); }
    char suffix[40]; snprintf(suffix, sizeof suffix, "%s", PQgetvalue(q, 0, 0)); PQclear(q);
    for (int lane=0; lane<writer.ncopy; lane++) {
        snprintf(writer.entity[lane], sizeof writer.entity[lane], "stage.lp_entity_%s_%d", suffix, lane);
        snprintf(writer.path[lane], sizeof writer.path[lane], "stage.lp_path_%s_%d", suffix, lane);
    }
    snprintf(writer.delta, sizeof writer.delta, "stage.lp_delta_%s", suffix);
    snprintf(writer.pending, sizeof writer.pending, "stage.lp_pending_%s", suffix);
    snprintf(writer.witnesses, sizeof writer.witnesses, "stage.lp_witness_%s", suffix);
    snprintf(writer.affected, sizeof writer.affected, "stage.lp_affected_%s", suffix);
    char sql[1800];
    statement(control, "BEGIN");
    for (int lane=0; lane<writer.ncopy; lane++) {
        snprintf(sql, sizeof sql,
            "CREATE UNLOGGED TABLE %s (LIKE stage.entity INCLUDING DEFAULTS,present boolean NOT NULL);"
            "ALTER TABLE %s ALTER coord DROP NOT NULL,ALTER hilbert DROP NOT NULL;"
            "CREATE UNLOGGED TABLE %s (LIKE stage.physicality INCLUDING DEFAULTS,present boolean NOT NULL);"
            "ALTER TABLE %s ALTER path DROP NOT NULL,ALTER hilbert DROP NOT NULL",
            writer.entity[lane],writer.entity[lane],writer.path[lane],writer.path[lane]);
        statement(control, sql);
    }
    snprintf(sql, sizeof sql,
        "CREATE UNLOGGED TABLE %s (LIKE stage.attestation INCLUDING DEFAULTS, wins double precision NOT NULL);"
        "CREATE UNLOGGED TABLE %s (LIKE stage.attestation INCLUDING DEFAULTS, wins double precision NOT NULL,"
        " CHECK(games>0 AND games<=9007199254740992 AND wins>=0 AND wins<=games));"
        "CREATE UNIQUE INDEX ON %s (claim,witness,lineage) NULLS NOT DISTINCT;"
        "CREATE UNLOGGED TABLE %s (LIKE @extschema@.witness INCLUDING DEFAULTS);"
        "CREATE UNLOGGED TABLE %s (claim @extschema@.blake3 PRIMARY KEY)",
        writer.delta,writer.pending,writer.pending,writer.witnesses,writer.affected);
    statement(control, sql);
    /* Creation and the durable ownership marker become visible together.
     * The control connection holds the session lease across all COPY/source
     * transactions. A later load may reclaim these objects only after it exits. */
    lp_buf owned={0};lp_buf_put(&owned,"{",1);
    for(int lane=0;lane<writer.ncopy;lane++){
        if(lane)lp_buf_put(&owned,",",1);
        lp_buf_put(&owned,writer.entity[lane],strlen(writer.entity[lane]));lp_buf_put(&owned,",",1);
        lp_buf_put(&owned,writer.path[lane],strlen(writer.path[lane]));
    }
    const char *extra[]={writer.delta,writer.pending,writer.witnesses,writer.affected};
    for(size_t i=0;i<sizeof extra/sizeof *extra;i++){lp_buf_put(&owned,",",1);lp_buf_put(&owned,extra[i],strlen(extra[i]));}
    lp_buf_put(&owned,"}",2);
    Args ownership={0};arg_text(&ownership,suffix);arg_text(&ownership,(const char *)owned.b);
    PQclear(ask_once(control,"SELECT @extschema@.laplace_stage_own($1::bigint,$2::regclass[])",&ownership));
    args_free(&ownership);lp_buf_free(&owned);statement(control,"COMMIT");
    writer.known = lp_idmap_sized(sizeof(Published));
    writer.series = db_connect(conninfo);
}
void load_begin(const char *conninfo,int npg){start_loader(conninfo,npg);writer.from_ingest=1;}

/* A complete tier frontier is one Native indexed presence request. */
static uint8_t *recorded(PGconn **pg,int npg,const lp_id *ids,uint64_t n){
    (void)npg;uint8_t *hit=calloc(n?(size_t)n:1,1);if(!n)return hit;
    Args input={0};arg_ids(&input,ids,(size_t)n);
    PGresult *result=ask_once(pg[0],"SELECT ordinal FROM @extschema@.laplace_present_ids($1::@extschema@.blake3[])",&input);
    for(int row=0;row<PQntuples(result);row++){
        int64_t ordinal=(int64_t)lp_be(PQgetvalue(result,row,0),8);
        if(ordinal>0 && (uint64_t)ordinal<=n)hit[(size_t)ordinal-1]=1;
    }
    PQclear(result);args_free(&input);return hit;
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
            "SELECT wanted.id FROM unnest($1::@extschema@.blake3[]) wanted(id)"
            " WHERE (SELECT true FROM @extschema@.entity e WHERE e.id OPERATOR(@extschema@.=) wanted.id)"
            " AND (SELECT true FROM @extschema@.physicality p WHERE p.entity OPERATOR(@extschema@.=) wanted.id)", &a);
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

/* Metadata is an ordered Native DAG, not a recursive SQL relation expansion.
 * Both context checks and compiled-key restore use these same set frontiers.
 * Tagged entries terminate traversal: their referenced product is never read. */
typedef struct { lp_id parts[5]; } MetadataEntry;
typedef struct { size_t first,count,entry; uint64_t visited; uint8_t terminal; } MetadataNode;
typedef struct {
    lp_idmap *nodes; lp_vec(size_t) children; lp_vec(MetadataEntry) entries;
    lp_id markers[2]; size_t arity; uint64_t generation;
} MetadataTree;
static size_t metadata_node(MetadataTree *tree,const lp_id *id){
    bool fresh;size_t index=lp_idmap_put(tree->nodes,id,&fresh);
    if(fresh)*(MetadataNode *)lp_idmap_at(tree->nodes,index)=(MetadataNode){.entry=SIZE_MAX};
    return index;
}
static MetadataTree metadata_read(PGconn *pg,const lp_id *roots,size_t count,size_t arity){
    MetadataTree tree={.nodes=lp_idmap_sized(sizeof(MetadataNode)),.arity=arity};
    tree.markers[0]=text_ref(CTX[0],(const uint8_t *)KEY_CONTEXT_TAG,sizeof KEY_CONTEXT_TAG-1).id;
    tree.markers[1]=text_ref(CTX[0],(const uint8_t *)KEY_BINDING_TAG,sizeof KEY_BINDING_TAG-1).id;
    for(size_t i=0;i<count;i++)metadata_node(&tree,roots+i);
    size_t begin=0,end=lp_idmap_count(tree.nodes);
    /* Match COPY's 4 MiB transport grain; this windows the complete input. */
    const size_t grain=DB_ID_WINDOW;
    while(begin<end){
        for(size_t at=begin;at<end;){
            size_t stop=at+((end-at)<grain?end-at:grain);lp_vec(lp_id) wanted={0};
            for(;at<stop;at++){
                const lp_id *id=lp_idmap_key(tree.nodes,at);
                if(lp_tier0_codepoint(T0,id)<0)lp_push(&wanted,*id);
            }
            if(!wanted.n){lp_vec_free(&wanted);continue;}
            Args args={0};arg_ids(&args,wanted.v,wanted.n);
            PGresult *result=ask(pg,"SELECT entity,path FROM @extschema@.laplace_paths($1::@extschema@.blake3[])",&args);
            for(int row=0;row<PQntuples(result);row++){
                lp_id id=*col_id(result,row,0);int64_t index=lp_idmap_find(tree.nodes,&id);if(index<0)continue;
                lp_path path=col_path(result,row,1);MetadataNode node={.first=tree.children.n,.entry=SIZE_MAX};
                lp_id first=path.n?lp_path_id(path,0):(lp_id){{0}};
                node.terminal=path.n && (lp_id_eq(&first,tree.markers) || lp_id_eq(&first,tree.markers+1));
                if(node.terminal){
                    if(lp_path_len(path)==arity){
                        MetadataEntry entry={0};size_t expanded=0;
                        for(size_t k=0;k<path.n;k++){
                            lp_id child=lp_path_id(path,k);uint32_t run=lp_path_run(path,k);
                            for(uint32_t r=0;r<run;r++)entry.parts[expanded++]=child;
                        }
                        node.entry=tree.entries.n;lp_push(&tree.entries,entry);
                    }
                }else for(size_t k=0;k<path.n;k++){
                    lp_id child=lp_path_id(path,k);size_t next=metadata_node(&tree,&child);lp_push(&tree.children,next);node.count++;
                }
                *(MetadataNode *)lp_idmap_at(tree.nodes,(size_t)index)=node;
            }
            PQclear(result);args_free(&args);lp_vec_free(&wanted);
        }
        begin=end;end=lp_idmap_count(tree.nodes);
    }
    return tree;
}
typedef void (*MetadataVisit)(const MetadataEntry *,void *);
static void metadata_visit(MetadataTree *tree,const lp_id *root,MetadataVisit visit,void *context){
    int64_t first=lp_idmap_find(tree->nodes,root);if(first<0)return;
    uint64_t generation=++tree->generation;lp_vec(size_t) pending={0};lp_push(&pending,(size_t)first);
    while(pending.n){
        MetadataNode *node=lp_idmap_at(tree->nodes,pending.v[--pending.n]);
        if(node->visited==generation)continue;node->visited=generation;
        if(node->entry!=SIZE_MAX)visit(tree->entries.v+node->entry,context);
        if(!node->terminal)for(size_t k=0;k<node->count;k++)lp_push(&pending,tree->children.v[node->first+k]);
    }
    lp_vec_free(&pending);
}
static void metadata_free(MetadataTree *tree){
    lp_idmap_free(tree->nodes);lp_vec_free(&tree->children);lp_vec_free(&tree->entries);
}
typedef struct {const lp_id *tag;lp_id expected;int invalid;} MetadataContext;
static void metadata_context(const MetadataEntry *entry,void *context){
    MetadataContext *reading=context;
    if(lp_id_eq(entry->parts,reading->tag) && !lp_id_eq(entry->parts+1,&reading->expected))reading->invalid=1;
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
    lp_id *roots=malloc(count*sizeof *roots);if(!roots){perror("recorded reading contexts");exit(1);}
    size_t at=0;for(int i=0;i<nfiles;i++)if(files[i].known)roots[at++]=files[i].recorded_metadata;
    PGconn *pg=db_connect(conninfo);MetadataTree tree=metadata_read(pg,roots,count,2);
    for(int i=0;i<nfiles;i++)if(files[i].known){
        MetadataContext reading={.tag=tree.markers,.expected=file_context_value(files+i).id};
        metadata_visit(&tree,&files[i].recorded_metadata,metadata_context,&reading);
        if(reading.invalid)files[i].known=files[i].has_file=0;
    }
    metadata_free(&tree);PQfinish(pg);free(roots);
}
/* Restore only the compiled index beneath a recorded file's metadata. Entry
 * vertices terminate the walk: their product targets are not exported into RAM.
 * One set read restores a provider; none of its source bytes are reopened. */
typedef struct {const lp_id *tag;lp_vec(MetadataEntry) entries;} MetadataBindings;
static void metadata_bindings(const MetadataEntry *entry,void *context){
    MetadataBindings *bindings=context;
    if(lp_id_eq(entry->parts,bindings->tag))lp_push(&bindings->entries,*entry);
}
int load_file_keys(PGconn *pg, const File *file){
    if (!file->known || !file->has_file) return 0;
    MetadataTree tree=metadata_read(pg,&file->recorded_metadata,1,5);
    MetadataBindings bindings={.tag=tree.markers+1};
    metadata_visit(&tree,&file->recorded_metadata,metadata_bindings,&bindings);
    size_t count=bindings.entries.n;
    lp_id *ids=malloc((count?count:1)*4*sizeof *ids);if(!ids){perror("recorded reference bindings");exit(1);}
    for(size_t row=0;row<count;row++)for(size_t col=0;col<4;col++)ids[col*count+row]=bindings.entries.v[row].parts[col+1];
    metadata_free(&tree);lp_vec_free(&bindings.entries);
    Args args={0};PGresult *result;int invalid=0;
    if(!count){free(ids);fprintf(stderr,"  restored 0 compiled reference bindings from recorded metadata: %s\n",file->path);return 0;}
    for (unsigned col=0;col<4;col++) arg_ids(&args,ids+col*count,count);
    result=ask_once(pg,
        "SELECT @extschema@.laplace_text(b.hash),@extschema@.laplace_text(b.header),b.value,@postgis@.st_asewkb(e.coord),e.tier,@extschema@.laplace_text(b.text)"
        " FROM unnest($1::@extschema@.blake3[],$2::@extschema@.blake3[],$3::@extschema@.blake3[],$4::@extschema@.blake3[]) b(hash,header,value,text)"
        " LEFT JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) b.value",&args);
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
    PQclear(result); args_free(&args); return invalid;
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
            uint8_t key[49]={0}; memcpy(key,event->claim.b,16); memcpy(key+16,source.id.b,16); key[48]=has_lineage;
            if (has_lineage) memcpy(key+32,lineage.b,16);
            bool fresh; size_t index=lp_strmap_put(f->series,key,sizeof key,&fresh);
            Series *s=lp_strmap_at(f->series,index);
            if (fresh) *s=(Series){.claim=event->claim,.witness=source.id,.lineage=lineage,
                .trust=observation_trust,.has_lineage=has_lineage};
            if (s->games==UINT64_C(9007199254740992)) { fprintf(stderr,"series exceeds exact Native game range\n"); exit(1); }
            /* Map slots are stable while a window grows. The database retains
             * totals; the client releases this map after every committed window. */
            if(!s->dirty)lp_push(&f->dirty,index);
            s->games++; s->sum+=lp_score_carried(event->score); s->dirty=1;
        }
    }
}
static void flush_series(Fold *f){
    if (!f || !f->dirty.n) return;
    lp_copy copy={0}; char sql[512];
    snprintf(sql,sizeof sql,"BEGIN;COPY %s (claim,witness,lineage,games,score,trust,wins) FROM STDIN (FORMAT binary)",writer.delta);
    copy_open(&copy,writer.series,sql);
    for (size_t i=0;i<f->dirty.n;i++) {
        Series *s=lp_strmap_at(f->series,f->dirty.v[i]);
        lp_copy_row(&copy,7); lp_copy_id(&copy,&s->claim); lp_copy_id(&copy,&s->witness);
        if(s->has_lineage) lp_copy_id(&copy,&s->lineage); else lp_copy_null(&copy);
        lp_copy_i64(&copy,(int64_t)s->games); lp_copy_f64(&copy,s->sum/(double)s->games); lp_copy_f64(&copy,s->trust);
        lp_copy_f64(&copy,s->sum);
    }
    copy_close(&copy); lp_buf_free(&copy.buf);
    Args a={0}; arg_text(&a,writer.pending); arg_text(&a,writer.delta);
    PGresult *q=ask(writer.series,"SELECT @extschema@.laplace_accumulate_series($1::regclass,$2::regclass)",&a);
    writer.series_changed |= lp_be(PQgetvalue(q,0,0),8)!=0; PQclear(q); args_free(&a);
    snprintf(sql,sizeof sql,"INSERT INTO %s SELECT DISTINCT claim FROM %s ON CONFLICT DO NOTHING;TRUNCATE %s;COMMIT",
        writer.affected,writer.delta,writer.delta);
    statement(writer.series,sql);
    lp_strmap_free(f->series);f->series=lp_strmap_sized(sizeof(Series));
    f->dirty.n=0;
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
    uint64_t wanted=payload/((uint64_t)1<<22);
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
    lp_vec_free(&selected);TABLE_EACH(x)if(x->keep==1 || x->keep==2)lp_idmap_put(writer.known,&x->id,NULL);
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
    PGresult *existing=ask(writer.series,"SELECT id,attestations FROM @extschema@.witness WHERE id OPERATOR(@extschema@.=) ANY($1)",&existing_args);
    lp_idmap *known_witnesses=lp_idmap_new();
    for(int i=0;i<PQntuples(existing);i++){
        lp_idmap_put(known_witnesses,col_id(existing,i,0),NULL);
        if(PQgetisnull(existing,i,1))writer.counts_unknown=1;
    }
    for(size_t i=0;i<count;i++)if(lp_idmap_find(known_witnesses,&roots[i].id)<0)lp_idmap_put(writer.new_witnesses,&roots[i].id,NULL);
    lp_idmap_free(known_witnesses);PQclear(existing);args_free(&existing_args);
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
        "WITH updated AS (UPDATE @extschema@.witness w SET lineage=s.lineage,trust=s.trust FROM %s s WHERE w.id OPERATOR(@extschema@.=) s.id"
        " AND (NOT coalesce(w.lineage OPERATOR(@extschema@.=) s.lineage,w.lineage IS NULL AND s.lineage IS NULL)"
        " OR w.trust IS DISTINCT FROM s.trust) RETURNING w.id),"
        " inserted AS (INSERT INTO @extschema@.witness (id,lineage,trust) SELECT DISTINCT s.id,s.lineage,s.trust FROM %s s"
        " WHERE NOT EXISTS(SELECT FROM @extschema@.witness w WHERE w.id OPERATOR(@extschema@.=) s.id) RETURNING id)"
        " SELECT (SELECT count(*) FROM updated)+(SELECT count(*) FROM inserted)",writer.witnesses,writer.witnesses);
    PGresult *result=db_exec(writer.series,sql);
    if(PQresultStatus(result)!=PGRES_TUPLES_OK){fprintf(stderr,"witness registration: %s",PQerrorMessage(writer.series));exit(1);}
    writer.witness_changed |= strtoull(PQgetvalue(result,0,0),NULL,10) != 0;
    PQclear(result);
    if(writer.series_changed || writer.witness_changed || writer.counts_unknown){
        /* New assertions already supplied their exact claim IDs in each
         * committed window. Only pruned files need their stored testimony
         * restored; never walk the newly ingested corpus down to text again. */
        lp_idmap *reused=lp_idmap_new();
        for(int i=0;i<nfiles;i++)if(files[i].known && !files[i].skipped && files[i].has_file)
            lp_idmap_put(reused,&files[i].file.id,NULL);
        if(lp_idmap_count(reused)){
            snprintf(sql,sizeof sql,"INSERT INTO %s SELECT DISTINCT claim FROM @extschema@.laplace_contained_series($1::@extschema@.blake3[]) ON CONFLICT DO NOTHING",writer.affected);
            Args args={0};arg_ids(&args,lp_idmap_keys(reused),lp_idmap_count(reused));
            PQclear(ask_once(writer.series,sql,&args));args_free(&args);
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
        Args roots={0};arg_ids(&roots,lp_idmap_keys(writer.registered),lp_idmap_count(writer.registered));
        /* One complete source publishes one witness. Its affected set already
         * contains every new claim and the claims restored from reused files.
         * Count assertions here, once per claim, never games or source totals. */
        if(lp_idmap_count(writer.registered)==1)
            snprintf(sql,sizeof sql,"UPDATE @extschema@.witness SET attestations=(SELECT count(*) FROM %s)"
                " WHERE id OPERATOR(@extschema@.=) ANY($1) RETURNING id,attestations",writer.affected);
        else snprintf(sql,sizeof sql,"WITH counts AS (SELECT trunk,count(DISTINCT claim)::bigint n FROM"
            " @extschema@.laplace_contained_series($1::@extschema@.blake3[]) GROUP BY trunk)"
            " UPDATE @extschema@.witness w SET attestations=coalesce((SELECT n FROM counts c WHERE c.trunk OPERATOR(@extschema@.=) w.id),0)"
            " WHERE w.id OPERATOR(@extschema@.=) ANY($1) RETURNING w.id,w.attestations");
        PGresult *measured=ask(writer.series,sql,&roots);
        for(int i=0;i<PQntuples(measured);i++)if(lp_idmap_find(writer.new_witnesses,col_id(measured,i,0))>=0){
            uint64_t n=(uint64_t)col_int(measured,i,1);
            if(n>UINT64_MAX-admitted){fprintf(stderr,"attestation counter overflow\n");return 1;}
            admitted+=n;
        }
        PQclear(measured);args_free(&roots);
    }
    /* Resolve only the affected claims through the indexed upward Native
     * provenance kernel, after the real trunk is registered. Private aliases
     * and event estimates never become independent public witnesses. */
    snprintf(sql,sizeof sql,
        "DELETE FROM %s;"
        "SELECT @extschema@.laplace_attest_stage('%s'::regclass,'%s'::regclass)",
        writer.pending,writer.pending,writer.affected);
    /* An unchanged registered source contributes no new evidence on replay.
     * Recompute only for new series or changed witness trust/containment. */
    if(writer.series_changed || writer.witness_changed) statement(writer.series,sql);
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
