/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "reliable_latency" (rmw_tickle/COMPARISON.md) -
// TickLE core native server/pong role (no rmw). Subscribes "ping", republishes the exact same
// sample on "pong" immediately - never logs per-request (matches the FastDDS/CycloneDDS server's
// own identical precedent).

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "Bench.h"
#include "BenchCreate.h"
#include "BenchWindow.h"
#include "CpuFreq.h"
#include "LatencyReliability.h" // best_effort_latency is this file with BEST_EFFORT

// Reorder-buffer geometry for this example's RELIABLE Subscriber - see where it is assigned.
//
// Sized to the widest tracking window this build allows, not to a round number. A Subscriber can
// hold at most as many samples as it can track ahead of its oldest gap, so a buffer that covers
// the widest window makes overflow impossible by construction rather than unlikely - the same
// argument rmw_tickle's RMW_TICKLE_REORDER_SLOTS rests on. This was 512 at first, which is
// narrower than the -w a reliable_throughput run is allowed to request (up to 4096), so any run
// that asked for a wide window would have overflowed into the re-request fallback and measured
// that instead of the protocol.
// 4096: tt_RELIABLE_BITMAP_MAX_BITS until large-message stage 2 raised it to 8192 (docs/DESIGN.md section 8), pinned
// so these cells stay the experiment they were - the same ring, warmed the same way.
#define BENCH_REORDER_SLOTS 4096

// The two latency scenarios' reliability (LatencyReliability.h), chosen here rather than by a conditional at each use.
#if BENCH_LATENCY_BEST_EFFORT
#define BENCH_LATENCY_KIND tt_QOS_RELIABILITY_BEST_EFFORT
#define BENCH_LATENCY_REORDER_SLOTS tt_QOS_REORDER_NONE // a best-effort reader holds no sample for a gap
#else
#define BENCH_LATENCY_KIND tt_QOS_RELIABILITY_RELIABLE
#define BENCH_LATENCY_REORDER_SLOTS BENCH_REORDER_SLOTS
#endif
// The slot stride is core's since the QoS redesign: tt_Subscriber_storage_size() sizes it from BenchTopic's
// max_encoded_size, a datagram's payload at most (docs/DESIGN.md section 9).

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static struct tt_Publisher* g_pub;

// TickLE-only diagnostics for the latency tail question (2026-09-25), not compared across frameworks -
// the CycloneDDS and FastDDS latency servers print no RESULT line at all. Sampled twice, when the loop
// starts and when it ends, and never per echo (2026-10-06): a sample is an open, a read and a close of
// sysfs, and taken after every echo it put three syscalls a round trip into this server's system time that
// the CycloneDDS and FastDDS servers, which sample nothing, never paid - in a cell that compares exactly
// that cost. The clients sample per round trip, alike in all three. So the fields stay, and say what the
// server's clock was at the two ends of the run, not at each echo.
static struct BenchCpuFreq g_echo_freq;

static void ping_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    tt_Publisher_publish(g_pub, (struct tt_Data*)data);
}

static const double default_safety_cap_s = 40.0;
// +15s, as every other scenario's server already has (deadline_miss_detection/server.c explains why).
// run_scenario.sh forwards the same -d to both sides, and here it caps this server's lifetime, while
// the client starts pinging PRE_CLIENT_SLEEP (3s) plus its own discovery margin (2s) later. Without
// the buffer this server exited halfway through the client's pings: at -i 0.1 -d 10 it answered 47
// of 100, a steady "53% loss" that the 2026-09-24 re-sweep first read as a TickLE regression.
static const double safety_cap_buffer_s = 15.0;

int main(int argc, char** argv) {
    BenchCpuFreq_init(&g_echo_freq);
    double safety_cap_s = default_safety_cap_s;
    // The client's warm-up and cool-down, forwarded like -d (BenchWindow.h).
    uint32_t warmup_rtts = BENCH_WARMUP_ROUND_TRIPS;
    uint32_t cooldown_rtts = BENCH_COOLDOWN_ROUND_TRIPS;
    double edge_interval_s = BENCH_EDGE_INTERVAL_S;
    double edge_max_s = BENCH_EDGE_MAX_S;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
            warmup_rtts = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            cooldown_rtts = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            edge_interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            edge_max_s = atof(argv[++i]);
        }
    }
    safety_cap_s += safety_cap_buffer_s;
    // -d is the client's MEASURED window; its warm-up and cool-down come on top, each at most its time bound plus one
    // round trip (BenchWindow_edge_bound_s()). A backstop only: run_scenario.sh SIGINTs this server when the client is
    // done. It used to allow 5 ms per edge round trip, which a 10 ms netem delay outran: the server ended mid-warm-up,
    // its client gave every later ping up at 500 ms each and outlived the cell's timeout (c12, campaign 2026-10-07).
    safety_cap_s += BenchWindow_edge_bound_s(warmup_rtts, edge_interval_s, edge_max_s) +
                    BenchWindow_edge_bound_s(cooldown_rtts, edge_interval_s, edge_max_s);

    // real HIL link's own broadcast address (run_perf.sh's own PERF_LINK_BROADCAST) - the
    // compiled-in default (255.255.255.255) doesn't match this subnet, which breaks
    // tt_get_node_id()'s own auto-detection (matches the local address against broadcast's
    // subnet, config.h's own doc comment) - found the hard way (0 received, first real run).
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Context node;
    tt_ret_t ret = tt_Context_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    struct tt_Publisher pub;
    // RELIABLE + HISTORY depth=8, as the client's (COMPARISON.md's QoS value matrix); best_effort_latency: BEST_EFFORT.
    // VOLATILE, no DEADLINE, LIFESPAN or LIVELINESS lease. The retained samples are sized by core from the profile;
    // the history bytes are never below the arena this file used to declare, (8 + 1) records of this shape.
    const struct tt_QosProfile pub_qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_LAST,
                    .depth = 8,
                    .bytes = BenchCreate_history_bytes(
                        8, tt_RELIABLE_CACHE_ARENA_BYTES(8, tt_RELIABLE_RECORD_BYTES(sizeof(struct BenchData))))},
        .reliability = {.kind = BENCH_LATENCY_KIND},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
    ret = BenchCreate_publisher(&node, &pub, "pong", &pub_qos, NULL);
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
    g_pub = &pub;

    struct tt_Subscriber sub;
    // RELIABLE and VOLATILE, the builtin tracking window, BENCH_REORDER_SLOTS reorder slots (below), no DEADLINE or
    // LIVELINESS lease. KEEP_ALL keeps keep_last_depth 0, never skip (BenchCreate.h).
    //
    // Ordered delivery (2026-09-24) - somewhere to hold a sample that arrives ahead of a gap.
    //
    // Not optional for a benchmark. A RELIABLE Subscriber with no reorder buffer is still correct
    // - it declines to record an out-of-order sample as received, so the ACKNACK exchange fetches
    // it again once the gap has filled - but under real tc loss that turns one lost datagram into
    // a re-request for everything behind it. On the HIL rig that more than halved reliable receive
    // throughput and tripped the regression gate, which is how this was found.
    //
    // Sized to hold a full tracking window of samples: the window is exactly how far ahead of its
    // oldest missing sample this Subscriber is allowed to get, so it is also the most it can ever
    // need to hold at once.
    //
    // best_effort_latency: BEST_EFFORT and no reorder buffer, as the client's.
    const struct tt_QosProfile sub_qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_ALL},
        .reliability = {.kind = BENCH_LATENCY_KIND, .tracking_words = 0, .reorder_slots = BENCH_LATENCY_REORDER_SLOTS},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
    ret = BenchCreate_subscriber(&node, &sub, "ping", &sub_qos, (tt_SUBSCRIBER_CALLBACK)ping_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    // 500ms (nanoseconds), so the deadline/g_interrupted check re-runs.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    ret = tt_RET_OK;
    BenchCpuFreq_sample(&g_echo_freq, tt_get_ns(), 0); // the run's start; see g_echo_freq
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Context_poll(&node, poll_timeout_ns);
    }
    BenchCpuFreq_sample(&g_echo_freq, tt_get_ns(), 0); // and its end

    printf("RESULT: framework=tickle scenario=" BENCH_LATENCY_SCENARIO
           " role=server cpu_mhz_mean=%.1f cpu_mhz_min=%.1f "
           "cpu_mhz_max=%.1f retransmitted=%u gap_abandoned=%u doorbells_sent=%llu bells_rung=%llu sleeps=%lu "
           "rx_shm=%llu tx_shm=%llu reliability=%s\n",
           BenchCpuFreq_mean_mhz(&g_echo_freq), BenchCpuFreq_min_mhz(&g_echo_freq), BenchCpuFreq_max_mhz(&g_echo_freq),
           pub.retransmitted, sub.gap_abandoned, (unsigned long long)node.segment_doorbells_sent,
           (unsigned long long)node.segment_bells_rung, (unsigned long)node.segment_sleep_generation,
           (unsigned long long)node.rx_datagrams_by_transport[tt_TRANSPORT_SHM],
           (unsigned long long)node.tx_datagrams_by_transport[tt_TRANSPORT_SHM],
           bench_reliability_name((int)pub.reliable, (int)sub.reliable));

    tt_Context_destroy(&node);
    return 0;
}
