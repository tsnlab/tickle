/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Large-message stage 2, step A (docs/DESIGN.md section 8): a stream of large samples - 1 MB, 4 MB - between two
// TickLE contexts over sockets, for large_sample_pc.sh (two private network namespaces on this PC, netem loss between
// them). One binary, two roles:
//
//   large_sample_bench pub -s BYTES -r HZ -d SECONDS [-R] [-A] [-b BROADCAST]
//   large_sample_bench sub -s BYTES -d SECONDS [-R] [-b BROADCAST]
//
// -R RELIABLE (KEEP_LAST 10 unless -A, KEEP_ALL), else BEST_EFFORT. Each sample carries its send time
// (CLOCK_MONOTONIC - both namespaces share the host's clock), its index and a pattern seeded by its index, so the
// subscriber checks every byte (torn=) and measures publish-to-callback latency. Built against core with rmw's
// datagram (tt_MAX_BUFFER_LENGTH 65507), which is what turns large samples on.
//
// RESULT line fields: role, size, published/delivered, latency median/p99/max in microseconds (sub), torn, cpu_ms (the
// process's user + system time while it ran), cpu_ms_per_mb (per MB delivered, sub; per MB published, pub),
// peak_rss_kb, and core's large_* counters.

#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507 // rmw_tickle's datagram: tt_MAX_SAMPLE_LENGTH 65507, and large samples above it
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

#define BENCH_HEADER 16U // send_ns (8), index (4), reserved (4)
#define BENCH_SEND_NS_BYTES 8U
#define BENCH_INDEX_OFFSET 8U
#define BENCH_INDEX_BYTES 4U
#define MAX_SAMPLES 200000U
#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000.0
#define US_PER_MS 1000.0
#define BYTES_PER_MB 1e6
#define DEFAULT_SIZE (1024U * 1024U)
#define DEFAULT_RATE_HZ 30.0
#define DEFAULT_DURATION_S 10.0
#define DISCOVERY_NS (2ULL * NS_PER_S) // the subscriber's announce, before the first sample
#define DRAIN_NS (3ULL * NS_PER_S)     // resends answered after the last sample
#define POLL_NS 10000000               // 10 ms
#define BLOCKED_POLL_NS 1000000        // 1 ms, while KEEP_ALL waits for acknowledgements
#define CACHE_RECORDS 64U              // small traffic only: large samples are kept by reference, outside the cache
#define KEEP_LAST_DEPTH 10U            // ROS's default
#define PATTERN_STEP 31U
#define PATTERN_HIGH_SHIFT 11U
#define PERCENT_99 99U
#define PERCENT 100U
#define SMALL_TRACKING_WORDS 16U
#define MISSING_NAMED 10U

static uint32_t sample_size = DEFAULT_SIZE;
static uint32_t current_index;
static volatile sig_atomic_t stop_requested;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - CLOCK_MONOTONIC: a glibc-private header
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static uint8_t pattern(uint32_t i, uint32_t index) {
    return (uint8_t)((i * PATTERN_STEP) + index + (i >> PATTERN_HIGH_SHIFT));
}

static int32_t bench_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_size;
}

static int32_t bench_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    uint64_t sent = now_ns();
    memcpy(payload, &sent, BENCH_SEND_NS_BYTES);
    memcpy(payload + BENCH_INDEX_OFFSET, &current_index, BENCH_INDEX_BYTES);
    memset(payload + BENCH_INDEX_OFFSET + BENCH_INDEX_BYTES, 0, BENCH_HEADER - BENCH_INDEX_OFFSET - BENCH_INDEX_BYTES);
    for (uint32_t i = BENCH_HEADER; i < len; i++) {
        payload[i] = pattern(i, current_index);
    }
    return (int32_t)len;
}

// --- subscriber ------------------------------------------------------------------------------------------------------

static uint32_t delivered;
static uint32_t torn;
static uint32_t out_of_order;
static uint32_t last_index;
static uint8_t seen[MAX_SAMPLES]; // by index: which samples arrived, so a missing one can be named
static uint64_t* latencies_ns;

static bool bytes_intact(const uint8_t* payload, uint32_t index) {
    for (uint32_t i = BENCH_HEADER; i < sample_size; i++) {
        if (payload[i] != pattern(i, index)) {
            return false;
        }
    }
    return true;
}

static int32_t bench_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    uint64_t arrived = now_ns();
    uint64_t sent = 0;
    uint32_t index = 0;
    if (len >= BENCH_HEADER) {
        memcpy(&sent, payload, BENCH_SEND_NS_BYTES);
        memcpy(&index, payload + BENCH_INDEX_OFFSET, BENCH_INDEX_BYTES);
    }
    if (len < sample_size || len < BENCH_HEADER || !bytes_intact(payload, index)) {
        torn++;
        return 0;
    }
    if (delivered > 0 && index <= last_index) {
        out_of_order++;
    }
    last_index = index;
    if (index < MAX_SAMPLES) {
        seen[index] = 1;
    }
    if (delivered < MAX_SAMPLES) {
        latencies_ns[delivered] = arrived - sent;
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
    .name = "large_sample_bench",
    .data_size = 8,
    .data_encode_size = bench_encode_size,
    .data_encode = bench_encode,
    .data_decode = bench_decode,
    .data_free = bench_free,
};

// --- the large buffers: malloc, as rmw_tickle's are
// -------------------------------------------------------------------

// A tree without stage 2 (the parent, for the small-sample A/B in small_sample_ab_pc.sh) builds this too: there is
// nothing large to set up or count.
#if defined(tt_LARGE_SAMPLES) && tt_LARGE_SAMPLES
#define BENCH_LARGE 1
#else
#define BENCH_LARGE 0
#endif
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

static int compare_u64(const void* first, const void* second) {
    uint64_t lhs = *(const uint64_t*)first;
    uint64_t rhs = *(const uint64_t*)second;
    return (lhs > rhs) - (lhs < rhs);
}

static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static double cpu_ms(long* peak_rss_kb) {
    struct rusage usage; // NOLINT(misc-include-cleaner) - see <sys/resource.h> above
    getrusage(RUSAGE_SELF, &usage);
    *peak_rss_kb = usage.ru_maxrss;
    double user_ms = ((double)usage.ru_utime.tv_sec * US_PER_MS) + ((double)usage.ru_utime.tv_usec / US_PER_MS);
    double system_ms = ((double)usage.ru_stime.tv_sec * US_PER_MS) + ((double)usage.ru_stime.tv_usec / US_PER_MS);
    return user_ms + system_ms;
}

static void print_large(const struct tt_Context* node) {
#if BENCH_LARGE
    printf(" large_published=%lu large_reassembled=%lu large_abandoned=%lu large_no_buffer=%lu large_dropped=%lu "
           "large_duplicate=%lu large_tail_abandoned=%lu large_send_waits=%lu tx_datagrams=%lu rx_datagrams=%lu",
           (unsigned long)node->large.published, (unsigned long)node->large.reassembled,
           (unsigned long)node->large.abandoned, (unsigned long)node->large.no_buffer,
           (unsigned long)node->large.dropped, (unsigned long)node->large.duplicate,
           (unsigned long)node->large.tail_abandoned, (unsigned long)node->large.send_waits,
           (unsigned long)node->tx_datagrams, (unsigned long)node->rx_datagrams);
#else
    printf(" tx_datagrams=%lu rx_datagrams=%lu", (unsigned long)node->tx_datagrams, (unsigned long)node->rx_datagrams);
#endif
}

static char default_broadcast[] = "192.168.10.255"; // the rig's link, as every bench uses it

struct options {
    bool publisher;
    bool reliable;
    bool keep_all;
    double rate_hz;
    double duration_s;
    char* broadcast; // as _tt_CONFIG.broadcast takes it
};

static bool parse_options(int argc, char** argv, struct options* opts) {
    if (argc < 2 || (strcmp(argv[1], "pub") != 0 && strcmp(argv[1], "sub") != 0)) {
        return false;
    }
    *opts = (struct options) {
        strcmp(argv[1], "pub") == 0, false, false, DEFAULT_RATE_HZ, DEFAULT_DURATION_S, default_broadcast};
    int opt;
    optind = 2;                                              // NOLINT(misc-include-cleaner) - see <unistd.h> above
    while ((opt = getopt(argc, argv, "s:r:d:RAb:")) != -1) { // NOLINT(misc-include-cleaner)
        switch (opt) {
        case 's':
            sample_size = (uint32_t)strtoul(optarg, NULL, 10); // NOLINT(misc-include-cleaner)
            break;
        case 'r':
            opts->rate_hz = strtod(optarg, NULL); // NOLINT(misc-include-cleaner)
            break;
        case 'd':
            opts->duration_s = strtod(optarg, NULL); // NOLINT(misc-include-cleaner)
            break;
        case 'R':
            opts->reliable = true;
            break;
        case 'A':
            opts->keep_all = true;
            break;
        case 'b':
            opts->broadcast = optarg; // NOLINT(misc-include-cleaner)
            break;
        default:
            return false;
        }
    }
    return true;
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
        cache.sample_depth = opts->keep_all ? 0 : KEEP_LAST_DEPTH;
        pub.reliable_cache = &cache;
        pub.reliable = true;
        pub.keep_all = opts->keep_all;
    }
    poll_for(node, DISCOVERY_NS);
    uint64_t period = (uint64_t)((double)NS_PER_S / opts->rate_hz);
    uint64_t begin = now_ns();
    uint64_t next = begin;
    uint32_t published = 0;
    uint32_t refused = 0;
    long peak_rss_kb = 0;
    double cpu_before = cpu_ms(&peak_rss_kb);
    while (!stop_requested && (double)(now_ns() - begin) / (double)NS_PER_S < opts->duration_s) {
        uint64_t now = now_ns();
        if (now < next) {
            (void)tt_Context_poll(node, (int64_t)(next - now));
            continue;
        }
        current_index = published;
        tt_ret_t ret = tt_Publisher_publish(&pub, (struct tt_Data*)&current_index);
        if (ret == tt_RET_WOULD_BLOCK) {
            refused++; // KEEP_ALL waits for acknowledgements, which only a poll can take in
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
    poll_for(node, DRAIN_NS);
    double cpu = cpu_ms(&peak_rss_kb) - cpu_before;
    double published_mb = (double)published * (double)sample_size / BYTES_PER_MB;
    printf("RESULT: role=pub size=%u reliable=%d keep_all=%d rate_hz=%.1f published=%u would_block=%u "
           "retransmitted=%u cpu_ms=%.1f cpu_ms_per_mb=%.3f peak_rss_kb=%ld",
           sample_size, opts->reliable ? 1 : 0, opts->keep_all ? 1 : 0, opts->rate_hz, published, refused,
           pub.retransmitted, cpu, published > 0 ? cpu / published_mb : 0.0, peak_rss_kb);
    print_large(node);
    printf("\n");
    return 0;
}

static double latency_us_at(uint32_t counted, uint32_t index) {
    return counted > 0 ? (double)latencies_ns[index] / NS_PER_US : 0.0;
}

static int run_subscriber(struct tt_Context* node, const struct options* opts) {
    latencies_ns = (uint64_t*)calloc(MAX_SAMPLES, sizeof(uint64_t));
    static struct tt_Subscriber sub;
    if (latencies_ns == NULL || tt_Context_create_subscriber(node, &sub, &topic, "stream", on_sample) != tt_RET_OK) {
        fprintf(stderr, "cannot create the subscriber\n");
        return 1;
    }
    static uint64_t tracking[tt_MAX_PEER_COUNT * tt_RELIABLE_BITMAP_MAX_WORDS];
    if (opts->reliable) {
        sub.reliable = true;
        sub.tracking_bitmaps = tracking;
        // The widest window for a large sample, which needs it; rmw's 16 words (RMW_TICKLE_TRACKING_WORDS) otherwise,
        // so a small-sample A/B against a parent with a narrower maximum compares the same window.
        sub.tracking_words =
            sample_size > tt_MAX_SAMPLE_LENGTH ? (uint16_t)tt_RELIABLE_BITMAP_MAX_WORDS : SMALL_TRACKING_WORDS;
    }
    long peak_rss_kb = 0;
    double cpu_before = cpu_ms(&peak_rss_kb);
    poll_for(node, (uint64_t)(opts->duration_s * (double)NS_PER_S));
    double cpu = cpu_ms(&peak_rss_kb) - cpu_before;
    uint32_t counted = delivered < MAX_SAMPLES ? delivered : MAX_SAMPLES;
    qsort(latencies_ns, counted, sizeof(uint64_t), compare_u64);
    uint32_t p99_index = (uint32_t)(((uint64_t)counted * PERCENT_99) / PERCENT);
    double delivered_mb = (double)delivered * (double)sample_size / BYTES_PER_MB;
    printf("RESULT: role=sub size=%u reliable=%d delivered=%u torn=%u out_of_order=%u latency_median_us=%.1f "
           "latency_p99_us=%.1f latency_max_us=%.1f cpu_ms=%.1f cpu_ms_per_mb=%.3f peak_rss_kb=%ld",
           sample_size, opts->reliable ? 1 : 0, delivered, torn, out_of_order, latency_us_at(counted, counted / 2),
           latency_us_at(counted, p99_index), latency_us_at(counted, counted > 0 ? counted - 1 : 0), cpu,
           delivered > 0 ? cpu / delivered_mb : 0.0, peak_rss_kb);
    print_large(node);
    // The first few samples below the highest that arrived that never did: which sample a loss was, not only how many.
    printf(" last_index=%u missing=", last_index);
    uint32_t named = 0;
    for (uint32_t i = 0; i < last_index && i < MAX_SAMPLES && named < MISSING_NAMED; i++) {
        if (seen[i] == 0) {
            printf("%s%u", named == 0 ? "" : ",", i);
            named++;
        }
    }
    printf("%s", named == 0 ? "none" : "");
#if BENCH_LARGE
    uint32_t held = 0;
    for (uint32_t i = 0; i < tt_LARGE_ASSEMBLIES; i++) {
        held += node->large.assemblies[i].buffer != NULL ? 1U : 0U;
    }
    printf(" assemblies_held=%u", held);
#endif
    printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    struct options opts;
    if (!parse_options(argc, argv, &opts)) {
        fprintf(stderr, "usage: %s pub|sub -s BYTES [-r HZ] -d SECONDS [-R] [-A] [-b BROADCAST]\n", argv[0]);
        return 2;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    _tt_CONFIG.broadcast = opts.broadcast;

    static struct tt_Context node;
    bool created = tt_Context_create(&node) == tt_RET_OK;
#if BENCH_LARGE
    created = created && tt_Context_set_large_buffers(&node, large_acquire, large_release, NULL) == tt_RET_OK;
#endif
    if (!created) {
        fprintf(stderr, "cannot create the context\n");
        return 1;
    }
    int result = opts.publisher ? run_publisher(&node, &opts) : run_subscriber(&node, &opts);
    tt_Context_destroy(&node);
    return result;
}
