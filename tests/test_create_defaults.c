/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tt_Node_create_publisher()/_subscriber() must set every field an announce or the receive path
// reads, whatever the struct held before - a caller need not zero it first.
//
// Found 2026-09-24 by UBSan while auditing the core at a larger buffer size: "load of value 69,
// which is not a valid value for type '_Bool'" reading tt_Publisher.liveliness_manual. keep_all,
// liveliness_manual and the deadline/lease durations of a Publisher, and durable,
// liveliness_manual, the durations and the tracking_bitmaps/tracking_words pair of a Subscriber,
// were left as found. rmw_tickle zero-allocates and core's examples are static, so neither saw it;
// a stack or reused allocation announced garbage QoS bits, and a garbage tracking_bitmaps is a
// pointer the reliable path writes through.
//
// Each test fills the struct with a non-zero byte pattern first, so a field creation forgot is
// visibly wrong rather than accidentally zero.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: endpoint_qos_bits() is static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox

#define GARBAGE 0xA5

static int32_t encode_size_one(struct tt_Data* data) {
    (void)data;
    return 1;
}

// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's fixed signature
static int32_t encode_one(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    (void)len;
    payload[0] = 0;
    return 1;
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

static struct tt_Node node;
static struct tt_Topic topic;

static void init(void) {
    test_mock_reset();
    memset(&node, 0, sizeof(node));
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    memset(&topic, 0, sizeof(topic));
    topic.name = "defaults";
    topic.data_size = 8;
    topic.data_encode_size = encode_size_one;
    topic.data_encode = encode_one;
    topic.data_decode = decode_nothing;
    topic.data_free = free_nothing;
}

static void test_publisher_defaults_override_garbage(void) {
    init();
    static struct tt_Publisher pub;
    memset(&pub, GARBAGE, sizeof(pub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub, &topic, "ep"));

    EXPECT_EQ_U32(0, endpoint_qos_bits((struct tt_Endpoint*)&pub)); // what the announce carries
    EXPECT_TRUE(!pub.reliable && !pub.durable && !pub.keep_all && !pub.liveliness_manual);
    EXPECT_TRUE(pub.lifespan_duration_ns == 0 && pub.deadline_duration_ns == 0);
    EXPECT_TRUE(pub.liveliness_lease_duration_ns == 0);
    EXPECT_TRUE(pub.writable_callback == NULL && pub.writable_callback_param == NULL && !pub.writable_pending);
    EXPECT_TRUE(pub.reliable_cache == NULL);
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        EXPECT_EQ_INT(tt_NODE_ID_INVALID, pub.peer_acks[i].node_id); // no ack slot looks occupied
    }
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        EXPECT_EQ_INT(tt_NODE_ID_INVALID, pub.peers[i].node_id);
    }
}

static void test_subscriber_defaults_override_garbage(void) {
    init();
    static struct tt_Subscriber sub;
    memset(&sub, GARBAGE, sizeof(sub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&node, &sub, &topic, "ep", on_data));

    EXPECT_EQ_U32(0, endpoint_qos_bits((struct tt_Endpoint*)&sub));
    EXPECT_TRUE(!sub.reliable && !sub.durable && !sub.liveliness_manual);
    EXPECT_TRUE(sub.deadline_duration_ns == 0 && sub.liveliness_lease_duration_ns == 0);
    // NULL/0 is documented (tickle.h) as this function's default: "use builtin_tracking[]".
    EXPECT_TRUE(sub.tracking_bitmaps == NULL);
    EXPECT_EQ_INT(0, sub.tracking_words);
    EXPECT_TRUE(sub.reorder_storage == NULL);
}

int main(void) {
    test_publisher_defaults_override_garbage();
    test_subscriber_defaults_override_garbage();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_create_defaults: all tests passed\n");
    return 0;
}
