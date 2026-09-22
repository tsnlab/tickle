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

// Whitebox: reaches add_endpoint_to_node()'s own duplicate-vs-not-duplicate decision directly
// through the real public tt_Node_create_publisher()/_subscriber()/_client()/_server() entry
// points - no other test file in this directory calls those four for real (they all construct
// their struct fields by hand, bypassing add_endpoint_to_node() entirely), so this is the only
// coverage of that function's own actual behavior via its real callers.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define REMOTE_NODE_ID 2
#define TEST_SENDER_IP 0xc0a80a02
#define TEST_SENDER_PORT 8282

static int32_t stub_data_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_data_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)data;
    (void)len;
    (void)payload;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len,
                                bool is_native_endian) {
    (void)data;
    (void)payload;
    (void)len;
    (void)is_native_endian;
    return (int32_t)sizeof(uint32_t);
}

static void stub_data_free(struct tt_Data* data) {
    (void)data;
}

static void stub_subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                     struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
}

static int32_t stub_request_encode_size(struct tt_Request* request) {
    (void)request;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    (void)request;
    (void)len;
    (void)payload;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_request_decode(struct tt_Request* request, const uint8_t* payload, const uint32_t len,
                                   bool is_native_endian) {
    (void)request;
    (void)payload;
    (void)len;
    (void)is_native_endian;
    return (int32_t)sizeof(uint32_t);
}

static void stub_request_free(struct tt_Request* request) {
    (void)request;
}

static int32_t stub_response_encode_size(struct tt_Response* response) {
    (void)response;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_response_encode(struct tt_Response* response, uint8_t* payload, const uint32_t len) {
    (void)response;
    (void)len;
    (void)payload;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_response_decode(struct tt_Response* response, const uint8_t* payload, const uint32_t len,
                                    bool is_native_endian) {
    (void)response;
    (void)payload;
    (void)len;
    (void)is_native_endian;
    return (int32_t)sizeof(uint32_t);
}

static void stub_response_free(struct tt_Response* response) {
    (void)response;
}

static void stub_client_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    (void)return_code;
    (void)response;
}

static int8_t stub_server_callback(struct tt_Server* server, struct tt_Request* request, struct tt_Response* response,
                                   tt_RequestId request_id) {
    (void)server;
    (void)request;
    (void)response;
    (void)request_id;
    return 0;
}

static void init_node(struct tt_Node* node) {
    memset(node, 0, sizeof(*node));
    node->id = 1;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

static void init_topic(struct tt_Topic* topic, const char* name) {
    memset(topic, 0, sizeof(*topic));
    topic->name = name;
    topic->data_size = sizeof(uint32_t);
    topic->data_encode_size = stub_data_encode_size;
    topic->data_encode = stub_data_encode;
    topic->data_decode = stub_data_decode;
    topic->data_free = stub_data_free;
}

static void init_service(struct tt_Service* service, const char* name) {
    memset(service, 0, sizeof(*service));
    service->name = name;
    service->request_size = sizeof(uint32_t);
    service->response_size = sizeof(uint32_t);
    service->request_encode_size = stub_request_encode_size;
    service->request_encode = stub_request_encode;
    service->request_decode = stub_request_decode;
    service->request_free = stub_request_free;
    service->response_encode_size = stub_response_encode_size;
    service->response_encode = stub_response_encode;
    service->response_decode = stub_response_decode;
    service->response_free = stub_response_free;
}

// Milestone 35 (rmw_tickle/PLAN.md) - two independent local Publishers for the exact same topic
// name must both succeed now, not just the first: real DDS lets multiple independent Publishers
// (or Subscribers, Clients, Servers) share a topic/service name, and this rmw_tickle's own
// Milestone 34 (multiple ROS 2 nodes sharing one tt_Node) made that reachable within a single
// process for the first time - a scenario add_endpoint_to_node() used to reject outright as
// "Duplicate endpoint", found the hard way when it broke test_rmw_implementation's own
// TestGraphAPI.count_clients_and_services in real CI.
static void test_two_publishers_same_topic_both_succeed(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub_a;
    struct tt_Publisher pub_b;
    init_node(&node);
    init_topic(&topic, "/test_topic");

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub_a, &topic, "/test_topic"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub_b, &topic, "/test_topic"));
    EXPECT_EQ_U32(2, node.endpoint_count);
    EXPECT_EQ_U32(pub_a.endpoint.id, pub_b.endpoint.id); // same name -> same wire id, by design
}

// The Subscriber-side mirror of the above - the single most common real-world shape of this
// milestone's own fix (e.g. two different callbacks, or two nodes in one process, both
// subscribing to the same topic), not just the Publisher/Server-side cases that happened to be
// what real CI found first.
static void test_two_subscribers_same_topic_both_succeed(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub_a;
    struct tt_Subscriber sub_b;
    init_node(&node);
    init_topic(&topic, "/test_topic");

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&node, &sub_a, &topic, "/test_topic", stub_subscriber_callback));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&node, &sub_b, &topic, "/test_topic", stub_subscriber_callback));
    EXPECT_EQ_U32(2, node.endpoint_count);
}

// The Client-side case.
static void test_two_clients_same_service_both_succeed(void) {
    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client_a;
    struct tt_Client client_b;
    init_node(&node);
    init_service(&service, "/test_service");

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(&node, &client_a, &service, "/test_service", stub_client_callback));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(&node, &client_b, &service, "/test_service", stub_client_callback));
    EXPECT_EQ_U32(2, node.endpoint_count);
}

// The Server-side case - the exact scenario test_rmw_implementation's own TestGraphAPI.count_
// clients_and_services needs (two independent Servers for one service name, in one process).
static void test_two_servers_same_service_both_succeed(void) {
    struct tt_Node node;
    struct tt_Service service;
    struct tt_Server server_a;
    struct tt_Server server_b;
    init_node(&node);
    init_service(&service, "/test_service");

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_server(&node, &server_a, &service, "/test_service", stub_server_callback));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_server(&node, &server_b, &service, "/test_service", stub_server_callback));
    EXPECT_EQ_U32(2, node.endpoint_count);
}

// add_endpoint_to_node()'s own remaining guard - registering the exact same struct pointer twice
// (a real bug: a double-create without an intervening destroy) is still rejected, unlike a
// genuinely distinct second instance sharing a name.
static void test_registering_same_pointer_twice_still_fails(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub;
    init_node(&node);
    init_topic(&topic, "/test_topic");

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub, &topic, "/test_topic"));
    EXPECT_EQ_INT(tt_RET_IILEGAL_ENDPOINT_ID, add_endpoint_to_node(&node, &pub.endpoint));
    EXPECT_EQ_U32(1, node.endpoint_count); // the rejected re-registration didn't leak a slot
}

// A destroyed duplicate must not affect its sibling still sharing the name - remove_endpoint_
// from_node() finds by pointer identity, not by (kind, id), so this was never actually at risk,
// but it's the one invariant this whole milestone's design depends on staying true.
static void test_destroying_one_duplicate_leaves_its_sibling_registered(void) {
    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub_a;
    struct tt_Publisher pub_b;
    init_node(&node);
    init_topic(&topic, "/test_topic");

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub_a, &topic, "/test_topic"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub_b, &topic, "/test_topic"));
    EXPECT_EQ_U32(2, node.endpoint_count);

    EXPECT_TRUE(remove_endpoint_from_node(&node, &pub_a.endpoint));
    EXPECT_EQ_U32(1, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == &pub_b.endpoint);
}

// Builds an UpdateHeader with a single following TOPIC_SUBSCRIBER UpdateEntity in node->rx_buffer,
// returning the tail offset - same shape as test_durability_pubsub.c's own write_update_one_
// subscriber(), needed here too (each tests/test_*.c is its own standalone binary, no helpers
// shared across files).
static uint32_t write_update_one_subscriber(struct tt_Node* node, uint64_t last_modified, uint32_t endpoint_id) {
    struct tt_UpdateHeader* update_header = (struct tt_UpdateHeader*)node->rx_buffer;
    update_header->last_modified = last_modified;
    update_header->entity_count = 1;
    uint32_t tail = sizeof(struct tt_UpdateHeader);

    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(node->rx_buffer + tail);
    memset(entity, 0, sizeof(*entity)); // explicit: rx_buffer is reused across writes in these tests
    entity->endpoint_id = endpoint_id;
    entity->kind = tt_KIND_TOPIC_SUBSCRIBER;
    tail += sizeof(struct tt_UpdateEntity);

    tt_encode_string(node->rx_buffer, &tail, tt_MAX_BUFFER_LENGTH * 2, "/test_topic");
    tt_encode_string(node->rx_buffer, &tail, tt_MAX_BUFFER_LENGTH * 2, "test_subscriber");

    return tail;
}

// Milestone 35 - a remote Subscriber's own discovery announce must register as a peer on *every*
// local Publisher sharing the announced topic name, not just whichever one happened to register
// first: otherwise a second local Publisher (now legal to create at all, per the tests above)
// would exist locally but never actually learn any remote peers of its own, making it silently
// unable to unicast/durability-backlog/heartbeat to anyone despite being a perfectly valid
// Publisher. Exercises the real decode_update_entities() path (via process_update()), not a direct
// unit call, since the fan-out itself lives inside that function's own for_each_endpoint() use.
static void test_update_registers_peer_on_every_matching_publisher(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Publisher pub_a;
    struct tt_Publisher pub_b;
    init_node(&node);
    init_topic(&topic, "/test_topic");

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub_a, &topic, "/test_topic"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub_b, &topic, "/test_topic"));
    EXPECT_EQ_U32(2, node.endpoint_count);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_update_one_subscriber(&node, 100, pub_a.endpoint.id);
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));

    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)pub_a.peers[0].node_id);
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)pub_b.peers[0].node_id);
}

int main(void) {
    test_two_publishers_same_topic_both_succeed();
    test_two_subscribers_same_topic_both_succeed();
    test_two_clients_same_service_both_succeed();
    test_two_servers_same_service_both_succeed();
    test_registering_same_pointer_twice_still_fails();
    test_destroying_one_duplicate_leaves_its_sibling_registered();
    test_update_registers_peer_on_every_matching_publisher();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_duplicate_endpoints: all tests passed\n");
    return 0;
}
