/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The transport seam (SHM_PLAN.md stage 0), before any transport exists to choose.
//
// Stage 0 adds no segment and sends nothing new, so there is exactly one behavioural claim to make
// and it is worth making now rather than at stage 3: **every datagram goes out as UDP and is
// counted, and the shared-memory count stays at zero.** That is a real assertion about the seam
// being wired, and - more to the point - it is what proves the counters themselves work *before*
// anything depends on them. Without it, the first use of these counters would also be their first
// test, which is the arrangement that has produced several false passes in this project.
//
// The counters exist because a transport test cannot ask "which transport was selected". A seam
// wired for tt_send() but not for tt_send_iov()/tt_send_batch() would answer that question
// correctly and still put fragmented samples and batched acks on the old path. Only "how many
// datagrams went each way" catches that, so that is what is counted, at the seam, where every send
// converges.
//
// g15 is pinned here too: tt_Context.tx_datagrams used to be incremented at the call sites and
// publish_zerocopy() was missed, so five datagrams left the node while the counter said none had.
// The seam makes coverage structural, and the zerocopy arm below is the regression guard - it is
// the path shared memory has the most to offer, so a counter blind there would report shm at zero
// for a payload that really did travel over the segment.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: the seam and publish_zerocopy() are static, same approach as the other whitebox tests.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define ENDPOINT_ID 0xaabbccdd
#define PEER_CONTEXT_ID 2
#define PEER_IP 0x0a000002
#define PEER_PORT 7000
#define SAMPLES 5
#define ZEROCOPY_BODY 16

static int32_t stub_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void stub_free(struct tt_Data* data) {
    (void)data;
}

static void init_node_topic_pub(struct tt_Context* node, struct tt_Topic* topic, struct tt_Publisher* pub) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = 1;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;

    memset(topic, 0, sizeof(*topic));
    topic->name = "seam_topic";
    topic->data_size = sizeof(uint32_t);
    topic->data_encode_size = stub_encode_size;
    topic->data_encode = stub_encode;
    topic->data_free = stub_free;

    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.name = "seam_pub";
    pub->node = node;
    pub->topic = topic;
    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)pub;
}

// Every datagram the mock was handed is counted, and counted as UDP. The mock's own count is the
// control: without it "tx_datagrams went up by five" would be consistent with five datagrams, with
// fifty, or with none actually reaching the transport.
static void expect_all_udp(struct tt_Context* node, const char* what) {
    EXPECT_EQ_U32((uint32_t)test_mock_send_call_count, (uint32_t)node->tx_datagrams);
    EXPECT_EQ_U32((uint32_t)test_mock_send_call_count, (uint32_t)node->tx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    EXPECT_EQ_U32(0, (uint32_t)node->tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    // The invariant that survives stage 1, when shm stops being zero: the split and the total come
    // from the same seam and must agree. A reader who sees them differ is looking at a counting
    // defect, not a transport story - which is what makes the traffic line's tx_udp/tx_shm
    // self-checking against the tx_datagrams printed beside them.
    uint64_t split = 0;
    for (int transport = 0; transport < tt_TRANSPORT_COUNT; transport++) {
        split += node->tx_datagrams_by_transport[transport];
    }
    EXPECT_EQ_U32((uint32_t)node->tx_datagrams, (uint32_t)split);
    if (test_mock_send_call_count == 0) {
        printf("  %s: nothing was sent, so this arm asserts nothing\n", what);
    }
}

// The ordinary publish path: encode, flush, send.
static void test_ordinary_publish_is_counted_as_udp(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);

    for (uint32_t i = 0; i < SAMPLES; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }

    EXPECT_TRUE(test_mock_send_call_count > 0); // the arm sent something, so it can fail
    expect_all_udp(&node, "ordinary publish");
}

// g15's path. Before the seam this sent datagrams the counter never saw.
static void test_zerocopy_publish_is_counted_as_udp(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub.peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub.peers[0].context_id = PEER_CONTEXT_ID;
    pub.peers[0].ip = PEER_IP;
    pub.peers[0].port = PEER_PORT;

    uint8_t body[ZEROCOPY_BODY] = {0};
    for (int i = 0; i < SAMPLES; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)publish_zerocopy(&pub, body, (uint32_t)sizeof(body)));
    }

    EXPECT_EQ_INT(SAMPLES, test_mock_send_call_count);
    expect_all_udp(&node, "zerocopy publish");
}

// The batch shape. send_datagram() sends to one destination through send_datagram_to() and to
// several as one seam_send_batch(), and a batch is several datagrams in one call - so it is the
// shape most likely to be counted once instead of per datagram, which is exactly the under-count
// that would make a half-wired seam look fine.
//
// Reached with two peers rather than with a fragmenting payload, deliberately. send_fragments()
// uses the same seam_send_batch(), but fragmentation is compiled in only by defining
// tt_MAX_SAMPLE_LENGTH above tt_MAX_BUFFER_LENGTH before including tickle.h, which this binary does
// not - so an arm written against fragments here would compile away to nothing while looking like
// coverage. Two peers exercise the same seam function unconditionally.
//
// The fragment case is not left unguarded: tests/test_data_frag.c is built with fragmentation in
// and asserts the per-fragment datagram count directly. That is not a claim from reading - a mutant
// counting a batch once instead of per datagram fails test_data_frag.c:508/523/550 before this file
// even runs.
static void test_batch_shape_is_counted_per_datagram(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub.peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub.peers[0].context_id = PEER_CONTEXT_ID;
    pub.peers[0].ip = PEER_IP;
    pub.peers[0].port = PEER_PORT;
    pub.peers[1].context_id = PEER_CONTEXT_ID + 1;
    pub.peers[1].ip = PEER_IP + 1;
    pub.peers[1].port = PEER_PORT;

    for (uint32_t i = 0; i < SAMPLES; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }

    // The arm only means something if a batch actually happened: without this, two peers that
    // resolved to one destination would send one datagram each and the assertion below would pass
    // while testing the single-destination path a second time.
    EXPECT_TRUE(test_mock_send_batch_call_count > 0);
    EXPECT_TRUE(test_mock_send_call_count > SAMPLES); // more datagrams than samples, i.e. per peer
    expect_all_udp(&node, "batch shape");
}

// The defect that got past the seam: tt_Context is caller-owned and reset_node_state() initialises
// field by field, so a field added to the struct and not to that function is never zeroed - it is
// whatever the caller's memory held, plus increments. The per-transport arrays were added that way
// and read as stack addresses (a run that sent 949,078 datagrams reported tx_udp=140723338891185)
// until they were reset beside their scalars.
//
// It is worth a test rather than a fix alone because the class recurs: every future field here has
// the same trap, and a counter starting from garbage cannot be told from one the seam never
// reached - the false negative shaped exactly like the failure these counters exist to catch. The
// arm fills the context with a non-zero pattern first, because a context that starts zeroed cannot
// fail this no matter how many fields the reset forgets.
static void test_reset_zeroes_the_per_transport_counters(void) {
    struct tt_Context node;
    memset(&node, 0xAA, sizeof(node));
    node_init_locks(&node);
    reset_node_state(&node);

    for (int transport = 0; transport < tt_TRANSPORT_COUNT; transport++) {
        EXPECT_EQ_U32(0, (uint32_t)node.tx_datagrams_by_transport[transport]);
        EXPECT_EQ_U32(0, (uint32_t)node.rx_datagrams_by_transport[transport]);
    }
    for (int reason = 0; reason < tt_SEGMENT_ATTACH_COUNT; reason++) {
        EXPECT_EQ_U32(0, node.segment_attach[reason]); // added later than the pair above, same trap
    }
    EXPECT_EQ_U32(0, (uint32_t)node.tx_datagrams); // the scalar they must stay beside
    EXPECT_EQ_U32(0, (uint32_t)node.rx_datagrams);
}

#define SEG_NAME_MAX 128
#define OWNER_IP 0x0a4d0002
#define OWNER_PORT 8282
#define OWNER_ID 7
#define OWNER_INCARNATION 0x12345678U

static struct tt_SegmentHeader valid_header(void) {
    struct tt_SegmentHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = tt_SEGMENT_MAGIC;
    header.version = tt_SEGMENT_VERSION;
    header.owner_ip = OWNER_IP;
    header.owner_port = OWNER_PORT;
    header.owner_context_id = OWNER_ID;
    header.incarnation = OWNER_INCARNATION;
    return header;
}

// The name is built from what discovery already knows, and its whole job is to be different for
// peers that are different. The two namespaces case is the one it exists for: same context id,
// different address, and the registry keys on the address for the same reason.
static void test_segment_name_separates_peers_that_differ(void) {
    char a[SEG_NAME_MAX];
    char b[SEG_NAME_MAX];

    EXPECT_TRUE(segment_name(a, sizeof(a), OWNER_IP, OWNER_PORT, OWNER_ID) > 0);

    // Same id, different address - two network namespaces on one host.
    EXPECT_TRUE(segment_name(b, sizeof(b), OWNER_IP + 1, OWNER_PORT, OWNER_ID) > 0);
    EXPECT_TRUE(strcmp(a, b) != 0);

    // Same address and id, different port.
    EXPECT_TRUE(segment_name(b, sizeof(b), OWNER_IP, OWNER_PORT + 1, OWNER_ID) > 0);
    EXPECT_TRUE(strcmp(a, b) != 0);

    // Same address and port, different id - two contexts in one namespace.
    EXPECT_TRUE(segment_name(b, sizeof(b), OWNER_IP, OWNER_PORT, OWNER_ID + 1) > 0);
    EXPECT_TRUE(strcmp(a, b) != 0);

    // And the same peer twice is the same name, or a reader could never find a writer at all.
    EXPECT_TRUE(segment_name(b, sizeof(b), OWNER_IP, OWNER_PORT, OWNER_ID) > 0);
    EXPECT_EQ_INT(0, strcmp(a, b));

    // A buffer too small is refused rather than truncated: a truncated name is a name two different
    // peers could share, which is the one outcome the naming exists to prevent.
    char tiny[8];
    EXPECT_TRUE(segment_name(tiny, sizeof(tiny), OWNER_IP, OWNER_PORT, OWNER_ID) < 0);
}

// What the name cannot see, and therefore what the header is for.
static void test_segment_header_catches_what_the_name_cannot(void) {
    struct tt_SegmentHeader header = valid_header();

    // The ordinary case: right peer, first attach, any incarnation accepted.
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED, (int)segment_header_check(&header, OWNER_IP, OWNER_PORT, OWNER_ID, 0));

    // Not ours, or a version this build cannot read. Checked before anything else is believed.
    header.magic = tt_SEGMENT_MAGIC + 1;
    EXPECT_EQ_INT((int)tt_SEGMENT_BAD_HEADER, (int)segment_header_check(&header, OWNER_IP, OWNER_PORT, OWNER_ID, 0));
    header = valid_header();
    header.version = tt_SEGMENT_VERSION + 1;
    EXPECT_EQ_INT((int)tt_SEGMENT_BAD_HEADER, (int)segment_header_check(&header, OWNER_IP, OWNER_PORT, OWNER_ID, 0));

    // A segment whose owner is not the peer the name was computed for - a collision. Each field of
    // the triple separately, because one of them agreeing is not the claim.
    header = valid_header();
    EXPECT_EQ_INT((int)tt_SEGMENT_WRONG_OWNER,
                  (int)segment_header_check(&header, OWNER_IP + 1, OWNER_PORT, OWNER_ID, 0));
    EXPECT_EQ_INT((int)tt_SEGMENT_WRONG_OWNER,
                  (int)segment_header_check(&header, OWNER_IP, OWNER_PORT + 1, OWNER_ID, 0));
    EXPECT_EQ_INT((int)tt_SEGMENT_WRONG_OWNER,
                  (int)segment_header_check(&header, OWNER_IP, OWNER_PORT, OWNER_ID + 1, 0));

    // The case that has no wire equivalent: the right owner triple, a different incarnation. A
    // context id is re-handed when its holder dies, so this segment legitimately carries the name a
    // new reader computes - and reading it would hand a dead peer's records to a live one. Only the
    // incarnation separates them, and it is why the header exists at all.
    EXPECT_EQ_INT((int)tt_SEGMENT_STALE,
                  (int)segment_header_check(&header, OWNER_IP, OWNER_PORT, OWNER_ID, OWNER_INCARNATION + 1));
    // Same incarnation as recorded: the peer we attached to is the peer still there.
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED,
                  (int)segment_header_check(&header, OWNER_IP, OWNER_PORT, OWNER_ID, OWNER_INCARNATION));
}

// A received datagram is counted once, on the transport it arrived over.
static void test_received_datagram_is_counted_as_udp(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = PEER_CONTEXT_ID;
    memcpy(node.rx_buffer, &header, sizeof(header));

    uint64_t before = node.rx_datagrams;
    (void)process_datagram_locked(&node, (int32_t)sizeof(header), PEER_IP, PEER_PORT);

    EXPECT_EQ_U32((uint32_t)(node.rx_datagrams - before), (uint32_t)node.rx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    EXPECT_EQ_U32(0, (uint32_t)node.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);
}

int main(void) {
    test_ordinary_publish_is_counted_as_udp();
    test_zerocopy_publish_is_counted_as_udp();
    test_batch_shape_is_counted_per_datagram();
    test_reset_zeroes_the_per_transport_counters();
    test_segment_name_separates_peers_that_differ();
    test_segment_header_catches_what_the_name_cannot();
    test_received_datagram_is_counted_as_udp();

    printf("test_transport_seam: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
