# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""ros2_cpp_adapter: the C++ converters rclcpp's messages go through. Their compiled behaviour is
checked end to end by check_ros2_interfaces.sh -r (CI); these pin the choices that are easy to
undo by accident, each of which was a way the C converters broke C++ messages."""

from rosidl_typesupport_tickle_c import ros2_cli


def _generate(tmp_path, name, text):
    pkg = tmp_path / "mini_msgs"
    (pkg / "msg").mkdir(parents=True)
    (pkg / "msg" / f"{name}.msg").write_text(text)
    out = tmp_path / "out"
    ros2_cli.generate("mini_msgs", "msg", name, str(pkg / "msg" / f"{name}.msg"), str(out))
    return (out / f"mini_msgs__msg__{name}__rosidl_typesupport_tickle_cpp.cpp").read_text()


def test_sequences_use_the_cpp_container_not_the_c_struct(tmp_path):
    source = _generate(tmp_path, "Seq", "uint8[<=8] data\nstring name\n")
    # The C converters read .size/.data as struct members; on a std::vector those are the end and
    # begin pointers, which is the bug this module exists for.
    assert "ros.data.size()" in source
    assert "ros.data.size >" not in source
    assert "const_cast<char *>(ros.name.c_str())" in source  # clang-formatted
    assert "ros.data.resize(tickle->data_count);" in source


def test_bool_sequences_are_copied_element_by_element(tmp_path):
    # std::vector<bool> is a bitset with no data(): a memcpy() from it would not compile, or worse.
    source = _generate(tmp_path, "Flags", "bool[<=4] flags\n")
    assert "memcpy" not in source.split("bool to_tickle", 1)[1]
    assert "tickle->flags[i] = ros.flags[i];" in source


def test_a_null_string_from_the_wire_becomes_an_empty_one(tmp_path):
    source = _generate(tmp_path, "Named", "string name\n")
    assert 'ros.name = (tickle->name != nullptr ? tickle->name : "");' in source


def test_layout_checks_stay_on_in_cpp(tmp_path):
    # The TickLE header's _Static_assert layout checks are mapped to static_assert, not disabled.
    source = _generate(tmp_path, "Plain", "int32 value\n")
    assert "#define _Static_assert static_assert" in source
