#include "weread_zip_stream.h"

#include <stdint.h>
#include <string.h>
#include <zlib.h>

static void put16(unsigned char *dst, unsigned value) {
    dst[0] = (unsigned char)value;
    dst[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *dst, uint32_t value) {
    put16(dst, value);
    put16(dst + 2, value >> 16);
}

bool wr_zip_append_stored_file(FILE *archive, FILE *central, FILE *source,
                               const char *name, size_t max_bytes,
                               void *scratch, size_t scratch_bytes) {
    if (!archive || !central || !source || !name || !scratch || !scratch_bytes ||
        scratch_bytes > UINT32_MAX || !name[0]) return false;
    size_t name_len = strlen(name);
    if (name_len > UINT16_MAX || max_bytes > UINT32_MAX) return false;
    long end = ftell(source);
    long offset = ftell(archive);
    if (end < 0 || offset < 0 || (uint64_t)end > max_bytes ||
        (uint64_t)offset > UINT32_MAX || fseek(source, 0, SEEK_SET)) return false;

    unsigned char local[30] = {0};
    put32(local, 0x04034b50);
    put16(local + 4, 20); /* minimum ZIP version to extract */
    put16(local + 6, 8); /* general-purpose bit 3: data descriptor follows */
    put16(local + 26, (unsigned)name_len);
    if (fwrite(local, 1, sizeof(local), archive) != sizeof(local) ||
        fwrite(name, 1, name_len, archive) != name_len) return false;

    uLong crc = crc32(0, Z_NULL, 0);
    for (long remaining = end; remaining > 0;) {
        size_t n = (uint64_t)remaining > scratch_bytes ? scratch_bytes : (size_t)remaining;
        if (fread(scratch, 1, n, source) != n) return false;
        crc = crc32(crc, scratch, (uInt)n);
        if (fwrite(scratch, 1, n, archive) != n) return false;
        remaining -= (long)n;
    }

    unsigned char descriptor[16] = {0};
    put32(descriptor, 0x08074b50);
    put32(descriptor + 4, (uint32_t)crc);
    put32(descriptor + 8, (uint32_t)end);
    put32(descriptor + 12, (uint32_t)end);
    if (fwrite(descriptor, 1, sizeof(descriptor), archive) != sizeof(descriptor)) return false;

    unsigned char record[46] = {0};
    put32(record, 0x02014b50);
    put16(record + 4, 20); /* made by */
    memcpy(record + 6, local + 4, 24);
    put32(record + 16, (uint32_t)crc);
    put32(record + 20, (uint32_t)end);
    put32(record + 24, (uint32_t)end);
    put32(record + 42, (uint32_t)offset);
    return fwrite(record, 1, sizeof(record), central) == sizeof(record) &&
           fwrite(name, 1, name_len, central) == name_len;
}
