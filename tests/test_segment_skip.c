/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Skip to newest: the segment drain passes over a sample when a KEEP_LAST Subscriber already has `keep_last_depth`
// newer complete samples of the same writer queued behind it in the ring (plan_segment_skips(), tickle.c).
//
// Why it exists: rmw_tickle same-host, BEST_EFFORT KEEP_LAST 1 at maximum rate. The drain decoded and delivered
// 29.7M samples into the rmw queue, which overwrote all but about one per take; the application took 61k/s and the
// subscriber's CPU went into samples nobody saw. Stopping the drain at each delivered sample (origin/ab/drain-one-
// sample) cut the waste but read the ring in order, so under overload the application saw samples ~1.4 ms old
// instead of the newest. The target is both: the newest sample, and no work on the ones it supersedes.
//
// Every record here is written by the real publisher into the real ring, so the reader sees exactly what
// production writes - including the fragments of a sample wider than a slot.
#define tt_MAX_SAMPLE_LENGTH 4096

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define TOPIC_A_ID 0xaabbccddU
#define TOPIC_B_ID 0x11223344U
#define OWNER_IP 0x0a4d0002
#define OWNER_PORT 8282
#define OWNER_ID 7
#define OWNER_INCARNATION 0x12345678U
#define PEER_IP 0x0a000002
#define PEER_PORT 7000

// Larger than one default slot and smaller than two, so a big sample travels as two fragments.
#define BIG_SAMPLE_BYTES 2048
#define SMALL_SAMPLE_BYTES 64

static uint32_t sample_bytes = SMALL_SAMPLE_BYTES;

static int32_t value_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_bytes;
}
// The value in the first four bytes and filler after it, so a torn or misassembled sample decodes to a wrong value.
static int32_t value_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sample_bytes) {
        return -1;
    }
    memset(payload, 0xA5, sample_bytes);
    memcpy(payload, data, sizeof(uint32_t));
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

// What each Subscriber was handed, in order.
#define MAX_SEEN 64
struct seen {
    uint32_t values[MAX_SEEN];
    uint32_t count;
};
static struct seen seen_a;
static struct seen seen_b;

static void record(struct seen* seen, const struct tt_Data* data) {
    if (seen->count < MAX_SEEN) {
        memcpy(&seen->values[seen->count], data, sizeof(uint32_t));
    }
    seen->count++;
}
// A writer that keeps writing while the reader drains: when set, A's first delivery publishes one more sample.
static struct tt_Publisher* publish_during_delivery;
static uint32_t publish_during_delivery_value;

static void callback_a(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    record(&seen_a, data);
    if (publish_during_delivery != NULL) {
        struct tt_Publisher* pub = publish_during_delivery;
        publish_during_delivery = NULL;
        test_mock_now += 1000000;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(pub, (struct tt_Data*)&publish_during_delivery_value));
    }
}
static void callback_b(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    record(&seen_b, data);
}

#define REORDER_SLOTS 16
#define REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + BIG_SAMPLE_BYTES)
static uint64_t reorder_a[REORDER_SLOTS * REORDER_SLOT_BYTES / sizeof(uint64_t)];

struct rig {
    struct tt_Context owner;
    struct tt_Context sender;
    struct tt_Topic topic;   // the sender's
    struct tt_Topic topic_a; // the owner's
    struct tt_Topic topic_b;
    struct tt_Publisher pub_a;
    struct tt_Publisher pub_b;
    struct tt_Subscriber sub_a;
    struct tt_Subscriber sub_b;
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

static void init_sub(struct tt_Subscriber* sub, uint32_t endpoint_id, struct tt_Topic* topic,
                     tt_SUBSCRIBER_CALLBACK callback, bool reliable, uint16_t depth) {
    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = endpoint_id;
    sub->node = &rig.owner;
    sub->topic = topic;
    sub->callback = callback;
    sub->reliable = reliable;
    sub->keep_last_depth = depth;
    if (reliable) {
        memset(reorder_a, 0, sizeof(reorder_a));
        sub->reorder_storage = reorder_a;
        sub->reorder_slots = REORDER_SLOTS;
        sub->reorder_slot_bytes = REORDER_SLOT_BYTES;
    }
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
}

static void init_pub(struct tt_Publisher* pub, uint32_t endpoint_id, bool reliable) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = endpoint_id;
    pub->endpoint.name = "skip_pub";
    pub->node = &rig.sender;
    pub->topic = &rig.topic;
    pub->reliable = reliable;
    pub->endpoint.entity_id = endpoint_id ^ 0x5a5a5a5aU; // one writer per topic, as production tells them apart
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub->peers[0].context_id = OWNER_ID;
    pub->peers[0].ip = OWNER_IP;
    pub->peers[0].port = OWNER_PORT;
}

// The owner with its ring and Subscriber A (and B when b_depth >= 0), the sender with a publisher per topic.
static void rig_up(bool reliable, uint16_t a_depth, int b_depth) {
    memset(&rig, 0, sizeof(rig));
    memset(&seen_a, 0, sizeof(seen_a));
    memset(&seen_b, 0, sizeof(seen_b));
    sample_bytes = SMALL_SAMPLE_BYTES;
    test_mock_reset();

    struct tt_Context* owner = &rig.owner;
    node_init_locks(owner);
    owner->id = OWNER_ID;
    owner->entity_id_base = OWNER_INCARNATION;
    owner->hal.own_ip = OWNER_IP;
    owner->hal.own_port = OWNER_PORT;
    owner->tx_tail = sizeof(struct tt_Header);
    owner->tx_size = sizeof(owner->tx_buffer);
    owner->rx_seq_span = 1;
    create_own_segment(owner); // the default geometry: a BIG sample does not fit a slot
    EXPECT_TRUE(owner->own_segment != NULL);

    init_topic(&rig.topic_a, "topic_a");
    init_topic(&rig.topic_b, "topic_b");
    init_sub(&rig.sub_a, TOPIC_A_ID, &rig.topic_a, callback_a, reliable, a_depth);
    owner->endpoints[owner->endpoint_count++] = (struct tt_Endpoint*)&rig.sub_a;
    if (b_depth >= 0) {
        init_sub(&rig.sub_b, TOPIC_B_ID, &rig.topic_b, callback_b, false, (uint16_t)b_depth);
        owner->endpoints[owner->endpoint_count++] = (struct tt_Endpoint*)&rig.sub_b;
    }
    owner->endpoint_index_valid = false;

    struct tt_Context* sender = &rig.sender;
    node_init_locks(sender);
    sender->id = 1;
    sender->tx_tail = sizeof(struct tt_Header);
    sender->tx_size = sizeof(sender->tx_buffer);
    sender->hal.own_ip = PEER_IP;
    sender->hal.own_port = PEER_PORT;
    init_topic(&rig.topic, "skip_topic");
    init_pub(&rig.pub_a, TOPIC_A_ID, reliable);
    init_pub(&rig.pub_b, TOPIC_B_ID, false);
    sender->endpoints[sender->endpoint_count++] = (struct tt_Endpoint*)&rig.pub_a;
    sender->endpoints[sender->endpoint_count++] = (struct tt_Endpoint*)&rig.pub_b;
    EXPECT_TRUE(peer_segment(sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
}

static void rig_down(void) {
    release_segments(&rig.sender);
    release_own_segment(&rig.owner);
    test_mock_segments_free();
}

static void publish(struct tt_Publisher* pub, uint32_t value) {
    test_mock_now += 1000000; // each newer than the last, or the reader discards it for its timestamp
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(pub, (struct tt_Data*)&value));
}

static void drain_all(void) {
    bool emptied = false;
    for (int pass = 0; pass < 8; pass++) {
        (void)drain_own_segment(&rig.owner, &emptied);
        if (emptied) {
            return;
        }
    }
}

static struct tt_WriterProxy* only_proxy(struct tt_Subscriber* sub) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (sub->writers[i].context_id != tt_CONTEXT_ID_INVALID) {
            return &sub->writers[i];
        }
    }
    return NULL;
}

// A backlog of N samples for a KEEP_LAST 1 Subscriber: the newest is delivered, the N - 1 before it are skipped
// and counted - and every record is still counted as received over shared memory.
static void test_keep_last_1_takes_only_the_newest(void) {
    rig_up(false, 1, -1);
    enum { N = 6 };
    for (uint32_t i = 0; i < N; i++) {
        publish(&rig.pub_a, 100 + i);
    }
    EXPECT_EQ_U32(N, rig.owner.own_segment->write_index);
    drain_all();

    EXPECT_EQ_U32(1, seen_a.count);
    EXPECT_EQ_U32(100 + N - 1, seen_a.values[0]);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U32(N - 1, rig.sub_a.superseded);
    EXPECT_EQ_U64(N, rig.owner.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U32(N, rig.owner.own_segment->read_index); // all of it gone from the ring
    // Handed over with the delivered sample, for a layer that counts gaps as loss.
    EXPECT_EQ_U32(N - 1, rig.sub_a.delivering_superseded);
    EXPECT_EQ_U32(0, rig.sub_a.superseded_pending);
    rig_down();
}

// KEEP_LAST 2: the newest two, in order.
static void test_keep_last_2_takes_the_newest_two(void) {
    rig_up(false, 2, -1);
    enum { N = 6 };
    for (uint32_t i = 0; i < N; i++) {
        publish(&rig.pub_a, 200 + i);
    }
    drain_all();
    EXPECT_EQ_U32(2, seen_a.count);
    EXPECT_EQ_U32(200 + N - 2, seen_a.values[0]);
    EXPECT_EQ_U32(200 + N - 1, seen_a.values[1]);
    EXPECT_EQ_U64(N - 2, rig.owner.rx_shm_skipped_superseded);
    rig_down();
}

// KEEP_ALL (depth 0): every sample, in order, nothing skipped. The control for the two above: same producer, same
// ring, only the depth differs.
static void test_keep_all_takes_everything_in_order(void) {
    rig_up(false, 0, -1);
    enum { N = 6 };
    for (uint32_t i = 0; i < N; i++) {
        publish(&rig.pub_a, 300 + i);
    }
    drain_all();
    EXPECT_EQ_U32(N, seen_a.count);
    for (uint32_t i = 0; i < N && i < MAX_SEEN; i++) {
        EXPECT_EQ_U32(300 + i, seen_a.values[i]);
    }
    EXPECT_EQ_U64(0, rig.owner.rx_shm_skipped_superseded);
    rig_down();
}

// Two topics interleaved in one ring: A is KEEP_LAST 1, B is KEEP_ALL. A gets its newest; B gets every sample in
// order, though A's skipped records sit between them.
static void test_a_mixed_backlog_keeps_the_keep_all_topic_complete(void) {
    rig_up(false, 1, 0);
    enum { N = 5 };
    for (uint32_t i = 0; i < N; i++) {
        publish(&rig.pub_a, 400 + i);
        publish(&rig.pub_b, 500 + i);
    }
    drain_all();
    EXPECT_EQ_U32(1, seen_a.count);
    EXPECT_EQ_U32(400 + N - 1, seen_a.values[0]);
    EXPECT_EQ_U32(N, seen_b.count);
    for (uint32_t i = 0; i < N && i < MAX_SEEN; i++) {
        EXPECT_EQ_U32(500 + i, seen_b.values[i]);
    }
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U32(0, rig.sub_b.superseded);
    rig_down();
}

// Fragmented samples. Three two-fragment samples, the last one's continuation claimed by its writer and not yet
// published - a writer mid-copy. The first is superseded by the second (complete) and skipped, both its records;
// the second is delivered; the third is partial and must not be skipped or counted as newer, and once its
// continuation lands it is delivered whole with its own value.
static void test_a_partial_fragmented_sample_is_not_skipped(void) {
    rig_up(false, 1, -1);
    sample_bytes = BIG_SAMPLE_BYTES;
    publish(&rig.pub_a, 600);
    publish(&rig.pub_a, 601);
    publish(&rig.pub_a, 602);
    struct tt_SegmentHeader* ring = rig.owner.own_segment;
    EXPECT_EQ_U32(6, ring->write_index); // two records per sample, or this is not the fragment path
    // Unpublish the last record: claimed (write_index past it) but its sequence not yet the publish value.
    struct tt_SegmentSlot* last = (struct tt_SegmentSlot*)segment_slot(ring, 5);
    EXPECT_EQ_U32(6, last->sequence);
    last->sequence = 5;

    bool emptied = true;
    (void)drain_own_segment(&rig.owner, &emptied);
    EXPECT_EQ_U32(5, ring->read_index);
    EXPECT_EQ_U32(1, seen_a.count);
    EXPECT_EQ_U32(601, seen_a.values[0]);
    EXPECT_EQ_U64(1, rig.owner.rx_shm_skipped_superseded); // 600 only; 602 was partial
    // 600's continuation went with it: nothing of it waits in the reassembly pool to be abandoned later.
    EXPECT_TRUE(!frag_pool_has(&rig.owner, rig.sender.id, rig.pub_a.endpoint.entity_id, 1));

    last->sequence = 6; // the writer finishes
    drain_all();
    EXPECT_EQ_U32(2, seen_a.count);
    EXPECT_EQ_U32(602, seen_a.values[1]);
    EXPECT_EQ_U64(1, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U64(0, rig.owner.frag_dropped);
    rig_down();
}

// Reliable KEEP_LAST 1. The first sample is taken the ordinary way (it creates the writer's tracking); then a
// backlog of N. Only the newest is delivered, and the skipped ones count as received: the watermark stands past all
// of them with nothing missing, so nothing is held and nothing will be asked for again.
static void test_reliable_keep_last_does_not_nack_skipped_samples(void) {
    rig_up(true, 1, -1);
    publish(&rig.pub_a, 700);
    drain_all();
    EXPECT_EQ_U32(1, seen_a.count);
    enum { N = 5 };
    for (uint32_t i = 1; i <= N; i++) {
        publish(&rig.pub_a, 700 + i);
    }
    int sends_before = test_mock_send_call_count + test_mock_send_to_call_count;
    drain_all();
    EXPECT_EQ_U32(2, seen_a.count);
    EXPECT_EQ_U32(700 + N, seen_a.values[1]);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded);
    struct tt_WriterProxy* proxy = only_proxy(&rig.sub_a);
    EXPECT_TRUE(proxy != NULL);
    if (proxy != NULL) {
        EXPECT_EQ_U32(N + 2, proxy->ack_seq_no); // seq_nos 1 .. N+1 all received
        EXPECT_EQ_U32(N + 1, proxy->highest_delivered);
        bool any_bit = false;
        for (uint32_t w = 0; w < proxy_words(proxy); w++) {
            any_bit = any_bit || proxy->received_bitmap[w] != 0;
        }
        EXPECT_TRUE(!any_bit); // no sample recorded ahead of a gap
    }
    EXPECT_EQ_U32(0, rig.sub_a.reorder_held);
    EXPECT_EQ_INT(sends_before, test_mock_send_call_count + test_mock_send_to_call_count); // no ACKNACK
    rig_down();
}

// The same for fragmented samples: each fragment is its own seq_no, and a skipped sample's are all received.
static void test_reliable_keep_last_skips_whole_fragmented_samples(void) {
    rig_up(true, 1, -1);
    sample_bytes = BIG_SAMPLE_BYTES;
    publish(&rig.pub_a, 800);
    drain_all();
    EXPECT_EQ_U32(1, seen_a.count);
    enum { N = 3 };
    for (uint32_t i = 1; i <= N; i++) {
        publish(&rig.pub_a, 800 + i);
    }
    drain_all();
    EXPECT_EQ_U32(2, seen_a.count);
    EXPECT_EQ_U32(800 + N, seen_a.values[1]);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded);
    struct tt_WriterProxy* proxy = only_proxy(&rig.sub_a);
    EXPECT_TRUE(proxy != NULL);
    if (proxy != NULL) {
        EXPECT_EQ_U32((2 * (N + 1)) + 1, proxy->ack_seq_no);
    }
    EXPECT_EQ_U32(0, rig.sub_a.reorder_held);
    EXPECT_EQ_U32(0, rig.sub_a.reorder_abandoned); // no continuation of a skipped sample was held on its own
    rig_down();
}

// The writer publishes a newer sample while the drain is delivering. Planning again in the same poll would deliver it
// too, overwriting in the reader's history the sample just handed over before anyone took it (7.6M delivered into the
// rmw queue for 1.7M taken, measured without this). The drain hands back after the plan that delivered, and the next
// poll takes the newer one.
static void test_the_drain_hands_back_after_delivering_the_newest(void) {
    rig_up(false, 1, -1);
    for (uint32_t i = 0; i < 4; i++) {
        publish(&rig.pub_a, 900 + i);
    }
    publish_during_delivery = &rig.pub_a;
    publish_during_delivery_value = 904;
    bool emptied = true;
    EXPECT_TRUE(drain_own_segment(&rig.owner, &emptied) > 0);
    EXPECT_EQ_U32(1, seen_a.count);
    EXPECT_EQ_U32(903, seen_a.values[0]);
    EXPECT_TRUE(!emptied); // 904 is waiting, and the caller is told so
    EXPECT_EQ_U32(4, rig.owner.own_segment->read_index);

    (void)drain_own_segment(&rig.owner, &emptied);
    EXPECT_EQ_U32(2, seen_a.count);
    EXPECT_EQ_U32(904, seen_a.values[1]);
    EXPECT_TRUE(emptied);
    rig_down();
}

// The control: a KEEP_ALL reader's drain is not stopped by a delivery - the same writer, the same mid-drain publish,
// and one poll takes all five in order.
static void test_a_keep_all_drain_is_not_handed_back(void) {
    rig_up(false, 0, -1);
    for (uint32_t i = 0; i < 4; i++) {
        publish(&rig.pub_a, 950 + i);
    }
    publish_during_delivery = &rig.pub_a;
    publish_during_delivery_value = 954;
    bool emptied = false;
    (void)drain_own_segment(&rig.owner, &emptied);
    EXPECT_EQ_U32(5, seen_a.count);
    EXPECT_EQ_U32(954, seen_a.values[4]);
    EXPECT_TRUE(emptied);
    rig_down();
}

// The plan's leading run (segment_skip_run(), tickle.c): a backlog of N samples of one writer for a KEEP_LAST 1
// reader plans its first N - 1 records as one run, which the drain passes over together before it reads the newest.
// Its accounting is the record-by-record one: the same counts test_keep_last_1_takes_only_the_newest() reads.
static void test_one_writers_backlog_is_one_leading_run(void) {
    rig_up(false, 1, -1);
    enum { N = 6 };
    for (uint32_t i = 0; i < N; i++) {
        publish(&rig.pub_a, 1000 + i);
    }
    state_lock(&rig.owner);
    plan_segment_skips(&rig.owner, 0);
    state_unlock(&rig.owner);
    EXPECT_EQ_U32(N, rig.owner.segment_plan_count);
    EXPECT_EQ_U32(N - 1, rig.owner.segment_plan_run.count);
    EXPECT_EQ_U32(TOPIC_A_ID, rig.owner.segment_plan_run.endpoint_id);
    EXPECT_EQ_U32(rig.pub_a.endpoint.entity_id, rig.owner.segment_plan_run.entity_id);
    rig.owner.segment_plan_count = 0; // the drain plans for itself
    drain_all();
    EXPECT_EQ_U32(1, seen_a.count);
    EXPECT_EQ_U32(1000 + N - 1, seen_a.values[0]);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U32(N - 1, rig.sub_a.superseded);
    EXPECT_EQ_U32(N - 1, rig.sub_a.delivering_superseded);
    EXPECT_EQ_U64(N, rig.owner.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U32(N, rig.owner.own_segment->read_index);
    for (uint32_t i = 0; i < N; i++) { // every slot handed back to the writers, one lap ahead
        const struct tt_SegmentSlot* slot = (const struct tt_SegmentSlot*)segment_slot(rig.owner.own_segment, i);
        EXPECT_EQ_U32(i + rig.owner.own_segment->slots, slot->sequence);
    }
    rig_down();
}

// Two KEEP_LAST 1 writers interleaved: a run is one writer's, so neither writer's skipped samples are counted as the
// other's. Each Subscriber gets its newest and is told of exactly its own N - 1 passed over.
static void test_interleaved_writers_are_counted_apart(void) {
    rig_up(false, 1, 1);
    enum { N = 5 };
    for (uint32_t i = 0; i < N; i++) {
        publish(&rig.pub_a, 1100 + i);
        publish(&rig.pub_b, 1200 + i);
    }
    drain_all();
    EXPECT_EQ_U32(1, seen_a.count);
    EXPECT_EQ_U32(1100 + N - 1, seen_a.values[0]);
    EXPECT_EQ_U32(1, seen_b.count);
    EXPECT_EQ_U32(1200 + N - 1, seen_b.values[0]);
    EXPECT_EQ_U32(N - 1, rig.sub_a.superseded);
    EXPECT_EQ_U32(N - 1, rig.sub_b.superseded);
    EXPECT_EQ_U64(2ULL * (N - 1), rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U64(2ULL * N, rig.owner.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    rig_down();
}

// Two writers of one topic in one context, offering different QoS (2026-10-09). The plan and the skip judge each
// record's Subscribers by the writer that wrote it (the record's entity_id): `kept` offers a deadline the reader
// accepts, `missed` one it refuses. Judged by the first publisher of the endpoint in the discovery table instead,
// the pair are judged alike: with `missed` first, the reader is taken for incompatible with `kept` too, nothing of
// `kept` is skipped and every one of its samples is delivered; with `kept` first, `missed`'s older samples are
// skipped and counted as superseded for a writer whose samples it drops. Both orders, both backlog shapes: the
// writers interleaved (record by record) and one after the other (the leading run, segment_skip_run()).
#define REQUESTED_DEADLINE_NS (100ULL * tt_MILLISECOND)
#define KEPT_DEADLINE_NS (50ULL * tt_MILLISECOND) // within what the reader requests
#define MISSED_DEADLINE_NS 0ULL                   // infinite: looser than requested, so incompatible
#define MISSED_ENTITY_ID 0x0badf00dU

static struct tt_Discovery skip_discovery;
static struct tt_Publisher pub_missed;

static void announce_writer(const struct tt_Publisher* pub, uint64_t deadline_ns) {
    upsert_discovered_entity(&rig.owner, rig.sender.id, pub->endpoint.id, pub->endpoint.entity_id,
                             tt_KIND_TOPIC_PUBLISHER, 0, 0, deadline_ns, 0, "skip_topic", "skip_pub");
}

static void two_writers_of_one_topic(bool missed_first, bool interleaved) {
    rig_up(false, 1, -1);
    rig.sub_a.deadline_duration_ns = REQUESTED_DEADLINE_NS;
    init_pub(&pub_missed, TOPIC_A_ID, false);
    pub_missed.endpoint.entity_id = MISSED_ENTITY_ID;
    rig.sender.endpoints[rig.sender.endpoint_count++] = (struct tt_Endpoint*)&pub_missed;
    memset(&skip_discovery, 0, sizeof(skip_discovery));
    rig.owner.discovery = &skip_discovery;
    if (missed_first) {
        announce_writer(&pub_missed, MISSED_DEADLINE_NS);
        announce_writer(&rig.pub_a, KEPT_DEADLINE_NS);
    } else {
        announce_writer(&rig.pub_a, KEPT_DEADLINE_NS);
        announce_writer(&pub_missed, MISSED_DEADLINE_NS);
    }
    EXPECT_TRUE(tt_Discovery_find_entity(&skip_discovery, rig.sender.id, TOPIC_A_ID, MISSED_ENTITY_ID) != NULL);
    EXPECT_TRUE(tt_Discovery_find_entity(&skip_discovery, rig.sender.id, TOPIC_A_ID, rig.pub_a.endpoint.entity_id) !=
                NULL);

    enum { N = 4 };
    if (interleaved) {
        for (uint32_t i = 0; i < N; i++) {
            publish(&rig.pub_a, 1300 + i);
            publish(&pub_missed, 1400 + i);
        }
    } else {
        for (uint32_t i = 0; i < N; i++) {
            publish(&rig.pub_a, 1300 + i);
        }
        for (uint32_t i = 0; i < N; i++) {
            publish(&pub_missed, 1400 + i);
        }
    }
    EXPECT_EQ_U32(2 * N, rig.owner.own_segment->write_index); // all of it over shared memory
    drain_all();

    // Only the compatible writer's newest sample is delivered and its older ones are skipped; the refused writer's
    // are read and dropped by RxO, none of them skipped or counted as passed over.
    EXPECT_EQ_U32(1, seen_a.count);
    EXPECT_EQ_U32(1300 + N - 1, seen_a.values[0]);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U32(N - 1, rig.sub_a.superseded);
    EXPECT_EQ_U32(N - 1, rig.sub_a.delivering_superseded);
    EXPECT_EQ_U32(N, rig.sub_a.rxo_drops);
    EXPECT_TRUE(find_writer_proxy(&rig.sub_a, rig.sender.id, MISSED_ENTITY_ID) == NULL);
    EXPECT_EQ_U64(2ULL * N, rig.owner.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U32(2 * N, rig.owner.own_segment->read_index);
    rig.owner.discovery = NULL;
    rig_down();
}

static void test_two_writers_of_one_topic_are_judged_apart(void) {
    two_writers_of_one_topic(true, true);
    two_writers_of_one_topic(false, true);
    two_writers_of_one_topic(true, false);
    two_writers_of_one_topic(false, false);
}

int main(void) {
    test_keep_last_1_takes_only_the_newest();
    test_one_writers_backlog_is_one_leading_run();
    test_interleaved_writers_are_counted_apart();
    test_keep_last_2_takes_the_newest_two();
    test_keep_all_takes_everything_in_order();
    test_a_mixed_backlog_keeps_the_keep_all_topic_complete();
    test_a_partial_fragmented_sample_is_not_skipped();
    test_reliable_keep_last_does_not_nack_skipped_samples();
    test_reliable_keep_last_skips_whole_fragmented_samples();
    test_the_drain_hands_back_after_delivering_the_newest();
    test_a_keep_all_drain_is_not_handed_back();
    test_two_writers_of_one_topic_are_judged_apart();

    printf("test_segment_skip: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
