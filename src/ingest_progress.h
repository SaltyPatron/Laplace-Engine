/* Optional operational progress for a supervising application. Content,
 * identity and evidence remain owned by the ordinary ingestion pipeline. */
static struct {
    FILE *out;
    File *files;
    unsigned char *state;
    int total, done;
    uint64_t bytes;
    LoadStats *stats;
    const char *source;
} ingest_progress;

static void progress_write(const char *event, File *file, const lp_id *source_id) {
    if (!ingest_progress.out) return;
    FILE *out = ingest_progress.out;
    fputs("{\"event\":", out); json_str(out, event);
    if (ingest_progress.source) { fputs(",\"source\":", out); json_str(out, ingest_progress.source); }
    fprintf(out, ",\"files_total\":%d,\"files_done\":%d,\"bytes_done\":%llu,\"entities\":%llu,\"physicalities\":%llu",
        ingest_progress.total, ingest_progress.done,
        (unsigned long long)ingest_progress.bytes,
        (unsigned long long)ingest_progress.stats->ent_rows,
        (unsigned long long)ingest_progress.stats->phy_rows);
    if (file) {
        fputs(",\"file\":", out); json_str(out, file->path);
        fprintf(out, ",\"file_bytes\":%llu,\"records\":%llu",
            (unsigned long long)file->bytes, (unsigned long long)file->records);
        if (file->has_file) { char id[33]; id_text(&file->file.id, id);
            fputs(",\"file_id\":", out); json_str(out, id); }
    }
    if (source_id) { char id[33]; id_text(source_id, id);
        fputs(",\"source_id\":", out); json_str(out, id); }
    fputs("}\n", out); fflush(out);
}

static int progress_open(File *files, int nfiles, LoadStats *stats) {
    const char *path = getenv("LAPLACE_INGEST_PROGRESS_PATH");
    if (!path || !*path) return 0;
    ingest_progress.out = fopen(path, "ab");
    if (!ingest_progress.out) { perror("ingest progress"); return 1; }
    ingest_progress.files = files; ingest_progress.stats = stats;
    ingest_progress.state = calloc((size_t)nfiles, 1);
    if (nfiles && !ingest_progress.state) { perror("ingest progress state"); return 1; }
    for (int i = 0; i < nfiles; i++) if (!files[i].skipped) {
        if (!ingest_progress.source && files[i].source) ingest_progress.source = files[i].source->name;
        ingest_progress.total++;
        if (files[i].known) { ingest_progress.state[i] = 2; ingest_progress.done++; }
    }
    progress_write("inventory", NULL, NULL);
    for (int i = 0; i < nfiles; i++) if (!files[i].skipped && files[i].known)
        progress_write("file-known", &files[i], NULL);
    return 0;
}

static void progress_file(const char *event, File *file) {
    if (!ingest_progress.out || file->skipped || file->known) return;
    #pragma omp critical(ingest_progress_write)
    {
        unsigned char *state = &ingest_progress.state[file - ingest_progress.files];
        if (!strcmp(event, "file-start")) {
            if (!*state) { *state = 1; progress_write(event, file, NULL); }
        } else if (!strcmp(event, "file-composed")) {
            /* A curated file can be fully read without producing content,
             * as with Unicode's comments-only NamedSequencesProv.txt. */
            if (*state != 2 && !file->partial) {
                *state = 2; ingest_progress.done++; ingest_progress.bytes += file->bytes;
                progress_write(event, file, NULL);
            }
        } else progress_write(event, file, NULL);
    }
}

static void progress_close(const lp_id *source_id) {
    if (!ingest_progress.out) return;
    progress_write("complete", NULL, source_id);
    fclose(ingest_progress.out); free(ingest_progress.state);
    memset(&ingest_progress, 0, sizeof ingest_progress);
}
