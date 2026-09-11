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


def max_wire_size(struct):
    """Worst-case wire size in bytes, from what's actually knowable at generate time - backs the
    "message fits in one datagram" _Static_assert every generated struct.h.em carries (PLAN.md /
    DESIGN.md's "Capacity" rule), not just variable-size structs. A variable array contributes
    its resolved capacity, because that capacity becomes a real fixed-size C buffer inside the
    struct (see emit.emit_struct_fields) - an oversized one is a genuine compile-time-detectable
    mistake. A plain (M1) string contributes only its own 2-byte length prefix: unlike an array,
    it has no fixed C buffer at all (it's a `char*` aliasing external memory - DESIGN.md's
    "Strings" rule), so its true bound is the `len` its caller passes to *_encode/_decode at
    runtime, not something a compile-time assert here could meaningfully check. (DESIGN.md's own
    "Capacity" rule is scoped the same way - "a variable array's *or bounded string's* C buffer" -
    a bounded `string<=N` would get the array-like treatment too, but TickLE has no generated
    bounded-string field yet to exercise that against.)"""
    offset = 0
    for wire_field in struct.fields:
        offset = align_up(offset, wire_field.wire_align)
        if wire_field.kind == "scalar" or (wire_field.kind == "array" and wire_field.array_mode == "fixed"):
            offset += wire_field.wire_size
        elif wire_field.kind == "string":
            offset += model.STRING_LEN_SIZE
        elif wire_field.kind == "array":  # variable
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
    if not (last.kind == "array" and last.array_mode == "variable" and last.element_size == 1):
        return None
    if plan_fields(struct.fields)[-1].static_offset is None:
        return None
    return last
