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
#include <stdlib.h>
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

// pub->batch defaults to false (tt_Node_create_publisher() - here, init_publisher()'s own
// memset() to 0 has the same effect), the same default RPC (tt_Client_call(), see
// test_client_call.c) already had: tt_Publisher_publish() must flush immediately, not just
// append to node->tx_buffer and wait for node_flush()'s next tick (DESIGN.md's "RPC and Publish
// flush immediately by default; batching is opt-in"). No known peers here, so that flush must be
// a broadcast - see test_publish_unicasts_to_known_peers_at_or_under_threshold below for the
// peer-list case, which mirrors tt_Client_call()'s own equivalent decision exactly.
static void test_publish_flushes_immediately_by_default(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    uint32_t value = 0x1234abcd;
    tt_ret_t ret = tt_Publisher_publish(&pub, (struct tt_Data*)&value);

    EXPECT_EQ_INT(tt_RET_OK, ret);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);    // flushed immediately, as a broadcast
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // not a unicast - no known peers
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);    // and the buffer was reset by the flush
    EXPECT_EQ_U32(1, (uint32_t)pub.seq_no);
}

// pub->batch == true opts a specific Publisher back into the pre-existing behavior: append to
// node->tx_buffer and bump pub->seq_no, never call tt_send() itself, leaving node_flush()'s own
// tt_NODE_TX_INTERVAL tick (see the test_node_flush_* cases below, which all set this too) to
// decide broadcast vs. unicast for the whole accumulated buffer at once - the escape hatch for a
// Publisher that really does call tt_Publisher_publish() several times in a row and would rather
// coalesce those into fewer packets than minimize any one message's own latency.
static void test_publish_batches_when_opted_in(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    pub.batch = true;

    uint32_t value = 0x1234abcd;
    tt_ret_t ret = tt_Publisher_publish(&pub, (struct tt_Data*)&value);

    EXPECT_EQ_INT(tt_RET_OK, ret);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count); // batched, not flushed
    EXPECT_TRUE(node.tx_tail > sizeof(struct tt_Header));  // something was appended
    EXPECT_EQ_U32(1, (uint32_t)pub.seq_no);
}

// At or under tt_UNICAST_PEER_THRESHOLD known peers, the immediate flush (pub->batch == false,
// the default) must unicast to each of them instead of broadcasting - mirrors tt_Client_call()'s
// own identical decision (test_client_call.c) exactly, now that Publish makes it too instead of
// only ever deferring it to node_flush().
static void test_publish_unicasts_to_known_peers_at_or_under_threshold(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    pub.peers[0] = (struct tt_Peer) {.node_id = 2, .ip = 0xc0a80a02, .port = 8282};
    pub.peers[1] = (struct tt_Peer) {.node_id = 3, .ip = 0xc0a80a03, .port = 8283};

    uint32_t value = 0x1234abcd;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_to_call_count); // one unicast per known peer
    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_call_count);    // and no broadcast on top
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);
}

// More known peers than tt_UNICAST_PEER_THRESHOLD must fall back to broadcasting instead.
static void test_publish_broadcasts_when_peer_count_exceeds_threshold(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    for (int i = 0; i < tt_UNICAST_PEER_THRESHOLD + 1; i++) {
        pub.peers[i] =
            (struct tt_Peer) {.node_id = (uint8_t)(2 + i), .ip = 0xc0a80a00 + (uint8_t)(2 + i), .port = 8282};
    }

    uint32_t value = 0x1234abcd;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// The shared-tx_buffer guard: an immediate flush must not unicast (even with an otherwise
// eligible peer count) when something else - a real, still-batched UPDATE announce from
// node_update(), not just the tx_has_pending_update flag alone - is already sitting unflushed
// ahead of this DATA submessage, since unicasting would only reach these peers, not the whole
// segment that pending content needs. Mirrors tt_Client_call()'s own identical guard
// (old_tx_tail == sizeof(struct tt_Header)).
static void test_publish_falls_back_to_broadcast_when_buffer_not_empty(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    pub.peers[0] = (struct tt_Peer) {.node_id = 2, .ip = 0xc0a80a02, .port = 8282};

    node_update(&node, 0, NULL); // really batches an UPDATE into tx_buffer, advancing tx_tail
    EXPECT_TRUE(node.tx_tail > sizeof(struct tt_Header));

    uint32_t value = 0x1234abcd;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    // publish() itself still flushes immediately here (pub->batch is still false) - it just must
    // broadcast the combined buffer (UPDATE + DATA) rather than unicasting only to its own peers,
    // which would have reached pub's Subscribers but not the rest of the segment the UPDATE needs.
    // A trailing node_flush() call would be a no-op (nothing left to flush) - not needed to
    // observe the result.
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail);
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
// pub.batch = true opts this Publisher into the pre-1(publish-flushes-immediately) behavior so
// there's actually something left for node_flush() itself to decide - see tickle.h's own doc
// comment on tt_Publisher.batch.
static void test_node_flush_broadcasts_with_no_known_peers(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    pub.batch = true;
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
// must unicast the whole batched buffer to each of them instead of broadcasting - batching (opted
// into via pub.batch = true - see test_publish_batches_when_opted_in) is preserved; only the
// destination changes at flush time.
static void test_node_flush_unicasts_to_known_publisher_peers_at_or_under_threshold(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    pub.batch = true;
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
    pub.batch = true;
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
    pub.batch = true;
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

// A node with more than one *batching* Publisher must keep broadcasting even if one of them has
// an eligible peer count - tx_buffer could hold batched DATA from either Publisher, and
// unicasting to just one's peers would misdirect (or simply drop, for the other Subscribers) the
// other's. Only relevant to pub.batch == true Publishers at all: the default (immediate-flush)
// ones never share a buffer window with another Publisher's own call in the first place - each
// flushes (and resets tx_tail) before the next one's own call can begin.
static void test_node_flush_broadcasts_when_multiple_publishers_on_node(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub1;
    struct tt_Publisher pub2;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub1, &node, &topic);
    init_publisher(&pub2, &node, &topic);
    pub1.batch = true;
    pub2.batch = true;
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

// tt_Node_destroy() must actually put its final entity-less UPDATE on the wire (broadcast), not
// just batch it into tx_buffer and then close the socket - otherwise peers never learn the node
// left. Regression test for that: the farewell has to be a real tt_send(), before tt_close().
static void test_node_destroy_broadcasts_farewell(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    EXPECT_EQ_INT(tt_RET_OK, (int)tt_Node_destroy(&node));

    EXPECT_TRUE(test_mock_send_call_count >= 1);              // farewell went out
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // as a broadcast, not a unicast
    EXPECT_TRUE(!node.tx_has_pending_update);                 // flush cleared it
}

// Milestone 47 "goodbye" - destroying one entity (while the node itself keeps running, unlike
// tt_Node_destroy() above) must also broadcast the now-reduced entity list right away, not just
// batch it for node_update()'s own next periodic tick (up to tt_NODE_UPDATE_INTERVAL later) -
// this is what actually narrows the window a departed Publisher could still be confused with a
// newly-arrived one under (rmw_tickle/PLAN.md's own Milestone 47 writeup). A second, surviving
// Publisher on the same node must remain correctly registered afterwards.
static void test_publisher_destroy_broadcasts_goodbye_immediately(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub1;
    struct tt_Publisher pub2;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub1, &node, &topic);
    init_publisher(&pub2, &node, &topic);
    pub2.endpoint.name = "test_publisher_2";
    node.endpoint_count = 2;
    node.endpoints[0] = (struct tt_Endpoint*)&pub1;
    node.endpoints[1] = (struct tt_Endpoint*)&pub2;

    EXPECT_EQ_INT(tt_RET_OK, (int)tt_Publisher_destroy(&pub1));

    EXPECT_TRUE(test_mock_send_call_count >= 1);              // goodbye went out immediately
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // as a broadcast, not a unicast
    EXPECT_TRUE(!node.tx_has_pending_update);                 // flush cleared it - not left batched
    EXPECT_EQ_U32(1, node.endpoint_count);                    // pub1 gone, pub2 survives
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&pub2);
}

// Builds a DataHeader + 4-byte payload at the start of node->rx_buffer, returning the tail
// offset (matching what process_packet() would have handed process_data()).
static uint32_t write_data_from(struct tt_Node* node, uint32_t seq_no, uint64_t timestamp, uint32_t value,
                                uint32_t entity_id) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node->rx_buffer;
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = timestamp;
    data_header->entity_id = entity_id;

    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node->rx_buffer + tail, &value, sizeof(value));
    return tail + sizeof(value);
}

static uint32_t write_data(struct tt_Node* node, uint32_t seq_no, uint64_t timestamp, uint32_t value) {
    return write_data_from(node, seq_no, timestamp, value, 0);
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

    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
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

    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
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

    EXPECT_TRUE(!process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(0, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(0, (uint32_t)data_free_call_count);
}

// Milestone 35 (rmw_tickle/PLAN.md) - add_endpoint_to_node() no longer rejects a second local
// Subscriber sharing an already-registered (kind, id): two independent Subscriptions to the same
// topic, in the same process, is now legal (previously this whole scenario could never even be
// set up - the second tt_Node_create_subscriber() call would have failed outright). process_data()
// must deliver the arriving sample to *both*, each with its own independent decode/callback/free,
// not just whichever one find_endpoint()'s own single-match lookup would have picked.
static void test_process_data_fans_out_to_every_matching_subscriber(void) {
    test_mock_reset();
    subscriber_callback_count = 0;
    data_free_call_count = 0;
    decode_should_fail = false;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub_a;
    struct tt_Subscriber sub_b;
    init_node_and_topic(&node, &topic);

    memset(&sub_a, 0, sizeof(sub_a));
    sub_a.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub_a.endpoint.id = ENDPOINT_ID;
    sub_a.node = &node;
    sub_a.topic = &topic;
    sub_a.callback = stub_subscriber_callback;

    memset(&sub_b, 0, sizeof(sub_b));
    sub_b.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub_b.endpoint.id = ENDPOINT_ID; // same topic name as sub_a - the case this milestone unblocks
    sub_b.node = &node;
    sub_b.topic = &topic;
    sub_b.callback = stub_subscriber_callback;

    node.endpoint_count = 2;
    node.endpoints[0] = (struct tt_Endpoint*)&sub_a;
    node.endpoints[1] = (struct tt_Endpoint*)&sub_b;

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_data(&node, 42, 12345, 0xdeadbeef);

    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count); // both subscribers got their own delivery
    EXPECT_EQ_U32(2, (uint32_t)data_free_call_count);      // each with its own independent decode+free
    EXPECT_EQ_U32(12345, (uint32_t)last_time);
    EXPECT_EQ_U32(42, (uint32_t)last_seq_no);
    EXPECT_EQ_U32(0xdeadbeef, last_value);
}

// The delivery-order diagnostic must actually move when delivery is out of order - an instrument
// nobody has seen fire is not evidence of anything when it reads zero. Drives the counters
// independently, because they answer different questions (tt_Subscriber.delivered, tickle.h).
//
// Calls record_delivery_order() directly rather than driving it through process_data(), and that
// is deliberate rather than convenient: BEST_EFFORT now discards a sample no newer than the last
// delivered, so out-of-order samples no longer reach the recorder by that route at all. Driving
// the recorder through the policy would make this test silently stop testing the recorder the
// moment the policy tightened - which is exactly what happened when the discard landed. The
// policy has its own test below.
//
// What these counters mean after the ordering work: they measure what ESCAPES the ordering
// policy. In normal operation they should read zero, and a non-zero value is a defect rather
// than a statistic.
static void test_delivery_order_diagnostic_counts_disorder(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    // Two in order from one writer: nothing is out of order, and the first sample has no
    // predecessor to be out of order against.
    record_delivery_order(&node, &sub, 10, 1000, REMOTE_NODE_ID, 7, node.rx_via_data_port);
    record_delivery_order(&node, &sub, 11, 2000, REMOTE_NODE_ID, 7, node.rx_via_data_port);
    EXPECT_EQ_U32(2, sub.delivered);
    EXPECT_EQ_U32(0, sub.out_of_order);
    EXPECT_EQ_U32(0, sub.timestamp_not_newer);
    EXPECT_EQ_U32(0, sub.writer_switches);

    // Same writer, both axes backwards: one sample, counted once on each.
    record_delivery_order(&node, &sub, 10, 1000, REMOTE_NODE_ID, 7, node.rx_via_data_port);
    EXPECT_EQ_U32(3, sub.delivered);
    EXPECT_EQ_U32(1, sub.out_of_order);
    EXPECT_EQ_U32(1, sub.timestamp_not_newer);
    EXPECT_EQ_U32(0, sub.writer_switches);

    // A different entity on the same node, with a lower seq_no but a newer timestamp. The seq_no
    // must NOT be read as disorder - it belongs to a different writer's own counting - while the
    // switch itself is recorded. This is the case that would otherwise report every legitimate
    // change of speaker as a fault.
    record_delivery_order(&node, &sub, 1, 3000, REMOTE_NODE_ID, 9, node.rx_via_data_port);
    EXPECT_EQ_U32(4, sub.delivered);
    EXPECT_EQ_U32(1, sub.out_of_order);
    EXPECT_EQ_U32(1, sub.timestamp_not_newer);
    EXPECT_EQ_U32(1, sub.writer_switches);

    // ... but a backwards timestamp across that switch IS counted, because that is the predicate
    // the application itself checks and it does not care which writer spoke.
    record_delivery_order(&node, &sub, 12, 2500, REMOTE_NODE_ID, 7, node.rx_via_data_port);
    EXPECT_EQ_U32(5, sub.delivered);
    EXPECT_EQ_U32(1, sub.out_of_order);
    EXPECT_EQ_U32(2, sub.timestamp_not_newer);
    EXPECT_EQ_U32(2, sub.writer_switches);

    // Everything so far arrived on the well-known socket, so nothing has flipped yet.
    EXPECT_EQ_U32(0, sub.via_socket_flips);

    // Two on the data socket then one back: two transitions, not three arrivals on a different
    // socket. The boundary is what is counted, because that is where an interleaving reader can
    // misorder; a run of samples on one socket offers no such chance however long it is.
    node.rx_via_data_port = true;
    record_delivery_order(&node, &sub, 13, 4000, REMOTE_NODE_ID, 7, node.rx_via_data_port);
    EXPECT_EQ_U32(1, sub.via_socket_flips);
    record_delivery_order(&node, &sub, 14, 5000, REMOTE_NODE_ID, 7, node.rx_via_data_port);
    EXPECT_EQ_U32(1, sub.via_socket_flips); // same socket again: not a transition
    node.rx_via_data_port = false;
    record_delivery_order(&node, &sub, 15, 6000, REMOTE_NODE_ID, 7, node.rx_via_data_port);
    EXPECT_EQ_U32(2, sub.via_socket_flips);

    EXPECT_EQ_U32(8, sub.delivered);
    EXPECT_EQ_U32(1, sub.out_of_order);
    EXPECT_EQ_U32(2, sub.timestamp_not_newer);
    EXPECT_EQ_U32(2, sub.writer_switches);
}

static void expect_delivery_line(bool destroy_subscriber_first) {
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

    uint32_t tail = write_data_from(&node, 1, 1000, 1, 5);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));

    char* buffer = NULL;
    size_t buffer_len = 0;
    FILE* captured = open_memstream(&buffer, &buffer_len);
    EXPECT_TRUE(captured != NULL);
    tt_log_set_output(captured);
    tt_log_set_level(TT_LOG_INFO);

    if (destroy_subscriber_first) {
        tt_Subscriber_destroy(&sub); // the rmw order
        tt_Node_destroy(&node);
    } else {
        tt_Node_destroy(&node); // the plain two-node-example order
    }

    fflush(captured);
    tt_log_set_output(stderr);

    EXPECT_TRUE(buffer != NULL);
    if (buffer != NULL) {
        // Present at all, and exactly once - reporting from both paths must not double-count a
        // Subscriber that took the first one.
        const char* first = strstr(buffer, "delivery: delivered=1");
        EXPECT_TRUE(first != NULL);
        if (first != NULL) {
            EXPECT_TRUE(strstr(first + 1, "delivery: delivered=1") == NULL);
        }
    }
    fclose(captured);
    free(buffer);
}

static void test_delivery_counters_are_reported_on_both_teardown_orders(void) {
    expect_delivery_line(/*destroy_subscriber_first=*/true);  // rmw: subscription, then node
    expect_delivery_line(/*destroy_subscriber_first=*/false); // examples: node with endpoints attached
}

// BEST_EFFORT must discard a sample no newer than the last delivered from the same writer, and
// must not discard one from a different writer merely because that writer is further along.
//
// This is the DDS reader policy. TickLE used to deliver everything in arrival order, which is a
// weaker guarantee than any DDS implementation offers - applications written against DDS assert
// on it, and performance_test does exactly that.
static void test_best_effort_discards_out_of_order(void) {
    test_mock_reset();
    subscriber_callback_count = 0;
    decode_should_fail = false;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reliable = false;

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_data_from(&node, 5, 5000, 1, 7);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);

    // Strictly newer: delivered.
    tail = write_data_from(&node, 6, 6000, 2, 7);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count);

    // Older, and an exact duplicate: both discarded, and the callback never sees them.
    tail = write_data_from(&node, 4, 4000, 3, 7);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    tail = write_data_from(&node, 6, 6000, 4, 7);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(2, sub.out_of_order_discarded);

    // A gap is still delivered - BEST_EFFORT gives up on missing samples, it does not wait for
    // them. This is the half that must NOT change.
    tail = write_data_from(&node, 20, 20000, 5, 7);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(3, (uint32_t)subscriber_callback_count);

    // A different writer on the same node, starting from seq 1. It must be delivered: seq_no
    // counts per writer, and comparing across writers would discard a perfectly good sample
    // because somebody else was further along. This is also the restarted-Publisher case, which
    // gets a new entity_id and therefore a new proxy.
    tail = write_data_from(&node, 1, 100, 6, 9);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(4, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(2, sub.out_of_order_discarded);
}

int main(void) {
    test_publish_flushes_immediately_by_default();
    test_publish_batches_when_opted_in();
    test_publish_rolls_back_on_out_of_buffer();
    test_publish_unicasts_to_known_peers_at_or_under_threshold();
    test_publish_broadcasts_when_peer_count_exceeds_threshold();
    test_publish_falls_back_to_broadcast_when_buffer_not_empty();
    test_node_flush_broadcasts_with_no_known_peers();
    test_node_flush_unicasts_to_known_publisher_peers_at_or_under_threshold();
    test_node_flush_broadcasts_when_peer_count_exceeds_threshold();
    test_node_flush_broadcasts_when_update_is_pending();
    test_node_flush_broadcasts_when_multiple_publishers_on_node();
    test_node_update_sets_pending_update_flag();
    test_node_destroy_broadcasts_farewell();
    test_publisher_destroy_broadcasts_goodbye_immediately();
    test_process_data_dispatches_to_subscriber();
    test_process_data_unknown_endpoint_is_ignored();
    test_process_data_decode_failure_is_reported();
    test_process_data_fans_out_to_every_matching_subscriber();
    test_delivery_order_diagnostic_counts_disorder();
    test_delivery_counters_are_reported_on_both_teardown_orders();
    test_best_effort_discards_out_of_order();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_publish_subscribe: all tests passed\n");
    return 0;
}
