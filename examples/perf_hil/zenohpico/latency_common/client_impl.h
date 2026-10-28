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
#include "BenchWindow.h"
#include "RttQuantiles.h"
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

// One round trip, shaped as the DDS clients' ping_once(): a 500 ms give-up (their response_wait), and only measured
// round trips enter the statistics. Edges (warm-up, cool-down) are counted in transmitted/received only.
struct zrtt_stats {
    uint64_t transmitted, received, measured_sent, measured_recv;
    double min_ms, max_ms, sum_ms;
};
static struct BenchRtt g_rtt;
static const double k_response_wait_s = BENCH_EDGE_GIVE_UP_S;

static void ping_once(const z_loaned_publisher_t* pub, uint8_t* buffer, uint32_t seq, bool measured,
                      struct zrtt_stats* st) {
    const uint64_t stamp = now_ns();
    memcpy(buffer, &stamp, sizeof(stamp));
    memcpy(buffer + sizeof(stamp), &seq, sizeof(seq));
    g_pong_seq = 0xFFFFFFFFu;
    z_owned_bytes_t payload;
    z_bytes_copy_from_buf(&payload, buffer, BENCH_SAMPLE_BYTES);
    st->transmitted++;
    if (measured) {
        st->measured_sent++;
    }
    if (z_publisher_put(pub, z_move(payload), NULL) < 0) {
        return; // counted as sent and unanswered
    }
    const uint64_t wait_until = now_ns() + (uint64_t)(k_response_wait_s * 1e9);
    while (g_pong_seq != seq && now_ns() < wait_until && !g_interrupted) {
        struct timespec ts = {0, 50000}; // 50 us: short enough not to quantise a 0.2 ms round trip
        nanosleep(&ts, NULL);
    }
    if (g_pong_seq != seq) {
        return;
    }
    const double rtt_ms = (double)(g_pong_ns - stamp) / 1e6;
    st->received++;
    if (!measured) {
        return;
    }
    st->measured_recv++;
    st->sum_ms += rtt_ms;
    if (st->min_ms < 0.0 || rtt_ms < st->min_ms) {
        st->min_ms = rtt_ms;
    }
    if (rtt_ms > st->max_ms) {
        st->max_ms = rtt_ms;
    }
    BenchRtt_add(&g_rtt, rtt_ms);
}

static void sleep_s(double s) {
    if (s > 0.0) {
        struct timespec ts = {(time_t)s, (long)((s - (double)(time_t)s) * 1e9)};
        nanosleep(&ts, NULL);
    }
}

int main(int argc, char** argv) {
    bench_stats_begin(&g_bench_stats);
    double duration_s = 10.0;
    double interval_s = 0.1;
    // Warm-up / cool-down edges exactly as the DDS and TickLE latency clients take them (BenchWindow.h): -W/-C round
    // trips, -I apart, each edge ended by count or by -T seconds. -T used to be this client's per-ping timeout; that is
    // now the DDS clients' fixed 500 ms give-up, so the campaign's argv means the same thing to all four frameworks.
    uint32_t warmup_rtts = BENCH_WARMUP_ROUND_TRIPS;
    uint32_t cooldown_rtts = BENCH_COOLDOWN_ROUND_TRIPS;
    double edge_interval_s = BENCH_EDGE_INTERVAL_S;
    double edge_max_s = BENCH_EDGE_MAX_S;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
            warmup_rtts = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            cooldown_rtts = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            edge_interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            edge_max_s = atof(argv[++i]);
        }
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
    struct zrtt_stats st = {.min_ms = -1.0};
    uint32_t seq = 0;

    uint32_t warmup_sent = 0;
    uint64_t warmup_first_ns = now_ns();
    const char* warmup_end = warmup_rtts == 0 ? "count" : NULL;
    while (!g_interrupted && warmup_end == NULL) {
        ping_once(z_loan(pub), buffer, seq++, false, &st);
        warmup_sent++;
        warmup_end = BenchWindow_edge_close(warmup_sent, warmup_rtts, warmup_first_ns, now_ns(), edge_max_s);
        sleep_s(warmup_end == NULL ? edge_interval_s : interval_s);
    }
    const uint64_t deadline = now_ns() + (uint64_t)(duration_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        ping_once(z_loan(pub), buffer, seq++, true, &st);
        sleep_s(interval_s);
    }
    uint32_t cooldown_sent = 0;
    uint64_t cooldown_first_ns = now_ns();
    const char* cooldown_end = cooldown_rtts == 0 ? "count" : NULL;
    while (!g_interrupted && cooldown_end == NULL) {
        ping_once(z_loan(pub), buffer, seq++, false, &st);
        cooldown_sent++;
        cooldown_end = BenchWindow_edge_close(cooldown_sent, cooldown_rtts, cooldown_first_ns, now_ns(), edge_max_s);
        if (cooldown_end == NULL) {
            sleep_s(edge_interval_s);
        }
    }

    bench_stats_end(&g_bench_stats);
    const double loss_pct =
        st.transmitted > 0 ? 100.0 * (double)(st.transmitted - st.received) / (double)st.transmitted : 0.0;
    const double avg = st.measured_recv > 0 ? st.sum_ms / (double)st.measured_recv : 0.0;
    printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO " role=client sent=%lu recv=%lu loss_pct=%.6f "
           "rtt_min_ms=%.3f rtt_avg_ms=%.6f rtt_max_ms=%.3f rtt_p50_ms=%.3f rtt_p99_ms=%.3f rtt_kept=%u "
           "warmup=%u cooldown=%u measured=%lu measured_sent=%lu edge_interval_s=%.4f edge_max_s=%.3f "
           "warmup_end=%s cooldown_end=%s window=%s " BENCH_TRANSPORT_FIELDS " %s\n",
           (unsigned long)st.transmitted, (unsigned long)st.received, loss_pct, st.min_ms < 0.0 ? 0.0 : st.min_ms, avg,
           st.max_ms, BenchRtt_quantile(&g_rtt, BENCH_RTT_P50), BenchRtt_quantile(&g_rtt, BENCH_RTT_P99),
           (unsigned)g_rtt.count, warmup_sent, cooldown_sent, (unsigned long)st.measured_recv,
           (unsigned long)st.measured_sent, edge_interval_s, edge_max_s, BenchWindow_edge_end_name(warmup_end),
           BenchWindow_edge_end_name(cooldown_end), st.measured_recv > 0 ? "ok" : "fail:no_measured_round_trip",
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_SENDER, st.transmitted, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    z_drop(z_move(pub));
    z_drop(z_move(sub));
    z_drop(z_move(session));
    return 0;
}

#endif
