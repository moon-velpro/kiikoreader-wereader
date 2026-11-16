#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <zlib.h>

/* Decode the streamed Reader response without buffering the full HTML page.
 * The sink may stop once its required fields have been found. */
typedef bool (*wr_body_sink_fn)(const char *data, size_t length, void *user);

typedef struct {
    z_stream z;
    wr_body_sink_fn sink;
    void *user;
    size_t decoded_bytes;
    size_t decoded_limit;
    unsigned char prefix[2];
    unsigned char output[2048];
    unsigned prefix_count;
    bool detected;
    bool compressed;
    bool inflater_ready;
    bool gzip_complete;
} wr_body_decoder_t;

void wr_body_decoder_init(wr_body_decoder_t *decoder, wr_body_sink_fn sink, void *user,
                          size_t decoded_limit, alloc_func alloc, free_func dealloc);
/* 1 = sink has all fields; 0 = continue; -1 = invalid or oversized body. */
int wr_body_decoder_feed(wr_body_decoder_t *decoder, const void *data, size_t length);
/* Call on complete HTTP response only, when feed() never returned 1. */
int wr_body_decoder_finish(wr_body_decoder_t *decoder);
void wr_body_decoder_close(wr_body_decoder_t *decoder);
