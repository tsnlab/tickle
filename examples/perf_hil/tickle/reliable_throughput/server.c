/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/COMPARISON.MD) -
// TickLE core native server/receiver role (no rmw). Same one-way stream shape as
// best_effort_throughput/server.c (this side is the authoritative one for loss/throughput),
// `sub.reliable = true` to match the client's own RELIABLE Publisher - no cache is needed on the
// Subscriber side (tickle.h: retransmission state lives on the Publisher's own reliable_cache).

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

// Order-independent receive tracking (2026-09-22, at the user's own direction, following the
// real ACKNACK-flood fix in tickle.c). RELIABLE only adds a delivery guarantee via
// retransmission, not an ordering one (deliver_data_to_subscriber()'s own doc comment, tickle.c;
// struct tt_WriterProxy's own doc comment makes the identical point) - a successfully recovered
// retransmit can legitimately arrive *after* later, in-order samples, once one real ACKNACK round
// trip (a handful of ms) has elapsed. The strict-order counting this file used before
// (`data->seq <= last_seq -> ignore`) could never credit that: by the time any retransmit could
// possibly land, `last_seq` had already advanced past it from later arrivals, so the gap was
// already counted "lost" the moment it was first noticed, and the retransmit's own actual arrival
// changed nothing - a genuine measurement bug, not a core bug, found investigating why loss_pct
// stayed far above the injected tc rate even after the flood fix made recovery attempts routine
// (PLAN.md's real-HIL record for this scenario, 2026-09-22).
//
// Fixed with a distinct-sequence-number bitmap instead: any sample that ever arrives, in any
// order, is counted as received exactly once (first arrival only - a true duplicate re-delivery,
// same doc-commented core residual as before, is silently ignored the same way). Genuine loss is
// computed once at the end as "how many sequence numbers up to the highest one ever seen were
// never received at all" - this correctly credits an out-of-order recovery regardless of when it
// arrives, rather than penalizing it twice (once for the transient gap, once more by discarding
// the recovery itself).
#define MAX_TRACKED_SEQ 20000000u // ~100s worth of headroom at this scenario's own ~200K msg/s max rate
static uint8_t received_bitmap[(MAX_TRACKED_SEQ / 8) + 1];
static uint64_t received = 0;
static uint32_t max_seq_seen = 0;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    if (data->seq == 0 || data->seq > MAX_TRACKED_SEQ) {
        return; // out of this scenario's own realistic tracked range - never expected in practice
    }
    uint32_t idx = data->seq - 1;
    if (received_bitmap[idx / 8] & (1U << (idx % 8))) {
        return; // genuine duplicate re-delivery (already counted on first arrival)
    }
    received_bitmap[idx / 8] |= (uint8_t)(1U << (idx % 8));
    received++;
    if (data->seq > max_seq_seen) {
        max_seq_seen = data->seq;
    }
}

static const double default_safety_cap_s = 40.0;
static const double safety_cap_buffer_s = 15.0;

int main(int argc, char** argv) {
    double safety_cap_s = default_safety_cap_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }
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

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "stream", (tt_SUBSCRIBER_CALLBACK)stream_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }
    sub.reliable = true;

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    // 500ms (nanoseconds), so the deadline/g_interrupted check re-runs.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, poll_timeout_ns);
    }

    uint64_t lost = max_seq_seen > received ? (uint64_t)max_seq_seen - received : 0;
    double loss_pct = max_seq_seen > 0 ? (100.0 * (double)lost / (double)max_seq_seen) : 0.0;

    printf("RESULT: framework=tickle scenario=reliable_throughput role=server recv=%lu lost=%lu loss_pct=%.1f\n",
           (unsigned long)received, (unsigned long)lost, loss_pct);

    tt_Node_destroy(&node);
    return 0;
}
