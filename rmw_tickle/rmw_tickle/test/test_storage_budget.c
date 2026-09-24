/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Per-entity storage sized by the entity's own type, with byte budgets where the type has no small
// bound - the storage design the user approved on 2026-09-24, needed before tt_MAX_BUFFER_LENGTH
// can rise to 65507 without every KEEP_LAST publisher and RELIABLE subscription reserving
// megabytes. Checked at this build's N, whatever it is; the numbers are derived, not hard-coded.
//
//   - KEEP_LAST reliable cache: min((depth + 1) x record, max(RMW_TICKLE_CACHE_BYTES, one record)).
//   - RELIABLE reorder buffer: slot payload capped at RMW_TICKLE_REORDER_SLOT_PAYLOAD, slots capped
//     at RMW_TICKLE_REORDER_BYTES / stride. BEST_EFFORT gets none.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h> // tt_RELIABLE_RECORD_BYTES
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define MIB (1024ULL * 1024ULL)
#define BOUNDED_PAYLOAD 76
#define SMALL_BUDGET 5000 // bytes: below /rosout-sized arenas, above one 1472-byte record

struct fake_msg {
    uint8_t value;
};

static bool fake_convert(const void* source, void* dest) {
    ((struct fake_msg*)dest)->value = ((const struct fake_msg*)source)->value;
    return true;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return 1;
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    payload[0] = ((struct fake_msg*)data)->value;
    return 1;
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)len;
    (void)is_native_endian;
    ((struct fake_msg*)data)->value = payload[0];
    return 1;
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

// tickle_max_encoded_size is edited per case: 0 = no bound (a plain string somewhere), or a bound.
static rosidl_typesupport_tickle_c_message_callbacks_t callbacks = {
    .ros_type_name = "test_storage_budget/msg/Fake",
    .tickle_struct_size = sizeof(struct fake_msg),
    .ros_struct_size = sizeof(struct fake_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};
static rosidl_message_type_support_t handle = {.data = &callbacks, .func = get_message_typesupport_handle_function};

static rmw_qos_profile_t qos(enum rmw_qos_reliability_policy_e reliability, size_t depth) {
    rmw_qos_profile_t q;
    memset(&q, 0, sizeof(q));
    q.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    q.depth = depth;
    q.reliability = reliability;
    q.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    q.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    q.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    q.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    q.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    return q;
}

static rmw_node_t* node;

static uint32_t keep_last_arena(size_t depth) {
    rmw_qos_profile_t q = qos(RMW_QOS_POLICY_RELIABILITY_RELIABLE, depth);
    rmw_publisher_options_t opts = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, &handle, "/budget", &q, &opts);
    assert(NULL != pub);
    uint32_t arena = ((rmw_tickle_publisher_t*)pub->data)->reliable_cache->arena_size;
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    return arena;
}

static void reorder_shape(enum rmw_qos_reliability_policy_e reliability, uint16_t* slots, uint16_t* slot_bytes,
                          bool* has_storage) {
    rmw_qos_profile_t q = qos(reliability, 10);
    rmw_subscription_options_t opts = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &handle, "/budget", &q, &opts);
    assert(NULL != sub);
    rmw_tickle_subscriber_t* impl = (rmw_tickle_subscriber_t*)sub->data;
    *slots = impl->tickle_subscriber.reorder_slots;
    *slot_bytes = impl->tickle_subscriber.reorder_slot_bytes;
    *has_storage = NULL != impl->reorder_storage;
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
}

static unsigned long long min_ull(unsigned long long a, unsigned long long b) {
    return a < b ? a : b;
}

int main(void) {
    handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    unsetenv("RMW_TICKLE_CACHE_BYTES");
    unsetenv("RMW_TICKLE_REORDER_SLOT_PAYLOAD");
    unsetenv("RMW_TICKLE_REORDER_BYTES");
    unsetenv("RMW_TICKLE_REORDER_SLOTS");

    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    node = rmw_create_node(&context, "test_storage_budget", "/");
    assert(NULL != node);

    const unsigned long long buf_len = (unsigned long long)tt_MAX_BUFFER_LENGTH;

    // --- KEEP_LAST cache, unbounded type: records of one datagram, capped at 1 MiB ---------------
    callbacks.tickle_max_encoded_size = ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED;
    assert(keep_last_arena(10) == min_ull(11 * buf_len, MIB));     // a typical depth: unchanged by the cap
    assert(keep_last_arena(1000) == min_ull(1001 * buf_len, MIB)); // /rosout's depth: the cap binds
    setenv("RMW_TICKLE_CACHE_BYTES", "5000", 1);                   // SMALL_BUDGET
    unsigned long long floor_or_budget = buf_len;                  // the budget, but never below one record
    if (floor_or_budget < SMALL_BUDGET) {
        floor_or_budget = SMALL_BUDGET;
    }
    assert(keep_last_arena(10) == min_ull(11 * buf_len, floor_or_budget));
    setenv("RMW_TICKLE_CACHE_BYTES", "100", 1); // below one record: one record, so a sample still fits
    assert(keep_last_arena(10) == buf_len);
    setenv("RMW_TICKLE_CACHE_BYTES", "nonsense", 1); // malformed: the default, not a failure
    assert(keep_last_arena(1000) == min_ull(1001 * buf_len, MIB));
    unsetenv("RMW_TICKLE_CACHE_BYTES");

    // --- KEEP_LAST cache, bounded type: records of the type's own size -----------------------------
    callbacks.tickle_max_encoded_size = BOUNDED_PAYLOAD;
    assert(keep_last_arena(10) == 11ULL * tt_RELIABLE_RECORD_BYTES(BOUNDED_PAYLOAD));

    // --- reorder buffer ----------------------------------------------------------------------------
    uint16_t slots = 0;
    uint16_t slot_bytes = 0;
    bool has_storage = false;
    const unsigned long long header = sizeof(struct tt_ReorderSlot);

    reorder_shape(RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT, &slots, &slot_bytes, &has_storage);
    assert(!has_storage && 0 == slots); // BEST_EFFORT never reads one, so it gets none

    // Bounded and small: the full window, slots of the type's size.
    reorder_shape(RMW_QOS_POLICY_RELIABILITY_RELIABLE, &slots, &slot_bytes, &has_storage);
    assert(has_storage);
    assert(slot_bytes == header + BOUNDED_PAYLOAD);
    assert(slots == min_ull(RMW_TICKLE_REORDER_SLOTS, MIB / slot_bytes));

    // Unbounded: the slot payload is capped at 2 KiB (or the datagram, if smaller), and the budget
    // decides how many.
    callbacks.tickle_max_encoded_size = ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED;
    reorder_shape(RMW_QOS_POLICY_RELIABILITY_RELIABLE, &slots, &slot_bytes, &has_storage);
    assert(slot_bytes == header + min_ull(buf_len, 2048));
    assert(slots == min_ull(RMW_TICKLE_REORDER_SLOTS, MIB / slot_bytes));
    assert((unsigned long long)slots * slot_bytes <= MIB);

    setenv("RMW_TICKLE_REORDER_SLOT_PAYLOAD", "512", 1);
    setenv("RMW_TICKLE_REORDER_BYTES", "65536", 1);
    reorder_shape(RMW_QOS_POLICY_RELIABILITY_RELIABLE, &slots, &slot_bytes, &has_storage);
    assert(slot_bytes == header + min_ull(buf_len, 512));
    assert(slots == 65536 / slot_bytes);
    setenv("RMW_TICKLE_REORDER_BYTES", "1", 1); // smaller than one slot: still one slot, never zero
    reorder_shape(RMW_QOS_POLICY_RELIABILITY_RELIABLE, &slots, &slot_bytes, &has_storage);
    assert(1 == slots && has_storage);
    unsetenv("RMW_TICKLE_REORDER_SLOT_PAYLOAD");
    unsetenv("RMW_TICKLE_REORDER_BYTES");

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_storage_budget: PASS\n");
    return 0;
}
