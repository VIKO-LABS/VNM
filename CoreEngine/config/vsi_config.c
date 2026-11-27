#include "vsi_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../services/forwardProxy/forwardProxy.h"
#include "../../services/reverseProxy/reverseProxy.h"

static int is_mapping_flag(const char *arg) {
    if (!arg || arg[0] != '-' || (arg[1] != 'M' && arg[1] != 'm')) {
        return 0;
    }
    if (arg[2] == '\0') {
        return 1;
    }
    for (size_t i = 2; arg[i] != '\0'; i++) {
        if (!isdigit((unsigned char)arg[i])) {
            return 0;
        }
    }
    return 1;
}

static int parse_target_url(const char *url, vsi_rp_mapping_t *mapping) {
    if (!url || !mapping || url[0] == '\0') {
        return -1;
    }

    const char *host_start = url;
    if (strncasecmp(host_start, "http://", 7) == 0) {
        host_start += 7;
    }

    const char *slash_ptr = strchr(host_start, '/');
    const char *colon_ptr = strchr(host_start, ':');

    if (colon_ptr && (!slash_ptr || colon_ptr < slash_ptr)) {
        size_t hlen = (size_t)(colon_ptr - host_start);
        if (hlen == 0) return -1;
        if (hlen >= VSI_RP_HOST_SIZE) hlen = VSI_RP_HOST_SIZE - 1;
        memcpy(mapping->target_host, host_start, hlen);
        mapping->target_host[hlen] = '\0';

        const char *port_start = colon_ptr + 1;
        size_t plen = slash_ptr ? (size_t)(slash_ptr - port_start) : strlen(port_start);
        if (plen == 0) return -1;
        if (plen >= VSI_RP_PORT_SIZE) plen = VSI_RP_PORT_SIZE - 1;
        memcpy(mapping->target_port, port_start, plen);
        mapping->target_port[plen] = '\0';
    } else {
        size_t hlen = slash_ptr ? (size_t)(slash_ptr - host_start) : strlen(host_start);
        if (hlen == 0) return -1;
        if (hlen >= VSI_RP_HOST_SIZE) hlen = VSI_RP_HOST_SIZE - 1;
        memcpy(mapping->target_host, host_start, hlen);
        mapping->target_host[hlen] = '\0';

        snprintf(mapping->target_port, VSI_RP_PORT_SIZE, "80");
    }

    return 0;
}

void vsi_config_load_default(vsi_config_t *cfg, int port) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(vsi_config_t));
    cfg->port = (port > 0 && port <= 65535) ? port : VSI_DEFAULT_PORT;
    cfg->backlog = VSI_DEFAULT_BACKLOG;
    cfg->mode = VSI_MODE_FORWARD_PROXY;
}

int vsi_config_parse_args(vsi_config_t *cfg, int argc, char **argv) {
    if (!cfg) return -1;
    vsi_config_load_default(cfg, VSI_DEFAULT_PORT);

    int has_mapping_flags = 0;
    for (int i = 1; i < argc; i++) {
        if (is_mapping_flag(argv[i])) {
            has_mapping_flags = 1;
            if (i + 2 >= argc) {
                fprintf(stderr, "[CONFIG] %s requires 2 arguments: <listen_port> <target_url>\n", argv[i]);
                return -1;
            }
            if (cfg->mapping_count >= VSI_MAX_RP_MAPPINGS) {
                fprintf(stderr, "[CONFIG] Maximum of %d reverse proxy mappings exceeded\n", VSI_MAX_RP_MAPPINGS);
                return -1;
            }

            int listen_port = atoi(argv[i + 1]);
            if (listen_port <= 0 || listen_port > 65535) {
                fprintf(stderr, "[CONFIG] Invalid listen port '%s' for %s\n", argv[i + 1], argv[i]);
                return -1;
            }

            vsi_rp_mapping_t *m = &cfg->mappings[cfg->mapping_count];
            m->listen_port = listen_port;
            if (parse_target_url(argv[i + 2], m) != 0) {
                fprintf(stderr, "[CONFIG] Invalid target URL '%s' for %s\n", argv[i + 2], argv[i]);
                return -1;
            }

            cfg->mapping_count++;
            i += 2;
        }
    }

    if (has_mapping_flags) {
        cfg->mode = VSI_MODE_REVERSE_PROXY;
        return 0;
    }

    if (argc > 1) {
        int port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "[CONFIG] Invalid port specified '%s'. Defaulting to %d\n",
                    argv[1], VSI_DEFAULT_PORT);
            port = VSI_DEFAULT_PORT;
        }
        cfg->port = port;
    }

    return 0;
}

vsi_connection_cb vsi_config_resolve_handler(vsi_service_mode_t mode) {
    switch (mode) {
        case VSI_MODE_FORWARD_PROXY:
            return forward_proxy_on_connection;
        case VSI_MODE_REVERSE_PROXY:
            return reverse_proxy_on_connection;
        default:
            return NULL;
    }
}

const char* vsi_mode_to_str(vsi_service_mode_t mode) {
    switch (mode) {
        case VSI_MODE_FORWARD_PROXY:
            return "FORWARD_PROXY";
        case VSI_MODE_REVERSE_PROXY:
            return "REVERSE_PROXY";
        default:
            return "UNKNOWN";
    }
}
