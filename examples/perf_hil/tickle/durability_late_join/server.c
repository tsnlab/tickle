/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "durability_late_join" (rmw_tickle/COMPARISON.MD) -
// TickLE core native publisher role. Same role split as the CycloneDDS/FastDDS twins' own
// server.c (writer+reader on both ends, "ping" for data / "pong" for the late subscriber's own
// ack) - publishes `backlog_count` (20, matching the DDS twins exactly) samples on "ping" *before*
// any subscriber exists, the actual thing under test, then waits for client.c's own ack on "pong"
// to know when it's actually done. No explicit match-wait before that first publish burst -
// there's genuinely no subscriber yet at that point.
//
// TickLE's own DURABLE is simpler than DDS's TRANSIENT_LOCAL + separate durability_service depth:
// one `reliable_cache` backs both RELIABILITY and DURABILITY (tickle.h's own doc comment), so
// there's no second, independently-configurable depth to get wrong the way the CycloneDDS twin's
// own doc comment describes (`dds_qset_durability_service`'s own history depth defaulting to 1,
// a real bug found there) - depth here is `tt_MAX_RELIABLE_HISTORY` (64), comfortably above
// `backlog_count`, matching reliable_throughput's own "as deep as this framework allows" choice.
// Likewise no RxO durability-incompatibility concept exists to accidentally trip (the real bug
// that made `-D` forwarding matter on the CycloneDDS twin) - TickLE's subscriber has no
// independent durability requirement to mismatch against, so `-D` only ever needs to reach the
// Publisher here.

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

static bool g_acked = false;

static void ack_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    (void)data;
    g_acked = true;
}

static void stop(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

static const uint32_t default_backlog_count = 20;
static const double default_wait_s = 40.0;

int main(int argc, char** argv) {
    bool durable = false;
    uint32_t backlog_count = default_backlog_count;
    double wait_s = default_wait_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0) {
            durable = true;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            wait_s = atof(argv[++i]);
        }
    }

    // real HIL link's own broadcast address - see best_effort_latency/server.c's own doc comment
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
    ret = tt_Node_create_publisher(&node, &pub, &BenchTopic, "ping");
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
    // entries[]/capacity are this file's own backing array now, not an embedded
    // tt_MAX_RELIABLE_HISTORY-sized one (struct tt_ReliableCache's own doc comment, tickle.h).
    // B1 (rmw_tickle/PLAN.md) - index slots plus a byte arena sized for this scenario's own
    // fixed sizeof(struct BenchData)-byte sample, instead of a 1472-byte buffer per slot.
    static struct tt_ReliableCacheIndex pub_cache_index[tt_MAX_RELIABLE_HISTORY];
    static uint8_t pub_cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(tt_MAX_RELIABLE_HISTORY,
                                                                 tt_RELIABLE_RECORD_BYTES(sizeof(struct BenchData)))];
    static struct tt_ReliableCache pub_cache = {0};
    pub_cache.index = pub_cache_index;
    pub_cache.capacity = tt_MAX_RELIABLE_HISTORY;
    pub_cache.depth = tt_MAX_RELIABLE_HISTORY;
    pub_cache.arena = pub_cache_arena;
    pub_cache.arena_size = (uint32_t)sizeof(pub_cache_arena);
    pub.reliable_cache = &pub_cache;
    pub.reliable = true;
    pub.durable = durable;

    struct tt_Subscriber ack_sub;
    ret = tt_Node_create_subscriber(&node, &ack_sub, &BenchTopic, "pong", (tt_SUBSCRIBER_CALLBACK)ack_callback);
    if (ret != 0) {
        printf("Cannot create ack subscriber: %d\n", ret);
        return ret;
    }

    printf("Publisher: durable=%d, publishing %u backlog samples before any subscriber exists\n", durable,
           backlog_count);
    for (uint32_t i = 1; i <= backlog_count; i++) {
        struct BenchData msg = {.seq = i, .send_ns = tt_get_ns()};
        tt_ret_t wrc = tt_Publisher_publish(&pub, (struct tt_Data*)&msg);
        if (wrc != tt_RET_OK) {
            fprintf(stderr, "tt_Publisher_publish(seq=%u) failed: %d\n", i, wrc);
        }
    }

    printf("Waiting up to %.0fs for the late subscriber's own ack...\n", wait_s);
    tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(wait_s * (double)tt_SECOND), stop, NULL);

    // 500ms (nanoseconds), so the g_acked check re-runs.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && !g_acked && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, poll_timeout_ns);
    }

    printf("RESULT: framework=tickle scenario=durability_late_join role=server durable=%d "
           "backlog_sent=%u acked=%d\n",
           durable, backlog_count, g_acked);

    tt_Node_destroy(&node);
    return 0;
}
