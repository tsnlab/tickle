# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Capacity files through the ROS half of the generator: a nested type is sized by its own
package's file, and ros2_cli's decline-versus-fail line. The file format and per-field rules are
tested with capacities.py itself, in tools/typesupport/tests/test_capacities_file.py.

The package used throughout is std_msgs' MultiArray family in miniature - the real reason this
exists. MultiArrayLayout.dim is a non-trailing array of a type holding a string, and
Float32MultiArray.data trails a field that is not fixed-size, so neither can be auto-derived and
both need a capacity from somewhere.
"""

import pathlib

import pytest
from rosidl_typesupport_tickle_c import ros2_cli, ros2_resolve
from tickle_typesupport import _rosidl_parser as rosidl
from tickle_typesupport import adapt, capacities

DIMENSION = "string label\nuint32 size\nuint32 stride\n"
LAYOUT = "MultiArrayDimension[] dim\nuint32 data_offset\n"
FLOAT32_ARRAY = "MultiArrayLayout layout\nfloat32[] data\n"
PKG = "mini_msgs"

ROWS = f"""\
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
    resolver = ros2_resolve.Ros2Resolver(package, str(pkg_dir / "msg"), include_dirs, typesupport_packages, table)
    return adapt.adapt_message(name, spec, resolver, capacities.for_message(table, name)).data


def _field(struct, name):
    return next(f for f in struct.fields if f.name == name)


# --- within one package ---------------------------------------------------------------------


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
    assert len(written) == 7  # the C set, and the C++ converter pair rosidl_typesupport_tickle_cpp compiles
    header = (out / "Wide.h").read_text()
    assert "TickLE has no typesupport for mini_msgs/msg/Wide" in header
    assert "wstring" in header
    assert "get_message_type_support_handle" not in (out / f"{PKG}__msg__Wide__type_support.c").read_text()
    # What the C++ type support wrapper tests to hand rclcpp no handle, rather than one calling C
    # converters that were never generated.
    cpp_header = (out / f"{PKG}__msg__Wide__rosidl_typesupport_tickle_cpp.hpp").read_text()
    assert f"#define {PKG}__msg__Wide__TICKLE_UNSUPPORTED 1" in cpp_header


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
            for suffix in (
                "rosidl_typesupport_tickle_c.h",
                "rosidl_typesupport_tickle_c.c",
                "type_support.c",
                "rosidl_typesupport_tickle_cpp.hpp",
                "rosidl_typesupport_tickle_cpp.cpp",
            )
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


def test_type_support_records_the_buffer_length_it_was_generated_for(tmp_path):
    # rmw_tickle refuses a type generated for a different tt_MAX_BUFFER_LENGTH, which only works if
    # the generated type support says which one it was: through the real CLI entry point, as the
    # CMake hook runs it.
    pkg = _mini(tmp_path)
    rows = tmp_path / "mini.capacities"
    rows.write_text(ROWS)
    out = tmp_path / "out"
    try:
        assert 0 == ros2_cli.main([
            "--package", PKG, "--subfolder", "msg", "--name", "Float32MultiArray",
            "--input", str(pkg / "msg" / "Float32MultiArray.msg"), "--outdir", str(out),
            "--capacities", str(rows), "--max-buffer-length", "4096",
        ])
    finally:
        from tickle_typesupport import model

        model.set_max_buffer_length(model.TT_MAX_BUFFER_LENGTH)
    source = (out / f"{PKG}__msg__Float32MultiArray__type_support.c").read_text()
    assert ".tickle_max_buffer_length = 4096," in source
