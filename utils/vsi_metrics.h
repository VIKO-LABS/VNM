#ifndef VSI_METRICS_H
#define VSI_METRICS_H

#include <stddef.h>
#include <stdint.h>

#define VSI_DEFAULT_METRICS_IP   "0.0.0.0"
#define VSI_DEFAULT_METRICS_PORT 9100

/**
 * Initializes shared lock-free atomic metrics storage (via MAP_SHARED anonymous
 * mmap so forked multi-service children update the same counters seamlessly).
 * Must be called before fork() or starting any service.
 *
 * @return 0 on success, or -1 on error.
 */
int vsi_metrics_init(void);

/**
 * Releases shared metrics storage.
 */
void vsi_metrics_destroy(void);

/**
 * Returns monotonic clock timestamp in nanoseconds via CLOCK_MONOTONIC_RAW (~25 ns).
 */
uint64_t vsi_now_ns(void);

void vsi_metrics_conn_open(void);
void vsi_metrics_conn_close(void);

/**
 * Records a completed request's byte and latency metrics in a single lock-free pass.
 */
void vsi_metrics_record_request(size_t bytes_in,
                                size_t bytes_out,
                                uint64_t proxy_overhead_ns,
                                uint64_t upstream_wait_ns,
                                uint64_t total_ns,
                                int failed);

/**
 * Starts the lightweight local HTTP JSON /metrics server in a detached thread.
 *
 * @param listen_ip Bind IP (defaults to "127.0.0.1" if NULL/empty)
 * @param port      Bind port (defaults to 9100 if <= 0)
 * @return 0 on success, or -1 on error.
 */
int vsi_metrics_start_server(const char *listen_ip, int port);

#endif // VSI_METRICS_H
