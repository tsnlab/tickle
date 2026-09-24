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
#include <stdint.h>

#include "rosidl_runtime_c/primitives_sequence_functions.h"

struct test_msgs__msg__Arrays {
    uint8_t fixed_bytes[4];
    int32_t fixed_ints[3];
    rosidl_runtime_c__uint16__Sequence bounded_values;
    rosidl_runtime_c__float32__Sequence samples;
};
