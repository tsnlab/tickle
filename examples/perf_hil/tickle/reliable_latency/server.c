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

#include "../common/Bench.h"

// Reorder-buffer geometry for this example's RELIABLE Subscriber - see where it is assigned.
//
// Sized to the widest tracking window this build allows, not to a round number. A Subscriber can
// hold at most as many samples as it can track ahead of its oldest gap, so a buffer that covers
// the widest window makes overflow impossible by construction rather than unlikely - the same
// argument rmw_tickle's RMW_TICKLE_REORDER_SLOTS rests on. This was 512 at first, which is
// narrower than the -w a reliable_throughput run is allowed to request (up to 4096), so any run
// that asked for a wide window would have overflowed into the re-request fallback and measured
// that instead of the protocol.
#define BENCH_REORDER_SLOTS tt_RELIABLE_BITMAP_MAX_BITS
// tt_REORDER_SLOT_SIZE rounds up to a multiple of 8. This was a bare sizeof(...) + 16 = 116, which
// core then addressed with a 120-byte stride - so the last slots of the array were past its end.
#define BENCH_REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(sizeof(struct BenchData) + 16)

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static struct tt_Publisher* g_pub;

static void ping_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    tt_Publisher_publish(g_pub, (struct tt_Data*)data);
}

static const double default_safety_cap_s = 40.0;

int main(int argc, char** argv) {
    double safety_cap_s = default_safety_cap_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
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
    ret = tt_Node_create_publisher(&node, &pub, &BenchTopic, "pong");
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
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
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "ping", (tt_SUBSCRIBER_CALLBACK)ping_callback);
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

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    // 500ms (nanoseconds), so the deadline/g_interrupted check re-runs.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, poll_timeout_ns);
    }

    printf("RESULT: framework=tickle scenario=reliable_latency role=server\n");

    tt_Node_destroy(&node);
    return 0;
}
