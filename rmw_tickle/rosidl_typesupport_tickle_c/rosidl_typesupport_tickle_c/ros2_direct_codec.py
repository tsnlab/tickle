# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""The direct codec (rmw_tickle/LARGE_MESSAGE_PLAN.md, stage 1): encode a ROS 2 C message straight
into TickLE's wire bytes, and decode those bytes straight into a ROS 2 C message, without the
fixed-capacity TickLE struct in between.

The bytes must be exactly those of `to_tickle` followed by the struct codec (emit.py), so this
follows the same rules rather than a simplified CDR:
  1. alignment is relative to the enclosing struct - a nested struct is encoded by its own
     function at `payload + encoded`, whose cursor starts at 0 - and a nested-array element pads
     to the type's self-alignment before every element but the first;
  2. a string's length is uint16 and counts the NUL; encoding stops at the first NUL, which is
     what the struct path's strnlen() does; a bounded string (string<=N) is refused above N on
     size, as to_tickle refuses it;
  3. a count is uint16; above 65,535 it is refused, not truncated;
  4. a primitive array or sequence is one memcpy (native byte order is what the struct codec's
     element-wise writes produce too);
  5. declined types stay declined (the generator never gets here for them);
  6. decode takes is_native_endian and byte-swaps.

The only limits checked are the IDL bounds, 65,535, and the buffer. A profile capacity - what the
TickLE struct can hold - is not a limit of the ROS message, so it no longer applies.

Return values follow the struct codec: >= 0 bytes, -1 buffer too short, -2 invalid or over a
bound, -3 a NULL string; decode adds -4 for a failed allocation.

Decode writes into an initialised message and may be handed one that already holds a previous
sample: strings are assigned, sequences finalised and re-initialised.

TICKLE_DIRECT_CODEC_MUTANT, at generation time, plants one of the pass-1 mutants
(LARGE_MESSAGE_PLAN.md, "Pass 1 harness"): absolute_align, size_strlen, len_prefix_plus1 or
no_bound_check. The harness must fail on each. Never set in a real build.
"""

import os

from tickle_typesupport import layout

from . import ros2_adapter

# cpp_string_size is the one mutant that changes only the C++ codec (ros2_cpp_direct_codec.py):
# the others live in the helpers both share, so they move both encoders the same way and the two
# still agree with each other. Without it, nothing would prove the harness's own C++ arm can fail.
# The last two change only the C++ codec (ros2_cpp_direct_codec.py); the others live in the
# helpers both share, so they move both encoders the same way and the two still agree with each
# other. Without a C++-only pair, nothing would prove the harness's C++ arm - or the C++ test
# beside it - can fail at all. cpp_string_size breaks the first-NUL rule, which only the test
# reaches; cpp_skip_pad breaks the layout, which only the arm reaches.
MUTANTS = (
    "absolute_align",
    "size_strlen",
    "len_prefix_plus1",
    "no_bound_check",
    "cpp_string_size",
    "cpp_skip_pad",
    "keep_shell_tail",
)
_SWAP = {2: ("uint16_t", "__builtin_bswap16"), 4: ("uint32_t", "__builtin_bswap32"), 8: ("uint64_t", "__builtin_bswap64")}
_SCALAR_SIZE = {
    "bool": 1,
    "byte": 1,
    "char": 1,
    "uint8": 1,
    "int8": 1,
    "uint16": 2,
    "int16": 2,
    "uint32": 4,
    "int32": 4,
    "float32": 4,
    "uint64": 8,
    "int64": 8,
    "float64": 8,
}


def mutant():
    value = os.environ.get("TICKLE_DIRECT_CODEC_MUTANT", "")
    if value and value not in MUTANTS:
        raise ValueError(f"TICKLE_DIRECT_CODEC_MUTANT={value!r}: not one of {', '.join(MUTANTS)}")
    return value


def _scalar_size(scalar_type):
    return _SCALAR_SIZE[scalar_type]


def _align(cursor, alignment):
    """Padding to the next multiple of `alignment` - relative to the struct's own start, since
    every struct is encoded from its own payload pointer. The absolute_align mutant measures from
    the buffer's address instead."""
    if mutant() == "absolute_align":
        return f"((uint32_t)(-(uintptr_t)(payload + {cursor})) & {alignment - 1}U)"
    return f"((uint32_t)(-{cursor}) & {alignment - 1}U)"


def _size_align(alignment):
    return f"((size_t)(-size) & {alignment - 1}U)"


def string_bound(f):
    """The IDL bound of a string field, or None. A capacity from a profile or annotation is the
    TickLE struct's, not the ROS type's."""
    return f.capacity if f.kind == "string" and f.capacity_source == "bounded" else None


def array_bound(f):
    return f.capacity if f.kind == "array" and f.array_mode == "variable" and f.capacity_source == "bounded" else None


def _bound_literal(bound):
    return "SIZE_MAX" if bound is None else f"(size_t){bound}"


def _pad_encode(alignment, static_padding):
    if alignment <= 1:
        return []
    if static_padding is not None and mutant() != "absolute_align":
        if static_padding == 0:
            return []
        return [
            f"if ((size_t)encoded + {static_padding} > len) {{ return -1; }}",
            f"memset(payload + encoded, 0, {static_padding});",
            f"encoded += {static_padding};",
        ]
    return [
        "{",
        f"    uint32_t pad = {_align('encoded', alignment)};",
        "    if ((size_t)encoded + pad > len) { return -1; }",
        "    memset(payload + encoded, 0, pad);",
        "    encoded += (int32_t)pad;",
        "}",
    ]


def _pad_decode(alignment, static_padding):
    if alignment <= 1:
        return []
    if static_padding is not None and mutant() != "absolute_align":
        if static_padding == 0:
            return []
        return [f"if ((size_t)decoded + {static_padding} > len) {{ return -1; }}", f"decoded += {static_padding};"]
    return [
        f"decoded += (int32_t){_align('decoded', alignment)};",
        "if ((uint32_t)decoded > len) { return -1; }",
    ]


def _pad_size(alignment, static_padding):
    if alignment <= 1:
        return []
    if static_padding is not None:
        return [f"size += {static_padding};"] if static_padding else []
    return [f"size += {_size_align(alignment)};"]


def _swap_in_place(ptr_expr, count_expr, size):
    """Byte-swaps `count` elements of `size` bytes at `ptr` - a foreign-endian decode's second
    step after the memcpy (rule 6). memcpy in and out, since `ptr` is a ROS allocation of the
    element's own type and the swap works on an unsigned integer of the same width."""
    if size == 1:
        return []
    wire, swap = _SWAP[size]
    return [
        "if (!is_native_endian) {",
        f"    uint8_t* bytes = (uint8_t*){ptr_expr};",
        f"    for (size_t i = 0; i < (size_t){count_expr}; i++) {{",
        f"        {wire} raw;",
        f"        memcpy(&raw, bytes + (i * {size}), {size});",
        f"        raw = {swap}(raw);",
        f"        memcpy(bytes + (i * {size}), &raw, {size});",
        "    }",
        "}",
    ]


def _braced(lines):
    return ["{"] + [f"    {ln}" for ln in lines] + ["}"]


# --- helpers shared by one file ---------------------------------------------------------------


def needs_string_helpers(struct):
    return any(f.kind == "string" or (f.kind == "array" and f.array_element_kind == "string") for f in struct.fields)


def emit_string_helpers():
    """Encode, decode and size one rosidl_runtime_c__String. `bound` is its IDL bound, or SIZE_MAX
    for an unbounded string (the only kind a string array holds - bounded elements are declined)."""
    m = mutant()
    if m == "size_strlen":
        length = ["size_t str_len = s->size + 1;"]
    else:
        length = [
            "const char* nul = (const char*)memchr(s->data, 0, s->size);",
            "size_t str_len = (nul != NULL ? (size_t)(nul - s->data) : s->size) + 1;",
        ]
    bound_check = [] if m == "no_bound_check" else ["if (s->size > bound) { return -2; }"]
    prefix = "(uint16_t)(str_len + 1)" if m == "len_prefix_plus1" else "(uint16_t)str_len"
    encode = (
        [
            "static int32_t direct_encode_string(const rosidl_runtime_c__String* s, size_t bound, uint8_t* payload, "
            "uint32_t len, int32_t encoded) {",
            "    if (s->data == NULL) { return -3; }",
        ]
        + [f"    {ln}" for ln in bound_check]
        + [f"    {ln}" for ln in length]
        + [
            "    if (str_len > 65535U) { return -2; }",
            "    if ((size_t)encoded + 2 > len) { return -1; }",
            "    {",
            f"        uint16_t prefix = {prefix};",
            "        memcpy(payload + encoded, &prefix, 2);",
            "    }",
            "    encoded += 2;",
            "    if ((size_t)encoded + str_len > len) { return -1; }",
            "    memcpy(payload + encoded, s->data, str_len - 1);",
            "    payload[(size_t)encoded + str_len - 1] = 0;",
            "    encoded += (int32_t)str_len;",
            "    {",
            f"        uint32_t pad4 = {_align('encoded', 4)};",
            "        if ((size_t)encoded + pad4 > len) { return -1; }",
            "        memset(payload + encoded, 0, pad4);",
            "        encoded += (int32_t)pad4;",
            "    }",
            "    return encoded;",
            "}",
            "",
        ]
    )
    size = (
        [
            "static int32_t direct_string_size(const rosidl_runtime_c__String* s, size_t bound, size_t* size) {",
            "    if (s->data == NULL) { return -3; }",
        ]
        + [f"    {ln}" for ln in bound_check]
        + [f"    {ln}" for ln in length]
        + [
            "    if (str_len > 65535U) { return -2; }",
            "    *size += 2 + str_len;",
            "    *size += (size_t)(-*size) & 3U;",
            "    return 0;",
            "}",
            "",
        ]
    )
    decode_bound = [] if m == "no_bound_check" else ["    if (bound != SIZE_MAX && str_len > bound + 1) { return -2; }"]
    decode = (
        [
            "static int32_t direct_decode_string(rosidl_runtime_c__String* s, size_t bound, const uint8_t* payload, "
            "uint32_t len, int32_t decoded, bool is_native_endian) {",
            "    if ((size_t)decoded + 2 > len) { return -1; }",
            "    uint16_t str_len;",
            "    memcpy(&str_len, payload + decoded, 2);",
            "    if (!is_native_endian) { str_len = __builtin_bswap16(str_len); }",
            "    if (str_len == 0) { return -2; }",
        ]
        + decode_bound
        + [
            "    decoded += 2;",
            "    if ((size_t)decoded + str_len > len) { return -1; }",
            "    if (payload[(size_t)decoded + str_len - 1] != 0) { return -2; }",
            # Up to the first NUL, as from_tickle's own assign of the struct path's aliased string.
            "    if (!rosidl_runtime_c__String__assign(s, (const char*)(payload + decoded))) { return -4; }",
            "    decoded += str_len;",
            f"    decoded += (int32_t){_align('decoded', 4)};",
            "    if ((uint32_t)decoded > len) { return -1; }",
            "    return decoded;",
            "}",
            "",
        ]
    )
    return encode + size + decode


# --- per field ----------------------------------------------------------------------------------


def _count_encode(f, count_expr):
    bound = array_bound(f)
    lines = [f"size_t count = {count_expr};"]
    if bound is not None and mutant() != "no_bound_check":
        lines.append(f"if (count > {bound}U) {{ return -2; }}")
    lines += [
        "if (count > 65535U) { return -2; }",
        "if ((size_t)encoded + 2 > len) { return -1; }",
        "{",
        "    uint16_t count16 = (uint16_t)count;",
        "    memcpy(payload + encoded, &count16, 2);",
        "}",
        "encoded += 2;",
    ]
    return lines


def _count_decode(f):
    bound = array_bound(f)
    lines = [
        "if ((size_t)decoded + 2 > len) { return -1; }",
        "uint16_t count;",
        "memcpy(&count, payload + decoded, 2);",
        "if (!is_native_endian) { count = __builtin_bswap16(count); }",
    ]
    if bound is not None and mutant() != "no_bound_check":
        lines.append(f"if (count > {bound}U) {{ return -2; }}")
    lines.append("decoded += 2;")
    return lines


def _count_size(f, count_expr):
    bound = array_bound(f)
    lines = [f"size_t count = {count_expr};"]
    if bound is not None and mutant() != "no_bound_check":
        lines.append(f"if (count > {bound}U) {{ return -2; }}")
    lines += ["if (count > 65535U) { return -2; }", "size += 2;"]
    return lines


def _nested_element_encode(f, elem):
    nested = ros2_adapter.ros2_nested_struct_name(f.nested)
    lines = []
    if f.element_align > 1:
        lines = ["if (i > 0) {"] + [f"    {ln}" for ln in _pad_encode(f.element_align, None)] + ["}"]
    return lines + [
        f"int32_t nested_size = {nested}__direct_encode(&{elem}, payload + encoded, len - (uint32_t)encoded);",
        "if (nested_size < 0) { return nested_size; }",
        "encoded += nested_size;",
    ]


def _nested_element_decode(f, elem):
    nested = ros2_adapter.ros2_nested_struct_name(f.nested)
    lines = []
    if f.element_align > 1:
        lines = ["if (i > 0) {"] + [f"    {ln}" for ln in _pad_decode(f.element_align, None)] + ["}"]
    return lines + [
        f"int32_t nested_size = {nested}__direct_decode(&{elem}, payload + decoded, len - (uint32_t)decoded, "
        "is_native_endian);",
        "if (nested_size < 0) { return nested_size; }",
        "decoded += nested_size;",
    ]


def _nested_element_size(f, elem):
    nested = ros2_adapter.ros2_nested_struct_name(f.nested)
    lines = []
    if f.element_align > 1:
        lines = ["if (i > 0) {", f"    size += {_size_align(f.element_align)};", "}"]
    return lines + [
        f"int32_t nested_size = {nested}__direct_encode_size(&{elem});",
        "if (nested_size < 0) { return nested_size; }",
        "size += (size_t)nested_size;",
        "if (size > INT32_MAX) { return -2; }",
    ]


def _loop(count_expr, body):
    return [f"for (size_t i = 0; i < (size_t){count_expr}; i++) {{"] + [f"    {ln}" for ln in body] + ["}"]


def _field_encode(f):
    r = f"ros->{f.name}"
    if f.kind == "scalar":
        size = _scalar_size(f.scalar_type)
        return _braced(
            [
                f"if ((size_t)encoded + {size} > len) {{ return -1; }}",
                f"memcpy(payload + encoded, &{r}, {size});",
                f"encoded += {size};",
            ]
        )
    if f.kind == "string":
        return _braced(
            [
                f"encoded = direct_encode_string(&{r}, {_bound_literal(string_bound(f))}, payload, len, encoded);",
                "if (encoded < 0) { return encoded; }",
            ]
        )
    if f.kind == "nested":
        nested = ros2_adapter.ros2_nested_struct_name(f.nested)
        return _braced(
            [
                f"int32_t nested_size = {nested}__direct_encode(&{r}, payload + encoded, len - (uint32_t)encoded);",
                "if (nested_size < 0) { return nested_size; }",
                "encoded += nested_size;",
            ]
        )
    fixed = f.array_mode == "fixed"
    count = str(f.array_size) if fixed else f"{r}.size"
    elem = f"{r}[i]" if fixed else f"{r}.data[i]"
    head = [] if fixed else _count_encode(f, count)
    if f.array_element_kind == "string":
        body = [
            "encoded = direct_encode_string(&" + elem + ", SIZE_MAX, payload, len, encoded);",
            "if (encoded < 0) { return encoded; }",
        ]
        return _braced(head + _loop("count" if not fixed else count, body))
    if f.array_element_kind == "nested":
        pad = [] if fixed else _pad_encode(f.element_align, None)
        return _braced(head + pad + _loop("count" if not fixed else count, _nested_element_encode(f, elem)))
    size = f.element_size
    data = r if fixed else f"{r}.data"
    n = count if fixed else "count"
    pad = [] if fixed else _pad_encode(f.element_align, None)
    return _braced(
        head
        + pad
        + [
            f"size_t bytes = (size_t){n} * {size};",
            "if ((size_t)encoded + bytes > len) { return -1; }",
            "if (bytes > 0) {",
            f"    memcpy(payload + encoded, {data}, bytes);",
            "}",
            "encoded += (int32_t)bytes;",
        ]
    )


def _field_decode(f):
    r = f"ros->{f.name}"
    if f.kind == "scalar":
        size = _scalar_size(f.scalar_type)
        body = [f"if ((size_t)decoded + {size} > len) {{ return -1; }}"]
        if size == 1:
            body.append(f"memcpy(&{r}, payload + decoded, 1);")
        else:
            wire, swap = _SWAP[size]
            body += [
                f"{wire} raw;",
                f"memcpy(&raw, payload + decoded, {size});",
                f"if (!is_native_endian) {{ raw = {swap}(raw); }}",
                f"memcpy(&{r}, &raw, {size});",
            ]
        return _braced(body + [f"decoded += {size};"])
    if f.kind == "string":
        return _braced(
            [
                f"decoded = direct_decode_string(&{r}, {_bound_literal(string_bound(f))}, payload, len, decoded, "
                "is_native_endian);",
                "if (decoded < 0) { return decoded; }",
            ]
        )
    if f.kind == "nested":
        nested = ros2_adapter.ros2_nested_struct_name(f.nested)
        return _braced(
            [
                f"int32_t nested_size = {nested}__direct_decode(&{r}, payload + decoded, len - (uint32_t)decoded, "
                "is_native_endian);",
                "if (nested_size < 0) { return nested_size; }",
                "decoded += nested_size;",
            ]
        )
    fixed = f.array_mode == "fixed"
    elem = f"{r}[i]" if fixed else f"{r}.data[i]"
    if f.array_element_kind == "string":
        body = [
            f"decoded = direct_decode_string(&{elem}, SIZE_MAX, payload, len, decoded, is_native_endian);",
            "if (decoded < 0) { return decoded; }",
        ]
        if fixed:
            return _braced(_loop(f.array_size, body))
        return _braced(
            _count_decode(f)
            + [
                # Every string element takes at least 4 bytes (prefix, NUL, padding) - refuse a
                # count the rest cannot hold before allocating for it.
                "if ((size_t)decoded + ((size_t)count * 4) > len) { return -1; }",
                f"rosidl_runtime_c__String__Sequence__fini(&{r});",
                f"if (!rosidl_runtime_c__String__Sequence__init(&{r}, count)) {{ return -4; }}",
            ]
            + _loop("count", body)
        )
    if f.array_element_kind == "nested":
        body = _nested_element_decode(f, elem)
        if fixed:
            return _braced(_loop(f.array_size, body))
        nested = ros2_adapter.ros2_nested_struct_name(f.nested)
        return _braced(
            _count_decode(f)
            + _pad_decode(f.element_align, None)
            + [
                f"{nested}__Sequence__fini(&{r});",
                f"if (!{nested}__Sequence__init(&{r}, count)) {{ return -4; }}",
            ]
            + _loop("count", body)
        )
    size = f.element_size
    if fixed:
        return _braced(
            [
                f"size_t bytes = (size_t){f.array_size} * {size};",
                "if ((size_t)decoded + bytes > len) { return -1; }",
                f"memcpy({r}, payload + decoded, bytes);",
            ]
            + _swap_in_place(r, f.array_size, size)
            + ["decoded += (int32_t)bytes;"]
        )
    seq = f"rosidl_runtime_c__{ros2_adapter._ros2_sequence_scalar_type(f.scalar_type)}__Sequence"
    # A decode has to leave the message holding this sample and nothing of the last one, because
    # rmw hands it a pooled shell that still holds the previous sample (rmw_subscription.c). The
    # keep_shell_tail mutant reuses an allocation that is merely big enough and leaves .size as it
    # was, so a shorter sample keeps the longer one's tail - the exact failure a pool invites.
    reset = (
        [f"if ({r}.size < count) {{"]
        + [f"    {ln}" for ln in [f"{seq}__fini(&{r});", f"if (!{seq}__init(&{r}, count)) {{ return -4; }}"]]
        + ["}"]
        if mutant() == "keep_shell_tail"
        else [f"{seq}__fini(&{r});", f"if (!{seq}__init(&{r}, count)) {{ return -4; }}"]
    )
    return _braced(
        _count_decode(f)
        + _pad_decode(f.element_align, None)
        + [
            f"size_t bytes = (size_t)count * {size};",
            "if ((size_t)decoded + bytes > len) { return -1; }",
        ]
        + reset
        + [
            "if (bytes > 0) {",
            f"    memcpy({r}.data, payload + decoded, bytes);",
            "}",
        ]
        + _swap_in_place(f"{r}.data", "count", size)
        + ["decoded += (int32_t)bytes;"]
    )


def _field_size(f):
    r = f"ros->{f.name}"
    if f.kind == "scalar":
        return [f"size += {_scalar_size(f.scalar_type)};"]
    if f.kind == "string":
        return _braced(
            [
                f"int32_t ret = direct_string_size(&{r}, {_bound_literal(string_bound(f))}, &size);",
                "if (ret < 0) { return ret; }",
            ]
        )
    if f.kind == "nested":
        nested = ros2_adapter.ros2_nested_struct_name(f.nested)
        return _braced(
            [
                f"int32_t nested_size = {nested}__direct_encode_size(&{r});",
                "if (nested_size < 0) { return nested_size; }",
                "size += (size_t)nested_size;",
            ]
        )
    fixed = f.array_mode == "fixed"
    elem = f"{r}[i]" if fixed else f"{r}.data[i]"
    head = [] if fixed else _count_size(f, f"{r}.size")
    n = str(f.array_size) if fixed else "count"
    if f.array_element_kind == "string":
        body = ["int32_t ret = direct_string_size(&" + elem + ", SIZE_MAX, &size);", "if (ret < 0) { return ret; }"]
        return _braced(head + _loop(n, body) + ["if (size > INT32_MAX) { return -2; }"])
    if f.array_element_kind == "nested":
        pad = [] if fixed else _pad_size(f.element_align, None)
        return _braced(head + pad + _loop(n, _nested_element_size(f, elem)))
    pad = [] if fixed else _pad_size(f.element_align, None)
    return _braced(head + pad + [f"size += (size_t){n} * {f.element_size};"])


# --- whole functions ----------------------------------------------------------------------------


def declarations(ros_name):
    return [
        f"int32_t {ros_name}__direct_encode_size(const struct {ros_name}* ros);",
        f"int32_t {ros_name}__direct_encode(const struct {ros_name}* ros, uint8_t* payload, uint32_t len);",
        f"int32_t {ros_name}__direct_decode(struct {ros_name}* ros, const uint8_t* payload, uint32_t len, "
        "bool is_native_endian);",
    ]


def emit_functions(struct, ros_name):
    plans = layout.plan_fields(struct.fields)
    encode = [
        f"int32_t {ros_name}__direct_encode(const struct {ros_name}* ros, uint8_t* payload, uint32_t len) {{",
        "    (void)ros;",
        "    (void)payload;",
        "    (void)len;",
        "    int32_t encoded = 0;",
    ]
    decode = [
        f"int32_t {ros_name}__direct_decode(struct {ros_name}* ros, const uint8_t* payload, uint32_t len, "
        "bool is_native_endian) {",
        "    (void)ros;",
        "    (void)payload;",
        "    (void)len;",
        "    (void)is_native_endian;",
        "    int32_t decoded = 0;",
    ]
    size = [f"int32_t {ros_name}__direct_encode_size(const struct {ros_name}* ros) {{", "    (void)ros;"]
    if struct.is_fixed_size:
        size.append(f"    return {struct.wire_size};")
    else:
        size.append("    size_t size = 0;")
    for plan in plans:
        f = plan.field
        encode += [f"    {ln}" for ln in _pad_encode(f.wire_align, plan.static_padding) + _field_encode(f)]
        decode += [f"    {ln}" for ln in _pad_decode(f.wire_align, plan.static_padding) + _field_decode(f)]
        if not struct.is_fixed_size:
            size += [f"    {ln}" for ln in _pad_size(f.wire_align, plan.static_padding) + _field_size(f)]
    encode += ["    return encoded;", "}"]
    decode += ["    return decoded;", "}"]
    if not struct.is_fixed_size:
        size += ["    if (size > INT32_MAX) { return -2; }", "    return (int32_t)size;"]
    size.append("}")
    return size + [""] + encode + [""] + decode
