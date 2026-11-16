#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../main/book/weread_body_decode.h"
#include "../main/book/weread_body_buffer.h"

typedef struct { size_t peak, calls; bool deny; } stats_t;
static void *resize_buffer(void *previous, size_t bytes, void *context) {
    stats_t *stats = context;
    if (stats->deny) return NULL;
    if (bytes > stats->peak) stats->peak = bytes;
    ++stats->calls;
    return realloc(previous, bytes);
}

static size_t make_gzip(const char *src, size_t size, unsigned char *out, size_t cap) {
    z_stream z = {0};
    assert(deflateInit2(&z, 6, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    z.next_in = (Bytef *)src; z.avail_in = (uInt)size;
    z.next_out = (Bytef *)out; z.avail_out = (uInt)cap;
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
    size_t n = z.total_out;
    assert(deflateEnd(&z) == Z_OK);
    return n;
}

static void test_decode(const unsigned char *body, size_t body_bytes,
                        const char *plain, size_t plain_bytes, size_t limit,
                        bool deny_alloc, bool should_succeed) {
    stats_t stats = {.deny = deny_alloc};
    wr_body_buffer_t sink = {.limit = limit, .resize = resize_buffer, .resize_context = &stats};
    wr_body_decoder_t decoder;
    wr_body_decoder_init(&decoder, wr_body_buffer_write, &sink, limit, NULL, NULL);
    int status = 0;
    for (size_t i = 0; i < body_bytes; i += 17) {
        size_t n = body_bytes - i < 17 ? body_bytes - i : 17;
        status = wr_body_decoder_feed(&decoder, body + i, n);
        if (status) break;
    }
    if (!status) status = wr_body_decoder_finish(&decoder);
    assert((status == 0) == should_succeed);
    if (should_succeed) {
        assert(sink.data && sink.used == plain_bytes);
        assert(!memcmp(sink.data, plain, plain_bytes));
        assert(sink.data[sink.used] == 0);
        assert(stats.peak <= limit + 1);
    }
    wr_body_decoder_close(&decoder);
    free(sink.data);
}

int main(void) {
    enum { N = 128 * 1024 };
    char *plain = malloc(N);
    unsigned char *wire = malloc(N * 2);
    assert(plain && wire);
    for (size_t i = 0; i < N; ++i)
        plain[i] = i % 79 ? 'A' : (char)('0' + i % 10);
    size_t compressed = make_gzip(plain, N, wire, N * 2);
    test_decode(wire, compressed, plain, N, N, false, true);
    test_decode((const unsigned char *)plain, N, plain, N, N, false, true);
    test_decode(wire, compressed, plain, N, N - 1, false, false);
    test_decode(wire, compressed, plain, N, N, true, false);
    test_decode(wire, compressed - 1, plain, N, N, false, false);

    stats_t stats = {0};
    wr_body_buffer_t tiny = {.limit = 2, .resize = resize_buffer, .resize_context = &stats};
    assert(!wr_body_buffer_write("ab", 2, &tiny));
    assert(tiny.capacity <= 3);
    assert(wr_body_buffer_write("c", 1, &tiny));
    assert(tiny.failed);
    free(tiny.data);
    free(plain);
    free(wire);
    puts("PASS: streaming gzip/identity, output cap, PSRAM budget, truncation, allocation failure");
    return 0;
}
