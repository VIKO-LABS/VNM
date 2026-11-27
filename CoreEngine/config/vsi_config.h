#ifndef VSI_CONFIG_H
#define VSI_CONFIG_H

#include "../transport/vsi_transport.h"
#include "../../services/reverseProxy/reverseProxy.h"

#define VSI_MAX_RP_MAPPINGS 16

/**
 * Active VSI service modes configured at startup.
 */
typedef enum {
    VSI_MODE_FORWARD_PROXY,
    VSI_MODE_REVERSE_PROXY
} vsi_service_mode_t;

/**
 * Engine startup configuration.
 */
typedef struct {
    int port;
    int backlog;
    vsi_service_mode_t mode;
    vsi_rp_mapping_t mappings[VSI_MAX_RP_MAPPINGS];
    int mapping_count;
} vsi_config_t;

/**
 * Loads default configuration values, applying the given port (or VSI_DEFAULT_PORT if <= 0).
 */
void vsi_config_load_default(vsi_config_t *cfg, int port);

/**
 * Parses CLI arguments into cfg.
 * Supports multi-mapping reverse proxy flags: -M1 <port> <target_url> -M2 <port> <target_url> ...
 * Falls back to Forward Proxy mode if no -M flags are provided.
 *
 * @return 0 on success, or -1 on invalid CLI arguments.
 */
int vsi_config_parse_args(vsi_config_t *cfg, int argc, char **argv);

/**
 * Resolves the L4 connection callback for the configured VSI service mode.
 *
 * @return Connection callback function pointer, or NULL if mode is unsupported
 */
vsi_connection_cb vsi_config_resolve_handler(vsi_service_mode_t mode);

/**
 * Returns a human-readable label for a service mode.
 */
const char* vsi_mode_to_str(vsi_service_mode_t mode);

#endif // VSI_CONFIG_H
