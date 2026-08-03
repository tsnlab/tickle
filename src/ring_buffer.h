#include <stdint.h>

#include <tickle/tickle.h>

struct rx_data_buffer {
    void* data;
    uint32_t len;
    bool is_native_endian;
};

int32_t ring_buffer_init(struct tt_ring_buffer* buffer, uint32_t elem_size, uint32_t capacity);
void ring_buffer_destroy(struct tt_ring_buffer* buffer);
int32_t ring_buffer_push(struct tt_ring_buffer* buffer, void* push_from);
int32_t ring_buffer_pop(struct tt_ring_buffer* buffer, void* pop_to);
uint32_t ring_buffer_size(const struct tt_ring_buffer* buffer);
