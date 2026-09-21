#ifndef MR_HOST_TIME_H
#define MR_HOST_TIME_H

#include <stdint.h>
#include <time.h>

static inline uint64_t mr_mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t mr_real_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline double mr_monotonic_ms(void) {
    return (double)mr_mono_ns() / 1e6;
}

#endif
