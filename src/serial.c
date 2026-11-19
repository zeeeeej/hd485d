#include "serial.h"
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
static void cleanup_tx(void *v) {
    serial_port *s = v;
    (void)tcflush(s->fd, TCOFLUSH);
    (void)de(s, 0);
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
        if (de(s, 1))
            err = errno;
        else if (s->gpio_fd >= 0)
            usleep(100);
        size_t off = 0;
        int64_t deadline = now_ms() + 2000 + (int64_t)len * 12000 / s->config.baud;
        while (!err && off < len) {
            ssize_t n = write(s->fd, s->data + off, len - off);
            if (n > 0) {
                off += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                struct pollfd p = {s->fd, POLLOUT, 0};
                poll(&p, 1, 10);
                if (now_ms() > deadline)
                    err = ETIMEDOUT;
                continue;
            }
            err = errno;
        }
        if (!err) {
            int r;
            do {
                r = tcdrain(s->fd);
            } while (r < 0 && errno == EINTR);
            if (r < 0)
                err = errno;
        }
        if (!err && s->gpio_fd >= 0)
            usleep(5000);
        if (err)
            (void)tcflush(s->fd, TCOFLUSH);
        if (de(s, 0) && !err)
            err = errno;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        pthread_cleanup_pop(0);
        pthread_mutex_lock(&s->lock);
        s->result = err;
        s->ready = 1;
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
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    s->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0)
        return -1;
    if (lock_device(s, lock_dir) < 0 || ioctl(s->fd, TIOCEXCL) < 0)
        goto fail;
    s->exclusive = 1;
    if (serial_configure(s, c))
        goto fail;
    if (!strcmp(backend, "gpio")) {
        s->gpio_fd = gpio_open(gpio);
        if (s->gpio_fd < 0)
            goto fail;
        s->backend = 1;
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
    s->len = n;
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
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->wake);
}
