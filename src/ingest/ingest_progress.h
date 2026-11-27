/* Optional operational progress for a supervising application. Content,
 * identity and evidence remain owned by the ordinary ingestion pipeline. */
#include <time.h>

static struct {
    FILE *out;
    File *files;
    unsigned char *state;
    int total, done;
    int explicit_out;
    uint64_t bytes;
    uint64_t bytes_total;
    double started_at;
    LoadStats *stats;
    const char *source;
    char run_id[64];
    char host[128];
    long pid;
} ingest_progress;

/* A supervising application renders the structured stream. Carriage-return
 * status belongs only to a terminal, never to redirected CI/service logs. */
static int progress_console(void){
    return !ingest_progress.explicit_out && isatty(fileno(stderr));
}

static void progress_mkdirs(const char *path) {
    char buf[4200];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *c = buf; *c; c++) {
        if (*c == '/' || *c == '\\') {
            if (c == buf) continue;
            if (c == buf + 2 && buf[1] == ':') continue;
            char saved = *c;
            *c = 0;
            mkdir(buf, 0775);
            *c = saved;
        }
    }
    mkdir(buf, 0775);
}

static void progress_iso_utc(char out[32]) {
    time_t sec = time(NULL);
    struct tm tm_utc;
#ifdef _WIN32
    gmtime_s(&tm_utc, &sec);
#else
    gmtime_r(&sec, &tm_utc);
#endif
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static uint64_t progress_splitmix64(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static void progress_make_uuid(char out[37]) {
    static uint64_t seq = 0;
    uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)(long)getpid() << 32)
                  ^ (uint64_t)(now() * 1e9) ^ (uint64_t)(uintptr_t)out ^ (++seq * UINT64_C(0x517cc1b727220a95));
    uint64_t hi = progress_splitmix64(&seed);
    uint64_t lo = progress_splitmix64(&seed);
    unsigned char b[16];
    for (int i = 0; i < 8; i++) {
        b[i]     = (unsigned char)(hi >> (56 - 8 * i));
        b[8 + i] = (unsigned char)(lo >> (56 - 8 * i));
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void progress_write(const char *event, File *file, const lp_id *source_id, const char *error) {
    if (!ingest_progress.out) return;
    FILE *out = ingest_progress.out;
    char ts[32];
    progress_iso_utc(ts);
    double elapsed_s = now() - ingest_progress.started_at;
    uint64_t elapsed_ms = elapsed_s > 0.0 ? (uint64_t)(elapsed_s * 1000.0) : 0;
    fputs("{\"event\":", out); lp_fput_json(out, event);
    fputs(",\"ts\":", out); lp_fput_json(out, ts);
    fputs(",\"run_id\":", out); lp_fput_json(out, ingest_progress.run_id);
    fprintf(out, ",\"pid\":%ld,\"host\":", ingest_progress.pid); lp_fput_json(out, ingest_progress.host);
    if (ingest_progress.source) { fputs(",\"source\":", out); lp_fput_json(out, ingest_progress.source); }
    fprintf(out, ",\"files_total\":%d,\"files_done\":%d,\"bytes_done\":%llu,\"bytes_total\":%llu,\"elapsed_ms\":%llu,\"entities\":%llu,\"physicalities\":%llu",
        ingest_progress.total, ingest_progress.done,
        (unsigned long long)ingest_progress.bytes,
        (unsigned long long)ingest_progress.bytes_total,
        (unsigned long long)elapsed_ms,
        (unsigned long long)ingest_progress.stats->ent_rows,
        (unsigned long long)ingest_progress.stats->phy_rows);
    fprintf(out, ",\"attestations\":%llu,\"attestations_measured\":%s",
        (unsigned long long)ingest_progress.stats->attestations,
        ingest_progress.stats->attestations_measured ? "true" : "false");
    if (file) {
        fputs(",\"file\":", out); lp_fput_json(out, file->path);
        fprintf(out, ",\"file_bytes\":%llu,\"records\":%llu",
            (unsigned long long)file->bytes, (unsigned long long)file->records);
        if (file->has_file) { char id[33]; id_text(&file->file.id, id);
            fputs(",\"file_id\":", out); lp_fput_json(out, id); }
    }
    if (source_id) { char id[33]; id_text(source_id, id);
        fputs(",\"source_id\":", out); lp_fput_json(out, id); }
    if (error && *error) {
        fputs(",\"error\":", out); lp_fput_json(out, error);
    }
    fputs("}\n", out); fflush(out);
}

static int progress_open(File *files, int nfiles, LoadStats *stats, const uint64_t *sizes, int do_load) {
    memset(&ingest_progress, 0, sizeof ingest_progress);
    const char *env_run = getenv("LAPLACE_INGEST_RUN_ID");
    if (env_run && *env_run && strlen(env_run) < sizeof ingest_progress.run_id)
        snprintf(ingest_progress.run_id, sizeof ingest_progress.run_id, "%s", env_run);
    else
        progress_make_uuid(ingest_progress.run_id);

    ingest_progress.pid = (long)getpid();
    ingest_progress.started_at = now();
#ifdef _WIN32
    const char *hn = getenv("COMPUTERNAME");
    snprintf(ingest_progress.host, sizeof ingest_progress.host, "%s", (hn && *hn) ? hn : "local");
#else
    if (gethostname(ingest_progress.host, sizeof ingest_progress.host - 1) != 0 || !ingest_progress.host[0]) {
        const char *hn = getenv("HOSTNAME");
        snprintf(ingest_progress.host, sizeof ingest_progress.host, "%s", (hn && *hn) ? hn : "local");
    }
    ingest_progress.host[sizeof ingest_progress.host - 1] = 0;
#endif

    const char *path = getenv("LAPLACE_INGEST_PROGRESS_PATH");
    char auto_path[4200];
    if (path && *path) {
        ingest_progress.explicit_out = 1;
        char parent[4200];
        snprintf(parent, sizeof parent, "%s", path);
        char *sep = strrchr(parent, '/');
        char *bsep = strrchr(parent, '\\');
        if (bsep && (!sep || bsep > sep)) sep = bsep;
        if (sep && sep != parent) { *sep = 0; progress_mkdirs(parent); }
        ingest_progress.out = fopen(path, "ab");
        if (!ingest_progress.out) { perror("ingest progress"); return 1; }
    } else if (do_load > 0) {
        ingest_progress.explicit_out = 0;
        const char *work = getenv("LAPLACE_WORK");
        if (!work || !*work) work = getenv("LAPLACE_WORK_ROOT");
        char runs_dir[4100];
        snprintf(runs_dir, sizeof runs_dir, "%s/logs/ingest/runs", (work && *work) ? work : ".");
        progress_mkdirs(runs_dir);
        snprintf(auto_path, sizeof auto_path, "%s/%s.jsonl", runs_dir, ingest_progress.run_id);
        ingest_progress.out = fopen(auto_path, "ab");
        if (!ingest_progress.out) {
            fprintf(stderr, "ingest progress: unable to open journal %s: %s (continuing without journal)\n",
                auto_path, strerror(errno));
            return 0;
        }
    } else {
        return 0;
    }

    ingest_progress.files = files; ingest_progress.stats = stats;
    ingest_progress.state = calloc((size_t)(nfiles > 0 ? nfiles : 1), 1);
    if (!ingest_progress.state) { perror("ingest progress state"); fclose(ingest_progress.out); ingest_progress.out = NULL; return 1; }
    for (int i = 0; i < nfiles; i++) if (!files[i].skipped) {
        if (!ingest_progress.source && files[i].source) ingest_progress.source = files[i].source->name;
        ingest_progress.total++;
        if (sizes) ingest_progress.bytes_total += sizes[i];
        if (files[i].known) { ingest_progress.state[i] = 2; ingest_progress.done++; ingest_progress.bytes += files[i].bytes; }
    }
    if (!ingest_progress.source && nfiles > 0 && files[0].path) ingest_progress.source = files[0].path;
    progress_write("inventory", NULL, NULL, NULL);
    for (int i = 0; i < nfiles; i++) if (!files[i].skipped && files[i].known)
        progress_write("file-known", &files[i], NULL, NULL);
    return 0;
}

static void progress_file_known(File *file) {
    if (!ingest_progress.out || !file || file->skipped) return;
    #pragma omp critical(ingest_progress_write)
    {
        unsigned char *state = &ingest_progress.state[file - ingest_progress.files];
        if (*state != 2) {
            *state = 2;
            ingest_progress.done++;
            ingest_progress.bytes += file->bytes;
            progress_write("file-known", file, NULL, NULL);
        }
    }
}

static void progress_file(const char *event, File *file) {
    if (!ingest_progress.out || !file || file->skipped) return;
    if (file->known) {
        if (!strcmp(event, "file-composed")) progress_file_known(file);
        return;
    }
    #pragma omp critical(ingest_progress_write)
    {
        unsigned char *state = &ingest_progress.state[file - ingest_progress.files];
        if (!strcmp(event, "file-start")) {
            if (!*state) { *state = 1; progress_write(event, file, NULL, NULL); }
        } else if (!strcmp(event, "file-composed")) {
            /* A curated file can be fully read without producing content,
             * as with Unicode's comments-only NamedSequencesProv.txt. */
            if (*state != 2 && !file->partial) {
                *state = 2; ingest_progress.done++; ingest_progress.bytes += file->bytes;
                progress_write(event, file, NULL, NULL);
            }
        } else progress_write(event, file, NULL, NULL);
    }
}

static void progress_phase(const char *event, const lp_id *source_id) {
    if (!ingest_progress.out) return;
    progress_write(event, NULL, source_id, NULL);
}

static void progress_end(const char *event, const lp_id *source_id, const char *error) {
    if (!ingest_progress.out) return;
    progress_write(event, NULL, source_id, error);
    fclose(ingest_progress.out); free(ingest_progress.state);
    memset(&ingest_progress, 0, sizeof ingest_progress);
}
