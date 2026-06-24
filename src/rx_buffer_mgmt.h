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
void tt_rx_buffer_drop_topic_counts(struct tt_Node* node, struct tt_RxBuffer* rx_buffer);

// Returns the cached number of pending DATA submessages that remain takable for
// a callback-less subscriber. Callback-based subscribers always return 0.
uint32_t tt_rx_buffer_get_takable_count(const struct tt_Subscriber* subscriber);

// Takes one pending DATA submessage for subscriber.
// Returns true if a matching DATA submessage was decoded and consumed.
bool tt_rx_buffer_take_topic(struct tt_Subscriber* subscriber, void* recv_topic_data_buffer, uint64_t* timestamp);
