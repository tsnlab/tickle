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
#include "rosidl_runtime_c/primitives_sequence_functions.h"

/* Milestone 55: a real `char[<=N]` field's own ROS 2 struct member is `rosidl_runtime_c__uint8__
 * Sequence`, not `rosidl_runtime_c__char__Sequence` - see primitives_sequence_functions.h's own
 * comment on the decoy type declared alongside this one, and ros2_adapter._ros2_sequence_scalar_
 * type()'s own doc comment for the full story. */
struct test_msgs__msg__CharArray {
    rosidl_runtime_c__uint8__Sequence char_values;
};
