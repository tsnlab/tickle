/*
 * HIL 3-way QoS-matrix comparison, scenario "lifespan_expiry" (rmw_tickle/comparison.md) -
 * CycloneDDS native (no rmw) publisher role. Writes a fixed count (`-n`) at a fixed rate (`-i`,
 * default 20ms) with LIFESPAN (`-T`, default 100ms, matched exactly against server.c's own reader
 * QoS - the actual variable under test) and a generous `HISTORY KEEP_ALL` + `resource_limits` -
 * deliberately *not* the shallow `KEEP_LAST(8)` scenario 6 tests, since this scenario's own point
 * is a sample's individual age-based expiry, not queue-depth eviction; conflating the two would
 * make it unclear which mechanism actually caused any given loss. `dds_wait_for_acks()` before
 * teardown - see history_depth_burst_loss/client.c's own doc comment for the real bug this avoids
 * (a duration/count-based writer tearing itself down before a stalled reader has caught up).
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
    double interval_s = 0.02;
    double lifespan_s = 0.1;
    uint32_t count = 100;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            lifespan_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = (uint32_t)atoi(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, 0);
    dds_qset_resource_limits(qos, 500, DDS_LENGTH_UNLIMITED, DDS_LENGTH_UNLIMITED);
    dds_qset_lifespan(qos, (dds_duration_t)(lifespan_s * 1e9));
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
    uint64_t interval_ns = (uint64_t)(interval_s * 1e9);

    while (!g_interrupted && seq < count) {
        struct Bench msg = {.seq = ++seq, .send_ns = now_ns()};
        dds_write(writer, &msg);
        sent++;
        struct timespec pace = {.tv_sec = (time_t)(interval_ns / 1000000000ULL),
                                .tv_nsec = (long)(interval_ns % 1000000000ULL)};
        nanosleep(&pace, NULL);
    }

    dds_return_t wait_rc = dds_wait_for_acks(writer, DDS_SECS(10));
    if (wait_rc != DDS_RETCODE_OK) {
        fprintf(stderr, "dds_wait_for_acks: %s\n", dds_strretcode(-wait_rc));
    }

    printf("RESULT: framework=cyclonedds scenario=lifespan_expiry role=client sent=%lu\n", (unsigned long)sent);

    dds_delete(participant);
    return 0;
}
