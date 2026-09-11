# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Turns a TopicIR / ServiceIR into (header_text, source_text). The per-struct pieces (struct.h.em
/ struct.c.em) are expanded once per WireStruct in plain Python and spliced into the outer
topic/service template as a text block, rather than nesting empy interpreters - simpler, and
keeps each struct's own render context (field names etc.) from leaking into its sibling's."""

import pathlib

import em

from . import emit, layout

_TEMPLATES = pathlib.Path(__file__).parent / "templates"


def _expand(template_name, **context):
    text = (_TEMPLATES / template_name).read_text(encoding="utf-8")
    return em.expand(text, **context)


def _nested_includes(struct):
    """The generated header for each of this struct's *own* nested fields (not recursed further -
    each of those headers already #includes whatever *it* nests, so the chain resolves the same
    way any C header dependency does)."""
    return sorted({f.nested.c_name for f in struct.fields if f.kind == "nested"})


def _struct_context(struct):
    layout.compute(struct)
    # An empty message (e.g. Trigger.srv's request) is fixed-size (wire_size 0) but still needs a
    # one-byte filler field to stay valid ISO C (see emit_struct_fields) - sizeof() is then 1, not
    # 0, so the sizeof/wire_size static_assert (and the *_encode_inplace/_decode_inplace this same
    # flag now also gates - DESIGN.md's "Struct layout" note) would be asserting something that
    # isn't actually a wire-layout invariant. Skip both rather than do that.
    is_fixed_size = struct.is_fixed_size and bool(struct.fields)
    return {
        "name": struct.c_name,
        "constant_lines": emit.emit_constants(struct),
        "field_lines": emit.emit_struct_fields(struct),
        "has_init": emit.has_defaults(struct),
        "init_lines": emit.emit_init(struct) if emit.has_defaults(struct) else [],
        "encode_size_lines": emit.emit_encode_size(struct),
        "encode_lines": emit.emit_encode(struct),
        "decode_lines": emit.emit_decode(struct),
        "encode_inplace_lines": emit.emit_encode_inplace(struct) if is_fixed_size else [],
        "decode_inplace_lines": emit.emit_decode_inplace(struct) if is_fixed_size else [],
        "free_lines": emit.emit_free(struct),
        "needs_string_h": emit.needs_string_h(struct),
        "needs_config_h": emit.needs_config_h(struct),
        "needs_hal_h": emit.needs_hal_h(struct),
        "nested_includes": _nested_includes(struct),
        "is_fixed_size": is_fixed_size,
        "wire_size": struct.wire_size,
        "max_wire_size": layout.max_wire_size(struct),
    }


def render_topic(topic_ir):
    ctx = _struct_context(topic_ir.data)
    header = _expand(
        "topic.h.em",
        name=topic_ir.name,
        data_name=topic_ir.data.c_name,
        data_struct_h=_expand("struct.h.em", **ctx),
        nested_includes=ctx["nested_includes"],
    )
    source = _expand(
        "topic.c.em",
        name=topic_ir.name,
        data_name=topic_ir.data.c_name,
        data_struct_c=_expand("struct.c.em", **ctx),
        needs_string_h=ctx["needs_string_h"],
        needs_config_h=ctx["needs_config_h"],
        needs_hal_h=ctx["needs_hal_h"],
        nested_includes=ctx["nested_includes"],
        data_is_fixed_size=ctx["is_fixed_size"],
    )
    return header, source


def render_service(service_ir):
    request_ctx = _struct_context(service_ir.request)
    response_ctx = _struct_context(service_ir.response)
    nested_includes = sorted(set(request_ctx["nested_includes"]) | set(response_ctx["nested_includes"]))
    header = _expand(
        "service.h.em",
        name=service_ir.name,
        request_name=service_ir.request.c_name,
        response_name=service_ir.response.c_name,
        request_struct_h=_expand("struct.h.em", **request_ctx),
        response_struct_h=_expand("struct.h.em", **response_ctx),
        nested_includes=nested_includes,
    )
    source = _expand(
        "service.c.em",
        name=service_ir.name,
        request_name=service_ir.request.c_name,
        response_name=service_ir.response.c_name,
        request_struct_c=_expand("struct.c.em", **request_ctx),
        response_struct_c=_expand("struct.c.em", **response_ctx),
        needs_string_h=request_ctx["needs_string_h"] or response_ctx["needs_string_h"],
        needs_config_h=request_ctx["needs_config_h"] or response_ctx["needs_config_h"],
        needs_hal_h=request_ctx["needs_hal_h"] or response_ctx["needs_hal_h"],
        nested_includes=nested_includes,
    )
    return header, source


def render_nested(struct):
    """A nested dependency (std_msgs__Header, say) gets its own <c_name>.h/.c pair - same struct
    content as a top-level interface's own data struct, but with no tt_Topic/tt_Service wrapper
    of its own (DESIGN.md: nesting has "no header" on the wire, and the same is true of its
    generated C - it's just the struct + codec functions a parent's #include reaches into)."""
    ctx = _struct_context(struct)
    header = _expand("nested.h.em", struct_h=_expand("struct.h.em", **ctx), nested_includes=ctx["nested_includes"])
    source = _expand(
        "nested.c.em",
        name=struct.c_name,
        struct_c=_expand("struct.c.em", **ctx),
        needs_string_h=ctx["needs_string_h"],
        needs_config_h=ctx["needs_config_h"],
        needs_hal_h=ctx["needs_hal_h"],
        nested_includes=ctx["nested_includes"],
    )
    return header, source
