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

/* Address space reserved and committed as the kernel hands out pages (mmap anonymous, MAP_NORESERVE; VirtualAlloc
 * reserve + commit): zero-filled, NULL on failure. */
void *os_reserve(size_t bytes);
/* A file mapped read-only (mmap; MapViewOfFile): its bytes and size, NULL on failure. */
const void *os_map_read(const char *path, size_t *size);
/* Pages of reserved memory handed back: they read as zero when touched again (madvise MADV_DONTNEED). */
void os_discard(void *p, size_t bytes);

#ifdef _WIN32
/* The POSIX calls the Engine uses, on Windows: same names and semantics, so callers do not change. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
typedef struct { void *opaque; } pthread_mutex_t;             /* an SRWLOCK: one pointer, zero-initialised */
#define PTHREAD_MUTEX_INITIALIZER { 0 }
int pthread_mutex_init(pthread_mutex_t *, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
void *memmem(const void *hay, size_t hn, const void *needle, size_t nn);
char *strndup(const char *s, size_t n);
typedef long long ssize_t;
ssize_t getline(char **line, size_t *cap, FILE *f);
#define strtok_r strtok_s
#define CLOCK_MONOTONIC 1
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
int clock_gettime(int clock, struct timespec *ts);
/* glibc's rand_r, step for step, so a seed gives the same sequence on both systems. */
int rand_r(unsigned int *seed);
/* glibc's qsort_r argument order (comparator takes the context last); a stable merge sort. */
void qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg);
/* statx(2): what Windows records of a file, under Linux's field names; a field Windows lacks is left out of
 * stx_mask (the caller already treats a missing field as absent). */
#include <stdint.h>
struct statx_timestamp { int64_t tv_sec; uint32_t tv_nsec; int32_t __reserved; };
struct statx { uint32_t stx_mask, stx_blksize; uint64_t stx_attributes; uint32_t stx_nlink, stx_uid, stx_gid; uint16_t stx_mode;
               uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
               struct statx_timestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
               uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor; };
#define AT_FDCWD -100
#define STATX_TYPE 0x1u
#define STATX_MODE 0x2u
#define STATX_NLINK 0x4u
#define STATX_UID 0x8u
#define STATX_GID 0x10u
#define STATX_ATIME 0x20u
#define STATX_MTIME 0x40u
#define STATX_CTIME 0x80u
#define STATX_INO 0x100u
#define STATX_SIZE 0x200u
#define STATX_BLOCKS 0x400u
#define STATX_BASIC_STATS 0x7ffu
#define STATX_BTIME 0x800u
int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *x);
/* printf's "'" flag (thousands grouping, which the Engine's counts use under LC_NUMERIC en_US.UTF-8): the Windows C
 * runtime has none, so the three printf forms the Engine calls group the digits themselves, as glibc does. */
#include <stdarg.h>
int os_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
int os_snprintf(char *out, size_t cap, const char *fmt, ...);
int os_fprintf(FILE *f, const char *fmt, ...);
int os_printf(const char *fmt, ...);
#define snprintf os_snprintf
#define fprintf os_fprintf
#define printf os_printf
#else
#include <pthread.h>
#endif

#endif
