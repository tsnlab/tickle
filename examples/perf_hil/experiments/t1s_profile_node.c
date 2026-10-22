/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// One node of t1s_profile_netns.sh: a context with one link of the given profile, and a best-effort Publisher or
// Subscriber of 100-byte samples. The Publisher waits up to 5 s for a match, then for `seconds` publishes a burst of 20
// samples every 10 ms (1.6 Mbit/s of payload, well inside 10 Mbit/s), polling between bursts. Each prints one RESULT
// line from the context's own fields and counters: what its link resolved to (MTU, threshold, batching, the datagram
// length it cuts at) and the datagrams it sent and received.
//
// Usage: t1s_profile_node pub|sub ethernet|ethernet-batch|t1s CONTEXT_ID BROADCAST SECONDS

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define SAMPLE_LEN 100U
#define BURST 20
#define BURST_PERIOD_NS (10ULL * tt_MILLISECOND)
#define DRAIN_NS (200ULL * tt_MILLISECOND) // the last batch tick, and its delivery
#define ARGS 6

static uint32_t received;

static int32_t encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)SAMPLE_LEN;
}

static int32_t encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    memcpy(payload, data, len);
    return (int32_t)len;
}

static int32_t decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    return (int32_t)len;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    received++;
}

static void poll_until(struct tt_Context* ctx, uint64_t until) {
    for (uint64_t now = tt_get_ns(); now < until; now = tt_get_ns()) {
        (void)tt_Context_poll(ctx, (int64_t)(until - now));
    }
}

static struct tt_Context ctx;
static struct tt_TypeSupport topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static const struct tt_QosProfile qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL},
                                         .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};
static uint32_t published;
static uint8_t matched;

static tt_ret_t create_context(const char* arm, int context_id, const char* broadcast) {
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.broadcast = broadcast;
    opts.net.context_id = context_id;
    opts.net.links[0].broadcast = broadcast;
    opts.net.links[0].profile = strcmp(arm, "t1s") == 0 ? tt_LINK_PROFILE_T1S : tt_LINK_PROFILE_ETHERNET;
    opts.net.link_count = 1;
    tt_ret_t created = tt_Context_create_with(&ctx, &opts);
    if (created != tt_RET_OK) {
        fprintf(stderr, "tt_Context_create_with: %d\n", (int)created);
        return created;
    }
    topic.name = "t1s_profile_check";
    topic.data_size = SAMPLE_LEN;
    topic.data_encode_size = encode_size;
    topic.data_encode = encode;
    topic.data_decode = decode;
    topic.data_free = free_nothing;
    topic.max_encoded_size = SAMPLE_LEN;
    return tt_RET_OK;
}

// Waits up to 5 s for a match, then publishes a burst every BURST_PERIOD_NS for `seconds`.
static int run_publisher(const char* arm, uint64_t seconds) {
    // ethernet-batch: an ETHERNET context whose Publisher batches - what the T1S profile gives, without its MTU.
    struct tt_PublisherOptions options = {.batching =
                                              strcmp(arm, "ethernet-batch") == 0 ? tt_SWITCH_ON : tt_SWITCH_DEFAULT};
    if (tt_Node_create_publisher(tt_Context_default_node(&ctx), &pub, &topic, "t1s_check", &qos, &options, NULL, 0) !=
        tt_RET_OK) {
        return 1;
    }
    uint64_t match_deadline = tt_get_ns() + (5ULL * tt_SECOND);
    while (tt_Publisher_matched_count(&pub) == 0 && tt_get_ns() < match_deadline) {
        poll_until(&ctx, tt_get_ns() + (10ULL * tt_MILLISECOND));
    }
    matched = tt_Publisher_matched_count(&pub);
    uint64_t end = tt_get_ns() + (seconds * tt_SECOND);
    uint8_t sample[SAMPLE_LEN] = {0};
    for (uint64_t next = tt_get_ns(); matched != 0 && next < end; next += BURST_PERIOD_NS) {
        for (int i = 0; i < BURST; i++) {
            sample[0] = (uint8_t)published;
            published += tt_Publisher_publish(&pub, (struct tt_Data*)sample) == tt_RET_OK ? 1U : 0U;
        }
        poll_until(&ctx, next + BURST_PERIOD_NS);
    }
    poll_until(&ctx, tt_get_ns() + DRAIN_NS);
    return 0;
}

static int run_subscriber(uint64_t seconds) {
    struct tt_SubscriberOptions options = {.callback = on_data};
    if (tt_Node_create_subscriber(tt_Context_default_node(&ctx), &sub, &topic, "t1s_check", &qos, &options, NULL, 0) !=
        tt_RET_OK) {
        return 1;
    }
    poll_until(&ctx, tt_get_ns() + (seconds * tt_SECOND));
    return 0;
}

int main(int argc, char** argv) {
    if (argc != ARGS) {
        fprintf(stderr, "usage: %s pub|sub ethernet|ethernet-batch|t1s CONTEXT_ID BROADCAST SECONDS\n", argv[0]);
        return 2;
    }
    bool publisher = strcmp(argv[1], "pub") == 0;
    const char* arm = argv[2];
    uint64_t seconds = strtoull(argv[5], NULL, 10);
    if (create_context(arm, atoi(argv[3]), argv[4]) != tt_RET_OK) {
        return 1;
    }
    int status = publisher ? run_publisher(arm, seconds) : run_subscriber(seconds);
    if (status != 0) {
        return status;
    }
    const struct tt_LinkOptions* link = &ctx.opts.net.links[0];
    printf("RESULT role=%s arm=%s link_mtu=%u interface_mtu=%d control_max=%u unicast_threshold=%u batch_default=%d "
           "pub_batch=%d matched=%u published=%u received=%u tx_datagrams=%llu rx_datagrams=%llu\n",
           publisher ? "pub" : "sub", arm, (unsigned)link->mtu, (int)ctx.links[0].resolved_mtu,
           (unsigned)ctx.control_max, (unsigned)ctx.links[0].unicast_threshold, ctx.batch_default ? 1 : 0,
           pub.batch ? 1 : 0, (unsigned)matched, (unsigned)published, (unsigned)received,
           (unsigned long long)ctx.tx_datagrams, (unsigned long long)ctx.rx_datagrams);
    (void)tt_Context_destroy(&ctx);
    return 0;
}
