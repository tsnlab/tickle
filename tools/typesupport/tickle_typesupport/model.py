# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""The internal IR that adapt.py builds from a rosidl spec, layout.py annotates with CDR-4
sizing, and emit.py/the templates render into C. Kept separate from the vendored rosidl types
(model.WireField etc., not rosidl_parser.Field/Type) so a future parser swap only touches
adapt.py.

M1 scope: scalar and string fields only. Arrays and nested messages are added in M2/M3 -
WireField already carries the fields they'll need (array_size, is_upper_bound, nested) so
layout.py/emit.py don't have to be revisited structurally, just extended.
"""

from dataclasses import dataclass, field

# TickLE CDR-4 (DESIGN.md, "Interface serialization"): each primitive is aligned to
# min(sizeof, 4) - notably 8-byte types are 4-aligned, not 8. Keys match the strings rosidl's
# Type.type already normalizes to.
SCALAR_SIZE = {
    "bool": 1,
    "byte": 1,
    "char": 1,
    "int8": 1,
    "uint8": 1,
    "int16": 2,
    "uint16": 2,
    "int32": 4,
    "uint32": 4,
    "float32": 4,
    "int64": 8,
    "uint64": 8,
    "float64": 8,
}
SCALAR_ALIGN = {name: min(size, 4) for name, size in SCALAR_SIZE.items()}
SCALAR_CTYPE = {
    "bool": "bool",
    "byte": "uint8_t",
    "char": "char",
    "int8": "int8_t",
    "uint8": "uint8_t",
    "int16": "int16_t",
    "uint16": "uint16_t",
    "int32": "int32_t",
    "uint32": "uint32_t",
    "float32": "float",
    "int64": "int64_t",
    "uint64": "uint64_t",
    "float64": "double",
}
STRING_LEN_SIZE = 2  # uint16 length prefix (see CDR-4 spec: never needs to be wider)
STRING_LEN_ALIGN = 2


@dataclass
class WireField:
    name: str
    kind: str  # "scalar" | "string"  (M1; "array" | "nested" land in M2/M3)
    scalar_type: str | None = None  # e.g. "uint32" - set when kind == "scalar"
    default: object | None = None  # python-side default value, or None
    comment: str = ""  # the field's own trailing comment, for @capacity (M2) and readability

    @property
    def ctype(self):
        if self.kind == "scalar":
            return SCALAR_CTYPE[self.scalar_type]
        if self.kind == "string":
            return "char*"
        raise NotImplementedError(self.kind)

    @property
    def wire_align(self):
        if self.kind == "scalar":
            return SCALAR_ALIGN[self.scalar_type]
        if self.kind == "string":
            return STRING_LEN_ALIGN
        raise NotImplementedError(self.kind)

    @property
    def wire_size(self):
        """Exact wire size in bytes, or None if it depends on runtime data (strings)."""
        if self.kind == "scalar":
            return SCALAR_SIZE[self.scalar_type]
        return None


@dataclass
class Constant:
    name: str
    scalar_type: str
    value: object


@dataclass
class WireStruct:
    c_name: str  # e.g. "UInt64Data"
    fields: list[WireField] = field(default_factory=list)
    constants: list[Constant] = field(default_factory=list)
    # Filled in by layout.compute(): None until then, then True/False. A struct is fixed-size
    # when every field's wire_size is known at generate time (no strings/variable arrays/
    # variable-size nested messages) - see layout.py.
    is_fixed_size: bool | None = None
    wire_size: int | None = None  # exact size, only meaningful when is_fixed_size


@dataclass
class TopicIR:
    name: str  # e.g. "UInt64" -> struct UInt64Data, UInt64Topic
    data: WireStruct


@dataclass
class ServiceIR:
    name: str  # e.g. "SetBool" -> SetBoolRequest/Response, SetBoolService
    request: WireStruct
    response: WireStruct
