/* Exercise the actual file reader, including sparse multi-gigabyte tails.
 * No grammar, database, or model runtime is needed for binary framing. */
#define _FILE_OFFSET_BITS 64
#include "input_frame.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_frame(const char *path, InputFrame frame, uint64_t count, const char *body){
    FILE *file = fopen(path, "wb"); assert(file);
    for (unsigned i = 0; i < frame.width; i++) {
        unsigned shift = 8 * (frame.big_endian ? frame.width - 1 - i : i);
        assert(fputc((int)((count >> shift) & 255), file) != EOF);
    }
    assert(fwrite(body, 1, strlen(body), file) == strlen(body)); assert(!fclose(file));
}
static void refuses(const char *path, InputFrame frame, int error){
    size_t length = 991; errno = 0;
    uint8_t *bytes = input_frame_read(&frame, path, &length);
    assert(!bytes && errno == error && length == 991);
}
int main(void){
    char path[] = "/tmp/laplace-input-frame.XXXXXX";
    int descriptor = mkstemp(path); assert(descriptor >= 0); close(descriptor);
    const char *encodings[] = {"u8", "u16le", "u16be", "u32le", "u32be", "u64le", "u64be"};
    for (size_t i = 0; i < sizeof encodings / sizeof encodings[0]; i++) {
        InputFrame frame; assert(input_frame_encoding(&frame, encodings[i]));
        write_frame(path, frame, 7, "{\"x\":1}\xff\x00");
        size_t length = 0; uint8_t *bytes = input_frame_read(&frame, path, &length);
        assert(bytes && length == 7 && !memcmp(bytes, "{\"x\":1}", 7) && !bytes[7]); free(bytes);
        write_frame(path, frame, 8, "short"); refuses(path, frame, EINVAL);
        write_frame(path, frame, 0, "unread payload");
        bytes = input_frame_read(&frame, path, &length); assert(bytes && !length && !bytes[0]); free(bytes);
    }
    /* Exercise all eight prefix bytes, independently of host byte order. */
    InputFrame little = {8, 0}, big = {8, 1};
    write_frame(path, little, UINT64_MAX, ""); refuses(path, little, EINVAL);
    write_frame(path, big, UINT64_MAX, ""); refuses(path, big, EINVAL);
    write_frame(path, little, 1, "");
    descriptor = open(path, O_WRONLY); assert(descriptor >= 0);
    assert(!ftruncate(descriptor, 3)); close(descriptor); refuses(path, little, EINVAL);
    write_frame(path, little, 7, "{\"x\":1}");
    descriptor = open(path, O_WRONLY); assert(descriptor >= 0);
    assert(!ftruncate(descriptor, (off_t)8 * 1024 * 1024 * 1024)); close(descriptor);
    size_t length; uint8_t *bytes = input_frame_read(&little, path, &length);
    assert(bytes && length == 7 && !memcmp(bytes, "{\"x\":1}", 7)); free(bytes);
    write_frame(path, little, (uint64_t)UINT32_MAX + 1, "");
    descriptor = open(path, O_WRONLY); assert(descriptor >= 0);
    assert(!ftruncate(descriptor, (off_t)UINT32_MAX + 9)); close(descriptor);
    refuses(path, little, EOVERFLOW);
    refuses(path, (InputFrame){3, 0}, EINVAL);
    refuses(path, (InputFrame){8, 2}, EINVAL);
    InputFrame unchanged = {0}; assert(!input_frame_encoding(&unchanged, "native") && !unchanged.width);
    assert(!unlink(path)); puts("input-frame: encodings, bounds, truncation, and sparse payload isolation passed");
    return 0;
}
