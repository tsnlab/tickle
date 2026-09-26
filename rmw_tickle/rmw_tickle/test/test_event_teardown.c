/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A Publisher or Subscription whose QoS-incompatible event was initialised arms a check that reschedules
// itself every RMW_TICKLE_QOS_INCOMPATIBLE_CHECK_PERIOD_NS (1 s). Destroying the entity must cancel it:
// until 2026-09-27 neither destroy did, and the check ran on the freed entity at its next period - a
// SIGSEGV in strcmp() on the freed topic name, in test_events about one run in twenty (found while
// benchmarking OPTIMIZATION_PLAN.md 11's D3, which only moved the timing).
//
// Deterministic here: every block the rmw layer frees is overwritten and held back from malloc until the
// test ends (poisoning_deallocate()), so a check that runs on a freed entity follows a poisoned pointer and
// crashes - rather than reading a block malloc has already handed out again, which is what the first
// version of this test did, and passed. The test then waits two periods with the poll thread running.

#include <assert.h>
#include <malloc.h> // malloc_usable_size() - glibc; this package is Linux-only
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/event.h"
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

#define POISON 0xA5
#define WAIT_PERIODS_S 2U // the check's period is 1 s: two of them, with margin below
#define WAIT_MARGIN_MS 500L
#define NS_PER_MS (1000L * 1000L)
#define MS_PER_S 1000L
#define HELD_MAX 4096 // blocks held back until the end; more are freed at once, unpoisoned-safe

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
    .ros_type_name = "test_event_teardown/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

// As test_events.c's own: .typesupport_identifier is set on first use, not statically.
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

static void* held[HELD_MAX];
static size_t held_count;

// Overwrites the whole block and keeps it from malloc until release_held(), so nothing reuses it meanwhile.
static void poisoning_deallocate(void* pointer, void* state) {
    (void)state;
    if (pointer == NULL) {
        return;
    }
    memset(pointer, POISON, malloc_usable_size(pointer));
    if (held_count < HELD_MAX) {
        held[held_count++] = pointer;
    } else {
        free(pointer);
    }
}

static void release_held(void) {
    for (size_t i = 0; i < held_count; i++) {
        free(held[i]);
    }
    held_count = 0;
}

static rmw_qos_profile_t base_qos(void) {
    rmw_qos_profile_t qos;
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = 1;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
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
    allocator.deallocate = poisoning_deallocate;

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator); // see test_node_lifecycle.c's own comment
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_event_teardown", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = fake_type_support();
    rmw_qos_profile_t qos = base_qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "teardown_topic", &qos, &pub_opts);
    rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "teardown_topic", &qos, &sub_opts);
    assert(NULL != pub && NULL != sub);

    // Each arms its periodic check.
    rmw_event_t offered = rmw_get_zero_initialized_event();
    rmw_event_t requested = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&offered, pub, RMW_EVENT_OFFERED_QOS_INCOMPATIBLE));
    assert(RMW_RET_OK == rmw_subscription_event_init(&requested, sub, RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE));

    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

    // The poll thread runs on: a check still armed would fire here, on poisoned memory.
    struct timespec wait = {WAIT_PERIODS_S, WAIT_MARGIN_MS * NS_PER_MS};
    (void)nanosleep(&wait, NULL);

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    release_held();
    (void)printf("test_event_teardown: passed (%ld ms with the poll thread running after destroy)\n",
                 (WAIT_PERIODS_S * MS_PER_S) + WAIT_MARGIN_MS);
    return 0;
}
