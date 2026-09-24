/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle_check_callbacks_usable() (rmw_typesupport.c) at every entity creation: a type generated
// for a different tt_MAX_BUFFER_LENGTH, or whose TickLE struct outgrows one datagram, is refused
// with an error naming the type and both numbers - not accepted, and not left to core's generic
// "tt_Node_create_publisher() failed".
//
// Why both matter (2026-09-24): rmw_tickle is raising tt_MAX_BUFFER_LENGTH to 65507, set once in
// rosidl_typesupport_tickle_c and compiled into rmw_tickle and every generated interface library.
// An interface package left over from a build with another value has layouts sized for a different
// datagram; nothing else would notice. And a type too large for a datagram cannot be created by
// core at all - the error has to say which type and by how much.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE, tt_MAX_BUFFER_LENGTH

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

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

#define FAKE_CALLBACKS(type_name)                                                         \
    {                                                                                     \
        .ros_type_name = (type_name),                                                     \
        .tickle_struct_size = sizeof(struct fake_msg),                                    \
        .ros_struct_size = sizeof(struct fake_msg),                                       \
        .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function) & fake_convert,     \
        .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function) & fake_convert, \
        .tickle_encode_size = (tt_DATA_ENCODE_SIZE) & fake_encode_size,                   \
        .tickle_encode = (tt_DATA_ENCODE) & fake_encode,                                  \
        .tickle_decode = (tt_DATA_DECODE) & fake_decode,                                  \
        .tickle_free = (tt_DATA_FREE) & fake_free,                                        \
    }

// Each test edits these and restores them.
static rosidl_typesupport_tickle_c_message_callbacks_t msg_callbacks = FAKE_CALLBACKS("test_type_checks/msg/Fake");
static rosidl_typesupport_tickle_c_message_callbacks_t request_callbacks =
    FAKE_CALLBACKS("test_type_checks/srv/Fake_Request");
static rosidl_typesupport_tickle_c_message_callbacks_t response_callbacks =
    FAKE_CALLBACKS("test_type_checks/srv/Fake_Response");

static rosidl_message_type_support_t msg_handle = {.data = &msg_callbacks,
                                                   .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t request_handle = {.data = &request_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t response_handle = {.data = &response_callbacks,
                                                        .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_service_callbacks_t service_callbacks = {.ros_type_name =
                                                                                "test_type_checks/srv/Fake"};
static rosidl_service_type_support_t service_handle = {.data = &service_callbacks,
                                                       .func = get_service_typesupport_handle_function,
                                                       .request_typesupport = &request_handle,
                                                       .response_typesupport = &response_handle};

static rmw_qos_profile_t qos(void) {
    rmw_qos_profile_t q;
    memset(&q, 0, sizeof(q));
    q.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    q.depth = 1;
    q.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    q.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    q.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    q.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    q.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    q.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    return q;
}

static bool error_mentions(const char* needle) {
    bool found = NULL != strstr(rmw_get_error_string().str, needle);
    if (!found) {
        printf("  error was: %s\n", rmw_get_error_string().str);
    }
    rmw_reset_error();
    return found;
}

// Creates and destroys every kind of entity on these types; returns how many were accepted (0..4).
static int create_all(rmw_node_t* node) {
    rmw_qos_profile_t q = qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    int accepted = 0;
    rmw_publisher_t* pub = rmw_create_publisher(node, &msg_handle, "/type_checks", &q, &pub_opts);
    if (NULL != pub) {
        accepted++;
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }
    rmw_subscription_t* sub = rmw_create_subscription(node, &msg_handle, "/type_checks", &q, &sub_opts);
    if (NULL != sub) {
        accepted++;
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }
    rmw_client_t* client = rmw_create_client(node, &service_handle, "/type_checks_srv", &q);
    if (NULL != client) {
        accepted++;
        assert(RMW_RET_OK == rmw_destroy_client(node, client));
    }
    rmw_service_t* service = rmw_create_service(node, &service_handle, "/type_checks_srv", &q);
    if (NULL != service) {
        accepted++;
        assert(RMW_RET_OK == rmw_destroy_service(node, service));
    }
    rmw_reset_error();
    return accepted;
}

int main(void) {
    msg_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    request_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    response_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    service_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;

    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_type_checks", "/");
    assert(NULL != node);

    // Control: unknown (0, a hand-written or older type) and this build's own value are accepted.
    assert(4 == create_all(node));
    msg_callbacks.tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH;
    request_callbacks.tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH;
    response_callbacks.tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH;
    assert(4 == create_all(node));

    // A type generated for another buffer length is refused by all four, naming it.
    msg_callbacks.tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH + 1;
    rmw_qos_profile_t q = qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    assert(NULL == rmw_create_publisher(node, &msg_handle, "/type_checks", &q, &pub_opts));
    assert(error_mentions("test_type_checks/msg/Fake' was generated for tt_MAX_BUFFER_LENGTH"));
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    assert(NULL == rmw_create_subscription(node, &msg_handle, "/type_checks", &q, &sub_opts));
    assert(error_mentions("rebuild the interface package"));
    msg_callbacks.tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH;
    response_callbacks.tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH + 1; // one side is enough
    assert(NULL == rmw_create_service(node, &service_handle, "/type_checks_srv", &q));
    assert(error_mentions("test_type_checks/srv/Fake_Response' was generated for"));
    assert(NULL == rmw_create_client(node, &service_handle, "/type_checks_srv", &q));
    assert(error_mentions("test_type_checks/srv/Fake_Response' was generated for"));
    response_callbacks.tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH;
    assert(4 == create_all(node)); // and nothing was left half-created

    // A type whose TickLE struct outgrows one datagram is refused with its size, not core's
    // generic failure.
    msg_callbacks.tickle_struct_size = (size_t)tt_MAX_BUFFER_LENGTH + 1;
    assert(NULL == rmw_create_publisher(node, &msg_handle, "/type_checks", &q, &pub_opts));
    assert(error_mentions("more than one"));
    msg_callbacks.tickle_struct_size = (size_t)tt_MAX_BUFFER_LENGTH; // exactly one datagram is fine
    assert(2 <= create_all(node));
    msg_callbacks.tickle_struct_size = sizeof(struct fake_msg);
    request_callbacks.tickle_struct_size = (size_t)tt_MAX_BUFFER_LENGTH + 1;
    assert(NULL == rmw_create_service(node, &service_handle, "/type_checks_srv", &q));
    assert(error_mentions("test_type_checks/srv/Fake_Request' needs"));
    request_callbacks.tickle_struct_size = sizeof(struct fake_msg);

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_type_checks: PASS\n");
    return 0;
}
