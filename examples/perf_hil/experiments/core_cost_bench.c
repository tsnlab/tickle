/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// core_cost_bench - TickLE core's own CPU cost per sample, with no kernel in the way (WIRE_PLAN.md 8,
// 2026-09-27). Built against one checkout's src/tickle.c (core_cost_ab.sh builds one per SHA), with a HAL
// of its own: sends are captured into memory and tt_get_ns() is the real clock, counted.
//
// Two nodes, a BEST_EFFORT Publisher of the p1 Bench sample on node 1 and its Subscriber on node 2,
// discovered through the captured datagrams. Then two timed phases, each one clock read pair around N
// samples:
//   send - node 1 publishes N samples, polling itself (tt_Node_poll(node, 0)) after each, as a throughput
//          client does; every datagram it sends is kept.
//   recv - node 2 is handed those datagrams one by one, as the poll loop hands it a received one.
// Prints ns per sample and tt_get_ns() calls per sample for each phase. The capture's copy is in the send
// phase of every arm alike; the kernel's work is in neither, which is the point - utime, not stime.
//
// Usage: core_cost_bench [samples] (default 200000).
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "Bench.h"
#ifndef TICKLE_C
#define TICKLE_C "../../../src/tickle.c" // core_cost_ab.sh names each build's own
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) - whitebox: the whole of core, as one unit

#define NS_PER_S 1000000000ULL
#define LINK_NET 0x0a000000U // 10.0.0.0/24, each node at 10.0.0.<id>
#define LINK_NETMASK 0xffffff00U
#define LINK_BROADCAST 0x0a0000ffU
#define PEER_PORT 7400U // any port: the nodes never bind one
#define ARENA_ALIGN 8U
#define DEFAULT_SAMPLES 200000U
#define DISCOVERY_DATAGRAMS 1000U // room kept in the capture for discovery
#define DISCOVERY_GIVE_UP_S 2U

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
    .node_id = tt_NODE_ID_INVALID,
};

// --- the HAL ---------------------------------------------------------------------------------------------

#define WIRE_MAX 400000
#define ARENA_BYTES ((size_t)WIRE_MAX * 128U) // every captured datagram's bytes, back to back
struct wire_datagram {
    uint8_t from;
    uint16_t len;
    uint32_t offset; // into arena
};
static struct wire_datagram* wire;
static uint8_t* arena;
static size_t arena_used;
static uint32_t wire_count;
static uint32_t wire_dropped; // no room left: reported, and a run that drops is not to be read
static uint8_t acting = 1;    // whose sends are being captured
static int32_t next_node_id = 1;
static uint64_t clock_calls;

uint64_t tt_get_ns(void) {
    clock_calls++;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts); // NOLINT(misc-include-cleaner) - <time.h>; as hal_linux.c
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

int32_t tt_get_node_id(void) {
    return next_node_id;
}

// One link, 10.0.0.0/24, each node at 10.0.0.<id>: tt_Node_create() refuses a broadcast no interface has.
bool tt_resolve_link(const char* broadcast, uint32_t* addr, uint32_t* netmask, uint32_t* bcast) {
    (void)broadcast;
    *addr = LINK_NET | (uint32_t)next_node_id;
    *netmask = LINK_NETMASK;
    *bcast = LINK_BROADCAST;
    return true;
}

int32_t tt_link_mtu(uint32_t addr) {
    (void)addr;
    return -1;
}

tt_ret_t tt_bind(struct tt_Node* node) {
    (void)node;
    return tt_RET_OK;
}

void tt_close(struct tt_Node* node) {
    (void)node;
}

tt_ret_t tt_wake_signal(struct tt_Node* node) {
    (void)node;
    return tt_RET_OK;
}

static int32_t capture(const void* head, size_t head_len, const void* body, size_t body_len) {
    size_t len = head_len + body_len;
    if (wire_count >= WIRE_MAX || arena_used + len > ARENA_BYTES) {
        wire_dropped++;
        return (int32_t)len;
    }
    struct wire_datagram* datagram = &wire[wire_count++];
    datagram->from = acting;
    datagram->len = (uint16_t)len;
    datagram->offset = (uint32_t)arena_used;
    memcpy(arena + arena_used, head, head_len);
    if (body_len != 0) {
        memcpy(arena + arena_used + head_len, body, body_len);
    }
    arena_used += (len + ARENA_ALIGN - 1U) & ~(size_t)(ARENA_ALIGN - 1U);
    return (int32_t)len;
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    (void)node;
    return capture(buf, len, NULL, 0);
}

int32_t tt_send_to(struct tt_Node* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    (void)node;
    (void)ip;
    (void)port;
    return capture(buf, len, NULL, 0);
}

int32_t tt_send_iov(struct tt_Node* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    (void)node;
    (void)ip;
    (void)port;
    return capture(hdr, hdr_len, body, body_len);
}

int32_t tt_send_batch(struct tt_Node* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
    (void)node;
    for (uint32_t i = 0; i < count; i++) {
        (void)capture(datagrams[i].head, datagrams[i].head_len, datagrams[i].body, datagrams[i].body_len);
    }
    return (int32_t)count;
}

// Receiving is taking the next captured datagram from the other node, as the kernel would hand it over:
// through tt_Node_poll() and its drain, so a received sample costs what it costs inside a real poll.
static struct tt_Node nodes[3];
static uint32_t cursor[3]; // the next captured datagram each node has not looked at

static int32_t take_next(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    uint8_t self = node == &nodes[1] ? 1 : 2;
    while (cursor[self] < wire_count) {
        const struct wire_datagram* datagram = &wire[cursor[self]++];
        if (datagram->from == self || datagram->len > len) {
            continue;
        }
        memcpy(buf, arena + datagram->offset, datagram->len);
        *ip = LINK_NET | datagram->from;
        *port = PEER_PORT;
        acting = self; // whatever it sends in reply is its own
        return datagram->len;
    }
    *ip = 0;
    *port = 0;
    return -1;
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    (void)timeout;
    return take_next(node, buf, len, ip, port);
}

int32_t tt_try_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    return take_next(node, buf, len, ip, port);
}

// --- the benchmark ---------------------------------------------------------------------------------------

static uint64_t received;

static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    received++;
}

// Node `to` polls until it has taken every captured datagram from the other node.
static void deliver(uint8_t receiver) {
    while (cursor[receiver] < wire_count) {
        acting = receiver;
        (void)tt_Node_poll(&nodes[receiver], 0);
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - <time.h>
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    uint32_t samples = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : DEFAULT_SAMPLES;
    if (samples + DISCOVERY_DATAGRAMS > WIRE_MAX) {
        fprintf(stderr, "at most %u samples\n", WIRE_MAX - DISCOVERY_DATAGRAMS);
        return 1;
    }
    wire = calloc(WIRE_MAX, sizeof(*wire));
    arena = malloc(ARENA_BYTES);
    if (wire == NULL || arena == NULL) {
        return 1;
    }

    for (int id = 1; id <= 2; id++) {
        next_node_id = id;
        acting = (uint8_t)id;
        if (tt_Node_create(&nodes[id]) != tt_RET_OK) {
            fprintf(stderr, "tt_Node_create(%d) failed\n", id);
            return 1;
        }
    }
    struct tt_Publisher pub;
    struct tt_Subscriber sub;
    acting = 1;
    if (tt_Node_create_publisher(&nodes[1], &pub, &BenchTopic, "bench") != tt_RET_OK) {
        return 1;
    }
    acting = 2;
    if (tt_Node_create_subscriber(&nodes[2], &sub, &BenchTopic, "bench", on_sample) != tt_RET_OK) {
        return 1;
    }

    // Discovery: both nodes poll and hear each other until the Publisher knows its Subscriber.
    uint64_t give_up = now_ns() + (DISCOVERY_GIVE_UP_S * NS_PER_S);
    while (pub.peers[0].node_id == 0 && now_ns() < give_up) {
        acting = 1;
        (void)tt_Node_poll(&nodes[1], 0);
        acting = 2;
        (void)tt_Node_poll(&nodes[2], 0);
        while (cursor[1] < wire_count || cursor[2] < wire_count) {
            deliver(1);
            deliver(2);
        }
    }
    if (pub.peers[0].node_id != 2) {
        fprintf(stderr, "no discovery (%u datagrams)\n", wire_count);
        return 1;
    }

    struct BenchData sample;
    memset(&sample, 0, sizeof(sample));

    // send
    wire_count = 0;
    arena_used = 0;
    cursor[2] = 0;
    acting = 1;
    uint64_t clock_before = clock_calls;
    uint64_t start = now_ns();
    for (uint32_t i = 0; i < samples; i++) {
        sample.seq = i + 1;
        (void)tt_Publisher_publish(&pub, (struct tt_Data*)&sample);
        (void)tt_Node_poll(&nodes[1], 0);
    }
    uint64_t send_ns = now_ns() - start;
    uint64_t send_clock = clock_calls - clock_before;
    uint32_t sent = wire_count;

    // recv
    received = 0;
    clock_before = clock_calls;
    start = now_ns();
    deliver(2);
    uint64_t recv_ns = now_ns() - start;
    uint64_t recv_clock = clock_calls - clock_before;

    printf("RESULT: samples=%u datagrams=%u received=%llu dropped=%u send_ns_per_sample=%.2f "
           "recv_ns_per_sample=%.2f send_clock_per_sample=%.3f recv_clock_per_sample=%.3f "
           "tt_version=%d\n",
           samples, sent, (unsigned long long)received, wire_dropped, (double)send_ns / samples,
           (double)recv_ns / samples, (double)send_clock / samples, (double)recv_clock / samples, tt_VERSION);
    return received == samples ? 0 : 2;
}
