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

// Whitebox: tt_Publisher_publish()'s buffering behavior and process_data() (static) are both
// exercised only by the RPC-shaped examples/tests before this file - the topic/pub-sub half of
// the API (tt_Publisher_publish/tt_Node_create_publisher/tt_Node_create_subscriber/process_data)
// had no dedicated unit coverage at all.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define ENDPOINT_ID 0xaabbccdd

static int subscriber_callback_count = 0;
static uint64_t last_time = 0;
static uint16_t last_seq_no = 0;
static uint32_t last_value = 0;
static int data_free_call_count = 0;
static bool decode_should_fail = false;

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
    if (decode_should_fail) {
        return -1;
    }
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void stub_data_free(struct tt_Data* data) {
    (void)data;
    data_free_call_count++;
}

static void stub_subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                     struct tt_Data* data) {
    (void)subscriber;
    subscriber_callback_count++;
    last_time = time;
    last_seq_no = seq_no;
    last_value = *(uint32_t*)data;
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

static void init_publisher(struct tt_Publisher* pub, struct tt_Node* node, struct tt_Topic* topic) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.name = "test_publisher"; // node_update()'s encode_update_entities() needs a name
    pub->node = node;
    pub->topic = topic;
}

static void init_subscriber_registered_on_node(struct tt_Subscriber* sub, struct tt_Node* node,
                                               struct tt_Topic* topic) {
    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = ENDPOINT_ID;
    sub->node = node;
    sub->topic = topic;
    sub->callback = stub_subscriber_callback;

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)sub;
}

// Unlike tt_Client_call() (which flushes immediately - see test_client_call.c), publish batches:
// it must only append to node->tx_buffer and bump pub->seq_no, never call tt_send() itself (see
// DESIGN.md's "RPC flushes immediately; Publish batches"). This holds regardless of pub->peers -
// the broadcast-vs-unicast decision for the batched content lives entirely in node_flush() (see
// the test_node_flush_* cases below), not here.
static void test_publish_buffers_without_flushing(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    uint32_t value = 0x1234abcd;
    tt_ret_t ret = tt_Publisher_publish(&pub, (struct tt_Data*)&value);

    EXPECT_EQ_INT(tt_RET_OK, ret);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count); // batched, not flushed
    EXPECT_TRUE(node.tx_tail > sizeof(struct tt_Header));  // something was appended
    EXPECT_EQ_U32(1, (uint32_t)pub.seq_no);
}

// A publish that can't fit in the remaining tx buffer must roll back to the pre-call tx_tail and
// leave seq_no untouched - mirrors test_client_call.c's outstanding-call rejection: no partial
// state on failure.
static void test_publish_rolls_back_on_out_of_buffer(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    node.tx_size = node.tx_tail; // no room left at all
    uint32_t old_tx_tail = node.tx_tail;

    uint32_t value = 0;
    tt_ret_t ret = tt_Publisher_publish(&pub, (struct tt_Data*)&value);

    EXPECT_EQ_INT(tt_RET_OUT_OF_BUFFER, ret);
    EXPECT_EQ_U32(old_tx_tail, node.tx_tail);
    EXPECT_EQ_U32(0, (uint32_t)pub.seq_no);
}

// node_flush() must broadcast (not unicast) when the Publisher has no known peers yet - the
// pre-existing, unchanged default behavior discovery falls back to before it's learned anyone.
static void test_node_flush_broadcasts_with_no_known_peers(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    uint32_t value = 0x1234abcd;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    node_flush(&node, 0, NULL);

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);
}

// At or under tt_UNICAST_PEER_THRESHOLD known peers on the node's one Publisher, node_flush()
// must unicast the whole batched buffer to each of them instead of broadcasting - batching (the
// publish() call itself never flushes, see test_publish_buffers_without_flushing) is preserved;
// only the destination changes at flush time.
static void test_node_flush_unicasts_to_known_publisher_peers_at_or_under_threshold(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    pub.peers[0] = (struct tt_Peer) {.node_id = 2, .ip = 0xc0a80a02, .port = 8282};
    pub.peers[1] = (struct tt_Peer) {.node_id = 3, .ip = 0xc0a80a03, .port = 8283};

    uint32_t value = 0x1234abcd;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    node_flush(&node, 0, NULL);

    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_to_call_count); // one unicast per known peer
    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_call_count);    // and no broadcast on top
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);
}

// More known peers than tt_UNICAST_PEER_THRESHOLD must fall back to broadcast instead of
// unicasting to all of them.
static void test_node_flush_broadcasts_when_peer_count_exceeds_threshold(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    for (int i = 0; i < tt_UNICAST_PEER_THRESHOLD + 1; i++) {
        pub.peers[i] =
            (struct tt_Peer) {.node_id = (uint8_t)(2 + i), .ip = 0xc0a80a00 + (uint8_t)(2 + i), .port = 8282};
    }

    uint32_t value = 0x1234abcd;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    node_flush(&node, 0, NULL);

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// A still-batched UPDATE announce (tx_has_pending_update) must force node_flush() to broadcast,
// even with an otherwise-eligible peer count - it's the regression test for the real bug this
// design was built to avoid: an UPDATE has to reach the whole segment, not just the peers a
// Publisher happens to already know, and tx_buffer is one shared buffer flushed as a unit.
static void test_node_flush_broadcasts_when_update_is_pending(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    pub.peers[0] = (struct tt_Peer) {.node_id = 2, .ip = 0xc0a80a02, .port = 8282};
    node.tx_has_pending_update = true; // simulates node_update() having just batched an UPDATE

    uint32_t value = 0x1234abcd;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    node_flush(&node, 0, NULL);

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    EXPECT_TRUE(!node.tx_has_pending_update); // cleared - it just went out in that broadcast
}

// A node with more than one Publisher must keep broadcasting even if one of them has an
// eligible peer count - tx_buffer could hold batched DATA from either Publisher, and unicasting
// to just one's peers would misdirect (or simply drop, for the other Subscribers) the other's.
static void test_node_flush_broadcasts_when_multiple_publishers_on_node(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub1;
    struct tt_Publisher pub2;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub1, &node, &topic);
    init_publisher(&pub2, &node, &topic);
    node.endpoint_count = 2;
    node.endpoints[0] = (struct tt_Endpoint*)&pub1;
    node.endpoints[1] = (struct tt_Endpoint*)&pub2;

    pub1.peers[0] = (struct tt_Peer) {.node_id = 2, .ip = 0xc0a80a02, .port = 8282};

    uint32_t value = 0x1234abcd;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub1, (struct tt_Data*)&value));

    node_flush(&node, 0, NULL);

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// node_update() must mark tx_has_pending_update so node_flush() knows to keep broadcasting while
// this announce is still sitting batched, unflushed, in tx_buffer.
static void test_node_update_sets_pending_update_flag(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    EXPECT_TRUE(!node.tx_has_pending_update);

    node_update(&node, 0, NULL);

    EXPECT_TRUE(node.tx_has_pending_update);
}

// Builds a DataHeader + 4-byte payload at the start of node->rx_buffer, returning the tail
// offset (matching what process_packet() would have handed process_data()).
static uint32_t write_data(struct tt_Node* node, uint32_t seq_no, uint64_t timestamp, uint32_t value) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node->rx_buffer;
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = timestamp;

    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node->rx_buffer + tail, &value, sizeof(value));
    return tail + sizeof(value);
}

// A registered subscriber must have its callback invoked exactly once, with the decoded value
// and the sender's timestamp/seq_no passed through - and the decoded tt_Data must be freed
// afterwards (topic->data_free), since process_data owns that buffer's lifetime.
static void test_process_data_dispatches_to_subscriber(void) {
    test_mock_reset();
    subscriber_callback_count = 0;
    data_free_call_count = 0;
    decode_should_fail = false;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_data(&node, 42, 12345, 0xdeadbeef);

    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(12345, (uint32_t)last_time);
    EXPECT_EQ_U32(42, (uint32_t)last_seq_no);
    EXPECT_EQ_U32(0xdeadbeef, last_value);
    EXPECT_EQ_U32(1, (uint32_t)data_free_call_count);
}

// An endpoint_id nobody subscribed to (e.g. meant for a different node sharing the broadcast
// domain) must be ignored, not treated as an error, and must not invoke any callback - same
// contract as process_callrequest()'s unknown-endpoint case (see test_process_callrequest.c).
static void test_process_data_unknown_endpoint_is_ignored(void) {
    test_mock_reset();
    subscriber_callback_count = 0;
    decode_should_fail = false;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node.rx_buffer;
    data_header->endpoint_id = 0xdeadbeef; // not ENDPOINT_ID
    data_header->seq_no = 1;
    data_header->timestamp = 1;
    uint32_t tail = sizeof(struct tt_DataHeader);

    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail));
    EXPECT_EQ_U32(0, (uint32_t)subscriber_callback_count);
}

// A payload the topic's own codec rejects (corrupt/truncated CDR) must fail the whole submessage
// (so process_one_submessage's caller can drop the packet) without ever invoking the subscriber
// callback or freeing a tt_Data that was never successfully decoded.
static void test_process_data_decode_failure_is_reported(void) {
    test_mock_reset();
    subscriber_callback_count = 0;
    data_free_call_count = 0;
    decode_should_fail = true;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_data(&node, 1, 1, 0);

    EXPECT_TRUE(!process_data(&node, &header, node.rx_buffer, 0, tail));
    EXPECT_EQ_U32(0, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(0, (uint32_t)data_free_call_count);
}

int main(void) {
    test_publish_buffers_without_flushing();
    test_publish_rolls_back_on_out_of_buffer();
    test_node_flush_broadcasts_with_no_known_peers();
    test_node_flush_unicasts_to_known_publisher_peers_at_or_under_threshold();
    test_node_flush_broadcasts_when_peer_count_exceeds_threshold();
    test_node_flush_broadcasts_when_update_is_pending();
    test_node_flush_broadcasts_when_multiple_publishers_on_node();
    test_node_update_sets_pending_update_flag();
    test_process_data_dispatches_to_subscriber();
    test_process_data_unknown_endpoint_is_ignored();
    test_process_data_decode_failure_is_reported();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_publish_subscribe: all tests passed\n");
    return 0;
}
