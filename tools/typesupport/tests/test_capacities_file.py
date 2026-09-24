# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Capacity files (tickle_typesupport/capacities.py): sizing a package's unbounded fields from a
file beside it instead of annotations inside its .msg files, so upstream interface packages such as
std_msgs can be built for TickLE without being edited.

The package used throughout is std_msgs' MultiArray family in miniature - the real reason this
exists. MultiArrayLayout.dim is a non-trailing array of a type holding a string, and
Float32MultiArray.data trails a field that is not fixed-size, so neither can be auto-derived and
both need a capacity from somewhere.

Three properties matter more than the happy path, and each has its own test:
  * every row must land - a typo is an error, never a silent no-op;
  * a row that does not apply is a build error, not a decline;
  * a type nested from another package is sized by THAT package's installed file, so both sides
    agree on the layout.
"""

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parent.parent))

from tickle_typesupport import _rosidl_parser as rosidl  # noqa: E402
from tickle_typesupport import adapt, capacities, resolve, ros2_cli  # noqa: E402

DIMENSION = "string label\nuint32 size\nuint32 stride\n"
LAYOUT = "MultiArrayDimension[] dim\nuint32 data_offset\n"
FLOAT32_ARRAY = "MultiArrayLayout layout\nfloat32[] data\n"
PKG = "mini_msgs"

ROWS = f"""\
# comment line, and rows carrying the proposal TSV's rule and note columns
{PKG}/msg/MultiArrayLayout\tdim\t4\tstructural\tup to 4-D
{PKG}/msg/Float32MultiArray\tdata\t320\tpayload
"""


def _package(root, name=PKG, msgs=None, srvs=None):
    pkg = pathlib.Path(root) / name
    (pkg / "msg").mkdir(parents=True, exist_ok=True)
    for msg_name, text in (msgs or {}).items():
        (pkg / "msg" / f"{msg_name}.msg").write_text(text)
    if srvs:
        (pkg / "srv").mkdir(exist_ok=True)
        for srv_name, text in srvs.items():
            (pkg / "srv" / f"{srv_name}.srv").write_text(text)
    return pkg


def _mini(root):
    return _package(
        root,
        msgs={"MultiArrayDimension": DIMENSION, "MultiArrayLayout": LAYOUT, "Float32MultiArray": FLOAT32_ARRAY},
    )


def _adapt_message(pkg_dir, name, table, include_dirs=(), typesupport_packages=()):
    package = pkg_dir.name
    spec = rosidl.parse_message_string(package, name, (pkg_dir / "msg" / f"{name}.msg").read_text())
    resolver = resolve.Ros2Resolver(package, str(pkg_dir / "msg"), include_dirs, typesupport_packages, table)
    return adapt.adapt_message(name, spec, resolver, capacities.for_message(table, name)).data


def _field(struct, name):
    return next(f for f in struct.fields if f.name == name)


# --- parsing --------------------------------------------------------------------------------


def test_parse_reads_rows_and_ignores_extra_columns():
    table = capacities.parse(ROWS, PKG)
    assert table == {("msg", "MultiArrayLayout"): {"dim": 4}, ("msg", "Float32MultiArray"): {"data": 320}}


@pytest.mark.parametrize(
    "text, message",
    [
        ("other_msgs/msg/Foo data 4\n", "for package 'other_msgs'"),
        (f"{PKG}/Foo data 4\n", "is not <pkg>/<msg|srv>/<Type>"),
        (f"{PKG}/action/Foo data 4\n", "is not <pkg>/<msg|srv>/<Type>"),
        (f"{PKG}/msg/Foo data\n", "expected"),
        (f"{PKG}/msg/Foo data four\n", "not a whole number"),
        (f"{PKG}/msg/Foo data 0\n", "at least 1"),
        (f"{PKG}/msg/Foo data 4\n{PKG}/msg/Foo data 8\n", "listed twice"),
    ],
)
def test_parse_rejects_malformed_rows(text, message):
    with pytest.raises(capacities.CapacityError, match=message):
        capacities.parse(text, PKG)


# --- applying -------------------------------------------------------------------------------


def test_file_sizes_what_nothing_else_could(tmp_path):
    # Without a capacity, both types are ungenerable - this is the control for the test itself:
    # if it passed without the file, the file would not be what made it pass.
    pkg = _mini(tmp_path)
    with pytest.raises(adapt.UnsupportedFieldError):
        _adapt_message(pkg, "MultiArrayLayout", {})
    with pytest.raises(adapt.UnsupportedFieldError):
        _adapt_message(pkg, "Float32MultiArray", {})

    table = capacities.parse(ROWS, PKG)
    layout = _adapt_message(pkg, "MultiArrayLayout", table)
    assert (_field(layout, "dim").capacity, _field(layout, "dim").capacity_source) == (4, "file")
    data = _field(_adapt_message(pkg, "Float32MultiArray", table), "data")
    assert (data.capacity, data.capacity_source) == (320, "file")


def test_sibling_nested_type_uses_the_same_file(tmp_path):
    # Float32MultiArray nests MultiArrayLayout from its own package: the nested struct must carry
    # dim=4 from the file, the same as MultiArrayLayout generated on its own.
    pkg = _mini(tmp_path)
    struct = _adapt_message(pkg, "Float32MultiArray", capacities.parse(ROWS, PKG))
    assert _field(_field(struct, "layout").nested, "dim").capacity == 4


def test_file_outranks_an_annotation(tmp_path):
    pkg = _package(tmp_path, msgs={"Samples": "float32[] samples  # @capacity 16\n"})
    table = capacities.parse(f"{PKG}/msg/Samples samples 64\n", PKG)
    field = _field(_adapt_message(pkg, "Samples", table), "samples")
    assert (field.capacity, field.capacity_source) == (64, "file")
    assert _field(_adapt_message(pkg, "Samples", {}), "samples").capacity == 16  # annotation alone


def test_file_bounds_a_plain_string(tmp_path):
    pkg = _package(tmp_path, msgs={"Named": "string name\n"})
    field = _field(_adapt_message(pkg, "Named", capacities.parse(f"{PKG}/msg/Named name 32\n", PKG)), "name")
    assert (field.kind, field.capacity, field.capacity_source) == ("string", 32, "file")


@pytest.mark.parametrize(
    "definition, message",
    [
        ("float32[<=8] values\n", "bounded array"),
        ("float32[8] values\n", "fixed-size array"),
        ("string<=8 values\n", "bounded string"),
        ("float32 values\n", "neither an array nor a string"),
    ],
)
def test_file_refuses_a_field_that_already_has_a_size(tmp_path, definition, message):
    # Overriding a bound the interface author chose would change the type's meaning; ignoring the
    # row would leave the file lying about what it did. Either way the row is a mistake.
    pkg = _package(tmp_path, msgs={"Sized": definition})
    with pytest.raises(capacities.CapacityError, match=message):
        _adapt_message(pkg, "Sized", capacities.parse(f"{PKG}/msg/Sized values 4\n", PKG))


def test_row_for_a_missing_field_fails(tmp_path):
    pkg = _mini(tmp_path)
    table = capacities.parse(f"{PKG}/msg/MultiArrayLayout dims 4\n", PKG)  # typo: dims
    with pytest.raises(capacities.CapacityError, match="'dims'"):
        _adapt_message(pkg, "MultiArrayLayout", table)


def test_row_for_a_missing_type_fails(tmp_path):
    # Checked against the whole package, because a row for a type that does not exist would
    # otherwise be read by no generator run at all.
    pkg = _mini(tmp_path)
    table = capacities.parse(f"{PKG}/msg/Float32MultiArrays data 4\n", PKG)
    with pytest.raises(capacities.CapacityError, match="Float32MultiArrays"):
        capacities.check_types_exist(table, pkg)
    capacities.check_types_exist(capacities.parse(ROWS, PKG), pkg)  # the real rows are fine


# --- services -------------------------------------------------------------------------------

SET_MAP = "float32[] cells\nstring note\n---\nbool success\nfloat32[] cells_out\n"
BOTH = "float32[] values\n---\nfloat32[] values\n"


def test_service_row_goes_to_the_side_that_has_the_field():
    table = capacities.parse(f"{PKG}/srv/SetMap cells 8\n{PKG}/srv/SetMap cells_out 4\n", PKG)
    request, response = capacities.for_service(table, "SetMap", {"cells", "note"}, {"success", "cells_out"})
    assert (request, response) == ({"cells": 8}, {"cells_out": 4})


def test_service_row_that_is_ambiguous_must_say_which_side():
    table = capacities.parse(f"{PKG}/srv/Both values 8\n", PKG)
    with pytest.raises(capacities.CapacityError, match="both the request and the response"):
        capacities.for_service(table, "Both", {"values"}, {"values"})
    table = capacities.parse(f"{PKG}/srv/Both_Request values 8\n{PKG}/srv/Both_Response values 2\n", PKG)
    assert capacities.for_service(table, "Both", {"values"}, {"values"}) == ({"values": 8}, {"values": 2})


def test_service_suffixed_rows_pass_the_type_check(tmp_path):
    pkg = _package(tmp_path, srvs={"Both": BOTH})
    capacities.check_types_exist(capacities.parse(f"{PKG}/srv/Both_Request values 8\n", PKG), pkg)


# --- across packages ------------------------------------------------------------------------


def test_nested_type_from_another_package_uses_that_packages_installed_file(tmp_path):
    # The layout-consistency property. user_msgs nests mini_msgs/MultiArrayLayout. mini_msgs was
    # generated with dim=4, and that file is installed beside it; user_msgs' generator must read it
    # from there and arrive at the same capacity, because the two packages' structs meet in memory.
    share = tmp_path / "share"
    _mini(share)
    (share / PKG / capacities.INSTALLED_NAME).write_text(ROWS)
    user = _package(tmp_path / "src", "user_msgs", msgs={"Grid": f"{PKG}/MultiArrayLayout layout\n"})

    grid = _adapt_message(user, "Grid", {}, include_dirs=[str(share)], typesupport_packages=[PKG])
    assert _field(_field(grid, "layout").nested, "dim").capacity == 4

    # Control: without the installed file, the same resolution has no capacity to find.
    (share / PKG / capacities.INSTALLED_NAME).unlink()
    with pytest.raises(adapt.UnsupportedFieldError):
        _adapt_message(user, "Grid", {}, include_dirs=[str(share)], typesupport_packages=[PKG])


# --- through the generator entry point -------------------------------------------------------


def test_generate_declines_a_field_it_cannot_represent(tmp_path):
    # A wstring can never be sized, so the type is declined - stub files carrying the reason - and
    # the package build carries on, rather than one type failing it.
    pkg = _package(tmp_path, msgs={"Wide": "wstring text\n"})
    out = tmp_path / "out"
    written = ros2_cli.generate(PKG, "msg", "Wide", str(pkg / "msg" / "Wide.msg"), str(out))
    assert len(written) == 5
    header = (out / "Wide.h").read_text()
    assert "TickLE has no typesupport for mini_msgs/msg/Wide" in header
    assert "wstring" in header
    assert "get_message_type_support_handle" not in (out / f"{PKG}__msg__Wide__type_support.c").read_text()


def test_generate_declines_a_service_with_the_files_cmake_expects(tmp_path):
    pkg = _package(tmp_path, srvs={"Wide": "wstring text\n---\nbool ok\n"})
    out = tmp_path / "out"
    written = ros2_cli.generate(PKG, "srv", "Wide", str(pkg / "srv" / "Wide.srv"), str(out))
    names = sorted(pathlib.Path(p).name for p in written)
    ros_name = f"{PKG}__srv__Wide"
    assert names == sorted(
        ["Wide_srv.h", "Wide_srv.c", f"{ros_name}__type_support.c"]
        + [
            f"{ros_name}_{part}__{suffix}"
            for part in ("Request", "Response")
            for suffix in ("rosidl_typesupport_tickle_c.h", "rosidl_typesupport_tickle_c.c", "type_support.c")
        ]
    )


def test_generate_fails_rather_than_declines_on_a_bad_row(tmp_path):
    # The line between the two: a type TickLE cannot represent is declined, but a capacity file
    # that does not fit the package is the user's mistake and must stop the build.
    pkg = _mini(tmp_path)
    rows = tmp_path / "mini.capacities"
    rows.write_text(f"{PKG}/msg/MultiArrayLayout dims 4\n")
    with pytest.raises(capacities.CapacityError):
        ros2_cli.generate(
            PKG, "msg", "MultiArrayLayout", str(pkg / "msg" / "MultiArrayLayout.msg"), str(tmp_path / "out"),
            capacities_path=str(rows),
        )


def test_generate_applies_the_file(tmp_path):
    pkg = _mini(tmp_path)
    rows = tmp_path / "mini.capacities"
    rows.write_text(ROWS)
    out = tmp_path / "out"
    ros2_cli.generate(
        PKG, "msg", "Float32MultiArray", str(pkg / "msg" / "Float32MultiArray.msg"), str(out),
        capacities_path=str(rows),
    )
    header = (out / "Float32MultiArray.h").read_text()
    assert "data[320]" in header
