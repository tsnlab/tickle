/*
 * HIL 3-way QoS-matrix comparison, scenario "durability_late_join" (rmw_tickle/comparison.md) -
 * CycloneDDS native (no rmw) subscriber role, deliberately started *after* the publisher has
 * already sent its whole backlog (run_scenario's own orchestration sleeps before starting this
 * side) - the actual thing under test: does TRANSIENT_LOCAL (-D) deliver that backlog anyway,
 * while VOLATILE (default) delivers none of it. See publisher.c's own doc comment for why this
 * side also has a writer (the ack), not just a reader.
 */
#include <dds/dds.h>
#include <signal.h>
#include <stdbool.h>
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
    bool durable = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0) {
            durable = true;
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t data_topic = dds_create_topic(participant, &Bench_desc, "ping", NULL, NULL);
    dds_entity_t ack_topic = dds_create_topic(participant, &Bench_desc, "pong", NULL, NULL);

    dds_qos_t* data_qos = dds_create_qos();
    dds_qset_reliability(data_qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    // Matches server.c's own backlog_count (20) - see its own doc comment for the real bug this
    // avoids (a shallower depth here caps how many replayed backlog samples this reader can even
    // hold, independent of the writer's own matching fix).
    dds_qset_history(data_qos, DDS_HISTORY_KEEP_LAST, 20);
    if (durable) {
        dds_qset_durability(data_qos, DDS_DURABILITY_TRANSIENT_LOCAL);
        // Depth 20, matching backlog_count - without this, the durability service's own default
        // history depth (independent of the regular KEEP_LAST depth=8 set above) only retained 1
        // sample for late-joiner delivery, a real finding from the first test run of this
        // scenario (received=1 of 20 sent).
        dds_qset_durability_service(data_qos, DDS_SECS(0), DDS_HISTORY_KEEP_LAST, 20, -1, -1, -1);
    }
    dds_entity_t reader = dds_create_reader(participant, data_topic, data_qos, NULL);
    dds_delete_qos(data_qos);

    dds_qos_t* ack_qos = dds_create_qos();
    dds_qset_reliability(ack_qos, DDS_RELIABILITY_BEST_EFFORT, 0);
    dds_entity_t ack_writer = dds_create_writer(participant, ack_topic, ack_qos, NULL);
    dds_delete_qos(ack_qos);

    // Both calls unconditionally, into separate variables (2026-09-20, real bug - see
    // best_effort_latency/client.c's own doc comment for the full story): `!A(...) || !B(...)`
    // short-circuits B when A already succeeded, silently skipping one of the two match waits.
    bool reader_matched = wait_for_reader_match(participant, reader, 15.0);
    bool ack_writer_matched = wait_for_writer_match(participant, ack_writer, 15.0);
    if (!reader_matched || !ack_writer_matched) {
        fprintf(stderr, "timed out waiting for a match\n");
        return 1;
    }

    uint64_t start = now_ns();
    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    uint32_t received = 0;
    // 15s, not 5s (2026-09-20, real finding): even with the KEEP_LAST(8) ceiling fixed above,
    // repeated real runs still showed variable partial delivery (8/10/11 of 20) inside a 5s window
    // - RELIABLE redelivery of a durability backlog goes through real ACKNACK round-trips per
    // sample over the actual network, not an instant local replay, and 5s wasn't reliably enough
    // for all 20 on this rig.
    uint64_t deadline = now_ns() + 15ULL * 1000000000ULL;
    // Batch-take, draining every buffered sample per wake (2026-09-20, real bug - same class
    // already found/fixed in reliable_throughput/server.c): taking one sample at a time here left
    // this variable, run-to-run partial delivery unexplained by the writer side alone - CycloneDDS
    // doesn't necessarily re-signal DDS_DATA_AVAILABLE_STATUS for every sample individually once
    // several arrive in one burst (the durability backlog replay arrives essentially all at once,
    // not one-by-one at network pace), so a single dds_take() per waitset wake could leave already-
    // arrived samples sitting unread in the reader's own queue, silently missed by this loop even
    // though the writer had already delivered them.
#define DURABILITY_MAX_BATCH 64
    static struct Bench batch[DURABILITY_MAX_BATCH];
    void* samples[DURABILITY_MAX_BATCH];
    dds_sample_info_t infos[DURABILITY_MAX_BATCH];
    for (int i = 0; i < DURABILITY_MAX_BATCH; i++) {
        samples[i] = &batch[i];
    }
    while (!g_interrupted && now_ns() < deadline) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_MSECS(500));
        if (rc <= 0) {
            continue;
        }
        dds_return_t n;
        while ((n = dds_take(reader, samples, infos, DURABILITY_MAX_BATCH, DURABILITY_MAX_BATCH)) > 0) {
            for (dds_return_t i = 0; i < n; i++) {
                if (infos[i].valid_data) {
                    received++;
                }
            }
        }
    }
    double backlog_delivery_ms = received > 0 ? (double)(now_ns() - start) / 1e6 : -1.0;

    struct Bench ack = {.seq = 1, .send_ns = now_ns()};
    dds_write(ack_writer, &ack);

    printf("RESULT: framework=cyclonedds scenario=durability_late_join role=client durable=%d "
           "received=%u backlog_delivery_ms=%.3f\n",
           durable, received, backlog_delivery_ms);

    dds_delete(participant);
    return 0;
}
