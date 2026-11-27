#ifndef VSI_CONFIG_H
#define VSI_CONFIG_H

#include "../transport/vsi_transport.h"
#include "../../services/reverseProxy/reverseProxy.h"

#define VSI_DEFAULT_CONFIG_PATH "config.json"
#define VSI_MAX_RP_MAPPINGS     16

/**
 * Active VSI service modes.
 */
typedef enum {
    VSI_MODE_FORWARD_PROXY,
    VSI_MODE_REVERSE_PROXY
} vsi_service_mode_t;

typedef struct {
    int  enabled;
    char listen_ip[VSI_LISTEN_IP_SIZE];
    int  listen_port;
} vsi_fp_config_t;

typedef struct {
    int              enabled;
    vsi_rp_mapping_t mappings[VSI_MAX_RP_MAPPINGS];
    int              mapping_count;
} vsi_rp_config_t;

typedef struct {
    int  enabled;
    char listen_ip[VSI_LISTEN_IP_SIZE];
    int  listen_port;
} vsi_metrics_config_t;

/**
 * Engine startup configuration loaded from config.json or CLI arguments.
 */
typedef struct {
    int                  backlog;
    vsi_fp_config_t      forward_proxy;
    vsi_rp_config_t      reverse_proxy;
    vsi_metrics_config_t metrics;
    int                  active_service_count;
} vsi_config_t;

/**
 * Loads default configuration values (Forward Proxy disabled until configured, backlog=128).
 */
void vsi_config_load_default(vsi_config_t *cfg, int port);

/**
 * Validates that no two enabled service listeners share the same (ip, port) tuple
 * or overlap via wildcard 0.0.0.0 on the same port.
 *
 * @return 0 if no conflicts exist, or -1 on conflict.
 */
int vsi_config_validate_ports(const vsi_config_t *cfg);

/**
 * Loads engine and service settings from a JSON configuration file via cJSON.
 *
 * @return 0 on valid configuration with >= 1 active service, or -1 on error.
 */
int vsi_config_load_json(vsi_config_t *cfg, const char *json_path);

/**
 * Parses CLI arguments:
 * - No args: loads default "config.json"
 * - -c / --config <path>: loads specified JSON config file
 * - -M1 <port> <target_url> ...: legacy reverse proxy CLI override
 * - <port>: legacy forward proxy CLI override
 *
 * @return 0 on success, or -1 on error.
 */
int vsi_config_parse_args(vsi_config_t *cfg, int argc, char **argv);

/**
 * Resolves the L4 connection callback for a given VSI service mode.
 */
vsi_connection_cb vsi_config_resolve_handler(vsi_service_mode_t mode);

/**
 * Returns a human-readable label for a service mode.
 */
const char* vsi_mode_to_str(vsi_service_mode_t mode);

#endif // VSI_CONFIG_H
