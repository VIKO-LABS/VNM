#ifndef COREENGINE_FORWARD_PROXY_H
#define COREENGINE_FORWARD_PROXY_H

#include "../../http1_x/parser_http_1_0.h"
#include <netdb.h>
#include <stdint.h>

/**
 * Transforms an incoming forward proxy HttpRequest into an origin-form
 * outbound HttpRequest suitable for transmission to the destination server.
 *
 * It extracts the target destination host/port, converts the absolute URI
 * into a relative origin path, and filters out proxy hop-by-hop headers
 * (like Proxy-Connection).
 *
 * @param in_req       Pointer to incoming HttpRequest parsed from client
 * @param out_req      Pointer to destination HttpRequest struct to populate
 * @param target_host  Buffer to store extracted target host/IP string
 * @param host_len     Maximum capacity of target_host buffer
 * @param target_port  Pointer to uint16_t to store extracted target port (defaults to 80)
 * @return             0 on successful transformation, or -1 on error
 */
int forward_proxy_transform_request(const HttpRequest *in_req,
                                    HttpRequest *out_req,
                                    struct addrinfo **res);
int handle_proxy_request(int clientfd, const HttpRequest *in_req);

#endif // COREENGINE_FORWARD_PROXY_H
