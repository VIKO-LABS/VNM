#include "http1_codec.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int send_all_fd(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, data + sent, len - sent);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

static int parse_strict_uint64(const char *s, uint64_t *out_val) {
    if (!s || !out_val) return -1;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return -1;

    uint64_t val = 0;
    int saw_digit = 0;
    while (*s >= '0' && *s <= '9') {
        saw_digit = 1;
        uint64_t digit = (uint64_t)(*s - '0');
        if (val > (UINT64_MAX - digit) / 10) {
            return -1;
        }
        val = val * 10 + digit;
        s++;
    }

    if (!saw_digit) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '\0') return -1;

    *out_val = val;
    return 0;
}

int http1_read_and_parse_headers(int client_fd,
                                 char *buffer,
                                 size_t buffer_size,
                                 HttpRequest *req,
                                 http1_body_framing_t *out_framing,
                                 size_t *out_bytes_read) {
    if (out_bytes_read) *out_bytes_read = 0;
    if (out_framing) {
        out_framing->mode = HTTP1_BODY_NONE;
        out_framing->content_length = 0;
    }
    if (client_fd < 0 || !buffer || buffer_size == 0 || !req) {
        return HTTP1_ERR_IO;
    }

    size_t filled = 0;
    const char *header_end = NULL;

    while (filled < buffer_size) {
        ssize_t r = read(client_fd, buffer + filled, buffer_size - filled);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            break;
        }
        filled += (size_t)r;

        header_end = (const char *)memmem(buffer, filled, "\r\n\r\n", 4);
        if (header_end != NULL) {
            break;
        }
    }

    if (out_bytes_read) *out_bytes_read = filled;

    if (filled == 0) {
        return HTTP1_ERR_IO;
    }

    if (header_end == NULL) {
        return (filled == buffer_size) ? HTTP1_ERR_HEADERS_TOO_LARGE : HTTP1_ERR_IO;
    }

    if (parse_http_1_0(buffer, filled, req) != 0 || req->state == PARSER_STATE_REJECTED) {
        return HTTP1_ERR_BAD_REQUEST;
    }

    const char *te = get_header_value(req->headers, "Transfer-Encoding", req->header_count);
    const char *cl = get_header_value(req->headers, "Content-Length", req->header_count);

    /* Reject ambiguous Transfer-Encoding + Content-Length (RFC 9112 §6.3) */
    if (te != NULL && cl != NULL) {
        return HTTP1_ERR_BAD_REQUEST;
    }

    if (te != NULL) {
        if (strcasestr(te, "chunked") != NULL) {
            if (out_framing) {
                out_framing->mode = HTTP1_BODY_CHUNKED;
                out_framing->content_length = 0;
            }
        } else {
            return HTTP1_ERR_BAD_REQUEST;
        }
    } else if (cl != NULL) {
        uint64_t parsed_cl = 0;
        if (parse_strict_uint64(cl, &parsed_cl) != 0) {
            return HTTP1_ERR_BAD_REQUEST;
        }
        if (out_framing) {
            out_framing->mode = HTTP1_BODY_CONTENT_LENGTH;
            out_framing->content_length = parsed_cl;
        }
    }

    return HTTP1_OK;
}

typedef enum {
    CHUNK_SIZE_LINE = 0,
    CHUNK_DATA,
    CHUNK_DATA_CRLF,
    CHUNK_TRAILERS,
    CHUNK_DONE
} chunk_parse_state_t;

typedef struct {
    chunk_parse_state_t state;
    uint64_t current_size;
    uint64_t remaining;
    int saw_hex;
    int in_ext;
    int saw_bws;
    int saw_cr;
    int data_cr;
    size_t trailer_len;
    int trailer_cr;
} chunked_fsm_t;

static int hex_digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int chunked_fsm_advance(chunked_fsm_t *fsm,
                               const char *buf,
                               size_t len,
                               size_t *out_consumed) {
    size_t pos = 0;

    while (pos < len && fsm->state != CHUNK_DONE) {
        switch (fsm->state) {
            case CHUNK_SIZE_LINE: {
                char c = buf[pos++];
                if (c == '\n') {
                    if (!fsm->saw_hex) return -1;
                    uint64_t sz = fsm->current_size;
                    fsm->current_size = 0;
                    fsm->saw_hex = 0;
                    fsm->in_ext = 0;
                    fsm->saw_bws = 0;
                    fsm->saw_cr = 0;
                    if (sz == 0) {
                        fsm->trailer_len = 0;
                        fsm->trailer_cr = 0;
                        fsm->state = CHUNK_TRAILERS;
                    } else {
                        fsm->remaining = sz;
                        fsm->state = CHUNK_DATA;
                    }
                } else if (fsm->saw_cr) {
                    return -1;
                } else if (c == '\r') {
                    if (!fsm->saw_hex) return -1;
                    fsm->saw_cr = 1;
                } else if (fsm->in_ext) {
                    /* Skip chunk-extension characters until CR/LF */
                } else if (c == ';') {
                    if (!fsm->saw_hex) return -1;
                    fsm->in_ext = 1;
                } else if (c == ' ' || c == '\t') {
                    if (!fsm->saw_hex) return -1;
                    fsm->saw_bws = 1;
                } else {
                    int d = hex_digit_val(c);
                    if (d < 0 || fsm->saw_bws) return -1;
                    if (fsm->current_size > (UINT64_MAX - (uint64_t)d) / 16) {
                        return -1;
                    }
                    fsm->current_size = fsm->current_size * 16 + (uint64_t)d;
                    fsm->saw_hex = 1;
                }
                break;
            }

            case CHUNK_DATA: {
                size_t avail = len - pos;
                size_t step = (fsm->remaining < avail) ? (size_t)fsm->remaining : avail;
                pos += step;
                fsm->remaining -= step;
                if (fsm->remaining == 0) {
                    fsm->data_cr = 0;
                    fsm->state = CHUNK_DATA_CRLF;
                }
                break;
            }

            case CHUNK_DATA_CRLF: {
                char c = buf[pos++];
                if (!fsm->data_cr && c == '\r') {
                    fsm->data_cr = 1;
                } else if (c == '\n') {
                    fsm->current_size = 0;
                    fsm->saw_hex = 0;
                    fsm->in_ext = 0;
                    fsm->saw_bws = 0;
                    fsm->saw_cr = 0;
                    fsm->state = CHUNK_SIZE_LINE;
                } else {
                    return -1;
                }
                break;
            }

            case CHUNK_TRAILERS: {
                char c = buf[pos++];
                if (c == '\n') {
                    if (fsm->trailer_len == 0) {
                        fsm->state = CHUNK_DONE;
                    } else {
                        fsm->trailer_len = 0;
                        fsm->trailer_cr = 0;
                    }
                } else if (fsm->trailer_cr) {
                    return -1;
                } else if (c == '\r') {
                    fsm->trailer_cr = 1;
                } else {
                    fsm->trailer_len++;
                }
                break;
            }

            case CHUNK_DONE:
                break;
        }
    }

    *out_consumed = pos;
    return 0;
}

int http1_stream_request_body(int client_fd,
                              int upstream_fd,
                              const char *initial_body,
                              size_t initial_len,
                              const http1_body_framing_t *framing,
                              size_t *out_streamed_bytes) {
    if (out_streamed_bytes) *out_streamed_bytes = 0;
    if (!framing || framing->mode == HTTP1_BODY_NONE) {
        return 0;
    }
    if (client_fd < 0 || upstream_fd < 0) {
        return -1;
    }

    if (framing->mode == HTTP1_BODY_CONTENT_LENGTH) {
        uint64_t remaining = framing->content_length;
        if (initial_body && initial_len > 0 && remaining > 0) {
            size_t to_send = (initial_len < remaining) ? initial_len : (size_t)remaining;
            if (send_all_fd(upstream_fd, initial_body, to_send) != 0) {
                return -1;
            }
            remaining -= to_send;
        }

        if (remaining == 0) {
            return 0;
        }

        char stream_buf[HTTP1_STREAM_BUFFER_SIZE];
        size_t total_streamed = 0;
        while (remaining > 0) {
            size_t want = (remaining < sizeof(stream_buf)) ? (size_t)remaining : sizeof(stream_buf);
            ssize_t r = read(client_fd, stream_buf, want);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
                return -1;
            }
            if (r == 0) {
                if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
                return -1; /* Premature EOF before Content-Length satisfied */
            }
            total_streamed += (size_t)r;
            if (send_all_fd(upstream_fd, stream_buf, (size_t)r) != 0) {
                if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
                return -1;
            }
            remaining -= (uint64_t)r;
        }

        if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
        return 0;
    }

    if (framing->mode == HTTP1_BODY_CHUNKED) {
        chunked_fsm_t fsm;
        memset(&fsm, 0, sizeof(fsm));
        fsm.state = CHUNK_SIZE_LINE;

        if (initial_body && initial_len > 0) {
            size_t consumed = 0;
            if (chunked_fsm_advance(&fsm, initial_body, initial_len, &consumed) != 0) {
                return -1;
            }
            if (consumed > 0 && send_all_fd(upstream_fd, initial_body, consumed) != 0) {
                return -1;
            }
        }

        char stream_buf[HTTP1_STREAM_BUFFER_SIZE];
        size_t total_streamed = 0;
        while (fsm.state != CHUNK_DONE) {
            ssize_t r = read(client_fd, stream_buf, sizeof(stream_buf));
            if (r < 0) {
                if (errno == EINTR) continue;
                if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
                return -1;
            }
            if (r == 0) {
                if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
                return -1; /* Premature EOF before terminating 0-chunk */
            }
            total_streamed += (size_t)r;

            size_t consumed = 0;
            if (chunked_fsm_advance(&fsm, stream_buf, (size_t)r, &consumed) != 0) {
                if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
                return -1;
            }
            if (consumed > 0 && send_all_fd(upstream_fd, stream_buf, consumed) != 0) {
                if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
                return -1;
            }
        }

        if (out_streamed_bytes) *out_streamed_bytes = total_streamed;
        return 0;
    }

    return -1;
}

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
