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
    return find_writer_proxy(sub, REMOTE_NODE_ID, 0);
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
    acknack_header->endpoint_id = endpoint_id;
    acknack_header->seq_no = seq_no;
    acknack_header->bitmap[0] = bitmap;
    for (int w = 1; w < tt_RELIABLE_BITMAP_WORDS; w++) {
        acknack_header->bitmap[w] = 0;
    }
    return sizeof(struct tt_AckNackHeader);
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;

    for (uint32_t i = 0; i < 6; i++) { // more than depth -> the first 2 (seq_no 1, 2) get evicted
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }
    EXPECT_EQ_U32(6, (uint32_t)pub.seq_no);

    bool saw_evicted = false;
    int live_count = 0;
    for (int i = 0; i < 4; i++) {
        if (cache.entries[i].len != 0) {
            live_count++;
            if (cache.entries[i].seq_no == 1 || cache.entries[i].seq_no == 2) {
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
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap));
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(3, (uint32_t)subscriber_callback_count);
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
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap));
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

    tail = write_data(&node, 3, 300, 3); // out of order, genuinely new - ahead of the gap at 2
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count);

    tail = write_data(&node, 3, 300, 3); // seq_no 3 again - e.g. a retransmit racing the original
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count); // not re-delivered - the bit was already set

    tail = write_data(&node, 2, 200, 2); // fills the gap - genuinely new, watermark advances past 3 too
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, (uint32_t)subscriber_callback_count);

    // seq_no 1 again, well below the watermark (4) now - deliberately NOT deduplicated (accepted
    // trade-off, see this test's own doc comment): still delivered, count advances.
    tail = write_data(&node, 1, 100, 1);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(4, (uint32_t)subscriber_callback_count);

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(4, proxy->ack_seq_no);
}

// Milestone 60 - the exact real-CI regression this milestone's own first attempt caused, pinned as
// its own permanent test: a gap wide enough to trigger jump_ack_baseline() must not cause every
// subsequently-arriving, merely-reordered (never actually lost) sample from the abandoned range to
// be silently dropped from delivery - each one is still genuinely new to this Subscriber.
static void test_reliable_reordered_arrivals_after_baseline_jump_are_still_delivered(void) {
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

    // A far-ahead arrival (offset >= tt_RELIABLE_BITMAP_BITS) triggers jump_ack_baseline() -
    // abandons tracking for 2..(far_seq - 1), none of which have actually been delivered yet.
    uint32_t far_seq = 2 + tt_RELIABLE_BITMAP_BITS;
    tail = write_data(&node, far_seq, far_seq * 100ULL, far_seq);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)subscriber_callback_count); // the jump-triggering sample itself delivered

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(far_seq + 1, proxy->ack_seq_no);

    // Several samples from the abandoned range now arrive, merely reordered (never actually lost) -
    // every one of them is genuinely new to this Subscriber and must still be delivered.
    for (uint32_t seq = 2; seq < far_seq; seq += 7) {
        tail = write_data(&node, seq, seq * 100ULL, seq);
        EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    }
    EXPECT_TRUE((uint32_t)subscriber_callback_count > 2); // not silently dropped
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
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap));
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
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap));
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
    EXPECT_EQ_U32(3, proxy->ack_seq_no);                  // now waiting on 3, not 2
    EXPECT_TRUE(!bitmap_is_zero(proxy->received_bitmap)); // still a gap (3 missing, 4 already in)
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
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap));
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
    EXPECT_EQ_U32(51, proxy->ack_seq_no);                     // synced straight to just past this first sample
    EXPECT_TRUE(bitmap_is_zero(proxy->received_bitmap));      // nothing "missing" before it - never tracked at all
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
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
    EXPECT_EQ_U32(1, (uint32_t)cache.entries[0].retry);
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.reliable = true;

    for (uint32_t i = 0; i < 6; i++) { // seq_no 1..6, ring wraps once past depth 4
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }
    EXPECT_EQ_U32(5, cache.entries[0].seq_no); // slot 0: seq_no 1 evicted, now holds 5
    EXPECT_EQ_U32(6, cache.entries[1].seq_no); // slot 1: seq_no 2 evicted, now holds 6
    EXPECT_EQ_U32(3, cache.entries[2].seq_no); // slot 2: never overwritten
    EXPECT_EQ_U32(4, cache.entries[3].seq_no); // slot 3: never overwritten

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
    EXPECT_EQ_U32(0, (uint32_t)cache.entries[0].retry); // untouched - correctly not matched
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
    EXPECT_EQ_U32(1, (uint32_t)cache.entries[2].retry); // seq_no 3 resent
    heartbeat = last_sent_heartbeat();
    EXPECT_TRUE(heartbeat != NULL);
    EXPECT_EQ_U32(3, heartbeat->first_available_seq_no);
    cache.entries[2].retry = 0; // reset for the "un-wrapped slot 2" case below

    // Wrapped-into slot 0, genuinely retained: seq_no 5.
    test_mock_send_to_call_count = 0;
    tail = write_acknack(&node, ENDPOINT_ID, 5, 1ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)cache.entries[0].retry); // found via slot 0, the right one
    EXPECT_EQ_U32(0, (uint32_t)cache.entries[2].retry); // slot 2 untouched by this request

    // Un-wrapped slot 2, genuinely retained: seq_no 3.
    test_mock_send_to_call_count = 0;
    tail = write_acknack(&node, ENDPOINT_ID, 3, 1ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)cache.entries[2].retry); // found via slot 2, the right one
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
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

    EXPECT_EQ_U32(0, (uint32_t)cache.entries[0].retry); // expired - no retransmit
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
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

// QoS roadmap #5 (RELIABILITY) follow-up - tt_Publisher_wait_for_all_acked(). An ACKNACK from a
// peer already present in pub->peers[] must advance that peer's own peer_ack_seq_no[] slot to the
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 5, 0ULL); // seq_no 5, no gap requested
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(5, pub.peer_ack_seq_no[0]);
}

// A stale/reordered ACKNACK (UDP gives no ordering guarantee) carrying a seq_no lower than what's
// already recorded must never regress peer_ack_seq_no[] - only ever advance it.
static void test_process_acknack_does_not_regress_peer_ack_seq_no(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;
    pub.peer_ack_seq_no[0] = 10;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 3, 0ULL); // stale - already at 10
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(10, pub.peer_ack_seq_no[0]);
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
    pub.reliable_cache = &cache; // pub.peers[] left entirely empty

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 5, 0ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        EXPECT_EQ_U32(0, pub.peer_ack_seq_no[i]);
    }
}

// forget_publisher_peer() must reset the departed peer's own peer_ack_seq_no[] slot alongside its
// peers[] slot - otherwise a later, unrelated node_id reclaiming that same array index (upsert_
// peer()'s own first-empty-slot reuse) would inherit a stale ack value that was never actually
// about it.
static void test_forget_publisher_peer_resets_ack_seq_no(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = TEST_SENDER_IP;
    pub.peers[0].port = TEST_SENDER_PORT;
    pub.peer_ack_seq_no[0] = 42;

    forget_publisher_peer(&pub, REMOTE_NODE_ID);

    EXPECT_EQ_INT((int)tt_NODE_ID_INVALID, (int)pub.peers[0].node_id);
    EXPECT_EQ_U32(0, pub.peer_ack_seq_no[0]);
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

    struct tt_ReliableCacheEntry cache_entries[4];
    memset(cache_entries, 0, sizeof(cache_entries));
    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.entries = cache_entries;
    cache.capacity = 4;
    cache.depth = 4;
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

int main(void) {
    test_reliable_publish_caches_and_evicts();
    test_reliable_subscribe_in_order_no_acknack();
    test_reliable_subscribe_gap_then_close();
    test_reliable_new_gap_while_armed_gets_immediate_narrow_nack();
    test_reliable_acknack_retry_uses_reliable_retry_interval();
    test_reliable_duplicate_delivery_is_not_re_delivered_to_callback();
    test_reliable_reordered_arrivals_after_baseline_jump_are_still_delivered();
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
    test_forget_publisher_peer_resets_ack_seq_no();
#ifdef tt_RELIABLE_STATS
    test_reliable_stats_subscriber_gap_accounting();
    test_reliable_stats_publisher_retransmit_accounting();
#endif

    printf("test_reliable_pubsub: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
