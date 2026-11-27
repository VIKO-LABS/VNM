#ifndef VSI_TRANSPORT_H
#define VSI_TRANSPORT_H

#include <stddef.h>

#define VSI_DEFAULT_LISTEN_IP "0.0.0.0"
#define VSI_LISTEN_IP_SIZE    64
#define VSI_DEFAULT_PORT      8085
#define VSI_DEFAULT_BACKLOG   128

/**
 * Callback invoked when a new client TCP connection is accepted.
 * The handler owns the client_fd and is responsible for closing it when done.
 *
 * @param client_fd The accepted client socket file descriptor
 * @param user_ctx  Optional per-listener context pointer (e.g. reverse proxy mapping)
 */
typedef void (*vsi_connection_cb)(int client_fd, void *user_ctx);

/**
 * Transport layer configuration.
 */
typedef struct {
    char listen_ip[VSI_LISTEN_IP_SIZE]; /* Bind IP address (default "0.0.0.0") */
    int port;                           /* Listening port (e.g. 8085) */
    int backlog;                        /* Kernel pending connection queue limit */
    vsi_connection_cb on_connection;    /* Service connection callback */
    void *user_ctx;                     /* Optional context passed to on_connection */
} vsi_transport_config_t;

/**
 * Initializes the Layer 4 TCP listener and enters the connection acceptance loop.
 *
 * @param config Transport configuration settings
 * @return 0 on clean shutdown, or non-zero on fatal error
 */
int vsi_transport_start(const vsi_transport_config_t *config);

#endif // VSI_TRANSPORT_H
