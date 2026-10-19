/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/COMPARISON.md)
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

#include "Bench.h"
#include "BenchCreate.h"
#include "BenchFreshRt.h"
#include "BenchFreshness.h"

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
// KEEP_LAST freshness (BenchFreshness.h): --fresh stamps each write with its burst's due time, --burst B writes B
// samples back to back per scheduled slot (default 1: the schedule as it always was).
static struct bench_fresh_sender g_fresh;
static char g_fresh_fields[BENCH_FRESH_FIELDS_MAX];
// Freshness by round trip (BenchFreshRt.h): --fresh-rt-listen PORT receives the reader's echoes on this side's clock.
static struct bench_rt_sender g_rt;
static char g_rt_fields[BENCH_RT_FIELDS_MAX];

static void send_one(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted || seq >= count) {
        g_sending_done = true;
        return;
    }
    // The slot's due time is `time`, when the schedule said to run this callback; every write of the burst has it.
    int64_t due_wall = g_fresh.on ? bench_fresh_mono_to_wall(time) : 0;
    for (uint32_t k = 0; k < g_fresh.burst && seq < count; k++) {
        struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
        bench_fresh_stamp(&g_fresh, msg.payload, sizeof msg.payload, due_wall, time);
        tt_ret_t ret = tt_Publisher_publish(g_pub, (struct tt_Data*)&msg);
        if (ret == tt_RET_OK) {
            sent++;
        }
    }
    tt_Context_schedule(node, time + (uint64_t)(interval_s * (double)tt_SECOND), send_one, NULL);
}

static void stop_draining(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

int main(int argc, char** argv) {
    bench_fresh_sender_init(&g_fresh);
    g_fresh.due_schedule = true;
    for (int i = 1; i < argc; i++) {
        if (bench_fresh_sender_parse_arg(&g_fresh, argc, argv, &i) ||
            bench_rt_sender_parse_arg(&g_rt, &g_fresh, argc, argv, &i)) {
            continue;
        }
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = (uint32_t)atoi(argv[++i]);
        }
    }

    // real HIL link's own broadcast address - see best_effort_latency/client.c's own doc comment
    // for the real bug this avoids.
    _tt_CONFIG.broadcast = "192.168.10.255";

    bench_rt_sender_start(&g_rt);

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
    // scenario "history_depth_burst_loss" - RELIABLE + HISTORY depth=8, matched exactly across
    // every framework (COMPARISON.md's own design principle 3 / QoS value matrix).
    // VOLATILE, no DEADLINE, LIFESPAN or LIVELINESS lease. The retained samples are sized by core from the profile;
    // the history bytes are never below the arena this file used to declare, (8 + 1) records of this shape.
    const struct tt_QosProfile pub_qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_LAST,
                    .depth = 8,
                    .bytes = BenchCreate_history_bytes(
                        8, tt_RELIABLE_CACHE_ARENA_BYTES(8, tt_RELIABLE_RECORD_BYTES(sizeof(struct BenchData))))},
        .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
    ret = BenchCreate_publisher(&node, &pub, "stream", &pub_qos, NULL);
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
    g_pub = &pub;

    uint64_t send_start = tt_get_ns() + (uint64_t)(discovery_margin_s * (double)tt_SECOND);
    tt_Context_schedule(&node, send_start, send_one, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && !g_sending_done && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Context_poll(&node, -1);
    }
    // drain period - no blocking "wait for acks" API exists (this file's own doc comment above),
    // just keep polling so the deliberately-late server.c has a chance to catch up (and any
    // in-flight retransmit can still land) before this side tears down.
    tt_Context_schedule(&node, tt_get_ns() + (uint64_t)(drain_s * (double)tt_SECOND), stop_draining, NULL);
    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Context_poll(&node, -1);
    }

    bench_rt_sender_finish(&g_rt);
    printf("RESULT: framework=tickle scenario=history_depth_burst_loss role=client sent=%lu%s%s\n", (unsigned long)sent,
           bench_fresh_sender_fields(&g_fresh, g_fresh_fields, sizeof g_fresh_fields),
           bench_rt_sender_fields(&g_rt, g_rt_fields, sizeof g_rt_fields));

    tt_Context_destroy(&node);
    return 0;
}
