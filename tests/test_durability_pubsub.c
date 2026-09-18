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

// Whitebox: QoS roadmap #4 (DURABILITY/TRANSIENT_LOCAL, rmw_tickle/PLAN.md) - exercises
// tt_Publisher_publish()'s durable_cache store/evict and deliver_durability_backlog()'s trigger
// from decode_update_entities() (via upsert_peer()'s own new/refresh distinction) - all static,
// same "reach tickle.c's own statics" approach as test_reliable_pubsub.c/test_peer_discovery.c.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
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

// Also registers pub as node->endpoints[0] - decode_update_entities()'s own find_endpoint() call
// needs this to match an incoming UpdateEntity back to it, unlike test_reliable_pubsub.c's own
// init_publisher() (which most of its own tests reach through tt_Publisher_publish() directly,
// never through incoming-packet dispatch).
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

static void init_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
}

// Builds an UpdateHeader with a single following TOPIC_SUBSCRIBER UpdateEntity in node->rx_buffer,
// returning the tail offset (matching what process_packet() would have handed process_update()) -
// same shape as tests/test_peer_discovery.c's own write_update_one_entity(), narrowed to the one
// entity kind these tests need.
static uint32_t write_update_one_subscriber(struct tt_Node* node, uint64_t last_modified, uint32_t endpoint_id) {
    struct tt_UpdateHeader* update_header = (struct tt_UpdateHeader*)node->rx_buffer;
    update_header->last_modified = last_modified;
    update_header->entity_count = 1;
    uint32_t tail = sizeof(struct tt_UpdateHeader);

    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(node->rx_buffer + tail);
    entity->endpoint_id = endpoint_id;
    entity->kind = tt_KIND_TOPIC_SUBSCRIBER;
    tail += sizeof(struct tt_UpdateEntity);

    tt_encode_string(node->rx_buffer, &tail, tt_MAX_BUFFER_LENGTH * 2, "test_topic");
    tt_encode_string(node->rx_buffer, &tail, tt_MAX_BUFFER_LENGTH * 2, "test_subscriber");

    return tail;
}

// tt_Publisher_publish() on a Publisher with durable_cache set must snapshot every sample into the
// ring, and once more than `depth` samples have gone out, only the most recent `depth` (KEEP_LAST)
// must remain - same eviction shape as test_reliable_pubsub.c's own
// test_reliable_publish_caches_and_evicts, mirrored here for the independent durable_cache.
static void test_durability_publish_caches_and_evicts(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_DurableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.durable_cache = &cache;

    for (uint32_t i = 0; i < 6; i++) {
        uint32_t value = i;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }

    // 6 samples published (seq_no 1..6), depth 4: only seq_no 3..6 should still be cached - the
    // ring wrapped exactly once (cache.next == 6 % 4 == 2, so entries[2]/[3] hold the oldest
    // remaining pair, seq_no 5/6, and entries[0]/[1] hold seq_no 3/4 overwritten last).
    int seen_seq_nos[4];
    for (int i = 0; i < 4; i++) {
        EXPECT_TRUE(cache.entries[i].len != 0);
        seen_seq_nos[i] = (int)cache.entries[i].seq_no;
    }
    int min_seq = seen_seq_nos[0];
    int max_seq = seen_seq_nos[0];
    for (int i = 1; i < 4; i++) {
        if (seen_seq_nos[i] < min_seq) {
            min_seq = seen_seq_nos[i];
        }
        if (seen_seq_nos[i] > max_seq) {
            max_seq = seen_seq_nos[i];
        }
    }
    EXPECT_EQ_INT(3, min_seq);
    EXPECT_EQ_INT(6, max_seq);
}

// A brand-new Subscriber (never seen before) announcing itself for a DURABLE Publisher's topic
// must receive every currently-retained sample, unicast straight to it - the core QoS roadmap #4
// behavior.
static void test_durability_delivers_backlog_to_newly_discovered_subscriber(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_DurableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.durable_cache = &cache;

    for (uint32_t i = 0; i < 3; i++) {
        uint32_t value = i;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1..3
    }

    // Marks REMOTE_NODE_ID as already-known to process_update()'s own discovery bookkeeping, so
    // its first-contact reply_with_own_announce() (a real, unrelated send_to of its own) doesn't
    // confound this test's own send_to count - this test cares about durability's own delivery
    // count specifically, not discovery's own reply mechanics.
    node.update_seen[REMOTE_NODE_ID] = true;
    test_mock_send_to_call_count = 0; // only count the backlog delivery below

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // 3 retained samples, all unicast to the new subscriber's own address.
    EXPECT_EQ_U32(3, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip);
    EXPECT_EQ_U32((uint32_t)TEST_SENDER_PORT, (uint32_t)test_mock_send_to_last_port);

    // The last packet still sitting in tx_buffer (end_encode() resets tx_tail after each unicast
    // flush - see its own comment) is the newest retained sample, seq_no 3.
    struct tt_DataHeader* last_data_header =
        (struct tt_DataHeader*)(node.tx_buffer + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    EXPECT_EQ_U32(3, last_data_header->seq_no);
}

// upsert_peer() itself must return true only the first time a given node_id claims a slot, not on
// a later refresh of that same node_id's address - the exact distinction decode_update_entities()
// relies on to fire deliver_durability_backlog() only for a genuinely new peer. Tested directly,
// not through the full process_update() pipeline: process_update()'s own forget_peers_from_source()
// wipes and re-adds every peer from a source on *any* changed announce (see this file's own
// upsert_peer() doc comment), so a same-node_id "refresh" at that level always looks like a fresh
// slot claim too - a real, already-documented trade-off, not something this unit-level test needs
// to also exercise.
static void test_upsert_peer_true_only_for_new_slot(void) {
    struct tt_Peer peers[tt_MAX_PEER_COUNT];
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        peers[i].node_id = tt_NODE_ID_INVALID;
    }

    EXPECT_TRUE(upsert_peer(peers, REMOTE_NODE_ID, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_TRUE(!upsert_peer(peers, REMOTE_NODE_ID, TEST_SENDER_IP, TEST_SENDER_PORT + 1)); // address refresh
    EXPECT_EQ_U32((uint32_t)(TEST_SENDER_PORT + 1), (uint32_t)peers[0].port);               // refresh still took effect
}

// A newly-discovered Subscriber for a DURABLE Publisher's topic, seen via an UPDATE whose
// last_modified exactly matches the last one already processed from that same source, must not
// re-trigger a fresh backlog delivery - process_update()'s own "nothing changed" dedup short-
// circuits before decode_update_entities() (and so upsert_peer()) ever runs again.
static void test_durability_no_redelivery_on_unchanged_update(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_DurableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.durable_cache = &cache;

    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1

    node.update_seen[REMOTE_NODE_ID] = true; // see the previous test's own comment on why
    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // the one retained sample, first time

    test_mock_send_to_call_count = 0; // only count the second (unchanged) UPDATE's own effect below

    tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID); // same last_modified -> deduped, unprocessed
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// A VOLATILE Publisher (durable_cache == NULL, today's default) seeing a brand-new Subscriber
// must be a harmless no-op - not a crash, not a delivery of anything.
static void test_durability_ignored_for_volatile_publisher(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    uint32_t value = 9;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    node.update_seen[REMOTE_NODE_ID] = true; // see the earlier test's own comment on why
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

int main(void) {
    test_durability_publish_caches_and_evicts();
    test_durability_delivers_backlog_to_newly_discovered_subscriber();
    test_upsert_peer_true_only_for_new_slot();
    test_durability_no_redelivery_on_unchanged_update();
    test_durability_ignored_for_volatile_publisher();

    printf("test_durability_pubsub: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
