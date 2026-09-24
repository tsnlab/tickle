# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Shared setup for the ROS 2 half of the generator's tests (rosidl_typesupport_tickle_c).

These tests moved here from tools/typesupport/tests/ with the code they cover (2026-09-24, the
user's decision that every ROS-related part lives in rmw_tickle). They still lean on the core
suite's shared fixtures - the session-scoped `generated_dir` of TickLE codecs for the core
fixture messages, and the compiler settings - so this re-exports those rather than keeping a
second copy that could drift.

Both packages are imported from this checkout's source, put first on sys.path. A pip-installed
tickle_typesupport in site-packages is exactly what a test run must not silently pick up instead:
it is frozen at whatever was installed last, and a regression check run against it checks nothing
(feedback from 2026-09-23, when that is what happened).

Run on its own - `python3 -m pytest rmw_tickle/rosidl_typesupport_tickle_c/test` - not in one
session with tools/typesupport/tests: both directories have a conftest.py, and pytest cannot
import two modules named `conftest` side by side.
"""

import importlib.util
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
CORE = REPO_ROOT / "tools" / "typesupport"

sys.path.insert(0, str(CORE))
sys.path.insert(0, str(HERE.parent))

_spec = importlib.util.spec_from_file_location("tickle_typesupport_core_conftest", CORE / "tests" / "conftest.py")
_core = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_core)

CC = _core.CC
CFLAGS = _core.CFLAGS
generated_dir = _core.generated_dir  # a fixture - pytest collects it from this module's namespace

# Core's fixture messages (Arrays.msg, BoundedString.msg, nested_arrays_pkg/, ...) - shared with
# TickLE's own codegen tests, so they stay there.
FIXTURES_CORE = CORE / "tests" / "fixtures_own"
# ROS-only fixtures: packages in ROS 2's pkg/msg layout, and ros2_adapter/'s hand-written
# rosidl_runtime_c stand-ins (see its README).
FIXTURES = HERE / "fixtures"
FIXTURES_ROS2_ADAPTER = FIXTURES / "ros2_adapter"
