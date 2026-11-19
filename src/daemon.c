#include "maintenance.h"
#include "serial.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define CLIENTS 16
#define QUEUE_LIMIT 65536u
typedef struct message {
    struct message *next;
    size_t len, off;
    int rx;
    unsigned char data[];
} message;
typedef struct {
    int fd, sub, release_pending;
    uint32_t generation, wait_id, release_id;
    int64_t deadline;
    uint64_t ticket, dropped;
    unsigned char input[HD_PACKET];
    size_t used, queued;
    message *head, *tail;
} client;
typedef struct tx {
    struct tx *next;
    int who;
    uint32_t generation, id;
    size_t len;
    unsigned char data[];
} tx;
typedef struct {
    uint32_t id, crc;
    size_t len;
    unsigned char data[HD_HEAD + 256];
} cached;
typedef struct {
    serial_port serial;
    maintenance maint;
    client clients[CLIENTS];
    int listener, owner, coexist, fault, inflight, active_who;
    uint32_t active_generation, active_id, epoch, rxseq, session, last_id;
    uint64_t next_ticket, hello_nonce;
    int hello_valid;
    port_config idle;
    tx *tx_head, *tx_tail;
    size_t tx_bytes;
    unsigned char remote[HD_PACKET], pending[HD_HEAD + 256];
    size_t remote_used, pending_len;
    int64_t remote_last, pending_until;
    unsigned address, response_ms;
    cached cache[16];
    unsigned cache_next;
} server;
static volatile sig_atomic_t quitting;
static void signal_stop(int s) {
    (void)s;
    quitting = 1;
}
static void disconnect(server *s, int i) {
    client *c = &s->clients[i];
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->sub = 0;
    c->wait_id = 0;
    c->ticket = 0;
    c->used = 0;
    while (c->head) {
        message *m = c->head;
        c->head = m->next;
        free(m);
    }
    c->tail = NULL;
    c->queued = 0;
}
static int queue_message(server *s, int i, const unsigned char *p, size_t n, int rx) {
    client *c = &s->clients[i];
    if (c->fd < 0)
        return -1;
    while (c->queued + n > QUEUE_LIMIT) {
        message *prev = NULL, *m = c->head;
        while (m && (!m->rx || m->off)) {
            prev = m;
            m = m->next;
        }
        if (!m) {
            if (rx) {
                c->dropped += n - HD_HEAD - 8;
                return -1;
            }
            disconnect(s, i);
            return -1;
        }
        if (prev)
            prev->next = m->next;
        else
            c->head = m->next;
        if (c->tail == m)
            c->tail = prev;
        c->queued -= m->len;
        c->dropped += m->len - HD_HEAD - 8;
        free(m);
    }
    message *m = malloc(sizeof(*m) + n);
    if (!m) {
        disconnect(s, i);
        return -1;
    }
    m->next = NULL;
    m->len = n;
    m->off = 0;
    m->rx = rx;
    memcpy(m->data, p, n);
    if (c->tail)
        c->tail->next = m;
    else
        c->head = m;
    c->tail = m;
    c->queued += n;
    return 0;
}
static void response(server *s, int i, unsigned op, uint32_t id, int err, const void *p, size_t n) {
    unsigned char b[HD_PACKET], d[HD_MAX];
    if (n > HD_MAX - 4)
        return;
    wr32(d, (uint32_t)err);
    if (n)
        memcpy(d + 4, p, n);
    size_t len = frame_make(b, "I485", op | 0x80, 0, 0, id, d, n + 4);
    queue_message(s, i, b, len, 0);
}
static void event_one(server *s, int i, unsigned code, uint32_t a, uint32_t b) {
    unsigned char f[64], d[12];
    wr32(d, code);
    wr32(d + 4, a);
    wr32(d + 8, b);
    queue_message(s, i, f, frame_make(f, "I485", EVENT, 0, 0, 0, d, 12), 0);
}
static void event_all(server *s, unsigned code, uint32_t a, uint32_t b) {
    for (int i = 0; i < CLIENTS; i++)
        if (s->clients[i].fd >= 0)
            event_one(s, i, code, a, b);
}
static void reset_remote(server *s) {
    s->remote_used = s->pending_len = 0;
    s->session = s->last_id = 0;
    s->hello_valid = 0;
    memset(s->cache, 0, sizeof(s->cache));
}
static int maintenance_allowed(server *s) {
    return !s->fault && (s->coexist || s->owner < 0);
}
static int waiting(server *s) {
    for (int i = 0; i < CLIENTS; i++)
        if (s->clients[i].fd >= 0 && s->clients[i].wait_id)
            return 1;
    return 0;
}
static void preempt(server *s) {
    s->pending_len = 0;
    maint_cancel(&s->maint, 3);
    if (!s->coexist)
        reset_remote(s);
}
static void fatal_port(server *s, int error) {
    if (s->fault)
        return;
    s->fault = error;
    event_all(s, 5, (uint32_t)error, 0);
    preempt(s);
    while (s->tx_head) {
        tx *t = s->tx_head;
        s->tx_head = t->next;
        if (s->clients[t->who].fd >= 0 && s->clients[t->who].generation == t->generation)
            event_one(s, t->who, 2, t->id, (uint32_t)error);
        free(t);
    }
    s->tx_tail = NULL;
    s->tx_bytes = 0;
}
static void encode_config(unsigned char *p, port_config c) {
    wr32(p, c.baud);
    wr32(p + 4, c.bits);
    wr32(p + 8, c.parity);
    wr32(p + 12, c.stops);
}
static void ipc_request(server *s, int i, const unsigned char *f) {
    client *c = &s->clients[i];
    unsigned op = f[5];
    uint32_t id = rd32(f + 12);
    size_t n = rd32(f + 16);
    const unsigned char *p = f + HD_HEAD;
    int err = 0;
    unsigned char out[32];
    size_t len = 0;
    if (!id || rd16(f + 6) || rd32(f + 8)) {
        disconnect(s, i);
        return;
    }
    switch (op) {
    case ACQUIRE:
        if (n != 4) {
            err = EINVAL;
            break;
        }
        if (s->fault) {
            err = s->fault;
            break;
        }
        if (s->owner == i || c->wait_id) {
            err = EALREADY;
            break;
        }
        if ((s->owner >= 0 || waiting(s)) && !rd32(p)) {
            err = EBUSY;
            break;
        }
        if (rd32(p) > 3600000) {
            err = EINVAL;
            break;
        }
        c->wait_id = id;
        c->ticket = ++s->next_ticket;
        c->deadline = now_ms() + (rd32(p) ? rd32(p) : 2000);
        if (s->owner < 0)
            preempt(s);
        return;
    case RELEASE:
        if (n) {
            err = EINVAL;
            break;
        }
        if (s->owner != i) {
            err = EPERM;
            break;
        }
        if (c->release_pending) {
            err = EALREADY;
            break;
        }
        c->release_pending = 1;
        c->release_id = id;
        return;
    case SUBSCRIBE:
        if (n)
            err = EINVAL;
        else
            c->sub = 1;
        break;
    case UNSUBSCRIBE:
        if (n)
            err = EINVAL;
        else
            c->sub = 0;
        break;
    case WRITE: {
        if (s->owner != i || c->release_pending) {
            err = EPERM;
            break;
        }
        if (s->fault) {
            err = s->fault;
            break;
        }
        if (!n) {
            err = EINVAL;
            break;
        }
        if (s->tx_bytes + n > QUEUE_LIMIT) {
            err = EAGAIN;
            break;
        }
        tx *t = malloc(sizeof(*t) + n);
        if (!t) {
            err = ENOMEM;
            break;
        }
        t->next = NULL;
        t->who = i;
        t->generation = c->generation;
        t->id = id;
        t->len = n;
        memcpy(t->data, p, n);
        if (s->tx_tail)
            s->tx_tail->next = t;
        else
            s->tx_head = t;
        s->tx_tail = t;
        s->tx_bytes += n;
        preempt(s);
        wr32(out, (uint32_t)n);
        len = 4;
        break;
    }
    case GET_CONFIG:
        if (n)
            err = EINVAL;
        else {
            encode_config(out, s->serial.config);
            len = 16;
        }
        break;
    case SET_CONFIG: {
        if (n != 16) {
            err = EINVAL;
            break;
        }
        if (s->owner != i || c->release_pending) {
            err = EPERM;
            break;
        }
        if (s->fault) {
            err = s->fault;
            break;
        }
        if (s->inflight || s->tx_head) {
            err = EBUSY;
            break;
        }
        port_config cfg = {rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12)};
        if (serial_configure(&s->serial, cfg))
            err = errno;
        else {
            ++s->epoch;
            reset_remote(s);
            event_all(s, 4, s->epoch, cfg.baud);
        }
        break;
    }
    case STATUS:
        if (n)
            err = EINVAL;
        else {
            wr32(out, s->coexist);
            wr32(out + 4, (uint32_t)(s->owner + 1));
            wr32(out + 8, s->epoch);
            wr32(out + 12, (uint32_t)s->tx_bytes);
            wr32(out + 16, (uint32_t)s->fault);
            wr32(out + 20, s->rxseq);
            len = 24;
        }
        break;
    default:
        err = ENOTSUP;
    }
    response(s, i, op, id, err, out, len);
}
static void client_read(server *s, int i) {
    client *c = &s->clients[i];
#ifdef __linux__
    unsigned char frame[HD_PACKET];
    ssize_t n = recv(c->fd, frame, sizeof(frame), MSG_TRUNC);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            disconnect(s, i);
        return;
    }
    if (!n) {
        disconnect(s, i);
        return;
    }
    if (n > (ssize_t)sizeof(frame) || frame_check(frame, (size_t)n, "I485") != 1 ||
        (size_t)n != HD_HEAD + rd32(frame + 16)) {
        disconnect(s, i);
        return;
    }
    ipc_request(s, i, frame);
#else
    ssize_t n = read(c->fd, c->input + c->used, sizeof(c->input) - c->used);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            disconnect(s, i);
        return;
    }
    if (!n) {
        disconnect(s, i);
        return;
    }
    c->used += (size_t)n;
    while (c->fd >= 0) {
        int check = frame_check(c->input, c->used, "I485");
        if (check < 0) {
            disconnect(s, i);
            return;
        }
        if (!check)
            break;
        size_t size = HD_HEAD + rd32(c->input + 16);
        ipc_request(s, i, c->input);
        if (c->fd < 0)
            return;
        memmove(c->input, c->input + size, c->used - size);
        c->used -= size;
    }
#endif
}
static void client_write(server *s, int i) {
    client *c = &s->clients[i];
    for (int k = 0; k < 8 && c->head; k++) {
        message *m = c->head;
        ssize_t n = send(c->fd, m->data + m->off, m->len - m->off, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                disconnect(s, i);
            return;
        }
        if (!n)
            return;
#ifdef __linux__
        if ((size_t)n != m->len) {
            disconnect(s, i);
            return;
        }
#endif
        m->off += (size_t)n;
        if (m->off < m->len)
            return;
        c->head = m->next;
        if (!c->head)
            c->tail = NULL;
        c->queued -= m->len;
        free(m);
    }
    if (c->dropped && c->queued < QUEUE_LIMIT / 2) {
        uint64_t lost = c->dropped;
        c->dropped = 0;
        event_one(s, i, 3, (uint32_t)lost, (uint32_t)(lost >> 32));
    }
}
static uint32_t random_session(void) {
    uint32_t n = 0;
    int f = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (f >= 0) {
        if (read(f, &n, 4) != 4)
            n = 0;
        close(f);
    }
    return n;
}
static void remote_request(server *s, const unsigned char *f) {
    unsigned op = f[5];
    uint32_t session = rd32(f + 8), id = rd32(f + 12), crc = rd32(f + 20);
    size_t n = rd32(f + 16);
    const unsigned char *p = f + HD_HEAD;
    if (rd16(f + 6) != s->address || !id || op & 0x80 || !maintenance_allowed(s) || waiting(s) ||
        s->pending_len)
        return;
    unsigned char payload[256];
    size_t outlen = 0;
    int error = 0;
    if (op == M_HELLO) {
        if (n != 8 || session)
            return;
        uint64_t nonce = rd64(p);
        if (!s->hello_valid || nonce != s->hello_nonce) {
            maint_new_session(&s->maint);
            s->session = random_session();
            if (!s->session)
                return;
            s->hello_nonce = nonce;
            s->hello_valid = 1;
            s->last_id = 0;
            memset(s->cache, 0, sizeof(s->cache));
        }
        wr32(payload + 4, s->session);
        wr32(payload + 8, 240);
        outlen = 8;
    } else {
        if (!s->session || session != s->session)
            return;
        for (int k = 0; k < 16; k++)
            if (s->cache[k].id == id) {
                if (s->cache[k].crc != crc)
                    return;
                memcpy(s->pending, s->cache[k].data, s->cache[k].len);
                s->pending_len = s->cache[k].len;
                s->pending_until = now_ms() + s->response_ms;
                return;
            }
        if (id <= s->last_id)
            error = ESTALE;
        else {
            error = maint_request(&s->maint, op, p, n, payload + 4, &outlen);
            s->last_id = id;
        }
    }
    wr32(payload, (uint32_t)error);
    if (error)
        outlen = 0;
    s->pending_len = frame_make(s->pending, "H485", op | 0x80, (uint16_t)s->address, s->session, id,
                                payload, 4 + outlen);
    s->pending_until = now_ms() + s->response_ms;
    if (op != M_HELLO) {
        cached *c = &s->cache[s->cache_next++ % 16];
        c->id = id;
        c->crc = crc;
        c->len = s->pending_len;
        memcpy(c->data, s->pending, c->len);
    }
}
static void remote_feed(server *s, const unsigned char *p, size_t n) {
    if (!maintenance_allowed(s) || waiting(s)) {
        s->remote_used = 0;
        return;
    }
    s->remote_last = now_ms();
    while (n--) {
        if (s->remote_used == sizeof(s->remote)) {
            memmove(s->remote, s->remote + 1, --s->remote_used);
        }
        s->remote[s->remote_used++] = *p++;
        for (;;) {
            if (s->remote_used >= 4 && memcmp(s->remote, "H485", 4)) {
                memmove(s->remote, s->remote + 1, --s->remote_used);
                continue;
            }
            int r = frame_check(s->remote, s->remote_used, "H485");
            if (r < 0) {
                memmove(s->remote, s->remote + 1, --s->remote_used);
                continue;
            }
            if (!r)
                break;
            size_t size = HD_HEAD + rd32(s->remote + 16);
            remote_request(s, s->remote);
            memmove(s->remote, s->remote + size, s->remote_used - size);
            s->remote_used -= size;
        }
    }
}
static void uart_read(server *s) {
    unsigned char p[4096], b[HD_PACKET], d[4104];
    ssize_t n = read(s->serial.fd, p, sizeof(p));
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            fatal_port(s, errno);
        return;
    }
    if (!n)
        return;
    wr32(d, ++s->rxseq);
    wr32(d + 4, s->epoch);
    memcpy(d + 8, p, (size_t)n);
    size_t len = frame_make(b, "I485", RX, 0, 0, 0, d, (size_t)n + 8);
    for (int i = 0; i < CLIENTS; i++)
        if (s->clients[i].fd >= 0 && s->clients[i].sub)
            queue_message(s, i, b, len, 1);
    remote_feed(s, p, (size_t)n);
}
static void progress(server *s) {
    int result;
    if (s->inflight && serial_result(&s->serial, &result)) {
        s->inflight = 0;
        if (s->active_who >= 0) {
            client *c = &s->clients[s->active_who];
            if (c->fd >= 0 && c->generation == s->active_generation)
                event_one(s, s->active_who, 2, s->active_id, (uint32_t)result);
        }
        if (result)
            fatal_port(s, result);
    }
    if (s->inflight &&
        now_ms() - s->serial.start > 10000 + (int64_t)s->serial.len * 12000 / s->serial.config.baud)
        fatal_port(s, ETIMEDOUT);
    if (s->owner >= 0 && !s->inflight && !s->tx_head) {
        client *c = &s->clients[s->owner];
        if (c->fd < 0 || c->release_pending) {
            int err = 0;
            if (!s->fault && serial_configure(&s->serial, s->idle))
                err = errno;
            if (c->fd >= 0 && c->release_pending)
                response(s, s->owner, RELEASE, c->release_id, err, NULL, 0);
            c->release_pending = 0;
            s->owner = -1;
            reset_remote(s);
            ++s->epoch;
            if (err)
                fatal_port(s, err);
            event_all(s, 1, 0, s->epoch);
        }
    }
    int best = -1;
    for (int i = 0; i < CLIENTS; i++) {
        client *c = &s->clients[i];
        if (c->fd < 0 || !c->wait_id)
            continue;
        if (s->fault || now_ms() >= c->deadline) {
            response(s, i, ACQUIRE, c->wait_id, s->fault ? s->fault : ETIMEDOUT, NULL, 0);
            c->wait_id = 0;
            c->ticket = 0;
            continue;
        }
        if (best < 0 || c->ticket < s->clients[best].ticket)
            best = i;
    }
    if (best >= 0 && s->owner < 0 && !s->inflight && !s->fault) {
        client *c = &s->clients[best];
        s->owner = best;
        c->sub = 1;
        c->release_pending = 0;
        preempt(s);
        response(s, best, ACQUIRE, c->wait_id, 0, NULL, 0);
        c->wait_id = 0;
        c->ticket = 0;
        ++s->epoch;
        event_all(s, 1, (uint32_t)(best + 1), s->epoch);
    }
    if (s->pending_len && now_ms() >= s->pending_until)
        s->pending_len = 0;
    if (s->remote_used &&
        now_ms() - s->remote_last > 2000 + (int64_t)HD_PACKET * 12000 / s->serial.config.baud)
        s->remote_used = 0;
    if (!s->inflight && !s->fault) {
        if (s->tx_head) {
            tx *t = s->tx_head;
            s->tx_head = t->next;
            if (!s->tx_head)
                s->tx_tail = NULL;
            s->tx_bytes -= t->len;
            s->active_who = t->who;
            s->active_generation = t->generation;
            s->active_id = t->id;
            serial_submit(&s->serial, t->data, t->len);
            s->inflight = 1;
            free(t);
        } else if (s->pending_len && maintenance_allowed(s) && !waiting(s)) {
            int64_t wire_ms = (int64_t)s->pending_len * 12000 / s->serial.config.baud + 7;
            if (now_ms() + wire_ms < s->pending_until) {
                serial_submit(&s->serial, s->pending, s->pending_len);
                s->inflight = 1;
                s->active_who = -1;
            }
            s->pending_len = 0;
        }
    }
    maint_tick(&s->maint);
}
static void usage(void) {
    fprintf(stderr, "hd485d --device PATH --socket PATH --root DIR [--baud 230400] [--mode "
                    "idle|coexist] [--de none|gpio|kernel] [--gpio 35] [--address 1] "
                    "[--response-ms 500] [--allow-shell] [--command-timeout-ms 30000] "
                    "[--lock-dir /run] [--fs-root DIR]\n");
}
static int version_command(const char *args, unsigned char *out, size_t capacity, size_t *length,
                           void *context) {
    (void)context;
    if (*args)
        return 2;
    const char *text = "hd485d 0.2.1\n";
    *length = strlen(text);
    if (*length > capacity)
        return 1;
    memcpy(out, text, *length);
    return 0;
}
int main(int argc, char **argv) {
    const char *device = NULL, *sock = "/run/hd485.sock", *root = "/userdata/hd-rs485/files",
               *backend = "none", *lock_dir = "/run", *fsroot = NULL;
    int gpio = 35, allow = 0, timeout = 30000;
    int inherited = 0;
    server *s = calloc(1, sizeof(*s));
    if (!s)
        return 1;
    s->owner = s->listener = -1;
    s->idle = (port_config){230400, 8, 0, 1};
    s->address = 1;
    s->response_ms = 500;
    for (int i = 0; i < CLIENTS; i++)
        s->clients[i].fd = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--allow-shell")) {
            allow = 1;
            continue;
        }
        if (i + 1 == argc) {
            usage();
            free(s);
            return 2;
        }
        const char *k = argv[i], *v = argv[++i];
        if (!strcmp(k, "--device"))
            device = v;
        else if (!strcmp(k, "--socket"))
            sock = v;
        else if (!strcmp(k, "--root"))
            root = v;
        else if (!strcmp(k, "--fs-root"))
            fsroot = v;
        else if (!strcmp(k, "--de"))
            backend = v;
        else if (!strcmp(k, "--lock-dir"))
            lock_dir = v;
#ifdef HD485_TEST_IO
        else if (!strcmp(k, "--test-client-fds")) {
            char *copy = strdup(v);
            if (!copy)
                return 1;
            char *part = strtok(copy, ",");
            while (part && inherited < CLIENTS) {
                int fd = atoi(part);
                s->clients[inherited].fd = fd;
                s->clients[inherited++].generation = 1;
                nonblock(fd);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                part = strtok(NULL, ",");
            }
            free(copy);
        }
#endif
        else if (!strcmp(k, "--mode")) {
            if (strcmp(v, "idle") && strcmp(v, "coexist")) {
                usage();
                free(s);
                return 2;
            }
            s->coexist = !strcmp(v, "coexist");
        } else if (!strcmp(k, "--baud"))
            s->idle.baud = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "--gpio"))
            gpio = atoi(v);
        else if (!strcmp(k, "--address"))
            s->address = (unsigned)atoi(v);
        else if (!strcmp(k, "--response-ms"))
            s->response_ms = (unsigned)atoi(v);
        else if (!strcmp(k, "--command-timeout-ms"))
            timeout = atoi(v);
        else {
            usage();
            free(s);
            return 2;
        }
    }
    if (!device || strlen(sock) >= sizeof(((struct sockaddr_un *)0)->sun_path) || !s->address ||
        s->address > 65535 || s->response_ms < 20 || s->response_ms > 5000 || timeout < 100 ||
        timeout > 3600000 || gpio < 0) {
        usage();
        free(s);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);
    if (maint_init(&s->maint, root, allow, timeout)) {
        perror("maintenance root");
        free(s);
        return 1;
    }
    if (fsroot && maint_fs_init(&s->maint, fsroot)) {
        perror("filesystem root");
        maint_close(&s->maint);
        free(s);
        return 1;
    }
    maint_register_command(&s->maint, "version", version_command, NULL);
    if (serial_open(&s->serial, device, backend, gpio, s->idle, lock_dir)) {
        perror("serial");
        maint_close(&s->maint);
        free(s);
        return 1;
    }
    if (!inherited) {
        s->listener = socket(AF_UNIX, HD_SOCK, 0);
        if (s->listener < 0) {
            perror("socket");
            goto fail;
        }
        fcntl(s->listener, F_SETFD, FD_CLOEXEC);
        nonblock(s->listener);
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, sock);
        /* Never unlink an existing endpoint: it may belong to a live daemon. */
        mode_t oldmask = umask(0077);
        int bound = bind(s->listener, (struct sockaddr *)&addr, sizeof(addr));
        umask(oldmask);
        if (bound < 0) {
            perror("bind (remove stale socket only after checking owner)");
            goto fail;
        }
        if (listen(s->listener, CLIENTS) < 0) {
            perror("listen");
            unlink(sock);
            goto fail;
        }
    }
    fprintf(stderr, "hd485d 0.2.1: %s %u 8N1, mode=%s, de=%s, socket=%s\n", device, s->idle.baud,
            s->coexist ? "coexist" : "idle", backend, sock);
    while (!quitting) {
        struct pollfd p[CLIENTS + 2];
        p[0] = (struct pollfd){s->listener, POLLIN, 0};
        p[1] = (struct pollfd){s->fault ? -1 : s->serial.fd, POLLIN, 0};
        for (int i = 0; i < CLIENTS; i++)
            p[i + 2] = (struct pollfd){s->clients[i].fd,
                                       (short)(POLLIN | (s->clients[i].head ? POLLOUT : 0)), 0};
        int r = poll(p, CLIENTS + 2, 10);
        if (r < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
        if (p[0].revents & POLLIN) {
            int fd = accept(s->listener, NULL, NULL);
            if (fd >= 0) {
                nonblock(fd);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                int slot = -1;
                for (int i = 0; i < CLIENTS; i++)
                    if (s->clients[i].fd < 0 && s->owner != i) {
                        slot = i;
                        break;
                    }
                if (slot < 0)
                    close(fd);
                else {
                    client *c = &s->clients[slot];
                    uint32_t gen = c->generation + 1;
                    memset(c, 0, sizeof(*c));
                    c->fd = fd;
                    c->generation = gen;
                }
            }
        }
        for (int i = 0; i < CLIENTS; i++) {
            if (s->clients[i].fd != p[i + 2].fd)
                continue;
            if (p[i + 2].revents & POLLIN)
                client_read(s, i);
            if (s->clients[i].fd >= 0 && (p[i + 2].revents & POLLOUT))
                client_write(s, i);
            if (p[i + 2].revents & (POLLERR | POLLHUP | POLLNVAL))
                disconnect(s, i);
        }
        if (p[1].revents & POLLIN)
            uart_read(s);
        if (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))
            fatal_port(s, EIO);
        progress(s);
    }
    for (int i = 0; i < CLIENTS; i++)
        disconnect(s, i);
    if (s->listener >= 0) {
        close(s->listener);
        unlink(sock);
    }
    s->listener = -1;
    serial_close(&s->serial);
    maint_close(&s->maint);
    while (s->tx_head) {
        tx *t = s->tx_head;
        s->tx_head = t->next;
        free(t);
    }
    free(s);
    return 0;
fail:
    if (s->listener >= 0)
        close(s->listener);
    serial_close(&s->serial);
    maint_close(&s->maint);
    free(s);
    return 1;
}
