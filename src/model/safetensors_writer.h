#ifndef LAPLACE_SAFETENSORS_WRITER_H
#define LAPLACE_SAFETENSORS_WRITER_H

#include "laplace/tensor.h"
#include <stddef.h>
#include <stdint.h>

typedef struct lp_st_writer lp_st_writer;

lp_st_writer *lp_st_writer_create(const char *output_path);
void lp_st_writer_free(lp_st_writer *w);

int lp_st_writer_add_meta(lp_st_writer *w, const char *key, const char *value);
int lp_st_writer_add_tensor(lp_st_writer *w,
                            const char *name,
                            lp_dtype dtype,
                            const uint64_t *dims,
                            uint32_t ndim,
                            const void *data);
int lp_st_writer_finalize(lp_st_writer *w);

#endif
