# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""decode() must byte-swap correctly when is_native_endian is false - see "Byte order" and
"Interface serialization (TickLE CDR-4)" in DESIGN.md. Unlike test_roundtrip.py (which only ever
round-trips through encode(), always native), these tests build the opposite-endian wire bytes
by hand with Python's struct module and feed them straight to decode() - so a generator bug that
happened to cancel out between its own encode and decode can't hide here.
"""

import ctypes
import struct
import sys

from test_roundtrip import TT_MAX_BUFFER_LENGTH, SetBoolResponse, UInt64Data, _bind

_OPPOSITE = ">" if sys.byteorder == "little" else "<"  # struct format prefix for "not host order"


def test_uint64_decode_swaps_reverse_endian(generated_lib):
    _encode_size, _encode, decode, _free = _bind(generated_lib, "UInt64Data", UInt64Data)
    value = 0xFEEDFACECAFEBEEF
    wire = struct.pack(_OPPOSITE + "Q", value)

    out = UInt64Data()
    decoded = decode(ctypes.byref(out), wire, len(wire), False)

    assert decoded == 8
    assert out.data == value


def test_uint64_decode_native_does_not_swap(generated_lib):
    # Sanity check for the test helper itself: is_native_endian=True on the *same* opposite-
    # order bytes must NOT produce the original value - otherwise this file would be unable to
    # tell a working swap from a decode that ignores is_native_endian entirely.
    _encode_size, _encode, decode, _free = _bind(generated_lib, "UInt64Data", UInt64Data)
    value = 0x0102030405060708
    wire = struct.pack(_OPPOSITE + "Q", value)

    out = UInt64Data()
    decode(ctypes.byref(out), wire, len(wire), True)

    assert out.data != value


def test_setbool_response_decode_swaps_string_length(generated_lib):
    # success(1) + pad(1) + length(uint16, opposite-endian) + "hi\0" + pad-to-4.
    message = b"hi\x00"
    wire = bytes([1, 0]) + struct.pack(_OPPOSITE + "H", len(message)) + message
    wire += b"\x00" * ((-len(wire)) % 4)

    _encode_size, _encode, decode, _free = _bind(generated_lib, "SetBoolResponse", SetBoolResponse)
    out = SetBoolResponse()
    buf = ctypes.create_string_buffer(wire, TT_MAX_BUFFER_LENGTH)
    decoded = decode(ctypes.byref(out), buf, len(wire), False)

    assert decoded == len(wire)
    assert out.success is True
    assert out.message == b"hi"
