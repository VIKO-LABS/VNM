#include "http1_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int http1_read_and_parse_request_ex(int client_fd,
                                    char *buffer,
                                    size_t buffer_size,
                                    HttpRequest *req,
                                    size_t *out_bytes_read) {
    if (out_bytes_read) *out_bytes_read = 0;
    if (client_fd < 0 || !buffer || buffer_size == 0 || !req) {
        return -1;
    }

    size_t filled = 0;
    char *header_end = NULL;

    // Phase 1: Read until the header delimiter (\r\n\r\n) is found
    while (filled < buffer_size - 1) {
        ssize_t r = read(client_fd, buffer + filled, buffer_size - 1 - filled);
        if (r <= 0) {
            break;
        }
        filled += (size_t)r;
        buffer[filled] = '\0';

        header_end = strstr(buffer, "\r\n\r\n");
        if (header_end != NULL) {
            break;
        }
    }

    if (out_bytes_read) *out_bytes_read = filled;

    if (filled == 0) {
        return -1;
    }

    // Phase 2: If headers were found, parse Content-Length and read remaining body
    if (header_end != NULL) {
        int content_length = 0;
        char *cl_ptr = strcasestr(buffer, "Content-Length:");
        if (cl_ptr != NULL) {
            content_length = atoi(cl_ptr + 15);
        }

        if (content_length > 0) {
            size_t header_len = (size_t)((header_end + 4) - buffer);
            size_t body_already_read = (filled > header_len) ? (filled - header_len) : 0;
            size_t cl = (size_t)content_length;
            size_t body_remaining = (cl > body_already_read) ? (cl - body_already_read) : 0;

            while (body_remaining > 0 && filled < buffer_size - 1) {
                size_t avail = buffer_size - 1 - filled;
                size_t to_read = (body_remaining < avail) ? body_remaining : avail;
                ssize_t r = read(client_fd, buffer + filled, to_read);
                if (r <= 0) {
                    break;
                }
                filled += (size_t)r;
                body_remaining -= (size_t)r;
                buffer[filled] = '\0';
            }
            if (out_bytes_read) *out_bytes_read = filled;
        }
    }

    // Phase 3: Parse HTTP/1.x bytes into HttpRequest
    parse_http_1_0(buffer, filled, req);

    if (req->state == PARSER_STATE_REJECTED) {
        printf("[HTTP1_CODEC] Request parsing failed. Rejecting connection.\n");
        return -1;
    }

    return 0;
}

int http1_read_and_parse_request(int client_fd, char *buffer, size_t buffer_size, HttpRequest *req) {
    return http1_read_and_parse_request_ex(client_fd, buffer, buffer_size, req, NULL);
}
