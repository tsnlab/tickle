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

static void send_ack(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_collecting = false;
    struct BenchData ack = {.seq = 1, .send_ns = tt_get_ns()};
    tt_Publisher_publish(g_ack_pub, (struct tt_Data*)&ack);
}

static void stop(struct tt_Node* node, uint64_t time, void* param) {
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

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    uint64_t start = tt_get_ns();

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "ping", (tt_SUBSCRIBER_CALLBACK)data_callback);
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

    struct tt_Publisher ack_pub;
    ret = tt_Node_create_publisher(&node, &ack_pub, &BenchTopic, "pong");
    if (ret != 0) {
        printf("Cannot create ack publisher: %d\n", ret);
        return ret;
    }
    g_ack_pub = &ack_pub;

    tt_Node_schedule(&node, start + (uint64_t)(collect_s * (double)tt_SECOND), send_ack, NULL);
    tt_Node_schedule(&node, start + (uint64_t)((collect_s + 1.0) * (double)tt_SECOND), stop, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    double backlog_delivery_ms = received > 0 ? (double)(tt_get_ns() - start) / ns_per_ms : -1.0;

    printf("RESULT: framework=tickle scenario=durability_late_join role=client durable=%d "
           "received=%u backlog_delivery_ms=%.3f\n",
           durable, received, backlog_delivery_ms);

    tt_Node_destroy(&node);
    return 0;
}
