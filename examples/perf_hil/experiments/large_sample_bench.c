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
//   large_sample_bench pub -s BYTES -r HZ -d SECONDS [-R] [-A] [-M [-G]] [-b BROADCAST] [-F auto|mtu|ip] [-n UNITS]
//                          [-H S] [-B CHUNKS]
//   large_sample_bench sub -s BYTES -d SECONDS [-R] [-b BROADCAST] [-X BYTES] [-C] [-w US] [-W US] [-K STRIDE]
//
// Hybrid transport (DESIGN.md section 8), in a tree that has it: -F sets the context's large_fragmentation, -n forces
// runs of exactly UNITS units (IP, with the Publisher's ip_datagram_max at UNITS large units: the model check's fixed
// n), -X the subscriber's advertised reassembly ceiling (0: a FreeRTOS-like peer that takes no run), -C its reassembly
// memory cap off (_tt_CONFIG.reassembly_cap_off: a forced arm's reader, so forced IP is n_max), -H halves the publish
// rate after S seconds (the control law's scaling with R). Each RESULT line then carries the realized treatment: runs
// and units sent in them and alone, the histogram of n, the reader's cap, ceiling and the control law's decisions
// (collapses, probes, rescales, path failures), and the loss counts.
//
// -M (pub): the sample is a message already in memory - filled before the publish, its send time stamped just before
// it - and encoding it is a memcpy, as rmw_tickle's is for a type whose wire bytes are its message (perf_test's
// Array4m). -G (pub, with -M): the topic offers a ranged encode too (core DESIGN.md section 8, "Encoding while
// sending"), so core copies each range just before the datagrams that carry it. Without -M the pattern is generated
// during the encode and the send time stamped at its start, as before. A tree without tt_DATA_ENCODE_RANGE builds
// this with -DBENCH_NO_RANGE, and -G is then refused.
// -B CHUNKS (pub): large-message step B's shared region of that many chunks for the Publisher (large_samehost_pc.sh),
// so a same-host reader takes each sample through one descriptor; 0, the default, none. Refused in a build without
// tt_LARGE_SHM.
// -R RELIABLE (KEEP_LAST 10 unless -A, KEEP_ALL), else BEST_EFFORT. Each sample carries its send time
// (CLOCK_MONOTONIC - both namespaces share the host's clock), its index and a pattern seeded by its index, so the
// subscriber checks every byte (torn=) and measures publish-to-callback latency. Built against core with rmw's
// datagram (tt_MAX_BUFFER_LENGTH 65507), which is what turns large samples on.
// Subscriber only: -K STRIDE checks every STRIDE-th byte of a sample and its last, not every byte - a receive-cost
// measurement (rx_gap_pc.sh) whose user-space count the check would otherwise swamp; it still reads every cache line
// at a stride of 64 or less, as an application consuming the sample does. -w US spins that long in each sample's
// callback, inside the poll, as rmw_tickle's decode of a large message does; -W US spins that long after a poll that
// delivered a sample, outside it, as an application's handling of it does (large_saturation_pc.sh).
//
// RESULT line fields: role, size, published/delivered, latency median/p99/max in microseconds (sub), torn, cpu_ms (the
// process's user + system time while it ran), cpu_ms_per_mb (per MB delivered, sub; per MB published, pub),
// peak_rss_kb, and core's large_* counters, then the context's discovery: miss_threshold, summary_copies and
// announce_repeats (what it sent of each).
//
// -P wifi (wifi_burst_loss_netns.sh, 2026-10-23): the context is created with tt_Context_create_with() and one link of
// -b's broadcast and profile WIFI, the -F/-X/-C settings in its net options, instead of tt_Context_create() and
// _tt_CONFIG; without -P nothing changes.

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

#include <linux/sock_diag.h> // SK_MEMINFO_*: bench_socket_drops()
#include <sys/resource.h> // NOLINT(misc-include-cleaner) - getrusage(): struct rusage lives in a glibc-private header
#include <sys/socket.h>   // getsockopt(), SO_MEMINFO
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h> // struct tt_hal: the receive sockets, the drain's counters
#include <tickle/tickle.h>

// RSTATS lines at the end of a run when built with -Dtt_RELIABLE_STATS (wifi_burst_loss_netns.sh's large cell). Guarded
// so a copy of this file built elsewhere (hybrid_first_n_pc.sh) compiles without the header beside it.
#if defined(tt_RELIABLE_STATS) && __has_include("../tickle/common/reliable_stats_print.h")
#include "../tickle/common/reliable_stats_print.h"
#define BENCH_RSTATS(role) print_reliable_stats(role)
#else
#define BENCH_RSTATS(role) ((void)(role))
#endif

#define BENCH_HEADER 16U // send_ns (8), index (4), reserved (4)
#define BENCH_SEND_NS_BYTES 8U
#define BENCH_INDEX_OFFSET 8U
#define BENCH_INDEX_BYTES 4U
#define MAX_SAMPLES 200000U
#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000.0
#define NS_PER_US_INT 1000ULL
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
// The retained-sample arena this bench gave its hand-built cache until the QoS creation API: CACHE_RECORDS control
// datagrams and one of wrap slack. Now history.bytes, so the arena the profile sizes is never larger than it was.
#define CACHE_ARENA_BYTES tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_RECORDS, tt_RELIABLE_RECORD_BYTES(tt_CONTROL_MAX_LENGTH))
#define PATTERN_STEP 31U
#define PATTERN_HIGH_SHIFT 11U
#define PERCENT_99 99U
#define PERCENT 100U
#define SMALL_TRACKING_WORDS 16U
#define BITS_PER_WORD 64U
#define MISSING_NAMED 10U
#define BENCH_RUN_FRAMING 28U  // a large run's framing on the wire: the single header and FRAG_RUN_L's
#define BENCH_LARGE_UNIT 1452U // one full unit of a large sample

static uint32_t sample_size = DEFAULT_SIZE;
static uint64_t decode_spin_ns;              // -w: in the callback
static uint64_t after_spin_ns;               // -W: after a poll that delivered
static uint32_t check_stride = 1U;           // -K: every byte checked by default; every Kth (and the last) with -K
static uint64_t discovery_ns = DISCOVERY_NS; // -D: how long a publisher polls before its first sample (rmw: 0)
static uint32_t current_index;
static volatile sig_atomic_t stop_requested;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - CLOCK_MONOTONIC: a glibc-private header
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static void spin_ns(uint64_t span) {
    uint64_t end = now_ns() + span;
    while (now_ns() < end) {
    }
}

static uint8_t pattern(uint32_t i, uint32_t index) {
    return (uint8_t)((i * PATTERN_STEP) + index + (i >> PATTERN_HIGH_SHIFT));
}

static int32_t bench_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_size;
}

// -M's message, and how many ranges the ranged encode copied (the -G arm's treatment check).
static uint8_t* message;
static uint64_t range_calls;

static int32_t message_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memcpy(payload, message, len);
    return (int32_t)len;
}

#ifndef BENCH_NO_RANGE
static int32_t message_encode_range(struct tt_Data* data, uint8_t* payload, uint32_t offset, uint32_t len) {
    (void)data;
    if ((uint64_t)offset + len > sample_size) {
        return -1;
    }
    memcpy(payload, message + offset, len);
    range_calls++;
    return (int32_t)len;
}
#endif

// The message for sample `index`: its pattern, then - last, just before the publish - its send time.
static void message_fill(uint32_t index) {
    memcpy(message + BENCH_INDEX_OFFSET, &index, BENCH_INDEX_BYTES);
    memset(message + BENCH_INDEX_OFFSET + BENCH_INDEX_BYTES, 0, BENCH_HEADER - BENCH_INDEX_OFFSET - BENCH_INDEX_BYTES);
    for (uint32_t i = BENCH_HEADER; i < sample_size; i++) {
        message[i] = pattern(i, index);
    }
    uint64_t sent = now_ns();
    memcpy(message, &sent, BENCH_SEND_NS_BYTES);
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
static uint32_t first_index;      // the first sample delivered: a VOLATILE reader's match point
static uint8_t seen[MAX_SAMPLES]; // by index: which samples arrived, so a missing one can be named
static uint64_t* latencies_ns;

static bool bytes_intact(const uint8_t* payload, uint32_t index) {
    for (uint32_t i = BENCH_HEADER; i < sample_size; i += check_stride) {
        if (payload[i] != pattern(i, index)) {
            return false;
        }
    }
    return sample_size <= BENCH_HEADER || payload[sample_size - 1U] == pattern(sample_size - 1U, index);
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
    if (delivered == 0) {
        first_index = index;
    }
    last_index = index;
    spin_ns(decode_spin_ns);
    if (index < MAX_SAMPLES) {
        seen[index] = 1;
    }
    if (delivered < MAX_SAMPLES) {
        latencies_ns[delivered] = arrived - sent;
    }
#ifdef R4_TRACE
    printf("LAT index=%u sent_ns=%llu arrived_ns=%llu\n", index, (unsigned long long)sent, (unsigned long long)arrived);
#endif
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

// max_encoded_size is the run's sample size (-s), set before either endpoint is created.
static struct tt_TypeSupport topic = {
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

// The context's discovery as it ran (the header's RESULT fields): its miss threshold and what it sent of the WIFI
// profile's summary copies and announce repeats - the treatment a -P wifi arm names.
static void print_discovery(const struct tt_Context* node) {
    printf(" miss_threshold=%u summary_copies=%lu announce_repeats=%lu", (unsigned)node->opts.discovery.miss_threshold,
           (unsigned long)node->summary_copies_sent, (unsigned long)node->announce_repeats_sent);
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
#ifndef BENCH_NO_RANGE
    printf(" large_encoded_by_range=%lu large_encode_failed=%lu", (unsigned long)node->large.encoded_by_range,
           (unsigned long)node->large.encode_failed);
#endif
#ifdef tt_LARGE_SEND_ON_WRITABLE // a tree with the writability resume (DESIGN.md section 8, "Send cursor")
    printf(" large_send_writable=%lu", (unsigned long)node->large.send_writable);
#endif
#if defined(tt_LARGE_SHM) && tt_LARGE_SHM
    printf(" large_ref_sent=%lu large_ref_received=%lu large_ref_refused=%lu large_ref_dropped=%lu "
           "large_region_full=%lu shm_full_dropped=%lu",
           (unsigned long)node->large_ref.ref_sent, (unsigned long)node->large_ref.ref_received,
           (unsigned long)node->large_ref.ref_refused, (unsigned long)node->large_ref.ref_dropped,
           (unsigned long)node->large_ref.region_full, (unsigned long)node->segment_full_dropped);
#endif
#else
    printf(" tx_datagrams=%lu rx_datagrams=%lu", (unsigned long)node->tx_datagrams, (unsigned long)node->rx_datagrams);
#endif
    // UDP offload (hal_linux.c): the treatment check of udp_offload_pc.sh's arms - what the HAL turned on and did.
    printf(" udp_offload=%u gso_sends=%lu gso_datagrams=%lu gro_reads=%lu gro_merged=%lu gro_copied=%lu gro_off8=%lu",
           (unsigned)node->udp_offload, (unsigned long)node->udp_gso_sends, (unsigned long)node->udp_gso_datagrams,
           (unsigned long)node->udp_gro_reads, (unsigned long)node->udp_gro_merged, (unsigned long)node->udp_gro_copied,
           (unsigned long)node->udp_gro_off8);
}

// Datagrams the kernel dropped at one socket because its receive buffer was full (SO_MEMINFO), 0 when it cannot say:
// read here rather than from the HAL so that a tree from before the HAL counted them builds this too.
static unsigned bench_socket_drops(int sock) {
    uint32_t info[SK_MEMINFO_VARS];
    socklen_t length = sizeof(info);
    memset(info, 0, sizeof(info));
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET, SO_MEMINFO: glibc-private headers behind <sys/socket.h>
    if (sock < 0 || getsockopt(sock, SOL_SOCKET, SO_MEMINFO, (void*)info, &length) < 0 ||
        length < (socklen_t)(sizeof(uint32_t) * (SK_MEMINFO_DROPS + 1))) {
        return 0;
    }
    return (unsigned)info[SK_MEMINFO_DROPS];
}

// The realized hybrid treatment (DESIGN.md section 8, pre-registration: "each arm prints its realized treatment").
#if defined(tt_HYBRID_TRANSPORT) && tt_HYBRID_TRANSPORT
#define BENCH_HYBRID 1
#else
#define BENCH_HYBRID 0
#endif
static void print_hybrid(const struct tt_Context* node, const struct tt_Publisher* pub) {
#if BENCH_HYBRID
    const struct tt_HybridState* hybrid = &node->hybrid;
    printf(" hybrid_runs=%lu hybrid_run_units=%lu hybrid_mtu_units=%lu hybrid_n_hist=", (unsigned long)hybrid->runs,
           (unsigned long)hybrid->run_units, (unsigned long)hybrid->mtu_units);
    for (int i = 0; i < tt_HYBRID_N_BUCKETS; i++) {
        printf("%s%lu", i == 0 ? "" : "/", (unsigned long)hybrid->n_hist[i]);
    }
    printf(" hybrid_path_failures=%lu", (unsigned long)hybrid->path_failures);
    printf(" hybrid_advertised=%u hybrid_cap=%u hybrid_n_fail=%u hybrid_cap_changes=%lu hybrid_collapses=%lu"
           " hybrid_probes=%lu hybrid_probe_failures=%lu hybrid_rescales=%lu hybrid_lifetime_s=%.1f"
           " hybrid_rx_loss=%u/%u hybrid_window=%u/%u",
           (unsigned)hybrid->advertised, (unsigned)hybrid->cap, (unsigned)hybrid->n_fail,
           (unsigned long)hybrid->cap_changes, (unsigned long)hybrid->collapses, (unsigned long)hybrid->probes,
           (unsigned long)hybrid->probe_failures, (unsigned long)hybrid->rescales,
           (double)hybrid->lifetime_ns / (double)tt_SECOND, hybrid->rx_loss.events, hybrid->rx_loss.frames,
           hybrid->window.lost, hybrid->window.sent);
    printf(" hybrid_mtu_loss=%u/%u hybrid_advertised_loss=0x%04x", hybrid->mtu_loss.events, hybrid->mtu_loss.frames,
           (unsigned)hybrid->advertised_loss);
#ifdef tt_HYBRID_STALE_EVENTS // a tree whose reader forgets a few events its clean frames outlived
    printf(" hybrid_stale_forgets=%lu hybrid_stale_events=%lu", (unsigned long)hybrid->stale_forgets,
           (unsigned long)hybrid->stale_events);
#endif
#ifdef tt_HAL_RECEIVER_DROPS
    printf(" hybrid_receiver_drops=%lu hybrid_receiver_lost=%lu", (unsigned long)hybrid->receiver_drops,
           (unsigned long)hybrid->receiver_lost);
#endif
    if (pub != NULL) {
        for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
            if (pub->peer_acks[i].context_id != tt_CONTEXT_ID_INVALID) {
                printf(" hybrid_peer_loss_p=0x%04x", (unsigned)hybrid->peer_loss_p[pub->peer_acks[i].context_id]);
                break;
            }
        }
    }
#else
    (void)node;
    (void)pub;
    printf(" hybrid=none");
#endif
}

static char default_broadcast[] = "192.168.10.255"; // the rig's link, as every bench uses it

struct options {
    bool publisher;
    bool reliable;
    bool keep_all;
    bool message;
    bool ranged;
    double rate_hz;
    double duration_s;
    char* broadcast;        // as _tt_CONFIG.broadcast takes it
    int fragmentation;      // -F: tt_LARGE_FRAGMENTATION_*, or -1 for the default
    uint32_t run_units;     // -n: runs of exactly this many units; 0: as the mode chooses
    long reassembly_max;    // -X: the advertised ceiling; -1: the default
    bool cap_off;           // -C: the reader advertises its ceiling uncapped (a forced-n arm's reader)
    double halve_after_s;   // -H: the rate halves after this many seconds; 0: never
    uint32_t region_chunks; // -B: step B's shared region for the publisher, 0 for none (fragments through the ring)
    bool wifi;              // -P wifi: one link of profile WIFI (see the header)
};

static bool parse_options(int argc, char** argv, struct options* opts) {
    if (argc < 2 || (strcmp(argv[1], "pub") != 0 && strcmp(argv[1], "sub") != 0)) {
        return false;
    }
    *opts = (struct options) {strcmp(argv[1], "pub") == 0,
                              false,
                              false,
                              false,
                              false,
                              DEFAULT_RATE_HZ,
                              DEFAULT_DURATION_S,
                              default_broadcast,
                              -1,
                              0,
                              -1,
                              false,
                              0.0,
                              0,
                              false};
    int opt;
    optind = 2; // NOLINT(misc-include-cleaner) - see <unistd.h> above
    while ((opt = getopt(argc, argv, "s:r:d:RAMGb:F:n:X:CH:w:W:K:B:D:P:")) != -1) { // NOLINT(misc-include-cleaner)
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
        case 'M':
            opts->message = true;
            break;
        case 'G':
            opts->ranged = true;
            break;
        case 'b':
            opts->broadcast = optarg; // NOLINT(misc-include-cleaner)
            break;
        case 'P':
            if (strcmp(optarg, "wifi") != 0) { // NOLINT(misc-include-cleaner)
                return false;
            }
            opts->wifi = true;
            break;
        case 'H':
            opts->halve_after_s = strtod(optarg, NULL); // NOLINT(misc-include-cleaner)
            break;
#if BENCH_HYBRID
        case 'F':
            opts->fragmentation = tt_LARGE_FRAGMENTATION_AUTO;
            if (strcmp(optarg, "mtu") == 0) {
                opts->fragmentation = tt_LARGE_FRAGMENTATION_MTU;
            } else if (strcmp(optarg, "ip") == 0) {
                opts->fragmentation = tt_LARGE_FRAGMENTATION_IP;
            }
            break;
        case 'n':
            opts->run_units = (uint32_t)strtoul(optarg, NULL, 10); // NOLINT(misc-include-cleaner)
            break;
        case 'X':
            opts->reassembly_max = strtol(optarg, NULL, 10); // NOLINT(misc-include-cleaner)
            break;
        case 'C':
            opts->cap_off = true;
            break;
#endif
        case 'w':
            decode_spin_ns = strtoull(optarg, NULL, 10) * NS_PER_US_INT; // NOLINT(misc-include-cleaner)
            break;
        case 'W':
            after_spin_ns = strtoull(optarg, NULL, 10) * NS_PER_US_INT; // NOLINT(misc-include-cleaner)
            break;
        case 'D':
            discovery_ns = strtoull(optarg, NULL, 10) * NS_PER_US_INT * NS_PER_US_INT; // NOLINT(misc-include-cleaner)
            break;
        case 'K':
            check_stride = (uint32_t)strtoul(optarg, NULL, 10); // NOLINT(misc-include-cleaner)
            if (check_stride == 0) {
                return false;
            }
            break;
        case 'B':
            opts->region_chunks = (uint32_t)strtoul(optarg, NULL, 10); // NOLINT(misc-include-cleaner)
            break;
        default:
            return false;
        }
    }
#ifdef BENCH_NO_RANGE
    if (opts->ranged) {
        return false; // this tree has no ranged encode
    }
#endif
    return !opts->ranged || opts->message;
}

static void poll_for(struct tt_Context* node, uint64_t span_ns) {
    uint64_t start = now_ns();
    while (!stop_requested && now_ns() - start < span_ns) {
        uint32_t before = delivered;
        (void)tt_Context_poll(node, POLL_NS);
        if (delivered != before) {
            spin_ns(after_spin_ns);
        }
    }
}

// A KEEP_ALL Publisher's depth: the hand-built cache it had held CACHE_RECORDS datagrams, and KEEP_ALL may hold that
// many unacknowledged (keep_all_bound()). The profile's index ring is (depth + 1) times a sample's datagrams, so the
// depth that gives exactly CACHE_RECORDS where a sample takes at most half of them, and 1 - the least - otherwise.
static uint32_t keep_all_depth(void) {
    uint32_t cdr = sample_size < (uint32_t)tt_MAX_SAMPLE_LENGTH ? sample_size : (uint32_t)tt_MAX_SAMPLE_LENGTH;
    uint32_t samples = CACHE_RECORDS / tt_sample_datagrams(cdr);
    return samples > 1U ? samples - 1U : 1U;
}

// The Publisher's profile: -R RELIABLE, KEEP_LAST 10 or (-A) KEEP_ALL, retaining at most the arena the hand-built
// cache had; BEST_EFFORT otherwise, which retains nothing. VOLATILE, AUTOMATIC and every duration infinite either way.
static struct tt_QosProfile publisher_qos(const struct options* opts) {
    if (!opts->reliable) {
        return (struct tt_QosProfile) {
            // BEST_EFFORT VOLATILE retains nothing: the history sizes nothing for it.
            .history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 1, .bytes = 0},
            .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT,
                            .tracking_words = 0,
                            .reorder_slots = 0,
                            .max_blocking_ns = 0},
            .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
            .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
            .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
            .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
        };
    }
    return (struct tt_QosProfile) {
        .history = {.kind = opts->keep_all ? tt_QOS_HISTORY_KEEP_ALL : tt_QOS_HISTORY_KEEP_LAST,
                    .depth = opts->keep_all ? keep_all_depth() : KEEP_LAST_DEPTH,
                    .bytes = CACHE_ARENA_BYTES},
        // max_blocking_ns 0: the default, which only tt_Publisher_get_actual_qos() reports.
        .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE,
                        .tracking_words = 0,
                        .reorder_slots = 0,
                        .max_blocking_ns = 0},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
}

// The Publisher as the options make it: the message's encode (-M, -G), the profile (-R, -A), a forced run length (-n)
// and the shared region (-B). False when it cannot be made.
static bool setup_publisher(struct tt_Context* node, struct tt_Publisher* pub, const struct options* opts) {
    if (opts->message) {
        message = (uint8_t*)malloc(sample_size);
        if (message == NULL) {
            return false;
        }
        topic.data_encode = message_encode;
#ifndef BENCH_NO_RANGE
        topic.data_encode_range = opts->ranged ? message_encode_range : NULL;
#endif
    }
    topic.max_encoded_size = sample_size;
    struct tt_QosProfile qos = publisher_qos(opts);
    struct tt_PublisherOptions options = {
        .batch = false,
        .heartbeat_piggyback_every = 0,
        .heartbeat_period_ns = 0,
        .ack_solicit_period_ns = 0,
        .ack_solicit_watermark_pct = 0,
        .idle_heartbeat_ns = 0, // tt_IDLE_HEARTBEAT_PERIOD
        .writable_callback = NULL,
        .writable_callback_param = NULL,
        .cache_grow = NULL,
        .large_fragmentation = tt_LARGE_FRAGMENTATION_INHERIT, // the context's: -F sets _tt_CONFIG's
        .ip_datagram_max = 0,
        .large_region_chunks = 0,
        .large_region_chunk_bytes = 0,
    };
#if BENCH_HYBRID
    if (opts->run_units != 0) {
        // Runs of exactly run_units large units (types 11/12's geometry): the run's 28 bytes of framing and the units.
        options.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
        options.ip_datagram_max = (uint16_t)(BENCH_RUN_FRAMING + (opts->run_units * BENCH_LARGE_UNIT));
    }
#endif
#if defined(tt_LARGE_SHM) && tt_LARGE_SHM
    if (opts->region_chunks != 0) {
        options.large_region_chunks = opts->region_chunks;
        options.large_region_chunk_bytes =
            tt_LARGE_CHUNK_BYTES(sample_size > tt_MAX_SAMPLE_LENGTH ? sample_size : tt_MAX_SAMPLE_LENGTH + 1U);
    }
#else
    if (opts->region_chunks != 0) {
        fprintf(stderr, "-B needs a build with large samples and the segment (tt_LARGE_SHM)\n");
        return false;
    }
#endif
    size_t storage_size = tt_Publisher_storage_size(&topic, &qos);
    void* storage = storage_size != 0 ? calloc(1, storage_size) : NULL; // calloc: 8-byte aligned
    if ((storage_size != 0 && storage == NULL) ||
        tt_Node_create_publisher(tt_Context_default_node(node), pub, &topic, "stream", &qos, &options, storage,
                                 storage_size) != tt_RET_OK) {
        fprintf(stderr, "cannot create the publisher\n");
        return false;
    }
    return true;
}

// The publisher's view of its first reader's p, one stderr line each time it changes: the sample about to be
// published, the code (0: none heard, read as p = 1; 0xff00: "none seen") and the n histogram so far - which samples
// went at n > 1 and on what p (hybrid_first_n_pc.sh).
static void trace_peer_p(const struct tt_Context* node, const struct tt_Publisher* pub, uint32_t published) {
#if BENCH_HYBRID
    static int last = -1;
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].context_id == tt_CONTEXT_ID_INVALID) {
            continue;
        }
        int code = (int)node->hybrid.peer_loss_p[pub->peer_acks[i].context_id];
        if (code != last) {
            last = code;
            fprintf(stderr, "PEER_P before_sample=%u code=0x%04x n_hist=", published, (unsigned)code);
            for (int bucket = 0; bucket < tt_HYBRID_N_BUCKETS; bucket++) {
                fprintf(stderr, "%s%lu", bucket == 0 ? "" : "/", (unsigned long)node->hybrid.n_hist[bucket]);
            }
            fprintf(stderr, "\n");
        }
        return;
    }
#else
    (void)node;
    (void)pub;
    (void)published;
#endif
}

static int run_publisher(struct tt_Context* node, const struct options* opts) {
    static struct tt_Publisher pub;
    if (!setup_publisher(node, &pub, opts)) {
        return 1;
    }
    poll_for(node, discovery_ns);
    uint64_t period = (uint64_t)((double)NS_PER_S / opts->rate_hz);
    double halve_after_s = opts->halve_after_s;
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
        trace_peer_p(node, &pub, published);
        if (opts->message) {
            message_fill(published);
        }
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
        if (halve_after_s > 0.0 && (double)(now - begin) / (double)NS_PER_S >= halve_after_s) {
            period *= 2U;
            fprintf(stderr, "RATE_HALVED after %u samples\n", published);
            halve_after_s = 0.0; // once
        }
        next += period;
    }
    poll_for(node, DRAIN_NS);
    double cpu = cpu_ms(&peak_rss_kb) - cpu_before;
    double published_mb = (double)published * (double)sample_size / BYTES_PER_MB;
    printf("RESULT: role=pub size=%u reliable=%d keep_all=%d message=%d ranged=%d range_calls=%lu rate_hz=%.1f "
           "published=%u would_block=%u retransmitted=%u cpu_ms=%.1f cpu_ms_per_mb=%.3f peak_rss_kb=%ld",
           sample_size, opts->reliable ? 1 : 0, opts->keep_all ? 1 : 0, opts->message ? 1 : 0, opts->ranged ? 1 : 0,
           (unsigned long)range_calls, opts->rate_hz, published, refused, pub.retransmitted, cpu,
           published > 0 ? cpu / published_mb : 0.0, peak_rss_kb);
    print_large(node);
    print_hybrid(node, &pub);
    print_discovery(node);
    printf("\n");
    return 0;
}

// From the match point on: a writer with no matched reader sends nothing, so a VOLATILE reader starts where the writer
// learnt it; what is missing between its first and its last sample is a loss, before it is not.
static uint32_t missing_after_first(void) {
    uint32_t missing = 0;
    for (uint32_t i = first_index; delivered > 0 && i < last_index && i < MAX_SAMPLES; i++) {
        missing += seen[i] == 0 ? 1U : 0U;
    }
    return missing;
}

static double latency_us_at(uint32_t counted, uint32_t index) {
    return counted > 0 ? (double)latencies_ns[index] / NS_PER_US : 0.0;
}

// The Subscriber's profile: -R RELIABLE, else BEST_EFFORT; VOLATILE, AUTOMATIC, every duration infinite. KEEP_ALL:
// keep_last_depth 0, never skips in the segment drain, as the legacy create left it.
static struct tt_QosProfile subscriber_qos(const struct options* opts) {
    if (!opts->reliable) {
        return (struct tt_QosProfile) {
            .history = {.kind = tt_QOS_HISTORY_KEEP_ALL, .depth = 0, .bytes = 0},
            .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT,
                            .tracking_words = 0, // the builtin window
                            .reorder_slots = tt_QOS_REORDER_NONE,
                            .max_blocking_ns = 0},
            .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
            .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
            .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
            .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
        };
    }
    bool large = sample_size > tt_MAX_SAMPLE_LENGTH;
    return (struct tt_QosProfile) {
        .history = {.kind = tt_QOS_HISTORY_KEEP_ALL, .depth = 0, .bytes = 0},
        // The widest window for a large sample, which needs it; rmw's 16 words (RMW_TICKLE_TRACKING_WORDS) otherwise,
        // so a small-sample A/B against a parent with a narrower maximum compares the same window.
        // A RELIABLE Subscriber of a sample that fragments holds each fragment in a reorder slot until the sample is
        // whole (tickle.h, tt_Subscriber.reorder_storage) - without one nothing fragmented is ever delivered. One
        // window of slots, as rmw_tickle sizes it, each one datagram's payload of this type. A large sample does not
        // use them.
        .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE,
                        .tracking_words =
                            large ? (uint16_t)tt_RELIABLE_BITMAP_MAX_WORDS : (uint16_t)SMALL_TRACKING_WORDS,
                        .reorder_slots =
                            large ? (uint16_t)tt_QOS_REORDER_NONE : (uint16_t)(SMALL_TRACKING_WORDS * BITS_PER_WORD),
                        .max_blocking_ns = 0},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
}

static int run_subscriber(struct tt_Context* node, const struct options* opts) {
    latencies_ns = (uint64_t*)calloc(MAX_SAMPLES, sizeof(uint64_t));
    topic.max_encoded_size = sample_size;
    struct tt_QosProfile qos = subscriber_qos(opts);
    struct tt_SubscriberOptions options = {.callback = on_sample,
                                           .accept_callback = NULL,
                                           .accept_callback_param = NULL};
    size_t storage_size = tt_Subscriber_storage_size(&topic, &qos);
    void* storage = storage_size != 0 ? calloc(1, storage_size) : NULL; // calloc: 8-byte aligned
    static struct tt_Subscriber sub;
    if (latencies_ns == NULL || (storage_size != 0 && storage == NULL) ||
        tt_Node_create_subscriber(tt_Context_default_node(node), &sub, &topic, "stream", &qos, &options, storage,
                                  storage_size) != tt_RET_OK) {
        fprintf(stderr, "cannot create the subscriber\n");
        return 1;
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
    print_hybrid(node, NULL);
    print_discovery(node);
    printf(" socket_drops=%u", bench_socket_drops(node->hal.sock) + bench_socket_drops(node->hal.data_sock));
#if defined(tt_RX_DRAIN) && tt_RX_DRAIN
    printf(" drain_starts=%lu drain_datagrams=%lu drain_overruns=%lu drain_truncated=%lu",
           (unsigned long)node->hal.drain_starts, (unsigned long)node->hal.drain_datagrams,
           (unsigned long)node->hal.drain_overruns, (unsigned long)node->hal.drain_truncated);
#if TT_HAL_UDP_GRO
    printf(" drain_merged=%lu", (unsigned long)node->hal.drain_merged);
#endif
#endif
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
    printf(" first_index=%u missing_after_first=%u", first_index, missing_after_first());
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

// The reveal re-ask (tt_LinkOptions.repair_reveal, docs/DESIGN.md "Repair reveal", 2026-10-30) from the environment, so
// that a harness's NODE_ENV picks it per tree: TT_REPAIR_REVEAL=off|revealed|all_missing, unset or empty = the
// profile's (0). Anything else stops the bench: an arm must not run a treatment it did not name. The context's resolved
// value is printed as a "REVEAL:" line, so an arm reports the treatment it ran.
static uint8_t repair_reveal_from_env(void) {
    const char* value = getenv("TT_REPAIR_REVEAL");
    if (value == NULL || value[0] == '\0') {
        return (uint8_t)tt_REPAIR_REVEAL_DEFAULT;
    }
    if (strcmp(value, "off") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_OFF;
    }
    if (strcmp(value, "revealed") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_REVEALED;
    }
    if (strcmp(value, "all_missing") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_ALL_MISSING;
    }
    fprintf(stderr, "TT_REPAIR_REVEAL=%s: expected off, revealed or all_missing\n", value);
    exit(2);
}

static void print_repair_reveal(const struct tt_Context* node) {
    const char* value = getenv("TT_REPAIR_REVEAL");
    printf("REVEAL: repair_reveal=%u pace_ns=%u env=%s\n", (unsigned)node->repair_reveal,
           (unsigned)node->repair_reveal_pace_ns, value != NULL ? value : "-");
}

int main(int argc, char** argv) {
    struct options opts;
    if (!parse_options(argc, argv, &opts)) {
        fprintf(stderr,
                "usage: %s pub|sub -s BYTES [-r HZ] -d SECONDS [-R] [-A] [-M [-G]] [-b BROADCAST] [-F auto|mtu|ip] "
                "[-n UNITS] [-X BYTES] [-C] [-H S] [-w US] [-W US] [-K STRIDE] [-B CHUNKS] [-D MS] [-P wifi]\n",
                argv[0]);
        return 2;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    _tt_CONFIG.broadcast = opts.broadcast;
#if BENCH_HYBRID
    if (opts.fragmentation >= 0) {
        _tt_CONFIG.large_fragmentation = (uint8_t)opts.fragmentation;
    }
    if (opts.reassembly_max >= 0) {
        _tt_CONFIG.ip_reassembly_max = (uint16_t)opts.reassembly_max;
    }
    _tt_CONFIG.reassembly_cap_off = opts.cap_off ? 1 : 0;
#endif

    static struct tt_Context node;
    bool created = false;
    if (opts.wifi) {
        struct tt_InitOptions init;
        memset(&init, 0, sizeof(init));
        init.net.broadcast = opts.broadcast;
        init.net.links[0].broadcast = opts.broadcast;
        init.net.links[0].profile = tt_LINK_PROFILE_WIFI;
        init.net.links[0].repair_reveal = repair_reveal_from_env(); // TT_REPAIR_REVEAL
        init.net.link_count = 1;
        init.net.large_fragmentation = _tt_CONFIG.large_fragmentation;
        init.net.ip_reassembly_max = _tt_CONFIG.ip_reassembly_max;
        init.net.reassembly_cap_off = _tt_CONFIG.reassembly_cap_off;
        created = tt_Context_create_with(&node, &init) == tt_RET_OK;
    } else {
        created = tt_Context_create(&node) == tt_RET_OK;
    }
#if BENCH_LARGE
    created = created && tt_Context_set_large_buffers(&node, large_acquire, large_release, NULL) == tt_RET_OK;
#endif
    if (!created) {
        fprintf(stderr, "cannot create the context\n");
        return 1;
    }
    print_repair_reveal(&node);
    int result = opts.publisher ? run_publisher(&node, &opts) : run_subscriber(&node, &opts);
    BENCH_RSTATS(opts.publisher ? "pub" : "sub");
    tt_Context_destroy(&node);
    return result;
}
