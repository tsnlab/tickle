# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""rosidl MessageSpecification / ServiceSpecification -> our WireStruct / TopicIR / ServiceIR
(model.py). The only module that reads rosidl's own field/type objects - resolve.py (nested
types, M3) and layout.py/emit.py only ever see our own IR."""

import re

from . import layout, model

STRING_TYPES = {"string", "wstring"}
# A field's own trailing/leading comment can pin a variable array's capacity explicitly:
#   uint8[] bytes  # @capacity 1440
# See PLAN.md's capacity priority order (annotation > ROS 2 upper bound > auto-derived).
_CAPACITY_RE = re.compile(r"@capacity\s+(\d+)")


class UnsupportedFieldError(ValueError):
    """A field this milestone's generator doesn't handle yet (nested messages land in M3).
    Raised with enough context to point at exactly which field and why."""


def _annotation_capacity(rosidl_field):
    for line in rosidl_field.annotations.get("comment", None) or []:
        match = _CAPACITY_RE.search(line)
        if match:
            return int(match.group(1))
    return None


def adapt_field(rosidl_field):
    field_type = rosidl_field.type

    if field_type.pkg_name is not None:
        raise UnsupportedFieldError(
            f"field '{rosidl_field.name}': nested message types aren't supported yet (M3) - "
            f"{field_type.pkg_name}/{field_type.type}"
        )
    if field_type.is_array:
        if field_type.type in STRING_TYPES:
            raise UnsupportedFieldError(
                f"field '{rosidl_field.name}': arrays of strings aren't supported"
            )
        if field_type.type not in model.SCALAR_SIZE:
            raise UnsupportedFieldError(
                f"field '{rosidl_field.name}': unknown array element type '{field_type.type}'"
            )
        if rosidl_field.default_value is not None:
            # ROS 2 lets a .msg give an array field its own default (e.g. `uint8[4] x [1,2,3,4]`)
            # - out of scope until M6 (PLAN.md) rather than silently dropped, which would hide a
            # real default the .msg author actually asked for.
            raise UnsupportedFieldError(
                f"field '{rosidl_field.name}': array default values aren't supported yet (M6)"
            )
        if field_type.array_size is not None and not field_type.is_upper_bound:
            # Fixed: T[N] - no length prefix, N is part of the wire format itself.
            return model.WireField(
                name=rosidl_field.name,
                kind="array",
                scalar_type=field_type.type,
                array_mode="fixed",
                array_size=field_type.array_size,
            )
        if field_type.array_size is not None and field_type.is_upper_bound:
            # Bounded: T[<=N] - variable, ROS 2's own upper bound becomes the wire capacity.
            return model.WireField(
                name=rosidl_field.name,
                kind="array",
                scalar_type=field_type.type,
                array_mode="variable",
                capacity=field_type.array_size,
                capacity_source="bounded",
            )
        # Unbounded: T[] - variable, capacity comes from an explicit @capacity annotation if
        # present, else gets auto-derived once every field's own size is known (_resolve_
        # auto_capacities, below - needs the whole struct, not just this one field).
        capacity = _annotation_capacity(rosidl_field)
        return model.WireField(
            name=rosidl_field.name,
            kind="array",
            scalar_type=field_type.type,
            array_mode="variable",
            capacity=capacity,
            capacity_source="annotation" if capacity is not None else None,
        )
    if field_type.type in STRING_TYPES:
        if field_type.type == "wstring":
            raise UnsupportedFieldError(f"field '{rosidl_field.name}': wstring is out of scope")
        return model.WireField(
            name=rosidl_field.name,
            kind="string",
            default=rosidl_field.default_value,
        )
    if field_type.type not in model.SCALAR_SIZE:
        raise UnsupportedFieldError(f"field '{rosidl_field.name}': unknown type '{field_type.type}'")
    return model.WireField(
        name=rosidl_field.name,
        kind="scalar",
        scalar_type=field_type.type,
        default=rosidl_field.default_value,
    )


def _resolve_auto_capacities(fields):
    """Fills in .capacity for any variable array left without one (no ROS 2 upper bound, no
    @capacity annotation) - priority (3), "auto", from PLAN.md's capacity rule. Restricted to at
    most one such field, and it must be the struct's last field: with more than one, "the
    remaining budget" isn't well-defined (which one gets it?), and a non-trailing auto field
    would need to know its own worst-case size to plan every later field's offset - solvable, but
    not needed by anything TickLE ships today (an unbounded bulk-payload array is always last, as
    examples/perf/Bulk's own hand-written struct already is). Both cases raise, asking for an
    explicit @capacity instead of guessing.
    """
    needs_auto = [
        i
        for i, f in enumerate(fields)
        if f.kind == "array" and f.array_mode == "variable" and f.capacity is None
    ]
    if not needs_auto:
        return
    if len(needs_auto) > 1 or needs_auto[0] != len(fields) - 1:
        raise UnsupportedFieldError(
            "auto-derived capacity only supports a single trailing variable array with no "
            "ROS 2 upper bound - add a '# @capacity <N>' annotation to the others"
        )
    target = fields[-1]
    offset = 0
    for f in fields[:-1]:
        offset = layout.align_up(offset, f.wire_align)
        if f.kind == "string":
            offset += model.STRING_LEN_SIZE + model.TT_MAX_STRING_LENGTH
            offset = layout.align_up(offset, 4)
        else:  # scalar, or a fixed array - always a known size
            offset += f.wire_size
    offset = layout.align_up(offset, model.ARRAY_COUNT_ALIGN) + model.ARRAY_COUNT_SIZE
    offset = layout.align_up(offset, target.element_align)
    remaining = (model.TT_MAX_BUFFER_LENGTH - model.FRAMING_OVERHEAD) - offset
    capacity = remaining // target.element_size
    if capacity < 1:
        raise UnsupportedFieldError(
            f"field '{target.name}': no room left to auto-derive a capacity - "
            "the preceding fields already use the whole datagram budget"
        )
    target.capacity = capacity
    target.capacity_source = "auto"


def adapt_constant(rosidl_constant):
    return model.Constant(
        name=rosidl_constant.name,
        scalar_type=rosidl_constant.type,
        value=rosidl_constant.value,
    )


def adapt_struct(c_name, rosidl_spec):
    fields = [adapt_field(f) for f in rosidl_spec.fields]
    _resolve_auto_capacities(fields)
    return model.WireStruct(
        c_name=c_name,
        fields=fields,
        constants=[adapt_constant(c) for c in rosidl_spec.constants],
    )


def adapt_message(name, rosidl_message_spec):
    """.msg -> TopicIR. `name` is the interface name (e.g. "UInt64"), independent of whatever
    package/message name rosidl needed to satisfy its own validation."""
    return model.TopicIR(name=name, data=adapt_struct(f"{name}Data", rosidl_message_spec))


def adapt_service(name, rosidl_service_spec):
    """.srv -> ServiceIR."""
    return model.ServiceIR(
        name=name,
        request=adapt_struct(f"{name}Request", rosidl_service_spec.request),
        response=adapt_struct(f"{name}Response", rosidl_service_spec.response),
    )
