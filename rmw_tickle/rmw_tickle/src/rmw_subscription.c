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
// rmw_take()/rmw_take_with_info(). Milestone 7 added real QoS handling on top: rmw_tickle_
// validate_qos_profile() (rmw_qos.c) rejects anything the QoS roadmap (PLAN.md) hasn't implemented
// yet, and the surviving qos_profile->depth sizes the queue for real (see rmw_create_subscription()
// below), replacing the fixed placeholder capacity this milestone originally shipped with.

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/config.h> // tt_LIVELINESS_MISS_THRESHOLD, tt_NODE_UPDATE_INTERVAL
#include <tickle/hal.h>    // tt_ret_t/tt_RET_OK/tt_get_ns
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h" // rmw_time_point_value_t
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
// Wakes anyone blocked in rmw_wait() - shared by subscriber_callback() (a newly queued message),
// check_subscription_deadline() (a fresh deadline miss), and check_subscription_liveliness() (an
// alive_count change) - same wait_mutex/wait_cond broadcast pattern every one of them needs, for
// the identical reason (rmw_tickle_context_impl_t's own doc comment, rmw_tickle.h).
static void wake_wait_cond(rmw_tickle_node_t* node_impl) {
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)node_impl->context->impl;
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);
}

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

    // QoS roadmap #2 (DEADLINE) - see rmw_tickle_subscriber_t.last_activity_time's own doc
    // comment. This function already runs under node->mutex (its own module doc comment above),
    // the same lock check_subscription_deadline() reads this under - harmless to set even when
    // deadline_period_ns is 0 (unused in that case).
    sub_impl->last_activity_time = tt_get_ns();

    pthread_mutex_lock(&sub_impl->queue_mutex);
    if (sub_impl->queue_count == sub_impl->queue_capacity) {
        // KEEP_LAST behavior (rmw_tickle_validate_qos_profile() rejects KEEP_ALL - see rmw_tickle.h's
        // own queue doc comment) - drop the oldest queued message to make room for this one.
        rmw_tickle_queued_message_t* oldest = &sub_impl->queue[sub_impl->queue_head];
        sub_impl->allocator.deallocate(oldest->ros_message, sub_impl->allocator.state);
        sub_impl->queue_head = (sub_impl->queue_head + 1) % sub_impl->queue_capacity;
        sub_impl->queue_count--;
    }
    size_t tail_index = (sub_impl->queue_head + sub_impl->queue_count) % sub_impl->queue_capacity;
    sub_impl->queue[tail_index].ros_message = ros_message;
    sub_impl->queue[tail_index].source_timestamp =
        time; // publisher's own wire timestamp - see tickle.c's process_data()
    sub_impl->queue[tail_index].received_timestamp = tt_get_ns();
    sub_impl->queue[tail_index].publication_sequence_number = seq_no;
    sub_impl->queue[tail_index].reception_sequence_number = sub_impl->reception_sequence_number++;
    sub_impl->queue_count++;
    pthread_mutex_unlock(&sub_impl->queue_mutex);

    // Wake anyone blocked in rmw_wait() on this queue becoming non-empty - wake_wait_cond()'s own
    // doc comment explains why the broadcast must happen under wait_mutex even though queue_count
    // itself is guarded by the separate queue_mutex above.
    wake_wait_cond(sub_impl->node);
}

// QoS roadmap #2 (DEADLINE) - see rmw_publisher.c's own check_publisher_deadline() doc comment,
// same reasoning/threading, for a Subscription's REQUESTED_DEADLINE_MISSED instead.
static void check_subscription_deadline(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)param;
    if (time - sub_impl->last_activity_time >= sub_impl->deadline_period_ns) {
        atomic_fetch_add(&sub_impl->deadline_missed.total_count, 1);
        atomic_fetch_add(&sub_impl->deadline_missed.unread_count, 1);
        wake_wait_cond(sub_impl->node); // see its own doc comment
    }
    // Deadline monitoring simply stops here on a reschedule failure - see rmw_publisher.c's own
    // check_publisher_deadline() doc comment on this same pattern.
    (void)tt_Node_schedule(&sub_impl->node->tickle_node, time + sub_impl->deadline_period_ns,
                           check_subscription_deadline, sub_impl);
}

// QoS roadmap #3 (LIVELINESS) - RMW_EVENT_LIVELINESS_CHANGED's own periodic check, generalizing
// check_liveliness()'s (tickle.c) existing per-node peer-death detection into "how many
// Publishers on my topic are alive right now" (rmw_tickle_count_matching_locked(), rmw_graph.c).
// Fires from inside tt_Node_poll() - poll_thread already holds node->mutex (rmw_tickle_node_t's
// own doc comment) - so this calls the *_locked() variant directly, never count_matching()/
// rmw_count_publishers() (which take that same mutex themselves and would self-deadlock here -
// see rmw_tickle_count_matching_locked()'s own doc comment, rmw_tickle.h).
static void check_subscription_liveliness(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)param;
    size_t current = rmw_tickle_count_matching_locked(sub_impl->node, sub_impl->rmw_subscription.topic_name,
                                                      tt_KIND_TOPIC_PUBLISHER);
    // QoS roadmap #3 follow-up - the live not_alive_count snapshot (tombstoned Publishers on this
    // topic, struct tt_DiscoveredEntity.alive's own doc comment, tickle.h), independent of the
    // alive_count-delta-based not_alive.total_count/unread_count tracking just below (which stays
    // as-is - that's still the right way to derive "how many *went* not-alive", this is just the
    // separate "how many are *currently* not-alive" question rmw_liveliness_changed_status_t's own
    // not_alive_count field asks).
    size_t current_not_alive = rmw_tickle_count_not_alive_matching_locked(
        sub_impl->node, sub_impl->rmw_subscription.topic_name, tt_KIND_TOPIC_PUBLISHER);
    rmw_tickle_liveliness_changed_status_t* status = &sub_impl->liveliness_changed;
    if ((int)current > status->last_alive_count) {
        int delta = (int)current - status->last_alive_count;
        atomic_fetch_add(&status->alive.total_count, delta);
        atomic_fetch_add(&status->alive.unread_count, delta);
        wake_wait_cond(sub_impl->node);
    } else if ((int)current < status->last_alive_count) {
        int delta = status->last_alive_count - (int)current;
        atomic_fetch_add(&status->not_alive.total_count, delta);
        atomic_fetch_add(&status->not_alive.unread_count, delta);
        wake_wait_cond(sub_impl->node);
    }
    atomic_store(&status->alive_count, (int)current);
    atomic_store(&status->not_alive_count, (int)current_not_alive);
    status->last_alive_count = (int)current;

    // Liveliness monitoring simply stops here on a reschedule failure - same reasoning as check_
    // subscription_deadline()'s own identical pattern just above.
    (void)tt_Node_schedule(&sub_impl->node->tickle_node, time + sub_impl->liveliness_lease_ns,
                           check_subscription_liveliness, sub_impl);
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
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }
    if (rmw_tickle_validate_qos_profile(qos_profile, RMW_TICKLE_ENTITY_SUBSCRIPTION) != RMW_RET_OK) {
        return NULL; // error message already set
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
    sub_impl->qos = *qos_profile;

    sub_impl->topic.name = callbacks->ros_type_name; // see rmw_tickle_publisher_t.topic's own doc comment
    sub_impl->topic.data_size = (uint32_t)callbacks->tickle_struct_size;
    sub_impl->topic.data_encode_size = callbacks->tickle_encode_size;
    sub_impl->topic.data_encode = callbacks->tickle_encode;
    sub_impl->topic.data_decode = callbacks->tickle_decode;
    sub_impl->topic.data_free = callbacks->tickle_free;

    // Milestone 7's QoS roadmap item #1 (HISTORY/DEPTH): qos_profile->depth sizes the queue for
    // real, rather than a fixed compile-time bound - RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT (0, unset)
    // falls back to the previous placeholder default. rmw_tickle_validate_qos_profile() above
    // already rejected RMW_QOS_POLICY_HISTORY_KEEP_ALL (an unbounded queue), so `depth` is always
    // the real, finite capacity to allocate here.
    sub_impl->queue_capacity = qos_profile->depth != RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT
                                   ? qos_profile->depth
                                   : RMW_TICKLE_SUBSCRIPTION_QUEUE_DEFAULT_DEPTH;
    sub_impl->queue = (rmw_tickle_queued_message_t*)allocator->zero_allocate(
        sub_impl->queue_capacity, sizeof(rmw_tickle_queued_message_t), allocator->state);
    if (NULL == sub_impl->queue) {
        RMW_SET_ERROR_MSG("failed to allocate subscriber queue");
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    if (pthread_mutex_init(&sub_impl->queue_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize subscriber queue mutex");
        allocator->deallocate(sub_impl->queue, allocator->state);
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
        allocator->deallocate(sub_impl->queue, allocator->state);
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
        allocator->deallocate(sub_impl->queue, allocator->state);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    // QoS roadmap #5 (RELIABILITY) - see tt_Subscriber.reliable's own doc comment (tickle.h).
    // Plain field access, no allocation needed (unlike the Publisher side's reliable_cache) -
    // process_data()/update_reliable_ack() (tickle.c) track ack state directly on tickle_
    // subscriber itself.
    sub_impl->tickle_subscriber.reliable = RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability;

    // QoS roadmap #2 (DEADLINE) - see rmw_tickle_subscriber_t.deadline_period_ns's own doc
    // comment. rmw_qos.c already accepted any finite qos.deadline; RMW_QOS_DEADLINE_DEFAULT
    // ({0,0}) leaves deadline_period_ns at its zero_allocate() default (0 = not requested).
    rmw_duration_t deadline_ns = rmw_time_total_nsec(qos_profile->deadline);
    if (deadline_ns > 0) {
        sub_impl->deadline_period_ns = (uint64_t)deadline_ns;
        sub_impl->last_activity_time = tt_get_ns();
        tt_Node_interrupt(&node_impl->tickle_node);
        pthread_mutex_lock(&node_impl->mutex);
        // A failure here just leaves deadline monitoring inactive for this Subscription - see
        // rmw_publisher.c's own check_publisher_deadline() doc comment on this same pattern.
        (void)tt_Node_schedule(&node_impl->tickle_node, tt_get_ns() + sub_impl->deadline_period_ns,
                               check_subscription_deadline, sub_impl);
        pthread_mutex_unlock(&node_impl->mutex);
    }

    // QoS roadmap #6 (LIFESPAN) - see rmw_tickle_subscriber_t.lifespan_ns's own doc comment.
    // rmw_qos.c already accepted any finite qos.lifespan; RMW_QOS_LIFESPAN_DEFAULT ({0,0}) leaves
    // lifespan_ns at its zero_allocate() default (0 = not requested, no cost) - no scheduling
    // needed, unlike DEADLINE just above, since this is a plain age check rmw_take_with_info()
    // makes on demand rather than a periodic timer.
    rmw_duration_t lifespan_ns = rmw_time_total_nsec(qos_profile->lifespan);
    if (lifespan_ns > 0) {
        sub_impl->lifespan_ns = (uint64_t)lifespan_ns;
    }

    return &sub_impl->rmw_subscription;
}

rmw_ret_t rmw_destroy_subscription(rmw_node_t* node, rmw_subscription_t* subscription) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier) ||
        !rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;

    tt_Node_interrupt(&sub_impl->node->tickle_node);
    pthread_mutex_lock(&sub_impl->node->mutex);
    // QoS roadmap #2/#3 (DEADLINE/LIVELINESS) - cancel any still-armed check before the
    // subscriber it closes over is freed below; both are no-ops if never started (tt_Node_
    // unschedule() just finds nothing matching).
    if (sub_impl->deadline_period_ns != 0) {
        tt_Node_unschedule(&sub_impl->node->tickle_node, check_subscription_deadline, sub_impl);
    }
    if (sub_impl->liveliness_lease_ns != 0) {
        tt_Node_unschedule(&sub_impl->node->tickle_node, check_subscription_liveliness, sub_impl);
    }
    tt_Subscriber_destroy(&sub_impl->tickle_subscriber);
    pthread_mutex_unlock(&sub_impl->node->mutex);

    // Drain anything still queued - rmw_take() never got to these.
    pthread_mutex_lock(&sub_impl->queue_mutex);
    while (sub_impl->queue_count > 0) {
        sub_impl->allocator.deallocate(sub_impl->queue[sub_impl->queue_head].ros_message, sub_impl->allocator.state);
        sub_impl->queue_head = (sub_impl->queue_head + 1) % sub_impl->queue_capacity;
        sub_impl->queue_count--;
    }
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    pthread_mutex_destroy(&sub_impl->queue_mutex);

    rcutils_allocator_t allocator = sub_impl->allocator;
    allocator.deallocate((char*)sub_impl->rmw_subscription.topic_name, allocator.state);
    allocator.deallocate(sub_impl->queue, allocator.state);
    allocator.deallocate(sub_impl, allocator.state);
    return RMW_RET_OK;
}

rmw_ret_t rmw_take_with_info(const rmw_subscription_t* subscription, void* ros_message, bool* taken,
                             rmw_message_info_t* message_info, rmw_subscription_allocation_t* allocation) {
    (void)allocation; // pre-allocated-message optimization, not implemented
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;

    pthread_mutex_lock(&sub_impl->queue_mutex);
    // QoS roadmap #6 (LIFESPAN) - see rmw_tickle_subscriber_t.lifespan_ns's own doc comment. Drops
    // (not returns) any already-expired entries from the front before taking the real head - "as
    // if it had never been sent", same wording tickle.c's own reliable_cache-side skip uses. A
    // no-op loop when lifespan_ns == 0 (not requested).
    while (sub_impl->queue_count > 0 && sub_impl->lifespan_ns != 0 &&
           tt_get_ns() - sub_impl->queue[sub_impl->queue_head].source_timestamp >= sub_impl->lifespan_ns) {
        sub_impl->allocator.deallocate(sub_impl->queue[sub_impl->queue_head].ros_message, sub_impl->allocator.state);
        sub_impl->queue_head = (sub_impl->queue_head + 1) % sub_impl->queue_capacity;
        sub_impl->queue_count--;
    }
    if (sub_impl->queue_count == 0) {
        pthread_mutex_unlock(&sub_impl->queue_mutex);
        *taken = false;
        return RMW_RET_OK;
    }
    rmw_tickle_queued_message_t entry = sub_impl->queue[sub_impl->queue_head];
    sub_impl->queue_head = (sub_impl->queue_head + 1) % sub_impl->queue_capacity;
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

// See rmw_publisher.c's own rmw_publisher_get_actual_qos() doc comment - same reasoning.
rmw_ret_t rmw_subscription_get_actual_qos(const rmw_subscription_t* subscription, rmw_qos_profile_t* qos) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    *qos = sub_impl->qos;
    return RMW_RET_OK;
}

// See rmw_publisher.c's own rmw_publisher_event_init() doc comment - same reasoning, for
// subscription-side QoS events. QoS roadmap #2 (DEADLINE) and #3 (LIVELINESS) are done -
// RMW_EVENT_REQUESTED_DEADLINE_MISSED/RMW_EVENT_LIVELINESS_CHANGED are real, queryable events now
// (rmw_take_event(), rmw_event.c); anything else still returns RMW_RET_UNSUPPORTED.
rmw_ret_t rmw_subscription_event_init(rmw_event_t* rmw_event, const rmw_subscription_t* subscription,
                                      rmw_event_type_t event_type) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(rmw_event, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    switch (event_type) {
    case RMW_EVENT_REQUESTED_DEADLINE_MISSED:
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = sub_impl;
        rmw_event->event_type = event_type;
        return RMW_RET_OK;
    case RMW_EVENT_LIVELINESS_CHANGED:
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = sub_impl;
        rmw_event->event_type = event_type;
        // Lazy, idempotent start (see rmw_tickle_subscriber_t.liveliness_lease_ns's own doc
        // comment) - only the first rmw_subscription_event_init() call for this event type
        // actually arms the periodic check; a later one just rewires the same rmw_event_t.
        if (sub_impl->liveliness_lease_ns == 0) {
            rmw_duration_t lease_ns = rmw_time_total_nsec(sub_impl->qos.liveliness_lease_duration);
            sub_impl->liveliness_lease_ns =
                lease_ns > 0 ? (uint64_t)lease_ns : (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL;
            tt_Node_interrupt(&sub_impl->node->tickle_node);
            pthread_mutex_lock(&sub_impl->node->mutex);
            // A failure here just leaves liveliness monitoring inactive for this
            // Subscription - same reasoning as the deadline scheduling above.
            (void)tt_Node_schedule(&sub_impl->node->tickle_node, tt_get_ns() + sub_impl->liveliness_lease_ns,
                                   check_subscription_liveliness, sub_impl);
            pthread_mutex_unlock(&sub_impl->node->mutex);
        }
        return RMW_RET_OK;
    default:
        RMW_SET_ERROR_MSG("rmw_tickle does not support this subscription QoS event yet");
        return RMW_RET_UNSUPPORTED;
    }
}

// See rmw_publisher.c's own loaned-message stubs (rmw_borrow_loaned_message() et al.) doc comment
// - same reasoning, subscription side. Found the same way: test_rmw_implementation's own
// TestSubscriptionUseLoan fixture calls all three expecting RMW_RET_UNSUPPORTED before GTEST_
// SKIP()-ing the rest of its own loan-specific cases; a missing symbol crashed that fixture's
// SetUp() outright instead of failing a single assertion.
rmw_ret_t rmw_take_loaned_message(const rmw_subscription_t* subscription, void** loaned_message, bool* taken,
                                  rmw_subscription_allocation_t* allocation) {
    (void)allocation;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(loaned_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    *loaned_message = NULL;
    *taken = false;
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_take_loaned_message_with_info(const rmw_subscription_t* subscription, void** loaned_message, bool* taken,
                                            rmw_message_info_t* message_info,
                                            rmw_subscription_allocation_t* allocation) {
    (void)message_info;
    (void)allocation;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(loaned_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    *loaned_message = NULL;
    *taken = false;
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_return_loaned_message_from_subscription(const rmw_subscription_t* subscription, void* loaned_message) {
    (void)loaned_message;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}
