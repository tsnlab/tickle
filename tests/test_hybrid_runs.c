/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Hybrid transport, step 2 (DESIGN.md section 8, "Hybrid transport"): receiving runs - FRAG_RUN (a sample on 8/9) and
// FRAG_RUN_L (a large sample on 11/12). The sender does not produce runs yet, so every run here is
// hand-built from the real MTU fragments a Publisher sent: its payload is those fragments' payloads concatenated, its
// header the sample's DataHeader with the first unit's seq_no and the run's index, count and run_count. That way a run
// carries exactly the bytes the units would have, and a delivered sample is checked byte for byte against the pattern
// it was encoded with.
//
// Covered: a run then the MTU tail; the tail first; runs out of order; a run overlapping units already stored; part of
// a run repaired by MTU units; the ACK bitmap marking each unit of a run; best effort and RELIABLE on both type pairs;
// a large run placed in the lendable assembly buffer itself. And a table of malformed runs, one validation condition
// each, every one refused whole and counted once, with nothing stored or recorded (tests/mutants_hybrid_transport.py
// removes each condition and requires this binary to fail).
//
// Built as rmw_tickle builds core: a 65507-byte datagram, large samples.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: process_packet() is static
#include "test_mock.h"

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define PORT 8282
#define TOPIC_NAME "hybrid_topic"
#define ENDPOINT_NAME "hybrid_endpoint"

// 6 units on 8/9 (1,447 + 4 x 1,454 + a short tail), and 69 on 11/12 (1,446 + 67 x 1,452 + a short tail).
#define SMALL_LEN 8000U
#define SMALL_UNITS 6U
#define LARGE_LEN 100000U
#define LARGE_UNITS 69U

// --- the receiver's large buffers, recorded so a delivery can be checked to lie inside one ---------------------------

#define LIVE_MAX 16
static uint8_t* live_buffers[LIVE_MAX];
static uint32_t live_sizes[LIVE_MAX];

static void* rx_acquire(void* user, uint32_t bytes) {
    (void)user;
    uint8_t* buffer = (uint8_t*)malloc(bytes);
    for (int i = 0; buffer != NULL && i < LIVE_MAX; i++) {
        if (live_buffers[i] == NULL) {
            live_buffers[i] = buffer;
            live_sizes[i] = bytes;
            break;
        }
    }
    return buffer;
}

static void rx_release(void* user, void* buffer) {
    (void)user;
    for (int i = 0; i < LIVE_MAX; i++) {
        if (live_buffers[i] == buffer) {
            live_buffers[i] = NULL;
        }
    }
    free(buffer);
}

static void* tx_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void tx_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}

static bool in_live_buffer(const uint8_t* p, uint32_t len) {
    for (int i = 0; i < LIVE_MAX; i++) {
        if (live_buffers[i] != NULL && p >= live_buffers[i] && p + len <= live_buffers[i] + live_sizes[i]) {
            return true;
        }
    }
    return false;
}

// --- the sample ------------------------------------------------------------------------------------------------------

static uint32_t sample_len;

static uint8_t pattern(uint32_t i) {
    return (uint8_t)((i * 7U) + 3U + (i >> 12U));
}

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = pattern(i);
    }
    return (int32_t)len;
}

static int delivered;
static int delivered_torn;
static int delivered_in_assembly;

static int32_t checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    bool intact = len == ROUNDUP(sample_len);
    for (uint32_t i = 0; intact && i < sample_len; i++) {
        intact = payload[i] == pattern(i);
    }
    delivered_torn += intact ? 0 : 1;
    delivered_in_assembly += in_live_buffer(payload, len) ? 1 : 0;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    (void)data;
    delivered++;
}

// --- the two contexts ------------------------------------------------------------------------------------------------

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
#define WIDE_WORDS (tt_RELIABLE_BITMAP_MAX_BITS / 64)
static uint64_t tracking[tt_MAX_PEER_COUNT * WIDE_WORDS];
#define REORDER_SLOTS 64
#define REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(1472)
static uint64_t reorder[REORDER_SLOTS * REORDER_SLOT_BYTES / sizeof(uint64_t)];

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

// --- capture: the MTU fragments one publish sends, parsed into units -------------------------------------------------

#define UNITS_MAX 80
#define DATAGRAM_MAX 1500
static uint8_t datagrams[UNITS_MAX][DATAGRAM_MAX];
static uint32_t datagram_len[UNITS_MAX];
static uint32_t datagram_count;
// Per unit: where its fragment header and its payload start in its datagram.
static uint32_t unit_payload_at[UNITS_MAX];
static struct tt_DataHeader sample_header; // fragment 0's

static void capture_hook(const void* buf, size_t len) {
    if (datagram_count == UNITS_MAX || len > DATAGRAM_MAX) {
        test_failures++;
        return;
    }
    memcpy(datagrams[datagram_count], buf, len);
    datagram_len[datagram_count++] = (uint32_t)len;
}

static uint32_t body_at(const uint8_t* bytes, uint8_t* type) {
    bool single = bytes[0] == tt_SINGLE_MARKER_LE || bytes[0] == tt_SINGLE_MARKER_BE;
    *type = single ? bytes[3] : bytes[sizeof(struct tt_Header)];
    return single ? (uint32_t)sizeof(struct tt_SingleHeader)
                  : (uint32_t)(sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
}

// A Publisher and a Subscriber, one sample published as MTU fragments and captured, nothing delivered yet.
static void init_pair(uint32_t len, bool reliable) {
    test_mock_reset();
    for (int i = 0; i < LIVE_MAX; i++) {
        if (live_buffers[i] != NULL) {
            free(live_buffers[i]);
            live_buffers[i] = NULL;
        }
    }
    sample_len = len;
    delivered = 0;
    delivered_torn = 0;
    delivered_in_assembly = 0;

    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = 8;
    sender_topic.data_encode_size = sized_encode_size;
    sender_topic.data_encode = sized_encode;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&sender, &pub, &sender_topic, ENDPOINT_NAME));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&sender, tx_acquire, tx_release, NULL));

    init_bare_node(&receiver, RECEIVER_ID);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = 8;
    receiver_topic.data_decode = checking_decode;
    receiver_topic.data_free = free_nothing;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&receiver, &sub, &receiver_topic, ENDPOINT_NAME, on_data));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&receiver, rx_acquire, rx_release, NULL));
    if (reliable) {
        sub.reliable = true;
        memset(tracking, 0, sizeof(tracking));
        sub.tracking_bitmaps = tracking;
        sub.tracking_words = WIDE_WORDS;
        memset(reorder, 0, sizeof(reorder));
        sub.reorder_storage = reorder;
        sub.reorder_slots = REORDER_SLOTS;
        sub.reorder_slot_bytes = REORDER_SLOT_BYTES;
    }

    // A Publisher with no matched reader sends nothing (2026-10-15): known peers past the unicast threshold keep the
    // broadcast this capture is about.
    for (uint8_t i = 0; i <= tt_UNICAST_PEER_THRESHOLD; i++) {
        (void)upsert_peer(pub.peers, (uint8_t)(100U + i), 0x0a000100U + i, 8282);
    }
    datagram_count = 0;
    test_mock_send_hook = capture_hook;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    test_mock_send_hook = NULL;
    for (uint32_t i = 0; i < datagram_count; i++) {
        uint8_t type = 0;
        uint32_t body = body_at(datagrams[i], &type);
        if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST || type == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L) {
            memcpy(&sample_header, datagrams[i] + body, sizeof(sample_header));
        }
        uint32_t header = (uint32_t)sizeof(struct tt_FragContLHeader);
        if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST) {
            header = (uint32_t)sizeof(struct tt_FragFirstHeader);
        } else if (type == tt_SUBMESSAGE_TYPE_FRAG_CONT) {
            header = (uint32_t)sizeof(struct tt_FragContHeader);
        } else if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L) {
            header = (uint32_t)sizeof(struct tt_FragFirstLHeader);
        }
        unit_payload_at[i] = body + header;
    }
}

static uint32_t unit_length(uint32_t i) {
    return datagram_len[i] - unit_payload_at[i];
}

// --- runs ------------------------------------------------------------------------------------------------------------

// What a malformed row changes in an otherwise valid run; the zero value changes nothing.
struct run_edit {
    bool set_zero;
    uint16_t zero;
    bool set_entity;
    uint32_t entity_id;
    bool set_count;
    uint32_t count;
    bool set_run_count;
    uint32_t run_count;
    bool set_seq;
    uint32_t seq_no;
    int32_t length_delta;  // bytes added to (or, negative, cut from) the payload
    bool header_only_part; // the datagram ends inside the run header
};

static uint8_t run_bytes[70000];

// Units first .. first + n - 1 of the captured sample as one run of the sample's type pair, with `edit` applied.
static uint32_t build_run(uint32_t first, uint32_t n, const struct run_edit* edit) {
    bool large = sample_len > tt_MAX_SAMPLE_LENGTH;
    uint16_t magic = NATIVE_MAGIC_VALUE;
    uint8_t marker = 0;
    memcpy(&marker, &magic, 1);
    struct tt_SingleHeader single = {(uint8_t)(marker | tt_SINGLE_MARKER_FLAG), tt_VERSION, SENDER_ID,
                                     large ? (uint8_t)tt_SUBMESSAGE_TYPE_FRAG_RUN_L
                                           : (uint8_t)tt_SUBMESSAGE_TYPE_FRAG_RUN};
    memcpy(run_bytes, &single, sizeof(single));
    uint32_t at = sizeof(single);
    struct tt_DataHeader data = sample_header;
    data.seq_no = edit->set_seq ? edit->seq_no : sample_header.seq_no + first;
    if (edit->set_entity) {
        data.entity_id = edit->entity_id;
    }
    uint32_t count = edit->set_count ? edit->count : datagram_count;
    uint32_t run_count = edit->set_run_count ? edit->run_count : n;
    uint16_t zero = edit->set_zero ? edit->zero : 0;
    if (large) {
        struct tt_FragRunLHeader header = {data, (uint16_t)first, (uint16_t)count, (uint16_t)run_count, zero};
        memcpy(run_bytes + at, &header, sizeof(header));
        at += sizeof(header);
    } else {
        struct tt_FragRunHeader header = {data, (uint8_t)first, (uint8_t)count, (uint8_t)run_count, (uint8_t)zero};
        memcpy(run_bytes + at, &header, sizeof(header));
        at += sizeof(header);
    }
    if (edit->header_only_part) {
        return at - 3;
    }
    for (uint32_t i = first; i < first + n; i++) {
        memcpy(run_bytes + at, datagrams[i] + unit_payload_at[i], unit_length(i));
        at += unit_length(i);
    }
    if (edit->length_delta > 0) {
        memset(run_bytes + at, 0x5A, (size_t)edit->length_delta);
    }
    return (uint32_t)((int32_t)at + edit->length_delta);
}

static const struct run_edit NO_EDIT = {0};

static void feed(const uint8_t* bytes, uint32_t len) {
    static uint8_t copy[70000];
    memcpy(copy, bytes, len); // process_packet() may swap in place; the source is reused
    (void)process_packet(&receiver, copy, 0, len, SENDER_IP, PORT, tt_TRANSPORT_UDP);
}

static void feed_run(uint32_t first, uint32_t n) {
    feed(run_bytes, build_run(first, n, &NO_EDIT));
}

static void feed_unit(uint32_t i) {
    feed(datagrams[i], datagram_len[i]);
}

static struct tt_WriterProxy* proxy(void) {
    return find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
}

// One delivery, whole and intact, and (RELIABLE) the watermark past all of the sample's units.
static void expect_one_intact(bool reliable, uint32_t units) {
    EXPECT_EQ_INT(1, delivered);
    EXPECT_EQ_INT(0, delivered_torn);
    if (reliable) {
        struct tt_WriterProxy* p = proxy();
        EXPECT_TRUE(p != NULL);
        if (p != NULL) {
            EXPECT_EQ_U32(sample_header.seq_no + units, p->ack_seq_no);
        }
    }
}

// --- FRAG_RUN --------------------------------------------------------------------------------------------------------

static void test_small_geometry(void) {
    init_pair(SMALL_LEN, true);
    EXPECT_EQ_U32(SMALL_UNITS, datagram_count);
    EXPECT_EQ_U32(1447, unit_length(0));
    EXPECT_EQ_U32(1454, unit_length(1));
}

static void test_small_run_then_tail(bool reliable) {
    init_pair(SMALL_LEN, reliable);
    feed_run(0, SMALL_UNITS - 1);
    EXPECT_EQ_INT(0, delivered);
    feed_unit(SMALL_UNITS - 1);
    expect_one_intact(reliable, SMALL_UNITS);
}

static void test_small_tail_first(bool reliable) {
    init_pair(SMALL_LEN, reliable);
    feed_unit(0); // RELIABLE: first contact is unit 0 - a continuation before it is asked for again
    feed_unit(SMALL_UNITS - 1);
    feed_run(1, SMALL_UNITS - 2);
    expect_one_intact(reliable, SMALL_UNITS);
}

// Two runs out of order, then the tail.
static void test_small_runs_out_of_order(bool reliable) {
    init_pair(SMALL_LEN, reliable);
    feed_run(0, 2);
    feed_run(3, 2);
    feed_run(2, 1);
    EXPECT_EQ_INT(0, delivered);
    feed_unit(SMALL_UNITS - 1);
    expect_one_intact(reliable, SMALL_UNITS);
}

// Units 0, 1 and 3 already stored when a run covering 0..4 arrives: the run fills the rest, and RELIABLE counts the
// three as duplicates.
static void test_small_overlap(bool reliable) {
    init_pair(SMALL_LEN, reliable);
    feed_unit(0);
    feed_unit(1);
    feed_unit(3);
    uint64_t duplicates = receiver.frag_duplicate;
    feed_run(0, SMALL_UNITS - 1);
    if (reliable) {
        EXPECT_EQ_U64(duplicates + 3, receiver.frag_duplicate); // the pool takes a duplicate silently
    }
    feed_unit(SMALL_UNITS - 1);
    expect_one_intact(reliable, SMALL_UNITS);
    EXPECT_EQ_U64(0, receiver.frag_dropped);
}

// A run lost, its units repaired as MTU fragments, part of them overlapping a later run.
static void test_small_run_repaired_by_mtu_units(void) {
    init_pair(SMALL_LEN, true);
    feed_run(0, 2);
    // run 2..4 lost; unit 3 repaired alone first, then the tail, then a re-sent run 2..4 that overlaps unit 3
    feed_unit(3);
    feed_unit(SMALL_UNITS - 1);
    EXPECT_EQ_INT(0, delivered);
    feed_run(2, 3);
    expect_one_intact(true, SMALL_UNITS);
}

// Every unit of a run is recorded in the ACK bitmap under its own seq_no, and none outside it.
static void test_small_bitmap_marks_each_unit(void) {
    init_pair(SMALL_LEN, true);
    feed_unit(0);
    struct tt_WriterProxy* p = proxy();
    EXPECT_TRUE(p != NULL);
    if (p == NULL) {
        return;
    }
    EXPECT_EQ_U32(sample_header.seq_no + 1, p->ack_seq_no);
    feed_run(2, 3); // units 2, 3, 4: offsets 1, 2, 3 from the watermark (unit 1)
    EXPECT_TRUE(!bitmap_test_bit(p->received_bitmap, 0));
    EXPECT_TRUE(bitmap_test_bit(p->received_bitmap, 1));
    EXPECT_TRUE(bitmap_test_bit(p->received_bitmap, 2));
    EXPECT_TRUE(bitmap_test_bit(p->received_bitmap, 3));
    EXPECT_TRUE(!bitmap_test_bit(p->received_bitmap, 4)); // the tail
    feed_unit(1);
    feed_unit(SMALL_UNITS - 1);
    expect_one_intact(true, SMALL_UNITS);
}

// A run that starts mid-sample, before the writer is tracked, is not first contact: only unit 0 can be (its DataHeader
// is what a run passes on for unit 0 alone). Left unrecorded, so asked for again; the sample then arrives whole.
static void test_small_mid_sample_run_is_not_first_contact(void) {
    init_pair(SMALL_LEN, true);
    feed_run(2, 3);
    EXPECT_TRUE(proxy() == NULL);
    EXPECT_EQ_U32(0, sub.reorder_held);
    for (uint32_t i = 0; i < SMALL_UNITS; i++) {
        feed_unit(i);
    }
    expect_one_intact(true, SMALL_UNITS);
}

static void test_large_mid_sample_run_is_not_first_contact(void) {
    init_pair(LARGE_LEN, true);
    feed_run(20, 10);
    EXPECT_TRUE(proxy() == NULL);
    for (uint32_t i = 0; i < LARGE_UNITS; i++) {
        feed_unit(i);
    }
    expect_one_intact(true, LARGE_UNITS);
}

// --- FRAG_RUN_L ------------------------------------------------------------------------------------------------------

static void test_large_geometry(void) {
    init_pair(LARGE_LEN, true);
    EXPECT_EQ_U32(LARGE_UNITS, datagram_count);
    EXPECT_EQ_U32(LARGE_FIRST_PAYLOAD, unit_length(0));
    EXPECT_EQ_U32(LARGE_CONT_PAYLOAD, unit_length(1));
}

static void test_large_runs_then_tail(bool reliable) {
    init_pair(LARGE_LEN, reliable);
    feed_run(0, 45);
    feed_run(45, LARGE_UNITS - 46);
    EXPECT_EQ_INT(0, delivered);
    feed_unit(LARGE_UNITS - 1);
    expect_one_intact(reliable, LARGE_UNITS);
    // Placed in the assembly buffer core acquired, and delivered from there: no staging copy.
    EXPECT_EQ_INT(1, delivered_in_assembly);
}

static void test_large_out_of_order_and_overlap(bool reliable) {
    init_pair(LARGE_LEN, reliable);
    feed_unit(0);
    feed_unit(LARGE_UNITS - 1);
    feed_unit(50);
    feed_run(45, LARGE_UNITS - 46); // overlaps unit 50
    feed_run(1, 44);
    expect_one_intact(reliable, LARGE_UNITS);
    EXPECT_EQ_INT(1, delivered_in_assembly);
    EXPECT_EQ_U64(0, receiver.large.dropped);
}

static void test_large_bitmap_marks_each_unit(void) {
    init_pair(LARGE_LEN, true);
    feed_unit(0);
    struct tt_WriterProxy* p = proxy();
    EXPECT_TRUE(p != NULL);
    if (p == NULL) {
        return;
    }
    feed_run(10, 20); // units 10 .. 29: offsets 9 .. 28 from the watermark (unit 1)
    for (uint32_t offset = 0; offset < 40; offset++) {
        EXPECT_EQ_INT(offset >= 9 && offset <= 28, bitmap_test_bit(p->received_bitmap, offset));
    }
}

// --- malformed runs --------------------------------------------------------------------------------------------------

struct malformed_row {
    const char* name;
    uint32_t first;
    uint32_t n;
    struct run_edit edit;
};

// Each row breaks one condition of an otherwise valid run of units first .. first + n - 1 (unit 0 delivered before, so
// the writer is tracked and a valid run there would be stored). Refused whole: counted once, nothing stored, nothing
// recorded; and the stream carries on - the valid units then deliver the sample.
static void run_malformed(bool large, const struct malformed_row* row) {
    init_pair(large ? LARGE_LEN : SMALL_LEN, true);
    uint32_t units = large ? LARGE_UNITS : SMALL_UNITS;
    feed_unit(0);
    uint64_t* dropped = large ? &receiver.large.dropped : &receiver.frag_dropped;
    uint64_t before = *dropped;
    uint32_t held = sub.reorder_held;
    uint32_t landed = 0;
    for (uint32_t i = 0; i < tt_LARGE_ASSEMBLIES; i++) {
        landed += receiver.large.assemblies[i].received;
    }
    feed(run_bytes, build_run(row->first, row->n, &row->edit));
    if (*dropped != before + 1) {
        printf("malformed %s run '%s': dropped %lu, expected 1\n", large ? "large" : "small", row->name,
               (unsigned long)(*dropped - before));
    }
    EXPECT_EQ_U64(before + 1, *dropped);
    struct tt_WriterProxy* p = proxy();
    EXPECT_TRUE(p != NULL);
    if (p != NULL) {
        EXPECT_EQ_U32(sample_header.seq_no + 1, p->ack_seq_no);
        for (uint32_t offset = 0; offset < units + 4; offset++) {
            if (bitmap_test_bit(p->received_bitmap, offset)) {
                printf("malformed %s run '%s': offset %u recorded\n", large ? "large" : "small", row->name, offset);
                EXPECT_TRUE(false);
                break;
            }
        }
    }
    EXPECT_EQ_U32(held, sub.reorder_held);
    uint32_t landed_after = 0;
    for (uint32_t i = 0; i < tt_LARGE_ASSEMBLIES; i++) {
        landed_after += receiver.large.assemblies[i].received;
    }
    EXPECT_EQ_U32(landed, landed_after);
    EXPECT_EQ_INT(0, delivered);
    // Control: the valid units still deliver it.
    for (uint32_t i = 1; i < units; i++) {
        feed_unit(i);
    }
    expect_one_intact(true, units);
}

static void test_malformed_runs(bool large) {
    uint32_t units = large ? LARGE_UNITS : SMALL_UNITS;
    uint32_t max_count = large ? tt_LARGE_MAX_FRAGMENTS : tt_FRAG_MAX_COUNT;
    uint32_t unit = large ? LARGE_CONT_PAYLOAD : 1454U;
    const struct malformed_row rows[] = {
        {"reserved field not zero", 1, 3, {.set_zero = true, .zero = 1}},
        {"discovery entity", 1, 3, {.set_entity = true, .entity_id = tt_DISCOVERY_ENTITY_ID}},
        {"count above the type's ceiling", 1, 3, {.set_count = true, .count = max_count + 1U}},
        {"run_count 0", 1, 0, {0}},
        {"holds the last unit", units - 3U, 3, {0}},
        {"runs past the count", units - 3U, 3, {.set_count = true, .count = units - 1U}},
        {"length one byte long", 1, 3, {.length_delta = 1}},
        {"length one byte short", 1, 3, {.length_delta = -1}},
        {"length one unit short of run_count", 1, 3, {.set_run_count = true, .run_count = 4}},
        {"units no longer than the first unit's shortfall",
         1,
         1,
         {.set_run_count = true, .run_count = 3, .length_delta = -(int32_t)unit + 15}},
        {"header cut short", 1, 3, {.header_only_part = true}},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        run_malformed(large, &rows[i]);
    }
}

// A run from this context itself is not received, as no fragment of its own is.
static void test_self_sent_run_ignored(void) {
    init_pair(SMALL_LEN, false);
    uint32_t len = build_run(0, SMALL_UNITS - 1, &NO_EDIT);
    run_bytes[2] = RECEIVER_ID; // tt_SingleHeader.source: the receiver's own id
    static uint8_t copy[70000];
    memcpy(copy, run_bytes, len);
    // From its own address: from any other, the id claim takes it for another context under the same id.
    (void)process_packet(&receiver, copy, 0, len, receiver.hal.own_ip, receiver.hal.own_port, tt_TRANSPORT_UDP);
    EXPECT_EQ_U64(1, receiver.rx_self_sent);
    EXPECT_EQ_U64(0, receiver.frag_reassembled);
    EXPECT_TRUE(!frag_pool_has(&receiver, RECEIVER_ID, pub.endpoint.entity_id, sample_header.seq_no));
    // Control: the same run from the sender is taken into the pool, so the check above can see one.
    feed_run(0, SMALL_UNITS - 1);
    EXPECT_TRUE(frag_pool_has(&receiver, SENDER_ID, pub.endpoint.entity_id, sample_header.seq_no));
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    test_small_geometry();
    test_small_run_then_tail(true);
    test_small_run_then_tail(false);
    test_small_tail_first(true);
    test_small_tail_first(false);
    test_small_runs_out_of_order(true);
    test_small_runs_out_of_order(false);
    test_small_overlap(true);
    test_small_overlap(false);
    test_small_run_repaired_by_mtu_units();
    test_small_bitmap_marks_each_unit();
    test_small_mid_sample_run_is_not_first_contact();
    test_large_mid_sample_run_is_not_first_contact();
    test_large_geometry();
    test_large_runs_then_tail(true);
    test_large_runs_then_tail(false);
    test_large_out_of_order_and_overlap(true);
    test_large_out_of_order_and_overlap(false);
    test_large_bitmap_marks_each_unit();
    test_malformed_runs(false);
    test_malformed_runs(true);
    test_self_sent_run_ignored();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_hybrid_runs: all tests passed\n");
    return 0;
}
