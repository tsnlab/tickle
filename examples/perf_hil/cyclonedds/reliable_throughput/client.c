/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) client/sender role. Unlike the two latency scenarios, this is a
 * one-way stream (no pong) - mirrors TickLE's own examples/linux/perf/perf_client.c: the sender
 * just blasts samples, the *receiver* (server.c) is the authoritative side for loss/throughput,
 * since only it can see what actually arrived.
 */
#include <dds/dds.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Bench.h"
#include "../common.h"

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
    double duration_s = 10.0;
    double interval_s = 0.0;        // -i: pause between writes, 0 = as fast as possible
    double max_blocking_ms = 10000; // -B: RELIABILITY max_blocking_time, default unchanged (10s)
    double drain_s = 3.0;           // cap on the teardown wait-for-acknowledgements below
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-B") == 0 && i + 1 < argc) {
            max_blocking_ms = atof(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    // scenario "reliable_throughput" - RELIABLE, matched exactly across frameworks
    // (COMPARISON.MD's design principle 3). KEEP_ALL + generous resource_limits (2026-09-20,
    // matching the real upstream eclipse-cyclonedds/cyclonedds examples/throughput/publisher.c's
    // own prepare_dds()): KEEP_LAST(8) was real, bisected root cause of a genuine 53% loss under
    // RELIABLE at full send rate on the rig (a shallow writer history queue backpressures/drops
    // under sustained high-rate writes long before the network itself is the bottleneck) - not a
    // discovery/matching problem, a resource-limits tuning gap against the official example.
    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, (dds_duration_t)(max_blocking_ms * 1e6));
    dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, 0);
    dds_qset_resource_limits(qos, 4000, DDS_LENGTH_UNLIMITED, DDS_LENGTH_UNLIMITED);
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
    uint64_t sent = 0;
    // Writes dds_write() refused (e.g. DDS_RETCODE_TIMEOUT once KEEP_ALL's resource_limits are full
    // for longer than max_blocking_time). seq is still consumed, so the server counts each one as
    // lost too; write_fail lets the two be told apart (rmw_tickle/PLAN.md Phase 3, item 5).
    uint64_t write_fail = 0;
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(duration_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        struct Bench msg = {.seq = ++seq, .send_ns = now_ns()};
        if (dds_write(writer, &msg) == DDS_RETCODE_OK) {
            sent++;
        } else {
            write_fail++;
        }
        if (interval_s > 0.0) {
            struct timespec pace = {(time_t)interval_s, (long)((interval_s - (time_t)interval_s) * 1e9)};
            nanosleep(&pace, NULL);
        }
    }

    // Teardown drain, matching what the TickLE harness does (examples/perf_hil/tickle/
    // reliable_throughput/client.c) so all three frameworks are measured the same way at the end
    // of a run: a Subscriber only notices a gap when a higher seq_no arrives, and after the last
    // write none ever does, so a lost final sample is invisible unless the writer waits for
    // acknowledgements. Without this, a framework's tail loss depends on drain luck rather than
    // on its own reliability. dds_wait_for_acks() is CycloneDDS's own equivalent.
    dds_return_t acked = dds_wait_for_acks(writer, (dds_duration_t)(drain_s * 1e9));
    const char* drained = acked == DDS_RETCODE_OK ? "acked" : "timeout";

    double elapsed_s = (double)(now_ns() - start) / 1e9;
    double mbps = elapsed_s > 0.0 ? ((double)sent * sizeof(struct Bench) * 8.0) / 1e6 / elapsed_s : 0.0;
    printf("RESULT: framework=cyclonedds scenario=reliable_throughput role=client sent=%lu write_fail=%lu "
           "elapsed_s=%.3f send_mbps=%.3f max_blocking_ms=%.3f drained=%s\n",
           (unsigned long)sent, (unsigned long)write_fail, elapsed_s, mbps, max_blocking_ms, drained);

    dds_delete(participant);
    return 0;
}
