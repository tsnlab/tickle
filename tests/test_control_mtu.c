/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A node built with a larger tt_MAX_BUFFER_LENGTH (rmw_tickle's, up to 65507) must still only send
// datagrams a node on core defaults can receive - unless one sample is large because its type is,
// and then that sample goes on its own. tt_CONTROL_MAX_LENGTH (config.h) is the rule; this is
// its test. Plan's review of the storage design, 2026-09-24: an MCU on 1472 would otherwise
// truncate a batched or announced datagram and never discover the larger node.
//
// This binary is built with its own, larger tt_MAX_BUFFER_LENGTH - defined before any TickLE
// header, so everything here, including the whitebox tickle.c, sees it. The rest of the suite
// runs at the default, where the two limits are equal and none of this can be observed.

#define tt_MAX_BUFFER_LENGTH 8192

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: node_flush()/build_and_send_update() are static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox

_Static_assert(tt_CONTROL_MAX_LENGTH == 1472, "the default control limit, whatever the buffer");

#define MAX_DATAGRAMS 64
static uint32_t datagram_len[MAX_DATAGRAMS];
static uint8_t datagram_type[MAX_DATAGRAMS];
static int datagram_count;

static void capture(const void* buf, size_t len) {
    if (datagram_count < MAX_DATAGRAMS) {
        datagram_len[datagram_count] = (uint32_t)len;
        datagram_type[datagram_count] =
            ((const struct tt_SubmessageHeader*)((const uint8_t*)buf + sizeof(struct tt_Header)))->type;
        datagram_count++;
    }
}

static int32_t sample_size;

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return sample_size;
}

// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's fixed signature
static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0x5A, len);
    return (int32_t)len;
}

#define ENDPOINTS 30
static struct tt_Node node;
static struct tt_Topic topics[ENDPOINTS];
static struct tt_Publisher pubs[ENDPOINTS];
static char names[ENDPOINTS][48];
static uint8_t sample[8];

static void init(int endpoints) {
    test_mock_reset();
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    for (int i = 0; i < endpoints; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "/my_robot_node/some_topic_name_%04d", i);
        memset(&topics[i], 0, sizeof(topics[i]));
        topics[i].name = names[i];
        topics[i].data_size = sizeof(sample);
        topics[i].data_encode_size = sized_encode_size;
        topics[i].data_encode = sized_encode;
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pubs[i], &topics[i], "my_robot_node_endpoint"));
    }
    datagram_count = 0;
    test_mock_send_hook = capture;
}

static void test_batched_samples_stay_within_the_control_limit(void) {
    // Ten 400-byte samples on a batching publisher: 4000 bytes, which the 8192 buffer could send as
    // one datagram. None may exceed 1472, and all ten must still go out.
    init(1);
    pubs[0].batch = true;
    sample_size = 400;
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pubs[0], (struct tt_Data*)sample));
    }
    node_flush(&node, 0, NULL);
    uint32_t sent = 0;
    for (int d = 0; d < datagram_count; d++) {
        EXPECT_TRUE(datagram_len[d] <= tt_CONTROL_MAX_LENGTH);
        sent += datagram_len[d];
    }
    EXPECT_TRUE(datagram_count >= 3);                      // 4000 bytes cannot fit fewer
    EXPECT_TRUE(sent > 10 * 400);                          // every sample went, with its framing
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail); // nothing left behind
    EXPECT_EQ_U32(0, (uint32_t)node.tx_dropped_oversize);
}

static void test_one_large_sample_goes_alone_at_full_size(void) {
    // A 5000-byte sample is large because its type is: it may use the larger buffer, on its own.
    init(1);
    sample_size = 5000;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pubs[0], (struct tt_Data*)sample));
    node_flush(&node, 0, NULL);
    EXPECT_EQ_INT(1, datagram_count);
    EXPECT_TRUE(datagram_len[0] > tt_CONTROL_MAX_LENGTH && datagram_len[0] <= tt_MAX_BUFFER_LENGTH);
}

static void test_large_sample_is_not_joined_by_batched_ones(void) {
    // A batch pending when a large sample arrives goes first, on its own, and nothing is added to
    // the large one's datagram: whatever shares a datagram stays within the control limit.
    init(1);
    pubs[0].batch = true;
    sample_size = 200;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pubs[0], (struct tt_Data*)sample));
    sample_size = 5000;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pubs[0], (struct tt_Data*)sample));
    sample_size = 200;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pubs[0], (struct tt_Data*)sample));
    node_flush(&node, 0, NULL);
    EXPECT_EQ_INT(3, datagram_count);
    int large = 0;
    for (int d = 0; d < datagram_count; d++) {
        if (datagram_len[d] > tt_CONTROL_MAX_LENGTH) {
            large++;
            EXPECT_TRUE(datagram_len[d] < 5000 + 64); // the large sample and its framing, nothing else
        }
    }
    EXPECT_EQ_INT(1, large);
}

static void test_announce_splits_at_the_control_limit(void) {
    // 30 endpoints (~2.7 KB) would fit one 8192-byte UPDATE; the announce is split for 1472 anyway.
    init(ENDPOINTS);
    EXPECT_TRUE(build_and_send_update(&node, NULL, 0));
    node_flush(&node, 0, NULL);
    EXPECT_TRUE(datagram_count >= 2);
    for (int d = 0; d < datagram_count; d++) {
        EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_UPDATE_PART, datagram_type[d]);
        EXPECT_TRUE(datagram_len[d] <= tt_CONTROL_MAX_LENGTH);
    }
}

int main(void) {
    test_batched_samples_stay_within_the_control_limit();
    test_one_large_sample_goes_alone_at_full_size();
    test_large_sample_is_not_joined_by_batched_ones();
    test_announce_splits_at_the_control_limit();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_control_mtu: all tests passed\n");
    return 0;
}
