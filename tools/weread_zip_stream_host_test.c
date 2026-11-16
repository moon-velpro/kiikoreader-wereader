#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/book/weread_zip_stream.h"

static void put16(unsigned char *dst, unsigned v) {
    dst[0] = (unsigned char)v;
    dst[1] = (unsigned char)(v >> 8);
}
static void put32(unsigned char *dst, uint32_t v) {
    put16(dst, v);
    put16(dst + 2, v >> 16);
}

static bool finish_zip(FILE *archive, FILE *central, unsigned count) {
    long directory_at = ftell(archive);
    long directory_size = ftell(central);
    if (directory_at < 0 || directory_size < 0 || fseek(central, 0, SEEK_SET)) return false;
    unsigned char buffer[1024];
    for (long remaining = directory_size; remaining > 0;) {
        size_t n = remaining < (long)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        if (fread(buffer, 1, n, central) != n ||
            fwrite(buffer, 1, n, archive) != n) return false;
        remaining -= (long)n;
    }
    unsigned char tail[22] = {0};
    put32(tail, 0x06054b50);
    put16(tail + 8, count);
    put16(tail + 10, count);
    put32(tail + 12, (uint32_t)directory_size);
    put32(tail + 16, (uint32_t)directory_at);
    return fwrite(tail, 1, sizeof(tail), archive) == sizeof(tail);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    static const char payload[] = "<html>\nHello EPUB!\n</html>\n";
    unsigned char scratch[1024];
    FILE *src = tmpfile(), *zip = tmpfile(), *central = tmpfile();
    assert(src && zip && central);
    assert(fwrite(payload, 1, sizeof(payload)-1, src) == sizeof(payload)-1);
    assert(!wr_zip_append_stored_file(zip, central, src, "bad", 1, scratch, sizeof(scratch)));
    assert(wr_zip_append_stored_file(zip, central, src, "c0.xhtml", 4096, scratch, sizeof(scratch)));
    FILE *empty = tmpfile();
    assert(empty);
    assert(wr_zip_append_stored_file(zip, central, empty, "nav.xhtml", 4096, scratch, sizeof(scratch)));
    assert(finish_zip(zip, central, 2));
    assert(fseek(zip, 0, SEEK_SET) == 0);

    FILE *out = fopen(argv[1], "wb");
    assert(out);
    unsigned char buffer[2048];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), zip)) > 0)
        assert(fwrite(buffer, 1, n, out) == n);
    assert(!ferror(zip));
    assert(fclose(out) == 0);
    fclose(empty); fclose(src); fclose(central); fclose(zip);
    puts("PASS: single-pass stored ZIP file entries with data descriptor");
    return 0;
}
