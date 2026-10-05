/* POSIX regular expressions where the C library has none (Windows): regcomp, regexec and regfree with the flags the
 * Engine uses (REG_EXTENDED, REG_NOSUB) and regmatch_t, over POSIX extended syntax: alternation, groups, the
 * quantifiers * + ? {m,n}, bracket expressions with ranges, negation and the named classes, '.', '^' and '$', '\'
 * escaping. Matching is by backtracking: the leftmost match, and at that position the longest the whole pattern can
 * make, as POSIX requires of the whole; a subexpression's extent is the one that path takes, where POSIX would take
 * the leftmost-longest of each in turn. Bytes, not characters: a multibyte UTF-8 sequence is matched by '.' one byte at
 * a time and inside brackets byte by byte, which is what the Engine's patterns need. On POSIX the system's regex.h
 * is used instead. */
#ifndef LAPLACE_OS_REGEX_H
#define LAPLACE_OS_REGEX_H
#ifndef _WIN32
#include <regex.h>
#else
#include <stddef.h>

typedef long regoff_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;
typedef struct { void *prog; size_t re_nsub; int nosub; } regex_t;

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8
#define REG_NOTBOL   1
#define REG_NOTEOL   2
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ESPACE   12

int regcomp(regex_t *re, const char *pattern, int cflags);
int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *m, int eflags);
void regfree(regex_t *re);
size_t regerror(int code, const regex_t *re, char *buf, size_t cap);
#endif
#endif
