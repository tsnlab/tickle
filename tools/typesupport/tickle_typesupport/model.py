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

M3 scope adds nested messages (kind == "nested"): a field whose type is itself a WireStruct,
resolved (resolve.py) from a `-I` search path or builtins.py. DESIGN.md's "nested message" rule
- "the nested type's fields are inlined recursively at the current offset, no header, no extra
alignment beyond what the first nested field needs" - is exactly why wire_align/wire_size below
can just delegate to the nested WireStruct's own first field / overall size: nothing about a
nested field's position in its parent needs new alignment or padding rules of its own.
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
ARRAY_COUNT_SIZE = 2  # uint16 element count prefix for a variable array - same reasoning
ARRAY_COUNT_ALIGN = 2

# Mirrors include/tickle/config.h - not read from the header (same reasoning as SCALAR_SIZE etc.
# above: this is a hand-kept mirror of the wire spec, not a build-time dependency on the C
# headers). Used only for the "message fits in one datagram" _Static_assert every generated
# struct gets, and to auto-derive a variable array's capacity when nothing else specifies one.
TT_MAX_STRING_LENGTH = 65535
TT_MAX_BUFFER_LENGTH = 1472
# Smallest framing overhead any submessage carrying a payload has (a CALLREQUEST/CALLRESPONSE
# payload starts at offset 16, DATA's at 24 - see DESIGN.md's "Interface serialization") - used
# only as auto-capacity's safety margin, so an auto-derived array still leaves room for framing
# in the tightest (DATA) case. The _Static_assert itself checks the message alone against
# TT_MAX_BUFFER_LENGTH, per PLAN.md - this margin is not part of that check.
FRAMING_OVERHEAD = 24


@dataclass
class WireField:
    name: str
    kind: str  # "scalar" | "string" | "array" | "nested"
    scalar_type: str | None = None  # e.g. "uint32" - element type for "scalar" and "array" kinds
    default: object | None = None  # python-side default value, or None
    comment: str = ""  # the field's own trailing comment, for @capacity and readability
    # Only set when kind == "array":
    array_mode: str | None = None  # "fixed" (T[N]) | "variable" (T[] / T[<=N])
    array_size: int | None = None  # element count, when array_mode == "fixed"
    capacity: int | None = None  # max element count, when array_mode == "variable"
    capacity_source: str | None = None  # "annotation" | "bounded" | "auto" - docs/errors only
    # Only set when kind == "nested":
    nested: "WireStruct | None" = None  # the resolved nested type's own struct (resolve.py)

    @property
    def element_ctype(self):
        """Only meaningful for kind == "array" - the C type of one element."""
        return SCALAR_CTYPE[self.scalar_type]

    @property
    def element_size(self):
        return SCALAR_SIZE[self.scalar_type]

    @property
    def element_align(self):
        return SCALAR_ALIGN[self.scalar_type]

    @property
    def ctype(self):
        if self.kind == "scalar":
            return SCALAR_CTYPE[self.scalar_type]
        if self.kind == "string":
            return "char*"
        if self.kind == "array":
            # The declaration needs "elem name[N];" (and, for a variable array, a second
            # "uint16_t name_count;" member) - not expressible as one type string, so
            # emit.emit_struct_fields() special-cases kind == "array" rather than using this.
            return self.element_ctype
        if self.kind == "nested":
            return f"struct {self.nested.c_name}"
        raise NotImplementedError(self.kind)

    @property
    def wire_align(self):
        if self.kind == "scalar":
            return SCALAR_ALIGN[self.scalar_type]
        if self.kind == "string":
            return STRING_LEN_ALIGN
        if self.kind == "array":
            # A fixed array has no length prefix - its own start aligns to its element type,
            # same as a bare scalar would. A variable array's uint16 count prefix aligns to 2.
            return self.element_align if self.array_mode == "fixed" else ARRAY_COUNT_ALIGN
        if self.kind == "nested":
            # "No extra alignment beyond what the first nested field needs" (DESIGN.md) - an
            # empty nested struct (no TickLE interface actually has one) needs none of its own.
            return self.nested.fields[0].wire_align if self.nested.fields else 1
        raise NotImplementedError(self.kind)

    @property
    def wire_size(self):
        """Exact wire size in bytes, or None if it depends on runtime data (strings, variable
        arrays, or a nested message that itself contains either)."""
        if self.kind == "scalar":
            return SCALAR_SIZE[self.scalar_type]
        if self.kind == "array" and self.array_mode == "fixed":
            return self.array_size * self.element_size
        if self.kind == "nested":
            return self.nested.wire_size if self.nested.is_fixed_size else None
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
