/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL service cell, scenario "service_call" - the TickLE core server (no rmw). A tt_Server of BenchService on the
// services default profile (RELIABLE) that answers every call at once with the request echoed back, and counts how
// many times its callback ran for each call: the falsifier of the cell is a call whose callback ran twice to answer
// it, the defect fb065d65 fixed in rmw_tickle (a retry that found no kept answer ran the service again).
//
// Counting, per call k (BenchService.h: the request's seq):
//   runs      every callback invocation for k.
//   answered  the invocations that answered (returned 0). dup_exec= is the number of calls with answered > 1, and
//             exec_max= the largest answered count; dup_exec must be 0.
//   probes    the client marks some warm-up calls as probes (service_call/client.c). The first invocation of a probe
//             returns tt_CALL_IGNORED - nothing is sent or kept, so the client's retry asks again and the callback
//             runs a second time for the same k. A probe answered with runs == 2 is probe_ok, anything else
//             probe_bad: the counter's own control, in every run. It shows that a second execution for one call,
//             arriving across the link as a retry, lands on that call's counter - the thing dup_exec has to see.
// The counts live in a ring of BENCH_SVC_RING calls indexed by k. A call whose slot already holds a LATER call cannot
// be judged (its earlier executions, if any, were overwritten) and is counted in unjudged=, which voids the row: a
// retry arrives within core's retry horizon, ~1 s, so the ring is that many calls deep at 262,144 calls a second.
//
// recv= is the calls answered at least once (the divisor of this side's per-call CPU and instructions), win_recv= those
// the client made in its measured phase. The process runs until SIGINT (run_scenario.sh stops it after the client),
// with a lifetime backstop of -d + 60 s.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "Bench.h"
#include "BenchService.h"
#include "BenchStats.h" // shared instrumentation - see its own header
#include "BenchWindow.h"

static struct BenchStats g_bench_stats;
static char g_bench_fields[BENCH_STATS_FIELDS_MAX];

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

#define BENCH_SVC_RING (1U << 18U)

struct call_count {
    uint32_t k; // the call this slot counts; 0: none yet (calls are numbered from 1)
    uint16_t runs;
    uint16_t answered;
};
static struct call_count g_ring[BENCH_SVC_RING];

static uint64_t g_runs = 0;      // callback invocations
static uint64_t g_ignored = 0;   // of them, probes turned away (tt_CALL_IGNORED)
static uint64_t g_unique = 0;    // calls answered at least once
static uint64_t g_win_recv = 0;  // of them, made in the client's measured phase
static uint64_t g_dup_exec = 0;  // calls answered more than once: must be 0
static uint32_t g_exec_max = 0;  // the most answers any one call got
static uint64_t g_probe_ok = 0;  // probes answered on their second run
static uint64_t g_probe_bad = 0; // probes answered on any other run
static uint64_t g_unjudged = 0;  // calls whose slot held a later call
// The first call answered twice, for whoever reads a dup_exec above 0: its k, the client's phase when it made it, and
// whether it was a probe (dup_first_k=0: none).
static uint32_t g_dup_first_k = 0;
static unsigned g_dup_first_phase = 0;
static unsigned g_dup_first_probe = 0;

static int8_t serve(struct tt_Server* server, struct BenchData* request, struct BenchData* response,
                    tt_RequestId request_id) {
    (void)server;
    (void)request_id; // 16-bit seq_no, wraps in a run; the request's own 32-bit k is the id counted
    g_runs++;
    uint32_t k = request->seq;
    struct call_count* slot = &g_ring[k & (BENCH_SVC_RING - 1U)];
    if (slot->k != k) {
        if (slot->k > k) {
            g_unjudged++; // its earlier executions, if any, are gone
        }
        slot->k = k;
        slot->runs = 0;
        slot->answered = 0;
    }
    if (slot->runs < UINT16_MAX) {
        slot->runs++;
    }
    bool probe = request->payload[BENCH_SVC_PROBE_BYTE] == BENCH_SVC_PROBE;
    if (probe && slot->runs == 1) {
        g_ignored++;
        return tt_CALL_IGNORED; // nothing sent or kept: the client's retry asks again
    }
    if (slot->answered < UINT16_MAX) {
        slot->answered++;
    }
    if (slot->answered == 1) {
        g_unique++;
        if (request->payload[BENCH_SVC_PHASE_BYTE] == BENCH_SVC_PHASE_MEASURED) {
            g_win_recv++;
        }
        if (probe) {
            if (slot->runs == 2) {
                g_probe_ok++;
            } else {
                g_probe_bad++;
            }
        }
    } else if (slot->answered == 2) {
        if (g_dup_exec++ == 0) {
            g_dup_first_k = k;
            g_dup_first_phase = request->payload[BENCH_SVC_PHASE_BYTE];
            g_dup_first_probe = probe ? 1U : 0U;
        }
    }
    if (slot->answered > g_exec_max) {
        g_exec_max = slot->answered;
    }
    *response = *request;
    return 0;
}

static const double default_duration_s = 5.0;
static const double lifetime_buffer_s = 60.0;

int main(int argc, char** argv) {
    bench_stats_begin(&g_bench_stats);
    double duration_s = default_duration_s;
    // Only its warm-up and cool-down are used, echoed so a reader can match the client's. Static: the bucket array is
    // half a megabyte, and its pages stay untouched here.
    static struct BenchWindow window_args;
    BenchWindow_init(&window_args);
    for (int i = 1; i < argc; i++) {
        if (BenchWindow_parse_arg(&window_args, argc, argv, &i)) {
            continue;
        }
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        }
    }

    _tt_CONFIG.broadcast = "192.168.10.255"; // the rig link's (reliable_latency/server.c says why)

    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Context node;
    tt_ret_t ret = tt_Context_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }
    static struct tt_Server server;
    const struct tt_ServerOptions options = {.callback = (tt_SERVER_CALLBACK)serve, .max_response_bytes = 0};
    ret = tt_Node_create_server(tt_Context_default_node(&node), &server, &bench_service, BENCH_SVC_NAME,
                                &tt_QOS_PROFILE_SERVICES_DEFAULT, &options, NULL, 0);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    const double lifetime_s = window_args.warmup_s + duration_s + window_args.cooldown_s + lifetime_buffer_s;
    uint64_t deadline = tt_get_ns() + (uint64_t)(lifetime_s * (double)tt_SECOND);
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000; // so the deadline and SIGINT are looked at
    ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline &&
           (ret == tt_RET_OK || ret == tt_RET_TIMEOUT || ret == tt_RET_INTERRUPTED)) {
        ret = tt_Context_poll(&node, poll_timeout_ns);
    }
    int poll_ret = (int)ret;

    bench_stats_end(&g_bench_stats);
    printf("RESULT: framework=tickle scenario=service_call role=server recv=%llu runs=%llu ignored=%llu "
           "dup_exec=%llu dup_first_k=%u dup_first_phase=%u dup_first_probe=%u exec_max=%u probe_ok=%llu "
           "probe_bad=%llu unjudged=%llu win_recv=%llu warmup_s=%.3f "
           "cooldown_s=%.3f win_s=%.3f window=%s poll_ret=%d %s\n",
           (unsigned long long)g_unique, (unsigned long long)g_runs, (unsigned long long)g_ignored,
           (unsigned long long)g_dup_exec, (unsigned)g_dup_first_k, g_dup_first_phase, g_dup_first_probe,
           (unsigned)g_exec_max, (unsigned long long)g_probe_ok, (unsigned long long)g_probe_bad,
           (unsigned long long)g_unjudged, (unsigned long long)g_win_recv, window_args.warmup_s, window_args.cooldown_s,
           duration_s, g_win_recv > 0 ? "ok" : "fail:no_measured_call", poll_ret,
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_RECEIVER, g_unique, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    tt_Server_destroy(&server);
    tt_Context_destroy(&node);
    return 0;
}
