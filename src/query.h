/* Query policy and navigation. Record readers do not depend on these declarations. */
#ifndef LAPLACE_QUERY_H
#define LAPLACE_QUERY_H
#include "engine.h"

/* ---- the personality firmware: a pull's decisions, read from a file, never from the records (firmware.c) */
enum { FW_HOP, FW_SEARCH, FW_TRANSLATE, FW_FOLLOWS, FW_PULL, FW_OPS };
enum { FW_FRECHET, FW_OUTLIERS, FW_DTW, FW_EDR };
enum { FW_TAKE_FACT, FW_TAKE_SEGMENT, FW_TAKE_ATTESTATIONS, FW_TAKE_CONSTITUENTS, FW_TAKE_CHAIN };
enum { FW_TIE_FIRST, FW_TIE_DRAW, FW_TIE_ASK };
enum { FW_E_GROUNDS, FW_E_CONTINUITY, FW_E_AGREE, FW_E_COOCCUR, FW_E_WALKS, FW_E_SHAPE, FW_E_CONFIDENCE, FW_E_SHARED, FW_E_HEADS, FW_E_KEYS };
#define FW_CHAIN 8
#define FW_ALTS 4
#define FW_NAMES 32
#define FW_TAKES 16
#define FW_WEIGHS 128
typedef struct {
    char path[4200];
    double k, lambda;                                 /* how far below its rating a standing must hold; the tax on a hop */
    int fan, hops;
    int emit;                                         /* how many constituents a turn may emit (the emission budget STEER is given) */
    double enough;                                    /* a turn is complete when no more than this of what its obligations owed is still owed (0: all of it answered) */
    double top_within;                                /* 0: the top every time; else how near a tie has to be */
    double fact;                                      /* the trust at which a curated member is returned as one fact; above 1: never */
    int order_witness;                                /* on an open claim, the witness's own order before the standing */
    int shape; double shape_n;
    int walks, steps, walk_fan, nearest, rounds, breadth; double restart;   /* rounds: coupling rounds (layers); breadth: how many responding entities a round carries on */                 /* walkers from each word, the steps each walks, how often one goes home: the walks' share of the evidence */
    double sure;                                      /* how many standard errors of the walks separate two counts; within them, the walks cannot tell two apart */
    double lift;                                      /* how much more often than its base rate a word must be observed beside an occurrence to ground it */
    int elect[FW_E_KEYS], nelect;                     /* the election order: the evidence keys a proposal is compared by, first to last */
    int tie, seed_session;                            /* what a real tie gets (first, draw, ask); whether the seed holds the session and turn */
    char refuse_predicate[FW_NAMES][96], refuse_witness[FW_NAMES][96]; int nrefuse_predicate, nrefuse_witness;
    char only_predicate[FW_NAMES][96]; int nonly_predicate;
    char prevalence[96]; int prevalence_pair;      /* prevalence_pair: the count is of the pair [word, concept] itself */                              /* the relation by which a source counts a word's concepts (a tagged corpus): the prior of a fork */   /* when any is named, only strands whose relation is one of these are read: one head, or a few, alone */
    struct { int what, n; } take[FW_TAKES]; int ntake; /* a pull's steps, in order */
    char weigh_name[FW_WEIGHS][96]; double weigh[FW_WEIGHS]; int nweigh;
    char role_by[96], role_name[FW_WEIGHS][96]; double role[FW_WEIGHS]; int nrole;      /* how hard a word pulls, by what is attested of it under role_by (its part of speech) */
    char chain[FW_ALTS][FW_CHAIN][96]; int nchain[FW_ALTS], nalt;                                             /* the relations a pull follows from the word that pulls hardest, in order */
    char lexical[4][96], gloss[2][96], equiv[5][96], usages[2][96]; int nequiv;                                                                         /* translation: the recipe, the thing it names a lexicalization, and its parts that are the language and the concept; the recipe and the part whose relation says what is shown of a concept; the recipe and the part whose relation states how often a sense is observed (usages, pull.c) */
    char up[FW_CHAIN][96]; int nup; char language[2][96];
    struct { int ready; lp_id prevalence; lp_id only[FW_NAMES], refuse[FW_NAMES], refuse_witness[FW_NAMES], weigh[FW_WEIGHS], role_by, role[FW_WEIGHS], chain[FW_ALTS][FW_CHAIN], up[FW_CHAIN], language[2]; } id;
} Firmware;                                           /* id: every name above as the entity it is, computed once a pass (firmware_ids) */
/* ---- the lookups the forward pass is made of (pull.c) */
typedef struct { lp_id id; const lp_id *part; int np; lp_rating r; int matches; double conf; int position; } Claim;   /* a claim is a tuple: a pair, three parts, or a longer path */
typedef struct { lp_rating r; int matches; } ClaimStanding;
const char *firmware_path(void);                     /* LAPLACE_FIRMWARE, or firmware/program.firmware beside the recipes */
Firmware firmware_for(const char *path, int op);
const Firmware *firmware_ids(Firmware *);            /* the firmware's names as entities: computed the first time they are asked, once a pass */
typedef struct { lp_id sense, gloss, eq[4], usage; int np, lang, concept, has_gloss, neq, has_usage; } Lexicalization;
int lexicalization_of(lp_text *, const Firmware *, Lexicalization *);
void firmware_say(const Firmware *, int op);
lp_ref entity_named(lp_text *, const char *text, lp_ref *parts, size_t cap, size_t *np);      /* a text's trunk, computed here */
/* Returned claims and all operands share one allocation; release with free(). */
Claim *claims_like(PGconn *, const lp_id *part, const int *have, int fan, double k, int *n, int *capped);
Claim *claims_of(PGconn *, const lp_id *e, int fan, double k, int *n, int *capped);    /* every claim that holds an entity */
Claim *claims_each(PGconn *, const lp_id *keys, int nk, const lp_id *rel, int subject, int fan, double k, int *n, int **src);   /* the claims of each of a set, one set: src[i] is the place in keys of claim i */
void   positions_of(PGconn *, Claim *, int n);
int    claim_by_position(const void *, const void *);
int    claim_by_conf(const void *, const void *);
int    refused(PGconn *, lp_text *, const Firmware *, Claim *, int n);
void   weights_named(lp_text *, const Firmware *, lp_id *ids);                           /* the kinds a firmware weighs, as entities */
double strand_weight(const Firmware *, const lp_id *ids, const lp_id *part, int np);     /* how hard a claim of these parts is allowed to pull */
void   weighed(lp_text *, const Firmware *, Claim *, int n);
int lexicalization_counts(PGconn *, Reader *, const Lexicalization *, const lp_id *, int, int, double, long long *);
double role_of(PGconn *, Firmware *, const lp_id *word);                                  /* how hard a word pulls, by its role; -1 when nothing says */
void   take_top(Claim *, int n, int want, const Firmware *, unsigned *seed);              /* the top, or as near a tie as the firmware allows (seed NULL: the top) */
typedef int (*ChainFork)(void *ctx, const Claim *cl, int n);           /* at a fork of a chain: the branch to take, or -1 for the chain's own order */
int    chain_follow(PGconn *, Reader *, Firmware *, const lp_id *word, const lp_id *reading, unsigned *seed, ChainFork fork, void *fork_ctx, lp_id *answer, ClaimStanding *last, int *alt);                             /* each claim's confidence by its weight, and the set in that order */                  /* what the firmware refuses, taken out */

int cmd_hop(int argc, char **argv);
int cmd_translate(int argc, char **argv);
int cmd_degrees(int argc, char **argv);
int cmd_pull(int argc, char **argv);
int cmd_turn(int argc, char **argv);
int cmd_text(int argc, char **argv);
int cmd_firmware(int argc, char **argv);
void   refuse_named(const Firmware *fw);                                 /* the predicates this pass's firmware refuses */

#endif
