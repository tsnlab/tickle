/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// DDS QoS policy coverage inventory (rmw_tickle/PLAN.md, 2026-09-21) gap 2: a Publisher requesting
// RMW_QOS_POLICY_HISTORY_KEEP_ALL used to pass rmw_tickle_validate_qos_profile() (only Subscriptions
// are rejected for KEEP_ALL, rmw_qos.c) and then silently fall through setup_reliable_cache()'s own
// ->depth branch (rmw_publisher.c), landing on tt_MAX_RELIABLE_HISTORY=64 (or whatever ->depth
// happened to be) instead of anything resembling "retain everything" - a real "reject explicitly,
// never silently downgrade" violation. Fixed by honoring KEEP_ALL with a large-but-bounded
// RMW_TICKLE_KEEP_ALL_DEPTH=8192 cache instead. This test exercises the real rmw_create_publisher()
// path (not a whitebox include of rmw_publisher.c) and inspects the resulting rmw_tickle_publisher_t
// through its own public header, the same pattern test_publish_take_reuse.c/test_events.c already
// use.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> // setenv()/unsetenv() - the arena-sizing knob below

#include <tickle/config.h> // tt_MAX_BUFFER_LENGTH, tt_RELIABLE_CACHE_ARENA_BYTES/_RECORD_BYTES
#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
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

// Must match RMW_TICKLE_KEEP_ALL_DEPTH_DURABLE / _VOLATILE (rmw_publisher.c) - not exported via any
// header (purely internal sizing constants, not part of this rmw's own public API/ABI), so this
// test pins the values it expects directly rather than referencing the macros.
//
// Phase 3 step 3 split these apart: a TRANSIENT_LOCAL Publisher replays its whole retained range to
// each late joiner, so every extra slot is observable history; a VOLATILE one can never accumulate
// more unacknowledged samples than the announced tracking window (1024), so depth past that window
// is unreachable memory. See RMW_TICKLE_KEEP_ALL_DEPTH_VOLATILE's own comment for the coupling.
#define EXPECTED_KEEP_ALL_DEPTH_DURABLE 8192
#define EXPECTED_KEEP_ALL_DEPTH_VOLATILE 2048

// What a generated typesupport reports for a type the generator can bound. 76 is BenchData's own
// figure (examples/perf_hil/tickle/common/Bench.h), i.e. a realistic fixed-layout telemetry type
// rather than a round number.
#define BOUNDED_TYPE_MAX_ENCODED 76

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
    .ros_type_name = "test_history_keep_all/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

// The same fake type, but reporting a real per-type maximum the way a generated typesupport does
// for any type whose variable-length fields all resolve to a capacity. The one above leaves
// tickle_max_encoded_size at its zero-initialised default, which IS the "no bound" marker - so
// between them these two cover both sides of the decision without needing a second message shape.
static rosidl_typesupport_tickle_c_message_callbacks_t fake_bounded_callbacks;

static rosidl_message_type_support_t fake_bounded_handle = {
    .data = &fake_bounded_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* fake_bounded_type_support(void) {
    fake_bounded_callbacks = fake_callbacks;
    fake_bounded_callbacks.tickle_max_encoded_size = BOUNDED_TYPE_MAX_ENCODED;
    if (NULL == fake_bounded_handle.typesupport_identifier) {
        fake_bounded_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    }
    return &fake_bounded_handle;
}

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

static rmw_qos_profile_t base_qos(void) {
    rmw_qos_profile_t qos;
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = 10;
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

    rmw_node_t* node = rmw_create_node(&context, "test_history_keep_all", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = fake_type_support();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();

    // The real point: RELIABLE + KEEP_ALL must size reliable_cache to the KEEP_ALL depth, not
    // tt_MAX_RELIABLE_HISTORY=64 (the old silent-downgrade behavior) and not ->depth (10 here,
    // deliberately left small/irrelevant to prove it's ignored once KEEP_ALL is set). Phase 3
    // step 3 also wires the core-side back-pressure flag and its writable callback here, which is
    // what makes KEEP_ALL a retention promise rather than just a bigger ring buffer.
    {
        rmw_qos_profile_t qos = base_qos();
        qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_all_reliable", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(NULL != pub_impl->reliable_cache);
        assert(EXPECTED_KEEP_ALL_DEPTH_VOLATILE == pub_impl->reliable_cache->capacity);
        assert(EXPECTED_KEEP_ALL_DEPTH_VOLATILE == pub_impl->reliable_cache->depth);
        assert(pub_impl->tickle_publisher.keep_all);
        assert(NULL != pub_impl->tickle_publisher.writable_callback);
        assert(pub_impl == pub_impl->tickle_publisher.writable_callback_param);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // Same for TRANSIENT_LOCAL (the other setup_reliable_cache()-triggering policy, Milestone 24's
    // shared cache) - KEEP_ALL must be honored there too, not just for RELIABLE - and this is the
    // case that keeps the deep cache, because a late joiner is replayed the whole retained range.
    //
    // BEST_EFFORT, though, so keep_all must stay *off*: back-pressure waits for acknowledgements,
    // and a BEST_EFFORT Subscriber never sends any, so blocking here could only ever deadlock a
    // publisher against a peer that is behaving exactly as asked.
    {
        rmw_qos_profile_t qos = base_qos();
        qos.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
        qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_all_durable", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(NULL != pub_impl->reliable_cache);
        assert(EXPECTED_KEEP_ALL_DEPTH_DURABLE == pub_impl->reliable_cache->capacity);
        assert(!pub_impl->tickle_publisher.keep_all);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // Contrast case: ordinary KEEP_LAST with an explicit depth must be completely unaffected by
    // this change - still sized to ->depth, not EXPECTED_KEEP_ALL_DEPTH.
    {
        rmw_qos_profile_t qos = base_qos();
        qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
        qos.depth = 10;
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_last_reliable", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(NULL != pub_impl->reliable_cache);
        assert(10 == pub_impl->reliable_cache->capacity);
        // KEEP_LAST is the "drop the oldest" policy by definition, so it must never refuse a write.
        assert(!pub_impl->tickle_publisher.keep_all);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES (rmw_tickle/PLAN.md, the deferred per-type maximum's
    // cheaper stand-in): an application that knows its own types can shrink the per-sample arena
    // reservation. Three things to pin - the default is byte-identical to what it was before the
    // knob existed, a set value really sizes the arena, and B1's count guarantee survives it.
    {
        rmw_qos_profile_t qos = base_qos();
        qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;

        // Default: unset must reserve exactly tt_MAX_BUFFER_LENGTH per record, not the
        // header-adjusted figure a set value goes through. "Unchanged" has to mean identical.
        unsetenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES");
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_all_arena_default", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(tt_RELIABLE_CACHE_ARENA_BYTES(EXPECTED_KEEP_ALL_DEPTH_VOLATILE, tt_MAX_BUFFER_LENGTH) ==
               pub_impl->reliable_cache->arena_size);
        assert(EXPECTED_KEEP_ALL_DEPTH_VOLATILE == pub_impl->reliable_cache->capacity); // count bound intact
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

        // Set: the value is a message payload, so the arena is sized from the *record* it implies
        // (payload plus submessage and DATA headers, padded) - getting that wrong would
        // under-reserve by 24 bytes a sample.
        setenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES", "76", 1);
        pub = rmw_create_publisher(node, type_support, "keep_all_arena_sized", &qos, &pub_opts);
        assert(NULL != pub);
        pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(tt_RELIABLE_CACHE_ARENA_BYTES(EXPECTED_KEEP_ALL_DEPTH_VOLATILE, tt_RELIABLE_RECORD_BYTES(76)) ==
               pub_impl->reliable_cache->arena_size);
        // ...and it is a real reduction, not a rounding difference.
        assert(pub_impl->reliable_cache->arena_size <
               tt_RELIABLE_CACHE_ARENA_BYTES(EXPECTED_KEEP_ALL_DEPTH_VOLATILE, tt_MAX_BUFFER_LENGTH) / 10);
        // B1's guarantee is by count, and a narrower arena must not weaken it: still `depth` slots,
        // and still `depth + 1` records of room so the byte bound can't evict before the count one.
        assert(EXPECTED_KEEP_ALL_DEPTH_VOLATILE == pub_impl->reliable_cache->capacity);
        assert(EXPECTED_KEEP_ALL_DEPTH_VOLATILE == pub_impl->reliable_cache->depth);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

        // Nonsense and out-of-range values fall back rather than failing node startup.
        const char* rejected[] = {"0", "not-a-number", "999999"};
        for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
            setenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES", rejected[i], 1);
            pub = rmw_create_publisher(node, type_support, "keep_all_arena_bad", &qos, &pub_opts);
            assert(NULL != pub);
            pub_impl = (rmw_tickle_publisher_t*)pub->data;
            assert(tt_RELIABLE_CACHE_ARENA_BYTES(EXPECTED_KEEP_ALL_DEPTH_VOLATILE, tt_MAX_BUFFER_LENGTH) ==
                   pub_impl->reliable_cache->arena_size);
            assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
        }

        // A type the generator CAN bound uses that number, and does not consult the environment -
        // there is nothing a human knows about a bounded type that beats a computed bound on it,
        // and honouring a smaller hand-set value would cost retention in the one case we had the
        // right answer for. Set the variable to something different from the generated figure so a
        // pass can't come from the two agreeing by accident.
        unsetenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES");
        const rosidl_message_type_support_t* bounded_ts = fake_bounded_type_support();
        pub = rmw_create_publisher(node, bounded_ts, "keep_all_generated_bound", &qos, &pub_opts);
        assert(NULL != pub);
        pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(tt_RELIABLE_CACHE_ARENA_BYTES(EXPECTED_KEEP_ALL_DEPTH_VOLATILE,
                                             tt_RELIABLE_RECORD_BYTES(BOUNDED_TYPE_MAX_ENCODED)) ==
               pub_impl->reliable_cache->arena_size);
        assert(EXPECTED_KEEP_ALL_DEPTH_VOLATILE == pub_impl->reliable_cache->capacity); // B1 intact
        assert(EXPECTED_KEEP_ALL_DEPTH_VOLATILE == pub_impl->reliable_cache->depth);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

        setenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES", "512", 1);
        pub = rmw_create_publisher(node, bounded_ts, "keep_all_generated_wins", &qos, &pub_opts);
        assert(NULL != pub);
        pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(tt_RELIABLE_CACHE_ARENA_BYTES(EXPECTED_KEEP_ALL_DEPTH_VOLATILE,
                                             tt_RELIABLE_RECORD_BYTES(BOUNDED_TYPE_MAX_ENCODED)) ==
               pub_impl->reliable_cache->arena_size); // the generated 76, not the environment's 512
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

        // KEEP_LAST must ignore it entirely - that is what keeps the variable's name honest.
        setenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES", "76", 1);
        qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
        qos.depth = 10;
        pub = rmw_create_publisher(node, type_support, "keep_last_arena", &qos, &pub_opts);
        assert(NULL != pub);
        pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(tt_RELIABLE_CACHE_ARENA_BYTES(10, tt_MAX_BUFFER_LENGTH) == pub_impl->reliable_cache->arena_size);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
        unsetenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES");
    }

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_history_keep_all: PASS\n");
    return 0;
}
