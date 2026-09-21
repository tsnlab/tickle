/*
 * HIL 3-way QoS-matrix comparison, scenario "lifespan_expiry" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) subscriber role. Mirrors client.c's own LIFESPAN + generous
 * `HISTORY KEEP_ALL` exactly, then deliberately stalls its own consumption for `-p` seconds right
 * after matching, before ever taking a single sample - the real thing under test: whether samples
 * written *during* that stall, once older than LIFESPAN by the time this side finally looks, are
 * correctly never delivered at all (age-based expiry) rather than delivered late (which
 * `HISTORY KEEP_ALL` alone, with no LIFESPAN, would do - this scenario is deliberately built with
 * generous history specifically so any loss observed here is attributable to LIFESPAN, not queue
 * depth, unlike scenario 6's own `KEEP_LAST(8)` design).
 *
 * Gap tracking starts `last_seq` at 0, not the first sample actually taken - see
 * history_depth_burst_loss/server.c's own doc comment for the real measurement bug this avoids
 * (treating whichever sample arrives first as the baseline silently hides real leading loss).
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
    double pause_s = 0.3;
    double lifespan_s = 0.1;
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pause_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            lifespan_s = atof(argv[++i]);
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
    dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, 0);
    dds_qset_resource_limits(qos, 500, DDS_LENGTH_UNLIMITED, DDS_LENGTH_UNLIMITED);
    dds_qset_lifespan(qos, (dds_duration_t)(lifespan_s * 1e9));
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

    printf("Subscriber: matched, now deliberately stalling for %.3fs before consuming anything (lifespan=%.3fs)\n",
           pause_s, lifespan_s);
    struct timespec pause_ts = {.tv_sec = (time_t)pause_s, .tv_nsec = (long)((pause_s - (time_t)pause_s) * 1e9)};
    nanosleep(&pause_ts, NULL);

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    uint64_t received = 0, lost = 0;
    uint32_t last_seq = 0;

#define LIFESPAN_MAX_BATCH 256
    static struct Bench batch[LIFESPAN_MAX_BATCH];
    void* samples[LIFESPAN_MAX_BATCH];
    dds_sample_info_t infos[LIFESPAN_MAX_BATCH];
    for (int i = 0; i < LIFESPAN_MAX_BATCH; i++) {
        samples[i] = &batch[i];
    }

    uint64_t deadline = now_ns() + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_SECS(1));
        if (rc <= 0) {
            continue;
        }
        dds_return_t n;
        while ((n = dds_take(reader, samples, infos, LIFESPAN_MAX_BATCH, LIFESPAN_MAX_BATCH)) > 0) {
            for (dds_return_t i = 0; i < n; i++) {
                if (!infos[i].valid_data) {
                    continue;
                }
                uint32_t seq = batch[i].seq;
                if (seq > last_seq + 1) {
                    lost += (seq - last_seq - 1);
                    last_seq = seq;
                } else {
                    last_seq = seq;
                }
                received++;
            }
        }
    }

    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;

    printf("RESULT: framework=cyclonedds scenario=lifespan_expiry role=server pause_s=%.3f "
           "recv=%lu lost=%lu loss_pct=%.1f\n",
           pause_s, (unsigned long)received, (unsigned long)lost, loss_pct);

    dds_delete(participant);
    return 0;
}
