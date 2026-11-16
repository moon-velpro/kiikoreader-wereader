#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Find an image by source basename in a bounded POSIX tar archive.
 * Result aliases the caller's archive and must not be freed separately.
 * Ambiguous repeated basenames and malformed headers fail closed. */
bool wr_tar_find_image(const void *archive, size_t bytes,
                       const char *source, size_t source_length,
                       const void **image, size_t *image_bytes);
