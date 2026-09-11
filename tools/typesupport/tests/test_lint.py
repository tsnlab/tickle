# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Runs the project's own clang-tidy over generated output, with the same three checks turned off
that M5 will put in examples/.clang-tidy once generated codecs actually live under examples/ (see
PLAN.md): readability-identifier-naming (ROS 2-derived names like FooData_encode don't - and
shouldn't have to - follow the project's own lower_case convention), readability-magic-numbers
(every offset/size in a wire codec is exactly that, a wire offset - naming each one would be
noise, not clarity), and readability-non-const-parameter (the codec ABI - tt_DATA_ENCODE et al.
in tickle.h - fixes each function pointer's signature across every message type, so a `payload`
or `data` parameter that one particular message's body happens not to write through, e.g. a
zero-field request's _encode, can't be made const without breaking the shared typedef; the exact
same exemption already exists in tests/.clang-tidy for this reason). Everything else the repo's
.clang-tidy enables must still pass - most importantly bugprone-* and the rest of readability-* /
misc-* / performance-*, which is what actually validates the generator's C, not just its
formatting (test_golden.py already covers that clang-format leaves the output unchanged).
"""

import pathlib
import shutil
import subprocess

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
# clang-tidy's own .clang-tidy file search walks up from the file *being analyzed*, not from
# cwd - InheritParentConfig only reaches the repo root's .clang-tidy (and its bugprone-reserved-
# identifier / readability-* CheckOptions, which generated code calling _tt_bswap_* etc. still
# wants) if the file it's pointed at actually lives under the repo tree. So the generated files
# get copied into one here, rather than linted from pytest's own /tmp working directory.
_LOCAL_TIDY_OVERRIDE = (
    "InheritParentConfig: true\n"
    "Checks: '-readability-identifier-naming,-readability-magic-numbers,-readability-non-const-parameter'\n"
)


@pytest.fixture
def generated_dir_in_repo(generated_dir, tmp_path):
    lint_dir = REPO_ROOT / "tools" / "typesupport" / ".pytest_lint_tmp" / tmp_path.name
    lint_dir.mkdir(parents=True, exist_ok=True)
    for src in generated_dir.glob("*"):
        (lint_dir / src.name).write_bytes(src.read_bytes())
    (lint_dir / ".clang-tidy").write_text(_LOCAL_TIDY_OVERRIDE, encoding="utf-8")
    yield lint_dir
    shutil.rmtree(lint_dir, ignore_errors=True)


@pytest.mark.skipif(shutil.which("clang-tidy") is None, reason="clang-tidy not installed")
def test_generated_c_files_pass_clang_tidy(generated_dir_in_repo):
    failures = {}
    for c_file in sorted(generated_dir_in_repo.glob("*.c")):
        result = subprocess.run(
            [
                "clang-tidy",
                f"--extra-arg=-I{REPO_ROOT / 'include'}",
                f"--extra-arg=-I{REPO_ROOT / 'src'}",
                "--quiet",
                str(c_file),
            ],
            capture_output=True,
            text=True,
            check=False,
            cwd=REPO_ROOT,
        )
        findings = [
            line
            for line in result.stdout.splitlines()
            if " warning: " in line or " error: " in line
        ]
        if findings:
            failures[c_file.name] = findings
    assert not failures, "\n".join(f"{name}:\n  " + "\n  ".join(lines) for name, lines in failures.items())
