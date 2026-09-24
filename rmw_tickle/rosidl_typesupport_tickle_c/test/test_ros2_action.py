# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""A .action through ros2_cli: the implicit interfaces rosidl derives from it, which its action
type support dispatch asks every typesupport for. End to end - an rclcpp_action client and server
through rmw_tickle - is check_ros2_interfaces.sh -A (CI); these pin the generator's side."""

import pathlib

from rosidl_typesupport_tickle_c import ros2_cli
from tickle_typesupport import model

FIBONACCI = "int32 order\n---\nint32[] sequence\n---\nint32[] sequence\n"


def _workspace(root, action_text):
    root = pathlib.Path(root)
    (root / "mini_msgs" / "action").mkdir(parents=True)
    (root / "mini_msgs" / "msg").mkdir()
    (root / "mini_msgs" / "action" / "Fib.action").write_text(action_text)
    # What the goal id and SendGoal's stamp resolve to - installed interface packages, in CMake's run.
    (root / "unique_identifier_msgs" / "msg").mkdir(parents=True)
    (root / "unique_identifier_msgs" / "msg" / "UUID.msg").write_text("uint8[16] uuid\n")
    (root / "builtin_interfaces" / "msg").mkdir(parents=True)
    (root / "builtin_interfaces" / "msg" / "Time.msg").write_text("int32 sec\nuint32 nanosec\n")
    return root


def _generate(root, action_text):
    root = _workspace(root, action_text)
    out = root / "out"
    written = ros2_cli.generate(
        "mini_msgs",
        "action",
        "Fib",
        str(root / "mini_msgs" / "action" / "Fib.action"),
        str(out),
        include_dirs=[str(root)],
        typesupport_packages=["unique_identifier_msgs", "builtin_interfaces"],
    )
    return out, sorted(pathlib.Path(p).name for p in written)


def test_writes_exactly_the_files_cmake_declares(tmp_path):
    _out, names = _generate(tmp_path, FIBONACCI)
    expected, _ = ros2_cli._interface_files("mini_msgs", "action", "Fib")
    assert names == sorted(expected)
    for part in ("Goal", "Result", "Feedback", "FeedbackMessage", "SendGoal_Request", "GetResult_Response"):
        assert f"mini_msgs__action__Fib_{part}__type_support.c" in names


def test_wrappers_nest_the_actions_own_messages_by_their_ros_names(tmp_path):
    out, _ = _generate(tmp_path, FIBONACCI)
    request = (out / "mini_msgs__action__Fib_SendGoal_Request__rosidl_typesupport_tickle_c.c").read_text()
    assert "mini_msgs__action__Fib_Goal__to_tickle" in request
    assert "unique_identifier_msgs__msg__UUID__to_tickle" in request
    response = (out / "mini_msgs__action__Fib_GetResult_Response__rosidl_typesupport_tickle_cpp.cpp").read_text()
    assert "::mini_msgs::action::rosidl_typesupport_tickle_cpp::from_tickle" in response
    assert '#include "mini_msgs/action/fib.hpp"' in (
        out / "mini_msgs__action__Fib_Goal__rosidl_typesupport_tickle_cpp.hpp"
    ).read_text()


def test_an_auto_sized_result_leaves_room_for_its_wrapper(tmp_path):
    # int32[] sequence is auto-sized; filling the whole datagram would leave GetResult_Response
    # (status + result) and FeedbackMessage (goal id + feedback) unable to fit in one.
    out, _ = _generate(tmp_path, FIBONACCI)
    result = (out / "Fib_Result.h").read_text()
    capacity = int(result.split("int32_t sequence[")[1].split("]")[0])
    worst_wrapper = 16 + 4 + capacity * 4  # goal id, count, elements
    assert worst_wrapper + model.FRAMING_OVERHEAD <= model.max_buffer_length()
    assert capacity * 4 > model.max_buffer_length() - 256  # and it still uses the datagram


def test_one_unrepresentable_part_declines_the_whole_action(tmp_path):
    out, names = _generate(tmp_path, "wstring label\n---\nbool ok\n---\n")
    expected, message_names = ros2_cli._interface_files("mini_msgs", "action", "Fib")
    assert names == sorted(expected)
    assert "TickLE has no typesupport for mini_msgs/action/Fib" in (out / "Fib_Result.h").read_text()
    for message_name in message_names:
        header = (out / f"{message_name}__rosidl_typesupport_tickle_cpp.hpp").read_text()
        assert f"#define {message_name}__TICKLE_UNSUPPORTED 1" in header
