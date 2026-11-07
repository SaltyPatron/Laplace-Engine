/* Compile declarative layout and format rules into integer Tree-sitter symbol/field dispatch tables. */
#define _GNU_SOURCE
#include "structure_internal.h"
static int grammar_path_compile(const TSLanguage *lang, const char *path, GPath *out){
    memset(out, 0, sizeof *out);
    if (path[0] == '.' && !path[1]) return 1;
    uint8_t descendant = 0;
    for (const char *at = path; *at;) {
        if (at[0] == '/' && at[1] == '/') { descendant = 8; at += 2; continue; }
        const char *end = strchr(at, '/'); uint32_t len = (uint32_t)(end ? (size_t)(end - at) : strlen(at));
        if (len) {
            if (out->n == G_PATH_STEPS) return 0;
            GPathStep *step = &out->step[out->n++];
            step->flags = descendant; descendant = 0;
            if (*at == '^') { step->flags |= 4; at++; len--; }
            step->field = ts_language_field_id_for_name(lang, at, len);
            step->symbol = ts_language_symbol_for_name(lang, at, len, true);
            if (!step->symbol) step->symbol = ts_language_symbol_for_name(lang, at, len, false);
            if (step->field) step->flags |= 1;
            if (step->symbol || (len == 5 && !memcmp(at, "ERROR", 5))) step->flags |= 2;
            if (!(step->flags & 3) || ((step->flags & 4) && !(step->flags & 2))) return 0;
        }
        if (end && end[1] == '/') { at = end + 2; descendant = 8; }
        else at = end ? end + 1 : at + len;
    }
    return !descendant;
}

static struct { char name[64]; Layout *layout; uint8_t state; } inner_formats[512];
static unsigned inner_count;
static int layout_resolved(const Layout *layout, const Layout **seen, unsigned count){
    if (!layout || !layout->g.compiled || layout->g.unresolved || layout->part_errors) return 0;
    for (unsigned i = 0; i < count; i++) if (seen[i] == layout) return 1;
    if (count == 512) return 0;
    seen[count++] = layout;
    for (int i = 0; i < layout->g.n; i++) if (layout->g.rule[i].compiled.inner &&
        !layout_resolved(layout->g.rule[i].compiled.inner, seen, count)) return 0;
    for (int i = 0; i < layout->npart; i++) {
        if (layout->part[i].inner && !layout_resolved(layout->part[i].inner, seen, count)) return 0;
        if (layout->part[i].syntax && !layout_resolved(layout->part[i].syntax, seen, count)) return 0;
    }
    return !layout->record_inner || layout_resolved(layout->record_inner, seen, count);
}
static FILE *format_open(const char *name, char *path, size_t capacity){
    snprintf(path, capacity, "%s/formats/%s.format", laplace_recipes(), name);
    FILE *file = fopen(path, "r");
    if (!file && errno == ENOENT && strcmp(laplace_recipes(), LAPLACE_RECIPES_DEFAULT)) {
        snprintf(path, capacity, "%s/formats/%s.format", LAPLACE_RECIPES_DEFAULT, name);
        file = fopen(path, "r");
    }
    if (!file) perror(path);
    return file;
}
static int format_lines(Layout *layout, Recipe *source, FILE *file, const char *path, int *declared, unsigned depth){
    if (depth == 32) { fprintf(stderr, "%s: format includes exceed 32 levels\n", path); return 0; }
    char line[2048];
    while (fgets(line, sizeof line, file)) {
        comment_off(line); recipe_program_line(source, path, line);
        char *token = strtok(line, " \t\r\n"); if (!token) continue;
        if (!strcmp(token, "grammar")) {
            char *language = strtok(NULL, " \t\r\n");
            if (*declared || !language || !*language || strlen(language) >= sizeof layout->g.language) return 0;
            str_set(layout->g.language, sizeof layout->g.language, language); *declared = 1;
        } else if (!strcmp(token, "include")) {
            char name[64], included_path[4400], *argument = strtok(NULL, " \t\r\n");
            if (!argument || strlen(argument) >= sizeof name || strtok(NULL, " \t\r\n")) return 0;
            str_set(name, sizeof name, argument);
            FILE *included = format_open(name, included_path, sizeof included_path); if (!included) return 0;
            int valid = format_lines(layout, source, included, included_path, declared, depth + 1);
            fclose(included); if (!valid) return 0;
        } else if (layout_says(layout, path, token) != 1) return 0;
    }
    return !ferror(file);
}
static const Layout *inner_layout(const char *name){
    unsigned id = 0;
    for (; id < inner_count; id++) if (!strcmp(inner_formats[id].name, name)) break;
    if (id < inner_count) {
        if (inner_formats[id].state == 1) return inner_formats[id].layout;
        if (inner_formats[id].state == 2) {
            const Layout *seen[512];
            if (layout_resolved(inner_formats[id].layout, seen, 0)) return inner_formats[id].layout;
            inner_formats[id].state = 3;
        }
        fprintf(stderr, "format %s: failed injected format\n", name); return NULL;
    }
    if (inner_count == 512 || strlen(name) >= sizeof inner_formats[id].name) return NULL;
    inner_count++; str_set(inner_formats[id].name, sizeof inner_formats[id].name, name);
    inner_formats[id].state = 1;
    char path[4400]; FILE *file = format_open(name, path, sizeof path);
    if (!file) { inner_formats[id].state = 3; return NULL; }
    Layout *layout = calloc(1, sizeof *layout); if (!layout) { perror("injected format"); abort(); }
    inner_formats[id].layout = layout;
    int declared = 0;
    blake3_hasher program; blake3_hasher_init(&program); Recipe source = {.program_state=&program};
    int valid = format_lines(layout, &source, file, path, &declared, 0);
    fclose(file);
    blake3_hasher_finalize(&program, layout->g.format_hash, sizeof layout->g.format_hash);
    if (!valid || !declared || !layout_compile_grammar(layout, layout->g.language)) { inner_formats[id].state = 3; return NULL; }
    inner_formats[id].state = 2; return layout;
}
const Layout *layout_format(const char *name){ return inner_layout(name); }
int layout_compile_parts(Layout *layout){
    if (layout->parts_compiled) return !layout->part_errors;
    layout->parts_compiled = 1;
    int valid = 1;
    for (int i = 0; i < layout->npart; i++) {
        SPart *part = &layout->part[i]; const char *name = part->grammar;
        if (!*name && ((part->seplen == 6 && !memcmp(part->sep, "object", 6)) || (part->seplen == 4 && !memcmp(part->sep, "json", 4)))) name = "json";
        if (*name) { part->inner = inner_layout(name); if (!part->inner) valid = 0; else part->child_grammar_id = part->inner->g.grammar_id; }
        else {
            part->syntax = inner_layout("lp_parts");
            if (!part->syntax) valid = 0;
            else {
                const GMap *grammar = &part->syntax->g;
                for (uint8_t s = 0; s < grammar->separator_count; s++) {
                    const GSeparator *separator = &grammar->separators[s];
                    if (part->seplen == separator->length && !memcmp(part->sep, separator->literal, separator->length)) part->separator = separator->symbol;
                    if (part->islen == separator->length && !memcmp(part->is, separator->literal, separator->length)) part->assignment = separator->symbol;
                }
                if (!part->separator || (part->islen && !part->assignment)) { fprintf(stderr, "part %s: separator is not declared by its compiled grammar\n", part->path); valid = 0; }
            }
        }
        part->name_length = (uint16_t)strlen(part->path);
        if (part->name_length && part->path[part->name_length - 1] == '*') { part->wildcard = 1; part->name_length--; layout->part_prefix_lengths |= UINT64_C(1) << part->name_length; }
        part->name_hash = part_hash((const uint8_t *)part->path, part->name_length);
        size_t bucket = (part->name_hash ^ (part->name_hash >> 32)) & 63u;
        part->next = layout->part_buckets[bucket]; layout->part_buckets[bucket] = (uint16_t)(i + 1);
    }
    layout->part_errors = (uint8_t)!valid;
    return valid;
}
static int compile_symbol_mask(const TSLanguage *language, uint32_t count, const char *name, uint8_t *mask){
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *symbol = ts_language_symbol_name(language, (TSSymbol)i);
        if (symbol && !strcmp(symbol, name)) { mask[i] = 1; found = 1; }
    }
    return found;
}
int use_format(Layout *layout, const char *name){
    char path[4400]; FILE *file = format_open(name, path, sizeof path); if (!file) return 0;
    int declared = 0;
    blake3_hasher program; blake3_hasher_init(&program); Recipe source = {.program_state=&program};
    int valid = format_lines(layout, &source, file, path, &declared, 0);
    fclose(file); blake3_hasher_finalize(&program, layout->g.format_hash, sizeof layout->g.format_hash); return valid && declared;
}
static void layout_hash(const Layout *layout, const Layout **ancestors, unsigned depth, uint8_t out[32]){
    blake3_hasher hash; blake3_hasher_init(&hash);
    if (layout) {
        uint8_t dependency[32]; grammar_program_hash(layout->g.grammar_id, dependency);
        blake3_hasher_update(&hash, dependency, sizeof dependency);
        blake3_hasher_update(&hash, layout->g.format_hash, sizeof layout->g.format_hash);
        for (unsigned i = 0; i < depth; i++) if (ancestors[i] == layout) {
            uint32_t distance = depth - i;
            uint8_t back[4] = {(uint8_t)distance, (uint8_t)(distance >> 8), (uint8_t)(distance >> 16), (uint8_t)(distance >> 24)};
            blake3_hasher_update(&hash, back, sizeof back);
            blake3_hasher_finalize(&hash, out, 32); return;
        }
        if (depth == 512) { fputs("format dependency graph exceeds 512 profiles\n", stderr); abort(); }
        ancestors[depth++] = layout;
        for (int i = 0; i < layout->g.n; i++) if (layout->g.rule[i].compiled.inner) {
            layout_hash(layout->g.rule[i].compiled.inner, ancestors, depth, dependency); blake3_hasher_update(&hash, dependency, sizeof dependency);
        }
        for (int i = 0; i < layout->npart; i++) {
            if (layout->part[i].inner) { layout_hash(layout->part[i].inner, ancestors, depth, dependency); blake3_hasher_update(&hash, dependency, sizeof dependency); }
            if (layout->part[i].syntax) { layout_hash(layout->part[i].syntax, ancestors, depth, dependency); blake3_hasher_update(&hash, dependency, sizeof dependency); }
        }
        if (layout->record_inner) { layout_hash(layout->record_inner, ancestors, depth, dependency); blake3_hasher_update(&hash, dependency, sizeof dependency); }
    }
    blake3_hasher_finalize(&hash, out, 32);
}
void layout_program_hash(const Layout *layout, uint8_t out[32]){
    const Layout *ancestors[512]; layout_hash(layout, ancestors, 0, out);
}
static int separated_by(const STier *tier, const char *separator){
    size_t length = strlen(separator);
    return (size_t)tier->seplen == length && !memcmp(tier->sep, separator, length);
}
static int native_counted_layout(const Layout *layout){
    static Layout *format;
    if (!format) {
        format = calloc(1, sizeof *format); if (!format) { perror("counted grammar schema"); abort(); }
        if (!use_format(format, "wndb")) { free(format); format = NULL; return -1; }
    }
    const STier *row = &layout->tier[0];
    for (unsigned p = 0; p < format->g.prefix_count; p++) {
        unsigned n = format->g.prefix_length[p]; if (n > (unsigned)row->nnames) continue;
        unsigned i = 0; while (i < n && !strcmp(row->names[i], format->g.layout_prefix[p][i])) i++;
        if (i == n) return 1;
    }
    return 0;
}
static int compile_product_records(Layout *layout, char *grammar, size_t capacity, const char *product, const char *framing){
    Layout *inner = malloc(sizeof *inner); if (!inner) { perror("record product grammar"); return 0; }
    *inner = *layout;
    if ((!inner->g.n && !use_format(inner, product)) || !layout_compile_grammar(inner, product)) { free(inner); return 0; }
    layout->g = (GMap){0}; layout->npart = layout->parts_compiled = layout->part_errors = 0;
    memset(layout->part_buckets, 0, sizeof layout->part_buckets); layout->part_prefix_lengths = 0; layout->nlevels = 0;
    str_set(grammar, capacity, framing);
    if (!use_format(layout, grammar)) { free(inner); return 0; }
    layout->record_inner = inner;
    for (int i = 0; i < layout->g.n; i++) if (layout->g.rule[i].what == G_TEXT) {
        layout->g.rule[i].compiled.inner = inner; layout->g.rule[i].compiled.child_grammar_id = inner->g.grammar_id;
        layout->g.rule[i].compiled.flags |= 2u;
    }
    return layout_compile_grammar(layout, grammar);
}
int layout_compile_tiers(Layout *layout, char *grammar, size_t capacity){
    if (!layout->ntier || layout->g.n || layout->g.compiled) return 0;
    const char *name = NULL;
    if (layout->ntier == 1 && separated_by(&layout->tier[0], "\n")) name = "lp_records";
    else if (layout->ntier == 2 && separated_by(&layout->tier[0], "\n%%\n") && separated_by(&layout->tier[1], "\n") &&
             layout->tier[1].kvlen == 1 && layout->tier[1].kv[0] == ':' && layout->tier[1].continued) name = "lp_registry";
    else if (layout->ntier == 2 && separated_by(&layout->tier[0], "\n\n") && separated_by(&layout->tier[1], "\n")) name = "lp_paragraphs";
    else if (layout->ntier == 3 && separated_by(&layout->tier[0], "\n\n") &&
             separated_by(&layout->tier[1], "\n") && separated_by(&layout->tier[2], "\t")) return compile_product_records(layout, grammar, capacity, "conllu", "lp_conllu_records");
    else if (layout->ntier == 2 && separated_by(&layout->tier[0], "\n")) {
        const STier *fields = &layout->tier[1];
        if (layout->tier[0].ncount && separated_by(fields, " ")) {
            int native = native_counted_layout(layout); if (native < 0) return 0;
            name = native ? "wndb" : "lp_words";
        } else if (separated_by(fields, " ")) name = "lp_words";
        else {
            const char *kind = separated_by(fields, ",") ? "csv" : separated_by(fields, "\t") ? "tsv" :
                               separated_by(fields, "|") ? "psv" : separated_by(fields, ";") ? "semicolon" : NULL;
            char escape = fields->escaped ? fields->escaped : layout->tier[0].escaped;
            int quoted = fields->quoted || layout->tier[0].quoted;
            int padded = layout->tier[0].padded;
            char remark = layout->tier[0].remark;
            if (kind && (!escape || escape == '\\') &&
                (!fields->escaped || !layout->tier[0].escaped || fields->escaped == layout->tier[0].escaped) && (!remark || remark == '#')) {
                snprintf(grammar, capacity, "lp_%s%s%s%s%s", kind, quoted ? "_quoted" : "", escape ? "_escaped" : "",
                         quoted && padded ? "_padded" : "", remark ? "_hash" : "");
                name = grammar;
            }
        }
    }
    if (!name) { fputs("tier layout has no matching compiled packaging grammar\n", stderr); return 0; }
    if (name != grammar) str_set(grammar, capacity, name);
    if (!use_format(layout, grammar)) return 0;
    for (int i = 0; i + 1 < layout->ntier; i++) if (layout->tier[i].padded) layout->g.padded = 1;
    return layout_compile_grammar(layout, grammar);
}
int layout_compile_recipe(Layout *layout, char *grammar, size_t capacity){
    if (layout->g.compiled) return !layout->g.unresolved;
    if (!layout->g.n) return layout_compile_tiers(layout, grammar, capacity);
    if (layout->ntier == 3 && !strcmp(grammar, "conllu") && separated_by(&layout->tier[0], "\n\n") &&
        separated_by(&layout->tier[1], "\n") && separated_by(&layout->tier[2], "\t")) return compile_product_records(layout, grammar, capacity, "conllu", "lp_conllu_records");
    if (layout->g.framing[0]) {
        if (layout->g.record_name_count) { fputs("product framing cannot discard selected ancestor scopes\n", stderr); return 0; }
        for (int i = 0; i < layout->g.n; i++) if (layout->g.rule[i].ncontext) {
            fputs("product framing requires ancestor headers in its outer grammar\n", stderr); return 0;
        }
        char product[64], framing[48]; str_set(product, sizeof product, grammar); str_set(framing, sizeof framing, layout->g.framing);
        return compile_product_records(layout, grammar, capacity, product, framing);
    }
    /* JSON Lines has a physical record container and an independent JSON
     * product grammar. Keep the recipe's own levels and part instructions on
     * the child, rather than replacing them with a generic JSON format. */
    if (layout->ntier == 1 && separated_by(&layout->tier[0], "\n") && !strcmp(grammar, "json")) {
        Layout *inner = malloc(sizeof *inner); if (!inner) { perror("record injection grammar"); return 0; }
        *inner = *layout; inner->ntier = 0;
        if (!layout_compile_grammar(inner, grammar)) { free(inner); return 0; }
        layout->g = (GMap){0}; layout->npart = layout->parts_compiled = layout->part_errors = 0;
        memset(layout->part_buckets, 0, sizeof layout->part_buckets); layout->part_prefix_lengths = 0; layout->nlevels = 0;
        str_set(grammar, capacity, "lp_records");
        if (!use_format(layout, grammar)) { free(inner); return 0; }
        layout->record_inner = inner;
        for (int i = 0; i < layout->g.n; i++) if (layout->g.rule[i].what == G_TEXT) {
            layout->g.rule[i].compiled.inner = inner;
            layout->g.rule[i].compiled.child_grammar_id = inner->g.grammar_id;
        }
    }
    return layout_compile_grammar(layout, grammar);
}
int map_count_columns(GMap *grammar, int pending_header){
    memset(grammar->count_by_column, 0, sizeof grammar->count_by_column);
    for (unsigned c = 0; c < grammar->count_count; c++) {
        const GColumn *trigger = &grammar->counts[c].trigger; unsigned found = 0;
        for (uint32_t column = 0; column < grammar->column_count && column < S_NAMES; column++) {
            const GColumn *name = &grammar->columns[column];
            if (name->length != trigger->length || memcmp(name->name, trigger->name, name->length)) continue;
            if (found || grammar->count_by_column[column]) { fputs("counted field has ambiguous column bindings\n", stderr); return 0; }
            grammar->count_by_column[column] = (uint8_t)(c + 1); found = 1;
        }
        if (!found && !pending_header) { fprintf(stderr, "counted %.*s: no named count column\n", (int)trigger->length, trigger->name); return 0; }
    }
    return 1;
}
void layout_bind_names(Layout *layout, s_compile_name_fn compile, const void *program){
    layout->compile_name = compile; layout->name_program = program;
    if (!compile) return;
    if (layout->record_inner) layout_bind_names(layout->record_inner, compile, program);
    for (int i=0;i<=layout->g.n;i++) {
        GRule *rule=i==layout->g.n?&layout->g.record_fallback:&layout->g.rule[i];
        if (rule->numbered[0]) rule->number_bindings=compile(program,(const uint8_t *)rule->numbered,(uint32_t)strlen(rule->numbered),NULL);
        if (rule->capture[0]) rule->capture_bindings=compile(program,(const uint8_t *)rule->capture,rule->capture_length,NULL);
    }
    for (uint32_t i = 0; i < layout->g.column_count; i++) {
        GColumn *column = &layout->g.columns[i]; column->bindings = compile(program, column->name, column->length, NULL);
    }
    for (unsigned i = 0; i < layout->g.count_count; i++) {
        GCountProgram *count = &layout->g.counts[i];
        count->group.bindings = compile(program, count->group.name, count->group.length, NULL);
        for (unsigned j = 0; j < count->arity; j++) {
            GColumn *column = &count->member[j]; column->bindings = compile(program, column->name, column->length, NULL);
        }
    }
}
static int compile_count_shapes(Layout *layout){
    GMap *grammar = &layout->g;
    for (int tier = 0; tier < layout->ntier; tier++) for (int c = 0; c < layout->tier[tier].ncount; c++) {
        const STier *source = &layout->tier[tier]; const GCountShape *shape = NULL;
        if (!grammar->count_shape_count) {
            if (tier || layout->ntier != 2 || !grammar->column_type_count || grammar->column_tier || grammar->count_count == 4) {
                fputs("counted fields require a compiled row/column grammar\n", stderr); return 0;
            }
            GCountProgram *program = &grammar->counts[grammar->count_count++];
            program->trigger = (GColumn){.name = (const uint8_t *)source->count[c].field, .length = (uint32_t)strlen(source->count[c].field)};
            program->group = (GColumn){.name = (const uint8_t *)source->count[c].group, .length = (uint32_t)strlen(source->count[c].group)};
            program->arity = (uint8_t)source->count[c].nmember; program->radix = (uint8_t)source->count[c].radix;
            for (unsigned member = 0; member < program->arity; member++)
                program->member[member] = (GColumn){.name = (const uint8_t *)source->count[c].member[member],
                    .length = (uint32_t)strlen(source->count[c].member[member]), .flags = S_FIELD_ALIAS | S_FIELD_SAME_TIER};
            continue;
        }
        for (uint8_t i = 0; i < grammar->count_shape_count; i++) {
            const GCountShape *candidate = &grammar->count_shapes[i];
            if (!strcmp(candidate->count, source->count[c].field) && candidate->nmember == source->count[c].nmember &&
                candidate->radix == source->count[c].radix) { shape = candidate; break; }
        }
        if (!shape) { fprintf(stderr, "counted %s: no compiled grammar shape of this radix and arity\n", source->count[c].field); return 0; }
        GRule *group = NULL;
        for (int i = 0; i < grammar->n; i++) if (grammar->rule[i].what == G_GROUP && !strcmp(grammar->rule[i].type, shape->group)) { group = &grammar->rule[i]; break; }
        if (!group) { fprintf(stderr, "counted %s: grammar has no group rule %s\n", shape->count, shape->group); return 0; }
        str_set(group->alias, sizeof group->alias, source->count[c].group);
        for (uint8_t member = 0; member < shape->nmember; member++) {
            if (grammar->n == 48) { fputs("counted shape exceeds the compiled rule limit\n", stderr); return 0; }
            GRule *rule = &grammar->rule[grammar->n++]; *rule = (GRule){.what=G_TEXT};
            rule->compiled.flags |= 1u;
            str_set(rule->type, sizeof rule->type, "field"); str_set(rule->field, sizeof rule->field, shape->member[member]);
            str_set(rule->parent, sizeof rule->parent, shape->group);
            str_set(rule->alias, sizeof rule->alias, source->count[c].member[member]);
        }
    }
    return 1;
}
static int compile_remarks(Layout *layout){
    if (!layout->ntier || !*layout->tier[0].remark_kept) return 1;
    GMap *grammar = &layout->g;
    if (!*grammar->remark_text) { fputs("kept remark has no compiled grammar content rule\n", stderr); return 0; }
    for (int i = 0; i < grammar->n; i++) if (grammar->rule[i].what == G_SKIP && !strcmp(grammar->rule[i].type, grammar->remark_group)) {
        memmove(grammar->rule + i, grammar->rule + i + 1, (size_t)(grammar->n - i - 1) * sizeof *grammar->rule); grammar->n--; break;
    }
    GRule *rule = NULL;
    for (int i = 0; i < grammar->n; i++) if (grammar->rule[i].what == G_TEXT && !strcmp(grammar->rule[i].type, grammar->remark_text)) { rule = &grammar->rule[i]; break; }
    if (!rule) {
        if (grammar->n == 48) return 0;
        rule = &grammar->rule[grammar->n++]; *rule = (GRule){.what=G_TEXT};
        str_set(rule->type, sizeof rule->type, grammar->remark_text);
    }
    rule->raw = rule->trim = 1; str_set(rule->alias, sizeof rule->alias, layout->tier[0].remark_kept);
    return 1;
}
int layout_compile_grammar(Layout *l, const char *grammar_name){
    GMap *g = &l->g;
    if (g->compiled) return !g->unresolved;
    if (*g->language) grammar_name = g->language;
    g->compiled = 1; g->grammar_id = grammar_id_for(grammar_name);
    if(g->record_fallback.what){
        GRule *r=&g->record_fallback;
        if(!g->record_children || r->what!=G_GROUP || !r->capture[0] || r->nname || r->text[0] || r->numbered[0] || r->ncontext){
            fputs("@record requires records children and a simple captured group\n",stderr);g->unresolved++;return 0;
        }
        r->compiled.kind=G_GROUP;r->compiled.alias_length=(uint16_t)strlen(r->alias);
    }
    if (l->ntier) {
        g->numbered_length = (uint16_t)strlen(l->tier[0].numbered);
        g->root_name_length = (uint16_t)strlen(l->tier[0].name);
    }
    if (!compile_count_shapes(l) || !compile_remarks(l)) { g->unresolved++; return 0; }
    const TSLanguage *lang = grammar_lang(g->grammar_id);
    if (!lang) { g->unresolved++; fprintf(stderr, "grammar %s: no compiled language\n", grammar_name); return 0; }
    g->symbol_count = ts_language_symbol_count(lang);
    if (g->scope_count) {
        g->scope_roles = calloc(g->symbol_count, 1); if (!g->scope_roles) { perror("scope dispatch"); abort(); }
        for (unsigned i = 0; i < g->scope_count; i++) {
            GScope *scope = &g->scopes[i]; const char *names[] = {scope->open, scope->close, scope->empty};
            if (!grammar_path_compile(lang, scope->name, &scope->identity)) g->unresolved++;
            for (unsigned role = 0; role < 3; role++) {
                TSSymbol symbol = ts_language_symbol_for_name(lang, names[role], (uint32_t)strlen(names[role]), true);
                if (!symbol || g->scope_roles[symbol]) g->unresolved++;
                else g->scope_roles[symbol] = (uint8_t)(1 + i + 8 * role);
            }
        }
        for (unsigned kind = 0; kind < 4; kind++) {
            unsigned count = kind == 0 ? g->scope_space_count : kind == 1 ? g->scope_misc_count : kind == 2 ? g->scope_prefix_count : g->scope_first_count;
            for (unsigned i = 0; i < count; i++) {
                const char *name = kind == 0 ? g->scope_space[i] : kind == 1 ? g->scope_misc[i] : kind == 2 ? g->scope_prefix[i] : g->scope_first[i];
                TSSymbol symbol = ts_language_symbol_for_name(lang, name, (uint32_t)strlen(name), true);
                if (!symbol || g->scope_roles[symbol]) g->unresolved++;
                else g->scope_roles[symbol] = kind == 0 ? 254 : kind == 1 ? 255 : kind == 2 ? 253 : 252;
            }
        }
        if (g->unresolved) fprintf(stderr, "grammar %s: unresolved or overlapping scope declaration\n", grammar_name);
    }
    g->field_count = ts_language_field_count(lang);
    g->field_names = calloc((size_t)g->field_count + 1, sizeof(char *));
    g->field_lengths = calloc((size_t)g->field_count + 1, sizeof(uint32_t));
    if (!g->field_names || !g->field_lengths) { perror("grammar fields"); abort(); }
    for (uint32_t i = 1; i <= g->field_count; i++) {
        g->field_names[i] = ts_language_field_name_for_id(lang, (TSFieldId)i);
        if (g->field_names[i]) g->field_lengths[i] = (uint32_t)strlen(g->field_names[i]);
    }
    if (g->record_count) {
        g->record_symbols = calloc(g->symbol_count, 1); if (!g->record_symbols) { perror("record symbol dispatch"); abort(); }
        for (uint8_t i = 0; i < g->record_count; i++) if (!compile_symbol_mask(lang, g->symbol_count, g->record_types[i], g->record_symbols)) {
            fprintf(stderr, "grammar %s: unresolved record symbol %s\n", grammar_name, g->record_types[i]); g->unresolved++;
        }
    }
    for (uint8_t i = 0; i < g->record_name_count; i++) {
        GRecordName *record = &g->record_names[i];
        record->length = (uint16_t)strlen(record->name); record->hash = part_hash((const uint8_t *)record->name, record->length);
        size_t bucket = (record->hash ^ (record->hash >> 32)) & 31u;
        record->next = g->record_name_buckets[bucket]; g->record_name_buckets[bucket] = (uint8_t)(i + 1);
    }
    if (g->quote_count) {
        g->quoted_symbols = calloc(g->symbol_count, 1); if (!g->quoted_symbols) { perror("quoted symbol dispatch"); abort(); }
        for (uint8_t i = 0; i < g->quote_count; i++) if (!compile_symbol_mask(lang, g->symbol_count, g->quote_types[i], g->quoted_symbols)) {
            fprintf(stderr, "grammar %s: unresolved quoted symbol %s\n", grammar_name, g->quote_types[i]); g->unresolved++;
        }
    }
    if (g->column_type_count) {
        g->column_symbols = calloc(g->symbol_count, 1); g->column_parents = calloc(g->symbol_count, 1);
        g->column_by_field = calloc((size_t)g->field_count + 1, sizeof *g->column_by_field);
        if (!g->column_symbols || !g->column_parents || !g->column_by_field) { perror("column dispatch"); abort(); }
        for (uint8_t i = 0; i < g->column_type_count; i++) if (!compile_symbol_mask(lang, g->symbol_count, g->column_types[i], g->column_symbols)) {
            fprintf(stderr, "grammar %s: unresolved column symbol %s\n", grammar_name, g->column_types[i]); g->unresolved++;
        }
        if (!compile_symbol_mask(lang, g->symbol_count, g->column_parent, g->column_parents)) {
            fprintf(stderr, "grammar %s: unresolved column parent %s\n", grammar_name, g->column_parent); g->unresolved++;
        }
        g->column_parent_symbol = ts_language_symbol_for_name(lang, g->column_parent, (uint32_t)strlen(g->column_parent), true);
        for (uint16_t i = 0; i < g->named_column_count; i++) {
            TSFieldId field = ts_language_field_id_for_name(lang, g->named_columns[i], (uint32_t)strlen(g->named_columns[i]));
            if (!field) { fprintf(stderr, "grammar %s: unresolved column field %s\n", grammar_name, g->named_columns[i]); g->unresolved++; }
            else g->column_by_field[field] = (uint16_t)(i + 1);
        }
        if (g->column_tier < l->ntier) {
            const STier *tier = &l->tier[g->column_tier]; g->column_count = (uint32_t)tier->nnames;
            if (g->column_count) {
                g->columns = calloc(g->column_count, sizeof *g->columns); if (!g->columns) { perror("column names"); abort(); }
                for (uint32_t i = 0; i < g->column_count; i++) g->columns[i] = (GColumn){.name = (const uint8_t *)tier->names[i], .length = (uint32_t)strlen(tier->names[i]), .flags = S_FIELD_ALIAS};
            }
        }
    }
    if (!map_count_columns(g, l->ntier && l->tier[0].header)) g->unresolved++;
    for (uint8_t i = 0; i < g->separator_count; i++) {
        GSeparator *separator = &g->separators[i];
        separator->symbol = ts_language_symbol_for_name(lang, separator->type, (uint32_t)strlen(separator->type), true);
        if (!separator->symbol) { fprintf(stderr, "grammar %s: unresolved separator symbol %s\n", grammar_name, separator->type); g->unresolved++; }
    }
    if (g->symbol_count > UINT16_MAX + 1u) { g->unresolved++; return 0; }
    g->sym_rule_offset = calloc((size_t)g->symbol_count + 1, sizeof(uint32_t));
    if (!g->sym_rule_offset) { perror("grammar dispatch"); abort(); }
    uint8_t found[48] = {0};
    for (uint32_t sym = 0; sym < g->symbol_count; sym++) {
        const char *name = ts_language_symbol_name(lang, (TSSymbol)sym);
        if (name) for (int i = 0; i < g->n; i++) if (!strcmp(g->rule[i].type, name)) {
            g->sym_rule_offset[sym + 1]++; found[i] = 1;
        }
    }
    for (uint32_t sym = 0; sym < g->symbol_count; sym++) g->sym_rule_offset[sym + 1] += g->sym_rule_offset[sym];
    uint32_t total = g->sym_rule_offset[g->symbol_count];
    g->sym_rules = malloc(sizeof(uint16_t) * (total ? total : 1));
    if (!g->sym_rules) { perror("grammar rules"); abort(); }
    for (uint32_t sym = 0; sym < g->symbol_count; sym++) {
        const char *name = ts_language_symbol_name(lang, (TSSymbol)sym); uint32_t at = g->sym_rule_offset[sym];
        if (name) for (int i = 0; i < g->n; i++) if (!strcmp(g->rule[i].type, name)) g->sym_rules[at++] = (uint16_t)i;
    }
    g->context_stream = 1;
    for (int i = 0; i < g->n; i++) {
        GRule *r = &g->rule[i]; CompiledRule *c = &r->compiled;
        if (r->alias_tier && r->alias_tier <= l->ntier) str_set(r->alias, sizeof r->alias, l->tier[r->alias_tier - 1].name);
        c->kind = (GNodeKind)r->what; c->type_length = (uint16_t)strlen(r->type);
        c->alias_length = (uint16_t)strlen(r->alias);
        c->sym_id = ts_language_symbol_for_name(lang, r->type, c->type_length, true);
        if (!found[i]) { fprintf(stderr, "grammar %s: unresolved node %s\n", grammar_name, r->type); g->unresolved++; }
        if (r->field[0]) {
            c->field_id = ts_language_field_id_for_name(lang, r->field, (uint32_t)strlen(r->field));
            if (!c->field_id) { fprintf(stderr, "grammar %s: unresolved field %s on %s\n", grammar_name, r->field, r->type); g->unresolved++; }
        }
        if (r->parent[0]) {
            c->parent_sym = ts_language_symbol_for_name(lang, r->parent, (uint32_t)strlen(r->parent), true);
            if (!c->parent_sym) { fprintf(stderr, "grammar %s: unresolved parent %s on %s\n", grammar_name, r->parent, r->type); g->unresolved++; }
        }
        for (int j = 0; j < r->nname; j++) if (!grammar_path_compile(lang, r->name[j], &c->name[j])) {
            fprintf(stderr, "grammar %s: unresolved name path %s on %s\n", grammar_name, r->name[j], r->type); g->unresolved++;
        }
        for (unsigned j = 0; j < r->ncontext; j++) if (!grammar_path_compile(lang, r->context[j], &c->context[j])) {
            fprintf(stderr, "grammar %s: unresolved context path %s on %s\n", grammar_name, r->context[j], r->type); g->unresolved++;
        }
        g->has_context |= r->ncontext != 0;
        if (r->ncontext && !r->context_level) g->context_stream = 0;
        if (r->text[0] && !grammar_path_compile(lang, r->text, &c->text)) {
            fprintf(stderr, "grammar %s: unresolved text path %s on %s\n", grammar_name, r->text, r->type); g->unresolved++;
        }
        if (r->condition_mode && !grammar_path_compile(lang, r->condition, &c->condition)) {
            fprintf(stderr, "grammar %s: unresolved condition path %s on %s\n", grammar_name, r->condition, r->type); g->unresolved++;
        }
        if (r->child_grammar[0]) {
            c->inner = inner_layout(r->child_grammar);
            if (!c->inner) { fprintf(stderr, "grammar %s: unresolved injection %s\n", grammar_name, r->child_grammar); g->unresolved++; }
            else c->child_grammar_id = c->inner->g.grammar_id;
        }
    }
    if (!g->context_stream) for (int i = 0; i < g->n; i++) if (g->rule[i].context_level) {
        fprintf(stderr, "grammar %s: every inherited header requires a context-level for streaming\n", grammar_name);
        g->unresolved++; break;
    }
    if (!layout_compile_parts(l)) g->unresolved++;
    return !g->unresolved;
}
