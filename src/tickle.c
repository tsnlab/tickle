/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "consts.h"
#include "encoding.h"
#include "log.h"

#define UNUSED(x) (void)(x)

// A message's CDR payload (see "Interface serialization (TickLE CDR-4)" in DESIGN.md) is written
// straight after the framing headers and must land at a 4-byte-aligned offset in tx_buffer /
// rx_buffer so a generated codec can read/write aligned scalars without memcpy. Guard the two
// things that make that true: the header sizes, and the buffers' own alignment.
#define TT_FRAMING_HDR (sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader))
_Static_assert(sizeof(struct tt_CallRequestHeader) == 8, "tt_CallRequestHeader must be 8 bytes");
_Static_assert((TT_FRAMING_HDR + sizeof(struct tt_DataHeader)) % 4 == 0, "DATA payload not 4-aligned");
_Static_assert((TT_FRAMING_HDR + sizeof(struct tt_CallRequestHeader)) % 4 == 0, "CALLREQUEST payload not 4-aligned");
_Static_assert((TT_FRAMING_HDR + sizeof(struct tt_CallResponseHeader)) % 4 == 0, "CALLRESPONSE payload not 4-aligned");
_Static_assert(offsetof(struct tt_Node, tx_buffer) % 4 == 0, "tx_buffer not 4-aligned in tt_Node");
_Static_assert(offsetof(struct tt_Node, rx_buffer) % 4 == 0, "rx_buffer not 4-aligned in tt_Node");
#undef TT_FRAMING_HDR

static uint32_t calculate_latency(uint64_t start, uint64_t end) {
    return end > start ? (uint32_t)(end - start) : 0;
}

static struct tt_SubmessageHeader* start_encode(struct tt_Node* node, uint8_t type, uint8_t receiver) {
    if (node->tx_tail + sizeof(struct tt_SubmessageHeader) >= node->tx_size) {
        TT_LOG_WARNING("Lack of tx buffer");
        return NULL;
    }

    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)(node->tx_buffer + node->tx_tail);
    node->tx_tail += sizeof(struct tt_SubmessageHeader);

    submessage_header->type = type;
    submessage_header->receiver = receiver;
    submessage_header->length = 0;

    return submessage_header;
}

static void* encode(struct tt_Node* node, uint32_t len) {
    if (node->tx_tail + len >= node->tx_size) {
        TT_LOG_WARNING("Lack of tx buffer");
        return NULL;
    }

    return tt_encode_buffer(node->tx_buffer, &node->tx_tail, len);
}

static bool encode_string(struct tt_Node* node, const char* str) {
    if (!tt_encode_string(node->tx_buffer, &node->tx_tail, node->tx_size, str)) {
        TT_LOG_WARNING("Lack of tx buffer");
        return false;
    }
    return true;
}

static void rollback(struct tt_Node* node, uint32_t old_tx_tail) {
    node->tx_tail = old_tx_tail;
}

// peer_count == 0 (peers may be NULL) means "no override, send to the node's usual broadcast
// address" - the direct successor to the old dest_ip == 0 sentinel. peer_count >= 1 sends the
// same already-encoded buffer to each peer in turn via tt_send_to() instead - used by
// process_callrequest() (a peer list of length 1: the CallResponse straight back to its own
// requester, see its own comment on why that's safe there specifically) and by
// tt_Publisher_publish()/tt_Client_call()/resend_call_request() (a short list of known peers, see
// tt_UNICAST_PEER_THRESHOLD) once discovery has learned a handful of them.
static bool flush_tx(struct tt_Node* node, uint32_t len, const struct tt_Peer* peers, uint8_t peer_count) {
    // Check at least 1 submessage is contained
    if (len < sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader)) {
        return true; // Nothing to flush
    }

    // Not possible
    if (len > tt_MAX_BUFFER_LENGTH) {
        TT_LOG_ERROR("Flush length %u exceeds tt_MAX_BUFFER_LENGTH %d", len, tt_MAX_BUFFER_LENGTH);
        return false;
    }

    struct tt_Header* header = (struct tt_Header*)node->tx_buffer;
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = node->id;

    bool sent_ok = true;
    if (peer_count == 0) {
        sent_ok = tt_send(node, node->tx_buffer, len) >= 0;
    } else {
        for (uint8_t i = 0; i < peer_count; i++) {
            if (tt_send_to(node, node->tx_buffer, len, peers[i].ip, peers[i].port) < 0) {
                sent_ok = false;
                break;
            }
        }
    }
    if (!sent_ok) {
        TT_LOG_ERROR("Cannot send packet: %s", strerror(errno));
        return false;
    }

    // Whatever was pending (including any batched UPDATE - see node_update()/node_flush()) just
    // went out in `len` bytes above, unconditionally: a deferred-flush's `base` always covers
    // everything appended before the submessage that triggered it, which includes an earlier
    // UPDATE if one was still batched.
    node->tx_has_pending_update = false;

    _tt_memmove(node->tx_buffer + sizeof(struct tt_Header), node->tx_buffer + len, node->tx_tail - len);
    node->tx_tail = sizeof(struct tt_Header) + (node->tx_tail - len);

    return true;
}

static bool end_encode(struct tt_Node* node, struct tt_SubmessageHeader* submessage_header, bool is_flush,
                       const struct tt_Peer* peers, uint8_t peer_count) {
    // Set submessage header length
    size_t length = (uintptr_t)node->tx_buffer + node->tx_tail - (uintptr_t)submessage_header;
    size_t roundup = ROUNDUP(length) - length;
    // tx_tail before this submessage was appended - what to flush when it doesn't fit and
    // has to be deferred to the next buffer instead of going out in this one.
    uint32_t base = (uint32_t)(node->tx_tail - length);

    // What to flush if is_flush ends up true below: the whole (padded) buffer including this
    // submessage by default, unless a branch below decides this submessage doesn't fit and
    // must be deferred, in which case it's overridden to `base` (everything before it).
    uint32_t flush_len;

    if (node->tx_tail + roundup <= tt_MAX_BUFFER_LENGTH) { // tail in below the buffer
        submessage_header->length = length + roundup;
        node->tx_tail += roundup;
        flush_len = node->tx_tail;
    } else if (node->tx_tail <= tt_MAX_BUFFER_LENGTH &&
               node->tx_tail + roundup > tt_MAX_BUFFER_LENGTH) { // tail exceeds buffer if roundup
        submessage_header->length = length;
        is_flush = true;
        flush_len = base;
    } else { // tail exceeds buffer: this submessage alone (or with roundup) already overshoots
             // tt_MAX_BUFFER_LENGTH, so it can't go out in this flush either - defer it, same
             // as the previous case, instead of trying to flush past flush_tx()'s own limit.
        submessage_header->length = length + roundup;
        node->tx_tail += roundup;
        is_flush = true;
        flush_len = base;
    }

    // Case 1: Immediate flush when is_flush is true
    // case 2: Flush when tx_tail exceeds tt_MAX_BUFFER_LENGTH
    if (is_flush) {
        if (!flush_tx(node, flush_len, peers, peer_count)) {
            return false;
        }
    } else {
        ; // Case 3: Don't flush
    }

    return true;
}

static void* decode(struct tt_Node* node, uint8_t* buffer, uint32_t* head, uint32_t tail, uint32_t length) {
    UNUSED(node);
    return tt_decode_buffer(buffer, head, tail, length);
}

static bool decode_string(struct tt_Node* node, uint8_t* buffer, uint32_t* head, uint32_t tail, uint16_t* str_len,
                          char** str, bool reverse) {
    UNUSED(node);

    if (!tt_decode_string(buffer, head, tail, str_len, str, reverse)) {
        TT_LOG_ERROR("Too big string length: %d + %d > %d", *head, *str_len, tail);
        return false;
    }

    return true;
}

// Framing-field reads for a packet whose tt_Header says the sender used the opposite byte order.
// Every field the library itself interprets (submessage length, endpoint ids, seq/ack numbers,
// timestamps) is stored in the sender's native order; the app's own CDR codecs get is_native_
// endian and handle their payload themselves.
static uint16_t rd16(struct tt_Header* header, uint16_t value) {
    return tt_is_reverse_endian(header) ? _tt_bswap_16(value) : value;
}
static uint32_t rd32(struct tt_Header* header, uint32_t value) {
    return tt_is_reverse_endian(header) ? _tt_bswap_32(value) : value;
}
static uint64_t rd64(struct tt_Header* header, uint64_t value) {
    return tt_is_reverse_endian(header) ? _tt_bswap_64(value) : value;
}

static void rebuild_endpoint_index(struct tt_Node* node) {
    for (uint32_t i = 0; i < tt_ENDPOINT_INDEX_SIZE; i++) {
        node->endpoint_index[i] = NULL;
    }
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL) {
            continue;
        }
        uint32_t slot = endpoint->id & (tt_ENDPOINT_INDEX_SIZE - 1);
        while (node->endpoint_index[slot] != NULL) {
            slot = (slot + 1) & (tt_ENDPOINT_INDEX_SIZE - 1);
        }
        node->endpoint_index[slot] = endpoint;
    }
    node->endpoint_index_valid = true;
}

static struct tt_Endpoint* find_endpoint(struct tt_Node* node, uint8_t kind, uint32_t endpoint_id) {
    if (!node->endpoint_index_valid) {
        rebuild_endpoint_index(node);
    }

    // Linear probe from the id's home slot; a NULL slot means "not present" (load is kept <= 0.5,
    // so the probe is short). Distinct endpoints sharing an id (different kind) just land in
    // adjacent slots and the kind check below picks the right one.
    uint32_t slot = endpoint_id & (tt_ENDPOINT_INDEX_SIZE - 1);
    for (uint32_t probe = 0; probe < tt_ENDPOINT_INDEX_SIZE; probe++) {
        struct tt_Endpoint* endpoint = node->endpoint_index[slot];
        if (endpoint == NULL) {
            return NULL;
        }
        if (endpoint->kind == kind && endpoint->id == endpoint_id) {
            return endpoint;
        }
        slot = (slot + 1) & (tt_ENDPOINT_INDEX_SIZE - 1);
    }

    return NULL;
}

// Refreshes node_id's existing slot in peers[] (its address may have changed), or claims the
// first empty one. Fixed-capacity, no malloc - see tt_MAX_PEER_COUNT. Silently drops the peer if
// the table is already full: a full table already means more peers than tt_UNICAST_PEER_THRESHOLD
// exist, i.e. the sender is already broadcasting instead of unicasting, so the dropped peer is
// still reached that way.
static void upsert_peer(struct tt_Peer* peers, uint8_t node_id, uint32_t ip, uint16_t port) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].node_id == node_id) {
            peers[i].ip = ip;
            peers[i].port = port;
            return;
        }
    }

    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].node_id == tt_NODE_ID_INVALID) {
            peers[i].node_id = node_id;
            peers[i].ip = ip;
            peers[i].port = port;
            return;
        }
    }

    TT_LOG_WARNING("Peer table full (%d), dropping newly seen peer node %u", tt_MAX_PEER_COUNT, node_id);
}

static uint8_t count_peers(const struct tt_Peer* peers) {
    uint8_t count = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].node_id != tt_NODE_ID_INVALID) {
            count++;
        }
    }
    return count;
}

static void forget_peer(struct tt_Peer* peers, uint8_t node_id) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].node_id == node_id) {
            peers[i].node_id = tt_NODE_ID_INVALID;
        }
    }
}

// Drops every peer-table entry pointing at `node_id`, across every Publisher and Client on this
// node. Called when a fresh UPDATE from that source arrives (process_update): its new announce is
// authoritative for what it still hosts, and decode_update_entities() re-adds whatever's still
// listed. Also does the right thing for a node that has left - tt_Node_destroy() broadcasts a
// final entity-less UPDATE, so this forgets it and nothing gets re-added.
static void forget_peers_from_source(struct tt_Node* node, uint8_t node_id) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL) {
            continue;
        }
        if (endpoint->kind == tt_KIND_TOPIC_PUBLISHER) {
            forget_peer(((struct tt_Publisher*)endpoint)->peers, node_id);
        } else if (endpoint->kind == tt_KIND_SERVICE_CLIENT) {
            forget_peer(((struct tt_Client*)endpoint)->peers, node_id);
        }
    }
}

static tt_ret_t add_endpoint_to_node(struct tt_Node* node, struct tt_Endpoint* endpoint) {
    if (node->endpoint_count >= tt_MAX_ENDPOINT_COUNT) {
        uint32_t endpoint_count = node->endpoint_count;
        TT_LOG_ERROR("Too many endpoints: %u", endpoint_count);
        return tt_RET_OUT_OF_BUFFER;
    }

    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* current = node->endpoints[i];
        if ((current != NULL) && (current->kind == endpoint->kind) && (current->id == endpoint->id)) {
            TT_LOG_ERROR("Duplicate endpoint kind=%u id=%u", endpoint->kind, endpoint->id);
            return tt_RET_IILEGAL_ENDPOINT_ID;
        }
    }

    node->endpoints[node->endpoint_count++] = endpoint;
    node->endpoint_index_valid = false;

    return tt_RET_OK;
}

static bool remove_endpoint_from_node(struct tt_Node* node, struct tt_Endpoint* endpoint) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        if (node->endpoints[i] == endpoint) {
            node->endpoint_count--;
            if (i < node->endpoint_count) {
                _tt_memmove((void*)&node->endpoints[i], (const void*)&node->endpoints[i + 1],
                            sizeof(struct tt_Endpoint*) * (node->endpoint_count - i));
            }
            node->endpoints[node->endpoint_count] = NULL;
            node->endpoint_index_valid = false;
            return true;
        }
    }

    return false;
}

static const char* endpoint_type_name(struct tt_Endpoint* endpoint) {
    switch (endpoint->kind) {
    case tt_KIND_TOPIC_PUBLISHER:
        return ((struct tt_Publisher*)endpoint)->topic->name;
    case tt_KIND_TOPIC_SUBSCRIBER:
        return ((struct tt_Subscriber*)endpoint)->topic->name;
    case tt_KIND_SERVICE_CLIENT:
        return ((struct tt_Client*)endpoint)->service->name;
    case tt_KIND_SERVICE_SERVER:
        return ((struct tt_Server*)endpoint)->service->name;
    default:
        return NULL;
    }
}

// scheduler[] is a binary min-heap keyed on TCB.time (heap[0] = earliest), sized by
// scheduler_tail. schedule/pop/unschedule are all O(log N) sift operations with no array
// memmove. Equal-time entries no longer keep strict FIFO order (heaps don't) - nothing in this
// codebase's scheduling depends on that.
static void sched_sift_up(struct tt_Node* node, int32_t index) {
    struct tt_TCB moving = node->scheduler[index];
    while (index > 0) {
        int32_t parent = (index - 1) / 2;
        if (node->scheduler[parent].time <= moving.time) {
            break;
        }
        node->scheduler[index] = node->scheduler[parent];
        index = parent;
    }
    node->scheduler[index] = moving;
}

static void sched_sift_down(struct tt_Node* node, int32_t index) {
    struct tt_TCB moving = node->scheduler[index];
    int32_t count = node->scheduler_tail;
    while (true) {
        int32_t left = (2 * index) + 1;
        int32_t right = left + 1;
        int32_t smallest = index;
        uint64_t best = moving.time;
        if (left < count && node->scheduler[left].time < best) {
            smallest = left;
            best = node->scheduler[left].time;
        }
        if (right < count && node->scheduler[right].time < best) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        node->scheduler[index] = node->scheduler[smallest];
        index = smallest;
    }
    node->scheduler[index] = moving;
}

bool tt_Node_schedule(struct tt_Node* node, uint64_t time,
                      void (*function)(struct tt_Node* node, uint64_t time, void* param), void* param) {
    if (node->scheduler_tail + 1 >= tt_MAX_SCHEDULER_LENGTH) {
        return false;
    }

    int32_t index = node->scheduler_tail;
    node->scheduler[index].time = time;
    node->scheduler[index].function = function;
    node->scheduler[index].param = param;
    node->scheduler_tail++;
    sched_sift_up(node, index);

    return true;
}

bool tt_Node_unschedule(struct tt_Node* node, void (*function)(struct tt_Node* node, uint64_t time, void* param),
                        void* param) {
    bool removed = false;

    for (int32_t i = 0; i < node->scheduler_tail; i++) {
        if (node->scheduler[i].function == function && node->scheduler[i].param == param) {
            node->scheduler_tail--;
            if (i < node->scheduler_tail) {
                node->scheduler[i] = node->scheduler[node->scheduler_tail];
                // The moved-in entry can be out of order in either direction; one of these is a
                // no-op. Re-check this same index next iteration - it holds a different TCB now.
                sched_sift_up(node, i);
                sched_sift_down(node, i);
            }
            removed = true;
            i--;
        }
    }

    return removed;
}

static struct tt_TCB* peek_scheduler(struct tt_Node* node) {
    if (node->scheduler_tail > 0) {
        return &node->scheduler[0];
    }

    return NULL;
}

static void pop_scheduler(struct tt_Node* node) {
    node->scheduler_tail--;
    if (node->scheduler_tail > 0) {
        node->scheduler[0] = node->scheduler[node->scheduler_tail];
        sched_sift_down(node, 0);
    }
}

static void node_update(struct tt_Node* node, uint64_t time, void* param);
static void node_flush(struct tt_Node* node, uint64_t time, void* param);
static void server_cache_clean(struct tt_Node* node, uint64_t time, void* param);
static void clear_server_cache_slot(struct tt_Server* server, int slot);

static void reset_node_state(struct tt_Node* node) {
    node->id = tt_NODE_ID_INVALID;
    node->endpoint_count = 0;
    node->endpoint_index_valid = false;

    for (int i = 0; i < tt_MAX_ENDPOINT_COUNT; i++) {
        node->endpoints[i] = NULL;
    }

    node->last_modified = 0;

    for (int i = 0; i < tt_MAX_ENDPOINT_COUNT; i++) {
        node->update_last_modified[i] = 0;
        node->update_seen[i] = false;
    }

    memset(node->tx_buffer, 0, (long)tt_MAX_BUFFER_LENGTH * 2);
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node->tx_has_pending_update = false;

    memset(node->rx_buffer, 0, (long)tt_MAX_BUFFER_LENGTH * 2);
    node->rx_tail = 0;
    node->rx_size = tt_MAX_BUFFER_LENGTH * 2;

    memset(node->scheduler, 0, sizeof(struct tt_TCB) * tt_MAX_SCHEDULER_LENGTH);
    node->scheduler_tail = 0;
}

// Arms node_update()/node_flush()'s first run, aligned to the next tt_NODE_CYCLE boundary.
static tt_ret_t schedule_periodic_tasks(struct tt_Node* node) {
    uint64_t basetime = tt_get_ns();
    uint64_t rem = basetime % tt_NODE_CYCLE;
    basetime = basetime - rem + tt_NODE_CYCLE;

    if (!tt_Node_schedule(node, basetime, node_update, NULL)) {
        TT_LOG_ERROR("Cannot schedule node_update");
        tt_close(node);
        return tt_RET_OUT_OF_SCHEDULE;
    }

    if (!tt_Node_schedule(node, basetime + tt_NODE_TX_INTERVAL, node_flush, NULL)) {
        TT_LOG_ERROR("Cannot schedule node_flush");
        tt_close(node);
        return tt_RET_OUT_OF_SCHEDULE;
    }

    return tt_RET_OK;
}

tt_ret_t tt_Node_create(struct tt_Node* node) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    reset_node_state(node);

    // _tt_CONFIG.node_id (see its own comment) skips auto-detection when set explicitly.
    node->id = (uint8_t)(_tt_CONFIG.node_id != tt_NODE_ID_INVALID ? _tt_CONFIG.node_id : tt_get_node_id());
    if (node->id == tt_NODE_ID_INVALID || node->id == tt_NODE_ID_BROADCAST) {
        TT_LOG_ERROR("Invalid node id: %u", node->id);
        return tt_RET_IILEGAL_NODE_ID;
    }

    if (tt_bind(node) != tt_RET_OK) {
        TT_LOG_ERROR("Cannot bind");
        return tt_RET_IO_ERROR;
    }

    TT_LOG_INFO("Node open at %d", _tt_CONFIG.port);

    return schedule_periodic_tasks(node);
}

// A message struct sized 0 or larger than one datagram is a misconfiguration: on the receive
// path the request/response/data is decoded into a stack buffer of exactly that size (see
// process_data(), build_call_response(), process_callresponse()), and nothing on the wire can
// exceed tt_MAX_BUFFER_LENGTH anyway - the protocol doesn't fragment. Catch it here, at init,
// instead of overflowing a task stack on the first message received.
static bool valid_msg_size(uint32_t size) {
    return size > 0 && size <= tt_MAX_BUFFER_LENGTH;
}

tt_ret_t tt_Node_create_client(struct tt_Node* node, struct tt_Client* client, struct tt_Service* service,
                               const char* endpoint_name, tt_CLIENT_CALLBACK callback) {
    if (node == NULL || client == NULL || service == NULL || endpoint_name == NULL || callback == NULL ||
        service->name == NULL || !valid_msg_size(service->request_size) || !valid_msg_size(service->response_size) ||
        service->request_encode_size == NULL || service->request_encode == NULL || service->response_decode == NULL ||
        service->response_free == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)client;
    endpoint->kind = tt_KIND_SERVICE_CLIENT;
    endpoint->id = tt_hash_id(service->name, endpoint_name);
    endpoint->name = endpoint_name;
    client->node = node;
    client->service = service;
    client->callback = callback;
    client->seq_no = 0;
    client->cache = NULL;
    client->cache_time = 0;
    client->latency = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        client->peers[i].node_id = tt_NODE_ID_INVALID;
    }

    tt_ret_t result = add_endpoint_to_node(node, endpoint);
    if (result != tt_RET_OK) {
        return result;
    }

    node->last_modified = tt_get_ns();

    return tt_RET_OK;
}

tt_ret_t tt_Node_create_server(struct tt_Node* node, struct tt_Server* server, struct tt_Service* service,
                               const char* endpoint_name, tt_SERVER_CALLBACK callback) {
    if (node == NULL || server == NULL || service == NULL || endpoint_name == NULL || callback == NULL ||
        service->name == NULL || !valid_msg_size(service->request_size) || !valid_msg_size(service->response_size) ||
        service->request_decode == NULL || service->request_free == NULL || service->response_encode_size == NULL ||
        service->response_encode == NULL || service->response_free == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)server;
    endpoint->kind = tt_KIND_SERVICE_SERVER;
    endpoint->id = tt_hash_id(service->name, endpoint_name);
    endpoint->name = endpoint_name;
    server->node = node;
    server->service = service;
    server->callback = callback;

    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        server->cache[i] = NULL;
        server->clean_scheduled[i] = false;
    }

    tt_ret_t result = add_endpoint_to_node(node, endpoint);
    if (result != tt_RET_OK) {
        return result;
    }

    node->last_modified = tt_get_ns();

    return tt_RET_OK;
}

tt_ret_t tt_Node_create_publisher(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Topic* topic,
                                  const char* endpoint_name) {
    if (node == NULL || pub == NULL || topic == NULL || endpoint_name == NULL || topic->name == NULL ||
        !valid_msg_size(topic->data_size) || topic->data_encode_size == NULL || topic->data_encode == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    endpoint->kind = tt_KIND_TOPIC_PUBLISHER;
    endpoint->id = tt_hash_id(topic->name, endpoint_name);
    endpoint->name = endpoint_name;
    pub->node = node;
    pub->topic = topic;
    pub->seq_no = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].node_id = tt_NODE_ID_INVALID;
    }

    tt_ret_t result = add_endpoint_to_node(node, endpoint);
    if (result != tt_RET_OK) {
        return result;
    }

    node->last_modified = tt_get_ns();

    return tt_RET_OK;
}

tt_ret_t tt_Node_create_subscriber(struct tt_Node* node, struct tt_Subscriber* sub, struct tt_Topic* topic,
                                   const char* endpoint_name, tt_SUBSCRIBER_CALLBACK callback) {
    if (node == NULL || sub == NULL || topic == NULL || endpoint_name == NULL || callback == NULL ||
        topic->name == NULL || !valid_msg_size(topic->data_size) || topic->data_decode == NULL ||
        topic->data_free == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)sub;
    endpoint->kind = tt_KIND_TOPIC_SUBSCRIBER;
    endpoint->id = tt_hash_id(topic->name, endpoint_name);
    endpoint->name = endpoint_name;
    sub->node = node;
    sub->topic = topic;
    sub->callback = callback;

    tt_ret_t result = add_endpoint_to_node(node, endpoint);
    if (result != tt_RET_OK) {
        return result;
    }

    node->last_modified = tt_get_ns();

    return tt_RET_OK;
}

// Re-sends the still-outstanding call request verbatim. Failing to encode/flush isn't fatal here
// - the retry timer armed by the caller will just try again.
static void resend_call_request(struct tt_Node* node, struct tt_Client* client,
                                struct tt_SubmessageHeader* submessage_header) {
    uint32_t old_tx_tail = node->tx_tail;
    void* buf = encode(node, submessage_header->length);
    if (buf == NULL) {
        TT_LOG_WARNING("Lack of tx buffer, will retry call_retry later");
        return;
    }

    _tt_memcpy(buf, submessage_header, submessage_header->length);
    // Flush immediately, same reasoning as the initial call in tt_Client_call(). Same peer
    // decision too, recomputed here in case discovery has learned more (or fewer) Servers since
    // the initial call went out - see tt_Client_call()'s own comment on the threshold and the
    // shared-tx_buffer guard (old_tx_tail == sizeof(tt_Header)).
    uint8_t peer_count = count_peers(client->peers);
    bool unicast =
        peer_count >= 1 && peer_count <= tt_UNICAST_PEER_THRESHOLD && old_tx_tail == sizeof(struct tt_Header);
    if (!end_encode(node, buf, true, unicast ? client->peers : NULL, unicast ? peer_count : 0)) {
        TT_LOG_WARNING("Cannot flush call request retry, will retry later");
        rollback(node, old_tx_tail);
    }
}

static uint32_t compute_retry_interval(struct tt_Client* client) {
    if (client->service->call_retry_interval != 0) {
        return client->service->call_retry_interval;
    }

    uint32_t retry_interval = client->latency == 0 ? tt_CALL_RETRY_INTERVAL : client->latency;
    return retry_interval + (retry_interval >> 1); // latency * 1.5
}

static void call_retry(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(time);

    struct tt_Client* client = param;

    struct tt_SubmessageHeader* submessage_header = client->cache;
    if (submessage_header == NULL) {
        // Already respond.
        return;
    }

    struct tt_CallRequestHeader* callrequest_header =
        (struct tt_CallRequestHeader*)((void*)submessage_header + sizeof(struct tt_SubmessageHeader));

    if (++callrequest_header->retry > client->service->call_retry_count) {
        client->callback(client, tt_CALL_TIMEOUT, NULL); // every retry went unanswered

        client->cache = NULL;
        return;
    }

    resend_call_request(node, client, submessage_header);

    if (!tt_Node_schedule(node, tt_get_ns() + compute_retry_interval(client), call_retry, client)) {
        TT_LOG_ERROR("Cannot schedule call_retry");
        client->callback(client, tt_CALL_TIMEOUT, NULL); // can't arm another retry - treat as no answer

        client->cache = NULL;
    }
}

tt_ret_t tt_Client_call(struct tt_Client* client, struct tt_Request* request) {
    if (client == NULL || request == NULL || client->node == NULL || client->service == NULL ||
        client->service->request_encode_size == NULL || client->service->request_encode == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    if (client->cache != NULL) {
        return tt_RET_ILLEGAL_STATUS; // waiting response
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)client;
    struct tt_Node* node = client->node;
    uint32_t old_tx_tail = node->tx_tail;

    // Header and SubmessageHeader
    struct tt_SubmessageHeader* submessage_header =
        start_encode(node, tt_SUBMESSAGE_TYPE_CALLREQUEST, tt_SUBMESSAGE_ID_ALL);
    if (submessage_header == NULL) {
        rollback(node, old_tx_tail);
        return tt_RET_OUT_OF_BUFFER;
    }

    // CallRequestHeader
    struct tt_CallRequestHeader* callrequest_header = encode(node, sizeof(struct tt_CallRequestHeader));
    if (callrequest_header == NULL) {
        rollback(node, old_tx_tail);
        return tt_RET_OUT_OF_BUFFER;
    }

    callrequest_header->endpoint_id = endpoint->id;
    callrequest_header->seq_no = client->seq_no;
    callrequest_header->retry = 0;
    callrequest_header->reserved = 0;

    // CallRequestBody
    int32_t cdr_len = client->service->request_encode_size(request);
    if (cdr_len < 0 || cdr_len > tt_MAX_BUFFER_LENGTH) {
        TT_LOG_ERROR("request_encode_size returned %d (out of range)", cdr_len);
        rollback(node, old_tx_tail);
        return tt_RET_PROTOCOL_ERROR;
    }
    void* cdr = encode(node, (uint32_t)cdr_len);
    if (cdr == NULL) {
        rollback(node, old_tx_tail);
        return tt_RET_OUT_OF_BUFFER;
    }

    int32_t encoded_len = client->service->request_encode(request, cdr, (uint32_t)cdr_len);
    if (encoded_len < 0) {
        rollback(node, old_tx_tail);
        return tt_RET_PROTOCOL_ERROR;
    }

    // Make cache: copy into the client's own fixed backing buffer instead of malloc'ing one.
    // tx_buffer is sized tt_MAX_BUFFER_LENGTH * 2 to let one submessage overshoot the flush
    // limit before being deferred, so cache_buf is sized to match that same worst case.
    size_t length = ROUNDUP((uintptr_t)node->tx_buffer + node->tx_tail - (uintptr_t)submessage_header);
    struct tt_SubmessageHeader* cache = (struct tt_SubmessageHeader*)client->cache_buf;
    _tt_memcpy(cache, submessage_header, length);
    cache->length = length;

    // Flush tx immediately: an RPC caller is synchronously waiting on the reply, so this can't
    // sit batched until node_flush()'s next 1ms tick like a pub/sub publish reasonably can.
    // Unicast to each known Server when there are few enough of them (tt_UNICAST_PEER_THRESHOLD)
    // and nothing else was already sitting unflushed in tx_buffer ahead of this CallRequest (same
    // shared-tx_buffer guard as tt_Publisher_publish()/process_callrequest()); otherwise - 0
    // known Servers (discovery hasn't matched one yet) or more than the threshold - broadcast, as
    // this always has until now.
    uint8_t peer_count = count_peers(client->peers);
    bool unicast =
        peer_count >= 1 && peer_count <= tt_UNICAST_PEER_THRESHOLD && old_tx_tail == sizeof(struct tt_Header);
    if (!end_encode(node, submessage_header, true, unicast ? client->peers : NULL, unicast ? peer_count : 0)) {
        rollback(node, old_tx_tail);
        return tt_RET_IO_ERROR;
    }

    client->cache = cache;
    client->cache_time = tt_get_ns();
    client->seq_no++;

    uint32_t retry_interval;
    if (client->service->call_retry_interval == 0) {
        retry_interval = client->latency == 0 ? tt_CALL_RETRY_INTERVAL : client->latency;
        retry_interval = retry_interval + (retry_interval >> 1); // latency * 1.5
    } else {
        retry_interval = client->service->call_retry_interval;
    }

    if (!tt_Node_schedule(node, tt_get_ns() + retry_interval, call_retry, client)) {
        TT_LOG_ERROR("Cannot schedule call_retry");
        client->cache = NULL;
        return tt_RET_OUT_OF_SCHEDULE;
    }

    return tt_RET_OK;
}

tt_ret_t tt_Client_destroy(struct tt_Client* client) {
    if (client == NULL || client->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)client;

    if (!remove_endpoint_from_node(client->node, endpoint)) {
        return tt_RET_IILEGAL_ENDPOINT_ID;
    }

    if (client->cache != NULL) {
        // Cancel the pending call_retry before clearing the cache it references, otherwise
        // that retry later fires on this (possibly freed/reused) client.
        tt_Node_unschedule(client->node, call_retry, client);
        client->cache = NULL;
    }

    client->node->last_modified = tt_get_ns();

    return tt_RET_OK;
}

tt_ret_t tt_Server_destroy(struct tt_Server* server) {
    if (server == NULL || server->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)server;

    if (!remove_endpoint_from_node(server->node, endpoint)) {
        return tt_RET_IILEGAL_ENDPOINT_ID;
    }

    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (server->cache[i] != NULL) {
            // Cancel each pending server_cache_clean before clearing the slot it references,
            // otherwise that timer later fires on this (possibly freed/reused) server.
            clear_server_cache_slot(server, i);
        }
    }

    server->node->last_modified = tt_get_ns();

    return tt_RET_OK;
}

// Zero-copy standalone-packet publish: framing (Header + SubmessageHeader + DataHeader) built in
// a stack buffer, the CDR sent straight from the publisher's own memory via one sendmsg() - no
// staging copy into tx_buffer. Only reachable when tx_buffer is empty (nothing to coalesce with),
// so it makes the same broadcast-vs-unicast destination choice node_flush() would for a batched
// flush. body_len must be 4-aligned (the caller checks) so the single submessage needs no pad.
static tt_ret_t publish_zerocopy(struct tt_Publisher* pub, const uint8_t* body, uint32_t body_len) {
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    struct tt_Node* node = pub->node;

    uint8_t framing[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader)];
    struct tt_Header* header = (struct tt_Header*)framing;
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = node->id;

    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)(framing + sizeof(struct tt_Header));
    submessage_header->type = tt_SUBMESSAGE_TYPE_DATA;
    submessage_header->receiver = tt_SUBMESSAGE_ID_ALL;
    submessage_header->length =
        (uint16_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader) + body_len);

    struct tt_DataHeader* data_header =
        (struct tt_DataHeader*)(framing + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    data_header->endpoint_id = endpoint->id;
    data_header->seq_no = pub->seq_no + 1;
    data_header->timestamp = tt_get_ns();

    uint8_t peer_count = count_peers(pub->peers);
    if (peer_count >= 1 && peer_count <= tt_UNICAST_PEER_THRESHOLD) {
        for (uint8_t i = 0; i < peer_count; i++) {
            if (tt_send_iov(node, framing, sizeof(framing), body, body_len, pub->peers[i].ip, pub->peers[i].port) < 0) {
                return tt_RET_IO_ERROR;
            }
        }
    } else if (tt_send_iov(node, framing, sizeof(framing), body, body_len, 0, 0) < 0) {
        return tt_RET_IO_ERROR;
    }

    pub->seq_no++;
    return tt_RET_OK;
}

tt_ret_t tt_Publisher_publish(struct tt_Publisher* pub, struct tt_Data* data) {
    if (pub == NULL || data == NULL || pub->node == NULL || pub->topic == NULL ||
        pub->topic->data_encode_size == NULL || pub->topic->data_encode == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    struct tt_Node* node = pub->node;
    uint32_t old_tx_tail = node->tx_tail;

    // Zero-copy path when the topic offers it, tx_buffer is empty (nothing batched to coalesce
    // with), and the message is big enough that a second one wouldn't fit in the same packet
    // anyway - i.e. batching has nothing to gain here. Small messages fall through to the staging
    // path so node_flush() can still pack several per packet.
    if (pub->topic->data_encode_inplace != NULL && old_tx_tail == sizeof(struct tt_Header)) {
        const uint8_t* body = NULL;
        int32_t body_len = pub->topic->data_encode_inplace(data, &body);
        uint32_t standalone_len = sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) +
                                  sizeof(struct tt_DataHeader) + (body_len >= 0 ? (uint32_t)body_len : 0);
        bool fills_packet =
            standalone_len + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader) > tt_MAX_BUFFER_LENGTH;
        if (body_len >= 0 && (body_len % 4) == 0 && fills_packet) {
            return publish_zerocopy(pub, body, (uint32_t)body_len);
        }
        // declined, unaligned, or small enough to want batching - use the staging copy path
    }

    // Header and SubmessageHeader
    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL);
    if (submessage_header == NULL) {
        rollback(node, old_tx_tail);
        return tt_RET_OUT_OF_BUFFER;
    }

    // CallRequestHeader
    struct tt_DataHeader* data_header = encode(node, sizeof(struct tt_DataHeader));
    if (data_header == NULL) {
        rollback(node, old_tx_tail);
        return tt_RET_OUT_OF_BUFFER;
    }

    data_header->endpoint_id = endpoint->id;
    data_header->seq_no = pub->seq_no + 1;
    data_header->timestamp = tt_get_ns();

    // DataBody
    int32_t cdr_len = pub->topic->data_encode_size(data);
    if (cdr_len < 0 || cdr_len > tt_MAX_BUFFER_LENGTH) {
        TT_LOG_ERROR("data_encode_size returned %d (out of range)", cdr_len);
        rollback(node, old_tx_tail);
        return tt_RET_PROTOCOL_ERROR;
    }
    void* cdr = encode(node, (uint32_t)cdr_len);
    if (cdr == NULL) {
        rollback(node, old_tx_tail);
        return tt_RET_OUT_OF_BUFFER;
    }

    int32_t encoded_len = pub->topic->data_encode(data, cdr, cdr_len);
    if (encoded_len < 0) {
        rollback(node, old_tx_tail);
        return tt_RET_PROTOCOL_ERROR;
    }

    // Always batches (is_flush=false) - node_flush()'s own 1ms tick decides broadcast vs. unicast
    // to pub->peers for the whole accumulated buffer at once (see its own comment on why that
    // decision has to live there and not here: unicasting a single publish() immediately, tried
    // and measured, collapsed a bulk/high-rate stream's real throughput by forgoing this
    // batching entirely - node_flush() gets the traffic-reduction benefit without that cost).
    if (!end_encode(node, submessage_header, false, NULL, 0)) {
        rollback(node, old_tx_tail);
        return tt_RET_IO_ERROR;
    }

    pub->seq_no++;

    return tt_RET_OK;
}

tt_ret_t tt_Publisher_destroy(struct tt_Publisher* pub) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;

    if (remove_endpoint_from_node(pub->node, endpoint)) {
        pub->node->last_modified = tt_get_ns();
        return tt_RET_OK;
    }

    return tt_RET_IILEGAL_ENDPOINT_ID;
}

tt_ret_t tt_Subscriber_destroy(struct tt_Subscriber* sub) {
    if (sub == NULL || sub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)sub;

    if (remove_endpoint_from_node(sub->node, endpoint)) {
        sub->node->last_modified = tt_get_ns();
        return tt_RET_OK;
    }

    return tt_RET_IILEGAL_ENDPOINT_ID;
}

// Encodes one UpdateEntity per non-NULL endpoint, up to UINT8_MAX of them. Returns the number
// encoded, or -1 on an encode failure (the caller must roll back the whole submessage in that
// case). encode()/encode_string() already log their own reason when they fail, so this doesn't
// log again on top of that - except "illegal endpoint kind", which is this function's own check.
static int encode_update_entities(struct tt_Node* node, struct tt_Endpoint* const* endpoints, uint32_t endpoint_count) {
    uint8_t entity_count = 0;
    for (uint32_t i = 0; i < endpoint_count; i++) {
        struct tt_Endpoint* endpoint = endpoints[i];
        if (endpoint == NULL) {
            continue;
        }

        const char* type = endpoint_type_name(endpoint);
        if (type == NULL) {
            TT_LOG_ERROR("Illegal endpoint kind: %d", endpoint->kind);
            return -1;
        }

        struct tt_UpdateEntity* update_entity = encode(node, sizeof(struct tt_UpdateEntity));
        if (update_entity == NULL) {
            return -1;
        }

        update_entity->endpoint_id = endpoint->id;
        update_entity->kind = endpoint->kind;

        if (!encode_string(node, type) || !encode_string(node, endpoint->name)) {
            return -1;
        }

        if (++entity_count == UINT8_MAX) {
            TT_LOG_WARNING("Maximum update entity: endpoint index: %u, endpoint count: %u", i, endpoint_count);
            break;
        }
    }

    return entity_count;
}

// Builds this node's current UPDATE announce (its own endpoint list) and sends it either way
// node_update()/process_update() need it sent: peer_count == 0 broadcasts it, batched
// (is_flush=false - the periodic case, no synchronous waiter, node_flush()'s own tick is fine);
// peer_count >= 1 unicasts it to that one peer, flushed immediately (the reactive first-contact
// reply case in process_update() - the whole point is the other side learning us as fast as
// possible, not waiting for the next tick or our own next periodic broadcast).
static bool build_and_send_update(struct tt_Node* node, const struct tt_Peer* peers, uint8_t peer_count) {
    uint32_t old_tx_tail = node->tx_tail;

    // start_encode()/encode() below already log their own reason when they fail (e.g. "Lack of
    // tx buffer"), so none of these failure branches log again on top of that.

    // Header and SubmessageHeader
    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_UPDATE, tt_SUBMESSAGE_ID_ALL);
    if (submessage_header == NULL) {
        return false;
    }

    struct tt_UpdateHeader* update_header = encode(node, sizeof(struct tt_UpdateHeader));
    if (update_header == NULL) {
        rollback(node, old_tx_tail);
        return false;
    }

    update_header->last_modified = node->last_modified;
    update_header->entity_count = 0;

    struct tt_Endpoint* endpoints[tt_MAX_ENDPOINT_COUNT];
    uint32_t endpoint_count = node->endpoint_count;
    for (uint32_t i = 0; i < endpoint_count; i++) {
        endpoints[i] = node->endpoints[i];
    }

    int entity_count = encode_update_entities(node, endpoints, endpoint_count);
    if (entity_count < 0) {
        rollback(node, old_tx_tail);
        return false;
    }
    update_header->entity_count = (uint8_t)entity_count;

    bool is_flush = peer_count > 0;
    if (!end_encode(node, submessage_header, is_flush, peers, peer_count)) {
        rollback(node, old_tx_tail);
        return false;
    }

    if (!is_flush) {
        // This UPDATE is now sitting batched in tx_buffer (or, rarely, was already flushed on
        // its own by end_encode()'s own overflow handling above) - either way it's
        // broadcast-only content that must not get swept into a unicast flush; node_flush()
        // clears this once it's actually sent, see flush_tx().
        node->tx_has_pending_update = true;
    }

    return true;
}

static void node_update(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(param);

    build_and_send_update(node, NULL, 0);

    if (!tt_Node_schedule(node, time + tt_NODE_UPDATE_INTERVAL, node_update, NULL)) {
        TT_LOG_ERROR("Cannot schedule node_update");
    }
}

// This periodic tick only ever flushes batched pub/sub content - a DATA submessage from
// tt_Publisher_publish() and/or an UPDATE from node_update() - never a CallResponse (that always
// flushes immediately from process_callrequest() itself instead). Broadcast is always correct
// for that content; unicasting it to a short list of known peers is only correct when (a) no
// UPDATE is currently batched in there (it must reach the whole segment, not just a couple of
// peers - see tx_has_pending_update) and (b) there's exactly one Publisher on this node to
// attribute the batched DATA to (tx_buffer is shared across every endpoint on a node - mixing
// two Publishers' data in one unicast flush could send one's data to the other's peers). Both
// conditions hold for every one of this codebase's own examples (one Publisher per node); a node
// with 0 or 2+ Publishers, or one with an UPDATE still pending, simply keeps broadcasting exactly
// as before this feature existed.
static void node_flush(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(param);

    const struct tt_Peer* peers = NULL;
    uint8_t peer_count = 0;

    if (!node->tx_has_pending_update) {
        struct tt_Publisher* sole_publisher = NULL;
        uint32_t publisher_count = 0;
        for (uint32_t i = 0; i < node->endpoint_count; i++) {
            struct tt_Endpoint* endpoint = node->endpoints[i];
            if (endpoint != NULL && endpoint->kind == tt_KIND_TOPIC_PUBLISHER) {
                sole_publisher = (struct tt_Publisher*)endpoint;
                publisher_count++;
            }
        }

        if (publisher_count == 1) {
            uint8_t count = count_peers(sole_publisher->peers);
            if (count >= 1 && count <= tt_UNICAST_PEER_THRESHOLD) {
                peers = sole_publisher->peers;
                peer_count = count;
            }
        }
    }

    // flush_tx() already logs its own reason on failure, so nothing to add here.
    flush_tx(node, node->tx_tail, peers, peer_count);

    if (!tt_Node_schedule(node, time + tt_NODE_TX_INTERVAL, node_flush, NULL)) {
        TT_LOG_ERROR("Cannot schedule node_flush");
    }
}

// Walks the entity_count UpdateEntity records following an UpdateHeader, advancing *head past
// them. Along the way, matches each announced entity against this node's own endpoints: a
// remote TOPIC_SUBSCRIBER (resp. SERVICE_SERVER) whose endpoint_id matches one of our own
// Publishers (resp. Clients) means that Publisher/Client just learned a new (or refreshed) peer
// it can unicast to - see upsert_peer(), tt_UNICAST_PEER_THRESHOLD. Returns false if a
// type/name string fails to decode.
static bool decode_update_entities(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t* head,
                                   uint32_t tail, int entity_count, uint32_t sender_ip, uint16_t sender_port) {
    bool reverse = tt_is_reverse_endian(header);
    for (int i = 0; i < entity_count && *head + sizeof(struct tt_UpdateEntity) + (2 * sizeof(uint16_t)) < tail; i++) {
        struct tt_UpdateEntity* update_entity = decode(node, buffer, head, tail, sizeof(struct tt_UpdateEntity));
        uint32_t entity_id = rd32(header, update_entity->endpoint_id);

        TT_LOG_DEBUG("UpdateEntity");
        TT_LOG_DEBUG("  endpoint_id: %08x", entity_id);
        TT_LOG_DEBUG("  kind: %d", update_entity->kind);

        if (update_entity->kind == tt_KIND_TOPIC_SUBSCRIBER) {
            struct tt_Endpoint* local = find_endpoint(node, tt_KIND_TOPIC_PUBLISHER, entity_id);
            if (local != NULL) {
                upsert_peer(((struct tt_Publisher*)local)->peers, header->source, sender_ip, sender_port);
            }
        } else if (update_entity->kind == tt_KIND_SERVICE_SERVER) {
            struct tt_Endpoint* local = find_endpoint(node, tt_KIND_SERVICE_CLIENT, entity_id);
            if (local != NULL) {
                upsert_peer(((struct tt_Client*)local)->peers, header->source, sender_ip, sender_port);
            }
        }

        uint16_t type_len = 0;
        char* type = NULL;
        if (!decode_string(node, buffer, head, tail, &type_len, &type, reverse)) {
            TT_LOG_ERROR("Cannot decode type");
            return false;
        }
        TT_LOG_DEBUG("  type: (%d)\"%s\"", type_len, type);

        uint16_t name_len = 0;
        char* name = NULL;
        if (!decode_string(node, buffer, head, tail, &name_len, &name, reverse)) {
            TT_LOG_ERROR("Cannot decode name");
            return false;
        }
        TT_LOG_DEBUG("  name: (%d)\"%s\"", name_len, name);
    }

    return true;
}

// Unicasts our own current UPDATE announce straight back to a peer we've just heard from for the
// first time - see process_update()'s own comment on why. Terminates rather than looping forever
// because it only ever fires on that first contact: by the time this reply reaches the peer and
// it processes it, node->update_seen[our own source] on ITS side is already true - either from
// whatever announce got us onto its radar in the first place, or, in the simultaneous-startup
// case, from this very reply - so replying to a reply never meets this same trigger condition
// again on either side. Skipped (not a correctness issue, just a missed optimization
// this one time - the periodic broadcast still reaches them eventually) whenever tx_buffer
// already has something else pending: redirecting that to a single peer here could be wrong for
// whatever else it's for (same shared-tx_buffer reasoning as process_callrequest()'s own unicast).
static void reply_with_own_announce(struct tt_Node* node, uint8_t sender_node_id, uint32_t sender_ip,
                                    uint16_t sender_port) {
    if (node->tx_tail != sizeof(struct tt_Header)) {
        return;
    }

    struct tt_Peer reply_to = {sender_node_id, sender_ip, sender_port};
    build_and_send_update(node, &reply_to, 1);
}

static bool process_update(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                           uint32_t tail, uint32_t sender_ip, uint16_t sender_port) {
    struct tt_UpdateHeader* update_header = decode(node, buffer, &head, tail, sizeof(struct tt_UpdateHeader));
    if (update_header == NULL) {
        TT_LOG_ERROR("Illegal UpdateHeader");
        return false;
    }

    uint8_t source = header->source;
    uint64_t last_modified = rd64(header, update_header->last_modified);

    TT_LOG_DEBUG("Update");
    TT_LOG_DEBUG("  last_modified: %lu", last_modified);
    TT_LOG_DEBUG("  entity_count: %u", update_header->entity_count);

    if (node->update_seen[source] && node->update_last_modified[source] == last_modified) {
        return true; // nothing changed since the announce we last acted on
    }

    // First time we've ever heard from this node, as opposed to it changing its endpoints since -
    // captured before update_seen[] is set below, because that's the state
    // reply_with_own_announce() needs to not reply forever (see its own comment).
    bool is_first_contact_from_sender = !node->update_seen[source];

    // This announce supersedes anything we knew about what this source hosts (it may have dropped
    // an endpoint, or left entirely - see tt_Node_destroy()'s farewell UPDATE). Forget its old
    // peer-table entries; decode_update_entities() below re-adds whatever it still lists.
    forget_peers_from_source(node, source);

    if (!decode_update_entities(node, header, buffer, &head, tail, update_header->entity_count, sender_ip,
                                sender_port)) {
        return false;
    }

    node->update_last_modified[source] = last_modified;
    node->update_seen[source] = true;

    if (is_first_contact_from_sender) {
        reply_with_own_announce(node, source, sender_ip, sender_port);
    }

    return true;
}

static bool process_data(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                         uint32_t tail) {
    struct tt_DataHeader* data_header = decode(node, buffer, &head, tail, sizeof(struct tt_DataHeader));
    if (data_header == NULL) {
        TT_LOG_ERROR("Illegal DataHeader");
        return false;
    }

    uint32_t endpoint_id = rd32(header, data_header->endpoint_id);
    uint32_t seq_no = rd32(header, data_header->seq_no);
    uint64_t timestamp = rd64(header, data_header->timestamp);

    TT_LOG_DEBUG("Data");
    TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
    TT_LOG_DEBUG("  timestamp: %ld", timestamp);
    TT_LOG_DEBUG("  seq_no: %d", seq_no);

    struct tt_Endpoint* endpoint = find_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id);
    if (endpoint == NULL) {
        return true;
    }

    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
    struct tt_Topic* topic = sub->topic;
    bool is_native = tt_is_native_endian(header);

    // Zero-copy path: hand the callback a tt_Data* aliasing rx_buffer directly, skipping the
    // decode-into-scratch copy and the matching data_free. Falls through to the copy path when
    // the topic doesn't offer it or it declines (e.g. byte-swapped wire).
    if (topic->data_decode_inplace != NULL) {
        struct tt_Data* inplace = topic->data_decode_inplace(buffer + head, tail - head, is_native);
        if (inplace != NULL) {
            sub->callback(sub, timestamp, (uint16_t)seq_no, inplace);
            return true;
        }
    }

    uint8_t data[topic->data_size];
    int32_t decoded = topic->data_decode((struct tt_Data*)data, buffer + head, tail - head, is_native);

    if (decoded < 0) {
        TT_LOG_ERROR("Cannot decode data for endpoint_id: %08x, seq_no: %d", endpoint_id, seq_no);
        return false;
    }

    sub->callback(sub, timestamp, (uint16_t)seq_no, (struct tt_Data*)data);
    topic->data_free((struct tt_Data*)data);
    return true;
}

static struct tt_SubmessageHeader* get_server_cache(struct tt_Server* server, uint8_t receiver, uint16_t seq_no) {
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (server->cache[i] != NULL) {
            struct tt_SubmessageHeader* submessage_header = server->cache[i];
            struct tt_CallResponseHeader* callresponse_header =
                (struct tt_CallResponseHeader*)((void*)submessage_header + sizeof(struct tt_SubmessageHeader));

            if ((submessage_header->receiver == receiver) && (callresponse_header->seq_no == seq_no)) {
                return submessage_header;
            }
        }
    }

    return NULL;
}

// Cancels slot i's cleanup timer (if any) and frees it up for reuse. The timer must be
// cancelled before the slot is reused, otherwise it later fires and clears whatever
// unrelated entry ends up occupying the slot by then.
static void clear_server_cache_slot(struct tt_Server* server, int slot) {
    if (server->clean_scheduled[slot]) {
        tt_Node_unschedule(server->node, server_cache_clean, &server->clean_config[slot]);
        server->clean_scheduled[slot] = false;
    }
    server->cache[slot] = NULL;
}

static void server_cache_clean(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(node);
    UNUSED(time);

    struct server_cache_clean_config* clean = param;
    clean->server->cache[clean->slot] = NULL;
    clean->server->clean_scheduled[clean->slot] = false;
}

static bool set_server_cache(struct tt_Server* server, struct tt_SubmessageHeader* submessage_header,
                             uint8_t receiver) {
    size_t length = ROUNDUP((uintptr_t)server->node->tx_buffer + server->node->tx_tail - (uintptr_t)submessage_header);

    int free_slot = -1;
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (server->cache[i] != NULL && server->cache[i]->receiver == receiver) {
            clear_server_cache_slot(server, i);
        }

        if (free_slot < 0 && server->cache[i] == NULL) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        TT_LOG_ERROR("Out of server cache slots");
        return false;
    }

    // Copy into this slot's own fixed buffer instead of malloc'ing one.
    struct tt_SubmessageHeader* cache = (struct tt_SubmessageHeader*)server->cache_buf[free_slot];
    _tt_memcpy(cache, submessage_header, length);
    cache->length = length;

    server->clean_config[free_slot].server = server;
    server->clean_config[free_slot].slot = free_slot;

    // Only publish `cache` into the slot once its cleanup timer is guaranteed to run;
    // otherwise the slot would hold an entry that never gets cleared.
    if (!tt_Node_schedule(server->node, tt_get_ns() + tt_SERVER_CACHE_TIMEOUT, server_cache_clean,
                          &server->clean_config[free_slot])) {
        TT_LOG_ERROR("Cannot schedule server_cache_clean");
        return false;
    }

    server->clean_scheduled[free_slot] = true;
    server->cache[free_slot] = cache;

    return true;
}

// Cache hit: re-encode the previously cached response verbatim (bumping its retry count) instead
// of re-running the service callback - the client is asking again because it hasn't seen the
// first response yet, not because it wants a fresh answer.
static struct tt_SubmessageHeader* resend_cached_response(struct tt_Node* node,
                                                          struct tt_SubmessageHeader* submessage_header) {
    struct tt_CallResponseHeader* callresponse_header = (void*)submessage_header + sizeof(struct tt_SubmessageHeader);
    callresponse_header->retry++;

    void* buf = encode(node, submessage_header->length);
    if (buf == NULL) {
        TT_LOG_ERROR("Cannot retry response");
        return NULL;
    }

    _tt_memcpy(buf, submessage_header, submessage_header->length);
    return buf;
}

// No cached response yet: run the service callback fresh, then encode and cache a new
// CallResponse. `old_tx_tail` is this submessage's start, for rolling back on a failure here.
static struct tt_SubmessageHeader* build_call_response(struct tt_Node* node, struct tt_Header* header,
                                                       struct tt_Server* server, uint16_t request_seq_no,
                                                       uint8_t* buffer, uint32_t head, uint32_t tail,
                                                       uint32_t old_tx_tail) {
    struct tt_Service* service = server->service;

    uint8_t request[service->request_size];
    int32_t decoded =
        service->request_decode((struct tt_Request*)request, buffer + head, tail - head, tt_is_native_endian(header));
    if (decoded < 0) {
        TT_LOG_ERROR("Cannot decode request: %d", decoded);
        return NULL;
    }

    uint8_t response[service->response_size];
    int8_t return_code = server->callback(server, (struct tt_Request*)request, (struct tt_Response*)response);
    service->request_free((struct tt_Request*)request);

    TT_LOG_DEBUG("  return_code: %d", return_code);

    // Header and SubmessageHeader
    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_CALLRESPONSE, header->source);
    if (submessage_header == NULL) {
        return NULL;
    }

    // CallResponseHeader
    struct tt_CallResponseHeader* call_response_header = encode(node, sizeof(struct tt_CallResponseHeader));
    if (call_response_header == NULL) {
        rollback(node, old_tx_tail);
        return NULL;
    }

    call_response_header->endpoint_id = server->endpoint.id;
    call_response_header->seq_no = request_seq_no; // native - encoded below in this node's own order
    call_response_header->retry = 0;
    call_response_header->return_code = return_code;

    // CallRequestBody
    if (return_code == 0) {
        int32_t cdr_len = service->response_encode_size((struct tt_Response*)response);
        if (cdr_len < 0 || cdr_len > tt_MAX_BUFFER_LENGTH) {
            TT_LOG_ERROR("response_encode_size returned %d (out of range)", cdr_len);
            rollback(node, old_tx_tail);
            return NULL;
        }
        void* cdr = encode(node, (uint32_t)cdr_len);
        if (cdr == NULL) {
            rollback(node, old_tx_tail);
            return NULL;
        }

        int32_t encoded_len = service->response_encode((struct tt_Response*)response, cdr, (uint32_t)cdr_len);
        service->response_free((struct tt_Response*)response);

        if (encoded_len < 0) {
            rollback(node, old_tx_tail);
            return NULL;
        }
    }

    // Cache submessage header before flush. set_server_cache() already logs its own reason on
    // failure, so nothing to add here.
    if (!set_server_cache(server, submessage_header, header->source)) {
        rollback(node, old_tx_tail);
        return NULL;
    }

    return submessage_header;
}

static bool process_callrequest(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                                uint32_t tail, uint32_t sender_ip, uint16_t sender_port) {
    struct tt_CallRequestHeader* callrequest_header =
        decode(node, buffer, &head, tail, sizeof(struct tt_CallRequestHeader));
    if (callrequest_header == NULL) {
        TT_LOG_ERROR("Illegal CallRequestHeader");
        return false;
    }

    uint32_t endpoint_id = rd32(header, callrequest_header->endpoint_id);
    uint16_t seq_no = rd16(header, callrequest_header->seq_no);

    TT_LOG_DEBUG("CallRequest");
    TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
    TT_LOG_DEBUG("  seq_no: %d", seq_no);
    TT_LOG_DEBUG("  retry: %d", callrequest_header->retry);

    struct tt_Endpoint* endpoint = find_endpoint(node, tt_KIND_SERVICE_SERVER, endpoint_id);
    if (endpoint == NULL) {
        return true;
    }

    struct tt_Server* server = (struct tt_Server*)endpoint;

    // Check cache
    struct tt_SubmessageHeader* cached = get_server_cache(server, header->source, seq_no);
    uint32_t old_tx_tail = node->tx_tail;

    struct tt_SubmessageHeader* submessage_header =
        cached != NULL ? resend_cached_response(node, cached)
                       : build_call_response(node, header, server, seq_no, buffer, head, tail, old_tx_tail);
    if (submessage_header == NULL) {
        return false;
    }

    // Unicast the response straight back to whoever's request we just decoded, instead of
    // broadcasting an answer the rest of the segment never asked for - the request already told
    // us exactly where to send it (sender_ip/sender_port, straight from tt_receive()), so there's
    // nothing left to discover the way the initial CallRequest still has to (see
    // tt_Client_call()'s own comment on why *that* stays broadcast). Only when old_tx_tail is
    // still at the just-reset baseline, i.e. nothing else was already sitting unflushed in
    // tx_buffer - this node's tx_buffer is shared across every endpoint on it (a publisher and a
    // server could in principle coexist on one node, even though nothing in this codebase's own
    // examples does that), so unicasting a flush that happens to also carry something else's
    // broadcast-destined content would be wrong. Falling back to broadcast in that case costs
    // nothing here (this server's own response still reaches its caller, everyone else just also
    // hears it, exactly like before this optimization existed) but keeps that mixed case correct.
    struct tt_Peer sender_peer = {header->source, sender_ip, sender_port};
    bool unicast_to_sender = old_tx_tail == sizeof(struct tt_Header);

    // Flush immediately: the client on the other end is synchronously waiting on this response
    // (or already retrying because it hasn't seen one yet), so it can't sit batched until
    // node_flush()'s next 1ms tick like a pub/sub publish reasonably can.
    if (!end_encode(node, submessage_header, true, unicast_to_sender ? &sender_peer : NULL,
                    unicast_to_sender ? 1 : 0)) {
        rollback(node, old_tx_tail);
        return false;
    }
    return true;
}

static bool process_callresponse(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                                 uint32_t tail) {
    struct tt_CallResponseHeader* callresponse_header =
        decode(node, buffer, &head, tail, sizeof(struct tt_CallResponseHeader));
    if (callresponse_header == NULL) {
        TT_LOG_ERROR("Illegal CallResponseHeader");
        return false;
    }

    uint32_t endpoint_id = rd32(header, callresponse_header->endpoint_id);
    uint16_t seq_no = rd16(header, callresponse_header->seq_no);

    TT_LOG_DEBUG("CallResponse");
    TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
    TT_LOG_DEBUG("  seq_no: %d", seq_no);
    TT_LOG_DEBUG("  retry: %d", callresponse_header->retry);
    TT_LOG_DEBUG("  return_code: %d", callresponse_header->return_code);

    struct tt_Endpoint* endpoint = find_endpoint(node, tt_KIND_SERVICE_CLIENT, endpoint_id);
    if (endpoint == NULL) {
        return true;
    }

    struct tt_Client* client = (struct tt_Client*)endpoint;

    // Only the response to the call that's still outstanding counts. A duplicate (the server
    // answered both the original request and a retry that crossed it on the wire) or a late one
    // (arriving after call_retry() already gave up, see its own client->callback(0, NULL)) would
    // otherwise invoke client->callback a second time and pollute the latency EMA with a stale
    // cache_time. client->cache == NULL means nothing is outstanding; a seq_no mismatch means
    // this is an answer to some earlier call.
    if (client->cache == NULL) {
        TT_LOG_DEBUG("CallResponse with no outstanding call, ignoring");
        return true;
    }
    const struct tt_CallRequestHeader* cached_request =
        (const struct tt_CallRequestHeader*)((const uint8_t*)client->cache + sizeof(struct tt_SubmessageHeader));
    if (cached_request->seq_no != seq_no) {
        TT_LOG_DEBUG("CallResponse seq_no %u != outstanding %u, ignoring", seq_no, cached_request->seq_no);
        return true;
    }

    struct tt_Service* service = client->service;
    uint32_t latency = calculate_latency(client->cache_time, tt_get_ns());

    struct tt_Response* response = NULL;
    uint8_t response_buffer[service->response_size];

    if (callresponse_header->return_code == 0) {
        int32_t decoded = service->response_decode((struct tt_Response*)response_buffer, buffer + head, tail - head,
                                                   tt_is_native_endian(header));

        if (decoded < 0) {
            TT_LOG_ERROR("Cannot decode response: %d", decoded);
            return false;
        }

        response = (struct tt_Response*)response_buffer;
    }

    client->cache = NULL;
    // The call is done; drop its still-pending retry timer so it doesn't occupy a scheduler slot
    // until it fires and no-ops (call_retry() already guards on cache == NULL).
    tt_Node_unschedule(node, call_retry, client);

    if (client->latency == 0) {
        client->latency = latency;
    } else {
        // Latency moving average
        // client->latency * 0.875 + latency * 0.125
        client->latency = (client->latency - (client->latency >> 3)) + (latency >> 3);
    }

    client->callback(client, callresponse_header->return_code, response);

    if (response != NULL) {
        service->response_free(response);
    }

    return true;
}

static bool process_submessage(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                               uint32_t body_tail, const struct tt_SubmessageHeader* submessage_header,
                               uint32_t sender_ip, uint16_t sender_port) {
    // Each process_X() below already logs its own specific reason on failure, so this switch
    // doesn't log again on top of that - only the type dispatch itself gets a message here.
    // sender_ip/sender_port (this packet's own source, from tt_receive() - see
    // handle_receive_result()) reach process_callrequest() (to unicast the CallResponse straight
    // back) and process_update() (to learn/refresh a peer table entry - see decode_update_
    // entities()'s own comment); the other cases don't need them.
    switch (submessage_header->type) {
    case tt_SUBMESSAGE_TYPE_UPDATE:
        process_update(node, header, buffer, head, body_tail, sender_ip, sender_port);
        return true;
    case tt_SUBMESSAGE_TYPE_DATA:
        process_data(node, header, buffer, head, body_tail);
        return true;
    case tt_SUBMESSAGE_TYPE_ACKNACK:
        // Reliable pub/sub isn't implemented in this release. Skip this one submessage and keep
        // parsing the rest of the datagram rather than dropping the whole packet - a peer that
        // does send ACKNACK will usually batch it alongside DATA/UPDATE we do understand.
        TT_LOG_WARNING("ACKNACK submessage not supported in this release, skipping");
        return true;
    case tt_SUBMESSAGE_TYPE_CALLREQUEST:
        process_callrequest(node, header, buffer, head, body_tail, sender_ip, sender_port);
        return true;
    case tt_SUBMESSAGE_TYPE_CALLRESPONSE:
        process_callresponse(node, header, buffer, head, body_tail);
        return true;
    default:
        // An unknown type is most likely a submessage from a newer protocol revision (see
        // validate_packet_header()'s "accept higher version"). Its length was already validated
        // by the caller, so skip exactly that far and carry on instead of discarding the packet.
        TT_LOG_WARNING("Unknown submessage type %d, skipping (len %u)", submessage_header->type, body_tail - head);
        return true;
    }
}

// Accept higher version while ignoring the reserved field. But not lower version.
static bool validate_packet_header(struct tt_Header* header) {
    if (!tt_is_native_endian(header) && !tt_is_reverse_endian(header)) {
        TT_LOG_ERROR("Illegal magic: 0x%04x", header->magic_value);
        return false;
    }

    TT_LOG_DEBUG("magic: 0x%04x (%c%c)", header->magic_value, header->magic[0], header->magic[1]);

    if (header->version < tt_VERSION) {
        TT_LOG_ERROR("Illegal version: %d < %d", header->version, tt_VERSION);
        return false;
    }

    return true;
}

enum submessage_walk_result { SUBMSG_ERROR, SUBMSG_DONE, SUBMSG_CONTINUE };

// Decodes and dispatches one submessage starting at *head, advancing *head past it.
static enum submessage_walk_result process_one_submessage(struct tt_Node* node, struct tt_Header* header,
                                                          uint8_t* buffer, uint32_t* head, uint32_t tail,
                                                          uint32_t sender_ip, uint16_t sender_port) {
    struct tt_SubmessageHeader* submessage_header =
        decode(node, buffer, head, tail, sizeof(struct tt_SubmessageHeader));
    if (submessage_header == NULL) {
        TT_LOG_DEBUG("End of submessage: %d", tail - *head);
        return SUBMSG_DONE;
    }

    uint16_t sub_length = rd16(header, submessage_header->length);

    TT_LOG_DEBUG("type: %d", submessage_header->type);
    TT_LOG_DEBUG("receiver: %d", submessage_header->receiver);
    TT_LOG_DEBUG("length: %d / %ld", sub_length, tail - *head + sizeof(struct tt_SubmessageHeader));

    if (sub_length < sizeof(struct tt_SubmessageHeader) ||
        sub_length > tail - *head + sizeof(struct tt_SubmessageHeader)) {
        TT_LOG_ERROR("Illegal submessage length: %d < %ld || %d > %ld", sub_length, sizeof(struct tt_SubmessageHeader),
                     sub_length, tail - *head + sizeof(struct tt_SubmessageHeader));
        return SUBMSG_ERROR;
    }

    const uint32_t body_tail = *head + sub_length - sizeof(struct tt_SubmessageHeader);
    if ((submessage_header->receiver == tt_SUBMESSAGE_ID_ALL || submessage_header->receiver == node->id) &&
        !process_submessage(node, header, buffer, *head, body_tail, submessage_header, sender_ip, sender_port)) {
        return SUBMSG_ERROR;
    }

    *head += sub_length - sizeof(struct tt_SubmessageHeader);
    return SUBMSG_CONTINUE;
}

static bool process_packet(struct tt_Node* node, uint8_t* buffer, uint32_t head, uint32_t tail, uint32_t sender_ip,
                           uint16_t sender_port) {
    struct tt_Header* header = decode(node, buffer, &head, tail, sizeof(struct tt_Header));
    if (header == NULL) {
        TT_LOG_ERROR("RX buffer underflow");
        return false;
    }

    if (!validate_packet_header(header)) {
        return false;
    }

    // Self sent message
    if (header->source == node->id) {
        TT_LOG_DEBUG("Self sent packet");
        return true;
    }
    TT_LOG_DEBUG("source: %d", header->source);

    while (true) {
        enum submessage_walk_result result =
            process_one_submessage(node, header, buffer, &head, tail, sender_ip, sender_port);
        if (result == SUBMSG_DONE) {
            break;
        }
        if (result == SUBMSG_ERROR) {
            return false;
        }
    }

    return true;
}

// Handles one tt_receive() outcome: on timeout/error/success that should end the poll, fills in
// *result and returns true; on a timeout that was just a short wait for a due scheduler entry
// (not the caller's real timeout), returns false so the caller keeps polling.
// Decodes and dispatches one just-received datagram of `len` bytes now sitting in node->rx_buffer.
static tt_ret_t process_datagram(struct tt_Node* node, int32_t len, uint32_t ip, uint16_t port) {
    node->rx_tail = (uint32_t)len;

    TT_LOG_DEBUG("Process packet from addr: %d.%d.%d.%d:%d len: %d", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                 (ip >> BITS_IN_1BYTE) & MASK_8BIT, (ip >> 0) & MASK_8BIT, port, len);

    if (!process_packet(node, node->rx_buffer, 0, len, ip, port)) {
        TT_LOG_ERROR("Cannot process packet");
        return tt_RET_PROTOCOL_ERROR;
    }

    return tt_RET_OK;
}

// After tt_receive() hands tt_Node_poll() the first datagram, pull whatever else the kernel
// already has buffered without another poll() per packet - a saturated receiver otherwise pays
// poll()+recvfrom() per packet instead of one poll() per drain. Best-effort: stops on the first
// "nothing waiting", a protocol error, or an I/O error (the outer poll picks that back up).
static tt_ret_t drain_rx(struct tt_Node* node, tt_ret_t first_result) {
    if (first_result != tt_RET_OK) {
        return first_result;
    }

    while (true) {
        uint32_t ip = 0;
        uint16_t port = 0;
        int32_t len = tt_try_receive(node, node->rx_buffer, tt_MAX_BUFFER_LENGTH, &ip, &port);
        if (len < 0) {
            break; // -1 nothing waiting, -2 I/O error - either way, done draining
        }

        tt_ret_t result = process_datagram(node, len, ip, port);
        if (result != tt_RET_OK) {
            return result;
        }
    }

    return tt_RET_OK;
}

static bool handle_receive_result(struct tt_Node* node, int32_t len, uint32_t ip, uint16_t port,
                                  bool woke_for_scheduler, tt_ret_t* result) {
    if (len == -1) { // Timeout
        if (woke_for_scheduler) {
            return false;
        }
        *result = tt_RET_TIMEOUT;
        return true;
    }

    if (len < 0) { // I/O error
        *result = tt_RET_IO_ERROR;
        return true;
    }

    *result = process_datagram(node, len, ip, port);
    return true;
}

tt_ret_t tt_Node_poll(struct tt_Node* node, int64_t timeout) {
    // Set default timeout
    if (timeout < 0) {
        timeout = tt_RECEIVE_TIMEOUT;
    }

    uint64_t time = tt_get_ns();

    // timeout == 0: one non-blocking pass - run everything due now, drain whatever RX is already
    // waiting, return. No poll()/select() wait at all. For a caller that just wants to make
    // progress and get straight back to its own work (a tight publish loop with -i 0), where a
    // sub-millisecond "wait" would otherwise round up to a full 1ms poll() and throttle it -
    // and where relying on broadcast self-receive to keep that poll() returning early breaks the
    // moment the publisher switches to unicast.
    if (timeout == 0) {
        struct tt_TCB* tcb;
        while ((tcb = peek_scheduler(node)) != NULL && tcb->time <= time) {
            tcb->function(node, time, tcb->param);
            pop_scheduler(node);
        }

        uint32_t ip = 0;
        uint16_t port = 0;
        int32_t len = tt_try_receive(node, node->rx_buffer, tt_MAX_BUFFER_LENGTH, &ip, &port);
        if (len < 0) {
            return tt_RET_TIMEOUT;
        }
        return drain_rx(node, process_datagram(node, len, ip, port));
    }

    while (timeout > 0) {
        struct tt_TCB* tcb = peek_scheduler(node);

        if (tcb != NULL && tcb->time <= time) {
            // Run scheduler first
            tcb->function(node, time, tcb->param);
            pop_scheduler(node);
        } else {
            // Run network I/O next
            int64_t rest = timeout;
            bool woke_for_scheduler = false;
            if (tcb != NULL && tcb->time - time < (uint64_t)timeout) {
                rest = (int64_t)(tcb->time - time);
                woke_for_scheduler = true;
            }

            uint32_t ip = 0;
            uint16_t port = 0;
            int32_t len = tt_receive(node, node->rx_buffer, tt_MAX_BUFFER_LENGTH, &ip, &port, rest);

            tt_ret_t result;
            if (handle_receive_result(node, len, ip, port, woke_for_scheduler, &result)) {
                return drain_rx(node, result);
            }
        }

        uint64_t new_time = tt_get_ns();
        timeout -= (int64_t)(new_time - time);
        time = new_time;
    }

    return tt_RET_TIMEOUT;
}

tt_ret_t tt_Node_destroy(struct tt_Node* node) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    uint64_t time = tt_get_ns();

    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        node->endpoints[i] = NULL;

        if (endpoint == NULL) {
            continue;
        }

        if ((endpoint->kind & tt_KIND_SENDER) == tt_KIND_SENDER) {
            node->last_modified = time;
        }

        // Both the client call cache and server response caches are fixed buffers now (no
        // malloc/free), so this just needs clearing. The scheduler entries referencing them are
        // about to be wiped wholesale below anyway, so there's no need to unschedule them
        // individually here.
        if (endpoint->kind == tt_KIND_SERVICE_CLIENT) {
            struct tt_Client* client = (struct tt_Client*)endpoint;
            client->cache = NULL;
        } else if (endpoint->kind == tt_KIND_SERVICE_SERVER) {
            struct tt_Server* server = (struct tt_Server*)endpoint;
            for (int j = 0; j < tt_MAX_SERVER_CACHE_COUNT; j++) {
                server->cache[j] = NULL;
                server->clean_scheduled[j] = false;
            }
        }
    }
    node->endpoint_count = 0;

    // Broadcast a final, entity-less UPDATE so peers can drop this node right away
    // (forget_peers_from_source() on their side) instead of carrying it until - nothing, there's
    // no other expiry. node_update() only batches it into tx_buffer; flush it out here, before
    // the socket closes below, since node_flush()'s tick is about to be cancelled too.
    node_update(node, time, NULL);
    if (!flush_tx(node, node->tx_tail, NULL, 0)) {
        TT_LOG_WARNING("Could not send farewell announce on node destroy");
    }

    // The node is fully torn down at this point; drop every pending scheduler entry
    // (including the node_update/node_flush ones just re-armed above) so nothing later
    // fires a callback into this now-destroyed node.
    node->scheduler_tail = 0;

    tt_close(node);

    return tt_RET_OK;
}

const char* tt_version(void) {
    return TICKLE_VERSION_STRING;
}
