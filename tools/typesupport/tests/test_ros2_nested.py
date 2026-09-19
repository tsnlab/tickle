# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""resolve.Ros2Resolver (rmw_tickle/PLAN.md's Milestone 15/38 nested-message follow-up) - the
*same-package sibling* nested-message case a real ROS 2 package's own rosidl_typesupport_tickle_c
CMake extension actually needs (test_msgs/msg/Nested.msg's own `BasicTypes basic_types_value`,
mirrored here by fixtures_own/Branch.msg's own `Leaf leaf_value`). Unlike test_ros2_adapter.py's
own existing coverage (which only ever calls adapt.adapt_message(..., resolver=None) directly,
never reaching ros2_cli.py's own resolver plumbing at all), this drives ros2_cli.generate() twice -
once per .msg, exactly like the real CMake extension's own foreach() loop over every .msg in a
package - to prove the two independently-generated outputs (Leaf's own standalone codec, Branch's
own nested reference to it) actually compile and link *together*, not just that adapt.py accepts
the field without raising.

Both `ros2_cli.generate()` calls happen once, in the single module-scoped `ros2_nested_generated`
fixture below - never a second time from an individual test function. ros2_cli.generate() drives
render.render_topic(), which uses empy, and empy's Interpreter keeps *global* state
(Interpreter._wasProxyInstalled) that pytest's own stdout capturing conflicts with across
independent call sites ("interpreter stdout proxy lost") - see test_ros2_adapter.py's own
_generate_adapter() docstring for the same caveat, already hit and documented once before.
"""

import pathlib
import subprocess

import pytest

from conftest import CC, CFLAGS, REPO_ROOT
from tickle_typesupport import ros2_cli

FIXTURES_OWN = pathlib.Path(__file__).parent / "fixtures_own"
FIXTURES_ROS2_ADAPTER = pathlib.Path(__file__).parent / "fixtures_ros2_adapter"

_MAIN_C = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "Branch.h"
#include "Leaf.h"
#include "test_msgs__msg__Branch__rosidl_typesupport_tickle_c.h"
#include "test_msgs__msg__Leaf__rosidl_typesupport_tickle_c.h"

static void test_branch_nests_leaf_by_reusing_its_own_independent_codec(void) {
    struct test_msgs__msg__Branch ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    ros_in.tag = 7;
    ros_in.leaf_value.value = -42;

    struct BranchData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(test_msgs__msg__Branch__to_tickle(&ros_in, &tickle));

    /* The whole point: Branch's own nested field is a real, plain struct LeafData - the *exact
     * same type* Leaf.msg's own independent, separate generate() call produces (not a second,
     * differently-named copy) - so this line only compiles at all if resolve.Ros2Resolver's own
     * "reuse, don't re-generate" contract actually held. */
    struct LeafData* nested = &tickle.leaf_value;
    assert(nested->value == -42);
    assert(tickle.tag == 7);

    uint8_t buf[64];
    int32_t size = BranchData_encode_size(&tickle);
    assert(size > 0);
    int32_t encoded = BranchData_encode(&tickle, buf, sizeof(buf));
    assert(encoded == size);

    struct BranchData decoded_tickle;
    memset(&decoded_tickle, 0, sizeof(decoded_tickle));
    int32_t decoded = BranchData_decode(&decoded_tickle, buf, encoded, true);
    assert(decoded == encoded);
    assert(decoded_tickle.tag == 7);
    assert(decoded_tickle.leaf_value.value == -42);

    struct test_msgs__msg__Branch ros_out;
    memset(&ros_out, 0, sizeof(ros_out));
    assert(test_msgs__msg__Branch__from_tickle(&decoded_tickle, &ros_out));
    assert(ros_out.tag == 7);
    assert(ros_out.leaf_value.value == -42);

    BranchData_free(&decoded_tickle);
    printf("test_branch_nests_leaf_by_reusing_its_own_independent_codec: PASS\n");
}

int main(void) {
    test_branch_nests_leaf_by_reusing_its_own_independent_codec();
    printf("ALL PASS\n");
    return 0;
}
"""


@pytest.fixture(scope="module")
def ros2_nested_generated(tmp_path_factory):
    """Generates Leaf.msg and Branch.msg via ros2_cli.generate() - same entry point, same per-.msg
    invocation shape, as the real CMake extension - into one shared outdir. The ONLY place in this
    module that calls ros2_cli.generate() (see this module's own docstring on why) - returns
    (outdir, branch_written) so every test below shares this one generation pass instead of
    triggering empy a second time."""
    outdir = tmp_path_factory.mktemp("ros2_nested")

    ros2_cli.generate("test_msgs", "msg", "Leaf", str(FIXTURES_OWN / "Leaf.msg"), str(outdir), style_dir=str(REPO_ROOT))
    branch_written = ros2_cli.generate(
        "test_msgs", "msg", "Branch", str(FIXTURES_OWN / "Branch.msg"), str(outdir), style_dir=str(REPO_ROOT)
    )
    return outdir, branch_written


@pytest.fixture(scope="module")
def ros2_nested_check_binary(ros2_nested_generated):
    """Compiles both interfaces' own TickLE codec + adapter .c files, src/encoding.c/log.c, and a
    small C harness into one binary, the same way a real interface package's own single
    __rosidl_typesupport_tickle_c library links every .msg's own generated .c together."""
    outdir, _branch_written = ros2_nested_generated

    main_c = outdir / "main.c"
    main_c.write_text(_MAIN_C, encoding="utf-8")

    binary = outdir / "roundtrip_check"
    subprocess.run(
        [
            CC,
            *CFLAGS,
            f"-I{outdir}",
            f"-I{FIXTURES_ROS2_ADAPTER}",
            "-o",
            str(binary),
            str(main_c),
            str(outdir / "Leaf.c"),
            str(outdir / "Branch.c"),
            str(outdir / "test_msgs__msg__Leaf__rosidl_typesupport_tickle_c.c"),
            str(outdir / "test_msgs__msg__Branch__rosidl_typesupport_tickle_c.c"),
            str(REPO_ROOT / "src" / "encoding.c"),
            str(REPO_ROOT / "src" / "log.c"),
            "-lm",
        ],
        check=True,
    )
    return binary


def test_ros2_nested_roundtrip(ros2_nested_check_binary):
    result = subprocess.run([str(ros2_nested_check_binary)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ALL PASS" in result.stdout
    assert "test_branch_nests_leaf_by_reusing_its_own_independent_codec: PASS" in result.stdout


def test_ros2_resolver_writes_no_duplicate_leaf_file(ros2_nested_generated):
    """resolve.Ros2Resolver's own defining property (its own doc comment): resolving Branch's own
    nested `Leaf leaf_value` field must NOT write a second Leaf.h/.c (or any other file for it) -
    Leaf.msg's own separate, independent generate() call is the only thing that ever does."""
    _outdir, branch_written = ros2_nested_generated
    names = {pathlib.Path(p).name for p in branch_written}
    assert "Leaf.h" not in names
    assert "Leaf.c" not in names
    assert "test_msgs__msg__Leaf__rosidl_typesupport_tickle_c.h" not in names
