/*
 * HIL 3-way QoS-matrix comparison, scenario "best_effort_throughput" (rmw_tickle/comparison.md) -
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
    double interval_s = 0.0; // 0 = as fast as possible, matching perf_client.c's own default
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    // scenario "best_effort_throughput" - BEST_EFFORT, matched exactly across frameworks
    // (comparison.md's design principle 3).
    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
    dds_entity_t writer = dds_create_writer(participant, topic, qos, NULL);
    dds_delete_qos(qos);
    if (writer < 0) {
        fprintf(stderr, "dds_create_writer failed\n");
        return 1;
    }

    if (!wait_for_writer_match(writer, 10.0)) {
        fprintf(stderr, "timed out waiting for a matched reader\n");
        return 1;
    }

    uint32_t seq = 0;
    uint64_t sent = 0;
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(duration_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        struct Bench msg = {.seq = ++seq, .send_ns = now_ns()};
        if (dds_write(writer, &msg) == DDS_RETCODE_OK) {
            sent++;
        }
        if (interval_s > 0.0) {
            struct timespec pace = {.tv_sec = (time_t)interval_s, .tv_nsec = (long)((interval_s - (time_t)interval_s) * 1e9)};
            nanosleep(&pace, NULL);
        }
    }

    double elapsed_s = (double)(now_ns() - start) / 1e9;
    double mbps = elapsed_s > 0.0 ? ((double)sent * sizeof(struct Bench) * 8.0) / 1e6 / elapsed_s : 0.0;
    printf("RESULT: framework=cyclonedds scenario=best_effort_throughput role=client sent=%lu elapsed_s=%.3f "
           "send_mbps=%.3f\n",
           (unsigned long)sent, elapsed_s, mbps);

    dds_delete(participant);
    return 0;
}
