# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""ros2_cli.generate()'s own sibling_dir computation for a *.srv* input (resolve.Ros2Resolver's
same-package sibling case, test_ros2_nested.py's own docstring) - a real, previously-broken case
test_ros2_nested.py's own fixtures never exercised, since its Leaf.msg/Branch.msg are both .msg
files sharing one flat directory. A real ROS 2 package keeps msg/ and srv/ as *separate*
directories (fixtures_own/srv_sibling_pkg/{msg/Leaf.msg, srv/UsesLeaf.srv} here, mirroring
rcl_interfaces' own SetParameters.srv nesting msg/Parameter) - ros2_cli.generate() used to take
its sibling_dir straight from os.path.dirname(input_path), which is srv/ for a .srv input, so a
.srv nesting a same-package .msg could never actually find it there and fell through to the -I
search path instead (returning None, then to resolve.Resolver's own builtin-fallback, which raises
resolve.UnresolvedTypeError for anything that isn't one of TickLE's two small bundled builtins -
Leaf is neither, so the failure mode was a hard, unmissable exception, not silently wrong output).
"""

import pathlib

from tickle_typesupport import ros2_cli

FIXTURES_OWN = pathlib.Path(__file__).parent / "fixtures_own"
PKG_DIR = FIXTURES_OWN / "srv_sibling_pkg"


def test_srv_resolves_same_package_msg_sibling(tmp_path):
    """No -I given at all: the only way this can succeed is via the same-package sibling lookup
    finding msg/Leaf.msg from srv/UsesLeaf.srv's own generate() call - before the fix, this raised
    resolve.UnresolvedTypeError instead."""
    written = ros2_cli.generate(
        "srv_sibling_pkg", "srv", "UsesLeaf", str(PKG_DIR / "srv" / "UsesLeaf.srv"), str(tmp_path)
    )
    assert written


def test_srv_sibling_reuses_leaf_not_a_second_copy(tmp_path):
    """Same "reuse, don't re-generate" contract test_ros2_nested.py's own
    test_ros2_resolver_writes_no_duplicate_leaf_file asserts for the .msg-from-.msg case - here for
    .msg-from-.srv: resolving UsesLeaf's own `Leaf leaf_value` request field must not write a
    second Leaf.h/.c of its own (that's Leaf.msg's own separate, independent generate() call's job
    alone - see resolve.Ros2Resolver's own class docstring)."""
    written = ros2_cli.generate(
        "srv_sibling_pkg", "srv", "UsesLeaf", str(PKG_DIR / "srv" / "UsesLeaf.srv"), str(tmp_path)
    )
    names = {pathlib.Path(p).name for p in written}
    assert "Leaf.h" not in names
    assert "Leaf.c" not in names
