/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "deadline_miss_detection" (rmw_tickle/comparison.md) -
// TickLE core native publisher role. `pub.deadline_duration_ns` is purely a wire/RxO-announced
// field (Milestone 49) - TickLE core itself never enforces or checks it (tickle.h's own doc
// comment, corrected in comparison.md's own native-QoS table this same pass) - so unlike the
// CycloneDDS/FastDDS twins, there is no `dds_lset_offered_deadline_missed()`-style listener to
// lean on; this file implements the check itself, the way tickle.h's own doc comment says any
// consumer must.
//
// Design: a periodic checker (`check_deadline`), scheduled on its own *fixed* cadence independent
// of the publish schedule (mirrors a real DDS implementation's own internal timer, not tied to
// individual write() calls) - each tick compares `now - last_publish_ns` against the deadline and
// counts a miss if exceeded. This makes detection latency bounded by the checker's own tick
// granularity (== the deadline itself here) - coarser than the CycloneDDS/FastDDS twins' own
// near-instant listener callback (~0.05ms, comparison.md), a real and expected architectural
// difference: TickLE has no internal listener thread to lean on, so a polling checker is the
// natural implementation, and its own granularity directly bounds how fast a miss can be noticed.
// Publishes normally at the deadline's own period, then deliberately skips one interval by 3x the
// deadline (matching the DDS twins' own "one intentionally-missed interval" design) before
// resuming.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static double deadline_s = 0.05;
static double duration_s = 10.0;
static uint32_t skip_at_seq = 30; // ~1.5s in at the default 50ms interval
static uint64_t sent = 0;
static uint32_t seq = 0;
static uint32_t misses = 0;
static struct tt_Publisher* g_pub;
static uint64_t last_publish_ns;
static uint64_t g_deadline_ns;

static void send_one(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted || tt_get_ns() >= g_deadline_ns) {
        return;
    }
    struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
    tt_ret_t ret = tt_Publisher_publish(g_pub, (struct tt_Data*)&msg);
    if (ret == tt_RET_OK) {
        sent++;
        last_publish_ns = tt_get_ns();
    }
    uint64_t next_interval_ns =
        (seq == skip_at_seq) ? (uint64_t)(3.0 * deadline_s * (double)tt_SECOND) : (uint64_t)(deadline_s * (double)tt_SECOND);
    tt_Node_schedule(node, time + next_interval_ns, send_one, NULL);
}

static void check_deadline(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted) {
        return;
    }
    if (last_publish_ns != 0 && time - last_publish_ns > (uint64_t)(deadline_s * (double)tt_SECOND)) {
        misses++;
    }
    if (time < g_deadline_ns) {
        tt_Node_schedule(node, time + (uint64_t)(deadline_s * (double)tt_SECOND), check_deadline, NULL);
    }
}

static void stop(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            deadline_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        }
    }

    // real HIL link's own broadcast address - see best_effort_latency/client.c's own doc comment
    // for the real bug this avoids.
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

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
    pub.deadline_duration_ns = (uint64_t)(deadline_s * (double)tt_SECOND);
    g_pub = &pub;

    uint64_t start = tt_get_ns();
    uint64_t send_start = start + (uint64_t)(2.0 * (double)tt_SECOND); // discovery margin
    g_deadline_ns = send_start + (uint64_t)(duration_s * (double)tt_SECOND);
    tt_Node_schedule(&node, send_start, send_one, NULL);
    tt_Node_schedule(&node, send_start + (uint64_t)(deadline_s * (double)tt_SECOND), check_deadline, NULL);
    tt_Node_schedule(&node, g_deadline_ns + (uint64_t)(1.0 * (double)tt_SECOND), stop, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    printf("RESULT: framework=tickle scenario=deadline_miss_detection role=client sent=%lu writer_misses=%u\n",
           (unsigned long)sent, misses);

    tt_Node_destroy(&node);
    return 0;
}
