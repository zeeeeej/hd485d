#include "sha256.h"
#include "wire.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    assert(hd_crc("123456789", 9) == 0xcbf43926u);
    sha256_ctx s;
    unsigned char h[32];
    sha256_init(&s);
    sha256_update(&s, "abc", 3);
    sha256_final(&s, h);
    const unsigned char expected[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    assert(!memcmp(h, expected, 32));
    unsigned char f[HD_PACKET];
    size_t n = frame_make(f, "H485", 2, 1, 2, 3, "hello", 5);
    assert(n == 29);
    for (size_t i = 0; i < n; i++)
        assert(frame_check(f, i, "H485") == 0);
    assert(frame_check(f, n, "H485") == 1);
    f[28] ^= 1;
    assert(frame_check(f, n, "H485") == -1);
    wr32(f + 16, HD_MAX + 1);
    assert(frame_check(f, n, "H485") == -1);
    puts("CRC32, SHA256 and framing vectors passed");
    return 0;
}
