/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/comparison.md)
// - TickLE core native subscriber role. Deliberately stalls for `pause_s` seconds (`-p`, default
// 3.0) *before creating the Subscriber at all* - TickLE has no DDS-style "matched reader that
// simply hasn't taken anything yet" state to stall in instead (a Subscriber's own callback fires
// straight out of tt_Node_poll(), there's no separate waitset/take split to defer) - so this side
// simply doesn't exist on the network yet during the stall, closest TickLE equivalent to the DDS
// twins' own design intent (client.c keeps publishing on a fixed schedule the whole time
// regardless, so this window alone determines how many samples pile up in the ring before this
// side ever looks - some recoverable within depth=8's own retention, some genuinely evicted by
// the time this side matches).
//
// last_seq starts at 0, not the first-received sample's own seq (2026-09-20, real measurement bug
// - already found and fixed once this session on the CycloneDDS twin, see its own doc comment):
// treating the first arrival as the new baseline would silently hide exactly the leading-gap loss
// this scenario exists to observe.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#include "../common/Bench.h"

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

int main(int argc, char** argv) {
    double pause_s = 3.0;
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pause_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }

    // real HIL link's own broadcast address - see best_effort_latency/server.c's own doc comment
    // for the real bug this avoids.
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    printf("Subscriber: deliberately not joining for %.1fs, before the writer's own depth=8 ring can "
           "evict anything published during that window\n",
           pause_s);
    struct timespec pause_ts = {.tv_sec = (time_t)pause_s, .tv_nsec = (long)((pause_s - (time_t)pause_s) * 1e9)};
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

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, 500 * 1000 * 1000); // 500ms (nanoseconds), so the deadline/g_interrupted check re-runs
    }

    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;

    printf("RESULT: framework=tickle scenario=history_depth_burst_loss role=server pause_s=%.1f "
           "recv=%lu lost=%lu loss_pct=%.1f\n",
           pause_s, (unsigned long)received, (unsigned long)lost, loss_pct);

    tt_Node_destroy(&node);
    return 0;
}
