/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The reorder hold-off's stage S0 (2026-11-03): a RELIABLE reader counts a "fill" - a gap filled by the writer's first
// transmission arriving late (not addressed to this node, tt_Context.rx_targeted false) rather than by a repair - and
// splits it by whether the gap had been NACKed already (nack_spurious, E1) or not yet (reorder_absorbed, E2)
// (tickle.c rstat_reorder_fill()). Instrumentation only: nothing here may change what the reader does.
//
// Every case has its control: the same gap filled by a repair (rx_targeted true) must count as recovered and never as
// a fill, so a classification that ignores the address, or inverts it, fails here.

// The reliability counters compiled in: every subject of this file is one.
#define tt_RELIABLE_STATS 1

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

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

static void init_node_and_topic(struct tt_Context* node, struct tt_TypeSupport* topic) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node->rx_seq_span = 1; // as node_init() leaves it: one seq_no per datagram

    memset(topic, 0, sizeof(*topic));
    topic->name = "test_topic";
    topic->data_size = sizeof(uint32_t);
    topic->data_encode_size = stub_data_encode_size;
    topic->data_encode = stub_data_encode;
    topic->data_decode = stub_data_decode;
    topic->data_free = stub_data_free;
}

#define TEST_REORDER_SLOTS 16
#define TEST_REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + 64)
static uint64_t test_reorder_storage[TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES / sizeof(uint64_t)];

// A reorder buffer as a Linux-class caller supplies one: without it a sample ahead of a gap is un-received and the
// out-of-order arrivals this file needs would be asked for again instead (test_heartbeat.c's own helper).
static void init_subscriber_registered_on_node(struct tt_Subscriber* sub, struct tt_Context* node,
                                               struct tt_TypeSupport* topic) {
    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = ENDPOINT_ID;
    sub->node = node;
    sub->topic = topic;
    sub->callback = stub_subscriber_callback;
    sub->reliable = true;
    sub->reorder_storage = test_reorder_storage;
    sub->reorder_slots = TEST_REORDER_SLOTS;
    sub->reorder_slot_bytes = TEST_REORDER_SLOT_BYTES;
    memset(test_reorder_storage, 0, sizeof(test_reorder_storage));
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)sub;
}

static void init_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
}

static uint32_t write_data(struct tt_Context* node, uint32_t seq_no) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node->rx_buffer;
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = (uint64_t)seq_no * 100U;
    data_header->entity_id = 0;

    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node->rx_buffer + tail, &seq_no, sizeof(seq_no));
    return tail + sizeof(seq_no);
}

static struct tt_Context node;
static struct tt_TypeSupport topic;
static struct tt_Subscriber sub;
static struct tt_Header header;

static void reset(void) {
    test_mock_reset();
    tt_reliable_stats_reset();
    test_mock_now = 1000 * tt_MICROSECOND; // non-zero: a 0 timestamp means "unset" to the stats
    init_node_and_topic(&node, &topic);
    init_subscriber_registered_on_node(&sub, &node, &topic);
    init_header(&header);
}

// One DATA of seq_no, `after_us` after the last; `repair`: addressed to this node (a retransmission), else the
// writer's original.
static void arrive(uint32_t seq_no, uint32_t after_us, bool repair) {
    test_mock_now += (uint64_t)after_us * tt_MICROSECOND;
    uint32_t tail = write_data(&node, seq_no);
    node.rx_targeted = repair;
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    node.rx_targeted = false;
}

static struct tt_WriterProxy* proxy_of(void) {
    return find_writer_proxy(&sub, REMOTE_NODE_ID, 0);
}

static uint64_t hist_total(const uint64_t* hist) {
    uint64_t total = 0;
    for (int i = 0; i < tt_RELIABLE_STATS_HIST_BUCKETS; i++) {
        total += hist[i];
    }
    return total;
}

// The original arrives after the gap was NACKed (E1). 1, 3 (gap at 2: the immediate ACKNACK names it), then 2's
// original 300 us later: one fill, spurious - its repair is still to come - while the reader has no recovery estimate
// yet. Then that repair, a duplicate, counts nothing new. The control on the same proxy: 4, 6 (gap at 5, NACKed), then
// 5 as a repair - recovered, and no fill.
static void test_a_late_original_after_its_nack_is_a_spurious_nack(void) {
    reset();
    arrive(1, 100, false);
    arrive(3, 100, false); // gap at 2, detected now
    struct tt_WriterProxy* proxy = proxy_of();
    EXPECT_TRUE(proxy != NULL);
    EXPECT_TRUE(oldest_request_naming(proxy, 2) != 0); // the precondition: 2 was asked for
    EXPECT_EQ_U32(0, proxy->recovery_srtt_ns);         // ...and the reader has no estimate yet
    arrive(2, 300, false);

    struct tt_ReliableStats stats;
    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(1, (uint32_t)stats.recovered);
    EXPECT_EQ_U32(1, (uint32_t)stats.reorder_fill_original);
    EXPECT_EQ_U32(1, (uint32_t)stats.nack_spurious);
    EXPECT_EQ_U32(0, (uint32_t)stats.reorder_absorbed);
    EXPECT_EQ_U32(1, (uint32_t)stats.reorder_fill_before_rtt);
    EXPECT_EQ_U64(1, hist_total(stats.reorder_delay_hist));
    EXPECT_EQ_U64(1, stats.reorder_delay_hist[9]); // 300 us after detection: [256, 512) us
    EXPECT_EQ_U64(1, hist_total(stats.reorder_distance_hist));
    EXPECT_EQ_U64(1, stats.reorder_distance_hist[1]); // one datagram behind 3: [1, 2)

    arrive(2, 100, true); // its repair: below the watermark now, a copy after delivery
    arrive(4, 100, false);
    arrive(6, 100, false); // gap at 5, NACKed
    EXPECT_TRUE(oldest_request_naming(proxy, 5) != 0);
    arrive(5, 100, true); // the control: a repair fills it

    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(2, (uint32_t)stats.recovered);
    EXPECT_EQ_U32(1, (uint32_t)stats.reorder_fill_original);
    EXPECT_EQ_U32(1, (uint32_t)stats.nack_spurious);
    EXPECT_EQ_U32(0, (uint32_t)stats.reorder_absorbed);
    EXPECT_EQ_U64(1, hist_total(stats.reorder_delay_hist));
}

// The original arrives before any NACK named its gap (E2). The ACKNACK the gap at 2 would send finds the tx buffer
// full and is not sent, so nothing is remembered as requested; 2's original then fills it: absorbed, not spurious.
// The control: the same gap at 2 on a fresh reader, filled by a repair, is no fill.
static void test_a_late_original_before_any_nack_is_absorbed(void) {
    for (int arm = 0; arm < 2; arm++) {
        bool repair = arm == 1;
        reset();
        arrive(1, 100, false);
        uint32_t tx_size = node.tx_size;
        node.tx_size = node.tx_tail; // no room: the immediate ACKNACK is not encoded
        arrive(3, 100, false);
        node.tx_size = tx_size;
        struct tt_WriterProxy* proxy = proxy_of();
        EXPECT_TRUE(proxy != NULL);
        EXPECT_EQ_U64(0, oldest_request_naming(proxy, 2)); // the precondition: 2 was never asked for
        arrive(2, 50, repair);

        struct tt_ReliableStats stats;
        tt_reliable_stats_get(&stats);
        EXPECT_EQ_U32(1, (uint32_t)stats.recovered);
        EXPECT_EQ_U32(repair ? 0U : 1U, (uint32_t)stats.reorder_fill_original);
        EXPECT_EQ_U32(0, (uint32_t)stats.nack_spurious);
        EXPECT_EQ_U32(repair ? 0U : 1U, (uint32_t)stats.reorder_absorbed);
        EXPECT_EQ_U32(repair ? 0U : 1U, (uint32_t)stats.reorder_fill_before_rtt);
        EXPECT_EQ_U64(repair ? 0U : 1U, stats.reorder_delay_hist[6]); // 50 us: [32, 64) us
    }
}

// A fill inside the window, not at the watermark, and once the reader has a recovery estimate. 10, 13 (gap 11-12,
// NACKed), then 12's original, one datagram behind 13, then 11's, two behind. Neither counts as before the first RTT
// sample.
static void test_a_fill_inside_the_window_with_an_estimate(void) {
    reset();
    arrive(10, 100, false);
    struct tt_WriterProxy* proxy = proxy_of();
    EXPECT_TRUE(proxy != NULL);
    proxy->recovery_srtt_ns = 1000000U; // as if one recovery had been timed
    arrive(13, 100, false);
    arrive(12, 100, false);
    arrive(11, 100, false);

    struct tt_ReliableStats stats;
    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(2, (uint32_t)stats.recovered);
    EXPECT_EQ_U32(2, (uint32_t)stats.reorder_fill_original);
    EXPECT_EQ_U32(2, (uint32_t)stats.nack_spurious);
    EXPECT_EQ_U32(0, (uint32_t)stats.reorder_absorbed);
    EXPECT_EQ_U32(0, (uint32_t)stats.reorder_fill_before_rtt);
    EXPECT_EQ_U64(1, stats.reorder_distance_hist[1]); // 12: one behind 13
    EXPECT_EQ_U64(1, stats.reorder_distance_hist[2]); // 11: two behind 13, [2, 4)
    EXPECT_EQ_U64(2, hist_total(stats.reorder_delay_hist));
}

// A datagram covering several seq_nos (a large sample's run: tt_Context.rx_seq_span) fills each missing one it covers,
// counted per seq_no as its repairs would come back. 20, 25 (gap 21-24), then 21's original spanning 21-23: three
// fills, 4, 3 and 2 seq_nos behind 25 - 1, 1 and 0 datagrams of 3. 24 stays missing.
static void test_a_spanning_original_counts_each_seq_no_it_fills(void) {
    reset();
    arrive(20, 100, false);
    arrive(25, 100, false);
    node.rx_seq_span = 3;
    arrive(21, 100, false);
    node.rx_seq_span = 1;

    struct tt_ReliableStats stats;
    tt_reliable_stats_get(&stats);
    EXPECT_EQ_U32(3, (uint32_t)stats.reorder_fill_original);
    EXPECT_EQ_U32(3, (uint32_t)stats.nack_spurious);
    EXPECT_EQ_U64(1, stats.reorder_distance_hist[0]);
    EXPECT_EQ_U64(2, stats.reorder_distance_hist[1]);
    EXPECT_EQ_U64(3, hist_total(stats.reorder_delay_hist));
    struct tt_WriterProxy* proxy = proxy_of();
    EXPECT_TRUE(proxy != NULL);
    EXPECT_EQ_U32(24, proxy->ack_seq_no); // the span was recorded: only 24 is still missing
}

int main(void) {
    test_a_late_original_after_its_nack_is_a_spurious_nack();
    test_a_late_original_before_any_nack_is_absorbed();
    test_a_fill_inside_the_window_with_an_estimate();
    test_a_spanning_original_counts_each_seq_no_it_fills();

    printf("test_reorder_fill_stats: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
