#include "rx_filter.h"
#include <errno.h>
#include <string.h>
static void flush(rx_filter *f) {
    if(f->business_used)f->on_business(f->context,f->business,f->business_used);
    f->business_used=0;
}
static void business(rx_filter *f,const unsigned char *p,size_t n) {
    while(n) {
        size_t k=sizeof(f->business)-f->business_used;if(k>n)k=n;
        memcpy(f->business+f->business_used,p,k);f->business_used+=k;p+=k;n-=k;
        if(f->business_used==sizeof(f->business))flush(f);
    }
}
static void consume(rx_filter *f,size_t n) {
    f->used-=n;memmove(f->pending,f->pending+n,f->used);
}
static void parse(rx_filter *f) {
    static const unsigned char qjy[]={0xaa,0x5a};
    while(f->used) {
        size_t prefix=f->used<4?f->used:4;
        if(!memcmp(f->pending,"H485",prefix)) {
            int valid=frame_check(f->pending,f->used,"H485");
            if(!valid)return;
            if(valid>0) {
                size_t n=HD_HEAD+rd32(f->pending+16);
                flush(f);f->on_maintenance(f->context,f->pending,n);consume(f,n);continue;
            }
        } else if(f->qjy && !memcmp(f->pending,qjy,f->used<2?f->used:2)) {
            if(f->used<8)return;
            uint32_t n=rd32(f->pending+4);
            if(n<=512u*1024u) {
                /* Only a length counter: no 512 KiB allocation or inspection of
                 * business payload. Original receiver owns CRC validation. */
                business(f,f->pending,8);consume(f,8);f->frame_left=(uint64_t)n+2;return;
            }
        }
        business(f,f->pending,1);consume(f,1);
    }
}
void rx_filter_feed(rx_filter *f,const unsigned char *p,size_t n) {
    f->last=now_ms();
    while(n) {
        uint64_t *left=f->raw_left?&f->raw_left:&f->frame_left;
        if(*left) {
            size_t k=n;if((uint64_t)k>*left)k=(size_t)*left;
            business(f,p,k);*left-=k;p+=k;n-=k;continue;
        }
        f->pending[f->used++]=*p++;n--;parse(f);
    }
    flush(f);
}
void rx_filter_tick(rx_filter *f,int64_t timeout_ms) {
    if(now_ms()-f->last<timeout_ms)return;
    if(f->used) {business(f,f->pending,f->used);f->used=0;flush(f);}
    /* A recognized QJY payload is length-delimited, like the original business
     * parser. Do not reinterpret its bytes after an arbitrary idle gap. */
}
int rx_filter_raw_begin(rx_filter *f,uint64_t n) {
    if(!n)return EINVAL;
    if(f->used || f->frame_left || f->raw_left)return EBUSY;
    f->raw_left=n;return 0;
}
void rx_filter_raw_end(rx_filter *f) {f->raw_left=0;}
