#include "parser_http_1_0.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

const char *get_header_value(const HttpRequest *req, const char *name) {
    if (!req || !name) return NULL;
    for (size_t i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

int parse_http_1_0(const char *input_buffer, size_t size, HttpRequest *req) {
    if (!input_buffer || size == 0 || !req) {
        return -1;
    }

    memset(req, 0, sizeof(HttpRequest));
    req->state = PARSER_STATE_METHOD;

    size_t m_idx = 0;
    size_t u_idx = 0;
    size_t v_idx = 0;
    size_t hk_idx = 0;
    size_t hv_idx = 0;

    int dot_count = 0;      /* For detecting consecutive '..' directory traversal */

    for (size_t i = 0; i < size; i++) {
        char c = input_buffer[i];

        switch (req->state) {
            case PARSER_STATE_METHOD:
                if (c == ' ') {
                    if (m_idx == 0) {
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                    req->method[m_idx] = '\0';
                    req->state = PARSER_STATE_URI;
                } else if (c == '\r' || c == '\n') {
                    /* Malformed: newline before URI */
                    req->state = PARSER_STATE_REJECTED;
                    return -1;
                } else {
                    if (m_idx < MAX_METHOD_LEN - 1) {
                        req->method[m_idx++] = c;
                    } else {
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                }
                break;

            case PARSER_STATE_URI:
                /* Path Traversal Protection: consecutive '..' check */
                if (c == '.') {
                    dot_count++;
                    if (dot_count == 2) {
                        /* Caught consecutive dots like '..' */
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                } else {
                    dot_count = 0;
                }

                if (c == ' ') {
                    if (u_idx == 0) {
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                    req->uri[u_idx] = '\0';
                    req->state = PARSER_STATE_VERSION;
                } else if (c == '\r') {
                    /* ignore CR */
                } else if (c == '\n') {
                    /* Fallback to HTTP/0.9 Simple-Request: GET <URI>\r\n without version */
                    if (u_idx == 0) {
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                    req->uri[u_idx] = '\0';
                    req->is_http_0_9 = 1;
                    strncpy(req->version, "HTTP/0.9", sizeof(req->version) - 1);
                    req->state = PARSER_STATE_ACCEPTED;
                    return 0;
                } else {
                    if (u_idx < MAX_URI_LEN - 1) {
                        req->uri[u_idx++] = c;
                    } else {
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                }
                break;

            case PARSER_STATE_VERSION:
                if (c == '\r') {
                    /* ignore CR */
                } else if (c == '\n') {
                    req->version[v_idx] = '\0';
                    /* End of Request-Line -> Move to headers */
                    req->state = PARSER_STATE_HEADER_KEY;
                    hk_idx = 0;
                } else {
                    if (v_idx < MAX_VERSION_LEN - 1) {
                        req->version[v_idx++] = c;
                    } else {
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                }
                break;

            case PARSER_STATE_HEADER_KEY:
                if (c == '\r') {
                    /* ignore CR */
                } else if (c == '\n') {
                    /* Found empty line (\r\n\r\n): End of headers! */
                    if (hk_idx == 0) {
                        /* Remainder of buffer is body payload */
                        if (i + 1 < size) {
                            req->body = &input_buffer[i + 1];
                            req->body_len = size - (i + 1);
                        } else {
                            req->body = NULL;
                            req->body_len = 0;
                        }
                        req->state = PARSER_STATE_ACCEPTED;
                        return 0;
                    } else {
                        /* Line ended before colon was seen: malformed header */
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                } else if (c == ':') {
                    /* Header key ended */
                    if (req->header_count >= MAX_HEADERS_COUNT) {
                        /* Reached header storage limit */
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                    req->headers[req->header_count].name[hk_idx] = '\0';
                    req->state = PARSER_STATE_HEADER_VALUE;
                    hv_idx = 0;
                } else {
                    if (hk_idx < MAX_HEADER_NAME_LEN - 1) {
                        req->headers[req->header_count].name[hk_idx++] = c;
                    } else {
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                }
                break;

            case PARSER_STATE_HEADER_VALUE:
                /* Trim leading whitespace after colon */
                if (hv_idx == 0 && (c == ' ' || c == '\t')) {
                    break;
                }

                if (c == '\r') {
                    /* ignore CR */
                } else if (c == '\n') {
                    /* Header line finished */
                    req->headers[req->header_count].value[hv_idx] = '\0';
                    req->header_count++;

                    /* Ready for next header line or empty terminating line */
                    req->state = PARSER_STATE_HEADER_KEY;
                    hk_idx = 0;
                } else {
                    if (hv_idx < MAX_HEADER_VAL_LEN - 1) {
                        req->headers[req->header_count].value[hv_idx++] = c;
                    } else {
                        req->state = PARSER_STATE_REJECTED;
                        return -1;
                    }
                }
                break;

            case PARSER_STATE_BODY:
            case PARSER_STATE_ACCEPTED:
            case PARSER_STATE_REJECTED:
                break;
        }
    }

    /* If loop finished without reaching terminating empty line */
    if (req->state != PARSER_STATE_ACCEPTED) {
        req->state = PARSER_STATE_REJECTED;
        return -1;
    }

    return 0;
}
