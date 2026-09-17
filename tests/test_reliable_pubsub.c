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

static uint32_t write_acknack(struct tt_Node* node, uint32_t endpoint_id, uint32_t seq_no, uint64_t bitmap) {
    struct tt_AckNackHeader* acknack_header = (struct tt_AckNackHeader*)node->rx_buffer;
    acknack_header->endpoint_id = endpoint_id;
    acknack_header->seq_no = seq_no;
    acknack_header->bitmap = bitmap;
    return sizeof(struct tt_AckNackHeader);
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

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
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

    EXPECT_EQ_U32(4, sub.ack_seq_no);
    EXPECT_TRUE(sub.received_bitmap == 0);
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

    EXPECT_EQ_U32(2, sub.ack_seq_no);         // still waiting on 2
    EXPECT_TRUE(sub.received_bitmap == 2ULL); // bit 1 -> seq_no 3 (ack_seq_no + 1) received early
    EXPECT_TRUE(sub.reliable_acknack_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // immediate ACKNACK
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip);
    EXPECT_EQ_U32(TEST_SENDER_PORT, (uint32_t)test_mock_send_to_last_port);

    tail = write_data(&node, 2, 200, 2); // the missing sample finally arrives
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(4, sub.ack_seq_no); // 2 lands, then absorbs the already-buffered bit for 3
    EXPECT_TRUE(sub.received_bitmap == 0);
    EXPECT_TRUE(!sub.reliable_acknack_scheduled);
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
    EXPECT_EQ_U32(2, sub.ack_seq_no); // in order so far

    tail = write_data(&node, 4, 400, 4); // seq_no 2, 3 both missing
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    tail = write_data(&node, 5, 500, 5);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(2, sub.ack_seq_no);          // 2 still the watermark - neither 4 nor 5 fill it
    EXPECT_TRUE(sub.received_bitmap == 12ULL); // bits 2,3: (ack_seq_no+2)=4, (ack_seq_no+3)=5

    tail = write_data(&node, 2, 200, 2); // fills the watermark itself via an exact match
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    // ack_seq_no advances to 3 (only 2 was confirmed - 3 is still missing, so it must NOT jump
    // any further); the bitmap must re-align to keep tracking 4 and 5 correctly relative to the
    // new watermark, not silently start claiming 5 and 6 are the ones already received.
    EXPECT_EQ_U32(3, sub.ack_seq_no);
    EXPECT_TRUE(sub.received_bitmap == 6ULL); // bits 1,2: (ack_seq_no+1)=4, (ack_seq_no+2)=5

    tail = write_data(&node, 3, 300, 3); // the last real gap closes
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(6, sub.ack_seq_no); // 3 lands, then absorbs the already-buffered 4 and 5 too
    EXPECT_TRUE(sub.received_bitmap == 0);
}

// Regression test for a third real bug found via run_perf.sh's own tc/netem loss-injection
// scenarios: a gap wider than tt_MAX_RELIABLE_HISTORY used to only ever shrink one position per
// tt_RELIABLE_RETRY cycle (acknack_retry()'s own give-up path) even though every position beyond
// the Publisher's own retained-cache depth is *provably* unrecoverable the instant a sample that
// far ahead is seen at all - nothing will ever un-evict it. Left alone, real time (and the
// Publisher's own cache) kept moving on regardless, so an initial small gap under real loss grew
// into a many-dozen-sequence-number backlog of "requested, not found in cache" ACKNACKs instead
// of resolving in a bounded number of retries. update_reliable_ack() must now fast-forward past
// the unrecoverable range in a single step instead.
static void test_reliable_subscribe_fast_forwards_past_unrecoverable_gap(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    EXPECT_EQ_U32(1, sub.ack_seq_no);

    struct tt_Header header;
    init_header(&header);

    // seq_no 14 is (tt_MAX_RELIABLE_HISTORY + 5) ahead of the watermark (1) - seq_no 1..6 are
    // guaranteed already evicted from any Publisher's reliable_cache by the time seq_no 14 exists
    // at all (KEEP_LAST, depth <= tt_MAX_RELIABLE_HISTORY), only 7..13 could still plausibly be
    // cached.
    uint32_t tail = write_data(&node, 14, 1400, 14);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // Fast-forwarded straight to 14 - tt_MAX_RELIABLE_HISTORY + 1 = 7 (the oldest position that
    // could plausibly still be recovered), not crept forward one at a time from 1.
    EXPECT_EQ_U32((uint32_t)(14 - tt_MAX_RELIABLE_HISTORY + 1), sub.ack_seq_no);
    // bit (tt_MAX_RELIABLE_HISTORY - 1): ack_seq_no + (tt_MAX_RELIABLE_HISTORY - 1) = 14, the
    // sample that triggered this - still correctly tracked as received, not lost in the jump.
    EXPECT_TRUE(sub.received_bitmap == (1ULL << (tt_MAX_RELIABLE_HISTORY - 1)));
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

    sub.ack_seq_no = 5;
    sub.received_bitmap = 2ULL; // seq_no 5 still missing, seq_no 6 already received (bit j: ack_seq_no + j)
    sub.reliable_sender_node_id = REMOTE_NODE_ID;
    sub.reliable_sender_ip = TEST_SENDER_IP;
    sub.reliable_sender_port = TEST_SENDER_PORT;
    sub.reliable_acknack_scheduled = true;

    for (int i = 0; i < tt_RELIABLE_RETRY; i++) {
        acknack_retry(&node, tt_get_ns(), &sub);
        EXPECT_TRUE(sub.reliable_acknack_scheduled);
    }
    EXPECT_EQ_U32((uint32_t)tt_RELIABLE_RETRY, (uint32_t)sub.reliable_retry);

    acknack_retry(&node, tt_get_ns(), &sub); // exceeds the cap -> give up

    EXPECT_TRUE(!sub.reliable_acknack_scheduled);
    EXPECT_TRUE(sub.received_bitmap == 0);
    EXPECT_EQ_U32(7, sub.ack_seq_no); // skipped past seq_no 5, absorbed the already-known 6 too
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
    EXPECT_EQ_U32(2, sub.ack_seq_no);

    tail = write_data(&node, 4, 400, 4); // seq_no 2, 3 missing - opens the first gap
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_TRUE(sub.reliable_acknack_scheduled);

    acknack_retry(&node, tt_get_ns(), &sub); // burn 2 of the 3 retries on seq_no 2's own gap
    acknack_retry(&node, tt_get_ns(), &sub);
    EXPECT_EQ_U32(2, (uint32_t)sub.reliable_retry);

    tail = write_data(&node, 2, 200, 2); // seq_no 2 recovers - a *different* gap (seq_no 3) remains
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(3, sub.ack_seq_no);      // now waiting on 3, not 2
    EXPECT_TRUE(sub.received_bitmap != 0); // still a gap (3 missing, 4 already in)
    // Without the fix, this would read 2 (inherited from seq_no 2's own already-used attempts)
    // instead of a fresh budget for the new watermark.
    EXPECT_EQ_U32(0, (uint32_t)sub.reliable_retry);

    // seq_no 3's own gap must get a full, independent tt_RELIABLE_RETRY budget.
    for (int i = 0; i < tt_RELIABLE_RETRY; i++) {
        acknack_retry(&node, tt_get_ns(), &sub);
        EXPECT_TRUE(sub.reliable_acknack_scheduled); // not given up yet
    }
    acknack_retry(&node, tt_get_ns(), &sub); // exceeds the cap now
    EXPECT_TRUE(!sub.reliable_acknack_scheduled);
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

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.reliable_cache = &cache;

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

int main(void) {
    test_reliable_publish_caches_and_evicts();
    test_reliable_subscribe_in_order_no_acknack();
    test_reliable_subscribe_gap_then_close();
    test_reliable_subscribe_bitmap_stays_aligned_after_partial_recovery();
    test_reliable_subscribe_fast_forwards_past_unrecoverable_gap();
    test_acknack_retry_exhausted_gives_up();
    test_acknack_retry_budget_resets_for_next_gap();
    test_process_acknack_retransmits_cached_sample();
    test_process_acknack_ignored_for_besteffort_publisher();

    printf("test_reliable_pubsub: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
