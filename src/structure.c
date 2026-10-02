/* The one decomposer: a file's bytes into the tree its recipe lays out (structure.h). Nothing here names a format
 * or a source. A tier's parts are found by its separator; the innermost tier's parts are texts; a named part the
 * recipe says is itself parts is parted again. Quotes, escapes, notes, comments and remarks are read where the
 * recipe's layout says the file writes them, and nowhere else. */
#define _GNU_SOURCE
#include "structure.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the tree */
static void tree_free(STree *t){ for (size_t i = 0; i < t->npool; i++) free(t->pool[i]); free(t->pool); free(t->n); memset(t, 0, sizeof *t); }
static void tree_reset(STree *t){ t->count = 0; if (t->npool > 1) { for (size_t i = 0; i < t->npool; i++) free(t->pool[i]); t->npool = 0; t->room = 0; } t->used = 0; }
static uint8_t *own(STree *t, size_t n){                               /* bytes the tree keeps; a block never moves */
    if (!t->npool || t->used + n > t->room) { size_t room = n > (1u << 20) ? n : (1u << 20);
        t->pool = realloc(t->pool, sizeof(uint8_t *) * (t->npool + 1)); t->pool[t->npool++] = malloc(room); t->room = room; t->used = 0; }
    uint8_t *p = t->pool[t->npool - 1] + t->used; t->used += n; return p;
}
static int32_t node(STree *t, uint8_t kind, uint8_t tier, int32_t parent, uint64_t at){
    if (t->count == t->cap) { t->cap = t->cap ? t->cap * 2 : 1024; t->n = realloc(t->n, sizeof(SNode) * t->cap); if (!t->n) { perror("realloc"); exit(1); } }
    int32_t i = (int32_t)t->count++; SNode *x = &t->n[i]; memset(x, 0, sizeof *x);
    x->kind = kind; x->tier = tier; x->parent = parent; x->first = x->last = x->next = -1; x->at = at;
    if (parent >= 0) { SNode *p = &t->n[parent]; if (p->last >= 0) t->n[p->last].next = i; else p->first = i; p->last = i; p->nkids++; }
    return i;
}
int32_t s_child(const STree *t, int32_t nd, const char *name, int32_t after){
    size_t l = strlen(name);
    for (int32_t c = after >= 0 ? t->n[after].next : t->n[nd].first; c >= 0; c = t->n[c].next) if (s_named(&t->n[c], name, l)) return c;
    return -1;
}
void s_print(const STree *t, int32_t nd, int depth, long *left){
    if (*left <= 0) return; (*left)--; const SNode *x = &t->n[nd];
    printf("%*s%s", depth * 2, "", x->kind == S_GROUP ? "group" : x->kind == S_VALUE ? "value" : x->kind == S_NOTE ? "note" : "text");
    if (x->nlen) printf(" %.*s", (int)x->nlen, x->name);
    if (x->kind != S_GROUP) { printf("  \""); for (uint32_t i = 0; i < x->vlen && i < 80; i++) { uint8_t c = x->val[i]; if (c == '\n') printf("\\n"); else if (c == '\t') printf("\\t"); else putchar(c); } printf(x->vlen > 80 ? "\xE2\x80\xA6\"" : "\""); }
    putchar('\n');
    for (int32_t c = x->first; c >= 0; c = t->n[c].next) s_print(t, c, depth + 1, left);
}

/* ---- the layout, as the recipe writes it */
static int sep_read(const char *w, char *out, size_t cap){                 /* \n \t \\, and space, tab, hash by name (a recipe's own comments begin with the hash); anything else as written */
    size_t k = 0; if (!w) return 0; if (!strcmp(w, "space")) { out[0] = ' '; out[1] = 0; return 1; } if (!strcmp(w, "tab")) { out[0] = '\t'; out[1] = 0; return 1; } if (!strcmp(w, "hash")) { out[0] = '#'; out[1] = 0; return 1; }
    for (; *w && k + 1 < cap; w++) { if (*w == '\\' && w[1]) { w++; out[k++] = *w == 'n' ? '\n' : *w == 't' ? '\t' : *w == 'r' ? '\r' : *w; } else out[k++] = *w; }
    out[k] = 0; return (int)k;
}
static char *name_next(char **at, char *out, size_t cap){                  /* a name of several words is written between double quotes */
    char *p = *at; if (!p) return NULL; while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\r' || *p == '\n') return NULL;
    char *e; if (*p == '"') { p++; e = strchr(p, '"'); if (!e) return NULL; snprintf(out, cap, "%.*s", (int)(e - p), p); *at = e + 1; }
    else { e = p; while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++; snprintf(out, cap, "%.*s", (int)(e - p), p); *at = e; }
    return out;
}
int layout_says(Layout *l, const char *path, char *tok){
    STier *t = l->ntier ? &l->tier[l->ntier - 1] : NULL;
    if (!strcmp(tok, "tier")) { char *nm = strtok(NULL, " \t\r\n"), *by = strtok(NULL, " \t\r\n"), *sp = strtok(NULL, " \t\r\n");
        if (!nm || !by || strcmp(by, "by") || !sp || l->ntier == S_TIERS) { fprintf(stderr, "%s: tier NAME by SEPARATOR (at most %d tiers)\n", path, S_TIERS); return -1; }
        t = &l->tier[l->ntier++]; memset(t, 0, sizeof *t); snprintf(t->name, sizeof t->name, "%s", nm); t->seplen = sep_read(sp, t->sep, sizeof t->sep);
        if (!t->seplen) { fprintf(stderr, "%s: tier %s is parted by nothing\n", path, nm); return -1; } return 1; }
    if (!strcmp(tok, "part")) { char *pa = strtok(NULL, " \t\r\n"), *by = strtok(NULL, " \t\r\n"), *sp = strtok(NULL, " \t\r\n"), *w;
        if (!pa || !by || strcmp(by, "by") || !sp || l->npart == 32) { fprintf(stderr, "%s: part NAME by SEPARATOR [is IS] [pieces N] [space CHAR]\n", path); return -1; }
        SPart *p = &l->part[l->npart++]; memset(p, 0, sizeof *p); snprintf(p->path, sizeof p->path, "%s", pa); p->seplen = sep_read(sp, p->sep, sizeof p->sep);
        while ((w = strtok(NULL, " \t\r\n"))) { char *v = strtok(NULL, " \t\r\n"), b[8];
            if (v && !strcmp(w, "is")) p->islen = sep_read(v, p->is, sizeof p->is);
            else if (v && !strcmp(w, "pieces") && atoi(v) > 0) p->pieces = atoi(v);
            else if (v && !strcmp(w, "space") && sep_read(v, b, sizeof b)) p->space = b[0];
            else { fprintf(stderr, "%s: part NAME by SEPARATOR [is IS] [pieces N] [space CHAR]\n", path); return -1; } }
        return 1; }
    if (!strcmp(tok, "trim")) { l->g.trim = 1; return 1; }
    if (!strcmp(tok, "levels")) { char *v; while ((v = strtok(NULL, " \t\r\n")) && l->nlevels < 8) snprintf(l->levels[l->nlevels++], 32, "%s", v);
        if (!l->nlevels) { fprintf(stderr, "%s: levels NAME...\n", path); return -1; } return 1; }
    if (!strcmp(tok, "resolve")) { char *v = strtok(NULL, " \t\r\n"); l->g.resolve = v && !strcmp(v, "xml") ? G_XML : v && !strcmp(v, "json") ? G_JSON : v && !strcmp(v, "turtle") ? G_TURTLE : 0;
        if (!l->g.resolve) { fprintf(stderr, "%s: resolve xml | json | turtle\n", path); return -1; } return 1; }
    if (!strcmp(tok, "node")) { char *ty = strtok(NULL, " \t\r\n"), *wh = strtok(NULL, " \t\r\n"), *w;
        if (!ty || !wh || l->g.n == 48) { fprintf(stderr, "%s: node TYPE group | value | text | member | skip ...\n", path); return -1; }
        GRule *r = &l->g.rule[l->g.n++]; memset(r, 0, sizeof *r); snprintf(r->type, sizeof r->type, "%s", ty);
        r->what = !strcmp(wh, "group") ? G_GROUP : !strcmp(wh, "value") ? G_VALUE : !strcmp(wh, "text") ? G_TEXT : !strcmp(wh, "member") ? G_MEMBER : !strcmp(wh, "skip") ? G_SKIP : 0;
        if (!r->what) { fprintf(stderr, "%s: node %s %s: group | value | text | member | skip\n", path, ty, wh); return -1; }
        int into = 0;                                                       /* 1: names follow; 2: the text's path follows */
        while ((w = strtok(NULL, " \t\r\n"))) { if (into == 2 && !r->text[0]) { snprintf(r->text, sizeof r->text, "%s", w); continue; }     /* the path after text/value is a path, whatever it is called */
            if (into == 1 && !r->nname) { snprintf(r->name[r->nname++], 64, "%s", w); continue; }
            if (!strcmp(w, "name")) into = 1; else if (!strcmp(w, "text") || !strcmp(w, "value")) into = 2; else if (!strcmp(w, "raw")) r->raw = 1; else if (!strcmp(w, "join")) r->join = 1; else if (!strcmp(w, "list")) r->list = 1; else if (!strcmp(w, "kind")) r->kind = 1;
            else if (into == 1 && r->nname < 4) snprintf(r->name[r->nname++], 64, "%s", w); else if (into == 2) snprintf(r->text, sizeof r->text, "%s", w);
            else { fprintf(stderr, "%s: node %s: %s is not something a node's line says\n", path, ty, w); return -1; } }
        if ((r->what == G_VALUE || r->what == G_MEMBER) && (!r->nname || !r->text[0])) { fprintf(stderr, "%s: node %s %s name PATH %s PATH\n", path, ty, wh, r->what == G_VALUE ? "text" : "value"); return -1; }
        return 1; }
    if (!strcmp(tok, "empty")) { char *v; int any = 0;
        while ((v = strtok(NULL, " \t\r\n")) && l->nempty < 16) { l->emptylen[l->nempty] = sep_read(v, l->empty[l->nempty], sizeof l->empty[0]); l->nempty++; any = 1; }
        if (!any) { fprintf(stderr, "%s: empty TEXT...\n", path); return -1; } return 1; }
    if (!t) return 0;                                                       /* what follows is said of the tier written last */
    if (!strcmp(tok, "names")) { char *at = strtok(NULL, "\r\n"), nm[64]; while (t->nnames < S_NAMES && name_next(&at, nm, sizeof nm)) snprintf(t->names[t->nnames++], 64, "%s", nm); return 1; }
    if (!strcmp(tok, "header")) { t->header = 1; return 1; }
    if (!strcmp(tok, "quoted")) { t->quoted = 1; return 1; }
    if (!strcmp(tok, "padded")) { t->padded = 1; return 1; }
    if (!strcmp(tok, "continued")) { t->continued = 1; return 1; }
    if (!strcmp(tok, "is")) { char *v = strtok(NULL, " \t\r\n"); if (!v || !(t->kvlen = sep_read(v, t->kv, sizeof t->kv))) { fprintf(stderr, "%s: is SEPARATOR\n", path); return -1; } return 1; }
    if (!strcmp(tok, "skip")) { char *v = strtok(NULL, " \t\r\n"); t->skip = v ? atoi(v) : 0; return 1; }
    if (!strcmp(tok, "note")) { char *p = strtok(NULL, " \t\r\n"), *is = strtok(NULL, " \t\r\n"); if (!p) { fprintf(stderr, "%s: note PREFIX [IS]\n", path); return -1; }
        t->notelen = sep_read(p, t->note, sizeof t->note); t->islen = is ? sep_read(is, t->note_is, sizeof t->note_is) : 0; return 1; }
    if (!strcmp(tok, "comment")) { char *p = strtok(NULL, " \t\r\n"); if (!p) { fprintf(stderr, "%s: comment PREFIX\n", path); return -1; } t->commentlen = sep_read(p, t->comment, sizeof t->comment); return 1; }
    if (!strcmp(tok, "remark")) { char *p = strtok(NULL, " \t\r\n"), b[8]; if (!p || !sep_read(p, b, sizeof b)) { fprintf(stderr, "%s: remark CHAR\n", path); return -1; } t->remark = b[0]; return 1; }
    if (!strcmp(tok, "escaped")) { char *p = strtok(NULL, " \t\r\n"); if (!p) { fprintf(stderr, "%s: escaped CHAR\n", path); return -1; } t->escaped = p[0]; return 1; }
    return 0;
}

/* ---- parting */
/* Where the part of tier k that begins at i ends: at the tier's separator, a quoted or escaped separator passed over
 * where the layout says the file writes them. *next: where the one after it begins. A carriage return before a line's
 * end belongs to the line's end. */
static size_t part_end(const Layout *l, int k, const uint8_t *s, size_t i, size_t hi, size_t *next){
    const STier *t = &l->tier[k]; const STier *up = k ? &l->tier[k - 1] : NULL; int quoted = up && up->quoted, sl = t->seplen; char esc = up ? up->escaped : t->escaped;
    size_t p = i;
    if (quoted && p < hi && s[p] == '"') { p++; while (p < hi) { if (s[p] == '"') { if (p + 1 < hi && s[p + 1] == '"') { p += 2; continue; } p++; break; } p++; } }
    /* a tier inside this one whose parts may stand between quotes (a row's fields): a quoted part may hold this tier's
     * separator (a line's end), so where one begins its quotes are passed over whole */
    int fq = -1; for (int j = k; j + 1 < l->ntier; j++) if (l->tier[j].quoted) { fq = j + 1; break; }
    const STier *ft = fq >= 0 ? &l->tier[fq] : NULL; int at_start = 1;
    for (; p < hi; p++) {
        if (ft && at_start && s[p] == '"') { p++; while (p < hi) { if (s[p] == '"') { if (p + 1 < hi && s[p + 1] == '"') { p += 2; continue; } break; } p++; } at_start = 0; continue; }
        at_start = 0;
        if (ft && s[p] == (uint8_t)ft->sep[0] && p + (size_t)ft->seplen <= hi && !memcmp(s + p, ft->sep, (size_t)ft->seplen) && !(s[p] == (uint8_t)t->sep[0] && !memcmp(s + p, t->sep, (size_t)sl))) { p += (size_t)ft->seplen - 1; at_start = 1; continue; }
        if (esc && s[p] == (uint8_t)esc && p + 1 < hi) { p++; continue; }
        if (s[p] == (uint8_t)t->sep[0] && p + (size_t)sl <= hi && !memcmp(s + p, t->sep, (size_t)sl)) { *next = p + (size_t)sl; return p; }
        if (sl == 2 && t->sep[0] == '\n' && t->sep[1] == '\n' && s[p] == '\n' && p + 2 < hi && s[p + 1] == '\r' && s[p + 2] == '\n') { *next = p + 3; return p; }
    }
    *next = hi; return hi;
}
static const uint8_t *trimmed(const uint8_t *p, size_t *n){ while (*n && (p[0] == ' ' || p[0] == '\t')) { p++; (*n)--; } while (*n && (p[*n - 1] == ' ' || p[*n - 1] == '\t' || p[*n - 1] == '\r')) (*n)--; return p; }
/* A text as the file means it: its quotes off and a doubled quote one; an escaped character itself. */
static void text_put(STree *t, int32_t x, const Layout *l, int k, const uint8_t *p, size_t n){
    const STier *up = k ? &l->tier[k - 1] : NULL; int quoted = up && up->quoted; char esc = up ? up->escaped : 0;
    if (n && p[n - 1] == '\r') n--;
    if (up && up->padded) p = trimmed(p, &n);
    if (quoted && n >= 2 && p[0] == '"' && p[n - 1] == '"') { p++; n -= 2;
        if (memmem(p, n, "\"\"", 2)) { uint8_t *o = own(t, n); size_t j = 0; for (size_t i = 0; i < n; i++) { o[j++] = p[i]; if (p[i] == '"' && i + 1 < n && p[i + 1] == '"') i++; } p = o; n = j; } }
    else if (esc && memchr(p, esc, n) && !(n == 2 && p[1] == 'N')) {       /* the escape and N alone is what a file writes for a part it leaves unknown: as written */
        uint8_t *o = own(t, n); size_t j = 0; for (size_t i = 0; i < n; i++) { if (p[i] == (uint8_t)esc && i + 1 < n) i++; o[j++] = p[i]; } p = o; n = j; }
    t->n[x].val = p; t->n[x].vlen = (uint32_t)n;
}
/* A JSON value written inside a part (part NAME by object), read into the part: an object's members each named by its
 * key, a list's values each named by the list, a text with its escapes resolved, a number or true or false as written;
 * null says nothing. */
size_t lp_utf8_put(uint32_t cp, uint8_t out[4]);
static int hex4(const uint8_t *p){ int v = 0; for (int i = 0; i < 4; i++) { int c = p[i]; v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1000000); } return v; }
/* A text's escapes resolved: JSON's, and Turtle's, which are the same and \U with eight digits; a code point past the
 * first plane written as its two halves is the one code point. o holds at least len bytes. */
static size_t esc_resolve(const uint8_t *s, size_t len, uint8_t *o){
    size_t k = 0;
    for (size_t i = 0; i < len; i++) { if (s[i] != '\\' || i + 1 >= len) { o[k++] = s[i]; continue; } i++;
        switch (s[i]) { case 'n': o[k++] = '\n'; break; case 't': o[k++] = '\t'; break; case 'r': o[k++] = '\r'; break; case 'b': o[k++] = '\b'; break; case 'f': o[k++] = '\f'; break;
            case 'u': { if (i + 4 >= len) { o[k++] = 'u'; break; } int v = hex4(s + i + 1); if (v < 0) { o[k++] = 'u'; break; } i += 4;
                if (v >= 0xD800 && v < 0xDC00 && i + 6 < len && s[i + 1] == '\\' && s[i + 2] == 'u') { int lo = hex4(s + i + 3); if (lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); i += 6; } }
                k += lp_utf8_put((uint32_t)v, o + k); break; }
            case 'U': { if (i + 8 >= len) { o[k++] = 'U'; break; } int hi = hex4(s + i + 1), lo = hex4(s + i + 5); if (hi < 0 || lo < 0) { o[k++] = 'U'; break; } i += 8;
                uint32_t v = ((uint32_t)hi << 16) | (uint32_t)lo; if (v < 0x110000) k += lp_utf8_put(v, o + k); break; }
            default: o[k++] = s[i]; } }
    return k;
}
static void jws(const uint8_t **p, const uint8_t *e){ while (*p < e && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')) (*p)++; }
static int jstr(STree *t, const uint8_t **p, const uint8_t *e, const uint8_t **v, uint32_t *vn){
    const uint8_t *a = *p + 1, *q = a; int esc = 0; while (q < e && *q != '"') { if (*q == '\\') { esc = 1; q++; } q++; } if (q >= e) return 0;
    *p = q + 1; if (!esc) { *v = a; *vn = (uint32_t)(q - a); return 1; }
    uint8_t *o = own(t, (size_t)(q - a) + 8); *vn = (uint32_t)esc_resolve(a, (size_t)(q - a), o); *v = o; return 1;
}
static void mini_json(STree *t, int32_t in, const uint8_t **p, const uint8_t *e, int depth){
    jws(p, e); if (*p >= e || depth > 32) return; uint8_t tier = t->n[in].tier; const uint8_t *nm = t->n[in].name; uint32_t nl = t->n[in].nlen;
    if (**p == '{') { (*p)++; jws(p, e);
        while (*p < e && **p == '"') { const uint8_t *k; uint32_t kn; if (!jstr(t, p, e, &k, &kn)) return; jws(p, e); if (*p < e && **p == ':') (*p)++; jws(p, e);
            if (*p < e && (**p == '{' || **p == '[')) { int32_t g = node(t, S_GROUP, tier, in, (uint64_t)0); t->n[g].name = k; t->n[g].nlen = kn; if (**p == '[') t->n[g].join = S_PIECES; mini_json(t, g, p, e, depth + 1); }
            else if (*p < e && **p == '"') { const uint8_t *v; uint32_t vn; if (!jstr(t, p, e, &v, &vn)) return; int32_t x = node(t, S_VALUE, tier, in, 0); t->n[x].name = k; t->n[x].nlen = kn; t->n[x].val = v; t->n[x].vlen = vn; }
            else { const uint8_t *a = *p; while (*p < e && **p != ',' && **p != '}' && **p != ']' && **p != ' ') (*p)++; if (!(*p - a == 4 && !memcmp(a, "null", 4))) { int32_t x = node(t, S_VALUE, tier, in, 0); t->n[x].name = k; t->n[x].nlen = kn; t->n[x].val = a; t->n[x].vlen = (uint32_t)(*p - a); } }
            jws(p, e); if (*p < e && **p == ',') { (*p)++; jws(p, e); } else break; }
        if (*p < e && **p == '}') (*p)++; return; }
    if (**p == '[') { (*p)++; jws(p, e);
        while (*p < e && **p != ']') {
            if (**p == '{' || **p == '[') { int32_t g = node(t, S_GROUP, tier, in, 0); t->n[g].name = nm; t->n[g].nlen = nl; if (**p == '[') t->n[g].join = S_PIECES; mini_json(t, g, p, e, depth + 1); }
            else if (**p == '"') { const uint8_t *v; uint32_t vn; if (!jstr(t, p, e, &v, &vn)) return; int32_t x = node(t, S_TEXT, tier, in, 0); t->n[x].name = nm; t->n[x].nlen = nl; t->n[x].val = v; t->n[x].vlen = vn; }
            else { const uint8_t *a = *p; while (*p < e && **p != ',' && **p != ']' && **p != ' ') (*p)++; int32_t x = node(t, S_TEXT, tier, in, 0); t->n[x].name = nm; t->n[x].nlen = nl; t->n[x].val = a; t->n[x].vlen = (uint32_t)(*p - a); }
            jws(p, e); if (*p < e && **p == ',') { (*p)++; jws(p, e); } else break; }
        if (*p < e && **p == ']') (*p)++; }
}
/* A named part that is itself parts (part NAME by SEP [is IS]). */
static void parts_of(STree *t, int32_t x, const Layout *l){
    SNode *v = &t->n[x];
    for (int z = 0; z < l->npart; z++) { const SPart *sp = &l->part[z]; size_t pl = strlen(sp->path); if (pl && sp->path[pl - 1] == '*' ? !(v->nlen >= pl - 1 && !memcmp(v->name, sp->path, pl - 1)) : !s_named(v, sp->path, pl)) continue;     /* PATH*: every part whose name begins so */
        if (s_empty(l, v->val, v->vlen)) return;
        const uint8_t *p = v->val, *e = p + v->vlen; uint64_t at = v->at; t->n[x].kind = S_GROUP; t->n[x].join = S_PIECES;
        if (sp->seplen == 6 && !memcmp(sp->sep, "object", 6)) {             /* a JSON object written in the part: its members, each named by its key */
            const uint8_t *q = p; mini_json(t, x, &q, e, 0); return; }
        if (sp->seplen == 4 && !memcmp(sp->sep, "json", 4)) {               /* a list written as JSON, ["a", "b"]: each text, its escapes resolved */
            while (p < e) { const uint8_t *q = memchr(p, '"', (size_t)(e - p)); if (!q) break; q++; const uint8_t *qe = q; int esc = 0;
                while (qe < e && *qe != '"') { if (*qe == '\\') { esc = 1; qe++; } qe++; } if (qe >= e) break;
                const uint8_t *vp = q; size_t vn = (size_t)(qe - q); if (esc) { uint8_t *o = own(t, vn); size_t j = 0; for (size_t i = 0; i < vn; i++) { if (vp[i] == '\\' && i + 1 < vn) { i++; o[j++] = vp[i] == 'n' ? '\n' : vp[i] == 't' ? '\t' : vp[i]; } else o[j++] = vp[i]; } vp = o; vn = j; }
                if (vn) { int32_t c = node(t, S_TEXT, t->n[x].tier, x, at + (uint64_t)(q - t->n[x].val)); t->n[c].val = vp; t->n[c].vlen = (uint32_t)vn; t->n[c].name = t->n[x].name; t->n[c].nlen = t->n[x].nlen; }
                p = qe + 1; }
            return; }
        int made = 0;
        while (p < e) { const uint8_t *q = sp->pieces && made == sp->pieces - 1 ? NULL : memmem(p, (size_t)(e - p), sp->sep, (size_t)sp->seplen); if (!q) q = e;     /* the last of as many pieces as there may be: the rest */
            if (q > p) { const uint8_t *is = sp->islen ? memmem(p, (size_t)(q - p), sp->is, (size_t)sp->islen) : NULL;
                int32_t c = node(t, is ? S_VALUE : S_TEXT, t->n[x].tier, x, at + (uint64_t)(p - t->n[x].val)); made++;
                if (is) { t->n[c].name = p; t->n[c].nlen = (uint32_t)(is - p); t->n[c].val = is + sp->islen; t->n[c].vlen = (uint32_t)(q - is - sp->islen); }
                else { t->n[c].val = p; t->n[c].vlen = (uint32_t)(q - p); t->n[c].name = t->n[x].name; t->n[c].nlen = t->n[x].nlen; }      /* a piece of a part is read as the part is */
                if (sp->space && memchr(t->n[c].val, sp->space, t->n[c].vlen)) { uint8_t *o = own(t, t->n[c].vlen); for (uint32_t i = 0; i < t->n[c].vlen; i++) o[i] = t->n[c].val[i] == (uint8_t)sp->space ? ' ' : t->n[c].val[i]; t->n[c].val = o; } }
            if (q >= e) break; p = q + sp->seplen; }
        return; }
}
/* One part of tier k, between lo and hi, under a node: the parts of the tier below it, or its text. */
static void tier_read(STree *t, const Layout *l, int k, int32_t in, const uint8_t *s, size_t lo, size_t hi){
    if (k + 1 >= l->ntier) return;
    const STier *me = &l->tier[k], *t1 = &l->tier[k + 1]; int pos = 0, innermost = k + 2 >= l->ntier;
    for (size_t i = lo; i < hi || (i == hi && pos && innermost && hi > lo && !memcmp(s + hi - t1->seplen, t1->sep, (size_t)t1->seplen)); ) {
        size_t next, e = i < hi ? part_end(l, k + 1, s, i, hi, &next) : hi; if (i >= hi) next = hi + 1;
        size_t len = e - i;
        if (!innermost) {
            if (t1->commentlen && len >= (size_t)t1->commentlen && !memcmp(s + i, t1->comment, (size_t)t1->commentlen)) { i = next; continue; }
            if (t1->notelen && len >= (size_t)t1->notelen && !memcmp(s + i, t1->note, (size_t)t1->notelen)) {              /* a note: KEY IS VALUE, or the text it is */
                size_t ln = len - (size_t)t1->notelen; const uint8_t *lp = trimmed(s + i + t1->notelen, &ln), *is = t1->islen ? memmem(lp, ln, t1->note_is, (size_t)t1->islen) : NULL;
                int32_t x = node(t, S_NOTE, (uint8_t)(k + 1), in, i);
                if (is) { size_t kn = (size_t)(is - lp), vn = ln - kn - (size_t)t1->islen; const uint8_t *kp = trimmed(lp, &kn), *vp = trimmed(is + t1->islen, &vn); t->n[x].name = kp; t->n[x].nlen = (uint32_t)kn; t->n[x].val = vp; t->n[x].vlen = (uint32_t)vn; }
                else { t->n[x].val = lp; t->n[x].vlen = (uint32_t)ln; }
                i = next; continue; }
            if (!len || (len == 1 && s[i] == '\r')) { i = next; continue; }
            size_t stop = e; if (t1->remark) { const uint8_t *r = memchr(s + i, t1->remark, len); if (r) stop = (size_t)(r - s); }
            int32_t g = node(t, S_GROUP, (uint8_t)(k + 1), in, i); t->n[g].name = (const uint8_t *)t1->name; t->n[g].nlen = (uint32_t)strlen(t1->name);
            tier_read(t, l, k + 1, g, s, i, stop);
        } else {
            size_t stop = e; if (me->remark && i < hi) { const uint8_t *r = memchr(s + i, me->remark, len); if (r) stop = (size_t)(r - s); }
            if (t1->continued && stop > i && (s[i] == ' ' || s[i] == '\t') && t->n[in].last >= 0 && t->n[t->n[in].last].kind != S_GROUP) {    /* it goes on with the part before, joined by one space */
                SNode *v = &t->n[t->n[in].last]; size_t cn = stop - i; const uint8_t *cp = trimmed(s + i, &cn); uint8_t *o = own(t, v->vlen + cn + 1);
                memcpy(o, v->val, v->vlen); size_t j = v->vlen; if (j && cn) o[j++] = ' '; memcpy(o + j, cp, cn); v = &t->n[t->n[in].last]; v->val = o; v->vlen = (uint32_t)(j + cn);
                i = next; continue; }
            const uint8_t *is = t1->kvlen && stop > i ? memmem(s + i, stop - i, t1->kv, (size_t)t1->kvlen) : NULL;
            int32_t x = node(t, pos < me->nnames || is ? S_VALUE : S_TEXT, (uint8_t)(k + 1), in, i);
            if (is) { size_t kn = (size_t)(is - s - i), vn = stop - (size_t)(is - s) - (size_t)t1->kvlen; const uint8_t *kp = trimmed(s + i, &kn), *vp = trimmed(is + t1->kvlen, &vn);   /* KEY IS VALUE: named by its key */
                t->n[x].name = kp; t->n[x].nlen = (uint32_t)kn; t->n[x].val = vp; t->n[x].vlen = (uint32_t)vn; (void)(kp + t1->kvlen); if (l->npart) parts_of(t, x, l); pos++; i = next; continue; }
            if (pos < me->nnames) { t->n[x].name = (const uint8_t *)me->names[pos]; t->n[x].nlen = (uint32_t)strlen(me->names[pos]); }
            text_put(t, x, l, k + 1, s + i, stop - i); if (l->npart && t->n[x].nlen) parts_of(t, x, l);
            if (stop < e) { i = hi; pos++; break; }
        }
        pos++; i = next;
    }
}
size_t s_head(Layout *l, const uint8_t *s, size_t n){
    if (!l->ntier) return 0; STier *t0 = &l->tier[0]; size_t i = 0;
    for (int k = 0; k < t0->skip && i < n; k++) { size_t next; part_end(l, 0, s, i, n, &next); i = next; }
    if (!t0->header || l->ntier < 2) return i;
    size_t next, e = part_end(l, 0, s, i, n, &next); STree t = { 0 }; int32_t g = node(&t, S_GROUP, 0, -1, i); int had = t0->nnames; t0->nnames = 0;
    tier_read(&t, l, 0, g, s, i, e); (void)had;
    for (int32_t c = t.n[g].first; c >= 0 && t0->nnames < S_NAMES; c = t.n[c].next) snprintf(t0->names[t0->nnames++], 64, "%.*s", (int)(t.n[c].vlen < 63 ? t.n[c].vlen : 63), t.n[c].val);
    tree_free(&t); return next;
}
size_t s_boundary(const Layout *l, const uint8_t *s, size_t n, size_t at){
    const STier *t0 = &l->tier[0]; if (at >= n) return n;
    if (l->ntier > 1 && l->tier[0].quoted) return n;                      /* a quoted part may hold the separator: where a part begins is known only from the file's start */
    for (size_t p = at; p + (size_t)t0->seplen <= n; p++) if (!memcmp(s + p, t0->sep, (size_t)t0->seplen)) {
        if (t0->escaped) { size_t b = 0; while (p > b && s[p - 1 - b] == (uint8_t)t0->escaped) b++; if (b & 1) continue; }
        return p + (size_t)t0->seplen; }
    return n;
}
uint64_t s_decompose(const Layout *l, const uint8_t *s, size_t n, s_unit_fn fn, void *sink){
    if (!l->ntier) { fprintf(stderr, "a layout with no tier parts nothing\n"); exit(2); }
    const STier *t0 = &l->tier[0]; STree t = { 0 }; uint64_t ord = 0;
    for (size_t i = 0; i < n; ) {
        size_t next, e = part_end(l, 0, s, i, n, &next), len = e - i;
        while (len && (s[i] == '\n' || s[i] == '\r') && l->tier[0].seplen > 1) { i++; len--; }          /* line ends left over between records */
        if (!len || (len == 1 && s[i] == '\r')) { i = next; continue; }
        if (t0->commentlen && len >= (size_t)t0->commentlen && !memcmp(s + i, t0->comment, (size_t)t0->commentlen)) { i = next; continue; }
        size_t stop = e; if (t0->remark && l->ntier > 1) { const uint8_t *r = memchr(s + i, t0->remark, len); if (r) stop = (size_t)(r - s); }
        tree_reset(&t); int32_t g = node(&t, S_GROUP, 0, -1, i); t.n[g].name = (const uint8_t *)t0->name; t.n[g].nlen = (uint32_t)strlen(t0->name);
        if (l->ntier == 1) { t.n[g].kind = S_TEXT; text_put(&t, g, l, 0, s + i, len); }
        else tier_read(&t, l, 0, g, s, i, stop);
        if (t.n[g].kind != S_GROUP || t.n[g].nkids) fn(sink, &t, g, ord++);
        i = next;
    }
    tree_free(&t); return ord;
}

/* ---- a grammar's tree as the file's tree, by what the recipe says each kind of node is */
#include <tree_sitter/api.h>
size_t xml_unescape(const uint8_t *s, size_t n, uint8_t *o);
typedef struct { const Layout *l; STree *t; const uint8_t *src; int level; const GRule **rs; uint32_t nrs; } GW;      /* level: how many members of objects the node being read is inside (levels); rs: the rule of each of the grammar's symbols */
static const GRule *rule_of(const GMap *g, const char *type){ for (int i = 0; i < g->n; i++) if (!strcmp(g->rule[i].type, type)) return &g->rule[i]; return NULL; }
/* The node a path names under a node: A, or A/B (the B in its A); a field of the node by its name where it has one. */
static int at_path(TSNode nd, const char *path, TSNode *out){
    TSNode cur = nd;
    for (const char *p = path; *p; ) { const char *e = strchr(p, '/'); size_t l = e ? (size_t)(e - p) : strlen(p);     /* a step at a time, as written: no copy of the path */
        if (l) { TSNode f = ts_node_child_by_field_name(cur, p, (uint32_t)l); int found = !ts_node_is_null(f);
            if (!found) { uint32_t nc = ts_node_named_child_count(cur); for (uint32_t i = 0; i < nc && !found; i++) { TSNode c = ts_node_named_child(cur, i); const char *ty = ts_node_type(c); if (strlen(ty) == l && !memcmp(ty, p, l)) { f = c; found = 1; } } }
            if (!found) return 0; cur = f; }
        p = e ? e + 1 : p + l; }
    *out = cur; return 1;
}
/* The rule of a node, by its symbol: each symbol's rule found by name once for the grammar being read. */
static const GRule *rule_at(GW *w, TSNode nd){
    if (!w->rs) { const TSLanguage *lg = ts_node_language(nd); w->nrs = ts_language_symbol_count(lg); w->rs = malloc(sizeof(GRule *) * (w->nrs + 1));
        for (uint32_t i = 0; i < w->nrs; i++) { const char *nm = ts_language_symbol_name(lg, (TSSymbol)i); w->rs[i] = nm ? rule_of(&w->l->g, nm) : NULL; } }
    TSSymbol sy = ts_node_symbol(nd); return sy < w->nrs ? w->rs[sy] : rule_of(&w->l->g, ts_node_type(nd));
}
/* A node's text as it means: its quotes off, and what the grammar's texts write for what they cannot write plainly
 * resolved (an XML reference, a JSON escape). */
static void g_text(GW *w, TSNode nd, int raw, const uint8_t **p, uint32_t *n){
    const uint8_t *s = w->src + ts_node_start_byte(nd); size_t len = ts_node_end_byte(nd) - ts_node_start_byte(nd);
    if (!raw && w->l->g.resolve == G_TURTLE) {                                 /* Turtle: an IRI without its angle brackets, a language tag without its @, a text without its quotes (one or three) */
        if (len >= 2 && s[0] == '<' && s[len - 1] == '>') { s++; len -= 2; } else if (len >= 1 && s[0] == '@') { s++; len--; }
        else if (len >= 6 && (s[0] == '"' || s[0] == '\'') && s[1] == s[0] && s[2] == s[0] && s[len - 1] == s[0] && s[len - 2] == s[0] && s[len - 3] == s[0]) { s += 3; len -= 6; } }
    if (!raw && len >= 2 && (s[0] == '"' || s[0] == '\'') && s[len - 1] == s[0]) { s++; len -= 2; }
    if (!raw && w->l->g.resolve == G_XML && memchr(s, '&', len)) { uint8_t *o = own(w->t, len + 8); len = xml_unescape(s, len, o); s = o; }
    else if (!raw && (w->l->g.resolve == G_JSON || w->l->g.resolve == G_TURTLE) && memchr(s, '\\', len)) { uint8_t *o = own(w->t, len + 8); len = esc_resolve(s, len, o); s = o; }
    *p = s; *n = (uint32_t)len;
}
static void g_node(GW *w, TSNode nd, int32_t in, const uint8_t *name, uint32_t nlen, int depth){
    const GRule *r = rule_at(w, nd); uint8_t tier = (uint8_t)(depth < 255 ? depth : 255);
    if (r && r->what == G_SKIP) return;
    if (r && r->what == G_MEMBER) {                                           /* it names what its other part is */
        TSNode kn, vn; const uint8_t *np; uint32_t nn; if (!at_path(nd, r->name[0], &kn) || !at_path(nd, r->text, &vn)) return;
        g_text(w, kn, 0, &np, &nn);
        if (w->level < w->l->nlevels) {                                       /* a member whose key is what the file says: a part of its level's name, holding its key and its value */
            const char *lv = w->l->levels[w->level]; int32_t g = node(w->t, S_GROUP, (uint8_t)depth, in, ts_node_start_byte(nd)); w->t->n[g].name = (const uint8_t *)lv; w->t->n[g].nlen = (uint32_t)strlen(lv);
            int32_t x = node(w->t, S_VALUE, (uint8_t)(depth + 1), g, ts_node_start_byte(kn)); w->t->n[x].name = (const uint8_t *)"key"; w->t->n[x].nlen = 3; w->t->n[x].val = np; w->t->n[x].vlen = nn;
            w->level++; g_node(w, vn, g, (const uint8_t *)"value", 5, depth + 1); w->level--; return; }
        g_node(w, vn, in, np, nn, depth); return; }
    if (r && r->what == G_VALUE) { TSNode kn, vn; if (!at_path(nd, r->name[0], &kn) || !at_path(nd, r->text, &vn)) return;
        int32_t x = node(w->t, S_VALUE, tier, in, ts_node_start_byte(nd)); const uint8_t *p; uint32_t n;
        g_text(w, kn, 1, &p, &n); w->t->n[x].name = p; w->t->n[x].nlen = n; g_text(w, vn, r->raw, &p, &n); w->t->n[x].val = p; w->t->n[x].vlen = n;
        if (w->l->npart) parts_of(w->t, x, w->l); return; }
    if (r && r->what == G_TEXT) { const uint8_t *p; uint32_t n; g_text(w, nd, r->raw, &p, &n); if (!n) return;
        if (!name && r->kind) { name = (const uint8_t *)r->type; nlen = (uint32_t)strlen(r->type); }      /* named by what kind of node it is */
        if (name) { int32_t x = node(w->t, S_VALUE, tier, in, ts_node_start_byte(nd)); w->t->n[x].name = name; w->t->n[x].nlen = nlen; w->t->n[x].val = p; w->t->n[x].vlen = n; if (w->l->npart) parts_of(w->t, x, w->l); return; }
        int32_t last = in >= 0 ? w->t->n[in].last : -1;
        if (r->join && last >= 0 && w->t->n[last].kind == S_TEXT && w->t->n[last].join) {          /* texts side by side are one text */
            SNode *v = &w->t->n[last]; uint8_t *o = own(w->t, v->vlen + n); memcpy(o, v->val, v->vlen); memcpy(o + v->vlen, p, n); v = &w->t->n[last]; v->val = o; v->vlen += n; return; }
        int32_t x = node(w->t, S_TEXT, tier, in, ts_node_start_byte(nd)); w->t->n[x].val = p; w->t->n[x].vlen = n; w->t->n[x].join = (uint8_t)r->join;
        if (in >= 0) { w->t->n[x].name = w->t->n[in].name; w->t->n[x].nlen = w->t->n[in].nlen; }      /* a text is said under the name of what holds it */
        return; }
    int32_t into = in;
    if (r && r->what == G_GROUP) {
        into = node(w->t, S_GROUP, tier, in, ts_node_start_byte(nd)); w->t->n[into].end = ts_node_end_byte(nd);
        if (!name && in >= 0 && w->t->n[in].join == S_PIECES && w->t->n[in].nlen) { name = w->t->n[in].name; nlen = w->t->n[in].nlen; }    /* a group in a list is named by the list */
        if (r->list) w->t->n[into].join = S_PIECES;
        if (!name && r->kind) { name = (const uint8_t *)r->type; nlen = (uint32_t)strlen(r->type); }      /* named by what kind of node it is */
        if (name) { w->t->n[into].name = name; w->t->n[into].nlen = nlen; }
        else for (int i = 0; i < r->nname; i++) { TSNode kn; if (at_path(nd, r->name[i], &kn)) { const uint8_t *p; uint32_t n; g_text(w, kn, 1, &p, &n); w->t->n[into].name = p; w->t->n[into].nlen = n; break; } }
        depth++;
    }
    TSTreeCursor cur = ts_tree_cursor_new(nd);                               /* children by cursor: asking for the i-th costs i */
    if (ts_tree_cursor_goto_first_child(&cur)) do { TSNode c = ts_tree_cursor_current_node(&cur); if (!ts_node_is_named(c)) continue;
        const char *fld = ts_tree_cursor_current_field_name(&cur);                /* the grammar's own name for the child's place (value, datatype), for what has no name of its own */
        { const GRule *cr = rule_at(w, c); if (cr && (cr->what == G_GROUP || cr->what == G_SKIP) && (cr->nname || cr->kind)) fld = NULL; }
        g_node(w, c, into, fld ? (const uint8_t *)fld : NULL, fld ? (uint32_t)strlen(fld) : 0, depth); } while (ts_tree_cursor_goto_next_sibling(&cur));
    ts_tree_cursor_delete(&cur);
    if (r && r->what == G_GROUP && w->t->n[into].first < 0 && !ts_node_named_child_count(nd)) {     /* a group that holds nothing named holds its own text (Turtle's a) */
        const uint8_t *p; uint32_t n; g_text(w, nd, 0, &p, &n);
        if (n) { int32_t x = node(w->t, S_TEXT, (uint8_t)depth, into, ts_node_start_byte(nd)); w->t->n[x].val = p; w->t->n[x].vlen = n; w->t->n[x].name = w->t->n[into].name; w->t->n[x].nlen = w->t->n[into].nlen; } }
    if (r && r->what == G_GROUP && w->l->g.trim)                              /* its texts without the layout around them */
        for (int32_t c = w->t->n[into].first; c >= 0; c = w->t->n[c].next) { SNode *v = &w->t->n[c]; if (v->kind != S_TEXT) continue; size_t n = v->vlen; v->val = trimmed(v->val, &n);
            while (n && (v->val[0] == '\n' || v->val[0] == '\r')) { v->val++; n--; } while (n && (v->val[n - 1] == '\n' || v->val[n - 1] == '\r' || v->val[n - 1] == ' ' || v->val[n - 1] == '\t')) n--;
            const uint8_t *p = v->val; while (n && (p[0] == ' ' || p[0] == '\t' || p[0] == '\n' || p[0] == '\r')) { p++; n--; } v->val = p; v->vlen = (uint32_t)n; }
    if (r && r->what == G_GROUP) {                                            /* white space between the nodes it holds is how the file is written down, not a text of its */
        int holds = 0; for (int32_t c = w->t->n[into].first; c >= 0 && !holds; c = w->t->n[c].next) holds = w->t->n[c].kind == S_GROUP;
        if (holds) { int32_t prev = -1; for (int32_t c = w->t->n[into].first; c >= 0; ) { int32_t next = w->t->n[c].next; SNode *v = &w->t->n[c]; int blank = v->kind == S_TEXT; for (uint32_t i = 0; blank && i < v->vlen; i++) blank = v->val[i] == ' ' || v->val[i] == '\n' || v->val[i] == '\t' || v->val[i] == '\r';
                if (blank) { if (prev >= 0) w->t->n[prev].next = next; else w->t->n[into].first = next; if (w->t->n[into].last == c) w->t->n[into].last = prev; w->t->n[into].nkids--; } else prev = c;
                c = next; } }
    }
}
void s_grammar(const Layout *l, const void *ts_root, const uint8_t *src, size_t n, s_unit_fn fn, void *sink){
    (void)n; STree t = { 0 }; int32_t root = node(&t, S_GROUP, 0, -1, 0); GW w = { l, &t, src, 0 };
    g_node(&w, *(const TSNode *)ts_root, root, NULL, 0, 1);
    if (l->ntier) for (int32_t c = t.n[root].first; c >= 0; c = t.n[c].next)  /* a part of a tier that is one tree: the tree's top is the part, named as the tier */
        if (t.n[c].kind == S_GROUP && !t.n[c].nlen) { t.n[c].name = (const uint8_t *)l->tier[0].name; t.n[c].nlen = (uint32_t)strlen(l->tier[0].name); }
    free(w.rs); fn(sink, &t, root, 0); tree_free(&t);
}

/* ---- laplace structure LAYOUT FILE [-n NODES]: a file's tree as a layout parts it, for writing recipes. LAYOUT is a
 * file of layout lines (a recipe's own lines; whatever is not layout is passed over). */
typedef struct { long left; uint64_t shown; } Show;
static void show_unit(void *sink, const STree *t, int32_t root, uint64_t ord){ Show *s = sink; if (s->left <= 0) return; printf("-- %llu\n", (unsigned long long)ord); s_print(t, root, 0, &s->left); s->shown++; }
int cmd_structure(int argc, char **argv){
    long nodes = 60; const char *lay = NULL, *file = NULL;
    for (int a = 1; a < argc; a++) { if (!strcmp(argv[a], "-n") && a + 1 < argc) nodes = atol(argv[++a]); else if (!lay) lay = argv[a]; else file = argv[a]; }
    if (!lay || !file) { fprintf(stderr, "usage: laplace structure LAYOUT FILE [-n nodes]\n"); return 2; }
    Layout *l = calloc(1, sizeof *l); FILE *f = fopen(lay, "r"); if (!f) { perror(lay); return 1; } char line[4096];
    while (fgets(line, sizeof line, f)) { char *h = strchr(line, '#'); if (h && h == line) continue; char *tok = strtok(line, " \t\r\n"); if (!tok) continue; if (layout_says(l, lay, tok) < 0) return 2; }
    fclose(f);
    f = fopen(file, "rb"); if (!f) { perror(file); return 1; } size_t cap = 1 << 20, n = 0; uint8_t *src = malloc(cap + 1); n = fread(src, 1, cap, f); fclose(f);   /* its first megabyte: enough to see its tree */
    { size_t e = n; while (e && src[e - 1] != '\n') e--; if (e && n == cap) n = e; }
    size_t at = s_head(l, src, n); Show s = { nodes, 0 }; uint64_t units = s_decompose(l, src + at, n - at, show_unit, &s);
    printf("%llu parts of the outermost tier in the first %zu bytes\n", (unsigned long long)units, n); free(src); free(l); return 0;
}

/* ---- a long file of records, parsed on every core and read as one tree */
typedef struct { size_t doc, file; } Seg;                                    /* a run of bytes copied into a part: where it begins there and in the file */
typedef struct { uint8_t *doc; size_t len; Seg *seg; size_t nseg; STree t; uint64_t bad; } SPartTree;
typedef struct { const SPartTree *p; int32_t i; uint64_t at; } Item;         /* a record's node in its part's tree, and where it begins in the file */
typedef struct { STree m; const SPartTree *around; Item *it; size_t nit, next; } Graft;
static uint64_t file_at(const SPartTree *p, uint64_t a){
    size_t lo = 0, hi = p->nseg; while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (p->seg[mid].doc <= a) lo = mid; else hi = mid; }
    return p->seg[lo].file + (a - p->seg[lo].doc);
}
static uint64_t file_end(const SPartTree *p, uint64_t e){ return e ? file_at(p, e - 1) + 1 : 0; }
static void part_tree(const Layout *l, const TSLanguage *lang, SPartTree *p){
    TSParser *ps = ts_parser_new(); ts_parser_set_language(ps, lang);
    TSTree *tt = ts_parser_parse_string(ps, NULL, (const char *)p->doc, (uint32_t)p->len); TSNode root = ts_tree_root_node(tt);
    if (ts_node_has_error(root)) p->bad++;                                   /* what parses is read */
    int32_t r = node(&p->t, S_GROUP, 0, -1, 0); p->t.n[r].end = p->len; GW w = { l, &p->t, p->doc, 0 };
    g_node(&w, root, r, NULL, 0, 1); free(w.rs); ts_tree_delete(tt); ts_parser_delete(ps);
}
static int32_t copy_node(STree *m, const SNode *x, int32_t parent, uint8_t tier, uint64_t at, uint64_t end){
    int32_t i = node(m, x->kind, tier, parent, at); SNode *y = &m->n[i];
    y->name = x->name; y->nlen = x->nlen; y->val = x->val; y->vlen = x->vlen; y->join = x->join; y->end = end; return i;
}
/* A record, where it stands: its tiers as deep as the group it is inside makes them. */
static void copy_rec(STree *m, const SPartTree *p, int32_t i, int32_t parent, int delta){
    const SNode *x = &p->t.n[i];
    int32_t y = copy_node(m, x, parent, (uint8_t)(x->tier + delta), file_at(p, x->at), x->kind == S_GROUP ? file_end(p, x->end) : 0);
    for (int32_t c = p->t.n[i].first; c >= 0; c = p->t.n[c].next) copy_rec(m, p, c, y, delta);
}
static void graft(Graft *g, int32_t parent){
    const Item *r = &g->it[g->next++];
    copy_rec(&g->m, r->p, r->i, parent, (int)g->m.n[parent].tier + 1 - (int)r->p->t.n[r->i].tier);
}
/* What stands around the records, in the file's order; each record goes into the innermost group around it, before the
 * first of that group's nodes that comes after it. */
static void copy_around(Graft *g, int32_t i, int32_t parent){
    const STree *t = &g->around->t; const SNode *x = &t->n[i]; int32_t y; uint64_t end;
    if (i == 0) { y = node(&g->m, S_GROUP, 0, -1, 0); end = UINT64_MAX; g->m.n[y].end = end; }
    else { end = x->kind == S_GROUP ? file_end(g->around, x->end) : 0; y = copy_node(&g->m, x, parent, x->tier, file_at(g->around, x->at), end); }
    if (x->kind != S_GROUP) return;
    for (int32_t c = x->first; c >= 0; c = t->n[c].next) {
        uint64_t ca = file_at(g->around, t->n[c].at);
        while (g->next < g->nit && g->it[g->next].at < ca) graft(g, y);
        copy_around(g, c, y);
    }
    while (g->next < g->nit && g->it[g->next].at < end) graft(g, y);
}
uint64_t s_grammar_split(const Layout *l, const void *lang, const uint8_t *src, size_t n, const size_t *ra, const size_t *rb, size_t nrs, s_unit_fn fn, void *sink){
    /* the records in parts of about two megabytes each, in the file's order; what stands around them is a part of its own */
    size_t *first = malloc(sizeof(size_t) * (nrs + 2)), np = 0, acc = 0;
    for (size_t i = 0; i < nrs; i++) { if (!acc) first[np++] = i; acc += rb[i] - ra[i]; if (acc >= (2u << 20)) acc = 0; }
    first[np] = nrs;
    SPartTree *pt = calloc(np + 1, sizeof(SPartTree));
    for (size_t i = 0; i < np; i++) { SPartTree *p = &pt[i]; size_t need = 16, k = first[i + 1] - first[i];
        for (size_t j = first[i]; j < first[i + 1]; j++) need += rb[j] - ra[j];
        p->doc = malloc(need); p->seg = malloc(sizeof(Seg) * (k + 1)); memcpy(p->doc, "<_>\n", 4); p->len = 4; p->seg[p->nseg++] = (Seg){ 0, 0 };
        for (size_t j = first[i]; j < first[i + 1]; j++) { p->seg[p->nseg++] = (Seg){ p->len, ra[j] }; memcpy(p->doc + p->len, src + ra[j], rb[j] - ra[j]); p->len += rb[j] - ra[j]; }
        memcpy(p->doc + p->len, "\n</_>", 5); p->len += 5; }
    { SPartTree *p = &pt[np]; p->doc = malloc(n + 1); p->seg = malloc(sizeof(Seg) * (nrs + 2)); size_t at = 0;
      for (size_t j = 0; j <= nrs; j++) { size_t b = j < nrs ? ra[j] : n; if (b > at) { p->seg[p->nseg++] = (Seg){ p->len, at }; memcpy(p->doc + p->len, src + at, b - at); p->len += b - at; } if (j < nrs) at = rb[j]; }
      if (!p->nseg) p->seg[p->nseg++] = (Seg){ 0, 0 }; }
    struct timespec T0_, T1_, T2_, T3_; clock_gettime(CLOCK_MONOTONIC, &T0_);
    #pragma omp taskloop grainsize(1)
    for (size_t i = 0; i <= np; i++) part_tree(l, (const TSLanguage *)lang, &pt[i]);
    clock_gettime(CLOCK_MONOTONIC, &T1_);
    /* every record: the nodes the part's own element (_) holds, in order */
    size_t ni = 0, ci = 1024; Item *it = malloc(sizeof(Item) * ci);
    for (size_t i = 0; i < np; i++) { const STree *t = &pt[i].t; int32_t u = -1;
        for (uint32_t g = 1; g < t->count && u < 0; g++) if (t->n[g].kind == S_GROUP) u = (int32_t)g;
        if (u < 0) continue;
        for (int32_t c = t->n[u].first; c >= 0; c = t->n[c].next) { if (ni == ci) { ci *= 2; it = realloc(it, sizeof(Item) * ci); }
            it[ni++] = (Item){ &pt[i], c, file_at(&pt[i], t->n[c].at) }; } }
    Graft g = { .around = &pt[np], .it = it, .nit = ni };
    copy_around(&g, 0, -1);
    while (g.next < g.nit) graft(&g, 0);
    /* white space between the nodes a group holds is how the file is laid out: where a record now stands beside it too */
    for (uint32_t x = 0; x < g.m.count; x++) { SNode *G = &g.m.n[x]; if (G->kind != S_GROUP) continue;
        int holds = 0; for (int32_t c = G->first; c >= 0 && !holds; c = g.m.n[c].next) holds = g.m.n[c].kind == S_GROUP;
        if (!holds) continue;
        int32_t prev = -1; for (int32_t c = G->first; c >= 0; ) { int32_t next = g.m.n[c].next; SNode *v = &g.m.n[c]; int blank = v->kind == S_TEXT;
            for (uint32_t i = 0; blank && i < v->vlen; i++) blank = v->val[i] == ' ' || v->val[i] == '\n' || v->val[i] == '\t' || v->val[i] == '\r';
            if (blank) { if (prev >= 0) g.m.n[prev].next = next; else G->first = next; if (G->last == c) G->last = prev; G->nkids--; } else prev = c;
            c = next; } }
    if (l->ntier) for (int32_t c = g.m.n[0].first; c >= 0; c = g.m.n[c].next)
        if (g.m.n[c].kind == S_GROUP && !g.m.n[c].nlen) { g.m.n[c].name = (const uint8_t *)l->tier[0].name; g.m.n[c].nlen = (uint32_t)strlen(l->tier[0].name); }
    clock_gettime(CLOCK_MONOTONIC, &T2_);
    fn(sink, &g.m, 0, 0);
    clock_gettime(CLOCK_MONOTONIC, &T3_);
    if (getenv("LAPLACE_TIMING")) fprintf(stderr, "\n  split: %zu parts parsed %.2f s, grafted %.2f s, read %.2f s\n", np + 1, (T1_.tv_sec - T0_.tv_sec) + (T1_.tv_nsec - T0_.tv_nsec) * 1e-9,
        (T2_.tv_sec - T1_.tv_sec) + (T2_.tv_nsec - T1_.tv_nsec) * 1e-9, (T3_.tv_sec - T2_.tv_sec) + (T3_.tv_nsec - T2_.tv_nsec) * 1e-9);
    uint64_t bad = 0;
    for (size_t i = 0; i <= np; i++) { bad += pt[i].bad; tree_free(&pt[i].t); free(pt[i].doc); free(pt[i].seg); }
    tree_free(&g.m); free(pt); free(it); free(first);
    return bad;
}
