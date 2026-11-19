#include "wire.h"
#include <fcntl.h>
#include <string.h>
#include <time.h>
uint16_t rd16(const void *v) {
    const unsigned char *p = v;
    return p[0] | (uint16_t)p[1] << 8;
}
uint32_t rd32(const void *v) {
    const unsigned char *p = v;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t rd64(const void *v) {
    const unsigned char *p = v;
    return rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
void wr16(void *v, uint16_t n) {
    unsigned char *p = v;
    p[0] = n;
    p[1] = n >> 8;
}
void wr32(void *v, uint32_t n) {
    unsigned char *p = v;
    for (int i = 0; i < 4; i++)
        p[i] = n >> (8 * i);
}
void wr64(void *v, uint64_t n) {
    wr32(v, (uint32_t)n);
    wr32((unsigned char *)v + 4, (uint32_t)(n >> 32));
}
static uint32_t crc_more(uint32_t c, const unsigned char *p, size_t n) {
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return c;
}
uint32_t hd_crc(const void *p, size_t n) {
    return ~crc_more(~0u, p, n);
}
size_t frame_make(unsigned char *p, const char *m, unsigned op, uint16_t addr, uint32_t s,
                  uint32_t id, const void *d, size_t n) {
    if (n > HD_MAX)
        return 0;
    memcpy(p, m, 4);
    p[4] = 1;
    p[5] = op;
    wr16(p + 6, addr);
    wr32(p + 8, s);
    wr32(p + 12, id);
    wr32(p + 16, (uint32_t)n);
    if (n)
        memcpy(p + HD_HEAD, d, n);
    wr32(p + 20, ~crc_more(crc_more(~0u, p, 20), p + HD_HEAD, n));
    return HD_HEAD + n;
}
int frame_check(const unsigned char *p, size_t n, const char *m) {
    if (n < HD_HEAD)
        return 0;
    size_t len = rd32(p + 16);
    if (memcmp(p, m, 4) || p[4] != 1 || len > HD_MAX)
        return -1;
    if (n < HD_HEAD + len)
        return 0;
    return rd32(p + 20) == ~crc_more(crc_more(~0u, p, 20), p + HD_HEAD, len) ? 1 : -1;
}
int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int nonblock(int fd) {
    int f = fcntl(fd, F_GETFL);
    return f < 0 ? -1 : fcntl(fd, F_SETFL, f | O_NONBLOCK);
}
