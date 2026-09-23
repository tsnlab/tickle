/*
 * HIL 3-way QoS-matrix comparison, scenario "deadline_miss_detection" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) publisher role. Publishes at a normal cadence (`-i`, default 20ms)
 * well under the DEADLINE (`-D`, default 50ms, matched exactly against server.c's own reader QoS -
 * the actual variable under test) for `-n` samples, except for exactly one deliberately-skipped
 * interval partway through (a real sleep, `deadline_s * 3`, not a simulated flag) - the real miss
 * this scenario exists to detect, both on this side (OFFERED_DEADLINE_MISSED) and the reader's
 * (REQUESTED_DEADLINE_MISSED, server.c's own concern).
 *
 * A listener, not a poll-after-sleep (2026-09-20, real methodology bug found on the rig): an
 * earlier version polled its own waitset right after each nanosleep() call - but that call is what
 * *causes* the deliberate gap, so the poll can only ever run once the whole gap has already
 * elapsed, not when the deadline itself actually expires partway through it. A real run showed
 * `detect_latency_ms=100` for a 150ms gap against a 50ms deadline - almost exactly (gap - one
 * period), i.e. purely an artifact of only checking once the sleep already returned, not a real
 * detection delay. dds_lset_offered_deadline_missed()'s own callback runs on CycloneDDS's own
 * internal thread, genuinely asynchronously to this loop's own blocking sleep, so it can actually
 * observe the real detection latency.
 *
 * `total_count` staying at exactly `ceil(gap / deadline)` (not just "1"): a writer silent for
 * three full deadline periods genuinely misses the deadline three times, once per period boundary
 * crossed with nothing new published - real, correct DDS behavior confirmed by reading the
 * listener's own per-callback status, not an assumption this scenario got right by luck.
 */
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dds/dds.h>

#include "../common.h"
#include "Bench.h"

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static _Atomic uint32_t g_miss_count = 0;
static _Atomic uint64_t g_first_miss_detect_ns = 0;

static void on_offered_deadline_missed(dds_entity_t writer, const dds_offered_deadline_missed_status_t status,
                                       void* arg) {
    (void)writer;
    (void)arg;
    uint64_t now = now_ns();
    uint64_t expected = 0;
    if (atomic_compare_exchange_strong(&g_first_miss_detect_ns, &expected, now)) {
        // first callback invocation only - leave it recorded.
    }
    atomic_store(&g_miss_count, status.total_count);
}

int main(int argc, char** argv) {
    double interval_s = 0.02; // 50/s, well under the 50ms deadline by default
    double deadline_s = 0.05;
    uint32_t count = 100;
    uint32_t miss_at = 50; // which sample index deliberately skips its own interval
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            deadline_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            miss_at = (uint32_t)atoi(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    dds_listener_t* listener = dds_create_listener(NULL);
    dds_lset_offered_deadline_missed(listener, on_offered_deadline_missed);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_deadline(qos, (dds_duration_t)(deadline_s * 1e9));
    dds_entity_t writer = dds_create_writer(participant, topic, qos, listener);
    dds_delete_qos(qos);
    dds_delete_listener(listener);
    if (writer < 0) {
        fprintf(stderr, "dds_create_writer failed\n");
        return 1;
    }

    if (!wait_for_writer_match(participant, writer, 10.0)) {
        fprintf(stderr, "timed out waiting for a matched reader\n");
        return 1;
    }

    uint32_t seq = 0;
    uint64_t sent = 0;
    uint64_t expected_miss_boundary_ns = 0;

    while (!g_interrupted && seq < count) {
        struct Bench msg = {.seq = ++seq, .send_ns = now_ns()};
        dds_write(writer, &msg);
        sent++;

        double this_interval_s = interval_s;
        if (seq == miss_at) {
            // The one deliberate miss: sleep well past the deadline before the next write, so the
            // deadline timer genuinely expires with nothing new published for this instance.
            this_interval_s = deadline_s * 3.0;
            expected_miss_boundary_ns = now_ns() + (uint64_t)(deadline_s * 1e9);
            printf("Publisher: deliberately skipping the interval after seq=%u (sleeping %.3fs, deadline=%.3fs)\n", seq,
                   this_interval_s, deadline_s);
        }
        uint64_t sleep_ns = (uint64_t)(this_interval_s * 1e9);
        struct timespec pace = {.tv_sec = (time_t)(sleep_ns / 1000000000ULL),
                                .tv_nsec = (long)(sleep_ns % 1000000000ULL)};
        nanosleep(&pace, NULL);
    }

    // The listener runs asynchronously - give it a brief real window to have fired for the very
    // last period boundary before reading final counts.
    struct timespec settle = {0, 200 * 1000 * 1000};
    nanosleep(&settle, NULL);

    uint32_t miss_count = atomic_load(&g_miss_count);
    uint64_t first_detect_ns = atomic_load(&g_first_miss_detect_ns);
    double detect_latency_ms = (first_detect_ns > 0 && expected_miss_boundary_ns > 0)
                                   ? (double)((int64_t)first_detect_ns - (int64_t)expected_miss_boundary_ns) / 1e6
                                   : -1.0;

    printf("RESULT: framework=cyclonedds scenario=deadline_miss_detection role=client sent=%lu "
           "offered_missed_total=%u detect_latency_ms=%.3f\n",
           (unsigned long)sent, miss_count, detect_latency_ms);

    dds_delete(participant);
    return 0;
}
