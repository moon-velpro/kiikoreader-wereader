#include <assert.h>
#include <string.h>

#include "weread_http_auth.h"

int main(void) {
    assert(wr_http_body_session_expired("{\"errcode\":-2012}"));
    assert(wr_http_body_session_expired("{\"errCode\": \"-2012\"}"));
    assert(!wr_http_body_session_expired("{\"errcode\":0}"));
    assert(!wr_http_body_session_expired("{\"errcode\":-20120}"));
    assert(!wr_http_body_session_expired("{\"message\":\"errcode -2012\"}"));
    assert(!wr_http_body_session_expired("{\"message\":\"\\\"errcode\\\":-2012\"}"));
    assert(!wr_http_body_session_expired("{\"errcode\" : -2012x}"));
    assert(!wr_http_body_session_expired_n("{\"errcode\":-2012", strlen("{\"errcode\":-2012")));
    assert(wr_http_body_session_expired("{\"data\":{\"errCode\":-2012}}"));

    wr_http_auth_scan_t scan = {0};
    const char *first = "{\"errcode\":";
    const char *second = "-2012}";
    assert(!wr_http_auth_scan_chunk(&scan, first, strlen(first)));
    assert(wr_http_auth_scan_chunk(&scan, second, strlen(second)));

    /* The key, colon, and value may arrive in three separate transport chunks. */
    memset(&scan, 0, sizeof(scan));
    first = "{\"errcode\"";
    second = ":";
    const char *third = "-2012}";
    assert(!wr_http_auth_scan_chunk(&scan, first, strlen(first)));
    assert(!wr_http_auth_scan_chunk(&scan, second, strlen(second)));
    assert(wr_http_auth_scan_chunk(&scan, third, strlen(third)));

    /* Do not treat a numeric value ending at a chunk boundary as complete. */
    memset(&scan, 0, sizeof(scan));
    first = "{\"errcode\":-2012";
    second = "0}";
    assert(!wr_http_auth_scan_chunk(&scan, first, strlen(first)));
    assert(!wr_http_auth_scan_chunk(&scan, second, strlen(second)));

    memset(&scan, 0, sizeof(scan));
    first = "{\"errcode\":-2012";
    second = "}";
    assert(!wr_http_auth_scan_chunk(&scan, first, strlen(first)));
    assert(wr_http_auth_scan_chunk(&scan, second, strlen(second)));

    memset(&scan, 0, sizeof(scan));
    first = "{\"errc";
    second = "ode\":-2012}";
    assert(!wr_http_auth_scan_chunk(&scan, first, strlen(first)));
    assert(wr_http_auth_scan_chunk(&scan, second, strlen(second)));

    memset(&scan, 0, sizeof(scan));
    const char *normal = "{\"errcode\":0,\"data\":{\"ok\":true}}";
    assert(!wr_http_auth_scan_chunk(&scan, normal, strlen(normal)));

    return 0;
}
