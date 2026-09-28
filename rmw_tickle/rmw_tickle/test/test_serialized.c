/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g1 (RMW_GAPS_PLAN.md, LARGE_MESSAGE_PLAN.md stage 1): serialized messages - what `ros2 bag
// record` and `ros2 bag play` need. Its pass criteria, in the order they were pre-registered:
//
//   1. rmw_serialize then rmw_deserialize gives the message back, and rmw_get_serialized_message_
//      size answers for a bounded type and refuses for an unbounded one rather than guessing.
//   2. Serialized out, ordinary in: what rmw_publish_serialized_message() sends, rmw_take() takes
//      as a ROS message.
//   3. Ordinary out, serialized in: what rmw_publish() sends, rmw_take_serialized_message() hands
//      back as bytes identical to rmw_serialize() of that message - so the psn header is not in
//      them, and nothing else is either.
//
// Criterion 4, the golden bytes, is checked by check_ros2_interfaces.sh against real ROS 2 types:
// this file's type is its own, so it has no bytes from before the change to compare with.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rcutils/types/rcutils_ret.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/serialized_message.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define MAX_TEXT 64
#define BOUNDED_SIZE 32
#define SERIALIZED_PSN 41ULL // the psn a serialized publish carries
#define ORDINARY_PSN 42ULL   // and an ordinary one

struct ser_ros_msg {
    char* text;
};

// The wire form: a uint16 length including the NUL, then the bytes - the shape the generator
// produces for a string, so the questions asked here are the ones a real type asks.
static int32_t ser_direct_encode_size(const void* ros_message) {
    const struct ser_ros_msg* ros = ros_message;
    const char* text = ros->text != NULL ? ros->text : "";
    return (int32_t)(2 + strlen(text) + 1);
}

static int32_t ser_direct_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    const struct ser_ros_msg* ros = ros_message;
    const char* text = ros->text != NULL ? ros->text : "";
    size_t bytes = strlen(text) + 1;
    if (len < 2 + bytes) {
        return -1;
    }
    uint16_t prefix = (uint16_t)bytes;
    memcpy(payload, &prefix, 2);
    memcpy(payload + 2, text, bytes);
    return (int32_t)(2 + bytes);
}

static int32_t ser_direct_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    struct ser_ros_msg* ros = ros_message;
    if (len < 2) {
        return -1;
    }
    uint16_t prefix = 0;
    memcpy(&prefix, payload, 2);
    if (prefix == 0 || len < (uint32_t)2 + prefix || payload[2 + prefix - 1] != '\0') {
        return -1;
    }
    free(ros->text);
    ros->text = strdup((const char*)(payload + 2));
    return ros->text != NULL ? (int32_t)(2 + prefix) : -4;
}

static void ser_ros_fini(void* ros_message) {
    struct ser_ros_msg* ros = ros_message;
    free(ros->text);
    ros->text = NULL;
}

static int32_t refuse_encode_size(struct tt_Data* data) {
    (void)data;
    return -1;
}

static rosidl_typesupport_tickle_c_message_callbacks_t ser_callbacks = {
    .struct_size = sizeof(rosidl_typesupport_tickle_c_message_callbacks_t),
    .ros_type_name = "test_serialized/msg/SerMsg",
    .tickle_struct_size = MAX_TEXT,
    .ros_struct_size = sizeof(struct ser_ros_msg),
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .tickle_max_encoded_size = ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED,
    .direct_encode_size = &ser_direct_encode_size,
    .direct_encode = &ser_direct_encode,
    .direct_decode = &ser_direct_decode,
    .ros_fini = &ser_ros_fini,
};

// The same type with a bound, so rmw_get_serialized_message_size() has something to answer for.
static rosidl_typesupport_tickle_c_message_callbacks_t bounded_callbacks;

static rosidl_message_type_support_t ser_handle = {
    .data = &ser_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static rosidl_message_type_support_t bounded_handle = {
    .data = &bounded_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* ser_type_support(void) {
    ser_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    return &ser_handle;
}

static const rosidl_message_type_support_t* bounded_type_support(void) {
    bounded_callbacks = ser_callbacks;
    bounded_callbacks.tickle_max_encoded_size = BOUNDED_SIZE;
    bounded_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    return &bounded_handle;
}

static rmw_qos_profile_t base_qos(void) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = 4;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    qos.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    qos.avoid_ros_namespace_conventions = false;
    return qos;
}

// Hands whatever the publisher's own encoder produced to the subscription, as core's poll thread
// would - the wire path in one process, header and all.
static void deliver(rmw_tickle_subscriber_t* sub_impl, rmw_tickle_publisher_t* pub_impl,
                    rmw_tickle_outgoing_message_t* outgoing) {
    uint8_t wire[MAX_TEXT + RMW_TICKLE_PSN_BYTES];
    int32_t size = pub_impl->topic.data_encode((struct tt_Data*)outgoing, wire, (uint32_t)sizeof(wire));
    assert(size > 0);
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, 0, 1,
                                         sub_impl->topic.data_decode_inplace(wire, (uint32_t)size, true));
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_serialized", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = ser_type_support();
    rmw_qos_profile_t qos = base_qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "ser_topic", &qos, &pub_opts);
    rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "ser_topic", &qos, &sub_opts);
    assert(NULL != pub && NULL != sub);
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;

    // 1. Round trip through the two serialize entry points.
    struct ser_ros_msg original = {.text = (char*)"serialize-me"};
    rmw_serialized_message_t bytes = rmw_get_zero_initialized_serialized_message();
    assert(RCUTILS_RET_OK == rmw_serialized_message_init(&bytes, 0, &allocator));
    assert(RMW_RET_OK == rmw_serialize(&original, type_support, &bytes));
    assert(bytes.buffer_length == (size_t)ser_direct_encode_size(&original));
    struct ser_ros_msg restored = {.text = NULL};
    assert(RMW_RET_OK == rmw_deserialize(&bytes, type_support, &restored));
    assert(NULL != restored.text && 0 == strcmp(original.text, restored.text));
    free(restored.text);
    restored.text = NULL;

    // 1b. The size question: answered for a bounded type, refused rather than guessed for one whose
    // length depends on the message.
    size_t answered = 0;
    assert(RMW_RET_OK == rmw_get_serialized_message_size(bounded_type_support(), NULL, &answered));
    assert(BOUNDED_SIZE == answered);
    assert(RMW_RET_OK != rmw_get_serialized_message_size(type_support, NULL, &answered));
    rmw_reset_error();

    // 2. Serialized out, ordinary in.
    {
        rmw_tickle_outgoing_message_t outgoing = {.publication_sequence_number = SERIALIZED_PSN,
                                                  .callbacks = &ser_callbacks,
                                                  .ros_message = NULL,
                                                  .tickle = NULL,
                                                  .serialized = bytes.buffer,
                                                  .serialized_len = bytes.buffer_length};
        deliver(sub_impl, pub_impl, &outgoing);
        struct ser_ros_msg incoming = {.text = NULL};
        bool taken = false;
        rmw_message_info_t info = rmw_get_zero_initialized_message_info();
        assert(RMW_RET_OK == rmw_take_with_info(sub, &incoming, &taken, &info, NULL));
        assert(taken);
        assert(0 == strcmp(original.text, incoming.text)); // the psn header was written and read
        assert(SERIALIZED_PSN == info.publication_sequence_number);
        free(incoming.text);
    }

    // 3. Ordinary out, serialized in: the same bytes rmw_serialize() gives, header stripped.
    {
        rmw_tickle_outgoing_message_t outgoing = {.publication_sequence_number = ORDINARY_PSN,
                                                  .callbacks = &ser_callbacks,
                                                  .ros_message = &original,
                                                  .tickle = NULL,
                                                  .serialized = NULL,
                                                  .serialized_len = 0};
        deliver(sub_impl, pub_impl, &outgoing);
        rmw_serialized_message_t taken_bytes = rmw_get_zero_initialized_serialized_message();
        assert(RCUTILS_RET_OK == rmw_serialized_message_init(&taken_bytes, 0, &allocator));
        bool taken = false;
        rmw_message_info_t info = rmw_get_zero_initialized_message_info();
        assert(RMW_RET_OK == rmw_take_serialized_message_with_info(sub, &taken_bytes, &taken, &info, NULL));
        assert(taken);
        assert(taken_bytes.buffer_length == bytes.buffer_length);
        assert(0 == memcmp(taken_bytes.buffer, bytes.buffer, bytes.buffer_length));
        assert(ORDINARY_PSN == info.publication_sequence_number);
        assert(RCUTILS_RET_OK == rmw_serialized_message_fini(&taken_bytes));
    }

    // And an empty queue answers "nothing taken" rather than failing.
    {
        rmw_serialized_message_t empty = rmw_get_zero_initialized_serialized_message();
        assert(RCUTILS_RET_OK == rmw_serialized_message_init(&empty, 0, &allocator));
        bool taken = true;
        assert(RMW_RET_OK == rmw_take_serialized_message(sub, &empty, &taken, NULL));
        assert(!taken);
        assert(RCUTILS_RET_OK == rmw_serialized_message_fini(&empty));
    }

    assert(RCUTILS_RET_OK == rmw_serialized_message_fini(&bytes));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_serialized: PASS\n");
    return 0;
}
