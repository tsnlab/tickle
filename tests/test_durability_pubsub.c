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

// Builds a DataHeader + 4-byte payload at the start of node->rx_buffer, returning the tail
// offset (matching what process_packet() would have handed process_data()) - same helper as
// tests/test_reliable_pubsub.c's own write_data(), needed here too for this file's own combined
// RELIABILITY+DURABILITY regression test (each tests/test_*.c is its own standalone binary, no
// helpers shared across files).
static uint32_t write_data(struct tt_Node* node, uint32_t seq_no, uint64_t timestamp, uint32_t value) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node->rx_buffer;
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = timestamp;

    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node->rx_buffer + tail, &value, sizeof(value));
    return tail + sizeof(value);
}

// See write_data()'s own comment - same helper as tests/test_reliable_pubsub.c's own write_acknack().
static uint32_t write_acknack(struct tt_Node* node, uint32_t endpoint_id, uint32_t seq_no, uint64_t bitmap) {
    struct tt_AckNackHeader* acknack_header = (struct tt_AckNackHeader*)node->rx_buffer;
    acknack_header->endpoint_id = endpoint_id;
    acknack_header->seq_no = seq_no;
    acknack_header->bitmap = bitmap;
    return sizeof(struct tt_AckNackHeader);
}

// tt_Publisher_publish() on a Publisher with durable set (and reliable_cache backing it - see
// struct tt_ReliableCache's own doc comment on the two QoS policies now sharing one cache) must
// snapshot every sample into the ring, and once more than `depth` samples have gone out, only the
// most recent `depth` (KEEP_LAST) must remain - same eviction shape as test_reliable_pubsub.c's
// own test_reliable_publish_caches_and_evicts, mirrored here with `durable` set too.
static void test_durability_publish_caches_and_evicts(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.durable = true;

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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.durable = true;

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

// QoS roadmap #6 (LIFESPAN) - a newly-discovered Subscriber must not receive backlog entries past
// pub->lifespan_duration_ns, even though they're still physically retained in reliable_cache -
// "as if it had never been sent" (tt_Publisher.lifespan_duration_ns's own doc comment, tickle.h).
static void test_durability_skips_expired_backlog_entries(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.durable = true;
    pub.lifespan_duration_ns = 1000;

    for (uint32_t i = 0; i < 3; i++) {
        uint32_t value = i;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1..3, ts 0
    }

    test_mock_now = 1000; // exactly at the lifespan boundary - already expired (>=, not >)
    node.update_seen[REMOTE_NODE_ID] = true;
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // every retained sample already expired
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.durable = true;

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

// Milestone 58 (rmw_tickle/PLAN.md) - a liveliness "presumed dead" false positive (a load-induced
// gap in a still-alive peer's periodic UPDATE announces, not a real departure - check_liveliness()'s
// own doc comment) must not cause DURABLE backlog re-delivery once that peer's very next (otherwise
// unchanged) announce arrives. check_liveliness() wipes peers[]/update_seen[]/update_last_modified[]
// for the presumed-dead source, which defeats process_update()'s own "nothing changed" dedup (it
// only short-circuits when update_seen[source] is still true) and makes upsert_peer() see the
// recovering peer as a genuinely new slot claim again - but its own last_modified is unchanged
// (nothing about its Publisher/Subscriber set actually changed), so durable_delivered[] (tickle.h)
// must still remember it already has this backlog.
static void test_durability_no_redelivery_after_liveliness_false_positive(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.durable = true;

    for (uint32_t i = 0; i < 3; i++) {
        uint32_t value = i;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1..3
    }

    node.update_seen[REMOTE_NODE_ID] = true; // see write_update_one_subscriber() callers' own comment on why
    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, (uint32_t)test_mock_send_to_call_count); // first contact, backlog delivered once

    // Simulate check_liveliness()'s own presumed-dead cleanup directly - this test cares about its
    // effect on the peer/update_seen tables, not the timeout arithmetic itself (already covered by
    // tests/test_liveliness.c).
    node.update_last_seen[REMOTE_NODE_ID] = 0;
    check_liveliness(&node, ((uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL) + 1, NULL);
    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]); // confirms the false-positive cleanup actually ran

    // Same as above - re-set so the recovering peer's own reply_with_own_announce() (a real, but
    // unrelated to durability, "first contact" reply - update_last_modified[] is still 0 from
    // check_liveliness()'s own reset, so process_update()'s "nothing changed" dedup still doesn't
    // short-circuit and decode_update_entities() still runs) doesn't confound this test's own
    // send_to count, same reasoning as this file's other tests.
    node.update_seen[REMOTE_NODE_ID] = true;
    test_mock_send_to_call_count = 0; // only count the recovering peer's own re-announce below

    // Same last_modified as before (the exact same continuous instance, nothing about its own
    // Publisher/Subscriber set changed) - looks like first contact again to upsert_peer() (peers[]
    // was just wiped), but durable_delivered[] must still remember it, so no redundant backlog
    // re-delivery happens.
    tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// The mirror image of the test above: a *genuine* restart of the same node_id (a fresh last_
// modified, since its own Publisher/Subscriber set was actually rebuilt at startup) must still get
// the backlog delivered even after an identical check_liveliness() cleanup - durable_delivered[]'s
// own (node_id, last_modified) key is what tells the two cases apart, not node_id alone.
static void test_durability_redelivers_after_genuine_restart(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.durable = true;

    for (uint32_t i = 0; i < 3; i++) {
        uint32_t value = i;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }

    node.update_seen[REMOTE_NODE_ID] = true; // see write_update_one_subscriber() callers' own comment on why
    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_update_one_subscriber(&node, 100, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, (uint32_t)test_mock_send_to_call_count);

    node.update_last_seen[REMOTE_NODE_ID] = 0;
    check_liveliness(&node, ((uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL) + 1, NULL);
    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]);

    node.update_seen[REMOTE_NODE_ID] = true; // suppress reply_with_own_announce()'s own unrelated send, see above
    test_mock_send_to_call_count = 0;

    // A different last_modified - a genuine restart, its own subscription state was wiped too, it
    // needs the backlog again.
    tail = write_update_one_subscriber(&node, 200, ENDPOINT_ID);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(3, (uint32_t)test_mock_send_to_call_count);
}

// A VOLATILE Publisher (durable == false, today's default) seeing a brand-new Subscriber must be
// a harmless no-op - not a crash, not a delivery of anything.
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

// Regression test for PLAN.md's Milestone 20 (originally written against two separate caches,
// simplified when Milestone 24 unified them into one - see struct tt_ReliableCache's own doc
// comment): a Publisher with *both* RELIABLE and TRANSIENT_LOCAL enabled, already well into a
// long-running stream (simulated: 100 publishes) before this test's own Subscriber ever shows up -
// only the last 4 samples (seq_no 97..100) remain in the cache. A brand-new Subscriber's default
// ack_seq_no (1) is far more than tt_RELIABLE_BITMAP_BITS behind that, exactly the oversized-
// first-gap shape test_reliable_pubsub.c's own test_reliable_subscribe_oversized_first_gap_jumps_
// baseline_instead_of_freezing() proves the fix for - confirmed here end to end: once the first
// real arrival establishes a live baseline, a *subsequent* gap within that same backlog burst is
// tracked normally and, crucially, still finds its sample sitting in the same cache for process_
// acknack() to retransmit - the actual "durability backlog is ACKNACK-protected too" guarantee
// this milestone is about, now structurally guaranteed rather than depending on a depth invariant
// between two caches (there's only one cache to find it in). Both sides (Subscriber-side process_
// data()/update_reliable_ack(), Publisher-side process_acknack()) are simulated directly on this
// one mock node, same convention test_reliable_pubsub.c's own process_acknack()/process_data()
// tests already use - deliver_durability_backlog() itself is covered separately by test_
// durability_delivers_backlog_to_newly_discovered_subscriber() above; what's new here is proving a
// sample *it* would have unicast is independently recoverable.
static void test_durability_backlog_recovered_via_acknack_when_reliable_too(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher_registered_on_node(&pub, &node, &topic);

    struct tt_ReliableCacheEntry reliable_cache_entries[4];
    memset(reliable_cache_entries, 0, sizeof(reliable_cache_entries));
    struct tt_ReliableCache reliable_cache;
    memset(&reliable_cache, 0, sizeof(reliable_cache));
    reliable_cache.entries = reliable_cache_entries;
    reliable_cache.capacity = 4;
    reliable_cache.depth = 4;
    pub.reliable_cache = &reliable_cache;
    pub.durable = true;
    // Milestone 62 (rmw_tickle/PLAN.md) - a real, pre-existing gap this function's own name/intent
    // ("_when_reliable_too") never actually matched its own setup: process_acknack() didn't check
    // pub->reliable at all before this milestone, so the ACKNACK-driven recovery this test exists
    // to prove worked "by accident" purely off reliable_cache being non-NULL - once process_acknack()
    // correctly started gating on pub->reliable too, this test's own final assertion started failing
    // (a durable-only Publisher, as this one actually was, no longer answers ACKNACK at all) until
    // this line was added to match what the test was always meant to exercise.
    pub.reliable = true;

    for (uint32_t i = 0; i < 100; i++) { // simulates a long-running stream, seq_no 1..100
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }
    EXPECT_EQ_U32(100, (uint32_t)pub.seq_no);

    // The late-joining Subscriber, simulated directly (not through decode_update_entities()'s own
    // unicast delivery - that part is already covered elsewhere, see this function's own doc
    // comment) - registered on the same node so process_data() can find it via find_endpoint(),
    // sharing ENDPOINT_ID with pub (find_endpoint() distinguishes by kind, not just id).
    struct tt_Subscriber sub;
    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = ENDPOINT_ID;
    sub.node = &node;
    sub.topic = &topic;
    sub.callback = stub_subscriber_callback;
    sub.reliable = true;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].node_id = tt_NODE_ID_INVALID; // all empty - never heard from this Publisher
                                                     // before, exactly the "first contact" case
                                                     // this milestone's own sequencing question was
                                                     // about (Milestone 47 - now a WriterProxy table
                                                     // entry, created lazily on first contact).
    }
    node.endpoints[1] = (struct tt_Endpoint*)&sub;
    node.endpoint_count = 2;

    struct tt_Header header;
    init_header(&header);

    // Backlog sample seq_no 97 arrives first (as deliver_durability_backlog() would send it,
    // oldest first).
    uint32_t tail = write_data(&node, 97, 9700, 97);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, REMOTE_NODE_ID, 0);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(98, proxy->ack_seq_no); // jumped to just past 97, not stuck at 1 - the Milestone 20 fix
    EXPECT_TRUE(proxy->received_bitmap == 0);

    // seq_no 98 is "lost in flight" (deliver_durability_backlog()'s own unicast never arrives) -
    // seq_no 100 (the newest retained sample) arrives instead.
    test_mock_send_to_call_count = 0; // only count the ACKNACK below
    tail = write_data(&node, 100, 10000, 100);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(98, proxy->ack_seq_no);        // correctly still waiting on 98 (and 99)
    EXPECT_TRUE(proxy->received_bitmap == 4ULL); // bit 2 -> seq_no 100 (98 + 2) received early
    EXPECT_TRUE(proxy->acknack_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // a real ACKNACK requesting 98 (and 99)

    // Publisher side: seq_no 98 must still actually be sitting in reliable_cache (it is - only
    // the last 4, 97..100, were ever evicted to, and 98 is one of them) for process_acknack() to
    // find and retransmit - the guarantee the depth invariant above exists to provide.
    test_mock_send_to_call_count = 0;                   // only count the retransmit below
    tail = write_acknack(&node, ENDPOINT_ID, 98, 1ULL); // requesting seq_no 98 (bit 0)
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // found it, retransmitted - not silently dropped
}

int main(void) {
    test_durability_publish_caches_and_evicts();
    test_durability_delivers_backlog_to_newly_discovered_subscriber();
    test_durability_skips_expired_backlog_entries();
    test_upsert_peer_true_only_for_new_slot();
    test_durability_no_redelivery_on_unchanged_update();
    test_durability_no_redelivery_after_liveliness_false_positive();
    test_durability_redelivers_after_genuine_restart();
    test_durability_ignored_for_volatile_publisher();
    test_durability_backlog_recovered_via_acknack_when_reliable_too();

    printf("test_durability_pubsub: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
