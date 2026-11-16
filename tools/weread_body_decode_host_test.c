#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "../main/book/weread_body_decode.h"

typedef struct { char data[16000]; size_t length; bool stop; } sink_t;

static bool sink(const char *data, size_t n, void *user) {
    sink_t *s = user;
    assert(s->length + n <= sizeof(s->data));
    memcpy(s->data + s->length, data, n);
    s->length += n;
    return s->stop;
}

static size_t make_gzip(const void *src, size_t n, unsigned char *dest, size_t cap) {
    z_stream z = {0};
    assert(deflateInit2(&z, 6, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)n;
    z.next_out = dest;
    z.avail_out = (uInt)cap;
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
    size_t bytes = z.total_out;
    assert(deflateEnd(&z) == Z_OK);
    return bytes;
}

int main(void) {
    static const char page[] = "<script>var psvts=\"123456789\";</script>";
    unsigned char packed[4096];
    size_t clen = make_gzip(page, sizeof(page)-1, packed, sizeof(packed));
    wr_body_decoder_t d;
    sink_t s = {0};
    wr_body_decoder_init(&d, sink, &s, 10000, NULL, NULL);
    for (size_t i = 0; i < clen; ++i)
        assert(wr_body_decoder_feed(&d, packed + i, 1) == 0);
    assert(wr_body_decoder_finish(&d) == 0);
    assert(s.length == sizeof(page)-1 && !memcmp(page, s.data, s.length));
    wr_body_decoder_close(&d);

    memset(&s, 0, sizeof(s));
    wr_body_decoder_init(&d, sink, &s, 10000, NULL, NULL);
    for (size_t i = 0; i < sizeof(page)-1; ++i)
        assert(wr_body_decoder_feed(&d, page + i, 1) == 0);
    assert(wr_body_decoder_finish(&d) == 0);
    assert(s.length == sizeof(page)-1 && !memcmp(page, s.data, s.length));
    wr_body_decoder_close(&d);

    memset(&s, 0, sizeof(s));
    wr_body_decoder_init(&d, sink, &s, 10000, NULL, NULL);
    assert(wr_body_decoder_feed(&d, packed, clen-2) == 0);
    assert(wr_body_decoder_finish(&d) == -1);
    wr_body_decoder_close(&d);

    memset(&s, 0, sizeof(s));
    wr_body_decoder_init(&d, sink, &s, 10, NULL, NULL);
    assert(wr_body_decoder_feed(&d, packed, clen) == -1);
    wr_body_decoder_close(&d);

    memset(&s, 0, sizeof(s));
    s.stop = true;
    wr_body_decoder_init(&d, sink, &s, 10000, NULL, NULL);
    assert(wr_body_decoder_feed(&d, packed, clen) == 1);
    wr_body_decoder_close(&d);

    memset(&s, 0, sizeof(s));
    wr_body_decoder_init(&d, sink, &s, 10000, NULL, NULL);
    assert(wr_body_decoder_feed(&d, "a", 1) == 0);
    assert(wr_body_decoder_finish(&d) == 0);
    assert(s.length == 1 && s.data[0] == 'a');
    wr_body_decoder_close(&d);

    memset(&s, 0, sizeof(s));
    wr_body_decoder_init(&d, sink, &s, 10000, NULL, NULL);
    const unsigned char malformed[] = {0x1f, 0x8b, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(wr_body_decoder_feed(&d, malformed, sizeof(malformed)) == -1);
    wr_body_decoder_close(&d);

    puts("PASS: identity/gzip, byte boundaries, truncated, oversized, malformed, early stop");
    return 0;
}
