/*
 * HIL 3-way QoS-matrix comparison, scenario "deadline_miss_detection" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) subscriber role. Mirrors client.c's own DEADLINE QoS exactly (RxO
 * requires the reader's own requested deadline be >= the writer's offered one; matching them
 * exactly is simplest and this exercise's own established convention) and independently monitors
 * this side's own REQUESTED_DEADLINE_MISSED_STATUS - the client's own single deliberate miss
 * should be visible here too, on its own timeline, not just on the writer's side.
 */
#include <signal.h>
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

int main(int argc, char** argv) {
    double deadline_s = 0.05;
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            deadline_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_deadline(qos, (dds_duration_t)(deadline_s * 1e9));
    dds_entity_t reader = dds_create_reader(participant, topic, qos, NULL);
    dds_delete_qos(qos);
    if (reader < 0) {
        fprintf(stderr, "dds_create_reader failed\n");
        return 1;
    }

    if (!wait_for_reader_match(participant, reader, 10.0)) {
        fprintf(stderr, "timed out waiting for a matched writer\n");
        return 1;
    }

    // A single waitset watching both real data arrival (to drain the stream normally, matching
    // every other scenario's own convention) and this reader's own REQUESTED_DEADLINE_MISSED_STATUS
    // (the actual thing under test here) - both conditions on the same reader entity.
    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS | DDS_REQUESTED_DEADLINE_MISSED_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    uint64_t received = 0;
    bool miss_detected = false;
    uint64_t miss_detect_ns = 0;

#define DDL_MAX_BATCH 16
    static struct Bench batch[DDL_MAX_BATCH];
    void* samples[DDL_MAX_BATCH];
    dds_sample_info_t infos[DDL_MAX_BATCH];
    for (int i = 0; i < DDL_MAX_BATCH; i++) {
        samples[i] = &batch[i];
    }

    uint64_t deadline = now_ns() + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_MSECS(200));
        if (rc <= 0) {
            continue;
        }
        dds_return_t n;
        while ((n = dds_take(reader, samples, infos, DDL_MAX_BATCH, DDL_MAX_BATCH)) > 0) {
            for (dds_return_t i = 0; i < n; i++) {
                if (infos[i].valid_data) {
                    received++;
                }
            }
        }
        if (!miss_detected) {
            dds_requested_deadline_missed_status_t status;
            dds_get_requested_deadline_missed_status(reader, &status);
            if (status.total_count > 0) {
                miss_detected = true;
                miss_detect_ns = now_ns();
            }
        }
    }

    dds_requested_deadline_missed_status_t final_status;
    dds_get_requested_deadline_missed_status(reader, &final_status);

    printf("RESULT: framework=cyclonedds scenario=deadline_miss_detection role=server recv=%lu "
           "requested_missed_total=%u\n",
           (unsigned long)received, final_status.total_count);
    (void)miss_detect_ns;

    dds_delete(participant);
    return 0;
}
