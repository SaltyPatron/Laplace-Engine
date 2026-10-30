/* Statement VM over one bound record. Compiler instructions are immutable;
 * selection, canonical values and containment output have separate owners. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>
#include "engine.h"
#include "structure.h"
#include "file_metadata.h"
#include "recipe_program.h"
#include "record_worker.h"
#include "record_select.h"
#include "record_diagnostics.h"
#include "record_values.h"
#include "record_output.h"
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

static void said_as(RecordWorker *k, const Dis *m, Ref S, Ref v){
    if (m->what != 1 && !memcmp(&v.id, &S.id, 16)) {
        /* A word can spell its own known category (UD's word "flat" and
         * deprel flat). The highway already records that membership. Do not
         * fabricate [flat, flat], which canonically denotes a repeated run.
         * An untyped or unknown self-value still lacks a readable claim. */
        const lp_list *list = HW && m->arg[0] ? lp_highway_list(HW, m->arg) : NULL;
        if (list && lp_highway_slot(HW, list, &v.id) >= 0) record_statement_count(k, m);
        else record_obligation_note(k, 'o', &record_diagnostic_labels[DIAG_SELF_VALUE], (const uint8_t *)m->name, m->name_length, NULL, 0);
        return;
    }
    record_statement_count(k, m);
    if (m->what == 1) { Ref p[3] = { S, string_ref((const uint8_t *)m->text, m->text_length), v }; record_emit_claim(k, p, 3); return; }     /* relation "TEXT": [thing, TEXT, value] */
    record_note_road_value(k, m, &v); Ref p[2] = { S, v }; record_emit_claim(k, p, 2); }

/* Qualifiers are content in a judgment record. The corpus remains its witness. */
static int qualifier_of(RecordWorker *k, const SemanticRecord *t, int32_t c){
    if (c<0 || (t->n[c].kind!=S_GROUP && record_value_empty(k->s,&t->n[c])) || !record_value(k,t,c,&k->qualifier,0)) return 0;
    k->qualified=1; return 1;
}
static int qualifiers(const Say *s, const SemanticRecord *t, int32_t g, const char *path, int32_t *out){
    if (!path[0] || record_selector_code(path)->special==SELECT_LABEL) return 0;
    int n=record_select_all(t,g,path,out,RECORD_REFERENCE_CAPACITY), count=0;
    for (int i=0; i<n; i++) if (out[i]>=0 && (t->n[out[i]].kind==S_GROUP ||
        (t->n[out[i]].vlen && !record_value_empty(s,&t->n[out[i]])))) out[count++]=out[i];
    return count;
}
static void field_qualifier(RecordWorker *k, const SemanticValue *field, int file){
    Ref parts[3]={k->fw,k->file_scope,string_ref(field->name,field->nlen)};
    if (!file) parts[1]=parts[2];
    size_t count=file?3:2;
    for (size_t i=0; i<count; i++) parts[i].said=0;
    k->qualifier=said_tuple(compose(parts,(uint32_t)count,ref_above(parts,count))); k->qualified=1;
}
static float score_of(const Say *s, const SemanticRecord *t, int32_t g){
    const SemanticValue *x = &t->n[g];
    for (int i = 0; i < s->nscore; i++) if (record_field_named(x, s->score[i].tier, strlen(s->score[i].tier))) { int32_t c = record_field_child(t, g, s->score[i].name, -1); if (c < 0 || !t->n[c].vlen || t->n[c].vlen > 31) return 1.0f;
        char z[32]; memcpy(z, t->n[c].val, t->n[c].vlen); z[t->n[c].vlen] = 0; char *e; double v = strtod(z, &e); if (e == z) return 1.0f;
        v = (v - s->score[i].from) / (s->score[i].to - s->score[i].from); return (float)(v < 0 ? 0 : v > 1 ? 1 : v); }
    return 1.0f;
}

typedef struct { int32_t end; uint64_t e0; Ref about; int has; } Pend;
static void unit_range(RecordWorker *k, const SemanticRecord *t, uint32_t g0, uint32_t g1, int speaks, Pend *pend, int *npp);

typedef struct { RecordSelectionFrame selection; RecordValueFrame values; } Tls;
static Tls tls_save(const SemanticRecord *t, int16_t *dm, const Say *say, const BindingMatrix *bindings){
    return (Tls){record_selection_enter(t,dm,say,bindings),record_values_enter()};
}
static void tls_back(const Tls *previous){
    record_selection_leave(&previous->selection);
    record_values_leave(&previous->values);
}

/* Execute statement instructions in the record's source order. */
static void unit_range(RecordWorker *k, const SemanticRecord *t, uint32_t g0, uint32_t g1, int speaks, Pend *pend, int *npp){
    const Say *s = k->s; int np_ = *npp;
    for (uint32_t g = g0; g < g1; g++) { const SemanticValue *x = &t->n[g];
        if (x->context) continue;
        /* a part of a wide part is a part of its own: what it says is said once in it (said_already), never once in the run
         * of parts a worker happens to read, whose bounds are the number of threads (runs_of): the same claims on any machine */
        while (np_ && pend[np_ - 1].end < (int32_t)g) { np_--; record_group_claims(k, pend[np_].e0, pend[np_].has ? &pend[np_].about : NULL); }
        if (x->kind == S_NOTE || x->kind == S_VALUE) { const Dis *d = record_field_disposition(s, x);
            if (d && d->what == D_METADATA && !record_value_empty(s, x)) { Ref p[2] = { string_ref(x->name, x->nlen), record_text(x->val, x->vlen) }; Ref m = said_tuple(compose(p, 2, ref_above(p, 2))); record_refs_push(&k->meta, &m); }
            continue; }
        if (x->kind != S_GROUP || !speaks) continue;
        k->dependence_class=NULL; memset(&k->dependence,0,sizeof k->dependence);
        for (int32_t up=(int32_t)g; up>=0 && !k->dependence_class; up=t->n[up].parent) {
            for (int a=0; a<s->ndependence; a++) { const Dependence *d=&s->dependence[a];
                if (record_field_matches(&t->n[up],d->tier) && record_conditions_match(s,t,up,d->when)) {
                    k->dependence_class=d->class; k->dependence=record_text((const uint8_t *)d->class->label,d->length); break;
                }
            }
        }
        if (x->nlen) { const Dis *dg = record_field_disposition(s, x); if (dg && dg->what == D_OMIT) { g = (uint32_t)record_scope_end(t, (int32_t)g); continue; } }     /* the file's bookkeeping: nothing inside it is read */
        k->selfcp = -1;
        if (s->selfmark) { Ref X; if (record_subject(k, t, (int32_t)g, &X) && X.tier == 0) k->selfcp = (long)lp_tier0_codepoint(T0, &X.id); }     /* the code point the part speaks of, for the source's mark */
        Ref S; int has = -1; float sc = score_of(s, t, (int32_t)g); k->score = s->nitself ? 1.0f : sc; k->qualified = 0;      /* the score is the attested tuple's, where there is one; what is said of it is said outright */
        uint64_t e0 = k->ev.n; k->grp.n = 0;
        record_emit_layers(k,t,(int32_t)g);
        for (int a = 0; a < s->nitself; a++) { if (!record_field_named(x, s->itself[a].tier, strlen(s->itself[a].tier)) || !record_conditions_match(s, t, (int32_t)g, s->itself[a].when)) continue;
            Ref X; if (!record_thing(k, t, (int32_t)g, &X, 0)) break;
            int32_t who[RECORD_REFERENCE_CAPACITY]; int nw = qualifiers(s, t, (int32_t)g, s->itself[a].with, who), was = k->qualified; Ref wv = k->qualifier;
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !qualifier_of(k, t, who[wi])) continue;
                record_emit_event(k, &X.id, sc); }
            k->qualified = was; k->qualifier = wv; }
        for (int a = 0; a < s->npair; a++) { if (!record_field_matches(x, s->pair[a].tier) || !record_conditions_match(s, t, (int32_t)g, s->pair[a].when)) continue;      /* nothing written between the two: the pair */
            Ref P;
            if (s->pair[a].of[0]) { int32_t oc = record_select_first(t, (int32_t)g, s->pair[a].of); if (oc < 0 || !record_value(k, t, oc, &P, 0)) continue; }      /* of another of its parts */
            else { if (has < 0) has = record_subject(k, t, (int32_t)g, &S); if (!has) break; P = S; }
            for (int z = 0; z < s->pair[a].n; z++) { Ref v;
                if (record_selector_code(s->pair[a].name[z])->special == SELECT_FILE) v = k->fstem;           /* what the file's own name says of each of its parts */
                else { int32_t c = record_select_first(t, (int32_t)g, s->pair[a].name[z]); if (c < 0) continue;
                    if (t->n[c].kind == S_GROUP && t->n[c].join == S_PIECES && record_field_first(&t->n[c]) >= 0 && record_field_next(&t->n[record_field_first(&t->n[c])]) >= 0) {      /* a list: the pair with each */
                        for (int32_t q = record_field_first(&t->n[c]); q >= 0; q = record_field_next(&t->n[q])) { Ref w; if (t->n[q].kind == S_GROUP || !record_value(k, t, q, &w, 0) || !memcmp(&w.id, &P.id, 16)) continue; Ref p[2] = { P, w }; record_emit_claim(k, p, 2); }
                        continue; }
                    if (!record_value(k, t, c, &v, 0)) continue; }
                if (!memcmp(&v.id, &P.id, 16)) continue; Ref p[2] = { P, v }; record_emit_claim(k, p, 2); } }
        for (int a = 0; a < s->natt; a++) { if (!record_field_matches(x, s->att[a].tier) || !record_conditions_match(s, t, (int32_t)g, s->att[a].when)) continue;
            Ref S0 = S; int had = has, was = k->qualified; Ref wv = k->qualifier;
            if (s->att[a].of[0]) { int32_t oc = record_select_first(t, (int32_t)g, s->att[a].of); if (oc < 0 || !record_value(k, t, oc, &S, 0)) continue; has = 1; }      /* said of another of its parts */
            else { if (has < 0) has = record_subject(k, t, (int32_t)g, &S); if (!has) break; }
            int32_t who[RECORD_REFERENCE_CAPACITY]; int nw = qualifiers(s, t, (int32_t)g, s->att[a].with, who);
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !qualifier_of(k, t, who[wi])) continue;
            for (int z = 0; z < s->att[a].n; z++) for (int32_t c = record_field_first(x); c >= 0; c = record_field_next(&t->n[c])) { const SemanticValue *y = &t->n[c];
                if (record_selector_code(s->att[a].name[z])->special == SELECT_ELEMENTS) {                      /* each element inside it that is a value: its own text, or its one attribute (rdf:resource), as the element's name means it */
                    if (y->kind != S_GROUP || !y->nlen || y->join == S_PIECES) continue; int32_t only = -1, nk = 0;
                    for (int32_t q = record_field_first(y); q >= 0; q = record_field_next(&t->n[q])) { const Dis *dq = t->n[q].nlen && t->n[q].kind == S_VALUE ? record_field_disposition(s, &t->n[q]) : NULL; if (dq && dq->what == D_OMIT) continue; nk++; only = q; }   /* what is omitted does not count */
                    if (nk != 1 || (t->n[only].kind != S_TEXT && t->n[only].kind != S_VALUE) || record_value_empty(s, &t->n[only])) continue;
                    if (!s->att[a].of[0] && record_names_identity(k, t, (int32_t)g, y)) continue;                     /* the element that names the thing is the thing, not said of it */
                    Ref v; if (!record_value(k, t, only, &v, 0)) continue;
                    const Dis *m = record_value_meaning(s, y); if (!m) { if (!wi) record_obligation_node(k, &record_diagnostic_labels[DIAG_ATTEST], y); continue; }      /* the element's name is markup: what it means, or no claim */
                    said_as(k, m, S, v); continue; }
                if (record_selector_code(s->att[a].name[z])->special == SELECT_SELF) {                       /* its own text, as its own name means it */
                    if (y->kind != S_TEXT || record_value_empty(s, y)) continue; Ref v = record_text(y->val, y->vlen); if (!memcmp(&v.id, &S.id, 16)) continue;    /* the text the thing is named by: the thing, said already */
                    const Dis *m = record_value_meaning(s, x); if (!m) { if (!wi) record_obligation_node(k, &record_diagnostic_labels[DIAG_ATTEST], x); continue; }
                    said_as(k, m, S, v); continue; }
                if (!y->nlen || y->kind == S_TEXT || !record_field_matches(y, s->att[a].name[z])) continue;
                if (!s->att[a].of[0] && record_names_identity(k, t, (int32_t)g, y)) continue;                         /* a part that names the thing is said already: it is the thing */
                if (record_field_defaulted(s,t,c)) continue;
                if (s->att[a].with[0] && record_selector_code(s->att[a].with)->special==SELECT_LABEL) field_qualifier(k,y,s->att[a].file);
                if (y->kind == S_GROUP && y->join != S_PIECES && strchr(s->att[a].name[z], '*')) continue;      /* a wildcard names the part's values, not the elements inside it, which speak for themselves */
                /* A recipe can name a structured field as one composition,
                 * including several injected bindings. Honor that identity
                 * before walking scalar children; groups have no scalar vlen. */
                if (y->kind == S_GROUP) {
                    const Dis *meaning=record_value_meaning(s,y); Ref value;
                    if (meaning && record_thing(k,t,c,&value,0)) { said_as(k,meaning,S,value); continue; }
                }
                /* A declared meaning attests a structured scalar as one value.
                 * Otherwise its named parts retain their own scoped meanings. */
                if (y->kind == S_GROUP && (!y->scalar || !record_value_meaning(s, y))) {
                    const Dis *my = record_value_meaning(s, y);
                    for (int32_t q = record_field_first(y); q >= 0; q = record_field_next(&t->n[q])) { const SemanticValue *w = &t->n[q]; if (!w->vlen) continue; Ref v;
                        if (w->kind == S_VALUE && w->nlen && !record_field_named(w, (const char *)y->name, y->nlen)) { const Dis *dw = record_field_disposition(s, w); if (dw && (dw->what == D_OMIT || dw->what == D_METADATA)) continue;      /* the file's bookkeeping says nothing */
                            const Dis *mw = record_value_meaning(s, w);
                            /* An explicit scoped meaning overrides the generic
                             * property cache: this context may not be covered. */
                            if ((!mw || !mw->parent_slot) && recipe_property_cached(s, (const char *)w->name, w->nlen)) continue;
                            if (record_names_identity(k,t,(int32_t)g,w)) continue;
                            Ref vw; if (!dw || !record_value(k, t, q, &vw, 0)) vw = record_text(w->val, w->vlen);
                            /* the KEY valued itself (MISC.Translit): [thing, VALUE] */
                            if (mw) { said_as(k, mw, S, vw); continue; }
                            if (!my) { if (!wi) record_obligation_note(k, 'o', &record_diagnostic_labels[DIAG_ATTEST], y->name, y->nlen, w->name, w->nlen); continue; }
                            Ref kv[2] = { string_ref(w->name, w->nlen), vw }; kv[0].said = 0; if (kv[1].said != LP_SAID_TUPLE) kv[1].said = 0;   /* the pair the record holds: the same entity */
                            Ref pv = said_tuple(compose(kv, 2, ref_above(kv, 2))); record_note_road_value(k, my, &pv); record_statement_count(k, my); Ref p[2] = { S, pv }; record_emit_claim(k, p, 2); continue; }
                        if (!record_value(k, t, q, &v, 0)) continue;
                        const Dis *meaning=record_value_meaning(s,w); if (!meaning) meaning=my;
                        if (!meaning) { if (!wi) record_obligation_node(k, &record_diagnostic_labels[DIAG_ATTEST], w); continue; }
                        said_as(k, meaning, S, v); } }
                else { const Dis *m = record_value_meaning(s, y);
                    if ((!m || !m->parent_slot) && recipe_property_cached(s, (const char *)y->name, y->nlen)) continue;
                    Ref v; if (!record_value(k, t, c, &v, 0)) continue;
                    if (!m) { if (!wi) record_obligation_node(k, &record_diagnostic_labels[DIAG_ATTEST], y); continue; }
                    said_as(k, m, S, v); } }
            }
            if (s->att[a].of[0]) { S = S0; has = had; }
            k->qualified = was; k->qualifier = wv; }
        for (int a = 0; a < s->nhold; a++) { if (!record_field_matches(x, s->hold[a].tier) || !record_conditions_match(s, t, (int32_t)g, s->hold[a].when)) continue;   /* the things inside it, under their own name */
            if (has < 0) has = record_subject(k, t, (int32_t)g, &S); if (!has) break;
            /* the nearest things of those names inside it, however deep: what holds them and is no thing is how the file groups them */
            size_t mark = scratch_mark();
            size_t scap = (size_t)t->count + 16, sp = 0; int32_t *stack = scratch_alloc(sizeof(int32_t) * scap);
            { size_t nk = 0; for (int32_t q = record_field_first(x); q >= 0; q = record_field_next(&t->n[q])) nk++;
              sp = nk; size_t i = nk; for (int32_t q = record_field_first(x); q >= 0; q = record_field_next(&t->n[q])) stack[--i] = q; }      /* in the file's order: the first on top */
            while (sp) { int32_t c = stack[--sp]; const SemanticValue *y = &t->n[c]; if (y->kind != S_GROUP) continue; Ref v; int named = 0;
                for (int z = 0; z < s->hold[a].n && !named; z++) named = record_field_matches(y, s->hold[a].name[z]);
                if (named && record_thing(k, t, c, &v, 0)) { if (memcmp(&v.id, &S.id, 16)) {
                        const SemanticValue *by = y; if (s->hold[a].file) { int32_t via = c; while (t->n[via].parent >= 0 && t->n[via].parent != (int32_t)g) via = t->n[via].parent; by = &t->n[via]; }    /* via: the element between them */
                        const Dis *m = record_value_meaning(s, by);                       /* an element's name is markup, and what holds what is the tree's: a claim only of what value means */
                        if (!m) record_obligation_node(k, &record_diagnostic_labels[DIAG_HOLDS], by); else said_as(k, m, S, v); } continue; }
                { Ref tmp; if (record_thing(k, t, c, &tmp, 0)) continue; }               /* another thing: what it holds is its own */
                size_t nk = 0; for (int32_t q = record_field_first(y); q >= 0; q = record_field_next(&t->n[q])) nk++;
                if (sp + nk <= scap) { size_t i = sp + nk; for (int32_t q = record_field_first(y); q >= 0; q = record_field_next(&t->n[q])) stack[--i] = q; sp += nk; } }
            scratch_restore(mark); }
        for (int a = 0; a < s->nrel; a++) { if (!record_field_matches(x, s->rel[a].tier) || !record_conditions_match(s, t, (int32_t)g, s->rel[a].when)) continue;
            Ref subject;
            if (s->rel[a].of[0]) { int32_t at=record_select_first(t,(int32_t)g,s->rel[a].of); if (at<0 || !record_value(k,t,at,&subject,0)) continue; }
            else { if (has < 0) has = record_subject(k, t, (int32_t)g, &S); if (!has) break; subject=S; }
            int was = k->qualified; Ref wv = k->qualifier; int32_t who[RECORD_REFERENCE_CAPACITY]; int nw = qualifiers(s, t, (int32_t)g, s->rel[a].with, who);
            for (int wi = 0; wi < (nw ? nw : 1); wi++) { if (nw && !qualifier_of(k, t, who[wi])) continue;
            Ref rel, o;
            if (record_selector_code(s->rel[a].rel)->special == SELECT_FILE) { if (!wi) record_obligation_note(k, 'o', &record_diagnostic_labels[DIAG_RELATE], (const uint8_t *)"{file}", 6, NULL, 0); break; }     /* a file's name is never a relation */
            else { int32_t rc = record_select_first(t, (int32_t)g, s->rel[a].rel); if (rc < 0 || !record_value(k, t, rc, &rel, 0)) break;
                const Dis *m = record_value_meaning(s, &t->n[rc]); if (!m) { if (!wi) record_obligation_node(k, &record_diagnostic_labels[DIAG_RELATE], &t->n[rc]); break; }     /* a layer's name, a field's: no relation; one of the source's own inventory, which value names */
                if (m->what==1) rel=string_ref((const uint8_t *)m->text,m->text_length);
                else record_note_road_value(k, m, &rel);
                record_statement_count(k, m); }
            if (record_selector_code(s->rel[a].to)->special == SELECT_SELF) {
                if (record_value(k,t,(int32_t)g,&o,0)) { Ref p[]={subject,rel,o}; record_emit_claim(k,p,3); }
            }
            else if (record_selector_code(s->rel[a].to)->special == SELECT_UNNAMED) {                              /* to each of the parts the file gives no name */
                for (int32_t c = record_field_first(x); c >= 0; c = record_field_next(&t->n[c])) { const SemanticValue *y = &t->n[c]; if (y->kind != S_TEXT || y->nlen || record_value_empty(s, y)) continue; o = record_text(y->val, y->vlen); Ref p[3] = { subject, rel, o }; record_emit_claim(k, p, 3); } }
            else if (record_field_child(t, (int32_t)g, s->rel[a].to, record_field_child(t, (int32_t)g, s->rel[a].to, -1)) >= 0) {   /* several parts of that name: the relation to each */
                for (int32_t oc = record_field_child(t, (int32_t)g, s->rel[a].to, -1); oc >= 0; oc = record_field_child(t, (int32_t)g, s->rel[a].to, oc)) if (record_value(k, t, oc, &o, 0)) { Ref p[3] = { subject, rel, o }; record_emit_claim(k, p, 3); } }
            else { int32_t oc = record_select_first(t, (int32_t)g, s->rel[a].to);
                if (oc >= 0 && t->n[oc].kind == S_GROUP && record_field_first(&t->n[oc]) >= 0 && record_field_next(&t->n[record_field_first(&t->n[oc])]) >= 0) {      /* several: the relation to each */
                    for (int32_t q = record_field_first(&t->n[oc]); q >= 0; q = record_field_next(&t->n[q])) if (record_value(k, t, q, &o, 0)) { Ref p[3] = { subject, rel, o }; record_emit_claim(k, p, 3); } }
                else if (oc >= 0 && record_value(k, t, oc, &o, 0)) { Ref p[3] = { subject, rel, o }; record_emit_claim(k, p, 3); }
                else if (s->rel[a].alone && oc >= 0) { Ref p[2] = { subject, rel }; record_emit_claim(k, p, 2); } }      /* to what names nothing (a treebank's root): the relation alone, where the recipe says so; otherwise no claim */
            }
            k->qualified = was; k->qualifier = wv; }
        for (int a = 0; a < s->ntogether; a++) if (record_field_named(x, s->together[a].tier, strlen(s->together[a].tier)) && record_conditions_match(s, t, (int32_t)g, s->together[a].when)) {
            if (np_ < 64) { pend[np_].end = record_scope_end(t, (int32_t)g); pend[np_].e0 = e0; pend[np_].has = record_thing(k, t, (int32_t)g, &pend[np_].about, 0); np_++; } break; }      /* closed when the reading passes the last node inside it */
    }
    *npp = np_;
}

/* Own the evaluation frame and restore it before another record runs. */
void record_execute(RecordWorker *sink, const SemanticRecord *t, int32_t root, uint64_t ordinal, int bind_keys){
    size_t record_mark = scratch_mark();
    RecordWorker *k = sink; const Say *s = k->s; k->kr.unit = ordinal; uint64_t ev0 = k->ev.n; k->score = 1.0f;
    k->dependence_class=NULL; memset(&k->dependence,0,sizeof k->dependence);
    keys_reader_file(k->kr.file);
    uint8_t binding_phase = k->binding_phase; k->binding_phase = (uint8_t)(bind_keys == 1 || k->defer_missing);
    record_worker_outputs(k);
    k->cs_epoch++; k->cs_n = 0; k->rec = root;                               /* a new part: none of its claims made yet, its own record */
    k->dm = scratch_alloc(sizeof(int16_t) * t->count); memset(k->dm, 0, sizeof(int16_t) * t->count);
    k->lo = scratch_alloc(t->count); memset(k->lo, 0, t->count); k->lo_tree = t;
    k->tc = scratch_alloc(sizeof(Ref) * t->count); k->ts = scratch_alloc(t->count); k->uk = scratch_alloc(t->count);
    k->tc_tree = t; k->tc_n = t->count; memset(k->ts, 0, t->count); memset(k->uk, 0, t->count);
    /* Products must outlive every nested composition's scratch restore. Keep
     * this cache in the record frame, never behind pointers into child frames. */
    k->text_products = scratch_alloc(sizeof(Ref) * t->count);
    k->text_ready = scratch_alloc(t->count); memset(k->text_ready, 0, t->count);
    k->surface_positions = NULL;
    if (s->surface.linear) {
        k->surface_positions = scratch_alloc(sizeof(SurfacePosition) * t->count);
        memset(k->surface_positions, 0xff, sizeof(SurfacePosition) * t->count);
    }
    Tls previous = tls_save(t, k->dm, s, &k->bindings);
    record_index_keys(k, t);
    if (s->nadmission && !record_admit_references(k,t,root,!bind_keys)) {
        k->binding_phase=binding_phase; tls_back(&previous); scratch_restore(record_mark); return;
    }
    if (bind_keys) { record_publish_keys(k, t, root, bind_keys == 3); k->binding_phase = binding_phase; tls_back(&previous); scratch_restore(record_mark); return; }
    if (k->hw) { record_highway_values(k, t, root); record_publish_keys(k, t, root, 0); k->binding_phase = binding_phase; tls_back(&previous); scratch_restore(record_mark); return; }     /* read for the highway: its types, not what it attests */
    if (s->nline && t->n[root].kind == S_TEXT) {                             /* a line of a page: where it matches a pattern, it says the pattern's parts */
        const SemanticValue *x = &t->n[root]; size_t mark = scratch_mark(); char stack[4096], *ln = x->vlen < sizeof stack ? stack : scratch_alloc((size_t)x->vlen + 1); memcpy(ln, x->val, x->vlen); ln[x->vlen] = 0;
        for (int i = 0; i < s->nline; i++) { regmatch_t m[5]; if (regexec(&s->line[i].re, ln, 5, m, 0)) continue;
            Ref part[4]; int np = 0; for (int q = 1; q < 5 && np < 4; q++) if (m[q].rm_so >= 0 && m[q].rm_eo > m[q].rm_so) part[np++] = record_text((const uint8_t *)ln + m[q].rm_so, (size_t)(m[q].rm_eo - m[q].rm_so));
            /* a predicate the recipe writes, and a line's first part taken as the relation, are a heading's or a field's name:
             * an obligation, never a relation (Laplace-Engine#32) */
            if (s->line[i].mode == 3 && np == 2) record_obligation_note(k, 'o', &record_diagnostic_labels[DIAG_LINE_PREDICATE], (const uint8_t *)s->line[i].pred, s->line[i].pred_length, NULL, 0);
            else if(s->line[i].mode>=4) {
                if(!k->has_fabout || np!=(s->line[i].mode==5?2:1)) {
                    record_obligation_note(k,'o',&record_diagnostic_labels[DIAG_LINE],(const uint8_t *)ln,x->vlen,NULL,0);
                } else {
                    Ref subject=k->fabout;
                    if(s->line[i].mode==5){Ref scoped[]={subject,part[0]};subject=said_tuple(compose(scoped,2,ref_above(scoped,2)));}
                    Ref claim[]={subject,record_text((const uint8_t *)s->line[i].pred,s->line[i].pred_length),part[np-1]};
                    record_emit_claim(k,claim,3);
                }
            }
            else if (s->line[i].mode == 2 && np == 2) record_emit_claim(k, part, 2);
            else if (s->line[i].mode == 1 && np >= 2) record_emit_claim(k, part, np);
            else if (s->line[i].mode == 0 && np == 2 && k->has_fabout) record_obligation_note(k, 'o', &record_diagnostic_labels[DIAG_LINE], (const uint8_t *)ln + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so), NULL, 0); }
        scratch_restore(mark); }
    int speaks = record_conditions_match(s, t, root, s->whole);
    Pend pend[64]; int np_ = 0;      /* the parts whose claims are said together, still open */
    unit_range(k, t, 0, t->count, speaks, pend, &np_);
    if (speaks) {
        k->qualified=0; k->dependence_class=NULL; memset(&k->dependence,0,sizeof k->dependence); k->score=1.0f;
        record_highway_values(k,t,root);
        record_vocabulary_evidence(k,t);
    }
    if (speaks) for (uint32_t i = 0; i < t->count; i++) { const SemanticValue *x = &t->n[i];          /* what no line of the recipe reads is said to be left open */
        if (x->context || x->kind != S_VALUE || !x->nlen || record_field_disposition(s, x) || !record_field_unmentioned(s, x)) continue;
        int32_t u = x->parent; while (u >= 0 && !record_field_disposition(s, &t->n[u])) u = t->n[u].parent;          /* a part of a part the recipe reads is read with it (r1's A0 of A0>Agent) */
        if (u < 0) record_note_unhandled(k, x); }

    if (speaks) for (uint32_t scope = 0; scope < t->count; scope++) if (t->n[scope].dropped_first) {
        int32_t ancestor = (int32_t)scope;
        while (ancestor >= 0 && !record_field_disposition(s, &t->n[ancestor])) ancestor = t->n[ancestor].parent;
        if (ancestor >= 0) continue;
        for (uint32_t at = t->n[scope].dropped_first; at; at = t->dropped[at - 1].scope_next) {
            const DroppedField *field = &t->dropped[at - 1];
            if (!field->length || field->length > 62 || !s->said || recipe_program_mentions(s->paths, field->name, field->length)) continue;
            DroppedField *mutable_field = &t->dropped[at - 1];
            if (field->all > field->seen) { record_note_open_name(k, field->name, field->length, field->all - field->seen); mutable_field->seen = field->all; }
        }
    }
    while (np_) { np_--; record_group_claims(k, pend[np_].e0, pend[np_].has ? &pend[np_].about : NULL); }
    record_publish_keys(k, t, root, 0);
    /* A named curated record holds its thing and its claims. Composing all
     * packaging fields beside that thing would record pointers and JSON keys
     * as another entity of it. Unnamed records retain their content tree. */
    Ref T; int whole=0;
    if (speaks) {
        int named=0;
        for (int i=0; i<s->nthing && !named; i++) named=record_field_named(&t->n[root],s->thing[i].tier,strlen(s->thing[i].tier));
        whole=named ? record_thing(k,t,root,&T,0)>0 : record_entity(k,t,root,&T);
    }
    record_build_provenance(k, t, root, ev0, whole ? &T : NULL);
    k->binding_phase = binding_phase;
    tls_back(&previous);
    scratch_restore(record_mark);
}
