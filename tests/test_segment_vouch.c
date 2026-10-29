/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A writer attaches only to a segment discovery vouches for (segment_vouched(), tickle.c), 2026-10-29.
//
// The defect: a segment is named from the owner's (address, data port, context id), which is unique within one network
// stack and not across network namespaces - they share /dev/shm, and two lo-only namespaces are both 127.0.0.1. A
// writer in one attached to the other's segment under the same name and its samples were delivered there
// (examples/perf_hil/experiments/shm_netns_collision.sh: 13 of 20 forced rounds on the parent of this commit). The
// header's owner triple matches by construction, so the header check alone accepts it. What tells them apart is what
// discovery heard over UDP, through the writer's own stack: the owner's entity ids are its entity_id_base, which is
// the header's incarnation, plus a count.
//
// Checked, with discovery driven by real announces through process_data() where it can be:
// - the collision: a segment under the peer's name with another incarnation is refused, its ring is not written (not
//   even the fence), and the datagram goes over UDP - the control shows the header check alone accepts it;
// - a peer discovery has heard is attached and a datagram crosses the ring (shm counters > 0);
// - a peer not heard yet is refused as UNVOUCHED, and attached once its announce arrives - shared memory is not lost;
// - the bound, at its edge: an entity id tt_SEGMENT_VOUCH_SPAN - 1 above the incarnation vouches, one at the span and
//   one below the incarnation do not;
// - ids that wrapped past 0 under a base near the top of the range vouch, heard in any order;
// - a base of 0 is the incarnation itself, not a clock reading no entity id can match;
// - a revalidation refuses a mapping that a later announce no longer vouches for;
// - a context sending to itself attaches to its own segment and refuses a stranger's under its own name.
//
// The first two tests use nothing this commit added, so they build against its parent, where the collision test fails.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#if tt_SEGMENT_ENABLED

#define HOST_IP 0x7f000001U // 127.0.0.1: what both namespaces of the collision have
#define OWNER_ID 7
#define OWNER_PORT 8282
#define WRITER_ID 2
#define WRITER_PORT 7000
#define BASE_HERE 0x12345678U  // the peer discovery heard, in the writer's namespace
#define BASE_THERE 0x9abcdef0U // the stranger in the other namespace, under the same name
#define UNMATCHED_ENDPOINT 0x0e0e0e0eU

static uint64_t announce_generation = 100;

static void init_context(struct tt_Context* node, uint8_t id, uint16_t port, uint32_t base) {
    memset(node, 0, sizeof(*node));
    tt_InitOptions_default(&node->opts);
    node_init_locks(node);
    node->id = id;
    node->entity_id_base = base;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node->hal.own_ip = HOST_IP;
    node->hal.own_port = port;
}

// `writer` hears an announce of context `source`, sent from HOST_IP:`port`, listing one Subscriber per entity id in
// `ids` (of an endpoint nothing here publishes, so nothing matches): what discovery records, through process_data().
static void hear(struct tt_Context* writer, uint8_t source, uint16_t port, const uint32_t* ids, int count) {
    uint8_t* buf = writer->rx_buffer;
    struct test_announce* announce = test_announce_at(buf);
    test_announce_set_last_modified(announce, announce_generation++);
    announce->announce.entity_count = (uint16_t)count;
    uint32_t tail = sizeof(struct test_announce);
    for (int i = 0; i < count; i++) {
        struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(buf + tail);
        memset(entity, 0, sizeof(*entity));
        entity->endpoint_id = UNMATCHED_ENDPOINT + (uint32_t)i;
        entity->entity_id = ids[i];
        entity->kind = tt_KIND_TOPIC_SUBSCRIBER;
        tail += sizeof(struct tt_UpdateEntity);
        tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, "vouch_type");
        tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, "vouch_sub");
    }
    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = source;
    EXPECT_TRUE(process_data(writer, &header, buf, 0, tail, HOST_IP, port));
}

// One datagram from `writer` to context `id` at HOST_IP:`port`: true when it went into the peer's ring.
static bool send_one(struct tt_Context* writer, uint8_t id, uint16_t port) {
    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = writer->id;
    enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
    return segment_deliver(writer, id, HOST_IP, port, &header, sizeof(header), NULL, 0, &reason);
}

static void drain_pass(struct tt_Context* node) {
    bool emptied = false;
    (void)drain_own_segment(node, &emptied);
}

// The collision. The writer's namespace has a peer at 127.0.0.1:8282 with context id 7 and base BASE_HERE, which it has
// heard. Another namespace has a context at the same three, base BASE_THERE, and its segment is the file under the
// name - the writer's peer never built one, or the stranger's replaced it (a create unlinks first).
static void test_a_segment_another_namespace_owns_is_refused(void) {
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context stranger;
    init_context(&stranger, OWNER_ID, OWNER_PORT, BASE_THERE);
    create_own_segment(&stranger);
    EXPECT_TRUE(stranger.own_segment != NULL);
    if (stranger.own_segment == NULL) {
        return;
    }
    // Control: the name and the header's owner triple both match - the check that ran before this commit accepts it.
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED,
                  (int)segment_header_check(stranger.own_segment, HOST_IP, OWNER_PORT, OWNER_ID, 0));

    struct tt_Context writer;
    init_context(&writer, WRITER_ID, WRITER_PORT, 0x5555aaaaU);
    const uint32_t ids[] = {BASE_HERE + 2U, BASE_HERE + 5U};
    hear(&writer, OWNER_ID, OWNER_PORT, ids, 2);

    for (int i = 0; i < 3; i++) {
        EXPECT_TRUE(!send_one(&writer, OWNER_ID, OWNER_PORT)); // over UDP, every time
    }
    EXPECT_EQ_U32(0, (uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U32(0, writer.segment_attach[tt_SEGMENT_ATTACHED]);
    EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_WRONG_OWNER]); // asked once; the "no" stands
    EXPECT_EQ_U32(0, stranger.own_segment->write_index);             // nothing written into its ring, not even a fence
    drain_pass(&stranger);
    EXPECT_EQ_U32(0, (uint32_t)stranger.rx_datagrams_by_transport[tt_TRANSPORT_SHM]); // nothing delivered there
    release_segments(&writer);
    release_segments(&stranger);
    test_mock_segments_free();
}

// The same writer and a peer that is the one it heard: attached, and a datagram crosses.
static void test_a_peer_discovery_heard_is_attached(void) {
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context owner;
    init_context(&owner, OWNER_ID, OWNER_PORT, BASE_HERE);
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);

    struct tt_Context writer;
    init_context(&writer, WRITER_ID, WRITER_PORT, 0x5555aaaaU);
    const uint32_t ids[] = {BASE_HERE + 3U, BASE_HERE + 1U, BASE_HERE + 2U};
    hear(&writer, OWNER_ID, OWNER_PORT, ids, 3); // answered unicast, by the first contact's reply - over the ring
    const uint64_t sent_before = writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM];

    EXPECT_TRUE(send_one(&writer, OWNER_ID, OWNER_PORT));
    EXPECT_TRUE(send_one(&writer, OWNER_ID, OWNER_PORT));
    EXPECT_EQ_U32(2, (uint32_t)(writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM] - sent_before));
    EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_ATTACHED]);
    EXPECT_EQ_U32(0, writer.segment_attach[tt_SEGMENT_WRONG_OWNER]);
    drain_pass(&owner);
    EXPECT_EQ_U32((uint32_t)writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM],
                  (uint32_t)owner.rx_datagrams_by_transport[tt_TRANSPORT_SHM]); // all of them, the reply included
    release_segments(&writer);
    release_segments(&owner);
    test_mock_segments_free();
}

// Not heard yet: no attach, and no "no" that outlives the announce.
static void test_a_peer_not_heard_yet_is_attached_once_heard(void) {
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context owner;
    init_context(&owner, OWNER_ID, OWNER_PORT, BASE_HERE);
    create_own_segment(&owner);

    struct tt_Context writer;
    init_context(&writer, WRITER_ID, WRITER_PORT, 0x5555aaaaU);
    EXPECT_TRUE(!send_one(&writer, OWNER_ID, OWNER_PORT));
    EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_UNVOUCHED]);
    EXPECT_EQ_U32(0, owner.own_segment->write_index);

    // Heard from another port - another incarnation under this id: still not this segment's owner.
    const uint32_t ids[] = {BASE_HERE};
    hear(&writer, OWNER_ID, OWNER_PORT + 1U, ids, 1);
    EXPECT_EQ_INT((int)tt_SEGMENT_UNVOUCHED, (int)segment_vouched(&writer, OWNER_ID, OWNER_PORT, BASE_HERE));

    // Heard from its own port: the announce lets the cached "no" go (note_same_host_peer()) and the next send attaches.
    hear(&writer, OWNER_ID, OWNER_PORT, ids, 1);
    EXPECT_TRUE(send_one(&writer, OWNER_ID, OWNER_PORT));
    EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_ATTACHED]);
    EXPECT_TRUE(writer.tx_datagrams_by_transport[tt_TRANSPORT_SHM] > 0);
    release_segments(&writer);
    release_segments(&owner);
    test_mock_segments_free();
}

// The bound, exactly, and an id below the incarnation (offset wraps to near 2^32).
static void test_the_vouch_span_at_its_edge(void) {
    struct tt_Context writer;
    init_context(&writer, WRITER_ID, WRITER_PORT, 0x5555aaaaU);
    const uint32_t span = (uint32_t)tt_SEGMENT_VOUCH_SPAN;
    const uint32_t last_in[] = {BASE_HERE + span - 1U};
    hear(&writer, OWNER_ID, OWNER_PORT, last_in, 1);
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED, (int)segment_vouched(&writer, OWNER_ID, OWNER_PORT, BASE_HERE));
    const uint32_t first_out[] = {BASE_HERE + span};
    hear(&writer, OWNER_ID, OWNER_PORT, first_out, 1);
    EXPECT_EQ_INT((int)tt_SEGMENT_WRONG_OWNER, (int)segment_vouched(&writer, OWNER_ID, OWNER_PORT, BASE_HERE));
    const uint32_t below[] = {BASE_HERE - 1U};
    hear(&writer, OWNER_ID, OWNER_PORT, below, 1);
    EXPECT_EQ_INT((int)tt_SEGMENT_WRONG_OWNER, (int)segment_vouched(&writer, OWNER_ID, OWNER_PORT, BASE_HERE));
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED, (int)segment_vouched(&writer, OWNER_ID, OWNER_PORT, BASE_HERE - 1U));
}

// A base two below 2^32: its ids wrapped past 0, heard in an order whose numeric minimum is not the earliest.
static void test_ids_that_wrapped_past_zero_vouch(void) {
    const uint32_t base = 0xFFFFFFFEU;
    struct tt_Context writer;
    init_context(&writer, WRITER_ID, WRITER_PORT, 0x5555aaaaU);
    const uint32_t ids[] = {0x00000001U, 0xFFFFFFFFU, 0x00000000U};
    hear(&writer, OWNER_ID, OWNER_PORT, ids, 3);
    EXPECT_EQ_U32(0xFFFFFFFFU, writer.segment_vouch_entity[OWNER_ID]); // the earliest modulo 2^32, not the least
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED, (int)segment_vouched(&writer, OWNER_ID, OWNER_PORT, base));
    EXPECT_EQ_INT((int)tt_SEGMENT_WRONG_OWNER, (int)segment_vouched(&writer, OWNER_ID, OWNER_PORT, BASE_THERE));
}

// A base of 0 is a base: the header carries it, and the peer's ids (0, 1, ...) vouch for it.
static void test_a_base_of_zero_is_the_incarnation(void) {
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context owner;
    init_context(&owner, OWNER_ID, OWNER_PORT, 0U);
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    EXPECT_EQ_U32(0, owner.own_segment != NULL ? owner.own_segment->incarnation : 1U);
    struct tt_Context writer;
    init_context(&writer, WRITER_ID, WRITER_PORT, 0x5555aaaaU);
    const uint32_t ids[] = {1U, 0U};
    hear(&writer, OWNER_ID, OWNER_PORT, ids, 2);
    EXPECT_TRUE(send_one(&writer, OWNER_ID, OWNER_PORT));
    EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_ATTACHED]);
    release_segments(&writer);
    release_segments(&owner);
    test_mock_segments_free();
}

// Attached legitimately; then an announce from that port lists ids of another base. The next revalidation gives the
// mapping up and the attach that follows refuses it. Without the vouch on revalidation the mapping would be kept.
static void test_a_revalidation_refuses_what_is_no_longer_vouched_for(void) {
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context owner;
    init_context(&owner, OWNER_ID, OWNER_PORT, BASE_HERE);
    create_own_segment(&owner);
    struct tt_Context writer;
    init_context(&writer, WRITER_ID, WRITER_PORT, 0x5555aaaaU);
    const uint32_t here[] = {BASE_HERE};
    hear(&writer, OWNER_ID, OWNER_PORT, here, 1);
    EXPECT_TRUE(send_one(&writer, OWNER_ID, OWNER_PORT));

    const uint32_t there[] = {BASE_THERE};
    hear(&writer, OWNER_ID, OWNER_PORT, there, 1);
    EXPECT_TRUE(send_one(&writer, OWNER_ID, OWNER_PORT)); // not due yet: the answer stands until the revalidation
    uint64_t now = 1;
    for (int tick = 0; tick < 3; tick++) {
        now += writer.opts.segment.revalidate_ns;
        segment_recheck_sweep(&writer, now);
    }
    EXPECT_TRUE(writer.segment_peers[OWNER_ID].recheck_due); // the treatment: it was asked
    EXPECT_TRUE(!send_one(&writer, OWNER_ID, OWNER_PORT));
    EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_WRONG_OWNER]);
    EXPECT_EQ_U32(0, writer.segment_revalidated);
    release_segments(&writer);
    release_segments(&owner);
    test_mock_segments_free();
}

// Ourselves: our own segment carries our own base, exactly; a stranger's file under our name does not.
static void test_a_context_sending_to_itself(void) {
    test_mock_reset();
    test_mock_segments_free();
    struct tt_Context self;
    init_context(&self, OWNER_ID, OWNER_PORT, BASE_HERE);
    EXPECT_TRUE(send_one(&self, OWNER_ID, OWNER_PORT)); // builds its segment and attaches it
    EXPECT_EQ_U32(1, self.segment_attach[tt_SEGMENT_ATTACHED]);
    release_segments(&self);
    test_mock_segments_free();

    struct tt_Context stranger;
    init_context(&stranger, OWNER_ID, OWNER_PORT, BASE_THERE);
    create_own_segment(&stranger);
    EXPECT_EQ_INT((int)tt_SEGMENT_WRONG_OWNER, (int)segment_vouched(&self, OWNER_ID, OWNER_PORT, BASE_THERE));
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED, (int)segment_vouched(&self, OWNER_ID, OWNER_PORT, BASE_HERE));
    release_segments(&stranger);
    test_mock_segments_free();
}
#endif

int main(void) {
#if tt_SEGMENT_ENABLED
    test_a_segment_another_namespace_owns_is_refused();
    test_a_peer_discovery_heard_is_attached();
    test_a_peer_not_heard_yet_is_attached_once_heard();
    test_the_vouch_span_at_its_edge();
    test_ids_that_wrapped_past_zero_vouch();
    test_a_base_of_zero_is_the_incarnation();
    test_a_revalidation_refuses_what_is_no_longer_vouched_for();
    test_a_context_sending_to_itself();
#endif
    if (test_result() != 0) {
        return 1;
    }
    printf("test_segment_vouch: all tests passed\n");
    return 0;
}
