#ifndef LAPLACE_FILE_METADATA_H
#define LAPLACE_FILE_METADATA_H

#include "engine.h"

/* Product content wildcards do not override OS packaging metadata. */
typedef enum { FILE_METADATA_UNSET, FILE_METADATA_OMIT, FILE_METADATA_KEEP, FILE_METADATA_CONTENT } FileMetadataPolicy;
FileMetadataPolicy recipe_file_metadata_policy(const Recipe *, const char *field);

#endif
