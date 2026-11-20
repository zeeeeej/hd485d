#include "maintenance.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define MAX_FILE (64u * 1024u * 1024u)
static void file_close(maintenance *m) {
    if (m->file >= 0)
        close(m->file);
    if (m->dir >= 0)
        close(m->dir);
    m->file = m->dir = -1;
    m->hashing = m->hash_done = m->hash_error = 0;
    m->getpath[0] = 0;
}
int maint_register_command(maintenance *m, const char *name, maint_handler run, void *context) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= 32 || !run || m->command_count >= 16) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') {
            errno = EINVAL;
            return -1;
        }
    for (unsigned i = 0; i < m->command_count; i++)
        if (!strcmp(m->commands[i].name, name)) {
            errno = EEXIST;
            return -1;
        }
    maint_command *c = &m->commands[m->command_count++];
    strcpy(c->name, name);
    c->run = run;
    c->context = context;
    return 0;
}
int maint_init(maintenance *m, const char *root, int allow, int timeout) {
    memset(m, 0, sizeof(*m));
    m->fsroot = m->root = m->file = m->dir = m->pipes[0] = m->pipes[1] = -1;
    m->cwd_base = m->cwd_fd = m->cwd_pipe = -1;
    m->exitcode = -1;
    m->allow_shell = allow;
    m->timeout_ms = timeout;
    if (mkdir(root, 0700) < 0 && errno != EEXIST)
        return -1;
    m->root = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (m->root < 0) return -1;
    if (maint_cwd_init(m)) {
        int e = errno;
        close(m->root); m->root = -1;
        errno = e;
        return -1;
    }
    return 0;
}
void maint_cancel(maintenance *m, int reason) {
    maint_pty_stop(m, reason);
    if (m->pid > 0 && m->cmd_state == 1) {
        m->cmd_state = reason;
        m->kill_at = now_ms() + 500;
        kill(-m->pgid, SIGTERM);
    }
}
void maint_new_session(maintenance *m) {
    maint_cancel(m, 3);
    maint_cwd_reset(m);
    maint_fs_close(m);
    file_close(m);
}
static int unchanged(maintenance *m) {
    struct stat s;
    if (fstat(m->file, &s) < 0)
        return 0;
#ifdef __APPLE__
    return s.st_size == m->snapshot.st_size &&
           s.st_mtimespec.tv_sec == m->snapshot.st_mtimespec.tv_sec &&
           s.st_mtimespec.tv_nsec == m->snapshot.st_mtimespec.tv_nsec &&
           s.st_ctimespec.tv_sec == m->snapshot.st_ctimespec.tv_sec &&
           s.st_ctimespec.tv_nsec == m->snapshot.st_ctimespec.tv_nsec;
#else
    return s.st_size == m->snapshot.st_size && s.st_mtim.tv_sec == m->snapshot.st_mtim.tv_sec &&
           s.st_mtim.tv_nsec == m->snapshot.st_mtim.tv_nsec &&
           s.st_ctim.tv_sec == m->snapshot.st_ctim.tv_sec &&
           s.st_ctim.tv_nsec == m->snapshot.st_ctim.tv_nsec;
#endif
}
void maint_tick(maintenance *m) {
    maint_pty_tick(m);
    for (int i = 0; i < 2; i++)
        if (m->pipes[i] >= 0) {
            unsigned char b[4096];
            for (int count = 0; count < 8; count++) {
                ssize_t n = read(m->pipes[i], b, sizeof(b));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0)
                    break;
                if (!n) {
                    close(m->pipes[i]);
                    m->pipes[i] = -1;
                    break;
                }
                size_t keep = (size_t)n;
                if (keep > OUTPUT_LIMIT - m->output_len[i]) {
                    keep = OUTPUT_LIMIT - m->output_len[i];
                    m->truncated = 1;
                }
                memcpy(m->output[i] + m->output_len[i], b, keep);
                m->output_len[i] += keep;
            }
        }
    if (m->pid > 0) {
        if (m->cmd_state == 1 && now_ms() - m->command_start >= m->timeout_ms)
            maint_cancel(m, 4);
        if (m->kill_at && now_ms() >= m->kill_at) {
            kill(-m->pgid, SIGKILL);
            m->kill_at = 0;
        }
        int status;
        pid_t r = waitpid(m->pid, &status, WNOHANG);
        if (r == m->pid) {
            m->exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            if (m->cmd_state == 1)
                m->cmd_state = 2;
            kill(-m->pgid, SIGKILL);
            m->pid = 0;
            m->kill_at = 0;
        }
    }
    maint_cwd_tick(m);
    if (m->hashing) {
        unsigned char b[16384];
        size_t want = sizeof(b);
        if (m->size - m->hash_offset < want)
            want = (size_t)(m->size - m->hash_offset);
        ssize_t n = want ? pread(m->file, b, want, (off_t)m->hash_offset) : 0;
        if (n < 0) {
            if (errno == EINTR)
                return;
            m->hash_error = errno;
            m->hashing = 0;
        } else if (n > 0) {
            sha256_update(&m->hash_ctx, b, (size_t)n);
            m->hash_offset += (uint64_t)n;
        } else if (m->hash_offset != m->size) {
            m->hash_error = EIO;
            m->hashing = 0;
        } else {
            unsigned char h[32];
            sha256_final(&m->hash_ctx, h);
            m->hashing = 0;
            if (m->download) {
                if (!unchanged(m))
                    m->hash_error = ESTALE;
                else
                    memcpy(m->hash, h, 32);
            } else if (memcmp(h, m->hash, 32))
                m->hash_error = EBADMSG;
            m->hash_done = 1;
        }
    }
}
static int start_command(maintenance *m, const unsigned char *p, size_t n) {
    if (!n || n > 4096 || memchr(p, 0, n))
        return EINVAL;
    if (m->pid > 0 || m->cwd_pipe >= 0)
        return EBUSY;
    if (p[0] == '@') {
        char text[4097];
        memcpy(text, p + 1, n - 1);
        text[n - 1] = 0;
        char *args = strchr(text, ' ');
        if (args)
            *args++ = 0;
        else
            args = text + strlen(text);
        for (unsigned i = 0; i < m->command_count; i++)
            if (!strcmp(text, m->commands[i].name)) {
                for (int stream = 0; stream < 2; stream++) {
                    if (m->pipes[stream] >= 0)
                        close(m->pipes[stream]);
                    m->pipes[stream] = -1;
                    m->output_len[stream] = 0;
                }
                m->truncated = 0;
                m->exitcode = m->commands[i].run(args, m->output[0], OUTPUT_LIMIT,
                                                 &m->output_len[0], m->commands[i].context);
                if (m->output_len[0] > OUTPUT_LIMIT) {
                    m->output_len[0] = OUTPUT_LIMIT;
                    m->truncated = 1;
                }
                m->cmd_state = 2;
                return 0;
            }
        return ENOENT;
    }
    if (!m->allow_shell)
        return EACCES;
    if (m->cwd_fd < 0) return EBADF;
    int f[3][2];
    for (int i = 0; i < 3; i++) {
        if (pipe(f[i])) {
            int e = errno;
            for (int j = 0; j < i; j++) {close(f[j][0]); close(f[j][1]);}
            return e;
        }
        fcntl(f[i][0], F_SETFD, FD_CLOEXEC);
        fcntl(f[i][1], F_SETFD, FD_CLOEXEC);
    }
    char command[4097];
    memcpy(command, p, n);
    command[n] = 0;
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        for (int i = 0; i < 3; i++) {
            close(f[i][0]);
            close(f[i][1]);
        }
        return e;
    }
    if (!pid) {
        setpgid(0, 0);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, 0);
            close(nullfd);
        }
        dup2(f[0][1], 1);
        dup2(f[1][1], 2);
        for (int i = 0; i < 2; i++) {
            close(f[i][0]);
            close(f[i][1]);
        }
        if (maint_cwd_enter(m)) {perror("working directory"); _exit(126);}
        close(f[2][0]);
        if (f[2][1] != 9) {
            if (dup2(f[2][1], 9) < 0) _exit(126);
            close(f[2][1]);
        } else if (fcntl(9, F_SETFD, 0)) _exit(126);
        /* CWD metadata has its own FD; stdout/stderr and command exit status stay untouched. */
        const char *wrapper =
            "if [ -n \"$2\" ]; then OLDPWD=$2; export OLDPWD; else unset OLDPWD; fi; "
            "__hd485_command=$1; shift 2; "
            "trap '(command pwd -P) >&9; printf \"\\000%s\\000%s\\000\" \"$PWD\" \"${OLDPWD-}\" >&9' 0; "
            "eval \"$__hd485_command\"";
        execl("/bin/sh", "sh", "-c", wrapper, "hd485-shell", command, m->cwd_old, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    for (int i = 0; i < 2; i++) {
        close(f[i][1]);
        if (m->pipes[i] >= 0)
            close(m->pipes[i]);
        m->pipes[i] = f[i][0];
        nonblock(m->pipes[i]);
        m->output_len[i] = 0;
    }
    close(f[2][1]);
    m->cwd_pipe = f[2][0];
    nonblock(m->cwd_pipe);
    m->cwd_report_len = 0;
    m->pid = m->pgid = pid;
    m->cmd_state = 1;
    m->exitcode = -1;
    m->truncated = 0;
    m->kill_at = 0;
    m->command_start = now_ms();
    return 0;
}
/* Resolve each directory component with O_NOFOLLOW, never concatenate a root path. */
int maint_parent(int root, const unsigned char *p, size_t n, int absolute, char name[256]) {
    if (absolute && n && p[0] == '/') {
        p++;
        n--;
    } else if (absolute) {
        errno = EINVAL;
        return -1;
    }
    if (!n || n >= (absolute ? 4095u : 512u) || p[0] == '/' || memchr(p, 0, n)) {
        errno = EINVAL;
        return -1;
    }
    char path[4096];
    memcpy(path, p, n);
    path[n] = 0;
    int fd = dup(root);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    char *start = path;
    for (;;) {
        char *slash = strchr(start, '/');
        if (slash)
            *slash = 0;
        size_t len = strlen(start);
        if (!len || len > 255 || !strcmp(start, ".") || !strcmp(start, "..") ||
            !strncmp(start, ".hd485-", 7)) {
            close(fd);
            errno = EINVAL;
            return -1;
        }
        if (!slash) {
            memcpy(name, start, len + 1);
            return fd;
        }
        int next = openat(fd, start, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int e = errno;
        close(fd);
        if (next < 0) {
            errno = e;
            return -1;
        }
        fd = next;
        start = slash + 1;
    }
}
static void start_hash(maintenance *m) {
    m->hashing = 1;
    m->hash_done = m->hash_error = 0;
    m->hash_offset = 0;
    sha256_init(&m->hash_ctx);
}
static int put_begin(maintenance *m, const unsigned char *p, size_t n, unsigned char *out,
                     size_t *len) {
    if (n < 42 || p[40] > 1 || rd64(p) > MAX_FILE)
        return EINVAL;
    file_close(m);
    m->dir = maint_parent(m->extended ? m->fsroot : m->root, p + 41, n - 41, m->extended, m->name);
    if (m->dir < 0)
        return errno;
    m->size = rd64(p);
    memcpy(m->hash, p + 8, 32);
    m->overwrite = p[40];
    m->download = 0;
    struct stat st;
    m->target_exists = 0;
    if (fstatat(m->dir, m->name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        m->target_exists = 1;
        m->target_snapshot = st;
        if (!S_ISREG(st.st_mode))
            return EINVAL;
        if (!m->overwrite)
            return EEXIST;
    } else if (errno != ENOENT)
        return errno;
    sha256_ctx h;
    unsigned char digest[32];
    sha256_init(&h);
    sha256_update(&h, m->name, strlen(m->name));
    sha256_final(&h, digest);
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    snprintf(m->part, sizeof(m->part), ".hd485-%s.part", hex);
    snprintf(m->meta, sizeof(m->meta), ".hd485-%s.meta", hex);
    int meta = openat(m->dir, m->meta, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (meta >= 0) {
        ssize_t w = write(meta, p, 40);
        int e = errno;
        if (w != 40 || fsync(meta)) {
            if (w == 40)
                e = errno;
            close(meta);
            return e ? e : EIO;
        }
        close(meta);
    } else if (errno == EEXIST) {
        meta = openat(m->dir, m->meta, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (meta < 0)
            return errno;
        unsigned char old[41];
        ssize_t r = read(meta, old, sizeof(old));
        close(meta);
        if (r != 40 || memcmp(old, p, 40))
            return EEXIST;
    } else
        return errno;
    m->file = openat(m->dir, m->part, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (m->file < 0)
        return errno;
    if (fstat(m->file, &st) < 0)
        return errno;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || (uint64_t)st.st_size > m->size)
        return EINVAL;
    m->offset = (uint64_t)st.st_size;
    wr64(out, m->offset);
    *len = 8;
    return 0;
}
int maint_request(maintenance *m, unsigned op, const unsigned char *p, size_t n, unsigned char *out,
                  size_t *len) {
    *len = 0;
    if(op>=M_PTY_OPEN && op<=M_PTY_STATUS) return maint_pty_request(m,op,p,n,out,len);
    if(maint_pty_busy(m) && op!=M_STATUS && op!=M_FS_INFO && op!=M_CANCEL) return EBUSY;
    switch (op) {
    case M_EXEC:
        return start_command(m, p, n);
    case M_CANCEL:
        if (n)
            return EINVAL;
        maint_cancel(m, 3);
        return 0;
    case M_POLL: {
        if (n != 5 || p[0] > 1)
            return EINVAL;
        unsigned stream = p[0];
        size_t off = rd32(p + 1);
        if (off > m->output_len[stream])
            return EINVAL;
        out[0] = (unsigned char)m->cmd_state;
        out[1] = (unsigned char)m->truncated;
        wr32(out + 2, (uint32_t)m->exitcode);
        wr32(out + 6, (uint32_t)m->output_len[stream]);
        out[10] = (m->pipes[0] >= 0 || m->pipes[1] >= 0 || m->cwd_pipe >= 0 || m->pid > 0) ? 0 : 1;
        size_t amount = m->output_len[stream] - off;
        if (amount > 237)
            amount = 237;
        memcpy(out + 11, m->output[stream] + off, amount);
        *len = 11 + amount;
        return 0;
    }
    case M_FS_PUT:
    case M_PUT_BEGIN: {
        file_close(m);
        m->extended = op == M_FS_PUT;
        if (m->extended && m->fsroot < 0)
            return ENOTSUP;
        int error = put_begin(m, p, n, out, len);
        if (error)
            file_close(m);
        return error;
    }
    case M_PUT_DATA: {
        if (n < 9 || n > 248 || m->file < 0 || m->download || m->hashing || m->hash_done)
            return EINVAL;
        uint64_t off = rd64(p);
        size_t amount = n - 8;
        if (off > m->offset || off > m->size || amount > m->size - off)
            return EINVAL;
        if (off < m->offset) {
            unsigned char b[240];
            if (off + amount > m->offset)
                return EINVAL;
            if (pread(m->file, b, amount, (off_t)off) != (ssize_t)amount ||
                memcmp(b, p + 8, amount))
                return EBADMSG;
        } else {
            size_t done = 0;
            while (done < amount) {
                ssize_t w = pwrite(m->file, p + 8 + done, amount - done, (off_t)(off + done));
                if (w < 0 && errno == EINTR)
                    continue;
                if (w <= 0)
                    return w < 0 ? errno : EIO;
                done += (size_t)w;
            }
            if (fsync(m->file))
                return errno;
            m->offset += amount;
        }
        wr64(out, m->offset);
        *len = 8;
        return 0;
    }
    case M_PUT_COMMIT: {
        if (n || m->file < 0 || m->download)
            return EINVAL;
        if (m->offset != m->size)
            return EINVAL;
        if (m->hash_error)
            return m->hash_error;
        if (!m->hash_done) {
            if (!m->hashing)
                start_hash(m);
            return EAGAIN;
        }
        if (fsync(m->file))
            return errno;
        if (m->extended) {
            struct stat current;
            int exists = fstatat(m->dir, m->name, &current, AT_SYMLINK_NOFOLLOW) == 0;
            if (!exists && errno != ENOENT) return errno;
            if (exists != m->target_exists ||
                (exists && !maint_same_stat(&current, &m->target_snapshot))) return ESTALE;
            if (fchmod(m->file, exists ? (current.st_mode & 0777) : 0644)) return errno;
        }
        if (m->overwrite) {
            struct stat st;
            if (fstatat(m->dir, m->name, &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(st.st_mode))
                return EINVAL;
            if (renameat(m->dir, m->part, m->dir, m->name))
                return errno;
        } else {
            if (linkat(m->dir, m->part, m->dir, m->name, 0))
                return errno;
            unlinkat(m->dir, m->part, 0);
        }
        unlinkat(m->dir, m->meta, 0);
        if (fsync(m->dir))
            return errno;
        file_close(m);
        return 0;
    }
    case M_FS_GET:
    case M_GET_BEGIN: {
        int extended = op == M_FS_GET;
        if (extended && m->fsroot < 0) return ENOTSUP;
        if (extended != m->extended) file_close(m);
        m->extended = extended;
        if (!n || n >= sizeof(m->getpath) || memchr(p, 0, n))
            return EINVAL;
        if (m->file < 0 || !m->download || strlen(m->getpath) != n || memcmp(m->getpath, p, n)) {
            file_close(m);
            m->dir = maint_parent(m->extended ? m->fsroot : m->root, p, n, m->extended, m->name);
            if (m->dir < 0)
                return errno;
            struct stat candidate;
            if (fstatat(m->dir, m->name, &candidate, AT_SYMLINK_NOFOLLOW))
                return errno;
            if (!S_ISREG(candidate.st_mode)) {
                file_close(m);
                return EINVAL;
            }
            m->file = openat(m->dir, m->name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
            if (m->file < 0)
                return errno;
            if (fstat(m->file, &m->snapshot))
                return errno;
            if (!S_ISREG(m->snapshot.st_mode) || m->snapshot.st_size < 0 ||
                (uint64_t)m->snapshot.st_size > MAX_FILE) {
                file_close(m);
                return EINVAL;
            }
            m->size = (uint64_t)m->snapshot.st_size;
            m->download = 1;
            memcpy(m->getpath, p, n);
            m->getpath[n] = 0;
            start_hash(m);
        }
        if (m->hash_error)
            return m->hash_error;
        if (!m->hash_done)
            return EAGAIN;
        if (!unchanged(m))
            return ESTALE;
        wr64(out, m->size);
        memcpy(out + 8, m->hash, 32);
        *len = 40;
        return 0;
    }
    case M_GET_DATA: {
        if (n != 8 || m->file < 0 || !m->download || !m->hash_done || m->hash_error)
            return EINVAL;
        if (!unchanged(m))
            return ESTALE;
        uint64_t off = rd64(p);
        if (off > m->size)
            return EINVAL;
        size_t amount = (size_t)(m->size - off);
        if (amount > 240)
            amount = 240;
        ssize_t r = pread(m->file, out + 8, amount, (off_t)off);
        if (r < 0)
            return errno;
        if ((size_t)r != amount)
            return EIO;
        wr64(out, off);
        *len = 8 + amount;
        return 0;
    }
    case M_STATUS:
        if (n)
            return EINVAL;
        wr32(out, (uint32_t)m->cmd_state);
        wr32(out + 4, (uint32_t)m->exitcode);
        wr64(out + 8, m->offset);
        *len = 16;
        return 0;
    case M_FILE_CLOSE:
        if (n) return EINVAL;
        file_close(m);
        return 0;
    default:
        return maint_fs_request(m, op, p, n, out, len);
    }
}
void maint_close(maintenance *m) {
    maint_pty_destroy(m);
    if (m->pid > 0) {
        kill(-m->pgid, SIGKILL);
        while (waitpid(m->pid, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    for (int i = 0; i < 2; i++)
        if (m->pipes[i] >= 0)
            close(m->pipes[i]);
    file_close(m);
    maint_cwd_close(m);
    maint_fs_close(m);
    if (m->fsroot >= 0) close(m->fsroot);
    if (m->root >= 0)
        close(m->root);
}
