#include "outboundEngine.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define RELAY_BUFFER_SIZE 65536

int outbound_connect_tcp(const char *ip, uint16_t port) {
    if (!ip) {
        return -1;
    }

    int upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream_fd < 0) {
        perror("outbound_connect_tcp: socket creation failed");
        return -1;
    }

    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &target_addr.sin_addr) <= 0) {
        perror("outbound_connect_tcp: invalid IP address format");
        close(upstream_fd);
        return -1;
    }

    if (connect(upstream_fd, (struct sockaddr *)&target_addr, sizeof(target_addr)) < 0) {
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

void outbound_close(int upstream_fd) {
    if (upstream_fd >= 0) {
        close(upstream_fd);
    }
}
