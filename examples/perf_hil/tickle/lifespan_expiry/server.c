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
// TickLE core native subscriber role. Same "deliberately doesn't join for `-p` seconds, before
// creating the Subscriber at all" design as history_depth_burst_loss/server.c's own doc comment
// explains (TickLE has no DDS-style "matched but hasn't taken yet" state to stall in instead).
// client.c's own generous depth=64 keeps this scenario's own loss attributable purely to
// LIFESPAN's age-based expiry, not queue-depth eviction (its own doc comment).

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static uint64_t received = 0;
static uint64_t lost = 0;
static uint32_t last_seq = 0;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    if (data->seq > last_seq + 1) {
        lost += (data->seq - last_seq - 1);
    }
    last_seq = data->seq;
    received++;
}

static const double default_pause_s = 0.3;
static const double default_safety_cap_s = 30.0;
static const double safety_cap_buffer_s = 15.0;
static const double ns_per_s = 1e9;

int main(int argc, char** argv) {
    double pause_s = default_pause_s;
    double safety_cap_s = default_safety_cap_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pause_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
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

    printf("Subscriber: deliberately not joining for %.2fs\n", pause_s);
    struct timespec pause_ts = {
        .tv_sec = (time_t)pause_s,
        .tv_nsec = (long)((pause_s - (double)(time_t)pause_s) * ns_per_s),
    };
    nanosleep(&pause_ts, NULL);

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

    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;

    printf("RESULT: framework=tickle scenario=lifespan_expiry role=server pause_s=%.2f recv=%lu lost=%lu "
           "loss_pct=%.1f\n",
           pause_s, (unsigned long)received, (unsigned long)lost, loss_pct);

    tt_Node_destroy(&node);
    return 0;
}
