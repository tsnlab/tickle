# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""rosidl_typesupport_tickle_c's own generation step (Milestone 1, rmw_tickle/PLAN.md) - NOT part
of the normal `tickle-typesupport` codegen path (cli.generate_interface()) at all. Where that path
generates TickLE's own bespoke struct + codec (<Name>.c/.h, used directly by examples/), this one
instead generates a *converter* between a real ROS 2 interface package's own `rosidl_generator_c`
struct (e.g. `struct std_msgs__msg__String`) and TickLE's already-generated, already-tested
struct/codec for that same message - so the actual CDR-4 wire logic in emit.py is reused
completely unchanged; only the "copy fields between two different C structs" part is new here.

Field type mapping this assumes (rosidl_runtime_c's own public, stable headers - not the rosidl/
ament_cmake *build-system* internals, which this module has nothing to do with):
  - a scalar field: the same C type on both sides (bool/uint8_t/int32_t/float/double/...) -
    direct assignment.
  - string (unbounded): `struct rosidl_runtime_c__String { char* data; size_t size; size_t
    capacity; }` (rosidl_runtime_c/string.h) - `.data` is always NUL-terminated at `.size`, so
    ros->tickle aliases it directly (zero copy, matching TickLE's own char* semantics);
    tickle->ros allocates via `rosidl_runtime_c__String__assign()`.
  - string (bounded, `string<=N` or `@capacity`): same rosidl_runtime_c__String on the ROS 2 side
    (rosidl has no fixed-capacity string type of its own - the bound is enforced here, not by its
    struct layout) - ros->tickle bounds-checks then memcpy()s into TickLE's fixed char[N+1]
    buffer; tickle->ros still just calls rosidl_runtime_c__String__assign().
  - fixed array `T[N]`: a plain C array both sides - direct memcpy(), sizes always match.
  - variable/bounded array `T[]`/`T[<=N]`: `struct rosidl_runtime_c__<T>__Sequence { T* data;
    size_t size; size_t capacity; }` (rosidl_runtime_c/primitives_sequence.h, one per primitive
    type) - ros->tickle bounds-checks then memcpy()s into TickLE's fixed buffer; tickle->ros
    allocates via `rosidl_runtime_c__<T>__Sequence__init()`. NOTE: the exact Sequence type name
    for the legacy `byte`/`char` IDL types specifically hasn't been verified against a real ROS 2
    install (no ROS 2 available in this tool's own dev/test environment - see PLAN.md) - every
    other primitive type's mapping (bool and every fixed-width int/float type) is the stable,
    long-documented rosidl_runtime_c convention.
  - nested message: recurses into that nested type's own <Ros2Name>__to_tickle/__from_tickle,
    named the same way (see ros2_nested_struct_name()).
"""

import re

# rosidl_generator_c's own struct-naming convention: package `foo_msgs`, message `Bar` (from
# `msg/Bar.msg`) -> `struct foo_msgs__msg__Bar`. TickLE's *own* nested-type naming (PLAN.md:
# "Nested pkg/Bar -> struct pkg__Bar") only keeps the package+type, not which subfolder ("msg" is
# the only one that can appear nested - ROS 2 doesn't nest .srv types) - so a nested field's ROS 2
# name is recovered by splitting TickLE's own c_name on its *first* "__" and reinserting "msg"
# in between. Relies on a ROS 2 package name never itself containing "__", which the ROS 2 naming
# guidelines already require (package names are a single lower_snake_case token).
def ros2_nested_struct_name(tickle_nested_c_name):
    pkg, _, type_name = tickle_nested_c_name.partition("__")
    return f"{pkg}__msg__{type_name}"


def ros2_struct_name(ros_pkg, ros_subfolder, ros_type_name):
    return f"{ros_pkg}__{ros_subfolder}__{ros_type_name}"


# rosidl_generator_c's own generated *header path* uses snake_case for the type-name segment
# (package and "msg"/"srv" are already snake_case) even though the C struct/type name itself
# keeps the original mixed case - e.g. std_msgs/UInt64.msg is `struct std_msgs__msg__UInt64` but
# `#include "std_msgs/msg/u_int64.h"`. Standard camelCase/PascalCase -> snake_case regex (splits
# before an uppercase-started word, and between a lowercase/digit and a following uppercase) -
# verified against that exact UInt64 -> u_int64 case, which is the one real ROS 2 example this
# tool's own dev environment (no ROS 2 install - see this module's own docstring) could check by
# hand rather than against a live rosidl.
def _camel_to_snake(name):
    step1 = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    step2 = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", step1)
    return step2.lower()


def ros2_header_path(ros_name):
    """`pkg__msg__Type` -> `pkg/msg/type.h` (the header rosidl_generator_c itself would emit)."""
    pkg, subfolder, type_name = ros_name.split("__")
    return f"{pkg}/{subfolder}/{_camel_to_snake(type_name)}.h"


def _to_tickle_field_lines(f):
    if f.kind == "scalar":
        return [f"tickle->{f.name} = ros->{f.name};"]
    if f.kind == "string" and f.capacity is None:
        return [f"tickle->{f.name} = ros->{f.name}.data;"]
    if f.kind == "string":
        return [
            f"if (ros->{f.name}.size > {f.capacity}) {{ return false; }}",
            f"memcpy(tickle->{f.name}, ros->{f.name}.data, ros->{f.name}.size);",
            f"tickle->{f.name}[ros->{f.name}.size] = '\\0';",
        ]
    if f.kind == "array" and f.array_mode == "fixed":
        return [f"memcpy(tickle->{f.name}, ros->{f.name}, sizeof(tickle->{f.name}));"]
    if f.kind == "array":
        return [
            f"if (ros->{f.name}.size > {f.capacity}) {{ return false; }}",
            f"memcpy(tickle->{f.name}, ros->{f.name}.data, ros->{f.name}.size * sizeof(*tickle->{f.name}));",
            f"tickle->{f.name}_count = (uint16_t)ros->{f.name}.size;",
        ]
    if f.kind == "nested":
        nested_ros_name = ros2_nested_struct_name(f.nested.c_name)
        return [
            f"if (!{nested_ros_name}__to_tickle(&ros->{f.name}, &tickle->{f.name})) {{ return false; }}",
        ]
    raise NotImplementedError(f.kind)


def _from_tickle_field_lines(f):
    if f.kind == "scalar":
        return [f"ros->{f.name} = tickle->{f.name};"]
    if f.kind == "string":
        # Bounded or not, the ROS 2 side is always a real owned rosidl_runtime_c__String -
        # __assign() allocates+copies, matching rosidl's own ownership contract (a decoded
        # message it hands to application code must own its own memory, unlike TickLE's own
        # decode(), which aliases the rx buffer - see DESIGN.md's "Strings" rule).
        return [f"if (!rosidl_runtime_c__String__assign(&ros->{f.name}, tickle->{f.name})) {{ return false; }}"]
    if f.kind == "array" and f.array_mode == "fixed":
        return [f"memcpy(ros->{f.name}, tickle->{f.name}, sizeof(ros->{f.name}));"]
    if f.kind == "array":
        count_var = f"tickle->{f.name}_count"
        return [
            f"if (!rosidl_runtime_c__{f.scalar_type}__Sequence__init(&ros->{f.name}, {count_var})) {{ return false; }}",
            f"memcpy(ros->{f.name}.data, tickle->{f.name}, (size_t){count_var} * sizeof(*tickle->{f.name}));",
        ]
    if f.kind == "nested":
        nested_ros_name = ros2_nested_struct_name(f.nested.c_name)
        return [
            f"if (!{nested_ros_name}__from_tickle(&tickle->{f.name}, &ros->{f.name})) {{ return false; }}",
        ]
    raise NotImplementedError(f.kind)


def emit_to_tickle(struct, ros_name):
    """`<ros_name>__to_tickle(const struct <ros_name>*, struct <struct.c_name>*) -> bool` - false
    on a bounds check failure (a variable array/bounded string longer than TickLE's resolved
    capacity), matching the existing codec's own `-2` capacity-rejection contract one level up.
    Not `static` - the typesupport wrapper this same package generates (Milestone 1's next piece)
    calls this from a different translation unit."""
    lines = [f"bool {ros_name}__to_tickle(const struct {ros_name}* ros, struct {struct.c_name}* tickle) {{"]
    for f in struct.fields:
        lines += [f"    {line}" for line in _to_tickle_field_lines(f)]
    lines += ["    return true;", "}"]
    return lines


def emit_from_tickle(struct, ros_name):
    """The decode-side mirror of emit_to_tickle() - false only if a rosidl_runtime_c allocation
    fails (bad_alloc-equivalent), since a value already inside TickLE's own resolved-capacity
    buffers can never overflow the ROS 2 side (rosidl_runtime_c__*__Sequence/String are all
    dynamically sized). Also not `static`, for the same reason as emit_to_tickle()."""
    lines = [f"bool {ros_name}__from_tickle(const struct {struct.c_name}* tickle, struct {ros_name}* ros) {{"]
    for f in struct.fields:
        lines += [f"    {line}" for line in _from_tickle_field_lines(f)]
    lines += ["    return true;", "}"]
    return lines


def nested_ros_includes(struct):
    """The ROS 2-generated header for each of this struct's own *directly* nested fields - same
    "only one level, each nested header pulls in what it itself needs" reasoning as render.py's
    own _nested_includes()."""
    names = sorted({ros2_nested_struct_name(f.nested.c_name) for f in struct.fields if f.kind == "nested"})
    return [ros2_header_path(name) for name in names]


def needs_rosidl_string(struct):
    return any(f.kind == "string" for f in struct.fields)


def needs_rosidl_sequence(struct):
    return any(f.kind == "array" and f.array_mode == "variable" for f in struct.fields)


def sequence_element_types(struct):
    """Distinct rosidl_runtime_c primitive Sequence element types this struct's own (not nested
    structs') variable arrays need - each is its own header,
    rosidl_runtime_c/<type>__functions.h."""
    return sorted({f.scalar_type for f in struct.fields if f.kind == "array" and f.array_mode == "variable"})


def render_adapter(struct, ros_name, tickle_header):
    """Returns (header_text, source_text) for <ros_name>__rosidl_typesupport_tickle_c.{h,c} -
    plain string assembly (not empy) since this is a fixed two-function shape, not a per-kind
    template the way struct.h.em/struct.c.em are. `tickle_header` is the *interface's* own
    generated header filename (e.g. "Arrays.h" - cli.generate_interface()'s own `<name>.h`,
    keyed off the .msg/.srv's interface name) - NOT `f"{struct.c_name}.h"`: struct.c_name is
    "ArraysData", but that struct is declared *inside* Arrays.h, not its own same-named file (a
    .srv's request/response structs share one file the same way)."""
    header_lines = [
        "#pragma once",
        "",
        "#include <stdbool.h>",
        "",
        f'#include "{ros2_header_path(ros_name)}"',
        f'#include "{tickle_header}"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        f"bool {ros_name}__to_tickle(const struct {ros_name}* ros, struct {struct.c_name}* tickle);",
        f"bool {ros_name}__from_tickle(const struct {struct.c_name}* tickle, struct {ros_name}* ros);",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
    ]

    source_includes = ["#include <stdbool.h>"]
    if needs_rosidl_sequence(struct):
        # Only a variable array's own `(uint16_t)ros->*.size` cast (see _to_tickle_field_lines)
        # actually names a stdint.h type in this file - a plain scalar/fixed-array/string field's
        # generated lines never do (the type itself is only ever named in the *struct*
        # declarations, which live in the headers this file includes, not here).
        source_includes.append("#include <stdint.h>")
    source_includes += [
        "#include <string.h>",
        "",
        f'#include "{ros_name}__rosidl_typesupport_tickle_c.h"',
        "",
        # Redundant with the paired header above transitively pulling both of these in - included
        # directly anyway, matching this generator's own misc-include-cleaner convention
        # elsewhere (every file includes what it directly uses, not just what happens to arrive
        # transitively).
        f'#include "{ros2_header_path(ros_name)}"',
        f'#include "{tickle_header}"',
    ]
    if needs_rosidl_string(struct):
        source_includes.append('#include "rosidl_runtime_c/string_functions.h"')
    if sequence_element_types(struct):
        # One header for every rosidl_runtime_c__<T>__Sequence type, regardless of how many
        # distinct primitive element types this struct's own variable arrays actually use -
        # rosidl_runtime_c/primitives_sequence_functions.h declares all of them together, it
        # isn't split per element type.
        source_includes.append('#include "rosidl_runtime_c/primitives_sequence_functions.h"')

    source_lines = source_includes + [""] + emit_to_tickle(struct, ros_name) + [""] + emit_from_tickle(
        struct, ros_name
    )

    header = "\n".join(header_lines) + "\n"
    source = "\n".join(source_lines) + "\n"
    return header, source
