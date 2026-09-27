/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g5 (RMW_GAPS_PLAN.md): rmw_take_sequence(). Five messages handed to the subscription as core delivers them; a take
// of 3 returns the first three in order, with their publication sequence numbers; a take of 5 returns the other two
// (taken below count); a take from the empty queue returns RMW_RET_OK with nothing taken and both sequences
// unchanged; count 0 and a sequence smaller than count are refused. Mutants, each killed here: one message per call;
// the sizes not set; count 0 accepted.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/message_sequence.h"
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

#define PAIR_CALLBACKS(type_name)                                                    \
    {                                                                                \
        .ros_type_name = (type_name),                                                \
        .tickle_struct_size = sizeof(struct pair),                                   \
        .ros_struct_size = sizeof(struct pair),                                      \
        .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function) & convert,     \
        .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function) & convert, \
        .tickle_encode_size = (tt_DATA_ENCODE_SIZE) & encode_size,                   \
        .tickle_encode = (tt_DATA_ENCODE) & encode,                                  \
        .tickle_decode = (tt_DATA_DECODE) & decode,                                  \
        .tickle_free = (tt_DATA_FREE) & free_nothing,                                \
        .tickle_max_encoded_size = sizeof(struct pair),                              \
        .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,                            \
    }

static rosidl_typesupport_tickle_c_message_callbacks_t pair_callbacks = PAIR_CALLBACKS("g5/msg/Pair");
static rosidl_message_type_support_t pair_handle = {.data = &pair_callbacks,
                                                    .func = get_message_typesupport_handle_function};

#define DELIVERED 5
#define FIRST_TAKE 3
#define CAPACITY 5
#define FIRST_PSN 100U
#define UNCHANGED 77U

// Message i, with psn FIRST_PSN + i, as core delivers it.
static void deliver(rmw_subscription_t* sub, uint32_t i) {
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    uint8_t wire[RMW_TICKLE_PSN_BYTES + sizeof(struct pair)] = {0};
    uint32_t header = rmw_tickle_psn_write(FIRST_PSN + i, wire);
    struct pair message = {.first = i, .second = 0};
    memcpy(wire + header, &message, sizeof(message));
    struct tt_Data* data = sub_impl->topic.data_decode_inplace(wire, header + (uint32_t)sizeof(message), true);
    assert(NULL != data);
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, 0, 0, data);
}

int main(void) {
    pair_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_take_sequence", "/");
    assert(NULL != node);
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.depth = CAPACITY;
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &pair_handle, "/g5_topic", &qos, &sub_options);
    assert(NULL != sub);

    struct pair storage[CAPACITY];
    rmw_message_sequence_t messages = rmw_get_zero_initialized_message_sequence();
    rmw_message_info_sequence_t infos = rmw_get_zero_initialized_message_info_sequence();
    assert(RMW_RET_OK == rmw_message_sequence_init(&messages, CAPACITY, &allocator));
    assert(RMW_RET_OK == rmw_message_info_sequence_init(&infos, CAPACITY, &allocator));
    for (size_t i = 0; i < CAPACITY; i++) {
        messages.data[i] = &storage[i];
    }
    for (uint32_t i = 0; i < DELIVERED; i++) {
        deliver(sub, i);
    }

    // Three of five, in order, with their psns.
    size_t taken = 0;
    assert(RMW_RET_OK == rmw_take_sequence(sub, FIRST_TAKE, &messages, &infos, &taken, NULL));
    assert(FIRST_TAKE == taken && FIRST_TAKE == messages.size && FIRST_TAKE == infos.size);
    for (uint32_t i = 0; i < FIRST_TAKE; i++) {
        assert(i == storage[i].first && FIRST_PSN + i == infos.data[i].publication_sequence_number);
    }

    // Asked for five, two are left: taken below count.
    assert(RMW_RET_OK == rmw_take_sequence(sub, CAPACITY, &messages, &infos, &taken, NULL));
    assert(DELIVERED - FIRST_TAKE == taken && DELIVERED - FIRST_TAKE == messages.size && taken == infos.size);
    assert(FIRST_TAKE == storage[0].first && FIRST_TAKE + 1 == storage[1].first);

    // Empty: RMW_RET_OK, nothing taken, both sequences unchanged.
    messages.size = UNCHANGED;
    infos.size = UNCHANGED;
    taken = UNCHANGED;
    assert(RMW_RET_OK == rmw_take_sequence(sub, FIRST_TAKE, &messages, &infos, &taken, NULL));
    assert(0 == taken && UNCHANGED == messages.size && UNCHANGED == infos.size);

    // Refused: count 0, and a count past either sequence's capacity.
    deliver(sub, 0);
    assert(RMW_RET_INVALID_ARGUMENT == rmw_take_sequence(sub, 0, &messages, &infos, &taken, NULL));
    rmw_reset_error();
    assert(RMW_RET_INVALID_ARGUMENT == rmw_take_sequence(sub, CAPACITY + 1, &messages, &infos, &taken, NULL));
    rmw_reset_error();
    assert(UNCHANGED == messages.size && UNCHANGED == infos.size);

    messages.size = 0;
    infos.size = 0;
    assert(RMW_RET_OK == rmw_message_sequence_fini(&messages));
    assert(RMW_RET_OK == rmw_message_info_sequence_fini(&infos));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_take_sequence: PASS\n");
    return 0;
}
