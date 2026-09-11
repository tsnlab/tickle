# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""M0: prove the vendored ROS 2 parser (tickle_typesupport._rosidl_parser) accepts real ROS 2
interface files, not just TickLE's own small examples/*.msg /*.srv. Fixtures in fixtures_ros2/
are unmodified files fetched from ros2/common_interfaces + ros2/rcl_interfaces (jazzy) laid out
in their real pkg/msg/Name.msg form, since the parser validates the message name derived from
that path.

This is a parse-smoke test, not a codegen test: M1+ add layout/roundtrip/golden coverage once
there's something to generate.
"""

import pathlib

import pytest

from tickle_typesupport import _rosidl_parser as rosidl
from tickle_typesupport.cli import _guess_package_and_name

FIXTURES = pathlib.Path(__file__).parent / "fixtures_ros2"


def _all_fixtures():
    return sorted(FIXTURES.rglob("*.msg")) + sorted(FIXTURES.rglob("*.srv"))


@pytest.mark.parametrize("path", _all_fixtures(), ids=lambda p: str(p.relative_to(FIXTURES)))
def test_real_ros2_interface_parses(path):
    package, name = _guess_package_and_name(str(path))
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".msg":
        spec = rosidl.parse_message_string(package, name, text)
        assert spec.fields or spec.constants
    else:
        spec = rosidl.parse_service_string(package, name, text)
        assert spec.request is not None and spec.response is not None


def test_nested_same_package_type_is_resolved():
    # geometry_msgs/Twist.msg references a bare "Vector3" (same package, no prefix) - the parser
    # must still record it as geometry_msgs/Vector3, not leave the package unset, or resolve.py
    # (M3) would have no way to find the referenced .msg file.
    path = FIXTURES / "geometry_msgs" / "msg" / "Twist.msg"
    spec = rosidl.parse_message_string("geometry_msgs", "Twist", path.read_text())
    assert [f.name for f in spec.fields] == ["linear", "angular"]
    for field in spec.fields:
        assert field.type.pkg_name == "geometry_msgs"
        assert field.type.type == "Vector3"


def test_own_examples_still_parse():
    # TickLE's own hand-written interface sources (the ones the hand-written codecs were derived
    # from) must parse too - these are the first ones M1 regenerates.
    examples = pathlib.Path(__file__).parent.parent.parent.parent / "examples"
    for name in ("UInt64.msg", "SetBool.srv", "Trigger.srv", "Image.msg"):
        path = examples / name
        package, ifname = _guess_package_and_name(str(path))
        text = path.read_text(encoding="utf-8")
        if path.suffix == ".msg":
            rosidl.parse_message_string(package, ifname, text)
        else:
            rosidl.parse_service_string(package, ifname, text)
