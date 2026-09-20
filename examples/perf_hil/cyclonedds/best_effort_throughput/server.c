/*
 * HIL 3-way QoS-matrix comparison, scenario "best_effort_throughput" (rmw_tickle/comparison.md) -
 * CycloneDDS native (no rmw) server/receiver role. The authoritative side for loss/throughput -
 * only it can see what actually arrived (see client.c's own doc comment). Runs until SIGINT
 * (sent by the orchestrating run_scenario.sh once the client's own -d duration elapses) or its
 * own generous safety cap, matching run_perf.sh's own perf_server.c precedent.
 *
 * No explicit match-wait (2026-09-20, rewritten to match the real upstream
 * eclipse-cyclonedds/cyclonedds examples/roundtrip/pong.c pattern - see client.c's own doc
 * comment for the full rationale): the official example creates its waitset and attaches a
 * data-availability condition immediately after the reader, with no prior match-status check at
 * all. Whatever is lost during the initial discovery window is an accepted warm-up cost, not a
 * condition to detect and gate on.
 *
 * Dummy reciprocal writer (2026-09-20, retest): see client.c's own doc comment - a real rig run
 * with the match-wait removed still produced recv=0 end-to-end, isolating this participant's own
 * read-only shape (mismatched against every official/working example, which always pairs a
 * writer+reader on the same participant) as the next hypothesis to test, independent of the
 * match-wait fix already applied above.
 */
#include <dds/dds.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);
    dds_entity_t ack_topic = dds_create_topic(participant, &Bench_desc, "stream_ack", NULL, NULL);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
    dds_entity_t reader = dds_create_reader(participant, topic, qos, NULL);
    dds_entity_t dummy_writer = dds_create_writer(participant, ack_topic, qos, NULL);
    dds_delete_qos(qos);
    if (reader < 0 || dummy_writer < 0) {
        fprintf(stderr, "dds_create_reader/writer failed\n");
        return 1;
    }

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    uint64_t received = 0, lost = 0;
    uint32_t last_seq = 0;
    bool first = true;
    uint64_t first_recv_ns = 0, last_recv_ns = 0;

    struct Bench sample;
    void* samples[1] = {&sample};
    dds_sample_info_t infos[1];

    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_SECS(1));
        if (rc <= 0) {
            continue;
        }
        dds_return_t n = dds_take(reader, samples, infos, 1, 1);
        if (n > 0 && infos[0].valid_data) {
            if (first) {
                first = false;
                first_recv_ns = now_ns();
                last_seq = sample.seq;
            } else if (sample.seq > last_seq + 1) {
                lost += (sample.seq - last_seq - 1);
                last_seq = sample.seq;
            } else {
                last_seq = sample.seq;
            }
            last_recv_ns = now_ns();
            received++;
        }
    }

    double elapsed_s = received > 0 ? (double)(last_recv_ns - first_recv_ns) / 1e9 : 0.0;
    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;
    double mbps = elapsed_s > 0.0 ? ((double)received * sizeof(struct Bench) * 8.0) / 1e6 / elapsed_s : 0.0;

    printf("RESULT: framework=cyclonedds scenario=best_effort_throughput role=server recv=%lu lost=%lu "
           "loss_pct=%.1f elapsed_s=%.3f recv_mbps=%.3f\n",
           (unsigned long)received, (unsigned long)lost, loss_pct, elapsed_s, mbps);

    dds_delete(participant);
    return 0;
}
