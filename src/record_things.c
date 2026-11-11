/* Canonical thing identities, key publication, and highway/vocabulary evidence. */
#define _GNU_SOURCE
#include "record_values_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int thing_compose(RecordWorker *k, const SemanticRecord *t, int32_t g, Ref *out, int depth);

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

static int referred_field(RecordWorker *k, const SemanticRecord *t, int32_t g, const char *name, KeyField *out, int depth){
    const ReferenceProjection *projection = record_selector_code(name)->projection;
    if (!projection) return 0;
    const PathStep *first = projection->reference;
    int32_t c = record_path_first(t, g, first, first->next); if (c < 0) return 0;
    const Dis *d = record_field_disposition(k->s, &t->n[c]); const SemanticValue *x = &t->n[c];
    if (x->kind == S_GROUP && record_field_first(x) >= 0 && record_field_next(&t->n[record_field_first(x)]) < 0 && t->n[record_field_first(x)].kind == S_TEXT) x = &t->n[record_field_first(x)];     /* a part that holds only its text */
    if (!d || d->what != D_REFER || !x->vlen) return 0;
    size_t key_length=x->vlen;
    const uint8_t *raw=x->val; Keys forms={0};
    if (d->k.has_re || d->k.nas) {
        record_key_forms(&d->k,raw,key_length,&forms,t,c); if (forms.n!=1) return 0;
        raw=(const uint8_t *)forms.k[0]; key_length=strlen(forms.k[0]);
    }
    const uint8_t *key=scoped_key(k,t,c,d->under,d->key_radix,raw,&key_length,depth);
    if (!key) return 0;
    for (uint8_t i = 0; i < d->ntargets; i++) {
        const TargetCode *target = &d->targets[i];
        if (target->within || !target->local) continue;
        for (const KeyProjection *p = projection->targets; p; p = p->next) if (p->tier == target->tier_id) {
            const KeyField *field = target->scope_key ? NULL : keys_field_get(&p->domain, key, key_length);
            if (field) { *out = *field; if (field->blocked) k->blocked++; return 1; }
            int32_t r = d->under[0] ? -1 : keyed(k, t, target, c, key, key_length); if (r < 0) break;
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

static Ref thing_operand(const Thing *thing,Ref value,const Say *program,const SemanticValue *field){
    if (value.said==LP_SAID_TUPLE) {
        const Dis *dis=field ? record_field_disposition(program,field) : NULL;
        /* A typed range is its two bounds, even inside a compound identity.
         * Keep that boundary without requiring a corpus-specific modifier. */
        if (thing->preserve || (dis && dis->what==D_RANGE)) return value;
    }
    value.said=0;
    return value;
}
static int one_thing(RecordWorker *k, const SemanticRecord *t, int32_t g, const Thing *th, Ref *out, int depth){
    if (k->s->surface.thing==(uint8_t)(th-k->s->thing+1)) return record_surface_text(k,t,g,out);
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
                    if (referred_field(k, t, g, th->name[z] + 1, &field, depth) && field.has_value) p[np++]=thing_operand(th,field.value,k->s,NULL);
                    else if (th->required[z]) return 0;
                    continue;
                }
                int32_t c = record_select_first(t, g, th->name[z]); if (c < 0) { if (th->required[z]) return 0; continue; }
                const SemanticValue *cn = &t->n[c]; if (cn->kind == S_GROUP && record_field_first(cn) >= 0 && record_field_next(&t->n[record_field_first(cn)]) < 0 && t->n[record_field_first(cn)].kind == S_TEXT) cn = &t->n[record_field_first(cn)];
                if (cn->kind != S_GROUP && (record_value_empty(k->s, cn) || !cn->vlen)) { if (th->required[z]) return 0; continue; }
                if (record_value(k, t, c, &p[np], depth)) { p[np]=thing_operand(th,p[np],k->s,&t->n[c]); np++; continue; }
                if (th->required[z]) return 0;
                record_obligation_note(k, 'o', &record_diagnostic_labels[DIAG_UNRESOLVED_KEY], (const uint8_t *)th->name[z], record_selector_code(th->name[z])->source_length, NULL, 0); }     /* said, never dropped */
            if (!np) return 0;
            Ref X = np == 1 ? p[0] : said_tuple(compose(p, np, ref_above(p, np)));
            *out = thing_scope(k,t,g,th,X,depth); return 1; }
        for (int z = 0; z < th->n; z++) {
            if (th->lit[z]) { p[z] = record_text((const uint8_t *)th->name[z], strlen(th->name[z])); p[z].said = 0; continue; }      /* a text the recipe writes */
            unsigned special=record_selector_code(th->name[z])->special;
            if (special==SELECT_SURFACE_FIRST || special==SELECT_SURFACE_LAST) {
                if (!record_surface_position(k,t,g,special==SELECT_SURFACE_LAST,&p[z],depth)) return 0;
                continue;
            }
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
                p[z]=thing_operand(th,p[z],k->s,NULL); continue;
            }
            int32_t c = record_select_first(t, g, th->name[z]); if (c < 0) return 0;
            if (th->product_id[z]) {                                         /* the part read as its native grammar's product, not its content */
                if (!record_product(k, t, c, th->product_id[z], &p[z], depth)) return 0;
                p[z].said = 0; continue; }
            if (th->form[z].has_re || th->space[z]) {                        /* the part's text as the source's notation parts it: what it writes, never what it is disposed as */
                const SemanticValue *cn = &t->n[c]; if (cn->kind == S_GROUP && record_field_first(cn) >= 0 && record_field_next(&t->n[record_field_first(cn)]) < 0 && t->n[record_field_first(cn)].kind == S_TEXT) cn = &t->n[record_field_first(cn)];
                if (cn->kind == S_GROUP || record_value_empty(k->s, cn)) return 0;
                Keys kf; kf.n = 0; if (th->form[z].has_re) { record_key_forms(&th->form[z], cn->val, cn->vlen, &kf, t, c); if (!kf.n) return 0; } else { if (cn->vlen >= sizeof kf.k[0]) return 0; memcpy(kf.k[0], cn->val, cn->vlen); kf.k[0][cn->vlen] = 0; }
                char *w = kf.k[0]; if (th->space[z]) for (char *q = w; *q; q++) if (*q == th->space[z]) *q = ' ';
                p[z] = record_text((const uint8_t *)w, strlen(w)); p[z].said = 0; continue; }
            if (th->preserve && t->n[c].kind == S_GROUP && record_value(k, t, c, &p[z], depth)) {
                /* A declared child composition is one operand. In particular,
                 * nested aligned layers must retain their own boundaries. */
            } else if (t->n[c].kind == S_GROUP && record_field_first(&t->n[c]) >= 0 && record_field_next(&t->n[record_field_first(&t->n[c])]) >= 0) {      /* a part that is several pieces: the path of what each is, in order */
                if(!compose_values(k,t,c,depth,0,&p[z]))return 0; }
            else if (!record_value(k, t, c, &p[z], depth)) {
            const Dis *cd = record_field_disposition(k->s, &t->n[c]);
            if (cd && cd->what == D_KEY && !record_value_empty(k->s, &t->n[c])) {
                const SemanticValue *cn = &t->n[c];
                if (cn->kind == S_GROUP && record_field_first(cn) >= 0 && record_field_next(&t->n[record_field_first(cn)]) < 0 && t->n[record_field_first(cn)].kind == S_TEXT) cn = &t->n[record_field_first(cn)];
                if (cn->kind == S_TEXT && cn->vlen) p[z] = record_text(cn->val, cn->vlen); else return 0;
            } else return 0;
        }
            p[z]=thing_operand(th,p[z],k->s,&t->n[c]); }
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
    /* Explicit pair operands are consumed by that assertion. A broad field
     * attestation must not reinterpret the same operands as relation names. */
    for (int i=0; i<k->s->npair; i++) {
        const Att *pair=&k->s->pair[i];
        if (!record_field_matches(&t->n[g],pair->tier) || !record_conditions_match(k->s,t,g,pair->when)) continue;
        if (pair->of[0] && record_select_first(t,g,pair->of)==(int32_t)(y-t->n)) return 1;
        for (int j=0; j<pair->n; j++) if (record_select_first(t,g,pair->name[j])==(int32_t)(y-t->n)) return 1;
    }
    Ref X; int r = record_thing(k, t, g, &X, 0); if (!r) return 0; const Thing *th = &k->s->thing[r - 1];
    for (int z = 0; z < th->n; z++) if (!th->lit[z] && !strchr(th->name[z], '/') && record_field_named(y, th->name[z], strlen(th->name[z]))) return 1;
    return 0;
}

void record_key_forms(const KSpec *spec, const uint8_t *value, size_t length, Keys *out, const SemanticRecord *tree, int32_t at){
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
        const Dis *disposition=record_field_disposition(s,x); if (disposition && disposition->what==D_OMIT) continue;
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
        for (int i = 0; i < s->nkey; i++) if (!s->key[i].scope[0] && record_field_named(x, s->key[i].tier, s->key[i].tier_length)) {
            int32_t field = record_field_child(t, g, s->key[i].name, -1); if (field < 0 || !t->n[field].vlen) continue;
            const Pair *key = &s->key[i]; KeyRank rank = k->kr; rank.member = (uint64_t)g;
            size_t length=t->n[field].vlen;
            const uint8_t *bytes=scoped_key(k,t,g,key->under,key->key_radix,t->n[field].val,&length,0); if (!bytes) continue;
            keys_field_put(&key->source_domain, bytes, length, &waiting, &rank);
            keys_field_put(&key->domain, bytes, length, &waiting, &rank);
            keys_field_put(&key->parent_domain, bytes, length, &waiting, &rank);
            if (reserve) for (const KeyProjection *projection = s->projections; projection; projection = projection->all_next)
                if (projection->tier == key->tier_id) keys_field_put(&projection->domain, bytes, length, &waiting, &rank);
        }
    }
    if (reserve) return;
    for (int32_t g = root; g <= end; g++) { const SemanticValue *x = &t->n[g]; if (x->context || x->kind != S_GROUP || !x->nlen) continue;
        for (int i = 0; i < s->nkey; i++) if (!s->key[i].scope[0] && record_field_named(x, s->key[i].tier, s->key[i].tier_length)) { int32_t kc = record_field_child(t, g, s->key[i].name, -1); Ref S;
            if (kc >= 0 && t->n[kc].vlen) {
                const Pair *key = &s->key[i];
                size_t length=t->n[kc].vlen;
                const uint8_t *bytes=scoped_key(k,t,g,key->under,key->key_radix,t->n[kc].val,&length,0); if (!bytes) continue;
                KeyRank rank = k->kr; rank.member = (uint64_t)g;
                for (const KeyProjection *projection = s->projections; projection; projection = projection->all_next) if (projection->tier == key->tier_id) {
                    KeyField field = projected_field(k, t, record_path_first(t, g, projection->field, NULL), 0);
                    keys_field_put(&projection->domain, bytes, length, &field, &rank);
                }
                uint64_t blocked = k->blocked; KeyField declaration = {0};
                if (record_thing(k, t, g, &S, 0)) { declaration.value = S; declaration.has_value = 1; }
                declaration.blocked = (uint8_t)(blocked != k->blocked);
                keys_field_put(&key->source_domain, bytes, length, &declaration, &rank);
                keys_field_put(&key->domain, bytes, length, &declaration, &rank);
                blocked = k->blocked; declaration = (KeyField){0};
                for (int32_t up = x->parent; up >= 0; up = t->n[up].parent) if (record_thing(k, t, up, &S, 0)) {
                    declaration.value = S; declaration.has_value = 1; break;
                }
                declaration.blocked = (uint8_t)(blocked != k->blocked);
                keys_field_put(&key->parent_domain, bytes, length, &declaration, &rank);
            } } }
}

static Ref resource_key(const char *list, const char *key){
    Ref parts[2]={string_ref((const uint8_t *)list,strlen(list)),record_text((const uint8_t *)key,strlen(key))};
    return said_tuple(compose(parts,2,ref_above(parts,2)));
}
static void resource_entry(RecordWorker *k, const HwLine *h, int line, const Ref *thing, const char *a, const char *b){
    if (k->hw) { record_highway_entry(k,h->what,line,thing,a,a?strlen(a):0,b,b?strlen(b):0); return; }
    Ref parts[3];
    if (h->what==1) {
        parts[0]=*thing; parts[1]=string_ref((const uint8_t *)"identifier vocabulary",21);
        parts[2]=string_ref((const uint8_t *)h->a.list,strlen(h->a.list));
    } else if (h->what==2) {
        parts[0]=resource_key(h->a.list,a); parts[1]=string_ref((const uint8_t *)"identifies",10); parts[2]=*thing;
    } else if (h->what==3) {
        parts[0]=resource_key(h->a.list,a); parts[1]=string_ref((const uint8_t *)"identifier alias",16); parts[2]=resource_key(h->b.list,b);
    } else {
        /* A crosswalk is an attested mapping, not a claim of unrestricted
         * equivalence. Its predicate retains both identifier vocabularies. */
        Ref relation[3]={string_ref((const uint8_t *)h->a.list,strlen(h->a.list)),
            string_ref((const uint8_t *)"cross-resource mapping",22),string_ref((const uint8_t *)h->b.list,strlen(h->b.list))};
        parts[0]=record_text((const uint8_t *)a,strlen(a));
        parts[1]=said_tuple(compose(relation,3,ref_above(relation,3)));
        parts[2]=record_text((const uint8_t *)b,strlen(b));
    }
    record_emit_claim_once(k,parts,3);
}
void record_highway_values(RecordWorker *k, const SemanticRecord *t, int32_t root){
    const Say *s = k->s; if (!record_conditions_match(s, t, root, s->whole)) return;
    for (uint32_t g = 0; g < t->count; g++) { const SemanticValue *x = &t->n[g]; if (x->context || x->kind == S_NOTE || !x->nlen) continue;
        if (x->kind==S_TEXT) { size_t i=0; while (i<x->vlen && (x->val[i]==' ' || x->val[i]=='\t' || x->val[i]=='\r' || x->val[i]=='\n')) i++; if (i==x->vlen) continue; }
        { const Dis *dg = record_field_disposition(s, x); if (dg && dg->what == D_OMIT) { g = (uint32_t)record_scope_end(t, (int32_t)g); continue; } }
        for (int i = 0; i < s->nhw; i++) { const HwLine *h = &s->hw[i]; if (!record_field_matches(x, h->tier) || !record_conditions_match(s, t, (int32_t)g, h->when)) continue;
            Ref X; Keys A, B;
            if (h->what <= 2) { if (x->kind == S_VALUE || !(x->kind == S_GROUP ? record_thing(k, t, (int32_t)g, &X, 0) : record_value(k, t, (int32_t)g, &X, 0))) continue;     /* a thing; or a text of a list, a type by itself */
                if (h->what == 1) { resource_entry(k,h,i,&X,NULL,NULL); continue; }
                keys_at(s, t, (int32_t)g, &h->a, &A); for (int a = 0; a < A.n; a++) resource_entry(k,h,i,&X,A.k[a],NULL); continue; }
            keys_at(s, t, (int32_t)g, &h->a, &A); if (!A.n) continue; keys_at(s, t, (int32_t)g, &h->b, &B);
            for (int a = 0; a < A.n; a++) for (int b = 0; b < B.n; b++) resource_entry(k,h,i,NULL,A.k[a],B.k[b]); } }
}

void record_vocabulary_evidence(RecordWorker *k, const SemanticRecord *t){
    for (uint32_t i=0; i<t->count; i++) {
        const SemanticValue *x=&t->n[i];
        if (x->context || !x->nlen) continue;
        const Dis *d=record_field_disposition(k->s,x);
        if (!d) continue;
        if (d->what==D_OMIT || d->what==D_METADATA) { i=(uint32_t)record_scope_end(t,(int32_t)i); continue; }
        if (!x->vlen) continue;
        if (d->what==D_REFER && d->ntargets==1 && d->targets[0].constructor_value &&
            d->targets[0].constructor_value->what==D_TYPE && d->targets[0].constructor_value->ntargets==1) {
            const TargetCode *family=&d->targets[0].constructor_value->targets[0]; Ref identifier;
            if (record_value(k,t,(int32_t)i,&identifier,0)) {
                Ref parts[3]={identifier,string_ref((const uint8_t *)"identifier vocabulary",21),
                    string_ref((const uint8_t *)family->name,family->length)};
                record_emit_claim_once(k,parts,3);
            }
        }
        if (d->what==D_REFER && (d->k.has_re || d->k.nas) && d->under[0]) {
            Ref scope, target;
            int32_t at=record_select_first(t,(int32_t)i,d->under);
            if (at<0 || !record_value(k,t,at,&scope,0) || !record_value(k,t,(int32_t)i,&target,0)) continue;
            Ref notation[2]={scope,record_text(x->val,x->vlen)};
            Ref parts[3]={said_tuple(compose(notation,2,ref_above(notation,2))),string_ref((const uint8_t *)"refers to",9),target};
            record_emit_claim_once(k,parts,3); continue;
        }
        if (d->what==D_TYPE && d->k.crosswalk[0]) {
            /* The source's notation and the type the highway's crosswalk reaches from it; a category of the tagset's
             * own (no edge) denotes nothing, and is said as written. */
            Ref canonical; if (!record_value(k,t,(int32_t)i,&canonical,0)) continue;
            Ref written=record_text(x->val,x->vlen); if (lp_id_eq(&canonical.id,&written.id)) continue;
            Ref notation[2]={string_ref((const uint8_t *)d->k.crosswalk,strlen(d->k.crosswalk)),written};
            Ref parts[3]={said_tuple(compose(notation,2,ref_above(notation,2))),string_ref((const uint8_t *)"denotes",7),canonical};
            record_emit_claim_once(k,parts,3); continue;
        }
        if (d->what==D_TYPE && !d->k.vocabulary && d->ntargets==1 &&
            (d->k.node || (HW && lp_highway_list(HW,d->targets[0].name) && !lp_highway_bank(HW,d->targets[0].name)))) {
            Ref identifier; if (!record_value(k,t,(int32_t)i,&identifier,0)) continue;
            /* Retain the reference's resource family in the source DAG even
             * when there is no frozen slot for this identifier. A text can be
             * used in several families; these are separate witnessed claims. */
            Ref parts[3]={identifier,string_ref((const uint8_t *)"identifier vocabulary",21),
                string_ref((const uint8_t *)d->targets[0].name,d->targets[0].length)};
            record_emit_claim_once(k,parts,3); continue;
        }
        if (d->what!=D_TYPE || !d->k.vocabulary) continue;
        Ref canonical; if (!record_value(k,t,(int32_t)i,&canonical,0)) continue;
        Ref notation[2]={string_ref((const uint8_t *)d->k.vocabulary_name,strlen(d->k.vocabulary_name)),record_text(x->val,x->vlen)};
        Ref parts[3]={said_tuple(compose(notation,2,ref_above(notation,2))),
            string_ref((const uint8_t *)"denotes",7),canonical};
        record_emit_claim_once(k,parts,3);
    }
}

int record_held_thing(RecordWorker *k, const SemanticRecord *t, int32_t g, Ref *out){
    int r = record_thing(k, t, g, out, 0); if (!r) return 0;
    const Thing *th = &k->s->thing[r - 1];
    if (th->n != 1 || !th->product_id[0] || th->of || th->span || th->resolved || th->within || th->infile) return r;
    int32_t c = record_select_first(t, g, th->name[0]); Ref content;
    if (c >= 0 && record_value(k, t, c, &content, 0)) { content.said = 0; *out = content; }
    return r;
}
