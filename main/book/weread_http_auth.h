#pragma once

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    char tail[64];
    size_t tail_length;
} wr_http_auth_scan_t;

static inline const char *wr_http_find_bytes(const char *data, size_t length,
                                              const char *needle, size_t needle_length) {
    if (!data || needle_length > length) return NULL;
    for (size_t i = 0; i + needle_length <= length; ++i)
        if (!memcmp(data + i, needle, needle_length)) return data + i;
    return NULL;
}

static inline bool wr_http_body_session_expired_n(const char *body, size_t length) {
    if (!body || !length) return false;
    const char *end = body + length;
    const char *key = wr_http_find_bytes(body, length, "\"errcode\"", 9);
    const char *alternate = wr_http_find_bytes(body, length, "\"errCode\"", 9);
    if (!key || (alternate && alternate < key)) key = alternate;
    if (!key) return false;
    const char *value = memchr(key, ':', (size_t)(end - key));
    if (!value) return false;
    ++value;
    while (value < end && isspace((unsigned char)*value)) ++value;
    bool quoted = value < end && *value == '"';
    if (quoted) ++value;
    if ((size_t)(end - value) < 5 || memcmp(value, "-2012", 5)) return false;
    value += 5;
    return quoted ? (value < end && *value == '"') :
        (value == end || *value == ',' || *value == '}' || isspace((unsigned char)*value));
}

static inline bool wr_http_body_session_expired(const char *body) {
    return body && wr_http_body_session_expired_n(body, strlen(body));
}

/* Reader responses are streamed through a reused buffer. Preserve a short tail
 * so the errcode key/value can be recognized when split across read boundaries. */
static inline bool wr_http_auth_scan_chunk(wr_http_auth_scan_t *scan,
                                           const char *data, size_t length) {
    if (!scan || !data || !length) return false;
    if (wr_http_body_session_expired_n(data, length)) return true;

    char prefix[128];
    size_t tail = scan->tail_length;
    if (tail > sizeof(prefix)) tail = sizeof(prefix);
    size_t take = length;
    if (take > sizeof(prefix) - tail) take = sizeof(prefix) - tail;
    memcpy(prefix, scan->tail, tail);
    memcpy(prefix + tail, data, take);
    const size_t combined_length = tail + take;
    if (wr_http_body_session_expired_n(prefix, combined_length)) return true;

    /* Keep the tail of the combined previous+current data. Keeping only the
     * current chunk loses a split key when its colon and value arrive later. */
    size_t keep;
    const char *source;
    if (length > take) {
        keep = length < sizeof(scan->tail) ? length : sizeof(scan->tail);
        source = data + length - keep;
    } else {
        keep = combined_length < sizeof(scan->tail) ? combined_length : sizeof(scan->tail);
        source = prefix + combined_length - keep;
    }
    if (keep) memcpy(scan->tail, source, keep);
    scan->tail_length = keep;
    return false;
}
