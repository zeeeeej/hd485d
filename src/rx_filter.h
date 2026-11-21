#ifndef HD_RX_FILTER_H
#define HD_RX_FILTER_H
#include "wire.h"
typedef void (*rx_sink)(void *, const unsigned char *, size_t);
typedef struct {
    unsigned char pending[HD_PACKET], business[4096];
    size_t used, business_used;
    uint64_t frame_left, raw_left;
    int qjy, pepsi;
    int64_t last;
    rx_sink on_business, on_maintenance;
    void *context;
} rx_filter;
void rx_filter_feed(rx_filter *, const unsigned char *, size_t);
void rx_filter_tick(rx_filter *, int64_t timeout_ms);
int rx_filter_raw_begin(rx_filter *, uint64_t);
void rx_filter_raw_end(rx_filter *);
#endif
