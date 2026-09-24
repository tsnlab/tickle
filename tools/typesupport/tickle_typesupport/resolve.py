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
        # ros2_adapter.py's own nested-field conversion needs the *real* ROS 2 (pkg, type) this
        # struct came from - independent of c_name's own "pkg__Name" convention here (see model.
        # WireStruct.ros_pkg_name's own doc comment for why c_name alone isn't enough any more).
        struct.ros_pkg_name = pkg_name
        struct.ros_type_name = msg_name
        self.resolved_structs[key] = struct
        self._resolve_order.append(key)
        return struct

    def in_discovery_order(self):
        """(pkg_name, msg_name, struct) triples, in the order each was first resolved - lets a
        caller (cli.py) label each nested file's provenance banner with what it actually came
        from, rather than the top-level interface that happened to need it first."""
        return [(pkg_name, msg_name, self.resolved_structs[(pkg_name, msg_name)]) for pkg_name, msg_name in self._resolve_order]


class UnsupportedNestedPackage(Exception):
    """A message nests a type from another package that TickLE has no typesupport for.

    Carries the package and type so the caller can say which one, rather than emitting a generic
    "no typesupport" that sends people looking in the wrong place - the two reasons a type can be
    undeliverable (too large for a datagram, versus nesting an unsupported package) have entirely
    different remedies.
    """

    def __init__(self, pkg_name, msg_name):
        super().__init__(f"{pkg_name}/{msg_name} has no TickLE typesupport and is not one of TickLE's bundled builtins")
        self.pkg_name = pkg_name
        self.msg_name = msg_name


class Ros2Resolver:
    """ros2_cli.py's own resolver - genuinely different from Resolver above, not just a thin
    wrapper: every message a real ROS 2 package's own rosidl_typesupport_tickle_c CMake extension
    might resolve as a nested field is *also* independently generated as its own top-level
    interface, by that exact same extension's own per-.msg invocation - either a sibling .msg in
    this same package (test_msgs/msg/Nested.msg's own `BasicTypes basic_types_value`, the common
    case: ROS 2's own grammar resolves an unqualified nested type name to the *current* package,
    tickle_typesupport._rosidl_parser's own Type.__init__ already handles this, nothing extra
    needed here to detect it), or another package's, if that one too builds rosidl_typesupport_
    tickle_c. That independent generation uses adapt_message()'s own "<Name>Data" c_name
    convention (not this module's own Resolver.resolve_struct()'s "pkg__Name" one) - so reusing
    it, rather than writing a second, differently-named, *incompatible* copy of an equivalent
    struct via render_nested(), is the entire point of this class: resolve_struct() never writes
    anything for what it resolves this way, it only returns a WireStruct shaped exactly like that
    independent generation already produces (right down to c_name/header_name), for adapt.py's own
    field-adapting and ros2_adapter.py's own converter-generation to reference as if it already
    existed - because, by the time anything actually links, it will (a completely separate
    add_custom_command() in the exact same CMakeLists.txt build, per rosidl_typesupport_tickle_c_
    generate_interfaces.cmake's own foreach() over every .msg in the package).

    TickLE's own two small bundled builtins (std_msgs/Header, builtin_interfaces/Time -
    builtins.py) are the one exception: nothing else ever independently generates "HeaderData"/
    "TimeData" anywhere, so those still need the *old* "pkg__Name" convention and a real
    render_nested()-written file - delegated to a plain Resolver instance for exactly that reason,
    the same and only thing this class's own in_discovery_order() ever reports.
    """

    def __init__(self, package_name, sibling_dir, include_dirs=(), typesupport_packages=()):
        self.package_name = package_name
        self.sibling_dir = sibling_dir
        self.include_dirs = list(include_dirs)
        # Packages that build TickLE typesupport of their own, as determined by CMake (which can
        # see their exported target) and passed in. Nothing visible from here distinguishes such a
        # package from one that merely has a .msg on the search path.
        self.typesupport_packages = set(typesupport_packages)
        self.resolved_structs = {}
        self._builtin_fallback = Resolver(include_dirs)

    def _find_independent_source(self, pkg_name, msg_name):
        if pkg_name == self.package_name:
            candidate = pathlib.Path(self.sibling_dir) / f"{msg_name}.msg"
            if candidate.is_file():
                return candidate.read_text(encoding="utf-8")
        return _find_on_search_path(pkg_name, msg_name, self.include_dirs)

    def resolve_struct(self, pkg_name, msg_name, adapt_struct_fn):
        key = (pkg_name, msg_name)
        if key in self.resolved_structs:
            return self.resolved_structs[key]

        # Any nested type from another package is declined in the ROS 2 path, and the rule is
        # deliberately broader than "TickLE has no struct for it" (2026-09-24).
        #
        # Three layers had to be peeled before this was the right line to draw. std_msgs/Header is
        # one of TickLE's own bundled builtins, so emitting the *struct* for it is easy - and doing
        # that still does not work, because ros2_adapter.py also needs std_msgs' own
        # ...__rosidl_typesupport_tickle_c.h to convert the ROS C struct into it, and that file
        # exists only if std_msgs itself builds this typesupport. It does not; it ships FastDDS
        # typesupport of its own and nothing else.
        #
        # Inlining the nested type instead is not available either, and it is worth recording so
        # it does not get proposed again: two packages that both nest std_msgs/Header would each
        # define `struct HeaderData` and `HeaderData_encode`, and a ROS 2 executable routinely
        # links both. That is a duplicate-symbol link failure, not a path collision, so writing
        # the files into per-package directories does not contain it.
        #
        # The real fix is the one FastDDS uses - the interface package ships its own typesupport -
        # which means building std_msgs, sensor_msgs and anything else a consumer nests from
        # source with this generator applied. That is a provisioning change, not a generator one.
        #
        # KNOWN LIMITATION, stated rather than silently wrong: a package that DOES build this
        # typesupport in the same workspace is declined here too, because nothing available at
        # generation time distinguishes it from one that does not. The decline says which package
        # it was, so a reader who knows better can tell immediately that this is the case they hit.
        if pkg_name != self.package_name and pkg_name not in self.typesupport_packages:
            raise UnsupportedNestedPackage(pkg_name, msg_name)

        text = self._find_independent_source(pkg_name, msg_name)

        if text is None:
            # Not this package's own sibling, not on any -I search path either - fall back to
            # TickLE's own small bundled builtins, same as Resolver's own resolve_spec() does.
            # Unlike the case above, *this* does need a real file written via render_nested() -
            # in_discovery_order() below reports it for exactly that reason.
            struct = self._builtin_fallback.resolve_struct(pkg_name, msg_name, adapt_struct_fn)
            self.resolved_structs[key] = struct
            return struct
        spec = rosidl.parse_message_string(pkg_name, msg_name, text)
        struct = adapt_struct_fn(f"{msg_name}Data", spec, self)
        struct.header_name = f"{msg_name}.h"
        struct.ros_pkg_name = pkg_name
        struct.ros_type_name = msg_name
        self.resolved_structs[key] = struct
        return struct

    def in_discovery_order(self):
        """Only ever the builtin_fallback's own resolutions - see this class's own doc comment
        for why every other kind of resolution here needs no file of its own written at all."""
        return self._builtin_fallback.in_discovery_order()
