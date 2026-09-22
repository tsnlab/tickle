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

static int subscriber_callback_count = 0;

static void stub_subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                     struct tt_Data* data) {
    subscriber_callback_count++;
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
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].node_id = tt_NODE_ID_INVALID; // all empty - matches tt_Node_create_
                                                      // subscriber()'s own init (Milestone 47 -
                                                      // each writer's own ack_seq_no starts at 1
                                                      // lazily, on first contact)
    }

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)sub;
}

// Milestone 47 - shorthand for the WriterProxy every test in this file cares about: this file's
// own single simulated remote Publisher, REMOTE_NODE_ID with entity_id 0 (write_heartbeat()/write_
// data() below never set entity_id, so it stays 0 - node->rx_buffer starts zeroed). Looks up the
// entry process_heartbeat()/process_data() already created on first contact.
static struct tt_WriterProxy* remote_writer_proxy(struct tt_Subscriber* sub) {
    return find_writer_proxy(sub, REMOTE_NODE_ID, 0);
}

static void init_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
}

// Builds a HeartbeatHeader at the start of node->rx_buffer, returning the tail offset (matching
// what process_packet() would have handed process_heartbeat()). flags is tt_HEARTBEAT_FLAG_FINAL
// or 0 - every existing call site below passes tt_HEARTBEAT_FLAG_FINAL, matching what send_
// heartbeat()/send_initial_heartbeat() actually send today; the new tt_HEARTBEAT_FLAG_FINAL-
// specific tests below are the only ones that pass 0.
static uint32_t write_heartbeat(struct tt_Node* node, uint32_t endpoint_id, uint32_t first_available_seq_no,
                                uint32_t last_seq_no, uint8_t flags) {
    struct tt_HeartbeatHeader* heartbeat_header = (struct tt_HeartbeatHeader*)node->rx_buffer;
    heartbeat_header->endpoint_id = endpoint_id;
    heartbeat_header->first_available_seq_no = first_available_seq_no;
    heartbeat_header->last_seq_no = last_seq_no;
    heartbeat_header->entity_id = 0; // explicit, like write_data()'s own: offset 12 overlaps a prior
                                     // write_data()'s timestamp, which would pick a different writer
    heartbeat_header->flags = flags;
    heartbeat_header->reserved[0] = 0;
    heartbeat_header->reserved[1] = 0;
    heartbeat_header->reserved[2] = 0;
    return sizeof(struct tt_HeartbeatHeader);
}

// Builds a DataHeader + 4-byte payload at the start of node->rx_buffer, returning the tail offset
// (matching what process_packet() would have handed process_data()) - same helper as tests/
// test_reliable_pubsub.c's own write_data(), needed here too for this file's own Milestone 60
// volatile-first-contact test (confirming the stream still tracks normally after the Heartbeat-
// only baseline sync).
static uint32_t write_data(struct tt_Node* node, uint32_t seq_no, uint64_t timestamp, uint32_t value) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node->rx_buffer;
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = timestamp;
    // Explicit, not left to whatever's already in rx_buffer - this file's own tests reuse the same
    // buffer across write_heartbeat()/write_data() calls, and tt_DataHeader.entity_id's own byte
    // offset (16) overlaps tt_HeartbeatHeader.flags/reserved[] from an earlier write_heartbeat()
    // call, so leaving it unset here would silently pick up stale, nonzero bytes - a real bug found
    // writing this exact helper, matching REMOTE_NODE_ID/entity_id 0's own doc comment below.
    data_header->entity_id = 0;

    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node->rx_buffer + tail, &value, sizeof(value));
    return tail + sizeof(value);
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

    TEST_RELIABLE_CACHE(cache, 4);
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

    TEST_RELIABLE_CACHE(cache, 4);
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

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.heartbeat_period_ns = 1000000;

    send_heartbeat(&node, tt_get_ns(), &pub);

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_INT(1, node.scheduler_tail); // still rescheduled itself for next time
}

// The actual DDS-parity case this whole feature is for: a brand-new *durable* Subscriber that has
// never received any DATA at all still learns the correct baseline purely from a Heartbeat, and
// the gap it reveals (nothing received yet, but the Publisher says it has up through last_seq_no)
// immediately triggers a real ACKNACK - not just silent bookkeeping. sub.durable = true here is
// deliberate (Milestone 60, rmw_tickle/PLAN.md) - a durable Subscriber wants the Publisher's whole
// still-retained history, matching DDS's own RxO (Requested vs Offered) design: DURABILITY, like
// every other RxO QoS, is governed by what the Subscriber itself requested, not merely what the
// matched Publisher can offer - see test_heartbeat_first_contact_volatile_subscriber_skips_backlog()
// immediately below for the opposite (and today's actual default) case.
static void test_heartbeat_first_contact_sets_baseline_with_no_data_ever_received(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.durable = true;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 97, 100, tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);           // Milestone 47 - created lazily, first contact
    EXPECT_EQ_U32(97, proxy->ack_seq_no); // learned directly from the Heartbeat, no DATA involved
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
    EXPECT_EQ_U32(100, proxy->heartbeat_last_seq_no);
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)proxy->node_id);
    EXPECT_TRUE(proxy->acknack_scheduled); // 97..100 gap revealed -> a real ACKNACK cycle
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
}

// Milestone 60 (rmw_tickle/PLAN.md) - the opposite, today's-default case: a brand-new *volatile*
// Subscriber (sub.durable left false, tt_Node_create_subscriber()'s own default) must NOT request
// any of a matched Publisher's pre-match history, even though the Heartbeat reveals the Publisher
// still has samples 97..100 retained and could offer them - a legal, common DDS pattern (a
// TRANSIENT_LOCAL-capable Publisher matched by a Subscriber that explicitly doesn't want history).
// Real HIL finding this closes: durability_late_join's own volatile-Subscriber scenario
// deterministically leaked an entire pre-match backlog through this exact first-contact branch.
static void test_heartbeat_first_contact_volatile_subscriber_skips_backlog(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 97, 100, tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    // Synced to just past last_seq_no (100), not first_available_seq_no (97) - "whatever's already
    // been published up to this instant" is this Subscriber's own baseline, not the Publisher's
    // oldest retained sample. +1, not last_seq_no itself: ack_seq_no means "next not yet
    // accounted for" - leaving it at 100 would still treat that one sample as outstanding.
    EXPECT_EQ_U32(101, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
    EXPECT_EQ_U32(100, proxy->heartbeat_last_seq_no);
    EXPECT_TRUE(!proxy->acknack_scheduled);                   // no gap from this Subscriber's own point of view
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // no ACKNACK requesting any of 97..100

    // The stream must still track normally from here - a real, later gap must still be detected.
    tail = write_data(&node, 103, 10300, 103); // 101, 102 skipped
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(101, proxy->ack_seq_no); // correctly still waiting on 101
    EXPECT_TRUE(proxy->acknack_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // a real ACKNACK for 101
}

// Decodes the most recently sent packet as a single Heartbeat - NULL if it wasn't one. Same helper
// as tests/test_reliable_pubsub.c's own.
static const struct tt_HeartbeatHeader* last_sent_heartbeat(void) {
    size_t submessage_off = sizeof(struct tt_Header);
    size_t heartbeat_off = submessage_off + sizeof(struct tt_SubmessageHeader);
    if (test_mock_send_last_len < heartbeat_off + sizeof(struct tt_HeartbeatHeader)) {
        return NULL;
    }
    const struct tt_SubmessageHeader* submessage =
        (const struct tt_SubmessageHeader*)(test_mock_send_last_buf + submessage_off);
    if (submessage->type != tt_SUBMESSAGE_TYPE_HEARTBEAT) {
        return NULL;
    }
    return (const struct tt_HeartbeatHeader*)(test_mock_send_last_buf + heartbeat_off);
}

// Same helper as tests/test_reliable_pubsub.c's own: word 0 == low, every other word == 0.
static bool bitmap_equals_u64(const uint64_t bitmap[tt_RELIABLE_BITMAP_WORDS], uint64_t low) {
    if (bitmap[0] != low) {
        return false;
    }
    for (int word = 1; word < tt_RELIABLE_BITMAP_WORDS; word++) {
        if (bitmap[word] != 0) {
            return false;
        }
    }
    return true;
}

// Feed one DATA (seq_no, value == seq_no) or one FINAL Heartbeat through the real process_* entry points.
static void feed_data(struct tt_Node* node, struct tt_Header* header, uint32_t seq_no) {
    uint32_t tail = write_data(node, seq_no, seq_no, seq_no);
    EXPECT_TRUE(process_data(node, header, node->rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
}

static void feed_heartbeat(struct tt_Node* node, struct tt_Header* header, uint32_t first_available_seq_no,
                           uint32_t last_seq_no) {
    uint32_t tail = write_heartbeat(node, ENDPOINT_ID, first_available_seq_no, last_seq_no, tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_TRUE(process_heartbeat(node, header, node->rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
}

// Phase 1-c (rmw_tickle/PLAN.md, B2) - an already-tracking Subscriber told by a Heartbeat (the
// Publisher's eviction Heartbeat, or a periodic one) that history now starts at
// first_available_seq_no advances ack_seq_no straight past everything below it: bitmap shifted to
// the new base, any following received run absorbed, retry budget reset - and, once nothing is
// left outstanding, the retry timer unscheduled.
static void test_heartbeat_first_available_advances_existing_proxy(void) {
    test_mock_reset();
#ifdef tt_RELIABLE_STATS
    tt_reliable_stats_reset();
#endif

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    feed_data(&node, &header, 1); // first contact via DATA -> ack_seq_no 2
    feed_data(&node, &header, 4);
    feed_data(&node, &header, 5);
    feed_data(&node, &header, 7); // missing: 2, 3, 6
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(2, proxy->ack_seq_no);
    EXPECT_TRUE(proxy->acknack_scheduled);
    proxy->retry = 2; // part-way through seq_no 2's own retry budget

    feed_heartbeat(&node, &header, 4, 7);                           // 2, 3 are gone at the Publisher
    EXPECT_EQ_U32(6, proxy->ack_seq_no);                            // past 2, 3; then 4, 5 absorbed
    EXPECT_TRUE(bitmap_equals_u64(proxy->received_bitmap, 0x2ULL)); // bit 1 -> seq_no 7
    EXPECT_EQ_U32(0, (uint32_t)proxy->retry);                       // seq_no 6 gets a fresh budget
    EXPECT_TRUE(proxy->acknack_scheduled);                          // 6 still outstanding

    feed_heartbeat(&node, &header, 8, 7); // everything up to 7 gone (first_available = newest + 1)
    EXPECT_EQ_U32(8, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
    EXPECT_TRUE(!proxy->acknack_scheduled); // nothing left to ask for

#ifdef tt_RELIABLE_STATS
    struct tt_ReliableStats stats;
    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(2, (uint32_t)stats.heartbeat_advances);
    EXPECT_EQ_U32(3, (uint32_t)stats.heartbeat_abandoned_seq); // 2, 3, then 6 (7 was received)
#endif
}

// A stale/reordered Heartbeat whose first_available_seq_no is at or below ack_seq_no (or 0, an
// empty cache) must never move ack_seq_no backwards or disturb the tracked gap.
static void test_heartbeat_stale_first_available_is_ignored(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    feed_data(&node, &header, 1);
    feed_data(&node, &header, 2);
    feed_data(&node, &header, 4); // missing: 3
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(3, proxy->ack_seq_no);

    feed_heartbeat(&node, &header, 2, 4);
    feed_heartbeat(&node, &header, 3, 4);
    feed_heartbeat(&node, &header, 0, 4);
    EXPECT_EQ_U32(3, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_equals_u64(proxy->received_bitmap, 0x2ULL)); // bit 1 -> seq_no 4
    EXPECT_TRUE(proxy->acknack_scheduled);
}

// DURABILITY: a durable late joiner's backlog must not be skipped by this path. Its first-contact
// baseline is the Publisher's first_available_seq_no; repeated Heartbeats with that same baseline
// change nothing while the backlog streams in, and every backlog sample is delivered. Only a later,
// genuinely newer first_available_seq_no (the Publisher evicted part of the backlog before it
// arrived) skips - and only the part that's actually gone.
static void test_heartbeat_durable_late_joiner_backlog_not_skipped(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.durable = true;

    struct tt_Header header;
    init_header(&header);

    feed_heartbeat(&node, &header, 1, 20); // first contact: backlog 1..20 wanted
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(1, proxy->ack_seq_no);

    for (uint32_t seq_no = 1; seq_no <= 10; seq_no++) {
        feed_data(&node, &header, seq_no);
        feed_heartbeat(&node, &header, 1, 20); // same baseline, repeated - no skip
    }
    for (uint32_t seq_no = 11; seq_no <= 20; seq_no++) {
        feed_data(&node, &header, seq_no);
    }
    EXPECT_EQ_U32(21, proxy->ack_seq_no);
    EXPECT_EQ_INT(20, subscriber_callback_count); // the whole backlog delivered

    // A second durable joiner whose backlog is partly evicted before it arrives: 1, 2 received,
    // then the Publisher's history starts at 10 - only 3..9 are skipped.
    test_mock_reset();
    subscriber_callback_count = 0;
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.durable = true;
    feed_heartbeat(&node, &header, 1, 20);
    feed_data(&node, &header, 1);
    feed_data(&node, &header, 2);
    proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(3, proxy->ack_seq_no);
    feed_heartbeat(&node, &header, 10, 25);
    EXPECT_EQ_U32(10, proxy->ack_seq_no);
    feed_data(&node, &header, 10);
    EXPECT_EQ_U32(11, proxy->ack_seq_no);
    EXPECT_EQ_INT(3, subscriber_callback_count); // 1, 2, 10
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
    // Milestone 47 - simulate "already had reliable contact" by pre-claiming this writer's own
    // WriterProxy entry, rather than a flat sentinel field.
    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    proxy->ack_seq_no = 1;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail =
        write_heartbeat(&node, ENDPOINT_ID, 1, 1000, tt_HEARTBEAT_FLAG_FINAL); // gap of 999 from ack_seq_no 1
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1001, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
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
    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    proxy->sender_ip = TEST_SENDER_IP;
    proxy->sender_port = TEST_SENDER_PORT;
    proxy->ack_seq_no = 1;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail =
        write_heartbeat(&node, ENDPOINT_ID, 1, 5, tt_HEARTBEAT_FLAG_FINAL); // gap of 4 - well within the window
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1, proxy->ack_seq_no); // untouched - this is not the oversized-gap case
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy))); // nothing *confirmed* received either
    EXPECT_EQ_U32(5, proxy->heartbeat_last_seq_no);
    EXPECT_TRUE(proxy->acknack_scheduled); // still triggers a real ACKNACK cycle
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

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 1, 100, tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_TRUE(remote_writer_proxy(&sub) == NULL); // best-effort - no WriterProxy ever created
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// tt_HEARTBEAT_FLAG_FINAL's own doc comment (tickle.h) / real RTPS finalFlag semantics - a
// Heartbeat that explicitly requests a response (flag clear) must elicit an ACKNACK even from an
// already fully-caught-up Subscriber, which otherwise has no reason of its own to ever send one
// (maybe_arm_acknack_retry()'s own "a healthy stream needs no ACKNACK at all"). This is the exact
// mechanism tt_Publisher_request_ack()/tt_Publisher_wait_for_all_acked() rely on.
static void test_heartbeat_final_flag_clear_forces_acknack_without_gap(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    // already tracking, not first contact
    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    proxy->ack_seq_no = 5; // already received everything up through 4

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 1, 4, 0); // fully caught up, flag clear
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(5, proxy->ack_seq_no);                      // untouched - no real gap
    EXPECT_TRUE(!proxy->acknack_scheduled);                   // no gap -> no retry cycle armed
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // but still replied once, on request
}

// The default (flag set, today's only real-world Heartbeat) must keep the existing bandwidth-
// saving behavior exactly - an already-caught-up Subscriber stays silent, same as before tt_
// HEARTBEAT_FLAG_FINAL existed.
static void test_heartbeat_final_flag_set_stays_silent_without_gap(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    proxy->ack_seq_no = 5;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 1, 4, tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// tt_Publisher_request_ack() itself: unicasts a flag-clear Heartbeat straight to every currently-
// matched peer (not broadcast) - one send_to per peer.
static void test_publisher_request_ack_sends_to_matched_peers(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;

    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1

    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;
    test_mock_send_to_call_count = 0;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_request_ack(&pub));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip);
    struct tt_HeartbeatHeader* sent =
        (struct tt_HeartbeatHeader*)(node.tx_buffer + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    EXPECT_EQ_U32(0, sent->flags & tt_HEARTBEAT_FLAG_FINAL); // response required, unlike a normal Heartbeat
}

// No currently-matched peers - nothing to solicit, a harmless no-op (tt_RET_OK, not an error - the
// caller's own peer_ack_seq_no[] scan afterward would just trivially see nothing to wait on).
static void test_publisher_request_ack_noop_with_no_peers(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;

    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    test_mock_send_to_call_count = 0;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_request_ack(&pub));
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// A best-effort Publisher (reliable_cache == NULL) has no ack state to solicit in the first place.
static void test_publisher_request_ack_requires_reliable_cache(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Publisher_request_ack(&pub));
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

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_heartbeat_period(&pub, 1000000));
    EXPECT_EQ_INT(1, node.scheduler_tail);

    tt_Publisher_destroy(&pub);
    EXPECT_EQ_INT(0, node.scheduler_tail);
}

// tt_Publisher_set_ack_solicit_period() must refuse to arm without reliable_cache set - same
// reasoning as tt_Publisher_set_heartbeat_period()'s own identical guard.
static void test_ack_solicit_set_period_requires_reliable_cache(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Publisher_set_ack_solicit_period(&pub, 1000000));
    EXPECT_EQ_INT(0, node.scheduler_tail);
}

// A successful tt_Publisher_set_ack_solicit_period() call arms exactly one scheduler entry,
// distinct from any already-armed periodic Heartbeat; disabling (period_ns == 0) removes just its
// own entry, leaving the other one untouched.
static void test_ack_solicit_set_period_arms_and_disarms(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_heartbeat_period(&pub, 1000000));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_ack_solicit_period(&pub, 500000));
    EXPECT_EQ_INT(2, node.scheduler_tail);
    EXPECT_EQ_U32(500000, (uint32_t)pub.ack_solicit_period_ns);

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_ack_solicit_period(&pub, 0));
    EXPECT_EQ_INT(1, node.scheduler_tail); // the still-armed Heartbeat entry remains
    EXPECT_EQ_U32(0, (uint32_t)pub.ack_solicit_period_ns);
}

// send_ack_solicit() itself: each tick unicasts a flag-clear Heartbeat straight to every
// currently-matched peer (via tt_Publisher_request_ack()), the same wire behavior tt_Publisher_
// request_ack()'s own direct test already covers - the thing genuinely new here is that this
// fires on a recurring timer, not just once per explicit call, and keeps rescheduling itself.
static void test_ack_solicit_fires_request_ack_periodically(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;

    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1

    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_ack_solicit_period(&pub, 500000));
    EXPECT_EQ_INT(1, node.scheduler_tail);
    test_mock_send_to_call_count = 0; // only count send_ack_solicit()'s own solicitation below

    send_ack_solicit(&node, tt_get_ns(), &pub);

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    struct tt_HeartbeatHeader* sent =
        (struct tt_HeartbeatHeader*)(node.tx_buffer + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    EXPECT_EQ_U32(0, sent->flags & tt_HEARTBEAT_FLAG_FINAL); // response required, same as a direct request_ack()
    // Not asserting node.scheduler_tail here - calling send_ack_solicit() directly (rather than via
    // the real scheduler dispatch, which pops the firing entry *before* invoking it) means the
    // already-armed entry from tt_Publisher_set_ack_solicit_period() above is still present when
    // this call's own re-arm adds a second one; same reasoning test_heartbeat_send_derives_range_
    // correctly() above already follows for its own direct send_heartbeat() call.
}

// A best-effort Publisher's own periodic ack-solicit tick must still self-reschedule even when
// tt_Publisher_request_ack() itself has nothing to solicit (no peers matched yet) - mirrors send_
// heartbeat()'s own "skip the send, still reschedule" behavior for its analogous case.
static void test_ack_solicit_reschedules_with_nothing_to_solicit(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache; // no peers matched, nothing published yet either

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_ack_solicit_period(&pub, 500000));
    test_mock_send_to_call_count = 0;

    send_ack_solicit(&node, tt_get_ns(), &pub);

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    // Not asserting node.scheduler_tail here - see test_ack_solicit_fires_request_ack_periodically()'s
    // own comment above for why a direct call double-counts against the already-armed entry.
}

// tt_Publisher_destroy() must cancel a still-armed periodic ack-solicit schedule entry too, not
// leave it dangling against a Publisher that no longer exists - mirrors test_publisher_destroy_
// cancels_armed_heartbeat() exactly, for the other of the two independent periodic mechanisms.
static void test_publisher_destroy_cancels_armed_ack_solicit(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_ack_solicit_period(&pub, 500000));
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

    TEST_RELIABLE_CACHE(cache, 4);
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

    TEST_RELIABLE_CACHE(cache, 4);
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

    TEST_RELIABLE_CACHE(reliable_cache, 4);
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

// Phase 3 prerequisite (c), rmw_tickle/PLAN.md - a remote node re-announcing (it changed *some*
// endpoint, which re-sends its whole entity list) makes process_update() forget and re-add its peer
// slots. That must not throw away ack state for a Subscriber that never went anywhere: Phase 3's
// KEEP_ALL blocking waits on exactly that state, and an unrelated announce would silently reset it.
static void test_publisher_peer_ack_survives_announce_refresh(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    // The remote Subscriber announces, becomes a peer, and acks up to 7.
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_INT((int)REMOTE_NODE_ID, (int)pub.peers[0].node_id);
    record_peer_ack(&pub, REMOTE_NODE_ID, 7);

    // It re-announces (a newer last_modified) while still listing the same Subscriber.
    tail = write_update_one_subscriber(&node, 200, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_INT((int)REMOTE_NODE_ID, (int)pub.peers[0].node_id); // re-added
    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID);
    EXPECT_TRUE(ack != NULL);
    EXPECT_EQ_U32(7, ack->ack_seq_no); // and its ack survived the round trip
}

// ...but an announce that genuinely drops the match (it no longer lists a Subscriber for this
// topic, or lists nothing at all - tt_Node_destroy()'s own farewell) must clear that ack state, so
// a departed Subscriber can't hold a KEEP_ALL writer's ack set forever.
static void test_publisher_peer_ack_dropped_when_announce_drops_match(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    record_peer_ack(&pub, REMOTE_NODE_ID, 7);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID) != NULL);

    // A Subscriber for a different topic only: this Publisher is no longer matched.
    tail = write_update_one_subscriber(&node, 200, ENDPOINT_ID + 1);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_INT((int)tt_NODE_ID_INVALID, (int)pub.peers[0].node_id);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID) == NULL);
}

// Publishes one sample and returns true if a solicited Heartbeat went out alongside the DATA. With
// one matched peer each publish unicasts its DATA (tt_UNICAST_PEER_THRESHOLD), so the send count
// rises by 1 for a plain publish and by 2 when the watermark also solicited an ACK.
static bool publish_and_check_solicit(struct tt_Publisher* pub) {
    uint32_t value = 1;
    int before = test_mock_send_to_call_count;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(pub, (struct tt_Data*)&value));
    int sent = test_mock_send_to_call_count - before;
    if (sent < 2) {
        return false;
    }
    const struct tt_HeartbeatHeader* heartbeat = last_sent_heartbeat();
    EXPECT_TRUE(heartbeat != NULL);
    EXPECT_TRUE((heartbeat->flags & tt_HEARTBEAT_FLAG_FINAL) == 0); // FINAL clear: a reply is required
    return true;
}

// Phase 3 prerequisite (d) - ack_solicit_watermark_pct is off by default, so an existing caller's
// healthy stream still sends no solicited Heartbeat at all, however full its cache gets.
static void test_ack_watermark_off_by_default_sends_nothing(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);
    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;
    EXPECT_EQ_U32(0, (uint32_t)pub.ack_solicit_watermark_pct);

    for (int i = 0; i < 8; i++) { // twice the depth: the cache is full of unacked samples
        EXPECT_TRUE(!publish_and_check_solicit(&pub));
    }
}

// With the watermark on, crossing it solicits exactly one Heartbeat, and the min-gap throttle
// suppresses every further crossing until the gap has elapsed - the property that keeps a max-rate
// Publisher (which crosses the watermark on essentially every publish) from heartbeat-flooding.
static void test_ack_watermark_solicits_once_then_throttles(void) {
    test_mock_reset();
    test_mock_now = 1000000; // non-zero: last_ack_solicit_ns == 0 means "never solicited"

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);
    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.ack_solicit_watermark_pct = 50; // solicit once 2 of 4 slots are unacked
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;

    EXPECT_TRUE(!publish_and_check_solicit(&pub)); // 1 unacked of 4 - below the watermark
    EXPECT_TRUE(publish_and_check_solicit(&pub));  // 2 of 4 - one solicited Heartbeat

    for (int i = 0; i < 10; i++) { // still past the watermark, but inside the min gap
        EXPECT_TRUE(!publish_and_check_solicit(&pub));
    }

    test_mock_now += tt_RELIABLE_RETRY_INTERVAL; // the gap elapses
    EXPECT_TRUE(publish_and_check_solicit(&pub));
}

// An acking peer keeps the cache below the watermark, so nothing is solicited: the trigger is
// *unacknowledged* samples, not merely retained ones.
static void test_ack_watermark_not_triggered_while_peer_keeps_up(void) {
    test_mock_reset();
    test_mock_now = 1000000;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);
    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.ack_solicit_watermark_pct = 50;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;

    for (int i = 0; i < 8; i++) {
        EXPECT_TRUE(!publish_and_check_solicit(&pub));
        record_peer_ack(&pub, REMOTE_NODE_ID, pub.seq_no + 1); // acked everything published so far
    }
}

// With no matched peer there is nobody to ask, so a watermark crossing must not heartbeat into the
// void (Plan's B1-review ask).
static void test_ack_watermark_silent_without_peers(void) {
    test_mock_reset();
    test_mock_now = 1000000;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);
    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.ack_solicit_watermark_pct = 50; // pub.peers[] left entirely empty

    uint32_t value = 1;
    for (uint32_t i = 0; i < 8; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // no peers: DATA broadcasts, no solicit
    EXPECT_EQ_U32(0, (uint32_t)pub.last_ack_solicit_ns);      // not even the throttle timestamp moved
}

int main(void) {
    test_publisher_peer_ack_survives_announce_refresh();
    test_publisher_peer_ack_dropped_when_announce_drops_match();
    test_ack_watermark_off_by_default_sends_nothing();
    test_ack_watermark_solicits_once_then_throttles();
    test_ack_watermark_not_triggered_while_peer_keeps_up();
    test_ack_watermark_silent_without_peers();
    test_heartbeat_set_period_requires_reliable_cache();
    test_heartbeat_set_period_arms_and_disarms();
    test_heartbeat_send_derives_range_correctly();
    test_heartbeat_send_skips_when_nothing_retained_yet();
    test_heartbeat_first_contact_sets_baseline_with_no_data_ever_received();
    test_heartbeat_first_contact_volatile_subscriber_skips_backlog();
    test_heartbeat_oversized_gap_jumps_baseline();
    test_heartbeat_first_available_advances_existing_proxy();
    test_heartbeat_stale_first_available_is_ignored();
    test_heartbeat_durable_late_joiner_backlog_not_skipped();
    test_heartbeat_gap_within_window_widens_request_without_jumping();
    test_heartbeat_ignored_for_besteffort_subscriber();
    test_heartbeat_final_flag_clear_forces_acknack_without_gap();
    test_heartbeat_final_flag_set_stays_silent_without_gap();
    test_publisher_request_ack_sends_to_matched_peers();
    test_publisher_request_ack_noop_with_no_peers();
    test_publisher_request_ack_requires_reliable_cache();
    test_publisher_destroy_cancels_armed_heartbeat();
    test_ack_solicit_set_period_requires_reliable_cache();
    test_ack_solicit_set_period_arms_and_disarms();
    test_ack_solicit_fires_request_ack_periodically();
    test_ack_solicit_reschedules_with_nothing_to_solicit();
    test_publisher_destroy_cancels_armed_ack_solicit();
    test_heartbeat_discovery_sends_immediate_heartbeat_to_new_peer();
    test_heartbeat_discovery_skipped_for_besteffort_publisher();
    test_heartbeat_discovery_no_redelivery_on_unchanged_update();
    test_heartbeat_discovery_sends_both_durability_backlog_and_heartbeat();

    printf("test_heartbeat: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
