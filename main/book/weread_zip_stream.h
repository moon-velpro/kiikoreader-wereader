#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Append a stored (uncompressed) ZIP member from an existing FILE * with
 * bounded scratch space. Uses general-purpose bit 3 and a data descriptor,
 * so the source is read once, without staging the whole file in PSRAM.
 * Both the standard ZIP reader and our zip_reader.c support bit 3.
 * On failure the caller must discard the incomplete archive. */
bool wr_zip_append_stored_file(FILE *archive, FILE *central, FILE *source,
                               const char *name, size_t max_bytes,
                               void *scratch, size_t scratch_bytes);
