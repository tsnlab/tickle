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
// rmw_count_publishers()/rmw_count_subscribers() before and after a matching endpoint exists, and
// rmw_get_node_names()/rmw_get_node_names_with_enclaves() reporting the one local node. No live
// two-process/two-node test infra exists yet for rmw_tickle (same boundary every Milestone 10 test
// already keeps - see PLAN.md), and every real upstream test_rmw_implementation TestGraphAPI case
// needs a *second* node in-process (Milestone 2's own one-node-per-process limit, so that whole
// fixture is skipped for rmw_tickle - see Milestone 16) - this test instead creates a raw TickLE
// tt_Publisher/tt_Subscriber directly on the *same* node's own tt_Node (bypassing rosidl/typesupport
// entirely, which rmw_create_publisher()/rmw_create_subscription() would otherwise require a real
// generated interface package for) to populate tickle_node.endpoints[], the exact table count_
// matching() (rmw_graph.c) scans - proving that scan against a real endpoint, not just a mock.
//
// Directly touches rmw_tickle_node_t's own private tickle_node field (rmw_tickle_c/rmw_tickle.h),
// so it follows the same tt_Node_interrupt()-then-lock contract every other entry point touching
// it must (see that header's own rmw_tickle_node_t doc comment) - the background poll thread this
// node's own rmw_create_node() already started is running concurrently.

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

    // rmw_get_node_names()/_with_enclaves(): only ever the local node - see rmw_graph.c's own
    // module doc comment on this documented gap.
    rcutils_string_array_t node_names = rcutils_get_zero_initialized_string_array();
    rcutils_string_array_t node_namespaces = rcutils_get_zero_initialized_string_array();
    assert(RMW_RET_OK == rmw_get_node_names(node, &node_names, &node_namespaces));
    assert(1u == node_names.size);
    assert(1u == node_namespaces.size);
    assert(0 == strcmp(node_name, node_names.data[0]));
    assert(0 == strcmp(node_namespace, node_namespaces.data[0]));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_names));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_namespaces));

    rcutils_string_array_t enclaves = rcutils_get_zero_initialized_string_array();
    node_names = rcutils_get_zero_initialized_string_array();
    node_namespaces = rcutils_get_zero_initialized_string_array();
    assert(RMW_RET_OK == rmw_get_node_names_with_enclaves(node, &node_names, &node_namespaces, &enclaves));
    assert(1u == enclaves.size);
    assert(0 == strcmp("/", enclaves.data[0]));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_names));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_namespaces));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&enclaves));

    // rmw_count_publishers()/rmw_count_subscribers(): zero before any matching endpoint exists.
    const char* topic_name = "/test_graph_topic";
    size_t count = 0;
    assert(RMW_RET_OK == rmw_count_publishers(node, topic_name, &count));
    assert(0u == count);
    assert(RMW_RET_OK == rmw_count_subscribers(node, topic_name, &count));
    assert(0u == count);

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
    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);
    tt_ret_t tt_ret = tt_Node_create_publisher(&node_impl->tickle_node, &pub, &topic, topic_name);
    pthread_mutex_unlock(&node_impl->mutex);
    assert(tt_RET_OK == tt_ret);

    assert(RMW_RET_OK == rmw_count_publishers(node, topic_name, &count));
    assert(1u == count);
    assert(RMW_RET_OK == rmw_count_subscribers(node, topic_name, &count));
    assert(0u == count); // a publisher isn't a subscriber

    struct tt_Subscriber sub;
    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);
    tt_ret = tt_Node_create_subscriber(&node_impl->tickle_node, &sub, &topic, topic_name, fake_subscriber_callback);
    pthread_mutex_unlock(&node_impl->mutex);
    assert(tt_RET_OK == tt_ret);

    assert(RMW_RET_OK == rmw_count_publishers(node, topic_name, &count));
    assert(1u == count);
    assert(RMW_RET_OK == rmw_count_subscribers(node, topic_name, &count));
    assert(1u == count);

    // A different topic name still sees neither.
    assert(RMW_RET_OK == rmw_count_publishers(node, "/unrelated_topic", &count));
    assert(0u == count);

    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);
    assert(tt_RET_OK == tt_Subscriber_destroy(&sub));
    assert(tt_RET_OK == tt_Publisher_destroy(&pub));
    pthread_mutex_unlock(&node_impl->mutex);

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("rmw graph queries (local endpoints): PASS\n");
    return 0;
}
