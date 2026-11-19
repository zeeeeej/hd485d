#ifndef HD485_H
#define HD485_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One thread per handle; callers sharing a handle must serialize all operations. */
typedef struct hd485 hd485;
typedef struct {
    uint32_t baud, bits, parity, stops;
} hd485_config;
typedef struct {
    uint32_t mode, owner, epoch, tx_queued, error, rx_sequence;
} hd485_status;
enum { HD485_OWNER = 1, HD485_TX_DONE = 2, HD485_RX_LOST = 3, HD485_CONFIG = 4, HD485_FAULT = 5 };
typedef struct {
    uint32_t type, a, b;
} hd485_event;
/* Errors return -1/NULL and set errno. timeout_ms bounds local RPC/read calls. */
hd485 *hd485_open(const char *socket_path, int timeout_ms);
/* Adopt an already connected Unix socket (including socketpair for embedding/tests).
 * Ownership transfers only on success. The socket must use the platform backend type. */
hd485 *hd485_open_fd(int connected_fd, int timeout_ms);
void hd485_close(hd485 *h);
int hd485_acquire(hd485 *h, uint32_t wait_ms);
int hd485_release(hd485 *h);
int hd485_subscribe(hd485 *h);
int hd485_unsubscribe(hd485 *h);
/* Success means queued; HD485_TX_DONE contains ticket in a, errno in b. */
ssize_t hd485_write(hd485 *h, const void *data, size_t length, uint32_t *ticket);
ssize_t hd485_read(hd485 *h, void *data, size_t capacity, uint32_t *sequence, uint32_t *epoch);
int hd485_get_config(hd485 *h, hd485_config *config);
int hd485_set_config(hd485 *h, const hd485_config *config);
int hd485_get_status(hd485 *h, hd485_status *status);
/* fd is borrowed. Check hd485_pending before poll: RPC may buffer RX/events. */
int hd485_get_event_fd(hd485 *h);
int hd485_pending(hd485 *h); /* bit 0: RX buffered; bit 1: event buffered */
/* Nonblocking; returns 1 for event, 0 if none, -1 on transport error. */
int hd485_next_event(hd485 *h, hd485_event *event);
#ifdef __cplusplus
}
#endif
#endif
