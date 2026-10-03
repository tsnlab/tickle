/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Nodes in discovery (CONTEXT_NODE_PLAN.md stage 3, wire v11, 2026-09-27). A context's announce lists its nodes
// (tt_KIND_NODE entries: namespace as type, name, own index) and every entry carries its node's index in spare bits of
// kind and qos. A receiving context records the nodes, attributes each endpoint to its node by (context id, node
// index), and never treats a node entry as an endpoint. An empty default node is not announced. Two limit shapes:
// 20 nodes x 12 endpoints (a composed bringup), and 256 nodes x 2 (the 8-bit index, all of it).
//
// Built at rmw_tickle's capacities, so both shapes fit.
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
#define tt_MAX_NODES 256
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

#define REMOTE_ID 2
#define LOCAL_ID 1
#define REMOTE_IP 0x0a000002
#define REMOTE_PORT 8282
#define CLOCK_NS 1000000000ULL
#define CAPTURE_MAX 256
#define NAME_LENGTH 64
#define MAX_ENDPOINTS 512

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
static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

static struct tt_Context remote;
static struct tt_Context local;
static struct tt_Discovery discovery;
static struct tt_Topic topic;
static struct tt_Node nodes[tt_MAX_NODES];
static char node_names[tt_MAX_NODES][NAME_LENGTH];
static char endpoint_names[MAX_ENDPOINTS][NAME_LENGTH];
static struct tt_Publisher pubs[MAX_ENDPOINTS];
static struct tt_Subscriber subs[MAX_ENDPOINTS];

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context->last_modified = CLOCK_NS;
}

static void setup(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&remote, REMOTE_ID);
    init_context(&local, LOCAL_ID);
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&local, &discovery, NULL, NULL));
    memset(&topic, 0, sizeof(topic));
    topic.name = "geometry_msgs::msg::dds_::Twist_";
    topic.data_size = 4;
    topic.data_encode_size = data_size;
    topic.data_encode = data_encode;
    topic.data_decode = data_decode;
    topic.data_free = data_free;
    memset(nodes, 0, sizeof(nodes));
}

// The remote's whole announce, handed to the local context.
static void carry_announce(void) {
    captured_count = 0;
    test_mock_send_hook = capture;
    test_mock_now += CLOCK_NS; // time moves on between changes, so each announce is a new generation
    remote.last_modified = test_mock_now;
    EXPECT_TRUE(build_and_send_update(&remote, NULL, 0));
    node_flush(&remote, 0, NULL);
    test_mock_send_hook = NULL;
    for (int i = 0; i < captured_count; i++) {
        memcpy(local.rx_buffer, captured[i], captured_length[i]);
        (void)process_packet(&local, local.rx_buffer, 0, (uint32_t)captured_length[i], REMOTE_IP, REMOTE_PORT,
                             tt_TRANSPORT_UDP);
    }
}

// The local context's record of the remote node named `name`, or NULL.
static const struct tt_DiscoveredEntity* remote_node(const char* name) {
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* entity = &discovery.entities[i];
        if (entity->context_id == REMOTE_ID && entity->kind == tt_KIND_NODE && strcmp(entity->name, name) == 0) {
            return entity;
        }
    }
    return NULL;
}

// node_count nodes of per_node endpoints (publishers and subscribers alternating) created on the remote context.
static void create_shape(int node_count, int per_node) {
    int created = 0;
    for (int n = 0; n < node_count; n++) {
        (void)snprintf(node_names[n], sizeof(node_names[n]), "node_%03d", n);
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&remote, &nodes[n], node_names[n], n % 2 == 0 ? "/" : "/ns"));
        for (int e = 0; e < per_node; e++) {
            int i = (n * per_node) + e;
            (void)snprintf(endpoint_names[i], sizeof(endpoint_names[i]), "rt/node_%03d/topic_%02d", n, e);
            tt_ret_t ret = e % 2 == 0
                               ? tt_Node_create_publisher(&nodes[n], &pubs[i], &topic, endpoint_names[i])
                               : tt_Node_create_subscriber(&nodes[n], &subs[i], &topic, endpoint_names[i], on_sample);
            created += ret == tt_RET_OK ? 1 : 0;
        }
    }
    EXPECT_EQ_INT(node_count * per_node, created);
}

// Of node n's per_node endpoints, how many the local context attributes to `entry` (*right) or to another node
// (*wrong).
static void count_attribution(int n, int per_node, const struct tt_DiscoveredEntity* entry, int* right, int* wrong) {
    for (int e = 0; e < per_node; e++) {
        int i = (n * per_node) + e;
        uint32_t id = e % 2 == 0 ? pubs[i].endpoint.id : subs[i].endpoint.id;
        const struct tt_DiscoveredEntity* endpoint = tt_Discovery_find(&discovery, REMOTE_ID, id);
        if (endpoint != NULL) {
            *(endpoint->node_index == entry->node_index ? right : wrong) += 1;
        }
    }
}

// The local context must list every node by name and namespace, and attribute each endpoint to its own node and to
// no other.
static void check_shape(int node_count, int per_node) {
    setup();
    create_shape(node_count, per_node);
    carry_announce();
    int nodes_listed = 0;
    int attributed = 0;
    int misattributed = 0;
    for (int n = 0; n < node_count; n++) {
        const struct tt_DiscoveredEntity* entry = remote_node(node_names[n]);
        if (entry == NULL || strcmp(entry->type, n % 2 == 0 ? "/" : "/ns") != 0 ||
            entry->node_index != nodes[n].index) {
            continue;
        }
        nodes_listed++;
        count_attribution(n, per_node, entry, &attributed, &misattributed);
    }
    EXPECT_EQ_INT(node_count, nodes_listed);
    EXPECT_EQ_INT(node_count * per_node, attributed);
    EXPECT_EQ_INT(0, misattributed);
}

static void test_twenty_nodes_of_twelve_endpoints(void) {
    check_shape(20, 12);
}

static void test_two_hundred_fifty_six_nodes(void) {
    check_shape(tt_MAX_NODES, 2);
}

// A default node that owns no endpoint is not announced; one that does is, as "tickle_<id>" in "/"; an explicit node
// without endpoints is.
static void test_only_a_default_node_in_use_is_announced(void) {
    setup();
    (void)tt_Context_default_node(&remote); // in use as an object, but owning nothing
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&remote, &nodes[1], "idle", "/"));
    carry_announce();
    EXPECT_TRUE(remote_node("tickle_2") == NULL);
    EXPECT_TRUE(remote_node("idle") != NULL);

    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&remote, &pubs[0], &topic, "rt/chatter"));
    carry_announce();
    EXPECT_TRUE(remote_node("tickle_2") != NULL);
}

// A node entry is recorded, and nothing else: it registers no peer on a local publisher of a colliding name and is
// never counted as an endpoint.
static void test_a_node_entry_is_never_an_endpoint(void) {
    setup();
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&remote, &nodes[1], "talker", "/"));
    // A local subscriber whose id equals the remote node entry's id: were the entry taken for a publisher, it would
    // become this subscriber's writer.
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&local, &subs[0], &topic, "rt/x", on_sample));
    subs[0].endpoint.id = nodes[1].entry.id;
    carry_announce();
    EXPECT_TRUE(remote_node("talker") != NULL);
    EXPECT_EQ_INT((int)tt_CONTEXT_ID_INVALID, (int)subs[0].writers[0].context_id);
    uint32_t endpoints_seen = 0;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        uint8_t kind = discovery.entities[i].kind;
        endpoints_seen += discovery.entities[i].context_id == REMOTE_ID && kind != tt_KIND_NODE ? 1U : 0U;
    }
    EXPECT_EQ_U32(0, endpoints_seen);
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    test_twenty_nodes_of_twelve_endpoints();
    test_two_hundred_fifty_six_nodes();
    test_only_a_default_node_in_use_is_announced();
    test_a_node_entry_is_never_an_endpoint();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_node_discovery: all tests passed\n");
    return 0;
}
