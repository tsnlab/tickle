/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Shared session setup for zenoh-pico's latency harnesses, so the best-effort and reliable pairs differ only in
// their transport and their scenario name rather than in four copies of the same boilerplate.
//
// BENCH_SCENARIO and BENCH_RELIABLE are defined by the four-line wrappers in best_effort_latency/ and
// reliable_latency/. Reliable means TCP peer-to-peer, which is the only configuration where zenoh-pico's
// reliability is real - its Z_RELIABILITY_RELIABLE is monotonic sequence numbers, not retransmission, by its own
// source on both transports. Best-effort stays on UDP multicast, where both sides promise the same thing.
#ifndef ZENOH_LATENCY_SESSION_H
#define ZENOH_LATENCY_SESSION_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zenoh-pico.h>

#include "BenchStats.h"

#define PING_KEY "bench/ping"
#define PONG_KEY "bench/pong"

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

// Returns false when the endpoints a reliable run needs are absent, rather than silently running the cell over
// multicast and labelling it reliable - the failure this harness family has already had once, in a different form.
static bool latency_session_open(z_owned_session_t* session) {
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "peer");
#if !BENCH_RELIABLE
    // Multicast peer mode needs the group given explicitly - with no endpoint at all z_open() simply fails, which
    // is what the first run of these cells did on both sides. Same group, port and interface as the best-effort
    // throughput harness beside it, so the two cells differ in what they measure and not in how they are connected.
    const char* iface = getenv("BENCH_IFACE") != NULL ? getenv("BENCH_IFACE") : "eth0";
    char listen[128];
    snprintf(listen, sizeof(listen), "udp/224.0.0.225:7447#iface=%s", iface);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, listen);
#endif
#if BENCH_RELIABLE
    const char* ep_listen = getenv("BENCH_ZENOH_LISTEN");
    const char* ep_connect = getenv("BENCH_ZENOH_CONNECT");
    if (ep_listen == NULL && ep_connect == NULL) {
        printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO " role=" ROLE_NAME " error=no_endpoint\n");
        return false;
    }
    if (ep_listen != NULL) {
        zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, ep_listen);
    }
    if (ep_connect != NULL) {
        zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, ep_connect);
    }
#endif
    if (z_open(session, z_move(config), NULL) < 0) {
        printf("RESULT: framework=zenohpico scenario=" BENCH_SCENARIO " role=" ROLE_NAME " error=open_failed\n");
        return false;
    }
    zp_start_read_task(z_loan_mut(*session), NULL);
    zp_start_lease_task(z_loan_mut(*session), NULL);
    return true;
}

// The other harnesses here settle for three seconds before their measured window; a ping that starts before the
// peers have matched would count discovery as latency.
static void latency_settle(void) {
    for (int i = 0; i < 30 && !g_interrupted; i++) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
}

#if BENCH_RELIABLE
#define BENCH_TRANSPORT_FIELDS "transport=tcp reliability=tcp_stream"
#else
#define BENCH_TRANSPORT_FIELDS "transport=udp_multicast reliability=best_effort"
#endif

#endif
