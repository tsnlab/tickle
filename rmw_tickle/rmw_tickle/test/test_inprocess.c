/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g9 (RMW_GAPS_PLAN.md): topics between two nodes of one process, which share one context. Checked: a 60 KB message
// arrives whole in-process; KEEP_LAST depth 3 with 10 published before any take keeps the newest 3, as for remote
// samples; a TRANSIENT_LOCAL subscription (depth 2) created after 4 publishes gets the last 2 from the durable backlog,
// in order and whole - each goes as ~42 fragments, so the backlog reassembles them. (Depth 2, not 4: a durable
// publisher of a type this large starts with an arena of one sample and grows only once depth messages have gone out,
// so at depth 4 after 4 publishes it retains one - for a remote late joiner as much as a local one; reported to Plan.)
// (Services within a process already worked; test_service_roundtrip keeps them from regressing.)

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define BIG_BYTES 60000
#define DEPTH 3
#define BURST 10
#define BACKLOG 4
#define DURABLE_DEPTH 2
#define FIRST_DURABLE_ID 100U

struct big {
    uint32_t id;
    uint8_t bytes[BIG_BYTES];
};

static bool convert(const void* source, void* dest) {
    memcpy(dest, source, sizeof(struct big));
    return true;
}
static int32_t encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(struct big);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(struct big)) {
        return -1;
    }
    memcpy(payload, data, sizeof(struct big));
    return (int32_t)sizeof(struct big);
}
static int32_t decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct big)) {
        return -1;
    }
    memcpy(data, payload, sizeof(struct big));
    return (int32_t)sizeof(struct big);
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t big_callbacks = {
    .ros_type_name = "g9/msg/Big",
    .tickle_struct_size = sizeof(struct big),
    .ros_struct_size = sizeof(struct big),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&encode,
    .tickle_decode = (tt_DATA_DECODE)&decode,
    .tickle_free = (tt_DATA_FREE)&free_nothing,
    .tickle_max_encoded_size = sizeof(struct big),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
};
static rosidl_message_type_support_t big_handle = {.data = &big_callbacks,
                                                   .func = get_message_typesupport_handle_function};

static struct big outgoing;
static struct big incoming;

static void fill(struct big* message, uint32_t id) {
    message->id = id;
    for (uint32_t i = 0; i < BIG_BYTES; i++) {
        message->bytes[i] = (uint8_t)(id + i);
    }
}
static bool intact(const struct big* message) {
    for (uint32_t i = 0; i < BIG_BYTES; i++) {
        if (message->bytes[i] != (uint8_t)(message->id + i)) {
            return false;
        }
    }
    return true;
}

static rmw_publisher_t* publisher(rmw_node_t* node, const char* topic, rmw_qos_profile_t qos) {
    rmw_publisher_options_t options = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, &big_handle, topic, &qos, &options);
    assert(NULL != pub);
    return pub;
}
static rmw_subscription_t* subscription(rmw_node_t* node, const char* topic, rmw_qos_profile_t qos) {
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &big_handle, topic, &qos, &options);
    assert(NULL != sub);
    return sub;
}
// The ids a subscription holds, taken in order; how many.
static int take_all(rmw_subscription_t* sub, uint32_t* ids, int max) {
    int count = 0;
    bool taken = true;
    while (taken) {
        assert(RMW_RET_OK == rmw_take(sub, &incoming, &taken, NULL));
        if (taken) {
            assert(intact(&incoming));
            if (count < max) {
                ids[count] = incoming.id;
            }
            count++;
        }
    }
    return count;
}

int main(void) {
    big_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* talker = rmw_create_node(&context, "g9_talker", "/");
    rmw_node_t* listener = rmw_create_node(&context, "g9_listener", "/");
    assert(NULL != talker && NULL != listener);
    uint32_t ids[BURST];

    // A 60 KB message, whole, and KEEP_LAST at the subscription's depth.
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.depth = DEPTH;
    rmw_subscription_t* sub = subscription(listener, "/g9_big", qos);
    rmw_publisher_t* pub = publisher(talker, "/g9_big", qos);
    for (uint32_t i = 0; i < BURST; i++) {
        fill(&outgoing, i);
        assert(RMW_RET_OK == rmw_publish(pub, &outgoing, NULL));
    }
    int got = take_all(sub, ids, BURST);
    assert(DEPTH == got);
    for (int i = 0; i < DEPTH; i++) {
        assert((uint32_t)(BURST - DEPTH + i) == ids[i]);
    }
    printf("in-process: %d of %d 60 KB messages kept at depth %d, the newest, intact\n", got, BURST, DEPTH);
    assert(RMW_RET_OK == rmw_destroy_publisher(talker, pub));
    assert(RMW_RET_OK == rmw_destroy_subscription(listener, sub));

    // TRANSIENT_LOCAL: a subscription created after the publishes gets them from the durable backlog.
    rmw_qos_profile_t durable = rmw_qos_profile_default;
    durable.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
    durable.depth = DURABLE_DEPTH;
    pub = publisher(talker, "/g9_durable", durable);
    for (uint32_t i = 0; i < BACKLOG; i++) {
        fill(&outgoing, FIRST_DURABLE_ID + i);
        assert(RMW_RET_OK == rmw_publish(pub, &outgoing, NULL));
    }
    sub = subscription(listener, "/g9_durable", durable);
    got = take_all(sub, ids, BURST);
    assert(DURABLE_DEPTH == got);
    for (int i = 0; i < DURABLE_DEPTH; i++) {
        assert(FIRST_DURABLE_ID + (uint32_t)(BACKLOG - DURABLE_DEPTH + i) == ids[i]);
    }
    printf("in-process: a late durable subscription got the last %d of %d 60 KB messages, in order, whole\n", got,
           BACKLOG);
    assert(RMW_RET_OK == rmw_destroy_publisher(talker, pub));
    assert(RMW_RET_OK == rmw_destroy_subscription(listener, sub));

    assert(RMW_RET_OK == rmw_destroy_node(listener));
    assert(RMW_RET_OK == rmw_destroy_node(talker));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_inprocess: PASS\n");
    return 0;
}
