/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Claimed-slot publish (tickle.h, tt_Publisher_claim(); DESIGN.md section 10): the caller builds the CDR in the
// same-host subscriber's ring slot, and the publish copies nothing.
//
// The claim this file holds the API to is that the caller's fill replaced the encoder and nothing else changed: each
// comparison runs one scenario twice from identical state - once with tt_Publisher_publish() (whose encoder writes the
// same slot, try_publish_into_slot()) and once with claim, fill, publish_claimed - and compares every ring record, the
// reliable cache, seq_no and the counters. Then the rules: one claim per publisher, no other publish behind it, the
// ring stopped at the claim until it is published, abandon and destroy giving the slot back empty, the mapping pinned
// while claimed, the shapes and states that refuse a claim (and say which), and the claim whose destination changed
// before its publish, which is copied out and sent the ordinary way, in ring order.
#define tt_LOCAL_DELIVERY 1 // rmw_tickle's setting, so the local-subscriber refusal is compiled and tested

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
#define CLOCK_START (100ULL * tt_SECOND)
#define CLOCK_STEP 1000000ULL
#define LARGEST_WHOLE_CDR (tt_CONTROL_MAX_LENGTH - 24) // 4 (tt_Header) + 4 + 16 + 1448 = 1472, as test_encode_in_slot

struct sample {
    uint32_t id;
    int32_t size;
};

static void fill_pattern(uint8_t* payload, uint32_t id, int32_t size) {
    for (int32_t i = 0; i < size; i++) {
        payload[i] = (uint8_t)((id * 31U) + (uint32_t)i);
    }
}
static int32_t sample_size(struct tt_Data* data) {
    return ((const struct sample*)data)->size;
}
static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    const struct sample* sample = (const struct sample*)data;
    if (sample->size < 0 || len < (uint32_t)sample->size) {
        return -1;
    }
    fill_pattern(payload, sample->id, sample->size);
    return sample->size;
}
static void sample_free(struct tt_Data* data) {
    (void)data;
}

#define CACHE_DEPTH 8
#define ARENA_MAX (CACHE_DEPTH * 1600)

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

enum mode { ENCODED = 0, CLAIMED = 1 };

static struct tt_SegmentHeader* ring(struct arm* arm) {
    return arm->owner.own_segment;
}

static void setup(struct arm* arm) {
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

    arm->topic.name = "claim_topic";
    arm->topic.data_size = sizeof(struct sample);
    arm->topic.data_encode_size = sample_size;
    arm->topic.data_encode = sample_encode;
    arm->topic.data_free = sample_free;

    struct tt_Publisher* pub = &arm->pub;
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.entity_id = 0x11110001;
    pub->endpoint.name = "claim_pub";
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

static void release_arm(struct arm* arm) {
    release_segments(&arm->sender);
    release_own_segment(&arm->owner);
}
static void teardown(struct arm* arm) {
    release_arm(arm);
    test_mock_segments_free();
}
// Both arms of a comparison: the mock's segments are freed once, after both are released.
static void teardown_pair(void) {
    release_arm(&arms[ENCODED]);
    release_arm(&arms[CLAIMED]);
    test_mock_segments_free();
}

static const struct tt_SegmentSlot* slot_at(struct arm* arm, uint32_t index) {
    return (const struct tt_SegmentSlot*)segment_slot(ring(arm), index);
}

// One sample, the arm's way: the encoder, or claim + fill + publish_claimed.
static tt_ret_t send_one(struct arm* arm, enum mode mode, uint32_t id, int32_t size) {
    if (mode == ENCODED) {
        struct sample sample = {id, size};
        return tt_Publisher_publish(&arm->pub, (struct tt_Data*)&sample);
    }
    uint8_t* payload = NULL;
    tt_ret_t claimed = tt_Publisher_claim(&arm->pub, (uint32_t)size, &payload);
    if (claimed != tt_RET_OK) {
        return claimed;
    }
    fill_pattern(payload, id, size);
    return tt_Publisher_publish_claimed(&arm->pub, (uint32_t)size);
}

static void expect_arms_identical(const char* what) {
    struct arm* encoded = &arms[ENCODED];
    struct arm* claimed = &arms[CLAIMED];
    EXPECT_EQ_U32(ring(encoded)->write_index, ring(claimed)->write_index);
    uint32_t records = ring(encoded)->write_index;
    for (uint32_t i = 0; i < records; i++) {
        const struct tt_SegmentSlot* a = slot_at(encoded, i);
        const struct tt_SegmentSlot* b = slot_at(claimed, i);
        EXPECT_EQ_U32(a->sequence, b->sequence);
        EXPECT_EQ_U32(a->length, b->length);
        EXPECT_EQ_U32(a->sender_ip, b->sender_ip);
        EXPECT_EQ_INT(a->sender_port, b->sender_port);
        EXPECT_EQ_INT(a->seq_span, b->seq_span);
        if (memcmp(a + 1, b + 1, a->length) != 0) {
            printf("  %s: record %u differs between the encoded and the claimed publish\n", what, (unsigned)i);
            EXPECT_TRUE(false);
        }
    }
    EXPECT_EQ_U32(encoded->pub.seq_no, claimed->pub.seq_no);
    EXPECT_EQ_U32(encoded->pub.heartbeat_piggyback_count, claimed->pub.heartbeat_piggyback_count);
    EXPECT_EQ_U64(encoded->sender.tx_datagrams_by_transport[tt_TRANSPORT_SHM],
                  claimed->sender.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U64(encoded->sender.segment_doorbells_sent, claimed->sender.segment_doorbells_sent);
    if (encoded->pub.reliable_cache != NULL) {
        EXPECT_EQ_U32(encoded->cache.oldest_seq_no, claimed->cache.oldest_seq_no);
        EXPECT_EQ_U32(encoded->cache.newest_seq_no, claimed->cache.newest_seq_no);
        EXPECT_EQ_U32(encoded->cache.tail, claimed->cache.tail);
        if (memcmp(encoded->arena, claimed->arena, encoded->cache.tail) != 0) {
            printf("  %s: the reliable cache's records differ\n", what);
            EXPECT_TRUE(false);
        }
    }
}

static const int32_t sizes[] = {1028, 1421, 3, 0, LARGEST_WHOLE_CDR, 64};
#define SIZE_COUNT ((uint32_t)(sizeof(sizes) / sizeof(sizes[0])))

static void run_best_effort(enum mode mode) {
    struct arm* arm = &arms[mode];
    setup(arm);
    __atomic_store_n(&ring(arm)->reader_waiting, 5U, __ATOMIC_SEQ_CST); // asleep: the first record rings, once
    for (uint32_t i = 0; i < SIZE_COUNT; i++) {
        test_mock_now += CLOCK_STEP;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)send_one(arm, mode, i + 1, sizes[i]));
    }
}

// The bytes the reader gets are the encoder's; the claimed arm's CDR was written by the caller where the slot is, at
// 4 mod 8 - so a 4-byte prefix and then a struct with 64-bit members is aligned for it there.
static void test_a_claimed_publish_writes_what_the_encoder_writes(void) {
    run_best_effort(ENCODED);
    run_best_effort(CLAIMED);
    expect_arms_identical("best effort");
    EXPECT_EQ_U64(SIZE_COUNT, arms[ENCODED].sender.segment_encoded_in_slot);
    EXPECT_EQ_U64(0, arms[CLAIMED].sender.segment_encoded_in_slot);
    EXPECT_EQ_U64(SIZE_COUNT, arms[CLAIMED].sender.segment_claims_published);
    EXPECT_EQ_U64(0, arms[CLAIMED].sender.segment_claims_copied);
    EXPECT_EQ_U32(sizeof(struct tt_Header), arms[CLAIMED].sender.tx_tail); // tx_buffer never used

    struct arm* arm = &arms[CLAIMED];
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 1024, &payload));
    EXPECT_EQ_U32(4, (uint32_t)((uintptr_t)payload % 8U));
    EXPECT_TRUE(payload == segment_slot(ring(arm), SIZE_COUNT) + sizeof(struct tt_SegmentSlot) +
                               sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub));
    teardown_pair();
}

// RELIABLE KEEP_LAST: the cache keeps the same records, and seq_no runs the same.
static void run_reliable(enum mode mode) {
    struct arm* arm = &arms[mode];
    setup(arm);
    make_reliable(arm, false, ARENA_MAX);
    for (uint32_t i = 0; i < 5; i++) {
        test_mock_now += CLOCK_STEP;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)send_one(arm, mode, i + 1, 1000 + (int32_t)i));
    }
}

static void test_reliable_claims_cache_what_the_encoder_caches(void) {
    run_reliable(ENCODED);
    run_reliable(CLAIMED);
    expect_arms_identical("reliable");
    EXPECT_EQ_U32(5, arms[CLAIMED].cache.newest_seq_no);
    EXPECT_EQ_U64(5, arms[CLAIMED].sender.segment_claims_published);
    teardown_pair();
}

// One claim per publisher; nothing else from it while the claim is out; the reader stopped at the claimed slot; and
// once published, the ring and the publisher go on.
static void test_the_rules_while_a_claim_is_out(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t* payload = NULL;
    uint8_t* second = NULL;
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_publish_claimed(&arm->pub, 0)); // nothing claimed
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_abandon_claim(&arm->pub));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_EQ_U32(1, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_claim(&arm->pub, 64, &second));
    EXPECT_TRUE(second == NULL);
    struct sample sample = {9, 64};
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_publish(&arm->pub, (struct tt_Data*)&sample));
    EXPECT_EQ_U32(1, ring(arm)->write_index);    // claimed: write_index moved past it
    EXPECT_EQ_U32(0, slot_at(arm, 0)->sequence); // and not published: the reader has nothing to read
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    uint16_t span = 0;
    EXPECT_TRUE(!segment_read(ring(arm), buf, sizeof(buf), &len, &ip, &port, &span));

    // Another writer into the same ring goes behind the claim and waits there.
    EXPECT_TRUE(segment_write(ring(arm), buf, 8, NULL, 0, REMOTE_IP, REMOTE_PORT, 1));
    EXPECT_TRUE(!segment_read(ring(arm), buf, sizeof(buf), &len, &ip, &port, &span));

    fill_pattern(payload, 9, 64);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, 64));
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_U32(1, arm->pub.seq_no);
    EXPECT_TRUE(segment_read(ring(arm), buf, sizeof(buf), &len, &ip, &port, &span));
    EXPECT_EQ_U32(sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader) + 64, len);
    EXPECT_EQ_U32(1, ((const struct tt_DataHeader*)(buf + sizeof(struct tt_SingleHeader)))->seq_no);
    EXPECT_TRUE(segment_read(ring(arm), buf, sizeof(buf), &len, &ip, &port, &span)); // then the one behind it
    EXPECT_EQ_U32(8, len);
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_publish_claimed(&arm->pub, 0)); // spent
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&arm->pub, (struct tt_Data*)&sample));
    EXPECT_EQ_U32(2, arm->pub.seq_no);
    teardown(arm);
}

// The capacity is room, the length is what goes: a claim may be published shorter than it was claimed (a ROS
// message's struct can be longer than its wire bytes), and a length over the capacity is refused, the slot given back
// empty and the claim spent.
static void test_a_claim_publishes_its_length_not_its_capacity(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 128, &payload));
    fill_pattern(payload, 3, 128);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, 100));
    EXPECT_EQ_U32(ROUNDUP(sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader) + 100),
                  slot_at(arm, 0)->length);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Publisher_publish_claimed(&arm->pub, 65));
    EXPECT_EQ_U32(0, slot_at(arm, 1)->length);
    EXPECT_EQ_U32(2, slot_at(arm, 1)->sequence);
    EXPECT_TRUE(arm->pub.claim_slot == NULL);
    EXPECT_EQ_U32(1, arm->pub.seq_no);
    teardown(arm);
}

// Abandon and destroy give the slot back as the empty record the reader drops, spending no seq_no.
static void test_abandon_and_destroy_give_the_slot_back(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub));
    EXPECT_EQ_U32(1, slot_at(arm, 0)->sequence);
    EXPECT_EQ_U32(0, slot_at(arm, 0)->length);
    EXPECT_EQ_U32(0, arm->pub.seq_no);
    EXPECT_EQ_U64(1, arm->sender.segment_claims_abandoned);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_destroy(&arm->pub));
    EXPECT_EQ_U32(2, slot_at(arm, 1)->sequence);
    EXPECT_EQ_U32(0, slot_at(arm, 1)->length);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_TRUE(arm->pub.claim_slot == NULL);
    teardown(arm);
}

// A claimed mapping is not given up under the caller: the revalidation that falls due and the dead-reader rule both
// wait until the claim is resolved, and then the revalidation runs.
static void test_a_claimed_mapping_stays_mapped(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    struct tt_SegmentPeer* entry = &arm->sender.segment_peers[OWNER_ID];
    struct tt_SegmentHeader* mapping = entry->mapping;
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    entry->recheck_in = 0; // the revalidation is due
    EXPECT_TRUE(peer_segment(&arm->sender, OWNER_ID, OWNER_IP, OWNER_PORT) == mapping);
    EXPECT_TRUE(entry->mapping == mapping);
    EXPECT_EQ_U32(0, entry->recheck_in); // put off, not done
    uint64_t attached = arm->sender.segment_attach[tt_SEGMENT_ATTACHED];
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub));
    EXPECT_TRUE(peer_segment(&arm->sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    EXPECT_EQ_U64(attached + 1, arm->sender.segment_attach[tt_SEGMENT_ATTACHED]); // the control: revalidated now
    teardown(arm);
}

// Nor does the dead-reader rule give it up: a ring that has refused this context's writes for longer than
// tt_SEGMENT_DEAD_READER_NS is normally detached and its peer reached over UDP, but a claimed slot in it keeps it.
// The second half is the control - the same refusal with the claim resolved does detach.
static struct tt_Publisher other_pub;
static void refuse_one_write_long_after(struct arm* arm) {
    other_pub = arm->pub; // a second Publisher of the sender, to the same peer
    other_pub.claim_slot = NULL;
    other_pub.endpoint.entity_id = 0x11110002;
    arm->sender.segment_peers[OWNER_ID].last_progress_ns = test_mock_now;
    test_mock_now += tt_SEGMENT_DEAD_READER_NS + 1;
    struct sample sample = {5, 64};
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&other_pub, (struct tt_Data*)&sample));
}
static void test_a_claimed_mapping_outlives_the_dead_reader_rule(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    uint8_t filler[16];
    memset(filler, 0x5A, sizeof(filler));
    while (segment_write(ring(arm), filler, sizeof(filler), NULL, 0, OWNER_IP, OWNER_PORT, 1)) {
    }
    refuse_one_write_long_after(arm);
    EXPECT_TRUE(arm->sender.segment_full_dropped > 0);
    EXPECT_TRUE(arm->sender.segment_peers[OWNER_ID].mapping != NULL);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub));
    refuse_one_write_long_after(arm);
    EXPECT_TRUE(arm->sender.segment_peers[OWNER_ID].mapping == NULL); // the control: no claim, given up
    teardown(arm);
}

// The shapes that have no slot to build in say so - UNSUPPORTED, publish ordinarily - and take nothing from the ring.
static void expect_unsupported(struct arm* arm, uint32_t length) {
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_UNSUPPORTED, (int)tt_Publisher_claim(&arm->pub, length, &payload));
    EXPECT_TRUE(payload == NULL && arm->pub.claim_slot == NULL);
    EXPECT_EQ_U32(0, ring(arm)->write_index);
}

static void test_what_cannot_be_claimed(void) {
    struct arm* arm = &arms[CLAIMED];

    setup(arm);
    arm->pub.batch = true;
    expect_unsupported(arm, 64);
    teardown(arm);

    setup(arm); // two destinations, one remote
    arm->pub.peers[1].context_id = REMOTE_ID;
    arm->pub.peers[1].ip = REMOTE_IP;
    arm->pub.peers[1].port = REMOTE_PORT;
    expect_unsupported(arm, 64);
    teardown(arm);

    setup(arm); // a broadcast: no peers known
    arm->pub.peers[0].context_id = tt_CONTEXT_ID_INVALID;
    expect_unsupported(arm, 64);
    teardown(arm);

    setup(arm);
    arm->pub.match_heartbeat_pending = true;
    expect_unsupported(arm, 64);
    teardown(arm);

    setup(arm);
    arm->pub.local_subscriber_count = 1;
    expect_unsupported(arm, 64);
    teardown(arm);

    setup(arm); // larger than the peer's slot
    ring(arm)->slot_bytes = 1024;
    expect_unsupported(arm, 1100);
    teardown(arm);

    setup(arm); // larger than a datagram; the largest that is not is claimed
    expect_unsupported(arm, LARGEST_WHOLE_CDR + 1);
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, LARGEST_WHOLE_CDR, &payload));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub));
    teardown(arm);

    setup(arm); // no segment: a different address behind the context id
    arm->pub.peers[0].ip = REMOTE_IP;
    expect_unsupported(arm, 64);
    teardown(arm);
}

// A full ring: OUT_OF_BUFFER, nothing claimed - the caller publishes ordinarily, which drops and counts it.
// KEEP_ALL with nothing acknowledged to make room: WOULD_BLOCK, as the publish would say.
static void test_full_ring_and_keep_all(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t filler[16];
    memset(filler, 0x5A, sizeof(filler));
    while (segment_write(ring(arm), filler, sizeof(filler), NULL, 0, OWNER_IP, OWNER_PORT, 1)) {
    }
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OUT_OF_BUFFER, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_TRUE(arm->pub.claim_slot == NULL);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    teardown(arm);

    setup(arm);
    make_reliable(arm, true, ARENA_MAX);
    arm->cache.depth = 1; // one unacknowledged sample is all KEEP_ALL may hold
    EXPECT_EQ_INT((int)tt_RET_OK, (int)send_one(arm, CLAIMED, 1, 64));
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_TRUE(arm->pub.claim_slot == NULL);
    EXPECT_TRUE(arm->pub.writable_pending);
    teardown(arm);
}

// The destination changed between claim and publish (here a match Heartbeat fell due): the sample is copied out and
// published the ordinary way, the claimed slot goes back empty, and the reader gets the empty record and then the
// sample - in that order, with the bytes the caller wrote.
static void test_a_changed_destination_is_published_by_copy(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 200, &payload));
    fill_pattern(payload, 42, 200);
    arm->pub.match_heartbeat_pending = true;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, 200));
    EXPECT_EQ_U64(1, arm->sender.segment_claims_copied);
    EXPECT_EQ_U64(0, arm->sender.segment_claims_published);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_U32(1, arm->pub.seq_no);
    EXPECT_TRUE(arm->pub.claim_slot == NULL);
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 99;
    uint32_t ip = 0;
    uint16_t port = 0;
    uint16_t span = 0;
    EXPECT_TRUE(segment_read(ring(arm), buf, sizeof(buf), &len, &ip, &port, &span));
    EXPECT_EQ_U32(0, len); // the claimed slot, empty
    EXPECT_TRUE(segment_read(ring(arm), buf, sizeof(buf), &len, &ip, &port, &span));
    // The copy: a match Heartbeat and then the DATA, in one datagram (full headers).
    uint8_t expected[200];
    fill_pattern(expected, 42, 200);
    EXPECT_TRUE(len >= 200 && memcmp(buf + len - 200, expected, 200) == 0);
    teardown(arm);
}

int main(void) {
    test_a_claimed_publish_writes_what_the_encoder_writes();
    test_reliable_claims_cache_what_the_encoder_caches();
    test_the_rules_while_a_claim_is_out();
    test_a_claim_publishes_its_length_not_its_capacity();
    test_abandon_and_destroy_give_the_slot_back();
    test_a_claimed_mapping_stays_mapped();
    test_a_claimed_mapping_outlives_the_dead_reader_rule();
    test_what_cannot_be_claimed();
    test_full_ring_and_keep_all();
    test_a_changed_destination_is_published_by_copy();
    printf("test_publish_claim: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
