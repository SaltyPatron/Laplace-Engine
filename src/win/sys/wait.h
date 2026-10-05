/* waitpid(2) on Windows: there are no forked children (see unistd.h), so it fails. */
#ifndef LAPLACE_WIN_WAIT_H
#define LAPLACE_WIN_WAIT_H
#include <unistd.h>
pid_t waitpid(pid_t pid, int *status, int options);
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define WTERMSIG(s)    ((s) & 0x7f)
#endif
