# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""A struct nesting an empty message. The empty one is zero bytes on the wire but carries a
one-byte filler in C, so the container's memory image is not its wire image even though every size
is fixed. The generator used to assert they were equal - a compile error, first hit by tf2_msgs'
LookupTransform action, whose FeedbackMessage wraps an empty Feedback (2026-09-25) - and would have
offered an in-place codec that copies the struct as if it were the wire bytes."""

import ctypes
import subprocess

from conftest import CC, CFLAGS, REPO_ROOT
from tickle_typesupport import cli


def test_a_struct_nesting_an_empty_message_compiles_and_round_trips(tmp_path):
    pkg = tmp_path / "mini_msgs" / "msg"
    pkg.mkdir(parents=True)
    (pkg / "Nothing.msg").write_text("")
    (pkg / "Holder.msg").write_text("uint8[16] id\nmini_msgs/Nothing nothing\nuint32 after\n")
    out = tmp_path / "out"
    cli.generate_interface(str(pkg / "Holder.msg"), str(out), style_dir=str(REPO_ROOT), include_dirs=[str(tmp_path)])
    header = (out / "Holder.h").read_text()
    assert "_Static_assert(sizeof(struct HolderData)" not in header
    assert "HolderData_encode_inplace" not in header

    # The control that matters: this compiled before the fix only if the assert was wrong about
    # nothing, and it round-trips, so the non-in-place codec skips the filler correctly.
    sources = sorted(str(p) for p in out.glob("*.c"))
    lib = tmp_path / "libholder.so"
    subprocess.run([CC, *CFLAGS, "-shared", "-o", str(lib), *sources, str(REPO_ROOT / "src" / "encoding.c"), str(REPO_ROOT / "src" / "log.c")], check=True)
    so = ctypes.CDLL(str(lib))

    class Nothing(ctypes.Structure):
        _fields_ = [("reserved", ctypes.c_uint8)]

    class Holder(ctypes.Structure):
        _pack_ = 4
        _fields_ = [("id", ctypes.c_uint8 * 16), ("nothing", Nothing), ("after", ctypes.c_uint32)]

    sent = Holder()
    sent.id[:] = list(range(16))
    sent.after = 0xDEADBEEF
    buf = (ctypes.c_uint8 * 64)()
    size = so.HolderData_encode(ctypes.byref(sent), buf, 64)
    assert size == 20  # 16 + 0 (the empty message) + 4 - the filler is not on the wire
    got = Holder()
    assert so.HolderData_decode(ctypes.byref(got), buf, size, True) == size
    assert list(got.id) == list(range(16)) and got.after == 0xDEADBEEF
