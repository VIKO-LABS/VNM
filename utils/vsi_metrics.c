#include "vsi_metrics.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

typedef struct {
    _Alignas(64) atomic_uint_fast64_t total_requests;
    _Alignas(64) atomic_uint_fast64_t active_connections;
    _Alignas(64) atomic_uint_fast64_t failed_requests;
    _Alignas(64) atomic_uint_fast64_t bytes_inbound;
    _Alignas(64) atomic_uint_fast64_t bytes_outbound;
    _Alignas(64) atomic_uint_fast64_t total_proxy_overhead_ns;
    _Alignas(64) atomic_uint_fast64_t max_proxy_overhead_ns;
    _Alignas(64) atomic_uint_fast64_t total_upstream_wait_ns;
    _Alignas(64) atomic_uint_fast64_t total_latency_ns;
} vsi_metrics_store_t;

typedef struct {
    char ip[64];
    int  port;
} vsi_metrics_srv_cfg_t;

static vsi_metrics_store_t *g_metrics = NULL;

int vsi_metrics_init(void) {
    if (g_metrics != NULL) {
        return 0;
    }

    void *mem = mmap(NULL, sizeof(vsi_metrics_store_t),
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("[METRICS] mmap failed");
        return -1;
    }

    g_metrics = (vsi_metrics_store_t *)mem;
    atomic_init(&g_metrics->total_requests, 0);
    atomic_init(&g_metrics->active_connections, 0);
    atomic_init(&g_metrics->failed_requests, 0);
    atomic_init(&g_metrics->bytes_inbound, 0);
    atomic_init(&g_metrics->bytes_outbound, 0);
    atomic_init(&g_metrics->total_proxy_overhead_ns, 0);
    atomic_init(&g_metrics->max_proxy_overhead_ns, 0);
    atomic_init(&g_metrics->total_upstream_wait_ns, 0);
    atomic_init(&g_metrics->total_latency_ns, 0);
    return 0;
}

void vsi_metrics_destroy(void) {
    if (g_metrics != NULL) {
        munmap(g_metrics, sizeof(vsi_metrics_store_t));
        g_metrics = NULL;
    }
}

uint64_t vsi_now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

void vsi_metrics_conn_open(void) {
    if (!g_metrics) return;
    atomic_fetch_add_explicit(&g_metrics->active_connections, 1, memory_order_relaxed);
}

void vsi_metrics_conn_close(void) {
    if (!g_metrics) return;
    atomic_fetch_sub_explicit(&g_metrics->active_connections, 1, memory_order_relaxed);
}

void vsi_metrics_record_request(size_t bytes_in,
                                size_t bytes_out,
                                uint64_t proxy_overhead_ns,
                                uint64_t upstream_wait_ns,
                                uint64_t total_ns,
                                int failed) {
    if (!g_metrics) return;

    atomic_fetch_add_explicit(&g_metrics->total_requests, 1, memory_order_relaxed);
    if (failed) {
        atomic_fetch_add_explicit(&g_metrics->failed_requests, 1, memory_order_relaxed);
    }
    if (bytes_in > 0) {
        atomic_fetch_add_explicit(&g_metrics->bytes_inbound, (uint64_t)bytes_in, memory_order_relaxed);
    }
    if (bytes_out > 0) {
        atomic_fetch_add_explicit(&g_metrics->bytes_outbound, (uint64_t)bytes_out, memory_order_relaxed);
    }

    atomic_fetch_add_explicit(&g_metrics->total_proxy_overhead_ns, proxy_overhead_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_metrics->total_upstream_wait_ns, upstream_wait_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_metrics->total_latency_ns, total_ns, memory_order_relaxed);

    uint64_t prev_max = atomic_load_explicit(&g_metrics->max_proxy_overhead_ns, memory_order_relaxed);
    while (proxy_overhead_ns > prev_max &&
           !atomic_compare_exchange_weak_explicit(&g_metrics->max_proxy_overhead_ns,
                                                  &prev_max,
                                                  proxy_overhead_ns,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static char* build_metrics_json(void) {
    uint64_t total_req   = g_metrics ? atomic_load_explicit(&g_metrics->total_requests, memory_order_relaxed) : 0;
    uint64_t active_conn = g_metrics ? atomic_load_explicit(&g_metrics->active_connections, memory_order_relaxed) : 0;
    uint64_t failed_req  = g_metrics ? atomic_load_explicit(&g_metrics->failed_requests, memory_order_relaxed) : 0;
    uint64_t bytes_in    = g_metrics ? atomic_load_explicit(&g_metrics->bytes_inbound, memory_order_relaxed) : 0;
    uint64_t bytes_out   = g_metrics ? atomic_load_explicit(&g_metrics->bytes_outbound, memory_order_relaxed) : 0;
    uint64_t sum_ovh_ns  = g_metrics ? atomic_load_explicit(&g_metrics->total_proxy_overhead_ns, memory_order_relaxed) : 0;
    uint64_t max_ovh_ns  = g_metrics ? atomic_load_explicit(&g_metrics->max_proxy_overhead_ns, memory_order_relaxed) : 0;
    uint64_t sum_up_ns   = g_metrics ? atomic_load_explicit(&g_metrics->total_upstream_wait_ns, memory_order_relaxed) : 0;
    uint64_t sum_tot_ns  = g_metrics ? atomic_load_explicit(&g_metrics->total_latency_ns, memory_order_relaxed) : 0;

    double avg_tot_us = (total_req > 0) ? ((double)sum_tot_ns / (double)total_req) / 1000.0 : 0.0;
    double avg_up_us  = (total_req > 0) ? ((double)sum_up_ns / (double)total_req) / 1000.0 : 0.0;
    double avg_ovh_us = (total_req > 0) ? ((double)sum_ovh_ns / (double)total_req) / 1000.0 : 0.0;
    double max_ovh_us = (double)max_ovh_ns / 1000.0;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON *requests = cJSON_AddObjectToObject(root, "requests");
    cJSON_AddNumberToObject(requests, "total", (double)total_req);
    cJSON_AddNumberToObject(requests, "active", (double)active_conn);
    cJSON_AddNumberToObject(requests, "failed", (double)failed_req);

    cJSON *traffic = cJSON_AddObjectToObject(root, "traffic_bytes");
    cJSON_AddNumberToObject(traffic, "inbound", (double)bytes_in);
    cJSON_AddNumberToObject(traffic, "outbound", (double)bytes_out);

    cJSON *latency = cJSON_AddObjectToObject(root, "latency_us");
    cJSON_AddNumberToObject(latency, "avg_total", avg_tot_us);
    cJSON_AddNumberToObject(latency, "avg_upstream_wait", avg_up_us);
    cJSON_AddNumberToObject(latency, "avg_proxy_overhead", avg_ovh_us);
    cJSON_AddNumberToObject(latency, "max_proxy_overhead", max_ovh_us);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    return json_str;
}

static void write_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, buf + sent, len - sent);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        sent += (size_t)w;
    }
}

static void* metrics_server_thread(void *arg) {
    vsi_metrics_srv_cfg_t *cfg = (vsi_metrics_srv_cfg_t *)arg;
    char ip[64];
    snprintf(ip, sizeof(ip), "%s", cfg->ip);
    int port = cfg->port;
    free(cfg);

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("[METRICS] socket() failed");
        return NULL;
    }

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (strcmp(ip, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[METRICS] Invalid listen_ip '%s'\n", ip);
        close(srv_fd);
        return NULL;
    }

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[METRICS] bind() failed");
        close(srv_fd);
        return NULL;
    }

    if (listen(srv_fd, 16) < 0) {
        perror("[METRICS] listen() failed");
        close(srv_fd);
        return NULL;
    }

    printf("[METRICS] JSON endpoint listening on http://%s:%d/metrics\n", ip, port);
    fflush(stdout);

    while (1) {
        int cfd = accept(srv_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        char req_buf[1024];
        ssize_t r = read(cfd, req_buf, sizeof(req_buf) - 1);
        if (r > 0) {
            req_buf[r] = '\0';
            if (strncmp(req_buf, "GET /metrics ", 13) == 0 ||
                strncmp(req_buf, "GET /metrics?", 13) == 0) {
                char *body = build_metrics_json();
                if (body) {
                    size_t body_len = strlen(body);
                    char hdr[256];
                    int hdr_len = snprintf(hdr, sizeof(hdr),
                                           "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: application/json\r\n"
                                           "Content-Length: %zu\r\n"
                                           "Connection: close\r\n\r\n",
                                           body_len);
                    if (hdr_len > 0) {
                        write_all(cfd, hdr, (size_t)hdr_len);
                        write_all(cfd, body, body_len);
                    }
                    free(body);
                } else {
                    const char *err = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    write_all(cfd, err, strlen(err));
                }
            } else {
                const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
                write_all(cfd, nf, strlen(nf));
            }
        }
        close(cfd);
    }

    close(srv_fd);
    return NULL;
}

int vsi_metrics_start_server(const char *listen_ip, int port) {
    vsi_metrics_srv_cfg_t *cfg = malloc(sizeof(vsi_metrics_srv_cfg_t));
    if (!cfg) return -1;

    snprintf(cfg->ip, sizeof(cfg->ip), "%s",
             (listen_ip && listen_ip[0] != '\0') ? listen_ip : VSI_DEFAULT_METRICS_IP);
    cfg->port = (port > 0 && port <= 65535) ? port : VSI_DEFAULT_METRICS_PORT;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t tid;
    int rc = pthread_create(&tid, &attr, metrics_server_thread, cfg);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        free(cfg);
        return -1;
    }
    return 0;
}
