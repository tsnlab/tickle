/*
 * HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) subscriber role. Deliberately stalls its own consumption for
 * `pause_s` seconds right after matching (`-p`, default 3.0), *before* ever creating the receive
 * waitset - a stalled/slow subscriber is exactly what HISTORY depth is meant to protect against
 * (real-world equivalent: a reader's own application thread busy elsewhere). The writer (client.c)
 * keeps writing on a fixed schedule the whole time regardless, so this window alone determines how
 * many samples pile up before this side ever looks - some recoverable (still within the writer's
 * own KEEP_LAST(8) retention when this side finally catches up), some not (evicted or write-side
 * dropped once the writer's own bounded WHC/max_blocking_time is exceeded - see client.c's own doc
 * comment for that half of the mechanism).
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
    double pause_s = 3.0;
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pause_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    // Fixed depth=8, matched exactly against client.c's own writer QoS (COMPARISON.MD's design
    // principle 3, and the actual variable under test here).
    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);
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

    printf("Subscriber: matched, now deliberately stalling for %.1fs before consuming anything\n", pause_s);
    struct timespec pause_ts = {.tv_sec = (time_t)pause_s, .tv_nsec = (long)((pause_s - (time_t)pause_s) * 1e9)};
    nanosleep(&pause_ts, NULL);

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    // last_seq starts at 0, not "whatever the first received sample's own seq happens to be"
    // (2026-09-20, real measurement bug found the hard way): the writer starts at seq=1, so if the
    // very first sample this reader ever takes is, say, seq=53 (because its own KEEP_LAST(8) cache
    // genuinely evicted 1-52 while this side was deliberately stalled - the real phenomenon this
    // whole scenario exists to observe), treating 53 as the new baseline instead of comparing it
    // against the expected start (1) hid that entire loss from this counter - real runs on the rig
    // showed "recv=108, lost=0" for a 160-sample send, silently missing the leading 52-sample gap
    // this fix now correctly attributes to `lost` on the very first sample taken.
    uint64_t received = 0, lost = 0;
    uint32_t last_seq = 0;

#define BURST_MAX_BATCH 256
    static struct Bench batch[BURST_MAX_BATCH];
    void* samples[BURST_MAX_BATCH];
    dds_sample_info_t infos[BURST_MAX_BATCH];
    for (int i = 0; i < BURST_MAX_BATCH; i++) {
        samples[i] = &batch[i];
    }

    uint64_t deadline = now_ns() + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_SECS(1));
        if (rc <= 0) {
            continue;
        }
        dds_return_t n;
        while ((n = dds_take(reader, samples, infos, BURST_MAX_BATCH, BURST_MAX_BATCH)) > 0) {
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

    printf("RESULT: framework=cyclonedds scenario=history_depth_burst_loss role=server pause_s=%.1f "
           "recv=%lu lost=%lu loss_pct=%.1f\n",
           pause_s, (unsigned long)received, (unsigned long)lost, loss_pct);

    dds_delete(participant);
    return 0;
}
