/* Stage loader lifecycle, indexed presence queries, metadata key replay, and observation series accumulation. */
#include "db_internal.h"
Fold *active_fold;
Loader writer;
int load_whole;
void start_loader(const char *conninfo, int requested){
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
        "CREATE UNLOGGED TABLE %s (id @extschema@.blake3 PRIMARY KEY, lineage @extschema@.blake3, trust double precision NOT NULL);"
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
uint8_t *recorded(PGconn **pg,int npg,const lp_id *ids,uint64_t n){
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
        /* A file of no source whose recipe names no class is content, not a witness: it attests nothing. */
        if (!file->source && file->trust == 0.0) continue;
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
            s->games++; s->tokens += event->run ? event->run : 1;
            s->sum+=lp_score_carried(event->score);
            if(event->position && (!s->position || event->position < s->position)) s->position = event->position;
            s->dirty=1;
        }
    }
}
void flush_series(Fold *f){
    if (!f || !f->dirty.n) return;
    lp_copy copy={0}; char sql[512];
    snprintf(sql,sizeof sql,"BEGIN;COPY %s (claim,witness,lineage,games,tokens,score,\"position\",trust,wins) FROM STDIN (FORMAT binary)",writer.delta);
    copy_open(&copy,writer.series,sql);
    for (size_t i=0;i<f->dirty.n;i++) {
        Series *s=lp_strmap_at(f->series,f->dirty.v[i]);
        lp_copy_row(&copy,9); lp_copy_id(&copy,&s->claim); lp_copy_id(&copy,&s->witness);
        if(s->has_lineage) lp_copy_id(&copy,&s->lineage); else lp_copy_null(&copy);
        lp_copy_i64(&copy,(int64_t)s->games); lp_copy_i64(&copy,(int64_t)(s->tokens >= s->games ? s->tokens : s->games));
        lp_copy_f64(&copy,s->sum/(double)s->games);
        if(s->position) lp_copy_i32(&copy,(int32_t)s->position); else lp_copy_null(&copy);
        lp_copy_f64(&copy,s->trust);
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

