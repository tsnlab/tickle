/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL service cell, scenario "service_call" - the TickLE core client (no rmw). A tt_Client of BenchService
// (common/BenchService.h) on the services default profile (RELIABLE: an unanswered request is resent on core's auto
// schedule) calls a server on the other Pi. Latency is the ROUND TRIP of each call on this side's own clock: from just
// before tt_Client_call_id() to the client callback that reports the answer. No figure here reads the other host's
// clock (cross-host timing is round trip only).
//
// Rates (-r):
//   -r <hz> > 0   one call every 1/hz s, on a fixed schedule from the first; a tick whose call core refuses (every
//                 call slot busy, tt_CLIENT_MAX_OUTSTANDING) is skipped and counted in refused=, so calls_per_s=
//                 falls below the rate rather than the schedule stretching. A schedule more than one period behind
//                 resumes from now (late_ticks=).
//   -r 0          as fast as answers come: after every answer (and at the start) the client calls until core refuses,
//                 so up to tt_CLIENT_MAX_OUTSTANDING calls are in flight, within the tt_CLIENT_CALL_WINDOW core keeps;
//                 in_flight_max= says how many were. refused= then counts the refusals that end each top-up, not lost
//                 work.
// Phases, by this side's clock: 2 s of discovery margin with no call, --warmup-s, then -d measured seconds, then
// --cooldown-s, then up to drain_s for the calls still in flight (core ends an auto-schedule call within 1 s). The
// round-trip statistics, measured=, measured_timeouts= and calls_per_s= are over the calls MADE in the measured phase;
// sent=, recv= and timeouts= over the whole run. The per-call CPU, RSS and wire figures (BenchStats.h) divide the whole
// process by sent=.
//
// Probes (the duplicate counter's control, service_call/server.c): warm-up calls with k % BENCH_SVC_PROBE_EVERY ==
// BENCH_SVC_PROBE_AT carry a flag; the server turns the first arrival of each away unanswered (tt_CALL_IGNORED), and
// this client's retry makes its callback run a second time for that call. Only warm-up calls, so no measured round
// trip includes a probe's retry wait. probes= and probes_answered= are this side's count of them.

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
#include "RttQuantiles.h"

#define BENCH_SVC_PROBE_EVERY 8U
#define BENCH_SVC_PROBE_AT 2U

static struct BenchStats g_bench_stats;
static char g_bench_fields[BENCH_STATS_FIELDS_MAX];

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static const double default_duration_s = 5.0;
static const double default_rate_hz = 1000.0;
static const double discovery_margin_s = 2.0;
static const double drain_s = 2.0;
static const double ns_per_ms = 1e6;
static const double ns_per_us = 1e3;

static struct tt_Context g_node;
static struct tt_Client g_client;
static double g_rate_hz = default_rate_hz;
static double g_duration_s = default_duration_s;
static struct BenchWindow g_window; // only its warm-up and cool-down (BenchWindow_parse_arg); static, see the server
static uint64_t g_warm_end_ns;
static uint64_t g_meas_end_ns;
static uint64_t g_cool_end_ns;
static uint64_t g_period_ns;
static uint64_t g_next_due_ns;

// One outstanding call, found again by the seq_no core gave it (tt_Client_call_id(), client->callback_seq_no).
struct outstanding {
    bool used;
    bool probe;
    uint8_t phase;
    uint16_t seq_no;
    uint32_t k;
    uint64_t send_ns;
};
static struct outstanding g_out[tt_CLIENT_MAX_OUTSTANDING];

static uint32_t g_k = 0; // the last call number made
static uint64_t g_sent = 0, g_answered = 0, g_timeouts = 0, g_refused = 0, g_call_errors = 0, g_late_ticks = 0;
static uint64_t g_mismatched = 0, g_unmatched = 0, g_bad_rc = 0, g_probes = 0, g_probes_answered = 0;
static uint64_t g_measured_sent = 0, g_measured = 0, g_measured_timeouts = 0;
static uint32_t g_in_flight = 0, g_in_flight_max = 0;
static double g_rtt_min_ms = -1.0, g_rtt_max_ms = 0.0, g_rtt_sum_ms = 0.0;
static struct BenchRtt g_rtt;
static bool g_calls_over = false; // the cool-down has ended: no more calls
static bool g_done = false;       // and every call has ended, or the drain ran out
static bool g_topup_pending = false;

static uint8_t phase_at(uint64_t now) {
    if (now < g_warm_end_ns) {
        return BENCH_SVC_PHASE_WARMUP;
    }
    return now < g_meas_end_ns ? BENCH_SVC_PHASE_MEASURED : BENCH_SVC_PHASE_COOLDOWN;
}

static void check_done(void) {
    if (g_calls_over && g_in_flight == 0) {
        g_done = true;
    }
}

static tt_ret_t make_call(uint64_t now) {
    uint32_t k = g_k + 1U;
    uint8_t phase = phase_at(now);
    bool probe = phase == BENCH_SVC_PHASE_WARMUP && (k % BENCH_SVC_PROBE_EVERY) == BENCH_SVC_PROBE_AT;
    struct BenchData request;
    memset(&request, 0, sizeof request);
    request.seq = k;
    request.payload[BENCH_SVC_PHASE_BYTE] = phase;
    request.payload[BENCH_SVC_PROBE_BYTE] = probe ? BENCH_SVC_PROBE : 0U;
    request.send_ns = tt_get_ns();
    uint16_t seq_no = 0;
    tt_ret_t ret = tt_Client_call_id(&g_client, (struct tt_Request*)&request, &seq_no);
    if (ret == tt_RET_ILLEGAL_STATUS) {
        g_refused++; // every call slot busy, or the oldest call is tt_CLIENT_CALL_WINDOW calls old
        return ret;
    }
    if (ret != tt_RET_OK) {
        g_call_errors++;
        return ret;
    }
    g_k = k;
    g_sent++;
    if (phase == BENCH_SVC_PHASE_MEASURED) {
        g_measured_sent++;
    }
    if (probe) {
        g_probes++;
    }
    for (int i = 0; i < tt_CLIENT_MAX_OUTSTANDING; i++) {
        if (!g_out[i].used) {
            g_out[i] = (struct outstanding) {.used = true,
                                             .probe = probe,
                                             .phase = phase,
                                             .seq_no = seq_no,
                                             .k = k,
                                             .send_ns = request.send_ns};
            break;
        }
    }
    g_in_flight++;
    if (g_in_flight > g_in_flight_max) {
        g_in_flight_max = g_in_flight;
    }
    return ret;
}

static void topup(struct tt_Context* node, uint64_t time, void* param);

static void request_topup(uint64_t now) {
    if (!g_topup_pending && !g_calls_over) {
        g_topup_pending = true;
        tt_Context_schedule(&g_node, now, topup, NULL);
    }
}

// -r 0: call until core refuses.
static void topup(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_topup_pending = false;
    uint64_t now = tt_get_ns();
    while (!g_interrupted && now < g_cool_end_ns && make_call(now) == tt_RET_OK) {
        now = tt_get_ns();
    }
}

// -r > 0: one call per period.
static void tick(struct tt_Context* node, uint64_t time, void* param) {
    (void)time;
    (void)param;
    uint64_t now = tt_get_ns();
    if (g_interrupted || now >= g_cool_end_ns) {
        return; // end_of_calls() ends the run
    }
    (void)make_call(now);
    g_next_due_ns += g_period_ns;
    if (g_next_due_ns + g_period_ns < now) {
        g_late_ticks++;
        g_next_due_ns = now + g_period_ns;
    }
    tt_Context_schedule(node, g_next_due_ns, tick, NULL);
}

static void end_of_calls(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_calls_over = true;
    check_done();
}

static void drain_deadline(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_done = true;
}

// One measured round trip into the statistics.
static void add_round_trip(double rtt_ms) {
    g_measured++;
    if (g_rtt_min_ms < 0.0 || rtt_ms < g_rtt_min_ms) {
        g_rtt_min_ms = rtt_ms;
    }
    if (rtt_ms > g_rtt_max_ms) {
        g_rtt_max_ms = rtt_ms;
    }
    g_rtt_sum_ms += rtt_ms;
    BenchRtt_add(&g_rtt, rtt_ms);
}

// The outstanding call core's callback is reporting (client->callback_seq_no), taken off the list; false if none is.
static bool take_outstanding(uint16_t seq_no, struct outstanding* out) {
    for (int i = 0; i < tt_CLIENT_MAX_OUTSTANDING; i++) {
        if (g_out[i].used && g_out[i].seq_no == seq_no) {
            *out = g_out[i];
            g_out[i].used = false;
            g_in_flight--;
            return true;
        }
    }
    return false;
}

static void answered(struct tt_Client* client, int8_t return_code, struct BenchData* response) {
    uint64_t now = tt_get_ns();
    struct outstanding call;
    if (!take_outstanding(client->callback_seq_no, &call)) {
        g_unmatched++; // a callback for no call this side made: the bookkeeping is wrong, and the row says so
        return;
    }
    bool measured = call.phase == BENCH_SVC_PHASE_MEASURED;
    if (return_code == tt_CALL_TIMEOUT || response == NULL) {
        g_timeouts++;
        g_measured_timeouts += measured ? 1U : 0U;
    } else {
        g_bad_rc += return_code != 0 ? 1U : 0U; // the server answers 0 always
        g_answered++;
        g_mismatched += response->seq != call.k ? 1U : 0U; // an answer handed to the wrong call
        g_probes_answered += call.probe ? 1U : 0U;
        if (measured) {
            add_round_trip((double)(now - call.send_ns) / ns_per_ms);
        }
    }
    if (g_rate_hz <= 0.0) {
        request_topup(now);
    }
    check_done();
}

int main(int argc, char** argv) {
    // Armed first, so the counters cover discovery too, as in every other bench.
    bench_stats_begin(&g_bench_stats);
    BenchWindow_init(&g_window);
    for (int i = 1; i < argc; i++) {
        if (BenchWindow_parse_arg(&g_window, argc, argv, &i)) {
            continue;
        }
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            g_rate_hz = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            g_duration_s = atof(argv[++i]); // the measured window, between warm-up and cool-down
        }
    }

    _tt_CONFIG.broadcast = "192.168.10.255"; // the rig link's (reliable_latency/client.c says why)

    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    tt_ret_t ret = tt_Context_create(&g_node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }
    const struct tt_ClientOptions options = {.callback = (tt_CLIENT_CALLBACK)answered,
                                             .call_retry_interval = 0,
                                             .call_retry_count = 0};
    ret = tt_Node_create_client(tt_Context_default_node(&g_node), &g_client, &bench_service, BENCH_SVC_NAME,
                                &tt_QOS_PROFILE_SERVICES_DEFAULT, &options, NULL, 0);
    if (ret != 0) {
        printf("Cannot create client: %d\n", ret);
        return ret;
    }

    uint64_t start = tt_get_ns() + (uint64_t)(discovery_margin_s * (double)tt_SECOND);
    g_warm_end_ns = start + (uint64_t)(g_window.warmup_s * (double)tt_SECOND);
    g_meas_end_ns = g_warm_end_ns + (uint64_t)(g_duration_s * (double)tt_SECOND);
    g_cool_end_ns = g_meas_end_ns + (uint64_t)(g_window.cooldown_s * (double)tt_SECOND);
    if (g_rate_hz > 0.0) {
        g_period_ns = (uint64_t)((double)tt_SECOND / g_rate_hz);
        g_next_due_ns = start;
        tt_Context_schedule(&g_node, start, tick, NULL);
    } else {
        g_topup_pending = true;
        tt_Context_schedule(&g_node, start, topup, NULL);
    }
    tt_Context_schedule(&g_node, g_cool_end_ns, end_of_calls, NULL);
    tt_Context_schedule(&g_node, g_cool_end_ns + (uint64_t)(drain_s * (double)tt_SECOND), drain_deadline, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && !g_done && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT || ret == tt_RET_INTERRUPTED)) {
        ret = tt_Context_poll(&g_node, -1);
    }
    int poll_ret = (int)ret;

    double avg = g_measured > 0 ? g_rtt_sum_ms / (double)g_measured : 0.0;
    double loss_pct = g_sent > 0 ? 100.0 * (double)g_timeouts / (double)g_sent : 0.0;
    double calls_per_s = g_duration_s > 0.0 ? (double)g_measured / g_duration_s : 0.0;
    printf("\n--- tickle service_call statistics ---\n");
    // "unanswered", not the word for it: rig_preflight.sh reads that word in a log as a fault.
    printf("%llu calls, %llu answered, %llu unanswered; %llu measured, rtt avg %.3f ms\n", (unsigned long long)g_sent,
           (unsigned long long)g_answered, (unsigned long long)g_timeouts, (unsigned long long)g_measured, avg);
    bench_stats_end(&g_bench_stats);
    printf("RESULT: framework=tickle scenario=service_call role=client rate_hz=%.1f sent=%llu recv=%llu "
           "timeouts=%llu loss_pct=%.6f refused=%llu call_errors=%llu late_ticks=%llu mismatched=%llu unmatched=%llu "
           "bad_rc=%llu probes=%llu probes_answered=%llu in_flight_max=%u max_outstanding=%d call_window=%d "
           "measured_sent=%llu measured=%llu measured_timeouts=%llu calls_per_s=%.3f rtt_min_ms=%.6f "
           "rtt_avg_ms=%.6f rtt_max_ms=%.6f rtt_p50_ms=%.6f rtt_p99_ms=%.6f rtt_kept=%u srtt_us=%.3f drained=%s "
           "warmup_s=%.3f cooldown_s=%.3f win_s=%.3f window=%s poll_ret=%d %s\n",
           g_rate_hz, (unsigned long long)g_sent, (unsigned long long)g_answered, (unsigned long long)g_timeouts,
           loss_pct, (unsigned long long)g_refused, (unsigned long long)g_call_errors, (unsigned long long)g_late_ticks,
           (unsigned long long)g_mismatched, (unsigned long long)g_unmatched, (unsigned long long)g_bad_rc,
           (unsigned long long)g_probes, (unsigned long long)g_probes_answered, (unsigned)g_in_flight_max,
           tt_CLIENT_MAX_OUTSTANDING, tt_CLIENT_CALL_WINDOW, (unsigned long long)g_measured_sent,
           (unsigned long long)g_measured, (unsigned long long)g_measured_timeouts, calls_per_s, g_rtt_min_ms, avg,
           g_rtt_max_ms, BenchRtt_quantile(&g_rtt, BENCH_RTT_P50), BenchRtt_quantile(&g_rtt, BENCH_RTT_P99),
           (unsigned)g_rtt.count, (double)g_client.latency / ns_per_us, g_in_flight == 0 ? "answered" : "timeout",
           g_window.warmup_s, g_window.cooldown_s, g_duration_s, g_measured > 0 ? "ok" : "fail:no_measured_call",
           poll_ret,
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_SENDER, g_sent, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    tt_Client_destroy(&g_client);
    tt_Context_destroy(&g_node);
    return 0;
}
