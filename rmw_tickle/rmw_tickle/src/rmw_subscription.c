/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 3: rmw_create_subscription()/rmw_destroy_subscription()/
// rmw_take()/rmw_take_with_info(). QoS (qos_profile) isn't validated or acted on yet beyond a
// fixed placeholder queue depth (RMW_TICKLE_SUBSCRIPTION_QUEUE_CAPACITY, rmw_tickle.h) -
// Milestone 7's QoS roadmap is the explicit-rejection/real-depth work, not this milestone.

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/hal.h> // tt_ret_t/tt_RET_OK/tt_get_ns
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

// Runs on the node's poll thread, inside tt_Node_poll() (see rmw_node.c) - node->mutex is already
// held by the caller. `data` (topic->data_size bytes, decoded by TickLE) may have string/array
// fields aliasing node->rx_buffer (DESIGN.md's "Strings" rule: TickLE's own decode() never
// copies), valid only until this function returns - topic->data_free() runs right after. So the
// tickle->ros conversion has to happen *here*, synchronously, not deferred to whenever rmw_take()
// is next called: callbacks->from_tickle() is what actually performs the deep copy (rosidl_
// runtime_c__String__assign() et al. - see ros2_adapter.py's own emit_from_tickle() doc comment),
// producing a ROS message independently owned from that point on, safe to queue for later.
static void subscriber_callback(struct tt_Subscriber* tt_sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    rmw_tickle_subscriber_t* sub_impl =
        (rmw_tickle_subscriber_t*)((char*)tt_sub - offsetof(rmw_tickle_subscriber_t, tickle_subscriber));
    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = sub_impl->callbacks;

    void* ros_message = sub_impl->allocator.zero_allocate(1, callbacks->ros_struct_size, sub_impl->allocator.state);
    if (NULL == ros_message) {
        return; // Nothing more useful to do from inside a poll-thread callback - drop silently.
    }
    if (!callbacks->from_tickle(data, ros_message)) {
        sub_impl->allocator.deallocate(ros_message, sub_impl->allocator.state);
        return;
    }

    pthread_mutex_lock(&sub_impl->queue_mutex);
    if (sub_impl->queue_count == RMW_TICKLE_SUBSCRIPTION_QUEUE_CAPACITY) {
        // Placeholder KEEP_LAST behavior (see rmw_tickle.h's own queue doc comment) - drop the
        // oldest queued message to make room for this one.
        rmw_tickle_queued_message_t* oldest = &sub_impl->queue[sub_impl->queue_head];
        sub_impl->allocator.deallocate(oldest->ros_message, sub_impl->allocator.state);
        sub_impl->queue_head = (sub_impl->queue_head + 1) % RMW_TICKLE_SUBSCRIPTION_QUEUE_CAPACITY;
        sub_impl->queue_count--;
    }
    size_t tail_index = (sub_impl->queue_head + sub_impl->queue_count) % RMW_TICKLE_SUBSCRIPTION_QUEUE_CAPACITY;
    sub_impl->queue[tail_index].ros_message = ros_message;
    sub_impl->queue[tail_index].source_timestamp =
        time; // publisher's own wire timestamp - see tickle.c's process_data()
    sub_impl->queue[tail_index].received_timestamp = tt_get_ns();
    sub_impl->queue[tail_index].publication_sequence_number = seq_no;
    sub_impl->queue[tail_index].reception_sequence_number = sub_impl->reception_sequence_number++;
    sub_impl->queue_count++;
    pthread_mutex_unlock(&sub_impl->queue_mutex);
}

rmw_subscription_t* rmw_create_subscription(const rmw_node_t* node, const rosidl_message_type_support_t* type_support,
                                            const char* topic_name, const rmw_qos_profile_t* qos_profile,
                                            const rmw_subscription_options_t* subscription_options) {
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
    if (NULL == subscription_options) {
        RMW_SET_ERROR_MSG("subscription_options is null");
        return NULL;
    }
    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }

    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = rmw_tickle_get_message_callbacks(type_support);
    if (NULL == callbacks) {
        return NULL; // error message already set
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rcutils_allocator_t* allocator = &node_impl->allocator;

    rmw_tickle_subscriber_t* sub_impl =
        (rmw_tickle_subscriber_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_subscriber_t), allocator->state);
    if (NULL == sub_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_subscriber_t");
        return NULL;
    }
    sub_impl->node = node_impl;
    sub_impl->type_support = type_support;
    sub_impl->callbacks = callbacks;
    sub_impl->allocator = *allocator;

    sub_impl->topic.name = callbacks->ros_type_name; // see rmw_tickle_publisher_t.topic's own doc comment
    sub_impl->topic.data_size = (uint32_t)callbacks->tickle_struct_size;
    sub_impl->topic.data_encode_size = callbacks->tickle_encode_size;
    sub_impl->topic.data_encode = callbacks->tickle_encode;
    sub_impl->topic.data_decode = callbacks->tickle_decode;
    sub_impl->topic.data_free = callbacks->tickle_free;

    if (pthread_mutex_init(&sub_impl->queue_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize subscriber queue mutex");
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    sub_impl->rmw_subscription.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    sub_impl->rmw_subscription.data = sub_impl;
    sub_impl->rmw_subscription.topic_name = rcutils_strdup(topic_name, *allocator);
    sub_impl->rmw_subscription.options = *subscription_options;
    sub_impl->rmw_subscription.can_loan_messages = false;
    sub_impl->rmw_subscription.is_cft_enabled = false;
    if (NULL == sub_impl->rmw_subscription.topic_name) {
        RMW_SET_ERROR_MSG("failed to allocate topic_name");
        pthread_mutex_destroy(&sub_impl->queue_mutex);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);
    tt_ret_t ret = tt_Node_create_subscriber(&node_impl->tickle_node, &sub_impl->tickle_subscriber, &sub_impl->topic,
                                             sub_impl->rmw_subscription.topic_name, subscriber_callback);
    pthread_mutex_unlock(&node_impl->mutex);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create_subscriber() failed");
        pthread_mutex_destroy(&sub_impl->queue_mutex);
        allocator->deallocate((char*)sub_impl->rmw_subscription.topic_name, allocator->state);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    return &sub_impl->rmw_subscription;
}

rmw_ret_t rmw_destroy_subscription(rmw_node_t* node, rmw_subscription_t* subscription) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0 ||
        strcmp(subscription->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;

    tt_Node_interrupt(&sub_impl->node->tickle_node);
    pthread_mutex_lock(&sub_impl->node->mutex);
    tt_Subscriber_destroy(&sub_impl->tickle_subscriber);
    pthread_mutex_unlock(&sub_impl->node->mutex);

    // Drain anything still queued - rmw_take() never got to these.
    pthread_mutex_lock(&sub_impl->queue_mutex);
    while (sub_impl->queue_count > 0) {
        sub_impl->allocator.deallocate(sub_impl->queue[sub_impl->queue_head].ros_message, sub_impl->allocator.state);
        sub_impl->queue_head = (sub_impl->queue_head + 1) % RMW_TICKLE_SUBSCRIPTION_QUEUE_CAPACITY;
        sub_impl->queue_count--;
    }
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    pthread_mutex_destroy(&sub_impl->queue_mutex);

    rcutils_allocator_t allocator = sub_impl->allocator;
    allocator.deallocate((char*)sub_impl->rmw_subscription.topic_name, allocator.state);
    allocator.deallocate(sub_impl, allocator.state);
    return RMW_RET_OK;
}

rmw_ret_t rmw_take_with_info(const rmw_subscription_t* subscription, void* ros_message, bool* taken,
                             rmw_message_info_t* message_info, rmw_subscription_allocation_t* allocation) {
    (void)allocation; // pre-allocated-message optimization, not implemented
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (strcmp(subscription->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;

    pthread_mutex_lock(&sub_impl->queue_mutex);
    if (sub_impl->queue_count == 0) {
        pthread_mutex_unlock(&sub_impl->queue_mutex);
        *taken = false;
        return RMW_RET_OK;
    }
    rmw_tickle_queued_message_t entry = sub_impl->queue[sub_impl->queue_head];
    sub_impl->queue_head = (sub_impl->queue_head + 1) % RMW_TICKLE_SUBSCRIPTION_QUEUE_CAPACITY;
    sub_impl->queue_count--;
    pthread_mutex_unlock(&sub_impl->queue_mutex);

    // Shallow copy: entry.ros_message's own string/array fields (rosidl_runtime_c__String et al.)
    // were heap-allocated by subscriber_callback()'s from_tickle() call and are transferred here
    // as-is (the pointers, not their backing buffers) - ros_message now owns them, and only the
    // outer entry.ros_message shell itself (not its fields) gets freed below.
    //
    // Known limitation: this assumes `ros_message` is a fresh/blank buffer (matching rclcpp's own
    // typical std::allocate_shared<MessageT>() per rmw_take() call for non-loaned subscriptions,
    // the only kind this rmw currently creates - can_loan_messages is always false, see rmw_
    // create_subscription()) - overwriting an *already populated* ros_message's own owned string/
    // array fields this way would leak their old backing buffers rather than fini() them first.
    // Fixing that generally needs a per-message __fini() function pointer rosidl_typesupport_
    // tickle_c doesn't generate yet - tracked as follow-on work, not solved here.
    memcpy(ros_message, entry.ros_message, sub_impl->callbacks->ros_struct_size);
    sub_impl->allocator.deallocate(entry.ros_message, sub_impl->allocator.state);
    *taken = true;

    if (NULL != message_info) {
        message_info->source_timestamp = (rmw_time_point_value_t)entry.source_timestamp;
        message_info->received_timestamp = (rmw_time_point_value_t)entry.received_timestamp;
        message_info->publication_sequence_number = entry.publication_sequence_number;
        message_info->reception_sequence_number = entry.reception_sequence_number;
        memset(&message_info->publisher_gid, 0, sizeof(message_info->publisher_gid));
        message_info->publisher_gid.implementation_identifier = RMW_TICKLE_IDENTIFIER;
        message_info->from_intra_process = false;
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_take(const rmw_subscription_t* subscription, void* ros_message, bool* taken,
                   rmw_subscription_allocation_t* allocation) {
    return rmw_take_with_info(subscription, ros_message, taken, NULL, allocation);
}
