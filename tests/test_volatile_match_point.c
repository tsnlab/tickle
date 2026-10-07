/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The writer owns the match point (2026-10-07). A RELIABLE KEEP_ALL writer under 5% loss lost its FIRST sample in ~4%
// of runs (p1, `first_seq=2 prematch_window=1`): seq 1 was lost on the wire, the reader's writer proxy was created by
// DATA seq 2, the reader pinned its baseline there, never asked for 1, and its next ACKNACK released 1 at the writer.
// The reader cannot tell a sample written before the writer matched it from one written after and lost; the writer
// can, and now puts a Heartbeat naming the owed sample ahead of the DATA of its first publishes after a match
// (tt_PeerAck.first_owed_seq_no). Two nodes exchange real announces here; every datagram but those a test drops is
// delivered, and a Heartbeat travelling alone is dropped until the reader has met the writer:
// - seq 1 dropped after the match: the reader still gets 1..5 in order, and until it has 1 the writer never counts 1
//   as acknowledged by it (on main: 2..5, and the writer's ack for it reads 2);
// - samples written before the reader existed are never delivered to it (VOLATILE): with nothing lost; with the
//   discovery-time Heartbeat lost; and with that and the first owed sample lost, which is still delivered (on main
//   it was not);
// - once the reader has acknowledged the owed sample, a publish carries no Heartbeat.
// Mutants, each killed here: no match Heartbeat; last_seq_no naming the owed sample rather than the one before it;
// the Heartbeat naming the oldest retained sample instead of the owed one; the Heartbeat always in its own datagram;
// the reader's ack not ending it.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define WRITER_ID 1
#define READER_ID 2
#define QUEUE 128
#define DEPTH 64
#define REORDER_SLOTS 64
#define REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + 64)
#define MAX_DELIVERED 64

struct datagram {
    uint8_t from;
    bool unicast;
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
};
static struct datagram queue[QUEUE];
static int queued;
static uint8_t acting;
static int seen_send_to;
static bool (*drop)(const struct datagram* datagram);
// Set while seq 1 has not reached the reader: the writer must not count it as acknowledged by it meanwhile.
static bool check_owed_ack;
static bool owed_delivered(void);
static struct tt_Publisher pub;
static int heartbeats_with_data; // writer datagrams that carried a Heartbeat ahead of a DATA
static bool carries_heartbeat_and_data(const struct datagram* datagram);

static void capture(const void* buf, size_t len) {
    EXPECT_TRUE(queued < QUEUE && len <= tt_MAX_BUFFER_LENGTH);
    if (queued >= QUEUE || len > tt_MAX_BUFFER_LENGTH) {
        return;
    }
    struct datagram* datagram = &queue[queued++];
    datagram->from = acting;
    datagram->unicast = test_mock_send_to_call_count != seen_send_to; // tt_send_to() counted it first
    seen_send_to = test_mock_send_to_call_count;
    datagram->len = (uint32_t)len;
    memcpy(datagram->bytes, buf, len);
}

static void run_due(struct tt_Context* node) {
    bool has_next = false;
    uint64_t next = 0;
    acting = node->id;
    while (true) {
        uint64_t head = UINT64_MAX;
        if (!sched_next_time(node, &head) || head > test_mock_now) {
            return;
        }
        (void)run_due_entry(node, test_mock_now, &has_next, &next);
    }
}

static void deliver(struct tt_Context* one, struct tt_Context* two) {
    for (int i = 0; i < queued; i++) { // grows while delivering: a reply is delivered in the same pass
        struct datagram* datagram = &queue[i];
        if (datagram->from == WRITER_ID && carries_heartbeat_and_data(datagram)) {
            heartbeats_with_data++;
        }
        if (drop != NULL && drop(datagram)) {
            continue;
        }
        struct tt_Context* to = datagram->from == one->id ? two : one;
        acting = to->id;
        memcpy(to->rx_buffer, datagram->bytes, datagram->len);
        to->rx_via_data_port = datagram->unicast;
        EXPECT_TRUE(
            process_packet(to, to->rx_buffer, 0, datagram->len, 0x0a000000U + datagram->from, 8282, tt_TRANSPORT_UDP));
        if (check_owed_ack && to->id == WRITER_ID && !owed_delivered()) {
            EXPECT_TRUE(tt_Publisher_min_acked_seq_no(&pub) <= 1);
        }
    }
    queued = 0;
}

static void run_until(struct tt_Context* one, struct tt_Context* two, uint64_t until) {
    while (true) {
        uint64_t next_one = UINT64_MAX;
        uint64_t next_two = UINT64_MAX;
        (void)sched_next_time(one, &next_one);
        (void)sched_next_time(two, &next_two);
        uint64_t next = next_one < next_two ? next_one : next_two;
        if (next > until) {
            test_mock_now = until;
            deliver(one, two);
            return;
        }
        if (next > test_mock_now) {
            test_mock_now = next;
        }
        run_due(one);
        run_due(two);
        deliver(one, two);
    }
}

// --- what a datagram carries -----------------------------------------------------------------------------------

// Calls visit for every submessage of the topic's endpoint in the datagram (classic form, test_classic_form()).
static uint32_t topic_endpoint_id;
static bool any_submessage(const struct datagram* datagram, bool (*match)(uint8_t type, const uint8_t* body)) {
    static uint8_t classic[tt_MAX_BUFFER_LENGTH + sizeof(struct tt_Header)];
    uint32_t classic_len = (uint32_t)test_classic_form(datagram->bytes, datagram->len, classic);
    uint32_t head = sizeof(struct tt_Header);
    while (head + sizeof(struct tt_SubmessageHeader) + sizeof(uint32_t) <= classic_len) {
        struct tt_SubmessageHeader sub;
        memcpy(&sub, &classic[head], sizeof(sub));
        uint32_t endpoint_id = 0;
        memcpy(&endpoint_id, &classic[head + sizeof(sub)], sizeof(endpoint_id));
        if (endpoint_id == topic_endpoint_id && match(sub.type, &classic[head + sizeof(sub)])) {
            return true;
        }
        if (sub.length < sizeof(sub)) {
            return false;
        }
        head += sub.length;
    }
    return false;
}

static uint32_t drop_data_seq_no; // 0: none
static bool is_dropped_data(uint8_t type, const uint8_t* body) {
    struct tt_DataHeader data;
    memcpy(&data, body, sizeof(data));
    return type == tt_SUBMESSAGE_TYPE_DATA && data.seq_no == drop_data_seq_no;
}
static bool is_heartbeat(uint8_t type, const uint8_t* body) {
    (void)body;
    return type == tt_SUBMESSAGE_TYPE_HEARTBEAT;
}

static bool is_data(uint8_t type, const uint8_t* body) {
    (void)body;
    return type == tt_SUBMESSAGE_TYPE_DATA;
}

// The datagram carrying DATA drop_data_seq_no, once: its retransmission goes through.
static bool drop_the_data(const struct datagram* datagram) {
    if (drop_data_seq_no == 0 || datagram->from != WRITER_ID || !any_submessage(datagram, is_dropped_data)) {
        return false;
    }
    drop_data_seq_no = 0;
    return true;
}

// That, and every datagram of the topic carrying a Heartbeat and no DATA until the reader has met the writer: the
// match Heartbeat sent on discovery is lost, and the reader's first contact is a publish.
static struct tt_Subscriber sub;
static bool drop_lone_heartbeats_and_the_data(const struct datagram* datagram) {
    if (drop_the_data(datagram)) {
        return true;
    }
    if (datagram->from != WRITER_ID) {
        return false;
    }
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (sub.writers[i].context_id == WRITER_ID) {
            return false; // the reader has met the writer: Heartbeats go through from here on
        }
    }
    return any_submessage(datagram, is_heartbeat) && !any_submessage(datagram, is_data);
}

static bool carries_heartbeat_and_data(const struct datagram* datagram) {
    return any_submessage(datagram, is_heartbeat) && any_submessage(datagram, is_data);
}

// --- the two nodes ---------------------------------------------------------------------------------------------

static uint32_t delivered[MAX_DELIVERED];
static int delivered_count;

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return 4;
}
static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < 4) {
        return -1;
    }
    memcpy(payload, data, 4);
    return 4;
}
static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native) {
    (void)is_native;
    if (len < 4) {
        return -1;
    }
    memcpy(data, payload, 4);
    return 4;
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}
static void on_data(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    if (delivered_count < MAX_DELIVERED) {
        delivered[delivered_count] = value;
    }
    delivered_count++;
}

static bool owed_delivered(void) {
    for (int i = 0; i < delivered_count && i < MAX_DELIVERED; i++) {
        if (delivered[i] == 1) {
            return true;
        }
    }
    return false;
}

static struct tt_Context writer;
static struct tt_Context reader;
static struct tt_Topic writer_topic = {.name = "match_point",
                                       .data_size = 4,
                                       .data_encode_size = sample_encode_size,
                                       .data_encode = sample_encode};
static struct tt_Topic reader_topic = {.name = "match_point",
                                       .data_size = 4,
                                       .data_decode = sample_decode,
                                       .data_free = free_nothing};
static struct tt_ReliableCacheIndex cache_index[DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(DEPTH, tt_RELIABLE_RECORD_BYTES(4))];
static struct tt_ReliableCache cache;
static uint64_t reorder_storage[REORDER_SLOTS * REORDER_SLOT_BYTES / sizeof(uint64_t)];

static void init_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

static void start(void) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    test_mock_send_hook = capture;
    queued = 0;
    seen_send_to = 0;
    drop = NULL;
    drop_data_seq_no = 0;
    check_owed_ack = false;
    heartbeats_with_data = 0;
    delivered_count = 0;
    memset(delivered, 0, sizeof(delivered));
    init_node(&writer, WRITER_ID);
    init_node(&reader, READER_ID);
    EXPECT_EQ_INT(tt_RET_OK, schedule_periodic_tasks(&writer));
    EXPECT_EQ_INT(tt_RET_OK, schedule_periodic_tasks(&reader));

    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&writer, &pub, &writer_topic, "match_point_ep"));
    memset(&cache, 0, sizeof(cache));
    EXPECT_EQ_INT(tt_RET_OK, tt_ReliableCache_init(&cache, cache_index, DEPTH, cache_arena, sizeof(cache_arena)));
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.keep_all = true;
    topic_endpoint_id = pub.endpoint.id;
}

static void create_reader(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&reader, &sub, &reader_topic, "match_point_ep", on_data));
    sub.reliable = true;
    memset(reorder_storage, 0, sizeof(reorder_storage));
    sub.reorder_storage = reorder_storage;
    sub.reorder_slots = REORDER_SLOTS;
    sub.reorder_slot_bytes = REORDER_SLOT_BYTES;
}

static void publish(uint32_t from, uint32_t to) {
    for (uint32_t value = from; value <= to; value++) {
        acting = WRITER_ID;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
        deliver(&writer, &reader);
    }
}

static void stop(void) {
    drop = NULL;
    test_mock_send_hook = NULL;
}

static void expect_delivered(uint32_t first, uint32_t last) {
    EXPECT_EQ_INT((int)(last - first + 1), delivered_count);
    for (uint32_t value = first; value <= last && (int)(value - first) < delivered_count; value++) {
        EXPECT_EQ_U32(value, delivered[value - first]);
    }
}

// The writer matched the reader, then seq 1 was lost: the reader's first contact is DATA 2.
static void test_first_sample_lost_after_the_match_is_still_delivered(void) {
    start();
    create_reader();
    run_until(&writer, &reader, test_mock_now + (3 * tt_SECOND)); // both announced and matched
    EXPECT_TRUE(any_peer_ack_matched(&pub)); // control: the writer matched the reader before writing
    EXPECT_EQ_U32(0, pub.seq_no);

    drop_data_seq_no = 1;
    drop = drop_lone_heartbeats_and_the_data; // and any Heartbeat that would travel alone
    check_owed_ack = true;
    publish(1, 2);
    run_until(&writer, &reader, test_mock_now + (2 * tt_MILLISECOND)); // the reader acknowledges what it has
    publish(3, 5);
    run_until(&writer, &reader, test_mock_now + (200 * tt_MILLISECOND));
    expect_delivered(1, 5);
    acting = WRITER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_request_ack(&pub));
    run_until(&writer, &reader, test_mock_now + (5 * tt_MILLISECOND));
    EXPECT_EQ_U32(6, tt_Publisher_min_acked_seq_no(&pub)); // control: acks were counted, the invariant not vacuous
    EXPECT_EQ_U32(0, sub.gap_evicted);
    stop();
}

// Samples written before the reader existed are retained (KEEP_ALL, its pre-match arena) and must never reach it.
static void prematch_then_reader(bool (*drop_rule)(const struct datagram*), uint32_t dropped) {
    start();
    run_until(&writer, &reader, test_mock_now + (2 * tt_SECOND));
    publish(1, 3); // nobody to send to yet
    EXPECT_TRUE(!any_peer_ack_matched(&pub));
    EXPECT_EQ_U32(1, cache.oldest_seq_no); // control: they are retained, so they could be resent

    drop_data_seq_no = dropped;
    drop = drop_rule;
    create_reader();
    run_until(&writer, &reader, test_mock_now + (3 * tt_SECOND));
    EXPECT_TRUE(any_peer_ack_matched(&pub));
    publish(4, 6);
    run_until(&writer, &reader, test_mock_now + (200 * tt_MILLISECOND));
}

static void test_prematch_samples_never_reach_a_volatile_reader(void) {
    prematch_then_reader(NULL, 0);
    expect_delivered(4, 6);
    EXPECT_TRUE(heartbeats_with_data > 0); // control: the match Heartbeat rode ahead of the DATA

    // Once the reader has acknowledged the owed sample, a publish carries no Heartbeat any more: the cost is the
    // first few datagrams after a match, not the stream.
    acting = WRITER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_request_ack(&pub));
    run_until(&writer, &reader, test_mock_now + (5 * tt_MILLISECOND));
    EXPECT_EQ_U32(7, tt_Publisher_min_acked_seq_no(&pub));
    int before = heartbeats_with_data;
    publish(7, 7);
    EXPECT_EQ_INT(before, heartbeats_with_data);
    expect_delivered(4, 7);
    stop();
}

static void test_prematch_samples_never_reach_it_when_its_first_contact_is_a_later_data(void) {
    prematch_then_reader(drop_lone_heartbeats_and_the_data, 4);
    expect_delivered(4, 6); // 4 is owed: asked for and resent; 1..3 are not
    EXPECT_EQ_U32(0, sub.gap_evicted);
    stop();
}

static void test_prematch_samples_never_reach_it_when_the_match_heartbeat_is_lost(void) {
    prematch_then_reader(drop_lone_heartbeats_and_the_data, 0);
    expect_delivered(4, 6);
    EXPECT_EQ_U32(0, sub.gap_evicted);
    stop();
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_first_sample_lost_after_the_match_is_still_delivered();
    test_prematch_samples_never_reach_a_volatile_reader();
    test_prematch_samples_never_reach_it_when_its_first_contact_is_a_later_data();
    test_prematch_samples_never_reach_it_when_the_match_heartbeat_is_lost();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_volatile_match_point: all tests passed\n");
    return 0;
}
