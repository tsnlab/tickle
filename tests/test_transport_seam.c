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

#include <pthread.h>
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

// One drain pass. drain_own_segment() reports whether the ring emptied, which is the poll loop's
// business and not these tests' - they drive the ring directly and assert on what came out.
static void drain_pass(struct tt_Context* node) {
    bool emptied = false;
    (void)drain_own_segment(node, &emptied);
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
    EXPECT_EQ_U32(0, (uint32_t)node.segment_oversized_to_udp);
    EXPECT_EQ_U32(0, (uint32_t)node.segment_unattached_to_udp);
    EXPECT_EQ_U32(0, (uint32_t)node.segment_full_dropped);
    EXPECT_EQ_U32(0, (uint32_t)node.tx_datagrams); // the scalar they must stay beside
    EXPECT_EQ_U32(0, (uint32_t)node.rx_datagrams);

#if tt_SEGMENT_ENABLED
    // And the segment's own state, which is the same trap with worse consequences. The counters
    // above only lied; segment_peers[].mapping holds POINTERS, and left at whatever was on the
    // caller's stack the teardown walked all 256 entries and called munmap() on 0x3, 0x40, 0x10 and
    // the rest, unmapping parts of the process at random. Every node in the integration suite
    // segfaulted at exit while this file stayed green - because every test here memsets its context
    // first, which is exactly what a real caller is not required to do. That is why this arm fills
    // the context with 0xAA and why it must keep doing so.
    EXPECT_TRUE(node.own_segment == NULL);
    for (int id = 0; id < tt_MAX_CONTEXT_IDS; id++) {
        EXPECT_TRUE(node.segment_peers[id].mapping == NULL);
        EXPECT_TRUE(!node.segment_peers[id].missing);
        EXPECT_EQ_U32(0, node.segment_peers[id].recheck_in);
        EXPECT_EQ_U32(0, (uint32_t)node.segment_peers[id].last_progress_ns);
    }
#endif
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
    // The geometry, without which this was never a valid header - only one nothing checked. A header
    // with slots == 0 is not merely incomplete: segment_slot() masks with slots - 1U, so zero becomes
    // 0xFFFFFFFF and every index is wild. create_own_segment() always writes both fields, so omitting
    // them here built something the product cannot produce, and the omission went unnoticed for as
    // long as nothing read them.
    header.slots = (uint32_t)tt_SEGMENT_SLOTS;
    header.slot_bytes = (uint32_t)tt_SEGMENT_SLOT_BYTES;
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

#define RING_SLOTS 4
#define RING_SLOT_BYTES 64

// A segment laid out in ordinary memory. The ring is the same code whether the pages came from
// shm_open or from the stack, and everything worth asserting about it is reachable here - which is
// why the ring lives in core and only the mapping is the HAL's.
static struct tt_SegmentHeader* make_ring(void* storage, uint32_t slots, uint32_t slot_bytes) {
    struct tt_SegmentHeader* header = storage;
    memset(storage, 0, segment_bytes(slots, slot_bytes));
    header->magic = tt_SEGMENT_MAGIC;
    header->version = tt_SEGMENT_VERSION;
    header->owner_ip = OWNER_IP;
    header->owner_port = OWNER_PORT;
    header->owner_context_id = OWNER_ID;
    header->incarnation = OWNER_INCARNATION;
    header->slots = slots;
    header->slot_bytes = slot_bytes;
    // As create_own_segment() does: each slot free for the writer of its own index, not zero.
    for (uint32_t index = 0; index < slots; index++) {
        ((struct tt_SegmentSlot*)segment_slot(header, index))->sequence = index;
    }
    return header;
}

static void test_ring_round_trips_a_datagram(void) {
    uint8_t storage[4096];
    struct tt_SegmentHeader* ring = make_ring(storage, RING_SLOTS, RING_SLOT_BYTES);

    uint8_t out[RING_SLOT_BYTES];
    uint32_t len = 0;
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    uint16_t from_span = 0;
    EXPECT_TRUE(!segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span)); // empty to begin with

    const char* payload = "a datagram";
    EXPECT_TRUE(segment_write(ring, payload, (uint32_t)strlen(payload) + 1, NULL, 0, OWNER_IP, OWNER_PORT, 1));
    EXPECT_TRUE(segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span));
    EXPECT_EQ_U32((uint32_t)strlen(payload) + 1, len);
    EXPECT_EQ_INT(0, strcmp(payload, (const char*)out));
    EXPECT_TRUE(!segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span)); // and empty again

    // More than a slot holds is refused rather than written short.
    uint8_t oversize[RING_SLOT_BYTES + 1];
    memset(oversize, 'x', sizeof(oversize));
    EXPECT_TRUE(!segment_write(ring, oversize, (uint32_t)sizeof(oversize), NULL, 0, OWNER_IP, OWNER_PORT, 1));
}

// The rule whose failure is silent corruption rather than an error: a full ring refuses, it does
// not overwrite. Asserted by reading the queue back afterwards, not by the return value alone -
// a writer that returned false AND overwrote would pass a check of the boolean.
static void test_full_ring_refuses_rather_than_overwriting(void) {
    uint8_t storage[4096];
    struct tt_SegmentHeader* ring = make_ring(storage, RING_SLOTS, RING_SLOT_BYTES);

    for (uint32_t i = 0; i < RING_SLOTS; i++) {
        char payload[RING_SLOT_BYTES];
        snprintf(payload, sizeof(payload), "record-%u", i);
        EXPECT_TRUE(segment_write(ring, payload, (uint32_t)strlen(payload) + 1, NULL, 0, OWNER_IP, OWNER_PORT, 1));
    }

    // Full. Several attempts, because a writer that overwrites once per call would still leave the
    // count right after one.
    for (int attempt = 0; attempt < 3; attempt++) {
        EXPECT_TRUE(!segment_write(ring, "intruder", 9, NULL, 0, OWNER_IP, OWNER_PORT, 1));
    }

    // The queued records are the ones written, in order, untouched by the refused writes.
    for (uint32_t i = 0; i < RING_SLOTS; i++) {
        char expected[RING_SLOT_BYTES];
        snprintf(expected, sizeof(expected), "record-%u", i);
        uint8_t out[RING_SLOT_BYTES];
        uint32_t len = 0;
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        uint16_t from_span = 0;
        EXPECT_TRUE(segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span));
        EXPECT_EQ_INT(0, strcmp(expected, (const char*)out));
    }

    // And a released slot is reusable, or the ring would wedge after one fill.
    EXPECT_TRUE(segment_write(ring, "after drain", 12, NULL, 0, OWNER_IP, OWNER_PORT, 1));
}

// Free-running indices, so the ring keeps working past the point where they wrap the slot count.
static void test_ring_survives_many_wraps(void) {
    uint8_t storage[4096];
    struct tt_SegmentHeader* ring = make_ring(storage, RING_SLOTS, RING_SLOT_BYTES);

    for (uint32_t i = 0; i < RING_SLOTS * 10U; i++) {
        char payload[RING_SLOT_BYTES];
        snprintf(payload, sizeof(payload), "wrap-%u", i);
        EXPECT_TRUE(segment_write(ring, payload, (uint32_t)strlen(payload) + 1, NULL, 0, OWNER_IP, OWNER_PORT, 1));

        uint8_t out[RING_SLOT_BYTES];
        uint32_t len = 0;
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        uint16_t from_span = 0;
        EXPECT_TRUE(segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span));
        EXPECT_EQ_INT(0, strcmp(payload, (const char*)out));
    }
}

// A length the slot could not hold means the segment is not what it claims. The slot is released so
// the ring cannot wedge, and nothing of the payload is handed up.
static void test_impossible_length_is_refused_and_does_not_wedge(void) {
    uint8_t storage[4096];
    struct tt_SegmentHeader* ring = make_ring(storage, RING_SLOTS, RING_SLOT_BYTES);

    EXPECT_TRUE(segment_write(ring, "good", 5, NULL, 0, OWNER_IP, OWNER_PORT, 1));
    EXPECT_TRUE(segment_write(ring, "also good", 10, NULL, 0, OWNER_IP, OWNER_PORT, 1));
    // Corrupt the first record's length, as a broken writer or a foreign mapping would.
    ((struct tt_SegmentSlot*)segment_slot(ring, 0))->length = RING_SLOT_BYTES + 1000U;

    uint8_t out[RING_SLOT_BYTES];
    uint32_t len = 0;
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    uint16_t from_span = 0;
    EXPECT_TRUE(!segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span)); // refused
    EXPECT_TRUE(segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span));  // and the ring moved on
    EXPECT_EQ_INT(0, strcmp("also good", (const char*)out));
}

// S2's assertion, pinned in-process: every UDP datagram has a named reason, so tx_udp equals the
// sum of the four by-reason counters. The module is built in here and no segment exists, so every
// datagram falls back - which is the case that would hide a miscount, because all four counters
// being zero except one is the easiest thing to get accidentally right.
//
// The invariant is structural (count_udp() is the only path that increments tx_udp, and it names a
// reason in the same call), so this test cannot fail while that holds. It is here because the
// structure is what a future edit would break, and because S2 asserts the same thing across a
// process boundary where no helper can reach - two checks of one property at two scopes.
static void test_every_udp_datagram_has_a_named_reason(void) {
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

    // A unicast shape and a broadcast shape, so more than one reason is in play - a test with one
    // reason would pass against a helper that always named that reason.
    for (uint32_t i = 0; i < SAMPLES; i++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&i));
    }
    uint8_t body[ZEROCOPY_BODY] = {0};
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish_zerocopy(&pub, body, (uint32_t)sizeof(body)));
    pub.peers[0].context_id = tt_CONTEXT_ID_INVALID; // now every destination is a broadcast
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish_zerocopy(&pub, body, (uint32_t)sizeof(body)));

    // Three reasons, not four: since 2026-09-29 a full ring drops the datagram rather than sending
    // it by UDP, so segment_full_dropped is not a fallback and adding it here would make the
    // invariant false. Asserted separately below that nothing was dropped in this arm.
    uint64_t named = node.segment_broadcast_to_udp + node.segment_oversized_to_udp + node.segment_unattached_to_udp;
    EXPECT_TRUE(node.tx_datagrams_by_transport[tt_TRANSPORT_UDP] > 0); // the arm sent something
    EXPECT_EQ_U32((uint32_t)node.tx_datagrams_by_transport[tt_TRANSPORT_UDP], (uint32_t)named);
    EXPECT_EQ_U32(0, (uint32_t)node.tx_datagrams_by_transport[tt_TRANSPORT_SHM]); // no segment exists

    // And more than one reason actually occurred, or the equality above is a weaker claim than it
    // looks: broadcast for the unaddressed shape, unattached for the peer with no segment.
    EXPECT_TRUE(node.segment_broadcast_to_udp > 0);
    EXPECT_TRUE(node.segment_unattached_to_udp > 0);
    EXPECT_EQ_U32(0, (uint32_t)node.segment_full_dropped); // no segment here, so nothing to fill
}

// The first datagram that actually goes over a segment, end to end in one process: one context
// creates its segment, another writes into it by name, and the first drains it into the same
// acceptance path a socket arrival takes.
//
// This is the arm that makes tx_shm and rx_shm mean anything. Everything before it could pass with
// the transport permanently inert, because a fallback to UDP is correct behaviour and the counters
// would still have summed - which is precisely the failure S2 exists to catch at process scope and
// this catches at function scope.
static void test_a_datagram_crosses_a_segment(void) {
    test_mock_reset();
    test_mock_segments_free();

    // The reader: a context with its own segment, which is what a peer attaches to.
    struct tt_Context reader;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&reader, &topic, &pub);
    reader.id = OWNER_ID;
    reader.entity_id_base = OWNER_INCARNATION;
    // Distinct, non-zero addresses for the two contexts. Without this every address assertion below
    // compares 0 with 0 and holds no matter what the code does - which is exactly how the first
    // version of this test passed against a mutant that dropped the sender address entirely.
    reader.hal.own_ip = OWNER_IP;
    reader.hal.own_port = OWNER_PORT;
    create_own_segment(&reader);
    EXPECT_TRUE(reader.own_segment != NULL);
    EXPECT_EQ_INT(1, test_mock_segment_creates);

    // The writer: a second context that reaches the first as a peer. tt_own_address() gives the
    // mock's own address for both, which is what makes them find each other here.
    struct tt_Context writer;
    struct tt_Topic writer_topic;
    struct tt_Publisher writer_pub;
    init_node_topic_pub(&writer, &writer_topic, &writer_pub);
    writer.hal.own_ip = PEER_IP; // different from the reader's, so "whose address is this" is a real question
    writer.hal.own_port = PEER_PORT;
    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&reader, &own_ip, &own_port);

    // A datagram the acceptance path will accept: a valid header from the writer.
    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = PEER_CONTEXT_ID;

    enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
    bool took = segment_deliver(&writer, OWNER_ID, own_ip, own_port, &header, sizeof(header), NULL, 0, &reason);

    EXPECT_TRUE(took); // it went over the segment, not to the socket
    EXPECT_EQ_U32(1, (uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U32(0, (uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    EXPECT_EQ_INT(2, test_mock_segment_attaches); // and it attached to find it: the header probe, then the region

    // The record carries the writer's own address, which is what a socket arrival would have. This
    // is asserted here because getting it wrong was invisible to every other check: the arrival was
    // correctly counted as shm, the totals agreed, and discovery silently recorded the peer at the
    // invented address 0.0.0.0:0 - after which nothing could ever address it again. The attribution
    // was right and a value inside it meant the wrong thing.
    // Inspected in place rather than read out. segment_read() releases the slot as well as moving
    // read_index - it has to, since a writer checks the slot's own sequence - so the older trick of
    // reading and restoring read_index no longer puts the record back, and the drain below would
    // find nothing. Whitebox test, whitebox look.
    struct tt_SegmentHeader* seg = reader.own_segment;
    const struct tt_SegmentSlot* first = (const struct tt_SegmentSlot*)segment_slot(seg, seg->read_index);
    uint32_t record_ip = first->sender_ip;
    uint16_t record_port = first->sender_port;
    EXPECT_EQ_U32(seg->read_index + 1U, first->sequence); // published, i.e. there is a record to look at
    // The WRITER's address, not the reader's - the first version of this compared against the
    // reader's, which was both the wrong subject and, with both of them zero, an assertion that
    // could not fail. Non-zero is asserted separately so that zeroing them again is caught even if
    // some future change makes the two contexts share an address.
    EXPECT_TRUE(record_ip != 0);
    EXPECT_EQ_U32(PEER_IP, record_ip);
    EXPECT_EQ_U32((uint32_t)PEER_PORT, (uint32_t)record_port);

    // The reader finds it on its own next pass - no notification, by design.
    uint64_t rx_before = reader.rx_datagrams_by_transport[tt_TRANSPORT_SHM];
    drain_pass(&reader);
    EXPECT_EQ_U32(1, (uint32_t)(reader.rx_datagrams_by_transport[tt_TRANSPORT_SHM] - rx_before));
    EXPECT_EQ_U32(0, (uint32_t)reader.rx_datagrams_by_transport[tt_TRANSPORT_UDP]);

    // Draining again finds nothing: the record was released, not re-read.
    drain_pass(&reader);
    EXPECT_EQ_U32(1, (uint32_t)(reader.rx_datagrams_by_transport[tt_TRANSPORT_SHM] - rx_before));

    test_mock_segments_free();
}

// The mock's test_mock_send_last_buf is deliberately NOT the wire bytes: a datagram sent in the
// single-submessage form is rewritten into the classic form, 4 bytes longer, so every decoder in
// the tests can read it. Byte identity has to compare what the HAL was actually handed, so it uses
// the raw hook instead - the first version of this test compared against the normalised copy and
// reported a 4-byte difference that was the mock's own rewrite, not the transport's.
static uint8_t raw_sent[tt_SEGMENT_SLOT_BYTES];
static size_t raw_sent_len = 0;

static void capture_raw_send(const void* buf, size_t len) {
    raw_sent_len = len < sizeof(raw_sent) ? len : sizeof(raw_sent);
    memcpy(raw_sent, buf, raw_sent_len);
}

// SHM_PLAN 6a item 2: byte identity. The datagram placed in the segment equals, byte for byte, the
// one the UDP path would have sent. That is section 1's constraint as a test rather than as an
// intention - "the same datagram through the same acceptance path" is what makes a same-host bypass
// impossible instead of merely discouraged, and a transport free to reshape its payload would be a
// second wire format nobody is testing.
//
// Both arms send the same publish through the same encoder; only the destination differs. The mock
// captures what the HAL was handed, which is the UDP arm's golden bytes.
static void test_segment_bytes_equal_what_udp_would_have_sent(void) {
    test_mock_reset();
    test_mock_segments_free();

    // Arm 1: no segment anywhere, so the datagram goes to the socket and the mock captures it.
    struct tt_Context udp_node;
    struct tt_Topic udp_topic;
    struct tt_Publisher udp_pub;
    init_node_topic_pub(&udp_node, &udp_topic, &udp_pub);
    udp_node.hal.own_ip = PEER_IP;
    udp_node.hal.own_port = PEER_PORT;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        udp_pub.peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    udp_pub.peers[0].context_id = OWNER_ID;
    udp_pub.peers[0].ip = OWNER_IP;
    udp_pub.peers[0].port = OWNER_PORT;

    uint32_t value = 0x5a5a5a5a;
    raw_sent_len = 0;
    test_mock_send_hook = capture_raw_send;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&udp_pub, (struct tt_Data*)&value));
    test_mock_send_hook = NULL;
    EXPECT_TRUE(raw_sent_len > 0); // the arm captured something to compare against

    uint8_t golden[tt_SEGMENT_SLOT_BYTES];
    uint32_t golden_len = (uint32_t)raw_sent_len;
    EXPECT_TRUE(golden_len <= sizeof(golden));
    memcpy(golden, raw_sent, golden_len);

    // Arm 2: the same publish, with the peer's segment present so it takes the segment instead.
    struct tt_Context reader;
    struct tt_Topic reader_topic;
    struct tt_Publisher reader_pub;
    init_node_topic_pub(&reader, &reader_topic, &reader_pub);
    reader.id = OWNER_ID;
    reader.hal.own_ip = OWNER_IP;
    reader.hal.own_port = OWNER_PORT;
    reader.entity_id_base = OWNER_INCARNATION;
    create_own_segment(&reader);
    EXPECT_TRUE(reader.own_segment != NULL);

    struct tt_Context shm_node;
    struct tt_Topic shm_topic;
    struct tt_Publisher shm_pub;
    init_node_topic_pub(&shm_node, &shm_topic, &shm_pub);
    shm_node.hal.own_ip = PEER_IP;
    shm_node.hal.own_port = PEER_PORT;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        shm_pub.peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    shm_pub.peers[0].context_id = OWNER_ID;
    shm_pub.peers[0].ip = OWNER_IP;
    shm_pub.peers[0].port = OWNER_PORT;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&shm_pub, (struct tt_Data*)&value));
    EXPECT_TRUE(shm_node.tx_datagrams_by_transport[tt_TRANSPORT_SHM] > 0); // it really took the segment

    uint8_t carried[tt_SEGMENT_SLOT_BYTES];
    uint32_t carried_len = 0;
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    uint16_t from_span = 0;
    EXPECT_TRUE(segment_read(reader.own_segment, carried, (uint32_t)sizeof(carried), &carried_len, &from_ip, &from_port,
                             &from_span));

    EXPECT_EQ_U32(golden_len, carried_len);
    EXPECT_EQ_INT(0, memcmp(golden, carried, carried_len < golden_len ? carried_len : golden_len));

    test_mock_segments_free();
}

// SHM_PLAN 6a item 3, the anti-bypass test: a datagram that must be refused is refused identically
// over the segment. The point is not that the segment validates anything itself - it is that a
// segment arrival goes through the SAME acceptance path, so a datagram cannot reach an application
// by choosing a transport. A seam that handed records straight to delivery would pass every
// functional test and be a hole.
static void test_a_refused_datagram_is_refused_over_the_segment_too(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context reader;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&reader, &topic, &pub);
    reader.id = OWNER_ID;
    reader.hal.own_ip = OWNER_IP;
    reader.hal.own_port = OWNER_PORT;
    reader.entity_id_base = OWNER_INCARNATION;
    create_own_segment(&reader);
    EXPECT_TRUE(reader.own_segment != NULL);

    // A datagram from the future: a version this build does not speak. Over UDP this is counted and
    // dropped (g11 - it used to end the poll loop outright).
    struct tt_Header bad;
    memset(&bad, 0, sizeof(bad));
    bad.magic_value = NATIVE_MAGIC_VALUE;
    bad.version = tt_VERSION + 1;
    bad.source = PEER_CONTEXT_ID;

    uint64_t drops_before = reader.version_mismatch_drops;
    EXPECT_TRUE(segment_write(reader.own_segment, &bad, (uint32_t)sizeof(bad), NULL, 0, PEER_IP, PEER_PORT, 1));
    drain_pass(&reader);

    EXPECT_EQ_U32(1, (uint32_t)(reader.version_mismatch_drops - drops_before));     // refused, and counted
    EXPECT_EQ_U32(1, (uint32_t)reader.rx_datagrams_by_transport[tt_TRANSPORT_SHM]); // it did arrive
    EXPECT_EQ_U32(0, (uint32_t)reader.rx_datagrams_by_transport[tt_TRANSPORT_UDP]);

    test_mock_segments_free();
}

// SHM_PLAN 6a item 4: many writers into one segment, which is this topology's inherent shape - a
// context's segment is written by every peer that wants to reach it and read by one.
//
// Two real threads, because the defect this exists for cannot be staged from one. segment_write()
// used to load write_index and store it back with no atomicity, so two writers that both read the
// same value took the same slot: one record lost, one written twice, nothing reported.
//
// The first version of this test tried to stage that by hand - one writer claiming an index, the
// other running in the window - and it PASSED against the pre-fix writer, because the staged claim
// had already advanced write_index so the second writer never collided. It tested the slot's
// readiness rule and called itself a contention test. Two threads hammering the same ring do
// reproduce it: the claim is lost on the first collision, and with this many writes a collision is
// not in doubt.
#define CONTEND_WRITES 20000
#define CONTEND_SLOTS 64
#define CONTEND_BYTES 32

struct contend_arg {
    struct tt_SegmentHeader* ring;
    uint32_t tag;      // which writer: goes in the payload so a lost or duplicated record is visible
    uint32_t accepted; // writes the ring took
    uint32_t finished; // set last, so the reader knows when nothing more is coming
};

// Retries a refused write rather than counting it as done: the ring is far smaller than the run,
// so a writer that gave up on "full" would finish early and the two would barely overlap - the test
// would stop contending, which is the one thing it is for.
static void* contend_writer(void* raw) {
    struct contend_arg* arg = raw;
    for (uint32_t i = 0; i < CONTEND_WRITES; i++) {
        uint32_t payload[2] = {arg->tag, i};
        while (!segment_write(arg->ring, payload, (uint32_t)sizeof(payload), NULL, 0, OWNER_IP + arg->tag, OWNER_PORT,
                              1)) {
            // Full: the reader is behind. Spin - it is a live thread and the wait is microseconds.
        }
        arg->accepted++;
    }
    __atomic_store_n(&arg->finished, 1U, __ATOMIC_RELEASE);
    return NULL;
}

static void test_two_writers_contend_for_one_segment(void) {
    static uint8_t storage[sizeof(struct tt_SegmentHeader) +
                           ((size_t)CONTEND_SLOTS * (sizeof(struct tt_SegmentSlot) + CONTEND_BYTES))];
    struct tt_SegmentHeader* ring = make_ring(storage, CONTEND_SLOTS, CONTEND_BYTES);

    struct contend_arg a = {ring, 0, 0, 0};
    struct contend_arg b = {ring, 1, 0, 0};
    pthread_t ta;
    pthread_t tb;
    EXPECT_EQ_INT(0, pthread_create(&ta, NULL, contend_writer, &a));
    EXPECT_EQ_INT(0, pthread_create(&tb, NULL, contend_writer, &b));

    // The reader runs here while both write, so the ring keeps draining and they keep contending.
    uint32_t drained[2] = {0, 0};
    uint32_t seen = 0;
    for (;;) {
        uint8_t out[CONTEND_BYTES];
        uint32_t len = 0;
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        uint16_t from_span = 0;
        if (segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span)) {
            uint32_t payload[2];
            memcpy(payload, out, sizeof(payload));
            EXPECT_TRUE(payload[0] < 2);
            if (payload[0] < 2) {
                EXPECT_EQ_U32(OWNER_IP + payload[0], from_ip); // its own writer's address, not the other's
                drained[payload[0]]++;
            }
            seen++;
            continue;
        }
        // Empty. Stop only once both writers have finished - and look once more after that, since
        // one can publish between the two checks.
        if (__atomic_load_n(&a.finished, __ATOMIC_ACQUIRE) != 0 &&
            __atomic_load_n(&b.finished, __ATOMIC_ACQUIRE) != 0) {
            if (!segment_read(ring, out, sizeof(out), &len, &from_ip, &from_port, &from_span)) {
                break;
            }
            uint32_t payload[2];
            memcpy(payload, out, sizeof(payload));
            if (payload[0] < 2) {
                drained[payload[0]]++;
            }
            seen++;
        }
    }
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    // Every record the ring accepted is one the reader saw: none lost to a stolen slot, none
    // duplicated by two writers filling one. This is what the pre-fix writer fails.
    EXPECT_EQ_U32(CONTEND_WRITES, a.accepted);
    EXPECT_EQ_U32(CONTEND_WRITES, b.accepted);
    EXPECT_EQ_U32(a.accepted, drained[0]);
    EXPECT_EQ_U32(b.accepted, drained[1]);
    EXPECT_EQ_U32(a.accepted + b.accepted, seen);
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
    (void)process_datagram_locked(&node, (int32_t)sizeof(header), PEER_IP, PEER_PORT, tt_TRANSPORT_UDP, 1);

    EXPECT_EQ_U32((uint32_t)(node.rx_datagrams - before), (uint32_t)node.rx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    EXPECT_EQ_U32(0, (uint32_t)node.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);
}

// The regression test for the defect Plan found on the rig on 2026-09-29: cross-host throughput
// halved and CPU per sample doubled between 9dbffd40 and d4413383, with CycloneDDS flat across the
// same runs. Every datagram to a peer on another host re-asked /dev/shm whether that peer had a
// segment, because peer_segment()'s failure path wrote nothing to the cache entry. The cost is a
// failed open() per datagram - about 87,000 a second per sender at the rates cell 1 runs at.
//
// What is asserted is the number of *calls*, including the failing ones, which is why the mock
// counts those separately: a counter that only rose on success could not see this defect at all,
// and every functional test passed while it was there. The control arm is the same peer once a
// segment exists for it, where one attach must serve every later datagram - so the test
// distinguishes "asks once" from "never asks".
static void test_a_peer_with_no_segment_is_asked_once_not_per_datagram(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.hal.own_ip = PEER_IP;
    node.hal.own_port = PEER_PORT;

    // No segment exists at the owner's address, which is every peer on another host.
    const int sends = 64; // well inside tt_SEGMENT_ATTACH_RETRY_SENDS, so one ask must cover them all
    for (int i = 0; i < sends; i++) {
        EXPECT_TRUE(peer_segment(&node, OWNER_ID, OWNER_IP, OWNER_PORT) == NULL);
    }
    EXPECT_EQ_INT(1, test_mock_segment_attach_calls);
    // The miss is counted once too, not once per datagram: segment_attach[] is a count of attempts,
    // and the per-datagram count is segment_unattached_to_udp, which the caller keeps.
    EXPECT_EQ_U32(1, node.segment_attach[tt_SEGMENT_ABSENT]);

    // And it is not permanent. Past the countdown the question is asked again, which is what makes
    // a peer that binds later reachable rather than written off.
    for (uint32_t i = 0; i < tt_SEGMENT_ATTACH_RETRY_SENDS; i++) {
        (void)peer_segment(&node, OWNER_ID, OWNER_IP, OWNER_PORT);
    }
    EXPECT_EQ_INT(2, test_mock_segment_attach_calls);

    // A different address behind the same context id is asked about at once - the cached "no" was
    // about a peer that is not this one.
    int before = test_mock_segment_attach_calls;
    EXPECT_TRUE(peer_segment(&node, OWNER_ID, OWNER_IP + 1, OWNER_PORT) == NULL);
    EXPECT_EQ_INT(before + 1, test_mock_segment_attach_calls);

    // Control: with a segment there, one attach serves every subsequent datagram. Without this arm
    // the assertions above would also pass against a peer_segment() that never attached at all.
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context owner;
    struct tt_Topic owner_topic;
    struct tt_Publisher owner_pub;
    init_node_topic_pub(&owner, &owner_topic, &owner_pub);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    memset(node.segment_peers, 0, sizeof(node.segment_peers));
    for (int i = 0; i < sends; i++) {
        EXPECT_TRUE(peer_segment(&node, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    }
    // Two, not one, since the attach became two-step: a probe that reads the owner's geometry, then the
    // region at the owner's size. What this assertion is for is unchanged - the count does not grow with
    // `sends`, so the attach is still cached rather than repeated per datagram. The absent case above
    // stays at one, because a probe that finds nothing returns before the second map.
    EXPECT_EQ_INT(2, test_mock_segment_attach_calls);

    test_mock_segments_free();
}

// Item 6 of SHM_PLAN 6a: capacity exhaustion counted and warned about once, never silent - and the
// warning has to say enough to tell the two causes apart. A ring that is too small for the offered
// load fills after roughly as many records as it has slots; a ring whose slot sequences were never
// seeded is a ring of ONE and fills after the first record. Both move segment_full_dropped, and
// reading the second as the first sends you to tt_SEGMENT_BYTES for a bug that is in
// create_own_segment(). Plan asked for this case specifically, and it is not hypothetical: the
// seeding loop exists because zeroed memory leaves slot 0 claimable and every other slot not.
static void test_capacity_exhaustion_is_counted_and_warned_once(void) {
    uint8_t storage[4096];

    // Arm A, seeded as create_own_segment() seeds it: the ring holds as many records as it has
    // slots before it refuses.
    struct tt_SegmentHeader* seeded = make_ring(storage, RING_SLOTS, RING_SLOT_BYTES);
    uint32_t accepted = 0;
    while (segment_write(seeded, "x", 2, NULL, 0, PEER_IP, PEER_PORT, 1)) {
        accepted++;
        if (accepted > RING_SLOTS * 4) {
            break; // a ring that never fills is its own failure, caught by the assertion below
        }
    }
    EXPECT_EQ_U32(RING_SLOTS, accepted);

    // Arm B, the ring of one: the same ring with the seeding left out, which is what zeroed shared
    // memory gives. It takes exactly one record and is full for ever after.
    uint8_t unseeded_storage[4096];
    struct tt_SegmentHeader* unseeded = make_ring(unseeded_storage, RING_SLOTS, RING_SLOT_BYTES);
    for (uint32_t index = 0; index < RING_SLOTS; index++) {
        ((struct tt_SegmentSlot*)segment_slot(unseeded, index))->sequence = 0; // as calloc left it
    }
    uint32_t accepted_unseeded = 0;
    while (segment_write(unseeded, "x", 2, NULL, 0, PEER_IP, PEER_PORT, 1)) {
        accepted_unseeded++;
        if (accepted_unseeded > RING_SLOTS * 4) {
            break;
        }
    }
    EXPECT_EQ_U32(1, accepted_unseeded);
    // The two arms differ, which is the whole point: the count of records a ring takes before its
    // first refusal is what separates a sizing problem from a seeding one, and it is that number
    // the warning carries.
    EXPECT_TRUE(accepted != accepted_unseeded);

    // Warned once, however many datagrams meet the full ring. Driven through segment_deliver(),
    // because that is where the warning lives and where a repeat would be emitted.
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context owner;
    struct tt_Topic owner_topic;
    struct tt_Publisher owner_pub;
    init_node_topic_pub(&owner, &owner_topic, &owner_pub);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    struct tt_Context writer;
    struct tt_Topic writer_topic;
    struct tt_Publisher writer_pub;
    init_node_topic_pub(&writer, &writer_topic, &writer_pub);
    writer.hal.own_ip = PEER_IP;
    writer.hal.own_port = PEER_PORT;

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = PEER_CONTEXT_ID;

    // Fill the owner's ring, then keep going. Nothing drains it, so every send past the fill meets
    // a full ring.
    uint32_t sends = tt_SEGMENT_SLOTS + 16U;
    uint32_t not_handled = 0;
    for (uint32_t i = 0; i < sends; i++) {
        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
        if (!segment_deliver(&writer, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason)) {
            not_handled++;
        }
    }
    // Every send was handled by the seam: the ones that fitted went into the ring and the rest were
    // dropped. None came back for UDP, which is the point - a datagram rerouted past a full ring
    // overtakes the records still in it and makes the reader discard them all.
    EXPECT_EQ_U32(0, not_handled);
    EXPECT_EQ_U32(tt_SEGMENT_SLOTS, (uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U32(0, (uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    EXPECT_EQ_U32(16, (uint32_t)writer.segment_full_dropped); // the ring took its slots and dropped the rest
    EXPECT_EQ_U32(1, writer.segment_full_warnings);           // said once
    // And not once per dropped datagram, which is the failure this latch exists for.
    EXPECT_TRUE(writer.segment_full_warnings != (uint32_t)writer.segment_full_dropped);

    test_mock_segments_free();
}

// A writer that claims a slot and dies before publishing it wedges the reader at that index for
// good: the slot keeps the sequence it had while free, so "empty" and "claimed and abandoned" look
// identical from the slot alone. The ring then fills, every datagram to that peer is dropped, and
// the only evidence is segment_full_dropped - a sizing signal - pointing at the wrong cause. This is
// the same confusion item 6 guards against from the other side, which is why both are here.
//
// The control arm is a published record: a head that can be read must not be reported as stalled,
// or the warning fires on every busy segment and means nothing.
static void test_a_stalled_head_is_noticed_rather_than_read_as_a_sizing_problem(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context owner;
    struct tt_Topic owner_topic;
    struct tt_Publisher owner_pub;
    init_node_topic_pub(&owner, &owner_topic, &owner_pub);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    // Control 1: an empty ring is not a stalled one. Nothing has been claimed, so there is nothing
    // the reader is waiting for.
    for (uint32_t i = 0; i < tt_SEGMENT_STALL_PASSES + 8U; i++) {
        drain_pass(&owner);
    }
    EXPECT_EQ_U64(0, owner.segment_head_stalls);
    EXPECT_EQ_U32(0, owner.segment_stall_warnings);

    // Control 2: a published record is read, and reading it clears the count rather than leaving it
    // to accumulate across the life of the segment.
    EXPECT_TRUE(segment_write(owner.own_segment, "hello", 6, NULL, 0, PEER_IP, PEER_PORT, 1));
    drain_pass(&owner);
    EXPECT_EQ_U64(0, owner.segment_head_stalls);

    // The wedge: a writer claims the head and never publishes it. Staged by moving write_index the
    // way segment_write()'s compare-and-exchange does, and then stopping - which is exactly what a
    // writer killed between its claim and its memcpy leaves behind.
    struct tt_SegmentHeader* seg = owner.own_segment;
    uint32_t claimed = __atomic_load_n(&seg->write_index, __ATOMIC_RELAXED);
    __atomic_store_n(&seg->write_index, claimed + 1U, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i < tt_SEGMENT_STALL_PASSES - 1U; i++) {
        drain_pass(&owner);
    }
    // Not yet: a writer in the middle of a memcpy looks the same, and warning on that would fire on
    // every busy segment. The count moves, the warning waits.
    EXPECT_EQ_U64(tt_SEGMENT_STALL_PASSES - 1U, owner.segment_head_stalls);
    EXPECT_EQ_U32(0, owner.segment_stall_warnings);

    drain_pass(&owner);
    EXPECT_EQ_U32(1, owner.segment_stall_warnings);

    // And once, however long it stays wedged.
    for (uint32_t i = 0; i < tt_SEGMENT_STALL_PASSES; i++) {
        drain_pass(&owner);
    }
    EXPECT_EQ_U32(1, owner.segment_stall_warnings);

    test_mock_segments_free();
}

// Item 7: a context that goes away does not leave its records, or its segment, behind. In this
// topology the reader is the owner, so "a reader that exits" is a context being destroyed - and
// until 2026-09-29 tt_Context_destroy() unlinked nothing, so every context that ever ran left a
// file in /dev/shm. The leak is invisible in a test unless it is looked for, because
// tt_segment_create() unlinks before creating and the next context at that name replaces it.
//
// The control is the assertion before the destroy: without it, a test that computed the wrong name
// would find "no segment there" both times and pass while unlinking nothing.
static void test_destroy_takes_the_segment_with_it(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context owner;
    struct tt_Topic owner_topic;
    struct tt_Publisher owner_pub;
    init_node_topic_pub(&owner, &owner_topic, &owner_pub);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    // The owner has a peer mapped as well, so the teardown is asked about both kinds of mapping.
    struct tt_Context other;
    struct tt_Topic other_topic;
    struct tt_Publisher other_pub;
    init_node_topic_pub(&other, &other_topic, &other_pub);
    other.id = PEER_CONTEXT_ID;
    other.entity_id_base = OWNER_INCARNATION + 1U;
    other.hal.own_ip = PEER_IP;
    other.hal.own_port = PEER_PORT;
    create_own_segment(&other);
    EXPECT_TRUE(peer_segment(&owner, PEER_CONTEXT_ID, PEER_IP, PEER_PORT) != NULL);

    char path[tt_SEGMENT_PATH_LENGTH];
    EXPECT_TRUE(segment_name(path, sizeof(path), OWNER_IP, OWNER_PORT, OWNER_ID) > 0);
    char peer_path[tt_SEGMENT_PATH_LENGTH];
    EXPECT_TRUE(segment_name(peer_path, sizeof(peer_path), PEER_IP, PEER_PORT, PEER_CONTEXT_ID) > 0);

    // Control: the segment is there under exactly this name before the destroy. An attach is how
    // the question is asked, because that is how a peer asks it.
    uint8_t why = (uint8_t)tt_SEGMENT_ABSENT;
    void* found = tt_segment_attach(path, segment_bytes(tt_SEGMENT_SLOTS, tt_SEGMENT_SLOT_BYTES), &why);
    EXPECT_TRUE(found != NULL);

    // A context is attached to its OWN segment as a peer too - it unicasts to itself - so the
    // teardown has the same region in segment_peers[] and in own_segment. Staged explicitly,
    // because it is what the real suite does on every node and what this test missed the first
    // time: the teardown unmapped it as a peer and then read the owner's name out of it, which is a
    // use-after-munmap. It segfaulted every node in the integration suite while this file stayed
    // green, because the mock's detach cannot make a page inaccessible.
    EXPECT_TRUE(peer_segment(&owner, OWNER_ID, OWNER_IP, OWNER_PORT) == owner.own_segment);

    EXPECT_EQ_INT(tt_RET_OK, (int)tt_Context_destroy(&owner));
    // Unmapped exactly once. The mock cannot fault on a stale pointer, so it counts instead - which
    // is the nearest a no-op detach can get to reproducing the crash.
    EXPECT_EQ_INT(0, test_mock_segment_double_detaches);

    // Gone: unlinked, so no peer can attach to a segment nobody is draining.
    why = (uint8_t)tt_SEGMENT_ATTACHED;
    found = tt_segment_attach(path, segment_bytes(tt_SEGMENT_SLOTS, tt_SEGMENT_SLOT_BYTES), &why);
    EXPECT_TRUE(found == NULL);
    EXPECT_EQ_INT((int)tt_SEGMENT_ABSENT, (int)why);
    EXPECT_TRUE(owner.own_segment == NULL);
    EXPECT_TRUE(owner.segment_peers[PEER_CONTEXT_ID].mapping == NULL);

    // The peer's own segment is NOT unlinked by the owner's teardown: it belongs to that peer, which
    // is still running and still reading it. Unmapping it is this context's business; removing it
    // is not.
    why = (uint8_t)tt_SEGMENT_ABSENT;
    found = tt_segment_attach(peer_path, segment_bytes(tt_SEGMENT_SLOTS, tt_SEGMENT_SLOT_BYTES), &why);
    EXPECT_TRUE(found != NULL);

    EXPECT_EQ_INT(tt_RET_OK, (int)tt_Context_destroy(&other));
    test_mock_segments_free();
}

// Item 8: a context that died without unlinking leaves a segment behind, and the next context at
// that name takes it over rather than inheriting it. tt_segment_create() unlinks first, so the new
// owner gets a new region - and the peer still holding the old mapping finds out through the
// incarnation, which is the field's whole purpose.
//
// "Died without unlinking" is staged by simply not tearing the first context down, which is what a
// SIGKILL leaves: the mapping goes with the process and the file stays.
static void test_a_segment_left_by_a_dead_owner_is_reclaimed(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context dead;
    struct tt_Topic dead_topic;
    struct tt_Publisher dead_pub;
    init_node_topic_pub(&dead, &dead_topic, &dead_pub);
    dead.id = OWNER_ID;
    dead.entity_id_base = OWNER_INCARNATION;
    dead.hal.own_ip = OWNER_IP;
    dead.hal.own_port = OWNER_PORT;
    create_own_segment(&dead);
    EXPECT_TRUE(dead.own_segment != NULL);

    // A peer attaches while it is alive, and puts a record in it.
    struct tt_Context writer;
    struct tt_Topic writer_topic;
    struct tt_Publisher writer_pub;
    init_node_topic_pub(&writer, &writer_topic, &writer_pub);
    writer.hal.own_ip = PEER_IP;
    writer.hal.own_port = PEER_PORT;
    struct tt_SegmentHeader* old_mapping = peer_segment(&writer, OWNER_ID, OWNER_IP, OWNER_PORT);
    EXPECT_TRUE(old_mapping != NULL);
    EXPECT_TRUE(segment_write(old_mapping, "orphan", 7, NULL, 0, PEER_IP, PEER_PORT, 1));

    // The owner dies. Nothing is unlinked and nothing is drained: the record above is now parked in
    // a segment no one reads.

    // A new context takes the same address and context id - the case the name cannot distinguish,
    // which is why the header carries an incarnation.
    struct tt_Context reborn;
    struct tt_Topic reborn_topic;
    struct tt_Publisher reborn_pub;
    init_node_topic_pub(&reborn, &reborn_topic, &reborn_pub);
    reborn.id = OWNER_ID;
    reborn.entity_id_base = OWNER_INCARNATION + 99U;
    reborn.hal.own_ip = OWNER_IP;
    reborn.hal.own_port = OWNER_PORT;
    create_own_segment(&reborn);
    EXPECT_TRUE(reborn.own_segment != NULL);

    // A different region, so the orphaned record did not come with it. Asserted by the ring's state
    // rather than by the pointer, because two allocations can land at one address once the first is
    // freed - and here the old one is deliberately still alive.
    EXPECT_TRUE(reborn.own_segment != old_mapping);
    EXPECT_EQ_U32(0, reborn.own_segment->write_index);
    EXPECT_EQ_U32(0, reborn.own_segment->read_index);
    EXPECT_EQ_U32(OWNER_INCARNATION + 99U, reborn.own_segment->incarnation);

    // The writer's cached mapping is still the dead one, and this is the assertion the first version
    // of this test got wrong. It expected the incarnation check to catch it - but that check reads
    // the header *through the mapping we already hold*, and that region is the old file: unlinked,
    // still mapped, still carrying the incarnation we recorded. Nothing inside a mapping can ever
    // say its owner has gone. So this is the real window, and it is asserted rather than wished
    // away: until the recheck falls due, the writer is putting datagrams into an orphan.
    EXPECT_TRUE(peer_segment(&writer, OWNER_ID, OWNER_IP, OWNER_PORT) == old_mapping);

    // What closes the window is asking the name again, which is the only question that has a
    // different answer now. Past tt_SEGMENT_REVALIDATE_SENDS the entry is dropped and re-attached,
    // and the file at that name is the successor's.
    struct tt_SegmentHeader* new_mapping = old_mapping;
    for (uint32_t i = 0; i < tt_SEGMENT_REVALIDATE_SENDS; i++) {
        new_mapping = peer_segment(&writer, OWNER_ID, OWNER_IP, OWNER_PORT);
    }
    EXPECT_TRUE(new_mapping != NULL);
    EXPECT_TRUE(new_mapping != old_mapping);
    EXPECT_TRUE(new_mapping == reborn.own_segment);
    EXPECT_EQ_U32(OWNER_INCARNATION + 99U, new_mapping->incarnation);

    // The new owner drains its own segment and finds nothing: the orphan was not inherited.
    uint64_t rx_before = reborn.rx_datagrams_by_transport[tt_TRANSPORT_SHM];
    drain_pass(&reborn);
    EXPECT_EQ_U64(rx_before, reborn.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);

    test_mock_segments_free();
}

// The other half of a dead owner, and the one item 11 needs: an owner that is killed with nobody
// taking its name. The file stays, the mapping stays valid, the incarnation never changes, and
// re-attaching by name finds the very same region - so every test the peer can apply to the segment
// says it is healthy. The only thing that changes is that nothing drains it, and the writer sees
// that as a ring that will not take anything, ever.
//
// What must happen is that the writer keeps making progress. It gives the segment up after a streak
// of refusals and goes back to UDP, where the peer is still reachable - it is the reader that died,
// not the link. The control is a ring that is full because the reader is merely behind: one drained
// slot must be enough to keep the segment.
static void test_a_writer_gives_up_on_a_ring_nobody_drains(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context dead;
    struct tt_Topic dead_topic;
    struct tt_Publisher dead_pub;
    init_node_topic_pub(&dead, &dead_topic, &dead_pub);
    dead.id = OWNER_ID;
    dead.entity_id_base = OWNER_INCARNATION;
    dead.hal.own_ip = OWNER_IP;
    dead.hal.own_port = OWNER_PORT;
    create_own_segment(&dead);
    EXPECT_TRUE(dead.own_segment != NULL);

    struct tt_Context writer;
    struct tt_Topic writer_topic;
    struct tt_Publisher writer_pub;
    init_node_topic_pub(&writer, &writer_topic, &writer_pub);
    writer.hal.own_ip = PEER_IP;
    writer.hal.own_port = PEER_PORT;

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = PEER_CONTEXT_ID;

    // Nothing ever drains the ring - the owner is dead. Send until the writer gives up, plus a
    // margin, and watch where the datagrams go.
    // The clock is what decides now, so the clock is what this arm drives. Each send costs a
    // millisecond of mock time; the reader never takes anything, so its silence crosses
    // tt_SEGMENT_DEAD_READER_NS and the writer gives up.
    uint32_t sends = tt_SEGMENT_SLOTS + 2048U;
    uint32_t to_udp = 0;
    for (uint32_t i = 0; i < sends; i++) {
        test_mock_now += 1000000ULL; // 1 ms per send
        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
        if (!segment_deliver(&writer, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason)) {
            EXPECT_EQ_INT((int)UDP_BECAUSE_UNATTACHED, (int)reason);
            to_udp++;
        }
    }
    // Progress resumes. While the mapping is held a full ring drops, because rerouting past it would
    // reorder the stream - but once the writer gives the mapping up there is no reader left to
    // reorder anything for, so the peer becomes UNATTACHED and its datagrams go by UDP again. That
    // is the difference between a reader that is behind and a reader that is gone.
    EXPECT_TRUE(to_udp > 0);
    EXPECT_EQ_U32(tt_SEGMENT_SLOTS, (uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U32(sends - tt_SEGMENT_SLOTS - (uint32_t)writer.segment_full_dropped, to_udp);
    // Given up rather than retried for ever. Asserted as an EVENT that happened, not as the state at
    // the end of the loop: the give-up is followed by a recheck that re-attaches, so whether the
    // mapping is held when the loop happens to stop is a fact about the loop's length and not about
    // the rule. The first version asserted the end state and failed for that reason alone.
    EXPECT_TRUE(writer.segment_attach[tt_SEGMENT_REFUSED] >= 1);

    // Control: a reader that is behind, not dead. The ring fills, but a single drained slot resets
    // the streak, so the segment is kept - otherwise the first busy moment would push every peer
    // onto UDP and the module would quietly stop being used.
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context slow;
    struct tt_Topic slow_topic;
    struct tt_Publisher slow_pub;
    init_node_topic_pub(&slow, &slow_topic, &slow_pub);
    slow.id = OWNER_ID;
    slow.entity_id_base = OWNER_INCARNATION;
    slow.hal.own_ip = OWNER_IP;
    slow.hal.own_port = OWNER_PORT;
    create_own_segment(&slow);

    struct tt_Context patient;
    struct tt_Topic patient_topic;
    struct tt_Publisher patient_pub;
    init_node_topic_pub(&patient, &patient_topic, &patient_pub);
    patient.hal.own_ip = PEER_IP;
    patient.hal.own_port = PEER_PORT;

    for (uint32_t i = 0; i < sends; i++) {
        test_mock_now += 1000000ULL; // the same millisecond a send, so the two arms differ only in the reader
        uint64_t dropped_before = patient.segment_full_dropped;
        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
        (void)segment_deliver(&patient, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason);
        if (patient.segment_full_dropped != dropped_before) {
            // The reader catches up by one pass, which is all it takes. Watched through the drop
            // counter rather than through the return value, because the seam now reports a dropped
            // datagram as handled - the first version of this arm watched the return value, never
            // drained at all, and so tested a dead reader twice instead of a slow one once.
            drain_pass(&slow);
        }
    }
    EXPECT_TRUE(patient.segment_peers[OWNER_ID].mapping != NULL);
    EXPECT_TRUE(!patient.segment_peers[OWNER_ID].missing);
    EXPECT_EQ_U32(0, patient.segment_attach[tt_SEGMENT_REFUSED]);

    // Third arm, and the one the first two between them missed: a reader that never catches up. It
    // drains one record for every refusal, so the ring is permanently full and yet the reader is
    // plainly alive. The control above emptied the ring on every pass, which is not what a reader
    // under load does - and against the first version of this rule, which counted refusals alone,
    // this arm gives the segment up on a healthy peer, sends by UDP until the recheck, re-attaches
    // and repeats. That flapping is one logical stream on two paths, which is exactly what
    // drop-on-full exists to prevent. On the rig it cost 2,446,848 "unattached" datagrams in a run
    // whose peer was alive from start to finish.
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context behind;
    struct tt_Topic behind_topic;
    struct tt_Publisher behind_pub;
    init_node_topic_pub(&behind, &behind_topic, &behind_pub);
    behind.id = OWNER_ID;
    behind.entity_id_base = OWNER_INCARNATION;
    behind.hal.own_ip = OWNER_IP;
    behind.hal.own_port = OWNER_PORT;
    create_own_segment(&behind);

    struct tt_Context ahead;
    struct tt_Topic ahead_topic;
    struct tt_Publisher ahead_pub;
    init_node_topic_pub(&ahead, &ahead_topic, &ahead_pub);
    ahead.hal.own_ip = PEER_IP;
    ahead.hal.own_port = PEER_PORT;

    // Long enough in mock time to cross the dead-reader threshold many times over. The reader here
    // is permanently behind but never stops, so it must never be given up on however long the run.
    uint32_t behind_sends = tt_SEGMENT_SLOTS + 8192U;
    for (uint32_t i = 0; i < behind_sends; i++) {
        test_mock_now += 1000000ULL; // 8 seconds of mock time, eight times the dead-reader threshold
        uint64_t dropped_before = ahead.segment_full_dropped;
        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
        (void)segment_deliver(&ahead, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason);
        if (ahead.segment_full_dropped != dropped_before) {
            // One record, not a whole pass: read_index moves, the ring stays full. That is what
            // "behind" means, and it is the state a loaded reader lives in.
            uint32_t len = 0;
            uint32_t from_ip = 0;
            uint16_t from_port = 0;
            uint16_t from_span = 0;
            uint8_t out[tt_SEGMENT_SLOT_BYTES];
            (void)segment_read(behind.own_segment, out, sizeof(out), &len, &from_ip, &from_port, &from_span);
        }
    }
    // Refused for far longer than the dead-reader threshold, or the arm cannot distinguish the rule
    // from its absence - which is the whole reason it exists.
    EXPECT_TRUE(ahead.segment_full_dropped > 1000);
    EXPECT_TRUE(ahead.segment_peers[OWNER_ID].mapping != NULL);
    EXPECT_TRUE(!ahead.segment_peers[OWNER_ID].missing);
    EXPECT_EQ_U32(0, ahead.segment_attach[tt_SEGMENT_REFUSED]);
    EXPECT_EQ_U32(0, (uint32_t)ahead.tx_datagrams_by_transport[tt_TRANSPORT_UDP]); // never flapped to UDP

    test_mock_segments_free();
}

// The drain's contract, which is the whole of the ordering fix stated as a property: it empties the
// ring, and when it cannot it says so.
//
// This is not fussiness about a return value. The socket is drained to exhaustion by drain_rx(),
// while this was draining 64 records against a 256-slot ring, so the ring ran permanently behind.
// TickLE sends DATA unicast with few peers and by broadcast otherwise, and a broadcast can never go
// over a segment - so some samples of the same stream always arrive by socket, and each one
// advances the subscriber's watermark past everything still queued in the ring, which is then
// discarded on arrival. One socket arrival discards the entire backlog behind it, which is why a
// few hundred thousand of them could invalidate thirty-six million ring records on CI's same-host
// cell. The caller must not read the socket while `emptied` is false, and this is where that is
// checked.
static void test_the_drain_empties_the_ring_or_says_it_did_not(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context owner;
    struct tt_Topic owner_topic;
    struct tt_Publisher owner_pub;
    init_node_topic_pub(&owner, &owner_topic, &owner_pub);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    // Control: an empty ring is emptied, trivially, and delivers nothing. Without this arm the
    // assertions below would also hold for a drain that always reported success.
    bool emptied = false;
    uint64_t acquisitions_before = owner.state_lock_stats.acquisitions;
    EXPECT_EQ_U32(0, drain_own_segment(&owner, &emptied));
    EXPECT_TRUE(emptied);
    // And it costs no lock. This is not a micro-optimisation dressed up as a test: a context on
    // another host from every peer has a ring that is ALWAYS empty, because nobody can attach to it,
    // so a lock taken per poll to discover that is a cost paid forever by the deployments that can
    // never benefit from the module. Plan measured the module costing 1.6-2.9% cross-host, and a
    // per-poll lock is exactly the size of thing that lands in that budget.
    EXPECT_EQ_U64(acquisitions_before, owner.state_lock_stats.acquisitions);

    // A ring holding more than the old 64-record cap. Every one of them comes out in a single call,
    // which is the change: at the old cap this returned after 64 and the caller went on to read the
    // socket with 136 records still queued behind it.
    const uint32_t records = 200;
    EXPECT_TRUE(records > 64); // the old cap, named here so this arm cannot quietly stop testing it
    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = PEER_CONTEXT_ID;
    for (uint32_t i = 0; i < records; i++) {
        EXPECT_TRUE(segment_write(owner.own_segment, &header, sizeof(header), NULL, 0, PEER_IP, PEER_PORT, 1));
    }

    emptied = false;
    EXPECT_EQ_U32(records, drain_own_segment(&owner, &emptied));
    EXPECT_TRUE(emptied);
    EXPECT_EQ_U32(owner.own_segment->write_index, owner.own_segment->read_index); // nothing left behind

    // And when it genuinely cannot empty the ring it reports that rather than pretending. Staged by
    // wedging the head - a writer that claimed a slot and never published it - with a record behind
    // it, which is the one case where records remain and no further progress is possible.
    struct tt_SegmentHeader* seg = owner.own_segment;
    uint32_t claimed = __atomic_load_n(&seg->write_index, __ATOMIC_RELAXED);
    __atomic_store_n(&seg->write_index, claimed + 1U, __ATOMIC_RELEASE); // claimed, never published
    emptied = false;
    EXPECT_EQ_U32(0, drain_own_segment(&owner, &emptied));
    // A wedged head is reported as emptied on purpose: nothing can ever be read past it, so refusing
    // to read the socket for ever would turn one dead writer into a dead context. The stall counter
    // is what carries that case, and it moved.
    EXPECT_TRUE(emptied);
    EXPECT_TRUE(owner.segment_head_stalls > 0);

    test_mock_segments_free();
}

// The doorbell. Shared memory cannot wake a thread that is inside a socket wait, and once the
// segment carries nearly all the traffic there is nothing left on the socket to wake it either: the
// reader then runs only on its timers. Measured on the same-host cell with ordering already fixed,
// that was 73,081 records delivered against UDP's 12,463,222, at 241 ms of latency. The module had
// been getting away with it only because a separate defect was still pushing millions of datagrams
// down the socket - so the doorbell looked optional for exactly as long as something else was
// accidentally doing its job.
//
// The control is the same publish with the flag clear: a loaded reader is never inside a wait, so it
// must cost nothing at all. Without that arm this would also pass against a writer that rang on
// every record, which is the version that would quietly undo the module's whole point.
static void test_a_sleeping_reader_is_rung_and_a_busy_one_is_not(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context owner;
    struct tt_Topic owner_topic;
    struct tt_Publisher owner_pub;
    init_node_topic_pub(&owner, &owner_topic, &owner_pub);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    EXPECT_EQ_U32(0, owner.own_segment->reader_waiting); // nobody is waiting yet

    struct tt_Context writer;
    struct tt_Topic writer_topic;
    struct tt_Publisher writer_pub;
    init_node_topic_pub(&writer, &writer_topic, &writer_pub);
    writer.hal.own_ip = PEER_IP;
    writer.hal.own_port = PEER_PORT;

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = PEER_CONTEXT_ID;

    // Control: the owner is awake. Records go into the ring and not one datagram goes to the socket.
    int sends_before = test_mock_send_to_call_count;
    for (int i = 0; i < 8; i++) {
        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
        EXPECT_TRUE(
            segment_deliver(&writer, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason));
    }
    EXPECT_EQ_INT(sends_before, test_mock_send_to_call_count);
    EXPECT_EQ_U32(0, (uint32_t)writer.segment_doorbells_sent);

    // Asleep. The owner publishes that before it blocks, and the writer reads it after it publishes.
    segment_reader_waiting(&owner, true);
    EXPECT_EQ_U32(1, owner.own_segment->reader_waiting);

    enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
    EXPECT_TRUE(segment_deliver(&writer, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason));
    EXPECT_EQ_INT(sends_before + 1, test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)writer.segment_doorbells_sent);
    // Zero length, to the owner's own address. The length is the whole of the contract: a datagram of
    // any other size would be parsed as data by the receiver.
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_last_wire_len);
    EXPECT_EQ_U32(OWNER_IP, test_mock_send_to_last_ip);
    EXPECT_EQ_U32((uint32_t)OWNER_PORT, (uint32_t)test_mock_send_to_last_port);
    // The record still went into the ring - the doorbell is a wake-up, not a delivery.
    EXPECT_EQ_U32(9, (uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U32(0, (uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_UDP]);

    // **A reader that does not answer is not rung again.** The flag says "asleep" and a reader killed
    // while blocked leaves it saying that for ever, with nobody to clear it - so without this rule a
    // writer rings a real sendto() for every datagram it sends to a corpse. Measured on a SIGKILL
    // run before the rule existed: 2,853,609 doorbells into a socket nobody was reading.
    //
    // read_index is what distinguishes the two. A live reader drains before it blocks again, so the
    // index has moved and the next record rings; a dead one leaves it where it was for ever.
    int sends_before_repeat = test_mock_send_to_call_count;
    for (int i = 0; i < 16; i++) {
        EXPECT_TRUE(
            segment_deliver(&writer, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason));
    }
    EXPECT_EQ_INT(sends_before_repeat, test_mock_send_to_call_count); // not one more, for sixteen records

    // And when the reader does answer - it drains, so read_index moves - the next record rings again.
    // Without this arm the assertion above would also hold for a doorbell that had simply stopped
    // working, which is the failure that would cost the module everything it just won.
    uint32_t len = 0;
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    uint16_t from_span = 0;
    uint8_t out[tt_SEGMENT_SLOT_BYTES];
    EXPECT_TRUE(segment_read(owner.own_segment, out, sizeof(out), &len, &from_ip, &from_port, &from_span));
    int rung_after_drain = test_mock_send_to_call_count;
    EXPECT_TRUE(segment_deliver(&writer, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason));
    EXPECT_EQ_INT(rung_after_drain + 1, test_mock_send_to_call_count);

    // Awake again: back to costing nothing.
    segment_reader_waiting(&owner, false);
    int after_wake = test_mock_send_to_call_count;
    EXPECT_TRUE(segment_deliver(&writer, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason));
    EXPECT_EQ_INT(after_wake, test_mock_send_to_call_count);

    // And the receiving end treats a zero-length datagram as a doorbell rather than as a malformed
    // datagram: counted, and dropped before the magic check, so ringing it never fills the log with
    // errors about the module doing its job.
    uint64_t rung_before = owner.segment_doorbells_received;
    uint64_t malformed_before = owner.rx_malformed_drops;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)process_datagram(&owner, 0, PEER_IP, PEER_PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_U32(1, (uint32_t)(owner.segment_doorbells_received - rung_before));
    EXPECT_EQ_U32(0, (uint32_t)(owner.rx_malformed_drops - malformed_before));

    test_mock_segments_free();
}

// ---------------------------------------------------------------------------------------------
// Lazy segment creation (SHM_PLAN stage 1, option B).
//
// The segment used to be built at bind, which meant every context paid for a ring whether or not
// anything on its host could ever open one - a cross-host-only deployment carried the whole cost for
// nothing. It is now built when something can use it, and the only reason that is possible is that
// discovery reports both edges: an announce says a same-host peer appeared, and a departure says one
// has gone.
//
// The claim under test is a negative one, which is why the controls matter more than usual here:
// "no segment yet" looks exactly like "the segment failed to build", and a test that only checks the
// creating cases would pass just as well against the old eager code. So the first case below asserts
// the absence, the third asserts that a peer on another host still produces nothing, and both would
// fail if creation were moved back to bind.
static void test_a_context_alone_on_its_host_builds_no_segment(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    // Nothing has happened to it but coming up. This is the assertion the eager version fails.
    EXPECT_TRUE(node.own_segment == NULL);
    EXPECT_EQ_U32(0, (uint32_t)node.segments_created);
    EXPECT_EQ_U32(0, (uint32_t)node.same_host_peer_count);
    EXPECT_EQ_INT(0, test_mock_segment_creates);
}

static void test_a_same_host_peer_appearing_builds_the_segment(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    note_same_host_peer(&node, PEER_CONTEXT_ID, own_ip);

    EXPECT_TRUE(node.own_segment != NULL);
    EXPECT_EQ_U32(1, (uint32_t)node.segments_created);
    EXPECT_EQ_U32(1, (uint32_t)node.same_host_peer_count);
    EXPECT_EQ_INT(1, test_mock_segment_creates);

    // And it is a usable segment, not merely a non-NULL pointer: the header a peer checks first.
    EXPECT_EQ_U32(tt_SEGMENT_MAGIC, node.own_segment->magic);
    EXPECT_EQ_U32(OWNER_ID, node.own_segment->owner_context_id);

    release_segments(&node);
}

// The control. A peer that does not share our address can never open the file we would create, so
// creating one for it is the eager behaviour under a different name. Remove the address comparison
// in note_same_host_peer() and this is the case that fails.
static void test_a_peer_on_another_host_builds_nothing(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    // Same port, different host - so it is the address that decides and not some other field.
    note_same_host_peer(&node, PEER_CONTEXT_ID, own_ip ^ 0x00010000U);

    EXPECT_TRUE(node.own_segment == NULL);
    EXPECT_EQ_U32(0, (uint32_t)node.segments_created);
    EXPECT_EQ_U32(0, (uint32_t)node.same_host_peer_count);
    EXPECT_EQ_INT(0, test_mock_segment_creates);
}

// Announces repeat - every periodic refresh arrives here - so the appearing edge has to be idempotent
// or a context would unlink and rebuild its segment under its peers once a second.
static void test_a_repeated_announce_builds_nothing_more(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    struct tt_SegmentHeader* first = NULL;
    for (int i = 0; i < 5; i++) {
        note_same_host_peer(&node, PEER_CONTEXT_ID, own_ip);
        if (first == NULL) {
            first = node.own_segment;
        }
    }

    EXPECT_EQ_U32(1, (uint32_t)node.segments_created);
    EXPECT_EQ_U32(1, (uint32_t)node.same_host_peer_count);
    EXPECT_EQ_INT(1, test_mock_segment_creates);
    EXPECT_TRUE(node.own_segment == first); // the same region throughout, not a replacement

    // A second, different peer on the same host counts once more but still builds nothing.
    note_same_host_peer(&node, PEER_CONTEXT_ID + 1, own_ip);
    EXPECT_EQ_U32(2, (uint32_t)node.same_host_peer_count);
    EXPECT_EQ_INT(1, test_mock_segment_creates);

    release_segments(&node);
}

// Self-delivery. A context unicasts to itself and then attaches to the file it created, which is why
// release_segments() has to unmap that region exactly once. With creation deferred, asking for our
// own id is the edge that keeps that on shared memory - and a context alone on its host has no other
// trigger at all, so without this it would have silently moved to UDP and no test would have said so.
static void test_delivering_to_ourselves_builds_the_segment(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    EXPECT_TRUE(node.own_segment == NULL); // nothing yet, and no announce is coming

    struct tt_SegmentHeader* mine = peer_segment(&node, node.id, own_ip, own_port);

    EXPECT_TRUE(node.own_segment != NULL); // asking for our own segment built it
    EXPECT_EQ_U32(1, (uint32_t)node.segments_created);
    EXPECT_TRUE(mine != NULL);                             // and the ask was answered, so a self-send takes it
    EXPECT_EQ_U32(0, (uint32_t)node.same_host_peer_count); // we are not a peer of ourselves

    release_segments(&node);
}

// The departing edge. Deferred creation without it only moves the cost: a context whose same-host
// peers have all gone would hold the ring for the rest of its life.
static void test_the_last_same_host_peer_leaving_takes_the_segment(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    note_same_host_peer(&node, PEER_CONTEXT_ID, own_ip);
    note_same_host_peer(&node, PEER_CONTEXT_ID + 1, own_ip);
    EXPECT_EQ_U32(2, (uint32_t)node.same_host_peer_count);
    EXPECT_TRUE(node.own_segment != NULL);

    // One of two leaving is not the last one. This is the control for the release: a rule that
    // released on any departure would pass every other assertion here and fail this one.
    forget_same_host_peer(&node, PEER_CONTEXT_ID);
    EXPECT_EQ_U32(1, (uint32_t)node.same_host_peer_count);
    EXPECT_TRUE(node.own_segment != NULL);
    EXPECT_EQ_U32(0, (uint32_t)node.segments_released);

    forget_same_host_peer(&node, PEER_CONTEXT_ID + 1);
    EXPECT_EQ_U32(0, (uint32_t)node.same_host_peer_count);
    EXPECT_TRUE(node.own_segment == NULL);
    EXPECT_EQ_U32(1, (uint32_t)node.segments_released);
    EXPECT_EQ_INT(1, test_mock_segment_unlinks);

    // And a peer appearing again builds a fresh one, so release is not a one-way door.
    note_same_host_peer(&node, PEER_CONTEXT_ID, own_ip);
    EXPECT_TRUE(node.own_segment != NULL);
    EXPECT_EQ_U32(2, (uint32_t)node.segments_created);

    release_segments(&node);
}

// A departure this context never counted must not decrement anything. The count is read to decide
// whether to release, so an unbalanced decrement would wrap uint16_t to 65535 and the segment would
// never be released again - or, from 1, release it while a peer was still there.
static void test_a_peer_we_never_counted_leaving_changes_nothing(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    note_same_host_peer(&node, PEER_CONTEXT_ID, own_ip);
    EXPECT_EQ_U32(1, (uint32_t)node.same_host_peer_count);

    // A cross-host peer we never counted, and the same one twice.
    forget_same_host_peer(&node, PEER_CONTEXT_ID + 9);
    forget_same_host_peer(&node, PEER_CONTEXT_ID);
    forget_same_host_peer(&node, PEER_CONTEXT_ID);

    EXPECT_EQ_U32(0, (uint32_t)node.same_host_peer_count); // zero, not 65535
    EXPECT_EQ_U32(1, (uint32_t)node.segments_released);    // released once, not twice
    EXPECT_EQ_INT(1, test_mock_segment_unlinks);

    release_segments(&node);
}

// Self-delivery is a claim on the segment that no departure can settle. A context that delivers to
// itself has our own id in the peer table pointing at our own region; releasing it there would unmap
// a region that table still refers to, and the next self-send would write through a dangling pointer.
static void test_a_context_delivering_to_itself_keeps_its_segment(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    note_same_host_peer(&node, PEER_CONTEXT_ID, own_ip);
    // We now deliver to ourselves as well, which attaches our own id to our own region.
    EXPECT_TRUE(peer_segment(&node, node.id, own_ip, own_port) != NULL);
    EXPECT_TRUE(node.segment_peers[node.id].mapping != NULL);

    forget_same_host_peer(&node, PEER_CONTEXT_ID);

    EXPECT_EQ_U32(0, (uint32_t)node.same_host_peer_count);
    EXPECT_TRUE(node.own_segment != NULL);              // still ours to use
    EXPECT_EQ_U32(0, (uint32_t)node.segments_released); // and not released under us
    EXPECT_EQ_INT(0, test_mock_segment_unlinks);

    release_segments(&node);
}

// move_id() renumbers a live context when two hold one id, and the peer table does not move with it:
// a context that had attached to its own segment under its old id still has that entry, at the old
// index. Releasing on the LAST departure then has to find that entry by identity, because looking at
// segment_peers[node->id] would find an empty slot, unmap the region, and leave the old entry
// pointing into it - a use-after-munmap, and one this release path introduced by being the first
// thing that can unmap a segment while the context is still running.
static void test_a_renumbered_context_still_knows_it_holds_its_own_segment(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&node, &topic, &pub);
    node.id = OWNER_ID;
    node.hal.own_ip = OWNER_IP;
    node.hal.own_port = OWNER_PORT;

    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    note_same_host_peer(&node, PEER_CONTEXT_ID, own_ip);
    EXPECT_TRUE(peer_segment(&node, node.id, own_ip, own_port) != NULL);
    struct tt_SegmentHeader* own = node.own_segment;
    EXPECT_TRUE(node.segment_peers[OWNER_ID].mapping == own);

    // The collision, as move_id() leaves it: a new id, and the old entry exactly where it was.
    node.id = OWNER_ID + 5;
    EXPECT_TRUE(node.segment_peers[node.id].mapping == NULL); // what indexing by the new id would see

    forget_same_host_peer(&node, PEER_CONTEXT_ID);

    EXPECT_TRUE(node.own_segment != NULL); // still held, because we still use it
    EXPECT_EQ_U32(0, (uint32_t)node.segments_released);
    EXPECT_EQ_INT(0, test_mock_segment_unlinks);
    EXPECT_TRUE(node.segment_peers[OWNER_ID].mapping == own); // and the old entry is not dangling

    release_segments(&node);
    EXPECT_EQ_INT(0, test_mock_segment_double_detaches);
}

// A segment whose geometry is not ours is refused, because every slot address inside it is computed
// from the OWNER's numbers while the mapping length is ours.
//
// Measured on 2026-10-02 with two real builds, a 512-slot owner and a 256-slot attacher: it does not
// fault, which is what makes it worth a test. The writer fills the slots it can address, then reads a
// slot header beyond its mapping, finds a sequence that is not the index it claimed, and returns
// "ring full" - for ever, because write_index never advances past that slot. 1535 of 2000 messages
// were dropped silently while the publisher reported "sent 2,000 message(s)" and exited 0.
//
// The opposite direction (owner SMALLER) was already refused by tt_segment_attach()'s fstat, which is
// why this hole survived: the half that is visible from the file size was covered in the HAL, and a
// larger file looks fine from there.
// SHM_PLAN 6e: the slot size is the user's, set at runtime. Three things, because a value that is read but
// ignored reads exactly like a value that had no effect - which is what tt_SEGMENT_SLOT_BYTES itself was
// until 2026-09-29, when it turned out -D had never reached it.
// SHM_PLAN 6e(a): the limit a sample may reach before being split is raised only for destinations that can
// actually take a whole record. Each condition is checked on its own, because the cost of getting this wrong is
// a record larger than a datagram with nowhere to go - UDP is not a fallback for it.
//
// And the ordering this exposes, which is behaviour rather than a test detail: a publisher attaches to a peer's
// segment on the SEND path, after the limit has been decided, so the FIRST sample to a peer always splits as the
// network requires and only later ones can go whole. Conservative in the right direction, and asserted below so
// that it is a decision rather than an accident.

static void test_only_an_attached_same_host_peer_raises_the_whole_limit(void) {
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

    init_node_topic_pub(&sender, &topic, &pub);
    sender.hal.own_ip = PEER_IP;
    sender.hal.own_port = PEER_PORT;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub.peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub.peers[0].context_id = OWNER_ID;
    pub.peers[0].ip = OWNER_IP;
    pub.peers[0].port = OWNER_PORT;

    // Nothing attached yet: this is the first-publish state, and it must refuse.
    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, pub.peers, 1));

    // A broadcast refuses whatever is attached, because its destinations are not these peers.
    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, NULL, 0));

    // Attach, as the send path does.
    EXPECT_TRUE(peer_segment(&sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    EXPECT_EQ_U32((uint32_t)tt_SEGMENT_SLOT_BYTES, whole_record_limit_for(&sender, pub.peers, 1));

    // A destination whose slots would put payloads off 4-alignment. An encoder writing a record into a
    // slot needs the alignment tx_buffer is asserted to have, and with both segment structs a multiple of
    // 4 that reduces to slot_bytes % 4. Checked here before any path depends on it, so the guard cannot
    // arrive after the code that needs it.
    uint32_t real_slot_bytes = sender.segment_peers[OWNER_ID].mapping->slot_bytes;
    sender.segment_peers[OWNER_ID].mapping->slot_bytes = real_slot_bytes - 1U;
    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, pub.peers, 1));
    sender.segment_peers[OWNER_ID].mapping->slot_bytes = real_slot_bytes;
    EXPECT_EQ_U32((uint32_t)tt_SEGMENT_SLOT_BYTES, whole_record_limit_for(&sender, pub.peers, 1));

    // A peer that moved: same context id, different address. The entry describes the segment we opened for
    // the OLD address, so granting on it would build a record for a segment we have never seen.
    uint32_t real_ip = pub.peers[0].ip;
    pub.peers[0].ip = real_ip + 1;
    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, pub.peers, 1));
    pub.peers[0].ip = real_ip;

    // Two peers, one of them not attached: the record is built once and every destination has to take it.
    pub.peers[1].context_id = (uint8_t)(OWNER_ID + 1);
    pub.peers[1].ip = OWNER_IP + 2;
    pub.peers[1].port = OWNER_PORT;
    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, pub.peers, 2));
    pub.peers[1].context_id = tt_CONTEXT_ID_INVALID;

    EXPECT_EQ_U32((uint32_t)tt_SEGMENT_SLOT_BYTES, whole_record_limit_for(&sender, pub.peers, 1));
    release_segments(&sender);
    release_own_segment(&owner);
    test_mock_segments_free();
}

// The limit must read peer_count entries and not one more. Every case above hands it pub.peers, a full
// tt_MAX_PEER_COUNT array, which is why none of them caught the loop walking tt_MAX_PEER_COUNT and skipping
// holes: that is safe for pub.peers and reads off the end of the single tt_Peer the retransmit path puts on
// its stack - end_encode(node, hdr, true, &target, 1), from send_cached_record(). Found on 2026-10-03 by the
// fuzz tier as an ASan stack-buffer-overflow, one second into the run, after the gates and 44 unit suites
// had all passed. The tests exercised the decision; nothing exercised the shape of the caller's array.
//
// This is a one-element array deliberately. Under ASan - which is how CI runs the unit suite, and how this
// was caught - anything read past [0] is a reported overflow rather than a silent pass.
static void test_the_limit_reads_only_the_peers_it_was_given(void) {
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

    init_node_topic_pub(&sender, &topic, &pub);
    sender.hal.own_ip = PEER_IP;
    sender.hal.own_port = PEER_PORT;

    // Exactly one, as the retransmit path has.
    struct tt_Peer target;
    memset(&target, 0, sizeof(target));
    target.context_id = OWNER_ID;
    target.ip = OWNER_IP;
    target.port = OWNER_PORT;

    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, &target, 1)); // nothing attached yet
    EXPECT_TRUE(peer_segment(&sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    EXPECT_EQ_U32((uint32_t)tt_SEGMENT_SLOT_BYTES, whole_record_limit_for(&sender, &target, 1));

    // And through the bound every send site actually asks, not only the predicate underneath it.
    EXPECT_EQ_U32((uint32_t)tt_SEGMENT_SLOT_BYTES,
                  record_size_limit(&sender, (uint32_t)sizeof(struct tt_Header), &target, 1));

    // The case that actually overflowed, and the first version of this test did not cover it. With a VALID
    // single peer the old loop stopped at i=0 because `seen` reached peer_count, so it passed on the broken
    // source and would have shipped as cover. An INVALID entry never increments `seen`, so the old loop ran
    // on to tt_MAX_PEER_COUNT and read off the end of this one-element object. That is the shape the fuzzer
    // produced: a retransmit target whose context id is unset.
    struct tt_Peer lone_invalid;
    memset(&lone_invalid, 0, sizeof(lone_invalid));
    lone_invalid.context_id = tt_CONTEXT_ID_INVALID;
    EXPECT_EQ_U32(0, whole_record_limit_for(&sender, &lone_invalid, 1));
    EXPECT_EQ_U32((uint32_t)sizeof(struct tt_Header),
                  record_size_limit(&sender, (uint32_t)sizeof(struct tt_Header), &lone_invalid, 1));

    release_segments(&sender);
    release_own_segment(&owner);
    test_mock_segments_free();
}

static void test_a_configured_slot_size_is_the_one_built(void) {
    const uint32_t configured = (uint32_t)tt_SEGMENT_SLOT_BYTES / 2U;
    struct tt_Context owner;
    uint32_t saved = _tt_CONFIG.segment_slot_bytes;

    // The control first, and it is not decoration: with nothing configured the header must carry the
    // COMPILED default, or "the configured value appeared" would be indistinguishable from "every segment
    // has that size now".
    _tt_CONFIG.segment_slot_bytes = 0;
    memset(&owner, 0, sizeof(owner));
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    EXPECT_EQ_U32((uint32_t)tt_SEGMENT_SLOT_BYTES, owner.own_segment->slot_bytes);
    release_own_segment(&owner);
    test_mock_segments_free();

    _tt_CONFIG.segment_slot_bytes = configured;
    memset(&owner, 0, sizeof(owner));
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    // In the header, which is what a peer reads and what sizes every later map of this region.
    EXPECT_EQ_U32(configured, owner.own_segment->slot_bytes);
    EXPECT_EQ_U32((uint32_t)tt_SEGMENT_SLOTS, owner.own_segment->slots);
    release_own_segment(&owner);
    test_mock_segments_free();

    // And a size this build cannot use is refused rather than quietly replaced by the default, because a
    // silently ignored setting is the failure this whole field exists to avoid repeating.
    _tt_CONFIG.segment_slot_bytes = 1; // below a datagram header
    memset(&owner, 0, sizeof(owner));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, (int)tt_Context_create(&owner));

    // Above a datagram is refused for the same reason and a different cause: a slot larger than a datagram
    // is only useful for carrying a record whole, and that path is held until SHM_PLAN 6e(b) because it was
    // measured to collapse the cell it was meant to win (see valid_slot_bytes()).
    //
    // Only compiled where it can decide. In a build whose samples all fit a datagram, tt_MAX_SAMPLE_LENGTH
    // and FRAG_WHOLE_DATA_LIMIT are the same number, so the old ceiling and the new one reject exactly the
    // same values and the assertion below passes whichever is in the source - checked by mutation on
    // 2026-10-03, where swapping the bound back left the whole suite green. A test that cannot fail is worse
    // than no test, because it reads as cover. This asserts the ceiling in a build that can tell them apart
    // (any frag build, e.g. the p4 harness at tt_MAX_SAMPLE_LENGTH=4096) and says so where it cannot.
#if tt_MAX_SAMPLE_LENGTH > FRAG_WHOLE_DATA_LIMIT
    _tt_CONFIG.segment_slot_bytes = (uint32_t)FRAG_WHOLE_DATA_LIMIT + 1U;
    memset(&owner, 0, sizeof(owner));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, (int)tt_Context_create(&owner));
#endif

    // The boundary itself is allowed, so the refusal above is the ceiling and not an off-by-one that would
    // also reject every usable size.
    _tt_CONFIG.segment_slot_bytes = (uint32_t)FRAG_WHOLE_DATA_LIMIT;
    memset(&owner, 0, sizeof(owner));
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    EXPECT_EQ_U32((uint32_t)FRAG_WHOLE_DATA_LIMIT, owner.own_segment->slot_bytes);
    release_own_segment(&owner);
    test_mock_segments_free();

    _tt_CONFIG.segment_slot_bytes = saved;
}

static void test_a_segment_with_another_geometry_is_refused(void) {
    test_mock_reset();
    test_mock_segments_free();

    struct tt_Context owner;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node_topic_pub(&owner, &topic, &pub);
    owner.id = OWNER_ID;
    owner.entity_id_base = OWNER_INCARNATION;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    // The header is otherwise perfect: right magic, version, owner and incarnation. Only the geometry
    // differs - and what that means changed when the attach became two-step.
    //
    // A geometry that is merely DIFFERENT is now attached rather than refused. The attacher reads these numbers
    // and maps what the owner actually built, so a bigger ring or a different slot size is something it
    // handles; a datagram too large for the owner's slot goes over UDP and is counted, which is already the
    // documented behaviour for a sample that does not fit. Between c97fac8b and the two-step attach these
    // refused, correctly, because the attacher mapped its own length and then indexed with the owner's.
    EXPECT_EQ_U32((uint32_t)tt_SEGMENT_SLOTS, owner.own_segment->slots);
    owner.own_segment->slots = (uint32_t)tt_SEGMENT_SLOTS * 2U;
    EXPECT_EQ_INT(tt_SEGMENT_ATTACHED, (int)segment_header_check(owner.own_segment, OWNER_IP, OWNER_PORT, OWNER_ID, 0));

    owner.own_segment->slots = (uint32_t)tt_SEGMENT_SLOTS; // restore, then the other field
    EXPECT_EQ_INT(tt_SEGMENT_ATTACHED, (int)segment_header_check(owner.own_segment, OWNER_IP, OWNER_PORT, OWNER_ID, 0));
    owner.own_segment->slot_bytes = (uint32_t)tt_SEGMENT_SLOT_BYTES - 1U;
    EXPECT_EQ_INT(tt_SEGMENT_ATTACHED, (int)segment_header_check(owner.own_segment, OWNER_IP, OWNER_PORT, OWNER_ID, 0));
    owner.own_segment->slot_bytes = (uint32_t)tt_SEGMENT_SLOT_BYTES;

    // What stays refused is a geometry that cannot be INDEXED, because segment_slot() masks with slots - 1.
    // Three ways to be unindexable, each checked: a guard that only rejected zero would pass a
    // non-power-of-two mask, which skips addresses that exist rather than making every index wild.
    owner.own_segment->slots = 0;
    EXPECT_EQ_INT(tt_SEGMENT_BAD_HEADER,
                  (int)segment_header_check(owner.own_segment, OWNER_IP, OWNER_PORT, OWNER_ID, 0));
    owner.own_segment->slots = (uint32_t)tt_SEGMENT_SLOTS + 1U; // not a power of two
    EXPECT_EQ_INT(tt_SEGMENT_BAD_HEADER,
                  (int)segment_header_check(owner.own_segment, OWNER_IP, OWNER_PORT, OWNER_ID, 0));
    owner.own_segment->slots = (uint32_t)tt_SEGMENT_SLOTS;
    owner.own_segment->slot_bytes = 0;
    EXPECT_EQ_INT(tt_SEGMENT_BAD_HEADER,
                  (int)segment_header_check(owner.own_segment, OWNER_IP, OWNER_PORT, OWNER_ID, 0));
    owner.own_segment->slot_bytes = (uint32_t)tt_SEGMENT_SLOT_BYTES;

    // And the control that keeps this from being "refuse everything": our own geometry still attaches.
    owner.own_segment->slot_bytes = (uint32_t)tt_SEGMENT_SLOT_BYTES;
    EXPECT_EQ_INT(tt_SEGMENT_ATTACHED, (int)segment_header_check(owner.own_segment, OWNER_IP, OWNER_PORT, OWNER_ID, 0));

    release_segments(&owner);
}

int main(void) {
    test_ordinary_publish_is_counted_as_udp();
    test_zerocopy_publish_is_counted_as_udp();
    test_batch_shape_is_counted_per_datagram();
    test_reset_zeroes_the_per_transport_counters();
    test_segment_name_separates_peers_that_differ();
    test_segment_header_catches_what_the_name_cannot();
    test_only_an_attached_same_host_peer_raises_the_whole_limit();
    test_the_limit_reads_only_the_peers_it_was_given();
    test_a_configured_slot_size_is_the_one_built();
    test_a_segment_with_another_geometry_is_refused();
    test_ring_round_trips_a_datagram();
    test_full_ring_refuses_rather_than_overwriting();
    test_ring_survives_many_wraps();
    test_impossible_length_is_refused_and_does_not_wedge();
    test_two_writers_contend_for_one_segment();
    test_every_udp_datagram_has_a_named_reason();
    test_a_datagram_crosses_a_segment();
    test_segment_bytes_equal_what_udp_would_have_sent();
    test_a_refused_datagram_is_refused_over_the_segment_too();
    test_received_datagram_is_counted_as_udp();
    test_a_peer_with_no_segment_is_asked_once_not_per_datagram();
    test_capacity_exhaustion_is_counted_and_warned_once();
    test_a_stalled_head_is_noticed_rather_than_read_as_a_sizing_problem();
    test_destroy_takes_the_segment_with_it();
    test_a_segment_left_by_a_dead_owner_is_reclaimed();
    test_a_writer_gives_up_on_a_ring_nobody_drains();
    test_the_drain_empties_the_ring_or_says_it_did_not();
    test_a_sleeping_reader_is_rung_and_a_busy_one_is_not();
    test_a_context_alone_on_its_host_builds_no_segment();
    test_a_same_host_peer_appearing_builds_the_segment();
    test_a_peer_on_another_host_builds_nothing();
    test_a_repeated_announce_builds_nothing_more();
    test_delivering_to_ourselves_builds_the_segment();
    test_the_last_same_host_peer_leaving_takes_the_segment();
    test_a_peer_we_never_counted_leaving_changes_nothing();
    test_a_context_delivering_to_itself_keeps_its_segment();
    test_a_renumbered_context_still_knows_it_holds_its_own_segment();

    printf("test_transport_seam: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
