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

static const char* tls_version_to_str(uint16_t ver) {
    switch (ver) {
        case 0x0300: return "SSL 3.0";
        case 0x0301: return "TLS 1.0";
        case 0x0302: return "TLS 1.1";
        case 0x0303: return "TLS 1.2 (or TLS 1.3 negotiated)";
        case 0x0304: return "TLS 1.3";
        default:     return "Unknown";
    }
}

static const char* cipher_suite_to_str(uint16_t cs) {
    switch (cs) {
        case 0x1301: return "TLS_AES_128_GCM_SHA256";
        case 0x1302: return "TLS_AES_256_GCM_SHA384";
        case 0x1303: return "TLS_CHACHA20_POLY1305_SHA256";
        case 0x1304: return "TLS_AES_128_CCM_SHA256";
        case 0x1305: return "TLS_AES_128_CCM_8_SHA256";
        case 0xC02B: return "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256";
        case 0xC02C: return "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384";
        case 0xC02F: return "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256";
        case 0xC030: return "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384";
        case 0xCCA8: return "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256";
        case 0xCCA9: return "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256";
        default:     return NULL;
    }
}

void print_tls_handshake(const char *buffer, size_t size) {
    if (!buffer || size < 5) {
        return;
    }

    const uint8_t *p = (const uint8_t *)buffer;
    uint8_t content_type = p[0];
    if (content_type != 0x16) {
        // Not a TLS Handshake record (e.g. 0x17 Application Data, 0x15 Alert)
        return;
    }

    uint16_t legacy_ver = ((uint16_t)p[1] << 8) | p[2];
    uint16_t record_len = ((uint16_t)p[3] << 8) | p[4];

    if (size < 6) return;

    uint8_t handshake_type = p[5];
    if (handshake_type != 0x01 && handshake_type != 0x02) {
        return;
    }

    printf("\n==================== [TLS HANDSHAKE INSPECTION] ====================\n");
    printf("Record Type       : Handshake (0x16)\n");
    printf("Record Version    : %s (0x%04X)\n", tls_version_to_str(legacy_ver), legacy_ver);
    printf("Record Length     : %u bytes\n", record_len);

    if (handshake_type == 0x01) {
        // ------------------ CLIENT HELLO ------------------
        if (size < 43) {
            printf("Message Type      : Client Hello (Truncated: %zu bytes)\n", size);
            printf("===================================================================\n\n");
            return;
        }

        uint32_t hs_len = ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 8) | p[8];
        uint16_t client_ver = ((uint16_t)p[9] << 8) | p[10];

        printf("Handshake Type    : Client Hello (0x01)\n");
        printf("Handshake Length  : %u bytes\n", hs_len);
        printf("Client Version    : %s (0x%04X)\n", tls_version_to_str(client_ver), client_ver);

        printf("Client Random     : ");
        for (int i = 0; i < 8; i++) printf("%02X", p[11 + i]);
        printf("... (32 bytes total)\n");

        size_t offset = 43;
        uint8_t session_id_len = p[offset++];
        offset += session_id_len;

        // Cipher suites
        if (offset + 2 <= size) {
            uint16_t cipher_suites_len = ((uint16_t)p[offset] << 8) | p[offset + 1];
            offset += 2;
            size_t ciphers_count = cipher_suites_len / 2;
            printf("Cipher Suites     : %zu suites offered\n", ciphers_count);

            size_t printed = 0;
            for (size_t i = 0; i < ciphers_count && offset + 2 <= size; i++) {
                uint16_t cs = ((uint16_t)p[offset] << 8) | p[offset + 1];
                offset += 2;
                const char *cs_name = cipher_suite_to_str(cs);
                if (cs_name && printed < 5) {
                    printf("  - [0x%04X] %s\n", cs, cs_name);
                    printed++;
                }
            }
            if (ciphers_count > printed) {
                printf("  - ... and %zu more cipher suites\n", ciphers_count - printed);
            }
        }

        // Compression methods
        if (offset < size) {
            uint8_t comp_len = p[offset++];
            offset += comp_len;
        }

        // Extensions
        if (offset + 2 <= size) {
            uint16_t ext_total_len = ((uint16_t)p[offset] << 8) | p[offset + 1];
            offset += 2;
            printf("Extensions Total  : %u bytes\n", ext_total_len);

            size_t ext_end = offset + ext_total_len;
            if (ext_end > size) ext_end = size;

            while (offset + 4 <= ext_end) {
                uint16_t ext_type = ((uint16_t)p[offset] << 8) | p[offset + 1];
                uint16_t ext_len = ((uint16_t)p[offset + 2] << 8) | p[offset + 3];
                offset += 4;

                if (offset + ext_len > ext_end) break;

                if (ext_type == 0x0000) {
                    // SNI (Server Name Indication)
                    if (ext_len >= 5) {
                        uint16_t sni_list_len = ((uint16_t)p[offset] << 8) | p[offset + 1];
                        (void)sni_list_len;
                        uint8_t name_type = p[offset + 2];
                        uint16_t name_len = ((uint16_t)p[offset + 3] << 8) | p[offset + 4];

                        if (name_type == 0 && offset + 5 + name_len <= ext_end) {
                            char sni_buf[256] = {0};
                            size_t copy_len = name_len < sizeof(sni_buf) - 1 ? name_len : sizeof(sni_buf) - 1;
                            memcpy(sni_buf, &p[offset + 5], copy_len);
                            sni_buf[copy_len] = '\0';
                            printf("  * SNI (Server Name): >>> %s <<<\n", sni_buf);
                        }
                    }
                } else if (ext_type == 0x0010) {
                    // ALPN (Application-Layer Protocol Negotiation)
                    if (ext_len >= 2) {
                        uint16_t alpn_list_len = ((uint16_t)p[offset] << 8) | p[offset + 1];
                        (void)alpn_list_len;
                        size_t a_off = offset + 2;
                        size_t a_end = offset + ext_len;
                        printf("  * ALPN Protocols   : ");
                        while (a_off < a_end) {
                            uint8_t proto_len = p[a_off++];
                            if (a_off + proto_len <= a_end) {
                                printf("\"%.*s\" ", (int)proto_len, &p[a_off]);
                                a_off += proto_len;
                            } else {
                                break;
                            }
                        }
                        printf("\n");
                    }
                } else if (ext_type == 0x002B) {
                    // Supported Versions (indicates TLS 1.3)
                    printf("  * Supported Vers   : TLS 1.3 supported (0x002B)\n");
                }
                offset += ext_len;
            }
        }
    } else if (handshake_type == 0x02) {
        // ------------------ SERVER HELLO ------------------
        printf("Handshake Type    : Server Hello (0x02)\n");
        if (size >= 40) {
            uint16_t server_ver = ((uint16_t)p[9] << 8) | p[10];
            printf("Server Version    : %s (0x%04X)\n", tls_version_to_str(server_ver), server_ver);
            size_t s_off = 43 + p[43] + 1; // skip session ID
            if (s_off + 2 <= size) {
                uint16_t chosen_cs = ((uint16_t)p[s_off] << 8) | p[s_off + 1];
                const char *cs_name = cipher_suite_to_str(chosen_cs);
                printf("Selected Cipher   : [0x%04X] %s\n", chosen_cs, cs_name ? cs_name : "Custom/Unknown");
            }
        }
    }

    printf("====================================================================\n\n");
    fflush(stdout);
}
