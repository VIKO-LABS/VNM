#include "reverseProxy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../utils/toolkit.h"
#include "../../utils/vsi_metrics.h"
#include "../../http1_x/outboundEngine.h"
#include "../../http1_x/http1_codec.h"

#define RP_RESP_HDR_BUF_SIZE 16384
#define RP_STREAM_BUF_SIZE   65536

static ssize_t send_all_bytes(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, data + sent, len - sent);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return (ssize_t)sent;
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
 * with `replacement` (case-insensitive). Returns bytes written, or -1 on error.
 */
static ssize_t write_line_replacing_host(int client_fd,
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
    size_t total_written = 0;

    while (pos < line_len) {
        if (pos + needle_len <= line_len && strncasecmp(line + pos, needle, needle_len) == 0) {
            ssize_t w = send_all_bytes(client_fd, replacement, repl_len);
            if (w < 0) return -1;
            total_written += (size_t)w;
            pos += needle_len;
        } else {
            ssize_t w = send_all_bytes(client_fd, line + pos, 1);
            if (w < 0) return -1;
            total_written += (size_t)w;
            pos++;
        }
    }
    return (ssize_t)total_written;
}

/**
 * Reads upstream response headers until \r\n\r\n, rewrites any internal backend
 * host references in Host:/Location: headers back to orig_host, and streams
 * the response body to client_fd until upstream EOF.
 */
static int rewrite_and_relay_response(int upstream_fd,
                                      int client_fd,
                                      const vsi_rp_mapping_t *mapping,
                                      const char *orig_host,
                                      size_t *out_bytes_sent,
                                      uint64_t *out_first_byte_ns) {
    if (out_bytes_sent) *out_bytes_sent = 0;
    if (out_first_byte_ns) *out_first_byte_ns = 0;

    char hdr_buf[RP_RESP_HDR_BUF_SIZE];
    size_t filled = 0;
    char *hdr_end = NULL;
    size_t total_sent = 0;
    uint64_t first_byte_ns = 0;

    while (filled < sizeof(hdr_buf) - 1) {
        ssize_t r = read(upstream_fd, hdr_buf + filled, sizeof(hdr_buf) - 1 - filled);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("[REVERSE_PROXY] Failed reading upstream response headers");
            return -1;
        }
        if (r == 0) break;

        if (first_byte_ns == 0 && out_first_byte_ns) {
            first_byte_ns = vsi_now_ns();
        }

        filled += (size_t)r;
        hdr_buf[filled] = '\0';

        hdr_end = strstr(hdr_buf, "\r\n\r\n");
        if (hdr_end != NULL) break;
    }

    if (out_first_byte_ns) *out_first_byte_ns = first_byte_ns;

    if (filled == 0) {
        return -1;
    }

    // Fallback: if upstream sent raw data without standard \r\n\r\n within 16 KB, forward verbatim
    if (hdr_end == NULL) {
        ssize_t w = send_all_bytes(client_fd, hdr_buf, filled);
        if (w < 0) return -1;
        total_sent += (size_t)w;
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

            ssize_t lw = should_rewrite
                         ? write_line_replacing_host(client_fd, cursor, line_len, mapped_host_port, orig_host)
                         : send_all_bytes(client_fd, cursor, line_len);
            if (lw < 0) return -1;
            total_sent += (size_t)lw;

            ssize_t cw = send_all_bytes(client_fd, "\r\n", 2);
            if (cw < 0) return -1;
            total_sent += (size_t)cw;

            is_first_line = 0;
            cursor = (crlf && crlf <= block_end) ? (crlf + 2) : block_end;
        }

        // Terminate response header section
        ssize_t tw = send_all_bytes(client_fd, "\r\n", 2);
        if (tw < 0) return -1;
        total_sent += (size_t)tw;

        // Flush any body bytes already captured inside hdr_buf after \r\n\r\n
        const char *body_start = hdr_end + 4;
        size_t already_body_len = filled - (size_t)(body_start - hdr_buf);
        if (already_body_len > 0) {
            ssize_t bw = send_all_bytes(client_fd, body_start, already_body_len);
            if (bw < 0) return -1;
            total_sent += (size_t)bw;
        }
    }

    // Stream remaining upstream body chunks until backend sends FIN (EOF)
    char stream_buf[RP_STREAM_BUF_SIZE];
    while (1) {
        ssize_t r = read(upstream_fd, stream_buf, sizeof(stream_buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("[REVERSE_PROXY] Upstream body read error");
            if (out_bytes_sent) *out_bytes_sent = total_sent;
            return -1;
        }
        if (r == 0) break;

        ssize_t sw = send_all_bytes(client_fd, stream_buf, (size_t)r);
        if (sw < 0) {
            if (out_bytes_sent) *out_bytes_sent = total_sent;
            return -1;
        }
        total_sent += (size_t)sw;
    }

    if (out_bytes_sent) *out_bytes_sent = total_sent;
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

        if (strcasecmp(name, "Proxy-Connection") == 0 ||
            strcasecmp(name, "Expect") == 0) {
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

static int handle_reverse_proxy_request_timed(int client_fd,
                                            HttpRequest *req,
                                            const vsi_rp_mapping_t *mapping,
                                            const http1_body_framing_t *framing,
                                            size_t *out_streamed_in,
                                            size_t *out_bytes_sent,
                                            uint64_t *out_after_send_ns,
                                            uint64_t *out_first_byte_ns) {
    if (out_streamed_in) *out_streamed_in = 0;
    if (out_bytes_sent) *out_bytes_sent = 0;
    if (out_after_send_ns) *out_after_send_ns = 0;
    if (out_first_byte_ns) *out_first_byte_ns = 0;

    if (client_fd < 0 || !req || !mapping) {
        return -1;
    }

    const char *expect = get_header_value(req->headers, "Expect", req->header_count);
    int expect_continue = (expect && strcasestr(expect, "100-continue") != NULL);

    struct addrinfo *res = NULL;
    char orig_host[MAX_HEADER_VAL_LEN] = {0};

    if (reverse_proxy_transform_request_v2(req, mapping,
                                           orig_host, sizeof(orig_host), &res) != 0 || !res) {
        const char *bad_gw = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Gateway";
        ssize_t w = send_all_bytes(client_fd, bad_gw, strlen(bad_gw));
        if (out_bytes_sent && w > 0) *out_bytes_sent = (size_t)w;
        return -1;
    }

    int upstream_fd = outbound_connect_tcp(res);
    freeaddrinfo(res);
    if (upstream_fd < 0) {
        const char *bad_gw = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Gateway";
        ssize_t w = send_all_bytes(client_fd, bad_gw, strlen(bad_gw));
        if (out_bytes_sent && w > 0) *out_bytes_sent = (size_t)w;
        return -1;
    }

    // Serialize headers separately, then stream body without copying memory
    const char *orig_body = req->body;
    size_t orig_body_len = req->body_len;
    req->body = NULL;
    req->body_len = 0;

    char header_buf[HTTP1_HEADER_BUFFER_SIZE + 512];
    int header_len = request_demapper(req, header_buf, sizeof(header_buf));
    if (header_len < 0) {
        outbound_close(upstream_fd);
        return -1;
    }

    if (send_all_bytes(upstream_fd, header_buf, (size_t)header_len) < 0) {
        outbound_close(upstream_fd);
        return -1;
    }

    size_t interim_sent = 0;
    if (expect_continue && framing && framing->mode != HTTP1_BODY_NONE) {
        const char *cont_msg = "HTTP/1.1 100 Continue\r\n\r\n";
        ssize_t cw = send_all_bytes(client_fd, cont_msg, strlen(cont_msg));
        if (cw < 0) {
            outbound_close(upstream_fd);
            return -1;
        }
        interim_sent = (size_t)cw;
    }

    size_t streamed_in = 0;
    if (http1_stream_request_body(client_fd, upstream_fd, orig_body, orig_body_len,
                                  framing, &streamed_in) != 0) {
        if (out_streamed_in) *out_streamed_in = streamed_in;
        if (out_bytes_sent) *out_bytes_sent = interim_sent;
        outbound_close(upstream_fd);
        return -1;
    }
    if (out_streamed_in) *out_streamed_in = streamed_in;

    if (out_after_send_ns) *out_after_send_ns = vsi_now_ns();

    int status = rewrite_and_relay_response(upstream_fd, client_fd, mapping, orig_host,
                                            out_bytes_sent, out_first_byte_ns); //here the upstradm_fd is the one which connect with the internal server (target host)
    if (out_bytes_sent) *out_bytes_sent += interim_sent;
    outbound_close(upstream_fd);
    return status;
}

int handle_reverse_proxy_request(int client_fd,
                                 HttpRequest *req,
                                 const vsi_rp_mapping_t *mapping) {
    http1_body_framing_t framing = { HTTP1_BODY_NONE, 0 };
    if (req && req->body && req->body_len > 0) {
        framing.mode = HTTP1_BODY_CONTENT_LENGTH;
        framing.content_length = req->body_len;
    }
    return handle_reverse_proxy_request_timed(client_fd, req, mapping,
                                              &framing, NULL, NULL, NULL, NULL);
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

    vsi_metrics_conn_open();
    uint64_t t0 = vsi_now_ns();
    size_t bytes_in = 0;
    size_t streamed_in = 0;
    size_t bytes_out = 0;
    uint64_t t_after_send = 0;
    uint64_t t_first_byte = 0;
    int failed = 0;

    char buffer[HTTP1_HEADER_BUFFER_SIZE];
    HttpRequest req;
    http1_body_framing_t framing;

    int rc = http1_read_and_parse_headers(client_fd, buffer, sizeof(buffer), &req, &framing, &bytes_in);
    if (rc == HTTP1_OK) {
        if (handle_reverse_proxy_request_timed(client_fd, &req, mapping,
                                               &framing, &streamed_in,
                                               &bytes_out, &t_after_send, &t_first_byte) != 0) {
            failed = 1;
        }
        bytes_in += streamed_in;
    } else if (rc == HTTP1_ERR_HEADERS_TOO_LARGE) {
        const char *resp = "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 31\r\nConnection: close\r\n\r\nRequest Header Fields Too Large";
        ssize_t w = send_all_bytes(client_fd, resp, strlen(resp));
        if (w > 0) bytes_out = (size_t)w;
        failed = 1;
    } else if (rc == HTTP1_ERR_BAD_REQUEST) {
        const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Request";
        ssize_t w = send_all_bytes(client_fd, resp, strlen(resp));
        if (w > 0) bytes_out = (size_t)w;
        failed = 1;
    } else {
        failed = 1;
    }

    uint64_t t_end = vsi_now_ns();
    uint64_t total_ns = (t_end > t0) ? (t_end - t0) : 0;
    uint64_t upstream_wait_ns = (t_first_byte > t_after_send && t_after_send > 0)
                                ? (t_first_byte - t_after_send) : 0;
    uint64_t proxy_overhead_ns = (total_ns > upstream_wait_ns) ? (total_ns - upstream_wait_ns) : total_ns;

    vsi_metrics_record_request(bytes_in, bytes_out, proxy_overhead_ns, upstream_wait_ns, total_ns, failed);
    vsi_metrics_conn_close();

    close(client_fd);
}
