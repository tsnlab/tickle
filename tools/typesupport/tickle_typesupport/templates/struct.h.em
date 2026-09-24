@# One message struct's declarations: constants + struct + (optional) init + the four codec
@# functions + static_assert. Included from topic.h.em / service.h.em once per struct (a .srv
@# has two - Request and Response). All per-field C is pre-rendered into *_lines strings by
@# emit.py - this template only arranges them, it holds no wire-format knowledge of its own.
@[for line in constant_lines]@
@(line)
@[end for]@
@[for line in capacity_lines]@
@(line)
@[end for]@

#pragma pack(push, 4)
struct @(name) {
@[for line in field_lines]@
    @(line)
@[end for]@
};
#pragma pack(pop)

@[if has_init]@
void @(name)_init(struct @(name)* data);
@[end if]@
int32_t @(name)_encode_size(struct @(name)* data);
int32_t @(name)_encode(struct @(name)* data, uint8_t* payload, uint32_t len);
int32_t @(name)_decode(struct @(name)* data, const uint8_t* payload, uint32_t len, bool is_native_endian);
@[if has_inplace]@
int32_t @(name)_encode_inplace(struct @(name)* data, const uint8_t** payload_out);
struct @(name)* @(name)_decode_inplace(const uint8_t* payload, uint32_t len, bool is_native_endian);
@[end if]@
void @(name)_free(struct @(name)* data);

@[if is_fixed_size]@
_Static_assert(sizeof(struct @(name)) == @(padded_wire_size), "@(name) must match its CDR-4 wire size - ABI mismatch");
@[end if]@
@# A flag the compiler evaluates, not an assertion that fails the build (2026-09-24).
@#
@# It was _Static_assert until a real package could no longer be generated at all.
@# performance_test declares Array4k, Array32k and PointCloud1m alongside the small types anyone
@# actually benchmarks, and rosidl_generate_interfaces() hands the generator every type in the
@# list. One type too large for a datagram therefore failed the whole package - including for
@# consumers that never touch it - and rosidl_typesupport_tickle_c could not build
@# performance_test at all. That went unnoticed for nine days because nothing rebuilt it.
@#
@# The assertion was never the safety net it looked like. tt_Publisher_publish() already refuses a
@# message whose data_encode_size() exceeds tt_MAX_BUFFER_LENGTH, with an error naming the size,
@# so an oversized type cannot be put on the wire whether or not this header objects at compile
@# time. What the assertion added was failing early - and it failed early for types the consumer
@# had not asked for, which is the wrong trade.
@#
@# Left to the compiler rather than decided here because the generator does not know
@# tt_MAX_BUFFER_LENGTH: it is a C constant the consumer configures (config.h), which is exactly
@# why this was written as an assertion in the first place.
#define @(name)_FITS_ONE_DATAGRAM (@(max_wire_size) <= tt_MAX_BUFFER_LENGTH)
