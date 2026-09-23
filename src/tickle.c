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
// The default Publisher cache depth must fit the Subscriber's tt_RELIABLE_BITMAP_BITS-wide
// tracking window (struct tt_WriterProxy.received_bitmap): a gap further back than the window can
// never be named in an ACKNACK, so retaining more than that by default buys no recovery (Phase 1-c
// removed the other user of this constant, skip_unrecoverable_backlog()'s compile-time depth guess;
// rmw_tickle/PLAN.md's Phase 3 KEEP_ALL rule - unacked samples <= the window - rests on the same
// bound).
_Static_assert(tt_MAX_RELIABLE_HISTORY <= tt_RELIABLE_BITMAP_BITS,
               "tt_MAX_RELIABLE_HISTORY must fit within the reliable ACKNACK bitmap window");
// B1 - tt_RELIABLE_CACHE_ARENA_BYTES()'s callers size an arena from tt_RELIABLE_RECORD_BYTES(),
// which spells out the framing overhead (4 + 20) rather than including tickle.h; keep the two in
// step with what cache_reliable_sample() actually stores.
_Static_assert(tt_RELIABLE_RECORD_BYTES(0) ==
                   ROUNDUP(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader)),
               "tt_RELIABLE_RECORD_BYTES must match the real DATA submessage framing size");

// RELIABLE recovery instrumentation - see include/tickle/reliable_stats.h. Everything below is
// compiled out (RSTAT_* expand to nothing) unless built with -Dtt_RELIABLE_STATS.
#ifdef tt_RELIABLE_STATS
#include <tickle/reliable_stats.h>

static struct tt_ReliableStats g_rstats;

// Per-seq_no timestamps for the two latency histograms, indexed by seq_no % RSTAT_SEQ_SLOTS -
// process-global like g_rstats, so only meaningful with a single reliable writer per process.
// 4096 slots covers several full tt_RELIABLE_BITMAP_BITS windows, well past any seq_no still
// trackable when it's recovered; a stale slot is overwritten on the next detection anyway.
#define RSTAT_SEQ_SLOTS 4096
static uint64_t g_rstats_missing_since_ns[RSTAT_SEQ_SLOTS]; // 0 = not currently missing
static uint64_t g_rstats_requested_ns[RSTAT_SEQ_SLOTS];     // 0 = not yet named in an ACKNACK

#define RSTAT_INC(field) (g_rstats.field++)
#define RSTAT_ADD(field, n) (g_rstats.field += (uint64_t)(n))

void tt_reliable_stats_get(struct tt_ReliableStats* out) {
    *out = g_rstats;
}

void tt_reliable_stats_reset(void) {
    memset(&g_rstats, 0, sizeof(g_rstats));
    memset(g_rstats_missing_since_ns, 0, sizeof(g_rstats_missing_since_ns));
    memset(g_rstats_requested_ns, 0, sizeof(g_rstats_requested_ns));
}

static void rstat_hist(uint64_t* hist, uint64_t delta_ns) {
    uint64_t micros = delta_ns / tt_MICROSECOND;
    int bucket = 0;
    while (micros > 0 && bucket < tt_RELIABLE_STATS_HIST_BUCKETS - 1) {
        micros >>= 1;
        bucket++;
    }
    hist[bucket]++;
}

static void rstat_mark_missing(uint32_t first_seq_no, uint32_t count, uint64_t now) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (first_seq_no + i) % RSTAT_SEQ_SLOTS;
        g_rstats_missing_since_ns[slot] = now;
        g_rstats_requested_ns[slot] = 0;
    }
}

static void rstat_recovered(uint32_t seq_no) {
    uint32_t slot = seq_no % RSTAT_SEQ_SLOTS;
    uint64_t now = tt_get_ns();
    g_rstats.recovered++;
    if (g_rstats_missing_since_ns[slot] != 0) {
        rstat_hist(g_rstats.detect_to_recover_hist, now - g_rstats_missing_since_ns[slot]);
    }
    if (g_rstats_requested_ns[slot] != 0) {
        g_rstats.recovered_after_request++;
        rstat_hist(g_rstats.request_to_recover_hist, now - g_rstats_requested_ns[slot]);
    }
    g_rstats_missing_since_ns[slot] = 0;
    g_rstats_requested_ns[slot] = 0;
}

static uint32_t rstat_popcount_bitmap(const uint64_t* bitmap, uint16_t words) {
    uint32_t count = 0;
    for (uint16_t word = 0; word < words; word++) {
        count += (uint32_t)__builtin_popcountll(bitmap[word]);
    }
    return count;
}
#else
#define RSTAT_INC(field) ((void)0)
#define RSTAT_ADD(field, n) ((void)0)
#endif

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

#ifdef tt_RELIABLE_STATS
    {
        uint64_t data_count = 0;
        uint32_t pos = sizeof(struct tt_Header);
        while (pos + sizeof(struct tt_SubmessageHeader) <= len) {
            const struct tt_SubmessageHeader* submsg = (const struct tt_SubmessageHeader*)(node->tx_buffer + pos);
            if (submsg->length == 0) {
                break;
            }
            if (submsg->type == tt_SUBMESSAGE_TYPE_DATA) {
                data_count++;
            }
            pos += submsg->length;
        }
        g_rstats.datagrams++;
        if (data_count > 0) {
            g_rstats.datagrams_with_data++;
            g_rstats.data_in_datagrams += data_count;
            if (data_count > g_rstats.max_data_per_datagram) {
                g_rstats.max_data_per_datagram = data_count;
            }
        }
    }
#endif

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

// Milestone 35 (rmw_tickle/PLAN.md) - more than one local endpoint may now share a (kind, id)
// pair (add_endpoint_to_node() no longer rejects it, see its own doc comment), so this returns
// only the FIRST one found by probe order, not necessarily "the" one a caller might have meant.
// Probe order is deterministic (rebuild_endpoint_index() always walks endpoints[] in registration
// order, oldest first, since removal compacts rather than leaving tombstones) - so "first found"
// concretely means "the oldest still-registered match" every time, not an arbitrary pick that
// could vary run to run. Callers that route a reply/retransmission to one specific instance
// (process_callrequest(), process_callresponse(), process_acknack()) deliberately keep this
// single-match behavior - the wire protocol has no per-instance id beyond the name hash, so
// picking a specific one of several identically-named local endpoints is inherently ambiguous at
// the wire level regardless of which local data structure look it up (matches real DDS's own
// undefined-which-one semantics for redundant same-name entities). Callers whose own correctness
// depends on reaching *every* local match instead - most importantly process_data()'s own
// Subscriber delivery, so N local Subscriptions on one topic each get every sample - use
// for_each_endpoint() below instead.
static struct tt_Endpoint* find_endpoint(struct tt_Node* node, uint8_t kind, uint32_t endpoint_id) {
    if (!node->endpoint_index_valid) {
        rebuild_endpoint_index(node);
    }

    // Linear probe from the id's home slot; a NULL slot means "not present" (load is kept <= 0.5,
    // so the probe is short). Distinct endpoints sharing an id (different kind, or now the same
    // kind too) just land in adjacent slots and the kind check below picks the right one(s).
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

// Milestone 47 - like find_endpoint() above, but among every local endpoint matching (kind,
// endpoint_id), prefers the one whose own entity_id matches - falling back to the plain first
// match find_endpoint() would have returned when entity_id is 0 (unknown - an older-style call
// site, or a sender that hasn't learned the real target entity_id yet) or none of the matches
// carry it. Closes Milestone 35's own previously-accepted "first match" ambiguity for whichever
// submessage type actually carries a *target* entity_id (struct tt_AckNackHeader, process_
// acknack()) - DATA/HEARTBEAT identify their own *sender* instead (struct tt_Endpoint.entity_id's
// own doc comment) and fan out to every local match via for_each_endpoint(), so they don't need
// this narrowing at all.
static struct tt_Endpoint* find_endpoint_by_entity(struct tt_Node* node, uint8_t kind, uint32_t endpoint_id,
                                                   uint32_t entity_id) {
    if (!node->endpoint_index_valid) {
        rebuild_endpoint_index(node);
    }

    struct tt_Endpoint* first_match = NULL;
    uint32_t slot = endpoint_id & (tt_ENDPOINT_INDEX_SIZE - 1);
    for (uint32_t probe = 0; probe < tt_ENDPOINT_INDEX_SIZE; probe++) {
        struct tt_Endpoint* endpoint = node->endpoint_index[slot];
        if (endpoint == NULL) {
            break;
        }
        if (endpoint->kind == kind && endpoint->id == endpoint_id) {
            if (entity_id != 0 && endpoint->entity_id == entity_id) {
                return endpoint;
            }
            if (first_match == NULL) {
                first_match = endpoint;
            }
        }
        slot = (slot + 1) & (tt_ENDPOINT_INDEX_SIZE - 1);
    }
    return first_match;
}

// Milestone 35 - calls visit(node, endpoint, ctx) once for every local endpoint matching (kind,
// endpoint_id), not just the first like find_endpoint() above. The same linear-probe walk,
// continued past a match instead of returning immediately: correct because rebuild_endpoint_
// index() always rebuilds from scratch (no tombstones from removal), so a probe chain never has
// a gap that could hide a later match sharing the same home slot. Used by callers where reaching
// every local instance is the actual correctness requirement, not an implementation convenience -
// see find_endpoint()'s own doc comment for which callers use which, and why.
static void for_each_endpoint(struct tt_Node* node, uint8_t kind, uint32_t endpoint_id,
                              void (*visit)(struct tt_Node* node, struct tt_Endpoint* endpoint, void* ctx), void* ctx) {
    if (!node->endpoint_index_valid) {
        rebuild_endpoint_index(node);
    }

    uint32_t slot = endpoint_id & (tt_ENDPOINT_INDEX_SIZE - 1);
    for (uint32_t probe = 0; probe < tt_ENDPOINT_INDEX_SIZE; probe++) {
        struct tt_Endpoint* endpoint = node->endpoint_index[slot];
        if (endpoint == NULL) {
            return;
        }
        if (endpoint->kind == kind && endpoint->id == endpoint_id) {
            visit(node, endpoint, ctx);
        }
        slot = (slot + 1) & (tt_ENDPOINT_INDEX_SIZE - 1);
    }
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

// One remote Subscriber entity's own entry in pub->peer_acks[] (keyed by (node_id, entity_id) -
// see that field's own doc comment, tickle.h), or NULL if this Publisher isn't tracking it.
static struct tt_PeerAck* find_peer_ack(struct tt_Publisher* pub, uint8_t node_id, uint32_t entity_id) {
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].node_id == node_id && pub->peer_acks[i].entity_id == entity_id) {
            return &pub->peer_acks[i];
        }
    }
    return NULL;
}

// Claims (or finds) the ack entry for one matched Subscriber entity, at match time rather than on
// its first ACKNACK: a matched-but-still-silent Subscriber must already count as "hasn't acked
// anything", or a KEEP_ALL writer would unblock without it (Phase 2/(b)). NULL when the table is
// full, which register_subscriber_peer_on_publisher() turns into "don't match at all" rather than
// matching a Subscriber whose acks can never be counted.
static struct tt_PeerAck* claim_peer_ack(struct tt_Publisher* pub, uint8_t node_id, uint32_t entity_id) {
    struct tt_PeerAck* ack = find_peer_ack(pub, node_id, entity_id);
    if (ack != NULL) {
        return ack;
    }
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].node_id == tt_NODE_ID_INVALID) {
            pub->peer_acks[i].node_id = node_id;
            pub->peer_acks[i].entity_id = entity_id;
            pub->peer_acks[i].ack_seq_no = 0;
            pub->peer_acks[i].tracking_words = 0; // set by the caller from the announce
            return &pub->peer_acks[i];
        }
    }
    return NULL;
}

// Drops ack state outright - a real departure (farewell UPDATE, liveliness timeout, or an announce
// that no longer lists a matching Subscriber), not process_update()'s own transient
// forget-then-re-add (Phase 3 prerequisite (c): that one must preserve it). entity_id 0 with
// match_any_entity drops every entity that node hosts (a whole node departing); otherwise just the
// one named entity (a single Subscriber's own lease expiring while its node stays up).
static void forget_peer_ack(struct tt_Publisher* pub, uint8_t node_id, uint32_t entity_id, bool match_any_entity) {
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].node_id != node_id) {
            continue;
        }
        if (!match_any_entity && pub->peer_acks[i].entity_id != entity_id) {
            continue;
        }
        pub->peer_acks[i].node_id = tt_NODE_ID_INVALID;
        pub->peer_acks[i].entity_id = 0;
        pub->peer_acks[i].ack_seq_no = 0;
    }
}

// Advances one Subscriber entity's own ack watermark. Only ever advances - a stale/reordered
// ACKNACK carrying a smaller seq_no must not regress it (UDP gives no ordering guarantee between
// two ACKNACKs from the same sender). An ACKNACK from an entity this Publisher isn't tracking
// (never matched, already departed, or sender_entity_id 0/unknown) still routes and retransmits,
// it just isn't counted as an ack - the conservative direction for KEEP_ALL.
static void record_peer_ack(struct tt_Publisher* pub, uint8_t node_id, uint32_t entity_id, uint32_t seq_no) {
    if (entity_id == 0) {
        return;
    }
    struct tt_PeerAck* ack = find_peer_ack(pub, node_id, entity_id);
    if (ack == NULL) {
        return;
    }
    if (seq_no > ack->ack_seq_no) {
        ack->ack_seq_no = seq_no;
    }
}

// forget_peer()'s own Publisher-specific counterpart - a separate function rather than teaching
// forget_peer() itself about peer_acks[], since that table only exists on struct tt_Publisher
// (tt_Client's own identically-shaped peers[] has no ack-aggregation concept to reset).
//
// preserve_ack distinguishes the two callers (Phase 3 prerequisite (c), rmw_tickle/PLAN.md): a
// fresh announce from a still-present node forgets its peer slots only so decode_update_entities()
// can re-add whatever it still lists, and its ack state must survive that round trip untouched -
// Phase 3's KEEP_ALL blocking waits on exactly that state, and any remote node changing any
// unrelated endpoint re-announces. A genuine departure passes false and clears it.
static void forget_publisher_peer(struct tt_Publisher* pub, uint8_t node_id, bool preserve_ack) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (pub->peers[i].node_id == node_id) {
            pub->peers[i].node_id = tt_NODE_ID_INVALID;
        }
    }
    if (!preserve_ack) {
        forget_peer_ack(pub, node_id, 0, /*match_any_entity=*/true);
    }
}

// Drops (node_id) from the peer and ack sets of every local Publisher sharing `endpoint_id` - the
// per-entity counterpart to forget_peers_from_source(), used when one remote Subscriber is
// presumed dead by its own liveliness lease while its node is otherwise still alive and
// announcing (Phase 3 prerequisite (a), rmw_tickle/PLAN.md).
static void forget_publisher_peers_for_endpoint(struct tt_Node* node, uint32_t endpoint_id, uint8_t node_id) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_PUBLISHER || endpoint->id != endpoint_id) {
            continue;
        }
        forget_publisher_peer((struct tt_Publisher*)endpoint, node_id, /*preserve_ack=*/false);
    }
}

// Clears the ack state of every local Publisher that `node_id` is no longer a matched peer of -
// process_update()'s own companion to forget_peers_from_source(..., preserve_ack=true), run once
// decode_update_entities() has re-added whatever the fresh announce still lists (Phase 3
// prerequisite (c), rmw_tickle/PLAN.md). A Publisher this node is still matched to keeps its ack
// watermark untouched across the announce.
static void drop_ack_state_for_unmatched_source(struct tt_Node* node, uint8_t node_id) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_PUBLISHER) {
            continue;
        }
        struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
        bool still_matched = false;
        for (int j = 0; j < tt_MAX_PEER_COUNT; j++) {
            if (pub->peers[j].node_id == node_id) {
                still_matched = true;
                break;
            }
        }
        if (!still_matched) {
            forget_peer_ack(pub, node_id, 0, /*match_any_entity=*/true);
        }
    }
}

// Drops every peer-table entry pointing at `node_id`, across every Publisher and Client on this
// node. Called when a fresh UPDATE from that source arrives (process_update): its new announce is
// authoritative for what it still hosts, and decode_update_entities() re-adds whatever's still
// listed. Also does the right thing for a node that has left - tt_Node_destroy() broadcasts a
// final entity-less UPDATE, so this forgets it and nothing gets re-added.
static void forget_peers_from_source(struct tt_Node* node, uint8_t node_id, bool preserve_ack) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL) {
            continue;
        }
        if (endpoint->kind == tt_KIND_TOPIC_PUBLISHER) {
            forget_publisher_peer((struct tt_Publisher*)endpoint, node_id, preserve_ack);
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
                                     uint8_t qos, uint64_t deadline_duration_ns, uint64_t liveliness_lease_duration_ns,
                                     const char* type, const char* name) {
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
    slot->deadline_duration_ns = deadline_duration_ns;
    slot->liveliness_lease_duration_ns = liveliness_lease_duration_ns;
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

// Milestone 35 (rmw_tickle/PLAN.md) - deliberately does NOT reject a second endpoint sharing an
// already-registered (kind, id): real DDS lets multiple independent entities (Publishers,
// Subscribers, Clients, or Servers) share one topic/service name, and rmw_tickle/PLAN.md's own
// Milestone 34 (multiple ROS 2 nodes per process, sharing one tt_Node) made this reachable within
// a single process for the first time - two rmw nodes in one process each creating a Publisher
// for the same topic, or a Server for the same service, is a normal pattern this used to reject
// outright as "Duplicate endpoint", which was never really a wire-protocol requirement, only a
// side effect of this table's own single-entry-per-id assumption (see find_endpoint()'s and
// for_each_endpoint()'s own doc comments for how lookups now handle more than one match). Still
// guards against the one thing that IS always a real bug: registering the exact same struct
// pointer twice (a double-create without an intervening destroy).
static tt_ret_t add_endpoint_to_node(struct tt_Node* node, struct tt_Endpoint* endpoint) {
    if (node->endpoint_count >= tt_MAX_ENDPOINT_COUNT) {
        uint32_t endpoint_count = node->endpoint_count;
        TT_LOG_ERROR("Too many endpoints: %u", endpoint_count);
        return tt_RET_OUT_OF_BUFFER;
    }

    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        if (node->endpoints[i] == endpoint) {
            TT_LOG_ERROR("Endpoint already registered: kind=%u id=%u", endpoint->kind, endpoint->id);
            return tt_RET_IILEGAL_ENDPOINT_ID;
        }
    }

    // Milestone 47 - every entity kind gets its own entity_id here, the single shared
    // registration point for all four (Publisher/Subscriber/Client/Server) - see struct tt_
    // Endpoint.entity_id's own doc comment (tickle.h) for what this is and struct tt_Node.
    // entity_id_base/next_entity_id's own doc comment for the generation scheme.
    endpoint->entity_id = node->entity_id_base + node->next_entity_id++;

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

// QoS roadmap #1 (RxO matching, Milestone 31) / #3 (LIVELINESS, Milestone 49) - the tt_UPDATE_
// QOS_* bits this endpoint's own UpdateEntity announces, offered (a Publisher) or requested (a
// Subscriber) depending on kind - see their own doc comment (tickle.h). Always 0 for a service/
// client: RELIABILITY there is already unconditional via tt_Client_call()'s own retry (no
// negotiation needed), and DURABILITY/LIVELINESS-kind have no service/client analog at all -
// matches endpoint_type_name()'s own kind-dispatch shape.
static uint8_t endpoint_qos_bits(struct tt_Endpoint* endpoint) {
    switch (endpoint->kind) {
    case tt_KIND_TOPIC_PUBLISHER: {
        struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
        return (uint8_t)((pub->reliable ? tt_UPDATE_QOS_RELIABLE : 0) | (pub->durable ? tt_UPDATE_QOS_DURABLE : 0) |
                         (pub->liveliness_manual ? tt_UPDATE_QOS_LIVELINESS_MANUAL : 0) |
                         (pub->keep_all ? tt_UPDATE_QOS_KEEP_ALL : 0)); // Phase 3 - see that bit's doc comment
    }
    case tt_KIND_TOPIC_SUBSCRIBER: {
        struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
        return (uint8_t)((sub->reliable ? tt_UPDATE_QOS_RELIABLE : 0) | (sub->durable ? tt_UPDATE_QOS_DURABLE : 0) |
                         (sub->liveliness_manual ? tt_UPDATE_QOS_LIVELINESS_MANUAL : 0));
    }
    default:
        return 0;
    }
}

// QoS roadmap #2 (DEADLINE) RxO, Milestone 49 - this endpoint's own offered/requested DEADLINE
// duration, the numeric counterpart endpoint_qos_bits() above can't carry (a bit says "which
// policy", not "how long"). Same kind-dispatch shape/convention as endpoint_qos_bits().
static uint64_t endpoint_deadline_duration_ns(struct tt_Endpoint* endpoint) {
    switch (endpoint->kind) {
    case tt_KIND_TOPIC_PUBLISHER:
        return ((struct tt_Publisher*)endpoint)->deadline_duration_ns;
    case tt_KIND_TOPIC_SUBSCRIBER:
        return ((struct tt_Subscriber*)endpoint)->deadline_duration_ns;
    default:
        return 0;
    }
}

// QoS roadmap #3 (LIVELINESS) RxO, Milestone 49 - this endpoint's own offered/requested
// liveliness lease duration, independent of the tt_UPDATE_QOS_LIVELINESS_MANUAL bit
// (endpoint_qos_bits() above) - see tt_UpdateEntity.liveliness_lease_duration_ns's own doc
// comment (tickle.h) for why kind and lease duration travel as two separate wire fields.
static uint64_t endpoint_liveliness_lease_duration_ns(struct tt_Endpoint* endpoint) {
    switch (endpoint->kind) {
    case tt_KIND_TOPIC_PUBLISHER:
        return ((struct tt_Publisher*)endpoint)->liveliness_lease_duration_ns;
    case tt_KIND_TOPIC_SUBSCRIBER:
        return ((struct tt_Subscriber*)endpoint)->liveliness_lease_duration_ns;
    default:
        return 0;
    }
}

// QoS roadmap #2 (DEADLINE) / #3 (LIVELINESS) RxO, Milestone 49 - true iff a pairing requesting
// `requested_deadline_ns`/`requested_manual`/`requested_lease_ns` can never be satisfied by one
// offering `offered_deadline_ns`/`offered_manual`/`offered_lease_ns`. Mirrors rmw_tickle's own
// already-correct, already-tested static rmw_qos_profile_check_compatible() (rmw_qos.c) exactly -
// TickLE core can't call that directly (it has no concept of rmw_qos_profile_t, being ROS/rmw-
// agnostic), so this is the identical logic re-expressed against TickLE's own plain duration/bool
// primitives instead. 0 means "no requirement/infinite" on either side for both duration fields,
// matching every other 0-disabled convention this codebase already uses (tt_Publisher.lifespan_
// duration_ns etc.) - DEADLINE: an offered duration must be <= whatever's requested (0 offered =
// infinite, always too loose for any finite request; 0 requested = no requirement, anything
// satisfies it). LIVELINESS: AUTOMATIC can't satisfy a MANUAL_BY_TOPIC request (this rmw's only
// two kinds, Milestone 32's own finding); the lease duration itself follows the identical
// offered<=requested rule as DEADLINE, independent of kind (real DDS's own LIVELINESS policy is
// (kind, lease_duration) as one combined unit - a Subscriber may legitimately request AUTOMATIC
// with a tight lease requirement, not just a MANUAL_BY_TOPIC one).
static bool deadline_liveliness_incompatible(uint64_t requested_deadline_ns, uint64_t offered_deadline_ns,
                                             bool requested_manual, bool offered_manual, uint64_t requested_lease_ns,
                                             uint64_t offered_lease_ns) {
    if (requested_deadline_ns != 0 && (offered_deadline_ns == 0 || offered_deadline_ns > requested_deadline_ns)) {
        return true;
    }
    if (requested_manual && !offered_manual) {
        return true;
    }
    return requested_lease_ns != 0 && (offered_lease_ns == 0 || offered_lease_ns > requested_lease_ns);
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
static bool build_and_send_update(struct tt_Node* node, const struct tt_Peer* peers, uint8_t peer_count);
// Milestone 47 "goodbye" - see its own definition's doc comment.
static void broadcast_goodbye(struct tt_Node* node);
static void node_flush(struct tt_Node* node, uint64_t time, void* param);
static void check_liveliness(struct tt_Node* node, uint64_t time, void* param);
static void server_cache_clean(struct tt_Node* node, uint64_t time, void* param);
static void clear_server_cache_slot(struct tt_Server* server, int slot);
// QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - see each definition's own comment.
static void acknack_retry(struct tt_Node* node, uint64_t time, void* param);
static uint16_t reliable_cache_depth(const struct tt_ReliableCache* cache);
static uint64_t reliable_retry_interval(void);
static void send_acknack(struct tt_Node* node, struct tt_WriterProxy* proxy);
static void advance_ack_seq_no(struct tt_WriterProxy* proxy);
static void maybe_arm_acknack_retry(struct tt_Node* node, struct tt_WriterProxy* proxy);
static bool update_reliable_ack(struct tt_Node* node, struct tt_Subscriber* sub, uint32_t seq_no,
                                uint8_t sender_node_id, uint32_t sender_entity_id, uint32_t sender_ip,
                                uint16_t sender_port);
static void jump_ack_baseline(struct tt_WriterProxy* proxy, uint32_t seq_no);
static int highest_relevant_bit(const struct tt_WriterProxy* proxy);
// Milestone 47 - WriterProxy table lookup/creation - see struct tt_WriterProxy's own doc comment
// (tickle.h) and each definition. find_endpoint_by_entity() (the entity_id-aware ACKNACK routing
// lookup) needs no forward declaration here - it's defined right next to find_endpoint() above,
// before its own only call site (process_acknack(), further down this file).
static struct tt_WriterProxy* find_writer_proxy(struct tt_Subscriber* sub, uint8_t node_id, uint32_t entity_id);
static struct tt_WriterProxy* find_or_create_writer_proxy(struct tt_Subscriber* sub, uint8_t node_id,
                                                          uint32_t entity_id, bool* out_created);
// QoS roadmap #5 (RELIABILITY) follow-up - Heartbeat, see struct tt_HeartbeatHeader's own doc
// comment (tickle.h).
static void send_heartbeat(struct tt_Node* node, uint64_t time, void* param);
static void send_initial_heartbeat(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Peer* target);
// QoS roadmap #5 (RELIABILITY) follow-up - periodic ACK solicitation, see struct tt_Publisher.
// ack_solicit_period_ns's own doc comment (tickle.h).
static void send_ack_solicit(struct tt_Node* node, uint64_t time, void* param);
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
    node->entity_id_base = 0; // real value assigned by tt_Node_create() itself, after this call
    node->next_entity_id = 0;

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

    // Milestone 47 - this launch's own random entity_id base (struct tt_Node.entity_id_base's own
    // doc comment, tickle.h): tt_get_ns()'s low 32 bits, no separate RNG primitive needed - this
    // node's own launch instant already is one, and this is exactly the kind of "coarse, no
    // cryptographic requirement" randomness every other sentinel/hash choice in this file already
    // accepts (e.g. tt_hash_id() itself).
    node->entity_id_base = (uint32_t)tt_get_ns();

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
    pub->batch = false;                 // see tickle.h's own doc comment on this field for why this is the default
    pub->reliable_cache = NULL;         // no retained-sample storage by default - see its own doc comment
    pub->reliable = false;              // best-effort by default - see tt_Publisher.reliable's own doc comment
    pub->durable = false;               // volatile by default - see tt_Publisher.durable's own doc comment
    pub->heartbeat_period_ns = 0;       // no periodic Heartbeat by default - see its own doc comment
    pub->ack_solicit_period_ns = 0;     // no periodic ACK solicitation by default - see its own doc comment
    pub->ack_solicit_watermark_pct = 0; // no watermark-triggered solicitation either (Phase 3 (d))
    pub->last_ack_solicit_ns = 0;
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
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].node_id = tt_NODE_ID_INVALID; // all empty - see struct tt_WriterProxy
    }

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
    broadcast_goodbye(client->node);

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
    broadcast_goodbye(server->node);

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
    data_header->entity_id = endpoint->entity_id; // Milestone 47 - this Publisher's own identity

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
// Phase 3 prerequisite (d), rmw_tickle/PLAN.md - the lowest cumulative ack across every currently
// matched peer, i.e. how far *all* of them have got. 0 when any matched peer has never sent an
// ACKNACK (or none are matched), matching tt_PeerAck.ack_seq_no's own "unknown" convention.
static uint32_t min_peer_ack_seq_no(const struct tt_Publisher* pub) {
    uint32_t lowest = 0;
    bool first = true;
    // Phase 2 - one entry per matched Subscriber *entity* (claimed at match time, dropped on
    // departure), so iterating the table is iterating exactly the set that has to agree.
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].node_id == tt_NODE_ID_INVALID) {
            continue;
        }
        // Read straight from the entry: the table is the matched set now, so no lookup is needed,
        // and this function taking a const pointer is what keeps tt_Publisher_min_acked_seq_no()
        // from laundering const through a uintptr_t cast (clang-tidy performance-no-int-to-ptr,
        // a real CI failure on main before d0263b4).
        uint32_t value = pub->peer_acks[i].ack_seq_no;
        if (first || value < lowest) {
            lowest = value;
            first = false;
        }
    }
    return lowest;
}

// Phase 3 (rmw_tickle/PLAN.md) - how many samples this Publisher may hold unacknowledged: its own
// retained depth, or the narrowest RELIABLE tracking window any matched Subscriber announced,
// whichever is smaller. A Subscriber cannot ask about a gap older than its own window, so an
// unacknowledged run deeper than that can never be recovered however much is retained here
// (measured in Phase 2: a window wider than the Publisher's own depth recovers strictly less).
static uint32_t keep_all_bound(const struct tt_Publisher* pub) {
    uint32_t depth = reliable_cache_depth(pub->reliable_cache);
    uint32_t window = tt_Publisher_unacked_bound(pub);
    return window < depth ? window : depth;
}

// Whether a KEEP_ALL Publisher may accept one more sample: refused only when accepting it would
// push the unacknowledged run past keep_all_bound(), i.e. would force cache_reliable_sample() to
// evict something nobody has acknowledged yet.
//
// An empty ack set means writable: with no matched Subscriber left - none ever matched, or the last
// one was declared not alive (Phase 3 (a)) - there is nobody whose acknowledgement could ever
// arrive, so staying blocked would mean waiting forever on nothing. min_peer_ack_seq_no() returns 0
// both for "no peers" and for "a matched peer that has never acked", so the two are told apart by
// the ack table being empty, not by that value.
// Does this Publisher still have acknowledgement state for any matched Subscriber entity? Shared
// by keep_all_writable()'s "nothing left to wait for" case and the counter that records when that
// case is what made a Publisher writable - one definition so the two can't drift apart.
static bool any_peer_ack_matched(const struct tt_Publisher* pub) {
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].node_id != tt_NODE_ID_INVALID) {
            return true;
        }
    }
    return false;
}

static bool keep_all_writable(const struct tt_Publisher* pub) {
    if (!pub->keep_all || reliable_cache_depth(pub->reliable_cache) == 0) {
        return true; // KEEP_LAST (the default), or nothing retained at all - never refuses a write
    }

    if (!any_peer_ack_matched(pub)) {
        return true; // nothing left to wait for
    }

    uint32_t min_ack = min_peer_ack_seq_no(pub);
    uint32_t acked_through = min_ack > 0 ? min_ack - 1 : 0; // ack_seq_no means "everything below it"
    uint32_t unacked_after_this = pub->seq_no + 1 - acked_through;
    return unacked_after_this <= keep_all_bound(pub);
}

// Phase 3 - fires the writable callback exactly once per refusal-to-writable transition: a KEEP_ALL
// Publisher that refused a write becomes writable again when an ACKNACK advances the slowest matched
// Subscriber (or when the last of them goes away). No-op unless a publish was actually refused, so
// an ordinary acking stream costs one boolean test per ACKNACK.
static void notify_writable_if_pending(struct tt_Publisher* pub) {
    if (!pub->writable_pending || !keep_all_writable(pub)) {
        return;
    }
    pub->writable_pending = false;
    RSTAT_INC(writable_callbacks);
    // Phase 3 step 4 follow-up - two very different events reach this line. Either the slowest
    // matched Subscriber acknowledged enough, which is KEEP_ALL working, or the last matched
    // Subscriber went away and keep_all_writable()'s "nothing left to wait for" made the Publisher
    // writable by default, which is KEEP_ALL silently ceasing to apply. Both are correct - there
    // genuinely is nobody to hold a sample for - but they are not the same thing to anyone reading
    // a measurement, and only the counter tells them apart.
    //
    // Not observed in practice: across 24 HIL runs at 20% and 50% injected loss the matched-peer
    // count never once fell to zero (rmw_tickle/PLAN.md Phase 3 step 4). This exists so that if it
    // ever does - a real partition, a liveliness timeout under sustained loss - it shows up in a
    // measurement already being taken rather than as an unexplained absence of back-pressure.
    if (!any_peer_ack_matched(pub)) {
        RSTAT_INC(writable_no_peers);
    }
    if (pub->writable_callback != NULL) {
        pub->writable_callback(pub, pub->writable_callback_param);
    }
}

// Phase 3 prerequisite (d) - asks every matched peer for an ACK once enough retained samples are
// unacknowledged. Throttled to at most one solicitation per max(ack_solicit_period_ns, the reliable
// retry interval), shared with the periodic path, so a max-rate Publisher - which crosses the
// watermark on essentially every publish - sends at most one extra Heartbeat per millisecond rather
// than one per sample.
#define PERCENT_SCALE 100U // ack_solicit_watermark_pct is a percentage, not a fraction

// Sends one solicitation unless the shared throttle says it's too soon. Split out so the watermark
// path below and the refusal path in tt_Publisher_publish() can't drift apart on the throttle.
static void solicit_ack_throttled(struct tt_Publisher* pub) {
    uint64_t now = tt_get_ns();
    uint64_t min_gap =
        pub->ack_solicit_period_ns > reliable_retry_interval() ? pub->ack_solicit_period_ns : reliable_retry_interval();
    if (pub->last_ack_solicit_ns != 0 && now - pub->last_ack_solicit_ns < min_gap) {
        RSTAT_INC(ack_solicit_suppressed);
        return;
    }
    pub->last_ack_solicit_ns = now;
    RSTAT_INC(ack_solicit_sent);
    (void)tt_Publisher_request_ack(pub);
}

// How many unacknowledged samples should trigger a solicitation, or 0 for "never".
//
// A KEEP_ALL Publisher is not optional about this, which is the whole point: it stops publishing
// when unacked reaches keep_all_bound(), and a Subscriber only ACKNACKs when it sees a gap, so on a
// clean link nothing would ever advance the acknowledgement and the Publisher would stall at the
// window and never recover. Measured on real hardware (rmw_tickle/PLAN.md Phase 3 step 4): a
// lossless 5-second run sent exactly its 1024-sample window and then nothing at all, 0.125 Mbps
// against the 109 Mbps the same run reaches once acknowledgements flow. Zero loss is the worst
// case here, not the easy one, which is why nothing caught it before KEEP_ALL made blocking depend
// on it.
//
// Half the bound, deliberately, and the bound rather than the cache depth: the bound is what
// actually stops the writes (min(depth, the narrowest announced window)), so a depth-relative
// watermark measures the wrong quantity - at depth 2048 with a 1024 window, a 80% watermark would
// first ask at 1638 unacked, i.e. 600 samples after the Publisher had already stopped. Half leaves
// a full round trip's worth of headroom to answer in before anything blocks.
static uint32_t ack_solicit_threshold(const struct tt_Publisher* pub, uint16_t depth) {
    if (pub->keep_all) {
        uint32_t bound = keep_all_bound(pub);
        return bound > 1 ? bound / 2 : 1;
    }
    if (pub->ack_solicit_watermark_pct == 0) {
        return 0; // opt-in for everyone else - see ack_solicit_watermark_pct's own doc comment
    }
    uint32_t threshold = (uint32_t)(((uint64_t)depth * pub->ack_solicit_watermark_pct) / PERCENT_SCALE);
    return threshold > 0 ? threshold : 1; // a percentage that rounds to nothing still means "ask early"
}

static void maybe_solicit_ack_at_watermark(struct tt_Publisher* pub) {
    if (pub->reliable_cache == NULL) {
        return;
    }
    uint16_t depth = reliable_cache_depth(pub->reliable_cache);
    if (depth == 0 || count_peers(pub->peers) == 0) {
        return; // nothing retained to ack, or nobody matched to ask (never heartbeat into the void)
    }
    uint32_t threshold = ack_solicit_threshold(pub, depth);
    if (threshold == 0) {
        return;
    }

    uint32_t min_ack = min_peer_ack_seq_no(pub);
    uint32_t acked_through = min_ack > 0 ? min_ack - 1 : 0; // ack_seq_no means "everything below it"
    uint32_t unacked = pub->seq_no > acked_through ? pub->seq_no - acked_through : 0;
    if (unacked < threshold) {
        return;
    }

    solicit_ack_throttled(pub);
}

// B1 (rmw_tickle/PLAN.md) - the in-use ring size, or 0 when this cache isn't usable at all (no
// index, no arena, or capacity 0 - struct tt_ReliableCache's own "nothing usable yet" state). The
// single place the old `(depth > 0 && depth <= capacity) ? depth : capacity` clamp expression,
// once repeated at every call site, now lives.
static uint16_t reliable_cache_depth(const struct tt_ReliableCache* cache) {
    if (cache == NULL || cache->capacity == 0 || cache->index == NULL || cache->arena == NULL ||
        cache->arena_size == 0) {
        return 0;
    }
    return (cache->depth > 0 && cache->depth <= cache->capacity) ? cache->depth : cache->capacity;
}

// The slot seq_no currently maps to - the direct index every lookup, eviction and backlog walk
// shares (see struct tt_ReliableCache.depth's own doc comment on why depth may not change once
// anything is cached).
static struct tt_ReliableCacheIndex* reliable_cache_slot(const struct tt_ReliableCache* cache, uint16_t depth,
                                                         uint32_t seq_no) {
    return &cache->index[(seq_no - 1) % depth];
}

// True when this slot genuinely holds seq_no's bytes right now - not empty, not a tombstone (an
// evicted or never-cached sample, see struct tt_ReliableCacheIndex.len), and not some other
// sample that has since taken the slot over.
static bool reliable_cache_slot_live(const struct tt_ReliableCache* cache, uint16_t depth, uint32_t seq_no) {
    const struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, seq_no);
    return entry->len != 0 && entry->seq_no == seq_no;
}

static void reliable_cache_drop_leading_tombstones(struct tt_ReliableCache* cache, uint16_t depth);

// Drops the oldest retained sample: KEEP_LAST eviction, whether it was the count bound or the byte
// bound that demanded the room. Leaves the cache with a *live* oldest entry (or empty), so
// reliable_cache_write_offset() below can read the oldest record's own offset directly.
static void reliable_cache_evict_oldest(struct tt_ReliableCache* cache, uint16_t depth) {
    if (cache->oldest_seq_no == 0) {
        return;
    }
    struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, cache->oldest_seq_no);
    if (entry->seq_no == cache->oldest_seq_no) {
        entry->len = 0; // tombstone: the bytes are gone, the slot may still be named by an ACKNACK
    }
    if (cache->oldest_seq_no == cache->newest_seq_no) {
        cache->oldest_seq_no = 0; // nothing retained any more; newest_seq_no stays (it's "last
        cache->tail = 0;          // published", what first_resendable falls back to)
        return;
    }
    cache->oldest_seq_no++;
    reliable_cache_drop_leading_tombstones(cache, depth);
}

// An oversized (never-cached) sample leaves a tombstone that may end up at the oldest end of the
// range; drop those so "oldest retained" always names real bytes.
static void reliable_cache_drop_leading_tombstones(struct tt_ReliableCache* cache, uint16_t depth) {
    while (cache->oldest_seq_no != 0 && !reliable_cache_slot_live(cache, depth, cache->oldest_seq_no)) {
        if (cache->oldest_seq_no == cache->newest_seq_no) {
            cache->oldest_seq_no = 0;
            cache->tail = 0;
            return;
        }
        cache->oldest_seq_no++;
    }
}

// Where a `length`-byte record may be written right now, or UINT32_MAX when the oldest retained
// record has to be evicted first. Records are never split (struct tt_ReliableCache.arena's own doc
// comment): when the space before the end of the arena is too small, the write wraps to offset 0
// and the tail fragment is simply wasted until the ring passes it.
static uint32_t reliable_cache_write_offset(const struct tt_ReliableCache* cache, uint16_t depth, uint32_t length) {
    if (cache->oldest_seq_no == 0) {
        return 0; // empty - the whole arena is free (the caller already rejected length > arena_size)
    }
    uint32_t head = reliable_cache_slot(cache, depth, cache->oldest_seq_no)->offset;
    if (cache->tail == head) {
        return UINT32_MAX; // the live records fill the arena exactly - evict before anything fits
                           // (tail == head reads as "empty" everywhere else, hence this first)
    }
    if (cache->tail > head) { // live bytes are one contiguous [head, tail) run
        if (length <= cache->arena_size - cache->tail) {
            return cache->tail;
        }
        return length <= head ? 0 : UINT32_MAX; // wrap to the front if the free head fragment fits
    }
    return length <= head - cache->tail ? cache->tail : UINT32_MAX; // live run wraps: free is [tail, head)
}

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
    uint16_t depth = reliable_cache_depth(cache);
    if (depth == 0) {
        return; // index[]/capacity/arena never set up (struct tt_ReliableCache's own doc comment) -
                // nothing to cache into, same safe no-op every other clamp site below shares
    }
    uint32_t length = (uint32_t)ROUNDUP((uintptr_t)node->tx_buffer + node->tx_tail - (uintptr_t)submessage_header);

    // Index room first, regardless of whether the bytes will fit below: this sample's own slot,
    // (seq_no - 1) % depth, is currently held by the sample exactly `depth` back, which KEEP_LAST
    // evicts to make room - the count bound, DDS's own HISTORY.depth.
    while (cache->oldest_seq_no != 0 && seq_no - cache->oldest_seq_no + 1 > depth) {
        RSTAT_INC(evicted_by_count);
        reliable_cache_evict_oldest(cache, depth);
    }

    struct tt_ReliableCacheIndex* entry = &cache->index[(seq_no - 1) % depth];
    entry->seq_no = seq_no;
    entry->retry = 0;
    entry->timestamp = tt_get_ns();
    entry->offset = 0;
    entry->len = 0; // a tombstone until the bytes below actually land
    cache->newest_seq_no = seq_no;

    if (length > cache->arena_size) {
        // B1 - this one sample can never fit, however much is evicted: publish it (the caller
        // already encoded it into tx_buffer) but retain nothing, and leave the rest of the cache
        // alone rather than evicting for room that could never exist. The slot stays a tombstone,
        // so an ACKNACK naming it gets Phase 1-c's eviction Heartbeat and the Subscriber skips it
        // immediately instead of retrying; DURABILITY's backlog simply doesn't contain it.
        TT_LOG_WARNING("Reliable sample %u (%u bytes) exceeds the %u-byte cache arena - sent, not cached", seq_no,
                       length, cache->arena_size);
        RSTAT_INC(not_cached_oversize);
        reliable_cache_drop_leading_tombstones(cache, depth);
        return;
    }

    // Contiguous byte room, evicting oldest-first until this record fits (the byte bound, DDS's
    // own RESOURCE_LIMITS). reliable_cache_write_offset() returns where it would go, or UINT32_MAX
    // while something still has to be evicted first.
    uint32_t offset = reliable_cache_write_offset(cache, depth, length);
    while (offset == UINT32_MAX) {
        RSTAT_INC(evicted_by_bytes);
        reliable_cache_evict_oldest(cache, depth);
        offset = reliable_cache_write_offset(cache, depth, length);
    }

    _tt_memcpy(cache->arena + offset, submessage_header, length);
    entry->offset = offset;
    entry->len = (uint16_t)length;
    cache->tail = offset + length;
    if (cache->oldest_seq_no == 0) {
        cache->oldest_seq_no = seq_no;
    }
}

// QoS roadmap #6 (LIFESPAN) - see tt_Publisher.lifespan_duration_ns's own doc comment (tickle.h).
// lifespan_duration_ns == 0 means "no LIFESPAN requested" - never expired, matching every other
// disabled-by-zero convention this struct already uses (heartbeat_period_ns, etc).
static bool reliable_cache_entry_expired(const struct tt_ReliableCacheIndex* entry, uint64_t lifespan_duration_ns) {
    return lifespan_duration_ns != 0 && (tt_get_ns() - entry->timestamp) >= lifespan_duration_ns;
}

// Milestone 58 (rmw_tickle/PLAN.md) - true if durable_delivered[] already records this exact
// (node_id, last_modified) pair, i.e. this announce is a re-announce from a peer that already has
// this Publisher's current backlog, not a genuinely new match. See struct tt_DurableDeliveryRecord's
// own doc comment (tickle.h) for why last_modified, not node_id alone, is the right key.
static bool durable_delivered_get(const struct tt_ReliableCache* cache, uint8_t node_id, uint64_t last_modified) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (cache->durable_delivered[i].node_id == node_id) {
            return cache->durable_delivered[i].last_modified == last_modified;
        }
    }
    return false;
}

// Records that node_id has now received the backlog as of last_modified - refreshes an existing
// slot for that node_id, or claims the first empty one, mirroring upsert_peer()'s own style. A full
// table (durable_delivered_upsert() finding neither) is a safe no-op: the worst case is one
// redundant re-delivery next time, never a correctness problem (struct tt_DurableDeliveryRecord's
// own doc comment).
static void durable_delivered_upsert(struct tt_ReliableCache* cache, uint8_t node_id, uint64_t last_modified) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (cache->durable_delivered[i].node_id == node_id) {
            cache->durable_delivered[i].last_modified = last_modified;
            return;
        }
    }
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (cache->durable_delivered[i].node_id == tt_NODE_ID_INVALID) {
            cache->durable_delivered[i].node_id = node_id;
            cache->durable_delivered[i].last_modified = last_modified;
            return;
        }
    }
}

tt_ret_t tt_Publisher_publish(struct tt_Publisher* pub, struct tt_Data* data) {
    if (pub == NULL || data == NULL || pub->node == NULL || pub->topic == NULL ||
        pub->topic->data_encode_size == NULL || pub->topic->data_encode == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    struct tt_Node* node = pub->node;
    uint32_t old_tx_tail = node->tx_tail;

    // Phase 3 (rmw_tickle/PLAN.md) - KEEP_ALL flow control: refuse rather than evict a sample
    // nobody has acknowledged. Checked before anything is encoded, so a refused publish sends
    // nothing, caches nothing and doesn't advance seq_no; the caller retries once
    // tt_Publisher_writable() is true (or its writable_callback fires).
    if (!keep_all_writable(pub)) {
        pub->writable_pending = true; // so the callback fires on the transition back
        RSTAT_INC(publish_refused);
        // Ask again, every refusal (subject to the same throttle). The watermark above is the
        // first line and normally the only one that fires, but it leaves a hole this closes: once
        // the Publisher has actually stopped, seq_no stops moving, so nothing can cross the
        // watermark a second time. If that one solicitation - or the ACKNACK answering it - is
        // lost, which is exactly what a lossy link does, the stall would last until something else
        // happened to ask. Soliciting here bounds recovery to one throttle interval in every case.
        solicit_ack_throttled(pub);
        return tt_RET_WOULD_BLOCK;
    }

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
    data_header->entity_id = endpoint->entity_id; // Milestone 47 - this Publisher's own identity

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

    // Phase 3 prerequisite (d) - after the sample is out and counted, ask for an ACK if the cache
    // is now watermark-full of unacknowledged samples. No-op unless a caller opted in.
    maybe_solicit_ack_at_watermark(pub);

    return tt_RET_OK;
}

// Oldest still-retained seq_no in cache, or 0 if nothing is retained yet (seq_no 0 never occurs on
// the wire - tt_Publisher_publish()'s own data_header->seq_no = pub->seq_no + 1, starting from 1 -
// so it doubles as "empty" here). Shared by send_heartbeat()'s own periodic announce and send_
// initial_heartbeat()'s own discovery-triggered one-off, below.
static uint32_t reliable_cache_oldest_seq_no(struct tt_ReliableCache* cache) {
    if (reliable_cache_depth(cache) == 0) {
        return 0; // not set up - same "nothing retained" return this already uses for a genuinely
                  // empty cache, see this function's own doc comment
    }
    // B1 - a plain field read now (0 when nothing is retained), where this used to be an O(depth)
    // scan over every slot on every periodic/initial/request_ack Heartbeat.
    return cache->oldest_seq_no;
}

// Encodes and sends one Heartbeat submessage announcing [first_seq_no, pub->seq_no] to the given
// target(s) - split out of send_heartbeat()/send_initial_heartbeat() purely to keep cognitive
// complexity down and share the actual wire encoding between the periodic and discovery-triggered
// paths, same reasoning cache_reliable_sample()/cache_durable_sample() were split out of tt_
// Publisher_publish() for. peers/peer_count follow end_encode()'s own convention directly (NULL/0
// broadcasts). flags is tt_HEARTBEAT_FLAG_FINAL or 0 - see its own doc comment (tickle.h); every
// caller before tt_Publisher_request_ack() existed always passed tt_HEARTBEAT_FLAG_FINAL.
static void encode_and_send_heartbeat(struct tt_Node* node, struct tt_Publisher* pub, uint32_t first_seq_no,
                                      const struct tt_Peer* peers, uint8_t peer_count, uint8_t flags) {
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
    heartbeat_header->entity_id = endpoint->entity_id; // Milestone 47 - this Publisher's own identity
    heartbeat_header->flags = flags;
    heartbeat_header->reserved[0] = 0;
    heartbeat_header->reserved[1] = 0;
    heartbeat_header->reserved[2] = 0;

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
        encode_and_send_heartbeat(node, pub, first_seq_no, peers, peer_count, tt_HEARTBEAT_FLAG_FINAL);
    }

    if (!tt_Node_schedule(node, time + pub->heartbeat_period_ns, send_heartbeat, pub)) {
        TT_LOG_ERROR("Cannot schedule send_heartbeat");
    }
}

// QoS roadmap #5 (RELIABILITY) follow-up - runs once per pub->ack_solicit_period_ns (armed by tt_
// Publisher_set_ack_solicit_period()), periodically calling tt_Publisher_request_ack() so pub->
// peer_acks[] stays fresh even on a fully healthy link - the gap send_heartbeat() above
// can't close on its own (it always sets tt_HEARTBEAT_FLAG_FINAL, so a healthy Subscriber has no
// reason to ever reply - see that flag's own doc comment, tickle.h, and struct tt_Publisher.
// ack_solicit_period_ns's own doc comment for why these two periodic mechanisms are distinct, not
// a duplicate of each other). Return value ignored, same reasoning tt_Publisher_set_ack_solicit_
// period()'s own doc comment gives - "nothing to solicit yet" (no peers matched, or nothing
// published) is a normal transient state during periodic operation, not an error worth logging on
// every tick, mirroring send_heartbeat()'s own silent-skip for its analogous case above.
static void send_ack_solicit(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;
    pub->last_ack_solicit_ns = tt_get_ns(); // shared throttle - see ack_solicit_watermark_pct (tickle.h)
    (void)tt_Publisher_request_ack(pub);

    if (!tt_Node_schedule(node, time + pub->ack_solicit_period_ns, send_ack_solicit, pub)) {
        TT_LOG_ERROR("Cannot schedule send_ack_solicit");
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
    encode_and_send_heartbeat(node, pub, first_seq_no, target, 1, tt_HEARTBEAT_FLAG_FINAL);
}

// See struct tt_Publisher.heartbeat_period_ns's own doc comment (tickle.h) for why this needs an
// explicit call rather than just setting that field directly.
uint32_t tt_Publisher_unacked_bound(const struct tt_Publisher* pub) {
    if (pub == NULL) {
        return tt_RELIABLE_BITMAP_BITS;
    }
    // The narrowest *announced* window, which is not the same as "start at the protocol default and
    // let peers lower it": a Subscriber announcing a window wider than tt_RELIABLE_BITMAP_BITS (as
    // rmw_tickle's own RMW_TICKLE_TRACKING_WORDS=16, i.e. 1024 samples, does on every subscription)
    // can genuinely ask about a gap that far back, so capping the bound at the default would block
    // a KEEP_ALL Publisher four times earlier than its peers can actually recover from - throughput
    // given away for nothing. The default is the answer only when nobody has announced anything.
    uint32_t bound = 0;
    bool announced = false;
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].node_id == tt_NODE_ID_INVALID) {
            continue;
        }
        uint32_t window = (uint32_t)pub->peer_acks[i].tracking_words * tt_RELIABLE_BITMAP_WORD_BITS;
        if (window == 0) {
            continue; // matched, but announced no window of its own - see this function's own doc comment
        }
        if (!announced || window < bound) {
            bound = window;
            announced = true;
        }
    }
    return announced ? bound : tt_RELIABLE_BITMAP_BITS;
}

uint32_t tt_Publisher_min_acked_seq_no(const struct tt_Publisher* pub) {
    return min_peer_ack_seq_no(pub);
}

bool tt_Publisher_writable(const struct tt_Publisher* pub) {
    if (pub == NULL) {
        return false;
    }
    return keep_all_writable(pub);
}

bool tt_Publisher_is_acked_by_all_peers(const struct tt_Publisher* pub, uint32_t seq_no) {
    // Phase 2 - every matched Subscriber entity must have got this far, not merely every matched
    // node: two Subscriptions of one topic in one remote process each have their own entry.
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].node_id == tt_NODE_ID_INVALID) {
            continue;
        }
        if (pub->peer_acks[i].ack_seq_no <= seq_no) {
            return false; // never acked anything, or not this far yet
        }
    }
    return true;
}

tt_ret_t tt_ReliableCache_init(struct tt_ReliableCache* cache, struct tt_ReliableCacheIndex* index, uint16_t capacity,
                               uint8_t* arena, uint32_t arena_size) {
    if (cache == NULL || index == NULL || arena == NULL || capacity == 0 || arena_size == 0) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (arena_size < tt_RELIABLE_RECORD_BYTES(0)) {
        return tt_RET_INVALID_ARGUMENT; // too small for even an empty payload's framing
    }

    memset(cache, 0, sizeof(*cache));
    cache->index = index;
    cache->capacity = capacity;
    cache->depth = capacity;
    cache->arena = arena;
    cache->arena_size = arena_size;
    return tt_RET_OK;
}

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

// See struct tt_Publisher.ack_solicit_period_ns's own doc comment (tickle.h) for why this needs an
// explicit call rather than just setting that field directly - same reasoning as tt_Publisher_
// set_heartbeat_period() above.
tt_ret_t tt_Publisher_set_ack_solicit_period(struct tt_Publisher* pub, uint64_t period_ns) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (period_ns != 0 && pub->reliable_cache == NULL) {
        return tt_RET_INVALID_ARGUMENT; // nothing for a solicited ACKNACK to confirm without one
    }

    if (pub->ack_solicit_period_ns != 0) {
        tt_Node_unschedule(pub->node, send_ack_solicit, pub); // re-arming or disabling either way
    }
    pub->ack_solicit_period_ns = period_ns;
    if (period_ns == 0) {
        return tt_RET_OK; // disabled
    }

    if (!tt_Node_schedule(pub->node, tt_get_ns() + period_ns, send_ack_solicit, pub)) {
        pub->ack_solicit_period_ns = 0; // failed to arm - stay disabled rather than claim it's on
        return tt_RET_OUT_OF_SCHEDULE;  // tt_MAX_SCHEDULER_LENGTH exhausted
    }
    return tt_RET_OK;
}

// See its own doc comment (tickle.h) for what this is for. Builds its own dense peer list from
// pub->peers[] rather than passing pub->peers/count_peers(pub->peers) straight through the way
// send_heartbeat()/tt_Publisher_publish() do - those two rely on peers[] having no gap before the
// first count_peers() slots, which forget_publisher_peer() alone doesn't guarantee (it clears a
// departed peer's node_id in place, not by compacting the array down) - a real, pre-existing gap
// in that shared shortcut, unrelated to this function, flagged separately rather than fixed here.
// Solicitation specifically must reach every *currently* matched peer correctly - unlike a
// periodic announce, there's no "next period" for a missed one to be silently caught by - so this
// one function is worth the extra O(tt_MAX_PEER_COUNT) filter to not depend on that assumption.
tt_ret_t tt_Publisher_request_ack(struct tt_Publisher* pub) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (pub->reliable_cache == NULL) {
        return tt_RET_INVALID_ARGUMENT; // best-effort - no ack state to solicit
    }

    struct tt_Peer live_peers[tt_MAX_PEER_COUNT];
    uint8_t live_count = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (pub->peers[i].node_id != tt_NODE_ID_INVALID) {
            live_peers[live_count++] = pub->peers[i];
        }
    }
    if (live_count == 0) {
        return tt_RET_OK; // nothing currently matched to ask
    }

    uint32_t first_seq_no = reliable_cache_oldest_seq_no(pub->reliable_cache);
    if (first_seq_no == 0) {
        return tt_RET_INVALID_ARGUMENT; // nothing published yet - see this function's own doc comment
    }

    encode_and_send_heartbeat(pub->node, pub, first_seq_no, live_peers, live_count, 0);
    return tt_RET_OK;
}

// Milestone 47 "goodbye" - broadcasts the node's own now-reduced entity list right away, instead
// of waiting for node_update()'s own next periodic tick (up to tt_NODE_UPDATE_INTERVAL later).
// Shared by every per-entity destroy function below (tt_Publisher_destroy()/tt_Subscriber_
// destroy()/tt_Client_destroy()/tt_Server_destroy()) - call only *after* remove_endpoint_from_
// node() has already removed the departing entity, so encode_update_entities() doesn't announce
// it as still present. decode_update_entities()'s own "authoritative announce" reconciliation
// (forget_peers_from_source(), tickle.c) is what actually makes a departed entity's own matched-
// peer/WriterProxy state disappear promptly on the *receiving* side once this arrives - narrowing,
// not eliminating, the overlap window a not-yet-departed writer could still be confused with a
// newly-arrived one under (real DDS's own lease-expiry-based cleanup has the identical residual
// gap for an ungraceful shutdown - see rmw_tickle/PLAN.md's own Milestone 47 for the full
// writeup). Calls build_and_send_update() directly, not node_update() (which would also re-arm
// its own periodic reschedule on top of the one already pending - safe inside tt_Node_destroy()
// only because that function wipes the whole scheduler right after, not true for a per-entity
// destroy that leaves the node running).
static void broadcast_goodbye(struct tt_Node* node) {
    build_and_send_update(node, NULL, 0);
    if (!flush_tx(node, node->tx_tail, NULL, 0)) {
        TT_LOG_WARNING("Could not send farewell announce on entity destroy");
    }
}

tt_ret_t tt_Publisher_destroy(struct tt_Publisher* pub) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    struct tt_Node* node = pub->node;

    // Cancel a still-armed Heartbeat before this Publisher (its own schedule param) goes away -
    // same reasoning as tt_Subscriber_destroy()'s own acknack_retry cancellation just below.
    if (pub->heartbeat_period_ns != 0) {
        tt_Node_unschedule(node, send_heartbeat, pub);
    }
    // Same reasoning, for a still-armed periodic ACK solicitation.
    if (pub->ack_solicit_period_ns != 0) {
        tt_Node_unschedule(node, send_ack_solicit, pub);
    }

    if (!remove_endpoint_from_node(node, endpoint)) {
        return tt_RET_IILEGAL_ENDPOINT_ID;
    }
    node->last_modified = tt_get_ns();
    broadcast_goodbye(node);
    return tt_RET_OK;
}

tt_ret_t tt_Subscriber_destroy(struct tt_Subscriber* sub) {
    if (sub == NULL || sub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)sub;
    struct tt_Node* node = sub->node;

    // Cancel every outstanding per-writer acknack_retry before this Subscriber's own writers[]
    // table (each entry's own schedule param) goes away - same reasoning as tt_Client_destroy()'s
    // own call_retry cancellation, just once per still-armed WriterProxy instead of once for the
    // whole Subscriber (Milestone 47 - acknack_retry() is now scheduled per struct tt_WriterProxy,
    // not per Subscriber, see its own doc comment).
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (sub->writers[i].acknack_scheduled) {
            tt_Node_unschedule(node, acknack_retry, &sub->writers[i]);
            sub->writers[i].acknack_scheduled = false;
        }
    }

    if (remove_endpoint_from_node(node, endpoint)) {
        node->last_modified = tt_get_ns();
        broadcast_goodbye(node);
        return tt_RET_OK;
    }

    return tt_RET_IILEGAL_ENDPOINT_ID;
}

// QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - encodes and unicasts one ACKNACK
// submessage back to proxy->sender_*, reporting proxy->ack_seq_no/received_bitmap. Not
// fatal on failure, same philosophy as resend_call_request()'s own comment: whichever caller
// armed a retry (update_reliable_ack()/acknack_retry()) will just try again.
// tt_RELIABLE_BITMAP_WORDS-word bitmap operations (config.h's own doc comment on that constant) -
// the small, fixed set struct tt_WriterProxy.received_bitmap/struct tt_AckNackHeader.bitmap (both
// tickle.h) need, each replacing exactly one single-word uint64_t expression this file used before
// the widening (rmw_tickle/PLAN.md's "TickLE-native performance" plan) - not a generic bitset
// library. Every one of these (except bitmap_shift_right(), which needs a genuine cross-word
// carry) is O(word-count), not O(bit-count), by construction.

// Phase 2 (rmw_tickle/PLAN.md) - how wide this Subscriber's own per-writer tracking window is, in
// 64-bit words. The caller-provided buffer's width when it gave one (clamped to
// tt_RELIABLE_BITMAP_MAX_WORDS, config.h - core never trusts a caller's count any more than a
// wire peer's), otherwise the embedded-first tt_RELIABLE_BITMAP_WORDS default.
static uint16_t subscriber_tracking_words(const struct tt_Subscriber* sub) {
    if (sub->tracking_bitmaps == NULL || sub->tracking_words == 0) {
        return (uint16_t)tt_RELIABLE_BITMAP_WORDS;
    }
    return sub->tracking_words <= tt_RELIABLE_BITMAP_MAX_WORDS ? sub->tracking_words
                                                               : (uint16_t)tt_RELIABLE_BITMAP_MAX_WORDS;
}

// The same width, for a writer proxy (which reaches its Subscriber via proxy->sub).
static uint16_t proxy_words(const struct tt_WriterProxy* proxy) {
    return subscriber_tracking_words(proxy->sub);
}

// ...and in bits, for the "is this gap too wide to track at all" checks.
static uint32_t proxy_window_bits(const struct tt_WriterProxy* proxy) {
    return (uint32_t)proxy_words(proxy) * tt_RELIABLE_BITMAP_WORD_BITS;
}

static bool bitmap_is_zero(const uint64_t* bitmap, uint16_t words) {
    for (uint16_t word = 0; word < words; word++) {
        if (bitmap[word] != 0) {
            return false;
        }
    }
    return true;
}

static void bitmap_clear(uint64_t* bitmap, uint16_t words) {
    for (uint16_t word = 0; word < words; word++) {
        bitmap[word] = 0;
    }
}

// bit 0 of the whole bitmap (word 0's own lowest bit) - the "is the position right after the
// watermark already received" check advance_ack_seq_no()'s/advance_past_unavailable()'s own
// absorb loops use.
static bool bitmap_lowest_bit_set(const uint64_t* bitmap) {
    return (bitmap[0] & 1) != 0;
}

static bool bitmap_test_bit(const uint64_t* bitmap, uint32_t offset) {
    return (bitmap[offset / tt_RELIABLE_BITMAP_WORD_BITS] & (1ULL << (offset % tt_RELIABLE_BITMAP_WORD_BITS))) != 0;
}

static void bitmap_set_bit(uint64_t* bitmap, uint32_t offset) {
    bitmap[offset / tt_RELIABLE_BITMAP_WORD_BITS] |= (1ULL << (offset % tt_RELIABLE_BITMAP_WORD_BITS));
}

// Shifts the whole multi-word bitmap right by exactly one bit, carrying word N+1's own bit 0 into
// word N's own top bit - advance_ack_seq_no()'s own per-step realigning shift, now spanning
// tt_RELIABLE_BITMAP_WORDS words instead of the single one this file used before the widening.
static void bitmap_shift_right_one(uint64_t* bitmap, uint16_t words) {
    for (uint16_t word = 0; word + 1 < words; word++) {
        bitmap[word] = (bitmap[word] >> 1) | (bitmap[word + 1] << (tt_RELIABLE_BITMAP_WORD_BITS - 1));
    }
    bitmap[words - 1] >>= 1;
}

// Shifts the whole multi-word bitmap right by `shift_bits` bits, 0 <= shift_bits <
// tt_RELIABLE_BITMAP_BITS - advance_past_unavailable()'s own jump-ahead shift (its own call site
// handles shift_bits >= tt_RELIABLE_BITMAP_BITS separately, via bitmap_clear() instead - a
// full-width-or-wider shift has nothing left to carry and would be undefined behavior for the
// per-word `<<`/`>>` below anyway). The only one of these helpers that isn't a plain O(word-count)
// loop - a genuine cross-word carry at an arbitrary bit offset needs it - but shift_bits itself is
// still bounded by the fixed, small tt_RELIABLE_BITMAP_BITS width, not by anything that scales
// with cache depth or config the way Milestone 61's own O(depth) regression did.
static void bitmap_shift_right(uint64_t* bitmap, uint16_t words, uint32_t shift_bits) {
    uint32_t word_shift = shift_bits / tt_RELIABLE_BITMAP_WORD_BITS;
    uint32_t bit_shift = shift_bits % tt_RELIABLE_BITMAP_WORD_BITS;
    // In place, lowest word first: every source index is >= the destination being written, and
    // sources only ever move upward, so a word is never read after it has been overwritten. (This
    // used to copy through a fixed-width scratch array, which a caller-sized window can't have.)
    for (uint16_t word = 0; word < words; word++) {
        uint32_t src = (uint32_t)word + word_shift;
        uint64_t value = src < words ? bitmap[src] >> bit_shift : 0;
        if (bit_shift != 0 && src + 1 < words) {
            value |= bitmap[src + 1] << (tt_RELIABLE_BITMAP_WORD_BITS - bit_shift);
        }
        bitmap[word] = value;
    }
}

// Highest bit index set across the whole multi-word bitmap (bit j: "received(ack_seq_no + j)" -
// see struct tt_WriterProxy's own doc comment, tickle.h), or -1 if none are set. Shared by send_
// acknack() and record_out_of_order_arrival() below - both need "how far ahead does anything
// *confirmed* reach", not just "which bits happen to be 0". Skips whole zero words from the top
// down before falling back to a per-bit scan within the one word that actually has something set -
// O(word-count) in the common (few bits set, high words empty) case, only ever O(word-bits) worst
// case within a single word, never O(tt_RELIABLE_BITMAP_BITS) as a flat scan would be.
static int bitmap_highest_bit(const uint64_t* bitmap, uint16_t words) {
    for (int word = (int)words - 1; word >= 0; word--) {
        if (bitmap[word] == 0) {
            continue;
        }
        int bit_in_word = tt_RELIABLE_BITMAP_WORD_BITS - 1;
        while (bit_in_word >= 0 && !((bitmap[word] >> (unsigned)bit_in_word) & 1)) {
            bit_in_word--;
        }
        return (word * tt_RELIABLE_BITMAP_WORD_BITS) + bit_in_word;
    }
    return -1;
}

// Builds a `highest + 1`-bit-wide low mask (bits 0..highest set, the rest clear) across the whole
// multi-word bitmap - send_acknack()'s own "which positions are even worth asking about" mask,
// mirroring the single-word `(1ULL << (highest + 1)) - 1` this file used before the widening.
// highest < 0 yields an all-zero mask (nothing to ask about); highest >= tt_RELIABLE_BITMAP_BITS - 1
// yields an all-ones mask, matching the single-word version's own `~0ULL` special case (avoiding a
// would-be full-width undefined-shift the same way that one avoided `1ULL << 64` directly).
static void bitmap_low_mask(uint64_t* mask, uint16_t words, int highest) {
    bitmap_clear(mask, words);
    if (highest < 0) {
        return;
    }
    int full_words = (highest + 1) / tt_RELIABLE_BITMAP_WORD_BITS;
    int remaining_bits = (highest + 1) % tt_RELIABLE_BITMAP_WORD_BITS;
    for (int word = 0; word < full_words && word < (int)words; word++) {
        mask[word] = ~0ULL;
    }
    if (remaining_bits != 0 && full_words < (int)words) {
        mask[full_words] = (1ULL << remaining_bits) - 1;
    }
}

// Phase 3 (rmw_tickle/PLAN.md) - did this remote writer's own last announce set
// tt_UPDATE_QOS_KEEP_ALL? Read once, when a WriterProxy is claimed; an announce arriving later
// refreshes the cached answer directly (update_writer_proxies_keep_all()).
//
// Phase 3 step 4 - returns UNKNOWN, not NO, when there is nothing to read: no discovery table
// attached (it's opt-in) or nothing heard from that writer yet. The distinction is the whole point;
// see tt_WriterProxy.keep_all's own doc comment for what assuming NO here cost.
static enum tt_WriterKeepAll writer_announced_keep_all(struct tt_Node* node, uint8_t node_id, uint32_t endpoint_id) {
    if (node == NULL || node->discovery == NULL) {
        return tt_WRITER_KEEP_ALL_UNKNOWN;
    }
    const struct tt_DiscoveredEntity* writer = tt_Discovery_find(node->discovery, node_id, endpoint_id);
    if (writer == NULL) {
        return tt_WRITER_KEEP_ALL_UNKNOWN;
    }
    return (writer->qos & tt_UPDATE_QOS_KEEP_ALL) != 0 ? tt_WRITER_KEEP_ALL_YES : tt_WRITER_KEEP_ALL_NO;
}

// Milestone 47 - finds sub's existing WriterProxy for (node_id, entity_id), or NULL if this
// specific writer isn't currently tracked (every slot empty, or all held by other writers). Never
// claims a new slot - see find_or_create_writer_proxy() below for the create half most call sites
// actually want; this bare find is only for a caller that must *not* create one on a miss
// (inform_subscriber_of_heartbeat()'s own "is this really first contact" check).
static struct tt_WriterProxy* find_writer_proxy(struct tt_Subscriber* sub, uint8_t node_id, uint32_t entity_id) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (sub->writers[i].node_id == node_id && sub->writers[i].entity_id == entity_id) {
            return &sub->writers[i];
        }
    }
    return NULL;
}

// Milestone 47 - find_writer_proxy() above, but claims and initializes the first empty slot on a
// miss instead of returning NULL (every DATA/HEARTBEAT-driven call site below wants this). A new
// entry starts at ack_seq_no 1 (a Publisher's first sample is always seq_no 1, never 0), matching
// tt_Node_create_subscriber()'s own former up-front default - now applied lazily, per writer, the
// first time each one is actually heard from instead of once for the whole Subscriber. *out_
// created (may be NULL) reports whether this call just claimed a fresh slot, for a caller that
// needs to tell "already tracking this writer" apart from "first contact" (inform_subscriber_of_
// heartbeat()'s own first-contact branch). Returns NULL only if the table is already full and no
// matching entry exists - every call site handles that the same way a full pub->peers[]/client->
// peers[] already silently drops a peer past tt_MAX_PEER_COUNT elsewhere in this file, not a new
// failure mode.
static struct tt_WriterProxy* find_or_create_writer_proxy(struct tt_Subscriber* sub, uint8_t node_id,
                                                          uint32_t entity_id, bool* out_created) {
    struct tt_WriterProxy* proxy = find_writer_proxy(sub, node_id, entity_id);
    if (proxy != NULL) {
        if (out_created != NULL) {
            *out_created = false;
        }
        return proxy;
    }

    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (sub->writers[i].node_id == tt_NODE_ID_INVALID) {
            proxy = &sub->writers[i];
            proxy->node_id = node_id;
            proxy->entity_id = entity_id;
            proxy->sender_ip = 0;
            proxy->sender_port = 0;
            proxy->ack_seq_no = 1;
            proxy->sub = sub; // before anything that reads the window width through the proxy
            // Phase 3 - whether this writer promises KEEP_ALL, from whatever its last announce
            // said (a later announce refreshes it via update_writer_proxies_keep_all()). Unknown
            // writer, or no discovery table attached, reads as UNKNOWN, which never gives up -
            // see tt_WriterProxy.keep_all's own doc comment.
            //
            // Note what ack_seq_no was just set from, a few lines up: this proxy is being created
            // by the first DATA (or Heartbeat) that actually arrived, and its baseline is that
            // sample. Anything the writer published earlier is invisible from here - see struct
            // tt_WriterProxy's own doc comment (tickle.h) for the limitation that implies.
            proxy->keep_all = writer_announced_keep_all(sub->node, node_id, ((struct tt_Endpoint*)sub)->id);
            // Phase 2 - this slot's own window inside the Subscriber's tracking storage: the
            // caller-provided buffer when it gave one, otherwise the builtin default.
            uint64_t* tracking = sub->tracking_bitmaps != NULL ? sub->tracking_bitmaps : sub->builtin_tracking;
            proxy->received_bitmap = tracking + ((size_t)i * subscriber_tracking_words(sub));
            bitmap_clear(proxy->received_bitmap, proxy_words(proxy));
            proxy->retry = 0;
            proxy->acknack_scheduled = false;
            proxy->heartbeat_last_seq_no = 0;
            if (out_created != NULL) {
                *out_created = true;
            }
            return proxy;
        }
    }

    if (out_created != NULL) {
        *out_created = false;
    }
    return NULL; // table full - see this function's own doc comment
}

// QoS roadmap #5 (RELIABILITY) follow-up - the highest bit position (bit j: seq_no proxy->
// ack_seq_no + j needs attention, one way or another) this writer currently has *any* reason to
// ask about - received_bitmap's own highest confirmed-out-of-order bit (bitmap_highest_bit()
// above, the only signal before this follow-up existed), widened by the highest seq_no the most
// recent struct tt_HeartbeatHeader from this writer claimed the Publisher has published, if that
// reaches further. A Heartbeat can reveal the Subscriber is behind even with zero out-of-order
// DATA arrivals yet - received_bitmap alone is blind to that case, since nothing has set any bit
// in it. Shared by send_acknack() (what to actually request) and maybe_arm_acknack_retry()
// (whether there's anything to do at all) - both need the same widened answer, not just received_
// bitmap's own. -1 if neither signal has anything to report.
static int highest_relevant_bit(const struct tt_WriterProxy* proxy) {
    int highest = bitmap_highest_bit(proxy->received_bitmap, proxy_words(proxy));
    if (proxy->heartbeat_last_seq_no >= proxy->ack_seq_no) {
        uint64_t hb_offset = (uint64_t)proxy->heartbeat_last_seq_no - proxy->ack_seq_no;
        uint32_t window_bits = proxy_window_bits(proxy);
        int hb_highest = hb_offset < window_bits ? (int)hb_offset : (int)window_bits - 1;
        if (hb_highest > highest) {
            highest = hb_highest;
        }
    }
    return highest;
}

// Requests (ACKNACK "please resend" bits) only positions low_bit..high_bit relative to ack_seq_no,
// further masked to what's still missing in received_bitmap. send_acknack() below is the usual
// full-range form; update_reliable_ack() uses a narrow range for Phase 1-a's per-new-gap NACK.
static void send_acknack_range(struct tt_Node* node, struct tt_WriterProxy* proxy, int low_bit, int high_bit) {
    struct tt_Subscriber* sub = proxy->sub;
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)sub;
    struct tt_Peer target = {proxy->node_id, proxy->sender_ip, proxy->sender_port};
    uint32_t old_tx_tail = node->tx_tail;

    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_ACKNACK, target.node_id);
    if (submessage_header == NULL) {
        rollback(node, old_tx_tail);
        return;
    }

    // Phase 2 - only the words this request actually reaches into go on the wire. high_bit is the
    // highest position worth asking about (highest_relevant_bit(), or the caller's narrower range),
    // so everything above it is zero by construction and never needs sending.
    uint16_t words = proxy_words(proxy);
    uint16_t wire_words = high_bit < 0 ? 0 : (uint16_t)((high_bit / tt_RELIABLE_BITMAP_WORD_BITS) + 1);
    if (wire_words > words) {
        wire_words = words;
    }
    struct tt_AckNackHeader* acknack_header =
        encode(node, sizeof(struct tt_AckNackHeader) + ((size_t)wire_words * sizeof(uint64_t)));
    if (acknack_header == NULL) {
        rollback(node, old_tx_tail);
        return;
    }

    acknack_header->endpoint_id = endpoint->id;
    acknack_header->sender_entity_id = endpoint->entity_id; // Phase 2 - which Subscriber is acking
    acknack_header->seq_no = proxy->ack_seq_no;
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
    // highest_relevant_bit() (send_acknack()'s own high_bit - not the plain received_bitmap-only
    // bitmap_highest_bit() this comment's own numbers were found against) also considers the most
    // recent Heartbeat's own last_seq_no - QoS roadmap #5's own follow-up, struct
    // tt_HeartbeatHeader's doc comment (tickle.h) - widening the request range to cover a gap a
    // Heartbeat revealed even when nothing has arrived out of order yet to set any bit here at all.
    uint64_t request_mask[tt_RELIABLE_BITMAP_MAX_WORDS];
    uint64_t below_low_mask[tt_RELIABLE_BITMAP_MAX_WORDS];
    bitmap_low_mask(request_mask, words, high_bit);
    bitmap_low_mask(below_low_mask, words, low_bit - 1);
    acknack_header->bitmap_words = wire_words;
    acknack_header->reserved = 0;
    for (uint16_t word = 0; word < wire_words; word++) {
        acknack_header->bitmap[word] = ~proxy->received_bitmap[word] & request_mask[word] & ~below_low_mask[word];
    }
    // Milestone 47 - the *target* Publisher's own entity_id, learned from whichever WriterProxy
    // this ACKNACK answers - see struct tt_AckNackHeader.entity_id's own doc comment (tickle.h).
    acknack_header->entity_id = proxy->entity_id;

    // Unicast straight back to whoever's DATA this acks - same "nothing else queued" guard as
    // process_callrequest()'s own CallResponse. Falling back to broadcast when something else is
    // already staged is still correct here: the submessage's own receiver field (target.node_id,
    // not tt_SUBMESSAGE_ID_ALL) confines actual processing to that one node regardless of how the
    // packet physically went out.
#ifdef tt_RELIABLE_STATS
    // Copied before end_encode(): a successful flush memmoves tx_buffer, invalidating acknack_header.
    uint64_t requested[tt_RELIABLE_BITMAP_MAX_WORDS];
    uint32_t requested_base = proxy->ack_seq_no;
    for (uint16_t word = 0; word < wire_words; word++) {
        requested[word] = acknack_header->bitmap[word];
    }
#endif
    bool unicast = old_tx_tail == sizeof(struct tt_Header);
    if (!end_encode(node, submessage_header, true, unicast ? &target : NULL, unicast ? 1 : 0)) {
        rollback(node, old_tx_tail);
    }
#ifdef tt_RELIABLE_STATS
    else {
        uint64_t now = tt_get_ns();
        g_rstats.acknack_sent++;
        g_rstats.acknack_bits_sent += rstat_popcount_bitmap(requested, wire_words);
        g_rstats.acknack_bytes_sent += sizeof(struct tt_AckNackHeader) + ((size_t)wire_words * sizeof(uint64_t));
        for (uint16_t word = 0; word < wire_words; word++) {
            for (uint64_t bits = requested[word]; bits != 0; bits &= bits - 1) {
                uint32_t seq =
                    requested_base + (uint32_t)(word * tt_RELIABLE_BITMAP_WORD_BITS) + (uint32_t)__builtin_ctzll(bits);
                if (g_rstats_requested_ns[seq % RSTAT_SEQ_SLOTS] == 0) {
                    g_rstats_requested_ns[seq % RSTAT_SEQ_SLOTS] = now;
                }
            }
        }
    }
#endif
}

static void send_acknack(struct tt_Node* node, struct tt_WriterProxy* proxy) {
    send_acknack_range(node, proxy, 0, highest_relevant_bit(proxy));
}

// The reliable Subscriber's ACKNACK retry cadence - tt_RELIABLE_DEADLINE when set, otherwise
// tt_RELIABLE_RETRY_INTERVAL (config.h, Phase 1-b). Shared by acknack_retry() and
// maybe_arm_acknack_retry() so the first retry and every later one use the same interval.
static uint64_t reliable_retry_interval(void) {
    return tt_RELIABLE_DEADLINE != 0 ? (uint64_t)tt_RELIABLE_DEADLINE : (uint64_t)tt_RELIABLE_RETRY_INTERVAL;
}

// Scheduled (tt_Node_schedule()) while proxy has an outstanding gap (proxy->received_bitmap !=
// 0), re-sending the ACKNACK on a timer for the case where no further DATA ever arrives to
// re-trigger update_reliable_ack() itself. Mirrors call_retry()'s own schedule/reschedule/give-up
// shape. Scheduled against proxy's own stable address (not the owning Subscriber) so several
// writers' independent retry timers on the same Subscriber never collide - see struct tt_
// WriterProxy.sub's own doc comment for why this can still reach node/endpoint from just `proxy`.
static void acknack_retry(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(time);

    struct tt_WriterProxy* proxy = param;

    if (bitmap_is_zero(proxy->received_bitmap, proxy_words(proxy))) {
        // A DATA arrival already closed the gap since this timer was armed.
        proxy->acknack_scheduled = false;
        return;
    }

    // Phase 3 - a KEEP_ALL writer never gives up, and neither may this Subscriber: abandoning the
    // gap here would advance ack_seq_no past a sample that was never received, unblocking that
    // writer as if it had been delivered. retry keeps counting for the stuck-gap warning below.
    //
    // Phase 3 step 4 - and a writer whose policy isn't known yet is treated the same way, because
    // abandoning data is not a decision to take on an assumption. This cannot retry forever: if the
    // writer genuinely no longer has the sample, its answer to the next ACKNACK is Phase 1-c's
    // eviction Heartbeat, whose first_available_seq_no makes advance_past_unavailable() skip
    // exactly the range that is really gone - so the worst case here is recovery delayed until the
    // writer's announce arrives, not an endless retry. That safety net is what makes refusing to
    // guess safe; without it this would need a grace period instead.
    proxy->retry++; // unconditionally now, so the stuck-gap warning below can report a real count
                    // in the two cases that never give up (it used to be short-circuited away)
    if (proxy->keep_all == tt_WRITER_KEEP_ALL_UNKNOWN && proxy->retry == tt_RELIABLE_RETRY + 1) {
        // Counted once per gap, at the point the bounded policy would have abandoned it, so this
        // reads directly against retry_giveups rather than tallying every later retry too.
        RSTAT_INC(giveups_suppressed_unknown);
    }
    if (proxy->keep_all == tt_WRITER_KEEP_ALL_NO && proxy->retry > tt_RELIABLE_RETRY) {
        TT_LOG_WARNING("Giving up on a reliable sample after %d ACKNACK retries", tt_RELIABLE_RETRY);
        RSTAT_INC(retry_giveups);
        proxy->acknack_scheduled = false;
        // Give up on ack_seq_no itself - the same "advance past it" advance_ack_seq_no() already
        // does for a real receipt, since from here on it makes no difference *why* nothing more
        // is waiting on it. A different, still-outstanding gap further ahead in the window (if
        // any) is untouched - it gets its own full tt_RELIABLE_RETRY budget against whatever
        // ack_seq_no ends up being next (advance_ack_seq_no()'s own reset of retry is what
        // actually grants that fresh budget) - and, unlike leaving it to the next DATA arrival to
        // notice, maybe_arm_acknack_retry() below starts requesting it immediately.
        advance_ack_seq_no(proxy);
        // No bulk skip of the rest here any more (Phase 1-c removed skip_unrecoverable_backlog()
        // and its compile-time tt_MAX_RELIABLE_HISTORY guess at the Publisher's depth, which threw
        // away samples a deeper cache still held). What's genuinely gone is now signalled
        // explicitly: every ACKNACK that names an evicted sample gets an eviction Heartbeat back,
        // and advance_past_unavailable() skips exactly that range.
        maybe_arm_acknack_retry(node, proxy);
        return;
    }

    RSTAT_INC(acknack_timer);
    send_acknack(node, proxy);

    // Phase 3 - with a KEEP_ALL writer there is no give-up, so a genuinely stuck gap would
    // otherwise be silent. Rate-limited by time rather than retry count, so the cadence doesn't
    // change with the retry interval.
    uint64_t now = tt_get_ns();
    if (proxy->keep_all != tt_WRITER_KEEP_ALL_NO && now - proxy->stuck_warned_ns >= tt_RELIABLE_STUCK_WARN_INTERVAL) {
        proxy->stuck_warned_ns = now;
        TT_LOG_WARNING("Still waiting on reliable seq_no %u from node %d after %u retries (%s: no give-up)",
                       proxy->ack_seq_no, proxy->node_id, proxy->retry,
                       proxy->keep_all == tt_WRITER_KEEP_ALL_YES ? "KEEP_ALL" : "policy not yet known");
    }

    if (!tt_Node_schedule(node, tt_get_ns() + reliable_retry_interval(), acknack_retry, proxy)) {
        TT_LOG_ERROR("Cannot schedule acknack_retry");
        proxy->acknack_scheduled = false;
    }
}

// Confirms proxy->ack_seq_no itself (whether just received, or - acknack_retry()'s own call site
// - given up on after too many retries) and advances past it, keeping received_bitmap correctly
// realigned: bit j always means "received(ack_seq_no + j)", matching tt_AckNackHeader's own wire
// convention exactly (see struct tt_WriterProxy's own doc comment, tickle.h) - which is why this
// shifts once *unconditionally* for this first step, not only inside the while loop below. A
// previous version only shifted inside the while loop, silently misaligning every subsequent
// bit's meaning by one position after any exact-match advance - found via run_perf.sh's real
// tc/netem loss-injection scenarios reporting RELIABLE recovering only partially, not because
// retransmission itself was failing, but because the ACKNACK requests it was reacting to were
// silently asking for the wrong sequence numbers (already-received ones) while dropping the
// genuinely still-missing one off the request entirely.
//
// Also resets retry to 0 unconditionally - whatever is now the oldest outstanding gap (if any
// remain - received_bitmap may still be nonzero here) is a *different* sample than the one retry
// was counting attempts against, and deserves its own full tt_RELIABLE_RETRY budget, not whatever
// was left over. Before this reset lived here, two losses close enough together that a second gap
// was still open when the first resolved would make the second one inherit however many attempts
// the first had already used - found the same way as the bitmap bug above: real tc/netem
// loss-injection runs recovering measurably worse than p^(tt_RELIABLE_RETRY + 1) predicts for a
// single isolated loss.
static void advance_ack_seq_no(struct tt_WriterProxy* proxy) {
    proxy->ack_seq_no++;
    bitmap_shift_right_one(proxy->received_bitmap, proxy_words(proxy));
    while (bitmap_lowest_bit_set(proxy->received_bitmap)) { // absorb whatever out-of-order run already follows it
        bitmap_shift_right_one(proxy->received_bitmap, proxy_words(proxy));
        proxy->ack_seq_no++;
    }
    proxy->retry = 0;
}

// Shared by update_reliable_ack() (a new/changed gap), process_heartbeat() (a Heartbeat revealing
// one even with received_bitmap still 0), and acknack_retry()'s own give-up path (the gap just
// written off might not have been the only one outstanding): unschedules cleanly once nothing is
// left to ask for, or sends an ACKNACK for whatever's still missing and (re-)arms the retry timer
// if one isn't already running. Splitting this out means a give-up no longer leaves a remaining,
// different gap waiting on the next DATA arrival before anything asks for it again.
static void maybe_arm_acknack_retry(struct tt_Node* node, struct tt_WriterProxy* proxy) {
    if (highest_relevant_bit(proxy) < 0) {
        // No outstanding gap by either signal (received_bitmap or the last Heartbeat) - a healthy
        // stream needs no ACKNACK at all.
        if (proxy->acknack_scheduled) {
            tt_Node_unschedule(node, acknack_retry, proxy);
            proxy->acknack_scheduled = false;
        }
        proxy->retry = 0;
        return;
    }

    if (!proxy->acknack_scheduled) {
        // Real bug found via direct HIL instrumentation (2026-09-22): this function runs once per
        // DATA arrival (update_reliable_ack()'s own unconditional call at the end of every DATA it
        // processes), not just once per gap - send_acknack() used to sit *outside* this guard and
        // fire unconditionally on every single call while any gap remained open, completely
        // bypassing the retry-interval pacing acknack_retry()'s own timer is supposed to
        // provide. At TickLE's real max throughput this produced an ACKNACK flood (measured: ~70K
        // ACKNACKs/sec from one WriterProxy, vs. a handful from the actual 5ms retry timer) that
        // starved both ends' own CPU/network budget - the real root cause of reliable_throughput's
        // own newly-severe loss regression once RELIABLE tracking became genuinely active (see
        // PLAN.md). Send exactly once here, on first detecting a new gap - every subsequent
        // update while this proxy already has a retry armed relies purely on acknack_retry()'s own
        // periodic re-send (which already re-reads the current bitmap state fresh each tick, so
        // nothing about a widened gap goes unreported, just delayed by at most one interval).
        RSTAT_INC(acknack_immediate);
        send_acknack(node, proxy);
        proxy->retry = 0;
        if (tt_Node_schedule(node, tt_get_ns() + reliable_retry_interval(), acknack_retry, proxy)) {
            proxy->acknack_scheduled = true;
        } else {
            TT_LOG_ERROR("Cannot schedule acknack_retry");
        }
    }
}

// Jumps proxy's own baseline straight to seq_no instead of trying to track anything below it -
// shared by update_reliable_ack()'s own oversized-DATA-gap branch and process_heartbeat()'s own
// oversized-Heartbeat-gap case (PLAN.md's Milestone 20 and its own Heartbeat follow-up
// respectively): an offset >= tt_RELIABLE_BITMAP_BITS can never be named in a tt_AckNackHeader.
// bitmap at all (tt_RELIABLE_BITMAP_BITS bits wide on the wire, config.h), so nothing genuinely
// recoverable is given up on by not tracking it - see update_reliable_ack()'s own call site for
// the full "why" comment, not repeated here.
static void jump_ack_baseline(struct tt_WriterProxy* proxy, uint32_t seq_no) {
#ifdef tt_RELIABLE_STATS
    if (seq_no > proxy->ack_seq_no) {
        uint64_t span = (uint64_t)seq_no - proxy->ack_seq_no;
        uint64_t received = rstat_popcount_bitmap(proxy->received_bitmap, proxy_words(proxy));
        g_rstats.jump_abandoned_seq += span > received ? span - received : 0;
    }
#endif
    proxy->ack_seq_no = seq_no;
    bitmap_clear(proxy->received_bitmap, proxy_words(proxy));
    advance_ack_seq_no(proxy);
}

// Bit positions (relative to ack_seq_no) of a gap one DATA arrival just revealed - positions above
// the previously highest received bit and below the arrival itself. low_bit < 0 means none.
struct new_gap_range {
    int low_bit;
    int high_bit;
};

// update_reliable_ack()'s in-window (0 < offset < tt_RELIABLE_BITMAP_BITS) arrival: records it in
// received_bitmap and reports any gap it newly opened via *new_gap. Returns false for a duplicate
// (its bit was already set), true otherwise.
static bool record_out_of_order_arrival(struct tt_WriterProxy* proxy, uint32_t offset, struct new_gap_range* new_gap) {
    if (bitmap_test_bit(proxy->received_bitmap, offset)) {
        RSTAT_INC(duplicates);
        return false; // already received this one out of order before - a duplicate
    }
    int prev_highest = bitmap_highest_bit(proxy->received_bitmap, proxy_words(proxy));
    if ((int)offset > prev_highest + 1) {
        new_gap->low_bit = prev_highest + 1;
        new_gap->high_bit = (int)offset - 1;
    }
    bitmap_set_bit(proxy->received_bitmap, offset);
    return true;
}

#ifdef tt_RELIABLE_STATS
// Classifies one DATA arrival (seq_no >= proxy->ack_seq_no) before update_reliable_ack() touches
// received_bitmap: filling an already-tracked gap (below the highest seq_no received so far) counts
// as recovered; landing above it opens a gap for every position skipped in between. Bit j of
// received_bitmap means "received(ack_seq_no + j)", and ack_seq_no itself is always still missing.
static void rstat_on_arrival(const struct tt_WriterProxy* proxy, uint32_t seq_no) {
    uint64_t offset = (uint64_t)seq_no - proxy->ack_seq_no;
    int prev_highest = bitmap_highest_bit(proxy->received_bitmap, proxy_words(proxy));
    if (offset == 0) {
        if (prev_highest >= 0) {
            rstat_recovered(seq_no); // head of a tracked gap - something above it already arrived
        }
        return;
    }
    if (offset >= proxy_window_bits(proxy) || bitmap_test_bit(proxy->received_bitmap, (uint32_t)offset)) {
        return; // jump or duplicate - counted at their own sites
    }
    if ((int)offset < prev_highest) {
        rstat_recovered(seq_no);
        return;
    }
    uint32_t newly_missing = (uint32_t)((int)offset - prev_highest - 1);
    if (newly_missing == 0) {
        return;
    }
    g_rstats.gaps_opened++;
    g_rstats.missing_opened += newly_missing;
    if (proxy->acknack_scheduled) {
        g_rstats.gaps_opened_while_scheduled++;
    }
    rstat_mark_missing(proxy->ack_seq_no + (uint32_t)(prev_highest + 1), newly_missing, tt_get_ns());
}
#define RSTAT_ON_ARRIVAL(proxy, seq_no) rstat_on_arrival((proxy), (seq_no))
#else
#define RSTAT_ON_ARRIVAL(proxy, seq_no) ((void)0)
#endif

// QoS roadmap #5 (RELIABILITY/RELIABLE) - called from process_data() for every DATA a reliable
// Subscriber receives (no-op otherwise). Finds or claims sub's own WriterProxy for (sender_node_
// id, sender_entity_id) - Milestone 47's own composite writer identity, see struct tt_
// WriterProxy's own doc comment for why this is no longer a single flat watermark - and updates
// its cumulative-ack watermark/out-of-order bitmap, keeping an ACKNACK flowing back to the sender
// while a gap is open.
//
// Milestone 60 (rmw_tickle/PLAN.md) - returns whether seq_no was genuinely new to this Subscriber,
// so deliver_data_to_subscriber() can skip re-invoking the application callback for a sample it
// already delivered (a legitimate ACKNACK-driven retransmit racing the original, or a stale
// duplicate arriving again) - real DDS readers de-duplicate by (writer GUID, sequence number)
// exactly this way; TickLE previously had no equivalent gate at all. A best-effort Subscriber
// (!sub->reliable, below) has no per-writer tracking to de-duplicate against and no retransmission
// mechanism to ever legitimately produce a duplicate in the first place - matches real DDS
// BEST_EFFORT's own identical non-guarantee, always "new".
//
// Deliberately narrow: only a bit *already set* in received_bitmap (below) is trustworthy evidence
// of "already delivered" - seq_no < ack_seq_no alone is NOT, once jump_ack_baseline() has ever
// fired for this proxy. A first attempt also treated seq_no < ack_seq_no as an always-duplicate
// case and reproduced a severe, real regression caught by real CI (Performance Test's own
// "reliable recv throughput" benchmark: a consistent ~820 Mbps across the prior 7 pushes on this
// exact rig collapsed to 66.276 Mbps, a 12.38x drop flagged by github-action-benchmark, with the
// receiver's own total_received_msgs - one increment per bulk_callback() invocation, examples/
// linux/perf/perf_server.c - capped at exactly 65,536 while the wire-level best-effort throughput
// test in the very same run stayed a normal ~900 Mbps). Root cause: jump_ack_baseline() (called
// when a gap exceeds tt_RELIABLE_BITMAP_BITS, plausible under real reordering at near-line-rate,
// not just genuine loss) *abandons* tracking for everything below its own jump point rather than
// confirming it was actually received - unlike the ordinary exact-match/bitmap-absorption path,
// where advancing past a position always means it genuinely arrived. Once a jump happens, every
// still-in-flight (merely reordered, never lost) sample from the abandoned range legitimately
// still arrives with seq_no < ack_seq_no and deserves delivery - "reliable only adds a guarantee
// via retransmission, not ordering" (deliver_data_to_subscriber()'s own older comment) - but the
// first attempt here treated all of them as duplicates, silently dropping the vast majority of a
// saturated stream's own genuinely-new samples. A bit in received_bitmap has no equivalent
// ambiguity: jump_ack_baseline() itself zeroes received_bitmap on every jump, so a set bit is
// always fresh, current-window evidence a sample was actually received - never contaminated by an
// abandoned range. This narrower gate no longer catches an already-cumulatively-passed-watermark
// duplicate (an accepted, narrow miss, same category as this file's other honest residuals) but
// carries no risk of misclassifying a genuinely new sample - the trade-off deliberately made in
// the safer direction after the wider version's own real-CI-confirmed failure.
static bool update_reliable_ack(struct tt_Node* node, struct tt_Subscriber* sub, uint32_t seq_no,
                                uint8_t sender_node_id, uint32_t sender_entity_id, uint32_t sender_ip,
                                uint16_t sender_port) {
    if (!sub->reliable) {
        return true;
    }

    bool first_contact = false;
    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(sub, sender_node_id, sender_entity_id, &first_contact);
    if (proxy == NULL) {
        return true; // WriterProxy table full - see find_or_create_writer_proxy()'s own doc comment,
                     // nothing to track against, deliver as-is same as always
    }

    proxy->sender_ip = sender_ip;
    proxy->sender_port = sender_port;

    if (first_contact) {
        // Milestone 60 - RELIABLE+VOLATILE DDS-parity fix, mirrors inform_subscriber_of_heartbeat()'s
        // own identical first-contact branch (its own doc comment: "the actual DDS-parity fix this
        // whole follow-up is for"). DATA can legally win the race against the discovery-triggered
        // initial Heartbeat (send_initial_heartbeat(), unicast and never itself retried/acked)
        // arriving first, especially under loss injection - without this, first contact via DATA
        // fell through to the offset-based gap logic below starting from the stale ack_seq_no==1
        // default, misreading "everything before this first sample" as a recoverable in-flight gap
        // and ACKNACK-requesting a VOLATILE Publisher's own pre-match history it was never
        // obligated to keep - re-deriving the exact bug the Heartbeat-first path already fixed, any
        // time DATA happened to win that race instead. A reordering-at-first-contact edge case (an
        // earlier backlog sample lost in flight while a later one wins the race here) is an
        // accepted, narrow residual, same category as Milestone 47's own honest residuals - the
        // Heartbeat-first path (when it wins the race instead) still catches it correctly via its
        // own first_available_seq_no, unaffected by this branch.
        proxy->ack_seq_no = seq_no;
    }

    if (seq_no < proxy->ack_seq_no) {
        RSTAT_INC(late_below_ack);
        return true; // old relative to the ack watermark, but NOT reliable evidence of "already
                     // delivered" once jump_ack_baseline() has ever fired for this proxy - see this
                     // function's own doc comment for the real regression this specific case caused
                     // when it used to return false here. Ack-tracking has nothing further to do
                     // for it either way (unchanged from before this milestone).
    }

    RSTAT_ON_ARRIVAL(proxy, seq_no);
    struct new_gap_range new_gap = {-1, -1};
    bool is_new = true;
    if (seq_no == proxy->ack_seq_no) {
        advance_ack_seq_no(proxy);
    } else {
        // NOTE: deliberately *not* fast-forwarding past a wide gap here, on every arrival that's
        // far ahead of ack_seq_no - an earlier version of this branch did, and it backfired: once
        // steady, in-order arrivals resume after ack_seq_no falls behind, each new one re-triggers
        // the same "too far ahead" check, since ack_seq_no is dragged along exactly
        // tt_MAX_RELIABLE_HISTORY - 1 behind the latest arrival forever - which preempts the very
        // retry cycle (acknack_retry()) that might have recovered ack_seq_no itself, over and
        // over, instead of ever letting it actually resolve. Skipping only belongs where it's
        // known to be right: the Publisher's own eviction Heartbeat (advance_past_unavailable()),
        // or acknack_retry()'s give-up after ack_seq_no's fair tt_RELIABLE_RETRY attempts.
        uint64_t offset = (uint64_t)seq_no - proxy->ack_seq_no;
        if (offset < proxy_window_bits(proxy)) {
            is_new = record_out_of_order_arrival(proxy, (uint32_t)offset, &new_gap);
        } else {
            // Unlike the "far ahead but still inside the tracking window" case this function's
            // own comment above warns against fast-forwarding on, an offset this wide (>=
            // tt_RELIABLE_BITMAP_BITS) can *never* be requested at all - tt_AckNackHeader.bitmap
            // is a fixed tt_RELIABLE_BITMAP_BITS bits wide on the wire (config.h), so no ACKNACK
            // this Subscriber could ever send has a way to name a position past the top bit in the
            // first place, regardless of what ack_seq_no does about it. Leaving ack_seq_no
            // untouched here (this function's own behavior before PLAN.md's Milestone 20)
            // permanently wedges it: every later arrival, however perfectly in-order from this
            // point on, has the exact same too-wide offset relative to the still-stuck ack_seq_no,
            // forever - a healthy stream never recovers. Most visible for a QoS roadmap #4
            // (DURABILITY) backlog delivered to a brand-new Subscriber whose default ack_seq_no (1)
            // starts arbitrarily far behind a Publisher that's been running a while, but applies
            // equally to a RELIABLE-only stream that takes one real burst loss wider than the
            // bitmap - jump the baseline to this arrival instead,
            // the same "give up on what's provably unrecoverable, keep the stream moving" logic
            // advance_past_unavailable() applies on an eviction Heartbeat, just applied here the
            // instant it's already known un-trackable rather than after wasting a retry cycle
            // chasing a position that could never have been named on the wire.
            TT_LOG_WARNING("Reliable gap too large to track (%u ahead of %u) - jumping ahead instead of getting stuck",
                           seq_no - proxy->ack_seq_no, proxy->ack_seq_no);
            RSTAT_INC(jump_data);
            jump_ack_baseline(proxy, seq_no);
        }
    }

    // Phase 1-a (rmw_tickle/PLAN.md, H2) - maybe_arm_acknack_retry() only sends an immediate
    // ACKNACK when no retry timer is armed yet; a gap that opens while one already is used to wait
    // for that timer (then tt_CALL_RETRY_INTERVAL, 5ms). At max rate the 256-sample window moves on in
    // ~1.35ms, so those gaps were always abandoned by jump_ack_baseline() before the timer fired
    // (HIL 0-c: ~48% of gaps at 1% tc loss, ~91% at 5%, all lost). NACK the new gap immediately -
    // and *only* its own positions: re-requesting every still-open gap here would re-send samples
    // whose retransmit is already in flight and burn the Publisher's per-sample retry budget
    // (find_resendable_cache_entry()'s tt_RELIABLE_RETRY cap) on duplicates. Bounded by one
    // ACKNACK per loss event (a new gap needs at least one genuinely missing position), not one per
    // DATA arrival - unlike the ACKNACK flood maybe_arm_acknack_retry()'s own comment describes.
    bool retry_already_armed = proxy->acknack_scheduled;
    maybe_arm_acknack_retry(node, proxy);
    if (retry_already_armed && new_gap.low_bit >= 0) {
        RSTAT_INC(acknack_new_gap);
        send_acknack_range(node, proxy, new_gap.low_bit, new_gap.high_bit);
    }
    return is_new;
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
        update_entity->entity_id = endpoint->entity_id; // Phase 2 - which instance, not just which topic
        update_entity->kind = endpoint->kind;
        update_entity->qos = endpoint_qos_bits(endpoint);
        // Phase 2 - a Subscriber announces the window it can actually track; everything else
        // announces 0 ("the default"), see tt_UpdateEntity.tracking_words' own doc comment.
        update_entity->tracking_words = endpoint->kind == tt_KIND_TOPIC_SUBSCRIBER
                                            ? subscriber_tracking_words((const struct tt_Subscriber*)endpoint)
                                            : 0;
        update_entity->deadline_duration_ns = endpoint_deadline_duration_ns(endpoint);
        update_entity->liveliness_lease_duration_ns = endpoint_liveliness_lease_duration_ns(endpoint);

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

// Phase 3 (rmw_tickle/PLAN.md) - records whether a remote writer promises KEEP_ALL on every local
// Subscriber already tracking it, from that writer's own announce (tt_UPDATE_QOS_KEEP_ALL). A
// proxy claimed later reads the same thing from the discovery table at first contact, so both
// orderings converge.
static void update_writer_proxies_keep_all(struct tt_Node* node, uint32_t endpoint_id, uint8_t node_id,
                                           uint32_t entity_id, enum tt_WriterKeepAll keep_all) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_SUBSCRIBER || endpoint->id != endpoint_id) {
            continue;
        }
        struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
        for (int j = 0; j < tt_MAX_PEER_COUNT; j++) {
            if (sub->writers[j].node_id == node_id && sub->writers[j].entity_id == entity_id) {
                sub->writers[j].keep_all = keep_all;
            }
        }
    }
}

// Phase 3 (rmw_tickle/PLAN.md) - drops one remote Publisher's WriterProxy from every local
// Subscriber sharing `endpoint_id`: the Subscriber-side mirror of
// forget_publisher_peers_for_endpoint() below, and the only thing that ends gap recovery for a
// writer that died. entity_id 0 with match_any_entity drops every writer that node hosts for this
// topic (the node itself departed); otherwise just the one entity.
//
// Without this, a KEEP_ALL writer (tt_UPDATE_QOS_KEEP_ALL, which switches off acknack_retry()'s own
// tt_RELIABLE_RETRY give-up) that vanished mid-gap would leave its Subscriber re-requesting the
// same samples every retry interval forever, unicast at an address nobody answers - and the slot
// would never free for a restarted Publisher. A KEEP_LAST writer only leaked a bounded number of
// retries, which is why this was survivable before.
static void forget_writer_proxies_for_endpoint(struct tt_Node* node, uint32_t endpoint_id, uint8_t node_id,
                                               uint32_t entity_id, bool match_any_entity) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_SUBSCRIBER || endpoint->id != endpoint_id) {
            continue;
        }
        struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
        for (int j = 0; j < tt_MAX_PEER_COUNT; j++) {
            struct tt_WriterProxy* proxy = &sub->writers[j];
            if (proxy->node_id != node_id) {
                continue;
            }
            if (!match_any_entity && proxy->entity_id != entity_id) {
                continue;
            }
            if (proxy->acknack_scheduled) {
                tt_Node_unschedule(node, acknack_retry, proxy);
                proxy->acknack_scheduled = false;
            }
            proxy->node_id = tt_NODE_ID_INVALID; // frees the slot; a restart re-runs first contact
            proxy->entity_id = 0;
            proxy->ack_seq_no = 1;
            proxy->heartbeat_last_seq_no = 0;
            proxy->retry = 0;
            proxy->keep_all = false;
            if (proxy->received_bitmap != NULL) {
                bitmap_clear(proxy->received_bitmap, proxy_words(proxy));
            }
            RSTAT_INC(proxies_dropped_liveliness);
        }
    }
}

// Milestone 62 follow-up - tt_Node_entity_alive()'s own per-entity-lease freshness (tickle.h)
// sharpens the discovery_callback(departed=true) signal for entities that requested a lease
// shorter than the loop above's fixed ~3s node-wide wait: without this, a leased entity's
// departure was only ever reported at that coarse node-level mark, same as an unleased one,
// silently discarding the lease it asked for. Runs every tick (this function's own caller already
// runs every tt_NODE_UPDATE_INTERVAL, ~1s) so a short-leased entity is tombstoned - and the
// callback fires - within about one tick of its own lease boundary instead of always the ~3s one.
// Entities with no lease (liveliness_lease_duration_ns == 0) are skipped here - tt_Node_entity_
// alive() itself defers those to .alive, which only the loop above (or a real farewell) changes,
// so behavior for them is unchanged. No-op if no discovery cache is attached.
static void tombstone_entities_past_own_lease(struct tt_Node* node, uint64_t time) {
    if (node->discovery == NULL) {
        return;
    }

    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        struct tt_DiscoveredEntity* entity = &entities[i];
        if (entity->node_id == tt_NODE_ID_INVALID || !entity->alive || entity->liveliness_lease_duration_ns == 0) {
            continue;
        }
        if (tt_Node_entity_alive(node, entity, time)) {
            continue;
        }
        entity->alive = false;
        // Phase 3 prerequisite (a), rmw_tickle/PLAN.md - a remote Subscriber presumed dead by its
        // own announced lease must also leave the matching local Publishers' peer/ack sets right
        // here. Before this, only check_liveliness()'s own node-level sweep (~3-3.6s, and only when
        // the whole node goes quiet) did that, so a crashed Subscriber kept its ack entry for
        // seconds after its lease expired - which under Phase 3's KEEP_ALL blocking would stall a
        // writer that is waiting on exactly that ack. Narrow on purpose: only the Publishers whose
        // own endpoint id this entity matched, and only this node_id, unlike the node-level sweep.
        if (entity->kind == tt_KIND_TOPIC_SUBSCRIBER) {
            forget_publisher_peers_for_endpoint(node, entity->endpoint_id, entity->node_id);
        } else if (entity->kind == tt_KIND_TOPIC_PUBLISHER) {
            // Phase 3 - the mirror case: a remote *Publisher* past its own lease stops being
            // something our Subscribers can still recover from, so its WriterProxy goes too.
            forget_writer_proxies_for_endpoint(node, entity->endpoint_id, entity->node_id, /*entity_id=*/0,
                                               /*match_any_entity=*/true);
        }
        if (node->discovery_callback != NULL) {
            node->discovery_callback(node, entity->node_id, entity->endpoint_id, entity->kind, /*departed=*/true,
                                     node->discovery_callback_param);
        }
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
// limitation. tombstone_entities_past_own_lease() (above) is this function's own per-entity-lease
// counterpart - same failure concept, finer timing for entities that asked for it.
static void check_liveliness(struct tt_Node* node, uint64_t time, void* param) {
    UNUSED(param);

    for (int i = 0; i < tt_MAX_ENDPOINT_COUNT; i++) {
        if (!node->update_seen[i]) {
            continue; // never heard from this node id at all - nothing to expire
        }
        if (time - node->update_last_seen[i] > (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL) {
            TT_LOG_WARNING("Node %d presumed dead (no UPDATE for %d consecutive intervals)", i,
                           tt_LIVELINESS_MISS_THRESHOLD);
            forget_peers_from_source(node, (uint8_t)i, /*preserve_ack=*/false);
            tombstone_discovered_entities_from_source(node, (uint8_t)i);
            node->update_seen[i] = false;
            node->update_last_modified[i] = 0;
            node->update_last_seen[i] = 0;
        }
    }

    tombstone_entities_past_own_lease(node, time);

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
    uint16_t depth = reliable_cache_depth(cache);
    if (depth == 0) {
        return; // index[]/capacity/arena never set up - nothing to deliver from
    }

    // B1 - walk the retained range in sequence order, oldest first (a late joiner must receive the
    // backlog in the order it was published). A slot that no longer holds its own seq_no is a
    // tombstone (evicted, or never cached because the sample was larger than the whole arena) and
    // is skipped, same as an empty slot always was.
    for (uint32_t seq_no = cache->oldest_seq_no; seq_no != 0 && seq_no <= cache->newest_seq_no; seq_no++) {
        struct tt_ReliableCacheIndex* cache_entry = reliable_cache_slot(cache, depth, seq_no);
        if (cache_entry->len == 0 || cache_entry->seq_no != seq_no) {
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
        _tt_memcpy(buf, cache->arena + cache_entry->offset, cache_entry->len);
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

// Milestone 35 - for_each_endpoint()'s own visitor context for registering one remote peer
// (learned from an UPDATE announce) against every local Publisher sharing the announced topic
// name - not just the first, now that more than one may exist (add_endpoint_to_node()'s own doc
// comment). qos is update_entity->qos verbatim (already native-endian - a single byte-ish
// bitfield, no rd*() needed, matching the original inline code this was lifted from).
struct update_peer_ctx {
    struct tt_Header* header;
    uint32_t sender_ip;
    uint16_t sender_port;
    uint16_t qos;
    // QoS roadmap #2 (DEADLINE) / #3 (LIVELINESS) RxO, Milestone 49 - the remote Subscriber's own
    // requested deadline/liveliness-lease durations, rd64()'d in decode_update_entities() the same
    // way qos above is already native-endian by the time it lands here.
    uint64_t deadline_duration_ns;
    uint64_t liveliness_lease_duration_ns;
    // Milestone 58 - the announcing node's own tt_Node.last_modified as of this UPDATE (process_
    // update()'s own already-decoded last_modified, threaded down through decode_update_entities()).
    // Phase 2 - the announcing entity's own entity_id and, for a Subscriber, the RELIABLE tracking
    // window it announced (tt_UpdateEntity.tracking_words). Both unused by
    // register_server_peer_on_client().
    uint32_t entity_id;
    uint16_t tracking_words;
    // Unused by register_server_peer_on_client() (Clients/Servers have no durability concept), only
    // meaningful to register_subscriber_peer_on_publisher()'s own durable_delivered[] check below.
    uint64_t announce_last_modified;
};

static void register_subscriber_peer_on_publisher(struct tt_Node* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    struct update_peer_ctx* ctx = (struct update_peer_ctx*)ctx_ptr;
    struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;

    // QoS roadmap #1 (RxO matching, Milestone 31) / #2 (DEADLINE) / #3 (LIVELINESS, Milestone 49) -
    // a remote Subscriber requesting a policy this local Publisher doesn't offer never becomes a
    // peer at all: no unicast optimization, no durability backlog, no discovery-triggered
    // Heartbeat - matching real DDS's own "an incompatible pair simply never connects" semantics.
    // See process_data()'s own subscriber_incompatible_with_publisher() for this check's own
    // mirror image on the Subscriber side (the more consequential half, since it's what actually
    // stops broadcast DATA delivery too - this Publisher-side half alone only gates the unicast-
    // only enhancements, tickle.c's own doc comment on tt_UPDATE_QOS_RELIABLE/_DURABLE explains
    // why both halves are needed).
    bool requested_reliable = (ctx->qos & tt_UPDATE_QOS_RELIABLE) != 0;
    bool requested_durable = (ctx->qos & tt_UPDATE_QOS_DURABLE) != 0;
    bool requested_manual = (ctx->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0;
    bool incompatible =
        (requested_reliable && !pub->reliable) || (requested_durable && !pub->durable) ||
        deadline_liveliness_incompatible(ctx->deadline_duration_ns, pub->deadline_duration_ns, requested_manual,
                                         pub->liveliness_manual, ctx->liveliness_lease_duration_ns,
                                         pub->liveliness_lease_duration_ns);
    if (incompatible) {
        return;
    }

    // Phase 2 (rmw_tickle/PLAN.md, prerequisite (b)) - claim this Subscriber entity's own ack entry
    // up front, so a matched-but-still-silent Subscriber already counts as "hasn't acked anything"
    // rather than being invisible until its first ACKNACK. A full table refuses the match outright
    // (the Subscriber simply doesn't connect to this Publisher, exactly like an incompatible QoS
    // pair) instead of matching a Subscriber whose acks could never be counted - under Phase 3's
    // KEEP_ALL blocking that would unblock a writer early, i.e. silent loss.
    if (pub->reliable && ctx->entity_id != 0) {
        struct tt_PeerAck* ack = claim_peer_ack(pub, ctx->header->source, ctx->entity_id);
        if (ack == NULL) {
            TT_LOG_WARNING("Ack table full (%d entries) - not matching Subscriber %08x on node %d", tt_MAX_ACK_ENTRIES,
                           ctx->entity_id, ctx->header->source);
            return;
        }
        // Phase 2 - remember how wide a gap this Subscriber can still ask about, so
        // tt_Publisher_unacked_bound() (Phase 3's KEEP_ALL bound) is the minimum across them.
        bool newly_claimed = ack->tracking_words != ctx->tracking_words;
        ack->tracking_words = ctx->tracking_words;

        // Real HIL finding (Phase 2, 2026-09-23): a window *wider* than this Publisher's own
        // retained depth is not merely useless, it's worse than a narrower one. The Subscriber
        // keeps asking for samples this Publisher has already evicted, so every one of them is
        // answered with an eviction Heartbeat and skipped - measured with depth 1024: window 1024
        // lost nothing across 6 runs, window 4096 lost 191-368 per run, and the lost count equalled
        // null_evicted exactly. Warn once per matching rather than per announce.
        uint32_t announced_bits = (uint32_t)ctx->tracking_words * tt_RELIABLE_BITMAP_WORD_BITS;
        uint16_t depth = reliable_cache_depth(pub->reliable_cache);
        if (newly_claimed && announced_bits > depth && depth > 0) {
            TT_LOG_WARNING("Subscriber %08x on node %d tracks %u samples, deeper than this Publisher retains (%u) - "
                           "the excess can only ever be skipped, not recovered",
                           ctx->entity_id, ctx->header->source, announced_bits, depth);
        }
    }

    if (upsert_peer(pub->peers, ctx->header->source, ctx->sender_ip, ctx->sender_port)) {
        struct tt_Peer target = {ctx->header->source, ctx->sender_ip, ctx->sender_port};
        // Milestone 58 - skip a redundant backlog re-delivery when this "genuinely new" peer slot
        // (check_liveliness()'s own presumed-dead cleanup, not necessarily a real departure - see
        // struct tt_DurableDeliveryRecord's own doc comment, tickle.h) already received this exact
        // announce's backlog. A real process restart lands on a different announce_last_modified
        // (durable_delivered_get() returns false), so it still gets delivered as usual.
        bool tracks_durable_delivery = pub->durable && pub->reliable_cache != NULL;
        bool already_delivered =
            tracks_durable_delivery &&
            durable_delivered_get(pub->reliable_cache, ctx->header->source, ctx->announce_last_modified);
        if (!already_delivered) {
            deliver_durability_backlog(node, pub, &target);
            if (tracks_durable_delivery) {
                durable_delivered_upsert(pub->reliable_cache, ctx->header->source, ctx->announce_last_modified);
            }
        }
        send_initial_heartbeat(node, pub, &target);
    }
}

// Milestone 35 - the SERVICE_SERVER-side mirror of register_subscriber_peer_on_publisher() above:
// every local Client sharing the announced service name learns this remote Server as a peer, not
// just the first. No QoS compatibility gate here (Clients/Servers have no RELIABLE/DURABLE
// policy to negotiate the way topics do), matching the original inline code this was lifted from.
static void register_server_peer_on_client(struct tt_Node* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    UNUSED(node);
    struct update_peer_ctx* ctx = (struct update_peer_ctx*)ctx_ptr;
    struct tt_Client* client = (struct tt_Client*)endpoint;
    upsert_peer(client->peers, ctx->header->source, ctx->sender_ip, ctx->sender_port);
}

static bool decode_update_entities(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t* head,
                                   uint32_t tail, int entity_count, uint32_t sender_ip, uint16_t sender_port,
                                   uint64_t last_modified) {
    bool reverse = tt_is_reverse_endian(header);
    for (int i = 0; i < entity_count && *head + sizeof(struct tt_UpdateEntity) + (2 * sizeof(uint16_t)) < tail; i++) {
        struct tt_UpdateEntity* update_entity = decode(node, buffer, head, tail, sizeof(struct tt_UpdateEntity));
        uint32_t endpoint_id = rd32(header, update_entity->endpoint_id);
        uint32_t remote_entity_id = rd32(header, update_entity->entity_id); // Phase 2
        uint16_t remote_tracking_words = rd16(header, update_entity->tracking_words);

        uint64_t deadline_duration_ns = rd64(header, update_entity->deadline_duration_ns);
        uint64_t liveliness_lease_duration_ns = rd64(header, update_entity->liveliness_lease_duration_ns);

        TT_LOG_DEBUG("UpdateEntity");
        TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
        TT_LOG_DEBUG("  kind: %d", update_entity->kind);

        if (update_entity->kind == tt_KIND_TOPIC_SUBSCRIBER) {
            // Designated initializers on purpose: this struct gained fields in the middle (Phase
            // 2's entity_id/tracking_words), and positional init silently mapped last_modified onto
            // the wrong member - which read as "this peer already has the backlog" and skipped a
            // real re-delivery. Order can change again; these can't drift.
            struct update_peer_ctx ctx = {.header = header,
                                          .sender_ip = sender_ip,
                                          .sender_port = sender_port,
                                          .qos = update_entity->qos,
                                          .deadline_duration_ns = deadline_duration_ns,
                                          .liveliness_lease_duration_ns = liveliness_lease_duration_ns,
                                          .entity_id = remote_entity_id,
                                          .tracking_words = remote_tracking_words,
                                          .announce_last_modified = last_modified};
            for_each_endpoint(node, tt_KIND_TOPIC_PUBLISHER, endpoint_id, register_subscriber_peer_on_publisher, &ctx);
        } else if (update_entity->kind == tt_KIND_TOPIC_PUBLISHER) {
            // Phase 3 - remember whether this writer promises KEEP_ALL, so acknack_retry() knows
            // not to give up on its gaps. Cached on the proxy (if one exists yet; otherwise first
            // contact picks it up from the discovery table the same way RxO matching does).
            update_writer_proxies_keep_all(node, endpoint_id, header->source, remote_entity_id,
                                           (update_entity->qos & tt_UPDATE_QOS_KEEP_ALL) != 0 ? tt_WRITER_KEEP_ALL_YES
                                                                                              : tt_WRITER_KEEP_ALL_NO);
        } else if (update_entity->kind == tt_KIND_SERVICE_SERVER) {
            struct update_peer_ctx ctx = {.header = header, .sender_ip = sender_ip, .sender_port = sender_port};
            for_each_endpoint(node, tt_KIND_SERVICE_CLIENT, endpoint_id, register_server_peer_on_client, &ctx);
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
        upsert_discovered_entity(node, header->source, endpoint_id, update_entity->kind, update_entity->qos,
                                 deadline_duration_ns, liveliness_lease_duration_ns, type, name);
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
    forget_peers_from_source(node, source, /*preserve_ack=*/true);
    forget_discovered_entities_from_source(node, source);

    if (!decode_update_entities(node, header, buffer, &head, tail, update_header->entity_count, sender_ip, sender_port,
                                last_modified)) {
        return false;
    }

    // Phase 3 prerequisite (c) - the forget above preserved this source's ack state so a re-added
    // Subscriber keeps it; now drop it wherever this announce genuinely dropped the match (an
    // endpoint it no longer lists, or a farewell UPDATE listing nothing at all), so a departed
    // Subscriber can't hold a KEEP_ALL writer's ack set forever.
    drop_ack_state_for_unmatched_source(node, source);

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
    bool offered_manual = (publisher->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0;
    return (sub->reliable && !offered_reliable) || (sub->durable && !offered_durable) ||
           deadline_liveliness_incompatible(sub->deadline_duration_ns, publisher->deadline_duration_ns,
                                            sub->liveliness_manual, offered_manual, sub->liveliness_lease_duration_ns,
                                            publisher->liveliness_lease_duration_ns);
}

// Milestone 35 - for_each_endpoint()'s own visitor context for delivering one arriving DATA
// submessage to every local Subscriber matching its endpoint_id. Fields are read-only snapshots
// taken once in process_data() below - buffer/head/tail point at the still-encoded payload, safe
// to decode independently once per matching Subscriber (each gets its own stack-local decode).
struct data_delivery_ctx {
    struct tt_Header* header;
    uint32_t endpoint_id;
    uint32_t entity_id; // Milestone 47 - the sending Publisher's own identity, tt_DataHeader.entity_id
    uint32_t seq_no;
    uint64_t timestamp;
    uint8_t* buffer;
    uint32_t head;
    uint32_t tail;
    uint32_t sender_ip;
    uint16_t sender_port;
    // Out-param: set true by deliver_data_to_subscriber() if any matching Subscriber's own
    // topic->data_decode() fails, so process_data() can still report the decode failure to ITS
    // OWN caller (test_publish_subscribe.c's own test_process_data_decode_failure_is_reported())
    // the same way it always has, even though delivery itself now fans out to more than one match.
    bool decode_failed;
};

// Milestone 35 - the actual per-Subscriber body process_data() used to run once (against find_
// endpoint()'s single match) before more than one local Subscription on the same topic became
// legal; now for_each_endpoint()'s own visitor, so it runs once per match - each with its own
// independent RxO compatibility check, reliable-ack state, and callback delivery, exactly as if
// each Subscriber had received its own private copy of the packet (which, semantically, it has:
// this is the same fan-out real DDS pub/sub gives every matched Subscriber for one Publisher).
static void deliver_data_to_subscriber(struct tt_Node* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    struct data_delivery_ctx* ctx = (struct data_delivery_ctx*)ctx_ptr;
    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;

    // QoS roadmap #1 (RxO matching, Milestone 31) - an incompatible Publisher's DATA is dropped
    // before any reliable-tracking side effects too (update_reliable_ack() below), not just before
    // delivery - no point generating ACKNACKs a Publisher that could never honor them will never
    // answer (see this file's own pre-Milestone-31 history of exactly that silent-degradation bug).
    if (subscriber_incompatible_with_publisher(node, sub, ctx->header->source, ctx->endpoint_id)) {
        return;
    }

    struct tt_Topic* topic = sub->topic;
    bool is_native = tt_is_native_endian(ctx->header);

    // QoS roadmap #5 (RELIABILITY/RELIABLE) - no-op (always "new") unless sub->reliable. Milestone
    // 60 (rmw_tickle/PLAN.md) - delivery to `callback` below is still unconditional for ordering (a
    // late, retransmitted sample is still delivered whenever it arrives, out of its original order)
    // but no longer for *identity* - a sample update_reliable_ack() recognizes as already delivered
    // (a legitimate ACKNACK-driven retransmit racing the original, or a stale duplicate) is skipped
    // here instead of re-invoking the application callback a second time for it, matching real DDS
    // readers' own per-writer sequence-number de-duplication.
    if (!update_reliable_ack(node, sub, ctx->seq_no, ctx->header->source, ctx->entity_id, ctx->sender_ip,
                             ctx->sender_port)) {
        return;
    }

    // Zero-copy path: hand the callback a tt_Data* aliasing rx_buffer directly, skipping the
    // decode-into-scratch copy and the matching data_free. Falls through to the copy path when
    // the topic doesn't offer it or it declines (e.g. byte-swapped wire).
    if (topic->data_decode_inplace != NULL) {
        struct tt_Data* inplace = topic->data_decode_inplace(ctx->buffer + ctx->head, ctx->tail - ctx->head, is_native);
        if (inplace != NULL) {
            sub->callback(sub, ctx->timestamp, (uint16_t)ctx->seq_no, inplace);
            return;
        }
    }

    uint8_t data[topic->data_size];
    int32_t decoded =
        topic->data_decode((struct tt_Data*)data, ctx->buffer + ctx->head, ctx->tail - ctx->head, is_native);

    if (decoded < 0) {
        TT_LOG_ERROR("Cannot decode data for endpoint_id: %08x, seq_no: %d", ctx->endpoint_id, ctx->seq_no);
        ctx->decode_failed = true;
        return;
    }

    sub->callback(sub, ctx->timestamp, (uint16_t)ctx->seq_no, (struct tt_Data*)data);
    topic->data_free((struct tt_Data*)data);
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
    uint32_t entity_id = rd32(header, data_header->entity_id);

    TT_LOG_DEBUG("Data");
    TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
    TT_LOG_DEBUG("  timestamp: %ld", timestamp);
    TT_LOG_DEBUG("  seq_no: %d", seq_no);

    struct data_delivery_ctx ctx = {
        .header = header,
        .endpoint_id = endpoint_id,
        .entity_id = entity_id,
        .seq_no = seq_no,
        .timestamp = timestamp,
        .buffer = buffer,
        .head = head,
        .tail = tail,
        .sender_ip = sender_ip,
        .sender_port = sender_port,
    };
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id, deliver_data_to_subscriber, &ctx);
    return !ctx.decode_failed;
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

// Milestone 35 - deliberately still uses find_endpoint()'s single-match lookup, not for_each_
// endpoint(), even though more than one local Server may now share this service name: a
// CallRequest must be answered by exactly one Server, and the wire protocol has no per-instance
// id beyond the service-name hash to say which of several identically-named ones a Client meant
// to reach - genuinely ambiguous at the wire level, not a gap this function's own logic could
// close. Picking find_endpoint()'s own deterministic "oldest still-registered match" (its own doc
// comment) is a reasonable, documented choice, matching real DDS's own undefined-which-one
// semantics for redundant same-name Servers - not attempted to be made "correct" beyond that here.
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

// Milestone 35 - deliberately still uses find_endpoint()'s single-match lookup, same reasoning as
// process_callrequest()'s own doc comment: a CallResponse names its target Client only via the
// service-name hash, so if more than one local Client now shares that name, which one actually
// gets it is genuinely ambiguous at the wire level, not something to fix here. The client->cache
// == NULL check just below already guards against corrupting an unrelated Client's own state if
// this ever does pick the "wrong" one of several - the response is simply dropped as unexpected,
// not misapplied.
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
//
// Milestone 62 (rmw_tickle/PLAN.md) - direct-indexed (reliable_cache_slot(): seq_no N always lives
// at index[(N - 1) % depth]), not a linear scan over every slot, this function's own original
// shape. Found and fixed after TickLE Plan's own real HIL re-measurement of Milestone 61
// (caller-configurable depth) showed a deeper cache measurably *hurting* RELIABLE recovery rather
// than merely not helping it - root-caused to that O(depth) scan: at depth=8192 the entry array
// was ~12MB (a 1472-byte buffer embedded per entry, since replaced by B1's byte arena), and
// process_acknack() calls this once per set ACKNACK bit (up to tt_RELIABLE_BITMAP_BITS times), so
// one ACKNACK could walk that whole region up to that many times, almost entirely cache misses on
// real hardware. Relies on `depth` not changing while anything is retained - now stated as a hard
// rule on struct tt_ReliableCache.depth itself (tickle.h), since the write side indexes the same
// way.
//
// Phase 1-c (rmw_tickle/PLAN.md, B2) - *gone is set when the NULL means the sample no longer exists
// for this Publisher at all (evicted, or aged out of LIFESPAN), as opposed to merely out of retry
// budget - retransmit_reliable_samples() answers that with an eviction Heartbeat.
static struct tt_ReliableCacheIndex* find_resendable_cache_entry(struct tt_ReliableCache* cache, uint16_t depth,
                                                                 uint32_t missing_seq_no, uint64_t lifespan_duration_ns,
                                                                 bool pub_keep_all, bool* gone) {
    struct tt_ReliableCacheIndex* cache_entry = reliable_cache_slot(cache, depth, missing_seq_no);
    if (cache_entry->len == 0 || cache_entry->seq_no != missing_seq_no) {
        RSTAT_INC(null_evicted);
        *gone = true;
        return NULL; // empty slot, a tombstone (evicted, or never cached because the sample was
                     // larger than the whole arena), or taken over by a different seq_no since
    }
    // Phase 3 - under KEEP_ALL this Publisher blocks rather than moving on, so it must keep
    // answering: the per-sample cap that normally stops a broken peer making us resend forever is
    // deliberately off here, bounded instead by the Publisher being unable to outrun the stuck
    // Subscriber (it is blocked on exactly that sample).
    if (!pub_keep_all && cache_entry->retry >= tt_RELIABLE_RETRY) {
        RSTAT_INC(null_retry_cap);
        return NULL; // give up on this one sample - the Subscriber's own retry cap will too
    }
    // Same "as if it had never been sent" rule deliver_durability_backlog() applies, here for a
    // live NACK'd retransmit instead of a discovery-triggered backlog push. Matches real DDS:
    // LIFESPAN removes data from the Writer's history outright, RELIABLE's own retry guarantee
    // doesn't override it.
    if (reliable_cache_entry_expired(cache_entry, lifespan_duration_ns)) {
        RSTAT_INC(null_lifespan);
        *gone = true;
        return NULL;
    }
    return cache_entry;
}

// Phase 1-c (rmw_tickle/PLAN.md, B2) - the oldest seq_no this Publisher can still resend: present in
// its slot and not aged out of LIFESPAN. Direct-indexed like find_resendable_cache_entry() above
// (seq_no N lives at entries[(N - 1) % depth]), so it starts at the oldest seq_no the ring can
// still hold and only walks forward past LIFESPAN-expired entries (which expire oldest-first) -
// O(1) without a lifespan, unlike reliable_cache_oldest_seq_no()'s full scan. pub->seq_no + 1 if
// nothing at all is resendable (everything published so far is gone).
static uint32_t reliable_cache_first_resendable_seq_no(const struct tt_Publisher* pub,
                                                       const struct tt_ReliableCache* cache, uint16_t depth) {
    uint32_t newest = cache->newest_seq_no;
    for (uint32_t seq_no = cache->oldest_seq_no; seq_no != 0 && seq_no <= newest; seq_no++) {
        const struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, seq_no);
        if (entry->len != 0 && entry->seq_no == seq_no &&
            !reliable_cache_entry_expired(entry, pub->lifespan_duration_ns)) {
            return seq_no;
        }
    }
    return newest + 1; // nothing resendable: everything published so far is gone
}

// retransmit_reliable_samples()'s per-bit body: resends missing_seq_no straight back to target if
// it's still resendable. Returns true if it's gone for good (see find_resendable_cache_entry()).
static bool retransmit_one_sample(struct tt_Node* node, struct tt_Publisher* pub, struct tt_ReliableCache* cache,
                                  uint16_t depth, uint32_t missing_seq_no, const struct tt_Peer* target) {
    bool gone = false;
    struct tt_ReliableCacheIndex* cache_entry =
        find_resendable_cache_entry(cache, depth, missing_seq_no, pub->lifespan_duration_ns, pub->keep_all, &gone);
    if (cache_entry == NULL) {
        return gone;
    }

    uint32_t old_tx_tail = node->tx_tail;
    void* buf = encode(node, cache_entry->len);
    if (buf == NULL) {
        TT_LOG_WARNING("Lack of tx buffer, cannot retransmit seq_no %u now", missing_seq_no);
        RSTAT_INC(retransmit_tx_fail);
        rollback(node, old_tx_tail);
        return false;
    }
    _tt_memcpy(buf, cache->arena + cache_entry->offset, cache_entry->len);
    if (!end_encode(node, (struct tt_SubmessageHeader*)buf, true, target, 1)) {
        RSTAT_INC(retransmit_tx_fail);
        rollback(node, old_tx_tail);
    } else {
        RSTAT_INC(retransmitted);
        cache_entry->retry++;
    }
    return false;
}

// Milestone 62 (rmw_tickle/PLAN.md) - the actual per-bit retransmit loop, split out of
// process_acknack() below purely to keep that function's own cognitive complexity under clang-
// tidy's threshold, same reasoning find_resendable_cache_entry() above was split out for (the new
// pub->reliable gate this milestone added tipped it over on its own). Only ever called when pub->
// reliable is true - see that call site's own doc comment for why. `bitmap` now spans tt_RELIABLE_
// BITMAP_WORDS words (config.h's own ACKNACK-widening plan) - the outer per-word loop skips a
// whole zero word (the common case for a request that only names a handful of missing samples)
// without touching find_resendable_cache_entry() at all, keeping this O(word-count + set-bit-
// count) rather than a flat O(tt_RELIABLE_BITMAP_BITS) scan - the same "stay O(word-count)" care
// tt_RELIABLE_BITMAP_WORDS's own doc comment calls for.
static void retransmit_reliable_samples(struct tt_Node* node, struct tt_Publisher* pub, struct tt_ReliableCache* cache,
                                        uint16_t depth, uint32_t seq_no, const uint64_t* bitmap, uint16_t words,
                                        const struct tt_Peer* target) {
#ifdef tt_RELIABLE_STATS
    g_rstats.acknack_received++;
    g_rstats.bits_requested += rstat_popcount_bitmap(bitmap, words);
#endif
    bool any_gone = false;
    // Phase 2 - `words` is whatever the requester actually sent (validated by the caller), and the
    // inner loop walks set bits only, so a window widened to tt_RELIABLE_BITMAP_MAX_BITS costs
    // nothing here unless the gaps really are that spread out: O(words + set bits), never
    // O(window).
    for (uint16_t word_idx = 0; word_idx < words; word_idx++) {
        uint64_t word = bitmap[word_idx];
        while (word != 0) {
            uint32_t bit_idx = (uint32_t)__builtin_ctzll(word);
            word &= word - 1; // clear the lowest set bit
            uint32_t missing_seq_no = seq_no + ((uint32_t)word_idx * tt_RELIABLE_BITMAP_WORD_BITS) + bit_idx;
            any_gone |= retransmit_one_sample(node, pub, cache, depth, missing_seq_no, target);
        }
    }

    // Phase 1-c (rmw_tickle/PLAN.md, B2) - at least one requested sample no longer exists here:
    // tell the requester where this Publisher's resendable history now starts, with one FINAL
    // Heartbeat unicast straight back (FINAL: no ACKNACK reply solicited). Its Subscriber then
    // advances past everything below first_available_seq_no at once (inform_subscriber_of_
    // heartbeat()) instead of re-requesting samples that can never come back until its own retry
    // budget gives up - retries that used to double as the only eviction signal. Once per ACKNACK,
    // not per missing bit.
    if (any_gone) {
        RSTAT_INC(eviction_heartbeats);
        encode_and_send_heartbeat(node, pub, reliable_cache_first_resendable_seq_no(pub, cache, depth), target, 1,
                                  tt_HEARTBEAT_FLAG_FINAL);
    }
}

// Milestone 35 - deliberately still uses find_endpoint()'s single-match lookup: retransmission
// must come from exactly one Publisher's own reliable_cache, and if more than one local Publisher
// now shares this topic name, RELIABLE QoS between them is already an accepted, documented gap
// (see add_endpoint_to_node()'s own doc comment) - two independent Publishers interleaving DATA
// under one wire id corrupts a remote reliable Subscriber's own seq_no tracking regardless of
// which one answers a given ACKNACK, so picking a specific one here doesn't make that any better
// or worse. Not attempted to be made "correct" beyond find_endpoint()'s own deterministic pick.
static bool process_acknack(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                            uint32_t tail, uint32_t sender_ip, uint16_t sender_port) {
    struct tt_AckNackHeader* acknack_header = decode(node, buffer, &head, tail, sizeof(struct tt_AckNackHeader));
    if (acknack_header == NULL) {
        TT_LOG_ERROR("Illegal AckNackHeader");
        return false;
    }

    uint32_t endpoint_id = rd32(header, acknack_header->endpoint_id);
    uint32_t seq_no = rd32(header, acknack_header->seq_no);
    uint32_t entity_id = rd32(header, acknack_header->entity_id);
    uint32_t sender_entity_id = rd32(header, acknack_header->sender_entity_id);

    // Phase 2 - the bitmap is variable length now, so its own claimed word count is untrusted
    // input: it must fit both this datagram's own remaining bytes and this receiver's own local
    // buffer. A count that fails either is a malformed (or hostile) packet, not something to clamp
    // and half-process - the request would name sequence numbers the sender never meant.
    uint16_t bitmap_words = rd16(header, acknack_header->bitmap_words);
    if (bitmap_words > tt_RELIABLE_BITMAP_MAX_WORDS) {
        TT_LOG_ERROR("Illegal AckNack bitmap_words: %u > %d", bitmap_words, tt_RELIABLE_BITMAP_MAX_WORDS);
        return false;
    }
    uint64_t bitmap[tt_RELIABLE_BITMAP_MAX_WORDS];
    if (decode(node, buffer, &head, tail, (uint32_t)bitmap_words * sizeof(uint64_t)) == NULL) {
        TT_LOG_ERROR("Illegal AckNack bitmap: %u words do not fit the datagram", bitmap_words);
        return false;
    }
    for (uint16_t word = 0; word < bitmap_words; word++) {
        bitmap[word] = rd64(header, acknack_header->bitmap[word]);
    }

    TT_LOG_DEBUG("AckNack");
    TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
    TT_LOG_DEBUG("  seq_no: %u", seq_no);

    // Milestone 47 - prefers the local Publisher this ACKNACK's own entity_id actually names when
    // more than one shares endpoint_id (Milestone 35's own previously-accepted ambiguity), falling
    // back to find_endpoint()'s own plain first match when entity_id is 0/unknown or unmatched -
    // see find_endpoint_by_entity()'s own doc comment.
    struct tt_Endpoint* endpoint = find_endpoint_by_entity(node, tt_KIND_TOPIC_PUBLISHER, endpoint_id, entity_id);
    if (endpoint == NULL) {
        return true;
    }

    struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
    if (pub->reliable_cache == NULL) {
        return true; // not a reliable/durable Publisher (or a stale ack) - nothing cached to
                     // resend or to update peer_acks against
    }

    struct tt_ReliableCache* cache = pub->reliable_cache;
    uint16_t depth = reliable_cache_depth(cache);
    if (depth == 0) {
        return true; // index[]/capacity/arena never set up - nothing cached to resend
    }
    struct tt_Peer target = {header->source, sender_ip, sender_port};

    // QoS roadmap #5 (RELIABILITY) follow-up - tt_Publisher_wait_for_all_acked(). record_peer_ack()
    // keeps peer_acks[] (keyed by node_id since Phase 3 prerequisite (c), see its own doc comment,
    // tickle.h) only ever advancing - a stale/reordered ACKNACK carrying a smaller seq_no than
    // what's already recorded must not regress it, since UDP gives no ordering guarantee between
    // two ACKNACKs from the same peer. A sender not currently in peers[] (already forgotten, e.g.
    // via forget_publisher_peer(), or never matched in the first place) has nothing to record
    // against. Unconditional on pub->reliable (unlike the
    // retransmission loop below) - matches tt_Publisher_wait_for_all_acked()'s own gate (pub->
    // reliable_cache != NULL, not pub->reliable specifically), since a DURABLE-only Publisher can
    // legitimately solicit and track an ACKNACK reply too (tt_Publisher_request_ack()'s own
    // Heartbeat, answered regardless of pub->reliable) - only the actual byte retransmission below
    // is RELIABILITY's own exclusive contract.
    record_peer_ack(pub, header->source, sender_entity_id, seq_no);
    // Phase 3 - this ACKNACK may have freed room a refused publish was waiting on. Fired here, from
    // inside tt_Node_poll()'s own packet handling, so the callback runs on the node's thread like
    // every other callback (see tt_Publisher.writable_callback's doc comment on what it may do).
    notify_writable_if_pending(pub);

    // Milestone 62 (rmw_tickle/PLAN.md) - gated on pub->reliable specifically, not merely pub->
    // reliable_cache != NULL: Milestone 24 unified RELIABILITY's and DURABILITY's own storage into
    // one cache, so a DURABLE-but-not-RELIABLE Publisher (TRANSIENT_LOCAL backlog offered, no
    // ongoing loss-recovery guarantee) still has reliable_cache allocated - but ACKNACK-driven
    // *retransmission* is squarely RELIABILITY's own contract, not DURABILITY's (which is already
    // served entirely by deliver_durability_backlog()'s own one-shot push, no ACKNACK involved at
    // all). The DDS semantic-parity backlog's own row 1 originally considered gating this on pub->
    // durable instead - rejected (see that row's own doc comment) since it would have broken the
    // legitimate in-flight-loss recovery a VOLATILE+RELIABLE Publisher must still guarantee to an
    // already-matched Subscriber; gating on pub->reliable alone has no such risk, since a Publisher
    // that never offered RELIABLE was never obligated to answer an ACKNACK's retransmission request
    // in the first place - a matching Subscriber's own RxO check (subscriber_incompatible_with_
    // publisher()) already refuses to match a reliable-requesting Subscriber against a non-reliable
    // Publisher, so no compatible peer would ever legitimately request one here anyway; this is the
    // defensive, spec-honest half of that same contract, on the answering side.
    if (pub->reliable) {
        retransmit_reliable_samples(node, pub, cache, depth, seq_no, bitmap, bitmap_words, &target);
    }

    return true;
}

// Milestone 35 - for_each_endpoint()'s own visitor context for informing every local Subscriber
// sharing the announced topic name about one arriving Heartbeat - not just the first, now that
// more than one may exist. header/sender_ip/sender_port/entity_id identify the Heartbeat's own
// sending Publisher (Milestone 47 - which of this Subscriber's own struct tt_WriterProxy entries
// this updates); first_available_seq_no/last_seq_no are already rd32()'d in process_heartbeat()
// below.
struct heartbeat_ctx {
    struct tt_Header* header;
    uint32_t sender_ip;
    uint16_t sender_port;
    uint32_t entity_id; // Milestone 47 - the sending Publisher's own identity, tt_HeartbeatHeader.entity_id
    uint32_t first_available_seq_no;
    uint32_t last_seq_no;
    uint8_t flags; // tt_HEARTBEAT_FLAG_FINAL or 0 - see its own doc comment, tickle.h
};

// Phase 1-c (rmw_tickle/PLAN.md, B2) - an already-tracking proxy learns from a Heartbeat's
// first_available_seq_no that everything below it no longer exists at the Publisher (evicted, or
// aged out of LIFESPAN) - RTPS's own "irrelevant" range. Advance ack_seq_no straight past it
// instead of re-requesting samples that can never come back: shift received_bitmap to the new
// base, absorb any already-received run that now follows, and hand the next gap (if any) a fresh
// retry budget, the same bookkeeping advance_ack_seq_no() does for a real receipt. Fires on any
// Heartbeat carrying a newer baseline - most often the eviction Heartbeat process_acknack() sends
// back when a requested sample is gone, but a periodic one says the same thing. Never moves
// ack_seq_no backwards (a stale/reordered Heartbeat, or first_available_seq_no 0 for an empty
// cache, is a no-op). A durable late joiner is unaffected at first contact (its baseline comes
// from the first-contact branch of inform_subscriber_of_heartbeat() below); afterwards anything
// the Publisher no longer holds is unrecoverable for it too.
static void advance_past_unavailable(struct tt_WriterProxy* proxy, uint32_t first_available_seq_no) {
    if (first_available_seq_no <= proxy->ack_seq_no) {
        return;
    }
    uint32_t skipped = first_available_seq_no - proxy->ack_seq_no;
#ifdef tt_RELIABLE_STATS
    {
        uint64_t skipped_mask[tt_RELIABLE_BITMAP_MAX_WORDS];
        uint64_t received_in_range[tt_RELIABLE_BITMAP_MAX_WORDS];
        bitmap_low_mask(skipped_mask, proxy_words(proxy),
                        skipped >= proxy_window_bits(proxy) ? (int)proxy_window_bits(proxy) - 1 : (int)skipped - 1);
        for (uint16_t word = 0; word < proxy_words(proxy); word++) {
            received_in_range[word] = proxy->received_bitmap[word] & skipped_mask[word];
        }
        g_rstats.heartbeat_advances++;
        g_rstats.heartbeat_abandoned_seq += skipped - rstat_popcount_bitmap(received_in_range, proxy_words(proxy));
    }
#endif
    if (skipped < proxy_window_bits(proxy)) {
        bitmap_shift_right(proxy->received_bitmap, proxy_words(proxy), skipped);
    } else {
        bitmap_clear(proxy->received_bitmap, proxy_words(proxy));
    }
    proxy->ack_seq_no = first_available_seq_no;
    while (bitmap_lowest_bit_set(proxy->received_bitmap)) { // absorb whatever's already confirmed right after it
        bitmap_shift_right_one(proxy->received_bitmap, proxy_words(proxy));
        proxy->ack_seq_no++;
    }
    proxy->retry = 0;
}

// Milestone 35 - the actual per-Subscriber body process_heartbeat() used to run once (against
// find_endpoint()'s single match) before more than one local Subscription on the same topic
// became legal; now for_each_endpoint()'s own visitor, so every matching Subscriber - each with
// its own independent reliable ack state - learns this Heartbeat's baseline/range, symmetric with
// deliver_data_to_subscriber()'s own fan-out for DATA. Milestone 47 - operates on this specific
// sender's own struct tt_WriterProxy (keyed by (header->source, entity_id)), not a single flat
// watermark - see that struct's own doc comment.
static void inform_subscriber_of_heartbeat(struct tt_Node* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    struct heartbeat_ctx* ctx = (struct heartbeat_ctx*)ctx_ptr;
    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
    if (!sub->reliable) {
        return; // a best-effort Subscriber has no ack state a Heartbeat could inform
    }

    bool first_contact = false;
    struct tt_WriterProxy* proxy =
        find_or_create_writer_proxy(sub, ctx->header->source, ctx->entity_id, &first_contact);
    if (proxy == NULL) {
        return; // WriterProxy table full - see find_or_create_writer_proxy()'s own doc comment
    }

    if (first_contact) {
        // First-ever reliable contact from this writer: learn the *real* starting baseline
        // straight from the Heartbeat, rather than guessing it from whatever DATA happens to
        // arrive first (PLAN.md's Milestone 20's own workaround for not having this signal at
        // all) - the actual DDS-parity fix this whole follow-up is for.
        //
        // Milestone 60 (rmw_tickle/PLAN.md) - which baseline counts as "the real starting point"
        // depends on THIS Subscriber's own requested durability (sub->durable), not the remote
        // Publisher's offered one - DDS's own RxO (Requested vs Offered) design philosophy applies
        // to DURABILITY exactly like every other RxO QoS policy (RELIABILITY, DEADLINE, LIVELINESS,
        // ...): the Offered side only gates compatibility (offered >= requested, already enforced
        // by subscriber_incompatible_with_publisher()), the Requested side defines what the
        // Subscriber actually wants out of the match. A durable (TRANSIENT_LOCAL-equivalent)
        // Subscriber wants everything the Publisher still retains, so first_available_seq_no (the
        // oldest still-cached sample) is the right baseline. A volatile Subscriber explicitly does
        // NOT want pre-match history, even when the matched Publisher happens to be durable and
        // could offer it (a legal, common DDS pattern) - its own baseline is "whatever's already
        // been published up to this instant", i.e. ctx->last_seq_no, so only samples published
        // *after* this point are ever tracked or ACKNACK-requested. Real HIL finding this closes:
        // durability_late_join's own volatile-Subscriber scenario deterministically leaked an
        // entire pre-match backlog via this exact branch, unaffected by update_reliable_ack()'s own
        // first-contact fix (that DATA-arrival path is never reached here - the discovery-triggered
        // Heartbeat, send_initial_heartbeat(), reaches a freshly-matched Subscriber first whenever
        // the Publisher's own backlog was written before the Subscriber even existed, exactly this
        // scenario's own shape). +1 on the volatile side, not last_seq_no itself - ack_seq_no
        // means "next NOT YET accounted for" (struct tt_WriterProxy's own doc comment), so leaving
        // it at last_seq_no would still treat that one already-published sample as outstanding and
        // ACKNACK-request it (highest_relevant_bit() sees heartbeat_last_seq_no == ack_seq_no as
        // offset 0, "needs attention") - an off-by-one leak of exactly the newest pre-match sample.
        proxy->ack_seq_no = sub->durable ? ctx->first_available_seq_no : ctx->last_seq_no + 1;
        bitmap_clear(proxy->received_bitmap, proxy_words(proxy));
    } else {
        advance_past_unavailable(proxy, ctx->first_available_seq_no);
    }
    if (!first_contact && ctx->last_seq_no >= proxy->ack_seq_no) {
        uint64_t offset = (uint64_t)ctx->last_seq_no - proxy->ack_seq_no;
        if (offset >= proxy_window_bits(proxy)) {
            // Already-tracking Subscriber, but this Heartbeat reveals a gap too wide to ever
            // track - the same "provably unrecoverable, don't get stuck" case update_reliable_
            // ack()'s own oversized-DATA-gap branch handles (see jump_ack_baseline()'s own doc
            // comment) - jump ahead here too, rather than only ever being able to discover this
            // reactively once *some* DATA sample eventually arrives to trigger update_reliable_
            // ack() instead.
            RSTAT_INC(jump_heartbeat);
            jump_ack_baseline(proxy, ctx->last_seq_no);
        }
        // else: within the trackable window - nothing to do here directly. highest_relevant_bit()
        // already picks this up from heartbeat_last_seq_no (set unconditionally below), and
        // maybe_arm_acknack_retry()/send_acknack() below act on it.
    }
    // last_seq_no < proxy->ack_seq_no: a stale/reordered Heartbeat (e.g. arrived after DATA
    // already caught this Subscriber up further) - nothing to do, same "duplicate/old" no-op
    // update_reliable_ack()'s own seq_no < ack_seq_no branch already has.

    proxy->sender_ip = ctx->sender_ip;
    proxy->sender_port = ctx->sender_port;
    // Monotonic guard: a Heartbeat can arrive out of order over UDP the same as any other
    // submessage - never let a late, older one regress what highest_relevant_bit() already knows.
    if (ctx->last_seq_no > proxy->heartbeat_last_seq_no) {
        proxy->heartbeat_last_seq_no = ctx->last_seq_no;
    }

    // tt_HEARTBEAT_FLAG_FINAL's own doc comment (tickle.h) - had_gap must be read before maybe_arm_
    // acknack_retry() below (it's the only thing that could otherwise change what highest_relevant_
    // bit() sees) so a Heartbeat that both reveals a real gap *and* explicitly requests a response
    // sends exactly one ACKNACK (maybe_arm_acknack_retry()'s own), not two.
    bool had_gap = highest_relevant_bit(proxy) >= 0;
    maybe_arm_acknack_retry(node, proxy);
    if (!had_gap && !(ctx->flags & tt_HEARTBEAT_FLAG_FINAL)) {
        // Healthy (no gap) but tt_Publisher_request_ack() explicitly asked anyway - the only way a
        // Publisher ever learns a healthy Subscriber has fully caught up (see maybe_arm_acknack_
        // retry()'s own "a healthy stream needs no ACKNACK at all" comment for why nothing above
        // already sent one).
        send_acknack(node, proxy);
    }
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
    uint32_t entity_id = rd32(header, heartbeat_header->entity_id);
    uint8_t flags = heartbeat_header->flags; // single byte - already native-endian, same as
                                             // struct tt_UpdateEntity.kind/.qos's own convention

    TT_LOG_DEBUG("Heartbeat");
    TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
    TT_LOG_DEBUG("  first_available_seq_no: %u", first_available_seq_no);
    TT_LOG_DEBUG("  last_seq_no: %u", last_seq_no);

    struct heartbeat_ctx ctx = {header, sender_ip, sender_port, entity_id, first_available_seq_no, last_seq_no, flags};
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id, inform_subscriber_of_heartbeat, &ctx);
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
        // validate_packet_header()'s own version check, exact since Phase 2). Its length was already validated
        // by the caller, so skip exactly that far and carry on instead of discarding the packet.
        TT_LOG_WARNING("Unknown submessage type %d, skipping (len %u)", submessage_header->type, body_tail - head);
        return true;
    }
}

// Rejects anything this node can't parse: a foreign magic value, or a different protocol version
// (exact match since Phase 2 - see the version check's own comment below).
static bool validate_packet_header(struct tt_Node* node, struct tt_Header* header) {
    if (!tt_is_native_endian(header) && !tt_is_reverse_endian(header)) {
        TT_LOG_ERROR("Illegal magic: 0x%04x", header->magic_value);
        return false;
    }

    TT_LOG_DEBUG("magic: 0x%04x (%c%c)", header->magic_value, header->magic[0], header->magic[1]);

    // Phase 2 (rmw_tickle/PLAN.md) - exact match, where this used to accept anything >= our own
    // version. That was one-directional: a newer peer's packet passed this check and was then
    // parsed with the older struct layout, silently misreading fields rather than failing. An
    // exact match refuses cleanly in both directions.
    //
    // Honest limitation: this only helps from tt_VERSION 6 onward. A node built before this change
    // still accepts a newer packet, and no change here can fix that retroactively - acceptable
    // because nothing is deployed (Plan/user, 2026-09-23).
    if (header->version != tt_VERSION) {
        // Rate-limited: at max rate a mismatched peer would otherwise log per packet, which is its
        // own denial of service. One line per source, re-armed when a different version shows up.
        if (node->version_mismatch_logged[header->source] != header->version) {
            node->version_mismatch_logged[header->source] = header->version;
            TT_LOG_ERROR("Illegal version from node %d: %d != %d", header->source, header->version, tt_VERSION);
        }
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

    if (!validate_packet_header(node, header)) {
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

    // EXPERIMENTAL (branch experiment/poll-loop-io-interleave, rmw_tickle/PLAN.md's own "Further
    // latency research" section) - counts scheduler entries run back-to-back without a receive
    // check, so a continuously-rescheduling task (a max-rate Publisher's own send loop) can't
    // starve tt_receive() for this whole call's own timeout budget. See tt_SCHEDULER_IO_INTERLEAVE's
    // own doc comment (config.h) for the full reasoning.
    uint32_t consecutive_scheduler_runs = 0;

    while (timeout > 0) {
        struct tt_TCB* tcb = peek_scheduler(node);

        if (tcb != NULL && tcb->time <= time && consecutive_scheduler_runs < tt_SCHEDULER_IO_INTERLEAVE) {
            // Run scheduler first
            tcb->function(node, time, tcb->param);
            pop_scheduler(node);
            consecutive_scheduler_runs++;
        } else if (tcb != NULL && tcb->time <= time) {
            // A scheduler entry is still due, but tt_SCHEDULER_IO_INTERLEAVE consecutive ones have
            // already run without a receive check - force one non-blocking peek before letting more
            // scheduler work run. Not the caller's own real wait (never blocks): if nothing's
            // there, fall straight back into scheduler processing next iteration.
            consecutive_scheduler_runs = 0;
            uint32_t ip = 0;
            uint16_t port = 0;
            int32_t len = tt_try_receive(node, node->rx_buffer, tt_MAX_BUFFER_LENGTH, &ip, &port);
            if (len >= 0) {
                return drain_rx(node, process_datagram(node, len, ip, port));
            }
        } else {
            // Run network I/O next
            consecutive_scheduler_runs = 0;
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

// See this function's own doc comment (tickle.h).
bool tt_Node_entity_alive(const struct tt_Node* node, const struct tt_DiscoveredEntity* entity, uint64_t now) {
    if (node == NULL || entity == NULL || entity->node_id == tt_NODE_ID_INVALID) {
        return false;
    }
    if (entity->liveliness_lease_duration_ns == 0) {
        return entity->alive; // no specific lease requested - defer to the node-level sweep
    }
    if (!node->update_seen[entity->node_id]) {
        return false; // never heard from this node id at all
    }
    return (now - node->update_last_seen[entity->node_id]) <= entity->liveliness_lease_duration_ns;
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
