// C12 step 5 (2026-10-31): a light periodic warmer for the cold-core diagnostic. Every PERIOD_US it wakes on an
// absolute CLOCK_MONOTONIC schedule and spins WORK_US on the clock, so the cpu it is pinned to (taskset) is never idle
// longer than one period, at a load of WORK_US / PERIOD_US (5 / 10000 = 0.05%). It touches no file, socket or shared
// memory. It ends by itself after DURATION_S and always prints one line:
//   WARMER_END iters=<n> expected=<n> cpu=<cpu at the end> allowed=<cpu mask, hex> work_us=<n> period_us=<n>
// Usage: warmer DURATION_S [WORK_US=5] [PERIOD_US=10000]
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NS_PER_US 1000LL
#define NS_PER_SEC 1000000000LL
#define EX_USAGE 64
#define MASK_CPUS 64

static int64_t now_ns(void) {
    struct timespec ts;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_MONOTONIC: <time.h>, via a glibc-private header
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * NS_PER_SEC) + ts.tv_nsec;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        (void)fprintf(stderr, "usage: warmer DURATION_S [WORK_US] [PERIOD_US]\n");
        return EX_USAGE;
    }
    const int64_t duration = atoll(argv[1]) * NS_PER_SEC;
    const int64_t work = (argc > 2 ? atoll(argv[2]) : 5) * NS_PER_US;
    const int64_t period = (argc > 3 ? atoll(argv[3]) : 10000) * NS_PER_US;
    const int64_t start = now_ns();
    int64_t next = start;
    long iters = 0;
    volatile uint64_t sink = 0;
    while (next - start < duration) {
        next += period;
        struct timespec wake_at = {(time_t)(next / NS_PER_SEC), (long)(next % NS_PER_SEC)};
        // NOLINTNEXTLINE(misc-include-cleaner) - TIMER_ABSTIME: <time.h>, via a glibc-private header
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake_at, NULL);
        const int64_t until = now_ns() + work;
        while (now_ns() < until) {
            sink = sink + 1;
        }
        iters++;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    unsigned long mask = 0;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int cpu = 0; cpu < MASK_CPUS; cpu++) {
            if (CPU_ISSET(cpu, &set)) {
                mask |= 1UL << (unsigned)cpu;
            }
        }
    }
    printf("WARMER_END iters=%ld expected=%lld cpu=%d allowed=%lx work_us=%lld period_us=%lld\n", iters,
           (long long)(duration / period), sched_getcpu(), mask, (long long)(work / NS_PER_US),
           (long long)(period / NS_PER_US));
    return 0;
}
