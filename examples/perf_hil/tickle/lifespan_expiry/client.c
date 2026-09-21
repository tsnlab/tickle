/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "lifespan_expiry" (rmw_tickle/COMPARISON.MD) -
// TickLE core native publisher role. Writes a fixed COUNT (`-n`, default 100) at a fixed, paced
// interval (`-i`, default 0.02s = 50/s, matching the CycloneDDS/FastDDS twins exactly) with
// LIFESPAN (`-T`, default 100ms, the actual variable under test) and a *generous*
// `depth=tt_MAX_RELIABLE_HISTORY` (64, matching reliable_throughput's own "as deep as this
// framework allows" choice) - deliberately not history_depth_burst_loss's own shallow depth=8, so
// any loss observed here is attributable purely to LIFESPAN's own per-sample age-based expiry, not
// queue-depth eviction (the two mechanisms are kept cleanly separated across the two scenarios,
// same design principle the DDS twins use). `reliable_cache_entry_expired()` (tickle.c) is checked
// both by the durability-push path and by `process_acknack()`'s own retransmit path (read
// directly, verified) - so a late-joining RELIABLE Subscriber's own ACKNACK-driven backfill
// correctly skips anything past its lifespan, "as if it had never been sent"
// (`tt_Publisher.lifespan_duration_ns`'s own doc comment).

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

static const double default_interval_s = 0.02;
static const double default_lifespan_s = 0.1;
static const double default_drain_s = 3.0;
static const uint32_t default_count = 100;
static const double near_zero_discovery_margin_s = 0.05;

static double interval_s = default_interval_s;
static double lifespan_s = default_lifespan_s;
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
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            lifespan_s = atof(argv[++i]);
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
    // entries[]/capacity are this file's own backing array now, not an embedded
    // tt_MAX_RELIABLE_HISTORY-sized one (struct tt_ReliableCache's own doc comment, tickle.h).
    static struct tt_ReliableCacheEntry pub_cache_entries[tt_MAX_RELIABLE_HISTORY];
    static struct tt_ReliableCache pub_cache = {0};
    pub_cache.entries = pub_cache_entries;
    pub_cache.capacity = tt_MAX_RELIABLE_HISTORY;
    pub_cache.depth = tt_MAX_RELIABLE_HISTORY;
    pub.reliable_cache = &pub_cache;
    pub.reliable = true;
    pub.lifespan_duration_ns = (uint64_t)(lifespan_s * (double)tt_SECOND);
    g_pub = &pub;

    // A near-zero (not zero) discovery margin - see run_scenario.sh's own PRE_CLIENT_SLEEP=0 note
    // for this scenario: this side needs to start well before the server's own `-p` stall
    // completes for lifespan expiry to have anything to bite on, but `lifespan_s` itself (100ms
    // default) is small enough that even this scenario's own established 2.0s margin (used
    // everywhere else) would swamp it.
    tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(near_zero_discovery_margin_s * (double)tt_SECOND), send_one, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && !g_sending_done && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }
    tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(drain_s * (double)tt_SECOND), stop_draining, NULL);
    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    printf("RESULT: framework=tickle scenario=lifespan_expiry role=client sent=%lu\n", (unsigned long)sent);

    tt_Node_destroy(&node);
    return 0;
}
