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

// Whitebox: QoS roadmap #5 (RELIABILITY) follow-up - periodic Heartbeat (struct
// tt_HeartbeatHeader, tickle.h) - exercises send_heartbeat()'s own [first_available_seq_no,
// last_seq_no] derivation from reliable_cache, process_heartbeat()'s own first-contact-baseline
// and oversized-gap handling, and tt_Publisher_set_heartbeat_period()'s own arm/disarm - all
// static, same "reach tickle.c's own statics" approach as test_reliable_pubsub.c.
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

static void stub_subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                     struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
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

static void init_publisher(struct tt_Publisher* pub, struct tt_Node* node, struct tt_Topic* topic) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.name = "test_publisher";
    pub->node = node;
    pub->topic = topic;
}

// Also registers pub as node->endpoints[0] and zeroes its own peers[] to the empty-slot sentinel -
// decode_update_entities()'s own find_endpoint()/upsert_peer() calls need both, unlike init_
// publisher() above (whose own tests reach tt_Publisher_publish() directly, never through
// incoming-packet dispatch) - same helper shape as test_durability_pubsub.c's own identically-
// named one.
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
    sub->reliable = true;
    sub->ack_seq_no = 1; // matches tt_Node_create_subscriber()'s own init - see tickle.h
    sub->reliable_sender_node_id = tt_NODE_ID_INVALID;

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)sub;
}

static void init_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
}

// Builds a HeartbeatHeader at the start of node->rx_buffer, returning the tail offset (matching
// what process_packet() would have handed process_heartbeat()).
static uint32_t write_heartbeat(struct tt_Node* node, uint32_t endpoint_id, uint32_t first_available_seq_no,
                                uint32_t last_seq_no) {
    struct tt_HeartbeatHeader* heartbeat_header = (struct tt_HeartbeatHeader*)node->rx_buffer;
    heartbeat_header->endpoint_id = endpoint_id;
    heartbeat_header->first_available_seq_no = first_available_seq_no;
    heartbeat_header->last_seq_no = last_seq_no;
    return sizeof(struct tt_HeartbeatHeader);
}

// Builds an UpdateHeader with a single following TOPIC_SUBSCRIBER UpdateEntity in node->rx_buffer,
// returning the tail offset (matching what process_packet() would have handed process_update()) -
// same helper as tests/test_durability_pubsub.c's own identically-named one, needed here too for
// this file's own discovery-triggered-Heartbeat tests below.
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

// tt_Publisher_set_heartbeat_period() must refuse to arm a Heartbeat for a Publisher with no
// reliable_cache set - nothing for it to ever announce.
static void test_heartbeat_set_period_requires_reliable_cache(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Publisher_set_heartbeat_period(&pub, 1000000));
    EXPECT_EQ_INT(0, node.scheduler_tail);
}

// A successful tt_Publisher_set_heartbeat_period() call arms exactly one scheduler entry;
// disabling (period_ns == 0) removes it again.
static void test_heartbeat_set_period_arms_and_disarms(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.reliable_cache = &cache;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_heartbeat_period(&pub, 1000000));
    EXPECT_EQ_INT(1, node.scheduler_tail);
    EXPECT_EQ_U32(1000000, (uint32_t)pub.heartbeat_period_ns);

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_heartbeat_period(&pub, 0));
    EXPECT_EQ_INT(0, node.scheduler_tail);
    EXPECT_EQ_U32(0, (uint32_t)pub.heartbeat_period_ns);
}

// send_heartbeat() must announce [oldest retained, newest published] straight from reliable_
// cache's own current state - including after eviction has moved the oldest retained entry
// forward, not just the very first samples ever published.
static void test_heartbeat_send_derives_range_correctly(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.reliable_cache = &cache;

    for (uint32_t i = 0; i < 6; i++) { // seq_no 1..6, depth 4 -> only 3..6 remain retained
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_heartbeat_period(&pub, 1000000));
    test_mock_send_call_count = 0; // only count the actual Heartbeat send below

    send_heartbeat(&node, tt_get_ns(), &pub);

    // No known peers (pub.peers[] never populated in this test) - broadcasts via tt_send()
    // rather than unicasting via tt_send_to(), same peer/broadcast decision tt_Publisher_
    // publish() itself makes - see test_mock_send_call_count's own doc comment (test_mock.h) for
    // why this counts both paths and test_heartbeat_first_contact_sets_baseline_with_no_data_
    // ever_received() below (a real known sender, so a real unicast) checks test_mock_send_to_
    // call_count instead.
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    struct tt_HeartbeatHeader* sent =
        (struct tt_HeartbeatHeader*)(node.tx_buffer + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    EXPECT_EQ_U32(3, sent->first_available_seq_no);
    EXPECT_EQ_U32(6, sent->last_seq_no);
}

// A Publisher that hasn't published anything yet (reliable_cache still entirely empty) must skip
// sending a Heartbeat this round - nothing retained, nothing to announce - but must still
// reschedule itself for the next period.
static void test_heartbeat_send_skips_when_nothing_retained_yet(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.heartbeat_period_ns = 1000000;

    send_heartbeat(&node, tt_get_ns(), &pub);

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_INT(1, node.scheduler_tail); // still rescheduled itself for next time
}

// The actual DDS-parity case this whole feature is for: a brand-new Subscriber that has never
// received any DATA at all still learns the correct baseline purely from a Heartbeat, and the
// gap it reveals (nothing received yet, but the Publisher says it has up through last_seq_no)
// immediately triggers a real ACKNACK - not just silent bookkeeping.
static void test_heartbeat_first_contact_sets_baseline_with_no_data_ever_received(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 97, 100);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(97, sub.ack_seq_no); // learned directly from the Heartbeat, no DATA involved
    EXPECT_TRUE(sub.received_bitmap == 0);
    EXPECT_EQ_U32(100, sub.reliable_heartbeat_last_seq_no);
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)sub.reliable_sender_node_id);
    EXPECT_TRUE(sub.reliable_acknack_scheduled); // 97..100 gap revealed -> a real ACKNACK cycle
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
}

// A Heartbeat arriving at an already-tracking Subscriber, revealing a gap too wide to ever
// represent in a 64-bit ACKNACK bitmap, must jump the baseline the same way an oversized DATA gap
// does (PLAN.md's Milestone 20) - not get permanently stuck the way a bare "not first contact,
// gap within window" case correctly leaves alone.
static void test_heartbeat_oversized_gap_jumps_baseline(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reliable_sender_node_id = REMOTE_NODE_ID; // simulate "already had reliable contact"
    sub.ack_seq_no = 1;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 1, 1000); // gap of 999 from ack_seq_no 1
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1001, sub.ack_seq_no);
    EXPECT_TRUE(sub.received_bitmap == 0);
}

// A Heartbeat arriving at an already-tracking Subscriber, revealing a gap that still fits inside
// the 64-bit tracking window, must widen what gets ACKNACKed without touching ack_seq_no itself -
// highest_relevant_bit() picks this up from reliable_heartbeat_last_seq_no, not a direct jump.
static void test_heartbeat_gap_within_window_widens_request_without_jumping(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reliable_sender_node_id = REMOTE_NODE_ID;
    sub.reliable_sender_ip = TEST_SENDER_IP;
    sub.reliable_sender_port = TEST_SENDER_PORT;
    sub.ack_seq_no = 1;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 1, 5); // gap of 4 - well within the window
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1, sub.ack_seq_no);      // untouched - this is not the oversized-gap case
    EXPECT_TRUE(sub.received_bitmap == 0); // nothing *confirmed* received either
    EXPECT_EQ_U32(5, sub.reliable_heartbeat_last_seq_no);
    EXPECT_TRUE(sub.reliable_acknack_scheduled); // still triggers a real ACKNACK cycle
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
}

// A best-effort Subscriber (reliable == false) has no ack state for a Heartbeat to update -
// must be a harmless no-op, not a crash.
static void test_heartbeat_ignored_for_besteffort_subscriber(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reliable = false;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 1, 100);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1, sub.ack_seq_no);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// tt_Publisher_destroy() must cancel a still-armed Heartbeat schedule entry, not leave it
// dangling against a Publisher that no longer exists.
static void test_publisher_destroy_cancels_armed_heartbeat(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.reliable_cache = &cache;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_heartbeat_period(&pub, 1000000));
    EXPECT_EQ_INT(1, node.scheduler_tail);

    tt_Publisher_destroy(&pub);
    EXPECT_EQ_INT(0, node.scheduler_tail);
}

// PLAN.md's Milestone 23: a brand-new Subscriber discovered via UPDATE for a RELIABLE Publisher
// must receive an immediate, one-off unicast Heartbeat - not wait for the periodic schedule -
// mirroring test_durability_pubsub.c's own test_durability_delivers_backlog_to_newly_discovered_
// subscriber() structure exactly, just asserting on the Heartbeat submessage instead of DATA.
static void test_heartbeat_discovery_sends_immediate_heartbeat_to_new_peer(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.reliable = true;

    for (uint32_t i = 0; i < 3; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i)); // seq_no 1..3
    }

    node.update_seen[REMOTE_NODE_ID] = true; // see test_durability_pubsub.c's own comment on why
    test_mock_send_to_call_count = 0;        // only count the initial Heartbeat below

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip);
    EXPECT_EQ_U32((uint32_t)TEST_SENDER_PORT, (uint32_t)test_mock_send_to_last_port);

    struct tt_HeartbeatHeader* sent =
        (struct tt_HeartbeatHeader*)(node.tx_buffer + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    EXPECT_EQ_U32(1, sent->first_available_seq_no);
    EXPECT_EQ_U32(3, sent->last_seq_no);
}

// A best-effort Publisher (reliable_cache == NULL) discovering a new peer must not send a
// Heartbeat at all - nothing for it to ever announce, same guard send_heartbeat()'s own periodic
// path relies on.
static void test_heartbeat_discovery_skipped_for_besteffort_publisher(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    node.update_seen[REMOTE_NODE_ID] = true;
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// A already-known peer's periodic UPDATE refresh (same last_modified, deduped by process_update()
// before decode_update_entities()/upsert_peer() ever run again) must not re-trigger a second
// initial Heartbeat - mirrors test_durability_pubsub.c's own test_durability_no_redelivery_on_
// unchanged_update().
static void test_heartbeat_discovery_no_redelivery_on_unchanged_update(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.reliable = true;

    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1

    node.update_seen[REMOTE_NODE_ID] = true;
    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // the initial Heartbeat, first time

    test_mock_send_to_call_count = 0; // only count the second (unchanged) UPDATE's own effect below

    tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID); // same last_modified -> deduped
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// A Publisher with *both* reliable and durable set (the same shared cache now backs both - see
// struct tt_ReliableCache's own doc comment) discovering a new peer must send both the durability
// backlog *and* the initial Heartbeat - order doesn't matter, both must happen, total send_to
// count is cache_entry_count + 1.
static void test_heartbeat_discovery_sends_both_durability_backlog_and_heartbeat(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_ReliableCache reliable_cache;
    memset(&reliable_cache, 0, sizeof(reliable_cache));
    reliable_cache.depth = 4;
    pub.reliable_cache = &reliable_cache;
    pub.reliable = true;
    pub.durable = true;

    for (uint32_t i = 0; i < 3; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i)); // seq_no 1..3
    }

    node.update_seen[REMOTE_NODE_ID] = true;
    test_mock_send_to_call_count = 0; // only count this discovery's own effect below

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // 3 durability backlog samples + 1 initial Heartbeat.
    EXPECT_EQ_U32(4, (uint32_t)test_mock_send_to_call_count);
}

int main(void) {
    test_heartbeat_set_period_requires_reliable_cache();
    test_heartbeat_set_period_arms_and_disarms();
    test_heartbeat_send_derives_range_correctly();
    test_heartbeat_send_skips_when_nothing_retained_yet();
    test_heartbeat_first_contact_sets_baseline_with_no_data_ever_received();
    test_heartbeat_oversized_gap_jumps_baseline();
    test_heartbeat_gap_within_window_widens_request_without_jumping();
    test_heartbeat_ignored_for_besteffort_subscriber();
    test_publisher_destroy_cancels_armed_heartbeat();
    test_heartbeat_discovery_sends_immediate_heartbeat_to_new_peer();
    test_heartbeat_discovery_skipped_for_besteffort_publisher();
    test_heartbeat_discovery_no_redelivery_on_unchanged_update();
    test_heartbeat_discovery_sends_both_durability_backlog_and_heartbeat();

    printf("test_heartbeat: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
