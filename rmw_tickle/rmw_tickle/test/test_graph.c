/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 6: exercises rmw_graph.c's own local-endpoint scan for real -
// rmw_count_publishers()/rmw_count_subscribers()/rmw_count_clients()/_services() before and after
// a matching endpoint exists, and rmw_get_node_names()/rmw_get_node_names_with_enclaves() (this
// file's own single-node case - see test_multi_node.c for the real multiple-logical-nodes-sharing-
// one-context exercise Milestone 34 made possible). This test creates a raw TickLE tt_Publisher/
// tt_Subscriber/tt_Client/tt_Server directly on the node's own shared tt_Node (bypassing rosidl/
// typesupport entirely, which rmw_create_publisher()/rmw_create_subscription()/etc. would
// otherwise require a real generated interface package for) to populate tickle_node.endpoints[],
// the exact table count_matching() (rmw_graph.c) scans - proving that scan against a real
// endpoint, not just a mock.
//
// Directly touches rmw_tickle_context_impl_t's own private tickle_node field (rmw_tickle_c/
// rmw_tickle.h, promoted up from rmw_tickle_node_t in Milestone 34), so it follows the same tt_
// Node_interrupt()-then-lock contract every other entry point touching it must (see that header's
// own rmw_tickle_context_impl_t doc comment) - the background poll thread this node's own rmw_
// create_node() already started is running concurrently.

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/hal.h> // tt_ret_t/tt_RET_OK
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rcutils/types/rcutils_ret.h" // RCUTILS_RET_OK
#include "rcutils/types/string_array.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

// Never actually invoked - no publish/receive happens in this test, only endpoint registration.
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return 0;
}
// payload can't be const: this must match tt_DATA_ENCODE's own fixed signature exactly, and a
// real encoder does write through it (only this no-op stub never does).
// NOLINTNEXTLINE(readability-non-const-parameter)
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)data;
    (void)payload;
    (void)len;
    return 0;
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)data;
    (void)payload;
    (void)len;
    (void)is_native_endian;
    return 0;
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}
static void fake_subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                     struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
}

// Never actually invoked - rmw_count_clients()/rmw_count_services() only need the client/server
// pair to exist in tickle_node.endpoints[], same "no real call happens" reasoning as the topic
// stubs above.
static int32_t fake_request_encode_size(struct tt_Request* request) {
    (void)request;
    return 0;
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_REQUEST_ENCODE's own fixed signature
static int32_t fake_request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    (void)request;
    (void)payload;
    (void)len;
    return 0;
}
static int32_t fake_request_decode(struct tt_Request* request, const uint8_t* payload, const uint32_t len,
                                   bool is_native_endian) {
    (void)request;
    (void)payload;
    (void)len;
    (void)is_native_endian;
    return 0;
}
static void fake_request_free(struct tt_Request* request) {
    (void)request;
}
static int32_t fake_response_encode_size(struct tt_Response* response) {
    (void)response;
    return 0;
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_RESPONSE_ENCODE's own fixed signature
static int32_t fake_response_encode(struct tt_Response* response, uint8_t* payload, const uint32_t len) {
    (void)response;
    (void)payload;
    (void)len;
    return 0;
}
static int32_t fake_response_decode(struct tt_Response* response, const uint8_t* payload, const uint32_t len,
                                    bool is_native_endian) {
    (void)response;
    (void)payload;
    (void)len;
    (void)is_native_endian;
    return 0;
}
static void fake_response_free(struct tt_Response* response) {
    (void)response;
}
static void fake_client_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    (void)return_code;
    (void)response;
}
static int8_t fake_server_callback(struct tt_Server* server, struct tt_Request* request, struct tt_Response* response,
                                   tt_RequestId request_id) {
    (void)server;
    (void)request;
    (void)response;
    (void)request_id;
    return 0;
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    // See test_node_lifecycle.c's own comment on this same line.
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    const char* node_name = "test_graph";
    const char* node_namespace = "/";
    rmw_node_t* node = rmw_create_node(&context, node_name, node_namespace);
    assert(NULL != node);

    // rmw_get_node_names()/_with_enclaves(): the one node this test itself creates - see
    // test_multi_node.c for the case where several share one context, and rmw_graph.c's own
    // module doc comment for what's still not knowable (any *other* process's own nodes).
    rcutils_string_array_t node_names = rcutils_get_zero_initialized_string_array();
    rcutils_string_array_t node_namespaces = rcutils_get_zero_initialized_string_array();
    assert(RMW_RET_OK == rmw_get_node_names(node, &node_names, &node_namespaces));
    assert(1U == node_names.size);
    assert(1U == node_namespaces.size);
    assert(0 == strcmp(node_name, node_names.data[0]));
    assert(0 == strcmp(node_namespace, node_namespaces.data[0]));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_names));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_namespaces));

    rcutils_string_array_t enclaves = rcutils_get_zero_initialized_string_array();
    node_names = rcutils_get_zero_initialized_string_array();
    node_namespaces = rcutils_get_zero_initialized_string_array();
    assert(RMW_RET_OK == rmw_get_node_names_with_enclaves(node, &node_names, &node_namespaces, &enclaves));
    assert(1U == enclaves.size);
    assert(0 == strcmp("/", enclaves.data[0]));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_names));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_namespaces));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&enclaves));

    // rmw_count_publishers()/rmw_count_subscribers(): zero before any matching endpoint exists.
    const char* topic_name = "/test_graph_topic";
    size_t count = 0;
    assert(RMW_RET_OK == rmw_count_publishers(node, topic_name, &count));
    assert(0U == count);
    assert(RMW_RET_OK == rmw_count_subscribers(node, topic_name, &count));
    assert(0U == count);

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;

    struct tt_Topic topic = {
        .name = "test_graph/Msg",
        .data_size = sizeof(struct tt_Data),
        .data_encode_size = fake_encode_size,
        .data_encode = fake_encode,
        .data_decode = fake_decode,
        .data_free = fake_free,
    };

    // See this file's own top comment on why the interrupt-then-lock pattern is needed here -
    // tickle_node is otherwise only ever touched by this node's own background poll thread.
    struct tt_Publisher pub;
    tt_Node_interrupt(&node_impl->context_impl->tickle_node);
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    tt_ret_t tt_ret = tt_Node_create_publisher(&node_impl->context_impl->tickle_node, &pub, &topic, topic_name);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    assert(tt_RET_OK == tt_ret);

    assert(RMW_RET_OK == rmw_count_publishers(node, topic_name, &count));
    assert(1U == count);
    assert(RMW_RET_OK == rmw_count_subscribers(node, topic_name, &count));
    assert(0U == count); // a publisher isn't a subscriber

    struct tt_Subscriber sub;
    tt_Node_interrupt(&node_impl->context_impl->tickle_node);
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    tt_ret = tt_Node_create_subscriber(&node_impl->context_impl->tickle_node, &sub, &topic, topic_name,
                                       fake_subscriber_callback);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    assert(tt_RET_OK == tt_ret);

    assert(RMW_RET_OK == rmw_count_publishers(node, topic_name, &count));
    assert(1U == count);
    assert(RMW_RET_OK == rmw_count_subscribers(node, topic_name, &count));
    assert(1U == count);

    // A different topic name still sees neither.
    assert(RMW_RET_OK == rmw_count_publishers(node, "/unrelated_topic", &count));
    assert(0U == count);

    // rmw_count_clients()/rmw_count_services() - rmw_tickle/PLAN.md's remaining-rmw-API-surface
    // backlog. Same shape as the publisher/subscriber pair above, mirrored onto the service side.
    const char* service_name = "/test_graph_service";
    assert(RMW_RET_OK == rmw_count_clients(node, service_name, &count));
    assert(0U == count);
    assert(RMW_RET_OK == rmw_count_services(node, service_name, &count));
    assert(0U == count);

    struct tt_Service service = {
        .name = "test_graph/Srv",
        .request_size = sizeof(struct tt_Request),
        .response_size = sizeof(struct tt_Response),
        .request_encode_size = fake_request_encode_size,
        .request_encode = fake_request_encode,
        .request_decode = fake_request_decode,
        .request_free = fake_request_free,
        .response_encode_size = fake_response_encode_size,
        .response_encode = fake_response_encode,
        .response_decode = fake_response_decode,
        .response_free = fake_response_free,
    };

    struct tt_Client client;
    tt_Node_interrupt(&node_impl->context_impl->tickle_node);
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    tt_ret = tt_Node_create_client(&node_impl->context_impl->tickle_node, &client, &service, service_name,
                                   fake_client_callback);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    assert(tt_RET_OK == tt_ret);

    assert(RMW_RET_OK == rmw_count_clients(node, service_name, &count));
    assert(1U == count);
    assert(RMW_RET_OK == rmw_count_services(node, service_name, &count));
    assert(0U == count); // a client isn't a server

    struct tt_Server server;
    tt_Node_interrupt(&node_impl->context_impl->tickle_node);
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    tt_ret = tt_Node_create_server(&node_impl->context_impl->tickle_node, &server, &service, service_name,
                                   fake_server_callback);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    assert(tt_RET_OK == tt_ret);

    assert(RMW_RET_OK == rmw_count_clients(node, service_name, &count));
    assert(1U == count);
    assert(RMW_RET_OK == rmw_count_services(node, service_name, &count));
    assert(1U == count);

    tt_Node_interrupt(&node_impl->context_impl->tickle_node);
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    assert(tt_RET_OK == tt_Client_destroy(&client));
    assert(tt_RET_OK == tt_Server_destroy(&server));
    assert(tt_RET_OK == tt_Subscriber_destroy(&sub));
    assert(tt_RET_OK == tt_Publisher_destroy(&pub));
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("rmw graph queries (local endpoints): PASS\n");
    return 0;
}
