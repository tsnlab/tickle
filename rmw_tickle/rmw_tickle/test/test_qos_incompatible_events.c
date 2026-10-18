/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// RMW_EVENT_OFFERED_QOS_INCOMPATIBLE / RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE between two processes, from core's
// judgement when an announce is applied (QoS plan D2). Two processes because the event is about the other side's
// announce: a talker (forked child, its own rmw context) and a listener (this process).
//
//   topic A: listener's RELIABLE subscription, talker's BEST_EFFORT publisher - incompatible by RELIABILITY: one
//            REQUESTED event on the listener, one OFFERED on the talker;
//   topic B: listener's VOLATILE publisher, talker's TRANSIENT_LOCAL subscription - incompatible by DURABILITY;
//   topic C: the control - listener's BEST_EFFORT subscription, talker's RELIABLE publisher, compatible: matched
//            (so discovery saw it) and no event.
//
// What must hold, decided before the code (the plan's falsifier is an event firing twice for one incompatibility):
//   1. each incompatible pair raises exactly one event on each side, with its policy, and rmw_wait() wakes for it;
//   2. it stays one through several announce intervals (repeated announces) - counts read again after 3 intervals;
//   3. a second incompatible writer the talker creates later raises one more on the listener - and the talker's new
//      announce generation, which lists its first writer and its TRANSIENT_LOCAL reader again, raises none;
//   4. that writer, created when the talker already knows the listener, counts its pair at creation;
//   5. the control counts nothing although it matched.
// Before D2 these events came from a 1 s rescan of the discovery table on each endpoint, started at event_init().

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <tickle/config.h> // tt_CONTEXT_UPDATE_INTERVAL
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/event.h"
#include "rmw/events_statuses/incompatible_qos.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_policy_kind.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define TOPIC_A "/qos_incompatible_a"
#define TOPIC_B "/qos_incompatible_b"
#define TOPIC_C "/qos_incompatible_c"
#define POLL_MS 50L
#define NS_PER_MS 1000000L
#define MS_PER_S 1000L
#define WAIT_POLLS 200 // 10 s for a first event or a match
// Three announce intervals: every peer's periodic summary and any reply have come and gone several times.
#define SETTLE_POLLS ((int)(3 * (tt_CONTEXT_UPDATE_INTERVAL / (POLL_MS * NS_PER_MS))))

static bool convert(const void* source, void* dest) {
    memcpy(dest, source, sizeof(uint32_t));
    return true;
}
static int32_t encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static int32_t decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t message_callbacks = {
    .ros_type_name = "qos_incompatible_events/msg/Count",
    .tickle_struct_size = sizeof(uint32_t),
    .ros_struct_size = sizeof(uint32_t),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&encode,
    .tickle_decode = (tt_DATA_DECODE)&decode,
    .tickle_free = (tt_DATA_FREE)&free_nothing,
    .tickle_max_encoded_size = sizeof(uint32_t),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
};
static rosidl_message_type_support_t message_handle = {.data = &message_callbacks,
                                                       .func = get_message_typesupport_handle_function};

// What the talker reports when it stops: its own events' counts.
struct talker_counts {
    int32_t pub_a;  // OFFERED on its BEST_EFFORT writer of A
    int32_t sub_b;  // REQUESTED on its TRANSIENT_LOCAL reader of B
    int32_t pub_a2; // OFFERED on its second writer of A
    int32_t pub_c;  // OFFERED on the control's writer
    int32_t policy_pub_a;
    int32_t policy_sub_b;
    int32_t policy_pub_a2;
};

static void sleep_poll(void) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = POLL_MS * NS_PER_MS};
    nanosleep(&interval, NULL);
}

static void init_context(rmw_init_options_t* options, rmw_context_t* context) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    *options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(options, allocator));
    options->enclave = rcutils_strdup("/", allocator);
    *context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(options, context));
}

static void fini_context(rmw_init_options_t* options, rmw_context_t* context) {
    assert(RMW_RET_OK == rmw_shutdown(context));
    assert(RMW_RET_OK == rmw_context_fini(context));
    assert(RMW_RET_OK == rmw_init_options_fini(options));
}

static rmw_qos_profile_t qos_of(rmw_qos_reliability_policy_t reliability, rmw_qos_durability_policy_t durability) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.reliability = reliability;
    qos.durability = durability;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    return qos;
}

// An event's counts now; *policy its last_policy_kind. Offered and requested statuses share one layout.
static int32_t take_count(rmw_event_t* event, int32_t* change, int32_t* policy) {
    rmw_offered_qos_incompatible_event_status_t status;
    memset(&status, 0, sizeof(status));
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_event(event, &status, &taken));
    assert(taken);
    if (NULL != change) {
        *change = status.total_count_change;
    }
    if (NULL != policy) {
        *policy = (int32_t)status.last_policy_kind;
    }
    return status.total_count;
}

static rmw_publisher_t* make_pub(rmw_node_t* node, const char* topic, const rmw_qos_profile_t* qos,
                                 rmw_event_t* event) {
    rmw_publisher_options_t options = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, &message_handle, topic, qos, &options);
    assert(NULL != pub);
    *event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(event, pub, RMW_EVENT_OFFERED_QOS_INCOMPATIBLE));
    return pub;
}

static rmw_subscription_t* make_sub(rmw_node_t* node, const char* topic, const rmw_qos_profile_t* qos,
                                    rmw_event_t* event) {
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &message_handle, topic, qos, &options);
    assert(NULL != sub);
    *event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_subscription_event_init(event, sub, RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE));
    return sub;
}

// The talker: its endpoints, then on '2' a second incompatible writer, then on end of file its counts.
static int run_talker(int report_fd, int command_fd) {
    rmw_init_options_t options;
    rmw_context_t context;
    init_context(&options, &context);
    rmw_node_t* node = rmw_create_node(&context, "qos_incompatible_talker", "/");
    assert(NULL != node);
    rmw_qos_profile_t best_effort = qos_of(RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT, RMW_QOS_POLICY_DURABILITY_VOLATILE);
    rmw_qos_profile_t durable = qos_of(RMW_QOS_POLICY_RELIABILITY_RELIABLE, RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL);
    rmw_qos_profile_t reliable = qos_of(RMW_QOS_POLICY_RELIABILITY_RELIABLE, RMW_QOS_POLICY_DURABILITY_VOLATILE);
    rmw_event_t pub_a_event;
    rmw_event_t sub_b_event;
    rmw_event_t pub_c_event;
    rmw_event_t pub_a2_event;
    rmw_publisher_t* pub_a = make_pub(node, TOPIC_A, &best_effort, &pub_a_event);
    rmw_subscription_t* sub_b = make_sub(node, TOPIC_B, &durable, &sub_b_event);
    rmw_publisher_t* pub_c = make_pub(node, TOPIC_C, &reliable, &pub_c_event);
    rmw_publisher_t* pub_a2 = NULL;

    char command = 0;
    while (1 == read(command_fd, &command, 1)) {
        if ('2' == command && NULL == pub_a2) {
            pub_a2 = make_pub(node, TOPIC_A, &best_effort, &pub_a2_event);
        }
    }
    struct talker_counts counts;
    memset(&counts, 0, sizeof(counts));
    counts.pub_a = take_count(&pub_a_event, NULL, &counts.policy_pub_a);
    counts.sub_b = take_count(&sub_b_event, NULL, &counts.policy_sub_b);
    counts.pub_c = take_count(&pub_c_event, NULL, NULL);
    if (NULL != pub_a2) {
        counts.pub_a2 = take_count(&pub_a2_event, NULL, &counts.policy_pub_a2);
        assert(RMW_RET_OK == rmw_event_fini(&pub_a2_event));
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub_a2));
    }
    assert((ssize_t)sizeof(counts) == write(report_fd, &counts, sizeof(counts)));
    close(report_fd);

    assert(RMW_RET_OK == rmw_event_fini(&pub_a_event));
    assert(RMW_RET_OK == rmw_event_fini(&sub_b_event));
    assert(RMW_RET_OK == rmw_event_fini(&pub_c_event));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub_c));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub_b));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub_a));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    fini_context(&options, &context);
    return 0;
}

// rmw_wait() on one event: true when it woke for it.
static bool wait_for_event(rmw_context_t* context, rmw_event_t* event) {
    rmw_wait_set_t* wait_set = rmw_create_wait_set(context, 1);
    assert(NULL != wait_set);
    void* storage[1] = {event};
    rmw_events_t events = {.event_count = 1, .events = storage};
    rmw_time_t timeout = {.sec = (uint64_t)(WAIT_POLLS * POLL_MS / MS_PER_S), .nsec = 0};
    rmw_ret_t ret = rmw_wait(NULL, NULL, NULL, NULL, &events, wait_set, &timeout);
    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    return RMW_RET_OK == ret && NULL != storage[0];
}

static void settle(void) {
    for (int i = 0; i < SETTLE_POLLS; i++) {
        sleep_poll();
    }
}

int main(void) {
    message_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    setvbuf(stdout, NULL, _IONBF, 0); // an assert aborts: what was printed before it must not be lost

    int report_pipe[2];
    int command_pipe[2];
    assert(0 == pipe(report_pipe) && 0 == pipe(command_pipe));
    fflush(stdout);
    pid_t talker = fork();
    assert(talker >= 0);
    if (0 == talker) {
        close(report_pipe[0]);
        close(command_pipe[1]);
        _exit(run_talker(report_pipe[1], command_pipe[0]));
    }
    close(report_pipe[1]);
    close(command_pipe[0]);

    rmw_init_options_t options;
    rmw_context_t context;
    init_context(&options, &context);
    rmw_node_t* node = rmw_create_node(&context, "qos_incompatible_listener", "/");
    assert(NULL != node);
    rmw_qos_profile_t reliable = qos_of(RMW_QOS_POLICY_RELIABILITY_RELIABLE, RMW_QOS_POLICY_DURABILITY_VOLATILE);
    rmw_qos_profile_t best_effort = qos_of(RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT, RMW_QOS_POLICY_DURABILITY_VOLATILE);
    rmw_event_t sub_a_event;
    rmw_event_t pub_b_event;
    rmw_event_t sub_c_event;
    rmw_subscription_t* sub_a = make_sub(node, TOPIC_A, &reliable, &sub_a_event);
    rmw_publisher_t* pub_b = make_pub(node, TOPIC_B, &reliable, &pub_b_event);
    rmw_subscription_t* sub_c = make_sub(node, TOPIC_C, &best_effort, &sub_c_event);

    // 1. The first event wakes rmw_wait(); the control matches.
    assert(wait_for_event(&context, &sub_a_event));
    assert(wait_for_event(&context, &pub_b_event));
    size_t control_matched = 0;
    for (int i = 0; i < WAIT_POLLS && 0 == control_matched; i++) {
        assert(RMW_RET_OK == rmw_subscription_count_matched_publishers(sub_c, &control_matched));
        if (0 == control_matched) {
            sleep_poll();
        }
    }
    assert(1 == control_matched);

    // 2. Still one each after three announce intervals.
    settle();
    int32_t change = 0;
    int32_t policy = 0;
    int32_t sub_a_total = take_count(&sub_a_event, &change, &policy);
    printf("listener after %d polls: sub_a %d (change %d, policy %d)", SETTLE_POLLS, sub_a_total, change, policy);
    assert(1 == sub_a_total && 1 == change && RMW_QOS_POLICY_RELIABILITY == policy);
    int32_t pub_b_total = take_count(&pub_b_event, &change, &policy);
    printf(", pub_b %d (change %d, policy %d)", pub_b_total, change, policy);
    assert(1 == pub_b_total && 1 == change && RMW_QOS_POLICY_DURABILITY == policy);
    int32_t sub_c_total = take_count(&sub_c_event, &change, NULL);
    printf(", control %d (matched %zu)\n", sub_c_total, control_matched);
    assert(0 == sub_c_total && 0 == change);

    // 3. A second incompatible writer: one more on sub_a, none on pub_b from the talker's new generation.
    assert(1 == write(command_pipe[1], "2", 1));
    assert(wait_for_event(&context, &sub_a_event));
    settle();
    sub_a_total = take_count(&sub_a_event, &change, &policy);
    pub_b_total = take_count(&pub_b_event, NULL, NULL);
    sub_c_total = take_count(&sub_c_event, NULL, NULL);
    printf("listener after the second writer: sub_a %d (change %d), pub_b %d, control %d\n", sub_a_total, change,
           pub_b_total, sub_c_total);
    assert(2 == sub_a_total && 1 == change && RMW_QOS_POLICY_RELIABILITY == policy);
    assert(1 == pub_b_total);
    assert(0 == sub_c_total);

    // 4. and the talker's side, each once.
    close(command_pipe[1]);
    struct talker_counts counts;
    assert((ssize_t)sizeof(counts) == read(report_pipe[0], &counts, sizeof(counts)));
    close(report_pipe[0]);
    printf("talker: pub_a %d (policy %d), sub_b %d (policy %d), pub_a2 %d (policy %d), control %d\n", counts.pub_a,
           counts.policy_pub_a, counts.sub_b, counts.policy_sub_b, counts.pub_a2, counts.policy_pub_a2, counts.pub_c);
    assert(1 == counts.pub_a && RMW_QOS_POLICY_RELIABILITY == counts.policy_pub_a);
    assert(1 == counts.sub_b && RMW_QOS_POLICY_DURABILITY == counts.policy_sub_b);
    assert(1 == counts.pub_a2 && RMW_QOS_POLICY_RELIABILITY == counts.policy_pub_a2);
    assert(0 == counts.pub_c);
    int status = 0;
    assert(talker == waitpid(talker, &status, 0));
    assert(WIFEXITED(status) && 0 == WEXITSTATUS(status));

    assert(RMW_RET_OK == rmw_event_fini(&sub_a_event));
    assert(RMW_RET_OK == rmw_event_fini(&pub_b_event));
    assert(RMW_RET_OK == rmw_event_fini(&sub_c_event));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub_c));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub_b));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub_a));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    fini_context(&options, &context);
    printf("test_qos_incompatible_events: PASS\n");
    return 0;
}
