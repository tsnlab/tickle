/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// nosend_bench - the bench of nosend_benefit_rig.sh (2026-10-15): what "a Publisher with no matched reader sends
// nothing" (faa178a3, ab/no-send-unmatched) buys and costs, against its parent, which broadcast such a sample to the
// link. One binary, three roles:
//
//   nosend_bench pub     [-R] [-r HZ] [-d SECONDS] [-b BROADCAST]   a writer; publishes for SECONDS (or until SIGINT);
//                                                                    -r 0: the Publisher exists and publishes nothing
//   nosend_bench sub     [-R] [-t TIMEOUT_S] [-l LINGER_S] [-b BROADCAST]   a late reader: time to its first sample
//   nosend_bench observe [-d SECONDS] [-b BROADCAST]                 a context with no endpoint at all, on the link
//
// The sample is p1's shape (76 bytes: send_ns, seq, 64 bytes) in the default build, and 1 MiB (perf_test's Array1m,
// encoded by one memcpy of a message already in memory, as rmw_tickle's is) in the build with rmw's datagram
// (-Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1), which is what turns large samples on. -R: RELIABLE, KEEP_LAST
// 10 (ROS's default depth); else BEST_EFFORT. The harness builds both, at each commit, with -Dtt_SEGMENT_ENABLED=0: the
// PC preflight's two namespaces share /dev/shm, and with the segment they would be same-host peers, which the rig's two
// Pis are not.
//
// RESULT lines (one per role, key=value):
//   pub      published, cpu_user_ns / cpu_sys_ns over the publishing window alone (getrusage), cpu_ns_per_sample, the
//            context's tx_datagrams / rx_datagrams. Whether the publishes were sent is core's own traffic line (stderr,
//            at context destroy): the branch adds tx_unmatched_dropped / tx_unmatched_kept there, the parent has none.
//   sub      ctx_to_announce_ms (context creation to the first announce of any other context: discovery, which the
//            change does not touch - the control), sub_to_first_ms (Subscriber creation to the first sample's
//            callback; -1 if none came within TIMEOUT_S), delivered (samples in that and the linger after it).
//            Every time is this process's own CLOCK_MONOTONIC: no clock is shared with the writer's host.
//   observe  cpu_user_ns / cpu_sys_ns over the run, rx_datagrams.
// A first line "BUILD: tree=<BENCH_TREE> sample_bytes=<n> large=<0|1>" names the tree it was built from.

#ifndef BENCH_TREE
#define BENCH_TREE "unknown"
#endif

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // NOLINT(misc-include-cleaner) - getopt()/optarg/optind: glibc-private headers

#include <sys/resource.h> // NOLINT(misc-include-cleaner) - getrusage(): struct rusage lives in a glibc-private header
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#if defined(tt_LARGE_SAMPLES) && tt_LARGE_SAMPLES
#define BENCH_LARGE 1
#define BENCH_SAMPLE_BYTES (1024U * 1024U) // perf_test's Array1m
#else
#define BENCH_LARGE 0
#define BENCH_SAMPLE_BYTES 76U // p1: send_ns (8), seq (4), payload (64)
#endif

#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000ULL
#define NS_PER_MS 1e6
#define POLL_NS 1000000 // 1 ms: the reader's time resolution
#define DISCOVERY_NS (1ULL * NS_PER_S)
#define BLOCKED_POLL_NS 1000000
#define CACHE_RECORDS 64U
#define KEEP_LAST_DEPTH 10U
#define SMALL_TRACKING_WORDS 16U
#define BITS_PER_WORD 64U
#define SEQ_OFFSET 8U
#define DEFAULT_RATE_HZ 1000.0
#define DEFAULT_DURATION_S 60.0
#define DEFAULT_TIMEOUT_S 10.0
#define DEFAULT_LINGER_S 0.5

static volatile sig_atomic_t stop_requested;
static uint8_t* message;
static uint32_t current_seq;
static uint32_t delivered;
static uint64_t first_delivery_ns;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - CLOCK_MONOTONIC: a glibc-private header
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static int32_t bench_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)BENCH_SAMPLE_BYTES;
}

// The message is in memory already (filled once); the publish stamps its send time and index and copies it.
static int32_t bench_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    uint64_t sent = now_ns();
    memcpy(message, &sent, sizeof(sent));
    memcpy(message + SEQ_OFFSET, &current_seq, sizeof(current_seq));
    memcpy(payload, message, len);
    return (int32_t)len;
}

static int32_t bench_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    if (len < BENCH_SAMPLE_BYTES) {
        return 0;
    }
    if (delivered == 0) {
        first_delivery_ns = now_ns();
    }
    delivered++;
    return 0;
}

static void bench_free(struct tt_Data* data) {
    (void)data;
}

static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

static struct tt_Topic topic = {
    .name = "nosend_benefit",
    .data_size = 8,
    .data_encode_size = bench_encode_size,
    .data_encode = bench_encode,
    .data_decode = bench_decode,
    .data_free = bench_free,
};

#if BENCH_LARGE
static void* large_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void large_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}
#endif

struct cpu_ns {
    uint64_t user;
    uint64_t sys;
};

static struct cpu_ns cpu_now(void) {
    struct rusage usage; // NOLINT(misc-include-cleaner) - see <sys/resource.h> above
    getrusage(RUSAGE_SELF, &usage);
    struct cpu_ns cpu = {((uint64_t)usage.ru_utime.tv_sec * NS_PER_S) + ((uint64_t)usage.ru_utime.tv_usec * NS_PER_US),
                         ((uint64_t)usage.ru_stime.tv_sec * NS_PER_S) + ((uint64_t)usage.ru_stime.tv_usec * NS_PER_US)};
    return cpu;
}

static char default_broadcast[] = "192.168.10.255"; // the rig's link, as every bench uses it

enum role { ROLE_PUB, ROLE_SUB, ROLE_OBSERVE };

struct options {
    enum role role;
    bool reliable;
    double rate_hz;
    double duration_s;
    double timeout_s;
    double linger_s;
    char* broadcast;
};

static bool parse_options(int argc, char** argv, struct options* opts) {
    if (argc < 2) {
        return false;
    }
    *opts = (struct options) {
        ROLE_PUB, false, DEFAULT_RATE_HZ, DEFAULT_DURATION_S, DEFAULT_TIMEOUT_S, DEFAULT_LINGER_S, default_broadcast};
    if (strcmp(argv[1], "sub") == 0) {
        opts->role = ROLE_SUB;
    } else if (strcmp(argv[1], "observe") == 0) {
        opts->role = ROLE_OBSERVE;
    } else if (strcmp(argv[1], "pub") != 0) {
        return false;
    }
    int opt;
    optind = 2;                                               // NOLINT(misc-include-cleaner) - see <unistd.h> above
    while ((opt = getopt(argc, argv, "Rr:d:t:l:b:")) != -1) { // NOLINT(misc-include-cleaner)
        switch (opt) {
        case 'R':
            opts->reliable = true;
            break;
        case 'r':
            opts->rate_hz = strtod(optarg, NULL); // NOLINT(misc-include-cleaner)
            break;
        case 'd':
            opts->duration_s = strtod(optarg, NULL); // NOLINT(misc-include-cleaner)
            break;
        case 't':
            opts->timeout_s = strtod(optarg, NULL); // NOLINT(misc-include-cleaner)
            break;
        case 'l':
            opts->linger_s = strtod(optarg, NULL); // NOLINT(misc-include-cleaner)
            break;
        case 'b':
            opts->broadcast = optarg; // NOLINT(misc-include-cleaner)
            break;
        default:
            return false;
        }
    }
    return opts->rate_hz >= 0.0 && opts->duration_s > 0.0 && opts->timeout_s > 0.0;
}

static void poll_for(struct tt_Context* node, uint64_t span_ns) {
    uint64_t start = now_ns();
    while (!stop_requested && now_ns() - start < span_ns) {
        (void)tt_Context_poll(node, POLL_NS);
    }
}

static int run_publisher(struct tt_Context* node, const struct options* opts) {
    static struct tt_Publisher pub;
    if (tt_Context_create_publisher(node, &pub, &topic, "stream") != tt_RET_OK) {
        fprintf(stderr, "cannot create the publisher\n");
        return 1;
    }
    static struct tt_ReliableCacheIndex index[CACHE_RECORDS];
    static uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_RECORDS, tt_RELIABLE_RECORD_BYTES(tt_CONTROL_MAX_LENGTH))];
    static struct tt_ReliableCache cache;
    if (opts->reliable) {
        cache.index = index;
        cache.capacity = CACHE_RECORDS;
        cache.depth = CACHE_RECORDS;
        cache.arena = arena;
        cache.arena_size = (uint32_t)sizeof(arena);
        cache.sample_depth = KEEP_LAST_DEPTH;
        pub.reliable_cache = &cache;
        pub.reliable = true;
    }
    poll_for(node, DISCOVERY_NS);
    // -r 0: a Publisher that publishes nothing (the harness's control cell: discovery and the link's background).
    bool idle = opts->rate_hz <= 0.0;
    uint64_t period = idle ? NS_PER_S : (uint64_t)((double)NS_PER_S / opts->rate_hz);
    uint64_t begin = now_ns();
    uint64_t next = begin;
    uint32_t published = 0;
    uint32_t refused = 0;
    struct cpu_ns before = cpu_now();
    printf("PUBLISHING: t_ns=%llu\n", (unsigned long long)begin);
    (void)fflush(stdout);
    while (!stop_requested && (double)(now_ns() - begin) / (double)NS_PER_S < opts->duration_s) {
        uint64_t now = now_ns();
        if (idle || now < next) {
            (void)tt_Context_poll(node, idle ? POLL_NS : (int64_t)(next - now));
            continue;
        }
        current_seq = published + 1U;
        tt_ret_t ret = tt_Publisher_publish(&pub, (struct tt_Data*)&current_seq);
        if (ret == tt_RET_WOULD_BLOCK) {
            refused++;
            (void)tt_Context_poll(node, BLOCKED_POLL_NS);
            continue;
        }
        if (ret == tt_RET_OK) {
            published++;
        } else {
            fprintf(stderr, "publish failed: %d\n", ret);
        }
        next += period;
    }
    struct cpu_ns after = cpu_now();
    uint64_t window = now_ns() - begin;
    uint64_t cpu = (after.user - before.user) + (after.sys - before.sys);
    printf("RESULT: role=pub sample_bytes=%u reliable=%d rate_hz=%.1f window_s=%.3f published=%u would_block=%u "
           "cpu_user_ns=%llu cpu_sys_ns=%llu cpu_ns_per_sample=%.1f tx_datagrams=%lu rx_datagrams=%lu\n",
           BENCH_SAMPLE_BYTES, opts->reliable ? 1 : 0, opts->rate_hz, (double)window / (double)NS_PER_S, published,
           refused, (unsigned long long)(after.user - before.user), (unsigned long long)(after.sys - before.sys),
           published > 0 ? (double)cpu / published : 0.0, (unsigned long)node->tx_datagrams,
           (unsigned long)node->rx_datagrams);
    (void)fflush(stdout);
    return 0;
}

// The first announce of any other context: update_seen[] is set by process_announce() once an announce is acted on.
static bool heard_another(const struct tt_Context* node) {
    for (uint32_t i = 0; i < tt_MAX_CONTEXT_IDS; i++) {
        if (i != node->id && node->update_seen[i]) {
            return true;
        }
    }
    return false;
}

static int run_subscriber(struct tt_Context* node, const struct options* opts, uint64_t ctx_ns) {
    static struct tt_Subscriber sub;
    uint64_t sub_ns = now_ns();
    if (tt_Context_create_subscriber(node, &sub, &topic, "stream", on_sample) != tt_RET_OK) {
        fprintf(stderr, "cannot create the subscriber\n");
        return 1;
    }
    static uint64_t tracking[tt_MAX_PEER_COUNT * tt_RELIABLE_BITMAP_MAX_WORDS];
    if (opts->reliable) {
        sub.reliable = true;
        sub.tracking_bitmaps = tracking;
        sub.tracking_words =
            BENCH_SAMPLE_BYTES > tt_MAX_SAMPLE_LENGTH ? (uint16_t)tt_RELIABLE_BITMAP_MAX_WORDS : SMALL_TRACKING_WORDS;
        // A RELIABLE Subscriber of a sample that fragments needs reorder slots (large_sample_bench.c); a large sample
        // does not use them, and p1 does not fragment - sized as there all the same.
        if (BENCH_SAMPLE_BYTES <= tt_MAX_SAMPLE_LENGTH) {
            size_t slot_bytes = (sizeof(struct tt_ReorderSlot) + tt_CONTROL_MAX_LENGTH + sizeof(uint64_t) - 1) &
                                ~(sizeof(uint64_t) - 1);
            uint16_t slots = (uint16_t)(SMALL_TRACKING_WORDS * BITS_PER_WORD);
            sub.reorder_storage = (uint64_t*)calloc(slots, slot_bytes);
            sub.reorder_slots = sub.reorder_storage != NULL ? slots : 0;
            sub.reorder_slot_bytes = (uint16_t)slot_bytes;
        }
    }
    uint64_t announce_ns = 0;
    uint64_t deadline = sub_ns + (uint64_t)(opts->timeout_s * (double)NS_PER_S);
    uint64_t linger = (uint64_t)(opts->linger_s * (double)NS_PER_S);
    while (!stop_requested) {
        (void)tt_Context_poll(node, POLL_NS);
        uint64_t now = now_ns();
        if (announce_ns == 0 && heard_another(node)) {
            announce_ns = now;
        }
        if (delivered > 0 ? now - first_delivery_ns >= linger : now >= deadline) {
            break;
        }
    }
    printf("RESULT: role=sub sample_bytes=%u reliable=%d ctx_to_announce_ms=%.3f ctx_to_sub_ms=%.3f "
           "sub_to_first_ms=%.3f delivered=%u timeout_s=%.1f rx_datagrams=%lu\n",
           BENCH_SAMPLE_BYTES, opts->reliable ? 1 : 0,
           announce_ns != 0 ? (double)(announce_ns - ctx_ns) / NS_PER_MS : -1.0, (double)(sub_ns - ctx_ns) / NS_PER_MS,
           delivered > 0 ? (double)(first_delivery_ns - sub_ns) / NS_PER_MS : -1.0, delivered, opts->timeout_s,
           (unsigned long)node->rx_datagrams);
    (void)fflush(stdout);
    return 0;
}

static int run_observer(struct tt_Context* node, const struct options* opts) {
    struct cpu_ns before = cpu_now();
    uint64_t begin = now_ns();
    poll_for(node, (uint64_t)(opts->duration_s * (double)NS_PER_S));
    struct cpu_ns after = cpu_now();
    printf("RESULT: role=observe window_s=%.3f cpu_user_ns=%llu cpu_sys_ns=%llu rx_datagrams=%lu tx_datagrams=%lu\n",
           (double)(now_ns() - begin) / (double)NS_PER_S, (unsigned long long)(after.user - before.user),
           (unsigned long long)(after.sys - before.sys), (unsigned long)node->rx_datagrams,
           (unsigned long)node->tx_datagrams);
    (void)fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    struct options opts;
    if (!parse_options(argc, argv, &opts)) {
        fprintf(stderr,
                "usage: %s pub [-R] [-r HZ] [-d S] | sub [-R] [-t TIMEOUT_S] [-l LINGER_S] | observe [-d S]"
                " [-b BROADCAST]\n",
                argv[0]);
        return 2;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    printf("BUILD: tree=%s sample_bytes=%u large=%d\n", BENCH_TREE, BENCH_SAMPLE_BYTES, BENCH_LARGE);
    (void)fflush(stdout);
    message = (uint8_t*)calloc(1, BENCH_SAMPLE_BYTES);
    if (message == NULL) {
        return 1;
    }
    for (uint32_t i = SEQ_OFFSET + sizeof(uint32_t); i < BENCH_SAMPLE_BYTES; i++) {
        message[i] = (uint8_t)i;
    }
    _tt_CONFIG.broadcast = opts.broadcast;

    static struct tt_Context node;
    uint64_t ctx_ns = now_ns();
    bool created = tt_Context_create(&node) == tt_RET_OK;
#if BENCH_LARGE
    created = created && tt_Context_set_large_buffers(&node, large_acquire, large_release, NULL) == tt_RET_OK;
#endif
    if (!created) {
        fprintf(stderr, "cannot create the context\n");
        return 1;
    }
    int result = 0;
    switch (opts.role) {
    case ROLE_PUB:
        result = run_publisher(&node, &opts);
        break;
    case ROLE_SUB:
        result = run_subscriber(&node, &opts, ctx_ns);
        break;
    case ROLE_OBSERVE:
        result = run_observer(&node, &opts);
        break;
    }
    tt_Context_destroy(&node);
    free(message);
    return result;
}
