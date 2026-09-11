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
FIXTURES_OWN = pathlib.Path(__file__).parent / "fixtures_own"
CC = "cc"
CFLAGS = ["-Wall", "-Wextra", "-fPIC", f"-I{REPO_ROOT / 'include'}", f"-I{REPO_ROOT / 'src'}"]

# The interfaces every codegen test exercises, and where each one's .msg/.srv source lives:
#   - UInt64/SetBool/Trigger: TickLE's own real example interfaces (examples/) - an all-scalar
#     fixed message, a request/response pair with a string + a scalar-then-string default
#     padding gap, and an empty request.
#   - Bulk: also a real example interface (examples/), added for M2 - a single unbounded array
#     with an auto-derived capacity (see examples/Bulk.msg's own comment on why it isn't yet
#     wire-compatible with examples/linux/perf/Bulk.c's hand-written version).
#   - Arrays: tests/fixtures_own/ - not a real TickLE interface, exists purely to exercise every
#     other array shape (fixed, bounded, annotated-capacity, float element) in one place.
GENERATED_INTERFACES = {
    "UInt64.msg": EXAMPLES,
    "SetBool.srv": EXAMPLES,
    "Trigger.srv": EXAMPLES,
    "Bulk.msg": EXAMPLES,
    "Arrays.msg": FIXTURES_OWN,
}


def _compile_to_object(path, outdir):
    obj = outdir / (path.stem + ".o")
    subprocess.run([CC, *CFLAGS, "-c", "-o", str(obj), str(path)], check=True)
    return obj


@pytest.fixture(scope="session")
def generated_dir(tmp_path_factory):
    """Generates every interface in GENERATED_INTERFACES into one temp directory and returns it -
    used both to compile (this file) and to diff against tests/golden/ (test_golden.py)."""
    outdir = tmp_path_factory.mktemp("generated")
    for name, source_dir in GENERATED_INTERFACES.items():
        cli.generate_interface(str(source_dir / name), str(outdir), style_dir=str(REPO_ROOT))
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
