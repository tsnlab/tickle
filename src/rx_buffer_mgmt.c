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

static tt_lock_state_t lock_endpoints(struct tt_Node* node) {
    return tt_lock(&node->endpoint_lock);
}

static void unlock_endpoints(struct tt_Node* node, tt_lock_state_t state) {
    tt_unlock(&node->endpoint_lock, state);
}

enum take_topic_result {
    TAKE_TOPIC_ERROR = -1,     // Malformed buffer or decode failure.
    TAKE_TOPIC_NOT_FOUND = -2, // Current rx_buffer/submessage does not contain the requested endpoint.
    TAKE_TOPIC_FOUND = 1,      // Matching DATA was decoded and consumed.
};

// Called only while rx_buffer_lock is held. Unlinks and frees one queued packet
// whose pending DATA submessages have all been taken or intentionally dropped.
static void remove_rx_buffer_locked(struct tt_Node* node, struct tt_RxBuffer** rx_buffer_ptr) {
    struct tt_RxBuffer* rx_buffer = *rx_buffer_ptr;
    *rx_buffer_ptr = rx_buffer->next_buffer;
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
    for (struct tt_RxBuffer* current = rx_buffer; current != NULL; current = current->next_buffer) {
        tt_rx_buffer_drop_topic_counts(node, current);
    }
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
            tt_rx_buffer_drop_topic_counts(node, old_buffer);
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

static struct tt_Subscriber* find_subscriber_locked(struct tt_Node* node, uint32_t endpoint_id) {
    tt_lock_state_t state = lock_endpoints(node);

    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL) {
            continue;
        }
        if (endpoint->kind == tt_KIND_TOPIC_SUBSCRIBER && endpoint->id == endpoint_id) {
            unlock_endpoints(node, state);
            return (struct tt_Subscriber*)endpoint;
        }
    }

    unlock_endpoints(node, state);
    return NULL;
}

static bool decrement_subscriber_rx_data_count(struct tt_Node* node, uint32_t endpoint_id) {
    struct tt_Subscriber* subscriber = find_subscriber_locked(node, endpoint_id);
    // Only callback-less subscribers cache DATA for later take(); callback
    // subscribers already received the DATA during packet processing.
    if (subscriber == NULL || subscriber->callback != NULL) {
        return false;
    }
    if (subscriber->rx_data_count > 0) {
        subscriber->rx_data_count--;
    }
    return true;
}

void tt_rx_buffer_drop_topic_counts(struct tt_Node* node, struct tt_RxBuffer* rx_buffer) {
    if (node == NULL || rx_buffer == NULL || rx_buffer->remaining_topic_count <= 0) {
        return;
    }

    uint8_t* buffer = rx_buffer->rx_data;
    uint32_t tail = rx_buffer->len;
    uint32_t head = 0;
    uint32_t dropped_count = 0;
    const uint32_t pending_count = (uint32_t)rx_buffer->remaining_topic_count;

    struct tt_Header* header = retrieve_header(buffer, &head, tail, sizeof(struct tt_Header));
    if (header == NULL || get_packet_endian(header, &(bool) {false}) == TAKE_TOPIC_ERROR) {
        return;
    }

    while (true) {
        uint32_t submessage_start = head;
        struct tt_SubmessageHeader* submessage_header =
            retrieve_header(buffer, &head, tail, sizeof(struct tt_SubmessageHeader));
        if (submessage_header == NULL) {
            return;
        }

        if (submessage_header->length < sizeof(struct tt_SubmessageHeader) ||
            submessage_header->length > tail - submessage_start) {
            return;
        }

        const uint32_t body_tail = submessage_start + submessage_header->length;
        if (is_data_submessage_for_node(node, submessage_header)) {
            uint32_t data_head = head;
            struct tt_DataHeader* data_header =
                retrieve_header(buffer, &data_head, body_tail, sizeof(struct tt_DataHeader));
            if (data_header != NULL && decrement_subscriber_rx_data_count(node, data_header->endpoint_id)) {
                dropped_count++;
                if (dropped_count >= pending_count) {
                    return;
                }
            }
        }

        head = body_tail;
    }
}

uint32_t tt_rx_buffer_get_takable_count(const struct tt_Subscriber* subscriber) {
    // The count is meaningful only for pull-style subscribers. Push-style
    // callback subscribers never leave DATA queued for tt_Subscriber_take().
    if (subscriber == NULL || subscriber->node == NULL || subscriber->callback != NULL) {
        return 0;
    }

    struct tt_Node* node = subscriber->node;
    // rx_data_count is updated together with rx_buffer_list mutations, so read
    // it under the same lock used by take/drop paths.
    tt_lock_state_t state = lock_rx_buffer_list(node);
    uint32_t count = subscriber->rx_data_count;
    unlock_rx_buffer_list(node, state);
    return count;
}

// Called only while rx_buffer_lock is held. Consumes one DATA submessage when it
// matches subscriber; otherwise reports that this submessage is not the target.
static enum take_topic_result take_data_submessage_locked(struct tt_Subscriber* subscriber,
                                                          struct tt_RxBuffer** rx_buffer_ptr, uint32_t submessage_start,
                                                          bool is_native_endian, void* recv_topic_data_buffer,
                                                          uint64_t* timestamp) {
    struct tt_Node* node = subscriber->node;
    struct tt_Topic* topic = subscriber->topic;
    const uint32_t endpoint_id = subscriber->endpoint.id;
    struct tt_RxBuffer* rx_buffer = *rx_buffer_ptr;
    uint8_t* buffer = rx_buffer->rx_data;
    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)(buffer + submessage_start);
    uint32_t data_head = submessage_start + sizeof(struct tt_SubmessageHeader);
    const uint32_t body_tail = submessage_start + submessage_header->length;

    struct tt_DataHeader* data_header = retrieve_header(buffer, &data_head, body_tail, sizeof(struct tt_DataHeader));
    if (data_header == NULL) {
        return TAKE_TOPIC_ERROR;
    }

    if (data_header->endpoint_id != endpoint_id) {
        return TAKE_TOPIC_NOT_FOUND;
    }

    // Decode lazily at take time. The original wire buffer must remain unchanged
    // until decoding succeeds.
    int32_t decoded = topic->data_decode((struct tt_Data*)recv_topic_data_buffer, buffer + data_head,
                                         body_tail - data_head, is_native_endian);
    if (decoded < 0) {
        return TAKE_TOPIC_ERROR;
    }
    if (timestamp != NULL) {
        *timestamp = data_header->timestamp;
    }

    // Remove the consumed submessage in place so the same DATA cannot be taken
    // again from this rx_buffer.
    const uint32_t submessage_length = body_tail - submessage_start;
    const uint32_t next_submessage = submessage_start + submessage_length;
    _tt_memmove(buffer + submessage_start, buffer + next_submessage, rx_buffer->len - next_submessage);
    rx_buffer->len -= submessage_length;
    rx_buffer->remaining_topic_count--;
    // Keep the cached per-subscriber takable count in sync with the packet's
    // remaining pull-deliverable DATA count after a successful take.
    if (subscriber->rx_data_count > 0) {
        subscriber->rx_data_count--;
    }

    // This was the last untaken DATA submessage in the packet. Once it reaches
    // zero, the rx_buffer no longer has pull-deliverable content and can leave
    // the pending list.
    if (rx_buffer->remaining_topic_count <= 0) {
        remove_rx_buffer_locked(node, rx_buffer_ptr);
    }

    return TAKE_TOPIC_FOUND;
}

// Called only while rx_buffer_lock is held. Scans one stored packet and, if it
// contains matching DATA, decodes and removes exactly that submessage.
static enum take_topic_result take_topic_from_rx_buffer_locked(struct tt_Subscriber* subscriber,
                                                               struct tt_RxBuffer** rx_buffer_ptr,
                                                               void* recv_topic_data_buffer, uint64_t* timestamp) {
    struct tt_Node* node = subscriber->node;
    struct tt_RxBuffer* rx_buffer = *rx_buffer_ptr;
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
            enum take_topic_result result = take_data_submessage_locked(
                subscriber, rx_buffer_ptr, submessage_start, is_native_endian, recv_topic_data_buffer, timestamp);
            if (result != TAKE_TOPIC_NOT_FOUND) {
                return result;
            }
        }

        head = body_tail;
    }
}

// Finds the oldest pending DATA submessage for subscriber, decodes it into the
// caller-provided buffer, then removes that submessage from its owning packet.
// Returns true if a matching DATA submessage was decoded and consumed.
bool tt_rx_buffer_take_topic(struct tt_Subscriber* subscriber, void* recv_topic_data_buffer, uint64_t* timestamp) {
    if (subscriber == NULL || subscriber->node == NULL || subscriber->topic == NULL ||
        subscriber->topic->data_decode == NULL || recv_topic_data_buffer == NULL) {
        return false;
    }

    struct tt_Node* node = subscriber->node;
    tt_lock_state_t state = lock_rx_buffer_list(node);
    bool taken = false;
    struct tt_RxBuffer** rx_buffer_ptr = &node->rx_buffer_list;

    // Outer loop: walk the queued packets from oldest to newest.
    // rx_data_count is a cheap fast path: if it is zero, no queued packet can
    // contain DATA for this subscriber.
    while (subscriber->rx_data_count > 0 && *rx_buffer_ptr != NULL) {
        // Inner scan is delegated: one rx_buffer may contain multiple
        // submessages, only one of which should be consumed per take call.
        enum take_topic_result result =
            take_topic_from_rx_buffer_locked(subscriber, rx_buffer_ptr, recv_topic_data_buffer, timestamp);
        if (result == TAKE_TOPIC_FOUND) {
            taken = true;
            break;
        }
        if (result == TAKE_TOPIC_ERROR) {
            break;
        }

        rx_buffer_ptr = &(*rx_buffer_ptr)->next_buffer;
    }

    unlock_rx_buffer_list(node, state);
    return taken;
}

// NOLINTEND(misc-include-cleaner)
