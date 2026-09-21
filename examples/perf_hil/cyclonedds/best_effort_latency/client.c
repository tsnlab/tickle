/*
 * HIL 3-way QoS-matrix comparison, scenario "best_effort_latency" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) client/ping role. Writer on "ping", reader on "pong" - measures
 * round-trip time the same way TickLE's own examples/linux/ping_pong/ping.c does (same
 * RESULT line shape, for direct comparison): send a sample with the current monotonic time
 * embedded, wait for the server's own echo on "pong", compute RTT from that embedded timestamp.
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
    double interval_s = 1.0;
    double duration_s = 10.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) {
        fprintf(stderr, "dds_create_participant: %s\n", dds_strretcode(-participant));
        return 1;
    }

    dds_entity_t ping_topic = dds_create_topic(participant, &Bench_desc, "ping", NULL, NULL);
    dds_entity_t pong_topic = dds_create_topic(participant, &Bench_desc, "pong", NULL, NULL);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);

    dds_entity_t writer = dds_create_writer(participant, ping_topic, qos, NULL);
    dds_entity_t reader = dds_create_reader(participant, pong_topic, qos, NULL);
    dds_delete_qos(qos);
    if (writer < 0 || reader < 0) {
        fprintf(stderr, "dds_create_writer/reader failed\n");
        return 1;
    }

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    // Give discovery a moment - a send before the writer/reader pair on both ends has matched
    // would just be lost, undercounting "sent" for no real reason.
    //
    // Both calls unconditionally, into separate variables (2026-09-20, real bug found the hard
    // way): `!wait_for_writer_match(...) || !wait_for_reader_match(...)` short-circuits - if the
    // writer matches first (the common case), the reader's own wait_for_reader_match() is never
    // even called, so the reader's own SUBSCRIPTION_MATCHED_STATUS is never confirmed before the
    // send loop starts. Reproduced as a real, reliable 100% RTT loss on the rig even with a clean
    // process restart and a generous 15s test window - not a network or discovery-speed issue.
    bool writer_matched = wait_for_writer_match(participant, writer, 10.0);
    bool reader_matched = wait_for_reader_match(participant, reader, 10.0);
    if (!writer_matched || !reader_matched) {
        fprintf(stderr, "timed out waiting for a match\n");
        return 1;
    }

    uint64_t transmitted = 0, received = 0;
    double rtt_min_ms = -1.0, rtt_max_ms = 0.0, rtt_sum_ms = 0.0, rtt_sum_sq_ms = 0.0;

    uint32_t seq = 0;
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(duration_s * 1e9);
    uint64_t interval_ns = (uint64_t)(interval_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        struct Bench req = {.seq = ++seq, .send_ns = now_ns()};
        dds_write(writer, &req);
        transmitted++;

        struct Bench resp;
        void* samples[1] = {&resp};
        dds_sample_info_t infos[1];
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_MSECS(500));
        if (rc > 0) {
            dds_return_t n = dds_take(reader, samples, infos, 1, 1);
            if (n > 0 && infos[0].valid_data && resp.seq == req.seq) {
                double rtt_ms = (double)(now_ns() - resp.send_ns) / 1e6;
                received++;
                if (rtt_min_ms < 0.0 || rtt_ms < rtt_min_ms) {
                    rtt_min_ms = rtt_ms;
                }
                if (rtt_ms > rtt_max_ms) {
                    rtt_max_ms = rtt_ms;
                }
                rtt_sum_ms += rtt_ms;
                rtt_sum_sq_ms += rtt_ms * rtt_ms;
            }
        }

        struct timespec sleep_ts = {.tv_sec = (time_t)(interval_ns / 1000000000ULL),
                                     .tv_nsec = (long)(interval_ns % 1000000000ULL)};
        nanosleep(&sleep_ts, NULL);
    }

    uint64_t lost = transmitted - received;
    double loss_pct = transmitted > 0 ? (100.0 * (double)lost / (double)transmitted) : 0.0;
    double avg = received > 0 ? rtt_sum_ms / (double)received : 0.0;

    printf("\n--- cyclonedds best_effort_latency statistics ---\n");
    printf("%lu sent, %lu received, %.0f%% loss\n", (unsigned long)transmitted, (unsigned long)received, loss_pct);
    if (received > 0) {
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", rtt_min_ms, avg, rtt_max_ms);
    }
    printf("RESULT: framework=cyclonedds scenario=best_effort_latency sent=%lu recv=%lu loss_pct=%.0f "
           "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
           (unsigned long)transmitted, (unsigned long)received, loss_pct, rtt_min_ms, avg, rtt_max_ms);

    dds_delete(participant);
    return 0;
}
