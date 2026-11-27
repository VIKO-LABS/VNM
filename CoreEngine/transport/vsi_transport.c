#include "vsi_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

typedef struct {
    int client_fd;
    vsi_connection_cb on_connection;
    void *user_ctx;
} vsi_worker_arg_t;

static void* vsi_transport_worker(void *arg) {
    vsi_worker_arg_t *warg = (vsi_worker_arg_t *)arg;
    int client_fd = warg->client_fd;
    vsi_connection_cb callback = warg->on_connection;
    void *user_ctx = warg->user_ctx;
    free(warg);

    if (callback) {
        callback(client_fd, user_ctx);
    } else {
        close(client_fd);
    }
    return NULL;
}

int vsi_transport_start(const vsi_transport_config_t *config) {
    if (!config) {
        fprintf(stderr, "[TRANSPORT] Fatal: NULL transport config provided\n");
        return -1;
    }

    int port = (config->port > 0 && config->port <= 65535) ? config->port : VSI_DEFAULT_PORT;
    int backlog = (config->backlog > 0) ? config->backlog : VSI_DEFAULT_BACKLOG;

    // 1. Isolate broken socket signals so aborted streams do not kill the server
    signal(SIGPIPE, SIG_IGN);

    // 2. Create the Layer 4 TCP socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[TRANSPORT] Socket creation failed");
        return -1;
    }

    // 3. Allow immediate address/port reuse on server restarts
    int opt = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[TRANSPORT] setsockopt(SO_REUSEADDR) failed");
        close(sock_fd);
        return -1;
    }

    // 4. Bind to configured IPv4 address (or INADDR_ANY for 0.0.0.0 / empty)
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    const char *bind_ip = (config->listen_ip[0] != '\0') ? config->listen_ip : VSI_DEFAULT_LISTEN_IP;
    if (strcmp(bind_ip, "0.0.0.0") == 0) {
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_ip, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "[TRANSPORT] Invalid listen_ip '%s'\n", bind_ip);
        close(sock_fd);
        return -1;
    }

    if (bind(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[TRANSPORT] Socket bind failed");
        close(sock_fd);
        return -1;
    }

    // 5. Enter passive listening state with configured kernel Accept queue depth
    if (listen(sock_fd, backlog) < 0) {
        perror("[TRANSPORT] Socket listen failed");
        close(sock_fd);
        return -1;
    }

    printf("[TRANSPORT] Layer 4 TCP Listener online on %s:%d (backlog=%d)\n", bind_ip, port, backlog);

    // 6. Configure detached worker thread attributes
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    // 7. Core Connection Acceptance Loop
    int client_init_count =0;
    while (1) {

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(sock_fd, (struct sockaddr *)&client_addr, &addr_len);
        printf("client connected count is %d\n",++client_init_count);


        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("[TRANSPORT] accept() failed");
            continue;
        }

        vsi_worker_arg_t *warg = malloc(sizeof(vsi_worker_arg_t));
        if (!warg) {
            fprintf(stderr, "[TRANSPORT] Out of memory allocating worker argument\n");
            close(client_fd);
            continue;
        }
        warg->client_fd = client_fd;
        warg->on_connection = config->on_connection;
        warg->user_ctx = config->user_ctx;

        pthread_t worker_tid;
        if (pthread_create(&worker_tid, &attr, vsi_transport_worker, warg) != 0) {
            perror("[TRANSPORT] pthread_create() failed");
            close(client_fd);
            free(warg);
            continue;
        }
    }

    pthread_attr_destroy(&attr);
    close(sock_fd);
    return 0;
}
