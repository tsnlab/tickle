/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The pong half: subscribe to the ping key and republish the identical bytes on the pong key, exactly as the other
// three harnesses' servers do. The payload is echoed unchanged so the client's own timestamp comes back to it and
// no clock is shared between the two machines - the round trip is measured entirely on the client's clock.
#ifndef ZENOH_LATENCY_SERVER_IMPL_H
#define ZENOH_LATENCY_SERVER_IMPL_H

#define ROLE_NAME "server"
#include "session.h"

static z_owned_publisher_t g_pong_pub;
static uint64_t g_echoed = 0;

static void on_ping(z_loaned_sample_t* sample, void* arg) {
    (void)arg;
    uint8_t buffer[BENCH_SAMPLE_BYTES];
    z_bytes_reader_t reader = z_bytes_get_reader(z_sample_payload(sample));
    const size_t got = z_bytes_reader_read(&reader, buffer, sizeof(buffer));
    if (got == 0) {
        return;
    }
    z_owned_bytes_t payload;
    z_bytes_copy_from_buf(&payload, buffer, got);
    if (z_publisher_put(z_loan(g_pong_pub), z_move(payload), NULL) >= 0) {
        g_echoed++;
    }
}

int main(int argc, char** argv) {
    bench_stats_begin(&g_bench_stats);
    double duration_s = 20.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
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

    z_publisher_options_t popts;
    z_publisher_options_default(&popts);
    popts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
#ifdef Z_FEATURE_UNSTABLE_API
    popts.reliability = BENCH_RELIABLE ? Z_RELIABILITY_RELIABLE : Z_RELIABILITY_BEST_EFFORT;
#endif
    if (z_declare_publisher(z_loan(session), &g_pong_pub, z_loan(pong_ke), &popts) < 0) {
        printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO " role=server error=declare_failed\n");
        return 1;
    }
    z_owned_closure_sample_t callback;
    z_closure(&callback, on_ping, NULL, NULL);
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(session), &sub, z_loan(ping_ke), z_move(callback), NULL) < 0) {
        printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO " role=server error=declare_failed\n");
        return 1;
    }

    const uint64_t deadline = now_ns() + (uint64_t)(duration_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
    }

    bench_stats_end(&g_bench_stats);
    // echoed is what this side actually sent back. A run where it is 0 means the pong half never ran, which is the
    // failure that has already cost this harness family two measurements - so it is on the line, not inferred.
    printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO
           " role=server received=%lu echoed=%lu " BENCH_TRANSPORT_FIELDS " %s\n",
           (unsigned long)g_echoed, (unsigned long)g_echoed,
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_RECEIVER, g_echoed, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    z_drop(z_move(sub));
    z_drop(z_move(g_pong_pub));
    z_drop(z_move(session));
    return 0;
}

#endif
