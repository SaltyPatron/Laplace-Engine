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

/* Thread-local zero-malloc scratch arena for record walking */
typedef struct { uint8_t *buf; size_t cap, committed, used, stack_used, stack_committed; } SayScratch;
void   *scratch_alloc(size_t bytes);
size_t  scratch_mark(void);
void    scratch_restore(size_t mark);
/* An explicitly owned arena survives record resets. File/worker output
 * buffers use it and release it after their references have been consumed. */
void   *arena_alloc(SayScratch *arena, size_t bytes);
void    arena_release(SayScratch *arena);
typedef struct { uint32_t count; uint32_t slots[]; } SNameBindings;
typedef const SNameBindings *(*s_compile_name_fn)(const void *program, const uint8_t *name, uint32_t length, SayScratch *arena);

/* Grammar ID and thread-local parser pool (grammars.c) */
uint16_t          grammar_id_for(const char *name);
uint16_t          grammar_id_for_language(const TSLanguage *language);
const TSLanguage *grammar_lang(uint16_t id);
TSParser         *lp_tls_parser(uint16_t grammar_id);
TSTreeCursor     *lp_tls_cursor(TSNode root);
void              lp_tls_cursor_release(TSTreeCursor *cursor);

#define S_TIERS 8
#define S_NAMES 256
typedef struct Layout Layout;
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
typedef struct { char path[64], sep[8], is[8]; int seplen, islen, pieces, padded; char space; char grammar[64];
                 uint16_t child_grammar_id, separator, assignment, name_length, next; uint8_t wildcard;
                 uint64_t name_hash; const Layout *inner, *syntax; } SPart;     /* pieces: at most so many, the last the rest as written; space: the character the file writes for a space in it; padded: each piece without the white space around it; grammar: chained inner grammar */
/* A file whose tree a grammar gives (the recipe names it: grammar NAME, loaded at run time): what each kind of the
 * grammar's nodes is in the file's tree, as the recipe (or the format file it names) writes it:
 *   node TYPE group [name PATH...] [list] [grammar NAME]
 *     context PATH...            ancestor header paths inherited by split records; their fields remain lexical
 *                               bindings, while the container's own header is reduced once in source order
 *   node TYPE value name PATH text PATH [grammar NAME]
 *   node TYPE text [raw] [join] [grammar NAME]
 *   node TYPE member name FIELD value FIELD [grammar NAME]
 *   node TYPE skip
 *   resolve xml | json | turtle
 *   trim
 * A kind the recipe says nothing of is passed through: what is inside it stands where it stands. */
typedef enum { G_GROUP = 1, G_VALUE, G_TEXT, G_MEMBER, G_SKIP, G_NOTE } GNodeKind;
enum { G_XML = 1, G_JSON, G_TURTLE, G_CSV, G_DELIMITED, G_LINES };

#define G_PATH_STEPS 8
typedef struct { TSSymbol symbol; TSFieldId field; uint8_t flags; } GPathStep;
typedef struct { GPathStep step[G_PATH_STEPS]; uint8_t n; } GPath;
typedef struct {
    uint16_t sym_id;
    TSFieldId field_id;
    uint16_t parent_sym;
    uint16_t child_grammar_id;
    GNodeKind kind;
    uint8_t tier_slot;
    uint16_t target_slot, type_length, alias_length;
    uint32_t flags;
    GPath name[4], text, context[4];
    const Layout *inner;
} CompiledRule;
typedef struct {
    char type[48]; int what, raw, join, list, kind, trim;
    char name[4][64]; int nname; char text[64];
    char context[4][64]; uint8_t ncontext;
    char child_grammar[64];
    char field[48], parent[48];
    char alias[64]; uint8_t alias_tier;
    CompiledRule compiled;
} GRule;     /* join: texts side by side are one text; list: a group of values, in order, each named by it; kind: named by its type */
typedef struct { const uint8_t *name; uint32_t length; uint8_t flags; const SNameBindings *bindings; } GColumn;
typedef struct { GColumn trigger, group, member[8]; uint8_t arity, radix; } GCountProgram;
typedef struct { char type[48], literal[8]; uint8_t length; TSSymbol symbol; } GSeparator;
typedef struct { char name[64]; uint64_t hash; uint16_t length; uint8_t next; } GRecordName;
typedef struct { char count[64], group[48], member[8][48]; uint8_t nmember, radix; } GCountShape;
typedef struct {
    GRule rule[48]; int n, resolve, trim; uint8_t quoted, escaped, padded;
    uint16_t grammar_id;
    uint32_t symbol_count, *sym_rule_offset;
    uint16_t *sym_rules;
    const char **field_names;
    uint32_t field_count, *field_lengths;
    int compiled, unresolved;
    char record_types[16][48], column_types[8][48], column_parent[48], named_columns[S_NAMES][48];
    uint8_t record_count, column_type_count, column_tier; uint16_t named_column_count;
    uint8_t *record_symbols, *column_symbols, *column_parents;
    TSSymbol column_parent_symbol;
    uint16_t *column_by_field;
    GColumn *columns; uint32_t column_count;
    GSeparator separators[32]; uint8_t separator_count;
    char quote_types[16][48]; uint8_t quote_count; uint8_t *quoted_symbols;
    GRecordName record_names[16]; uint8_t record_name_count, record_name_buckets[32];
    GCountShape count_shapes[8]; uint8_t count_shape_count;
    char remark_group[48], remark_text[48];
    uint16_t numbered_length, root_name_length;
    uint8_t has_context;
    char layout_prefix[4][8][64]; uint8_t prefix_length[4], prefix_count;
    GCountProgram counts[4]; uint8_t count_count, count_by_column[S_NAMES];
} GMap;                 /* trim: the white space a text begins and ends with is how the file is laid out, not the text's */
struct Layout { STier tier[S_TIERS]; int ntier; SPart part[32]; int npart; uint16_t part_buckets[64]; uint64_t part_prefix_lengths; uint8_t parts_compiled, part_errors;
                char empty[16][32]; int emptylen[16], nempty; char levels[8][32]; uint8_t level_lengths[8]; int nlevels; GMap g;
                s_compile_name_fn compile_name; const void *name_program; Layout *record_inner; };    /* empty: what the file writes where it leaves a part empty (several: empty _ None); levels: the names of the members of objects at each depth, whose keys are what the file says (levels roleset class) */
static inline int s_empty(const Layout *l, const uint8_t *v, uint32_t n){ if (!n) return 1; for (int i = 0; i < l->nempty; i++) if (n == (uint32_t)l->emptylen[i] && !__builtin_memcmp(v, l->empty[i], n)) return 1; return 0; }

/* Precompile grammar rules and chained sub-grammars into integer symbol/field IDs at recipe load time */
int layout_compile_grammar(Layout *l, const char *grammar_name);
int layout_compile_parts(Layout *l);
/* Compile declarative tier syntax into an owned outer grammar and the same
 * integer dispatch used by explicit .format files. No input bytes are read. */
int layout_compile_tiers(Layout *layout, char *grammar, size_t capacity);
int layout_compile_recipe(Layout *layout, char *grammar, size_t capacity);
void layout_bind_names(Layout *layout, s_compile_name_fn compile, const void *program);

/* A compiled grammar emits semantic fields as the native cursor enters and
 * leaves scopes. Only active cursor frames are retained; no projected AST is
 * built. Names and decoded values live in the caller's record scratch arena.
 * A consumer composes fields on S_CLOSE and keeps entity references, rather
 * than retaining the parser's nodes after the callback returns. */
enum { S_OPEN = 1, S_FIELD, S_CLOSE, S_FINISH };
enum { S_FIELD_JOIN = 1, S_FIELD_LIST = 2, S_FIELD_TRIM = 4, S_FIELD_ALIAS = 8, S_FIELD_DERIVED = 16,
       S_FIELD_SAME_TIER = 32, S_FIELD_PARTS = 64, S_FIELD_TIER = 128, S_FIELD_CONTEXT = 256, S_FIELD_RECORD = 512 };
enum { S_VISIT_OK = 1, S_VISIT_RELEASE = 2, S_VISIT_SKIP = 4 };
typedef struct {
    TSNode source;
    TSRange range;
    const uint8_t *name, *value;
    const SNameBindings *bindings;
    size_t arena_begin;
    uint32_t name_length, value_length;
    uint16_t grammar, flags;
    uint8_t kind, tier;
} SField;
/* Return S_VISIT_OK, optionally OR S_VISIT_RELEASE on S_CLOSE after reducing
 * an independent record. Release discards that record's decoded fields and
 * bindings; the consumer must retain only native entity references at that
 * point. Cursor frames live separately and survive the release. S_FINISH runs
 * once after the record's scopes close, before its scratch arena is reset;
 * remaining semantic bindings must be reduced in that callback. S_VISIT_SKIP
 * on S_OPEN omits its descendants but still emits the matching S_CLOSE. */
typedef int (*s_visit_fn)(void *sink, int event, const SField *field);
int s_cursor(const Layout *layout, TSNode root, const uint8_t *source, size_t length, s_visit_fn visit, void *sink);

/* A recipe's line, if it lays the file out: 1 when it was one, 0 when it is something else, -1 when it is written wrong
 * (what is wrong is said). tok is the line's first word; the rest of the line is read with strtok. */
int layout_says(Layout *l, const char *path, char *tok);

/* Single-pass streaming cursor over outermost tier records */
/* A parsed outer packaging window. Record iteration skips each returned
 * subtree, so nested symbols cannot split their enclosing record. The caller
 * finishes all work using a record before closing the window. */
typedef struct {
    const Layout *layout;
    const uint8_t *source;
    uint32_t length;
    TSTree *tree;
    TSTreeCursor *cursor;
    Layout *file_layout;
    size_t header_mark;
    uint64_t ordinal;
    uint8_t started, done, failed, header_pending, descend;
} SRecords;
typedef struct { TSNode node; TSRange range; uint64_t ordinal; uint8_t header; } SRecord;
int s_records_open(SRecords *records, const Layout *layout, const uint8_t *source, size_t length);
int s_record_next(SRecords *records, SRecord *record);
void s_records_close(SRecords *records);
int s_cursor_record(const Layout *layout, const SRecord *record, const uint8_t *source, size_t length, s_visit_fn visit, void *sink);

#endif
