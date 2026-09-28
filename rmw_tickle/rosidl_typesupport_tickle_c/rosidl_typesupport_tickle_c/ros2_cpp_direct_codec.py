# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""The C++ direct codec: a ROS 2 C++ message straight to TickLE's wire bytes, and back.

The C counterpart is ros2_direct_codec.py, and these bytes must be exactly those - a message is
routinely published by a C++ node and taken by a C one. So everything that decides the *layout* -
the padding, the field order, a string's uint16 length including its NUL, a sequence's uint16
count, the bound checks, the byte swap - is imported from that module rather than written again
here. Those helpers emit plain C statements, which are valid C++, so sharing them is not a
convenience: it is what stops two hand-written encoders of one wire format drifting apart, which
rmw_tickle/LARGE_MESSAGE_PLAN.md names as this stage's own first risk.

What differs is only how a field is reached: `ros.field`, `.size()`, `.data()`, `c_str()`,
`resize()`, `assign()`. std::vector<bool> keeps an element loop, since it has no data() - the same
exception ros2_cpp_adapter.py already makes.

The mutants of the pass-1 harness (TICKLE_DIRECT_CODEC_MUTANT) reach this codec too, because they
live in the shared helpers.
"""

from . import ros2_direct_codec as c_codec


def _nested_namespace(field):
    pkg, subfolder, _type_name = field.nested.origin
    return f"::{pkg}::{subfolder}::rosidl_typesupport_tickle_cpp"


def _is_vector_bool(field):
    """std::vector<bool> - the one container with no data(), so no memcpy."""
    return field.kind == "array" and field.array_mode == "variable" and field.scalar_type == "bool"


def _braced(lines):
    return ["{"] + [f"    {ln}" for ln in lines] + ["}"]


def _loop(count_expr, body):
    return [f"for (size_t i = 0; i < static_cast<size_t>({count_expr}); i++) {{"] + [
        f"    {ln}" for ln in body
    ] + ["}"]


# --- strings ------------------------------------------------------------------------------------


def needs_string_helpers(struct):
    return any(
        f.kind == "string" or (f.kind == "array" and f.array_element_kind == "string") for f in struct.fields
    )


def emit_string_helpers():
    """Encode, size and decode one std::string, in the same wire shape the C codec's own helpers
    produce. `bound` is the IDL bound, or SIZE_MAX where there is none."""
    mutant = c_codec.mutant()
    if mutant in ("size_strlen", "cpp_string_size"):
        length = ["size_t str_len = s.size() + 1;"]
    else:
        # Up to the first NUL, which is what the wire carries and what the struct path's strnlen
        # produced from the same message.
        length = [
            "size_t const nul = s.find('\\0');",
            "size_t str_len = (nul == std::string::npos ? s.size() : nul) + 1;",
        ]
    bound_check = [] if mutant == "no_bound_check" else ["if (s.size() > bound) { return -2; }"]
    prefix = (
        "static_cast<uint16_t>(str_len + 1)" if mutant == "len_prefix_plus1" else "static_cast<uint16_t>(str_len)"
    )
    encode = (
        [
            "static int32_t direct_encode_string(const std::string& s, size_t bound, uint8_t* payload, uint32_t len,",
            "                                    int32_t encoded) {",
        ]
        + [f"    {ln}" for ln in bound_check + length]
        + [
            "    if (str_len > 65535U) { return -2; }",
            "    if (static_cast<size_t>(encoded) + 2 > len) { return -1; }",
            "    {",
            f"        uint16_t const prefix = {prefix};",
            "        memcpy(payload + encoded, &prefix, 2);",
            "    }",
            "    encoded += 2;",
            "    if (static_cast<size_t>(encoded) + str_len > len) { return -1; }",
            "    memcpy(payload + encoded, s.data(), str_len - 1);",
            "    payload[static_cast<size_t>(encoded) + str_len - 1] = 0;",
            "    encoded += static_cast<int32_t>(str_len);",
            "    {",
            f"        uint32_t const pad4 = {c_codec._align('encoded', 4)};",
            "        if (static_cast<size_t>(encoded) + pad4 > len) { return -1; }",
            "        memset(payload + encoded, 0, pad4);",
            "        encoded += static_cast<int32_t>(pad4);",
            "    }",
            "    return encoded;",
            "}",
            "",
        ]
    )
    size = (
        ["static int32_t direct_string_size(const std::string& s, size_t bound, size_t* size) {"]
        + [f"    {ln}" for ln in bound_check + length]
        + [
            "    if (str_len > 65535U) { return -2; }",
            "    *size += 2 + str_len;",
            "    *size += (0U - *size) & 3U;",
            "    return 0;",
            "}",
            "",
        ]
    )
    decode_bound = [] if mutant == "no_bound_check" else ["    if (bound != SIZE_MAX && str_len > bound + 1) { return -2; }"]
    decode = (
        [
            "static int32_t direct_decode_string(std::string& s, size_t bound, const uint8_t* payload, uint32_t len,",
            "                                    int32_t decoded, bool is_native_endian) {",
            "    if (static_cast<size_t>(decoded) + 2 > len) { return -1; }",
            "    uint16_t str_len = 0;",
            "    memcpy(&str_len, payload + decoded, 2);",
            "    if (!is_native_endian) { str_len = __builtin_bswap16(str_len); }",
            "    if (str_len == 0) { return -2; }",
        ]
        + decode_bound
        + [
            "    decoded += 2;",
            "    if (static_cast<size_t>(decoded) + str_len > len) { return -1; }",
            "    if (payload[static_cast<size_t>(decoded) + str_len - 1] != 0) { return -2; }",
            # Up to the first NUL, as the C++ adapter's own `ros = tickle->field` does.
            "    s.assign(reinterpret_cast<const char*>(payload + decoded));",
            "    decoded += str_len;",
            f"    decoded += static_cast<int32_t>({c_codec._align('decoded', 4)});",
            "    if (static_cast<uint32_t>(decoded) > len) { return -1; }",
            "    return decoded;",
            "}",
            "",
        ]
    )
    return encode + size + decode


# --- per field ----------------------------------------------------------------------------------


def _cpp_pad_encode(alignment, static_padding):
    """The shared padding, unless cpp_skip_pad is planted - the one mutant that moves the C++
    codec's layout without moving the C one."""
    if c_codec.mutant() == "cpp_skip_pad":
        return []
    return c_codec._pad_encode(alignment, static_padding)


def _field_encode(field):
    ros = f"ros.{field.name}"
    if field.kind == "scalar":
        size = c_codec._scalar_size(field.scalar_type)
        # A bool is one byte on the wire and std::vector<bool>'s element is a proxy, so take a
        # plain local either way rather than the address of the member.
        return _braced(
            [
                f"if (static_cast<size_t>(encoded) + {size} > len) {{ return -1; }}",
                f"{field.ctype} const value = {ros};",
                f"memcpy(payload + encoded, &value, {size});",
                f"encoded += {size};",
            ]
        )
    if field.kind == "string":
        return _braced(
            [
                f"encoded = direct_encode_string({ros}, {c_codec._bound_literal(c_codec.string_bound(field))}, "
                "payload, len, encoded);",
                "if (encoded < 0) { return encoded; }",
            ]
        )
    if field.kind == "nested":
        return _braced(
            [
                f"int32_t const nested_size = {_nested_namespace(field)}::direct_encode({ros}, payload + encoded, "
                "len - static_cast<uint32_t>(encoded));",
                "if (nested_size < 0) { return nested_size; }",
                "encoded += nested_size;",
            ]
        )
    fixed = field.array_mode == "fixed"
    count = str(field.array_size) if fixed else f"{ros}.size()"
    elem = f"{ros}[i]"
    head = [] if fixed else c_codec._count_encode(field, count)
    counter = count if fixed else "count"
    if field.array_element_kind == "string":
        body = [
            f"encoded = direct_encode_string({elem}, SIZE_MAX, payload, len, encoded);",
            "if (encoded < 0) { return encoded; }",
        ]
        return _braced(head + _loop(counter, body))
    if field.array_element_kind == "nested":
        pad = [] if fixed else c_codec._pad_encode(field.element_align, None)
        body = []
        if field.element_align > 1:
            body = ["if (i > 0) {"] + [f"    {ln}" for ln in c_codec._pad_encode(field.element_align, None)] + ["}"]
        body += [
            f"int32_t const nested_size = {_nested_namespace(field)}::direct_encode({elem}, payload + encoded, "
            "len - static_cast<uint32_t>(encoded));",
            "if (nested_size < 0) { return nested_size; }",
            "encoded += nested_size;",
        ]
        return _braced(head + pad + _loop(counter, body))
    size = field.element_size
    pad = [] if fixed else c_codec._pad_encode(field.element_align, None)
    bytes_check = [
        f"size_t const bytes = static_cast<size_t>({counter}) * {size};",
        "if (static_cast<size_t>(encoded) + bytes > len) { return -1; }",
    ]
    if _is_vector_bool(field):
        # std::vector<bool> has no data(): one byte per element, written from the proxy.
        copy = _loop(counter, [f"payload[static_cast<size_t>(encoded) + i] = {elem} ? 1 : 0;"])
    else:
        copy = ["if (bytes > 0) {", f"    memcpy(payload + encoded, &{ros}[0], bytes);", "}"]
    return _braced(head + pad + bytes_check + copy + ["encoded += static_cast<int32_t>(bytes);"])


def _field_decode(field):
    ros = f"ros.{field.name}"
    if field.kind == "scalar":
        size = c_codec._scalar_size(field.scalar_type)
        body = [f"if (static_cast<size_t>(decoded) + {size} > len) {{ return -1; }}"]
        if size == 1:
            body += [
                f"{field.ctype} value = {field.ctype}();",
                "memcpy(&value, payload + decoded, 1);",
                f"{ros} = value;",
            ]
        else:
            wire, swap = c_codec._SWAP[size]
            body += [
                f"{wire} raw = 0;",
                f"memcpy(&raw, payload + decoded, {size});",
                f"if (!is_native_endian) {{ raw = {swap}(raw); }}",
                f"{field.ctype} value = {field.ctype}();",
                f"memcpy(&value, &raw, {size});",
                f"{ros} = value;",
            ]
        return _braced(body + [f"decoded += {size};"])
    if field.kind == "string":
        return _braced(
            [
                f"decoded = direct_decode_string({ros}, {c_codec._bound_literal(c_codec.string_bound(field))}, "
                "payload, len, decoded, is_native_endian);",
                "if (decoded < 0) { return decoded; }",
            ]
        )
    if field.kind == "nested":
        return _braced(
            [
                f"int32_t const nested_size = {_nested_namespace(field)}::direct_decode({ros}, payload + decoded, "
                "len - static_cast<uint32_t>(decoded), is_native_endian);",
                "if (nested_size < 0) { return nested_size; }",
                "decoded += nested_size;",
            ]
        )
    fixed = field.array_mode == "fixed"
    elem = f"{ros}[i]"
    if field.array_element_kind == "string":
        body = [
            f"decoded = direct_decode_string({elem}, SIZE_MAX, payload, len, decoded, is_native_endian);",
            "if (decoded < 0) { return decoded; }",
        ]
        if fixed:
            return _braced(_loop(field.array_size, body))
        return _braced(
            c_codec._count_decode(field)
            + [
                # Every string element takes at least 4 bytes; refuse a count the rest cannot hold
                # before resizing for it.
                "if (static_cast<size_t>(decoded) + (static_cast<size_t>(count) * 4) > len) { return -1; }",
                f"{ros}.resize(count);",
            ]
            + _loop("count", body)
        )
    if field.array_element_kind == "nested":
        body = []
        if field.element_align > 1:
            body = ["if (i > 0) {"] + [f"    {ln}" for ln in c_codec._pad_decode(field.element_align, None)] + ["}"]
        body += [
            f"int32_t const nested_size = {_nested_namespace(field)}::direct_decode({elem}, payload + decoded, "
            "len - static_cast<uint32_t>(decoded), is_native_endian);",
            "if (nested_size < 0) { return nested_size; }",
            "decoded += nested_size;",
        ]
        if fixed:
            return _braced(_loop(field.array_size, body))
        return _braced(
            c_codec._count_decode(field)
            + c_codec._pad_decode(field.element_align, None)
            + [f"{ros}.resize(count);"]
            + _loop("count", body)
        )
    size = field.element_size
    if fixed:
        body = [
            f"size_t const bytes = static_cast<size_t>({field.array_size}) * {size};",
            "if (static_cast<size_t>(decoded) + bytes > len) { return -1; }",
            f"memcpy(&{ros}[0], payload + decoded, bytes);",
        ]
        return _braced(
            body
            + c_codec._swap_in_place(f"&{ros}[0]", field.array_size, size)
            + ["decoded += static_cast<int32_t>(bytes);"]
        )
    head = c_codec._count_decode(field) + c_codec._pad_decode(field.element_align, None)
    body = [
        f"size_t const bytes = static_cast<size_t>(count) * {size};",
        "if (static_cast<size_t>(decoded) + bytes > len) { return -1; }",
        f"{ros}.resize(count);",
    ]
    if _is_vector_bool(field):
        copy = _loop("count", [f"{elem} = payload[static_cast<size_t>(decoded) + i] != 0;"])
        swap = []
    else:
        # The address of the first element, and the swap that follows it, only where there is one.
        copy = (
            ["if (bytes > 0) {"]
            + [f"    {ln}" for ln in [f"memcpy(&{ros}[0], payload + decoded, bytes);"]
               + c_codec._swap_in_place(f"&{ros}[0]", "count", size)]
            + ["}"]
        )
        swap = []
    return _braced(head + body + copy + swap + ["decoded += static_cast<int32_t>(bytes);"])


def _field_size(field):
    ros = f"ros.{field.name}"
    if field.kind == "scalar":
        return [f"size += {c_codec._scalar_size(field.scalar_type)};"]
    if field.kind == "string":
        return _braced(
            [
                f"int32_t const ret = direct_string_size({ros}, "
                f"{c_codec._bound_literal(c_codec.string_bound(field))}, &size);",
                "if (ret < 0) { return ret; }",
            ]
        )
    if field.kind == "nested":
        return _braced(
            [
                f"int32_t const nested_size = {_nested_namespace(field)}::direct_encode_size({ros});",
                "if (nested_size < 0) { return nested_size; }",
                "size += static_cast<size_t>(nested_size);",
            ]
        )
    fixed = field.array_mode == "fixed"
    elem = f"{ros}[i]"
    head = [] if fixed else c_codec._count_size(field, f"{ros}.size()")
    counter = str(field.array_size) if fixed else "count"
    if field.array_element_kind == "string":
        body = [
            f"int32_t const ret = direct_string_size({elem}, SIZE_MAX, &size);",
            "if (ret < 0) { return ret; }",
        ]
        return _braced(head + _loop(counter, body) + ["if (size > INT32_MAX) { return -2; }"])
    if field.array_element_kind == "nested":
        pad = [] if fixed else c_codec._pad_size(field.element_align, None)
        body = []
        if field.element_align > 1:
            body = ["if (i > 0) {", f"    size += {c_codec._size_align(field.element_align)};", "}"]
        body += [
            f"int32_t const nested_size = {_nested_namespace(field)}::direct_encode_size({elem});",
            "if (nested_size < 0) { return nested_size; }",
            "size += static_cast<size_t>(nested_size);",
            "if (size > INT32_MAX) { return -2; }",
        ]
        return _braced(head + pad + _loop(counter, body))
    pad = [] if fixed else c_codec._pad_size(field.element_align, None)
    return _braced(head + pad + [f"size += static_cast<size_t>({counter}) * {field.element_size};"])


# --- whole functions ------------------------------------------------------------------------------


def declarations(msg_type):
    return [
        f"int32_t direct_encode_size(const {msg_type}& ros);",
        f"int32_t direct_encode(const {msg_type}& ros, uint8_t* payload, uint32_t len);",
        f"int32_t direct_decode({msg_type}& ros, const uint8_t* payload, uint32_t len, bool is_native_endian);",
    ]


def emit_functions(struct, msg_type):
    from tickle_typesupport import layout

    plans = layout.plan_fields(struct.fields)
    encode = [
        f"int32_t direct_encode(const {msg_type}& ros, uint8_t* payload, uint32_t len) {{",
        "    (void)ros;",
        "    (void)payload;",
        "    (void)len;",
        "    int32_t encoded = 0;",
    ]
    decode = [
        f"int32_t direct_decode({msg_type}& ros, const uint8_t* payload, uint32_t len, bool is_native_endian) {{",
        "    (void)ros;",
        "    (void)payload;",
        "    (void)len;",
        "    (void)is_native_endian;",
        "    int32_t decoded = 0;",
    ]
    size = [f"int32_t direct_encode_size(const {msg_type}& ros) {{", "    (void)ros;"]
    if struct.is_fixed_size:
        size.append(f"    return {struct.wire_size};")
    else:
        size.append("    size_t size = 0;")
    for plan in plans:
        field = plan.field
        encode += [f"    {ln}" for ln in _cpp_pad_encode(field.wire_align, plan.static_padding) + _field_encode(field)]
        decode += [f"    {ln}" for ln in c_codec._pad_decode(field.wire_align, plan.static_padding) + _field_decode(field)]
        if not struct.is_fixed_size:
            size += [f"    {ln}" for ln in c_codec._pad_size(field.wire_align, plan.static_padding) + _field_size(field)]
    encode += ["    return encoded;", "}"]
    decode += ["    return decoded;", "}"]
    if not struct.is_fixed_size:
        size += ["    if (size > INT32_MAX) { return -2; }", "    return static_cast<int32_t>(size);"]
    size.append("}")
    return size + [""] + encode + [""] + decode
