@# One message struct's function bodies - see struct.h.em for its declarations.
@[if has_init]@
void @(name)_init(struct @(name)* data) {
@[for line in init_lines]@
    @(line)
@[end for]@
}

@[end if]@
int32_t @(name)_encode_size(struct @(name)* data) {
@[for line in encode_size_lines]@
    @(line)
@[end for]@
}

int32_t @(name)_encode(struct @(name)* data, uint8_t* payload, uint32_t len) {
@[for line in encode_lines]@
    @(line)
@[end for]@
}

int32_t @(name)_decode(struct @(name)* data, const uint8_t* payload, uint32_t len, bool is_native_endian) {
@[for line in decode_lines]@
    @(line)
@[end for]@
}

@[if has_inplace]@
int32_t @(name)_encode_inplace(struct @(name)* data, const uint8_t** payload_out) {
@[for line in encode_inplace_lines]@
    @(line)
@[end for]@
}

struct @(name)* @(name)_decode_inplace(const uint8_t* payload, uint32_t len, bool is_native_endian) {
@[for line in decode_inplace_lines]@
    @(line)
@[end for]@
}

@[end if]@
void @(name)_free(struct @(name)* data) {
@[for line in free_lines]@
    @(line)
@[end for]@
}
