#include "vsi_config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../services/forwardProxy/forwardProxy.h"
#include "../../services/reverseProxy/reverseProxy.h"
#include "../../utils/cJSON.h"
#include "../../utils/vsi_metrics.h"

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

static int validate_ipv4_or_wildcard(const char *ip) {
    if (!ip || ip[0] == '\0') return 0;
    if (strcmp(ip, "0.0.0.0") == 0) return 1;
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1;
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

static char* read_entire_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("[CONFIG] Failed to open JSON config file");
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0 || len > (1024L * 1024L)) {
        fprintf(stderr, "[CONFIG] Invalid config file size\n");
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if (read_bytes != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

void vsi_config_load_default(vsi_config_t *cfg, int port) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(vsi_config_t));
    cfg->backlog = VSI_DEFAULT_BACKLOG;

    cfg->forward_proxy.enabled = 1;
    snprintf(cfg->forward_proxy.listen_ip, sizeof(cfg->forward_proxy.listen_ip), "%s", VSI_DEFAULT_LISTEN_IP);
    cfg->forward_proxy.listen_port = (port > 0 && port <= 65535) ? port : VSI_DEFAULT_PORT;

    cfg->metrics.enabled = 1;
    snprintf(cfg->metrics.listen_ip, sizeof(cfg->metrics.listen_ip), "%s", VSI_DEFAULT_METRICS_IP);
    cfg->metrics.listen_port = VSI_DEFAULT_METRICS_PORT;

    cfg->active_service_count = 1;
}

int vsi_config_validate_ports(const vsi_config_t *cfg) {
    if (!cfg) return -1;

    typedef struct {
        const char *ip;
        int         port;
        const char *label;
    } vsi_endpoint_t;

    vsi_endpoint_t eps[2 + VSI_MAX_RP_MAPPINGS];
    int ep_count = 0;

    if (cfg->forward_proxy.enabled) {
        eps[ep_count].ip = cfg->forward_proxy.listen_ip;
        eps[ep_count].port = cfg->forward_proxy.listen_port;
        eps[ep_count].label = "forwardProxy";
        ep_count++;
    }

    if (cfg->reverse_proxy.enabled) {
        for (int i = 0; i < cfg->reverse_proxy.mapping_count; i++) {
            eps[ep_count].ip = cfg->reverse_proxy.mappings[i].listen_ip;
            eps[ep_count].port = cfg->reverse_proxy.mappings[i].listen_port;
            eps[ep_count].label = "reverseProxy";
            ep_count++;
        }
    }

    if (cfg->metrics.enabled) {
        eps[ep_count].ip = cfg->metrics.listen_ip;
        eps[ep_count].port = cfg->metrics.listen_port;
        eps[ep_count].label = "metrics";
        ep_count++;
    }

    for (int i = 0; i < ep_count; i++) {
        for (int j = i + 1; j < ep_count; j++) {
            if (eps[i].port == eps[j].port) {
                int ip_conflict = (strcmp(eps[i].ip, eps[j].ip) == 0) ||
                                  (strcmp(eps[i].ip, "0.0.0.0") == 0) ||
                                  (strcmp(eps[j].ip, "0.0.0.0") == 0);
                if (ip_conflict) {
                    fprintf(stderr,
                            "[CONFIG] Port conflict detected: %s (%s:%d) conflicts with %s (%s:%d)\n",
                            eps[i].label, eps[i].ip, eps[i].port,
                            eps[j].label, eps[j].ip, eps[j].port);
                    return -1;
                }
            }
        }
    }

    return 0;
}

int vsi_config_load_json(vsi_config_t *cfg, const char *json_path) {
    if (!cfg || !json_path) return -1;

    memset(cfg, 0, sizeof(vsi_config_t));
    cfg->backlog = VSI_DEFAULT_BACKLOG;

    char *raw = read_entire_file(json_path);
    if (!raw) {
        fprintf(stderr, "[CONFIG] Unable to load '%s'\n", json_path);
        return -1;
    }

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        const char *err_ptr = cJSON_GetErrorPtr();
        fprintf(stderr, "[CONFIG] JSON syntax error near: %s\n", err_ptr ? err_ptr : "unknown");
        return -1;
    }

    // Parse engine settings
    const cJSON *engine = cJSON_GetObjectItemCaseSensitive(root, "engine");
    if (cJSON_IsObject(engine)) {
        const cJSON *backlog_item = cJSON_GetObjectItemCaseSensitive(engine, "backlog");
        if (cJSON_IsNumber(backlog_item) && backlog_item->valueint > 0) {
            cfg->backlog = backlog_item->valueint;
        }
    }

    const cJSON *services = cJSON_GetObjectItemCaseSensitive(root, "services");
    if (!cJSON_IsObject(services)) {
        fprintf(stderr, "[CONFIG] Missing or invalid 'services' object in %s\n", json_path);
        cJSON_Delete(root);
        return -1;
    }

    // Parse forwardProxy configuration
    const cJSON *fp = cJSON_GetObjectItemCaseSensitive(services, "forwardProxy");
    if (cJSON_IsObject(fp)) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(fp, "enabled");
        if (cJSON_IsBool(enabled) && cJSON_IsTrue(enabled)) {
            cfg->forward_proxy.enabled = 1;

            const cJSON *ip = cJSON_GetObjectItemCaseSensitive(fp, "listen_ip");
            const char *ip_str = (cJSON_IsString(ip) && ip->valuestring && ip->valuestring[0] != '\0')
                                 ? ip->valuestring : VSI_DEFAULT_LISTEN_IP;
            if (!validate_ipv4_or_wildcard(ip_str)) {
                fprintf(stderr, "[CONFIG] Invalid forwardProxy listen_ip '%s'\n", ip_str);
                cJSON_Delete(root);
                return -1;
            }
            snprintf(cfg->forward_proxy.listen_ip, sizeof(cfg->forward_proxy.listen_ip), "%s", ip_str);

            const cJSON *port = cJSON_GetObjectItemCaseSensitive(fp, "listen_port");
            if (!cJSON_IsNumber(port) || port->valueint <= 0 || port->valueint > 65535) {
                fprintf(stderr, "[CONFIG] forwardProxy enabled but 'listen_port' is missing or invalid (1..65535)\n");
                cJSON_Delete(root);
                return -1;
            }
            cfg->forward_proxy.listen_port = port->valueint;
            cfg->active_service_count++;
        }
    }

    // Parse reverseProxy configuration
    const cJSON *rp = cJSON_GetObjectItemCaseSensitive(services, "reverseProxy");
    if (cJSON_IsObject(rp)) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(rp, "enabled");
        if (cJSON_IsBool(enabled) && cJSON_IsTrue(enabled)) {
            cfg->reverse_proxy.enabled = 1;

            const cJSON *mappings = cJSON_GetObjectItemCaseSensitive(rp, "mappings");
            if (!cJSON_IsArray(mappings) || cJSON_GetArraySize(mappings) == 0) {
                fprintf(stderr, "[CONFIG] reverseProxy enabled but 'mappings' array is empty or missing\n");
                cJSON_Delete(root);
                return -1;
            }

            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, mappings) {
                if (cfg->reverse_proxy.mapping_count >= VSI_MAX_RP_MAPPINGS) {
                    fprintf(stderr, "[CONFIG] Maximum of %d reverseProxy mappings exceeded\n", VSI_MAX_RP_MAPPINGS);
                    cJSON_Delete(root);
                    return -1;
                }
                if (!cJSON_IsObject(entry)) {
                    fprintf(stderr, "[CONFIG] Invalid reverseProxy mapping entry (expected object)\n");
                    cJSON_Delete(root);
                    return -1;
                }

                vsi_rp_mapping_t *m = &cfg->reverse_proxy.mappings[cfg->reverse_proxy.mapping_count];
                const cJSON *ip = cJSON_GetObjectItemCaseSensitive(entry, "listen_ip");
                const char *ip_str = (cJSON_IsString(ip) && ip->valuestring && ip->valuestring[0] != '\0')
                                     ? ip->valuestring : VSI_DEFAULT_LISTEN_IP;
                if (!validate_ipv4_or_wildcard(ip_str)) {
                    fprintf(stderr, "[CONFIG] Invalid reverseProxy mapping listen_ip '%s'\n", ip_str);
                    cJSON_Delete(root);
                    return -1;
                }
                snprintf(m->listen_ip, sizeof(m->listen_ip), "%s", ip_str);

                const cJSON *port = cJSON_GetObjectItemCaseSensitive(entry, "listen_port");
                if (!cJSON_IsNumber(port) || port->valueint <= 0 || port->valueint > 65535) {
                    fprintf(stderr, "[CONFIG] Invalid reverseProxy mapping 'listen_port'\n");
                    cJSON_Delete(root);
                    return -1;
                }
                m->listen_port = port->valueint;

                const cJSON *target = cJSON_GetObjectItemCaseSensitive(entry, "target");
                if (!cJSON_IsString(target) || !target->valuestring ||
                    parse_target_url(target->valuestring, m) != 0) {
                    fprintf(stderr, "[CONFIG] Invalid reverseProxy mapping 'target'\n");
                    cJSON_Delete(root);
                    return -1;
                }

                cfg->reverse_proxy.mapping_count++;
            }

            cfg->active_service_count++;
        }
    }

    // Parse optional metrics endpoint configuration (defaults to enabled on 127.0.0.1:9100)
    cfg->metrics.enabled = 1;
    snprintf(cfg->metrics.listen_ip, sizeof(cfg->metrics.listen_ip), "%s", VSI_DEFAULT_METRICS_IP);
    cfg->metrics.listen_port = VSI_DEFAULT_METRICS_PORT;

    const cJSON *metrics = cJSON_GetObjectItemCaseSensitive(root, "metrics");
    if (cJSON_IsObject(metrics)) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(metrics, "enabled");
        if (cJSON_IsBool(enabled)) {
            cfg->metrics.enabled = cJSON_IsTrue(enabled) ? 1 : 0;
        }
        if (cfg->metrics.enabled) {
            const cJSON *ip = cJSON_GetObjectItemCaseSensitive(metrics, "listen_ip");
            const char *ip_str = (cJSON_IsString(ip) && ip->valuestring && ip->valuestring[0] != '\0')
                                 ? ip->valuestring : VSI_DEFAULT_METRICS_IP;
            if (!validate_ipv4_or_wildcard(ip_str)) {
                fprintf(stderr, "[CONFIG] Invalid metrics listen_ip '%s'\n", ip_str);
                cJSON_Delete(root);
                return -1;
            }
            snprintf(cfg->metrics.listen_ip, sizeof(cfg->metrics.listen_ip), "%s", ip_str);

            const cJSON *port = cJSON_GetObjectItemCaseSensitive(metrics, "listen_port");
            if (cJSON_IsNumber(port)) {
                if (port->valueint <= 0 || port->valueint > 65535) {
                    fprintf(stderr, "[CONFIG] Invalid metrics listen_port %d\n", port->valueint);
                    cJSON_Delete(root);
                    return -1;
                }
                cfg->metrics.listen_port = port->valueint;
            }
        }
    }

    cJSON_Delete(root);

    if (cfg->active_service_count == 0) {
        fprintf(stderr, "[CONFIG] No services are enabled in '%s'. Enable at least one service.\n", json_path);
        return -1;
    }

    return vsi_config_validate_ports(cfg);
}

int vsi_config_parse_args(vsi_config_t *cfg, int argc, char **argv) {
    if (!cfg) return -1;

    // 1. Default: load config.json from working directory
    if (argc <= 1) {
        return vsi_config_load_json(cfg, VSI_DEFAULT_CONFIG_PATH);
    }

    // 2. Explicit JSON config path: -c <path> or --config <path>
    if (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "--config") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[CONFIG] %s requires a file path argument\n", argv[1]);
            return -1;
        }
        return vsi_config_load_json(cfg, argv[2]);
    }

    // 3. Backwards-compatible CLI overrides (-M<n> for reverse proxy, or <port> for forward proxy)
    memset(cfg, 0, sizeof(vsi_config_t));
    cfg->backlog = VSI_DEFAULT_BACKLOG;
    cfg->metrics.enabled = 1;
    snprintf(cfg->metrics.listen_ip, sizeof(cfg->metrics.listen_ip), "%s", VSI_DEFAULT_METRICS_IP);
    cfg->metrics.listen_port = VSI_DEFAULT_METRICS_PORT;

    int has_mapping_flags = 0;
    for (int i = 1; i < argc; i++) {
        if (is_mapping_flag(argv[i])) {
            has_mapping_flags = 1;
            if (i + 2 >= argc) {
                fprintf(stderr, "[CONFIG] %s requires 2 arguments: <listen_port> <target_url>\n", argv[i]);
                return -1;
            }
            if (cfg->reverse_proxy.mapping_count >= VSI_MAX_RP_MAPPINGS) {
                fprintf(stderr, "[CONFIG] Maximum of %d reverse proxy mappings exceeded\n", VSI_MAX_RP_MAPPINGS);
                return -1;
            }

            int listen_port = atoi(argv[i + 1]);
            if (listen_port <= 0 || listen_port > 65535) {
                fprintf(stderr, "[CONFIG] Invalid listen port '%s' for %s\n", argv[i + 1], argv[i]);
                return -1;
            }

            vsi_rp_mapping_t *m = &cfg->reverse_proxy.mappings[cfg->reverse_proxy.mapping_count];
            snprintf(m->listen_ip, sizeof(m->listen_ip), "%s", VSI_DEFAULT_LISTEN_IP);
            m->listen_port = listen_port;
            if (parse_target_url(argv[i + 2], m) != 0) {
                fprintf(stderr, "[CONFIG] Invalid target URL '%s' for %s\n", argv[i + 2], argv[i]);
                return -1;
            }

            cfg->reverse_proxy.mapping_count++;
            i += 2;
        }
    }

    if (has_mapping_flags) {
        cfg->reverse_proxy.enabled = 1;
        cfg->active_service_count = 1;
        return vsi_config_validate_ports(cfg);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[CONFIG] Invalid port specified '%s'. Defaulting to %d\n",
                argv[1], VSI_DEFAULT_PORT);
        port = VSI_DEFAULT_PORT;
    }
    vsi_config_load_default(cfg, port);
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
