/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS roadmap #2 (DEADLINE) + #3 (LIVELINESS): the first rmw_tickle test to exercise real rmw_
// create_publisher()/rmw_create_subscription()/rmw_publish() end to end (every earlier Milestone
// 10 test either stayed one level below that - test_graph.c builds a raw tt_Publisher/tt_
// Subscriber directly on tickle_node, bypassing rosidl entirely - or never touched a publisher/
// subscriber at all). rmw_tickle_get_message_callbacks() (rmw_typesupport.c) needs a real
// rosidl_message_type_support_t resolvable to a rosidl_typesupport_tickle_c_message_callbacks_t,
// which normally only tools/typesupport's own generated <name>__type_support.c produces from a
// real .msg - no such generated interface package is a dependency of this one, so this file hand-
// builds the exact same shape by hand instead (confirmed against a real generated Empty.msg
// build's own output - see fake_type_support()'s own doc comment below), the same spirit as test_
// graph.c's own fake tt_DATA_ENCODE/_DECODE stubs, one level up.
//
// LIVELINESS_CHANGED's own real signal (a matched Publisher's alive/not-alive transition) needs a
// *second* node - Milestone 2's own one-node-per-process rmw-level limit means this process can't
// have one, so that case stays a smoke test here (event_init()/rmw_take_event() wiring proven, an
// actual transition isn't) - see rmw_tickle/PLAN.md's own note on this same gap.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/event.h"
#include "rmw/events_statuses/liveliness_changed.h"
#include "rmw/events_statuses/liveliness_lost.h"
#include "rmw/events_statuses/offered_deadline_missed.h"
#include "rmw/events_statuses/requested_deadline_missed.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
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

static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "test_events/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

// Mirrors a real generated <name>__type_support.c's own handle shape exactly (confirmed against a
// real build's own test_msgs__msg__Empty__type_support.c output) - .typesupport_identifier is set
// on first access below, not in this initializer: a plain extern const char* like rosidl_
// typesupport_tickle_c__identifier isn't a compile-time constant in C, so it can't be a static
// initializer (that generated file's own comment gives the identical reason).
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
// Deliberately generous relative to DEADLINE_MS - see this file's own use of it for why a wide
// margin matters on a possibly-loaded CI machine, not just this test's own idle one.
#define DEADLINE_MS 60
#define WAIT_MS 500

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
    // See test_node_lifecycle.c's own comment on this same line.
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_events", "/");
    assert(NULL != node);

    rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 0);
    assert(NULL != wait_set);

    const rosidl_message_type_support_t* type_support = fake_type_support();
    rmw_time_t wait_timeout = {0, (uint64_t)WAIT_MS * NS_PER_MS};

    // -- RMW_EVENT_OFFERED_DEADLINE_MISSED: a Publisher that never publishes must miss. --
    rmw_qos_profile_t pub_qos = base_qos();
    pub_qos.deadline.sec = 0;
    pub_qos.deadline.nsec = (uint64_t)DEADLINE_MS * NS_PER_MS;
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "deadline_pub_topic", &pub_qos, &pub_opts);
    assert(NULL != pub);

    rmw_event_t offered_deadline_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&offered_deadline_event, pub, RMW_EVENT_OFFERED_DEADLINE_MISSED));

    void* events_storage[1] = {&offered_deadline_event};
    rmw_events_t events = {.event_count = 1, .events = events_storage};
    assert(RMW_RET_OK == rmw_wait(NULL, NULL, NULL, NULL, &events, wait_set, &wait_timeout));
    assert(NULL != events.events[0]);

    rmw_offered_deadline_missed_status_t offered_status;
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_event(&offered_deadline_event, &offered_status, &taken));
    assert(taken);
    assert(offered_status.total_count >= 1);
    assert(offered_status.total_count_change >= 1);

    // -- RMW_EVENT_LIVELINESS_LOST: initializable, but this rmw's AUTOMATIC liveliness can never
    // structurally observe its own loss - see rmw_tickle_publisher_t.liveliness_lost's own doc
    // comment (rmw_tickle_c/rmw_tickle.h) for why. Always {0, 0}, checked here after real time has
    // already passed above (the deadline wait), not immediately at creation - proves it stays
    // permanently zero, not just zero-at-first-glance.
    rmw_event_t liveliness_lost_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&liveliness_lost_event, pub, RMW_EVENT_LIVELINESS_LOST));
    rmw_liveliness_lost_status_t lost_status;
    taken = false;
    assert(RMW_RET_OK == rmw_take_event(&liveliness_lost_event, &lost_status, &taken));
    assert(taken);
    assert(0 == lost_status.total_count);
    assert(0 == lost_status.total_count_change);

    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

    // -- RMW_EVENT_REQUESTED_DEADLINE_MISSED: a Subscription that never receives must miss. --
    rmw_qos_profile_t sub_qos = base_qos();
    sub_qos.deadline.sec = 0;
    sub_qos.deadline.nsec = (uint64_t)DEADLINE_MS * NS_PER_MS;
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "deadline_sub_topic", &sub_qos, &sub_opts);
    assert(NULL != sub);

    rmw_event_t requested_deadline_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK ==
           rmw_subscription_event_init(&requested_deadline_event, sub, RMW_EVENT_REQUESTED_DEADLINE_MISSED));

    events_storage[0] = &requested_deadline_event;
    events.event_count = 1;
    assert(RMW_RET_OK == rmw_wait(NULL, NULL, NULL, NULL, &events, wait_set, &wait_timeout));
    assert(NULL != events.events[0]);

    rmw_requested_deadline_missed_status_t requested_status;
    taken = false;
    assert(RMW_RET_OK == rmw_take_event(&requested_deadline_event, &requested_status, &taken));
    assert(taken);
    assert(requested_status.total_count >= 1);
    assert(requested_status.total_count_change >= 1);

    // -- RMW_EVENT_LIVELINESS_CHANGED: smoke test only - see this file's own module doc comment
    // for why a real alive/not-alive transition needs a second node this process can't have.
    // event_init() itself lazily arms the periodic check (rmw_subscription.c); with no matching
    // Publisher ever discovered, alive_count/not_alive_count and both *_change deltas stay 0 - a
    // genuine 0 (nothing matched, nothing tombstoned either), not the documented "always 0, no
    // data behind it" gap not_alive_count used to be before struct tt_DiscoveredEntity.alive
    // (tickle.h) gave it real data to report; see tests/test_discovery.c (TickLE core) for the
    // actual tombstone-tracking coverage this file's own one-node limit can't reach.
    rmw_event_t liveliness_changed_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_subscription_event_init(&liveliness_changed_event, sub, RMW_EVENT_LIVELINESS_CHANGED));
    rmw_liveliness_changed_status_t changed_status;
    taken = false;
    assert(RMW_RET_OK == rmw_take_event(&liveliness_changed_event, &changed_status, &taken));
    assert(taken);
    assert(0 == changed_status.alive_count);
    assert(0 == changed_status.not_alive_count);
    assert(0 == changed_status.alive_count_change);
    assert(0 == changed_status.not_alive_count_change);

    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("DEADLINE/LIVELINESS events: PASS\n");
    return 0;
}
