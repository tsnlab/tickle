/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 37 - the "names and types" function family Milestone 33 deferred:
// rmw_get_topic_names_and_types()/_service_names_and_types(), the four *_by_node() variants, and
// rmw_get_publishers_info_by_topic()/_subscriptions_info_by_topic(). Needs a real, matched
// rmw_publisher_t/rmw_subscription_t pair AND a real rmw_client_t/rmw_service_t pair (owning_
// node_name only gets populated by the real rmw_create_*() entry points, not by test_graph.c's own
// deliberately-raw tt_Publisher/tt_Subscriber approach) - hence its own file, same reasoning as
// test_rmw_api_surface.c's own doc comment. Fake message type support shape reused verbatim from
// there; fake service type support built the same way, wrapping two fake message type supports.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/get_node_info_and_types.h"
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/names_and_types.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/topic_endpoint_info.h"
#include "rmw/topic_endpoint_info_array.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

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

static rosidl_typesupport_tickle_c_message_callbacks_t fake_topic_callbacks = {
    .ros_type_name = "test_names_and_types/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

static rosidl_message_type_support_t fake_topic_handle = {
    .data = &fake_topic_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* fake_topic_type_support(void) {
    if (NULL == fake_topic_handle.typesupport_identifier) {
        fake_topic_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    }
    return &fake_topic_handle;
}

// Request and response happen to share the exact same wire shape here (one uint8_t) - only their
// own ros_type_name strings differ, which is all rmw_get_*_names_and_types() family callers below
// ever actually look at.
static rosidl_typesupport_tickle_c_message_callbacks_t fake_request_callbacks = {
    .ros_type_name = "test_names_and_types/srv/FakeSrv_Request",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};
static rosidl_typesupport_tickle_c_message_callbacks_t fake_response_callbacks = {
    .ros_type_name = "test_names_and_types/srv/FakeSrv_Response",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};
static rosidl_message_type_support_t fake_request_handle = {
    .data = &fake_request_callbacks,
    .func = get_message_typesupport_handle_function,
};
static rosidl_message_type_support_t fake_response_handle = {
    .data = &fake_response_callbacks,
    .func = get_message_typesupport_handle_function,
};
static rosidl_typesupport_tickle_c_service_callbacks_t fake_service_callbacks = {
    .ros_type_name = "test_names_and_types/srv/FakeSrv",
};
static rosidl_service_type_support_t fake_service_handle = {
    .data = &fake_service_callbacks,
    .func = get_service_typesupport_handle_function,
    .request_typesupport = &fake_request_handle,
    .response_typesupport = &fake_response_handle,
};

static const rosidl_service_type_support_t* fake_service_type_support(void) {
    if (NULL == fake_service_handle.typesupport_identifier) {
        fake_service_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
        fake_request_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
        fake_response_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    }
    return &fake_service_handle;
}

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

static bool names_and_types_has(const rmw_names_and_types_t* names_and_types, const char* name, const char* type) {
    for (size_t i = 0; i < names_and_types->names.size; i++) {
        if (strcmp(names_and_types->names.data[i], name) == 0) {
            for (size_t j = 0; j < names_and_types->types[i].size; j++) {
                if (strcmp(names_and_types->types[i].data[j], type) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_names_and_types_node", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* topic_type_support = fake_topic_type_support();
    rmw_qos_profile_t pub_qos = base_qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, topic_type_support, "/test_names_topic", &pub_qos, &pub_opts);
    assert(NULL != pub);

    rmw_qos_profile_t sub_qos = base_qos();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_subscription_t* sub =
        rmw_create_subscription(node, topic_type_support, "/test_names_topic", &sub_qos, &sub_opts);
    assert(NULL != sub);

    const rosidl_service_type_support_t* service_type_support = fake_service_type_support();
    rmw_qos_profile_t service_qos = base_qos();
    rmw_client_t* client = rmw_create_client(node, service_type_support, "/test_names_service", &service_qos);
    assert(NULL != client);
    rmw_service_t* server = rmw_create_service(node, service_type_support, "/test_names_service", &service_qos);
    assert(NULL != server);

    // rmw_get_topic_names_and_types() reports the topic once, with its real type - not twice just
    // because both a Publisher and a Subscription exist on it (add_name_type_entry()'s own
    // dedup-by-(name,type) contract).
    rmw_names_and_types_t topic_nt = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_topic_names_and_types(node, &allocator, false, &topic_nt));
    assert(names_and_types_has(&topic_nt, "/test_names_topic", "test_names_and_types/msg/FakeMsg"));
    size_t topic_occurrences = 0;
    for (size_t i = 0; i < topic_nt.names.size; i++) {
        if (strcmp(topic_nt.names.data[i], "/test_names_topic") == 0) {
            topic_occurrences++;
        }
    }
    assert(1U == topic_occurrences);
    assert(RMW_RET_OK == rmw_names_and_types_fini(&topic_nt));

    // rmw_get_service_names_and_types() reports the *service's own* type name (FakeSrv), not
    // either side's own request/response message type.
    rmw_names_and_types_t service_nt = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_service_names_and_types(node, &allocator, &service_nt));
    assert(names_and_types_has(&service_nt, "/test_names_service", "test_names_and_types/srv/FakeSrv"));
    assert(RMW_RET_OK == rmw_names_and_types_fini(&service_nt));

    // The four *_by_node() queries, against this same process's own node - the only case TickLE
    // can truthfully answer (this file's own module doc comment).
    rmw_names_and_types_t pub_by_node = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_publisher_names_and_types_by_node(node, &allocator, "test_names_and_types_node", "/",
                                                                   false, &pub_by_node));
    assert(names_and_types_has(&pub_by_node, "/test_names_topic", "test_names_and_types/msg/FakeMsg"));
    assert(RMW_RET_OK == rmw_names_and_types_fini(&pub_by_node));

    rmw_names_and_types_t sub_by_node = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_subscriber_names_and_types_by_node(node, &allocator, "test_names_and_types_node", "/",
                                                                    false, &sub_by_node));
    assert(names_and_types_has(&sub_by_node, "/test_names_topic", "test_names_and_types/msg/FakeMsg"));
    assert(RMW_RET_OK == rmw_names_and_types_fini(&sub_by_node));

    rmw_names_and_types_t server_by_node = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_service_names_and_types_by_node(node, &allocator, "test_names_and_types_node", "/",
                                                                 &server_by_node));
    assert(names_and_types_has(&server_by_node, "/test_names_service", "test_names_and_types/srv/FakeSrv"));
    assert(RMW_RET_OK == rmw_names_and_types_fini(&server_by_node));

    rmw_names_and_types_t client_by_node = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK ==
           rmw_get_client_names_and_types_by_node(node, &allocator, "test_names_and_types_node", "/", &client_by_node));
    assert(names_and_types_has(&client_by_node, "/test_names_service", "test_names_and_types/srv/FakeSrv"));
    assert(RMW_RET_OK == rmw_names_and_types_fini(&client_by_node));

    // A node name TickLE genuinely has never heard of - not this process's own, not any remote
    // node either (TickLE's wire protocol has no node-name concept at all) - is honestly reported
    // as non-existent, not silently treated as "found, but empty".
    rmw_names_and_types_t nonexistent_nt = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_NODE_NAME_NON_EXISTENT ==
           rmw_get_publisher_names_and_types_by_node(node, &allocator, "no_such_node", "/", false, &nonexistent_nt));
    assert(RMW_RET_OK == rmw_names_and_types_check_zero(&nonexistent_nt));

    // rmw_get_publishers_info_by_topic()/_subscriptions_info_by_topic() - real per-endpoint rows,
    // with this process's own real node name/namespace attached (Milestone 34's own owning_node_
    // name/_namespace, consumed for the first time here).
    rmw_topic_endpoint_info_array_t publishers_info = rmw_get_zero_initialized_topic_endpoint_info_array();
    assert(RMW_RET_OK ==
           rmw_get_publishers_info_by_topic(node, &allocator, "/test_names_topic", false, &publishers_info));
    assert(1U == publishers_info.size);
    assert(0 == strcmp("test_names_and_types_node", publishers_info.info_array[0].node_name));
    assert(0 == strcmp("/", publishers_info.info_array[0].node_namespace));
    assert(0 == strcmp("test_names_and_types/msg/FakeMsg", publishers_info.info_array[0].topic_type));
    assert(RMW_ENDPOINT_PUBLISHER == publishers_info.info_array[0].endpoint_type);
    assert(RMW_RET_OK == rmw_topic_endpoint_info_array_fini(&publishers_info, &allocator));

    rmw_topic_endpoint_info_array_t subscriptions_info = rmw_get_zero_initialized_topic_endpoint_info_array();
    assert(RMW_RET_OK ==
           rmw_get_subscriptions_info_by_topic(node, &allocator, "/test_names_topic", false, &subscriptions_info));
    assert(1U == subscriptions_info.size);
    assert(RMW_ENDPOINT_SUBSCRIPTION == subscriptions_info.info_array[0].endpoint_type);
    assert(RMW_RET_OK == rmw_topic_endpoint_info_array_fini(&subscriptions_info, &allocator));

    // A topic nobody publishes or subscribes to returns a real, empty array - not an error (its
    // own doc comment: "Names of non-existent topics are allowed").
    rmw_topic_endpoint_info_array_t empty_info = rmw_get_zero_initialized_topic_endpoint_info_array();
    assert(RMW_RET_OK == rmw_get_publishers_info_by_topic(node, &allocator, "/no_such_topic", false, &empty_info));
    assert(0U == empty_info.size);
    assert(RMW_RET_OK == rmw_topic_endpoint_info_array_fini(&empty_info, &allocator));

    assert(RMW_RET_OK == rmw_destroy_service(node, server));
    assert(RMW_RET_OK == rmw_destroy_client(node, client));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_names_and_types: all tests passed\n");
    return 0;
}
