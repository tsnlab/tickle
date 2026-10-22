/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// One node of t1s_pacing_netns.sh: a context with one T1S link, paced (its profile's default) or not (pacing_rate
// tt_PACING_OFF), built as rmw_tickle builds core (a 65507-byte datagram, large samples). The Publisher node has two
// Publishers: a small stream - BEST_EFFORT, SMALL_LEN bytes every SMALL_PERIOD_NS, each carrying the date it was
// published - and, unless `large` is 0, a large stream - RELIABLE KEEP_LAST 2, `large` bytes every LARGE_PERIOD_NS. The
// Subscriber node takes both and measures each small sample's one-way latency against the same host's clock (both
// namespaces read one CLOCK_REALTIME). Each prints one RESULT line from its own counters: what the link resolved to
// (the pacing rate and burst it applies - the arm's treatment), the samples published and received, the small
// stream's latency percentiles, and the pacing and retransmission counters.
//
// Usage: t1s_pacing_node pub|sub on|off CONTEXT_ID BROADCAST SECONDS LARGE_BYTES

// The build the harness gives it (-D), and what a bare compile - a linter's - takes.
#ifndef tt_LARGE_SAMPLES
#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define SMALL_LEN 100U
#define SMALL_PERIOD_NS (10ULL * tt_MILLISECOND)
#define LARGE_PERIOD_NS (500ULL * tt_MILLISECOND)
#define MATCH_WAIT_NS (5ULL * tt_SECOND)
#define DRAIN_NS (1500ULL * tt_MILLISECOND) // the last large sample, its repairs, and their delivery
#define ARGS 7
#define LATENCY_MAX 65536U
#define PERCENT 100U
#define P99 99U
#define P50 50U
#define LARGE_PATTERN_STEP 13U
#define STORAGE_BYTES ((size_t)4U << 20U) // a RELIABLE large endpoint's storage: room for KEEP_LAST 2
// argv: role, arm, context id, broadcast, seconds, large bytes
#define ARG_ROLE 1
#define ARG_ARM 2
#define ARG_CONTEXT_ID 3
#define ARG_BROADCAST 4
#define ARG_SECONDS 5
#define ARG_LARGE 6

// ---- The two topics. A small sample is its publish date and padding; a large one is `large_len` patterned bytes.

static uint32_t large_len;

static int32_t small_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)SMALL_LEN;
}

static int32_t small_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    memset(payload, 0, len);
    memcpy(payload, data, sizeof(uint64_t));
    return (int32_t)len;
}

static uint64_t latencies[LATENCY_MAX];
static uint32_t small_received;
static uint32_t large_received;

static int32_t small_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(uint64_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint64_t));
    return (int32_t)len;
}

static int32_t large_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)large_len;
}

static int32_t large_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = (uint8_t)(i * LARGE_PATTERN_STEP);
    }
    return (int32_t)len;
}

static int32_t large_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    return (int32_t)len;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_small(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    uint64_t sent = 0;
    memcpy(&sent, data, sizeof(sent));
    uint64_t now = tt_get_wall_ns();
    if (small_received < LATENCY_MAX) {
        latencies[small_received] = now > sent ? now - sent : 0;
    }
    small_received++;
}

static void on_large(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    large_received++;
}

static void* large_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void large_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}

static void poll_until(struct tt_Context* ctx, uint64_t until) {
    for (uint64_t now = tt_get_ns(); now < until; now = tt_get_ns()) {
        (void)tt_Context_poll(ctx, (int64_t)(until - now));
    }
}

static struct tt_Context ctx;
static struct tt_TypeSupport small_topic;
static struct tt_TypeSupport large_topic;
static struct tt_Publisher small_pub;
static struct tt_Publisher large_pub;
static struct tt_Subscriber small_sub;
static struct tt_Subscriber large_sub;
static uint64_t large_pub_storage[STORAGE_BYTES / sizeof(uint64_t)];
static uint64_t large_sub_storage[STORAGE_BYTES / sizeof(uint64_t)];
static const struct tt_QosProfile small_qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL},
                                               .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};
static const struct tt_QosProfile large_qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 2},
                                               .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
static uint32_t small_published;
static uint32_t large_published;
static uint32_t large_refused;
static uint8_t small_matched;
static uint8_t large_matched;

static tt_ret_t create_context(bool paced, int context_id, const char* broadcast) {
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.broadcast = broadcast;
    opts.net.context_id = context_id;
    opts.net.links[0].broadcast = broadcast;
    opts.net.links[0].profile = tt_LINK_PROFILE_T1S;
    opts.net.links[0].pacing_rate = paced ? 0 : tt_PACING_OFF;
    opts.net.link_count = 1;
    tt_ret_t created = tt_Context_create_with(&ctx, &opts);
    if (created != tt_RET_OK) {
        fprintf(stderr, "tt_Context_create_with: %d\n", (int)created);
        return created;
    }
    tt_ret_t buffers = tt_Context_set_large_buffers(&ctx, large_acquire, large_release, NULL);
    if (buffers != tt_RET_OK) {
        return buffers;
    }
    small_topic.name = "t1s_pacing_small";
    small_topic.data_size = sizeof(uint64_t);
    small_topic.data_encode_size = small_encode_size;
    small_topic.data_encode = small_encode;
    small_topic.data_decode = small_decode;
    small_topic.data_free = free_nothing;
    small_topic.max_encoded_size = SMALL_LEN;
    large_topic.name = "t1s_pacing_large";
    large_topic.data_size = sizeof(uint64_t);
    large_topic.data_encode_size = large_encode_size;
    large_topic.data_encode = large_encode;
    large_topic.data_decode = large_decode;
    large_topic.data_free = free_nothing;
    return tt_RET_OK;
}

static int create_publishers(void) {
    struct tt_PublisherOptions unbatched = {.batching = tt_SWITCH_DEFAULT}; // T1S's: batched on the 1 ms tick
    if (tt_Node_create_publisher(tt_Context_default_node(&ctx), &small_pub, &small_topic, "small", &small_qos,
                                 &unbatched, NULL, 0) != tt_RET_OK) {
        return 1;
    }
    if (large_len == 0) {
        return 0;
    }
    size_t storage = tt_Publisher_storage_size(&large_topic, &large_qos);
    if (storage > sizeof(large_pub_storage) ||
        tt_Node_create_publisher(tt_Context_default_node(&ctx), &large_pub, &large_topic, "large", &large_qos, NULL,
                                 storage != 0 ? large_pub_storage : NULL, storage) != tt_RET_OK) {
        fprintf(stderr, "large publisher: storage %zu\n", storage);
        return 1;
    }
    return 0;
}

static bool all_matched(void) {
    return tt_Publisher_matched_count(&small_pub) != 0 &&
           (large_len == 0 || tt_Publisher_matched_count(&large_pub) != 0);
}

// Waits up to MATCH_WAIT_NS for both matches, then publishes the small stream every SMALL_PERIOD_NS and a large sample
// every LARGE_PERIOD_NS for `seconds`, polling in between.
static int run_publisher(uint64_t seconds) {
    if (create_publishers() != 0) {
        return 1;
    }
    uint64_t match_deadline = tt_get_ns() + MATCH_WAIT_NS;
    while (!all_matched() && tt_get_ns() < match_deadline) {
        poll_until(&ctx, tt_get_ns() + (10ULL * tt_MILLISECOND));
    }
    small_matched = tt_Publisher_matched_count(&small_pub);
    large_matched = large_len != 0 ? tt_Publisher_matched_count(&large_pub) : 0;
    if (!all_matched()) {
        return 0;
    }
    uint64_t start = tt_get_ns();
    uint64_t end = start + (seconds * tt_SECOND);
    uint64_t next_large = start;
    for (uint64_t next = start; next < end; next += SMALL_PERIOD_NS) {
        uint64_t sent = tt_get_wall_ns();
        small_published += tt_Publisher_publish(&small_pub, (struct tt_Data*)&sent) == tt_RET_OK ? 1U : 0U;
        if (large_len != 0 && next >= next_large) {
            uint64_t unused = 0;
            tt_ret_t ret = tt_Publisher_publish(&large_pub, (struct tt_Data*)&unused);
            large_published += ret == tt_RET_OK ? 1U : 0U;
            large_refused += ret == tt_RET_OK ? 0U : 1U;
            next_large += LARGE_PERIOD_NS;
        }
        poll_until(&ctx, next + SMALL_PERIOD_NS);
    }
    poll_until(&ctx, tt_get_ns() + DRAIN_NS);
    return 0;
}

static int run_subscriber(uint64_t seconds) {
    struct tt_SubscriberOptions small_options = {.callback = on_small};
    if (tt_Node_create_subscriber(tt_Context_default_node(&ctx), &small_sub, &small_topic, "small", &small_qos,
                                  &small_options, NULL, 0) != tt_RET_OK) {
        return 1;
    }
    if (large_len != 0) {
        struct tt_SubscriberOptions large_options = {.callback = on_large};
        size_t storage = tt_Subscriber_storage_size(&large_topic, &large_qos);
        if (storage > sizeof(large_sub_storage) ||
            tt_Node_create_subscriber(tt_Context_default_node(&ctx), &large_sub, &large_topic, "large", &large_qos,
                                      &large_options, storage != 0 ? large_sub_storage : NULL, storage) != tt_RET_OK) {
            fprintf(stderr, "large subscriber: storage %zu\n", storage);
            return 1;
        }
    }
    poll_until(&ctx, tt_get_ns() + (seconds * tt_SECOND));
    return 0;
}

static int compare_u64(const void* left, const void* right) {
    uint64_t lhs = *(const uint64_t*)left;
    uint64_t rhs = *(const uint64_t*)right;
    return (lhs > rhs) - (lhs < rhs);
}

// The small stream's latency at `pct` percent (nearest rank), in us; 0 with no samples.
static uint64_t latency_us(uint32_t count, uint32_t pct) {
    if (count == 0) {
        return 0;
    }
    uint32_t rank = (uint32_t)(((uint64_t)count * pct + PERCENT - 1U) / PERCENT);
    return latencies[(rank != 0 ? rank : 1U) - 1U] / tt_MICROSECOND;
}

int main(int argc, char** argv) {
    if (argc != ARGS) {
        fprintf(stderr, "usage: %s pub|sub on|off CONTEXT_ID BROADCAST SECONDS LARGE_BYTES\n", argv[0]);
        return 2;
    }
    bool publisher = strcmp(argv[ARG_ROLE], "pub") == 0;
    bool paced = strcmp(argv[ARG_ARM], "on") == 0;
    uint64_t seconds = strtoull(argv[ARG_SECONDS], NULL, 10);
    large_len = (uint32_t)strtoul(argv[ARG_LARGE], NULL, 10);
    if (create_context(paced, atoi(argv[ARG_CONTEXT_ID]), argv[ARG_BROADCAST]) != tt_RET_OK) {
        return 1;
    }
    int status = publisher ? run_publisher(seconds) : run_subscriber(seconds);
    if (status != 0) {
        return status;
    }
    uint32_t kept = small_received < LATENCY_MAX ? small_received : LATENCY_MAX;
    qsort(latencies, kept, sizeof(latencies[0]), compare_u64);
    const struct tt_LinkOptions* link = &ctx.opts.net.links[0];
    printf("RESULT role=%s arm=%s pacing_rate=%llu pacing_burst=%u link_mtu=%u small_matched=%u large_matched=%u "
           "small_published=%u large_published=%u large_refused=%u small_received=%u large_received=%u "
           "small_p50_us=%llu small_p99_us=%llu small_max_us=%llu pacing_waits=%llu repairs_deferred=%llu "
           "retransmitted=%u send_waits=%llu tx_datagrams=%llu\n",
           publisher ? "pub" : "sub", argv[ARG_ARM],
           (unsigned long long)(link->pacing_rate == tt_PACING_OFF ? 0 : link->pacing_rate),
           (unsigned)link->pacing_burst, (unsigned)link->mtu, (unsigned)small_matched, (unsigned)large_matched,
           (unsigned)small_published, (unsigned)large_published, (unsigned)large_refused, (unsigned)small_received,
           (unsigned)large_received, (unsigned long long)latency_us(kept, P50),
           (unsigned long long)latency_us(kept, P99), kept != 0 ? latencies[kept - 1U] / tt_MICROSECOND : 0ULL,
           (unsigned long long)ctx.pacing_waits, (unsigned long long)ctx.pacing_repairs_deferred,
           large_len != 0 && publisher ? large_pub.retransmitted : 0U, (unsigned long long)ctx.large.send_waits,
           (unsigned long long)ctx.tx_datagrams);
    (void)tt_Context_destroy(&ctx);
    return 0;
}
