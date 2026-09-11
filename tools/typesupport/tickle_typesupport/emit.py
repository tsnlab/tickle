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
    x86-64 and the riscv64-unknown-elf cross compiler; see PLAN.md)."""
    if not struct.fields:
        # An empty struct is a GNU extension (see tt_Request's own note in tickle.h) - a message
        # with no fields (e.g. Trigger.srv's request) still needs one byte to stay valid ISO C.
        # It is not a wire field: nothing ever reads or writes it.
        return ["uint8_t reserved; // empty message - not serialized, just fills the struct"]
    return [f"{field.ctype} {field.name};" for field in struct.fields]


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
        else:
            raise NotImplementedError(plan.field.kind)
    lines.append("return size;")
    return lines


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
    for plan in layout.plan_fields(struct.fields):
        if plan.field.wire_align > 1 and (plan.static_padding is None or plan.static_padding):
            return True
    return False


def needs_config_h(struct):
    """True if the generated .c references tt_MAX_STRING_LENGTH (from <tickle/config.h>) - only
    a string field's *_encode/_encode_size/_decode do (see _emit_string_encode et al.). A struct
    with no string fields (e.g. UInt64Data) doesn't need the include."""
    return any(f.kind == "string" for f in struct.fields)
