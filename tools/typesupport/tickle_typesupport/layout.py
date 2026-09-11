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


def _align_up(offset, alignment):
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
            aligned = _align_up(offset, wire_field.wire_align)
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
