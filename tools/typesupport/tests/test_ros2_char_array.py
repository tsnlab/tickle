# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""ros2_adapter.py's own real ROS 2 <-> TickLE conversion for a `char[<=N]` field (Milestone 55) -
isolated from test_ros2_adapter.py's own shared Arrays.msg fixture on purpose (a fourth array
field there would perturb test_crossendian.py's/test_golden.py's/test_lint.py's own unrelated
coverage of that same message). Real, previously-latent bug this reproduces: a real ROS 2 install's
own generated struct member for an IDL `char[<=N]` field is `rosidl_runtime_c__uint8__Sequence`,
not `rosidl_runtime_c__char__Sequence` (a real, distinct type `rosidl_runtime_c` also happens to
separately declare) - `rosidl_runtime_c/primitives_sequence_functions.h`'s own fixture here
declares both, exactly like a real ROS 2 install's headers do, so calling the wrong one fails to
compile (a hard pointer-type mismatch) the same way it did in a real `colcon build` - `conftest.
CFLAGS`'s own `-Werror=incompatible-pointer-types` is what makes that failure actually fail this
test, not just print a warning `subprocess.run(..., check=True)` would otherwise ignore.
"""

import pathlib
import subprocess

from conftest import CC, CFLAGS, REPO_ROOT
from tickle_typesupport import ros2_cli

FIXTURES_OWN = pathlib.Path(__file__).parent / "fixtures_own"
FIXTURES_ROS2_ADAPTER = pathlib.Path(__file__).parent / "fixtures_ros2_adapter"

_MAIN_C = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "CharArray.h"
#include "test_msgs__msg__CharArray__rosidl_typesupport_tickle_c.h"

static void test_char_array_roundtrip(void) {
    struct test_msgs__msg__CharArray ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    uint8_t char_values_data[2] = {'a', 'b'}; /* rosidl_runtime_c__uint8__Sequence.data is uint8_t* */
    ros_in.char_values.data = char_values_data;
    ros_in.char_values.size = 2;
    ros_in.char_values.capacity = 2;

    struct CharArrayData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(test_msgs__msg__CharArray__to_tickle(&ros_in, &tickle));

    uint8_t buf[64];
    int32_t size = CharArrayData_encode_size(&tickle);
    assert(size > 0);
    int32_t encoded = CharArrayData_encode(&tickle, buf, sizeof(buf));
    assert(encoded == size);

    struct CharArrayData decoded_tickle;
    memset(&decoded_tickle, 0, sizeof(decoded_tickle));
    int32_t decoded = CharArrayData_decode(&decoded_tickle, buf, encoded, true);
    assert(decoded == encoded);

    struct test_msgs__msg__CharArray ros_out;
    memset(&ros_out, 0, sizeof(ros_out));
    assert(test_msgs__msg__CharArray__from_tickle(&decoded_tickle, &ros_out));

    assert(ros_out.char_values.size == 2);
    assert(memcmp(ros_out.char_values.data, char_values_data, sizeof(char_values_data)) == 0);

    CharArrayData_free(&decoded_tickle);
    printf("test_char_array_roundtrip: PASS\n");
}

int main(void) {
    test_char_array_roundtrip();
    printf("ALL PASS\n");
    return 0;
}
"""


def test_char_array_roundtrip(tmp_path):
    ros2_cli.generate(
        "test_msgs",
        "msg",
        "CharArray",
        str(FIXTURES_OWN / "char_array_pkg" / "msg" / "CharArray.msg"),
        str(tmp_path),
        style_dir=str(REPO_ROOT),
    )

    main_c = tmp_path / "main.c"
    main_c.write_text(_MAIN_C, encoding="utf-8")

    binary = tmp_path / "char_array_check"
    subprocess.run(
        [
            CC,
            *CFLAGS,
            f"-I{tmp_path}",
            f"-I{FIXTURES_ROS2_ADAPTER}",
            "-o",
            str(binary),
            str(main_c),
            str(tmp_path / "CharArray.c"),
            str(tmp_path / "test_msgs__msg__CharArray__rosidl_typesupport_tickle_c.c"),
            str(REPO_ROOT / "src" / "encoding.c"),
            str(REPO_ROOT / "src" / "log.c"),
        ],
        check=True,
    )

    result = subprocess.run([str(binary)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ALL PASS" in result.stdout
    assert "test_char_array_roundtrip: PASS" in result.stdout
