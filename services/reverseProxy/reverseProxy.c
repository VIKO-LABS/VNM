#include "reverseProxy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../utils/toolkit.h"
#include "../../http1_x/outboundEngine.h"
#include "../../http1_x/http1_codec.h"

#define RP_RESP_HDR_BUF_SIZE 16384
#define RP_STREAM_BUF_SIZE   65536

static int send_all_bytes(int fd, const char *data, size_t len) {
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

static void format_host_port(const vsi_rp_mapping_t *mapping, char *out, size_t out_size) {
    if (strcmp(mapping->target_port, "80") == 0) {
        snprintf(out, out_size, "%s", mapping->target_host);
    } else {
        snprintf(out, out_size, "%s:%s", mapping->target_host, mapping->target_port);
    }
}

/**
 * Copies `line` into `client_fd`, replacing all occurrences of `needle`
 * with `replacement` (case-insensitive).
 */
static int write_line_replacing_host(int client_fd,
                                     const char *line,
                                     size_t line_len,
                                     const char *needle,
                                     const char *replacement) {
    if (!needle || needle[0] == '\0' || !replacement || replacement[0] == '\0') {
        return send_all_bytes(client_fd, line, line_len);
    }

    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);
    size_t pos = 0;

    while (pos < line_len) {
        if (pos + needle_len <= line_len && strncasecmp(line + pos, needle, needle_len) == 0) {
            if (send_all_bytes(client_fd, replacement, repl_len) < 0) return -1;
            pos += needle_len;
        } else {
            if (send_all_bytes(client_fd, line + pos, 1) < 0) return -1;
            pos++;
        }
    }
    return 0;
}

/**
 * Reads upstream response headers until \r\n\r\n, rewrites any internal backend
 * host references in Host:/Location: headers back to orig_host, and streams
 * the response body to client_fd until upstream EOF.
 */
static int rewrite_and_relay_response(int upstream_fd,
                                      int client_fd,
                                      const vsi_rp_mapping_t *mapping,
                                      const char *orig_host) {
    char hdr_buf[RP_RESP_HDR_BUF_SIZE];
    size_t filled = 0;
    char *hdr_end = NULL;

    while (filled < sizeof(hdr_buf) - 1) {
        ssize_t r = read(upstream_fd, hdr_buf + filled, sizeof(hdr_buf) - 1 - filled);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("[REVERSE_PROXY] Failed reading upstream response headers");
            return -1;
        }
        if (r == 0) break;

        filled += (size_t)r;
        hdr_buf[filled] = '\0';

        hdr_end = strstr(hdr_buf, "\r\n\r\n");
        if (hdr_end != NULL) break;
    }

    if (filled == 0) {
        return -1;
    }

    // Fallback: if upstream sent raw data without standard \r\n\r\n within 16 KB, forward verbatim
    if (hdr_end == NULL) {
        if (send_all_bytes(client_fd, hdr_buf, filled) < 0) return -1;
    } else {
        char mapped_host_port[VSI_RP_HOST_SIZE + VSI_RP_PORT_SIZE + 2];
        format_host_port(mapping, mapped_host_port, sizeof(mapped_host_port));

        size_t headers_block_len = (size_t)(hdr_end - hdr_buf);
        const char *cursor = hdr_buf;
        const char *block_end = hdr_buf + headers_block_len;
        int is_first_line = 1;

        while (cursor < block_end) {
            const char *crlf = strstr(cursor, "\r\n");
            const char *line_end = (crlf && crlf <= block_end) ? crlf : block_end;
            size_t line_len = (size_t)(line_end - cursor);

            int should_rewrite = (!is_first_line && orig_host && orig_host[0] != '\0') &&
                                 ((line_len >= 5 && strncasecmp(cursor, "Host:", 5) == 0) ||
                                  (line_len >= 9 && strncasecmp(cursor, "Location:", 9) == 0));

            if (should_rewrite) {
                if (write_line_replacing_host(client_fd, cursor, line_len, mapped_host_port, orig_host) < 0) {
                    return -1;
                }
            } else {
                if (send_all_bytes(client_fd, cursor, line_len) < 0) return -1;
            }

            if (send_all_bytes(client_fd, "\r\n", 2) < 0) return -1;
            is_first_line = 0;
            cursor = (crlf && crlf <= block_end) ? (crlf + 2) : block_end;
        }

        // Terminate response header section
        if (send_all_bytes(client_fd, "\r\n", 2) < 0) return -1;

        // Flush any body bytes already captured inside hdr_buf after \r\n\r\n
        const char *body_start = hdr_end + 4;
        size_t already_body_len = filled - (size_t)(body_start - hdr_buf);
        if (already_body_len > 0) {
            if (send_all_bytes(client_fd, body_start, already_body_len) < 0) return -1;
        }
    }

    // Stream remaining upstream body chunks until backend sends FIN (EOF)
    char stream_buf[RP_STREAM_BUF_SIZE];
    while (1) {
        ssize_t r = read(upstream_fd, stream_buf, sizeof(stream_buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("[REVERSE_PROXY] Upstream body read error");
            return -1;
        }
        if (r == 0) break;

        if (send_all_bytes(client_fd, stream_buf, (size_t)r) < 0) {
            return -1;
        }
    }

    return 0;
}

static int resolve_backend_addr(const vsi_rp_mapping_t *mapping, struct addrinfo **res) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int dns_status = getaddrinfo(mapping->target_host, mapping->target_port, &hints, res);
    if (dns_status != 0) {
        fprintf(stderr, "[REVERSE_PROXY] DNS resolution failed for %s:%s: %s\n",
                mapping->target_host, mapping->target_port, gai_strerror(dns_status));
        return -1;
    }
    return 0;
}

int reverse_proxy_transform_request(const HttpRequest *in_req,
                                    const vsi_rp_mapping_t *mapping,
                                    HttpRequest *out_req,
                                    char *orig_host,
                                    size_t orig_host_size,
                                    struct addrinfo **res) {
    if (!in_req || !mapping || !out_req || !res) {
        return -1;
    }

    if (orig_host && orig_host_size > 0) {
        orig_host[0] = '\0';
        const char *incoming_host = get_header_value(in_req->headers, "Host", in_req->header_count);
        if (incoming_host) {
            snprintf(orig_host, orig_host_size, "%s", incoming_host);
        }
    }
    //here why did you unnnessarly create new outbound request instead of changeing the required places in the incomming request . memory allocation and assignment requies time .anyways have this as v1 and implement v2 that get these details asynchrounsly hle change the field n the request itself
    memset(out_req, 0, sizeof(HttpRequest));
    snprintf(out_req->method, sizeof(out_req->method), "%s", in_req->method);
    snprintf(out_req->uri, sizeof(out_req->uri), "%s", in_req->uri[0] != '\0' ? in_req->uri : "/");
    snprintf(out_req->version, sizeof(out_req->version), "%s",
             in_req->version[0] != '\0' ? in_req->version : "HTTP/1.1");

    char mapped_host[VSI_RP_HOST_SIZE + VSI_RP_PORT_SIZE + 2];
    format_host_port(mapping, mapped_host, sizeof(mapped_host));

    size_t out_hcount = 0;
    int host_replaced = 0;

    for (size_t i = 0; i < in_req->header_count && out_hcount < MAX_HEADERS_COUNT; i++) {
        const char *name = in_req->headers[i].name;

        // Skip incoming Connection / Proxy-Connection so we control upstream lifecycle
        if (strcasecmp(name, "Connection") == 0 || strcasecmp(name, "Proxy-Connection") == 0) {
            continue;
        }

        snprintf(out_req->headers[out_hcount].name, MAX_HEADER_NAME_LEN, "%s", name);
        if (strcasecmp(name, "Host") == 0) {
            snprintf(out_req->headers[out_hcount].value, MAX_HEADER_VAL_LEN, "%s", mapped_host);
            host_replaced = 1;
        } else {
            snprintf(out_req->headers[out_hcount].value, MAX_HEADER_VAL_LEN, "%s", in_req->headers[i].value);
        }
        out_hcount++;
    }

    if (!host_replaced && out_hcount < MAX_HEADERS_COUNT) {
        snprintf(out_req->headers[out_hcount].name, MAX_HEADER_NAME_LEN, "Host");
        snprintf(out_req->headers[out_hcount].value, MAX_HEADER_VAL_LEN, "%s", mapped_host);
        out_hcount++;
    }

    // Force Connection: close so backend emits FIN cleanly after response body (V1 EOF framing)
    if (out_hcount < MAX_HEADERS_COUNT) {
        snprintf(out_req->headers[out_hcount].name, MAX_HEADER_NAME_LEN, "Connection");
        snprintf(out_req->headers[out_hcount].value, MAX_HEADER_VAL_LEN, "close");
        out_hcount++;
    }

    out_req->header_count = out_hcount;
    out_req->body = in_req->body;
    out_req->body_len = in_req->body_len;

    return resolve_backend_addr(mapping, res);
}

int reverse_proxy_transform_request_v2(HttpRequest *req,
                                       const vsi_rp_mapping_t *mapping,
                                       char *orig_host,
                                       size_t orig_host_size,
                                       struct addrinfo **res) {
    if (!req || !mapping || !res) {
        return -1;
    }

    if (orig_host && orig_host_size > 0) {
        orig_host[0] = '\0';
    }

    if (req->uri[0] == '\0') {
        snprintf(req->uri, sizeof(req->uri), "/");
    }
    if (req->version[0] == '\0') {
        snprintf(req->version, sizeof(req->version), "HTTP/1.1");
    }

    char mapped_host[VSI_RP_HOST_SIZE + VSI_RP_PORT_SIZE + 2];
    format_host_port(mapping, mapped_host, sizeof(mapped_host));

    size_t write_idx = 0;
    int host_replaced = 0;
    int conn_replaced = 0;

    for (size_t i = 0; i < req->header_count && i < MAX_HEADERS_COUNT; i++) {
        const char *name = req->headers[i].name;

        if (strcasecmp(name, "Proxy-Connection") == 0) {
            continue;
        }

        if (strcasecmp(name, "Connection") == 0) {
            if (conn_replaced) {
                continue;
            }
            snprintf(req->headers[i].value, MAX_HEADER_VAL_LEN, "close");
            if (write_idx != i) {
                req->headers[write_idx] = req->headers[i];
            }
            conn_replaced = 1;
            write_idx++;
            continue;
        }

        if (strcasecmp(name, "Host") == 0) {
            if (host_replaced) {
                continue;
            }
            if (orig_host && orig_host_size > 0) {
                snprintf(orig_host, orig_host_size, "%s", req->headers[i].value);
            }
            snprintf(req->headers[i].value, MAX_HEADER_VAL_LEN, "%s", mapped_host);
            if (write_idx != i) {
                req->headers[write_idx] = req->headers[i];
            }
            host_replaced = 1;
            write_idx++;
            continue;
        }

        if (write_idx != i) {
            req->headers[write_idx] = req->headers[i];
        }
        write_idx++;
    }

    if (!host_replaced && write_idx < MAX_HEADERS_COUNT) {
        snprintf(req->headers[write_idx].name, MAX_HEADER_NAME_LEN, "Host");
        snprintf(req->headers[write_idx].value, MAX_HEADER_VAL_LEN, "%s", mapped_host);
        write_idx++;
    }

    if (!conn_replaced && write_idx < MAX_HEADERS_COUNT) {
        snprintf(req->headers[write_idx].name, MAX_HEADER_NAME_LEN, "Connection");
        snprintf(req->headers[write_idx].value, MAX_HEADER_VAL_LEN, "close");
        write_idx++;
    }

    req->header_count = write_idx;

    return resolve_backend_addr(mapping, res);
}

int handle_reverse_proxy_request(int client_fd,
                                 HttpRequest *req,
                                 const vsi_rp_mapping_t *mapping) {
    if (client_fd < 0 || !req || !mapping) {
        return -1;
    }

    struct addrinfo *res = NULL;
    char orig_host[MAX_HEADER_VAL_LEN] = {0};

    if (reverse_proxy_transform_request_v2(req, mapping,
                                           orig_host, sizeof(orig_host), &res) != 0 || !res) {
        const char *bad_gw = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Gateway";
        send_all_bytes(client_fd, bad_gw, strlen(bad_gw));
        return -1;
    }

    int upstream_fd = outbound_connect_tcp(res);
    freeaddrinfo(res);
    if (upstream_fd < 0) {
        const char *bad_gw = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Gateway";
        send_all_bytes(client_fd, bad_gw, strlen(bad_gw));
        return -1;
    }

    // Serialize headers separately, then stream body without copying memory
    const char *orig_body = req->body;
    size_t orig_body_len = req->body_len;
    req->body = NULL;
    req->body_len = 0;

    char header_buf[4096];
    int header_len = request_demapper(req, header_buf, sizeof(header_buf));
    if (header_len < 0) {
        outbound_close(upstream_fd);
        return -1;
    }

    if (send_all_bytes(upstream_fd, header_buf, (size_t)header_len) < 0) {
        outbound_close(upstream_fd);
        return -1;
    }

    if (orig_body && orig_body_len > 0) {
        if (send_all_bytes(upstream_fd, orig_body, orig_body_len) < 0) {
            outbound_close(upstream_fd);
            return -1;
        }
    }

    int status = rewrite_and_relay_response(upstream_fd, client_fd, mapping, orig_host); //here the upstradm_fd is the one which connect with the internal server (target host)
    outbound_close(upstream_fd);
    return status;
}

void reverse_proxy_on_connection(int client_fd, void *user_ctx) {
    if (client_fd < 0) {
        return;
    }

    const vsi_rp_mapping_t *mapping = (const vsi_rp_mapping_t *)user_ctx;
    if (!mapping) {
        close(client_fd);
        return;
    }

    char buffer[HTTP1_READ_BUFFER_SIZE];
    HttpRequest req;

    if (http1_read_and_parse_request(client_fd, buffer, sizeof(buffer), &req) == 0) {
        handle_reverse_proxy_request(client_fd, &req, mapping);
    }

    close(client_fd);
}
