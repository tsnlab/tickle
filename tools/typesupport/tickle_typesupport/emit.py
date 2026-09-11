# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Renders a WireStruct's C. This is where the CDR-4 rules in DESIGN.md ("Interface
serialization") actually turn into bytes-on-the-wire code; the templates (templates/*.em) call
these functions and splice the returned lines in - they don't encode any wire-format knowledge
themselves.

Two things every function here leans on:
  - the payload pointer a generated *_encode/*_decode is ever called with is always at least
    4-byte aligned (tt_Node's tx_buffer/rx_buffer are _Alignas(4), and every framing header ends
    on a 4-byte boundary - see the _Static_assert()s in src/tickle.c) - so once a field's own
    offset is aligned to its own requirement, direct pointer casts (`*(uint32_t*)(payload+off)`)
    are well-defined, not just "happens to work". That's what buys the "no memcpy" scalar
    encode/decode the CDR-4 alignment rule exists for.
  - layout.plan_fields() already worked out, per field, whether the padding in front of it is a
    compile-time constant (most fields, right up until the first string) or has to be computed
    at runtime (every field after that) - see FieldPlan.static_padding.
"""

from . import layout, model

_WIRE_UINT = {2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
_BSWAP = {2: "_tt_bswap_16", 4: "_tt_bswap_32", 8: "_tt_bswap_64"}
_FLOAT_TYPES = {"float32", "float64"}


def emit_struct_fields(struct):
    """Field declarations only - the template wraps them in `#pragma pack(push, 4)` /
    `struct NAME { ... };` / `#pragma pack(pop)`. Plain field-ordered declarations are enough:
    pack(4) caps the compiler's own alignment at min(natural, 4) per member, which is exactly
    the CDR-4 rule - no manually inserted padding fields needed (verified against both gcc/
    x86-64 and the riscv64-unknown-elf cross compiler; see PLAN.md). An array needs more than
    one type-string can express (a fixed array is just `elem name[N];`, but a variable array
    also needs its own count member alongside the fixed-capacity buffer - declared *before* the
    buffer, matching the count-then-elements wire order, so a struct that's otherwise fixed-size
    up to a single trailing byte array can alias its own memory as the wire payload the same way
    a fully fixed-size one does - see layout.prefix_array_field), so it's special-cased here
    rather than going through field.ctype like every other kind does."""
    if not struct.fields:
        # An empty struct is a GNU extension (see tt_Request's own note in tickle.h) - a message
        # with no fields (e.g. Trigger.srv's request) still needs one byte to stay valid ISO C.
        # It is not a wire field: nothing ever reads or writes it.
        return ["uint8_t reserved; // empty message - not serialized, just fills the struct"]
    lines = []
    for wire_field in struct.fields:
        if wire_field.kind == "array":
            if wire_field.array_mode == "fixed":
                lines.append(f"{wire_field.element_ctype} {wire_field.name}[{wire_field.array_size}];")
            else:
                lines.append(f"uint16_t {wire_field.name}_count; // <= {wire_field.capacity}")
                lines.append(f"{wire_field.element_ctype} {wire_field.name}[{wire_field.capacity}];")
        else:
            lines.append(f"{wire_field.ctype} {wire_field.name};")
    return lines


def emit_constants(struct):
    lines = []
    for constant in struct.constants:
        macro_name = f"{struct.c_name.upper()}__{constant.name}"
        if constant.scalar_type in ("string", "wstring"):
            lines.append(f'static const char * const {macro_name} = "{constant.value}";')
        elif constant.scalar_type in _FLOAT_TYPES:
            ctype = model.SCALAR_CTYPE[constant.scalar_type]
            lines.append(f"static const {ctype} {macro_name} = {constant.value};")
        else:
            lines.append(f"enum {{ {macro_name} = {constant.value} }};")
    return lines


def emit_array_capacity_constants(struct):
    """A #define for each variable array field's resolved capacity (a fixed array doesn't need
    one - its N is already spelled out in the field declaration itself, `elem name[N];`). Lets
    application code bounds-check against - or size something relative to - the number the
    generator picked without hardcoding it, particularly for the "auto" capacity-resolution case
    where nothing else in the .msg says what it is (examples/Bulk.msg's own `payload`, whose
    hand-written predecessor exposed exactly this as its own `BULK_MAX_PAYLOAD_SIZE` macro)."""
    return [
        f"#define {struct.c_name.upper()}__{f.name.upper()}_CAPACITY {f.capacity}"
        for f in struct.fields
        if f.kind == "array" and f.array_mode == "variable"
    ]


def _emit_pad(cursor, count_expr, *, runtime):
    """count_expr is either an int literal (runtime=False) or a C expression string that
    evaluates the pad count at runtime (runtime=True). The runtime form declares a local `pad`,
    so it's wrapped in its own { } scope - this can run once per field in the same function."""
    if not runtime and count_expr == 0:
        return []
    if runtime:
        body = [
            f"uint32_t pad = {count_expr};",
            f"if ((uint32_t){cursor} + pad > len) {{ return -1; }}",
            f"memset(payload + {cursor}, 0, pad);",
            f"{cursor} += (int32_t)pad;",
        ]
        return ["{"] + [f"    {ln}" for ln in body] + ["}"]
    count = str(count_expr)
    return [
        f"if ((uint32_t){cursor} + ({count}) > len) {{ return -1; }}",
        f"memset(payload + {cursor}, 0, {count});",
        f"{cursor} += (int32_t)({count});",
    ]


def _runtime_align_expr(cursor, alignment):
    # alignment is a power of two (1, 2 or 4); this is the usual "round a running byte count up
    # to the next multiple" bit trick, on the two's-complement negation so it works without a
    # modulo or a branch. `cursor` (encoded/decoded/size) is always already int32_t, so negating
    # it needs no cast - adding one would be a no-op clang-tidy flags as redundant. Fully
    # parenthesized: callers wrap this in `(int32_t){...}`, and without the outer parens here
    # `&` binds looser than a leading cast, so `(int32_t)(uint32_t)(-x) & Nu` would apply the
    # int32_t cast *before* the mask (narrowing the unsigned `&` result back on assignment,
    # which is exactly the "implementation-defined narrowing conversion" clang-tidy flags).
    return f"((uint32_t)(-{cursor}) & {alignment - 1}U)"


def _emit_align_encode(plan):
    if plan.field.wire_align <= 1:
        return []
    if plan.static_padding is not None:
        return _emit_pad("encoded", plan.static_padding, runtime=False)
    return _emit_pad("encoded", _runtime_align_expr("encoded", plan.field.wire_align), runtime=True)


def _emit_align_decode(plan):
    if plan.field.wire_align <= 1:
        return []
    if plan.static_padding is not None:
        if plan.static_padding == 0:
            return []
        return [f"if ((uint32_t)decoded + {plan.static_padding} > len) {{ return -1; }}", f"decoded += {plan.static_padding};"]
    return [
        f"decoded += (int32_t){_runtime_align_expr('decoded', plan.field.wire_align)};",
        "if ((uint32_t)decoded > len) { return -1; }",
    ]


def _braced(body_lines):
    """Wraps a field's own emitted statements in their own { } scope, so per-field locals
    (raw/convert) from different fields in the same encode/decode function never collide."""
    return ["{"] + [f"    {ln}" for ln in body_lines] + ["}"]


def _emit_scalar_encode(field):
    size = model.SCALAR_SIZE[field.scalar_type]
    body = [f"if ((uint32_t)encoded + {size} > len) {{ return -1; }}"]
    if field.scalar_type in _FLOAT_TYPES:
        wire = _WIRE_UINT[size]
        body += [
            f"union {{ {field.ctype} value; {wire} bits; }} convert;",
            f"convert.value = data->{field.name};",
            f"*({wire}*)(payload + encoded) = convert.bits;",
        ]
    else:
        body.append(f"*({field.ctype}*)(payload + encoded) = data->{field.name};")
    body.append(f"encoded += {size};")
    return _braced(body)


def _emit_scalar_decode(field):
    size = model.SCALAR_SIZE[field.scalar_type]
    body = [f"if ((uint32_t)decoded + {size} > len) {{ return -1; }}"]
    if size == 1:
        body.append(f"data->{field.name} = *(const {field.ctype}*)(payload + decoded);")
    else:
        wire = _WIRE_UINT[size]
        bswap = _BSWAP[size]
        body.append(f"{wire} raw = *(const {wire}*)(payload + decoded);")
        body.append(f"if (!is_native_endian) {{ raw = {bswap}(raw); }}")
        if field.scalar_type in _FLOAT_TYPES:
            body += [
                f"union {{ {wire} bits; {field.ctype} value; }} convert;",
                "convert.bits = raw;",
                f"data->{field.name} = convert.value;",
            ]
        elif field.ctype != wire:
            body.append(f"data->{field.name} = ({field.ctype})raw;")
        else:
            # field.ctype and the wire-unsigned type coincide (e.g. uint64 -> uint64_t) - an
            # explicit same-type cast here is what readability-redundant-casting flags.
            body.append(f"data->{field.name} = raw;")
    body.append(f"decoded += {size};")
    return _braced(body)


def _emit_string_encode(field):
    return [
        f"if (data->{field.name} == NULL) {{ return -3; }}",
        "{",
        f"    size_t str_len = _tt_strnlen(data->{field.name}, tt_MAX_STRING_LENGTH) + 1;",
        "    if (str_len > tt_MAX_STRING_LENGTH) { return -2; }",
        "    if ((uint32_t)encoded + 2 > len) { return -1; }",
        "    *(uint16_t*)(payload + encoded) = (uint16_t)str_len;",
        "    encoded += 2;",
        "    if ((uint32_t)encoded + str_len > len) { return -1; }",
        f"    memcpy(payload + encoded, data->{field.name}, str_len);",
        "    encoded += (int32_t)str_len;",
        "    {",
        f"        uint32_t pad4 = {_runtime_align_expr('encoded', 4)};",
        "        if ((uint32_t)encoded + pad4 > len) { return -1; }",
        "        memset(payload + encoded, 0, pad4);",
        "        encoded += (int32_t)pad4;",
        "    }",
        "}",
    ]


def _emit_string_decode(field):
    return [
        "{",
        "    if ((uint32_t)decoded + 2 > len) { return -1; }",
        "    uint16_t str_len = *(const uint16_t*)(payload + decoded);",
        "    if (!is_native_endian) { str_len = _tt_bswap_16(str_len); }",
        "    if (str_len == 0) { return -2; }",
        "    decoded += 2;",
        "    if ((uint32_t)decoded + str_len > len) { return -1; }",
        "    if (payload[decoded + str_len - 1] != '\\0') { return -2; }",
        f"    data->{field.name} = (char*)(payload + decoded); // aliases the input buffer - see *_free()",
        "    decoded += str_len;",
        f"    decoded += (int32_t){_runtime_align_expr('decoded', 4)};",
        "    if ((uint32_t)decoded > len) { return -1; }",
        "}",
    ]


def _emit_fixed_array_encode(field):
    n, size = field.array_size, field.element_size
    total = n * size
    check = [f"if ((uint32_t)encoded + {total} > len) {{ return -1; }}"]
    if size == 1:
        # 1-byte elements (uint8/int8/byte/char/bool) never need a byte-swap - a plain memcpy
        # is both simpler and faster than a per-element loop.
        return _braced(check + [f"memcpy(payload + encoded, data->{field.name}, {total});", f"encoded += {total};"])
    wire = _WIRE_UINT[size]
    if field.scalar_type in _FLOAT_TYPES:
        loop = [
            f"union {{ {field.element_ctype} value; {wire} bits; }} convert;",
            f"convert.value = data->{field.name}[i];",
            f"*({wire}*)(payload + encoded + ((size_t)i * {size})) = convert.bits;",
        ]
    else:
        loop = [f"*({field.element_ctype}*)(payload + encoded + ((size_t)i * {size})) = data->{field.name}[i];"]
    body = check + [f"for (uint32_t i = 0; i < {n}; i++) {{"] + [f"    {ln}" for ln in loop] + ["}", f"encoded += {total};"]
    return _braced(body)


def _emit_fixed_array_decode(field):
    n, size = field.array_size, field.element_size
    total = n * size
    check = [f"if ((uint32_t)decoded + {total} > len) {{ return -1; }}"]
    if size == 1:
        return _braced(check + [f"memcpy(data->{field.name}, payload + decoded, {total});", f"decoded += {total};"])
    wire, bswap = _WIRE_UINT[size], _BSWAP[size]
    loop = [
        f"{wire} raw = *(const {wire}*)(payload + decoded + ((size_t)i * {size}));",
        f"if (!is_native_endian) {{ raw = {bswap}(raw); }}",
    ]
    if field.scalar_type in _FLOAT_TYPES:
        loop += [
            f"union {{ {wire} bits; {field.element_ctype} value; }} convert;",
            "convert.bits = raw;",
            f"data->{field.name}[i] = convert.value;",
        ]
    elif field.element_ctype != wire:
        loop.append(f"data->{field.name}[i] = ({field.element_ctype})raw;")
    else:
        loop.append(f"data->{field.name}[i] = raw;")
    body = check + [f"for (uint32_t i = 0; i < {n}; i++) {{"] + [f"    {ln}" for ln in loop] + ["}", f"decoded += {total};"]
    return _braced(body)


def _emit_variable_array_encode(field):
    count_var = f"data->{field.name}_count"
    size = field.element_size
    lines = [
        f"if ({count_var} > {field.capacity}) {{ return -2; }}",
        "{",
        "    if ((uint32_t)encoded + 2 > len) { return -1; }",
        f"    *(uint16_t*)(payload + encoded) = {count_var};",
        "    encoded += 2;",
    ]
    if field.element_align > 1:
        lines += [
            "    {",
            f"        uint32_t pad = {_runtime_align_expr('encoded', field.element_align)};",
            "        if ((uint32_t)encoded + pad > len) { return -1; }",
            "        memset(payload + encoded, 0, pad);",
            "        encoded += (int32_t)pad;",
            "    }",
        ]
    total_expr = f"((uint32_t){count_var} * {size})"
    if size == 1:
        lines += [
            f"    if ((uint32_t)encoded + {count_var} > len) {{ return -1; }}",
            f"    memcpy(payload + encoded, data->{field.name}, {count_var});",
            f"    encoded += (int32_t){count_var};",
        ]
    else:
        wire = _WIRE_UINT[size]
        if field.scalar_type in _FLOAT_TYPES:
            loop = [
                f"union {{ {field.element_ctype} value; {wire} bits; }} convert;",
                f"convert.value = data->{field.name}[i];",
                f"*({wire}*)(payload + encoded + ((size_t)i * {size})) = convert.bits;",
            ]
        else:
            loop = [f"*({field.element_ctype}*)(payload + encoded + ((size_t)i * {size})) = data->{field.name}[i];"]
        lines += (
            [
                f"    if ((uint32_t)encoded + {total_expr} > len) {{ return -1; }}",
                f"    for (uint32_t i = 0; i < {count_var}; i++) {{",
            ]
            + [f"        {ln}" for ln in loop]
            + ["    }", f"    encoded += (int32_t){total_expr};"]
        )
    lines.append("}")
    return lines


def _emit_variable_array_decode(field):
    size = field.element_size
    lines = [
        "{",
        "    if ((uint32_t)decoded + 2 > len) { return -1; }",
        "    uint16_t count = *(const uint16_t*)(payload + decoded);",
        "    if (!is_native_endian) { count = _tt_bswap_16(count); }",
        f"    if (count > {field.capacity}) {{ return -2; }}",
        "    decoded += 2;",
    ]
    if field.element_align > 1:
        lines += [
            "    {",
            f"        uint32_t pad = {_runtime_align_expr('decoded', field.element_align)};",
            "        if ((uint32_t)decoded + pad > len) { return -1; }",
            "        decoded += (int32_t)pad;",
            "    }",
        ]
    total_expr = f"((uint32_t)count * {size})"
    if size == 1:
        lines += [
            "    if ((uint32_t)decoded + count > len) { return -1; }",
            f"    memcpy(data->{field.name}, payload + decoded, count);",
            "    decoded += (int32_t)count;",
        ]
    else:
        wire, bswap = _WIRE_UINT[size], _BSWAP[size]
        loop = [
            f"{wire} raw = *(const {wire}*)(payload + decoded + ((size_t)i * {size}));",
            f"if (!is_native_endian) {{ raw = {bswap}(raw); }}",
        ]
        if field.scalar_type in _FLOAT_TYPES:
            loop += [
                f"union {{ {wire} bits; {field.element_ctype} value; }} convert;",
                "convert.bits = raw;",
                f"data->{field.name}[i] = convert.value;",
            ]
        elif field.element_ctype != wire:
            loop.append(f"data->{field.name}[i] = ({field.element_ctype})raw;")
        else:
            loop.append(f"data->{field.name}[i] = raw;")
        lines += (
            [
                f"    if ((uint32_t)decoded + {total_expr} > len) {{ return -1; }}",
                "    for (uint32_t i = 0; i < count; i++) {",
            ]
            + [f"        {ln}" for ln in loop]
            + ["    }", f"    decoded += (int32_t){total_expr};"]
        )
    lines += [f"    data->{field.name}_count = count;", "}"]
    return lines


def _emit_nested_encode(field):
    # A nested type's own *_encode already writes starting at its own payload pointer's offset
    # 0 with no header of its own (DESIGN.md's "no header, no extra alignment beyond what the
    # first nested field needs") - calling it with `payload + encoded` is exactly "inlining its
    # fields at the current offset", just delegated instead of pasted in field-by-field. Its
    # error codes (-1/-2/-3) already mean the same thing here, so they propagate unchanged.
    return [
        "{",
        f"    int32_t nested_size = {field.nested.c_name}_encode(&data->{field.name}, payload + encoded, len - (uint32_t)encoded);",
        "    if (nested_size < 0) { return nested_size; }",
        "    encoded += nested_size;",
        "}",
    ]


def _emit_nested_decode(field):
    return [
        "{",
        f"    int32_t nested_size = {field.nested.c_name}_decode(&data->{field.name}, payload + decoded, "
        "len - (uint32_t)decoded, is_native_endian);",
        "    if (nested_size < 0) { return nested_size; }",
        "    decoded += nested_size;",
        "}",
    ]


def emit_encode(struct):
    # Defensive (void) casts, not conditional on whether each parameter ends up used below: a
    # message with zero fields (e.g. Trigger.srv's request) never touches data/payload/len at
    # all, and `(void)x;` ahead of a real use of `x` is a no-op, never a new warning.
    lines = ["(void)data;", "(void)payload;", "(void)len;", "int32_t encoded = 0;"]
    for plan in layout.plan_fields(struct.fields):
        lines += _emit_align_encode(plan)
        if plan.field.kind == "scalar":
            lines += _emit_scalar_encode(plan.field)
        elif plan.field.kind == "string":
            lines += _emit_string_encode(plan.field)
        elif plan.field.kind == "array" and plan.field.array_mode == "fixed":
            lines += _emit_fixed_array_encode(plan.field)
        elif plan.field.kind == "array":
            lines += _emit_variable_array_encode(plan.field)
        elif plan.field.kind == "nested":
            lines += _emit_nested_encode(plan.field)
        else:
            raise NotImplementedError(plan.field.kind)
    lines.append("return encoded;")
    return lines


def emit_decode(struct):
    lines = [
        "(void)data;",
        "(void)payload;",
        "(void)len;",
        "(void)is_native_endian;",
        "int32_t decoded = 0;",
    ]
    for plan in layout.plan_fields(struct.fields):
        lines += _emit_align_decode(plan)
        if plan.field.kind == "scalar":
            lines += _emit_scalar_decode(plan.field)
        elif plan.field.kind == "string":
            lines += _emit_string_decode(plan.field)
        elif plan.field.kind == "array" and plan.field.array_mode == "fixed":
            lines += _emit_fixed_array_decode(plan.field)
        elif plan.field.kind == "array":
            lines += _emit_variable_array_decode(plan.field)
        elif plan.field.kind == "nested":
            lines += _emit_nested_decode(plan.field)
        else:
            raise NotImplementedError(plan.field.kind)
    lines.append("return decoded;")
    return lines


def emit_encode_size(struct):
    if struct.is_fixed_size:
        return ["(void)data;", f"return {struct.wire_size};"]
    lines = ["int32_t size = 0;"]
    for plan in layout.plan_fields(struct.fields):
        if plan.field.wire_align > 1:
            if plan.static_padding is not None:
                if plan.static_padding:
                    lines.append(f"size += {plan.static_padding};")
            else:
                lines.append(f"size += (int32_t){_runtime_align_expr('size', plan.field.wire_align)};")
        if plan.field.kind == "scalar":
            lines.append(f"size += {model.SCALAR_SIZE[plan.field.scalar_type]};")
        elif plan.field.kind == "string":
            lines += [
                f"if (data->{plan.field.name} == NULL) {{ return -3; }}",
                "{",
                f"    size_t str_len = _tt_strnlen(data->{plan.field.name}, tt_MAX_STRING_LENGTH) + 1;",
                "    if (str_len > tt_MAX_STRING_LENGTH) { return -2; }",
                "    size += (int32_t)(2 + str_len);",
                f"    size += (int32_t){_runtime_align_expr('size', 4)};",
                "}",
            ]
        elif plan.field.kind == "array" and plan.field.array_mode == "fixed":
            lines.append(f"size += {plan.field.wire_size};")
        elif plan.field.kind == "array":
            count_var = f"data->{plan.field.name}_count"
            lines.append(f"if ({count_var} > {plan.field.capacity}) {{ return -2; }}")
            lines.append("size += 2;")
            if plan.field.element_align > 1:
                lines.append(f"size += (int32_t){_runtime_align_expr('size', plan.field.element_align)};")
            lines.append(f"size += (int32_t)((uint32_t){count_var} * {plan.field.element_size});")
        elif plan.field.kind == "nested":
            lines += [
                "{",
                f"    int32_t nested_size = {plan.field.nested.c_name}_encode_size(&data->{plan.field.name});",
                "    if (nested_size < 0) { return nested_size; }",
                "    size += nested_size;",
                "}",
            ]
        else:
            raise NotImplementedError(plan.field.kind)
    lines.append("return size;")
    return lines


def emit_encode_inplace(struct):
    """Only emitted for an all-fixed-size struct (see render._struct_context's "is_fixed_size",
    the same gate the sizeof/wire_size _Static_assert uses) - #pragma pack(4) already makes the
    struct's own memory byte-identical to its CDR-4 wire form on every supported ABI (DESIGN.md's
    "Struct layout" note), so there's nothing to serialize: just hand the struct's own address
    over as the payload pointer. Always safe regardless of the local host's endianness - the
    struct is however this same process's encode()/scalar writes would have produced it, i.e.
    already native, exactly what encode_inplace is contractually allowed to hand back."""
    return ["*payload_out = (const uint8_t*)data;", f"return {struct.wire_size};"]


def emit_decode_inplace(struct):
    """The decode-side mirror of emit_encode_inplace - only safe when the wire bytes are already
    native-endian (a foreign-endian payload's bytes are NOT what the struct's own memory would
    contain, so this must fall back to NULL - the caller then uses the regular copying *_decode()
    instead, per tt_DATA_DECODE_INPLACE's own contract in tickle.h) and long enough to hold the
    whole fixed-size struct."""
    return [
        f"if (!is_native_endian || len < {struct.wire_size}) {{ return NULL; }}",
        f"return (struct {struct.c_name}*)payload;",
    ]


def emit_prefix_encode_inplace(struct, array_field):
    """*_encode_inplace for a struct that isn't fully fixed-size but does end in exactly one
    prefix-aliasable trailing byte array (layout.prefix_array_field) - the fixed header plus
    however many of the array's elements are actually in use (data->*_count, not its full
    capacity) is still one contiguous span of the struct's own memory, count member declared
    right before the buffer (emit_struct_fields) to match the wire's own count-then-elements
    order. Same capacity check *_encode/_encode_size already do, for the same reason: a
    data->*_count an application set larger than the array's own declared capacity would read
    out of bounds below, not just write a wrong wire length."""
    prefix_offset = layout.plan_fields(struct.fields)[-1].static_offset
    count_var = f"data->{array_field.name}_count"
    return [
        f"if ({count_var} > {array_field.capacity}) {{ return -2; }}",
        "*payload_out = (const uint8_t*)data;",
        f"return {prefix_offset} + 2 + (int32_t){count_var};",
    ]


def emit_prefix_decode_inplace(struct, array_field):
    """The decode-side mirror of emit_prefix_encode_inplace. Reads the count directly out of the
    (already known native-endian) payload rather than through a decoded struct - there isn't one
    yet, the whole point is handing back a pointer into `payload` itself - then bounds-checks it
    exactly like the copying *_decode() does (count <= capacity, and the elements it claims
    actually fit in `len`) before aliasing."""
    prefix_offset = layout.plan_fields(struct.fields)[-1].static_offset
    return [
        f"if (!is_native_endian || len < (uint32_t){prefix_offset} + 2) {{ return NULL; }}",
        "{",
        f"    uint16_t count = *(const uint16_t*)(payload + {prefix_offset});",
        f"    if (count > {array_field.capacity} || (uint32_t){prefix_offset} + 2 + count > len) {{ return NULL; }}",
        "}",
        f"return (struct {struct.c_name}*)payload;",
    ]


def emit_free(struct):
    # Nothing is ever malloc'd: strings alias the rx buffer they were decoded from (see
    # _emit_string_decode), and there are no other owned resources - see DESIGN.md's "No
    # dynamic allocation" note, which this generator preserves rather than reintroducing malloc.
    return ["(void)data;"]


def emit_init(struct):
    """Only emitted (and only declared in the header) when at least one field has a default."""
    lines = ["memset(data, 0, sizeof(*data));"]
    for f in struct.fields:
        if f.default is None:
            continue
        if f.kind == "string":
            lines.append(f'data->{f.name} = "{f.default}";')
        elif f.scalar_type == "bool":
            lines.append(f"data->{f.name} = {'true' if f.default else 'false'};")
        else:
            lines.append(f"data->{f.name} = {f.default!r};")
    return lines


def has_defaults(struct):
    return any(f.default is not None for f in struct.fields)


def needs_string_h(struct):
    """True if the generated .c actually calls something from <string.h> - emit_init() always
    memset()s when it's emitted at all (has_defaults), a string field's *_encode uses memcpy()
    (via _emit_string_encode), and any field needing runtime or non-zero static padding in front
    of it uses memset() (via _emit_pad, from _emit_align_encode). A struct with none of these
    (e.g. UInt64Data: one naturally-aligned scalar, no defaults, no strings) doesn't need the
    include at all - misc-include-cleaner flags it as unused if it's included unconditionally."""
    if has_defaults(struct):
        return True
    for f in struct.fields:
        if f.kind == "string":
            return True
        if f.kind == "array":
            # A fixed array of 1-byte elements always memcpy()s (see _emit_fixed_array_encode/
            # decode); a variable array always does too (element_size == 1) or memset()s its
            # count-to-element-align pad (element_align > 1) - one of those two is always true.
            if f.array_mode == "fixed" and f.element_size == 1:
                return True
            if f.array_mode == "variable":
                return True
    for plan in layout.plan_fields(struct.fields):
        if plan.field.wire_align > 1 and (plan.static_padding is None or plan.static_padding):
            return True
    return False


def needs_hal_h(struct):
    """True if the generated .c calls something from <tickle/hal.h> directly - the only two
    things it ever uses are _tt_bswap_16/32/64 (any multi-byte scalar, any string or variable
    array's own uint16 length/count prefix, or a multi-byte array element) and _tt_strnlen (any
    string field). A struct built entirely out of 1-byte scalars, fixed arrays of 1-byte
    elements, and/or nested fields (which delegate - see emit_nested_encode/decode - rather than
    calling _tt_bswap_* themselves) doesn't need it at all: geometry_msgs__Vector3 (three
    float64s) does, but examples/... Twist (two Vector3 *fields*, nothing scalar of its own)
    does not."""
    for f in struct.fields:
        if f.kind == "string":
            return True
        if f.kind == "scalar" and model.SCALAR_SIZE[f.scalar_type] > 1:
            return True
        if f.kind == "array" and (f.array_mode == "variable" or f.element_size > 1):
            return True
    return False


def needs_config_h(struct):
    """True if the generated .c references tt_MAX_STRING_LENGTH (from <tickle/config.h>) - only
    a string field's *_encode/_encode_size/_decode do (see _emit_string_encode et al.). A struct
    with no string fields (e.g. UInt64Data) doesn't need the include."""
    return any(f.kind == "string" for f in struct.fields)
