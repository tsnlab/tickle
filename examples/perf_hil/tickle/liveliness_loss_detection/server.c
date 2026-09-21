/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "liveliness_loss_detection" (rmw_tickle/COMPARISON.MD)
// - TickLE core native subscriber role. Real peer-departure detection in TickLE is node-level, not
// per-Publisher-entity (COMPARISON.MD's own native-QoS table, corrected this same pass, read
// directly from tickle.h): `tt_Node_set_discovery()` + `tt_DISCOVERY_CALLBACK` fires
// `departed=true` on `check_liveliness()`'s own fixed timeout window
// (`tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL`, config.h, ~3s) - independent of
// whatever lease value `-T` announces on the wire (`sub.liveliness_lease_duration_ns` below is set
// purely for RxO-matching parity with the DDS twins' own QoS, it does not change this detection
// window at all). This is the real, expected, and worth-documenting difference from the DDS twins'
// own per-entity, lease-configurable detection: TickLE's own number here should track its fixed
// ~3s window regardless of `-T`, not the announced lease the way CycloneDDS/FastDDS's own results
// do.
//
// Detection latency measured entirely from this process's own clock (last-received-sample
// timestamp vs. departure-detected timestamp) - the same single-clock design principle the DDS
// twins' own scenario 8 established (never subtract a timestamp read on a different host's clock,
// e.g. the publisher's own `kill -9` wall-clock time).

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

static const double default_lease_s = 2.0;
static const double default_safety_cap_s = 40.0;
static const double safety_cap_buffer_s = 15.0;
static const double ns_per_ms = 1e6;

static uint64_t received = 0;
static uint64_t last_received_ns = 0;
static bool departed = false;
static uint64_t departed_detected_ns = 0;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    (void)data;
    received++;
    last_received_ns = tt_get_ns();
}

static void discovery_callback(struct tt_Node* node, uint8_t node_id, uint32_t endpoint_id, uint8_t kind,
                               bool is_departed, void* param) {
    (void)node;
    (void)node_id;
    (void)endpoint_id;
    (void)kind;
    (void)param;
    if (is_departed && !departed) {
        departed = true;
        departed_detected_ns = tt_get_ns();
    }
}

int main(int argc, char** argv) {
    double lease_s = default_lease_s;
    double safety_cap_s = default_safety_cap_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            lease_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }
    // +15s buffer - see deadline_miss_detection/server.c's own doc comment for the real bug this
    // avoids. This scenario is invoked manually (not via run_scenario.sh's shared $CLIENT_ARGS,
    // since it needs a custom kill -9 step), so it wasn't hit here, fixed preemptively for
    // consistency with every other scenario's own server.c.
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

    static struct tt_Discovery discovery = {0};
    tt_Node_set_discovery(&node, &discovery, discovery_callback, NULL);

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "stream", (tt_SUBSCRIBER_CALLBACK)stream_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }
    sub.liveliness_lease_duration_ns = (uint64_t)(lease_s * (double)tt_SECOND);

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    // 200ms - fine enough granularity to measure detection latency.
    const int64_t poll_timeout_ns = 200LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && !departed && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, poll_timeout_ns);
    }

    double detect_latency_ms =
        (departed && last_received_ns != 0) ? (double)(departed_detected_ns - last_received_ns) / ns_per_ms : -1.0;

    printf("RESULT: framework=tickle scenario=liveliness_loss_detection role=server recv=%lu departed=%d "
           "detect_latency_ms=%.3f\n",
           (unsigned long)received, departed, detect_latency_ms);

    tt_Node_destroy(&node);
    return 0;
}
