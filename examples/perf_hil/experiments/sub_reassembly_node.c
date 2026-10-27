/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// sub_reassembly_node - one side of sub_reassembly_pc_ab.sh: the mixed shape of docs/DESIGN.md section 8, "Reassembly
// slots" (the user, 2026-10-27). A BEST_EFFORT stream of fragmented samples beside N RELIABLE ones whose
// Subscribers have no reorder buffer (tt_QOS_REORDER_NONE, rmw_tickle's shape for an unbounded type), on one node,
// plus a control the change cannot touch: a RELIABLE stream of 64-byte samples (whole DATAs, never a reassembly
// slot) read by a Subscriber with a reorder buffer.
//
//   pub -n N -s BYTES -r HZ -d SECONDS [-c CONTROL_HZ] [-b BROADCAST]
//       N RELIABLE KEEP_LAST 10 Publishers on topics sr_rel_<i>, one BEST_EFFORT on sr_be (both BYTES at HZ), one
//       RELIABLE KEEP_LAST 10 on sr_ctl (64 bytes at CONTROL_HZ, default 100).
//   sub -n N -s BYTES -d SECONDS [-b BROADCAST]
//       The Subscribers: sr_rel_<i> RELIABLE with no reorder buffer, sr_be BEST_EFFORT, sr_ctl RELIABLE with the
//       window's reorder buffer. The types are unbounded (max_encoded_size 0): a Subscriber that reassembles in slots
//       owns one of tt_MAX_SAMPLE_LENGTH where the change is built in.
//
// RESULT line fields. pub: published per class (rel is the sum over the N streams). sub: delivered, torn (bytes not
// the publisher's) and latency median / p99 in microseconds (publish to decode, CLOCK_MONOTONIC: both namespaces are
// this host) per class; cpu_ms (user + system, the whole process); the node's frag counters; and the treatment,
// own_slot_bytes: what tt_Subscriber_storage_size() asks of the BEST_EFFORT Subscriber - 0 on a tree without per-
// Subscriber slots, and the slot on one with them. An arm that does not print what it was built as is not an arm.

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

#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000ULL
#define US_PER_MS 1000.0
#define MAX_STREAMS 16
#define DEFAULT_CONTROL_HZ 100.0
#define CONTROL_BYTES 64U
#define KEEP_LAST_DEPTH 10U
#define HEADER_BYTES 16U // send time (8), index (4), stream (4)
#define INDEX_OFFSET 8U
#define STREAM_OFFSET 12U
#define MAX_LATENCIES 200000U
#define POLL_NS 1000000LL              // 1 ms
#define DISCOVERY_NS (2ULL * NS_PER_S) // before the first publish: every Subscriber matched
#define DRAIN_NS NS_PER_S              // the publisher stays for repairs after its last sample
#define SUB_EXTRA_NS (4ULL * NS_PER_S) // the subscriber outlives the publisher's run and drain
#define BE_STREAM 0xbeU                // the stream field of a BEST_EFFORT sample
#define CTL_STREAM 0xc0U               // and of a control one
#define PATTERN_STEP 7U
#define PERCENTILE_50 0.5
#define PERCENTILE_99 0.99
#define BASE_10 10

enum sample_class { CLASS_REL, CLASS_BE, CLASS_CTL, CLASS_COUNT };

static volatile sig_atomic_t stop_requested;
static uint32_t sample_size;
static uint32_t current_index;
static uint32_t current_stream;

struct class_tally {
    uint32_t delivered;
    uint32_t torn;
    uint32_t latencies_kept;
    uint64_t* latencies_ns;
};
static struct class_tally tally[CLASS_COUNT];

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - CLOCK_MONOTONIC: a glibc-private header
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static uint8_t pattern(uint32_t i, uint32_t index, uint32_t stream) {
    return (uint8_t)((i * PATTERN_STEP) + index + stream);
}

static int32_t large_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_size;
}

static int32_t control_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)CONTROL_BYTES;
}

static int32_t encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    uint64_t sent = now_ns();
    memcpy(payload, &sent, sizeof(sent));
    memcpy(payload + INDEX_OFFSET, &current_index, sizeof(current_index));
    memcpy(payload + STREAM_OFFSET, &current_stream, sizeof(current_stream));
    for (uint32_t i = HEADER_BYTES; i < len; i++) {
        payload[i] = pattern(i, current_index, current_stream);
    }
    return (int32_t)len;
}

static void tally_sample(enum sample_class class_of, const uint8_t* payload, uint32_t len, uint32_t expected_len) {
    uint64_t arrived = now_ns();
    struct class_tally* kept = &tally[class_of];
    uint64_t sent = 0;
    uint32_t index = 0;
    uint32_t stream = 0;
    bool intact = len == expected_len && len >= HEADER_BYTES;
    if (intact) {
        memcpy(&sent, payload, sizeof(sent));
        memcpy(&index, payload + INDEX_OFFSET, sizeof(index));
        memcpy(&stream, payload + STREAM_OFFSET, sizeof(stream));
        for (uint32_t i = HEADER_BYTES; i < len && intact; i++) {
            intact = payload[i] == pattern(i, index, stream);
        }
    }
    if (!intact) {
        kept->torn++;
        return;
    }
    if (kept->latencies_kept < MAX_LATENCIES) {
        kept->latencies_ns[kept->latencies_kept++] = arrived - sent;
    }
    kept->delivered++;
}

static int32_t rel_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    tally_sample(CLASS_REL, payload, len, sample_size);
    return 0;
}

static int32_t be_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    tally_sample(CLASS_BE, payload, len, sample_size);
    return 0;
}

static int32_t ctl_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    tally_sample(CLASS_CTL, payload, len, CONTROL_BYTES);
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

// Unbounded types (max_encoded_size 0), as rmw_tickle's for an image or a point cloud.
static struct tt_TypeSupport rel_type = {.name = "sr_large",
                                         .data_size = 8,
                                         .data_encode_size = large_encode_size,
                                         .data_encode = encode,
                                         .data_decode = rel_decode,
                                         .data_free = free_nothing};
static struct tt_TypeSupport be_type = {.name = "sr_large",
                                        .data_size = 8,
                                        .data_encode_size = large_encode_size,
                                        .data_encode = encode,
                                        .data_decode = be_decode,
                                        .data_free = free_nothing};
static struct tt_TypeSupport ctl_type = {.name = "sr_control",
                                         .data_size = 8,
                                         .data_encode_size = control_encode_size,
                                         .data_encode = encode,
                                         .data_decode = ctl_decode,
                                         .data_free = free_nothing};

static struct tt_QosProfile qos_of(bool reliable, bool reorder_buffer) {
    return (struct tt_QosProfile) {
        .history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = KEEP_LAST_DEPTH, .bytes = 0},
        .reliability = {.kind = reliable ? tt_QOS_RELIABILITY_RELIABLE : tt_QOS_RELIABILITY_BEST_EFFORT,
                        .tracking_words = 0,
                        .reorder_slots = reliable && !reorder_buffer ? tt_QOS_REORDER_NONE : 0,
                        .max_blocking_ns = 0},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
}

struct options {
    bool publisher;
    uint32_t streams;
    double rate_hz;
    double control_hz;
    double duration_s;
    char* broadcast;
};

static bool parse_options(int argc, char** argv, struct options* opts) {
    memset(opts, 0, sizeof(*opts));
    opts->control_hz = DEFAULT_CONTROL_HZ;
    if (argc < 2) {
        return false;
    }
    opts->publisher = strcmp(argv[1], "pub") == 0;
    if (!opts->publisher && strcmp(argv[1], "sub") != 0) {
        return false;
    }
    int opt = 0;
    optind = 2;                                                // NOLINT(misc-include-cleaner) - see <unistd.h> above
    while ((opt = getopt(argc, argv, "n:s:r:d:c:b:")) != -1) { // NOLINT(misc-include-cleaner)
        switch (opt) {
        case 'n':
            opts->streams = (uint32_t)strtoul(optarg, NULL, BASE_10); // NOLINT(misc-include-cleaner)
            break;
        case 's':
            sample_size = (uint32_t)strtoul(optarg, NULL, BASE_10); // NOLINT(misc-include-cleaner)
            break;
        case 'r':
            opts->rate_hz = strtod(optarg, NULL);
            break;
        case 'd':
            opts->duration_s = strtod(optarg, NULL);
            break;
        case 'c':
            opts->control_hz = strtod(optarg, NULL);
            break;
        case 'b':
            opts->broadcast = optarg;
            break;
        default:
            return false;
        }
    }
    return opts->streams >= 1 && opts->streams <= MAX_STREAMS && sample_size > HEADER_BYTES &&
           sample_size <= tt_MAX_SAMPLE_LENGTH && opts->duration_s > 0.0 &&
           (!opts->publisher || (opts->rate_hz > 0.0 && opts->control_hz > 0.0));
}

static double cpu_ms(void) {
    struct rusage usage; // NOLINT(misc-include-cleaner) - see <sys/resource.h> above
    getrusage(RUSAGE_SELF, &usage);
    double user_ms = ((double)usage.ru_utime.tv_sec * US_PER_MS) + ((double)usage.ru_utime.tv_usec / US_PER_MS);
    double system_ms = ((double)usage.ru_stime.tv_sec * US_PER_MS) + ((double)usage.ru_stime.tv_usec / US_PER_MS);
    return user_ms + system_ms;
}

static void poll_for(struct tt_Context* node, uint64_t span_ns) {
    uint64_t start = now_ns();
    while (!stop_requested && now_ns() - start < span_ns) {
        (void)tt_Context_poll(node, POLL_NS);
    }
}

// Caller storage as tt_*_storage_size() asks; calloc is 8-aligned. NULL with *made true when none is needed.
static void* storage_for(size_t size, bool* made) {
    void* storage = size != 0 ? calloc(1, size) : NULL;
    *made = size == 0 || storage != NULL;
    return storage;
}

static struct tt_Publisher rel_pub[MAX_STREAMS];
static struct tt_Publisher be_pub;
static struct tt_Publisher ctl_pub;
static char rel_topic[MAX_STREAMS][16];

static bool create_publisher(struct tt_Context* node, struct tt_Publisher* pub, struct tt_TypeSupport* type,
                             const char* topic, bool reliable) {
    struct tt_QosProfile qos = qos_of(reliable, false);
    bool made = false;
    size_t size = tt_Publisher_storage_size(type, &qos);
    void* storage = storage_for(size, &made);
    return made && tt_Node_create_publisher(tt_Context_default_node(node), pub, type, topic, &qos, NULL, storage,
                                            size) == tt_RET_OK;
}

static int run_publisher(struct tt_Context* node, const struct options* opts) {
    bool made = true;
    for (uint32_t i = 0; i < opts->streams && made; i++) {
        made = create_publisher(node, &rel_pub[i], &rel_type, rel_topic[i], true);
    }
    made = made && create_publisher(node, &be_pub, &be_type, "sr_be", false) &&
           create_publisher(node, &ctl_pub, &ctl_type, "sr_ctl", true);
    if (!made) {
        fprintf(stderr, "cannot create the publishers\n");
        return 1;
    }
    poll_for(node, DISCOVERY_NS);
    uint64_t period = (uint64_t)((double)NS_PER_S / opts->rate_hz);
    uint64_t control_period = (uint64_t)((double)NS_PER_S / opts->control_hz);
    uint64_t begin = now_ns();
    uint64_t next = begin;
    uint64_t next_control = begin;
    uint32_t published[CLASS_COUNT] = {0};
    uint32_t round = 0;
    uint32_t control_index = 0;
    double cpu_before = cpu_ms();
    while (!stop_requested && (double)(now_ns() - begin) / (double)NS_PER_S < opts->duration_s) {
        uint64_t now = now_ns();
        if (now >= next_control) {
            current_index = control_index++;
            current_stream = CTL_STREAM;
            published[CLASS_CTL] += tt_Publisher_publish(&ctl_pub, (struct tt_Data*)&current_index) == tt_RET_OK;
            next_control += control_period;
        }
        if (now >= next) {
            // One round: every RELIABLE stream's sample, then the BEST_EFFORT one's, as a camera rig's frames.
            current_index = round;
            for (uint32_t i = 0; i < opts->streams; i++) {
                current_stream = i;
                published[CLASS_REL] += tt_Publisher_publish(&rel_pub[i], (struct tt_Data*)&current_index) == tt_RET_OK;
            }
            current_stream = BE_STREAM;
            published[CLASS_BE] += tt_Publisher_publish(&be_pub, (struct tt_Data*)&current_index) == tt_RET_OK;
            round++;
            next += period;
        }
        uint64_t wake = next < next_control ? next : next_control;
        now = now_ns();
        if (wake > now) {
            (void)tt_Context_poll(node, (int64_t)(wake - now));
        }
    }
    poll_for(node, DRAIN_NS);
    printf("RESULT: role=pub streams=%u size=%u rate_hz=%.1f rel_published=%u be_published=%u ctl_published=%u "
           "cpu_ms=%.1f\n",
           opts->streams, sample_size, opts->rate_hz, published[CLASS_REL], published[CLASS_BE], published[CLASS_CTL],
           cpu_ms() - cpu_before);
    return 0;
}

static int compare_u64(const void* first, const void* second) {
    uint64_t lhs = *(const uint64_t*)first;
    uint64_t rhs = *(const uint64_t*)second;
    return (lhs > rhs) - (lhs < rhs);
}

// The latency at `fraction` of the class's kept samples, in microseconds; 0 with none.
static double latency_us(struct class_tally* kept, double fraction) {
    if (kept->latencies_kept == 0) {
        return 0.0;
    }
    qsort(kept->latencies_ns, kept->latencies_kept, sizeof(uint64_t), compare_u64);
    uint32_t pos = (uint32_t)(fraction * (double)(kept->latencies_kept - 1U));
    return (double)kept->latencies_ns[pos] / (double)NS_PER_US;
}

static struct tt_Subscriber rel_sub[MAX_STREAMS];
static struct tt_Subscriber be_sub;
static struct tt_Subscriber ctl_sub;

static bool create_subscriber(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_TypeSupport* type,
                              const char* topic, struct tt_QosProfile qos) {
    bool made = false;
    size_t size = tt_Subscriber_storage_size(type, &qos);
    void* storage = storage_for(size, &made);
    const struct tt_SubscriberOptions options = {.callback = on_sample};
    return made && tt_Node_create_subscriber(tt_Context_default_node(node), sub, type, topic, &qos, &options, storage,
                                             size) == tt_RET_OK;
}

static int run_subscriber(struct tt_Context* node, const struct options* opts) {
    for (int cls = 0; cls < CLASS_COUNT; cls++) {
        tally[cls].latencies_ns = (uint64_t*)calloc(MAX_LATENCIES, sizeof(uint64_t));
        if (tally[cls].latencies_ns == NULL) {
            return 1;
        }
    }
    bool made = true;
    for (uint32_t i = 0; i < opts->streams && made; i++) {
        made = create_subscriber(node, &rel_sub[i], &rel_type, rel_topic[i], qos_of(true, false));
    }
    made = made && create_subscriber(node, &be_sub, &be_type, "sr_be", qos_of(false, false)) &&
           create_subscriber(node, &ctl_sub, &ctl_type, "sr_ctl", qos_of(true, true));
    if (!made) {
        fprintf(stderr, "cannot create the subscribers\n");
        return 1;
    }
    const struct tt_QosProfile be_qos = qos_of(false, false);
    size_t own_slot_bytes = tt_Subscriber_storage_size(&be_type, &be_qos);
    double cpu_before = cpu_ms();
    poll_for(node, DISCOVERY_NS + (uint64_t)(opts->duration_s * (double)NS_PER_S) + DRAIN_NS + SUB_EXTRA_NS);
    double cpu = cpu_ms() - cpu_before;
    printf("RESULT: role=sub streams=%u size=%u own_slot_bytes=%zu rel_delivered=%u rel_torn=%u rel_p50_us=%.1f "
           "rel_p99_us=%.1f be_delivered=%u be_torn=%u be_p50_us=%.1f be_p99_us=%.1f ctl_delivered=%u ctl_torn=%u "
           "ctl_p50_us=%.1f ctl_p99_us=%.1f cpu_ms=%.1f frag_reassembled=%lu frag_abandoned=%lu frag_dropped=%lu "
           "frag_duplicate=%lu\n",
           opts->streams, sample_size, own_slot_bytes, tally[CLASS_REL].delivered, tally[CLASS_REL].torn,
           latency_us(&tally[CLASS_REL], PERCENTILE_50), latency_us(&tally[CLASS_REL], PERCENTILE_99),
           tally[CLASS_BE].delivered, tally[CLASS_BE].torn, latency_us(&tally[CLASS_BE], PERCENTILE_50),
           latency_us(&tally[CLASS_BE], PERCENTILE_99), tally[CLASS_CTL].delivered, tally[CLASS_CTL].torn,
           latency_us(&tally[CLASS_CTL], PERCENTILE_50), latency_us(&tally[CLASS_CTL], PERCENTILE_99), cpu,
           (unsigned long)node->frag_reassembled, (unsigned long)node->frag_abandoned,
           (unsigned long)node->frag_dropped, (unsigned long)node->frag_duplicate);
    return 0;
}

int main(int argc, char** argv) {
    struct options opts;
    if (!parse_options(argc, argv, &opts)) {
        fprintf(stderr, "usage: %s pub|sub -n STREAMS -s BYTES [-r HZ] -d SECONDS [-c CONTROL_HZ] [-b BROADCAST]\n",
                argv[0]);
        return 2;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    for (uint32_t i = 0; i < opts.streams; i++) {
        (void)snprintf(rel_topic[i], sizeof(rel_topic[i]), "sr_rel_%u", i);
    }
    static struct tt_Context node;
    struct tt_InitOptions init;
    memset(&init, 0, sizeof(init));
    init.net.broadcast = opts.broadcast;
    if (tt_Context_create_with(&node, &init) != tt_RET_OK) {
        fprintf(stderr, "cannot create the context\n");
        return 1;
    }
    int result = opts.publisher ? run_publisher(&node, &opts) : run_subscriber(&node, &opts);
    tt_Context_destroy(&node);
    return result;
}
