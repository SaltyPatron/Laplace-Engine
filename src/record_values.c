/* Reduce compiled value rules to Native references, with scoped memoization.
 * Source scheduling, SQL, statement emission and provenance layout are separate. */
#define _GNU_SOURCE
#include "record_values_internal.h"
#include "laplace/highway.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread long self_cp = -1; static __thread char self_mark;
__thread int32_t lp_record_busy[BUSY_MAX];
__thread int lp_record_nbusy, lp_record_busy_base;     /* the things this thread is composing, innermost last; from busy_base, the reading it is in now */

static int codepoints_text(const uint8_t *p, size_t n, uint8_t *out, size_t cap, size_t *len){
    size_t k = 0; const uint8_t *e = p + n;
    while (p < e) { while (p < e && *p == ' ') p++; if (p >= e) break; const uint8_t *t0 = p; while (p < e && *p != ' ') p++;
        size_t o = (size_t)(p - t0);
        if (o == 1 && self_mark && *t0 == (uint8_t)self_mark && self_cp >= 0) { if (k + 4 > cap) return 0; k += lp_utf8_put((uint32_t)self_cp, out + k); continue; } if (o > 2 && t0[0] == 'U' && t0[1] == '+') { t0 += 2; o -= 2; } char h[16]; if (!o || o > 8) return 0; memcpy(h, t0, o); h[o] = 0;
        char *end; unsigned long cp = strtoul(h, &end, 16); if (*end || cp >= LP_NCP || k + 4 > cap) return 0; k += lp_utf8_put((uint32_t)cp, out + k); }
    *len = k; return k > 0;
}

static int range_endpoint(const uint8_t *p, size_t n, Ref *out){
    while (n && *p==' ') { p++; n--; }
    while (n && p[n-1]==' ') n--;
    uint8_t bytes[8]; size_t length;
    if (!codepoints_text(p,n,bytes,sizeof bytes,&length)) return 0;
    *out=string_ref(bytes,length); return 1;
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

/* Numeric pointers compare by value, regardless of hexadecimal letter case
 * or zero padding. The explicit recipe radix leaves ordinary keys literal. */
static int numeric_key(const uint8_t *bytes,size_t length,unsigned radix,uint8_t out[8]){
    if (!length) return 0; uint64_t value=0;
    for (size_t i=0;i<length;i++) {
        unsigned digit=bytes[i]>='0' && bytes[i]<='9' ? bytes[i]-'0' :
            bytes[i]>='a' && bytes[i]<='z' ? bytes[i]-'a'+10 :
            bytes[i]>='A' && bytes[i]<='Z' ? bytes[i]-'A'+10 : radix;
        if (digit>=radix || value>(UINT64_MAX-digit)/radix) return 0;
        value=value*radix+digit;
    }
    for (unsigned i=0;i<8;i++) out[i]=(uint8_t)(value>>(8*i));
    return 1;
}
static uint64_t local_key_hash(uint16_t tier, int32_t scope, const uint8_t *bytes, size_t length){
    return selector_hash(bytes, length) ^ ((uint64_t)tier * UINT64_C(0x9e3779b97f4a7c15)) ^
        ((uint64_t)((uint32_t)scope+1) * UINT64_C(0xbf58476d1ce4e5b9));
}
static int32_t key_scope(const RecordWorker *worker, const SemanticRecord *record, int32_t node, const Pair *key){
    if (!key->scope[0]) return -1;
    for (int32_t up=record->n[node].parent; up>=0; up=record->n[up].parent)
        if (record_field_matches(&record->n[up],key->scope)) return up;
    fprintf(stderr,"%s: key tier %s has no enclosing scope %s\n",worker->r->name,key->tier,key->scope); exit(2);
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
            const Pair *key = &say->key[i]; if (key->under[0] || !record_field_named(node, key->tier, key->tier_length)) continue;
            int32_t field = record_select_first(tree, (int32_t)group, key->name);
            if (field < 0 || !tree->n[field].vlen) continue;
            const SemanticValue *value = &tree->n[field];
            const uint8_t *bytes=value->val; size_t length=value->vlen;
            if (key->key_radix) {
                uint8_t number[8]; if (!numeric_key(bytes,length,key->key_radix,number)) continue;
                uint8_t *copy=scratch_alloc(sizeof number); memcpy(copy,number,sizeof number); bytes=copy; length=sizeof number;
            }
            if ((sink->local_count + 1) * 2 >= sink->local_capacity) local_key_grow(sink);
            int32_t scope=key_scope(sink,tree,(int32_t)group,key);
            uint64_t hash = local_key_hash(key->tier_id, scope, bytes, length);
            size_t at = (hash ^ (hash >> 32)) & (sink->local_capacity - 1);
            while (sink->local_keys[at].tier) {
                const LocalKey *entry = &sink->local_keys[at];
                if (entry->hash == hash && entry->tier == key->tier_id && entry->scope == scope && entry->length == length &&
                    !memcmp(entry->bytes, bytes, length)) break;
                at = (at + 1) & (sink->local_capacity - 1);
            }
            if (!sink->local_keys[at].tier) {
                sink->local_keys[at] = (LocalKey){.bytes=bytes,.hash=hash,.length=(uint32_t)length,.node=(int32_t)group,.scope=scope,.tier=key->tier_id}; sink->local_count++;
            }
        }
    }
}

int keyed(RecordWorker *sink, const SemanticRecord *tree, const TargetCode *target, int32_t from, const uint8_t *bytes, size_t length){
    if (sink->ix_tree != tree || sink->ix_count != tree->count) { fputs("record key index was not prepared before reduction\n", stderr); abort(); }
    if (!sink->local_capacity || !target->tier_id || length > UINT32_MAX) return -1;
    int32_t scope=target->scope_key ? key_scope(sink,tree,from,&sink->s->key[target->scope_key-1]) : -1;
    uint64_t hash = local_key_hash(target->tier_id, scope, bytes, length);
    size_t at = (hash ^ (hash >> 32)) & (sink->local_capacity - 1);
    while (sink->local_keys[at].tier) {
        const LocalKey *entry = &sink->local_keys[at];
        if (entry->hash == hash && entry->tier == target->tier_id && entry->scope == scope && entry->length == length &&
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

/* A publisher may export relations over a wider domain than the licensed or
 * approved records it exports. Such recipes explicitly restrict admission.
 * Missing providers defer until source closure; a broken declaration remains
 * an error, and ordinary references retain their strict unresolved behavior. */
int record_admit_references(RecordWorker *k, const SemanticRecord *t, int32_t root, int count){
    const Say *s=k->s; int missing=0, broken=0;
    for (int i=0;i<s->nadmission;i++) {
        int32_t at=record_select_first(t,root,s->admission[i].field);
        if (at<0) { record_note_open_name(k,(const uint8_t *)s->admission[i].field,(uint32_t)strlen(s->admission[i].field),1);broken=1;continue; }
        const SemanticValue *value=&t->n[at]; const Dis *reference=s->admission[i].reference;
        if (!value->vlen || value->kind==S_GROUP || record_value_empty(s,value)) { record_unknown_key(k,t,at,value);broken=1;continue; }
        int present=0, blocked=0;
        for (int j=0;j<reference->ntargets;j++) {
            const KeyField *key=keys_field_get(&reference->targets[j].domain,value->val,value->vlen);
            if (key && key->has_value) { present=1;break; }
            if (key && key->blocked) { blocked=1;break; }
        }
        if (present) continue;
        if (blocked) { k->blocked++;if (!k->binding_phase) record_unknown_key(k,t,at,value);broken=1; }
        else missing=1;
    }
    if (broken) return 0;
    if (!missing) return 1;
    if (k->binding_phase) k->blocked++;
    else if (count) k->excluded++;
    return 0;
}

Ref record_text(const uint8_t *p, size_t n){ return n > 256 ? text_ref(CTX[omp_get_thread_num()], p, n) : string_ref(p, n); }

/* A positional pointer is local to its canonical parent. These bytes are an
 * index key only; they never mint an entity or survive as corpus content. */
const uint8_t *scoped_key(RecordWorker *k, const SemanticRecord *t, int32_t at,
                          const char *under, unsigned radix, const uint8_t *raw, size_t *length, int depth){
    if (!under[0] && !radix) return raw;
    uint8_t number[8];
    if (radix) { if (!numeric_key(raw,*length,radix,number)) return NULL; raw=number; *length=sizeof number; }
    if (!under[0]) { uint8_t *bytes=scratch_alloc(*length); memcpy(bytes,raw,*length); return bytes; }
    int32_t parent=record_select_first(t,at,under); Ref scope;
    if (parent<0 || !record_value(k,t,parent,&scope,depth+1)) return NULL;
    if (*length>SIZE_MAX-sizeof scope.id.b) { fputs("scoped pointer exceeds arena\n",stderr); abort(); }
    uint8_t *bytes=scratch_alloc(sizeof scope.id.b+*length);
    memcpy(bytes,scope.id.b,sizeof scope.id.b); memcpy(bytes+sizeof scope.id.b,raw,*length);
    *length+=sizeof scope.id.b; return bytes;
}

static size_t hex_upper(unsigned long cp, char *out){
    static const char hd[] = "0123456789ABCDEF";
    char tmp[16]; int k = 0;
    do { tmp[k++] = hd[cp & 15]; cp >>= 4; } while (cp > 0);
    while (k < 4) tmp[k++] = '0';
    for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    return (size_t)k;
}

/* Both field values and compound identities compose the complete child set.
 * The packaging event owner records its arity; scratch storage lives only
 * until Native composition has copied those operands into the node table. */
int compose_values(RecordWorker *k,const SemanticRecord *t,int32_t c,int depth,int flags,Ref *out){
    const SemanticValue *x=&t->n[c];size_t capacity=x->arity;
    if(capacity>SIZE_MAX/sizeof(Ref)){fputs("record values exceed addressable memory\n",stderr);abort();}
    size_t mark=scratch_mark();Ref *parts=scratch_alloc((capacity?capacity:1)*sizeof *parts);uint32_t count=0;
    for(int32_t child=record_field_first(x);child>=0;child=record_field_next(&t->n[child])){
        if((flags&VALUE_PARTS_FLAT) && t->n[child].kind==S_GROUP){scratch_restore(mark);return 0;}
        Ref value;if(!record_value(k,t,child,&value,depth))continue;
        if(count>=capacity){fputs("record child count exceeds its declared arity\n",stderr);abort();}
        value.said=0;parts[count++]=value;
    }
    if(count){
        *out=count==1?parts[0]:compose(parts,count,ref_above(parts,count));
        if(count>1 && (flags&VALUE_PARTS_TUPLE))*out=said_tuple(*out);
    }
    scratch_restore(mark);return count!=0;
}

/* A tagset's spelling, by way of the highway: the type of the tagset its key names, then that type's one edge to the
 * target list (manifest/slots: the tagset's list and its edges). A type with no edge there is the tagset's own category,
 * equivalent to nothing, and is kept as written; a type with several names no one type, and is kept as written too. */
static int crosswalk_value(RecordWorker *k, const SemanticRecord *t, int32_t c, const SemanticValue *x, const Dis *d,
    const uint8_t *kp, size_t kn, Ref *out){
    const lp_list *tagset = HW ? lp_highway_list(HW, d->k.crosswalk) : NULL, *target = HW && d->ntargets == 1 ? lp_highway_list(HW, d->targets[0].name) : NULL;
    if (!tagset || !target || !kn || kn > 1000) { record_unknown_key(k, t, c, x); return 0; }
    char key[1001]; memcpy(key, kp, kn); key[kn] = 0;
    int64_t slot = lp_highway_key(HW, tagset, key);
    if (slot < 0) { record_unknown_key(k, t, c, x); return 0; }
    const lp_edge *edge; size_t n = lp_highway_edges(HW, tagset->name, (uint32_t)slot, target->name, &edge);
    const lp_tier0_record *record = n == 1 ? lp_highway_at(HW, target, edge[0].to) : NULL;
    if (!record) { *out = record_text(kp, kn); return 1; }
    /* The type's own text, composed here as a vocabulary's canonical spelling is, so the claim names content. */
    const char *const *parts; const char *name = lp_highway_identifier_parts(HW, target, edge[0].to, &parts) == 1 ? parts[0] : NULL;
    if (name) { Ref text = string_ref((const uint8_t *)name, strlen(name)); if (lp_id_eq(&text.id, &record->id)) { *out = text; return 1; } }
    memset(out, 0, sizeof *out); out->id = record->id; memcpy(out->c.m, record->m, sizeof out->c.m); out->tier = (uint8_t)record->pad;
    return 1;
}
static int type_value(RecordWorker *k, const SemanticRecord *t, int32_t c, const SemanticValue *x, const Dis *d, Ref *out){
        const uint8_t *kp = x->val; size_t kn = x->vlen;
        for (int more = 1; more; ) { more = 0;                                /* white space around it, a no-break space (U+00A0) among it */
            if (kn && (*kp == ' ' || *kp == '\t')) { kp++; kn--; more = 1; } else if (kn >= 2 && kp[0] == 0xC2 && kp[1] == 0xA0) { kp += 2; kn -= 2; more = 1; }
            if (kn && (kp[kn - 1] == ' ' || kp[kn - 1] == '\t' || kp[kn - 1] == '\r')) { kn--; more = 1; } else if (kn >= 2 && kp[kn - 2] == 0xC2 && kp[kn - 1] == 0xA0) { kn -= 2; more = 1; } }
        Keys kf; kf.n = 0; if (d->k.has_re || d->k.nas) { record_key_forms(&d->k, kp, kn, &kf, t, c); if (!kf.n) { record_unknown_key(k,t,c,x); return 0; } }
        if (d->k.crosswalk[0]) return crosswalk_value(k, t, c, x, d, kp, kn, out);
        if (d->k.vocabulary) {
            const VocabularyValue *value=lp_strmap_lookup(d->k.vocabulary,kp,kn);
            if (!value) { record_unknown_key(k,t,c,x); return 0; }
            if (!value->length) { *out=record_text(kp,kn); return 1; }
            kp=(const uint8_t *)value->canonical; kn=value->length;
        }
        const uint8_t *open_key=kf.n ? (const uint8_t *)kf.k[0] : kp;
        size_t open_length=kf.n ? strlen(kf.k[0]) : kn;
        int nk = kf.n ? kf.n : 1;                                              /* with no template, the key as written, once */
        for (int j = 0; j < nk; j++) { if (kf.n) { kp = (const uint8_t *)kf.k[j]; kn = strlen(kf.k[j]); } Ref v = string_ref(kp, kn);     /* each template's key in turn: the first a list knows */
            for (uint8_t i = 0; i < d->ntargets; i++) {
                const TargetCode *target = &d->targets[i];
                int has = 0; *out = highway_typed(target->name, kp, kn, &has); if (has>0) return 1;
                if (has<0) {
                    if (record_value_once(k,t,c)) record_obligation_note(k,'o',&record_diagnostic_labels[DIAG_AMBIGUOUS_KEY],x->name,x->nlen,kp,kn);
                    return 0;
                }
                const lp_list *L = HW ? lp_highway_list(HW, target->name) : NULL; if (L && lp_highway_slot(HW, L, &v.id) >= 0) { *out = v; return 1; }
            } }
        int open=d->k.node;
        if (!d->k.vocabulary) for (uint8_t i=0; i<d->ntargets; i++)
            if (HW && lp_highway_list(HW,d->targets[i].name) && !lp_highway_bank(HW,d->targets[i].name)) open=1;
        if (open) { if (record_value_once(k, t, c)) record_obligation_note(k, 'm', d->road_label, x->name, x->nlen, NULL, 0); *out = record_text(open_key, open_length); return 1; }
        record_unknown_key(k, t, c, x);     /* a key no list of the highway holds: said, never taken for text */
        return 0;
}

int record_value(RecordWorker *k, const SemanticRecord *t, int32_t c, Ref *out, int depth){
    const SemanticValue *x = &t->n[c]; const Say *s = k->s; const Dis *d = record_field_disposition(s, x);
    int typed_payload=x->kind==S_GROUP && x->scalar && d &&
        (d->what==D_CONTENT || d->what==D_TYPE || d->what==D_REFER || d->what==D_CODEPOINTS || d->what==D_RANGE);
    SemanticValue payload;
    /* An injected list may have a recipe-defined identity made from its
     * structured members. Resolve that identity before the flat scalar-list
     * path, which deliberately refuses group members. */
    if (x->kind == S_GROUP && depth < 16 && k->tc_tree == t && (uint32_t)c < k->tc_n && !composing(c)) {
        Ref value; if (record_thing(k,t,c,&value,depth+1)) { *out=value; return 1; }
    }
    if (!typed_payload && x->kind == S_GROUP && x->join == S_PIECES && record_field_first(x) >= 0 && record_field_next(&t->n[record_field_first(x)]) >= 0) {      /* a list of values: the tuple of what each is ([display text, target]) */
        return compose_values(k,t,c,depth,VALUE_PARTS_FLAT|VALUE_PARTS_TUPLE,out); }
    if (x->kind == S_GROUP && depth < 16) {                                    /* a part that is a thing: that thing */
        int32_t only = record_field_first(x); if (!typed_payload && only >= 0 && record_field_next(&t->n[only]) < 0 && t->n[only].kind == S_GROUP) return record_value(k, t, only, out, depth + 1); }     /* holding one part: what that part is */
    if (typed_payload) { payload=*x; payload.kind=S_TEXT; x=&payload; }
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
        *out = semantic_text_ref(k, typed_payload ? &t->n[c] : x); return 1; }
    if (d->what == D_TYPE) return type_value(k,t,c,x,d,out);
    if (d->what == D_CODEPOINTS || d->what == D_RANGE) {
        self_cp = k->selfcp; self_mark = s->selfmark;
        const uint8_t *p = x->val; size_t n = x->vlen; while (n && *p == ' ') { p++; n--; } while (n && p[n - 1] == ' ') n--;
        const uint8_t *dd = d->what == D_RANGE ? memmem(p, n, "..", 2) : NULL;
        if (dd) {                                                            /* a range, written as its first and its last: the path of the two */
            Ref two[2]; if (!range_endpoint(p, (size_t)(dd-p), &two[0]) || !range_endpoint(dd+2, n-(size_t)(dd-p)-2, &two[1])) { *out=record_text(p,n); return 1; }
            *out = said_tuple(compose(two, 2, ref_above(two, 2))); return 1; }
        uint8_t buf[512]; size_t l; if (!codepoints_text(p, n, buf, sizeof buf, &l)) { if (!n) return 0; *out = record_text(p, n); return 1; }
        *out = string_ref(buf, l); return 1; }
    if (d->what==D_INDEX && depth<16) {
        const uint8_t *digits=x->val; size_t length=x->vlen; Keys forms={0};
        if (d->k.has_re) {
            record_key_forms(&d->k,digits,length,&forms,t,c);
            if (forms.n!=1) goto invalid_index;
            digits=(const uint8_t *)forms.k[0]; length=strlen(forms.k[0]);
        }
        uint32_t position=0;
        if (!length) goto invalid_index;
        for (size_t i=0;i<length;i++) {
            if (digits[i]<'0' || digits[i]>'9' || position>(UINT32_MAX-(digits[i]-'0'))/10) goto invalid_index;
            position=position*10+(uint32_t)(digits[i]-'0');
        }
        if (position<d->index_base) goto invalid_index;
        position-=d->index_base;
        int32_t list=record_select_first(t,t->n[c].parent>=0?t->n[c].parent:0,d->arg);
        if (list<0 || t->n[list].kind!=S_GROUP || t->n[list].join!=S_PIECES) goto invalid_index;
        int32_t item=record_field_first(&t->n[list]);
        while (item>=0 && position) { item=record_field_next(&t->n[item]); position--; }
        if (item>=0 && item!=c && record_value(k,t,item,out,depth+1)) return 1;
invalid_index:
        if (k->binding_phase || k->defer_missing) k->blocked++;
        else record_unknown_key(k,t,c,x);
        return 0;
    }
    if (d->what == D_REFER && depth < 16) {                                  /* the first of its targets that holds the key */
        size_t key_length=x->vlen;
        const uint8_t *raw=x->val; Keys forms={0};
        if (d->k.has_re || d->k.nas) {
            record_key_forms(&d->k,raw,key_length,&forms,t,c);
            if (forms.n!=1) { if (k->binding_phase) k->blocked++; else record_unknown_key(k,t,c,x); return 0; }
            raw=(const uint8_t *)forms.k[0]; key_length=strlen(forms.k[0]);
        }
        size_t raw_length=key_length;
        const uint8_t *key=scoped_key(k,t,c,d->under,d->key_radix,raw,&key_length,depth);
        if (!key) { if (k->binding_phase) k->blocked++; else record_unknown_key(k,t,c,x); return 0; }
        for (uint8_t i = 0; i < d->ntargets; i++) {
            const TargetCode *target = &d->targets[i];
            /* Published keys obey source-order rank, including duplicate
             * declarations outside this record. Local bindings also bootstrap
             * products while their declarations are still being evaluated. */
            const KeyField *declaration = target->scope_key ? NULL : keys_field_get(&target->domain, key, key_length);
            if (declaration && declaration->has_value) { *out = declaration->value; return 1; }
            if (declaration && declaration->blocked) {
                k->blocked++; if (!k->binding_phase) record_unknown_key(k, t, c, x); return 0;
            }
            if (target->local && !d->under[0]) {
                int32_t g = keyed(k, t, target, c, key, key_length);
                if (g >= 0) {
                    if (target->within) { for (int32_t up = t->n[g].parent; up >= 0; up = t->n[up].parent) if (record_thing(k, t, up, out, depth + 1)) return 1; }
                    else if (record_thing(k, t, g, out, depth + 1)) return 1;
                }
            }
        }
        if (!k->binding_phase && d->ntargets==1) for (uint8_t i=0; i<d->ntargets; i++) {
            const TargetCode *target=&d->targets[i];
            Ref value;
            if (!target->constructor) continue;
            if (target->constructor_value->what==D_TYPE) {
                SemanticValue observed=*x; observed.val=raw; observed.vlen=(uint32_t)raw_length;
                if (!type_value(k,t,c,&observed,target->constructor_value,&value)) return 0;
            } else value=record_text(raw,raw_length);
            if (target->constructor==1) { *out=value; return 1; }
            if (target->constructor==2) {
                int32_t at=record_select_first(t,c,d->under); Ref scope;
                if (at<0 || !record_value(k,t,at,&scope,depth+1)) continue;
                Ref parts[2]={scope,value}; parts[0].said=parts[1].said=0;
                *out=said_tuple(compose(parts,2,ref_above(parts,2))); return 1;
            }
        }
        if (k->binding_phase) { k->blocked++; return 0; }
        record_unknown_key(k, t, c, x);
        return 0; }
    return 0;
}

KeyField projected_field(RecordWorker *k, const SemanticRecord *t, int32_t c, int depth){
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

int record_entity(RecordWorker *k, const SemanticRecord *t, int32_t c, Ref *out){
    const SemanticValue *x = &t->n[c]; const Say *s = k->s; const SemanticValue *up = x->parent >= 0 ? &t->n[x->parent] : NULL;
    if (x->kind == S_GROUP && x->capture) { *out = record_text(x->val, x->vlen); return 1; }
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
    /* A capture is the complete byte surface of this grammar-bounded group.
     * Its extracted semantic fields still produce their own claims, but are
     * not appended to the surface a second time when realizing the record. */
    for (int32_t q = record_field_first(x); q >= 0; q = record_field_next(&t->n[q])) {
        if (t->n[q].capture) { *out = semantic_text_ref(k, &t->n[q]); return 1; }
    }
    record_note_dropped(k, t, (uint32_t)c, 0);
    size_t mark = scratch_mark();
    Ref stack[64], *kid = x->arity <= 64 ? stack : scratch_alloc(sizeof(Ref) * x->arity); uint32_t n = 0;
    for (int32_t q = record_field_first(x); q >= 0; q = record_field_next(&t->n[q])) { Ref v; if (record_entity(k, t, q, &v)) { if (v.said != LP_SAID_TUPLE) v.said = 0; kid[n++] = v; } }
    if (n == 1) *out = kid[0]; else if (n > 1) *out = compose(kid, n, ref_above(kid, n));
    scratch_restore(mark);
    return n > 0;
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
