# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Regenerates TickLE's own real interfaces and diffs the result (already clang-formatted, same
as `make regen` would produce) against tests/golden/ - a snapshot of accepted output. Unlike
test_roundtrip.py / test_crossendian.py, which prove the generated code behaves correctly, this
is a plain regression guard: an unintended template/emit.py change shows up as a diff here even
if it happens not to break any behavioral test. Update tests/golden/ deliberately (copy the
freshly generated files over) when a change is meant to alter the output.
"""

import pathlib

GOLDEN = pathlib.Path(__file__).parent / "golden"


def test_regenerated_output_matches_golden(generated_dir):
    # golden/ also carries a .clang-tidy (see its own comment) so `make lint` accepts these
    # ROS 2-named snapshots repo-wide - that's not generator output, so it's excluded here.
    generated_files = sorted(p.name for p in generated_dir.glob("*"))
    golden_files = sorted(p.name for p in GOLDEN.glob("*.[ch]"))
    assert generated_files == golden_files, "generated and golden/ don't even agree on which files exist"

    mismatches = []
    for name in generated_files:
        actual = (generated_dir / name).read_text(encoding="utf-8")
        expected = (GOLDEN / name).read_text(encoding="utf-8")
        if actual != expected:
            mismatches.append(name)
    assert not mismatches, (
        f"generated output drifted from tests/golden/ for: {mismatches} - "
        "if this is an intended change, copy the freshly generated files over tests/golden/"
    )
