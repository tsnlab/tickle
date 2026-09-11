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

void @(name)_free(struct @(name)* data) {
@[for line in free_lines]@
    @(line)
@[end for]@
}
