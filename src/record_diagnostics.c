/* Reports unresolved semantics without minting substitute claims. */
#define _GNU_SOURCE
#include "record_diagnostics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const DiagnosticLabel record_diagnostic_labels[DIAG_COUNT] = {
    {"attest", 6, UINT64_C(0x907bd39a08f34aac)},
    {"holds", 5, UINT64_C(0x7477287ad37c50f1)},
    {"relate", 6, UINT64_C(0xb279534d85fc01cc)},
    {"line :: predicate", 17, UINT64_C(0xb638b662bd815814)},
    {"line", 4, UINT64_C(0xbf4ba5ad694f5907)},
    {"no list in the highway:", 23, UINT64_C(0x9c679eb7e39af651)},
    {"value equal to its thing:", 25, UINT64_C(0x50c83810e5074575)},
    {"a key no list of the highway holds, left out of the path:", 57, UINT64_C(0xb5c022f381bb9b49)}
};

static struct { char recipe[64], name[160]; uint64_t n; char what; } *OBL; static size_t NOBL, COBL; static uint64_t OBL_MORE;

typedef struct {
    const DiagnosticLabel *label;
    const uint8_t *a, *b;
    uint8_t a_length, b_length, has_b;
} ObligationView;

void record_note_open_name(RecordWorker *sink, const uint8_t *name, uint32_t length, uint64_t count){
    if (!count) return;
    sink->open[0] += count;
    for (int i = 0; i < sink->nopen; i++) if (sink->open_length[i] == length && (!length || !memcmp(sink->opennm[i], name, length))) return;
    if (sink->nopen < 16) {
        size_t n = length < 63 ? length : 63; if (n) memcpy(sink->opennm[sink->nopen], name, n);
        sink->opennm[sink->nopen][n] = 0; sink->open_length[sink->nopen++] = (uint8_t)n;
    }
}

void record_note_unhandled(RecordWorker *sink, const SemanticValue *value){
    const SemanticRecord *record = sink->lo_tree;
    if (record && sink->lo && value >= record->n && value < record->n + record->count) {
        size_t index = (size_t)(value - record->n); if (sink->lo[index]) return; sink->lo[index] = 1;
    }
    record_note_open_name(sink, value->name, value->nlen, 1);
}

void record_note_dropped(RecordWorker *sink, const SemanticRecord *record, uint32_t scope, int all){
    for (uint32_t at = record->n[scope].dropped_first; at; at = record->dropped[at - 1].scope_next) {
        DroppedField *field = &record->dropped[at - 1]; uint32_t count = all ? field->all : field->nonempty;
        if (count > field->seen) { record_note_open_name(sink, field->name, field->length, count - field->seen); field->seen = count; }
    }
}

int record_field_unmentioned(const Say *s, const SemanticValue *x){
    if (!s->said || !x->nlen || x->nlen > 62) return 0;
    return !recipe_program_mentions(s->paths, x->name, x->nlen);
}

static unsigned diagnostic_length(const ObligationView *view){
    return view->label->length + 1u + view->a_length + (view->has_b ? 1u + view->b_length : 0u);
}

static uint8_t diagnostic_byte(const ObligationView *view, unsigned position){
    if (position < view->label->length) return (uint8_t)view->label->text[position];
    position -= view->label->length;
    if (!position--) return ' ';
    if (position < view->a_length) return view->a[position];
    position -= view->a_length;
    if (!position--) return '.';
    return view->b[position];
}

static int obligation_equal(const Obligation *entry, const ObligationView *view){
    if (entry->label->length == view->label->length && entry->a_length == view->a_length &&
        entry->b_length == view->b_length && entry->has_b == view->has_b &&
        (entry->label == view->label || !memcmp(entry->label->text, view->label->text, view->label->length)) &&
        (!view->a_length || !memcmp(entry->a, view->a, view->a_length)) &&
        (!view->b_length || !memcmp(entry->b, view->b, view->b_length))) return 1;
    ObligationView stored = {entry->label, entry->a, entry->b, entry->a_length, entry->b_length, entry->has_b};
    unsigned length = diagnostic_length(view);
    if (length != diagnostic_length(&stored)) return 0;
    for (unsigned i = 0; i < length; i++) if (diagnostic_byte(view, i) != diagnostic_byte(&stored, i)) return 0;
    return 1;
}

void record_obligation_add(RecordWorker *sink, char what, const DiagnosticLabel *label, const uint8_t *a, size_t a_length,
                           const uint8_t *b, size_t b_length, uint64_t count){
    ObligationView view = {label, a, b, (uint8_t)(a_length < 48 ? a_length : 48),
                           (uint8_t)(b ? (b_length < 40 ? b_length : 40) : 0), (uint8_t)(b != NULL)};
    uint64_t hash = (label->hash ^ ' ') * NAME_HASH_PRIME;
    for (unsigned i = 0; i < view.a_length; i++) hash = (hash ^ a[i]) * NAME_HASH_PRIME;
    if (view.has_b) {
        hash = (hash ^ '.') * NAME_HASH_PRIME;
        for (unsigned i = 0; i < view.b_length; i++) hash = (hash ^ b[i]) * NAME_HASH_PRIME;
    }
    hash ^= (uint64_t)(unsigned char)what * UINT64_C(0x9e3779b97f4a7c15);
    size_t bucket = (hash ^ (hash >> 32)) & 511u;
    while (sink->obligation_buckets[bucket]) {
        Obligation *entry = &sink->obl[sink->obligation_buckets[bucket] - 1];
        if (entry->hash == hash && entry->what == what && obligation_equal(entry, &view)) { entry->count += count; return; }
        bucket = (bucket + 1) & 511u;
    }
    if (sink->nobl == 256) { sink->obl_more += count; return; }
    Obligation *entry = &sink->obl[sink->nobl]; *entry = (Obligation){.label=label, .hash=hash, .count=count,
        .a_length=view.a_length, .b_length=view.b_length, .has_b=view.has_b, .what=what};
    if (view.a_length) memcpy(entry->a, a, view.a_length);
    if (view.b_length) memcpy(entry->b, b, view.b_length);
    sink->obligation_buckets[bucket] = (uint16_t)++sink->nobl;
}

void record_obligation_note(RecordWorker *sink, char what, const DiagnosticLabel *label, const uint8_t *a, size_t a_length,
                            const uint8_t *b, size_t b_length){
    record_obligation_add(sink, what, label, a, a_length, b, b_length, 1);
}

void record_obligation_node(RecordWorker *sink, const DiagnosticLabel *label, const SemanticValue *node){
    record_obligation_note(sink, 'o', label, node->name, node->nlen, NULL, 0);
}

static void format_obligation(const Obligation *entry, char text[160]){
    size_t used = entry->label->length;
    memcpy(text, entry->label->text, used); text[used++] = ' ';
    if (entry->a_length) { memcpy(text + used, entry->a, entry->a_length); used += entry->a_length; }
    if (entry->has_b) { text[used++] = '.'; if (entry->b_length) { memcpy(text + used, entry->b, entry->b_length); used += entry->b_length; } }
    text[used] = 0;
}

void record_note_road_value(RecordWorker *k, const Dis *m, const Ref *v){
    if (!m->arg[0] || !HW) return; const lp_list *L = lp_highway_list(HW, m->arg);
    if (!L) { record_obligation_note(k, 'm', &record_diagnostic_labels[DIAG_NO_LIST], (const uint8_t *)m->arg, m->arg_length, NULL, 0); return; }
    if (lp_highway_slot(HW, L, &v->id) < 0) record_obligation_note(k, 'm', m->road_label, (const uint8_t *)m->name, m->name_length, NULL, 0);
}

void record_statement_count(RecordWorker *k, const Dis *m){
    record_obligation_note(k, 's', m->statement_label, (const uint8_t *)m->name, m->name_length, NULL, 0); }

void record_collect_diagnostics(const char *recipe, const RecordWorker *k){
    #pragma omp critical(laplace_obligations)
    { OBL_MORE += k->obl_more;
      for (int i = 0; i < k->nobl; i++) { size_t j = 0; char name[160]; format_obligation(&k->obl[i], name);
          for (; j < NOBL; j++) if (OBL[j].what == k->obl[i].what && !strcmp(OBL[j].recipe, recipe) && !strcmp(OBL[j].name, name)) break;
          if (j == NOBL) { if (NOBL == COBL) { COBL = COBL ? COBL * 2 : 64; OBL = xrealloc(OBL, sizeof *OBL * COBL); }
              snprintf(OBL[j].recipe, sizeof OBL[j].recipe, "%s", recipe); snprintf(OBL[j].name, sizeof OBL[j].name, "%s", name); OBL[j].n = 0; OBL[j].what = k->obl[i].what; NOBL++; }
          OBL[j].n += k->obl[i].count; } }
}

static int obl_cmp(const void *x, const void *y){ const char *a = (const char *)x, *b = (const char *)y; int c = strcmp(a, b); return c ? c : strcmp(a + 64, b + 64); }

void say_obligations(FILE *o){
    uint64_t n = OBL_MORE, m = 0, sd = 0; for (size_t i = 0; i < NOBL; i++) { if (OBL[i].what == 'o') n += OBL[i].n; else if (OBL[i].what == 'm') m += OBL[i].n; else sd += OBL[i].n; }
    if (NOBL) qsort(OBL, NOBL, sizeof *OBL, obl_cmp);
    if (sd) { fprintf(o, "\n== statements, by the meaning the recipe gives each part (value LIST NAME, relation \"TEXT\" NAME): %'llu\n", (unsigned long long)sd);
        for (size_t i = 0; i < NOBL; i++) if (OBL[i].what == 's') fprintf(o, "   %-22s %-80s %'14llu\n", OBL[i].recipe, OBL[i].name, (unsigned long long)OBL[i].n); }
    fprintf(o, "\n== unresolved obligations: %'llu parts a claim would name have no meaning the recipe gives, so no claim is made of them (Laplace-Engine#32)\n", (unsigned long long)n);
    for (size_t i = 0; i < NOBL; i++) if (OBL[i].what == 'o') fprintf(o, "   %-22s %-80s %'14llu\n", OBL[i].recipe, OBL[i].name, (unsigned long long)OBL[i].n);
    if (OBL_MORE) fprintf(o, "   of them %'llu under names past the 256 a part keeps\n", (unsigned long long)OBL_MORE);
    if (m) { fprintf(o, "   values said as a road class's that the highway's list does not hold (claimed as written; no slot): %'llu\n", (unsigned long long)m);
        for (size_t i = 0; i < NOBL; i++) if (OBL[i].what == 'm') fprintf(o, "   %-22s %-80s %'14llu\n", OBL[i].recipe, OBL[i].name, (unsigned long long)OBL[i].n); }
    free(OBL); OBL = NULL; NOBL = COBL = 0; OBL_MORE = 0;
}

void record_merge_diagnostics(RecordWorker *result, const RecordWorker *worker){
    for (int i = 0; i < 256; i++) result->open[i] += worker->open[i];
    for (int i = 0; i < worker->nopen; i++) {
        int duplicate = 0;
        for (int j = 0; j < result->nopen && !duplicate; j++)
            duplicate = result->open_length[j] == worker->open_length[i] &&
                !memcmp(result->opennm[j], worker->opennm[i], worker->open_length[i]);
        if (!duplicate && result->nopen < 16) {
            memcpy(result->opennm[result->nopen], worker->opennm[i], sizeof result->opennm[0]);
            result->open_length[result->nopen++] = worker->open_length[i];
        }
    }
    result->unknown += worker->unknown;
    for (int i = 0; i < worker->nunk && result->nunk < 8; i++)
        memcpy(result->unknm[result->nunk++], worker->unknm[i], sizeof result->unknm[0]);
    result->obl_more += worker->obl_more;
    for (int i = 0; i < worker->nobl; i++) {
        const Obligation *entry = &worker->obl[i];
        record_obligation_add(result, entry->what, entry->label, entry->a, entry->a_length,
                       entry->has_b ? entry->b : NULL, entry->b_length, entry->count);
    }
}
