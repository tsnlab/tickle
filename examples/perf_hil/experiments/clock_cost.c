// What one clock read costs on the rig, against an empty loop as the control. The publish path reads the clock four
// times per sample (gprof call counts, 2026-10-04), so this sizes that term without a profiler: the rig has no perf,
// and gprof's per-function times on this code were not credible (it attributed an inlined callback to a function
// that runs twice per process).
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define NS_PER_S 1000000000ULL
#define ITERATIONS 20000000U

static uint64_t now_ns(void) {
    struct timespec now;
    // CLOCK_MONOTONIC lives in a glibc-private header; <time.h> is the public one.
    // NOLINTNEXTLINE(misc-include-cleaner)
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * NS_PER_S) + (uint64_t)now.tv_nsec;
}

int main(void) {
    volatile uint64_t sink = 0;
    uint64_t start = now_ns();
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        sink += i; // control: the loop and the store, without the clock
    }
    uint64_t mid = now_ns();
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        sink += now_ns();
    }
    uint64_t end = now_ns();
    printf("control_ns_per_iter=%.2f clock_ns_per_iter=%.2f clock_cost_ns=%.2f\n", (double)(mid - start) / ITERATIONS,
           (double)(end - mid) / ITERATIONS, (double)((end - mid) - (mid - start)) / ITERATIONS);
    return (int)(sink & 0U);
}
