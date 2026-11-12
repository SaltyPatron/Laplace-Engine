#ifndef LAPLACE_PROGRAM_INTERNAL_H
#define LAPLACE_PROGRAM_INTERNAL_H

#include "query.h"
#include "blake3.h"
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define MAXOCC 256
#define LANGS 64
typedef struct { uint64_t w[MAXOCC / 64]; } Bits;
static inline void bit_set(Bits *b, int i){ if (i >= 0 && i < MAXOCC) b->w[i >> 6] |= 1ull << (i & 63); }
static inline int bit_count_and(const Bits *a, const Bits *b){ int n = 0; for (int i = 0; i < MAXOCC / 64; i++) n += __builtin_popcountll(a->w[i] & b->w[i]); return n; }
static inline void bit_clear(Bits *a, const Bits *b){ for (int i = 0; i < MAXOCC / 64; i++) a->w[i] &= ~b->w[i]; }

enum { R_CLAIM, R_FOLLOWS, R_DISCOURSE, R_SCAN, R_CONTAIN, R_LAYER, R_KINDS };
#define CELL_HEADS 8
typedef struct { lp_id id; double force, cost; Bits support; int routes[R_KINDS]; lp_id via, rel; lp_rating r; int has_r, hub, shared, segment, tier; lp_id heads[CELL_HEADS]; int nheads, layer; } Cell;
typedef struct { Cell *c; int n; size_t cap; lp_idmap *m; } Field;
static inline Cell *cell(Field *f, const lp_id *id){
    if (!f->m) f->m = lp_idmap_new(); bool fresh; size_t i = lp_idmap_put(f->m, id, &fresh);
    if (!fresh) return &f->c[i];
    lp_reserve((void **)&f->c, &f->cap, (size_t)f->n + 1, sizeof(Cell));
    Cell *x = &f->c[f->n++]; memset(x, 0, sizeof *x); x->id = *id; x->cost = INFINITY; return x;
}
static inline Cell *cell_find(const Field *f, const lp_id *id){ int64_t i = lp_idmap_find(f->m, id); return i < 0 ? NULL : &f->c[i]; }
static inline void field_free(Field *f){ free(f->c); lp_idmap_free(f->m); memset(f, 0, sizeof *f); }

typedef struct {
    const Firmware *fw; PGconn *pg; Reader *rd; lp_text *c;
    Lexicalization lexical;
    lp_id prompt; lp_id occ[MAXOCC]; int nocc; double role[MAXOCC]; int composed[MAXOCC];
    lp_id disc[MAXOCC]; int ndisc;
    lp_id traj[4096]; int ntraj;
    Bits open;
    uint64_t trips;
    uint64_t admitted;
    uint64_t refused;
    uint8_t seed[32];
    int npos;
    lp_id pass[4096]; int npass;
    lp_id lang[8]; int nlang;
    long df[MAXOCC], df_max; int df_ready;
    lp_idmap *walk_at; int *walk_visits; size_t walk_cap; uint64_t walk_steps, walk_homes; int walk_read;
    lp_idmap *walk_am; struct Adj_ *walk_adj; size_t walk_acap; int walk_nadj;
    lp_idmap *ctx[MAXOCC]; int ctx_n[MAXOCC], ctx_hub[MAXOCC]; lp_idmap *ctx_all; int ctx_total;
} State;

static inline double draw(const State *st, uint32_t a, uint32_t b, uint32_t c){
    blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, st->seed, 32); uint32_t v[3] = { a, b, c }; blake3_hasher_update(&h, v, sizeof v);
    uint8_t o[8]; blake3_hasher_finalize(&h, o, 8); uint64_t x; memcpy(&x, o, 8); return (double)(x >> 11) * 0x1.0p-53;
}
static inline PGresult *ask_st(State *st, const char *sql, Args *a){ st->trips++; return ask(st->pg, sql, a); }
static inline int head_read(const Firmware *fw, const lp_id *v, size_t n){
    if (lp_tuple_middle_any(v, n, fw->id.refuse, (size_t)fw->nrefuse_predicate)) return 0;
    if (fw->nonly_predicate && !lp_tuple_middle_any(v, n, fw->id.only, (size_t)fw->nonly_predicate)) return 0;
    return 1;
}
static inline void pass_add(State *st, const lp_id *id){
    if (st->npass >= 4096 || lp_tier0_codepoint(T0, id) >= 0) return;
    if (st->npass && lp_id_eq(&st->pass[st->npass - 1], id)) return;
    st->pass[st->npass++] = *id;
}
static inline int visits(const State *st, const lp_id *id, int o){ if (!st->walk_at) return 0; int64_t i = lp_idmap_find(st->walk_at, id); return i < 0 ? 0 : st->walk_visits[(size_t)i * MAXOCC + (size_t)o]; }
static inline int beside(const State *st, int i, const lp_id *id){ if (!st->ctx[i]) return 0; int64_t k = lp_idmap_find(st->ctx[i], id); return k < 0 ? 0 : (int)*lp_idmap_value(st->ctx[i], (size_t)k); }

typedef struct { lp_id id; double d; lp_id *v; int nv; } Curve;
typedef struct { long strands; double force; Bits support; } HeadStat;
typedef struct { lp_id id; long times; int len; } Next;

#define HEADS lp_program_HEADS
#define resolve_roles lp_program_resolve_roles
#define layer_round lp_program_layer_round
#define heads_say lp_program_heads_say
#define couple lp_program_couple
#define follows lp_program_follows
#define shape_near lp_program_shape_near
#define walk lp_program_walk
#define context lp_program_context
#define beside_grounds lp_program_beside_grounds
#define langs_of lp_program_langs_of
#define chain_from lp_program_chain_from

extern lp_idmap *HEADS;
void resolve_roles(State *st);
int layer_round(State *st, Field *fd, int round, lp_idmap *heads_out);
void heads_say(State *st, lp_idmap *hm, const char *label);
int couple(State *st, Field *fd, const lp_id *ids, int n, const int *occ_of, int kind, Curve *near, int keep);
int follows(State *st, Next *out, int cap);
int shape_near(State *st, const lp_id *ids, int n, Curve *near, int keep);
void walk(State *st, int from, int to);
void context(State *st, int from, int to);
int beside_grounds(const State *st, int i, const lp_id *id);
int langs_of(State *st, const lp_id *w, int n, lp_id (*out)[LANGS], int *nout);
int chain_from(State *st, const lp_id *word, const lp_id *reading, lp_id *answer, lp_rating *rating);

#endif
