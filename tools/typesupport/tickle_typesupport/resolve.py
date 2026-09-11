# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Finds and parses a nested message type's own `.msg` (`std_msgs/Header` referenced from inside
another interface, say) - either from a caller-supplied `-I` search path (ROS 2's own `pkg/msg/
Name.msg` layout) or, failing that, from builtins.py's small built-in fallback. adapt.py is the
only caller; kept separate so a future change to *how* dependencies are found (a real ament
package index, say) doesn't have to touch adapt.py's own field-adapting logic.
"""

import pathlib

from . import _rosidl_parser as rosidl
from . import builtins


class UnresolvedTypeError(ValueError):
    """A nested message type (`pkg/Name`) that is neither a TickLE builtin nor found on any
    `-I` search path."""


def _find_on_search_path(pkg_name, msg_name, include_dirs):
    for include_dir in include_dirs:
        candidate = pathlib.Path(include_dir) / pkg_name / "msg" / f"{msg_name}.msg"
        if candidate.is_file():
            return candidate.read_text(encoding="utf-8")
    return None


class Resolver:
    """Parses (and adapts, via adapt.adapt_struct) each distinct (pkg_name, msg_name) nested
    dependency exactly once, no matter how many fields - in one message, or across several
    messages generated together - reference it. That single shared model.WireStruct is what
    makes emit.py's/render.py's "generate this nested type's own <pkg>__<Name>.h/.c exactly once"
    logic possible: two fields nested on the same type must resolve to the *same* WireStruct
    object, not two separately-adapted copies of an identical shape (see cli.py, which uses
    `resolved_nested` to decide what else needs writing out alongside the top-level interface).
    """

    def __init__(self, include_dirs):
        self.include_dirs = list(include_dirs)
        self._specs = {}
        # In discovery order - cli.py writes each of these out once, after the top-level
        # interface, in exactly this order (so a dependency-of-a-dependency is written after the
        # thing that needed it first, which is only a readability nicety - order otherwise
        # doesn't matter, every file's content is independent of every other's).
        self.resolved_structs = {}
        self._resolve_order = []

    def resolve_spec(self, pkg_name, msg_name):
        key = (pkg_name, msg_name)
        if key in self._specs:
            return self._specs[key]
        text = _find_on_search_path(pkg_name, msg_name, self.include_dirs)
        if text is None:
            text = builtins.BUILTINS.get(key)
        if text is None:
            raise UnresolvedTypeError(
                f"nested type '{pkg_name}/{msg_name}' not found on any -I search path "
                f"({self.include_dirs or 'none given'}) and is not a built-in "
                f"({sorted('/'.join(k) for k in builtins.BUILTINS)})"
            )
        spec = rosidl.parse_message_string(pkg_name, msg_name, text)
        self._specs[key] = spec
        return spec

    def resolve_struct(self, pkg_name, msg_name, adapt_struct_fn):
        """adapt_struct_fn is adapt.adapt_struct, passed in rather than imported to avoid a
        resolve.py <-> adapt.py import cycle (adapt.py already imports this module)."""
        key = (pkg_name, msg_name)
        if key in self.resolved_structs:
            return self.resolved_structs[key]
        spec = self.resolve_spec(pkg_name, msg_name)
        struct = adapt_struct_fn(f"{pkg_name}__{msg_name}", spec, self)
        self.resolved_structs[key] = struct
        self._resolve_order.append(key)
        return struct

    def in_discovery_order(self):
        """(pkg_name, msg_name, struct) triples, in the order each was first resolved - lets a
        caller (cli.py) label each nested file's provenance banner with what it actually came
        from, rather than the top-level interface that happened to need it first."""
        return [(pkg_name, msg_name, self.resolved_structs[(pkg_name, msg_name)]) for pkg_name, msg_name in self._resolve_order]
