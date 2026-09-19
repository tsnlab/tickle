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
# payload starts at offset 16, DATA's at 28 - see DESIGN.md's "Interface serialization") - used
# only as auto-capacity's safety margin, so an auto-derived array still leaves room for framing
# in the tightest (DATA) case. The _Static_assert itself checks the message alone against
# TT_MAX_BUFFER_LENGTH, per PLAN.md - this margin is not part of that check. Bumped 24 -> 28 for
# TickLE core's own Milestone 47 (rmw_tickle/PLAN.md): tt_DataHeader grew 16 -> 20 bytes for its
# new entity_id field, a real wire-format change this Python-side mirror has to track by hand
# (not read from the C headers - see this file's own module docstring / TT_MAX_BUFFER_LENGTH's
# comment above for why). Missing this bump doesn't fail generation or its own drift-check (the
# regenerated output stays internally consistent with itself either way) - it only surfaces as a
# real, oversized DATA packet at runtime, caught by CI's own "Integration - Linux HAL over netns"
# job (`Illegal submessage length: 1472 < 4 || 1472 > 1468`), not by the "Check all" typesupport
# regen-and-diff step.
FRAMING_OVERHEAD = 28


def struct_self_align(struct):
    """The alignment the C compiler actually places a `struct <struct.c_name>` MEMBER (or array
    element) at inside another `#pragma pack(push, 4)` struct - the type's own overall/self
    alignment under that same pragma: `max()` over its own fields' `wire_align` (each already
    `min(natural, 4)`), matching how a struct's own alignment-as-a-type is itself capped at 4 the
    same way each of its members already is. **Not** "the first field's own alignment" - that's
    only what a *single* nested field's own internal layout needs, starting from wherever it
    begins (DESIGN.md's own "Nested messages" rule), a strictly smaller requirement in general
    that happens to coincide with self-alignment for every nested type this codebase has used so
    far as a nested field (Time/Header/Vector3, each with its own largest-aligned field first) -
    not true for test_msgs' own BasicTypes (starts with `bool`, alignment 1, but self-aligns to
    4). Used for a single nested field's own `wire_align` (conservative, but keeps its C member
    placement identical to where the compiler would put it either way - see DESIGN.md), a nested
    array element's own `element_align` (not conservative there, load-bearing: a nested array's
    own C memory layout genuinely does follow this stride, not the smaller one, so this file's
    own `_Static_assert(sizeof/offsetof …)` safety net would simply fail to compile if it didn't),
    and - for *any* fixed-size struct, nested or not - layout.padded_wire_size()'s own trailing-
    padding computation (the C compiler always rounds a struct's own `sizeof()` up to a multiple
    of this same self-alignment, exactly the way it rounds up array-of-it strides too)."""
    return max((f.wire_align for f in struct.fields), default=1)


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
    # "scalar" (scalar_type names the element type, as always), "string" (every element is a
    # plain, unbounded string - `scalar_type` stays None; a *bounded* string element, e.g.
    # `string<=8[]`, isn't supported yet - adapt.py raises for it rather than silently truncating
    # or over-allocating), or "nested" (every element is the resolved struct `nested` below points
    # at - `scalar_type` stays None). DESIGN.md's "Variable/Fixed arrays" rule composes directly
    # with its own "Strings"/"Nested messages" rules for these two cases - see emit.py's own
    # dedicated `_emit_*_string_array_*`/`_emit_*_nested_array_*` functions, kept separate from
    # the scalar-element ones rather than unified, since neither a string nor a nested element has
    # a fixed per-element wire size known at generate time the way a scalar element always does.
    array_element_kind: str = "scalar"
    # The resolved nested type's own struct (resolve.py) - set whenever kind == "nested", OR
    # kind == "array" and array_element_kind == "nested" (one field, two different uses of the
    # same slot rather than a separate "element_nested" name, since a field is never both at once).
    nested: "WireStruct | None" = None

    @property
    def element_ctype(self):
        """Only meaningful for kind == "array" - the C type of one element."""
        if self.array_element_kind == "string":
            return "char*"
        if self.array_element_kind == "nested":
            return f"struct {self.nested.c_name}"
        return SCALAR_CTYPE[self.scalar_type]

    @property
    def element_size(self):
        """Only meaningful for kind == "array" and array_element_kind == "scalar" - a string or
        nested element has no fixed per-element wire size known at generate time (see wire_size/
        wire_align below, and layout.max_wire_size(), which all route around ever calling this for
        either)."""
        return SCALAR_SIZE[self.scalar_type]

    @property
    def element_align(self):
        if self.array_element_kind == "string":
            return STRING_LEN_ALIGN
        if self.array_element_kind == "nested":
            # Same formula as a single top-level nested field's own wire_align, below - see
            # struct_self_align()'s own doc comment for why this is the type's own overall
            # self-alignment, not just its first field's, and why that's not necessarily a
            # divisor of the type's own wire size (DESIGN.md's own note on why a nested array,
            # unlike a scalar one, may need a gap *between* elements too).
            return struct_self_align(self.nested)
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
            # same as a bare scalar would (a string element's own align is STRING_LEN_ALIGN, same
            # value as ARRAY_COUNT_ALIGN, so this formula needs no extra branch for it). A
            # variable array's uint16 count prefix aligns to 2.
            return self.element_align if self.array_mode == "fixed" else ARRAY_COUNT_ALIGN
        if self.kind == "nested":
            return struct_self_align(self.nested)
        raise NotImplementedError(self.kind)

    @property
    def wire_size(self):
        """Exact wire size in bytes, or None if it depends on runtime data (strings, variable
        arrays, a string array of either mode - each element's own length varies at runtime even
        when the array's own element *count* is fixed - a nested message that itself contains any
        of these, or an array of a nested message that itself isn't fixed-size)."""
        if self.kind == "scalar":
            return SCALAR_SIZE[self.scalar_type]
        if self.kind == "array" and self.array_mode == "fixed" and self.array_element_kind == "scalar":
            return self.array_size * self.element_size
        if self.kind == "array" and self.array_mode == "fixed" and self.array_element_kind == "nested":
            # Unlike a self-aligned scalar element, a fixed-size nested element's own wire_size
            # isn't necessarily a multiple of its own alignment - each element after the first may
            # need a gap before it (DESIGN.md's own note), so N elements take (N-1) full strides
            # (element rounded up to its own alignment) plus one final unpadded element.
            if not self.nested.is_fixed_size:
                return None
            if self.array_size == 0:
                return 0
            align = self.element_align
            stride = (self.nested.wire_size + align - 1) & ~(align - 1)
            return (self.array_size - 1) * stride + self.nested.wire_size
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
    # Only set for a struct resolve.py resolved as someone else's nested field (never for a
    # struct adapt_message()/adapt_service() built directly as a top-level interface). header_name
    # is the file a `#include` for it should name - None means "the same as c_name.h", true for
    # every nested dependency resolve.Resolver itself writes out (render_nested(), c_name IS the
    # file name) but not for one resolve.Ros2Resolver resolves to an *already independently
    # generated* real ROS 2 sibling message (c_name there is that message's own "<Name>Data",
    # adapt_message()'s own convention, but the file is still just "<Name>.h" - see Ros2Resolver's
    # own doc comment for why reusing that file, not writing a second one, is the whole point).
    header_name: str | None = None
    # ros_pkg_name/ros_type_name: the *original* ROS 2 (package, message) this struct was resolved
    # from, when known - independent of whatever naming convention c_name itself follows (see
    # header_name's own comment - c_name alone is no longer enough to recover this, now that two
    # different conventions exist). ros2_adapter.py's own nested-field conversion code needs this
    # to find the *other* message's own already-generated __to_tickle/__from_tickle functions,
    # which are always named from the real ROS 2 (pkg, type), never from c_name.
    ros_pkg_name: str | None = None
    ros_type_name: str | None = None
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
