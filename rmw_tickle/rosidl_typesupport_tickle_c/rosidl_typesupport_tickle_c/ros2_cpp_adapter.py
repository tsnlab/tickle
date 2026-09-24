# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""The C++ counterpart of ros2_adapter.render_adapter(): converters between a ROS 2 *C++* message
(rosidl_generator_cpp - std::string, std::vector, std::array) and the same TickLE struct the C
adapter fills.

Why they are needed (2026-09-25). rclcpp hands rmw_tickle C++ message objects, and rosidl_
typesupport_tickle_cpp used to lend it the C converters, which read those objects as if they were
rosidl_generator_c structs. A std::string happens to begin like rosidl_runtime_c__String - a
pointer, then a length - so strings and flat types worked, which is how it went unnoticed. A
std::vector does not: it is (begin, end, capacity), so the C code read the end *pointer* as the
element count, and any message with a sequence failed to publish (a default rclcpp::Node's own
/parameter_events among them). The receive side wrote C allocations into C++ objects, which is heap
corruption rather than an error.

Generated from the same model, in the same run, as the C adapter, so the two can never disagree on
the TickLE struct's layout or capacities. Each package gets its converters as overloads in
`<pkg>::<subfolder>::rosidl_typesupport_tickle_cpp`, so a nested field - from this package or
another - is converted by calling `to_tickle`/`from_tickle` on it, in its own type's namespace.

The TickLE side is passed as `void*` / `const void*`: the struct's C name is generator-internal,
and the type support wrapper (rosidl_typesupport_tickle_cpp's msg__type_support.cpp.in) only knows
the ROS names. Element-wise loops rather than memcpy() for sequences: std::vector<bool> has no
data(), and rosidl_runtime_cpp::BoundedVector is not guaranteed to have it either.
"""

from .ros2_adapter import ros2_header_path


def cpp_type_name(ros_name):
    """`pkg__srv__Type_Request` -> `::pkg::srv::Type_Request`."""
    pkg, subfolder, type_name = ros_name.split("__")
    return f"::{pkg}::{subfolder}::{type_name}"


def cpp_namespace(ros_name):
    pkg, subfolder, _type_name = ros_name.split("__")
    return f"{pkg}::{subfolder}::rosidl_typesupport_tickle_cpp"


def _nested_call(f, direction, ros_expr, tickle_expr):
    pkg, type_name = f.nested.origin
    namespace = f"::{pkg}::msg::rosidl_typesupport_tickle_cpp"
    if direction == "to":
        return f"if (!{namespace}::to_tickle({ros_expr}, {tickle_expr})) {{ return false; }}"
    return f"if (!{namespace}::from_tickle({tickle_expr}, {ros_expr})) {{ return false; }}"


def _mutable(expr):
    # The TickLE struct's plain strings are `char*`: aliased to the ROS message for the length of
    # one publish, never written through (see the C adapter's identical aliasing).
    return f"const_cast<char*>({expr}.c_str())"


def _to_tickle_field_lines(f):
    ros = f"ros.{f.name}"
    tickle = f"tickle->{f.name}"
    if f.kind == "scalar":
        return [f"{tickle} = {ros};"]
    if f.kind == "string" and f.capacity is None:
        return [f"{tickle} = {_mutable(ros)};"]
    if f.kind == "string":
        return [
            f"if ({ros}.size() > {f.capacity}) {{ return false; }}",
            f"memcpy({tickle}, {ros}.data(), {ros}.size());",
            f"{tickle}[{ros}.size()] = '\\0';",
        ]
    if f.kind == "array" and f.array_mode == "fixed":
        loop = [f"for (size_t i = 0; i < {f.array_size}; i++) {{"]
        if f.array_element_kind == "string":
            body = f"    {tickle}[i] = {_mutable(f'{ros}[i]')};"
        elif f.array_element_kind == "nested":
            body = "    " + _nested_call(f, "to", f"{ros}[i]", f"&{tickle}[i]")
        else:
            body = f"    {tickle}[i] = {ros}[i];"
        return loop + [body, "}"]
    if f.kind == "array":
        lines = [
            f"if ({ros}.size() > {f.capacity}) {{ return false; }}",
            f"for (size_t i = 0; i < {ros}.size(); i++) {{",
        ]
        if f.array_element_kind == "string":
            lines.append(f"    {tickle}[i] = {_mutable(f'{ros}[i]')};")
        elif f.array_element_kind == "nested":
            lines.append("    " + _nested_call(f, "to", f"{ros}[i]", f"&{tickle}[i]"))
        else:
            lines.append(f"    {tickle}[i] = {ros}[i];")
        return lines + ["}", f"{tickle}_count = static_cast<uint16_t>({ros}.size());"]
    if f.kind == "nested":
        return [_nested_call(f, "to", ros, f"&{tickle}")]
    raise NotImplementedError(f.kind)


def _string_from(expr):
    # A decoded plain string can be NULL (an absent value on the wire); the ROS side has no NULL.
    return f"({expr} != nullptr ? {expr} : \"\")"


def _from_tickle_field_lines(f):
    ros = f"ros.{f.name}"
    tickle = f"tickle->{f.name}"
    if f.kind == "scalar":
        return [f"{ros} = {tickle};"]
    if f.kind == "string":
        return [f"{ros} = {_string_from(tickle)};"]
    if f.kind == "array" and f.array_mode == "fixed":
        loop = [f"for (size_t i = 0; i < {f.array_size}; i++) {{"]
        if f.array_element_kind == "string":
            body = f"    {ros}[i] = {_string_from(f'{tickle}[i]')};"
        elif f.array_element_kind == "nested":
            body = "    " + _nested_call(f, "from", f"{ros}[i]", f"&{tickle}[i]")
        else:
            body = f"    {ros}[i] = {tickle}[i];"
        return loop + [body, "}"]
    if f.kind == "array":
        lines = [f"{ros}.resize({tickle}_count);", f"for (size_t i = 0; i < {tickle}_count; i++) {{"]
        if f.array_element_kind == "string":
            lines.append(f"    {ros}[i] = {_string_from(f'{tickle}[i]')};")
        elif f.array_element_kind == "nested":
            lines.append("    " + _nested_call(f, "from", f"{ros}[i]", f"&{tickle}[i]"))
        else:
            lines.append(f"    {ros}[i] = {tickle}[i];")
        return lines + ["}"]
    if f.kind == "nested":
        return [_nested_call(f, "from", ros, f"&{tickle}")]
    raise NotImplementedError(f.kind)


def _nested_cpp_adapter_headers(struct):
    names = set()
    for f in struct.fields:
        if f.kind == "nested" or (f.kind == "array" and f.array_element_kind == "nested"):
            pkg, type_name = f.nested.origin
            names.add(f"{pkg}__msg__{type_name}__rosidl_typesupport_tickle_cpp.hpp")
    return sorted(names)


def render_cpp_adapter(struct, ros_name, tickle_header):
    """(header_text, source_text) for <ros_name>__rosidl_typesupport_tickle_cpp.{hpp,cpp}."""
    msg_type = cpp_type_name(ros_name)
    namespace = cpp_namespace(ros_name)
    header_lines = [
        "#pragma once",
        "",
        f'#include "{ros2_header_path(ros_name)}pp"',
        "",
        f"namespace {namespace} {{",
        "",
        f"// false when a sequence or string is longer than the capacity {struct.c_name} was generated with.",
        f"bool to_tickle(const {msg_type}& ros, void* tickle_struct);",
        "// false only if allocating the ROS side fails.",
        f"bool from_tickle(const void* tickle_struct, {msg_type}& ros);",
        "",
        f"}}  // namespace {namespace}",
    ]
    has_ros_fields = bool(struct.fields)
    source_lines = [
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <cstring>",
        "",
        f'#include "{ros_name}__rosidl_typesupport_tickle_cpp.hpp"',
        "",
        "// The TickLE struct header is C11: its layout checks are _Static_assert, which C++ spells",
        "// static_assert. Mapped, not switched off - they are what proves this side and the C side",
        "// see the same struct.",
        "#ifndef _Static_assert",
        "#define _Static_assert static_assert",
        "#define TICKLE_CPP_MAPPED_STATIC_ASSERT",
        "#endif",
        f'#include "{tickle_header}"',
        "#ifdef TICKLE_CPP_MAPPED_STATIC_ASSERT",
        "#undef _Static_assert",
        "#undef TICKLE_CPP_MAPPED_STATIC_ASSERT",
        "#endif",
    ]
    source_lines += [f'#include "{h}"' for h in _nested_cpp_adapter_headers(struct)]
    source_lines += [
        "",
        f"namespace {namespace} {{",
        "",
        f"bool to_tickle(const {msg_type}& ros, void* tickle_struct) {{",
        f"    auto* tickle = static_cast<struct {struct.c_name}*>(tickle_struct);",
    ]
    if not has_ros_fields:
        source_lines += ["    (void)ros;", "    (void)tickle;"]
    for f in struct.fields:
        source_lines += [f"    {line}" for line in _to_tickle_field_lines(f)]
    source_lines += [
        "    return true;",
        "}",
        "",
        f"bool from_tickle(const void* tickle_struct, {msg_type}& ros) {{",
        f"    const auto* tickle = static_cast<const struct {struct.c_name}*>(tickle_struct);",
    ]
    if not has_ros_fields:
        source_lines += ["    (void)ros;", "    (void)tickle;"]
    for f in struct.fields:
        source_lines += [f"    {line}" for line in _from_tickle_field_lines(f)]
    source_lines += [
        "    return true;",
        "}",
        "",
        f"}}  // namespace {namespace}",
    ]
    return "\n".join(header_lines) + "\n", "\n".join(source_lines) + "\n"
