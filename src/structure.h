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
 *     remark CHAR               in a NAME, what follows CHAR is not read
 *     quoted                    a part of the tier below may stand between double quotes (a quote in it doubled)
 *     padded                    the parts of the tier below are written with spaces around them that are not theirs
 *     is SEP                    each part of this tier is written KEY SEP VALUE: the part is named KEY
 *     continued                 a part of this tier that begins with white space goes on with the one before it
 *     escaped CHAR              the character after CHAR is itself, a tier's separator included
 *   part PATH by SEP [is IS] [pieces N] [space CHAR]
 *                               a named part is itself parts, parted by SEP; with IS, each is KEY IS VALUE; with pieces,
 *                               at most N, the last of them the rest as written; with space, CHAR in it stands for a space.
 *                               SEP object: the part is a JSON value, read into the tree (each member under its key)
 *   empty TEXT                  what the file writes where it leaves a part empty
 * The tree is observed structure: what each node becomes is said by the recipe's dispositions, not here. */
#ifndef LAPLACE_STRUCTURE_H
#define LAPLACE_STRUCTURE_H

#include <stddef.h>
#include <stdint.h>

enum { S_GROUP = 1, S_VALUE, S_TEXT, S_NOTE };        /* holds nodes; a name and its text; a text; a named text beside a tier's parts */
#define S_PIECES 2                                    /* in a group's join: it is a value parted into pieces (part NAME by SEP), not an element */

typedef struct {
    const uint8_t *name, *val;        /* as the file writes them; they point into the file or into bytes the tree owns */
    uint32_t nlen, vlen;
    int32_t parent, first, last, next;  /* -1: none */
    uint32_t nkids;
    uint64_t at, end;                 /* where it begins in the file; where a group ends */
    uint8_t kind, tier, join;         /* tier: which of the layout's tiers it is a part of; join: a text the next joins */
} SNode;
typedef struct { SNode *n; uint32_t count, cap; uint8_t **pool; size_t npool, used, room; } STree;

#define S_TIERS 8
#define S_NAMES 256
typedef struct {
    char name[32], sep[16]; int seplen;
    char note[8], note_is[8], comment[8]; int notelen, islen, commentlen;
    char remark, escaped; int header, quoted, skip, padded;
    char kv[8]; int kvlen, continued;                     /* kv: each part is written KEY kv VALUE (the recipe's "is"); continued: a part that begins with white space goes on with the one before */
    char names[S_NAMES][64]; int nnames;                  /* the names of the parts of the tier below, by position */
} STier;
typedef struct { char path[64], sep[8], is[8]; int seplen, islen, pieces; char space; } SPart;     /* pieces: at most so many, the last the rest as written; space: the character the file writes for a space in it */
/* A file whose tree a grammar gives (the recipe names it: grammar NAME, loaded at run time): what each kind of the
 * grammar's nodes is in the file's tree, as the recipe (or the format file it names) writes it:
 *   node TYPE group [name PATH...] [list] it holds nodes; its name is the text at the first PATH there is (A/B: the B in its A);
 *                                         a list's values, and the groups in it, are named by it (a sense in "senses")
 *   node TYPE value name PATH text PATH   a name and its text
 *   node TYPE text [raw] [join]           a text, under the name of what holds it; with join, texts side by side are one text
 *   node TYPE member name FIELD value FIELD
 *                                         it names what its other part is: that part, read as its own kind, under the name
 *   node TYPE skip                        it and what is inside it are not of the tree
 *   resolve xml | json                    how the grammar's texts write what they cannot write plainly
 *   trim                                  the white space a text begins and ends with is the file's layout, not the text's
 * A kind the recipe says nothing of is passed through: what is inside it stands where it stands. */
enum { G_GROUP = 1, G_VALUE, G_TEXT, G_MEMBER, G_SKIP };
enum { G_XML = 1, G_JSON, G_TURTLE };
typedef struct { char type[48]; int what, raw, join, list, kind; char name[4][64]; int nname; char text[64]; } GRule;     /* join: texts side by side are one text; list: a group of values, in order, each named by it; kind: named by its type */
typedef struct { GRule rule[48]; int n, resolve, trim; } GMap;                 /* trim: the white space a text begins and ends with is how the file is laid out, not the text's */
typedef struct { STier tier[S_TIERS]; int ntier; SPart part[32]; int npart; char empty[16][16]; int emptylen[16], nempty; char levels[8][32]; int nlevels; GMap g; } Layout;    /* empty: what the file writes where it leaves a part empty (several: empty _ None); levels: the names of the members of objects at each depth, whose keys are what the file says (levels roleset class) */
static inline int s_empty(const Layout *l, const uint8_t *v, uint32_t n){ if (!n) return 1; for (int i = 0; i < l->nempty; i++) if (n == (uint32_t)l->emptylen[i] && !__builtin_memcmp(v, l->empty[i], n)) return 1; return 0; }
/* The tree of a file a grammar parsed: root is the grammar's own tree (a TSNode), given by address. The whole file
 * is one part: handed over once. */
void s_grammar(const Layout *l, const void *ts_root, const uint8_t *src, size_t n, void (*fn)(void *sink, const STree *t, int32_t root, uint64_t ordinal), void *sink);
/* A long file of records (split ELEMENT...), parsed a part at a time on every core and read as the one tree the whole
 * file is: each record grafted where it stands in the file, so its keys resolve across the file and it is read once.
 * spans: where each record begins and ends, in the file's order; lang: the grammar (a TSLanguage). Returns how many parts
 * did not parse whole. */
uint64_t s_grammar_split(const Layout *l, const void *lang, const uint8_t *src, size_t n, const size_t *ra, const size_t *rb, size_t nrs,
                         void (*fn)(void *sink, const STree *t, int32_t root, uint64_t ordinal), void *sink);

/* A recipe's line, if it lays the file out: 1 when it was one, 0 when it is something else, -1 when it is written wrong
 * (what is wrong is said). tok is the line's first word; the rest of the line is read with strtok. */
int layout_says(Layout *l, const char *path, char *tok);

/* The file's outermost tier, one part at a time: each handed over with its place among them, its tree its alone and
 * given back before the next. names: read from the file's head where the layout says header. Returns the parts read. */
typedef void (*s_unit_fn)(void *sink, const STree *t, int32_t root, uint64_t ordinal);
uint64_t s_decompose(const Layout *l, const uint8_t *src, size_t n, s_unit_fn fn, void *sink);
/* Where the outermost tier's parts begin (past what the layout skips and past a header, whose names are read into it). */
size_t s_head(Layout *l, const uint8_t *src, size_t n);
/* Where the next outermost part begins at or after at: a file is parted there to be read on every core. */
size_t s_boundary(const Layout *l, const uint8_t *src, size_t n, size_t at);

int32_t s_child(const STree *t, int32_t node, const char *name, int32_t after);
static inline int s_named(const SNode *x, const char *name, size_t l){ return x->nlen == l && !__builtin_memcmp(x->name, name, l); }
void s_print(const STree *t, int32_t node, int depth, long *left);

#endif
