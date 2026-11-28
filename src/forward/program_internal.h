#ifndef LAPLACE_PROGRAM_INTERNAL_H
#define LAPLACE_PROGRAM_INTERNAL_H

#include "query.h"
#include "blake3.h"
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#define MAXOCC 256
#define LANGS 64
typedef struct { uint64_t w[MAXOCC / 64]; } Bits;
static inline void bit_set(Bits *b, int i){ if (i >= 0 && i < MAXOCC) b->w[i >> 6] |= 1ull << (i & 63); }
static inline int bit_count_and(const Bits *a, const Bits *b){ int n = 0; for (int i = 0; i < MAXOCC / 64; i++) n += __builtin_popcountll(a->w[i] & b->w[i]); return n; }
static inline void bit_clear(Bits *a, const Bits *b){ for (int i = 0; i < MAXOCC / 64; i++) a->w[i] &= ~b->w[i]; }

enum { R_CLAIM, R_FOLLOWS, R_DISCOURSE, R_SCAN, R_CONTAIN, R_LAYER, R_KINDS };
#define CELL_HEADS 8
typedef struct { lp_id id; double force, cost; Bits support, neg_support, direct_support; int routes[R_KINDS]; lp_id via, rel; lp_rating r; int has_r, hub, shared, segment, bridge, tier, lateral_hop; lp_id heads[CELL_HEADS]; int nheads, layer; } Cell;
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
    lp_id prompt, handle, program_id;
    const uint8_t *prompt_text; size_t prompt_len;
    lp_id occ[MAXOCC]; int nocc; double role[MAXOCC]; int composed[MAXOCC];
    int8_t polarity[MAXOCC];
    lp_id role_read[MAXOCC]; int role_votes[MAXOCC], role_seen[MAXOCC];    /* the reading each word is read as where it stands, by how many of how many observations of its run */
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
    lp_idmap *heads, *ctx_obs;
    const lp_id *witness_scope; int nwitness_scope, witness_scoped;
    const lp_envelope *envelope; lp_work_counters work; double t_start_s;
    double salience[MAXOCC], max_salience; LpSalienceRow *salience_rows; int nsalience;
    size_t occ_off[MAXOCC], occ_len[MAXOCC];
    long df_exact[MAXOCC];
    lp_id occ_lower[MAXOCC]; int has_occ_lower[MAXOCC];
    lp_id spans[MAXOCC]; Bits span_bits[MAXOCC]; int nspans;
    FILE *trace_out;
} State;

static inline double draw(const State *st, uint32_t a, uint32_t b, uint32_t c){
    blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, st->seed, 32); uint32_t v[3] = { a, b, c }; blake3_hasher_update(&h, v, sizeof v);
    uint8_t o[8]; blake3_hasher_finalize(&h, o, 8); uint64_t x; memcpy(&x, o, 8); return (double)(x >> 11) * 0x1.0p-53;
}
static inline int envelope_hit(State *st){
    st->work.wall_us = (uint64_t)((now() - st->t_start_s) * 1e6);
    return st->envelope && lp_work_exceeded(st->envelope, &st->work) != LP_LIMIT_NONE;
}
static inline int can_spend_probes(State *st, uint64_t need){
    if (envelope_hit(st)) return 0;
    if (st->envelope && st->envelope->max_db_probes > 0 &&
        st->work.db_probes + need > st->envelope->max_db_probes) return 0;
    return 1;
}
static inline PGresult *call_st(State *st, LpSqlId id, Args *a){
    st->trips++; st->work.db_probes++;
    PGresult *r = db_call(st->pg, id, a);
    if (r) st->work.rows_scanned += (uint64_t)PQntuples(r);
    return r;
}
static inline int head_read(const Firmware *fw, const lp_id *v, size_t n){
    if (lp_tuple_middle_any(v, n, fw->id.refuse, (size_t)fw->nrefuse_predicate)) return 0;
    if (fw->nonly_predicate && !lp_tuple_middle_any(v, n, fw->id.only, (size_t)fw->nonly_predicate)) return 0;
    return 1;
}
static inline int is_lexical_sense_rel(const State *st, const lp_id *rel){
    if (st->lexical.np > 0 && lp_id_eq(rel, &st->lexical.sense)) return 1;
    for (int u = 0; u < st->fw->nup; u++) if (lp_id_eq(rel, &st->fw->id.up[u])) return 1;
    static _Thread_local int ref_init = 0;
    static _Thread_local lp_id ref_id;
    if (!ref_init && st->c) {
        ref_id = lp_text_decompose(st->c, (const uint8_t *)"reference", 9, NULL, NULL).id;
        ref_init = 1;
    }
    return ref_init && lp_id_eq(rel, &ref_id);
}
static inline int is_gloss_rel(const State *st, const lp_id *rel){
    if (!st || !rel) return 0;
    if (st->lexical.has_gloss && lp_id_eq(rel, &st->lexical.gloss)) return 1;
    static _Thread_local int init = 0;
    static _Thread_local lp_id g0, g1, g2;
    if (!init && st->c) {
        g0 = lp_text_decompose(st->c, (const uint8_t *)"definition of the synset", 24, NULL, NULL).id;
        g1 = lp_text_decompose(st->c, (const uint8_t *)"English definition", 18, NULL, NULL).id;
        g2 = lp_text_decompose(st->c, (const uint8_t *)"standard definition of the synset", 33, NULL, NULL).id;
        init = 1;
    }
    return init && (lp_id_eq(rel, &g0) || lp_id_eq(rel, &g1) || lp_id_eq(rel, &g2));
}
static inline int is_lateral_hop_rel(const State *st, const lp_id *rel){
    if (!st || !rel || !st->fw) return 0;
    for (int i = 0; i < st->nocc; i++) if (lp_id_eq(&st->occ[i], rel)) return 0;
    if (st->fw->nlateral_relation > 0) {
        for (int i = 0; i < st->fw->nlateral_relation; i++) {
            if (lp_id_eq(rel, &st->fw->id.lateral[i])) return 1;
        }
        return 0;
    }
    return 0;
}
static inline int lp_is_internal_metadata_path(lp_text *c, const uint8_t *path, size_t path_len){
    static _Thread_local int init = 0;
    static _Thread_local lp_id tag = { { 0 } };
    if (!path || !path_len) return 0;
    lp_path p = lp_path_of(path, path_len);
    if (p.n == 0) return 0;
    if (lp_path_said(p, 0) == LP_SAID_METADATA) return 1;
    if (p.n == 2 && lp_path_said(p, 0) == LP_SAID_TUPLE) return 1;
    if (!init) {
        tag = lp_text_decompose(c, (const uint8_t *)"compiled reference binding v1", 29, NULL, NULL).id;
        init = 1;
    }
    lp_id first = lp_path_id(p, 0);
    return lp_id_eq(&first, &tag);
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

#define resolve_roles lp_program_resolve_roles
#define resolve_salience lp_program_resolve_salience
#define resolve_polarity lp_program_resolve_polarity
#define contextual_salience_lift lp_program_contextual_salience_lift
#define layer_round lp_program_layer_round
#define heads_say lp_program_heads_say
#define couple lp_program_couple
#define couple_ingest_holds lp_program_couple_ingest_holds
#define shape_measured lp_program_shape_measured
#define follows lp_program_follows
#define shape_near lp_program_shape_near
#define walk lp_program_walk
#define walk_seed_holds lp_program_walk_seed_holds
#define context lp_program_context
#define beside_grounds lp_program_beside_grounds
#define langs_of lp_program_langs_of
#define resolve_prompt_langs lp_program_resolve_prompt_langs
#define chain_from lp_program_chain_from
#define state_free lp_program_state_free

void resolve_roles(State *st);
void resolve_salience(State *st);
void resolve_polarity(State *st);
void contextual_salience_lift(State *st, const Field *fd);
int layer_round(State *st, Field *fd, int round, lp_idmap *heads_out);
void heads_say(State *st, lp_idmap *hm, const char *label);
int couple(State *st, Field *fd, const lp_id *ids, int n, const int *occ_of, int kind, Curve *near, int keep);
void couple_ingest_holds(State *st, Field *fd, const Hold *h, int nh, const int *hub, const lp_id *keys, const int *of, int nk, const int *occ_of, int kind);
int shape_measured(State *st, const lp_id *ids, int n, const int *occ_of, const Hold *h0, int nh0, Curve *near, int keep);
int follows(State *st, Next *out, int cap);
int shape_near(State *st, const lp_id *ids, int n, Curve *near, int keep);
void walk(State *st, int from, int to);
void walk_seed_holds(State *st, const lp_id *keys, int nk, const Hold *hh, int nh, const int *hub);
void context(State *st, int from, int to);
int beside_grounds(const State *st, int i, const lp_id *id);
int langs_of(State *st, const lp_id *w, int n, lp_id (*out)[LANGS], int *nout);
void resolve_prompt_langs(State *st, const Field *fd);
int chain_from(State *st, const lp_id *word, const lp_id *reading, lp_id *answer, lp_rating *rating);
void state_free(State *st);

void lp_program_append_out(char **out, size_t *outn, const char *tx, int is_follow, int combine_clauses);
int lp_program_is_prompt_echo(const uint8_t *prompt_text, size_t prompt_len, const char *tx, size_t tl, int is_follow);
int lp_program_should_stop_enough(int sel_is_follow, int combine_clauses, int rem, int owed0, double enough, int open_anchors);
int lp_program_format_claim_clause(const char *via_tx, const char *rel_tx, const char *val_tx, char *buf, size_t cap);

#endif
