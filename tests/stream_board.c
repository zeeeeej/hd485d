/* Explicit isolated-board regression. Sends synthetic bytes in abort mode;
 * never run alongside a real business transaction. No firmware callbacks. */
#include "hd485.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static long long ms(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
int main(int argc,char **argv) {
    if(argc!=3 || (strcmp(argv[2],"abort") && strcmp(argv[2],"stall")))return 2;
    hd485 *h=hd485_open(argv[1],5000);uint32_t ticket=0;
    if(!h || hd485_acquire(h,1000) || hd485_tx_begin(h,524288,&ticket))return 3;
    if(!strcmp(argv[2],"abort")) {
        char b[8192];memset(b,'x',sizeof(b));
        for(int i=0;i<4;i++)if(hd485_tx_data(h,b,sizeof(b)))return 4;
        hd485_close(h);puts("STREAM_DISCONNECTED");return 0;
    }
    long long begin=ms();
    while(ms()-begin<4000) {
        hd485_event e;int r=hd485_next_event(h,&e);
        if(r<0)return 5;
        if(r && e.type==HD485_TX_DONE && e.a==ticket) {
            if(e.b!=ETIMEDOUT || hd485_release(h))return 6;
            printf("STREAM_TIMEOUT_OK %lld\n",ms()-begin);hd485_close(h);return 0;
        }
        if(!r){struct pollfd p={hd485_get_event_fd(h),POLLIN,0};poll(&p,1,100);}
    }
    hd485_close(h);return 7;
}
