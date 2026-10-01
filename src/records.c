/* Tables whose rows come in records: a treebank's sentences, each with a row for every word.
 *
 * What the record is about (the sentence, as the source gives it) is content like any other. What the source says
 * of its words, column by column, is a layer: the column's values in the order of the words, one path aligned to the
 * sentence (Research: Semantics Experiments, "Annotation layers as content": one claim per sentence per layer, not
 * one record per token per layer; composed like text, layers share their small sub-structures almost entirely).
 * Each thing said is a claim, a tuple of entities, every part written as the source writes it:
 *   [sentence, COLUMN, layer]      a column the recipe attests: its values down the sentence, a path of them
 *   [word, COLUMN, value]          and, of each word, the value the column gives it: one claim per word and value,
 *                                  wherever they meet, which is what stands for "dog is a NOUN"
 *   [sentence, KEY, layer]         a field of KEY is VALUE parts: the layer of each word's parts, under each key
 *   [sentence, relation, layer]    the relations: for each word, its relation and the word it relates to, as a path
 *   [word, relation, head word]    and each relation itself, once wherever it occurs
 *   [token, [word, word...]]       a row that spans rows: the token, and the words it is
 *   [sentence, KEY, VALUE]         a note of the record, unless KEY is one of the source's keys (key KEY...): how it
 *                                  numbers its sentences and documents is recorded nowhere
 * The record is the path of what it is about, its notes, its spans, and its layers. A row's number (number COLUMN)
 * is how the source points at its rows: never recorded.
 *
 * The record is what was witnessed; its claims are witnessed within it. One ledger row per record; every claim in it
 * plays. Nothing here renames, reorders or fills in anything the source did not write. */
#define _GNU_SOURCE
#include "engine.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

typedef struct { const uint8_t *p; size_t n; } Cell;
typedef struct {
    Cell cell[64]; int nc;
    Cell id; int span, head, kid, sib, last;          /* head: a row, or -1 none, or -2 written but no row; kid/sib: its dependents */
    Ref word; int has_word;
    Ref *own; int nown, cown;                         /* what is said of the word itself */
    Ref edge; int has_edge;                           /* what relates it to its head */
    Ref node; int state;
} Row;
typedef struct { Row *row; int n, cap; Ref *part; int np, cp; Ref *claim; int ncl, ccl; } Rec;
typedef struct { int word, number, rel, head, pairs[8], relations[4], attest[64], nattest; float enter_rating, enter_deviation; size_t about_len, is_len, empty_len; } Plan;

static int col_of(const Recipe *r, const char *name){
    if (!name[0]) return -1;
    for (int i = 0; i < r->ncolumn; i++) if (!strcmp(r->column[i], name)) return i;
    fprintf(stderr, "%s: there is no column %s\n", r->name, name); exit(2);
}
static int is_empty(const Recipe *r, const Plan *pl, Cell c){ return !c.n || (pl->empty_len && c.n == pl->empty_len && !memcmp(c.p, r->empty, c.n)); }
static Cell trimmed(Cell c){
    while (c.n && (c.p[0] == ' ' || c.p[0] == '\t')) { c.p++; c.n--; }
    while (c.n && (c.p[c.n - 1] == ' ' || c.p[c.n - 1] == '\t' || c.p[c.n - 1] == '\r')) c.n--;
    return c;
}
static uint8_t tier_over(const Ref *r, size_t n){ uint8_t t = 0; for (size_t i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }
static Ref tuple(Rec *rc, const Ref *part, int n){
    Ref c = said_claim(compose(part, (uint32_t)n, tier_over(part, (size_t)n)));
    for (int i = 0; i < rc->ncl; i++) if (!memcmp(&rc->claim[i].id, &c.id, 16)) return c;      /* said twice in one record: witnessed in it once */
    if (rc->ncl == rc->ccl) { rc->ccl = rc->ccl ? rc->ccl * 2 : 256; rc->claim = xrealloc(rc->claim, sizeof(Ref) * (size_t)rc->ccl); }
    rc->claim[rc->ncl++] = c; return c;
}
static void own_push(Row *w, Ref c){
    if (w->nown == w->cown) { w->cown = w->cown ? w->cown * 2 : 16; w->own = xrealloc(w->own, sizeof(Ref) * (size_t)w->cown); }
    w->own[w->nown++] = c;
}
static void part_push(Rec *rc, Ref x){
    if (rc->np == rc->cp) { rc->cp = rc->cp ? rc->cp * 2 : 64; rc->part = xrealloc(rc->part, sizeof(Ref) * (size_t)rc->cp); }
    rc->part[rc->np++] = x;
}
static int row_numbered(const Rec *rc, Cell id){
    for (int i = 0; i < rc->n; i++) if (!rc->row[i].span && rc->row[i].id.n == id.n && !memcmp(rc->row[i].id.p, id.p, id.n)) return i;
    return -1;
}
/* The layers of a record: for each attested column and each pairs column, the values down the words. */
typedef struct { Ref *v; int n, cap; } Layer;
static void layer_push(Layer *l, Ref x){ if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = xrealloc(l->v, sizeof(Ref) * (size_t)l->cap); } x.said = 0; l->v[l->n++] = x; }
static int key_note(const Recipe *r, Cell k){ for (int i = 0; i < r->nkey; i++) if (strlen(r->key[i]) == k.n && !memcmp(r->key[i], k.p, k.n)) return 1; return 0; }
/* What is said of a row's word in its own fields. */
static void said_of(const Recipe *r, const Plan *pl, Rec *rc, Row *w){
    for (int a = 0; a < pl->nattest; a++) {
        int ci = pl->attest[a]; if (ci >= w->nc || is_empty(r, pl, w->cell[ci])) continue;
        Ref t[3] = { w->word, string_ref((const uint8_t *)r->column[ci], strlen(r->column[ci])), string_ref(w->cell[ci].p, w->cell[ci].n) };
        own_push(w, tuple(rc, t, 3));
    }
    for (int k = 0; k < r->npairs; k++) {
        int ci = pl->pairs[k]; if (ci >= w->nc || is_empty(r, pl, w->cell[ci])) continue;
        const uint8_t *p = w->cell[ci].p, *e = p + w->cell[ci].n;
        while (p < e) {
            const uint8_t *q = memchr(p, r->pairs[k].part, (size_t)(e - p)); if (!q) q = e;
            const uint8_t *is = memchr(p, r->pairs[k].is, (size_t)(q - p));
            if (!is) { if (q > p) { Ref t[3] = { w->word, string_ref((const uint8_t *)r->column[ci], strlen(r->column[ci])), string_ref(p, (size_t)(q - p)) }; own_push(w, tuple(rc, t, 3)); } }
            else if (is > p) {
                Ref key = string_ref(p, (size_t)(is - p)); const uint8_t *v = is + 1;
                while (v < q) {
                    const uint8_t *ve = r->pairs[k].list ? memchr(v, r->pairs[k].list, (size_t)(q - v)) : NULL; if (!ve) ve = q;
                    if (ve > v) { Ref t[3] = { w->word, key, string_ref(v, (size_t)(ve - v)) }; own_push(w, tuple(rc, t, 3)); }
                    v = ve + 1;
                }
            }
            p = q + 1;
        }
    }
}
/* One record: its notes and its rows, between two offsets. Returns 0 if it holds nothing. */
static int record(const Recipe *r, const Plan *pl, Ctx *ctx, Rec *rc, const uint8_t *src, size_t lo, size_t hi, Events *ev, Ref *out){
    rc->n = rc->np = rc->ncl = 0; Ref about = { 0 }; int has_about = 0;
    typedef struct { Cell k, v; } Note; Note note[64]; int nnote = 0;
    for (size_t at = lo; at < hi; ) {
        const uint8_t *nl = memchr(src + at, '\n', hi - at); size_t e = nl ? (size_t)(nl - src) : hi, next = e + 1; if (e > at && src[e - 1] == '\r') e--;
        if (e == at) { at = next; continue; }
        if (r->comment && src[at] == (uint8_t)r->comment) {
            Cell line = trimmed((Cell){ src + at + 1, e - at - 1 }), k = line, v = { NULL, 0 };
            const uint8_t *is = pl->is_len ? memmem(line.p, line.n, r->note_is, pl->is_len) : NULL;
            if (is) { k = trimmed((Cell){ line.p, (size_t)(is - line.p) }); v = trimmed((Cell){ is + pl->is_len, line.n - (size_t)(is - line.p) - pl->is_len }); }
            if (k.n && v.n && k.n == pl->about_len && !memcmp(k.p, r->about, k.n)) { about = text_ref(ctx, v.p, v.n); has_about = 1; }
            else if (k.n && nnote < 64) note[nnote++] = (Note){ k, v };
            at = next; continue;
        }
        if (rc->n == rc->cap) { int oc = rc->cap; rc->cap = oc ? oc * 2 : 64; rc->row = xrealloc(rc->row, sizeof(Row) * (size_t)rc->cap); memset(rc->row + oc, 0, sizeof(Row) * (size_t)(rc->cap - oc)); }
        Row *w = &rc->row[rc->n++]; Ref *own = w->own; int cown = w->cown; memset(w, 0, sizeof *w); w->own = own; w->cown = cown;
        for (size_t i = at, f0 = at; i <= e && w->nc < 64; i++) if (i == e || src[i] == (uint8_t)r->separator) { w->cell[w->nc++] = (Cell){ src + f0, i - f0 }; f0 = i + 1; }
        w->head = -1; w->kid = w->last = w->sib = -1;
        if (pl->number >= 0 && pl->number < w->nc) { w->id = w->cell[pl->number]; w->span = r->span && memchr(w->id.p, r->span, w->id.n) != NULL; }
        if (pl->word < w->nc && !is_empty(r, pl, w->cell[pl->word])) { w->word = string_ref(w->cell[pl->word].p, w->cell[pl->word].n); w->has_word = 1; }
        at = next;
    }
    if (!rc->n && !has_about) return 0;
    if (has_about) part_push(rc, about);
    for (int i = 0; has_about && i < nnote; i++) {                           /* the record's notes, said of what it is about; its keys are not */
        if (key_note(r, note[i].k)) continue;
        Ref t[3] = { about, string_ref(note[i].k.p, note[i].k.n) }; int n = 2;
        if (note[i].v.n) t[n++] = note[i].v.n > 256 ? text_ref(ctx, note[i].v.p, note[i].v.n) : string_ref(note[i].v.p, note[i].v.n);
        part_push(rc, tuple(rc, t, n));
    }
    for (int i = 0; i < rc->n; i++) {
        Row *w = &rc->row[i]; if (!w->has_word) continue;
        said_of(r, pl, rc, w);
        if (w->span) {                                                       /* a token, and the words it is */
            const uint8_t *sp = memchr(w->id.p, r->span, w->id.n);
            int a = row_numbered(rc, (Cell){ w->id.p, (size_t)(sp - w->id.p) }), b = row_numbered(rc, (Cell){ sp + 1, w->id.n - (size_t)(sp - w->id.p) - 1 });
            if (a >= 0 && b >= a) {
                Ref stack[16], *ws = b - a + 1 <= 16 ? stack : malloc(sizeof(Ref) * (size_t)(b - a + 1)); int n = 0;
                for (int j = a; j <= b; j++) if (!rc->row[j].span && rc->row[j].has_word) ws[n++] = rc->row[j].word;
                if (n) { Ref t[2] = { w->word, compose(ws, (uint32_t)n, tier_over(ws, (size_t)n)) }; part_push(rc, tuple(rc, t, 2)); }
                if (ws != stack) free(ws);
            }
            for (int j = 0; j < w->nown; j++) part_push(rc, w->own[j]);
            continue;
        }
        if (pl->head >= 0 && pl->head < w->nc && pl->rel >= 0 && pl->rel < w->nc && !is_empty(r, pl, w->cell[pl->head]) && !is_empty(r, pl, w->cell[pl->rel])) {
            int h = row_numbered(rc, w->cell[pl->head]); Ref rel = string_ref(w->cell[pl->rel].p, w->cell[pl->rel].n);
            if (h >= 0 && h != i && rc->row[h].has_word) { Ref t[3] = { w->word, rel, rc->row[h].word }; w->edge = tuple(rc, t, 3); w->has_edge = 1; w->head = h; }
            else if (h < 0) { Ref t[2] = { w->word, rel }; w->edge = tuple(rc, t, 2); w->has_edge = 1; w->head = -2; }      /* a head that numbers no row (0: the root): the relation alone */
        }
        for (int k = 0; k < r->nrelations; k++) {
            int ci = pl->relations[k]; if (ci >= w->nc || is_empty(r, pl, w->cell[ci])) continue;
            const uint8_t *p = w->cell[ci].p, *e = p + w->cell[ci].n;
            while (p < e) {
                const uint8_t *q = memchr(p, r->relations[k].part, (size_t)(e - p)); if (!q) q = e;
                const uint8_t *is = memchr(p, r->relations[k].is, (size_t)(q - p));
                if (is && is > p && is + 1 < q) {
                    Cell hid = { p, (size_t)(is - p) }; int h = row_numbered(rc, hid); Ref rel = string_ref(is + 1, (size_t)(q - is - 1));
                    Ref t[3] = { w->word, rel, h >= 0 && rc->row[h].has_word ? rc->row[h].word : string_ref(hid.p, hid.n) };
                    if (h < 0 || rc->row[h].has_word) {                     /* one the row's own relation already says is said once */
                        Ref c = said_claim(compose(t, 3, tier_over(t, 3)));
                        if (!w->has_edge || memcmp(&c.id, &w->edge.id, 16)) own_push(w, tuple(rc, t, 3)); }
                }
                p = q + 1;
            }
        }
    }
    /* the layers: each attested column's values down the words; each pairs column's parts; the relations */
    if (has_about) {
        for (int a = 0; a < pl->nattest; a++) { int ci = pl->attest[a]; Layer l = { 0 }; int any = 0;
            for (int i = 0; i < rc->n; i++) { Row *w = &rc->row[i]; if (w->span || !w->has_word) continue;
                if (ci < w->nc && !is_empty(r, pl, w->cell[ci])) { layer_push(&l, string_ref(w->cell[ci].p, w->cell[ci].n)); any = 1; } else layer_push(&l, string_ref((const uint8_t *)r->empty, strlen(r->empty))); }
            if (any && l.n) { Ref t[3] = { about, string_ref((const uint8_t *)r->column[ci], strlen(r->column[ci])), l.n == 1 ? l.v[0] : compose(l.v, (uint32_t)l.n, tier_over(l.v, (size_t)l.n)) }; part_push(rc, tuple(rc, t, 3)); }
            free(l.v); }
        for (int k = 0; k < r->npairs; k++) { int ci = pl->pairs[k]; Layer l = { 0 }; int any = 0;
            for (int i = 0; i < rc->n; i++) { Row *w = &rc->row[i]; if (w->span || !w->has_word) continue;
                if (ci < w->nc && !is_empty(r, pl, w->cell[ci])) { Ref parts[64]; int np = 0; const uint8_t *p = w->cell[ci].p, *e = p + w->cell[ci].n;
                    while (p < e && np < 64) { const uint8_t *q = memchr(p, r->pairs[k].part, (size_t)(e - p)); if (!q) q = e; const uint8_t *is = memchr(p, r->pairs[k].is, (size_t)(q - p));
                        if (is && is > p && is + 1 < q) { Ref pr[2] = { string_ref(p, (size_t)(is - p)), string_ref(is + 1, (size_t)(q - is - 1)) }; parts[np++] = said_tuple(compose(pr, 2, tier_over(pr, 2))); }
                        else if (q > p) parts[np++] = string_ref(p, (size_t)(q - p));
                        p = q + 1; }
                    layer_push(&l, np == 1 ? parts[0] : np ? said_tuple(compose(parts, (uint32_t)np, tier_over(parts, (size_t)np))) : string_ref((const uint8_t *)r->empty, strlen(r->empty))); any |= np > 0; }
                else layer_push(&l, string_ref((const uint8_t *)r->empty, strlen(r->empty))); }
            if (any && l.n) { Ref t[3] = { about, string_ref((const uint8_t *)r->column[ci], strlen(r->column[ci])), l.n == 1 ? l.v[0] : compose(l.v, (uint32_t)l.n, tier_over(l.v, (size_t)l.n)) }; part_push(rc, tuple(rc, t, 3)); }
            free(l.v); }
        if (pl->rel >= 0 && pl->head >= 0) { Layer l = { 0 }; int any = 0;    /* the relations: for each word, [relation, head word], down the sentence */
            for (int i = 0; i < rc->n; i++) { Row *w = &rc->row[i]; if (w->span || !w->has_word) continue;
                if (w->has_edge && w->head >= 0) { Ref pr[2] = { string_ref(w->cell[pl->rel].p, w->cell[pl->rel].n), rc->row[w->head].word }; layer_push(&l, said_tuple(compose(pr, 2, tier_over(pr, 2)))); any = 1; }
                else if (w->has_edge) { layer_push(&l, string_ref(w->cell[pl->rel].p, w->cell[pl->rel].n)); any = 1; }
                else layer_push(&l, string_ref((const uint8_t *)r->empty, strlen(r->empty))); }
            if (any && l.n) { Ref t[3] = { about, string_ref((const uint8_t *)r->column[pl->rel], strlen(r->column[pl->rel])), l.n == 1 ? l.v[0] : compose(l.v, (uint32_t)l.n, tier_over(l.v, (size_t)l.n)) }; part_push(rc, tuple(rc, t, 3)); }
            free(l.v); }
    }
    for (int i = 0; i < rc->n; i++) { Row *w = &rc->row[i]; if (w->span || !w->has_word) continue;   /* every claim of every word, once, as part of the record */
        if (w->has_edge) part_push(rc, w->edge);
        for (int j = 0; j < w->nown; j++) part_push(rc, w->own[j]); }
    if (!rc->np) return 0;
    *out = rc->np == 1 ? rc->part[0] : compose(rc->part, (uint32_t)rc->np, tier_over(rc->part, (size_t)rc->np));
    Event x = { out->id, out->id, 1.0f, pl->enter_rating, pl->enter_deviation, 0, EV_RECORD }; ev_push(ev, &x);
    for (int i = 0; i < rc->ncl; i++) { Event m = { rc->claim[i].id, out->id, 1.0f, pl->enter_rating, pl->enter_deviation, 0, EV_MEMBER }; ev_push(ev, &m); }
    return 1;
}

void attest_records(const Recipe *r, File *f, const uint8_t *src, size_t n){
    Plan pl = { 0 }; pl.word = col_of(r, r->word); pl.number = col_of(r, r->number);
    pl.rel = col_of(r, r->relation.rel); pl.head = col_of(r, r->relation.head);
    for (int k = 0; k < r->npairs; k++) pl.pairs[k] = col_of(r, r->pairs[k].col);
    for (int k = 0; k < r->nrelations; k++) pl.relations[k] = col_of(r, r->relations[k].col);
    pl.enter_rating = 1500.0f; pl.enter_deviation = 0.0f;
    for (int k = 0; k < r->nblock; k++) { const Block *b = &r->block[k]; if (b->is_map) continue;
        pl.enter_rating = b->enter_rating; pl.enter_deviation = b->enter_deviation;
        for (int a = 0; a < b->nattest && pl.nattest < 64; a++) pl.attest[pl.nattest++] = col_of(r, b->attest[a]); }
    pl.about_len = strlen(r->about); pl.is_len = strlen(r->note_is); pl.empty_len = strlen(r->empty);

    /* records end at an empty line; stretches of them are read on every core and joined in order */
    size_t ncut = n / (2u << 20) + 1; size_t *cut = malloc(sizeof(size_t) * (ncut + 2)); size_t k = 1; cut[0] = 0;
    for (size_t i = 1; i < ncut; i++) {
        size_t c = n / ncut * i; if (c <= cut[k - 1]) continue;
        while (c < n && !(src[c] == '\n' && c + 1 < n && (src[c + 1] == '\n' || (src[c + 1] == '\r' && c + 2 < n && src[c + 2] == '\n')))) c++;
        if (c >= n) break;
        c += 1; if (c > cut[k - 1] && c < n) cut[k++] = c;                   /* after the line break that ends a record's last row */
    }
    cut[k] = n;
    typedef struct { Ref *rec; size_t n, cap; Events ev; } Piece; Piece *piece = calloc(k, sizeof(Piece));
    #pragma omp taskloop grainsize(1)
    for (size_t t = 0; t < k; t++) {
        Rec rc = { 0 }; Ctx *ctx = CTX[omp_get_thread_num()]; Piece *pc = &piece[t];
        for (size_t at = cut[t], end = cut[t + 1]; at < end; ) {
            size_t e = at, rows = 0;                                        /* lines up to an empty one */
            while (e < end) {
                const uint8_t *nl = memchr(src + e, '\n', end - e); size_t le = nl ? (size_t)(nl - src) : end, len = le - e;
                if (len && src[le - 1] == '\r') len--;
                if (!len) break;
                rows++; e = nl ? le + 1 : end;
            }
            Ref out;
            if (rows && record(r, &pl, ctx, &rc, src, at, e, &pc->ev, &out)) {
                if (pc->n == pc->cap) { pc->cap = pc->cap ? pc->cap * 2 : 1024; pc->rec = xrealloc(pc->rec, sizeof(Ref) * pc->cap); }
                pc->rec[pc->n++] = out;
            }
            if (e < end) { const uint8_t *nl = memchr(src + e, '\n', end - e); e = nl ? (size_t)(nl - src) + 1 : end; }     /* past the empty line */
            at = e;
        }
        for (int i = 0; i < rc.cap; i++) free(rc.row[i].own);
        free(rc.row); free(rc.part); free(rc.claim);
    }
    size_t tot = 0; for (size_t t = 0; t < k; t++) tot += piece[t].n;
    Ref *all = malloc(sizeof(Ref) * (tot + 1)); size_t m = 0; uint32_t ordinal = (uint32_t)f->records;
    for (size_t t = 0; t < k; t++) {
        memcpy(all + m, piece[t].rec, sizeof(Ref) * piece[t].n); m += piece[t].n; free(piece[t].rec);
        for (uint64_t j = 0; j < piece[t].ev.n; j++) { Event x; memcpy(&x, &piece[t].ev.e[j], sizeof x); if (x.kind == EV_RECORD) x.position = ++ordinal; ev_push(&f->ev, &x); }
        free(piece[t].ev.e);
    }
    f->records = ordinal; (void)m;                                          /* the order of the records is their position in the ledger */
    free(all); free(piece); free(cut);
}

/* ---- records of KEY IS VALUE lines. Every field is said of what the record is about, under the field's own key. */
void attest_fields(const Recipe *r, File *f, const uint8_t *src, size_t n){
    size_t rl = strlen(r->record_line), il = strlen(r->field_is); const Block *b = r->nblock ? &r->block[0] : NULL;
    float er = b ? b->enter_rating : 1500.0f, ed = b ? b->enter_deviation : 0.0f;
    typedef struct { Cell k; uint8_t *v; size_t vn, vcap; } Field; Field fld[256]; int nf = 0; memset(fld, 0, sizeof fld);
    for (size_t at = 0; at <= n; ) {
        const uint8_t *nl = at < n ? memchr(src + at, '\n', n - at) : NULL; size_t e = nl ? (size_t)(nl - src) : n, next = e + 1; if (e > at && src[e - 1] == '\r') e--;
        int ends = at >= n || (rl && e - at == rl && !memcmp(src + at, r->record_line, rl)) || (!rl && e == at);
        if (ends) {
            int ab = -1; char keys[64]; snprintf(keys, sizeof keys, "%s", r->about); char *save = NULL;
            for (char *k = strtok_r(keys, " ", &save); k && ab < 0; k = strtok_r(NULL, " ", &save))
                for (int i = 0; i < nf && ab < 0; i++) if (fld[i].k.n == strlen(k) && !memcmp(fld[i].k.p, k, fld[i].k.n) && fld[i].vn) ab = i;
            if (ab >= 0) { Ref about = string_ref(fld[ab].v, fld[ab].vn);
                for (int i = 0; i < nf; i++) { if (i == ab || !fld[i].vn) continue;
                    Ref t[3] = { about, string_ref(fld[i].k.p, fld[i].k.n), fld[i].vn > 256 ? text_ref(CTX[omp_get_thread_num()], fld[i].v, fld[i].vn) : string_ref(fld[i].v, fld[i].vn) };
                    Ref c = said_claim(compose(t, 3, tier_over(t, 3))); Event x = { c.id, c.id, 1.0f, er, ed, 0, EV_CLAIM }; ev_push(&f->ev, &x); } }
            nf = 0; if (at >= n) break; at = next; continue;
        }
        if ((src[at] == ' ' || src[at] == '\t') && nf) {                       /* the line before, continued */
            Cell c = trimmed((Cell){ src + at, e - at }); Field *x = &fld[nf - 1];
            if (x->vn + c.n + 1 > x->vcap) { x->vcap = (x->vn + c.n + 1) * 2; x->v = xrealloc(x->v, x->vcap); }
            if (x->vn && c.n) x->v[x->vn++] = ' ';
            memcpy(x->v + x->vn, c.p, c.n); x->vn += c.n;
        } else if (nf < 256) {
            const uint8_t *is = il ? memmem(src + at, e - at, r->field_is, il) : NULL;
            if (is) { Field *x = &fld[nf++]; x->k = trimmed((Cell){ src + at, (size_t)(is - src - at) }); Cell v = trimmed((Cell){ is + il, e - (size_t)(is - src) - il });
                if (v.n + 1 > x->vcap) { x->vcap = v.n * 2 + 64; x->v = xrealloc(x->v, x->vcap); } memcpy(x->v, v.p, v.n); x->vn = v.n; }
        }
        at = next;
    }
    for (int i = 0; i < 256; i++) free(fld[i].v);
}

/* ---- lines that say something where they match a pattern: a page of a source's own documentation */
void attest_lines(const Recipe *r, File *f, const uint8_t *src, size_t n){
    Ref about = { 0 }; int has_about = 0; char *ln = malloc(1 << 16); regmatch_t m[5];
    for (int pass = 0; pass < 2; pass++)
    for (size_t at = 0; at < n; ) {
        const uint8_t *nl = memchr(src + at, '\n', n - at); size_t e = nl ? (size_t)(nl - src) : n, next = e + 1; if (e > at && src[e - 1] == '\r') e--;
        size_t l = e - at; if (!l || l >= (1 << 16)) { at = next; continue; }
        memcpy(ln, src + at, l); ln[l] = 0;
        if (!pass) { if (!has_about && r->about_re && !regexec(r->about_re, ln, 2, m, 0) && m[1].rm_so >= 0 && m[1].rm_eo > m[1].rm_so) { about = string_ref((const uint8_t *)ln + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so)); has_about = 1; } at = next; if (has_about) break; continue; }
        for (int k = 0; k < r->nblock; k++) { const Block *b = &r->block[k]; if (b->is_map || !b->line_re || regexec(b->line_re, ln, 5, m, 0)) continue;
            Ref part[4]; int np = 0; for (int g = 1; g < 5 && np < 4; g++) if (m[g].rm_so >= 0 && m[g].rm_eo > m[g].rm_so) { size_t gl = (size_t)(m[g].rm_eo - m[g].rm_so); part[np++] = r->path_sep && ln[m[g].rm_so] == r->path_sep ? path_ref((const uint8_t *)ln + m[g].rm_so, gl, r->path_sep, r->path_join) : gl > 256 ? text_ref(CTX[omp_get_thread_num()], (const uint8_t *)ln + m[g].rm_so, gl) : string_ref((const uint8_t *)ln + m[g].rm_so, gl); }
            Ref t[4]; int nt = 0;
            if (np == 2 && b->predicate[0]) { t[nt++] = part[0]; t[nt++] = string_ref((const uint8_t *)b->predicate, strlen(b->predicate)); t[nt++] = part[1]; }
            else if (np == 2 && !b->pair) { if (!has_about) continue; t[nt++] = about; t[nt++] = part[0]; t[nt++] = part[1]; }
            else if (np >= 2) { for (int i = 0; i < np; i++) t[nt++] = part[i]; }
            else continue;
            Ref c = said_claim(compose(t, (uint32_t)nt, tier_over(t, (size_t)nt))); Event x = { c.id, c.id, 1.0f, b->enter_rating, b->enter_deviation, 0, EV_CLAIM }; ev_push(&f->ev, &x); }
        at = next;
    }
    free(ln);
}
