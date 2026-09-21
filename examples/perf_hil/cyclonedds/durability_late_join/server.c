/*
 * HIL 3-way QoS-matrix comparison, scenario "durability_late_join" (rmw_tickle/COMPARISON.MD) -
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
 * separately in COMPARISON.MD; giving every participant in every later scenario both a writer and
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
    // Matches backlog_count (2026-09-20, real bug - consistently, deterministically received=8/20
    // on the rig across repeated runs, exactly this old depth): TRANSIENT_LOCAL durability in
    // CycloneDDS (no separate persistence service running here) serves a late joiner directly from
    // the writer's own regular HISTORY cache, not something durability_service tracks
    // independently - a shallower regular depth here caps what's replayable regardless of
    // durability_service's own depth=20 below.
    dds_qset_history(data_qos, DDS_HISTORY_KEEP_LAST, backlog_count);
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
        dds_return_t wrc = dds_write(writer, &msg);
        if (wrc != DDS_RETCODE_OK) {
            fprintf(stderr, "dds_write(seq=%u) failed: %s\n", i, dds_strretcode(-wrc));
        }
    }

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(ack_reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, ack_reader, 0);

    // 40s, not 20s (2026-09-20): client.c's own worst case (up to two 15s match-waits sequentially
    // + a 15s collection window) can run past 20s on its own before ever sending the ack.
    printf("Waiting up to 40s for the late subscriber's own ack...\n");
    uint64_t deadline = now_ns() + 40ULL * 1000000000ULL;
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
