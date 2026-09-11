@# One message struct's declarations: constants + struct + (optional) init + the four codec
@# functions + static_assert. Included from topic.h.em / service.h.em once per struct (a .srv
@# has two - Request and Response). All per-field C is pre-rendered into *_lines strings by
@# emit.py - this template only arranges them, it holds no wire-format knowledge of its own.
@[for line in constant_lines]@
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
void @(name)_free(struct @(name)* data);

@[if is_fixed_size]@
_Static_assert(sizeof(struct @(name)) == @(wire_size), "@(name) must match its CDR-4 wire size - ABI mismatch");
@[end if]@
_Static_assert(@(max_wire_size) <= tt_MAX_BUFFER_LENGTH, "@(name)'s worst-case wire size exceeds a single datagram");
