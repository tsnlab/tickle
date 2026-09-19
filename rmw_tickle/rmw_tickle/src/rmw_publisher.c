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
// doesn't support; rmw_create_publisher() below acts on the two policies that need more than a
// yes/no - RELIABILITY (QoS roadmap #5) and DURABILITY (QoS roadmap #4) - by wiring a shared
// struct tt_ReliableCache into the Publisher (one cache backs both, Milestone 24).

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h> // clock_gettime()/struct timespec/nanosleep() - rmw_publisher_wait_for_all_acked()

#include <tickle/config.h> // tt_MAX_RELIABLE_HISTORY, tt_MAX_PEER_COUNT, tt_CALL_RETRY_INTERVAL, tt_SECOND
#include <tickle/hal.h>    // tt_ret_t/tt_RET_OK, tt_get_ns()
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h" // rmw_time_total_nsec() - QoS roadmap #2 (DEADLINE)
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

// QoS roadmap #2 (DEADLINE) - runs once per pub_impl->deadline_period_ns (rescheduled
// unconditionally every time, a steady period, matching DDS's own "one miss per elapsed period
// with no write" semantics), checking whether rmw_publish() updated last_activity_time since the
// last check. Fires from inside tt_Node_poll() - poll_thread already holds context_impl->
// node_mutex around that whole call (rmw_tickle_context_impl_t's own doc comment) - so last_
// activity_time is safe to read here without a separate lock, same as rmw_publish() writing it
// under that same lock.
static void check_publisher_deadline(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)param;
    if (time - pub_impl->last_activity_time >= pub_impl->deadline_period_ns) {
        atomic_fetch_add(&pub_impl->deadline_missed.total_count, 1);
        atomic_fetch_add(&pub_impl->deadline_missed.unread_count, 1);
        // Wake anyone blocked in rmw_wait() on this event becoming ready - same wait_mutex/
        // wait_cond subscriber_callback() (rmw_subscription.c) already broadcasts on for a newly
        // queued message, for the identical reason (rmw_tickle_context_impl_t's own doc comment,
        // rmw_tickle.h).
        rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;
        pthread_mutex_lock(&context_impl->wait_mutex);
        pthread_cond_broadcast(&context_impl->wait_cond);
        pthread_mutex_unlock(&context_impl->wait_mutex);
    }
    // Deadline monitoring simply stops here on a reschedule failure (tt_MAX_SCHEDULER_LENGTH
    // exhausted) - no logging facility in this package to report it through, and no return path
    // out of a scheduled void callback anyway.
    (void)tt_Node_schedule(&pub_impl->node->context_impl->tickle_node, time + pub_impl->deadline_period_ns,
                           check_publisher_deadline, pub_impl);
}

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
    // Milestone 34 - see rmw_tickle_publisher_t.owning_node_name's own doc comment. Failure here
    // isn't fatal to publisher creation - just leaves attribution unavailable for this publisher,
    // matching the "not consumed by anything yet" scope that field's own doc comment describes;
    // nothing downstream depends on these being non-NULL today.
    pub_impl->owning_node_name = rcutils_strdup(node_impl->rmw_node.name, *allocator);
    pub_impl->owning_node_namespace = rcutils_strdup(node_impl->rmw_node.namespace_, *allocator);
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

    // Milestone 45 - see rmw_tickle_publisher_t.publish_scratch_buf's own doc comment. Allocated
    // once here (callbacks->tickle_struct_size is fixed for this Publisher's whole lifetime),
    // reused by every rmw_publish() call from here on instead of a fresh allocate() each time.
    pub_impl->publish_scratch_buf = allocator->allocate(callbacks->tickle_struct_size, allocator->state);
    if (NULL == pub_impl->publish_scratch_buf) {
        RMW_SET_ERROR_MSG("failed to allocate publish scratch buffer");
        allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }
    if (pthread_mutex_init(&pub_impl->publish_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize publisher publish_mutex");
        allocator->deallocate(pub_impl->publish_scratch_buf, allocator->state);
        allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }

    // Same tt_Node_interrupt()-then-lock pattern rmw_destroy_node() already established - see
    // rmw_tickle.h's own rmw_tickle_context_impl_t doc comment for the full contract.
    tt_Node_interrupt(&node_impl->context_impl->tickle_node);
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    tt_ret_t ret = tt_Node_create_publisher(&node_impl->context_impl->tickle_node, &pub_impl->tickle_publisher,
                                            &pub_impl->topic, pub_impl->rmw_publisher.topic_name);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create_publisher() failed");
        allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }

    // QoS roadmap #5 (RELIABILITY) / #4 (DURABILITY) - see rmw_tickle_publisher_t.reliable_cache's
    // own doc comment. One shared struct tt_ReliableCache backs both policies now (PLAN.md's
    // Milestone 24 - matches real DDS/RTPS's own single Writer History Cache), allocated once if
    // either is requested; depth defaults to tt_MAX_RELIABLE_HISTORY (the largest this build
    // supports, the only cap now - RELIABILITY and DURABILITY used to have independently-tunable,
    // independently-capped depths here) when unset (RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT); an
    // explicit depth past that cap is rejected outright rather than silently clamped, matching this
    // package's own "Rejects anything outside the currently-supported set explicitly" design
    // philosophy. ROS 2's own rmw_qos_profile_t.depth is a single shared field regardless - both
    // policies always read the exact same requested depth, so merging this into one allocation/one
    // check changes no observable behavior for a Publisher requesting just one of the two, and
    // fixes a real asymmetry for one requesting both: a depth between the old, smaller DURABILITY
    // cap and the old, larger RELIABILITY cap used to reject the whole publisher outright even
    // though RELIABILITY alone would have accepted it.
    if (RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability ||
        RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == qos_profile->durability) {
        size_t depth = qos_profile->depth != RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT ? qos_profile->depth
                                                                                 : (size_t)tt_MAX_RELIABLE_HISTORY;
        if (depth > (size_t)tt_MAX_RELIABLE_HISTORY) {
            RMW_SET_ERROR_MSG("rmw_tickle's RELIABLE/TRANSIENT_LOCAL publisher can retain at most "
                              "tt_MAX_RELIABLE_HISTORY samples - see rmw_tickle/PLAN.md's QoS "
                              "roadmap #4/#5");
            tt_Node_interrupt(&node_impl->context_impl->tickle_node);
            pthread_mutex_lock(&node_impl->context_impl->node_mutex);
            tt_Publisher_destroy(&pub_impl->tickle_publisher);
            pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
            allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
            allocator->deallocate(pub_impl, allocator->state);
            return NULL;
        }

        pub_impl->reliable_cache =
            (struct tt_ReliableCache*)allocator->zero_allocate(1, sizeof(struct tt_ReliableCache), allocator->state);
        if (NULL == pub_impl->reliable_cache) {
            RMW_SET_ERROR_MSG("failed to allocate reliable_cache");
            tt_Node_interrupt(&node_impl->context_impl->tickle_node);
            pthread_mutex_lock(&node_impl->context_impl->node_mutex);
            tt_Publisher_destroy(&pub_impl->tickle_publisher);
            pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
            allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
            allocator->deallocate(pub_impl, allocator->state);
            return NULL;
        }
        pub_impl->reliable_cache->depth = (uint16_t)depth;
        pub_impl->tickle_publisher.reliable_cache = pub_impl->reliable_cache;
        pub_impl->tickle_publisher.reliable = RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability;
        pub_impl->tickle_publisher.durable = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == qos_profile->durability;
    }

    // QoS roadmap #2 (DEADLINE) - see rmw_tickle_publisher_t.deadline_period_ns's own doc comment.
    // rmw_qos.c already accepted any finite qos.deadline; RMW_QOS_DEADLINE_DEFAULT ({0,0}) leaves
    // deadline_period_ns at its zero_allocate() default (0 = not requested, no cost).
    rmw_duration_t deadline_ns = rmw_time_total_nsec(qos_profile->deadline);
    if (deadline_ns > 0) {
        pub_impl->deadline_period_ns = (uint64_t)deadline_ns;
        pub_impl->last_activity_time = tt_get_ns();
        tt_Node_interrupt(&node_impl->context_impl->tickle_node);
        pthread_mutex_lock(&node_impl->context_impl->node_mutex);
        // A failure here (tt_MAX_SCHEDULER_LENGTH exhausted) just leaves deadline monitoring
        // inactive for this Publisher - no logging facility in this package to report it through.
        (void)tt_Node_schedule(&node_impl->context_impl->tickle_node, tt_get_ns() + pub_impl->deadline_period_ns,
                               check_publisher_deadline, pub_impl);
        pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    }

    // QoS roadmap #6 (LIFESPAN) - see tt_Publisher.lifespan_duration_ns's own doc comment
    // (tickle.h). rmw_qos.c already accepted any finite qos.lifespan; RMW_QOS_LIFESPAN_DEFAULT
    // ({0,0}) leaves lifespan_duration_ns at its zero_allocate() default (0 = not requested, no
    // cost) - a plain field TickLE core itself reads on demand (deliver_durability_backlog()/
    // process_acknack()), no rmw_tickle-side state or scheduling needed, unlike DEADLINE above.
    rmw_duration_t lifespan_ns = rmw_time_total_nsec(qos_profile->lifespan);
    if (lifespan_ns > 0) {
        pub_impl->tickle_publisher.lifespan_duration_ns = (uint64_t)lifespan_ns;
    }

    // QoS roadmap #3 (LIVELINESS) follow-up, Milestone 32 - MANUAL_BY_TOPIC. See rmw_tickle_
    // publisher_t.liveliness_lease_ns's own doc comment. AUTOMATIC (or SYSTEM_DEFAULT) leaves
    // liveliness_lease_ns at its zero_allocate() default (0), the disambiguating sentinel check_
    // liveliness_lost() (rmw_node.c) uses to pick the AUTOMATIC (node-wide poll_thread health)
    // path over the manual-lease one. Left at RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT ({0,0})
    // falls back to the same floor rmw_qos.c's own acceptance check already established, rather
    // than leaving liveliness_lease_ns at 0 too (which would be indistinguishable from AUTOMATIC
    // here).
    if (RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == qos_profile->liveliness) {
        rmw_duration_t lease_ns = rmw_time_total_nsec(qos_profile->liveliness_lease_duration);
        pub_impl->liveliness_lease_ns =
            lease_ns > 0 ? (uint64_t)lease_ns : (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL;
        atomic_store(&pub_impl->last_asserted_ns, tt_get_ns());
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

    tt_Node_interrupt(&pub_impl->node->context_impl->tickle_node);
    pthread_mutex_lock(&pub_impl->node->context_impl->node_mutex);
    // QoS roadmap #2 (DEADLINE) - cancel a still-armed check before the publisher it closes over
    // is freed below; a no-op if deadline_period_ns was never set (tt_Node_unschedule() just finds
    // nothing matching).
    if (pub_impl->deadline_period_ns != 0) {
        tt_Node_unschedule(&pub_impl->node->context_impl->tickle_node, check_publisher_deadline, pub_impl);
    }
    tt_Publisher_destroy(&pub_impl->tickle_publisher);
    pthread_mutex_unlock(&pub_impl->node->context_impl->node_mutex);

    pthread_mutex_destroy(&pub_impl->publish_mutex); // Milestone 45 - see its own doc comment

    rcutils_allocator_t allocator = pub_impl->allocator;
    allocator.deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator.state);
    allocator.deallocate(pub_impl->reliable_cache, allocator.state);   // NULL is a no-op, see its own doc comment
    allocator.deallocate(pub_impl->owning_node_name, allocator.state); // NULL is a no-op too (a failed strdup)
    allocator.deallocate(pub_impl->owning_node_namespace, allocator.state);
    allocator.deallocate(pub_impl->publish_scratch_buf, allocator.state); // Milestone 45
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

    // Milestone 45 - publish_scratch_buf's own doc comment (rmw_tickle.h): reused every call
    // instead of a fresh allocate()/deallocate() pair. Safe to reuse the instant tt_Publisher_
    // publish() below returns - it never retains a pointer to `data` past its own call (either
    // encodes straight from it inline, or memcpy()s the *encoded wire bytes* into reliable_cache,
    // tickle.c's own cache_reliable_sample() - never `data` itself), and every field to_tickle()
    // writes here either aliases the caller's own ros_message (a plain unbounded string, DESIGN.md's
    // "Strings" rule) or copies into a fixed in-place buffer - nothing this struct itself owns that
    // a second call's own to_tickle() would need to free first.
    pthread_mutex_lock(&pub_impl->publish_mutex);
    void* tickle_buf = pub_impl->publish_scratch_buf;

    if (!callbacks->to_tickle(ros_message, tickle_buf)) {
        // A bounds-check failure (a variable array/bounded string longer than TickLE's resolved
        // capacity) - see ros2_adapter.py's own emit_to_tickle() doc comment.
        RMW_SET_ERROR_MSG("failed to convert ROS message to TickLE wire struct (capacity exceeded?)");
        pthread_mutex_unlock(&pub_impl->publish_mutex);
        return RMW_RET_ERROR;
    }

    tt_Node_interrupt(&pub_impl->node->context_impl->tickle_node);
    pthread_mutex_lock(&pub_impl->node->context_impl->node_mutex);
    tt_ret_t ret = tt_Publisher_publish(&pub_impl->tickle_publisher, (struct tt_Data*)tickle_buf);
    // QoS roadmap #2 (DEADLINE) - see rmw_tickle_publisher_t.last_activity_time's own doc comment.
    // Under the same lock check_publisher_deadline() reads it under, harmless to set even when
    // deadline_period_ns is 0 (unused in that case).
    if (ret == tt_RET_OK) {
        pub_impl->last_activity_time = tt_get_ns();
    }
    pthread_mutex_unlock(&pub_impl->node->context_impl->node_mutex);
    pthread_mutex_unlock(&pub_impl->publish_mutex);

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
// rmw_publisher_get_actual_qos()'s own doc comment for how this was found. Milestone 47
// (rmw_tickle/PLAN.md) - (node id, endpoint id) alone is *not* a unique identity for this
// publisher: endpoint_id is a pure name hash, deliberately shared by any two Publishers with the
// same topic+endpoint name (tickle.h's own struct tt_Endpoint doc comment on .id) - the exact
// cross-instance identity gap that milestone root-caused and fixed at the wire level. entity_id
// (also on struct tt_Endpoint, assigned per-instance by TickLE core) is the real per-instance
// identity; using it here instead closes the identical latent gid-collision this rmw layer had -
// two co-existing Publisher instances of the same topic would otherwise report the same gid to
// rclcpp, which real DDS's own per-Writer GUID never does. Zero-extended into gid.data's remaining
// bytes, matching rmw_subscription.c's own zeroed rmw_message_info_t.publisher_gid for a received
// message from the *sending* side's point of view instead.
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
    uint8_t node_id = pub_impl->node->context_impl->tickle_node.id;
    uint32_t entity_id = pub_impl->tickle_publisher.endpoint.entity_id;
    gid->data[0] = node_id;
    memcpy(&gid->data[1], &entity_id, sizeof(entity_id));
    return RMW_RET_OK;
}

// A real rclcpp::Publisher constructor calls this once per QoS event type NodeOptions/QoS asks
// for (offered_deadline_missed, liveliness_lost, ...). QoS roadmap #2 (DEADLINE) and #3
// (LIVELINESS) are done - RMW_EVENT_OFFERED_DEADLINE_MISSED/RMW_EVENT_LIVELINESS_LOST are real,
// queryable events now (rmw_take_event(), rmw_event.c); everything else this rmw hasn't
// implemented still returns RMW_RET_UNSUPPORTED, this API's own documented way to say exactly
// that (rmw/event.h) - unlike most other not-yet-implemented rmw_*() extras, the *symbol* itself
// still has to exist (an unresolved dlsym is fatal to rclcpp here, a returned RMW_RET_UNSUPPORTED
// is not).
rmw_ret_t rmw_publisher_event_init(rmw_event_t* rmw_event, const rmw_publisher_t* publisher,
                                   rmw_event_type_t event_type) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(rmw_event, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    switch (event_type) {
    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
    case RMW_EVENT_LIVELINESS_LOST:
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = publisher->data;
        rmw_event->event_type = event_type;
        return RMW_RET_OK;
    default:
        RMW_SET_ERROR_MSG("rmw_tickle does not support this publisher QoS event yet");
        return RMW_RET_UNSUPPORTED;
    }
}

// QoS roadmap #3 (LIVELINESS) follow-up, Milestone 32 - MANUAL_BY_TOPIC (the only manual kind
// this rmw's own rmw_qos_policy_liveliness_t still defines - see rmw_qos.c's own doc comment on
// MANUAL_BY_PARTICIPANT/_BY_NODE having been removed from the real rmw spec). A no-op (RMW_RET_OK)
// for an AUTOMATIC Publisher (liveliness_lease_ns == 0) - matches real DDS, where asserting on an
// AUTOMATIC entity is harmless. For a manual one, a plain atomic store into last_asserted_ns (no
// lock needed - see its own doc comment, rmw_tickle.h) is the entire mechanism: MANUAL_BY_TOPIC's
// own defining trait is that this Publisher's lease is refreshed only by its own explicit
// assertion, independent of every other entity on the node.
rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t* publisher) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    if (pub_impl->liveliness_lease_ns != 0) {
        atomic_store(&pub_impl->last_asserted_ns, tt_get_ns());
    }
    return RMW_RET_OK;
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

// How often rmw_publisher_wait_for_all_acked() below re-solicits (tt_Publisher_request_ack(),
// tickle.h) and re-checks peer_ack_seq_no[] while waiting - reuses acknack_retry()'s own fallback
// cadence (tickle.c: `tt_RELIABLE_DEADLINE != 0 ? tt_RELIABLE_DEADLINE : tt_CALL_RETRY_INTERVAL`)
// rather than inventing a new arbitrary number: both are "how long to wait before assuming a
// RELIABLE round trip's own UDP datagram needs retrying," the exact same question.
#define RMW_TICKLE_WAIT_FOR_ACKED_POLL_INTERVAL_NS tt_CALL_RETRY_INTERVAL

// True once every currently-matched peer (pub->peers[]) has acked at least up through
// target_seq_no - peer_ack_seq_no[i] is "every seq_no below this was received" (struct tt_
// AckNackHeader's own doc comment, tickle.h), so target_seq_no itself counts as acked once that
// value is strictly greater than it. No currently-matched peers at all is vacuously true - nothing
// left to wait on, matching tt_Publisher_request_ack()'s own "nothing to solicit" no-op. Caller
// must already hold context_impl->node_mutex - reads pub->peers[]/peer_ack_seq_no[] directly, the
// same fields process_acknack() (tickle.c) updates from inside tt_Node_poll(), under that same lock.
static bool all_peers_acked_locked(const struct tt_Publisher* pub, uint32_t target_seq_no) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (pub->peers[i].node_id != tt_NODE_ID_INVALID && pub->peer_ack_seq_no[i] <= target_seq_no) {
            return false;
        }
    }
    return true;
}

// rmw_tickle/PLAN.md's remaining-rmw-API-surface backlog - previously missing as a symbol
// entirely (Milestone 15's own note), then an honest RMW_RET_UNSUPPORTED for RELIABLE (Milestone
// 33 - TickLE core's own ack bookkeeping ran the other way around: each reliable_sender-tracking
// Subscriber owned its own ack_seq_no/received_bitmap, tickle.h, but a Publisher only ever saw
// ACKNACKs reactively via process_acknack(), with no aggregated "which peers have fully caught
// up" view of its own). Now real: pub->peer_ack_seq_no[] (tickle.h) is that aggregation, and tt_
// Publisher_request_ack() (tickle.h)'s own solicited, response-required Heartbeat (tt_HEARTBEAT_
// FLAG_FINAL clear - real RTPS's own wait_for_acknowledgments() mechanism) is what elicits an
// ACKNACK even from an already-healthy peer that would otherwise never send one at all (see that
// function's own doc comment). BEST_EFFORT has nothing to guarantee (matches the real spec: no
// delivery acknowledgment exists for BEST_EFFORT at all) - returns immediately, same as before.
//
// wait_timeout's own {0,0} means non-blocking poll-once (matches rcl_publisher_wait_for_all_
// acked()'s own explicit "timeout is 0 -> non-blocking" doc comment, rcl/publisher.h - the rmw
// layer's own rmw/rmw.h doesn't repeat that detail itself, but rmw_wait()'s own identical {0,0}
// convention, rmw_wait_set.c, is the same idea one layer down); any other value waits up to that
// duration, polling on RMW_TICKLE_WAIT_FOR_ACKED_POLL_INTERVAL_NS's own cadence rather than a
// condition-variable wait - deliberately not context_impl->wait_cond/wait_mutex: nothing broadcasts
// it on an ACKNACK arrival specifically (unlike a new subscriber-queue message or a triggered
// guard condition), and holding wait_mutex across a nested context_impl->node_mutex acquisition
// here would invert the lock order every other broadcast site depends on (poll_thread's own
// callbacks, e.g. check_publisher_deadline() above, already lock node_mutex first, wait_mutex
// second - see rmw_tickle_context_impl_t's own doc comment, rmw_tickle.h) - a bounded poll avoids
// that risk entirely, at the cost of up to one poll interval of extra latency once truly acked,
// same trade-off watchdog_thread_main() (rmw_node.c) already accepts for its own periodic checks.
rmw_ret_t rmw_publisher_wait_for_all_acked(const rmw_publisher_t* publisher, rmw_time_t wait_timeout) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    if (!pub_impl->tickle_publisher.reliable) {
        return RMW_RET_OK;
    }

    rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;

    bool poll_only = 0 == wait_timeout.sec && 0 == wait_timeout.nsec;
    struct timespec deadline;
    if (!poll_only) {
        clock_gettime(CLOCK_REALTIME, &deadline); // NOLINT(misc-include-cleaner) - see rmw_wait_set.c's own comment
        deadline.tv_sec += (time_t)wait_timeout.sec;
        deadline.tv_nsec += (long)wait_timeout.nsec;
        deadline.tv_sec += deadline.tv_nsec / (long)tt_SECOND;
        deadline.tv_nsec %= (long)tt_SECOND;
    }

    while (true) {
        tt_Node_interrupt(&context_impl->tickle_node);
        pthread_mutex_lock(&context_impl->node_mutex);
        // pub_impl->tickle_publisher.seq_no - the most recently published sample as of *this*
        // check, not re-read on every loop iteration below: a concurrent rmw_publish() growing it
        // mid-wait must not move this call's own target (matches real DDS - this only ever waits
        // for what was already written when called).
        uint32_t target_seq_no = pub_impl->tickle_publisher.seq_no;
        bool acked = target_seq_no == 0 || all_peers_acked_locked(&pub_impl->tickle_publisher, target_seq_no);
        if (!acked) {
            tt_Publisher_request_ack(&pub_impl->tickle_publisher);
        }
        pthread_mutex_unlock(&context_impl->node_mutex);

        if (acked) {
            return RMW_RET_OK;
        }
        if (poll_only) {
            return RMW_RET_TIMEOUT;
        }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now); // NOLINT(misc-include-cleaner)
        if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            return RMW_RET_TIMEOUT;
        }

        struct timespec sleep_duration = {.tv_sec = 0, .tv_nsec = (long)RMW_TICKLE_WAIT_FOR_ACKED_POLL_INTERVAL_NS};
        nanosleep(&sleep_duration, NULL);
    }
}
