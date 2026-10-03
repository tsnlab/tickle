/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A context at rmw_tickle's capacity (CONTEXT_NODE_PLAN.md 4a, 2026-09-27): 64 nodes of 32 endpoints each, 2048 in all,
// with ROS-length names. Every one is created and found locally, one more is refused, and a remote context's
// discovery table learns all 2048 from the announce - which, at ~15 such endpoints per 1472-byte fragment, needs far
// more than the 32 fragments an announce was limited to.
//
// Built here at rmw_tickle's settings. Controls, built by hand: -Dtt_UPDATE_MAX_PARTS=32 (the old fragment limit) fails
// the remote half; and before 4a, tt_MAX_ENDPOINT_COUNT 2048 did not compile at all (config.h asserted <= 256).
#ifndef tt_MAX_ENDPOINT_COUNT
#define tt_MAX_ENDPOINT_COUNT 2048
#endif
#ifndef tt_ENDPOINT_INDEX_SIZE
#define tt_ENDPOINT_INDEX_SIZE 4096
#endif
#ifndef tt_MAX_DISCOVERED_ENTITIES
#define tt_MAX_DISCOVERED_ENTITIES 2048
#endif
#ifndef tt_UPDATE_MAX_PARTS
#define tt_UPDATE_MAX_PARTS 255
#endif
#ifndef tt_MAX_NODES
#define tt_MAX_NODES 64
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: two contexts in one process, the announce carried between them by hand.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define NODES 64
#define PER_NODE 32
#define ENDPOINTS (NODES * PER_NODE)
#define REMOTE_ID 2
#define LOCAL_ID 1
#define REMOTE_IP 0x0a000002
#define REMOTE_PORT 8282
#define CLOCK_NS 1000000000ULL
#define CAPTURE_MAX 256
#define NAME_LENGTH 64

static uint8_t captured[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH];
static size_t captured_length[CAPTURE_MAX];
static int captured_count;

static void capture(const void* buf, size_t len) {
    if (captured_count < CAPTURE_MAX && len <= tt_MAX_BUFFER_LENGTH) {
        memcpy(captured[captured_count], buf, len);
        captured_length[captured_count++] = len;
    }
}

static int32_t data_size(struct tt_Data* data) {
    (void)data;
    return 4;
}
static int32_t data_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)data;
    return len < 4 ? -1 : (memset(payload, 0, 4), 4);
}
static int32_t data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    return (int32_t)len;
}
static void data_free(struct tt_Data* data) {
    (void)data;
}

static struct tt_Context remote;
static struct tt_Context local;
static struct tt_Discovery discovery;
static struct tt_Node nodes[NODES];
static char node_names[NODES][NAME_LENGTH];
static struct tt_Topic topic;
static char endpoint_names[ENDPOINTS][NAME_LENGTH];
static struct tt_Publisher pubs[ENDPOINTS];

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context->last_modified = CLOCK_NS;
}

static void test_a_context_at_capacity_is_created_found_and_announced(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&remote, REMOTE_ID);
    init_context(&local, LOCAL_ID);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&local, &discovery, NULL, NULL));
    topic.name = "geometry_msgs::msg::dds_::TwistStamped_"; // a ROS type name, as rmw_tickle announces it
    topic.data_size = 4;
    topic.data_encode_size = data_size;
    topic.data_encode = data_encode;
    topic.data_decode = data_decode;
    topic.data_free = data_free;

    int created = 0;
    for (int n = 0; n < NODES; n++) {
        (void)snprintf(node_names[n], sizeof(node_names[n]), "navigation_node_%02d", n);
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&remote, &nodes[n], node_names[n], "/"));
        for (int e = 0; e < PER_NODE; e++) {
            int i = (n * PER_NODE) + e;
            (void)snprintf(endpoint_names[i], sizeof(endpoint_names[i]), "rt/navigation_node_%02d/topic_%02d_cmd_vel",
                           n, e);
            created += tt_Node_create_publisher(&nodes[n], &pubs[i], &topic, endpoint_names[i]) == tt_RET_OK ? 1 : 0;
        }
    }
    EXPECT_EQ_INT(ENDPOINTS, created);
    static struct tt_Publisher one_more;
    EXPECT_EQ_INT(tt_RET_OUT_OF_BUFFER, tt_Node_create_publisher(&nodes[0], &one_more, &topic, "rt/one_more"));

    // Found locally: every endpoint by its own id, on its own node.
    int found = 0;
    for (int i = 0; i < ENDPOINTS; i++) {
        struct tt_Endpoint* endpoint = find_endpoint(&remote, tt_KIND_TOPIC_PUBLISHER, pubs[i].endpoint.id);
        found += endpoint == &pubs[i].endpoint && tt_Endpoint_node(&remote, endpoint) == &nodes[i / PER_NODE] ? 1 : 0;
    }
    EXPECT_EQ_INT(ENDPOINTS, found);

    // Announced: the remote context's discovery table learns every one.
    captured_count = 0;
    test_mock_send_hook = capture;
    EXPECT_TRUE(build_and_send_update(&remote, NULL, 0));
    node_flush(&remote, 0, NULL);
    test_mock_send_hook = NULL;
    printf("test_endpoint_capacity: %d endpoints announced in %d datagrams (%.1f per datagram)\n", ENDPOINTS,
           captured_count, captured_count > 0 ? (double)ENDPOINTS / captured_count : 0.0);
    EXPECT_TRUE(captured_count > 32); // more fragments than the old limit allowed
    for (int i = 0; i < captured_count; i++) {
        memcpy(local.rx_buffer, captured[i], captured_length[i]);
        (void)process_packet(&local, local.rx_buffer, 0, (uint32_t)captured_length[i], REMOTE_IP, REMOTE_PORT,
                             tt_TRANSPORT_UDP);
    }
    int known = 0;
    for (int i = 0; i < ENDPOINTS; i++) {
        known += tt_Discovery_find(&discovery, REMOTE_ID, pubs[i].endpoint.id) != NULL ? 1 : 0;
    }
    EXPECT_EQ_INT(ENDPOINTS, known);
    EXPECT_TRUE(local.update_seen[REMOTE_ID]); // the announce completed
}

int main(void) {
    test_a_context_at_capacity_is_created_found_and_announced();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_endpoint_capacity: all tests passed\n");
    return 0;
}
