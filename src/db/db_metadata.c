/* Stage loader lifecycle, indexed presence queries, metadata key replay, and observation series accumulation. */
#include "db_internal.h"
Fold *active_fold;
Loader writer;
int load_whole;
void start_loader(const char *conninfo, int requested){
    if (writer.series) return;
    writer.from_ingest = active_fold != NULL;
    PGconn *control = db_connect(conninfo);
    PGresult *q = db_call(control, LP_SQL_POOL_SLOTS, NULL);
    if (PQntuples(q) != 1 || PQgetisnull(q, 0, 0) || PQgetisnull(q, 0, 1) || PQgetisnull(q, 0, 2)) {
        fputs("ingest connections: invalid pool check result\n", stderr); PQclear(q); PQfinish(control); exit(1);
    }
    if (!col_bool(q, 0, 1)) {
        fputs("ingest requires the installed Native observation-window API (laplace 1.44)\n", stderr);
        PQclear(q); PQfinish(control); exit(1);
    }
    writer.window_index = col_bool(q, 0, 2);
    writer.bucket_publish = (PQnfields(q) > 3 && !PQgetisnull(q, 0, 3)) ? col_bool(q, 0, 3) : 0;
    int available = col_i32(q, 0, 0); PQclear(q);
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
    db_exec_contract(control, LP_SQL_ENSURE_STAGE, NULL);
    db_exec_contract(control, LP_SQL_STAGE_RETIRE, NULL);
    q = db_call(control, LP_SQL_CURRENT_XACT, NULL);
    if (PQntuples(q) != 1 || PQgetisnull(q, 0, 0)) { fputs("ingest identity: invalid transaction id\n", stderr); PQclear(q); exit(1); }
    int64_t xid = col_i64(q, 0, 0);
    char suffix[40]; snprintf(suffix, sizeof suffix, "%lld", (long long)xid); PQclear(q);
    for (int lane=0; lane<writer.ncopy; lane++) {
        snprintf(writer.entity[lane], sizeof writer.entity[lane], "stage.lp_entity_%s_%d", suffix, lane);
        snprintf(writer.path[lane], sizeof writer.path[lane], "stage.lp_path_%s_%d", suffix, lane);
    }
    snprintf(writer.delta, sizeof writer.delta, "stage.lp_delta_%s", suffix);
    snprintf(writer.pending, sizeof writer.pending, "stage.lp_pending_%s", suffix);
    snprintf(writer.witnesses, sizeof writer.witnesses, "stage.lp_witness_%s", suffix);
    snprintf(writer.affected, sizeof writer.affected, "stage.lp_affected_%s", suffix);
    db_tx(control, LP_SQL_TX_BEGIN);
    for (int lane=0; lane<writer.ncopy; lane++) {
        if (db_stage_create_pair(control, writer.entity[lane], writer.path[lane]) != 0) exit(1);
    }
    if (db_stage_create_series(control, writer.delta, writer.pending, writer.witnesses, writer.affected) != 0) exit(1);
    /* Creation and the durable ownership marker become visible together.
     * The control connection holds the session lease across all COPY/source
     * transactions. A later load may reclaim these objects only after it exits. */
    size_t nowned = (size_t)writer.ncopy * 2u + 4u;
    const char **owned = malloc(nowned * sizeof *owned);
    if (!owned) { perror("stage ownership"); exit(1); }
    size_t k = 0;
    for (int lane = 0; lane < writer.ncopy; lane++) {
        owned[k++] = writer.entity[lane];
        owned[k++] = writer.path[lane];
    }
    const char *extra[] = {writer.delta, writer.pending, writer.witnesses, writer.affected};
    for (size_t i = 0; i < sizeof extra / sizeof *extra; i++) owned[k++] = extra[i];
    Args ownership = {0};
    arg_i64(&ownership, xid);
    arg_text_array(&ownership, owned, nowned);
    db_exec_contract(control, LP_SQL_STAGE_OWN, &ownership);
    args_free(&ownership);
    free(owned);
    db_tx(control, LP_SQL_TX_COMMIT);
    writer.known = lp_idmap_sized(sizeof(Published));
    writer.series = db_connect(conninfo);
}
void load_begin(const char *conninfo,int npg){start_loader(conninfo,npg);writer.from_ingest=1;}

/* A complete tier frontier is one Native indexed presence request. */
uint8_t *recorded(PGconn **pg,int npg,const lp_id *ids,uint64_t n){
    (void)npg;uint8_t *hit=calloc(n?(size_t)n:1,1);if(!n)return hit;
    Args input={0};arg_ids(&input,ids,(size_t)n);
    PGresult *result=db_call(pg[0],LP_SQL_PRESENT_IDS,&input);
    for(int row=0;row<PQntuples(result);row++){
        int64_t ordinal=col_i64(result,row,0);
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
        PGresult *q = db_call(pg[0], LP_SQL_FILES_KNOWN, &a);
        lp_idmap *recorded = lp_idmap_new();
        for (int row = 0; row < PQntuples(q); row++) { lp_id id = *col_id(q, row, 0); bool fresh; lp_idmap_put(recorded, &id, &fresh); }
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
            if (s->t.tokens > 9007199254740992.0 - (double)run) { fprintf(stderr,"series exceeds exact Native game range\n"); exit(1); }
            /* Map slots are stable while a window grows. The database retains
             * totals; the client releases this map after every committed window. */
            if(!s->dirty)lp_push(&f->dirty,index);
            lp_tally_add(&s->t, (double)played, (double)run, lp_score_carried(event->score)*(double)played, event->position);
            s->dirty=1;
        }
    }
}
/* The one accumulation of observations into the pending series: whatever is staged in writer.delta (a window's
 * events, or a recorded file's restored testimony) is folded by laplace_accumulate_series, its claims marked affected,
 * and the delta emptied. Runs inside the caller's transaction. */
void series_delta_apply(void){
    Args a={0}; arg_text(&a,writer.pending); arg_text(&a,writer.delta); arg_text(&a,writer.affected);
    PGresult *q=db_call(writer.series,LP_SQL_STAGE_FLUSH_CLAIMS,&a);
    writer.series_changed |= col_i64(q,0,0)!=0;
    PQclear(q); args_free(&a);
}
void flush_series(Fold *f){    if (!f || !f->dirty.n) return;
    lp_copy copy={0};
    db_copy_open_stage_delta(&copy,writer.series,writer.delta);
    for (size_t i=0;i<f->dirty.n;i++) {
        Series *s=lp_strmap_at(f->series,f->dirty.v[i]);
        lp_copy_row(&copy,9); lp_copy_id(&copy,&s->claim); lp_copy_id(&copy,&s->witness);
        if(s->has_lineage) lp_copy_id(&copy,&s->lineage); else lp_copy_null(&copy);
        lp_copy_i64(&copy,(int64_t)s->t.games); lp_copy_i64(&copy,(int64_t)(s->t.tokens >= s->t.games ? s->t.tokens : s->t.games));
        lp_copy_f64(&copy,s->t.wins/s->t.games);
        if(s->t.position) lp_copy_i32(&copy,(int32_t)s->t.position); else lp_copy_null(&copy);
        lp_copy_f64(&copy,s->trust);
        lp_copy_f64(&copy,s->t.wins);
    }
    copy_close(&copy); lp_buf_free(&copy.buf);
    series_delta_apply(); db_tx(writer.series, LP_SQL_TX_COMMIT);
    lp_strmap_free(f->series);f->series=lp_strmap_sized(sizeof(Series));
    f->dirty.n=0;
}

