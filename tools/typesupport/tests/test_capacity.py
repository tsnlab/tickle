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
    BoundedStringData,
    BulkData,
    NestedArraysData,
    NestedArraysPkgOddAlign,
    StringArraysData,
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


def test_bounded_string_encode_rejects_over_capacity(generated_lib):
    # Fill `bounded_name` (`string<=8`, capacity 8) with 9 non-NUL bytes so _tt_strnlen can't find
    # a terminator within the field's own capacity - the fixed-buffer analog of
    # test_bounded_array_encode_rejects_over_capacity above. Writes straight into the struct's
    # memory (bypassing ctypes' own c_char-array get/set copying) so the buffer genuinely holds no
    # NUL, the same way a caller who forgot to terminate their string would leave it.
    encode_size, encode, _decode, _free = _bind(generated_lib, "BoundedStringData", BoundedStringData)
    data = BoundedStringData()
    offset = BoundedStringData.bounded_name.offset
    ctypes.memmove(ctypes.byref(data, offset), b"123456789", 9)
    assert encode_size(ctypes.byref(data)) == -2
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == -2


def test_bounded_string_decode_rejects_over_capacity_length_on_wire(generated_lib):
    # A malformed (or hostile) peer's wire length prefix must be rejected before ever memcpy()ing
    # it into the fixed char[9] buffer underneath - not just an over-long struct field on the way
    # out. `bounded_name` is the struct's first field (offset 0), so its wire length prefix is the
    # first two bytes.
    _encode_size, _encode, decode, _free = _bind(generated_lib, "BoundedStringData", BoundedStringData)
    wire = (10).to_bytes(2, sys.byteorder)  # str_len = 10, over bounded_name's capacity+1 (9)
    buf = ctypes.create_string_buffer(wire, TT_MAX_BUFFER_LENGTH)
    out = BoundedStringData()
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


def test_string_array_encode_rejects_over_capacity(generated_lib):
    # `bounded_names` (string[<=4]) - the array-of-string analog of
    # test_bounded_array_encode_rejects_over_capacity, checked before ever looping over an
    # element (none need to be set for this to be rejected).
    encode_size, encode, _decode, _free = _bind(generated_lib, "StringArraysData", StringArraysData)
    data = StringArraysData()
    data.fixed_names[:] = [b"a", b"b", b"c"]
    data.bounded_names_count = 5  # one past the ROS 2 upper bound `string[<=4]`
    assert encode_size(ctypes.byref(data)) == -2
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == -2


def test_string_array_decode_rejects_over_capacity_count_on_wire(generated_lib):
    # A malformed (or hostile) peer's wire count must be rejected on the way in too - the array-
    # of-string analog of test_bounded_array_decode_rejects_over_capacity_count_on_wire.
    # fixed_names (string[3]) comes first: 3 elements, each str_len=1 ("" + NUL, no padding
    # needed since 2 + 1 + 1(pad) = 4), then bounded_names' own count prefix, set to 5 (one past
    # its capacity of 4).
    _encode_size, _encode, decode, _free = _bind(generated_lib, "StringArraysData", StringArraysData)
    one_empty_string = (1).to_bytes(2, sys.byteorder) + b"\x00" + b"\x00"  # len=1, "\0", pad to 4
    wire = one_empty_string * 3 + (5).to_bytes(2, sys.byteorder)
    buf = ctypes.create_string_buffer(wire, TT_MAX_BUFFER_LENGTH)
    out = StringArraysData()
    assert decode(ctypes.byref(out), buf, len(wire), True) == -2


def test_string_array_element_rejects_null(generated_lib):
    # A NULL char* element (never assigned, or explicitly cleared) must be rejected the same way
    # a NULL plain top-level string field already is (test_setbool_response_rejects_null_message)
    # - checked by the shared <name>_encode_string_element() helper (emit.
    # emit_string_element_helpers), not just the field-level _emit_string_encode path.
    encode_size, encode, _decode, _free = _bind(generated_lib, "StringArraysData", StringArraysData)
    data = StringArraysData()
    data.fixed_names[:] = [b"a", None, b"c"]
    assert encode_size(ctypes.byref(data)) == -3
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == -3


def test_nested_array_encode_rejects_over_capacity(generated_lib):
    # `bounded_items` (OddAlign[<=3]) - the array-of-nested-type analog of
    # test_bounded_array_encode_rejects_over_capacity.
    encode_size, encode, _decode, _free = _bind(generated_lib, "NestedArraysData", NestedArraysData)
    data = NestedArraysData()
    data.bounded_items_count = 4  # one past the ROS 2 upper bound `OddAlign[<=3]`
    assert encode_size(ctypes.byref(data)) == -2
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == -2


def test_nested_array_decode_rejects_over_capacity_count_on_wire(generated_lib):
    # A malformed (or hostile) peer's wire count must be rejected on the way in too - the array-
    # of-nested-type analog of test_bounded_array_decode_rejects_over_capacity_count_on_wire.
    # fixed_items (OddAlign[2]) comes first: 2 elements (13 bytes + a 3-byte inter-element gap),
    # then a 1-byte pad to align bounded_items_count to 2, then that count itself, set to 4 (one
    # past its capacity of 3).
    _encode_size, _encode, decode, _free = _bind(generated_lib, "NestedArraysData", NestedArraysData)
    one_odd_align = bytes(13)  # flag=false, big=0, tail=0 - all-zero is a valid element
    wire = one_odd_align + bytes(3) + one_odd_align + b"\x00" + (4).to_bytes(2, sys.byteorder)
    buf = ctypes.create_string_buffer(wire, TT_MAX_BUFFER_LENGTH)
    out = NestedArraysData()
    assert decode(ctypes.byref(out), buf, len(wire), True) == -2


def test_odd_align_ctypes_sizeof_matches_generated_padding():
    # OddAlign's own wire size (13) genuinely isn't a multiple of its own self-alignment (4) -
    # ctypes.sizeof() must independently agree with the generator's own computed sizeof (16, via
    # layout.padded_wire_size()) or every offset in this whole test file would be silently wrong.
    assert ctypes.sizeof(NestedArraysPkgOddAlign) == 16
