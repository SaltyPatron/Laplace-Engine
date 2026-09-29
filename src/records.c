/* Tables whose rows come in records: a treebank's sentences, each with a row for every word.
 *
 * What the record is about (the sentence, as the source gives it) is content like any other. Every row is a word with
 * what the source says of it within that sentence. Each thing said is a claim, a tuple of entities, every part written
 * as the source writes it:
 *   [word, COLUMN, value]          a column the recipe attests, under the column's name
 *   [word, KEY, VALUE]             each part of a field of KEY is VALUE parts
 *   [word, relation, head word]    the word's relation to the word its head column numbers; a head that numbers no
 *                                  row is recorded as written
 *   [token, [word, word...]]       a row that spans rows: the token, and the words it is
 *   [sentence, KEY, VALUE]         a note of the record
 * The rows form a tree by their heads. A word's node is the path of its dependents' nodes and its own, in the order
 * of the rows, each dependent's node after the claim that relates it: so a phrase analysed the same way is the same
 * node wherever it occurs. The record is the path of what it is about, its notes, its spans, and its trees.
 *
 * The record is what was witnessed; its claims are witnessed within it. One ledger row per record; every claim in it
 * plays. Nothing here renames, reorders or fills in anything the source did not write. */
#define _GNU_SOURCE
#include "engine.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static Ref node_of(Rec *rc, int i){
    Row *w = &rc->row[i];
    if (w->state == 2) return w->node;
    Ref stack[64], *it = stack; int n = 0, cap = 64, self = 0;
    #define PUT(x) do { if (n == cap) { cap *= 2; Ref *m = malloc(sizeof(Ref) * (size_t)cap); memcpy(m, it, sizeof(Ref) * (size_t)n); if (it != stack) free(it); it = m; } it[n++] = (x); } while (0)
    w->state = 1;
    for (int c = w->kid; c >= 0; c = rc->row[c].sib) {
        if (rc->row[c].state == 1) continue;                                /* heads that lead back to themselves: not a tree */
        if (!self && c > i) { self = 1; if (w->nown) PUT(said_record(compose(w->own, (uint32_t)w->nown, tier_over(w->own, (size_t)w->nown)))); else if (w->has_word) PUT(w->word); }
        if (rc->row[c].has_edge) PUT(rc->row[c].edge);
        Ref k = node_of(rc, c); PUT(k);
    }
    if (!self) { if (w->nown) PUT(said_record(compose(w->own, (uint32_t)w->nown, tier_over(w->own, (size_t)w->nown)))); else if (w->has_word) PUT(w->word); }
    if (!n) { w->state = 2; w->node = w->word; return w->node; }
    w->node = said_record(compose(it, (uint32_t)n, tier_over(it, (size_t)n))); w->state = 2;
    if (it != stack) free(it);
    return w->node;
    #undef PUT
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
    for (int i = 0; has_about && i < nnote; i++) {                           /* the record's notes, said of what it is about */
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
            else if (h < 0) { Ref t[3] = { w->word, rel, string_ref(w->cell[pl->head].p, w->cell[pl->head].n) }; w->edge = tuple(rc, t, 3); w->has_edge = 1; w->head = -2; }
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
    for (int i = 0; i < rc->n; i++) { Row *w = &rc->row[i]; if (w->span || w->head < 0) continue;
        Row *h = &rc->row[w->head]; if (h->last < 0) h->kid = i; else rc->row[h->last].sib = i; h->last = i; }
    for (int i = 0; i < rc->n; i++) {                                         /* the trees, each from a row no row is the head of */
        Row *w = &rc->row[i]; if (w->span || w->head >= 0 || w->state) continue;
        Ref k = node_of(rc, i); if (!w->has_word && !w->nown && w->kid < 0) continue;
        if (w->has_edge) part_push(rc, w->edge);
        part_push(rc, k);
    }
    for (int i = 0; i < rc->n; i++) {                                         /* rows whose heads lead back to themselves */
        Row *w = &rc->row[i]; if (w->span || w->state) continue;
        Ref k = node_of(rc, i); if (w->has_edge) part_push(rc, w->edge); part_push(rc, k);
    }
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
    Ref *all = malloc(sizeof(Ref) * (tot + 1)); size_t m = 0; uint32_t ordinal = 0;
    for (size_t t = 0; t < k; t++) {
        memcpy(all + m, piece[t].rec, sizeof(Ref) * piece[t].n); m += piece[t].n; free(piece[t].rec);
        for (uint64_t j = 0; j < piece[t].ev.n; j++) { Event x; memcpy(&x, &piece[t].ev.e[j], sizeof x); if (x.kind == EV_RECORD) x.position = ++ordinal; ev_push(&f->ev, &x); }
        free(piece[t].ev.e);
    }
    (void)m;                                                                /* the order of the records is their position in the ledger */
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
