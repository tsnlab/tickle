/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Milestone 45 (rmw_tickle/PLAN.md) - rmw_publish()'s own publish_scratch_buf and rmw_
// subscription.c's own shell_pool (subscriber_callback()/rmw_take_with_info()) both replace a
// fresh allocate()/deallocate() pair per message with a reused buffer, closing part of comparison.
// md's own measured ~1.3-1.6x latency gap against FastDDS/CycloneDDS. The one real correctness
// hazard that reuse introduces (shell_pool's own doc comment, rmw_tickle_c/rmw_tickle.h): after
// rmw_take_with_info()'s own shallow memcpy() transfers a heap-owned field's pointer *value* out
// to the caller, the drained shell's own copy of that same pointer is still sitting there
// unchanged - reusing that shell for a *later* message without zeroing it first would make
// subscriber_callback()'s next from_tickle() call free/reallocate memory the earlier caller still
// owns (a real use-after-free/double-free, not hypothetical) - shell_pool_push()'s own memset()
// is what prevents it. A fake_ros_msg with a heap-owned `char* text` field (the same shape hazard
// a real rosidl_runtime_c__String has, without pulling in that library just for this one test) run
// through several cycles on the *same* Subscription is what actually exercises reuse - test_
// events.c's own fake_type_support() never does more than one publish, and test_rmw_api_surface.
// c's own fake message is a bare uint8_t with nothing to double-free in the first place, so
// neither would have caught a missing memset() here.
//
// Deliberately does NOT rely on a real rmw_publish() -> wire -> rmw_take() round trip to exercise
// the *receive* side: a co-located Publisher/Subscriber on the very same tt_Node never actually
// see each other's DATA over the wire at all (process_submessage()'s own self_sent filter drops
// it unconditionally, tickle.c) - a real, pre-existing, already-documented gap (rmw_tickle/PLAN.md
// Milestone 10's own "a full publish/subscribe round trip... no live multi-process test infra
// exists yet for rmw_tickle" note, still open), not something this milestone touches or needs to
// solve. Instead this invokes tt_Subscriber.callback directly (a genuinely public tickle.h field -
// literally the same function pointer tt_Node_create_subscriber() was given, and precisely what a
// real wire delivery would have called had one arrived) - the same "reach through an already-
// public field/header, not a private implementation detail" precedent test_multi_node.c already
// set. rmw_publish() itself is still called for real each cycle too, exercising publish_scratch_
// buf's own reuse on the send side independently (its own success/failure is observable through
// its return value alone, whether or not anything is listening on the wire).

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE, struct tt_Subscriber.callback

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define FAKE_TICKLE_TEXT_CAPACITY 32

struct fake_ros_msg {
    char* text; // heap-owned (strdup()/free()), NULL when empty - the hazard shell_pool guards
};
struct fake_tickle_msg {
    char text[FAKE_TICKLE_TEXT_CAPACITY];
};

static bool fake_to_tickle(const void* ros_message, void* tickle_message) {
    const struct fake_ros_msg* ros = ros_message;
    struct fake_tickle_msg* tickle = tickle_message;
    const char* text = ros->text != NULL ? ros->text : "";
    size_t len = strlen(text);
    if (len >= FAKE_TICKLE_TEXT_CAPACITY) {
        return false;
    }
    memcpy(tickle->text, text, len + 1);
    return true;
}
// Mirrors rosidl_runtime_c__String__assign()'s own real contract exactly: free whatever this
// field already owns before allocating a fresh copy - correct only if ros_message is either fresh
// (zero_allocate()'s own default, `text == NULL`, free(NULL) is a no-op) or a shell_pool_push()-
// zeroed reuse (also NULL) - the exact invariant this whole test exists to confirm holds in
// practice, not just on paper.
static bool fake_from_tickle(const void* tickle_message, void* ros_message) {
    const struct fake_tickle_msg* tickle = tickle_message;
    struct fake_ros_msg* ros = ros_message;
    free(ros->text);
    ros->text = strdup(tickle->text);
    return ros->text != NULL;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)FAKE_TICKLE_TEXT_CAPACITY;
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    memcpy(payload, ((struct fake_tickle_msg*)data)->text, FAKE_TICKLE_TEXT_CAPACITY);
    return (int32_t)FAKE_TICKLE_TEXT_CAPACITY;
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < FAKE_TICKLE_TEXT_CAPACITY) {
        return -1;
    }
    memcpy(((struct fake_tickle_msg*)data)->text, payload, FAKE_TICKLE_TEXT_CAPACITY);
    return (int32_t)FAKE_TICKLE_TEXT_CAPACITY;
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "test_publish_take_reuse/msg/FakeMsg",
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
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_publish_take_reuse", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = fake_type_support();

    rmw_qos_profile_t pub_qos = base_qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "reuse_topic", &pub_qos, &pub_opts);
    assert(NULL != pub);

    rmw_qos_profile_t sub_qos = base_qos();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "reuse_topic", &sub_qos, &sub_opts);
    assert(NULL != sub);
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;

    char* previous = NULL;
    for (int i = 0; i < 5; i++) {
        char expected[FAKE_TICKLE_TEXT_CAPACITY];
        snprintf(expected, sizeof(expected), "cycle-%d", i);

        // Exercises publish_scratch_buf's own reuse on the send side for real, independently of
        // whether anything local is listening on the wire - see this file's own module comment.
        struct fake_ros_msg outgoing = {.text = expected};
        assert(RMW_RET_OK == rmw_publish(pub, &outgoing, NULL));

        // Simulates the delivery a real remote peer's DATA would have triggered - direct call
        // through the exact same tt_SUBSCRIBER_CALLBACK function pointer tt_Node_create_
        // subscriber() was given (tt_Subscriber.callback, a plain public tickle.h field), not a
        // private implementation detail - see this file's own module comment for why a real wire
        // round trip can't reach this Subscription from this same process's own Publisher.
        struct fake_tickle_msg incoming_wire;
        memcpy(incoming_wire.text, expected, strlen(expected) + 1);
        sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, /*time=*/0, (uint16_t)i,
                                             (struct tt_Data*)&incoming_wire);

        struct fake_ros_msg incoming = {.text = NULL};
        bool taken = false;
        assert(RMW_RET_OK == rmw_take_with_info(sub, &incoming, &taken, NULL, NULL));
        assert(taken);
        assert(NULL != incoming.text);
        assert(strcmp(expected, incoming.text) == 0);

        if (i == 0) {
            // The very first take must have returned its own shell straight to the pool -
            // shell_pool's own doc comment (rmw_tickle.h): this is the actual mechanism under
            // test, not just "the string content happened to still be right."
            assert(1 == sub_impl->shell_pool_count);
        }

        free(previous);
        previous = incoming.text;
    }
    free(previous);

    // Every cycle above queued exactly one message and drained exactly that one - the queue must
    // be genuinely empty now, not left with something stuck.
    bool taken = false;
    struct fake_ros_msg drained = {.text = NULL};
    assert(RMW_RET_OK == rmw_take_with_info(sub, &drained, &taken, NULL, NULL));
    assert(!taken);

    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_publish_take_reuse: PASS\n");
    return 0;
}
