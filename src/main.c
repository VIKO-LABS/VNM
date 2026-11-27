#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../CoreEngine/transport/vsi_transport.h"
#include "../CoreEngine/config/vsi_config.h"
#include "../utils/vsi_metrics.h"

#define VSI_MAX_SERVICES 8

static volatile sig_atomic_t g_shutdown_requested = 0;

static void on_supervisor_signal(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

static void* reverse_proxy_listener_thread(void *arg) {
    const vsi_transport_config_t *tcfg = (const vsi_transport_config_t *)arg;
    vsi_transport_start(tcfg);
    return NULL;
}

static int run_forward_proxy_service(const vsi_config_t *cfg) {
    vsi_transport_config_t transport_cfg = {0};
    snprintf(transport_cfg.listen_ip, sizeof(transport_cfg.listen_ip), "%s", cfg->forward_proxy.listen_ip);
    transport_cfg.port = cfg->forward_proxy.listen_port;
    transport_cfg.backlog = cfg->backlog;
    transport_cfg.on_connection = vsi_config_resolve_handler(VSI_MODE_FORWARD_PROXY);
    transport_cfg.user_ctx = NULL;

    return vsi_transport_start(&transport_cfg);
}

static int run_reverse_proxy_service(const vsi_config_t *cfg) {
    vsi_connection_cb handler = vsi_config_resolve_handler(VSI_MODE_REVERSE_PROXY);
    vsi_transport_config_t transport_cfgs[VSI_MAX_RP_MAPPINGS];

    for (int i = 0; i < cfg->reverse_proxy.mapping_count; i++) {
        memset(&transport_cfgs[i], 0, sizeof(transport_cfgs[i]));
        snprintf(transport_cfgs[i].listen_ip, sizeof(transport_cfgs[i].listen_ip),
                 "%s", cfg->reverse_proxy.mappings[i].listen_ip);
        transport_cfgs[i].port = cfg->reverse_proxy.mappings[i].listen_port;
        transport_cfgs[i].backlog = cfg->backlog;
        transport_cfgs[i].on_connection = handler;
        transport_cfgs[i].user_ctx = (void *)&cfg->reverse_proxy.mappings[i];
    }

    if (cfg->reverse_proxy.mapping_count == 1) {
        return vsi_transport_start(&transport_cfgs[0]);
    }

    pthread_t listener_threads[VSI_MAX_RP_MAPPINGS];
    for (int i = 0; i < cfg->reverse_proxy.mapping_count; i++) {
        if (pthread_create(&listener_threads[i], NULL,
                           reverse_proxy_listener_thread, &transport_cfgs[i]) != 0) {
            perror("[MAIN] Failed to start reverse proxy listener thread");
            return 1;
        }
    }
    for (int i = 0; i < cfg->reverse_proxy.mapping_count; i++) {
        pthread_join(listener_threads[i], NULL);
    }
    return 0;
}

static void print_startup_banner(const vsi_config_t *cfg) {
    printf("====================================================\n");
    printf("        VSI Core Engine (vsi_ngine)                 \n");
    printf("        Active Services: %d | Backlog: %d\n",
           cfg->active_service_count, cfg->backlog);
    if (cfg->forward_proxy.enabled) {
        printf("        [FORWARD_PROXY] %s:%d\n",
               cfg->forward_proxy.listen_ip, cfg->forward_proxy.listen_port);
    }
    if (cfg->reverse_proxy.enabled) {
        for (int i = 0; i < cfg->reverse_proxy.mapping_count; i++) {
            const vsi_rp_mapping_t *m = &cfg->reverse_proxy.mappings[i];
            printf("        [REVERSE_PROXY M%d] %s:%d -> %s:%s\n",
                   i + 1, m->listen_ip, m->listen_port, m->target_host, m->target_port);
        }
    }
    if (cfg->metrics.enabled) {
        printf("        [METRICS] http://%s:%d/metrics\n",
               cfg->metrics.listen_ip, cfg->metrics.listen_port);
    }
    printf("====================================================\n");
    fflush(stdout);
}

static int run_multi_service_supervisor(const vsi_config_t *cfg) {
    pid_t child_pids[VSI_MAX_SERVICES];
    const char *child_names[VSI_MAX_SERVICES];
    int child_count = 0;

    // Fork MUST happen while main is single-threaded (before any pthread_create)
    if (cfg->forward_proxy.enabled) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("[SUPERVISOR] fork() failed for forwardProxy");
            for (int i = 0; i < child_count; i++) kill(child_pids[i], SIGTERM);
            return 1;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            int rc = run_forward_proxy_service(cfg);
            _exit(rc == 0 ? 0 : 1);
        }
        printf("[SUPERVISOR] Forked FORWARD_PROXY service (PID %d)\n", pid);
        child_pids[child_count] = pid;
        child_names[child_count] = "FORWARD_PROXY";
        child_count++;
    }

    if (cfg->reverse_proxy.enabled) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("[SUPERVISOR] fork() failed for reverseProxy");
            for (int i = 0; i < child_count; i++) kill(child_pids[i], SIGTERM);
            return 1;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            int rc = run_reverse_proxy_service(cfg);
            _exit(rc == 0 ? 0 : 1);
        }
        printf("[SUPERVISOR] Forked REVERSE_PROXY service (PID %d)\n", pid);
        child_pids[child_count] = pid;
        child_names[child_count] = "REVERSE_PROXY";
        child_count++;
    }

    fflush(stdout);

    if (cfg->metrics.enabled) {
        vsi_metrics_start_server(cfg->metrics.listen_ip, cfg->metrics.listen_port);
    }

    // Install async-signal-safe handlers without SA_RESTART so waitpid() wakes on signal
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_supervisor_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int alive = child_count;
    int forwarded_shutdown = 0;

    while (alive > 0) {
        int status = 0;
        pid_t exited_pid = waitpid(-1, &status, 0);

        if (exited_pid > 0) {
            for (int i = 0; i < child_count; i++) {
                if (child_pids[i] == exited_pid) {
                    printf("[SUPERVISOR] Reaped %s (PID %d)\n", child_names[i], exited_pid);
                    child_pids[i] = -1;
                    alive--;
                    break;
                }
            }
        } else if (exited_pid < 0 && errno == EINTR) {
            if (g_shutdown_requested && !forwarded_shutdown) {
                printf("\n[SUPERVISOR] Shutdown signal received; stopping child services...\n");
                fflush(stdout);
                for (int i = 0; i < child_count; i++) {
                    if (child_pids[i] > 0) {
                        kill(child_pids[i], SIGTERM);
                    }
                }
                forwarded_shutdown = 1;
            }
        } else {
            break;
        }
    }

    printf("[SUPERVISOR] All child services exited cleanly.\n");
    return 0;
}

int main(int argc, char **argv) {
    vsi_config_t cfg;
    if (vsi_config_parse_args(&cfg, argc, argv) != 0) {
        fprintf(stderr, "Usage:\n"
                        "  JSON Config (default config.json) : %s [-c <config.json>]\n"
                        "  Forward Proxy (CLI override)      : %s <port>\n"
                        "  Reverse Proxy (CLI override)      : %s -M1 <port> <target_url> [-M2 ...]\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    print_startup_banner(&cfg);

    if (cfg.metrics.enabled && vsi_metrics_init() != 0) {
        return 1;
    }

    // Single active service runs directly in the main process (no fork overhead)
    if (cfg.active_service_count == 1) {
        if (cfg.metrics.enabled) {
            vsi_metrics_start_server(cfg.metrics.listen_ip, cfg.metrics.listen_port);
        }
        int rc = cfg.forward_proxy.enabled
                 ? run_forward_proxy_service(&cfg)
                 : run_reverse_proxy_service(&cfg);
        vsi_metrics_destroy();
        return rc;
    }

    // Multiple active services spawn isolated processes supervised by main.
    // Note: if metrics enabled, fork children first while single-threaded, then start
    // the metrics HTTP endpoint inside the supervisor or let supervisor fork cleanly.
    return run_multi_service_supervisor(&cfg);
}
