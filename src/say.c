#define _GNU_SOURCE
/* What a recipe says each part of a file's tree is. The decomposer (structure.c) gives the tree the recipe's layout
 * parts the file into; this gives every named part its disposition, as the recipe's configuration writes it, and
 * composes what follows from that: the file's trunk over its metadata tree and its content tree, and what the file
 * attests. There is no code here for a format or for a source: only for what a recipe can say.
 *
 *   thing TIER NAME...         a TIER is the thing its part NAME names (a row is its word; a record is its text); named
 *                              by several parts together, it is the path of them
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
 *   metadata NAME...           the part is said of the file itself: it goes in the file's metadata tree
 *   omit NAME...               the part is the file's bookkeeping: read by nothing
 *   perfcache                  a part named as a property the perf-cache's flags hold (tier0.flags, its layout) is read
 *                              from there for every codepoint: it is not attested again
 *   attest TIER NAME...        of a TIER's thing, what each part NAME says, under the part's own name: [thing, NAME, value];
 *                              a part that is itself KEY IS VALUE parts says each VALUE under its KEY: [thing, KEY, VALUE]
 *   relate TIER NAME to NAME   of a TIER's thing, its relation (the first part's value) to what the second part is:
 *                              [thing, relation, other]; where the second names nothing, the pair [thing, relation]
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
typedef struct { char list[32], path[160], as[64]; regex_t re; int has_re; } KSpec;
typedef struct { char name[64]; int what; char arg[64]; KSpec k;
                 struct { uint8_t off, len, wild, note; } whole, part; uint8_t el_l; } Dis;   /* k: of a type, how its key is written; whole, part, el_l: its name as matched, split once (dis_parse) */
typedef struct { char tier[32], name[64]; } Pair;
typedef struct { char tier[32]; char name[64][64]; int n; uint32_t when; char by[64], of[64]; int file; } Att;      /* when: the conditions it is said under, by their places in where[]; by: the part that names who says it; of: the part it is said of; file: a witness stands within the file */
typedef struct { char tier[32], rel[64], to[64]; uint32_t when; char by[64]; int alone; } Rel;     /* alone: where what it is to names nothing, the relation alone */
typedef struct { char tier[32], name[8][160]; int n, within, infile, span, inclusive, of; } Thing;    /* of: the path of the things of its parts of these names, in order (a sentence of its words) */   /* within: its name stands only within the thing it is inside: the pair of that thing and it; infile: only within the file; span: the stretch of a text name[2] between the characters name[0] and name[1] */
typedef struct { char tier[32], name[64]; double from, to; int mapped; } Score;
typedef struct { char name[64]; int op; char val[128]; regex_t re; } Where;
typedef struct { int what; char tier[32], say[64]; KSpec a, b; uint32_t when; } HwLine;       /* what: 1 types, 2 keyed, 3 alias, 4 maps */
typedef struct { Layout lay; Dis dis[256]; int ndis; HwLine hw[32]; int nhw; Thing thing[32]; int nthing; Pair key[32]; int nkey; Att att[32]; int natt; Att pair[8]; int npair; Att hold[16]; int nhold; Rel rel[16]; int nrel;
                 Att itself[8]; int nitself;                                 /* a TIER's thing is itself what is attested: a tuple is the claim it is */
                 Att voices[8]; int nvoices;                                 /* parts each of which is a witness of its own, saying its field of the thing */
                 Att together[8]; int ntogether;                             /* a TIER whose claims are said together: one record, witnessed once, its claims within it */
                 char stem, stemfrom;                                        /* what ends the part of the file's name {file} stands for (. unless said), and what it begins after */
                 char split[16][64]; int nsplit;                             /* a long file a grammar reads is parted before each line that begins one of these elements */
                 char (*perf)[64]; int nperf;                               /* perfcache: the flags' property names, short and as said, folded (UAX44-LM3) */
                 char selfmark;                                              /* what the source writes, in a value, for the code point the part is (UCD: #) */
                 regex_t about_re; int has_about;                            /* a page's lines: the first that matches names what the page is about */
                 struct { regex_t re; int mode; char pred[64]; } line[8]; int nline;    /* a line that matches says its parts: 0 of what the page is about, 1 the claim itself, 2 a pair, 3 under a name */
                 Score score[4]; int nscore; Where where[16]; int nwhere; uint32_t whole, now; Pair voice; } Say;   /* whole: the conditions on the outermost part (where); now: the ones the lines being read are said under (when) */

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
int say_says(Recipe *r, const char *path, char *tok){
    Say *s = r->say; if (!s) s = r->say = calloc(1, sizeof(Say));
    char nm[160], a2[64], *at;
    #define LAID (s->lay.ntier || s->lay.g.n)
    if (!strcmp(tok, "format")) {                                            /* the format's own lines, kept once for every recipe of that format: recipes/formats/NAME.format */
        char *fn = strtok(NULL, " \t\r\n"), fp[4400], line[2048]; if (!fn) { fprintf(stderr, "%s: format NAME\n", path); return -1; }
        snprintf(fp, sizeof fp, "%s/formats/%s.format", laplace_recipes(), fn); FILE *f = fopen(fp, "r"); if (!f) { perror(fp); return -1; }
        while (fgets(line, sizeof line, f)) { comment_off(line); char *t2 = strtok(line, " \t\r\n"); if (!t2) continue;
            if (!strcmp(t2, "grammar")) { char *g = strtok(NULL, " \t\r\n"); if (g) snprintf(r->grammar, sizeof r->grammar, "%s", g); continue; }
            int sv = say_says(r, fp, t2); if (sv <= 0) { if (!sv) fprintf(stderr, "%s: \"%s\" is not something a format says\n", fp, t2); fclose(f); return -1; } }
        fclose(f); return 1; }
    if (!strcmp(tok, "thing")) { at = strtok(NULL, "\r\n"); if (!name_next(&at, a2, sizeof a2) || s->nthing == 32) { fprintf(stderr, "%s: thing TIER NAME... [within]\n", path); return -1; }
        Thing *p = &s->thing[s->nthing++]; snprintf(p->tier, sizeof p->tier, "%s", a2); while (p->n < 8 && name_next(&at, nm, sizeof nm)) { if (!strcmp(nm, "within")) { p->within = 1; continue; } if (!strcmp(nm, "within-file")) { p->infile = 1; continue; }
            if (!strcmp(nm, "span")) { p->span = 1; continue; } if (!strcmp(nm, "inclusive")) { p->inclusive = 1; continue; } if (!strcmp(nm, "of")) { p->of = 1; continue; } snprintf(p->name[p->n++], sizeof p->name[0], "%s", nm); }
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
    if ((!strcmp(tok, "where") || !strcmp(tok, "when")) && LAID) { char op[16]; int when = tok[2] == 'e' && tok[3] == 'n'; at = strtok(NULL, "\r\n");
        if (!name_next(&at, nm, sizeof nm) || !name_next(&at, op, sizeof op) || s->nwhere == 16) { fprintf(stderr, "%s: %s NAME is VALUE | is-not VALUE | matches PATTERN\n", path, tok); return -1; }
        if (when) s->now |= 1u << s->nwhere; else s->whole |= 1u << s->nwhere;
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
                if (!strcmp(more, "as")) { if (!name_next(&at, ks.as, sizeof ks.as)) { fprintf(stderr, "%s: type NAME LIST... as TEMPLATE\n", path); return -1; } continue; }
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
        if (tok[0] == 'l') { char *m = strstr(pat, " :: "); if (m) { *m = 0; char *w = m + 4; while (*w == ' ') w++;     /* PATTERN :: pair | claim | predicate NAME */
                if (!strncmp(w, "pair", 4)) mode = 2; else if (!strncmp(w, "claim", 5)) mode = 1; else if (!strncmp(w, "predicate ", 10)) { mode = 3; snprintf(pred, sizeof pred, "%s", w + 10); size_t pl = strlen(pred); while (pl && (pred[pl - 1] == ' ' || pred[pl - 1] == '\r')) pred[--pl] = 0; }
                else { fprintf(stderr, "%s: line PATTERN :: pair | claim | predicate NAME\n", path); return -1; } } }
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
            else if (!strcmp(w, "as")) ok = NEXT(k->as);
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
    return layout_says(&s->lay, path, tok);
}
int say_lays(const Recipe *r){ const Say *s = r->say; return s && (s->lay.ntier > 0 || s->lay.g.n > 0); }
/* Whether a file this recipe lays out can be read a stretch at a time: its outermost parts are lines (1) or end at an
 * empty line (2), and nothing in a part points at another part (a key within the file). */
static int is_local(const Say *s, const char *tier);
int say_stretches(const Recipe *r){ const Say *s = r->say; if (!s || !s->lay.ntier || s->nsplit || s->lay.tier[0].header || s->lay.tier[0].quoted) return 0;
    for (int z = 0; z < s->ndis; z++) if (s->dis[z].what == D_REFER) { char a[64]; snprintf(a, sizeof a, "%s", s->dis[z].arg); for (char *c = strtok(a, " "); c; c = strtok(NULL, " ")) if (is_local(s, *c == '^' ? c + 1 : c)) return 0; }
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
typedef struct { Ref *c; size_t n, cap; } Refs;
static void push(Refs *a, const Ref *x){ if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 256; a->c = xrealloc(a->c, sizeof(Ref) * a->cap); } a->c[a->n++] = *x; }   /* by address: a copy per call, in a loop, is stack that is never given back */
typedef struct { const Recipe *r; const Say *s; Events ev; Refs things, meta; uint64_t open[256]; float er, ed, score; uint64_t ordinal; Ref fw, voice; int voiced; char opennm[16][64]; int nopen; uint64_t unknown; char unknm[8][96]; int nunk; Refs grp; Ref fname, fstem;
                 const STree *ix_tree; uint32_t ix_count; uint64_t *ix_h; int32_t *ix_g; size_t ix_cap;      /* the key index of the tree being read */
                 const STree *tc_tree; uint32_t tc_n, tc_cap; Ref *tc; uint8_t *ts;
                 long selfcp;                                                /* the code point the part being read is, or -1 */
                 Ref fabout; int has_fabout;                                 /* what the page is about, where its about line names it */
                 Hw *hw; struct HwRec *hr; size_t nhr, chr; int worker; int16_t *dm; uint32_t dm_cap; } Sink;          /* reading for the highway (laplace highway): what the part says of its types, in order */
typedef struct HwRec { int what, line; Ref thing; char *a, *b; } HwRec;     /* what: 1 a type, 2 a key of one, 3 a key naming what another names, 4 an edge */
static void hw_rec(Sink *k, int what, int line, const Ref *thing, const char *a, size_t al, const char *b, size_t bl){
    if (k->nhr == k->chr) { k->chr = k->chr ? k->chr * 2 : 1024; k->hr = xrealloc(k->hr, sizeof(HwRec) * k->chr); }
    HwRec *x = &k->hr[k->nhr++]; x->what = what; x->line = line; if (thing) x->thing = *thing; else memset(&x->thing, 0, sizeof x->thing);
    x->a = a ? strndup(a, al) : NULL; x->b = b ? strndup(b, bl) : NULL;
}                   /* the things of the tree being read, each composed once */   /* fname: the file's name; fstem: its name up to its first dot */   /* grp: the claims the part being read has made */   /* fw: the file's witness; voice: who says the part being read, when the file names one; opennm: the names left open */
/* A named part the recipe gives no disposition: counted, and its name kept to be said. */
static void left_open(Sink *k, const SNode *x){
    k->open[0]++; for (int i = 0; i < k->nopen; i++) if (strlen(k->opennm[i]) == x->nlen && !memcmp(k->opennm[i], x->name, x->nlen)) return;
    if (k->nopen < 16) snprintf(k->opennm[k->nopen++], 64, "%.*s", (int)(x->nlen < 63 ? x->nlen : 63), x->name);
}
static uint8_t over(const Ref *r, size_t n){ uint8_t t = 0; for (size_t i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }
/* A name as the recipe writes it: NAME* is every name that begins with NAME (a header's columns that begin alike). */
static int named_as(const SNode *x, const char *name){
    if (!strncmp(name, "note:", 5)) { if (x->kind != S_NOTE) return 0; name += 5; }       /* note:NAME: a note of that name, and nothing else (note:*: every note) */
    size_t l = strlen(name); if (l && name[l - 1] == '*') return x->nlen >= l - 1 && !memcmp(x->name, name, l - 1); return s_named(x, name, l); }
/* The tree being read, for a name scoped to what holds it (ELEMENT.NAME: that part of that element only). */
static __thread const STree *TT;
#define BUSY_MAX 256
#define FAN 4096                                                        /* a part holding this many parts has them read on every core */
static __thread int32_t busy[BUSY_MAX]; static __thread int nbusy, busy_base;     /* the things this thread is composing, innermost last; from busy_base, the reading it is in now */
static inline int composing(int32_t g){ for (int i = busy_base; i < nbusy; i++) if (busy[i] == g) return 1; return 0; }
/* named_as, for a name split when the recipe was read */
static inline int name_is(const SNode *x, const char *n, uint8_t off, uint8_t len, uint8_t wild, uint8_t note){
    if (note && x->kind != S_NOTE) return 0; return wild ? x->nlen >= len && !memcmp(x->name, n + off, len) : s_named(x, n + off, len); }
static const Dis *dis_scan(const Say *s, const SNode *x){
    if (TT && x->parent >= 0) {                                              /* ELEMENT.NAME first: it says more than NAME */
        const SNode *el = &TT->n[x->parent]; if (el->nlen == x->nlen && !memcmp(el->name, x->name, x->nlen) && el->parent >= 0) el = &TT->n[el->parent];   /* a piece of a part: the part's element */
        for (int i = 0; i < s->ndis; i++) { const Dis *d = &s->dis[i]; if (!d->el_l) continue;
            if (el->nlen == d->el_l && !memcmp(el->name, d->name, d->el_l) && name_is(x, d->name, d->part.off, d->part.len, d->part.wild, d->part.note)) return d; } }
    for (int i = 0; i < s->ndis; i++) { const Dis *d = &s->dis[i]; if (name_is(x, d->name, d->whole.off, d->whole.len, d->whole.wild, d->whole.note)) return d; }
    return NULL;
}
/* A part's disposition, found once for each node of the tree being read (DM, shared by the threads reading it). */
static __thread int16_t *DM;
static const Dis *dis_of(const Say *s, const SNode *x){
    if (!DM || !TT || x < TT->n || x >= TT->n + TT->count) return dis_scan(s, x);
    size_t i = (size_t)(x - TT->n); int16_t m = __atomic_load_n(&DM[i], __ATOMIC_RELAXED); if (m) return m > 0 ? &s->dis[m - 1] : NULL;
    const Dis *d = dis_scan(s, x); __atomic_store_n(&DM[i], (int16_t)(d ? d - s->dis + 1 : -1), __ATOMIC_RELAXED); return d;
}
/* One step of a path from a part: ^ what it is inside; N (a number) its Nth part, counted from 1 (a piece of a part
 * parted by its separator: /c/en/ice_cream/n, its 3 is ice_cream); otherwise its first part of that name. */
static int32_t child_path(const STree *t, int32_t g, const char *path);
/* NAME[CHILD=VALUE]: a part of that name whose CHILD is written VALUE (a property whose predicate is skos:definition). */
static int32_t filtered(const STree *t, int32_t c, const char *step, int32_t after){
    const char *br = strchr(step, '['), *eq = br ? strchr(br, '=') : NULL; size_t sl = strlen(step);
    if (!br || !eq || step[sl - 1] != ']') return s_child(t, c, step, after);
    char nm[64], ch[64]; snprintf(nm, sizeof nm, "%.*s", (int)(br - step), step); snprintf(ch, sizeof ch, "%.*s", (int)(eq - br - 1), br + 1);
    const char *v = eq + 1; size_t vl = (size_t)(step + sl - 1 - v);
    for (int32_t q = s_child(t, c, nm, after); q >= 0; q = s_child(t, c, nm, q)) { int32_t w = child_path(t, q, ch);
        if (w >= 0 && t->n[w].kind == S_GROUP && t->n[w].first >= 0 && t->n[t->n[w].first].next < 0) w = t->n[w].first;      /* a part that holds only its text */
        if (w >= 0 && t->n[w].vlen == vl && !memcmp(t->n[w].val, v, vl)) return q; }
    return -1;
}
static int32_t step_of(const STree *t, int32_t c, const char *step){
    if (!strcmp(step, "^")) return t->n[c].parent;
    if (step[0] == '^') { size_t l = strlen(step + 1); for (int32_t up = t->n[c].parent; up >= 0; up = t->n[up].parent) if (s_named(&t->n[up], step + 1, l)) return up; return -1; }     /* ^NAME: the nearest part of that name it is inside */
    if (*step >= '0' && *step <= '9') { char *e; long n = strtol(step, &e, 10); if (!*e) { int32_t q = t->n[c].first; while (q >= 0 && --n > 0) q = t->n[q].next; return n == 0 ? q : -1; } }
    return filtered(t, c, step, -1);
}
/* A part by a path of steps, A/B: the B of its A. */
static int32_t child_path(const STree *t, int32_t g, const char *path){
    char buf[256]; snprintf(buf, sizeof buf, "%s", path); char *save = NULL; int32_t c = g;
    for (char *step = strtok_r(buf, "/", &save); step && c >= 0; step = strtok_r(NULL, "/", &save)) c = step_of(t, c, step);
    return c;
}
/* Every part a path names: at each step, every part of that name (the contributor of each of an edge's sources). */
static int all_path(const STree *t, int32_t g, const char *path, int32_t *out, int cap){
    if (!strcmp(path, ".")) { if (cap < 1) return 0; out[0] = g; return 1; }   /* the part itself */
    const char *sl = strchr(path, '/'); char step[64]; size_t l = sl ? (size_t)(sl - path) : strlen(path); if (l >= sizeof step) return 0; memcpy(step, path, l); step[l] = 0;
    int n = 0;
    if (!strcmp(step, "^") || (*step >= '0' && *step <= '9')) { int32_t c = step_of(t, g, step); if (c < 0) return 0; if (!sl) { out[0] = c; return 1; } return all_path(t, c, sl + 1, out, cap); }
    for (int32_t c = filtered(t, g, step, -1); c >= 0 && n < cap; c = filtered(t, g, step, c)) { if (!sl) out[n++] = c; else n += all_path(t, c, sl + 1, out + n, cap - n); }
    return n;
}
static int is_local(const Say *s, const char *tier){ for (int i = 0; i < s->nkey; i++) if (!strcmp(s->key[i].tier, tier)) return 1; return 0; }
/* The key index of a tree: every part with a key, by its tier and the key's text, built once per tree. */
static uint64_t kh(const char *tier, const uint8_t *v, size_t n){ uint64_t h = 1469598103934665603ull; for (const char *c = tier; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull; h = (h ^ 0xff) * 1099511628211ull; for (size_t i = 0; i < n; i++) h = (h ^ v[i]) * 1099511628211ull; return h | 1; }
static void index_keys(Sink *k, const STree *t){
    const Say *s = k->s; size_t cap = 64; while (cap < (size_t)t->count * 2) cap <<= 1;
    free(k->ix_h); free(k->ix_g); k->ix_h = calloc(cap, 8); k->ix_g = malloc(sizeof(int32_t) * cap); k->ix_cap = cap; k->ix_tree = t; k->ix_count = t->count;
    for (uint32_t g = 0; g < t->count; g++) { const SNode *y = &t->n[g]; if (y->kind != S_GROUP || !y->nlen) continue;
        for (int i = 0; i < s->nkey; i++) { if (!s_named(y, s->key[i].tier, strlen(s->key[i].tier))) continue; int32_t kc = s_child(t, (int32_t)g, s->key[i].name, -1); if (kc < 0 || !t->n[kc].vlen) continue;
            uint64_t h = kh(s->key[i].tier, t->n[kc].val, t->n[kc].vlen); size_t at = h & (cap - 1); while (k->ix_h[at]) { if (k->ix_h[at] == h) break; at = (at + 1) & (cap - 1); }
            if (!k->ix_h[at]) { k->ix_h[at] = h; k->ix_g[at] = (int32_t)g; } } }       /* the first that names a key keeps it */
}
static int32_t keyed(Sink *k, const STree *t, const char *tier, const uint8_t *v, size_t n){
    if (k->ix_tree != t || k->ix_count != t->count) index_keys(k, t);
    uint64_t h = kh(tier, v, n); size_t at = h & (k->ix_cap - 1); while (k->ix_h[at]) { if (k->ix_h[at] == h) return k->ix_g[at]; at = (at + 1) & (k->ix_cap - 1); }
    return -1;
}
static int left_empty(const Say *s, const SNode *x){ return s_empty(&s->lay, x->val, x->vlen); }
static Ref text_of(const uint8_t *p, size_t n){ return n > 256 ? text_ref(CTX[omp_get_thread_num()], p, n) : string_ref(p, n); }
static int thing_of(Sink *k, const STree *t, int32_t g, Ref *out, int depth);
static int thing_compose(Sink *k, const STree *t, int32_t g, Ref *out, int depth);
/* What a part is, by its disposition: content, the type it is a key of, the thing it is a key of; or nothing. */
typedef struct { char k[16][160]; int n; } Keys;
static void key_form(const KSpec *ks, const uint8_t *v, size_t n, Keys *out);
static int value_of(Sink *k, const STree *t, int32_t c, Ref *out, int depth){
    const SNode *x = &t->n[c]; const Say *s = k->s; const Dis *d = dis_of(s, x);
    if (x->kind == S_GROUP && x->join == S_PIECES && x->first >= 0 && t->n[x->first].next >= 0) {      /* a list of values: the tuple of what each is ([display text, target]) */
        Ref pc[64]; uint32_t np = 0; for (int32_t q = x->first; q >= 0 && np < 64; q = t->n[q].next) { if (t->n[q].kind == S_GROUP) return 0; if (value_of(k, t, q, &pc[np], depth)) { pc[np].said = 0; np++; } }
        if (!np) return 0; *out = np == 1 ? pc[0] : said_tuple(compose(pc, np, over(pc, np))); return 1; }
    if (x->kind == S_GROUP && depth < 16) {                                    /* a part that is a thing: that thing */
        if (k->tc_tree == t && (uint32_t)c < k->tc_n && !composing(c)) { Ref v; if (thing_of(k, t, c, &v, depth + 1)) { *out = v; return 1; } }
        int32_t only = x->first; if (only >= 0 && t->n[only].next < 0 && t->n[only].kind == S_GROUP) return value_of(k, t, only, out, depth + 1); }     /* holding one part: what that part is */
    if (x->kind == S_GROUP) {                                                 /* a part that holds only its text: that text, as the part is disposed of */
        int32_t only = x->first; if (only < 0 || t->n[only].next >= 0 || t->n[only].kind != S_TEXT) return 0;
        x = &t->n[only];
        if (!d) { if (left_empty(s, x)) return 0; *out = text_of(x->val, x->vlen); return 1; }
        c = only; }
    if (left_empty(s, x)) return 0;
    if (!d) { if (x->kind == S_TEXT) { *out = text_of(x->val, x->vlen); return 1; } if (x->nlen) left_open(k, x); return 0; }
    if (d->what == D_CONTENT) {
        if (s->selfmark && k->selfcp >= 0 && memchr(x->val, s->selfmark, x->vlen)) {    /* the code point itself, written out where the source writes its mark */
            char hex[16]; int hl = snprintf(hex, sizeof hex, "%04lX", k->selfcp); uint8_t *o = malloc(x->vlen * 8 + 8); size_t j = 0;
            for (uint32_t i = 0; i < x->vlen; i++) { if (x->val[i] == (uint8_t)s->selfmark) { memcpy(o + j, hex, (size_t)hl); j += (size_t)hl; } else o[j++] = x->val[i]; }
            *out = text_of(o, j); free(o); return 1; }
        *out = text_of(x->val, x->vlen); return 1; }
    if (d->what == D_OWN) { Ref w[3] = { k->fw, string_ref(x->name, x->nlen), string_ref(x->val, x->vlen) }; w[0].said = 0; *out = said_tuple(compose(w, 3, over(w, 3))); return 1; }
    if (d->what == D_TYPE) {                                                 /* by the source's key of it; or the type whose content this text is; a text that is neither is left open */
        char lists[64], *save = NULL; snprintf(lists, sizeof lists, "%s", d->arg);    /* the first of its lists that knows it; a key is the key without the spaces written around it */
        const uint8_t *kp = x->val; size_t kn = x->vlen;
        for (int more = 1; more; ) { more = 0;                                /* white space around it, a no-break space (U+00A0) among it */
            if (kn && (*kp == ' ' || *kp == '\t')) { kp++; kn--; more = 1; } else if (kn >= 2 && kp[0] == 0xC2 && kp[1] == 0xA0) { kp += 2; kn -= 2; more = 1; }
            if (kn && (kp[kn - 1] == ' ' || kp[kn - 1] == '\t' || kp[kn - 1] == '\r')) { kn--; more = 1; } else if (kn >= 2 && kp[kn - 2] == 0xC2 && kp[kn - 1] == 0xA0) { kn -= 2; more = 1; } }
        Keys kf; if (d->k.has_re || d->k.as[0]) { kf.n = 0; key_form(&d->k, kp, kn, &kf); if (!kf.n) return 0; kp = (const uint8_t *)kf.k[0]; kn = strlen(kf.k[0]); }     /* the key as the lists write it; a key the pattern does not match is no key of theirs */
        Ref v = string_ref(kp, kn);
        for (char *ln = strtok_r(lists, " ", &save); ln; ln = strtok_r(NULL, " ", &save)) { int has = 0; *out = highway_typed(ln, kp, kn, &has); if (has) return 1;
            const lp_list *L = HW ? lp_highway_list(HW, ln) : NULL; if (L && lp_highway_slot(HW, L, &v.id) >= 0) { *out = v; return 1; } }
        k->unknown++; if (k->nunk < 8) snprintf(k->unknm[k->nunk++], 96, "%.*s %.*s", (int)(x->nlen < 40 ? x->nlen : 40), x->name, (int)(x->vlen < 48 ? x->vlen : 48), x->val);     /* a key no list of the highway holds: said, never taken for text */
        return 0; }
    if (d->what == D_CODEPOINTS || d->what == D_RANGE) {
        self_cp = k->selfcp; self_mark = s->selfmark;
        const uint8_t *p = x->val; size_t n = x->vlen; while (n && *p == ' ') { p++; n--; } while (n && p[n - 1] == ' ') n--;
        const uint8_t *dd = d->what == D_RANGE ? memmem(p, n, "..", 2) : NULL;
        if (dd) {                                                            /* a range, written as its first and its last: the path of the two */
            uint8_t a[8], b[8]; size_t al, bl; if (!codepoints_text(p, (size_t)(dd - p), a, sizeof a, &al) || !codepoints_text(dd + 2, n - (size_t)(dd - p) - 2, b, sizeof b, &bl)) { *out = text_of(p, n); return 1; }   /* not code points: the text it is written as */
            Ref two[2] = { string_ref(a, al), string_ref(b, bl) }; *out = said_tuple(compose(two, 2, over(two, 2))); return 1; }
        uint8_t buf[512]; size_t l; if (!codepoints_text(p, n, buf, sizeof buf, &l)) { if (!n) return 0; *out = text_of(p, n); return 1; }
        *out = string_ref(buf, l); return 1; }
    if (d->what == D_REFER && depth < 16) {                                  /* the first of its targets that holds the key */
        char targets[64], *save = NULL; snprintf(targets, sizeof targets, "%s", d->arg);
        for (char *tg = strtok_r(targets, " ", &save); tg; tg = strtok_r(NULL, " ", &save)) { int within = *tg == '^'; if (within) tg++;
            if (!is_local(s, tg)) { if (keys_get(tg, x->val, x->vlen, out)) return 1; continue; }      /* a key of another of the source's files: the thing its row is */
            int32_t g = keyed(k, t, tg, x->val, x->vlen); if (g < 0) continue;                            /* in the same tree: by the index */
            if (within) { for (int32_t up = t->n[g].parent; up >= 0; up = t->n[up].parent) if (thing_of(k, t, up, out, depth + 1)) return 1; continue; }   /* the thing what holds the key is inside */
            if (thing_of(k, t, g, out, depth + 1)) return 1; }
        k->unknown++; if (k->nunk < 8) snprintf(k->unknm[k->nunk++], 96, "%.*s %.*s", (int)(x->nlen < 40 ? x->nlen : 40), x->name, (int)(x->vlen < 48 ? x->vlen : 48), x->val);
        return 0; }
    return 0;
}
/* What a part speaks of: its own thing, or, for a part that is no thing, the nearest thing it is inside. */
static int spoken_thing(Sink *k, const STree *t, int32_t g, Ref *out){
    for (int32_t up = g; up >= 0; up = t->n[up].parent) if (thing_of(k, t, up, out, 0)) return 1;
    return 0;
}
static int entity_of(Sink *k, const STree *t, int32_t c, Ref *out);
static uint32_t entities_wide(Sink *k, const STree *t, int32_t g, Ref *kid);
static int thing_of(Sink *k, const STree *t, int32_t g, Ref *out, int depth){
    int memo = k->tc_tree == t && (uint32_t)g < k->tc_n;                     /* each thing is composed once in a tree; the threads reading one tree share what is composed */
    if (memo) { uint8_t st = __atomic_load_n(&k->ts[g], __ATOMIC_ACQUIRE); if (st == 1) { *out = k->tc[g]; return 1; } if (st == 2) return 0; }
    if (memo && composing(g)) return 0;                                            /* being composed by this thread: a thing named by itself names nothing */
    int pushed = memo && nbusy < BUSY_MAX; if (pushed) busy[nbusy++] = g;
    int r = thing_compose(k, t, g, out, depth);
    if (pushed) nbusy--;
    if (memo) { if (r) k->tc[g] = *out; __atomic_store_n(&k->ts[g], (uint8_t)(r ? 1 : 2), __ATOMIC_RELEASE); }
    return r;
}
/* A part as one way its kind is named: 0 when it is not named that way. */
static int one_thing(Sink *k, const STree *t, int32_t g, const Thing *th, Ref *out, int depth){
    const SNode *x = &t->n[g]; Ref p[8]; {
        if (!strcmp(th->name[0], ".")) { if (!entity_of(k, t, g, out)) return 0; out->said = 0; return 1; }        /* the thing is its content: the part itself, whole */
        if (th->of) {                                                        /* the path of what its parts of those names are, in the file's order */
            size_t cap = 64, np = 0; Ref *pc = malloc(sizeof(Ref) * cap);
            for (int32_t c = x->first; c >= 0; c = t->n[c].next) { int hit = 0; for (int z = 0; z < th->n && !hit; z++) hit = named_as(&t->n[c], th->name[z]); if (!hit) continue;
                Ref v; if (!(t->n[c].kind == S_GROUP ? thing_of(k, t, c, &v, depth + 1) : value_of(k, t, c, &v, depth))) continue;
                if (np == cap) { cap *= 2; pc = xrealloc(pc, sizeof(Ref) * cap); } v.said = 0; pc[np++] = v; }
            if (!np) { free(pc); return 0; } *out = np == 1 ? pc[0] : compose(pc, (uint32_t)np, over(pc, np)); out->said = 0; free(pc); return 1; }
        if (th->span) {                                                      /* the stretch of the text of TEXT, found in what it is inside, from the character START to END */
            int32_t a = child_path(t, g, th->name[0]), b = child_path(t, g, th->name[1]); if (a < 0 || b < 0 || !t->n[a].vlen || !t->n[b].vlen || t->n[a].vlen > 11 || t->n[b].vlen > 11) return 0;
            char za[16], zb[16]; memcpy(za, t->n[a].val, t->n[a].vlen); za[t->n[a].vlen] = 0; memcpy(zb, t->n[b].val, t->n[b].vlen); zb[t->n[b].vlen] = 0; char *e1, *e2; long lo = strtol(za, &e1, 10), hi = strtol(zb, &e2, 10);
            if (*e1 || *e2 || lo < 0 || hi < lo) return 0; if (th->inclusive) hi++;
            int32_t tx = -1; for (int32_t up = x->parent; up >= 0 && tx < 0; up = t->n[up].parent) tx = s_child(t, up, th->name[2], -1);
            if (tx < 0) return 0; const SNode *tn = &t->n[tx]; if (tn->kind == S_GROUP) { if (tn->first < 0) return 0; tn = &t->n[tn->first]; }
            size_t i = 0, blo = 0, bhi = 0; long ch = 0; int have = 0;                 /* characters counted as the source counts them: code points */
            for (; i <= tn->vlen; ch++) { if (ch == lo) { blo = i; have = 1; } if (ch == hi) { bhi = i; break; } if (i == tn->vlen) return 0;
                uint8_t c = tn->val[i]; i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
            if (!have || bhi <= blo || bhi > tn->vlen) return 0;
            *out = text_of(tn->val + blo, bhi - blo); out->said = 0; return 1; }
        for (int z = 0; z < th->n; z++) { int32_t c = child_path(t, g, th->name[z]); if (c < 0) return 0;
            if (t->n[c].kind == S_GROUP && t->n[c].first >= 0 && t->n[t->n[c].first].next >= 0) {      /* a part that is several pieces: the path of what each is, in order */
                Ref pc[256]; uint32_t np = 0; for (int32_t q = t->n[c].first; q >= 0 && np < 256; q = t->n[q].next) if (value_of(k, t, q, &pc[np], depth)) { pc[np].said = 0; np++; }
                if (!np) return 0; p[z] = np == 1 ? pc[0] : compose(pc, np, over(pc, np)); }
            else if (!value_of(k, t, c, &p[z], depth)) return 0;
            p[z].said = 0; }
        Ref X = th->n == 1 ? p[0] : said_tuple(compose(p, (uint32_t)th->n, over(p, (size_t)th->n)));      /* named by several parts together: the path of them */
        if (th->within && depth < 8) {                                       /* its name stands only within what it is inside: the pair of the two */
            Ref P; int32_t up = x->parent; while (up >= 0 && !thing_of(k, t, up, &P, depth + 1)) up = t->n[up].parent;
            if (up >= 0) { Ref two[2] = { P, X }; two[0].said = two[1].said = 0; X = said_tuple(compose(two, 2, over(two, 2))); } }
        if (th->infile) { Ref two[2] = { k->fname, X }; two[0].said = two[1].said = 0; X = said_tuple(compose(two, 2, over(two, 2))); }      /* its name stands only within the file: [the file's name, it] */
        *out = X; return 1; }
}
/* A part as the thing it is: the first of the ways its kind is named that names it (a code point, or a range). */
static int thing_compose(Sink *k, const STree *t, int32_t g, Ref *out, int depth){
    const SNode *x = &t->n[g]; const Say *s = k->s;
    for (int i = 0; i < s->nthing; i++) if (named_as(x, s->thing[i].tier) && one_thing(k, t, g, &s->thing[i], out, depth)) return 1;
    return 0;
}
/* Whether a part is one of those that name the thing it is in (an entry's word and pos). */
static int names_it(const Say *s, const SNode *x, const SNode *y){
    for (int i = 0; i < s->nthing; i++) { if (!named_as(x, s->thing[i].tier)) continue;
        for (int z = 0; z < s->thing[i].n; z++) if (!strchr(s->thing[i].name[z], '/') && s_named(y, s->thing[i].name[z], strlen(s->thing[i].name[z]))) return 1; }
    return 0;
}
/* Whether a part meets the conditions named: the outermost part the recipe speaks of (where ...), or a claim said
 * only of the parts that meet them (when ...). */
static int spoken_of(const Say *s, const STree *t, int32_t root, uint32_t which){
    for (int i = 0; i < s->nwhere; i++) { if (!(which & (1u << i))) continue; const Where *w = &s->where[i]; int32_t c = s_child(t, root, w->name, -1);
        if (c >= 0 && t->n[c].kind == S_GROUP && t->n[c].first >= 0 && t->n[t->n[c].first].next < 0 && t->n[t->n[c].first].kind != S_GROUP) c = t->n[c].first;     /* a part that holds only its text */
        const uint8_t *v = c >= 0 ? t->n[c].val : (const uint8_t *)""; size_t n = c >= 0 ? t->n[c].vlen : 0;
        if (w->op == 2) { char stack[512], *z = n < sizeof stack ? stack : malloc(n + 1); memcpy(z, v, n); z[n] = 0; int hit = !regexec(&w->re, z, 0, NULL, 0); if (z != stack) free(z); if (!hit) return 0; }
        else { int same = n == strlen(w->val) && !memcmp(v, w->val, n); if (same == (w->op == 1)) return 0; } }
    return 1;
}
/* A node of the file's tree as the entity it is: a record of what the source teaches. A part is what its disposition
 * says; a part that is itself parts is the path of them, a KEY IS VALUE part the pair of the two; and whatever holds
 * parts is the path of what they are, in the file's order. A key is how the file's rows point at each other, and is in
 * no record (key: written nowhere); a reference is the thing it names, never its key's text. A row, a line and the
 * source's numbering are packaging, never content (Recipes 10.3, 10.8). What parts a tier (a tab, a line's end) is the
 * layout's, written in the recipe: it is how the file is written down, and is not a constituent of anything. */
static int entity_of(Sink *k, const STree *t, int32_t c, Ref *out){
    const SNode *x = &t->n[c]; const Say *s = k->s; const SNode *up = x->parent >= 0 ? &t->n[x->parent] : NULL;
    if (x->kind != S_GROUP) {
        if (left_empty(s, x)) return 0;
        if (up && up->kind == S_GROUP && up->tier == x->tier && up->nlen) {       /* a part of a part */
            const Dis *dp = x->nlen ? dis_of(s, x) : NULL; if (dp && (dp->what == D_OMIT || dp->what == D_METADATA)) return 0;
            if (x->kind == S_VALUE && x->nlen) { Ref p[2] = { string_ref(x->name, x->nlen), string_ref(x->val, x->vlen) };      /* a KEY IS VALUE part: the pair of its key and what its value is */
                if (dp && dp->what == D_KEY) return 0;                                  /* the file's own numbering: in no record */
                if (dp && !value_of(k, t, c, &p[1], 0)) return 0;                     /* a reference: the thing it names */
                if (p[1].said != LP_SAID_TUPLE) p[1].said = 0; *out = said_tuple(compose(p, 2, over(p, 2))); return 1; }
            if (x->nlen && value_of(k, t, c, out, 0)) return 1;                 /* a piece read as its part is (a type, a code point) */
            *out = string_ref(x->val, x->vlen); return 1; }
        const Dis *d = x->nlen ? dis_of(s, x) : NULL;
        if (!d) { if (x->nlen && x->kind != S_TEXT) { left_open(k, x); return 0; } *out = text_of(x->val, x->vlen); return 1; }     /* a text is the text it is */
        if (d->what == D_OMIT || d->what == D_METADATA) return 0;
        if (d->what == D_KEY) return 0;                                      /* the file's own numbering: in no record */
        return value_of(k, t, c, out, 0);                                    /* a reference: the thing it names, or nothing */
    }
    const Dis *d = x->nlen ? dis_of(s, x) : NULL; if (d && (d->what == D_OMIT || d->what == D_METADATA)) return 0;
    Ref stack[64], *kid = x->nkids <= 64 ? stack : malloc(sizeof(Ref) * x->nkids); uint32_t n = 0;
    if (x->nkids >= FAN && !k->worker) { n = entities_wide(k, t, c, kid); goto composed; }       /* its parts on every core, kept in order */
    for (int32_t q = x->first; q >= 0; q = t->n[q].next) { Ref v; if (entity_of(k, t, q, &v)) { if (v.said != LP_SAID_TUPLE) v.said = 0; kid[n++] = v; } }
    composed:
    if (n == 1) *out = kid[0]; else if (n > 1) *out = compose(kid, n, over(kid, n));
    if (kid != stack) free(kid);
    return n > 0;
}
/* Who says what follows: the part named, as the witness it names ([the source's witness, NAME, the name], or what an
 * own part already is). 0 when the part names no one. */
static int voice_of(Sink *k, const STree *t, int32_t c){
    if (c < 0 || left_empty(k->s, &t->n[c])) return 0; const SNode *y = &t->n[c]; const Dis *d = dis_of(k->s, y);
    if (d && d->what == D_OWN) { if (!value_of(k, t, c, &k->voice, 0)) return 0; }
    else { Ref w[3] = { k->fw, string_ref(y->name, y->nlen), string_ref(y->val, y->vlen) }; w[0].said = 0; k->voice = said_tuple(compose(w, 3, over(w, 3))); }
    k->voiced = 1; return 1;
}
static int said_by(Sink *k, const STree *t, int32_t g, const char *by){ return voice_of(k, t, child_path(t, g, by)); }
/* The witnesses a part names for what it says (by PATH): every one the path names, each saying it in its own voice.
 * Where the part names none, the source says it, as it says everything it writes. */
#define WHO 64
static int whos(const STree *t, int32_t g, const char *by, int32_t *who){ return by[0] ? all_path(t, g, by, who, WHO) : 0; }
static void claim(Sink *k, Ref *part, int n){
    for (int i = 0; i < n; i++) if (part[i].said != LP_SAID_TUPLE) part[i].said = 0;      /* a tuple held by a claim stays a tuple: M says so, the ID is the same */
    Ref c = said_claim(compose(part, (uint32_t)n, over(part, (size_t)n)));
    Event x = { c.id, c.id, k->score, k->er, k->ed, 0, EV_CLAIM }; if (k->voiced) { x.own_witness = 1; x.witness = k->voice.id; } ev_push(&k->ev, &x);
    push(&k->grp, &c);
}
/* What a part said, with everything inside it, said together: its claims one record, the path of them, witnessed once,
 * each claim within it. e0 .. the end: the events it and what is inside it made. */
static void together(Sink *k, uint64_t e0, const Ref *about){
    uint64_t m = k->ev.n - e0; if (m < 2) return;
    Ref *c = malloc(sizeof(Ref) * (size_t)(m + 1)); uint32_t n = 0;
    if (about) { c[n] = *about; c[n].said = 0; n++; }                         /* the record is of what the part is, first: a sentence with what is said within it */
    for (uint64_t i = e0; i < k->ev.n; i++) { if (k->ev.e[i].kind != EV_CLAIM) continue; Node *x = table_find(&k->ev.e[i].claim); if (!x) continue;
        Ref r; memset(&r, 0, sizeof r); r.id = x->id; memcpy(r.c.m, x->m, sizeof r.c.m); r.tier = x->tier; r.said = LP_SAID_CLAIM; c[n++] = r; }
    if (n < 2 + (about ? 1 : 0)) { free(c); return; }
    Ref rec = said_record(compose(c, n, over(c, n))); free(c);
    Event r = { rec.id, rec.id, 1.0f, k->er, k->ed, 0, EV_RECORD }; ev_push(&k->ev, &r);
    memmove(&k->ev.e[e0 + 1], &k->ev.e[e0], sizeof(Event) * (size_t)(k->ev.n - 1 - e0)); k->ev.e[e0] = r;     /* the record first: its claims are within the record before them */
    for (uint64_t i = e0 + 1; i < k->ev.n; i++) if (k->ev.e[i].kind == EV_CLAIM) { k->ev.e[i].kind = EV_MEMBER; k->ev.e[i].witnessed = rec.id; }
}
/* The last node inside a part: its subtree is the nodes from it to there. */
static int32_t subtree_end(const STree *t, int32_t g){ while (t->n[g].last >= 0) g = t->n[g].last; return g; }
/* The score a part of a tier gives what it attests: the number the recipe names, on the scale the source writes it on
 * (its lowest a loss, its highest a win, halfway a draw); a win where it gives none. */
static float score_of(const Say *s, const STree *t, int32_t g){
    const SNode *x = &t->n[g];
    for (int i = 0; i < s->nscore; i++) if (s_named(x, s->score[i].tier, strlen(s->score[i].tier))) { int32_t c = s_child(t, g, s->score[i].name, -1); if (c < 0 || !t->n[c].vlen || t->n[c].vlen > 31) return 1.0f;
        char z[32]; memcpy(z, t->n[c].val, t->n[c].vlen); z[t->n[c].vlen] = 0; char *e; double v = strtod(z, &e); if (e == z) return 1.0f;
        v = (v - s->score[i].from) / (s->score[i].to - s->score[i].from); return (float)(v < 0 ? 0 : v > 1 ? 1 : v); }
    return 1.0f;
}
/* A key as the highway keeps it: the text as written, or, where the line gives a pattern, what it matches written as
 * the template says (\0 the whole, \1 to \9 its parts; with no template, its first part, or the whole). */
static void key_form(const KSpec *ks, const uint8_t *v, size_t n, Keys *out){
    if (out->n == 16 || !n || n >= 160) return; char z[160]; memcpy(z, v, n); z[n] = 0;
    regmatch_t m[10]; for (int i = 0; i < 10; i++) m[i].rm_so = m[i].rm_eo = -1; m[0].rm_so = 0; m[0].rm_eo = (regoff_t)n;
    if (ks->has_re && regexec(&ks->re, z, 10, m, 0)) return;
    const char *tp = ks->as[0] ? ks->as : ks->has_re && m[1].rm_so >= 0 ? "\\1" : "\\0"; char *o = out->k[out->n]; size_t k = 0;
    for (const char *c = tp; *c && k + 1 < 160; c++) {
        if (*c == '\\' && c[1] >= '0' && c[1] <= '9') { int d = c[1] - '0'; c++; if (m[d].rm_so < 0) continue; size_t l = (size_t)(m[d].rm_eo - m[d].rm_so); if (k + l >= 160) return; memcpy(o + k, z + m[d].rm_so, l); k += l; }
        else o[k++] = *c; }
    o[k] = 0; if (k) out->n++;
}
/* Every key a part names at a path: each value there, and each piece of a value written in pieces. */
static void keys_at(const Say *s, const STree *t, int32_t g, const KSpec *ks, Keys *out){
    out->n = 0; int32_t at[WHO]; int na = all_path(t, g, ks->path, at, WHO);
    for (int i = 0; i < na; i++) { const SNode *x = &t->n[at[i]];
        if (x->kind != S_GROUP) { if (!left_empty(s, x)) key_form(ks, x->val, x->vlen, out); continue; }
        for (int32_t q = x->first; q >= 0; q = t->n[q].next) if (t->n[q].kind != S_GROUP && !left_empty(s, &t->n[q])) key_form(ks, t->n[q].val, t->n[q].vlen, out); }
}
/* The file's key for a part, kept for the source's other files that point at it (refer NAME RECIPE). */
static void keep_keys(Sink *k, const STree *t, int32_t root){
    const Say *s = k->s;
    for (int i = 0; i < s->nkey; i++) if (s_named(&t->n[root], s->key[i].tier, strlen(s->key[i].tier))) { int32_t kc = s_child(t, root, s->key[i].name, -1); Ref S;
        if (kc >= 0 && t->n[kc].vlen && thing_of(k, t, root, &S, 0)) keys_put(k->r->name, t->n[kc].val, t->n[kc].vlen, S); }
}
/* A part read for the highway: the types it is, their keys, and the keys it maps to one another. */
static void unit_highway(Sink *k, const STree *t, int32_t root){
    const Say *s = k->s; if (!spoken_of(s, t, root, s->whole)) return;
    for (uint32_t g = 0; g < t->count; g++) { const SNode *x = &t->n[g]; if (x->kind == S_NOTE || !x->nlen) continue;
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
static void unit_range(Sink *k, const STree *t, uint32_t g0, uint32_t g1, int speaks, Pend *pend, int *npp);
static void worker_of(Sink *w, const Sink *k){
    *w = *k; memset(&w->ev, 0, sizeof w->ev); memset(&w->things, 0, sizeof w->things); memset(&w->meta, 0, sizeof w->meta); memset(&w->grp, 0, sizeof w->grp);
    memset(w->open, 0, sizeof w->open); w->nopen = 0; w->unknown = 0; w->nunk = 0; w->hr = NULL; w->nhr = w->chr = 0; w->worker = 1;
}
static void sink_merge(Sink *k, Sink *w){
    for (uint64_t i = 0; i < w->ev.n; i++) ev_push(&k->ev, &w->ev.e[i]);
    for (size_t i = 0; i < w->meta.n; i++) push(&k->meta, &w->meta.c[i]);
    for (size_t i = 0; i < w->things.n; i++) push(&k->things, &w->things.c[i]);
    for (int i = 0; i < 256; i++) k->open[i] += w->open[i];
    for (int i = 0; i < w->nopen; i++) { int dup = 0; for (int j = 0; j < k->nopen && !dup; j++) dup = !strcmp(k->opennm[j], w->opennm[i]); if (!dup && k->nopen < 16) strcpy(k->opennm[k->nopen++], w->opennm[i]); }
    k->unknown += w->unknown; for (int i = 0; i < w->nunk && k->nunk < 8; i++) strcpy(k->unknm[k->nunk++], w->unknm[i]);
    free(w->ev.e); free(w->meta.c); free(w->things.c); free(w->grp.c);
}
/* A thread may take up a part while it waits inside its own reading: what it was in the middle of is put back after. */
typedef struct { const STree *tt; int16_t *dm; int base, nbusy; long self_cp; char self_mark; } Tls;
static Tls tls_save(const STree *t, int16_t *dm){ Tls x = { TT, DM, busy_base, nbusy, self_cp, self_mark }; TT = t; DM = dm; busy_base = nbusy; return x; }
static void tls_back(const Tls *x){ TT = x->tt; DM = x->dm; busy_base = x->base; nbusy = x->nbusy; self_cp = x->self_cp; self_mark = x->self_mark; }
/* Parts of g in runs of about the same number of nodes; returns the last node inside g. */
static size_t runs_of(const STree *t, int32_t g, uint32_t **a, uint32_t **b){
    uint32_t end = (uint32_t)subtree_end(t, g) + 1, total = end - (uint32_t)g - 1, want = total / (uint32_t)(omp_get_max_threads() * 16) + 1, from = (uint32_t)g + 1;
    size_t n = 0, cap = 64; *a = malloc(sizeof(uint32_t) * cap); *b = malloc(sizeof(uint32_t) * cap);
    for (int32_t c = t->n[g].first; c >= 0; c = t->n[c].next) { uint32_t ce = (uint32_t)subtree_end(t, c) + 1;
        if (ce - from >= want || t->n[c].next < 0) { if (n == cap) { cap *= 2; *a = xrealloc(*a, sizeof(uint32_t) * cap); *b = xrealloc(*b, sizeof(uint32_t) * cap); } (*a)[n] = from; (*b)[n++] = ce; from = ce; } }
    return n;
}
static uint32_t wide(Sink *k, const STree *t, int32_t g, int speaks){
    uint32_t last = (uint32_t)subtree_end(t, g); if (last <= (uint32_t)g) return (uint32_t)g;
    if (k->ix_tree != t || k->ix_count != t->count) index_keys(k, t);      /* built once, read by every thread */
    uint32_t *a, *b; size_t n = runs_of(t, g, &a, &b); Sink *w = calloc(n, sizeof(Sink));
    #pragma omp taskloop grainsize(1)
    for (size_t i = 0; i < n; i++) { Tls was = tls_save(t, k->dm); worker_of(&w[i], k); Pend pd[64]; int np = 0;
        unit_range(&w[i], t, a[i], b[i], speaks, pd, &np); while (np) { np--; together(&w[i], pd[np].e0, pd[np].has ? &pd[np].about : NULL); }
        tls_back(&was); }
    for (size_t i = 0; i < n; i++) sink_merge(k, &w[i]);
    free(w); free(a); free(b);
    return last;
}
/* The parts of a wide part, whole (entity_of), on every core: each run of them into a sink of its own, in order. */
static uint32_t entities_wide(Sink *k, const STree *t, int32_t g, Ref *kid){
    uint32_t nk = 0; for (int32_t q = t->n[g].first; q >= 0; q = t->n[q].next) nk++;
    int32_t *ch = malloc(sizeof(int32_t) * nk); uint8_t *ok = calloc(nk, 1); { uint32_t i = 0; for (int32_t q = t->n[g].first; q >= 0; q = t->n[q].next) ch[i++] = q; }
    if (k->ix_tree != t || k->ix_count != t->count) index_keys(k, t);
    uint32_t per = nk / (uint32_t)(omp_get_max_threads() * 16) + 1, nr = (nk + per - 1) / per; Sink *w = calloc(nr, sizeof(Sink));
    #pragma omp taskloop grainsize(1)
    for (uint32_t r = 0; r < nr; r++) { Tls was = tls_save(t, k->dm); worker_of(&w[r], k);
        for (uint32_t i = r * per; i < nk && i < (r + 1) * per; i++) { Ref v; if (entity_of(&w[r], t, ch[i], &v)) { if (v.said != LP_SAID_TUPLE) v.said = 0; kid[i] = v; ok[i] = 1; } }
        tls_back(&was); }
    for (uint32_t r = 0; r < nr; r++) sink_merge(k, &w[r]);
    uint32_t n = 0; for (uint32_t i = 0; i < nk; i++) if (ok[i]) kid[n++] = kid[i];
    free(w); free(ch); free(ok); return n;
}
/* A tree read from node g0 up to g1, in the file's order. pend: the parts whose claims are said together, still open. */
static void unit_range(Sink *k, const STree *t, uint32_t g0, uint32_t g1, int speaks, Pend *pend, int *npp){
    const Say *s = k->s; int np_ = *npp;
    for (uint32_t g = g0; g < g1; g++) { const SNode *x = &t->n[g];
        while (np_ && pend[np_ - 1].end < (int32_t)g) { np_--; together(k, pend[np_].e0, pend[np_].has ? &pend[np_].about : NULL); }
        if (x->kind == S_NOTE || x->kind == S_VALUE) { const Dis *d = dis_of(s, x);
            if (d && d->what == D_METADATA && !left_empty(s, x)) { Ref p[2] = { string_ref(x->name, x->nlen), text_of(x->val, x->vlen) }; Ref m = said_tuple(compose(p, 2, over(p, 2))); push(&k->meta, &m); }
            continue; }
        if (x->kind != S_GROUP || !speaks) continue;
        if (x->nlen) { const Dis *dg = dis_of(s, x); if (dg && dg->what == D_OMIT) { g = (uint32_t)subtree_end(t, (int32_t)g); continue; } }     /* the file's bookkeeping: nothing inside it is read */
        k->selfcp = -1;
        if (s->selfmark) { Ref X; if (spoken_thing(k, t, (int32_t)g, &X) && X.tier == 0) k->selfcp = (long)lp_tier0_codepoint(T0, &X.id); }     /* the code point the part speaks of, for the source's mark */
        Ref S; int has = -1; float sc = score_of(s, t, (int32_t)g); k->score = s->nitself ? 1.0f : sc; k->voiced = 0;      /* the score is the attested tuple's, where there is one; what is said of it is said outright */
        if (s->voice.tier[0] && s_named(x, s->voice.tier, strlen(s->voice.tier))) said_by(k, t, (int32_t)g, s->voice.name);     /* who says everything the part says */
        uint64_t e0 = k->ev.n; k->grp.n = 0;
        for (int a = 0; a < s->nvoices; a++) { if (!s_named(x, s->voices[a].tier, strlen(s->voices[a].tier)) || !spoken_of(s, t, (int32_t)g, s->voices[a].when)) continue;   /* each such part a witness of its own: [the source's witness, NAME] */
            if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break; int was = k->voiced; Ref wv = k->voice;
            for (int z = 0; z < s->voices[a].n; z++) for (int32_t c = x->first; c >= 0; c = t->n[c].next) { const SNode *y = &t->n[c]; Ref v; if (!y->nlen || !named_as(y, s->voices[a].name[z]) || !value_of(k, t, c, &v, 0)) continue;
                Ref w[3] = { k->fw, k->fname, string_ref(y->name, y->nlen) }; int nw = 2; if (s->voices[a].file) nw = 3; else w[1] = w[2]; w[0].said = 0; w[1].said = 0;     /* [the source's witness, NAME], or within the file [witness, file, NAME] */
                k->voice = said_tuple(compose(w, (uint32_t)nw, over(w, (size_t)nw))); k->voiced = 1; Ref p[2] = { S, v }; claim(k, p, 2); }
            k->voiced = was; k->voice = wv; }
        for (int a = 0; a < s->nitself; a++) { if (!s_named(x, s->itself[a].tier, strlen(s->itself[a].tier)) || !spoken_of(s, t, (int32_t)g, s->itself[a].when)) continue;
            Ref X; if (!thing_of(k, t, (int32_t)g, &X, 0)) break;
            int32_t who[WHO]; int nw = whos(t, (int32_t)g, s->itself[a].by, who), was = k->voiced; Ref wv = k->voice;
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !voice_of(k, t, who[wi])) continue;
                Event e = { X.id, X.id, sc, k->er, k->ed, 0, EV_CLAIM }; if (k->voiced) { e.own_witness = 1; e.witness = k->voice.id; } ev_push(&k->ev, &e); }
            k->voiced = was; k->voice = wv; }
        for (int a = 0; a < s->npair; a++) { if (!named_as(x, s->pair[a].tier) || !spoken_of(s, t, (int32_t)g, s->pair[a].when)) continue;      /* nothing written between the two: the pair */
            Ref P;
            if (s->pair[a].of[0]) { int32_t oc = child_path(t, (int32_t)g, s->pair[a].of); if (oc < 0 || !value_of(k, t, oc, &P, 0)) continue; }      /* of another of its parts */
            else { if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break; P = S; }
            for (int z = 0; z < s->pair[a].n; z++) { Ref v;
                if (!strcmp(s->pair[a].name[z], "{file}")) v = k->fstem;           /* what the file's own name says of each of its parts */
                else { int32_t c = child_path(t, (int32_t)g, s->pair[a].name[z]); if (c < 0) continue;
                    if (t->n[c].kind == S_GROUP && t->n[c].join == S_PIECES && t->n[c].first >= 0 && t->n[t->n[c].first].next >= 0) {      /* a list: the pair with each */
                        for (int32_t q = t->n[c].first; q >= 0; q = t->n[q].next) { Ref w; if (t->n[q].kind == S_GROUP || !value_of(k, t, q, &w, 0) || !memcmp(&w.id, &P.id, 16)) continue; Ref p[2] = { P, w }; claim(k, p, 2); }
                        continue; }
                    if (!value_of(k, t, c, &v, 0)) continue; }
                if (!memcmp(&v.id, &P.id, 16)) continue; Ref p[2] = { P, v }; claim(k, p, 2); } }
        for (int a = 0; a < s->natt; a++) { if (!named_as(x, s->att[a].tier) || !spoken_of(s, t, (int32_t)g, s->att[a].when)) continue;
            Ref S0 = S; int had = has, was = k->voiced; Ref wv = k->voice;
            if (s->att[a].of[0]) { int32_t oc = child_path(t, (int32_t)g, s->att[a].of); if (oc < 0 || !value_of(k, t, oc, &S, 0)) continue; has = 1; }      /* said of another of its parts */
            else { if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break; }
            int32_t who[WHO]; int nw = whos(t, (int32_t)g, s->att[a].by, who);
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !voice_of(k, t, who[wi])) continue;
            for (int z = 0; z < s->att[a].n; z++) for (int32_t c = x->first; c >= 0; c = t->n[c].next) { const SNode *y = &t->n[c];
                if (!strcmp(s->att[a].name[z], "*/")) {                      /* each element inside it that is a value: its own text, or its one attribute (rdf:resource), under the element's name */
                    if (y->kind != S_GROUP || !y->nlen || y->join == S_PIECES) continue; int32_t only = -1, nk = 0;
                    for (int32_t q = y->first; q >= 0; q = t->n[q].next) { const Dis *dq = t->n[q].nlen && t->n[q].kind == S_VALUE ? dis_of(s, &t->n[q]) : NULL; if (dq && dq->what == D_OMIT) continue; nk++; only = q; }   /* what is omitted does not count */
                    if (nk != 1 || (t->n[only].kind != S_TEXT && t->n[only].kind != S_VALUE) || left_empty(s, &t->n[only])) continue;
                    Ref v; if (!value_of(k, t, only, &v, 0) || !memcmp(&v.id, &S.id, 16)) continue; Ref p[3] = { S, string_ref(y->name, y->nlen), v }; claim(k, p, 3); continue; }
                if (!strcmp(s->att[a].name[z], ".")) {                       /* its own text, under its own name */
                    if (y->kind != S_TEXT || left_empty(s, y)) continue; Ref v = text_of(y->val, y->vlen); if (!memcmp(&v.id, &S.id, 16)) continue;
                    Ref p[3] = { S, string_ref(x->name, x->nlen), v }; claim(k, p, 3); continue; }
                if (!y->nlen || y->kind == S_TEXT || !named_as(y, s->att[a].name[z])) continue;
                if (!s->att[a].of[0] && names_it(s, x, y)) continue;                         /* a part that names the thing is said already: it is the thing */
                if (y->kind == S_GROUP && y->join != S_PIECES && strchr(s->att[a].name[z], '*')) continue;      /* a wildcard names the part's values, not the elements inside it, which speak for themselves */
                if (y->kind == S_GROUP) {                                    /* parts: each VALUE under its KEY, or each piece, read as the part is, under the part's own name */
                    for (int32_t q = y->first; q >= 0; q = t->n[q].next) { const SNode *w = &t->n[q]; if (!w->vlen) continue; Ref v;
                        if (w->kind == S_VALUE && w->nlen && !s_named(w, (const char *)y->name, y->nlen)) { const Dis *dw = dis_of(s, w); if (dw && (dw->what == D_OMIT || dw->what == D_METADATA)) continue;      /* the file's bookkeeping says nothing */
                            if (in_perf(s, (const char *)w->name, w->nlen)) continue;
                            Ref vw; if (!dw || !value_of(k, t, q, &vw, 0)) vw = text_of(w->val, w->vlen); Ref p[3] = { S, string_ref(w->name, w->nlen), vw }; claim(k, p, 3); continue; }
                        if (!value_of(k, t, q, &v, 0) || !memcmp(&v.id, &S.id, 16)) continue; Ref p[3] = { S, string_ref(y->name, y->nlen), v }; claim(k, p, 3); } }
                else { if (in_perf(s, (const char *)y->name, y->nlen)) continue;
                    Ref v; if (!value_of(k, t, c, &v, 0) || !memcmp(&v.id, &S.id, 16)) continue; Ref p[3] = { S, string_ref(y->name, y->nlen), v }; claim(k, p, 3); } }
            }
            if (s->att[a].of[0]) { S = S0; has = had; }
            k->voiced = was; k->voice = wv; }
        for (int a = 0; a < s->nhold; a++) { if (!named_as(x, s->hold[a].tier) || !spoken_of(s, t, (int32_t)g, s->hold[a].when)) continue;   /* the things inside it, under their own name */
            if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break;
            /* the nearest things of those names inside it, however deep: what holds them and is no thing is how the file groups them */
            size_t scap = 1024, sp = 0; int32_t *stack = malloc(sizeof(int32_t) * scap);
            { size_t nk = 0; for (int32_t q = x->first; q >= 0; q = t->n[q].next) nk++; if (nk > scap) { scap = nk * 2; stack = xrealloc(stack, sizeof(int32_t) * scap); }
              sp = nk; size_t i = nk; for (int32_t q = x->first; q >= 0; q = t->n[q].next) stack[--i] = q; }      /* in the file's order: the first on top */
            while (sp) { int32_t c = stack[--sp]; const SNode *y = &t->n[c]; if (y->kind != S_GROUP) continue; Ref v; int named = 0;
                for (int z = 0; z < s->hold[a].n && !named; z++) named = named_as(y, s->hold[a].name[z]);
                if (named && thing_of(k, t, c, &v, 0)) { if (memcmp(&v.id, &S.id, 16)) {
                        const SNode *by = y; if (s->hold[a].file) { int32_t via = c; while (t->n[via].parent >= 0 && t->n[via].parent != (int32_t)g) via = t->n[via].parent; by = &t->n[via]; }    /* via: the element between them names the relation */
                        Ref p[3] = { S, string_ref(by->name, by->nlen), v }; claim(k, p, 3); } continue; }
                { Ref tmp; if (thing_of(k, t, c, &tmp, 0)) continue; }               /* another thing: what it holds is its own */
                size_t nk = 0; for (int32_t q = y->first; q >= 0; q = t->n[q].next) nk++;
                if (sp + nk > scap) { while (sp + nk > scap) scap *= 2; stack = xrealloc(stack, sizeof(int32_t) * scap); }
                size_t i = sp + nk; for (int32_t q = y->first; q >= 0; q = t->n[q].next) stack[--i] = q; sp += nk; }
            free(stack); }
        for (int a = 0; a < s->nrel; a++) { if (!named_as(x, s->rel[a].tier) || !spoken_of(s, t, (int32_t)g, s->rel[a].when)) continue;
            if (has < 0) has = spoken_thing(k, t, (int32_t)g, &S); if (!has) break;
            int was = k->voiced; Ref wv = k->voice; int32_t who[WHO]; int nw = whos(t, (int32_t)g, s->rel[a].by, who);
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !voice_of(k, t, who[wi])) continue;
            Ref rel, o;
            if (!strcmp(s->rel[a].rel, "{file}")) rel = k->fstem;                /* the relation the file's own name gives (noun.exc: noun) */
            else { int32_t rc = child_path(t, (int32_t)g, s->rel[a].rel); if (rc < 0 || !value_of(k, t, rc, &rel, 0)) break; }
            if (!strcmp(s->rel[a].to, "...")) {                              /* to each of the parts the file gives no name */
                for (int32_t c = x->first; c >= 0; c = t->n[c].next) { const SNode *y = &t->n[c]; if (y->kind != S_TEXT || y->nlen || left_empty(s, y)) continue; o = text_of(y->val, y->vlen); Ref p[3] = { S, rel, o }; claim(k, p, 3); } }
            else if (s_child(t, (int32_t)g, s->rel[a].to, s_child(t, (int32_t)g, s->rel[a].to, -1)) >= 0) {   /* several parts of that name: the relation to each */
                for (int32_t oc = s_child(t, (int32_t)g, s->rel[a].to, -1); oc >= 0; oc = s_child(t, (int32_t)g, s->rel[a].to, oc)) if (value_of(k, t, oc, &o, 0)) { Ref p[3] = { S, rel, o }; claim(k, p, 3); } }
            else { int32_t oc = child_path(t, (int32_t)g, s->rel[a].to);
                if (oc >= 0 && t->n[oc].kind == S_GROUP && t->n[oc].first >= 0 && t->n[t->n[oc].first].next >= 0) {      /* several: the relation to each */
                    for (int32_t q = t->n[oc].first; q >= 0; q = t->n[q].next) if (value_of(k, t, q, &o, 0)) { Ref p[3] = { S, rel, o }; claim(k, p, 3); } }
                else if (oc >= 0 && value_of(k, t, oc, &o, 0)) { Ref p[3] = { S, rel, o }; claim(k, p, 3); }
                else if (s->rel[a].alone && oc >= 0) { Ref p[2] = { S, rel }; claim(k, p, 2); } }      /* to what names nothing (a treebank's root): the relation alone, where the recipe says so; otherwise no claim */
            }
            k->voiced = was; k->voice = wv; }
        for (int a = 0; a < s->ntogether; a++) if (s_named(x, s->together[a].tier, strlen(s->together[a].tier)) && spoken_of(s, t, (int32_t)g, s->together[a].when)) {
            if (np_ < 64) { pend[np_].end = subtree_end(t, (int32_t)g); pend[np_].e0 = e0; pend[np_].has = thing_of(k, t, (int32_t)g, &pend[np_].about, 0); np_++; } break; }      /* closed when the reading passes the last node inside it */
        if (x->nkids >= FAN && !k->worker) g = wide(k, t, (int32_t)g, speaks);      /* its parts, each read on its own, on every core */
    }
    *npp = np_;
}
static void unit(void *sink, const STree *t, int32_t root, uint64_t ordinal){
    Sink *k = sink; const Say *s = k->s; (void)ordinal; uint64_t ev0 = k->ev.n; k->score = 1.0f; TT = t;
    if (k->dm_cap < t->count) { k->dm_cap = t->count * 2; k->dm = xrealloc(k->dm, sizeof(int16_t) * k->dm_cap); } memset(k->dm, 0, sizeof(int16_t) * t->count); DM = k->dm;
    if (k->tc_cap < t->count) { k->tc_cap = t->count * 2; k->tc = xrealloc(k->tc, sizeof(Ref) * k->tc_cap); k->ts = xrealloc(k->ts, k->tc_cap); }
    k->tc_tree = t; k->tc_n = t->count; memset(k->ts, 0, t->count); k->ix_tree = NULL;      /* a new tree: nothing of the last is known of it */
    if (k->hw) { unit_highway(k, t, root); keep_keys(k, t, root); return; }     /* read for the highway: its types, not what it attests */
    if (s->nline && t->n[root].kind == S_TEXT) {                             /* a line of a page: where it matches a pattern, it says the pattern's parts */
        const SNode *x = &t->n[root]; char stack[4096], *ln = x->vlen < sizeof stack ? stack : malloc(x->vlen + 1); memcpy(ln, x->val, x->vlen); ln[x->vlen] = 0;
        for (int i = 0; i < s->nline; i++) { regmatch_t m[5]; if (regexec(&s->line[i].re, ln, 5, m, 0)) continue;
            Ref part[4]; int np = 0; for (int q = 1; q < 5 && np < 4; q++) if (m[q].rm_so >= 0 && m[q].rm_eo > m[q].rm_so) part[np++] = text_of((const uint8_t *)ln + m[q].rm_so, (size_t)(m[q].rm_eo - m[q].rm_so));
            if (s->line[i].mode == 3 && np == 2) { Ref p[3] = { part[0], string_ref((const uint8_t *)s->line[i].pred, strlen(s->line[i].pred)), part[1] }; claim(k, p, 3); }
            else if (s->line[i].mode == 2 && np == 2) claim(k, part, 2);
            else if (s->line[i].mode == 1 && np >= 2) claim(k, part, np);
            else if (s->line[i].mode == 0 && np == 2 && k->has_fabout) { Ref p[3] = { k->fabout, part[0], part[1] }; claim(k, p, 3); } }
        if (ln != stack) free(ln); }
    int speaks = spoken_of(s, t, root, s->whole);
    Pend pend[64]; int np_ = 0;      /* the parts whose claims are said together, still open */
    unit_range(k, t, 0, t->count, speaks, pend, &np_);
    while (np_) { np_--; together(k, pend[np_].e0, pend[np_].has ? &pend[np_].about : NULL); }
    keep_keys(k, t, root);
    /* the part itself, whole, into the file's content tree; where it is the very tuple it attests (a row that is a
     * claim), the tree holds it as the claim it is. A part the recipe does not speak of (a row its where leaves out)
     * is in no tree: a recipe that says what a file's parts are reads a curated source, mined for what it says and
     * not kept byte for byte (Storage: curated sources are mined for knowledge, not recorded bit-perfect) */
    Ref T; int whole = speaks ? entity_of(k, t, root, &T) : 0;
    if (whole) { T.said = 0; for (uint64_t e = ev0; e < k->ev.n; e++) if (!memcmp(&k->ev.e[e].claim, &T.id, 16)) { T.said = LP_SAID_CLAIM; break; } push(&k->things, &T); }
}

/* What a file holds, in its order, as one composition. A few thousand parts are one path. More are factored into
 * blocks from the content alone (Storage: repeated blocks are factored from the content alone): a block ends after a
 * part whose own ID says so (one in BLOCK, by its bits), never at a count or a position, so the same parts always part
 * the same way, a part put in or taken out moves only its own block, and a run of parts that repeats is one block
 * wherever it stands. The blocks are composed the same way, level by level, until one holds them all: no path is
 * longer than a row can hold, and a file of millions of records is a tree of its own content. */
#define BLOCK 4096
static Ref blocks_of(Ref *r, size_t n){
    if (n == 1) return r[0];
    if (n <= BLOCK) return compose(r, (uint32_t)n, over(r, n));
    Ref *up = malloc(sizeof(Ref) * n); size_t m = 0, from = 0;
    for (size_t i = 0; i < n; i++) { uint64_t b; memcpy(&b, r[i].id.b + 8, 8);
        if ((b & (BLOCK - 1)) == 0 || i == n - 1) { Ref x = i - from + 1 == 1 ? r[from] : compose(r + from, (uint32_t)(i - from + 1), over(r + from, i - from + 1)); if (x.said != LP_SAID_TUPLE && x.said != LP_SAID_CLAIM) x.said = 0; up[m++] = x; from = i + 1; } }
    Ref out = m == n ? compose(r, (uint32_t)n, over(r, n)) : blocks_of(up, m); free(up); return out;
}
/* The OS's record of a file, as parts named as the OS names them: its pathname and filename (POSIX), and what statx
 * returns. Each is disposed of by the file's recipe, else by the stock recipe file, as any part is: metadata, a
 * [name, value] in the file's metadata tree; omit, nowhere; neither, an obligation left open and said once. A pathname's
 * value is its filenames, one composition: a path is a Merkle DAG of its folders. */
static const Recipe *stock_file;
void file_record_stock(const Recipe *r){ stock_file = r; }
int say_only_disposes(const Recipe *r){ const Say *s = r->say; return s && s->ndis && !s->nthing && !s->natt && !s->nrel && !s->nhw && !s->npair && !s->nhold && !s->nkey; }
static const Dis *dis_named(const Say *s, const char *name){
    if (!s) return NULL; SNode x; memset(&x, 0, sizeof x); x.kind = S_VALUE; x.name = (const uint8_t *)name; x.nlen = (uint32_t)strlen(name); x.parent = -1;
    const STree *was = TT; TT = NULL; const Dis *d = dis_scan(s, &x); TT = was; return d;
}
static uint64_t record_open;                                                  /* the parts no recipe disposed of, said once */
static void record_part(const Recipe *r, Ref *out, size_t *n, size_t cap, int k, const char *name, Ref value){
    const Dis *d = dis_named(r ? r->say : NULL, name); if (!d && stock_file) d = dis_named(stock_file->say, name);
    if (!d) { if (!(__atomic_fetch_or(&record_open, 1ull << k, __ATOMIC_RELAXED) & (1ull << k))) fprintf(stderr, "  the OS's record of a file: %s is disposed of by no recipe (metadata or omit, in the file recipe)\n", name); return; }
    if (d->what != D_METADATA || *n >= cap) return;
    Ref p[2] = { string_ref((const uint8_t *)name, strlen(name)), value }; p[0].said = p[1].said = 0;
    out[(*n)++] = said_tuple(compose(p, 2, over(p, 2)));
}
static Ref number_ref(uint64_t v){ char b[24]; int l = snprintf(b, sizeof b, "%llu", (unsigned long long)v); return string_ref((const uint8_t *)b, (size_t)l); }
static Ref when_ref(const struct statx_timestamp *t){ char b[40]; int l = snprintf(b, sizeof b, "%lld.%09u", (long long)t->tv_sec, t->tv_nsec); return string_ref((const uint8_t *)b, (size_t)l); }
size_t file_record(const Recipe *r, const File *f, Ref *out, size_t cap){
    size_t n = 0; int k = 0; Ref seg[256]; uint32_t ns = 0; const char *p = f->path;
    while (*p && ns < 256) { const char *e = strchr(p, '/'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l) { seg[ns] = string_ref((const uint8_t *)p, l); seg[ns].said = 0; ns++; } p += l; if (*p == '/') p++; }
    if (ns) { Ref path = compose(seg, ns, over(seg, ns)); path.said = 0; record_part(r, out, &n, cap, k++, "pathname", path); record_part(r, out, &n, cap, k++, "filename", seg[ns - 1]); }
    struct statx x; if (statx(AT_FDCWD, f->path, 0, STATX_BASIC_STATS | STATX_BTIME, &x)) return n;
    #define NUM(F, M) do { if (!(M) || (x.stx_mask & (M))) record_part(r, out, &n, cap, k, #F, number_ref((uint64_t)x.F)); k++; } while (0)
    #define WHEN(F, M) do { if (x.stx_mask & (M)) record_part(r, out, &n, cap, k, #F, when_ref(&x.F)); k++; } while (0)
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
    *out = compose(m, (uint32_t)n, over(m, n)); out->said = 0; return 1;
}
/* The file, read: every part of its outermost tier on every core, joined in the file's order; then its trunk, over
 * its metadata tree and its content tree. */
static void read_laid(const Recipe *r, File *f, const uint8_t *src, size_t n, Hw *hw){
    Say s = *(const Say *)r->say;
    const char *base = strrchr(f->path, '/'); base = base ? base + 1 : f->path; size_t bl = strlen(base); if (bl > 3 && !strcmp(base + bl - 3, ".gz")) bl -= 3;
    const char *sb = base; if (s.stemfrom) { const char *f0 = NULL; for (const char *c = base; c < base + bl; c++) if (*c == s.stemfrom) f0 = c; if (f0) sb = f0 + 1; }
    const char *dot = memchr(sb, s.stem ? s.stem : '.', (size_t)(base + bl - sb)); Ref fname = string_ref((const uint8_t *)base, bl), fstem = string_ref((const uint8_t *)sb, dot ? (size_t)(dot - sb) : (size_t)(base + bl - sb));    /* the file's own name, and the part of it {file} stands for */
    Ref fabout; int has_fabout = 0; memset(&fabout, 0, sizeof fabout);
    if (s.has_about) for (size_t c = 0; c < n && !has_fabout; ) {           /* what the page is about: the first line that names it */
        const uint8_t *nl = memchr(src + c, '\n', n - c); size_t e = nl ? (size_t)(nl - src) : n, len = e - c; if (len && src[e - 1] == '\r') len--;
        if (len && len < 4096) { char ln[4096]; memcpy(ln, src + c, len); ln[len] = 0; regmatch_t m[2];
            if (!regexec(&s.about_re, ln, 2, m, 0) && m[1].rm_so >= 0 && m[1].rm_eo > m[1].rm_so) { fabout = string_ref((const uint8_t *)ln + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so)); has_fabout = 1; } }
        c = nl ? e + 1 : n; }
    float er = 1500.0f, ed = 0.0f;                                           /* a claim enters at the rating of the unrated, with the deviation its witness's trust plays with */
    size_t nc = 1, *cut = malloc(sizeof(size_t) * 2); Sink *part;
    if (s.lay.g.n && r->lang && s.lay.ntier) {                               /* parts of the file, each a tree the grammar gives (a JSON value on every line): on every core */
        size_t want = n / (2u << 20) + 1; cut = xrealloc(cut, sizeof(size_t) * (want + 2)); cut[0] = 0;
        for (size_t i = 1; i < want; i++) { size_t c = s_boundary(&s.lay, src, n, n / want * i); if (c > cut[nc - 1] && c < n) cut[nc++] = c; }
        cut[nc] = n; part = calloc(nc, sizeof(Sink)); const STier *t0 = &s.lay.tier[0];
        #pragma omp taskloop grainsize(1)
        for (size_t i = 0; i < nc; i++) { Sink *k = &part[i]; k->r = r; k->s = &s; k->er = er; k->ed = ed; k->fw = f->witness; k->fname = fname; k->fstem = fstem; k->hw = hw;
            TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, r->lang);
            for (size_t a = cut[i]; a < cut[i + 1]; ) { const uint8_t *e0 = memmem(src + a, cut[i + 1] - a, t0->sep, (size_t)t0->seplen); size_t e = e0 ? (size_t)(e0 - src) : cut[i + 1];
                size_t b = a; while (b < e && (src[b] == ' ' || src[b] == '\t' || src[b] == '\r')) b++;
                if (b < e) { TSTree *tt = ts_parser_parse_string(ps, NULL, (const char *)src + a, (uint32_t)(e - a)); TSNode root = ts_tree_root_node(tt);
                    if (ts_node_has_error(root)) k->open[1]++; else s_grammar(&s.lay, &root, src + a, e - a, unit, k); ts_tree_delete(tt); }
                a = e0 ? e + (size_t)t0->seplen : cut[i + 1]; }
            ts_parser_delete(ps); }
        uint64_t bad = 0; for (size_t i = 0; i < nc; i++) bad += part[i].open[1];
        if (bad) { fprintf(stderr, "\n  %s: %llu parts of %s do not parse by the grammar and are not read\n", r->name, (unsigned long long)bad, f->path); f->incomplete += bad; }
    } else if (s.lay.g.n && r->lang && s.nsplit && n > (8u << 20)) {
        /* a long file of records, each an element on lines of its own (split ELEMENT...): parted before lines that
         * begin one, each part parsed as a document of its own on every core; what stands around them is read once */
        #define BEGINS(c) ({ size_t b_ = (c); while (b_ < n && (src[b_] == ' ' || src[b_] == '\t')) b_++; int y_ = 0; \
            if (b_ < n && src[b_] == '<') for (int q_ = 0; q_ < s.nsplit && !y_; q_++) { size_t l_ = strlen(s.split[q_]); y_ = b_ + 1 + l_ < n && !memcmp(src + b_ + 1, s.split[q_], l_) && (src[b_ + 1 + l_] == ' ' || src[b_ + 1 + l_] == '>' || src[b_ + 1 + l_] == '/'); } y_; })
        /* every record, from the line that begins it to where its element closes; every other line stands around them */
        typedef struct { size_t a, b; } Span; size_t nrs = 0, crs = 4096; Span *rs = malloc(sizeof(Span) * crs);
        uint8_t *around = malloc(n + 1); size_t alen = 0;
        for (size_t c = 0; c < n; ) {
            const uint8_t *nl = memchr(src + c, '\n', n - c); size_t le = nl ? (size_t)(nl - src) + 1 : n;
            if (!BEGINS(c)) { memcpy(around + alen, src + c, le - c); alen += le - c; c = le; continue; }
            int depth = 0; size_t e = c;
            for (; e < n; e++) { if (src[e] != '<') continue;
                if (e + 1 < n && src[e + 1] == '/') { const uint8_t *g = memchr(src + e, '>', n - e); size_t ge = g ? (size_t)(g - src) + 1 : n; if (--depth <= 0) { e = ge; break; } e = ge - 1; }
                else if (e + 1 < n && src[e + 1] != '!' && src[e + 1] != '?') { const uint8_t *g = memchr(src + e, '>', n - e); size_t ge = g ? (size_t)(g - src) + 1 : n;
                    if (g && g[-1] == '/') { if (depth == 0) { e = ge; break; } } else depth++; e = ge - 1; } }
            { const uint8_t *nl2 = e < n ? memchr(src + e, '\n', n - e) : NULL; e = nl2 ? (size_t)(nl2 - src) + 1 : n; }
            if (nrs == crs) { crs *= 2; rs = xrealloc(rs, sizeof(Span) * crs); } rs[nrs].a = c; rs[nrs++].b = e; c = e;
        }
        /* parsed a part at a time on every core, read as the one tree the whole file is (s_grammar_split) */
        #undef BEGINS
        size_t *ra = malloc(sizeof(size_t) * (nrs + 1)), *rb = malloc(sizeof(size_t) * (nrs + 1)); for (size_t i = 0; i < nrs; i++) { ra[i] = rs[i].a; rb[i] = rs[i].b; }
        part = calloc(1, sizeof(Sink)); part[0].r = r; part[0].s = &s; part[0].er = er; part[0].ed = ed; part[0].fw = f->witness; part[0].fname = fname; part[0].fstem = fstem; part[0].hw = hw;
        uint64_t bad = s_grammar_split(&s.lay, r->lang, src, n, ra, rb, nrs, unit, &part[0]); nc = 1;
        free(ra); free(rb); free(rs); free(around);
        if (bad) { fprintf(stderr, "\n  %s: %llu parts of %s do not parse whole by the grammar; what parses is read\n", r->name, (unsigned long long)bad, f->path); f->incomplete += bad; }
    } else if (s.lay.g.n && r->lang) {                                       /* the grammar gives the tree: the file parsed once, its tree read by the same dispositions */
        part = calloc(1, sizeof(Sink)); part[0].r = r; part[0].s = &s; part[0].er = er; part[0].ed = ed; part[0].fw = f->witness; part[0].fname = fname; part[0].fstem = fstem; part[0].hw = hw;
        TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, r->lang); TSTree *tt = ts_parser_parse_string(ps, NULL, (const char *)src, (uint32_t)n);
        TSNode root = ts_tree_root_node(tt); if (ts_node_has_error(root)) { fprintf(stderr, "\n  %s: %s does not parse whole by its grammar; what parses is read\n", r->name, f->path); f->incomplete++; }
        s_grammar(&s.lay, &root, src, n, unit, &part[0]); ts_tree_delete(tt); ts_parser_delete(ps);
    } else {
        size_t at = s_head(&s.lay, src, n);                                  /* a header's names are this file's */
        size_t want = (n - at) / (2u << 20) + 1; cut = xrealloc(cut, sizeof(size_t) * (want + 2)); cut[0] = at;
        for (size_t i = 1; i < want; i++) { size_t c = s_boundary(&s.lay, src, n, at + (n - at) / want * i); if (c > cut[nc - 1] && c < n) cut[nc++] = c; }
        cut[nc] = n; part = calloc(nc, sizeof(Sink));
        #pragma omp taskloop grainsize(1)
        for (size_t i = 0; i < nc; i++) { part[i].r = r; part[i].s = &s; part[i].er = er; part[i].ed = ed; part[i].fw = f->witness; part[i].fname = fname; part[i].fstem = fstem; part[i].hw = hw; part[i].fabout = fabout; part[i].has_fabout = has_fabout; s_decompose(&s.lay, src + cut[i], cut[i + 1] - cut[i], unit, &part[i]); }
    }
    Refs things = { 0 }, meta = { 0 }; uint64_t open = 0;
    for (size_t i = 0; i < nc; i++) {
        for (uint64_t j = 0; j < part[i].ev.n; j++) ev_push(&f->ev, &part[i].ev.e[j]);
        for (size_t j = 0; j < part[i].things.n; j++) push(&things, &part[i].things.c[j]);
        for (size_t j = 0; j < part[i].meta.n; j++) push(&meta, &part[i].meta.c[j]);
        open += part[i].open[0]; free(part[i].ev.e); free(part[i].things.c); free(part[i].meta.c);
        free(part[i].tc); free(part[i].ts); free(part[i].dm); free(part[i].ix_h); free(part[i].ix_g); free(part[i].grp.c); }
    if (open) { fprintf(stderr, "\n  %s: %llu named parts of %s have no disposition in the recipe: they are left open, not read:", r->name, (unsigned long long)open, f->path);
        char seen[64][64]; int ns = 0;
        for (size_t i = 0; i < nc; i++) for (int j = 0; j < part[i].nopen; j++) { int dup = 0; for (int z = 0; z < ns && !dup; z++) dup = !strcmp(seen[z], part[i].opennm[j]); if (dup || ns == 64) continue; snprintf(seen[ns++], 64, "%s", part[i].opennm[j]); fprintf(stderr, " %s", part[i].opennm[j]); }
        fputc('\n', stderr); f->incomplete += open; }
    { uint64_t unk = 0; for (size_t i = 0; i < nc; i++) unk += part[i].unknown;
      if (unk) { fprintf(stderr, "\n  %s: %llu keys in %s name a type no list of the highway holds, and say nothing; for instance:", r->name, (unsigned long long)unk, f->path);
          for (size_t i = 0, shown = 0; i < nc && shown < 3; i++) for (int j = 0; j < part[i].nunk && shown < 3; j++, shown++) fprintf(stderr, " [%s]", part[i].unknm[j]); fputc('\n', stderr); } }
    if (hw) {                                                                /* for the highway: what the file says of its types, in the file's order */
        for (size_t i = 0; i < nc; i++) { for (size_t j = 0; j < part[i].nhr; j++) { HwRec *x = &part[i].hr[j]; const HwLine *h = &s.hw[x->line];
                if (x->what == 1) hw_type(hw, h->a.list, h->say, x->thing);
                else if (x->what == 2) hw_key(hw, h->a.list, x->thing, x->a);
                else if (x->what == 3) hw_alias(hw, h->a.list, x->a, x->b);
                else hw_edge(hw, h->a.list, x->a, h->b.list, x->b);
                free(x->a); free(x->b); }
            free(part[i].hr); }
        free(things.c); free(meta.c); free(part); free(cut); return; }
    /* the trunk: [metadata, content]. The metadata tree: the file's name, and what its parts say of the file itself.
     * The content tree: the things of its outermost tier, in the file's order. */
    /* A file read a stretch at a time gathers what each stretch held, and its trunk is made when the last is read. */
    f->laid = 1; f->has_file = 0;
    { uint64_t need = f->nsaid + things.n + meta.n; if (need > f->csaid) { f->csaid = need * 2 + 1024; f->said = xrealloc(f->said, f->csaid * sizeof(Ref)); }
      for (size_t j = 0; j < meta.n; j++) { Ref x = meta.c[j]; x.said = LP_SAID_METADATA; f->said[f->nsaid++] = x; }
      for (size_t j = 0; j < things.n; j++) { Ref x = things.c[j]; if (x.said == LP_SAID_METADATA) x.said = 0; f->said[f->nsaid++] = x; } }
    if (f->partial) { free(things.c); free(meta.c); free(part); free(cut); return; }
    things.n = meta.n = 0;
    for (uint64_t j = 0; j < f->nsaid; j++) { Ref x = f->said[j]; if (x.said == LP_SAID_METADATA) { x.said = LP_SAID_TUPLE; push(&meta, &x); } else push(&things, &x); }
    free(f->said); f->said = NULL; f->nsaid = f->csaid = 0;
    Ref *m = malloc(sizeof(Ref) * (meta.n + 1)); size_t nr = (size_t)file_os(r, f, m);          /* the OS's record of it, one node, first */
    memcpy(m + nr, meta.c, sizeof(Ref) * meta.n);
    size_t nm = nr + meta.n; Ref metadata; memset(&metadata, 0, sizeof metadata); if (nm) metadata = blocks_of(m, nm); free(m);
    if (things.n) { Ref content = blocks_of(things.c, things.n); content.said = 0;
        if (nm) { Ref two[2] = { said_metadata(metadata), content }; two[1].said = 0; f->file = compose(two, 2, over(two, 2)); } else f->file = content;
        f->file.said = 0; f->has_file = 1; f->trunk = content; }
    else if (f->ev.n && nm) { Ref two[1] = { said_metadata(metadata) }; f->file = two[0]; f->file.said = 0; f->has_file = 0; }
    free(things.c); free(meta.c); free(part); free(cut);
}
void attest_layout(const Recipe *r, File *f, const uint8_t *src, size_t n){ read_laid(r, f, src, n, NULL); }
void say_highway(const Recipe *r, File *f, const uint8_t *src, size_t n, Hw *hw){ read_laid(r, f, src, n, hw); }
int  say_has_highway(const Recipe *r){ const Say *s = r->say; return s && s->nhw; }
