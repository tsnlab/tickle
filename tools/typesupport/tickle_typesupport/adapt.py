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

from . import model

STRING_TYPES = {"string", "wstring"}


class UnsupportedFieldError(ValueError):
    """A field this milestone's generator doesn't handle yet (arrays land in M2, nested
    messages in M3). Raised with enough context to point at exactly which field and why."""


def adapt_field(rosidl_field):
    field_type = rosidl_field.type

    if field_type.is_array:
        raise UnsupportedFieldError(
            f"field '{rosidl_field.name}': arrays aren't supported yet (M2) - {field_type}"
        )
    if field_type.pkg_name is not None:
        raise UnsupportedFieldError(
            f"field '{rosidl_field.name}': nested message types aren't supported yet (M3) - "
            f"{field_type.pkg_name}/{field_type.type}"
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


def adapt_constant(rosidl_constant):
    return model.Constant(
        name=rosidl_constant.name,
        scalar_type=rosidl_constant.type,
        value=rosidl_constant.value,
    )


def adapt_struct(c_name, rosidl_spec):
    return model.WireStruct(
        c_name=c_name,
        fields=[adapt_field(f) for f in rosidl_spec.fields],
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
