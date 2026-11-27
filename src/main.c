#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "../CoreEngine/transport/vsi_transport.h"
#include "../CoreEngine/config/vsi_config.h"

static void* reverse_proxy_listener_thread(void *arg) {
    const vsi_transport_config_t *tcfg = (const vsi_transport_config_t *)arg;
    vsi_transport_start(tcfg);
    return NULL;
}

int main(int argc, char **argv) {
    vsi_config_t cfg;
    if (vsi_config_parse_args(&cfg, argc, argv) != 0) {
        fprintf(stderr, "Usage:\n"
                        "  Forward Proxy : %s [port]\n"
                        "  Reverse Proxy : %s -M1 <port> <target_url> [-M2 <port> <target_url> ...]\n",
                argv[0], argv[0]);
        return 1;
    }

    vsi_connection_cb handler = vsi_config_resolve_handler(cfg.mode);
    if (!handler) {
        fprintf(stderr, "[MAIN] Fatal: No handler registered for mode %s\n", vsi_mode_to_str(cfg.mode));
        return 1;
    }

    printf("====================================================\n");
    printf("        VSI Core Engine (vsi_ngine)                 \n");
    printf("        Mode: %s | Backlog: %d\n", vsi_mode_to_str(cfg.mode), cfg.backlog);

    if (cfg.mode == VSI_MODE_REVERSE_PROXY) {
        vsi_transport_config_t transport_cfgs[VSI_MAX_RP_MAPPINGS];
        for (int i = 0; i < cfg.mapping_count; i++) {
            transport_cfgs[i].port = cfg.mappings[i].listen_port;
            transport_cfgs[i].backlog = cfg.backlog;
            transport_cfgs[i].on_connection = handler;
            transport_cfgs[i].user_ctx = &cfg.mappings[i];
            printf("        [M%d] :%d -> %s:%s\n",
                   i + 1,
                   cfg.mappings[i].listen_port,
                   cfg.mappings[i].target_host,
                   cfg.mappings[i].target_port);
        }
        printf("====================================================\n");

        if (cfg.mapping_count == 1) {
            return vsi_transport_start(&transport_cfgs[0]);
        }

        pthread_t listener_threads[VSI_MAX_RP_MAPPINGS];
        for (int i = 0; i < cfg.mapping_count; i++) {
            if (pthread_create(&listener_threads[i], NULL,
                               reverse_proxy_listener_thread, &transport_cfgs[i]) != 0) {
                perror("[MAIN] Failed to start reverse proxy listener thread");
                return 1;
            }
        }
        for (int i = 0; i < cfg.mapping_count; i++) {
            pthread_join(listener_threads[i], NULL);
        }
        return 0;
    }

    vsi_transport_config_t transport_cfg = {
        .port = cfg.port,
        .backlog = cfg.backlog,
        .on_connection = handler,
        .user_ctx = NULL
    };

    printf("        Port: %d\n", cfg.port);
    printf("====================================================\n");

    return vsi_transport_start(&transport_cfg);
}
