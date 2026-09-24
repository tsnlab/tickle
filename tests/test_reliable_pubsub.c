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

// Whitebox: QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - exercises
// tt_Publisher_publish()'s reliable_cache store/evict, process_data()'s update_reliable_ack()
// gap tracking, acknack_retry()'s give-up path, and process_acknack()'s retransmit - all static,
// same "reach tickle.c's own statics" approach as test_publish_subscribe.c.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define REMOTE_SUB_ENTITY_ID 0x22220001 // Phase 2 - which remote Subscriber entity acks
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

static void init_publisher(struct tt_Publisher* pub, struct tt_Node* node, struct tt_Topic* topic) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.name = "test_publisher";
    pub->node = node;
    pub->topic = topic;
}

#define TEST_REORDER_SLOTS 16
#define TEST_REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + 64)
static uint64_t test_reorder_storage[TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES / sizeof(uint64_t)];

static void init_subscriber_registered_on_node(struct tt_Subscriber* sub, struct tt_Node* node,
                                               struct tt_Topic* topic) {
    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = ENDPOINT_ID;
    sub->node = node;
    sub->topic = topic;
    sub->callback = stub_subscriber_callback;
    sub->reliable = true;
    // A reorder buffer, the way a Linux-class caller supplies one (tickle.h's own reorder_storage
    // comment). Every test in this file asserts on gap *tracking* - which bits are set, where the
    // watermark is - and a RELIABLE Subscriber with no buffer un-receives a sample that arrives
    // ahead of the gap so the ACKNACK exchange fetches it again. That is correct, and it would
    // turn every one of these into a test of the no-buffer fallback instead of the thing it was
    // written for.
    sub->reorder_storage = test_reorder_storage;
    sub->reorder_slots = TEST_REORDER_SLOTS;
    sub->reorder_slot_bytes = TEST_REORDER_SLOT_BYTES;
    memset(test_reorder_storage, 0, sizeof(test_reorder_storage));
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].node_id = tt_NODE_ID_INVALID; // all empty - matches tt_Node_create_
                                                      // subscriber()'s own init (Milestone 47 -
                                                      // each writer's own ack_seq_no starts at 1
                                                      // lazily, on first contact, see struct tt_
                                                      // WriterProxy's own doc comment, tickle.h)
    }

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)sub;
}

// Milestone 47 - shorthand for the WriterProxy every test in this file cares about: this file's
// own single simulated remote Publisher, REMOTE_NODE_ID with entity_id 0 (write_data()/write_
// acknack() below never set tt_DataHeader.entity_id/tt_AckNackHeader.entity_id, so it stays 0 -
// node->rx_buffer starts zeroed, see init_node_and_topic()). Looks up the entry process_data()/
// process_heartbeat() already created on first contact - tests that need to pre-seed state before
// any real packet call find_or_create_writer_proxy() directly instead (see e.g. test_acknack_
// retry_exhausted_gives_up()).
static struct tt_WriterProxy* remote_writer_proxy(struct tt_Subscriber* sub) {
    struct tt_WriterProxy* proxy = find_writer_proxy(sub, REMOTE_NODE_ID, 0);
    if (proxy != NULL && proxy->keep_all == tt_WRITER_KEEP_ALL_UNKNOWN) {
        // Phase 3 step 4 - these tests model an ordinary writer whose announce has already been
        // seen, so say so. A proxy claimed from DATA alone starts UNKNOWN and never gives up (by
        // design - see tt_WriterProxy.keep_all), which would otherwise silently disarm every
        // give-up assertion in this file. The tests that care about UNKNOWN set it themselves.
        proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    }
    return proxy;
}

static void init_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
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

// `bitmap` fills word 0 only (every call site in this file fits well within 64 bits) - the rest of
// acknack_header->bitmap[] relies on node->rx_buffer starting zeroed, same as every other
// multi-byte field this helper leaves at its default.
static uint32_t write_acknack(struct tt_Node* node, uint32_t endpoint_id, uint32_t seq_no, uint64_t bitmap) {
    struct tt_AckNackHeader* acknack_header = (struct tt_AckNackHeader*)node->rx_buffer;
    memset(acknack_header, 0, sizeof(*acknack_header));
    acknack_header->endpoint_id = endpoint_id;
    acknack_header->sender_entity_id = REMOTE_SUB_ENTITY_ID; // Phase 2
    acknack_header->seq_no = seq_no;
    // Phase 2 - the bitmap is variable length now: one word here, with the count on the wire.
    acknack_header->bitmap_words = 1;
    acknack_header->bitmap[0] = bitmap;
    return sizeof(struct tt_AckNackHeader) + sizeof(uint64_t);
}

// Compares the multi-word received_bitmap against a plain uint64_t test expectation - every test
// value in this file fits in word 0, so this is "word 0 == low, every other word == 0".
static bool bitmap_equals_u64(const uint64_t bitmap[tt_RELIABLE_BITMAP_WORDS], uint64_t low) {
    if (bitmap[0] != low) {
        return false;
    }
    for (int w = 1; w < tt_RELIABLE_BITMAP_WORDS; w++) {
        if (bitmap[w] != 0) {
            return false;
        }
    }
    return true;
}

// Sets received_bitmap's own word 0 to `low`, zeroing every other word - the test-side equivalent
// of bitmap_set_bit()/bitmap_clear() combined, for tests that pre-seed proxy state directly rather
// than driving it through a real ACKNACK/DATA packet.
static void bitmap_set_u64(uint64_t bitmap[tt_RELIABLE_BITMAP_WORDS], uint64_t low) {
    bitmap[0] = low;
    for (int w = 1; w < tt_RELIABLE_BITMAP_WORDS; w++) {
        bitmap[w] = 0;
    }
}

// tt_Publisher_publish() on a Publisher with reliable_cache set must snapshot every sample into
// the ring, and once more than `depth` samples have gone out, only the most recent `depth`
// (KEEP_LAST) must remain - the oldest ones evicted, same eviction shape as
// rmw_subscription.c's own reader-side queue.
static void test_reliable_publish_caches_and_evicts(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;

    for (uint32_t i = 0; i < 6; i++) { // more than depth -> the first 2 (seq_no 1, 2) get evicted
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }
    EXPECT_EQ_U32(6, (uint32_t)pub.seq_no);

    bool saw_evicted = false;
    int live_count = 0;
    for (int i = 0; i < 4; i++) {
        if (cache.index[i].len != 0) {
            live_count++;
            if (cache.index[i].seq_no == 1 || cache.index[i].seq_no == 2) {
                saw_evicted = true;
            }
        }
    }
    EXPECT_EQ_INT(4, live_count);
    EXPECT_TRUE(!saw_evicted);
}

// A reliable Subscriber receiving every sample in order must never generate an ACKNACK - a
// healthy stream needs none - and must still deliver every sample to its callback.
static void test_reliable_subscribe_in_order_no_acknack(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    for (uint32_t seq = 1; seq <= 3; seq++) {
        uint32_t tail = write_data(&node, seq, (uint64_t)seq * 100, seq);
        EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    }

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(4, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(3, (uint32_t)subscriber_callback_count);
}

// --- B1 (rmw_tickle/PLAN.md): byte-arena cache ------------------------------------------------
//
// These drive cache_reliable_sample() directly (whitebox, like the rest of this file) so a record
// of any size can be written without inventing a matching topic codec: it caches
// ROUNDUP(tx_tail - submessage_header) bytes starting at submessage_header, so writing a pattern
// into node.tx_buffer and setting tx_tail is exactly one "publish" as far as the cache is concerned.

// Writes a `payload_len`-byte record for seq_no whose bytes are all (uint8_t)seq_no, and caches it.
static void cache_write_record(struct tt_Node* node, struct tt_ReliableCache* cache, uint32_t seq_no,
                               uint32_t payload_len) {
    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)(node->tx_buffer);
    // The cache stores ROUNDUP(len) bytes (end_encode()'s own 4-byte submessage alignment), so
    // fill the padding too - otherwise cache_record_intact() below compares against whatever the
    // previous record left in those one-to-three bytes.
    memset(node->tx_buffer, (int)(seq_no & 0xFF), ROUNDUP(payload_len));
    node->tx_tail = payload_len;
    cache_reliable_sample(node, submessage_header, cache, seq_no);
}

// True when seq_no is retained right now *and* its arena bytes still read back as written.
static bool cache_record_intact(const struct tt_ReliableCache* cache, uint32_t seq_no) {
    if (cache->oldest_seq_no == 0 || seq_no < cache->oldest_seq_no || seq_no > cache->newest_seq_no) {
        return false;
    }
    const struct tt_ReliableCacheIndex* entry = &cache->index[(seq_no - 1) % cache->depth];
    if (entry->seq_no != seq_no || entry->len == 0) {
        return false;
    }
    if ((uint32_t)entry->offset + entry->len > cache->arena_size) {
        return false; // a record must never be split across the end of the arena
    }
    for (uint16_t i = 0; i < entry->len; i++) {
        if (cache->arena[entry->offset + i] != (uint8_t)(seq_no & 0xFF)) {
            return false;
        }
    }
    return true;
}

static int cache_retained_count(const struct tt_ReliableCache* cache) {
    int count = 0;
    if (cache->oldest_seq_no == 0) {
        return 0;
    }
    for (uint32_t seq_no = cache->oldest_seq_no; seq_no <= cache->newest_seq_no; seq_no++) {
        if (cache_record_intact(cache, seq_no)) {
            count++;
        }
    }
    return count;
}

// The byte bound: with an arena far smaller than `depth` records, the oldest records are evicted to
// make room, newest-first retention is preserved, and every survivor's bytes are untouched.
static void test_reliable_cache_evicts_by_bytes_oldest_first(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    init_node_and_topic(&node, &topic);

    const uint32_t record = 32; // ROUNDUP(32) == 32
    TEST_RELIABLE_CACHE(cache, 8);
    cache.arena_size = 3 * record; // room for 3 records, but depth 8

    for (uint32_t seq_no = 1; seq_no <= 6; seq_no++) {
        cache_write_record(&node, &cache, seq_no, record);
    }

    EXPECT_EQ_U32(4, cache.oldest_seq_no); // 1..3 evicted for room, not for depth
    EXPECT_EQ_U32(6, cache.newest_seq_no);
    EXPECT_EQ_INT(3, cache_retained_count(&cache));
    for (uint32_t seq_no = 4; seq_no <= 6; seq_no++) {
        EXPECT_TRUE(cache_record_intact(&cache, seq_no));
    }
    for (uint32_t seq_no = 1; seq_no <= 3; seq_no++) {
        EXPECT_TRUE(!cache_record_intact(&cache, seq_no));
    }
}

// Fix 1 (Plan's B1 review): records are never split, so a wrap wastes the tail fragment. An arena
// sized with tt_RELIABLE_CACHE_ARENA_BYTES() carries one record of slack for exactly that, so
// `depth` samples are always retained however the wraps land - and without the slack they aren't.
static void test_reliable_cache_wrap_slack_keeps_depth_samples(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    init_node_and_topic(&node, &topic);

    const uint32_t record = 36; // not a divisor of either arena size below - wraps land mid-arena
    const uint16_t depth = 4;

    TEST_RELIABLE_CACHE(cache, 4);
    cache.arena_size = (depth + 1) * record; // what tt_RELIABLE_CACHE_ARENA_BYTES() gives
    for (uint32_t seq_no = 1; seq_no <= 40; seq_no++) {
        cache_write_record(&node, &cache, seq_no, record);
        int expected = (int)(seq_no < depth ? seq_no : depth);
        EXPECT_EQ_INT(expected, cache_retained_count(&cache));
    }

    // Plan's requested case: depth-full with *maximum*-size records, across several wraps - the
    // arena is sized in max records, so this is where a missing slack byte would show up first.
    TEST_RELIABLE_CACHE(max_sized, 4);
    const uint32_t max_record = 64;
    max_sized.arena_size = tt_RELIABLE_CACHE_ARENA_BYTES(depth, max_record);
    for (uint32_t seq_no = 1; seq_no <= 60; seq_no++) {
        cache_write_record(&node, &max_sized, seq_no, max_record);
        EXPECT_EQ_INT((int)(seq_no < depth ? seq_no : depth), cache_retained_count(&max_sized));
    }

    // Mixed sizes, same arena: still exactly `depth` retained however the wraps land. (With one
    // uniform size the slack is never strictly needed - depth * record divides the arena evenly, so
    // a wrap wastes nothing - it's varying sizes that strand a partial record's worth of bytes.)
    const uint32_t sizes[] = {36, 20, 48, 24, 64};
    TEST_RELIABLE_CACHE(mixed, 4);
    mixed.arena_size = tt_RELIABLE_CACHE_ARENA_BYTES(depth, max_record);
    for (uint32_t seq_no = 1; seq_no <= 60; seq_no++) {
        cache_write_record(&node, &mixed, seq_no, sizes[seq_no % 5]);
        EXPECT_EQ_INT((int)(seq_no < depth ? seq_no : depth), cache_retained_count(&mixed));
    }
}

// Fix 2 (Plan's B1 review): a record larger than the whole arena is published but not retained -
// no byte eviction on its behalf, its slot left a tombstone, and the retained range still tracks it
// as the newest sample so first_resendable/direct indexing stay correct.
static void test_reliable_cache_oversize_record_is_not_cached(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    init_node_and_topic(&node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    cache.arena_size = 64;

    cache_write_record(&node, &cache, 1, 32);
    cache_write_record(&node, &cache, 2, 32);
    EXPECT_EQ_INT(2, cache_retained_count(&cache));

    cache_write_record(&node, &cache, 3, 128); // larger than the whole 64-byte arena
    EXPECT_EQ_U32(3, cache.newest_seq_no);
    EXPECT_TRUE(!cache_record_intact(&cache, 3));
    EXPECT_EQ_INT(2, cache_retained_count(&cache)); // 1 and 2 untouched - no eviction on its behalf
    EXPECT_TRUE(cache_record_intact(&cache, 1));
    EXPECT_TRUE(cache_record_intact(&cache, 2));

    // An ACKNACK naming it must report it gone (Phase 1-c's eviction Heartbeat path), not resend
    // whatever bytes happen to sit in its slot.
    bool gone = false;
    EXPECT_TRUE(find_resendable_cache_entry(&cache, cache.depth, 3, 0, false, &gone) == NULL);
    EXPECT_TRUE(gone);
}

// An oversized record at the very front (nothing retained yet) must leave the cache empty rather
// than "oldest = a sample whose bytes were never stored".
static void test_reliable_cache_oversize_record_on_empty_cache(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    init_node_and_topic(&node, &topic);

    TEST_RELIABLE_CACHE(cache, 4);
    cache.arena_size = 64;

    cache_write_record(&node, &cache, 1, 128);
    EXPECT_EQ_U32(0, cache.oldest_seq_no); // nothing retained
    EXPECT_EQ_U32(1, cache.newest_seq_no); // but it was published
    EXPECT_EQ_INT(0, cache_retained_count(&cache));

    cache_write_record(&node, &cache, 2, 32); // the cache still works afterwards
    EXPECT_EQ_U32(2, cache.oldest_seq_no);
    EXPECT_TRUE(cache_record_intact(&cache, 2));
}

// Randomized property check against the invariants that matter, rather than a second copy of the
// eviction logic: retention is always a contiguous *newest-first* suffix (KEEP_LAST), never more
// than `depth` samples, every retained record reads back exactly as written and lies wholly inside
// the arena, and no two retained records overlap. Fixed seed, so a failure is reproducible.
static void test_reliable_cache_randomized_invariants(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    init_node_and_topic(&node, &topic);

    const uint16_t depth = 16;
    TEST_RELIABLE_CACHE(cache, 16);
    cache.arena_size = 700; // deliberately far smaller than depth * max record: both bounds bite

    uint32_t rng = 12345;
    for (uint32_t seq_no = 1; seq_no <= 20000; seq_no++) {
        rng = (rng * 1103515245U) + 12345U;
        uint32_t payload_len = 4 + ((rng >> 16) % 160); // 4..163 bytes, ROUNDUP'd by the cache
        cache_write_record(&node, &cache, seq_no, payload_len);

        EXPECT_TRUE(cache.newest_seq_no == seq_no);
        int retained = cache_retained_count(&cache);
        EXPECT_TRUE(retained <= (int)depth);
        if (cache.oldest_seq_no != 0) {
            EXPECT_TRUE(cache.newest_seq_no - cache.oldest_seq_no + 1 <= (uint32_t)depth);
        }

        // Contiguous newest-first suffix: once a sample is retained, every newer one is too.
        bool seen_retained = false;
        for (uint32_t s = cache.oldest_seq_no; s != 0 && s <= cache.newest_seq_no; s++) {
            bool intact = cache_record_intact(&cache, s);
            if (seen_retained && !intact) {
                EXPECT_TRUE(s == cache.newest_seq_no); // only the newest may be an uncached oversize
                break;
            }
            seen_retained = seen_retained || intact;
        }

        // No two retained records overlap in the arena.
        for (uint32_t a = cache.oldest_seq_no; a != 0 && a < cache.newest_seq_no; a++) {
            if (!cache_record_intact(&cache, a)) {
                continue;
            }
            const struct tt_ReliableCacheIndex* ea = &cache.index[(a - 1) % depth];
            for (uint32_t b = a + 1; b <= cache.newest_seq_no; b++) {
                if (!cache_record_intact(&cache, b)) {
                    continue;
                }
                const struct tt_ReliableCacheIndex* eb = &cache.index[(b - 1) % depth];
                EXPECT_TRUE(ea->offset + ea->len <= eb->offset || eb->offset + eb->len <= ea->offset);
            }
        }
    }
}

// A gap (seq_no 2 missing between 1 and 3) must produce an immediate ACKNACK back to the sender
// and arm a retry; the late arrival of the missing sample must then close the gap and cancel
// that retry.
static void test_reliable_subscribe_gap_then_close(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);

    tail = write_data(&node, 3, 300, 3); // seq_no 2 skipped
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(2, proxy->ack_seq_no);                          // still waiting on 2
    EXPECT_TRUE(bitmap_equals_u64(proxy->received_bitmap, 2ULL)); // bit 1 -> seq_no 3 (ack_seq_no + 1) received early
    EXPECT_TRUE(proxy->acknack_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // immediate ACKNACK
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip);
    EXPECT_EQ_U32(TEST_SENDER_PORT, (uint32_t)test_mock_send_to_last_port);

    tail = write_data(&node, 2, 200, 2); // the missing sample finally arrives
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(4, proxy->ack_seq_no); // 2 lands, then absorbs the already-buffered bit for 3
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
    EXPECT_TRUE(!proxy->acknack_scheduled);
}

// Decodes the most recently sent packet as a single ACKNACK (the only thing a Subscriber-side test
// here ever sends) - NULL if it wasn't one.
static const struct tt_AckNackHeader* last_sent_acknack(void) {
    size_t submessage_off = sizeof(struct tt_Header);
    size_t acknack_off = submessage_off + sizeof(struct tt_SubmessageHeader);
    if (test_mock_send_last_len < acknack_off + sizeof(struct tt_AckNackHeader)) {
        return NULL;
    }
    const struct tt_SubmessageHeader* submessage =
        (const struct tt_SubmessageHeader*)(test_mock_send_last_buf + submessage_off);
    if (submessage->type != tt_SUBMESSAGE_TYPE_ACKNACK) {
        return NULL;
    }
    return (const struct tt_AckNackHeader*)(test_mock_send_last_buf + acknack_off);
}

// Same, for a Heartbeat (Phase 1-c's eviction Heartbeat) - NULL if the last packet wasn't one.
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

// Phase 1-a (rmw_tickle/PLAN.md, H2) - a gap that opens while the retry timer from an earlier gap
// is still armed gets its own immediate ACKNACK, naming only its own positions (never re-requesting
// the earlier, still-open gap, whose retransmit may already be in flight). Arrivals that open no
// new gap - contiguous with the highest received, or filling an existing gap - send nothing, so this
// can't regress into the per-DATA ACKNACK flood maybe_arm_acknack_retry()'s own comment describes.
static void test_reliable_new_gap_while_armed_gets_immediate_narrow_nack(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 3, 300, 3); // gap at 2: the usual immediate ACKNACK, arms the retry timer
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_TRUE(proxy->acknack_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    const struct tt_AckNackHeader* acknack = last_sent_acknack();
    EXPECT_TRUE(acknack != NULL);
    EXPECT_EQ_U32(2, acknack->seq_no);
    EXPECT_TRUE(acknack->bitmap[0] == 0x1ULL); // seq_no 2

    tail = write_data(&node, 7, 700, 7); // new gap at 4..6 while armed
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_to_call_count); // immediate, not left for the 5ms timer
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip);
    acknack = last_sent_acknack();
    EXPECT_TRUE(acknack != NULL);
    EXPECT_EQ_U32(2, acknack->seq_no);          // base (cumulative ack) unchanged
    EXPECT_TRUE(acknack->bitmap[0] == 0x1CULL); // bits 2..4 = seq_no 4..6 only, not seq_no 2 again
    for (int word = 1; word < tt_RELIABLE_BITMAP_WORDS; word++) {
        EXPECT_TRUE(acknack->bitmap[word] == 0);
    }

    tail = write_data(&node, 8, 800, 8); // contiguous with the highest received - no new gap
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 5, 500, 5); // fills part of an existing gap - no new gap
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 2, 200, 2); // fills the head gap - no new gap
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(4, proxy->ack_seq_no); // 2 landed, absorbed 3; 4 still missing
    EXPECT_TRUE(proxy->acknack_scheduled);

    tail = write_data(&node, 10, 1000, 10); // another new gap (9) while still armed
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, (uint32_t)test_mock_send_to_call_count);
    acknack = last_sent_acknack();
    EXPECT_TRUE(acknack != NULL);
    EXPECT_EQ_U32(4, acknack->seq_no);
    EXPECT_TRUE(acknack->bitmap[0] == (1ULL << 5)); // seq_no 9 = ack_seq_no 4 + 5; not 4 or 6
}

// When the scheduler will next run acknack_retry() for proxy, or 0 if it isn't scheduled.
static uint64_t scheduled_acknack_retry_time(const struct tt_Node* node, const struct tt_WriterProxy* proxy) {
    for (int32_t i = 0; i < node->scheduler_tail; i++) {
        if (node->scheduler[i].function == acknack_retry && node->scheduler[i].param == proxy) {
            return node->scheduler[i].time;
        }
    }
    return 0;
}

// Phase 1-b (rmw_tickle/PLAN.md, H3) - the ACKNACK retry timer runs on the reliable-specific
// tt_RELIABLE_RETRY_INTERVAL (1ms), not RPC's tt_CALL_RETRY_INTERVAL (5ms): both the first arming
// (maybe_arm_acknack_retry()) and every re-arm (acknack_retry()) use it.
static void test_reliable_acknack_retry_uses_reliable_retry_interval(void) {
    test_mock_reset();
    _Static_assert(tt_RELIABLE_DEADLINE == 0, "this test assumes no tt_RELIABLE_DEADLINE override");
    _Static_assert(tt_RELIABLE_RETRY_INTERVAL < tt_CALL_RETRY_INTERVAL, "reliable retry must be shorter than RPC's");
    test_mock_now = 10 * tt_MILLISECOND;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 3, 300, 3); // gap at 2 arms the retry
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_TRUE(proxy->acknack_scheduled);
    EXPECT_TRUE(scheduled_acknack_retry_time(&node, proxy) == test_mock_now + tt_RELIABLE_RETRY_INTERVAL);

    test_mock_now += tt_RELIABLE_RETRY_INTERVAL; // the timer fires; the gap is still open
    tt_Node_unschedule(&node, acknack_retry, proxy);
    int sends_before = test_mock_send_to_call_count;
    acknack_retry(&node, test_mock_now, proxy);
    EXPECT_EQ_U32((uint32_t)sends_before + 1, (uint32_t)test_mock_send_to_call_count); // re-sent
    EXPECT_TRUE(scheduled_acknack_retry_time(&node, proxy) == test_mock_now + tt_RELIABLE_RETRY_INTERVAL);
}

// Milestone 60 (rmw_tickle/PLAN.md) - receive-side de-duplication regression: TickLE Plan's own
// real HIL finding (history_depth_burst_loss/lifespan_expiry scenarios, recv > sent) traced to
// deliver_data_to_subscriber() invoking the application callback unconditionally, with no seq_no-
// based gate at all - a legitimate ACKNACK-driven retransmit racing the original delivery (or any
// other stray duplicate) used to double-deliver to the app. Real DDS readers de-duplicate by
// (writer GUID, sequence number) before ever notifying the listener. Deliberately narrow, only the
// one duplicate shape update_reliable_ack() can catch *safely* (an out-of-order sample arriving
// twice before the watermark reaches it, bit already set in received_bitmap) - a first version also
// treated any seq_no < ack_seq_no as a duplicate and reproduced a severe real regression (a 12.38x
// "reliable recv throughput" drop caught by real CI, see update_reliable_ack()'s own doc comment
// for the full root cause); a below-watermark arrival is deliberately left undeduplicated here,
// same accepted trade-off.
static void test_reliable_duplicate_delivery_is_not_re_delivered_to_callback(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1); // in order, first contact
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);

    // Ordered delivery (2026-09-24): seq_no 3 is ahead of the gap at 2, so it is HELD rather than
    // delivered. It used to go straight to the callback here, and the count used to reach 2. What
    // this test is for - a retransmit must not be delivered twice - is unchanged; only when the
    // first copy reaches the application has changed, which is the whole point of the feature.
    tail = write_data(&node, 3, 300, 3); // out of order, genuinely new - ahead of the gap at 2
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count); // held, not delivered

    tail = write_data(&node, 3, 300, 3); // seq_no 3 again - e.g. a retransmit racing the original
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count); // still just held once, not twice

    // The gap fills: 2 is delivered, and 3 follows it immediately, in order, out of the buffer.
    tail = write_data(&node, 2, 200, 2);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(1, sub.reorder_delivered); // exactly one sample came back out of the buffer

    // And it was delivered in order: seq_no 3 is what the application saw last, after 2.
    EXPECT_EQ_U32(3, sub.last_seq_no);
    EXPECT_EQ_U32(0, sub.out_of_order);

    // seq_no 1 again, well below the watermark (4). Before strict order this was delivered a second
    // time - the "accepted trade-off" in this test's doc comment, which was about ACK tracking and
    // still is. Delivery is now strictly ordered, so it is discarded as a step backwards, counted
    // rather than silent, and the watermark below shows ACK tracking was not touched by it.
    tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(1, sub.out_of_order_discarded);

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(4, proxy->ack_seq_no);
}

// Milestone 60, re-specified 2026-09-24 when RELIABLE became strictly ordered.
//
// Originally: samples from a range abandoned by jump_ack_baseline() must still be DELIVERED when
// they turn up late. That was the right contract while delivery was unordered, and it was pinned
// after a first attempt that broke ACK tracking and cost a 12.38x reliable throughput regression.
//
// The user has since ruled that RELIABLE is strictly ordered, and these are exactly the samples
// that ruling excludes: by the time they arrive, the application has been handed far_seq, and
// delivering 2 after it would be a step backwards. So they are now discarded - but two things from
// the original test still hold, and they are what this pins:
//
//   - not SILENTLY: every one is counted in out_of_order_discarded, so a reader seeing a gap can
//     tell a discarded late sample from one that never arrived;
//   - ACK tracking is untouched: the watermark stays where the jump put it. The 12.38x regression
//     came from treating below-watermark arrivals as already-received for ACK purposes, and
//     strict order is a delivery decision only - it must not bring that back.
static void test_reliable_late_arrivals_after_baseline_jump_are_discarded_not_silently(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1); // in order, first contact
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);

    // A far-ahead arrival triggers jump_ack_baseline() - abandons 2..(far_seq - 1).
    uint32_t far_seq = 2 + tt_RELIABLE_BITMAP_BITS;
    tail = write_data(&node, far_seq, far_seq * 100ULL, far_seq);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count); // the jump-triggering sample itself delivered

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(far_seq + 1, proxy->ack_seq_no);

    uint32_t late = 0;
    for (uint32_t seq = 2; seq < far_seq; seq += 7) {
        tail = write_data(&node, seq, seq * 100ULL, seq);
        EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
        late++;
    }
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count); // none handed up after far_seq
    EXPECT_EQ_U32(late, sub.out_of_order_discarded);       // every one counted, none silent
    EXPECT_EQ_U32(0, sub.out_of_order);                    // the application never saw a step back
    EXPECT_EQ_U32(far_seq + 1, proxy->ack_seq_no);         // ACK tracking untouched
}

// A sample held behind a gap that is then ABANDONED must be delivered BEFORE the sample that caused
// the jump, not after it. The jump sample is far above everything held, so strict order puts it
// last. Delivering it first - which the code did until 2026-09-24 - would have made every held
// sample a step backwards, and strict order would then have discarded good data because of a
// sequencing bug rather than because it was late.
static void test_reliable_held_samples_precede_the_jump_that_releases_them(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    // 5 and 6 are held behind the gap at 2.
    tail = write_data(&node, 5, 500, 5);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 6, 600, 6);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);

    // Far ahead: abandons the gap. 5, 6 and the jump sample must all be delivered, in that order.
    uint32_t far_seq = 2 + tt_RELIABLE_BITMAP_BITS;
    tail = write_data(&node, far_seq, far_seq * 100ULL, far_seq);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(4, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(far_seq, sub.last_seq_no);      // the jump sample went last
    EXPECT_EQ_U32(0, sub.out_of_order);           // and nothing went backwards
    EXPECT_EQ_U32(0, sub.out_of_order_discarded); // nor was anything good discarded
    EXPECT_EQ_U32(2, sub.reorder_delivered);
}

// Regression test for a real bug found via run_perf.sh's own tc/netem loss-injection scenarios:
// update_reliable_ack()'s exact-match branch used to advance ack_seq_no without also shifting
// received_bitmap, silently misaligning every bit still tracking a *different*, still-outstanding
// gap above it - by one position, permanently - the moment any earlier gap got filled by an exact
// watermark match while later out-of-order samples were already buffered. On real hardware this
// made ACKNACK requests silently point at already-received sequence numbers instead of the
// genuinely missing one, capping RELIABLE's own recovery rate far below what tt_MAX_RELIABLE_HISTORY
// and the retry budget should have allowed. This reproduces that exact shape: two out-of-order
// arrivals buffered ahead of a gap, then the gap's own exact-match arrival, then checks the
// bitmap still correctly tracks the *other* two samples afterwards - not a corrupted mix.
static void test_reliable_subscribe_bitmap_stays_aligned_after_partial_recovery(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(2, proxy->ack_seq_no); // in order so far

    tail = write_data(&node, 4, 400, 4); // seq_no 2, 3 both missing
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 5, 500, 5);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, proxy->ack_seq_no);                           // 2 still the watermark - neither 4 nor 5 fill it
    EXPECT_TRUE(bitmap_equals_u64(proxy->received_bitmap, 12ULL)); // bits 2,3: (ack_seq_no+2)=4, (ack_seq_no+3)=5

    tail = write_data(&node, 2, 200, 2); // fills the watermark itself via an exact match
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    // ack_seq_no advances to 3 (only 2 was confirmed - 3 is still missing, so it must NOT jump
    // any further); the bitmap must re-align to keep tracking 4 and 5 correctly relative to the
    // new watermark, not silently start claiming 5 and 6 are the ones already received.
    EXPECT_EQ_U32(3, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_equals_u64(proxy->received_bitmap, 6ULL)); // bits 1,2: (ack_seq_no+1)=4, (ack_seq_no+2)=5

    tail = write_data(&node, 3, 300, 3); // the last real gap closes
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(6, proxy->ack_seq_no); // 3 lands, then absorbs the already-buffered 4 and 5 too
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
}

// Phase 1-c (rmw_tickle/PLAN.md, B2) - acknack_retry()'s give-up no longer bulk-skips everything
// more than tt_MAX_RELIABLE_HISTORY behind the highest received sample (skip_unrecoverable_backlog(),
// removed): that was a compile-time guess at the remote Publisher's depth, and threw away samples a
// deeper cache still held. What's genuinely gone is now signalled by the Publisher's own eviction
// Heartbeat instead (see test_heartbeat.c). Giving up on ack_seq_no advances past it alone, and a
// sample received far ahead - past the old 64-deep guess - stays tracked, its gap still requested.
static void test_acknack_retry_give_up_does_not_bulk_skip(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO; // an ordinary writer whose announce has been seen
    EXPECT_EQ_U32(1, proxy->ack_seq_no);

    // seq_no 1 (ack_seq_no itself) never arrives; seq_no far_seq already did, out of order - past
    // the old tt_MAX_RELIABLE_HISTORY-deep guess, still inside the tracking window.
    const uint32_t far_seq = tt_MAX_RELIABLE_HISTORY + 6;
    _Static_assert(tt_MAX_RELIABLE_HISTORY + 6 < tt_RELIABLE_BITMAP_BITS, "far_seq must fit the tracking window");
    bitmap_set_bit(proxy->received_bitmap, far_seq - 1);
    proxy->sender_ip = TEST_SENDER_IP;
    proxy->sender_port = TEST_SENDER_PORT;
    proxy->acknack_scheduled = true;

    for (int i = 0; i < tt_RELIABLE_RETRY; i++) {
        acknack_retry(&node, tt_get_ns(), proxy);
        EXPECT_EQ_U32(1, proxy->ack_seq_no);
    }

    acknack_retry(&node, tt_get_ns(), proxy); // exceeds the cap -> give up on seq_no 1 only

    EXPECT_EQ_U32(2, proxy->ack_seq_no);                               // past seq_no 1 alone, no bulk skip
    EXPECT_TRUE(bitmap_test_bit(proxy->received_bitmap, far_seq - 2)); // far_seq still tracked
    EXPECT_TRUE(proxy->acknack_scheduled); // the rest of the gap (2..far_seq-1) gets its own retries
}

// acknack_retry() must keep re-sending up to tt_RELIABLE_RETRY times, then give up: skip past
// the unrecoverable hole (so an unrelated future gap can still be tracked) and stop rescheduling
// itself - mirrors test_client_call.c's own test_call_retry_exhausted_reports_timeout.
static void test_acknack_retry_exhausted_gives_up(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO; // an ordinary writer whose announce has been seen
    proxy->ack_seq_no = 5;
    bitmap_set_u64(proxy->received_bitmap,
                   2ULL); // seq_no 5 still missing, seq_no 6 already received (bit j: ack_seq_no + j)
    proxy->sender_ip = TEST_SENDER_IP;
    proxy->sender_port = TEST_SENDER_PORT;
    proxy->acknack_scheduled = true;

    for (int i = 0; i < tt_RELIABLE_RETRY; i++) {
        acknack_retry(&node, tt_get_ns(), proxy);
        EXPECT_TRUE(proxy->acknack_scheduled);
    }
    EXPECT_EQ_U32((uint32_t)tt_RELIABLE_RETRY, (uint32_t)proxy->retry);

    acknack_retry(&node, tt_get_ns(), proxy); // exceeds the cap -> give up

    EXPECT_TRUE(!proxy->acknack_scheduled);
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
    EXPECT_EQ_U32(7, proxy->ack_seq_no); // skipped past seq_no 5, absorbed the already-known 6 too
}

// Regression test for a second real bug found via run_perf.sh's own tc/netem loss-injection
// scenarios: reliable_retry used to be one counter shared across a whole "gap episode" (from the
// first out-of-order arrival until received_bitmap fully clears), so a *different*, still-
// outstanding gap inherited however many attempts an earlier one in the same episode had already
// used, instead of its own full tt_RELIABLE_RETRY budget. Two losses close enough together that a
// second gap was still open when the first resolved would then get written off far sooner than
// intended - measurably raising real-world loss_pct above what p^(tt_RELIABLE_RETRY + 1) predicts
// for isolated losses. Reproduces the exact shape: burn part of the budget on the *older* gap,
// let it resolve while a *different* one remains, and check the survivor still gets a full,
// independent budget.
static void test_acknack_retry_budget_resets_for_next_gap(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1); // in order
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(2, proxy->ack_seq_no);

    tail = write_data(&node, 4, 400, 4); // seq_no 2, 3 missing - opens the first gap
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_TRUE(proxy->acknack_scheduled);

    acknack_retry(&node, tt_get_ns(), proxy); // burn 2 of the 3 retries on seq_no 2's own gap
    acknack_retry(&node, tt_get_ns(), proxy);
    EXPECT_EQ_U32(2, (uint32_t)proxy->retry);

    tail = write_data(&node, 2, 200, 2); // seq_no 2 recovers - a *different* gap (seq_no 3) remains
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, proxy->ack_seq_no);                                      // now waiting on 3, not 2
    EXPECT_TRUE(!bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy))); // still a gap (3 missing, 4 already in)
    // Without the fix, this would read 2 (inherited from seq_no 2's own already-used attempts)
    // instead of a fresh budget for the new watermark.
    EXPECT_EQ_U32(0, (uint32_t)proxy->retry);

    // seq_no 3's own gap must get a full, independent tt_RELIABLE_RETRY budget.
    for (int i = 0; i < tt_RELIABLE_RETRY; i++) {
        acknack_retry(&node, tt_get_ns(), proxy);
        EXPECT_TRUE(proxy->acknack_scheduled); // not given up yet
    }
    acknack_retry(&node, tt_get_ns(), proxy); // exceeds the cap now
    EXPECT_TRUE(!proxy->acknack_scheduled);
}

// Regression test for PLAN.md's Milestone 20: a gap so wide it can't even be represented in
// received_bitmap (>= tt_RELIABLE_BITMAP_BITS, the wire format's own fixed-width ceiling) used
// to just warn and leave ack_seq_no frozen forever - every later arrival, however perfectly
// in-order from that point on, had the exact same too-wide offset relative to the still-stuck
// ack_seq_no, so a healthy stream never recovered once this happened once. Most likely to bite a
// brand-new Subscriber whose default ack_seq_no (1) starts arbitrarily far behind a Publisher
// that's already been running a while (QoS roadmap #4 DURABILITY's own backlog delivery is
// exactly this shape - see tests/test_durability_pubsub.c's own combined-QoS test), but this
// reproduces the underlying bug at the plain RELIABLE level, no durability involved at all.
static void test_reliable_subscribe_oversized_first_gap_jumps_baseline_instead_of_freezing(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    // ack_seq_no starts at 1 (init_subscriber_registered_on_node()'s own default) - first-ever
    // arrival is seq_no 1000, an offset (999) far past tt_RELIABLE_BITMAP_BITS.
    uint32_t tail = write_data(&node, 1000, 100000, 1000);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // Fixed: baseline jumps to just past this arrival instead of staying stuck at 1.
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(1001, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
    EXPECT_TRUE(!proxy->acknack_scheduled); // no phantom gap left armed for 1..999

    // The stream must now track normally from here - a small, genuinely resolvable gap right
    // after the jump must still be detected and ACKNACKed, proving ack_seq_no didn't just move,
    // it's actually live again (the exact failure mode being fixed: without it, *every* arrival,
    // including this one, would have hit the same oversized-offset branch and done nothing).
    test_mock_send_to_call_count = 0;
    tail = write_data(&node, 1002, 100200, 1002); // 1001 skipped
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1001, proxy->ack_seq_no);                       // still correctly waiting on 1001
    EXPECT_TRUE(bitmap_equals_u64(proxy->received_bitmap, 2ULL)); // bit 1 -> seq_no 1002 (1001 + 1) received early
    EXPECT_TRUE(proxy->acknack_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // a real ACKNACK for 1001
}

// Milestone 60 (rmw_tickle/PLAN.md) - RELIABLE+VOLATILE DDS-parity regression: first contact via
// DATA whose own seq_no is a *small* offset (< tt_RELIABLE_BITMAP_BITS) ahead of the stale
// ack_seq_no==1 default must NOT be misread as "everything before this is a recoverable in-flight
// gap" - that offset range used to fall into the ordinary bitmap-tracking branch instead of the
// oversized-gap jump above, ACKNACK-requesting a VOLATILE Publisher's own pre-match history it was
// never obligated to keep (TickLE Plan's own real finding: 57 samples received where DDS RELIABLE+
// VOLATILE would give 0). Real DDS readers have zero basis to assume anything existed before the
// very first sample they ever see for a writer, absent an explicit Heartbeat saying otherwise
// (inform_subscriber_of_heartbeat()'s own identical first-contact branch already got this right -
// this is the DATA-arrival path's equivalent, for whichever one wins the race to arrive first).
static void test_reliable_first_contact_via_data_does_not_request_pre_match_history(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    // First-ever arrival from this writer is seq_no 50 - a VOLATILE Publisher already 49 samples
    // into its own stream by the time this Subscriber matched it, well within tt_RELIABLE_BITMAP_
    // BITS of the stale ack_seq_no==1 default.
    uint32_t tail = write_data(&node, 50, 5000, 50);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(51, proxy->ack_seq_no); // synced straight to just past this first sample
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap,
                               proxy_words(proxy)));          // nothing "missing" before it - never tracked at all
    EXPECT_TRUE(!proxy->acknack_scheduled);                   // no phantom gap for 1..49
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // no ACKNACK ever sent requesting them

    // The stream must still track normally from here - a real, later gap must still be detected.
    tail = write_data(&node, 52, 5200, 52); // 51 skipped
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(51, proxy->ack_seq_no); // correctly still waiting on 51
    EXPECT_TRUE(proxy->acknack_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // a real ACKNACK for 51
}

// An incoming ACKNACK requesting a seq_no still in a reliable Publisher's cache must be
// retransmitted, unicast straight back to whoever sent the ACKNACK.
static void test_process_acknack_retransmits_cached_sample(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true; // Milestone 62 (rmw_tickle/PLAN.md) - retransmission is gated on this now,
                         // this test's own name/intent ("retransmits_cached_sample") needs it set

    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1

    test_mock_send_to_call_count = 0; // only count the retransmit below

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 1, 1ULL); // requesting seq_no 1 (bit 0)
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip);
    EXPECT_EQ_U32(1, (uint32_t)cache.index[0].retry);
}

// Milestone 62 (rmw_tickle/PLAN.md) - find_resendable_cache_entry()'s own direct-index math
// ((missing_seq_no - 1) % depth), pinned against the exact ring-wraparound shape that would expose
// a wrong-slot bug: with depth=4 and 6 samples published (seq_no 1..6), the ring has wrapped once
// - slot 0 now holds seq_no 5 (overwrote seq_no 1's own old slot), slot 1 holds seq_no 6 (overwrote
// seq_no 2's), slots 2/3 still hold their original seq_no 3/4 (never overwritten, only 6 total
// published). Confirms three distinct cases the old linear scan handled correctly by construction
// but a direct-index computation could get subtly wrong: an evicted seq_no whose own slot now holds
// a *different*, newer seq_no (must not false-match that newer entry), a wrapped-into slot 0, and
// an un-wrapped slot 2 - each checked by seeing exactly one entries[]'s own retry counter increment,
// confirming the correct physical slot was found, not merely that *some* retransmit happened.
static void test_process_acknack_direct_index_correct_after_wraparound(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;

    for (uint32_t i = 0; i < 6; i++) { // seq_no 1..6, ring wraps once past depth 4
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }
    EXPECT_EQ_U32(5, cache.index[0].seq_no); // slot 0: seq_no 1 evicted, now holds 5
    EXPECT_EQ_U32(6, cache.index[1].seq_no); // slot 1: seq_no 2 evicted, now holds 6
    EXPECT_EQ_U32(3, cache.index[2].seq_no); // slot 2: never overwritten
    EXPECT_EQ_U32(4, cache.index[3].seq_no); // slot 3: never overwritten

    struct tt_Header header;
    init_header(&header);

    // Evicted: seq_no 1's own slot (0) now genuinely holds seq_no 5, not seq_no 1 - must not
    // false-match and retransmit the wrong (newer) sample. Phase 1-c: the only thing sent back is
    // one eviction Heartbeat saying resendable history now starts at 3 (the oldest seq_no the
    // depth-4 ring still holds, 6 - 4 + 1).
    test_mock_send_to_call_count = 0;
    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 1, 1ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(0, (uint32_t)cache.index[0].retry); // untouched - correctly not matched
    const struct tt_HeartbeatHeader* heartbeat = last_sent_heartbeat();
    EXPECT_TRUE(heartbeat != NULL);
    EXPECT_EQ_U32(3, heartbeat->first_available_seq_no);
    EXPECT_EQ_U32(6, heartbeat->last_seq_no);
    EXPECT_TRUE(heartbeat->flags == tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip); // unicast back to the requester

    // Mixed: seq_no 2 (evicted) and 3 (retained) in one ACKNACK - 3 is retransmitted, and exactly
    // one eviction Heartbeat follows it (once per ACKNACK, not per gone bit).
    test_mock_send_to_call_count = 0;
    tail = write_acknack(&node, ENDPOINT_ID, 1, 0x6ULL); // bits 1, 2 = seq_no 2, 3
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)cache.index[2].retry); // seq_no 3 resent
    heartbeat = last_sent_heartbeat();
    EXPECT_TRUE(heartbeat != NULL);
    EXPECT_EQ_U32(3, heartbeat->first_available_seq_no);
    cache.index[2].retry = 0; // reset for the "un-wrapped slot 2" case below

    // Wrapped-into slot 0, genuinely retained: seq_no 5.
    test_mock_send_to_call_count = 0;
    tail = write_acknack(&node, ENDPOINT_ID, 5, 1ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)cache.index[0].retry); // found via slot 0, the right one
    EXPECT_EQ_U32(0, (uint32_t)cache.index[2].retry); // slot 2 untouched by this request

    // Un-wrapped slot 2, genuinely retained: seq_no 3.
    test_mock_send_to_call_count = 0;
    tail = write_acknack(&node, ENDPOINT_ID, 3, 1ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)cache.index[2].retry); // found via slot 2, the right one
}

// QoS roadmap #6 (LIFESPAN) - a cached sample past pub->lifespan_duration_ns must not be
// retransmitted even though it's still physically sitting in reliable_cache and the Subscriber's
// ACKNACK is otherwise perfectly valid - "as if it had never been sent" (tt_Publisher.lifespan_
// duration_ns's own doc comment, tickle.h).
static void test_process_acknack_skips_expired_sample(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true; // Milestone 62 - must be set so the "skipped" result below is genuinely
                         // caused by the lifespan-expiry check inside the retransmit loop, not by
                         // that whole loop being gated off before ever reaching it
    pub.lifespan_duration_ns = 1000;

    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1, timestamp 0

    test_mock_now = 1000; // exactly at the lifespan boundary - already expired (>=, not >)
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 1, 1ULL); // requesting seq_no 1 (bit 0)
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)cache.index[0].retry); // expired - no retransmit
    // Phase 1-c: just one eviction Heartbeat - nothing resendable remains, so resendable history
    // "starts" one past the newest published seq_no.
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    const struct tt_HeartbeatHeader* heartbeat = last_sent_heartbeat();
    EXPECT_TRUE(heartbeat != NULL);
    EXPECT_EQ_U32(2, heartbeat->first_available_seq_no);
    EXPECT_EQ_U32(1, heartbeat->last_seq_no);
}

// An ACKNACK for a best-effort Publisher (reliable_cache == NULL, today's default) must be a
// harmless no-op - not a crash, not a retransmit of anything.
static void test_process_acknack_ignored_for_besteffort_publisher(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 1, 1ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// Milestone 62 (rmw_tickle/PLAN.md) - a DURABLE-but-not-RELIABLE Publisher (TRANSIENT_LOCAL backlog
// offered, no ongoing loss-recovery guarantee) must ignore an ACKNACK too, even though Milestone
// 24's own unified cache means it still has a real, populated reliable_cache (deliver_durability_
// backlog()'s own one-shot push already served that peer through a completely different path, no
// ACKNACK involved) - the "reliable_cache == NULL" check alone (test above) doesn't catch this
// shape at all, since this Publisher's own cache is very much non-NULL and non-empty.
static void test_process_acknack_ignored_for_durable_only_publisher(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.durable = true; // deliberately NOT pub.reliable = true

    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1, cached

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 1, 1ULL); // requesting the one real, cached sample
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // not answered - RELIABILITY's own contract, not offered
}

// --- Phase 2: the wider tracking window itself ---

// A gap wider than the default 256-sample window is normally abandoned outright
// (jump_ack_baseline()) - that abandonment is what the intermittent max-rate loss comes from. With
// a caller-provided wider window the same gap stays tracked and is still requestable.
static void test_wide_gap_tracked_with_caller_sized_window(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    // 1024-sample window: 16 words per writer, one window per tracked writer.
    static uint64_t tracking[tt_MAX_PEER_COUNT * 16];
    memset(tracking, 0, sizeof(tracking));
    sub.tracking_bitmaps = tracking;
    sub.tracking_words = 16;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // seq_no 800 is 798 ahead of the still-missing 2: past the 256 default, inside 1024.
    tail = write_data(&node, 800, 80000, 800);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(2, proxy->ack_seq_no); // still waiting on 2 - not jumped past it
    EXPECT_TRUE(bitmap_test_bit(proxy->received_bitmap, 798));
    EXPECT_TRUE(proxy->acknack_scheduled);

    // The ACKNACK it sent must name the gap across several words, and say how many it carries.
    const struct tt_AckNackHeader* acknack = last_sent_acknack();
    EXPECT_TRUE(acknack != NULL);
    EXPECT_EQ_U32(2, acknack->seq_no);
    EXPECT_TRUE(acknack->bitmap_words >= 13); // bit 798 lives in word 12
    EXPECT_TRUE(acknack->bitmap_words <= 16);
    EXPECT_TRUE((acknack->bitmap[0] & 1ULL) != 0); // seq_no 2 requested

    // ...and the very same gap is abandoned with the default window, which is what this fixes. A
    // separate node/subscriber pair, so nothing above carries over.
    test_mock_reset();
    struct tt_Node narrow_node;
    struct tt_Topic narrow_topic;
    struct tt_Subscriber narrow;
    init_node_and_topic(&narrow_node, &narrow_topic);
    init_subscriber_registered_on_node(&narrow, &narrow_node, &narrow_topic);
    struct tt_Header narrow_header;
    init_header(&narrow_header);

    tail = write_data(&narrow_node, 1, 100, 1);
    EXPECT_TRUE(
        process_data(&narrow_node, &narrow_header, narrow_node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&narrow_node, 800, 80000, 800);
    EXPECT_TRUE(
        process_data(&narrow_node, &narrow_header, narrow_node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* narrow_proxy = remote_writer_proxy(&narrow);
    EXPECT_TRUE(narrow_proxy != NULL);
    EXPECT_EQ_U32(801, narrow_proxy->ack_seq_no); // jumped past the gap outright
}

// A caller asking for more than the build's own maximum is clamped, not trusted - the window it
// gets is the maximum, and nothing indexes past the buffer it actually provided.
static void test_oversized_window_request_is_clamped(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    static uint64_t tracking[tt_MAX_PEER_COUNT * tt_RELIABLE_BITMAP_MAX_WORDS];
    memset(tracking, 0, sizeof(tracking));
    sub.tracking_bitmaps = tracking;
    sub.tracking_words = tt_RELIABLE_BITMAP_MAX_WORDS * 4; // four times what this build allows

    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_MAX_WORDS, (uint32_t)subscriber_tracking_words(&sub));

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_MAX_BITS, proxy_window_bits(proxy));
}

// --- Phase 2 (rmw_tickle/PLAN.md): per-Subscriber ack identity and the variable-length bitmap ---

// Writes an ACKNACK naming a specific sending Subscriber entity and `words` bitmap words.
static uint32_t write_acknack_from(struct tt_Node* node, uint32_t endpoint_id, uint32_t sender_entity_id,
                                   uint32_t seq_no, const uint64_t* words, uint16_t word_count) {
    struct tt_AckNackHeader* acknack_header = (struct tt_AckNackHeader*)node->rx_buffer;
    memset(acknack_header, 0, sizeof(*acknack_header));
    acknack_header->endpoint_id = endpoint_id;
    acknack_header->sender_entity_id = sender_entity_id;
    acknack_header->seq_no = seq_no;
    acknack_header->bitmap_words = word_count;
    for (uint16_t i = 0; i < word_count; i++) {
        acknack_header->bitmap[i] = words[i];
    }
    return (uint32_t)(sizeof(struct tt_AckNackHeader) + ((size_t)word_count * sizeof(uint64_t)));
}

// The KEEP_ALL hazard prerequisite (b) exists for: two Subscribers of one topic on one remote node
// each get their own ack entry, so the faster one's ACKNACK must not make the Publisher believe
// both have caught up.
static void test_two_subscriber_entities_on_one_node_ack_independently(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;

    const uint32_t fast_entity = REMOTE_SUB_ENTITY_ID;
    const uint32_t slow_entity = REMOTE_SUB_ENTITY_ID + 1;
    EXPECT_TRUE(claim_peer_ack(&pub, REMOTE_NODE_ID, fast_entity) != NULL);
    EXPECT_TRUE(claim_peer_ack(&pub, REMOTE_NODE_ID, slow_entity) != NULL);

    struct tt_Header header;
    init_header(&header);

    // The fast Subscriber acks everything below 10; the slow one has said nothing yet.
    uint32_t tail = write_acknack_from(&node, ENDPOINT_ID, fast_entity, 10, NULL, 0);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_TRUE(!tt_Publisher_is_acked_by_all_peers(&pub, 5)); // the slow one still hasn't
    EXPECT_EQ_U32(0, tt_Publisher_min_acked_seq_no(&pub));

    // The slow one catches up only as far as 7.
    tail = write_acknack_from(&node, ENDPOINT_ID, slow_entity, 7, NULL, 0);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(7, tt_Publisher_min_acked_seq_no(&pub)); // the minimum, not the fast one's 10
    EXPECT_TRUE(tt_Publisher_is_acked_by_all_peers(&pub, 6));
    EXPECT_TRUE(!tt_Publisher_is_acked_by_all_peers(&pub, 7)); // 7 itself isn't acked by the slow one
}

// An ACKNACK whose sender this Publisher never matched (or with no sender id at all) still routes
// and retransmits, but must not be counted as anyone's ack - the conservative direction: counting
// it would let a KEEP_ALL writer unblock on a Subscriber it isn't actually tracking.
static void test_process_acknack_from_unmatched_entity_records_nothing(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    EXPECT_TRUE(claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) != NULL);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack_from(&node, ENDPOINT_ID, REMOTE_SUB_ENTITY_ID + 99, 10, NULL, 0);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_acknack_from(&node, ENDPOINT_ID, 0, 10, NULL, 0); // no sender id at all
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    EXPECT_EQ_U32(0, ack->ack_seq_no); // the tracked Subscriber still hasn't acked anything
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + 99) == NULL);
}

// The ack table filling up must refuse the match outright rather than matching a Subscriber whose
// acks could never be counted (which would unblock a KEEP_ALL writer early).
static void test_ack_table_full_refuses_further_entities(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        EXPECT_TRUE(claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + (uint32_t)i) != NULL);
    }
    EXPECT_TRUE(claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + tt_MAX_ACK_ENTRIES) == NULL);

    // Freeing one entity's entry (its own lease expiring, say) makes room again - and only that one.
    forget_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + 3, /*match_any_entity=*/false);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + 3) == NULL);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + 4) != NULL);
    EXPECT_TRUE(claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + tt_MAX_ACK_ENTRIES) != NULL);
}

// A malformed bitmap_words must be rejected, not clamped and half-processed: the request would name
// sequence numbers the sender never meant. Both bounds are checked - this receiver's own capacity,
// and the bytes the datagram actually carries.
static void test_acknack_rejects_malformed_bitmap_words(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.peers[0].node_id = REMOTE_NODE_ID;

    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);
    uint64_t one_word[1] = {1ULL};

    // Absurd count, far past any window this build could have.
    uint32_t tail = write_acknack_from(&node, ENDPOINT_ID, REMOTE_SUB_ENTITY_ID, 1, one_word, 1);
    ((struct tt_AckNackHeader*)node.rx_buffer)->bitmap_words = 60000;
    EXPECT_TRUE(!process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // In range for this build, but more words than the datagram actually carries.
    tail = write_acknack_from(&node, ENDPOINT_ID, REMOTE_SUB_ENTITY_ID, 1, one_word, 1);
    ((struct tt_AckNackHeader*)node.rx_buffer)->bitmap_words = tt_RELIABLE_BITMAP_MAX_WORDS;
    EXPECT_TRUE(!process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // nothing retransmitted from either

    // ...and a well-formed one still works, so the checks aren't simply rejecting everything.
    tail = write_acknack_from(&node, ENDPOINT_ID, REMOTE_SUB_ENTITY_ID, 1, one_word, 1);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
}

// A zero-word ACKNACK is a pure cumulative ack: nothing to retransmit, but the ack still counts.
static void test_acknack_with_no_bitmap_words_is_a_pure_ack(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    EXPECT_TRUE(claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) != NULL);

    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_acknack_from(&node, ENDPOINT_ID, REMOTE_SUB_ENTITY_ID, 2, NULL, 0);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // nothing requested
    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    EXPECT_EQ_U32(2, ack->ack_seq_no); // ...but the ack landed
}

// A request spanning several words retransmits every named sample, wherever it sits in the bitmap -
// the whole point of letting the window grow past one word.
static void test_acknack_multi_word_bitmap_retransmits_each_named_sample(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 200);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;

    for (uint32_t i = 0; i < 200; i++) { // seq_no 1..200, all retained
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);
    // From seq_no 1: bit 0 (seq 1), bit 64 (seq 65) and bit 130 (seq 131) - three words.
    uint64_t words[3] = {1ULL, 1ULL, 1ULL << 2};
    uint32_t tail = write_acknack_from(&node, ENDPOINT_ID, REMOTE_SUB_ENTITY_ID, 1, words, 3);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(3, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)cache.index[0].retry);   // seq_no 1
    EXPECT_EQ_U32(1, (uint32_t)cache.index[64].retry);  // seq_no 65
    EXPECT_EQ_U32(1, (uint32_t)cache.index[130].retry); // seq_no 131
    EXPECT_EQ_U32(0, (uint32_t)cache.index[1].retry);   // an unnamed one stays untouched
}

// QoS roadmap #5 (RELIABILITY) follow-up - tt_Publisher_wait_for_all_acked(). An ACKNACK from a
// peer already present in pub->peers[] must advance that peer's own peer_acks[] entry to the
// ACKNACK's own cumulative seq_no - the aggregation tt_Publisher_wait_for_all_acked() is built on.
static void test_process_acknack_updates_peer_ack_seq_no(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;

    struct tt_Header header;
    init_header(&header);

    // Phase 2 - ack entries are claimed when the Subscriber matches (its announce), which this
    // test skips by wiring peers[] directly; claim it explicitly so the ACKNACK has somewhere to
    // land. An ACKNACK from an entity that was never matched is deliberately not counted - see
    // test_process_acknack_from_unmatched_entity_records_nothing() below.
    EXPECT_TRUE(claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) != NULL);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 5, 0ULL); // seq_no 5, no gap requested
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    EXPECT_EQ_U32(5, ack->ack_seq_no);
    // ...and the public aggregation agrees: 4 is acked (5 means "everything below 5"), 5 isn't.
    EXPECT_TRUE(tt_Publisher_is_acked_by_all_peers(&pub, 4));
    EXPECT_TRUE(!tt_Publisher_is_acked_by_all_peers(&pub, 5));
}

// A stale/reordered ACKNACK (UDP gives no ordering guarantee) carrying a seq_no lower than what's
// already recorded must never regress a peer's ack entry - only ever advance it.
static void test_process_acknack_does_not_regress_peer_ack_seq_no(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;
    claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    record_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID, 10);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 3, 0ULL); // stale - already at 10
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    EXPECT_EQ_U32(10, ack->ack_seq_no);
}

// An ACKNACK from a sender not currently in pub->peers[] at all (never matched, or already
// forgotten) has no peer_ack_seq_no[] slot to update - must be a harmless no-op for that part,
// not a crash - retransmission itself (keyed off the wire seq_no directly) is unaffected.
static void test_process_acknack_from_unmatched_peer_updates_nothing(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache; // pub.peers[] left entirely empty

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 5, 0ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) ==
                NULL); // nothing recorded for an unmatched node
}

// forget_publisher_peer() must drop a genuinely departed peer's ack state alongside its peers[]
// slot (preserve_ack = false), so a later, unrelated node_id can never inherit an ack value that
// was never about it. Phase 3 prerequisite (c) added the other half - see
// test_publisher_peer_ack_survives_announce_refresh() below for the preserve_ack = true case.
static void test_forget_publisher_peer_resets_ack_seq_no(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;
    claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    record_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID, 42);

    forget_publisher_peer(&pub, REMOTE_NODE_ID, /*preserve_ack=*/false);

    EXPECT_EQ_INT((int)tt_NODE_ID_INVALID, (int)pub.peers[0].node_id);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) == NULL);

    // preserve_ack = true keeps it: process_update()'s own forget-then-re-add must not throw away
    // ack state for a Subscriber that never went anywhere (Phase 3 prerequisite (c)).
    pub.peers[0].node_id = REMOTE_NODE_ID;
    claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    record_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID, 42);
    forget_publisher_peer(&pub, REMOTE_NODE_ID, /*preserve_ack=*/true);
    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    EXPECT_EQ_U32(42, ack->ack_seq_no);
}

#ifdef tt_RELIABLE_STATS
// experiment/reliable-recovery-instrumentation - pins the subscriber-side counters against a
// hand-traced sequence: 1, 3 (gap at 2: immediate ACKNACK), 5 (gap at 4, opened while the retry
// timer from the first gap is still armed - H2; Phase 1-a now NACKs it immediately too, naming only
// seq_no 4), then 2 and 4 arrive.
static void test_reliable_stats_subscriber_gap_accounting(void) {
    test_mock_reset();
    tt_reliable_stats_reset();
    test_mock_now = 1000 * tt_MICROSECOND; // non-zero: a 0 timestamp means "unset" to the stats

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    const uint32_t order[] = {1, 3, 5, 2, 4};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        test_mock_now += 100 * tt_MICROSECOND;
        uint32_t tail = write_data(&node, order[i], (uint64_t)order[i] * 100, order[i]);
        EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    }

    struct tt_ReliableStats stats;
    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(2, (uint32_t)stats.gaps_opened);
    EXPECT_EQ_U32(2, (uint32_t)stats.missing_opened);
    EXPECT_EQ_U32(1, (uint32_t)stats.gaps_opened_while_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)stats.acknack_immediate);
    EXPECT_EQ_U32(0, (uint32_t)stats.acknack_timer);
    EXPECT_EQ_U32(1, (uint32_t)stats.acknack_new_gap); // Phase 1-a, for the gap at 4
    EXPECT_EQ_U32(2, (uint32_t)stats.acknack_sent);
    EXPECT_EQ_U32(2, (uint32_t)stats.acknack_bits_sent); // seq_no 2, then seq_no 4 - each named once
    EXPECT_EQ_U32(2, (uint32_t)stats.recovered);
    EXPECT_EQ_U32(2, (uint32_t)stats.recovered_after_request);
    EXPECT_EQ_U32(0, (uint32_t)stats.jump_data);
    EXPECT_EQ_U32(0, (uint32_t)stats.duplicates);

    uint64_t detect_total = 0;
    uint64_t request_total = 0;
    for (int i = 0; i < tt_RELIABLE_STATS_HIST_BUCKETS; i++) {
        detect_total += stats.detect_to_recover_hist[i];
        request_total += stats.request_to_recover_hist[i];
    }
    EXPECT_EQ_U32(2, (uint32_t)detect_total);
    EXPECT_EQ_U32(2, (uint32_t)request_total);
    // seq_no 2: requested at 1200us, recovered at 1400us; seq_no 4: requested at 1300us, recovered
    // at 1500us - both 200us, bucket [128, 256)us = index 8
    EXPECT_EQ_U32(2, (uint32_t)stats.request_to_recover_hist[8]);
}

// Publisher side: one ACKNACK naming a cached seq_no (retransmitted) and one never published at
// all (not in its slot - counted as evicted).
static void test_reliable_stats_publisher_retransmit_accounting(void) {
    test_mock_reset();
    tt_reliable_stats_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;

    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 1, 0x3ULL); // requesting seq_no 1 and 2
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_ReliableStats stats;
    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(1, (uint32_t)stats.acknack_received);
    EXPECT_EQ_U32(2, (uint32_t)stats.bits_requested);
    EXPECT_EQ_U32(1, (uint32_t)stats.retransmitted);
    EXPECT_EQ_U32(1, (uint32_t)stats.null_evicted);
    EXPECT_EQ_U32(0, (uint32_t)stats.null_retry_cap);
    EXPECT_EQ_U32(1, (uint32_t)stats.eviction_heartbeats); // Phase 1-c, for seq_no 2
    EXPECT_TRUE(stats.datagrams_with_data >= 1);           // at least the retransmit itself
    EXPECT_TRUE(stats.max_data_per_datagram >= 1);
}
#endif

// --- Phase 3 step 2 (rmw_tickle/PLAN.md): KEEP_ALL write blocking ------------------------------

// Sets up a KEEP_ALL Publisher with one matched Subscriber entity that has acked nothing yet.
static void init_keep_all_publisher(struct tt_Node* node, struct tt_Topic* topic, struct tt_Publisher* pub,
                                    struct tt_ReliableCache* cache, uint16_t window_words) {
    init_node_and_topic(node, topic);
    init_publisher(pub, node, topic);
    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)pub;
    pub->reliable_cache = cache;
    pub->reliable = true;
    pub->keep_all = true;
    pub->peers[0].node_id = REMOTE_NODE_ID;
    pub->peers[0].ip = TEST_SENDER_IP;
    pub->peers[0].port = TEST_SENDER_PORT;
    struct tt_PeerAck* ack = claim_peer_ack(pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    ack->tracking_words = window_words; // what that Subscriber announced it can track
}

// A KEEP_ALL Publisher accepts exactly `bound` unacknowledged samples and then refuses, without
// sending or caching the refused one - and an ACK unblocks it again.
static void test_keep_all_refuses_at_bound_and_unblocks_on_ack(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 4);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 16); // window 1024 >> depth 4, so depth binds

    uint32_t value = 1;
    for (int i = 0; i < 4; i++) { // fills the depth-4 cache with unacknowledged samples
        EXPECT_TRUE(tt_Publisher_writable(&pub));
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }

    EXPECT_TRUE(!tt_Publisher_writable(&pub));
    int sends_before = test_mock_send_call_count + test_mock_send_to_call_count;
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    // Phase 3 step 4 - a refusal does send something now, but never the sample: it solicits an ACK
    // (see solicit_ack_throttled()'s own call site), which is what bounds how long the stall can
    // last when the watermark's own solicitation or its answer was lost. The sample itself is
    // still neither sent nor retained, which is what these three check.
    EXPECT_TRUE(test_mock_send_call_count + test_mock_send_to_call_count > sends_before);
    EXPECT_EQ_U32(4, pub.seq_no);               // seq_no didn't advance
    EXPECT_EQ_U32(4, cache.newest_seq_no);      // ...and nothing was cached
    EXPECT_TRUE(last_sent_heartbeat() != NULL); // ...the extra traffic is a solicitation

    // The Subscriber acks everything below 3, freeing two slots.
    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 3, 0ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_TRUE(tt_Publisher_writable(&pub));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    EXPECT_EQ_U32(5, pub.seq_no);
}

// KEEP_LAST (the default) never refuses: it evicts, exactly as before this feature existed.
static void test_keep_last_still_evicts_rather_than_refusing(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 4);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 16);
    pub.keep_all = false;

    uint32_t value = 1;
    for (int i = 0; i < 10; i++) { // well past the depth, nothing acked
        EXPECT_TRUE(tt_Publisher_writable(&pub));
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_EQ_U32(10, pub.seq_no);
    EXPECT_EQ_U32(7, cache.oldest_seq_no); // the oldest 6 were evicted, as KEEP_LAST does
}

// The bound is the SMALLER of the Publisher's depth and the narrowest announced Subscriber window:
// a Subscriber that can only track 128 samples makes a deeper unacknowledged run pointless.
static void test_keep_all_bound_follows_smallest_window(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 200);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 2); // 2 words = 128 samples, below depth 200

    EXPECT_EQ_U32(128, tt_Publisher_unacked_bound(&pub));

    uint32_t value = 1;
    for (int i = 0; i < 128; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_TRUE(!tt_Publisher_writable(&pub)); // blocked by the window, not the depth
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    // A second Subscriber with an even narrower window lowers the bound further.
    struct tt_PeerAck* second = claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + 1);
    EXPECT_TRUE(second != NULL);
    second->tracking_words = 1; // 64 samples
    EXPECT_EQ_U32(64, tt_Publisher_unacked_bound(&pub));
}

// The other direction, which every case above misses by only ever using windows *narrower* than
// tt_RELIABLE_BITMAP_BITS: a Subscriber announcing a window wider than the protocol default must
// raise the bound, not be silently clamped to it.
//
// Regression - this was wrong (the bound started at tt_RELIABLE_BITMAP_BITS and could only be
// lowered), and no core test caught it because they all used narrow windows. rmw_tickle announces
// RMW_TICKLE_TRACKING_WORDS = 16 words = 1024 samples on every subscription, so in the one
// configuration that actually ships, a KEEP_ALL Publisher blocked after 256 unacknowledged samples
// instead of 1024 - a quarter of the in-flight depth its peers could really recover from. Found by
// rmw_tickle's own test_keep_all_blocking.c counting the writes it got before the refusal.
static void test_keep_all_bound_honors_wide_window(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 2048);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 16); // 16 words = 1024, above the 256 default

    EXPECT_EQ_U32(1024, tt_Publisher_unacked_bound(&pub));

    // ...and the wider bound is what publishing actually gets: 1024 accepted, the 1025th refused.
    uint32_t value = 1;
    for (int i = 0; i < 1024; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_TRUE(!tt_Publisher_writable(&pub));
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    // A matched Subscriber that announced no window at all doesn't drag the bound down to zero -
    // it just doesn't participate, leaving the wide one standing.
    struct tt_PeerAck* silent = claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID + 1);
    EXPECT_TRUE(silent != NULL);
    silent->tracking_words = 0;
    EXPECT_EQ_U32(1024, tt_Publisher_unacked_bound(&pub));

    // With nothing matched, the protocol default is still the answer.
    forget_peer_ack(&pub, REMOTE_NODE_ID, 0, /*match_any_entity=*/true);
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_BITS, tt_Publisher_unacked_bound(&pub));
}

// The regression the Phase 3 step 4 HIL smoke test found: on a *lossless* link a KEEP_ALL
// Publisher used to publish exactly its bound and then stall forever. A Subscriber only ACKNACKs
// when it sees a gap, so with nothing lost there was nothing to advance the acknowledgement, and
// keep_all_writable() never became true again. Measured at 0.125 Mbps against 109 Mbps once
// acknowledgements flowed - not slow, stopped. Zero loss is the worst case here, which is why it
// took a clean-link smoke test to find.
//
// Asserted as "far past the bound" rather than any timing bound: what matters is that the stream
// keeps going at all, and a count is stable under whatever a CI runner is doing at the time.
static void test_keep_all_sustains_on_clean_link(void) {
    test_mock_reset();
    test_mock_now = 10 * tt_MILLISECOND; // see test_keep_all_solicitation_is_throttled() on why not 0

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 64);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 1); // 1 word = 64 samples, so bound = 64

    // Models a real Subscriber on a lossless link: it answers a solicitation, and does nothing
    // otherwise. That is the whole point - acking unprompted here would test nothing, because the
    // bug is precisely that nothing prompts it. So the only ACKNACK this loop ever sends is one in
    // reply to a Heartbeat the Publisher itself chose to send.
    const uint32_t bound = 64;
    const uint32_t target = bound * 8;
    uint32_t value = 1;
    uint32_t published = 0;
    struct tt_Header header;
    init_header(&header);

    for (uint32_t i = 0; i < target; i++) {
        test_mock_send_last_len = 0; // so a Heartbeat seen below is one *this* publish sent
        if (tt_Publisher_publish(&pub, (struct tt_Data*)&value) == tt_RET_OK) {
            published++;
        }
        if (last_sent_heartbeat() == NULL) {
            continue; // nothing asked us for anything
        }
        // Asked: acknowledge everything published so far, exactly as a Subscriber that has missed
        // nothing would. Then let the loop carry on - no retry, no second chance.
        uint32_t tail = write_acknack(&node, ENDPOINT_ID, pub.seq_no + 1, 0ULL);
        EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
        test_mock_now += tt_RELIABLE_RETRY_INTERVAL; // let the solicitation throttle reopen
    }

    // Before the fix this stopped at 64 no matter how long the loop ran.
    EXPECT_TRUE(published > bound * 4);
}

// The watermark fires on its own, without ack_solicit_watermark_pct being set: keep_all implies it
// (ack_solicit_threshold()), because a KEEP_ALL Publisher that never asks cannot recover.
static void test_keep_all_solicits_before_blocking(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 64);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 1); // bound = 64, so the watermark is 32
    EXPECT_EQ_U32(0, pub.ack_solicit_watermark_pct);         // nothing opted in - keep_all is the whole reason

    uint32_t value = 1;
    for (int i = 0; i < 31; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_TRUE(last_sent_heartbeat() == NULL); // still below half the bound - nothing asked yet

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // 32nd
    EXPECT_TRUE(last_sent_heartbeat() != NULL); // ...asks here, with the bound still 32 samples away
}

// ...and the throttle still bounds it. Every publish past the watermark would otherwise solicit,
// putting an extra Heartbeat on the wire per sample - at max rate that is a real cost, which is
// what the shared min-gap throttle exists to prevent.
//
// Self-calibrating rather than pinning a send count: how many sends one publish costs is an
// implementation detail (broadcast plus per-peer unicast, batching), so this compares two stretches
// of equal length, one below the watermark and one entirely past it. If the throttle works they
// cost the same; if it doesn't, the second carries a solicitation per sample.
static void test_keep_all_solicitation_is_throttled(void) {
    test_mock_reset();
    // A clock that isn't zero. solicit_ack_throttled() records last_ack_solicit_ns and treats 0 as
    // "never solicited yet", so at the mock's own default t=0 that stamp reads as unset again and
    // nothing would ever throttle - an artefact of the test clock, not of the throttle. A real
    // tt_get_ns() is monotonic-since-boot and past this within a millisecond of starting.
    test_mock_now = 10 * tt_MILLISECOND;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 64);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 1); // bound = 64, watermark 32

    const int stretch = 16;
    uint32_t value = 1;

    int before_quiet = test_mock_send_call_count + test_mock_send_to_call_count;
    for (int i = 0; i < stretch; i++) { // 1..16, well below the watermark - no solicitation at all
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    int quiet_sends = (test_mock_send_call_count + test_mock_send_to_call_count) - before_quiet;

    for (int i = 0; i < stretch; i++) { // 17..32, crossing the watermark - one solicitation here
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_TRUE(last_sent_heartbeat() != NULL); // it really did ask

    int before_throttled = test_mock_send_call_count + test_mock_send_to_call_count;
    for (int i = 0; i < stretch; i++) { // 33..48, every one past the watermark, clock unmoved
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    int throttled_sends = (test_mock_send_call_count + test_mock_send_to_call_count) - before_throttled;

    EXPECT_EQ_INT(quiet_sends, throttled_sends); // past the watermark costs exactly what below it did
}

// Lays one Heartbeat into node->rx_buffer, same shape as tests/test_heartbeat.c's own - duplicated
// rather than shared because each of these files is a standalone whitebox translation unit.
static uint32_t write_heartbeat(struct tt_Node* node, uint32_t endpoint_id, uint32_t first_available_seq_no,
                                uint32_t last_seq_no, uint8_t flags) {
    struct tt_HeartbeatHeader* heartbeat_header = (struct tt_HeartbeatHeader*)node->rx_buffer;
    heartbeat_header->endpoint_id = endpoint_id;
    heartbeat_header->first_available_seq_no = first_available_seq_no;
    heartbeat_header->last_seq_no = last_seq_no;
    heartbeat_header->entity_id = 0; // explicit: offset 12 overlaps a prior write_data()'s timestamp
    heartbeat_header->flags = flags;
    heartbeat_header->reserved[0] = 0;
    heartbeat_header->reserved[1] = 0;
    heartbeat_header->reserved[2] = 0;
    return sizeof(struct tt_HeartbeatHeader);
}

// Phase 3 step 4 - the safety net the "unknown policy never gives up" default depends on, and the
// reason it can't wedge: a writer whose policy this Subscriber hasn't learned yet is never
// abandoned by the retry cap, but if that writer genuinely no longer holds the sample it says so,
// and the Subscriber moves on. Without this, refusing to guess would trade silent data loss for an
// endless retry, which is not obviously the better bargain - so it is asserted, not assumed.
static void test_unknown_policy_still_terminates_on_eviction(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32((uint32_t)tt_WRITER_KEEP_ALL_UNKNOWN, (uint32_t)proxy->keep_all); // claimed from DATA alone
    proxy->ack_seq_no = 5;
    proxy->sender_ip = TEST_SENDER_IP;
    proxy->sender_port = TEST_SENDER_PORT;
    bitmap_set_bit(proxy->received_bitmap, 2); // seq_no 7 arrived; 5 and 6 are missing
    proxy->acknack_scheduled = true;

    // Retries far past the bounded budget without ever advancing - which is the intended behavior
    // here, and on its own would be an unbounded wait.
    for (int i = 0; i < tt_RELIABLE_RETRY * 5; i++) {
        acknack_retry(&node, tt_get_ns(), proxy);
    }
    EXPECT_EQ_U32(5, proxy->ack_seq_no);   // never abandoned on a guess
    EXPECT_TRUE(proxy->acknack_scheduled); // ...and still asking

    // Now the writer answers the way a KEEP_LAST writer that has evicted those samples does: its
    // oldest retained sample is 7 (Phase 1-c's eviction Heartbeat). 5 and 6 are genuinely gone.
    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_heartbeat(&node, ENDPOINT_ID, 7, 7, tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // Skipped exactly the range the writer said was unavailable, and stopped: the gap ended without
    // the retry cap ever being what ended it.
    EXPECT_EQ_U32(8, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));
}

#ifdef tt_RELIABLE_STATS
// Phase 3 step 4 follow-up - a KEEP_ALL Publisher becomes writable again for two very different
// reasons, and writable_no_peers is what tells them apart. Acks arriving is KEEP_ALL working; the
// last matched Subscriber disappearing is KEEP_ALL ceasing to apply, since there is then nobody to
// hold a sample for. Both are correct and both look identical from outside without this counter.
static void test_keep_all_writable_cause_is_distinguished(void) {
    // (1) Writable because the Subscriber acknowledged - the counter must NOT move.
    test_mock_reset();
    tt_reliable_stats_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 4);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 16); // window 1024 >> depth 4, so depth binds

    uint32_t value = 1;
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 5, 0ULL); // everything below 5 received
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_ReliableStats stats;
    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(1, (uint32_t)stats.writable_callbacks);
    EXPECT_EQ_U32(0, (uint32_t)stats.writable_no_peers); // acks arrived - the guarantee held

    // (2) Writable because the last matched Subscriber went away - the counter must move, and
    // publishing must resume, because there is genuinely nobody left to retain anything for.
    test_mock_reset();
    tt_reliable_stats_reset();

    struct tt_Node node2;
    struct tt_Topic topic2;
    struct tt_Publisher pub2;
    TEST_RELIABLE_CACHE(cache2, 4);
    init_keep_all_publisher(&node2, &topic2, &pub2, &cache2, 16);

    for (int i = 0; i < 4; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub2, (struct tt_Data*)&value));
    }
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_publish(&pub2, (struct tt_Data*)&value));

    forget_peer_ack(&pub2, REMOTE_NODE_ID, 0, /*match_any_entity=*/true);
    notify_writable_if_pending(&pub2);

    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(1, (uint32_t)stats.writable_callbacks);
    EXPECT_EQ_U32(1, (uint32_t)stats.writable_no_peers); // ...this is the one worth noticing
    EXPECT_TRUE(tt_Publisher_writable(&pub2));
}
#endif

// The writable callback fires once on the refusal-to-writable transition, on the node's own thread
// (here: from inside process_acknack()), and not once per ACKNACK.
static int writable_callback_count = 0;
static struct tt_Publisher* writable_callback_pub = NULL;
static void count_writable(struct tt_Publisher* pub, void* param) {
    writable_callback_count++;
    writable_callback_pub = pub;
    *(int*)param += 1;
}

static void test_keep_all_writable_callback_fires_once(void) {
    test_mock_reset();
    writable_callback_count = 0;
    writable_callback_pub = NULL;
    int param_hits = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 4);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 16);
    pub.writable_callback = count_writable;
    pub.writable_callback_param = &param_hits;

    struct tt_Header header;
    init_header(&header);
    uint32_t value = 1;
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    EXPECT_EQ_INT(0, writable_callback_count); // nothing has freed space yet

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 3, 0ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_INT(1, writable_callback_count);
    EXPECT_EQ_INT(1, param_hits);
    EXPECT_TRUE(writable_callback_pub == &pub);

    // A second ACKNACK while already writable must not fire it again.
    tail = write_acknack(&node, ENDPOINT_ID, 4, 0ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_INT(1, writable_callback_count);
}

// Plan's addition 4: when every matched Subscriber is gone, a blocked KEEP_ALL Publisher unblocks -
// there is nobody whose acknowledgement could ever arrive, so waiting would be waiting on nothing.
static void test_keep_all_unblocks_when_last_subscriber_leaves(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 4);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 16);

    uint32_t value = 1;
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    // The Subscriber is declared not alive: its ack entry goes (Phase 3 (a)).
    forget_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID, /*match_any_entity=*/false);

    EXPECT_TRUE(tt_Publisher_writable(&pub));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
}

// Plan's addition 1: LIFESPAN still ends a gap under KEEP_ALL - an expired sample is "as if never
// sent", so the Publisher reports it gone (1-c's eviction Heartbeat) rather than retransmitting it
// forever.
static void test_keep_all_still_honours_lifespan(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    TEST_RELIABLE_CACHE(cache, 4);
    init_keep_all_publisher(&node, &topic, &pub, &cache, 16);
    pub.lifespan_duration_ns = 1000;

    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1 at t=0

    test_mock_now = 1000; // exactly at the lifespan boundary - expired
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 1, 1ULL); // asking for seq_no 1
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)cache.index[0].retry);         // not retransmitted
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // one eviction Heartbeat instead
    const struct tt_HeartbeatHeader* heartbeat = last_sent_heartbeat();
    EXPECT_TRUE(heartbeat != NULL);
    EXPECT_EQ_U32(2, heartbeat->first_available_seq_no); // nothing resendable remains
}

// A KEEP_ALL writer switches off the Subscriber's own ACKNACK give-up: the gap stays tracked past
// the point a KEEP_LAST writer's gap would have been abandoned.
static void test_keep_all_subscriber_never_gives_up(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    proxy->keep_all = tt_WRITER_KEEP_ALL_YES; // as its announce said
    proxy->sender_ip = TEST_SENDER_IP;
    proxy->sender_port = TEST_SENDER_PORT;
    bitmap_set_bit(proxy->received_bitmap, 4); // seq_no 5 arrived; 1..4 are missing
    proxy->acknack_scheduled = true;

    for (int i = 0; i < tt_RELIABLE_RETRY * 5; i++) { // far past the give-up budget
        acknack_retry(&node, tt_get_ns(), proxy);
    }
    EXPECT_EQ_U32(1, proxy->ack_seq_no);   // still waiting on seq_no 1, never abandoned it
    EXPECT_TRUE(proxy->acknack_scheduled); // ...and still asking

    // The same proxy with keep_all off gives up as before.
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    for (int i = 0; i <= tt_RELIABLE_RETRY; i++) {
        acknack_retry(&node, tt_get_ns(), proxy);
    }
    EXPECT_TRUE(proxy->ack_seq_no > 1);
}

// RELIABLE delivers in order, and does so whether or not it has somewhere to hold what it is
// waiting on. The two configurations reach the same guarantee by different routes, and both are
// pinned here because the no-buffer one is the default and the easy one to leave untested.
static void test_reliable_delivers_in_order_with_buffer(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);

    // Three samples arrive ahead of the gap at 2, out of order among themselves. None is
    // delivered, and the buffer does not care what order they arrived in.
    tail = write_data(&node, 5, 500, 5);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 3, 300, 3);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 4, 400, 4);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(3, sub.reorder_held_peak);

    // The gap fills. 2 goes up, then 3, 4 and 5 follow it in sequence order - not arrival order.
    tail = write_data(&node, 2, 200, 2);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(5, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(3, sub.reorder_delivered);
    EXPECT_EQ_U32(5, sub.last_seq_no);
    // The whole point: the application never saw a step backwards.
    EXPECT_EQ_U32(0, sub.out_of_order);
    EXPECT_EQ_U32(0, sub.timestamp_not_newer);
    EXPECT_EQ_U32(0, sub.reorder_overflow);
}

// With no buffer - the default, and what a small target runs - ordering is still correct. The
// sample ahead of the gap is not delivered and not recorded as received, so the ordinary ACKNACK
// exchange fetches it again once the gap has filled. Slower, never wrong.
static void test_reliable_delivers_in_order_without_buffer(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    sub.reorder_storage = NULL;
    sub.reorder_slots = 0;
    sub.reorder_slot_bytes = 0;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);

    tail = write_data(&node, 3, 300, 3); // ahead of the gap at 2
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count); // not delivered
    EXPECT_EQ_U32(1, sub.reorder_overflow);

    // And crucially not recorded as received, so it is still asked for. A set bit here would mean
    // the sample had been silently lost forever - the one outcome RELIABLE must never produce.
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(2, proxy->ack_seq_no);
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy)));

    // The gap fills, then 3 is retransmitted and delivered - in order, one round trip later.
    tail = write_data(&node, 2, 200, 2);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count);
    tail = write_data(&node, 3, 300, 3);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(3, sub.last_seq_no);
    EXPECT_EQ_U32(0, sub.out_of_order);
}

// A gap the Publisher says is unrecoverable must release what is waiting behind it. Otherwise a
// held sample waits for something that is never coming, and on a stream that then stops it waits
// forever - the failure mode of ordered delivery, and the reason DDS hands over what it has.
static void test_reliable_releases_held_samples_when_gap_is_abandoned(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 4, 400, 4); // 2 and 3 missing
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count); // 4 is held

    // The Publisher reports it no longer holds 2 or 3: the oldest it still has is 4.
    tail = write_heartbeat(&node, ENDPOINT_ID, 4, 4, tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count); // 4 released rather than stranded
    EXPECT_EQ_U32(1, sub.reorder_delivered);
    EXPECT_EQ_U32(4, sub.last_seq_no);
    EXPECT_EQ_U32(0, sub.out_of_order);
}

// Core must never address past slots * reorder_slot_bytes, whatever slot size the caller picks.
//
// The reorder stride has to be a multiple of 8 for its uint64_t header, and it first rounded UP -
// so a caller with a slot size that is not a multiple of 8 had core read and write past the end
// of its buffer. The perf_hil examples used 116-byte slots; core addressed 120, and on the rig
// that overran the reorder array into the adjacent globals in every RELIABLE scenario.
// AddressSanitizer: global-buffer-overflow in drain_reorder().
//
// Every other test in this file uses a slot size that is already a multiple of 8, which is the
// only reason none of them noticed. This one deliberately does not, sizes its storage to exactly
// what the contract promises and not a byte more, and fills every slot - so `make sanitize`
// fails if core ever steps past the end again.
#define ODD_SLOTS 4
#define ODD_SLOT_BYTES ((uint16_t)(sizeof(struct tt_ReorderSlot) + 12 + 5)) // deliberately not a multiple of 8
static uint64_t odd_storage[((size_t)ODD_SLOTS * ODD_SLOT_BYTES) / sizeof(uint64_t)];

static void test_reorder_stays_inside_an_odd_sized_buffer(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    memset(odd_storage, 0, sizeof(odd_storage));
    sub.reorder_storage = odd_storage;
    sub.reorder_slots = ODD_SLOTS;
    sub.reorder_slot_bytes = ODD_SLOT_BYTES;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // Fill every slot: 3..6 all arrive ahead of the gap at 2.
    for (uint32_t seq = 3; seq < 3 + ODD_SLOTS; seq++) {
        tail = write_data(&node, seq, seq * 100ULL, seq);
        EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    }
    // The gap fills and every held sample drains - which walks every slot.
    tail = write_data(&node, 2, 200, 2);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(2 + ODD_SLOTS, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(0, sub.out_of_order);
    EXPECT_EQ_U32(ODD_SLOTS, sub.reorder_delivered);
}

// Every path that moves a writer's watermark must release what the watermark passed. These two
// did not, and neither had a test - which is the only reason they survived.
//
// The retry give-up calls advance_ack_seq_no(), which steps past the abandoned sample AND absorbs
// every received sample contiguous behind it. With ordered delivery those are samples still held.
// The watermark was past them, nothing released them, and on a stream that then stopped they were
// never delivered at all.
static void test_retry_giveup_releases_the_samples_it_absorbs(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    // Only 2 is missing; 3, 4, 5 are held contiguously behind it.
    for (uint32_t seq = 3; seq <= 5; seq++) {
        tail = write_data(&node, seq, seq * 100ULL, seq);
        EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    }
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);

    // The writer does not promise KEEP_ALL and 2 has used its whole retry budget: give up on it.
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    proxy->retry = tt_RELIABLE_RETRY + 1;
    acknack_retry(&node, tt_get_ns(), proxy);

    // No further DATA arrives. 3, 4, 5 must be delivered anyway, in order, by the give-up itself.
    EXPECT_EQ_U32(6, proxy->ack_seq_no);
    EXPECT_EQ_U32(4, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(5, sub.last_seq_no);
    EXPECT_EQ_U32(0, sub.out_of_order);
    EXPECT_EQ_U32(0, sub.reorder_held);
}

// A Heartbeat revealing a gap wider than the window jumps the watermark past it. The drain in that
// handler used to sit between its two watermark moves - after advance_past_unavailable() and before
// this jump - so the jump passed held samples after they had last been checked.
static void test_heartbeat_jump_releases_what_it_passes(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 5, 500, 5);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 6, 600, 6);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)subscriber_callback_count);

    // Nothing evicted (first_available is the current watermark), but the Publisher is now so far
    // ahead that the gap can never be tracked: the handler jumps.
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    uint32_t far = proxy->ack_seq_no + proxy_window_bits(proxy) + 10;
    tail = write_heartbeat(&node, ENDPOINT_ID, proxy->ack_seq_no, far, tt_HEARTBEAT_FLAG_FINAL);
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_TRUE(proxy->ack_seq_no > 6);                    // the jump happened
    EXPECT_EQ_U32(3, (uint32_t)subscriber_callback_count); // and 5, 6 were released by it
    EXPECT_EQ_U32(6, sub.last_seq_no);
    EXPECT_EQ_U32(0, sub.out_of_order);
    EXPECT_EQ_U32(0, sub.reorder_held);
}

// A sample released from the reorder buffer is recorded against the socket it ARRIVED on, not the
// socket of whatever packet happened to release it.
//
// via_socket_flips counts socket changes in the delivered stream, and it read the socket off the
// node - which is the packet being processed right now. For a held sample that is a later DATA, a
// Heartbeat, or nothing at all when the retry timer releases it. Once drains were added to the
// heartbeat and give-up paths, a benchmark's flips jumped from 5 to 1462 with no change in the
// traffic: the metric was counting its triggers. This pins the attribution to the arrival.
static void test_released_sample_keeps_its_arrival_socket(void) {
    test_mock_reset();
    subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    struct tt_Header header;
    init_header(&header);

    node.rx_via_data_port = false; // 1 arrives on the well-known socket
    uint32_t tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    node.rx_via_data_port = true; // 3 and 4 arrive on the data socket, ahead of the gap at 2
    tail = write_data(&node, 3, 300, 3);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 4, 400, 4);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    node.rx_via_data_port = false; // 2 fills the gap on the well-known socket, releasing 3 and 4
    tail = write_data(&node, 2, 200, 2);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // Delivered: 1 well-known, 2 well-known, 3 data, 4 data - one change of socket, between 2 and 3.
    // Attributing 3 and 4 to the releasing packet's socket instead would read zero.
    EXPECT_EQ_U32(4, (uint32_t)subscriber_callback_count);
    EXPECT_EQ_U32(1, sub.via_socket_flips);
    EXPECT_TRUE(sub.last_via_data_port);
}

int main(void) {
    test_keep_all_refuses_at_bound_and_unblocks_on_ack();
    test_keep_last_still_evicts_rather_than_refusing();
    test_keep_all_bound_follows_smallest_window();
    test_keep_all_bound_honors_wide_window();
    test_keep_all_sustains_on_clean_link();
    test_keep_all_solicits_before_blocking();
    test_keep_all_solicitation_is_throttled();
    test_unknown_policy_still_terminates_on_eviction();
    test_keep_all_writable_callback_fires_once();
    test_keep_all_unblocks_when_last_subscriber_leaves();
    test_keep_all_still_honours_lifespan();
    test_keep_all_subscriber_never_gives_up();
    test_reliable_publish_caches_and_evicts();
    test_reliable_cache_evicts_by_bytes_oldest_first();
    test_reliable_cache_wrap_slack_keeps_depth_samples();
    test_reliable_cache_oversize_record_is_not_cached();
    test_reliable_cache_oversize_record_on_empty_cache();
    test_reliable_cache_randomized_invariants();
    test_reliable_subscribe_in_order_no_acknack();
    test_reliable_subscribe_gap_then_close();
    test_reliable_new_gap_while_armed_gets_immediate_narrow_nack();
    test_reliable_acknack_retry_uses_reliable_retry_interval();
    test_reliable_duplicate_delivery_is_not_re_delivered_to_callback();
    test_reliable_delivers_in_order_with_buffer();
    test_reliable_delivers_in_order_without_buffer();
    test_reliable_releases_held_samples_when_gap_is_abandoned();
    test_reorder_stays_inside_an_odd_sized_buffer();
    test_retry_giveup_releases_the_samples_it_absorbs();
    test_heartbeat_jump_releases_what_it_passes();
    test_released_sample_keeps_its_arrival_socket();
    test_reliable_late_arrivals_after_baseline_jump_are_discarded_not_silently();
    test_reliable_held_samples_precede_the_jump_that_releases_them();
    test_reliable_subscribe_bitmap_stays_aligned_after_partial_recovery();
    test_acknack_retry_exhausted_gives_up();
    test_acknack_retry_give_up_does_not_bulk_skip();
    test_reliable_subscribe_oversized_first_gap_jumps_baseline_instead_of_freezing();
    test_reliable_first_contact_via_data_does_not_request_pre_match_history();
    test_acknack_retry_budget_resets_for_next_gap();
    test_process_acknack_retransmits_cached_sample();
    test_process_acknack_direct_index_correct_after_wraparound();
    test_process_acknack_skips_expired_sample();
    test_process_acknack_ignored_for_besteffort_publisher();
    test_process_acknack_ignored_for_durable_only_publisher();
    test_process_acknack_updates_peer_ack_seq_no();
    test_process_acknack_does_not_regress_peer_ack_seq_no();
    test_process_acknack_from_unmatched_peer_updates_nothing();
    test_wide_gap_tracked_with_caller_sized_window();
    test_oversized_window_request_is_clamped();
    test_two_subscriber_entities_on_one_node_ack_independently();
    test_process_acknack_from_unmatched_entity_records_nothing();
    test_ack_table_full_refuses_further_entities();
    test_acknack_rejects_malformed_bitmap_words();
    test_acknack_with_no_bitmap_words_is_a_pure_ack();
    test_acknack_multi_word_bitmap_retransmits_each_named_sample();
    test_forget_publisher_peer_resets_ack_seq_no();
#ifdef tt_RELIABLE_STATS
    test_keep_all_writable_cause_is_distinguished();
    test_reliable_stats_subscriber_gap_accounting();
    test_reliable_stats_publisher_retransmit_accounting();
#endif

    printf("test_reliable_pubsub: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
