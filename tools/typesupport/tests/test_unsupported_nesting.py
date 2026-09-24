# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""What the generator does with a nested type from a package it has no typesupport for.

Both behaviours here were found the hard way. rosidl_typesupport_tickle_c could not build
performance_test at all: a message nesting std_msgs/Header resolved it off the -I search path and
assumed some other package would generate HeaderData, which nothing does, so the output carried
`#include "Header.h"` for a file that was never written. A provisioning workaround had masked it
for nine days by restricting generation to the only two types in that package with no
cross-package nesting - and its own note described that restriction as a path-derivation
limitation, which sent the next person looking in the wrong place.
"""

import pathlib
import subprocess
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
ROS_SHARE = pathlib.Path("/opt/ros")


def _ros_share_dir():
    for candidate in sorted(ROS_SHARE.glob("*/share")):
        if (candidate / "std_msgs" / "msg" / "Header.msg").is_file():
            return candidate
    return None


def _generate(tmp_path, name, body, include_dir):
    source = tmp_path / f"{name}.msg"
    source.write_text(body, encoding="utf-8")
    outdir = tmp_path / "out"
    outdir.mkdir(exist_ok=True)
    result = subprocess.run(
        [sys.executable, "-m", "tickle_typesupport.ros2_cli",
         "--package", "pkg_under_test", "--subfolder", "msg", "--name", name,
         "--input", str(source), "--outdir", str(outdir), "-I", str(include_dir)],
        capture_output=True, text=True, cwd=str(REPO_ROOT / "tools" / "typesupport"),
    )
    assert result.returncode == 0, f"generator failed outright:\n{result.stderr}"
    return outdir, result.stderr


@pytest.mark.skipif(_ros_share_dir() is None, reason="no ROS distribution installed to resolve std_msgs against")
def test_bundled_builtin_from_another_package_is_declined_too(tmp_path):
    """std_msgs/Header is declined despite TickLE bundling its own copy, and that is not an
    oversight.

    An earlier version of this fix drew the line at "TickLE has no struct for it" and made the
    bundled builtin win over the search path. That emitted std_msgs__Header.h correctly and still
    did not build: ros2_adapter.py also needs std_msgs' own
    ...__rosidl_typesupport_tickle_c.h to convert the ROS C struct into the TickLE one, and that
    exists only if std_msgs itself builds this typesupport. Having the struct is not the same as
    being able to adapt it, and only the second one makes a message usable from ROS 2.
    """
    outdir, stderr = _generate(tmp_path, "HeaderOnly", "std_msgs/Header header\nint64 value\n", _ros_share_dir())
    assert "DECLINED" in stderr
    assert "std_msgs/Header" in stderr
    # Nothing half-generated: no dangling include naming a file that was never written, which is
    # the exact shape of the bug that made performance_test unbuildable.
    header = (outdir / "HeaderOnly.h").read_text(encoding="utf-8")
    assert '#include "Header.h"' not in header
    assert '#include "std_msgs__Header.h"' not in header


@pytest.mark.skipif(_ros_share_dir() is None, reason="no ROS distribution installed to resolve sensor_msgs against")
def test_unsupported_nested_package_declines_without_failing_the_build(tmp_path):
    """sensor_msgs is not bundled and does not build this typesupport, so the type is declined.

    Declined, not failed: rosidl_generate_interfaces() hands the generator every type a package
    declares, so failing here would fail the whole package for consumers that never touch this
    type. That is exactly what made performance_test unbuildable.
    """
    outdir, stderr = _generate(
        tmp_path, "HasPointField", "sensor_msgs/PointField[8] fields\nint64 value\n", _ros_share_dir()
    )
    assert "DECLINED" in stderr
    assert "sensor_msgs/PointField" in stderr
    # The two reasons a type can be undeliverable have different remedies, so the message must not
    # be mistakable for the size limit.
    assert "NOT a size limit" in stderr

    header = (outdir / "HasPointField.h").read_text(encoding="utf-8")
    assert "sensor_msgs/PointField" in header, "the reason belongs in the file, not only in build output"
    # Nothing registered: rosidl's lookup must find no handle, so rmw refuses at create time.
    support = (outdir / "pkg_under_test__msg__HasPointField__type_support.c").read_text(encoding="utf-8")
    assert "get_message_type_support_handle" not in support

    # Every file CMake's add_custom_command declares still has to exist, or the build fails just as
    # hard as it did before - for a different reason, which is not an improvement.
    for expected in (
        "HasPointField.h", "HasPointField.c",
        "pkg_under_test__msg__HasPointField__rosidl_typesupport_tickle_c.h",
        "pkg_under_test__msg__HasPointField__rosidl_typesupport_tickle_c.c",
        "pkg_under_test__msg__HasPointField__type_support.c",
    ):
        assert (outdir / expected).is_file(), f"{expected} was not written"
