#include "weread_tar.h"

#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

static const char *tar_tag_end(const char *start) {
    char quote = 0;
    for (const char *p = start; *p; ++p) {
        if (quote) { if (*p == quote) quote = 0; }
        else if (*p == '\'' || *p == '"') quote = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

static const char *tar_image_src(const char *p, const char *end, size_t *length) {
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) ++p;
        const char *key = p;
        while (p < end && !strchr("= \r\n\t", *p)) ++p;
        size_t key_size = (size_t)(p - key);
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p == end || *p != '=') continue;
        do { ++p; } while (p < end && isspace((unsigned char)*p));
        if (p == end) break;
        char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
        const char *value = p;
        while (p < end && (quote ? *p != quote : !isspace((unsigned char)*p))) ++p;
        if ((key_size == 3 && !strncasecmp(key, "src", 3)) ||
            (key_size == 10 && !strncasecmp(key, "xlink:href", 10))) {
            *length = (size_t)(p - value);
            return value;
        }
        if (quote && p < end) ++p;
    }
    return NULL;
}

static bool tar_image_url(const char *src, size_t length) {
    static const char *const prefixes[] = {
        "https://res.weread.qq.com/wrepub/",
        "http://res.weread.qq.com/wrepub/",
        "//res.weread.qq.com/wrepub/"
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t n = strlen(prefixes[i]);
        if (length >= n && !strncasecmp(src, prefixes[i], n)) return true;
    }
    return false;
}

bool wr_tar_chapter_has_images(const char *xhtml) {
    if (!xhtml) return false;
    const char *limit = xhtml + strlen(xhtml);
    for (const char *p = xhtml; p < limit;) {
        if (*p != '<') { ++p; continue; }
        size_t remaining = (size_t)(limit - p);
        bool image = remaining >= 4 && !strncasecmp(p, "<img", 4) &&
                     (p[4] == '>' || isspace((unsigned char)p[4]));
        bool svg_image = remaining >= 6 && !strncasecmp(p, "<image", 6) &&
                         isspace((unsigned char)p[6]);
        if (!image && !svg_image) { ++p; continue; }
        const char *end = tar_tag_end(p);
        if (!end) return false;
        size_t length = 0;
        const char *src = tar_image_src(p + (image ? 4 : 6), end, &length);
        if (src && tar_image_url(src, length)) return true;
        p = end + 1;
    }
    return false;
}

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
