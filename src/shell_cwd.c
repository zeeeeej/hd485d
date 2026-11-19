#include "maintenance.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void discard_report(maintenance *m) {
    if (m->cwd_pipe >= 0)
        close(m->cwd_pipe);
    m->cwd_pipe = -1;
    m->cwd_report_len = 0;
}
void maint_cwd_reset(maintenance *m) {
    discard_report(m);
    if (m->cwd_fd >= 0)
        close(m->cwd_fd);
    m->cwd_fd = fcntl(m->cwd_base, F_DUPFD_CLOEXEC, 3);
    m->cwd_logical[0] = m->cwd_old[0] = 0;
}
int maint_cwd_init(maintenance *m) {
    m->cwd_base = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (m->cwd_base < 0)
        return -1;
    maint_cwd_reset(m);
    if (m->cwd_fd < 0) {
        int e = errno;
        close(m->cwd_base);
        m->cwd_base = -1;
        errno = e;
        return -1;
    }
    return 0;
}
int maint_cwd_enter(maintenance *m) {
    if (fchdir(m->cwd_fd))
        return -1;
    /* Shell validates PWD against its actual directory. Keep logical symlink semantics. */
    if (m->cwd_logical[0]) {
        if (setenv("PWD", m->cwd_logical, 1)) return -1;
    } else if (unsetenv("PWD")) return -1;
    if (m->cwd_old[0]) {
        if (setenv("OLDPWD", m->cwd_old, 1)) return -1;
    } else if (unsetenv("OLDPWD")) return -1;
    return 0;
}
static void apply_report(maintenance *m) {
    unsigned char *parts[3];
    size_t lengths[3], offset = 0;
    for (int i = 0; i < 3; i++) {
        parts[i] = m->cwd_report + offset;
        unsigned char *end = memchr(parts[i], 0, m->cwd_report_len - offset);
        if (!end) return;
        lengths[i] = (size_t)(end - parts[i]);
        if (lengths[i] >= 4096 + (i == 0 ? 1u : 0u)) return;
        offset += lengths[i] + 1;
    }
    if (offset != m->cwd_report_len || lengths[0] < 2 ||
        parts[0][0] != '/' || parts[0][lengths[0] - 1] != '\n') return;
    /* pwd emits exactly one extra LF; directory names themselves may contain LF. */
    parts[0][lengths[0] - 1] = 0;
    int fd = open((char *)parts[0], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat actual, logical;
    if (fstat(fd, &actual)) { close(fd); return; }
    const char *path = (char *)parts[0];
    if (parts[1][0] == '/' && stat((char *)parts[1], &logical) == 0 &&
        actual.st_dev == logical.st_dev && actual.st_ino == logical.st_ino)
        path = (char *)parts[1];
    if (m->cwd_fd >= 0) close(m->cwd_fd);
    m->cwd_fd = fd;
    strcpy(m->cwd_logical, path);
    memcpy(m->cwd_old, parts[2], lengths[2] + 1);
}
void maint_cwd_tick(maintenance *m) {
    if (m->cwd_pipe >= 0) {
        for (int i = 0; i < 4; i++) {
            size_t left = sizeof(m->cwd_report) - m->cwd_report_len;
            if (!left) { discard_report(m); break; }
            ssize_t n = read(m->cwd_pipe, m->cwd_report + m->cwd_report_len, left);
            if (n > 0) { m->cwd_report_len += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            close(m->cwd_pipe);
            m->cwd_pipe = -1;
            if (n < 0) m->cwd_report_len = 0;
            break;
        }
    }
    if (m->pid == 0 && m->cwd_pipe < 0 && m->cwd_report_len) {
        if (m->cmd_state == 2) apply_report(m);
        m->cwd_report_len = 0;
    }
}
void maint_cwd_close(maintenance *m) {
    discard_report(m);
    if (m->cwd_fd >= 0) close(m->cwd_fd);
    if (m->cwd_base >= 0) close(m->cwd_base);
    m->cwd_fd = m->cwd_base = -1;
}
