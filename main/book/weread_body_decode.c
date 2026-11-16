#include "weread_body_decode.h"

#include <limits.h>
#include <string.h>

static int emit(wr_body_decoder_t *d, const unsigned char *data, size_t length) {
    if (!length) return 0;
    if (length > d->decoded_limit - d->decoded_bytes) return -1;
    d->decoded_bytes += length;
    return d->sink((const char *)data, length, d->user) ? 1 : 0;
}

void wr_body_decoder_init(wr_body_decoder_t *d, wr_body_sink_fn sink, void *user,
                          size_t decoded_limit, alloc_func alloc, free_func dealloc) {
    memset(d, 0, sizeof(*d));
    d->sink = sink;
    d->user = user;
    d->decoded_limit = decoded_limit;
    d->z.zalloc = alloc;
    d->z.zfree = dealloc;
}

static int inflate_bytes(wr_body_decoder_t *d, const unsigned char *bytes, size_t length) {
    if (length > UINT_MAX) return -1;
    d->z.next_in = (Bytef *)bytes;
    d->z.avail_in = (uInt)length;
    while (d->z.avail_in && !d->gzip_complete) {
        uInt before = d->z.avail_in;
        d->z.next_out = d->output;
        d->z.avail_out = sizeof(d->output);
        int zret = inflate(&d->z, Z_NO_FLUSH);
        size_t output_size = sizeof(d->output) - d->z.avail_out;
        int result = emit(d, d->output, output_size);
        if (result) return result;
        if (zret == Z_STREAM_END) {
            d->gzip_complete = true;
            return 0;
        }
        if (zret != Z_OK && zret != Z_BUF_ERROR) return -1;
        if (d->z.avail_in == before && !output_size) return -1;
        if (!d->z.avail_in && d->z.avail_out) break;
    }
    return 0;
}

int wr_body_decoder_feed(wr_body_decoder_t *d, const void *data, size_t length) {
    if (!d || !d->sink || (length && !data)) return -1;
    const unsigned char *bytes = data;
    if (!d->detected) {
        while (d->prefix_count < 2 && length) {
            d->prefix[d->prefix_count++] = *bytes++;
            --length;
        }
        if (d->prefix_count < 2) return 0;
        d->detected = true;
        d->compressed = d->prefix[0] == 0x1f && d->prefix[1] == 0x8b;
        if (d->compressed) {
            if (inflateInit2(&d->z, MAX_WBITS + 16) != Z_OK) return -1;
            d->inflater_ready = true;
            int result = inflate_bytes(d, d->prefix, 2);
            if (result) return result;
        } else {
            int result = emit(d, d->prefix, 2);
            if (result) return result;
        }
    }
    if (!length) return 0;
    return d->compressed ? inflate_bytes(d, bytes, length) : emit(d, bytes, length);
}

int wr_body_decoder_finish(wr_body_decoder_t *d) {
    if (!d || !d->sink) return -1;
    if (!d->detected) return emit(d, d->prefix, d->prefix_count);
    return d->compressed && !d->gzip_complete ? -1 : 0;
}

void wr_body_decoder_close(wr_body_decoder_t *d) {
    if (d && d->inflater_ready) {
        inflateEnd(&d->z);
        d->inflater_ready = false;
    }
}
