/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 5: rmw_create_guard_condition()/rmw_destroy_guard_condition()/
// rmw_trigger_guard_condition(). A guard condition is a plain, application-triggerable wakeup for
// rmw_wait() - rclcpp uses one internally to interrupt a blocked spin (e.g. when a new entity is
// added to the executor), and rmw_init()'s own graph_guard_condition (rmw_node_get_graph_guard_
// condition()) reuses this exact struct shape so rmw_wait() treats both identically; see rmw_
// tickle_context_impl_t's own doc comment for why *triggering* the graph one on real graph changes
// is deferred to Milestone 6.

#include <pthread.h> // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own <pthread.h> comment
#include <stdatomic.h>
#include <stdbool.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rmw/error_handling.h"
#include "rmw/init.h" // rmw_context_t
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_guard_condition_t* rmw_create_guard_condition(rmw_context_t* context) {
    if (NULL == context) {
        RMW_SET_ERROR_MSG("context is null");
        return NULL;
    }
    if (!rmw_tickle_identifier_matches(context->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }

    rcutils_allocator_t* allocator = &context->options.allocator;
    rmw_tickle_guard_condition_t* guard = (rmw_tickle_guard_condition_t*)allocator->zero_allocate(
        1, sizeof(rmw_tickle_guard_condition_t), allocator->state);
    if (NULL == guard) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_guard_condition_t");
        return NULL;
    }
    guard->context_impl = (rmw_tickle_context_impl_t*)context->impl;
    atomic_init(&guard->has_triggered, false);
    guard->allocator = *allocator;

    guard->rmw_guard_condition.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    guard->rmw_guard_condition.data = guard;
    guard->rmw_guard_condition.context = context;
    return &guard->rmw_guard_condition;
}

rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t* guard_condition) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(guard_condition, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(guard_condition->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_guard_condition_t* guard = (rmw_tickle_guard_condition_t*)guard_condition->data;
    rcutils_allocator_t allocator = guard->allocator;
    allocator.deallocate(guard, allocator.state);
    return RMW_RET_OK;
}

rmw_ret_t rmw_trigger_guard_condition(const rmw_guard_condition_t* guard_condition) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(guard_condition, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(guard_condition->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_guard_condition_t* guard = (rmw_tickle_guard_condition_t*)guard_condition->data;
    atomic_store(&guard->has_triggered, true);

    // Wake anyone blocked in rmw_wait() on this context - see rmw_tickle_context_impl_t's own
    // doc comment for why the broadcast must happen while holding wait_mutex, even though
    // has_triggered itself is atomic and needs no lock of its own.
    pthread_mutex_lock(&guard->context_impl->wait_mutex);
    pthread_cond_broadcast(&guard->context_impl->wait_cond);
    pthread_mutex_unlock(&guard->context_impl->wait_mutex);
    return RMW_RET_OK;
}
