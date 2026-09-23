/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) server/receiver role. The authoritative side for loss/throughput -
 * only it can see what actually arrived (see client.c's own doc comment). Runs until SIGINT
 * (sent by the orchestrating run_scenario.sh once the client's own -d duration elapses) or its
 * own generous safety cap, matching run_perf.sh's own perf_server.c precedent.
 */
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dds/dds.h>

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
    // +15s buffer - see best_effort_throughput/server.c's own doc comment (this same directory)
    // for the real bug this avoids.
    safety_cap_s += 15.0;

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    // KEEP_ALL + generous resource_limits + batch-take (2026-09-20, matching the real upstream
    // eclipse-cyclonedds/cyclonedds examples/throughput/subscriber.c's own prepare_dds()/do_take()):
    // KEEP_LAST(8) plus a one-sample-at-a-time dds_take() was real, bisected root cause of a
    // genuine 53% app-level loss under RELIABLE at full send rate on the rig - the app fell behind
    // its own shallow reader queue, which is a resource-limits/drain-rate tuning gap against the
    // official example, not a discovery/matching problem.
    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, 0);
    dds_qset_resource_limits(qos, 4000, DDS_LENGTH_UNLIMITED, DDS_LENGTH_UNLIMITED);
    dds_entity_t reader = dds_create_reader(participant, topic, qos, NULL);
    dds_delete_qos(qos);
    if (reader < 0) {
        fprintf(stderr, "dds_create_reader failed\n");
        return 1;
    }

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    uint64_t received = 0, lost = 0;
    uint32_t last_seq = 0;
    bool first = true;
    uint64_t first_recv_ns = 0, last_recv_ns = 0;

#define MAX_BATCH 1000
    static struct Bench batch[MAX_BATCH];
    void* samples[MAX_BATCH];
    dds_sample_info_t infos[MAX_BATCH];
    for (int i = 0; i < MAX_BATCH; i++) {
        samples[i] = &batch[i];
    }

    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_SECS(1));
        if (rc <= 0) {
            continue;
        }
        dds_return_t n;
        while ((n = dds_take(reader, samples, infos, MAX_BATCH, MAX_BATCH)) > 0) {
            for (dds_return_t i = 0; i < n; i++) {
                if (!infos[i].valid_data) {
                    continue;
                }
                uint32_t seq = batch[i].seq;
                if (first) {
                    first = false;
                    first_recv_ns = now_ns();
                    last_seq = seq;
                } else if (seq > last_seq + 1) {
                    lost += (seq - last_seq - 1);
                    last_seq = seq;
                } else {
                    last_seq = seq;
                }
                received++;
            }
            last_recv_ns = now_ns();
        }
    }

    double elapsed_s = received > 0 ? (double)(last_recv_ns - first_recv_ns) / 1e9 : 0.0;
    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;
    double mbps = elapsed_s > 0.0 ? ((double)received * sizeof(struct Bench) * 8.0) / 1e6 / elapsed_s : 0.0;

    printf("RESULT: framework=cyclonedds scenario=reliable_throughput role=server recv=%lu lost=%lu "
           "loss_pct=%.1f elapsed_s=%.3f recv_mbps=%.3f\n",
           (unsigned long)received, (unsigned long)lost, loss_pct, elapsed_s, mbps);

    dds_delete(participant);
    return 0;
}
