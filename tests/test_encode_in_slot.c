/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// SHM_PLAN 6e(b), "encode into the slot" (try_publish_into_slot()): a publish whose one destination is an attached
// same-host segment encodes the sample straight into the claimed slot, where the two-copy path encodes it into
// tx_buffer and segment_write() then copies it into the slot.
//
// The claim this file holds the change to is that nothing but the copy changed. Every arm below runs one scenario
// twice, from identical state: once on the two-copy path and once on the one-copy path, and compares everything the
// receiver and the publisher's own retransmission can see - every ring record (bytes, length, sender, seq span), the
// reliable cache, seq_no, the KEEP_ALL and piggyback bookkeeping, and the reliable statistics. The two-copy arm is
// forced by summary_skip_armed with no summary riding, a condition the one-copy path declines and under which the
// staging path's bytes are exactly its ordinary ones (note_reached_armed() only records who was reached).
//
// Then the cases that must not take the new path, the failure after a claim (a claimed slot is always published),
// and the full ring, which must keep dropping and counting rather than rerouting.
//
// Mutants, each killed here (tests/mutants_encode_in_slot.py lists and runs them): every decline condition in
// encode_in_slot_destination() and try_publish_into_slot() removed in turn; the zero-length publish on failure
// removed; the padding left unzeroed; the single header left unwritten; the cache's copy skipped; the piggyback
// cadence not advanced; the watermark solicitation skipped.
#define tt_LOCAL_DELIVERY 1 // rmw_tickle's setting, so the local-subscriber decline is compiled and tested
#define tt_RELIABLE_STATS 1 // so flush_tx()'s datagram accounting is compared too

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define ENDPOINT_ID 0xaabbccdd
#define OWNER_IP 0x0a4d0002
#define OWNER_PORT 8282
#define OWNER_ID 7
#define OWNER_INCARNATION 0x12345678U
#define SENDER_IP 0x0a000002
#define SENDER_PORT 7000
#define SENDER_ID 1
#define REMOTE_IP 0x0a000009
#define REMOTE_PORT 7100
#define REMOTE_ID 9
#define SUB_ENTITY_ID 0x22220001
#define GARBAGE 0x77
#define CANARY 0xEE
#define CLOCK_START (100ULL * tt_SECOND)
#define CLOCK_STEP 1000000ULL
// The largest CDR that keeps a DATA within one datagram: 4 (tt_Header) + 4 + 16 + 1448 = 1472.
#define LARGEST_WHOLE_CDR (tt_CONTROL_MAX_LENGTH - 24)

// A sample: `size` bytes of a pattern seeded by `id`, or an encoder failure on request.
struct sample {
    uint32_t id;
    int32_t size;
    bool fail;
};

static int32_t sample_size(struct tt_Data* data) {
    return ((const struct sample*)data)->size;
}
static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    const struct sample* sample = (const struct sample*)data;
    if (sample->fail || sample->size < 0 || len < (uint32_t)sample->size) {
        return -1;
    }
    for (int32_t i = 0; i < sample->size; i++) {
        payload[i] = (uint8_t)((sample->id * 31U) + (uint32_t)i);
    }
    return sample->size;
}
static void sample_free(struct tt_Data* data) {
    (void)data;
}

#define CACHE_DEPTH 8
#define ARENA_MAX (CACHE_DEPTH * 1600)

// One arm: an owner whose segment is the destination, and a publisher in another context attached to it. Static: a
// context is large, and the two arms of a comparison must coexist.
struct arm {
    struct tt_Context owner;
    struct tt_Context sender;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    struct tt_ReliableCacheIndex index[CACHE_DEPTH];
    uint8_t arena[ARENA_MAX];
    struct tt_ReliableCache cache;
};
static struct arm arms[2];

enum mode { TWO_COPY = 0, ONE_COPY = 1 };

static struct tt_SegmentHeader* ring(struct arm* arm) {
    return arm->owner.own_segment;
}

static void setup(struct arm* arm, enum mode mode) {
    test_mock_reset();
    test_mock_now = CLOCK_START;
    memset(arm, 0, sizeof(*arm));

    arm->owner.id = OWNER_ID;
    arm->owner.entity_id_base = OWNER_INCARNATION;
    arm->owner.hal.own_ip = OWNER_IP;
    arm->owner.hal.own_port = OWNER_PORT;
    node_init_locks(&arm->owner);
    create_own_segment(&arm->owner);
    EXPECT_TRUE(ring(arm) != NULL);
    // Every payload filled with garbage, so a byte the publisher fails to write - padding above all - differs from
    // the two-copy path's rather than matching it by being zero in both.
    for (uint32_t i = 0; i < ring(arm)->slots; i++) {
        memset(segment_slot(ring(arm), i) + sizeof(struct tt_SegmentSlot), GARBAGE, ring(arm)->slot_bytes);
    }

    struct tt_Context* node = &arm->sender;
    node_init_locks(node);
    node->id = SENDER_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
    node->hal.own_ip = SENDER_IP;
    node->hal.own_port = SENDER_PORT;
    node->summary_skip_armed = mode == TWO_COPY ? 1 : 0; // with no summary riding: the staging path's own bytes

    arm->topic.name = "slot_topic";
    arm->topic.data_size = sizeof(struct sample);
    arm->topic.data_encode_size = sample_size;
    arm->topic.data_encode = sample_encode;
    arm->topic.data_free = sample_free;

    struct tt_Publisher* pub = &arm->pub;
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.entity_id = 0x11110001;
    pub->endpoint.name = "slot_pub";
    pub->node = node;
    pub->topic = &arm->topic;
    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)pub;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub->peers[0].context_id = OWNER_ID;
    pub->peers[0].ip = OWNER_IP;
    pub->peers[0].port = OWNER_PORT;
    EXPECT_TRUE(peer_segment(node, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    tt_reliable_stats_reset();
}

static void make_reliable(struct arm* arm, bool keep_all, uint32_t arena_size) {
    arm->cache.index = arm->index;
    arm->cache.capacity = CACHE_DEPTH;
    arm->cache.depth = CACHE_DEPTH;
    arm->cache.arena = arm->arena;
    arm->cache.arena_size = arena_size;
    arm->pub.reliable_cache = &arm->cache;
    arm->pub.reliable = true;
    arm->pub.keep_all = keep_all;
    struct tt_PeerAck* ack = claim_peer_ack(&arm->pub, OWNER_ID, SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    ack->tracking_words = 16;
}

static void teardown(struct arm* arm) {
    release_segments(&arm->sender);
    release_own_segment(&arm->owner);
}

static tt_ret_t publish(struct arm* arm, uint32_t id, int32_t size) {
    struct sample sample = {id, size, false};
    return tt_Publisher_publish(&arm->pub, (struct tt_Data*)&sample);
}

// Everything the two arms must agree on. The ring is compared slot by slot over every record either wrote, the
// slot's own header included: length, sender and seq span are what the reader acts on besides the bytes.
static void expect_arms_identical(const char* what) {
    struct arm* two = &arms[TWO_COPY];
    struct arm* one = &arms[ONE_COPY];
    EXPECT_EQ_U32(ring(two)->write_index, ring(one)->write_index);
    uint32_t records =
        ring(two)->write_index < ring(one)->write_index ? ring(two)->write_index : ring(one)->write_index;
    for (uint32_t i = 0; i < records; i++) {
        const struct tt_SegmentSlot* a = (const struct tt_SegmentSlot*)segment_slot(ring(two), i);
        const struct tt_SegmentSlot* b = (const struct tt_SegmentSlot*)segment_slot(ring(one), i);
        EXPECT_EQ_U32(a->sequence, b->sequence);
        EXPECT_EQ_U32(a->length, b->length);
        EXPECT_EQ_U32(a->sender_ip, b->sender_ip);
        EXPECT_EQ_INT(a->sender_port, b->sender_port);
        EXPECT_EQ_INT(a->seq_span, b->seq_span);
        uint32_t len = a->length < ring(two)->slot_bytes ? a->length : ring(two)->slot_bytes;
        if (memcmp(a + 1, b + 1, len) != 0) {
            printf("  %s: record %u differs between the two paths\n", what, (unsigned)i);
            EXPECT_TRUE(false);
        }
    }
    EXPECT_EQ_U32(two->pub.seq_no, one->pub.seq_no);
    EXPECT_EQ_U32(two->pub.heartbeat_piggyback_count, one->pub.heartbeat_piggyback_count);
    EXPECT_EQ_U32(two->pub.blocked_record_bytes, one->pub.blocked_record_bytes);
    EXPECT_EQ_U64(two->sender.tx_datagrams_by_transport[tt_TRANSPORT_SHM],
                  one->sender.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U64(two->sender.tx_datagrams_by_transport[tt_TRANSPORT_UDP],
                  one->sender.tx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    EXPECT_EQ_U64(two->sender.segment_doorbells_sent, one->sender.segment_doorbells_sent);
    if (two->pub.reliable_cache != NULL) {
        EXPECT_EQ_U32(two->cache.oldest_seq_no, one->cache.oldest_seq_no);
        EXPECT_EQ_U32(two->cache.newest_seq_no, one->cache.newest_seq_no);
        EXPECT_EQ_U32(two->cache.tail, one->cache.tail);
        for (int i = 0; i < CACHE_DEPTH; i++) {
            EXPECT_EQ_U32(two->index[i].seq_no, one->index[i].seq_no);
            EXPECT_EQ_U32(two->index[i].offset, one->index[i].offset);
            EXPECT_EQ_U32(two->index[i].len, one->index[i].len);
        }
        if (memcmp(two->arena, one->arena, two->cache.tail) != 0) {
            printf("  %s: the reliable cache's records differ between the two paths\n", what);
            EXPECT_TRUE(false);
        }
    }
}

// The stats are process-wide, so each arm's are read right after it runs.
static struct tt_ReliableStats arm_stats[2];

// p3's size first (1424, the bench's), then an odd size so the record is padded, the smallest, and the largest that
// still goes as one datagram.
static const int32_t sizes[] = {1424, 1421, 3, 0, LARGEST_WHOLE_CDR, 64};
#define SIZE_COUNT ((uint32_t)(sizeof(sizes) / sizeof(sizes[0])))

static void run_best_effort(struct arm* arm, enum mode mode) {
    setup(arm, mode);
    // tx_buffer behind the header, filled: a publish that encodes there overwrites it.
    memset(arm->sender.tx_buffer + sizeof(struct tt_Header), CANARY,
           sizeof(arm->sender.tx_buffer) - sizeof(struct tt_Header));
    // The reader is asleep, so the first record rings its doorbell - once per sleep, as on the two-copy path.
    __atomic_store_n(&ring(arm)->reader_waiting, 5U, __ATOMIC_SEQ_CST);
    for (uint32_t i = 0; i < SIZE_COUNT; i++) {
        test_mock_now += CLOCK_STEP;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, i + 1, sizes[i]));
    }
    tt_reliable_stats_get(&arm_stats[mode]);
}

static bool tx_buffer_untouched(const struct arm* arm) {
    for (size_t i = sizeof(struct tt_Header); i < sizeof(arm->sender.tx_buffer); i++) {
        if (arm->sender.tx_buffer[i] != CANARY) {
            return false;
        }
    }
    return true;
}

// The bytes the slot receives are the bytes the two-copy path put there, and the tx_buffer copy is gone: the one-copy
// arm never writes tx_buffer at all, while the two-copy arm - the positive control for that check - does.
static void test_best_effort_slots_match_the_two_copy_path(void) {
    run_best_effort(&arms[TWO_COPY], TWO_COPY);
    run_best_effort(&arms[ONE_COPY], ONE_COPY);
    EXPECT_EQ_U32(SIZE_COUNT, ring(&arms[ONE_COPY])->write_index);
    expect_arms_identical("best effort");

    EXPECT_EQ_U64(0, arms[TWO_COPY].sender.segment_encoded_in_slot);
    EXPECT_EQ_U64(SIZE_COUNT, arms[ONE_COPY].sender.segment_encoded_in_slot);
    EXPECT_TRUE(!tx_buffer_untouched(&arms[TWO_COPY])); // the control: the staging path does encode there
    EXPECT_TRUE(tx_buffer_untouched(&arms[ONE_COPY]));
    EXPECT_EQ_U32(sizeof(struct tt_Header), arms[ONE_COPY].sender.tx_tail);
    EXPECT_EQ_U64(1, arms[ONE_COPY].sender.segment_doorbells_sent);
    EXPECT_EQ_U64(arm_stats[TWO_COPY].datagrams, arm_stats[ONE_COPY].datagrams);
    EXPECT_EQ_U64(arm_stats[TWO_COPY].data_in_datagrams, arm_stats[ONE_COPY].data_in_datagrams);
    EXPECT_EQ_U64(arm_stats[TWO_COPY].max_data_per_datagram, arm_stats[ONE_COPY].max_data_per_datagram);
    EXPECT_EQ_U64(SIZE_COUNT, arm_stats[ONE_COPY].datagrams);

    // And the record is one the reader takes: the first slot read back as a DATA from this publisher.
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    uint16_t span = 0;
    EXPECT_TRUE(segment_read(ring(&arms[ONE_COPY]), buf, sizeof(buf), &len, &ip, &port, &span));
    EXPECT_EQ_U32(sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader) + 1424, len);
    EXPECT_EQ_U32(SENDER_IP, ip);
    EXPECT_EQ_INT(1, span);
    const struct tt_SingleHeader* single = (const struct tt_SingleHeader*)buf;
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_DATA, single->type);
    EXPECT_EQ_INT(SENDER_ID, single->source);
    const struct tt_DataHeader* data = (const struct tt_DataHeader*)(buf + sizeof(*single));
    EXPECT_EQ_U32(1, data->seq_no);
    EXPECT_EQ_U32(ENDPOINT_ID, data->endpoint_id);

    for (int i = 0; i < 2; i++) {
        teardown(&arms[i]);
    }
    test_mock_segments_free();
}

// RELIABLE KEEP_LAST with a Heartbeat piggybacked every third publish and the watermark solicitation on: the cache
// keeps the same records, the piggybacked publishes go the staging path (their datagram carries two submessages),
// and the cadence counts the one-copy publishes too.
static void run_reliable(struct arm* arm, enum mode mode) {
    setup(arm, mode);
    make_reliable(arm, false, ARENA_MAX);
    arm->pub.heartbeat_piggyback_every = 3;
    arm->pub.ack_solicit_watermark_pct = 50;
    for (uint32_t i = 0; i < 7; i++) {
        test_mock_now += CLOCK_STEP;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, i + 1, 1000 + (int32_t)i));
    }
    tt_reliable_stats_get(&arm_stats[mode]);
}

static void test_reliable_records_and_cache_match(void) {
    run_reliable(&arms[TWO_COPY], TWO_COPY);
    run_reliable(&arms[ONE_COPY], ONE_COPY);
    expect_arms_identical("reliable");
    // Publishes 3 and 6 carry the piggyback; the other five went into the slot directly.
    EXPECT_EQ_U64(5, arms[ONE_COPY].sender.segment_encoded_in_slot);
    EXPECT_EQ_U32(7, arms[ONE_COPY].cache.newest_seq_no);
    EXPECT_TRUE(ring(&arms[ONE_COPY])->write_index > 7); // the watermark's solicitations went in as well
    EXPECT_EQ_U64(arm_stats[TWO_COPY].datagrams, arm_stats[ONE_COPY].datagrams);
    EXPECT_EQ_U64(arm_stats[TWO_COPY].datagrams_with_data, arm_stats[ONE_COPY].datagrams_with_data);
    for (int i = 0; i < 2; i++) {
        teardown(&arms[i]);
    }
    test_mock_segments_free();
}

// KEEP_ALL refusing on the arena's bytes: asked before the claim, so the refused publish takes nothing from the ring
// but what the staging path's refusal sends (its solicitation), and the two arms agree on all of it.
static tt_ret_t keep_all_second;
static void run_keep_all(struct arm* arm, enum mode mode) {
    setup(arm, mode);
    make_reliable(arm, true, 1500); // one 1020-byte record fits, a second does not without evicting the first
    test_mock_now += CLOCK_STEP;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, 1000));
    test_mock_now += CLOCK_STEP;
    tt_ret_t second = publish(arm, 2, 1000);
    if (mode == ONE_COPY) {
        keep_all_second = second;
    }
}

static void test_keep_all_refusal_is_unchanged(void) {
    run_keep_all(&arms[TWO_COPY], TWO_COPY);
    run_keep_all(&arms[ONE_COPY], ONE_COPY);
    expect_arms_identical("keep all");
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)keep_all_second);
    EXPECT_EQ_U32(1, arms[ONE_COPY].pub.seq_no);
    EXPECT_EQ_U32(1, arms[ONE_COPY].cache.newest_seq_no);
    EXPECT_EQ_U64(1, arms[ONE_COPY].sender.segment_encoded_in_slot);
    for (int i = 0; i < 2; i++) {
        teardown(&arms[i]);
    }
    test_mock_segments_free();
}

// An encoder that fails after the slot is claimed: the slot is published anyway, as a zero-length record, because a
// claim cannot be taken back - left unpublished it would stop the ring for every writer. The reader releases it and
// drops it before parsing; the publish reports the error the staging path reports and spends no seq_no.
static void test_a_failed_encode_publishes_a_harmless_record(void) {
    struct arm* arm = &arms[ONE_COPY];
    setup(arm, ONE_COPY);
    struct sample bad = {1, 100, true};
    EXPECT_EQ_INT((int)tt_RET_PROTOCOL_ERROR, (int)tt_Publisher_publish(&arm->pub, (struct tt_Data*)&bad));
    EXPECT_EQ_U32(1, ring(arm)->write_index);
    const struct tt_SegmentSlot* slot = (const struct tt_SegmentSlot*)segment_slot(ring(arm), 0);
    EXPECT_EQ_U32(1, slot->sequence); // published: claimed index 0 + 1
    EXPECT_EQ_U32(0, slot->length);
    EXPECT_EQ_U32(0, arm->pub.seq_no);
    EXPECT_EQ_U64(0, arm->sender.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U64(0, arm->sender.segment_encoded_in_slot);

    // The ring still moves: the next publish takes the next slot, with the seq_no the failed one did not spend.
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 2, 64));
    EXPECT_EQ_U32(2, ring(arm)->write_index);

    // The reader's side: the empty record is read, released and handed to the receive path, which drops it without
    // calling it malformed, and the DATA behind it is delivered as the next record.
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 99;
    uint32_t ip = 0;
    uint16_t port = 0;
    uint16_t span = 0;
    EXPECT_TRUE(segment_read(ring(arm), buf, sizeof(buf), &len, &ip, &port, &span));
    EXPECT_EQ_U32(0, len);
    uint64_t malformed = arm->owner.rx_malformed_drops;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)process_datagram_locked(&arm->owner, 0, ip, port, tt_TRANSPORT_SHM, span));
    EXPECT_EQ_U64(malformed, arm->owner.rx_malformed_drops);
    EXPECT_TRUE(segment_read(ring(arm), buf, sizeof(buf), &len, &ip, &port, &span));
    EXPECT_EQ_U32(sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader) + 64, len);
    EXPECT_EQ_U32(1, ((const struct tt_DataHeader*)(buf + sizeof(struct tt_SingleHeader)))->seq_no);

    // A size the topic cannot even state is refused before anything is claimed.
    struct sample unsized = {3, -1, false};
    EXPECT_EQ_INT((int)tt_RET_PROTOCOL_ERROR, (int)tt_Publisher_publish(&arm->pub, (struct tt_Data*)&unsized));
    EXPECT_EQ_U32(2, ring(arm)->write_index);
    teardown(arm);
    test_mock_segments_free();
}

// A ring that is full when the publish arrives keeps today's behaviour: the sample is dropped and counted, never sent
// over UDP (it would overtake the records still in the ring), and nothing is claimed.
static void test_a_full_ring_still_drops_and_counts(void) {
    struct arm* arm = &arms[ONE_COPY];
    setup(arm, ONE_COPY);
    uint8_t filler[16];
    memset(filler, 0x5A, sizeof(filler));
    uint32_t filled = 0;
    while (segment_write(ring(arm), filler, sizeof(filler), NULL, 0, OWNER_IP, OWNER_PORT, 1)) {
        filled++;
    }
    EXPECT_EQ_U32(ring(arm)->slots, filled);
    int udp_before = test_mock_send_to_call_count;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, 1424));
    EXPECT_EQ_U32(filled, ring(arm)->write_index);
    EXPECT_EQ_U64(1, arm->sender.segment_full_dropped);
    EXPECT_EQ_U64(0, arm->sender.segment_encoded_in_slot);
    EXPECT_EQ_INT(udp_before, test_mock_send_to_call_count);
    EXPECT_EQ_U64(0, arm->sender.tx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    teardown(arm);
    test_mock_segments_free();
}

// The shapes that must stay on the staging path, one decline condition each.
static void test_what_stays_on_the_staging_path(void) {
    struct arm* arm = &arms[ONE_COPY];

    // A batching publisher: the sample waits in tx_buffer for node_flush().
    setup(arm, ONE_COPY);
    arm->pub.batch = true;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, 64));
    EXPECT_EQ_U64(0, arm->sender.segment_encoded_in_slot);
    EXPECT_EQ_U32(0, ring(arm)->write_index);
    EXPECT_TRUE(arm->sender.tx_tail > sizeof(struct tt_Header));
    teardown(arm);
    test_mock_segments_free();

    // Two destinations: the record is built once for both, and the remote one is reached over UDP.
    setup(arm, ONE_COPY);
    arm->pub.peers[1].context_id = REMOTE_ID;
    arm->pub.peers[1].ip = REMOTE_IP;
    arm->pub.peers[1].port = REMOTE_PORT;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, 64));
    EXPECT_EQ_U64(0, arm->sender.segment_encoded_in_slot);
    EXPECT_EQ_U32(1, ring(arm)->write_index);
    EXPECT_EQ_U64(1, arm->sender.tx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    teardown(arm);
    test_mock_segments_free();

    // A match Heartbeat is owed (tt_PeerAck.first_owed_seq_no): it goes ahead of this DATA in the same datagram, so
    // the publish takes the staging path - and still reaches the peer's segment, as one record.
    setup(arm, ONE_COPY);
    arm->pub.match_heartbeat_pending = true;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, 64));
    EXPECT_EQ_U64(0, arm->sender.segment_encoded_in_slot);
    EXPECT_EQ_U32(1, ring(arm)->write_index);
    teardown(arm);
    test_mock_segments_free();

    // A local Subscriber wants its own copy of the CDR (g9).
    setup(arm, ONE_COPY);
    arm->pub.local_subscriber_count = 1;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, 64));
    EXPECT_EQ_U64(0, arm->sender.segment_encoded_in_slot);
    EXPECT_EQ_U32(1, ring(arm)->write_index);
    teardown(arm);
    test_mock_segments_free();

    // A record the peer's slot cannot hold goes over UDP as oversized, as before - never into the slot.
    setup(arm, ONE_COPY);
    ring(arm)->slot_bytes = 1024;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, 1100));
    EXPECT_EQ_U64(0, arm->sender.segment_encoded_in_slot);
    EXPECT_EQ_U32(0, ring(arm)->write_index);
    EXPECT_EQ_U64(1, arm->sender.segment_oversized_to_udp);
    teardown(arm);
    test_mock_segments_free();

    // The boundary: the largest one-datagram sample goes into the slot, one byte more does not (this build does not
    // fragment, so the staging path refuses it).
    setup(arm, ONE_COPY);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, LARGEST_WHOLE_CDR));
    EXPECT_EQ_U64(1, arm->sender.segment_encoded_in_slot);
    EXPECT_TRUE(publish(arm, 2, LARGEST_WHOLE_CDR + 1) != tt_RET_OK);
    EXPECT_EQ_U64(1, arm->sender.segment_encoded_in_slot);
    EXPECT_EQ_U32(1, ring(arm)->write_index);
    teardown(arm);
    test_mock_segments_free();

    // No segment for this peer: a different address behind the context id.
    setup(arm, ONE_COPY);
    arm->pub.peers[0].ip = REMOTE_IP;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(arm, 1, 64));
    EXPECT_EQ_U64(0, arm->sender.segment_encoded_in_slot);
    EXPECT_EQ_U32(0, ring(arm)->write_index);
    EXPECT_EQ_U64(1, arm->sender.tx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    teardown(arm);
    test_mock_segments_free();
}

int main(void) {
    test_best_effort_slots_match_the_two_copy_path();
    test_reliable_records_and_cache_match();
    test_keep_all_refusal_is_unchanged();
    test_a_failed_encode_publishes_a_harmless_record();
    test_a_full_ring_still_drops_and_counts();
    test_what_stays_on_the_staging_path();
    return test_result();
}
