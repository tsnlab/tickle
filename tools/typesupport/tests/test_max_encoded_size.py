# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""layout.max_encoded_size(): a real upper bound on a type's encoded payload, or None when the type
has none.

rmw_tickle reserves storage per retained sample and, without this, has to assume TickLE's whole
single-datagram ceiling for every type - about 12 MB for a depth-8192 KEEP_ALL DURABLE publisher,
whether it carries a 76-byte telemetry struct or a camera frame.

The distinction that matters, and the reason this isn't just layout.max_wire_size(): a plain
`string` field has no capacity to bound it. It is a char* aliasing external memory (DESIGN.md's
"Strings" rule), adapt.py deliberately doesn't auto-derive a capacity for it, and the runtime cap
is tt_MAX_STRING_LENGTH = 65535, forty-four times a datagram. max_wire_size() counts such a field
as its 2-byte length prefix - correct for the "fits in one datagram" _Static_assert it backs, and
an underestimate here. fixtures_own/Image.msg is the case that shows the difference plainly.
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent.parent))

from tickle_typesupport import _rosidl_parser as rosidl  # noqa: E402
from tickle_typesupport import adapt, layout, resolve  # noqa: E402

FIXTURES_OWN = pathlib.Path(__file__).parent / "fixtures_own"
FIXTURES_ROS2 = pathlib.Path(__file__).parent / "fixtures_ros2"
EXAMPLES = pathlib.Path(__file__).parent.parent.parent.parent / "examples"


def _struct(path, pkg, name):
    resolver = resolve.Resolver([str(FIXTURES_ROS2), str(FIXTURES_OWN)])
    spec = rosidl.parse_message_string(pkg, name, pathlib.Path(path).read_text())
    return adapt.adapt_message(name, spec, resolver).data


def _bound(path, pkg, name):
    return layout.max_encoded_size(_struct(path, pkg, name))


# --- the three shapes that cannot resolve, one test each -------------------------------------


def test_plain_string_has_no_bound():
    # BoundedString.msg carries all three forms side by side: `string<=8`, a `string` with an
    # explicit @capacity, and a plain `string`. The first two resolve; the third is what makes the
    # whole type unbounded, and the point of using this fixture is that two thirds of it resolving
    # must not be enough.
    assert _bound(FIXTURES_OWN / "BoundedString.msg", "fixtures_own", "BoundedString") is None


def test_string_element_array_has_no_bound():
    # An array of strings: adapt.py rejects `string<=8[]` outright rather than truncating, so a
    # string element is always plain and unbounded - the array's own capacity bounds how MANY
    # there are, never how long each one is.
    assert _bound(FIXTURES_OWN / "StringArrays.msg", "fixtures_own", "StringArrays") is None
    assert _bound(FIXTURES_OWN / "StringArrayDefaults.msg", "fixtures_own", "StringArrayDefaults") is None


def test_nested_type_propagates_unboundedness():
    # Stamped.msg's only interesting field is a std_msgs/Header, which carries a plain `string
    # frame_id` two levels down (Header -> builtin_interfaces/Time is the other branch). Nothing at
    # the top level of Stamped is unbounded, so this only comes out right if the recursion works.
    assert _bound(FIXTURES_OWN / "Stamped.msg", "fixtures_own", "Stamped") is None


def test_unbounded_string_is_not_its_two_byte_prefix():
    # The regression this function exists to prevent, stated as a test rather than a comment:
    # max_wire_size() returns a number for Image.msg that looks usable - it even fits in a datagram
    # - while the type has no bound at all. Reserving that figure per sample would silently lose
    # retention on every message with a longer `encoding` string.
    struct = _struct(FIXTURES_OWN / "Image.msg", "fixtures_own", "Image")
    assert layout.max_wire_size(struct) > 0
    assert layout.max_encoded_size(struct) is None


# --- types that do resolve -------------------------------------------------------------------


def test_fixed_and_bounded_fields_resolve():
    # Fixed arrays, a ROS 2 upper bound, and an @capacity annotation - all three capacity sources
    # in adapt.py's priority order, none of them a string.
    assert _bound(FIXTURES_OWN / "Arrays.msg", "fixtures_own", "Arrays") == 100
    assert _bound(FIXTURES_OWN / "ArrayDefaults.msg", "fixtures_own", "ArrayDefaults") == 14


def test_real_interfaces_resolve():
    # TickLE's own shipped interfaces, so the numbers below are what rmw_tickle would actually
    # reserve rather than fixture arithmetic. Bulk.msg is the auto-derived-capacity case and lands
    # just under the datagram ceiling by construction.
    assert _bound(EXAMPLES / "uint64" / "UInt64.msg", "uint64", "UInt64") == 8
    assert _bound(EXAMPLES / "perf" / "Bulk.msg", "perf", "Bulk") == 1444


def test_bound_never_understates_the_fixed_size_walk():
    # For a type with no unbounded field the two walks must agree: max_wire_size() and
    # max_encoded_size() differ only in how they treat a field that has no capacity, and by
    # definition such a type has none. A divergence here would mean one of them is miscounting
    # padding or a capacity, which no other test would catch.
    for path, pkg, name in (
        (FIXTURES_OWN / "Arrays.msg", "fixtures_own", "Arrays"),
        (FIXTURES_OWN / "ArrayDefaults.msg", "fixtures_own", "ArrayDefaults"),
        (EXAMPLES / "uint64" / "UInt64.msg", "uint64", "UInt64"),
        (EXAMPLES / "perf" / "Bulk.msg", "perf", "Bulk"),
    ):
        struct = _struct(path, pkg, name)
        assert layout.max_encoded_size(struct) == layout.max_wire_size(struct), name
