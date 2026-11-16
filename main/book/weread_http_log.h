#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Keep signed query parameters and fragments out of diagnostic logs. */
static inline void wr_http_log_target(const char *url, char *out, size_t capacity) {
    if (!out || !capacity) return;
    if (!url) url = "";

    const char *end = strpbrk(url, "?#");
    size_t length = end ? (size_t)(end - url) : strlen(url);
    size_t copied = length;
    bool truncated = false;
    if (copied >= capacity) {
        if (capacity > 4) {
            copied = capacity - 4;
            truncated = true;
        } else {
            copied = capacity - 1;
        }
    }

    for (size_t i = 0; i < copied; ++i) {
        unsigned char c = (unsigned char)url[i];
        out[i] = c >= 0x21 && c <= 0x7e && c != '"' && c != '\'' ? (char)c : '?';
    }
    if (truncated) {
        out[copied++] = '.';
        out[copied++] = '.';
        out[copied++] = '.';
    }
    out[copied] = 0;
}
