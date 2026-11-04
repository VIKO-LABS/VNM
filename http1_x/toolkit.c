#include "toolkit.h"
#include <stdio.h>
#include <string.h>

int request_mapper(const char *input_buffer, size_t buffer_size, HttpRequest *req) {
    if (!input_buffer || buffer_size == 0 || !req) {
        return -1;
    }
    return parse_http_1_0(input_buffer, buffer_size, req);
}

int request_demapper(const HttpRequest *req, char *output_buffer, size_t buffer_size) {
    if (!req || !output_buffer || buffer_size == 0) {
        return -1;
    }

    size_t offset = 0;

    // 1. Serialize Request Line: "<METHOD> <URI> <VERSION>\r\n"
    int written = snprintf(output_buffer + offset, buffer_size - offset,
                           "%s %s %s\r\n",
                           req->method,
                           req->uri,
                           req->version[0] != '\0' ? req->version : "HTTP/1.0");
    if (written < 0 || (size_t)written >= buffer_size - offset) {
        return -1; // Overflow
    }
    offset += (size_t)written;

    // 2. Serialize Headers: "<Header-Name>: <Header-Value>\r\n"
    for (size_t i = 0; i < req->header_count; i++) {
        written = snprintf(output_buffer + offset, buffer_size - offset,
                           "%s: %s\r\n",
                           req->headers[i].name,
                           req->headers[i].value);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            return -1; // Overflow
        }
        offset += (size_t)written;
    }

    // 3. Serialize Header Delimiter: "\r\n"
    written = snprintf(output_buffer + offset, buffer_size - offset, "\r\n");
    if (written < 0 || (size_t)written >= buffer_size - offset) {
        return -1; // Overflow
    }
    offset += (size_t)written;

    // 4. Append Body (if present)
    if (req->body && req->body_len > 0) {
        if (req->body_len >= buffer_size - offset) {
            return -1; // Not enough buffer space for body
        }
        memcpy(output_buffer + offset, req->body, req->body_len);
        offset += req->body_len;
        output_buffer[offset] = '\0';
    }

    return (int)offset;
}

int response_mapper(const char *input_buffer, size_t buffer_size, HttpResponse *res) {
    if (!input_buffer || buffer_size == 0 || !res) {
        return -1;
    }

    memset(res, 0, sizeof(HttpResponse));

    // Status-line parsing state machine: VERSION -> STATUS_CODE -> REASON_PHRASE
    enum {
        RESP_STATE_VERSION,
        RESP_STATE_STATUS_CODE,
        RESP_STATE_REASON,
        RESP_STATE_HEADER_KEY,
        RESP_STATE_HEADER_VALUE,
        RESP_STATE_BODY
    } state = RESP_STATE_VERSION;

    size_t v_idx = 0;
    size_t r_idx = 0;
    size_t hk_idx = 0;
    size_t hv_idx = 0;
    char code_buf[16] = {0};
    size_t code_idx = 0;

    for (size_t i = 0; i < buffer_size; i++) {
        char c = input_buffer[i];

        switch (state) {
            case RESP_STATE_VERSION:
                if (c == ' ') {
                    res->version[v_idx] = '\0';
                    state = RESP_STATE_STATUS_CODE;
                } else if (c == '\r' || c == '\n') {
                    return -1;
                } else if (v_idx < sizeof(res->version) - 1) {
                    res->version[v_idx++] = c;
                }
                break;

            case RESP_STATE_STATUS_CODE:
                if (c == ' ') {
                    code_buf[code_idx] = '\0';
                    res->status_code = 0;
                    for (size_t k = 0; k < code_idx; k++) {
                        if (code_buf[k] < '0' || code_buf[k] > '9') return -1;
                        res->status_code = res->status_code * 10 + (code_buf[k] - '0');
                    }
                    state = RESP_STATE_REASON;
                } else if (c == '\r' || c == '\n') {
                    return -1;
                } else if (code_idx < sizeof(code_buf) - 1) {
                    code_buf[code_idx++] = c;
                }
                break;

            case RESP_STATE_REASON:
                if (c == '\r') {
                    res->reason_phrase[r_idx] = '\0';
                } else if (c == '\n') {
                    res->reason_phrase[r_idx] = '\0';
                    state = RESP_STATE_HEADER_KEY;
                } else if (r_idx < sizeof(res->reason_phrase) - 1) {
                    res->reason_phrase[r_idx++] = c;
                }
                break;

            case RESP_STATE_HEADER_KEY:
                if (c == '\r') {
                    // Possible end of headers
                } else if (c == '\n') {
                    // Empty line: Transition to Body
                    state = RESP_STATE_BODY;
                    if (i + 1 < buffer_size) {
                        res->body = &input_buffer[i + 1];
                        res->body_len = buffer_size - (i + 1);
                    }
                    return 0; // Headers successfully mapped
                } else if (c == ':') {
                    if (res->header_count < 32) {
                        res->headers[res->header_count].name[hk_idx] = '\0';
                        hk_idx = 0;
                        state = RESP_STATE_HEADER_VALUE;
                    }
                } else if (hk_idx < MAX_HEADER_NAME_LEN - 1) {
                    if (res->header_count < 32) {
                        res->headers[res->header_count].name[hk_idx++] = c;
                    }
                }
                break;

            case RESP_STATE_HEADER_VALUE:
                if (c == ' ' && hv_idx == 0) {
                    // Skip leading whitespace after ':'
                } else if (c == '\r') {
                    // Line end carriage return
                } else if (c == '\n') {
                    if (res->header_count < 32) {
                        res->headers[res->header_count].value[hv_idx] = '\0';
                        hv_idx = 0;
                        res->header_count++;
                    }
                    state = RESP_STATE_HEADER_KEY;
                } else if (hv_idx < MAX_HEADER_VAL_LEN - 1) {
                    if (res->header_count < 32) {
                        res->headers[res->header_count].value[hv_idx++] = c;
                    }
                }
                break;

            case RESP_STATE_BODY:
                return 0;
        }
    }

    return (state == RESP_STATE_BODY || res->status_code > 0) ? 0 : -1;
}

int response_demapper(const HttpResponse *res, char *output_buffer, size_t buffer_size) {
    if (!res || !output_buffer || buffer_size == 0) {
        return -1;
    }

    size_t offset = 0;

    // 1. Serialize Status Line: "<VERSION> <STATUS_CODE> <REASON_PHRASE>\r\n"
    int written = snprintf(output_buffer + offset, buffer_size - offset,
                           "%s %d %s\r\n",
                           res->version[0] != '\0' ? res->version : "HTTP/1.1",
                           res->status_code,
                           res->reason_phrase[0] != '\0' ? res->reason_phrase : "OK");
    if (written < 0 || (size_t)written >= buffer_size - offset) {
        return -1;
    }
    offset += (size_t)written;

    // 2. Serialize Headers: "<Name>: <Value>\r\n"
    for (size_t i = 0; i < res->header_count; i++) {
        written = snprintf(output_buffer + offset, buffer_size - offset,
                           "%s: %s\r\n",
                           res->headers[i].name,
                           res->headers[i].value);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            return -1;
        }
        offset += (size_t)written;
    }

    // 3. Serialize Header Delimiter: "\r\n"
    written = snprintf(output_buffer + offset, buffer_size - offset, "\r\n");
    if (written < 0 || (size_t)written >= buffer_size - offset) {
        return -1;
    }
    offset += (size_t)written;

    // 4. Append Body (if present)
    if (res->body && res->body_len > 0) {
        if (res->body_len >= buffer_size - offset) {
            return -1;
        }
        memcpy(output_buffer + offset, res->body, res->body_len);
        offset += res->body_len;
        output_buffer[offset] = '\0';
    }

    return (int)offset;
}

void print_buffer(const char *buffer, size_t size) {
    if (!buffer || size == 0) {
        printf("[empty buffer]\n");
        return;
    }
    fwrite(buffer, 1, size, stdout);
    printf("\n");
    fflush(stdout);
}
