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
// TickLE core native server/pong role (no rmw). Subscribes "ping", republishes the exact same
// sample on "pong" immediately - never logs per-request (matches the FastDDS/CycloneDDS server's
// own identical precedent).

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

static struct tt_Publisher* g_pub;

static void ping_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    tt_Publisher_publish(g_pub, (struct tt_Data*)data);
}

int main(int argc, char** argv) {
    double safety_cap_s = 40.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
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
    ret = tt_Node_create_publisher(&node, &pub, &BenchTopic, "pong");
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
    static struct tt_ReliableCache pub_cache = {0};
    pub_cache.depth = 8;
    pub.reliable_cache = &pub_cache;
    pub.reliable = true;
    g_pub = &pub;

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "ping", (tt_SUBSCRIBER_CALLBACK)ping_callback);
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

    printf("RESULT: framework=tickle scenario=reliable_latency role=server\n");

    tt_Node_destroy(&node);
    return 0;
}
