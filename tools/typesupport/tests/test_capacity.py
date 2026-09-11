# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Variable-array capacity handling (PLAN.md's "capacity" rule): a count over the resolved
capacity must be rejected - by *_encode_size/_encode (an over-large data->*_count) and by
*_decode (an over-large count read off the wire, which might come from a malformed or hostile
peer) alike - rather than writing past the fixed-capacity C array underneath. See
tests/fixtures_own/Arrays.msg (ROS 2 upper bound + an explicit @capacity annotation) and
examples/Bulk.msg (auto-derived capacity) for where each of the three capacity-resolution
priorities in PLAN.md gets exercised.
"""

import ctypes
import sys

from test_roundtrip import (
    BULK_PAYLOAD_CAPACITY,
    TT_MAX_BUFFER_LENGTH,
    ArraysData,
    BulkData,
    _bind,
)


def test_bounded_array_at_capacity_succeeds(generated_lib):
    # Boundary value: exactly the ROS 2 upper bound (8) must still be accepted, not just capacity
    # - 1 - an off-by-one here would silently shrink the usable range by one element.
    encode_size, encode, decode, _free = _bind(generated_lib, "ArraysData", ArraysData)
    data = ArraysData()
    data.bounded_values_count = 8
    data.bounded_values[:] = list(range(8))

    size = encode_size(ctypes.byref(data))
    assert size > 0
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == size

    out = ArraysData()
    assert decode(ctypes.byref(out), buf, size, True) == size
    assert out.bounded_values_count == 8
    assert list(out.bounded_values) == list(range(8))


def test_bounded_array_encode_rejects_over_capacity(generated_lib):
    encode_size, encode, _decode, _free = _bind(generated_lib, "ArraysData", ArraysData)
    data = ArraysData()
    data.bounded_values_count = 9  # one past the ROS 2 upper bound `uint16[<=8]`
    assert encode_size(ctypes.byref(data)) == -2
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == -2


def test_annotated_capacity_array_encode_rejects_over_capacity(generated_lib):
    # `samples` (float32[] with `# @capacity 16`) - same check, exercising the annotation-driven
    # capacity path rather than a ROS 2 upper bound.
    encode_size, encode, _decode, _free = _bind(generated_lib, "ArraysData", ArraysData)
    data = ArraysData()
    data.samples_count = 17
    assert encode_size(ctypes.byref(data)) == -2
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == -2


def test_bounded_array_decode_rejects_over_capacity_count_on_wire(generated_lib):
    # A malformed (or hostile) peer's wire count must be rejected on the way in too, not just an
    # over-large struct field on the way out - decode() has no struct to check against, only the
    # count it just read off the wire.
    _encode_size, _encode, decode, _free = _bind(generated_lib, "ArraysData", ArraysData)
    wire = bytes(4 + 12) + (9).to_bytes(2, sys.byteorder) + bytes(9 * 2)  # fixed_bytes+fixed_ints, then count=9
    buf = ctypes.create_string_buffer(wire, TT_MAX_BUFFER_LENGTH)
    out = ArraysData()
    assert decode(ctypes.byref(out), buf, len(wire), True) == -2


def test_bulk_payload_at_auto_derived_capacity_succeeds(generated_lib):
    # The auto-derived case (examples/Bulk.msg's `payload` has neither a ROS 2 upper bound nor an
    # @capacity annotation) - filling the datagram right up to the derived limit must still work.
    encode_size, encode, decode, _free = _bind(generated_lib, "BulkData", BulkData)
    data = BulkData()
    data.seq = 1
    data.payload_count = BULK_PAYLOAD_CAPACITY
    ctypes.memmove(data.payload, bytes(BULK_PAYLOAD_CAPACITY), BULK_PAYLOAD_CAPACITY)

    size = encode_size(ctypes.byref(data))
    assert size > 0
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == size

    out = BulkData()
    assert decode(ctypes.byref(out), buf, size, True) == size
    assert out.payload_count == BULK_PAYLOAD_CAPACITY


def test_bulk_payload_over_auto_derived_capacity_rejected(generated_lib):
    encode_size, encode, _decode, _free = _bind(generated_lib, "BulkData", BulkData)
    data = BulkData()
    data.seq = 1
    data.payload_count = BULK_PAYLOAD_CAPACITY + 1  # ctypes lets us set this even though the
    # physical array is exactly BULK_PAYLOAD_CAPACITY elements - encode_size/encode must catch it
    # from the count field alone, before ever looping over the (too-short) array.
    assert encode_size(ctypes.byref(data)) == -2
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == -2
