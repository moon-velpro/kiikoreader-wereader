/* Bounded body sink shared by PSRAM HTTP decoding and host regressions.
 * No content-specific assumptions: the caller owns the resize allocator. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void *(*wr_body_buffer_resize_fn)(void *old, size_t bytes, void *context);

typedef struct {
    char *data;
    size_t used, capacity, limit;
    wr_body_buffer_resize_fn resize;
    void *resize_context;
    bool failed;
} wr_body_buffer_t;

/* Returns true to abort decoding if the decoded output exceeds the limit
 * or its bounded allocation cannot grow. Always reserve one NUL byte. */
static inline bool wr_body_buffer_write(const char *bytes, size_t n, void *arg) {
    wr_body_buffer_t *sink = arg;
    if (!sink || !sink->resize || (n && !bytes) || sink->limit == SIZE_MAX ||
        sink->used > sink->limit || n > sink->limit - sink->used) {
        if (sink) sink->failed = true;
        return true;
    }
    if (!n) return false;
    size_t need = sink->used + n + 1;
    if (need > sink->capacity) {
        size_t max_capacity = sink->limit + 1;
        size_t next = sink->capacity ? sink->capacity :
                      (max_capacity < 1024 ? max_capacity : 1024);
        while (next < need) {
            next = next > max_capacity / 2 ? max_capacity : next * 2;
        }
        if (next < need) {
            sink->failed = true;
            return true;
        }
        char *grown = sink->resize(sink->data, next, sink->resize_context);
        if (!grown) {
            sink->failed = true;
            return true;
        }
        sink->data = grown;
        sink->capacity = next;
    }
    memcpy(sink->data + sink->used, bytes, n);
    sink->used += n;
    sink->data[sink->used] = 0;
    return false;
}
