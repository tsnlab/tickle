/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g13 (RMW_GAPS_PLAN.md): a KEEP_ALL subscription. Its capacity comes from a byte budget rather
// than from `depth`, and a full queue declines the arriving sample instead of destroying an unread
// one - which is what separates KEEP_ALL from a deep KEEP_LAST, and the whole reason `ros2 bag
// record` needs it.
//
// Division of labour with the core-side tests, so neither is asked to prove the other's half:
// tests/test_reliable_pubsub.c pins that core consults the accept hook BEFORE it records anything
// about a sample, and that a declined sample is therefore still retransmitted and delivered. What
// is pinned here is rmw's answer - that the hook is set only for KEEP_ALL, that it says no exactly
// when the queue is full, that a full queue never evicts, and that the samples an application gets
// back are the FIRST ones published rather than the last. deliver() below asks the hook itself, in
// the same order core does, because driving a real writer into a unit test would replace what is
// being tested with a network.

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

#define WANTED_CAPACITY 4
#define OVERFLOW_BY 3
#define MAX_TEXT 64

// The same one-heap-field shape test_shell_pool.c uses, for the same reason: a stale tail would be
// visible if anything reused a shell without rewriting it.
struct keep_all_ros_msg {
    char* text;
};

static int32_t keep_all_encode_size(const void* ros_message) {
    const struct keep_all_ros_msg* ros = ros_message;
    const char* text = ros->text != NULL ? ros->text : "";
    return (int32_t)(2 + strlen(text) + 1);
}

static int32_t keep_all_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    const struct keep_all_ros_msg* ros = ros_message;
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

static int32_t keep_all_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    struct keep_all_ros_msg* ros = ros_message;
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

static rosidl_typesupport_tickle_c_message_callbacks_t keep_all_callbacks = {
    .struct_size = sizeof(rosidl_typesupport_tickle_c_message_callbacks_t),
    .ros_type_name = "test_keep_all_reader/msg/KeepAllMsg",
    .tickle_struct_size = MAX_TEXT,
    .ros_struct_size = sizeof(struct keep_all_ros_msg),
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .direct_encode_size = &keep_all_encode_size,
    .direct_encode = &keep_all_encode,
    .direct_decode = &keep_all_decode,
};

static rosidl_message_type_support_t keep_all_handle = {
    .data = &keep_all_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* keep_all_type_support(void) {
    keep_all_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    return &keep_all_handle;
}

static rmw_qos_profile_t base_qos(rmw_qos_history_policy_t history) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.history = history;
    qos.depth = 2; // deliberately wrong for KEEP_ALL: the budget must be what sizes it, not this
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    qos.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    qos.avoid_ros_namespace_conventions = false;
    return qos;
}

// One sample, in the order core delivers one: ask the accept hook, and hand it to the callback only
// if the hook said yes. Returns false when it was declined.
static bool deliver(rmw_tickle_subscriber_t* sub_impl, rmw_tickle_publisher_t* pub_impl, const char* text,
                    uint64_t psn) {
    if (NULL != sub_impl->tickle_subscriber.accept_callback &&
        !sub_impl->tickle_subscriber.accept_callback(&sub_impl->tickle_subscriber, (uint32_t)psn,
                                                     sub_impl->tickle_subscriber.accept_callback_param)) {
        return false;
    }
    struct keep_all_ros_msg message = {.text = (char*)text};
    rmw_tickle_outgoing_message_t outgoing = {.publication_sequence_number = psn,
                                              .callbacks = &keep_all_callbacks,
                                              .ros_message = &message,
                                              .tickle = NULL};
    uint8_t wire[MAX_TEXT + RMW_TICKLE_PSN_BYTES];
    int32_t size = pub_impl->topic.data_encode((struct tt_Data*)&outgoing, wire, (uint32_t)sizeof(wire));
    assert(size > 0);
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, 0, (uint16_t)psn,
                                         sub_impl->topic.data_decode_inplace(wire, (uint32_t)size, true));
    return true;
}

// What core's reorder buffer does: hands up a sample the hook already admitted when it arrived out of order, without
// asking again, once the gap before it fills.
static void release(rmw_tickle_subscriber_t* sub_impl, rmw_tickle_publisher_t* pub_impl, const char* text,
                    uint64_t psn) {
    struct keep_all_ros_msg message = {.text = (char*)text};
    rmw_tickle_outgoing_message_t outgoing = {.publication_sequence_number = psn,
                                              .callbacks = &keep_all_callbacks,
                                              .ros_message = &message,
                                              .tickle = NULL};
    uint8_t wire[MAX_TEXT + RMW_TICKLE_PSN_BYTES];
    int32_t size = pub_impl->topic.data_encode((struct tt_Data*)&outgoing, wire, (uint32_t)sizeof(wire));
    assert(size > 0);
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, 0, (uint16_t)psn,
                                         sub_impl->topic.data_decode_inplace(wire, (uint32_t)size, true));
}

static char* take_one(rmw_subscription_t* sub) {
    struct keep_all_ros_msg incoming = {.text = NULL};
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_with_info(sub, &incoming, &taken, NULL, NULL));
    return taken ? incoming.text : NULL;
}

int main(void) {
    // The budget is set from the capacity wanted rather than the other way round, so the test does
    // not depend on the size of a struct it does not own.
    char budget[32];
    snprintf(budget, sizeof(budget), "%zu",
             WANTED_CAPACITY * (sizeof(rmw_tickle_queued_message_t) + sizeof(struct keep_all_ros_msg)));
    assert(0 == setenv("RMW_TICKLE_READER_KEEP_ALL_BYTES", budget, 1));

    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_keep_all_reader", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = keep_all_type_support();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();

    rmw_qos_profile_t pub_qos = base_qos(RMW_QOS_POLICY_HISTORY_KEEP_LAST);
    rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_all_topic", &pub_qos, &pub_opts);
    assert(NULL != pub);

    // Criterion 1, the local half: a KEEP_ALL subscription is created without error, against a
    // KEEP_LAST publisher. HISTORY is a local policy, not a requested/offered one, so the two must
    // match - the CycloneDDS control arm for that claim lives in the acceptance script, not here.
    rmw_qos_profile_t sub_qos = base_qos(RMW_QOS_POLICY_HISTORY_KEEP_ALL);
    rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "keep_all_topic", &sub_qos, &sub_opts);
    assert(NULL != sub);

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;

    assert(sub_impl->keep_all);
    assert(WANTED_CAPACITY == sub_impl->queue_limit); // the budget sized it, not qos.depth (2)
    assert(WANTED_CAPACITY + RMW_TICKLE_KEEP_ALL_HEADROOM == sub_impl->queue_capacity);
    assert(NULL != sub_impl->tickle_subscriber.accept_callback);
    assert(sub_impl == sub_impl->tickle_subscriber.accept_callback_param);

    // Criterion 2, no overwrite. Publish capacity + K with nothing taken; the overflow must be
    // declined rather than making room. Mutant: restore the evict-the-oldest branch for KEEP_ALL -
    // the declines drop to zero and the take loop below gets the LAST four instead of the first.
    for (int i = 0; i < WANTED_CAPACITY; i++) {
        char text[MAX_TEXT];
        snprintf(text, sizeof(text), "sample-%d", i);
        assert(deliver(sub_impl, pub_impl, text, (uint64_t)(i + 1)));
    }
    assert(WANTED_CAPACITY == (int)sub_impl->queue_count);

    // The overflow is counted rather than asserted here, deliberately. Asserting the decline first
    // makes the decline the thing this test can fail on, and criterion 2's actual claim - that what
    // comes back is the FIRST capacity samples - would then never be evaluated under the mutant it
    // exists for. The order check goes first; the decline count is asserted after it.
    int declined = 0;
    for (int i = 0; i < OVERFLOW_BY; i++) {
        char text[MAX_TEXT];
        snprintf(text, sizeof(text), "overflow-%d", i);
        if (!deliver(sub_impl, pub_impl, text, (uint64_t)(WANTED_CAPACITY + i + 1))) {
            declined++;
        }
    }

    // Criterion 2 itself: the samples the application gets back are the first ones published, in
    // order. A reader that overwrote hands back "overflow-*" here, which is the failure this whole
    // gap is about, and it is what the restore-the-evict-branch mutant produces.
    for (int i = 0; i < WANTED_CAPACITY; i++) {
        char expected[MAX_TEXT];
        snprintf(expected, sizeof(expected), "sample-%d", i);
        char* text = take_one(sub);
        assert(NULL != text);
        assert(0 == strcmp(expected, text));
        free(text);
    }

    assert(OVERFLOW_BY == declined); // every overflowing sample was refused, none quietly absorbed
    // The enqueue's own full-queue branch must never have been reached: the hook declined first, so
    // nothing arrived to drop there. Non-zero means the hook was bypassed and a sample the writer
    // believes was delivered has been lost.
    assert(0 == sub_impl->keep_all_unconsulted_drops);
    assert(NULL == take_one(sub));
    assert(0 == (int)sub_impl->queue_count);

    // And once there is room again the hook says yes, so the stall is a stall and not a wedge.
    assert(deliver(sub_impl, pub_impl, "after-drain", 99));
    char* resumed = take_one(sub);
    assert(NULL != resumed && 0 == strcmp("after-drain", resumed));
    free(resumed);

    // Criterion 3 (2026-10-05): samples admitted while there was room, held in core's reorder buffer and released
    // together when the gap before them fills, all fit - even onto a queue the application has let fill to its limit.
    // On the rig ~8,000 samples per 20 s run at 5% loss were dropped here as "unreachable". Mutant: no headroom
    // (queue_capacity = queue_limit) - the released samples are dropped and keep_all_unconsulted_drops counts them.
    enum { RELEASED = 16, PSN_HELD = 200, PSN_RELEASED = 300, PSN_REFILL = 400, PSN_LATE = 500, PSN_NEW = 501 };
    for (int i = 0; i < WANTED_CAPACITY; i++) {
        char text[MAX_TEXT];
        snprintf(text, sizeof(text), "held-before-%d", i);
        assert(deliver(sub_impl, pub_impl, text, (uint64_t)PSN_HELD + (uint64_t)i));
    }
    for (int i = 0; i < RELEASED; i++) {
        char text[MAX_TEXT];
        snprintf(text, sizeof(text), "released-%d", i);
        release(sub_impl, pub_impl, text, (uint64_t)PSN_RELEASED + (uint64_t)i);
    }
    assert(0 == sub_impl->keep_all_unconsulted_drops);
    assert(WANTED_CAPACITY + RELEASED == (int)sub_impl->queue_count);
    for (int i = 0; i < WANTED_CAPACITY + RELEASED; i++) {
        char expected[MAX_TEXT];
        if (i < WANTED_CAPACITY) {
            snprintf(expected, sizeof(expected), "held-before-%d", i);
        } else {
            snprintf(expected, sizeof(expected), "released-%d", i - WANTED_CAPACITY);
        }
        char* text = take_one(sub);
        assert(NULL != text && 0 == strcmp(expected, text));
        free(text);
    }
    // ...and the admission limit still holds while the headroom is in use: a NEW arrival is declined.
    for (int i = 0; i < WANTED_CAPACITY; i++) {
        assert(deliver(sub_impl, pub_impl, "refill", (uint64_t)PSN_REFILL + (uint64_t)i));
    }
    release(sub_impl, pub_impl, "released-late", PSN_LATE);
    assert(!deliver(sub_impl, pub_impl, "new-arrival", PSN_NEW));
    while (NULL != (resumed = take_one(sub))) {
        free(resumed);
    }

    // The control: an otherwise identical KEEP_LAST subscription sets no hook at all, so nothing on
    // its path changed. Its depth, not the budget, sizes it.
    rmw_qos_profile_t last_qos = base_qos(RMW_QOS_POLICY_HISTORY_KEEP_LAST);
    rmw_subscription_t* last = rmw_create_subscription(node, type_support, "keep_last_topic", &last_qos, &sub_opts);
    assert(NULL != last);
    rmw_tickle_subscriber_t* last_impl = (rmw_tickle_subscriber_t*)last->data;
    assert(!last_impl->keep_all);
    assert(NULL == last_impl->tickle_subscriber.accept_callback);
    assert(2 == (int)last_impl->queue_capacity);
    assert(2 == (int)last_impl->queue_limit); // no headroom: KEEP_LAST evicts, it never has to hold a burst

    assert(RMW_RET_OK == rmw_destroy_subscription(node, last));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_keep_all_reader: PASS\n");
    return 0;
}
