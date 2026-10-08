# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Which types rmw_tickle may lend (ros2_adapter.inplace_bytes_initializer(), docs/RMW.md "Loaned messages"): those
whose CDR-4 wire bytes are their ROS 2 C struct in memory. Each case is one shape the rule must decide, and the
compile check at the end has the C compiler confirm that the expression emitted for a lendable one is true - so a
generator that claimed a layout the compiler does not produce would emit 0, and the expected wire size here would
fail."""

import subprocess

from conftest import CC
from rosidl_typesupport_tickle_c import ros2_adapter
from tickle_typesupport import layout, model


def scalar(name, kind):
    return model.WireField(name=name, kind="scalar", scalar_type=kind)


def fixed_array(name, kind, size):
    return model.WireField(name=name, kind="array", scalar_type=kind, array_mode="fixed", array_size=size)


def struct(c_name, fields, origin=None):
    s = model.WireStruct(c_name=c_name, fields=fields, origin=origin)
    layout.compute(s)
    return s


def nested(name, inner):
    return model.WireField(name=name, kind="nested", nested=inner)


def nested_array(name, inner, size):
    return model.WireField(
        name=name, kind="array", array_mode="fixed", array_size=size, array_element_kind="nested", nested=inner
    )


def claim(s):
    """The wire size the initialiser claims, or 0 - read off the emitted expression."""
    text = ros2_adapter.inplace_bytes_initializer(s, "struct_under_test")
    return 0 if text == "0" else int(text.rsplit("?", 1)[1].split(":")[0])


VECTOR3 = struct("Vector3Data", [scalar("x", "float64"), scalar("y", "float64"), scalar("z", "float64")])


def test_array1k_lends():
    s = struct("Array1kData", [fixed_array("array", "byte", 1024), scalar("time", "int64"), scalar("id", "uint64")])
    assert claim(s) == 1040


def test_an_int64_after_a_uint32_does_not():
    # Wire: the int64 at 4 (CDR-4 caps alignment at 4). Memory: at 8.
    assert claim(struct("Skew", [scalar("a", "uint32"), scalar("b", "int64")])) == 0


def test_two_uint32_then_an_int64_does():
    assert claim(struct("Even", [scalar("a", "uint32"), scalar("b", "uint32"), scalar("c", "int64")])) == 16


def test_bool_does_not():
    assert claim(struct("Flag", [scalar("on", "bool"), scalar("pad", "uint8")])) == 0
    assert claim(struct("Flags", [fixed_array("on", "bool", 4)])) == 0


def test_strings_and_sequences_do_not():
    assert claim(struct("Named", [model.WireField(name="name", kind="string")])) == 0
    seq = model.WireField(name="v", kind="array", scalar_type="uint8", array_mode="variable", capacity=8)
    assert claim(struct("Seq", [seq])) == 0


def test_empty_does_not():
    assert claim(struct("Empty", [])) == 0


def test_nested_plain_lends():
    pose = struct("Accel", [nested("linear", VECTOR3), nested("angular", VECTOR3)])
    assert claim(pose) == 48


def test_nested_trailing_padding_moves_the_next_field():
    # {int64, uint8} is 9 bytes on the wire and 16 in memory, so a uint8 after it sits at 9 against 16.
    inner = struct("Tail", [scalar("a", "int64"), scalar("b", "uint8")])
    assert claim(struct("AfterTail", [nested("t", inner), scalar("c", "uint8")])) == 0


def test_a_nested_array_needs_the_same_stride():
    inner = struct("Tail", [scalar("a", "int64"), scalar("b", "uint8")])
    assert claim(struct("Tails", [nested_array("t", inner, 2)])) == 0
    assert claim(struct("Vectors", [nested_array("v", VECTOR3, 3)])) == 72


def test_the_compiler_agrees(tmp_path):
    """The emitted expression, against a C struct laid out as rosidl_generator_c lays Array1k and Accel out."""
    array1k = struct("Array1kData", [fixed_array("array", "byte", 1024), scalar("time", "int64"), scalar("id", "uint64")])
    accel = struct("Accel", [nested("linear", VECTOR3), nested("angular", VECTOR3)])
    skew = struct("Skew", [scalar("a", "uint32"), scalar("b", "int64")])
    source = "\n".join(
        [
            "#include <stddef.h>",
            "#include <stdint.h>",
            "struct a1k { uint8_t array[1024]; int64_t time; uint64_t id; };",
            "struct v3 { double x; double y; double z; };",
            "struct acc { struct v3 linear; struct v3 angular; };",
            "struct skew { uint32_t a; int64_t b; };",
            f"_Static_assert(({ros2_adapter.inplace_bytes_initializer(array1k, 'a1k')}) == 1040, \"array1k\");",
            f"_Static_assert(({ros2_adapter.inplace_bytes_initializer(accel, 'acc')}) == 48, \"accel\");",
            f"_Static_assert(({ros2_adapter.inplace_bytes_initializer(skew, 'skew')}) == 0, \"skew\");",
            "int main(void) { return 0; }",
        ]
    )
    path = tmp_path / "inplace.c"
    path.write_text(source)
    result = subprocess.run([CC, "-std=c11", "-c", str(path), "-o", str(tmp_path / "inplace.o")], capture_output=True)
    assert result.returncode == 0, result.stderr.decode()
