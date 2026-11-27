#ifndef VSI_HTTP1_CODEC_H
#define VSI_HTTP1_CODEC_H

#include <stddef.h>
#include "parser_http_1_0.h"

#define HTTP1_READ_BUFFER_SIZE 65536

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
