#include "serial.h"
#include "tx_trace.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/serial.h>
#endif
static speed_t speed(uint32_t b) {
    switch (b) {
    case 1200:
        return B1200;
    case 2400:
        return B2400;
    case 4800:
        return B4800;
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    case 230400:
        return B230400;
#ifdef B460800
    case 460800:
        return B460800;
#endif
#ifdef B921600
    case 921600:
        return B921600;
#endif
    default:
        return 0;
    }
}
static int de(serial_port *s, int tx) {
    if (s->gpio_fd < 0)
        return 0;
    char c = tx ? '1' : '0';
    if (lseek(s->gpio_fd, 0, SEEK_SET) < 0 || write(s->gpio_fd, &c, 1) != 1)
        return -1;
    return 0;
}
int serial_configure(serial_port *s, port_config c) {
    speed_t b = speed(c.baud);
    struct termios t, old;
    if (!b || c.bits < 5 || c.bits > 8 || c.parity > 2 || (c.stops != 1 && c.stops != 2)) {
        errno = EINVAL;
        return -1;
    }
    if (tcgetattr(s->fd, &old) < 0)
        return -1;
    t = old;
    cfmakeraw(&t);
    t.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    t.c_cflag |= CREAD | CLOCAL;
    const tcflag_t bits[] = {CS5, CS6, CS7, CS8};
    t.c_cflag |= bits[c.bits - 5];
    if (c.parity)
        t.c_cflag |= PARENB;
    if (c.parity == 1)
        t.c_cflag |= PARODD;
    if (c.stops == 2)
        t.c_cflag |= CSTOPB;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (cfsetispeed(&t, b) < 0 || cfsetospeed(&t, b) < 0)
        return -1;
    if (tcsetattr(s->fd, TCSANOW, &t) < 0) {
        int e = errno;
        (void)tcsetattr(s->fd, TCSANOW, &old);
        errno = e;
        return -1;
    }
    s->config = c;
    return 0;
}
static int syswrite(const char *p, const char *v) {
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, v, strlen(v));
    int e = errno;
    close(fd);
    errno = e;
    return n == (ssize_t)strlen(v) ? 0 : -1;
}
static int gpio_open(int gpio) {
    char dir[96], p[128], v[32];
    snprintf(dir, sizeof(dir), "/sys/class/gpio/gpio%d", gpio);
    if (access(dir, F_OK)) {
        snprintf(v, sizeof(v), "%d", gpio);
        if (syswrite("/sys/class/gpio/export", v) && errno != EBUSY)
            return -1;
        for (int i = 0; i < 50 && access(dir, F_OK); i++)
            usleep(10000);
    }
    snprintf(p, sizeof(p), "%s/direction", dir);
    if (syswrite(p, "low"))
        return -1;
    snprintf(p, sizeof(p), "%s/value", dir);
    return open(p, O_WRONLY | O_CLOEXEC);
}
static int restore_priority(serial_port *s) {
    if(!s->tx_priority_changed)return 0;
    int e=pthread_setschedparam(pthread_self(),s->tx_policy,&s->tx_priority);
    if(!e)s->tx_priority_changed=0;
    return e;
}
static int anyka_drain(serial_port *s) {
#ifdef __linux__
    int64_t end=now_ms()+5000;
    while(now_ms()<end) {
        pthread_testcancel();
        int queued=0,lsr=0;
        if(ioctl(s->fd,TIOCOUTQ,&queued) || ioctl(s->fd,TIOCSERGETLSR,&lsr)) {
            if(errno==EINTR)continue;
            return -1;
        }
        if(!queued && (lsr&TIOCSER_TEMT)) {
            int64_t guard=now_ms()+2;
            while(now_ms()<guard)pthread_testcancel();
            return 0;
        }
        if(queued>256)usleep(1000);
    }
    errno=ETIMEDOUT;
#else
    (void)s;errno=ENOTSUP;
#endif
    return -1;
}
static void cleanup_tx(void *v) {
    serial_port *s = v;
    (void)tcflush(s->fd, TCOFLUSH);
    (void)de(s, 0);
    (void)restore_priority(s);
}
static void *worker(void *v) {
    serial_port *s = v;
    /* Cancellation is only enabled during I/O, never while holding the mutex. */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    for (;;) {
        pthread_mutex_lock(&s->lock);
        while (!s->busy && !s->stopping)
            pthread_cond_wait(&s->wake, &s->lock);
        if (s->stopping) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        size_t len = s->len;
        pthread_mutex_unlock(&s->lock);
        int err = 0;
        pthread_cleanup_push(cleanup_tx, s);
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        if(s->backend==3) {
            err=pthread_getschedparam(pthread_self(),&s->tx_policy,&s->tx_priority);
            struct sched_param priority={.sched_priority=1};
            if(!err)err=pthread_setschedparam(pthread_self(),SCHED_FIFO,&priority);
            if(!err)s->tx_priority_changed=1;
        }
        if (!err && de(s, 1))
            err = errno;
        else if (s->gpio_fd >= 0)
            usleep(100);
        size_t off = 0;
        unsigned char chunk[HD_MAX];
        int64_t deadline = now_ms() + 2000 + (int64_t)len * 12000 / s->config.baud;
        tx_trace(s->trace, "worker_start", s->ticket, len);
        while (!err && off < len) {
            /* Cancellation stays disabled while taking/awaiting producer credit. */
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
            pthread_mutex_lock(&s->lock);
            int64_t idle_deadline = now_ms() + 2000;
            while (!s->queued && !s->abort_error && !s->stopping) {
                struct timespec until;
#ifdef __linux__
                clock_gettime(CLOCK_MONOTONIC, &until);
#else
                clock_gettime(CLOCK_REALTIME, &until);
#endif
                until.tv_nsec += 100000000;
                if (until.tv_nsec >= 1000000000) {++until.tv_sec;until.tv_nsec-=1000000000;}
                pthread_cond_timedwait(&s->wake, &s->lock, &until);
                if (now_ms() >= idle_deadline) s->abort_error = ETIMEDOUT;
            }
            if (s->abort_error || s->stopping) err = s->abort_error ? s->abort_error : ECANCELED;
            size_t count = s->queued < sizeof(chunk) ? s->queued : sizeof(chunk);
            size_t first = HD_TX_RING - s->head;
            if (first > count) first = count;
            memcpy(chunk, s->data + s->head, first);
            memcpy(chunk + first, s->data, count - first);
            s->head = (s->head + count) % HD_TX_RING;
            s->queued -= count;
            pthread_mutex_unlock(&s->lock);
            (void)write(s->notify[1], "c", 1);
            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
            size_t pos = 0;
            while (!err && pos < count) {
                pthread_mutex_lock(&s->lock);
                err = s->abort_error;
                pthread_mutex_unlock(&s->lock);
                if (err) break;
                ssize_t n = write(s->fd, chunk + pos, count - pos);
                if (n > 0) {pos += (size_t)n;continue;}
                if (n < 0 && errno == EINTR) continue;
                if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                    struct pollfd p = {s->fd, POLLOUT, 0};
                    poll(&p, 1, 10);
                    if (now_ms() > deadline) err = ETIMEDOUT;
                    continue;
                }
                err = errno;
            }
            off += pos;
            tx_trace(s->trace, "write", s->ticket, off);
        }
        tx_trace(s->trace, "drain_begin", s->ticket, off);
        if (!err) {
            int r;
            do {
                r = s->backend==3 ? anyka_drain(s) : tcdrain(s->fd);
            } while (r < 0 && errno == EINTR);
            if (r < 0)
                err = errno;
        }
        tx_trace(s->trace, "drain_end", s->ticket, off);
        if (!err && s->gpio_fd >= 0 && s->backend!=3)
            usleep(5000);
        if (err)
            (void)tcflush(s->fd, TCOFLUSH);
        if (de(s, 0) && !err)
            err = errno;
        int priority_error=restore_priority(s);
        if(!err)err=priority_error;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        pthread_cleanup_pop(0);
        pthread_mutex_lock(&s->lock);
        s->result = err;
        s->ready = 1;
        tx_trace(s->trace, "ready", s->ticket, len);
        (void)write(s->notify[1], "d", 1);
        /* Main thread acknowledges completion before another job can start. */
        while (s->ready && !s->stopping)
            pthread_cond_wait(&s->wake, &s->lock);
        int stop = s->stopping;
        pthread_mutex_unlock(&s->lock);
        if (stop)
            break;
    }
    return NULL;
}
static int lock_device(serial_port *s, const char *directory) {
    struct stat st;
    if (fstat(s->fd, &st) < 0)
        return -1;
    if (!S_ISCHR(st.st_mode)) {
        errno = ENOTTY;
        return -1;
    }
    int n = snprintf(s->lock_path, sizeof(s->lock_path), "%s/hd485-uart-%jx.lock", directory,
                     (uintmax_t)st.st_rdev);
    if (n < 0 || (size_t)n >= sizeof(s->lock_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    s->lock_fd = open(s->lock_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s->lock_fd < 0) {
        if (errno == EEXIST) {
            fprintf(stderr, "UART lock exists: %s; check its PID before removing a stale lock\n",
                    s->lock_path);
            errno = EBUSY;
        }
        return -1;
    }
    char pid[32];
    n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
    if (write(s->lock_fd, pid, (size_t)n) != n) {
        errno = EIO;
        return -1;
    }
    int result;
#ifdef HD485_TEST_IO
    if (getenv("HD485_TEST_NO_FLOCK")) {
        errno = ENOSYS;
        result = -1;
    } else
#endif
        result = flock(s->fd, LOCK_EX | LOCK_NB);
    if (result < 0) {
        if (errno != ENOSYS && errno != EOPNOTSUPP)
            return -1;
        fprintf(stderr, "flock unavailable; using atomic UART lock %s plus TIOCEXCL\n",
                s->lock_path);
    }
    return 0;
}
int serial_open(serial_port *s, const char *path, const char *backend, int gpio, port_config c,
                const char *lock_dir) {
    memset(s, 0, sizeof(*s));
    s->fd = s->gpio_fd = s->lock_fd = -1;
    s->notify[0] = s->notify[1] = -1;
    s->trace = getenv("HD485_TX_TRACE") != NULL;
    pthread_mutex_init(&s->lock, NULL);
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
#ifdef __linux__
    /* Board NTP/time corrections must not extend a missing-data timeout. */
    int clock_error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (clock_error) {
        pthread_condattr_destroy(&attributes);
        pthread_mutex_destroy(&s->lock);
        errno = clock_error;
        return -1;
    }
#endif
    int condition_error = pthread_cond_init(&s->wake, &attributes);
    pthread_condattr_destroy(&attributes);
    if (condition_error) {
        pthread_mutex_destroy(&s->lock);
        errno = condition_error;
        return -1;
    }
    if (pipe(s->notify)) goto fail;
    for (int i=0;i<2;i++) {
        if (nonblock(s->notify[i]) || fcntl(s->notify[i], F_SETFD, FD_CLOEXEC)<0) goto fail;
    }
    s->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0)
        goto fail;
    if (lock_device(s, lock_dir) < 0 || ioctl(s->fd, TIOCEXCL) < 0)
        goto fail;
    s->exclusive = 1;
    if (serial_configure(s, c))
        goto fail;
    if (!strcmp(backend, "gpio") || !strcmp(backend,"gpio-anyka")) {
        s->gpio_fd = gpio_open(gpio);
        if (s->gpio_fd < 0)
            goto fail;
        s->backend = !strcmp(backend,"gpio-anyka") ? 3 : 1;
    } else if (!strcmp(backend, "kernel")) {
#ifdef __linux__
        struct serial_rs485 rs = {0};
        rs.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
        rs.delay_rts_after_send = 5;
        if (ioctl(s->fd, TIOCSRS485, &rs) < 0)
            goto fail;
        s->backend = 2;
#else
        errno = ENOTSUP;
        goto fail;
#endif
    } else if (strcmp(backend, "none")) {
        errno = EINVAL;
        goto fail;
    }
    int e = pthread_create(&s->thread, NULL, worker, s);
    if (e) {
        errno = e;
        goto fail;
    }
    s->started = 1;
    return 0;
fail: {
    int e = errno;
    serial_close(s);
    errno = e;
    return -1;
}
}
int serial_submit(serial_port *s, const void *p, size_t n) {
    pthread_mutex_lock(&s->lock);
    if (s->busy || n > HD_MAX) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    memcpy(s->data, p, n);
    s->len = s->accepted = s->queued = n;
    s->head = 0; s->stream = 0; s->abort_error = 0;
    s->busy = 1;
    s->start = now_ms();
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    return 0;
}
int serial_result(serial_port *s, int *r) {
    pthread_mutex_lock(&s->lock);
    int ready = s->ready;
    if (ready) {
        *r = s->result;
        s->ready = 0;
        s->busy = 0;
        pthread_cond_signal(&s->wake);
    }
    pthread_mutex_unlock(&s->lock);
    return ready;
}
int serial_stream_begin(serial_port *s, size_t total, uint32_t ticket) {
    pthread_mutex_lock(&s->lock);
    if (s->busy) {pthread_mutex_unlock(&s->lock);return EBUSY;}
    s->head = s->queued = s->accepted = 0;
    s->len = total; s->stream = 1; s->abort_error = 0; s->ticket = ticket;
    s->busy = 1; s->start = now_ms();
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    return 0;
}
int serial_stream_data(serial_port *s, const void *p, size_t n) {
    pthread_mutex_lock(&s->lock);
    int e = 0;
    if (!s->busy || !s->stream || s->ready) e = EPIPE;
    else if (s->abort_error) e = s->abort_error;
    else if (!n || n > s->len - s->accepted) e = EMSGSIZE;
    else if (n > HD_TX_RING - s->queued) e = EAGAIN;
    else {
        size_t tail = (s->head + s->queued) % HD_TX_RING;
        size_t first = HD_TX_RING - tail;
        if (first > n) first = n;
        memcpy(s->data + tail,p,first);memcpy(s->data,(const char *)p+first,n-first);
        s->queued += n;s->accepted += n;pthread_cond_signal(&s->wake);
    }
    pthread_mutex_unlock(&s->lock);
    return e;
}
void serial_abort(serial_port *s, int error) {
    pthread_mutex_lock(&s->lock);
    if (s->busy && s->stream) {s->abort_error=error;pthread_cond_signal(&s->wake);}
    pthread_mutex_unlock(&s->lock);
}
void serial_notifications(serial_port *s) {
    char b[128];while (read(s->notify[0],b,sizeof(b))>0) {}
}
void serial_close(serial_port *s) {
    if (s->started) {
        pthread_mutex_lock(&s->lock);
        s->stopping = 1;
        pthread_cond_signal(&s->wake);
        pthread_mutex_unlock(&s->lock);
        pthread_cancel(s->thread);
        pthread_join(s->thread, NULL);
        s->started = 0;
    }
    if (s->gpio_fd >= 0) {
        (void)de(s, 0);
        close(s->gpio_fd);
        s->gpio_fd = -1;
    }
    if (s->fd >= 0) {
        if (s->exclusive)
            ioctl(s->fd, TIOCNXCL);
        close(s->fd);
        s->fd = -1;
    }
    if (s->lock_fd >= 0) {
        struct stat owned, current;
        if (fstat(s->lock_fd, &owned) == 0 && lstat(s->lock_path, &current) == 0 &&
            owned.st_dev == current.st_dev && owned.st_ino == current.st_ino)
            unlink(s->lock_path);
        close(s->lock_fd);
        s->lock_fd = -1;
    }
    for (int i=0;i<2;i++) if(s->notify[i]>=0) {close(s->notify[i]);s->notify[i]=-1;}
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->wake);
}
