/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Send pacing (docs/DESIGN.md "Send pacing"): a context whose links are not paced - every ETHERNET link, a context
// with no links configured - sends what it sent before pacing existed: every datagram, its bytes, its destination and
// the moment (the mock clock) it left. Built by tests/wire_identity_send_pacing.sh against the parent's tickle.c and
// this tree's, with -DWIRE_CAPTURE_OUT=path, in the core default build, a fragmenting one
// (-Dtt_MAX_SAMPLE_LENGTH=16000) and rmw_tickle's (a 65507-byte datagram, large samples). The scenario, twice - an
// explicitly configured ETHERNET link, then no links: broadcast and unicast samples, a batch, a long sample, 30
// RELIABLE samples and retransmissions of eight of them; in the large build a 256 KB sample sent through a socket
// buffer that takes 40 datagrams a poll step (the send cursor's waits) and four of its units retransmitted; then
// polling - announces, summaries, HEARTBEATs. Not a test of its own: `make test` does not build it (no test_ prefix).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox: the parent's tickle.c, or this tree's

#ifndef WIRE_CAPTURE_OUT
#define WIRE_CAPTURE_OUT "/dev/null" // tests/wire_identity_send_pacing.sh gives a path
#endif

#define CAPTURE_MAX ((size_t)4U * 1024U * 1024U)
#define POLL_STEPS 400
#define POLL_STEP_NS (5ULL * tt_MILLISECOND)
#if tt_FRAG_ENABLED
#define LONG_SAMPLE 4000U // three fragments at the default
#else
#define LONG_SAMPLE 1400U // the longest a build that does not fragment sends whole
#endif
#define LARGE_SAMPLE (256U * 1024U)
#define ROOM_PER_STEP 40

static uint8_t capture[CAPTURE_MAX];
static uint32_t capture_len;
static uint32_t capture_datagrams;
static uint32_t sample_len = 100U;

// One record a datagram: its length, its destination (0: the HAL's broadcast address), the mock clock when it was
// handed to the HAL, then the bytes as sent.
static void capture_hook(const void* buf, size_t len) {
    if (capture_len + 16U + len > CAPTURE_MAX) {
        test_failures++;
        return;
    }
    uint32_t length = (uint32_t)len;
    memcpy(capture + capture_len, &length, 4);
    memcpy(capture + capture_len + 4, &test_mock_send_hook_ip, 4);
    memcpy(capture + capture_len + 8, &test_mock_now, 8);
    memcpy(capture + capture_len + 16, buf, len);
    capture_len += 16U + length;
    capture_datagrams++;
}

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = (uint8_t)((i * 7U) + 3U);
    }
    return (int32_t)len;
}

static int32_t decode_nothing(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

#if tt_LARGE_SAMPLES
static void* large_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void large_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}
#endif

static struct tt_Context node;
static struct tt_TypeSupport topic;
static struct tt_Publisher best_effort;
static struct tt_Publisher batched;
static struct tt_Publisher reliable;
static struct tt_Subscriber sub;
static uint64_t reliable_storage[(size_t)8U * 1024U * 1024U / sizeof(uint64_t)];
static uint64_t subscriber_storage[(size_t)64U * 1024U / sizeof(uint64_t)];

static void publish(struct tt_Publisher* pub, uint32_t len) {
    sample_len = len;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(pub, (struct tt_Data*)&sample_len));
}

static void poll_steps(int steps) {
    for (int step = 0; step < steps; step++) {
        test_mock_now += POLL_STEP_NS;
#if tt_LARGE_SAMPLES
        test_mock_nonblocking_room = ROOM_PER_STEP; // the socket drained since the last step
#endif
        (void)tt_Context_poll(&node, 0);
    }
}

static void scenario(bool explicit_link) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_mtu = 1500;
    test_mock_send_hook = capture_hook;

    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    if (explicit_link) {
        opts.net.links[0].broadcast = "10.0.0.255"; // every other field 0: ETHERNET, unpaced
        opts.net.link_count = 1;
    }
    memset(&node, 0, sizeof(node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&node, &opts));

    memset(&topic, 0, sizeof(topic));
    topic.name = "wire_send_pacing";
    topic.data_size = 8;
    topic.data_encode_size = sized_encode_size;
    topic.data_encode = sized_encode;
    topic.data_decode = decode_nothing;
    topic.data_free = free_nothing;
    struct tt_Node* default_node = tt_Context_default_node(&node);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(default_node, &best_effort, &topic, "be", &TEST_QOS_UNRELIABLE,
                                                      NULL, NULL, 0));
    struct tt_PublisherOptions batch = {.batch = true};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(default_node, &batched, &topic, "batched", &TEST_QOS_UNRELIABLE,
                                                      &batch, NULL, 0));
    struct tt_QosProfile reliable_qos = tt_QOS_PROFILE_DEFAULT;
    reliable_qos.history.depth = 4; // samples, small and large: room in the storage below for both kinds
    size_t storage = tt_Publisher_storage_size(&topic, &reliable_qos);
    EXPECT_TRUE(storage <= sizeof(reliable_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(default_node, &reliable, &topic, "rel", &reliable_qos, NULL,
                                                      reliable_storage, storage));
    size_t sub_storage = tt_Subscriber_storage_size(&topic, &TEST_QOS_UNRELIABLE);
    EXPECT_TRUE(sub_storage <= sizeof(subscriber_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(default_node, &sub, &topic, "wire_sub", &TEST_QOS_UNRELIABLE,
                                                       &(struct tt_SubscriberOptions) {.callback = on_data},
                                                       sub_storage != 0 ? subscriber_storage : NULL, sub_storage));
#if tt_LARGE_SAMPLES
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&node, large_acquire, large_release, NULL));
#endif

    // Past the unicast threshold: broadcasts. A batch longer than one datagram. A long sample.
    for (uint8_t i = 0; i <= tt_UNICAST_PEER_THRESHOLD; i++) {
        (void)upsert_peer(best_effort.peers, (uint8_t)(100U + i), 0x0a000100U + i, 8282);
        (void)upsert_peer(batched.peers, (uint8_t)(100U + i), 0x0a000100U + i, 8282);
    }
    (void)upsert_peer(reliable.peers, 120, 0x0a000178U, 8282);
    publish(&best_effort, 100U);
    for (int i = 0; i < 15; i++) {
        publish(&batched, 100U);
    }
    publish(&best_effort, LONG_SAMPLE);
    for (int i = 0; i < 30; i++) {
        publish(&reliable, i % 10 == 0 ? LONG_SAMPLE : 100U);
        test_mock_now += tt_MILLISECOND;
    }
    // Retransmissions, to the one reader, of the last samples the history holds.
    struct tt_Peer target = {120, 0x0a000178U, 8282};
    for (uint32_t k = 0; k < 8; k++) {
        (void)retransmit_one_sample(&node, &reliable, reliable.reliable_cache,
                                    reliable_cache_depth(reliable.reliable_cache), reliable.seq_no - k, &target);
    }
    // One peer: unicast.
    memset(best_effort.peers, 0, sizeof(best_effort.peers));
    (void)upsert_peer(best_effort.peers, 120, 0x0a000178U, 8282);
    publish(&best_effort, 100U);
    publish(&best_effort, LONG_SAMPLE);
    poll_steps(20);

#if tt_LARGE_SAMPLES
    // A large sample through a socket buffer of ROOM_PER_STEP datagrams a step, then four of its units again.
    test_mock_nonblocking_room = ROOM_PER_STEP;
    publish(&reliable, LARGE_SAMPLE);
    uint32_t first_unit = reliable.seq_no - 150U;
    poll_steps(POLL_STEPS / 2);
    for (uint32_t k = 0; k < 4; k++) {
        (void)large_resend(&node, &reliable, first_unit + (k * 37U), &target);
    }
#endif
    poll_steps(POLL_STEPS / 2);
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    scenario(true);
    scenario(false);

    EXPECT_TRUE(capture_datagrams >= 40U);
    FILE* out = fopen(WIRE_CAPTURE_OUT, "wb");
    if (out == NULL || fwrite(capture, 1, capture_len, out) != capture_len) {
        fprintf(stderr, "cannot write %s\n", WIRE_CAPTURE_OUT);
        return 1;
    }
    (void)fclose(out);
    printf("wire capture: %u datagrams, %u bytes -> %s\n", capture_datagrams, capture_len, WIRE_CAPTURE_OUT);
    return test_result();
}
