/* Native output construction: record-local claim deduplication, event runs,
 * aligned layers and provenance vertices. The statement VM decides what is
 * asserted; this module owns how those products are retained and contained. */
#define _GNU_SOURCE
#include "record_output.h"
#include "record_values.h"
#include "record_surface.h"
#include "record_diagnostics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CSeen { lp_id claim, qualifier, dependence; uint64_t epoch, ev; };
typedef struct { Ref head; int parent, first, next, last; } TNode;
static uint64_t claims_unplaced;                                              /* a claim no reference could be made of: in no record (said) */

void record_refs_push(Refs *a, const Ref *x){
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

void record_worker_outputs(RecordWorker *sink){ sink->things.arena = sink->meta.arena = sink->grp.arena = &sink->output; }

void record_event_push(RecordWorker *sink, const Event *event){
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

void record_workers_free(RecordWorker *sinks, size_t count){
    for (size_t i = 0; i < count; i++) arena_release(&sinks[i].output);
    free(sinks);
}

static char *output_text(RecordWorker *sink, const char *bytes, size_t length){
    if (!bytes) return NULL;
    if (length == SIZE_MAX) { fputs("output text exceeds the arena\n", stderr); abort(); }
    char *text = arena_alloc(&sink->output, length + 1); memcpy(text, bytes, length); text[length] = 0; return text;
}

void record_highway_entry(RecordWorker *k, int what, int line, const Ref *thing, const char *a, size_t al, const char *b, size_t bl){
    if (k->nhr == k->chr) {
        size_t capacity = k->chr ? k->chr * 2 : 1024;
        if (capacity < k->chr || capacity > SIZE_MAX / sizeof(HwRec)) { fputs("highway records exceed the arena\n", stderr); abort(); }
        HwRec *records = arena_alloc(&k->output, capacity * sizeof *records);
        if (k->nhr) memcpy(records, k->hr, k->nhr * sizeof *records);
        k->hr = records; k->chr = capacity;
    }
    HwRec *x = &k->hr[k->nhr++]; x->what = what; x->line = line; if (thing) x->thing = *thing; else memset(&x->thing, 0, sizeof x->thing);
    x->a = output_text(k, a, al); x->b = output_text(k, b, bl);
}
static int said_already(RecordWorker *k, const lp_id *c){
    if (!k->cs_epoch) k->cs_epoch = 1;                                      /* a slot never used is of epoch 0: no part's */
    lp_id qualifier; memset(&qualifier, 0, sizeof qualifier); if (k->qualified) qualifier = k->qualifier.id;     /* an unqualified source assertion: no qualifier ID */
    if ((k->cs_n + 1) * 2 > k->cs_cap) {                                     /* half full: twice the room, this part's keys moved */
        struct CSeen *o = k->cs; uint64_t oc = k->cs_cap; k->cs_cap = oc ? oc * 2 : 256;
        if (k->cs_cap < oc || k->cs_cap > SIZE_MAX / sizeof(struct CSeen)) { fputs("claim index exceeds the output arena\n", stderr); abort(); }
        k->cs = arena_alloc(&k->output, (size_t)k->cs_cap * sizeof *k->cs); memset(k->cs, 0, (size_t)k->cs_cap * sizeof *k->cs);
        for (uint64_t i = 0; i < oc; i++) if (o[i].epoch == k->cs_epoch) { uint64_t h, w, d; memcpy(&h, o[i].claim.b, 8); memcpy(&w, o[i].qualifier.b + 8, 8); memcpy(&d,o[i].dependence.b,8); uint64_t s = (h ^ w ^ d) & (k->cs_cap - 1);
            while (k->cs[s].epoch == k->cs_epoch) s = (s + 1) & (k->cs_cap - 1); k->cs[s] = o[i]; }
    }
    uint64_t h, w, d; memcpy(&h, c->b, 8); memcpy(&w, qualifier.b + 8, 8); memcpy(&d,k->dependence.id.b,8); uint64_t s = (h ^ w ^ d) & (k->cs_cap - 1);
    while (k->cs[s].epoch == k->cs_epoch) { if (!memcmp(&k->cs[s].claim, c, 16) && !memcmp(&k->cs[s].qualifier, &qualifier, 16) && lp_id_eq(&k->cs[s].dependence,&k->dependence.id)) {
            uint64_t e = k->cs[s].ev; if (e < k->ev.n && k->ev.e[e].run < (1u << LP_M_RUN_BITS) - 1) k->ev.e[e].run++; return 1; }
        s = (s + 1) & (k->cs_cap - 1); }
    k->cs[s].claim = *c; k->cs[s].qualifier = qualifier; k->cs[s].dependence=k->dependence.id; k->cs[s].epoch = k->cs_epoch; k->cs[s].ev = k->ev.n; k->cs_n++; return 0;
}

void record_emit_event(RecordWorker *k, const lp_id *c, float score){
    Event x = {.claim=*c, .witnessed=*c, .score=score, .enter_rating=k->er, .enter_deviation=k->ed, .kind=EV_CLAIM, .run=1, .rec=k->rec};
    if (k->dependence_class) { x.has_dependence=1; x.dependence=k->dependence.id; x.dependence_trust=k->dependence_class->prior; }
    if (k->qualified) { x.has_qualifier = 1; x.qualifier = k->qualifier.id; } record_event_push(k, &x);
}

void record_emit_claim(RecordWorker *k, Ref *part, int n){
    for (int i = 0; i < n; i++) if (part[i].said != LP_SAID_TUPLE) part[i].said = 0;      /* a tuple held by a claim stays a tuple: M says so, the ID is the same */
    Ref c = said_claim(compose(part, (uint32_t)n, ref_above(part, (size_t)n)));
    if (said_already(k, &c.id)) return;
    record_emit_event(k, &c.id, k->score);
    record_refs_push(&k->grp, &c);
}

void record_group_claims(RecordWorker *k, uint64_t e0, const Ref *about){
    (void)about; uint32_t at = 0;
    for (uint64_t i = e0; i < k->ev.n; i++) if (k->ev.e[i].kind == EV_CLAIM && !k->ev.e[i].position) k->ev.e[i].position = ++at;
}

static Ref empty_ref(const Say *s){ return s->lay.nempty ? string_ref((const uint8_t *)s->lay.empty[0], (size_t)s->lay.emptylen[0]) : string_ref((const uint8_t *)"_", 1); }

static int field_of(RecordWorker *k, const SemanticRecord *t, int32_t row, const char *name, Ref *out){
    int32_t c = record_select_first(t, row, name); if (c < 0) return 0;
    Ref v; if (!record_entity(k, t, c, &v)) return 0; if (v.said != LP_SAID_TUPLE) v.said = 0; *out = v; return 1;
}

static int32_t text_int(const SemanticRecord *t, int32_t row, const char *name, int *ok){
    int32_t c = record_select_first(t, row, name); *ok = 0; if (c < 0) return 0;
    const SemanticValue *x = &t->n[c]; if (x->kind == S_GROUP && record_field_first(x) >= 0 && record_field_next(&t->n[record_field_first(x)]) < 0) x = &t->n[record_field_first(x)];
    if (!x->vlen || x->vlen > 9) return 0; int32_t v = 0; for (uint32_t i = 0; i < x->vlen; i++) { if (x->val[i] < '0' || x->val[i] > '9') return 0; v = v * 10 + (x->val[i] - '0'); }
    *ok = 1; return v;
}

static Ref subtree(TNode *tn, int i, int depth, int n){
    Ref part[256]; uint32_t np = 0; int put = 0;
    for (int c = tn[i].first; c >= 0 && depth < n; c = tn[c].next) {
        if (!put && c > i && np < 256) { part[np++] = tn[i].head; put = 1; }
        if (np < 256) { part[np] = subtree(tn, c, depth + 1, n); part[np].said = 0; np++; } }
    if (!put && np < 256) part[np++] = tn[i].head;
    return np == 1 ? part[0] : compose(part, np, ref_above(part, np));
}

static uint32_t record_rows(RecordWorker *k, const SemanticRecord *t, int32_t root, Ref *part, uint32_t cap){
    const Say *s = k->s; uint32_t np = record_surface_parts(k,t,root,part,cap), nrow = 0;
    int lay = s->nlayer && record_field_matches(&t->n[root], s->lay_tier), tree = s->tree_row[0] && record_field_matches(&t->n[root], s->tree_tier);
    if (!lay && !tree) return np;
    size_t mark = scratch_mark();
    uint32_t rcap = t->n[root].arity ? t->n[root].arity : 1;
    int32_t *rows = scratch_alloc(sizeof(int32_t) * rcap);
    for (int32_t c = record_field_first(&t->n[root]); c >= 0; c = record_field_next(&t->n[c])) {
        if (t->n[c].kind != S_GROUP || !record_field_matches(&t->n[c], lay ? s->lay_row : s->tree_row)) continue;
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
            Ref w, rel; if (!record_thing(k, t, rows[r], &w, 0)) continue; if (!field_of(k, t, rows[r], s->tree_rel, &rel)) rel = empty;
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

void record_emit_layers(RecordWorker *worker, const SemanticRecord *record, int32_t root){
    const Say *program=worker->s;
    if (!(program->surface.thing && record_field_matches(&record->n[root],program->surface.tier)) &&
        !(program->nlayer && record_field_matches(&record->n[root],program->lay_tier)) &&
        !(program->tree_row[0] && record_field_matches(&record->n[root],program->tree_tier))) return;
    Ref subject;
    if (!record_thing(worker,record,root,&subject,0)) return;
    Ref layers[20]; uint32_t count=record_rows(worker,record,root,layers,20);
    if (!count) return;
    if (count==1 && lp_id_eq(&subject.id,&layers[0].id)) return;
    Ref bundle[21]; bundle[0]=subject;
    for (uint32_t i=0; i<count; i++) bundle[i+1]=layers[i];
    /* The aligned axes describe one source record together. Separate pairs
     * erase which axis an equal-valued sequence occupies. */
    record_emit_claim(worker,bundle,(int)count+1);
}

int record_claim_vertex(const Event *e, Ref *out){
    int ok; Ref c = ref_of_id(&e->claim, &ok); if (!ok) { __atomic_fetch_add(&claims_unplaced, 1, __ATOMIC_RELAXED); return 0; }
    c.said = LP_SAID_CLAIM; c.outcome = (uint8_t)lp_outcome_of((double)e->score, &c.spare); c.position = e->position; *out = c; return 1;
}

/* A record contains its subject, aligned layers/tree and claim vertices.
 * M carries role, run, outcome and position; an adjacent class qualifier
 * changes dependence, while the source trunk remains the witness. */
void record_build_provenance(RecordWorker *k, const SemanticRecord *t, int32_t root, uint64_t ev0, const Ref *T){
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
    uint64_t most = 0; for (size_t i = 0; i < nr; i++) { uint64_t need = 24; for (uint64_t j = at[i]; j < at[i + 1]; j++) need += (k->ev.e[ord[j]].run ? k->ev.e[ord[j]].run : 1) + 2; if (need > most) most = need; }
    Ref *part = scratch_alloc(sizeof(Ref) * (most ? most : 24));              /* each claim said run times, with content/dependence qualifiers */
    for (size_t ri = 0; ri < nr; ri++) { int32_t r = recs[ri]; uint32_t np = 0; int mine = r == root;
        int t_claim = 0;                                                      /* the thing is itself a claim it says: that claim's vertex stands for it */
        if (mine && T) for (uint64_t j = at[ri]; j < at[ri + 1] && !t_claim; j++) t_claim = !memcmp(&k->ev.e[ord[j]].claim, &T->id, 16);
        if (mine && T && !t_claim) { part[np] = *T; part[np].said = 0; part[np].outcome = 0; part[np].position = 0; part[np].spare = 0; np++; }
        if (mine) np += record_rows(k, t, root, part + np, 20);
        uint32_t nclaims = 0, ones = 0;
        for (uint64_t j = at[ri]; j < at[ri + 1]; j++) { const Event *x = &k->ev.e[ord[j]]; if (x->kind != EV_CLAIM) continue;
            Ref c; if (!record_claim_vertex(x, &c)) continue;
            uint32_t first=np;
            if (x->has_qualifier) {
                int ok; Ref v=ref_of_id(&x->qualifier,&ok);
                if (!ok) { fputs("content qualifier is missing from its record\n",stderr); abort(); }
                v.said=v.outcome=v.position=v.spare=0; part[np++]=v;
            }
            if (x->has_dependence) { int ok; Ref v=ref_of_id(&x->dependence,&ok);
                if (!ok) { fputs("dependence class is missing from its record\n",stderr); abort(); }
                v.said=LP_SAID_DEPENDENCE; v.outcome=v.position=v.spare=0; part[np++]=v;
            }
            uint32_t run = x->run ? x->run : 1; for (uint32_t z = 0; z < run; z++) part[np++] = c;    /* said run times: one vertex, the run in M */
            if (x->has_qualifier) {
                Ref judgment=compose(part+first,np-first,ref_above(part+first,np-first));
                judgment.said=LP_SAID_RECORD; judgment.outcome=judgment.position=judgment.spare=0;
                np=first; part[np++]=judgment;
            }
            nclaims++; ones += run == 1 && !x->has_qualifier && !x->has_dependence; }
        if (!np) continue;
        Ref R;
        if (np == 1 && nclaims == 1 && ones == 1) { R = part[0]; R.said = LP_SAID_RECORD; }      /* one claim said once: the claim, its own record */
        else if (np == 1 && !nclaims) { R = part[0]; R.said = 0; }                               /* only its thing: content that says nothing */
        else { R = compose(part, np, ref_above(part, np)); R.said = nclaims ? LP_SAID_RECORD : 0; R.outcome = 0; R.position = 0; R.spare = 0; }
        record_refs_push(&k->things, &R); }
    scratch_restore(mark);
}

uint64_t say_unplaced(void){ return __atomic_load_n(&claims_unplaced, __ATOMIC_RELAXED); }

void record_merge(RecordWorker *result, const RecordWorker *worker){
    for (uint64_t i = 0; i < worker->ev.n; i++) record_event_push(result, &worker->ev.e[i]);
    for (size_t i = 0; i < worker->meta.n; i++) record_refs_push(&result->meta, &worker->meta.c[i]);
    for (size_t i = 0; i < worker->things.n; i++) record_refs_push(&result->things, &worker->things.c[i]);
    for (size_t i = 0; i < worker->nhr; i++) {
        const HwRec *record = &worker->hr[i];
        record_highway_entry(result, record->what, record->line, &record->thing,
               record->a, record->a ? strlen(record->a) : 0, record->b, record->b ? strlen(record->b) : 0);
    }
    record_merge_diagnostics(result,worker);
}
