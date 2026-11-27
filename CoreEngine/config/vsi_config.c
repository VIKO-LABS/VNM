#include "vsi_config.h"

#include <stddef.h>
#include "../../services/forwardProxy/forwardProxy.h"

void vsi_config_load_default(vsi_config_t *cfg, int port) {
    if (!cfg) return;
    cfg->port = (port > 0 && port <= 65535) ? port : VSI_DEFAULT_PORT;
    cfg->backlog = VSI_DEFAULT_BACKLOG;
    cfg->mode = VSI_MODE_FORWARD_PROXY;
}

vsi_connection_cb vsi_config_resolve_handler(vsi_service_mode_t mode) {
    switch (mode) {
        case VSI_MODE_FORWARD_PROXY:
            return forward_proxy_on_connection;
        default:
            return NULL;
    }
}

const char* vsi_mode_to_str(vsi_service_mode_t mode) {
    switch (mode) {
        case VSI_MODE_FORWARD_PROXY:
            return "FORWARD_PROXY";
        default:
            return "UNKNOWN";
    }
}
