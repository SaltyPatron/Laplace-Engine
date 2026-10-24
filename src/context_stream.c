/* Active lexical headers survive disposable packaging windows. Two reusable
 * arenas separate the immutable worker snapshot from the next window's state. */
#include "context_stream.h"
#include <stdlib.h>
#include <string.h>

struct SContextStream { SayScratch arena[2]; const SContext *active; unsigned bank; };

SField s_context_field_copy(SayScratch *arena, const SField *field){
    SField copy = *field; copy.source = (TSNode){0}; copy.flags |= S_FIELD_CONTEXT;
    if (field->name_length) {
        uint8_t *name = arena_alloc(arena, field->name_length); memcpy(name, field->name, field->name_length); copy.name = name;
    }
    if (field->value_length) {
        uint8_t *value = arena_alloc(arena, field->value_length); memcpy(value, field->value, field->value_length); copy.value = value;
    }
    if (field->bindings) {
        size_t size = sizeof(SNameBindings) + (size_t)field->bindings->count * sizeof(uint32_t);
        SNameBindings *bindings = arena_alloc(arena, size); memcpy(bindings, field->bindings, size); copy.bindings = bindings;
    }
    return copy;
}

SContextStream *s_context_stream_open(void){ return calloc(1, sizeof(SContextStream)); }
void s_context_stream_close(SContextStream *stream){
    if (!stream) return;
    arena_release(&stream->arena[0]); arena_release(&stream->arena[1]); free(stream);
}

int s_context_stream_inherit(const SContextStream *stream, SContext *context){
    if (!stream) return 1;
    uint8_t previous = 0;
    for (uint32_t i = 0; i < context->count; i++) {
        uint8_t level = context->opened[i].context_level;
        if (!level || level <= previous) return 0;
        previous = level;
    }
    const SContext *parent = stream->active;
    if (context->count) {
        uint8_t first = context->opened[0].context_level;
        while (parent && parent->opened[0].context_level >= first) parent = parent->parent;
    }
    context->parent = parent; return 1;
}

int s_context_stream_commit(SContextStream *stream, const SContext *context){
    if (!stream || !context) return 1;
    const SContext *chain[256]; unsigned count = 0;
    for (const SContext *part = context; part; part = part->parent) {
        if (count == 256) return 0;
        chain[count++] = part;
    }
    unsigned bank = stream->bank ^ 1u;
    SayScratch *arena = &stream->arena[bank]; arena->used = 0;
    SContext *active = NULL; uint8_t previous = 0;
    while (count) {
        const SContext *part = chain[--count]; uint32_t openings = 0;
        for (const ContextEvent *event = part->first; event; event = event->next) {
            if (event->event == S_OPEN && event->field.context_level) {
                uint8_t level = event->field.context_level;
                if (level <= previous) return 0;
                SContext *frame = arena_alloc(arena, sizeof *frame);
                *frame = (SContext){.parent=active, .count=1};
                frame->opened = arena_alloc(arena, sizeof *frame->opened);
                *frame->opened = s_context_field_copy(arena, &event->field);
                active = frame; previous = level; openings++;
            }
            if (!active) return 0;
            ContextEvent *copy = arena_alloc(arena, sizeof *copy);
            *copy = (ContextEvent){.field=s_context_field_copy(arena, &event->field), .event=event->event};
            if (active->last) active->last->next = copy; else active->first = copy;
            active->last = copy;
            if (event == part->last) break;
        }
        if (openings != part->count) return 0;
    }
    stream->active = active; stream->bank = bank; return 1;
}
