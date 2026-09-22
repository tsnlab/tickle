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

// Whitebox: process_update()/check_liveliness()/count_peers()/peek_scheduler() are static.
// rmw_tickle/PLAN.md's Milestone 0(b) - the timeout-based counterpart to test_peer_discovery.c's
// content-change dedup tests: a node that stays silent (not just unchanged) has to be noticed.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define REMOTE_SUB_ENTITY_ID 0x22220001 // Phase 2 - which remote Subscriber entity acks
#define PUB_ENDPOINT_ID 0x11111111

static struct tt_Topic test_topic = {.name = "test_topic"};

static void init_node(struct tt_Node* node) {
    memset(node, 0, sizeof(*node));
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

static void init_publisher(struct tt_Publisher* pub, struct tt_Node* node) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = PUB_ENDPOINT_ID;
    pub->endpoint.name = "test_publisher";
    pub->topic = &test_topic;
    pub->node = node;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].node_id = tt_NODE_ID_INVALID;
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
static uint32_t write_update_one_entity(uint8_t* buf, uint64_t last_modified, uint32_t endpoint_id, uint8_t kind,
                                        const char* type, const char* name) {
    struct tt_UpdateHeader* update_header = (struct tt_UpdateHeader*)buf;
    update_header->last_modified = last_modified;
    update_header->entity_count = 1;
    uint32_t tail = sizeof(struct tt_UpdateHeader);

    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(buf + tail);
    entity->endpoint_id = endpoint_id;
    entity->kind = kind;
    tail += sizeof(struct tt_UpdateEntity);

    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, type);
    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, name);
    return tail;
}

static void receive_update(struct tt_Node* node, uint64_t at_time, uint64_t last_modified) {
    test_mock_now = at_time;
    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node->rx_buffer, last_modified, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER,
                                            "topic", "sub");
    EXPECT_TRUE(process_update(node, &header, node->rx_buffer, 0, tail, 0xc0a80a02, 8282));
}

// A node heard from once, then never again: once tt_LIVELINESS_MISS_THRESHOLD full intervals
// pass with no announce at all, it must be forgotten - peer-table entries dropped and update_
// seen[] cleared so a later announce from the same id is treated as first contact again.
static void test_expires_peer_after_missed_intervals(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    receive_update(&node, 0, 100);
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);

    uint64_t past_threshold = (tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL) + 1;
    check_liveliness(&node, past_threshold, NULL);

    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(0, (uint32_t)node.update_last_modified[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));
}

// Before the threshold is reached, the peer must still be considered live.
static void test_does_not_expire_before_threshold(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    receive_update(&node, 0, 100);

    uint64_t before_threshold = (tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL) - 1;
    check_liveliness(&node, before_threshold, NULL);

    EXPECT_TRUE(node.update_seen[REMOTE_NODE_ID]);
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
}

// The critical regression case: a *repeated, content-unchanged* announce (process_update()'s own
// dedup path - see its early return) must still push update_last_seen[] forward, exactly like a
// changed one would. Without that, a perfectly healthy node whose endpoints never change would
// get falsely expired the first time check_liveliness() ran after tt_LIVELINESS_MISS_THRESHOLD
// intervals, even though it kept announcing the whole time.
static void test_repeated_unchanged_update_resets_liveliness_timer(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    uint64_t interval = tt_NODE_UPDATE_INTERVAL;
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
    struct tt_Node node;
    init_node(&node);

    check_liveliness(&node, tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL * 100, NULL);

    for (int i = 0; i < tt_MAX_ENDPOINT_COUNT; i++) {
        EXPECT_TRUE(!node.update_seen[i]);
    }
}

// check_liveliness() must re-arm itself, the same self-rescheduling pattern node_update() uses -
// otherwise liveliness checking would silently run exactly once per node lifetime.
static void test_reschedules_itself(void) {
    struct tt_Node node;
    init_node(&node);

    check_liveliness(&node, 1000, NULL);

    struct tt_TCB* tcb = peek_scheduler(&node);
    EXPECT_TRUE(tcb != NULL);
    EXPECT_TRUE(tcb->function == check_liveliness);
    EXPECT_EQ_U32((uint32_t)(1000 + tt_NODE_UPDATE_INTERVAL), (uint32_t)tcb->time);
}

// Milestone 62 (rmw_tickle/PLAN.md) - tt_Node_entity_alive()'s own zero-lease branch: an entity
// that never requested a specific liveliness_lease_duration_ns must defer entirely to its own
// .alive field (whatever the coarser, node-level check_liveliness() sweep last set it to) -
// verbatim, in both directions, regardless of what update_last_seen[] says.
static void test_entity_alive_with_zero_lease_defers_to_alive_flag(void) {
    struct tt_Node node;
    init_node(&node);

    struct tt_DiscoveredEntity entity = {0};
    entity.node_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 0;
    entity.alive = true;
    EXPECT_TRUE(tt_Node_entity_alive(&node, &entity, 1000));

    entity.alive = false;
    EXPECT_TRUE(!tt_Node_entity_alive(&node, &entity, 1000));
}

// An entity that *did* request a specific lease gets a freshly-computed answer instead, checked
// right at the boundary in both directions - within the lease is alive, one nanosecond past it
// is not, independent of the coarser sweep's own ~3s cadence.
static void test_entity_alive_with_lease_computed_fresh_at_boundary(void) {
    struct tt_Node node;
    init_node(&node);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 1000;

    struct tt_DiscoveredEntity entity = {0};
    entity.node_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 500;
    entity.alive = true; // deliberately irrelevant here - a non-zero lease ignores this field

    EXPECT_TRUE(tt_Node_entity_alive(&node, &entity, 1000));  // exactly at last_seen
    EXPECT_TRUE(tt_Node_entity_alive(&node, &entity, 1500));  // exactly at the lease boundary
    EXPECT_TRUE(!tt_Node_entity_alive(&node, &entity, 1501)); // one ns past it
}

// The real point of this milestone: a short-lease entity whose lease has genuinely expired must
// report not-alive even while .alive still (incorrectly, from this entity's own specific lease's
// point of view) says true, because the slower ~3s node-level sweep hasn't caught up yet - this
// function is authoritative for a leased entity, not the periodic sweep's own timing.
static void test_entity_alive_with_lease_ignores_stale_true_alive_flag(void) {
    struct tt_Node node;
    init_node(&node);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 0;

    struct tt_DiscoveredEntity entity = {0};
    entity.node_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 100;
    entity.alive = true; // the coarse sweep (fixed ~3s window) hasn't run yet

    EXPECT_TRUE(!tt_Node_entity_alive(&node, &entity, 1000)); // far past its own 100ns lease
}

// A node id never heard from at all (update_seen[] still false) must report not-alive for a
// leased entity, not crash or fall through to some stale default.
static void test_entity_alive_never_seen_node_returns_false(void) {
    struct tt_Node node;
    init_node(&node);

    struct tt_DiscoveredEntity entity = {0};
    entity.node_id = REMOTE_NODE_ID;
    entity.liveliness_lease_duration_ns = 500;
    entity.alive = true;

    EXPECT_TRUE(!tt_Node_entity_alive(&node, &entity, 1000));
}

// An empty/never-populated discovery slot (node_id == tt_NODE_ID_INVALID, this struct's own
// zero-init default) must report not-alive - a safe, harmless no-op, not a crash.
static void test_entity_alive_invalid_node_id_returns_false(void) {
    struct tt_Node node;
    init_node(&node);

    struct tt_DiscoveredEntity entity = {0}; // node_id stays tt_NODE_ID_INVALID
    EXPECT_TRUE(!tt_Node_entity_alive(&node, &entity, 1000));
}

// Phase 3 prerequisite (a), rmw_tickle/PLAN.md - when a remote Subscriber's own announced
// liveliness lease expires, tombstone_entities_past_own_lease() must also drop it from the matching
// local Publisher's peer and ack sets, not just mark the discovery entry departed. Before this, a
// crashed Subscriber kept its ack entry until check_liveliness()'s own node-level sweep (~3-3.6s,
// and only if the whole node went quiet) - long enough to stall a Phase 3 KEEP_ALL writer waiting
// on that exact ack.
static void test_lease_expiry_drops_subscriber_from_publisher_ack_set(void) {
    struct tt_Node node;
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
    pub.peers[0].node_id = REMOTE_NODE_ID;
    pub.peers[0].ip = 0x0A000001;
    pub.peers[0].port = 7447;
    claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    record_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID, 7);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 0;

    entities[0].node_id = REMOTE_NODE_ID;
    entities[0].endpoint_id = PUB_ENDPOINT_ID; // the Subscriber matching this Publisher's own id
    entities[0].kind = tt_KIND_TOPIC_SUBSCRIBER;
    entities[0].liveliness_lease_duration_ns = 100;
    entities[0].alive = true;

    // Well within the lease: nothing changes.
    tombstone_entities_past_own_lease(&node, 50);
    EXPECT_TRUE(entities[0].alive);
    EXPECT_EQ_INT((int)REMOTE_NODE_ID, (int)pub.peers[0].node_id);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) != NULL);

    // Past its own lease: departed, and out of both the peer set and the ack set.
    tombstone_entities_past_own_lease(&node, 1000);
    EXPECT_TRUE(!entities[0].alive);
    EXPECT_EQ_INT((int)tt_NODE_ID_INVALID, (int)pub.peers[0].node_id);
    EXPECT_TRUE(find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID) == NULL);
}

// ...but only for the Publisher it actually matched: a lease-expired Subscriber of some *other*
// topic must leave this Publisher's own peer/ack state alone.
static void test_lease_expiry_leaves_unrelated_publisher_alone(void) {
    struct tt_Node node;
    struct tt_Publisher pub;
    init_node(&node);
    init_publisher(&pub, &node);
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    struct tt_DiscoveredEntity* entities = discovery.entities;
    node.discovery = &discovery;

    pub.peers[0].node_id = REMOTE_NODE_ID;
    claim_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    record_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID, 7);
    node.update_seen[REMOTE_NODE_ID] = true;
    node.update_last_seen[REMOTE_NODE_ID] = 0;

    entities[0].node_id = REMOTE_NODE_ID;
    entities[0].endpoint_id = PUB_ENDPOINT_ID + 1; // a different topic's Subscriber
    entities[0].kind = tt_KIND_TOPIC_SUBSCRIBER;
    entities[0].liveliness_lease_duration_ns = 100;
    entities[0].alive = true;

    tombstone_entities_past_own_lease(&node, 1000);
    EXPECT_TRUE(!entities[0].alive);                               // still tombstoned
    EXPECT_EQ_INT((int)REMOTE_NODE_ID, (int)pub.peers[0].node_id); // but this Publisher is untouched
    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    EXPECT_TRUE(ack != NULL);
    EXPECT_EQ_U32(7, ack->ack_seq_no);
}

int main(void) {
    test_lease_expiry_drops_subscriber_from_publisher_ack_set();
    test_lease_expiry_leaves_unrelated_publisher_alone();
    test_mock_reset();
    test_expires_peer_after_missed_intervals();
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
