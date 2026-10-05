/* glob(3) on Windows over the platform layer's os_glob: flags 0, sorted byte-wise. */
#ifndef LAPLACE_WIN_GLOB_H
#define LAPLACE_WIN_GLOB_H
#include <stddef.h>
typedef struct { size_t gl_pathc; char **gl_pathv; size_t gl_offs; } glob_t;
int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g);
void globfree(glob_t *g);
#endif
