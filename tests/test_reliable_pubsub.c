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

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(4, proxy->ack_seq_no);
    EXPECT_TRUE(proxy->received_bitmap == 0);
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
    EXPECT_EQ_U32(2, proxy->ack_seq_no);         // still waiting on 2
    EXPECT_TRUE(proxy->received_bitmap == 2ULL); // bit 1 -> seq_no 3 (ack_seq_no + 1) received early
    EXPECT_TRUE(proxy->acknack_scheduled);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // immediate ACKNACK
    EXPECT_EQ_U32(TEST_SENDER_IP, test_mock_send_to_last_ip);
    EXPECT_EQ_U32(TEST_SENDER_PORT, (uint32_t)test_mock_send_to_last_port);

    tail = write_data(&node, 2, 200, 2); // the missing sample finally arrives
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(4, proxy->ack_seq_no); // 2 lands, then absorbs the already-buffered bit for 3
    EXPECT_TRUE(proxy->received_bitmap == 0);
    EXPECT_TRUE(!proxy->acknack_scheduled);
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
    EXPECT_EQ_U32(2, proxy->ack_seq_no);          // 2 still the watermark - neither 4 nor 5 fill it
    EXPECT_TRUE(proxy->received_bitmap == 12ULL); // bits 2,3: (ack_seq_no+2)=4, (ack_seq_no+3)=5

    tail = write_data(&node, 2, 200, 2); // fills the watermark itself via an exact match
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    // ack_seq_no advances to 3 (only 2 was confirmed - 3 is still missing, so it must NOT jump
    // any further); the bitmap must re-align to keep tracking 4 and 5 correctly relative to the
    // new watermark, not silently start claiming 5 and 6 are the ones already received.
    EXPECT_EQ_U32(3, proxy->ack_seq_no);
    EXPECT_TRUE(proxy->received_bitmap == 6ULL); // bits 1,2: (ack_seq_no+1)=4, (ack_seq_no+2)=5

    tail = write_data(&node, 3, 300, 3); // the last real gap closes
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(6, proxy->ack_seq_no); // 3 lands, then absorbs the already-buffered 4 and 5 too
    EXPECT_TRUE(proxy->received_bitmap == 0);
}

// Regression test for a third real bug found via run_perf.sh's own tc/netem loss-injection
// scenarios: a gap wider than tt_MAX_RELIABLE_HISTORY used to only ever shrink one position per
// tt_RELIABLE_RETRY cycle (acknack_retry()'s own give-up path) even though every position beyond
// the Publisher's own retained-cache depth is *provably* unrecoverable by the time ack_seq_no
// itself finally gets given up on - nothing will ever un-evict it. Left alone, real time (and the
// Publisher's own cache) kept moving on regardless, so an initial small gap under real loss grew
// into a many-dozen-sequence-number backlog of "requested, not found in cache" ACKNACKs instead
// of resolving in a bounded number of retries. (An even earlier version of this fix tried
// fast-forwarding reactively on every far-ahead arrival instead of only at give-up time - that
// backfired by perpetually re-triggering itself and pre-empting ack_seq_no's own retry cycle
// before it ever got a fair chance to resolve normally; skip_unrecoverable_backlog() only ever
// runs from acknack_retry()'s own give-up path now, confirmed below.)
//
// PLAN.md's Milestone 24 briefly raised tt_MAX_RELIABLE_HISTORY all the way to tt_RELIABLE_BITMAP_
// BITS (64, its own hard ceiling), which made this bulk-skip logic provably a no-op - any position
// received_bitmap can even represent (bit 0..63) was then, by construction, always within a
// plausibly-still-cached window, since update_reliable_ack()'s own oversized-gap check (offset >=
// tt_RELIABLE_BITMAP_BITS) already catches anything wider before it's ever recorded in the bitmap
// at all. Milestone 25 (the real loss_pct-floor fix, elsewhere) freed depth to be re-tuned down
// again on its own real-HIL merits, back below the bitmap width - so this test is parametrized to
// work correctly either way, whichever regime the current depth sits in, rather than needing its
// own rewrite every time that tuning changes: below the bitmap width, a gap "6 past depth" is the
// classic bulk-skip case this test was originally written for; at (or past) it, the same gap isn't
// representable in the bitmap at all, and the correct behavior is Milestone 24's own no-op proof
// instead.
static void test_acknack_retry_bulk_skip_matches_depth_vs_bitmap_width(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);

    // Milestone 47 - this test pre-seeds gap state directly (no real packet involved yet), so it
    // must claim the WriterProxy entry itself first - find_or_create_writer_proxy() sets ack_seq_no
    // to 1, matching this test's own original "freshly-initialized" assumption.
    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(1, proxy->ack_seq_no);

    // seq_no 1 (ack_seq_no itself) never arrives; seq_no far_seq already did, out of order - either
    // 6 past the current depth (classic bulk-skip shape) or the widest bit received_bitmap can ever
    // represent at all, whichever is narrower.
    const bool bulk_skip_possible = tt_MAX_RELIABLE_HISTORY + 6 < tt_RELIABLE_BITMAP_BITS;
    const uint32_t far_seq = bulk_skip_possible ? tt_MAX_RELIABLE_HISTORY + 6 : tt_RELIABLE_BITMAP_BITS;
    proxy->received_bitmap = 1ULL << (far_seq - 1); // bit (far_seq - 1): ack_seq_no(1) + (far_seq - 1) = far_seq
    proxy->sender_ip = TEST_SENDER_IP;
    proxy->sender_port = TEST_SENDER_PORT;
    proxy->acknack_scheduled = true;

    // ack_seq_no (seq_no 1) gets its own fair tt_RELIABLE_RETRY attempts first - untouched, not
    // pre-empted by the far-ahead bit already sitting in the bitmap.
    for (int i = 0; i < tt_RELIABLE_RETRY; i++) {
        acknack_retry(&node, tt_get_ns(), proxy);
        EXPECT_EQ_U32(1, proxy->ack_seq_no);
    }

    acknack_retry(&node, tt_get_ns(), proxy); // exceeds the cap -> give up, then (maybe) bulk-skip

    if (bulk_skip_possible) {
        // advance_ack_seq_no() moves past seq_no 1 alone (bit 0 isn't set, nothing immediately
        // following to absorb), landing at 2; skip_unrecoverable_backlog() then jumps the rest of
        // the way in one step, since far_seq - 2 + 1 > tt_MAX_RELIABLE_HISTORY.
        EXPECT_EQ_U32(far_seq - tt_MAX_RELIABLE_HISTORY + 1, proxy->ack_seq_no);
        // bit (tt_MAX_RELIABLE_HISTORY - 1): ack_seq_no + (tt_MAX_RELIABLE_HISTORY - 1) = far_seq,
        // the sample already known received - still correctly tracked, not lost in the jump.
        EXPECT_TRUE(proxy->received_bitmap == (1ULL << (tt_MAX_RELIABLE_HISTORY - 1)));
    } else {
        // depth >= bitmap width: far_seq's own bit is always within the tracking window by
        // construction, so this correctly resolves through the normal retry cycle instead - no
        // bulk skip, same reasoning Milestone 24's own removed test proved directly.
        EXPECT_EQ_U32(2, proxy->ack_seq_no);
        EXPECT_TRUE(proxy->received_bitmap == (1ULL << (far_seq - 2)));
    }
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
    proxy->received_bitmap = 2ULL; // seq_no 5 still missing, seq_no 6 already received (bit j: ack_seq_no + j)
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
    EXPECT_TRUE(proxy->received_bitmap == 0);
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
    EXPECT_EQ_U32(3, proxy->ack_seq_no);      // now waiting on 3, not 2
    EXPECT_TRUE(proxy->received_bitmap != 0); // still a gap (3 missing, 4 already in)
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
// received_bitmap (>= tt_RELIABLE_BITMAP_BITS, the wire format's own fixed 64-bit ceiling) used
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
    // arrival is seq_no 1000, an offset (999) far past tt_RELIABLE_BITMAP_BITS (64).
    uint32_t tail = write_data(&node, 1000, 100000, 1000);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    // Fixed: baseline jumps to just past this arrival instead of staying stuck at 1.
    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(1001, proxy->ack_seq_no);
    EXPECT_TRUE(proxy->received_bitmap == 0);
    EXPECT_TRUE(!proxy->acknack_scheduled); // no phantom gap left armed for 1..999

    // The stream must now track normally from here - a small, genuinely resolvable gap right
    // after the jump must still be detected and ACKNACKed, proving ack_seq_no didn't just move,
    // it's actually live again (the exact failure mode being fixed: without it, *every* arrival,
    // including this one, would have hit the same oversized-offset branch and done nothing).
    test_mock_send_to_call_count = 0;
    tail = write_data(&node, 1002, 100200, 1002); // 1001 skipped
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(1001, proxy->ack_seq_no);      // still correctly waiting on 1001
    EXPECT_TRUE(proxy->received_bitmap == 2ULL); // bit 1 -> seq_no 1002 (1001 + 1) received early
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
    // BITS (64) of the stale ack_seq_no==1 default.
    uint32_t tail = write_data(&node, 50, 5000, 50);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    struct tt_WriterProxy* proxy = remote_writer_proxy(&sub);
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(51, proxy->ack_seq_no);                     // synced straight to just past this first sample
    EXPECT_TRUE(proxy->received_bitmap == 0);                 // nothing "missing" before it - never tracked at all
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

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.depth = 4;
    pub.reliable_cache = &cache;
    pub.lifespan_duration_ns = 1000;

    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1, timestamp 0

    test_mock_now = 1000; // exactly at the lifespan boundary - already expired (>=, not >)
    test_mock_send_to_call_count = 0;

    struct tt_Header header;
    init_header(&header);

    uint32_t tail = write_acknack(&node, ENDPOINT_ID, 1, 1ULL); // requesting seq_no 1 (bit 0)
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count); // expired - no retransmit
    EXPECT_EQ_U32(0, (uint32_t)cache.entries[0].retry);
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

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
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

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
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

    struct tt_ReliableCache cache;
    memset(&cache, 0, sizeof(cache));
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

int main(void) {
    test_reliable_publish_caches_and_evicts();
    test_reliable_subscribe_in_order_no_acknack();
    test_reliable_subscribe_gap_then_close();
    test_reliable_duplicate_delivery_is_not_re_delivered_to_callback();
    test_reliable_reordered_arrivals_after_baseline_jump_are_still_delivered();
    test_reliable_subscribe_bitmap_stays_aligned_after_partial_recovery();
    test_acknack_retry_exhausted_gives_up();
    test_acknack_retry_bulk_skip_matches_depth_vs_bitmap_width();
    test_reliable_subscribe_oversized_first_gap_jumps_baseline_instead_of_freezing();
    test_reliable_first_contact_via_data_does_not_request_pre_match_history();
    test_acknack_retry_budget_resets_for_next_gap();
    test_process_acknack_retransmits_cached_sample();
    test_process_acknack_skips_expired_sample();
    test_process_acknack_ignored_for_besteffort_publisher();
    test_process_acknack_updates_peer_ack_seq_no();
    test_process_acknack_does_not_regress_peer_ack_seq_no();
    test_process_acknack_from_unmatched_peer_updates_nothing();
    test_forget_publisher_peer_resets_ack_seq_no();

    printf("test_reliable_pubsub: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
