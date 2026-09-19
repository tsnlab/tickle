/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's remaining-rmw-API-surface backlog (Milestone 15's own note) - exercises
// rmw_publisher_count_matched_subscriptions()/rmw_subscription_count_matched_publishers() (need a
// real, matched rmw_publisher_t/rmw_subscription_t pair on the same topic, unlike test_graph.c's
// own deliberately-raw tt_Publisher/tt_Subscriber approach - hence its own file rather than an
// extension there), rmw_feature_supported() (takes no node/publisher at all), and
// rmw_publisher_wait_for_all_acked() (BEST_EFFORT returns immediately; RELIABLE polls pub->peer_
// ack_seq_no[] against tt_Publisher_request_ack()'s own solicited response - see rmw_publisher.c's
// own doc comment on the mechanism). Same hand-built fake type support shape as test_events.c's
// own fake_type_support() - see its doc comment there.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/features.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

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
    .ros_type_name = "test_rmw_api_surface/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

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

// Deliberately not 0 - a sentinel a real RMW_RET_OK write would always overwrite, so leaving it
// unchanged would prove the function silently no-op'd instead of actually setting the count.
#define UNSET_COUNT_SENTINEL 999

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

int main(void) {
    // rmw_feature_supported() - no node/context needed at all.
    assert(true == rmw_feature_supported(RMW_FEATURE_MESSAGE_INFO_PUBLICATION_SEQUENCE_NUMBER));
    assert(true == rmw_feature_supported(RMW_FEATURE_MESSAGE_INFO_RECEPTION_SEQUENCE_NUMBER));
    assert(false == rmw_feature_supported(RMW_MIDDLEWARE_SUPPORTS_TYPE_DISCOVERY));
    assert(false == rmw_feature_supported(RMW_MIDDLEWARE_CAN_TAKE_DYNAMIC_MESSAGE));

    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_rmw_api_surface", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = fake_type_support();

    // A Publisher with no Subscription anywhere sees zero matches.
    rmw_qos_profile_t pub_qos = base_qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "matched_topic", &pub_qos, &pub_opts);
    assert(NULL != pub);

    size_t subscription_count = UNSET_COUNT_SENTINEL;
    assert(RMW_RET_OK == rmw_publisher_count_matched_subscriptions(pub, &subscription_count));
    assert(0U == subscription_count);

    // rmw_publisher_wait_for_all_acked() - BEST_EFFORT (this Publisher's own qos) returns
    // immediately, real spec behavior: nothing to acknowledge for BEST_EFFORT at all.
    rmw_time_t zero_wait = {0, 0};
    assert(RMW_RET_OK == rmw_publisher_wait_for_all_acked(pub, zero_wait));

    // A Subscription on the *same* topic must now show up on both sides.
    rmw_qos_profile_t sub_qos = base_qos();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "matched_topic", &sub_qos, &sub_opts);
    assert(NULL != sub);

    assert(RMW_RET_OK == rmw_publisher_count_matched_subscriptions(pub, &subscription_count));
    assert(1U == subscription_count);

    size_t publisher_count = UNSET_COUNT_SENTINEL;
    assert(RMW_RET_OK == rmw_subscription_count_matched_publishers(sub, &publisher_count));
    assert(1U == publisher_count);

    // A Subscription on an *unrelated* topic sees neither side of the pair above.
    rmw_subscription_t* unrelated_sub =
        rmw_create_subscription(node, type_support, "unrelated_topic", &sub_qos, &sub_opts);
    assert(NULL != unrelated_sub);
    assert(RMW_RET_OK == rmw_subscription_count_matched_publishers(unrelated_sub, &publisher_count));
    assert(0U == publisher_count);

    // RMW_QOS_POLICY_RELIABILITY_RELIABLE - two cases this single process can actually exercise:
    // nothing published yet (vacuously "all acked" - nothing to wait on), and something published
    // but no wire-level peer to wait on either. A co-located Subscription on the same tt_Node
    // deliberately never becomes a wire-level peer at all here - a reliable Subscriber never even
    // sees its own co-located Publisher's DATA in the first place (self_sent-suppressed in process_
    // submessage(), tickle.c: "a reliable Subscriber never sees its own co-located Publisher's DATA
    // in the first place, so it never has anything to ack locally either"), so pub->peers[] stays
    // empty regardless of how many Subscriptions rmw_publisher_count_matched_subscriptions() (a
    // separate, discovery/local-endpoint-table-based count, rmw_graph.c - not pub->peers[] at all)
    // reports matched. Exercising the real "a genuinely unresponsive *wire* peer times out" and "a
    // peer that does ACKNACK succeeds" paths needs an actual second node_id/process - already
    // covered at the mechanism's own level by TickLE core's tests/test_heartbeat.c (tt_Publisher_
    // request_ack(), tt_HEARTBEAT_FLAG_FINAL) and tests/test_reliable_pubsub.c (peer_ack_seq_no[]
    // aggregation), not re-derived here.
    rmw_qos_profile_t reliable_qos = base_qos();
    reliable_qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    rmw_publisher_t* reliable_pub =
        rmw_create_publisher(node, type_support, "reliable_topic", &reliable_qos, &pub_opts);
    assert(NULL != reliable_pub);
    assert(RMW_RET_OK == rmw_publisher_wait_for_all_acked(reliable_pub, zero_wait));

    struct fake_ros_msg published_msg = {.value = 5}; // arbitrary - .clang-tidy's own ignored-magic-numbers list
    assert(RMW_RET_OK == rmw_publish(reliable_pub, &published_msg, NULL));
    assert(RMW_RET_OK == rmw_publisher_wait_for_all_acked(reliable_pub, zero_wait));

    assert(RMW_RET_OK == rmw_destroy_publisher(node, reliable_pub));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, unrelated_sub));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_rmw_api_surface: all tests passed\n");
    return 0;
}
