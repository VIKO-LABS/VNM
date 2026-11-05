#include "forwardProxy.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/select.h>
#include <unistd.h>
#include "../../http1_x/toolkit.h"
#include "../../http1_x/outboundEngine.h"

#define TUNNEL_BUFFER_SIZE 65536
#define HOST_SIZE 64
#define PORT_SIZE 8


void handle_request_through_tunnel(int clientfd,int outstream_fd) {
    if (clientfd < 0 || outstream_fd < 0) {
        return;
    }
    char buffer[TUNNEL_BUFFER_SIZE];
    fd_set read_fds;
    int max_fd = (clientfd > outstream_fd) ? clientfd : outstream_fd;
    //int min_fd = (clientfd < outstream_fd) ? clientfd : outstream_fd;
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(clientfd, &read_fds);
        FD_SET(outstream_fd, &read_fds);
        int triggered = select(max_fd+1,&read_fds,NULL,NULL,NULL);
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
                    return;
                }
                written += (size_t)w;
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
                    return;
                }
                written += (size_t)w;
            }
        }

    }
    //readclientfd -> write outstremfd
    //read outstreamfd -> write clientfd

}
void handle_request_through_http(int clientfd,int outstreamfd,HttpRequest *outbound_req) {
    char header_buf[4096];
    // Clear body from outbound_req temporarily so demapper only serializes headers
    const char *orig_body = outbound_req->body;
    size_t orig_body_len = outbound_req->body_len;
    outbound_req->body = NULL;
    outbound_req->body_len = 0;

    int header_len = request_demapper(outbound_req, header_buf, sizeof(header_buf));
    if (outstreamfd >= 0) {
        // Relays headers from header_buf and streams original body with zero memory copies
        outbound_relay_split(outstreamfd, clientfd, header_buf, header_len, orig_body, orig_body_len);
        outbound_close(outstreamfd);
    }

}
int handle_proxy_request(int clientfd,const HttpRequest *in_req) {
    if (!in_req || clientfd < 0) {
        return -1;
    }
    HttpRequest outbound_req;
    struct addrinfo *res = NULL;
    int transform_status = forward_proxy_transform_request(in_req, &outbound_req, &res);
    if (transform_status != 0 || !res) {
        return -1;
    }
    int outstream_fd = outbound_connect_tcp(res);
    if (outstream_fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (strcmp(outbound_req.method, "CONNECT") == 0) {
        const char *ok_msg = "HTTP/1.1 200 Connection Established\r\n\r\n";
        write(clientfd, ok_msg, strlen(ok_msg));
        handle_request_through_tunnel(clientfd,outstream_fd);
        close(outstream_fd);
        freeaddrinfo(res);
        return 0;
    }
    handle_request_through_http(clientfd,outstream_fd,&outbound_req);
    freeaddrinfo(res);

    return 0;
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
        if (strcasecmp(in_req->headers[i].name, "Proxy-Connection") == 0) {
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
