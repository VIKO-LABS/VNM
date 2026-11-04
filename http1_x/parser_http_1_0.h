#ifndef PARSER_HTTP_1_0_H
#define PARSER_HTTP_1_0_H

#include <stddef.h>

#define MAX_METHOD_LEN 16
#define MAX_URI_LEN 1024
#define MAX_VERSION_LEN 16
#define MAX_HEADER_NAME_LEN 128
#define MAX_HEADER_VAL_LEN 512
#define MAX_HEADERS_COUNT 32

typedef enum {
    PARSER_STATE_METHOD,
    PARSER_STATE_URI,
    PARSER_STATE_VERSION,
    PARSER_STATE_HEADER_KEY,
    PARSER_STATE_HEADER_VALUE,
    PARSER_STATE_BODY,
    PARSER_STATE_ACCEPTED,
    PARSER_STATE_REJECTED
} HttpParserState;

typedef struct {
    char name[MAX_HEADER_NAME_LEN];
    char value[MAX_HEADER_VAL_LEN];
} HttpHeader;

typedef struct {
    char method[MAX_METHOD_LEN];
    char uri[MAX_URI_LEN];
    char version[MAX_VERSION_LEN];
    
    HttpHeader headers[MAX_HEADERS_COUNT];
    size_t header_count;

    const char *body;      /* Pointer to start of body inside input_buffer */
    size_t body_len;       /* Length of body bytes available */
    
    HttpParserState state;
    int is_http_0_9;       /* 1 if fallback to HTTP/0.9 simple request */
} HttpRequest;

typedef struct {
    char version[10];
    int status_code;
    char reason_phrase[64];
    HttpHeader headers[32];
    size_t header_count;
    const char *body;
    size_t body_len;
} HttpResponse;

/**
 * Parses an HTTP/1.0 request buffer.
 *
 * @param input_buffer  Pointer to raw request buffer received from socket
 * @param size          Total bytes in input_buffer
 * @param req           Pointer to HttpRequest struct to populate
 * @return              0 on success (ACCEPTED), -1 on error (REJECTED)
 */
int parse_http_1_0(const char *input_buffer, size_t size, HttpRequest *req);

/**
 * Helper to retrieve a header value by name (case-insensitive).
 *
 * @param req   Parsed request
 * @param name  Header name (e.g. "Content-Length", "Host")
 * @return      Pointer to header value or NULL if not found
 */
const char *get_header_value(const HttpHeader *header, const char *name, const size_t header_count);


#endif /* PARSER_HTTP_1_0_H */
