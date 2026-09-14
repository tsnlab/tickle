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

// Whitebox: process_update()/check_liveliness() are static.
// rmw_tickle/PLAN.md's Milestone 0(c) - opt-in graph introspection built on top of the same
// UPDATE processing test_peer_discovery.c/test_liveliness.c already exercise for peers/
// liveliness, now also recording into an attached struct tt_Discovery.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define ENTITY_ID 0x11111111

static void init_node(struct tt_Node* node) {
    memset(node, 0, sizeof(*node));
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

static void init_header(struct tt_Header* header, uint8_t source) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = source;
}

// Same shape as test_peer_discovery.c's/test_liveliness.c's own helper.
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

// Records: (node, callback fire count, last args) - a test-local observer, not part of
// test_mock.h since discovery callbacks are specific to this file's tests.
static int callback_calls = 0;
static uint8_t last_node_id;
static uint32_t last_endpoint_id;
static uint8_t last_kind;
static bool last_departed;

static void reset_callback_observations(void) {
    callback_calls = 0;
    last_node_id = 0;
    last_endpoint_id = 0;
    last_kind = 0;
    last_departed = false;
}

static void observe_discovery(struct tt_Node* node, uint8_t node_id, uint32_t endpoint_id, uint8_t kind, bool departed,
                              void* param) {
    (void)node;
    (void)param;
    callback_calls++;
    last_node_id = node_id;
    last_endpoint_id = endpoint_id;
    last_kind = kind;
    last_departed = departed;
}

// A node with no attached struct tt_Discovery must behave exactly as before this feature -
// no callback, and (implicitly, since node->discovery stays NULL) no attempt to write into any
// caller-owned struct.
static void test_no_discovery_attached_is_a_no_op(void) {
    struct tt_Node node;
    init_node(&node);
    reset_callback_observations();

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node.rx_buffer, 100, ENTITY_ID, tt_KIND_TOPIC_PUBLISHER,
                                            "std_msgs/msg/"
                                            "String",
                                            "topic");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    EXPECT_EQ_INT(0, callback_calls);
}

// The primary path: attaching a struct tt_Discovery records the announced entity (queryable via
// tt_Discovery_find()/tt_Discovery_count()) and fires the callback with departed=false.
static void test_attached_discovery_records_entity_and_fires_callback(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    reset_callback_observations();
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_set_discovery(&node, &discovery, observe_discovery, NULL));

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node.rx_buffer, 100, ENTITY_ID, tt_KIND_TOPIC_PUBLISHER,
                                            "std_msgs/msg/String", "my_topic");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    EXPECT_EQ_INT(1, callback_calls);
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)last_node_id);
    EXPECT_EQ_U32(ENTITY_ID, last_endpoint_id);
    EXPECT_EQ_U32(tt_KIND_TOPIC_PUBLISHER, (uint32_t)last_kind);
    EXPECT_TRUE(!last_departed);

    EXPECT_EQ_U32(1, tt_Discovery_count(&discovery));
    const struct tt_DiscoveredEntity* found = tt_Discovery_find(&discovery, REMOTE_NODE_ID, ENTITY_ID);
    EXPECT_TRUE(found != NULL);
    EXPECT_EQ_U32(tt_KIND_TOPIC_PUBLISHER, (uint32_t)found->kind);
    EXPECT_TRUE(strcmp(found->type, "std_msgs/msg/String") == 0);
    EXPECT_TRUE(strcmp(found->name, "my_topic") == 0);
}

// An explicit farewell (an announce that no longer lists a previously-seen entity) must fire the
// callback with departed=true and drop it from the table - the same forget-then-readd cycle
// test_peer_discovery.c's own peer table already relies on, now extended to discovery.
static void test_entity_dropped_from_new_announce_fires_departed(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_set_discovery(&node, &discovery, observe_discovery, NULL));

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node.rx_buffer, 100, ENTITY_ID, tt_KIND_TOPIC_PUBLISHER,
                                            "std_msgs/msg/String", "my_topic");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(1, tt_Discovery_count(&discovery));

    reset_callback_observations();

    // A farewell UPDATE: same source, different (higher) last_modified, zero entities.
    struct tt_UpdateHeader* update_header = (struct tt_UpdateHeader*)node.rx_buffer;
    update_header->last_modified = 200;
    update_header->entity_count = 0;
    uint32_t farewell_tail = sizeof(struct tt_UpdateHeader);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, farewell_tail, 0xc0a80a02, 8282));

    EXPECT_EQ_INT(1, callback_calls);
    EXPECT_TRUE(last_departed);
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)last_node_id);
    EXPECT_EQ_U32(ENTITY_ID, last_endpoint_id);
    EXPECT_EQ_U32(0, tt_Discovery_count(&discovery));
    EXPECT_TRUE(tt_Discovery_find(&discovery, REMOTE_NODE_ID, ENTITY_ID) == NULL);
}

// check_liveliness()'s timeout-based expiry must also fire departed=true and clear the entity -
// the case a farewell UPDATE never arrives for at all (a crash, a network partition).
static void test_liveliness_timeout_fires_departed(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_set_discovery(&node, &discovery, observe_discovery, NULL));

    test_mock_now = 0;
    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node.rx_buffer, 100, ENTITY_ID, tt_KIND_TOPIC_PUBLISHER,
                                            "std_msgs/msg/String", "my_topic");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(1, tt_Discovery_count(&discovery));

    reset_callback_observations();
    uint64_t past_threshold = (tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL) + 1;
    check_liveliness(&node, past_threshold, NULL);

    EXPECT_EQ_INT(1, callback_calls);
    EXPECT_TRUE(last_departed);
    EXPECT_EQ_U32(0, tt_Discovery_count(&discovery));
}

// tt_Node_set_discovery(node, NULL, ...) detaches - subsequent announces stop being recorded and
// stop firing the callback, without disturbing whatever was already recorded up to that point.
static void test_detaching_stops_recording(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Discovery discovery;
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_set_discovery(&node, &discovery, observe_discovery, NULL));

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node.rx_buffer, 100, ENTITY_ID, tt_KIND_TOPIC_PUBLISHER,
                                            "std_msgs/msg/String", "my_topic");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_set_discovery(&node, NULL, NULL, NULL));
    reset_callback_observations();

    uint32_t second_tail = write_update_one_entity(node.rx_buffer, 999, ENTITY_ID + 1, tt_KIND_TOPIC_SUBSCRIBER,
                                                   "std_msgs/msg/String", "another_topic");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, second_tail, 0xc0a80a02, 8282));

    EXPECT_EQ_INT(0, callback_calls);
    // The first entity, recorded before detaching, is untouched - detaching isn't the same as
    // clearing.
    EXPECT_EQ_U32(1, tt_Discovery_count(&discovery));
}

static void test_null_discovery_helpers_are_safe(void) {
    EXPECT_EQ_U32(0, tt_Discovery_count(NULL));
    EXPECT_TRUE(tt_Discovery_find(NULL, REMOTE_NODE_ID, ENTITY_ID) == NULL);
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_set_discovery(NULL, NULL, NULL, NULL));
}

int main(void) {
    test_mock_reset();
    test_no_discovery_attached_is_a_no_op();
    test_mock_reset();
    test_attached_discovery_records_entity_and_fires_callback();
    test_mock_reset();
    test_entity_dropped_from_new_announce_fires_departed();
    test_mock_reset();
    test_liveliness_timeout_fires_departed();
    test_mock_reset();
    test_detaching_stops_recording();
    test_null_discovery_helpers_are_safe();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_discovery: all tests passed\n");
    return 0;
}
