#ifndef LAPLACE_RECORD_SURFACE_H
#define LAPLACE_RECORD_SURFACE_H

/* Compiled indexed-sequence composition over prepared semantic operands.
 * Packaging grammars and corpus policies remain outside this module. */
#include "record_worker.h"

int record_surface_text(RecordWorker *, const SemanticRecord *, int32_t, Ref *);
uint32_t record_surface_parts(RecordWorker *, const SemanticRecord *, int32_t, Ref *, uint32_t);

#endif
