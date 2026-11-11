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
 *   games TIER NAME            the number of games the TIER gives what it attests: the part is a count of times the source observed it, a
 *                              whole number of one or more, and is no claim's value
 *   dependence TIER CLASS      observations in this TIER use the named native trust class's canonical dependence
 *                              root and prior; the corpus trunk remains the witness. Applies under when/always.
 *   where NAME is VALUE | is-not VALUE | matches PATTERN
 *   admit references NAME...  admit only records whose external keys belong to the exported provider domain
 *                              the outermost parts the recipe speaks of; another is in the file's tree and attests nothing
 *   when NAME is VALUE | is-not VALUE | matches PATTERN
 *                              the attest, pair and relate lines after it are said only of the parts that meet it
 *                              (several are met together), up to a line "always"
 *   content NAME...            the part is content, the text it is, the same entity wherever that text stands
 *   default FIELD VALUE        suppress testimony for the field's declared default, retaining its packaging
 *   unless_equal FIELD OTHER   suppress a scalar field equal to its sibling; multi-value sets stay distinct
 *   key TIER NAME              the part is the file's key for a TIER: it resolves to the TIER's thing; written nowhere
 *   refer NAME TIER            the part's text is a key of a TIER: it is read as that TIER's thing
 *   index FIELD in LIST [matching PATTERN] [base 0|1]
 *                              a record-local numeric pointer selects a parsed list member; invalid, negative and
 *                              out-of-range pointers reject admission instead of naming a fabricated value
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
 *   relate TIER NAME to NAME [of PATH]
 *                              of a TIER's thing (or the explicit subject PATH), its relation (the first part's value, which value names: a relation
 *                              of the source's own inventory) to what the second part is: [thing, relation, other];
 *                              to . names the whole TIER thing; where the second names nothing, the pair [thing, relation]
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
#include <blake3.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <tree_sitter/api.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#include "recipe_program.h"

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
int recipe_program_mentions(const PathProgram *program, const uint8_t *bytes, uint32_t length){
    if (!program->mention_capacity || !length || length > 62) return 0;
    uint64_t hash = selector_hash(bytes, length); size_t at = mention_bucket(hash, program->mention_capacity);
    while (program->mentions[at].length) {
        const Mention *entry = &program->mentions[at];
        if (entry->hash == hash && entry->length == length && !memcmp(entry->bytes, bytes, length)) return 1;
        at = (at + 1) & (program->mention_capacity - 1);
    }
    return 0;
}
static NameCode compile_name(const char *bytes, size_t length){
    NameCode code = {.bytes=bytes, .length=(uint32_t)length};
    if (length >= 5 && !memcmp(bytes, "note:", 5)) { code.note = 1; code.bytes += 5; code.length -= 5; }
    if (code.length && code.bytes[code.length - 1] == '*') { code.wildcard = 1; code.length--; }
    return code;
}
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
    PathCode *code = calloc(1, sizeof *code); if (!code) { perror("selector program"); abort(); }
    size_t length = strlen(name); code->whole = compile_name(name, length); code->first = compile_steps(name, length, NULL); code->source_length = (uint16_t)length;
    code->special = length == 1 && name[0] == '.' ? SELECT_SELF : length == 6 && !memcmp(name, "{file}", 6) ? SELECT_FILE :
                    length == 7 && !memcmp(name, "{field}", 7) ? SELECT_LABEL :
                    length == 7 && !memcmp(name, "{first}", 7) ? SELECT_SURFACE_FIRST :
                    length == 6 && !memcmp(name, "{last}", 6) ? SELECT_SURFACE_LAST :
                    length == 2 && !memcmp(name, "*/", 2) ? SELECT_ELEMENTS : length == 3 && !memcmp(name, "...", 3) ? SELECT_UNNAMED : SELECT_FIELD;
    path_insert(program, (PathEntry){offset, code});
    if (*name == '>') path_register(say, name + 1);
}
static void register_att_paths(Say *say, const Att *att, int count){
    for (int i = 0; i < count; i++) {
        path_register(say, att[i].tier); path_register(say, att[i].with); path_register(say, att[i].of);
        for (int j = 0; j < att[i].n; j++) path_register(say, att[i].name[j]);
    }
}
static void compile_targets(Say *say, Dis *rules, int count, const char *recipe){
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
            if ((size_t)(next-at)>=sizeof target->name) { fprintf(stderr,"%s: reference/type target exceeds %zu bytes\n",recipe,sizeof target->name-1);exit(2); }
            target->length = (uint16_t)(next - at); memcpy(target->name, at, target->length); target->name[target->length] = 0;
            for (int j = 0; j < say->nkey; j++) if (!strcmp(say->key[j].tier, target->name)) {
                target->local = 1; target->tier_id = say->key[j].tier_id;
                if (say->key[j].scope[0]) target->scope_key=(uint16_t)(j+1);
                const Pair *key=&say->key[j];
                /* An identifier which IS its declared key can be constructed
                 * from an observed reference. No invented definition or corpus
                 * lookup is needed. Compound lexical identities cannot. */
                if (!target->within && !key->scope[0] && !key->key_radix && !rule->key_radix && !strcmp(key->under,rule->under)) {
                    char qualified[100]; snprintf(qualified,sizeof qualified,"%s.%s",key->tier,key->name);
                    const Dis *value=NULL; int conditional=0;
                    for (int z=0; z<say->ndis; z++) if (!strcmp(say->dis[z].name,qualified) || !strcmp(say->dis[z].name,key->name)) {
                        if (say->dis[z].when) conditional=1;
                        if (!value || !strcmp(say->dis[z].name,qualified)) value=&say->dis[z];
                    }
                    int literal=value && !conditional && (value->what==D_KEY || value->what==D_CONTENT ||
                        (value->what==D_TYPE && value->k.node && !value->k.vocabulary && !value->k.crosswalk[0] && !value->k.has_re && !value->k.nas));
                    for (int z=0; literal && z<say->nthing; z++) {
                        const Thing *thing=&say->thing[z];
                        if (strcmp(thing->tier,key->tier)) continue;
                        if (thing->within || thing->infile || thing->span || thing->of || thing->preserve || say->surface.thing==z+1) break;
                        int scoped=!!key->under[0], n=scoped?2:1, eligible=thing->n==n && (!scoped || thing->resolved);
                        for (int p=0; eligible && p<n; p++) eligible=!thing->lit[p] && !thing->space[p] && !thing->form[p].has_re && !thing->form[p].nas;
                        if (eligible && !strcmp(thing->name[scoped],key->name) && (!scoped || !strcmp(thing->name[0],key->under))) {
                            target->constructor=(uint8_t)(scoped?2:1);
                            target->constructor_value=value;
                        }
                        break; /* the tier's first identity rule owns its constructor */
                    }
                }
                break;
            }
            int space=(target->local && target->within ? KEY_PARENT : 0) | (rule->under[0] ? KEY_CONTENT_SCOPE : 0) | (rule->key_radix ? KEY_NUMERIC : 0);
            keys_domain(target->local ? recipe : target->name, target->local ? target->name : "", space, &target->domain);
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
    for (size_t i = 0; i < program->capacity; i++) if (program->entries[i].offset) {
        PathCode *path = program->entries[i].code; intern_name(program, &path->whole); intern_steps(program, path->first);
    }
    intern_dispositions(program, say->dis, say->ndis); intern_dispositions(program, say->val, say->nval);
    intern_dispositions(program, say->defaults, say->ndefaults);
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
static PathCode *registered_path(const Say *say, const char *name){
    uintptr_t offset = (uintptr_t)name - (uintptr_t)say + 1;
    if (!offset || offset > sizeof *say || !say->paths->capacity) { fputs("selector is outside its compiled recipe\n", stderr); abort(); }
    size_t at = path_bucket((uint32_t)offset, say->paths->capacity);
    while (say->paths->entries[at].offset) {
        if (say->paths->entries[at].offset == offset) return say->paths->entries[at].code;
        at = (at + 1) & (say->paths->capacity - 1);
    }
    fputs("semantic selector was not compiled at recipe load\n", stderr); abort();
}
/* Publish only selectors the recipe actually reads through >reference/path.
 * Both sides use the same immutable integer path program. No AST nodes or
 * record-arena pointers escape into the source-lifetime declaration table. */
static void compile_key_projections(Say *say){
    for (int i = 0; i < say->nthing; i++) for (int j = 0; j < say->thing[i].n; j++) {
        const char *name = say->thing[i].name[j]; if (*name != '>') continue;
        PathCode *path = registered_path(say, name + 1);
        if (path->projection || !path->first) continue;
        ReferenceProjection *projection = calloc(1, sizeof *projection);
        if (!projection) { perror("reference projection program"); abort(); }
        path->projection = projection; projection->reference = path->first;
        const char *field = name + 1; unsigned brackets = 0;
        for (; *field; field++) {
            if (*field == '[') brackets++;
            else if (*field == ']' && brackets) brackets--;
            else if (*field == '/' && !brackets) { field++; break; }
        }
        while (*field == '/') field++;
        for (int k = 0; k < say->nkey; k++) {
            const Pair *key = &say->key[k]; int duplicate = 0;
            for (KeyProjection *p = projection->targets; p; p = p->next) duplicate |= p->tier == key->tier_id;
            if (duplicate) continue;
            KeyProjection *target = calloc(1, sizeof *target);
            if (!target) { perror("key projection export"); abort(); }
            target->field = path->first->next; target->tier = key->tier_id;
            keys_field_domain(&key->domain, field, &target->domain);
            target->next = projection->targets; projection->targets = target;
            target->all_next = say->projections; say->projections = target;
        }
    }
}
static void compile_semantic_paths(Say *say, const char *recipe){
    if (say->paths) return;
    say->paths = calloc(1, sizeof *say->paths); if (!say->paths) { perror("recipe selectors"); abort(); }
    keys_domain(recipe, "", 0, &say->key_domain);
    compile_mentions(say);
    for (int i = 0; i < say->nthing; i++) {
        path_register(say, say->thing[i].tier);
        for (int j = 0; j < say->thing[i].n; j++) { path_register(say, say->thing[i].name[j]); path_register(say, say->thing[i].form[j].path); compile_key_forms(&say->thing[i].form[j]); }
    }
    register_att_paths(say, say->att, say->natt); register_att_paths(say, say->pair, say->npair);
    for (int i=0; i<say->natt; i++) if (say->att[i].file && !strcmp(say->att[i].with,"{field}")) say->file_qualifiers=1;
    register_att_paths(say, say->hold, say->nhold); register_att_paths(say, say->itself, say->nitself);
    register_att_paths(say, say->together, say->ntogether);
    for (int i=0; i<say->ndependence; i++) path_register(say,say->dependence[i].tier);
    for (int i=0; i<say->ndefaults; i++) if (say->defaults[i].what == DEFAULT_FIELD) path_register(say,say->defaults[i].arg);
    for (int i = 0; i < say->nrel; i++) {
        path_register(say, say->rel[i].tier); path_register(say, say->rel[i].rel); path_register(say, say->rel[i].to); path_register(say, say->rel[i].with); path_register(say, say->rel[i].of);
    }
    for (int i = 0; i < say->nkey; i++) {
        Pair *key = &say->key[i]; path_register(say, key->tier); path_register(say, key->name); path_register(say,key->scope); path_register(say,key->under);
        key->tier_length = (uint16_t)strlen(key->tier); key->tier_id = (uint16_t)(i + 1);
        int space=(key->under[0] ? KEY_CONTENT_SCOPE : 0) | (key->key_radix ? KEY_NUMERIC : 0);
        keys_domain(recipe,"",space,&key->source_domain);
        keys_domain(recipe, key->tier, space, &key->domain); keys_domain(recipe, key->tier, space|KEY_PARENT, &key->parent_domain);
        for (int j = 0; j < i; j++) if (!strcmp(say->key[j].tier, key->tier)) { key->tier_id = say->key[j].tier_id; break; }
        if (!strcmp(key->tier, say->tree_row)) say->tree_key_index = (uint16_t)(i + 1);
    }
    for (int i = 0; i < say->nwhere; i++) path_register(say, say->where[i].name);
    for (int i = 0; i < say->nadmission; i++) path_register(say, say->admission[i].field);
    for (int i = 0; i < say->nscore; i++) { path_register(say, say->score[i].tier); path_register(say, say->score[i].name); }
    for (int i = 0; i < say->ngames; i++) { path_register(say, say->games[i].tier); path_register(say, say->games[i].name); }
    for (int i = 0; i < say->nhw; i++) { path_register(say, say->hw[i].tier); path_register(say, say->hw[i].a.path); path_register(say, say->hw[i].b.path); compile_key_forms(&say->hw[i].a); compile_key_forms(&say->hw[i].b); }
    for (int i = 0; i < say->ndis; i++) { compile_key_forms(&say->dis[i].k); path_register(say,say->dis[i].under);
        if (say->dis[i].what==D_INDEX) path_register(say,say->dis[i].arg); }
    for (int i = 0; i < say->nval; i++) compile_key_forms(&say->val[i].k);
    path_register(say, say->tree_tier); path_register(say, say->tree_row);
    path_register(say, say->tree_head); path_register(say, say->tree_rel);
    for (int i = 0; i < say->nlayers; i++) {
        path_register(say,say->layers[i].tier); path_register(say,say->layers[i].row);
        for (int j=0; j<say->layers[i].count; j++) path_register(say,say->layers[i].field[j]);
    }
    path_register(say, say->surface.tier); path_register(say, say->surface.row);
    path_register(say, say->surface.key); path_register(say, say->surface.form);
    path_register(say, say->surface.adjoining); path_register(say, say->surface.verify);
    compile_targets(say, say->dis, say->ndis, recipe); compile_targets(say, say->val, say->nval, recipe);
    for (int i=0; i<say->nadmission; i++) {
        const Dis *reference=NULL;
        for (int j=0; j<say->ndis; j++) if (!strcmp(say->dis[j].name,say->admission[i].field)) reference=&say->dis[j];
        int valid=reference && reference->what==D_REFER && !reference->when && reference->ntargets &&
            !reference->under[0] && !reference->key_radix && !reference->k.has_re && !reference->k.nas;
        for (int j=0; valid && j<reference->ntargets; j++)
            if (reference->targets[j].local || reference->targets[j].within || reference->targets[j].constructor) valid=0;
        if (!valid) { fprintf(stderr,"%s: admit references requires an unconditional, direct external reference: %s\n",recipe,say->admission[i].field);exit(2); }
        say->admission[i].reference=reference;
    }
    compile_diagnostics(say->dis, say->ndis); compile_diagnostics(say->val, say->nval);
    for (int i = 0; i < say->nline; i++) say->line[i].pred_length = (uint16_t)strlen(say->line[i].pred);
    compile_name_slots(say);
    compile_key_projections(say);
}
static const SNameBindings *compile_name_bindings(const void *context, const uint8_t *name, uint32_t length, SayScratch *arena);
int say_compile(Recipe *r){
    Say *s = r->say;
    if (!s) return 1;
    compile_semantic_paths(s, r->name);
    if (!s->dis_program) s->dis_program = compile_dispositions(s->dis, s->ndis);
    if (!s->value_program) s->value_program = compile_dispositions(s->val, s->nval);
    if (!s->default_program) s->default_program = compile_dispositions(s->defaults, s->ndefaults);
    if (s->nsplit && !s->lay.g.compiled) {
        s->lay.g.record_name_count = (uint8_t)s->nsplit;
        for (int i = 0; i < s->nsplit; i++) memcpy(s->lay.g.record_names[i].name, s->split[i], sizeof s->split[i]);
    }
    if (!s->lay.ntier && !s->lay.g.n) return 1;
    if (!layout_compile_recipe(&s->lay, r->grammar, sizeof r->grammar)) return 0;
    layout_bind_names(&s->lay, compile_name_bindings, s->paths);
    r->lang = grammar_lang(s->lay.g.grammar_id); return r->lang != NULL;
}
static int static_name_matches(const NameCode *pattern, const uint8_t *name, uint32_t length){
    return !pattern->note && pattern->length <= length && (pattern->wildcard || pattern->length == length) &&
        (!pattern->length || !memcmp(pattern->bytes, name, pattern->length));
}
static const SNameBindings *compile_name_bindings(const void *context, const uint8_t *name, uint32_t length, SayScratch *arena){
    const PathProgram *program = context; size_t count = program->has_positions;
    for (size_t i = 0; i < program->name_count; i++) count += static_name_matches(&program->names[i].name, name, length);
    if (count > UINT32_MAX || count > (SIZE_MAX - sizeof(SNameBindings)) / sizeof(uint32_t)) { fputs("compiled name bindings exceed address space\n", stderr); abort(); }
    size_t bytes = sizeof(SNameBindings) + count * sizeof(uint32_t);
    SNameBindings *bindings = arena ? arena_alloc(arena, bytes) : malloc(bytes);
    if (!bindings) { perror("compiled field bindings"); abort(); }
    bindings->count = 0;
    if (program->has_positions) bindings->slots[bindings->count++] = 0;
    for (size_t i = 0; i < program->name_count; i++)
        if (static_name_matches(&program->names[i].name, name, length)) bindings->slots[bindings->count++] = (uint32_t)(i + 1);
    return bindings;
}

static const PathCode empty_path;
const PathCode *recipe_program_path(const Say *say, const char *name){
    if (!name || !*name) return &empty_path;
    uintptr_t offset = say ? (uintptr_t)name - (uintptr_t)say + 1 : 0;
    if (!say || !say->paths || !say->paths->capacity || offset > sizeof *say || !offset) { fputs("uncompiled semantic selector\n", stderr); abort(); }
    const PathProgram *program = say->paths; size_t at = path_bucket((uint32_t)offset, program->capacity);
    while (program->entries[at].offset) {
        if (program->entries[at].offset == offset) return program->entries[at].code;
        at = (at + 1) & (program->capacity - 1);
    }
    fputs("semantic selector was not compiled at recipe load\n", stderr); abort();
}
