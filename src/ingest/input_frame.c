#include "input_frame.h"
#include "laplace/os.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int input_frame_encoding(InputFrame *frame, const char *encoding){
    static const struct { const char *name; InputFrame frame; } encodings[] = {
        {"u8", {1, 0}}, {"u16le", {2, 0}}, {"u16be", {2, 1}},
        {"u32le", {4, 0}}, {"u32be", {4, 1}},
        {"u64le", {8, 0}}, {"u64be", {8, 1}}
    };
    if (!frame || !encoding) return 0;
    for (size_t i = 0; i < sizeof encodings / sizeof encodings[0]; i++)
        if (!strcmp(encoding, encodings[i].name)) { *frame = encodings[i].frame; return 1; }
    return 0;
}

uint8_t *input_frame_read(const InputFrame *frame, const char *path, size_t *length){
    if (!frame || !length || (frame->width != 1 && frame->width != 2 && frame->width != 4 && frame->width != 8) || frame->big_endian > 1) {
        errno = EINVAL; return NULL;
    }
    FILE *file = fopen(path, "rb"); if (!file) return NULL;
    uint8_t prefix[8], *bytes = NULL; int failure = 0;
    int64_t file_size = os_fsize(file);
    if (file_size < 0) failure = errno ? errno : EIO;
    if (!failure && fread(prefix, 1, frame->width, file) != frame->width) failure = ferror(file) ? EIO : EINVAL;
    uint64_t count = 0;
    if (!failure) {
        for (unsigned i = 0; i < frame->width; i++) {
            unsigned shift = 8 * (frame->big_endian ? frame->width - 1 - i : i);
            count |= (uint64_t)prefix[i] << shift;
        }
        if (file_size < (int64_t)frame->width || count > (uint64_t)file_size - frame->width) failure = EINVAL;
        else if (count > UINT32_MAX || count >= SIZE_MAX) failure = EOVERFLOW;
    }
    if (!failure) {
        bytes = malloc((size_t)count + 1);
        if (!bytes) failure = ENOMEM;
        else if (fread(bytes, 1, (size_t)count, file) != count) failure = ferror(file) ? EIO : EINVAL;
    }
    if (fclose(file) && !failure) failure = errno;
    if (failure) { free(bytes); errno = failure; return NULL; }
    bytes[count] = 0; *length = (size_t)count; return bytes;
}
