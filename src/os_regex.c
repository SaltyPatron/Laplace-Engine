/* POSIX extended regular expressions for Windows (os_regex.h): a parsed pattern matched by backtracking. Every way of
 * matching at the leftmost position that matches is tried and the longest kept, with the groups as that way set them;
 * a step budget bounds the search on a pathological pattern, after which the best found so far stands. */
#ifdef _WIN32
#include "os_regex.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { N_CHAR, N_ANY, N_SET, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_GROUP, N_ALT, N_REPEAT };
typedef struct Node Node;
struct Node { int type; unsigned char c; unsigned char set[32]; int min, max, group; Node **kid; int nkid; Node *one; };
typedef struct { Node *root; Node **all; size_t nall, call; const char *p; int err, ngroup; } Parser;

static Node *node(Parser *P, int type){
    Node *n = calloc(1, sizeof *n); n->type = type;
    if (P->nall == P->call) { P->call = P->call ? P->call * 2 : 64; P->all = realloc(P->all, sizeof *P->all * P->call); }
    P->all[P->nall++] = n; return n;
}
static void push(Node *n, Node *k){ n->kid = realloc(n->kid, sizeof *n->kid * (size_t)(n->nkid + 1)); n->kid[n->nkid++] = k; }
static void set_bit(unsigned char *s, int c){ s[c >> 3] |= (unsigned char)(1 << (c & 7)); }
static int has_bit(const unsigned char *s, int c){ return s[c >> 3] >> (c & 7) & 1; }
static void set_class(unsigned char *s, const char *name, size_t n){
    for (int c = 0; c < 256; c++) {
        int in = !strncmp(name, "alpha", n) ? isalpha(c) : !strncmp(name, "digit", n) ? isdigit(c) : !strncmp(name, "alnum", n) ? isalnum(c)
               : !strncmp(name, "space", n) ? isspace(c) : !strncmp(name, "upper", n) ? isupper(c) : !strncmp(name, "lower", n) ? islower(c)
               : !strncmp(name, "punct", n) ? ispunct(c) : !strncmp(name, "xdigit", n) ? isxdigit(c) : !strncmp(name, "blank", n) ? (c == ' ' || c == '\t')
               : !strncmp(name, "cntrl", n) ? iscntrl(c) : !strncmp(name, "print", n) ? isprint(c) : !strncmp(name, "graph", n) ? isgraph(c) : 0;
        if (in) set_bit(s, c);
    }
}
static int word(int c){ return c == '_' || isalnum(c); }

static Node *parse_alt(Parser *P);
static Node *parse_atom(Parser *P){
    const char *p = P->p; Node *n;
    switch (*p) {
    case '(': { P->p = p + 1; n = node(P, N_GROUP); n->group = ++P->ngroup; n->one = parse_alt(P);
                if (*P->p != ')') { P->err = REG_BADPAT; return n; } P->p++; return n; }
    case '.': P->p = p + 1; return node(P, N_ANY);
    case '^': P->p = p + 1; return node(P, N_BOL);
    case '$': P->p = p + 1; return node(P, N_EOL);
    case '[': {
        n = node(P, N_SET); p++; int neg = *p == '^'; if (neg) p++;
        int first = 1;
        while (*p && (first || *p != ']')) {
            first = 0;
            if (*p == '[' && p[1] == ':') { const char *e = strstr(p + 2, ":]"); if (!e) { P->err = REG_BADPAT; return n; } set_class(n->set, p + 2, (size_t)(e - p - 2)); p = e + 2; continue; }
            int lo = (unsigned char)*p++;
            if (*p == '-' && p[1] && p[1] != ']') { p++; int hi = (unsigned char)*p++; for (int c = lo; c <= hi; c++) set_bit(n->set, c); }
            else set_bit(n->set, lo);
        }
        if (*p != ']') { P->err = REG_BADPAT; return n; }
        if (neg) for (int i = 0; i < 32; i++) n->set[i] = (unsigned char)~n->set[i];
        P->p = p + 1; return n; }
    case '\\': {
        p++; if (!*p) { P->err = REG_BADPAT; return node(P, N_CHAR); }
        P->p = p + 1;
        switch (*p) {
        case 'w': case 'W': n = node(P, N_SET); for (int c = 0; c < 256; c++) if (word(c) == (*p == 'w')) set_bit(n->set, c); return n;
        case 's': case 'S': n = node(P, N_SET); for (int c = 0; c < 256; c++) if (!!isspace(c) == (*p == 's')) set_bit(n->set, c); return n;
        case 'd': case 'D': n = node(P, N_SET); for (int c = 0; c < 256; c++) if (!!isdigit(c) == (*p == 'd')) set_bit(n->set, c); return n;
        case 'b': return node(P, N_WORDB);
        case 'B': return node(P, N_NWORDB);
        case 'n': n = node(P, N_CHAR); n->c = '\n'; return n;
        case 't': n = node(P, N_CHAR); n->c = '\t'; return n;
        default: n = node(P, N_CHAR); n->c = (unsigned char)*p; return n;
        } }
    default: P->p = p + 1; n = node(P, N_CHAR); n->c = (unsigned char)*p; return n;
    }
}
static Node *parse_seq(Parser *P){
    Node *seq = node(P, N_ALT);                        /* one alternative: its items in order */
    while (*P->p && *P->p != '|' && *P->p != ')' && !P->err) {
        Node *a = parse_atom(P);
        for (;;) {
            int min, max; const char *p = P->p;
            if (*p == '*') { min = 0; max = -1; p++; }
            else if (*p == '+') { min = 1; max = -1; p++; }
            else if (*p == '?') { min = 0; max = 1; p++; }
            else if (*p == '{' && isdigit((unsigned char)p[1])) { char *e; min = (int)strtol(p + 1, &e, 10); max = min;
                if (*e == ',') { e++; max = isdigit((unsigned char)*e) ? (int)strtol(e, &e, 10) : -1; }
                if (*e != '}') { P->err = REG_BADPAT; return seq; } p = e + 1; }
            else break;
            P->p = p; Node *r = node(P, N_REPEAT); r->min = min; r->max = max; r->one = a; a = r;
        }
        push(seq, a);
    }
    return seq;
}
static Node *parse_alt(Parser *P){
    Node *alt = node(P, N_ALT); alt->group = -1;      /* alternatives, each a sequence */
    push(alt, parse_seq(P));
    while (*P->p == '|' && !P->err) { P->p++; push(alt, parse_seq(P)); }
    return alt;
}

typedef struct { Node *root; Node **all; size_t nall; } Prog;

int regcomp(regex_t *re, const char *pattern, int cflags){
    (void)cflags; Parser P = { 0 }; P.p = pattern;
    Node *root = parse_alt(&P);
    if (!P.err && *P.p) P.err = REG_BADPAT;              /* an unmatched ')' */
    Prog *g = malloc(sizeof *g); g->root = root; g->all = P.all; g->nall = P.nall;
    re->prog = g; re->re_nsub = (size_t)P.ngroup; re->nosub = (cflags & REG_NOSUB) != 0;
    if (P.err) { regfree(re); return P.err; }
    return 0;
}
void regfree(regex_t *re){
    Prog *g = re->prog; if (!g) return;
    for (size_t i = 0; i < g->nall; i++) { free(g->all[i]->kid); free(g->all[i]); }
    free(g->all); free(g); re->prog = NULL;
}
size_t regerror(int code, const regex_t *re, char *buf, size_t cap){
    (void)re; const char *m = code == REG_NOMATCH ? "no match" : code == REG_ESPACE ? "out of memory" : "invalid regular expression";
    if (cap) { strncpy(buf, m, cap - 1); buf[cap - 1] = 0; } return strlen(m) + 1;
}

/* ---- matching: a continuation is what is left to match after the node at hand */
enum { K_SEQ, K_CLOSE, K_REPEAT };
typedef struct Cont { int type; const Node *seq; int i; const Node *rep; int count, from; const struct Cont *next; } Cont;
typedef struct { const char *s; int len; regmatch_t *m; int nm; regmatch_t *best; int best_end; long steps; } Match;

static int run(Match *M, const Cont *k, int pos);
static int run_node(Match *M, const Node *n, const Cont *next, int pos){
    const char *s = M->s;
    switch (n->type) {
    case N_CHAR: return pos < M->len && (unsigned char)s[pos] == n->c ? run(M, next, pos + 1) : -1;
    case N_ANY: return pos < M->len ? run(M, next, pos + 1) : -1;
    case N_SET: return pos < M->len && has_bit(n->set, (unsigned char)s[pos]) ? run(M, next, pos + 1) : -1;
    case N_BOL: return pos == 0 ? run(M, next, pos) : -1;
    case N_EOL: return pos == M->len ? run(M, next, pos) : -1;
    case N_WORDB: case N_NWORDB: {
        int a = pos > 0 && word((unsigned char)s[pos - 1]), b = pos < M->len && word((unsigned char)s[pos]);
        return ((a != b) == (n->type == N_WORDB)) ? run(M, next, pos) : -1; }
    case N_GROUP: {
        regmatch_t was = n->group < M->nm ? M->m[n->group] : (regmatch_t){ -1, -1 };
        if (n->group < M->nm) M->m[n->group].rm_so = pos;
        Cont close = { K_CLOSE, NULL, n->group, NULL, 0, 0, next };
        int r = run_node(M, n->one, &close, pos);
        if (n->group < M->nm) M->m[n->group] = was;    /* every path restores what it set: the best copy keeps its own */
        return r; }
    case N_ALT: {
        int r = -1;
        for (int a = 0; a < n->nkid; a++) {            /* each alternative: its sequence, then what follows */
            const Node *seq = n->kid[a];
            if (!seq->nkid) { int t = run(M, next, pos); if (t > r) r = t; continue; }
            Cont k = { K_SEQ, seq, 0, NULL, 0, 0, next };
            int t = run(M, &k, pos); if (t > r) r = t;
        }
        return r; }
    case N_REPEAT: { Cont k = { K_REPEAT, NULL, 0, n, 0, pos, next }; return run(M, &k, pos); }
    }
    return -1;
}
static int run(Match *M, const Cont *k, int pos){
    if (++M->steps > 4000000) return -1;               /* the budget: a pathological pattern stops here with its best */
    if (!k) {                                          /* the whole pattern matched: the longest end wins, with its groups */
        if (pos > M->best_end) { M->best_end = pos; if (M->best) memcpy(M->best, M->m, sizeof *M->m * (size_t)M->nm); }
        return pos;
    }
    switch (k->type) {
    case K_SEQ: {
        const Node *item = k->seq->kid[k->i];
        if (k->i + 1 < k->seq->nkid) { Cont n = { K_SEQ, k->seq, k->i + 1, NULL, 0, 0, k->next }; return run_node(M, item, &n, pos); }
        return run_node(M, item, k->next, pos); }
    case K_CLOSE: {
        regmatch_t was = k->i < M->nm ? M->m[k->i] : (regmatch_t){ -1, -1 };
        if (k->i < M->nm) M->m[k->i].rm_eo = pos;
        int r = run(M, k->next, pos);
        if (k->i < M->nm) M->m[k->i] = was;
        return r; }
    case K_REPEAT: {
        const Node *n = k->rep; int r = -1;
        int more = (n->max < 0 || k->count < n->max) && !(k->count > 0 && pos == k->from);   /* an empty iteration ends the loop */
        if (more) { Cont again = { K_REPEAT, NULL, 0, n, k->count + 1, pos, k->next }; r = run_node(M, n->one, &again, pos); }
        if (k->count >= n->min) { int t = run(M, k->next, pos); if (t > r) r = t; }
        return r; }
    }
    return -1;
}

int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *pm, int eflags){
    (void)eflags; const Prog *g = re->prog; if (!g) return REG_NOMATCH;
    int len = (int)strlen(s), nm = (int)re->re_nsub + 1;
    regmatch_t *m = malloc(sizeof *m * (size_t)nm), *best = malloc(sizeof *m * (size_t)nm);
    Match M = { s, len, m, nm, best, -1, 0 };
    int found = -1;
    for (int start = 0; start <= len && found < 0; start++) {
        for (int i = 0; i < nm; i++) m[i].rm_so = m[i].rm_eo = -1;
        M.best_end = -1; M.steps = 0;
        run_node(&M, g->root, NULL, start);
        if (M.best_end >= 0) { found = start; best[0].rm_so = start; best[0].rm_eo = M.best_end; }
    }
    if (found >= 0 && !re->nosub) for (size_t i = 0; i < nmatch; i++) pm[i] = (int)i < nm ? best[i] : (regmatch_t){ -1, -1 };
    free(m); free(best);
    return found >= 0 ? 0 : REG_NOMATCH;
}
#else
typedef int os_regex_not_needed_here;
#endif
