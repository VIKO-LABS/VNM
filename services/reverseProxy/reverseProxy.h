#ifndef VSI_REVERSE_PROXY_H
#define VSI_REVERSE_PROXY_H

#include <netdb.h>
#include <stddef.h>
#include "../../http1_x/parser_http_1_0.h"

#define VSI_RP_HOST_SIZE 64
#define VSI_RP_PORT_SIZE 8

/**
 * Port-to-backend mapping for the basic reverse proxy.
 */
typedef struct {
    char listen_ip[VSI_RP_HOST_SIZE];
    int  listen_port;
    char target_host[VSI_RP_HOST_SIZE];
    char target_port[VSI_RP_PORT_SIZE];
} vsi_rp_mapping_t;

/**
 * V1: Transforms an incoming HTTP request into a separate out_req struct for the
 * mapped backend server. Kept alongside V2 for direct A/B performance comparison.
 *
 * @return 0 on success (caller owns *res and must call freeaddrinfo), or -1 on error.
 */
int reverse_proxy_transform_request(const HttpRequest *in_req,
                                    const vsi_rp_mapping_t *mapping,
                                    HttpRequest *out_req,
                                    char *orig_host,
                                    size_t orig_host_size,
                                    struct addrinfo **res);

/**
 * V2: Transforms an incoming HTTP request in-place (zero extra HttpRequest
 * allocation/copy) for the mapped backend server:
 * - Saves the original incoming Host header into orig_host.
 * - Replaces Host in-place with the mapped backend host[:port].
 * - Forces Connection: close in-place (dropping Proxy-Connection).
 * - Resolves the mapped backend address via getaddrinfo().
 *
 * @return 0 on success (caller owns *res and must call freeaddrinfo), or -1 on error.
 */
int reverse_proxy_transform_request_v2(HttpRequest *req,
                                       const vsi_rp_mapping_t *mapping,
                                       char *orig_host,
                                       size_t orig_host_size,
                                       struct addrinfo **res);

/**
 * Forwards a parsed request to the mapped backend and relays the response
 * (rewriting internal host occurrences in response Host/Location headers back
 * to the original client-facing host).
 *
 * @return 0 on success, or -1 on error.
 */
int handle_reverse_proxy_request(int client_fd,
                                 HttpRequest *req,
                                 const vsi_rp_mapping_t *mapping);

/**
 * L4 connection callback wired for VSI_MODE_REVERSE_PROXY.
 *
 * @param client_fd Accepted client socket descriptor
 * @param user_ctx  Pointer to the vsi_rp_mapping_t for this listening port
 */
void reverse_proxy_on_connection(int client_fd, void *user_ctx);

#endif // VSI_REVERSE_PROXY_H
