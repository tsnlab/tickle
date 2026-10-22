/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The segment drain's skip-to-newest and the hybrid reader's loss count, together: samples the drain passes over as
// superseded were received, and the jump to the next sample read is not loss (hybrid_rx_missed(), tickle.c).
//
// Why it exists: rmw_tickle same-host, Array4k (three MTU units a sample) BEST_EFFORT KEEP_LAST 1 at maximum rate.
// Every skip of up to eight samples was counted as that many samples lost whole: 2.2M "missed" samples in 20 s on the
// rig, a p of 0.63 announced for shared memory, and two getsockopt(SO_MEMINFO) calls per gap on the drain's thread -
// about 13 system calls per delivered sample on the PC. How many skips landed within the limit followed from the
// reader's pace, so the executor hold-over, which made rmw_wait() faster, read 22% slower on the rig (2026-10-20).
//
// Built as rmw_tickle builds core (a 65507-byte datagram, large samples): the hybrid reader exists only there. A
// 4096-byte sample travels the ring as three fragments, as Array4k does.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define TOPIC_ID 0xaabbccddU
#define OWNER_IP 0x0a4d0002
#define OWNER_PORT 8282
#define OWNER_ID 7
#define OWNER_INCARNATION 0x12345678U
#define PEER_IP 0x0a000002
#define PEER_PORT 7000
#define SAMPLE_BYTES 4096U // Array4k's payload: three fragments of a default slot
#define UNITS 3U           // ... each its own seq_no

static int32_t value_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)SAMPLE_BYTES;
}
static int32_t value_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < SAMPLE_BYTES) {
        return -1;
    }
    memset(payload, 0xA5, SAMPLE_BYTES);
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)SAMPLE_BYTES;
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

#define MAX_SEEN 64
static uint32_t seen[MAX_SEEN];
static uint32_t seen_count;

static void callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    if (seen_count < MAX_SEEN) {
        memcpy(&seen[seen_count], data, sizeof(uint32_t));
    }
    seen_count++;
}

struct rig {
    struct tt_Context owner;
    struct tt_Context sender;
    struct tt_TypeSupport topic;     // the sender's
    struct tt_TypeSupport own_topic; // the owner's
    struct tt_Publisher pub;
    struct tt_Subscriber sub;
};
static struct rig rig;

static void init_topic(struct tt_TypeSupport* topic, const char* name) {
    memset(topic, 0, sizeof(*topic));
    topic->name = name;
    topic->data_size = sizeof(uint32_t);
    topic->data_encode_size = value_encode_size;
    topic->data_encode = value_encode;
    topic->data_decode = value_decode;
    topic->data_free = value_free;
}

// The owner with its ring, its hybrid reader started as tt_Context_init() starts it, and a BEST_EFFORT Subscriber of
// `depth` (0: KEEP_ALL); the sender with one publisher whose only peer is the owner.
static void rig_up(uint16_t depth) {
    memset(&rig, 0, sizeof(rig));
    memset(seen, 0, sizeof(seen));
    seen_count = 0;
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
    owner->ip_reassembly_max = advertised_ip_reassembly_max(net_ip_reassembly_max(owner));
    hybrid_reader_start(owner);
    EXPECT_TRUE(hybrid_rx_capable(owner)); // or nothing below is measured
    create_own_segment(owner);
    EXPECT_TRUE(owner->own_segment != NULL);

    init_topic(&rig.own_topic, "hybrid_skip_topic");
    struct tt_Subscriber* sub = &rig.sub;
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = TOPIC_ID;
    sub->node = owner;
    sub->topic = &rig.own_topic;
    sub->callback = callback;
    sub->endpoint.name = "hybrid_skip_sub";
    sub->keep_last_depth = depth;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    owner->endpoints[owner->endpoint_count++] = (struct tt_Endpoint*)sub;
    owner->endpoint_index_mask = 0;

    struct tt_Context* sender = &rig.sender;
    node_init_locks(sender);
    sender->id = 1;
    sender->tx_tail = sizeof(struct tt_Header);
    sender->tx_size = sizeof(sender->tx_buffer);
    sender->hal.own_ip = PEER_IP;
    sender->hal.own_port = PEER_PORT;
    init_topic(&rig.topic, "hybrid_skip_topic");
    struct tt_Publisher* pub = &rig.pub;
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = TOPIC_ID;
    pub->endpoint.name = "hybrid_skip_pub";
    pub->endpoint.entity_id = TOPIC_ID ^ 0x5a5a5a5aU;
    pub->node = sender;
    pub->topic = &rig.topic;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub->peers[0].context_id = OWNER_ID;
    pub->peers[0].ip = OWNER_IP;
    pub->peers[0].port = OWNER_PORT;
    sender->endpoints[sender->endpoint_count++] = (struct tt_Endpoint*)pub;
    EXPECT_TRUE(peer_segment(sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
}

static void rig_down(void) {
    release_segments(&rig.sender);
    release_own_segment(&rig.owner);
    test_mock_segments_free();
}

static void publish(uint32_t value) {
    test_mock_now += 1000000; // each newer than the last, or the reader discards it for its timestamp
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&rig.pub, (struct tt_Data*)&value));
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

// The first sample read gives the hybrid reader the writer to jump from; then a backlog of N. Shape checks first: the
// sample is three records, and the backlog is skipped down to its newest - or the case under test did not happen.
static void first_sample_then_backlog(uint16_t depth, uint32_t backlog, uint32_t base) {
    rig_up(depth);
    publish(base);
    EXPECT_EQ_U32(UNITS, rig.owner.own_segment->write_index); // three fragments, or this is not Array4k's path
    drain_all();
    EXPECT_EQ_U32(1, seen_count);
    for (uint32_t i = 1; i <= backlog; i++) {
        publish(base + i);
    }
    drain_all();
}

// The case: KEEP_LAST 1 behind a backlog of six. Five are passed over and the newest read - and none of the five is a
// lost sample: no missed sample, no loss event, nothing put down to a receiver drop.
static void test_superseded_samples_are_not_missed(void) {
    enum { N = 6 };
    first_sample_then_backlog(1, N, 100);
    EXPECT_EQ_U32(2, seen_count);
    EXPECT_EQ_U32(100 + N, seen[1]);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded); // the shape: they were skipped, not read
    EXPECT_EQ_U64(0, rig.owner.hybrid.missed_samples);
    EXPECT_EQ_U32(0, rig.owner.hybrid.rx_loss.events);
    EXPECT_EQ_U64(0, rig.owner.hybrid.receiver_lost);
    rig_down();
}

// More superseded samples than HYBRID_MISSED_MAX: the same. Then one sample truly lost inside such a backlog: the jump
// is longer than the limit, but what the skips do not explain is one sample, and that one is counted - the limit is
// on samples unaccounted for, not on the jump.
static void test_a_long_backlog_is_not_missed(void) {
    enum { N = 12 };
    first_sample_then_backlog(1, N, 200);
    EXPECT_EQ_U32(2, seen_count);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U64(0, rig.owner.hybrid.missed_samples);
    EXPECT_EQ_U32(0, rig.owner.hybrid.rx_loss.events);
    rig_down();

    first_sample_then_backlog(1, 0, 250);
    for (uint32_t i = 1; i <= N; i++) {
        if (i == N - 1) {
            rig.pub.seq_no += UNITS; // one sample's seq_nos, never sent
        }
        publish(250 + i);
    }
    drain_all();
    EXPECT_EQ_U32(2, seen_count);
    EXPECT_EQ_U32(250 + N, seen[1]);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded); // a jump of N samples: N - 1 skipped, one lost
    EXPECT_EQ_U64(1, rig.owner.hybrid.missed_samples);
    rig_down();
}

// The control the change must not touch: a sample the reader never received - its seq_nos taken by the writer and
// nothing written - is still a sample lost whole, with nothing skipped to stand for it. KEEP_ALL, so nothing is
// superseded; then the same after a skipped backlog, so a credit spent on one jump is not left for the next.
static void test_a_sample_never_received_is_still_missed(void) {
    rig_up(0);
    publish(300);
    drain_all();
    rig.pub.seq_no += UNITS; // one sample's seq_nos, never sent
    publish(301);
    drain_all();
    EXPECT_EQ_U32(2, seen_count);
    EXPECT_EQ_U64(0, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U64(1, rig.owner.hybrid.missed_samples);
    rig_down();

    enum { N = 4 };
    first_sample_then_backlog(1, N, 400); // N - 1 skipped, credited and spent on this jump
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded);
    EXPECT_EQ_U64(0, rig.owner.hybrid.missed_samples);
    rig.pub.seq_no += UNITS;
    publish(400 + N + 1);
    drain_all();
    EXPECT_EQ_U32(3, seen_count);
    EXPECT_EQ_U64(N - 1, rig.owner.rx_shm_skipped_superseded); // nothing more skipped
    EXPECT_EQ_U64(1, rig.owner.hybrid.missed_samples);
    rig_down();
}

int main(void) {
    test_superseded_samples_are_not_missed();
    test_a_long_backlog_is_not_missed();
    test_a_sample_never_received_is_still_missed();

    printf("test_segment_skip_hybrid: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
