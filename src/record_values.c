/* Reduce compiled value rules to Native references, with scoped memoization.
 * Source scheduling, SQL, statement emission and provenance layout are separate. */
#define _GNU_SOURCE
#include "record_values.h"
#include "record_diagnostics.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread long self_cp = -1; static __thread char self_mark;
#define BUSY_MAX 256
static __thread int32_t busy[BUSY_MAX]; static __thread int nbusy, busy_base;     /* the things this thread is composing, innermost last; from busy_base, the reading it is in now */



static int thing_compose(RecordWorker *k, const SemanticRecord *t, int32_t g, Ref *out, int depth);


static void record_key_forms(const KSpec *ks, const uint8_t *v, size_t n, Keys *out, const SemanticRecord *t, int32_t at);

static int codepoints_text(const uint8_t *p, size_t n, uint8_t *out, size_t cap, size_t *len){
    size_t k = 0; const uint8_t *e = p + n;
    while (p < e) { while (p < e && *p == ' ') p++; if (p >= e) break; const uint8_t *t0 = p; while (p < e && *p != ' ') p++;
        size_t o = (size_t)(p - t0);
        if (o == 1 && self_mark && *t0 == (uint8_t)self_mark && self_cp >= 0) { if (k + 4 > cap) return 0; k += lp_utf8_put((uint32_t)self_cp, out + k); continue; } if (o > 2 && t0[0] == 'U' && t0[1] == '+') { t0 += 2; o -= 2; } char h[16]; if (!o || o > 8) return 0; memcpy(h, t0, o); h[o] = 0;
        char *end; unsigned long cp = strtoul(h, &end, 16); if (*end || cp >= LP_NCP || k + 4 > cap) return 0; k += lp_utf8_put((uint32_t)cp, out + k); }
    *len = k; return k > 0;
}

static Ref semantic_text_ref(RecordWorker *sink, const SemanticValue *value){
    if (value->owner != sink->tc_tree) { fputs("text product has no record cache\n", stderr); abort(); }
    size_t at = (size_t)(value - value->owner->n);
    if (at >= sink->tc_n) { fputs("text product is outside its record\n", stderr); abort(); }
    if (!sink->text_ready[at]) {
        sink->text_products[at] = record_text(value->val, value->vlen);
        sink->text_ready[at] = 1;
    }
    return sink->text_products[at];
}

static inline int composing(int32_t g){ for (int i = busy_base; i < nbusy; i++) if (busy[i] == g) return 1; return 0; }

static uint64_t local_key_hash(uint16_t tier, const uint8_t *bytes, size_t length){
    return selector_hash(bytes, length) ^ ((uint64_t)tier * UINT64_C(0x9e3779b97f4a7c15));
}

static void local_key_place(LocalKey *entries, size_t capacity, LocalKey entry){
    size_t at = (entry.hash ^ (entry.hash >> 32)) & (capacity - 1);
    while (entries[at].tier) at = (at + 1) & (capacity - 1);
    entries[at] = entry;
}

static void local_key_grow(RecordWorker *sink){
    size_t capacity = sink->local_capacity ? sink->local_capacity * 2 : 64;
    if (capacity < sink->local_capacity || capacity > SIZE_MAX / sizeof(LocalKey)) { fputs("record key index is too large\n", stderr); abort(); }
    LocalKey *entries = scratch_alloc(capacity * sizeof *entries); memset(entries, 0, capacity * sizeof *entries);
    for (size_t i = 0; i < sink->local_capacity; i++) if (sink->local_keys[i].tier) local_key_place(entries, capacity, sink->local_keys[i]);
    sink->local_keys = entries; sink->local_capacity = capacity;
}

void record_index_keys(RecordWorker *sink, const SemanticRecord *tree){
    const Say *say = sink->s;
    sink->ix_tree = tree; sink->ix_count = tree->count;
    sink->local_keys = NULL; sink->local_capacity = sink->local_count = 0;
    if (!say->nkey) return;
    for (uint32_t group = 0; group < tree->count; group++) {
        const SemanticValue *node = &tree->n[group]; if (node->context || node->kind != S_GROUP || !node->nlen) continue;
        for (int i = 0; i < say->nkey; i++) {
            const Pair *key = &say->key[i]; if (!record_field_named(node, key->tier, key->tier_length)) continue;
            int32_t field = record_select_first(tree, (int32_t)group, key->name);
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

static int32_t keyed(RecordWorker *sink, const SemanticRecord *tree, const TargetCode *target, const uint8_t *bytes, size_t length){
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

int record_value_empty(const Say *s, const SemanticValue *x){ return s_empty(&s->lay, x->val, x->vlen); }

static void record_unknown_save(RecordWorker *k, const SemanticRecord *t, int32_t c, UnkMark *m){
    m->unknown = k->unknown; m->nunk = k->nunk; m->at[0] = c; m->at[1] = c >= 0 && t->n[c].kind == S_GROUP ? record_field_first(&t->n[c]) : -1;
    for (int i = 0; i < 2; i++) m->had[i] = k->tc_tree == t && k->uk && m->at[i] >= 0 && (uint32_t)m->at[i] < k->tc_n ? __atomic_load_n(&k->uk[m->at[i]], __ATOMIC_ACQUIRE) : 1; }

static void record_unknown_restore(RecordWorker *k, const SemanticRecord *t, const UnkMark *m){
    k->unknown = m->unknown; k->nunk = m->nunk;
    for (int i = 0; i < 2; i++) if (!m->had[i] && k->tc_tree == t && k->uk && m->at[i] >= 0 && (uint32_t)m->at[i] < k->tc_n) __atomic_store_n(&k->uk[m->at[i]], 0, __ATOMIC_RELEASE); }

static int record_value_once(RecordWorker *k, const SemanticRecord *t, int32_t c){ return !(k->tc_tree == t && k->uk && (uint32_t)c < k->tc_n && __atomic_exchange_n(&k->uk[c], 1, __ATOMIC_ACQ_REL)); }

static void record_unknown_key(RecordWorker *k, const SemanticRecord *t, int32_t c, const SemanticValue *x){
    if (!record_value_once(k, t, c)) return;
    k->unknown++;
    if (k->nunk < 8) {
        char *dst = k->unknm[k->nunk++];
        size_t nl = x->nlen < 40 ? x->nlen : 40, vl = x->vlen < 48 ? x->vlen : 48;
        memcpy(dst, x->name, nl); dst[nl] = ' '; memcpy(dst + nl + 1, x->val, vl); dst[nl + 1 + vl] = 0;
    }
}

Ref record_text(const uint8_t *p, size_t n){ return n > 256 ? text_ref(CTX[omp_get_thread_num()], p, n) : string_ref(p, n); }

static size_t hex_upper(unsigned long cp, char *out){
    static const char hd[] = "0123456789ABCDEF";
    char tmp[16]; int k = 0;
    do { tmp[k++] = hd[cp & 15]; cp >>= 4; } while (cp > 0);
    while (k < 4) tmp[k++] = '0';
    for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    return (size_t)k;
}

int record_value(RecordWorker *k, const SemanticRecord *t, int32_t c, Ref *out, int depth){
    const SemanticValue *x = &t->n[c]; const Say *s = k->s; const Dis *d = record_field_disposition(s, x);
    if (x->kind == S_GROUP && x->join == S_PIECES && record_field_first(x) >= 0 && record_field_next(&t->n[record_field_first(x)]) >= 0) {      /* a list of values: the tuple of what each is ([display text, target]) */
        Ref pc[64]; uint32_t np = 0; for (int32_t q = record_field_first(x); q >= 0 && np < 64; q = record_field_next(&t->n[q])) { if (t->n[q].kind == S_GROUP) return 0; if (record_value(k, t, q, &pc[np], depth)) { pc[np].said = 0; np++; } }
        if (!np) return 0; *out = np == 1 ? pc[0] : said_tuple(compose(pc, np, ref_above(pc, np))); return 1; }
    if (x->kind == S_GROUP && depth < 16) {                                    /* a part that is a thing: that thing */
        if (k->tc_tree == t && (uint32_t)c < k->tc_n && !composing(c)) { Ref v; if (record_thing(k, t, c, &v, depth + 1)) { *out = v; return 1; } }
        int32_t only = record_field_first(x); if (only >= 0 && record_field_next(&t->n[only]) < 0 && t->n[only].kind == S_GROUP) return record_value(k, t, only, out, depth + 1); }     /* holding one part: what that part is */
    if (x->kind == S_GROUP) {                                                 /* a part that holds only its text: that text, as the part is disposed of */
        int32_t only = record_field_first(x); if (only < 0 || record_field_next(&t->n[only]) >= 0 || t->n[only].kind != S_TEXT) return 0;
        x = &t->n[only];
        if (!d) { if (record_value_empty(s, x)) return 0; *out = record_text(x->val, x->vlen); return 1; }
        c = only; }
    if (record_value_empty(s, x)) return 0;
    if (!d) { if (x->kind == S_TEXT) { *out = record_text(x->val, x->vlen); return 1; } if (x->nlen) record_note_unhandled(k, x); return 0; }
    if (d->what == D_CONTENT) {
        if (s->selfmark && k->selfcp >= 0 && memchr(x->val, s->selfmark, x->vlen)) {    /* the code point itself, written out where the source writes its mark */
            char hex[16]; size_t hl = hex_upper((unsigned long)k->selfcp, hex);
            size_t mark = scratch_mark();
            uint8_t *o = scratch_alloc((size_t)x->vlen * 8 + 8); size_t j = 0;
            for (uint32_t i = 0; i < x->vlen; i++) { if (x->val[i] == (uint8_t)s->selfmark) { memcpy(o + j, hex, hl); j += hl; } else o[j++] = x->val[i]; }
            *out = record_text(o, j); scratch_restore(mark); return 1; }
        *out = semantic_text_ref(k, x); return 1; }
    if (d->what == D_OWN) { Ref w[3] = { k->fw, string_ref(x->name, x->nlen), string_ref(x->val, x->vlen) }; w[0].said = 0; *out = said_tuple(compose(w, 3, ref_above(w, 3))); return 1; }
    if (d->what == D_TYPE) {                                                 /* by the source's key of it; or the type whose content this text is; a text that is neither is left open */
        const uint8_t *kp = x->val; size_t kn = x->vlen;
        for (int more = 1; more; ) { more = 0;                                /* white space around it, a no-break space (U+00A0) among it */
            if (kn && (*kp == ' ' || *kp == '\t')) { kp++; kn--; more = 1; } else if (kn >= 2 && kp[0] == 0xC2 && kp[1] == 0xA0) { kp += 2; kn -= 2; more = 1; }
            if (kn && (kp[kn - 1] == ' ' || kp[kn - 1] == '\t' || kp[kn - 1] == '\r')) { kn--; more = 1; } else if (kn >= 2 && kp[kn - 2] == 0xC2 && kp[kn - 1] == 0xA0) { kn -= 2; more = 1; } }
        Keys kf; kf.n = 0; if (d->k.has_re || d->k.nas) { record_key_forms(&d->k, kp, kn, &kf, t, c); if (!kf.n) return 0; }     /* the key as the lists write it; a key the pattern does not match is no key of theirs */
        int nk = kf.n ? kf.n : 1;                                              /* with no template, the key as written, once */
        for (int j = 0; j < nk; j++) { if (kf.n) { kp = (const uint8_t *)kf.k[j]; kn = strlen(kf.k[j]); } Ref v = string_ref(kp, kn);     /* each template's key in turn: the first a list knows */
            for (uint8_t i = 0; i < d->ntargets; i++) {
                const TargetCode *target = &d->targets[i];
                int has = 0; *out = highway_typed(target->name, kp, kn, &has); if (has) return 1;
                const lp_list *L = HW ? lp_highway_list(HW, target->name) : NULL; if (L && lp_highway_slot(HW, L, &v.id) >= 0) { *out = v; return 1; }
            } }
        if (d->k.node) { if (record_value_once(k, t, c)) record_obligation_note(k, 'm', d->road_label, x->name, x->nlen, NULL, 0); *out = string_ref(kp, kn); return 1; }
        record_unknown_key(k, t, c, x);     /* a key no list of the highway holds: said, never taken for text */
        return 0; }
    if (d->what == D_CODEPOINTS || d->what == D_RANGE) {
        self_cp = k->selfcp; self_mark = s->selfmark;
        const uint8_t *p = x->val; size_t n = x->vlen; while (n && *p == ' ') { p++; n--; } while (n && p[n - 1] == ' ') n--;
        const uint8_t *dd = d->what == D_RANGE ? memmem(p, n, "..", 2) : NULL;
        if (dd) {                                                            /* a range, written as its first and its last: the path of the two */
            uint8_t a[8], b[8]; size_t al, bl; if (!codepoints_text(p, (size_t)(dd - p), a, sizeof a, &al) || !codepoints_text(dd + 2, n - (size_t)(dd - p) - 2, b, sizeof b, &bl)) { *out = record_text(p, n); return 1; }   /* not code points: the text it is written as */
            Ref two[2] = { string_ref(a, al), string_ref(b, bl) }; *out = said_tuple(compose(two, 2, ref_above(two, 2))); return 1; }
        uint8_t buf[512]; size_t l; if (!codepoints_text(p, n, buf, sizeof buf, &l)) { if (!n) return 0; *out = record_text(p, n); return 1; }
        *out = string_ref(buf, l); return 1; }
    if (d->what == D_REFER && depth < 16) {                                  /* the first of its targets that holds the key */
        for (uint8_t i = 0; i < d->ntargets; i++) {
            const TargetCode *target = &d->targets[i];
            /* Published keys obey source-order rank, including duplicate
             * declarations outside this record. Local bindings also bootstrap
             * products while their declarations are still being evaluated. */
            const KeyField *declaration = keys_field_get(&target->domain, x->val, x->vlen);
            if (declaration && declaration->has_value) { *out = declaration->value; return 1; }
            if (declaration && declaration->blocked) {
                k->blocked++; if (!k->binding_phase) record_unknown_key(k, t, c, x); return 0;
            }
            if (target->local) {
                int32_t g = keyed(k, t, target, x->val, x->vlen);
                if (g >= 0) {
                    if (target->within) { for (int32_t up = t->n[g].parent; up >= 0; up = t->n[up].parent) if (record_thing(k, t, up, out, depth + 1)) return 1; }
                    else if (record_thing(k, t, g, out, depth + 1)) return 1;
                }
            }
        }
        if (k->binding_phase) { k->blocked++; return 0; }
        record_unknown_key(k, t, c, x);
        return 0; }
    return 0;
}

int record_subject(RecordWorker *k, const SemanticRecord *t, int32_t g, Ref *out){
    for (int32_t up = g; up >= 0; up = t->n[up].parent) { if (record_thing(k, t, up, out, 0)) return 1;
        for (int i = 0; i < k->s->nthing; i++) if (record_field_matches(&t->n[up], k->s->thing[i].tier)) return 0; }     /* a kind the recipe names a thing of, naming none here (a synset with no members), speaks of nothing: never of what holds it */
    return 0;
}

int record_thing(RecordWorker *k, const SemanticRecord *t, int32_t g, Ref *out, int depth){
    int memo = k->tc_tree == t && (uint32_t)g < k->tc_n;                     /* each thing is composed once in a tree; the threads reading one tree share what is composed */
    if (memo) { uint8_t st = __atomic_load_n(&k->ts[g], __ATOMIC_ACQUIRE); if (st >= 3) { *out = k->tc[g]; return st - 2; } if (st == 1 || st == 2) { if (st == 1) k->blocked++; return 0; } }     /* 1: awaits a declaration; 2: unnamed; 3 and up: named, by the way st - 3 */
    if (memo && composing(g)) return 0;                                            /* being composed by this thread: a thing named by itself names nothing */
    int pushed = memo && nbusy < BUSY_MAX; if (pushed) busy[nbusy++] = g;
    uint64_t blocked = k->blocked;
    int r = thing_compose(k, t, g, out, depth);
    if (pushed) nbusy--;
    if (memo) { if (r) k->tc[g] = *out; __atomic_store_n(&k->ts[g], (uint8_t)(r ? (r < 250 ? r + 2 : 252) : blocked != k->blocked ? 1 : 2), __ATOMIC_RELEASE); }
    return r;
}

static KeyField projected_field(RecordWorker *k, const SemanticRecord *t, int32_t c, int depth){
    KeyField field = {0}; if (c < 0) return field;
    const SemanticValue *text = &t->n[c];
    if (text->kind == S_GROUP && record_field_first(text) >= 0 && record_field_next(&t->n[record_field_first(text)]) < 0 &&
        t->n[record_field_first(text)].kind == S_TEXT) text = &t->n[record_field_first(text)];
    if (text->kind != S_GROUP) { field.text = text->val; field.length = text->vlen; field.has_text = 1; }
    UnkMark mark; record_unknown_save(k, t, c, &mark);
    uint64_t blocked = k->blocked;
    field.has_value = (uint8_t)record_value(k, t, c, &field.value, depth);
    field.blocked = (uint8_t)(blocked != k->blocked);
    if (field.blocked) field.has_value = 0;
    record_unknown_restore(k, t, &mark); return field;
}

static int referred_field(RecordWorker *k, const SemanticRecord *t, int32_t g, const char *name, KeyField *out, int depth){
    const ReferenceProjection *projection = record_selector_code(name)->projection;
    if (!projection) return 0;
    const PathStep *first = projection->reference;
    int32_t c = record_path_first(t, g, first, first->next); if (c < 0) return 0;
    const Dis *d = record_field_disposition(k->s, &t->n[c]); const SemanticValue *x = &t->n[c];
    if (x->kind == S_GROUP && record_field_first(x) >= 0 && record_field_next(&t->n[record_field_first(x)]) < 0 && t->n[record_field_first(x)].kind == S_TEXT) x = &t->n[record_field_first(x)];     /* a part that holds only its text */
    if (!d || d->what != D_REFER || !x->vlen) return 0;
    for (uint8_t i = 0; i < d->ntargets; i++) {
        const TargetCode *target = &d->targets[i];
        if (target->within || !target->local) continue;
        for (const KeyProjection *p = projection->targets; p; p = p->next) if (p->tier == target->tier_id) {
            const KeyField *field = keys_field_get(&p->domain, x->val, x->vlen);
            if (field) { *out = *field; if (field->blocked) k->blocked++; return 1; }
            int32_t r = keyed(k, t, target, x->val, x->vlen); if (r < 0) break;
            *out = projected_field(k, t, record_path_first(t, r, p->field, NULL), depth); return 1;
        }
    }
    if (k->binding_phase) k->blocked++;
    return 0;
}

static Ref thing_scope(RecordWorker *k,const SemanticRecord *t,int32_t g,const Thing *th,Ref value,int depth){
    if (th->within && depth<8) {
        Ref parent; int32_t up=t->n[g].parent;
        while (up>=0 && !record_thing(k,t,up,&parent,depth+1)) up=t->n[up].parent;
        if (up>=0) { Ref parts[]={parent,value}; for (unsigned i=0; i<2; i++) if (parts[i].said!=LP_SAID_TUPLE) parts[i].said=0;
            value=said_tuple(compose(parts,2,ref_above(parts,2))); }
    }
    if (th->infile) { Ref parts[]={k->fname,value}; for (unsigned i=0; i<2; i++) if (parts[i].said!=LP_SAID_TUPLE) parts[i].said=0;
        value=said_tuple(compose(parts,2,ref_above(parts,2))); }
    return value;
}

static int one_thing(RecordWorker *k, const SemanticRecord *t, int32_t g, const Thing *th, Ref *out, int depth){
    const SemanticValue *x = &t->n[g]; Ref p[8]; {
        if (record_selector_code(th->name[0])->special == SELECT_SELF) { if (!record_entity(k, t, g, out)) return 0; out->said = 0; return 1; }        /* the thing is its content: the part itself, whole */
        if (th->of) {                                                        /* the path of what its parts of those names are, in the file's order */
            size_t mark = scratch_mark();
            uint32_t max_k = x->arity ? x->arity : 64;
            Ref *pc = scratch_alloc(sizeof(Ref) * max_k); size_t np = 0;
            for (int32_t c = record_field_first(x); c >= 0; c = record_field_next(&t->n[c])) { int hit = 0; for (int z = 0; z < th->n && !hit; z++) hit = record_field_matches(&t->n[c], th->name[z]); if (!hit) continue;
                Ref v; if (!(t->n[c].kind == S_GROUP ? record_thing(k, t, c, &v, depth + 1) : record_value(k, t, c, &v, depth))) continue;
                if (np < max_k) { if (v.said!=LP_SAID_TUPLE) v.said=0; pc[np++] = v; } }
            if (!np) { scratch_restore(mark); return 0; } Ref value=np==1 ? pc[0] : said_tuple(compose(pc,(uint32_t)np,ref_above(pc,np)));
            *out=thing_scope(k,t,g,th,value,depth); scratch_restore(mark); return 1; }
        if (th->span) {                                                      /* the stretch of the text of TEXT, found in what it is inside, from the character START to END */
            int32_t a = record_select_first(t, g, th->name[0]), b = record_select_first(t, g, th->name[1]); if (a < 0 || b < 0 || !t->n[a].vlen || !t->n[b].vlen || t->n[a].vlen > 11 || t->n[b].vlen > 11) return 0;
            char za[16], zb[16]; memcpy(za, t->n[a].val, t->n[a].vlen); za[t->n[a].vlen] = 0; memcpy(zb, t->n[b].val, t->n[b].vlen); zb[t->n[b].vlen] = 0; char *e1, *e2; long lo = strtol(za, &e1, 10), hi = strtol(zb, &e2, 10);
            if (*e1 || *e2 || lo < 0 || hi < lo) return 0; if (th->inclusive) hi++;
            int32_t tx = -1; for (int32_t up = x->parent; up >= 0 && tx < 0; up = t->n[up].parent) tx = record_field_child(t, up, th->name[2], -1);
            if (tx < 0) return 0; const SemanticValue *tn = &t->n[tx]; if (tn->kind == S_GROUP) { if (record_field_first(tn) < 0) return 0; tn = &t->n[record_field_first(tn)]; }
            size_t i = 0, blo = 0, bhi = 0; long ch = 0; int have = 0;                 /* characters counted as the source counts them: code points */
            for (; i <= tn->vlen; ch++) { if (ch == lo) { blo = i; have = 1; } if (ch == hi) { bhi = i; break; } if (i == tn->vlen) return 0;
                uint8_t c = tn->val[i]; i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
            if (!have || bhi <= blo || bhi > tn->vlen) return 0;
            *out = record_text(tn->val + blo, bhi - blo); out->said = 0; return 1; }
        if (th->resolved) {                                                  /* the path of those of its parts that resolve, in the recipe's order */
            uint32_t np = 0;
            for (int z = 0; z < th->n; z++) { int ref = th->name[z][0] == '>';
                if (ref) {
                    KeyField field;
                    if (referred_field(k, t, g, th->name[z] + 1, &field, depth) && field.has_value) { p[np] = field.value; p[np++].said = 0; }
                    continue;
                }
                int32_t c = record_select_first(t, g, th->name[z]); if (c < 0) continue;
                const SemanticValue *cn = &t->n[c]; if (cn->kind == S_GROUP && record_field_first(cn) >= 0 && record_field_next(&t->n[record_field_first(cn)]) < 0 && t->n[record_field_first(cn)].kind == S_TEXT) cn = &t->n[record_field_first(cn)];
                if (record_value_empty(k->s, cn) || (cn->kind != S_GROUP && !cn->vlen)) continue;            /* what the source leaves empty: in no path, and creates no obligation */
                if (record_value(k, t, c, &p[np], depth)) { p[np].said = 0; np++; continue; }
                record_obligation_note(k, 'o', &record_diagnostic_labels[DIAG_UNRESOLVED_KEY], (const uint8_t *)th->name[z], record_selector_code(th->name[z])->source_length, NULL, 0); }     /* said, never dropped */
            if (!np) return 0;
            Ref X = np == 1 ? p[0] : said_tuple(compose(p, np, ref_above(p, np)));
            *out = thing_scope(k,t,g,th,X,depth); return 1; }
        for (int z = 0; z < th->n; z++) {
            if (th->lit[z]) { p[z] = record_text((const uint8_t *)th->name[z], strlen(th->name[z])); p[z].said = 0; continue; }      /* a text the recipe writes */
            if (th->name[z][0] == '>') {
                KeyField field;
                if (!referred_field(k, t, g, th->name[z] + 1, &field, depth)) return 0;
                if (th->form[z].has_re || th->space[z]) {
                    if (!field.has_text || !field.length) return 0;
                    Keys form = {.n=0};
                    if (th->form[z].has_re) { record_key_forms(&th->form[z], field.text, field.length, &form, t, g); if (!form.n) return 0; }
                    else { if (field.length >= sizeof form.k[0]) return 0; memcpy(form.k[0], field.text, field.length); form.k[0][field.length] = 0; }
                    char *text = form.k[0]; if (th->space[z]) for (char *q = text; *q; q++) if (*q == th->space[z]) *q = ' ';
                    p[z] = record_text((const uint8_t *)text, strlen(text));
                } else { if (!field.has_value) return 0; p[z] = field.value; }
                p[z].said = 0; continue;
            }
            int32_t c = record_select_first(t, g, th->name[z]); if (c < 0) return 0;
            if (th->form[z].has_re || th->space[z]) {                        /* the part's text as the source's notation parts it: what it writes, never what it is disposed as */
                const SemanticValue *cn = &t->n[c]; if (cn->kind == S_GROUP && record_field_first(cn) >= 0 && record_field_next(&t->n[record_field_first(cn)]) < 0 && t->n[record_field_first(cn)].kind == S_TEXT) cn = &t->n[record_field_first(cn)];
                if (cn->kind == S_GROUP || record_value_empty(k->s, cn)) return 0;
                Keys kf; kf.n = 0; if (th->form[z].has_re) { record_key_forms(&th->form[z], cn->val, cn->vlen, &kf, t, c); if (!kf.n) return 0; } else { if (cn->vlen >= sizeof kf.k[0]) return 0; memcpy(kf.k[0], cn->val, cn->vlen); kf.k[0][cn->vlen] = 0; }
                char *w = kf.k[0]; if (th->space[z]) for (char *q = w; *q; q++) if (*q == th->space[z]) *q = ' ';
                p[z] = record_text((const uint8_t *)w, strlen(w)); p[z].said = 0; continue; }
            if (t->n[c].kind == S_GROUP && record_field_first(&t->n[c]) >= 0 && record_field_next(&t->n[record_field_first(&t->n[c])]) >= 0) {      /* a part that is several pieces: the path of what each is, in order */
                Ref pc[256]; uint32_t np = 0; for (int32_t q = record_field_first(&t->n[c]); q >= 0 && np < 256; q = record_field_next(&t->n[q])) if (record_value(k, t, q, &pc[np], depth)) { pc[np].said = 0; np++; }
                if (!np) return 0; p[z] = np == 1 ? pc[0] : compose(pc, np, ref_above(pc, np)); }
            else if (!record_value(k, t, c, &p[z], depth)) {
            const Dis *cd = record_field_disposition(k->s, &t->n[c]);
            if (cd && cd->what == D_KEY && !record_value_empty(k->s, &t->n[c])) {
                const SemanticValue *cn = &t->n[c];
                if (cn->kind == S_GROUP && record_field_first(cn) >= 0 && record_field_next(&t->n[record_field_first(cn)]) < 0 && t->n[record_field_first(cn)].kind == S_TEXT) cn = &t->n[record_field_first(cn)];
                if (cn->kind == S_TEXT && cn->vlen) p[z] = record_text(cn->val, cn->vlen); else return 0;
            } else return 0;
        }
            p[z].said = 0; }
        Ref X = th->n == 1 ? p[0] : said_tuple(compose(p, (uint32_t)th->n, ref_above(p, (size_t)th->n)));      /* named by several parts together: the path of them */
        *out = thing_scope(k,t,g,th,X,depth); return 1; }
}

static int thing_compose(RecordWorker *k, const SemanticRecord *t, int32_t g, Ref *out, int depth){
    const SemanticValue *x = &t->n[g]; const Say *s = k->s;
    for (int i = 0; i < s->nthing; i++) if (record_field_matches(x, s->thing[i].tier)) {
        uint64_t blocked = k->blocked;
        int named = one_thing(k, t, g, &s->thing[i], out, depth);
        if (blocked != k->blocked) return 0;
        if (named) return i + 1;
    }
    return 0;
}

int record_names_identity(RecordWorker *k, const SemanticRecord *t, int32_t g, const SemanticValue *y){
    Ref X; int r = record_thing(k, t, g, &X, 0); if (!r) return 0; const Thing *th = &k->s->thing[r - 1];
    for (int z = 0; z < th->n; z++) if (!th->lit[z] && !strchr(th->name[z], '/') && record_field_named(y, th->name[z], strlen(th->name[z]))) return 1;
    return 0;
}

int record_entity(RecordWorker *k, const SemanticRecord *t, int32_t c, Ref *out){
    const SemanticValue *x = &t->n[c]; const Say *s = k->s; const SemanticValue *up = x->parent >= 0 ? &t->n[x->parent] : NULL;
    if (x->kind != S_GROUP) {
        if (record_value_empty(s, x)) return 0;
        if (up && up->kind == S_GROUP && up->tier == x->tier && up->nlen) {       /* a part of a part */
            const Dis *dp = x->nlen ? record_field_disposition(s, x) : NULL; if (dp && (dp->what == D_OMIT || dp->what == D_METADATA)) return 0;
            if (x->kind == S_VALUE && x->nlen) { Ref p[2] = { string_ref(x->name, x->nlen), string_ref(x->val, x->vlen) };      /* a KEY IS VALUE part: the pair of its key and what its value is */
                if (dp && dp->what == D_KEY) return 0;                                  /* the file's own numbering: in no record */
                if (dp && !record_value(k, t, c, &p[1], 0)) return 0;                     /* a reference: the thing it names */
                if (p[1].said != LP_SAID_TUPLE) p[1].said = 0; *out = said_tuple(compose(p, 2, ref_above(p, 2))); return 1; }
            if (x->nlen && record_value(k, t, c, out, 0)) return 1;                 /* a piece read as its part is (a type, a code point) */
            *out = string_ref(x->val, x->vlen); return 1; }
        const Dis *d = x->nlen ? record_field_disposition(s, x) : NULL;
        if (!d) { if (x->nlen && x->kind != S_TEXT) { record_note_unhandled(k, x); return 0; } *out = semantic_text_ref(k, x); return 1; }     /* a text is the text it is */
        if (d->what == D_OMIT || d->what == D_METADATA) return 0;
        if (d->what == D_KEY) return 0;                                      /* the file's own numbering: in no record */
        return record_value(k, t, c, out, 0);                                    /* a reference: the thing it names, or nothing */
    }
    const Dis *d = x->nlen ? record_field_disposition(s, x) : NULL; if (d && (d->what == D_OMIT || d->what == D_METADATA)) return 0;
    record_note_dropped(k, t, (uint32_t)c, 0);
    size_t mark = scratch_mark();
    Ref stack[64], *kid = x->arity <= 64 ? stack : scratch_alloc(sizeof(Ref) * x->arity); uint32_t n = 0;
    for (int32_t q = record_field_first(x); q >= 0; q = record_field_next(&t->n[q])) { Ref v; if (record_entity(k, t, q, &v)) { if (v.said != LP_SAID_TUPLE) v.said = 0; kid[n++] = v; } }
    if (n == 1) *out = kid[0]; else if (n > 1) *out = compose(kid, n, ref_above(kid, n));
    scratch_restore(mark);
    return n > 0;
}

static void record_key_forms(const KSpec *spec, const uint8_t *value, size_t length, Keys *out, const SemanticRecord *tree, int32_t at){
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
                if (parent >= 0) for (int32_t child = record_field_first(&tree->n[parent]); child >= 0; child = record_field_next(&tree->n[child]))
                    if (record_name_matches(&tree->n[child], &instruction->text)) { sibling = child; break; }
                if (sibling < 0 || !tree->n[sibling].vlen) { valid = 0; break; }
                part = tree->n[sibling].val; n = tree->n[sibling].vlen;
            }
            if (n >= 160 - used) { valid = 0; break; }
            if (n) memcpy(result + used, part, n); used += n;
        }
        result[used] = 0; if (valid && used) out->n++;
    }
}

static void keys_at(const Say *s, const SemanticRecord *t, int32_t g, const KSpec *ks, Keys *out){
    out->n = 0; int32_t at[RECORD_REFERENCE_CAPACITY]; int na = record_select_all(t, g, ks->path, at, RECORD_REFERENCE_CAPACITY);
    for (int i = 0; i < na; i++) { const SemanticValue *x = &t->n[at[i]];
        if (x->kind != S_GROUP) { if (!record_value_empty(s, x)) record_key_forms(ks, x->val, x->vlen, out, t, at[i]); continue; }
        for (int32_t q = record_field_first(x); q >= 0; q = record_field_next(&t->n[q])) if (t->n[q].kind != S_GROUP && !record_value_empty(s, &t->n[q])) record_key_forms(ks, t->n[q].val, t->n[q].vlen, out, t, q); }
}

void record_publish_keys(RecordWorker *k, const SemanticRecord *t, int32_t root, int reserve){
    const Say *s = k->s; if (!s->nkey) return; int32_t end = record_scope_end(t, root);
    /* Establish declaration rank before evaluating any identity. An earlier
     * unresolved declaration reserves its key instead of allowing a later
     * duplicate's already available product to impersonate it. */
    KeyField waiting = {.blocked=1};
    for (int32_t g = root; g <= end; g++) {
        const SemanticValue *x = &t->n[g]; if (x->context || x->kind != S_GROUP || !x->nlen) continue;
        for (int i = 0; i < s->nkey; i++) if (record_field_named(x, s->key[i].tier, s->key[i].tier_length)) {
            int32_t field = record_field_child(t, g, s->key[i].name, -1); if (field < 0 || !t->n[field].vlen) continue;
            const Pair *key = &s->key[i]; KeyRank rank = k->kr; rank.member = (uint64_t)g;
            keys_field_put(&s->key_domain, t->n[field].val, t->n[field].vlen, &waiting, &rank);
            keys_field_put(&key->domain, t->n[field].val, t->n[field].vlen, &waiting, &rank);
            keys_field_put(&key->parent_domain, t->n[field].val, t->n[field].vlen, &waiting, &rank);
            if (reserve) for (const KeyProjection *projection = s->projections; projection; projection = projection->all_next)
                if (projection->tier == key->tier_id) keys_field_put(&projection->domain, t->n[field].val, t->n[field].vlen, &waiting, &rank);
        }
    }
    if (reserve) return;
    for (int32_t g = root; g <= end; g++) { const SemanticValue *x = &t->n[g]; if (x->context || x->kind != S_GROUP || !x->nlen) continue;
        for (int i = 0; i < s->nkey; i++) if (record_field_named(x, s->key[i].tier, s->key[i].tier_length)) { int32_t kc = record_field_child(t, g, s->key[i].name, -1); Ref S;
            if (kc >= 0 && t->n[kc].vlen) {
                const Pair *key = &s->key[i];
                KeyRank rank = k->kr; rank.member = (uint64_t)g;
                for (const KeyProjection *projection = s->projections; projection; projection = projection->all_next) if (projection->tier == key->tier_id) {
                    KeyField field = projected_field(k, t, record_path_first(t, g, projection->field, NULL), 0);
                    keys_field_put(&projection->domain, t->n[kc].val, t->n[kc].vlen, &field, &rank);
                }
                uint64_t blocked = k->blocked; KeyField declaration = {0};
                if (record_thing(k, t, g, &S, 0)) { declaration.value = S; declaration.has_value = 1; }
                declaration.blocked = (uint8_t)(blocked != k->blocked);
                keys_field_put(&s->key_domain, t->n[kc].val, t->n[kc].vlen, &declaration, &rank);
                keys_field_put(&key->domain, t->n[kc].val, t->n[kc].vlen, &declaration, &rank);
                blocked = k->blocked; declaration = (KeyField){0};
                for (int32_t up = x->parent; up >= 0; up = t->n[up].parent) if (record_thing(k, t, up, &S, 0)) {
                    declaration.value = S; declaration.has_value = 1; break;
                }
                declaration.blocked = (uint8_t)(blocked != k->blocked);
                keys_field_put(&key->parent_domain, t->n[kc].val, t->n[kc].vlen, &declaration, &rank);
            } } }
}

void record_highway_values(RecordWorker *k, const SemanticRecord *t, int32_t root){
    const Say *s = k->s; if (!record_conditions_match(s, t, root, s->whole)) return;
    for (uint32_t g = 0; g < t->count; g++) { const SemanticValue *x = &t->n[g]; if (x->context || x->kind == S_NOTE || !x->nlen) continue;
        { const Dis *dg = record_field_disposition(s, x); if (dg && dg->what == D_OMIT) { g = (uint32_t)record_scope_end(t, (int32_t)g); continue; } }
        for (int i = 0; i < s->nhw; i++) { const HwLine *h = &s->hw[i]; if (!record_field_matches(x, h->tier) || !record_conditions_match(s, t, (int32_t)g, h->when)) continue;
            Ref X; Keys A, B;
            if (h->what <= 2) { if (x->kind == S_VALUE || !(x->kind == S_GROUP ? record_thing(k, t, (int32_t)g, &X, 0) : record_value(k, t, (int32_t)g, &X, 0))) continue;     /* a thing; or a text of a list, a type by itself */
                if (h->what == 1) { record_highway_entry(k, 1, i, &X, NULL, 0, NULL, 0); continue; }
                keys_at(s, t, (int32_t)g, &h->a, &A); for (int a = 0; a < A.n; a++) record_highway_entry(k, 2, i, &X, A.k[a], strlen(A.k[a]), NULL, 0); continue; }
            keys_at(s, t, (int32_t)g, &h->a, &A); if (!A.n) continue; keys_at(s, t, (int32_t)g, &h->b, &B);
            for (int a = 0; a < A.n; a++) for (int b = 0; b < B.n; b++) record_highway_entry(k, h->what, i, NULL, A.k[a], strlen(A.k[a]), B.k[b], strlen(B.k[b])); } }
}

RecordValueFrame record_values_enter(void){
    RecordValueFrame previous={busy_base,nbusy,self_cp,self_mark};
    busy_base=nbusy;
    return previous;
}
void record_values_leave(const RecordValueFrame *previous){
    busy_base=previous->base; nbusy=previous->depth;
    self_cp=previous->codepoint; self_mark=previous->marker;
}
