/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Large samples (DESIGN.md section 8) meeting the same-host zero-copy paths (DESIGN.md section 10): a claimed slot
// (tt_Publisher_claim()) and receive-buffer lending (tt_Sample_retain()). The two were built on separate branches, each
// against a core without the other, so every test here is about state one of them keeps that the other reads:
//
// - a large publish while a claim is out is refused before it acquires a buffer, and a claim never covers a large
//   sample (UNSUPPORTED: "publish ordinarily");
// - a claimed publish takes its seq_no after a large sample published before it that still waits behind a send in
//   progress (tt_Publisher.large_pending), as an ordinary small publish does;
// - a large sample whose fragments come through the ring is lent from its own buffer, at the alignment a receive
//   buffer gives, with no ring slot held for it;
// - a departure processed inside that delivery does not unmap the ring under the record still being read: the
//   delivery swaps the lending state to LARGE, and the "a slot is being read" test must see through it.
#define tt_MAX_BUFFER_LENGTH 65507 // as rmw_tickle builds core
#define tt_LARGE_SAMPLES 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define ENDPOINT_ID 0xaabbccddU
#define OWNER_IP 0x0a4d0002
#define OWNER_PORT 8282
#define OWNER_ID 7
#define SENDER_IP 0x0a4d0003
#define SENDER_PORT 7000
#define SENDER_ID 1
#define REMOTE_IP 0x0a000009
#define REMOTE_PORT 7100
#define REMOTE_ID 9
#define LARGE_BYTES 70000U // above tt_MAX_SAMPLE_LENGTH (65507): 49 FRAG_FIRST_L/_CONT_L datagrams
#define SMALL_BYTES 64U

_Static_assert(LARGE_BYTES > tt_MAX_SAMPLE_LENGTH, "the large sample must be large");

// --- the caller's buffers -----------------------------------------------------------------------------------------

struct allocator {
    uint32_t acquires;
    uint32_t releases;
    uint32_t live;
};
static struct allocator sender_alloc;
static struct allocator owner_alloc;

static void* test_acquire(void* user, uint32_t bytes) {
    struct allocator* alloc = (struct allocator*)user;
    alloc->acquires++;
    uint8_t* block = (uint8_t*)malloc((size_t)bytes + 16U); // 16-aligned, as malloc() and rmw_large.c give
    if (block == NULL) {
        return NULL;
    }
    memset(block + 16, 0xA5, bytes);
    alloc->live++;
    return block + 16;
}
static void test_release(void* user, void* buffer) {
    struct allocator* alloc = (struct allocator*)user;
    alloc->releases++;
    alloc->live--;
    free((uint8_t*)buffer - 16);
}

// --- the sample ---------------------------------------------------------------------------------------------------

struct sample {
    uint32_t id;
    uint32_t size;
};
static uint8_t pattern(uint32_t i, uint32_t id) {
    return (uint8_t)((i * 7U) + (id * 31U) + (i >> 8U));
}
static int32_t sample_size(struct tt_Data* data) {
    return (int32_t)((const struct sample*)data)->size;
}
static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    const struct sample* sample = (const struct sample*)data;
    if (len < sample->size) {
        return -1;
    }
    for (uint32_t i = 0; i < sample->size; i++) {
        payload[i] = pattern(i, sample->id);
    }
    return (int32_t)sample->size;
}
static void fill(uint8_t* payload, uint32_t id, uint32_t size) {
    struct sample sample = {id, size};
    (void)sample_encode((struct tt_Data*)&sample, payload, size);
}
static bool intact(const uint8_t* payload, uint32_t id, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        if (payload[i] != pattern(i, id)) {
            return false;
        }
    }
    return true;
}

// --- the receiving Subscriber -------------------------------------------------------------------------------------

#define SEEN_MAX 16
static struct {
    uint32_t lengths[SEEN_MAX];
    uint16_t seq_nos[SEEN_MAX];
    uint32_t seen;
    bool retain;       // retain every large sample delivered
    bool forget_peer;  // run the sender's departure inside the large sample's delivery
    bool segment_kept; // ... and whether the ring was still mapped after it
    struct tt_Sample held;
    tt_ret_t retain_result;
    uint8_t lend_kind; // node->lend.rx_kind while the callback ran
} cb;
static uint32_t decoded_length;

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    decoded_length = len;
    return 0;
}
static void sample_free(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)data;
    if (cb.seen < SEEN_MAX) {
        cb.lengths[cb.seen] = decoded_length;
        cb.seq_nos[cb.seen] = seq_no;
    }
    cb.seen++;
    if (decoded_length <= tt_MAX_SAMPLE_LENGTH) {
        return;
    }
    cb.lend_kind = sub->node->lend.rx_kind;
    if (cb.retain) {
        cb.retain_result = tt_Sample_retain(sub, &cb.held);
    }
    if (cb.forget_peer) {
        forget_same_host_peer(sub->node, SENDER_ID);
        cb.segment_kept = sub->node->own_segment != NULL;
    }
}

// --- the rig: a sender with one Publisher to a same-host owner, which has a ring and a Subscriber -----------------

static struct tt_Context owner;
static struct tt_Context sender;
static struct tt_Topic sender_topic;
static struct tt_Topic owner_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;

static void rig_up(void) {
    test_mock_reset();
    test_mock_now = 100ULL * tt_SECOND;
    memset(&cb, 0, sizeof(cb));
    memset(&sender_alloc, 0, sizeof(sender_alloc));
    memset(&owner_alloc, 0, sizeof(owner_alloc));
    memset(&owner, 0, sizeof(owner));
    memset(&sender, 0, sizeof(sender));
    memset(&pub, 0, sizeof(pub));
    memset(&sub, 0, sizeof(sub));

    node_init_locks(&owner);
    owner.id = OWNER_ID;
    owner.entity_id_base = 0x12345678U;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    owner.tx_tail = sizeof(struct tt_Header);
    owner.tx_size = sizeof(owner.tx_buffer);
    owner.rx_seq_span = 1;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    owner.same_host_peer[SENDER_ID] = true; // the sender is a same-host peer, the one that keeps the ring wanted
    owner.same_host_peer_count = 1;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Context_set_large_buffers(&owner, test_acquire, test_release, &owner_alloc));

    memset(&owner_topic, 0, sizeof(owner_topic));
    owner_topic.name = "large_shm";
    owner_topic.data_size = sizeof(struct sample);
    owner_topic.data_decode = sample_decode;
    owner_topic.data_free = sample_free;
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = ENDPOINT_ID;
    sub.node = &owner;
    sub.topic = &owner_topic;
    sub.callback = on_data;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    owner.endpoints[owner.endpoint_count++] = (struct tt_Endpoint*)&sub;
    owner.endpoint_index_valid = false;

    node_init_locks(&sender);
    sender.id = SENDER_ID;
    sender.tx_tail = sizeof(struct tt_Header);
    sender.tx_size = sizeof(sender.tx_buffer);
    sender.hal.own_ip = SENDER_IP;
    sender.hal.own_port = SENDER_PORT;
    EXPECT_EQ_INT((int)tt_RET_OK,
                  (int)tt_Context_set_large_buffers(&sender, test_acquire, test_release, &sender_alloc));
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = "large_shm";
    sender_topic.data_size = sizeof(struct sample);
    sender_topic.data_encode_size = sample_size;
    sender_topic.data_encode = sample_encode;
    sender_topic.data_free = sample_free;
    pub.endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub.endpoint.id = ENDPOINT_ID;
    pub.endpoint.entity_id = 0x5a5a5a5aU;
    pub.endpoint.name = "large_shm_pub";
    pub.node = &sender;
    pub.topic = &sender_topic;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub.peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub.peers[0].context_id = OWNER_ID;
    pub.peers[0].ip = OWNER_IP;
    pub.peers[0].port = OWNER_PORT;
    sender.endpoints[sender.endpoint_count++] = (struct tt_Endpoint*)&pub;
    EXPECT_TRUE(peer_segment(&sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
}

static void rig_down(void) {
    large_release_publisher(&sender, &pub);
    large_release_assemblies(&owner);
    release_segments(&sender);
    release_own_segment(&owner);
    test_mock_segments_free();
}

static struct tt_SegmentHeader* ring(void) {
    return owner.own_segment;
}

static tt_ret_t publish(uint32_t id, uint32_t size) {
    test_mock_now += tt_MILLISECOND; // each newer than the last, or the reader discards it for its timestamp
    struct sample sample = {id, size};
    return tt_Publisher_publish(&pub, (struct tt_Data*)&sample);
}

static void drain(void) {
    bool emptied = false;
    for (int pass = 0; pass < 16 && owner.own_segment != NULL; pass++) {
        (void)drain_own_segment(&owner, &emptied);
        if (emptied) {
            return;
        }
    }
}

// The seq_no of the one single-form DATA record in the ring (the claimed sample), or 0.
static uint32_t claimed_record_seq_no(void) {
    for (uint32_t i = 0; i < ring()->write_index; i++) {
        const struct tt_SegmentSlot* slot =
            (const struct tt_SegmentSlot*)segment_slot(ring(), i); // NOLINT(readability-redundant-casting)
        const uint8_t* record = (const uint8_t*)(slot + 1);
        const struct tt_SingleHeader* single = (const struct tt_SingleHeader*)record;
        if (slot->length == sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader) + SMALL_BYTES &&
            single->type == tt_SUBMESSAGE_TYPE_DATA) {
            return ((const struct tt_DataHeader*)(record + sizeof(struct tt_SingleHeader)))->seq_no;
        }
    }
    return 0;
}

// ---- the claimed slot ----

// A claim out stops every other publish of its Publisher, a large one included: refused before a buffer is acquired,
// so nothing is held and nothing reaches the ring. Once the claim is published the large one goes, behind it, and the
// reader gets both whole and in order.
static void test_a_large_publish_behind_a_claim_is_refused_before_it_acquires(void) {
    rig_up();
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&pub, SMALL_BYTES, &payload));
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)publish(2, LARGE_BYTES));
    EXPECT_EQ_U32(0, sender_alloc.acquires);
    EXPECT_EQ_U64(0, sender.large.published);
    EXPECT_EQ_U32(1, ring()->write_index); // the claim, nothing behind it
    EXPECT_EQ_U32(0, pub.seq_no);

    fill(payload, 1, SMALL_BYTES);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&pub, SMALL_BYTES));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(2, LARGE_BYTES));
    uint32_t count = large_count_for(ROUNDUP(LARGE_BYTES));
    EXPECT_EQ_U32(1 + count, ring()->write_index);
    EXPECT_EQ_U32(1 + count, pub.seq_no);
    drain();
    EXPECT_EQ_U32(2, cb.seen);
    EXPECT_EQ_U32(SMALL_BYTES, cb.lengths[0]);
    EXPECT_EQ_U32(1, cb.seq_nos[0]);
    EXPECT_EQ_U32(LARGE_BYTES, cb.lengths[1]);
    EXPECT_EQ_U32(2, cb.seq_nos[1]);
    EXPECT_EQ_U64(1, owner.large.reassembled);
    rig_down();
}

// A claim is one slot: a large sample never fits one, so its claim says "publish ordinarily" - for any size above a
// sample, rmw_tickle's slot loan of an Array1m (its psn and the 1 MB struct) included - and holds nothing.
static void test_a_claim_of_a_large_sample_is_unsupported(void) {
    rig_up();
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_UNSUPPORTED,
                  (int)tt_Publisher_claim(&pub, (uint32_t)tt_MAX_SAMPLE_LENGTH + 1U, &payload));
    EXPECT_EQ_INT((int)tt_RET_UNSUPPORTED, (int)tt_Publisher_claim(&pub, 4U + 1048576U + 16U, &payload));
    EXPECT_TRUE(payload == NULL);
    EXPECT_TRUE(pub.claim_slot == NULL);
    EXPECT_EQ_U32(0, sender.segment_peers[OWNER_ID].claims);
    EXPECT_EQ_U32(0, ring()->write_index);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, LARGE_BYTES)); // and the ordinary publish takes it
    drain();
    EXPECT_EQ_U32(1, cb.seen);
    EXPECT_EQ_U32(LARGE_BYTES, cb.lengths[0]);
    rig_down();
}

// A large sample published while another is still being sent waits without seq_nos (large_pending). A claimed publish
// after it must not overtake it: the waiting one takes its seq_nos first, as it does ahead of an ordinary small
// publish. The send is held in progress by a second, remote peer whose socket buffer is full; the remote peer then
// leaves, which makes the owner the claim's one destination.
static void test_a_claimed_publish_follows_a_waiting_large_sample(void) {
    rig_up();
    pub.peers[1].context_id = REMOTE_ID;
    pub.peers[1].ip = REMOTE_IP;
    pub.peers[1].port = REMOTE_PORT;
    test_mock_nonblocking_room = 0;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, LARGE_BYTES));
    EXPECT_TRUE(pub.large_cursor.seq_no != 0); // in progress: the remote peer's share has not gone
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(2, LARGE_BYTES));
    EXPECT_TRUE(pub.large_pending.buffer != NULL); // waiting behind it, no seq_nos yet
    uint32_t count = large_count_for(ROUNDUP(LARGE_BYTES));
    EXPECT_EQ_U32(count, pub.seq_no);

    pub.peers[1].context_id = tt_CONTEXT_ID_INVALID;
    uint8_t* payload = NULL;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_claim(&pub, SMALL_BYTES, &payload));
    fill(payload, 3, SMALL_BYTES);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_claimed(&pub, SMALL_BYTES));
    EXPECT_TRUE(pub.large_pending.buffer == NULL);
    EXPECT_EQ_U32(2, pub.large_count);
    EXPECT_EQ_U32(count + 1, large_record_at(&pub, 1)->seq_no); // the second large sample, in publish order
    EXPECT_EQ_U32((2 * count) + 1, claimed_record_seq_no());    // and the claimed one after it
    EXPECT_EQ_U32((2 * count) + 1, pub.seq_no);
    test_mock_nonblocking_room = -1;
    rig_down();
}

// ---- lending ----

// A large sample whose fragments came through the ring is lent from its own buffer: the CDR at 4 mod 8 there, as in
// every receive buffer (tt_Context.rx_buffer), no ring slot held for it - the ring reads on - and its buffer kept
// until the release, when it goes back to the context's large release.
static void test_a_large_sample_from_the_ring_is_lent_from_its_own_buffer(void) {
    rig_up();
    cb.retain = true;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, LARGE_BYTES));
    drain();
    EXPECT_EQ_U32(1, cb.seen);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)cb.retain_result);
    EXPECT_EQ_INT((int)tt_LEND_LARGE, (int)cb.lend_kind);
    EXPECT_EQ_U32(4, (uint32_t)((uintptr_t)cb.held.payload % 8U));
    EXPECT_EQ_U32(0, owner.lend.held_slots);
    EXPECT_EQ_U32(ring()->write_index, ring()->read_index); // every fragment's slot read and given back
    EXPECT_EQ_U32(1, owner_alloc.live);
    EXPECT_TRUE(intact(cb.held.payload, 1, LARGE_BYTES));

    cb.retain = false;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(2, LARGE_BYTES)); // another assembly: never the kept buffer
    drain();
    EXPECT_EQ_U32(2, cb.seen);
    EXPECT_TRUE(intact(cb.held.payload, 1, LARGE_BYTES));
    EXPECT_EQ_U32(1, owner_alloc.live);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&owner, &cb.held));
    EXPECT_EQ_U32(0, owner_alloc.live);
    EXPECT_EQ_U32(owner_alloc.acquires, owner_alloc.releases);
    rig_down();
}

// The last fragment's record is still being read from the ring when its sample is delivered, though the lending state
// says LARGE for the callback's sake. A departure processed there - the last same-host peer gone - must leave the ring
// mapped until the record is done (lend_finish_release()), as it does for a sample read from a slot in place.
static void test_a_departure_inside_a_large_delivery_keeps_the_ring_until_read(void) {
    rig_up();
    cb.forget_peer = true;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, LARGE_BYTES));
    drain();
    EXPECT_EQ_U32(1, cb.seen);
    EXPECT_TRUE(cb.segment_kept);                      // mapped through the callback, under the record
    EXPECT_TRUE(owner.own_segment == NULL);            // and released by the drain once the record was done
    EXPECT_TRUE(!owner.lend.segment_release_deferred); // (lend_finish_release())
    EXPECT_EQ_U64(1, owner.segments_released);
    rig_down();
}

int main(void) {
    test_a_large_publish_behind_a_claim_is_refused_before_it_acquires();
    test_a_claim_of_a_large_sample_is_unsupported();
    test_a_claimed_publish_follows_a_waiting_large_sample();
    test_a_large_sample_from_the_ring_is_lent_from_its_own_buffer();
    test_a_departure_inside_a_large_delivery_keeps_the_ring_until_read();
    printf("test_large_shm: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
