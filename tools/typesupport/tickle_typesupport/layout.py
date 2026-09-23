# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""CDR-4 layout (DESIGN.md, "Interface serialization"): walks a WireStruct's fields in order and
works out, for each one, whether its offset from the start of the payload is still knowable at
generate time.

It is, right up until the first variable-size field (a string; later, an array) - every field
before that has a fixed size, so the padding needed to align the next field is a compile-time
constant. From there on, an encoder/decoder can't know the offset without looking at the actual
data, so alignment has to be computed at runtime instead. emit.py uses this to pick the cheaper
form (a literal padding count) whenever it can, and only falls back to a runtime formula once
it has to.
"""

from dataclasses import dataclass

from . import model


def align_up(offset, alignment):
    return (offset + alignment - 1) & ~(alignment - 1)


@dataclass
class FieldPlan:
    field: model.WireField
    # The field's own offset from the payload start, if every field before it was fixed-size;
    # None once a variable-size field has already occurred (so the caller must align at runtime).
    static_offset: int | None
    # Bytes of padding needed before this field, IF static_offset is not None (a compile-time
    # constant in that case - 0 is common and means "no padding needed").
    static_padding: int | None


def plan_fields(fields):
    """[WireField] -> [FieldPlan], in the same order."""
    offset = 0
    plans = []
    for wire_field in fields:
        if offset is None:
            plans.append(FieldPlan(field=wire_field, static_offset=None, static_padding=None))
        else:
            aligned = align_up(offset, wire_field.wire_align)
            plans.append(
                FieldPlan(field=wire_field, static_offset=aligned, static_padding=aligned - offset)
            )
            offset = aligned
        if wire_field.wire_size is None:
            offset = None
        elif offset is not None:
            offset += wire_field.wire_size
    return plans


def compute(struct):
    """Fills in struct.is_fixed_size / struct.wire_size in place."""
    plans = plan_fields(struct.fields)
    if plans and plans[-1].static_offset is None:
        struct.is_fixed_size = False
        struct.wire_size = None
        return
    offset = 0
    for plan in plans:
        offset = plan.static_offset + plan.field.wire_size if plan.field.wire_size is not None else None
        if offset is None:
            struct.is_fixed_size = False
            struct.wire_size = None
            return
    struct.is_fixed_size = True
    struct.wire_size = offset if plans else 0


def padded_wire_size(struct):
    """`struct.wire_size` rounded up to the struct's own self-alignment (model.
    struct_self_align()) - what `sizeof(struct <c_name>)` actually is once the C compiler adds
    its own trailing padding, which `wire_size` itself deliberately never includes (it's the true
    minimal *wire* byte count - the value actually used for a top-level `_encode()`'s own return,
    for `encode_inplace`'s returned length, and for a nested array's own per-element stride).
    `None` when the struct isn't fixed-size (nothing to round). Exists purely to check `sizeof()`
    correctly in the generated `_Static_assert` (struct.h.em) - checking it against the smaller,
    unpadded `wire_size` instead would spuriously fail to compile for the (fully legitimate, not
    a bug) case where the struct's own last field doesn't happen to end at a multiple of the
    struct's own overall alignment (a `bool` then `int64` then `uint8` struct self-aligns to 4 but
    ends at byte 13, not a multiple of 4 - tests/fixtures_own's own OddAlign.msg is exactly this
    shape, found via the array-of-nested-type milestone that needed a fixture shaped like it)."""
    if not struct.is_fixed_size:
        return None
    return align_up(struct.wire_size, model.struct_self_align(struct))


def max_wire_size(struct):
    """Worst-case wire size in bytes, from what's actually knowable at generate time - backs the
    "message fits in one datagram" _Static_assert every generated struct.h.em carries (PLAN.md /
    DESIGN.md's "Capacity" rule), not just variable-size structs. A variable array contributes
    its resolved capacity, because that capacity becomes a real fixed-size C buffer inside the
    struct (see emit.emit_struct_fields) - an oversized one is a genuine compile-time-detectable
    mistake. A plain (M1) string contributes only its own 2-byte length prefix: unlike an array,
    it has no fixed C buffer at all (it's a `char*` aliasing external memory - DESIGN.md's
    "Strings" rule), so its true bound is the `len` its caller passes to *_encode/_decode at
    runtime, not something a compile-time assert here could meaningfully check. A *bounded*
    string (`string<=N`, or a plain string with an explicit @capacity annotation) DOES get the
    array-like treatment - DESIGN.md's "Capacity" rule is scoped as "a variable array's *or
    bounded string's* C buffer" - because it has a real fixed char[N+1] buffer (emit.
    emit_struct_fields), same reasoning as a variable array above."""
    offset = 0
    for wire_field in struct.fields:
        offset = align_up(offset, wire_field.wire_align)
        if wire_field.kind == "array" and wire_field.array_element_kind == "string":
            # A string array element has no fixed C buffer at all (same reasoning as a plain
            # unbounded string field, below) - only its own 2-byte length prefix contributes a
            # knowable worst case, whether the array's own element *count* is fixed (array_size)
            # or variable (capacity, plus its own 2-byte count prefix).
            if wire_field.array_mode == "fixed":
                offset += wire_field.array_size * model.STRING_LEN_SIZE
            else:
                offset += model.ARRAY_COUNT_SIZE + wire_field.capacity * model.STRING_LEN_SIZE
        elif wire_field.kind == "array" and wire_field.array_element_kind == "nested":
            # Recurse for one element's own worst case, then add the most padding a gap *before*
            # it could ever need (element_align - 1) - conservative, not the exact stride math
            # WireField.wire_size uses for the fixed-size case below, but this only backs a safety
            # _Static_assert, where overestimating is fine and underestimating never is.
            per_element = max_wire_size(wire_field.nested) + (wire_field.element_align - 1)
            if wire_field.array_mode == "fixed":
                offset += wire_field.array_size * per_element
            else:
                offset += model.ARRAY_COUNT_SIZE + wire_field.capacity * per_element
        elif wire_field.kind == "scalar" or (wire_field.kind == "array" and wire_field.array_mode == "fixed"):
            offset += wire_field.wire_size
        elif wire_field.kind == "string" and wire_field.capacity is not None:
            offset += model.STRING_LEN_SIZE + wire_field.capacity + 1
            offset = align_up(offset, 4)
        elif wire_field.kind == "string":
            offset += model.STRING_LEN_SIZE
        elif wire_field.kind == "array":  # variable, scalar elements
            offset += model.ARRAY_COUNT_SIZE
            offset = align_up(offset, wire_field.element_align)
            offset += wire_field.capacity * wire_field.element_size
        elif wire_field.kind == "nested":
            # Recurse: a nested struct's own worst case follows exactly the same rules (a string
            # inside it contributes only its prefix, an array inside it its resolved capacity,
            # and so on down through however many levels are nested) - see this function's own
            # docstring on why a string never contributes its full tt_MAX_STRING_LENGTH here.
            offset += max_wire_size(wire_field.nested)
        else:
            raise NotImplementedError(wire_field.kind)
    return offset


def max_encoded_size(struct):
    """Worst-case encoded payload in bytes, or None when the type has no such bound.

    Distinct from max_wire_size() above, and the difference is the whole point of this function.
    max_wire_size() answers "how large can the parts with a fixed C buffer get", which is what the
    generated "fits in one datagram" _Static_assert needs; it counts a plain string as just its
    2-byte length prefix, because a plain string has no fixed buffer - it's a char* aliasing
    external memory (DESIGN.md's "Strings" rule) and its real length is whatever the caller passes
    to _encode at runtime. That makes max_wire_size() an underestimate for such a type, which is
    fine for its own purpose and wrong for this one.

    This function refuses to guess instead. It returns a real upper bound when every variable-length
    field resolves to a capacity - annotation, ROS 2 upper bound, or auto-derived, in adapt.py's
    own priority order - and None when any of them doesn't. The three shapes that don't resolve:

      * a plain `string` with no capacity. adapt.py deliberately doesn't auto-derive one (see its
        own comment), and the runtime cap is tt_MAX_STRING_LENGTH = 65535, forty-four times a
        datagram, so it is no substitute for a bound.
      * an array whose elements are strings. Those elements are always plain and unbounded -
        adapt.py rejects `string<=8[]` rather than truncating or over-allocating - so the same
        applies per element.
      * a nested type that hits either of the above, at any depth.

    Callers use this to size storage per retained sample (rmw_tickle's KEEP_ALL arena), where
    under-reserving costs retention and guessing would cost it silently. None means "no bound
    exists below the datagram ceiling, use that ceiling" - not "unknown, pick something".

    Real types land on both sides: a fixed-layout telemetry message resolves to tens of bytes,
    while sensor_msgs/msg/Image and std_srvs/srv/SetBool carry a plain string and do not.
    """
    total = 0
    for wire_field in struct.fields:
        total = align_up(total, wire_field.wire_align)
        if wire_field.kind == "string":
            if wire_field.capacity is None:
                return None  # plain unbounded string - see this function's own docstring
            total += model.STRING_LEN_SIZE + wire_field.capacity + 1
            total = align_up(total, 4)
        elif wire_field.kind == "nested":
            nested_max = max_encoded_size(wire_field.nested)
            if nested_max is None:
                return None
            total += nested_max
        elif wire_field.kind == "array" and wire_field.array_element_kind == "string":
            return None  # string elements are always plain and unbounded (model.WireField)
        elif wire_field.kind == "array" and wire_field.array_element_kind == "nested":
            nested_max = max_encoded_size(wire_field.nested)
            if nested_max is None:
                return None
            # Same conservative per-element padding max_wire_size() uses: the exact stride depends
            # on where the element lands, and over-reserving here is safe where under-reserving
            # never is.
            per_element = nested_max + (wire_field.element_align - 1)
            if wire_field.array_mode == "fixed":
                total += wire_field.array_size * per_element
            else:
                total += model.ARRAY_COUNT_SIZE + wire_field.capacity * per_element
        elif wire_field.kind == "scalar" or (wire_field.kind == "array" and wire_field.array_mode == "fixed"):
            total += wire_field.wire_size
        elif wire_field.kind == "array":  # variable, scalar elements
            total += model.ARRAY_COUNT_SIZE
            total = align_up(total, wire_field.element_align)
            total += wire_field.capacity * wire_field.element_size
        else:
            raise NotImplementedError(wire_field.kind)
    return total


def prefix_array_field(struct):
    """The struct's own trailing field, IF it's a variable byte array (1-byte elements) that
    every other field precedes with a fixed size - the one common "fixed header + one trailing
    bulk-payload array" shape (examples/Bulk.msg's own `seq` + `payload`) where, once
    emit.emit_struct_fields() declares the array's uint16 count member *before* its data buffer
    (matching wire order), #pragma pack(4) makes the struct's own memory byte-identical to the
    wire bytes up through however many of the array's elements are actually in use - even though
    the struct as a whole isn't fixed-size (compute()'s is_fixed_size), so it doesn't qualify for
    the simpler whole-struct emit_encode_inplace/emit_decode_inplace. Backs emit.
    emit_prefix_encode_inplace/emit_prefix_decode_inplace. None if no field qualifies.

    Restricted to exactly this shape (byte elements, last field, everything before it fixed) the
    same way adapt._resolve_auto_capacities is: a 1-byte element means no padding can ever fall
    between the count and the data, and being last means nothing needs to follow it - both are
    what let *_encode_inplace hand back a single contiguous span with a length that isn't known
    until encode_size time, without also having to re-derive per-field alignment at runtime."""
    if not struct.fields:
        return None
    last = struct.fields[-1]
    if last.kind == "array" and last.array_element_kind in ("string", "nested"):
        # A string array's own C representation (an array of char* pointers) is never memory-
        # identical to its wire bytes (a length-prefixed byte run per element) regardless of
        # position - not eligible for this optimization at all. A nested array isn't either: even
        # when the nested type is fixed-size, an inter-element wire gap may be needed that its own
        # #pragma pack(4) C layout wouldn't reproduce (DESIGN.md's own note on this). Neither kind
        # has a last.element_size to check below either (see model.WireField.element_size's own
        # doc comment).
        return None
    if not (last.kind == "array" and last.array_mode == "variable" and last.element_size == 1):
        return None
    if plan_fields(struct.fields)[-1].static_offset is None:
        return None
    return last
