#ifndef HD485_WIRE_H
#define HD485_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#define HD_MAX 8192u
#define HD_HEAD 24u
#define HD_PACKET (HD_HEAD + HD_MAX)
#ifdef __linux__
#define HD_SOCK SOCK_SEQPACKET
#else
#define HD_SOCK SOCK_STREAM
#endif
enum {
    ACQUIRE = 1,
    RELEASE,
    SUBSCRIBE,
    UNSUBSCRIBE,
    WRITE,
    GET_CONFIG,
    SET_CONFIG,
    STATUS,
    RX = 0x70,
    EVENT = 0x71
};
enum {
    M_HELLO = 1,
    M_EXEC,
    M_POLL,
    M_CANCEL,
    M_PUT_BEGIN,
    M_PUT_DATA,
    M_PUT_COMMIT,
    M_GET_BEGIN,
    M_GET_DATA,
    M_STATUS,
    M_FS_INFO = 32,
    M_FS_LIST,
    M_FS_READ,
    M_FS_CLOSE,
    M_FS_STAT,
    M_FS_PUT,
    M_FS_GET,
    M_FILE_CLOSE
};
uint16_t rd16(const void *p);
uint32_t rd32(const void *p);
uint64_t rd64(const void *p);
void wr16(void *p, uint16_t n);
void wr32(void *p, uint32_t n);
void wr64(void *p, uint64_t n);
uint32_t hd_crc(const void *p, size_t n);
size_t frame_make(unsigned char *out, const char *magic, unsigned op, uint16_t address,
                  uint32_t session, uint32_t id, const void *data, size_t len);
/* 1 = complete, 0 = incomplete, -1 = invalid. Frame CRC includes header[0:20]. */
int frame_check(const unsigned char *p, size_t n, const char *magic);
int64_t now_ms(void);
int nonblock(int fd);
#endif
