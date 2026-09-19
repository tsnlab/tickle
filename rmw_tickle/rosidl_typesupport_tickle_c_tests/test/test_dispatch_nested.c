/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The cross-package counterpart to test_dispatch.c: Branch.msg's own `leaf_value` field is a
// message from a *completely separate* ament package (rosidl_typesupport_tickle_c_tests_dep),
// not a same-package sibling (test_msgs/Nested.msg's own BasicTypes field, Milestone 40, is the
// same-package case - it never reaches rosidl_typesupport_tickle_c_generate_interfaces.cmake's
// own cross-package -I/include-dir/link-library wiring at all). Proves that wiring actually
// resolves, compiles, and links for real - not just that Branch.h's own text happens to parse -
// by round-tripping a real value through both fields, the nested one included.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rosidl_runtime_c/message_type_support_struct.h>
#include <rosidl_typesupport_tickle_c/identifier.h>
#include <rosidl_typesupport_tickle_c/message_type_support.h>

#include "rosidl_typesupport_tickle_c_tests/msg/branch.h"

int main(void) {
    const rosidl_message_type_support_t* top =
        ROSIDL_GET_MSG_TYPE_SUPPORT(rosidl_typesupport_tickle_c_tests, msg, Branch);
    assert(top != NULL);

    const rosidl_message_type_support_t* ours =
        get_message_typesupport_handle(top, rosidl_typesupport_tickle_c__identifier);
    assert(ours != NULL);
    assert(ours->typesupport_identifier == rosidl_typesupport_tickle_c__identifier);

    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks =
        (const rosidl_typesupport_tickle_c_message_callbacks_t*)ours->data;
    assert(callbacks != NULL);
    assert(callbacks->tickle_struct_size > 0 && callbacks->tickle_struct_size <= 64);
    assert(callbacks->ros_struct_size == sizeof(struct rosidl_typesupport_tickle_c_tests__msg__Branch));
    assert(strcmp(callbacks->ros_type_name, "rosidl_typesupport_tickle_c_tests/msg/Branch") == 0);

    struct rosidl_typesupport_tickle_c_tests__msg__Branch ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    ros_in.tag = 7;
    ros_in.leaf_value.value = -42;

    uint64_t tickle_storage[8] = {0}; // 64 bytes, naturally 8-byte aligned - see the assert above
    assert(callbacks->to_tickle(&ros_in, tickle_storage));

    struct rosidl_typesupport_tickle_c_tests__msg__Branch ros_out;
    memset(&ros_out, 0, sizeof(ros_out));
    assert(callbacks->from_tickle(tickle_storage, &ros_out));
    assert(ros_out.tag == 7);
    assert(ros_out.leaf_value.value == -42);

    printf("rosidl_typesupport_tickle_c cross-package nested dispatch: PASS\n");
    return 0;
}
