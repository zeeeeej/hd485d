#ifndef HD_TX_TRACE_H
#define HD_TX_TRACE_H
#include <inttypes.h>
#include <stdio.h>
#include <time.h>
/* Explicit opt-in; monotonic timestamps, no business payloads or credentials. */
static inline void tx_trace(int enabled, const char *stage, uint32_t ticket, size_t bytes) {
    if (!enabled) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(stderr, "TXTRACE %" PRId64 " %s %u %zu\n",
            (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000, stage, ticket, bytes);
}
#endif
