/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// zenoh-pico's RELIABLE cell, over TCP - the only configuration where its reliability is real.
//
// Over UDP it does not repair loss, by its own TODO on both transports (see this directory's best_effort twin and
// rmw_tickle/ZENOH_PICO_PLAN.md), so a reliable row there would be a label rather than a behaviour. Over TCP the repair
// is TCP's: no sample is lost, but by head-of-line blocking rather than by per-datagram repair, which is a different
// mechanism from ours and is named in the row rather than hidden by it.
//
// Peer mode with an explicit endpoint, so no router is involved: the subscriber listens and the publisher connects,
// both given by BENCH_ZENOH_LISTEN / BENCH_ZENOH_CONNECT. The user asked for this configuration on 2026-09-29.
//
// Z_CONGESTION_CONTROL_BLOCK, not DROP: the reliable cell's promise on our side is KEEP_ALL with a max_blocking_time -
// block rather than discard when the link is full - and BLOCK is the same choice in zenoh's vocabulary.
// zenoh-pico's subscriber for the reliable throughput cell - the twin of this directory's client, and
// shaped like the cyclonedds and fastdds servers so one parser reads all four RESULT lines.
//
// Loss is computed the way the other harnesses compute it, from the sender's own sequence numbers rather
// than from anything the middleware reports: highest seq seen plus one is what was sent by the time the
// last arrival happened, and received is what arrived. That keeps the figure comparable across frameworks
// that count internally in different ways - and for zenoh-pico it is the only source, since it does not
// repair loss and therefore has nothing to report about it (ZENOH_PICO_PLAN.md section 1).
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

static uint64_t g_received = 0;
static uint32_t g_highest_seq = 0;
static bool g_any = false;
static uint64_t g_latency_sum_ns = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void on_sample(z_loaned_sample_t* sample, void* arg) {
    (void)arg;
    const z_loaned_bytes_t* payload = z_sample_payload(sample);
    uint8_t head[12];
    z_bytes_reader_t reader = z_bytes_get_reader(payload);
    if (z_bytes_reader_read(&reader, head, sizeof(head)) != sizeof(head)) {
        return; // too short to carry a stamp and a sequence number: not one of ours
    }
    uint64_t stamp = 0;
    uint32_t seq = 0;
    memcpy(&stamp, head, sizeof(stamp));
    memcpy(&seq, head + sizeof(stamp), sizeof(seq));
    g_received++;
    // One-way delay is only meaningful when the two clocks are the same one, which they are not across
    // hosts - so it is summed here and reported, and the campaign reads it only for a same-host cell.
    const uint64_t arrived = now_ns();
    if (arrived > stamp) {
        g_latency_sum_ns += arrived - stamp;
    }
    if (!g_any || seq > g_highest_seq) {
        g_highest_seq = seq;
        g_any = true;
    }
}

#define ROLE_NAME "server"

int main(int argc, char** argv) {
    bench_stats_begin(&g_bench_stats);
    double duration_s = 20.0;
    const char* iface = getenv("BENCH_IFACE") != NULL ? getenv("BENCH_IFACE") : "eth0";
    const char* keyexpr = "bench/stream";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        }
    }
    signal(SIGINT, handle_sigint);

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "peer");
    // One side listens, the other connects - peer mode, no router. Defaults keep a lone run from silently becoming a
    // different configuration: an endpoint that is not set is an error rather than a fallback to multicast.
    const char* ep_listen = getenv("BENCH_ZENOH_LISTEN");
    const char* ep_connect = getenv("BENCH_ZENOH_CONNECT");
    if (ep_listen == NULL && ep_connect == NULL) {
        printf("RESULT: framework=zenohpico scenario=reliable_throughput role=%s error=no_endpoint\n", ROLE_NAME);
        return 1;
    }
    if (ep_listen != NULL) {
        zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, ep_listen);
    }
    if (ep_connect != NULL) {
        zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, ep_connect);
    }
    (void)iface;

    z_owned_session_t session;
    if (z_open(&session, z_move(config), NULL) < 0) {
        printf("RESULT: framework=zenohpico scenario=reliable_throughput role=server error=open_failed\n");
        return 1;
    }
    zp_start_read_task(z_loan_mut(session), NULL);
    zp_start_lease_task(z_loan_mut(session), NULL);

    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, keyexpr);
    z_owned_closure_sample_t callback;
    z_closure(&callback, on_sample, NULL, NULL);
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(session), &sub, z_loan(ke), z_move(callback), NULL) < 0) {
        printf("RESULT: framework=zenohpico scenario=reliable_throughput role=server error=declare_failed\n");
        return 1;
    }

    const uint64_t deadline = now_ns() + (uint64_t)(duration_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
    }

    // sent_by_sender is the sender's own count as far as this receiver can tell: the highest sequence
    // number it saw, plus one. A receiver that saw nothing reports 0 rather than guessing.
    const uint64_t sent_by_sender = g_any ? (uint64_t)g_highest_seq + 1 : 0;
    const uint64_t lost = sent_by_sender > g_received ? sent_by_sender - g_received : 0;
    bench_stats_end(&g_bench_stats);
    printf("RESULT: framework=zenohpico scenario=reliable_throughput role=server received=%lu "
           "sent_by_sender=%lu lost=%lu loss_pct=%.3f mean_owd_ns=%lu %s\n",
           (unsigned long)g_received, (unsigned long)sent_by_sender, (unsigned long)lost,
           sent_by_sender > 0 ? 100.0 * (double)lost / (double)sent_by_sender : 0.0,
           (unsigned long)(g_received > 0 ? g_latency_sum_ns / g_received : 0),
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_RECEIVER, g_received, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    z_drop(z_move(sub));
    z_drop(z_move(session));
    return 0;
}
