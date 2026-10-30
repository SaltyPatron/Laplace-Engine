/* A recipe-selected, length-prefixed byte section, before its grammar runs.
 * Framing knows byte encodings only; it knows no file format or source. */
#ifndef LAPLACE_INPUT_FRAME_H
#define LAPLACE_INPUT_FRAME_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t width, big_endian; } InputFrame;
int input_frame_encoding(InputFrame *frame, const char *encoding);
/* Read only the prefix and the section it declares. The remaining file is
 * neither parsed nor recorded. Reject a section beyond the held file's size
 * or Tree-sitter's byte address space before allocating its buffer. */
uint8_t *input_frame_read(const InputFrame *frame, const char *path, size_t *length);
#endif
