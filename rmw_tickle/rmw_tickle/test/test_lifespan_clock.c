/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A subscription's LIFESPAN and the two clocks (DESIGN.md section 2, "Time"; 2026-10-16). The source timestamp is
// the writer's date, so a sample's age is taken once, on arrival, against this host's date (tt_get_wall_ns()); from
// there it is counted on the monotonic clock (tt_get_ns()), which a step of the date does not move. Messages are handed
// to the subscription as core delivers them, with a source timestamp of the test's choosing:
//   fresh       stamped now                                   -> taken; received_timestamp is this host's date
//   old         stamped 10 s ago, lifespan 500 ms             -> dropped on the take
//   ahead       stamped 1 h ahead (a writer whose date is ahead, or ours stepped back since) -> age 0: taken, not an
//               unsigned wrap to "expired" - which is what the take did before the split, against main's clock
//   ages        stamped LIFESPAN - 100 ms ago, taken at once  -> taken
//               the same, taken 200 ms later                  -> dropped: the rest of its life ran on after arrival
// Mutants, each killed here at the assertion named (2026-10-16): the take as before the split, date minus source at the
// take (ahead); the expiry not reduced by the age on arrival (old); received_timestamp from the deadline clock (fresh);
// no clamp on a stamp ahead (ahead).

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

struct pair {
    uint32_t first;
    uint32_t second;
};

static bool convert(const void* source, void* dest) {
    *(struct pair*)dest = *(const struct pair*)source;
    return true;
}
static int32_t encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(struct pair);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    memcpy(payload, data, sizeof(struct pair));
    return (int32_t)sizeof(struct pair);
}
static int32_t decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct pair)) {
        return -1;
    }
    memcpy(data, payload, sizeof(struct pair));
    return (int32_t)sizeof(struct pair);
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t pair_callbacks = {
    .ros_type_name = "lifespan/msg/Pair",
    .tickle_struct_size = sizeof(struct pair),
    .ros_struct_size = sizeof(struct pair),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&encode,
    .tickle_decode = (tt_DATA_DECODE)&decode,
    .tickle_free = (tt_DATA_FREE)&free_nothing,
    .tickle_max_encoded_size = sizeof(struct pair),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
};
static rosidl_message_type_support_t pair_handle = {.data = &pair_callbacks,
                                                    .func = get_message_typesupport_handle_function};

#define LIFESPAN_MS 500
#define MS_NS 1000000ULL
#define CAPACITY 8

// Message `id`, stamped `source_ns` (the writer's date), as core delivers it.
static void deliver(rmw_subscription_t* sub, uint32_t id, uint64_t source_ns) {
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    uint8_t wire[RMW_TICKLE_PSN_BYTES + sizeof(struct pair)] = {0};
    uint32_t header = rmw_tickle_psn_write(id, wire);
    struct pair message = {.first = id, .second = 0};
    memcpy(wire + header, &message, sizeof(message));
    struct tt_Data* data = sub_impl->topic.data_decode_inplace(wire, header + (uint32_t)sizeof(message), true);
    assert(NULL != data);
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, source_ns, 0, data);
}

// Takes one message: true and its id and info if there was one.
static bool take(rmw_subscription_t* sub, uint32_t* id, rmw_message_info_t* info) {
    struct pair message = {0, 0};
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_with_info(sub, &message, &taken, info, NULL));
    *id = message.first;
    return taken;
}

static void sleep_ms(long ms) {
    struct timespec delay = {.tv_sec = 0, .tv_nsec = ms * (long)MS_NS};
    nanosleep(&delay, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); // each case's line survives a failed assert() after it
    pair_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_lifespan_clock", "/");
    assert(NULL != node);
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.depth = CAPACITY;
    qos.lifespan = (rmw_time_t) {.sec = 0, .nsec = LIFESPAN_MS * MS_NS};
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &pair_handle, "/lifespan_clock", &qos, &sub_options);
    assert(NULL != sub);
    rmw_message_info_t info;
    uint32_t id = 0;

    // fresh: taken, with the writer's stamp and this host's date as the reception time.
    uint64_t now = tt_get_wall_ns();
    deliver(sub, 1, now);
    assert(take(sub, &id, &info) && 1 == id);
    assert((uint64_t)info.source_timestamp == now);
    uint64_t date = tt_get_wall_ns();
    uint64_t received = (uint64_t)info.received_timestamp;
    printf("fresh: received_timestamp %llu, the date %llu\n", (unsigned long long)received, (unsigned long long)date);
    assert(received >= now && received <= date);

    // old: past its lifespan on arrival.
    deliver(sub, 2, tt_get_wall_ns() - (10000ULL * MS_NS));
    bool old_taken = take(sub, &id, &info);
    printf("old: %s\n", old_taken ? "TAKEN" : "dropped");
    assert(!old_taken);

    // ahead: the writer's date an hour ahead of ours is age 0, not an expired wrap.
    deliver(sub, 3, tt_get_wall_ns() + (3600000ULL * MS_NS));
    bool ahead_taken = take(sub, &id, &info);
    printf("ahead: %s\n", ahead_taken ? "taken" : "DROPPED");
    assert(ahead_taken && 3 == id);

    // ages: 100 ms of its life left on arrival - taken at once, gone 200 ms later.
    deliver(sub, 4, tt_get_wall_ns() - ((LIFESPAN_MS - 100ULL) * MS_NS));
    bool at_once = take(sub, &id, &info) && 4 == id;
    deliver(sub, 5, tt_get_wall_ns() - ((LIFESPAN_MS - 100ULL) * MS_NS));
    sleep_ms(200);
    bool later = take(sub, &id, &info);
    printf("ages: taken at once %s, taken 200 ms later %s\n", at_once ? "yes" : "NO", later ? "YES" : "no");
    assert(at_once && !later);

    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_lifespan_clock: PASS\n");
    return 0;
}
