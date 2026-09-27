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
    assert "ros.data.assign(tickle->data, tickle->data + tickle->data_count);" in source


def test_primitive_sequences_and_arrays_copy_through_the_container_not_a_loop(tmp_path):
    # RMW_PERF_PLAN.md 12.1: an element loop over uint8_t could not be vectorised (its stores may alias the
    # vector's own pointer) and cost 40x a memcpy on a 64-KB Image. std::copy / assign() over the container's
    # iterators is one memmove for contiguous trivially copyable elements.
    source = _generate(tmp_path, "Bulk", "uint8[<=65000] data\nfloat64[3] xyz\n")
    to_part, from_part = source.split("bool to_tickle", 1)[1].split("bool from_tickle", 1)
    assert "std::copy(ros.data.begin(), ros.data.end(), tickle->data);" in to_part
    assert "std::copy(ros.xyz.begin(), ros.xyz.end(), tickle->xyz);" in to_part
    assert "ros.data.assign(tickle->data, tickle->data + tickle->data_count);" in from_part
    assert "std::copy(tickle->xyz, tickle->xyz + 3, ros.xyz.begin());" in from_part
    assert "[i] = " not in to_part and "[i] = " not in from_part
    assert "#include <algorithm>" in source


def test_bool_sequences_never_memcpy_from_the_bitset(tmp_path):
    # std::vector<bool> is a bitset with no data(): a memcpy() from it would not compile, or worse. std::copy over
    # its iterators is element-wise by construction, so it stays correct.
    source = _generate(tmp_path, "Flags", "bool[<=4] flags\n")
    assert "memcpy" not in source.split("bool to_tickle", 1)[1]
    assert "std::copy(ros.flags.begin(), ros.flags.end(), tickle->flags);" in source


def test_a_null_string_from_the_wire_becomes_an_empty_one(tmp_path):
    source = _generate(tmp_path, "Named", "string name\n")
    assert 'ros.name = (tickle->name != nullptr ? tickle->name : "");' in source


def test_layout_checks_stay_on_in_cpp(tmp_path):
    # The TickLE header's _Static_assert layout checks are mapped to static_assert, not disabled.
    source = _generate(tmp_path, "Plain", "int32 value\n")
    assert "#define _Static_assert static_assert" in source
