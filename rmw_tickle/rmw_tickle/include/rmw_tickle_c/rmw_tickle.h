/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rmw/init.h"  // rmw_context_t
#include "rmw/types.h" // rmw_node_t, rmw_publisher_t, rmw_subscription_t, rmw_client_t, rmw_service_t,
                       // rmw_guard_condition_t, rmw_wait_set_t
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"

#ifdef __cplusplus
extern "C" {
#endif

// TickLE RMW implementation identifier
#define RMW_TICKLE_IDENTIFIER "rmw_tickle"

// TickLE serialization format
#define RMW_TICKLE_SERIALIZATION_FORMAT "tickle"

// External identifier variables
extern const char* const rmw_tickle_identifier;
extern const char* const rmw_tickle_serialization_format;

// TickLE context implementation
typedef struct rmw_tickle_context_impl_t {
    // Graph guard condition for node discovery
    rmw_guard_condition_t graph_guard_condition;
    // Placeholder for TickLE context data
    // This can be extended with TickLE-specific context information
    int dummy; // Temporary field to avoid empty struct
} rmw_tickle_context_impl_t;

// TickLE specific node data
typedef struct rmw_tickle_node_t {
    rmw_node_t rmw_node; // RMW node structure (must be first)
    struct tt_Node tickle_node;
    rcutils_allocator_t allocator;
    const rmw_context_t* context; // Store context reference

    // rmw_tickle/PLAN.md's threading model ("TickLE" row: "Stays single-threaded per tt_Node...";
    // "rmw_tickle" row: "Owns all lock/thread management. A background thread per node drives
    // tt_Node_poll(); a per-node mutex serializes every other entry point (rmw_publish, ...)
    // against it"). poll_thread loops tt_Node_poll(&tickle_node, tt_RECEIVE_TIMEOUT), holding
    // `mutex` only around each individual call (not across iterations, and not while blocked in
    // the syscall underneath it for longer than that short timeout - see rmw_node.c) - every
    // other rmw_tickle_c entry point that touches tickle_node (rmw_publish() et al., Milestone
    // 3+) must tt_Node_interrupt(&tickle_node) *then* lock `mutex` before doing so, the same
    // pattern rmw_destroy_node() itself uses to stop poll_thread. tt_Node_interrupt() is the one
    // tt_Node_* call explicitly safe to make without holding `mutex` first (see tickle.h's own
    // doc comment on it).
    pthread_t poll_thread;
    pthread_mutex_t mutex;
    volatile bool poll_thread_running;
} rmw_tickle_node_t;

// TickLE specific publisher data
typedef struct rmw_tickle_publisher_t {
    rmw_publisher_t rmw_publisher; // RMW publisher structure (must be first)
    struct tt_Publisher tickle_publisher;
    rmw_tickle_node_t* node;
    const rosidl_message_type_support_t* type_support;
} rmw_tickle_publisher_t;

// TickLE specific subscriber data
typedef struct rmw_tickle_subscriber_t {
    rmw_subscription_t rmw_subscription; // RMW subscription structure (must be first)
    struct tt_Subscriber tickle_subscriber;
    rmw_tickle_node_t* node;
    const rosidl_message_type_support_t* type_support;
} rmw_tickle_subscriber_t;

// TickLE specific client data
typedef struct rmw_tickle_client_t {
    rmw_client_t rmw_client; // RMW client structure (must be first)
    struct tt_Client tickle_client;
    rmw_tickle_node_t* node;
    const rosidl_service_type_support_t* type_support;
} rmw_tickle_client_t;

// TickLE specific service data
typedef struct rmw_tickle_service_t {
    rmw_service_t rmw_service; // RMW service structure (must be first)
    struct tt_Server tickle_server;
    rmw_tickle_node_t* node;
    const rosidl_service_type_support_t* type_support;
} rmw_tickle_service_t;

// TickLE specific guard condition data
typedef struct rmw_tickle_guard_condition_t {
    rmw_guard_condition_t rmw_guard_condition; // RMW guard condition structure (must be first)
    bool has_triggered;
    rcutils_allocator_t allocator;
} rmw_tickle_guard_condition_t;

// TickLE specific wait set data
typedef struct rmw_tickle_wait_set_t {
    rmw_wait_set_t rmw_wait_set; // RMW wait set structure (must be first)
    rmw_tickle_guard_condition_t** guard_conditions;
    size_t guard_condition_count;
    rcutils_allocator_t allocator;
} rmw_tickle_wait_set_t;

#ifdef __cplusplus
}
#endif
