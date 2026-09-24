/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The rest of the rmw API, so that every entry point rmw_implementation looks up exists
// (2026-09-25). Missing ones were harmless but loud: at every node start rmw_implementation logged
// "failed to resolve symbol '<name>' in shared library librmw_tickle.so" for each - 20 on jazzy,
// 22 on lyrical - and a caller of one got that instead of a reason.
//
// Two kinds, kept apart because they are different claims:
//
// Features TickLE has no counterpart for. These answer exactly what rmw_cyclonedds_cpp and
// rmw_fastrtps_cpp answer for a feature they do not support: RMW_RET_UNSUPPORTED, with the error
// set. Loaned-message allocations, content filters, network flow endpoints, dynamic messages.
//
// Real gaps, not yet implemented, which the upstream implementations do support: serialized
// messages (rosbag2 and `ros2 topic echo --raw` need them), the on-new-data callbacks rclcpp's
// events executor uses, and on lyrical the service endpoint queries. Also RMW_RET_UNSUPPORTED for
// now, each error saying it is a TickLE gap rather than an unsupported option.
//
// rmw_set_log_severity is implemented: it sets TickLE's own log level.

#include <stdbool.h>
#include <stddef.h>

#include <tickle/log.h>

#include "rcutils/allocator.h"
#include "rmw/dynamic_message_type_support.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/event_callback_type.h"
#include "rmw/get_network_flow_endpoints.h"
#include "rmw/network_flow_endpoint_array.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/serialized_message.h"
#include "rmw/subscription_content_filter_options.h"
#include "rmw/types.h"
#include "rosidl_dynamic_typesupport/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/sequence_bound.h"

// The NOLINT(readability-non-const-parameter) below: each output parameter's type is fixed by
// rmw.h's prototype, which these have to match.

#define UNSUPPORTED(name)                                        \
    do {                                                         \
        RMW_SET_ERROR_MSG(name ": not supported by rmw_tickle"); \
        return RMW_RET_UNSUPPORTED;                              \
    } while (0)

#define NOT_YET(name)                                                  \
    do {                                                               \
        RMW_SET_ERROR_MSG(name ": not implemented in rmw_tickle yet"); \
        return RMW_RET_UNSUPPORTED;                                    \
    } while (0)

rmw_ret_t rmw_set_log_severity(rmw_log_severity_t severity) {
    switch (severity) {
    case RMW_LOG_SEVERITY_DEBUG:
        tt_log_set_level(TT_LOG_DEBUG);
        return RMW_RET_OK;
    case RMW_LOG_SEVERITY_INFO:
        tt_log_set_level(TT_LOG_INFO);
        return RMW_RET_OK;
    case RMW_LOG_SEVERITY_WARN:
        tt_log_set_level(TT_LOG_WARNING);
        return RMW_RET_OK;
    case RMW_LOG_SEVERITY_ERROR:
    case RMW_LOG_SEVERITY_FATAL: // TickLE has nothing above ERROR
        tt_log_set_level(TT_LOG_ERROR);
        return RMW_RET_OK;
    default:
        RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("rmw_set_log_severity: invalid log severity %d", (int)severity);
        return RMW_RET_INVALID_ARGUMENT;
    }
}

// --- features TickLE has no counterpart for -------------------------------------------------------

rmw_ret_t rmw_init_publisher_allocation(const rosidl_message_type_support_t* type_support,
                                        const rosidl_runtime_c__Sequence__bound* message_bounds,
                                        rmw_publisher_allocation_t* allocation) {
    (void)type_support;
    (void)message_bounds;
    (void)allocation;
    UNSUPPORTED("rmw_init_publisher_allocation");
}

rmw_ret_t rmw_fini_publisher_allocation(rmw_publisher_allocation_t* allocation) {
    (void)allocation;
    UNSUPPORTED("rmw_fini_publisher_allocation");
}

rmw_ret_t rmw_init_subscription_allocation(const rosidl_message_type_support_t* type_support,
                                           const rosidl_runtime_c__Sequence__bound* message_bounds,
                                           rmw_subscription_allocation_t* allocation) {
    (void)type_support;
    (void)message_bounds;
    (void)allocation;
    UNSUPPORTED("rmw_init_subscription_allocation");
}

rmw_ret_t rmw_fini_subscription_allocation(rmw_subscription_allocation_t* allocation) {
    (void)allocation;
    UNSUPPORTED("rmw_fini_subscription_allocation");
}

rmw_ret_t rmw_get_serialized_message_size(const rosidl_message_type_support_t* type_support,
                                          const rosidl_runtime_c__Sequence__bound* message_bounds,
                                          size_t* size) { // NOLINT(readability-non-const-parameter)
    (void)type_support;
    (void)message_bounds;
    (void)size;
    UNSUPPORTED("rmw_get_serialized_message_size");
}

rmw_ret_t rmw_subscription_set_content_filter(rmw_subscription_t* subscription,
                                              const rmw_subscription_content_filter_options_t* options) {
    (void)subscription;
    (void)options;
    UNSUPPORTED("rmw_subscription_set_content_filter");
}

rmw_ret_t rmw_subscription_get_content_filter(const rmw_subscription_t* subscription, rcutils_allocator_t* allocator,
                                              rmw_subscription_content_filter_options_t* options) {
    (void)subscription;
    (void)allocator;
    (void)options;
    UNSUPPORTED("rmw_subscription_get_content_filter");
}

rmw_ret_t rmw_publisher_get_network_flow_endpoints(const rmw_publisher_t* publisher, rcutils_allocator_t* allocator,
                                                   rmw_network_flow_endpoint_array_t* network_flow_endpoint_array) {
    (void)publisher;
    (void)allocator;
    (void)network_flow_endpoint_array;
    UNSUPPORTED("rmw_publisher_get_network_flow_endpoints");
}

rmw_ret_t rmw_subscription_get_network_flow_endpoints(const rmw_subscription_t* subscription,
                                                      rcutils_allocator_t* allocator,
                                                      rmw_network_flow_endpoint_array_t* network_flow_endpoint_array) {
    (void)subscription;
    (void)allocator;
    (void)network_flow_endpoint_array;
    UNSUPPORTED("rmw_subscription_get_network_flow_endpoints");
}

rmw_ret_t rmw_take_dynamic_message(const rmw_subscription_t* subscription,
                                   rosidl_dynamic_typesupport_dynamic_data_t* dynamic_message,
                                   bool* taken, // NOLINT(readability-non-const-parameter)
                                   rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)dynamic_message;
    (void)taken;
    (void)allocation;
    UNSUPPORTED("rmw_take_dynamic_message");
}

rmw_ret_t rmw_take_dynamic_message_with_info(const rmw_subscription_t* subscription,
                                             rosidl_dynamic_typesupport_dynamic_data_t* dynamic_message,
                                             bool* taken, // NOLINT(readability-non-const-parameter)
                                             rmw_message_info_t* message_info,
                                             rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)dynamic_message;
    (void)taken;
    (void)message_info;
    (void)allocation;
    UNSUPPORTED("rmw_take_dynamic_message_with_info");
}

rmw_ret_t rmw_serialization_support_init(const char* serialization_lib_name, rcutils_allocator_t* allocator,
                                         rosidl_dynamic_typesupport_serialization_support_t* serialization_support) {
    (void)serialization_lib_name;
    (void)allocator;
    (void)serialization_support;
    UNSUPPORTED("rmw_serialization_support_init");
}

// --- real gaps: supported upstream, not yet here --------------------------------------------------

rmw_ret_t rmw_publish_serialized_message(const rmw_publisher_t* publisher,
                                         const rmw_serialized_message_t* serialized_message,
                                         rmw_publisher_allocation_t* allocation) {
    (void)publisher;
    (void)serialized_message;
    (void)allocation;
    NOT_YET("rmw_publish_serialized_message");
}

rmw_ret_t rmw_take_serialized_message(const rmw_subscription_t* subscription,
                                      rmw_serialized_message_t* serialized_message,
                                      bool* taken, // NOLINT(readability-non-const-parameter)
                                      rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)serialized_message;
    (void)taken;
    (void)allocation;
    NOT_YET("rmw_take_serialized_message");
}

rmw_ret_t rmw_take_serialized_message_with_info(const rmw_subscription_t* subscription,
                                                rmw_serialized_message_t* serialized_message,
                                                bool* taken, // NOLINT(readability-non-const-parameter)
                                                rmw_message_info_t* message_info,
                                                rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)serialized_message;
    (void)taken;
    (void)message_info;
    (void)allocation;
    NOT_YET("rmw_take_serialized_message_with_info");
}

// rclcpp's events executor and its set_on_new_*_callback() API use these; its default executors do
// not, so an ordinary node never reaches them.
rmw_ret_t rmw_subscription_set_on_new_message_callback(rmw_subscription_t* subscription, rmw_event_callback_t callback,
                                                       const void* user_data) {
    (void)subscription;
    (void)callback;
    (void)user_data;
    NOT_YET("rmw_subscription_set_on_new_message_callback");
}

rmw_ret_t rmw_service_set_on_new_request_callback(rmw_service_t* service, rmw_event_callback_t callback,
                                                  const void* user_data) {
    (void)service;
    (void)callback;
    (void)user_data;
    NOT_YET("rmw_service_set_on_new_request_callback");
}

rmw_ret_t rmw_client_set_on_new_response_callback(rmw_client_t* client, rmw_event_callback_t callback,
                                                  const void* user_data) {
    (void)client;
    (void)callback;
    (void)user_data;
    NOT_YET("rmw_client_set_on_new_response_callback");
}

rmw_ret_t rmw_event_set_callback(rmw_event_t* event, rmw_event_callback_t callback, const void* user_data) {
    (void)event;
    (void)callback;
    (void)user_data;
    NOT_YET("rmw_event_set_callback");
}

// Lyrical's rmw added these (and the header); jazzy's has neither, so they exist only where it does.
#if defined(__has_include)
#if __has_include("rmw/get_service_endpoint_info.h")
#include "rmw/get_service_endpoint_info.h"
#include "rmw/service_endpoint_info_array.h"

rmw_ret_t rmw_get_clients_info_by_service(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                          const char* service_name, bool no_mangle,
                                          rmw_service_endpoint_info_array_t* clients_info) {
    (void)node;
    (void)allocator;
    (void)service_name;
    (void)no_mangle;
    (void)clients_info;
    NOT_YET("rmw_get_clients_info_by_service");
}

rmw_ret_t rmw_get_servers_info_by_service(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                          const char* service_name, bool no_mangle,
                                          rmw_service_endpoint_info_array_t* servers_info) {
    (void)node;
    (void)allocator;
    (void)service_name;
    (void)no_mangle;
    (void)servers_info;
    NOT_YET("rmw_get_servers_info_by_service");
}
#endif
#endif
