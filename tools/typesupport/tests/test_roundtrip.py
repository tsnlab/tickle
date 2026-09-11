# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Compiles the generated codecs for TickLE's own real interfaces and drives them through
ctypes: encode a message, decode the exact bytes back, check the struct comes back equal. This
is the only test that proves the generator's output actually *runs* correctly, not just that it
looks right or compiles - see test_golden.py / test_lint.py for those.
"""

import ctypes
import sys

TT_MAX_BUFFER_LENGTH = 1472


class UInt64Data(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("data", ctypes.c_uint64)]


class SetBoolRequest(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("data", ctypes.c_bool)]


class SetBoolResponse(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("success", ctypes.c_bool), ("message", ctypes.c_char_p)]


class TriggerRequest(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("reserved", ctypes.c_uint8)]


class TriggerResponse(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("success", ctypes.c_bool), ("message", ctypes.c_char_p)]


# examples/Bulk.msg's `payload` has no ROS 2 upper bound or @capacity annotation, so its capacity
# is auto-derived (PLAN.md's lowest-priority rule) from TT_MAX_BUFFER_LENGTH (1472) minus
# FRAMING_OVERHEAD (24) minus `seq` (4 bytes) minus the array's own uint16 count prefix (2 bytes,
# already 1-aligned so no further padding) = 1442 - see model.py / adapt._resolve_auto_capacities.
# A ctypes mirror has to hardcode this the same way a hand-written struct.h consumer would: it's
# baked into the generated struct's own layout, not discoverable at the ABI level.
BULK_PAYLOAD_CAPACITY = 1442


class BulkData(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [
        ("seq", ctypes.c_uint32),
        # A variable array's count member is declared *before* its buffer (matching wire order -
        # see emit.emit_struct_fields) so a struct like this one, otherwise fixed-size up to a
        # single trailing byte array, can alias its own memory as the wire payload - the same
        # reason examples/Bulk.msg now gets *_encode_inplace/*_decode_inplace (M4's
        # layout.prefix_array_field).
        ("payload_count", ctypes.c_uint16),
        ("payload", ctypes.c_uint8 * BULK_PAYLOAD_CAPACITY),
    ]


class ArraysData(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [
        ("fixed_bytes", ctypes.c_uint8 * 4),
        ("fixed_ints", ctypes.c_int32 * 3),
        ("bounded_values_count", ctypes.c_uint16),
        ("bounded_values", ctypes.c_uint16 * 8),
        ("samples_count", ctypes.c_uint16),
        ("samples", ctypes.c_float * 16),
    ]


# M3: nested messages. Mirrors builtin_interfaces__Time / std_msgs__Header / geometry_msgs__
# Vector3's own generated structs (tests/fixtures_own/{Stamped,Image}.msg, tests/fixtures_ros2/
# geometry_msgs/msg/Twist.msg) - a nested field embeds the nested type's ctypes.Structure *by
# value*, same as the generated C struct does (see DESIGN.md: nesting inlines, it doesn't point).
class BuiltinInterfacesTime(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("sec", ctypes.c_int32), ("nanosec", ctypes.c_uint32)]


class StdMsgsHeader(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("stamp", BuiltinInterfacesTime), ("frame_id", ctypes.c_char_p)]


class StampedData(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("header", StdMsgsHeader), ("value", ctypes.c_uint32)]


class GeometryMsgsVector3(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double), ("z", ctypes.c_double)]


class TwistData(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("linear", GeometryMsgsVector3), ("angular", GeometryMsgsVector3)]


class ArrayDefaultsData(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [
        ("fixed_with_default", ctypes.c_uint8 * 4),
        ("bounded_with_default_count", ctypes.c_uint16),
        ("bounded_with_default", ctypes.c_uint16 * 4),
    ]


class PingPongRequest(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("seq", ctypes.c_uint32), ("timestamp", ctypes.c_uint64)]


class PingPongResponse(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [("seq", ctypes.c_uint32), ("timestamp", ctypes.c_uint64)]


IMAGE_DATA_CAPACITY = 1400  # tests/fixtures_own/Image.msg's `# @capacity 1400` annotation


class ImageData(ctypes.Structure):
    _pack_ = 4
    _layout_ = "ms"
    _fields_ = [
        ("header", StdMsgsHeader),
        ("height", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("encoding", ctypes.c_char_p),
        ("is_bigendian", ctypes.c_uint8),
        ("step", ctypes.c_uint32),
        ("data_count", ctypes.c_uint16),
        ("data", ctypes.c_uint8 * IMAGE_DATA_CAPACITY),
    ]


def _bind_inplace(lib, prefix, struct_type):
    """*_encode_inplace/*_decode_inplace (only generated for an all-fixed-size struct - M4, see
    emit.emit_encode_inplace's own docstring) have a different shape than the four ctypes.
    _bind() below covers: encode_inplace hands back a pointer *through* an out-param instead of
    writing into a caller-supplied buffer, and decode_inplace returns a struct pointer directly
    instead of a byte count."""
    encode_inplace = getattr(lib, f"{prefix}_encode_inplace")
    decode_inplace = getattr(lib, f"{prefix}_decode_inplace")
    encode_inplace.argtypes = [ctypes.POINTER(struct_type), ctypes.POINTER(ctypes.c_void_p)]
    encode_inplace.restype = ctypes.c_int32
    decode_inplace.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_bool]
    decode_inplace.restype = ctypes.POINTER(struct_type)
    return encode_inplace, decode_inplace


def _bind(lib, prefix, struct_type):
    """Sets up ctypes argtypes/restype for one message's four codec functions - ctypes assumes
    every C function returns `int` and takes no particular argument types unless told otherwise,
    which is wrong here (int32_t return, pointer args) and would corrupt values on a 64-bit
    build if left unbound."""
    encode_size = getattr(lib, f"{prefix}_encode_size")
    encode = getattr(lib, f"{prefix}_encode")
    decode = getattr(lib, f"{prefix}_decode")
    free_ = getattr(lib, f"{prefix}_free")

    encode_size.argtypes = [ctypes.POINTER(struct_type)]
    encode_size.restype = ctypes.c_int32
    encode.argtypes = [ctypes.POINTER(struct_type), ctypes.c_char_p, ctypes.c_uint32]
    encode.restype = ctypes.c_int32
    decode.argtypes = [ctypes.POINTER(struct_type), ctypes.c_char_p, ctypes.c_uint32, ctypes.c_bool]
    decode.restype = ctypes.c_int32
    free_.argtypes = [ctypes.POINTER(struct_type)]
    free_.restype = None
    return encode_size, encode, decode, free_


def _roundtrip(lib, prefix, struct_type, populate):
    encode_size, encode, decode, free_ = _bind(lib, prefix, struct_type)

    original = struct_type()
    populate(original)

    size = encode_size(ctypes.byref(original))
    assert size > 0

    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    encoded = encode(ctypes.byref(original), buf, TT_MAX_BUFFER_LENGTH)
    assert encoded == size

    decoded_struct = struct_type()
    decoded = decode(ctypes.byref(decoded_struct), buf, encoded, True)
    assert decoded == encoded
    # decoded_struct may alias `buf` (a decoded string/array points straight into it, per the
    # CDR-4 spec's no-copy contract - see DESIGN.md) - keep both alive together so a caller
    # reading e.g. .message after this function returns doesn't dereference freed memory. Calling
    # *_free() first, as real code does right after using the decoded fields, doesn't change
    # that: *_free() is a no-op (nothing is ever malloc'd - see emit.emit_free), it does not
    # invalidate the alias.
    free_(ctypes.byref(decoded_struct))
    return decoded_struct, buf


def test_uint64_roundtrip(generated_lib):
    result, _buf = _roundtrip(generated_lib, "UInt64Data", UInt64Data, lambda d: setattr(d, "data", 0xFEEDFACECAFEBEEF))
    assert result.data == 0xFEEDFACECAFEBEEF


def test_uint64_roundtrip_zero(generated_lib):
    # The all-zero case is worth pinning explicitly: a bug that only shows up when a "have we
    # written anything" check is accidentally value-sensitive (e.g. `if (x)` instead of a real
    # length check) would otherwise slip through the more interesting-looking values above.
    result, _buf = _roundtrip(generated_lib, "UInt64Data", UInt64Data, lambda d: setattr(d, "data", 0))
    assert result.data == 0


def test_setbool_request_roundtrip(generated_lib):
    result, _buf = _roundtrip(generated_lib, "SetBoolRequest", SetBoolRequest, lambda d: setattr(d, "data", True))
    assert result.data is True


def test_setbool_response_roundtrip(generated_lib):
    def populate(d):
        d.success = True
        d.message = b"all good"

    result, _buf = _roundtrip(generated_lib, "SetBoolResponse", SetBoolResponse, populate)
    assert result.success is True
    assert result.message == b"all good"


def test_setbool_response_empty_string_roundtrip(generated_lib):
    # length 1 (just the terminating '\0') is the shortest a string can legally be - the decode
    # path's "str_len == 0 -> reject" check must not also reject this.
    def populate(d):
        d.success = False
        d.message = b""

    result, _buf = _roundtrip(generated_lib, "SetBoolResponse", SetBoolResponse, populate)
    assert result.success is False
    assert result.message == b""


def test_setbool_response_rejects_null_message(generated_lib):
    encode_size, encode, _decode, _free = _bind(generated_lib, "SetBoolResponse", SetBoolResponse)
    data = SetBoolResponse(success=True, message=None)
    assert encode_size(ctypes.byref(data)) == -3
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == -3


def test_setbool_response_encode_rejects_short_buffer(generated_lib):
    _encode_size, encode, _decode, _free = _bind(generated_lib, "SetBoolResponse", SetBoolResponse)
    data = SetBoolResponse(success=True, message=b"this does not fit")
    buf = ctypes.create_string_buffer(4)
    assert encode(ctypes.byref(data), buf, 4) == -1


def test_trigger_request_roundtrip(generated_lib):
    # Trigger's request has zero fields - encode_size must be exactly 0 and decode must accept a
    # zero-length payload (nothing to reject it on) without touching the reserved filler byte.
    encode_size, encode, decode, _free = _bind(generated_lib, "TriggerRequest", TriggerRequest)
    data = TriggerRequest()
    assert encode_size(ctypes.byref(data)) == 0
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    assert encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH) == 0
    out = TriggerRequest()
    assert decode(ctypes.byref(out), buf, 0, True) == 0


def test_trigger_response_roundtrip(generated_lib):
    result, _buf = _roundtrip(
        generated_lib,
        "TriggerResponse",
        TriggerResponse,
        lambda d: (setattr(d, "success", True), setattr(d, "message", b"triggered")),
    )
    assert result.success is True
    assert result.message == b"triggered"


def test_bulk_roundtrip(generated_lib):
    def populate(d):
        d.seq = 42
        payload = bytes(range(256)) * 4  # 1024 bytes, well under the 1442 capacity
        d.payload_count = len(payload)
        ctypes.memmove(d.payload, payload, len(payload))

    result, _buf = _roundtrip(generated_lib, "BulkData", BulkData, populate)
    assert result.seq == 42
    assert result.payload_count == 1024
    assert bytes(result.payload[:1024]) == bytes(range(256)) * 4


def test_bulk_roundtrip_empty_payload(generated_lib):
    result, _buf = _roundtrip(generated_lib, "BulkData", BulkData, lambda d: setattr(d, "seq", 7))
    assert result.seq == 7
    assert result.payload_count == 0


def test_arrays_roundtrip(generated_lib):
    def populate(d):
        d.fixed_bytes[:] = [1, 2, 3, 4]
        d.fixed_ints[:] = [-1, 0, 2_000_000_000]
        d.bounded_values_count = 3
        d.bounded_values[:3] = [10, 20, 30]
        d.samples_count = 2
        d.samples[:2] = [1.5, -2.25]

    result, _buf = _roundtrip(generated_lib, "ArraysData", ArraysData, populate)
    assert list(result.fixed_bytes) == [1, 2, 3, 4]
    assert list(result.fixed_ints) == [-1, 0, 2_000_000_000]
    assert result.bounded_values_count == 3
    assert list(result.bounded_values[:3]) == [10, 20, 30]
    assert result.samples_count == 2
    assert list(result.samples[:2]) == [1.5, -2.25]


def test_stamped_roundtrip(generated_lib):
    def populate(d):
        d.header.stamp.sec = -5
        d.header.stamp.nanosec = 123456789
        d.header.frame_id = b"map"
        d.value = 0xDEADBEEF

    result, _buf = _roundtrip(generated_lib, "StampedData", StampedData, populate)
    assert result.header.stamp.sec == -5
    assert result.header.stamp.nanosec == 123456789
    assert result.header.frame_id == b"map"
    assert result.value == 0xDEADBEEF


def test_twist_roundtrip(generated_lib):
    # Both fields (linear, angular) nest the *same* type (geometry_msgs/Vector3) - proves the
    # resolver's single shared struct/codec is usable independently for each field, not just
    # generated once and only ever exercised through one of them.
    def populate(d):
        d.linear.x, d.linear.y, d.linear.z = 1.0, 2.0, 3.0
        d.angular.x, d.angular.y, d.angular.z = -1.5, 0.0, 4.25

    result, _buf = _roundtrip(generated_lib, "TwistData", TwistData, populate)
    assert (result.linear.x, result.linear.y, result.linear.z) == (1.0, 2.0, 3.0)
    assert (result.angular.x, result.angular.y, result.angular.z) == (-1.5, 0.0, 4.25)


def test_image_roundtrip(generated_lib):
    def populate(d):
        d.header.stamp.sec = 1
        d.header.stamp.nanosec = 2
        d.header.frame_id = b"camera"
        d.height = 480
        d.width = 640
        d.encoding = b"rgb8"
        d.is_bigendian = 0
        d.step = 640 * 3
        pixels = bytes(range(256)) * 4  # 1024 bytes, under the 1400 capacity
        d.data_count = len(pixels)
        ctypes.memmove(d.data, pixels, len(pixels))

    result, _buf = _roundtrip(generated_lib, "ImageData", ImageData, populate)
    assert result.header.frame_id == b"camera"
    assert (result.height, result.width) == (480, 640)
    assert result.encoding == b"rgb8"
    assert result.step == 1920
    assert result.data_count == 1024
    assert bytes(result.data[:1024]) == bytes(range(256)) * 4


def test_uint64_encode_inplace_aliases_struct_memory(generated_lib):
    # No serialization happens at all - the payload pointer handed back must be the struct's own
    # address (see emit.emit_encode_inplace: "just hand the struct's own address over").
    encode_inplace, _decode_inplace = _bind_inplace(generated_lib, "UInt64Data", UInt64Data)
    data = UInt64Data(data=0xFEEDFACECAFEBEEF)
    payload_out = ctypes.c_void_p()
    size = encode_inplace(ctypes.byref(data), ctypes.byref(payload_out))
    assert size == 8
    assert payload_out.value == ctypes.addressof(data)


def test_uint64_decode_inplace_native_aliases_payload_buffer(generated_lib):
    encode_size, encode, _decode, _free = _bind(generated_lib, "UInt64Data", UInt64Data)
    _encode_inplace, decode_inplace = _bind_inplace(generated_lib, "UInt64Data", UInt64Data)
    original = UInt64Data(data=0x0102030405060708)
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    size = encode(ctypes.byref(original), buf, TT_MAX_BUFFER_LENGTH)
    assert size == encode_size(ctypes.byref(original))

    # Pass `buf` itself, not buf.raw[:size]: a fresh bytes object would have no reference kept
    # beyond this call, and decode_inplace's whole point is returning a pointer *into* whatever
    # buffer it was given - same dangling-pointer gotcha _roundtrip() already documents for a
    # decoded string.
    result = decode_inplace(buf, size, True)
    assert bool(result)  # non-NULL
    assert result.contents.data == 0x0102030405060708


def test_uint64_decode_inplace_rejects_reverse_endian(generated_lib):
    # A foreign-endian payload's raw bytes are NOT what UInt64Data's own memory would contain -
    # must fall back to NULL (the caller then uses the regular, copying *_decode() instead).
    _encode_inplace, decode_inplace = _bind_inplace(generated_lib, "UInt64Data", UInt64Data)
    wire = (0).to_bytes(8, "big" if sys.byteorder == "little" else "little")
    result = decode_inplace(wire, len(wire), False)
    assert not result  # NULL


def test_uint64_decode_inplace_rejects_short_buffer(generated_lib):
    _encode_inplace, decode_inplace = _bind_inplace(generated_lib, "UInt64Data", UInt64Data)
    result = decode_inplace(b"\x00" * 4, 4, True)
    assert not result  # NULL


def test_twist_encode_inplace_aliases_struct_memory(generated_lib):
    # A fixed-size struct composed *entirely* of nested fields (TwistData: two Vector3s, nothing
    # scalar of its own) is eligible too - the same whole-struct-is-the-payload trick applies
    # regardless of what's inside it, exactly because it needs "no per-field analysis" (DESIGN.md).
    encode_inplace, _decode_inplace = _bind_inplace(generated_lib, "TwistData", TwistData)
    data = TwistData()
    data.linear.x, data.linear.y, data.linear.z = 1.0, 2.0, 3.0
    payload_out = ctypes.c_void_p()
    size = encode_inplace(ctypes.byref(data), ctypes.byref(payload_out))
    assert size == 48
    assert payload_out.value == ctypes.addressof(data)


def test_bulk_encode_inplace_aliases_struct_memory(generated_lib):
    # examples/Bulk.msg isn't fully fixed-size (payload is a variable array) but its own trailing
    # byte array still makes it prefix-aliasable (layout.prefix_array_field) - the returned size
    # covers only seq + the count prefix + the *active* payload_count bytes, not the array's full
    # declared capacity.
    encode_inplace, _decode_inplace = _bind_inplace(generated_lib, "BulkData", BulkData)
    data = BulkData()
    data.seq = 7
    data.payload_count = 100
    ctypes.memmove(data.payload, bytes(range(100)), 100)
    payload_out = ctypes.c_void_p()
    size = encode_inplace(ctypes.byref(data), ctypes.byref(payload_out))
    assert size == 4 + 2 + 100
    assert payload_out.value == ctypes.addressof(data)


def test_bulk_encode_inplace_rejects_over_capacity(generated_lib):
    encode_inplace, _decode_inplace = _bind_inplace(generated_lib, "BulkData", BulkData)
    data = BulkData()
    data.payload_count = BULK_PAYLOAD_CAPACITY + 1  # ctypes lets us set this even past the
    # array's own physical size - see test_capacity.py's identical note on the non-inplace path.
    payload_out = ctypes.c_void_p()
    assert encode_inplace(ctypes.byref(data), ctypes.byref(payload_out)) == -2


def test_bulk_decode_inplace_native_aliases_payload_buffer(generated_lib):
    # Round-trips through the real *_encode() (not encode_inplace - proves decode_inplace reads
    # back exactly what a normal sender would have put on the wire, not just its own encoder's
    # output) then decode_inplace()s the result and checks it aliases the payload buffer itself.
    _encode_size, encode, _decode, _free = _bind(generated_lib, "BulkData", BulkData)
    _encode_inplace, decode_inplace = _bind_inplace(generated_lib, "BulkData", BulkData)
    original = BulkData()
    original.seq = 0xAABBCCDD
    original.payload_count = 5
    ctypes.memmove(original.payload, b"\x01\x02\x03\x04\x05", 5)

    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    size = encode(ctypes.byref(original), buf, TT_MAX_BUFFER_LENGTH)

    # Pass `buf` itself, not buf.raw[:size]: a fresh bytes object has no reference kept beyond
    # this call, and decode_inplace's whole point is returning a pointer *into* whatever buffer
    # it was given - the same dangling-pointer gotcha _roundtrip() already documents for a
    # decoded string, just one call earlier in the chain here.
    result = decode_inplace(buf, size, True)
    assert bool(result)
    assert result.contents.seq == 0xAABBCCDD
    assert result.contents.payload_count == 5
    assert bytes(result.contents.payload[:5]) == b"\x01\x02\x03\x04\x05"


def test_bulk_decode_inplace_rejects_over_capacity_count_on_wire(generated_lib):
    wire = (0).to_bytes(4, sys.byteorder) + (BULK_PAYLOAD_CAPACITY + 1).to_bytes(2, sys.byteorder)
    _encode_inplace, decode_inplace = _bind_inplace(generated_lib, "BulkData", BulkData)
    result = decode_inplace(wire, len(wire), True)
    assert not result  # NULL - a count this large can't fit the declared capacity either way


def test_bulk_decode_inplace_rejects_reverse_endian(generated_lib):
    _encode_inplace, decode_inplace = _bind_inplace(generated_lib, "BulkData", BulkData)
    wire = bytes(4) + b"\x00\x00"
    result = decode_inplace(wire, len(wire), False)
    assert not result  # NULL


def test_array_defaults_init_sets_fixed_and_variable_array_defaults(generated_lib):
    # M6: a fixed array's default fills every declared element (no count of its own to set); a
    # variable array's default also sets its own _count - fewer elements than its capacity here,
    # proving _init() doesn't also touch the untouched tail (already handled by its own
    # memset(0), never overwritten with something like a repeated last value).
    init_fn = generated_lib.ArrayDefaultsData_init
    init_fn.argtypes = [ctypes.POINTER(ArrayDefaultsData)]
    init_fn.restype = None

    data = ArrayDefaultsData(fixed_with_default=(9, 9, 9, 9), bounded_with_default_count=99)
    init_fn(ctypes.byref(data))

    assert list(data.fixed_with_default) == [1, 2, 3, 4]
    assert data.bounded_with_default_count == 2
    assert list(data.bounded_with_default[:2]) == [10, 20]
    assert list(data.bounded_with_default[2:]) == [0, 0]


def test_pingpong_roundtrip(generated_lib):
    result, _buf = _roundtrip(
        generated_lib, "PingPongRequest", PingPongRequest, lambda d: (setattr(d, "seq", 7), setattr(d, "timestamp", 123456789))
    )
    assert result.seq == 7
    assert result.timestamp == 123456789


def test_setbool_response_layout_has_one_byte_gap(generated_lib):
    # Pins the CDR-4 padding this test file's docstring is really about: `bool success` (1 byte)
    # must be followed by exactly 1 padding byte before the string's 2-byte-aligned length
    # prefix - not 0 (misaligned) and not 3 (over-padded, as a naive "always pad to 4" rule
    # would do). Encodes by hand instead of through *_encode() so a bug in the generator's own
    # alignment math can't cancel out against an equally wrong expectation here.
    encode_size, encode, _decode, _free = _bind(generated_lib, "SetBoolResponse", SetBoolResponse)
    data = SetBoolResponse(success=True, message=b"x")
    buf = ctypes.create_string_buffer(TT_MAX_BUFFER_LENGTH)
    size = encode(ctypes.byref(data), buf, TT_MAX_BUFFER_LENGTH)
    raw = buf.raw[:size]
    assert raw[0] == 1  # success = true
    assert raw[1] == 0  # the one padding byte
    length = int.from_bytes(raw[2:4], sys.byteorder)  # encode() always writes host-native
    assert length == 2  # "x" + '\0'
    assert raw[4:6] == b"x\x00"
    assert size == 8  # 1 + 1 (pad) + 2 (len) + 2 (data) + 2 (pad to 4) = 8
