/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */
// Prints rmw_serialize()'s bytes for the two fixed messages rmw_serialize_bytes.txt records
// (g1's criterion 4). Built by hand against a workspace that has the interface overlay, which is
// why it is not a colcon package - it ran once, before the direct codec reached rmw_serialize, to
// capture bytes the new encoder could be compared against rather than against itself:
//
//   gcc -O1 -o capture capture_rmw_serialize_bytes.c $INCLUDES $LIBS \
//       -lrmw_tickle -lstd_msgs__rosidl_generator_c -lstd_msgs__rosidl_typesupport_c \
//       -lsensor_msgs__rosidl_generator_c -lsensor_msgs__rosidl_typesupport_c \
//       -lrosidl_runtime_c -lrcutils -lrmw
//
// Kept so the record can be re-made deliberately if a wire change ever moves these bytes, which
// would be a wire-version change and not something to do quietly.
#include <stdio.h>
#include <string.h>

#include "rcutils/allocator.h"
#include "rmw/rmw.h"
#include "rmw/serialized_message.h"
#include "rosidl_runtime_c/primitives_sequence_functions.h"
#include "rosidl_runtime_c/string_functions.h"
#include "rosidl_typesupport_c/type_support_map.h"
#include "sensor_msgs/msg/detail/image__functions.h"
#include "sensor_msgs/msg/image.h"
#include "std_msgs/msg/detail/string__functions.h"
#include "std_msgs/msg/string.h"

static void dump(const char* name, const void* msg, const rosidl_message_type_support_t* ts) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_serialized_message_t buffer = rmw_get_zero_initialized_serialized_message();
    if (rmw_serialized_message_init(&buffer, 0, &allocator) != RMW_RET_OK) {
        printf("%s: init failed\n", name);
        return;
    }
    if (rmw_serialize(msg, ts, &buffer) != RMW_RET_OK) {
        printf("%s: serialize failed\n", name);
        return;
    }
    printf("GOLDEN %s %zu ", name, buffer.buffer_length);
    for (size_t i = 0; i < buffer.buffer_length; i++) {
        printf("%02x", buffer.buffer[i]);
    }
    printf("\n");
    (void)rmw_serialized_message_fini(&buffer);
}

int main(void) {
    std_msgs__msg__String text;
    std_msgs__msg__String__init(&text);
    rosidl_runtime_c__String__assign(&text.data, "golden-bytes");
    dump("std_msgs/String", &text, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String));
    std_msgs__msg__String__fini(&text);

    sensor_msgs__msg__Image image;
    sensor_msgs__msg__Image__init(&image);
    image.header.stamp.sec = 7;
    image.header.stamp.nanosec = 11;
    rosidl_runtime_c__String__assign(&image.header.frame_id, "camera");
    image.height = 2;
    image.width = 3;
    rosidl_runtime_c__String__assign(&image.encoding, "mono8");
    image.is_bigendian = 0;
    image.step = 3;
    rosidl_runtime_c__uint8__Sequence__init(&image.data, 6);
    for (size_t i = 0; i < image.data.size; i++) {
        image.data.data[i] = (uint8_t)(i * 17 + 1);
    }
    dump("sensor_msgs/Image", &image, ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, Image));
    sensor_msgs__msg__Image__fini(&image);
    return 0;
}
