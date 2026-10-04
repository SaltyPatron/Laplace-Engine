/* The Engine's one platform layer: everything it asks of the operating system beyond standard C, behind one
 * contract. On POSIX each call is the libc call it replaces, so behaviour there is unchanged; Windows implements the
 * same semantics with Win32. Paths are UTF-8 with '/' separators on every platform. */
#ifndef LAPLACE_OS_H
#define LAPLACE_OS_H

#include <stddef.h>

#ifdef _WIN32
#define OS_DLL_SUFFIX ".dll"
#else
#define OS_DLL_SUFFIX ".so"
#endif

/* A loaded library and its symbols (dlopen/dlsym; LoadLibrary/GetProcAddress). */
void *os_dl_open(const char *path);
void *os_dl_sym(void *lib, const char *symbol);
const char *os_dl_error(void);                        /* why the last os_dl_open failed */

/* A list of paths or names, owned by the list. */
typedef struct { char **item; size_t n; } os_list;
void os_list_free(os_list *);

/* Every existing path a pattern names (glob(3) with flags 0): '*', '?' and '[...]' within one path component, a
 * leading '.' matched only by a leading '.', sorted byte-wise. No match is an empty list. */
void os_glob(const char *pattern, os_list *out);

/* The names in a directory, '.' and '..' included, unsorted; 0, or -1 when it cannot be read (errno set). */
int os_list_dir(const char *dir, os_list *out);

/* Whether anything exists at a path. */
int os_exists(const char *path);

/* fnmatch(pattern, string, 0): 0 when the string matches. '*' and '?' match '/' too; '\' escapes. */
int os_fnmatch(const char *pattern, const char *string);

/* A path as the Engine writes paths: '/' separators (Windows accepts them). In place; a no-op on POSIX. */
void os_slashes(char *path);

#endif
