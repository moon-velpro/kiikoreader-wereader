#include "weread_tar.h"

#include <stdint.h>
#include <string.h>

static bool tar_octal(const unsigned char *field, size_t length, size_t *value) {
    size_t n = 0;
    bool digit = false, ended = false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = field[i];
        if (c == 0 || c == ' ') {
            if (digit) ended = true;
            continue;
        }
        if (ended || c < '0' || c > '7' || n > (SIZE_MAX - 7) / 8) return false;
        n = n * 8 + (unsigned)(c - '0');
        digit = true;
    }
    if (!digit) return false;
    *value = n;
    return true;
}

static bool tar_header_valid(const unsigned char *header) {
    size_t expected = 0;
    if (!tar_octal(header + 148, 8, &expected)) return false;
    size_t sum = 0;
    for (size_t i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? (unsigned char)' ' : header[i];
    return sum == expected;
}

static bool tar_reject(const void **image, size_t *image_bytes) {
    *image = NULL;
    *image_bytes = 0;
    return false;
}

bool wr_tar_find_image(const void *archive, size_t bytes,
                       const char *source, size_t source_length,
                       const void **image, size_t *image_bytes) {
    if (!archive || !source || !image || !image_bytes) return false;
    *image = NULL;
    *image_bytes = 0;
    size_t end = 0;
    while (end < source_length && source[end] && source[end] != '?' && source[end] != '#') ++end;
    size_t first = end;
    while (first && source[first - 1] != '/') --first;
    if (first == end) return false;
    const unsigned char *p = archive;
    bool matched = false;
    for (size_t off = 0; off < bytes;) {
        if (bytes - off < 512) return tar_reject(image, image_bytes);
        const unsigned char *head = p + off;
        bool zero = true;
        for (unsigned i = 0; i < 512; ++i) if (head[i]) { zero = false; break; }
        if (zero) break;
        size_t size = 0;
        if (!tar_header_valid(head) ||
            !tar_octal(head + 124, 12, &size) ||
            size > bytes - off - 512 || size > SIZE_MAX - 511)
            return tar_reject(image, image_bytes);
        size_t padded = (size + 511) & ~(size_t)511;
        if (padded > bytes - off - 512) return tar_reject(image, image_bytes);
        size_t nl = 0;
        while (nl < 100 && head[nl]) ++nl;
        const unsigned char *basename = head;
        size_t basename_size = nl;
        for (size_t i = 0; i < nl; ++i) {
            if (head[i] == '/') {
                basename = head + i + 1;
                basename_size = nl - i - 1;
            }
        }
        if ((head[156] == '0' || head[156] == 0) && size &&
            basename_size == end - first &&
            !memcmp(basename, source + first, basename_size)) {
            if (matched) return tar_reject(image, image_bytes); /* basename collision: never choose blindly */
            matched = true;
            *image = p + off + 512;
            *image_bytes = size;
        }
        off += 512 + padded;
    }
    return matched;
}
