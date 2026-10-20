#define _GNU_SOURCE
/* What a recipe says each part of a file's tree is. The decomposer (structure.c) gives the tree the recipe's layout
 * parts the file into; this gives every named part its disposition, as the recipe's configuration writes it, and
 * composes what follows from that: the file's trunk over its metadata tree and its content tree, and what the file
 * attests. There is no code here for a format or for a source: only for what a recipe can say.
 *
 *   thing TIER NAME...         a TIER is the thing its part NAME names (a row is its word; a record is its text); named
 *                              by several parts together, it is the path of them. resolved: the path of those of them
 *                              that resolve, in the recipe's order (a mapping row over the nodes it aligns): a part the
 *                              source leaves empty is in no path; a key no list of the highway holds is left out and is
 *                              an unresolved obligation, said, never dropped silently; one none of whose parts resolves
 *                              names nothing
 *                              A part may be written
 *                              NAME matching PATTERN (the pattern's first group of NAME's text, as the source's
 *                              notation parts it), NAME space CHAR (CHAR in it stands for a space), or literal TEXT (a
 *                              text the recipe writes, quoted from the source's own documentation in a comment)
 *   pair TIER NAME...          of a TIER's thing, a part the file writes beside it with nothing between: [thing, value]
 *   score TIER NAME [from A to B]
 *                              the score the TIER gives what it attests, on the scale the source writes it on
 *   where NAME is VALUE | is-not VALUE | matches PATTERN
 *                              the outermost parts the recipe speaks of; another is in the file's tree and attests nothing
 *   when NAME is VALUE | is-not VALUE | matches PATTERN
 *                              the attest, pair and relate lines after it are said only of the parts that meet it
 *                              (several are met together), up to a line "always"
 *   content NAME...            the part is content, the text it is, the same entity wherever that text stands
 *   key TIER NAME              the part is the file's key for a TIER: it resolves to the TIER's thing; written nowhere
 *   refer NAME TIER            the part's text is a key of a TIER: it is read as that TIER's thing
 *   type NAME LIST             the part's text is the source's key of a type in the highway's LIST: read as that type
 *                              ([matching PATTERN] [as TEMPLATE...]: the key as the lists write it; {NAME} in a template
 *                              is the part NAME beside it, and each template is tried in turn: key_form)
 *     [node]                   the key, as the lists write it, is the node's own text (a PropBank roleset abandon.01, a
 *                              FrameNet frame), so it is content whether or not a list holds it (BRIEF: a highway node is
 *                              content): where none does, it is that node, recorded as its text and counted; never for
 *                              a source's own pointer to a node (vn:51.3.1, mcr:ili-30-01976841-v), which only resolves
 *   metadata NAME...           the part is said of the file itself: it goes in the file's metadata tree
 *   omit NAME...               the part is the file's bookkeeping: read by nothing
 *   perfcache                  a part named as a property the perf-cache's flags hold (tier0.flags, its layout) is read
 *                              from there for every codepoint: it is not attested again
 *   value LIST NAME...         what each part NAME means when a claim is made of it: its value is a value of the road
 *                              class LIST (the highway's list of that name; - where the highway lists none yet: the
 *                              source's own vocabulary for the part), a tagset's tag, a lemma, a feature's Name=Value,
 *                              a relation of the source's own inventory. The value is recorded as the source writes it;
 *                              where the highway holds LIST, a value it does not list is counted and said. ELEMENT.NAME
 *                              names the part only within that element (frameRelationType.name, MISC.Translit)
 *   attest TIER NAME...        of a TIER's thing, what each part NAME says: [thing, value], NAME a part that value names
 *                              the meaning of; a part that is itself KEY IS VALUE parts says each [KEY, VALUE]: [thing,
 *                              [KEY, VALUE]] (a feature's Name=Value), or, where KEY itself is valued, [thing, VALUE]
 *   holds TIER NAME... [via]   of a TIER's thing, the things of those names inside it that value names: [thing, other]
 *   relate TIER NAME to NAME   of a TIER's thing, its relation (the first part's value, which value names: a relation
 *                              of the source's own inventory) to what the second part is: [thing, relation, other];
 *                              where the second names nothing, the pair [thing, relation]
 *   relation "TEXT" NAME...    each part NAME states a relation of the thing to its value, which the source's own
 *                              documentation words as TEXT (quoted there, cited in the recipe; never the field's own
 *                              name, which is refused): [thing, TEXT, value]. TEXT is content the source writes, in its
 *                              language ("is a" is a sentence, Semantics: Claims); that two sources' wordings mean one
 *                              relation is an attested equivalence, as n and NOUN are
 * A relation is what the source means, never the name its markup gives a field (Laplace-Engine#32): no part's name, no
 * element's, no layer's, no file's, is ever a part of a claim. A part a claim names that value does not give a meaning,
 * a relation taken from the file's name ({file}) and a line's predicate are an unresolved obligation: no claim is made,
 * and each is counted by its name and said when the ingest ends (say_obligations).
 * A named part the recipe gives no disposition is an obligation left open: it is counted and said, never taken for
 * content and never dropped in silence. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>
#include "engine.h"
#include "structure.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <tree_sitter/api.h>

enum { D_CONTENT = 1, D_KEY, D_REFER, D_TYPE, D_METADATA, D_OMIT, D_CODEPOINTS, D_RANGE, D_OWN };
/* A key as a resource writes it, matched by a pattern and written as a template (\1: the pattern's first part) where it
 * writes it otherwise than those it points at write it (vn:51.2 for 51.2): the highway's lines, and a type's key. */
typedef struct KeyProgram KeyProgram;
typedef struct DiagnosticLabel DiagnosticLabel;
typedef struct { char list[32], path[160], as[4][64]; int nas; regex_t re; int has_re, node; KeyProgram *program; } KSpec;     /* as: the templates, tried in order */
typedef struct { char name[64]; uint16_t length, tier_id; uint8_t within, local; } TargetCode;
typedef struct { char name[64]; int what; char arg[64]; KSpec k; uint32_t when;
                 struct { uint8_t off, len, wild, note; } whole, part; uint8_t el_l; const char *text;
                 TargetCode *targets; uint8_t ntargets; uint16_t name_length, arg_length, text_length;
                 DiagnosticLabel *road_label, *statement_label; uint32_t name_slot, part_slot, parent_slot; } Dis;   /* k: of a type, how its key is written; whole, part, el_l: its name as matched, split once (dis_parse); text: what a relation line words it as */
typedef struct { char tier[32], name[64]; uint16_t tier_length, tier_id; } Pair;
typedef struct { char tier[32]; char name[64][64]; int n; uint32_t when; char by[64], of[64]; int file; } Att;      /* when: the conditions it is said under, by their places in where[]; by: the part that names who says it; of: the part it is said of; file: a witness stands within the file */
typedef struct { char tier[32], rel[64], to[64]; uint32_t when; char by[64]; int alone; } Rel;     /* alone: where what it is to names nothing, the relation alone */
typedef struct { char tier[32], name[8][160]; int n, within, infile, span, inclusive, of, resolved; char lit[8], space[8]; KSpec form[8]; } Thing;    /* of: the path of the things of its parts of these names, in order (a sentence of its words) */   /* within: its name stands only within the thing it is inside: the pair of that thing and it; infile: only within the file; span: the stretch of a text name[2] between the characters name[0] and name[1] */
typedef struct { char tier[32], name[64]; double from, to; int mapped; } Score;
typedef struct { char name[64]; int op; char val[128]; regex_t re; } Where;
typedef struct { int what; char tier[32], say[64]; KSpec a, b; uint32_t when; } HwLine;       /* what: 1 types, 2 keyed, 3 alias, 4 maps */
typedef struct PathProgram PathProgram;
typedef struct DisProgram DisProgram;
typedef struct { Layout lay; Dis dis[256]; int ndis; HwLine hw[32]; int nhw; Thing thing[32]; int nthing; Pair key[32]; int nkey; Att att[32]; int natt; Att pair[8]; int npair; Att hold[16]; int nhold; Rel rel[16]; int nrel;
                 Att itself[8]; int nitself;                                 /* a TIER's thing is itself what is attested: a tuple is the claim it is */
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
                 PathProgram *paths; DisProgram *dis_program, *value_program; uint16_t tree_key_index; uint32_t sc_slot; } Say;

typedef struct { uint64_t key; uint16_t next, rule; uint8_t qualified; } DisEntry;
struct DisProgram { uint16_t *buckets; DisEntry *entries; size_t capacity, count; uint64_t wildcard_lengths; };
#define NAME_HASH_START UINT64_C(14695981039346656037)
#define NAME_HASH_PRIME UINT64_C(1099511628211)
static uint64_t selector_hash(const uint8_t *bytes, size_t length){
    uint64_t hash = NAME_HASH_START;
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * NAME_HASH_PRIME;
    return hash;
}
struct DiagnosticLabel { const char *text; uint16_t length; uint64_t hash; };
static DiagnosticLabel *compile_label(const char *text, size_t length){
    DiagnosticLabel *label = malloc(sizeof *label + length + 1); if (!label) { perror("diagnostic label"); abort(); }
    char *copy = (char *)(label + 1); memcpy(copy, text, length); copy[length] = 0;
    *label = (DiagnosticLabel){copy, (uint16_t)(length < 64 ? length : 64), selector_hash((const uint8_t *)copy, length < 64 ? length : 64)};
    return label;
}
static void compile_diagnostics(Dis *rules, int count){
    for (int i = 0; i < count; i++) {
        Dis *rule = &rules[i]; rule->name_length = (uint16_t)strlen(rule->name); rule->arg_length = (uint16_t)strlen(rule->arg);
        rule->text_length = rule->text ? (uint16_t)strlen(rule->text) : 0; rule->road_label = compile_label(rule->arg, rule->arg_length);
        char text[96]; size_t length;
        if (rule->what == 1) {
            size_t n = rule->text_length < 60 ? rule->text_length : 60;
            memcpy(text, "relation \"", 10); if (n) memcpy(text + 10, rule->text, n); text[10 + n] = '"'; length = 11 + n;
        } else {
            const char *argument = rule->arg_length ? rule->arg : "-"; size_t n = rule->arg_length ? rule->arg_length : 1;
            memcpy(text, "value ", 6); memcpy(text + 6, argument, n); length = 6 + n;
        }
        rule->statement_label = compile_label(text, length);
    }
}
static uint64_t disposition_key(uint64_t name, uint64_t parent, unsigned note, unsigned wildcard, unsigned qualified){
    uint64_t key = name;
    if (qualified) key ^= (parent << 1 | parent >> 63) ^ UINT64_C(0x6eed0e9da4d94a4f);
    if (note) key ^= UINT64_C(0x9e3779b97f4a7c15);
    if (wildcard) key ^= UINT64_C(0x94d049bb133111eb);
    return key;
}
static DisProgram *compile_dispositions(const Dis *rules, int count){
    DisProgram *program = calloc(1, sizeof *program); if (!program) { perror("disposition program"); abort(); }
    program->capacity = 8;
    while (program->capacity < (size_t)count * 4) program->capacity *= 2;
    program->buckets = calloc(program->capacity, sizeof *program->buckets);
    program->entries = calloc(count ? (size_t)count * 2 : 1, sizeof *program->entries);
    if (!program->buckets || !program->entries) { perror("disposition index"); abort(); }
    for (int i = 0; i < count; i++) {
        const Dis *rule = &rules[i];
        for (unsigned qualified = 0; qualified <= (unsigned)(rule->el_l != 0); qualified++) {
            unsigned offset = qualified ? rule->part.off : rule->whole.off, length = qualified ? rule->part.len : rule->whole.len;
            unsigned wildcard = qualified ? rule->part.wild : rule->whole.wild, note = qualified ? rule->part.note : rule->whole.note;
            uint64_t key = disposition_key(selector_hash((const uint8_t *)rule->name + offset, length),
                                           qualified ? selector_hash((const uint8_t *)rule->name, rule->el_l) : 0,
                                           note, wildcard, qualified);
            size_t bucket = (key ^ (key >> 32)) & (program->capacity - 1);
            program->entries[program->count] = (DisEntry){key, program->buckets[bucket], (uint16_t)i, (uint8_t)qualified};
            program->buckets[bucket] = (uint16_t)++program->count;
            if (wildcard) program->wildcard_lengths |= UINT64_C(1) << length;
        }
    }
    return program;
}

/* Semantic selectors are compiled with the recipe, separately from packaging
 * symbols. XML tag names and JSON object keys are values in those grammars;
 * their selectors therefore retain byte predicates, while path navigation,
 * positions, ancestor tests and filters become immutable instructions. */
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
typedef struct { NameCode whole; PathStep *first; uint16_t source_length; uint8_t special; } PathCode;
typedef struct { uint32_t offset; PathCode *code; } PathEntry;
typedef struct { NameCode name; uint64_t hash; uint32_t next; } NamePattern;
typedef struct { const uint8_t *bytes; uint64_t hash; uint8_t length; } Mention;
struct PathProgram {
    PathEntry *entries; size_t capacity, count;
    NamePattern *names; uint32_t *name_buckets; size_t name_count, name_capacity, name_bucket_count;
    uint64_t wildcard_lengths[3]; uint32_t max_name_length; uint8_t has_positions;
    Mention *mentions; size_t mention_capacity, mention_count;
};
static size_t mention_bucket(uint64_t hash, size_t capacity){ return (hash ^ (hash >> 32)) & (capacity - 1); }
static void mention_place(Mention *mentions, size_t capacity, Mention value){
    size_t at = mention_bucket(value.hash, capacity);
    while (mentions[at].length) at = (at + 1) & (capacity - 1);
    mentions[at] = value;
}
static void mention_add(PathProgram *program, const uint8_t *bytes, uint8_t length, uint64_t hash){
    if (!program->mention_capacity || (program->mention_count + 1) * 2 >= program->mention_capacity) {
        size_t capacity = program->mention_capacity ? program->mention_capacity * 2 : 128;
        if (capacity < program->mention_capacity || capacity > SIZE_MAX / sizeof(Mention)) { fputs("recipe mention index exceeds address space\n", stderr); abort(); }
        Mention *mentions = calloc(capacity, sizeof *mentions); if (!mentions) { perror("recipe mention index"); abort(); }
        for (size_t i = 0; i < program->mention_capacity; i++) if (program->mentions[i].length) mention_place(mentions, capacity, program->mentions[i]);
        free(program->mentions); program->mentions = mentions; program->mention_capacity = capacity;
    }
    size_t at = mention_bucket(hash, program->mention_capacity);
    while (program->mentions[at].length) {
        const Mention *entry = &program->mentions[at];
        if (entry->hash == hash && entry->length == length && !memcmp(entry->bytes, bytes, length)) return;
        at = (at + 1) & (program->mention_capacity - 1);
    }
    program->mentions[at] = (Mention){bytes, hash, length}; program->mention_count++;
}
static void compile_mentions(Say *say){
    if (!say->said) return;
    /* Preserve the DSL's exact space-bounded mention semantics, including
     * names containing spaces. This scan happens once, never on a record. */
    for (size_t begin = 0; begin < say->nsaid; begin++) if (say->said[begin] == ' ') {
        uint64_t hash = NAME_HASH_START;
        for (size_t end = begin + 1; end < say->nsaid && end - begin <= 63; end++) {
            if (say->said[end] == ' ' && end > begin + 1) mention_add(say->paths, (const uint8_t *)say->said + begin + 1,
                (uint8_t)(end - begin - 1), hash);
            hash = (hash ^ (uint8_t)say->said[end]) * NAME_HASH_PRIME;
        }
    }
}
static int mentioned(const PathProgram *program, const uint8_t *bytes, uint32_t length){
    if (!program->mention_capacity || !length || length > 62) return 0;
    uint64_t hash = selector_hash(bytes, length); size_t at = mention_bucket(hash, program->mention_capacity);
    while (program->mentions[at].length) {
        const Mention *entry = &program->mentions[at];
        if (entry->hash == hash && entry->length == length && !memcmp(entry->bytes, bytes, length)) return 1;
        at = (at + 1) & (program->mention_capacity - 1);
    }
    return 0;
}
static __thread const Say *active_say;

static NameCode compile_name(const char *bytes, size_t length){
    NameCode code = {.bytes=bytes, .length=(uint32_t)length};
    if (length >= 5 && !memcmp(bytes, "note:", 5)) { code.note = 1; code.bytes += 5; code.length -= 5; }
    if (code.length && code.bytes[code.length - 1] == '*') { code.wildcard = 1; code.length--; }
    return code;
}
enum { KEY_LITERAL = 1, KEY_CAPTURE, KEY_FIELD };
typedef struct { NameCode text; uint8_t operation, capture; } KeyInstruction;
typedef struct { KeyInstruction *instructions; uint16_t count; } KeyTemplate;
struct KeyProgram { KeyTemplate forms[4]; };
static void compile_key_forms(KSpec *spec){
    if (spec->program || !spec->nas) return;
    KeyProgram *program = calloc(1, sizeof *program); if (!program) { perror("key template program"); abort(); }
    for (int i = 0; i < spec->nas; i++) {
        const char *text = spec->as[i]; size_t length = strlen(text), at = 0;
        KeyTemplate *form = &program->forms[i]; form->instructions = calloc(length ? length : 1, sizeof *form->instructions);
        if (!form->instructions) { perror("key instructions"); abort(); }
        while (at < length) {
            KeyInstruction instruction = {0};
            if (text[at] == '\\' && at + 1 < length && text[at + 1] >= '0' && text[at + 1] <= '9') {
                instruction.operation = KEY_CAPTURE; instruction.capture = (uint8_t)(text[at + 1] - '0'); at += 2;
            } else if (text[at] == '{' && memchr(text + at + 1, '}', length - at - 1)) {
                const char *end = memchr(text + at + 1, '}', length - at - 1);
                instruction.operation = KEY_FIELD; instruction.text.bytes = text + at + 1;
                instruction.text.length = (uint32_t)(end - instruction.text.bytes); at = (size_t)(end - text) + 1;
            } else {
                size_t begin = at++;
                while (at < length) {
                    if (text[at] == '\\' && at + 1 < length && text[at + 1] >= '0' && text[at + 1] <= '9') break;
                    if (text[at] == '{' && memchr(text + at + 1, '}', length - at - 1)) break;
                    at++;
                }
                instruction.operation = KEY_LITERAL; instruction.text.bytes = text + begin; instruction.text.length = (uint32_t)(at - begin);
            }
            form->instructions[form->count++] = instruction;
        }
    }
    spec->program = program;
}
static PathStep *compile_steps(const char *bytes, size_t length, PathStep *tail);
static PathStep *compile_step(const char *bytes, size_t length, PathStep *tail){
    PathStep *step = calloc(1, sizeof *step); if (!step) { perror("recipe selector"); abort(); }
    step->next = tail; step->operation = PATH_CHILD;
    if (length == 1 && bytes[0] == '.') { step->operation = PATH_SELF; return step; }
    if (length && bytes[0] == '^') {
        step->operation = length == 1 ? PATH_PARENT : PATH_ANCESTOR;
        step->name = (NameCode){.bytes=bytes + 1, .length=(uint32_t)(length - 1)}; return step;
    }
    const char *bracket = memchr(bytes, '[', length), *equals = bracket ? memchr(bracket, '=', length - (size_t)(bracket - bytes)) : NULL;
    size_t name_length = length;
    if (bracket && equals && length && bytes[length - 1] == ']') {
        name_length = (size_t)(bracket - bytes);
        step->filter = compile_steps(bracket + 1, (size_t)(equals - bracket - 1), NULL);
        step->filter_value = equals + 1; step->filter_length = (uint32_t)(bytes + length - 1 - (equals + 1));
    }
    step->name = compile_name(bytes, name_length);
    if (!bracket && name_length) {
        uint32_t position = 0; int number = 1;
        for (size_t i = 0; i < name_length; i++) {
            if (bytes[i] < '0' || bytes[i] > '9' || position > (UINT32_MAX - (unsigned)(bytes[i] - '0')) / 10u) { number = 0; break; }
            position = position * 10u + (unsigned)(bytes[i] - '0');
        }
        if (number) { step->operation = PATH_POSITION; step->position = position; }
        const char *dot = NULL;
        for (size_t i = 0; i < name_length; i++) if (bytes[i] == '.') dot = bytes + i;
        if (dot && dot != bytes && dot + 1 < bytes + name_length) {
            PathStep *child = compile_step(dot + 1, (size_t)(bytes + name_length - dot - 1), tail);
            step->alternative = compile_step(bytes, (size_t)(dot - bytes), child);
        }
    }
    return step;
}
static PathStep *compile_steps(const char *bytes, size_t length, PathStep *tail){
    while (length && *bytes == '/') { bytes++; length--; }
    if (!length) return tail;
    size_t split = 0; unsigned brackets = 0;
    for (; split < length; split++) {
        if (bytes[split] == '[') brackets++;
        else if (bytes[split] == ']' && brackets) brackets--;
        else if (bytes[split] == '/' && !brackets) break;
    }
    PathStep *next = split < length ? compile_steps(bytes + split + 1, length - split - 1, tail) : tail;
    return compile_step(bytes, split, next);
}
static size_t path_bucket(uint32_t offset, size_t capacity){ return ((uint64_t)offset * UINT64_C(11400714819323198485)) & (capacity - 1); }
static void path_insert(PathProgram *program, PathEntry entry){
    size_t at = path_bucket(entry.offset, program->capacity);
    while (program->entries[at].offset) at = (at + 1) & (program->capacity - 1);
    program->entries[at] = entry; program->count++;
}
static void path_register(Say *say, const char *name){
    if (!*name) return;
    PathProgram *program = say->paths;
    uint32_t offset = (uint32_t)(name - (const char *)say) + 1;
    if ((size_t)offset > sizeof *say) { fputs("selector is outside its recipe\n", stderr); abort(); }
    if ((program->count + 1) * 2 >= program->capacity) {
        size_t old_capacity = program->capacity; PathEntry *old = program->entries;
        program->capacity = old_capacity ? old_capacity * 2 : 128;
        program->entries = calloc(program->capacity, sizeof *program->entries); if (!program->entries) { perror("selector index"); abort(); }
        program->count = 0;
        for (size_t i = 0; i < old_capacity; i++) if (old[i].offset) path_insert(program, old[i]);
        free(old);
    }
    size_t at = path_bucket(offset, program->capacity);
    while (program->entries[at].offset && program->entries[at].offset != offset) at = (at + 1) & (program->capacity - 1);
    if (program->entries[at].offset) return;
    PathCode *code = malloc(sizeof *code); if (!code) { perror("selector program"); abort(); }
    size_t length = strlen(name); code->whole = compile_name(name, length); code->first = compile_steps(name, length, NULL); code->source_length = (uint16_t)length;
    code->special = length == 1 && name[0] == '.' ? SELECT_SELF : length == 6 && !memcmp(name, "{file}", 6) ? SELECT_FILE :
                    length == 2 && !memcmp(name, "*/", 2) ? SELECT_ELEMENTS : length == 3 && !memcmp(name, "...", 3) ? SELECT_UNNAMED : SELECT_FIELD;
    path_insert(program, (PathEntry){offset, code});
    if (*name == '>') path_register(say, name + 1);
}
static void register_att_paths(Say *say, const Att *att, int count){
    for (int i = 0; i < count; i++) {
        path_register(say, att[i].tier); path_register(say, att[i].by); path_register(say, att[i].of);
        for (int j = 0; j < att[i].n; j++) path_register(say, att[i].name[j]);
    }
}
static void compile_targets(Say *say, Dis *rules, int count){
    for (int i = 0; i < count; i++) {
        Dis *rule = &rules[i]; if (rule->targets || (rule->what != D_TYPE && rule->what != D_REFER)) continue;
        size_t length = strlen(rule->arg), capacity = length / 2 + 1;
        rule->targets = calloc(capacity, sizeof *rule->targets); if (!rule->targets) { perror("reference targets"); abort(); }
        const char *at = rule->arg, *end = at + length;
        while (at < end) {
            while (at < end && *at == ' ') at++;
            if (at == end) break;
            const char *next = memchr(at, ' ', (size_t)(end - at)); if (!next) next = end;
            TargetCode *target = &rule->targets[rule->ntargets++];
            if (rule->what == D_REFER && *at == '^') { target->within = 1; at++; }
            target->length = (uint16_t)(next - at); memcpy(target->name, at, target->length); target->name[target->length] = 0;
            for (int j = 0; j < say->nkey; j++) if (!strcmp(say->key[j].tier, target->name)) { target->local = 1; target->tier_id = say->key[j].tier_id; break; }
            at = next;
        }
    }
}
static uint32_t intern_name(PathProgram *program, NameCode *name){
    uint64_t hash = disposition_key(selector_hash((const uint8_t *)name->bytes, name->length), 0, name->note, name->wildcard, 0);
    for (size_t i = 0; i < program->name_count; i++) {
        const NamePattern *pattern = &program->names[i];
        if (pattern->hash == hash && pattern->name.length == name->length && pattern->name.note == name->note &&
            pattern->name.wildcard == name->wildcard && (!name->length || !memcmp(pattern->name.bytes, name->bytes, name->length))) {
            name->slot = (uint32_t)(i + 1); return name->slot;
        }
    }
    if (program->name_count == program->name_capacity) {
        size_t capacity = program->name_capacity ? program->name_capacity * 2 : 64;
        if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof *program->names) { fputs("too many compiled name selectors\n", stderr); abort(); }
        NamePattern *names = realloc(program->names, capacity * sizeof *names); if (!names) { perror("compiled name slots"); abort(); }
        program->names = names; program->name_capacity = capacity;
    }
    name->slot = (uint32_t)++program->name_count;
    program->names[name->slot - 1] = (NamePattern){.name=*name, .hash=hash}; return name->slot;
}
static void intern_steps(PathProgram *program, PathStep *step){
    while (step) {
        if (step->name.slot) return;
        intern_name(program, &step->name);
        if (step->operation == PATH_POSITION) program->has_positions = 1;
        if (step->filter) intern_steps(program, step->filter);
        if (step->alternative) intern_steps(program, step->alternative);
        step = step->next;
    }
}
static void intern_key_fields(PathProgram *program, KSpec *spec){
    if (!spec->program) return;
    for (int i = 0; i < spec->nas; i++) {
        KeyTemplate *form = &spec->program->forms[i];
        for (uint16_t j = 0; j < form->count; j++) if (form->instructions[j].operation == KEY_FIELD) intern_name(program, &form->instructions[j].text);
    }
}
static void intern_dispositions(PathProgram *program, Dis *rules, int count){
    for (int i = 0; i < count; i++) {
        Dis *rule = &rules[i];
        NameCode whole = {.bytes=rule->name + rule->whole.off, .length=rule->whole.len, .wildcard=rule->whole.wild, .note=rule->whole.note};
        rule->name_slot = intern_name(program, &whole);
        if (rule->el_l) {
            NameCode part = {.bytes=rule->name + rule->part.off, .length=rule->part.len, .wildcard=rule->part.wild, .note=rule->part.note};
            NameCode parent = {.bytes=rule->name, .length=rule->el_l};
            rule->part_slot = intern_name(program, &part); rule->parent_slot = intern_name(program, &parent);
        }
        intern_key_fields(program, &rule->k);
    }
}
static void compile_name_slots(Say *say){
    PathProgram *program = say->paths;
    NameCode sc = compile_name("sc", 2); say->sc_slot = intern_name(program, &sc);
    for (size_t i = 0; i < program->capacity; i++) if (program->entries[i].offset) {
        PathCode *path = program->entries[i].code; intern_name(program, &path->whole); intern_steps(program, path->first);
    }
    intern_dispositions(program, say->dis, say->ndis); intern_dispositions(program, say->val, say->nval);
    for (int i = 0; i < say->nthing; i++) for (int j = 0; j < say->thing[i].n; j++) intern_key_fields(program, &say->thing[i].form[j]);
    for (int i = 0; i < say->nhw; i++) { intern_key_fields(program, &say->hw[i].a); intern_key_fields(program, &say->hw[i].b); }
    program->name_bucket_count = 8;
    while (program->name_bucket_count < program->name_count * 2) program->name_bucket_count *= 2;
    program->name_buckets = calloc(program->name_bucket_count, sizeof *program->name_buckets);
    if (!program->name_buckets) { perror("compiled binding name index"); abort(); }
    for (size_t i = 0; i < program->name_count; i++) {
        NamePattern *pattern = &program->names[i]; size_t bucket = (pattern->hash ^ (pattern->hash >> 32)) & (program->name_bucket_count - 1);
        pattern->next = program->name_buckets[bucket]; program->name_buckets[bucket] = (uint32_t)(i + 1);
        uint32_t length = pattern->name.length;
        if (length > program->max_name_length) program->max_name_length = length;
        if (pattern->name.wildcard) {
            if (length >= 192) { fputs("compiled name prefix exceeds the selector width\n", stderr); abort(); }
            program->wildcard_lengths[length >> 6] |= UINT64_C(1) << (length & 63);
        }
    }
}
static void compile_semantic_paths(Say *say){
    if (say->paths) return;
    say->paths = calloc(1, sizeof *say->paths); if (!say->paths) { perror("recipe selectors"); abort(); }
    compile_mentions(say);
    for (int i = 0; i < say->nthing; i++) {
        path_register(say, say->thing[i].tier);
        for (int j = 0; j < say->thing[i].n; j++) { path_register(say, say->thing[i].name[j]); path_register(say, say->thing[i].form[j].path); compile_key_forms(&say->thing[i].form[j]); }
    }
    register_att_paths(say, say->att, say->natt); register_att_paths(say, say->pair, say->npair);
    register_att_paths(say, say->hold, say->nhold); register_att_paths(say, say->itself, say->nitself);
    register_att_paths(say, say->together, say->ntogether); register_att_paths(say, say->voices, say->nvoices);
    for (int i = 0; i < say->nrel; i++) {
        path_register(say, say->rel[i].tier); path_register(say, say->rel[i].rel); path_register(say, say->rel[i].to); path_register(say, say->rel[i].by);
    }
    for (int i = 0; i < say->nkey; i++) {
        Pair *key = &say->key[i]; path_register(say, key->tier); path_register(say, key->name);
        key->tier_length = (uint16_t)strlen(key->tier); key->tier_id = (uint16_t)(i + 1);
        for (int j = 0; j < i; j++) if (!strcmp(say->key[j].tier, key->tier)) { key->tier_id = say->key[j].tier_id; break; }
        if (!strcmp(key->tier, say->tree_row)) say->tree_key_index = (uint16_t)(i + 1);
    }
    for (int i = 0; i < say->nwhere; i++) path_register(say, say->where[i].name);
    for (int i = 0; i < say->nscore; i++) { path_register(say, say->score[i].tier); path_register(say, say->score[i].name); }
    for (int i = 0; i < say->nhw; i++) { path_register(say, say->hw[i].tier); path_register(say, say->hw[i].a.path); path_register(say, say->hw[i].b.path); compile_key_forms(&say->hw[i].a); compile_key_forms(&say->hw[i].b); }
    for (int i = 0; i < say->ndis; i++) compile_key_forms(&say->dis[i].k);
    for (int i = 0; i < say->nval; i++) compile_key_forms(&say->val[i].k);
    path_register(say, say->lay_tier); path_register(say, say->lay_row); path_register(say, say->tree_tier); path_register(say, say->tree_row);
    path_register(say, say->tree_head); path_register(say, say->tree_rel); path_register(say, say->voice.tier); path_register(say, say->voice.name);
    for (int i = 0; i < say->nlayer; i++) path_register(say, say->layer[i]);
    compile_targets(say, say->dis, say->ndis); compile_targets(say, say->val, say->nval);
    compile_diagnostics(say->dis, say->ndis); compile_diagnostics(say->val, say->nval);
    for (int i = 0; i < say->nline; i++) say->line[i].pred_length = (uint16_t)strlen(say->line[i].pred);
    compile_name_slots(say);
}
static const PathCode empty_path;
static const PathCode *path_code(const char *name){
    if (!name || !*name) return &empty_path;
    const Say *say = active_say;
    uintptr_t offset = say ? (uintptr_t)name - (uintptr_t)say + 1 : 0;
    if (!say || !say->paths || !say->paths->capacity || offset > sizeof *say || !offset) { fputs("uncompiled semantic selector\n", stderr); abort(); }
    const PathProgram *program = say->paths; size_t at = path_bucket((uint32_t)offset, program->capacity);
    while (program->entries[at].offset) {
        if (program->entries[at].offset == offset) return program->entries[at].code;
        at = (at + 1) & (program->capacity - 1);
    }
    fputs("semantic selector was not compiled at recipe load\n", stderr); abort();
}

static char *name_next(char **at, char *out, size_t cap){
    char *p = *at; if (!p) return NULL; while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\r' || *p == '\n') return NULL;
    char *e; if (*p == '"') { p++; e = strchr(p, '"'); if (!e) return NULL; snprintf(out, cap, "%.*s", (int)(e - p), p); *at = e + 1; }
    else { e = p; while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++; snprintf(out, cap, "%.*s", (int)(e - p), p); *at = e; }
    return out;
}
/* A name as it is matched, split once when the recipe is read, not again for every part of every row: NAME, NAME* (every
 * name that begins so), note:NAME (a note of that name); and ELEMENT.NAME, NAME scoped to what holds it. */
static void name_split(const char *n, size_t at, size_t end, uint8_t *off, uint8_t *len, uint8_t *wild, uint8_t *note){
    *note = end - at >= 5 && !strncmp(n + at, "note:", 5); if (*note) at += 5;
    size_t l = end - at; *wild = l && n[at + l - 1] == '*'; if (*wild) l--; *off = (uint8_t)at; *len = (uint8_t)l; }
static void dis_parse(Dis *d){
    size_t n = strlen(d->name); name_split(d->name, 0, n, &d->whole.off, &d->whole.len, &d->whole.wild, &d->whole.note);
    const char *dot = strrchr(d->name, '.'); d->el_l = dot && dot != d->name ? (uint8_t)(dot - d->name) : 0;
    if (d->el_l) name_split(d->name, (size_t)d->el_l + 1, n, &d->part.off, &d->part.len, &d->part.wild, &d->part.note); }
/* A part named as a property the perf-cache's flags hold: under perfcache it is read from there, never attested. */
static size_t perf_fold(const char *in, size_t n, char *out){              /* the standard's loose match: case, spaces, - and _ do not count */
    size_t o = 0; for (size_t i = 0; i < n && o < 63; i++) { char c = in[i]; if (c == '_' || c == '-' || c == ' ') continue; out[o++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
    out[o] = 0; return o; }
static int in_perf(const Say *s, const char *name, size_t n){
    if (!s->nperf || !n || n >= 64) return 0; char f[64]; perf_fold(name, n, f);
    for (int i = 0; i < s->nperf; i++) if (!strcmp(s->perf[i], f)) return 1; return 0; }
static int dis_add(Say *s, const char *path, const char *name, int what, const char *arg){
    if (s->ndis == 256) { fprintf(stderr, "%s: more parts than a recipe disposes of (256)\n", path); return -1; }
    for (int i = 0; i < s->ndis; i++) if (!strcmp(s->dis[i].name, name)) { if (what == D_KEY && s->dis[i].what == D_KEY) return 1;      /* one name may be the key of several kinds of thing (id) */
        fprintf(stderr, "%s: the part %s is given two dispositions\n", path, name); return -1; }
    Dis *d = &s->dis[s->ndis++]; snprintf(d->name, sizeof d->name, "%s", name); dis_parse(d); d->what = what; snprintf(d->arg, sizeof d->arg, "%s", arg ? arg : ""); return 1;
}
/* A recipe's line, if it lays the file out or disposes of a part: 1, or 0 for something else, or -1 when written wrong. */
/* A recipe's line, its comment off, as written: each word of it kept, so that a part the recipe names nowhere is known to be
 * left open rather than read past (unmentioned). A word: what lies between spaces, quotes, and the marks a path, a key's
 * template or a list's name is written with (^ / : { } , ( ) = ). */
void say_mentions(Recipe *r, const char *line){
    Say *s = r->say; if (!s || !line) return; if (s->laid_line) { s->laid_line = 0; return; }   /* a layout's names are not a reading */
    for (const char *c = line; *c; ) {
        while (*c && strchr(" \t\r\n\"/:{},()=^", *c)) c++; const char *b = c; while (*c && !strchr(" \t\r\n\"/:{},()=^", *c)) c++;
        if (c == b) continue; size_t l = (size_t)(c - b);
        s->said = xrealloc(s->said, s->nsaid + l + 3); if (!s->nsaid) s->said[s->nsaid++] = ' ';
        memcpy(s->said + s->nsaid, b, l); s->nsaid += l; s->said[s->nsaid++] = ' '; s->said[s->nsaid] = 0; }
}
int say_says(Recipe *r, const char *path, char *tok){
    Say *s = r->say; if (!s) s = r->say = calloc(1, sizeof(Say)); s->laid_line = 0;
    char nm[160], a2[64], *at;
    #define LAID (s->lay.ntier || s->lay.g.n)
    if (!strcmp(tok, "format")) {                                            /* the format's own lines, kept once for every recipe of that format: recipes/formats/NAME.format */
        char *fn = strtok(NULL, " \t\r\n"), fp[4400], line[2048]; if (!fn) { fprintf(stderr, "%s: format NAME\n", path); return -1; }
        snprintf(fp, sizeof fp, "%s/formats/%s.format", laplace_recipes(), fn); FILE *f = fopen(fp, "r"); if (!f) { perror(fp); return -1; }
        while (fgets(line, sizeof line, f)) { comment_off(line); char whole[2048]; snprintf(whole, sizeof whole, "%s", line); char *t2 = strtok(line, " \t\r\n"); if (!t2) continue;
            if (!strcmp(t2, "grammar")) { char *g = strtok(NULL, " \t\r\n"); if (g) snprintf(r->grammar, sizeof r->grammar, "%s", g); continue; }
            int sv = say_says(r, fp, t2); if (sv > 0) say_mentions(r, whole); if (sv <= 0) { if (!sv) fprintf(stderr, "%s: \"%s\" is not something a format says\n", fp, t2); fclose(f); return -1; } }
        fclose(f); return 1; }
    if (!strcmp(tok, "thing")) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->nthing == 32) { fprintf(stderr, "%s: thing TIER NAME... [within] [resolved]\n", path); return -1; }
        Thing *p = &s->thing[s->nthing++]; snprintf(p->tier, sizeof p->tier, "%s", a2); while (name_next(&at, nm, sizeof nm)) { if (!strcmp(nm, "within")) { p->within = 1; continue; } if (!strcmp(nm, "within-file")) { p->infile = 1; continue; }
            /* literal TEXT: a part the recipe writes, as the source's own documentation words it (a source that writes
             * the language of its words nowhere but in its README); NAME matching PATTERN: the part is the pattern's
             * first group of NAME's text, as the source's notation parts it (a sense key's lemma, lemma%...); NAME space
             * CHAR: CHAR in its text stands for a space (senseidx.5: collocations "joining individual words with an
             * underscore") */
            if (!strcmp(nm, "literal")) { if (p->n == 8 || !name_next(&at, nm, sizeof nm)) { fprintf(stderr, "%s: thing TIER ... literal TEXT (at most 8 parts)\n", path); return -1; } p->lit[p->n] = 1; snprintf(p->name[p->n++], sizeof p->name[0], "%s", nm); continue; }
            if (!strcmp(nm, "matching") && p->n && !p->lit[p->n - 1]) { char pat[160]; KSpec *ks = &p->form[p->n - 1];
                if (!name_next(&at, pat, sizeof pat) || regcomp(&ks->re, pat, REG_EXTENDED)) { fprintf(stderr, "%s: thing TIER NAME matching PATTERN (it must compile)\n", path); return -1; } ks->has_re = 1; continue; }
            if (!strcmp(nm, "space") && p->n && !p->lit[p->n - 1]) { char ch[8]; if (!name_next(&at, ch, sizeof ch) || strlen(ch) != 1) { fprintf(stderr, "%s: thing TIER NAME space CHAR\n", path); return -1; } p->space[p->n - 1] = ch[0]; continue; }
            if (!strcmp(nm, "span")) { p->span = 1; continue; } if (!strcmp(nm, "inclusive")) { p->inclusive = 1; continue; } if (!strcmp(nm, "of")) { p->of = 1; continue; } if (!strcmp(nm, "resolved")) { p->resolved = 1; continue; } if (p->n < 8) snprintf(p->name[p->n++], sizeof p->name[0], "%s", nm); }
        if (p->span && p->n != 3) { fprintf(stderr, "%s: thing TIER span START END TEXT [inclusive]\n", path); return -1; }
        if (!p->n) { fprintf(stderr, "%s: thing TIER NAME... [within]\n", path); return -1; } return 1; }
    if ((!strcmp(tok, "voices") || !strcmp(tok, "together")) && LAID) { int v = tok[0] == 'v'; at = strtok(NULL, "\r\n");
        Att *x = v ? &s->voices[s->nvoices] : &s->together[s->ntogether]; if (!name_next(&at, a2, sizeof a2) || (v ? s->nvoices : s->ntogether) == 8) { fprintf(stderr, "%s: %s TIER%s\n", path, tok, v ? " NAME... [within-file]" : ""); return -1; }
        memset(x, 0, sizeof *x); snprintf(x->tier, sizeof x->tier, "%s", a2); x->when = s->now;
        while (v && x->n < 64 && name_next(&at, nm, sizeof nm)) { if (!strcmp(nm, "within-file")) { x->file = 1; continue; } snprintf(x->name[x->n++], 64, "%s", nm); }
        if (v && !x->n) { fprintf(stderr, "%s: voices TIER NAME...\n", path); return -1; }
        if (v) s->nvoices++; else s->ntogether++; return 1; }
    if (!strcmp(tok, "itself") && LAID) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->nitself == 8) { fprintf(stderr, "%s: itself TIER [by PATH]\n", path); return -1; }
        Att *x = &s->itself[s->nitself++]; memset(x, 0, sizeof *x); snprintf(x->tier, sizeof x->tier, "%s", a2); x->when = s->now;
        if (name_next(&at, nm, sizeof nm)) { if (strcmp(nm, "by") || !name_next(&at, x->by, sizeof x->by)) { fprintf(stderr, "%s: itself TIER [by PATH]\n", path); return -1; } }
        return 1; }
    if (!strcmp(tok, "holds") && LAID) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->nhold == 16) { fprintf(stderr, "%s: holds TIER NAME... [via]\n", path); return -1; }
        Att *x = &s->hold[s->nhold++]; memset(x, 0, sizeof *x); snprintf(x->tier, sizeof x->tier, "%s", a2); x->when = s->now;
        while (x->n < 64 && name_next(&at, nm, sizeof nm)) { if (!strcmp(nm, "via")) { x->file = 1; continue; } snprintf(x->name[x->n++], 64, "%s", nm); }    /* via: under the name of the element between the two (RDF's property) */
        if (!x->n) { fprintf(stderr, "%s: holds TIER NAME...\n", path); return -1; } return 1; }
    if (!strcmp(tok, "key") && LAID) { at = strtok(NULL, "\r\n");
        if (!name_next(&at, a2, sizeof a2) || !name_next(&at, nm, sizeof nm) || s->nkey == S_TIERS) { fprintf(stderr, "%s: key TIER NAME\n", path); return -1; }
        Pair *p = &s->key[s->nkey++]; snprintf(p->tier, sizeof p->tier, "%s", a2); snprintf(p->name, sizeof p->name, "%s", nm);
        return dis_add(s, path, nm, D_KEY, a2); }
    if (!strcmp(tok, "pair") && LAID) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->npair == 8) { fprintf(stderr, "%s: pair TIER NAME...\n", path); return -1; }
        Att *x = &s->pair[s->npair++]; memset(x, 0, sizeof *x); snprintf(x->tier, sizeof x->tier, "%s", a2); x->when = s->now;
        while (x->n < 64 && name_next(&at, nm, sizeof nm)) { if (!strcmp(nm, "of")) { if (!name_next(&at, x->of, sizeof x->of)) { fprintf(stderr, "%s: pair TIER NAME... [of PATH]\n", path); return -1; } continue; } snprintf(x->name[x->n++], 64, "%s", nm); }
        if (!x->n) { fprintf(stderr, "%s: pair TIER NAME... [of PATH]\n", path); return -1; } return 1; }
    if (!strcmp(tok, "score") && LAID) { char w1[16], lo[32], w2[16], hi[32]; at = strtok(NULL, "\r\n");
        if (!name_next(&at, a2, sizeof a2) || !name_next(&at, nm, sizeof nm) || s->nscore == 4) { fprintf(stderr, "%s: score TIER NAME [from A to B]\n", path); return -1; }
        Score *x = &s->score[s->nscore++]; snprintf(x->tier, sizeof x->tier, "%s", a2); snprintf(x->name, sizeof x->name, "%s", nm); x->from = 0; x->to = 1; x->mapped = 0;
        if (name_next(&at, w1, sizeof w1)) { if (strcmp(w1, "from") || !name_next(&at, lo, sizeof lo) || !name_next(&at, w2, sizeof w2) || strcmp(w2, "to") || !name_next(&at, hi, sizeof hi) || atof(hi) <= atof(lo)) { fprintf(stderr, "%s: score TIER NAME from A to B\n", path); return -1; }
            x->from = atof(lo); x->to = atof(hi); x->mapped = 1; }
        return 1; }                                                          /* the part is still what its own disposition says (content, usually) */
    if (!strcmp(tok, "always") && LAID) { s->now = 0; return 1; }
    /* Every part the recipe speaks of whole (the outermost tier) is a record: a path in the file's content tree over its
     * thing and the claims it says (record_of). Where the recipe gives them, the record holds its rows' layers and tree too:
     * layers TIER ROW NAME...            each NAME a path down the TIER's ROWs, aligned to them: a value the row leaves
     *                                    empty stands as the file writes empty
     * tree TIER ROW HEAD REL             the ROWs' tree by HEAD (a row's key, or 0 for none), each subtree the path of its
     *                                    dependents' subtrees and [REL, the row's thing] in the rows' order */
    if (!strcmp(tok, "layers") && LAID) { at = strtok(NULL, "\r\n");
        if (!name_next(&at, s->lay_tier, sizeof s->lay_tier) || !name_next(&at, s->lay_row, sizeof s->lay_row)) { fprintf(stderr, "%s: layers TIER ROW NAME...\n", path); return -1; }
        while (s->nlayer < 16 && name_next(&at, s->layer[s->nlayer], sizeof s->layer[0])) s->nlayer++;
        if (!s->nlayer) { fprintf(stderr, "%s: layers TIER ROW NAME...\n", path); return -1; } return 1; }
    if (!strcmp(tok, "tree") && LAID) { at = strtok(NULL, "\r\n");
        if (!name_next(&at, s->tree_tier, sizeof s->tree_tier) || !name_next(&at, s->tree_row, sizeof s->tree_row) || !name_next(&at, s->tree_head, sizeof s->tree_head) || !name_next(&at, s->tree_rel, sizeof s->tree_rel)) { fprintf(stderr, "%s: tree TIER ROW HEAD REL\n", path); return -1; }
        return 1; }
    if ((!strcmp(tok, "where") || !strcmp(tok, "when")) && LAID) { char op[16]; int when = tok[2] == 'e' && tok[3] == 'n'; at = strtok(NULL, "\r\n");
        if (!name_next(&at, nm, sizeof nm) || !name_next(&at, op, sizeof op) || s->nwhere == 16) { fprintf(stderr, "%s: %s NAME is VALUE | is-not VALUE | matches PATTERN\n", path, tok); return -1; }
        if (when) s->now = 1u << s->nwhere; else s->whole |= 1u << s->nwhere;
        Where *x = &s->where[s->nwhere++]; snprintf(x->name, sizeof x->name, "%s", nm); x->op = !strcmp(op, "is") ? 0 : !strcmp(op, "is-not") ? 1 : !strcmp(op, "matches") ? 2 : -1;
        while (at && (*at == ' ' || *at == '\t')) at++; snprintf(x->val, sizeof x->val, "%s", at ? at : ""); { size_t l = strlen(x->val); while (l && (x->val[l - 1] == '\n' || x->val[l - 1] == '\r' || x->val[l - 1] == ' ')) x->val[--l] = 0; }
        if (x->op < 0 || (x->op == 2 && regcomp(&x->re, x->val, REG_EXTENDED | REG_NOSUB))) { fprintf(stderr, "%s: where NAME is VALUE | is-not VALUE | matches PATTERN (the pattern must compile)\n", path); return -1; }
        return 1; }
    if (!strcmp(tok, "content") || !strcmp(tok, "metadata") || !strcmp(tok, "omit")) { int what = tok[0] == 'c' ? D_CONTENT : tok[0] == 'm' ? D_METADATA : D_OMIT; at = strtok(NULL, "\r\n"); int any = 0;
        while (name_next(&at, nm, sizeof nm)) { if (dis_add(s, path, nm, what, NULL) < 0) return -1; any = 1; }
        if (!any) { fprintf(stderr, "%s: %s NAME...\n", path, tok); return -1; } return 1; }
    if ((!strcmp(tok, "codepoints") || !strcmp(tok, "range")) && LAID) { int what = tok[0] == 'c' ? D_CODEPOINTS : D_RANGE; at = strtok(NULL, "\r\n"); int any = 0;
        while (name_next(&at, nm, sizeof nm)) { if (dis_add(s, path, nm, what, NULL) < 0) return -1; any = 1; }
        if (!any) { fprintf(stderr, "%s: %s NAME...\n", path, tok); return -1; } return 1; }
    if (!strcmp(tok, "voice") && LAID) { at = strtok(NULL, "\r\n");
        if (!name_next(&at, a2, sizeof a2) || !name_next(&at, nm, sizeof nm)) { fprintf(stderr, "%s: voice TIER NAME\n", path); return -1; }
        snprintf(s->voice.tier, sizeof s->voice.tier, "%s", a2); snprintf(s->voice.name, sizeof s->voice.name, "%s", nm); return 1; }      /* the part stays what its own disposition says */
    if ((!strcmp(tok, "refer") || !strcmp(tok, "type")) && LAID) { at = strtok(NULL, "\r\n");
        if (!name_next(&at, nm, sizeof nm) || !name_next(&at, a2, sizeof a2)) { fprintf(stderr, "%s: %s NAME %s\n", path, tok, tok[0] == 'r' ? "TIER" : "LIST..."); return -1; }
        KSpec ks; memset(&ks, 0, sizeof ks);
        if (tok[0] == 't') { char more[160]; while (name_next(&at, more, sizeof more)) {     /* type NAME LIST... [matching PATTERN] [as TEMPLATE]: the first list that knows it, by the key as those lists write it */
                if (!strcmp(more, "matching")) { if (!name_next(&at, more, sizeof more) || regcomp(&ks.re, more, REG_EXTENDED)) { fprintf(stderr, "%s: type NAME LIST... matching PATTERN (it must compile)\n", path); return -1; } ks.has_re = 1; continue; }
                if (!strcmp(more, "as")) { if (ks.nas == 4 || !name_next(&at, ks.as[ks.nas], sizeof ks.as[0])) { fprintf(stderr, "%s: type NAME LIST... as TEMPLATE... (at most 4)\n", path); return -1; } ks.nas++; continue; }
                if (!strcmp(more, "node")) { ks.node = 1; continue; }
                size_t l = strlen(a2); snprintf(a2 + l, sizeof a2 - l, " %s", more); }
            int rv = dis_add(s, path, nm, D_TYPE, a2); if (rv > 0) s->dis[s->ndis - 1].k = ks; return rv; }
        else { char more[64], targets[64]; snprintf(targets, sizeof targets, "%s", a2);       /* refer NAME TIER [within] [TIER [within]]...: the first that holds the key; within: the thing that TIER is inside */
            while (name_next(&at, more, sizeof more)) { size_t l = strlen(targets);
                if (!strcmp(more, "within")) { char *last = strrchr(targets, ' '); last = last ? last + 1 : targets; memmove(last + 1, last, strlen(last) + 1); *last = '^'; }
                else snprintf(targets + l, sizeof targets - l, " %s", more); }
            snprintf(a2, sizeof a2, "%s", targets); }
        return dis_add(s, path, nm, tok[0] == 'r' ? D_REFER : D_TYPE, a2); }
    if (!strcmp(tok, "perfcache") && LAID) {                                    /* nothing read from the flags is attested: without them, nothing is read at all */
        const lp_layout *l = lp_flags_map(NULL);
        if (!l) { fprintf(stderr, "%s: perfcache: the flags are not at %s (laplace flags generates them)\n", path, lp_flags_path()); return -1; }
        s->perf = xrealloc(s->perf, sizeof *s->perf * 2 * l->nfields); s->nperf = 0;
        for (size_t i = 0; i < l->nfields; i++) { perf_fold(l->field[i].name, strlen(l->field[i].name), s->perf[s->nperf++]); perf_fold(l->field[i].say, strlen(l->field[i].say), s->perf[s->nperf++]); }
        return 1; }
    if (!strcmp(tok, "attest") && LAID) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->natt == 32) { fprintf(stderr, "%s: attest TIER NAME... [of NAME] [by NAME]\n", path); return -1; }
        Att *x = &s->att[s->natt++]; memset(x, 0, sizeof *x); snprintf(x->tier, sizeof x->tier, "%s", a2); x->when = s->now;
        while (x->n < 64 && name_next(&at, nm, sizeof nm)) { if (!strcmp(nm, "by") || !strcmp(nm, "of")) { char w[64]; if (!name_next(&at, w, sizeof w)) { fprintf(stderr, "%s: attest ... %s NAME\n", path, nm); return -1; } snprintf(nm[0] == 'b' ? x->by : x->of, 64, "%s", w); continue; }
            snprintf(x->name[x->n++], 64, "%s", nm); }
        if (!x->n) { fprintf(stderr, "%s: attest TIER NAME... [of NAME] [by NAME]\n", path); return -1; } return 1; }
    if ((!strcmp(tok, "about") || !strcmp(tok, "line")) && LAID) {           /* PATTERN: the rest of the line; its parentheses are the parts it says */
        char *rest = strtok(NULL, "\r\n"); if (!rest) { fprintf(stderr, "%s: %s PATTERN\n", path, tok); return -1; } while (*rest == ' ' || *rest == '\t') rest++;
        char pat[1024]; snprintf(pat, sizeof pat, "%s", rest); int mode = 0; char pred[64] = "";
        if (tok[0] == 'l') { char *m = strstr(pat, " :: "); if (m) { *m = 0; char *w = m + 4; while (*w == ' ') w++;     /* PATTERN :: pair | claim | predicate NAME (an obligation) */
                if (!strncmp(w, "pair", 4)) mode = 2; else if (!strncmp(w, "claim", 5)) mode = 1; else if (!strncmp(w, "predicate ", 10)) { mode = 3; snprintf(pred, sizeof pred, "%s", w + 10); size_t pl = strlen(pred); while (pl && (pred[pl - 1] == ' ' || pred[pl - 1] == '\r')) pred[--pl] = 0; }
                else { fprintf(stderr, "%s: line PATTERN :: pair | claim | relation \"TEXT\" [within]\n", path); return -1; } } }
        regex_t *re = tok[0] == 'a' ? &s->about_re : (s->nline < 8 ? &s->line[s->nline].re : NULL);
        if (!re || regcomp(re, pat, REG_EXTENDED)) { fprintf(stderr, "%s: the pattern does not compile (or there are more than 8): %s\n", path, pat); return -1; }
        if (tok[0] == 'a') s->has_about = 1; else { s->line[s->nline].mode = mode; snprintf(s->line[s->nline].pred, 64, "%s", pred); s->nline++; }
        return 1; }
    if (!strcmp(tok, "split") && LAID) { at = strtok(NULL, "\r\n"); while (s->nsplit < 16 && name_next(&at, nm, sizeof nm)) snprintf(s->split[s->nsplit++], 64, "%s", nm);
        if (!s->nsplit) { fprintf(stderr, "%s: split ELEMENT...\n", path); return -1; } return 1; }
    if (!strcmp(tok, "itself-mark") && LAID) { char *c = strtok(NULL, " \t\r\n"), b[8]; if (!c) { fprintf(stderr, "%s: itself-mark CHAR\n", path); return -1; }
        snprintf(b, sizeof b, "%s", !strcmp(c, "hash") ? "#" : c); s->selfmark = b[0]; return 1; }
    if (!strcmp(tok, "stem") && LAID) { char *a = strtok(NULL, " \t\r\n"), *b = strtok(NULL, " \t\r\n"); if (!a) { fprintf(stderr, "%s: stem [FROM] TO\n", path); return -1; }
        if (b) { s->stemfrom = a[0]; s->stem = b[0]; } else s->stem = a[0]; return 1; }      /* {file}: the file's name after its last FROM, up to the TO after it */
    if (!strcmp(tok, "value")) { at = strtok(NULL, "\r\n"); int any = 0;          /* value LIST NAME...: what the parts mean in a claim, a value of that road class */
        if (!name_next(&at, a2, sizeof a2) || strlen(a2) >= 32) { fprintf(stderr, "%s: value LIST NAME... (LIST: the highway's list, or -)\n", path); return -1; }
        while (name_next(&at, nm, sizeof nm)) {
            if (!strcmp(nm, "{file}")) { fprintf(stderr, "%s: value: a file's name is never what a claim means (Laplace-Engine#32)\n", path); return -1; }
            if (s->nval == 256) { fprintf(stderr, "%s: more parts than value and relation name (256)\n", path); return -1; }
            Dis *d = &s->val[s->nval++]; memset(d, 0, sizeof *d); snprintf(d->name, sizeof d->name, "%s", nm); dis_parse(d); d->when = s->now; snprintf(d->arg, sizeof d->arg, "%s", strcmp(a2, "-") ? a2 : ""); any = 1; }
        if (!any) { fprintf(stderr, "%s: value LIST NAME...\n", path); return -1; } return 1; }
    if (!strcmp(tok, "relation")) { at = strtok(NULL, "\r\n"); int any = 0;     /* relation "TEXT" NAME...: each part states that relation, in the words the source's documentation gives it */
        char tx[512]; if (!name_next(&at, tx, sizeof tx) || !tx[0] || strlen(tx) >= sizeof tx - 1) { fprintf(stderr, "%s: relation \"TEXT\" NAME... (TEXT: the relation as the source's documentation words it)\n", path); return -1; }
        while (name_next(&at, nm, sizeof nm)) {
            const char *dot = strrchr(nm, '.'); const char *bare = dot ? dot + 1 : nm;
            if (!strcmp(nm, "{file}")) { fprintf(stderr, "%s: relation: a file's name is never what a claim means (Laplace-Engine#32)\n", path); return -1; }
            if (!strcmp(tx, nm) || !strcmp(tx, bare)) { fprintf(stderr, "%s: relation \"%s\" %s: a field's own name is never its relation (Laplace-Engine#32); write what the documentation says it states\n", path, tx, nm); return -1; }
            if (s->nval == 256) { fprintf(stderr, "%s: more parts than value and relation name (256)\n", path); return -1; }
            Dis *d = &s->val[s->nval++]; memset(d, 0, sizeof *d); snprintf(d->name, sizeof d->name, "%s", nm); dis_parse(d); d->when = s->now;
            size_t length = strlen(tx) + 1; char *text = malloc(length); if (!text) { perror("recipe relation"); abort(); }
            memcpy(text, tx, length); d->text = text; d->what = 1; any = 1; }      /* kept for the recipe's life */
        if (!any) { fprintf(stderr, "%s: relation \"TEXT\" NAME...\n", path); return -1; } return 1; }
    if (!strcmp(tok, "own") && LAID) { at = strtok(NULL, "\r\n"); int any = 0;      /* a name that stands only within the source: [the source's witness, NAME, the name] */
        while (name_next(&at, nm, sizeof nm)) { if (dis_add(s, path, nm, D_OWN, NULL) < 0) return -1; any = 1; }
        if (!any) { fprintf(stderr, "%s: own NAME...\n", path); return -1; } return 1; }
    if ((!strcmp(tok, "types") || !strcmp(tok, "keyed") || !strcmp(tok, "alias") || !strcmp(tok, "maps")) && LAID) { at = strtok(NULL, "\r\n");      /* the highway's (laplace highway) */
        static const char *how[] = { "", "types LIST SAY TIER", "keyed LIST TIER PATH [matching PATTERN] [as TEMPLATE]", "alias LIST TIER PATH [matching PATTERN] [as TEMPLATE] to PATH [...]", "maps TIER PATH LIST [matching PATTERN] [as TEMPLATE] to PATH LIST [...]" };
        int what = tok[0] == 't' ? 1 : tok[0] == 'k' ? 2 : tok[0] == 'a' ? 3 : 4; if (s->nhw == 32) { fprintf(stderr, "%s: more than 32 lines of the highway\n", path); return -1; }
        HwLine *x = &s->hw[s->nhw]; memset(x, 0, sizeof *x); x->what = what; x->when = s->now; char w[160];
        #define NEXT(into) (name_next(&at, w, sizeof w) && snprintf(into, sizeof into, "%s", w) >= 0)
        int ok = 1;
        if (what == 1) ok = NEXT(x->a.list) && NEXT(x->say) && NEXT(x->tier);
        else if (what == 2 || what == 3) ok = NEXT(x->a.list) && NEXT(x->tier) && NEXT(x->a.path);
        else ok = NEXT(x->tier) && NEXT(x->a.path) && NEXT(x->a.list);
        KSpec *k = &x->a; int to = 0;
        while (ok && name_next(&at, w, sizeof w)) {
            if (!strcmp(w, "matching")) { ok = name_next(&at, w, sizeof w) && !regcomp(&k->re, w, REG_EXTENDED); k->has_re = ok; }
            else if (!strcmp(w, "as")) { ok = k->nas < 4 && name_next(&at, w, sizeof w); if (ok) snprintf(k->as[k->nas++], sizeof k->as[0], "%s", w); }
            else if (!strcmp(w, "to") && what >= 3 && !to) { to = 1; k = &x->b; if (what == 3) snprintf(k->list, sizeof k->list, "%s", x->a.list); ok = NEXT(k->path) && (what == 3 || NEXT(k->list)); }
            else ok = 0; }
        #undef NEXT
        if (!ok || (what >= 3 && !to)) { fprintf(stderr, "%s: %s (a pattern must compile)\n", path, how[what]); return -1; }
        s->nhw++; return 1; }
    if (!strcmp(tok, "relate") && LAID) { char to[8], b[64]; at = strtok(NULL, "\r\n");
        if (!name_next(&at, a2, sizeof a2) || !name_next(&at, nm, sizeof nm) || !name_next(&at, to, sizeof to) || strcmp(to, "to") || !name_next(&at, b, sizeof b) || s->nrel == 16) { fprintf(stderr, "%s: relate TIER NAME to NAME\n", path); return -1; }
        Rel *x = &s->rel[s->nrel++]; memset(x, 0, sizeof *x); x->when = s->now; snprintf(x->tier, sizeof x->tier, "%s", a2); snprintf(x->rel, sizeof x->rel, "%s", nm); snprintf(x->to, sizeof x->to, "%s", b);
        char w1[8], w2[64]; while (name_next(&at, w1, sizeof w1)) { if (!strcmp(w1, "alone")) { x->alone = 1; continue; }
            if (strcmp(w1, "by") || !name_next(&at, w2, sizeof w2)) { fprintf(stderr, "%s: relate TIER NAME to NAME [alone] [by NAME]\n", path); return -1; } snprintf(x->by, sizeof x->by, "%s", w2); }
        return 1; }
    s->laid_line = 1; return layout_says(&s->lay, path, tok);
}
int say_lays(const Recipe *r){ const Say *s = r->say; return s && (s->lay.ntier > 0 || s->lay.g.n > 0); }
int say_compile(Recipe *r){
    Say *s = r->say;
    if (!s) return 1;
    compile_semantic_paths(s);
    if (!s->dis_program) s->dis_program = compile_dispositions(s->dis, s->ndis);
    if (!s->value_program) s->value_program = compile_dispositions(s->val, s->nval);
    if (s->nsplit && !s->lay.g.compiled) {
        s->lay.g.record_name_count = (uint8_t)s->nsplit;
        for (int i = 0; i < s->nsplit; i++) memcpy(s->lay.g.record_names[i].name, s->split[i], sizeof s->split[i]);
    }
    if (!s->lay.ntier && !s->lay.g.n) return 1;
    if (!layout_compile_recipe(&s->lay, r->grammar, sizeof r->grammar)) return 0;
    r->lang = grammar_lang(s->lay.g.grammar_id); return r->lang != NULL;
}
/* Whether a file this recipe lays out can be read a stretch at a time: its outermost parts are lines (1) or end at an
 * empty line (2), and nothing in a part points at another part (a key within the file). */
int say_stretches(const Recipe *r){ const Say *s = r->say; if (!s || !s->lay.ntier || s->nsplit || s->lay.tier[0].quoted || s->lay.tier[0].escaped || s->lay.tier[0].numbered[0] || s->has_about) return 0;     /* numbered: a line is counted from the file's start, read whole */
    for (int z = 0; z < s->ndis; z++) if (s->dis[z].what == D_REFER)
        for (uint8_t i = 0; i < s->dis[z].ntargets; i++) if (s->dis[z].targets[i].local) return 0;
    if (!strcmp(s->lay.tier[0].sep, "\n")) return 1; if (!strcmp(s->lay.tier[0].sep, "\n\n")) return 2; return 0; }
static int is_tier(const Say *s, const char *name){ for (int i = 0; i < s->lay.ntier; i++) if (!strcmp(s->lay.tier[i].name, name)) return 1; return 0; }
const char *say_refers(const Recipe *r, int i){ const Say *s = r->say; if (!s) return NULL;
    for (int z = 0; z < s->ndis; z++) if (s->dis[z].what == D_REFER && !is_tier(s, s->dis[z].arg) && !i--) return s->dis[z].arg;
    return NULL; }
/* Codepoints written in hex, parted by spaces (U+ before one passed over), as the text they are; the source's mark for
 * the code point itself (self, when there is one) stands for it. */
static __thread long self_cp = -1; static __thread char self_mark;
static int codepoints_text(const uint8_t *p, size_t n, uint8_t *out, size_t cap, size_t *len){
    size_t k = 0; const uint8_t *e = p + n;
    while (p < e) { while (p < e && *p == ' ') p++; if (p >= e) break; const uint8_t *t0 = p; while (p < e && *p != ' ') p++;
        size_t o = (size_t)(p - t0);
        if (o == 1 && self_mark && *t0 == (uint8_t)self_mark && self_cp >= 0) { if (k + 4 > cap) return 0; k += lp_utf8_put((uint32_t)self_cp, out + k); continue; } if (o > 2 && t0[0] == 'U' && t0[1] == '+') { t0 += 2; o -= 2; } char h[16]; if (!o || o > 8) return 0; memcpy(h, t0, o); h[o] = 0;
        char *end; unsigned long cp = strtoul(h, &end, 16); if (*end || cp >= LP_NCP || k + 4 > cap) return 0; k += lp_utf8_put((uint32_t)cp, out + k); }
    *len = k; return k > 0;
}

/* ---- one part of the outermost tier, read */
typedef struct { Ref *c; size_t n, cap; SayScratch *arena; } Refs;
/* A binding is a variable selected by a compiled name slot in a semantic
 * scope. The table stores references, not AST nodes, copied names or a second
 * parent/child tree. Multiple occurrences keep their source order. */
typedef struct { uint64_t key; uint32_t first, last; } BindingBucket;
typedef struct { uint32_t value, next; } BindingLink;
typedef struct {
    BindingBucket *buckets; BindingLink *links;
    size_t capacity, count, link_capacity, link_count;
} BindingMatrix;

/* Variables and composition operands of the compiled semantic program.
 * This is a selector binding store: it retains no native AST nodes or copied
 * packaging hierarchy, and has no first/last/next child links. */
#define BIND_INPUTS UINT32_MAX
typedef struct SemanticRecord SemanticRecord;
typedef struct TextFragment { const uint8_t *bytes; uint32_t length; struct TextFragment *next; } TextFragment;
typedef struct { TextFragment *first, *last; uint32_t length; } TextRun;
typedef struct {
    SemanticRecord *owner;
    const uint8_t *name, *val;
    uint32_t nlen, vlen, arity, extent, operand;
    int32_t parent;
    uint8_t kind, tier, join, trim, context;
    uint32_t dropped_first;
    TextRun *text;
    const Ref *product;
} SemanticValue;
typedef struct {
    const uint8_t *name;
    uint64_t hash;
    uint32_t length, parent, all, nonempty, seen, next, scope_next;
} DroppedField;
struct SemanticRecord {
    SemanticValue *n; uint32_t count, capacity;
    BindingMatrix *bindings;
    DroppedField *dropped; uint32_t dropped_count, dropped_capacity, *dropped_buckets, dropped_bucket_count;
};
static int semantic_named(const SemanticValue *value, const char *name, size_t length){
    return value->nlen == length && (!length || !memcmp(value->name, name, length));
}
static __thread const BindingMatrix *active_bindings;

static size_t binding_bucket(uint64_t key, size_t capacity){
    key ^= key >> 30; key *= UINT64_C(0xbf58476d1ce4e5b9); key ^= key >> 27;
    return (key ^ (key >> 31)) & (capacity - 1);
}
static const BindingBucket *bindings_find(const BindingMatrix *matrix, uint32_t scope, uint32_t slot){
    if (!matrix || !matrix->capacity || scope == UINT32_MAX) return NULL;
    uint64_t key = ((uint64_t)scope + 1) << 32 | slot; size_t at = binding_bucket(key, matrix->capacity);
    while (matrix->buckets[at].key) {
        if (matrix->buckets[at].key == key) return &matrix->buckets[at];
        at = (at + 1) & (matrix->capacity - 1);
    }
    return NULL;
}

static int32_t semantic_first(const SemanticValue *value){
    const SemanticRecord *record = value->owner;
    const BindingBucket *bucket = bindings_find(record->bindings, (uint32_t)(value - record->n), BIND_INPUTS);
    return bucket ? (int32_t)record->bindings->links[bucket->first - 1].value : -1;
}
static int32_t semantic_last(const SemanticValue *value){
    const SemanticRecord *record = value->owner;
    const BindingBucket *bucket = bindings_find(record->bindings, (uint32_t)(value - record->n), BIND_INPUTS);
    return bucket ? (int32_t)record->bindings->links[bucket->last - 1].value : -1;
}
static int32_t semantic_next(const SemanticValue *value){
    if (!value->operand) return -1;
    const BindingMatrix *matrix = value->owner->bindings;
    uint32_t next = matrix->links[value->operand - 1].next;
    return next ? (int32_t)matrix->links[next - 1].value : -1;
}
static int32_t semantic_slot_first(const SemanticRecord *record, int32_t scope, uint32_t slot){
    if (scope < 0) return -1;
    const BindingBucket *bucket = bindings_find(record->bindings, (uint32_t)scope, slot);
    return bucket ? (int32_t)record->bindings->links[bucket->first - 1].value : -1;
}
static int32_t semantic_child(const SemanticRecord *record, int32_t scope, const char *name, int32_t after){
    const BindingBucket *bucket = bindings_find(record->bindings, (uint32_t)scope, path_code(name)->whole.slot);
    int ready = after < 0;
    for (uint32_t link = bucket ? bucket->first : 0; link; link = record->bindings->links[link - 1].next) {
        uint32_t value = record->bindings->links[link - 1].value;
        if (ready) return (int32_t)value;
        if (value == (uint32_t)after) ready = 1;
    }
    return -1;
}

static void bindings_grow(BindingMatrix *matrix){
    size_t capacity = matrix->capacity ? matrix->capacity * 2 : 128;
    if (capacity < matrix->capacity || capacity > SIZE_MAX / sizeof *matrix->buckets) { fputs("binding index exceeds the record arena\n", stderr); abort(); }
    BindingBucket *buckets = scratch_alloc(capacity * sizeof *buckets); memset(buckets, 0, capacity * sizeof *buckets);
    for (size_t i = 0; i < matrix->capacity; i++) if (matrix->buckets[i].key) {
        size_t at = binding_bucket(matrix->buckets[i].key, capacity);
        while (buckets[at].key) at = (at + 1) & (capacity - 1);
        buckets[at] = matrix->buckets[i];
    }
    matrix->buckets = buckets; matrix->capacity = capacity;
}
static uint32_t bindings_add(BindingMatrix *matrix, uint32_t scope, uint32_t slot, uint32_t value){
    if (scope == UINT32_MAX) { fputs("too many semantic scopes in one record\n", stderr); abort(); }
    if ((matrix->count + 1) * 2 >= matrix->capacity) bindings_grow(matrix);
    uint64_t key = ((uint64_t)scope + 1) << 32 | slot; size_t at = binding_bucket(key, matrix->capacity);
    while (matrix->buckets[at].key && matrix->buckets[at].key != key) at = (at + 1) & (matrix->capacity - 1);
    if (matrix->link_count == matrix->link_capacity) {
        size_t capacity = matrix->link_capacity ? matrix->link_capacity * 2 : 128;
        if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof *matrix->links) { fputs("too many record bindings\n", stderr); abort(); }
        BindingLink *links = scratch_alloc(capacity * sizeof *links);
        if (matrix->link_count) memcpy(links, matrix->links, matrix->link_count * sizeof *links);
        matrix->links = links; matrix->link_capacity = capacity;
    }
    uint32_t link = (uint32_t)++matrix->link_count; matrix->links[link - 1] = (BindingLink){value, 0};
    BindingBucket *bucket = &matrix->buckets[at];
    if (!bucket->key) { *bucket = (BindingBucket){key, link, link}; matrix->count++; }
    else { matrix->links[bucket->last - 1].next = link; bucket->last = link; }
    return link;
}
static int bind_name_matches(BindingMatrix *matrix, const PathProgram *program, uint32_t scope, uint32_t value,
                              const uint8_t *name, uint32_t length, int note, uint64_t hash, unsigned wildcard){
    int matches = 0;
    for (unsigned is_note = 0; is_note <= (unsigned)note; is_note++) {
        uint64_t key = disposition_key(hash, 0, is_note, wildcard, 0);
        size_t bucket = (key ^ (key >> 32)) & (program->name_bucket_count - 1);
        for (uint32_t at = program->name_buckets[bucket]; at; at = program->names[at - 1].next) {
            const NamePattern *pattern = &program->names[at - 1];
            if (pattern->hash == key && pattern->name.length == length && pattern->name.wildcard == wildcard &&
                pattern->name.note == is_note && (!length || !memcmp(pattern->name.bytes, name, length))) {
                if (matrix) bindings_add(matrix, scope, at, value);
                matches++;
            }
        }
    }
    return matches;
}
static int bind_field_name(BindingMatrix *matrix, const PathProgram *program, uint32_t scope, uint32_t value,
                            const uint8_t *name, uint32_t length, int note){
    int matches = 0;
    uint64_t hash = NAME_HASH_START;
    if (program->has_positions) { if (matrix) bindings_add(matrix, scope, 0, value); matches++; }
    if (program->wildcard_lengths[0] & 1) matches += bind_name_matches(matrix, program, scope, value, name, 0, note, hash, 1);
    uint32_t limit = length < program->max_name_length ? length : program->max_name_length;
    for (uint32_t i = 0; i < limit; i++) {
        hash = (hash ^ name[i]) * NAME_HASH_PRIME; uint32_t prefix = i + 1;
        if (prefix < 192 && (program->wildcard_lengths[prefix >> 6] & (UINT64_C(1) << (prefix & 63))))
            matches += bind_name_matches(matrix, program, scope, value, name, prefix, note, hash, 1);
    }
    if (length <= program->max_name_length) matches += bind_name_matches(matrix, program, scope, value, name, length, note, hash, 0);
    return matches;
}

enum { DIAG_ATTEST, DIAG_HOLDS, DIAG_RELATE, DIAG_LINE_PREDICATE, DIAG_LINE, DIAG_NO_LIST, DIAG_SELF_VALUE, DIAG_UNRESOLVED_KEY };
static const DiagnosticLabel diagnostic_labels[] = {
    {"attest", 6, UINT64_C(0x907bd39a08f34aac)},
    {"holds", 5, UINT64_C(0x7477287ad37c50f1)},
    {"relate", 6, UINT64_C(0xb279534d85fc01cc)},
    {"line :: predicate", 17, UINT64_C(0xb638b662bd815814)},
    {"line", 4, UINT64_C(0xbf4ba5ad694f5907)},
    {"no list in the highway:", 23, UINT64_C(0x9c679eb7e39af651)},
    {"value equal to its thing:", 25, UINT64_C(0x50c83810e5074575)},
    {"a key no list of the highway holds, left out of the path:", 57, UINT64_C(0xb5c022f381bb9b49)}
};
typedef struct {
    const DiagnosticLabel *label;
    uint64_t hash, count;
    uint8_t a[48], b[40], a_length, b_length, has_b;
    char what;
} Obligation;
typedef struct { const uint8_t *bytes; uint64_t hash; uint32_t length; int32_t node; uint16_t tier; } LocalKey;
static void push(Refs *a, const Ref *x){
    if (!a->arena) { fputs("output references have no owning arena\n", stderr); abort(); }
    if (a->n == a->cap) {
        size_t capacity = a->cap ? a->cap * 2 : 256;
        if (capacity < a->cap || capacity > SIZE_MAX / sizeof(Ref)) { fputs("output references exceed the arena\n", stderr); abort(); }
        Ref *refs = arena_alloc(a->arena, capacity * sizeof *refs);
        if (a->n) memcpy(refs, a->c, a->n * sizeof *refs);
        a->c = refs; a->cap = capacity;
    }
    a->c[a->n++] = *x;
}
typedef struct { const Recipe *r; const Say *s; Events ev; Refs things, meta; uint64_t open[256]; float er, ed, score; uint64_t ordinal; Ref fw, voice; int voiced; char opennm[16][64]; uint8_t open_length[16]; int nopen; uint64_t unknown; char unknm[8][96]; int nunk; Refs grp; Ref fname, fstem;
                 const SemanticRecord *ix_tree; uint32_t ix_count; LocalKey *local_keys; size_t local_capacity, local_count;
                 BindingMatrix bindings;
                 SayScratch output;
                 const SemanticRecord *tc_tree; uint32_t tc_n; Ref *tc; uint8_t *ts, *uk;
                 long selfcp;                                                /* the code point the part being read is, or -1 */
                 Ref fabout; int has_fabout;                                 /* what the page is about, where its about line names it */
                 Hw *hw; struct HwRec *hr; size_t nhr, chr; int worker; int32_t wide_of; KeyRank kr; int16_t *dm;
                 struct CSeen *cs; uint64_t cs_cap, cs_n, cs_epoch; int32_t rec;
                 Obligation obl[256]; uint16_t obligation_buckets[512]; int nobl; uint64_t obl_more;
                 uint8_t *lo; const SemanticRecord *lo_tree;     /* the parts of the record already left open, each counted once */
                 } Sink;   /* obl: the obligations the reading left (no meaning: no claim), and values outside the road class named, by name */        /* cs: the claims the part being read has made, each once (claim) */          /* reading for the highway (laplace highway): what the part says of its types, in order */
static void sink_buffers(Sink *sink){ sink->things.arena = sink->meta.arena = sink->grp.arena = &sink->output; }
static void sink_event(Sink *sink, const Event *event){
    Events *events = &sink->ev;
    if (events->n == events->cap) {
        uint64_t capacity = events->cap ? events->cap * 2 : 1024;
        if (capacity < events->cap || capacity > SIZE_MAX / sizeof(Event)) { fputs("events exceed the output arena\n", stderr); abort(); }
        Event *data = arena_alloc(&sink->output, (size_t)capacity * sizeof *data);
        if (events->n) memcpy(data, events->e, (size_t)events->n * sizeof *data);
        events->e = data; events->cap = capacity;
    }
    events->e[events->n++] = *event;
}
static void release_sinks(Sink *sinks, size_t count){
    for (size_t i = 0; i < count; i++) arena_release(&sinks[i].output);
    free(sinks);
}
static char *output_text(Sink *sink, const char *bytes, size_t length){
    if (!bytes) return NULL;
    if (length == SIZE_MAX) { fputs("output text exceeds the arena\n", stderr); abort(); }
    char *text = arena_alloc(&sink->output, length + 1); memcpy(text, bytes, length); text[length] = 0; return text;
}
typedef struct HwRec { int what, line; Ref thing; char *a, *b; } HwRec;     /* what: 1 a type, 2 a key of one, 3 a key naming what another names, 4 an edge */
static void hw_rec(Sink *k, int what, int line, const Ref *thing, const char *a, size_t al, const char *b, size_t bl){
    if (k->nhr == k->chr) {
        size_t capacity = k->chr ? k->chr * 2 : 1024;
        if (capacity < k->chr || capacity > SIZE_MAX / sizeof(HwRec)) { fputs("highway records exceed the arena\n", stderr); abort(); }
        HwRec *records = arena_alloc(&k->output, capacity * sizeof *records);
        if (k->nhr) memcpy(records, k->hr, k->nhr * sizeof *records);
        k->hr = records; k->chr = capacity;
    }
    HwRec *x = &k->hr[k->nhr++]; x->what = what; x->line = line; if (thing) x->thing = *thing; else memset(&x->thing, 0, sizeof x->thing);
    x->a = output_text(k, a, al); x->b = output_text(k, b, bl);
}                   /* the things of the tree being read, each composed once */   /* fname: the file's name; fstem: its name up to its first dot */   /* grp: the claims the part being read has made */   /* fw: the file's witness; voice: who says the part being read, when the file names one; opennm: the names left open */
/* A named part the recipe gives no disposition: counted, and its name kept to be said. */

static void open_name(Sink *sink, const uint8_t *name, uint32_t length, uint64_t count){
    if (!count) return;
    sink->open[0] += count;
    for (int i = 0; i < sink->nopen; i++) if (sink->open_length[i] == length && (!length || !memcmp(sink->opennm[i], name, length))) return;
    if (sink->nopen < 16) {
        size_t n = length < 63 ? length : 63; if (n) memcpy(sink->opennm[sink->nopen], name, n);
        sink->opennm[sink->nopen][n] = 0; sink->open_length[sink->nopen++] = (uint8_t)n;
    }
}
static void left_open(Sink *sink, const SemanticValue *value){
    const SemanticRecord *record = sink->lo_tree;
    if (record && sink->lo && value >= record->n && value < record->n + record->count) {
        size_t index = (size_t)(value - record->n); if (sink->lo[index]) return; sink->lo[index] = 1;
    }
    open_name(sink, value->name, value->nlen, 1);
}
static void dropped_open(Sink *sink, const SemanticRecord *record, uint32_t scope, int all){
    for (uint32_t at = record->n[scope].dropped_first; at; at = record->dropped[at - 1].scope_next) {
        DroppedField *field = &record->dropped[at - 1]; uint32_t count = all ? field->all : field->nonempty;
        if (count > field->seen) { open_name(sink, field->name, field->length, count - field->seen); field->seen = count; }
    }
}

/* A named value of a part the recipe speaks of that it neither disposes of nor writes the name of anywhere (a field of a
 * counted group no line reads, a column no line names): left open, counted and said like a part with no disposition,
 * never read past in silence. The name's answer is kept for the parts after. */
static int unmentioned(const Say *s, const SemanticValue *x){
    if (!s->said || !x->nlen || x->nlen > 62) return 0;
    return !mentioned(s->paths, x->name, x->nlen);
}
/* An unresolved obligation (what 'o'): a part a claim would name that the recipe gives no meaning (value), so no claim is
 * made of it; or (what 'm') a value of a road class the highway holds that its list does not, said as written. Counted by
 * the directive and the part's name, and said when the ingest ends (say_obligations). */
typedef struct {
    const DiagnosticLabel *label;
    const uint8_t *a, *b;
    uint8_t a_length, b_length, has_b;
} ObligationView;
static unsigned diagnostic_length(const ObligationView *view){
    return view->label->length + 1u + view->a_length + (view->has_b ? 1u + view->b_length : 0u);
}
static uint8_t diagnostic_byte(const ObligationView *view, unsigned position){
    if (position < view->label->length) return (uint8_t)view->label->text[position];
    position -= view->label->length;
    if (!position--) return ' ';
    if (position < view->a_length) return view->a[position];
    position -= view->a_length;
    if (!position--) return '.';
    return view->b[position];
}
static int obligation_equal(const Obligation *entry, const ObligationView *view){
    if (entry->label->length == view->label->length && entry->a_length == view->a_length &&
        entry->b_length == view->b_length && entry->has_b == view->has_b &&
        (entry->label == view->label || !memcmp(entry->label->text, view->label->text, view->label->length)) &&
        (!view->a_length || !memcmp(entry->a, view->a, view->a_length)) &&
        (!view->b_length || !memcmp(entry->b, view->b, view->b_length))) return 1;
    ObligationView stored = {entry->label, entry->a, entry->b, entry->a_length, entry->b_length, entry->has_b};
    unsigned length = diagnostic_length(view);
    if (length != diagnostic_length(&stored)) return 0;
    for (unsigned i = 0; i < length; i++) if (diagnostic_byte(view, i) != diagnostic_byte(&stored, i)) return 0;
    return 1;
}
static void add_obligation(Sink *sink, char what, const DiagnosticLabel *label, const uint8_t *a, size_t a_length,
                           const uint8_t *b, size_t b_length, uint64_t count){
    ObligationView view = {label, a, b, (uint8_t)(a_length < 48 ? a_length : 48),
                           (uint8_t)(b ? (b_length < 40 ? b_length : 40) : 0), (uint8_t)(b != NULL)};
    uint64_t hash = (label->hash ^ ' ') * NAME_HASH_PRIME;
    for (unsigned i = 0; i < view.a_length; i++) hash = (hash ^ a[i]) * NAME_HASH_PRIME;
    if (view.has_b) {
        hash = (hash ^ '.') * NAME_HASH_PRIME;
        for (unsigned i = 0; i < view.b_length; i++) hash = (hash ^ b[i]) * NAME_HASH_PRIME;
    }
    hash ^= (uint64_t)(unsigned char)what * UINT64_C(0x9e3779b97f4a7c15);
    size_t bucket = (hash ^ (hash >> 32)) & 511u;
    while (sink->obligation_buckets[bucket]) {
        Obligation *entry = &sink->obl[sink->obligation_buckets[bucket] - 1];
        if (entry->hash == hash && entry->what == what && obligation_equal(entry, &view)) { entry->count += count; return; }
        bucket = (bucket + 1) & 511u;
    }
    if (sink->nobl == 256) { sink->obl_more += count; return; }
    Obligation *entry = &sink->obl[sink->nobl]; *entry = (Obligation){.label=label, .hash=hash, .count=count,
        .a_length=view.a_length, .b_length=view.b_length, .has_b=view.has_b, .what=what};
    if (view.a_length) memcpy(entry->a, a, view.a_length);
    if (view.b_length) memcpy(entry->b, b, view.b_length);
    sink->obligation_buckets[bucket] = (uint16_t)++sink->nobl;
}
static void note_obligation(Sink *sink, char what, const DiagnosticLabel *label, const uint8_t *a, size_t a_length,
                            const uint8_t *b, size_t b_length){
    add_obligation(sink, what, label, a, a_length, b, b_length, 1);
}
static void note_node(Sink *sink, const DiagnosticLabel *label, const SemanticValue *node){
    note_obligation(sink, 'o', label, node->name, node->nlen, NULL, 0);
}
static void format_obligation(const Obligation *entry, char text[160]){
    size_t used = entry->label->length;
    memcpy(text, entry->label->text, used); text[used++] = ' ';
    if (entry->a_length) { memcpy(text + used, entry->a, entry->a_length); used += entry->a_length; }
    if (entry->has_b) { text[used++] = '.'; if (entry->b_length) { memcpy(text + used, entry->b, entry->b_length); used += entry->b_length; } }
    text[used] = 0;
}
/* A value said as a value of a road class: where the highway holds the class's list and the list does not hold the value,
 * it is said (the claim stands: the value is what the source writes; only its slot is missing). */
static void of_class(Sink *k, const Dis *m, const Ref *v){
    if (!m->arg[0] || !HW) return; const lp_list *L = lp_highway_list(HW, m->arg);
    if (!L) { note_obligation(k, 'm', &diagnostic_labels[DIAG_NO_LIST], (const uint8_t *)m->arg, m->arg_length, NULL, 0); return; }
    if (lp_highway_slot(HW, L, &v->id) < 0) note_obligation(k, 'm', m->road_label, (const uint8_t *)m->name, m->name_length, NULL, 0);
}
static void claim(Sink *k, Ref *part, int n);
/* A part's value said of a thing as the recipe says the part means it (value LIST NAME): a value of that road class,
 * [thing, value]. The part's name is in no claim. */
/* A claim made of a part under the meaning the recipe gave it, counted by that meaning (say_obligations says them). */
static void said_count(Sink *k, const Dis *m){
    note_obligation(k, 's', m->statement_label, (const uint8_t *)m->name, m->name_length, NULL, 0); }
static void said_as(Sink *k, const Dis *m, Ref S, Ref v){ if (m->what != 1 && !memcmp(&v.id, &S.id, 16)) { note_obligation(k, 'o', &diagnostic_labels[DIAG_SELF_VALUE], (const uint8_t *)m->name, m->name_length, NULL, 0); return; } said_count(k, m);     /* a relation to itself is a statement (fra's 639-2/T code is fra); a value that is the thing itself, [x, x], is a run, no claim: counted */
    if (m->what == 1) { Ref p[3] = { S, string_ref((const uint8_t *)m->text, m->text_length), v }; claim(k, p, 3); return; }     /* relation "TEXT": [thing, TEXT, value] */
    of_class(k, m, &v); Ref p[2] = { S, v }; claim(k, p, 2); }     /* a value that is the thing itself (a lemma its own form) says nothing of it */
/* The obligations of every file read, by recipe: kept until said. */
static struct { char recipe[64], name[160]; uint64_t n; char what; } *OBL; static size_t NOBL, COBL; static uint64_t OBL_MORE;
static void keep_obligations(const char *recipe, const Sink *k){
    #pragma omp critical(laplace_obligations)
    { OBL_MORE += k->obl_more;
      for (int i = 0; i < k->nobl; i++) { size_t j = 0; char name[160]; format_obligation(&k->obl[i], name);
          for (; j < NOBL; j++) if (OBL[j].what == k->obl[i].what && !strcmp(OBL[j].recipe, recipe) && !strcmp(OBL[j].name, name)) break;
          if (j == NOBL) { if (NOBL == COBL) { COBL = COBL ? COBL * 2 : 64; OBL = xrealloc(OBL, sizeof *OBL * COBL); }
              snprintf(OBL[j].recipe, sizeof OBL[j].recipe, "%s", recipe); snprintf(OBL[j].name, sizeof OBL[j].name, "%s", name); OBL[j].n = 0; OBL[j].what = k->obl[i].what; NOBL++; }
          OBL[j].n += k->obl[i].count; } }
}
static int obl_cmp(const void *x, const void *y){ const char *a = (const char *)x, *b = (const char *)y; int c = strcmp(a, b); return c ? c : strcmp(a + 64, b + 64); }
void say_obligations(FILE *o){
    uint64_t n = OBL_MORE, m = 0, sd = 0; for (size_t i = 0; i < NOBL; i++) { if (OBL[i].what == 'o') n += OBL[i].n; else if (OBL[i].what == 'm') m += OBL[i].n; else sd += OBL[i].n; }
    if (!NOBL && !OBL_MORE) return;
    qsort(OBL, NOBL, sizeof *OBL, obl_cmp);
    if (sd) { fprintf(o, "\n== statements, by the meaning the recipe gives each part (value LIST NAME, relation \"TEXT\" NAME): %'llu\n", (unsigned long long)sd);
        for (size_t i = 0; i < NOBL; i++) if (OBL[i].what == 's') fprintf(o, "   %-22s %-80s %'14llu\n", OBL[i].recipe, OBL[i].name, (unsigned long long)OBL[i].n); }
    fprintf(o, "\n== unresolved obligations: %'llu parts a claim would name have no meaning the recipe gives, so no claim is made of them (Laplace-Engine#32)\n", (unsigned long long)n);
    for (size_t i = 0; i < NOBL; i++) if (OBL[i].what == 'o') fprintf(o, "   %-22s %-80s %'14llu\n", OBL[i].recipe, OBL[i].name, (unsigned long long)OBL[i].n);
    if (OBL_MORE) fprintf(o, "   of them %'llu under names past the 256 a part keeps\n", (unsigned long long)OBL_MORE);
    if (m) { fprintf(o, "   values said as a road class's that the highway's list does not hold (claimed as written; no slot): %'llu\n", (unsigned long long)m);
        for (size_t i = 0; i < NOBL; i++) if (OBL[i].what == 'm') fprintf(o, "   %-22s %-80s %'14llu\n", OBL[i].recipe, OBL[i].name, (unsigned long long)OBL[i].n); }
    free(OBL); OBL = NULL; NOBL = COBL = 0; OBL_MORE = 0;
}
/* A name as the recipe writes it: NAME* is every name that begins with NAME (a header's columns that begin alike). */
static int name_matches(const SemanticValue *node, const NameCode *name){
    if (name->note && node->kind != S_NOTE) return 0;
    if (name->wildcard ? node->nlen < name->length : node->nlen != name->length) return 0;
    return !name->length || !memcmp(node->name, name->bytes, name->length);
}
static int named_as(const SemanticValue *x, const char *name){
    const NameCode *code = &path_code(name)->whole;
    return name_matches(x, code->slot ? &active_say->paths->names[code->slot - 1].name : code);
}

static Ref text_of(const uint8_t *bytes, size_t length);
static Ref semantic_text_ref(const SemanticValue *value){
    if (!value->product) {
        Ref *product = scratch_alloc(sizeof *product); *product = text_of(value->val, value->vlen);
        ((SemanticValue *)value)->product = product;
    }
    return *value->product;
}

/* The tree being read, for a name scoped to what holds it (ELEMENT.NAME: that part of that element only). */
static __thread const SemanticRecord *TT;
#define BUSY_MAX 256
static __thread int32_t busy[BUSY_MAX]; static __thread int nbusy, busy_base;     /* the things this thread is composing, innermost last; from busy_base, the reading it is in now */
static inline int composing(int32_t g){ for (int i = busy_base; i < nbusy; i++) if (busy[i] == g) return 1; return 0; }
/* named_as, for a name split when the recipe was read */
static int spoken_of(const Say *s, const SemanticRecord *t, int32_t root, uint32_t which);
static void disposition_candidates(const DisProgram *program, uint64_t hash, uint64_t parent, unsigned note,
                                   unsigned wildcard, unsigned qualified, uint64_t candidates[2][4]){
    uint64_t key = disposition_key(hash, parent, note, wildcard, qualified);
    size_t bucket = (key ^ (key >> 32)) & (program->capacity - 1);
    for (uint16_t at = program->buckets[bucket]; at; at = program->entries[at - 1].next) {
        const DisEntry *entry = &program->entries[at - 1];
        if (entry->key == key && entry->qualified == qualified)
            candidates[qualified][entry->rule >> 6] |= UINT64_C(1) << (entry->rule & 63);
    }
}
static void disposition_lookup(const DisProgram *program, const SemanticValue *node, uint64_t parent_hash, int has_parent,
                               uint64_t hash, unsigned wildcard, uint64_t candidates[2][4]){
    for (unsigned note = 0; note <= (unsigned)(node->kind == S_NOTE); note++) {
        disposition_candidates(program, hash, 0, note, wildcard, 0, candidates);
        if (has_parent) disposition_candidates(program, hash, parent_hash, note, wildcard, 1, candidates);
    }
}
static const Dis *dispatch_dis(const Say *say, const Dis *rules, int count, const SemanticValue *node, int resolve_conditions){
    if (!count) return NULL;
    const DisProgram *program = rules == say->dis ? say->dis_program : say->value_program;
    if (!program) { fputs("dispositions were not compiled at recipe load\n", stderr); abort(); }
    int32_t root = TT && node >= TT->n && node < TT->n + TT->count && node->parent >= 0 ? node->parent : 0;
    const SemanticValue *parent = NULL;
    if (TT && node->parent >= 0) {
        parent = &TT->n[node->parent];
        if (parent->nlen == node->nlen && (!node->nlen || !memcmp(parent->name, node->name, node->nlen)) && parent->parent >= 0) {
            parent = &TT->n[parent->parent]; root = (int32_t)(parent - TT->n);
        }
        if (parent->nlen > 63) parent = NULL;
    }
    uint64_t candidates[2][4] = {{0}}, hash = NAME_HASH_START;
    uint64_t parent_hash = parent ? selector_hash(parent->name, parent->nlen) : 0;
    if (program->wildcard_lengths & 1) disposition_lookup(program, node, parent_hash, parent != NULL, hash, 1, candidates);
    uint32_t length = node->nlen < 63 ? node->nlen : 63;
    for (uint32_t i = 0; i < length; i++) {
        hash = (hash ^ node->name[i]) * NAME_HASH_PRIME;
        if (program->wildcard_lengths & (UINT64_C(1) << (i + 1))) disposition_lookup(program, node, parent_hash, parent != NULL, hash, 1, candidates);
    }
    if (node->nlen <= 63) disposition_lookup(program, node, parent_hash, parent != NULL, hash, 0, candidates);
    for (int qualified = 1; qualified >= 0; qualified--) for (unsigned word = 0; word < 4; word++) {
        uint64_t bits = candidates[qualified][word];
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll(bits), index = word * 64 + bit; bits &= bits - 1;
            const Dis *rule = &rules[index];
            if (qualified) {
                if (!parent || !rule->parent_slot || !name_matches(parent, &say->paths->names[rule->parent_slot - 1].name) ||
                    !name_matches(node, &say->paths->names[rule->part_slot - 1].name)) continue;
            } else if (!name_matches(node, &say->paths->names[rule->name_slot - 1].name)) continue;
            if (rule->when) {
                if (!resolve_conditions) return NULL;
                if (!TT || !spoken_of(say, TT, root, rule->when)) continue;
            }
            return rule;
        }
    }
    return NULL;
}
static const Dis *dis_in(const Say *say, const Dis *rules, int count, const SemanticValue *node){ return dispatch_dis(say, rules, count, node, 1); }
static const Dis *match_dis(const Say *s, const SemanticValue *x){ return dis_in(s, s->dis, s->ndis, x); }
/* What a part means in a claim (value LIST NAME): NULL where the recipe says nothing of it, and no claim is made of it. */
static const Dis *meaning(const Say *s, const SemanticValue *x){ return s->nval && x->nlen ? dis_in(s, s->val, s->nval, x) : NULL; }
/* A part's disposition, found once for each node of the tree being read (DM, shared by the threads reading it). */
static __thread int16_t *DM;
static const Dis *dis_of(const Say *s, const SemanticValue *x){
    if (!DM || !TT || x < TT->n || x >= TT->n + TT->count) return match_dis(s, x);
    size_t i = (size_t)(x - TT->n); int16_t m = __atomic_load_n(&DM[i], __ATOMIC_RELAXED); if (m) return m > 0 ? &s->dis[m - 1] : NULL;
    const Dis *d = match_dis(s, x); __atomic_store_n(&DM[i], (int16_t)(d ? d - s->dis + 1 : -1), __ATOMIC_RELAXED); return d;
}
/* The selector VM consumes only instructions compiled when the recipe loaded.
 * No node or row splits a path string, scans a numeric position, or assembles a
 * dotted-path fallback at runtime. */
static int32_t path_first(const SemanticRecord *tree, int32_t root, const PathStep *step, const PathStep *stop);
static int path_filter(const SemanticRecord *tree, int32_t node, const PathStep *step){
    if (!step->filter) return 1;
    int32_t value = path_first(tree, node, step->filter, NULL);
    if (value >= 0 && tree->n[value].kind == S_GROUP && semantic_first(&tree->n[value]) >= 0 &&
        semantic_next(&tree->n[semantic_first(&tree->n[value])]) < 0) value = semantic_first(&tree->n[value]);
    return value >= 0 && tree->n[value].vlen == step->filter_length &&
           (!step->filter_length || !memcmp(tree->n[value].val, step->filter_value, step->filter_length));
}
static int32_t path_single(const SemanticRecord *tree, int32_t root, const PathStep *step){
    if (step->operation == PATH_SELF) return root;
    if (step->operation == PATH_PARENT) return tree->n[root].parent;
    if (step->operation == PATH_ANCESTOR) {
        for (int32_t up = tree->n[root].parent; up >= 0; up = tree->n[up].parent)
            if (name_matches(&tree->n[up], &step->name)) return up;
        return -1;
    }
    if (!active_bindings) { fputs("semantic bindings were not prepared before selector execution\n", stderr); abort(); }
    const BindingBucket *bucket = bindings_find(active_bindings, (uint32_t)root, step->name.slot);
    for (uint32_t link = bucket ? bucket->first : 0; link; link = active_bindings->links[link - 1].next) {
        int32_t child = (int32_t)active_bindings->links[link - 1].value;
        if (path_filter(tree, child, step)) return child;
    }
    if (step->operation == PATH_POSITION && step->position) {
        uint32_t position = step->position; bucket = bindings_find(active_bindings, (uint32_t)root, 0);
        for (uint32_t link = bucket ? bucket->first : 0; link; link = active_bindings->links[link - 1].next)
            if (!--position) return (int32_t)active_bindings->links[link - 1].value;
    }
    return -1;
}
static int32_t path_first(const SemanticRecord *tree, int32_t root, const PathStep *step, const PathStep *stop){
    while (root >= 0 && step && step != stop) {
        int32_t next = path_single(tree, root, step);
        if (next < 0 && step->alternative) return path_first(tree, root, step->alternative, stop);
        root = next; step = step->next;
    }
    return root;
}
static int path_all(const SemanticRecord *tree, int32_t root, const PathStep *step, const PathStep *stop, int32_t *out, int capacity){
    if (root < 0 || capacity <= 0) return 0;
    if (!step || step == stop) { out[0] = root; return 1; }
    if (step->operation == PATH_PARENT || step->operation == PATH_ANCESTOR || step->operation == PATH_SELF)
        return path_all(tree, path_single(tree, root, step), step->next, stop, out, capacity);
    int count = 0, named = 0;
    if (!active_bindings) { fputs("semantic bindings were not prepared before path enumeration\n", stderr); abort(); }
    const BindingBucket *bucket = bindings_find(active_bindings, (uint32_t)root, step->name.slot);
    for (uint32_t link = bucket ? bucket->first : 0; link && count < capacity; link = active_bindings->links[link - 1].next) {
        int32_t child = (int32_t)active_bindings->links[link - 1].value;
        if (!path_filter(tree, child, step)) continue;
        named = 1; count += path_all(tree, child, step->next, stop, out + count, capacity - count);
    }
    if (!named && step->operation == PATH_POSITION)
        count = path_all(tree, path_single(tree, root, step), step->next, stop, out, capacity);
    if (!count && step->alternative) return path_all(tree, root, step->alternative, stop, out, capacity);
    return count;
}
static int32_t resolve_node_path(const SemanticRecord *tree, int32_t root, const char *name){
    return path_first(tree, root, path_code(name)->first, NULL);
}
static int all_path(const SemanticRecord *tree, int32_t root, const char *name, int32_t *out, int capacity){
    return path_all(tree, root, path_code(name)->first, NULL, out, capacity);
}
/* Record-local keys use compiled namespace IDs and original field spans.
 * The immutable table is built before temporary reducer frames are allocated;
 * parallel readers share it without a copied key buffer or per-row heap map. */
static uint64_t local_key_hash(uint16_t tier, const uint8_t *bytes, size_t length){
    return selector_hash(bytes, length) ^ ((uint64_t)tier * UINT64_C(0x9e3779b97f4a7c15));
}
static void local_key_place(LocalKey *entries, size_t capacity, LocalKey entry){
    size_t at = (entry.hash ^ (entry.hash >> 32)) & (capacity - 1);
    while (entries[at].tier) at = (at + 1) & (capacity - 1);
    entries[at] = entry;
}
static void local_key_grow(Sink *sink){
    size_t capacity = sink->local_capacity ? sink->local_capacity * 2 : 64;
    if (capacity < sink->local_capacity || capacity > SIZE_MAX / sizeof(LocalKey)) { fputs("record key index is too large\n", stderr); abort(); }
    LocalKey *entries = scratch_alloc(capacity * sizeof *entries); memset(entries, 0, capacity * sizeof *entries);
    for (size_t i = 0; i < sink->local_capacity; i++) if (sink->local_keys[i].tier) local_key_place(entries, capacity, sink->local_keys[i]);
    sink->local_keys = entries; sink->local_capacity = capacity;
}
static void index_keys(Sink *sink, const SemanticRecord *tree){
    const Say *say = sink->s;
    sink->ix_tree = tree; sink->ix_count = tree->count;
    sink->local_keys = NULL; sink->local_capacity = sink->local_count = 0;
    if (!say->nkey) return;
    for (uint32_t group = 0; group < tree->count; group++) {
        const SemanticValue *node = &tree->n[group]; if (node->context || node->kind != S_GROUP || !node->nlen) continue;
        for (int i = 0; i < say->nkey; i++) {
            const Pair *key = &say->key[i]; if (!semantic_named(node, key->tier, key->tier_length)) continue;
            int32_t field = resolve_node_path(tree, (int32_t)group, key->name);
            if (field < 0 || !tree->n[field].vlen) continue;
            const SemanticValue *value = &tree->n[field];
            if ((sink->local_count + 1) * 2 >= sink->local_capacity) local_key_grow(sink);
            uint64_t hash = local_key_hash(key->tier_id, value->val, value->vlen);
            size_t at = (hash ^ (hash >> 32)) & (sink->local_capacity - 1);
            while (sink->local_keys[at].tier) {
                const LocalKey *entry = &sink->local_keys[at];
                if (entry->hash == hash && entry->tier == key->tier_id && entry->length == value->vlen &&
                    !memcmp(entry->bytes, value->val, value->vlen)) break;
                at = (at + 1) & (sink->local_capacity - 1);
            }
            if (!sink->local_keys[at].tier) {
                sink->local_keys[at] = (LocalKey){value->val, hash, value->vlen, (int32_t)group, key->tier_id}; sink->local_count++;
            }
        }
    }
}
static int32_t keyed(Sink *sink, const SemanticRecord *tree, const TargetCode *target, const uint8_t *bytes, size_t length){
    if (sink->ix_tree != tree || sink->ix_count != tree->count) { fputs("record key index was not prepared before reduction\n", stderr); abort(); }
    if (!sink->local_capacity || !target->tier_id || length > UINT32_MAX) return -1;
    uint64_t hash = local_key_hash(target->tier_id, bytes, length);
    size_t at = (hash ^ (hash >> 32)) & (sink->local_capacity - 1);
    while (sink->local_keys[at].tier) {
        const LocalKey *entry = &sink->local_keys[at];
        if (entry->hash == hash && entry->tier == target->tier_id && entry->length == length &&
            (!length || !memcmp(entry->bytes, bytes, length))) return entry->node;
        at = (at + 1) & (sink->local_capacity - 1);
    }
    return -1;
}
static int left_empty(const Say *s, const SemanticValue *x){ return s_empty(&s->lay, x->val, x->vlen); }
/* A key no list (or no target) holds, counted once for the node it is written in, however many of the recipe's lines read
 * it (a type line and a maps line read the same part): the tree's own, shared by the threads reading it. */
/* What a reading has counted of unknown keys, and the once-flags of a part and the text it holds: a reading that is
 * not this part's own (a sense reading its synset's ili) puts both back, so the part's own reading counts the key. */
typedef struct { uint64_t unknown; int nunk; int32_t at[2]; uint8_t had[2]; } UnkMark;
static void unknown_mark(Sink *k, const SemanticRecord *t, int32_t c, UnkMark *m){
    m->unknown = k->unknown; m->nunk = k->nunk; m->at[0] = c; m->at[1] = c >= 0 && t->n[c].kind == S_GROUP ? semantic_first(&t->n[c]) : -1;
    for (int i = 0; i < 2; i++) m->had[i] = k->tc_tree == t && k->uk && m->at[i] >= 0 && (uint32_t)m->at[i] < k->tc_n ? __atomic_load_n(&k->uk[m->at[i]], __ATOMIC_ACQUIRE) : 1; }
static void unknown_back(Sink *k, const SemanticRecord *t, const UnkMark *m){
    k->unknown = m->unknown; k->nunk = m->nunk;
    for (int i = 0; i < 2; i++) if (!m->had[i] && k->tc_tree == t && k->uk && m->at[i] >= 0 && (uint32_t)m->at[i] < k->tc_n) __atomic_store_n(&k->uk[m->at[i]], 0, __ATOMIC_RELEASE); }
static int once_for(Sink *k, const SemanticRecord *t, int32_t c){ return !(k->tc_tree == t && k->uk && (uint32_t)c < k->tc_n && __atomic_exchange_n(&k->uk[c], 1, __ATOMIC_ACQ_REL)); }
static void unknown_key(Sink *k, const SemanticRecord *t, int32_t c, const SemanticValue *x){
    if (!once_for(k, t, c)) return;
    k->unknown++;
    if (k->nunk < 8) {
        char *dst = k->unknm[k->nunk++];
        size_t nl = x->nlen < 40 ? x->nlen : 40, vl = x->vlen < 48 ? x->vlen : 48;
        memcpy(dst, x->name, nl); dst[nl] = ' '; memcpy(dst + nl + 1, x->val, vl); dst[nl + 1 + vl] = 0;
    }
}
static Ref text_of(const uint8_t *p, size_t n){ return n > 256 ? text_ref(CTX[omp_get_thread_num()], p, n) : string_ref(p, n); }
static int thing_of(Sink *k, const SemanticRecord *t, int32_t g, Ref *out, int depth);
static int thing_compose(Sink *k, const SemanticRecord *t, int32_t g, Ref *out, int depth);
/* What a part is, by its disposition: content, the type it is a key of, the thing it is a key of; or nothing. */
typedef struct { char k[16][160]; int n; } Keys;
static void key_form(const KSpec *ks, const uint8_t *v, size_t n, Keys *out, const SemanticRecord *t, int32_t at);
static size_t hex_upper(unsigned long cp, char *out){
    static const char hd[] = "0123456789ABCDEF";
    char tmp[16]; int k = 0;
    do { tmp[k++] = hd[cp & 15]; cp >>= 4; } while (cp > 0);
    while (k < 4) tmp[k++] = '0';
    for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    return (size_t)k;
}
static int value_of(Sink *k, const SemanticRecord *t, int32_t c, Ref *out, int depth){
    const SemanticValue *x = &t->n[c]; const Say *s = k->s; const Dis *d = dis_of(s, x);
    if (x->kind == S_GROUP && x->join == S_PIECES && semantic_first(x) >= 0 && semantic_next(&t->n[semantic_first(x)]) >= 0) {      /* a list of values: the tuple of what each is ([display text, target]) */
        Ref pc[64]; uint32_t np = 0; for (int32_t q = semantic_first(x); q >= 0 && np < 64; q = semantic_next(&t->n[q])) { if (t->n[q].kind == S_GROUP) return 0; if (value_of(k, t, q, &pc[np], depth)) { pc[np].said = 0; np++; } }
        if (!np) return 0; *out = np == 1 ? pc[0] : said_tuple(compose(pc, np, ref_above(pc, np))); return 1; }
    if (x->kind == S_GROUP && depth < 16) {                                    /* a part that is a thing: that thing */
        if (k->tc_tree == t && (uint32_t)c < k->tc_n && !composing(c)) { Ref v; if (thing_of(k, t, c, &v, depth + 1)) { *out = v; return 1; } }
        int32_t only = semantic_first(x); if (only >= 0 && semantic_next(&t->n[only]) < 0 && t->n[only].kind == S_GROUP) return value_of(k, t, only, out, depth + 1); }     /* holding one part: what that part is */
    if (x->kind == S_GROUP) {                                                 /* a part that holds only its text: that text, as the part is disposed of */
        int32_t only = semantic_first(x); if (only < 0 || semantic_next(&t->n[only]) >= 0 || t->n[only].kind != S_TEXT) return 0;
        x = &t->n[only];
        if (!d) { if (left_empty(s, x)) return 0; *out = text_of(x->val, x->vlen); return 1; }
        c = only; }
    if (left_empty(s, x)) return 0;
    if (!d) { if (x->kind == S_TEXT) { *out = text_of(x->val, x->vlen); return 1; } if (x->nlen) left_open(k, x); return 0; }
    if (d->what == D_CONTENT) {
        if (s->selfmark && k->selfcp >= 0 && memchr(x->val, s->selfmark, x->vlen)) {    /* the code point itself, written out where the source writes its mark */
            char hex[16]; size_t hl = hex_upper((unsigned long)k->selfcp, hex);
            size_t mark = scratch_mark();
            uint8_t *o = scratch_alloc((size_t)x->vlen * 8 + 8); size_t j = 0;
            for (uint32_t i = 0; i < x->vlen; i++) { if (x->val[i] == (uint8_t)s->selfmark) { memcpy(o + j, hex, hl); j += hl; } else o[j++] = x->val[i]; }
            *out = text_of(o, j); scratch_restore(mark); return 1; }
        *out = semantic_text_ref(x); return 1; }
    if (d->what == D_OWN) { Ref w[3] = { k->fw, string_ref(x->name, x->nlen), string_ref(x->val, x->vlen) }; w[0].said = 0; *out = said_tuple(compose(w, 3, ref_above(w, 3))); return 1; }
    if (d->what == D_TYPE) {                                                 /* by the source's key of it; or the type whose content this text is; a text that is neither is left open */
        const uint8_t *kp = x->val; size_t kn = x->vlen;
        for (int more = 1; more; ) { more = 0;                                /* white space around it, a no-break space (U+00A0) among it */
            if (kn && (*kp == ' ' || *kp == '\t')) { kp++; kn--; more = 1; } else if (kn >= 2 && kp[0] == 0xC2 && kp[1] == 0xA0) { kp += 2; kn -= 2; more = 1; }
            if (kn && (kp[kn - 1] == ' ' || kp[kn - 1] == '\t' || kp[kn - 1] == '\r')) { kn--; more = 1; } else if (kn >= 2 && kp[kn - 2] == 0xC2 && kp[kn - 1] == 0xA0) { kn -= 2; more = 1; } }
        Keys kf; kf.n = 0; if (d->k.has_re || d->k.nas) { key_form(&d->k, kp, kn, &kf, t, c); if (!kf.n) return 0; }     /* the key as the lists write it; a key the pattern does not match is no key of theirs */
        int nk = kf.n ? kf.n : 1;                                              /* with no template, the key as written, once */
        for (int j = 0; j < nk; j++) { if (kf.n) { kp = (const uint8_t *)kf.k[j]; kn = strlen(kf.k[j]); } Ref v = string_ref(kp, kn);     /* each template's key in turn: the first a list knows */
            for (uint8_t i = 0; i < d->ntargets; i++) {
                const TargetCode *target = &d->targets[i];
                int has = 0; *out = highway_typed(target->name, kp, kn, &has); if (has) return 1;
                const lp_list *L = HW ? lp_highway_list(HW, target->name) : NULL; if (L && lp_highway_slot(HW, L, &v.id) >= 0) { *out = v; return 1; }
            } }
        if (d->k.node) { if (once_for(k, t, c)) note_obligation(k, 'm', d->road_label, x->name, x->nlen, NULL, 0); *out = string_ref(kp, kn); return 1; }
        unknown_key(k, t, c, x);     /* a key no list of the highway holds: said, never taken for text */
        return 0; }
    if (d->what == D_CODEPOINTS || d->what == D_RANGE) {
        self_cp = k->selfcp; self_mark = s->selfmark;
        const uint8_t *p = x->val; size_t n = x->vlen; while (n && *p == ' ') { p++; n--; } while (n && p[n - 1] == ' ') n--;
        const uint8_t *dd = d->what == D_RANGE ? memmem(p, n, "..", 2) : NULL;
        if (dd) {                                                            /* a range, written as its first and its last: the path of the two */
            uint8_t a[8], b[8]; size_t al, bl; if (!codepoints_text(p, (size_t)(dd - p), a, sizeof a, &al) || !codepoints_text(dd + 2, n - (size_t)(dd - p) - 2, b, sizeof b, &bl)) { *out = text_of(p, n); return 1; }   /* not code points: the text it is written as */
            Ref two[2] = { string_ref(a, al), string_ref(b, bl) }; *out = said_tuple(compose(two, 2, ref_above(two, 2))); return 1; }
        uint8_t buf[512]; size_t l; if (!codepoints_text(p, n, buf, sizeof buf, &l)) { if (!n) return 0; *out = text_of(p, n); return 1; }
        *out = string_ref(buf, l); return 1; }
    if (d->what == D_REFER && depth < 16) {                                  /* the first of its targets that holds the key */
        for (uint8_t i = 0; i < d->ntargets; i++) {
            const TargetCode *target = &d->targets[i];
            if (!target->local) { if (keys_get(target->name, x->val, x->vlen, out)) return 1; continue; }      /* a key of another of the source's files: the thing its row is */
            int32_t g = keyed(k, t, target, x->val, x->vlen); if (g < 0) continue;                            /* in the same tree: by the index */
            if (target->within) { for (int32_t up = t->n[g].parent; up >= 0; up = t->n[up].parent) if (thing_of(k, t, up, out, depth + 1)) return 1; continue; }   /* the thing what holds the key is inside */
            if (thing_of(k, t, g, out, depth + 1)) return 1; }
        unknown_key(k, t, c, x);
        return 0; }
    return 0;
}
/* What a part speaks of: its own thing, or, for a part that is no thing, the nearest thing it is inside. */
static int spoken_thing(Sink *k, const SemanticRecord *t, int32_t g, Ref *out){
    for (int32_t up = g; up >= 0; up = t->n[up].parent) { if (thing_of(k, t, up, out, 0)) return 1;
        for (int i = 0; i < k->s->nthing; i++) if (named_as(&t->n[up], k->s->thing[i].tier)) return 0; }     /* a kind the recipe names a thing of, naming none here (a synset with no members), speaks of nothing: never of what holds it */
    return 0;
}
static int entity_of(Sink *k, const SemanticRecord *t, int32_t c, Ref *out);
static int thing_of(Sink *k, const SemanticRecord *t, int32_t g, Ref *out, int depth){
    int memo = k->tc_tree == t && (uint32_t)g < k->tc_n;                     /* each thing is composed once in a tree; the threads reading one tree share what is composed */
    if (memo) { uint8_t st = __atomic_load_n(&k->ts[g], __ATOMIC_ACQUIRE); if (st >= 3) { *out = k->tc[g]; return st - 2; } if (st == 2) return 0; }     /* 3 and up: named, by the way st - 3 */
    if (memo && composing(g)) return 0;                                            /* being composed by this thread: a thing named by itself names nothing */
    int pushed = memo && nbusy < BUSY_MAX; if (pushed) busy[nbusy++] = g;
    int r = thing_compose(k, t, g, out, depth);
    if (pushed) nbusy--;
    if (memo) { if (r) k->tc[g] = *out; __atomic_store_n(&k->ts[g], (uint8_t)(r ? (r < 250 ? r + 2 : 252) : 2), __ATOMIC_RELEASE); }
    return r;
}
/* >REF/PATH: the PATH of the part of the same tree that the part REF refers to (refer REF TIER: the TIER whose key REF is
 * written as; a sense's synset, and the synset's ili); -1 where it names nothing here. */
static int32_t referred_path(Sink *k, const SemanticRecord *t, int32_t g, const char *name){
    const PathStep *first = path_code(name)->first; if (!first) return -1;
    int32_t c = path_first(t, g, first, first->next); if (c < 0) return -1;
    const Dis *d = dis_of(k->s, &t->n[c]); const SemanticValue *x = &t->n[c];
    if (x->kind == S_GROUP && semantic_first(x) >= 0 && semantic_next(&t->n[semantic_first(x)]) < 0 && t->n[semantic_first(x)].kind == S_TEXT) x = &t->n[semantic_first(x)];     /* a part that holds only its text */
    if (!d || d->what != D_REFER || !x->vlen) return -1;
    for (uint8_t i = 0; i < d->ntargets; i++) {
        const TargetCode *target = &d->targets[i];
        if (target->within || !target->local) continue;
        int32_t r = keyed(k, t, target, x->val, x->vlen); if (r < 0) continue; return path_first(t, r, first->next, NULL); }
    return -1;
}
/* A part as one way its kind is named: 0 when it is not named that way. */
static int one_thing(Sink *k, const SemanticRecord *t, int32_t g, const Thing *th, Ref *out, int depth){
    const SemanticValue *x = &t->n[g]; Ref p[8]; {
        if (path_code(th->name[0])->special == SELECT_SELF) { if (!entity_of(k, t, g, out)) return 0; out->said = 0; return 1; }        /* the thing is its content: the part itself, whole */
        if (th->of) {                                                        /* the path of what its parts of those names are, in the file's order */
            size_t mark = scratch_mark();
            uint32_t max_k = x->arity ? x->arity : 64;
            Ref *pc = scratch_alloc(sizeof(Ref) * max_k); size_t np = 0;
            for (int32_t c = semantic_first(x); c >= 0; c = semantic_next(&t->n[c])) { int hit = 0; for (int z = 0; z < th->n && !hit; z++) hit = named_as(&t->n[c], th->name[z]); if (!hit) continue;
                Ref v; if (!(t->n[c].kind == S_GROUP ? thing_of(k, t, c, &v, depth + 1) : value_of(k, t, c, &v, depth))) continue;
                if (np < max_k) { v.said = 0; pc[np++] = v; } }
            if (!np) { scratch_restore(mark); return 0; } *out = np == 1 ? pc[0] : compose(pc, (uint32_t)np, ref_above(pc, np)); out->said = 0; scratch_restore(mark); return 1; }
        if (th->span) {                                                      /* the stretch of the text of TEXT, found in what it is inside, from the character START to END */
            int32_t a = resolve_node_path(t, g, th->name[0]), b = resolve_node_path(t, g, th->name[1]); if (a < 0 || b < 0 || !t->n[a].vlen || !t->n[b].vlen || t->n[a].vlen > 11 || t->n[b].vlen > 11) return 0;
            char za[16], zb[16]; memcpy(za, t->n[a].val, t->n[a].vlen); za[t->n[a].vlen] = 0; memcpy(zb, t->n[b].val, t->n[b].vlen); zb[t->n[b].vlen] = 0; char *e1, *e2; long lo = strtol(za, &e1, 10), hi = strtol(zb, &e2, 10);
            if (*e1 || *e2 || lo < 0 || hi < lo) return 0; if (th->inclusive) hi++;
            int32_t tx = -1; for (int32_t up = x->parent; up >= 0 && tx < 0; up = t->n[up].parent) tx = semantic_child(t, up, th->name[2], -1);
            if (tx < 0) return 0; const SemanticValue *tn = &t->n[tx]; if (tn->kind == S_GROUP) { if (semantic_first(tn) < 0) return 0; tn = &t->n[semantic_first(tn)]; }
            size_t i = 0, blo = 0, bhi = 0; long ch = 0; int have = 0;                 /* characters counted as the source counts them: code points */
            for (; i <= tn->vlen; ch++) { if (ch == lo) { blo = i; have = 1; } if (ch == hi) { bhi = i; break; } if (i == tn->vlen) return 0;
                uint8_t c = tn->val[i]; i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
            if (!have || bhi <= blo || bhi > tn->vlen) return 0;
            *out = text_of(tn->val + blo, bhi - blo); out->said = 0; return 1; }
        if (th->resolved) {                                                  /* the path of those of its parts that resolve, in the recipe's order */
            uint32_t np = 0;
            for (int z = 0; z < th->n; z++) { int ref = th->name[z][0] == '>'; int32_t c = ref ? referred_path(k, t, g, th->name[z] + 1) : resolve_node_path(t, g, th->name[z]); if (c < 0) continue;
                const SemanticValue *cn = &t->n[c]; if (cn->kind == S_GROUP && semantic_first(cn) >= 0 && semantic_next(&t->n[semantic_first(cn)]) < 0 && t->n[semantic_first(cn)].kind == S_TEXT) cn = &t->n[semantic_first(cn)];
                if (left_empty(k->s, cn) || (cn->kind != S_GROUP && !cn->vlen)) continue;            /* what the source leaves empty: in no path, and creates no obligation */
                if (ref) { UnkMark um; unknown_mark(k, t, c, &um); if (value_of(k, t, c, &p[np], depth)) { p[np].said = 0; np++; } else unknown_back(k, t, &um); continue; }     /* what another part says: its own reading owes what it leaves unsaid */
                if (value_of(k, t, c, &p[np], depth)) { p[np].said = 0; np++; continue; }
                note_obligation(k, 'o', &diagnostic_labels[DIAG_UNRESOLVED_KEY], (const uint8_t *)th->name[z], path_code(th->name[z])->source_length, NULL, 0); }     /* said, never dropped */
            if (!np) return 0;
            Ref X = np == 1 ? p[0] : said_tuple(compose(p, np, ref_above(p, np)));
            if (th->infile) { Ref two[2] = { k->fname, X }; two[0].said = two[1].said = 0; X = said_tuple(compose(two, 2, ref_above(two, 2))); }
            *out = X; return 1; }
        for (int z = 0; z < th->n; z++) {
            if (th->lit[z]) { p[z] = text_of((const uint8_t *)th->name[z], strlen(th->name[z])); p[z].said = 0; continue; }      /* a text the recipe writes */
            int32_t c = th->name[z][0] == '>' ? referred_path(k, t, g, th->name[z] + 1) : resolve_node_path(t, g, th->name[z]); if (c < 0) return 0;
            if (th->form[z].has_re || th->space[z]) {                        /* the part's text as the source's notation parts it: what it writes, never what it is disposed as */
                const SemanticValue *cn = &t->n[c]; if (cn->kind == S_GROUP && semantic_first(cn) >= 0 && semantic_next(&t->n[semantic_first(cn)]) < 0 && t->n[semantic_first(cn)].kind == S_TEXT) cn = &t->n[semantic_first(cn)];
                if (cn->kind == S_GROUP || left_empty(k->s, cn)) return 0;
                Keys kf; kf.n = 0; if (th->form[z].has_re) { key_form(&th->form[z], cn->val, cn->vlen, &kf, t, c); if (!kf.n) return 0; } else { if (cn->vlen >= sizeof kf.k[0]) return 0; memcpy(kf.k[0], cn->val, cn->vlen); kf.k[0][cn->vlen] = 0; }
                char *w = kf.k[0]; if (th->space[z]) for (char *q = w; *q; q++) if (*q == th->space[z]) *q = ' ';
                p[z] = text_of((const uint8_t *)w, strlen(w)); p[z].said = 0; continue; }
            if (th->name[z][0] == '>') {                                     /* what another part says (a synset's ili): its own reading counts what it leaves unsaid, not this one */
                UnkMark um; unknown_mark(k, t, c, &um); if (!value_of(k, t, c, &p[z], depth)) { unknown_back(k, t, &um); return 0; }
                p[z].said = 0; continue; }
            if (t->n[c].kind == S_GROUP && semantic_first(&t->n[c]) >= 0 && semantic_next(&t->n[semantic_first(&t->n[c])]) >= 0) {      /* a part that is several pieces: the path of what each is, in order */
                Ref pc[256]; uint32_t np = 0; for (int32_t q = semantic_first(&t->n[c]); q >= 0 && np < 256; q = semantic_next(&t->n[q])) if (value_of(k, t, q, &pc[np], depth)) { pc[np].said = 0; np++; }
                if (!np) return 0; p[z] = np == 1 ? pc[0] : compose(pc, np, ref_above(pc, np)); }
            else if (!value_of(k, t, c, &p[z], depth)) {
            const Dis *cd = dis_of(k->s, &t->n[c]);
            if (cd && cd->what == D_KEY && !left_empty(k->s, &t->n[c])) {
                const SemanticValue *cn = &t->n[c];
                if (cn->kind == S_GROUP && semantic_first(cn) >= 0 && semantic_next(&t->n[semantic_first(cn)]) < 0 && t->n[semantic_first(cn)].kind == S_TEXT) cn = &t->n[semantic_first(cn)];
                if (cn->kind == S_TEXT && cn->vlen) p[z] = text_of(cn->val, cn->vlen); else return 0;
            } else return 0;
        }
            p[z].said = 0; }
        Ref X = th->n == 1 ? p[0] : said_tuple(compose(p, (uint32_t)th->n, ref_above(p, (size_t)th->n)));      /* named by several parts together: the path of them */
        if (th->within && depth < 8) {                                       /* its name stands only within what it is inside: the pair of the two */
            Ref P; int32_t up = x->parent; while (up >= 0 && !thing_of(k, t, up, &P, depth + 1)) up = t->n[up].parent;
            if (up >= 0) { Ref two[2] = { P, X }; two[0].said = two[1].said = 0; X = said_tuple(compose(two, 2, ref_above(two, 2))); } }
        if (th->infile) { Ref two[2] = { k->fname, X }; two[0].said = two[1].said = 0; X = said_tuple(compose(two, 2, ref_above(two, 2))); }      /* its name stands only within the file: [the file's name, it] */
        *out = X; return 1; }
}
/* A part as the thing it is: the first of the ways its kind is named that names it (a code point, or a range). */
static int thing_compose(Sink *k, const SemanticRecord *t, int32_t g, Ref *out, int depth){
    const SemanticValue *x = &t->n[g]; const Say *s = k->s;
    for (int i = 0; i < s->nthing; i++) if (named_as(x, s->thing[i].tier) && one_thing(k, t, g, &s->thing[i], out, depth)) return i + 1;                       /* which way named it, counted from 1 */
    return 0;
}
/* Whether a part is one of those that name the thing it is in (an entry's word and pos): by the way that named it,
 * never by another way of its kind (a synset named by its members still says its ili; one with none, named by its ili,
 * does not). */
static int names_it(Sink *k, const SemanticRecord *t, int32_t g, const SemanticValue *y){
    Ref X; int r = thing_of(k, t, g, &X, 0); if (!r) return 0; const Thing *th = &k->s->thing[r - 1];
    for (int z = 0; z < th->n; z++) if (!th->lit[z] && !strchr(th->name[z], '/') && semantic_named(y, th->name[z], strlen(th->name[z]))) return 1;
    return 0;
}
/* Whether a part meets the conditions named: the outermost part the recipe speaks of (where ...), or a claim said
 * only of the parts that meet them (when ...). */
static int spoken_of(const Say *s, const SemanticRecord *t, int32_t root, uint32_t which){
    for (int i = 0; i < s->nwhere; i++) { if (!(which & (1u << i))) continue; const Where *w = &s->where[i]; int32_t c = resolve_node_path(t, root, w->name);
        if (c >= 0 && t->n[c].kind == S_GROUP && semantic_first(&t->n[c]) >= 0 && semantic_next(&t->n[semantic_first(&t->n[c])]) < 0 && t->n[semantic_first(&t->n[c])].kind != S_GROUP) c = semantic_first(&t->n[c]);     /* a part that holds only its text */
        const uint8_t *v = c >= 0 ? t->n[c].val : (const uint8_t *)""; size_t n = c >= 0 ? t->n[c].vlen : 0;
        if (w->op == 2) { size_t mark = scratch_mark(); char stack[512], *z = n < sizeof stack ? stack : scratch_alloc(n + 1); memcpy(z, v, n); z[n] = 0; int hit = !regexec(&w->re, z, 0, NULL, 0); scratch_restore(mark); if (!hit) return 0; }
        else { int same = n == strlen(w->val) && !memcmp(v, w->val, n); if (same == (w->op == 1)) return 0; } }
    return 1;
}
/* A node of the file's tree as the entity it is: a record of what the source teaches. A part is what its disposition
 * says; a part that is itself parts is the path of them, a KEY IS VALUE part the pair of the two; and whatever holds
 * parts is the path of what they are, in the file's order. A key is how the file's rows point at each other, and is in
 * no record (key: written nowhere); a reference is the thing it names, never its key's text. A row, a line and the
 * source's numbering are packaging, never content (Recipes 10.3, 10.8). What parts a tier (a tab, a line's end) is the
 * layout's, written in the recipe: it is how the file is written down, and is not a constituent of anything. */
static int entity_of(Sink *k, const SemanticRecord *t, int32_t c, Ref *out){
    const SemanticValue *x = &t->n[c]; const Say *s = k->s; const SemanticValue *up = x->parent >= 0 ? &t->n[x->parent] : NULL;
    if (x->kind != S_GROUP) {
        if (left_empty(s, x)) return 0;
        if (up && up->kind == S_GROUP && up->tier == x->tier && up->nlen) {       /* a part of a part */
            const Dis *dp = x->nlen ? dis_of(s, x) : NULL; if (dp && (dp->what == D_OMIT || dp->what == D_METADATA)) return 0;
            if (x->kind == S_VALUE && x->nlen) { Ref p[2] = { string_ref(x->name, x->nlen), string_ref(x->val, x->vlen) };      /* a KEY IS VALUE part: the pair of its key and what its value is */
                if (dp && dp->what == D_KEY) return 0;                                  /* the file's own numbering: in no record */
                if (dp && !value_of(k, t, c, &p[1], 0)) return 0;                     /* a reference: the thing it names */
                if (p[1].said != LP_SAID_TUPLE) p[1].said = 0; *out = said_tuple(compose(p, 2, ref_above(p, 2))); return 1; }
            if (x->nlen && value_of(k, t, c, out, 0)) return 1;                 /* a piece read as its part is (a type, a code point) */
            *out = string_ref(x->val, x->vlen); return 1; }
        const Dis *d = x->nlen ? dis_of(s, x) : NULL;
        if (!d) { if (x->nlen && x->kind != S_TEXT) { left_open(k, x); return 0; } *out = semantic_text_ref(x); return 1; }     /* a text is the text it is */
        if (d->what == D_OMIT || d->what == D_METADATA) return 0;
        if (d->what == D_KEY) return 0;                                      /* the file's own numbering: in no record */
        return value_of(k, t, c, out, 0);                                    /* a reference: the thing it names, or nothing */
    }
    const Dis *d = x->nlen ? dis_of(s, x) : NULL; if (d && (d->what == D_OMIT || d->what == D_METADATA)) return 0;
    dropped_open(k, t, (uint32_t)c, 0);
    size_t mark = scratch_mark();
    Ref stack[64], *kid = x->arity <= 64 ? stack : scratch_alloc(sizeof(Ref) * x->arity); uint32_t n = 0;
    for (int32_t q = semantic_first(x); q >= 0; q = semantic_next(&t->n[q])) { Ref v; if (entity_of(k, t, q, &v)) { if (v.said != LP_SAID_TUPLE) v.said = 0; kid[n++] = v; } }
    if (n == 1) *out = kid[0]; else if (n > 1) *out = compose(kid, n, ref_above(kid, n));
    scratch_restore(mark);
    return n > 0;
}
/* Who says what follows: the part named, as the witness it names ([the source's witness, NAME, the name], or what an
 * own part already is). 0 when the part names no one. */
static int voice_of(Sink *k, const SemanticRecord *t, int32_t c){
    if (c < 0 || left_empty(k->s, &t->n[c])) return 0; const SemanticValue *y = &t->n[c]; const Dis *d = dis_of(k->s, y);
    if (d && d->what == D_OWN) { if (!value_of(k, t, c, &k->voice, 0)) return 0; }
    else { Ref w[3] = { k->fw, string_ref(y->name, y->nlen), string_ref(y->val, y->vlen) }; w[0].said = 0; k->voice = said_tuple(compose(w, 3, ref_above(w, 3))); }
    k->voiced = 1; return 1;
}
static int said_by(Sink *k, const SemanticRecord *t, int32_t g, const char *by){ return voice_of(k, t, resolve_node_path(t, g, by)); }
/* The witnesses a part names for what it says (by PATH): every one the path names, each saying it in its own voice.
 * Where the part names none, the source says it, as it says everything it writes. */
#define WHO 64
static int whos(const Say *s, const SemanticRecord *t, int32_t g, const char *by, int32_t *who){
    if (!by[0]) return 0; int n = all_path(t, g, by, who, WHO), m = 0;
    for (int i = 0; i < n; i++) if (who[i] >= 0 && t->n[who[i]].vlen && !left_empty(s, &t->n[who[i]])) who[m++] = who[i];      /* a column it leaves empty names no one: the source says it */
    return m;
}
/* A part says a claim once. Two directives of a recipe that reach the same claim of one part (holds record * via and
 * attest * *, for a sense of a Wiktextract entry) are one statement, not two: the same witness attesting the same claim
 * in one record twice would play it twice. The claims a part has made are kept by the claim and who says it, for the
 * part being read alone (cs_epoch: a new part, an empty set, nothing cleared): the pair itself, so no two pairs share a slot
 * by a mix of their IDs. Said again, it is the same statement said once more: its run in the record (ev: the event that
 * says it, in this sink). */
struct CSeen { lp_id claim, voice; uint64_t epoch, ev; };
static int said_already(Sink *k, const lp_id *c){
    if (!k->cs_epoch) k->cs_epoch = 1;                                      /* a slot never used is of epoch 0: no part's */
    lp_id voice; memset(&voice, 0, sizeof voice); if (k->voiced) voice = k->voice.id;     /* the source's own voice: no ID */
    if ((k->cs_n + 1) * 2 > k->cs_cap) {                                     /* half full: twice the room, this part's keys moved */
        struct CSeen *o = k->cs; uint64_t oc = k->cs_cap; k->cs_cap = oc ? oc * 2 : 256;
        if (k->cs_cap < oc || k->cs_cap > SIZE_MAX / sizeof(struct CSeen)) { fputs("claim index exceeds the output arena\n", stderr); abort(); }
        k->cs = arena_alloc(&k->output, (size_t)k->cs_cap * sizeof *k->cs); memset(k->cs, 0, (size_t)k->cs_cap * sizeof *k->cs);
        for (uint64_t i = 0; i < oc; i++) if (o[i].epoch == k->cs_epoch) { uint64_t h, w; memcpy(&h, o[i].claim.b, 8); memcpy(&w, o[i].voice.b + 8, 8); uint64_t s = (h ^ w) & (k->cs_cap - 1);
            while (k->cs[s].epoch == k->cs_epoch) s = (s + 1) & (k->cs_cap - 1); k->cs[s] = o[i]; }
    }
    uint64_t h, w; memcpy(&h, c->b, 8); memcpy(&w, voice.b + 8, 8); uint64_t s = (h ^ w) & (k->cs_cap - 1);
    while (k->cs[s].epoch == k->cs_epoch) { if (!memcmp(&k->cs[s].claim, c, 16) && !memcmp(&k->cs[s].voice, &voice, 16)) {
            uint64_t e = k->cs[s].ev; if (e < k->ev.n && k->ev.e[e].run < (1u << LP_M_RUN_BITS) - 1) k->ev.e[e].run++; return 1; }
        s = (s + 1) & (k->cs_cap - 1); }
    k->cs[s].claim = *c; k->cs[s].voice = voice; k->cs[s].epoch = k->cs_epoch; k->cs[s].ev = k->ev.n; k->cs_n++; return 0;
}
/* A claim said by the part being read: an event, in the record being read (rec), said once (run 1) so far. */
static void said(Sink *k, const lp_id *c, float score){
    Event x = {.claim=*c, .witnessed=*c, .score=score, .enter_rating=k->er, .enter_deviation=k->ed, .kind=EV_CLAIM, .run=1, .rec=k->rec};
    if (k->voiced) { x.own_witness = 1; x.witness = k->voice.id; } sink_event(k, &x);
}
static void claim(Sink *k, Ref *part, int n){
    for (int i = 0; i < n; i++) if (part[i].said != LP_SAID_TUPLE) part[i].said = 0;      /* a tuple held by a claim stays a tuple: M says so, the ID is the same */
    Ref c = said_claim(compose(part, (uint32_t)n, ref_above(part, (size_t)n)));
    if (said_already(k, &c.id)) return;
    said(k, &c.id, k->score);
    push(&k->grp, &c);
}
/* What a part said, with everything inside it, in the order its source gave it: each claim keeps its place among the
 * part's claims (position: the place the source gave the claim among its like, in the claim's vertex of its record).
 * e0 .. the end: the events it and what is inside it made. */
static void together(Sink *k, uint64_t e0, const Ref *about){
    (void)about; uint32_t at = 0;
    for (uint64_t i = e0; i < k->ev.n; i++) if (k->ev.e[i].kind == EV_CLAIM && !k->ev.e[i].position) k->ev.e[i].position = ++at;
}
/* The last node inside a part: its subtree is the nodes from it to there. */
static int32_t subtree_end(const SemanticRecord *t, int32_t g){ return (int32_t)t->n[g].extent; }
/* The score a part of a tier gives what it attests: the number the recipe names, on the scale the source writes it on
 * (its lowest a loss, its highest a win, halfway a draw); a win where it gives none. */
static float score_of(const Say *s, const SemanticRecord *t, int32_t g){
    const SemanticValue *x = &t->n[g];
    for (int i = 0; i < s->nscore; i++) if (semantic_named(x, s->score[i].tier, strlen(s->score[i].tier))) { int32_t c = semantic_child(t, g, s->score[i].name, -1); if (c < 0 || !t->n[c].vlen || t->n[c].vlen > 31) return 1.0f;
        char z[32]; memcpy(z, t->n[c].val, t->n[c].vlen); z[t->n[c].vlen] = 0; char *e; double v = strtod(z, &e); if (e == z) return 1.0f;
        v = (v - s->score[i].from) / (s->score[i].to - s->score[i].from); return (float)(v < 0 ? 0 : v > 1 ? 1 : v); }
    return 1.0f;
}
/* A key as the highway keeps it: the text as written, or, where the line gives a pattern, what it matches written as
 * the template says (\0 the whole, \1 to \9 its parts; with no template, its first part, or the whole). {NAME} in a
 * template is the text of the part NAME beside it, in what holds both (wndb.5: a pointer's synset_offset is a synset
 * only with its pos: \0-{pos}); where there is no such part, that template makes no key. Several templates are each a
 * key, in order (as \0-{pos} as \0-s: the first a list knows). */
static void key_form(const KSpec *spec, const uint8_t *value, size_t length, Keys *out, const SemanticRecord *tree, int32_t at){
    if (!length || length >= 160) return;
    char text[160]; memcpy(text, value, length); text[length] = 0;
    regmatch_t matches[10];
    for (int i = 0; i < 10; i++) matches[i].rm_so = matches[i].rm_eo = -1;
    matches[0].rm_so = 0; matches[0].rm_eo = (regoff_t)length;
    if (spec->has_re && regexec(&spec->re, text, 10, matches, 0)) return;
    if (!spec->nas) {
        unsigned capture = spec->has_re && matches[1].rm_so >= 0 ? 1 : 0;
        size_t n = (size_t)(matches[capture].rm_eo - matches[capture].rm_so);
        if (n && out->n < 16) { memcpy(out->k[out->n], text + matches[capture].rm_so, n); out->k[out->n++][n] = 0; }
        return;
    }
    if (!spec->program) { fputs("key templates were not compiled at recipe load\n", stderr); abort(); }
    for (int i = 0; i < spec->nas && out->n < 16; i++) {
        const KeyTemplate *form = &spec->program->forms[i];
        char *result = out->k[out->n]; size_t used = 0; int valid = 1;
        for (uint16_t j = 0; j < form->count; j++) {
            const KeyInstruction *instruction = &form->instructions[j];
            const void *part = instruction->text.bytes; size_t n = instruction->text.length;
            if (instruction->operation == KEY_CAPTURE) {
                regmatch_t capture = matches[instruction->capture];
                if (capture.rm_so < 0) continue;
                part = text + capture.rm_so; n = (size_t)(capture.rm_eo - capture.rm_so);
            } else if (instruction->operation == KEY_FIELD) {
                int32_t parent = tree && at >= 0 ? tree->n[at].parent : -1, sibling = -1;
                if (parent >= 0) for (int32_t child = semantic_first(&tree->n[parent]); child >= 0; child = semantic_next(&tree->n[child]))
                    if (name_matches(&tree->n[child], &instruction->text)) { sibling = child; break; }
                if (sibling < 0 || !tree->n[sibling].vlen) { valid = 0; break; }
                part = tree->n[sibling].val; n = tree->n[sibling].vlen;
            }
            if (n >= 160 - used) { valid = 0; break; }
            if (n) memcpy(result + used, part, n); used += n;
        }
        result[used] = 0; if (valid && used) out->n++;
    }
}
/* Every key a part names at a path: each value there, and each piece of a value written in pieces. */
static void keys_at(const Say *s, const SemanticRecord *t, int32_t g, const KSpec *ks, Keys *out){
    out->n = 0; int32_t at[WHO]; int na = all_path(t, g, ks->path, at, WHO);
    for (int i = 0; i < na; i++) { const SemanticValue *x = &t->n[at[i]];
        if (x->kind != S_GROUP) { if (!left_empty(s, x)) key_form(ks, x->val, x->vlen, out, t, at[i]); continue; }
        for (int32_t q = semantic_first(x); q >= 0; q = semantic_next(&t->n[q])) if (t->n[q].kind != S_GROUP && !left_empty(s, &t->n[q])) key_form(ks, t->n[q].val, t->n[q].vlen, out, t, q); }
}
/* The file's key for every part of the unit named the key's tier, wherever it is in the unit (an instance of a sentence
 * of an XML document read as one tree, or a row that is the unit itself), kept for the source's other files that point
 * at it (refer NAME RECIPE). */
static void keep_keys(Sink *k, const SemanticRecord *t, int32_t root){
    const Say *s = k->s; if (!s->nkey) return; int32_t end = subtree_end(t, root);
    for (int32_t g = root; g <= end; g++) { const SemanticValue *x = &t->n[g]; if (x->context || x->kind != S_GROUP || !x->nlen) continue;
        for (int i = 0; i < s->nkey; i++) if (semantic_named(x, s->key[i].tier, s->key[i].tier_length)) { int32_t kc = semantic_child(t, g, s->key[i].name, -1); Ref S;
            if (kc >= 0 && t->n[kc].vlen && thing_of(k, t, g, &S, 0)) keys_put(k->r->name, t->n[kc].val, t->n[kc].vlen, S, &k->kr); } }
}
/* A part read for the highway: the types it is, their keys, and the keys it maps to one another. */
static void unit_highway(Sink *k, const SemanticRecord *t, int32_t root){
    const Say *s = k->s; if (!spoken_of(s, t, root, s->whole)) return;
    for (uint32_t g = 0; g < t->count; g++) { const SemanticValue *x = &t->n[g]; if (x->context || x->kind == S_NOTE || !x->nlen) continue;
        { const Dis *dg = dis_of(s, x); if (dg && dg->what == D_OMIT) { g = (uint32_t)subtree_end(t, (int32_t)g); continue; } }
        for (int i = 0; i < s->nhw; i++) { const HwLine *h = &s->hw[i]; if (!named_as(x, h->tier) || !spoken_of(s, t, (int32_t)g, h->when)) continue;
            Ref X; Keys A, B;
            if (h->what <= 2) { if (x->kind == S_VALUE || !(x->kind == S_GROUP ? thing_of(k, t, (int32_t)g, &X, 0) : value_of(k, t, (int32_t)g, &X, 0))) continue;     /* a thing; or a text of a list, a type by itself */
                if (h->what == 1) { hw_rec(k, 1, i, &X, NULL, 0, NULL, 0); continue; }
                keys_at(s, t, (int32_t)g, &h->a, &A); for (int a = 0; a < A.n; a++) hw_rec(k, 2, i, &X, A.k[a], strlen(A.k[a]), NULL, 0); continue; }
            keys_at(s, t, (int32_t)g, &h->a, &A); if (!A.n) continue; keys_at(s, t, (int32_t)g, &h->b, &B);
            for (int a = 0; a < A.n; a++) for (int b = 0; b < B.n; b++) hw_rec(k, h->what, i, NULL, A.k[a], strlen(A.k[a]), B.k[b], strlen(B.k[b])); } }
}
/* ---- a wide part (Unicode's repertoire, a wordnet's lexicon): the parts inside it read on every core, each into a sink
 * of its own, and kept in the file's order, so what is said is what one thread reading the tree in order says. The
 * things composed of the tree (tc) and its key index are shared: each is the same whoever composes it. */
typedef struct { int32_t end; uint64_t e0; Ref about; int has; } Pend;
static void unit_range(Sink *k, const SemanticRecord *t, uint32_t g0, uint32_t g1, int speaks, Pend *pend, int *npp);


/* A thread may take up a part while it waits inside its own reading: what it was in the middle of is put back after. */
typedef struct { const SemanticRecord *tt; const Say *say; const BindingMatrix *bindings; int16_t *dm; int base, nbusy; long self_cp; char self_mark; } Tls;
static Tls tls_save(const SemanticRecord *t, int16_t *dm, const Say *say, const BindingMatrix *bindings){
    Tls x = { TT, active_say, active_bindings, DM, busy_base, nbusy, self_cp, self_mark };
    TT = t; active_say = say; active_bindings = bindings; DM = dm; busy_base = nbusy; return x;
}
static void tls_back(const Tls *x){ TT = x->tt; active_say = x->say; active_bindings = x->bindings; DM = x->dm; busy_base = x->base; nbusy = x->nbusy; self_cp = x->self_cp; self_mark = x->self_mark; }
/* Parts of g in runs of about the same number of nodes; returns the last node inside g. */


/* The parts of a wide part, whole (entity_of), on every core: each run of them into a sink of its own, in order. */

/* A tree read from node g0 up to g1, in the file's order. pend: the parts whose claims are said together, still open. */
static void unit_range(Sink *k, const SemanticRecord *t, uint32_t g0, uint32_t g1, int speaks, Pend *pend, int *npp){
    const Say *s = k->s; int np_ = *npp;
    for (uint32_t g = g0; g < g1; g++) { const SemanticValue *x = &t->n[g];
        if (x->context) continue;
        /* a part of a wide part is a part of its own: what it says is said once in it (said_already), never once in the run
         * of parts a worker happens to read, whose bounds are the number of threads (runs_of): the same claims on any machine */
        while (np_ && pend[np_ - 1].end < (int32_t)g) { np_--; together(k, pend[np_].e0, pend[np_].has ? &pend[np_].about : NULL); }
        if (x->kind == S_NOTE || x->kind == S_VALUE) { const Dis *d = dis_of(s, x);
            if (d && d->what == D_METADATA && !left_empty(s, x)) { Ref p[2] = { string_ref(x->name, x->nlen), text_of(x->val, x->vlen) }; Ref m = said_tuple(compose(p, 2, ref_above(p, 2))); push(&k->meta, &m); }
            continue; }
        if (x->kind != S_GROUP || !speaks) continue;
        if (x->nlen) { const Dis *dg = dis_of(s, x); if (dg && dg->what == D_OMIT) { g = (uint32_t)subtree_end(t, (int32_t)g); continue; } }     /* the file's bookkeeping: nothing inside it is read */
        k->selfcp = -1;
        if (s->selfmark) { Ref X; if (spoken_thing(k, t, (int32_t)g, &X) && X.tier == 0) k->selfcp = (long)lp_tier0_codepoint(T0, &X.id); }     /* the code point the part speaks of, for the source's mark */
        Ref S; int has = -1; float sc = score_of(s, t, (int32_t)g); k->score = s->nitself ? 1.0f : sc; k->voiced = 0;      /* the score is the attested tuple's, where there is one; what is said of it is said outright */
        if (s->voice.tier[0] && semantic_named(x, s->voice.tier, strlen(s->voice.tier))) said_by(k, t, (int32_t)g, s->voice.name);     /* who says everything the part says */
        uint64_t e0 = k->ev.n; k->grp.n = 0;
        for (int a = 0; a < s->nvoices; a++) { if (!semantic_named(x, s->voices[a].tier, strlen(s->voices[a].tier)) || !spoken_of(s, t, (int32_t)g, s->voices[a].when)) continue;   /* each such part a witness of its own: [the source's witness, NAME] */
            if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break; int was = k->voiced; Ref wv = k->voice;
            for (int z = 0; z < s->voices[a].n; z++) for (int32_t c = semantic_first(x); c >= 0; c = semantic_next(&t->n[c])) { const SemanticValue *y = &t->n[c]; Ref v; if (!y->nlen || !named_as(y, s->voices[a].name[z]) || !value_of(k, t, c, &v, 0)) continue;
                Ref w[3] = { k->fw, k->fname, string_ref(y->name, y->nlen) }; int nw = 2; if (s->voices[a].file) nw = 3; else w[1] = w[2]; w[0].said = 0; w[1].said = 0;     /* [the source's witness, NAME], or within the file [witness, file, NAME] */
                k->voice = said_tuple(compose(w, (uint32_t)nw, ref_above(w, (size_t)nw))); k->voiced = 1; Ref p[2] = { S, v }; claim(k, p, 2); }
            k->voiced = was; k->voice = wv; }
        for (int a = 0; a < s->nitself; a++) { if (!semantic_named(x, s->itself[a].tier, strlen(s->itself[a].tier)) || !spoken_of(s, t, (int32_t)g, s->itself[a].when)) continue;
            Ref X; if (!thing_of(k, t, (int32_t)g, &X, 0)) break;
            int32_t who[WHO]; int nw = whos(s, t, (int32_t)g, s->itself[a].by, who), was = k->voiced; Ref wv = k->voice;
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !voice_of(k, t, who[wi])) continue;
                said(k, &X.id, sc); }
            k->voiced = was; k->voice = wv; }
        for (int a = 0; a < s->npair; a++) { if (!named_as(x, s->pair[a].tier) || !spoken_of(s, t, (int32_t)g, s->pair[a].when)) continue;      /* nothing written between the two: the pair */
            Ref P;
            if (s->pair[a].of[0]) { int32_t oc = resolve_node_path(t, (int32_t)g, s->pair[a].of); if (oc < 0 || !value_of(k, t, oc, &P, 0)) continue; }      /* of another of its parts */
            else { if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break; P = S; }
            for (int z = 0; z < s->pair[a].n; z++) { Ref v;
                if (path_code(s->pair[a].name[z])->special == SELECT_FILE) v = k->fstem;           /* what the file's own name says of each of its parts */
                else { int32_t c = resolve_node_path(t, (int32_t)g, s->pair[a].name[z]); if (c < 0) continue;
                    if (t->n[c].kind == S_GROUP && t->n[c].join == S_PIECES && semantic_first(&t->n[c]) >= 0 && semantic_next(&t->n[semantic_first(&t->n[c])]) >= 0) {      /* a list: the pair with each */
                        for (int32_t q = semantic_first(&t->n[c]); q >= 0; q = semantic_next(&t->n[q])) { Ref w; if (t->n[q].kind == S_GROUP || !value_of(k, t, q, &w, 0) || !memcmp(&w.id, &P.id, 16)) continue; Ref p[2] = { P, w }; claim(k, p, 2); }
                        continue; }
                    if (!value_of(k, t, c, &v, 0)) continue; }
                if (!memcmp(&v.id, &P.id, 16)) continue; Ref p[2] = { P, v }; claim(k, p, 2); } }
        for (int a = 0; a < s->natt; a++) { if (!named_as(x, s->att[a].tier) || !spoken_of(s, t, (int32_t)g, s->att[a].when)) continue;
            Ref S0 = S; int had = has, was = k->voiced; Ref wv = k->voice;
            if (s->att[a].of[0]) { int32_t oc = resolve_node_path(t, (int32_t)g, s->att[a].of); if (oc < 0 || !value_of(k, t, oc, &S, 0)) continue; has = 1; }      /* said of another of its parts */
            else { if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break; }
            int32_t who[WHO]; int nw = whos(s, t, (int32_t)g, s->att[a].by, who);
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !voice_of(k, t, who[wi])) continue;
            for (int z = 0; z < s->att[a].n; z++) for (int32_t c = semantic_first(x); c >= 0; c = semantic_next(&t->n[c])) { const SemanticValue *y = &t->n[c];
                if (path_code(s->att[a].name[z])->special == SELECT_ELEMENTS) {                      /* each element inside it that is a value: its own text, or its one attribute (rdf:resource), as the element's name means it */
                    if (y->kind != S_GROUP || !y->nlen || y->join == S_PIECES) continue; int32_t only = -1, nk = 0;
                    for (int32_t q = semantic_first(y); q >= 0; q = semantic_next(&t->n[q])) { const Dis *dq = t->n[q].nlen && t->n[q].kind == S_VALUE ? dis_of(s, &t->n[q]) : NULL; if (dq && dq->what == D_OMIT) continue; nk++; only = q; }   /* what is omitted does not count */
                    if (nk != 1 || (t->n[only].kind != S_TEXT && t->n[only].kind != S_VALUE) || left_empty(s, &t->n[only])) continue;
                    if (!s->att[a].of[0] && names_it(k, t, (int32_t)g, y)) continue;                     /* the element that names the thing is the thing, not said of it */
                    Ref v; if (!value_of(k, t, only, &v, 0)) continue;
                    const Dis *m = meaning(s, y); if (!m) { if (!wi) note_node(k, &diagnostic_labels[DIAG_ATTEST], y); continue; }      /* the element's name is markup: what it means, or no claim */
                    said_as(k, m, S, v); continue; }
                if (path_code(s->att[a].name[z])->special == SELECT_SELF) {                       /* its own text, as its own name means it */
                    if (y->kind != S_TEXT || left_empty(s, y)) continue; Ref v = text_of(y->val, y->vlen); if (!memcmp(&v.id, &S.id, 16)) continue;    /* the text the thing is named by: the thing, said already */
                    const Dis *m = meaning(s, x); if (!m) { if (!wi) note_node(k, &diagnostic_labels[DIAG_ATTEST], x); continue; }
                    said_as(k, m, S, v); continue; }
                if (!y->nlen || y->kind == S_TEXT || !named_as(y, s->att[a].name[z])) continue;
                if (!s->att[a].of[0] && names_it(k, t, (int32_t)g, y)) continue;                         /* a part that names the thing is said already: it is the thing */
                if (y->kind == S_GROUP && y->join != S_PIECES && strchr(s->att[a].name[z], '*')) continue;      /* a wildcard names the part's values, not the elements inside it, which speak for themselves */
                if (y->kind == S_GROUP) {                                    /* parts: each KEY IS VALUE the pair [KEY, VALUE] (a feature's Name=Value), or each piece, read as the part is */
                    const Dis *my = meaning(s, y);
                    int32_t sc_c = y->nlen == 3 && !memcmp(y->name, "scx", 3) && semantic_first(y) >= 0 && semantic_next(&t->n[semantic_first(y)]) < 0 ? semantic_slot_first(t, (int32_t)g, s->sc_slot) : -1;
                    for (int32_t q = semantic_first(y); q >= 0; q = semantic_next(&t->n[q])) { const SemanticValue *w = &t->n[q]; if (!w->vlen) continue; Ref v;
                        if (sc_c >= 0 && t->n[sc_c].vlen == w->vlen && !memcmp(t->n[sc_c].val, w->val, w->vlen)) continue;     /* scx equal to sc alone: the declared default (UAX #44, Laplace-Engine#25) */
                        if (w->kind == S_VALUE && w->nlen && !semantic_named(w, (const char *)y->name, y->nlen)) { const Dis *dw = dis_of(s, w); if (dw && (dw->what == D_OMIT || dw->what == D_METADATA)) continue;      /* the file's bookkeeping says nothing */
                            if (in_perf(s, (const char *)w->name, w->nlen)) continue;
                            Ref vw; if (!dw || !value_of(k, t, q, &vw, 0)) vw = text_of(w->val, w->vlen);
                            const Dis *mw = meaning(s, w);                   /* the KEY valued itself (MISC.Translit): [thing, VALUE] */
                            if (mw) { said_as(k, mw, S, vw); continue; }
                            if (!my) { if (!wi) note_obligation(k, 'o', &diagnostic_labels[DIAG_ATTEST], y->name, y->nlen, w->name, w->nlen); continue; }
                            Ref kv[2] = { string_ref(w->name, w->nlen), vw }; kv[0].said = 0; if (kv[1].said != LP_SAID_TUPLE) kv[1].said = 0;   /* the pair the record holds: the same entity */
                            Ref pv = said_tuple(compose(kv, 2, ref_above(kv, 2))); of_class(k, my, &pv); said_count(k, my); Ref p[2] = { S, pv }; claim(k, p, 2); continue; }
                        if (!value_of(k, t, q, &v, 0)) continue;
                        if (!my) { if (!wi) note_node(k, &diagnostic_labels[DIAG_ATTEST], y); continue; }
                        said_as(k, my, S, v); } }
                else { if (in_perf(s, (const char *)y->name, y->nlen)) continue;
                    if (y->nlen == 8 && !memcmp(y->name, "kEH_Core", 8) && y->vlen == 1 && y->val[0] == 'N') continue;     /* Unikemet.txt: "Core value: kEH_Core (C for core, L for legacy, default is N)" (Laplace-Engine#25) */
                    Ref v; if (!value_of(k, t, c, &v, 0)) continue;
                    const Dis *m = meaning(s, y); if (!m) { if (!wi) note_node(k, &diagnostic_labels[DIAG_ATTEST], y); continue; }
                    said_as(k, m, S, v); } }
            }
            if (s->att[a].of[0]) { S = S0; has = had; }
            k->voiced = was; k->voice = wv; }
        for (int a = 0; a < s->nhold; a++) { if (!named_as(x, s->hold[a].tier) || !spoken_of(s, t, (int32_t)g, s->hold[a].when)) continue;   /* the things inside it, under their own name */
            if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break;
            /* the nearest things of those names inside it, however deep: what holds them and is no thing is how the file groups them */
            size_t mark = scratch_mark();
            size_t scap = (size_t)t->count + 16, sp = 0; int32_t *stack = scratch_alloc(sizeof(int32_t) * scap);
            { size_t nk = 0; for (int32_t q = semantic_first(x); q >= 0; q = semantic_next(&t->n[q])) nk++;
              sp = nk; size_t i = nk; for (int32_t q = semantic_first(x); q >= 0; q = semantic_next(&t->n[q])) stack[--i] = q; }      /* in the file's order: the first on top */
            while (sp) { int32_t c = stack[--sp]; const SemanticValue *y = &t->n[c]; if (y->kind != S_GROUP) continue; Ref v; int named = 0;
                for (int z = 0; z < s->hold[a].n && !named; z++) named = named_as(y, s->hold[a].name[z]);
                if (named && thing_of(k, t, c, &v, 0)) { if (memcmp(&v.id, &S.id, 16)) {
                        const SemanticValue *by = y; if (s->hold[a].file) { int32_t via = c; while (t->n[via].parent >= 0 && t->n[via].parent != (int32_t)g) via = t->n[via].parent; by = &t->n[via]; }    /* via: the element between them */
                        const Dis *m = meaning(s, by);                       /* an element's name is markup, and what holds what is the tree's: a claim only of what value means */
                        if (!m) note_node(k, &diagnostic_labels[DIAG_HOLDS], by); else said_as(k, m, S, v); } continue; }
                { Ref tmp; if (thing_of(k, t, c, &tmp, 0)) continue; }               /* another thing: what it holds is its own */
                size_t nk = 0; for (int32_t q = semantic_first(y); q >= 0; q = semantic_next(&t->n[q])) nk++;
                if (sp + nk <= scap) { size_t i = sp + nk; for (int32_t q = semantic_first(y); q >= 0; q = semantic_next(&t->n[q])) stack[--i] = q; sp += nk; } }
            scratch_restore(mark); }
        for (int a = 0; a < s->nrel; a++) { if (!named_as(x, s->rel[a].tier) || !spoken_of(s, t, (int32_t)g, s->rel[a].when)) continue;
            if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break;
            int was = k->voiced; Ref wv = k->voice; int32_t who[WHO]; int nw = whos(s, t, (int32_t)g, s->rel[a].by, who);
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !voice_of(k, t, who[wi])) continue;
            Ref rel, o;
            if (path_code(s->rel[a].rel)->special == SELECT_FILE) { if (!wi) note_obligation(k, 'o', &diagnostic_labels[DIAG_RELATE], (const uint8_t *)"{file}", 6, NULL, 0); break; }     /* a file's name is never a relation */
            else { int32_t rc = resolve_node_path(t, (int32_t)g, s->rel[a].rel); if (rc < 0 || !value_of(k, t, rc, &rel, 0)) break;
                const Dis *m = meaning(s, &t->n[rc]); if (!m) { if (!wi) note_node(k, &diagnostic_labels[DIAG_RELATE], &t->n[rc]); break; }     /* a layer's name, a field's: no relation; one of the source's own inventory, which value names */
                of_class(k, m, &rel); said_count(k, m); }
            if (path_code(s->rel[a].to)->special == SELECT_UNNAMED) {                              /* to each of the parts the file gives no name */
                for (int32_t c = semantic_first(x); c >= 0; c = semantic_next(&t->n[c])) { const SemanticValue *y = &t->n[c]; if (y->kind != S_TEXT || y->nlen || left_empty(s, y)) continue; o = text_of(y->val, y->vlen); Ref p[3] = { S, rel, o }; claim(k, p, 3); } }
            else if (semantic_child(t, (int32_t)g, s->rel[a].to, semantic_child(t, (int32_t)g, s->rel[a].to, -1)) >= 0) {   /* several parts of that name: the relation to each */
                for (int32_t oc = semantic_child(t, (int32_t)g, s->rel[a].to, -1); oc >= 0; oc = semantic_child(t, (int32_t)g, s->rel[a].to, oc)) if (value_of(k, t, oc, &o, 0)) { Ref p[3] = { S, rel, o }; claim(k, p, 3); } }
            else { int32_t oc = resolve_node_path(t, (int32_t)g, s->rel[a].to);
                if (oc >= 0 && t->n[oc].kind == S_GROUP && semantic_first(&t->n[oc]) >= 0 && semantic_next(&t->n[semantic_first(&t->n[oc])]) >= 0) {      /* several: the relation to each */
                    for (int32_t q = semantic_first(&t->n[oc]); q >= 0; q = semantic_next(&t->n[q])) if (value_of(k, t, q, &o, 0)) { Ref p[3] = { S, rel, o }; claim(k, p, 3); } }
                else if (oc >= 0 && value_of(k, t, oc, &o, 0)) { Ref p[3] = { S, rel, o }; claim(k, p, 3); }
                else if (s->rel[a].alone && oc >= 0) { Ref p[2] = { S, rel }; claim(k, p, 2); } }      /* to what names nothing (a treebank's root): the relation alone, where the recipe says so; otherwise no claim */
            }
            k->voiced = was; k->voice = wv; }
        for (int a = 0; a < s->ntogether; a++) if (semantic_named(x, s->together[a].tier, strlen(s->together[a].tier)) && spoken_of(s, t, (int32_t)g, s->together[a].when)) {
            if (np_ < 64) { pend[np_].end = subtree_end(t, (int32_t)g); pend[np_].e0 = e0; pend[np_].has = thing_of(k, t, (int32_t)g, &pend[np_].about, 0); np_++; } break; }      /* closed when the reading passes the last node inside it */
    }
    *npp = np_;
}
/* ---- provenance by containment (Engine#22; Semantics: Attestations, Witnesses): what a source says of a part it speaks
 * of is a record, a path in the file's content tree, under the file's trunk, under the source's trunk, which is the
 * witness:
 *   record   [its thing, its layers and tree where the recipe gives them, the claims it says]
 *   claim    a vertex said to be a claim (LP_SAID_CLAIM), in the order the source said them; its run is how many times
 *            the record says it, and its M says how: the outcome (a score in the vertex's spare bits) and its position
 *            among the claims said together (lp_m_full)
 *   voice    a vertex said to be a voice (LP_SAID_VOICE) right before a claim says who in the record says it (a column
 *            of annotators, a speaker): content of the record, never a witness of its own
 *   layer    the values of one name down the record's rows, aligned to them; one the row leaves empty as the file writes it
 *   tree     each row's subtree: its dependents' subtrees and [REL, its thing], in the rows' order; a leaf is its pair
 * A record that is one claim said once is that claim, standing in the content tree as its own record (LP_SAID_RECORD,
 * its outcome and position on that vertex). Pointers (the row's key, HEAD) resolve here and are written nowhere. Who
 * said a claim, how many times and how, is a walk up the container index from it to the records, files and trunks that
 * hold it (provenance.c); nothing else records it. */
static Ref empty_ref(const Say *s){ return s->lay.nempty ? string_ref((const uint8_t *)s->lay.empty[0], (size_t)s->lay.emptylen[0]) : string_ref((const uint8_t *)"_", 1); }
static int field_of(Sink *k, const SemanticRecord *t, int32_t row, const char *name, Ref *out){
    int32_t c = resolve_node_path(t, row, name); if (c < 0) return 0;
    Ref v; if (!entity_of(k, t, c, &v)) return 0; if (v.said != LP_SAID_TUPLE) v.said = 0; *out = v; return 1;
}
static int32_t text_int(const SemanticRecord *t, int32_t row, const char *name, int *ok){
    int32_t c = resolve_node_path(t, row, name); *ok = 0; if (c < 0) return 0;
    const SemanticValue *x = &t->n[c]; if (x->kind == S_GROUP && semantic_first(x) >= 0 && semantic_next(&t->n[semantic_first(x)]) < 0) x = &t->n[semantic_first(x)];
    if (!x->vlen || x->vlen > 9) return 0; int32_t v = 0; for (uint32_t i = 0; i < x->vlen; i++) { if (x->val[i] < '0' || x->val[i] > '9') return 0; v = v * 10 + (x->val[i] - '0'); }
    *ok = 1; return v;
}
typedef struct { Ref head; int parent, first, next, last; } TNode;
static Ref subtree(TNode *tn, int i, int depth, int n){
    Ref part[256]; uint32_t np = 0; int put = 0;
    for (int c = tn[i].first; c >= 0 && depth < n; c = tn[c].next) {
        if (!put && c > i && np < 256) { part[np++] = tn[i].head; put = 1; }
        if (np < 256) { part[np] = subtree(tn, c, depth + 1, n); part[np].said = 0; np++; } }
    if (!put && np < 256) part[np++] = tn[i].head;
    return np == 1 ? part[0] : compose(part, np, ref_above(part, np));
}
/* The record's layers and tree, where the recipe gives them for its tier: appended to part, at most cap; the count. */
static uint32_t record_rows(Sink *k, const SemanticRecord *t, int32_t root, Ref *part, uint32_t cap){
    const Say *s = k->s; uint32_t np = 0, nrow = 0;
    int lay = s->nlayer && named_as(&t->n[root], s->lay_tier), tree = s->tree_row[0] && named_as(&t->n[root], s->tree_tier);
    if (!lay && !tree) return 0;
    size_t mark = scratch_mark();
    uint32_t rcap = t->n[root].arity ? t->n[root].arity : 1;
    int32_t *rows = scratch_alloc(sizeof(int32_t) * rcap);
    for (int32_t c = semantic_first(&t->n[root]); c >= 0; c = semantic_next(&t->n[c])) {
        if (t->n[c].kind != S_GROUP || !named_as(&t->n[c], lay ? s->lay_row : s->tree_row)) continue;
        if (nrow < rcap) rows[nrow++] = c; }
    Ref empty = empty_ref(s), *v = scratch_alloc(sizeof(Ref) * (nrow ? nrow : 1));
    for (int L = 0; lay && L < s->nlayer && nrow && np < cap; L++) {           /* each layer down the rows */
        for (uint32_t r = 0; r < nrow; r++) if (!field_of(k, t, rows[r], s->layer[L], &v[r])) v[r] = empty;
        Ref x = nrow == 1 ? v[0] : compose(v, nrow, ref_above(v, nrow)); x.said = 0; part[np++] = x; }
    if (tree && nrow && np < cap) {                                           /* the tree by head: pointers resolved, written nowhere */
        const char *key = s->tree_key_index ? s->key[s->tree_key_index - 1].name : NULL;
        TNode *tn = scratch_alloc(sizeof(TNode) * nrow); memset(tn, 0, sizeof(TNode) * nrow);
        int32_t *id = scratch_alloc(sizeof(int32_t) * nrow); int nw = 0, *at = scratch_alloc(sizeof(int) * nrow);
        for (uint32_t r = 0; r < nrow; r++) { int ok; int32_t x = key ? text_int(t, rows[r], key, &ok) : 0; if (!key || !ok) continue;   /* a range or an empty node: no word of the tree */
            Ref w, rel; if (!thing_of(k, t, rows[r], &w, 0)) continue; if (!field_of(k, t, rows[r], s->tree_rel, &rel)) rel = empty;
            w.said = 0; rel.said = 0; Ref p2[2] = { rel, w }; tn[nw].head = said_tuple(compose(p2, 2, ref_above(p2, 2))); tn[nw].first = tn[nw].last = -1; tn[nw].next = -1; id[nw] = x; at[nw] = (int)r; nw++; }
        int nroot = 0, roots[64];
        for (int i = 0; i < nw; i++) { int ok; int32_t h = text_int(t, rows[at[i]], s->tree_head, &ok); tn[i].parent = -2;
            if (ok && h == 0) { tn[i].parent = -1; if (nroot < 64) roots[nroot++] = i; continue; }
            for (int j = 0; ok && j < nw; j++) if (id[j] == h && j != i) { tn[i].parent = j; break; }
            if (tn[i].parent >= 0) { int p = tn[i].parent; if (tn[p].last < 0) tn[p].first = i; else tn[tn[p].last].next = i; tn[p].last = i; } }
        if (nroot) { Ref rt[64]; for (int i = 0; i < nroot; i++) { rt[i] = subtree(tn, roots[i], 0, nw); rt[i].said = 0; }
            Ref x = nroot == 1 ? rt[0] : compose(rt, (uint32_t)nroot, ref_above(rt, (size_t)nroot)); x.said = 0; part[np++] = x; } }
    scratch_restore(mark);
    return np;
}
static uint64_t claims_unplaced;                                              /* a claim no reference could be made of: in no record (said) */
/* A claim of a record as the vertex it is there: said to be a claim, said run times, with its outcome and position. */
static int claim_vertex(const Event *e, Ref *out){
    int ok; Ref c = ref_of_id(&e->claim, &ok); if (!ok) { __atomic_fetch_add(&claims_unplaced, 1, __ATOMIC_RELAXED); return 0; }
    c.said = LP_SAID_CLAIM; c.outcome = (uint8_t)lp_outcome_of((double)e->score, &c.spare); c.position = e->position; *out = c; return 1;
}
/* The records of a part read whole, made of what it said: one record a node of the tree that is a record (rec: the part
 * itself, or each part of a wide part, which says its own), in the order first said; the part's own record holds its
 * thing T, where it has one, first, and its layers and tree. Each record goes into the file's content tree. */
static void records_of(Sink *k, const SemanticRecord *t, int32_t root, uint64_t ev0, const Ref *T){
    uint64_t n = k->ev.n - ev0;
    size_t mark = scratch_mark();
    /* the records, in the order first said, and each event's: one pass over the events, an arena map of the record nodes */
    size_t mcap = 16; while (mcap <= (size_t)(n + 2) * 2) mcap *= 2;
    int32_t *mkey = scratch_alloc(sizeof(int32_t) * mcap);
    uint32_t *mval = scratch_alloc(sizeof(uint32_t) * mcap);
    for (size_t i = 0; i < mcap; i++) mkey[i] = -2;
    uint32_t *of = scratch_alloc(sizeof(uint32_t) * (n ? n : 1));
    int32_t *recs = scratch_alloc(sizeof(int32_t) * (n + 2));
    size_t nr = 0;
    for (uint64_t e = 0; e < n; e++) {
        int32_t r = k->ev.e[ev0 + e].rec;
        size_t s = ((uint64_t)(uint32_t)r * 0x9E3779B97F4A7C15ull) & (mcap - 1);
        while (mkey[s] != -2 && mkey[s] != r) s = (s + 1) & (mcap - 1);
        if (mkey[s] == -2) { mkey[s] = r; mval[s] = (uint32_t)nr; recs[nr++] = r; }
        of[e] = mval[s];
    }
    if (T) {
        size_t s = ((uint64_t)(uint32_t)root * 0x9E3779B97F4A7C15ull) & (mcap - 1);
        while (mkey[s] != -2 && mkey[s] != root) s = (s + 1) & (mcap - 1);
        if (mkey[s] == -2) { mkey[s] = root; mval[s] = (uint32_t)nr; recs[nr++] = root; }
    }
    /* the events of each record, in order: counted, then placed */
    uint64_t *at = scratch_alloc(sizeof(uint64_t) * (nr + 1)); memset(at, 0, sizeof(uint64_t) * (nr + 1));
    uint64_t *ord = scratch_alloc(sizeof(uint64_t) * (n ? n : 1));
    for (uint64_t e = 0; e < n; e++) at[of[e] + 1]++;
    for (size_t i = 0; i < nr; i++) at[i + 1] += at[i];
    { uint64_t *fill = scratch_alloc(sizeof(uint64_t) * (nr + 1)); memcpy(fill, at, sizeof(uint64_t) * (nr + 1)); for (uint64_t e = 0; e < n; e++) ord[fill[of[e]]++] = ev0 + e; }
    uint64_t most = 0; for (size_t i = 0; i < nr; i++) { uint64_t need = 24; for (uint64_t j = at[i]; j < at[i + 1]; j++) need += (k->ev.e[ord[j]].run ? k->ev.e[ord[j]].run : 1) + 1; if (need > most) most = need; }
    Ref *part = scratch_alloc(sizeof(Ref) * (most ? most : 24));              /* each claim said run times, and a voice before it */
    for (size_t ri = 0; ri < nr; ri++) { int32_t r = recs[ri]; uint32_t np = 0; int mine = r == root;
        int t_claim = 0;                                                      /* the thing is itself a claim it says: that claim's vertex stands for it */
        if (mine && T) for (uint64_t j = at[ri]; j < at[ri + 1] && !t_claim; j++) t_claim = !memcmp(&k->ev.e[ord[j]].claim, &T->id, 16);
        if (mine && T && !t_claim) { part[np] = *T; part[np].said = 0; part[np].outcome = 0; part[np].position = 0; part[np].spare = 0; np++; }
        if (mine) np += record_rows(k, t, root, part + np, 20);
        uint32_t nclaims = 0, ones = 0;
        for (uint64_t j = at[ri]; j < at[ri + 1]; j++) { const Event *x = &k->ev.e[ord[j]]; if (x->kind != EV_CLAIM) continue;
            Ref c; if (!claim_vertex(x, &c)) continue;
            if (x->own_witness) { int ok; Ref v = ref_of_id(&x->witness, &ok); if (ok) { v.said = LP_SAID_VOICE; v.outcome = 0; v.position = 0; v.spare = 0; part[np++] = v; } }
            uint32_t run = x->run ? x->run : 1; for (uint32_t z = 0; z < run; z++) part[np++] = c;    /* said run times: one vertex, the run in M */
            nclaims++; ones += run == 1 && !x->own_witness; }
        if (!np) continue;
        Ref R;
        if (np == 1 && nclaims == 1 && ones == 1) { R = part[0]; R.said = LP_SAID_RECORD; }      /* one claim said once: the claim, its own record */
        else if (np == 1 && !nclaims) { R = part[0]; R.said = 0; }                               /* only its thing: content that says nothing */
        else { R = compose(part, np, ref_above(part, np)); R.said = nclaims ? LP_SAID_RECORD : 0; R.outcome = 0; R.position = 0; R.spare = 0; }
        push(&k->things, &R); }
    scratch_restore(mark);
}
static void unit(void *sink, const SemanticRecord *t, int32_t root, uint64_t ordinal){
    size_t record_mark = scratch_mark();
    Sink *k = sink; const Say *s = k->s; k->kr.unit = ordinal; uint64_t ev0 = k->ev.n; k->score = 1.0f;
    sink_buffers(k);
    k->cs_epoch++; k->cs_n = 0; k->rec = root;                               /* a new part: none of its claims made yet, its own record */
    k->dm = scratch_alloc(sizeof(int16_t) * t->count); memset(k->dm, 0, sizeof(int16_t) * t->count);
    k->lo = scratch_alloc(t->count); memset(k->lo, 0, t->count); k->lo_tree = t;
    k->tc = scratch_alloc(sizeof(Ref) * t->count); k->ts = scratch_alloc(t->count); k->uk = scratch_alloc(t->count);
    k->tc_tree = t; k->tc_n = t->count; memset(k->ts, 0, t->count); memset(k->uk, 0, t->count);
    Tls previous = tls_save(t, k->dm, s, &k->bindings);
    index_keys(k, t);
    if (k->hw) { unit_highway(k, t, root); keep_keys(k, t, root); tls_back(&previous); scratch_restore(record_mark); return; }     /* read for the highway: its types, not what it attests */
    if (s->nline && t->n[root].kind == S_TEXT) {                             /* a line of a page: where it matches a pattern, it says the pattern's parts */
        const SemanticValue *x = &t->n[root]; size_t mark = scratch_mark(); char stack[4096], *ln = x->vlen < sizeof stack ? stack : scratch_alloc((size_t)x->vlen + 1); memcpy(ln, x->val, x->vlen); ln[x->vlen] = 0;
        for (int i = 0; i < s->nline; i++) { regmatch_t m[5]; if (regexec(&s->line[i].re, ln, 5, m, 0)) continue;
            Ref part[4]; int np = 0; for (int q = 1; q < 5 && np < 4; q++) if (m[q].rm_so >= 0 && m[q].rm_eo > m[q].rm_so) part[np++] = text_of((const uint8_t *)ln + m[q].rm_so, (size_t)(m[q].rm_eo - m[q].rm_so));
            /* a predicate the recipe writes, and a line's first part taken as the relation, are a heading's or a field's name:
             * an obligation, never a relation (Laplace-Engine#32) */
            if (s->line[i].mode == 3 && np == 2) note_obligation(k, 'o', &diagnostic_labels[DIAG_LINE_PREDICATE], (const uint8_t *)s->line[i].pred, s->line[i].pred_length, NULL, 0);
            else if (s->line[i].mode == 2 && np == 2) claim(k, part, 2);
            else if (s->line[i].mode == 1 && np >= 2) claim(k, part, np);
            else if (s->line[i].mode == 0 && np == 2 && k->has_fabout) note_obligation(k, 'o', &diagnostic_labels[DIAG_LINE], (const uint8_t *)ln + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so), NULL, 0); }
        scratch_restore(mark); }
    int speaks = spoken_of(s, t, root, s->whole);
    Pend pend[64]; int np_ = 0;      /* the parts whose claims are said together, still open */
    unit_range(k, t, 0, t->count, speaks, pend, &np_);
    if (speaks) for (uint32_t i = 0; i < t->count; i++) { const SemanticValue *x = &t->n[i];          /* what no line of the recipe reads is said to be left open */
        if (x->context || x->kind != S_VALUE || !x->nlen || dis_of(s, x) || !unmentioned(s, x)) continue;
        int32_t u = x->parent; while (u >= 0 && !dis_of(s, &t->n[u])) u = t->n[u].parent;          /* a part of a part the recipe reads is read with it (r1's A0 of A0>Agent) */
        if (u < 0) left_open(k, x); }

    if (speaks) for (uint32_t scope = 0; scope < t->count; scope++) if (t->n[scope].dropped_first) {
        int32_t ancestor = (int32_t)scope;
        while (ancestor >= 0 && !dis_of(s, &t->n[ancestor])) ancestor = t->n[ancestor].parent;
        if (ancestor >= 0) continue;
        for (uint32_t at = t->n[scope].dropped_first; at; at = t->dropped[at - 1].scope_next) {
            const DroppedField *field = &t->dropped[at - 1];
            if (!field->length || field->length > 62 || !s->said || mentioned(s->paths, field->name, field->length)) continue;
            DroppedField *mutable_field = &t->dropped[at - 1];
            if (field->all > field->seen) { open_name(k, field->name, field->length, field->all - field->seen); mutable_field->seen = field->all; }
        }
    }
    while (np_) { np_--; together(k, pend[np_].e0, pend[np_].has ? &pend[np_].about : NULL); }
    keep_keys(k, t, root);
    /* the part itself, whole, with what it says, into the file's content tree as its record (records_of). A part the
     * recipe does not speak of (a row its where leaves out) is in no tree: a recipe that says what a file's parts are
     * reads a curated source, mined for what it says and not kept byte for byte (Storage: curated sources are mined for
     * knowledge, not recorded bit-perfect) */
    Ref T; int whole = speaks ? entity_of(k, t, root, &T) : 0;
    records_of(k, t, root, ev0, whole ? &T : NULL);
    tls_back(&previous);
    scratch_restore(record_mark);
}

static uint32_t semantic_variable(SemanticRecord *record, const PathProgram *program, int32_t parent, const SField *field){
    if (record->count == UINT32_MAX) { fputs("too many semantic variables in a record\n", stderr); abort(); }
    if (record->count == record->capacity) {
        uint32_t capacity = record->capacity ? record->capacity * 2 : 64;
        if (capacity < record->capacity) { fputs("semantic record exceeds its arena\n", stderr); abort(); }
        SemanticValue *values = scratch_alloc((size_t)capacity * sizeof *values);
        if (record->count) memcpy(values, record->n, (size_t)record->count * sizeof *values);
        record->n = values; record->capacity = capacity;
    }
    uint32_t id = record->count++;
    unsigned tier = field->flags & S_FIELD_TIER ? field->tier : parent >= 0 ?
        record->n[parent].tier + ((field->flags & S_FIELD_SAME_TIER) ? 0 : 1) : 0;
    record->n[id] = (SemanticValue){.owner=record, .name=field->name, .val=field->value,
        .nlen=field->name_length, .vlen=field->value_length, .extent=id, .parent=parent,
        .kind=field->kind, .tier=(uint8_t)(tier < 255 ? tier : 255),
        .join=(uint8_t)((field->flags & S_FIELD_LIST) ? S_PIECES : (field->flags & S_FIELD_JOIN) != 0),
        .trim=(uint8_t)((field->flags & S_FIELD_TRIM) != 0), .context=(uint8_t)((field->flags & S_FIELD_CONTEXT) != 0)};
    if (parent >= 0) {
        record->n[id].operand = bindings_add(record->bindings, (uint32_t)parent, BIND_INPUTS, id);
        record->n[parent].arity++;
        bind_field_name(record->bindings, program, (uint32_t)parent, id, field->name, field->name_length, field->kind == S_NOTE);
    }
    return id;
}
static void dropped_rehash(SemanticRecord *record){
    uint32_t capacity = record->dropped_bucket_count ? record->dropped_bucket_count * 2 : 128;
    if (capacity < record->dropped_bucket_count) { fputs("diagnostic index exceeds its arena\n", stderr); abort(); }
    uint32_t *buckets = scratch_alloc((size_t)capacity * sizeof *buckets); memset(buckets, 0, (size_t)capacity * sizeof *buckets);
    for (uint32_t i = 0; i < record->dropped_count; i++) {
        DroppedField *field = &record->dropped[i]; size_t at = (field->hash ^ (field->hash >> 32)) & (capacity - 1);
        field->next = buckets[at]; buckets[at] = i + 1;
    }
    record->dropped_buckets = buckets; record->dropped_bucket_count = capacity;
}
static void drop_field(SemanticRecord *record, int32_t parent, const SField *source, const Layout *layout){
    if (parent < 0) { fputs("an unbound field has no semantic diagnostic scope\n", stderr); abort(); }
    if (!record->dropped_bucket_count || (record->dropped_count + 1) * 2 >= record->dropped_bucket_count) dropped_rehash(record);
    uint64_t hash = selector_hash(source->name, source->name_length) ^ ((uint64_t)(uint32_t)parent * UINT64_C(0x9e3779b97f4a7c15));
    size_t bucket = (hash ^ (hash >> 32)) & (record->dropped_bucket_count - 1);
    for (uint32_t at = record->dropped_buckets[bucket]; at; at = record->dropped[at - 1].next) {
        DroppedField *field = &record->dropped[at - 1];
        if (field->hash == hash && field->parent == (uint32_t)parent && field->length == source->name_length &&
            (!field->length || !memcmp(field->name, source->name, field->length))) {
            if (field->all == UINT32_MAX) { fputs("diagnostic count exceeds one record\n", stderr); abort(); }
            field->all++; field->nonempty += !s_empty(layout, source->value, source->value_length); return;
        }
    }
    if (record->dropped_count == record->dropped_capacity) {
        uint32_t capacity = record->dropped_capacity ? record->dropped_capacity * 2 : 32;
        if (capacity < record->dropped_capacity) { fputs("record diagnostics exceed the arena\n", stderr); abort(); }
        DroppedField *fields = scratch_alloc((size_t)capacity * sizeof *fields);
        if (record->dropped_count) memcpy(fields, record->dropped, (size_t)record->dropped_count * sizeof *fields);
        record->dropped = fields; record->dropped_capacity = capacity;
    }
    uint32_t id = record->dropped_count++;
    record->dropped[id] = (DroppedField){.name=source->name, .hash=hash, .length=source->name_length, .parent=(uint32_t)parent,
        .all=1, .nonempty=(uint32_t)!s_empty(layout, source->value, source->value_length),
        .next=record->dropped_buckets[bucket], .scope_next=record->n[parent].dropped_first};
    record->dropped_buckets[bucket] = id + 1; record->n[parent].dropped_first = id + 1;
}
static void text_fragment(TextRun *run, const uint8_t *bytes, uint32_t length){
    if (length > UINT32_MAX - run->length) { fputs("joined text exceeds the grammar byte limit\n", stderr); abort(); }
    TextFragment *fragment = scratch_alloc(sizeof *fragment); *fragment = (TextFragment){bytes, length, NULL};
    if (run->last) run->last->next = fragment; else run->first = fragment;
    run->last = fragment; run->length += length;
}
static void join_text(SemanticValue *value, const SField *field){
    if (!value->text) {
        value->text = scratch_alloc(sizeof *value->text); *value->text = (TextRun){0};
        text_fragment(value->text, value->val, value->vlen);
    }
    text_fragment(value->text, field->value, field->value_length);
    value->trim |= (uint8_t)((field->flags & S_FIELD_TRIM) != 0);
}
static void finish_text(Sink *sink, SemanticValue *value){
    (void)sink;
    if (value->kind == S_GROUP) return;
    if (value->text) {
        uint8_t *text = scratch_alloc(value->text->length); uint32_t used = 0;
        for (const TextFragment *part = value->text->first; part; part = part->next) {
            if (part->length) memcpy(text + used, part->bytes, part->length);
            used += part->length;
        }
        value->val = text; value->vlen = used; value->text = NULL;
    }
    if (value->trim) {
        while (value->vlen && (value->val[0] == ' ' || value->val[0] == '\t')) { value->val++; value->vlen--; }
        while (value->vlen && (value->val[value->vlen - 1] == ' ' || value->val[value->vlen - 1] == '\t' || value->val[value->vlen - 1] == '\r')) value->vlen--;
    }
    value->trim = 0;

}
typedef struct { uint32_t scope; } SemanticScope;
typedef struct {
    Sink *sink; const Layout *layout; SemanticRecord record;
    SemanticScope *scopes, *context_scopes; uint32_t depth, capacity, context_depth, context_capacity; uint64_t ordinal;
    uint8_t merged_root, parent_fixed;
} RecordCollector;
static void collector_root(RecordCollector *collector){
    if (collector->record.count) return;
    SField root = {.kind=S_GROUP};
    if (collector->layout->ntier) { root.name = (const uint8_t *)collector->layout->tier[0].name; root.name_length = collector->layout->g.root_name_length; }
    collector->record.bindings = &collector->sink->bindings;
    semantic_variable(&collector->record, collector->sink->s->paths, -1, &root);
}
static int collect_record(void *sink, int event, const SField *field){
    RecordCollector *collector = sink; Sink *output = collector->sink; collector_root(collector);
    SemanticRecord *record = &collector->record;
    if (field->flags & S_FIELD_CONTEXT) {
        if (event == S_CLOSE) {
            if (!collector->context_depth) return 0;
            uint32_t scope = collector->context_scopes[--collector->context_depth].scope;
            record->n[scope].extent = record->count - 1;
            Tls previous = tls_save(record, NULL, output->s, record->bindings);
            for (int32_t value = semantic_first(&record->n[scope]); value >= 0; value = semantic_next(&record->n[value])) finish_text(output, &record->n[value]);
            tls_back(&previous); return S_VISIT_OK;
        }
        int32_t parent = collector->context_depth ? (int32_t)collector->context_scopes[collector->context_depth - 1].scope : -1;
        if (event == S_FIELD && field->name_length &&
            !bind_field_name(NULL, output->s->paths, (uint32_t)parent, 0, field->name, field->name_length, field->kind == S_NOTE)) return S_VISIT_OK;
        uint32_t variable = semantic_variable(record, output->s->paths, parent, field);
        if (event == S_OPEN) {
            if (collector->context_depth == collector->context_capacity) {
                uint32_t capacity = collector->context_capacity ? collector->context_capacity * 2 : 8;
                if (capacity < collector->context_capacity) return 0;
                SemanticScope *scopes = scratch_alloc((size_t)capacity * sizeof *scopes);
                if (collector->context_depth) memcpy(scopes, collector->context_scopes, (size_t)collector->context_depth * sizeof *scopes);
                collector->context_scopes = scopes; collector->context_capacity = capacity;
            }
            collector->context_scopes[collector->context_depth++] = (SemanticScope){variable};
        }
        return S_VISIT_OK;
    }
    if (!collector->parent_fixed && event != S_FINISH) {
        if (collector->context_depth) record->n[0].parent = (int32_t)collector->context_scopes[collector->context_depth - 1].scope;
        collector->parent_fixed = 1;
    }
    if (event == S_FINISH) {
        if (collector->depth || collector->context_depth) return 0;
        record->n[0].extent = record->count - 1;
        Tls previous = tls_save(record, NULL, output->s, record->bindings);
        for (int32_t value = semantic_first(&record->n[0]); value >= 0; value = semantic_next(&record->n[value])) finish_text(output, &record->n[value]);
        if (record->n[0].kind != S_GROUP) finish_text(output, &record->n[0]);
        tls_back(&previous);
        unit(output, record, 0, collector->ordinal);
        return S_VISIT_OK;
    }
    if (event == S_CLOSE) {
        if (!collector->depth) return 0;
        uint32_t scope = collector->scopes[--collector->depth].scope;
        Tls previous = tls_save(record, NULL, output->s, record->bindings);
        for (int32_t value = semantic_first(&record->n[scope]); value >= 0; value = semantic_next(&record->n[value])) finish_text(output, &record->n[value]);
        tls_back(&previous);
        record->n[scope].extent = record->count - 1;
        return S_VISIT_OK;
    }
    int32_t parent = collector->depth ? (int32_t)collector->scopes[collector->depth - 1].scope : 0;
    int merge_root = !collector->merged_root && !collector->depth && record->count == 1 &&
                     (field->flags & S_FIELD_TIER) && !field->tier;
    uint32_t variable;
    if (merge_root) {
        SemanticValue *root = &record->n[0];
        root->name = field->name; root->nlen = field->name_length; root->val = field->value; root->vlen = field->value_length;
        root->kind = field->kind; root->join = (uint8_t)((field->flags & S_FIELD_LIST) ? S_PIECES : (field->flags & S_FIELD_JOIN) != 0);
        root->trim = (uint8_t)((field->flags & S_FIELD_TRIM) != 0); variable = 0; collector->merged_root = 1;
    } else if (event == S_FIELD && field->kind == S_TEXT && (field->flags & S_FIELD_JOIN)) {
        int32_t previous = semantic_last(&record->n[parent]);
        if (previous >= 0 && record->n[previous].kind == S_TEXT && record->n[previous].join == 1) {
            join_text(&record->n[previous], field); return S_VISIT_OK;
        }
        variable = semantic_variable(record, output->s->paths, parent, field);
    } else {
        int required = event == S_OPEN || field->kind != S_VALUE || !field->name_length ||
            (field->flags & S_FIELD_SAME_TIER) ||
            bind_field_name(NULL, output->s->paths, (uint32_t)parent, 0, field->name, field->name_length, field->kind == S_NOTE);
        if (!required) { drop_field(record, parent, field, collector->layout); return S_VISIT_OK; }
        variable = semantic_variable(record, output->s->paths, parent, field);
    }
    if (parent == 0 && variable && record->n[0].parent >= 0) {
        /* The synthetic reduction root owns composition operands, but it is
         * not an ancestor in the source grammar. Parent selectors see the
         * actual inherited scope directly. */
        record->n[variable].parent = record->n[0].parent;
        bind_field_name(record->bindings, output->s->paths, (uint32_t)record->n[0].parent, variable,
                        field->name, field->name_length, field->kind == S_NOTE);
    }
    if (event == S_OPEN) {
        if (collector->depth == collector->capacity) {
            uint32_t capacity = collector->capacity ? collector->capacity * 2 : 32;
            if (capacity < collector->capacity) return 0;
            SemanticScope *scopes = scratch_alloc((size_t)capacity * sizeof *scopes);
            if (collector->depth) memcpy(scopes, collector->scopes, (size_t)collector->depth * sizeof *scopes);
            collector->scopes = scopes; collector->capacity = capacity;
        }
        collector->scopes[collector->depth++] = (SemanticScope){variable};
        Tls previous = tls_save(record, NULL, output->s, record->bindings);
        const Dis *rule = dispatch_dis(output->s, output->s->dis, output->s->ndis, &record->n[variable], 0);
        int skip = rule && rule->what == D_OMIT;
        tls_back(&previous);
        if (skip) return S_VISIT_OK | S_VISIT_SKIP;
    }
    return S_VISIT_OK;
}

uint64_t say_unplaced(void){ return __atomic_load_n(&claims_unplaced, __ATOMIC_RELAXED); }
/* What a file holds, in its order, as one composition. A few thousand parts are one path. More are factored into
 * blocks from the content alone (Storage: repeated blocks are factored from the content alone): a block ends after a
 * part whose own ID says so (one in BLOCK, by its bits), never at a count or a position, so the same parts always part
 * the same way, a part put in or taken out moves only its own block, and a run of parts that repeats is one block
 * wherever it stands. The blocks are composed the same way, level by level, until one holds them all: no path is
 * longer than a row can hold, and a file of millions of records is a tree of its own content. */
#define BLOCK 4096
/* A block holding records, or blocks that hold them, is said to hold them (LP_SAID_HOLDS): a walk down from a trunk
 * to what its records say goes through it, and past content that says nothing. A part standing alone in its block keeps
 * what it is said to be, and how. */
static int any_records(const Ref *r, size_t n){ for (size_t i = 0; i < n; i++) if (r[i].said == LP_SAID_RECORD || r[i].said == LP_SAID_HOLDS) return 1; return 0; }
static Ref block(Ref *r, size_t n){
    if (n == 1) { Ref x = r[0]; if (x.said != LP_SAID_TUPLE && x.said != LP_SAID_CLAIM && x.said != LP_SAID_RECORD && x.said != LP_SAID_HOLDS) x.said = 0; return x; }
    Ref x = compose(r, (uint32_t)n, ref_above(r, n)); x.said = any_records(r, n) ? LP_SAID_HOLDS : 0; return x;
}
static Ref blocks_of(Ref *r, size_t n){
    if (n == 1) return r[0];
    if (n <= BLOCK) return block(r, n);
    Ref *up = malloc(sizeof(Ref) * n); size_t m = 0, from = 0;
    for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, r[i].id.b + 8, 8);
        if ((b & (BLOCK - 1)) == 0 || i == n - 1) { up[m++] = block(r + from, i - from + 1); from = i + 1; } }
    Ref out = m == n ? block(r, n) : blocks_of(up, m); free(up); return out;
}
/* The OS's record of a file, as parts named as the OS names them: its pathname and filename (POSIX), and what statx
 * returns. Each is disposed of by the file's recipe, else by the stock recipe file, as any part is: metadata, a
 * [name, value] in the file's metadata tree; omit, nowhere; neither, an obligation left open and said once. A pathname's
 * value is its filenames, one composition: a path is a Merkle DAG of its folders. */
static const Recipe *stock_file;
void file_record_stock(const Recipe *r){ stock_file = r; }
int say_only_disposes(const Recipe *r){ const Say *s = r->say; return s && s->ndis && !s->nthing && !s->natt && !s->nrel && !s->nhw && !s->npair && !s->nhold && !s->nkey; }
static const Dis *dis_named(const Say *s, const char *name){
    if (!s) return NULL; SemanticValue x; memset(&x, 0, sizeof x); x.kind = S_VALUE; x.name = (const uint8_t *)name; x.nlen = (uint32_t)strlen(name); x.parent = -1;
    const SemanticRecord *was = TT; TT = NULL; const Dis *d = match_dis(s, &x); TT = was; return d;
}
static uint64_t record_open;                                                  /* the parts no recipe disposed of, said once */
/* Whether a part of the OS's record is kept in the metadata tree. Asked before its value is made: a value is composed
 * into the node table as it is made, and one the recipe omits (the access time, which reading the file changes) would
 * leave nodes of its own there, different on every read. */
static int record_kept(const Recipe *r, int k, const char *name){
    const Dis *d = dis_named(r ? r->say : NULL, name); if (!d && stock_file) d = dis_named(stock_file->say, name);
    if (!d) { if (!(__atomic_fetch_or(&record_open, 1ull << k, __ATOMIC_RELAXED) & (1ull << k))) fprintf(stderr, "  the OS's record of a file: %s is disposed of by no recipe (metadata or omit, in the file recipe)\n", name); return 0; }
    return d->what == D_METADATA;
}
static void record_part(const Recipe *r, Ref *out, size_t *n, size_t cap, int k, const char *name, Ref value){
    (void)r; (void)k; if (*n >= cap) return;
    Ref p[2] = { string_ref((const uint8_t *)name, strlen(name)), value }; p[0].said = p[1].said = 0;
    out[(*n)++] = said_tuple(compose(p, 2, ref_above(p, 2)));
}
static size_t fmt_u64(uint64_t v, char *out){
    char tmp[24]; int k = 0;
    do { tmp[k++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    return (size_t)k;
}
static Ref number_ref(uint64_t v){ char b[24]; size_t l = fmt_u64(v, b); return string_ref((const uint8_t *)b, l); }
static Ref when_ref(const struct statx_timestamp *t){
    char b[40]; size_t k = 0; long long sec = (long long)t->tv_sec;
    if (sec < 0) { b[k++] = '-'; sec = -sec; }
    k += fmt_u64((uint64_t)sec, b + k);
    b[k++] = '.';
    uint32_t ns = t->tv_nsec;
    for (int i = 8; i >= 0; i--) { b[k + (size_t)i] = (char)('0' + (ns % 10)); ns /= 10; }
    k += 9;
    return string_ref((const uint8_t *)b, k);
}
size_t file_record(const Recipe *r, const File *f, Ref *out, size_t cap){
    size_t n = 0; int k = 0; Ref seg[256]; uint32_t ns = 0; const char *p = f->path;
    while (*p && ns < 256) { const char *e = strchr(p, '/'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l) { seg[ns] = string_ref((const uint8_t *)p, l); seg[ns].said = 0; ns++; } p += l; if (*p == '/') p++; }
    if (ns) { if (record_kept(r, k, "pathname")) { Ref path = compose(seg, ns, ref_above(seg, ns)); path.said = 0; record_part(r, out, &n, cap, k, "pathname", path); } k++;
              if (record_kept(r, k, "filename")) record_part(r, out, &n, cap, k, "filename", seg[ns - 1]); k++; }
    struct statx x; if (statx(AT_FDCWD, f->path, 0, STATX_BASIC_STATS | STATX_BTIME, &x)) return n;
    #define NUM(F, M) do { if ((!(M) || (x.stx_mask & (M))) && record_kept(r, k, #F)) record_part(r, out, &n, cap, k, #F, number_ref((uint64_t)x.F)); k++; } while (0)
    #define WHEN(F, M) do { if ((x.stx_mask & (M)) && record_kept(r, k, #F)) record_part(r, out, &n, cap, k, #F, when_ref(&x.F)); k++; } while (0)
    NUM(stx_mode, STATX_MODE); NUM(stx_uid, STATX_UID); NUM(stx_gid, STATX_GID); NUM(stx_nlink, STATX_NLINK); NUM(stx_ino, STATX_INO);
    NUM(stx_size, STATX_SIZE); NUM(stx_blocks, STATX_BLOCKS); NUM(stx_blksize, 0); NUM(stx_attributes, 0);
    NUM(stx_dev_major, 0); NUM(stx_dev_minor, 0); NUM(stx_rdev_major, 0); NUM(stx_rdev_minor, 0);
    WHEN(stx_atime, STATX_ATIME); WHEN(stx_btime, STATX_BTIME); WHEN(stx_ctime, STATX_CTIME); WHEN(stx_mtime, STATX_MTIME);
    #undef NUM
    #undef WHEN
    return n;
}
/* The OS's record of a file as one node, the first part of its metadata tree: computed from stat alone, before a byte
 * of the file is read, so a file whose trunk is recorded is found by it (ingest.c, files_recorded). 0: none. */
int file_os(const Recipe *r, const File *f, Ref *out){
    Ref m[32]; size_t n = file_record(r, f, m, 32); if (!n) return 0;
    *out = compose(m, (uint32_t)n, ref_above(m, n)); out->said = 0; return 1;
}
/* The file, read: every part of its outermost tier on every core, joined in the file's order; then its trunk, over
 * its metadata tree and its content tree. */

enum { RECORD_WINDOW = 1024, RECORDS_PER_LANE = 16, MAX_RECORD_LANES = RECORD_WINDOW / RECORDS_PER_LANE };
typedef struct {
    const Recipe *recipe; File *file; const Say *say; const uint8_t *source; size_t length;
    Sink *result, *workers; uint32_t worker_count;
    Ref filename, stem, about; int has_about, failed;
} RecordPipeline;
static void merge_record_output(Sink *result, const Sink *worker){
    for (uint64_t i = 0; i < worker->ev.n; i++) sink_event(result, &worker->ev.e[i]);
    for (size_t i = 0; i < worker->meta.n; i++) push(&result->meta, &worker->meta.c[i]);
    for (size_t i = 0; i < worker->things.n; i++) push(&result->things, &worker->things.c[i]);
    for (size_t i = 0; i < worker->nhr; i++) {
        const HwRec *record = &worker->hr[i];
        hw_rec(result, record->what, record->line, &record->thing,
               record->a, record->a ? strlen(record->a) : 0, record->b, record->b ? strlen(record->b) : 0);
    }
    for (int i = 0; i < 256; i++) result->open[i] += worker->open[i];
    for (int i = 0; i < worker->nopen; i++) {
        int duplicate = 0;
        for (int j = 0; j < result->nopen && !duplicate; j++)
            duplicate = result->open_length[j] == worker->open_length[i] &&
                !memcmp(result->opennm[j], worker->opennm[i], worker->open_length[i]);
        if (!duplicate && result->nopen < 16) {
            memcpy(result->opennm[result->nopen], worker->opennm[i], sizeof result->opennm[0]);
            result->open_length[result->nopen++] = worker->open_length[i];
        }
    }
    result->unknown += worker->unknown;
    for (int i = 0; i < worker->nunk && result->nunk < 8; i++)
        memcpy(result->unknm[result->nunk++], worker->unknm[i], sizeof result->unknm[0]);
    result->obl_more += worker->obl_more;
    for (int i = 0; i < worker->nobl; i++) {
        const Obligation *entry = &worker->obl[i];
        add_obligation(result, entry->what, entry->label, entry->a, entry->a_length,
                       entry->has_b ? entry->b : NULL, entry->b_length, entry->count);
    }
}
static void reset_record_worker(RecordPipeline *pipeline, Sink *worker){
    SayScratch arena = worker->output; arena.used = arena.stack_used = 0;
    *worker = (Sink){.r=pipeline->recipe, .s=pipeline->say, .er=1500.0f, .ed=0.0f,
        .fw=pipeline->file->witness, .fname=pipeline->filename, .fstem=pipeline->stem,
        .fabout=pipeline->about, .has_fabout=pipeline->has_about, .hw=pipeline->result->hw, .output=arena};
    sink_buffers(worker);
}
static void page_about(RecordPipeline *pipeline, const SRecords *records){
    if (!pipeline->say->has_about) return;
    SRecords view = *records;
    view.tree = NULL; view.file_layout = NULL; view.started = view.done = view.failed = view.header_pending = view.descend = 0; view.ordinal = 0;
    view.cursor = lp_tls_cursor(ts_tree_root_node(records->tree));
    if (!view.cursor) { pipeline->failed = 1; return; }
    SRecord record;
    while (!pipeline->has_about && s_record_next(&view, &record)) {
        uint32_t length = record.range.end_byte - record.range.start_byte;
        if (!length || length >= 4096) continue;
        char text[4096]; memcpy(text, pipeline->source + record.range.start_byte, length); text[length] = 0;
        regmatch_t match[2];
        if (!regexec(&pipeline->say->about_re, text, 2, match, 0) && match[1].rm_so >= 0 && match[1].rm_eo > match[1].rm_so) {
            pipeline->about = string_ref((const uint8_t *)text + match[1].rm_so, (size_t)(match[1].rm_eo - match[1].rm_so));
            pipeline->has_about = 1;
        }
    }
    if (view.failed) pipeline->failed = 1;
    s_records_close(&view);
}
static void read_records(RecordPipeline *pipeline){
    SRecords records;
    if (!s_records_open(&records, &pipeline->say->lay, pipeline->source, pipeline->length)) {
        fprintf(stderr, "\n  %s: %s does not parse completely with its compiled packaging grammar\n", pipeline->recipe->name, pipeline->file->path);
        pipeline->failed = 1; return;
    }
    page_about(pipeline, &records);
    SRecord batch[RECORD_WINDOW];
    while (!pipeline->failed) {
        uint32_t count = 0;
        while (count < RECORD_WINDOW && s_record_next(&records, &batch[count])) count++;
        if (records.failed) { pipeline->failed = 1; break; }
        if (!count) break;
        uint32_t lanes = (count + RECORDS_PER_LANE - 1) / RECORDS_PER_LANE;
        if (lanes > pipeline->worker_count) lanes = pipeline->worker_count;
        const Layout *layout = records.layout;
        #pragma omp taskloop grainsize(1) shared(batch)
        for (uint32_t lane = 0; lane < lanes; lane++) {
            Sink *worker = &pipeline->workers[lane]; reset_record_worker(pipeline, worker);
            uint32_t begin = (uint32_t)((uint64_t)count * lane / lanes), end = (uint32_t)((uint64_t)count * (lane + 1) / lanes);
            for (uint32_t i = begin; i < end && !__atomic_load_n(&pipeline->failed, __ATOMIC_ACQUIRE); i++) {
                size_t mark = scratch_mark(); worker->bindings = (BindingMatrix){0};
                worker->kr.file = pipeline->file->order; worker->kr.at = pipeline->file->at + batch[i].range.start_byte;
                RecordCollector collector = {.sink=worker, .layout=layout, .ordinal=batch[i].ordinal};
                int valid = s_cursor_record(layout, &batch[i], pipeline->source, pipeline->length, collect_record, &collector);
                scratch_restore(mark);
                if (!valid) __atomic_store_n(&pipeline->failed, 1, __ATOMIC_RELEASE);
            }
        }
        if (pipeline->failed) break;
        /* The taskgroup has joined: append complete lane results in source
         * order before reusing their arenas for the next bounded window. */
        for (uint32_t lane = 0; lane < lanes; lane++) merge_record_output(pipeline->result, &pipeline->workers[lane]);
    }
    s_records_close(&records);
}

static void read_laid(const Recipe *r, File *f, const uint8_t *src, size_t n, Hw *hw){
    Say *sy = xrealloc(NULL, sizeof *sy); *sy = *(const Say *)r->say;      /* the recipe's sayings, a copy for this file: on the heap, as a Say is larger than a thread's stack holds */
    if (!sy->lay.g.compiled || sy->lay.g.unresolved) {
        fprintf(stderr, "%s: semantic grammar was not compiled before record execution\n", r->name); free(sy); exit(2);
    }
    const char *base = strrchr(f->path, '/'); base = base ? base + 1 : f->path; size_t bl = strlen(base); if (bl > 3 && !strcmp(base + bl - 3, ".gz")) bl -= 3;
    const char *sb = base; if (sy->stemfrom) { const char *f0 = NULL; for (const char *c = base; c < base + bl; c++) if (*c == sy->stemfrom) f0 = c; if (f0) sb = f0 + 1; }
    const char *dot = memchr(sb, sy->stem ? sy->stem : '.', (size_t)(base + bl - sb)); Ref fname = string_ref((const uint8_t *)base, bl), fstem = string_ref((const uint8_t *)sb, dot ? (size_t)(dot - sb) : (size_t)(base + bl - sb));    /* the file's own name, and the part of it {file} stands for */

    uint32_t workers = (uint32_t)omp_get_max_threads(); if (!workers) workers = 1;
    if (workers > MAX_RECORD_LANES) workers = MAX_RECORD_LANES;
    size_t nc = 1; Sink *part = calloc(1, sizeof *part), *lanes = calloc(workers, sizeof *lanes);
    if (!part || !lanes) { perror("record workers"); free(part); free(lanes); free(sy); exit(2); }
    part[0].r = r; part[0].s = sy; part[0].hw = hw; sink_buffers(&part[0]);
    RecordPipeline pipeline = {.recipe=r, .file=f, .say=sy, .source=src, .length=n,
        .result=&part[0], .workers=lanes, .worker_count=workers, .filename=fname, .stem=fstem};
    if (omp_in_parallel()) read_records(&pipeline);
    else {
        #pragma omp parallel
        {
            #pragma omp master
            read_records(&pipeline);
        }
    }
    release_sinks(lanes, workers);
    if (pipeline.failed) { f->incomplete++; release_sinks(part, nc); free(sy); exit(2); }
    Refs things = {.arena=&part[0].output}, meta = {.arena=&part[0].output}; uint64_t open = 0;
    for (size_t i = 0; i < nc; i++) {
        for (uint64_t j = 0; j < part[i].ev.n; j++) ev_push(&f->ev, &part[i].ev.e[j]);
        for (size_t j = 0; j < part[i].things.n; j++) push(&things, &part[i].things.c[j]);
        for (size_t j = 0; j < part[i].meta.n; j++) push(&meta, &part[i].meta.c[j]);
        open += part[i].open[0]; keep_obligations(r->name, &part[i]);
    }
    if (open) { fprintf(stderr, "\n  %s: %llu named parts of %s have no disposition in the recipe: they are left open, not read:", r->name, (unsigned long long)open, f->path);
        char seen[64][64]; int ns = 0;
        for (size_t i = 0; i < nc; i++) for (int j = 0; j < part[i].nopen; j++) { int dup = 0; for (int z = 0; z < ns && !dup; z++) dup = !strcmp(seen[z], part[i].opennm[j]); if (dup || ns == 64) continue; memcpy(seen[ns++], part[i].opennm[j], 64); fprintf(stderr, " %s", part[i].opennm[j]); }
        fputc('\n', stderr); f->incomplete += open; }
    { uint64_t unk = 0; for (size_t i = 0; i < nc; i++) unk += part[i].unknown;
      if (unk) { fprintf(stderr, "\n  %s: %llu keys in %s name a type no list of the highway holds, and say nothing; for instance:", r->name, (unsigned long long)unk, f->path);
          for (size_t i = 0, shown = 0; i < nc && shown < 3; i++) for (int j = 0; j < part[i].nunk && shown < 3; j++, shown++) fprintf(stderr, " [%s]", part[i].unknm[j]); fputc('\n', stderr); } }
    if (hw) {                                                                /* for the highway: what the file says of its types, in the file's order */
        for (size_t i = 0; i < nc; i++) { for (size_t j = 0; j < part[i].nhr; j++) { HwRec *x = &part[i].hr[j]; const HwLine *h = &sy->hw[x->line];
                if (x->what == 1) hw_type(hw, h->a.list, h->say, x->thing);
                else if (x->what == 2) hw_key(hw, h->a.list, x->thing, x->a);
                else if (x->what == 3) hw_alias(hw, h->a.list, x->a, x->b);
                else hw_edge(hw, h->a.list, x->a, h->b.list, x->b);
            }
        }
        release_sinks(part, nc); free(sy); return; }
    /* the trunk: [metadata, content]. The metadata tree: the file's name, and what its parts say of the file itself.
     * The content tree: the things of its outermost tier, in the file's order. */
    /* A file read a stretch at a time gathers what each stretch held, and its trunk is made when the last is read. */
    f->laid = 1; f->has_file = 0;
    { uint64_t need = f->nsaid + things.n + meta.n; if (need > f->csaid) { f->csaid = need * 2 + 1024; f->said = xrealloc(f->said, f->csaid * sizeof(Ref)); }
      for (size_t j = 0; j < meta.n; j++) { Ref x = meta.c[j]; x.said = LP_SAID_METADATA; f->said[f->nsaid++] = x; }
      for (size_t j = 0; j < things.n; j++) { Ref x = things.c[j]; if (x.said == LP_SAID_METADATA) x.said = 0; f->said[f->nsaid++] = x; } }
    if (f->partial) { release_sinks(part, nc); free(sy); return; }
    things.n = meta.n = 0;
    for (uint64_t j = 0; j < f->nsaid; j++) { Ref x = f->said[j]; if (x.said == LP_SAID_METADATA) { x.said = LP_SAID_TUPLE; push(&meta, &x); } else push(&things, &x); }
    free(f->said); f->said = NULL; f->nsaid = f->csaid = 0;
    Ref *m = malloc(sizeof(Ref) * (meta.n + 1)); size_t nr = (size_t)file_os(r, f, m);          /* the OS's record of it, one node, first */
    memcpy(m + nr, meta.c, sizeof(Ref) * meta.n);
    size_t nm = nr + meta.n; Ref metadata; memset(&metadata, 0, sizeof metadata); if (nm) metadata = blocks_of(m, nm); free(m);
    if (things.n) { Ref content = blocks_of(things.c, things.n); if (content.said != LP_SAID_HOLDS && content.said != LP_SAID_RECORD) content.said = 0;     /* what holds records says so in the file's path */
        if (nm) { Ref two[2] = { said_metadata(metadata), content }; f->file = compose(two, 2, ref_above(two, 2)); } else f->file = content;
        if (nm) { f->file.said = 0; f->file.outcome = 0; f->file.position = 0; f->file.spare = 0; }   /* a file with no metadata is its content: it says in its source's trunk what its content is (records, or a record) */
        f->has_file = 1; f->trunk = content; }
    else if (f->ev.n && nm) { Ref two[1] = { said_metadata(metadata) }; f->file = two[0]; f->file.said = 0; f->has_file = 0; }
    release_sinks(part, nc); free(sy);
}
void attest_layout(const Recipe *r, File *f, const uint8_t *src, size_t n){ read_laid(r, f, src, n, NULL); }
void say_highway(const Recipe *r, File *f, const uint8_t *src, size_t n, Hw *hw){ read_laid(r, f, src, n, hw); }
int  say_has_highway(const Recipe *r){ const Say *s = r->say; return s && s->nhw; }
void say_highway_lists(const Recipe *r, Hw *hw){
    const Say *s = r->say; if (!s) return;
    for (int i = 0; i < s->nhw; i++) {
        const HwLine *h = &s->hw[i];
        if (h->what == 1) hw_list(hw, h->a.list, h->say);
        else if (h->what == 2 || h->what == 3) hw_list(hw, h->a.list, NULL);
        else if (h->what == 4) { hw_list(hw, h->a.list, NULL); hw_list(hw, h->b.list, NULL); }
    }
}
/* What the recipe says a part states (relation "TEXT" NAME): TEXT, or NULL where it declares no relation of it. */
const char *say_relation(const Recipe *r, const char *name){
    const Say *s = r ? r->say : NULL; if (!s) return NULL;
    for (int i = 0; i < s->nval; i++) if (s->val[i].what == 1 && !strcmp(s->val[i].name, name)) return s->val[i].text;
    return NULL;
}
/* The first way the recipe names a TIER by parts of which two are A and B (each part by the last step of its path): how
 * many parts it names it by, and where A and B stand among them; -1 where no way does. */
int say_thing_parts(const Recipe *r, const char *tier, const char *a, const char *b, int *ia, int *ib){
    const Say *s = r ? r->say : NULL; if (!s) return -1;
    for (int i = 0; i < s->nthing; i++) { const Thing *th = &s->thing[i]; if (strcmp(th->tier, tier) || th->of || th->span || th->within || th->infile) continue;
        int x = -1, y = -1;
        for (int z = 0; z < th->n; z++) { if (th->lit[z]) continue; const char *nm = th->name[z], *sl = strrchr(nm, '/'), *last = sl ? sl + 1 : nm + (*nm == '>' || *nm == '^');
            if (x < 0 && !strcmp(last, a)) x = z; else if (y < 0 && !strcmp(last, b)) y = z; }
        if (x >= 0 && y >= 0) { *ia = x; *ib = y; return th->n; } }
    return -1;
}
