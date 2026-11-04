#ifndef HTTPSERVER_OUTBOUNDENGINE_H
#define HTTPSERVER_OUTBOUNDENGINE_H

#include <stddef.h>
#include <stdint.h>

/**
 * Connects to an upstream server via TCP IPv4.
 *
 * @param ip    IPv4 address string (e.g., "127.0.0.1")
 * @param port  Target port number (e.g., 8080)
 * @return      Connected socket file descriptor on success, or -1 on error
 */
int outbound_connect_tcp(const char *ip, uint16_t port);

/**
 * Forwards/relays request bytes to the upstream server, reads the upstream
 * response, and pipes it directly back into client_fd.
 *
 * @param upstream_fd  Connected file descriptor to the upstream server
 * @param client_fd    Connected file descriptor to the downstream client
 * @param req_data     Raw HTTP request buffer to send upstream
 * @param req_len      Number of bytes in req_data
 * @return             Total response bytes relayed to client_fd, or -1 on error
 */
int outbound_relay_traffic(int upstream_fd, int client_fd, const char *req_data, size_t req_len);

/**
 * Closes an upstream socket descriptor cleanly.
 *
 * @param upstream_fd  File descriptor to close
 */
void outbound_close(int upstream_fd);

#endif // HTTPSERVER_OUTBOUNDENGINE_H
