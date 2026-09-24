/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// interfaces_check pub|sub SECONDS: one side of a two-process round trip of std_msgs/String and
// std_msgs/Header through whatever rmw RMW_IMPLEMENTATION names (check_ros2_interfaces.sh sets
// rmw_tickle). The publisher sends both types every 100 ms; the subscriber prints what it received
// and exits 0 once it has one correct sample of each, 1 if SECONDS pass first. Header is the type
// that proves nesting across packages - it holds a builtin_interfaces/Time - and String the one with
// a plain unbounded string.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/string_functions.h"
#include "std_msgs/msg/header.h"
#include "std_msgs/msg/string.h"

#define TEXT "hello from rmw_tickle"
#define FRAME "tickle_frame"
#define STAMP_SEC 1234
#define STAMP_NSEC 567890U
#define PERIOD_MS 100L
#define MS_PER_S 1000L
#define NS_PER_MS 1000000L
#define STRING_TOPIC "/tickle_check_string"
#define HEADER_TOPIC "/tickle_check_header"

static void nap(void) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = PERIOD_MS * NS_PER_MS};
    nanosleep(&interval, NULL);
}

static int fail(const char* what) {
    fprintf(stderr, "interfaces_check: %s: %s\n", what, rcutils_get_error_string().str);
    return 2;
}

static rmw_node_t* create_node(rmw_context_t* context, const char* name) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    if (RMW_RET_OK != rmw_init_options_init(&options, allocator)) {
        return NULL;
    }
    options.enclave = rcutils_strdup("/", allocator);
    if (RMW_RET_OK != rmw_init(&options, context)) {
        return NULL;
    }
    return rmw_create_node(context, name, "/");
}

static int publish(rmw_node_t* node, const rmw_qos_profile_t* qos, long rounds) {
    rmw_publisher_options_t opts = rmw_get_default_publisher_options();
    rmw_publisher_t* string_pub =
        rmw_create_publisher(node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String), STRING_TOPIC, qos, &opts);
    rmw_publisher_t* header_pub =
        rmw_create_publisher(node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Header), HEADER_TOPIC, qos, &opts);
    if (NULL == string_pub || NULL == header_pub) {
        return fail("rmw_create_publisher");
    }
    std_msgs__msg__String string_msg;
    std_msgs__msg__Header header_msg;
    std_msgs__msg__String__init(&string_msg);
    std_msgs__msg__Header__init(&header_msg);
    rosidl_runtime_c__String__assign(&string_msg.data, TEXT);
    rosidl_runtime_c__String__assign(&header_msg.frame_id, FRAME);
    header_msg.stamp.sec = STAMP_SEC;
    header_msg.stamp.nanosec = STAMP_NSEC;
    int status = 0;
    for (long i = 0; i < rounds && 0 == status; i++) {
        if (RMW_RET_OK != rmw_publish(string_pub, &string_msg, NULL) ||
            RMW_RET_OK != rmw_publish(header_pub, &header_msg, NULL)) {
            status = fail("rmw_publish");
        }
        nap();
    }
    std_msgs__msg__String__fini(&string_msg);
    std_msgs__msg__Header__fini(&header_msg);
    if (0 == status) {
        printf("interfaces_check pub: published %ld of each\n", rounds);
    }
    return status;
}

// Each take_* reports whether a sample arrived AND was the one sent; a wrong one is printed as such.
static bool take_string(rmw_subscription_t* sub, std_msgs__msg__String* msg) {
    bool taken = false;
    if (RMW_RET_OK != rmw_take(sub, msg, &taken, NULL) || !taken) {
        return false;
    }
    const char* data = msg->data.data;
    bool correct = NULL != data && 0 == strcmp(data, TEXT);
    printf("String: \"%s\"%s\n", data ? data : "(null)", correct ? "" : "  <- WRONG");
    return correct;
}

static bool take_header(rmw_subscription_t* sub, std_msgs__msg__Header* msg) {
    bool taken = false;
    if (RMW_RET_OK != rmw_take(sub, msg, &taken, NULL) || !taken) {
        return false;
    }
    const char* frame = msg->frame_id.data;
    bool correct =
        msg->stamp.sec == STAMP_SEC && msg->stamp.nanosec == STAMP_NSEC && NULL != frame && 0 == strcmp(frame, FRAME);
    printf("Header: stamp %d.%09u frame \"%s\"%s\n", msg->stamp.sec, msg->stamp.nanosec, frame ? frame : "(null)",
           correct ? "" : "  <- WRONG");
    return correct;
}

static int subscribe(rmw_node_t* node, const rmw_qos_profile_t* qos, long rounds) {
    rmw_subscription_options_t opts = rmw_get_default_subscription_options();
    rmw_subscription_t* string_sub =
        rmw_create_subscription(node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String), STRING_TOPIC, qos, &opts);
    rmw_subscription_t* header_sub =
        rmw_create_subscription(node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Header), HEADER_TOPIC, qos, &opts);
    if (NULL == string_sub || NULL == header_sub) {
        return fail("rmw_create_subscription");
    }
    std_msgs__msg__String string_msg;
    std_msgs__msg__Header header_msg;
    std_msgs__msg__String__init(&string_msg);
    std_msgs__msg__Header__init(&header_msg);
    bool got_string = false;
    bool got_header = false;
    for (long i = 0; i < rounds && !(got_string && got_header); i++) {
        got_string = got_string || take_string(string_sub, &string_msg);
        got_header = got_header || take_header(header_sub, &header_msg);
        nap();
    }
    std_msgs__msg__String__fini(&string_msg);
    std_msgs__msg__Header__fini(&header_msg);
    int status = got_string && got_header ? 0 : 1;
    printf("interfaces_check sub: %s\n", 0 == status ? "PASS - both types round-tripped" : "FAIL");
    return status;
}

int main(int argc, char** argv) {
    if (argc != 3 || (strcmp(argv[1], "pub") != 0 && strcmp(argv[1], "sub") != 0)) {
        fprintf(stderr, "usage: interfaces_check pub|sub SECONDS\n");
        return 2;
    }
    bool publisher = 0 == strcmp(argv[1], "pub");
    long rounds = atol(argv[2]) * MS_PER_S / PERIOD_MS;

    rmw_context_t context = rmw_get_zero_initialized_context();
    rmw_node_t* node = create_node(&context, publisher ? "interfaces_pub" : "interfaces_sub");
    if (NULL == node) {
        return fail("creating the node");
    }
    rmw_qos_profile_t qos = rmw_qos_profile_default; // RELIABLE, KEEP_LAST 10 - ROS 2's default
    int status = publisher ? publish(node, &qos, rounds) : subscribe(node, &qos, rounds);
    fflush(stdout);
    // Leave through the OS rather than tearing the context down: this is a one-shot check, and the
    // exit status is its whole result.
    return status;
}
