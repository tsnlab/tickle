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

#include "nested_arrays_pkg/msg/odd_align.h"

// Hand-written stand-in for nested_arrays_pkg/msg/NestedArrays.msg (tests/fixtures_own/
// nested_arrays_pkg/msg/NestedArrays.msg) - see odd_align.h's own comment. A fixed array of a
// nested message is a plain in-place C array (no Sequence wrapper - matches a real ROS 2
// generated struct exactly, e.g. action_msgs__msg__GoalStatusArray's own status_list field is
// the *variable* case, `action_msgs__msg__GoalStatus__Sequence`, verified against a real
// installed ROS 2 package); the two variable arrays are each that element type's own __Sequence.
struct nested_arrays_pkg__msg__NestedArrays {
    struct nested_arrays_pkg__msg__OddAlign fixed_items[2];
    nested_arrays_pkg__msg__OddAlign__Sequence bounded_items;
    nested_arrays_pkg__msg__OddAlign__Sequence tagged_items;
};
