#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../main/book/weread_tar.h"

static void checksum(unsigned char *header) {
    memset(header + 148, ' ', 8);
    unsigned sum = 0;
    for (unsigned i = 0; i < 512; ++i) sum += header[i];
    snprintf((char *)header + 148, 7, "%06o", sum);
    header[154] = 0;
    header[155] = ' ';
}

static void member(unsigned char *dst, const char *name, const unsigned char *data, size_t size) {
    assert(size <= 512 && strlen(name) < 100);
    memset(dst, 0, 1024);
    memcpy(dst, name, strlen(name));
    snprintf((char *)dst + 124, 12, "%011lo", (unsigned long)size);
    dst[156] = '0';
    memcpy(dst + 512, data, size);
    checksum(dst);
}

int main(void) {
    unsigned char tar[3072] = {0};
    const unsigned char jpg[] = {0xff, 0xd8, 0xff, 0xd9};
    const unsigned char png[] = {0x89, 'P', 'N', 'G'};
    member(tar, "36/epub_43208843_44", jpg, sizeof(jpg));
    member(tar + 1024, "Images/chart.png", png, sizeof(png));
    const void *data = NULL;
    size_t bytes = 0;
    assert(wr_tar_find_image(tar, sizeof(tar),
                            "https://res.weread.qq.com/wrepub/epub_43208843_44?sign=test",
                            strlen("https://res.weread.qq.com/wrepub/epub_43208843_44?sign=test"),
                            &data, &bytes));
    assert(bytes == sizeof(jpg) && !memcmp(data, jpg, bytes));
    assert(wr_tar_find_image(tar, sizeof(tar), "../Images/chart.png",
                            strlen("../Images/chart.png"), &data, &bytes));
    assert(bytes == sizeof(png) && !memcmp(data, png, bytes));
    assert(!wr_tar_find_image(tar, sizeof(tar), "../Images/not-found.png",
                             strlen("../Images/not-found.png"), &data, &bytes));
    member(tar + 1024, "other/epub_43208843_44", png, sizeof(png));
    assert(!wr_tar_find_image(tar, sizeof(tar), "epub_43208843_44",
                             strlen("epub_43208843_44"), &data, &bytes));
    tar[0] ^= 1;
    assert(!wr_tar_find_image(tar, sizeof(tar), "epub_43208843_44",
                             strlen("epub_43208843_44"), &data, &bytes));
    tar[0] ^= 1;
    memset(tar + 124, '9', 12);
    checksum(tar);
    assert(!wr_tar_find_image(tar, sizeof(tar), "epub_43208843_44",
                             strlen("epub_43208843_44"), &data, &bytes));
    puts("PASS: POSIX tar lookup, checksum, overflow, URL query, missing and ambiguous guards");
    return 0;
}
