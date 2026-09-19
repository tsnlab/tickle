# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""tickle_typesupport.ros2_adapter (rmw_tickle/PLAN.md's Milestone 1) generates a converter
between a real ROS 2 interface package's own rosidl_generator_c struct and TickLE's own codec for
that same message - see ros2_adapter.py's own module docstring for the exact field mapping this
assumes. No ROS 2 install exists in this tool's own dev/test environment (see that same
docstring), so this compiles and round-trips the generated converter against fixtures_ros2_
adapter/'s hand-written stand-ins for what rosidl_generator_c/rosidl_runtime_c would actually
provide - proving the *generator's own logic* (field-by-field conversion, bounds checking)
independently of whether every rosidl_runtime_c API name recalled by hand is byte-for-byte what a
real ROS 2 install has (see fixtures_ros2_adapter/README.md).
"""

import pathlib
import subprocess

import pytest

from conftest import CC, CFLAGS, REPO_ROOT
from tickle_typesupport import _rosidl_parser as rosidl
from tickle_typesupport import adapt, layout, postprocess, resolve, ros2_adapter

FIXTURES_OWN = pathlib.Path(__file__).parent / "fixtures_own"
FIXTURES_ROS2_ADAPTER = pathlib.Path(__file__).parent / "fixtures_ros2_adapter"
NESTED_ARRAYS_MSG_DIR = FIXTURES_OWN / "nested_arrays_pkg" / "msg"

_MAIN_C = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "Arrays.h"
#include "BoundedString.h"
#include "NestedArrays.h"
#include "StringArrays.h"
#include "nested_arrays_pkg__msg__NestedArrays__rosidl_typesupport_tickle_c.h"
#include "test_msgs__msg__Arrays__rosidl_typesupport_tickle_c.h"
#include "test_msgs__msg__BoundedString__rosidl_typesupport_tickle_c.h"
#include "test_msgs__msg__StringArrays__rosidl_typesupport_tickle_c.h"

static void test_arrays_roundtrip(void) {
    struct test_msgs__msg__Arrays ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    ros_in.fixed_bytes[0] = 1;
    ros_in.fixed_bytes[1] = 2;
    ros_in.fixed_bytes[2] = 3;
    ros_in.fixed_bytes[3] = 4;
    ros_in.fixed_ints[0] = -1;
    ros_in.fixed_ints[1] = 0;
    ros_in.fixed_ints[2] = 2000000000;

    uint16_t bounded_data[3] = {10, 20, 30};
    ros_in.bounded_values.data = bounded_data;
    ros_in.bounded_values.size = 3;
    ros_in.bounded_values.capacity = 3;

    float samples_data[2] = {1.5f, -2.25f};
    ros_in.samples.data = samples_data;
    ros_in.samples.size = 2;
    ros_in.samples.capacity = 2;

    struct ArraysData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(test_msgs__msg__Arrays__to_tickle(&ros_in, &tickle));

    uint8_t buf[256];
    int32_t size = ArraysData_encode_size(&tickle);
    assert(size > 0);
    int32_t encoded = ArraysData_encode(&tickle, buf, sizeof(buf));
    assert(encoded == size);

    struct ArraysData decoded_tickle;
    memset(&decoded_tickle, 0, sizeof(decoded_tickle));
    int32_t decoded = ArraysData_decode(&decoded_tickle, buf, encoded, true);
    assert(decoded == encoded);

    struct test_msgs__msg__Arrays ros_out;
    memset(&ros_out, 0, sizeof(ros_out));
    assert(test_msgs__msg__Arrays__from_tickle(&decoded_tickle, &ros_out));

    assert(memcmp(ros_out.fixed_bytes, ros_in.fixed_bytes, sizeof(ros_in.fixed_bytes)) == 0);
    assert(memcmp(ros_out.fixed_ints, ros_in.fixed_ints, sizeof(ros_in.fixed_ints)) == 0);
    assert(ros_out.bounded_values.size == 3);
    assert(memcmp(ros_out.bounded_values.data, bounded_data, sizeof(bounded_data)) == 0);
    assert(ros_out.samples.size == 2);
    assert(memcmp(ros_out.samples.data, samples_data, sizeof(samples_data)) == 0);

    ArraysData_free(&decoded_tickle);
    printf("test_arrays_roundtrip: PASS\n");
}

static void test_arrays_rejects_over_capacity(void) {
    struct test_msgs__msg__Arrays ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    uint16_t bounded_data[9] = {0};
    ros_in.bounded_values.data = bounded_data;
    ros_in.bounded_values.size = 9; /* one past the ROS 2 upper bound (<=8) */
    ros_in.bounded_values.capacity = 9;

    struct ArraysData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(!test_msgs__msg__Arrays__to_tickle(&ros_in, &tickle));
    printf("test_arrays_rejects_over_capacity: PASS\n");
}

static void test_bounded_string_roundtrip(void) {
    struct test_msgs__msg__BoundedString ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    ros_in.bounded_name.data = (char*)"eight888"; /* exactly capacity (8) */
    ros_in.bounded_name.size = 8;
    ros_in.annotated_name.data = (char*)"abcd"; /* exactly capacity (4) */
    ros_in.annotated_name.size = 4;
    ros_in.plain_name.data = (char*)"unbounded and free";
    ros_in.plain_name.size = strlen(ros_in.plain_name.data);

    struct BoundedStringData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(test_msgs__msg__BoundedString__to_tickle(&ros_in, &tickle));

    uint8_t buf[256];
    int32_t size = BoundedStringData_encode_size(&tickle);
    assert(size > 0);
    int32_t encoded = BoundedStringData_encode(&tickle, buf, sizeof(buf));
    assert(encoded == size);

    struct BoundedStringData decoded_tickle;
    memset(&decoded_tickle, 0, sizeof(decoded_tickle));
    int32_t decoded = BoundedStringData_decode(&decoded_tickle, buf, encoded, true);
    assert(decoded == encoded);

    struct test_msgs__msg__BoundedString ros_out;
    memset(&ros_out, 0, sizeof(ros_out));
    assert(test_msgs__msg__BoundedString__from_tickle(&decoded_tickle, &ros_out));

    assert(strcmp(ros_out.bounded_name.data, "eight888") == 0);
    assert(strcmp(ros_out.annotated_name.data, "abcd") == 0);
    assert(strcmp(ros_out.plain_name.data, "unbounded and free") == 0);

    BoundedStringData_free(&decoded_tickle);
    printf("test_bounded_string_roundtrip: PASS\n");
}

static void test_bounded_string_rejects_over_capacity(void) {
    struct test_msgs__msg__BoundedString ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    ros_in.bounded_name.data = (char*)"toolongvalue"; /* 12 chars, over capacity (8) */
    ros_in.bounded_name.size = 12;
    ros_in.annotated_name.data = (char*)"";
    ros_in.plain_name.data = (char*)"";

    struct BoundedStringData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(!test_msgs__msg__BoundedString__to_tickle(&ros_in, &tickle));
    printf("test_bounded_string_rejects_over_capacity: PASS\n");
}

static void test_string_arrays_roundtrip(void) {
    struct test_msgs__msg__StringArrays ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    assert(rosidl_runtime_c__String__assign(&ros_in.fixed_names[0], "alpha"));
    assert(rosidl_runtime_c__String__assign(&ros_in.fixed_names[1], ""));
    assert(rosidl_runtime_c__String__assign(&ros_in.fixed_names[2], "gamma"));

    assert(rosidl_runtime_c__String__Sequence__init(&ros_in.bounded_names, 2));
    assert(rosidl_runtime_c__String__assign(&ros_in.bounded_names.data[0], "one"));
    assert(rosidl_runtime_c__String__assign(&ros_in.bounded_names.data[1], "two"));

    assert(rosidl_runtime_c__String__Sequence__init(&ros_in.tagged_names, 3));
    assert(rosidl_runtime_c__String__assign(&ros_in.tagged_names.data[0], "x"));
    assert(rosidl_runtime_c__String__assign(&ros_in.tagged_names.data[1], "yy"));
    assert(rosidl_runtime_c__String__assign(&ros_in.tagged_names.data[2], "zzz"));

    struct StringArraysData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(test_msgs__msg__StringArrays__to_tickle(&ros_in, &tickle));

    uint8_t buf[256];
    int32_t size = StringArraysData_encode_size(&tickle);
    assert(size > 0);
    int32_t encoded = StringArraysData_encode(&tickle, buf, sizeof(buf));
    assert(encoded == size);

    struct StringArraysData decoded_tickle;
    memset(&decoded_tickle, 0, sizeof(decoded_tickle));
    int32_t decoded = StringArraysData_decode(&decoded_tickle, buf, encoded, true);
    assert(decoded == encoded);

    struct test_msgs__msg__StringArrays ros_out;
    memset(&ros_out, 0, sizeof(ros_out));
    assert(test_msgs__msg__StringArrays__from_tickle(&decoded_tickle, &ros_out));

    assert(strcmp(ros_out.fixed_names[0].data, "alpha") == 0);
    assert(strcmp(ros_out.fixed_names[1].data, "") == 0);
    assert(strcmp(ros_out.fixed_names[2].data, "gamma") == 0);
    assert(ros_out.bounded_names.size == 2);
    assert(strcmp(ros_out.bounded_names.data[0].data, "one") == 0);
    assert(strcmp(ros_out.bounded_names.data[1].data, "two") == 0);
    assert(ros_out.tagged_names.size == 3);
    assert(strcmp(ros_out.tagged_names.data[0].data, "x") == 0);
    assert(strcmp(ros_out.tagged_names.data[1].data, "yy") == 0);
    assert(strcmp(ros_out.tagged_names.data[2].data, "zzz") == 0);

    StringArraysData_free(&decoded_tickle);
    printf("test_string_arrays_roundtrip: PASS\n");
}

static void test_string_arrays_rejects_over_capacity(void) {
    struct test_msgs__msg__StringArrays ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    assert(rosidl_runtime_c__String__assign(&ros_in.fixed_names[0], ""));
    assert(rosidl_runtime_c__String__assign(&ros_in.fixed_names[1], ""));
    assert(rosidl_runtime_c__String__assign(&ros_in.fixed_names[2], ""));
    assert(rosidl_runtime_c__String__Sequence__init(&ros_in.bounded_names, 5)); /* one past <=4 */
    for (size_t i = 0; i < 5; i++) {
        assert(rosidl_runtime_c__String__assign(&ros_in.bounded_names.data[i], "x"));
    }

    struct StringArraysData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(!test_msgs__msg__StringArrays__to_tickle(&ros_in, &tickle));
    printf("test_string_arrays_rejects_over_capacity: PASS\n");
}

static void test_nested_arrays_roundtrip(void) {
    struct nested_arrays_pkg__msg__NestedArrays ros_in;
    memset(&ros_in, 0, sizeof(ros_in));

    ros_in.fixed_items[0].flag = true;
    ros_in.fixed_items[0].big = 100;
    ros_in.fixed_items[0].tail = 1;
    ros_in.fixed_items[1].flag = false;
    ros_in.fixed_items[1].big = -200;
    ros_in.fixed_items[1].tail = 2;

    assert(nested_arrays_pkg__msg__OddAlign__Sequence__init(&ros_in.bounded_items, 2));
    ros_in.bounded_items.data[0].flag = true;
    ros_in.bounded_items.data[0].big = 300;
    ros_in.bounded_items.data[0].tail = 3;
    ros_in.bounded_items.data[1].flag = false;
    ros_in.bounded_items.data[1].big = 400;
    ros_in.bounded_items.data[1].tail = 4;

    assert(nested_arrays_pkg__msg__OddAlign__Sequence__init(&ros_in.tagged_items, 3));
    for (size_t i = 0; i < 3; i++) {
        ros_in.tagged_items.data[i].flag = (i % 2 == 0);
        ros_in.tagged_items.data[i].big = (int64_t)(1000 + i);
        ros_in.tagged_items.data[i].tail = (uint8_t)(10 + i);
    }

    struct NestedArraysData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(nested_arrays_pkg__msg__NestedArrays__to_tickle(&ros_in, &tickle));

    uint8_t buf[256];
    int32_t size = NestedArraysData_encode_size(&tickle);
    assert(size > 0);
    int32_t encoded = NestedArraysData_encode(&tickle, buf, sizeof(buf));
    assert(encoded == size);

    struct NestedArraysData decoded_tickle;
    memset(&decoded_tickle, 0, sizeof(decoded_tickle));
    int32_t decoded = NestedArraysData_decode(&decoded_tickle, buf, encoded, true);
    assert(decoded == encoded);

    struct nested_arrays_pkg__msg__NestedArrays ros_out;
    memset(&ros_out, 0, sizeof(ros_out));
    assert(nested_arrays_pkg__msg__NestedArrays__from_tickle(&decoded_tickle, &ros_out));

    assert(ros_out.fixed_items[0].flag == true);
    assert(ros_out.fixed_items[0].big == 100);
    assert(ros_out.fixed_items[0].tail == 1);
    assert(ros_out.fixed_items[1].flag == false);
    assert(ros_out.fixed_items[1].big == -200);
    assert(ros_out.fixed_items[1].tail == 2);

    assert(ros_out.bounded_items.size == 2);
    assert(ros_out.bounded_items.data[0].flag == true);
    assert(ros_out.bounded_items.data[0].big == 300);
    assert(ros_out.bounded_items.data[1].flag == false);
    assert(ros_out.bounded_items.data[1].big == 400);

    assert(ros_out.tagged_items.size == 3);
    for (size_t i = 0; i < 3; i++) {
        assert(ros_out.tagged_items.data[i].big == (int64_t)(1000 + i));
        assert(ros_out.tagged_items.data[i].tail == (uint8_t)(10 + i));
    }

    NestedArraysData_free(&decoded_tickle);
    printf("test_nested_arrays_roundtrip: PASS\n");
}

static void test_nested_arrays_rejects_over_capacity(void) {
    struct nested_arrays_pkg__msg__NestedArrays ros_in;
    memset(&ros_in, 0, sizeof(ros_in));
    assert(nested_arrays_pkg__msg__OddAlign__Sequence__init(&ros_in.bounded_items, 4)); /* one past <=3 */

    struct NestedArraysData tickle;
    memset(&tickle, 0, sizeof(tickle));
    assert(!nested_arrays_pkg__msg__NestedArrays__to_tickle(&ros_in, &tickle));
    printf("test_nested_arrays_rejects_over_capacity: PASS\n");
}

int main(void) {
    test_arrays_roundtrip();
    test_arrays_rejects_over_capacity();
    test_bounded_string_roundtrip();
    test_bounded_string_rejects_over_capacity();
    test_string_arrays_roundtrip();
    test_string_arrays_rejects_over_capacity();
    test_nested_arrays_roundtrip();
    test_nested_arrays_rejects_over_capacity();
    printf("ALL PASS\n");
    return 0;
}
"""


def _write_adapter_files(outdir, struct, ros_name, tickle_header, msg_name_for_banner):
    """Renders + writes <ros_name>__rosidl_typesupport_tickle_c.{h,c} into outdir - the shared
    tail end of _generate_adapter() below, also used directly by ros2_adapter_check_binary() for
    NestedArrays/OddAlign (which need to render *two* messages off of one shared resolver's own
    resolved struct - see that fixture's own comment on why _generate_adapter()'s normal
    "adapt this fixture fresh" flow doesn't fit there)."""
    a_header, a_source = ros2_adapter.render_adapter(struct, ros_name, tickle_header)
    (outdir / f"{ros_name}__rosidl_typesupport_tickle_c.h").write_text(
        postprocess.clang_format(
            postprocess.add_banner(a_header, msg_name_for_banner),
            style_dir=str(REPO_ROOT),
            filename=f"{ros_name}__rosidl_typesupport_tickle_c.h",
        ),
        encoding="utf-8",
    )
    (outdir / f"{ros_name}__rosidl_typesupport_tickle_c.c").write_text(
        postprocess.clang_format(
            postprocess.add_banner(a_source, msg_name_for_banner),
            style_dir=str(REPO_ROOT),
            filename=f"{ros_name}__rosidl_typesupport_tickle_c.c",
        ),
        encoding="utf-8",
    )


def _generate_adapter(outdir, fixture_name, ros_name, *, ros_pkg_name="test_msgs", msg_dir=None, resolver=None):
    """Generates just the ros2_adapter converter for one <msg_dir>/<fixture_name>.msg (default
    tests/fixtures_own/), into outdir - TickLE's own codec (<fixture_name>.c/.h) comes from
    conftest.py's own shared, session-scoped `generated_dir` fixture instead of being generated a
    second time here (that fixture already includes both Arrays.msg and BoundedString.msg - see
    GENERATED_INTERFACES). Deliberately avoids calling render.render_topic() (which uses empy -
    see render.py) a second time independently of that shared fixture: empy's Interpreter keeps
    *global* state (Interpreter._wasProxyInstalled) that pytest's own stdout capturing can
    conflict with across unrelated test modules ("interpreter stdout proxy lost") if two
    independent call sites create their own em.expand() calls rather than sharing one generation
    pass - this only needs ros2_adapter.render_adapter(), which is plain Python string assembly
    with no empy involved at all. `resolver` is needed whenever `fixture_name`'s own .msg nests
    another type that this same call doesn't *also* need its own separate adapter file for (None,
    the default, is fine for every leaf fixture that doesn't nest anything) - see
    ros2_adapter_check_binary()'s own comment for the case that needs more than this handles."""
    text = ((msg_dir or FIXTURES_OWN) / f"{fixture_name}.msg").read_text(encoding="utf-8")
    spec = rosidl.parse_message_string(ros_pkg_name, fixture_name, text)
    ir = adapt.adapt_message(fixture_name, spec, resolver)
    layout.compute(ir.data)
    _write_adapter_files(outdir, ir.data, ros_name, f"{fixture_name}.h", f"{fixture_name}.msg")


@pytest.fixture(scope="module")
def ros2_adapter_check_binary(tmp_path_factory, generated_dir):
    """Generates just the ros2_adapter converter for both fixtures (TickLE's own codec comes from
    conftest.py's shared `generated_dir` fixture - see _generate_adapter()'s own comment on why),
    compiles everything together with fixtures_ros2_adapter/'s fake rosidl headers and a small C
    test harness, and returns the resulting executable's path."""
    outdir = tmp_path_factory.mktemp("ros2_adapter")
    _generate_adapter(outdir, "Arrays", "test_msgs__msg__Arrays")
    _generate_adapter(outdir, "BoundedString", "test_msgs__msg__BoundedString")
    _generate_adapter(outdir, "StringArrays", "test_msgs__msg__StringArrays")
    # NestedArrays.msg nests OddAlign (a sibling .msg in the same real-ROS2-package fixture,
    # nested_arrays_pkg/). Deliberately resolve.Resolver here, NOT Ros2Resolver: conftest.py's own
    # generated_dir fixture generates NestedArrays.msg's TickLE codec via plain
    # cli.generate_interface() (GENERATED_INTERFACES/INCLUDE_DIRS), which itself resolves OddAlign
    # via resolve.Resolver - its own "pkg__Name" c_name convention (confirmed against a real run:
    # "nested_arrays_pkg__OddAlign", not "OddAlignData") must match exactly, or the two independent
    # adapt_message() calls below would produce a WireField.nested pointing at a *different*,
    # differently-c_name'd struct than the one generated_dir's own NestedArrays.c/OddAlign.c
    # actually declares - a real mismatch this fixture's own single-shared-resolver design avoids
    # by construction: resolving NestedArrays.msg through this resolver populates the *same*
    # resolver.resolved_structs entry render_adapter() for OddAlign itself (below) also reuses,
    # rather than each independently re-adapting OddAlign.msg with its own default c_name.
    nested_arrays_resolver = resolve.Resolver([FIXTURES_OWN])
    nested_arrays_text = (NESTED_ARRAYS_MSG_DIR / "NestedArrays.msg").read_text(encoding="utf-8")
    nested_arrays_spec = rosidl.parse_message_string("nested_arrays_pkg", "NestedArrays", nested_arrays_text)
    nested_arrays_ir = adapt.adapt_message("NestedArrays", nested_arrays_spec, nested_arrays_resolver)
    layout.compute(nested_arrays_ir.data)
    odd_align_struct = nested_arrays_resolver.resolved_structs[("nested_arrays_pkg", "OddAlign")]
    # OddAlign's own adapter must be generated too, separately - NestedArrays's own generated .c
    # #includes and calls it, matching a real ROS 2 package's own per-.msg CMake generation.
    _write_adapter_files(
        outdir, odd_align_struct, "nested_arrays_pkg__msg__OddAlign", "nested_arrays_pkg__OddAlign.h", "OddAlign.msg"
    )
    _write_adapter_files(
        outdir, nested_arrays_ir.data, "nested_arrays_pkg__msg__NestedArrays", "NestedArrays.h", "NestedArrays.msg"
    )

    main_c = outdir / "main.c"
    main_c.write_text(_MAIN_C, encoding="utf-8")

    binary = outdir / "roundtrip_check"
    subprocess.run(
        [
            CC,
            *CFLAGS,
            f"-I{outdir}",
            f"-I{generated_dir}",
            f"-I{FIXTURES_ROS2_ADAPTER}",
            "-o",
            str(binary),
            str(main_c),
            str(generated_dir / "Arrays.c"),
            str(generated_dir / "BoundedString.c"),
            str(generated_dir / "StringArrays.c"),
            str(generated_dir / "NestedArrays.c"),
            str(generated_dir / "nested_arrays_pkg__OddAlign.c"),
            str(outdir / "test_msgs__msg__Arrays__rosidl_typesupport_tickle_c.c"),
            str(outdir / "test_msgs__msg__BoundedString__rosidl_typesupport_tickle_c.c"),
            str(outdir / "test_msgs__msg__StringArrays__rosidl_typesupport_tickle_c.c"),
            str(outdir / "nested_arrays_pkg__msg__OddAlign__rosidl_typesupport_tickle_c.c"),
            str(outdir / "nested_arrays_pkg__msg__NestedArrays__rosidl_typesupport_tickle_c.c"),
            str(REPO_ROOT / "src" / "encoding.c"),
            str(REPO_ROOT / "src" / "log.c"),
            "-lm",
        ],
        check=True,
    )
    return binary


def test_ros2_adapter_roundtrip(ros2_adapter_check_binary):
    result = subprocess.run([str(ros2_adapter_check_binary)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ALL PASS" in result.stdout
    assert "test_arrays_roundtrip: PASS" in result.stdout
    assert "test_arrays_rejects_over_capacity: PASS" in result.stdout
    assert "test_bounded_string_roundtrip: PASS" in result.stdout
    assert "test_bounded_string_rejects_over_capacity: PASS" in result.stdout
    assert "test_string_arrays_roundtrip: PASS" in result.stdout
    assert "test_string_arrays_rejects_over_capacity: PASS" in result.stdout
    assert "test_nested_arrays_roundtrip: PASS" in result.stdout
    assert "test_nested_arrays_rejects_over_capacity: PASS" in result.stdout
