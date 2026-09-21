/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/comparison.md) -
// TickLE core native client/sender role (no rmw). Same one-way max-rate stream as
// best_effort_throughput's own client.c, plus RELIABLE + a caller-configurable HISTORY depth
// (rmw_tickle/PLAN.md's own "DDS semantic-parity backlog" row 2 - struct tt_ReliableCache's
// entries[]/capacity are caller-owned now, tickle.h, no longer a single build-wide
// tt_MAX_RELIABLE_HISTORY=64 ceiling every Publisher was capped by alike) - defaults to that same
// 64 for continuity with earlier measurements, -K raises it up to MAX_RELIABLE_DEPTH below without
// a rebuild, for directly re-measuring whether a deeper Publisher-side cache actually improves
// RELIABLE's own tc-loss recovery at TickLE's own real max throughput, or - per struct tt_
// ReliableCache's own doc comment's honest answer - the Subscriber-side 64-bit received_bitmap
// window is the real bottleneck regardless of how deep this side's own cache reaches back. No
// blocking "wait for all acks" API exists in tickle.h (only tt_Publisher_request_ack(), which
// solicits but doesn't block) - a fixed drain period after the send loop substitutes for that,
// polling so any in-flight retransmits can still land before teardown.

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

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static const double default_duration_s = 10.0;
static const double default_drain_s = 3.0;
static const double discovery_margin_s = 2.0;
static const double bits_per_byte = 8.0;
static const double bits_per_megabit = 1e6;

// Compile-time backing-array ceiling for this scenario's own -K flag below - deliberately far
// past the old tt_MAX_RELIABLE_HISTORY=64 default, since re-measuring at a genuinely deeper depth
// is this flag's entire purpose (struct tt_ReliableCache's own doc comment, tickle.h).
#define MAX_RELIABLE_DEPTH 8192

static double interval_s = 0.0; // 0 = as fast as possible, matching best_effort_throughput's own default
static double duration_s = default_duration_s;
static double drain_s = default_drain_s;
static uint32_t reliable_depth = tt_MAX_RELIABLE_HISTORY; // -K overrides; 0 stays the historical default
static uint64_t sent = 0;
static uint32_t seq = 0;
static struct tt_Publisher* g_pub;
static uint64_t g_deadline_ns;
static bool g_sending_done = false;

static void send_one(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted || tt_get_ns() >= g_deadline_ns) {
        g_sending_done = true;
        return;
    }
    struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
    tt_ret_t ret = tt_Publisher_publish(g_pub, (struct tt_Data*)&msg);
    if (ret == tt_RET_OK) {
        sent++;
    }
    uint64_t next = interval_s > 0.0 ? time + (uint64_t)(interval_s * (double)tt_SECOND) : time;
    tt_Node_schedule(node, next, send_one, NULL);
}

static void stop_draining(struct tt_Node* node, uint64_t time, void* param) {
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
        } else if (strcmp(argv[i], "-K") == 0 && i + 1 < argc) {
            reliable_depth = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
    }
    if (reliable_depth == 0 || reliable_depth > MAX_RELIABLE_DEPTH) {
        printf("Requested reliable cache depth %u out of range (1..%u); clamping to %u.\n", reliable_depth,
               MAX_RELIABLE_DEPTH, (unsigned)tt_MAX_RELIABLE_HISTORY);
        reliable_depth = tt_MAX_RELIABLE_HISTORY;
    }

    // real HIL link's own broadcast address - see best_effort_latency/client.c's own doc comment
    // for the real bug this avoids.
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
    ret = tt_Node_create_publisher(&node, &pub, &BenchTopic, "stream");
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
    // scenario "reliable_throughput" - RELIABLE + a caller-chosen HISTORY depth (-K, default 64 -
    // see this file's own doc comment above). entries[]/capacity are this file's own backing
    // array now, not an embedded tt_MAX_RELIABLE_HISTORY-sized one (struct tt_ReliableCache's own
    // doc comment, tickle.h) - sized to MAX_RELIABLE_DEPTH so -K can actually reach past 64.
    static struct tt_ReliableCacheEntry pub_cache_entries[MAX_RELIABLE_DEPTH];
    static struct tt_ReliableCache pub_cache = {0};
    pub_cache.entries = pub_cache_entries;
    pub_cache.capacity = (uint16_t)reliable_depth;
    pub_cache.depth = (uint16_t)reliable_depth;
    pub.reliable_cache = &pub_cache;
    pub.reliable = true;
    g_pub = &pub;

    uint64_t send_start = tt_get_ns() + (uint64_t)(discovery_margin_s * (double)tt_SECOND);
    g_deadline_ns = send_start + (uint64_t)(duration_s * (double)tt_SECOND);
    tt_Node_schedule(&node, send_start, send_one, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && !g_sending_done && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }
    // drain period - no blocking "wait for acks" API exists (this file's own doc comment above),
    // just keep polling so any in-flight retransmit can still land before teardown.
    tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(drain_s * (double)tt_SECOND), stop_draining, NULL);
    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    double mbps = duration_s > 0.0
                      ? ((double)sent * sizeof(struct BenchData) * bits_per_byte) / bits_per_megabit / duration_s
                      : 0.0;
    printf("RESULT: framework=tickle scenario=reliable_throughput role=client sent=%lu elapsed_s=%.3f "
           "send_mbps=%.3f reliable_depth=%u\n",
           (unsigned long)sent, duration_s, mbps, reliable_depth);

    tt_Node_destroy(&node);
    return 0;
}
