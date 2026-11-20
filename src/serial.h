#ifndef HD_SERIAL_H
#define HD_SERIAL_H
#include "wire.h"
#include <limits.h>
#include <pthread.h>
#define HD_TX_RING 32768u
typedef struct {
    uint32_t baud, bits, parity, stops;
} port_config;
typedef struct {
    int fd, gpio_fd, backend, busy, ready, result, stopping, started, exclusive;
    int lock_fd, tx_policy, tx_priority_changed;
    struct sched_param tx_priority;
    char lock_path[PATH_MAX];
    int64_t start;
    size_t len;
    unsigned char data[HD_TX_RING];
    size_t head, queued, accepted;
    int stream, abort_error, notify[2], trace;
    uint32_t ticket;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    port_config config;
} serial_port;
int serial_open(serial_port *s, const char *path, const char *backend, int gpio, port_config cfg,
                const char *lock_dir);
int serial_configure(serial_port *s, port_config cfg);
int serial_submit(serial_port *s, const void *p, size_t n);
int serial_result(serial_port *s, int *result);
int serial_stream_begin(serial_port *s, size_t total, uint32_t ticket);
int serial_stream_data(serial_port *s, const void *p, size_t n);
void serial_abort(serial_port *s, int error);
void serial_notifications(serial_port *s);
void serial_close(serial_port *s);
#endif
