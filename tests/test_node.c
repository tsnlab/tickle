/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Nodes within a context (CONTEXT_NODE_PLAN.md stage 2, 2026-09-27): every endpoint belongs to exactly one node, a
// node that owns endpoints cannot be destroyed, the tt_Context_create_*() shorthands create on the context's default
// node - named uniquely per context, and brought into use only by them - and a context whose endpoints are all
// created on its own nodes, as rmw_tickle's are, has no default node.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: a context set up by hand (no socket), and its node table read directly.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

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

static void init_node(struct tt_Context* node) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
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

static struct tt_Topic topic;
static struct tt_Service service;

static void setup(struct tt_Context* context, uint8_t id) {
    test_mock_reset();
    init_node(context);
    context->id = id;
    init_topic(&topic, "node_topic");
    init_service(&service, "node_service");
}

// Endpoints created on two nodes of one context each report their own node, whatever their kind.
static void test_endpoints_on_two_nodes_each_report_their_own(void) {
    static struct tt_Context context;
    setup(&context, 1);
    struct tt_Node a;
    struct tt_Node b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&context, &a, "talker", "/"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&context, &b, "listener", "/ns"));
    EXPECT_EQ_U32(1, a.index);
    EXPECT_EQ_U32(2, b.index);
    EXPECT_TRUE(context.nodes[1] == &a && context.nodes[2] == &b);

    struct tt_Publisher pub;
    struct tt_Subscriber sub;
    struct tt_Client client;
    struct tt_Server server;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&a, &pub, &topic, "chatter"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&b, &sub, &topic, "chatter", stub_subscriber_callback));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(&a, &client, &service, "add", stub_client_callback));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_server(&b, &server, &service, "add", stub_server_callback));
    EXPECT_TRUE(tt_Endpoint_node(&context, &pub.endpoint) == &a);
    EXPECT_TRUE(tt_Endpoint_node(&context, &sub.endpoint) == &b);
    EXPECT_TRUE(tt_Endpoint_node(&context, &client.endpoint) == &a);
    EXPECT_TRUE(tt_Endpoint_node(&context, &server.endpoint) == &b);
}

// A node that still owns an endpoint refuses destroy and stays registered; once the endpoint is gone it is
// destroyed, its index freed, and endpoints can no longer be created on it.
static void test_a_node_with_endpoints_refuses_destroy(void) {
    static struct tt_Context context;
    setup(&context, 1);
    struct tt_Node a;
    memset(&a, 0, sizeof(a));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&context, &a, "talker", "/"));
    struct tt_Publisher pub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&a, &pub, &topic, "chatter"));

    EXPECT_EQ_INT(tt_RET_ILLEGAL_STATUS, tt_Node_destroy(&a));
    EXPECT_TRUE(a.context == &context && context.nodes[a.index] == &a);

    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&pub));
    uint8_t index = a.index;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_destroy(&a));
    EXPECT_TRUE(a.context == NULL && context.nodes[index] == NULL);
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_create_publisher(&a, &pub, &topic, "chatter"));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_destroy(&a));
}

// The shorthands create on the default node: index 0, "tickle_<context id>" in "/". It does not exist before the
// first one, and a shorthand call that fails does not bring it into use.
static void test_the_shorthands_create_on_the_default_node(void) {
    static struct tt_Context context;
    setup(&context, 7);
    EXPECT_TRUE(context.default_node.context == NULL && context.nodes[0] == NULL);
    struct tt_Publisher pub;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Context_create_publisher(&context, &pub, NULL, "chatter"));
    EXPECT_TRUE(context.default_node.context == NULL);

    struct tt_Subscriber sub;
    struct tt_Client client;
    struct tt_Server server;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&context, &pub, &topic, "chatter"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&context, &sub, &topic, "chatter", stub_subscriber_callback));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_client(&context, &client, &service, "add", stub_client_callback));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_server(&context, &server, &service, "add", stub_server_callback));
    struct tt_Node* fallback = &context.default_node;
    EXPECT_TRUE(tt_Endpoint_node(&context, &pub.endpoint) == fallback &&
                tt_Endpoint_node(&context, &sub.endpoint) == fallback);
    EXPECT_TRUE(tt_Endpoint_node(&context, &client.endpoint) == fallback &&
                tt_Endpoint_node(&context, &server.endpoint) == fallback);
    EXPECT_TRUE(fallback->context == &context && context.nodes[0] == fallback);
    EXPECT_EQ_U32(0, fallback->index);
    EXPECT_TRUE(strcmp(fallback->name, "tickle_7") == 0);
    EXPECT_TRUE(strcmp(fallback->namespace_name, "/") == 0);
    EXPECT_TRUE(tt_Context_default_node(&context) == fallback);
}

// Two contexts' default nodes have different names: a shared one would appear once per process in `ros2 node list`.
static void test_default_node_names_differ_between_contexts(void) {
    static struct tt_Context first;
    static struct tt_Context second;
    setup(&first, 5);
    setup(&second, 6);
    struct tt_Node* one = tt_Context_default_node(&first);
    struct tt_Node* other = tt_Context_default_node(&second);
    EXPECT_TRUE(one != NULL && other != NULL);
    if (one == NULL || other == NULL) {
        return;
    }
    EXPECT_TRUE(strcmp(one->name, other->name) != 0);
}

// A context whose endpoints are all created on its own nodes - as rmw_tickle's are - never has a default node.
static void test_explicit_nodes_leave_no_default_node(void) {
    static struct tt_Context context;
    setup(&context, 1);
    struct tt_Node a;
    memset(&a, 0, sizeof(a));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&context, &a, "talker", "/"));
    struct tt_Publisher pub;
    struct tt_Server server;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&a, &pub, &topic, "chatter"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_server(&a, &server, &service, "add", stub_server_callback));
    EXPECT_TRUE(context.default_node.context == NULL && context.nodes[0] == NULL);
}

// tt_MAX_NODES bounds a context's nodes, the default node's index included; a destroyed node's index is reused, and
// a node already created cannot be created again.
static void test_the_node_table_is_bounded_and_reused(void) {
    static struct tt_Context context;
    setup(&context, 1);
    static struct tt_Node nodes[tt_MAX_NODES];
    memset(nodes, 0, sizeof(nodes));
    for (int i = 0; i < tt_MAX_NODES - 1; i++) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&context, &nodes[i], "n", "/"));
    }
    EXPECT_EQ_INT(tt_RET_OUT_OF_BUFFER, tt_Node_create(&context, &nodes[tt_MAX_NODES - 1], "n", "/"));
    EXPECT_EQ_INT(tt_RET_ILLEGAL_STATUS, tt_Node_create(&context, &nodes[0], "n", "/"));
    uint8_t freed = nodes[3].index;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_destroy(&nodes[3]));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create(&context, &nodes[tt_MAX_NODES - 1], "n", "/"));
    EXPECT_EQ_U32(freed, nodes[tt_MAX_NODES - 1].index);
    EXPECT_TRUE(tt_Context_default_node(&context) != NULL); // index 0 was kept for it all along
}

int main(void) {
    test_endpoints_on_two_nodes_each_report_their_own();
    test_a_node_with_endpoints_refuses_destroy();
    test_the_shorthands_create_on_the_default_node();
    test_default_node_names_differ_between_contexts();
    test_explicit_nodes_leave_no_default_node();
    test_the_node_table_is_bounded_and_reused();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_node: all tests passed\n");
    return 0;
}
