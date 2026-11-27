#include <stdio.h>
#include <stdlib.h>
#include "../CoreEngine/transport/vsi_transport.h"
#include "../CoreEngine/config/vsi_config.h"

int main(int argc, char **argv) {
    // Dynamically retrieve port from CLI arguments, falling back to default 8085
    int port = (argc > 1) ? atoi(argv[1]) : VSI_DEFAULT_PORT;
    if (argc > 1 && (port <= 0 || port > 65535)) {
        fprintf(stderr, "[MAIN] Invalid port specified '%s'. Defaulting to %d\n", argv[1], VSI_DEFAULT_PORT);
        port = VSI_DEFAULT_PORT;
    }

    vsi_config_t cfg;
    vsi_config_load_default(&cfg, port);

    vsi_connection_cb handler = vsi_config_resolve_handler(cfg.mode);
    if (!handler) {
        fprintf(stderr, "[MAIN] Fatal: No handler registered for mode %s\n", vsi_mode_to_str(cfg.mode));
        return 1;
    }

    vsi_transport_config_t transport_cfg = {
        .port = cfg.port,
        .backlog = cfg.backlog,
        .on_connection = handler
    };

    printf("====================================================\n");
    printf("        VSI Core Engine (vsi_ngine)                 \n");
    printf("        Mode: %s | Port: %d | Backlog: %d\n",
           vsi_mode_to_str(cfg.mode), cfg.port, cfg.backlog);
    printf("====================================================\n");

    return vsi_transport_start(&transport_cfg);
}
