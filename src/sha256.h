#ifndef HD_SHA256_H
#define HD_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    unsigned char block[64];
} sha256_ctx;
void sha256_init(sha256_ctx *s);
void sha256_update(sha256_ctx *s, const void *p, size_t n);
void sha256_final(sha256_ctx *s, unsigned char out[32]);
int sha256_fd(int fd, unsigned char out[32]);
#endif
