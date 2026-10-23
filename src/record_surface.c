/* Compose an indexed surface sequence, including range tokens. Decimal-indexed
 * rows are syntactic positions and do not add surface bytes. Selector operands,
 * separator and adjacency policy are compiled from the recipe. */
#include "record_surface.h"
#include "record_values.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SURFACE_WORD=1, SURFACE_RANGE, SURFACE_EMPTY };
typedef struct {
    int32_t scope; uint32_t first, last; uint8_t kind, adjoining;
    const SemanticValue *form;
} SurfaceRow;
typedef struct { SurfaceRow *rows; uint32_t count; } SurfaceRows;

static void invalid_surface(const RecordWorker *worker, const char *reason){
    fprintf(stderr,"%s: invalid indexed surface: %s\n",worker->r->name,reason); exit(2);
}
static const SemanticValue *scalar(const SemanticRecord *record, int32_t at){
    if (at<0) return NULL;
    const SemanticValue *value=&record->n[at];
    while (value->kind==S_GROUP) {
        int32_t child=record_field_first(value);
        if (child<0 || record_field_next(&record->n[child])>=0) return NULL;
        value=&record->n[child];
    }
    return value;
}
static int number(const uint8_t *bytes, uint32_t length, uint32_t *at, uint32_t *out){
    uint32_t begin=*at, n=0;
    while (*at<length && bytes[*at]>='0' && bytes[*at]<='9') {
        unsigned digit=bytes[(*at)++]-'0';
        if (n>(UINT32_MAX-digit)/10) return 0;
        n=n*10+digit;
    }
    *out=n; return *at>begin;
}
static int index_row(const SemanticValue *value, SurfaceRow *row){
    if (!value) return 0;
    uint32_t at=0;
    if (!number(value->val,value->vlen,&at,&row->first)) return 0;
    row->last=row->first; row->kind=SURFACE_WORD;
    if (at==value->vlen) return row->first!=0;
    uint8_t separator=value->val[at++];
    if ((separator!='-' && separator!='.') || !number(value->val,value->vlen,&at,&row->last) || at!=value->vlen) return 0;
    row->kind=separator=='-' ? SURFACE_RANGE : SURFACE_EMPTY;
    return row->kind==SURFACE_RANGE ? row->first && row->last>row->first : row->last!=0;
}
static SurfaceRows rows_of(RecordWorker *worker, const SemanticRecord *record, int32_t root){
    const SurfaceProgram *program=&worker->s->surface;
    size_t capacity=record->n[root].arity;
    SurfaceRows out={.rows=scratch_alloc((capacity?capacity:1)*sizeof(SurfaceRow))};
    uint32_t previous=0, covered=0;
    for (int32_t child=record_field_first(&record->n[root]); child>=0; child=record_field_next(&record->n[child])) {
        if (!record_field_matches(&record->n[child],program->row)) continue;
        SurfaceRow row={.scope=child};
        if (program->linear) {
            row.first=row.last=++previous; row.kind=SURFACE_WORD;
        } else if (!index_row(scalar(record,record_select_first(record,child,program->key)),&row)) invalid_surface(worker,"invalid index notation");
        row.form=scalar(record,record_select_first(record,child,program->form));
        if (row.kind!=SURFACE_EMPTY && (!row.form || !row.form->vlen)) invalid_surface(worker,"surface row has no form");
        if (!program->linear && row.kind==SURFACE_WORD) {
            if (row.first!=previous+1) invalid_surface(worker,"word indices are not consecutive");
            previous=row.first;
        } else if (row.kind==SURFACE_RANGE) {
            if (row.first!=previous+1 || row.first<=covered) invalid_surface(worker,"overlapping or misplaced range");
            covered=row.last;
        } else if (row.first!=previous) invalid_surface(worker,"empty position is not after its anchor");
        const SemanticValue *adjacency=program->linear ? NULL : scalar(record,record_select_first(record,child,program->adjoining));
        row.adjoining=(uint8_t)(adjacency && adjacency->vlen==program->adjoining_length &&
                               !memcmp(adjacency->val,program->adjoining_value,adjacency->vlen));
        if (out.count>=capacity) invalid_surface(worker,"row count exceeds its prepared scope");
        out.rows[out.count++]=row;
    }
    if (!previous || covered>previous) invalid_surface(worker,"incomplete surface sequence");
    return out;
}
static int surface_row(const SurfaceRow *row, uint32_t *covered){
    if (row->kind==SURFACE_EMPTY) return 0;
    if (row->kind==SURFACE_RANGE) { *covered=row->last; return 1; }
    return row->first>*covered;
}
int record_surface_text(RecordWorker *worker, const SemanticRecord *record, int32_t root, Ref *out){
    const SurfaceProgram *program=&worker->s->surface;
    if (!program->thing || !record_field_matches(&record->n[root],program->tier)) return 0;
    size_t mark=scratch_mark(), length=0; SurfaceRows rows=rows_of(worker,record,root);
    uint32_t covered=0, characters=0; const SurfaceRow *previous=NULL;
    for (uint32_t i=0; i<rows.count; i++) if (surface_row(&rows.rows[i],&covered)) {
        size_t separator=previous && !previous->adjoining ? program->separator_length : 0;
        if (separator>SIZE_MAX-length || rows.rows[i].form->vlen>SIZE_MAX-length-separator) invalid_surface(worker,"surface exceeds address space");
        length+=separator+rows.rows[i].form->vlen; previous=&rows.rows[i];
        if (program->linear) {
            const SemanticValue *form=rows.rows[i].form; uint32_t count=0;
            for (uint32_t byte=0; byte<form->vlen; byte++) count+=(form->val[byte]&0xc0)!=0x80;
            if (count>UINT32_MAX-characters) invalid_surface(worker,"surface character position overflow");
            SurfacePosition position={characters,characters+count};
            worker->surface_positions[rows.rows[i].scope]=position;
            worker->surface_positions[form-record->n]=position;
            characters+=count;
        }
    }
    if (program->linear) worker->surface_positions[root]=(SurfacePosition){0,characters};
    uint8_t *bytes=scratch_alloc(length); size_t used=0; covered=0; previous=NULL;
    for (uint32_t i=0; i<rows.count; i++) if (surface_row(&rows.rows[i],&covered)) {
        if (previous && !previous->adjoining) { memcpy(bytes+used,program->separator,program->separator_length); used+=program->separator_length; }
        memcpy(bytes+used,rows.rows[i].form->val,rows.rows[i].form->vlen); used+=rows.rows[i].form->vlen; previous=&rows.rows[i];
    }
    const SemanticValue *declared=program->linear ? NULL : scalar(record,record_select_first(record,root,program->verify));
    if (declared && (declared->vlen!=length || memcmp(declared->val,bytes,length))) invalid_surface(worker,"declared text differs from the composed tokens");
    *out=record_text(bytes,length); out->said=0; scratch_restore(mark); return 1;
}
int record_surface_position(RecordWorker *worker, const SemanticRecord *record, int32_t scope,
    int last, Ref *out, int depth){
    const SurfaceProgram *program=&worker->s->surface;
    if (!program->linear || !worker->surface_positions || scope<0 || (uint32_t)scope>=record->count) return 0;
    if (worker->surface_positions[scope].first==UINT32_MAX) {
        int32_t root=scope;
        while (root>=0 && !record_field_matches(&record->n[root],program->tier)) root=record->n[root].parent;
        Ref surface;
        if (root<0 || !record_thing(worker,record,root,&surface,depth+1)) return 0;
    }
    SurfacePosition position=worker->surface_positions[scope];
    if (position.first==UINT32_MAX || position.end<=position.first) return 0;
    uint32_t value=last ? position.end-1 : position.first;
    uint8_t digits[10]; size_t count=0;
    do { digits[count++]=(uint8_t)('0'+value%10); value/=10; } while (value);
    for (size_t a=0,b=count-1; a<b; a++,b--) {
        uint8_t byte=digits[a]; digits[a]=digits[b]; digits[b]=byte;
    }
    *out=record_text(digits,count); out->said=0; return 1;
}
uint32_t record_surface_parts(RecordWorker *worker, const SemanticRecord *record, int32_t root, Ref *parts, uint32_t capacity){
    const SurfaceProgram *program=&worker->s->surface;
    if (!program->thing || program->linear || !record_field_matches(&record->n[root],program->tier)) return 0;
    size_t mark=scratch_mark(); SurfaceRows rows=rows_of(worker,record,root);
    Ref *tokens=scratch_alloc((size_t)rows.count*sizeof(Ref)), *bindings=scratch_alloc((size_t)rows.count*sizeof(Ref));
    Ref *components=scratch_alloc((size_t)rows.count*sizeof(Ref));
    uint32_t nt=0, nb=0, covered=0;
    for (uint32_t i=0; i<rows.count; i++) {
        SurfaceRow *row=&rows.rows[i];
        if (surface_row(row,&covered)) tokens[nt++]=record_text(row->form->val,row->form->vlen);
        if (row->kind!=SURFACE_RANGE) continue;
        uint32_t count=0;
        for (uint32_t j=i+1; j<rows.count && rows.rows[j].first<=row->last; j++) {
            SurfaceRow *component=&rows.rows[j]; if (component->kind!=SURFACE_WORD) continue;
            components[count++]=record_text(component->form->val,component->form->vlen);
        }
        if (count!=row->last-row->first+1) invalid_surface(worker,"range components are incomplete");
        Ref component_path=compose(components,count,ref_above(components,count));
        Ref pair[2]={record_text(row->form->val,row->form->vlen),component_path};
        bindings[nb++]=compose(pair,2,ref_above(pair,2));
    }
    uint32_t n=0;
    if (nt && n<capacity) parts[n++]=nt==1 ? tokens[0] : compose(tokens,nt,ref_above(tokens,nt));
    if (nb && n<capacity) parts[n++]=nb==1 ? bindings[0] : compose(bindings,nb,ref_above(bindings,nb));
    if ((nt!=0)+(nb!=0)>capacity) invalid_surface(worker,"record has no capacity for surface structure");
    scratch_restore(mark); return n;
}
