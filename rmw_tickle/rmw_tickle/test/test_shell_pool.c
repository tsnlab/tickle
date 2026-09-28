/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The pooled ROS shells a subscription decodes into (LARGE_MESSAGE_PLAN.md stage 1, its risk list):
// how many can exist at once, what happens when the queue is full, and that a shell handed back out
// carries nothing of the sample before it.
//
// The type here has a direct codec, so this is the path a generated type takes: the payload is
// decoded straight into a shell from the pool, with no TickLE struct in between. Its decoder writes
// every field, which is what makes reuse safe - the property the generator's own `keep_shell_tail`
// mutant breaks and the pass-1 harness's check 1b catches per type. What this test adds is the pool
// itself: its bound, its behaviour when the queue overflows, and that nothing leaks.

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
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define QUEUE_DEPTH 4
#define BURST_FIRST_PSN 10ULL
#define RUBBISH_SEQ_NO 99
#define MAX_TEXT 64

// One heap-owned, variable-length field: the shape a stale tail would show in.
struct pool_ros_msg {
    char* text;
};

// The wire form: a uint16 length including the NUL, then the bytes. Deliberately the same shape the
// generated codec produces for a string, so the reuse question is the same one.
static int32_t pool_direct_encode_size(const void* ros_message) {
    const struct pool_ros_msg* ros = ros_message;
    const char* text = ros->text != NULL ? ros->text : "";
    return (int32_t)(2 + strlen(text) + 1);
}

static int32_t pool_direct_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    const struct pool_ros_msg* ros = ros_message;
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

// Writes the whole field every time - free what the shell already held, then take this sample's.
static int32_t pool_direct_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    struct pool_ros_msg* ros = ros_message;
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

static int32_t refuse_encode_size(struct tt_Data* data) {
    (void)data;
    return -1;
}

static rosidl_typesupport_tickle_c_message_callbacks_t pool_callbacks = {
    .struct_size = sizeof(rosidl_typesupport_tickle_c_message_callbacks_t),
    .ros_type_name = "test_shell_pool/msg/PoolMsg",
    .tickle_struct_size = MAX_TEXT,
    .ros_struct_size = sizeof(struct pool_ros_msg),
    // No to_tickle/from_tickle: with a direct codec neither is on the path any more.
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .direct_encode_size = &pool_direct_encode_size,
    .direct_encode = &pool_direct_encode,
    .direct_decode = &pool_direct_decode,
};

static rosidl_message_type_support_t pool_handle = {
    .data = &pool_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* pool_type_support(void) {
    pool_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    return &pool_handle;
}

static rmw_qos_profile_t base_qos(void) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = QUEUE_DEPTH;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    qos.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    qos.avoid_ros_namespace_conventions = false;
    return qos;
}

// Hands the subscription one message, as core's poll thread would.
static void deliver(rmw_tickle_subscriber_t* sub_impl, rmw_tickle_publisher_t* pub_impl, const char* text,
                    uint64_t psn) {
    struct pool_ros_msg message = {.text = (char*)text};
    rmw_tickle_outgoing_message_t outgoing = {.publication_sequence_number = psn,
                                              .callbacks = &pool_callbacks,
                                              .ros_message = &message,
                                              .tickle = NULL};
    uint8_t wire[MAX_TEXT + RMW_TICKLE_PSN_BYTES];
    int32_t size = pub_impl->topic.data_encode((struct tt_Data*)&outgoing, wire, (uint32_t)sizeof(wire));
    assert(size > 0);
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, 0, (uint16_t)psn,
                                         sub_impl->topic.data_decode_inplace(wire, (uint32_t)size, true));
}

static char* take_one(rmw_subscription_t* sub) {
    struct pool_ros_msg incoming = {.text = NULL};
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_with_info(sub, &incoming, &taken, NULL, NULL));
    return taken ? incoming.text : NULL;
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_shell_pool", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = pool_type_support();
    rmw_qos_profile_t pub_qos = base_qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "pool_topic", &pub_qos, &pub_opts);
    assert(NULL != pub);
    rmw_qos_profile_t sub_qos = base_qos();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "pool_topic", &sub_qos, &sub_opts);
    assert(NULL != sub);
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;

    // A type with a direct codec keeps no message-sized buffer on either side.
    assert(NULL == pub_impl->publish_scratch_buf);
    assert(NULL == sub_impl->decode_scratch);
    assert(QUEUE_DEPTH == sub_impl->queue_capacity);
    assert(0 == sub_impl->shell_pool_count);

    // A long sample, then a short one into the shell the first one came back in. A decoder that
    // reused the allocation without rewriting the field would hand back the long text again.
    deliver(sub_impl, pub_impl, "a-long-first-sample", 1);
    char* first = take_one(sub);
    assert(NULL != first && 0 == strcmp("a-long-first-sample", first));
    free(first);
    assert(1 == sub_impl->shell_pool_count); // the taken shell went back

    deliver(sub_impl, pub_impl, "b", 2);
    assert(0 == sub_impl->shell_pool_count); // and was reused rather than a new one allocated
    char* second = take_one(sub);
    assert(NULL != second && 0 == strcmp("b", second));
    free(second);

    // The queue full: depth + 2 delivered without a take. KEEP_LAST drops the oldest back into the
    // pool rather than leaking it, so exactly depth are queued and the newest are the ones kept.
    for (int i = 0; i < QUEUE_DEPTH + 2; i++) {
        char text[MAX_TEXT];
        snprintf(text, sizeof(text), "burst-%d", i);
        deliver(sub_impl, pub_impl, text, BURST_FIRST_PSN + (uint64_t)i);
    }
    assert(QUEUE_DEPTH == sub_impl->queue_count);
    // The pool can never hold more than the queue's capacity, whatever the traffic.
    assert(sub_impl->shell_pool_count <= sub_impl->queue_capacity);
    for (int i = 2; i < QUEUE_DEPTH + 2; i++) {
        char expected[MAX_TEXT];
        snprintf(expected, sizeof(expected), "burst-%d", i);
        char* text = take_one(sub);
        assert(NULL != text);
        assert(0 == strcmp(expected, text)); // the oldest two were dropped, not the newest
        free(text);
    }
    assert(NULL == take_one(sub)); // and nothing is left stuck
    assert(QUEUE_DEPTH == sub_impl->shell_pool_count);
    assert(sub_impl->shell_pool_count <= sub_impl->queue_capacity);

    // A payload that does not decode returns its shell rather than losing it.
    size_t before = sub_impl->shell_pool_count;
    uint8_t rubbish[RMW_TICKLE_PSN_BYTES + 4] = {0};
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, 0, RUBBISH_SEQ_NO,
                                         sub_impl->topic.data_decode_inplace(rubbish, sizeof(rubbish), true));
    assert(before == sub_impl->shell_pool_count);
    assert(0 == sub_impl->queue_count);

    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_shell_pool: PASS\n");
    return 0;
}
