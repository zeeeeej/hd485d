#include "hd485.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: hd485-example SOCKET status|listen|echo|send [TEXT]\n");
        return 2;
    }
    hd485 *h = !strncmp(argv[1], "fd:", 3) ? hd485_open_fd(atoi(argv[1] + 3), 5000)
                                           : hd485_open(argv[1], 5000);
    if (!h) {
        perror("open");
        return 1;
    }
    int rc = 0;
    if (!strcmp(argv[2], "status")) {
        hd485_status s;
        if (hd485_get_status(h, &s)) {
            perror("status");
            rc = 1;
        } else
            printf("mode=%u owner=%u epoch=%u tx=%u error=%u rx=%u\n", s.mode, s.owner, s.epoch,
                   s.tx_queued, s.error, s.rx_sequence);
    } else if (!strcmp(argv[2], "send") && argc == 4) {
        uint32_t ticket;
        if (hd485_acquire(h, 0) || hd485_write(h, argv[3], strlen(argv[3]), &ticket) < 0 ||
            hd485_release(h)) {
            perror("send");
            rc = 1;
        }
    } else if (!strcmp(argv[2], "listen") || !strcmp(argv[2], "echo")) {
        int echo = !strcmp(argv[2], "echo");
        if ((echo ? hd485_acquire(h, 0) : hd485_subscribe(h)) < 0) {
            perror("subscribe/acquire");
            rc = 1;
        } else
            for (;;) {
                unsigned char data[8192];
                ssize_t n = hd485_read(h, data, sizeof(data), NULL, NULL);
                if (n < 0) {
                    if (errno == ETIMEDOUT)
                        continue;
                    perror("read");
                    rc = 1;
                    break;
                }
                if (echo) {
                    if (hd485_write(h, data, (size_t)n, NULL) < 0) {
                        perror("write");
                        rc = 1;
                        break;
                    }
                } else {
                    fwrite(data, 1, (size_t)n, stdout);
                    fflush(stdout);
                }
                hd485_event e;
                while (hd485_next_event(h, &e) > 0)
                    fprintf(stderr, "event %u %u %u\n", e.type, e.a, e.b);
            }
    } else {
        fprintf(stderr, "unknown command\n");
        rc = 2;
    }
    hd485_close(h);
    return rc;
}
