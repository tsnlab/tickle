# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Shared fixtures for the codegen tests: generate TickLE's own .msg/.srv examples, compile them
(with the project's real flags - see platform/linux/Makefile - not some stricter standard the
generator doesn't actually have to satisfy) alongside src/encoding.c + src/log.c into one shared
library, and hand back a ctypes handle test_roundtrip.py / test_crossendian.py can call straight
into. test_golden.py / test_lint.py regenerate separately - they care about the generated text
itself, not about running it.
"""

import ctypes
import pathlib
import subprocess

import pytest

from tickle_typesupport import cli

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
EXAMPLES = REPO_ROOT / "examples"
CC = "cc"
CFLAGS = ["-Wall", "-Wextra", "-fPIC", f"-I{REPO_ROOT / 'include'}", f"-I{REPO_ROOT / 'src'}"]

# The interfaces every codegen test exercises: TickLE's own real example interfaces, covering an
# all-scalar fixed message, a request/response pair with a string + a scalar-then-string default
# padding gap, and an empty request.
GENERATED_INTERFACES = ["UInt64.msg", "SetBool.srv", "Trigger.srv"]


def _compile_to_object(path, outdir):
    obj = outdir / (path.stem + ".o")
    subprocess.run([CC, *CFLAGS, "-c", "-o", str(obj), str(path)], check=True)
    return obj


@pytest.fixture(scope="session")
def generated_dir(tmp_path_factory):
    """Generates every interface in GENERATED_INTERFACES into one temp directory and returns it -
    used both to compile (this file) and to diff against tests/golden/ (test_golden.py)."""
    outdir = tmp_path_factory.mktemp("generated")
    for name in GENERATED_INTERFACES:
        cli.generate_interface(str(EXAMPLES / name), str(outdir), style_dir=str(REPO_ROOT))
    return outdir


@pytest.fixture(scope="session")
def generated_lib(generated_dir, tmp_path_factory):
    """Compiles every generated .c (+ src/encoding.c, src/log.c - the only two the generated
    codecs themselves depend on, matching how the real build links them - see
    platform/linux/Makefile) into one shared library and loads it via ctypes."""
    build_dir = tmp_path_factory.mktemp("build")
    objects = [_compile_to_object(p, build_dir) for p in sorted(generated_dir.glob("*.c"))]
    objects += [
        _compile_to_object(REPO_ROOT / "src" / "encoding.c", build_dir),
        _compile_to_object(REPO_ROOT / "src" / "log.c", build_dir),
    ]
    lib_path = build_dir / "libgenerated.so"
    subprocess.run([CC, "-shared", "-o", str(lib_path), *[str(o) for o in objects]], check=True)
    return ctypes.CDLL(str(lib_path))
