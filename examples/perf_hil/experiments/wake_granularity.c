// Measures G for RFC 6298's RTO = srtt + max(G, K*rttvar), as proposed by TickLE Dev 2026-09-26.
//
// G is the one term in that formula that is a property of the HOST rather than the link: how late
// a wake scheduled for time t actually happens. It is what stops the retry interval converging on
// srtt itself on a very steady link, where rttvar decays toward zero and a retry at the *mean*
// response time would fire while half the responses are still in flight.
//
// What is measured: ppoll() with a computed relative timeout and no fds, which is exactly what
// tt_Node_poll() does when it waits for the next scheduler entry. Lateness = actual elapsed -
// requested. CLOCK_MONOTONIC throughout. Built with -D_GNU_SOURCE by wake_granularity.sh (ppoll
// needs it on glibc), and declared here too so clang-tidy sees the declaration - same
// NOLINTNEXTLINE precedent as src/hal_linux.c:19.
//
// Pre-registered reading, written before the run:
//   - G is the p99 lateness at the SHORT delay. p99 rather than max because a single outlier is
//     a preemption, not granularity, and rather than mean because G's job is to cover the common
//     bad case, not the typical one.
//   - CONTROL: the same measurement at a 10x longer delay. If lateness is a host property it must
//     be ~the same at both. If it scales with the requested delay it is not granularity at all -
//     it is proportional timer slack - and a single absolute G would be the wrong shape entirely.
//     That outcome invalidates the proposal rather than adjusting its number, which is why the
//     control is here.
//   - A p99 far above the 100us Dev is building with provisionally means the provisional value is
//     too small and spurious retransmits survive the change; far below means G is nearly free and
//     could be tightened.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NS_PER_SEC 1000000000LL
#define SHORT_DELAY_NS 200000L
#define CONTROL_DELAY_NS 2000000L
#define DEFAULT_SAMPLES 20000
#define MIN_SAMPLES 100
#define PCT_90 90
#define PCT_99 99
#define PCT_DIV 100

static int cmp_lateness(const void* lhs, const void* rhs) {
    long long left = *(const long long*)lhs;
    long long right = *(const long long*)rhs;
    return (left > right) - (left < right);
}

static void arm(const char* label, long requested_ns, int samples) {
    long long* late = malloc((size_t)samples * sizeof(long long));
    if (late == NULL) {
        exit(1);
    }
    for (int idx = 0; idx < samples; idx++) {
        struct timespec want = {.tv_sec = requested_ns / NS_PER_SEC, .tv_nsec = requested_ns % NS_PER_SEC};
        struct timespec before;
        struct timespec after;
        // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_MONOTONIC/ppoll come from the includes above
        clock_gettime(CLOCK_MONOTONIC, &before);
        // NOLINTNEXTLINE(misc-include-cleaner)
        ppoll(NULL, 0, &want, NULL);
        // NOLINTNEXTLINE(misc-include-cleaner)
        clock_gettime(CLOCK_MONOTONIC, &after);
        long long elapsed = ((after.tv_sec - before.tv_sec) * NS_PER_SEC) + (after.tv_nsec - before.tv_nsec);
        late[idx] = elapsed - requested_ns;
    }
    qsort(late, (size_t)samples, sizeof(long long), cmp_lateness);
    long long sum = 0;
    for (int idx = 0; idx < samples; idx++) {
        sum += late[idx];
    }
    printf("arm=%s requested_ns=%ld n=%d late_mean_ns=%lld late_p50_ns=%lld late_p90_ns=%lld "
           "late_p99_ns=%lld late_max_ns=%lld\n",
           label, requested_ns, samples, sum / samples, late[samples / 2], late[(samples * PCT_90) / PCT_DIV],
           late[(samples * PCT_99) / PCT_DIV], late[samples - 1]);
    free(late);
}

int main(int argc, char** argv) {
    int samples = (argc > 1) ? atoi(argv[1]) : DEFAULT_SAMPLES;
    if (samples < MIN_SAMPLES) {
        samples = MIN_SAMPLES;
    }
    arm("short_200us", SHORT_DELAY_NS, samples);
    arm("control_2ms", CONTROL_DELAY_NS, samples);
    return 0;
}
