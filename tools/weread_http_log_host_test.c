#include <assert.h>
#include <string.h>

#include "weread_http_log.h"

int main(void) {
    char output[256];

    wr_http_log_target(
        "https://img.qq.com/book/cover.jpg?sign=secret-token&expires=123#fragment",
        output, sizeof(output));
    assert(strcmp(output, "https://img.qq.com/book/cover.jpg") == 0);
    assert(strstr(output, "secret-token") == NULL);

    wr_http_log_target("/web/book/getProgress?bookId=private-book-id", output, sizeof(output));
    assert(strcmp(output, "/web/book/getProgress") == 0);

    wr_http_log_target("https://cdn.qq.com/a.png#private-fragment", output, sizeof(output));
    assert(strcmp(output, "https://cdn.qq.com/a.png") == 0);

    wr_http_log_target("/path/with\ncontrol'quote?secret=1", output, sizeof(output));
    assert(strcmp(output, "/path/with?control?quote") == 0);

    char short_output[8];
    wr_http_log_target("https://cdn.qq.com/path/image.jpg?sig=hidden",
                       short_output, sizeof(short_output));
    assert(strcmp(short_output, "http...") == 0);

    wr_http_log_target(NULL, output, sizeof(output));
    assert(output[0] == 0);

    return 0;
}
