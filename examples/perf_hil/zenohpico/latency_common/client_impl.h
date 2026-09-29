/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The ping half of zenoh-pico's latency cells, shaped like the tickle, cyclonedds and fastdds twins so one parser
// reads all four RESULT lines: sent, recv, loss_pct, rtt_min_ms, rtt_avg_ms, rtt_max_ms.
//
// One ping at a time, waiting for its pong before the next - that is what makes the number a round trip rather than
// a queue depth, and it is what the other three harnesses do. A ping that never comes back is counted as lost after
// a timeout rather than stalling the run.
#ifndef ZENOH_LATENCY_CLIENT_IMPL_H
#define ZENOH_LATENCY_CLIENT_IMPL_H

#define ROLE_NAME "client"
#include "session.h"

static volatile uint32_t g_pong_seq = 0xFFFFFFFFu;
static volatile uint64_t g_pong_ns = 0;

static void on_pong(z_loaned_sample_t* sample, void* arg) {
    (void)arg;
    const uint64_t arrived = now_ns();
    const z_loaned_bytes_t* payload = z_sample_payload(sample);
    uint8_t head[sizeof(uint64_t) + sizeof(uint32_t)];
    z_bytes_reader_t reader = z_bytes_get_reader(payload);
    if (z_bytes_reader_read(&reader, head, sizeof(head)) != (size_t)sizeof(head)) {
        return;
    }
    uint32_t seq = 0;
    memcpy(&seq, head + sizeof(uint64_t), sizeof(seq));
    g_pong_ns = arrived;
    g_pong_seq = seq; // written last: the waiter spins on the sequence, so the timestamp must already be there
}

int main(int argc, char** argv) {
    bench_stats_begin(&g_bench_stats);
    double duration_s = 10.0;
    double interval_s = 0.1;
    double timeout_s = 1.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            timeout_s = atof(argv[++i]);
        }
        // -K/-N/-B/-C are history depth, a KEEP_ALL bound, max_blocking_time and a drain cap: accepted and ignored
        // so the campaign hands all four frameworks the same argv. zenoh-pico has no counterpart for any of them.
    }
    signal(SIGINT, handle_sigint);

    z_owned_session_t session;
    if (!latency_session_open(&session)) {
        return 1;
    }

    z_view_keyexpr_t ping_ke, pong_ke;
    z_view_keyexpr_from_str(&ping_ke, PING_KEY);
    z_view_keyexpr_from_str(&pong_ke, PONG_KEY);

    z_owned_closure_sample_t callback;
    z_closure(&callback, on_pong, NULL, NULL);
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(session), &sub, z_loan(pong_ke), z_move(callback), NULL) < 0) {
        printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO " role=client error=declare_failed\n");
        return 1;
    }
    z_owned_publisher_t pub;
    z_publisher_options_t popts;
    z_publisher_options_default(&popts);
#if BENCH_RELIABLE
    popts.congestion_control = Z_CONGESTION_CONTROL_BLOCK; // the reliable pair's promise, as in its throughput twin
#else
    popts.congestion_control = Z_CONGESTION_CONTROL_DROP; // the best-effort pair's promise: drop when full
#endif
#ifdef Z_FEATURE_UNSTABLE_API
    popts.reliability = BENCH_RELIABLE ? Z_RELIABILITY_RELIABLE : Z_RELIABILITY_BEST_EFFORT;
#endif
    if (z_declare_publisher(z_loan(session), &pub, z_loan(ping_ke), &popts) < 0) {
        printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO " role=client error=declare_failed\n");
        return 1;
    }

    latency_settle();

    uint8_t buffer[BENCH_SAMPLE_BYTES];
    memset(buffer, 0, sizeof(buffer));
    uint64_t sent = 0, recv = 0;
    double rtt_min_ms = -1.0, rtt_max_ms = 0.0, rtt_sum_ms = 0.0;
    const uint64_t start = now_ns();
    const uint64_t deadline = start + (uint64_t)(duration_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        const uint32_t seq = (uint32_t)sent;
        const uint64_t stamp = now_ns();
        memcpy(buffer, &stamp, sizeof(stamp));
        memcpy(buffer + sizeof(stamp), &seq, sizeof(seq));
        g_pong_seq = 0xFFFFFFFFu;
        z_owned_bytes_t payload;
        z_bytes_copy_from_buf(&payload, buffer, sizeof(buffer));
        if (z_publisher_put(z_loan(pub), z_move(payload), NULL) < 0) {
            sent++;
            continue; // counted as sent and unanswered, which is what loss_pct means here
        }
        sent++;
        const uint64_t wait_until = now_ns() + (uint64_t)(timeout_s * 1e9);
        while (g_pong_seq != seq && now_ns() < wait_until && !g_interrupted) {
            struct timespec ts = {0, 50000}; // 50 us: short enough not to quantise a 0.2 ms round trip
            nanosleep(&ts, NULL);
        }
        if (g_pong_seq == seq) {
            const double rtt_ms = (double)(g_pong_ns - stamp) / 1e6;
            recv++;
            rtt_sum_ms += rtt_ms;
            if (rtt_min_ms < 0.0 || rtt_ms < rtt_min_ms) {
                rtt_min_ms = rtt_ms;
            }
            if (rtt_ms > rtt_max_ms) {
                rtt_max_ms = rtt_ms;
            }
        }
        if (interval_s > 0.0) {
            struct timespec ts = {(time_t)interval_s, (long)((interval_s - (double)(time_t)interval_s) * 1e9)};
            nanosleep(&ts, NULL);
        }
    }

    bench_stats_end(&g_bench_stats);
    const double loss_pct = sent > 0 ? 100.0 * (double)(sent - recv) / (double)sent : 0.0;
    const double avg = recv > 0 ? rtt_sum_ms / (double)recv : 0.0;
    printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO " role=client sent=%lu recv=%lu loss_pct=%.1f "
           "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f " BENCH_TRANSPORT_FIELDS " %s\n",
           (unsigned long)sent, (unsigned long)recv, loss_pct, rtt_min_ms < 0.0 ? 0.0 : rtt_min_ms, avg, rtt_max_ms,
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_SENDER, recv, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    z_drop(z_move(pub));
    z_drop(z_move(sub));
    z_drop(z_move(session));
    return 0;
}

#endif
