#ifndef HD_SERIAL_H
#define HD_SERIAL_H
#include "wire.h"
#include <limits.h>
#include <pthread.h>
typedef struct {
    uint32_t baud, bits, parity, stops;
} port_config;
typedef struct {
    int fd, gpio_fd, backend, busy, ready, result, stopping, started, exclusive;
    int lock_fd;
    char lock_path[PATH_MAX];
    int64_t start;
    size_t len;
    unsigned char data[HD_MAX];
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
void serial_close(serial_port *s);
#endif
