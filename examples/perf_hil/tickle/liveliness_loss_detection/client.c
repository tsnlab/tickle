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
// - TickLE core native publisher role. `pub.liveliness_lease_duration_ns` (matched lease, `-T`,
// default 2.0s like the DDS twins) is purely a wire/RxO-announced value in TickLE (COMPARISON.MD's
// own native-QoS table, corrected this same pass) - the actual peer-departure DETECTION mechanism
// is separate and node-level (server.c's own doc comment). This side just publishes normally on a
// fixed schedule - the orchestration (run_scenario.sh, NOT this program) sends a real `kill -9`
// partway through, the same "genuinely stop, no graceful farewell" design the DDS twins use.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "../common/Bench.h"

// Never set any more: this scenario installs no SIGINT handler (see main()). Kept so the
// send loop reads the same as its sibling scenarios; the loop ends on its own duration cap.
static volatile sig_atomic_t g_interrupted = 0;
// 0.1s, matching cyclonedds/ and fastdds/ liveliness_loss_detection/client.c. The server's
// detect_latency_ms is measured from the last received sample, so the publish interval sets how
// stale that reference point can be; at the previous 0.5s this harness sampled five times coarser
// than the DDS twins it is compared against, and the difference was invisible in the table.
static const double default_interval_s = 0.1;
static const double default_lease_s = 2.0;
static const double discovery_margin_s = 2.0;

static double interval_s = default_interval_s;
static uint32_t seq = 0;
static struct tt_Publisher* g_pub;

static void send_one(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted) {
        return;
    }
    struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
    tt_Publisher_publish(g_pub, (struct tt_Data*)&msg);
    tt_Node_schedule(node, time + (uint64_t)(interval_s * (double)tt_SECOND), send_one, NULL);
}

int main(int argc, char** argv) {
    double lease_s = default_lease_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            lease_s = atof(argv[++i]);
        }
    }

    // real HIL link's own broadcast address - see best_effort_latency/client.c's own doc comment
    // for the real bug this avoids.
    _tt_CONFIG.broadcast = "192.168.10.255";

    // A liveliness_loss_detection client must die the way a crashed process dies, so this
    // scenario deliberately does NOT install a SIGINT handler. Measured 2026-09-23, lease 2.0s, 2
    // reps each: under `kill -INT` TickLE was detected in 88/92ms (its own goodbye broadcast) while
    // BOTH DDS vendors raised no liveliness event at all (clean unregister), against 1900-2332ms
    // and ~1999-2000ms under `kill -9`. A run with the wrong signal would not merely flatter one
    // framework - it would make the other two look broken, which is the kind of table nobody
    // double-checks because it confirms what they wanted. Leaving SIGINT at its default
    // disposition (terminate, no cleanup) makes the correct measurement the only obtainable one,
    // whichever signal the orchestrator sends and whoever runs it.
    //
    // Verified on the rig after the change (TickLE, lease 2.0s): `kill -9` still detects normally
    // (2325/2325ms), while `kill -INT` now leaves the client RUNNING and the run ends
    // departed=0/detect_latency_ms=-1. That is the intended outcome and worth understanding: the
    // orchestrator launches this client with `nohup ... &`, and a non-interactive shell sets
    // SIGINT to ignore for a background job, which the explicit handler used to override. So a
    // wrong-signal run now fails loudly instead of producing a plausible wrong number - and all
    // three frameworks fail it the same way, since the DDS twins raise no liveliness event under
    // -INT either.

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
    pub.liveliness_lease_duration_ns = (uint64_t)(lease_s * (double)tt_SECOND);
    g_pub = &pub;

    printf("Publisher: pid=%d, announcing lease=%.1fs, publishing every %.1fs until killed\n", getpid(), lease_s,
           interval_s);
    fflush(stdout);

    tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(discovery_margin_s * (double)tt_SECOND), send_one, NULL);

    tt_ret_t poll_ret = tt_RET_OK;
    while (!g_interrupted && (poll_ret == tt_RET_OK || poll_ret == tt_RET_TIMEOUT)) {
        poll_ret = tt_Node_poll(&node, -1);
    }

    tt_Node_destroy(&node);
    return 0;
}
