/* <sys/stat.h> on Windows, with sizes in 64 bits: the CRT's struct stat holds a file's size in a long, 32 bits there,
 * so stat() fails on a file of 2 GB or more and the file is read as empty (Wiktionary's 2.9 and 21.9 GB dumps were).
 * Every stat and fstat of the Engine is the 64-bit one. */
#ifndef LAPLACE_WIN_SYS_STAT_H
#define LAPLACE_WIN_SYS_STAT_H
#include_next <sys/stat.h>
#define stat _stat64
#define fstat _fstat64
#endif
