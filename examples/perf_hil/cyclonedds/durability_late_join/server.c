/*
 * HIL 3-way QoS-matrix comparison, scenario "durability_late_join" (rmw_tickle/comparison.md) -
 * CycloneDDS native (no rmw) publisher role.
 *
 * Publishes N samples on "ping" *before* any subscriber exists (the whole point of this
 * scenario - TRANSIENT_LOCAL vs VOLATILE backlog delivery to a late joiner), then waits for the
 * late subscriber's own ack on "pong" to know when it's actually done - deliberately
 * writer+reader on *both* ends (mirrors the proven-reliable best_effort_latency/reliable_latency
 * ping-pong topology), not a pure write-only role: a real, unresolved discovery-asymmetry bug
 * found while building the best_effort_throughput/reliable_throughput scenarios (a write-only
 * participant's own dds_get_publication_matched_status() never went non-zero against a pure
 * read-only peer, even though the peer's own matched status did) - not solved yet, tracked
 * separately in comparison.md; giving every participant in every later scenario both a writer and
 * a reader sidesteps it without waiting on the real fix.
 *
 * -D: durable (TRANSIENT_LOCAL) instead of the default volatile - the actual variable under test.
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
    uint32_t backlog_count = 20;
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
    dds_qset_history(data_qos, DDS_HISTORY_KEEP_LAST, 8);
    if (durable) {
        dds_qset_durability(data_qos, DDS_DURABILITY_TRANSIENT_LOCAL);
        // Depth 20, matching backlog_count - without this, the durability service's own default
        // history depth (independent of the regular KEEP_LAST depth=8 set above) only retained 1
        // sample for late-joiner delivery, a real finding from the first test run of this
        // scenario (received=1 of 20 sent).
        dds_qset_durability_service(data_qos, DDS_SECS(0), DDS_HISTORY_KEEP_LAST, 20, -1, -1, -1);
    }
    dds_entity_t writer = dds_create_writer(participant, data_topic, data_qos, NULL);
    dds_delete_qos(data_qos);

    dds_qos_t* ack_qos = dds_create_qos();
    dds_qset_reliability(ack_qos, DDS_RELIABILITY_BEST_EFFORT, 0);
    dds_entity_t ack_reader = dds_create_reader(participant, ack_topic, ack_qos, NULL);
    dds_delete_qos(ack_qos);

    printf("Publisher: durable=%d, publishing %u backlog samples before any subscriber exists\n", durable,
           backlog_count);
    for (uint32_t i = 1; i <= backlog_count; i++) {
        struct Bench msg = {.seq = i, .send_ns = now_ns()};
        dds_write(writer, &msg);
    }

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(ack_reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, ack_reader, 0);

    printf("Waiting up to 20s for the late subscriber's own ack...\n");
    uint64_t deadline = now_ns() + 20ULL * 1000000000ULL;
    bool acked = false;
    struct Bench ack_sample;
    void* samples[1] = {&ack_sample};
    dds_sample_info_t infos[1];
    while (!g_interrupted && now_ns() < deadline && !acked) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_SECS(1));
        if (rc <= 0) {
            continue;
        }
        dds_return_t n = dds_take(ack_reader, samples, infos, 1, 1);
        if (n > 0 && infos[0].valid_data) {
            acked = true;
        }
    }

    printf("RESULT: framework=cyclonedds scenario=durability_late_join role=server durable=%d "
           "backlog_sent=%u acked=%d\n",
           durable, backlog_count, acked);

    dds_delete(participant);
    return 0;
}
