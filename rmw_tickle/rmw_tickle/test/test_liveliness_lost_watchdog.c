/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS roadmap #3 (LIVELINESS) follow-up - Milestone 30, implementing Milestone 28(b)'s own design
// sketch for RMW_EVENT_LIVELINESS_LOST: a watchdog thread (rmw_node.c) independent of poll_thread
// notices when poll_thread has stopped making progress and marks every live Publisher sharing that
// context LIVELINESS_LOST. Unlike test_events.c's own black-box event-plumbing smoke tests, proving
// this actually fires needs a real, genuine stall - not something reachable through the public rmw
// API alone - so this file reaches into rmw_tickle_c/rmw_tickle.h directly (a legitimate public
// header of this package, not a private implementation detail) to hold node_impl->context_impl->
// node_mutex from this test's own thread for longer than the watchdog's own stale threshold, the
// same mutex poll_thread's own loop (rmw_node.c's poll_thread_main()) needs to reacquire before it
// can call tt_Node_poll() again - a real, if externally-induced, stall of poll_thread's own
// progress, not a forged timestamp standing in for one.

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h> // struct timespec/nanosleep/time_t - the mutex-hold hang simulation below

#include <tickle/config.h> // tt_LIVELINESS_MISS_THRESHOLD, tt_NODE_UPDATE_INTERVAL, tt_SECOND
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/event.h"
#include "rmw/events_statuses/liveliness_lost.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

struct fake_ros_msg {
    uint8_t value;
};
struct fake_tickle_msg {
    uint8_t value;
};

static bool fake_to_tickle(const void* ros_message, void* tickle_message) {
    ((struct fake_tickle_msg*)tickle_message)->value = ((const struct fake_ros_msg*)ros_message)->value;
    return true;
}
static bool fake_from_tickle(const void* tickle_message, void* ros_message) {
    ((struct fake_ros_msg*)ros_message)->value = ((const struct fake_tickle_msg*)tickle_message)->value;
    return true;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint8_t);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    payload[0] = ((struct fake_tickle_msg*)data)->value;
    return (int32_t)sizeof(uint8_t);
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)len;
    (void)is_native_endian;
    ((struct fake_tickle_msg*)data)->value = payload[0];
    return (int32_t)sizeof(uint8_t);
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

// Same fake type support shape as test_events.c's own - see its own doc comment on fake_type_
// support() for why this is hand-built rather than a real generated one.
static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "test_liveliness_lost_watchdog/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

static rosidl_message_type_support_t fake_handle = {
    .data = &fake_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* fake_type_support(void) {
    if (NULL == fake_handle.typesupport_identifier) {
        fake_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    }
    return &fake_handle;
}

#define NS_PER_MS (1000L * 1000L)
#define SANITY_CHECK_TIMEOUT_MS 100

static rmw_qos_profile_t base_qos(void) {
    rmw_qos_profile_t qos;
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = 1;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    qos.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    qos.avoid_ros_namespace_conventions = false;
    return qos;
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_liveliness_lost_watchdog", "/");
    assert(NULL != node);

    rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 0);
    assert(NULL != wait_set);

    rmw_qos_profile_t pub_qos = base_qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, fake_type_support(), "watchdog_topic", &pub_qos, &pub_opts);
    assert(NULL != pub);

    rmw_event_t liveliness_lost_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&liveliness_lost_event, pub, RMW_EVENT_LIVELINESS_LOST));

    // Sanity check: a perfectly healthy node (nothing has stalled yet) must not report a spurious
    // loss - rmw_wait() times out on this short wait with nothing ready (RMW_RET_TIMEOUT, not
    // RMW_RET_OK - that's this call's own "nothing became ready before the deadline" result).
    void* events_storage[1] = {&liveliness_lost_event};
    rmw_events_t events = {.event_count = 1, .events = events_storage};
    rmw_time_t short_timeout = {0, (uint64_t)SANITY_CHECK_TIMEOUT_MS * NS_PER_MS};
    assert(RMW_RET_TIMEOUT == rmw_wait(NULL, NULL, NULL, NULL, &events, wait_set, &short_timeout));
    assert(NULL == events.events[0]); // rmw_wait() nulls out anything not ready

    // Induce a real stall: hold node_impl->context_impl->node_mutex (rmw_tickle_c/rmw_tickle.h) from this thread for
    // longer than the watchdog's own stale threshold - poll_thread_main() (rmw_node.c) needs this
    // exact mutex before it can call tt_Node_poll() again, so holding it externally genuinely
    // blocks poll_thread's own progress, the same way an unrelated bug wedging *anything* that
    // holds this mutex too long would. tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL (3
    // real seconds today) is the exact floor rmw_node.c's own RMW_TICKLE_WATCHDOG_STALE_
    // THRESHOLD_NS uses (not exported as its own symbol - it's a translation-unit-local #define -
    // so this recomputes it from the same two public tickle/config.h constants that back it,
    // rather than hardcoding "3 seconds" and silently drifting from it if either ever changes).
    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    uint64_t stale_threshold_ns = (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL;
    uint64_t hang_duration_ns = stale_threshold_ns + tt_SECOND; // +1s margin over the bare floor

    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    struct timespec hang_duration = {
        .tv_sec = (time_t)(hang_duration_ns / tt_SECOND),
        .tv_nsec = (long)(hang_duration_ns % tt_SECOND),
    };
    nanosleep(&hang_duration, NULL);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);

    // The watchdog may have been blocked on this same mutex (inside mark_automatic_publishers_
    // lost(), rmw_node.c) waiting for the unlock() just above - rmw_wait() with a real timeout (rather
    // than immediately calling rmw_take_event()) gives it room to finish that call and broadcast
    // its own wait_cond, without this test racing a fixed short sleep against it.
    rmw_time_t recovery_timeout = {2, 0}; // generous: only needs to cover scheduling latency
    events.events[0] = &liveliness_lost_event;
    assert(RMW_RET_OK == rmw_wait(NULL, NULL, NULL, NULL, &events, wait_set, &recovery_timeout));
    assert(NULL != events.events[0]);

    rmw_liveliness_lost_status_t lost_status;
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_event(&liveliness_lost_event, &lost_status, &taken));
    assert(taken);
    assert(lost_status.total_count >= 1);
    assert(lost_status.total_count_change >= 1);

    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

    // QoS roadmap #3 (LIVELINESS) follow-up, Milestone 32 - MANUAL_BY_TOPIC. Unlike the AUTOMATIC
    // case above, this doesn't need any node_impl->context_impl->node_mutex trickery at all: check_manual_publishers_
    // lost() (rmw_node.c) checks each manual Publisher's own last_asserted_ns independently of
    // poll_thread's health, so a plain wait (no assertion at all) past the lease is a real,
    // unforced test of the actual obligation this QoS kind imposes.
    rmw_qos_profile_t manual_qos = base_qos();
    manual_qos.liveliness = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC;
    // rmw_qos.c's own floor - the shortest lease this rmw accepts, same reasoning as the AUTOMATIC
    // threshold above (recomputed, not hardcoded, for the same "don't silently drift" reason).
    uint64_t manual_lease_ns = (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL;
    manual_qos.liveliness_lease_duration.sec = manual_lease_ns / tt_SECOND;
    manual_qos.liveliness_lease_duration.nsec = manual_lease_ns % tt_SECOND;

    rmw_publisher_t* neglected_pub =
        rmw_create_publisher(node, fake_type_support(), "neglected_topic", &manual_qos, &pub_opts);
    assert(NULL != neglected_pub);
    rmw_event_t neglected_lost_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&neglected_lost_event, neglected_pub, RMW_EVENT_LIVELINESS_LOST));

    // Never asserted at all - must go LIVELINESS_LOST once its own lease elapses, entirely on its
    // own schedule (no external stall needed, unlike AUTOMATIC's own node-wide check above).
    rmw_time_t neglected_timeout = {
        .sec = (manual_lease_ns + tt_SECOND) / tt_SECOND, // +1s margin over the bare floor
        .nsec = (manual_lease_ns + tt_SECOND) % tt_SECOND,
    };
    void* neglected_events_storage[1] = {&neglected_lost_event};
    rmw_events_t neglected_events = {.event_count = 1, .events = neglected_events_storage};
    assert(RMW_RET_OK == rmw_wait(NULL, NULL, NULL, NULL, &neglected_events, wait_set, &neglected_timeout));
    assert(NULL != neglected_events.events[0]);

    rmw_liveliness_lost_status_t neglected_status;
    bool neglected_taken = false;
    assert(RMW_RET_OK == rmw_take_event(&neglected_lost_event, &neglected_status, &neglected_taken));
    assert(neglected_taken);
    assert(neglected_status.total_count >= 1);

    assert(RMW_RET_OK == rmw_destroy_publisher(node, neglected_pub));

    // The mirror image: a Publisher that keeps calling rmw_publisher_assert_liveliness() faster
    // than its own lease must never go LIVELINESS_LOST, even well past when it would have without
    // those calls - proves the assertion API actually keeps a manual Publisher alive, not just
    // that a neglected one goes lost.
    rmw_publisher_t* asserted_pub =
        rmw_create_publisher(node, fake_type_support(), "asserted_topic", &manual_qos, &pub_opts);
    assert(NULL != asserted_pub);
    rmw_event_t asserted_lost_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&asserted_lost_event, asserted_pub, RMW_EVENT_LIVELINESS_LOST));

    struct timespec assert_interval = {.tv_sec = 1, .tv_nsec = 0}; // well under manual_lease_ns (>= 3s)
    for (int i = 0; i < (int)((manual_lease_ns + tt_SECOND) / tt_SECOND); i++) {
        assert(RMW_RET_OK == rmw_publisher_assert_liveliness(asserted_pub));
        nanosleep(&assert_interval, NULL);
    }

    void* asserted_events_storage[1] = {&asserted_lost_event};
    rmw_events_t asserted_events = {.event_count = 1, .events = asserted_events_storage};
    rmw_time_t asserted_check_timeout = {0, (uint64_t)SANITY_CHECK_TIMEOUT_MS * NS_PER_MS};
    assert(RMW_RET_TIMEOUT == rmw_wait(NULL, NULL, NULL, NULL, &asserted_events, wait_set, &asserted_check_timeout));
    assert(NULL == asserted_events.events[0]);

    assert(RMW_RET_OK == rmw_destroy_publisher(node, asserted_pub));

    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_liveliness_lost_watchdog: all tests passed\n");
    return 0;
}
