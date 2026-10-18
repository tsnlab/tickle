/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "durability_late_join" (rmw_tickle/COMPARISON.md) -
// TickLE core native subscriber role, deliberately started *after* the publisher has already sent
// its whole backlog (run_scenario's own orchestration starts this side second) - the actual thing
// under test: does DURABLE (-D, server.c's own flag) deliver that backlog anyway, while VOLATILE
// (default) delivers none of it. See server.c's own doc comment for the full mechanism. `-D` here
// is accepted only for the RESULT line's own bookkeeping symmetry with the DDS twins - TickLE's
// Subscriber has no independent durability requirement to set (server.c's own doc comment).
//
// backlog_delivery_ms measured the same imprecise way the CycloneDDS/FastDDS twins do (time from
// this process's own start to when the fixed collection window ends, not to the actual moment of
// arrival) - kept consistent for direct comparability rather than independently made tighter.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "Bench.h"
#include "BenchCreate.h"

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
// The slot stride is core's since the QoS redesign: tt_Subscriber_storage_size() sizes it from BenchTopic's
// max_encoded_size, a datagram's payload at most (docs/DESIGN.md section 9).

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static const double default_collect_s = 15.0;
static const double ns_per_ms = 1e6;

static uint32_t received = 0;
static struct tt_Publisher* g_ack_pub;
static bool g_collecting = true;

static void data_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    (void)data;
    if (g_collecting) {
        received++;
    }
}

static void send_ack(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_collecting = false;
    struct BenchData ack = {.seq = 1, .send_ns = tt_get_ns()};
    tt_Publisher_publish(g_ack_pub, (struct tt_Data*)&ack);
}

static void stop(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

int main(int argc, char** argv) {
    bool durable = false;
    double collect_s = default_collect_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0) {
            durable = true;
        }
    }

    // real HIL link's own broadcast address - see best_effort_latency/client.c's own doc comment
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

    uint64_t start = tt_get_ns();

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
    const struct tt_QosProfile sub_qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_ALL},
        .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .tracking_words = 0, .reorder_slots = BENCH_REORDER_SLOTS},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
    ret = BenchCreate_subscriber(&node, &sub, "ping", &sub_qos, (tt_SUBSCRIBER_CALLBACK)data_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    struct tt_Publisher ack_pub;
    // BEST_EFFORT and VOLATILE, no DEADLINE, LIFESPAN or LIVELINESS lease: what the deprecated create left. Nothing is
    // retained, so the history depth is inert; KEEP_LAST 1 says so.
    static const struct tt_QosProfile ack_pub_qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 1},
        .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
    ret = BenchCreate_publisher(&node, &ack_pub, "pong", &ack_pub_qos, NULL);
    if (ret != 0) {
        printf("Cannot create ack publisher: %d\n", ret);
        return ret;
    }
    g_ack_pub = &ack_pub;

    tt_Context_schedule(&node, start + (uint64_t)(collect_s * (double)tt_SECOND), send_ack, NULL);
    tt_Context_schedule(&node, start + (uint64_t)((collect_s + 1.0) * (double)tt_SECOND), stop, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Context_poll(&node, -1);
    }

    double backlog_delivery_ms = received > 0 ? (double)(tt_get_ns() - start) / ns_per_ms : -1.0;

    printf("RESULT: framework=tickle scenario=durability_late_join role=client durable=%d "
           "received=%u backlog_delivery_ms=%.3f\n",
           durable, received, backlog_delivery_ms);

    tt_Context_destroy(&node);
    return 0;
}
