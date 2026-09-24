# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""The ROS 2 half of TickLE's type support generator: turns a ROS 2 package's .msg/.srv into
TickLE codecs plus the adapters and rosidl type-support handles that make them reachable from
rmw_tickle. Run by this package's CMake extension during a ROS 2 build (ros2_cli).

TickLE's own, ROS-agnostic generator is tools/typesupport (the tickle_typesupport Python
package); everything here builds on it and nothing there imports from here. Moved out of
tickle_typesupport on 2026-09-24 by the user's decision that every ROS-related part lives in
rmw_tickle.
"""
