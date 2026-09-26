/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// core_cost_socket - core_cost_bench's "-c -R" loop through the real HAL (WIRE_PLAN.md 8.8, 2026-09-27): two nodes
// in one thread, on real UDP sockets (hal_linux.c), linked as any application links TickLE - no whitebox. Node 1
// has a RELIABLE KEEP_LAST 64 Publisher of the p1 Bench sample and publishes it from an entry that reschedules
// itself under tt_Node_poll(-1), as the campaign's throughput clients do; node 2 has the RELIABLE Subscriber.
//
// In rounds: node 1 sends ROUND samples (and takes whatever ACKNACKs have come back), then node 2 takes them off
// its socket. Per phase: wall time, and the thread's user and system time (getrusage(RUSAGE_THREAD)) - the
// campaign's metric - per sample.
//
// Needs an interface whose broadcast address is BENCH_BROADCAST (the Pi: 192.168.10.255; the PC: a private netns
// with one), because tt_Node_create() refuses a broadcast no interface has. Data goes node to node over the
// host's own address; discovery broadcasts leave on that interface.
//
// Usage: BENCH_BROADCAST=<addr> core_cost_socket [samples] (default 400000)
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/resource.h> // getrusage(RUSAGE_THREAD)
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "Bench.h"

#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000ULL
#define DEFAULT_SAMPLES 400000U
#define ROUND 512U        // samples sent before the reader takes them: well inside the socket's receive buffer
#define DEPTH 64U         // the Q2 cell's -K 64
#define POLL_NS 1000000LL // the reader's wait for a datagram, 1 ms
#define DISCOVERY_GIVE_UP_S 5U
#define ROUND_GIVE_UP_S 2U
#define ARG_BASE 10

static struct tt_Node writer;
static struct tt_Node reader;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static uint64_t received;
static uint32_t left_to_send;
static struct BenchData sample;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - <time.h>
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static void thread_cpu_ns(uint64_t* user, uint64_t* sys) {
    struct rusage usage;              // NOLINT(misc-include-cleaner) - <sys/resource.h>
    getrusage(RUSAGE_THREAD, &usage); // NOLINT(misc-include-cleaner) - <sys/resource.h>
    *user = ((uint64_t)usage.ru_utime.tv_sec * NS_PER_S) + ((uint64_t)usage.ru_utime.tv_usec * NS_PER_US);
    *sys = ((uint64_t)usage.ru_stime.tv_sec * NS_PER_S) + ((uint64_t)usage.ru_stime.tv_usec * NS_PER_US);
}

static void on_sample(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
    received++;
}

// The campaign clients' send_one(): publish, then run again at once.
static void send_one(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (left_to_send == 0) {
        return;
    }
    left_to_send--;
    sample.seq++;
    sample.send_ns = tt_get_ns();
    (void)tt_Publisher_publish(&pub, (struct tt_Data*)&sample);
    (void)tt_Node_schedule(node, time, send_one, NULL);
}

static bool set_up(void) {
    static struct tt_ReliableCacheIndex cache_index[DEPTH];
    static uint8_t
        cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(DEPTH, tt_RELIABLE_RECORD_BYTES(sizeof(struct BenchData)))];
    static struct tt_ReliableCache cache;
    _tt_CONFIG.node_id = 1;
    if (tt_Node_create(&writer) != tt_RET_OK) {
        return false;
    }
    _tt_CONFIG.node_id = 2;
    if (tt_Node_create(&reader) != tt_RET_OK) {
        return false;
    }
    if (tt_Node_create_publisher(&writer, &pub, &BenchTopic, "bench") != tt_RET_OK ||
        tt_Node_create_subscriber(&reader, &sub, &BenchTopic, "bench", on_sample) != tt_RET_OK) {
        return false;
    }
    cache.index = cache_index;
    cache.capacity = DEPTH;
    cache.depth = DEPTH;
    cache.arena = cache_arena;
    cache.arena_size = sizeof(cache_arena);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    sub.reliable = true;
    uint64_t give_up = now_ns() + (DISCOVERY_GIVE_UP_S * NS_PER_S);
    while (pub.peers[0].node_id != 2 && now_ns() < give_up) {
        (void)tt_Node_poll(&writer, POLL_NS);
        (void)tt_Node_poll(&reader, POLL_NS);
    }
    return pub.peers[0].node_id == 2;
}

struct totals {
    uint64_t wall;
    uint64_t user;
    uint64_t sys;
};

int main(int argc, char** argv) {
    uint32_t samples = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, ARG_BASE) : DEFAULT_SAMPLES;
    const char* broadcast = getenv("BENCH_BROADCAST");
    if (broadcast == NULL) {
        fprintf(stderr, "BENCH_BROADCAST must name the broadcast address of an interface on this host\n");
        return 1;
    }
    _tt_CONFIG.broadcast = (char*)broadcast;
    if (!set_up()) {
        fprintf(stderr, "no node, or no discovery\n");
        return 1;
    }

    struct totals send = {0};
    struct totals recv = {0};
    uint32_t sent = 0;
    bool stalled = false;
    while (sent < samples && !stalled) {
        uint32_t round = samples - sent < ROUND ? samples - sent : ROUND;
        uint64_t user0 = 0;
        uint64_t sys0 = 0;
        uint64_t user1 = 0;
        uint64_t sys1 = 0;
        thread_cpu_ns(&user0, &sys0);
        uint64_t start = now_ns();
        left_to_send = round;
        (void)tt_Node_schedule(&writer, tt_get_ns(), send_one, NULL);
        while (left_to_send > 0) {
            (void)tt_Node_poll(&writer, -1);
        }
        (void)tt_Node_poll(&writer, 0); // ACKNACKs back since the last round
        send.wall += now_ns() - start;
        thread_cpu_ns(&user1, &sys1);
        send.user += user1 - user0;
        send.sys += sys1 - sys0;
        sent += round;

        start = now_ns();
        uint64_t give_up = start + (ROUND_GIVE_UP_S * NS_PER_S);
        while (received < sent && now_ns() < give_up) {
            (void)tt_Node_poll(&reader, POLL_NS);
        }
        stalled = received < sent;
        recv.wall += now_ns() - start;
        thread_cpu_ns(&user0, &sys0);
        recv.user += user0 - user1;
        recv.sys += sys0 - sys1;
    }

    printf("RESULT: samples=%u received=%llu stalled=%d send_ns_per_sample=%.2f recv_ns_per_sample=%.2f "
           "send_utime_ns_per_sample=%.2f send_stime_ns_per_sample=%.2f recv_utime_ns_per_sample=%.2f "
           "recv_stime_ns_per_sample=%.2f tx_datagrams=%llu tt_version=%d\n",
           sent, (unsigned long long)received, stalled ? 1 : 0, (double)send.wall / sent, (double)recv.wall / sent,
           (double)send.user / sent, (double)send.sys / sent, (double)recv.user / sent, (double)recv.sys / sent,
           (unsigned long long)writer.tx_datagrams, tt_VERSION);
    return stalled ? 2 : 0;
}
