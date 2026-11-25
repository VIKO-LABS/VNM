#ifndef VSI_CONFIG_H
#define VSI_CONFIG_H

#include "../transport/vsi_transport.h"

/**
 * Active VSI service modes configured at startup.
 */
typedef enum {
    VSI_MODE_FORWARD_PROXY
} vsi_service_mode_t;

/**
 * Engine startup configuration.
 */
typedef struct {
    int port;
    int backlog;
    vsi_service_mode_t mode;
} vsi_config_t;

/**
 * Loads default configuration values, applying the given port (or VSI_DEFAULT_PORT if <= 0).
 */
void vsi_config_load_default(vsi_config_t *cfg, int port);

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
