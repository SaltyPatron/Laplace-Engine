/* Structure: the tree a file has, as its recipe lays it out. One decomposer for every file; what it knows of a file
 * is what the recipe's configuration says and nothing else. There is no code here for a format or for a source.
 *
 * A recipe lays a file out in tiers, outermost first, each parted from the next by what the recipe writes:
 *   tier NAME by SEP            the file (or the tier above) is NAMEs parted by SEP: \n, \n\n, \t, a character, a word
 *     names A B C ...           the parts of the tier below have these names, by position
 *     header                    the first NAME of the file names the positions of the tier below, and is no NAME itself
 *     skip N                    the first N are not NAMEs
 *     note PREFIX [IS]          a NAME that begins with PREFIX is a note: KEY IS VALUE when IS is written in it
 *     comment PREFIX            a NAME that begins with PREFIX is not read
 *     remark CHAR [KEPT]        in a NAME, what follows CHAR is not read as its parts; with KEPT, it is one text of
 *                               the NAME, named KEPT (what wndb.5 writes after a synset's | is its gloss)
 *     quoted                    a part of the tier below may stand between double quotes (a quote in it doubled)
 *     padded                    the parts of the tier below are written with spaces around them that are not theirs
 *     is SEP                    each part of this tier is written KEY SEP VALUE: the part is named KEY
 *     continued                 a part of this tier that begins with white space goes on with the one before it
 *     escaped CHAR              the character after CHAR is itself, a tier's separator included
 *     counted NAME [radix N] GROUP MEMBER...
 *                               the part NAME of the tier below is a count, written in radix N (2 to 36; 10 when not
 *                               said), of the GROUPs that follow it: each GROUP is the next parts, named MEMBER... in
 *                               order, and the names after NAME go on after the last GROUP (wndb.5: w_cnt, "two digit
 *                               hexadecimal integer indicating the number of words in the synset", then that many
 *                               word lex_id). A count the file does not write as a number in its radix counts nothing
 *   part PATH by SEP [is IS] [pieces N] [space CHAR] [padded]
 *                               a named part is itself parts, parted by SEP; with IS, each is KEY IS VALUE; with pieces,
 *                               at most N, the last of them the rest as written; with space, CHAR in it stands for a space;
 *                               with padded, each piece is without the white space around it (frames.vrb aligns its texts).
 *                               SEP object: the part is a JSON value, read into the tree (each member under its key)
 *   empty TEXT                  what the file writes where it leaves a part empty
 * The tree is observed structure: what each node becomes is said by the recipe's dispositions, not here. */
#ifndef LAPLACE_STRUCTURE_H
#define LAPLACE_STRUCTURE_H

#include <stddef.h>
#include <stdint.h>
#include <tree_sitter/api.h>

enum { S_GROUP = 1, S_VALUE, S_TEXT, S_NOTE };        /* holds nodes; a name and its text; a text; a named text beside a tier's parts */
#define S_PIECES 2                                    /* in a group's join: it is a value parted into pieces (part NAME by SEP), not an element */

typedef struct RecNode {
    const uint8_t *name, *val;        /* as the file writes them; they point into the file or into bytes the tree owns */
    uint32_t nlen, vlen;
    int32_t parent, first, last, next;  /* -1: none */
    uint32_t nkids;
    uint64_t at, end;                 /* where it begins in the file; where a group ends */
    uint8_t kind, tier, join;         /* tier: which of the layout's tiers it is a part of; join: a text the next joins */
} RecNode;
typedef struct RecTree { RecNode *n; uint32_t count, cap; uint8_t **pool; size_t npool, used, room; } RecTree;

/* Thread-local zero-malloc scratch arena for record walking */
typedef struct { uint8_t *buf; size_t cap, used; } SayScratch;
void   *scratch_alloc(size_t bytes);
size_t  scratch_mark(void);
void    scratch_restore(size_t mark);

/* Grammar ID and thread-local parser pool (grammars.c) */
uint16_t          grammar_id_for(const char *name);
const TSLanguage *grammar_lang(uint16_t id);
TSParser         *lp_tls_parser(uint16_t grammar_id);

#define S_TIERS 8
#define S_NAMES 256
typedef struct {
    char name[32], sep[16]; int seplen;
    char note[8], note_is[8], comment[8]; int notelen, islen, commentlen;
    char remark, escaped; int header, quoted, skip, padded;
    char remark_kept[32];                                 /* remark CHAR KEPT: what follows CHAR is one text, named KEPT */
    struct { char field[64], group[32], member[8][32]; int nmember, radix; } count[4]; int ncount;     /* counted NAME [radix N] [hex] [of] GROUP MEMBER... */
    char kv[8]; int kvlen, continued;                     /* kv: each part is written KEY kv VALUE (the recipe's "is"); continued: a part that begins with white space goes on with the one before */
    char numbered[32];                                    /* numbered NAME: each part also holds NAME, its line in the file (empty lines counted), so files written line for line join by it */
    char names[S_NAMES][64]; int nnames, rest;            /* the names of the parts of the tier below, by position; rest: the last (written NAME...) names every further part too */
} STier;
typedef struct { char path[64], sep[8], is[8]; int seplen, islen, pieces, padded; char space; char grammar[64]; uint16_t child_grammar_id; } SPart;     /* pieces: at most so many, the last the rest as written; space: the character the file writes for a space in it; padded: each piece without the white space around it; grammar: chained inner grammar */
/* A file whose tree a grammar gives (the recipe names it: grammar NAME, loaded at run time): what each kind of the
 * grammar's nodes is in the file's tree, as the recipe (or the format file it names) writes it:
 *   node TYPE group [name PATH...] [list] [grammar NAME]
 *   node TYPE value name PATH text PATH [grammar NAME]
 *   node TYPE text [raw] [join] [grammar NAME]
 *   node TYPE member name FIELD value FIELD [grammar NAME]
 *   node TYPE skip
 *   resolve xml | json | turtle
 *   trim
 * A kind the recipe says nothing of is passed through: what is inside it stands where it stands. */
typedef enum { G_GROUP = 1, G_VALUE, G_TEXT, G_MEMBER, G_SKIP } GNodeKind;
enum { G_XML = 1, G_JSON, G_TURTLE };
typedef struct {
    uint16_t sym_id;
    TSFieldId name_field_id;
    TSFieldId text_field_id;
    uint16_t child_grammar_id;
    GNodeKind kind;
    const char *tag;
    char name_attr[64];
    char text_attr[64];
} CompiledRule;
typedef struct {
    char type[48]; int what, raw, join, list, kind;
    char name[4][64]; int nname; char text[64];
    char child_grammar[64];
    CompiledRule compiled;
} GRule;     /* join: texts side by side are one text; list: a group of values, in order, each named by it; kind: named by its type */
typedef struct {
    GRule rule[48]; int n, resolve, trim;
    uint16_t grammar_id;
    uint16_t sym_rule_offset[4096];                       /* 0 = none, 1..48 = rule index + 1 */
} GMap;                 /* trim: the white space a text begins and ends with is how the file is laid out, not the text's */
typedef struct { STier tier[S_TIERS]; int ntier; SPart part[32]; int npart; char empty[16][32]; int emptylen[16], nempty; char levels[8][32]; int nlevels; GMap g; } Layout;    /* empty: what the file writes where it leaves a part empty (several: empty _ None); levels: the names of the members of objects at each depth, whose keys are what the file says (levels roleset class) */
static inline int s_empty(const Layout *l, const uint8_t *v, uint32_t n){ if (!n) return 1; for (int i = 0; i < l->nempty; i++) if (n == (uint32_t)l->emptylen[i] && !__builtin_memcmp(v, l->empty[i], n)) return 1; return 0; }

/* Precompile grammar rules and chained sub-grammars into integer symbol/field IDs at recipe load time */
void layout_compile_grammar(Layout *l, const char *grammar_name);

/* The tree of a file a grammar parsed: root is the grammar's own tree (a TSNode), given by address. The whole file
 * is one part: handed over once. */
typedef void (*s_unit_fn)(void *sink, const RecTree *t, int32_t root, uint64_t ordinal);
void s_grammar(const Layout *l, const void *ts_root, const uint8_t *src, size_t n, s_unit_fn fn, void *sink);

/* A recipe's line, if it lays the file out: 1 when it was one, 0 when it is something else, -1 when it is written wrong
 * (what is wrong is said). tok is the line's first word; the rest of the line is read with strtok. */
int layout_says(Layout *l, const char *path, char *tok);

/* Single-pass streaming cursor over outermost tier records */
int s_record_next(const Layout *l, const uint8_t *src, size_t n, size_t *cursor, size_t *rec_lo, size_t *rec_hi, uint64_t *line_no);

/* The file's outermost tier, one part at a time: each handed over with its place among them, its tree its alone and
 * given back before the next. names: read from the file's head where the layout says header. Returns the parts read. */
uint64_t s_decompose(const Layout *l, const uint8_t *src, size_t n, s_unit_fn fn, void *sink);
/* Where the outermost tier's parts begin (past what the layout skips and past a header, whose names are read into it). */
size_t s_head(Layout *l, const uint8_t *src, size_t n);
/* Where the next outermost part begins at or after at: a file is parted there to be read on every core. */
size_t s_boundary(const Layout *l, const uint8_t *src, size_t n, size_t at);

int32_t s_child(const RecTree *t, int32_t node, const char *name, int32_t after);
static inline int s_named(const RecNode *x, const char *name, size_t l){ return x->nlen == l && !__builtin_memcmp(x->name, name, l); }
void s_print(const RecTree *t, int32_t node, int depth, long *left);

#endif
