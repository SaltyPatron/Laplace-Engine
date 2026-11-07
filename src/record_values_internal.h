#ifndef LAPLACE_RECORD_VALUES_INTERNAL_H
#define LAPLACE_RECORD_VALUES_INTERNAL_H

#include "record_values.h"
#include "record_surface.h"
#include "record_diagnostics.h"
#include "record_output.h"
#include "laplace/tier0.h"

enum { VALUE_PARTS_FLAT = 1, VALUE_PARTS_TUPLE = 2 };

#define BUSY_MAX 256
extern __thread int32_t lp_record_busy[BUSY_MAX];
extern __thread int lp_record_nbusy, lp_record_busy_base;
#define busy lp_record_busy
#define nbusy lp_record_nbusy
#define busy_base lp_record_busy_base

static inline int composing(int32_t g){ for (int i = busy_base; i < nbusy; i++) if (busy[i] == g) return 1; return 0; }

int lp_record_keyed(RecordWorker *sink, const SemanticRecord *tree, const TargetCode *target, int32_t from, const uint8_t *bytes, size_t length);
const uint8_t *lp_record_scoped_key(RecordWorker *k, const SemanticRecord *t, int32_t at,
                                    const char *under, unsigned radix, const uint8_t *raw, size_t *length, int depth);
int lp_record_compose_values(RecordWorker *k, const SemanticRecord *t, int32_t c, int depth, int flags, Ref *out);
KeyField lp_record_projected_field(RecordWorker *k, const SemanticRecord *t, int32_t c, int depth);
void lp_record_key_forms(const KSpec *spec, const uint8_t *value, size_t length, Keys *out, const SemanticRecord *tree, int32_t at);

#define keyed lp_record_keyed
#define scoped_key lp_record_scoped_key
#define compose_values lp_record_compose_values
#define projected_field lp_record_projected_field
#define record_key_forms lp_record_key_forms

#endif
