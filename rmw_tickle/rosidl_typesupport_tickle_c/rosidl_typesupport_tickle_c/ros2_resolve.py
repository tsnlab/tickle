# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Nested-type resolution for a real ROS 2 package build - ros2_cli's resolver.

Moved out of tickle_typesupport/resolve.py (2026-09-24, user decision: every ROS-related part
lives in rmw_tickle; TickLE core keeps no ROS dependency). resolve.Resolver, the ROS-agnostic
resolver TickLE's own codegen uses, stays there; this one builds on it.
"""

import pathlib

from tickle_typesupport import _rosidl_parser as rosidl
from tickle_typesupport import capacities as capacity_file
from tickle_typesupport.resolve import Resolver, find_on_search_path

from . import builtins


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

    def __init__(self, package_name, sibling_dir, include_dirs=(), typesupport_packages=(), capacities=None):
        self.package_name = package_name
        self.sibling_dir = sibling_dir
        self.include_dirs = list(include_dirs)
        # Capacity tables by package (capacities.py). This package's own comes from the caller
        # (--capacities); another package's is read from where it was installed, so a type nested
        # from it gets exactly the capacities it was generated with - the layout has to match.
        self._capacity_tables = {package_name: capacities or {}}
        # Packages that build TickLE typesupport of their own, as determined by CMake (which can
        # see their exported target) and passed in. Nothing visible from here distinguishes such a
        # package from one that merely has a .msg on the search path.
        self.typesupport_packages = set(typesupport_packages)
        self.resolved_structs = {}
        self._builtin_fallback = Resolver(include_dirs, builtins.BUILTINS)
        # An action's own implicit messages (<A>_Goal, <A>_Result, <A>_Feedback), which the
        # wrappers derived from the same .action nest: {type name: .msg text}. They live under
        # "action", not "msg", so they are found here rather than as sibling files -
        # add_action_local() (ros2_cli.py's action path) registers them.
        self._action_local = {}

    def add_action_local(self, type_name, text):
        self._action_local[type_name] = text

    def _find_independent_source(self, pkg_name, msg_name):
        if pkg_name == self.package_name:
            candidate = pathlib.Path(self.sibling_dir) / f"{msg_name}.msg"
            if candidate.is_file():
                return candidate.read_text(encoding="utf-8")
        return find_on_search_path(pkg_name, msg_name, self.include_dirs)

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

        if pkg_name == self.package_name and msg_name in self._action_local:
            spec = rosidl.parse_message_string(pkg_name, msg_name, self._action_local[msg_name])
            struct = adapt_struct_fn(f"{msg_name}Data", spec, self, self._capacities_for(pkg_name, msg_name, "action"))
            struct.header_name = f"{msg_name}.h"
            struct.origin = (pkg_name, "action", msg_name)
            self.resolved_structs[key] = struct
            return struct

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
        struct = adapt_struct_fn(f"{msg_name}Data", spec, self, self._capacities_for(pkg_name, msg_name))
        struct.header_name = f"{msg_name}.h"
        struct.origin = (pkg_name, "msg", msg_name)
        self.resolved_structs[key] = struct
        return struct

    def _capacities_for(self, pkg_name, msg_name, subfolder="msg"):
        if pkg_name not in self._capacity_tables:
            self._capacity_tables[pkg_name] = capacity_file.find_installed(pkg_name, self.include_dirs)
        return capacity_file.for_message(self._capacity_tables[pkg_name], msg_name, subfolder)

    def in_discovery_order(self):
        """Only ever the builtin_fallback's own resolutions - see this class's own doc comment
        for why every other kind of resolution here needs no file of its own written at all."""
        return self._builtin_fallback.in_discovery_order()
