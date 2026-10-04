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
#endif
