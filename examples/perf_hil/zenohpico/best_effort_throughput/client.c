/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// zenoh-pico's publisher for the best-effort throughput cell, shaped exactly like the cyclonedds and
// fastdds twins so one parser reads all four frameworks' RESULT lines and the instrument gate in
// BenchStats.h applies unchanged.
//
// Why best-effort and not reliable: zenoh-pico does not repair loss. Its own source says so on both
// transports - src/transport/multicast/rx.c:194 and src/transport/unicast/rx.c:106, "@TODO: amend once
// reliability is in place. For the time being only monotonic SNs are ensured". So Z_RELIABILITY_RELIABLE
// means ordering rather than retransmission, and the only cell where both sides make the same promise is
// this one. See rmw_tickle/ZENOH_PICO_PLAN.md.
//
// -X (express) is the fairness arm, not a tuning knob. zenoh-pico batches several messages into one
// network operation by default (Z_BATCH_MULTICAST_SIZE 2048), while TickLE sends one datagram per sample;
// is_express=true turns that off per publisher, so both arms come from one binary. Every row states which.
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zenoh-pico.h>

// Not Bench.h: that header pulls in TickLE's own config, and the DDS harnesses do not use it either -
// they generate from examples/perf_hil/idl. All this harness needs from the shape is its byte count,
// which build.sh passes as -DBENCH_SAMPLE_BYTES exactly as it does for the other three.
#include "BenchStats.h"

static char g_bench_fields[BENCH_STATS_FIELDS_MAX];
static struct BenchStats g_bench_stats;
static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    // Armed before any middleware setup, so the counters cover session establishment and discovery too -
    // identically for all four frameworks, which is what makes them comparable.
    bench_stats_begin(&g_bench_stats);
    double duration_s = 10.0;
    double interval_s = 0.0;
    bool express = false;
    const char* iface = getenv("BENCH_IFACE") != NULL ? getenv("BENCH_IFACE") : "eth0";
    const char* keyexpr = "bench/stream";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-X") == 0) {
            express = true;
        }
        // Options the other harnesses take and this one cannot honour are accepted and ignored on purpose,
        // so the campaign passes all four the same argv: -K/-N/-B/-C are history depth, a KEEP_ALL bound,
        // max_blocking_time and a drain cap, and zenoh-pico has no counterpart for any of them
        // (ZENOH_PICO_PLAN.md section 3). A row that needed them is not scored for this framework.
    }
    signal(SIGINT, handle_sigint);

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "peer");
    char listen[128];
    snprintf(listen, sizeof(listen), "udp/224.0.0.225:7447#iface=%s", iface);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, listen);

    z_owned_session_t session;
    if (z_open(&session, z_move(config), NULL) < 0) {
        printf("RESULT: framework=zenohpico scenario=best_effort_throughput role=client error=open_failed\n");
        return 1;
    }
    zp_start_read_task(z_loan_mut(session), NULL);
    zp_start_lease_task(z_loan_mut(session), NULL);

    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, keyexpr);
    z_owned_publisher_t pub;
    z_publisher_options_t popts;
    z_publisher_options_default(&popts);
    popts.congestion_control = Z_CONGESTION_CONTROL_DROP; // the best-effort pair's promise: drop when full
    popts.is_express = express;
    if (z_declare_publisher(z_loan(session), &pub, z_loan(ke), &popts) < 0) {
        printf("RESULT: framework=zenohpico scenario=best_effort_throughput role=client error=declare_failed\n");
        return 1;
    }

    // The same payload layout as the other three: send_ns, seq, then the shape's bytes. BENCH_SAMPLE_BYTES
    // is the CDR size the generated shape reports, so the wire carries the same number of payload bytes.
    uint8_t buffer[BENCH_SAMPLE_BYTES];
    memset(buffer, 0, sizeof(buffer));

    // Let the peers find each other before the measured window, as the other harnesses do.
    for (int i = 0; i < 30 && !g_interrupted; i++) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }

    uint64_t sent = 0;
    uint64_t write_fail = 0;
    const uint64_t start = now_ns();
    const uint64_t deadline = start + (uint64_t)(duration_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        const uint64_t stamp = now_ns();
        memcpy(buffer, &stamp, sizeof(stamp));
        const uint32_t seq = (uint32_t)sent;
        memcpy(buffer + sizeof(stamp), &seq, sizeof(seq));
        z_owned_bytes_t payload;
        z_bytes_copy_from_buf(&payload, buffer, sizeof(buffer));
        if (z_publisher_put(z_loan(pub), z_move(payload), NULL) < 0) {
            write_fail++;
        } else {
            sent++;
        }
        if (interval_s > 0.0) {
            struct timespec ts = {(time_t)interval_s, (long)((interval_s - (double)(time_t)interval_s) * 1e9)};
            nanosleep(&ts, NULL);
        }
    }
    const double elapsed_s = (double)(now_ns() - start) / 1e9;

    bench_stats_end(&g_bench_stats);
    printf("RESULT: framework=zenohpico scenario=best_effort_throughput role=client sent=%lu write_fail=%lu "
           "elapsed_s=%.3f send_mbps=%.3f express=%d batch_multicast_bytes=%d %s\n",
           (unsigned long)sent, (unsigned long)write_fail, elapsed_s,
           elapsed_s > 0.0 ? (double)sent * (double)BENCH_SAMPLE_BYTES * 8.0 / elapsed_s / 1e6 : 0.0, express ? 1 : 0,
           (int)Z_BATCH_MULTICAST_SIZE,
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_SENDER, sent, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    z_drop(z_move(pub));
    z_drop(z_move(session));
    return 0;
}
