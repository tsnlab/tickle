/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ring_starvation_repro.c - the core shape of the rig's rmw Array4k BEST_EFFORT starvation (commit 56565720's
// message), without ROS: a publisher whose sending threads are not its polling thread, publishing at max rate from
// before it knows any subscriber. Until it learns one it broadcasts, its own broadcasts come back to its well-known
// socket, and its polling thread can stay inside one socket drain for as long as the stream lasts - then the
// subscriber's directed announce, written into this context's segment, is never read and the whole run stays on UDP.
//
// Run with drain_ring_cost.sh (MODE=repro), which starts the best_effort_throughput server a little after this
// publisher, in one private network namespace. Prints one line:
//   REPRO: threads= sent= tx_udp= tx_shm= tx_shm_share= rx_self_sent= rx_shm= rx_drain_ring_turns=
//   shm_segments_created=
// Built against the core of the arm under test, with one payload shape's Bench (examples/perf_hil/tickle/common/pN).
//
// Usage: ring_starvation_repro [-d SECONDS] [-t THREADS]   (defaults 6 s, 1 publishing thread). More publishing
// threads refill the socket faster than the one polling thread reads it - the rig's Pi reads slower than it writes,
// and this PC needs the help.

#include <pthread.h>
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

#define MAX_PUBLISHERS 16

static volatile bool g_stop = false;
static struct tt_Publisher g_pub;
#define DEFAULT_DURATION_S 6.0
static double g_duration_s = DEFAULT_DURATION_S;
static uint64_t g_sent = 0;

static void* poll_thread(void* arg) {
    struct tt_Context* node = arg;
    const int64_t slice_ns = 100LL * 1000 * 1000;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        tt_ret_t ret = tt_Context_poll(node, slice_ns);
        if (ret != tt_RET_OK && ret != tt_RET_TIMEOUT && ret != tt_RET_INTERRUPTED) {
            fprintf(stderr, "poll failed: %d\n", ret);
            break;
        }
    }
    return NULL;
}

// Max rate from the first moment, before any subscriber is known: the rig's perf_test at -r 0.
static void* publish_thread(void* arg) {
    (void)arg;
    uint32_t seq = 0;
    uint64_t sent = 0;
    const uint64_t deadline = tt_get_ns() + (uint64_t)(g_duration_s * (double)tt_SECOND);
    while (tt_get_ns() < deadline) {
        struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
        if (tt_Publisher_publish(&g_pub, (struct tt_Data*)&msg) == tt_RET_OK) {
            sent++;
        }
    }
    __atomic_add_fetch(&g_sent, sent, __ATOMIC_RELAXED);
    return NULL;
}

int main(int argc, char** argv) {
    int threads = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            g_duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        }
    }
    if (threads < 1 || threads > MAX_PUBLISHERS) {
        printf("REPRO: error=threads\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    _tt_CONFIG.broadcast = "192.168.10.255"; // the bench link's, as every perf_hil harness sets it

    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("REPRO: error=create\n");
        return 1;
    }
    if (tt_Context_create_publisher(&node, &g_pub, &BenchTopic, "stream") != tt_RET_OK) {
        printf("REPRO: error=publisher\n");
        return 1;
    }
    pthread_t poller; // NOLINT(misc-include-cleaner) - <pthread.h> above
    if (pthread_create(&poller, NULL, poll_thread, &node) != 0) {
        printf("REPRO: error=thread\n");
        return 1;
    }
    pthread_t publishers[MAX_PUBLISHERS]; // NOLINT(misc-include-cleaner) - <pthread.h> above
    for (int i = 0; i < threads; i++) {
        if (pthread_create(&publishers[i], NULL, publish_thread, NULL) != 0) {
            printf("REPRO: error=thread\n");
            return 1;
        }
    }
    for (int i = 0; i < threads; i++) {
        pthread_join(publishers[i], NULL);
    }
    __atomic_store_n(&g_stop, true, __ATOMIC_RELEASE);
    (void)tt_Context_interrupt(&node);
    pthread_join(poller, NULL);

    const uint64_t udp = node.tx_datagrams_by_transport[tt_TRANSPORT_UDP];
    const uint64_t shm = node.tx_datagrams_by_transport[tt_TRANSPORT_SHM];
    // rx_drain_ring_turns only exists in a build with the ring-turn rule; the harness passes -DREPRO_RING_TURNS then.
    char turns[32];
#ifdef REPRO_RING_TURNS
    snprintf(turns, sizeof turns, "%llu", (unsigned long long)node.rx_drain_ring_turns);
#else
    snprintf(turns, sizeof turns, "absent");
#endif
    printf("REPRO: threads=%d sent=%llu tx_udp=%llu tx_shm=%llu tx_shm_share=%.3f rx_self_sent=%llu rx_shm=%llu "
           "rx_drain_ring_turns=%s shm_segments_created=%llu\n",
           threads, (unsigned long long)g_sent, (unsigned long long)udp, (unsigned long long)shm,
           udp + shm > 0 ? (double)shm / (double)(udp + shm) : 0.0, (unsigned long long)node.rx_self_sent,
           (unsigned long long)node.rx_datagrams_by_transport[tt_TRANSPORT_SHM], turns,
           (unsigned long long)node.segments_created);
    tt_Context_destroy(&node);
    return 0;
}
