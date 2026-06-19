#pragma once

#include <stdint.h>

#include <tickle/hal.h>
#include <tickle/tickle.h>

void tt_rx_buffer_list_init(struct tt_Node* node);
void tt_rx_buffer_list_deinit(struct tt_Node* node);

// Takes ownership of rx_buffer and queues it for callback-less subscribers.
// If the queue is already full, the oldest queued buffer is dropped first.
void tt_rx_buffer_list_append(struct tt_Node* node, struct tt_RxBuffer* rx_buffer);

void tt_rx_buffer_list_clear(struct tt_Node* node);

// Takes one pending DATA submessage for endpoint_id.
// Returns decoded byte length, or -1 if none is available or an error occurs.
// A zero byte return is a valid decoded DATA length.
int32_t tt_rx_buffer_take_topic(struct tt_Node* node, uint32_t endpoint_id, tt_DATA_DECODE data_decode,
                                void* recv_topic_data_buffer);
