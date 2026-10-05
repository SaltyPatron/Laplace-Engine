/* The Engine's one platform layer: everything it asks of the operating system beyond standard C, behind one
 * contract. On POSIX each call is the libc call it replaces, so behaviour there is unchanged; Windows implements the
 * same semantics with Win32. Paths are UTF-8 with '/' separators on every platform. */
#ifndef LAPLACE_OS_H
#define LAPLACE_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#define OS_DLL_SUFFIX ".dll"
#else
#define OS_DLL_SUFFIX ".so"
#endif

/* ---------------------------------------------------------------- what the C library lacks on Windows
 * GNU and POSIX extensions the Engine uses under their own names; Windows gets them from os.c. printf's ' flag
 * (thousands grouping) is not in the Universal CRT, and an unknown flag there is an invalid parameter: the format is
 * copied without it, so Windows prints 1234567 where Linux prints 1,234,567. */
#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
void *os_memmem(const void *hay, size_t hn, const void *needle, size_t nn);
char *os_strndup(const char *s, size_t n);
ssize_t os_getline(char **line, size_t *cap, FILE *f);
int os_printf(const char *fmt, ...);
int os_fprintf(FILE *f, const char *fmt, ...);
int os_snprintf(char *s, size_t n, const char *fmt, ...);
static inline uint32_t os_htonl(uint32_t x){ return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24); }
static inline uint16_t os_htons(uint16_t x){ return (uint16_t)((x >> 8) | (x << 8)); }
int os_rand_r(unsigned *seed);                        /* glibc's rand_r, step for step: the same sequence on every machine */
void os_qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg);
#define memmem os_memmem
#define strndup os_strndup
#define getline os_getline
#define strtok_r strtok_s
#define printf os_printf
#define fprintf os_fprintf
#define snprintf os_snprintf
#define htonl os_htonl
#define ntohl os_htonl
#define htons os_htons
#define ntohs os_htons
#define rand_r os_rand_r
#define qsort_r os_qsort_r
/* A mutex under pthread's names (a slim reader/writer lock, held exclusively). */
typedef struct { void *p; } pthread_mutex_t;
typedef int pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
/* statx, answered with what Windows keeps of a file: size, links, the file's index as its inode, the volume's serial
 * as its device, its attributes, and its four times (creation is the birth time). Mode is made from the attributes;
 * owner and group are not Windows notions and are not in the mask. */
struct statx_timestamp { int64_t tv_sec; uint32_t tv_nsec; int32_t pad_; };
struct statx { uint32_t stx_mask, stx_blksize; uint64_t stx_attributes; uint32_t stx_nlink, stx_uid, stx_gid; uint16_t stx_mode, pad_;
               uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask; struct statx_timestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
               uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor; };
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
#define AT_FDCWD (-100)
int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *out);
#endif

/* ---------------------------------------------------------------- libraries */
/* A loaded library and its symbols (dlopen/dlsym; LoadLibrary/GetProcAddress). */
void *os_dl_open(const char *path);
void *os_dl_sym(void *lib, const char *symbol);
const char *os_dl_error(void);                        /* why the last os_dl_open failed */

/* ---------------------------------------------------------------- files and directories */
/* A list of paths or names, owned by the list. */
typedef struct { char **item; size_t n; } os_list;
void os_list_free(os_list *);

/* Every existing path a pattern names (glob(3) with flags 0): '*', '?' and '[...]' within one path component, a
 * leading '.' matched only by a leading '.', sorted byte-wise. No match is an empty list. */
void os_glob(const char *pattern, os_list *out);

/* The names in a directory, '.' and '..' included, unsorted; 0, or -1 when it cannot be read (errno set). */
int os_list_dir(const char *dir, os_list *out);

/* Whether anything exists at a path; whether a directory is there. */
int os_exists(const char *path);
int os_is_dir(const char *path);

/* A regular file's size; -1 when there is no such file. */
int os_file_size(const char *path, uint64_t *size);

/* A directory made (0 when it is there afterwards, -1 otherwise); every directory along a path. */
int os_mkdir(const char *path);
int os_mkdirs(const char *path);

/* The absolute path a path names, symbolic links resolved (realpath); NULL when it cannot be resolved. */
char *os_realpath(const char *path, char *out, size_t cap);

/* This executable's path; 0, or -1. */
int os_self_path(char *out, size_t cap);

/* Every regular file under a directory, by its path and size, in the order the directories list them: a name
 * beginning with '.' is passed over, and so is any symbolic link (nftw with FTW_PHYS, as the ingest walked). */
void os_walk(const char *dir, void (*cb)(const char *path, uint64_t size, void *arg), void *arg);

/* The volume holding a path: bytes free to this user and bytes in all; 0, or -1 when they cannot be seen. */
int os_disk_space(const char *path, double *free_bytes, double *total_bytes);

/* fnmatch(pattern, string, 0): 0 when the string matches. '*' and '?' match '/' too; '\' escapes. */
int os_fnmatch(const char *pattern, const char *string);

/* A path as the Engine writes paths: '/' separators (Windows accepts them). In place; a no-op on POSIX. */
void os_slashes(char *path);

/* ---------------------------------------------------------------- memory and time */
/* A whole file mapped read-only (NULL when it cannot be), and an address range reserved once and committed as it is
 * touched (the node table's arenas; exits when the reservation fails). */
void *os_map_file(const char *path, size_t *len);
void *os_reserve(size_t bytes);
/* Pages of a reserved range handed back: zero when touched again (madvise MADV_DONTNEED; decommitted and committed). */
void os_discard(void *p, size_t bytes);

/* Seconds on a monotonic clock. */
double os_now(void);

/* ---------------------------------------------------------------- processes */
/* A program run with these arguments (argv[0] the program, NULL-terminated), its stdout and stderr written to a log
 * file made afresh, one environment variable set for it; waited for. Its exit code, 128 + the signal when it was
 * killed, or 127 when it could not be started. */
int os_run_logged(const char *program, char *const argv[], const char *log, const char *envname, const char *envval);

#endif
