/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "reliable_latency" (rmw_tickle/COMPARISON.MD) -
// TickLE core native client/ping role (no rmw). Mirrors examples/perf_hil/{cyclonedds,fastdds}/
// reliable_latency/client.c's own logic exactly - single-clock RTT (this side's own
// tt_get_ns(), both send and receipt), same RESULT line shape, for direct comparison.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "../common/Bench.h"

// Reorder-buffer geometry for this example's RELIABLE Subscriber - see where it is assigned.
#define BENCH_REORDER_SLOTS 512
#define BENCH_REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + sizeof(struct BenchData) + 16)

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static const double default_duration_s = 10.0;
static const double discovery_margin_s = 2.0;
static const double ns_per_ms = 1e6;

static double interval_s = 1.0;
static double duration_s = default_duration_s;
static uint64_t transmitted = 0, received = 0;
static double rtt_min_ms = -1.0, rtt_max_ms = 0.0, rtt_sum_ms = 0.0;
static uint32_t seq = 0;
static struct tt_Publisher* g_pub;

static void pong_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    double rtt_ms = (double)(tt_get_ns() - data->send_ns) / ns_per_ms;
    received++;
    if (rtt_min_ms < 0.0 || rtt_ms < rtt_min_ms) {
        rtt_min_ms = rtt_ms;
    }
    if (rtt_ms > rtt_max_ms) {
        rtt_max_ms = rtt_ms;
    }
    rtt_sum_ms += rtt_ms;
}

static void ping(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted) {
        return;
    }
    struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
    tt_ret_t ret = tt_Publisher_publish(g_pub, (struct tt_Data*)&msg);
    if (ret == tt_RET_OK) {
        transmitted++;
    }
    tt_Node_schedule(node, time + (uint64_t)(interval_s * (double)tt_SECOND), ping, NULL);
}

static void stop(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        }
    }

    // real HIL link's own broadcast address (run_perf.sh's own PERF_LINK_BROADCAST) - the
    // compiled-in default (255.255.255.255) doesn't match this subnet, which breaks
    // tt_get_node_id()'s own auto-detection (matches the local address against broadcast's
    // subnet, config.h's own doc comment) - found the hard way (0 received, first real run).
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    struct tt_Publisher pub;
    ret = tt_Node_create_publisher(&node, &pub, &BenchTopic, "ping");
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
    // scenario "reliable_latency" - RELIABLE + HISTORY depth=8, matched exactly across every
    // framework (COMPARISON.MD's own design principle 3 / QoS value matrix).
    // entries[]/capacity are this file's own backing array now, not an embedded
    // tt_MAX_RELIABLE_HISTORY-sized one (struct tt_ReliableCache's own doc comment, tickle.h).
    // B1 (rmw_tickle/PLAN.md) - index slots plus a byte arena sized for this scenario's own
    // fixed sizeof(struct BenchData)-byte sample, instead of a 1472-byte buffer per slot.
    static struct tt_ReliableCacheIndex pub_cache_index[8];
    static uint8_t
        pub_cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(8, tt_RELIABLE_RECORD_BYTES(sizeof(struct BenchData)))];
    static struct tt_ReliableCache pub_cache = {0};
    pub_cache.index = pub_cache_index;
    pub_cache.capacity = 8;
    pub_cache.depth = 8;
    pub_cache.arena = pub_cache_arena;
    pub_cache.arena_size = (uint32_t)sizeof(pub_cache_arena);
    pub.reliable_cache = &pub_cache;
    pub.reliable = true;
    g_pub = &pub;

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "pong", (tt_SUBSCRIBER_CALLBACK)pong_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }
    sub.reliable = true;
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
    static uint64_t reorder[BENCH_REORDER_SLOTS * BENCH_REORDER_SLOT_BYTES / sizeof(uint64_t)];
    sub.reorder_storage = reorder;
    sub.reorder_slots = BENCH_REORDER_SLOTS;
    sub.reorder_slot_bytes = BENCH_REORDER_SLOT_BYTES;

    uint64_t start = tt_get_ns();
    tt_Node_schedule(&node, start + (uint64_t)(discovery_margin_s * (double)tt_SECOND), ping, NULL);
    tt_Node_schedule(&node, start + (uint64_t)((discovery_margin_s + duration_s) * (double)tt_SECOND), stop, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    uint64_t lost = transmitted - received;
    double loss_pct = transmitted > 0 ? (100.0 * (double)lost / (double)transmitted) : 0.0;
    double avg = received > 0 ? rtt_sum_ms / (double)received : 0.0;

    printf("\n--- tickle reliable_latency statistics ---\n");
    printf("%lu sent, %lu received, %.0f%% loss\n", (unsigned long)transmitted, (unsigned long)received, loss_pct);
    if (received > 0) {
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", rtt_min_ms, avg, rtt_max_ms);
    }
    printf("RESULT: framework=tickle scenario=reliable_latency sent=%lu recv=%lu loss_pct=%.0f "
           "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
           (unsigned long)transmitted, (unsigned long)received, loss_pct, rtt_min_ms, avg, rtt_max_ms);

    tt_Node_destroy(&node);
    return 0;
}
