/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE_SIZE/tt_DATA_ENCODE/tt_DATA_DECODE/tt_DATA_FREE

#ifdef __cplusplus
extern "C" {
#endif

// rosidl's typesupport contract never inspects a rosidl_message_type_support_t's own `.data`
// pointer (see rosidl_runtime_c/message_type_support_struct.h's own doc comment) - it's opaque to
// everyone except whichever specific typesupport implementation's identifier matches, so this
// shape is private to rmw_tickle and the code tools/typesupport/tickle_typesupport/ros2_cli.py
// generates (ros2_adapter.render_type_support()) - the two only need to agree with EACH OTHER,
// not with rosidl/ament_cmake or any other typesupport.
//
// A generic `const void*`/`void*` pair rather than the real ROS 2 struct / TickLE struct types,
// the same type-erasure idiom examples/*/*.c's own <Name>Topic definitions already use for
// TickLE's own tt_DATA_ENCODE et al. (a real per-message struct pointer cast through a common
// function pointer signature) - to_tickle/from_tickle need their own pair of typedefs here since,
// unlike tt_DATA_ENCODE et al., TickLE's tt_Data itself has nothing to do with ROS 2's own struct
// shape on the other side of the conversion.
typedef bool (*rosidl_typesupport_tickle_c_to_tickle_function)(const void* ros_message, void* tickle_message);
typedef bool (*rosidl_typesupport_tickle_c_from_tickle_function)(const void* tickle_message, void* ros_message);

typedef struct rosidl_typesupport_tickle_c_message_callbacks_t {
    // "pkg/subfolder/Type" (e.g. "test_msgs/msg/Simple") - the same string tt_Topic.name needs
    // (rmw_tickle/PLAN.md's Milestone 3: tt_hash_id(topic->name, endpoint_name) mixes in both the
    // type and the ROS topic name, matching ROS 2's own "both type and topic name must match to
    // connect" rule), stable and identical across every process/build so two independently
    // generated rmw_tickle nodes agree on the same hash for the same message type.
    const char* ros_type_name;
    size_t tickle_struct_size; // sizeof() the generated TickLE-side struct - lets rmw_tickle
                                // allocate scratch storage generically, without needing a
                                // per-message struct definition of its own
    size_t ros_struct_size;    // sizeof() the rosidl_generator_c struct - same reason, for the ROS
                                // 2 side (a subscriber's receive queue holds already-from_tickle()-
                                // converted, independently-owned ROS messages - see rmw_tickle's
                                // own rmw_subscription.c - so it needs to allocate these without a
                                // per-message struct definition of its own either)
    rosidl_typesupport_tickle_c_to_tickle_function to_tickle;
    rosidl_typesupport_tickle_c_from_tickle_function from_tickle;
    tt_DATA_ENCODE_SIZE tickle_encode_size;
    tt_DATA_ENCODE tickle_encode;
    tt_DATA_DECODE tickle_decode;
    tt_DATA_FREE tickle_free;
} rosidl_typesupport_tickle_c_message_callbacks_t;

#ifdef __cplusplus
}
#endif
