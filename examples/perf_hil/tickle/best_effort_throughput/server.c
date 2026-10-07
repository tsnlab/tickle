/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "best_effort_throughput" (rmw_tickle/COMPARISON.md) -
// TickLE core native server/receiver role (no rmw). One-way stream, no reply - this side is the
// authoritative one for loss/throughput since only it sees what actually arrived (mirrors
// examples/perf_hil/{cyclonedds,fastdds}/best_effort_throughput/server.c's own role split).
// Gap-detection initializes last_seq to 0, not the first-received sample - see
// examples/perf_hil/cyclonedds/history_depth_burst_loss/server.c's own doc comment for the real
// bug this avoids (silently hiding real leading loss).

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
#include "BenchHistory.h"
#include "BenchStats.h" // shared instrumentation - see its own header
#include "BenchWindow.h"

static struct BenchStats g_bench_stats;
static char g_bench_fields[BENCH_STATS_FIELDS_MAX];
// What arrived, windowed by the sender's own clock exactly as the client windows what it sent (BenchWindow.h).
static struct BenchWindow g_window;
static char g_window_fields[BENCH_WINDOW_FIELDS_MAX];
// -K, accepted and echoed so all three frameworks are given the same arguments (BenchHistory.h). TickLE has no
// history cache to apply it to; history= says so and names the ring that queues instead.
static int g_history = BENCH_HISTORY_DEFAULT;
static char g_history_field[BENCH_HISTORY_FIELD_MAX];

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint64_t received = 0;
static uint64_t lost = 0;
static uint32_t last_seq = 0;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    if (data->seq > last_seq + 1) {
        lost += (data->seq - last_seq - 1);
    }
    last_seq = data->seq;
    received++;
    BenchWindow_add(&g_window, data->send_ns);
}

static const double default_safety_cap_s = 40.0;
static const double safety_cap_buffer_s = 15.0;

int main(int argc, char** argv) {
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&g_bench_stats);
    double safety_cap_s = default_safety_cap_s;
    BenchWindow_init(&g_window);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        } else if (!BenchWindow_parse_arg(&g_window, argc, argv, &i)) {
            (void)BenchHistory_parse_arg(&g_history, argc, argv, &i);
        }
    }
    // The client sends for warm-up + -d + cool-down, so this side's backstop covers all three.
    safety_cap_s += g_window.warmup_s + g_window.cooldown_s;
    // +15s buffer - see deadline_miss_detection/server.c's own doc comment for the real bug this
    // avoids (run_scenario.sh forwards the same -d to both sides, but it means "this side's own
    // send duration" on the client vs. "don't hang forever" here - taken verbatim, this side could
    // exit mid-stream, before the client - which starts several seconds later - had even finished).
    safety_cap_s += safety_cap_buffer_s;

    // real HIL link's own broadcast address - see best_effort_latency/server.c's own doc comment
    // for the real bug this avoids.
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

    struct tt_Subscriber sub;
    ret = tt_Context_create_subscriber(&node, &sub, &BenchTopic, "stream", (tt_SUBSCRIBER_CALLBACK)stream_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    // 500ms (nanoseconds), so the deadline/g_interrupted check re-runs.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Context_poll(&node, poll_timeout_ns);
    }

    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;

    // The seam's own per-transport counts, for SHM_PLAN's S2 assertion (tx_udp must be 0 for a same-host pair once the
    // segment carries the shape under test). Read from the context rather than counted here, so one place counts.
    bench_stats_set_transport(&g_bench_stats, node.tx_datagrams_by_transport, node.rx_datagrams_by_transport,
                              (size_t)tt_TRANSPORT_COUNT);
    bench_stats_set_shm_diagnostics(&g_bench_stats, node.rx_span_absorbed, node.segment_head_stalls,
                                    node.segment_stall_warnings, node.rx_shm_only_on_socket,
                                    node.rx_shm_skipped_superseded);
    bench_stats_set_fallbacks(&g_bench_stats, node.segment_broadcast_to_udp, node.segment_oversized_to_udp,
                              node.segment_unattached_to_udp, node.segment_full_dropped);
    // Beside them, and never without them: how often the context asked /dev/shm about a peer at all.
    // segment_unattached_to_udp counts datagrams that fell back; this counts the attempts behind
    // them, and the day the two were equal is the day cross-host throughput halved.
    bench_stats_set_attach(&g_bench_stats, node.segment_attach, (size_t)tt_SEGMENT_ATTACH_COUNT,
                           (size_t)tt_SEGMENT_ATTACHED, (size_t)tt_SEGMENT_ABSENT);
    bench_stats_end(&g_bench_stats);

    printf("RESULT: framework=tickle scenario=best_effort_throughput role=server recv=%lu lost=%lu loss_pct=%.6f %s "
           "history=none segment_slots=%d %s %s\n",
           (unsigned long)received, (unsigned long)lost, loss_pct,
           BenchHistory_arg_field(g_history, g_history_field, sizeof g_history_field),
           tt_SEGMENT_ENABLED ? (int)tt_SEGMENT_SLOTS : 0,
           BenchWindow_fields(&g_window, "recv", "recv", BENCH_SAMPLE_BYTES, g_window_fields, sizeof g_window_fields),
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_RECEIVER, received, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    tt_Context_destroy(&node);
    return 0;
}
