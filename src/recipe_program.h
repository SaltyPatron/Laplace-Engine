#ifndef LAPLACE_RECIPE_PROGRAM_H
#define LAPLACE_RECIPE_PROGRAM_H

/* Immutable semantic bytecode shared by the recipe compiler and record VM.
 * Construction belongs to recipe_compile.c; workers borrow this program and
 * keep all record-specific state outside it. */
#include "engine.h"
#include "structure.h"
#include <regex.h>
#include <stdint.h>
#include <string.h>

enum { D_CONTENT = 1, D_KEY, D_REFER, D_TYPE, D_METADATA, D_OMIT, D_CODEPOINTS, D_RANGE, D_OWN };
/* A key as a resource writes it, matched by a pattern and written as a template (\1: the pattern's first part) where it
 * writes it otherwise than those it points at write it (vn:51.2 for 51.2): the highway's lines, and a type's key. */
typedef struct KeyProgram KeyProgram;
typedef struct DiagnosticLabel DiagnosticLabel;
typedef struct { char list[32], path[160], as[4][64]; int nas; regex_t re; int has_re, node; KeyProgram *program; } KSpec;     /* as: the templates, tried in order */
typedef struct { char name[64]; uint16_t length, tier_id; uint8_t within, local; lp_id domain; } TargetCode;
typedef struct { char name[64]; int what; char arg[64]; KSpec k; uint32_t when;
                 struct { uint8_t off, len, wild, note; } whole, part; uint8_t el_l; const char *text;
                 TargetCode *targets; uint8_t ntargets; uint16_t name_length, arg_length, text_length;
                 DiagnosticLabel *road_label, *statement_label; uint32_t name_slot, part_slot, parent_slot; } Dis;   /* k: of a type, how its key is written; whole, part, el_l: its name as matched, split once (dis_parse); text: what a relation line words it as */
typedef struct { char tier[32], name[64]; uint16_t tier_length, tier_id; lp_id domain, parent_domain; } Pair;
typedef struct { char tier[32]; char name[64][64]; int n; uint32_t when; char by[64], of[64]; int file; } Att;      /* when: the conditions it is said under, by their places in where[]; by: the part that names who says it; of: the part it is said of; file: a witness stands within the file */
typedef struct { char tier[32], rel[64], to[64]; uint32_t when; char by[64], of[64]; int alone; } Rel;     /* alone: where what it is to names nothing, the relation alone */
typedef struct { char tier[32], name[8][160]; int n, within, infile, span, inclusive, of, resolved; char lit[8], space[8]; KSpec form[8]; } Thing;    /* of: the path of the things of its parts of these names, in order (a sentence of its words) */   /* within: its name stands only within the thing it is inside: the pair of that thing and it; infile: only within the file; span: the stretch of a text name[2] between the characters name[0] and name[1] */
typedef struct { char tier[32], name[64]; double from, to; int mapped; } Score;
typedef struct { char name[64]; int op; char val[128]; regex_t re; } Where;
typedef struct { char tier[32]; uint32_t when; const lp_trust_class *class; uint16_t length; } Dependence;
typedef struct { int what; char tier[32], say[64]; KSpec a, b; uint32_t when; } HwLine;       /* what: 1 types, 2 keyed, 3 alias, 4 maps */
typedef struct PathProgram PathProgram;
typedef struct DisProgram DisProgram;
typedef struct KeyProjection KeyProjection;
typedef struct { Layout lay; Dis dis[256]; int ndis; HwLine hw[32]; int nhw; Thing thing[32]; int nthing; Pair key[32]; int nkey; Att att[32]; int natt; Att pair[8]; int npair; Att hold[16]; int nhold; Rel rel[16]; int nrel;
                 Att itself[8]; int nitself;                                 /* a TIER's thing is itself what is attested: a tuple is the claim it is */
                 Dependence dependence[8]; int ndependence;
                 Att voices[8]; int nvoices;                                 /* parts each of which is a witness of its own, saying its field of the thing */
                 Att together[8]; int ntogether;                             /* a TIER whose claims are said together: one record, witnessed once, its claims within it */
                 char stem, stemfrom;                                        /* what ends the part of the file's name {file} stands for (. unless said), and what it begins after */
                 char split[16][64]; int nsplit;                             /* a long file a grammar reads is parted before each line that begins one of these elements */
                 char (*perf)[64]; int nperf;                               /* perfcache: the flags' property names, short and as said, folded (UAX44-LM3) */
                 char selfmark;                                              /* what the source writes, in a value, for the code point the part is (UCD: #) */
                 regex_t about_re; int has_about;                            /* a page's lines: the first that matches names what the page is about */
                 struct { regex_t re; int mode; char pred[64]; uint16_t pred_length; } line[8]; int nline;    /* a line that matches says its parts: 0 of what the page is about, 1 the claim itself, 2 a pair, 3 under a name */
                 char *said; size_t nsaid; int laid_line;                                  /* every word the recipe's lines write, each between spaces: a part named nowhere in them is left open (unmentioned); laid_line: the line just said lays the file out, which names its parts and reads none */
                 Dis val[256]; int nval;                                     /* value LIST NAME...: what a part means in a claim; arg: its road class's list, "" for none the highway holds */
                 Score score[4]; int nscore; Where where[16]; int nwhere; uint32_t whole, now; Pair voice;   /* whole: the conditions on the outermost part (where); now: the ones the lines being read are said under (when) */
                 /* a record's layers down its rows and its tree by head, where the recipe gives them (layers, tree): parts of the
                  * record's path beside the claims it says (record_of) */
                 char lay_tier[32], lay_row[32], layer[16][64]; int nlayer; char tree_tier[32], tree_row[32], tree_head[64], tree_rel[64];
                 PathProgram *paths; DisProgram *dis_program, *value_program; uint16_t tree_key_index; uint32_t sc_slot; lp_id key_domain; KeyProjection *projections; } Say;

typedef struct { uint64_t key; uint16_t next, rule; uint8_t qualified; } DisEntry;
struct DisProgram { uint16_t *buckets; DisEntry *entries; size_t capacity, count; uint64_t wildcard_lengths; };
#define NAME_HASH_START UINT64_C(14695981039346656037)
#define NAME_HASH_PRIME UINT64_C(1099511628211)

struct DiagnosticLabel { const char *text; uint16_t length; uint64_t hash; };

typedef struct { const char *bytes; uint32_t length, slot; uint8_t wildcard, note; } NameCode;
enum { PATH_CHILD = 1, PATH_PARENT, PATH_ANCESTOR, PATH_SELF, PATH_POSITION };
typedef struct PathStep PathStep;
struct PathStep {
    NameCode name;
    PathStep *next, *alternative, *filter;
    const char *filter_value;
    uint32_t filter_length, position;
    uint8_t operation;
};
enum { SELECT_FIELD, SELECT_SELF, SELECT_FILE, SELECT_ELEMENTS, SELECT_UNNAMED };
struct KeyProjection { const PathStep *field; lp_id domain; uint16_t tier; KeyProjection *next, *all_next; };
typedef struct { const PathStep *reference; KeyProjection *targets; } ReferenceProjection;
typedef struct { NameCode whole; PathStep *first; uint16_t source_length; uint8_t special; ReferenceProjection *projection; } PathCode;
typedef struct { uint32_t offset; PathCode *code; } PathEntry;
typedef struct { NameCode name; uint64_t hash; uint32_t next; } NamePattern;
typedef struct { const uint8_t *bytes; uint64_t hash; uint8_t length; } Mention;
struct PathProgram {
    PathEntry *entries; size_t capacity, count;
    NamePattern *names; uint32_t *name_buckets; size_t name_count, name_capacity, name_bucket_count;
    uint64_t wildcard_lengths[3]; uint32_t max_name_length; uint8_t has_positions;
    Mention *mentions; size_t mention_capacity, mention_count;
};

enum { KEY_LITERAL = 1, KEY_CAPTURE, KEY_FIELD };
typedef struct { NameCode text; uint8_t operation, capture; } KeyInstruction;
typedef struct { KeyInstruction *instructions; uint16_t count; } KeyTemplate;
struct KeyProgram { KeyTemplate forms[4]; };

static inline uint64_t selector_hash(const uint8_t *bytes, size_t length){
    uint64_t hash = NAME_HASH_START;
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * NAME_HASH_PRIME;
    return hash;
}

static inline uint64_t disposition_key(uint64_t name, uint64_t parent, unsigned note, unsigned wildcard, unsigned qualified){
    uint64_t key = name;
    if (qualified) key ^= (parent << 1 | parent >> 63) ^ UINT64_C(0x6eed0e9da4d94a4f);
    if (note) key ^= UINT64_C(0x9e3779b97f4a7c15);
    if (wildcard) key ^= UINT64_C(0x94d049bb133111eb);
    return key;
}

int recipe_program_mentions(const PathProgram *, const uint8_t *, uint32_t);
int recipe_property_cached(const Say *, const char *, size_t);
const PathCode *recipe_program_path(const Say *, const char *);

#endif
