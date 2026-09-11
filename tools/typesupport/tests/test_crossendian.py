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

from test_roundtrip import TT_MAX_BUFFER_LENGTH, ArraysData, SetBoolResponse, UInt64Data, _bind

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


def test_arrays_decode_swaps_fixed_and_variable_array_fields(generated_lib):
    # Exercises every array-specific byte-swap path in one message, built by hand so a decode bug
    # that only swaps one of these can't hide behind the others happening to be right: a fixed
    # array's per-element swap (fixed_ints), a variable array's count-prefix swap plus its own
    # per-element swap (bounded_values), and the same again with a float element (samples), to
    # cover the union-based bit-reinterpretation path too.
    fixed_ints = [1, -2, 0x11223344]
    bounded_values = [1, 0xABCD, 42]
    samples = [2.5]

    wire = bytes(4)  # fixed_bytes - content doesn't matter, no byte-swapping applies to it
    wire += b"".join(struct.pack(_OPPOSITE + "i", v) for v in fixed_ints)
    wire += struct.pack(_OPPOSITE + "H", len(bounded_values))
    wire += b"".join(struct.pack(_OPPOSITE + "H", v) for v in bounded_values)
    wire += struct.pack(_OPPOSITE + "H", len(samples))
    wire += b"\x00\x00"  # pad the count prefix (2 bytes) up to samples' 4-byte element alignment
    wire += b"".join(struct.pack(_OPPOSITE + "f", v) for v in samples)

    _encode_size, _encode, decode, _free = _bind(generated_lib, "ArraysData", ArraysData)
    out = ArraysData()
    buf = ctypes.create_string_buffer(wire, TT_MAX_BUFFER_LENGTH)
    decoded = decode(ctypes.byref(out), buf, len(wire), False)

    assert decoded == len(wire)
    assert list(out.fixed_ints) == fixed_ints
    assert out.bounded_values_count == len(bounded_values)
    assert list(out.bounded_values[: len(bounded_values)]) == bounded_values
    assert out.samples_count == len(samples)
    assert list(out.samples[: len(samples)]) == samples
