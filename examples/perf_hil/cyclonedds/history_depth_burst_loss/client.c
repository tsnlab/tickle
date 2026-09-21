/*
 * HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) publisher role. Writes on a fixed schedule (`-i`, default 50ms)
 * regardless of whether the subscriber is actually consuming - server.c's own doc comment explains
 * why it deliberately doesn't for its own first `-p` seconds. `max_blocking_time` is short (200ms,
 * not the 1s used elsewhere in this exercise) so a stalled subscriber's effect is directly visible
 * here too: once depth=8's own bounded write-history-cache (WHC) fills and stays unacknowledged
 * past that timeout, dds_write() genuinely fails (DDS_RETCODE_TIMEOUT) rather than blocking this
 * loop indefinitely - real, deliberate write-side loss, not a network-level drop, and the actual
 * mechanism this scenario is exercising (HISTORY depth is what bounds how much a RELIABLE writer
 * will hold for a slow reader before giving up, not an unlimited buffer).
 */
#include <signal.h>
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

int main(int argc, char** argv) {
    double interval_s = 0.05; // 20 samples/s by default
    // Fixed sample COUNT, not a fixed duration (2026-09-20, real finding): a duration-based loop
    // exits at a fixed wall-clock deadline regardless of whether the reader has actually caught up
    // yet - a real, reproduced ~52-sample gap on the rig traced to exactly that (the writer's own
    // participant torn down mid-catch-up), nothing to do with HISTORY depth or watermarks. A fixed
    // count plus dds_wait_for_acks() below removes wall-clock timing as a variable entirely.
    uint32_t count = 160;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = (uint32_t)atoi(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    // Fixed depth=8, matched exactly against server.c's own reader QoS - the actual variable under
    // test. max_blocking_time=200ms (not this exercise's usual 1s): short enough that a genuinely
    // stalled reader produces real, timely write failures within this test's own run time, not
    // just a long stall that happens to still resolve before any deadline is hit.
    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_MSECS(200));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);
    dds_entity_t writer = dds_create_writer(participant, topic, qos, NULL);
    dds_delete_qos(qos);
    if (writer < 0) {
        fprintf(stderr, "dds_create_writer failed\n");
        return 1;
    }

    if (!wait_for_writer_match(participant, writer, 10.0)) {
        fprintf(stderr, "timed out waiting for a matched reader\n");
        return 1;
    }

    uint32_t seq = 0;
    uint64_t sent = 0, dropped = 0;
    uint64_t interval_ns = (uint64_t)(interval_s * 1e9);

    while (!g_interrupted && seq < count) {
        struct Bench msg = {.seq = ++seq, .send_ns = now_ns()};
        dds_return_t rc = dds_write(writer, &msg);
        if (rc == DDS_RETCODE_OK) {
            sent++;
        } else {
            dropped++;
        }
        struct timespec pace = {.tv_sec = (time_t)(interval_ns / 1000000000ULL),
                                .tv_nsec = (long)(interval_ns % 1000000000ULL)};
        nanosleep(&pace, NULL);
    }

    // Wait for real acks before tearing down (2026-09-20, real finding): without this, the writer's
    // own participant gets deleted the instant this fixed-duration loop ends, regardless of whether
    // a still-catching-up reader has actually received everything yet - a real, reproduced 52-
    // sample gap on the rig (client sent 160, server's own gap-free count stopped dead at 108) that
    // had nothing to do with HISTORY depth or watermarks at all, just this writer vanishing mid-
    // catch-up. dds_wait_for_acks() blocks until every reliable reader has genuinely acked
    // everything (or the timeout below elapses) - this is what actually lets the depth/watermark
    // mechanism under test be observed cleanly, instead of being confounded by premature shutdown.
    dds_return_t wait_rc = dds_wait_for_acks(writer, DDS_SECS(10));
    if (wait_rc != DDS_RETCODE_OK) {
        fprintf(stderr, "dds_wait_for_acks: %s\n", dds_strretcode(-wait_rc));
    }

    printf("RESULT: framework=cyclonedds scenario=history_depth_burst_loss role=client sent=%lu "
           "write_dropped=%lu\n",
           (unsigned long)sent, (unsigned long)dropped);

    dds_delete(participant);
    return 0;
}
