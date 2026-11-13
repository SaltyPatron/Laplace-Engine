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
int recipe_property_cached(const Say *s, const char *name, size_t n){
    if (!s->perf || !n || n >= 64) return 0; char folded[64]; size_t length=perf_fold(name,n,folded);
    return lp_strmap_find(s->perf,folded,length) >= 0; }
static int dis_add(Say *s, const char *path, const char *name, int what, const char *arg){
    if(arg && strlen(arg)>=RECIPE_ARGUMENT_BYTES){fprintf(stderr,"%s: disposition argument exceeds %d bytes\n",path,RECIPE_ARGUMENT_BYTES-1);return -1;}
    if (s->ndis == 256) { fprintf(stderr, "%s: more parts than a recipe disposes of (256)\n", path); return -1; }
    for (int i = 0; i < s->ndis; i++) if (!strcmp(s->dis[i].name, name)) { if (what == D_KEY && s->dis[i].what == D_KEY && s->dis[i].when==s->now) return 1;      /* one name may be the key of several kinds of thing (id) */
        if (s->dis[i].when && s->dis[i].when!=s->now) continue; /* ordered conditional cases, followed by at most one fallback */
        fprintf(stderr, "%s: the part %s is given two dispositions\n", path, name); return -1; }
    Dis *d = &s->dis[s->ndis++]; snprintf(d->name, sizeof d->name, "%s", name); dis_parse(d); d->what = what; d->when=s->now; snprintf(d->arg, sizeof d->arg, "%s", arg ? arg : ""); return 1;
}
static int target_append(char *targets,size_t capacity,const char *next,const char *path){
    size_t length=strlen(targets), added=strlen(next);
    if(length+1+added>=capacity){fprintf(stderr,"%s: reference/type target list exceeds %zu bytes\n",path,capacity-1);return 0;}
    targets[length]=' ';memcpy(targets+length+1,next,added+1);return 1;
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
/* Small, reusable source vocabularies compile once to an immutable Native map.
 * They translate only fields which explicitly opt into that vocabulary. */
static int vocabulary_read(Recipe *recipe, KSpec *spec, const char *name, const char *list){
    if (!*name || strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != strlen(name)) return 0;
    char path[4400], line[512]; snprintf(path,sizeof path,"%s/vocabularies/%s.tsv",recipes_directory(),name);
    FILE *file=fopen(path,"r"); if (!file) { perror(path); return 0; }
    lp_strmap *map=lp_strmap_sized(sizeof(VocabularyValue)); int valid=1, header=0;
    while (valid && fgets(line,sizeof line,file)) {
        comment_off(line); recipe_program_line(recipe,path,line); char *at=line, raw[64], canonical[64], extra[2];
        if (!name_next(&at,raw,sizeof raw)) continue;
        if (!name_next(&at,canonical,sizeof canonical) || name_next(&at,extra,sizeof extra)) { valid=0; break; }
        if (!header) { valid=!strcmp(raw,"list") && !strcmp(canonical,list); header=1; continue; }
        bool fresh; VocabularyValue *value=lp_strmap_get(map,raw,strlen(raw),&fresh);
        if (!fresh) { valid=0; break; }
        /* '-' explicitly preserves a documented source category for which
         * this vocabulary makes no assertion of equivalence. */
        value->length=(uint16_t)(strcmp(canonical,"-") ? strlen(canonical) : 0);
        memcpy(value->canonical,canonical,value->length); value->canonical[value->length]=0;
    }
    valid=valid && header && lp_strmap_count(map) && !ferror(file); fclose(file);
    if (!valid) { fprintf(stderr,"%s: expected list %s followed by unique SOURCE CANONICAL pairs\n",path,list); lp_strmap_free(map); return 0; }
    spec->vocabulary=map; snprintf(spec->vocabulary_name,sizeof spec->vocabulary_name,"%s",name); return 1;
}
static int format_says(Recipe *r, const char *name, unsigned depth){
    char path[4400], line[2048];
    if (depth == 32) { fprintf(stderr, "format %s: includes exceed 32 levels\n", name); return -1; }
    snprintf(path, sizeof path, "%s/formats/%s.format", recipes_directory(), name);
    FILE *file = fopen(path, "r"); if (!file) { perror(path); return -1; }
    int valid = 1;
    while (valid && fgets(line, sizeof line, file)) {
        comment_off(line); recipe_program_line(r, path, line);
        char whole[2048]; snprintf(whole, sizeof whole, "%s", line);
        char *token = strtok(line, " \t\r\n"); if (!token) continue;
        if (!strcmp(token, "grammar")) {
            char *language = strtok(NULL, " \t\r\n");
            if (!language || strlen(language) >= sizeof r->grammar || strtok(NULL, " \t\r\n")) valid = 0;
            else snprintf(r->grammar, sizeof r->grammar, "%s", language);
        } else if (!strcmp(token, "include")) {
            char included[64], *argument = strtok(NULL, " \t\r\n");
            if (!argument || strlen(argument) >= sizeof included || strtok(NULL, " \t\r\n")) valid = 0;
            else { snprintf(included, sizeof included, "%s", argument); valid = format_says(r, included, depth + 1) > 0; }
        } else {
            int result = say_says(r, path, token);
            if (result > 0) say_mentions(r, whole);
            else { if (!result) fprintf(stderr, "%s: \"%s\" is not something a format says\n", path, token); valid = 0; }
        }
    }
    valid = valid && !ferror(file); fclose(file); return valid ? 1 : -1;
}
int recipe_product_format(Recipe *r, const char *name){
    Recipe projection = {0}; projection.program_state = r->program_state;
    if (format_says(&projection, name, 0) <= 0 || !projection.say) return 0;
    if (projection.input_frame.width) {
        fprintf(stderr, "%s: input framing belongs to the packaging format, not a product projection\n", r->name); return 0;
    }
    Say *s = projection.say;
    if (!layout_compile_recipe(&s->lay, projection.grammar, sizeof projection.grammar)) return 0;
    if (r->grammar[0] && strcmp(r->grammar, projection.grammar)) {
        fprintf(stderr, "%s: product and packaging must use the same grammar\n", r->name); return 0;
    }
    snprintf(r->grammar, sizeof r->grammar, "%s", projection.grammar);
    r->product = &s->lay;
    return 1;
}
int say_says(Recipe *r, const char *path, char *tok){
    Say *s = r->say; if (!s) s = r->say = calloc(1, sizeof(Say)); s->laid_line = 0;
    char nm[160], a2[RECIPE_ARGUMENT_BYTES], *at;
    #define LAID (s->lay.ntier || s->lay.g.n)
    if (!strcmp(tok,"admit") && LAID) {
        at=strtok(NULL,"\r\n"); char mode[64]; int before=s->nadmission;
        if (!name_next(&at,mode,sizeof mode) || strcmp(mode,"references")) return -1;
        while (name_next(&at,nm,sizeof nm)) {
            if (s->nadmission==8 || strlen(nm)>=64 || s->now) return -1;
            for (int i=0;i<s->nadmission;i++) if (!strcmp(s->admission[i].field,nm)) return -1;
            snprintf(s->admission[s->nadmission++].field,64,"%s",nm);
        }
        return s->nadmission>before ? 1 : -1;
    }
    if (!strcmp(tok, "input")) {
        char *kind = strtok(NULL, " \t\r\n"), *encoding = strtok(NULL, " \t\r\n");
        InputFrame frame;
        if (kind && !strcmp(kind, "decode")) {                                /* input decode NAME: an input decoder of the engine's own */
            const InputDecoder *decoder = encoding ? input_decoder(encoding) : NULL;
            if (!decoder || strtok(NULL, " \t\r\n") || r->decoder || r->input_frame.width) {
                fprintf(stderr, "%s: input decode NAME (once; a decoder the engine holds: grammars.c)\n", path); return -1;
            }
            r->decoder = decoder; s->laid_line = 1; return 1;
        }
        if (!kind || strcmp(kind, "length-prefix") || !encoding || strtok(NULL, " \t\r\n") ||
            r->input_frame.width || r->decoder || !input_frame_encoding(&frame, encoding)) {
            fprintf(stderr, "%s: input length-prefix u8|u16le|u16be|u32le|u32be|u64le|u64be (once)\n", path); return -1;
        }
        r->input_frame = frame; s->laid_line = 1; return 1;
    }
    if (!strcmp(tok,"dependence") && LAID) {
        at=strtok(NULL,"\r\n"); char label[128],extra[8];
        if (s->ndependence==8 || !name_next(&at,a2,sizeof a2) || !name_next(&at,label,sizeof label) || name_next(&at,extra,sizeof extra)) {
            fprintf(stderr,"%s: dependence TIER CLASS\n",path); return -1;
        }
        const lp_trust_class *c=lp_trust_class_named(label);
        if (!c) { fprintf(stderr,"%s: dependence class %s is not in the trust registry\n",path,label); return -1; }
        Dependence *d=&s->dependence[s->ndependence++]; snprintf(d->tier,sizeof d->tier,"%s",a2); d->class=c; d->when=s->now; d->length=(uint16_t)strlen(c->label);
        return 1;
    }
    if (!strcmp(tok, "format")) {
        char name[64], *argument = strtok(NULL, " \t\r\n");
        if (!argument || strlen(argument) >= sizeof name || strtok(NULL, " \t\r\n")) { fprintf(stderr, "%s: format NAME\n", path); return -1; }
        snprintf(name, sizeof name, "%s", argument); return format_says(r, name, 0);
    }
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
            /* NAME product PRODUCT: the part NAME, which a grammar of the engine's own parts (part NAME grammar G), read as
             * that grammar's PRODUCT of its text rather than its content (NativeGrammar.field: an opening's pgn read
             * as the position its line ends in) */
            if (!strcmp(nm, "product") && p->n && !p->lit[p->n - 1]) { if (!name_next(&at, p->product[p->n - 1], sizeof p->product[0])) { fprintf(stderr, "%s: thing TIER NAME product PRODUCT\n", path); return -1; } continue; }
            if (!strcmp(nm, "space") && p->n && !p->lit[p->n - 1]) { char ch[8]; if (!name_next(&at, ch, sizeof ch) || strlen(ch) != 1) { fprintf(stderr, "%s: thing TIER NAME space CHAR\n", path); return -1; } p->space[p->n - 1] = ch[0]; continue; }
            if (!strcmp(nm,"required")) {
                if (!p->resolved || !p->n || p->lit[p->n-1]) { fprintf(stderr,"%s: required follows a resolved selector\n",path); return -1; }
                p->required[p->n-1]=1; continue;
            }
            if (!strcmp(nm,"preserve")) { p->preserve=1; continue; }
            if (!strcmp(nm, "span")) { p->span = 1; continue; } if (!strcmp(nm, "inclusive")) { p->inclusive = 1; continue; } if (!strcmp(nm, "of")) { p->of = 1; continue; } if (!strcmp(nm, "resolved")) { p->resolved = 1; continue; } if (p->n < 8) snprintf(p->name[p->n++], sizeof p->name[0], "%s", nm); }
        if (p->of && !p->n) { p->of = 0; snprintf(p->name[p->n++], sizeof p->name[0], "of"); }
        if (p->span && p->n != 3) { fprintf(stderr, "%s: thing TIER span START END TEXT [inclusive]\n", path); return -1; }
        if (!p->n) { fprintf(stderr, "%s: thing TIER NAME... [within]\n", path); return -1; } return 1; }
    if (!strcmp(tok, "together") && LAID) { at = strtok(NULL, "\r\n");
        if (!name_next(&at,a2,sizeof a2) || s->ntogether==8) { fprintf(stderr,"%s: together TIER\n",path); return -1; }
        Att *x=&s->together[s->ntogether++]; memset(x,0,sizeof *x); snprintf(x->tier,sizeof x->tier,"%s",a2); x->when=s->now; return 1; }
    if (!strcmp(tok, "itself") && LAID) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->nitself == 8) { fprintf(stderr, "%s: itself TIER [with PATH]\n", path); return -1; }
        Att *x = &s->itself[s->nitself++]; memset(x, 0, sizeof *x); snprintf(x->tier, sizeof x->tier, "%s", a2); x->when = s->now;
        if (name_next(&at, nm, sizeof nm)) { if (strcmp(nm, "with") || !name_next(&at, x->with, sizeof x->with)) { fprintf(stderr, "%s: itself TIER [with PATH]\n", path); return -1; } }
        return 1; }
    if (!strcmp(tok, "holds") && LAID) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->nhold == 16) { fprintf(stderr, "%s: holds TIER NAME... [via]\n", path); return -1; }
        Att *x = &s->hold[s->nhold++]; memset(x, 0, sizeof *x); snprintf(x->tier, sizeof x->tier, "%s", a2); x->when = s->now;
        while (x->n < 64 && name_next(&at, nm, sizeof nm)) { if (!strcmp(nm, "via")) { x->file = 1; continue; } snprintf(x->name[x->n++], 64, "%s", nm); }    /* via: under the name of the element between the two (RDF's property) */
        if (!x->n) { fprintf(stderr, "%s: holds TIER NAME...\n", path); return -1; } return 1; }
    if (!strcmp(tok, "key") && LAID) { at = strtok(NULL, "\r\n");
        if (!name_next(&at, a2, sizeof a2) || !name_next(&at, nm, sizeof nm) || s->nkey == S_TIERS) { fprintf(stderr, "%s: key TIER NAME\n", path); return -1; }
        Pair *p = &s->key[s->nkey]; char keyword[32]; int retain=0;
        while (name_next(&at,keyword,sizeof keyword)) {
            if (!strcmp(keyword,"retain")) { retain=1; continue; }
            if (!strcmp(keyword,"radix")) {
                char number[16],*end; unsigned long radix;
                if (!name_next(&at,number,sizeof number) || (radix=strtoul(number,&end,10))<2 || radix>36 || *end || p->key_radix) {
                    fprintf(stderr,"%s: key radix must be 2..36\n",path); return -1;
                }
                p->key_radix=(uint8_t)radix; continue;
            }
            char *scope = !strcmp(keyword,"within") ? p->scope : !strcmp(keyword,"under") ? p->under : NULL;
            size_t capacity = scope==p->scope ? sizeof p->scope : sizeof p->under;
            if (!scope || p->scope[0] || p->under[0] || !name_next(&at,scope,capacity)) {
                fprintf(stderr,"%s: key TIER NAME [within SCOPE | under CONTENT_PATH] [radix N]\n",path); return -1;
            }
        }
        for (int i=0; i<s->nkey; i++) if (!strcmp(s->key[i].tier,a2) && (strcmp(s->key[i].scope,p->scope) || strcmp(s->key[i].under,p->under) || !!s->key[i].key_radix != !!p->key_radix)) {
            fprintf(stderr,"%s: keys of tier %s must share one declared scope\n",path,a2); return -1;
        }
        s->nkey++; snprintf(p->tier, sizeof p->tier, "%s", a2); snprintf(p->name, sizeof p->name, "%s", nm);
        return retain ? 1 : dis_add(s, path, nm, D_KEY, a2); }
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
    if (!strcmp(tok, "strict") && LAID) { char extra[16]; at = strtok(NULL, "\r\n");                 /* strict TIER: what a witness states at run time is read whole or not at all */
        if (!name_next(&at, a2, sizeof a2) || name_next(&at, extra, sizeof extra) || s->nstrict == 4 || strlen(a2) >= sizeof s->strict[0]) { fprintf(stderr, "%s: strict TIER\n", path); return -1; }
        snprintf(s->strict[s->nstrict++], sizeof s->strict[0], "%s", a2); return 1; }
    if (!strcmp(tok, "games") && LAID) { char extra[16]; at = strtok(NULL, "\r\n");
        if (!name_next(&at, a2, sizeof a2) || !name_next(&at, nm, sizeof nm) || name_next(&at, extra, sizeof extra) || s->ngames == 4) { fprintf(stderr, "%s: games TIER NAME\n", path); return -1; }
        Score *x = &s->games[s->ngames++]; snprintf(x->tier, sizeof x->tier, "%s", a2); snprintf(x->name, sizeof x->name, "%s", nm); x->from = 0; x->to = 1; x->mapped = 0;
        return 1; }                                                          /* the count is a number the source states of what it attests, not a claim's part */
    if (!strcmp(tok, "always") && LAID) { s->now = 0; return 1; }
    /* Every part the recipe speaks of whole (the outermost tier) is a record: a path in the file's content tree over its
     * thing and the claims it says (record_of). Where the recipe gives them, the record holds its rows' layers and tree too:
     * layers TIER ROW NAME...            each NAME a path down the TIER's ROWs, aligned to them: a value the row leaves
     *                                    empty stands as the file writes empty
     * tree TIER ROW HEAD REL             the ROWs' tree by HEAD (a row's key, or 0 for none), each subtree the path of its
     *                                    dependents' subtrees and [REL, the row's thing] in the rows' order */
    if (!strcmp(tok, "surface") && LAID) {
        SurfaceProgram *program=&s->surface; char keyword[32], extra[32]; at=strtok(NULL,"\r\n");
        int valid = !program->thing && s->nthing<32 &&
            name_next(&at,program->tier,sizeof program->tier) && name_next(&at,program->row,sizeof program->row) &&
            name_next(&at,program->key,sizeof program->key);
        if (valid && !name_next(&at,program->form,sizeof program->form)) {
            program->linear=1; memcpy(program->form,program->key,sizeof program->form); program->key[0]=0;
        } else if (valid) valid =
            name_next(&at,keyword,sizeof keyword) && !strcmp(keyword,"separator") &&
            name_next(&at,program->separator,sizeof program->separator) &&
            name_next(&at,keyword,sizeof keyword) && !strcmp(keyword,"unless") &&
            name_next(&at,program->adjoining,sizeof program->adjoining) &&
            name_next(&at,program->adjoining_value,sizeof program->adjoining_value) &&
            name_next(&at,keyword,sizeof keyword) && !strcmp(keyword,"verify") &&
            name_next(&at,program->verify,sizeof program->verify);
        if (valid && !program->linear && name_next(&at,extra,sizeof extra)) {
            valid=!strcmp(extra,"spacing") && !name_next(&at,extra,sizeof extra);
            if (valid) {
                program->preserve_spacing=1; program->flags=lp_flags_map(NULL);
                program->white_space=program->flags ? lp_flags_field(program->flags,"White_Space") : NULL;
                valid=program->white_space!=NULL;
            }
        }
        if (!valid) {
            fprintf(stderr,"%s: surface TIER ROW FORM, or surface TIER ROW ID FORM separator TEXT unless FIELD VALUE verify FIELD [spacing] (spacing requires Unicode flags; one per recipe)\n",path); return -1;
        }
        program->separator_length=(uint16_t)strlen(program->separator);
        program->adjoining_length=(uint16_t)strlen(program->adjoining_value);
        Thing *thing=&s->thing[s->nthing++]; memset(thing,0,sizeof *thing);
        snprintf(thing->tier,sizeof thing->tier,"%s",program->tier);
        snprintf(thing->name[0],sizeof thing->name[0],"%s",program->linear ? "." : program->verify); thing->n=1;
        program->thing=(uint8_t)s->nthing; return 1;
    }
    if (!strcmp(tok, "layers") && LAID) { at = strtok(NULL, "\r\n");
        if (s->nlayers==8) { fprintf(stderr,"%s: at most 8 aligned layer declarations\n",path); return -1; }
        int i=s->nlayers++;
        if (!name_next(&at,s->layers[i].tier,sizeof s->layers[i].tier) || !name_next(&at,s->layers[i].row,sizeof s->layers[i].row)) { fprintf(stderr,"%s: layers TIER ROW NAME...\n",path); return -1; }
        for(int j=0;j<i;j++) if(!strcmp(s->layers[j].tier,s->layers[i].tier)){fprintf(stderr,"%s: duplicate aligned layer tier %s\n",path,s->layers[i].tier);return -1;}
        while (name_next(&at,nm,sizeof nm)) {
            if(s->layers[i].count==16){fprintf(stderr,"%s: at most 16 axes in an aligned layer declaration\n",path);return -1;}
            snprintf(s->layers[i].field[s->layers[i].count++],64,"%s",nm);
        }
        if (!s->layers[i].count) { fprintf(stderr,"%s: layers TIER ROW NAME...\n",path); return -1; } return 1; }
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
    if (!strcmp(tok, "default") || !strcmp(tok, "unless_equal")) {
        char value[64], extra[2]; at = strtok(NULL, "\r\n");
        if (!name_next(&at, nm, sizeof nm) || !name_next(&at, value, sizeof value) || name_next(&at, extra, sizeof extra) || s->ndefaults == 256) {
            fprintf(stderr, "%s: %s FIELD %s (at most 256 field defaults)\n", path, tok, tok[0] == 'd' ? "VALUE" : "OTHER_FIELD"); return -1;
        }
        for (int i=0; i<s->ndefaults; i++) if (!strcmp(s->defaults[i].name,nm)) {
            fprintf(stderr,"%s: duplicate default for %s\n",path,nm); return -1;
        }
        Dis *rule=&s->defaults[s->ndefaults++];
        snprintf(rule->name,sizeof rule->name,"%s",nm); dis_parse(rule);
        snprintf(rule->arg,sizeof rule->arg,"%s",value); rule->arg_length=(uint16_t)strlen(value);
        rule->what=tok[0] == 'd' ? DEFAULT_LITERAL : DEFAULT_FIELD; rule->when=s->now;
        return 1;
    }
    if ((!strcmp(tok, "codepoints") || !strcmp(tok, "range")) && LAID) { int what = tok[0] == 'c' ? D_CODEPOINTS : D_RANGE; at = strtok(NULL, "\r\n"); int any = 0;
        while (name_next(&at, nm, sizeof nm)) { if (dis_add(s, path, nm, what, NULL) < 0) return -1; any = 1; }
        if (!any) { fprintf(stderr, "%s: %s NAME...\n", path, tok); return -1; } return 1; }
    if (!strcmp(tok,"index") && LAID) {
        char word[64], list[64]; KSpec form={0}; uint8_t base=0;
        at=strtok(NULL,"\r\n");
        if (!name_next(&at,nm,sizeof nm) || !name_next(&at,word,sizeof word) || strcmp(word,"in") ||
            !name_next(&at,list,sizeof list)) { fprintf(stderr,"%s: index FIELD in LIST [matching PATTERN] [base 0|1]\n",path); return -1; }
        while (name_next(&at,word,sizeof word)) {
            if (!strcmp(word,"matching")) {
                if (form.has_re || !name_next(&at,word,sizeof word) || regcomp(&form.re,word,REG_EXTENDED)) return -1;
                form.has_re=1;
            } else if (!strcmp(word,"base")) {
                if (!name_next(&at,word,sizeof word) || (strcmp(word,"0") && strcmp(word,"1"))) return -1;
                base=(uint8_t)(word[0]-'0');
            } else return -1;
        }
        int result=dis_add(s,path,nm,D_INDEX,list);
        if (result>0) { Dis *rule=&s->dis[s->ndis-1]; rule->k=form; rule->index_base=base; }
        return result;
    }
    if ((!strcmp(tok, "refer") || !strcmp(tok, "type")) && LAID) { at = strtok(NULL, "\r\n");
        if (!name_next(&at, nm, sizeof nm) || !name_next(&at, a2, sizeof a2)) { fprintf(stderr, "%s: %s NAME %s\n", path, tok, tok[0] == 'r' ? "TIER" : "LIST..."); return -1; }
        KSpec ks; memset(&ks, 0, sizeof ks);
        if (tok[0] == 't') { char more[160]; while (name_next(&at, more, sizeof more)) {     /* type NAME LIST... [matching PATTERN] [as TEMPLATE]: the first list that knows it, by the key as those lists write it */
                if (!strcmp(more, "matching")) { if (!name_next(&at, more, sizeof more) || regcomp(&ks.re, more, REG_EXTENDED)) { fprintf(stderr, "%s: type NAME LIST... matching PATTERN (it must compile)\n", path); return -1; } ks.has_re = 1; continue; }
                if (!strcmp(more, "as")) { if (ks.nas == 4 || !name_next(&at, ks.as[ks.nas], sizeof ks.as[0])) { fprintf(stderr, "%s: type NAME LIST... as TEMPLATE... (at most 4)\n", path); return -1; } ks.nas++; continue; }
                if (!strcmp(more, "node")) { ks.node = 1; continue; }
                if (!strcmp(more,"vocabulary")) {
                    char vocabulary[64];
                    if (ks.vocabulary || strchr(a2,' ') || !name_next(&at,vocabulary,sizeof vocabulary) || !vocabulary_read(r,&ks,vocabulary,a2)) {
                        fprintf(stderr,"%s: type FIELD LIST vocabulary NAME (one target list)\n",path); return -1;
                    }
                    continue;
                }
                /* A tagset the highway lists (manifest/slots), with edges to the target list: the source's spelling is
                 * a key of the tagset, and the tagset's type reaches the target's type by its one edge. */
                if (!strcmp(more,"crosswalk")) {
                    if (ks.vocabulary || ks.crosswalk[0] || strchr(a2,' ') || !name_next(&at,more,sizeof more) || !*more || strlen(more)>=sizeof ks.crosswalk) {
                        fprintf(stderr,"%s: type FIELD LIST crosswalk TAGSET (one target list)\n",path); return -1;
                    }
                    snprintf(ks.crosswalk,sizeof ks.crosswalk,"%s",more); continue;
                }
                if(!target_append(a2,sizeof a2,more,path))return -1; }
            if (ks.crosswalk[0] && (ks.vocabulary || ks.has_re || ks.nas || ks.node || strchr(a2,' '))) {
                fprintf(stderr,"%s: crosswalk lookup takes one list and the source spelling directly\n",path); return -1;
            }
            if (ks.vocabulary && (ks.has_re || ks.nas || strchr(a2,' '))) {
                fprintf(stderr,"%s: vocabulary lookup takes one list and the source spelling directly\n",path); return -1;
            }
            int rv = dis_add(s, path, nm, D_TYPE, a2); if (rv > 0) s->dis[s->ndis - 1].k = ks; return rv; }
        else { char more[64], targets[RECIPE_ARGUMENT_BYTES], under[64]=""; uint8_t key_radix=0; snprintf(targets, sizeof targets, "%s", a2);       /* under qualifies a transient pointer by canonical parent content */
            while (name_next(&at, more, sizeof more)) { size_t l = strlen(targets);
                if (!strcmp(more,"matching")) {
                    if (ks.has_re || !name_next(&at,more,sizeof more) || regcomp(&ks.re,more,REG_EXTENDED)) return -1;
                    ks.has_re=1;
                } else if (!strcmp(more,"as")) {
                    if (ks.nas || !name_next(&at,ks.as[0],sizeof ks.as[0])) return -1;
                    ks.nas=1;
                } else if (!strcmp(more,"under")) {
                    if (under[0] || !name_next(&at,under,sizeof under)) { fprintf(stderr,"%s: refer NAME TIER... [under CONTENT_PATH]\n",path); return -1; }
                } else if (!strcmp(more,"radix")) {
                    char number[16],*end; unsigned long radix;
                    if (!name_next(&at,number,sizeof number) || (radix=strtoul(number,&end,10))<2 || radix>36 || *end || key_radix) {
                        fprintf(stderr,"%s: refer radix must be 2..36\n",path); return -1;
                    }
                    key_radix=(uint8_t)radix;
                } else if (!strcmp(more, "within")) {
                    if(l+1>=sizeof targets){fprintf(stderr,"%s: scoped reference target list is too long\n",path);return -1;}
                    char *last = strrchr(targets, ' '); last = last ? last + 1 : targets; memmove(last + 1, last, strlen(last) + 1); *last = '^'; }
                else if(!target_append(targets,sizeof targets,more,path))return -1; }
            int rv=dis_add(s,path,nm,D_REFER,targets); if (rv>0) { Dis *rule=&s->dis[s->ndis-1]; snprintf(rule->under,sizeof rule->under,"%s",under); rule->key_radix=key_radix; rule->k=ks; } return rv; }
        return dis_add(s, path, nm, tok[0] == 'r' ? D_REFER : D_TYPE, a2); }
    if (!strcmp(tok, "perfcache") && LAID) {                                    /* nothing read from the flags is attested: without them, nothing is read at all */
        const lp_layout *l = lp_flags_map(NULL);
        if (!l) { fprintf(stderr, "%s: perfcache: the flags are not at %s (laplace flags generates them)\n", path, lp_flags_path()); return -1; }
        if (s->perf) lp_strmap_free(s->perf);
        s->perf=lp_strmap_sized(1);
        for (size_t i=0; i<l->nfields; i++) {
            char folded[64]; size_t length=perf_fold(l->field[i].name,strlen(l->field[i].name),folded); lp_strmap_put(s->perf,folded,length,NULL);
            length=perf_fold(l->field[i].say,strlen(l->field[i].say),folded); lp_strmap_put(s->perf,folded,length,NULL);
        }
        return 1; }
    if (!strcmp(tok, "attest") && LAID) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->natt == 32) { fprintf(stderr, "%s: attest TIER NAME... [of NAME] [with NAME]\n", path); return -1; }
        Att *x = &s->att[s->natt++]; memset(x, 0, sizeof *x); snprintf(x->tier, sizeof x->tier, "%s", a2); x->when = s->now;
        while (x->n < 64 && name_next(&at, nm, sizeof nm)) {
            if (!strcmp(nm,"within-file")) { x->file=1; continue; }
            if (!strcmp(nm, "with") || !strcmp(nm, "of")) { char w[64]; if (!name_next(&at, w, sizeof w)) { fprintf(stderr, "%s: attest ... %s NAME\n", path, nm); return -1; } snprintf(nm[0] == 'w' ? x->with : x->of, 64, "%s", w); continue; }
            snprintf(x->name[x->n++], 64, "%s", nm); }
        if (!x->n || (x->file && strcmp(x->with,"{field}"))) { fprintf(stderr, "%s: attest TIER NAME... [of NAME] [with NAME]; within-file requires with {field}\n", path); return -1; } return 1; }
    if ((!strcmp(tok, "about") || !strcmp(tok, "line")) && LAID) {           /* PATTERN: the rest of the line; its parentheses are the parts it says */
        char *rest = strtok(NULL, "\r\n"); if (!rest) { fprintf(stderr, "%s: %s PATTERN\n", path, tok); return -1; } while (*rest == ' ' || *rest == '\t') rest++;
        char pat[1024]; snprintf(pat, sizeof pat, "%s", rest); int mode = 0; char pred[64] = "";
        if(tok[0]=='a') {
            char *fallback=strstr(pat," else {file}");
            if(fallback && !fallback[12]){*fallback=0;s->about_file=1;}
        }
        if (tok[0] == 'l') { char *m = strstr(pat, " :: "); if (m) { *m = 0; char *w = m + 4; while (*w == ' ') w++;     /* PATTERN :: pair | claim | predicate NAME (an obligation) */
                if (!strncmp(w, "pair", 4)) mode = 2; else if (!strncmp(w, "claim", 5)) mode = 1; else if (!strncmp(w, "predicate ", 10)) { mode = 3; snprintf(pred, sizeof pred, "%s", w + 10); size_t pl = strlen(pred); while (pl && (pred[pl - 1] == ' ' || pred[pl - 1] == '\r')) pred[--pl] = 0; }
                else if (!strncmp(w,"relation ",9)) {
                    char *tail=w+9,extra[64];
                    if(!name_next(&tail,pred,sizeof pred)){fprintf(stderr,"%s: a line relation needs its documented meaning\n",path);return -1;}
                    mode=4;
                    if(name_next(&tail,extra,sizeof extra)) {
                        if(strcmp(extra,"within") || name_next(&tail,extra,sizeof extra)){fprintf(stderr,"%s: line PATTERN :: relation \"TEXT\" [within]\n",path);return -1;}
                        mode=5;
                    }
                }
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
            if ((strcmp(w1, "with") && strcmp(w1, "of")) || !name_next(&at, w2, sizeof w2)) { fprintf(stderr, "%s: relate TIER NAME to NAME [alone] [with NAME] [of PATH]\n", path); return -1; } snprintf(w1[0]=='w' ? x->with : x->of, sizeof x->with, "%s", w2); }
        return 1; }
    s->laid_line = 1; return layout_says(&s->lay, path, tok);
}
int say_lays(const Recipe *r){ const Say *s = r->say; return s && (s->lay.ntier > 0 || s->lay.g.n > 0); }
/* Whether a file this recipe lays out can be read a stretch at a time: its outermost parts are lines (1) or end at an
 * empty line (2), and nothing in a part points at another part (a key within the file). */
int say_stretches(const Recipe *r){ const Say *s = r->say; if (!s || !s->lay.ntier || s->nsplit || s->lay.tier[0].quoted || s->lay.tier[0].escaped || s->lay.tier[0].numbered[0] || s->has_about) return 0;     /* numbered: a line is counted from the file's start, read whole */
    for (int z = 0; z < s->ndis; z++) if (s->dis[z].what == D_REFER)
        for (uint8_t i = 0; i < s->dis[z].ntargets; i++) if (s->dis[z].targets[i].local) return 0;
    if (!strcmp(s->lay.tier[0].sep, "\n")) return 1; if (!strcmp(s->lay.tier[0].sep, "\n\n")) return 2; return 0; }
const char *say_refers(const Recipe *r, int i){ const Say *s = r ? r->say : NULL; if (!s) return NULL;
    for (int z=0; z<s->ndis; z++) if (s->dis[z].what==D_REFER)
        for (uint8_t target=0; target<s->dis[z].ntargets; target++)
            if (!s->dis[z].targets[target].local && strcmp(s->dis[z].targets[target].name,r->name) && !i--) return s->dis[z].targets[target].name;
    return NULL; }
void say_program_hash(const Recipe *recipe, uint8_t out[32]){
    const Say *say = recipe ? recipe->say : NULL; layout_program_hash(say ? &say->lay : NULL, out);
    /* Record interpretation changed even where the recipe text did not:
     * cached file metadata must not hide newly retained mapping evidence. */
    blake3_hasher hash; blake3_hasher_init(&hash);
    static const char version[]="laplace-source-interpretation-v4";
    blake3_hasher_update(&hash,version,sizeof version-1); blake3_hasher_update(&hash,out,32);
    blake3_hasher_finalize(&hash,out,32);
}

int say_only_disposes(const Recipe *r){ const Say *s = r->say; return s && s->ndis && !s->nthing && !s->natt && !s->nrel && !s->nhw && !s->npair && !s->nhold && !s->nkey; }

int say_has_keys(const Recipe *recipe){ const Say *say = recipe ? recipe->say : NULL;
    for (int i=0; say && i<say->nkey; i++) if (!say->key[i].scope[0]) return 1;
    return 0;
}

int say_reads_keys(const Recipe *recipe){ const Say *say = recipe ? recipe->say : NULL;
    for (int i=0; say && i<say->ndis; i++) if (say->dis[i].what==D_REFER) return 1;
    return 0;
}

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

const char *say_relation(const Recipe *r, const char *name){
    const Say *s = r ? r->say : NULL; if (!s) return NULL;
    for (int i = 0; i < s->nval; i++) if (s->val[i].what == 1 && !strcmp(s->val[i].name, name)) return s->val[i].text;
    return NULL;
}

int say_thing_parts(const Recipe *r, const char *tier, const char *a, const char *b, int *ia, int *ib){
    const Say *s = r ? r->say : NULL; if (!s) return -1;
    for (int i = 0; i < s->nthing; i++) { const Thing *th = &s->thing[i]; if (strcmp(th->tier, tier) || th->of || th->span || th->within || th->infile) continue;
        int x = -1, y = -1;
        for (int z = 0; z < th->n; z++) { if (th->lit[z]) continue; const char *nm = th->name[z], *sl = strrchr(nm, '/'), *last = sl ? sl + 1 : nm + (*nm == '>' || *nm == '^');
            if (x < 0 && !strcmp(last, a)) x = z; else if (y < 0 && !strcmp(last, b)) y = z; }
        if (x >= 0 && y >= 0) { *ia = x; *ib = y; return th->n; } }
    return -1;
}


int say_games(const Recipe *r, const char *name){
    const Say *s = r ? r->say : NULL; if (!s) return 0;
    for (int i = 0; i < s->ngames; i++) if (!strcmp(s->games[i].name, name)) return 1;
    return 0;
}
int say_part_native(const Recipe *r, const struct NativeGrammar *g){
    const Say *s = r ? r->say : NULL; if (!s || !g) return 0;
    for (int i = 0; i < s->lay.npart; i++) if (s->lay.part[i].native == g) return 1;
    return 0;
}
