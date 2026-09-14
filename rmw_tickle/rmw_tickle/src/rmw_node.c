/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 2: rmw_create_node()/rmw_destroy_node(), a background thread per
// node driving tt_Node_poll(), and the per-node mutex every other entry point (rmw_publish() et
// al., Milestone 3+) must serialize against it with - see rmw_tickle.h's own rmw_tickle_node_t
// doc comment for the exact locking contract.

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <tickle/tickle.h>

#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw_tickle_c/rmw_tickle.h"

// One process-wide _tt_CONFIG (include/tickle/config.h) means one tt_Node per process for now -
// rmw_tickle/PLAN.md's own "Deferred" note under the milestone table. Guards against a second
// rmw_create_node() call silently reusing/colliding with the first tt_Node rather than failing
// loudly. Process-wide rather than per-rmw_context_t: the constraint is about _tt_CONFIG, which
// doesn't belong to any one context either.
static atomic_bool g_tickle_node_created = false;

// tt_RECEIVE_TIMEOUT (config.h, 100us) is what tt_Node_poll() itself substitutes for any
// negative timeout - passed explicitly here (rather than -1, matching examples/*/*.c's own
// top-level poll loops) so this file doesn't depend on that substitution as an implicit,
// undocumented-at-the-call-site default.
static const int64_t RMW_TICKLE_POLL_TIMEOUT_NS = tt_RECEIVE_TIMEOUT;

static void* poll_thread_main(void* arg) {
    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)arg;
    while (node_impl->poll_thread_running) {
        pthread_mutex_lock(&node_impl->mutex);
        // tt_RET_INTERRUPTED (another entry point below wants `mutex`, or rmw_destroy_node() is
        // stopping this thread) and tt_RET_TIMEOUT/tt_RET_OK (nothing due, or ordinary receive/
        // scheduler work) all just mean "loop back and check poll_thread_running again" here -
        // there's nothing this thread could usefully do differently for any other tt_Node_poll()
        // failure either, so no per-code special-casing beyond the loop condition itself.
        tt_Node_poll(&node_impl->tickle_node, RMW_TICKLE_POLL_TIMEOUT_NS);
        pthread_mutex_unlock(&node_impl->mutex);
    }
    return NULL;
}

rmw_node_t* rmw_create_node(rmw_context_t* context, const char* name, const char* namespace_) {
    if (NULL == context) {
        RMW_SET_ERROR_MSG("context is null");
        return NULL;
    }
    if (NULL == name) {
        RMW_SET_ERROR_MSG("name is null");
        return NULL;
    }
    if (NULL == namespace_) {
        RMW_SET_ERROR_MSG("namespace_ is null");
        return NULL;
    }
    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }

    bool already_created = false;
    if (!atomic_compare_exchange_strong(&g_tickle_node_created, &already_created, true)) {
        // rmw_tickle/PLAN.md's "Deferred: multiple ROS 2 nodes per process" - a component
        // container wanting several ROS nodes in one process needs one tt_Node (own socket, own
        // poll thread) per node, which this milestone doesn't build yet.
        RMW_SET_ERROR_MSG("rmw_tickle supports only one node per process for now");
        return NULL;
    }

    rcutils_allocator_t* allocator = &context->options.allocator;
    rmw_tickle_node_t* node_impl =
        (rmw_tickle_node_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_node_t), allocator->state);
    if (NULL == node_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_node_t");
        atomic_store(&g_tickle_node_created, false);
        return NULL;
    }
    node_impl->allocator = *allocator;
    node_impl->context = context;

    node_impl->rmw_node.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    node_impl->rmw_node.data = node_impl;
    node_impl->rmw_node.context = context;
    node_impl->rmw_node.name = rcutils_strdup(name, *allocator);
    node_impl->rmw_node.namespace_ = rcutils_strdup(namespace_, *allocator);
    if (NULL == node_impl->rmw_node.name || NULL == node_impl->rmw_node.namespace_) {
        RMW_SET_ERROR_MSG("failed to allocate node name/namespace");
        goto fail;
    }

    if (pthread_mutex_init(&node_impl->mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize node mutex");
        goto fail;
    }

    // tt_Node_create() reads its setup (node id, bind address, broadcast address) entirely from
    // the process-wide _tt_CONFIG (rmw_init()'s own TICKLE_BROADCAST_ADDR handling, or its
    // compiled-in defaults) - see tickle.h's "Lifetime / ownership" note. `name`/`namespace_`
    // above are rmw-level bookkeeping only; TickLE's own wire protocol has no node-name concept,
    // identifying a node purely by its numeric id.
    tt_ret_t ret = tt_Node_create(&node_impl->tickle_node);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create() failed");
        pthread_mutex_destroy(&node_impl->mutex);
        goto fail;
    }

    node_impl->poll_thread_running = true;
    if (pthread_create(&node_impl->poll_thread, NULL, poll_thread_main, node_impl) != 0) {
        RMW_SET_ERROR_MSG("failed to start poll thread");
        node_impl->poll_thread_running = false;
        tt_Node_destroy(&node_impl->tickle_node);
        pthread_mutex_destroy(&node_impl->mutex);
        goto fail;
    }

    return &node_impl->rmw_node;

fail:
    if (node_impl->rmw_node.name != NULL) {
        allocator->deallocate((char*)node_impl->rmw_node.name, allocator->state);
    }
    if (node_impl->rmw_node.namespace_ != NULL) {
        allocator->deallocate((char*)node_impl->rmw_node.namespace_, allocator->state);
    }
    allocator->deallocate(node_impl, allocator->state);
    atomic_store(&g_tickle_node_created, false);
    return NULL;
}

rmw_ret_t rmw_destroy_node(rmw_node_t* node) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rcutils_allocator_t allocator = node_impl->allocator;

    // Same tt_Node_interrupt()-then-lock pattern rmw_publish() et al. (Milestone 3+) will use -
    // see rmw_tickle.h's own rmw_tickle_node_t doc comment. poll_thread_running is set first so
    // the thread's own loop condition is already false by the time it next wakes, whichever of
    // the (possibly several) tt_Node_poll() calls the interrupt actually cuts short.
    node_impl->poll_thread_running = false;
    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_join(node_impl->poll_thread, NULL);

    tt_Node_destroy(&node_impl->tickle_node);
    pthread_mutex_destroy(&node_impl->mutex);

    allocator.deallocate((char*)node_impl->rmw_node.name, allocator.state);
    allocator.deallocate((char*)node_impl->rmw_node.namespace_, allocator.state);
    allocator.deallocate(node_impl, allocator.state);

    atomic_store(&g_tickle_node_created, false);
    return RMW_RET_OK;
}

const rmw_guard_condition_t* rmw_node_get_graph_guard_condition(const rmw_node_t* node) {
    if (NULL == node) {
        RMW_SET_ERROR_MSG("node is null");
        return NULL;
    }
    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)node->context->impl;
    return &context_impl->graph_guard_condition.rmw_guard_condition;
}
