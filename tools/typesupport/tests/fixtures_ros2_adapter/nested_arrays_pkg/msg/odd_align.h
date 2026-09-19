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
#include <stdint.h>
#include <stdlib.h>

// Hand-written stand-in for what rosidl_generator_c would emit for nested_arrays_pkg/msg/
// OddAlign.msg (tests/fixtures_own/nested_arrays_pkg/msg/OddAlign.msg) - see fixtures_ros2_
// adapter/README.md. Also declares OddAlign's own __Sequence type/__init() the same file real
// rosidl_generator_c would (nested_arrays_pkg/msg/odd_align.h's own umbrella header always
// #includes its own detail/odd_align__functions.h, which is where __Sequence__init actually
// lives - verified against a real installed ROS 2 package, action_msgs/msg/goal_status.h -
// collapsed into this one file here since there's no real build-system split to mirror).
struct nested_arrays_pkg__msg__OddAlign {
    bool flag;
    int64_t big;
    uint8_t tail;
};

typedef struct nested_arrays_pkg__msg__OddAlign__Sequence {
    struct nested_arrays_pkg__msg__OddAlign* data;
    size_t size;
    size_t capacity;
} nested_arrays_pkg__msg__OddAlign__Sequence;

static inline bool nested_arrays_pkg__msg__OddAlign__Sequence__init(nested_arrays_pkg__msg__OddAlign__Sequence* seq,
                                                                    size_t size) {
    seq->data = (struct nested_arrays_pkg__msg__OddAlign*)calloc(size, sizeof(struct nested_arrays_pkg__msg__OddAlign));
    if (seq->data == NULL && size > 0) {
        return false;
    }
    seq->size = size;
    seq->capacity = size;
    return true;
}
