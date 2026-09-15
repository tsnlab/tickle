/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A separate executable from test_dispatch.c, not an addition to it, because the two exercise
// genuinely different entry points into the dispatch chain: test_dispatch.c's ROSIDL_GET_MSG_
// TYPE_SUPPORT() macro goes straight to the C-level "rosidl_typesupport_c" dispatch, which was
// never affected by the gap rosidl_typesupport_tickle_cpp (see its own package.xml doc comment)
// closes - a real rclcpp::Node's create_publisher<T>()/create_subscription<T>() always start one
// level higher, from rosidl_typesupport_cpp::get_message_type_support_handle<T>(). That C++-level
// starting point is what this file reproduces, which is *why* rosidl_typesupport_tickle_c being
// unreachable from any real rclcpp application went undetected until a real benchmark (buildfarm_
// perf_tests) actually exercised it - test_dispatch.c alone couldn't have caught it.

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rosidl_runtime_c/message_type_support_struct.h>
#include <rosidl_typesupport_cpp/message_type_support.hpp>
#include <rosidl_typesupport_tickle_c/message_type_support.h>
#include <rosidl_typesupport_tickle_cpp/identifier.h>

#include "rosidl_typesupport_tickle_c_tests/msg/simple.h"   // the C struct to_tickle/from_tickle actually use
#include "rosidl_typesupport_tickle_c_tests/msg/simple.hpp" // the C++ type a real rclcpp app passes

auto main() -> int {
    // The exact handle a real rclcpp::create_publisher<Simple>()/create_subscription<Simple>()
    // call resolves internally, before ever reaching rmw_create_publisher()/rmw_create_
    // subscription() - see rosidl_typesupport_cpp/message_type_support.hpp's own declaration.
    const rosidl_message_type_support_t* top =
        rosidl_typesupport_cpp::get_message_type_support_handle<rosidl_typesupport_tickle_c_tests::msg::Simple>();
    assert(top != nullptr);
    assert(strcmp(top->typesupport_identifier, "rosidl_typesupport_cpp") == 0);

    // Only reachable because rosidl_typesupport_tickle_cpp registered itself as a "rosidl_
    // typesupport_cpp" ament_index resource (see its own CMakeLists.txt) - without that, this
    // call returns nullptr regardless of whether rosidl_typesupport_tickle_c's own C-level
    // dispatch (test_dispatch.c) is perfectly correct, which is exactly the bug this test guards
    // against.
    const rosidl_message_type_support_t* ours =
        get_message_typesupport_handle(top, rosidl_typesupport_tickle_cpp__identifier);
    assert(ours != nullptr);
    assert(ours->typesupport_identifier == rosidl_typesupport_tickle_cpp__identifier);

    // .data here is rosidl_typesupport_tickle_c's own real callbacks struct (borrowed directly,
    // not a second copy) - see rosidl_typesupport_tickle_cpp/resource/msg__type_support.cpp.in's
    // own doc comment for why it's reached this way instead of one more dispatch hop.
    const auto* callbacks = static_cast<const rosidl_typesupport_tickle_c_message_callbacks_t*>(ours->data);
    assert(callbacks != nullptr);
    assert(callbacks->ros_struct_size == sizeof(struct rosidl_typesupport_tickle_c_tests__msg__Simple));
    assert(strcmp(callbacks->ros_type_name, "rosidl_typesupport_tickle_c_tests/msg/Simple") == 0);

    struct rosidl_typesupport_tickle_c_tests__msg__Simple ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    ros_in.value = 10;

    std::array<uint64_t, 8> tickle_storage {}; // see test_dispatch.c's own sizing comment
    assert(callbacks->to_tickle(&ros_in, tickle_storage.data()));

    struct rosidl_typesupport_tickle_c_tests__msg__Simple ros_out;
    memset(&ros_out, 0, sizeof(ros_out));
    assert(callbacks->from_tickle(tickle_storage.data(), &ros_out));
    assert(ros_out.value == 10);

    printf("rosidl_typesupport_tickle_cpp dispatch (C++ entry point): PASS\n");
    return 0;
}
