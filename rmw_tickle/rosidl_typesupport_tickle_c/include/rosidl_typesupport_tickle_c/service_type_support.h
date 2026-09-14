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

#ifdef __cplusplus
extern "C" {
#endif

// rosidl's typesupport contract never inspects a rosidl_service_type_support_t's own `.data`
// pointer either (same rationale as message_type_support.h's own doc comment) - this shape is
// private to rmw_tickle and the code tools/typesupport/tickle_typesupport/ros2_cli.py generates
// (ros2_adapter.render_service_type_support()). Deliberately minimal: `.request_typesupport`/
// `.response_typesupport` (rosidl_service_type_support_t's own, standard fields) already carry
// everything needed to reach each side's own rosidl_typesupport_tickle_c_message_callbacks_t
// (rmw_tickle's own rmw_tickle_get_message_callbacks(), applied to each) - this only adds what
// neither side has: the service's own type name, which TickLE's struct tt_Service needs the same
// way struct tt_Topic needs a message's own ros_type_name (see message_type_support.h) -
// tt_hash_id(service->name, endpoint_name) mixes in both, matching ROS 2's own "type and service
// name must both match to connect" rule; endpoint_name is the ROS service name.
typedef struct rosidl_typesupport_tickle_c_service_callbacks_t {
    const char* ros_type_name; // "pkg/srv/Type"
} rosidl_typesupport_tickle_c_service_callbacks_t;

#ifdef __cplusplus
}
#endif
