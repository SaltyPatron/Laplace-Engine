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
            uint64_t run = event->run ? event->run : 1, played = event->games ? run : 1;   /* a stated count is games; a record's repeat, tokens */
            if (s->games > UINT64_C(9007199254740992) - played) { fprintf(stderr,"series exceeds exact Native game range\n"); exit(1); }
            /* Map slots are stable while a window grows. The database retains
             * totals; the client releases this map after every committed window. */
            if(!s->dirty)lp_push(&f->dirty,index);
            s->games += played; s->tokens += run;
            s->sum+=lp_score_carried(event->score)*(double)played;
            if(event->position && (!s->position || event->position < s->position)) s->position = event->position;
            s->dirty=1;
        }
    }
}
/* The one accumulation of observations into the pending series: whatever is staged in writer.delta (a window's
 * events, or a recorded file's restored testimony) is folded by laplace_accumulate_series, its claims marked affected,
 * and the delta emptied. Runs inside the caller's transaction. */
void series_delta_apply(void){
    char sql[512]; Args a={0}; arg_text(&a,writer.pending); arg_text(&a,writer.delta);
    PGresult *q=ask(writer.series,"SELECT @extschema@.laplace_accumulate_series($1::regclass,$2::regclass)",&a);
    writer.series_changed |= lp_be(PQgetvalue(q,0,0),8)!=0; PQclear(q); args_free(&a);
    snprintf(sql,sizeof sql,"INSERT INTO %s SELECT DISTINCT claim FROM %s ON CONFLICT DO NOTHING;TRUNCATE %s",
        writer.affected,writer.delta,writer.delta);
    statement(writer.series,sql);
}
void flush_series(Fold *f){    if (!f || !f->dirty.n) return;
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
    series_delta_apply(); statement(writer.series,"COMMIT");
    lp_strmap_free(f->series);f->series=lp_strmap_sized(sizeof(Series));
    f->dirty.n=0;
}

