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

#include "rosidl_runtime_c/string.h"

struct test_msgs__msg__BoundedString {
    rosidl_runtime_c__String bounded_name;
    rosidl_runtime_c__String annotated_name;
    rosidl_runtime_c__String plain_name;
};
