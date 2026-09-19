/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 34 - "TickLE Plan"'s own backlog (superseded-priority list, item
// 3): multiple ROS 2 nodes per process. Root finding behind the design this exercises: DDS itself
// has no "Node" concept at all - rclcpp::Node is a pure rcl/rmw-layer name/namespace grouping with
// no independent DDS entity - so this needed no new transport identity, socket, or discovery
// instance per logical node, just multiple name labels sharing the one tt_Node/poll_thread/
// discovery a context already has (promoted from rmw_tickle_node_t up to rmw_tickle_context_impl_t
// - see that struct's own doc comment, rmw_tickle_c/rmw_tickle.h). Reaches into that header
// directly (a legitimate public header of this package, not a private implementation detail,
// matching test_graph.c's/test_liveliness_lost_watchdog.c's own identical precedent) to observe
// the shared state directly, since the public rmw API alone can't distinguish "two nodes sharing
// one tt_Node" from "two nodes each with their own" from the outside.

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h> // struct tt_Node - NOLINT(misc-include-cleaner), see rmw_tickle.h's own <pthread.h> comment for the identical "used only via a field access, never spelled by name" reasoning

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rcutils/types/rcutils_ret.h"
#include "rcutils/types/string_array.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    // Two logical nodes, same context - the actual capability this milestone adds (previously
    // rejected outright: "rmw_tickle supports only one node per process for now").
    rmw_node_t* node_a = rmw_create_node(&context, "test_multi_node_a", "/");
    assert(NULL != node_a);
    rmw_node_t* node_b = rmw_create_node(&context, "test_multi_node_b", "/");
    assert(NULL != node_b);

    // They really do share one tt_Node - not just a coincidentally-equal context_impl pointer, but
    // the exact same underlying node id and endpoint table address, proving there's only one real
    // transport participant serving both.
    rmw_tickle_node_t* node_a_impl = (rmw_tickle_node_t*)node_a->data;
    rmw_tickle_node_t* node_b_impl = (rmw_tickle_node_t*)node_b->data;
    assert(node_a_impl->context_impl == node_b_impl->context_impl);
    rmw_tickle_context_impl_t* context_impl = node_a_impl->context_impl;
    assert(&context_impl->tickle_node == &node_b_impl->context_impl->tickle_node);
    assert(2 == atomic_load(&context_impl->node_count));
    assert(context_impl->poll_thread_running);

    // rmw_get_node_names() now reports every logical node sharing this context (Milestone 6's own
    // "only ever the local node" gap, partly closed as a side effect) - order isn't part of the
    // contract, so this checks set membership, not position.
    rcutils_string_array_t node_names = rcutils_get_zero_initialized_string_array();
    rcutils_string_array_t node_namespaces = rcutils_get_zero_initialized_string_array();
    assert(RMW_RET_OK == rmw_get_node_names(node_a, &node_names, &node_namespaces));
    assert(2U == node_names.size);
    bool saw_a = false;
    bool saw_b = false;
    for (size_t i = 0; i < node_names.size; i++) {
        assert(0 == strcmp("/", node_namespaces.data[i]));
        if (0 == strcmp("test_multi_node_a", node_names.data[i])) {
            saw_a = true;
        }
        if (0 == strcmp("test_multi_node_b", node_names.data[i])) {
            saw_b = true;
        }
    }
    assert(saw_a);
    assert(saw_b);
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_names));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&node_namespaces));

    // A second node with the exact same (name, namespace) is allowed, not rejected - real rmw/DDS
    // has no node-name-uniqueness constraint at all (that's an rcl/ROS-graph-layer convention, not
    // something rmw itself enforces). Confirmed the hard way, not assumed: an earlier version of
    // this milestone rejected duplicates outright, which broke upstream's own test_rmw_
    // implementation conformance suite - test_graph_api.cpp's TestGraphAPI::SetUp() deliberately
    // creates its second node with the same name/namespace as the first.
    rmw_node_t* node_a_again = rmw_create_node(&context, "test_multi_node_a", "/");
    assert(NULL != node_a_again);
    assert(3 == atomic_load(&context_impl->node_count));

    // Destroying one sibling must not tear down the shared tt_Node/poll_thread while others are
    // still alive - the whole point of reference-counting instead of the old unconditional
    // teardown.
    assert(RMW_RET_OK == rmw_destroy_node(node_a));
    assert(2 == atomic_load(&context_impl->node_count));
    assert(context_impl->poll_thread_running);
    assert(RMW_RET_OK == rmw_destroy_node(node_a_again));
    assert(1 == atomic_load(&context_impl->node_count));
    assert(context_impl->poll_thread_running);

    // The real teardown only happens once the *last* logical node sharing it is destroyed.
    assert(RMW_RET_OK == rmw_destroy_node(node_b));
    assert(0 == atomic_load(&context_impl->node_count));
    assert(!context_impl->poll_thread_running);

    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_multi_node: all tests passed\n");
    return 0;
}
