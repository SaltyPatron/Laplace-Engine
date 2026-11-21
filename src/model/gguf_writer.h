#ifndef LAPLACE_GGUF_WRITER_H
#define LAPLACE_GGUF_WRITER_H

#include "laplace/tensor.h"
#include <stddef.h>
#include <stdint.h>

typedef struct lp_gguf_writer lp_gguf_writer;

lp_gguf_writer *lp_gguf_writer_create(const char *output_path);
void lp_gguf_writer_free(lp_gguf_writer *w);

int lp_gguf_writer_add_str(lp_gguf_writer *w, const char *key, const char *value);
int lp_gguf_writer_add_u32(lp_gguf_writer *w, const char *key, uint32_t value);
int lp_gguf_writer_add_u64(lp_gguf_writer *w, const char *key, uint64_t value);
int lp_gguf_writer_add_f32(lp_gguf_writer *w, const char *key, float value);
int lp_gguf_writer_add_bool(lp_gguf_writer *w, const char *key, int value);

int lp_gguf_writer_add_str_array(lp_gguf_writer *w, const char *key,
                                 const char *const *values, size_t count);
int lp_gguf_writer_add_u8_array(lp_gguf_writer *w, const char *key,
                                const uint8_t *values, size_t count);
int lp_gguf_writer_add_f32_array(lp_gguf_writer *w, const char *key,
                                 const float *values, size_t count);
int lp_gguf_writer_add_i32_array(lp_gguf_writer *w, const char *key,
                                 const int32_t *values, size_t count);

int lp_gguf_writer_add_tensor(lp_gguf_writer *w,
                              const char *name,
                              lp_dtype dtype,
                              const uint64_t *dims,
                              uint32_t ndim,
                              const void *data);

int lp_gguf_writer_finalize(lp_gguf_writer *w);

#endif
