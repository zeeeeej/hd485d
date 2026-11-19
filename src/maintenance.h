#ifndef HD_MAINT_H
#define HD_MAINT_H
#include "sha256.h"
#include "wire.h"
#include <sys/stat.h>
#include <sys/types.h>
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
    int root, allow_shell, timeout_ms;
    pid_t pid, pgid;
    int pipes[2], cmd_state, exitcode, truncated;
    int64_t command_start, kill_at;
    unsigned char output[2][OUTPUT_LIMIT];
    size_t output_len[2];
    int file, dir, download, overwrite;
    char name[256], part[96], meta[96], getpath[512];
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
void maint_tick(maintenance *m);
void maint_cancel(maintenance *m, int reason);
void maint_new_session(maintenance *m);
int maint_register_command(maintenance *m, const char *name, maint_handler run, void *context);
void maint_close(maintenance *m);
/* Returns errno, response excludes the 4-byte status prefix. */
int maint_request(maintenance *m, unsigned op, const unsigned char *p, size_t n, unsigned char *out,
                  size_t *len);
#endif
