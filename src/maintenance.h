#ifndef HD_MAINT_H
#define HD_MAINT_H
#include "sha256.h"
#include "wire.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#define OUTPUT_LIMIT (128 * 1024)
/* Handlers run on the service loop: bounded, nonblocking work only. */
typedef int (*maint_handler)(const char *args, unsigned char *out, size_t capacity, size_t *length,
                             void *context);
typedef struct {
    char name[32];
    maint_handler run;
    void *context;
} maint_command;
typedef struct {
    struct pty_session *pty;
    int allow_pty, pty_timeout_ms;
    uint32_t pty_sequence;
    int root, allow_shell, timeout_ms;
    pid_t pid, pgid;
    int pipes[2], cmd_state, exitcode, truncated;
    int64_t command_start, kill_at;
    unsigned char output[2][OUTPUT_LIMIT];
    size_t output_len[2];
    int cwd_base, cwd_fd, cwd_pipe;
    char cwd_logical[4096], cwd_old[4096];
    unsigned char cwd_report[3 * 4096 + 4];
    size_t cwd_report_len;
    int file, dir, download, overwrite;
    char name[256], part[96], meta[96], getpath[4096];
    int fsroot, extended, target_exists;
    char fsroot_path[237];
    struct stat target_snapshot;
    DIR *listing;
    struct stat listing_snapshot;
    unsigned char record[8192];
    size_t record_len, record_pos;
    uint64_t listing_offset;
    int listing_active, listing_eof;
    uint64_t size, offset;
    unsigned char hash[32];
    struct stat snapshot;
    int hashing, hash_error, hash_done;
    uint64_t hash_offset;
    sha256_ctx hash_ctx;
    maint_command commands[16];
    unsigned command_count;
} maintenance;
int maint_init(maintenance *m, const char *root, int allow_shell, int timeout_ms);
int maint_pty_busy(maintenance *m);
void maint_pty_tick(maintenance *m);
void maint_pty_stop(maintenance *m, int reason);
void maint_pty_destroy(maintenance *m);
int maint_pty_request(maintenance *m, unsigned op, const unsigned char *data, size_t n,
                      unsigned char *out, size_t *len);
int maint_cwd_init(maintenance *m);
int maint_cwd_enter(maintenance *m);
void maint_cwd_reset(maintenance *m);
void maint_cwd_tick(maintenance *m);
void maint_cwd_close(maintenance *m);
int maint_fs_init(maintenance *m, const char *root);
void maint_fs_close(maintenance *m);
int maint_fs_request(maintenance *m, unsigned op, const unsigned char *p, size_t n,
                     unsigned char *out, size_t *len);
int maint_parent(int root, const unsigned char *p, size_t n, int absolute, char name[256]);
int maint_same_stat(const struct stat *a, const struct stat *b);
void maint_tick(maintenance *m);
void maint_cancel(maintenance *m, int reason);
void maint_new_session(maintenance *m);
int maint_register_command(maintenance *m, const char *name, maint_handler run, void *context);
void maint_close(maintenance *m);
/* Returns errno, response excludes the 4-byte status prefix. */
int maint_request(maintenance *m, unsigned op, const unsigned char *p, size_t n, unsigned char *out,
                  size_t *len);
#endif
