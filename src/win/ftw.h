/* nftw(3) on Windows, over the platform layer's directory listing: the flags and callbacks the Engine uses. */
#ifndef LAPLACE_WIN_FTW_H
#define LAPLACE_WIN_FTW_H
#include <sys/stat.h>
enum { FTW_F, FTW_D, FTW_DNR, FTW_NS, FTW_SL, FTW_DP, FTW_SLN };
enum { FTW_PHYS = 1, FTW_MOUNT = 2, FTW_DEPTH = 8, FTW_ACTIONRETVAL = 16 };
enum { FTW_CONTINUE = 0, FTW_STOP = 1, FTW_SKIP_SUBTREE = 2, FTW_SKIP_SIBLINGS = 3 };
struct FTW { int base; int level; };
int nftw(const char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *), int fds, int flags);
#endif
