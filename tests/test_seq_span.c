/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// SHM_PLAN 6e's invariant: a sample consumes the number of seq_nos the NETWORK form would need,
// whatever path it takes.
//
// This needs a configuration no other test binary has: fragmentation compiled in (so a sample CAN
// need more than one datagram) AND segments (so a destination's slot can be wide enough to take it
// whole). test_data_frag.c has the first and says every other binary is built without it;
// test_transport_seam.c has the second at the shipped sample size, where the two counts are equal
// for every sample and so cannot tell a correct advance from a path-dependent one.
//
// Before 2026-10-03 the advance was counted against whole_limit, which comes from the destinations:
// the same sample consumed one seq_no for an all-local publisher and two when a remote subscriber
// was present. That is why 6e(a) had to forbid mixed destinations, and forbidding them was not even
// sufficient - it is decided at publish while retransmission happens later, so a segment detaching
// in between left a whole record that had to go out over UDP, where it exceeds a datagram by
// construction and was dropped. That is the measured tx_dropped_oversize.
#define tt_MAX_SAMPLE_LENGTH 4096

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: whole_record_limit_for() and the segment helpers are static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define ENDPOINT_ID 0xaabbccdd
#define OWNER_IP 0x0a4d0002
#define OWNER_PORT 8282
#define OWNER_ID 7
#define OWNER_INCARNATION 0x12345678U
#define PEER_IP 0x0a000002
#define PEER_PORT 7000

// Larger than one datagram and smaller than two, so the wire form needs exactly two.
#define BIG_SAMPLE_BYTES 2048

static int32_t big_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)BIG_SAMPLE_BYTES;
}
static int32_t big_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)data;
    if (len < BIG_SAMPLE_BYTES) {
        return -1;
    }
    memset(payload, 0xA5, BIG_SAMPLE_BYTES);
    return (int32_t)BIG_SAMPLE_BYTES;
}
static void big_free(struct tt_Data* data) {
    (void)data;
}

// The receiving side of this file's topic. big_encode() writes BIG_SAMPLE_BYTES of filler with the
// value in the first four bytes; this takes those four back out. The payload is deliberately larger
// than the value, which is the whole point of the file: a sample the wire form must split.
static int32_t span_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static uint32_t span_callback_count;
static void span_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
    span_callback_count++;
}

// Wide enough for a whole BIG_SAMPLE_BYTES record: a RELIABLE Subscriber with no usable reorder
// buffer un-receives a sample that arrives ahead of a gap, which would make the second half of the
// receive test measure that fallback instead of the watermark.
#define SPAN_REORDER_SLOTS 8
#define SPAN_REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + BIG_SAMPLE_BYTES)
static uint64_t span_reorder_storage[SPAN_REORDER_SLOTS * SPAN_REORDER_SLOT_BYTES / sizeof(uint64_t)];

static void init_sender(struct tt_Context* node, struct tt_Topic* topic, struct tt_Publisher* pub) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = 1;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
    node->hal.own_ip = PEER_IP;
    node->hal.own_port = PEER_PORT;

    memset(topic, 0, sizeof(*topic));
    topic->name = "span_topic";
    topic->data_size = sizeof(uint32_t);
    topic->data_encode_size = big_encode_size;
    topic->data_encode = big_encode;
    topic->data_free = big_free;

    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.name = "span_pub";
    pub->node = node;
    pub->topic = topic;
    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)pub;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
}

// The control for the test below: with no segment attached, the destination cannot take a whole
// record and both counts agree. If this arm ever disagreed with the one below, the test would be
// measuring the publish path rather than the seq space.
static void test_without_a_segment_the_span_is_the_wire_count(void) {
    struct tt_Context sender;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_sender(&sender, &topic, &pub);
    pub.peers[0].context_id = OWNER_ID;
    pub.peers[0].ip = OWNER_IP;
    pub.peers[0].port = OWNER_PORT;

    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, pub.peers, 1)); // nothing attached
    uint32_t before = pub.seq_no;
    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    EXPECT_EQ_U32(2, pub.seq_no - before);
    test_mock_segments_free();
}

// The arm that distinguishes the fix: the destination's slot COULD take the record whole, and the
// advance must still be the wire form's two.
static void test_a_wide_slot_does_not_shrink_the_span(void) {
    struct tt_Context owner;
    struct tt_Context sender;
    struct tt_Topic topic;
    struct tt_Publisher pub;

    memset(&owner, 0, sizeof(owner));
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    init_sender(&sender, &topic, &pub);
    pub.peers[0].context_id = OWNER_ID;
    pub.peers[0].ip = OWNER_IP;
    pub.peers[0].port = OWNER_PORT;
    EXPECT_TRUE(peer_segment(&sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);

    // Widened on the mapping directly: valid_slot_bytes() caps the runtime knob at one datagram
    // until 6e(b) lands, and this test is about the seq space rather than that ceiling. 4096 is a
    // multiple of 4, which whole_record_limit_for() requires of any slot it grants.
    sender.segment_peers[OWNER_ID].mapping->slot_bytes = 4096;
    // The slot is widened after the attach, which production never does (a segment's geometry is fixed when it is
    // created), so the ceiling record_size_limit() keeps from the attach is widened with it.
    sender.segment_slot_ceiling = 4096;
    EXPECT_EQ_U32(4096, whole_record_limit_for(&sender, pub.peers, 1));

    uint32_t before = pub.seq_no;
    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    // Two: what the wire form needs. Not one, which is what this destination could have taken whole.
    EXPECT_EQ_U32(2, pub.seq_no - before);

    // And the number reached the slot. Without this the publisher could stop reporting the span and
    // every other assertion in this file would still pass - the reset discipline and the bound check
    // both hold perfectly well about a number nobody ever sets.
    // End to end: the number the publisher allocated reached the slot. This assertion was written when
    // it could not pass - end_encode()'s flush branch capped at tt_MAX_BUFFER_LENGTH rather than at
    // record_size_limit(), so a record wider than a datagram was neither fragmented nor refused but
    // silently retained in tx_buffer, with tx_shm and tx_udp both 0 and this write_index still 0.
    EXPECT_EQ_U32(1, owner.own_segment->write_index); // it was actually sent, over shared memory
    const struct tt_SegmentSlot* slot = (const struct tt_SegmentSlot*)segment_slot(owner.own_segment, 0);
    EXPECT_EQ_U32(2, slot->seq_span);

    // And the publisher's copy went back to 1 when the buffer emptied, so it cannot ride out on the next
    // record. That is set_tx_tail()'s doing, not this test's.
    EXPECT_EQ_INT(1, sender.tx_seq_span);

    release_segments(&sender);
    release_own_segment(&owner);
    test_mock_segments_free();
}

// SHM_PLAN 6e's table says a segment detaching after publish re-fragments. Until 2026-10-03 nothing
// did: send_cached_record()'s whole-record branch ended in end_encode(), whose own comment is "the
// protocol does not fragment, so there is nothing else to do with it; the caller learns from false and
// rolls back". A record cached whole for a same-host peer whose segment then went would have been
// refused for ever - the oversize drop in a later disguise.
//
// The receiver must not be able to tell these fragments from ones the first publish would have sent,
// which is what makes the seq span load-bearing: frag_write_header() gives fragment i the base seq_no
// plus i, and the span reserved exactly that many.
static void test_a_whole_record_the_target_cannot_take_is_refragmented(void) {
    struct tt_Context sender;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_sender(&sender, &topic, &pub);
    test_mock_reset();

    // A cached record as check_and_cache_sample() leaves one: submessage header, DataHeader, CDR.
    static uint8_t record[sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader) + BIG_SAMPLE_BYTES];
    memset(record, 0xC3, sizeof(record));
    struct tt_SubmessageHeader* sub = (struct tt_SubmessageHeader*)record;
    sub->type = tt_SUBMESSAGE_TYPE_DATA;
    sub->receiver = tt_SUBMESSAGE_ID_ALL;
    sub->length = (uint16_t)sizeof(record);
    struct tt_DataHeader* data = (struct tt_DataHeader*)(record + sizeof(struct tt_SubmessageHeader));
    data->endpoint_id = ENDPOINT_ID;
    data->seq_no = 41; // the sample's base; its span reserved 41 and 42
    data->entity_id = 9;

    struct tt_Peer target;
    memset(&target, 0, sizeof(target));
    target.context_id = OWNER_ID;
    target.ip = OWNER_IP;
    target.port = OWNER_PORT;

    // No segment attached, so this destination takes datagrams and the record is larger than one.
    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, &target, 1));
    uint64_t oversize_before = sender.tx_dropped_oversize;

    EXPECT_TRUE(send_cached_record(&sender, record, (uint16_t)sizeof(record), true, &target));

    // Not refused, and not counted as something that could never be sent.
    EXPECT_EQ_U64(oversize_before, sender.tx_dropped_oversize);

    // The last datagram out is the sample's second fragment: seq_no 42 = base + 1, index 1 of 2, and
    // addressed to the node that asked rather than broadcast. Those are exactly the values a first
    // publish of this sample would have put there.
    const struct tt_SubmessageHeader* out =
        (const struct tt_SubmessageHeader*)(test_mock_send_last_buf + sizeof(struct tt_Header));
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_CONT, out->type);
    EXPECT_EQ_INT(OWNER_ID, out->receiver);
    const struct tt_FragContHeader* cont =
        (const struct tt_FragContHeader*)(test_mock_send_last_buf + sizeof(struct tt_Header) +
                                          sizeof(struct tt_SubmessageHeader));
    EXPECT_EQ_U32(42, cont->seq_no);
    EXPECT_EQ_INT(1, cont->frag_index);
    EXPECT_EQ_INT(2, cont->frag_count);

    test_mock_segments_free();
}

// The reset discipline, pre-registered by Plan as the test that decides whether it holds: a stale span
// survives every test that publishes one kind of record, because it only shows up on the NEXT one. So
// publish a wide record (span 2) and then an ordinary one, and look at what the second slot got.
//
// The second arm is the abandon path: a datagram that is built and rewound must not leave its span
// behind either. rollback() is the one way the tail rewinds, and set_tx_tail() is the one way it moves,
// which is what makes this a property rather than a convention.
static void test_a_span_does_not_outlive_its_datagram(void) {
    struct tt_Context sender;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_sender(&sender, &topic, &pub);

    // A span belongs to a datagram in tx_buffer. An empty buffer has none.
    EXPECT_EQ_U32(sizeof(struct tt_Header), sender.tx_tail);
    sender.tx_seq_span = 1;

    // Set one as a publish would, then abandon the datagram the way every rewind path does.
    sender.tx_seq_span = 2;
    uint32_t saved = sender.tx_tail;
    set_tx_tail(&sender, saved + 64); // a half-built datagram
    EXPECT_EQ_INT(2, sender.tx_seq_span);
    rollback(&sender, saved);
    EXPECT_EQ_INT(1, sender.tx_seq_span); // abandoned, so the span went with it

    // And whenever the buffer empties, however it empties.
    sender.tx_seq_span = 3;
    set_tx_tail(&sender, sizeof(struct tt_Header));
    EXPECT_EQ_INT(1, sender.tx_seq_span);

    test_mock_segments_free();
}

// segment_write() checks the publisher's number against a bound with its own source of truth rather
// than trusting it: a span above tt_FRAG_MAX_COUNT is a record the network form could not have carried,
// since frag_count counts the same datagrams in a uint8_t.
static void test_a_span_outside_the_wire_bound_is_refused(void) {
    struct tt_Context owner;
    memset(&owner, 0, sizeof(owner));
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    uint8_t datagram[64];
    memset(datagram, 0x5A, sizeof(datagram));

    EXPECT_TRUE(segment_write(owner.own_segment, datagram, sizeof(datagram), NULL, 0, OWNER_IP, OWNER_PORT, 2));
    EXPECT_EQ_INT(2, ((const struct tt_SegmentSlot*)segment_slot(owner.own_segment, 0))->seq_span);

    EXPECT_TRUE(segment_write(owner.own_segment, datagram, sizeof(datagram), NULL, 0, OWNER_IP, OWNER_PORT,
                              tt_FRAG_MAX_COUNT + 1));
    EXPECT_EQ_INT(
        1,
        ((const struct tt_SegmentSlot*)segment_slot(owner.own_segment, 1))->seq_span); // out of range -> the safe one

    EXPECT_TRUE(segment_write(owner.own_segment, datagram, sizeof(datagram), NULL, 0, OWNER_IP, OWNER_PORT, 0));
    EXPECT_EQ_INT(1, ((const struct tt_SegmentSlot*)segment_slot(owner.own_segment, 2))->seq_span); // 0 means 1

    release_own_segment(&owner);
    test_mock_segments_free();
}

// The receiving half, and the half that did not exist until 2026-10-03. Every test above is about
// the number the PUBLISHER allocates and puts in the slot; none of them asks whether anything then
// reads it. Nothing did. segment_read() never looked at tt_SegmentSlot.seq_span, so the reader
// advanced its watermark by one per record and then waited for a seq_no the publisher had already
// spent on the same sample - a number no datagram will ever carry.
//
// It is worth being precise about how invisible that is, because it is why this test is here and
// not somewhere cheaper. On the rig at slot_bytes 4096 the transport was perfect: rx_shm equalled
// tx_shm to the digit, 15,401 against 15,401, and frag_abandoned, gap_abandoned, gap_evicted and
// lost all read zero. 15,401 flawlessly delivered records produced one delivered sample. A field
// that is written and never read has no shape of its own: it compiles, it passes, and from outside
// it looks exactly like a working protocol that is simply not delivering anything.
//
// So this asserts on the watermark and on the second sample, not on the slot. The slot assertion
// in test_a_wide_slot_does_not_shrink_the_span() passed throughout.
static void test_a_whole_record_advances_the_reader_by_its_span(void) {
    struct tt_Context owner;
    struct tt_Context sender;
    struct tt_Topic topic;
    struct tt_Topic owner_topic;
    struct tt_Publisher pub;
    struct tt_Subscriber sub;

    memset(&owner, 0, sizeof(owner));
    node_init_locks(&owner);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    owner.tx_tail = sizeof(struct tt_Header);
    owner.tx_size = sizeof(owner.tx_buffer);
    // Wide slots from the start, not widened afterwards. create_own_segment() sizes the mapping and
    // seeds every slot's sequence using the stride it was built with, so moving slot_bytes later
    // leaves slot 0 valid and every slot after it pointing at a sequence that will never match -
    // a ring of one. The test above never noticed because it writes a single record; this one
    // writes two, and the second silently never reached the segment.
    // (valid_slot_bytes() caps the runtime knob at one datagram until 6e(b); this is whitebox and
    // sets the field create_own_segment() actually reads, same as that test's own comment says.)
    const uint32_t saved_slot_bytes = _tt_CONFIG.segment_slot_bytes;
    _tt_CONFIG.segment_slot_bytes = 4096;
    create_own_segment(&owner);
    _tt_CONFIG.segment_slot_bytes = saved_slot_bytes;
    EXPECT_TRUE(owner.own_segment != NULL);
    EXPECT_EQ_U32(4096, owner.own_segment->slot_bytes);

    memset(&owner_topic, 0, sizeof(owner_topic));
    owner_topic.name = "span_topic";
    owner_topic.data_size = sizeof(uint32_t);
    owner_topic.data_encode_size = big_encode_size;
    owner_topic.data_encode = big_encode;
    owner_topic.data_decode = span_decode;
    owner_topic.data_free = big_free;

    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = ENDPOINT_ID;
    sub.node = &owner;
    sub.topic = &owner_topic;
    sub.callback = span_callback;
    sub.reliable = true;
    sub.reorder_storage = span_reorder_storage;
    sub.reorder_slots = SPAN_REORDER_SLOTS;
    sub.reorder_slot_bytes = SPAN_REORDER_SLOT_BYTES;
    memset(span_reorder_storage, 0, sizeof(span_reorder_storage));
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    owner.endpoint_count = 1;
    owner.endpoints[0] = (struct tt_Endpoint*)&sub;

    init_sender(&sender, &topic, &pub);
    pub.reliable = true;
    pub.peers[0].context_id = OWNER_ID;
    pub.peers[0].ip = OWNER_IP;
    pub.peers[0].port = OWNER_PORT;
    EXPECT_TRUE(peer_segment(&sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    // Read from the owner's real header, not set here: the limit the publisher uses and the ring
    // the reader drains are now the same geometry.
    EXPECT_EQ_U32(4096, whole_record_limit_for(&sender, pub.peers, 1));

    span_callback_count = 0;

    // Sample one: seq_no 1, and it consumes 2 because that is what the wire form would need.
    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    EXPECT_EQ_U32(2, pub.seq_no); // seq_no 1 was this sample's; the span spent 2 as well

    bool emptied = false;
    (void)drain_own_segment(&owner, &emptied);
    EXPECT_EQ_U32(1, span_callback_count);

    // The assertion the bug was hiding behind. The reader saw ONE record and must stand at 3,
    // because seq_no 2 was spent on the sample it just took. At 2 it is waiting for a number that
    // will never come, and every later sample piles up behind it.
    // Found by scanning rather than by guessing the entity_id the sender stamps on the record:
    // this file's sender is built by hand, and a lookup that silently missed would read as "no
    // proxy" rather than as a wrong expectation.
    struct tt_WriterProxy* proxy = NULL;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (sub.writers[i].context_id != tt_CONTEXT_ID_INVALID) {
            proxy = &sub.writers[i];
            break;
        }
    }
    EXPECT_TRUE(proxy != NULL);
    if (proxy != NULL) {
        EXPECT_EQ_U32(3, proxy->ack_seq_no);
    }
    // And the absorption is counted, so a span that stops arriving is visible as a number rather
    // than as silence.
    EXPECT_EQ_U64(1, owner.rx_span_absorbed);

    // Sample two: seq_no 3. This is the one the rig never delivered - 15,400 further records after
    // the first produced nothing, because the watermark was stuck one short of every one of them.
    // The clock has to move, or the second sample is discarded as not newer than the first and
    // this would pass or fail for a reason that has nothing to do with the span.
    test_mock_now += 1000000;
    value = 8;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    (void)drain_own_segment(&owner, &emptied);
    EXPECT_EQ_U32(2, span_callback_count);

    release_segments(&sender);
    release_own_segment(&owner);
    test_mock_segments_free();
}

// The other path, and the one the test above does not reach. A sample too wide for the
// destination's slot goes as fragments, and frag_write_header() gives fragment i the base seq_no
// plus i - so each fragment genuinely occupies one seq position and its span is 1. The sample's
// span is spent by the set of them, not carried by each.
//
// Until 2026-10-03 every fragment's slot claimed the WHOLE sample's span, because the publisher
// sets tt_Context.tx_seq_span once per sample and segment_deliver() reads it for every record it
// writes. While nothing read the field back that was harmless. The moment the reader began acting
// on it, a two-fragment sample made the reader absorb four seq positions instead of two, its
// watermark ran ahead of the stream, and every later sample arrived below it and was counted as a
// duplicate. On the rig at the DEFAULT slot_bytes that was recv=0 with frag_duplicate equal to
// every sample sent - and the publisher's own sent and send_mbps looked completely healthy, which
// is how a harness summary reported 121,781 samples/s for an arm that delivered nothing.
//
// The test above passed throughout, because it only ever exercises the whole-record path. Fixing
// one path and testing one path is what let this through 45 unit binaries and 15 gates.
static void test_each_fragment_carries_its_own_seq_position(void) {
    struct tt_Context owner;
    struct tt_Context sender;
    struct tt_Topic topic;
    struct tt_Topic owner_topic;
    struct tt_Publisher pub;
    struct tt_Subscriber sub;

    memset(&owner, 0, sizeof(owner));
    node_init_locks(&owner);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    owner.tx_tail = sizeof(struct tt_Header);
    owner.tx_size = sizeof(owner.tx_buffer);
    owner.rx_seq_span = 1;
    // The DEFAULT geometry, deliberately: this is the shipping configuration, and it is the one
    // the whole-record test cannot reach because there a BIG_SAMPLE_BYTES record does not fit.
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    EXPECT_TRUE(owner.own_segment->slot_bytes < BIG_SAMPLE_BYTES); // or this is not the frag path

    memset(&owner_topic, 0, sizeof(owner_topic));
    owner_topic.name = "span_topic";
    owner_topic.data_size = sizeof(uint32_t);
    owner_topic.data_encode_size = big_encode_size;
    owner_topic.data_encode = big_encode;
    owner_topic.data_decode = span_decode;
    owner_topic.data_free = big_free;

    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = ENDPOINT_ID;
    sub.node = &owner;
    sub.topic = &owner_topic;
    sub.callback = span_callback;
    sub.reliable = true;
    sub.reorder_storage = span_reorder_storage;
    sub.reorder_slots = SPAN_REORDER_SLOTS;
    sub.reorder_slot_bytes = SPAN_REORDER_SLOT_BYTES;
    memset(span_reorder_storage, 0, sizeof(span_reorder_storage));
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    owner.endpoint_count = 1;
    owner.endpoints[0] = (struct tt_Endpoint*)&sub;

    init_sender(&sender, &topic, &pub);
    pub.reliable = true;
    pub.peers[0].context_id = OWNER_ID;
    pub.peers[0].ip = OWNER_IP;
    pub.peers[0].port = OWNER_PORT;
    EXPECT_TRUE(peer_segment(&sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);

    span_callback_count = 0;
    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    // Two datagrams for one sample, so two slots, and the publisher still spent two seq_nos.
    EXPECT_EQ_U32(2, owner.own_segment->write_index);
    EXPECT_EQ_U32(2, pub.seq_no);

    // The direct assertion: each slot says ONE, not the sample's two. This is what was wrong, and
    // it is checkable without running the reader at all.
    for (uint32_t i = 0; i < 2; i++) {
        const struct tt_SegmentSlot* slot = (const struct tt_SegmentSlot*)segment_slot(owner.own_segment, i);
        EXPECT_EQ_U32(1, slot->seq_span);
    }

    // And the consequence, which is what the rig saw: with four positions absorbed for two
    // fragments the watermark passes the stream and the next sample reads as already seen.
    bool emptied = false;
    (void)drain_own_segment(&owner, &emptied);
    EXPECT_EQ_U32(1, span_callback_count);
    EXPECT_EQ_U64(0, owner.rx_span_absorbed); // a fragment absorbs nothing beyond itself

    test_mock_now += 1000000;
    value = 8;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    (void)drain_own_segment(&owner, &emptied);
    EXPECT_EQ_U32(2, span_callback_count);

    release_segments(&sender);
    release_own_segment(&owner);
    test_mock_segments_free();
}

int main(void) {
    test_without_a_segment_the_span_is_the_wire_count();
    test_a_wide_slot_does_not_shrink_the_span();
    test_a_whole_record_the_target_cannot_take_is_refragmented();
    test_a_span_does_not_outlive_its_datagram();
    test_a_span_outside_the_wire_bound_is_refused();
    test_a_whole_record_advances_the_reader_by_its_span();
    test_each_fragment_carries_its_own_seq_position();

    printf("test_seq_span: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
