/* The platform layer (os.h). */
#include "os.h"
#include <errno.h>
#include <stdio.h>
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
int os_rename(const char *from, const char *to){ return rename(from, to); }
int os_fnmatch(const char *pattern, const char *string){ return fnmatch(pattern, string, 0); }
void os_slashes(char *path){ (void)path; }

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
void *os_reserve(size_t bytes){
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    madvise(p, bytes, MADV_HUGEPAGE);                   /* random probes over gigabytes: fewer TLB misses */
    return p;
}
const void *os_map_read(const char *path, size_t *size){
    int fd = open(path, O_RDONLY); if (fd < 0) return NULL;
    struct stat st; if (fstat(fd, &st) || st.st_size == 0) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd);
    if (p == MAP_FAILED) return NULL;
    *size = (size_t)st.st_size; return p;
}
void os_discard(void *p, size_t bytes){ if (bytes) madvise(p, bytes, MADV_DONTNEED); }
int os_run_logged(const char *exe, char *const argv[], const char *log, const char *var, const char *value){
    (void)exe; (void)argv; (void)log; (void)var; (void)value; errno = ENOSYS; return -1;      /* POSIX callers fork */
}

#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <unistd.h>

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
int os_rename(const char *from, const char *to){
    wchar_t *f = wide(from), *t = wide(to); BOOL ok = MoveFileExW(f, t, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED); free(f); free(t);
    if (!ok) { errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES; return -1; }
    return 0;
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

/* Address space as Linux overcommits it: reserved whole, and each page committed the first time it is touched, by a
 * handler for the access violation that touch raises inside a reserved region. Committing 32 GB of slots up front
 * would charge it all against the commit limit; this way a table costs only what it uses, as MAP_NORESERVE gives. */
#define OS_REGIONS 64
static struct { char *base; size_t bytes; } region[OS_REGIONS];
static volatile LONG nregion;
static LONG CALLBACK commit_on_touch(PEXCEPTION_POINTERS e){
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    char *at = (char *)e->ExceptionRecord->ExceptionInformation[1];
    for (LONG i = 0; i < nregion; i++)
        if (at >= region[i].base && at < region[i].base + region[i].bytes) {
            char *page = region[i].base + ((size_t)(at - region[i].base) & ~(size_t)0xFFFF);      /* 64 KB at a time */
            size_t n = region[i].base + region[i].bytes - page; if (n > 0x10000) n = 0x10000;
            return VirtualAlloc(page, n, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
        }
    return EXCEPTION_CONTINUE_SEARCH;
}
void *os_reserve(size_t bytes){
    static volatile LONG handler; if (!InterlockedExchange(&handler, 1)) AddVectoredExceptionHandler(1, commit_on_touch);
    void *p = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE); if (!p) return NULL;
    LONG i = InterlockedIncrement(&nregion) - 1; if (i >= OS_REGIONS) { VirtualFree(p, 0, MEM_RELEASE); return NULL; }
    region[i].bytes = bytes; region[i].base = p;
    return p;
}
const void *os_map_read(const char *path, size_t *size){
    wchar_t *w = wide(path); if (!w) return NULL;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); free(w);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER n; if (!GetFileSizeEx(f, &n) || n.QuadPart == 0) { CloseHandle(f); return NULL; }
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL); CloseHandle(f); if (!m) return NULL;
    const void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0); CloseHandle(m);
    if (p) *size = (size_t)n.QuadPart;
    return p;
}

/* Decommitted and committed again, the whole pages come back zero; a partial last page is zeroed by hand. */
void os_discard(void *p, size_t bytes){
    if (!bytes) return;
    SYSTEM_INFO si; GetSystemInfo(&si); size_t page = si.dwPageSize, whole = bytes / page * page;
    if (whole) VirtualFree(p, whole, MEM_DECOMMIT);                                  /* committed again, zero, when next touched */
    if (bytes > whole) memset((char *)p + whole, 0, bytes - whole);
}

/* pthread mutexes as slim reader/writer locks, taken exclusively: same size as a pointer, zero is unlocked. */
int pthread_mutex_init(pthread_mutex_t *m, const void *attr){ (void)attr; InitializeSRWLock((PSRWLOCK)m); return 0; }
int pthread_mutex_lock(pthread_mutex_t *m){ AcquireSRWLockExclusive((PSRWLOCK)m); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m){ ReleaseSRWLockExclusive((PSRWLOCK)m); return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m){ (void)m; return 0; }

void *memmem(const void *hay, size_t hn, const void *needle, size_t nn){
    if (nn == 0) return (void *)hay;
    if (nn > hn) return NULL;
    const unsigned char *h = hay, *n = needle, *end = h + (hn - nn) + 1;
    for (const unsigned char *p = h; (p = memchr(p, n[0], (size_t)(end - p))) != NULL; p++)
        if (!memcmp(p, n, nn)) return (void *)p;
    return NULL;
}
char *strndup(const char *s, size_t n){
    size_t k = 0; while (k < n && s[k]) k++;
    char *d = malloc(k + 1); if (d) { memcpy(d, s, k); d[k] = 0; }
    return d;
}
ssize_t getline(char **line, size_t *cap, FILE *f){
    size_t n = 0; int c;
    if (!*line || !*cap) { *cap = 128; *line = realloc(*line, *cap); if (!*line) return -1; }
    while ((c = getc(f)) != EOF) {
        if (n + 2 > *cap) { size_t nc = *cap * 2; char *l = realloc(*line, nc); if (!l) return -1; *line = l; *cap = nc; }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return -1;
    (*line)[n] = 0; return (ssize_t)n;
}
int rand_r(unsigned int *seed){
    unsigned int next = *seed; int result;
    next *= 1103515245; next += 12345; result = (unsigned int)(next / 65536) % 2048;
    next *= 1103515245; next += 12345; result <<= 10; result ^= (unsigned int)(next / 65536) % 1024;
    next *= 1103515245; next += 12345; result <<= 10; result ^= (unsigned int)(next / 65536) % 1024;
    *seed = next; return result;
}
static void merge_sort(char *a, char *t, size_t n, size_t sz, int (*cmp)(const void *, const void *, void *), void *arg){
    if (n < 2) return;
    size_t h = n / 2; merge_sort(a, t, h, sz, cmp, arg); merge_sort(a + h * sz, t, n - h, sz, cmp, arg);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) { if (cmp(a + j * sz, a + i * sz, arg) < 0) memcpy(t + k++ * sz, a + j++ * sz, sz); else memcpy(t + k++ * sz, a + i++ * sz, sz); }
    while (i < h) memcpy(t + k++ * sz, a + i++ * sz, sz);
    while (j < n) memcpy(t + k++ * sz, a + j++ * sz, sz);
    memcpy(a, t, n * sz);
}
void qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg){
    char *t = malloc(n * size ? n * size : 1); if (!t) abort();
    merge_sort(base, t, n, size, cmp, arg); free(t);
}
/* FILETIME (100 ns since 1601) to a Unix timestamp. */
static struct statx_timestamp unix_time(FILETIME f){
    uint64_t t = ((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime) - 116444736000000000ull;
    struct statx_timestamp s = { (int64_t)(t / 10000000ull), (uint32_t)(t % 10000000ull) * 100u, 0 }; return s;
}
int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *x){
    (void)dirfd; (void)flags; (void)mask; memset(x, 0, sizeof *x);
    wchar_t *w = wide(path); if (!w) { errno = ENOMEM; return -1; }
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w); if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return -1; }
    BY_HANDLE_FILE_INFORMATION i; BOOL ok = GetFileInformationByHandle(h, &i); CloseHandle(h);
    if (!ok) { errno = EIO; return -1; }
    x->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_SIZE | STATX_ATIME | STATX_MTIME | STATX_BTIME;
    x->stx_mode = (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0040755 : (i.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0100444 : 0100644;
    x->stx_nlink = i.nNumberOfLinks; x->stx_ino = (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow;
    x->stx_size = (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow; x->stx_blksize = 4096;
    x->stx_atime = unix_time(i.ftLastAccessTime); x->stx_mtime = unix_time(i.ftLastWriteTime); x->stx_btime = unix_time(i.ftCreationTime);
    x->stx_dev_major = i.dwVolumeSerialNumber;
    return 0;
}

#include <ftw.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <glob.h>
/* nftw: depth first in byte order of names, directories before their contents (no FTW_DEPTH), FTW_ACTIONRETVAL's
 * skip and stop honoured; links are not followed (FTW_PHYS: Windows reparse points are not descended). */
static int walk(char *path, size_t len, int level, int (*fn)(const char *, const struct stat *, int, struct FTW *), int flags){
    struct stat st; int type = stat(path, &st) ? FTW_NS : S_ISDIR(st.st_mode) ? FTW_D : FTW_F;
    const char *slash = strrchr(path, '/'); struct FTW fw = { slash ? (int)(slash - path + 1) : 0, level };
    int r = fn(path, &st, type, &fw);
    if (flags & FTW_ACTIONRETVAL) { if (r == FTW_STOP) return FTW_STOP; if (r == FTW_SKIP_SUBTREE || r == FTW_SKIP_SIBLINGS || type != FTW_D) return r == FTW_SKIP_SIBLINGS ? r : 0; }
    else if (r) return r;
    if (type != FTW_D) return 0;
    wchar_t *w = wide(path); DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES; free(w);
    if ((flags & FTW_PHYS) && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    os_list names; if (os_list_dir(path, &names)) return 0;
    qsort(names.item, names.n, sizeof(char *), by_bytes);
    int out = 0;
    for (size_t i = 0; i < names.n && !out; i++) {
        if (!strcmp(names.item[i], ".") || !strcmp(names.item[i], "..")) continue;
        size_t nl = strlen(names.item[i]); if (len + 1 + nl >= 4096) continue;
        path[len] = '/'; memcpy(path + len + 1, names.item[i], nl + 1);
        int rr = walk(path, len + 1 + nl, level + 1, fn, flags); path[len] = 0;
        if (rr == FTW_STOP || (!(flags & FTW_ACTIONRETVAL) && rr)) out = rr;
        else if (rr == FTW_SKIP_SIBLINGS) break;
    }
    os_list_free(&names); return out;
}
int nftw(const char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *), int fds, int flags){
    (void)fds; char p[4096]; snprintf(p, sizeof p, "%s", path); os_slashes(p);
    size_t n = strlen(p); while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    int r = walk(p, n, 0, fn, flags); return r == FTW_STOP ? FTW_STOP : r < 0 ? -1 : r;
}
int statvfs(const char *path, struct statvfs *v){
    wchar_t *w = wide(path); ULARGE_INTEGER avail, total, free_; BOOL ok = w && GetDiskFreeSpaceExW(w, &avail, &total, &free_); free(w);
    if (!ok) { errno = ENOENT; return -1; }
    v->f_bsize = v->f_frsize = 1; v->f_blocks = total.QuadPart; v->f_bfree = free_.QuadPart; v->f_bavail = avail.QuadPart;
    return 0;
}
pid_t fork(void){ errno = ENOSYS; return -1; }
/* The command line as CommandLineToArgvW reads it back: each argument quoted, backslashes before a quote doubled. */
static void quote(wchar_t *out, size_t *n, size_t cap, const char *arg){
    wchar_t *w = wide(arg); if (!w) return;
    if (*n + 1 < cap) out[(*n)++] = L'"';
    for (const wchar_t *p = w; ; p++) {
        size_t bs = 0; while (*p == L'\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < bs * 2 && *n + 1 < cap; i++) out[(*n)++] = L'\\'; break; }
        if (*p == L'"') { for (size_t i = 0; i < bs * 2 + 1 && *n + 1 < cap; i++) out[(*n)++] = L'\\'; }
        else for (size_t i = 0; i < bs && *n + 1 < cap; i++) out[(*n)++] = L'\\';
        if (*n + 1 < cap) out[(*n)++] = *p;
    }
    if (*n + 1 < cap) out[(*n)++] = L'"';
    free(w);
}
int os_run_logged(const char *exe, char *const argv[], const char *log, const char *var, const char *value){
    static wchar_t cmd[32768]; size_t n = 0;
    for (int i = 0; argv[i]; i++) { if (i && n + 1 < 32768) cmd[n++] = L' '; quote(cmd, &n, 32768, argv[i]); }
    cmd[n] = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    wchar_t *wl = wide(log), *we = wide(exe); if (!wl || !we) { free(wl); free(we); errno = ENOMEM; return -1; }
    HANDLE h = CreateFileW(wl, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); free(wl);
    if (h == INVALID_HANDLE_VALUE) { free(we); errno = EACCES; return -1; }
    STARTUPINFOW si = { sizeof si }; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = h; si.hStdError = h;
    char *was = getenv(var) ? _strdup(getenv(var)) : NULL; _putenv_s(var, value);   /* the child inherits it */
    PROCESS_INFORMATION pi; BOOL ok = CreateProcessW(we, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    _putenv_s(var, was ? was : ""); free(was); free(we); CloseHandle(h);
    if (!ok) { errno = ENOEXEC; return -1; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}
pid_t waitpid(pid_t pid, int *status, int options){ (void)pid; (void)status; (void)options; errno = ECHILD; return -1; }
int pipe(int fd[2]){ return _pipe(fd, 1 << 16, _O_BINARY); }
long long readlink(const char *path, char *buf, size_t size){
    if (strcmp(path, "/proc/self/exe")) { errno = EINVAL; return -1; }
    wchar_t w[4096]; DWORD n = GetModuleFileNameW(NULL, w, 4096); if (!n || n >= 4096) { errno = ENAMETOOLONG; return -1; }
    int k = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, buf, (int)size, NULL, NULL); if (k <= 0) { errno = ENAMETOOLONG; return -1; }
    for (int i = 0; i < k; i++) if (buf[i] == '\\') buf[i] = '/';
    return k;                                           /* not terminated, as readlink(2) */
}
int setenv(const char *name, const char *value, int overwrite){ if (!overwrite && getenv(name)) return 0; return _putenv_s(name, value) ? -1 : 0; }
char *realpath(const char *path, char *resolved){ char *r = _fullpath(resolved, path, 4096); if (r) os_slashes(r); return r; }
int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g){
    (void)flags; (void)errfunc; os_list l; os_glob(pattern, &l);
    g->gl_pathc = l.n; g->gl_pathv = l.item; g->gl_offs = 0; return l.n ? 0 : 3;   /* GLOB_NOMATCH */
}
void globfree(glob_t *g){ os_list l = { g->gl_pathv, g->gl_pathc }; os_list_free(&l); g->gl_pathv = NULL; g->gl_pathc = 0; }

/* printf with the "'" flag. Each conversion is formatted alone by the C runtime, without the flag; where the flag
 * was, the run of digits before the decimal point gets a ',' every three digits and the field is padded again to its
 * width. */
#undef snprintf
#undef fprintf
#undef printf
static int group(char *s, int cap, int len){
    int a = 0; while (a < len && (s[a] == ' ' || s[a] == '+' || s[a] == '-')) a++;
    int b = a; while (b < len && s[b] >= '0' && s[b] <= '9') b++;
    int digits = b - a, commas = digits > 3 ? (digits - 1) / 3 : 0;
    if (!commas || len + commas >= cap) return len;
    memmove(s + b + commas, s + b, (size_t)(len - b + 1));
    for (int i = digits - 1, w = b + commas - 1, k = 0; i >= 0; i--, k++) {
        if (k && k % 3 == 0) s[w--] = ',';
        s[w--] = s[a + i];
    }
    return len + commas;
}
int os_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap){
    size_t n = 0; char spec[64], piece[512];
    for (const char *p = fmt; *p; ) {
        if (*p != '%') { if (n + 1 < cap) out[n] = *p; n++; p++; continue; }
        const char *start = p++; int grouped = 0, width = -1, left = 0; size_t sl = 1; spec[0] = '%';
        while (*p && strchr("'-+ #0", *p)) { if (*p == '\'') grouped = 1; else { if (*p == '-') left = 1; spec[sl++] = *p; } p++; }
        if (*p == '*') { width = va_arg(ap, int); sl += (size_t)sprintf(spec + sl, "%d", width); p++; }
        else { if (*p >= '0' && *p <= '9') width = atoi(p); while (*p >= '0' && *p <= '9') spec[sl++] = *p++; }
        if (*p == '.') { spec[sl++] = *p++; if (*p == '*') { sl += (size_t)sprintf(spec + sl, "%d", va_arg(ap, int)); p++; } else while (*p >= '0' && *p <= '9') spec[sl++] = *p++; }
        char len[3] = ""; int ll = 0;
        while (*p && strchr("hlLzjt", *p) && ll < 2) { len[ll++] = *p; spec[sl++] = *p++; }
        char c = *p ? *p++ : 0; spec[sl++] = c; spec[sl] = 0;
        int k;
        switch (c) {
        case '%': k = 1; piece[0] = '%'; piece[1] = 0; break;
        case 'd': case 'i':
            if (len[0] == 'l' && len[1] == 'l') k = snprintf(piece, sizeof piece, spec, va_arg(ap, long long));
            else if (len[0] == 'z' || len[0] == 'j' || len[0] == 't' || len[0] == 'l') k = snprintf(piece, sizeof piece, spec, va_arg(ap, long long));
            else k = snprintf(piece, sizeof piece, spec, va_arg(ap, int));
            break;
        case 'u': case 'x': case 'X': case 'o':
            if (len[0] == 'l' && len[1] == 'l') k = snprintf(piece, sizeof piece, spec, va_arg(ap, unsigned long long));
            else if (len[0] == 'z' || len[0] == 'j' || len[0] == 't' || len[0] == 'l') k = snprintf(piece, sizeof piece, spec, va_arg(ap, unsigned long long));
            else k = snprintf(piece, sizeof piece, spec, va_arg(ap, unsigned int));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            k = len[0] == 'L' ? snprintf(piece, sizeof piece, spec, va_arg(ap, long double)) : snprintf(piece, sizeof piece, spec, va_arg(ap, double)); break;
        case 'c': k = snprintf(piece, sizeof piece, spec, va_arg(ap, int)); break;
        case 'p': k = snprintf(piece, sizeof piece, spec, va_arg(ap, void *)); break;
        case 's': {                                     /* a string can be longer than the piece buffer */
            const char *str = va_arg(ap, const char *); int need = snprintf(NULL, 0, spec, str);
            if (need >= (int)sizeof piece) { char *big = malloc((size_t)need + 1); snprintf(big, (size_t)need + 1, spec, str);
                for (int i = 0; i < need; i++) { if (n + 1 < cap) out[n] = big[i]; n++; } free(big); continue; }
            k = snprintf(piece, sizeof piece, spec, str); break; }
        case 'n': *va_arg(ap, int *) = (int)n; continue;
        default: k = (int)(p - start); memcpy(piece, start, (size_t)k); piece[k] = 0; break;
        }
        if (k < 0) k = 0;
        if (grouped && strchr("diufFgG", c)) {
            int t = 0; while (piece[t] == ' ') t++;             /* the digits without the width's padding, grouped, padded again */
            memmove(piece, piece + t, (size_t)(k - t + 1)); k -= t;
            k = group(piece, (int)sizeof piece, k);
            if (width > k) { int pad = width - k; if (left) { memset(piece + k, ' ', (size_t)pad); piece[width] = 0; } else { memmove(piece + pad, piece, (size_t)k + 1); memset(piece, ' ', (size_t)pad); } k = width; }
        }
        for (int i = 0; i < k; i++) { if (n + 1 < cap) out[n] = piece[i]; n++; }
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return (int)n;
}
int os_snprintf(char *out, size_t cap, const char *fmt, ...){ va_list ap; va_start(ap, fmt); int n = os_vsnprintf(out, cap, fmt, ap); va_end(ap); return n; }
static int os_vfprintf(FILE *f, const char *fmt, va_list ap){
    char small[4096]; va_list again; va_copy(again, ap);
    int n = os_vsnprintf(small, sizeof small, fmt, ap);
    if (n < (int)sizeof small) { fwrite(small, 1, (size_t)n, f); va_end(again); return n; }
    char *big = malloc((size_t)n + 1); os_vsnprintf(big, (size_t)n + 1, fmt, again); va_end(again);
    fwrite(big, 1, (size_t)n, f); free(big); return n;
}
int os_fprintf(FILE *f, const char *fmt, ...){ va_list ap; va_start(ap, fmt); int n = os_vfprintf(f, fmt, ap); va_end(ap); return n; }
int os_printf(const char *fmt, ...){ va_list ap; va_start(ap, fmt); int n = os_vfprintf(stdout, fmt, ap); va_end(ap); return n; }

/* CLOCK_MONOTONIC from the performance counter. */
int clock_gettime(int clock, struct timespec *ts){
    (void)clock;
    static LARGE_INTEGER freq; if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    ts->tv_sec = (time_t)(c.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)((c.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}
#endif
