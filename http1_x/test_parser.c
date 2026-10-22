#include "parser_http_1_0.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_http_1_0_request(void) {
    const char raw_req[] = 
        "GET /index.html HTTP/1.0\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: CustomClient/1.0\r\n"
        "If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
        "\r\n";

    HttpRequest req;
    int res = parse_http_1_0(raw_req, sizeof(raw_req) - 1, &req);

    assert(res == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.uri, "/index.html") == 0);
    assert(strcmp(req.version, "HTTP/1.0") == 0);
    assert(req.header_count == 3);

    const char *host = get_header_value(&req, "Host");
    assert(host != NULL && strcmp(host, "localhost:8080") == 0);

    const char *ua = get_header_value(&req, "user-agent");
    assert(ua != NULL && strcmp(ua, "CustomClient/1.0") == 0);

    printf("✓ test_http_1_0_request passed!\n");
}

void test_http_0_9_fallback(void) {
    const char raw_req[] = "GET /about.html\r\n";

    HttpRequest req;
    int res = parse_http_1_0(raw_req, sizeof(raw_req) - 1, &req);

    assert(res == 0);
    assert(req.is_http_0_9 == 1);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.uri, "/about.html") == 0);
    assert(req.header_count == 0);

    printf("✓ test_http_0_9_fallback passed!\n");
}

void test_directory_traversal_rejection(void) {
    const char raw_req[] = "GET /../etc/passwd HTTP/1.0\r\n\r\n";

    HttpRequest req;
    int res = parse_http_1_0(raw_req, sizeof(raw_req) - 1, &req);

    assert(res == -1);
    assert(req.state == PARSER_STATE_REJECTED);

    printf("✓ test_directory_traversal_rejection passed!\n");
}

int main(void) {
    printf("Running HTTP/1.0 parser tests...\n");
    test_http_1_0_request();
    test_http_0_9_fallback();
    test_directory_traversal_rejection();
    printf("All parser tests passed successfully!\n");
    return 0;
}
