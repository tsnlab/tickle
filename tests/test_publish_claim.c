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
// reliable cache, seq_no and the counters - record by record as the reader gets them, the claimed arm's through their
// references (tt_SEGMENT_SPAN_REF). Then the rules: several claims per publisher and its ordinary publishes meanwhile,
// the ring never stopped at a claim (its slot is held and passed over), samples read in publish order, abandon and
// both destroys giving the slot back, the mapping pinned while claimed, the shapes and states that refuse a claim (and
// say which), KEEP_ALL at the claim and at its publish, a ring with no room for the reference, and the claim whose
// destination changed before its publish, which is copied out and sent the ordinary way.
#define tt_LOCAL_DELIVERY 1 // rmw_tickle's setting, so the local-subscriber refusal is compiled and tested

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_capacity.sh passes a mutant
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
    struct tt_TypeSupport topic;
    struct tt_Publisher pub;
    struct tt_ReliableCacheIndex index[CACHE_DEPTH];
    uint8_t arena[ARENA_MAX];
    struct tt_ReliableCache cache;
};
// How send_one() publishes a sample. ENCODED: the encoder (the control). CLAIMED: claim, fill, publish - nothing
// claimed after it, so it is published in place (claim_publish_in_place()). REFERENCED: the same with a second claim
// made after it and abandoned after the publish, so the ring's next index is taken and it goes by reference. PASSED:
// the reader passes the claim's held index between the claim and the publish (note_head_stall(); the claim must be at
// the head), so it goes by reference - the race the publish loses.
enum mode { ENCODED = 0, CLAIMED, REFERENCED, PASSED, MODES };
static struct arm arms[MODES];

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
    segment_vouch_note(node, OWNER_ID, OWNER_INCARNATION,
                       OWNER_PORT); // discovery has heard the owner (segment_vouched())

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
// The arms of a comparison, ENCODED up to `last`: the mock's segments are freed once, after all are released.
static void teardown_arms(enum mode last) {
    for (int mode = ENCODED; mode <= (int)last; mode++) {
        release_arm(&arms[mode]);
    }
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
    uint8_t* after = NULL;
    if (mode == REFERENCED) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 8, &after));
    } else if (mode == PASSED) {
        EXPECT_EQ_INT((int)HEAD_STALL_PASSED, (int)note_head_stall(&arm->owner));
    }
    const tt_ret_t published = tt_Publisher_publish_claimed(&arm->pub, payload, (uint32_t)size);
    if (after != NULL) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, after));
    }
    return published;
}

// A ring record as the reader gets it: in its own slot, or - a reference - in the held slot it names.
struct logical {
    const struct tt_SegmentSlot* header; // sender and span
    const uint8_t* bytes;
    uint32_t length;
    uint16_t span;
};

// Every published record of the arm's ring in ring order, without reading any: references resolved, held slots (a
// claim's own index) left out, as the reader leaves them.
#define LOGICAL_MAX 64
static uint32_t logical_records(struct arm* arm, struct logical* out) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < ring(arm)->write_index && count < LOGICAL_MAX; i++) {
        const struct tt_SegmentSlot* slot = slot_at(arm, i);
        if (slot->sequence != i + 1U) {
            continue;
        }
        if (slot->seq_span == tt_SEGMENT_SPAN_REF) {
            struct tt_SegmentRef desc;
            memcpy(&desc, slot + 1, sizeof(desc));
            out[count] = (struct logical) {slot, (const uint8_t*)(slot_at(arm, desc.index) + 1), desc.length, 1};
        } else {
            out[count] = (struct logical) {slot, (const uint8_t*)(slot + 1), slot->length, slot->seq_span};
        }
        count++;
    }
    return count;
}

// The reader's next record, consumed: segment_read(), and past what it leaves at the head - an index passed while
// held (segment_pass_head()), or a reference, read in its held slot which is then given back - as the drain does
// (note_head_stall(), segment_ref_at_head()).
static bool ring_next(struct arm* arm, uint8_t* buf, uint32_t* len, uint32_t* ip) {
    struct tt_SegmentHeader* r = ring(arm);
    for (;;) {
        uint16_t port = 0;
        uint16_t span = 0;
        if (segment_read(r, buf, tt_CONTROL_MAX_LENGTH, len, ip, &port, &span)) {
            return true;
        }
        const uint32_t read_index = r->read_index;
        if (r->write_index == read_index) {
            return false;
        }
        const struct tt_SegmentSlot* head = slot_at(arm, read_index);
        if (segment_pass_head(r, read_index, head->sequence)) {
            continue;
        }
        if (head->sequence != read_index + 1U || head->seq_span != tt_SEGMENT_SPAN_REF) {
            return false;
        }
        struct tt_SegmentRef desc;
        memcpy(&desc, head + 1, sizeof(desc));
        *ip = head->sender_ip;
        segment_release(r, read_index);
        memcpy(buf, slot_at(arm, desc.index) + 1, desc.length);
        *len = desc.length;
        segment_unhold(r, desc.index);
        return true;
    }
}

static uint32_t seq_no_of(const uint8_t* datagram) {
    return ((const struct tt_DataHeader*)(datagram + sizeof(struct tt_SingleHeader)))->seq_no;
}

static void expect_arms_identical(const char* what, enum mode mode) {
    struct arm* encoded = &arms[ENCODED];
    struct arm* claimed = &arms[mode];
    struct logical a_records[LOGICAL_MAX];
    struct logical b_records[LOGICAL_MAX];
    uint32_t records = logical_records(encoded, a_records);
    EXPECT_EQ_U32(records, logical_records(claimed, b_records));
    EXPECT_TRUE(records > 0);
    for (uint32_t i = 0; i < records; i++) {
        const struct logical* a = &a_records[i];
        const struct logical* b = &b_records[i];
        EXPECT_EQ_U32(a->length, b->length);
        EXPECT_EQ_U32(a->header->sender_ip, b->header->sender_ip);
        EXPECT_EQ_INT(a->header->sender_port, b->header->sender_port);
        EXPECT_EQ_INT(a->span, b->span);
        if (a->length != b->length || memcmp(a->bytes, b->bytes, a->length) != 0) {
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

// The bytes the reader gets are the encoder's, published in place or by reference; the claimed arm's CDR was written
// by the caller where the slot is, at 4 mod 8 - so a 4-byte prefix and then a struct with 64-bit members is aligned
// for it there.
static void test_a_claimed_publish_writes_what_the_encoder_writes(void) {
    run_best_effort(ENCODED);
    run_best_effort(CLAIMED);
    run_best_effort(REFERENCED);
    expect_arms_identical("best effort, in place", CLAIMED);
    expect_arms_identical("best effort, by reference", REFERENCED);
    EXPECT_EQ_U64(SIZE_COUNT, arms[ENCODED].sender.segment_encoded_in_slot);
    for (int mode = CLAIMED; mode <= REFERENCED; mode++) {
        EXPECT_EQ_U64(0, arms[mode].sender.segment_encoded_in_slot);
        EXPECT_EQ_U64(SIZE_COUNT, arms[mode].sender.segment_claims_published);
        EXPECT_EQ_U64(0, arms[mode].sender.segment_claims_copied);
        EXPECT_EQ_U32(sizeof(struct tt_Header), arms[mode].sender.tx_tail); // tx_buffer never used
        EXPECT_EQ_U64(SIZE_COUNT, arms[mode].sender.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    }
    // In place: one index a sample, the claim's own, which reads "published" there.
    EXPECT_EQ_U64(SIZE_COUNT, arms[CLAIMED].sender.segment_claims_in_place);
    EXPECT_EQ_U32(SIZE_COUNT, ring(&arms[CLAIMED])->write_index);
    for (uint32_t i = 0; i < SIZE_COUNT; i++) {
        EXPECT_EQ_U32(i + 1U, slot_at(&arms[CLAIMED], i)->sequence);
    }
    // By reference: the claim's held index, the second claim's and the reference.
    EXPECT_EQ_U64(0, arms[REFERENCED].sender.segment_claims_in_place);
    EXPECT_EQ_U32(3 * SIZE_COUNT, ring(&arms[REFERENCED])->write_index);

    struct arm* arm = &arms[CLAIMED];
    uint8_t* payload = NULL;
    const uint32_t next = ring(arm)->write_index;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 1024, &payload));
    EXPECT_EQ_U32(4, (uint32_t)((uintptr_t)payload % 8U));
    EXPECT_TRUE(payload == segment_slot(ring(arm), next) + sizeof(struct tt_SegmentSlot) +
                               sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader));
    EXPECT_EQ_U32(next + 2U, slot_at(arm, next)->sequence); // held from the claim on
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, payload));
    teardown_arms(REFERENCED);
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
    run_reliable(REFERENCED);
    expect_arms_identical("reliable, in place", CLAIMED);
    expect_arms_identical("reliable, by reference", REFERENCED);
    for (int mode = CLAIMED; mode <= REFERENCED; mode++) {
        EXPECT_EQ_U32(5, arms[mode].cache.newest_seq_no);
        EXPECT_EQ_U64(5, arms[mode].sender.segment_claims_published);
    }
    EXPECT_EQ_U64(5, arms[CLAIMED].sender.segment_claims_in_place);
    EXPECT_EQ_U64(0, arms[REFERENCED].sender.segment_claims_in_place);
    teardown_arms(REFERENCED);
}

// Several claims at once, and the Publisher's ordinary publishes go on meanwhile; the reader is never stopped at a
// claim - its index is passed - and gets every sample in publish order whatever order the claims were made in, each
// with the seq_no it took at its publish. Then the slots are back and the ring runs on.
static void test_several_claims_and_publishes_meanwhile(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t* payloads[tt_PUBLISHER_CLAIMS];
    uint8_t bogus[64];
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_publish_claimed(&arm->pub, bogus, 0)); // not claimed
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_abandon_claim(&arm->pub, bogus));
    for (uint32_t k = 0; k < tt_PUBLISHER_CLAIMS; k++) {
        payloads[k] = NULL;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payloads[k]));
        EXPECT_EQ_U32(k + 2U, slot_at(arm, k)->sequence); // held
    }
    EXPECT_EQ_U32(tt_PUBLISHER_CLAIMS, arm->pub.claims_out);
    EXPECT_EQ_U32(tt_PUBLISHER_CLAIMS, arm->sender.segment_peers[OWNER_ID].claims);
    uint8_t* one_more = NULL;
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_claim(&arm->pub, 64, &one_more));
    EXPECT_TRUE(one_more == NULL);

    struct sample sample = {9, 64};
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&arm->pub, (struct tt_Data*)&sample)); // meanwhile
    EXPECT_EQ_U32(1, arm->pub.seq_no);
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 0;
    uint32_t ip = 0;
    EXPECT_TRUE(ring_next(arm, buf, &len, &ip)); // past the four held indices, not stopped at them
    EXPECT_EQ_U32(1, seq_no_of(buf));
    EXPECT_EQ_U64(0, arm->owner.segment_held_passed); // (ring_next() passes without the owner's counter)
    EXPECT_TRUE(!ring_next(arm, buf, &len, &ip));
    EXPECT_TRUE(segment_write(ring(arm), buf, 8, NULL, 0, REMOTE_IP, REMOTE_PORT, 1)); // another writer, not behind
    EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
    EXPECT_EQ_U32(8, len);

    // Published last claim first: read in that order, seq_nos 2.. in that order.
    for (uint32_t k = tt_PUBLISHER_CLAIMS; k-- > 0;) {
        fill_pattern(payloads[k], 100 + k, 64);
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, payloads[k], 64));
    }
    EXPECT_EQ_U32(0, arm->pub.claims_out);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_U32(1 + tt_PUBLISHER_CLAIMS, arm->pub.seq_no);
    uint8_t expected[64];
    for (uint32_t n = 0; n < tt_PUBLISHER_CLAIMS; n++) {
        const uint32_t k = tt_PUBLISHER_CLAIMS - 1U - n;
        EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
        EXPECT_EQ_U32(ROUNDUP(sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader) + 64), len);
        EXPECT_EQ_U32(2 + n, seq_no_of(buf));
        EXPECT_EQ_U32(SENDER_IP, ip);
        fill_pattern(expected, 100 + k, 64);
        EXPECT_TRUE(memcmp(buf + sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader), expected, 64) == 0);
        EXPECT_EQ_U32(k + ring(arm)->slots, slot_at(arm, k)->sequence); // given back by the reader
    }
    EXPECT_TRUE(!ring_next(arm, buf, &len, &ip));
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_publish_claimed(&arm->pub, payloads[0], 0)); // spent
    EXPECT_EQ_U64(tt_PUBLISHER_CLAIMS, arm->sender.segment_claims_published);

    // Two laps more, claims and publishes alternating: every slot comes round free.
    for (uint32_t n = 0; n < 2U * ring(arm)->slots; n++) {
        test_mock_now += CLOCK_STEP;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)send_one(arm, (n % 2U) != 0 ? CLAIMED : ENCODED, n, 32));
        EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
    }
    EXPECT_EQ_U64(0, arm->sender.segment_full_dropped);
    teardown(arm);
}

// tt_InitOptions bundle 9: a context's capacity.publisher_claims is the most claims one of its Publishers holds at
// once, claims[] keeping its compiled size. Two here: the third is refused as a full claims[] is, and a claim given
// back makes room. The control is the same Publisher at the default, which takes the third.
// tests/mutants_init_options_capacity.sh puts tt_PUBLISHER_CLAIMS back in the reader and expects this to fail.
#define CAPACITY_CLAIMS 2U
static void test_capacity_publisher_claims_is_the_claims_held(void) {
    static_assert(CAPACITY_CLAIMS < tt_PUBLISHER_CLAIMS, "the limit must be below the compiled size to show anything");
    struct arm* arm = &arms[CLAIMED];
    for (int limited = 0; limited <= 1; limited++) {
        setup(arm);
        EXPECT_EQ_U32(tt_PUBLISHER_CLAIMS, arm->sender.opts.capacity.publisher_claims); // resolved by node_init_locks()
        if (limited) {
            arm->sender.opts.capacity.publisher_claims = CAPACITY_CLAIMS;
        }
        uint8_t* payloads[CAPACITY_CLAIMS + 1U] = {NULL};
        for (uint32_t k = 0; k < CAPACITY_CLAIMS; k++) {
            EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payloads[k]));
        }
        tt_ret_t third = tt_Publisher_claim(&arm->pub, 64, &payloads[CAPACITY_CLAIMS]);
        EXPECT_EQ_INT(limited ? (int)tt_RET_ILLEGAL_STATUS : (int)tt_RET_OK, (int)third);
        EXPECT_EQ_U32(limited ? CAPACITY_CLAIMS : CAPACITY_CLAIMS + 1U, arm->pub.claims_out);
        if (limited) {
            EXPECT_TRUE(payloads[CAPACITY_CLAIMS] == NULL);
            EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, payloads[0]));
            EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payloads[0])); // room again
        }
        for (uint32_t k = 0; k <= CAPACITY_CLAIMS; k++) {
            if (payloads[k] != NULL) {
                EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, payloads[k]));
            }
        }
        EXPECT_EQ_U32(0, arm->pub.claims_out);
        teardown(arm);
    }
}

// In place only while it is safe (claim_publish_in_place()): a claim followed in the ring by an ordinary publish of its
// own Publisher goes by reference, or the reader would take it, with the later seq_no, ahead of that publish; and a
// claim the reader has passed goes by reference, its index marked by the reader (i + 3), which the publish's exchange
// cannot take. The control is a claim with nothing after it and nobody passing it, published in place.
static void test_in_place_only_when_last_and_unpassed(void) {
    struct arm* arm = &arms[CLAIMED];
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 0;
    uint32_t ip = 0;

    setup(arm); // control
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    fill_pattern(payload, 7, 64);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, payload, 64));
    EXPECT_EQ_U64(1, arm->sender.segment_claims_in_place);
    EXPECT_EQ_U32(1, ring(arm)->write_index);
    EXPECT_EQ_U32(1, slot_at(arm, 0)->sequence); // published at its own index
    // The race's other side: a drain that found the head held, and whose pass then lost to this publish, sees it
    // published and reads on - not a stalled head.
    EXPECT_EQ_INT((int)HEAD_STALL_PASSED, (int)note_head_stall(&arm->owner));
    EXPECT_EQ_U64(0, arm->owner.segment_head_stalls);
    EXPECT_EQ_U32(0, ring(arm)->read_index);
    EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
    EXPECT_EQ_U32(1, seq_no_of(buf));
    teardown(arm);

    setup(arm); // an ordinary publish after the claim
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    struct sample sample = {9, 64};
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&arm->pub, (struct tt_Data*)&sample)); // index 1, seq 1
    fill_pattern(payload, 7, 64);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, payload, 64)); // seq 2
    EXPECT_EQ_U64(0, arm->sender.segment_claims_in_place);
    EXPECT_EQ_U32(3, ring(arm)->write_index);    // the claim's index, the publish, the reference
    EXPECT_EQ_U32(2, slot_at(arm, 0)->sequence); // still held: nobody has passed it yet
    EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
    EXPECT_EQ_U32(1, seq_no_of(buf));
    EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
    EXPECT_EQ_U32(2, seq_no_of(buf));
    EXPECT_TRUE(!ring_next(arm, buf, &len, &ip));
    teardown(arm);

    setup(arm); // the reader passes the claim first
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_EQ_INT((int)HEAD_STALL_PASSED, (int)note_head_stall(&arm->owner));
    EXPECT_EQ_U32(3, slot_at(arm, 0)->sequence); // the reader's mark
    EXPECT_EQ_U32(1, ring(arm)->read_index);
    fill_pattern(payload, 7, 64);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, payload, 64));
    EXPECT_EQ_U64(0, arm->sender.segment_claims_in_place);
    EXPECT_EQ_U32(3, slot_at(arm, 0)->sequence); // the exchange lost: the slot is untouched as a ring state
    EXPECT_EQ_U32(2, ring(arm)->write_index);
    EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
    EXPECT_EQ_U32(1, seq_no_of(buf));
    EXPECT_EQ_U32(ring(arm)->slots, slot_at(arm, 0)->sequence); // given back by the reader: free for the next lap
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
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, payload, 100));
    struct logical records[LOGICAL_MAX];
    EXPECT_EQ_U32(1, logical_records(arm, records));
    EXPECT_EQ_U32(ROUNDUP(sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader) + 100), records[0].length);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload)); // index 1: the first in place
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Publisher_publish_claimed(&arm->pub, payload, 65));
    EXPECT_EQ_U32(1 + ring(arm)->slots, slot_at(arm, 1)->sequence); // given back: free for the next lap
    EXPECT_EQ_U32(2, ring(arm)->write_index);                       // and no reference written
    EXPECT_EQ_U32(0, arm->pub.claims_out);
    EXPECT_EQ_U32(1, arm->pub.seq_no);
    teardown(arm);
}

// Abandon and destroy give the slot back, spending no seq_no and writing nothing: the reader passes the claim's index.
static void test_abandon_and_destroy_give_the_slot_back(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    const uint32_t slots = ring(arm)->slots;
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, payload));
    EXPECT_EQ_U32(slots, slot_at(arm, 0)->sequence); // free for the next lap; index 0 stays passed
    EXPECT_EQ_U32(0, arm->pub.seq_no);
    EXPECT_EQ_U64(1, arm->sender.segment_claims_abandoned);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_abandon_claim(&arm->pub, payload)); // spent

    uint8_t* first = NULL;
    uint8_t* second = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &first));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &second));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_destroy(&arm->pub));
    EXPECT_EQ_U32(1 + slots, slot_at(arm, 1)->sequence);
    EXPECT_EQ_U32(2 + slots, slot_at(arm, 2)->sequence);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_U32(0, arm->pub.claims_out);
    EXPECT_EQ_U64(3, arm->sender.segment_claims_abandoned);
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 0;
    uint32_t ip = 0;
    EXPECT_TRUE(!ring_next(arm, buf, &len, &ip)); // nothing to read: three indices passed
    EXPECT_EQ_U32(3, ring(arm)->read_index);
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
    entry->recheck_due = true; // the revalidation is due
    EXPECT_TRUE(peer_segment(&arm->sender, OWNER_ID, OWNER_IP, OWNER_PORT) == mapping);
    EXPECT_TRUE(entry->mapping == mapping);
    EXPECT_TRUE(entry->recheck_due); // put off, not done
    uint64_t attached = arm->sender.segment_attach[tt_SEGMENT_ATTACHED] + arm->sender.segment_revalidated;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, payload));
    EXPECT_TRUE(peer_segment(&arm->sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    EXPECT_EQ_U64(attached + 1, arm->sender.segment_attach[tt_SEGMENT_ATTACHED] +
                                    arm->sender.segment_revalidated); // the control: revalidated now
    teardown(arm);
}

// Nor does the dead-reader rule give it up: a ring that has refused this context's writes for longer than
// tt_SEGMENT_DEAD_READER_NS is normally detached and its peer reached over UDP, but a claimed slot in it keeps it.
// The second half is the control - the same refusal with the claim resolved does detach.
static struct tt_Publisher other_pub;
static void refuse_one_write_long_after(struct arm* arm) {
    other_pub = arm->pub; // a second Publisher of the sender, to the same peer
    memset(other_pub.claims, 0, sizeof(other_pub.claims));
    other_pub.claims_out = 0;
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
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, payload));
    refuse_one_write_long_after(arm);
    EXPECT_TRUE(arm->sender.segment_peers[OWNER_ID].mapping == NULL); // the control: no claim, given up
    teardown(arm);
}

// The shapes that have no slot to build in say so - UNSUPPORTED, publish ordinarily - and take nothing from the ring.
static void expect_unsupported(struct arm* arm, uint32_t length) {
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_UNSUPPORTED, (int)tt_Publisher_claim(&arm->pub, length, &payload));
    EXPECT_TRUE(payload == NULL && arm->pub.claims_out == 0);
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
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, payload));
    teardown(arm);

    setup(arm); // a reader that cannot pass a held slot or read a reference (a build before 2026-10-20)
    ring(arm)->features &= (uint16_t)~tt_SEGMENT_FEATURE_HELD_REF;
    expect_unsupported(arm, 64);
    teardown(arm);

    setup(arm); // no segment: a different address behind the context id
    arm->pub.peers[0].ip = REMOTE_IP;
    expect_unsupported(arm, 64);
    teardown(arm);
}

// A full ring: OUT_OF_BUFFER, nothing claimed - the caller publishes ordinarily, which drops and counts it.
// KEEP_ALL with nothing acknowledged to make room: WOULD_BLOCK, as the publish would say - at the claim, and at its
// publish when ordinary publishes since took the room: the claim is kept then, and can be abandoned.
static void test_full_ring_and_keep_all(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t filler[16];
    memset(filler, 0x5A, sizeof(filler));
    while (segment_write(ring(arm), filler, sizeof(filler), NULL, 0, OWNER_IP, OWNER_PORT, 1)) {
    }
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OUT_OF_BUFFER, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_EQ_U32(0, arm->pub.claims_out);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    teardown(arm);

    setup(arm);
    make_reliable(arm, true, ARENA_MAX);
    arm->cache.depth = 1; // one unacknowledged sample is all KEEP_ALL may hold
    EXPECT_EQ_INT((int)tt_RET_OK, (int)send_one(arm, CLAIMED, 1, 64));
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    EXPECT_EQ_U32(0, arm->pub.claims_out);
    EXPECT_TRUE(arm->pub.writable_pending);
    teardown(arm);

    setup(arm);
    make_reliable(arm, true, ARENA_MAX);
    arm->cache.depth = 1;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload)); // room now
    struct sample sample = {2, 64};
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&arm->pub, (struct tt_Data*)&sample)); // takes it
    fill_pattern(payload, 3, 64);
    EXPECT_EQ_INT((int)tt_RET_WOULD_BLOCK, (int)tt_Publisher_publish_claimed(&arm->pub, payload, 64));
    EXPECT_EQ_U32(1, arm->pub.claims_out); // kept
    EXPECT_EQ_U32(1, arm->pub.seq_no);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_abandon_claim(&arm->pub, payload));
    teardown(arm);
}

// No room left for the reference at the publish: the sample is dropped as a full ring drops one - counted, its seq_no
// spent - and its held slot given back.
static void test_no_room_for_the_reference_drops_as_a_full_ring(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, &payload));
    uint8_t filler[16];
    memset(filler, 0x5A, sizeof(filler));
    while (segment_write(ring(arm), filler, sizeof(filler), NULL, 0, OWNER_IP, OWNER_PORT, 1)) {
    }
    fill_pattern(payload, 4, 64);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, payload, 64));
    EXPECT_EQ_U64(1, arm->sender.segment_full_dropped);
    EXPECT_EQ_U64(0, arm->sender.segment_claims_published);
    EXPECT_EQ_U32(1, arm->pub.seq_no);
    EXPECT_EQ_U32(0, arm->pub.claims_out);
    EXPECT_EQ_U32(ring(arm)->slots, slot_at(arm, 0)->sequence); // given back
    teardown(arm);
}

// A sample read through its reference is a record taken, as one read in its own slot is: the drain counts it, and the
// poll that took it hands it back. It did not (2026-10-20): segment_ref_at_head() processed the datagram from the
// drain's "nothing readable" branch and the drain returned 0, so the poll went round, found the ring empty and slept on
// top of a sample already delivered - until the budget or the next scheduler entry. rmw_perf_pingpong with slot loans
// on both sides: every round trip waited out the ping's deadline or a 250 ms idle Heartbeat (rtt p50 ~200-250 ms, 20
// round trips in 3 s against ~4,400; ab_loans.sh's PC preflight). The control is the same sample encoded into its own
// slot; every arm must end the poll at once, and the arms by reference (REFERENCED, PASSED) must have read their
// references (their treatment) and the arm in place none.
static void test_a_sample_read_by_reference_ends_the_poll(void) {
    static const uint64_t refs[MODES] = {0, 0, 2, 2};
    static const uint64_t passes[MODES] = {0, 0, 4, 2}; // REFERENCED: the claim's index and the abandoned one's
    static const char* const names[MODES] = {"encoded", "in place", "referenced", "passed"};
    for (int mode = ENCODED; mode < MODES; mode++) {
        struct arm* arm = &arms[mode];
        setup(arm);
        test_mock_receive_advances_clock = true;              // a wait with nothing arriving lets its whole length pass
        const int64_t budget = 500 * (int64_t)tt_MILLISECOND; // what the ping's spin_once() passes

        // The drain itself: one record taken, through the reference or in its slot.
        EXPECT_EQ_INT((int)tt_RET_OK, (int)send_one(arm, (enum mode)mode, 1, 64));
        bool emptied = false;
        EXPECT_EQ_U32(1, drain_own_segment(&arm->owner, &emptied));
        EXPECT_TRUE(emptied);

        // The poll: handed back at once, never asleep on what it took.
        EXPECT_EQ_INT((int)tt_RET_OK, (int)send_one(arm, (enum mode)mode, 2, 64));
        const uint64_t start = test_mock_now;
        const tt_ret_t polled = tt_Context_poll(&arm->owner, budget);
        EXPECT_EQ_U32(2, (uint32_t)arm->owner.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);
        EXPECT_EQ_U64(refs[mode], arm->owner.segment_refs_read); // the arm's treatment
        EXPECT_EQ_U64(passes[mode], arm->owner.segment_held_passed);
        EXPECT_EQ_U64(mode == CLAIMED ? 2U : 0U, arm->sender.segment_claims_in_place);
        EXPECT_EQ_INT((int)tt_RET_OK, (int)polled);
        EXPECT_EQ_INT(0, test_mock_receive_call_count);
        EXPECT_TRUE(test_mock_now - start < (uint64_t)budget);
        if (test_mock_now - start >= (uint64_t)budget) {
            printf("  %s: the poll held a delivered sample for %llu ms\n", names[mode],
                   (unsigned long long)((test_mock_now - start) / tt_MILLISECOND));
        }
        teardown(arm);
    }
}

// The destination changed between claim and publish (here a match Heartbeat fell due): the sample is copied out and
// published the ordinary way and the claimed slot given back; the reader passes the claim's index and gets the sample
// with the bytes the caller wrote.
static void test_a_changed_destination_is_published_by_copy(void) {
    struct arm* arm = &arms[CLAIMED];
    setup(arm);
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 200, &payload));
    fill_pattern(payload, 42, 200);
    arm->pub.match_heartbeat_pending = true;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&arm->pub, payload, 200));
    EXPECT_EQ_U64(1, arm->sender.segment_claims_copied);
    EXPECT_EQ_U64(0, arm->sender.segment_claims_published);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_U32(1, arm->pub.seq_no);
    EXPECT_EQ_U32(0, arm->pub.claims_out);
    EXPECT_EQ_U32(ring(arm)->slots, slot_at(arm, 0)->sequence); // given back
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 99;
    uint32_t ip = 0;
    EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
    // The copy: a match Heartbeat and then the DATA, in one datagram (full headers).
    uint8_t expected[200];
    fill_pattern(expected, 42, 200);
    EXPECT_TRUE(len >= 200 && memcmp(buf + len - 200, expected, 200) == 0);
    EXPECT_TRUE(!ring_next(arm, buf, &len, &ip));
    teardown(arm);
}

// Whether `p` lies in a mock region its last user has detached - a page a real munmap has taken away.
static bool in_detached_region(const void* p) {
    for (int i = 0; i < TEST_MOCK_MAX_SEGMENTS; i++) {
        const uint8_t* region = (const uint8_t*)test_mock_segments[i].region;
        if (region != NULL && (const uint8_t*)p >= region && (const uint8_t*)p < region + test_mock_segments[i].bytes) {
            return test_mock_segments[i].detached;
        }
    }
    return false;
}

// tt_Context_destroy() with a claim still out resolves it before the mapping goes: the slot is given back, so the
// subscriber's ring runs on past it for the other writers, and the Publisher holds nothing that points into the
// unmapped region - a later abandon or destroy is refused and writes nowhere. The control: the same teardown with
// nothing claimed, where the other writer's record is the first the reader takes.
static void destroy_sender_behind_another_writer(struct arm* arm, bool claim, uint8_t** payload) {
    setup(arm);
    if (claim) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&arm->pub, 64, payload));
    }
    uint8_t other[8] = {0};
    EXPECT_TRUE(segment_write(ring(arm), other, sizeof(other), NULL, 0, REMOTE_IP, REMOTE_PORT, 1));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Context_destroy(&arm->sender));
    EXPECT_TRUE(in_detached_region(ring(arm))); // the teardown did unmap the peer mapping (the mock's record of it)
}
static void expect_the_other_writer_read(struct arm* arm) {
    uint8_t buf[tt_CONTROL_MAX_LENGTH];
    uint32_t len = 99;
    uint32_t ip = 0;
    EXPECT_TRUE(ring_next(arm, buf, &len, &ip));
    EXPECT_EQ_U32(8, len);
    EXPECT_EQ_U32(REMOTE_IP, ip);
}
static void test_context_destroy_resolves_an_outstanding_claim(void) {
    struct arm* arm = &arms[CLAIMED];
    uint8_t* payload = NULL;

    destroy_sender_behind_another_writer(arm, false, &payload); // the control: nothing claimed
    expect_the_other_writer_read(arm);
    teardown(arm);

    destroy_sender_behind_another_writer(arm, true, &payload);
    EXPECT_EQ_U32(ring(arm)->slots, slot_at(arm, 0)->sequence); // the claimed slot given back before the unmap
    EXPECT_EQ_U64(1, arm->sender.segment_claims_abandoned);
    EXPECT_EQ_U32(0, arm->pub.claims_out);
    expect_the_other_writer_read(arm); // the reader passes the claim's index, and the ring runs on

    // Later calls on the Publisher find nothing claimed and write nowhere.
    const uint32_t sequence = slot_at(arm, 0)->sequence;
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Publisher_abandon_claim(&arm->pub, payload));
    EXPECT_EQ_U32(sequence, slot_at(arm, 0)->sequence);
    EXPECT_EQ_U32(0, arm->sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_U64(1, arm->sender.segment_claims_abandoned);
    teardown(arm);
}

int main(void) {
    test_a_claimed_publish_writes_what_the_encoder_writes();
    test_reliable_claims_cache_what_the_encoder_caches();
    test_several_claims_and_publishes_meanwhile();
    test_capacity_publisher_claims_is_the_claims_held();
    test_in_place_only_when_last_and_unpassed();
    test_a_claim_publishes_its_length_not_its_capacity();
    test_abandon_and_destroy_give_the_slot_back();
    test_a_claimed_mapping_stays_mapped();
    test_a_claimed_mapping_outlives_the_dead_reader_rule();
    test_what_cannot_be_claimed();
    test_full_ring_and_keep_all();
    test_no_room_for_the_reference_drops_as_a_full_ring();
    test_a_sample_read_by_reference_ends_the_poll();
    test_a_changed_destination_is_published_by_copy();
    test_context_destroy_resolves_an_outstanding_claim();
    printf("test_publish_claim: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
