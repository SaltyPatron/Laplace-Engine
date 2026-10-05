/* statvfs(3) on Windows: the fields the Engine reads, from GetDiskFreeSpaceEx. */
#ifndef LAPLACE_WIN_STATVFS_H
#define LAPLACE_WIN_STATVFS_H
struct statvfs { unsigned long long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail; };
int statvfs(const char *path, struct statvfs *v);
#endif
