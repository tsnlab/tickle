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


def adapt_field(rosidl_field, resolver=None):
    field_type = rosidl_field.type

    if field_type.pkg_name is not None:
        if field_type.is_array:
            raise UnsupportedFieldError(
                f"field '{rosidl_field.name}': arrays of nested message types aren't supported"
            )
        if resolver is None:
            raise UnsupportedFieldError(
                f"field '{rosidl_field.name}': nested type '{field_type.pkg_name}/{field_type.type}'"
                " needs a resolver (pass -I search paths) - none was given"
            )
        if rosidl_field.default_value is not None:
            raise UnsupportedFieldError(
                f"field '{rosidl_field.name}': defaults on a nested message field aren't supported"
            )
        nested = resolver.resolve_struct(field_type.pkg_name, field_type.type, adapt_struct)
        return model.WireField(name=rosidl_field.name, kind="nested", nested=nested)
    if field_type.is_array:
        if field_type.type in STRING_TYPES:
            raise UnsupportedFieldError(
                f"field '{rosidl_field.name}': arrays of strings aren't supported"
            )
        if field_type.type not in model.SCALAR_SIZE:
            raise UnsupportedFieldError(
                f"field '{rosidl_field.name}': unknown array element type '{field_type.type}'"
            )
        # ROS 2 lets a .msg give an array field its own default (e.g. `uint8[4] x [1,2,3,4]`) -
        # a plain Python list, already scalar-typed, straight from rosidl. Bounds-checked against
        # array_size/capacity once every field's own capacity is fully known
        # (_validate_array_defaults, below) - an unbounded field's capacity may still be pending
        # auto-derivation at this point.
        default = rosidl_field.default_value
        if field_type.array_size is not None and not field_type.is_upper_bound:
            # Fixed: T[N] - no length prefix, N is part of the wire format itself.
            return model.WireField(
                name=rosidl_field.name,
                kind="array",
                scalar_type=field_type.type,
                array_mode="fixed",
                array_size=field_type.array_size,
                default=default,
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
                default=default,
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
            default=default,
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
    most one such field, it must be the struct's last field, and every field before it must be
    fully fixed-size (scalars, fixed arrays, or an all-fixed-size nested struct):
      - with more than one auto field, "the remaining budget" isn't well-defined (which one gets
        it?);
      - a non-trailing auto field would need to know its own worst-case size to plan every later
        field's offset;
      - a preceding string (or a nested struct that itself isn't fixed-size, e.g. one containing
        a string) has no useful worst-case bound to budget against: `tt_MAX_STRING_LENGTH`
        (65535) alone already exceeds `tt_MAX_BUFFER_LENGTH`, so treating it as the worst case
        would make auto-derivation fail even for a message whose strings are, in practice, only
        ever a few bytes long.
    None of these are needed by anything TickLE ships today (an unbounded bulk-payload array is
    always both last and preceded only by fixed-size fields, as examples/Bulk.msg and
    examples/perf/Bulk.c's own hand-written struct both are) - all three raise, asking for an
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
        if f.wire_size is None:
            raise UnsupportedFieldError(
                f"field '{target.name}': its capacity can't be auto-derived because a preceding "
                f"field ('{f.name}') isn't fixed-size (a string, or a nested type containing "
                "one) - add an explicit '# @capacity <N>' annotation instead"
            )
        offset = layout.align_up(offset, f.wire_align) + f.wire_size
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


def _validate_array_defaults(fields):
    """A fixed array's default must supply exactly array_size elements (there's no length
    prefix on the wire to fall back to - every element needs a real initial value); a variable
    array's default must fit within its (by now fully resolved, including auto-derived)
    capacity. Called after _resolve_auto_capacities so every field's capacity is final."""
    for f in fields:
        if f.kind != "array" or f.default is None:
            continue
        if f.array_mode == "fixed" and len(f.default) != f.array_size:
            raise UnsupportedFieldError(
                f"field '{f.name}': default has {len(f.default)} elements, but the array is "
                f"fixed-size {f.array_size} - a fixed array's default must supply exactly that many"
            )
        if f.array_mode == "variable" and len(f.default) > f.capacity:
            raise UnsupportedFieldError(
                f"field '{f.name}': default has {len(f.default)} elements, exceeding its "
                f"capacity {f.capacity}"
            )


def adapt_constant(rosidl_constant):
    return model.Constant(
        name=rosidl_constant.name,
        scalar_type=rosidl_constant.type,
        value=rosidl_constant.value,
    )


def adapt_struct(c_name, rosidl_spec, resolver=None):
    fields = [adapt_field(f, resolver) for f in rosidl_spec.fields]
    _resolve_auto_capacities(fields)
    _validate_array_defaults(fields)
    struct = model.WireStruct(
        c_name=c_name,
        fields=fields,
        constants=[adapt_constant(c) for c in rosidl_spec.constants],
    )
    # A nested field (adapt_field, above) reads .wire_size/.is_fixed_size off the nested
    # WireStruct it was resolved to - both need layout.compute() to have already run on it. Doing
    # that here (rather than leaving it to render.py, which also calls it - harmless, just
    # redundant - once this struct itself gets rendered) means it's already done by the time
    # resolve.Resolver caches this struct and any *other* field elsewhere nests the same type.
    layout.compute(struct)
    return struct


def adapt_message(name, rosidl_message_spec, resolver=None):
    """.msg -> TopicIR. `name` is the interface name (e.g. "UInt64"), independent of whatever
    package/message name rosidl needed to satisfy its own validation."""
    return model.TopicIR(name=name, data=adapt_struct(f"{name}Data", rosidl_message_spec, resolver))


def adapt_service(name, rosidl_service_spec, resolver=None):
    """.srv -> ServiceIR."""
    return model.ServiceIR(
        name=name,
        request=adapt_struct(f"{name}Request", rosidl_service_spec.request, resolver),
        response=adapt_struct(f"{name}Response", rosidl_service_spec.response, resolver),
    )
