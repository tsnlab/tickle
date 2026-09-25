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
#include "rmw_tickle_c/publisher_payload.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define MIB (1024ULL * 1024ULL)
// Must match RMW_TICKLE_REORDER_BYTES_DEFAULT (rmw_subscription.c): a full window of slots sized
// for the standard 1472-byte datagram, i.e. what an unbounded type reserved before any budget.
#define REORDER_BUDGET \
    ((unsigned long long)RMW_TICKLE_REORDER_SLOTS * (sizeof(struct tt_ReorderSlot) + tt_ETHERNET_UDP_PAYLOAD))
#define BOUNDED_PAYLOAD 76
#define SMALL_BUDGET 5000                 // bytes: below /rosout-sized arenas, above one 1472-byte record
#define PAYLOAD_SAMPLE_BYTES 16384        // what an application claims its samples reach, through the payload
#define FOREIGN_PAYLOAD_MAGIC 0xDEADBEEFU // a payload that is not rmw_tickle's, however it got here
#define LAZY_INITIAL_BYTES (64U * 1024U)  // RMW_TICKLE_INITIAL_ARENA_BYTES (rmw_publisher.c)
#define ROSOUT_DEPTH 1000      // the deepest queue a default rclcpp::Node creates, and the budget's test case
#define BIG_SAMPLE_BYTES 16384 // a sample large enough that the initial arena slice holds only a few
#define GROWTH_DEPTH 10        // ...and a depth the byte bound falls short of until the arena grows

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
// A type whose samples are BIG_SAMPLE_BYTES, for the growth tests: with the initial 64 KiB slice a
// KEEP_LAST publisher holds three of these, far fewer than its depth, until the arena grows.
static int32_t big_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)BIG_SAMPLE_BYTES;
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t big_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < BIG_SAMPLE_BYTES) {
        return -1;
    }
    memset(payload, ((struct fake_msg*)data)->value, BIG_SAMPLE_BYTES);
    return (int32_t)BIG_SAMPLE_BYTES;
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

// The arena a publisher of this type gets, with `payload` attached to its options (NULL for none).
static uint32_t keep_last_arena_with(size_t depth, rmw_tickle_publisher_payload_t* payload) {
    rmw_qos_profile_t q = qos(RMW_QOS_POLICY_RELIABILITY_RELIABLE, depth);
    rmw_publisher_options_t opts = rmw_get_default_publisher_options();
    opts.rmw_specific_publisher_payload = payload;
    rmw_publisher_t* pub = rmw_create_publisher(node, &handle, "/budget", &q, &opts);
    assert(NULL != pub);
    // arena_limit, not arena_size: since 2026-09-25 the arena is reserved lazily, so arena_size is
    // the first slice and the limit is the budget these assertions are about. The slice itself is
    // checked once, below.
    uint32_t arena = ((rmw_tickle_publisher_t*)pub->data)->reliable_cache->arena_limit;
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    return arena;
}

static uint32_t keep_last_arena(size_t depth) {
    return keep_last_arena_with(depth, NULL);
}

// The bytes a KEEP_ALL publisher of this type reserves per sample, with `payload` attached.
static uint32_t keep_all_record_with(rmw_tickle_publisher_payload_t* payload) {
    rmw_qos_profile_t q = qos(RMW_QOS_POLICY_RELIABILITY_RELIABLE, 1);
    q.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
    rmw_publisher_options_t opts = rmw_get_default_publisher_options();
    opts.rmw_specific_publisher_payload = payload;
    rmw_publisher_t* pub = rmw_create_publisher(node, &handle, "/budget_keep_all", &q, &opts);
    assert(NULL != pub);
    const rmw_tickle_publisher_t* pub_impl = (const rmw_tickle_publisher_t*)pub->data;
    // The arena is (depth + 1) records of whatever was reserved, so the record is what it divides to.
    uint32_t record = pub_impl->reliable_cache->arena_limit / (pub_impl->reliable_cache->depth + 1U);
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    return record;
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
    assert(keep_last_arena(10) == min_ull(11 * buf_len, MIB)); // a typical depth: unchanged by the cap
    assert(keep_last_arena(ROSOUT_DEPTH) ==
           min_ull((ROSOUT_DEPTH + 1) * buf_len, MIB)); // /rosout's depth: the cap binds
    setenv("RMW_TICKLE_CACHE_BYTES", "5000", 1);        // SMALL_BUDGET
    unsigned long long floor_or_budget = buf_len;       // the budget, but never below one record
    if (floor_or_budget < SMALL_BUDGET) {
        floor_or_budget = SMALL_BUDGET;
    }
    assert(keep_last_arena(10) == min_ull(11 * buf_len, floor_or_budget));
    setenv("RMW_TICKLE_CACHE_BYTES", "100", 1); // below one record: one record, so a sample still fits
    assert(keep_last_arena(10) == buf_len);
    setenv("RMW_TICKLE_CACHE_BYTES", "nonsense", 1); // malformed: the default, not a failure
    assert(keep_last_arena(ROSOUT_DEPTH) == min_ull((ROSOUT_DEPTH + 1) * buf_len, MIB));
    unsetenv("RMW_TICKLE_CACHE_BYTES");

    // --- KEEP_LAST cache, bounded type: records of the type's own size -----------------------------
    callbacks.tickle_max_encoded_size = BOUNDED_PAYLOAD;
    assert(keep_last_arena(10) == 11ULL * tt_RELIABLE_RECORD_BYTES(BOUNDED_PAYLOAD));

    // --- lazy reservation --------------------------------------------------------------------------
    // The budget decides the limit; only the first slice is allocated, and publish_blocking() grows
    // toward the limit if the traffic asks. A publisher that never fills it never pays for it.
    callbacks.tickle_max_encoded_size = ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED; // as above
    {
        rmw_qos_profile_t q = qos(RMW_QOS_POLICY_RELIABILITY_RELIABLE, ROSOUT_DEPTH);
        rmw_publisher_options_t opts = rmw_get_default_publisher_options();
        rmw_publisher_t* pub = rmw_create_publisher(node, &handle, "/lazy", &q, &opts);
        assert(NULL != pub);
        const struct tt_ReliableCache* cache = ((rmw_tickle_publisher_t*)pub->data)->reliable_cache;
        assert(cache->arena_limit == min_ull((ROSOUT_DEPTH + 1) * buf_len, MIB)); // the budget, unchanged
        assert(cache->arena_size < cache->arena_limit);                           // ...and not all of it yet
        assert(cache->arena_size == LAZY_INITIAL_BYTES);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }
    {
        // A budget smaller than that slice is allocated whole - there is nothing to defer.
        setenv("RMW_TICKLE_CACHE_BYTES", "5000", 1);
        rmw_qos_profile_t q = qos(RMW_QOS_POLICY_RELIABILITY_RELIABLE, 10);
        rmw_publisher_options_t opts = rmw_get_default_publisher_options();
        rmw_publisher_t* pub = rmw_create_publisher(node, &handle, "/lazy_small", &q, &opts);
        assert(NULL != pub);
        const struct tt_ReliableCache* cache = ((rmw_tickle_publisher_t*)pub->data)->reliable_cache;
        assert(cache->arena_size == cache->arena_limit);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
        unsetenv("RMW_TICKLE_CACHE_BYTES");
    }

    // --- KEEP_LAST grows back to its depth -------------------------------------------------------
    // KEEP_LAST never blocks, so the blocking path's growth trigger cannot serve it: its signal is
    // retention falling below depth. Without that trigger this publisher would sit at the initial
    // slice and keep three samples where its depth promises ten.
    {
        callbacks.tickle_max_encoded_size = BIG_SAMPLE_BYTES;
        callbacks.tickle_encode_size = (tt_DATA_ENCODE_SIZE)&big_encode_size;
        callbacks.tickle_encode = (tt_DATA_ENCODE)&big_encode;
        rmw_qos_profile_t q = qos(RMW_QOS_POLICY_RELIABILITY_RELIABLE, GROWTH_DEPTH);
        rmw_publisher_options_t opts = rmw_get_default_publisher_options();
        rmw_publisher_t* pub = rmw_create_publisher(node, &handle, "/growth", &q, &opts);
        assert(NULL != pub);
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
        const struct tt_ReliableCache* cache = pub_impl->reliable_cache;
        const uint32_t started_at = cache->arena_size;
        assert(started_at < cache->arena_limit); // there is room to grow into

        // Enough publishes to see the whole arc: the arena starts too small to hold depth samples,
        // grows once retention falls short, and then refills to depth. Each grow only doubles, and
        // retention climbs one sample per publish afterwards, so the count has to cover both.
        struct fake_msg message = {.value = (uint8_t)BIG_SAMPLE_BYTES}; // any value; the size is the point
        for (int i = 0; i < 3 * GROWTH_DEPTH; i++) {
            assert(RMW_RET_OK == rmw_publish(pub, &message, NULL));
        }

        assert(cache->arena_size > started_at); // it grew, without anyone asking
        // ...and retention is what depth promises again: the last GROWTH_DEPTH samples are held.
        assert(cache->newest_seq_no - cache->oldest_seq_no + 1 == GROWTH_DEPTH);
        assert(cache->arena_size <= cache->arena_limit); // never past the budget
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
        callbacks.tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size;
        callbacks.tickle_encode = (tt_DATA_ENCODE)&fake_encode;
    }

    // --- per-publisher payload (rmw_tickle_c/publisher_payload.h) ----------------------------------
    // It is the process-wide environment variables that these override, for this publisher only.
    callbacks.tickle_max_encoded_size = ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED;
    setenv("RMW_TICKLE_CACHE_BYTES", "5000", 1); // the process default, deliberately small
    rmw_tickle_publisher_payload_t payload = RMW_TICKLE_PUBLISHER_PAYLOAD_INIT;
    payload.cache_bytes = 4U * (uint32_t)MIB;
    assert(keep_last_arena_with(ROSOUT_DEPTH, &payload) ==
           min_ull((ROSOUT_DEPTH + 1) * buf_len, 4 * MIB));                               // payload wins
    assert(keep_last_arena(ROSOUT_DEPTH) < keep_last_arena_with(ROSOUT_DEPTH, &payload)); // ...and the env still binds
                                                                                          // the publisher without one

    payload.cache_bytes = 0; // 0 means "leave this knob to the environment", not "no budget"
    assert(keep_last_arena_with(ROSOUT_DEPTH, &payload) == keep_last_arena(ROSOUT_DEPTH));
    unsetenv("RMW_TICKLE_CACHE_BYTES");

    // A payload from another rmw implementation, or built against another version of the header:
    // ignored with a warning, never obeyed and never a failure.
    rmw_tickle_publisher_payload_t foreign = RMW_TICKLE_PUBLISHER_PAYLOAD_INIT;
    foreign.magic = FOREIGN_PAYLOAD_MAGIC;
    foreign.cache_bytes = 4U * (uint32_t)MIB;
    assert(keep_last_arena_with(10, &foreign) == keep_last_arena(10));
    rmw_tickle_publisher_payload_t stale = RMW_TICKLE_PUBLISHER_PAYLOAD_INIT;
    stale.struct_size = (uint32_t)sizeof(stale) + 4U;
    stale.cache_bytes = 4U * (uint32_t)MIB;
    assert(keep_last_arena_with(10, &stale) == keep_last_arena(10));

    // The KEEP_ALL reservation, the knob that decides when such a publisher starts blocking.
    setenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES", "2000", 1);
    assert(keep_all_record_with(NULL) == tt_RELIABLE_RECORD_BYTES(2000)); // the process default
    rmw_tickle_publisher_payload_t big = RMW_TICKLE_PUBLISHER_PAYLOAD_INIT;
    big.max_sample_bytes = PAYLOAD_SAMPLE_BYTES;
    assert(keep_all_record_with(&big) == tt_RELIABLE_RECORD_BYTES(PAYLOAD_SAMPLE_BYTES)); // payload wins
    unsetenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES");
    assert(keep_all_record_with(&big) == tt_RELIABLE_RECORD_BYTES(PAYLOAD_SAMPLE_BYTES)); // ...with or without one

    // A bounded type clamps the payload on the way up - reserving past an exact maximum is waste -
    // but takes it on the way down, which is the useful direction: the same ten samples for an
    // eighth of the memory when the application knows its images are smaller than the bound allows.
    callbacks.tickle_max_encoded_size = BOUNDED_PAYLOAD;
    assert(keep_all_record_with(&big) == tt_RELIABLE_RECORD_BYTES(BOUNDED_PAYLOAD)); // clamped up
    rmw_tickle_publisher_payload_t small = RMW_TICKLE_PUBLISHER_PAYLOAD_INIT;
    small.max_sample_bytes = BOUNDED_PAYLOAD / 2;
    assert(keep_all_record_with(&small) == tt_RELIABLE_RECORD_BYTES(BOUNDED_PAYLOAD / 2)); // lowered
    // ...and it lowers a KEEP_LAST publisher's arena the same way, keeping its whole depth.
    assert(keep_last_arena_with(10, &small) == 11ULL * tt_RELIABLE_RECORD_BYTES(BOUNDED_PAYLOAD / 2));
    assert(keep_last_arena_with(10, &small) < keep_last_arena(10));
    // Left bounded on purpose: that is the state the reorder section below starts from, and this
    // block sits between it and the one that set it.

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
    assert(slots == min_ull(RMW_TICKLE_REORDER_SLOTS, REORDER_BUDGET / slot_bytes));

    // Unbounded: the slot payload is capped at 2 KiB (or the datagram, if smaller), and the budget
    // decides how many - at N=1472 the whole window, exactly as before there was a budget.
    callbacks.tickle_max_encoded_size = ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED;
    reorder_shape(RMW_QOS_POLICY_RELIABILITY_RELIABLE, &slots, &slot_bytes, &has_storage);
    assert(slot_bytes == header + min_ull(buf_len, 2048));
    assert(slots == min_ull(RMW_TICKLE_REORDER_SLOTS, REORDER_BUDGET / slot_bytes));
    assert((unsigned long long)slots * slot_bytes <= REORDER_BUDGET);
    if (tt_MAX_BUFFER_LENGTH <= tt_ETHERNET_UDP_PAYLOAD) {
        assert(slots == RMW_TICKLE_REORDER_SLOTS); // today's configuration: unchanged
    }

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
