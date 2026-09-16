/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The .srv counterpart to test_dispatch_cpp.cpp, one dispatch level up - see that file's own doc
// comment for why a C++-entry-point test is needed at all (a real rclcpp::Client<T>/Service<T>
// always starts from rosidl_typesupport_cpp::get_service_type_support_handle<T>(), never from
// test_dispatch_srv.c's own ROSIDL_GET_SRV_TYPE_SUPPORT() C-level macro) - this closes the
// service-side half of that same gap, left open by Milestone 11's own note that "the cross-vendor
// test needs the service-side counterpart of this same C++-reachability fix" (rmw_tickle/PLAN.md).

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rosidl_runtime_c/message_type_support_struct.h>
#include <rosidl_runtime_c/service_type_support_struct.h>
#include <rosidl_typesupport_cpp/service_type_support.hpp>
#include <rosidl_typesupport_tickle_c/message_type_support.h>
#include <rosidl_typesupport_tickle_c/service_type_support.h>
#include <rosidl_typesupport_tickle_cpp/identifier.h>

#include "rosidl_typesupport_tickle_c_tests/srv/simple_service.h"   // the C structs to_tickle/from_tickle actually use
#include "rosidl_typesupport_tickle_c_tests/srv/simple_service.hpp" // the C++ type a real rclcpp app passes

int main() {
    // The exact handle a real rclcpp::Client<SimpleService>()/Service<SimpleService>() call
    // resolves internally, before ever reaching rmw_create_client()/rmw_create_service() - see
    // rosidl_typesupport_cpp/service_type_support.hpp's own declaration.
    const rosidl_service_type_support_t* top =
        rosidl_typesupport_cpp::get_service_type_support_handle<rosidl_typesupport_tickle_c_tests::srv::SimpleService>();
    assert(top != nullptr);
    assert(strcmp(top->typesupport_identifier, "rosidl_typesupport_cpp") == 0);

    // Only reachable because rosidl_typesupport_tickle_cpp registered itself as a "rosidl_
    // typesupport_cpp" ament_index resource (see its own CMakeLists.txt) - same gap test_dispatch_
    // cpp.cpp already guards at the message level, one dispatch level down. top's own map-based
    // dispatch (built from every "rosidl_typesupport_cpp" candidate: tickle_cpp, fastrtps_cpp,
    // introspection_cpp) only knows those CANDIDATE identifiers, not the lower-level "_c" one this
    // handle ultimately delegates to internally - so this asks for "_cpp", exactly matching
    // test_dispatch_cpp.cpp's own analogous message-level lookup one line below its own `top`.
    const rosidl_service_type_support_t* ours =
        get_service_typesupport_handle(top, rosidl_typesupport_tickle_cpp__identifier);
    assert(ours != nullptr);
    assert(ours->typesupport_identifier == rosidl_typesupport_tickle_cpp__identifier);
    assert(ours->request_typesupport != nullptr);
    assert(ours->response_typesupport != nullptr);

    // .data here is rosidl_typesupport_tickle_c's own real service callbacks struct (borrowed
    // directly, not a second copy) - see resource/srv__type_support.cpp.in's own doc comment.
    const auto* service_callbacks =
        static_cast<const rosidl_typesupport_tickle_c_service_callbacks_t*>(ours->data);
    assert(service_callbacks != nullptr);
    assert(strcmp(service_callbacks->ros_type_name, "rosidl_typesupport_tickle_c_tests/srv/SimpleService") == 0);

    // request_typesupport/response_typesupport are themselves CPP-level leaf handles (resource/
    // srv__type_support.cpp.in points them at the sibling msg__type_support.cpp.in shims generated
    // for the same .srv) - each one's own .data is *already* the C-level callbacks struct, borrowed
    // directly at its own first access (same as `ours->data` above, one level up) - no second
    // get_message_typesupport_handle() hop needed, and asking one for "_c" specifically would fail
    // anyway: a leaf handle's own .func only self-matches its OWN identifier ("_cpp" here), it
    // doesn't recurse - see resource/msg__type_support.cpp.in's own "self-match only" comment.
    assert(ours->request_typesupport->typesupport_identifier == rosidl_typesupport_tickle_cpp__identifier);
    const auto* request_callbacks =
        static_cast<const rosidl_typesupport_tickle_c_message_callbacks_t*>(ours->request_typesupport->data);
    assert(request_callbacks != nullptr);

    assert(ours->response_typesupport->typesupport_identifier == rosidl_typesupport_tickle_cpp__identifier);
    const auto* response_callbacks =
        static_cast<const rosidl_typesupport_tickle_c_message_callbacks_t*>(ours->response_typesupport->data);
    assert(response_callbacks != nullptr);

    struct rosidl_typesupport_tickle_c_tests__srv__SimpleService_Request request_in;
    memset(&request_in, 0, sizeof(request_in));
    request_in.request_value = true;

    std::array<uint64_t, 8> request_storage {};
    assert(request_callbacks->to_tickle(&request_in, request_storage.data()));

    struct rosidl_typesupport_tickle_c_tests__srv__SimpleService_Request request_out;
    memset(&request_out, 0, sizeof(request_out));
    assert(request_callbacks->from_tickle(request_storage.data(), &request_out));
    assert(request_out.request_value == request_in.request_value);

    printf("rosidl_typesupport_tickle_cpp service dispatch (C++ entry point): PASS\n");
    return 0;
}
