/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: process_data()/check_liveliness()/count_peers()/peek_scheduler() are static.
// rmw_tickle/PLAN.md's Milestone 0(b) - the timeout-based counterpart to test_peer_discovery.c's
// content-change dedup tests: a node that stays silent (not just unchanged) has to be noticed.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define REMOTE_SUB_ENTITY_ID 0x22220001 // Phase 2 - which remote Subscriber entity acks
#define PUB_ENDPOINT_ID 0x11111111

static struct tt_Topic test_topic = {.name = "test_topic"};

static void init_node(struct tt_Context* node) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

static void init_publisher(struct tt_Publisher* pub, struct tt_Context* node) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = PUB_ENDPOINT_ID;
    pub->endpoint.name = "test_publisher";
    pub->topic = &test_topic;
    pub->node = node;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    node->endpoints[node->endpoint_count++] = (struct tt_Endpoint*)pub;
}

static void init_header(struct tt_Header* header, uint8_t source) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = source;
}

// Same shape as test_peer_discovery.c's own write_update_one_entity() - kept local rather than
// shared so this file doesn't take on a cross-file dependency for one small helper.
// The entity_id a test announce carries for a given endpoint_id - see write_update_one_entity().
#define TEST_ENTITY_ID_OF(endpoint_id) ((uint32_t)(endpoint_id) ^ 0xE1D00000U)

static uint32_t write_update_one_entity(uint8_t* buf, uint64_t last_modified, uint32_t endpoint_id, uint8_t kind,
                                        const char* type, const char* name) {
    struct test_announce* update_header = test_announce_at(buf);
    test_announce_set_last_modified(update_header, last_modified);
    update_header->announce.entity_count = 1;
    uint32_t tail = sizeof(struct test_announce);

    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(buf + tail);
    entity->endpoint_id = endpoint_id;
    // Deliberately NOT endpoint_id, and deliberately set at all: the real announce always carries
    // entity_id (Phase 2) and this helper used to leave whatever was in the buffer, which reached the
    // discovery cache as soon as upsert_discovered_entity() began recording it. Derived rather than
    // constant so a test can predict it, and distinct from endpoint_id so anything that confuses the
    // two fails instead of agreeing by accident.
    entity->entity_id = TEST_ENTITY_ID_OF(endpoint_id);
    entity->kind = kind;
    tail += sizeof(struct tt_UpdateEntity);

    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, type);
    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, name);
    return tail;
}

static void receive_update(struct tt_Context* node, uint64_t at_time, uint64_t last_modified) {
    test_mock_now = at_time;
    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node->rx_buffer, last_modified, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER,
                                            "topic", "sub");
    EXPECT_TRUE(process_data(node, &header, node->rx_buffer, 0, tail, 0xc0a80a02, 8282));
}

// Phase 3 follow-up (2026-09-23) - evidence of life is ANY validated packet from a node, not only
// its periodic UPDATE announce. These four build real packets and drive them through
// process_packet(), because that is where the evidence is recorded: one site above the per-type
// handlers, so it cannot drift and so a submessage type added later is covered for free.
//
// The bug this closes was not theoretical. rmw_tickle's perf comparison aborted both async runs
// with "Data consistency violated... Received sample id 1 Prev. sample id : 7427", each abort
// preceded by "Node N presumed dead" - a peer that was transmitting the whole time got forgotten
// because its announces were the packets that happened to be dropped, and its next announce then
// read as fresh discovery.
static uint32_t liveliness_write_packet(uint8_t* buf, uint8_t source, uint8_t submessage_type, uint16_t body_len) {
    struct tt_Header* header = (struct tt_Header*)buf;
    memset(buf, 0, sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + body_len);
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = source;

    struct tt_SubmessageHeader* submessage = (struct tt_SubmessageHeader*)(buf + sizeof(struct tt_Header));
    submessage->type = submessage_type;
    submessage->receiver = tt_SUBMESSAGE_ID_ALL;
    // tt_SubmessageHeader.length counts the header itself (process_one_submessage() derives
    // body_tail by subtracting it back off), so a body-sized value here would truncate the body.
    submessage->length = (uint16_t)(sizeof(struct tt_SubmessageHeader) + body_len);
    return (uint32_t)(sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + body_len);
}

// DATA from a node is proof it is alive. A Publisher streaming at full rate whose announces are
// the ones being dropped must not be declared dead.
static void test_data_refreshes_node_liveliness(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    receive_update(&node, 0, 100);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);

    // Far past the threshold, but DATA arrived just now.
    uint64_t late = tt_LIVELINESS_SILENCE_NS * 10;
    test_mock_now = late;
    uint8_t buf[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader)];
    uint32_t len =
        liveliness_write_packet(buf, REMOTE_NODE_ID, tt_SUBMESSAGE_TYPE_DATA, (uint16_t)sizeof(struct tt_DataHeader));
    EXPECT_TRUE(process_packet(&node, buf, 0, len, 0xc0a80a02, 8282, tt_TRANSPORT_UDP));

    check_liveliness(&node, late, NULL);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]); // still alive - it is plainly transmitting
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
}

// ACKNACK counts for the same reason, and matters for the opposite direction: a Subscriber that
// is acknowledging every gap is demonstrably alive even if its own announces are being lost.
static void test_acknack_refreshes_node_liveliness(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    receive_update(&node, 0, 100);

    uint64_t late = tt_LIVELINESS_SILENCE_NS * 10;
    test_mock_now = late;
    uint8_t buf[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_AckNackHeader)];
    uint32_t len = liveliness_write_packet(buf, REMOTE_NODE_ID, tt_SUBMESSAGE_TYPE_ACKNACK,
                                           (uint16_t)sizeof(struct tt_AckNackHeader));
    EXPECT_TRUE(process_packet(&node, buf, 0, len, 0xc0a80a02, 8282, tt_TRANSPORT_UDP));

    check_liveliness(&node, late, NULL);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
}

// A node is presumed dead tt_LIVELINESS_SILENCE_NS after its last sign of life of any kind, not after its
// last announce (rmw_tickle/LIVELINESS_PLAN.md rule 1, the user's decision of 2026-09-26). Until then the
// announce clock governed and traffic was only a veto, so detection could not be read against the last
// data sample the way DDS's is. Here the node goes quiet after one DATA packet an interval after its
// announce, and the limit runs from that packet.
static void test_node_limit_runs_from_the_last_packet(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    receive_update(&node, 0, 100); // announce clock starts at 0

    uint64_t last_packet_at = tt_CONTEXT_UPDATE_INTERVAL; // one DATA a full interval later
    test_mock_now = last_packet_at;
    uint8_t buf[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader)];
    uint32_t len =
        liveliness_write_packet(buf, REMOTE_NODE_ID, tt_SUBMESSAGE_TYPE_DATA, (uint16_t)sizeof(struct tt_DataHeader));
    EXPECT_TRUE(process_packet(&node, buf, 0, len, 0xc0a80a02, 8282, tt_TRANSPORT_UDP));

    // Past the limit from the announce, but not from the DATA: alive.
    check_liveliness(&node, tt_LIVELINESS_SILENCE_NS + 1, NULL);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);
    check_liveliness(&node, last_packet_at + tt_LIVELINESS_SILENCE_NS, NULL);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);

    // Just past it from the DATA: dead.
    check_liveliness(&node, last_packet_at + tt_LIVELINESS_SILENCE_NS + 1, NULL);
    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));
}

// A node's own packets must not extend its own lease - self_sent is excluded, so a node talking to
// itself (the co-located client/service topology rmw_tickle allows) can't keep a stale entry for
// its own id alive.
static void test_self_sent_packet_does_not_refresh(void) {
    struct tt_Context node;
    init_node(&node);

    node.update_seen[LOCAL_NODE_ID] = true;
    node.update_last_seen[LOCAL_NODE_ID] = 0;

    uint64_t late = tt_LIVELINESS_SILENCE_NS * 10;
    test_mock_now = late;
    uint8_t buf[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader)];
    uint32_t len =
        liveliness_write_packet(buf, LOCAL_NODE_ID, tt_SUBMESSAGE_TYPE_DATA, (uint16_t)sizeof(struct tt_DataHeader));
    EXPECT_TRUE(process_packet(&node, buf, 0, len, 0xc0a80a02, 8282, tt_TRANSPORT_UDP));

    EXPECT_EQ_U32(0, (uint32_t)node.update_last_seen[LOCAL_NODE_ID]);
}

// A node heard from once, then never again: once tt_LIVELINESS_SILENCE_NS passes with no
// announce at all, it must be forgotten - peer-table entries dropped and update_
// seen[] cleared so a later announce from the same id is treated as first contact again.
static void test_expires_peer_after_missed_intervals(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    receive_update(&node, 0, 100);
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);

    uint64_t past_threshold = tt_LIVELINESS_SILENCE_NS + 1;
    check_liveliness(&node, past_threshold, NULL);

    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(0, node.update_generation[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));
}

// Before the threshold is reached, the peer must still be considered live.
static void test_does_not_expire_before_threshold(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    receive_update(&node, 0, 100);

    uint64_t before_threshold = tt_LIVELINESS_SILENCE_NS - 1;
    check_liveliness(&node, before_threshold, NULL);

    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
}

// The critical regression case: a *repeated, content-unchanged* announce (process_data()'s own
// dedup path - see its early return) must still push update_last_seen[] forward, exactly like a
// changed one would. Without that, a perfectly healthy node whose endpoints never change would
// get falsely expired the first time check_liveliness() ran after tt_LIVELINESS_MISS_THRESHOLD
// intervals, even though it kept announcing the whole time.
static void test_repeated_unchanged_update_resets_liveliness_timer(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    uint64_t interval = tt_CONTEXT_UPDATE_INTERVAL;
    receive_update(&node, 0, 100);

    // Re-announce the *same* content (last_modified unchanged) just before the threshold would
    // otherwise expire the peer.
    uint64_t just_before_threshold = (tt_LIVELINESS_MISS_THRESHOLD * interval) - 1;
    receive_update(&node, just_before_threshold, 100);

    // Now check at a time that's past the threshold *from the first announce*, but not from the
    // second (repeated) one.
    uint64_t checked_at = just_before_threshold + interval;
    check_liveliness(&node, checked_at, NULL);

    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
}

// A node id never heard from at all must not be touched (no false log/expire on an all-zero
// update_last_seen[] entry that was simply never populated).
static void test_never_seen_node_id_is_not_flagged(void) {
    struct tt_Context node;
    init_node(&node);

    check_liveliness(&node, tt_LIVELINESS_MISS_THRESHOLD * tt_CONTEXT_UPDATE_INTERVAL * 100, NULL);

    for (int i = 0; i < tt_MAX_CONTEXT_IDS; i++) {
        EXPECT_TRUE(!node.update_seen[i]);
    }
}

// check_liveliness() must re-arm itself, the same self-rescheduling pattern node_update() uses -
// otherwise liveliness checking would silently run exactly once per node lifetime.
static void test_reschedules_itself(void) {
    struct tt_Context node;
    init_node(&node);

    check_liveliness(&node, 1000, NULL);

    struct tt_TCB* tcb = peek_scheduler(&node);
    EXPECT_TRUE(tcb != NULL);
    EXPECT_TRUE(tcb->function == check_liveliness);
    EXPECT_EQ_U32((uint32_t)(1000 + tt_CONTEXT_UPDATE_INTERVAL), (uint32_t)tcb->time);
}

// Milestone 62 (rmw_tickle/PLAN.md) - tt_Context_entity_alive()'s own zero-lease branch: an entity
// that never requested a specific liveliness_lease_duration_ns must defer entirely to its own
// .alive field (whatever the coarser, node-level check_liveliness() sweep last set it to) -
// verbatim, in both directions, regardless of what update_last_seen[] says.
static void test_entity_alive_with_zero_lease_defers_to_alive_flag(void) {
    struct tt_Context node;
    init_node(&node);

    struct tt_DiscoveredEntity entity = {0};
    entity.context_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 0;
    entity.alive = true;
    EXPECT_TRUE(tt_Context_entity_alive(&node, &entity, 1000));

    entity.alive = false;
    EXPECT_TRUE(!tt_Context_entity_alive(&node, &entity, 1000));
}

// An entity that *did* request a specific lease gets a freshly-computed answer instead, checked
// right at the boundary in both directions - within the lease is alive, one nanosecond past it
// is not, independent of the coarser sweep's own ~3s cadence.
static void test_entity_alive_with_lease_computed_fresh_at_boundary(void) {
    struct tt_Context node;
    init_node(&node);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 1000;

    struct tt_DiscoveredEntity entity = {0};
    entity.context_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 500;
    entity.alive = true; // deliberately irrelevant here - a non-zero lease ignores this field

    EXPECT_TRUE(tt_Context_entity_alive(&node, &entity, 1000));  // exactly at last_seen
    EXPECT_TRUE(tt_Context_entity_alive(&node, &entity, 1500));  // exactly at the lease boundary
    EXPECT_TRUE(!tt_Context_entity_alive(&node, &entity, 1501)); // one ns past it
}

// An AUTOMATIC entity's lease runs from the last datagram of its node, whatever it was (LIVELINESS_PLAN.md
// rule 1): an entity whose node is still sending DATA is alive although its announces stopped long ago,
// and it expires exactly one lease after that last packet. Until 2026-09-26 the lease ran from the last
// announce and traffic only held off the verdict for half a lease, so detection landed at either of two
// points (the HIL's bimodal ~500 ms) and could not be compared with DDS, whose lease runs from the data.
static void test_entity_lease_runs_from_the_last_packet(void) {
    struct tt_Context node;
    init_node(&node);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 1000;  // announces stopped long ago
    node.traffic_last_seen[REMOTE_NODE_ID] = 9000; // ...but DATA is still arriving

    struct tt_DiscoveredEntity entity = {0};
    entity.context_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 500;
    entity.alive = true;

    EXPECT_TRUE(tt_Context_entity_alive(&node, &entity, 9000));  // far past the announce lease
    EXPECT_TRUE(tt_Context_entity_alive(&node, &entity, 9500));  // one lease after the last packet
    EXPECT_TRUE(!tt_Context_entity_alive(&node, &entity, 9501)); // and not a nanosecond more
}

// With every clock stopping together - what a killed process does - detection lands exactly one lease
// after that moment.
static void test_entity_alive_traffic_does_not_delay_expiry(void) {
    struct tt_Context node;
    init_node(&node);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 1000;
    node.traffic_last_seen[REMOTE_NODE_ID] = 1000; // both stopped at the same instant

    struct tt_DiscoveredEntity entity = {0};
    entity.context_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 500;
    entity.alive = true;

    EXPECT_TRUE(tt_Context_entity_alive(&node, &entity, 1500));  // at the lease boundary
    EXPECT_TRUE(!tt_Context_entity_alive(&node, &entity, 1501)); // one ns past it - not one ns later
}

// The real point of this milestone: a short-lease entity whose lease has genuinely expired must
// report not-alive even while .alive still (incorrectly, from this entity's own specific lease's
// point of view) says true, because the slower ~3s node-level sweep hasn't caught up yet - this
// function is authoritative for a leased entity, not the periodic sweep's own timing.
static void test_entity_alive_with_lease_ignores_stale_true_alive_flag(void) {
    struct tt_Context node;
    init_node(&node);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 0;

    struct tt_DiscoveredEntity entity = {0};
    entity.context_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 100;
    entity.alive = true; // the coarse sweep (fixed ~3s window) hasn't run yet

    EXPECT_TRUE(!tt_Context_entity_alive(&node, &entity, 1000)); // far past its own 100ns lease
}

// A node id never heard from at all (update_seen[] still false) must report not-alive for a
// leased entity, not crash or fall through to some stale default.
static void test_entity_alive_never_seen_node_returns_false(void) {
    struct tt_Context node;
    init_node(&node);

    struct tt_DiscoveredEntity entity = {0};
    entity.context_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 500;
    entity.alive = true;

    EXPECT_TRUE(!tt_Context_entity_alive(&node, &entity, 1000));
}

// An empty/never-populated discovery slot (node_id == tt_CONTEXT_ID_INVALID, this struct's own
// zero-init default) must report not-alive - a safe, harmless no-op, not a crash.
static void test_entity_alive_invalid_node_id_returns_false(void) {
    struct tt_Context node;
    init_node(&node);

    struct tt_DiscoveredEntity entity = {0}; // node_id stays tt_CONTEXT_ID_INVALID
    EXPECT_TRUE(!tt_Context_entity_alive(&node, &entity, 1000));
}

// Phase 3 prerequisite (a), rmw_tickle/PLAN.md - when a remote Subscriber's own announced
// liveliness lease expires, check_liveliness() must also drop it from the matching
// local Publisher's peer and ack sets, not just mark the discovery entry departed. Before this, a
// crashed Subscriber kept its ack entry until check_liveliness()'s own node-level sweep (~3-3.6s,
// and only if the whole node went quiet) - long enough to stall a Phase 3 KEEP_ALL writer waiting
// on that exact ack.
static void test_lease_expiry_drops_subscriber_from_publisher_ack_set(void) {
    struct tt_Context node;
    struct tt_Publisher pub;
    init_node(&node);
    init_publisher(&pub, &node);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    struct tt_DiscoveredEntity* entities = discovery.entities;
    node.discovery = &discovery;

    // The remote Subscriber: matched as a peer, has acked up to 7, and announced a 100ns lease.
    pub.peers[0].context_id = REMOTE_NODE_ID;
    pub.peers[0].ip = 0x0A000001;
    pub.peers[0].port = 7447;
    claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    record_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID, 7);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 0;

    entities[0].context_id = REMOTE_NODE_ID;
    entities[0].endpoint_id = PUB_ENDPOINT_ID; // the Subscriber matching this Publisher's own id
    entities[0].kind = tt_KIND_TOPIC_SUBSCRIBER;
    entities[0].liveliness_lease_duration_ns = 100;
    entities[0].alive = true;
    tt_Discovery_reindex(&discovery); // the key was written directly (struct tt_Discovery.index)

    // Well within the lease: nothing changes.
    check_liveliness(&node, 50, NULL);
    EXPECT_TRUE(entities[0].alive);
    EXPECT_EQ_INT((int)REMOTE_NODE_ID, (int)pub.peers[0].context_id);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) != NULL);

    // Past its own lease: departed, and out of both the peer set and the ack set.
    check_liveliness(&node, 1000, NULL);
    EXPECT_TRUE(!entities[0].alive);
    EXPECT_EQ_INT((int)tt_CONTEXT_ID_INVALID, (int)pub.peers[0].context_id);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) == NULL);
}

// ...but only for the Publisher it actually matched: a lease-expired Subscriber of some *other*
// topic must leave this Publisher's own peer/ack state alone.
static void test_lease_expiry_leaves_unrelated_publisher_alone(void) {
    struct tt_Context node;
    struct tt_Publisher pub;
    init_node(&node);
    init_publisher(&pub, &node);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    struct tt_DiscoveredEntity* entities = discovery.entities;
    node.discovery = &discovery;

    pub.peers[0].context_id = REMOTE_NODE_ID;
    claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    record_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID, 7);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 0;

    entities[0].context_id = REMOTE_NODE_ID;
    entities[0].endpoint_id = PUB_ENDPOINT_ID + 1; // a different topic's Subscriber
    entities[0].kind = tt_KIND_TOPIC_SUBSCRIBER;
    entities[0].liveliness_lease_duration_ns = 100;
    entities[0].alive = true;
    tt_Discovery_reindex(&discovery); // the key was written directly (struct tt_Discovery.index)

    check_liveliness(&node, 1000, NULL);
    EXPECT_TRUE(!entities[0].alive);                                  // still tombstoned
    EXPECT_EQ_INT((int)REMOTE_NODE_ID, (int)pub.peers[0].context_id); // but this Publisher is untouched
    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    EXPECT_EQ_U32(7, ack->ack_seq_no);
}

// Phase 3 step 2 (rmw_tickle/PLAN.md) - the Subscriber-side mirror of the lease cleanup: a remote
// *Publisher* past its own announced lease loses its WriterProxy, which is the only thing that ends
// gap recovery for a writer that died. Without it, a KEEP_ALL writer (which switches off the
// Subscriber's ACKNACK give-up) that vanished mid-gap would have its Subscriber re-requesting the
// same samples every retry interval forever, at an address nobody answers.
static void test_lease_expiry_drops_writer_proxy_on_subscriber(void) {
    struct tt_Context node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node(&node);
    memset(&topic, 0, sizeof(topic));
    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = PUB_ENDPOINT_ID; // the topic this remote writer publishes on
    sub.endpoint.name = "test_subscriber";
    sub.node = &node;
    sub.topic = &topic;
    sub.reliable = true;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&sub;

    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    struct tt_DiscoveredEntity* entities = discovery.entities;
    node.discovery = &discovery;

    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(proxy != NULL);
    proxy->keep_all = tt_WRITER_KEEP_ALL_YES; // no give-up: only liveliness can end this
    proxy->ack_seq_no = 7;
    bitmap_set_bit(proxy->received_bitmap, 3); // a gap it is still chasing
    proxy->acknack_scheduled = true;
    EXPECT_TRUE(tt_Context_schedule(&node, 1000, acknack_retry, proxy));

    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 0;
    entities[0].context_id = REMOTE_NODE_ID;
    entities[0].endpoint_id = PUB_ENDPOINT_ID;
    entities[0].kind = tt_KIND_TOPIC_PUBLISHER;
    entities[0].liveliness_lease_duration_ns = 100;
    entities[0].alive = true;
    tt_Discovery_reindex(&discovery); // the key was written directly (struct tt_Discovery.index)

    check_liveliness(&node, 50, NULL); // still inside its lease
    EXPECT_EQ_INT((int)REMOTE_NODE_ID, (int)proxy->context_id);

    check_liveliness(&node, 1000, NULL);                               // past it
    EXPECT_EQ_INT((int)tt_CONTEXT_ID_INVALID, (int)proxy->context_id); // slot freed
    EXPECT_TRUE(!proxy->acknack_scheduled);                            // and its retry cancelled
    EXPECT_TRUE(find_writer_proxy(&sub, REMOTE_NODE_ID, 0) == NULL);

    // Plan's addition 2: a restarted Publisher reusing the slot must go through first contact
    // again - its seq_no starts back at 1, so a carried-over ack_seq_no of 7 would ignore
    // everything it sends until it caught up.
    struct tt_WriterProxy* restarted = find_or_create_writer_proxy(&sub, REMOTE_NODE_ID, 0, NULL);
    EXPECT_TRUE(restarted != NULL);
    EXPECT_EQ_U32(1, restarted->ack_seq_no); // not the departed writer's 7
    EXPECT_TRUE(bitmap_is_zero(restarted->received_bitmap, proxy_words(restarted)));
    EXPECT_TRUE(restarted->keep_all != tt_WRITER_KEEP_ALL_YES); // re-learned from the new announce, not inherited
}

// A peer whose datagrams are sitting unread in our socket buffer was NOT silent - we had not looked.
//
// This is the CI failure of 2026-09-30 (run 36787898559, Check all on 45d64060): on a loaded GitHub
// runner both nodes declared EACH OTHER dead, 3.5 s apart, while the publisher published 100 samples
// and delivery stopped at 5 - on a commit that changed no code at all.
//
// The mechanism is ORDERING, not a threshold. poll_once_nonblocking() is:
//
//     while (run_due_entry(node, time, &has_next, &next)) { }   // every due entry, this check among them
//     ...
//     len = tt_try_receive(...);                                 // the socket, only afterwards
//
// So a process descheduled for longer than tt_LIVELINESS_SILENCE_NS wakes with its own clock far
// advanced AND its peers' datagrams queued, and judges the whole gap as peer silence before reading
// one of them. Both sides were starved, so both did it. Raising the limit does not fix this - it only
// changes how much starvation is required, and a shared runner can always supply more.
//
// Every other liveliness test in this file calls process_packet() and THEN check_liveliness(), so they
// all prove that evidence ALREADY READ refreshes the clock. None covers evidence that has arrived and
// not been read, which is the only case that fails.
static void test_a_peer_with_datagrams_waiting_unread_is_not_dead(void) {
    // Arm 1: starved observer, the peer's DATA waiting unread. It must survive.
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);
    receive_update(&node, 0, 100);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);

    const uint64_t starved = tt_LIVELINESS_SILENCE_NS * 3;
    test_mock_now = starved;
    // The peer's datagram, left in the buffer the mock hands back and NOT processed - which is what an
    // unread socket looks like. tt_try_receive() leaves the buffer as it is and returns the length.
    uint32_t pending_len = liveliness_write_packet(node.rx_buffer, REMOTE_NODE_ID, tt_SUBMESSAGE_TYPE_DATA,
                                                   (uint16_t)sizeof(struct tt_DataHeader));
    test_mock_try_receive_len = (int32_t)pending_len;
    test_mock_try_receive_remaining = 1;
    // Armed by hand: schedule_periodic_tasks() is part of opening a node and this whitebox context
    // never opened one, so without this the poll has no due entry, runs nothing, and every arm below
    // passes for the wrong reason. The control in arm 2 caught exactly that.
    arm_liveliness_check(&node, test_mock_now);

    (void)tt_Context_poll(&node, 0);

    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]); // it was transmitting the whole time
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    test_mock_try_receive_remaining = 0;

    // Arm 2, the control that must still die: the same silence with NOTHING waiting is a real death.
    struct tt_Context quiet;
    init_node(&quiet);
    struct tt_Publisher quiet_pub;
    init_publisher(&quiet_pub, &quiet);
    receive_update(&quiet, 0, 100);
    EXPECT_TRUE(quiet.update_seen[REMOTE_NODE_ID]);
    test_mock_now = starved;
    test_mock_try_receive_remaining = 0;
    arm_liveliness_check(&quiet, test_mock_now);
    (void)tt_Context_poll(&quiet, 0);
    EXPECT_TRUE(!quiet.update_seen[REMOTE_NODE_ID]); // nothing to hear, so presumed gone - correct

    // Arm 3, the ordering already covered: read first, then check, and the peer survives. Kept so the
    // two orderings sit side by side - arm 1 differs from this one only in when the datagram is read.
    struct tt_Context read_first;
    init_node(&read_first);
    struct tt_Publisher read_first_pub;
    init_publisher(&read_first_pub, &read_first);
    receive_update(&read_first, 0, 100);
    test_mock_now = starved;
    uint8_t buf[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader)];
    uint32_t len =
        liveliness_write_packet(buf, REMOTE_NODE_ID, tt_SUBMESSAGE_TYPE_DATA, (uint16_t)sizeof(struct tt_DataHeader));
    EXPECT_TRUE(process_packet(&read_first, buf, 0, len, 0xc0a80a02, 8282, tt_TRANSPORT_UDP));
    arm_liveliness_check(&read_first, test_mock_now);
    (void)tt_Context_poll(&read_first, 0);
    EXPECT_TRUE(read_first.update_seen[REMOTE_NODE_ID]);
}

// The cap on that deferral, which is the half that can silently not exist. "Defer while anything is
// unread" with no bound lets a socket that is never empty postpone a real death for ever - and a
// saturated node is where one is most likely.
//
// This drives check_liveliness() DIRECTLY rather than through tt_Context_poll(), and that is the whole
// point of it. The first version of this arm went through the poll with a 16-datagram backlog and
// passed - but drain_rx() empties the queue in one call, so the queue was empty from the second pass
// on, the cap was never reached, and removing the cap altogether changed nothing. It measured the
// backlog draining, not the bound. A mutant that deleted the cap survived it.
//
// Calling the check directly is what keeps rx_buffered() above zero across every round, so the cap is
// the only thing that can end the deferral. Worth knowing that in real use the cap is rarely reached
// for exactly the reason that broke the first attempt: the next receive pass empties the queue.
static void test_a_busy_socket_cannot_postpone_a_real_death_for_ever(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);
    receive_update(&node, 0, 100);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);

    test_mock_now = tt_LIVELINESS_SILENCE_NS * 3;
    // Buffered and never drained, because nothing here reads the socket.
    test_mock_try_receive_remaining = 1;
    EXPECT_TRUE(tt_rx_buffered(&node) > 0);

    for (int i = 0; i < tt_LIVELINESS_MAX_DEFERRALS; i++) {
        check_liveliness(&node, test_mock_now, NULL);
        EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]); // deferred, not judged, every round up to the cap
    }
    EXPECT_EQ_U32(tt_LIVELINESS_MAX_DEFERRALS, (uint32_t)node.liveliness_deferrals);
    EXPECT_TRUE(tt_rx_buffered(&node) > 0); // still busy: only the cap can end this

    check_liveliness(&node, test_mock_now, NULL); // one past the cap

    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]); // silent throughout, and the queue was never its
    EXPECT_TRUE(node.liveliness_deferrals_total >= (uint64_t)tt_LIVELINESS_MAX_DEFERRALS);
    // The counter resets the moment a run judges, so the NEXT starved episode gets its deferrals too.
    // Without this the fix works once per process and then never again, which no other assertion here
    // would notice - a mutant that deleted the reset survived until this line existed.
    EXPECT_EQ_U32(0, (uint32_t)node.liveliness_deferrals);
    test_mock_try_receive_remaining = 0;
}

// A node that was not running cannot have heard anyone, so the window it spent descheduled is not
// evidence of their silence.
//
// This is the CI failure of 2026-09-30 and again of 2026-10-01 (runs 36787898559, 36834931043), on
// commits that changed no code: on a loaded GitHub runner both nodes declared EACH OTHER dead 3.5 s
// apart while 100 samples were published, and delivery stopped at 5.
//
// The first attempt at this deferred while tt_rx_buffered() was non-zero and was a no-op in
// production - on Linux that counts datagrams already pulled into our own batch, not what the kernel
// holds, so it is zero exactly on a starved wake-up. It passed its tests only because the mock gives
// that function the other meaning. The lateness below needs no HAL query at all: the scheduler entry
// knows when it was due.
static void test_a_descheduled_node_does_not_blame_its_peers(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);
    receive_update(&node, 0, 100);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);

    // Armed for a moment well inside the limit, then not run until long past it: the gap is ours.
    const uint64_t armed_for = tt_LIVELINESS_SILENCE_NS / 4;
    arm_liveliness_check(&node, armed_for);
    test_mock_now = tt_LIVELINESS_SILENCE_NS * 3;
    check_liveliness(&node, test_mock_now, NULL);

    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]); // we were not listening; that is not their silence
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
}

// The control that makes the one above mean something: a HEALTHY run must not measurably widen. A
// guard that forgives every run forgives the real deaths too, and "widened correctly" and "widened
// always" look identical from the surviving peer alone.
static void test_an_ordinary_run_does_not_widen_the_limit(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);
    receive_update(&node, 0, 100);

    // Due just past the limit and running 1 us late, which is what an ordinary scheduler looks like.
    // The peer has been silent the whole time and must be given up.
    const uint64_t due = tt_LIVELINESS_SILENCE_NS + 1;
    arm_liveliness_check(&node, due);
    test_mock_now = due + tt_MICROSECOND;
    check_liveliness(&node, test_mock_now, NULL);

    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]); // ordinary lateness forgives nothing
}

// And the allowance is granted once rather than compounding: after a stall, the NEXT run is late only
// by its own lateness, so a peer that really has gone is still given up one ordinary interval later.
// Without this a single stall would make a node permanently unable to declare anyone dead.
static void test_the_allowance_does_not_carry_into_the_next_run(void) {
    struct tt_Context node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);
    receive_update(&node, 0, 100);

    arm_liveliness_check(&node, tt_LIVELINESS_SILENCE_NS / 4);
    test_mock_now = tt_LIVELINESS_SILENCE_NS * 3;
    check_liveliness(&node, test_mock_now, NULL);
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]); // forgiven once

    // The stall is over. This run is armed and punctual, and the peer has still said nothing.
    uint64_t const armed = node.liveliness_check_ns;
    test_mock_now = armed + tt_MICROSECOND;
    check_liveliness(&node, test_mock_now, NULL);
    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]); // and now it is a real death
}

int main(void) {
    test_lease_expiry_drops_writer_proxy_on_subscriber();
    test_lease_expiry_drops_subscriber_from_publisher_ack_set();
    test_lease_expiry_leaves_unrelated_publisher_alone();
    test_mock_reset();
    test_data_refreshes_node_liveliness();
    test_acknack_refreshes_node_liveliness();
    test_node_limit_runs_from_the_last_packet();
    test_self_sent_packet_does_not_refresh();
    test_expires_peer_after_missed_intervals();
    test_mock_reset();
    test_a_descheduled_node_does_not_blame_its_peers();
    test_mock_reset();
    test_an_ordinary_run_does_not_widen_the_limit();
    test_mock_reset();
    test_the_allowance_does_not_carry_into_the_next_run();
    test_mock_reset();
    test_a_peer_with_datagrams_waiting_unread_is_not_dead();
    test_mock_reset();
    test_a_busy_socket_cannot_postpone_a_real_death_for_ever();
    test_mock_reset();
    test_does_not_expire_before_threshold();
    test_mock_reset();
    test_repeated_unchanged_update_resets_liveliness_timer();
    test_mock_reset();
    test_never_seen_node_id_is_not_flagged();
    test_mock_reset();
    test_reschedules_itself();
    test_mock_reset();
    test_entity_alive_with_zero_lease_defers_to_alive_flag();
    test_mock_reset();
    test_entity_alive_with_lease_computed_fresh_at_boundary();
    test_entity_lease_runs_from_the_last_packet();
    test_entity_alive_traffic_does_not_delay_expiry();
    test_mock_reset();
    test_entity_alive_with_lease_ignores_stale_true_alive_flag();
    test_mock_reset();
    test_entity_alive_never_seen_node_returns_false();
    test_mock_reset();
    test_entity_alive_invalid_node_id_returns_false();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_liveliness: all tests passed\n");
    return 0;
}
