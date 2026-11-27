#ifndef VSI_HTTP1_CODEC_H
#define VSI_HTTP1_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include "parser_http_1_0.h"

#define HTTP1_READ_BUFFER_SIZE   65536
#define HTTP1_HEADER_BUFFER_SIZE 16384
#define HTTP1_STREAM_BUFFER_SIZE 65536

#define HTTP1_OK                    0
#define HTTP1_ERR_IO               -1
#define HTTP1_ERR_HEADERS_TOO_LARGE -2
#define HTTP1_ERR_BAD_REQUEST      -3

typedef enum {
    HTTP1_BODY_NONE = 0,
    HTTP1_BODY_CONTENT_LENGTH,
    HTTP1_BODY_CHUNKED
} http1_body_mode_t;

typedef struct {
    http1_body_mode_t mode;
    uint64_t content_length;
} http1_body_framing_t;

/**
 * Reads only HTTP/1.x headers (up to \r\n\r\n) from client_fd into caller-owned
 * buffer, parses headers into req, and determines body framing (Content-Length or
 * Transfer-Encoding: chunked).
 *
 * Any initial body bytes read alongside the headers remain referenced via
 * req->body / req->body_len for subsequent streaming.
 *
 * @return HTTP1_OK (0), HTTP1_ERR_IO (-1), HTTP1_ERR_HEADERS_TOO_LARGE (-2),
 *         or HTTP1_ERR_BAD_REQUEST (-3)
 */
int http1_read_and_parse_headers(int client_fd,
                                 char *buffer,
                                 size_t buffer_size,
                                 HttpRequest *req,
                                 http1_body_framing_t *out_framing,
                                 size_t *out_bytes_read);

/**
 * Streams an HTTP/1.x request body (initial buffered bytes + remaining socket
 * bytes) directly from client_fd to upstream_fd using constant memory.
 *
 * @param out_streamed_bytes Receives additional bytes read from client_fd during streaming.
 * @return 0 on success, -1 on I/O error, premature EOF, or malformed chunked framing.
 */
int http1_stream_request_body(int client_fd,
                              int upstream_fd,
                              const char *initial_body,
                              size_t initial_len,
                              const http1_body_framing_t *framing,
                              size_t *out_streamed_bytes);

/**
 * Extended reader/parser returning total inbound bytes read via out_bytes_read.
 */
int http1_read_and_parse_request_ex(int client_fd,
                                    char *buffer,
                                    size_t buffer_size,
                                    HttpRequest *req,
                                    size_t *out_bytes_read);

/**
 * Reads an incoming HTTP/1.x request from client_fd into caller-owned buffer
 * (scanning for the \r\n\r\n header delimiter and draining any Content-Length body),
 * then parses the raw bytes into req.
 *
 * Caller retains ownership of buffer so req->body pointers remain valid.
 *
 * @param client_fd   Accepted client socket descriptor
 * @param buffer      Caller-provided buffer to store raw request bytes
 * @param buffer_size Capacity of buffer in bytes
 * @param req         Destination HttpRequest struct to populate
 * @return            0 on valid parsed request, -1 on read failure or rejected syntax
 */
int http1_read_and_parse_request(int client_fd, char *buffer, size_t buffer_size, HttpRequest *req);

#endif // VSI_HTTP1_CODEC_H
