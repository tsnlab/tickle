/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 3: rmw_create_publisher()/rmw_destroy_publisher()/rmw_publish().
// rmw_tickle_validate_qos_profile() (rmw_qos.c, Milestone 7) is what rejects anything this rmw
// doesn't support; rmw_create_publisher() below acts on the one policy that needs more than a
// yes/no - RELIABILITY (QoS roadmap #5) - by wiring a struct tt_ReliableCache into the Publisher.

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include <tickle/config.h> // tt_MAX_RELIABLE_HISTORY
#include <tickle/hal.h>    // tt_ret_t/tt_RET_OK
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

rmw_publisher_t* rmw_create_publisher(const rmw_node_t* node, const rosidl_message_type_support_t* type_support,
                                      const char* topic_name, const rmw_qos_profile_t* qos_profile,
                                      const rmw_publisher_options_t* publisher_options) {
    if (NULL == node) {
        RMW_SET_ERROR_MSG("node is null");
        return NULL;
    }
    if (NULL == topic_name) {
        RMW_SET_ERROR_MSG("topic_name is null");
        return NULL;
    }
    if (NULL == qos_profile) {
        RMW_SET_ERROR_MSG("qos_profile is null");
        return NULL;
    }
    if (NULL == publisher_options) {
        RMW_SET_ERROR_MSG("publisher_options is null");
        return NULL;
    }
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }
    if (rmw_tickle_validate_qos_profile(qos_profile, RMW_TICKLE_ENTITY_PUBLISHER) != RMW_RET_OK) {
        return NULL; // error message already set
    }

    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = rmw_tickle_get_message_callbacks(type_support);
    if (NULL == callbacks) {
        return NULL; // error message already set
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rcutils_allocator_t* allocator = &node_impl->allocator;

    rmw_tickle_publisher_t* pub_impl =
        (rmw_tickle_publisher_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_publisher_t), allocator->state);
    if (NULL == pub_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_publisher_t");
        return NULL;
    }
    pub_impl->node = node_impl;
    pub_impl->type_support = type_support;
    pub_impl->callbacks = callbacks;
    pub_impl->allocator = *allocator;
    pub_impl->qos = *qos_profile;

    // topic.name is callbacks->ros_type_name - a generated-code string literal, so it already
    // satisfies tickle.h's "Lifetime / ownership" rule (stay valid and unmoved until tt_Publisher_
    // destroy()) without needing its own copy - see rmw_tickle.h's own rmw_tickle_publisher_t.topic
    // doc comment.
    pub_impl->topic.name = callbacks->ros_type_name;
    pub_impl->topic.data_size = (uint32_t)callbacks->tickle_struct_size;
    pub_impl->topic.data_encode_size = callbacks->tickle_encode_size;
    pub_impl->topic.data_encode = callbacks->tickle_encode;
    pub_impl->topic.data_decode = callbacks->tickle_decode;
    pub_impl->topic.data_free = callbacks->tickle_free;
    // data_encode_inplace/data_decode_inplace stay NULL (zero_allocate) - no zero-copy support.

    pub_impl->rmw_publisher.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    pub_impl->rmw_publisher.data = pub_impl;
    pub_impl->rmw_publisher.topic_name = rcutils_strdup(topic_name, *allocator);
    pub_impl->rmw_publisher.options = *publisher_options;
    pub_impl->rmw_publisher.can_loan_messages = false;
    if (NULL == pub_impl->rmw_publisher.topic_name) {
        RMW_SET_ERROR_MSG("failed to allocate topic_name");
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }

    // Same tt_Node_interrupt()-then-lock pattern rmw_destroy_node() already established - see
    // rmw_tickle.h's own rmw_tickle_node_t doc comment for the full contract.
    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);
    tt_ret_t ret = tt_Node_create_publisher(&node_impl->tickle_node, &pub_impl->tickle_publisher, &pub_impl->topic,
                                            pub_impl->rmw_publisher.topic_name);
    pthread_mutex_unlock(&node_impl->mutex);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create_publisher() failed");
        allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }

    // QoS roadmap #5 (RELIABILITY) - see rmw_tickle_publisher_t.reliable_cache's own doc comment.
    // depth defaults to tt_MAX_RELIABLE_HISTORY (the largest this build supports) when unset
    // (RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT); an explicit depth past that cap is rejected outright
    // rather than silently clamped, matching this package's own "Rejects anything outside the
    // currently-supported set explicitly" design philosophy.
    if (RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability) {
        size_t depth = qos_profile->depth != RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT ? qos_profile->depth
                                                                                 : (size_t)tt_MAX_RELIABLE_HISTORY;
        if (depth > (size_t)tt_MAX_RELIABLE_HISTORY) {
            RMW_SET_ERROR_MSG("rmw_tickle's RELIABLE publisher can retain at most "
                              "tt_MAX_RELIABLE_HISTORY samples - see rmw_tickle/PLAN.md's QoS "
                              "roadmap #5 (RELIABILITY)");
            tt_Node_interrupt(&node_impl->tickle_node);
            pthread_mutex_lock(&node_impl->mutex);
            tt_Publisher_destroy(&pub_impl->tickle_publisher);
            pthread_mutex_unlock(&node_impl->mutex);
            allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
            allocator->deallocate(pub_impl, allocator->state);
            return NULL;
        }

        pub_impl->reliable_cache =
            (struct tt_ReliableCache*)allocator->zero_allocate(1, sizeof(struct tt_ReliableCache), allocator->state);
        if (NULL == pub_impl->reliable_cache) {
            RMW_SET_ERROR_MSG("failed to allocate reliable_cache");
            tt_Node_interrupt(&node_impl->tickle_node);
            pthread_mutex_lock(&node_impl->mutex);
            tt_Publisher_destroy(&pub_impl->tickle_publisher);
            pthread_mutex_unlock(&node_impl->mutex);
            allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
            allocator->deallocate(pub_impl, allocator->state);
            return NULL;
        }
        pub_impl->reliable_cache->depth = (uint16_t)depth;
        pub_impl->tickle_publisher.reliable_cache = pub_impl->reliable_cache;
    }

    return &pub_impl->rmw_publisher;
}

rmw_ret_t rmw_destroy_publisher(rmw_node_t* node, rmw_publisher_t* publisher) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier) ||
        !rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;

    tt_Node_interrupt(&pub_impl->node->tickle_node);
    pthread_mutex_lock(&pub_impl->node->mutex);
    tt_Publisher_destroy(&pub_impl->tickle_publisher);
    pthread_mutex_unlock(&pub_impl->node->mutex);

    rcutils_allocator_t allocator = pub_impl->allocator;
    allocator.deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator.state);
    allocator.deallocate(pub_impl->reliable_cache, allocator.state); // NULL is a no-op, see its own doc comment
    allocator.deallocate(pub_impl, allocator.state);
    return RMW_RET_OK;
}

rmw_ret_t rmw_publish(const rmw_publisher_t* publisher, const void* ros_message,
                      rmw_publisher_allocation_t* allocation) {
    (void)allocation; // pre-allocated-message optimization, not implemented
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = pub_impl->callbacks;

    void* tickle_buf = pub_impl->allocator.allocate(callbacks->tickle_struct_size, pub_impl->allocator.state);
    if (NULL == tickle_buf) {
        RMW_SET_ERROR_MSG("failed to allocate scratch TickLE struct");
        return RMW_RET_BAD_ALLOC;
    }

    if (!callbacks->to_tickle(ros_message, tickle_buf)) {
        // A bounds-check failure (a variable array/bounded string longer than TickLE's resolved
        // capacity) - see ros2_adapter.py's own emit_to_tickle() doc comment.
        RMW_SET_ERROR_MSG("failed to convert ROS message to TickLE wire struct (capacity exceeded?)");
        pub_impl->allocator.deallocate(tickle_buf, pub_impl->allocator.state);
        return RMW_RET_ERROR;
    }

    tt_Node_interrupt(&pub_impl->node->tickle_node);
    pthread_mutex_lock(&pub_impl->node->mutex);
    tt_ret_t ret = tt_Publisher_publish(&pub_impl->tickle_publisher, (struct tt_Data*)tickle_buf);
    pthread_mutex_unlock(&pub_impl->node->mutex);

    pub_impl->allocator.deallocate(tickle_buf, pub_impl->allocator.state);

    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Publisher_publish() failed");
        return RMW_RET_ERROR;
    }
    return RMW_RET_OK;
}

// A real rclcpp::Publisher construction (rcl_publisher_init()) calls this unconditionally, not
// just optionally/best-effort like most of the other rmw_publisher_*() extras - discovered while
// provisioning rmw_tickle/PLAN.md's rmw-perf.yml benchmark rig (the first time this rmw was
// exercised via a real rclcpp C++ node rather than this package's own C-level unit tests).
// rmw_tickle never negotiates/downgrades a requested QoS policy, so "actual" is always just
// whatever rmw_create_publisher() already validated and stored.
rmw_ret_t rmw_publisher_get_actual_qos(const rmw_publisher_t* publisher, rmw_qos_profile_t* qos) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    *qos = pub_impl->qos;
    return RMW_RET_OK;
}

// Another call rcl_publisher_init() makes unconditionally (via rclcpp::Publisher's own
// constructor, to populate every future rmw_message_info_t.publisher_gid it hands out) - see
// rmw_publisher_get_actual_qos()'s own doc comment for how this was found. tt_Publisher's own
// (node id, endpoint id) pair is already a unique-within-this-TickLE-network identity for this
// publisher (see tickle.h's own struct tt_Endpoint doc comment on .id) - zero-extended into
// gid.data's remaining bytes, matching rmw_subscription.c's own zeroed rmw_message_info_t.
// publisher_gid for a received message from the *sending* side's point of view instead.
rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t* publisher, rmw_gid_t* gid) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    memset(gid, 0, sizeof(*gid));
    gid->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    uint8_t node_id = pub_impl->node->tickle_node.id;
    uint32_t endpoint_id = pub_impl->tickle_publisher.endpoint.id;
    gid->data[0] = node_id;
    memcpy(&gid->data[1], &endpoint_id, sizeof(endpoint_id));
    return RMW_RET_OK;
}

// A real rclcpp::Publisher constructor calls this once per QoS event type NodeOptions/QoS asks
// for (offered_deadline_missed, liveliness_lost, ...) - rmw_tickle doesn't implement any of them
// yet (no deadline/liveliness/matched-count tracking - see rmw_qos.c's own QoS roadmap), and
// RMW_RET_UNSUPPORTED is this API's own documented way to say exactly that (rmw/event.h) - unlike
// most other not-yet-implemented rmw_*() extras, the *symbol* still has to exist (an unresolved
// dlsym is fatal to rclcpp here, a returned RMW_RET_UNSUPPORTED is not).
rmw_ret_t rmw_publisher_event_init(rmw_event_t* rmw_event, const rmw_publisher_t* publisher,
                                   rmw_event_type_t event_type) {
    (void)rmw_event;
    (void)event_type;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RMW_SET_ERROR_MSG("rmw_tickle does not support any publisher QoS events yet");
    return RMW_RET_UNSUPPORTED;
}

// Loaned (zero-copy) messages: rmw_publisher_t.can_loan_messages is always false (tt_Node_create_
// publisher() never sets it true - no shared-memory/zero-copy transport exists), and unlike most
// other not-yet-implemented rmw_*() extras, these three symbols still have to actually exist -
// same reasoning as rmw_publisher_event_init() just above (an unresolved dlsym is fatal to
// rmw_implementation's own dispatch, a returned RMW_RET_UNSUPPORTED is not). Found via test_rmw_
// implementation's own TestPublisherUseLoan fixture, which calls all three expecting exactly this
// return before GTEST_SKIP()-ing the rest of its own loan-specific test cases - a missing symbol
// crashed that fixture's SetUp() outright (a NULL function pointer call) instead of failing a
// single assertion.
rmw_ret_t rmw_borrow_loaned_message(const rmw_publisher_t* publisher, const rosidl_message_type_support_t* type_support,
                                    void** ros_message) {
    (void)type_support;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    *ros_message = NULL;
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_return_loaned_message_from_publisher(const rmw_publisher_t* publisher, void* loaned_message) {
    (void)loaned_message;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_publish_loaned_message(const rmw_publisher_t* publisher, void* ros_message,
                                     rmw_publisher_allocation_t* allocation) {
    (void)ros_message;
    (void)allocation;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}
