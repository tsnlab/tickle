# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Shared fixtures for the codegen tests: generate TickLE's own .msg/.srv examples, compile them
(with the project's real flags - see platform/linux/Makefile - not some stricter standard the
generator doesn't actually have to satisfy) alongside src/encoding.c + src/log.c into one shared
library, and hand back a ctypes handle test_roundtrip.py / test_crossendian.py can call straight
into. test_golden.py / test_lint.py regenerate separately - they care about the generated text
itself, not about running it.
"""

import ctypes
import pathlib
import subprocess

import pytest

from tickle_typesupport import cli

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
EXAMPLES = REPO_ROOT / "examples"
FIXTURES_OWN = pathlib.Path(__file__).parent / "fixtures_own"
CC = "cc"
CFLAGS = ["-Wall", "-Wextra", "-fPIC", f"-I{REPO_ROOT / 'include'}", f"-I{REPO_ROOT / 'src'}"]

# The interfaces every codegen test exercises, and where each one's .msg/.srv source lives:
#   - UInt64/SetBool/PingPong/Bulk: TickLE's own real example interfaces - flattened into
#     examples/<proto>/ alongside their generated .c/.h (M5 - see tools/typesupport/PLAN.md and
#     examples/linux/{uint64,set_bool,ping_pong,perf}/'s own drivers, which build against the
#     copies actually committed there, not these regenerated-into-a-tmp-dir ones). Between them:
#     an all-scalar fixed message, a request/response pair with a string + a scalar-then-string
#     default padding gap, an all-scalar fixed request/response pair, and (Bulk, M2) a single
#     unbounded array with an auto-derived capacity (see examples/perf/Bulk.msg's own comment on
#     why it isn't wire-compatible with the hand-written codec it replaced).
#   - Trigger: tests/fixtures_own/ - not a real TickLE interface (nothing builds against it),
#     exists purely for the empty-message edge case (Trigger.srv's request has zero fields).
#   - Arrays/ArrayDefaults: tests/fixtures_own/ - every other array shape (fixed, bounded,
#     annotated-capacity, float element) and, separately, M6's array default values.
#   - Stamped/Image (fixtures_own/, M3): nested messages - Stamped nests std_msgs/Header (itself
#     nesting builtin_interfaces/Time) purely via tickle_typesupport.builtins, two levels deep,
#     with no -I needed at all; Image is the same std_msgs/Header nest plus a real ROS 2 shape
#     (sensor_msgs/Image, with an explicit @capacity added to its own unbounded uint8[] - see its
#     own file for why that's a fixtures_own/ copy rather than tests/fixtures_ros2/'s unmodified
#     one).
#   - Twist (fixtures_ros2/geometry_msgs/, M3): a real ROS 2 message exercising the *other* nested
#     resolution path - an explicit `-I` search path (INCLUDE_DIRS, below) rather than a builtin -
#     and the same nested type referenced twice (linear/angular, both Vector3), proving the
#     resolver caches rather than re-adapting (and re-emitting) it twice.
FIXTURES_ROS2 = pathlib.Path(__file__).parent / "fixtures_ros2"
GENERATED_INTERFACES = {
    "UInt64.msg": EXAMPLES / "uint64",
    "SetBool.srv": EXAMPLES / "set_bool",
    "PingPong.srv": EXAMPLES / "ping_pong",
    "Bulk.msg": EXAMPLES / "perf",
    "Trigger.srv": EXAMPLES,
    "Arrays.msg": FIXTURES_OWN,
    "ArrayDefaults.msg": FIXTURES_OWN,
    "Stamped.msg": FIXTURES_OWN,
    "Image.msg": FIXTURES_OWN,
    "Twist.msg": FIXTURES_ROS2 / "geometry_msgs" / "msg",
}
# -I search paths generate_interface() needs for the interfaces above that nest a nonstandard
# type not covered by tickle_typesupport.builtins - keyed the same way as GENERATED_INTERFACES.
# Stamped.msg/Image.msg both only nest std_msgs/Header (a builtin), so neither needs one.
INCLUDE_DIRS = {
    "Twist.msg": [FIXTURES_ROS2],  # nests geometry_msgs/Vector3, found under fixtures_ros2/
}


def _compile_to_object(path, outdir):
    obj = outdir / (path.stem + ".o")
    subprocess.run([CC, *CFLAGS, "-c", "-o", str(obj), str(path)], check=True)
    return obj


@pytest.fixture(scope="session")
def generated_dir(tmp_path_factory):
    """Generates every interface in GENERATED_INTERFACES into one temp directory and returns it -
    used both to compile (this file) and to diff against tests/golden/ (test_golden.py). A nested
    dependency (std_msgs__Header.c, say) that more than one interface here needs is regenerated
    once per interface that references it, into this same shared directory - harmless, since
    generate_interface() is deterministic per (pkg, name): the file just gets overwritten with
    identical bytes, so only one copy of it actually exists (and gets compiled) by the time every
    interface above has been generated."""
    outdir = tmp_path_factory.mktemp("generated")
    for name, source_dir in GENERATED_INTERFACES.items():
        cli.generate_interface(
            str(source_dir / name), str(outdir), style_dir=str(REPO_ROOT), include_dirs=INCLUDE_DIRS.get(name, [])
        )
    return outdir


@pytest.fixture(scope="session")
def generated_lib(generated_dir, tmp_path_factory):
    """Compiles every generated .c (+ src/encoding.c, src/log.c - the only two the generated
    codecs themselves depend on, matching how the real build links them - see
    platform/linux/Makefile) into one shared library and loads it via ctypes."""
    build_dir = tmp_path_factory.mktemp("build")
    objects = [_compile_to_object(p, build_dir) for p in sorted(generated_dir.glob("*.c"))]
    objects += [
        _compile_to_object(REPO_ROOT / "src" / "encoding.c", build_dir),
        _compile_to_object(REPO_ROOT / "src" / "log.c", build_dir),
    ]
    lib_path = build_dir / "libgenerated.so"
    subprocess.run([CC, "-shared", "-o", str(lib_path), *[str(o) for o in objects]], check=True)
    return ctypes.CDLL(str(lib_path))
