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
// TickLE core native subscriber role. See client.c's own doc comment for the full mechanism this
// mirrors on the reader side - `sub.deadline_duration_ns` is the RxO-matched requested value
// (Milestone 49), a periodic checker (own fixed cadence, independent of when samples actually
// arrive) counts a miss whenever more than one deadline period has elapsed since the last receive.

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

static const double default_deadline_s = 0.05;
static const double miss_tolerance_multiplier = 1.5;
static const double default_safety_cap_s = 40.0;
static const double safety_cap_buffer_s = 15.0;
static const double discovery_margin_s = 2.0;

static double deadline_s = default_deadline_s;
static uint64_t received = 0;
static uint32_t misses = 0;
static uint64_t last_received_ns = 0;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    (void)data;
    received++;
    last_received_ns = tt_get_ns();
}

static void check_deadline(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted) {
        return;
    }
    // 1.5x tolerance, not a phase offset (2026-09-21, real bug found the hard way, same symptom
    // class as client.c's own fix but a different cause): client.c's own send_one and
    // check_deadline share one process/clock, so a controlled relative offset between them is
    // meaningful there. This checker and the remote client's own send schedule run on two
    // different machines' own independent clocks, with no controlled phase relationship at all
    // (run_scenario.sh's own `sleep 3` between launching each side is only approximate, subject to
    // real SSH/process-launch jitter) - a strict `> deadline_s` threshold trips on essentially any
    // unlucky relative timing (reader_misses=30 over a 10s run with recv=sent=198, i.e. every
    // sample actually arrived on time - the real symptom this fixes). 1.5x absorbs that jitter
    // while still catching a genuine, multi-period gap.
    if (last_received_ns != 0 &&
        time - last_received_ns > (uint64_t)(miss_tolerance_multiplier * deadline_s * (double)tt_SECOND)) {
        misses++;
    }
    tt_Node_schedule(node, time + (uint64_t)(deadline_s * (double)tt_SECOND), check_deadline, NULL);
}

int main(int argc, char** argv) {
    double safety_cap_s = default_safety_cap_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            deadline_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }
    // +15s buffer (2026-09-21, real bug found the hard way): run_scenario.sh forwards the same
    // $CLIENT_ARGS to both sides, and `-d` means something different on each - the client's own
    // real send duration vs. this side's own "don't hang forever" upper bound. Taking it verbatim
    // here made this side exit (and print its RESULT, ending the count) *before* the client had
    // even finished sending: the client doesn't start until run_scenario.sh's own PRE_CLIENT_SLEEP
    // (default 3s) plus its own ~2s discovery margin have elapsed, both counted against *this*
    // side's clock too. `-d 10` reproducibly gave recv=91/sent=198 (not genuine loss - this
    // side's own 10s cap expired mid-stream, cutting off the tail) - identical both times, which
    // is itself the tell: real network loss wouldn't reproduce to the exact same sample.
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
    sub.deadline_duration_ns = (uint64_t)(deadline_s * (double)tt_SECOND);

    tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(discovery_margin_s * (double)tt_SECOND), check_deadline, NULL);

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    // 500ms (nanoseconds), so the deadline/g_interrupted check re-runs.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, poll_timeout_ns);
    }

    printf("RESULT: framework=tickle scenario=deadline_miss_detection role=server recv=%lu reader_misses=%u\n",
           (unsigned long)received, misses);

    tt_Node_destroy(&node);
    return 0;
}
