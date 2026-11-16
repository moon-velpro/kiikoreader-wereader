#pragma once
#include <stdbool.h>
#include <stdint.h>

/* UI-only coalescing: cache progress is measured in 5% buckets.
 * Completion is rendered by the status transition, never by a competing 100% frame. */
static inline bool weread_progress_should_redraw(unsigned shown_done, unsigned shown_total,
                                                  unsigned done, unsigned total, bool caching) {
    if (!caching) return done != shown_done || total != shown_total;
    if (total && done >= total) return false;
    if (shown_total != total) return true;
    if (!total || done <= shown_done) return false;
    unsigned old_bucket = (unsigned)(((uint64_t)shown_done * 20u) / total);
    unsigned new_bucket = (unsigned)(((uint64_t)done * 20u) / total);
    return new_bucket > old_bucket;
}
