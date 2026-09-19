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

// Whitebox: QoS roadmap #1 (RxO matching, Milestone 31, rmw_tickle/PLAN.md) - exercises both
// halves decode_update_entities()'s own tt_KIND_TOPIC_SUBSCRIBER branch (Publisher-side: an
// incompatible remote Subscriber never becomes a peer) and process_data()'s own subscriber_
// incompatible_with_publisher() (Subscriber-side: an incompatible remote Publisher's DATA is
// silently dropped, the more consequential half since it's what actually stops broadcast delivery
// too) - all static, same "reach tickle.c's own statics" approach as test_reliable_pubsub.c/
// test_durability_pubsub.c.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define ENDPOINT_ID 0xaabbccdd
#define TEST_SENDER_IP 0x0a000001
#define TEST_SENDER_PORT 12345

static int subscriber_callback_count = 0;

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

static void init_publisher_registered_on_node(struct tt_Publisher* pub, struct tt_Node* node, struct tt_Topic* topic) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.name = "test_publisher";
    pub->node = node;
    pub->topic = topic;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].node_id = tt_NODE_ID_INVALID;
    }

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)pub;
}

static void init_subscriber_registered_on_node(struct tt_Subscriber* sub, struct tt_Node* node,
                                               struct tt_Topic* topic) {
    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = ENDPOINT_ID;
    sub->node = node;
    sub->topic = topic;
    sub->callback = stub_subscriber_callback;
    sub->ack_seq_no = 1; // matches tt_Node_create_subscriber()'s own init - see tickle.h

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)sub;
}

static void init_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
}

// Builds a DataHeader + 4-byte payload at the start of node->rx_buffer, returning the tail
// offset (matching what process_packet() would have handed process_data()) - same helper as
// tests/test_reliable_pubsub.c's own write_data().
static uint32_t write_data(struct tt_Node* node, uint32_t seq_no, uint64_t timestamp, uint32_t value) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node->rx_buffer;
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = timestamp;

    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node->rx_buffer + tail, &value, sizeof(value));
    return tail + sizeof(value);
}

// Builds an UpdateHeader with a single following TOPIC_SUBSCRIBER UpdateEntity carrying `qos` in
// node->rx_buffer, returning the tail offset - same shape as tests/test_durability_pubsub.c's own
// write_update_one_subscriber(), extended with the QoS byte this milestone added to the wire.
static uint32_t write_update_one_subscriber_with_qos(struct tt_Node* node, uint64_t last_modified, uint32_t endpoint_id,
                                                     uint8_t qos) {
    struct tt_UpdateHeader* update_header = (struct tt_UpdateHeader*)node->rx_buffer;
    update_header->last_modified = last_modified;
    update_header->entity_count = 1;
    uint32_t tail = sizeof(struct tt_UpdateHeader);

    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(node->rx_buffer + tail);
    entity->endpoint_id = endpoint_id;
    entity->kind = tt_KIND_TOPIC_SUBSCRIBER;
    entity->qos = qos;
    tail += sizeof(struct tt_UpdateEntity);

    tt_encode_string(node->rx_buffer, &tail, tt_MAX_BUFFER_LENGTH * 2, "test_topic");
    tt_encode_string(node->rx_buffer, &tail, tt_MAX_BUFFER_LENGTH * 2, "test_subscriber");

    return tail;
}

// --- Subscriber-side gate: process_data()'s own subscriber_incompatible_with_publisher() ---

// A Subscriber requesting RELIABLE must not receive DATA from a discovered Publisher that didn't
// offer it - the consequential half of RxO matching (stops delivery outright, not just the
// unicast-only enhancements the Publisher-side gate withholds).
static void test_reliable_subscriber_drops_data_from_besteffort_publisher(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reliable = true;

    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Node_set_discovery(&node, &discovery, NULL, NULL));
    // Discovered Publisher offers neither RELIABLE nor DURABLE (qos = 0) - incompatible with this
    // Subscriber's own sub.reliable request.
    upsert_discovered_entity(&node, REMOTE_NODE_ID, ENDPOINT_ID, tt_KIND_TOPIC_PUBLISHER, 0, "test_topic",
                             "test_publisher");

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_data(&node, 1, 1000, 42);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_INT(0, subscriber_callback_count);
}

// The mirror image: a discovered Publisher that DOES offer RELIABLE is compatible, and DATA is
// delivered normally.
static void test_reliable_subscriber_receives_data_from_reliable_publisher(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reliable = true;

    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Node_set_discovery(&node, &discovery, NULL, NULL));
    upsert_discovered_entity(&node, REMOTE_NODE_ID, ENDPOINT_ID, tt_KIND_TOPIC_PUBLISHER, tt_UPDATE_QOS_RELIABLE,
                             "test_topic", "test_publisher");

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_data(&node, 1, 1000, 42);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_INT(1, subscriber_callback_count);
}

// Same shape, DURABILITY instead of RELIABILITY: a Subscriber requiring TRANSIENT_LOCAL (sub.
// durable) must not receive DATA from a discovered Publisher that only offers VOLATILE.
static void test_durable_subscriber_drops_data_from_volatile_publisher(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.durable = true;

    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Node_set_discovery(&node, &discovery, NULL, NULL));
    upsert_discovered_entity(&node, REMOTE_NODE_ID, ENDPOINT_ID, tt_KIND_TOPIC_PUBLISHER, 0, "test_topic",
                             "test_publisher");

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_data(&node, 1, 1000, 42);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_INT(0, subscriber_callback_count);
}

// Fails open when no discovery cache is attached at all - a raw TickLE-core caller that never
// called tt_Node_set_discovery() must see no behavior change from this milestone, even for a
// Subscriber that requested RELIABLE.
static void test_no_discovery_attached_delivers_regardless(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reliable = true;
    // node.discovery left NULL - tt_Node_set_discovery() never called.

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_data(&node, 1, 1000, 42);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_INT(1, subscriber_callback_count);
}

// Fails open for the startup-race case: discovery is attached, but this specific Publisher hasn't
// been announced yet (DATA arrived first) - not distinguishable from "never checked", so this
// Subscriber gets the benefit of the doubt rather than dropping a possibly-compatible pair's very
// first sample.
static void test_publisher_not_yet_discovered_delivers(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reliable = true;

    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Node_set_discovery(&node, &discovery, NULL, NULL));
    // Nothing recorded for REMOTE_NODE_ID/ENDPOINT_ID - discovery is empty.

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_data(&node, 1, 1000, 42);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_INT(1, subscriber_callback_count);
}

// --- Publisher-side gate: decode_update_entities()'s own tt_KIND_TOPIC_SUBSCRIBER branch ---

// A remote Subscriber requesting RELIABLE from a BEST_EFFORT Publisher must never become a peer -
// no unicast optimization, no durability backlog, no discovery-triggered Heartbeat.
static void test_publisher_side_gate_skips_incompatible_subscriber(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);
    // pub.reliable/.durable both left false (BEST_EFFORT/VOLATILE) - reliable_cache is NULL too,
    // matching a real best-effort Publisher exactly (no cache to have anything backlog-worthy in).

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber_with_qos(&node, 100, ENDPOINT_ID, tt_UPDATE_QOS_RELIABLE);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));
}

// The mirror image: a remote Subscriber requesting nothing this Publisher doesn't already offer
// becomes a peer normally - RxO matching must not reject a genuinely compatible pair.
static void test_publisher_side_gate_accepts_compatible_subscriber(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber_with_qos(&node, 100, ENDPOINT_ID, 0); // requests nothing extra
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
}

int main(void) {
    test_reliable_subscriber_drops_data_from_besteffort_publisher();
    test_reliable_subscriber_receives_data_from_reliable_publisher();
    test_durable_subscriber_drops_data_from_volatile_publisher();
    test_no_discovery_attached_delivers_regardless();
    test_publisher_not_yet_discovered_delivers();
    test_publisher_side_gate_skips_incompatible_subscriber();
    test_publisher_side_gate_accepts_compatible_subscriber();

    printf("test_rxo_matching: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
