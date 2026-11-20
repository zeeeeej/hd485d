#include "maintenance.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int maint_same_stat(const struct stat *a, const struct stat *b) {
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_mode != b->st_mode ||
        a->st_size != b->st_size) return 0;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
        a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
        a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
        a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}
int maint_fs_init(maintenance *m, const char *root) {
    if (!root || root[0] != '/' || strlen(root) >= sizeof(m->fsroot_path)) {
        errno = EINVAL; return -1;
    }
    m->fsroot = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (m->fsroot < 0) return -1;
    strcpy(m->fsroot_path, root);
    return 0;
}
void maint_fs_close(maintenance *m) {
    if (m->listing) closedir(m->listing);
    m->listing = NULL;
    m->listing_active = m->listing_eof = 0;
    m->record_len = m->record_pos = 0;
    m->listing_offset = 0;
}
/* One bounded record, fragmented by FS_READ. No unbounded directory snapshot. */
static int record(maintenance *m, int dir, const char *name) {
    struct stat st;
    if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW)) return errno;
    size_t names = strlen(name), links = 0;
    unsigned char *b = m->record;
    if (S_ISLNK(st.st_mode)) {
        ssize_t n = readlinkat(dir, name, (char *)b + 28 + names, sizeof(m->record) - 28 - names);
        if (n < 0) return errno;
        if ((size_t)n == sizeof(m->record) - 28 - names) return ENAMETOOLONG;
        links = (size_t)n;
    }
    wr32(b, (uint32_t)(24 + names + links));
    wr32(b + 4, (uint32_t)st.st_mode);
    wr64(b + 8, (uint64_t)st.st_size);
    wr64(b + 16, (uint64_t)st.st_mtime);
    wr16(b + 24, (uint16_t)names);
    wr16(b + 26, (uint16_t)links);
    memcpy(b + 28, name, names);
    m->record_len = 28 + names + links;
    m->record_pos = 0;
    return 0;
}
static int open_dir(maintenance *m, const unsigned char *p, size_t n) {
    if (n == 1 && p[0] == '/') return openat(m->fsroot, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char name[256];
    int parent = maint_parent(m->fsroot, p, n, 1, name);
    if (parent < 0) return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int e = errno; close(parent); errno = e;
    return fd;
}
int maint_fs_request(maintenance *m, unsigned op, const unsigned char *p, size_t n,
                     unsigned char *out, size_t *len) {
    if (op == M_FS_INFO) {
        if (n) return EINVAL;
        size_t rootlen = strlen(m->fsroot_path);
        wr16(out, 1); /* extension revision */
        wr16(out + 2, (m->fsroot >= 0 ? 1 : 0) | (m->allow_shell ? 6 : 0) | (m->allow_pty ? 8 : 0));
        wr64(out + 4, 64u * 1024u * 1024u);
        wr16(out + 12, 240); wr16(out + 14, (uint16_t)rootlen);
        memcpy(out + 16, m->fsroot_path, rootlen);
        *len = 16 + rootlen; return 0;
    }
    if (op < M_FS_LIST || op > M_FS_STAT) return ENOTSUP;
    if (m->fsroot < 0) return ENOTSUP;
    if (op == M_FS_CLOSE) {
        if (n) return EINVAL;
        maint_fs_close(m); return 0;
    }
    if (op == M_FS_LIST || op == M_FS_STAT) {
        maint_fs_close(m);
        if (op == M_FS_LIST) {
            int fd = open_dir(m, p, n);
            if (fd < 0) return errno;
            m->listing = fdopendir(fd);
            if (!m->listing) { int e = errno; close(fd); return e; }
            if (fstat(fd, &m->listing_snapshot)) { int e = errno; maint_fs_close(m); return e; }
        } else {
            char name[256];
            int fd;
            if (n == 1 && p[0] == '/') { fd = dup(m->fsroot); strcpy(name, "."); }
            else fd = maint_parent(m->fsroot, p, n, 1, name);
            if (fd < 0) return errno;
            int e = record(m, fd, name); close(fd);
            if (e) return e;
            m->listing_eof = 1;
        }
        m->listing_active = 1; return 0;
    }
    if (n != 8 || !m->listing_active) return EINVAL;
    if (rd64(p) != m->listing_offset) return ESTALE;
    if (m->listing) {
        struct stat st;
        if (fstat(dirfd(m->listing), &st)) return errno;
        if (!maint_same_stat(&st, &m->listing_snapshot)) return ESTALE;
    }
    if (m->record_pos == m->record_len && !m->listing_eof) {
        struct dirent *ent;
        /* Bound skipping internal entries so business acquisition remains responsive. */
        int scanned = 0;
        for (;;) {
            errno = 0; ent = readdir(m->listing);
            if (!ent) { if (errno) return errno; m->listing_eof = 1; break; }
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..") ||
                !strncmp(ent->d_name, ".hd485-", 7)) {
                if (++scanned >= 64) return EAGAIN;
                continue;
            }
            int e = record(m, dirfd(m->listing), ent->d_name);
            if (e) return e;
            break;
        }
    }
    size_t amount = m->record_len - m->record_pos;
    if (amount > 240) amount = 240;
    out[0] = m->listing_eof && m->record_pos + amount == m->record_len;
    wr64(out + 1, m->listing_offset);
    memcpy(out + 9, m->record + m->record_pos, amount);
    m->record_pos += amount; m->listing_offset += amount;
    *len = 9 + amount; return 0;
}
