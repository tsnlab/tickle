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
    EXPECT_EQ_U32(4096, whole_record_limit_for(&sender, pub.peers, 1));

    uint32_t before = pub.seq_no;
    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    // Two: what the wire form needs. Not one, which is what this destination could have taken whole.
    EXPECT_EQ_U32(2, pub.seq_no - before);

    release_segments(&sender);
    release_own_segment(&owner);
    test_mock_segments_free();
}

int main(void) {
    test_without_a_segment_the_span_is_the_wire_count();
    test_a_wide_slot_does_not_shrink_the_span();

    printf("test_seq_span: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
