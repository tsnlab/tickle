/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Receive-buffer lending (DESIGN.md section 10): tt_Sample_retain() keeps a delivered sample where it arrived - its
// ring slot, or its receive buffer - until tt_Sample_release().
//
// Every property here is one whose failure is silent: a retained sample whose slot or buffer is reused reads as a
// sample that changed, and nothing else reports it. So each test makes the reuse happen if the hold does not work -
// the ring is 8 slots, so a lap is 8 records, and the socket path writes its next datagram into whatever buffer the
// context reads into next - and checks the retained bytes against a copy taken at retain time.
// tests/mutants_sample_lending.py removes each piece and checks a test here fails.
#define tt_MAX_SAMPLE_LENGTH 4096
#define tt_SEGMENT_BYTES (8 * (16 + 1472)) // 8 slots: a lap is 8 records

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#if !tt_SAMPLE_LENDING
int main(void) {
    // The compiled-out build: the API answers and nothing can be held.
    struct tt_Subscriber sub;
    struct tt_Sample sample;
    memset(&sub, 0, sizeof(sub));
    memset(&sample, 0, sizeof(sample));
    EXPECT_EQ_INT((int)tt_RET_UNSUPPORTED, (int)tt_Sample_retain(&sub, &sample));
    printf("test_sample_lending: %s\n", test_failures == 0 ? "all tests passed (lending compiled out)" : "FAILED");
    return test_result();
}
#else

static_assert(tt_SEGMENT_SLOTS == 8, "the tests below count laps of an 8-slot ring");

#define TOPIC_A_ID 0xaabbccddU
#define TOPIC_B_ID 0x11223344U
#define OWNER_IP 0x0a4d0002
#define OWNER_PORT 8282
#define OWNER_ID 7
#define OWNER2_IP 0x0a4d0003
#define OWNER2_ID 9
#define PEER_IP 0x0a000002
#define PEER_PORT 7000
#define SLOTS tt_SEGMENT_SLOTS

#define SMALL_SAMPLE_BYTES 64
#define BIG_SAMPLE_BYTES 2048 // two fragments: larger than one slot, smaller than two

static uint32_t sample_bytes = SMALL_SAMPLE_BYTES;

static int32_t value_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_bytes;
}
// The value in the first four bytes and the value's low byte after it, so a reused slot or buffer differs everywhere.
static int32_t value_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sample_bytes) {
        return -1;
    }
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    memset(payload, (int)(value & 0xFFU), sample_bytes);
    memcpy(payload, &value, sizeof(value));
    return (int32_t)sample_bytes;
}
static int32_t value_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static void value_free(struct tt_Data* data) {
    (void)data;
}

enum keep_mode {
    KEEP_NONE,
    KEEP_FIRST,     // retain the first sample delivered, nothing after
    KEEP_EVERY,     // retain every sample delivered
    KEEP_AND_GIVE,  // retain and release inside the same callback
    KEEP_FOR_OTHER, // retain for the OTHER Subscriber, inside this one's callback
};

#define KEPT_MAX 24
struct kept {
    struct tt_Sample sample;
    uint8_t copy[tt_MAX_SAMPLE_LENGTH];
    uint32_t value;
};

// What the Subscriber was handed, and what it kept.
static struct {
    enum keep_mode mode;
    uint32_t values[64];
    uint32_t seen;
    tt_ret_t last_retain;
    tt_ret_t last_release;
    struct kept kept[KEPT_MAX];
    uint32_t kept_count;
    struct tt_Subscriber* other;
    bool forget_peer; // the callback runs forget_same_host_peer(owner, 1), as a farewell read from the ring would
    uint32_t sequence_in_callback;
} cb;

static uint32_t value_of(const uint8_t* payload) {
    uint32_t value = 0;
    memcpy(&value, payload, sizeof(value));
    return value;
}

static uint32_t slot_sequence(const struct tt_Context* owner, uint32_t index) {
    return ((const struct tt_SegmentSlot*)segment_slot(owner->own_segment, index))->sequence;
}

static void callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    if (cb.seen < 64) {
        cb.values[cb.seen] = value;
    }
    cb.seen++;
    if (cb.forget_peer) {
        cb.forget_peer = false;
        forget_same_host_peer(subscriber->node, 1);
    }
    bool keep = cb.mode == KEEP_EVERY || cb.mode == KEEP_AND_GIVE || (cb.mode == KEEP_FIRST && cb.seen == 1);
    if (cb.mode == KEEP_FOR_OTHER) {
        struct tt_Sample sample;
        cb.last_retain = tt_Sample_retain(cb.other, &sample);
        return;
    }
    if (!keep || cb.kept_count >= KEPT_MAX) {
        return;
    }
    struct kept* kept = &cb.kept[cb.kept_count];
    cb.last_retain = tt_Sample_retain(subscriber, &kept->sample);
    if (cb.last_retain != tt_RET_OK) {
        return;
    }
    memcpy(kept->copy, kept->sample.payload, kept->sample.length);
    kept->value = value;
    cb.kept_count++;
    if (cb.mode == KEEP_AND_GIVE) {
        cb.last_release = tt_Sample_release(subscriber->node, &kept->sample);
        // The record is still being read: its slot must not be free for a writer yet.
        const struct tt_Context* node = subscriber->node;
        cb.sequence_in_callback = slot_sequence(node, node->lend.rx_index);
    }
}

// The retained bytes are exactly what they were when retained, and decode to the value delivered then.
static bool kept_intact(const struct kept* kept) {
    return kept->sample.payload != NULL && memcmp(kept->sample.payload, kept->copy, kept->sample.length) == 0 &&
           value_of(kept->sample.payload) == kept->value;
}

struct rig {
    struct tt_Context owner;
    struct tt_Context owner2; // a second receiving context with a ring of its own
    struct tt_Context sender;
    struct tt_Topic topic;       // the sender's
    struct tt_Topic owner_topic; // the receivers'
    struct tt_Publisher pub;
    struct tt_Subscriber sub;
    struct tt_Subscriber sub_b; // another Subscriber of the owner, on topic B
    struct tt_Subscriber sub2;  // owner2's
};
static struct rig rig;

static void init_topic(struct tt_Topic* topic, const char* name) {
    memset(topic, 0, sizeof(*topic));
    topic->name = name;
    topic->data_size = sizeof(uint32_t);
    topic->data_encode_size = value_encode_size;
    topic->data_encode = value_encode;
    topic->data_decode = value_decode;
    topic->data_free = value_free;
}

static void init_sub(struct tt_Subscriber* sub, struct tt_Context* node, uint32_t endpoint_id, uint16_t depth) {
    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = endpoint_id;
    sub->node = node;
    sub->topic = &rig.owner_topic;
    sub->callback = callback;
    sub->keep_last_depth = depth;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    node->endpoints[node->endpoint_count++] = (struct tt_Endpoint*)sub;
    node->endpoint_index_valid = false;
}

static void init_owner(struct tt_Context* owner, uint8_t id, uint32_t ip, bool with_ring) {
    node_init_locks(owner);
    owner->id = id;
    owner->entity_id_base = 0x12345678U + id;
    owner->hal.own_ip = ip;
    owner->hal.own_port = OWNER_PORT;
    owner->tx_tail = sizeof(struct tt_Header);
    owner->tx_size = sizeof(owner->tx_buffer);
    owner->rx_seq_span = 1;
    if (with_ring) {
        create_own_segment(owner);
        EXPECT_TRUE(owner->own_segment != NULL);
    }
}

// The owner (ring or not) with Subscriber `sub` (KEEP_LAST depth, 0 = KEEP_ALL) and Subscriber B on topic B, and the
// sender with one publisher to the owner.
static void rig_up(bool with_ring, uint16_t depth) {
    memset(&rig, 0, sizeof(rig));
    memset(&cb, 0, sizeof(cb));
    sample_bytes = SMALL_SAMPLE_BYTES;
    test_mock_reset();
    init_owner(&rig.owner, OWNER_ID, OWNER_IP, with_ring);
    init_topic(&rig.owner_topic, "lend_topic");
    init_sub(&rig.sub, &rig.owner, TOPIC_A_ID, depth);
    init_sub(&rig.sub_b, &rig.owner, TOPIC_B_ID, 0);

    struct tt_Context* sender = &rig.sender;
    node_init_locks(sender);
    sender->id = 1;
    sender->tx_tail = sizeof(struct tt_Header);
    sender->tx_size = sizeof(sender->tx_buffer);
    sender->hal.own_ip = PEER_IP;
    sender->hal.own_port = PEER_PORT;
    init_topic(&rig.topic, "lend_topic");
    struct tt_Publisher* pub = &rig.pub;
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = TOPIC_A_ID;
    pub->endpoint.name = "lend_pub";
    pub->endpoint.entity_id = 0x5a5a5a5aU;
    pub->node = sender;
    pub->topic = &rig.topic;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub->peers[0].context_id = OWNER_ID;
    pub->peers[0].ip = OWNER_IP;
    pub->peers[0].port = OWNER_PORT;
    sender->endpoints[sender->endpoint_count++] = (struct tt_Endpoint*)pub;
    if (with_ring) {
        EXPECT_TRUE(peer_segment(sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    }
}

static void rig_down(void) {
    release_segments(&rig.sender);
    release_own_segment(&rig.owner);
    release_own_segment(&rig.owner2);
    test_mock_segments_free();
}

static void publish(uint32_t value) {
    test_mock_now += 1000000; // each newer than the last, or the reader discards it for its timestamp
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&rig.pub, (struct tt_Data*)&value));
}

static void drain(struct tt_Context* owner) {
    bool emptied = false;
    for (int pass = 0; pass < 8; pass++) {
        (void)drain_own_segment(owner, &emptied);
        if (emptied) {
            return;
        }
    }
}

// ---- The segment path.

// A retained sample stays in its slot while the ring goes on, a lap and more: the slot is not released, so no writer
// reuses it, and from one lap on the ring is full - counted, and counted as a hold. The release frees it and the
// stream continues. Without the hold the ninth record overwrites the first.
static void test_a_retained_slot_survives_a_lap(void) {
    rig_up(true, 0);
    cb.mode = KEEP_FIRST;
    publish(100);
    drain(&rig.owner);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)cb.last_retain);
    EXPECT_EQ_U32(1, cb.kept_count);
    EXPECT_EQ_U32(1, slot_sequence(&rig.owner, 0)); // published, unread by the writers' reckoning: held
    EXPECT_EQ_U32(1, rig.owner.own_segment->read_index);
    EXPECT_EQ_U32(1, rig.owner.lend.held_slots);

    for (uint32_t i = 1; i <= 2 * SLOTS; i++) {
        publish(100 + i);
        drain(&rig.owner);
    }
    EXPECT_TRUE(kept_intact(&cb.kept[0]));
    EXPECT_EQ_U32(SLOTS, cb.seen); // the first sample and the seven that fit before the held slot came round
    EXPECT_EQ_U64(SLOTS + 1, rig.sender.segment_full_dropped);
    EXPECT_EQ_U64(SLOTS + 1, rig.sender.lend.full_retained);

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample));
    EXPECT_EQ_U32(SLOTS, slot_sequence(&rig.owner, 0)); // free for the writer one lap ahead
    EXPECT_TRUE(cb.kept[0].sample.payload == NULL && cb.kept[0].sample.handle == 0);
    EXPECT_EQ_U32(0, rig.owner.lend.held_slots);
    publish(500);
    drain(&rig.owner);
    EXPECT_EQ_U32(SLOTS + 1, cb.seen);
    EXPECT_EQ_U32(500, cb.values[SLOTS]);
    EXPECT_EQ_U64(1, rig.owner.lend.retains);
    EXPECT_EQ_U64(1, rig.owner.lend.releases);
    rig_down();
}

// The control for the test above: the same stream with nothing retained never fills the ring.
static void test_unretained_records_are_released_as_read(void) {
    rig_up(true, 0);
    for (uint32_t i = 0; i < 2 * SLOTS; i++) {
        publish(200 + i);
        drain(&rig.owner);
    }
    EXPECT_EQ_U32(2 * SLOTS, cb.seen);
    EXPECT_EQ_U64(0, rig.sender.segment_full_dropped);
    EXPECT_EQ_U32(2 * SLOTS, rig.owner.own_segment->read_index);
    EXPECT_EQ_U32((3 * SLOTS) - 1, slot_sequence(&rig.owner, (2 * SLOTS) - 1)); // released: index + slots
    rig_down();
}

// A held slot blocks only its own ring: a second receiving context fed by the same writer keeps receiving.
static void test_a_held_slot_blocks_only_its_own_ring(void) {
    rig_up(true, 0);
    init_owner(&rig.owner2, OWNER2_ID, OWNER2_IP, true);
    init_sub(&rig.sub2, &rig.owner2, TOPIC_A_ID, 0);
    rig.pub.peers[1].context_id = OWNER2_ID;
    rig.pub.peers[1].ip = OWNER2_IP;
    rig.pub.peers[1].port = OWNER_PORT;
    EXPECT_TRUE(peer_segment(&rig.sender, OWNER2_ID, OWNER2_IP, OWNER_PORT) != NULL);
    cb.mode = KEEP_FIRST;
    publish(300);
    drain(&rig.owner);
    drain(&rig.owner2);
    for (uint32_t i = 1; i <= 2 * SLOTS; i++) {
        publish(300 + i);
        drain(&rig.owner);
        drain(&rig.owner2);
    }
    // owner: the first lap, then full. owner2: every record (seen counts both Subscribers' deliveries).
    EXPECT_EQ_U32(SLOTS + (2 * SLOTS + 1), cb.seen);
    EXPECT_EQ_U32((2 * SLOTS) + 1, rig.owner2.own_segment->read_index);
    EXPECT_EQ_U32(0, rig.owner2.lend.held_slots);
    EXPECT_TRUE(kept_intact(&cb.kept[0]));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample));
    rig_down();
}

// SKIP TO NEWEST never frees a held slot: a KEEP_LAST 1 drain passes over a backlog behind a retained sample, and
// over a second lap of it, and the retained slot stays held and its bytes intact.
static void test_skip_to_newest_never_frees_a_retained_slot(void) {
    rig_up(true, 1);
    cb.mode = KEEP_FIRST;
    publish(400);
    drain(&rig.owner);
    EXPECT_EQ_U32(1, cb.kept_count);
    for (uint32_t i = 1; i < SLOTS; i++) {
        publish(400 + i); // fills the ring up to the held slot
    }
    drain(&rig.owner);
    EXPECT_EQ_U32(2, cb.seen); // the newest of the backlog
    EXPECT_EQ_U32(400 + SLOTS - 1, cb.values[1]);
    EXPECT_EQ_U64(SLOTS - 2, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U32(1, slot_sequence(&rig.owner, 0));
    for (uint32_t i = 0; i < 3; i++) {
        publish(500 + i); // the held slot has come round: refused
    }
    drain(&rig.owner);
    EXPECT_EQ_U32(1, slot_sequence(&rig.owner, 0));
    EXPECT_TRUE(kept_intact(&cb.kept[0]));
    EXPECT_EQ_U64(3, rig.sender.lend.full_retained);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample));
    EXPECT_EQ_U32(SLOTS, slot_sequence(&rig.owner, 0));
    rig_down();
}

// Two samples in one record (a batch): the slot goes back when the second is released, not the first.
static void test_a_batch_record_is_released_with_its_last_sample(void) {
    rig_up(true, 0);
    rig.pub.batch = true;
    cb.mode = KEEP_EVERY;
    publish(600);
    publish(601);
    node_flush(&rig.sender, test_mock_now, NULL);
    EXPECT_EQ_U32(1, rig.owner.own_segment->write_index); // one record carrying both
    drain(&rig.owner);
    EXPECT_EQ_U32(2, cb.kept_count);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample));
    EXPECT_EQ_U32(1, slot_sequence(&rig.owner, 0)); // still held by the second
    EXPECT_TRUE(kept_intact(&cb.kept[1]));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[1].sample));
    EXPECT_EQ_U32(SLOTS, slot_sequence(&rig.owner, 0));
    rig_down();
}

// Retained and released inside the same callback: the drain still owns the record and releases it after.
static void test_retain_and_release_in_one_callback(void) {
    rig_up(true, 0);
    cb.mode = KEEP_AND_GIVE;
    publish(700);
    drain(&rig.owner);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)cb.last_retain);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)cb.last_release);
    EXPECT_EQ_U32(1, cb.sequence_in_callback); // released by the drain after the record, not under it
    EXPECT_EQ_U32(SLOTS, slot_sequence(&rig.owner, 0));
    EXPECT_EQ_U32(0, rig.owner.lend.held);
    EXPECT_EQ_U32(1, rig.owner.own_segment->read_index);
    rig_down();
}

// ---- Handles.

// A second release, a release of handle 0 and a release of a handle whose entry has since been reused are each
// refused, counted, and touch nothing - in particular not the slot the entry's new sample holds.
static void test_bad_releases_are_refused_and_touch_nothing(void) {
    rig_up(true, 0);
    cb.mode = KEEP_EVERY;
    publish(800);
    drain(&rig.owner);
    struct tt_Sample stale = cb.kept[0].sample; // a copy of the first handle
    struct tt_Sample twice = stale;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample));
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample)); // handle 0
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Sample_release(&rig.owner, &twice)); // the same handle again
    EXPECT_EQ_U32(SLOTS, slot_sequence(&rig.owner, 0));
    EXPECT_EQ_U32(0, rig.owner.lend.held);
    publish(801);
    drain(&rig.owner); // the second sample takes the first one's entry
    EXPECT_EQ_U32(2, cb.kept_count);
    EXPECT_EQ_U32(stale.handle & 0xFFU, cb.kept[1].sample.handle & 0xFFU);
    EXPECT_TRUE(stale.handle != cb.kept[1].sample.handle);
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Sample_release(&rig.owner, &stale)); // released already
    EXPECT_EQ_U32(2, slot_sequence(&rig.owner, 1)); // the new sample's slot: still held
    EXPECT_EQ_U32(1, rig.owner.lend.held);
    struct tt_Sample unknown = {NULL, 0, 0xABCDEF00U | tt_SAMPLE_RETAIN_MAX, false};
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Sample_release(&rig.owner, &unknown)); // an entry not in use
    unknown.handle = 0xFFU;
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Sample_release(&rig.owner, &unknown)); // past the table
    EXPECT_EQ_U64(5, rig.owner.lend.bad_releases);
    EXPECT_TRUE(kept_intact(&cb.kept[1]));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[1].sample));
    EXPECT_EQ_U32(1 + SLOTS, slot_sequence(&rig.owner, 1));
    rig_down();
}

// Outside a callback, or for another Subscriber than the one being called: refused.
static void test_retain_outside_its_callback_is_refused(void) {
    rig_up(true, 0);
    struct tt_Sample sample;
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Sample_retain(&rig.sub, &sample));
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Sample_retain(NULL, &sample));
    cb.mode = KEEP_FOR_OTHER;
    cb.other = &rig.sub_b;
    publish(1000);
    drain(&rig.owner);
    EXPECT_EQ_U32(1, cb.seen);
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)cb.last_retain);
    EXPECT_EQ_U32(SLOTS, slot_sequence(&rig.owner, 0)); // nothing held: released as read
    rig_down();
}

// A sample put together from fragments is not lent: it lives in core's reassembly storage, not in its datagram.
static void test_a_reassembled_sample_is_not_lent(void) {
    rig_up(true, 0);
    sample_bytes = BIG_SAMPLE_BYTES;
    cb.mode = KEEP_FIRST;
    publish(1100);
    EXPECT_EQ_U32(2, rig.owner.own_segment->write_index); // two fragments, two records
    drain(&rig.owner);
    EXPECT_EQ_U32(1, cb.seen);
    EXPECT_EQ_U32(1100, cb.values[0]);
    EXPECT_EQ_INT((int)tt_RET_UNSUPPORTED, (int)cb.last_retain);
    EXPECT_EQ_U64(1, rig.owner.lend.unlendable);
    EXPECT_EQ_U32(0, rig.owner.lend.held);
    EXPECT_EQ_U32(SLOTS + 1, slot_sequence(&rig.owner, 1));
    rig_down();
}

// ---- Lifetime and threads.

// The lazy segment release waits while a slot is held, and the polling thread finishes it after the last release.
static void test_the_segment_outlives_a_held_slot(void) {
    rig_up(true, 0);
    rig.owner.same_host_peer[1] = true;
    rig.owner.same_host_peer_count = 1;
    cb.mode = KEEP_FIRST;
    publish(1200);
    drain(&rig.owner);
    forget_same_host_peer(&rig.owner, 1); // the last same-host peer goes
    EXPECT_TRUE(rig.owner.own_segment != NULL);
    EXPECT_TRUE(rig.owner.lend.segment_release_deferred);
    EXPECT_TRUE(!lend_finish_release(&rig.owner)); // still held
    EXPECT_TRUE(kept_intact(&cb.kept[0]));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample));
    EXPECT_TRUE(rig.owner.own_segment != NULL); // not from the releasing call: the polling thread does it
    EXPECT_TRUE(lend_finish_release(&rig.owner));
    EXPECT_TRUE(rig.owner.own_segment == NULL);
    rig_down();
}

// The last same-host peer's farewell read from the ring itself: the ring is not released under the record being read
// in it, and goes once the record is done - and the drain stops there rather than read a ring that is gone.
static void test_a_farewell_read_in_place_releases_the_ring_after(void) {
    rig_up(true, 0);
    rig.owner.same_host_peer[1] = true;
    rig.owner.same_host_peer_count = 1;
    cb.forget_peer = true;
    publish(1250);
    publish(1251);
    drain(&rig.owner);
    EXPECT_EQ_U32(1, cb.seen); // the second record was in a ring that is gone
    EXPECT_TRUE(rig.owner.own_segment == NULL);
    EXPECT_TRUE(!rig.owner.lend.segment_release_deferred);
    EXPECT_EQ_U64(1, rig.owner.segments_released);
    rig_down();
}

static void* release_from_another_thread(void* arg) {
    struct kept* kept = (struct kept*)arg;
    cb.last_release = tt_Sample_release(&rig.owner, &kept->sample);
    return NULL;
}

// A release from another thread frees the slot, and the stream goes on past a lap.
static void test_release_from_another_thread(void) {
    rig_up(true, 0);
    cb.mode = KEEP_FIRST;
    publish(1300);
    drain(&rig.owner);
    pthread_t thread;
    EXPECT_EQ_INT(0, pthread_create(&thread, NULL, release_from_another_thread, &cb.kept[0]));
    EXPECT_EQ_INT(0, pthread_join(thread, NULL));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)cb.last_release);
    for (uint32_t i = 1; i <= 2 * SLOTS; i++) {
        publish(1300 + i);
        drain(&rig.owner);
    }
    EXPECT_EQ_U32((2 * SLOTS) + 1, cb.seen);
    EXPECT_EQ_U64(0, rig.sender.segment_full_dropped);
    rig_down();
}

// tt_Context_destroy() forgets every held sample: a release afterwards is refused and writes nothing.
static void test_destroy_forgets_held_samples(void) {
    rig_up(true, 0);
    cb.mode = KEEP_FIRST;
    publish(1400);
    drain(&rig.owner);
    lend_forget_all(&rig.owner);
    EXPECT_EQ_INT((int)tt_RET_INVALID_ARGUMENT, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample));
    EXPECT_EQ_U32(1, slot_sequence(&rig.owner, 0)); // untouched (the real teardown unmaps it next)
    rig_down();
}

// ---- The socket path.

static uint8_t captured[tt_MAX_BUFFER_LENGTH];
static size_t captured_len;
static void capture(const void* buf, size_t len) {
    if (len <= sizeof(captured)) {
        memcpy(captured, buf, len);
        captured_len = len;
    }
}

// One datagram from the sender over UDP, written where the owner's socket reads next - as the HAL would - and
// processed.
static void socket_deliver(uint32_t value) {
    captured_len = 0;
    publish(value);
    EXPECT_TRUE(captured_len > 0);
    memcpy(RX_LANDING(&rig.owner), captured, captured_len);
    EXPECT_EQ_INT((int)tt_RET_OK,
                  (int)process_datagram(&rig.owner, (int32_t)captured_len, PEER_IP, PEER_PORT, tt_TRANSPORT_UDP));
}

static void socket_rig_up(void) {
    rig_up(false, 0);
    test_mock_send_hook = capture;
}

// No pool: nowhere for the next datagram to go, so retain fails cleanly and the sample is still delivered.
static void test_socket_retain_without_a_pool_fails_cleanly(void) {
    socket_rig_up();
    cb.mode = KEEP_FIRST;
    socket_deliver(1500);
    EXPECT_EQ_U32(1, cb.seen);
    EXPECT_EQ_INT((int)tt_RET_OUT_OF_BUFFER, (int)cb.last_retain);
    EXPECT_EQ_U64(1, rig.owner.lend.exhausted);
    EXPECT_EQ_U32(0, rig.owner.lend.landing);
    rig_down();
}

#define POOL_BUFFERS (tt_SAMPLE_RETAIN_MAX + 2)
static uint64_t pool_storage[(size_t)POOL_BUFFERS * tt_RX_POOL_BUFFER_BYTES / sizeof(uint64_t)];

// With a pool of one: the retained datagram's buffer is kept and the next one goes to the pool's; a second retain,
// with both buffers in use, fails cleanly; the release frees the first buffer for the next retain.
static void test_socket_retain_keeps_its_buffer(void) {
    socket_rig_up();
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Context_set_rx_pool(&rig.owner, pool_storage, 1));
    cb.mode = KEEP_EVERY;
    socket_deliver(1600);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)cb.last_retain);
    EXPECT_EQ_U32(1, rig.owner.lend.landing); // the next datagram goes to the pool's buffer
    EXPECT_TRUE(cb.kept[0].sample.payload >= rig.owner.rx_buffer &&
                cb.kept[0].sample.payload < rig.owner.rx_buffer + sizeof(rig.owner.rx_buffer));
    socket_deliver(1601);
    EXPECT_EQ_INT((int)tt_RET_OUT_OF_BUFFER, (int)cb.last_retain); // no spare: rx_buffer held, the pool's current
    EXPECT_EQ_U32(2, cb.seen);
    socket_deliver(1602);
    EXPECT_EQ_U32(3, cb.seen);
    EXPECT_TRUE(kept_intact(&cb.kept[0])); // three datagrams later
    EXPECT_EQ_INT((int)tt_RET_ILLEGAL_STATUS, (int)tt_Context_set_rx_pool(&rig.owner, NULL, 0));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[0].sample));
    socket_deliver(1603); // into the pool's buffer, retained: rx_buffer is free again
    EXPECT_EQ_INT((int)tt_RET_OK, (int)cb.last_retain);
    EXPECT_EQ_U32(0, rig.owner.lend.landing);
    socket_deliver(1604);
    EXPECT_TRUE(kept_intact(&cb.kept[1]));
    EXPECT_EQ_U32(1603, cb.kept[1].value);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[1].sample));
    EXPECT_EQ_U64(3, rig.owner.lend.exhausted); // 1601, 1602 and 1604: each time both buffers were in use
    rig_down();
}

// The handle table full: the next retain fails cleanly with buffers to spare, the sample is still delivered, and every
// held sample is intact.
static void test_a_full_handle_table_fails_cleanly(void) {
    socket_rig_up();
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Context_set_rx_pool(&rig.owner, pool_storage, POOL_BUFFERS));
    cb.mode = KEEP_EVERY;
    for (uint32_t i = 0; i <= tt_SAMPLE_RETAIN_MAX; i++) {
        socket_deliver(1800 + i);
    }
    EXPECT_EQ_U32(tt_SAMPLE_RETAIN_MAX + 1, cb.seen);
    EXPECT_EQ_INT((int)tt_RET_OUT_OF_BUFFER, (int)cb.last_retain);
    EXPECT_EQ_U32(tt_SAMPLE_RETAIN_MAX, rig.owner.lend.held);
    EXPECT_EQ_U64(1, rig.owner.lend.exhausted);
    for (uint32_t k = 0; k < tt_SAMPLE_RETAIN_MAX; k++) {
        EXPECT_TRUE(kept_intact(&cb.kept[k]));
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[k].sample));
    }
    EXPECT_EQ_U32(0, rig.owner.lend.held);
    rig_down();
}

// The control: a socket sample not retained is overwritten by the next datagram - which is what makes the test above
// able to fail.
static void test_socket_buffer_is_reused_when_not_retained(void) {
    socket_rig_up();
    socket_deliver(1700);
    const uint8_t* where = rig.owner.rx_buffer;
    uint32_t first = value_of(where + (captured_len - SMALL_SAMPLE_BYTES));
    socket_deliver(1701);
    EXPECT_EQ_U32(1700, first);
    EXPECT_EQ_U32(1701, value_of(where + (captured_len - SMALL_SAMPLE_BYTES)));
    rig_down();
}

// A struct with a 64-bit member behind a 4-byte prefix - rmw_tickle's psn ahead of an Array1k - read in place from
// wherever a socket datagram lands: the context's rx_buffer and a pool buffer. Both put a lone DATA's payload at 4 mod
// 8, so the struct is aligned for its uint64_t. rx_buffer used to sit 4 bytes off that (tt_ALIGNAS(4)), and the rmw
// layer then copied every such sample that landed there (docs/RMW.md "Loaned messages").
struct wide_sample {
    uint64_t stamp;
    uint32_t value;
    uint32_t pad;
};
static void test_a_socket_sample_is_aligned_for_a_64_bit_type(void) {
    socket_rig_up();
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Context_set_rx_pool(&rig.owner, pool_storage, 2));
    cb.mode = KEEP_EVERY;
    socket_deliver(1900); // into rx_buffer, retained: the next goes to the pool's buffer
    socket_deliver(1901);
    EXPECT_EQ_U32(2, cb.kept_count);
    const uint8_t* in_rx_buffer = cb.kept[0].sample.payload;
    const uint8_t* in_pool = cb.kept[1].sample.payload;
    EXPECT_TRUE(in_rx_buffer >= rig.owner.rx_buffer &&
                in_rx_buffer < rig.owner.rx_buffer + sizeof(rig.owner.rx_buffer));
    EXPECT_TRUE(in_pool >= (const uint8_t*)pool_storage &&
                in_pool < (const uint8_t*)pool_storage + tt_RX_POOL_BUFFER_BYTES);
    for (int k = 0; k < 2; k++) {
        const uint8_t* payload = cb.kept[k].sample.payload;
        EXPECT_EQ_U32(4, (uint32_t)((uintptr_t)payload % 8U));
        EXPECT_EQ_U32(0, (uint32_t)((uintptr_t)(payload + sizeof(uint32_t)) % _Alignof(struct wide_sample)));
        // What sits there is the sample's: value_encode() filled every byte after the first four with the value's low
        // byte, so the struct's uint64_t reads as that byte eight times.
        struct wide_sample wide;
        memcpy(&wide, payload + sizeof(uint32_t), sizeof(wide));
        uint8_t fill = (uint8_t)(cb.kept[k].value & 0xFFU);
        uint64_t expected = 0;
        memset(&expected, fill, sizeof(expected));
        EXPECT_EQ_U64(expected, wide.stamp);
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&rig.owner, &cb.kept[k].sample));
    }
    rig_down();
}

int main(void) {
    test_a_socket_sample_is_aligned_for_a_64_bit_type();
    test_a_retained_slot_survives_a_lap();
    test_unretained_records_are_released_as_read();
    test_a_held_slot_blocks_only_its_own_ring();
    test_skip_to_newest_never_frees_a_retained_slot();
    test_a_batch_record_is_released_with_its_last_sample();
    test_retain_and_release_in_one_callback();
    test_bad_releases_are_refused_and_touch_nothing();
    test_retain_outside_its_callback_is_refused();
    test_a_reassembled_sample_is_not_lent();
    test_the_segment_outlives_a_held_slot();
    test_a_farewell_read_in_place_releases_the_ring_after();
    test_release_from_another_thread();
    test_destroy_forgets_held_samples();
    test_socket_retain_without_a_pool_fails_cleanly();
    test_socket_retain_keeps_its_buffer();
    test_a_full_handle_table_fails_cleanly();
    test_socket_buffer_is_reused_when_not_retained();

    printf("test_sample_lending: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
#endif
