/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/COMPARISON.MD)
// - TickLE core native publisher role. Writes a fixed COUNT (`-n`, default 160) at a fixed,
// paced interval (`-i`, default 0.05s = 20/s - deliberately paced, not scenario 3/4's own
// unbounded "as fast as possible": at TickLE's own max-rate ceiling, depth=8 would be
// overwhelmed by ordinary send-side throughput alone, confounding it with the deliberate
// stalled-reader eviction this scenario means to isolate - see scenario 4's own results for what
// that confound looks like). RELIABLE + HISTORY depth=8, matched exactly against server.c's own
// Subscriber-side expectation and the CycloneDDS/FastDDS twins' own depth. No blocking
// "wait for all acks" API exists (reliable_throughput/client.c's own doc comment) - a fixed drain
// period after the send loop substitutes, giving the deliberately-late server.c a chance to catch
// up before this side tears down (mirrors dds_wait_for_acks()'s own role there).

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

static const double default_interval_s = 0.05;
static const double default_drain_s = 5.0;
static const uint32_t default_count = 160;
static const double discovery_margin_s = 2.0;

static double interval_s = default_interval_s;
static double drain_s = default_drain_s;
static uint32_t count = default_count;
static uint64_t sent = 0;
static uint32_t seq = 0;
static struct tt_Publisher* g_pub;
static bool g_sending_done = false;

static void send_one(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted || seq >= count) {
        g_sending_done = true;
        return;
    }
    struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
    tt_ret_t ret = tt_Publisher_publish(g_pub, (struct tt_Data*)&msg);
    if (ret == tt_RET_OK) {
        sent++;
    }
    tt_Node_schedule(node, time + (uint64_t)(interval_s * (double)tt_SECOND), send_one, NULL);
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
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = (uint32_t)atoi(argv[++i]);
        }
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
    // scenario "history_depth_burst_loss" - RELIABLE + HISTORY depth=8, matched exactly across
    // every framework (COMPARISON.MD's own design principle 3 / QoS value matrix).
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

    uint64_t send_start = tt_get_ns() + (uint64_t)(discovery_margin_s * (double)tt_SECOND);
    tt_Node_schedule(&node, send_start, send_one, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && !g_sending_done && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }
    // drain period - no blocking "wait for acks" API exists (this file's own doc comment above),
    // just keep polling so the deliberately-late server.c has a chance to catch up (and any
    // in-flight retransmit can still land) before this side tears down.
    tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(drain_s * (double)tt_SECOND), stop_draining, NULL);
    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    printf("RESULT: framework=tickle scenario=history_depth_burst_loss role=client sent=%lu\n", (unsigned long)sent);

    tt_Node_destroy(&node);
    return 0;
}
