#include "forwardProxy.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/select.h>
#include <unistd.h>
#include "../../utils/toolkit.h"
#include "../../utils/vsi_metrics.h"
#include "../../http1_x/outboundEngine.h"
#include "../../http1_x/http1_codec.h"

#define TUNNEL_BUFFER_SIZE 65536
#define HOST_SIZE 64
#define PORT_SIZE 8

static ssize_t fp_write_all(int fd, const char *data, size_t len) {
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

static void handle_request_through_tunnel_tracked(int clientfd,
                                                  int outstream_fd,
                                                  size_t *out_bytes_in,
                                                  size_t *out_bytes_out) {
    if (out_bytes_in) *out_bytes_in = 0;
    if (out_bytes_out) *out_bytes_out = 0;
    if (clientfd < 0 || outstream_fd < 0) {
        return;
    }

    char buffer[TUNNEL_BUFFER_SIZE];
    fd_set read_fds;
    int max_fd = (clientfd > outstream_fd) ? clientfd : outstream_fd;
    size_t tunnel_in = 0;
    size_t tunnel_out = 0;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(clientfd, &read_fds);
        FD_SET(outstream_fd, &read_fds);
        int triggered = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (triggered < 0) {
            perror("select error");
            break;
        }
        if (FD_ISSET(clientfd, &read_fds)) {
            ssize_t r = read(clientfd, buffer, sizeof(buffer));
            if (r <= 0) {
                break; // Client closed connection or error
            }
            size_t written = 0;
            while (written < (size_t)r) {
                ssize_t w = write(outstream_fd, buffer + written, (size_t)r - written);
                if (w <= 0) {
                    goto tunnel_done;
                }
                written += (size_t)w;
                tunnel_in += (size_t)w;
            }
        }
        if (FD_ISSET(outstream_fd, &read_fds)) {
            ssize_t r = read(outstream_fd, buffer, sizeof(buffer));
            if (r <= 0) {
                break; // Upstream closed connection or error
            }
            size_t written = 0;
            while (written < (size_t)r) {
                ssize_t w = write(clientfd, buffer + written, (size_t)r - written);
                if (w <= 0) {
                    goto tunnel_done;
                }
                written += (size_t)w;
                tunnel_out += (size_t)w;
            }
        }
    }

tunnel_done:
    if (out_bytes_in) *out_bytes_in = tunnel_in;
    if (out_bytes_out) *out_bytes_out = tunnel_out;
}

void handle_request_through_tunnel(int clientfd, int outstream_fd) {
    handle_request_through_tunnel_tracked(clientfd, outstream_fd, NULL, NULL);
}

static int handle_request_through_http_streamed(int clientfd,
                                                int outstreamfd,
                                                HttpRequest *outbound_req,
                                                const http1_body_framing_t *framing,
                                                int expect_continue,
                                                size_t *out_streamed_in) {
    if (out_streamed_in) *out_streamed_in = 0;
    if (clientfd < 0 || outstreamfd < 0 || !outbound_req) {
        if (outstreamfd >= 0) outbound_close(outstreamfd);
        return -1;
    }

    char header_buf[HTTP1_HEADER_BUFFER_SIZE + 512];
    const char *orig_body = outbound_req->body;
    size_t orig_body_len = outbound_req->body_len;
    outbound_req->body = NULL;
    outbound_req->body_len = 0;

    int header_len = request_demapper(outbound_req, header_buf, sizeof(header_buf));
    if (header_len <= 0) {
        outbound_close(outstreamfd);
        return -1;
    }

    if (fp_write_all(outstreamfd, header_buf, (size_t)header_len) < 0) {
        outbound_close(outstreamfd);
        return -1;
    }

    size_t interim_sent = 0;
    if (expect_continue && framing && framing->mode != HTTP1_BODY_NONE) {
        const char *cont_msg = "HTTP/1.1 100 Continue\r\n\r\n";
        ssize_t cw = fp_write_all(clientfd, cont_msg, strlen(cont_msg));
        if (cw < 0) {
            outbound_close(outstreamfd);
            return -1;
        }
        interim_sent = (size_t)cw;
    }

    if (http1_stream_request_body(clientfd, outstreamfd, orig_body, orig_body_len,
                                  framing, out_streamed_in) != 0) {
        outbound_close(outstreamfd);
        return -1;
    }

    char resp_buf[HTTP1_STREAM_BUFFER_SIZE];
    size_t total_relayed = interim_sent;
    while (1) {
        ssize_t r = read(outstreamfd, resp_buf, sizeof(resp_buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            outbound_close(outstreamfd);
            return -1;
        }
        if (r == 0) {
            break;
        }
        if (fp_write_all(clientfd, resp_buf, (size_t)r) < 0) {
            outbound_close(outstreamfd);
            return -1;
        }
        total_relayed += (size_t)r;
    }

    outbound_close(outstreamfd);
    return (int)total_relayed;
}

int handle_request_through_http(int clientfd, int outstreamfd, HttpRequest *outbound_req) {
    char header_buf[4096];
    // Clear body from outbound_req temporarily so demapper only serializes headers
    const char *orig_body = outbound_req->body;
    size_t orig_body_len = outbound_req->body_len;
    outbound_req->body = NULL;
    outbound_req->body_len = 0;

    int header_len = request_demapper(outbound_req, header_buf, sizeof(header_buf));
    int relayed = -1;
    if (outstreamfd >= 0 && header_len > 0) {
        // Relays headers from header_buf and streams original body with zero memory copies
        relayed = outbound_relay_split(outstreamfd, clientfd, header_buf, header_len, orig_body, orig_body_len);
        outbound_close(outstreamfd);
    } else if (outstreamfd >= 0) {
        outbound_close(outstreamfd);
    }
    return relayed;
}

static int handle_proxy_request_timed(int clientfd,
                                      const HttpRequest *in_req,
                                      const http1_body_framing_t *framing,
                                      uint64_t t0,
                                      size_t *out_streamed_in,
                                      size_t *out_tunnel_in,
                                      size_t *out_bytes_out,
                                      uint64_t *out_latency_ns) {
    if (out_streamed_in) *out_streamed_in = 0;
    if (out_tunnel_in) *out_tunnel_in = 0;
    if (out_bytes_out) *out_bytes_out = 0;
    if (out_latency_ns) *out_latency_ns = 0;

    if (!in_req || clientfd < 0) {
        return -1;
    }

    const char *expect = get_header_value(in_req->headers, "Expect", in_req->header_count);
    int expect_continue = (expect && strcasestr(expect, "100-continue") != NULL);

    HttpRequest outbound_req;
    struct addrinfo *res = NULL;
    int transform_status = forward_proxy_transform_request(in_req, &outbound_req, &res);
    if (transform_status != 0 || !res) {
        return -1;
    }

    int outstream_fd = outbound_connect_tcp(res);
    freeaddrinfo(res);
    if (outstream_fd < 0) {
        return -1;
    }

    if (strcmp(outbound_req.method, "CONNECT") == 0) {
        const char *ok_msg = "HTTP/1.1 200 Connection Established\r\n\r\n";
        size_t ok_len = strlen(ok_msg);
        ssize_t w = fp_write_all(clientfd, ok_msg, ok_len);
        size_t established_bytes = (w > 0) ? (size_t)w : 0;

        // Capture tunnel setup latency right as 200 Connection Established is sent
        // (excludes arbitrary user idle/streaming time inside the tunnel)
        if (out_latency_ns && t0 > 0) {
            uint64_t t_setup = vsi_now_ns();
            *out_latency_ns = (t_setup > t0) ? (t_setup - t0) : 0;
        }

        size_t tunnel_in = 0;
        size_t tunnel_out = 0;
        if (w > 0) {
            handle_request_through_tunnel_tracked(clientfd, outstream_fd, &tunnel_in, &tunnel_out);
        }
        close(outstream_fd);

        if (out_tunnel_in) *out_tunnel_in = tunnel_in;
        if (out_bytes_out) *out_bytes_out = established_bytes + tunnel_out;
        return (w > 0) ? 0 : -1;
    }

    int relayed = handle_request_through_http_streamed(clientfd, outstream_fd, &outbound_req,
                                                       framing, expect_continue, out_streamed_in);
    if (out_latency_ns && t0 > 0) {
        uint64_t t_end = vsi_now_ns();
        *out_latency_ns = (t_end > t0) ? (t_end - t0) : 0;
    }
    if (out_bytes_out && relayed > 0) {
        *out_bytes_out = (size_t)relayed;
    }

    return (relayed >= 0) ? 0 : -1;
}

int handle_proxy_request(int clientfd, const HttpRequest *in_req) {
    http1_body_framing_t framing = { HTTP1_BODY_NONE, 0 };
    if (in_req && in_req->body && in_req->body_len > 0) {
        framing.mode = HTTP1_BODY_CONTENT_LENGTH;
        framing.content_length = in_req->body_len;
    }
    return handle_proxy_request_timed(clientfd, in_req, &framing, 0, NULL, NULL, NULL, NULL);
}

void forward_proxy_on_connection(int client_fd, void *user_ctx) {
    (void)user_ctx;
    if (client_fd < 0) {
        return;
    }

    vsi_metrics_conn_open();
    uint64_t t0 = vsi_now_ns();
    size_t req_bytes_in = 0;
    size_t streamed_in = 0;
    size_t tunnel_in = 0;
    size_t bytes_out = 0;
    uint64_t elapsed_ns = 0;
    int failed = 0;

    char buffer[HTTP1_HEADER_BUFFER_SIZE];
    HttpRequest req;
    http1_body_framing_t framing;

    int rc = http1_read_and_parse_headers(client_fd, buffer, sizeof(buffer), &req, &framing, &req_bytes_in);
    if (rc == HTTP1_OK) {
        if (handle_proxy_request_timed(client_fd, &req, &framing, t0,
                                       &streamed_in, &tunnel_in, &bytes_out, &elapsed_ns) != 0) {
            failed = 1;
        }
    } else if (rc == HTTP1_ERR_HEADERS_TOO_LARGE) {
        const char *resp = "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 31\r\nConnection: close\r\n\r\nRequest Header Fields Too Large";
        ssize_t w = fp_write_all(client_fd, resp, strlen(resp));
        if (w > 0) bytes_out = (size_t)w;
        elapsed_ns = vsi_now_ns() - t0;
        failed = 1;
    } else if (rc == HTTP1_ERR_BAD_REQUEST) {
        const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Request";
        ssize_t w = fp_write_all(client_fd, resp, strlen(resp));
        if (w > 0) bytes_out = (size_t)w;
        elapsed_ns = vsi_now_ns() - t0;
        failed = 1;
    } else {
        elapsed_ns = vsi_now_ns() - t0;
        failed = 1;
    }

    vsi_metrics_record_request(req_bytes_in + streamed_in + tunnel_in, bytes_out, elapsed_ns, 0, elapsed_ns, failed);
    vsi_metrics_conn_close();

    close(client_fd);
}



int forward_proxy_transform_request(const HttpRequest *in_req,
                                    HttpRequest *out_req,
                                    struct addrinfo **res) {
    if (!in_req || !out_req || !res) {
        return -1;
    }
    char target_port[PORT_SIZE];
    char target_host[HOST_SIZE];

    // Initialize the outbound request
    memset(out_req, 0, sizeof(HttpRequest));
    strncpy(out_req->method, in_req->method, sizeof(out_req->method) - 1);
    strncpy(out_req->version, in_req->version, sizeof(out_req->version) - 1);

    // Step 1: Parse URI
    const char *uri_ptr = in_req->uri;
    const char *path_start = NULL;

    if (strcasecmp(in_req->method, "CONNECT") == 0) {
        // Authority-form for CONNECT tunneling (e.g. "www.google.com:443" or "example.com:443")
        const char *colon_ptr = strchr(uri_ptr, ':');
        if (colon_ptr) {
            size_t hlen = colon_ptr - uri_ptr;
            if (hlen >= HOST_SIZE) hlen = HOST_SIZE - 1;
            strncpy(target_host, uri_ptr, hlen);
            target_host[hlen] = '\0';

            strncpy(target_port, colon_ptr + 1, PORT_SIZE - 1);
            target_port[PORT_SIZE - 1] = '\0';
        } else {
            strncpy(target_host, uri_ptr, HOST_SIZE - 1);
            target_host[HOST_SIZE - 1] = '\0';

            strncpy(target_port, "443", PORT_SIZE - 1);
            target_port[PORT_SIZE - 1] = '\0';
        }
        path_start = "/";
    } else if (strncasecmp(uri_ptr, "http://", 7) == 0) {
        const char *host_start = uri_ptr + 7;
        const char *slash_ptr = strchr(host_start, '/');
        const char *colon_ptr = strchr(host_start, ':');

        if (colon_ptr && (!slash_ptr || colon_ptr < slash_ptr)) {
            // Host is between host_start and colon_ptr
            size_t hlen = colon_ptr - host_start;
            if (hlen >= HOST_SIZE) hlen = HOST_SIZE - 1;
            strncpy(target_host, host_start, hlen);
            target_host[hlen] = '\0';

            // Port string is between colon_ptr + 1 and slash_ptr
            const char *port_start = colon_ptr + 1;
            size_t plen = slash_ptr ? (size_t)(slash_ptr - port_start) : strlen(port_start);
            if (plen >= PORT_SIZE) plen = PORT_SIZE - 1;
            strncpy(target_port, port_start, plen);
            target_port[plen] = '\0';
        } else {
            // No port in URI -> Default HTTP port "80"
            strncpy(target_port, "80", PORT_SIZE - 1);
            target_port[PORT_SIZE - 1] = '\0';

            size_t hlen = slash_ptr ? (size_t)(slash_ptr - host_start) : strlen(host_start);
            if (hlen >= HOST_SIZE) hlen = HOST_SIZE - 1;
            strncpy(target_host, host_start, hlen);
            target_host[hlen] = '\0';
        }

        path_start = slash_ptr ? slash_ptr : "/";
    } else {
        // Fallback: If already relative, copy as is and fallback host to Host header
        path_start = in_req->uri[0] == '/' ? in_req->uri : "/";
        strncpy(target_port, "80", PORT_SIZE - 1);
        target_port[PORT_SIZE - 1] = '\0';

        const char *host_val = get_header_value(in_req->headers, "Host", in_req->header_count);
        if (host_val) {
            const char *colon = strchr(host_val, ':');
            if (colon) {
                size_t hlen = colon - host_val;
                if (hlen >= HOST_SIZE) hlen = HOST_SIZE - 1;
                strncpy(target_host, host_val, hlen);
                target_host[hlen] = '\0';

                strncpy(target_port, colon + 1, PORT_SIZE - 1);
                target_port[PORT_SIZE - 1] = '\0';
            } else {
                strncpy(target_host, host_val, HOST_SIZE - 1);
                target_host[HOST_SIZE - 1] = '\0';
            }
        }
    }

    // Step 2: Set origin-form relative path in outbound request (e.g. "/favicon.ico")
    strncpy(out_req->uri, path_start, sizeof(out_req->uri) - 1);

    // Step 3: Copy headers, filtering out hop-by-hop proxy headers
    size_t out_hcount = 0;
    for (size_t i = 0; i < in_req->header_count && out_hcount < MAX_HEADERS_COUNT; i++) {
        // Skip hop-by-hop proxy headers
        if (strcasecmp(in_req->headers[i].name, "Proxy-Connection") == 0 ||
            strcasecmp(in_req->headers[i].name, "Expect") == 0) {
            continue;
        }

        strncpy(out_req->headers[out_hcount].name, in_req->headers[i].name, MAX_HEADER_NAME_LEN - 1);
        strncpy(out_req->headers[out_hcount].value, in_req->headers[i].value, MAX_HEADER_VAL_LEN - 1);
        out_hcount++;
    }

    // Ensure standard Connection header is present
    if (out_hcount < MAX_HEADERS_COUNT) {
        strncpy(out_req->headers[out_hcount].name, "Connection", MAX_HEADER_NAME_LEN - 1);
        strncpy(out_req->headers[out_hcount].value, "close", MAX_HEADER_VAL_LEN - 1);
        out_hcount++;
    }
    out_req->header_count = out_hcount;

    // Resolve DNS using clean target_host and target_port string
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP

    int dns_status = getaddrinfo(target_host, target_port, &hints, res);
    if (dns_status != 0) {
        fprintf(stderr, "DNS resolution failed for %s:%s: %s\n", target_host, target_port, gai_strerror(dns_status));
        return -1;
    }

    // Step 4: Link body if present
    out_req->body = in_req->body;
    out_req->body_len = in_req->body_len;

    return 0;
}
