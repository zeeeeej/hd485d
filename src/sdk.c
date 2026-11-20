#include "hd485.h"
#include "wire.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>
typedef struct item {
    struct item *next;
    size_t n, off;
    unsigned char data[];
} item;
struct hd485 {
    int fd, timeout;
    uint32_t id;
    unsigned char input[HD_PACKET];
    size_t used;
    item *rx, *rx_tail, *ev, *ev_tail;
    size_t rx_bytes, ev_count;
    uint64_t dropped;
};
static int waitfd(int fd, short events, int64_t end) {
    struct pollfd p = {fd, events, 0};
    for (;;) {
        int64_t left = end - now_ms();
        if (left < 0)
            left = 0;
        int r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR)
            continue;
        if (!r) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (r < 0)
            return -1;
        if (p.revents & events)
            return 0;
        errno = ECONNRESET;
        return -1;
    }
}
static int send_frame(hd485 *h, const unsigned char *p, size_t n, int64_t end) {
    size_t off = 0;
    while (off < n) {
        if (waitfd(h->fd, POLLOUT, end))
            return -1;
#ifdef MSG_NOSIGNAL
        ssize_t r = send(h->fd, p + off, n - off, MSG_NOSIGNAL);
#else
        ssize_t r = send(h->fd, p + off, n - off, 0);
#endif
        if (r < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return -1;
        }
        if (!r) {
            errno = EPIPE;
            return -1;
        }
        off += (size_t)r;
#ifdef __linux__
        if (off != n) {
            errno = EPROTO;
            return -1;
        }
#endif
    }
    return 0;
}
static int receive(hd485 *h, unsigned char *out, int64_t end) {
    for (;;) {
#ifndef __linux__
        int check = frame_check(h->input, h->used, "I485");
        if (check < 0) {
            errno = EPROTO;
            return -1;
        }
        if (check) {
            size_t n = HD_HEAD + rd32(h->input + 16);
            memcpy(out, h->input, n);
            memmove(h->input, h->input + n, h->used - n);
            h->used -= n;
            return 0;
        }
#endif
        if (waitfd(h->fd, POLLIN, end))
            return -1;
#ifdef __linux__
        ssize_t n = recv(h->fd, out, HD_PACKET, MSG_TRUNC);
        if (n > 0) {
            if (n > (ssize_t)HD_PACKET || frame_check(out, (size_t)n, "I485") != 1 ||
                (size_t)n != HD_HEAD + rd32(out + 16)) {
                errno = EPROTO;
                return -1;
            }
            return 0;
        }
#else
        ssize_t n = recv(h->fd, h->input + h->used, HD_PACKET - h->used, 0);
        if (n > 0) {
            h->used += (size_t)n;
            continue;
        }
#endif
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (!n)
            errno = ECONNRESET;
        return -1;
    }
}
static void pop(item **head, item **tail) {
    item *i = *head;
    *head = i->next;
    if (!*head)
        *tail = NULL;
    free(i);
}
static int stash(hd485 *h, const unsigned char *f) {
    size_t n = rd32(f + 16);
    int rx = f[5] == RX;
    if ((rx && n < 8) || (!rx && (f[5] != EVENT || n != 12))) {
        errno = EPROTO;
        return -1;
    }
    if (rx) {
        while (h->rx && h->rx_bytes + n > 65536) {
            h->dropped += h->rx->n - h->rx->off;
            h->rx_bytes -= h->rx->n;
            pop(&h->rx, &h->rx_tail);
        }
    } else if (h->ev_count >= 1024) {
        errno = ENOBUFS;
        return -1;
    }
    item *i = malloc(sizeof(*i) + n);
    if (!i)
        return -1;
    i->n = n;
    i->off = rx ? 8 : 0;
    i->next = NULL;
    memcpy(i->data, f + HD_HEAD, n);
    item **head = rx ? &h->rx : &h->ev, **tail = rx ? &h->rx_tail : &h->ev_tail;
    if (*tail)
        (*tail)->next = i;
    else
        *head = i;
    *tail = i;
    if (rx)
        h->rx_bytes += n;
    else
        h->ev_count++;
    return 0;
}
static int rpc(hd485 *h, unsigned op, const void *p, size_t n, unsigned char *out, size_t *len,
               int timeout, uint32_t *ticket) {
    if (!h || n > HD_MAX) {
        errno = EINVAL;
        return -1;
    }
    unsigned char f[HD_PACKET];
    uint32_t id = ++h->id;
    if (!id)
        id = ++h->id;
    int64_t end = now_ms() + timeout;
    size_t size = frame_make(f, "I485", op, 0, 0, id, p, n);
    if (send_frame(h, f, size, end))
        return -1;
    for (;;) {
        if (receive(h, f, end))
            return -1;
        if (f[5] == RX || f[5] == EVENT) {
            if (stash(h, f))
                return -1;
            continue;
        }
        if (rd32(f + 12) != id)
            continue;
        size_t got = rd32(f + 16);
        if (f[5] != (op | 0x80) || got < 4) {
            errno = EPROTO;
            return -1;
        }
        uint32_t e = rd32(f + HD_HEAD);
        if (e) {
            errno = (int)e;
            return -1;
        }
        got -= 4;
        if (len) {
            if (got > *len) {
                errno = EMSGSIZE;
                return -1;
            }
            *len = got;
            if (got && out)
                memcpy(out, f + HD_HEAD + 4, got);
        }
        if (ticket)
            *ticket = id;
        return 0;
    }
}
hd485 *hd485_open_fd(int fd, int timeout) {
    int type = 0;
    socklen_t n = sizeof(type);
    if (timeout < 1 || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &n) < 0)
        return NULL;
    if (type != HD_SOCK) {
        errno = EPROTOTYPE;
        return NULL;
    }
    hd485 *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->fd = fd;
    h->timeout = timeout;
    if (nonblock(fd) < 0) {
        free(h);
        return NULL;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    return h;
}
hd485 *hd485_open(const char *path, int timeout) {
    if (!path || timeout < 1 || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = EINVAL;
        return NULL;
    }
    hd485 *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->fd = socket(AF_UNIX, HD_SOCK, 0);
    if (h->fd < 0) {
        free(h);
        return NULL;
    }
    fcntl(h->fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(h->fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    if (connect(h->fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        int e = errno;
        close(h->fd);
        free(h);
        errno = e;
        return NULL;
    }
    nonblock(h->fd);
    h->timeout = timeout;
    return h;
}
void hd485_close(hd485 *h) {
    if (!h)
        return;
    close(h->fd);
    while (h->rx)
        pop(&h->rx, &h->rx_tail);
    while (h->ev)
        pop(&h->ev, &h->ev_tail);
    free(h);
}
int hd485_acquire(hd485 *h, uint32_t wait) {
    unsigned char b[4];
    if (wait > 3600000) {
        errno = EINVAL;
        return -1;
    }
    wr32(b, wait);
    return rpc(h, ACQUIRE, b, 4, NULL, NULL, (int)wait + h->timeout, NULL);
}
int hd485_release(hd485 *h) {
    return rpc(h, RELEASE, NULL, 0, NULL, NULL, h->timeout, NULL);
}
int hd485_subscribe(hd485 *h) {
    return rpc(h, SUBSCRIBE, NULL, 0, NULL, NULL, h->timeout, NULL);
}
int hd485_unsubscribe(hd485 *h) {
    return rpc(h, UNSUBSCRIBE, NULL, 0, NULL, NULL, h->timeout, NULL);
}
ssize_t hd485_write(hd485 *h, const void *p, size_t n, uint32_t *ticket) {
    if (!p || !n || n > HD_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    unsigned char b[4];
    size_t len = 4;
    if (rpc(h, WRITE, p, n, b, &len, h->timeout, ticket))
        return -1;
    if (len != 4) {
        errno = EPROTO;
        return -1;
    }
    return rd32(b);
}
ssize_t hd485_read(hd485 *h, void *p, size_t cap, uint32_t *seq, uint32_t *epoch) {
    if (!p || !cap) {
        errno = EINVAL;
        return -1;
    }
    int64_t end = now_ms() + h->timeout;
    unsigned char f[HD_PACKET];
    while (!h->rx) {
        if (receive(h, f, end) || stash(h, f))
            return -1;
    }
    item *i = h->rx;
    size_t n = i->n - i->off;
    if (n > cap)
        n = cap;
    memcpy(p, i->data + i->off, n);
    i->off += n;
    if (seq)
        *seq = rd32(i->data);
    if (epoch)
        *epoch = rd32(i->data + 4);
    if (i->off == i->n) {
        h->rx_bytes -= i->n;
        pop(&h->rx, &h->rx_tail);
    }
    return (ssize_t)n;
}
int hd485_get_config(hd485 *h, hd485_config *c) {
    unsigned char b[16];
    size_t n = 16;
    if (!c) {
        errno = EINVAL;
        return -1;
    }
    if (rpc(h, GET_CONFIG, NULL, 0, b, &n, h->timeout, NULL))
        return -1;
    if (n != 16) {
        errno = EPROTO;
        return -1;
    }
    *c = (hd485_config){rd32(b), rd32(b + 4), rd32(b + 8), rd32(b + 12)};
    return 0;
}
int hd485_set_config(hd485 *h, const hd485_config *c) {
    if (!c) {
        errno = EINVAL;
        return -1;
    }
    unsigned char b[16];
    wr32(b, c->baud);
    wr32(b + 4, c->bits);
    wr32(b + 8, c->parity);
    wr32(b + 12, c->stops);
    return rpc(h, SET_CONFIG, b, 16, NULL, NULL, h->timeout, NULL);
}
int hd485_get_status(hd485 *h, hd485_status *s) {
    unsigned char b[24];
    size_t n = 24;
    if (!s) {
        errno = EINVAL;
        return -1;
    }
    if (rpc(h, STATUS, NULL, 0, b, &n, h->timeout, NULL))
        return -1;
    if (n != 24) {
        errno = EPROTO;
        return -1;
    }
    *s =
        (hd485_status){rd32(b), rd32(b + 4), rd32(b + 8), rd32(b + 12), rd32(b + 16), rd32(b + 20)};
    return 0;
}
int hd485_get_event_fd(hd485 *h) {
    return h->fd;
}
int hd485_pending(hd485 *h) {
    return (h->rx ? 1 : 0) | ((h->ev || h->dropped) ? 2 : 0);
}
int hd485_next_event(hd485 *h, hd485_event *e) {
    if (!e) {
        errno = EINVAL;
        return -1;
    }
    if (h->dropped) {
        *e = (hd485_event){HD485_RX_LOST, (uint32_t)h->dropped, (uint32_t)(h->dropped >> 32)};
        h->dropped = 0;
        return 1;
    }
    unsigned char f[HD_PACKET];
    for (int i = 0; !h->ev && i < 16; i++) {
        if (receive(h, f, now_ms())) {
            if (errno == ETIMEDOUT)
                return 0;
            return -1;
        }
        if (stash(h, f))
            return -1;
    }
    if (!h->ev)
        return 0;
    *e = (hd485_event){rd32(h->ev->data), rd32(h->ev->data + 4), rd32(h->ev->data + 8)};
    pop(&h->ev, &h->ev_tail);
    h->ev_count--;
    return 1;
}

int hd485_get_capabilities(hd485 *h, uint32_t *flags) {
    unsigned char b[4];size_t n=sizeof(b);
    if(!flags){errno=EINVAL;return -1;}
    if(rpc(h,CAPABILITIES,NULL,0,b,&n,h->timeout,NULL))return -1;
    if(n!=4){errno=EPROTO;return -1;}*flags=rd32(b);return 0;
}
int hd485_rx_raw_begin(hd485 *h,uint64_t bytes) {
    unsigned char b[8];wr64(b,bytes);
    return rpc(h,RX_RAW_BEGIN,b,sizeof(b),NULL,NULL,h->timeout,NULL);
}
int hd485_rx_raw_end(hd485 *h) {
    return rpc(h,RX_RAW_END,NULL,0,NULL,NULL,h->timeout,NULL);
}

int hd485_tx_begin(hd485 *h, uint32_t total, uint32_t *ticket) {
    if (!total || total > 16u*1024u*1024u) {errno=EMSGSIZE;return -1;}
    unsigned char b[4];wr32(b,total);
    return rpc(h,TX_BEGIN,b,4,NULL,NULL,h->timeout,ticket);
}
int hd485_tx_data(hd485 *h, const void *data, size_t length) {
    if (!data || !length || length>HD_MAX) {errno=EMSGSIZE;return -1;}
    return rpc(h,TX_DATA,data,length,NULL,NULL,h->timeout,NULL);
}
int hd485_tx_abort(hd485 *h) {
    return rpc(h,TX_ABORT,NULL,0,NULL,NULL,h->timeout,NULL);
}
