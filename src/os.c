/* The platform layer (os.h). */
#include "os.h"
#include <stdlib.h>
#include <string.h>

void os_list_free(os_list *l){
    for (size_t i = 0; i < l->n; i++) free(l->item[i]);
    free(l->item); l->item = NULL; l->n = 0;
}
static void list_add(os_list *l, const char *s){
    char **g = realloc(l->item, sizeof(char *) * (l->n + 1)); if (!g) return;
    l->item = g;
    size_t n = strlen(s) + 1; char *c = malloc(n); if (c) l->item[l->n++] = memcpy(c, s, n);
}

#ifndef _WIN32
#include <dirent.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <glob.h>
#include <sys/stat.h>

void *os_dl_open(const char *path){ return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void *os_dl_sym(void *lib, const char *symbol){ return dlsym(lib, symbol); }
const char *os_dl_error(void){ const char *e = dlerror(); return e ? e : ""; }

void os_glob(const char *pattern, os_list *out){
    out->item = NULL; out->n = 0;
    glob_t g; if (!glob(pattern, 0, NULL, &g)) for (size_t i = 0; i < g.gl_pathc; i++) list_add(out, g.gl_pathv[i]);
    globfree(&g);
}
int os_list_dir(const char *dir, os_list *out){
    out->item = NULL; out->n = 0;
    DIR *d = opendir(dir); if (!d) return -1;
    struct dirent *de; while ((de = readdir(d))) list_add(out, de->d_name);
    closedir(d); return 0;
}
int os_exists(const char *path){ struct stat st; return stat(path, &st) == 0; }
int os_fnmatch(const char *pattern, const char *string){ return fnmatch(pattern, string, 0); }
void os_slashes(char *path){ (void)path; }

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int os_is_dir(const char *path){ struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
int os_file_size(const char *path, uint64_t *size){ struct stat st; if (stat(path, &st) || !S_ISREG(st.st_mode)) return -1; *size = (uint64_t)st.st_size; return 0; }
int os_mkdir(const char *path){ return mkdir(path, 0775) == 0 || errno == EEXIST ? 0 : -1; }
int os_mkdirs(const char *path){
    char p[4096]; snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++) if (*c == '/') { *c = 0; os_mkdir(p); *c = '/'; }
    return os_mkdir(p);
}
char *os_realpath(const char *path, char *out, size_t cap){
    char r[PATH_MAX]; if (!realpath(path, r) || strlen(r) >= cap) return NULL;
    return strcpy(out, r);
}
int os_self_path(char *out, size_t cap){ ssize_t n = readlink("/proc/self/exe", out, cap - 1); if (n <= 0) return -1; out[n] = 0; return 0; }

static __thread void (*walk_fn)(const char *, uint64_t, void *); static __thread void *walk_arg;
static int walk_cb(const char *p, const struct stat *st, int type, struct FTW *fw){
    const char *b = p + fw->base;
    if (type == FTW_D && b[0] == '.' && fw->level > 0) return FTW_SKIP_SUBTREE;          /* hidden directories */
    if (type == FTW_F && b[0] != '.') walk_fn(p, (uint64_t)st->st_size, walk_arg);
    return FTW_CONTINUE;
}
void os_walk(const char *dir, void (*cb)(const char *, uint64_t, void *), void *arg){
    walk_fn = cb; walk_arg = arg; nftw(dir, walk_cb, 64, FTW_PHYS | FTW_ACTIONRETVAL);
}
int os_disk_space(const char *path, double *free_bytes, double *total_bytes){
    struct statvfs v; if (statvfs(path, &v)) return -1;
    *free_bytes = (double)v.f_bavail * (double)v.f_frsize; *total_bytes = (double)v.f_blocks * (double)v.f_frsize; return 0;
}
void *os_map_file(const char *path, size_t *len){
    int fd = open(path, O_RDONLY); struct stat sb; if (fd < 0) return NULL;
    if (fstat(fd, &sb)) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd);
    if (p == MAP_FAILED) return NULL; *len = (size_t)sb.st_size; return p;
}
void *os_reserve(size_t bytes){
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    madvise(p, bytes, MADV_HUGEPAGE);                   /* random probes over gigabytes: fewer TLB misses */
    return p;
}
void os_discard(void *p, size_t bytes){ madvise(p, bytes, MADV_DONTNEED); }
double os_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int os_run_logged(const char *program, char *const argv[], const char *log, const char *envname, const char *envval){
    fflush(NULL); pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 127; }
    if (!pid) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0664); if (fd < 0) { perror(log); _exit(127); }
        dup2(fd, 1); dup2(fd, 2); close(fd); if (envname) setenv(envname, envval, 1);
        execv(program, argv); perror(program); _exit(127);
    }
    int st = 0; while (waitpid(pid, &st, 0) < 0) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdio.h>

static wchar_t *wide(const char *s){
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0); if (n <= 0) return NULL;
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n); if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *narrow(const wchar_t *w){
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL); if (n <= 0) return NULL;
    char *s = malloc((size_t)n); if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static char dl_err[512];
void *os_dl_open(const char *path){
    wchar_t *w = wide(path); HMODULE h = w ? LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL; free(w);
    if (!h) FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), 0, dl_err, sizeof dl_err, NULL);
    return (void *)h;
}
void *os_dl_sym(void *lib, const char *symbol){ return (void *)GetProcAddress((HMODULE)lib, symbol); }
const char *os_dl_error(void){ return dl_err; }

void os_slashes(char *path){ for (char *c = path; *c; c++) if (*c == '\\') *c = '/'; }

int os_exists(const char *path){
    wchar_t *w = wide(path); DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES; free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}

int os_list_dir(const char *dir, os_list *out){
    out->item = NULL; out->n = 0;
    char pat[4096]; snprintf(pat, sizeof pat, "%s/*", dir);
    wchar_t *w = wide(pat); WIN32_FIND_DATAW fd; HANDLE h = w ? FindFirstFileW(w, &fd) : INVALID_HANDLE_VALUE; free(w);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return -1; }
    do { char *s = narrow(fd.cFileName); if (s) { list_add(out, s); free(s); } } while (FindNextFileW(h, &fd));
    FindClose(h); return 0;
}

/* fnmatch with flags 0: '*' and '?' match any character including '/', '[...]' a set ('!' or '^' negates, ranges),
 * '\' escapes the next character. */
static int bracket(const char **pp, char c){
    const char *p = *pp; int neg = 0, hit = 0;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    int first = 1;
    while (*p && (first || *p != ']')) {
        first = 0; char lo = *p == '\\' && p[1] ? *++p : *p; p++;
        if (*p == '-' && p[1] && p[1] != ']') { p++; char hi = *p == '\\' && p[1] ? *++p : *p; p++; if (lo <= c && c <= hi) hit = 1; }
        else if (lo == c) hit = 1;
    }
    if (*p != ']') return -1;                            /* no closing bracket: '[' is literal */
    *pp = p + 1; return hit != neg;
}
int os_fnmatch(const char *p, const char *s){
    for (;; p++, s++) {
        switch (*p) {
        case 0: return *s ? 1 : 0;
        case '?': if (!*s) return 1; break;
        case '*': {
            while (p[1] == '*') p++;
            if (!p[1]) return 0;
            for (const char *t = s; ; t++) { if (!os_fnmatch(p + 1, t)) return 0; if (!*t) return 1; }
        }
        case '[': {
            if (!*s) return 1; const char *q = p + 1; int r = bracket(&q, *s);
            if (r < 0) { if (*s != '[') return 1; break; }
            if (!r) return 1; p = q - 1; break;
        }
        case '\\': if (p[1]) p++; /* fall through: the escaped character, literally */
        default: if (*p != *s) return 1;
        }
    }
}

static int has_magic(const char *s, size_t n){ for (size_t i = 0; i < n; i++) if (strchr("*?[", s[i])) return 1; return 0; }
static int by_bytes(const void *a, const void *b){ return strcmp(*(char *const *)a, *(char *const *)b); }
/* One component of the pattern at a time: a literal component is appended; a component with wildcards is matched
 * against the directory's names, a leading '.' only by a leading '.'. */
static void expand(const char *base, const char *rest, os_list *out){
    while (*rest == '/') rest++;
    if (!*rest) { if (os_exists(base)) list_add(out, base); return; }
    const char *end = strchr(rest, '/'); size_t n = end ? (size_t)(end - rest) : strlen(rest);
    char comp[1024]; if (n >= sizeof comp) return; memcpy(comp, rest, n); comp[n] = 0;
    char next[4096];
    if (!has_magic(comp, n)) {
        snprintf(next, sizeof next, "%s%s%s", base, (*base && base[strlen(base) - 1] != '/') ? "/" : "", comp);
        expand(next, rest + n, out); return;
    }
    os_list names; if (os_list_dir(*base ? base : ".", &names)) return;
    for (size_t i = 0; i < names.n; i++) {
        const char *nm = names.item[i];
        if (nm[0] == '.' && comp[0] != '.') continue;                 /* '.' and '..' too, as glob(3) gives them */
        if (os_fnmatch(comp, nm)) continue;
        snprintf(next, sizeof next, "%s%s%s", base, (*base && base[strlen(base) - 1] != '/') ? "/" : "", nm);
        expand(next, rest + n, out);
    }
    os_list_free(&names);
}
void os_glob(const char *pattern, os_list *out){
    out->item = NULL; out->n = 0;
    char p[4096]; snprintf(p, sizeof p, "%s", pattern); os_slashes(p);
    char base[16] = "";
    const char *rest = p;
    if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') { base[0] = p[0]; base[1] = ':'; base[2] = '/'; base[3] = 0; rest = p + 2; }
    else if (p[0] == '/') { base[0] = '/'; base[1] = 0; }
    expand(base, rest, out);
    qsort(out->item, out->n, sizeof(char *), by_bytes);
}

/* ---- what the C library lacks */
void *os_memmem(const void *hay, size_t hn, const void *needle, size_t nn){
    if (!nn) return (void *)hay;
    if (nn > hn) return NULL;
    const unsigned char *h = hay, *n = needle, *end = h + hn - nn;
    for (; h <= end; h++) { h = memchr(h, n[0], (size_t)(end - h) + 1); if (!h) return NULL; if (!memcmp(h, n, nn)) return (void *)h; }
    return NULL;
}
char *os_strndup(const char *s, size_t n){
    size_t l = 0; while (l < n && s[l]) l++;
    char *c = malloc(l + 1); if (c) { memcpy(c, s, l); c[l] = 0; } return c;
}
ssize_t os_getline(char **line, size_t *cap, FILE *f){
    if (!*line || !*cap) { *cap = 256; *line = malloc(*cap); if (!*line) return -1; }
    size_t n = 0; int c;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) { size_t nc = *cap * 2; char *g = realloc(*line, nc); if (!g) return -1; *line = g; *cap = nc; }
        (*line)[n++] = (char)c; if (c == '\n') break;
    }
    if (!n && c == EOF) return -1;
    (*line)[n] = 0; return (ssize_t)n;
}
/* The format without the ' flag (thousands grouping), which the Universal CRT does not know. */
static const char *plain(const char *fmt, char *buf, size_t cap){
    if (!strchr(fmt, '\'')) return fmt;
    size_t j = 0; int in = 0;
    for (const char *p = fmt; *p && j + 1 < cap; p++) {
        if (in && *p == '\'') continue;
        if (*p == '%') in = !in || p[-1] != '%'; else if (in && !strchr("-+ #0123456789.*hlLqjzt", *p)) in = 0;
        buf[j++] = *p;
    }
    buf[j] = 0; return buf;
}
#undef printf
#undef fprintf
#undef snprintf
#include <stdarg.h>
int os_printf(const char *fmt, ...){ char b[2048]; va_list ap; va_start(ap, fmt); int r = vprintf(plain(fmt, b, sizeof b), ap); va_end(ap); return r; }
int os_fprintf(FILE *f, const char *fmt, ...){ char b[2048]; va_list ap; va_start(ap, fmt); int r = vfprintf(f, plain(fmt, b, sizeof b), ap); va_end(ap); return r; }
int os_snprintf(char *s, size_t n, const char *fmt, ...){ char b[2048]; va_list ap; va_start(ap, fmt); int r = vsnprintf(s, n, plain(fmt, b, sizeof b), ap); va_end(ap); return r; }

/* ---- files and directories */
int os_is_dir(const char *path){
    wchar_t *w = wide(path); DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES; free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
int os_file_size(const char *path, uint64_t *size){
    wchar_t *w = wide(path); WIN32_FILE_ATTRIBUTE_DATA d; BOOL ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &d); free(w);
    if (!ok || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    *size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow; return 0;
}
int os_mkdir(const char *path){
    wchar_t *w = wide(path); BOOL ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS); free(w);
    return ok ? 0 : -1;
}
int os_mkdirs(const char *path){
    char p[4096]; snprintf(p, sizeof p, "%s", path); os_slashes(p);
    for (char *c = p + 1; *c; c++) if (*c == '/' && c[-1] != ':') { *c = 0; os_mkdir(p); *c = '/'; }
    return os_mkdir(p);
}
char *os_realpath(const char *path, char *out, size_t cap){
    wchar_t *w = wide(path); if (!w) return NULL;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL); free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    wchar_t full[4096]; DWORD n = GetFinalPathNameByHandleW(h, full, 4096, FILE_NAME_NORMALIZED); CloseHandle(h);
    if (!n || n >= 4096) return NULL;
    const wchar_t *f = full; if (!wcsncmp(f, L"\\\\?\\UNC\\", 8)) f += 6; else if (!wcsncmp(f, L"\\\\?\\", 4)) f += 4;   /* the extended prefix off */
    char *s = narrow(f); if (!s) return NULL;
    if (strlen(s) >= cap) { free(s); return NULL; }
    strcpy(out, s); free(s); os_slashes(out); return out;
}
int os_self_path(char *out, size_t cap){
    wchar_t w[4096]; DWORD n = GetModuleFileNameW(NULL, w, 4096); if (!n || n >= 4096) return -1;
    char *s = narrow(w); if (!s || strlen(s) >= cap) { free(s); return -1; }
    strcpy(out, s); free(s); os_slashes(out); return 0;
}
void os_walk(const char *dir, void (*cb)(const char *, uint64_t, void *), void *arg){
    char pat[4096]; snprintf(pat, sizeof pat, "%s/*", dir);
    wchar_t *w = wide(pat); WIN32_FIND_DATAW fd; HANDLE h = w ? FindFirstFileW(w, &fd) : INVALID_HANDLE_VALUE; free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *nm = narrow(fd.cFileName); if (!nm) continue;
        if (nm[0] == '.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { free(nm); continue; }   /* hidden, '.', '..', links */
        char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, nm); free(nm);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) os_walk(p, cb, arg);
        else cb(p, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, arg);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
int os_disk_space(const char *path, double *free_bytes, double *total_bytes){
    wchar_t *w = wide(path); ULARGE_INTEGER avail, total; BOOL ok = w && GetDiskFreeSpaceExW(w, &avail, &total, NULL); free(w);
    if (!ok) return -1;
    *free_bytes = (double)avail.QuadPart; *total_bytes = (double)total.QuadPart; return 0;
}

/* ---- memory and time */
void *os_map_file(const char *path, size_t *len){
    wchar_t *w = wide(path); if (!w) return NULL;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); free(w);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz; if (!GetFileSizeEx(f, &sz)) { CloseHandle(f); return NULL; }
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL); CloseHandle(f);
    if (!m) return NULL;
    void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0); CloseHandle(m);
    if (p) *len = (size_t)sz.QuadPart; return p;
}
/* MAP_NORESERVE memory on Windows: the range is reserved, and Windows counts committed pages against the paging file,
 * so a 2^32-slot table (hundreds of GB) is committed a 2 MB piece at a time, at the first touch of each: the access
 * violation the touch raises is answered by committing the piece and resuming. A piece handed back (os_discard) is
 * decommitted, and reads as zero when touched again, as MADV_DONTNEED leaves it. */
static struct { char *base; size_t len; } reserved[16]; static int nreserved, faults_on;
#define PIECE ((size_t)1 << 21)
static LONG CALLBACK commit_on_fault(EXCEPTION_POINTERS *e){
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    char *at = (char *)e->ExceptionRecord->ExceptionInformation[1];
    for (int i = 0; i < nreserved; i++) if (at >= reserved[i].base && at < reserved[i].base + reserved[i].len) {
        char *from = reserved[i].base + ((size_t)(at - reserved[i].base) & ~(PIECE - 1)), *end = reserved[i].base + reserved[i].len;
        size_t n = (size_t)(end - from) < PIECE ? (size_t)(end - from) : PIECE;
        return VirtualAlloc(from, n, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void *os_reserve(size_t bytes){
    void *p = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!p) { fprintf(stderr, "VirtualAlloc(reserve %zu bytes): error %lu\n", bytes, GetLastError()); exit(1); }
    if (nreserved == 16) { fprintf(stderr, "os_reserve: more than 16 reservations\n"); exit(1); }
    reserved[nreserved].base = p; reserved[nreserved].len = bytes; nreserved++;
    if (!faults_on) { AddVectoredExceptionHandler(1, commit_on_fault); faults_on = 1; }
    return p;
}
void os_discard(void *p, size_t bytes){ if (bytes) VirtualFree(p, bytes, MEM_DECOMMIT); }
double os_now(void){
    static LARGE_INTEGER freq; LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)freq.QuadPart;
}

/* ---- threads, random numbers, sorting with an argument */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a){ (void)a; InitializeSRWLock((PSRWLOCK)m); return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m){ (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m){ AcquireSRWLockExclusive((PSRWLOCK)m); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m){ ReleaseSRWLockExclusive((PSRWLOCK)m); return 0; }
int os_rand_r(unsigned *seed){                        /* glibc stdlib/rand_r.c */
    unsigned next = *seed; int result;
    next *= 1103515245; next += 12345; result = (int)((next / 65536) % 2048);
    next *= 1103515245; next += 12345; result <<= 10; result ^= (int)((next / 65536) % 1024);
    next *= 1103515245; next += 12345; result <<= 10; result ^= (int)((next / 65536) % 1024);
    *seed = next; return result;
}
static __declspec(thread) int (*qs_cmp)(const void *, const void *, void *); static __declspec(thread) void *qs_arg;
static int qs_call(const void *a, const void *b){ return qs_cmp(a, b, qs_arg); }
void os_qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg){
    int (*wc)(const void *, const void *, void *) = qs_cmp; void *wa = qs_arg;   /* one per thread, and reentrant */
    qs_cmp = cmp; qs_arg = arg; qsort(base, n, size, qs_call); qs_cmp = wc; qs_arg = wa;
}

/* ---- statx from what Windows keeps */
static void when(const FILETIME *ft, struct statx_timestamp *t){
    uint64_t v = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime, u = v - 116444736000000000ull;   /* 1601 to 1970 */
    t->tv_sec = (int64_t)(u / 10000000ull); t->tv_nsec = (uint32_t)(u % 10000000ull) * 100; t->pad_ = 0;
}
int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *out){
    (void)dirfd; (void)flags; (void)mask;
    wchar_t *w = wide(path); if (!w) return -1;
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL); free(w);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return -1; }
    BY_HANDLE_FILE_INFORMATION bi; FILE_BASIC_INFO fb; int ok = GetFileInformationByHandle(h, &bi) && GetFileInformationByHandleEx(h, FileBasicInfo, &fb, sizeof fb);
    CloseHandle(h); if (!ok) { errno = EIO; return -1; }
    memset(out, 0, sizeof *out);
    int dir = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ro = (bi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    out->stx_mode = (uint16_t)((dir ? 0040000 : 0100000) | (ro ? 0555 : 0755)); out->stx_nlink = bi.nNumberOfLinks;
    out->stx_ino = ((uint64_t)bi.nFileIndexHigh << 32) | bi.nFileIndexLow; out->stx_size = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
    out->stx_blocks = (out->stx_size + 511) / 512; out->stx_blksize = 4096; out->stx_attributes = bi.dwFileAttributes;
    out->stx_dev_major = bi.dwVolumeSerialNumber >> 16; out->stx_dev_minor = bi.dwVolumeSerialNumber & 0xffff;
    when(&bi.ftLastAccessTime, &out->stx_atime); when(&bi.ftCreationTime, &out->stx_btime); when(&bi.ftLastWriteTime, &out->stx_mtime);
    FILETIME ct = { (DWORD)fb.ChangeTime.LowPart, (DWORD)fb.ChangeTime.HighPart }; when(&ct, &out->stx_ctime);
    out->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_SIZE | STATX_BLOCKS | STATX_ATIME | STATX_BTIME | STATX_CTIME | STATX_MTIME;
    return 0;
}

/* ---- processes */
/* An argument quoted as CommandLineToArgvW reads it back: in double quotes when it holds a space, a tab or a quote,
 * each inner quote and the backslashes before it escaped. */
static void quote_arg(const char *a, char *out, size_t *n, size_t cap){
    int plain_ = *a && !strpbrk(a, " \t\"");
    if (!plain_ && *n < cap) out[(*n)++] = '"';
    for (const char *p = a; *p; p++) {
        size_t bs = 0; while (*p == '\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < bs * (plain_ ? 1 : 2) && *n < cap; i++) out[(*n)++] = '\\'; break; }
        if (*p == '"') { for (size_t i = 0; i < bs * 2 + 1 && *n < cap; i++) out[(*n)++] = '\\'; }
        else for (size_t i = 0; i < bs && *n < cap; i++) out[(*n)++] = '\\';
        if (*n < cap) out[(*n)++] = *p;
    }
    if (!plain_ && *n < cap) out[(*n)++] = '"';
}
int os_run_logged(const char *program, char *const argv[], const char *log, const char *envname, const char *envval){
    fflush(NULL);
    size_t cap = 1; for (int i = 0; argv[i]; i++) cap += strlen(argv[i]) * 2 + 4;
    char *cmd = malloc(cap); size_t n = 0;
    for (int i = 0; argv[i]; i++) { if (i) cmd[n++] = ' '; quote_arg(argv[i], cmd, &n, cap - 1); }
    cmd[n] = 0;
    wchar_t *wcmd = wide(cmd), *wprog = wide(program), *wlog = wide(log); free(cmd);
    if (envname) { wchar_t *wn = wide(envname), *wv = wide(envval); SetEnvironmentVariableW(wn, wv); free(wn); free(wv); }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out = wlog ? CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    int code = 127;
    if (out != INVALID_HANDLE_VALUE && wcmd && wprog) {
        STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = out; si.hStdError = out;
        PROCESS_INFORMATION pi;
        if (CreateProcessW(wprog, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE); DWORD ec = 127; GetExitCodeProcess(pi.hProcess, &ec);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            code = ec > 255 ? 128 + (int)(ec & 127) : (int)ec;               /* a crash (0xC000....) as a signal would be */
        } else fprintf(stderr, "%s: CreateProcess error %lu\n", program, GetLastError());
    } else if (out == INVALID_HANDLE_VALUE) fprintf(stderr, "%s: cannot be written (error %lu)\n", log, GetLastError());
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (envname) { wchar_t *wn = wide(envname); SetEnvironmentVariableW(wn, NULL); free(wn); }
    free(wcmd); free(wprog); free(wlog);
    return code;
}
#endif
