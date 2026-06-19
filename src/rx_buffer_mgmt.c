#include "rx_buffer_mgmt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tickle/hal.h>

// This file intentionally centralizes TickLE RX wire-buffer handling, so it uses
// protocol structs and constants exposed by <tickle/tickle.h> through its own header.
// NOLINTBEGIN(misc-include-cleaner)

static void* retrieve_header(void* buffer, uint32_t* head, uint32_t tail, uint32_t length) {
    if (*head + length > tail) {
        return NULL;
    }

    void* p = (uint8_t*)buffer + *head;
    *head += length;
    return p;
}

static tt_lock_state_t lock_rx_buffer_list(struct tt_Node* node) {
    return tt_lock(&node->rx_buffer_lock);
}

static void unlock_rx_buffer_list(struct tt_Node* node, tt_lock_state_t state) {
    tt_unlock(&node->rx_buffer_lock, state);
}

enum take_topic_result {
    TAKE_TOPIC_ERROR = -1,     // Malformed buffer or decode failure.
    TAKE_TOPIC_NOT_FOUND = -2, // Current rx_buffer/submessage does not contain the requested endpoint.
    TAKE_TOPIC_FOUND = 1,      // Matching DATA was decoded and consumed.
};

// Called only while rx_buffer_lock is held. Unlinks and frees one queued packet
// whose pending DATA submessages have all been taken or intentionally dropped.
static void remove_rx_buffer_locked(struct tt_Node* node, struct tt_RxBuffer* previous, struct tt_RxBuffer* rx_buffer) {
    struct tt_RxBuffer* next_buffer = rx_buffer->next_buffer;
    if (previous == NULL) {
        node->rx_buffer_list = next_buffer;
    } else {
        previous->next_buffer = next_buffer;
    }
    rx_buffer->next_buffer = NULL;
    if (node->rx_buffer_count > 0) {
        node->rx_buffer_count--;
    }
    _tt_free(rx_buffer);
}

void tt_rx_buffer_list_init(struct tt_Node* node) {
    node->rx_buffer_list = NULL;
    node->rx_buffer_count = 0;
    tt_lock_init(&node->rx_buffer_lock);
}

void tt_rx_buffer_list_deinit(struct tt_Node* node) {
    tt_rx_buffer_list_clear(node);
    tt_lock_deinit(&node->rx_buffer_lock);
}

void tt_rx_buffer_list_clear(struct tt_Node* node) {
    // Detach the list while locked, then free nodes after unlocking so the lock
    // is held only for the shared-list mutation.
    tt_lock_state_t state = lock_rx_buffer_list(node);
    struct tt_RxBuffer* rx_buffer = node->rx_buffer_list;
    node->rx_buffer_list = NULL;
    node->rx_buffer_count = 0;
    unlock_rx_buffer_list(node, state);

    while (rx_buffer != NULL) {
        struct tt_RxBuffer* next_buffer = rx_buffer->next_buffer;
        _tt_free(rx_buffer);
        rx_buffer = next_buffer;
    }
}

void tt_rx_buffer_list_append(struct tt_Node* node, struct tt_RxBuffer* rx_buffer) {
    rx_buffer->next_buffer = NULL;

    tt_lock_state_t state = lock_rx_buffer_list(node);

    // Current history policy is FIFO old-drop: if the queue is full, discard the
    // oldest packet before appending the newly received packet.
    if (node->rx_buffer_count >= tt_MAX_RX_BUFFER_COUNT) {
        struct tt_RxBuffer* old_buffer = node->rx_buffer_list;
        if (old_buffer != NULL) {
            node->rx_buffer_list = old_buffer->next_buffer;
            node->rx_buffer_count--;
            _tt_free(old_buffer);
        }
    }

    if (node->rx_buffer_list == NULL) {
        node->rx_buffer_list = rx_buffer;
    } else {
        struct tt_RxBuffer* tail = node->rx_buffer_list;
        while (tail->next_buffer != NULL) {
            tail = tail->next_buffer;
        }
        tail->next_buffer = rx_buffer;
    }

    node->rx_buffer_count++;
    unlock_rx_buffer_list(node, state);
}

static enum take_topic_result get_packet_endian(struct tt_Header* header, bool* is_native_endian) {
    if (tt_is_native_endian(header)) {
        *is_native_endian = true;
        return TAKE_TOPIC_FOUND;
    }
    if (tt_is_reverse_endian(header)) {
        *is_native_endian = false;
        return TAKE_TOPIC_FOUND;
    }
    return TAKE_TOPIC_ERROR;
}

static bool is_data_submessage_for_node(struct tt_Node* node, const struct tt_SubmessageHeader* submessage_header) {
    return (submessage_header->receiver == tt_SUBMESSAGE_ID_ALL || submessage_header->receiver == node->id) &&
           submessage_header->type == tt_SUBMESSAGE_TYPE_DATA;
}

// Called only while rx_buffer_lock is held. Consumes one DATA submessage when it
// matches endpoint_id; otherwise reports that this submessage is not the target.
static enum take_topic_result take_data_submessage_locked(struct tt_Node* node, struct tt_RxBuffer* prev_buffer,
                                                          struct tt_RxBuffer* rx_buffer, uint32_t endpoint_id,
                                                          uint32_t submessage_start, uint32_t data_head,
                                                          uint32_t body_tail, bool is_native_endian,
                                                          tt_DATA_DECODE data_decode, void* recv_topic_data_buffer,
                                                          int32_t* decoded) {
    uint8_t* buffer = rx_buffer->rx_data;
    struct tt_DataHeader* data_header = retrieve_header(buffer, &data_head, body_tail, sizeof(struct tt_DataHeader));
    if (data_header == NULL) {
        return TAKE_TOPIC_ERROR;
    }

    if (data_header->endpoint_id != endpoint_id) {
        return TAKE_TOPIC_NOT_FOUND;
    }

    // Decode lazily at take time. The original wire buffer must remain unchanged
    // until decoding succeeds.
    *decoded = data_decode((struct tt_Data*)recv_topic_data_buffer, buffer + data_head, body_tail - data_head,
                           is_native_endian);
    if (*decoded < 0) {
        return TAKE_TOPIC_ERROR;
    }

    // Remove the consumed submessage in place so the same DATA cannot be taken
    // again from this rx_buffer.
    const uint32_t submessage_length = body_tail - submessage_start;
    const uint32_t next_submessage = submessage_start + submessage_length;
    _tt_memmove(buffer + submessage_start, buffer + next_submessage, rx_buffer->len - next_submessage);
    rx_buffer->len -= submessage_length;
    rx_buffer->remaining_topic_count--;

    // This was the last untaken DATA submessage in the packet. Once it reaches
    // zero, the rx_buffer no longer has pull-deliverable content and can leave
    // the pending list.
    if (rx_buffer->remaining_topic_count <= 0) {
        remove_rx_buffer_locked(node, prev_buffer, rx_buffer);
    }

    return TAKE_TOPIC_FOUND;
}

// Called only while rx_buffer_lock is held. Scans one stored packet and, if it
// contains matching DATA, decodes and removes exactly that submessage.
static enum take_topic_result take_topic_from_rx_buffer_locked(struct tt_Node* node, struct tt_RxBuffer* prev_buffer,
                                                               struct tt_RxBuffer* rx_buffer, uint32_t endpoint_id,
                                                               tt_DATA_DECODE data_decode, void* recv_topic_data_buffer,
                                                               int32_t* decoded) {
    uint8_t* buffer = rx_buffer->rx_data;
    uint32_t tail = rx_buffer->len;
    uint32_t head = 0;

    struct tt_Header* header = retrieve_header(buffer, &head, tail, sizeof(struct tt_Header));
    if (header == NULL) {
        return TAKE_TOPIC_ERROR;
    }

    bool is_native_endian = false;
    if (get_packet_endian(header, &is_native_endian) == TAKE_TOPIC_ERROR) {
        return TAKE_TOPIC_ERROR;
    }

    while (true) {
        uint32_t submessage_start = head;
        struct tt_SubmessageHeader* submessage_header =
            retrieve_header(buffer, &head, tail, sizeof(struct tt_SubmessageHeader));
        if (submessage_header == NULL) {
            return TAKE_TOPIC_NOT_FOUND;
        }

        if (submessage_header->length < sizeof(struct tt_SubmessageHeader) ||
            submessage_header->length > tail - submessage_start) {
            return TAKE_TOPIC_ERROR;
        }

        const uint32_t body_tail = submessage_start + submessage_header->length;
        if (is_data_submessage_for_node(node, submessage_header)) {
            uint32_t data_head = head;
            enum take_topic_result result =
                take_data_submessage_locked(node, prev_buffer, rx_buffer, endpoint_id, submessage_start, data_head,
                                            body_tail, is_native_endian, data_decode, recv_topic_data_buffer, decoded);
            if (result != TAKE_TOPIC_NOT_FOUND) {
                return result;
            }
        }

        head = body_tail;
    }
}

// Finds the oldest pending DATA submessage for endpoint_id, decodes it into the
// caller-provided buffer, then removes that submessage from its owning packet.
// Returns decoded byte length, or -1 if no matching DATA exists, decoding fails,
// or a queued buffer is malformed. A zero byte return is a valid decode result.
int32_t tt_rx_buffer_take_topic(struct tt_Node* node, uint32_t endpoint_id, tt_DATA_DECODE data_decode,
                                void* recv_topic_data_buffer) {
    if (node == NULL || data_decode == NULL || recv_topic_data_buffer == NULL) {
        return -1;
    }

    tt_lock_state_t state = lock_rx_buffer_list(node);
    struct tt_RxBuffer* prev_buffer = NULL;
    struct tt_RxBuffer* rx_buffer = node->rx_buffer_list;

    // Outer loop: walk the queued packets from oldest to newest.
    while (rx_buffer != NULL) {
        // Inner scan is delegated: one rx_buffer may contain multiple
        // submessages, only one of which should be consumed per take call.
        int32_t decoded = -1;
        enum take_topic_result result = take_topic_from_rx_buffer_locked(node, prev_buffer, rx_buffer, endpoint_id,
                                                                         data_decode, recv_topic_data_buffer, &decoded);
        if (result == TAKE_TOPIC_FOUND) {
            unlock_rx_buffer_list(node, state);
            return decoded;
        }
        if (result == TAKE_TOPIC_ERROR) {
            unlock_rx_buffer_list(node, state);
            return -1;
        }

        prev_buffer = rx_buffer;
        rx_buffer = rx_buffer->next_buffer;
    }

    unlock_rx_buffer_list(node, state);
    return -1;
}

// NOLINTEND(misc-include-cleaner)
