#include "forwardProxy.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int forward_proxy_transform_request(const HttpRequest *in_req,
                                    HttpRequest *out_req,
                                    char *target_host,
                                    size_t host_len,
                                    uint16_t *target_port) {
    if (!in_req || !out_req || !target_host || host_len == 0 || !target_port) {
        return -1;
    }

    // Initialize the outbound request
    memset(out_req, 0, sizeof(HttpRequest));
    strncpy(out_req->method, in_req->method, sizeof(out_req->method) - 1);
    strncpy(out_req->version, in_req->version, sizeof(out_req->version) - 1);

    *target_port = 80; // Default HTTP port

    // Step 1: Parse absolute URI (e.g. "http://192.168.8.111:3000/favicon.ico")
    const char *uri_ptr = in_req->uri;
    const char *path_start = NULL;

    if (strncasecmp(uri_ptr, "http://", 7) == 0) {
        const char *host_start = uri_ptr + 7;
        const char *slash_ptr = strchr(host_start, '/');
        const char *colon_ptr = strchr(host_start, ':');

        if (colon_ptr && (!slash_ptr || colon_ptr < slash_ptr)) {
            // Port is present: "192.168.8.111:3000/..."
            size_t hlen = colon_ptr - host_start;
            if (hlen >= host_len) hlen = host_len - 1;
            strncpy(target_host, host_start, hlen);
            target_host[hlen] = '\0';

            *target_port = (uint16_t)atoi(colon_ptr + 1);
        } else {
            // No port in URI: "192.168.8.111/..."
            size_t hlen = slash_ptr ? (size_t)(slash_ptr - host_start) : strlen(host_start);
            if (hlen >= host_len) hlen = host_len - 1;
            strncpy(target_host, host_start, hlen);
            target_host[hlen] = '\0';
        }

        path_start = slash_ptr ? slash_ptr : "/";
    } else {
        // Fallback: If already relative, copy as is and fallback host to Host header
        path_start = in_req->uri[0] == '/' ? in_req->uri : "/";

        const char *host_val = get_header_value(in_req->headers, "Host", in_req->header_count);
        if (host_val) {
            const char *colon = strchr(host_val, ':');
            if (colon) {
                size_t hlen = colon - host_val;
                if (hlen >= host_len) hlen = host_len - 1;
                strncpy(target_host, host_val, hlen);
                target_host[hlen] = '\0';
                *target_port = (uint16_t)atoi(colon + 1);
            } else {
                strncpy(target_host, host_val, host_len - 1);
                target_host[host_len - 1] = '\0';
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

    // Step 4: Link body if present
    out_req->body = in_req->body;
    out_req->body_len = in_req->body_len;

    return 0;
}
