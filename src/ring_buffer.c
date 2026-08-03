#include "ring_buffer.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/hal.h>
#include <tickle/tickle.h>

struct tt_ring_buffer ring_buffer_create(uint32_t elem_size, uint32_t capacity) {
    struct tt_ring_buffer buffer = {
        0,
    };
    ring_buffer_init(&buffer, elem_size, capacity);
    return buffer;
}

int32_t ring_buffer_init(struct tt_ring_buffer* buffer, uint32_t elem_size, uint32_t capacity) {
    if (buffer == NULL || elem_size == 0 || capacity == 0) {
        return -1;
    }
    buffer->elem_size = 8 * (1 + (elem_size - 1) / 8); // 8 byte alignment
    buffer->capacity = capacity;
    buffer->read_end = 0;
    buffer->write_end = 0;
    buffer->data = _tt_malloc((uint64_t)(capacity + 1) * elem_size);
    if (buffer->data == NULL) {
        return -1;
    }
    return 0;
}

void ring_buffer_destroy(struct tt_ring_buffer* buffer) {
    _tt_free(buffer->data);
    *buffer = (struct tt_ring_buffer) {
        0,
    };
}

// read_end is inclusive, write_end is exclusive index
int32_t ring_buffer_push(struct tt_ring_buffer* buffer, void* push_from) {
    uint32_t write_end = buffer->write_end;
    uint32_t new_write_end = (write_end + 1) % (buffer->capacity + 1);
    uint32_t elem_size = buffer->elem_size;

    // buffer is full
    if (new_write_end == buffer->read_end) {
        return -1;
    }
    memcpy(&buffer->data[(size_t)write_end * elem_size], push_from, elem_size);
    buffer->write_end = new_write_end;
    return 0;
}

int32_t ring_buffer_pop(struct tt_ring_buffer* buffer, void* pop_to) {
    uint32_t read_end = buffer->read_end;
    uint32_t elem_size = buffer->elem_size;

    // nothing to read
    if (read_end == buffer->write_end) {
        return -1;
    }
    memcpy(pop_to, &buffer->data[(size_t)read_end * elem_size], elem_size);
    buffer->read_end = (read_end + 1) % (buffer->capacity + 1);
    return 0;
}

uint32_t ring_buffer_size(const struct tt_ring_buffer* buffer) {
    uint32_t read_end = buffer->read_end;
    uint32_t write_end = buffer->write_end;

    if (read_end <= write_end) {
        return write_end - read_end;
    }
    return write_end + buffer->capacity + 1 - read_end;
}
