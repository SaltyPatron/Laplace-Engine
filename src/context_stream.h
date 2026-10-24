#ifndef LAPLACE_CONTEXT_STREAM_H
#define LAPLACE_CONTEXT_STREAM_H

#include "structure.h"

typedef struct ContextEvent { SField field; int event; struct ContextEvent *next; } ContextEvent;
struct SContext { ContextEvent *first, *last; SField *opened; uint32_t count; const SContext *parent; };

/* These snapshots own decoded fields, never Tree-sitter nodes or input bytes. */
SField s_context_field_copy(SayScratch *arena, const SField *field);
SContextStream *s_context_stream_open(void);
void s_context_stream_close(SContextStream *stream);
int s_context_stream_inherit(const SContextStream *stream, SContext *context);
int s_context_stream_commit(SContextStream *stream, const SContext *context);

#endif
