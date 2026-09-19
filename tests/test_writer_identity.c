/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: Milestone 47 (rmw_tickle/PLAN.md) - the real, root-caused fix for a confirmed
// cross-instance data-mixing gap ("Data consistency violated. Received sample with not strictly
// higher id.", a real rmw_tickle async-mode crash). Before this milestone, struct tt_Subscriber's
// RELIABLE tracking was a single flat ack_seq_no/received_bitmap watermark shared by *any* sender
// matching endpoint_id - two independent Publisher instances (different node_id, or the same
// node_id with a different entity_id) publishing under the same topic+endpoint name had their
// seq_no streams silently conflated. These tests reproduce the exact multi-writer shape the
// original whitebox repro used and confirm each writer now gets its own independent struct tt_
// WriterProxy entry, keyed by (node_id, entity_id) - see that struct's own doc comment, tickle.h.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID_A 50
#define REMOTE_NODE_ID_B 51
#define ENDPOINT_ID 0xaabbccdd
#define TEST_SENDER_IP 0x0a000001
#define TEST_SENDER_PORT 12345

static int32_t stub_data_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_data_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len,
                                bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void stub_data_free(struct tt_Data* data) {
    (void)data;
}

static int subscriber_callback_count = 0;

static void stub_subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                     struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
    subscriber_callback_count++;
}

static void init_node_and_topic(struct tt_Node* node, struct tt_Topic* topic) {
    memset(node, 0, sizeof(*node));
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;

    memset(topic, 0, sizeof(*topic));
    topic->name = "test_topic";
    topic->data_size = sizeof(uint32_t);
    topic->data_encode_size = stub_data_encode_size;
    topic->data_encode = stub_data_encode;
    topic->data_decode = stub_data_decode;
    topic->data_free = stub_data_free;
}

static void init_subscriber_registered_on_node(struct tt_Subscriber* sub, struct tt_Node* node,
                                               struct tt_Topic* topic) {
    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = ENDPOINT_ID;
    sub->node = node;
    sub->topic = topic;
    sub->callback = stub_subscriber_callback;
    sub->reliable = true;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].node_id = tt_NODE_ID_INVALID; // all empty - see struct tt_WriterProxy
    }

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)sub;
}

static void init_header(struct tt_Header* header, uint8_t source_node_id) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = source_node_id;
}

// Builds a DataHeader + 4-byte payload at the start of node->rx_buffer, returning the tail offset
// (matching what process_packet() would have handed process_data()) - unlike test_reliable_
// pubsub.c's own identically-shaped helper, this one also stamps entity_id, since these tests are
// specifically about disambiguating senders that would otherwise share tracking state.
static uint32_t write_data(struct tt_Node* node, uint32_t entity_id, uint32_t seq_no, uint64_t timestamp,
                           uint32_t value) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node->rx_buffer;
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = timestamp;
    data_header->entity_id = entity_id;

    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node->rx_buffer + tail, &value, sizeof(value));
    return tail + sizeof(value);
}

// The actual reported-crash shape, reproduced and proven fixed: a Subscriber already deep into a
// stream from one Publisher (mirroring the real incident's own seq_no 4788) must not have its own
// watermark disturbed at all by a single low-seq_no DATA packet from a *different* Publisher
// sharing the identical topic+endpoint name - each gets its own independent struct tt_WriterProxy
// entry now, keyed by (node_id, entity_id), instead of one shared flat ack_seq_no. Before this
// milestone, the second packet would have been compared against the first writer's own ack_seq_no
// and reported exactly the "not strictly higher id" shape the real crash hit.
static void test_two_different_nodes_same_endpoint_track_independently(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header_a;
    init_header(&header_a, REMOTE_NODE_ID_A);

    // Writer A streams in order, well past a small test-scale "long-running" watermark.
    for (uint32_t seq = 1; seq <= 500; seq++) {
        uint32_t tail = write_data(&node, /*entity_id=*/100, seq, (uint64_t)seq * 100, seq);
        EXPECT_TRUE(process_data(&node, &header_a, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    }
    struct tt_WriterProxy* proxy_a = find_writer_proxy(&sub, REMOTE_NODE_ID_A, 100);
    EXPECT_TRUE(proxy_a != NULL);
    EXPECT_EQ_U32(501, proxy_a->ack_seq_no);

    // Writer B (a different node_id entirely) sends its own very first sample, seq_no 1 - far
    // "behind" writer A's own watermark by wire seq_no alone, but a genuinely different, valid
    // writer, not a regression of writer A's own stream.
    struct tt_Header header_b;
    init_header(&header_b, REMOTE_NODE_ID_B);
    uint32_t tail = write_data(&node, /*entity_id=*/200, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header_b, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // The exact assertion the original crash violated: writer A's own watermark must be
    // completely untouched by writer B's unrelated, independent stream.
    EXPECT_EQ_U32(501, proxy_a->ack_seq_no);
    struct tt_WriterProxy* proxy_b = find_writer_proxy(&sub, REMOTE_NODE_ID_B, 200);
    EXPECT_TRUE(proxy_b != NULL);
    EXPECT_EQ_U32(2, proxy_b->ack_seq_no);

    // Both writers' DATA is still delivered to the application (real DDS also delivers samples
    // from every matched Writer - see rmw_tickle/PLAN.md's own Milestone 47 writeup) - what
    // changed is that their RELIABLE tracking no longer collides, not whether either is delivered.
    EXPECT_EQ_U32(501, (uint32_t)subscriber_callback_count);
}

// The narrower, same-remote-node variant: two Publisher instances that happen to share a node_id
// (e.g. two local Publishers on one remote tt_Node with the same topic+endpoint name, Milestone
// 35) must still be disambiguated purely by entity_id.
static void test_two_entity_ids_on_same_node_track_independently(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID_A);

    // entity_id 10 streams three in-order samples (each an exact-match watermark advance);
    // entity_id 20 streams only one - both share node_id, only entity_id tells them apart.
    for (uint32_t seq = 1; seq <= 3; seq++) {
        uint32_t tail = write_data(&node, /*entity_id=*/10, seq, (uint64_t)seq * 100, seq);
        EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    }
    uint32_t tail = write_data(&node, /*entity_id=*/20, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_WriterProxy* proxy_10 = find_writer_proxy(&sub, REMOTE_NODE_ID_A, 10);
    struct tt_WriterProxy* proxy_20 = find_writer_proxy(&sub, REMOTE_NODE_ID_A, 20);
    EXPECT_TRUE(proxy_10 != NULL);
    EXPECT_TRUE(proxy_20 != NULL);
    EXPECT_TRUE(proxy_10 != proxy_20);
    EXPECT_EQ_U32(4, proxy_10->ack_seq_no); // 3 in-order samples landed
    EXPECT_EQ_U32(2, proxy_20->ack_seq_no); // its own independent, fresh watermark
}

// A gap opening against one writer must not affect a different, healthy writer's own retry state -
// each struct tt_WriterProxy's own acknack_scheduled/retry tracking is genuinely independent, not
// just its ack_seq_no/received_bitmap.
static void test_gap_on_one_writer_does_not_affect_another(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header_a;
    init_header(&header_a, REMOTE_NODE_ID_A);
    struct tt_Header header_b;
    init_header(&header_b, REMOTE_NODE_ID_B);

    // Writer A: in-order, healthy, no gap.
    uint32_t tail = write_data(&node, /*entity_id=*/1, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header_a, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* proxy_a = find_writer_proxy(&sub, REMOTE_NODE_ID_A, 1);
    EXPECT_TRUE(proxy_a != NULL);
    EXPECT_TRUE(!proxy_a->acknack_scheduled);

    // Writer B: opens a real gap (seq_no 1 skipped).
    tail = write_data(&node, /*entity_id=*/2, 2, 200, 2);
    EXPECT_TRUE(process_data(&node, &header_b, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* proxy_b = find_writer_proxy(&sub, REMOTE_NODE_ID_B, 2);
    EXPECT_TRUE(proxy_b != NULL);
    EXPECT_TRUE(proxy_b->acknack_scheduled);

    // Writer A must still be completely healthy - B's own gap never touched it.
    EXPECT_TRUE(!proxy_a->acknack_scheduled);
    EXPECT_EQ_U32(2, proxy_a->ack_seq_no);
}

int main(void) {
    test_two_different_nodes_same_endpoint_track_independently();
    test_two_entity_ids_on_same_node_track_independently();
    test_gap_on_one_writer_does_not_affect_another();

    printf("test_writer_identity: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
