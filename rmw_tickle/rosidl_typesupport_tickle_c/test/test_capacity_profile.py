# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""rosidl_typesupport_tickle_c.capacity_profile: which shipped profile a build gets, and how a
user's overrides go on top (the user's decision of 2026-09-24 - TickLE ships per-message capacity
defaults, the user can override them). Checked against small fixtures, and against the real shipped
profiles so a regression in either shows up here rather than in someone's build."""

import os
import pathlib

import pytest
from conftest import HERE
from rosidl_typesupport_tickle_c import capacity_profile

SHIPPED = HERE.parent / "capacities"


def _profiles(tmp_path, **profiles):
    for n, text in profiles.items():
        (tmp_path / f"profile_{n.lstrip('n')}.tsv").write_text(text)
    return tmp_path


def test_profile_is_the_largest_not_above_n(tmp_path):
    shipped = _profiles(tmp_path, n1472="", n65507="")
    assert capacity_profile.choose_profile(shipped, 65507).name == "profile_65507.tsv"
    # A user who lowers N to 8192 gets the 1472 profile: the 65507 one's images would not fit.
    assert capacity_profile.choose_profile(shipped, 8192).name == "profile_1472.tsv"
    assert capacity_profile.choose_profile(shipped, 1472).name == "profile_1472.tsv"
    # Below every profile: the smallest - rmw_tickle then names any type that does not fit.
    assert capacity_profile.choose_profile(shipped, 1024).name == "profile_1472.tsv"
    assert capacity_profile.choose_profile(tmp_path / "missing", 65507) is None


def test_user_rows_go_on_top_row_by_row(tmp_path):
    (tmp_path / "shipped").mkdir()
    shipped = _profiles(
        tmp_path / "shipped", n1472="std_msgs/msg/A data 10\nstd_msgs/msg/B data 20\nother_msgs/msg/C data 30\n"
    )
    first = tmp_path / "first"
    second = tmp_path / "second"
    first.mkdir()
    second.mkdir()
    (second / "std_msgs.capacities").write_text("std_msgs/msg/A data 11\nstd_msgs/msg/B data 21\n")
    (first / "std_msgs.capacities").write_text("std_msgs/msg/A data 12  # first on the path wins\n")
    text, sources = capacity_profile.effective_table(
        "std_msgs", 1472, shipped, os.pathsep.join([str(first), str(second)])
    )
    rows = [line for line in text.splitlines() if line and not line.startswith("#")]
    assert rows == ["std_msgs/msg/A data 12  # first on the path wins", "std_msgs/msg/B data 21"]
    assert "other_msgs" not in text  # another package's shipped rows are not this package's
    assert sources == [shipped / "profile_1472.tsv", second / "std_msgs.capacities", first / "std_msgs.capacities"]


def test_a_user_file_naming_another_package_is_an_error(tmp_path):
    # Filtering it, as the shipped profile is filtered, would silently drop what the user asked for.
    (tmp_path / "std_msgs.capacities").write_text("geometry_msgs/msg/Polygon points 8\n")
    with pytest.raises(ValueError, match="not a std_msgs row"):
        capacity_profile.effective_table("std_msgs", 1472, tmp_path / "none", str(tmp_path))


def test_output_is_rewritten_only_when_it_changes(tmp_path):
    # It is a DEPENDS of every generator run: touching it needlessly would regenerate the package.
    shipped = _profiles(tmp_path, n1472="std_msgs/msg/A data 10\n")
    out = tmp_path / "out" / "std_msgs.capacities"
    args = ["--package", "std_msgs", "--max-buffer-length", "1472", "--shipped-dir", str(shipped),
            "--search-path", "", "--out", str(out)]
    assert capacity_profile.main(args) == 0
    stamp = out.stat().st_mtime_ns
    os.utime(out, ns=(stamp - 10**9, stamp - 10**9))
    assert capacity_profile.main(args) == 0
    assert out.stat().st_mtime_ns == stamp - 10**9  # unchanged content, untouched file


@pytest.mark.parametrize("n, expected", [(65507, "16384"), (1472, "1280")])
def test_real_shipped_profiles(n, expected):
    # The shipped data itself: std_msgs' byte arrays are sized for the datagram the build uses.
    text, _ = capacity_profile.effective_table("std_msgs", n, SHIPPED, "")
    row = next(line for line in text.splitlines() if line.startswith("std_msgs/msg/ByteMultiArray"))
    assert row.split()[2] == expected


def test_every_shipped_row_parses():
    # What the generator would reject at build time - a malformed or duplicated row - found here.
    from tickle_typesupport import capacities

    for profile in sorted(pathlib.Path(SHIPPED).glob("profile_*.tsv")):
        text = profile.read_text()
        packages = {line.split("/")[0] for line in text.splitlines() if line.strip() and not line.startswith("#")}
        for package in packages:
            rows = "\n".join(line for line in text.splitlines() if line.startswith(package + "/"))
            assert capacities.parse(rows, package, str(profile))
