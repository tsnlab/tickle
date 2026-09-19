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
// skip_unrecoverable_backlog()'s own bulk-skip (QoS roadmap #5, acknack_retry()'s give-up path)
// relies on a retained-sample window this narrow always fitting inside
// tt_Subscriber.received_bitmap's own tt_RELIABLE_BITMAP_BITS-wide tracking window.
_Static_assert(tt_MAX_RELIABLE_HISTORY <= tt_RELIABLE_BITMAP_BITS,
               "tt_MAX_RELIABLE_HISTORY must fit within the reliable ACKNACK bitmap window");

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
//
// Returns whether this call claimed a previously-empty slot (a genuinely new-to-this-table
// node_id), as opposed to refreshing one already there - QoS roadmap #4 (DURABILITY) needs this
// from decode_update_entities()'s own call site, to trigger a one-time retained-sample backlog
// delivery instead of on every periodic UPDATE refresh. Most callers (the Client/Server peer
// direction) still just ignore the return value, which is fine in C.
static bool upsert_peer(struct tt_Peer* peers, uint8_t node_id, uint32_t ip, uint16_t port) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].node_id == node_id) {
            peers[i].ip = ip;
            peers[i].port = port;
            return false;
        }
    }

    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].node_id == tt_NODE_ID_INVALID) {
            peers[i].node_id = node_id;
            peers[i].ip = ip;
            peers[i].port = port;
            return true;
        }
    }

    TT_LOG_WARNING("Peer table full (%d), dropping newly seen peer node %u", tt_MAX_PEER_COUNT, node_id);
    return false;
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

// Records one remote entity into node->discovery (tt_Node_set_discovery(), rmw_tickle/PLAN.md's
// Milestone 0(c)), refreshing its existing slot or claiming the first empty one, then fires the
// appear/refresh callback. No-op (not even the callback) if no discovery cache is attached -
// every caller below calls this unconditionally rather than checking node->discovery first, the
// same way logging macros check their own level instead of every call site checking it.
static void upsert_discovered_entity(struct tt_Node* node, uint8_t node_id, uint32_t endpoint_id, uint8_t kind,
                                     uint8_t qos, const char* type, const char* name) {
    if (node->discovery == NULL) {
        return;
    }

    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    struct tt_DiscoveredEntity* slot = NULL;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (entities[i].node_id == node_id && entities[i].endpoint_id == endpoint_id) {
            slot = &entities[i];
            break;
        }
    }
    if (slot == NULL) {
        // Prefer a truly-empty slot; fall back to reclaiming the first tombstoned one (struct
        // tt_DiscoveredEntity.alive's own doc comment, tickle.h) rather than dropping a genuinely
        // new entity on the floor while the table still has room for it in spirit, just not in a
        // never-used slot - tombstones are remembered on a best-effort basis, not guaranteed.
        struct tt_DiscoveredEntity* tombstone_slot = NULL;
        for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
            if (entities[i].node_id == tt_NODE_ID_INVALID) {
                slot = &entities[i];
                break;
            }
            if (tombstone_slot == NULL && !entities[i].alive) {
                tombstone_slot = &entities[i];
            }
        }
        if (slot == NULL) {
            slot = tombstone_slot;
        }
    }
    if (slot == NULL) {
        TT_LOG_WARNING("Discovery table full (%d), dropping newly seen entity node %u endpoint %08x",
                       tt_MAX_DISCOVERED_ENTITIES, node_id, endpoint_id);
        return;
    }

    slot->node_id = node_id;
    slot->endpoint_id = endpoint_id;
    slot->kind = kind;
    slot->qos = qos;
    slot->alive = true;
    size_t type_len = _tt_strnlen(type, tt_MAX_NAME_LENGTH);
    _tt_memcpy(slot->type, type, type_len);
    slot->type[type_len] = '\0';
    size_t name_len = _tt_strnlen(name, tt_MAX_NAME_LENGTH);
    _tt_memcpy(slot->name, name, name_len);
    slot->name[name_len] = '\0';

    if (node->discovery_callback != NULL) {
        node->discovery_callback(node, node_id, endpoint_id, kind, /*departed=*/false, node->discovery_callback_param);
    }
}

// The discovery-cache counterpart to forget_peers_from_source() - same "authoritative announce
// supersedes old state" reasoning (a fresh UPDATE means decode_update_entities() is about to
// re-add whatever `node_id` still actually hosts, so anything not re-added here first must have
// been dropped). A real removal (the slot is freed, not tombstoned) - this is a normal,
// intentional departure (an explicit farewell, or the node simply not listing this entity
// anymore), not a liveliness failure, and struct tt_DiscoveredEntity.alive's own doc comment (QoS
// roadmap #3, RMW_EVENT_LIVELINESS_CHANGED.not_alive_count) explicitly excludes normal deletion
// from "not alive" - see tombstone_discovered_entities_from_source() below for the liveliness-
// timeout counterpart that keeps the entity instead. No-op if no discovery cache is attached.
static void forget_discovered_entities_from_source(struct tt_Node* node, uint8_t node_id) {
    if (node->discovery == NULL) {
        return;
    }

    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (entities[i].node_id != node_id) {
            continue;
        }
        uint32_t endpoint_id = entities[i].endpoint_id;
        uint8_t kind = entities[i].kind;
        entities[i].node_id = tt_NODE_ID_INVALID;
        if (node->discovery_callback != NULL) {
            node->discovery_callback(node, node_id, endpoint_id, kind, /*departed=*/true,
                                     node->discovery_callback_param);
        }
    }
}

// check_liveliness()'s own counterpart to forget_discovered_entities_from_source() just above -
// same trigger (a source presumed dead) but tombstones instead of freeing the slot (alive =
// false, entity otherwise left intact) so QoS roadmap #3's own RMW_EVENT_LIVELINESS_CHANGED.
// not_alive_count (a live snapshot, rmw_tickle/PLAN.md) has a real "known but not currently alive"
// set to count instead of always reporting 0. Still fires the discovery callback with
// departed=true - an existing plain appear/depart consumer doesn't need to know about the
// tombstone distinction, only rmw_tickle_c's own count_not_alive_matching_locked() (rmw_graph.c)
// needs to see the .alive flag directly. No-op if no discovery cache is attached.
static void tombstone_discovered_entities_from_source(struct tt_Node* node, uint8_t node_id) {
    if (node->discovery == NULL) {
        return;
    }

    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (entities[i].node_id != node_id || !entities[i].alive) {
            continue; // not from this source, or already tombstoned - nothing new to report
        }
        entities[i].alive = false;
        if (node->discovery_callback != NULL) {
            node->discovery_callback(node, node_id, entities[i].endpoint_id, entities[i].kind, /*departed=*/true,
                                     node->discovery_callback_param);
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

// QoS roadmap #1 (RxO matching, Milestone 31) - the tt_UPDATE_QOS_* bits this endpoint's own
// UpdateEntity announces, offered (a Publisher) or requested (a Subscriber) depending on kind -
// see their own doc comment (tickle.h). Always 0 for a service/client: RELIABILITY there is
// already unconditional via tt_Client_call()'s own retry (no negotiation needed), and DURABILITY
// has no service/client analog at all - matches endpoint_type_name()'s own kind-dispatch shape.
static uint8_t endpoint_qos_bits(struct tt_Endpoint* endpoint) {
    switch (endpoint->kind) {
    case tt_KIND_TOPIC_PUBLISHER: {
        struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
        return (uint8_t)((pub->reliable ? tt_UPDATE_QOS_RELIABLE : 0) | (pub->durable ? tt_UPDATE_QOS_DURABLE : 0));
    }
    case tt_KIND_TOPIC_SUBSCRIBER: {
        struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
        return (uint8_t)((sub->reliable ? tt_UPDATE_QOS_RELIABLE : 0) | (sub->durable ? tt_UPDATE_QOS_DURABLE : 0));
    }
    default:
        return 0;
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
static void check_liveliness(struct tt_Node* node, uint64_t time, void* param);
static void server_cache_clean(struct tt_Node* node, uint64_t time, void* param);
static void clear_server_cache_slot(struct tt_Server* server, int slot);
// QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - see each definition's own comment.
static void acknack_retry(struct tt_Node* node, uint64_t time, void* param);
static void send_acknack(struct tt_Node* node, struct tt_Subscriber* sub);
static void advance_ack_seq_no(struct tt_Subscriber* sub);
static void skip_unrecoverable_backlog(struct tt_Subscriber* sub);
static void maybe_arm_acknack_retry(struct tt_Node* node, struct tt_Subscriber* sub);
static void update_reliable_ack(struct tt_Node* node, struct tt_Subscriber* sub, uint32_t seq_no,
                                uint8_t sender_node_id, uint32_t sender_ip, uint16_t sender_port);
static void jump_ack_baseline(struct tt_Subscriber* sub, uint32_t seq_no);
static int highest_relevant_bit(const struct tt_Subscriber* sub);
// QoS roadmap #5 (RELIABILITY) follow-up - Heartbeat, see struct tt_HeartbeatHeader's own doc
// comment (tickle.h).
static void send_heartbeat(struct tt_Node* node, uint64_t time, void* param);
static void send_initial_heartbeat(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Peer* target);
static bool process_heartbeat(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                              uint32_t tail, uint32_t sender_ip, uint16_t sender_port);
// QoS roadmap #4 (DURABILITY/TRANSIENT_LOCAL, rmw_tickle/PLAN.md) - see its own definition's comment.
static void deliver_durability_backlog(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Peer* target);

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
        node->update_last_seen[i] = 0;
    }

    node->discovery = NULL;
    node->discovery_callback = NULL;
    node->discovery_callback_param = NULL;

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

    if (!tt_Node_schedule(node, basetime + tt_NODE_UPDATE_INTERVAL, check_liveliness, NULL)) {
        TT_LOG_ERROR("Cannot schedule check_liveliness");
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
// process_data(), process_callrequest(), process_callresponse()), and nothing on the wire can
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
        server->slot_state[i] = tt_SERVER_SLOT_EMPTY;
        server->pending_timeout_scheduled[i] = false;
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
    pub->batch = false;           // see tickle.h's own doc comment on this field for why this is the default
    pub->reliable_cache = NULL;   // no retained-sample storage by default - see its own doc comment
    pub->reliable = false;        // best-effort by default - see tt_Publisher.reliable's own doc comment
    pub->durable = false;         // volatile by default - see tt_Publisher.durable's own doc comment
    pub->heartbeat_period_ns = 0; // no periodic Heartbeat by default - see its own doc comment
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
    sub->reliable = false; // best-effort by default - see tickle.h's own doc comment
    sub->ack_seq_no = 1;   // a Publisher's first sample is always seq_no 1, never 0
    sub->received_bitmap = 0;
    sub->reliable_sender_node_id = tt_NODE_ID_INVALID;
    sub->reliable_retry = 0;
    sub->reliable_acknack_scheduled = false;
    sub->reliable_heartbeat_last_seq_no = 0; // no Heartbeat seen yet - see its own doc comment

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

// QoS roadmap #5 (RELIABILITY/RELIABLE) / #4 (DURABILITY/TRANSIENT_LOCAL) - snapshot the
// just-encoded DATA submessage (header through CDR) into the ring before end_encode() below, same
// "cache first, then flush" order tt_Client_call() already uses for its own single-slot cache.
// Both QoS policies share this one cache (struct tt_ReliableCache's own doc comment, tickle.h): a
// later incoming ACKNACK (process_acknack()) resends this exact copy verbatim, and a newly-
// discovered Subscriber (deliver_durability_backlog()) gets every currently-retained entry pushed
// straight to it, whichever of the two (or both) this Publisher opted into. KEEP_LAST eviction
// once `depth` slots are full (mirrors rmw_subscription.c's own queue eviction), not an error.
// Split out of tt_Publisher_publish() below purely to keep that function's own cognitive
// complexity under clang-tidy's threshold.
static void cache_reliable_sample(struct tt_Node* node, struct tt_SubmessageHeader* submessage_header,
                                  struct tt_ReliableCache* cache, uint32_t seq_no) {
    uint16_t depth =
        (cache->depth > 0 && cache->depth <= tt_MAX_RELIABLE_HISTORY) ? cache->depth : tt_MAX_RELIABLE_HISTORY;
    size_t length = ROUNDUP((uintptr_t)node->tx_buffer + node->tx_tail - (uintptr_t)submessage_header);
    struct tt_ReliableCacheEntry* cache_entry = &cache->entries[cache->next % depth];
    _tt_memcpy(cache_entry->buffer, submessage_header, length);
    cache_entry->seq_no = seq_no;
    cache_entry->len = (uint16_t)length;
    cache_entry->retry = 0;
    cache_entry->timestamp = tt_get_ns();
    cache->next = (cache->next + 1) % depth;
}

// QoS roadmap #6 (LIFESPAN) - see tt_Publisher.lifespan_duration_ns's own doc comment (tickle.h).
// lifespan_duration_ns == 0 means "no LIFESPAN requested" - never expired, matching every other
// disabled-by-zero convention this struct already uses (heartbeat_period_ns, etc).
static bool reliable_cache_entry_expired(const struct tt_ReliableCacheEntry* entry, uint64_t lifespan_duration_ns) {
    return lifespan_duration_ns != 0 && (tt_get_ns() - entry->timestamp) >= lifespan_duration_ns;
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
    // A reliable or durable Publisher (below) always needs the staging copy path so it has encoded
    // bytes at a known tx_buffer location to save into reliable_cache - zero-copy publishes
    // straight from the caller's own tt_Data, nothing to retain for a later retransmit or backlog
    // delivery.
    if (pub->topic->data_encode_inplace != NULL && old_tx_tail == sizeof(struct tt_Header) &&
        pub->reliable_cache == NULL) {
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

    // QoS roadmap #5 (RELIABILITY) / #4 (DURABILITY) - see cache_reliable_sample()'s own doc
    // comment above; one shared write serves both, whichever (or both) this Publisher opted into.
    if (pub->reliable_cache != NULL) {
        cache_reliable_sample(node, submessage_header, pub->reliable_cache, pub->seq_no + 1);
    }

    // pub->batch (default false, tt_Node_create_publisher() - see tickle.h's own doc comment on
    // it for why immediate is the default now): mirrors tt_Client_call()'s own peer decision and
    // shared-tx_buffer guard exactly - unicast to pub->peers when there's a small enough known
    // count (tt_UNICAST_PEER_THRESHOLD) *and* nothing else (e.g. a still-batched UPDATE announce
    // from node_update()) was already sitting unflushed ahead of this DATA submessage, since
    // unicasting would only reach these peers, not whatever else needs the whole segment.
    // pub->batch == true keeps today's behavior unconditionally: never flush here, let
    // node_flush()'s own tt_NODE_TX_INTERVAL tick decide broadcast vs. unicast for the whole
    // accumulated buffer at once.
    bool is_flush = !pub->batch;
    const struct tt_Peer* peers = NULL;
    uint8_t peer_count = 0;
    if (is_flush) {
        uint8_t count = count_peers(pub->peers);
        if (count >= 1 && count <= tt_UNICAST_PEER_THRESHOLD && old_tx_tail == sizeof(struct tt_Header)) {
            peers = pub->peers;
            peer_count = count;
        }
    }
    if (!end_encode(node, submessage_header, is_flush, peers, peer_count)) {
        rollback(node, old_tx_tail);
        return tt_RET_IO_ERROR;
    }

    pub->seq_no++;

    return tt_RET_OK;
}

// Oldest still-retained seq_no in cache, or 0 if nothing is retained yet (seq_no 0 never occurs on
// the wire - tt_Publisher_publish()'s own data_header->seq_no = pub->seq_no + 1, starting from 1 -
// so it doubles as "empty" here). Shared by send_heartbeat()'s own periodic announce and send_
// initial_heartbeat()'s own discovery-triggered one-off, below.
static uint32_t reliable_cache_oldest_seq_no(struct tt_ReliableCache* cache) {
    uint16_t depth =
        (cache->depth > 0 && cache->depth <= tt_MAX_RELIABLE_HISTORY) ? cache->depth : tt_MAX_RELIABLE_HISTORY;
    uint32_t first_seq_no = 0;
    for (int i = 0; i < depth; i++) {
        if (cache->entries[i].len != 0 && (first_seq_no == 0 || cache->entries[i].seq_no < first_seq_no)) {
            first_seq_no = cache->entries[i].seq_no;
        }
    }
    return first_seq_no;
}

// Encodes and sends one Heartbeat submessage announcing [first_seq_no, pub->seq_no] to the given
// target(s) - split out of send_heartbeat()/send_initial_heartbeat() purely to keep cognitive
// complexity down and share the actual wire encoding between the periodic and discovery-triggered
// paths, same reasoning cache_reliable_sample()/cache_durable_sample() were split out of tt_
// Publisher_publish() for. peers/peer_count follow end_encode()'s own convention directly (NULL/0
// broadcasts).
static void encode_and_send_heartbeat(struct tt_Node* node, struct tt_Publisher* pub, uint32_t first_seq_no,
                                      const struct tt_Peer* peers, uint8_t peer_count) {
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    uint32_t old_tx_tail = node->tx_tail;

    struct tt_SubmessageHeader* submessage_header =
        start_encode(node, tt_SUBMESSAGE_TYPE_HEARTBEAT, tt_SUBMESSAGE_ID_ALL);
    if (submessage_header == NULL) {
        rollback(node, old_tx_tail);
        return;
    }
    struct tt_HeartbeatHeader* heartbeat_header = encode(node, sizeof(struct tt_HeartbeatHeader));
    if (heartbeat_header == NULL) {
        rollback(node, old_tx_tail);
        return;
    }
    heartbeat_header->endpoint_id = endpoint->id;
    heartbeat_header->first_available_seq_no = first_seq_no;
    heartbeat_header->last_seq_no = pub->seq_no;

    if (!end_encode(node, submessage_header, true, peers, peer_count)) {
        rollback(node, old_tx_tail);
    }
}

// QoS roadmap #5 (RELIABILITY) follow-up - runs once per pub->heartbeat_period_ns (armed by tt_
// Publisher_set_heartbeat_period()), announcing pub->reliable_cache's own currently-retained
// range - see struct tt_HeartbeatHeader's own doc comment (tickle.h) for what this buys over the
// purely-reactive gap detection RELIABILITY already had on its own. Skips sending (but still
// reschedules) when nothing has been published yet - entries[] is still entirely empty, nothing
// to announce, same "nothing retained yet" short-circuit deliver_durability_backlog() already has.
static void send_heartbeat(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;
    uint32_t first_seq_no = reliable_cache_oldest_seq_no(pub->reliable_cache);
    if (first_seq_no != 0) {
        // Same peer/broadcast decision tt_Publisher_publish() already makes for DATA.
        const struct tt_Peer* peers = NULL;
        uint8_t peer_count = 0;
        uint8_t count = count_peers(pub->peers);
        if (count >= 1 && count <= tt_UNICAST_PEER_THRESHOLD && node->tx_tail == sizeof(struct tt_Header)) {
            peers = pub->peers;
            peer_count = count;
        }
        encode_and_send_heartbeat(node, pub, first_seq_no, peers, peer_count);
    }

    if (!tt_Node_schedule(node, time + pub->heartbeat_period_ns, send_heartbeat, pub)) {
        TT_LOG_ERROR("Cannot schedule send_heartbeat");
    }
}

// QoS roadmap #5 (RELIABILITY) follow-up - fires once, the instant decode_update_entities()'s own
// upsert_peer() claims a previously-empty slot for this exact Publisher (a genuinely new - or
// forgotten-then-rejoined - peer, not every periodic UPDATE refresh), mirroring deliver_
// durability_backlog()'s own identical trigger exactly. Closes the race a purely periodic
// Heartbeat can't: a newly-matched Subscriber's very first few samples are also the ones a slow
// periodic period is most likely to arrive too late to save (by the time it fires, reliable_
// cache's own tiny KEEP_LAST window has already evicted them) - an immediate, one-off greeting
// reaches the new peer as soon as discovery itself completes instead. No-op unless pub->reliable
// is set (see its own doc comment - this is exactly the unprompted-traffic case that flag exists
// to gate, so a durable-only Publisher doesn't also start emitting Heartbeats nobody asked for)
// and pub->reliable_cache is non-NULL and non-empty (nothing published yet) - fires regardless of
// whether periodic Heartbeat (tt_Publisher_set_heartbeat_period()) was ever separately enabled.
static void send_initial_heartbeat(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Peer* target) {
    if (!pub->reliable || pub->reliable_cache == NULL) {
        return;
    }
    uint32_t first_seq_no = reliable_cache_oldest_seq_no(pub->reliable_cache);
    if (first_seq_no == 0) {
        return;
    }
    encode_and_send_heartbeat(node, pub, first_seq_no, target, 1);
}

// See struct tt_Publisher.heartbeat_period_ns's own doc comment (tickle.h) for why this needs an
// explicit call rather than just setting that field directly.
tt_ret_t tt_Publisher_set_heartbeat_period(struct tt_Publisher* pub, uint64_t period_ns) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (period_ns != 0 && pub->reliable_cache == NULL) {
        return tt_RET_INVALID_ARGUMENT; // nothing for a Heartbeat to announce without one
    }

    if (pub->heartbeat_period_ns != 0) {
        tt_Node_unschedule(pub->node, send_heartbeat, pub); // re-arming or disabling either way
    }
    pub->heartbeat_period_ns = period_ns;
    if (period_ns == 0) {
        return tt_RET_OK; // disabled
    }

    if (!tt_Node_schedule(pub->node, tt_get_ns() + period_ns, send_heartbeat, pub)) {
        pub->heartbeat_period_ns = 0;  // failed to arm - stay disabled rather than claim it's on
        return tt_RET_OUT_OF_SCHEDULE; // tt_MAX_SCHEDULER_LENGTH exhausted
    }
    return tt_RET_OK;
}

tt_ret_t tt_Publisher_destroy(struct tt_Publisher* pub) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;

    // Cancel a still-armed Heartbeat before this Publisher (its own schedule param) goes away -
    // same reasoning as tt_Subscriber_destroy()'s own acknack_retry cancellation just below.
    if (pub->heartbeat_period_ns != 0) {
        tt_Node_unschedule(pub->node, send_heartbeat, pub);
    }

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

    // Cancel any outstanding acknack_retry before this Subscriber (its own schedule param) goes
    // away - same reasoning as tt_Client_destroy()'s own call_retry cancellation.
    if (sub->reliable_acknack_scheduled) {
        tt_Node_unschedule(sub->node, acknack_retry, sub);
        sub->reliable_acknack_scheduled = false;
    }

    if (remove_endpoint_from_node(sub->node, endpoint)) {
        sub->node->last_modified = tt_get_ns();
        return tt_RET_OK;
    }

    return tt_RET_IILEGAL_ENDPOINT_ID;
}

// QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - encodes and unicasts one ACKNACK
// submessage back to sub->reliable_sender_*, reporting sub->ack_seq_no/received_bitmap. Not
// fatal on failure, same philosophy as resend_call_request()'s own comment: whichever caller
// armed a retry (update_reliable_ack()/acknack_retry()) will just try again.
// Highest bit index set in a tt_Subscriber's own received_bitmap (bit j: "received(ack_seq_no +
// j)" - see struct tt_Subscriber's own doc comment, tickle.h), or -1 if none are set. Shared by
// send_acknack() and skip_unrecoverable_backlog() below - both need "how far ahead does anything
// *confirmed* reach", not just "which bits happen to be 0".
static int highest_received_bit(uint64_t received_bitmap) {
    int highest = tt_RELIABLE_BITMAP_BITS - 1;
    while (highest >= 0 && !((received_bitmap >> (unsigned)highest) & 1)) {
        highest--;
    }
    return highest;
}

// QoS roadmap #5 (RELIABILITY) follow-up - the highest bit position (bit j: seq_no ack_seq_no + j
// needs attention, one way or another) this Subscriber currently has *any* reason to ask about -
// received_bitmap's own highest confirmed-out-of-order bit (highest_received_bit() above, the
// only signal before this follow-up existed), widened by the highest seq_no the most recent
// struct tt_HeartbeatHeader claimed the Publisher has published, if that reaches further. A
// Heartbeat can reveal the Subscriber is behind even with zero out-of-order DATA arrivals yet -
// received_bitmap alone is blind to that case, since nothing has set any bit in it. Shared by
// send_acknack() (what to actually request) and maybe_arm_acknack_retry() (whether there's
// anything to do at all) - both need the same widened answer, not just received_bitmap's own.
// -1 if neither signal has anything to report.
static int highest_relevant_bit(const struct tt_Subscriber* sub) {
    int highest = highest_received_bit(sub->received_bitmap);
    if (sub->reliable_heartbeat_last_seq_no >= sub->ack_seq_no) {
        uint64_t hb_offset = (uint64_t)sub->reliable_heartbeat_last_seq_no - sub->ack_seq_no;
        int hb_highest = hb_offset < tt_RELIABLE_BITMAP_BITS ? (int)hb_offset : tt_RELIABLE_BITMAP_BITS - 1;
        if (hb_highest > highest) {
            highest = hb_highest;
        }
    }
    return highest;
}

static void send_acknack(struct tt_Node* node, struct tt_Subscriber* sub) {
    if (sub->reliable_sender_node_id == tt_NODE_ID_INVALID) {
        return; // no reliable DATA seen yet to ack
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)sub;
    struct tt_Peer target = {sub->reliable_sender_node_id, sub->reliable_sender_ip, sub->reliable_sender_port};
    uint32_t old_tx_tail = node->tx_tail;

    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_ACKNACK, target.node_id);
    if (submessage_header == NULL) {
        rollback(node, old_tx_tail);
        return;
    }

    struct tt_AckNackHeader* acknack_header = encode(node, sizeof(struct tt_AckNackHeader));
    if (acknack_header == NULL) {
        rollback(node, old_tx_tail);
        return;
    }

    acknack_header->endpoint_id = endpoint->id;
    acknack_header->seq_no = sub->ack_seq_no;
    // wire direction is "please resend", opposite of received_bitmap - but masked to bits below
    // the highest *confirmed* arrival: a bare ~received_bitmap requested every one of all
    // tt_RELIABLE_BITMAP_BITS positions whenever received_bitmap had only a few bits set,
    // including positions the Publisher hasn't even sent yet (not "evicted", just nonexistent so
    // far) - the Publisher can't tell those apart from a genuinely lost sample, so they showed up
    // identically as "not found in reliable_cache", swamping every real one out. Found via
    // run_perf.sh's real loss-injection scenarios: the Publisher's own diagnostic saw ~4,000
    // "not found" ACKNACKs against a run of only ~500 total messages - only possible if most
    // requests were for seq_nos that were never sent, not actually lost ones.
    //
    // highest_relevant_bit() (not the plain received_bitmap-only highest_received_bit() this
    // comment's own numbers were found against) also considers the most recent Heartbeat's own
    // last_seq_no - QoS roadmap #5's own follow-up, struct tt_HeartbeatHeader's doc comment
    // (tickle.h) - widening the request range to cover a gap a Heartbeat revealed even when
    // nothing has arrived out of order yet to set any bit here at all.
    int highest = highest_relevant_bit(sub);
    uint64_t request_mask = 0;
    if (highest >= tt_RELIABLE_BITMAP_BITS - 1) {
        request_mask = ~0ULL; // highest is the top bit - avoid a 64-bit shift's own UB below
    } else if (highest >= 0) {
        request_mask = (1ULL << (highest + 1)) - 1;
    }
    acknack_header->bitmap = ~sub->received_bitmap & request_mask;

    // Unicast straight back to whoever's DATA this acks - same "nothing else queued" guard as
    // process_callrequest()'s own CallResponse. Falling back to broadcast when something else is
    // already staged is still correct here: the submessage's own receiver field (target.node_id,
    // not tt_SUBMESSAGE_ID_ALL) confines actual processing to that one node regardless of how the
    // packet physically went out.
    bool unicast = old_tx_tail == sizeof(struct tt_Header);
    if (!end_encode(node, submessage_header, true, unicast ? &target : NULL, unicast ? 1 : 0)) {
        rollback(node, old_tx_tail);
    }
}

// Scheduled (tt_Node_schedule()) while sub has an outstanding gap (sub->received_bitmap != 0),
// re-sending the ACKNACK on a timer for the case where no further DATA ever arrives to re-trigger
// update_reliable_ack() itself. Mirrors call_retry()'s own schedule/reschedule/give-up shape.
static void acknack_retry(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(time);

    struct tt_Subscriber* sub = param;

    if (sub->received_bitmap == 0) {
        // A DATA arrival already closed the gap since this timer was armed.
        sub->reliable_acknack_scheduled = false;
        return;
    }

    if (++sub->reliable_retry > tt_RELIABLE_RETRY) {
        TT_LOG_WARNING("Giving up on a reliable sample after %d ACKNACK retries", tt_RELIABLE_RETRY);
        sub->reliable_acknack_scheduled = false;
        // Give up on ack_seq_no itself - the same "advance past it" advance_ack_seq_no() already
        // does for a real receipt, since from here on it makes no difference *why* nothing more
        // is waiting on it. A different, still-outstanding gap further ahead in the window (if
        // any) is untouched - it gets its own full tt_RELIABLE_RETRY budget against whatever
        // ack_seq_no ends up being next (advance_ack_seq_no()'s own reset of reliable_retry is
        // what actually grants that fresh budget) - and, unlike leaving it to the next DATA
        // arrival to notice, maybe_arm_acknack_retry() below starts requesting it immediately.
        advance_ack_seq_no(sub);
        // Now that ack_seq_no itself just had its own fair tt_RELIABLE_RETRY attempts and still
        // didn't resolve, also bulk-skip anything *else* already provably unrecoverable for the
        // identical reason (see skip_unrecoverable_backlog()'s own comment) - but only here, at
        // the natural give-up point, never pre-empting an still-in-progress retry cycle the way
        // doing this reactively on every new arrival did.
        skip_unrecoverable_backlog(sub);
        maybe_arm_acknack_retry(node, sub);
        return;
    }

    send_acknack(node, sub);

    uint32_t interval = tt_RELIABLE_DEADLINE != 0 ? tt_RELIABLE_DEADLINE : tt_CALL_RETRY_INTERVAL;
    if (!tt_Node_schedule(node, tt_get_ns() + interval, acknack_retry, sub)) {
        TT_LOG_ERROR("Cannot schedule acknack_retry");
        sub->reliable_acknack_scheduled = false;
    }
}

// Confirms sub->ack_seq_no itself (whether just received, or - acknack_retry()'s own call site -
// given up on after too many retries) and advances past it, keeping received_bitmap correctly
// realigned: bit j always means "received(ack_seq_no + j)", matching tt_AckNackHeader's own wire
// convention exactly (see struct tt_Subscriber's own doc comment, tickle.h) - which is why this
// shifts once *unconditionally* for this first step, not only inside the while loop below. A
// previous version only shifted inside the while loop, silently misaligning every subsequent
// bit's meaning by one position after any exact-match advance - found via run_perf.sh's real
// tc/netem loss-injection scenarios reporting RELIABLE recovering only partially, not because
// retransmission itself was failing, but because the ACKNACK requests it was reacting to were
// silently asking for the wrong sequence numbers (already-received ones) while dropping the
// genuinely still-missing one off the request entirely.
//
// Also resets reliable_retry to 0 unconditionally - whatever is now the oldest outstanding gap
// (if any remain - received_bitmap may still be nonzero here) is a *different* sample than the
// one reliable_retry was counting attempts against, and deserves its own full tt_RELIABLE_RETRY
// budget, not whatever was left over. Before this reset lived here, two losses close enough
// together that a second gap was still open when the first resolved would make the second one
// inherit however many attempts the first had already used - found the same way as the bitmap
// bug above: real tc/netem loss-injection runs recovering measurably worse than
// p^(tt_RELIABLE_RETRY + 1) predicts for a single isolated loss.
static void advance_ack_seq_no(struct tt_Subscriber* sub) {
    sub->ack_seq_no++;
    sub->received_bitmap >>= 1;
    while (sub->received_bitmap & 1) { // absorb whatever out-of-order run already follows it
        sub->received_bitmap >>= 1;
        sub->ack_seq_no++;
    }
    sub->reliable_retry = 0;
}

// Called only from acknack_retry()'s own give-up path, right after advance_ack_seq_no() - never
// reactively on a new arrival (see update_reliable_ack()'s own comment on why that backfired).
// ack_seq_no itself just had its fair tt_RELIABLE_RETRY attempts and still didn't resolve; this
// additionally skips anything *else* now provably unrecoverable too, in one step: everything more
// than tt_MAX_RELIABLE_HISTORY behind the highest sample already confirmed received (out of
// order, still recorded in received_bitmap) is guaranteed evicted from the Publisher's own
// KEEP_LAST reliable_cache by now, for the identical "no amount of retrying will un-evict it"
// reason ack_seq_no itself was just given up on. Without this, once one position needed a full
// give-up cycle, everything behind it also needed its own full cycle serially, one at a time,
// even though most of that range was just as hopeless from the moment it first appeared.
static void skip_unrecoverable_backlog(struct tt_Subscriber* sub) {
    if (sub->received_bitmap == 0) {
        return; // nothing else known to be ahead - nothing to skip
    }

    int highest = highest_received_bit(sub->received_bitmap);
    // highest's own absolute sequence number is ack_seq_no + highest (received_bitmap's own bit
    // j means "received(ack_seq_no + j)" - see struct tt_Subscriber's own doc comment, tickle.h).
    uint32_t highest_seq_no = sub->ack_seq_no + (uint32_t)highest;
    if (highest_seq_no - sub->ack_seq_no + 1 <= tt_MAX_RELIABLE_HISTORY) {
        return; // still within a plausibly-recoverable window - let it resolve normally
    }

    uint32_t new_ack_seq_no = highest_seq_no - tt_MAX_RELIABLE_HISTORY + 1;
    uint32_t skipped = new_ack_seq_no - sub->ack_seq_no;
    sub->received_bitmap = skipped < tt_RELIABLE_BITMAP_BITS ? (sub->received_bitmap >> skipped) : 0;
    sub->ack_seq_no = new_ack_seq_no;
    while (sub->received_bitmap & 1) { // absorb whatever's already confirmed right after the jump
        sub->received_bitmap >>= 1;
        sub->ack_seq_no++;
    }
}

// Shared by update_reliable_ack() (a new/changed gap), process_heartbeat() (a Heartbeat revealing
// one even with received_bitmap still 0), and acknack_retry()'s own give-up path (the gap just
// written off might not have been the only one outstanding): unschedules cleanly once nothing is
// left to ask for, or sends an ACKNACK for whatever's still missing and (re-)arms the retry timer
// if one isn't already running. Splitting this out means a give-up no longer leaves a remaining,
// different gap waiting on the next DATA arrival before anything asks for it again.
static void maybe_arm_acknack_retry(struct tt_Node* node, struct tt_Subscriber* sub) {
    if (highest_relevant_bit(sub) < 0) {
        // No outstanding gap by either signal (received_bitmap or the last Heartbeat) - a healthy
        // stream needs no ACKNACK at all.
        if (sub->reliable_acknack_scheduled) {
            tt_Node_unschedule(node, acknack_retry, sub);
            sub->reliable_acknack_scheduled = false;
        }
        sub->reliable_retry = 0;
        return;
    }

    send_acknack(node, sub);

    if (!sub->reliable_acknack_scheduled) {
        sub->reliable_retry = 0;
        uint32_t interval = tt_RELIABLE_DEADLINE != 0 ? tt_RELIABLE_DEADLINE : tt_CALL_RETRY_INTERVAL;
        if (tt_Node_schedule(node, tt_get_ns() + interval, acknack_retry, sub)) {
            sub->reliable_acknack_scheduled = true;
        } else {
            TT_LOG_ERROR("Cannot schedule acknack_retry");
        }
    }
}

// Jumps sub's own baseline straight to seq_no instead of trying to track anything below it -
// shared by update_reliable_ack()'s own oversized-DATA-gap branch and process_heartbeat()'s own
// oversized-Heartbeat-gap case (PLAN.md's Milestone 20 and its own Heartbeat follow-up
// respectively): an offset >= tt_RELIABLE_BITMAP_BITS can never be named in a tt_AckNackHeader.
// bitmap at all (fixed 64 bits wide on the wire), so nothing genuinely recoverable is given up on
// by not tracking it - see update_reliable_ack()'s own call site for the full "why" comment, not
// repeated here.
static void jump_ack_baseline(struct tt_Subscriber* sub, uint32_t seq_no) {
    sub->ack_seq_no = seq_no;
    sub->received_bitmap = 0;
    advance_ack_seq_no(sub);
}

// QoS roadmap #5 (RELIABILITY/RELIABLE) - called from process_data() for every DATA a reliable
// Subscriber receives (no-op otherwise). Updates the cumulative-ack watermark/out-of-order
// bitmap and, while a gap is open, keeps an ACKNACK flowing back to the sender.
static void update_reliable_ack(struct tt_Node* node, struct tt_Subscriber* sub, uint32_t seq_no,
                                uint8_t sender_node_id, uint32_t sender_ip, uint16_t sender_port) {
    if (!sub->reliable) {
        return;
    }

    sub->reliable_sender_node_id = sender_node_id;
    sub->reliable_sender_ip = sender_ip;
    sub->reliable_sender_port = sender_port;

    if (seq_no < sub->ack_seq_no) {
        return; // duplicate/old - already accounted for, e.g. a retransmit that arrived after we
                // otherwise caught up on our own
    }

    if (seq_no == sub->ack_seq_no) {
        advance_ack_seq_no(sub);
    } else {
        // NOTE: deliberately *not* fast-forwarding past a wide gap here, on every arrival that's
        // far ahead of ack_seq_no - an earlier version of this branch did, and it backfired: once
        // steady, in-order arrivals resume after ack_seq_no falls behind, each new one re-triggers
        // the same "too far ahead" check, since ack_seq_no is dragged along exactly
        // tt_MAX_RELIABLE_HISTORY - 1 behind the latest arrival forever - which preempts the very
        // retry cycle (acknack_retry()) that might have recovered ack_seq_no itself, over and
        // over, instead of ever letting it actually resolve. Bulk-skipping only belongs at
        // acknack_retry()'s own give-up point (skip_unrecoverable_backlog()) - *after* ack_seq_no
        // has had its fair tt_RELIABLE_RETRY attempts, not preempting them.
        uint64_t offset = (uint64_t)seq_no - sub->ack_seq_no;
        if (offset < tt_RELIABLE_BITMAP_BITS) {
            sub->received_bitmap |= (1ULL << offset);
        } else {
            // Unlike the "far ahead but still inside the tracking window" case this function's
            // own comment above warns against fast-forwarding on, an offset this wide (>=
            // tt_RELIABLE_BITMAP_BITS) can *never* be requested at all - tt_AckNackHeader.bitmap
            // is a fixed 64 bits wide on the wire, so no ACKNACK this Subscriber could ever send
            // has a way to name a position past bit 63 in the first place, regardless of what
            // ack_seq_no does about it. Leaving ack_seq_no untouched here (this function's own
            // behavior before PLAN.md's Milestone 20) permanently wedges it: every later arrival,
            // however perfectly in-order from this point on, has the exact same too-wide offset
            // relative to the still-stuck ack_seq_no, forever - a healthy stream never recovers.
            // Most visible for a QoS roadmap #4 (DURABILITY) backlog delivered to a brand-new
            // Subscriber whose default ack_seq_no (1) starts arbitrarily far behind a Publisher
            // that's been running a while, but applies equally to a RELIABLE-only stream that
            // takes one real burst loss wider than 64 - jump the baseline to this arrival instead,
            // the same "give up on what's provably unrecoverable, keep the stream moving" logic
            // skip_unrecoverable_backlog() already applies once retries are exhausted, just
            // applied here the instant it's already known un-trackable rather than after wasting
            // a retry cycle chasing a position that could never have been named on the wire.
            TT_LOG_WARNING("Reliable gap too large to track (%u ahead of %u) - jumping ahead instead of getting stuck",
                           seq_no - sub->ack_seq_no, sub->ack_seq_no);
            jump_ack_baseline(sub, seq_no);
        }
    }

    maybe_arm_acknack_retry(node, sub);
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
        update_entity->qos = endpoint_qos_bits(endpoint);

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

// Runs once per tt_NODE_UPDATE_INTERVAL (schedule_periodic_tasks()'s own first-run comment
// applies here too) - the timeout-based counterpart to process_update()'s content-change
// dedup: a remote node whose announce hasn't been *heard at all* (not just unchanged) for
// tt_LIVELINESS_MISS_THRESHOLD consecutive intervals is presumed gone - same forget_peers_from_
// source() peer-table cleanup a farewell UPDATE would also do, and the same update_seen[]/
// update_last_modified[] reset so a later announce from the same node id is treated as first
// contact again (reply_with_own_announce() fires, matching a genuinely new node). Its own
// discovery-cache cleanup (tombstone_discovered_entities_from_source(), unlike forget_discovered_
// entities_from_source() a farewell/dropped-from-announce uses) deliberately differs from a real
// farewell though - a liveliness timeout is a *failure*, not the normal deletion QoS roadmap #3's
// own RMW_EVENT_LIVELINESS_CHANGED.not_alive_count needs to exclude (struct tt_DiscoveredEntity.
// alive's own doc comment). Doesn't distinguish "crashed" from "network partitioned" from "just
// slow" - none of those are observable from here, and DDS-style liveliness has the same
// limitation.
static void check_liveliness(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(param);

    for (int i = 0; i < tt_MAX_ENDPOINT_COUNT; i++) {
        if (!node->update_seen[i]) {
            continue; // never heard from this node id at all - nothing to expire
        }
        if (time - node->update_last_seen[i] > (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL) {
            TT_LOG_WARNING("Node %d presumed dead (no UPDATE for %d consecutive intervals)", i,
                           tt_LIVELINESS_MISS_THRESHOLD);
            forget_peers_from_source(node, (uint8_t)i);
            tombstone_discovered_entities_from_source(node, (uint8_t)i);
            node->update_seen[i] = false;
            node->update_last_modified[i] = 0;
            node->update_last_seen[i] = 0;
        }
    }

    if (!tt_Node_schedule(node, time + tt_NODE_UPDATE_INTERVAL, check_liveliness, NULL)) {
        TT_LOG_ERROR("Cannot schedule check_liveliness");
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

// Sends every currently-retained sample (oldest first) straight to a Subscriber this Publisher's
// topic just discovered - QoS roadmap #4 (DURABILITY/TRANSIENT_LOCAL, rmw_tickle/PLAN.md). Called
// only from decode_update_entities() below when upsert_peer() just claimed a previously-empty
// peer slot for this exact Publisher - a genuinely new (or forgotten-then-rejoined) peer, not
// every periodic UPDATE refresh. No-op unless pub->durable is set (VOLATILE, today's default) and
// pub->reliable_cache is non-NULL (nothing to deliver from otherwise) - matches process_acknack()'s
// own retransmit loop exactly, just unicasting to a fixed target instead of reacting to a NACK
// bitmap, and reading from the same shared cache (struct tt_ReliableCache's own doc comment).
static void deliver_durability_backlog(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Peer* target) {
    if (!pub->durable || pub->reliable_cache == NULL) {
        return;
    }
    struct tt_ReliableCache* cache = pub->reliable_cache;
    uint16_t depth =
        (cache->depth > 0 && cache->depth <= tt_MAX_RELIABLE_HISTORY) ? cache->depth : tt_MAX_RELIABLE_HISTORY;

    // entries[] is a ring buffer tt_Publisher_publish() writes round-robin via cache->next -
    // starting the scan there and wrapping around visits oldest-to-newest in both the
    // not-yet-wrapped case (the slots from cache->next onward are still empty, len == 0, skipped
    // below) and the already-wrapped case (cache->next is exactly the oldest still-retained entry).
    for (int i = 0; i < depth; i++) {
        struct tt_ReliableCacheEntry* cache_entry = &cache->entries[(cache->next + i) % depth];
        if (cache_entry->len == 0) {
            continue;
        }
        // QoS roadmap #6 (LIFESPAN) - a backlog entry past its lifespan is skipped, "as if it had
        // never been sent" (tt_Publisher.lifespan_duration_ns's own doc comment) - a late-joining
        // durable Subscriber must not receive stale data just because it's still physically cached.
        if (reliable_cache_entry_expired(cache_entry, pub->lifespan_duration_ns)) {
            continue;
        }
        uint32_t old_tx_tail = node->tx_tail;
        void* buf = encode(node, cache_entry->len);
        if (buf == NULL) {
            TT_LOG_WARNING("Lack of tx buffer, cannot deliver durability backlog seq_no %u", cache_entry->seq_no);
            rollback(node, old_tx_tail);
            continue;
        }
        _tt_memcpy(buf, cache_entry->buffer, cache_entry->len);
        if (!end_encode(node, (struct tt_SubmessageHeader*)buf, true, target, 1)) {
            rollback(node, old_tx_tail);
        }
    }
}

// Walks the entity_count UpdateEntity records following an UpdateHeader, advancing *head past
// them. Along the way, matches each announced entity against this node's own endpoints: a
// remote TOPIC_SUBSCRIBER (resp. SERVICE_SERVER) whose endpoint_id matches one of our own
// Publishers (resp. Clients) means that Publisher/Client just learned a new (or refreshed) peer
// it can unicast to - see upsert_peer(), tt_UNICAST_PEER_THRESHOLD. A genuinely new peer for a
// DURABLE Publisher also gets deliver_durability_backlog()'d, see its own comment. Returns false
// if a type/name string fails to decode.
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
                struct tt_Publisher* pub = (struct tt_Publisher*)local;
                // QoS roadmap #1 (RxO matching, Milestone 31) - a remote Subscriber requesting a
                // policy this local Publisher doesn't offer never becomes a peer at all: no
                // unicast optimization, no durability backlog, no discovery-triggered Heartbeat -
                // matching real DDS's own "an incompatible pair simply never connects" semantics.
                // See process_data()'s own subscriber_incompatible_with_publisher() for this
                // check's own mirror image on the Subscriber side (the more consequential half,
                // since it's what actually stops broadcast DATA delivery too - this Publisher-side
                // half alone only gates the unicast-only enhancements, tickle.c's own doc comment
                // on tt_UPDATE_QOS_RELIABLE/_DURABLE explains why both halves are needed).
                bool requested_reliable = (update_entity->qos & tt_UPDATE_QOS_RELIABLE) != 0;
                bool requested_durable = (update_entity->qos & tt_UPDATE_QOS_DURABLE) != 0;
                bool incompatible = (requested_reliable && !pub->reliable) || (requested_durable && !pub->durable);
                if (!incompatible && upsert_peer(pub->peers, header->source, sender_ip, sender_port)) {
                    struct tt_Peer target = {header->source, sender_ip, sender_port};
                    deliver_durability_backlog(node, pub, &target);
                    send_initial_heartbeat(node, pub, &target);
                }
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

        // Recorded regardless of kind or whether a local endpoint matched above - discovery
        // (tt_Node_set_discovery()) lists every remote entity a node has heard of, not just ones
        // this node itself can talk to.
        upsert_discovered_entity(node, header->source, entity_id, update_entity->kind, update_entity->qos, type, name);
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

    // Liveliness (check_liveliness(), below) cares that *an* announce arrived, not whether its
    // content changed - update this on every valid announce, including the "nothing changed"
    // dedup case just below, unlike update_last_modified[] which only moves on real change.
    node->update_last_seen[source] = tt_get_ns();

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
    forget_discovered_entities_from_source(node, source);

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

// QoS roadmap #1 (RxO matching, Milestone 31) - true iff sub's own requested RELIABILITY/
// DURABILITY cannot be honored by the remote Publisher (publisher_node_id, publisher_endpoint_id)
// that just sent it DATA, per that Publisher's own last-announced tt_UpdateEntity.qos (mirrored
// into the discovery table by upsert_discovered_entity() - see struct tt_DiscoveredEntity.qos's
// own doc comment for why this reuses that table rather than a second cache). This is the
// consequential half of RxO matching: unlike decode_update_entities()'s own Publisher-side gate
// (which only withholds peer-list/backlog/Heartbeat, enhancements a broadcast-by-default Publisher
// doesn't need to reach anyone), this is what actually stops an incompatible Publisher's plain
// broadcast DATA from being delivered at all, matching real DDS's "an incompatible pair simply
// never connects" semantics instead of TickLE's previous "everything matches, degraded QoS is
// silently accepted" behavior.
//
// Fails OPEN (returns false, "compatible enough to deliver") whenever there's nothing to check
// against yet: no discovery cache attached at all (a raw TickLE-core caller that never called
// tt_Node_set_discovery() sees no behavior change from this milestone), or this Publisher hasn't
// been discovered yet (DATA arriving before its own first UPDATE announce - a narrow startup
// race, not a genuine incompatibility; giving the benefit of the doubt here is strictly better
// than dropping a legitimately compatible pair's very first samples).
static bool subscriber_incompatible_with_publisher(struct tt_Node* node, struct tt_Subscriber* sub,
                                                   uint8_t publisher_node_id, uint32_t publisher_endpoint_id) {
    if (node->discovery == NULL) {
        return false;
    }
    const struct tt_DiscoveredEntity* publisher =
        tt_Discovery_find(node->discovery, publisher_node_id, publisher_endpoint_id);
    if (publisher == NULL) {
        return false;
    }
    bool offered_reliable = (publisher->qos & tt_UPDATE_QOS_RELIABLE) != 0;
    bool offered_durable = (publisher->qos & tt_UPDATE_QOS_DURABLE) != 0;
    return (sub->reliable && !offered_reliable) || (sub->durable && !offered_durable);
}

static bool process_data(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head, uint32_t tail,
                         uint32_t sender_ip, uint16_t sender_port) {
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

    // QoS roadmap #1 (RxO matching, Milestone 31) - an incompatible Publisher's DATA is dropped
    // before any reliable-tracking side effects too (update_reliable_ack() below), not just before
    // delivery - no point generating ACKNACKs a Publisher that could never honor them will never
    // answer (see this file's own pre-Milestone-31 history of exactly that silent-degradation bug).
    if (subscriber_incompatible_with_publisher(node, sub, header->source, endpoint_id)) {
        return true;
    }

    struct tt_Topic* topic = sub->topic;
    bool is_native = tt_is_native_endian(header);

    // QoS roadmap #5 (RELIABILITY/RELIABLE) - no-op unless sub->reliable. Delivery to `callback`
    // below is unconditional either way (reliable only adds a delivery *guarantee* via
    // retransmission, not ordering - a late, retransmitted sample is still delivered whenever it
    // arrives, out of its original order).
    update_reliable_ack(node, sub, seq_no, header->source, sender_ip, sender_port);

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

// Encodes and caches a CallResponse for a request whose return_code/response are already known -
// either just computed synchronously (process_callrequest() calls this straight after a non-
// deferred tt_SERVER_CALLBACK returns), or handed to us later by flush_pending_responses() for a
// request whose callback returned tt_CALL_DEFERRED (Milestone 17, rmw_tickle/PLAN.md). Split out
// from what used to be one function (build_call_response()) precisely so the callback-invocation
// half (which decides whether an answer exists *yet* at all) stays separate from this, the
// encode-what-already-exists half - `receiver`/`response` come from a live just-decoded packet in
// the synchronous case, or from a pending slot's own stored fields in the deferred case, but this
// function itself doesn't need to know which. `old_tx_tail` is this submessage's start, for
// rolling back on a failure here.
static struct tt_SubmessageHeader* encode_call_response(struct tt_Node* node, uint8_t receiver,
                                                        struct tt_Server* server, uint16_t request_seq_no,
                                                        int8_t return_code, struct tt_Response* response,
                                                        uint32_t old_tx_tail) {
    struct tt_Service* service = server->service;

    // Header and SubmessageHeader
    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_CALLRESPONSE, receiver);
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
        int32_t cdr_len = service->response_encode_size(response);
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

        int32_t encoded_len = service->response_encode(response, cdr, (uint32_t)cdr_len);
        service->response_free(response);

        if (encoded_len < 0) {
            rollback(node, old_tx_tail);
            return NULL;
        }
    }

    // Cache submessage header before flush. set_server_cache() already logs its own reason on
    // failure, so nothing to add here.
    if (!set_server_cache(server, submessage_header, receiver)) {
        rollback(node, old_tx_tail);
        return NULL;
    }

    return submessage_header;
}

// Milestone 17 (rmw_tickle/PLAN.md): returns the index of server's own pending slot matching
// (receiver, seq_no), or -1 if none. Matches a slot in *either* tt_SERVER_SLOT_PENDING or
// tt_SERVER_SLOT_READY - a retry that arrives after tt_Server_send_response() already ran but
// before the poll thread has flushed it out still shouldn't re-invoke the callback a second time.
// Only pending_request_id[] itself needs no atomic care to read here (only ever written by the
// poll thread, in defer_call_response() below - tt_Server_send_response() never touches it) - the
// slot_state[] load guarding it does, since that field is also written from another thread.
static int find_pending_slot(struct tt_Server* server, uint8_t receiver, uint16_t seq_no) {
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (__atomic_load_n(&server->slot_state[i], __ATOMIC_ACQUIRE) == tt_SERVER_SLOT_EMPTY) {
            continue;
        }
        if (server->pending_request_id[i].receiver == receiver && server->pending_request_id[i].seq_no == seq_no) {
            return i;
        }
    }
    return -1;
}

// Timer callback (tt_Node_schedule(), Milestone 17): if a deferred request still hasn't been
// answered by the time it fires, reclaim its slot rather than let it leak forever.
// tt_Server_send_response()'s own compare-exchange against tt_SERVER_SLOT_PENDING loses cleanly
// if it races against this, exactly like server_cache_clean() already reclaims an *answered*
// slot's own retry-cache lifetime.
static void pending_response_timeout(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(node);
    UNUSED(time);

    struct server_cache_clean_config* config = param;
    struct tt_Server* server = config->server;
    int slot = config->slot;

    uint8_t expected = tt_SERVER_SLOT_PENDING;
    if (__atomic_compare_exchange_n(&server->slot_state[slot], &expected, tt_SERVER_SLOT_EMPTY, false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
        TT_LOG_WARNING("Deferred service response for seq_no %d timed out, giving up",
                       server->pending_request_id[slot].seq_no);
    }
    // else: tt_Server_send_response() already claimed this slot (now READY) - nothing to reclaim.
    server->pending_timeout_scheduled[slot] = false;
}

// Allocates a pending slot for a request whose callback just returned tt_CALL_DEFERRED, so
// tt_Server_send_response() has somewhere to find it later. Only ever called from the poll thread
// (inside process_callrequest()), so slot *allocation* itself needs no atomics - only the final
// publish (the slot_state[] store that makes this slot visible to tt_Server_send_response() on
// another thread) does.
static bool defer_call_response(struct tt_Server* server, tt_RequestId request_id, uint32_t sender_ip,
                                uint16_t sender_port) {
    int slot = -1;
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (__atomic_load_n(&server->slot_state[i], __ATOMIC_RELAXED) == tt_SERVER_SLOT_EMPTY) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        TT_LOG_ERROR("Out of server slots for a deferred response");
        return false;
    }

    server->pending_request_id[slot] = request_id;
    server->pending_sender_ip[slot] = sender_ip;
    server->pending_sender_port[slot] = sender_port;
    server->pending_timeout_config[slot].server = server;
    server->pending_timeout_config[slot].slot = slot;

    // Only publish once the timeout is guaranteed to run - same "don't publish an entry with no
    // way to reclaim it" reasoning set_server_cache() already follows for its own timer.
    if (!tt_Node_schedule(server->node, tt_get_ns() + tt_SERVER_DEFERRED_RESPONSE_TIMEOUT, pending_response_timeout,
                          &server->pending_timeout_config[slot])) {
        TT_LOG_ERROR("Cannot schedule pending_response_timeout");
        return false;
    }
    server->pending_timeout_scheduled[slot] = true;

    // Release: everything written above must be visible to tt_Server_send_response() (another
    // thread) once it observes this store via its own acquire load - the standard C11
    // release/acquire handoff.
    __atomic_store_n(&server->slot_state[slot], tt_SERVER_SLOT_PENDING, __ATOMIC_RELEASE);
    return true;
}

tt_ret_t tt_Server_send_response(struct tt_Server* server, tt_RequestId request_id, int8_t return_code,
                                 struct tt_Response* response) {
    if (server == NULL || response == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (__atomic_load_n(&server->slot_state[i], __ATOMIC_ACQUIRE) != tt_SERVER_SLOT_PENDING) {
            continue;
        }
        if (server->pending_request_id[i].receiver != request_id.receiver ||
            server->pending_request_id[i].seq_no != request_id.seq_no) {
            continue;
        }

        // request_id is unique among outstanding requests, so this is the only slot that could
        // ever match - safe to memcpy before the compare-exchange below claims it, since only the
        // poll thread (pending_response_timeout()) could otherwise touch this slot concurrently,
        // and it only ever *reclaims* (PENDING -> EMPTY), never overwrites pending_response_buf.
        _tt_memcpy(server->pending_response_buf[i], response, server->service->response_size);
        server->pending_return_code[i] = return_code;

        uint8_t expected = tt_SERVER_SLOT_PENDING;
        if (!__atomic_compare_exchange_n(&server->slot_state[i], &expected, tt_SERVER_SLOT_READY, false,
                                         __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            // Reclaimed (timed out) between the load above and now - too late.
            return tt_RET_NOT_FOUND;
        }

        // Wake the poll thread promptly instead of leaving this READY slot waiting out
        // tt_Node_poll()'s own normal receive timeout - same primitive and reasoning
        // tt_Node_interrupt() already exists for (Phase 0).
        tt_Node_interrupt(server->node);
        return tt_RET_OK;
    }

    return tt_RET_NOT_FOUND;
}

// Encodes and sends every response tt_Server_send_response() has queued (slot_state[] ==
// tt_SERVER_SLOT_READY) since the last time this ran, for every service server this node owns.
// The actual CDR encode + node->tx_buffer write this library's single-thread-owns-tx_buffer
// invariant (DESIGN.md's "Concurrency") requires happen here, on whichever thread is driving this
// node's own tt_Node_poll() loop - never on whatever thread called tt_Server_send_response()
// itself (see that function's own doc comment in tickle.h). Runs once at the top of every
// tt_Node_poll() call, the same "drain everything already ready" spirit drain_rx() already has
// for received datagrams.
static void flush_pending_responses(struct tt_Node* node) {
    for (uint32_t ep = 0; ep < node->endpoint_count; ep++) {
        struct tt_Endpoint* endpoint = node->endpoints[ep];
        if (endpoint->kind != tt_KIND_SERVICE_SERVER) {
            continue;
        }
        struct tt_Server* server = (struct tt_Server*)endpoint;

        for (int slot = 0; slot < tt_MAX_SERVER_CACHE_COUNT; slot++) {
            if (__atomic_load_n(&server->slot_state[slot], __ATOMIC_ACQUIRE) != tt_SERVER_SLOT_READY) {
                continue;
            }

            tt_RequestId request_id = server->pending_request_id[slot];
            uint32_t sender_ip = server->pending_sender_ip[slot];
            uint16_t sender_port = server->pending_sender_port[slot];
            int8_t return_code = server->pending_return_code[slot];

            uint32_t old_tx_tail = node->tx_tail;
            struct tt_SubmessageHeader* submessage_header =
                encode_call_response(node, request_id.receiver, server, request_id.seq_no, return_code,
                                     (struct tt_Response*)server->pending_response_buf[slot], old_tx_tail);

            // Reclaim the slot regardless of encode success - a failure here is already logged by
            // encode_call_response() itself, and retrying it from this same stale slot on the
            // next poll() call would just fail identically forever.
            __atomic_store_n(&server->slot_state[slot], tt_SERVER_SLOT_EMPTY, __ATOMIC_RELAXED);

            if (submessage_header == NULL) {
                continue;
            }

            // Same unicast-when-possible optimization process_callrequest() already applies to a
            // synchronous response - see its own comment for the full reasoning (only safe when
            // tx_buffer was otherwise empty before this one response).
            struct tt_Peer sender_peer = {request_id.receiver, sender_ip, sender_port};
            bool unicast_to_sender = old_tx_tail == sizeof(struct tt_Header);
            if (!end_encode(node, submessage_header, true, unicast_to_sender ? &sender_peer : NULL,
                            unicast_to_sender ? 1 : 0)) {
                rollback(node, old_tx_tail);
            }
        }
    }
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

    // Milestone 17 (rmw_tickle/PLAN.md): a retry for a request already deferred (callback
    // returned tt_CALL_DEFERRED, no answer computed yet) must not re-invoke the callback a second
    // time - the real answer is already on its way, whenever tt_Server_send_response() gets
    // called. Checked before the cache lookup below since a deferred-then-answered request only
    // ever gets *cached* once flush_pending_responses() has actually sent it.
    if (find_pending_slot(server, header->source, seq_no) >= 0) {
        TT_LOG_DEBUG("CallRequest retry for a still-deferred response, ignoring");
        return true;
    }

    // Check cache
    struct tt_SubmessageHeader* cached = get_server_cache(server, header->source, seq_no);
    uint32_t old_tx_tail = node->tx_tail;

    struct tt_SubmessageHeader* submessage_header;
    if (cached != NULL) {
        submessage_header = resend_cached_response(node, cached);
        if (submessage_header == NULL) {
            return false;
        }
    } else {
        struct tt_Service* service = server->service;

        uint8_t request[service->request_size];
        int32_t decoded = service->request_decode((struct tt_Request*)request, buffer + head, tail - head,
                                                  tt_is_native_endian(header));
        if (decoded < 0) {
            TT_LOG_ERROR("Cannot decode request: %d", decoded);
            return false;
        }

        uint8_t response[service->response_size];
        tt_RequestId request_id = {header->source, seq_no};
        int8_t return_code =
            server->callback(server, (struct tt_Request*)request, (struct tt_Response*)response, request_id);
        service->request_free((struct tt_Request*)request);

        TT_LOG_DEBUG("  return_code: %d", return_code);

        if (return_code == tt_CALL_DEFERRED) {
            // Nothing to encode/send yet - tt_Server_send_response() (any thread, any time up to
            // tt_SERVER_DEFERRED_RESPONSE_TIMEOUT from now) and flush_pending_responses() (the
            // poll thread, once that call happens) do the rest.
            return defer_call_response(server, request_id, sender_ip, sender_port);
        }

        submessage_header = encode_call_response(node, header->source, server, seq_no, return_code,
                                                 (struct tt_Response*)response, old_tx_tail);
        if (submessage_header == NULL) {
            return false;
        }
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

// QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - a reliable Subscriber's ACKNACK
// arrives here at whichever local Publisher it targets. Retransmits, straight back to the
// sender, whichever requested (bitmap bit set) samples are still in that Publisher's own
// reliable_cache; a sample already evicted (KEEP_LAST) or already retried past tt_RELIABLE_RETRY
// is silently skipped - the Subscriber's own acknack_retry() gives up on its side independently.
// Finds the still-resendable cache entry for missing_seq_no, or NULL if there isn't one - either
// nothing in cache matches it, the retry cap (tt_RELIABLE_RETRY) is already exhausted, or (QoS
// roadmap #6, LIFESPAN) it's aged out of pub->lifespan_duration_ns. Split out of process_acknack()
// below purely to keep that function's own cognitive complexity under clang-tidy's threshold, same
// reasoning cache_reliable_sample()/reliable_cache_oldest_seq_no() were split out for.
static struct tt_ReliableCacheEntry* find_resendable_cache_entry(struct tt_ReliableCache* cache, uint16_t depth,
                                                                 uint32_t missing_seq_no,
                                                                 uint64_t lifespan_duration_ns) {
    for (int i = 0; i < depth; i++) {
        struct tt_ReliableCacheEntry* cache_entry = &cache->entries[i];
        if (cache_entry->len == 0 || cache_entry->seq_no != missing_seq_no) {
            continue;
        }
        if (cache_entry->retry >= tt_RELIABLE_RETRY) {
            return NULL; // give up on this one sample - the Subscriber's own retry cap will too
        }
        // Same "as if it had never been sent" rule deliver_durability_backlog() applies, here for
        // a live NACK'd retransmit instead of a discovery-triggered backlog push. Matches real
        // DDS: LIFESPAN removes data from the Writer's history outright, RELIABLE's own retry
        // guarantee doesn't override it.
        if (reliable_cache_entry_expired(cache_entry, lifespan_duration_ns)) {
            return NULL;
        }
        return cache_entry;
    }
    return NULL;
}

static bool process_acknack(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                            uint32_t tail, uint32_t sender_ip, uint16_t sender_port) {
    struct tt_AckNackHeader* acknack_header = decode(node, buffer, &head, tail, sizeof(struct tt_AckNackHeader));
    if (acknack_header == NULL) {
        TT_LOG_ERROR("Illegal AckNackHeader");
        return false;
    }

    uint32_t endpoint_id = rd32(header, acknack_header->endpoint_id);
    uint32_t seq_no = rd32(header, acknack_header->seq_no);
    uint64_t bitmap = rd64(header, acknack_header->bitmap);

    TT_LOG_DEBUG("AckNack");
    TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
    TT_LOG_DEBUG("  seq_no: %u", seq_no);

    struct tt_Endpoint* endpoint = find_endpoint(node, tt_KIND_TOPIC_PUBLISHER, endpoint_id);
    if (endpoint == NULL) {
        return true;
    }

    struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
    if (pub->reliable_cache == NULL) {
        return true; // not a reliable Publisher (or a stale ack) - nothing cached to resend
    }

    struct tt_ReliableCache* cache = pub->reliable_cache;
    uint16_t depth =
        (cache->depth > 0 && cache->depth <= tt_MAX_RELIABLE_HISTORY) ? cache->depth : tt_MAX_RELIABLE_HISTORY;
    struct tt_Peer target = {header->source, sender_ip, sender_port};

    for (int bit = 0; bit < tt_RELIABLE_BITMAP_BITS; bit++) {
        if (!(bitmap & (1ULL << bit))) {
            continue;
        }
        uint32_t missing_seq_no = seq_no + (uint32_t)bit;

        struct tt_ReliableCacheEntry* cache_entry =
            find_resendable_cache_entry(cache, depth, missing_seq_no, pub->lifespan_duration_ns);
        if (cache_entry == NULL) {
            continue;
        }

        uint32_t old_tx_tail = node->tx_tail;
        void* buf = encode(node, cache_entry->len);
        if (buf == NULL) {
            TT_LOG_WARNING("Lack of tx buffer, cannot retransmit seq_no %u now", missing_seq_no);
            rollback(node, old_tx_tail);
            continue;
        }
        _tt_memcpy(buf, cache_entry->buffer, cache_entry->len);
        if (!end_encode(node, (struct tt_SubmessageHeader*)buf, true, &target, 1)) {
            rollback(node, old_tx_tail);
        } else {
            cache_entry->retry++;
        }
    }

    return true;
}

// QoS roadmap #5 (RELIABILITY) follow-up - process_submessage()'s own new HEARTBEAT case. See
// struct tt_HeartbeatHeader's own doc comment (tickle.h) for what this is; matches process_data()/
// process_acknack()'s own decode-then-dispatch shape and "silently no-op if nothing local
// matches" convention. Returns false only on a genuine decode failure (illegal header) - a
// Heartbeat with no local match, or for a non-reliable Subscriber, is a normal no-op, not an
// error, same as those two functions' own equivalent cases.
static bool process_heartbeat(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                              uint32_t tail, uint32_t sender_ip, uint16_t sender_port) {
    struct tt_HeartbeatHeader* heartbeat_header = decode(node, buffer, &head, tail, sizeof(struct tt_HeartbeatHeader));
    if (heartbeat_header == NULL) {
        TT_LOG_ERROR("Illegal HeartbeatHeader");
        return false;
    }

    uint32_t endpoint_id = rd32(header, heartbeat_header->endpoint_id);
    uint32_t first_available_seq_no = rd32(header, heartbeat_header->first_available_seq_no);
    uint32_t last_seq_no = rd32(header, heartbeat_header->last_seq_no);

    TT_LOG_DEBUG("Heartbeat");
    TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
    TT_LOG_DEBUG("  first_available_seq_no: %u", first_available_seq_no);
    TT_LOG_DEBUG("  last_seq_no: %u", last_seq_no);

    struct tt_Endpoint* endpoint = find_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id);
    if (endpoint == NULL) {
        return true;
    }

    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
    if (!sub->reliable) {
        return true; // a best-effort Subscriber has no ack state a Heartbeat could inform
    }

    if (sub->reliable_sender_node_id == tt_NODE_ID_INVALID) {
        // First-ever reliable contact from this sender: learn the *real* starting baseline
        // straight from the Heartbeat, rather than guessing it from whatever DATA happens to
        // arrive first (PLAN.md's Milestone 20's own workaround for not having this signal at
        // all) - the actual DDS-parity fix this whole follow-up is for. Matches send_acknack()'s
        // own tt_NODE_ID_INVALID sentinel check for "no reliable contact yet".
        sub->ack_seq_no = first_available_seq_no;
        sub->received_bitmap = 0;
    } else if (last_seq_no >= sub->ack_seq_no) {
        uint64_t offset = (uint64_t)last_seq_no - sub->ack_seq_no;
        if (offset >= tt_RELIABLE_BITMAP_BITS) {
            // Already-tracking Subscriber, but this Heartbeat reveals a gap too wide to ever
            // track - the same "provably unrecoverable, don't get stuck" case update_reliable_
            // ack()'s own oversized-DATA-gap branch handles (see jump_ack_baseline()'s own doc
            // comment) - jump ahead here too, rather than only ever being able to discover this
            // reactively once *some* DATA sample eventually arrives to trigger update_reliable_
            // ack() instead.
            jump_ack_baseline(sub, last_seq_no);
        }
        // else: within the trackable window - nothing to do here directly. highest_relevant_bit()
        // already picks this up from reliable_heartbeat_last_seq_no (set unconditionally below),
        // and maybe_arm_acknack_retry()/send_acknack() below act on it.
    }
    // last_seq_no < sub->ack_seq_no: a stale/reordered Heartbeat (e.g. arrived after DATA already
    // caught this Subscriber up further) - nothing to do, same "duplicate/old" no-op update_
    // reliable_ack()'s own seq_no < ack_seq_no branch already has.

    sub->reliable_sender_node_id = header->source;
    sub->reliable_sender_ip = sender_ip;
    sub->reliable_sender_port = sender_port;
    // Monotonic guard: a Heartbeat can arrive out of order over UDP the same as any other
    // submessage - never let a late, older one regress what highest_relevant_bit() already knows.
    if (last_seq_no > sub->reliable_heartbeat_last_seq_no) {
        sub->reliable_heartbeat_last_seq_no = last_seq_no;
    }

    maybe_arm_acknack_retry(node, sub);
    return true;
}

static bool process_submessage(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                               uint32_t body_tail, const struct tt_SubmessageHeader* submessage_header,
                               uint32_t sender_ip, uint16_t sender_port, bool self_sent) {
    // Each process_X() below already logs its own specific reason on failure, so this switch
    // doesn't log again on top of that - only the type dispatch itself gets a message here.
    // sender_ip/sender_port (this packet's own source, from tt_receive() - see
    // handle_receive_result()) reach process_callrequest() (to unicast the CallResponse straight
    // back), process_update() (to learn/refresh a peer table entry - see decode_update_
    // entities()'s own comment), process_data() (to remember where a reliable Subscriber's own
    // ACKNACK should go, QoS roadmap #5), and process_acknack() (to unicast a retransmit straight
    // back the same way CallResponse does); process_callresponse() doesn't need them.
    //
    // self_sent (this whole packet's own header->source == node->id) only suppresses the two
    // topic-shaped types below, not CALLREQUEST/CALLRESPONSE - rmw_tickle/PLAN.md's own Milestone
    // 17 finding: a client and its service can end up on the exact same tt_Node (the only
    // topology rmw_tickle's one-node-per-process model allows), and RPC has no separate in-
    // process delivery path the way "a node already has its own published data locally" is true
    // for pub/sub - the request/response datagrams *are* the only path, so suppressing them here
    // made a co-located client structurally unable to ever reach its own service.
    switch (submessage_header->type) {
    case tt_SUBMESSAGE_TYPE_UPDATE:
        if (!self_sent) {
            process_update(node, header, buffer, head, body_tail, sender_ip, sender_port);
        }
        return true;
    case tt_SUBMESSAGE_TYPE_DATA:
        if (!self_sent) {
            process_data(node, header, buffer, head, body_tail, sender_ip, sender_port);
        }
        return true;
    case tt_SUBMESSAGE_TYPE_ACKNACK:
        // QoS roadmap #5 (RELIABILITY/RELIABLE) - self_sent-guarded for the same reason as DATA/
        // UPDATE above: a reliable Subscriber never sees its own co-located Publisher's DATA in
        // the first place (self_sent-suppressed), so it never has anything to ack locally either.
        if (!self_sent) {
            process_acknack(node, header, buffer, head, body_tail, sender_ip, sender_port);
        }
        return true;
    case tt_SUBMESSAGE_TYPE_HEARTBEAT:
        // QoS roadmap #5 (RELIABILITY) follow-up - self_sent-guarded for the identical reason
        // ACKNACK's own case just above is.
        if (!self_sent) {
            process_heartbeat(node, header, buffer, head, body_tail, sender_ip, sender_port);
        }
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
                                                          uint32_t sender_ip, uint16_t sender_port, bool self_sent) {
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
        !process_submessage(node, header, buffer, *head, body_tail, submessage_header, sender_ip, sender_port,
                            self_sent)) {
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

    // Self sent message - no longer short-circuited here: see process_submessage()'s own comment
    // on why this now only suppresses the topic-shaped types (UPDATE/DATA), not CALLREQUEST/
    // CALLRESPONSE, and so has to be threaded down per-submessage rather than dropping the whole
    // packet up front.
    bool self_sent = header->source == node->id;
    TT_LOG_DEBUG("source: %d%s", header->source, self_sent ? " (self)" : "");

    while (true) {
        enum submessage_walk_result result =
            process_one_submessage(node, header, buffer, &head, tail, sender_ip, sender_port, self_sent);
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

    if (len == -3) { // tt_Node_interrupt() - always ends the poll, even if woke_for_scheduler:
                     // an explicit interrupt request must never be swallowed the way a plain
                     // short wait for a due scheduler entry is, or the caller that asked to be
                     // woken (tt_Node_interrupt()'s own caller, on another thread) could end up
                     // waiting for however much longer the scheduler-driven work takes instead.
        *result = tt_RET_INTERRUPTED;
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

    // Milestone 17 (rmw_tickle/PLAN.md): send whatever tt_Server_send_response() queued since the
    // last call, before doing anything else this call - same "drain what's ready first" spirit as
    // the scheduler/RX handling below, and importantly *before* this call might otherwise block in
    // tt_receive() for up to `timeout` with a real response already sitting there ready to go out.
    flush_pending_responses(node);

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

tt_ret_t tt_Node_interrupt(struct tt_Node* node) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    return tt_wake_signal(node);
}

tt_ret_t tt_Node_set_discovery(struct tt_Node* node, struct tt_Discovery* discovery, tt_DISCOVERY_CALLBACK callback,
                               void* param) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    node->discovery = discovery;
    node->discovery_callback = discovery != NULL ? callback : NULL;
    node->discovery_callback_param = discovery != NULL ? param : NULL;
    return tt_RET_OK;
}

uint32_t tt_Discovery_count(const struct tt_Discovery* discovery) {
    if (discovery == NULL) {
        return 0;
    }
    uint32_t count = 0;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (discovery->entities[i].node_id != tt_NODE_ID_INVALID && discovery->entities[i].alive) {
            count++;
        }
    }
    return count;
}

const struct tt_DiscoveredEntity* tt_Discovery_find(const struct tt_Discovery* discovery, uint8_t node_id,
                                                    uint32_t endpoint_id) {
    if (discovery == NULL) {
        return NULL;
    }
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (discovery->entities[i].node_id == node_id && discovery->entities[i].endpoint_id == endpoint_id) {
            return &discovery->entities[i];
        }
    }
    return NULL;
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
