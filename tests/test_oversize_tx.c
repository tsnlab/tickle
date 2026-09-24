/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A submessage no datagram can carry must be refused and counted - never left in tx_buffer.
//
// Found 2026-09-24: a node whose discovery UPDATE outgrew one datagram (16 endpoints with ROS-sized
// names, at the default tt_MAX_BUFFER_LENGTH of 1472) logged "Flush length 1476 exceeds" and kept
// the bytes. Every later send appends behind them and flushes them first, so from then on every
// publish failed too: the node went silent for good. rmw_tickle runs one tt_Node per process, so
// two default rclcpp nodes in one process are already past that line.
//
// These tests check the node stays usable and that the loss is visible (tx_dropped_oversize),
// not merely survived. Announcing a large endpoint list at all is a separate, later change.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: build_and_send_update()/node_flush()/flush_tx() are static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define MAX_ENDPOINTS 40

static int32_t sample_size = 1;

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return sample_size;
}

// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's fixed signature
static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0xAB, len);
    return (int32_t)len;
}

static struct tt_Node node;
static struct tt_Topic topics[MAX_ENDPOINTS];
static struct tt_Publisher pubs[MAX_ENDPOINTS];
static char names[MAX_ENDPOINTS][48];
static uint8_t sample[8];

// A node with `count` publishers named the way rmw_tickle names them - a ROS topic path of ~36
// characters and a ~22-character endpoint name - which puts each UPDATE entity near 91 bytes.
static void init_node_with_endpoints(int count) {
    memset(&node, 0, sizeof(node));
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    sample_size = 1;
    for (int i = 0; i < count; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "/my_robot_node/some_topic_name_%04d", i);
        memset(&topics[i], 0, sizeof(topics[i]));
        topics[i].name = names[i];
        topics[i].data_size = sizeof(sample);
        topics[i].data_encode_size = sized_encode_size;
        topics[i].data_encode = sized_encode;
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pubs[i], &topics[i], "my_robot_node_endpoint"));
    }
}

// After whatever happened before, the node must still be able to publish, and that publish must
// actually reach the wire.
static void expect_node_still_publishes(void) {
    int before = test_mock_send_call_count;
    sample_size = 1;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pubs[0], (struct tt_Data*)sample));
    node_flush(&node, 0, NULL);
    EXPECT_EQ_INT(before + 1, test_mock_send_call_count);
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);
}

static void test_small_update_is_sent_and_counts_nothing(void) {
    // Control: the path the fix must not disturb.
    test_mock_reset();
    init_node_with_endpoints(8);
    EXPECT_TRUE(build_and_send_update(&node, NULL, 0));
    node_flush(&node, 0, NULL);
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    EXPECT_EQ_U32(0, (uint32_t)node.tx_dropped_oversize);
    expect_node_still_publishes();
}

static void test_update_past_one_datagram_is_refused_not_wedged(void) {
    // 20 endpoints: the UPDATE fits tx_buffer (2 x the datagram) but no datagram. It used to be
    // kept, and then failed every flush after it.
    test_mock_reset();
    init_node_with_endpoints(20);
    EXPECT_TRUE(!build_and_send_update(&node, NULL, 0));
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);
    EXPECT_EQ_U32(1, (uint32_t)node.tx_dropped_oversize);
    node_flush(&node, 0, NULL);
    EXPECT_EQ_INT(0, test_mock_send_call_count);
    expect_node_still_publishes();
}

static void test_update_past_tx_buffer_leaves_node_usable(void) {
    // 40 endpoints: does not even fit tx_buffer, so encoding fails before end_encode() - a
    // different refusal, but the node must come out of it just as usable.
    test_mock_reset();
    init_node_with_endpoints(40);
    EXPECT_TRUE(!build_and_send_update(&node, NULL, 0));
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);
    expect_node_still_publishes();
}

static void test_sample_that_cannot_fit_is_refused_and_not_cached(void) {
    // A DATA sample whose payload passes the encode-size check (<= tt_MAX_BUFFER_LENGTH) but whose
    // framed submessage does not fit: the same wedge, reached from publish. It must also not be
    // retained, or a reliable reader could later be offered a sample it was never sent.
    test_mock_reset();
    init_node_with_endpoints(1);
    static struct tt_ReliableCacheIndex index[4];
    static uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(4, tt_MAX_BUFFER_LENGTH * 2)];
    static struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    EXPECT_EQ_INT(tt_RET_OK, tt_ReliableCache_init(&cache, index, 4, arena, sizeof(arena)));
    pubs[0].reliable_cache = &cache;
    pubs[0].reliable = true;

    sample_size = tt_MAX_BUFFER_LENGTH;
    EXPECT_EQ_INT(tt_RET_PROTOCOL_ERROR, tt_Publisher_publish(&pubs[0], (struct tt_Data*)sample));
    EXPECT_EQ_U32(1, (uint32_t)node.tx_dropped_oversize);
    EXPECT_EQ_U32(0, cache.newest_seq_no); // nothing retained
    EXPECT_EQ_U32(0, pubs[0].seq_no);      // and no sequence number consumed

    expect_node_still_publishes();
    EXPECT_EQ_U32(1, cache.newest_seq_no); // the sample that fit is retained as seq 1
}

static void test_flush_of_an_unsendable_buffer_drops_it(void) {
    // flush_tx()'s own guard, reached directly: end_encode() now refuses anything that would get
    // here, so this is the second line of defence - it must drop, not keep.
    test_mock_reset();
    init_node_with_endpoints(1);
    node.tx_tail = tt_MAX_BUFFER_LENGTH + 8;
    EXPECT_TRUE(!flush_tx(&node, node.tx_tail, NULL, 0));
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);
    EXPECT_EQ_U32(1, (uint32_t)node.tx_dropped_oversize);
    expect_node_still_publishes();
}

int main(void) {
    test_small_update_is_sent_and_counts_nothing();
    test_update_past_one_datagram_is_refused_not_wedged();
    test_update_past_tx_buffer_leaves_node_usable();
    test_sample_that_cannot_fit_is_refused_and_not_cached();
    test_flush_of_an_unsendable_buffer_drops_it();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_oversize_tx: all tests passed\n");
    return 0;
}
