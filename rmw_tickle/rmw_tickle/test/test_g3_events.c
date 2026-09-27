/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g3 (RMW_GAPS_PLAN.md): the five event types rmw_tickle lacked. MATCHED and INCOMPATIBLE_TYPE between local
// endpoints of one context (the remote half is the `matched` acceptance test): a same-type, compatible pair matches;
// an unmatch and a rematch are counted; another type is INCOMPATIBLE_TYPE, never matched; an RxO-incompatible pair
// does not match; rmw_*_count_matched_*() agree with the event. MESSAGE_LOST, whitebox: psn sequences handed to the
// subscription as core would deliver them, from writers core names in tt_Subscriber.last_source/last_entity_id.
// Mutants, each killed here: MATCHED not raised on unmatch; core's gap counter added to the psn count; the first
// message from a writer counted as a loss from psn 0; a reset on any backward psn; the predicate ignoring the type.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/event.h"
#include "rmw/events_statuses/incompatible_type.h"
#include "rmw/events_statuses/matched.h"
#include "rmw/events_statuses/message_lost.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

#if defined(__has_include)
#if __has_include("rmw/get_service_endpoint_info.h")
#include "rmw/get_service_endpoint_info.h"
#include "rmw/service_endpoint_info.h"
#include "rmw/service_endpoint_info_array.h"
#define HAVE_SERVICE_ENDPOINT_INFO 1
#endif
#endif

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

static rosidl_typesupport_tickle_c_message_callbacks_t pair_callbacks = PAIR_CALLBACKS("g3/msg/Pair");
static rosidl_typesupport_tickle_c_message_callbacks_t other_callbacks = PAIR_CALLBACKS("g3/msg/Other");
static rosidl_message_type_support_t pair_handle = {.data = &pair_callbacks,
                                                    .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t other_handle = {.data = &other_callbacks,
                                                     .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_message_callbacks_t request_callbacks = PAIR_CALLBACKS("g3/srv/Add_Request");
static rosidl_typesupport_tickle_c_message_callbacks_t response_callbacks = PAIR_CALLBACKS("g3/srv/Add_Response");
static rosidl_message_type_support_t request_handle = {.data = &request_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t response_handle = {.data = &response_callbacks,
                                                        .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_service_callbacks_t service_callbacks = {.ros_type_name = "g3/srv/Add"};
static rosidl_service_type_support_t service_handle = {.data = &service_callbacks,
                                                       .func = get_service_typesupport_handle_function,
                                                       .request_typesupport = &request_handle,
                                                       .response_typesupport = &response_handle};

#define TOPIC "/g3_topic"
#define WRITER_A 11U
#define WRITER_B 12U
#define WRITER_C 13U
#define WRITER_D 14U
#define SOURCE 2U
#define NOT_COUNTED 99 // what a count reads until a query sets it
#define LOST_QUEUE_DEPTH 100
#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static rmw_qos_profile_t best_effort(void) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    return qos;
}

static rmw_publisher_t* publisher(rmw_node_t* node, const rosidl_message_type_support_t* type) {
    rmw_qos_profile_t qos = best_effort();
    rmw_publisher_options_t options = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, type, TOPIC, &qos, &options);
    assert(NULL != pub);
    return pub;
}

static rmw_subscription_t* subscription(rmw_node_t* node, const rosidl_message_type_support_t* type, bool reliable) {
    rmw_qos_profile_t qos = reliable ? rmw_qos_profile_default : best_effort();
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, type, TOPIC, &qos, &options);
    assert(NULL != sub);
    return sub;
}

// The changes an event reported through its callback (g2) - what makes it ready, as rmw_wait() sees it too.
static size_t raised;
static void on_raised(const void* user_data, size_t count) {
    (void)user_data;
    raised += count;
}

static rmw_matched_status_t take_matched(rmw_event_t* event) {
    rmw_matched_status_t status;
    memset(&status, 0, sizeof(status));
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_event(event, &status, &taken));
    assert(taken);
    return status;
}

static int32_t take_incompatible_type(rmw_event_t* event) {
    rmw_incompatible_type_status_t status = {0, 0};
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_event(event, &status, &taken));
    return status.total_count;
}

static void test_matched_and_incompatible_type(rmw_node_t* node) {
    rmw_publisher_t* pub = publisher(node, &pair_handle);
    rmw_event_t pub_matched = rmw_get_zero_initialized_event();
    rmw_event_t pub_other_type = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&pub_matched, pub, RMW_EVENT_PUBLICATION_MATCHED));
    assert(RMW_RET_OK == rmw_publisher_event_init(&pub_other_type, pub, RMW_EVENT_PUBLISHER_INCOMPATIBLE_TYPE));
    size_t count = NOT_COUNTED;

    // A same-type, compatible Subscription: matched on both sides, and the query agrees.
    rmw_subscription_t* sub = subscription(node, &pair_handle, false);
    rmw_matched_status_t status = take_matched(&pub_matched);
    assert(1 == status.total_count && 1 == status.total_count_change && 1 == status.current_count &&
           1 == status.current_count_change);
    assert(RMW_RET_OK == rmw_publisher_count_matched_subscriptions(pub, &count) && 1 == count);
    rmw_event_t sub_matched = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_subscription_event_init(&sub_matched, sub, RMW_EVENT_SUBSCRIPTION_MATCHED));
    status = take_matched(&sub_matched);
    assert(1 == status.total_count && 1 == status.current_count);
    assert(RMW_RET_OK == rmw_subscription_count_matched_publishers(sub, &count) && 1 == count);

    // Another type on the topic: INCOMPATIBLE_TYPE on both sides, and no match.
    rmw_subscription_t* other = subscription(node, &other_handle, false);
    assert(1 == take_incompatible_type(&pub_other_type));
    rmw_event_t other_type = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_subscription_event_init(&other_type, other, RMW_EVENT_SUBSCRIPTION_INCOMPATIBLE_TYPE));
    assert(1 == take_incompatible_type(&other_type));
    assert(RMW_RET_OK == rmw_subscription_count_matched_publishers(other, &count) && 0 == count);
    assert(RMW_RET_OK == rmw_publisher_count_matched_subscriptions(pub, &count) && 1 == count);

    // RELIABLE requested, BEST_EFFORT offered: same type, but not matched.
    rmw_subscription_t* reliable = subscription(node, &pair_handle, true);
    assert(RMW_RET_OK == rmw_subscription_count_matched_publishers(reliable, &count) && 0 == count);
    assert(RMW_RET_OK == rmw_publisher_count_matched_subscriptions(pub, &count) && 1 == count);

    // Unmatch: the Subscription goes; the event is raised, current falls, total stays.
    raised = 0;
    assert(RMW_RET_OK == rmw_event_set_callback(&pub_matched, on_raised, NULL)); // nothing unread: no call yet
    assert(0 == raised);
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(1 == raised);
    assert(RMW_RET_OK == rmw_event_set_callback(&pub_matched, NULL, NULL));
    status = take_matched(&pub_matched);
    assert(1 == status.total_count && 0 == status.total_count_change && 0 == status.current_count &&
           -1 == status.current_count_change);

    // Rematch: counted again.
    sub = subscription(node, &pair_handle, false);
    status = take_matched(&pub_matched);
    assert(2 == status.total_count && 1 == status.total_count_change && 1 == status.current_count &&
           1 == status.current_count_change);

    assert(RMW_RET_OK == rmw_event_fini(&pub_matched));
    assert(RMW_RET_OK == rmw_event_fini(&pub_other_type));
    assert(RMW_RET_OK == rmw_event_fini(&sub_matched));
    assert(RMW_RET_OK == rmw_event_fini(&other_type));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, other));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, reliable));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    printf("matched: match, unmatch, rematch; another type and an incompatible QoS never match\n");
}

// One message with this psn, as core delivers it from writer (SOURCE, entity).
static void deliver(rmw_subscription_t* sub, uint32_t entity, uint64_t psn) {
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    uint8_t wire[RMW_TICKLE_PSN_BYTES + sizeof(struct pair)] = {0};
    uint32_t header = rmw_tickle_psn_write(psn, wire);
    struct tt_Data* data = sub_impl->topic.data_decode_inplace(wire, header + (uint32_t)sizeof(struct pair), true);
    assert(NULL != data);
    sub_impl->tickle_subscriber.last_source = SOURCE; // what core's record_delivery_order() sets before the call
    sub_impl->tickle_subscriber.last_entity_id = entity;
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, 0, 0, data);
}

static void deliver_all(rmw_subscription_t* sub, uint32_t entity, const uint64_t* psns, size_t count) {
    for (size_t i = 0; i < count; i++) {
        deliver(sub, entity, psns[i]);
    }
}

static rmw_message_lost_status_t take_lost(rmw_event_t* event) {
    rmw_message_lost_status_t status = {0, 0};
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_event(event, &status, &taken));
    return status;
}

static void test_message_lost(rmw_node_t* node) {
    rmw_qos_profile_t qos = best_effort();
    qos.depth = LOST_QUEUE_DEPTH;
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &pair_handle, "/g3_lost", &qos, &options);
    assert(NULL != sub);
    rmw_event_t lost = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_subscription_event_init(&lost, sub, RMW_EVENT_MESSAGE_LOST));

    // Writer A skips 3-4 and 7-9: five lost. Two of them are a RELIABLE gap core gave up on - counted once.
    const uint64_t a_first[] = {1, 2};
    deliver_all(sub, WRITER_A, a_first, COUNT_OF(a_first));
    ((rmw_tickle_subscriber_t*)sub->data)->tickle_subscriber.gap_abandoned += 2;
    const uint64_t a_rest[] = {5, 6, 10};
    deliver_all(sub, WRITER_A, a_rest, COUNT_OF(a_rest));
    rmw_message_lost_status_t status = take_lost(&lost);
    assert(5 == status.total_count && 5 == status.total_count_change);

    // Writer B joins late, at 100, interleaved with A: nothing lost before its first; then 102 is missing.
    const struct {
        uint32_t writer;
        uint64_t psn;
    } interleaved[] = {{WRITER_B, 100}, {WRITER_A, 11}, {WRITER_B, 101}, {WRITER_B, 103}};
    for (size_t i = 0; i < COUNT_OF(interleaved); i++) {
        deliver(sub, interleaved[i].writer, interleaved[i].psn);
    }
    status = take_lost(&lost);
    assert(6 == status.total_count && 1 == status.total_count_change);

    // Writer C: 3 arrives late. It was counted when 4 came, and is not counted again after it.
    const uint64_t c_psns[] = {1, 2, 4, 3, 5, 6};
    deliver_all(sub, WRITER_C, c_psns, COUNT_OF(c_psns));
    status = take_lost(&lost);
    assert(7 == status.total_count && 1 == status.total_count_change);

    // Writer D jumps back far - it started again: a new baseline, nothing lost.
    const uint64_t d_psns[] = {1000, 1001, 1, 2};
    deliver_all(sub, WRITER_D, d_psns, COUNT_OF(d_psns));
    status = take_lost(&lost);
    assert(7 == status.total_count && 0 == status.total_count_change);

    assert(RMW_RET_OK == rmw_event_fini(&lost));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    printf("message lost: 7 over four writers - gaps, a late join, a late arrival, a restart\n");
}

#ifdef HAVE_SERVICE_ENDPOINT_INFO
// Lyrical's service queries: one row per client and per server, with its node and kind.
static void expect_one_row(const rmw_service_endpoint_info_array_t* rows, rmw_endpoint_type_t type) {
    assert(1 == rows->size);
    const rmw_service_endpoint_info_t* row = &rows->info_array[0];
    assert(0 == strcmp("test_g3_events", row->node_name) && 0 == strcmp("/", row->node_namespace));
    assert(type == row->endpoint_type && 1 == row->endpoint_count && NULL != row->service_type);
}

static void test_service_endpoint_info(rmw_node_t* node) {
    rmw_qos_profile_t qos = rmw_qos_profile_services_default;
    rmw_service_t* service = rmw_create_service(node, &service_handle, "/g3_add", &qos);
    rmw_client_t* client = rmw_create_client(node, &service_handle, "/g3_add", &qos);
    assert(NULL != service && NULL != client);
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_service_endpoint_info_array_t clients = rmw_get_zero_initialized_service_endpoint_info_array();
    rmw_service_endpoint_info_array_t servers = rmw_get_zero_initialized_service_endpoint_info_array();
    assert(RMW_RET_OK == rmw_get_clients_info_by_service(node, &allocator, "/g3_add", false, &clients));
    assert(RMW_RET_OK == rmw_get_servers_info_by_service(node, &allocator, "/g3_add", false, &servers));
    expect_one_row(&clients, RMW_ENDPOINT_CLIENT);
    expect_one_row(&servers, RMW_ENDPOINT_SERVER);
    assert(RMW_RET_OK == rmw_service_endpoint_info_array_fini(&clients, &allocator));
    assert(RMW_RET_OK == rmw_service_endpoint_info_array_fini(&servers, &allocator));
    assert(RMW_RET_OK == rmw_destroy_client(node, client));
    assert(RMW_RET_OK == rmw_destroy_service(node, service));
    printf("service info: one client row and one server row, with their node\n");
}
#endif

int main(void) {
    pair_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    other_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    request_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    response_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    service_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    assert(rmw_event_type_is_supported(RMW_EVENT_PUBLICATION_MATCHED));
    assert(rmw_event_type_is_supported(RMW_EVENT_SUBSCRIPTION_MATCHED));
    assert(rmw_event_type_is_supported(RMW_EVENT_PUBLISHER_INCOMPATIBLE_TYPE));
    assert(rmw_event_type_is_supported(RMW_EVENT_SUBSCRIPTION_INCOMPATIBLE_TYPE));
    assert(rmw_event_type_is_supported(RMW_EVENT_MESSAGE_LOST));

    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_g3_events", "/");
    assert(NULL != node);

    test_matched_and_incompatible_type(node);
    test_message_lost(node);
#ifdef HAVE_SERVICE_ENDPOINT_INFO
    test_service_endpoint_info(node);
#endif

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_g3_events: PASS\n");
    return 0;
}
