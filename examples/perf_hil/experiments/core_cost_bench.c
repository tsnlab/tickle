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
//   send - node 1 publishes N samples, polling itself (tt_Context_poll(node, 0)) after each, as a throughput
//          client does; every datagram it sends is kept.
//   recv - node 2 is handed those datagrams one by one, as the poll loop hands it a received one.
// Prints ns per sample and tt_get_ns() calls per sample for each phase. The capture's copy is in the send
// phase of every arm alike; the kernel's work is in neither, which is the point - utime, not stime.
//
// Usage: core_cost_bench [samples] (default 200000).
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h> // cpu_set_t, sched_getaffinity(), pthread_setaffinity_np()
#include <stdatomic.h>
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
    .addr = _tt_CONTEXT_ADDRESS,
    .port = _tt_CONTEXT_PORT,
    .broadcast = _tt_CONTEXT_BROADCAST,
    .context_id = tt_CONTEXT_ID_INVALID,
};

// --- the HAL ---------------------------------------------------------------------------------------------

#define WIRE_MAX 400000
#define ARENA_BYTES ((size_t)WIRE_MAX * 128U) // every captured datagram's bytes, back to back
struct wire_datagram {
    uint8_t from;
    uint8_t to; // a node id, or 0 for everyone (a broadcast)
    uint16_t len;
    uint32_t offset; // into arena
};
static struct wire_datagram* wire;
static uint8_t* arena;
static size_t arena_used;
static uint32_t wire_count;
static uint64_t wire_bytes;   // what those datagrams weigh on the wire, headers included (WIRE_PLAN.md 9.1 step 3)
static uint32_t wire_dropped; // no room left: reported, and a run that drops is not to be read
static uint8_t acting = 1;    // whose sends are being captured
static int32_t next_node_id = 1;
static _Thread_local uint64_t clock_calls; // per thread: -p's publisher does not count in the phases
static _Thread_local bool discard_sends;   // -p's publisher: its datagrams are not captured

uint64_t tt_get_ns(void) {
    clock_calls++;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts); // NOLINT(misc-include-cleaner) - <time.h>; as hal_linux.c
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

uint64_t tt_timer_resolution_ns(void) {
    return 1;
}

int32_t tt_get_node_id(void) {
    return next_node_id;
}

// One link, 10.0.0.0/24, each node at 10.0.0.<id>: tt_Context_create() refuses a broadcast no interface has.
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

tt_ret_t tt_bind(struct tt_Context* node) {
    (void)node;
    return tt_RET_OK;
}

void tt_close(struct tt_Context* node) {
    (void)node;
}

tt_ret_t tt_wake_signal(struct tt_Context* node) {
    (void)node;
    return tt_RET_OK;
}

static uint8_t destination(uint32_t ip) {
    return (ip & LINK_NETMASK) == LINK_NET && ip != LINK_BROADCAST ? (uint8_t)(ip & 0xffU) : 0;
}

static int32_t capture(uint32_t ip, const void* head, size_t head_len, const void* body, size_t body_len) {
    size_t len = head_len + body_len;
    if (discard_sends) {
        return (int32_t)len;
    }
    if (wire_count >= WIRE_MAX || arena_used + len > ARENA_BYTES) {
        wire_dropped++;
        return (int32_t)len;
    }
    struct wire_datagram* datagram = &wire[wire_count++];
    wire_bytes += len;
    datagram->from = acting;
    datagram->to = destination(ip);
    datagram->len = (uint16_t)len;
    datagram->offset = (uint32_t)arena_used;
    memcpy(arena + arena_used, head, head_len);
    if (body_len != 0) {
        memcpy(arena + arena_used + head_len, body, body_len);
    }
    arena_used += (len + ARENA_ALIGN - 1U) & ~(size_t)(ARENA_ALIGN - 1U);
    return (int32_t)len;
}

#if tt_SEGMENT_ENABLED
// The segment's HAL entry points (SHM_PLAN.md stage 1). Refusing to create and reporting every peer
// as absent is the honest stub here: this binary has no shared memory and every send goes over its
// own transport, so the module must decide "not same host" rather than be half-present.
void* tt_segment_create(const char* path, size_t bytes) {
    (void)path;
    (void)bytes;
    return NULL;
}

void* tt_segment_attach(const char* path, size_t bytes, uint8_t* why) {
    (void)path;
    (void)bytes;
    *why = (uint8_t)tt_SEGMENT_ABSENT;
    return NULL;
}

void tt_segment_detach(void* mapping, size_t bytes) {
    (void)mapping;
    (void)bytes;
}

void tt_segment_unlink(const char* path) {
    (void)path;
}
#endif

int32_t tt_send(struct tt_Context* node, const void* buf, size_t len) {
    (void)node;
    return capture(0, buf, len, NULL, 0);
}

int32_t tt_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    (void)node;
    (void)port;
    return capture(ip, buf, len, NULL, 0);
}

int32_t tt_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    (void)node;
    (void)port;
    return capture(ip, hdr, hdr_len, body, body_len);
}

int32_t tt_send_batch(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
    (void)node;
    for (uint32_t i = 0; i < count; i++) {
        (void)capture(datagrams[i].ip, datagrams[i].head, datagrams[i].head_len, datagrams[i].body,
                      datagrams[i].body_len);
    }
    return (int32_t)count;
}

// Receiving is taking the next captured datagram from the other node, as the kernel would hand it over:
// through tt_Context_poll() and its drain, so a received sample costs what it costs inside a real poll.
// Writer nodes 1..W, the reader node W+1. BENCH_MAX_WRITERS raises it (WIRE_PLAN.md 9.1's 32-writer cases), with
// tt_MAX_PEER_COUNT and tt_MAX_DISCOVERED_ENTITIES raised to match in every arm alike (core_cost_ab.sh BENCH_CFLAGS).
#ifndef BENCH_MAX_WRITERS
#define BENCH_MAX_WRITERS 8
#endif
#define MAX_WRITERS BENCH_MAX_WRITERS
static struct tt_Context nodes[MAX_WRITERS + 2];
static uint32_t cursor[MAX_WRITERS + 2]; // the next captured datagram each node has not looked at

static int32_t take_next(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    uint8_t self = (uint8_t)(node - nodes);
    while (cursor[self] < wire_count) {
        const struct wire_datagram* datagram = &wire[cursor[self]++];
        if (datagram->from == self || (datagram->to != 0 && datagram->to != self) || datagram->len > len) {
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

// All the captured datagrams stand in for a batch already read (an upper bound: some are the node's own,
// which take_next() skips). Defined for every build; only those with OPTIMIZATION_PLAN.md 11.4's D4 call it.
uint32_t tt_rx_buffered(const struct tt_Context* node) {
    uint8_t self = (uint8_t)(node - nodes);
    return cursor[self] < wire_count ? wire_count - cursor[self] : 0U;
}

int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    (void)timeout;
    return take_next(node, buf, len, ip, port);
}

int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    return take_next(node, buf, len, ip, port);
}

// --- the benchmark ---------------------------------------------------------------------------------------

#define MAX_EXTRA 14 // unrelated Publishers on writer node 1: entries in the reader's discovery table
#define EXTRA_NAME_BYTES 8
#define RELIABLE_DEPTH 64U // -R's KEEP_LAST cache, in samples: the Q2 cell's -K 64
#define RELIABLE_ROUND 32U // -R: samples published before the reader takes them and its ACKNACKs come back
#define MICRO_ITERATIONS 1000000U
#define ARG_BASE 10

static uint64_t received;
static volatile uint64_t sink; // keeps the micro loops' results alive

static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    received++;
}

// Node `receiver` polls until it has taken every captured datagram from the others.
static void deliver(uint8_t receiver) {
    while (cursor[receiver] < wire_count) {
        acting = receiver;
        (void)tt_Context_poll(&nodes[receiver], 0);
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - <time.h>
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static void count_visit(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx) {
    (void)node;
    (void)endpoint;
    (*(uint64_t*)ctx)++;
}

// -c: the throughput clients' send_one() - publish, then schedule itself again at once - on writer 1.
static struct tt_Publisher* client_pub;
static uint32_t client_left;
static struct BenchData client_sample;
static void client_send(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    if (client_left == 0) {
        return;
    }
    client_left--;
    client_sample.seq++;
    client_sample.send_ns = tt_get_ns(); // as the harness stamps it
    (void)tt_Publisher_publish(client_pub, (struct tt_Data*)&client_sample);
    (void)tt_Context_schedule(node, time, client_send, NULL);
}

struct options {
    uint32_t samples;
    uint32_t writers;
    uint32_t extra;
    bool discovery;
    bool micro;
    bool client; // publish from a self-rescheduling entry under tt_Context_poll(-1), as the throughput clients do
    bool concurrent_publisher; // -p: another thread publishes on the reader node through the receive phase
    bool reliable; // -R: RELIABLE KEEP_LAST 64, as the Q2 cell (c9); send and receive alternate in rounds - or, with
                   // -c, the clients' own loop (send_one rescheduling itself under tt_Context_poll(-1)) on that writer
};

static bool parse(int argc, char** argv, struct options* opt) {
    *opt = (struct options) {DEFAULT_SAMPLES, 1, 0, false, false, false, false, false};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            opt->samples = (uint32_t)strtoul(argv[++i], NULL, ARG_BASE);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            opt->writers = (uint32_t)strtoul(argv[++i], NULL, ARG_BASE);
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            opt->extra = (uint32_t)strtoul(argv[++i], NULL, ARG_BASE);
        } else if (strcmp(argv[i], "-D") == 0) {
            opt->discovery = true;
        } else if (strcmp(argv[i], "-m") == 0) {
            opt->micro = true;
        } else if (strcmp(argv[i], "-R") == 0) {
            opt->reliable = true;
        } else if (strcmp(argv[i], "-p") == 0) {
            opt->concurrent_publisher = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            opt->client = true;
        } else if (i == 1 && argv[i][0] != '-') {
            opt->samples = (uint32_t)strtoul(argv[i], NULL, ARG_BASE); // the first form: a bare sample count
        } else {
            return false;
        }
    }
    return opt->writers >= 1 && opt->writers <= MAX_WRITERS && opt->extra <= MAX_EXTRA &&
           opt->samples + DISCOVERY_DATAGRAMS <= WIRE_MAX;
}

// Each stage of a received sample's path, alone, in a loop: ns per call. Isolation, not bracketing - hot
// caches and a trained predictor - so it ranks the stages and bounds them from below.
static void micro(uint8_t reader, struct tt_Subscriber* sub, const struct tt_Publisher* last_writer,
                  uint8_t writer_node) {
    struct tt_Context* node = &nodes[reader];
    uint32_t endpoint_id = ((struct tt_Endpoint*)sub)->id;
    uint32_t entity_id = ((const struct tt_Endpoint*)last_writer)->entity_id;
    uint64_t hits = 0;

    uint64_t start = now_ns();
    for (uint32_t i = 0; i < MICRO_ITERATIONS; i++) {
        for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id, count_visit, &hits);
    }
    double lookup = (double)(now_ns() - start) / MICRO_ITERATIONS;

    start = now_ns();
    for (uint32_t i = 0; i < MICRO_ITERATIONS; i++) {
        hits += find_writer_proxy(sub, writer_node, entity_id) != NULL;
    }
    double proxy = (double)(now_ns() - start) / MICRO_ITERATIONS;

    start = now_ns();
    for (uint32_t i = 0; i < MICRO_ITERATIONS; i++) {
        hits += subscriber_incompatible_with_writer(node, sub, writer_node, endpoint_id, entity_id);
    }
    double rxo = (double)(now_ns() - start) / MICRO_ITERATIONS;

    struct BenchData sample;
    memset(&sample, 0, sizeof(sample));
    uint8_t cdr[sizeof(struct BenchData)];
    int32_t cdr_len = BenchData_encode(&sample, cdr, sizeof(cdr));
    struct BenchData decoded;
    start = now_ns();
    for (uint32_t i = 0; i < MICRO_ITERATIONS; i++) {
        hits += (uint64_t)BenchData_decode(&decoded, cdr, (uint32_t)cdr_len, true);
    }
    double decode = (double)(now_ns() - start) / MICRO_ITERATIONS;

    start = now_ns();
    for (uint32_t i = 0; i < MICRO_ITERATIONS; i++) {
        record_delivery_order(node, sub, i + 1, i, writer_node, entity_id, false);
    }
    double order = (double)(now_ns() - start) / MICRO_ITERATIONS;

    start = now_ns();
    for (uint32_t i = 0; i < MICRO_ITERATIONS; i++) {
        acting = reader;
        (void)tt_Context_poll(node, 0); // nothing to take: the poll's own fixed cost
    }
    double poll = (double)(now_ns() - start) / MICRO_ITERATIONS;

    start = now_ns();
    for (uint32_t i = 0; i < MICRO_ITERATIONS; i++) {
        state_lock(node);
        state_unlock(node);
    }
    double lock = (double)(now_ns() - start) / MICRO_ITERATIONS;

    start = now_ns();
    for (uint32_t i = 0; i < MICRO_ITERATIONS; i++) {
        hits += tt_get_ns();
    }
    double clock = (double)(now_ns() - start) / MICRO_ITERATIONS;

    sink = hits;
    printf("MICRO: endpoint_lookup_ns=%.2f writer_proxy_ns=%.2f rxo_check_ns=%.2f decode_ns=%.2f "
           "delivery_order_ns=%.2f empty_poll_ns=%.2f lock_pair_ns=%.2f clock_ns=%.2f\n",
           lookup, proxy, rxo, decode, order, poll, lock, clock);
}

static struct tt_Publisher pubs[MAX_WRITERS + 1];
static struct tt_Subscriber sub;

// Writer nodes 1..W with a Publisher each, `extra` more Publishers on node 1, the reader node W+1 with the
// Subscriber (and a discovery table, with -D).
static bool set_up(const struct options* opt, uint8_t reader) {
    for (uint8_t id = 1; id <= reader; id++) {
        next_node_id = id;
        acting = id;
        if (tt_Context_create(&nodes[id]) != tt_RET_OK) {
            fprintf(stderr, "tt_Context_create(%d) failed\n", id);
            return false;
        }
    }
    static struct tt_Discovery discovery;
    if (opt->discovery && tt_Context_set_discovery(&nodes[reader], &discovery, NULL, NULL) != tt_RET_OK) {
        return false;
    }
    for (uint8_t id = 1; id < reader; id++) {
        acting = id;
        if (tt_Context_create_publisher(&nodes[id], &pubs[id], &BenchTopic, "bench") != tt_RET_OK) {
            return false;
        }
    }
    static struct tt_Publisher extra[MAX_EXTRA];
    static char extra_names[MAX_EXTRA][EXTRA_NAME_BYTES];
    for (uint32_t i = 0; i < opt->extra; i++) {
        acting = 1;
        (void)snprintf(extra_names[i], sizeof(extra_names[i]), "x%u", i % MAX_EXTRA);
        if (tt_Context_create_publisher(&nodes[1], &extra[i], &BenchTopic, extra_names[i]) != tt_RET_OK) {
            return false;
        }
    }
    acting = reader;
    if (tt_Context_create_subscriber(&nodes[reader], &sub, &BenchTopic, "bench", on_sample) != tt_RET_OK) {
        return false;
    }
    if (opt->reliable) {
        // As reliable_throughput/client.c at Q2 (-K 64): a KEEP_LAST cache on writer 1, a RELIABLE Subscriber - set
        // before discovery, so the announces carry them. Not KEEP_ALL: its acknowledgement requests are rate-limited
        // in time, and a loop this fast would spend itself refused.
        static struct tt_ReliableCacheIndex cache_index[RELIABLE_DEPTH];
        static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(RELIABLE_DEPTH,
                                                                 tt_RELIABLE_RECORD_BYTES(sizeof(struct BenchData)))];
        static struct tt_ReliableCache cache;
        cache.index = cache_index;
        cache.capacity = RELIABLE_DEPTH;
        cache.depth = RELIABLE_DEPTH;
        cache.arena = cache_arena;
        cache.arena_size = sizeof(cache_arena);
        pubs[1].reliable_cache = &cache;
        pubs[1].reliable = true;
        pubs[1].keep_all = false;
        sub.reliable = true;
    }
    return true;
}

// Every node polls and hears the others until each Publisher knows the Subscriber.
static bool discover(uint8_t reader) {
    uint64_t give_up = now_ns() + (DISCOVERY_GIVE_UP_S * NS_PER_S);
    while (now_ns() < give_up) {
        for (uint8_t id = 1; id <= reader; id++) {
            acting = id;
            (void)tt_Context_poll(&nodes[id], 0);
        }
        bool pending = true;
        while (pending) {
            pending = false;
            for (uint8_t id = 1; id <= reader; id++) {
                deliver(id);
                pending = pending || cursor[id] < wire_count;
            }
        }
        bool matched = true;
        for (uint8_t id = 1; id < reader; id++) {
            matched = matched && pubs[id].peers[0].context_id == reader;
        }
        if (matched) {
            return true;
        }
    }
    fprintf(stderr, "no discovery (%u datagrams)\n", wire_count);
    return false;
}

// The send phase: the writers take turns, one sample each, polling after it - or, with -c, writer 1's
// self-rescheduling entry under tt_Context_poll(-1).
static void send_all(const struct options* opt) {
    if (opt->client) {
        client_pub = &pubs[1];
        client_left = opt->samples;
        acting = 1;
        (void)tt_Context_schedule(&nodes[1], tt_get_ns(), client_send, NULL);
        while (client_left > 0) {
            (void)tt_Context_poll(&nodes[1], -1);
        }
        return;
    }
    struct BenchData sample;
    memset(&sample, 0, sizeof(sample));
    for (uint32_t i = 0; i < opt->samples; i++) {
        uint8_t id = (uint8_t)(1 + (i % opt->writers));
        acting = id;
        sample.seq = (i / opt->writers) + 1;
        (void)tt_Publisher_publish(&pubs[id], (struct tt_Data*)&sample);
        (void)tt_Context_poll(&nodes[id], 0);
    }
}

// -R: the writer publishes RELIABLE_ROUND samples (polling after each) and takes the reader's ACKNACKs - the
// send side; then the reader takes the round - the receive side. Each side's time and clock reads are summed.
struct phase_totals {
    uint64_t send_ns;
    uint64_t recv_ns;
    uint64_t send_clock;
    uint64_t recv_clock;
    uint32_t publish_errors;
};

static void run_reliable(const struct options* opt, uint8_t reader, struct phase_totals* totals) {
    struct BenchData sample;
    memset(&sample, 0, sizeof(sample));
    for (uint32_t done = 0; done < opt->samples;) {
        uint32_t round = opt->samples - done < RELIABLE_ROUND ? opt->samples - done : RELIABLE_ROUND;
        uint64_t clock_before = clock_calls;
        uint64_t start = now_ns();
        for (uint32_t i = 0; i < round; i++) {
            acting = 1;
            sample.seq = done + i + 1;
            totals->publish_errors += tt_Publisher_publish(&pubs[1], (struct tt_Data*)&sample) != tt_RET_OK;
            (void)tt_Context_poll(&nodes[1], 0);
        }
        deliver(1); // the reader's ACKNACKs
        totals->send_ns += now_ns() - start;
        totals->send_clock += clock_calls - clock_before;
        clock_before = clock_calls;
        start = now_ns();
        deliver(reader);
        totals->recv_ns += now_ns() - start;
        totals->recv_clock += clock_calls - clock_before;
        done += round;
    }
}

#ifdef BENCH_CP_ENABLED
// A scratch copy of tickle.c with rdtsc brackets on the receive path (OPTIMIZATION_PLAN.md 11) defines these.
static void print_brackets(uint32_t samples) {
    extern uint64_t bench_cp_sum[8];
    static const char* names[] = {"", "lock", "packet", "data_header", "lookup_checks", "deliver", "unlock"};
    uint64_t t0 = __rdtsc();
    uint64_t n0 = now_ns();
    while (now_ns() - n0 < NS_PER_S / 10) {
    }
    double cycles_per_ns = (double)(__rdtsc() - t0) / (double)(now_ns() - n0);
    printf("BRACKET:");
    for (int k = 1; k <= 6; k++) {
        printf(" %s_ns=%.2f", names[k], (double)bench_cp_sum[k] / cycles_per_ns / samples);
    }
    printf("\n");
}
#endif

// -p (OPTIMIZATION_PLAN.md 11.4): what a thread publishing on the node pays while the node drains - each
// tt_Publisher_publish() timed. Tight, so it contends for the state lock as hard as it can: a bound, not a
// typical rate. On its own CPU, the lowest in the process's mask; the draining thread takes the highest.
#define PUBLISH_SAMPLES_MAX 4000000U
#define PERCENT 100U
#define P99 99U
static struct tt_Publisher back_pub;
static _Atomic bool publisher_stop;
static uint32_t* publish_ns;
static uint32_t publish_count;

static int pin_to(bool lowest) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        return -1;
    }
    int chosen = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &mask) && (chosen < 0 || !lowest)) {
            chosen = cpu;
        }
    }
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(chosen, &one);
    return pthread_setaffinity_np(pthread_self(), sizeof(one), &one) == 0 ? chosen : -1;
}

static void* publish_loop(void* arg) {
    (void)arg;
    discard_sends = true;
    (void)pin_to(true);
    struct BenchData sample;
    memset(&sample, 0, sizeof(sample));
    while (!atomic_load(&publisher_stop) && publish_count < PUBLISH_SAMPLES_MAX) {
        sample.seq++;
        uint64_t start = now_ns();
        (void)tt_Publisher_publish(&back_pub, (struct tt_Data*)&sample);
        publish_ns[publish_count++] = (uint32_t)(now_ns() - start);
    }
    return NULL;
}

static int compare_u32(const void* left, const void* right) {
    uint32_t lhs = *(const uint32_t*)left;
    uint32_t rhs = *(const uint32_t*)right;
    return (lhs > rhs) - (lhs < rhs);
}

static void print_publish_latency(void) {
    if (publish_count == 0) {
        printf("PUBLISH: calls=0\n");
        return;
    }
    qsort(publish_ns, publish_count, sizeof(publish_ns[0]), compare_u32);
    printf("PUBLISH: calls=%u p50_ns=%u p99_ns=%u max_ns=%u\n", publish_count, publish_ns[publish_count / 2],
           publish_ns[(uint64_t)publish_count * P99 / PERCENT], publish_ns[publish_count - 1]);
}

// WIRE_PLAN.md 9.1: how the reader routed the writers' short forms, when built against the W1 prototype. A run
// counts only with short_unrouted=0.
static void print_w1(const struct options* opt, uint8_t reader) {
#ifdef tt_W1_PROTOTYPE
    uint64_t short_sent = 0;
    for (uint32_t w = 1; w <= opt->writers; w++) {
        short_sent += nodes[w].short_sent;
    }
    printf("W1: short_sent=%llu short_routed=%llu short_slow=%llu short_unrouted=%llu\n",
           (unsigned long long)short_sent, (unsigned long long)nodes[reader].short_routed,
           (unsigned long long)nodes[reader].short_slow, (unsigned long long)nodes[reader].short_unrouted);
#else
    (void)opt;
    (void)reader;
#endif
}

int main(int argc, char** argv) {
    struct options opt;
    if (!parse(argc, argv, &opt) || ((opt.client || opt.reliable) && opt.writers != 1)) {
        fprintf(
            stderr,
            "usage: %s [-n samples] [-w writers 1-%d] [-e extra 0-%d] [-D] [-m] [-c, with -w 1] [-p] [-R, with -w 1]\n",
            argv[0], MAX_WRITERS, MAX_EXTRA);
        return 1;
    }
    wire = calloc(WIRE_MAX, sizeof(*wire));
    arena = malloc(ARENA_BYTES);
    const uint8_t reader = (uint8_t)(opt.writers + 1);
    if (wire == NULL || arena == NULL || !set_up(&opt, reader) || !discover(reader)) {
        return 1;
    }

    wire_count = 0;
    wire_bytes = 0;
    arena_used = 0;
    cursor[reader] = 0;
    if (opt.reliable && !opt.client) { // with -c: the clients' scheduler-driven send, on the reliable writer
        struct phase_totals totals = {0};
        received = 0;
        run_reliable(&opt, reader, &totals);
        printf("RESULT: samples=%u reliable=1 datagrams=%u received=%llu dropped=%u publish_errors=%u "
               "send_ns_per_sample=%.2f recv_ns_per_sample=%.2f send_clock_per_sample=%.3f "
               "recv_clock_per_sample=%.3f wire_bytes_per_sample=%.2f tt_version=%d\n",
               opt.samples, wire_count, (unsigned long long)received, wire_dropped, totals.publish_errors,
               (double)totals.send_ns / opt.samples, (double)totals.recv_ns / opt.samples,
               (double)totals.send_clock / opt.samples, (double)totals.recv_clock / opt.samples,
               (double)wire_bytes / opt.samples, tt_VERSION);
        print_w1(&opt, reader);
        return received == opt.samples && totals.publish_errors == 0 ? 0 : 2;
    }
    uint64_t clock_before = clock_calls;
    uint64_t start = now_ns();
    send_all(&opt);
    uint64_t send_ns = now_ns() - start;
    uint64_t send_clock = clock_calls - clock_before;
    uint32_t sent = wire_count;
    uint64_t sent_bytes = wire_bytes;

    pthread_t publisher; // NOLINT(misc-include-cleaner) - <pthread.h> above
    if (opt.concurrent_publisher) {
        acting = reader;
        publish_ns = calloc(PUBLISH_SAMPLES_MAX, sizeof(*publish_ns));
        if (publish_ns == NULL || pin_to(false) < 0 ||
            tt_Context_create_publisher(&nodes[reader], &back_pub, &BenchTopic, "back") != tt_RET_OK ||
            pthread_create(&publisher, NULL, publish_loop, NULL) != 0) {
            return 1;
        }
    }
    received = 0;
    clock_before = clock_calls;
    start = now_ns();
    deliver(reader);
    uint64_t recv_ns = now_ns() - start;
    uint64_t recv_clock = clock_calls - clock_before;
    if (opt.concurrent_publisher) {
        atomic_store(&publisher_stop, true);
        (void)pthread_join(publisher, NULL);
    }

    printf("RESULT: samples=%u client=%d writers=%u extra=%u discovery=%d datagrams=%u received=%llu dropped=%u "
           "send_ns_per_sample=%.2f recv_ns_per_sample=%.2f send_clock_per_sample=%.3f "
           "recv_clock_per_sample=%.3f wire_bytes_per_sample=%.2f tt_version=%d\n",
           opt.samples, opt.client ? 1 : 0, opt.writers, opt.extra, opt.discovery ? 1 : 0, sent,
           (unsigned long long)received, wire_dropped, (double)send_ns / opt.samples, (double)recv_ns / opt.samples,
           (double)send_clock / opt.samples, (double)recv_clock / opt.samples, (double)sent_bytes / opt.samples,
           tt_VERSION);
#ifdef BENCH_CP_ENABLED
    print_brackets(opt.samples);
#endif
    print_w1(&opt, reader);
    if (opt.concurrent_publisher) {
        print_publish_latency();
    }
    if (opt.micro) {
        micro(reader, &sub, &pubs[opt.writers], (uint8_t)opt.writers);
    }
    return received == opt.samples ? 0 : 2;
}
