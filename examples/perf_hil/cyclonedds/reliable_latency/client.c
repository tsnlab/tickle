/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_latency" (rmw_tickle/COMPARISON.md) -
 * CycloneDDS native (no rmw) client/ping role. Writer on "ping", reader on "pong" - measures
 * round-trip time the same way TickLE's own examples/linux/ping_pong/ping.c does (same
 * RESULT line shape, for direct comparison): send a sample with the current monotonic time
 * embedded, wait for the server's own echo on "pong", compute RTT from that embedded timestamp.
 */
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dds/dds.h>

#include "../../tickle/common/BenchStats.h" // shared instrumentation - see its own header
#include "../../tickle/common/BenchWindow.h"
#include "../../tickle/common/CpuFreq.h"
#include "../../tickle/common/RttQuantiles.h"
#include "../common.h"
#include "Bench.h"

static struct BenchStats g_bench_stats;
static char g_bench_fields[BENCH_STATS_FIELDS_MAX];

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

// CPU frequency around each round trip (2026-09-25). A tail excursion after the scheduler-driven poll
// has two platform explanations besides the change itself: an ordinary loss recovery, or the ondemand
// governor lowering the package clock once the client stopped spinning and a P-state change - made
// through the firmware, with a latency the kernel reports as unknown - landing on a round trip.
// Sampled AFTER the round trip is recorded, never between send and receive, so it cannot perturb what
// it measures. Identical in all three frameworks' clients, per the fairness rule.
static struct BenchCpuFreq g_rtt_freq;
static struct BenchRtt g_rtt;
static double cpu_mhz_at_rtt_max = -1.0;

// The whole run's counts (transmitted, received) and the measured window's (BenchWindow.h): only a measured round
// trip enters the RTT statistics.
struct rtt_stats {
    uint64_t transmitted;
    uint64_t received;
    uint64_t measured_sent;
    uint64_t measured_recv;
    double min_ms;
    double max_ms;
    double sum_ms;
};

static const double ns_per_s_real = 1e9;
static const double ns_per_ms_real = 1e6;
static const uint64_t ns_per_s_int = 1000000000ULL;
static const dds_duration_t response_wait = DDS_MSECS(500);

static void sleep_s(double seconds) {
    uint64_t sleep_ns = (uint64_t)(seconds * ns_per_s_real);
    struct timespec sleep_ts = {.tv_sec = (time_t)(sleep_ns / ns_per_s_int),
                                .tv_nsec = (long)(sleep_ns % ns_per_s_int)};
    nanosleep(&sleep_ts, NULL);
}

// One round trip: write a ping, wait up to 500 ms for its own echo.
static void ping_once(dds_entity_t writer, dds_entity_t reader, dds_entity_t waitset, uint32_t seq, bool measured,
                      struct rtt_stats* stats) {
    struct Bench req = {.seq = seq, .send_ns = now_ns()};
    dds_write(writer, &req);
    stats->transmitted++;
    if (measured) {
        stats->measured_sent++;
    }

    struct Bench resp;
    void* samples[1] = {&resp};
    dds_sample_info_t infos[1];
    dds_return_t ready = dds_waitset_wait(waitset, NULL, 0, response_wait);
    if (ready <= 0) {
        return;
    }
    dds_return_t taken = dds_take(reader, samples, infos, 1, 1);
    if (taken <= 0 || !infos[0].valid_data || resp.seq != req.seq) {
        return;
    }
    double rtt_ms = (double)(now_ns() - resp.send_ns) / ns_per_ms_real;
    stats->received++;
    if (!measured) {
        return; // a warm-up or cool-down round trip: counted in recv=, kept out of every statistic
    }
    stats->measured_recv++;
    if (stats->min_ms < 0.0 || rtt_ms < stats->min_ms) {
        stats->min_ms = rtt_ms;
    }
    bool new_max = rtt_ms > stats->max_ms;
    if (new_max) {
        stats->max_ms = rtt_ms;
    }
    stats->sum_ms += rtt_ms;
    BenchRtt_add(&g_rtt, rtt_ms);
    BenchCpuFreq_sample(&g_rtt_freq, now_ns(), 0);
    if (new_max) {
        cpu_mhz_at_rtt_max = BenchCpuFreq_last_mhz(&g_rtt_freq);
    }
}

int main(int argc, char** argv) {
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&g_bench_stats);
    BenchCpuFreq_init(&g_rtt_freq);
    double interval_s = 1.0;
    // -d: the measured window, between warm-up and cool-down.
    double duration_s = 10.0;
    uint32_t warmup_rtts = BENCH_WARMUP_ROUND_TRIPS;
    uint32_t cooldown_rtts = BENCH_COOLDOWN_ROUND_TRIPS;
    double edge_interval_s = BENCH_EDGE_INTERVAL_S;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
            warmup_rtts = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            cooldown_rtts = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            edge_interval_s = atof(argv[++i]);
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

    // scenario "reliable_latency" - RELIABLE + HISTORY depth=8, matched exactly across every
    // framework (COMPARISON.md's own design principle 3 / QoS value matrix).
    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);

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
    // Both calls unconditionally, into separate variables (2026-09-20, real bug - see
    // best_effort_latency/client.c's own doc comment for the full story): `!A(...) || !B(...)`
    // short-circuits B when A already succeeded, so the reader's own match never gets confirmed.
    bool writer_matched = wait_for_writer_match(participant, writer, 10.0);
    bool reader_matched = wait_for_reader_match(participant, reader, 10.0);
    if (!writer_matched || !reader_matched) {
        fprintf(stderr, "timed out waiting for a match\n");
        return 1;
    }

    struct rtt_stats stats = {.min_ms = -1.0};
    uint32_t seq = 0;

    // Warm-up, measured window, cool-down (BenchWindow.h), paced exactly as the TickLE client paces them: the edge
    // interval between edge round trips, the measured -i before every measured ping including the first, and before
    // the first cool-down ping.
    uint32_t warmup_sent = 0;
    while (!g_interrupted && warmup_sent < warmup_rtts) {
        ping_once(writer, reader, waitset, ++seq, false, &stats);
        warmup_sent++;
        sleep_s(warmup_sent < warmup_rtts ? edge_interval_s : interval_s);
    }
    uint64_t deadline = now_ns() + (uint64_t)(duration_s * ns_per_s_real);
    while (!g_interrupted && now_ns() < deadline) {
        ping_once(writer, reader, waitset, ++seq, true, &stats);
        sleep_s(interval_s);
    }
    uint32_t cooldown_sent = 0;
    while (!g_interrupted && cooldown_sent < cooldown_rtts) {
        ping_once(writer, reader, waitset, ++seq, false, &stats);
        cooldown_sent++;
        sleep_s(edge_interval_s);
    }

    uint64_t lost = stats.transmitted - stats.received;
    double loss_pct = stats.transmitted > 0 ? (100.0 * (double)lost / (double)stats.transmitted) : 0.0;
    double avg = stats.measured_recv > 0 ? stats.sum_ms / (double)stats.measured_recv : 0.0;

    printf("\n--- cyclonedds reliable_latency statistics ---\n");
    printf("%lu sent, %lu received, %.0f%% loss\n", (unsigned long)stats.transmitted, (unsigned long)stats.received,
           loss_pct);
    if (stats.measured_recv > 0) {
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms over %lu measured round trips\n", stats.min_ms, avg, stats.max_ms,
               (unsigned long)stats.measured_recv);
    }
    bench_stats_end(&g_bench_stats);
    printf("RESULT: framework=cyclonedds scenario=reliable_latency sent=%lu recv=%lu loss_pct=%.6f "
           "rtt_min_ms=%.3f rtt_avg_ms=%.6f rtt_max_ms=%.3f cpu_mhz_mean=%.1f cpu_mhz_min=%.1f cpu_mhz_max=%.1f "
           "cpu_mhz_at_rtt_max=%.1f rtt_p50_ms=%.3f rtt_p99_ms=%.3f rtt_kept=%u "
           "warmup=%u cooldown=%u measured=%lu measured_sent=%lu edge_interval_s=%.4f window=%s %s\n",
           (unsigned long)stats.transmitted, (unsigned long)stats.received, loss_pct, stats.min_ms, avg, stats.max_ms,
           BenchCpuFreq_mean_mhz(&g_rtt_freq), BenchCpuFreq_min_mhz(&g_rtt_freq), BenchCpuFreq_max_mhz(&g_rtt_freq),
           cpu_mhz_at_rtt_max, BenchRtt_quantile(&g_rtt, BENCH_RTT_P50), BenchRtt_quantile(&g_rtt, BENCH_RTT_P99),
           (unsigned)g_rtt.count, warmup_sent, cooldown_sent, (unsigned long)stats.measured_recv,
           (unsigned long)stats.measured_sent, edge_interval_s,
           stats.measured_recv > 0 ? "ok" : "fail:no_measured_round_trip",
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_SENDER, stats.transmitted, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    dds_delete(participant);
    return 0;
}
