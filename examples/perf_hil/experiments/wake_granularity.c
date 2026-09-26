// Measures G for RFC 6298's RTO = srtt + max(G, K*rttvar), as proposed by TickLE Dev 2026-09-26.
//
// G is the one term in that formula that is a property of the HOST rather than the link: how late
// a wake scheduled for time t actually happens. It is what stops the retry interval converging on
// srtt itself on a very steady link, where rttvar decays toward zero and a retry at the *mean*
// response time would fire while half the responses are still in flight.
//
// What is measured: ppoll() with a computed relative timeout and no fds, which is exactly what
// tt_Node_poll() does when it waits for the next scheduler entry. Lateness = actual elapsed -
// requested. CLOCK_MONOTONIC throughout.
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
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}

static void arm(const char *label, long req_ns, int n) {
    long long *late = malloc((size_t)n * sizeof(long long));
    if (late == NULL) {
        exit(1);
    }
    for (int i = 0; i < n; i++) {
        struct timespec want = {.tv_sec = req_ns / 1000000000L, .tv_nsec = req_ns % 1000000000L};
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ppoll(NULL, 0, &want, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long long el = (t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
        late[i] = el - req_ns;
    }
    qsort(late, (size_t)n, sizeof(long long), cmp_ll);
    long long sum = 0;
    for (int i = 0; i < n; i++) {
        sum += late[i];
    }
    printf("arm=%s requested_ns=%ld n=%d late_mean_ns=%lld late_p50_ns=%lld late_p90_ns=%lld "
           "late_p99_ns=%lld late_max_ns=%lld\n",
           label, req_ns, n, sum / n, late[n / 2], late[(n * 90) / 100], late[(n * 99) / 100], late[n - 1]);
    free(late);
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 20000;
    if (n < 100) {
        n = 100;
    }
    arm("short_200us", 200000L, n);
    arm("control_2ms", 2000000L, n);
    return 0;
}
