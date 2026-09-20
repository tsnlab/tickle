/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "reliable_latency" (rmw_tickle/comparison.md) -
// TickLE core native client/ping role (no rmw). Mirrors examples/perf_hil/{cyclonedds,fastdds}/
// reliable_latency/client.c's own logic exactly - single-clock RTT (this side's own
// tt_get_ns(), both send and receipt), same RESULT line shape, for direct comparison.

#include <math.h>
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

static double interval_s = 1.0;
static double duration_s = 10.0;
static uint64_t transmitted = 0, received = 0;
static double rtt_min_ms = -1.0, rtt_max_ms = 0.0, rtt_sum_ms = 0.0;
static uint32_t seq = 0;
static struct tt_Publisher* g_pub;

static void pong_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    double rtt_ms = (double)(tt_get_ns() - data->send_ns) / 1e6;
    received++;
    if (rtt_min_ms < 0.0 || rtt_ms < rtt_min_ms) {
        rtt_min_ms = rtt_ms;
    }
    if (rtt_ms > rtt_max_ms) {
        rtt_max_ms = rtt_ms;
    }
    rtt_sum_ms += rtt_ms;
}

static void ping(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted) {
        return;
    }
    struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
    tt_ret_t ret = tt_Publisher_publish(g_pub, (struct tt_Data*)&msg);
    if (ret == tt_RET_OK) {
        transmitted++;
    }
    tt_Node_schedule(node, time + (uint64_t)(interval_s * (double)tt_SECOND), ping, NULL);
}

static void stop(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        }
    }

    // real HIL link's own broadcast address (run_perf.sh's own PERF_LINK_BROADCAST) - the
    // compiled-in default (255.255.255.255) doesn't match this subnet, which breaks
    // tt_get_node_id()'s own auto-detection (matches the local address against broadcast's
    // subnet, config.h's own doc comment) - found the hard way (0 received, first real run).
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
    ret = tt_Node_create_publisher(&node, &pub, &BenchTopic, "ping");
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
    // scenario "reliable_latency" - RELIABLE + HISTORY depth=8, matched exactly across every
    // framework (comparison.md's own design principle 3 / QoS value matrix).
    static struct tt_ReliableCache pub_cache = {0};
    pub_cache.depth = 8;
    pub.reliable_cache = &pub_cache;
    pub.reliable = true;
    g_pub = &pub;

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "pong", (tt_SUBSCRIBER_CALLBACK)pong_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }
    sub.reliable = true;

    uint64_t start = tt_get_ns();
    tt_Node_schedule(&node, start + (uint64_t)(2.0 * (double)tt_SECOND), ping, NULL); // discovery margin
    tt_Node_schedule(&node, start + (uint64_t)((2.0 + duration_s) * (double)tt_SECOND), stop, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    uint64_t lost = transmitted - received;
    double loss_pct = transmitted > 0 ? (100.0 * (double)lost / (double)transmitted) : 0.0;
    double avg = received > 0 ? rtt_sum_ms / (double)received : 0.0;

    printf("\n--- tickle reliable_latency statistics ---\n");
    printf("%lu sent, %lu received, %.0f%% loss\n", (unsigned long)transmitted, (unsigned long)received, loss_pct);
    if (received > 0) {
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", rtt_min_ms, avg, rtt_max_ms);
    }
    printf("RESULT: framework=tickle scenario=reliable_latency sent=%lu recv=%lu loss_pct=%.0f "
           "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
           (unsigned long)transmitted, (unsigned long)received, loss_pct, rtt_min_ms, avg, rtt_max_ms);

    tt_Node_destroy(&node);
    return 0;
}
