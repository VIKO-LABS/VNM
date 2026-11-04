#include "outboundEngine.h"

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "toolkit.h"

#define RELAY_BUFFER_SIZE 65536

int outbound_connect_tcp(const struct addrinfo *req) {

    int upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream_fd < 0) {
        perror("outbound_connect_tcp: socket creation failed");
        return -1;
    }


    if (connect(upstream_fd, req->ai_addr, req->ai_addrlen)) {
        perror("outbound_connect_tcp: connect failed");
        close(upstream_fd);
        return -1;
    }

    return upstream_fd;
}

int outbound_relay_traffic(int upstream_fd, int client_fd, const char *req_data, size_t req_len) {
    if (upstream_fd < 0 || client_fd < 0 || !req_data || req_len == 0) {
        return -1;
    }

    // Step 1: Send the request payload to the upstream server
    size_t total_written = 0;
    while (total_written < req_len) {
        ssize_t w = write(upstream_fd, req_data + total_written, req_len - total_written);
        if (w <= 0) {
            perror("outbound_relay_traffic: write to upstream failed");
            return -1;
        }
        total_written += (size_t)w;
    }

    // Step 2: Stream the upstream response directly into client_fd
    char buffer[RELAY_BUFFER_SIZE];
    ssize_t total_relayed = 0;

    while (1) {
        ssize_t r = read(upstream_fd, buffer, sizeof(buffer));
        if (r < 0) {
            perror("outbound_relay_traffic: read from upstream failed");
            return -1;
        }
        //print_buffer(buffer,r);
        if (r == 0) {
            // Upstream closed the connection (EOF) - response stream completed
            break;
        }

        // Pipe chunk straight to downstream client
        size_t chunk_written = 0;
        while (chunk_written < (size_t)r) {
            ssize_t w = write(client_fd, buffer + chunk_written, (size_t)r - chunk_written);
            if (w <= 0) {
                perror("outbound_relay_traffic: write to client failed");
                return -1;
            }
            chunk_written += (size_t)w;
        }

        total_relayed += r;
    }

    return (int)total_relayed;
}

int outbound_relay_split(int upstream_fd, int client_fd,
                         const char *header_data, size_t header_len,
                         const char *body_data, size_t body_len) {
    if (upstream_fd < 0 || client_fd < 0 || !header_data || header_len == 0) {
        return -1;
    }

    // Step 1: Write rewritten headers to upstream
    size_t header_written = 0;
    while (header_written < header_len) {
        ssize_t w = write(upstream_fd, header_data + header_written, header_len - header_written);
        if (w <= 0) {
            perror("outbound_relay_split: write header failed");
            return -1;
        }
        header_written += (size_t)w;
    }

    // Step 2: Write raw body directly without any memory copies
    if (body_data && body_len > 0) {
        size_t body_written = 0;
        while (body_written < body_len) {
            ssize_t w = write(upstream_fd, body_data + body_written, body_len - body_written);
            if (w <= 0) {
                perror("outbound_relay_split: write body failed");
                return -1;
            }
            body_written += (size_t)w;
        }
    }

    // Step 3: Stream response from upstream to downstream client
    char buffer[RELAY_BUFFER_SIZE];
    ssize_t total_relayed = 0;

    while (1) {
        ssize_t r = read(upstream_fd, buffer, sizeof(buffer));
        if (r < 0) {
            perror("outbound_relay_split: read from upstream failed");
            return -1;
        }
        if (r == 0) {
            break; // EOF
        }

        size_t chunk_written = 0;
        while (chunk_written < (size_t)r) {
            ssize_t w = write(client_fd, buffer + chunk_written, (size_t)r - chunk_written);
            if (w <= 0) {
                perror("outbound_relay_split: write to client failed");
                return -1;
            }
            chunk_written += (size_t)w;
        }
        total_relayed += r;
    }

    return (int)total_relayed;
}

void outbound_close(int upstream_fd) {
    if (upstream_fd >= 0) {
        close(upstream_fd);
    }
}
