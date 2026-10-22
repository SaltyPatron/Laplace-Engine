#ifndef LAPLACE_RECORD_WORKER_H
#define LAPLACE_RECORD_WORKER_H

/* A worker owns its record/output arenas. The source scheduler owns worker
 * arrays and transfers compiled operands/products; it never evaluates rules.
 * Bytecode is borrowed immutable state; operand views belong to one window. */
#include "recipe_program.h"

typedef struct { Ref *c; size_t n, cap; SayScratch *arena; } Refs;
/* A binding is a variable selected by a compiled name slot in a semantic
 * scope. The table stores references, not AST nodes, copied names or a second
 * parent/child tree. Multiple occurrences keep their source order. */
typedef struct { uint64_t key; uint32_t first, last; } BindingBucket;
typedef struct { uint32_t value, next; } BindingLink;
typedef struct {
    BindingBucket *buckets; BindingLink *links;
    size_t capacity, count, link_capacity, link_count;
} BindingMatrix;

/* Variables and composition operands of the compiled semantic program.
 * This is a selector binding store: it retains no native AST nodes or copied
 * packaging hierarchy, and has no first/last/next child links. */
#define BIND_INPUTS UINT32_MAX
typedef struct SemanticRecord SemanticRecord;
typedef struct TextFragment { const uint8_t *bytes; uint32_t length; struct TextFragment *next; } TextFragment;
typedef struct { TextFragment *first, *last; uint32_t length; } TextRun;
typedef struct {
    SemanticRecord *owner;
    const uint8_t *name, *val;
    uint32_t nlen, vlen, arity, extent, operand;
    int32_t parent;
    uint8_t kind, tier, join, trim, context, scalar;
    uint32_t dropped_first;
    TextRun *text;
} SemanticValue;
typedef struct {
    const uint8_t *name;
    uint64_t hash;
    uint32_t length, parent, all, nonempty, seen, next, scope_next;
} DroppedField;
struct SemanticRecord {
    SemanticValue *n; uint32_t count, capacity;
    BindingMatrix *bindings;
    DroppedField *dropped; uint32_t dropped_count, dropped_capacity, *dropped_buckets, dropped_bucket_count;
};

enum { RECORD_REFERENCE_CAPACITY = 64 };
static inline int32_t record_scope_end(const SemanticRecord *record, int32_t scope){ return (int32_t)record->n[scope].extent; }
typedef struct {
    const DiagnosticLabel *label;
    uint64_t hash, count;
    uint8_t a[48], b[40], a_length, b_length, has_b;
    char what;
} Obligation;
typedef struct { const uint8_t *bytes; uint64_t hash; uint32_t length; int32_t node, scope; uint16_t tier; } LocalKey;
typedef struct HwRec { int what, line; Ref thing; char *a, *b; } HwRec;     /* what: 1 a type, 2 a key of one, 3 a key naming what another names, 4 an edge */
typedef struct { const Recipe *r; const Say *s; Events ev; Refs things, meta; uint64_t open[256]; float er, ed, score; uint64_t ordinal; Ref fw, qualifier; int qualified; char opennm[16][64]; uint8_t open_length[16]; int nopen; uint64_t unknown; char unknm[8][96]; int nunk; Refs grp; Ref fname, fstem, file_scope;
                 Ref dependence; const lp_trust_class *dependence_class;
                 const SemanticRecord *ix_tree; uint32_t ix_count; LocalKey *local_keys; size_t local_capacity, local_count;
                 BindingMatrix bindings;
                 uint64_t blocked; uint8_t binding_phase, defer_missing, reserve_declarations;
                 SayScratch output;
                 const SemanticRecord *tc_tree; uint32_t tc_n; Ref *tc, *text_products; uint8_t *ts, *uk, *text_ready;
                 long selfcp;                                                /* the code point the part being read is, or -1 */
                 Ref fabout; int has_fabout;                                 /* what the page is about, where its about line names it */
                 Hw *hw; struct HwRec *hr; size_t nhr, chr; int worker; int32_t wide_of; KeyRank kr; int16_t *dm;
                 struct CSeen *cs; uint64_t cs_cap, cs_n, cs_epoch; int32_t rec;
                 Obligation obl[256]; uint16_t obligation_buckets[512]; int nobl; uint64_t obl_more;
                 uint8_t *lo; const SemanticRecord *lo_tree;     /* the parts of the record already left open, each counted once */
                 } RecordWorker;   /* obl: the obligations the reading left (no meaning: no claim), and values outside the road class named, by name */        /* cs: the claims the part being read has made, each once (claim) */          /* reading for the highway (laplace highway): what the part says of its types, in order */
typedef struct { uint32_t scope; } SemanticScope;
typedef struct {
    SemanticRecord record; BindingMatrix bindings; KeyRank rank; uint64_t ordinal;
    size_t things_at, things_count, meta_at, meta_count; uint8_t ready, reserve;
} PreparedRecord;
typedef struct {
    RecordWorker *sink; const Layout *layout; SemanticRecord record;
    SemanticScope *scopes, *context_scopes; uint32_t depth, capacity, context_depth, context_capacity; uint64_t ordinal;
    uint8_t merged_root, parent_fixed;
    PreparedRecord *prepared;
} RecordCollector;

void record_refs_push(Refs *a, const Ref *x);
void record_worker_outputs(RecordWorker *worker);
void record_event_push(RecordWorker *worker, const Event *event);
void record_workers_free(RecordWorker *sinks, size_t count);
void record_highway_entry(RecordWorker *k, int what, int line, const Ref *thing, const char *a, size_t al, const char *b, size_t bl);
void record_collect_diagnostics(const char *recipe, const RecordWorker *k);
void record_execute(RecordWorker *worker, const SemanticRecord *t, int32_t root, uint64_t ordinal, int bind_keys);
void record_prepared_clear(PreparedRecord *record);
void record_bind_keys(RecordWorker *worker, PreparedRecord *record, int final);
void record_bind_declarations(RecordWorker *worker, PreparedRecord *record);
int record_collect_fields(void *context, int event, const SField *field);
void record_merge(RecordWorker *result, const RecordWorker *worker);
int record_has_unbound_key(const Say *, const PreparedRecord *);

#endif
