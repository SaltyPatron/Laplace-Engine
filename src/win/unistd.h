/* What the Engine takes from <unistd.h> on Windows. fork has no Windows form: it fails with ENOSYS, and the one caller
 * that needs it (ingest's per-source process and batch writer) reports that. */
#ifndef LAPLACE_WIN_UNISTD_H
#define LAPLACE_WIN_UNISTD_H
#include <io.h>
#include <process.h>
#include <stdlib.h>
typedef int pid_t;
pid_t fork(void);
int pipe(int fd[2]);
int setenv(const char *name, const char *value, int overwrite);
char *realpath(const char *path, char *resolved);
#define dup2 _dup2
#include <sys/stat.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
/* readlink: "/proc/self/exe" names this program (GetModuleFileName), as on Linux; no other link is read. */
long long readlink(const char *path, char *buf, size_t size);
#endif
