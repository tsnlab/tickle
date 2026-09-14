# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""A tiny built-in copy of the two ROS 2 interfaces almost every other message nests
(`builtin_interfaces/Time`, `std_msgs/Header`), so a `.msg` that references them "just works"
without the caller having to vendor those two upstream packages behind a `-I` path of their own
just to get a timestamp. resolve.py falls back to these only when nothing on the caller's own
`-I` search path provides the same (package, name) - an explicit `-I` always wins, so a caller who
does vendor their own copy (e.g. a real ROS 2 checkout) is never overridden by this fallback.

Verbatim `.msg` text, not reparsed/rewritten from the originals, so a diff against upstream stays
meaningful. Fetched from `ros2` GitHub repositories at the `jazzy` ref, licensed Apache License
2.0 by Open Source Robotics Foundation, Inc. (per each source package's own `package.xml`) - same
provenance as tests/fixtures_ros2/ (see its own README.md), just embedded here instead of kept as
standalone files, since these two are load-bearing for the generator itself rather than only for
its test suite.
"""

TIME_MSG = """\
# This message communicates ROS Time defined here:
# https://design.ros2.org/articles/clock_and_time.html

# The seconds component, valid over all int32 values.
int32 sec

# The nanoseconds component, valid in the range [0, 1e9), to be added to the seconds component.
# e.g.
# The time -1.7 seconds is represented as {sec: -2, nanosec: 3e8}
# The time 1.7 seconds is represented as {sec: 1, nanosec: 7e8}
uint32 nanosec
"""

HEADER_MSG = """\
# Standard metadata for higher-level stamped data types.
# This is generally used to communicate timestamped data
# in a particular coordinate frame.

# Two-integer timestamp that is expressed as seconds and nanoseconds.
builtin_interfaces/Time stamp

# Transform frame with which this data is associated.
string frame_id
"""

# (package, message name) -> its .msg source text.
BUILTINS = {
    ("builtin_interfaces", "Time"): TIME_MSG,
    ("std_msgs", "Header"): HEADER_MSG,
}
