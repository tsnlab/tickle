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
_Static_assert(offsetof(struct tt_Context, tx_buffer) % 4 == 0, "tx_buffer not 4-aligned in tt_Context");
// A slot's payload sits at sizeof(tt_SegmentHeader) + N * (sizeof(tt_SegmentSlot) + slot_bytes) +
// sizeof(tt_SegmentSlot). An encoder writing a record there needs the same 4-alignment the assert above
// gives tx_buffer, and these two make that reduce to slot_bytes % 4 == 0 - which is what
// whole_record_limit_for() checks. Without them a field added to either struct would silently make a
// passing runtime check insufficient, which is how a guard comes to be sufficient by accident.
_Static_assert(sizeof(struct tt_SegmentHeader) % 4 == 0, "tt_SegmentHeader must keep slots 4-aligned");
_Static_assert(sizeof(struct tt_SegmentSlot) % 4 == 0, "tt_SegmentSlot must keep payloads 4-aligned");
_Static_assert(offsetof(struct tt_Context, rx_buffer) % 4 == 0, "rx_buffer not 4-aligned in tt_Context");
#undef TT_FRAMING_HDR
#if tt_FRAG_ENABLED
_Static_assert(sizeof(struct tt_DataHeader) == tt_FRAG_DATA_HEADER_LENGTH, "tt_FRAG_DATA_HEADER_LENGTH is stale");
_Static_assert(sizeof(struct tt_FragFirstHeader) == sizeof(struct tt_DataHeader) + 1 &&
                   sizeof(struct tt_FragContHeader) == (2 * sizeof(uint32_t)) + 2,
               "fragment headers are unpadded by design - see DATAFRAG_PLAN.md section 6.3");
#endif
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

// Latency trace stamps - see include/tickle/trace.h. Compiled out unless built with -Dtt_TRACE: without it
// TT_TRACE() is a no-op, which is why the header is included either way.
#include <tickle/trace.h>
#ifdef tt_TRACE

// tt_trace_stamp()'s ring (trace.h). Process-global and written by any thread: the slot is claimed with an
// atomic add, and a reader copies after the writers it cares about have stopped.
static struct tt_TraceStamp g_trace[tt_TRACE_CAPACITY];
static uint32_t g_trace_next;

void tt_trace_stamp(enum tt_TracePoint point) {
    uint32_t slot = __atomic_fetch_add(&g_trace_next, 1U, __ATOMIC_RELAXED) & (tt_TRACE_CAPACITY - 1U);
    g_trace[slot] = (struct tt_TraceStamp) {.ns = tt_get_ns(),
                                            .thread = (uint64_t)tt_thread_self(),
                                            .point = (uint32_t)point}; // NOLINT(misc-include-cleaner)
}

uint32_t tt_trace_read(struct tt_TraceStamp* out, uint32_t max) {
    uint32_t written = __atomic_load_n(&g_trace_next, __ATOMIC_ACQUIRE);
    uint32_t held = written < tt_TRACE_CAPACITY ? written : tt_TRACE_CAPACITY;
    uint32_t count = held < max ? held : max;
    uint32_t first = written - count;
    for (uint32_t i = 0; i < count; i++) {
        out[i] = g_trace[(first + i) & (tt_TRACE_CAPACITY - 1U)];
    }
    return count;
}
#endif

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

// Saturates at UINT32_MAX (~4.3 s) rather than wrapping: a call answered after 5 s would otherwise read as 0.7 s.
static uint32_t calculate_latency(uint64_t start, uint64_t end) {
    if (end <= start) {
        return 0;
    }
    return end - start > UINT32_MAX ? UINT32_MAX : (uint32_t)(end - start);
}

static struct tt_SubmessageHeader* start_encode(struct tt_Context* node, uint8_t type, uint8_t receiver) {
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

static void* encode(struct tt_Context* node, uint32_t len) {
    if (node->tx_tail + len >= node->tx_size) {
        TT_LOG_WARNING("Lack of tx buffer");
        return NULL;
    }

    return tt_encode_buffer(node->tx_buffer, &node->tx_tail, len);
}

static bool encode_string(struct tt_Context* node, const char* str) {
    if (!tt_encode_string(node->tx_buffer, &node->tx_tail, node->tx_size, str)) {
        TT_LOG_WARNING("Lack of tx buffer");
        return false;
    }
    return true;
}

// The only way tx_tail moves. A datagram's seq span (tt_Context.tx_seq_span) belongs to the datagram in
// tx_buffer, so it returns to 1 the moment that buffer empties - as a property of this function rather
// than a rule each site has to remember. The sites that rewind the tail to abandon a half-built datagram
// are the ones that would otherwise leak a span onto the next record, and they all come through here or
// through the two that empty the buffer outright.
static void set_tx_tail(struct tt_Context* node, uint32_t tail) {
    node->tx_tail = tail;
    if (tail <= sizeof(struct tt_Header)) {
        node->tx_seq_span = 1;
    }
}

static void rollback(struct tt_Context* node, uint32_t old_tx_tail) {
    set_tx_tail(node, old_tx_tail);
}

// Where a client's or server's retry and deferred-response storage lives: the caller's, attached by
// tt_Client_set_storage()/tt_Server_set_storage(), or the inline default when that pointer is NULL -
// which is also what a zeroed struct holds, so an endpoint never set up through create or attach
// still gets working storage rather than a zero-length one.
static uint8_t* client_cache_area(struct tt_Client* client, uint32_t* length) {
    *length = client->cache_storage != NULL ? client->cache_length : (uint32_t)tt_CLIENT_CACHE_LENGTH;
    return client->cache_storage != NULL ? client->cache_storage : client->cache_buf;
}

static uint32_t server_cache_entry_length(const struct tt_Server* server) {
    return server->cache_storage != NULL ? server->cache_entry_length : (uint32_t)tt_SERVER_CACHE_ENTRY_LENGTH;
}

static uint8_t* server_cache_entry(struct tt_Server* server, int slot) {
    return server->cache_storage != NULL ? server->cache_storage + ((size_t)slot * server->cache_entry_length)
                                         : server->cache_buf[slot];
}

// peer_count == 0 (peers may be NULL) means "no override, send to the node's usual broadcast
// address" - the direct successor to the old dest_ip == 0 sentinel. peer_count >= 1 sends the
// same already-encoded buffer to each peer in turn via tt_send_to() instead - used by
// process_callrequest() (a peer list of length 1: the CallResponse straight back to its own
// requester, see its own comment on why that's safe there specifically) and by
// tt_Publisher_publish()/tt_Client_call()/resend_call_request() (a short list of known peers, see
// tt_UNICAST_PEER_THRESHOLD) once discovery has learned a handful of them.
// How many links this node talks on. Reads 1 when nothing has been configured *and* nothing has
// resolved the table yet - a whitebox test constructs a tt_Context directly without going through
// tt_Context_create(), so this cannot assume resolve_links() has run, and a zero here would make
// every send loop iterate zero times and silently send nothing.
static uint8_t link_count(void) {
    if (_tt_CONFIG.link_count == 0) {
        // Populate the default link here rather than only in resolve_links(), because a whitebox
        // test constructs a tt_Context directly and never calls tt_Context_create(). Leaving the table
        // empty was not merely "unconfigured": unicast_threshold read 0, so a single known peer
        // failed the `on_link <= threshold` test and every call fell through to broadcast. A
        // default that is never written is indistinguishable from a configured zero.
        //
        // Only the values, not the OS resolution - that needs the HAL and belongs at node
        // creation. An unresolved link is the catch-all, which is the correct reading of "nothing
        // has been configured" anyway.
        _tt_CONFIG.links[0].broadcast = _tt_CONFIG.broadcast;
        _tt_CONFIG.links[0].addr = _tt_CONFIG.addr;
        _tt_CONFIG.links[0].unicast_threshold = tt_UNICAST_PEER_THRESHOLD;
        _tt_CONFIG.link_count = 1;
    }
    return _tt_CONFIG.link_count < tt_MAX_LINK_COUNT ? _tt_CONFIG.link_count : tt_MAX_LINK_COUNT;
}

// Fills in the link table once, at node creation. A node with nothing configured gets exactly one
// link, built from the scalar addr/broadcast/tt_UNICAST_PEER_THRESHOLD fields, so the single-link
// case is the degenerate one rather than a branch everything else has to remember.
//
// Idempotent: re-resolving an already-resolved link asks the OS the same question and gets the
// same answer, so a second node in the same process costs one getifaddrs and changes nothing.
// tt_ETHERNET_UDP_PAYLOAD (config.h) sizes control messages and every "fits one datagram" decision for a
// 1500-byte Ethernet MTU, and nothing checked that the link actually has one. A narrower link - a VPN
// tunnel at 1420, PPPoE at 1492 - still delivers, through IP fragmentation, which is why this warns
// rather than refusing the node: but a fragmented control message is lost whenever any one fragment is,
// and nothing else would say so. A wider link (jumbo frames) is fine; the assumption is conservative
// there. An unresolved link - the default limited broadcast, owned by no interface - has no MTU to ask.
// Large rmw samples are fragmented on purpose (the user's decision), and are not what this is about.
#define tt_ASSUMED_MTU (tt_ETHERNET_UDP_PAYLOAD + 20 + 8) // + IPv4 and UDP headers
static void check_link_mtu(struct _tt_Link* link) {
    link->resolved_mtu = link->resolved ? tt_link_mtu(link->resolved_addr) : -1;
    link->mtu_below_assumed = link->resolved_mtu > 0 && link->resolved_mtu < tt_ASSUMED_MTU;
    if (link->mtu_below_assumed) {
        TT_LOG_WARNING("Link %s has MTU %d, below the %d bytes this build assumes (tt_ETHERNET_UDP_PAYLOAD %d): "
                       "control messages near that size will be IP-fragmented, and lost whenever any fragment is",
                       link->broadcast, (int)link->resolved_mtu, (int)tt_ASSUMED_MTU, (int)tt_ETHERNET_UDP_PAYLOAD);
    }
}

#if tt_DISCOVERY_OPTIONS
// (g6) A peer link's address, "a.b.c.d" or "a.b.c.d/nn", into its address, netmask and the destination it is reached at
// (the address itself, or the subnet's directed broadcast). No OS lookup: a peer is not a local interface.
static bool resolve_peer_link(struct _tt_Link* link) {
    const char* text = link->broadcast;
    uint32_t addr = 0;
    uint32_t prefix = 32;
    for (int part = 0; part < 4; part++) {
        if (text == NULL || *text < '0' || *text > '9') {
            return false;
        }
        uint32_t value = 0;
        while (*text >= '0' && *text <= '9') {
            value = (value * 10U) + (uint32_t)(*text++ - '0');
            if (value > 255U) {
                return false;
            }
        }
        addr = (addr << 8) | value;
        if (part < 3 && *text++ != '.') {
            return false;
        }
    }
    if (*text == '/') {
        text++;
        prefix = 0;
        while (*text >= '0' && *text <= '9') {
            prefix = (prefix * 10U) + (uint32_t)(*text++ - '0');
        }
    }
    if (*text != '\0' || prefix > 32U) {
        return false;
    }
    uint32_t netmask = prefix == 0 ? 0 : UINT32_MAX << (32U - prefix);
    link->resolved_addr = addr;
    link->resolved_netmask = netmask;
    link->resolved_broadcast = prefix == 32U ? addr : (addr | ~netmask);
    link->resolved = true;
    link->resolved_mtu = -1;
    link->mtu_below_assumed = false;
    return true;
}
#endif

static tt_ret_t resolve_links(void) {
    for (uint8_t i = 0; i < link_count(); i++) {
        struct _tt_Link* link = &_tt_CONFIG.links[i];
#if tt_DISCOVERY_OPTIONS
        if (link->peer) {
            if (!resolve_peer_link(link)) {
                TT_LOG_ERROR("Link %u: peer %s is not an IPv4 address - not creating the node", i,
                             link->broadcast != NULL ? link->broadcast : "(null)");
                return tt_RET_NO_SUCH_LINK;
            }
            continue;
        }
#endif
        link->resolved =
            tt_resolve_link(link->broadcast, &link->resolved_addr, &link->resolved_netmask, &link->resolved_broadcast);
        check_link_mtu(link);
        if (!link->resolved) {
            // Two different situations, and the whole point of separating them is that one is a
            // configuration error and the other is the default.
            //
            // The limited broadcast is owned by no interface by definition. It is the compiled-in
            // default and the catch-all that makes an unconfigured node work, so it is normal and
            // says nothing - warning here would fire on every node that has configured nothing,
            // which is all of them today.
            //
            // A *directed* broadcast that matches no local interface is the caller having asked
            // for an interface that is not here. That is reported rather than absorbed: the node
            // is not created, and the caller decides what to do about it. A ROS 2 launch or a
            // supervised service retries, because the interface may simply not be up yet - DHCP
            // and network managers routinely lose that race - while a fixed-configuration embedded
            // target treats it as fatal. The library does not decide that on the application's
            // behalf, and it does not quietly run with a link the caller asked for silently not in
            // use, which was the option this replaced.
            if (link->resolved_broadcast != tt_LIMITED_BROADCAST) {
                // Logged as well as returned, for a different reader: the return code tells the
                // program, and this tells whoever is looking at a boot log wondering why a service
                // keeps restarting. That person is usually the one who made the typo.
                TT_LOG_ERROR("Link %u: no local interface has broadcast address %s - not creating the node", i,
                             link->broadcast != NULL ? link->broadcast : "(null)");
                return tt_RET_NO_SUCH_LINK;
            }
            TT_LOG_DEBUG("Link %u (%s) is the limited broadcast - no interface owns it, so it is the catch-all", i,
                         link->broadcast != NULL ? link->broadcast : "(null)");
        }
    }
    return tt_RET_OK;
}

// Which configured link a peer address belongs to. A link the OS resolved matches addresses in its
// subnet; a link it did not resolve is the catch-all for everything else, which is what makes the
// limited broadcast still work as the default. Falls back to link 0 so this always names a link.
static uint8_t link_of_ip(uint32_t ip) {
    uint8_t fallback = 0;
    bool have_fallback = false;
    for (uint8_t i = 0; i < link_count(); i++) {
        const struct _tt_Link* link = &_tt_CONFIG.links[i];
        if (link->resolved) {
            if ((ip & link->resolved_netmask) == (link->resolved_addr & link->resolved_netmask)) {
                return i;
            }
        } else if (!have_fallback) {
            fallback = i;
            have_fallback = true;
        }
    }
    return fallback;
}

// Every send below takes the datagram as a struct tx_datagram: a head, and optionally a body sent from
// wherever it already lies. A flush of tx_buffer is all head. A fragment is a few bytes of framing built
// on the stack and a body read straight out of tx_buffer, the reliable cache or the caller's sample, so
// a large sample is never copied again just to be cut up.
_Static_assert(sizeof(struct tt_SingleHeader) == sizeof(struct tt_SubmessageHeader) &&
                   sizeof(struct tt_SingleHeader) == sizeof(struct tt_Header),
               "the single-submessage form must take exactly one of the classic headers' places");

// The single-submessage header's first byte from this node: its magic's first byte in memory, in lower case.
static uint8_t native_single_marker(void) {
    uint16_t magic = NATIVE_MAGIC_VALUE;
    uint8_t first = 0;
    _tt_memcpy(&first, &magic, sizeof(first));
    return (uint8_t)(first | tt_SINGLE_MARKER_FLAG);
}

// Puts a datagram about to be sent in the single-submessage form (struct tt_SingleHeader, tt_VERSION 10) when it
// qualifies: exactly one submessage, addressed to everyone. `framing` holds the classic tt_Header and
// tt_SubmessageHeader (framing_len bytes of it, the datagram continuing with body_len more). The submessage
// header's four bytes are rewritten in place into the single header, and the datagram then starts
// sizeof(struct tt_Header) bytes in - the returned offset; 0 leaves it classic. The buffer is the one about
// to be sent, never a cached copy: whatever reads it afterwards reads it from the start again.
static uint32_t to_single_form(uint8_t* framing, uint32_t framing_len, uint32_t body_len) {
    if (framing_len < sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader)) {
        return 0;
    }
    const struct tt_Header* header = (const struct tt_Header*)framing;
    struct tt_SubmessageHeader* submessage = (struct tt_SubmessageHeader*)(framing + sizeof(struct tt_Header));
    if (submessage->receiver != tt_SUBMESSAGE_ID_ALL ||
        submessage->length != framing_len + body_len - sizeof(struct tt_Header)) {
        return 0;
    }
    struct tt_SingleHeader single = {native_single_marker(), header->version, header->source, submessage->type};
    _tt_memcpy(submessage, &single, sizeof(single));
    return sizeof(struct tt_Header);
}

struct tx_datagram {
    const uint8_t* head;
    uint32_t head_len;
    const uint8_t* body;
    uint32_t body_len;
};

// Records who a datagram sent now reaches, for node_update()'s summary skip: every peer when it is
// broadcast (no peers), otherwise the peers it is addressed to. A link's broadcast of an addressed datagram
// also reaches that link's other peers; they are not counted, which only means a summary goes out anyway.
static void send_summary_ahead(struct tt_Context* node);

// The record itself, out of line: only a node whose summaries run at the short-lease cadence gets here.
static void note_reached_armed(struct tt_Context* node, const struct tt_Peer* peers, uint8_t peer_count) {
    if (node->summary_rides != 0) {
        node->summary_rides = 0;
        send_summary_ahead(node); // its own datagram, just ahead of this one
    }
    if (peers == NULL || peer_count == 0) {
        node->reached_everyone = 1;
        return;
    }
    for (uint8_t i = 0; i < peer_count; i++) {
        uint8_t id = peers[i].context_id;
        node->reached_nodes[id / 32U] |= 1U << (id % 32U);
    }
}

// The gate, inline at every send (OPTIMIZATION_PLAN.md 11.5, D5): unarmed - every node without short leases - a send
// pays one load and a branch, where an out-of-line call cost the Pi 4-6 ns a sample.
static inline void note_reached(struct tt_Context* node, const struct tt_Peer* peers, uint8_t peer_count) {
    if (node->summary_skip_armed) {
        note_reached_armed(node, peers, peer_count);
    }
}

// ---------------------------------------------------------------------------------------------
// The transport seam (SHM_PLAN.md stage 0). Every datagram core sends leaves through one of these
// four, and every datagram it receives is counted in one place, so "which transport carried this"
// is asked and answered where the bytes actually move.
//
// Stage 0 puts UDP behind all of them and adds nothing else: the wire is byte-identical to the
// build before the seam, which is the claim that carries this stage. What the seam buys now is the
// counting, and the counting is the point rather than a side effect - a test that asks a mode flag
// "which transport is this peer on" gets a correct answer from a seam that is only half wired,
// because the flag describes the decision and not the traffic. Counts describe the traffic.
//
// Why it wraps the HAL calls rather than sitting where tx_datagrams was counted before: it was
// counted at three places that between them missed publish_zerocopy() (g15), which sent datagrams
// the counter never saw. Per-transport counters placed there would inherit that hole on the
// zero-copy path - the path shared memory has the most to offer - and report shm at zero for a
// payload that really did travel over the segment. Here, coverage is a property of the code's
// shape: a send that does not go through these does not reach a transport at all.
//
// Where the choice is made: `segment_deliver()`, in one place, because it has to assemble the
// datagram anyway and cannot decide "segment" without also deciding it fits a slot. Stage 0 left a
// separate `transport_for()` here that answered the same question from the peer alone; it was never
// called once stage 1 landed, and it is gone rather than kept, since two functions answering one
// question is how the answers come to differ. With the segment compiled out every datagram is udp
// and shm is zero, which is still stage 0's own assertion about the seam being wired.
// The name a reader computes for a peer's segment: (address, port, context id), every field from
// ordinary discovery (struct tt_Peer), so no new discovery mechanism and no wire change. The
// address is what makes it unique where the context id alone is not - two network namespaces share
// /dev/shm but never an address, which is the same reason registry_path() keys on it.
//
// Returns the length written, or a negative value if it would not fit - never a truncated name,
// because a truncated name is a name two different peers could share.
//
// TT_SEGMENT_DIR is a test seam, not a setting: tests/test_segment_owner.c builds segments with the real HAL and a
// real process death, and must not do that in the host's /dev/shm, which other users' contexts share.
#ifndef TT_SEGMENT_DIR
#define TT_SEGMENT_DIR "/dev/shm/"
#endif
static int32_t segment_name(char* buf, size_t size, uint32_t ip, uint16_t port, uint8_t context_id) {
    int written = snprintf(buf, size, TT_SEGMENT_DIR "tickle-seg-%u.%u.%u.%u-%u-%u", (ip >> 24) & MASK_8BIT,
                           (ip >> 16) & MASK_8BIT, (ip >> BITS_IN_1BYTE) & MASK_8BIT, ip & MASK_8BIT, port, context_id);
    return written > 0 && (size_t)written < size ? written : -1;
}

#if tt_SEGMENT_ENABLED && tt_SEGMENT_BELL_FIFO
// The doorbell FIFO's name: the segment's plus a suffix, so a writer that can name the segment can name its bell and
// the two cannot drift apart (tt_segment_bell_create(), hal.h). Same contract as segment_name(): never truncated.
static int32_t bell_name(char* buf, size_t size, uint32_t ip, uint16_t port, uint8_t context_id) {
    char segment[tt_SEGMENT_PATH_LENGTH];
    if (segment_name(segment, sizeof(segment), ip, port, context_id) < 0) {
        return -1;
    }
    int written = snprintf(buf, size, "%s.bell", segment);
    return written > 0 && (size_t)written < size ? written : -1;
}
#endif

#if tt_SEGMENT_ENABLED
// Closes this entry's end of the peer's bell. Every path that forgets a peer calls it before its memset: the memset
// alone would leak the descriptor, and a stale one could later ring a stranger's pipe.
static void peer_bell_close(struct tt_SegmentPeer* entry) {
    if (entry->bell_fd_plus1 > 0) {
        tt_segment_bell_close(entry->bell_fd_plus1 - 1);
        entry->bell_fd_plus1 = 0;
    }
}
#endif

// What a reader checks after attaching, and the reason the name alone is not enough: see
// struct tt_SegmentHeader. `expected_incarnation` is 0 on a first attach, when any incarnation is
// acceptable and the caller records what it found; on a later check it is what was recorded, and a
// different value means the peer we knew has been replaced by one that happens to hold the same
// context id.
static enum tt_SegmentAttach segment_header_check(const struct tt_SegmentHeader* header, uint32_t ip, uint16_t port,
                                                  uint8_t context_id, uint32_t expected_incarnation) {
    // ACQUIRE, and everything below depends on it. create_own_segment() writes every other field and
    // seeds every slot's sequence, and only then stores the magic with __ATOMIC_RELEASE - so the magic
    // is the barrier that publishes the rest. A release store pairs with an acquire load and with
    // nothing else: read this with a plain load and the compiler or the processor may hand back a
    // magic that is already set beside a `slots` that is still zero, which is a header believed and a
    // ring that cannot be indexed.
    //
    // This was written as a plain load and was safe only by accident: the segment used to be created
    // at bind, before any peer could possibly attach, so the two never overlapped. Deferred creation
    // makes "a peer attaches while the owner is still building it" the ordinary case, and
    // ThreadSanitizer reported it as a real race between create_own_segment() and this function the
    // first time the two could run at once.
    if (__atomic_load_n(&header->magic, __ATOMIC_ACQUIRE) != tt_SEGMENT_MAGIC ||
        header->version != tt_SEGMENT_VERSION) {
        return tt_SEGMENT_BAD_HEADER;
    }
    if (header->owner_ip != ip || header->owner_port != port || header->owner_context_id != context_id) {
        return tt_SEGMENT_WRONG_OWNER;
    }
    // The geometry, which until 2026-10-02 nothing checked. segment_slot() strides by
    // header->slot_bytes and masks by header->slots - the OWNER's numbers - while peer_segment() maps
    // segment_bytes(tt_SEGMENT_SLOTS, tt_SEGMENT_SLOT_BYTES), OURS. Both are #ifndef-guarded, so two
    // processes built with different -D are a configuration somebody can reach.
    //
    // Half of it was already covered, in the HAL rather than here, which is why reading this function
    // alone did not show it: tt_segment_attach() fstats the file and refuses one SMALLER than the
    // length asked for. That direction fails cleanly today - measured, 2026-10-02: tx_shm=0, every
    // sample over UDP, nothing lost.
    //
    // The other direction is invisible from the file size, because a LARGER file looks fine - and it
    // does not fault, which is what both of the predictions made before the measurement got wrong. The
    // writer fills the slots its short mapping can address, then reads a slot header it cannot see,
    // finds a sequence that is not the index it claimed, and returns "ring full". write_index never
    // advances past that slot, so the ring is full for ever after. Measured with a 512-slot owner and
    // a 256-slot attacher: 259 datagrams over shared memory, then **shm_full_dropped=1535 of 2000
    // messages, silently** - drop-on-full discards rather than rerouting - while the application was
    // told "sent 2,000 message(s)" and the process exited 0. Its subscriber received 536.
    //
    // That was refused outright between c97fac8b and this commit - any mismatch fell back to UDP, which was
    // right while the attacher mapped ITS OWN length. It no longer does: peer_segment() reads the geometry from
    // the header and maps what the owner actually built, so a different geometry is now something we handle
    // rather than something that wedges us. A pair whose slots are smaller than our datagrams simply sends the
    // ones that do not fit over UDP, counted in segment_oversized_to_udp, which is already the documented
    // behaviour for a sample larger than a slot.
    //
    // What stays refused is a geometry that cannot be indexed at all, because segment_slot() masks with
    // slots - 1: zero slots makes that mask 0xFFFFFFFF and every index wild, and a non-power-of-two makes the
    // mask skip addresses that exist. Neither is something create_own_segment() can produce, so meeting one
    // means the header is corrupt or is not ours - and test_transport_seam.c's valid_header() was building
    // exactly that (slots = 0, slot_bytes = 0) and calling it valid until 2026-10-02.
    if (header->slots == 0 || (header->slots & (header->slots - 1U)) != 0 || header->slot_bytes == 0) {
        return tt_SEGMENT_BAD_HEADER;
    }
    if (expected_incarnation != 0 && header->incarnation != expected_incarnation) {
        return tt_SEGMENT_STALE;
    }
    return tt_SEGMENT_ATTACHED;
}

// The ring. Single writer (the segment's owner) and single reader (the peer that attached), which
// is what lets the two indices be ordinary loads and stores with acquire/release rather than a
// lock: each index has exactly one writer, and each side reads the other's.
//
// THE RULE WHOSE FAILURE IS SILENT: a writer must never reuse a slot the reader has not released.
// Every other error here announces itself - a refused write returns false, a bad header is a
// counted attach failure - but overwriting a slot in flight hands the reader a datagram that
// changes underneath it, and it is detected by nothing. So the writer refuses when the ring is
// full, which costs a fallback to UDP for that datagram, and the test for it sits beside this code
// rather than after it.
static uint8_t* segment_slot(struct tt_SegmentHeader* header, uint32_t index) {
    uint8_t* base = (uint8_t*)header + sizeof(*header);
    size_t stride = sizeof(struct tt_SegmentSlot) + header->slot_bytes;
    return base + ((size_t)(index & (header->slots - 1U)) * stride);
}

// The slot size this context builds its own segment with: the user's if they set one, the compiled default
// otherwise. One place interprets the sentinel, so "did the user choose this?" is asked where it is answered
// and not re-derived at each use.
static uint32_t own_slot_bytes(void) {
    return _tt_CONFIG.segment_slot_bytes != 0 ? _tt_CONFIG.segment_slot_bytes : (uint32_t)tt_SEGMENT_SLOT_BYTES;
}

// Is a user-set slot size one this context can build a segment from at all? Checked at creation, where the
// answer is the same for every topic and the user is still in a position to change it.
//
// WHAT THIS DOES NOT CHECK, said here because the gap is the interesting half. The user's decision of
// 2026-10-02 was that a user who SETS this and then has a type too large for it should get an error from
// tt_Context_create_publisher(), while a user on the default gets the segment_oversized_to_udp counter -
// correctness against performance. The per-topic half of that is not here, and not because it was
// forgotten: struct tt_Topic carries data_size (the C struct) and data_encode_size (a function OF a sample),
// and neither is the maximum ENCODED size. For a sequence or a string the encoded size depends on how full
// the sample is, so calling data_encode_size() on a zeroed instance would return the minimum and pass a type
// that does not fit when populated - a check that reads as a guarantee and is not one. The maximum is known
// one layer up, in rosidl_typesupport_tickle_c's capacity profile, and the per-topic check belongs there.
// Core checks what core knows exactly.
// The largest record that goes whole: one control datagram. Both the point where a sample starts
// fragmenting and the ceiling on a configured segment slot, declared once here because they are the
// same bound and were briefly two.
#define FRAG_WHOLE_DATA_LIMIT tt_CONTROL_MAX_LENGTH

static bool valid_slot_bytes(uint32_t slot_bytes) {
    // Below a datagram header nothing can be placed at all. The ceiling is a datagram's worth rather than
    // tt_MAX_SAMPLE_LENGTH, because a slot larger than a datagram is only useful for carrying a record whole
    // - SHM_PLAN 6e(a).
    //
    // THE MEASUREMENT THIS CEILING WAS PUT HERE FOR WAS MEASURING A BUG, and is corrected in place
    // rather than deleted because the reading drawn from it was published too. It said: "2713 Mbps
    // at the default slot against 0.8 at 4096, the ring full 934 times and RELIABLE never
    // recovering", and concluded that a whole record costs about 2.7x the ring bytes so samples
    // and acks starve each other. The 0.8 was not ring economics. At 4096 a sample travels whole,
    // and until 2026-10-03 nothing read the span the slot carried, so the reader advanced its
    // watermark by one per record and waited for a seq_no the publisher had already spent. The
    // cell delivered one sample out of fifteen thousand received records with every loss counter
    // at zero. Re-measured on the fixed build, same harness, same run, control included:
    //
    //     1472 (default)  2732 Mbps   2.01 slots/sample   recv = sent, loss 0
    //     4096            2713 Mbps   1.01 slots/sample   recv = sent, loss 0
    //
    // So a whole record costs HALF the slots and delivers the same rate - 0.99x, three reps. The
    // starvation argument does not survive, and neither does "3000x worse".
    //
    // The ceiling stays anyway, and for a plainer reason: there is no measured benefit. Halving
    // slots per sample bought nothing, so raising the runtime knob would hand an application a
    // larger per-slot memory cost for a throughput that does not move. 6e(b) - the encoder writing
    // into the slot - remains the prerequisite for the memory side of the argument, and if it
    // lands, this is re-measured again rather than assumed. A build may still set
    // tt_SEGMENT_SLOT_BYTES higher to measure that path (whole_record_refusal.sh does).
    return slot_bytes >= sizeof(struct tt_Header) && slot_bytes <= FRAG_WHOLE_DATA_LIMIT;
}

// Bytes a segment of this shape occupies, so the writer that creates it and the reader that maps it
// agree without either one recomputing the layout from parts.
static size_t segment_bytes(uint32_t slots, uint32_t slot_bytes) {
    return sizeof(struct tt_SegmentHeader) + ((size_t)slots * (sizeof(struct tt_SegmentSlot) + slot_bytes));
}

// One datagram into the ring. False when it will not fit the slot, or when the ring is full - both
// leave the ring untouched and both mean "send this over UDP instead", which is the module's own
// documented failure mode rather than an error.
// The record is given in its two pieces and assembled IN THE SLOT. segment_deliver() used to join them
// in a stack buffer of tt_SEGMENT_SLOT_BYTES - our own compile-time constant - while the size it checked
// against was the PEER's slot_bytes, read from the mapped header. Two deciders of one bound: a peer built
// with a larger slot than ours accepted a record our buffer could not hold, and a 2068-byte record into a
// 1472-byte frame smashed the stack (found 2026-10-03, the return addresses were the payload's own fill
// byte). Writing the pieces straight into the slot removes the second number and the buffer with it.
//
// Safe because a slot is not published until the release store of `sequence` below: a reader cannot see a
// half-written record whether it was written in one memcpy or two.
//
// Split into a claim and a publish (SHM_PLAN 6e(b), 2026-10-06) so that a publisher can encode a sample straight
// into the slot between the two, rather than into tx_buffer and then copying it here. THE RULE THAT SPLIT CREATES:
// a claimed slot must be published, whatever happens in between. The ring has many writers, so a claim cannot be
// taken back - write_index has already moved past it and another writer may have claimed the next one - and the
// single reader takes records in index order, so an index claimed and never published stops the segment for every
// writer (segment_head_stalls). A writer with nothing to put in it publishes a zero-length record, which the reader
// releases like any other and the receive path drops before parsing (process_datagram_locked(): len 0 is a doorbell).

// Claims the next slot of `header`, or NULL when the ring is full - the oldest slot still in flight, or the slot at
// the claimed index not yet released by the reader. `*claimed` is the index won, which segment_publish() needs.
static struct tt_SegmentSlot* segment_claim(struct tt_SegmentHeader* header, uint32_t* claimed_out) {
    // Claim an index. Many peers write into one context's segment, so this is a compare-and-exchange
    // rather than a load and a store: two writers that both read the same index would both fill the
    // same slot, losing one record and writing the other twice, with nothing to report it.
    uint32_t claimed = __atomic_load_n(&header->write_index, __ATOMIC_RELAXED);
    struct tt_SegmentSlot* slot_header = NULL;
    for (;;) {
        uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_ACQUIRE);
        if (claimed - read_index >= header->slots) {
            return NULL; // full: the oldest slot is still in flight and is not ours to reuse
        }
        slot_header = (struct tt_SegmentSlot*)segment_slot(header, claimed);
        // The slot must also be free by its own reckoning. A reader releases a slot by setting its
        // sequence to the index one lap ahead, so this is what says the previous occupant has gone -
        // the index alone cannot, since the reader moves read_index before any particular slot is
        // reusable in a multi-writer ring.
        if (__atomic_load_n(&slot_header->sequence, __ATOMIC_ACQUIRE) != claimed) {
            return NULL;
        }
        if (__atomic_compare_exchange_n(&header->write_index, &claimed, claimed + 1U, true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break; // ours; `claimed` is the index we won
        }
        // Lost the race: __atomic_compare_exchange_n has reloaded `claimed` with the current value.
    }
    *claimed_out = claimed;
    return slot_header;
}

// The payload of a claimed slot, where the record is written before segment_publish().
static uint8_t* segment_slot_payload(struct tt_SegmentSlot* slot_header) {
    return (uint8_t*)slot_header + sizeof(*slot_header);
}

// Publishes a slot segment_claim() returned, its `len` payload bytes already written. len 0 is the harmless record.
static void segment_publish(struct tt_SegmentSlot* slot_header, uint32_t claimed, uint32_t len, uint32_t sender_ip,
                            uint16_t sender_port, uint16_t seq_span) {
    slot_header->length = len;
    slot_header->sender_ip = sender_ip;
    slot_header->sender_port = sender_port;
    // The publisher's number, not recomputed here. Checked against a bound with its own source of
    // truth: a span above tt_FRAG_MAX_COUNT is a record the network form could not have carried at all,
    // since frag_count counts the same datagrams in a uint8_t. Out of range means the value did not come
    // from the arithmetic that should have produced it, so take the one that is always safe.
    slot_header->seq_span = (seq_span >= 1 && seq_span <= tt_FRAG_MAX_COUNT) ? seq_span : 1;
    // Release: everything above must be visible before the sequence that publishes it. The reader
    // takes this slot exactly when it sees claimed + 1 here, so a writer that finished later than a
    // writer with a higher index cannot make the reader read an unwritten slot.
    __atomic_store_n(&slot_header->sequence, claimed + 1U, __ATOMIC_RELEASE);
}

static bool segment_write(struct tt_SegmentHeader* header, const void* hdr, uint32_t hdr_len, const void* body,
                          uint32_t body_len, uint32_t sender_ip, uint16_t sender_port, uint16_t seq_span) {
    const uint32_t len = hdr_len + body_len;
    if (len > header->slot_bytes) {
        return false;
    }
    uint32_t claimed = 0;
    struct tt_SegmentSlot* slot_header = segment_claim(header, &claimed);
    if (slot_header == NULL) {
        return false;
    }
    uint8_t* payload = segment_slot_payload(slot_header);
    memcpy(payload, hdr, hdr_len);
    if (body_len != 0) {
        memcpy(payload + hdr_len, body, body_len);
    }
    segment_publish(slot_header, claimed, len, sender_ip, sender_port, seq_span);
    return true;
}

// One datagram out of the ring, copied (stage 1 copies on arrival; lending is stage 2). False when
// the ring is empty. `size` is the caller's buffer, and a record larger than it is refused rather
// than truncated - a truncated datagram would be handed to the acceptance path as if it were whole.
//
// One reader, so read_index needs no compare-and-exchange; what it does need is the slot's own
// sequence, because with many writers a published write_index does not mean this slot is filled.
static bool segment_read(struct tt_SegmentHeader* header, void* buf, uint32_t size, uint32_t* len, uint32_t* sender_ip,
                         uint16_t* sender_port, uint16_t* seq_span) {
    uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_RELAXED); // ours to move
    struct tt_SegmentSlot* slot_header = (struct tt_SegmentSlot*)segment_slot(header, read_index);
    if (__atomic_load_n(&slot_header->sequence, __ATOMIC_ACQUIRE) != read_index + 1U) {
        return false; // empty, or the writer that claimed this slot has not finished with it
    }

    uint8_t* slot = (uint8_t*)slot_header;
    uint32_t length = slot_header->length;
    bool usable = length <= header->slot_bytes && length <= size;
    if (usable) {
        memcpy(buf, slot + sizeof(*slot_header), length);
        *len = length;
        *sender_ip = slot_header->sender_ip;
        *sender_port = slot_header->sender_port;
        // "0 means 1", the field's own convention (tt_SegmentSlot.seq_span), and a writer from
        // before version 3 leaves zeroes here. Anything above the wire bound is a slot that does
        // not agree with the protocol, and is read as 1 rather than trusted: believing it would
        // skip the reader's watermark past seq_nos that do exist.
        uint16_t span = slot_header->seq_span;
        *seq_span = (span >= 1 && span <= tt_FRAG_MAX_COUNT) ? span : 1;
    }
    // Released either way, so a record this mapping could not hold cannot wedge the ring. The slot
    // is marked free for the writer one lap ahead, and only then does read_index move - a writer
    // checks the sequence, so releasing in this order is what stops it reusing a slot still being
    // copied out of.
    __atomic_store_n(&slot_header->sequence, read_index + header->slots, __ATOMIC_RELEASE);
    __atomic_store_n(&header->read_index, read_index + 1U, __ATOMIC_RELEASE);
    return usable;
}

#if tt_SAMPLE_LENDING
// segment_read() without the copy (receive-buffer lending, DESIGN.md section 10): the head record, read where it lies
// in its slot. False when the ring is empty or its head is unfinished; a record longer than a slot is released unread
// and false returned, exactly as segment_read() treats one.
//
// read_index moves past the record here, before it is processed, exactly when segment_read() moved it: writers and
// the ring's own watchers (bell_wake_check reads "consumed" off it) see the same index at the same moment as with
// the copy. What keeps the slot is its sequence, left at "published": no writer claims it until segment_done() - or,
// if a sample in it is retained, tt_Sample_release() - sets it one lap ahead.
static bool segment_take(struct tt_SegmentHeader* header, uint8_t** record, uint32_t* index, uint32_t* len,
                         uint32_t* sender_ip, uint16_t* sender_port, uint16_t* seq_span) {
    uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_RELAXED); // ours to move
    struct tt_SegmentSlot* slot_header = (struct tt_SegmentSlot*)segment_slot(header, read_index);
    if (__atomic_load_n(&slot_header->sequence, __ATOMIC_ACQUIRE) != read_index + 1U) {
        return false;
    }
    uint32_t length = slot_header->length;
    if (length > header->slot_bytes) {
        __atomic_store_n(&slot_header->sequence, read_index + header->slots, __ATOMIC_RELEASE);
        __atomic_store_n(&header->read_index, read_index + 1U, __ATOMIC_RELEASE);
        return false;
    }
    *record = (uint8_t*)slot_header + sizeof(*slot_header);
    *index = read_index;
    *len = length;
    *sender_ip = slot_header->sender_ip;
    *sender_port = slot_header->sender_port;
    uint16_t span = slot_header->seq_span; // as segment_read(): 0 and out-of-range spans read as 1
    *seq_span = (span >= 1 && span <= tt_FRAG_MAX_COUNT) ? span : 1;
    __atomic_store_n(&header->read_index, read_index + 1U, __ATOMIC_RELEASE);
    return true;
}

// Whether a retained sample still holds ring slot `index` of `region`.
static bool lend_holds_slot(const struct tt_Context* node, const void* region, uint32_t index) {
    for (uint32_t k = 0; k < tt_SAMPLE_RETAIN_MAX; k++) {
        const struct tt_LendEntry* entry = &node->lend.entries[k];
        if (entry->kind == tt_LEND_SLOT && entry->index == index && entry->region == region) {
            return true;
        }
    }
    return false;
}

// A record segment_take() handed out, processed: its slot goes back to the writers, after every read of it - unless
// a sample in it was retained. The slot then keeps the sequence that says "published", which no writer claims, until
// tt_Sample_release() frees it with index + slots (lend_release_slot()). Writers claim in index order, so the ring
// runs on past it for one lap and is full from there for every writer (DESIGN.md).
static void segment_done(struct tt_Context* node, struct tt_SegmentHeader* header, uint32_t index) {
    if (node->lend.held_slots != 0 && lend_holds_slot(node, header, index)) {
        return;
    }
    __atomic_store_n(&((struct tt_SegmentSlot*)segment_slot(header, index))->sequence, index + header->slots,
                     __ATOMIC_RELEASE);
}

// Whether a writer's refusal by `header` is its reader holding a slot: the slot at write_index was published one lap
// ago (sequence index - slots + 1), and the reader has read past it (the ring is not full by the indices). A racy
// look, for a counter only; the writer reads the same three words to refuse.
static bool segment_full_by_hold(struct tt_SegmentHeader* header) {
    uint32_t claimed = __atomic_load_n(&header->write_index, __ATOMIC_RELAXED);
    uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_ACQUIRE);
    const struct tt_SegmentSlot* slot_header = (const struct tt_SegmentSlot*)segment_slot(header, claimed);
    return claimed - read_index < header->slots &&
           __atomic_load_n(&slot_header->sequence, __ATOMIC_ACQUIRE) == claimed - header->slots + 1U;
}
#endif

// Counted where it happens rather than by the caller, so a new attach path cannot forget to - the
// same reason the transport counts live in the seam and not at the twelve send sites.
static void note_attach(struct tt_Context* node, enum tt_SegmentAttach reason) {
    node->segment_attach[reason]++;
}

#if tt_SEGMENT_ENABLED
// A peer asked about and found to have no segment for us. Kept against the address it was asked
// about, so the same id at a different address is asked about at once rather than inheriting this
// answer, and with a countdown rather than a flag, so "no" is temporary by construction.
// A peer's entry in segment_peers[], zeroed the first time it is wanted rather than at reset (tt_Context's
// segment_peer_live[]). Every write to the table goes through here; every read of an entry that may never have been set
// up goes through segment_peer_if_live(), which answers NULL - "all zero" - for it. So a context whose peers are all on
// other hosts writes the entries of the peers it sends to and leaves the other pages of the 14 KB table untouched,
// which zeroing the whole table at create did not (stage 1 / S1: the module on and unused must cost nothing).
static struct tt_SegmentPeer* segment_peer(struct tt_Context* node, uint8_t context_id) {
    struct tt_SegmentPeer* entry = &node->segment_peers[context_id];
    if (node->segment_peer_live[context_id] == 0) {
        memset(entry, 0, sizeof(*entry));
        node->segment_peer_live[context_id] = 1;
    }
    return entry;
}

static const struct tt_SegmentPeer* segment_peer_if_live(const struct tt_Context* node, uint8_t context_id) {
    return node->segment_peer_live[context_id] != 0 ? &node->segment_peers[context_id] : NULL;
}

// An entry handed back: its bell closed, and the entry no longer set up, so it reads as zero from here on.
static void segment_peer_clear(struct tt_Context* node, uint8_t context_id) {
    if (node->segment_peer_live[context_id] == 0) {
        return; // never set up: nothing in it, and its bytes are not ours to read
    }
    peer_bell_close(&node->segment_peers[context_id]);
    memset(&node->segment_peers[context_id], 0, sizeof(node->segment_peers[context_id]));
    node->segment_peer_live[context_id] = 0;
}

static void remember_absent(struct tt_SegmentPeer* entry, uint32_t ip, uint16_t port) {
    entry->mapping = NULL;
    entry->incarnation = 0;
    entry->ip = ip;
    entry->port = port;
    entry->missing = true;
    entry->recheck_in = tt_SEGMENT_ATTACH_RETRY_SENDS;
}

// The peer's segment, attached on first use and kept. NULL when this peer is not reachable that way
// - another host, no module, or a segment we refused - and the reason is counted by the caller.
//
// Attaching lazily rather than at discovery keeps the decision where the traffic is: a peer we
// never send to costs no mapping, and a peer that appears and disappears costs one attempt rather
// than a subscription to its lifecycle. The cached entry is keyed by context id and remembers the
// address it was named from, because a peer that reappears at a different address is a different
// segment and must not be reached through this one.
//
// **Both answers are cached, and the negative one was not.** The paragraph above described "one
// attempt" as though it were implemented, and for a peer with no segment it was one attempt *per
// datagram*: the failure path wrote nothing to the entry, so the next send recomputed the name and
// called tt_segment_attach() again. On another host that is an open() that walks /dev/shm and
// fails, and it cost half of TickLE's cross-host throughput and doubled CPU per sample between
// 9dbffd40 and d4413383 - with CycloneDDS flat across the same runs, so it was ours. The comment
// was the specification and the code was the bug. A miss is now remembered for
// tt_SEGMENT_ATTACH_RETRY_SENDS sends: long enough that the cost disappears, short enough that a
// peer which binds later still becomes attachable.
static void ensure_own_segment(struct tt_Context* node);

// Maps a peer's segment in two steps, because the length to map is inside the thing being mapped: the header
// first, on its own, and then the region at the size the OWNER built. A single attach could only ask for our own
// geometry and would then index with the owner's, which is what wedged a mismatched pair - with a larger owner
// the writer filled what its short mapping could address, read a slot header it could not see, got a sequence
// that was not the index it claimed, and reported the ring full for ever: 1,535 of 2,000 datagrams silently
// dropped, exit 0 (measured 2026-10-02, examples/perf_hil/experiments/segment_geometry_mismatch.sh). Two opens
// instead of one, on a path that runs once per peer and is cached, not once per datagram.
//
// NULL with *out_verdict set on every refusal, so the caller has one failure path and this has one job.
static struct tt_SegmentHeader* attach_peer_segment(const char* path, uint32_t ip, uint16_t port, uint8_t context_id,
                                                    size_t* out_bytes, enum tt_SegmentAttach* out_verdict) {
    uint8_t why = (uint8_t)tt_SEGMENT_ABSENT;
    struct tt_SegmentHeader* probe = tt_segment_attach(path, sizeof(struct tt_SegmentHeader), &why);
    if (probe == NULL) {
        *out_verdict = (enum tt_SegmentAttach)why;
        return NULL;
    }
    // Checked on the probe before its numbers size anything: a geometry has to be one that can be indexed
    // before it can be trusted to say how much to map.
    enum tt_SegmentAttach verdict = segment_header_check(probe, ip, port, context_id, 0);
    size_t bytes = segment_bytes(probe->slots, probe->slot_bytes);
    tt_segment_detach(probe, sizeof(struct tt_SegmentHeader));
    if (verdict != tt_SEGMENT_ATTACHED) {
        *out_verdict = verdict;
        return NULL;
    }

    struct tt_SegmentHeader* mapping = tt_segment_attach(path, bytes, &why);
    if (mapping == NULL) {
        *out_verdict = (enum tt_SegmentAttach)why;
        return NULL;
    }
    // Again on the full mapping, not only on the probe. Between the two opens the owner could have exited and a
    // new one created a segment at the same name with a different geometry, and the numbers that sized this
    // mapping would then describe something that is gone - the same reason the incarnation is checked at all.
    verdict = segment_header_check(mapping, ip, port, context_id, 0);
    if (verdict == tt_SEGMENT_ATTACHED && segment_bytes(mapping->slots, mapping->slot_bytes) != bytes) {
        verdict = tt_SEGMENT_STALE;
    }
    if (verdict != tt_SEGMENT_ATTACHED) {
        tt_segment_detach(mapping, bytes);
        *out_verdict = verdict;
        return NULL;
    }
    *out_bytes = bytes;
    *out_verdict = tt_SEGMENT_ATTACHED;
    return mapping;
}

static struct tt_SegmentHeader* peer_segment(struct tt_Context* node, uint8_t context_id, uint32_t ip, uint16_t port) {
    if (context_id == tt_CONTEXT_ID_INVALID) {
        return NULL; // a broadcast has no single peer, so no name to compute
    }
    if (context_id == node->id) {
        // Ourselves. A context unicasts to itself and then attaches to the file it created, which is
        // why release_segments() has to unmap that region exactly once - so with the segment built on
        // demand rather than at bind, this is the edge that keeps self-delivery on shared memory. It
        // is also the only trigger a context alone on its host ever has, which is the point: it pays
        // for a segment when it uses one, not because it started.
        ensure_own_segment(node);
    }
    struct tt_SegmentPeer* entry = segment_peer(node, context_id);
    if (entry->mapping == NULL && entry->missing) {
        if (entry->ip != ip || entry->port != port) {
            peer_bell_close(entry);
            memset(entry, 0, sizeof(*entry)); // a different peer behind this id: ask about that one now
        } else if (entry->recheck_in > 0) {
            entry->recheck_in--;
            return NULL; // asked recently; the answer does not change between two datagrams
        }
    }
    if (entry->mapping != NULL) {
        // Asked and answered as two questions rather than a chain of four branches: three of those
        // branches did the same two things and differed only in what was worth counting, which is a
        // clone however it is written out.
        const bool same_address = entry->ip == ip && entry->port == port;
        const bool same_owner = same_address && entry->mapping->incarnation == entry->incarnation;
        if (same_owner && entry->recheck_in > 0) {
            entry->recheck_in--;
            return entry->mapping; // the common case, and the only one that costs nothing
        }
        if (same_address && !same_owner) {
            // Replaced in place by an owner holding the same id at the same address. Counted,
            // because this is what the incarnation is for - though it is worth being honest that
            // tt_segment_create() unlinks before it creates, so a successor gets a *new* file while
            // this mapping keeps the old one. What actually notices a replaced owner is the recheck
            // below; this covers a hypothetical in-place reuse and is not the mechanism to rely on.
            note_attach(node, tt_SEGMENT_STALE);
        }
        // Given up, for whichever of the three reasons brought us here: a different address behind
        // this id, an owner replaced, or simply that the recheck has fallen due. That last one is
        // not housekeeping - an owner killed since we attached leaves this region mapped, readable,
        // and carrying the incarnation we recorded, so nothing read *through* this mapping can ever
        // report its death. Only asking the name again can, and a peer that never asks writes into
        // a ring nobody drains for as long as it lives. The file at that name is now either gone,
        // the same one, or the successor's, and the attach below answers all three.
        // The dead-reader clock survives this, and that is the whole of a defect shipped in
        // f938461e. A revalidation is bookkeeping about the MAPPING; it says nothing about whether
        // the reader is consuming. Clearing it here - and the entry is recomputed every
        // tt_SEGMENT_REVALIDATE_SENDS sends - restarted the clock about seventy times a second on a
        // writer sending three hundred thousand datagrams a second, so it could never reach
        // tt_SEGMENT_DEAD_READER_NS and the rule was unreachable on every writer that matters.
        // Measured: a writer against a reader killed with SIGKILL reported shm_gave_up=0 for the
        // whole run.
        //
        // Carried across only when the address is unchanged. A different peer behind this id is a
        // different relationship and starts its own clock.
        uint64_t carried_progress = same_address ? entry->last_progress_ns : 0;
        tt_segment_detach(entry->mapping, entry->mapped_bytes);
        peer_bell_close(entry);
        memset(entry, 0, sizeof(*entry));
        entry->last_progress_ns = carried_progress;
    }

    char path[tt_SEGMENT_PATH_LENGTH];
    if (segment_name(path, sizeof(path), ip, port, context_id) < 0) {
        note_attach(node, tt_SEGMENT_BAD_HEADER); // a name we cannot form is a segment we cannot find
        remember_absent(entry, ip, port);
        return NULL;
    }
    size_t bytes = 0;
    enum tt_SegmentAttach verdict = tt_SEGMENT_ABSENT;
    struct tt_SegmentHeader* header = attach_peer_segment(path, ip, port, context_id, &bytes, &verdict);
    if (header == NULL) {
        note_attach(node, verdict);
        remember_absent(entry, ip, port);
        return NULL;
    }
    entry->mapped_bytes = bytes;
    note_attach(node, tt_SEGMENT_ATTACHED);
    entry->mapping = header;
    entry->ip = ip;
    entry->port = port;
    entry->incarnation = header->incarnation;
    entry->missing = false;
    // last_progress_ns is NOT cleared here: a fresh entry has it at zero from the memset, and a
    // re-attach carries the reader's own clock across (above). Setting it here was the other half of
    // the same defect.
    entry->recheck_in = tt_SEGMENT_REVALIDATE_SENDS;
    // Only ever raised: a ceiling too high costs the walk record_size_limit() skips; one too low would cap a record
    // below what a peer can take.
    if (header->slot_bytes > node->segment_slot_ceiling) {
        node->segment_slot_ceiling = header->slot_bytes;
    }
#if tt_SEGMENT_BELL_FIFO
    char bell[tt_SEGMENT_PATH_LENGTH];
    if (bell_name(bell, sizeof(bell), ip, port, context_id) >= 0) {
        int32_t bell_fd = tt_segment_bell_open(bell);
        entry->bell_fd_plus1 = bell_fd >= 0 ? bell_fd + 1 : 0; // none: this peer is rung over UDP
    }
#endif
    return header;
}
#endif

#if tt_SEGMENT_ENABLED
// This context's own segment: the one its peers attach to. Named from its own (address, port,
// context id), which is what a peer computes from what discovery told it.
static void create_own_segment(struct tt_Context* node) {
    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(node, &own_ip, &own_port);

    char path[tt_SEGMENT_PATH_LENGTH];
    if (segment_name(path, sizeof(path), own_ip, own_port, node->id) < 0) {
        return;
    }
    void* mapping = tt_segment_create(path, segment_bytes(tt_SEGMENT_SLOTS, own_slot_bytes()));
    if (mapping == NULL) {
        return; // no segment: peers reach this context over UDP, counted as unattached on their side
    }

    struct tt_SegmentHeader* header = mapping;
    // The ring's fields before the magic, so a peer that attaches between these two writes cannot
    // see a header it believes and a ring it cannot index. The magic is the last thing written and
    // the first thing checked.
    header->version = tt_SEGMENT_VERSION;
    header->owner_ip = own_ip;
    header->owner_port = own_port;
    header->owner_context_id = node->id;
    header->incarnation = node->entity_id_base != 0 ? node->entity_id_base : (uint32_t)tt_get_ns();
    header->slots = tt_SEGMENT_SLOTS;
    header->slot_bytes = own_slot_bytes();
    header->write_index = 0;
    header->read_index = 0;
    // Each slot starts free for the writer of its own index. Zeroed memory would leave slot 0
    // claimable and every other slot permanently not - a ring of one, which would have looked like
    // a working module carrying a trickle.
    for (uint32_t index = 0; index < header->slots; index++) {
        ((struct tt_SegmentSlot*)segment_slot(header, index))->sequence = index;
    }
#if tt_SEGMENT_BELL_FIFO
    // The bell before the magic: a writer opens the bell once, right after it has checked the magic (attach), so a
    // bell created after the magic missed every writer that attached in between, and each of them rang this context
    // over UDP for as long as it stayed attached - slower, still correct, and what failed bell_wake_check in CI on
    // 2026-10-07 (2,323 of 59,759 rings over UDP). A writer cannot find the bell without the segment anyway: it only
    // looks for it after the magic. Without a bell, peers ring over UDP.
    char bell[tt_SEGMENT_PATH_LENGTH];
    if (bell_name(bell, sizeof(bell), own_ip, own_port, node->id) >= 0) {
        (void)tt_segment_bell_create(node, bell);
    }
#endif
    __atomic_store_n(&header->magic, tt_SEGMENT_MAGIC, __ATOMIC_RELEASE);
    node->own_segment = header;
    node->segments_created++;
    node->segment_stretch_index = 0; // a new ring counts from zero: no pace carries over from the last one
    node->segment_stretch_ns = tt_get_ns();
    node->segment_epoch_ns = 0; // and no epoch: the polling thread begins one at its first decision
    // Nor a generation kept from a sleep called off on the last segment: a ring for it went to the last bell.
    node->segment_generation_unspent = 0;
}

// Whether this context has a segment for peers to write into, building it if a same-host peer has
// appeared and it does not exist yet. Idempotent, because both edges that can want it - discovery
// learning of a peer, and a send that needs our own ring - arrive independently and in either order.
static void ensure_own_segment(struct tt_Context* node) {
    if (node->own_segment == NULL) {
        create_own_segment(node);
    }
}

// Discovery's appearing edge. A peer at our own address is one that can open the file we create; one
// anywhere else never will, and building a segment for it would be the eager behaviour under another
// name. Our own id is not a peer here: a context that delivers to itself gets its segment from
// peer_segment() instead, so a node alone on a host still builds one only if it actually needs it.
static void note_same_host_peer(struct tt_Context* node, uint8_t context_id, uint32_t peer_ip) {
    if (context_id == tt_CONTEXT_ID_INVALID || context_id == node->id) {
        return;
    }
    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(node, &own_ip, &own_port);
    // own_ip 0 means the address is not known yet, and every peer_ip would compare equal to it.
    if (own_ip == 0 || peer_ip != own_ip) {
        return;
    }
    if (!node->same_host_peer[context_id]) {
        node->same_host_peer[context_id] = true;
        node->same_host_peer_count++;
    }
    ensure_own_segment(node);
#if tt_SAMPLE_LENDING
    node->lend.segment_release_deferred = false; // a same-host peer again: the segment is wanted after all
#endif
    // And let a cached "no" about this peer expire now rather than in tt_SEGMENT_ATTACH_RETRY_SENDS
    // sends. Measured 2026-10-02 (COMPARISON 2.2c): in a same-host ping/pong every repetition read
    // shm_attach_absent=1, tx_udp_unattached=257, shm_attach_ok=1 - the first attach lost a race with
    // the peer building its own segment, and the countdown then held the answer for 256 more sends. An
    // exchange shorter than that never used shared memory at all, which is the shape of a request/reply
    // service. The countdown is right for the case it was built for, a peer on another host that will
    // never have a segment; it is wrong here, because this function is reached only for a peer at our
    // own address, which is exactly the peer about to have one. Discovery already knew what the sender
    // was waiting for and had no way to say it.
    //
    // Only the negative answer. A "yes" expires for its own reason - an owner that was killed leaves
    // the mapping intact and only asking the name again can tell - and clearing that here would make
    // every attached peer re-verify once per announce, which is a cost with no finding behind it.
    //
    // The bound on the new cost: an announce, not a datagram. A same-host peer that genuinely never
    // builds a segment is re-asked once per announce interval, against the once-per-datagram open()
    // that cost half our cross-host throughput and motivated the cache. peer_segment() still compares
    // the address itself, so an entry cached for a different (ip, port) is reset there as before.
    // An entry never set up holds no answer to expire.
    if (node->segment_peer_live[context_id] != 0) {
        struct tt_SegmentPeer* entry = &node->segment_peers[context_id];
        if (entry->mapping == NULL && entry->missing) {
            entry->recheck_in = 0;
        }
    }
}

// Everything this context mapped, handed back. Nothing else does it: a segment is a file in
// /dev/shm, so a context that exits without unlinking leaves it there for good. The leak hides in
// a test because create unlinks first, so the next context at the same address and id replaces the
// stale file and sees nothing wrong - it is only unbounded in a system that runs long enough.
//
// Peers' mappings are unmapped and never unlinked: those files belong to those peers and are still
// being read by them. Only this context's own segment is this context's to remove.
// The owner's doorbell goes with its segment, named from the header for release_own_segment()'s reason: the id
// may have moved since, and the file carries the one it was created under.
static void release_own_bell(struct tt_Context* node, const struct tt_SegmentHeader* own) {
#if tt_SEGMENT_BELL_FIFO
    char bell[tt_SEGMENT_PATH_LENGTH];
    if (bell_name(bell, sizeof(bell), own->owner_ip, own->owner_port, own->owner_context_id) >= 0) {
        tt_segment_bell_destroy(node, bell);
    }
#else
    (void)node;
    (void)own;
#endif
}

static void release_segments(struct tt_Context* node) {
    // From the header, not from the configuration, for the same reason the attach reads it there: with the
    // slot size settable at runtime, what this context BUILT is the only thing that says how much to unmap, and
    // re-deriving it would hand munmap a length that is right only while nothing changed between.
    size_t bytes = node->own_segment != NULL ? segment_bytes(node->own_segment->slots, node->own_segment->slot_bytes)
                                             : segment_bytes(tt_SEGMENT_SLOTS, own_slot_bytes());

    // The name is computed FIRST, while the mapping is certainly still there, and the own mapping is
    // taken out of the table before the loop - because a context is routinely attached to its own
    // segment as one of its peers. It unicasts to itself, so peer_segment() attaches to the file it
    // created, and segment_peers[own id].mapping and own_segment are then the same region. Unmapping
    // it in the loop and reading owner_ip out of it afterwards is a use-after-munmap, and it
    // segfaulted every node in the suite at teardown while every unit test stayed green, because the
    // mock's detach is a no-op and cannot reproduce an unmapped page.
    struct tt_SegmentHeader* own = node->own_segment;
    node->own_segment = NULL;
    char path[tt_SEGMENT_PATH_LENGTH];
    bool named =
        own != NULL && segment_name(path, sizeof(path), own->owner_ip, own->owner_port, own->owner_context_id) >= 0;

    // Peers' mappings are unmapped and never unlinked: those files belong to those peers and are
    // still being read by them. Only this context's own segment is this context's to remove - and it
    // is skipped here so it is unmapped exactly once, below.
    // Only the entries ever set up: the rest were never written, and their bytes are whatever the caller's memory held.
    for (int id = 0; id < tt_MAX_CONTEXT_IDS; id++) {
        const struct tt_SegmentPeer* entry = segment_peer_if_live(node, (uint8_t)id);
        if (entry == NULL) {
            continue;
        }
        if (entry->mapping != NULL && entry->mapping != own) {
            // The peer's own length, not ours: since the attach became two-step these can differ.
            tt_segment_detach(entry->mapping, entry->mapped_bytes);
        }
        segment_peer_clear(node, (uint8_t)id);
    }

    if (own != NULL) {
        if (named) {
            // Unlinked before unmapping, so no peer attaches to a segment this context has stopped
            // draining. A peer already holding it keeps its mapping until it next asks the name,
            // which is what the recheck is for.
            tt_segment_unlink(path);
        }
        release_own_bell(node, own);
        tt_segment_detach(own, bytes);
    }
}

// This context's own segment alone, given up while the context goes on running - the departing half
// of deferred creation, and the half that makes it worth anything: a context that outlives its
// same-host peers would otherwise hold the ring for as long as it ran.
//
// Unlink before unmap, for release_segments()' reason: no peer should attach to a segment nobody is
// draining any more.
//
// Releasing is safe but not free, and it is worth being exact about the cost rather than calling it
// none. A peer that discovery judged gone by liveliness timeout may still be alive and still hold a
// mapping of this file, and unlinking does not invalidate a mapping that already exists - so it goes
// on writing into a file that will never be read again. Its own dead-reader rule notices within
// tt_SEGMENT_DEAD_READER_NS and it falls back to UDP, and its revalidation re-attaches it to whatever
// this context builds next. So being wrong here costs that peer a second on the slower path. It is
// not loss, and it does not wedge - but it is why this is driven by the last departure and not by a
// timer.
static void release_own_segment(struct tt_Context* node) {
    struct tt_SegmentHeader* own = node->own_segment;
    if (own == NULL) {
        return;
    }
    node->own_segment = NULL;
    char path[tt_SEGMENT_PATH_LENGTH];
    // From the header, not from node->id: move_id() can renumber a live context on an id collision,
    // and the file on disk still carries the id it was created under.
    const bool named = segment_name(path, sizeof(path), own->owner_ip, own->owner_port, own->owner_context_id) >= 0;
    // Any entry aliasing this region is cleared, found BY IDENTITY rather than at index node->id. A
    // context delivering to itself has its own id pointing at its own region - but move_id() can
    // change node->id while that entry stays where it was, so indexing by the current id would find
    // an empty slot, unmap the region anyway, and leave the old entry dangling. That is a
    // use-after-munmap of the kind release_segments() was written to avoid, and this is the function
    // that can now run while the context is still going.
    for (int id = 0; id < tt_MAX_CONTEXT_IDS; id++) {
        const struct tt_SegmentPeer* entry = segment_peer_if_live(node, (uint8_t)id);
        if (entry != NULL && entry->mapping == own) {
            segment_peer_clear(node, (uint8_t)id);
        }
    }
    if (named) {
        tt_segment_unlink(path);
    }
    release_own_bell(node, own);
    // The header's own geometry, as release_segments() uses: what this context built is what must be unmapped.
    tt_segment_detach(own, segment_bytes(own->slots, own->slot_bytes));
    node->segments_released++;
}

// Whether this context is itself attached to its own segment, which is what delivering to itself
// leaves behind. Asked by identity, not at index node->id: move_id() can renumber a live context on
// an id collision and the entry does not move with it.
static bool own_segment_attached_by_self(const struct tt_Context* node) {
    if (node->own_segment == NULL) {
        return false;
    }
    for (int id = 0; id < tt_MAX_CONTEXT_IDS; id++) {
        const struct tt_SegmentPeer* entry = segment_peer_if_live(node, (uint8_t)id);
        if (entry != NULL && entry->mapping == node->own_segment) {
            return true;
        }
    }
    return false;
}

// Discovery's departing edge. The segment goes when the last peer that could open it has gone - and
// not while this context is still using it to deliver to itself, which is a separate claim on it that
// no departure can settle.
static void forget_same_host_peer(struct tt_Context* node, uint8_t context_id) {
    if (context_id == tt_CONTEXT_ID_INVALID || !node->same_host_peer[context_id]) {
        return;
    }
    node->same_host_peer[context_id] = false;
    node->same_host_peer_count--;
    if (node->same_host_peer_count == 0 && !own_segment_attached_by_self(node)) {
#if tt_SAMPLE_LENDING
        // Not while a retained sample lives in it, nor under the record being read in place (a farewell processed
        // from the ring ends here): the polling thread finishes it once neither is true (lend_finish_release()).
        if (node->lend.held_slots != 0 || node->lend.rx_kind == tt_LEND_SLOT) {
            node->lend.segment_release_deferred = true;
            return;
        }
#endif
        release_own_segment(node);
    }
}

#if tt_SAMPLE_LENDING
// The lazy release forget_same_host_peer() put off, if it is still due and nothing holds the segment now. On the
// polling thread only: the drain reads own_segment without the lock (drain_own_segment()'s fast path), so it is never
// unmapped from another thread - which is why tt_Sample_release() does not call this. True when it released.
static bool lend_finish_release(struct tt_Context* node) {
    if (!node->lend.segment_release_deferred || node->lend.held_slots != 0 || node->lend.rx_kind == tt_LEND_SLOT) {
        return false;
    }
    node->lend.segment_release_deferred = false;
    if (node->same_host_peer_count != 0 || own_segment_attached_by_self(node)) {
        return false; // a peer came back meanwhile: the segment is wanted again
    }
    release_own_segment(node);
    return true;
}
#endif

#endif

// The most destinations one datagram can have: a peer each, or a broadcast per link.
#define TX_MAX_DESTINATIONS (tt_MAX_LINK_COUNT > tt_MAX_PEER_COUNT ? tt_MAX_LINK_COUNT : tt_MAX_PEER_COUNT)

// The most datagrams one batch can carry, which is NOT the same number. send_datagram() batches one
// datagram to several destinations (TX_MAX_DESTINATIONS, 8); send_fragments() batches all of one
// sample's fragments to a single destination (tt_FRAG_MAX_COUNT, 64). Sizing a batch-sized array by
// the first overflows on the second - which is how this constant came to exist, from a stack
// smashing abort in test_data_frag rather than from reading the two call sites.
#define TX_MAX_BATCH (TX_MAX_DESTINATIONS > tt_FRAG_MAX_COUNT ? TX_MAX_DESTINATIONS : tt_FRAG_MAX_COUNT)
static_assert(TX_MAX_BATCH >= TX_MAX_DESTINATIONS && TX_MAX_BATCH >= tt_FRAG_MAX_COUNT,
              "a batch array must hold the largest batch either caller can pass");

static void count_tx(struct tt_Context* node, enum tt_Transport transport, uint32_t datagrams) {
    node->tx_datagrams += datagrams;
    node->tx_datagrams_by_transport[transport] += datagrams;
}

// Why a datagram went over UDP although the module is built in. Every UDP datagram has exactly one
// of these, which is what makes tx_udp == the sum of the four by-reason counters an invariant
// rather than a hope (SHM_PLAN.md's S2 asserts the same thing end to end).
enum udp_reason {
    UDP_BECAUSE_BROADCAST, // no single peer, so no name: by design
    UDP_BECAUSE_OVERSIZED, // larger than a slot: a service, which does not fragment
    UDP_BECAUSE_UNATTACHED
};

// The only way to count a UDP datagram. The two increments happen together here so that no path can
// record a fallback without naming its reason - asserting that afterwards would catch a drift, this
// makes the drift impossible to write.
static void count_udp(struct tt_Context* node, enum udp_reason reason, uint32_t datagrams) {
    count_tx(node, tt_TRANSPORT_UDP, datagrams);
    switch (reason) {
    case UDP_BECAUSE_BROADCAST:
        node->segment_broadcast_to_udp += datagrams;
        break;
    case UDP_BECAUSE_OVERSIZED:
        node->segment_oversized_to_udp += datagrams;
        break;
    case UDP_BECAUSE_UNATTACHED:
        node->segment_unattached_to_udp += datagrams;
        break;
    }
}

#if tt_SEGMENT_ENABLED
// One datagram to a peer's segment. Returns true when the segment took it; otherwise *reason says
// why it must go over UDP, and the caller counts it - the reason is returned rather than counted
// here so that exactly one place increments tx_udp and its reason together.
// Rings the owner of `segment` if it is asleep, once per sleep. Called after the records it is for are published -
// by segment_deliver() for a single record, by seam_send_batch() once per peer after the whole batch (2026-10-05):
// ringing between the fragments of one sample put the doorbell's write() and the wake-up it starts between the
// first fragment and the second, so after an idle period every p4 sample paid it before its second half could even
// be written - the rig's p4 - p3 round-trip gap was +6 us at 0.5 ms ping spacing and +20 us at 5 ms, CycloneDDS's
// 0-2 us (experiments/p4_interval_rig.sh).
static void segment_ring_if_asleep(struct tt_Context* node, uint8_t context_id, struct tt_SegmentHeader* segment,
                                   uint32_t ip, uint16_t port) {
    // Read after the write, never before: an owner that set the flag while this record was being
    // copied has already passed its own last drain, so only a check on this side of the publish can
    // see that it needs waking. A zero-length datagram is not a valid TickLE datagram under any
    // circumstance - the receive path drops it before the magic check - so this adds nothing another
    // implementation can parse and nothing that could be mistaken for data.
    //
    // The fence is what makes "after" true (2026-10-06). The record is published by a RELEASE store of its slot's
    // sequence (segment_write()), and a release store followed by a load of another word is the one pair C11 lets
    // the hardware reorder: on x86 the load is answered while the store still sits in this core's store buffer. Then
    // this side reads 0 here, the owner stores its generation and finds the slot not yet published, and both conclude
    // the other will act - the store-buffer litmus, a lost wake-up. Seen on the PC as 3-4 per 100,000 round trips
    // (platform/linux/bell_wake_check.c); "sequentially consistent on both sides" was true of the two accesses to
    // reader_waiting and not of the two that have to be ordered across it. Arm64's acquire load after a release
    // store happens to be ordered, which is why nothing on the rig showed it.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t sleeping = __atomic_load_n(&segment->reader_waiting, __ATOMIC_SEQ_CST);
    if (sleeping != 0) {
        // Once per sleep of the reader, not once per datagram (struct tt_SegmentPeer.doorbell_generation).
        struct tt_SegmentPeer* peer = segment_peer(node, context_id);
        if (sleeping != peer->doorbell_generation) {
            peer->doorbell_generation = sleeping;
            if (peer->bell_fd_plus1 > 0) {
                tt_segment_bell_ring(peer->bell_fd_plus1 - 1); // the FIFO: no socket layer on either side
                node->segment_bells_rung++;
            } else {
                (void)tt_send_to(node, "", 0, ip, port);
            }
            node->segment_doorbells_sent++;
        }
    }
}

// What follows a record published into a peer's segment, whichever way it was written: counted, and the reader rung.
static void segment_note_written(struct tt_Context* node, uint8_t context_id, struct tt_SegmentHeader* segment,
                                 uint32_t ip, uint16_t port, bool ring_now) {
    // A slot taken means we made progress, not that the reader did - so the reader's own clock is
    // left alone here and only the refusal path touches it.
    segment_peer(node, context_id)->last_progress_ns = 0;
    count_tx(node, tt_TRANSPORT_SHM, 1);
    if (ring_now) {
        segment_ring_if_asleep(node, context_id, segment, ip, port);
    }
}

static bool segment_deliver_ringing(struct tt_Context* node, uint8_t context_id, uint32_t ip, uint16_t port,
                                    const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                                    enum udp_reason* reason, bool ring_now) {
    // node->tx_seq_span is this datagram's, set by the publisher and returned to 1 by set_tx_tail() when
    // the buffer empties. Read here and passed down because segment_write() is given no node.
    if (context_id == tt_CONTEXT_ID_INVALID) {
        *reason = UDP_BECAUSE_BROADCAST; // no single peer, so no name to compute: never a candidate
        return false;
    }
    struct tt_SegmentHeader* segment = peer_segment(node, context_id, ip, port);
    if (segment == NULL) {
        *reason = UDP_BECAUSE_UNATTACHED;
        return false;
    }
    size_t total = hdr_len + body_len;
    if (total > segment->slot_bytes) {
        *reason = UDP_BECAUSE_OVERSIZED; // a service request or response: they do not fragment
        return false;
    }
    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(node, &own_ip, &own_port);
    if (!segment_write(segment, hdr, (uint32_t)hdr_len, body, (uint32_t)body_len, own_ip, own_port,
                       node->tx_seq_span)) {
        // Dropped, not rerouted, and this is the whole reason the function returns true here. A
        // datagram sent over UDP because the ring was full arrives AHEAD of the records already in
        // the ring - the socket does not wait for the reader's next drain - so the reader delivers
        // it and then discards everything older behind it. That is not a corner case: it cost CI's
        // same-host cell 97.6% of its traffic and 8x of its throughput. One logical stream, one
        // path. A full ring is a full queue, and a full queue drops.
        node->segment_full_dropped++;
#if tt_SAMPLE_LENDING
        if (segment_full_by_hold(segment)) {
            node->lend.full_retained++;
        }
#endif
        // A ring that will not take anything, again and again, is how an owner that stopped draining
        // looks from here - there is no other signal, since a killed owner's region stays mapped and
        // valid. Given up past the streak, after which this peer is UNATTACHED and reached over UDP,
        // which is safe precisely because there is no longer a reader to deliver anything out of
        // order to. The next recheck attaches again if it was wrong.
        struct tt_SegmentPeer* entry = segment_peer(node, context_id);
        uint64_t now = tt_get_ns();
        if (entry->last_progress_ns == 0) {
            entry->last_progress_ns = now; // the first refusal since we last got something in
        }
        if (now - entry->last_progress_ns >= tt_SEGMENT_DEAD_READER_NS) {
            tt_segment_detach(entry->mapping, entry->mapped_bytes);
            peer_bell_close(entry);
            memset(entry, 0, sizeof(*entry));
            remember_absent(entry, ip, port);
            note_attach(node, tt_SEGMENT_REFUSED); // asked for and given up on, which is what REFUSED says
        }
        if (node->segment_full_warnings == 0) {
            node->segment_full_warnings++;
            // The ring's shape and how much got through before it first filled, because
            // segment_full_dropped on its own cannot tell the two causes apart: a ring that is too
            // small for the load fills after roughly as many datagrams as it has slots, while a
            // ring whose slot sequences were never seeded is a ring of ONE and fills after the
            // first record - and that reads as a sizing problem, which is the wrong repair.
            TT_LOG_WARNING("Segment ring of context %u is full: %u slots of %u bytes, first full after %llu datagrams "
                           "over shared memory. Filling after about as many datagrams as there are slots is a sizing "
                           "signal; filling after one or two is a ring that was never seeded.",
                           (unsigned)context_id, (unsigned)segment->slots, (unsigned)segment->slot_bytes,
                           (unsigned long long)node->tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
        }
        return true; // handled: by dropping it, which is the ordered thing to do
    }
    segment_note_written(node, context_id, segment, ip, port, ring_now);
    return true;
}

// One record, rung for at once if its reader is asleep.
static bool segment_deliver(struct tt_Context* node, uint8_t context_id, uint32_t ip, uint16_t port, const void* hdr,
                            size_t hdr_len, const void* body, size_t body_len, enum udp_reason* reason) {
    return segment_deliver_ringing(node, context_id, ip, port, hdr, hdr_len, body, body_len, reason, true);
}
#endif

// The four send shapes. Each tries the peer's segment first when the destination is a known peer,
// and falls back to UDP for any reason at all - the fallback is the module's documented failure
// mode rather than an error, which is exactly why every reason for taking it is counted.
static int32_t seam_send(struct tt_Context* node, const void* buf, size_t len) {
    count_udp(node, UDP_BECAUSE_BROADCAST, 1); // this shape is the HAL's broadcast address by definition
    return tt_send(node, buf, len);
}

static int32_t seam_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port,
                            uint8_t context_id) {
    enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
#if tt_SEGMENT_ENABLED
    if (segment_deliver(node, context_id, ip, port, buf, len, NULL, 0, &reason)) {
        return (int32_t)len;
    }
#else
    (void)context_id;
    reason = context_id == tt_CONTEXT_ID_INVALID ? UDP_BECAUSE_BROADCAST : UDP_BECAUSE_UNATTACHED;
#endif
    if (len > (size_t)tt_MAX_BUFFER_LENGTH) {
        TT_LOG_ERROR("Record of %u bytes needs a segment and this destination has none free - dropped", (unsigned)len);
        node->tx_dropped_oversize++;
        return -1;
    }
    count_udp(node, reason, 1);
    return tt_send_to(node, buf, len, ip, port);
}

static int32_t seam_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body,
                             size_t body_len, uint32_t ip, uint16_t port, uint8_t context_id) {
    enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
#if tt_SEGMENT_ENABLED
    if (segment_deliver(node, context_id, ip, port, hdr, hdr_len, body, body_len, &reason)) {
        return (int32_t)(hdr_len + body_len);
    }
#else
    reason = context_id == tt_CONTEXT_ID_INVALID ? UDP_BECAUSE_BROADCAST : UDP_BECAUSE_UNATTACHED;
#endif
    count_udp(node, reason, 1);
    return tt_send_iov(node, hdr, hdr_len, body, body_len, ip, port);
}

// A batch is one call and several datagrams, each possibly to a different peer, so the segment is
// tried per datagram and only the ones that did not take it are left for the socket. The copy
// happens only when the batch is actually split.
static int32_t seam_send_batch(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count,
                               const uint8_t* context_ids) {
#if tt_SEGMENT_ENABLED
    struct tt_OutDatagram remaining[TX_MAX_BATCH];
    uint32_t left = 0;
    // The peers this batch wrote to, each rung once after the whole batch is in (segment_ring_if_asleep()).
    uint8_t rung_for[TX_MAX_BATCH];
    uint32_t peers = 0;
    for (uint32_t i = 0; i < count; i++) {
        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
        if (segment_deliver_ringing(node, context_ids[i], datagrams[i].ip, datagrams[i].port, datagrams[i].head,
                                    datagrams[i].head_len, datagrams[i].body, datagrams[i].body_len, &reason, false)) {
            bool seen = false;
            for (uint32_t known = 0; known < peers && !seen; known++) {
                seen = rung_for[known] == context_ids[i];
            }
            if (!seen) {
                rung_for[peers++] = context_ids[i];
            }
            continue;
        }
        // Counted per datagram, by its own reason - a batch can mix them, and a batch counted once
        // by the first datagram's reason would name the wrong cause for the rest.
        count_udp(node, reason, 1);
        remaining[left++] = datagrams[i];
    }
    for (uint32_t written = 0; written < peers; written++) {
        struct tt_SegmentPeer* peer = segment_peer(node, rung_for[written]);
        if (peer->mapping != NULL) { // a peer given up on during this batch has nobody left to wake
            segment_ring_if_asleep(node, rung_for[written], peer->mapping, peer->ip, peer->port);
        }
    }
    if (left == 0) {
        return 0; // every datagram took a segment; nothing for the socket
    }
    return tt_send_batch(node, remaining, left);
#else
    for (uint32_t i = 0; i < count; i++) {
        count_udp(node, context_ids[i] == tt_CONTEXT_ID_INVALID ? UDP_BECAUSE_BROADCAST : UDP_BECAUSE_UNATTACHED, 1);
    }
    return tt_send_batch(node, datagrams, count);
#endif
}

// One datagram to one address; ip 0 is the HAL's own broadcast address, as for tt_send_iov().
static bool send_datagram_to(struct tt_Context* node, const struct tx_datagram* dgram, uint32_t ip, uint16_t port,
                             uint8_t context_id) {
    if (dgram->body_len != 0) {
        return seam_send_iov(node, dgram->head, dgram->head_len, dgram->body, dgram->body_len, ip, port, context_id) >=
               0;
    }
    if (ip == 0) {
        return seam_send(node, dgram->head, dgram->head_len) >= 0;
    }
    return seam_send_to(node, dgram->head, dgram->head_len, ip, port, context_id) >= 0;
}

// Where one datagram goes, as ip/port with ip 0 meaning the HAL's own broadcast address. The routing
// below decides these once, and the send that follows is the only thing that differs between one
// destination and several.
struct tx_destination {
    uint32_t ip;
    uint16_t port;
    // Which peer this destination is, or tt_CONTEXT_ID_INVALID for a broadcast. The segment's name
    // is computed from (address, port, context id), so a destination with no single peer has no
    // name to compute and can only go over UDP - which is why announces and summaries never leave
    // the interface however well the module is working.
    uint8_t context_id;
};
// Every link's broadcast, or every peer: the most either rule below can produce.

// Broadcast, when there is no addressable peer set: either nobody is known yet, or the caller has
// batched submessages for different peers into one buffer and cannot aim it. Goes out on every
// link, because this is how a node is discovered at all and its peers may be on any of them.
//
// One link goes through seam_send() and the HAL's precomputed broadcast address. That is not only an
// optimisation: it keeps the single-link case - every deployment that has not configured links[],
// which is all of them today - on exactly the path it used before per-link existed, rather than on
// a new one that happens to be equivalent.
static uint8_t broadcast_destinations(struct tx_destination* out) {
    if (link_count() <= 1) {
        out[0] = (struct tx_destination) {0, 0, tt_CONTEXT_ID_INVALID};
        return 1;
    }
    for (uint8_t i = 0; i < link_count(); i++) {
        out[i] = (struct tx_destination) {_tt_CONFIG.links[i].resolved_broadcast, (uint16_t)_tt_CONFIG.port,
                                          tt_CONTEXT_ID_INVALID};
    }
    return link_count();
}

// One link's share of an addressed buffer: unicast to its peers while there are few enough of
// them, one broadcast once there are not. Per link rather than across the whole peer set because
// the right answer differs by medium - five subscribers on a 10Base-T1S segment and one on
// Ethernet want opposite answers, and a single count across both loses on whichever it is not
// sized for. Appends to out[] after the `written` entries already there and returns the new count.
static uint8_t link_destinations(const struct tt_Peer* peers, uint8_t peer_count, uint8_t link_index,
                                 struct tx_destination* out, uint8_t written) {
    uint8_t on_link = 0;
    for (uint8_t i = 0; i < peer_count; i++) {
        if (link_of_ip(peers[i].ip) == link_index) {
            on_link++;
        }
    }
    if (on_link == 0) {
        return written; // nobody known on this link, and this buffer is for known peers
    }

    if (on_link > _tt_CONFIG.links[link_index].unicast_threshold) {
        if (link_count() <= 1) {
            out[written] = (struct tx_destination) {0, 0, tt_CONTEXT_ID_INVALID};
        } else {
            out[written] = (struct tx_destination) {_tt_CONFIG.links[link_index].resolved_broadcast,
                                                    (uint16_t)_tt_CONFIG.port, tt_CONTEXT_ID_INVALID};
        }
        return (uint8_t)(written + 1);
    }

    for (uint8_t i = 0; i < peer_count && written < TX_MAX_DESTINATIONS; i++) {
        if (link_of_ip(peers[i].ip) == link_index) {
            out[written++] = (struct tx_destination) {peers[i].ip, peers[i].port, peers[i].context_id};
        }
    }
    return written;
}

// Where a flush sends its datagram: broadcast when peer_count is 0, otherwise each link's share of the
// peers (link_destinations()). Returns how many destinations were written to out[].
static uint8_t tx_destinations(const struct tt_Peer* peers, uint8_t peer_count, struct tx_destination* out) {
    if (peer_count == 0) {
        return broadcast_destinations(out);
    }
    uint8_t written = 0;
    for (uint8_t i = 0; i < link_count(); i++) {
        written = link_destinations(peers, peer_count, i, out, written);
    }
    return written;
}

// One datagram to the destinations a flush would send it to. To one destination it goes exactly as it always
// has, through send_datagram_to(); to several - peers on one link, or one broadcast per link - it goes as one
// seam_send_batch(), so that sending the same bytes to more peers stops costing a system call per peer.
static bool send_datagram(struct tt_Context* node, const struct tx_datagram* dgram, const struct tt_Peer* peers,
                          uint8_t peer_count) {
    struct tx_destination destinations[TX_MAX_DESTINATIONS];
    uint8_t count = tx_destinations(peers, peer_count, destinations);
    if (count == 0) {
        return true; // addressed peers, none on any link: nothing to send, as before
    }
    if (count == 1) {
        return send_datagram_to(node, dgram, destinations[0].ip, destinations[0].port, destinations[0].context_id);
    }
    struct tt_OutDatagram batch[TX_MAX_DESTINATIONS];
    uint8_t batch_context_ids[TX_MAX_DESTINATIONS];
    for (uint8_t i = 0; i < count; i++) {
        batch[i] = (struct tt_OutDatagram) {dgram->head,     dgram->head_len,    dgram->body,
                                            dgram->body_len, destinations[i].ip, destinations[i].port};
        batch_context_ids[i] = destinations[i].context_id;
    }
    return seam_send_batch(node, batch, count, batch_context_ids) >= 0;
}

static uint32_t whole_record_limit_for(struct tt_Context* node, const struct tt_Peer* peers, uint8_t peer_count);

// The largest record this particular send may carry: a datagram's worth, unless every destination is a
// same-host peer whose segment can take more (SHM_PLAN 6e(a)). Asked at each send site with THAT send's
// destinations rather than computed once and passed down, because the destinations differ between a
// publish and the retransmission of the same sample to the one node that asked for it - a single threaded
// value would be right for one of them and wrong for the other.
static uint32_t record_size_limit(struct tt_Context* node, uint32_t floor, const struct tt_Peer* peers,
                                  uint8_t peer_count) {
#if tt_SEGMENT_ENABLED
    // whole_record_limit_for() answers with the smallest slot among the destinations, or 0, so it can never exceed
    // the largest slot this context has ever attached to. When that is no more than the floor - every peer at the
    // default slot of one datagram - the answer is the floor whatever the destinations are, and the walk is skipped.
    // It was four walks per publish; skipping them measured -1.5% publisher CPU on p3 (2026-10-05, 6 reps per arm,
    // ranges separate), small and just above the ~1% layout floor of WIRE_PLAN 10.4.
    if (node->segment_slot_ceiling <= floor) {
        return floor;
    }
#endif
    uint32_t whole = whole_record_limit_for(node, peers, peer_count);
    return whole > floor ? whole : floor;
}

static bool flush_tx(struct tt_Context* node, uint32_t len, const struct tt_Peer* peers, uint8_t peer_count) {
    // Check at least 1 submessage is contained
    if (len < sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader)) {
        return true; // Nothing to flush
    }

    // Not sendable, ever: no datagram can carry `len` bytes. Drop the whole pending buffer rather
    // than leave it. This used to return false and keep tx_tail where it was, and since every
    // later send appends behind the same unsendable bytes and flushes them first, one oversized
    // flush silenced the node for good - found 2026-09-24 as a node whose discovery UPDATE grew
    // past one datagram (16 ROS-sized endpoints) and then failed every publish after it.
    // end_encode() now refuses a submessage that could never fit, so this should be unreachable;
    // it stays as the guarantee that nothing can wedge the buffer.
    uint32_t flush_limit = record_size_limit(node, tt_MAX_BUFFER_LENGTH, peers, peer_count);
    if (len > flush_limit) {
        TT_LOG_ERROR("Flush length %u exceeds the %u this send can carry - dropping %u pending bytes", len, flush_limit,
                     node->tx_tail - (uint32_t)sizeof(struct tt_Header));
        node->tx_dropped_oversize++;
        set_tx_tail(node, sizeof(struct tt_Header));
        node->tx_has_pending_update = false;
        return false;
    }

    struct tt_Header* header = (struct tt_Header*)node->tx_buffer;
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = node->id;

    // Read before the send below, which may rewrite this datagram's first submessage header in place.
#ifdef tt_RELIABLE_STATS
    {
        uint64_t data_count = 0;
        uint32_t pos = sizeof(struct tt_Header);
        while (pos + sizeof(struct tt_SubmessageHeader) <= len) {
            const struct tt_SubmessageHeader* submsg = (const struct tt_SubmessageHeader*)(node->tx_buffer + pos);
            if (submsg->length == 0) {
                break;
            }
            if (submsg->type == tt_SUBMESSAGE_TYPE_DATA &&
                ((const struct tt_DataHeader*)(submsg + 1))->endpoint_id != tt_DISCOVERY_ENDPOINT_ID) {
                data_count++; // a sample - an announce is a DATA too since tt_VERSION 7
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

    uint32_t skip = to_single_form(node->tx_buffer, len, 0);
    struct tx_datagram dgram = {node->tx_buffer + skip, len - skip, NULL, 0};
    if (node->summary_skip_armed) {
        if (len != node->tx_summary_alone_len) {
            note_reached_armed(node, peers, peer_count);
        }
        node->tx_summary_alone_len = 0;
    }
    if (!send_datagram(node, &dgram, peers, peer_count)) {
        TT_LOG_ERROR("Cannot send packet: %s", strerror(errno));
        return false;
    }

    // Whatever was pending (including any batched announce - see node_update()/node_flush()) just
    // went out in `len` bytes above, unconditionally: a deferred-flush's `base` always covers
    // everything appended before the submessage that triggered it, which includes an earlier
    // announce if one was still batched.
    node->tx_has_pending_update = false;

    _tt_memmove(node->tx_buffer + sizeof(struct tt_Header), node->tx_buffer + len, node->tx_tail - len);
    set_tx_tail(node, (uint32_t)(sizeof(struct tt_Header) + (node->tx_tail - len)));

    return true;
}

// Outside the tt_FRAG_ENABLED guard since 2026-10-03: the publish path passes it as the limit a sample may
// reach before being split, and does so whether or not this build fragments - a build that cannot split
// still has to say what 'whole' means. The value does not depend on the flag.

#if tt_FRAG_ENABLED
// The largest datagram a DATA goes as whole; a larger sample fragments. The control datagram rather than
// tt_MAX_BUFFER_LENGTH, so that a build with a large datagram for its services (rmw_tickle) still fragments
// its samples rather than leaving them to the OS's IP fragmentation (config.h, tt_FRAG_ENABLED).

// CDR bytes in fragment 0 and in each full continuation. Fragments are cut to tt_CONTROL_MAX_LENGTH,
// not tt_MAX_BUFFER_LENGTH, for the reason that constant exists: a node on core defaults has to be able
// to receive them. The two are equal unless tt_MAX_BUFFER_LENGTH has been raised.
#define FRAG_FRAMING_LENGTH (sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader))
#define FRAG_FIRST_PAYLOAD (tt_CONTROL_MAX_LENGTH - FRAG_FRAMING_LENGTH - sizeof(struct tt_FragFirstHeader))
#define FRAG_CONT_PAYLOAD (tt_CONTROL_MAX_LENGTH - FRAG_FRAMING_LENGTH - sizeof(struct tt_FragContHeader))
// The largest CDR a fragmented sample carries: tt_MAX_SAMPLE_LENGTH, plus the padding a DATA record is
// rounded up by (record_length_checked()), since a retransmission is cut from the cached record.
#define FRAG_MAX_CDR (tt_MAX_SAMPLE_LENGTH + 3)
_Static_assert(1 + ((FRAG_MAX_CDR - FRAG_FIRST_PAYLOAD + FRAG_CONT_PAYLOAD - 1) / FRAG_CONT_PAYLOAD) <=
                   tt_FRAG_MAX_COUNT,
               "tt_MAX_SAMPLE_LENGTH needs more than tt_FRAG_MAX_COUNT fragments at tt_CONTROL_MAX_LENGTH");
_Static_assert(FRAG_FIRST_PAYLOAD + tt_FRAG_FIRST_SHORTFALL == FRAG_CONT_PAYLOAD,
               "fragment 0 must carry exactly tt_FRAG_FIRST_SHORTFALL fewer bytes - the receiver relies on it");

// How many fragments a sample of cdr_len CDR bytes takes, and where fragment `index` of it starts and
// how long it is. Fragment 0 carries tt_FRAG_FIRST_SHORTFALL bytes less than a continuation, and every
// fragment but the last is full.
static uint32_t frag_count_for(uint32_t cdr_len) {
    return 1 + ((cdr_len - FRAG_FIRST_PAYLOAD + FRAG_CONT_PAYLOAD - 1) / FRAG_CONT_PAYLOAD);
}

static uint32_t frag_payload_offset(uint32_t index) {
    return index == 0 ? 0 : FRAG_FIRST_PAYLOAD + ((index - 1) * FRAG_CONT_PAYLOAD);
}

static uint32_t frag_payload_length(uint32_t index, uint32_t cdr_len) {
    uint32_t offset = frag_payload_offset(index);
    uint32_t full = index == 0 ? FRAG_FIRST_PAYLOAD : FRAG_CONT_PAYLOAD;
    return cdr_len - offset < full ? cdr_len - offset : full;
}

// Writes fragment `index` of the sample data_header names - its submessage header and fragment header,
// not its payload - at out, and returns how many bytes that is. Every datagram of a sample takes its own
// seq_no (DATAFRAG_PLAN.md section 13): fragment i is data_header->seq_no + i, which is what lets the
// ordinary ACKNACK name, and the writer resend, exactly the datagram that was lost.
static uint32_t frag_write_header(uint8_t* out, const struct tt_DataHeader* data_header, uint32_t index, uint32_t count,
                                  uint32_t payload_length, uint8_t receiver) {
    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)out;
    submessage_header->receiver = receiver;
    uint32_t header_length;
    if (index == 0) {
        struct tt_FragFirstHeader* first = (struct tt_FragFirstHeader*)(out + sizeof(struct tt_SubmessageHeader));
        _tt_memcpy(&first->data, data_header, sizeof(struct tt_DataHeader));
        first->frag_count = (uint8_t)count;
        submessage_header->type = tt_SUBMESSAGE_TYPE_FRAG_FIRST;
        header_length = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_FragFirstHeader);
    } else {
        struct tt_FragContHeader* cont = (struct tt_FragContHeader*)(out + sizeof(struct tt_SubmessageHeader));
        cont->entity_id = data_header->entity_id;
        cont->seq_no = data_header->seq_no + index;
        cont->frag_index = (uint8_t)index;
        cont->frag_count = (uint8_t)count;
        submessage_header->type = tt_SUBMESSAGE_TYPE_FRAG_CONT;
        header_length = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_FragContHeader);
    }
    submessage_header->length = (uint16_t)(header_length + payload_length);
    return header_length;
}

// Sends one sample as fragments, each its own datagram, to the destinations a flush would use
// (tx_destinations()). The CDR is read from where it lies - tx_buffer or the caller's own memory - and
// only the framing is built here, once for every destination. Only for a sample that no DATA could
// carry, so there are always at least two.
//
// All of a sample's fragments to one destination go in one seam_send_batch() - one sendmmsg() on Linux -
// which is what returns a fragmented sample to the single send system call it cost before it had to be
// fragmented (DATAFRAG_PLAN.md 6.5, step 3). Destination by destination, so each receiver gets a sample's
// fragments back to back.
static bool send_fragments(struct tt_Context* node, const struct tt_DataHeader* data_header, const uint8_t* cdr,
                           uint32_t cdr_len, const struct tt_Peer* peers, uint8_t peer_count, uint8_t receiver) {
    if (cdr_len <= FRAG_FIRST_PAYLOAD || cdr_len > FRAG_MAX_CDR) {
        TT_LOG_ERROR("Sample of %u bytes cannot be sent as fragments", cdr_len);
        node->tx_dropped_oversize++;
        return false;
    }
    uint32_t count = frag_count_for(cdr_len);

    // Every datagram from here carries its own seq_no - frag_write_header() gives fragment i the
    // base plus i - so each one's span is 1 and the sample's span is spent by the set of them.
    // Cleared here rather than at the call sites because this is the one place that knows it is
    // about to emit several datagrams for one sample, and because getting it wrong is silent in
    // both directions. Leaving the sample's span standing made EVERY fragment's slot claim the
    // whole sample's span: the reader then absorbed two seq positions per fragment, its watermark
    // ran ahead of the stream, and every later sample was classified as a duplicate. Measured on
    // the rig at the default slot_bytes 1472 on 2026-10-03 - recv=0 with frag_duplicate equal to
    // the number of samples sent, while the publisher's own sent count looked perfectly healthy.
    node->tx_seq_span = 1;

    // One framing per fragment: header, submessage header, and the fragment header.
    uint8_t framings[tt_FRAG_MAX_COUNT][FRAG_FRAMING_LENGTH + sizeof(struct tt_FragFirstHeader)];
    struct tt_OutDatagram batch[tt_FRAG_MAX_COUNT];
    for (uint32_t index = 0; index < count; index++) {
        uint8_t* framing = framings[index];
        struct tt_Header* header = (struct tt_Header*)framing;
        header->magic_value = NATIVE_MAGIC_VALUE;
        header->version = tt_VERSION;
        header->source = node->id;
        uint32_t length = frag_payload_length(index, cdr_len);
        uint32_t header_length =
            frag_write_header(framing + sizeof(struct tt_Header), data_header, index, count, length, receiver);
        uint32_t framing_length = (uint32_t)sizeof(struct tt_Header) + header_length;
        uint32_t skip = to_single_form(framing, framing_length, length);
        batch[index] = (struct tt_OutDatagram) {
            framing + skip, framing_length - skip, cdr + frag_payload_offset(index), length, 0, 0};
    }

    note_reached(node, peers, peer_count);
    struct tx_destination destinations[TX_MAX_DESTINATIONS];
    uint8_t destination_count = tx_destinations(peers, peer_count, destinations);
    for (uint8_t dest = 0; dest < destination_count; dest++) {
        // Every fragment of this sample goes to the same destination, so the batch's peer ids are
        // uniform - unlike send_datagram()'s batch, where one datagram goes to several peers.
        uint8_t fragment_context_ids[TX_MAX_BATCH];
        for (uint32_t index = 0; index < count; index++) {
            batch[index].ip = destinations[dest].ip;
            batch[index].port = destinations[dest].port;
            fragment_context_ids[index] = destinations[dest].context_id;
        }
        if (seam_send_batch(node, batch, count, fragment_context_ids) < 0) {
            return false;
        }
    }
    return true;
}

// Bytes of CDR a DATA submessage at submessage_header, padded as it is sent, carries.
static uint32_t sample_cdr_length(const struct tt_Context* node, const struct tt_SubmessageHeader* submessage_header) {
    uint32_t length = (uint32_t)((uintptr_t)node->tx_buffer + node->tx_tail - (uintptr_t)submessage_header);
    return ROUNDUP(length) - (uint32_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader));
}

// Sends the DATA submessage at submessage_header - the last thing in tx_buffer, and too large for one
// datagram - as fragments, after whatever was batched ahead of it, and takes it out of tx_buffer.
// Leaves tx_tail consistent whether or not it succeeds, so the caller must not roll back afterwards:
// once the batch ahead has been flushed, the tail it would roll back to no longer exists.
//
// Padded as end_encode() would pad it, so that the fragments of the original and of a retransmission
// cut from the cached record (which is padded) always agree on the sample's length, and so on how many
// fragments it has.
static bool send_tail_as_fragments(struct tt_Context* node, struct tt_SubmessageHeader* submessage_header,
                                   const struct tt_Peer* peers, uint8_t peer_count) {
    uint32_t base = (uint32_t)((uint8_t*)submessage_header - node->tx_buffer);
    uint32_t length = node->tx_tail - base;
    memset(node->tx_buffer + node->tx_tail, 0, ROUNDUP(length) - length);
    length = ROUNDUP(length);
    set_tx_tail(node, base + length);
    if (base > sizeof(struct tt_Header)) {
        // What was batched ahead goes first, as it would have ahead of a DATA. flush_tx() moves the
        // sample down behind the header, which is where it is read from below.
        if (!flush_tx(node, base, NULL, 0)) {
            set_tx_tail(node, base);
            return false;
        }
        base = sizeof(struct tt_Header);
    }
    const uint8_t* record = node->tx_buffer + base;
    bool sent = send_fragments(node, (const struct tt_DataHeader*)(record + sizeof(struct tt_SubmessageHeader)),
                               record + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader),
                               length - (uint32_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader)),
                               peers, peer_count, tt_SUBMESSAGE_ID_ALL);
    set_tx_tail(node, base);
    return sent;
}
#endif

// Whether the submessage being encoded at submessage_header (everything from it to tx_tail, padded)
// could fit one datagram on its own. False means it never can, however the buffer around it is
// flushed: the protocol does not fragment. Counts and logs the refusal, so each caller only has to
// roll back.
// `length` is passed rather than derived. It used to be taken as tx_buffer + tx_tail -
// submessage_header, which requires two things the signature does not ask for: that the pointer is
// inside tx_buffer, and that tx_tail is this submessage's end. send_cached_record() needs the same
// question answered about a record in the reliable arena, where that subtraction is between two
// unrelated addresses - passing it one delivered 23 of 200 samples in test_data_frag, and the fix
// there was a comment telling the next caller not to use this function. A function whose repair is a
// warning to its callers is the thing that is wrong. Both callers already hold the length.
static bool submessage_fits_datagram(struct tt_Context* node, const struct tt_SubmessageHeader* submessage_header,
                                     size_t length, uint32_t limit) {
    if (sizeof(struct tt_Header) + ROUNDUP(length) <= limit) {
        return true;
    }
    TT_LOG_ERROR("Submessage type %u of %u bytes can never fit the %u this send can carry - not sent",
                 submessage_header->type, (unsigned)ROUNDUP(length), limit);
    node->tx_dropped_oversize++;
    return false;
}

// Arms node_flush() if anything is waiting in tx_buffer and no flush is armed yet (2026-09-25). It used to
// tick every tt_CONTEXT_TX_INTERVAL unconditionally, which kept an idle node waking a thousand times a second
// for an empty buffer. The flush is armed on the grid that tick ran on - the next tt_CONTEXT_TX_INTERVAL
// boundary of the clock, as the old tick started cycle-aligned and rescheduled from its own due time - so
// a batched submessage waits exactly as long as it did before: 0 to one interval, not a full interval.
static void node_flush(struct tt_Context* node, uint64_t time, void* param);

static void ensure_flush_scheduled(struct tt_Context* node) {
    if (node->flush_scheduled || node->tx_tail <= sizeof(struct tt_Header)) {
        return;
    }
    uint64_t now = tt_get_ns();
    uint64_t due = now - (now % tt_CONTEXT_TX_INTERVAL) + tt_CONTEXT_TX_INTERVAL;
    if (tt_Context_schedule(node, due, node_flush, NULL)) {
        node->flush_scheduled = true;
    } else {
        TT_LOG_ERROR("Cannot schedule node_flush");
    }
}

static bool end_encode(struct tt_Context* node, struct tt_SubmessageHeader* submessage_header, bool is_flush,
                       const struct tt_Peer* peers, uint8_t peer_count) {
    // Set submessage header length
    size_t length = (uintptr_t)node->tx_buffer + node->tx_tail - (uintptr_t)submessage_header;
    size_t roundup = ROUNDUP(length) - length;
    // tx_tail before this submessage was appended - what to flush when it doesn't fit and
    // has to be deferred to the next buffer instead of going out in this one.
    uint32_t base = (uint32_t)(node->tx_tail - length);

    // A submessage that cannot fit a datagram even on its own is refused here, before it can sit
    // in tx_buffer. Deferring it (the branches below) only moves it to the front of the next
    // buffer, where flush_tx() would refuse it again - and before that check dropped instead of
    // returning, it stayed there and blocked every later send. The protocol does not fragment, so
    // there is nothing else to do with it; the caller learns from `false` and rolls back.
    if (!submessage_fits_datagram(node, submessage_header, length,
                                  record_size_limit(node, tt_MAX_BUFFER_LENGTH, peers, peer_count))) {
        set_tx_tail(node, base);
        return false;
    }

    // What to flush if is_flush ends up true below: the whole (padded) buffer including this
    // submessage by default, unless a branch below decides this submessage doesn't fit and
    // must be deferred, in which case it's overridden to `base` (everything before it).
    uint32_t flush_len;

    // How large the datagram carrying this submessage may grow: the full tt_MAX_BUFFER_LENGTH only
    // for a submessage that is on its own in the buffer, tt_CONTROL_MAX_LENGTH once it shares one
    // (config.h). Identical to before whenever the two are equal, which is core's default.
    //
    // Through record_size_limit() since 2026-10-03, as the refusal at the top of this function and
    // flush_tx()'s own flush_limit already were. Without it the three disagreed: the refusal admitted a
    // record the destination's slot could take, and this branch then measured it against a datagram,
    // found it too large, and DEFERRED it - "it can't go out in this flush either". Deferred where
    // nothing comes back for it, so the record sat in tx_buffer and was never sent while the publish
    // returned OK. Measured: tx_shm 0, tx_udp 0, the peer's write_index still 0, and no counter moved.
    // A silent retention, not a drop, which is why no existing test or statistic showed it.
    //
    // Only the alone case is raised. A submessage sharing a datagram cannot be a whole record bound for
    // a slot: unicast_destinations_for() requires an empty buffer before it will grant one.
    uint32_t limit = tt_CONTROL_MAX_LENGTH;
    if (base == sizeof(struct tt_Header)) {
        limit = record_size_limit(node, tt_MAX_BUFFER_LENGTH, peers, peer_count);
    }

    // The padding goes on the wire, counted in the submessage's length, so it is zeroed: it used to carry whatever an
    // earlier datagram had left in tx_buffer there (2026-09-27). A receiver then saw a payload ending in stale bytes -
    // test_thread_safety's response codec, which requires its string's terminator last, failed every retry of a call
    // whose padding held one, and the client never completed it - and the bytes of an earlier datagram left the host.
    for (size_t i = 0; i < roundup; i++) {
        node->tx_buffer[node->tx_tail + i] = 0;
    }

    if (node->tx_tail + roundup <= limit) { // tail in below the buffer
        submessage_header->length = length + roundup;
        node->tx_tail += roundup;
        flush_len = node->tx_tail;
    } else if (node->tx_tail <= limit && node->tx_tail + roundup > limit) { // tail exceeds buffer if roundup
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
    // Whatever is still in tx_buffer - batched here, or deferred past a flush that could not take it -
    // leaves on the next flush tick.
    ensure_flush_scheduled(node);

    return true;
}

static void* decode(struct tt_Context* node, uint8_t* buffer, uint32_t* head, uint32_t tail, uint32_t length) {
    UNUSED(node);
    return tt_decode_buffer(buffer, head, tail, length);
}

static bool decode_string(struct tt_Context* node, uint8_t* buffer, uint32_t* head, uint32_t tail, uint16_t* str_len,
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

static void rebuild_endpoint_index(struct tt_Context* node) {
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
static struct tt_Endpoint* find_endpoint(struct tt_Context* node, uint8_t kind, uint32_t endpoint_id) {
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
static struct tt_Endpoint* find_endpoint_by_entity(struct tt_Context* node, uint8_t kind, uint32_t endpoint_id,
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
static void for_each_endpoint(struct tt_Context* node, uint8_t kind, uint32_t endpoint_id,
                              void (*visit)(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx),
                              void* ctx) {
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
// delivery instead of on every periodic announce refresh. Most callers (the Client/Server peer
// direction) still just ignore the return value, which is fine in C.
static bool upsert_peer(struct tt_Peer* peers, uint8_t node_id, uint32_t ip, uint16_t port) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].context_id == node_id) {
            peers[i].ip = ip;
            peers[i].port = port;
            return false;
        }
    }

    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].context_id == tt_CONTEXT_ID_INVALID) {
            peers[i].context_id = node_id;
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
        if (peers[i].context_id != tt_CONTEXT_ID_INVALID) {
            count++;
        }
    }
    return count;
}

// Removes node_id from peers[] by closing the gap it leaves: every entry after it moves down one slot. The table
// is kept DENSE - its live entries are exactly the first count_peers() slots - because every walk that sends to it
// takes (peers, count_peers(peers)) and reads peers[0 .. count): link_destinations(), whole_record_limit_for(),
// note_reached_armed(), the fragment and heartbeat sends. Clearing the slot in place, which this did until
// 2026-10-08, left a hole those walks read as a peer: with A in slot 0 and B in slot 1, A's farewell made the
// count 1 and every later send went to slot 0 - A's old address, under no context id, so over UDP to a port
// nobody held, counted as a broadcast - and B was never sent anything again. CI's interfaces check failed on it
// twice (2026-10-06, 2026-10-08): the C++ subscriber took its four samples and left, and the C subscriber matched
// after it took nothing in 15 s. Order kept, so the peers that remain are addressed as before.
static void forget_peer(struct tt_Peer* peers, uint8_t node_id) {
    int kept = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (peers[i].context_id != tt_CONTEXT_ID_INVALID && peers[i].context_id != node_id) {
            peers[kept++] = peers[i];
        }
    }
    for (int i = kept; i < tt_MAX_PEER_COUNT; i++) {
        peers[i] = (struct tt_Peer) {.context_id = tt_CONTEXT_ID_INVALID, .ip = 0, .port = 0};
    }
}

// One remote Subscriber entity's own entry in pub->peer_acks[] (keyed by (node_id, entity_id) -
// see that field's own doc comment, tickle.h), or NULL if this Publisher isn't tracking it.
static struct tt_PeerAck* find_peer_ack(struct tt_Publisher* pub, uint8_t node_id, uint32_t entity_id) {
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].context_id == node_id && pub->peer_acks[i].entity_id == entity_id) {
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
        if (pub->peer_acks[i].context_id == tt_CONTEXT_ID_INVALID) {
            pub->peer_acks[i].context_id = node_id;
            pub->peer_acks[i].entity_id = entity_id;
            pub->peer_acks[i].ack_seq_no = 0;
            pub->peer_acks[i].tracking_words = 0; // set by the caller from the announce
            pub->peer_acks[i].first_owed_seq_no = 0;
            pub->peer_acks[i].match_heartbeats_left = 0;
            return &pub->peer_acks[i];
        }
    }
    return NULL;
}

// Drops ack state outright - a real departure (farewell announce, liveliness timeout, or an announce
// that no longer lists a matching Subscriber), not process_announce()'s own transient
// forget-then-re-add (Phase 3 prerequisite (c): that one must preserve it). entity_id 0 with
// match_any_entity drops every entity that node hosts (a whole node departing); otherwise just the
// one named entity (a single Subscriber's own lease expiring while its node stays up).
static void forget_peer_ack(struct tt_Publisher* pub, uint8_t node_id, uint32_t entity_id, bool match_any_entity) {
    struct tt_DepartedAck* departed = &pub->departed_acks[pub->departed_next];
    pub->departed_next = (uint8_t)((pub->departed_next + 1U) % tt_DEPARTED_ACKS);
    departed->context_id = node_id;
    departed->entity_id = match_any_entity ? 0 : entity_id;
    departed->at_ns = tt_get_ns();
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].context_id != node_id) {
            continue;
        }
        if (!match_any_entity && pub->peer_acks[i].entity_id != entity_id) {
            continue;
        }
        pub->peer_acks[i].context_id = tt_CONTEXT_ID_INVALID;
        pub->peer_acks[i].entity_id = 0;
        pub->peer_acks[i].ack_seq_no = 0;
        pub->peer_acks[i].first_owed_seq_no = 0;
        pub->peer_acks[i].match_heartbeats_left = 0;
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
    if (ack->ack_seq_no > ack->first_owed_seq_no) {
        ack->match_heartbeats_left = 0; // it has the owed sample: its baseline is at or below it
    }
}

// The writer owns the match point (tt_PeerAck.first_owed_seq_no, 2026-10-07). A VOLATILE Subscriber's writer proxy
// took its baseline from the first DATA to arrive, so when the first sample written after the match was lost on the
// wire, the next one became the baseline: the lost one was never asked for, and the next ack released it at a
// KEEP_ALL writer (p1 under 5% loss, ~4% of runs, first_seq=2). Only this side knows when it matched that Subscriber,
// so it says so: the next tt_MATCH_HEARTBEATS publishes carry a Heartbeat ahead of their DATA whose last_seq_no + 1 is
// the owed sample - the baseline a VOLATILE Subscriber already takes from a first-contact Heartbeat
// (inform_subscriber_of_heartbeat()). Not for a DURABLE one, which starts at the oldest retained sample anyway.
#define tt_MATCH_HEARTBEATS 8

static void arm_match_heartbeat(struct tt_Publisher* pub, struct tt_PeerAck* ack) {
    ack->first_owed_seq_no = pub->seq_no + 1;
    ack->match_heartbeats_left = tt_MATCH_HEARTBEATS;
    pub->match_heartbeat_pending = true;
}

// The lowest first_owed_seq_no among the entries still owed a match Heartbeat, each counted down by one; 0 when
// none is. The lowest, because one datagram goes to every peer: a Subscriber matched later than another then starts
// at the earlier match point - over-delivering what was written between the two matches, never losing.
static uint32_t take_match_heartbeat(struct tt_Publisher* pub) {
    uint32_t owed = 0;
    bool still_pending = false;
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        struct tt_PeerAck* ack = &pub->peer_acks[i];
        if (ack->context_id == tt_CONTEXT_ID_INVALID || ack->match_heartbeats_left == 0) {
            continue;
        }
        if (owed == 0 || ack->first_owed_seq_no < owed) {
            owed = ack->first_owed_seq_no;
        }
        ack->match_heartbeats_left--;
        still_pending |= ack->match_heartbeats_left != 0;
    }
    pub->match_heartbeat_pending = still_pending;
    return owed;
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
    forget_peer(pub->peers, node_id); // compacting, so the table stays dense - see forget_peer()
    if (!preserve_ack) {
        forget_peer_ack(pub, node_id, 0, /*match_any_entity=*/true);
    }
}

// Drops (node_id) from the peer and ack sets of every local Publisher sharing `endpoint_id` - the
// per-entity counterpart to forget_peers_from_source(), used when one remote Subscriber is
// presumed dead by its own liveliness lease while its node is otherwise still alive and
// announcing (Phase 3 prerequisite (a), rmw_tickle/PLAN.md).
static void forget_publisher_peers_for_endpoint(struct tt_Context* node, uint32_t endpoint_id, uint8_t node_id) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_PUBLISHER || endpoint->id != endpoint_id) {
            continue;
        }
        forget_publisher_peer((struct tt_Publisher*)endpoint, node_id, /*preserve_ack=*/false);
    }
}

// forget_publisher_peers_for_endpoint() for one remote Subscriber entity whose context hosts another of the topic:
// the peer (the context's address) stays, the departed reader's ack entry goes.
static void forget_publisher_acks_for_entity(struct tt_Context* node, uint32_t endpoint_id, uint8_t node_id,
                                             uint32_t entity_id) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_PUBLISHER || endpoint->id != endpoint_id) {
            continue;
        }
        forget_peer_ack((struct tt_Publisher*)endpoint, node_id, entity_id, /*match_any_entity=*/false);
    }
}

// Clears the ack state of every local Publisher that `node_id` is no longer a matched peer of -
// process_announce()'s own companion to forget_peers_from_source(..., preserve_ack=true), run once
// decode_update_entities() has re-added whatever the fresh announce still lists (Phase 3
// prerequisite (c), rmw_tickle/PLAN.md). A Publisher this node is still matched to keeps its ack
// watermark untouched across the announce.
static void drop_ack_state_for_unmatched_source(struct tt_Context* node, uint8_t node_id) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_PUBLISHER) {
            continue;
        }
        struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
        bool still_matched = false;
        for (int j = 0; j < tt_MAX_PEER_COUNT; j++) {
            if (pub->peers[j].context_id == node_id) {
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
// node. Called when a fresh announce from that source arrives (process_announce): its new announce is
// authoritative for what it still hosts, and decode_update_entities() re-adds whatever's still
// listed. Also does the right thing for a node that has left - tt_Context_destroy() broadcasts a
// final entity-less announce, so this forgets it and nothing gets re-added.
static void forget_peers_from_source(struct tt_Context* node, uint8_t node_id, bool preserve_ack) {
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

// tt_Context.liveliness_flags bits - see its comment (tickle.h) and refresh_liveliness_flags().
enum {
    tt_LIVELINESS_SOURCE_MANUAL = 1U << 0, // announced a leased MANUAL_BY_TOPIC Publisher
    tt_LIVELINESS_SOURCE_LAPSED = 1U << 1, // has a leased entity tombstoned while the node is still heard
};
static void refresh_liveliness_flags(struct tt_Context* node, uint8_t source);
static void arm_liveliness_check(struct tt_Context* node, uint64_t due_ns);
static void reschedule_summary_for_leases(struct tt_Context* node, uint64_t now);
static const struct tt_DiscoveredEntity* discovery_find_kind(const struct tt_Discovery* discovery, uint8_t context_id,
                                                             uint32_t endpoint_id, uint8_t kind);

#if tt_DISCOVERY_INDEXED
// The discovery table's index (struct tt_Discovery.index, CONTEXT_NODE_PLAN.md 4b): open addressing, linear probing.
// The context id spread by the golden-ratio constant, then murmur3's 32-bit finaliser steps (multiplier, shifts).
#define DISCOVERY_HASH_GOLDEN 0x9E3779B1U
#define DISCOVERY_HASH_MIX 0x85EBCA6BU
#define DISCOVERY_HASH_SHIFT_HIGH 16U
#define DISCOVERY_HASH_SHIFT_LOW 13U

static uint32_t discovery_hash(uint8_t context_id, uint32_t endpoint_id) {
    uint32_t hash = endpoint_id ^ ((uint32_t)context_id * DISCOVERY_HASH_GOLDEN);
    hash ^= hash >> DISCOVERY_HASH_SHIFT_HIGH;
    hash *= DISCOVERY_HASH_MIX;
    hash ^= hash >> DISCOVERY_HASH_SHIFT_LOW;
    return hash & (tt_DISCOVERY_INDEX_SIZE - 1U);
}

// The slot holding (context_id, endpoint_id) - with match_entity, the one whose entity_id is also `entity_id` - or
// -1. Hashed on the endpoint, not the entity: every lookup on the receive path knows the endpoint, and the entities
// of one endpoint on one context (two publishers of a topic in one process) share its probe chain. The index is never
// deleted from, only rebuilt (tt_Discovery_reindex()), so no chain has a hole and the probe meets each of them before
// the first empty entry.
// A key for discovery_probe(): which of the entities of (context_id, endpoint_id) is wanted.
struct discovery_key {
    bool match_entity;
    uint32_t entity_id;
    bool match_kind;
    uint8_t kind;
};

static int32_t discovery_probe(const struct tt_Discovery* discovery, uint8_t context_id, uint32_t endpoint_id,
                               struct discovery_key key) {
    uint32_t position = discovery_hash(context_id, endpoint_id);
    for (uint32_t probe = 0; probe < tt_DISCOVERY_INDEX_SIZE; probe++) {
        uint16_t entry = discovery->index[position];
        if (entry == 0) {
            return -1;
        }
        const struct tt_DiscoveredEntity* entity = &discovery->entities[entry - 1U];
        if (entity->context_id == context_id && entity->endpoint_id == endpoint_id &&
            (!key.match_entity || entity->entity_id == key.entity_id) &&
            (!key.match_kind || entity->kind == key.kind)) {
            return (int32_t)(entry - 1U);
        }
        position = (position + 1U) & (tt_DISCOVERY_INDEX_SIZE - 1U);
    }
    return -1;
}

static int32_t discovery_slot_of(const struct tt_Discovery* discovery, uint8_t context_id, uint32_t endpoint_id) {
    return discovery_probe(discovery, context_id, endpoint_id, (struct discovery_key) {0});
}

static int32_t discovery_kind_slot_of(const struct tt_Discovery* discovery, uint8_t context_id, uint32_t endpoint_id,
                                      uint8_t kind) {
    return discovery_probe(discovery, context_id, endpoint_id,
                           (struct discovery_key) {.match_kind = true, .kind = kind});
}

static int32_t discovery_entity_slot_of(const struct tt_Discovery* discovery, uint8_t context_id, uint32_t endpoint_id,
                                        uint32_t entity_id) {
    return discovery_probe(discovery, context_id, endpoint_id,
                           (struct discovery_key) {.match_entity = true, .entity_id = entity_id});
}

// Bounded, although at twice the table's size the index cannot fill while it holds only live keys: a probe that
// never ends would hang the poll thread, where a missing index entry costs one lookup.
static void discovery_index_add(struct tt_Discovery* discovery, uint32_t slot) {
    const struct tt_DiscoveredEntity* entity = &discovery->entities[slot];
    uint32_t position = discovery_hash(entity->context_id, entity->endpoint_id);
    for (uint32_t probe = 0; probe < tt_DISCOVERY_INDEX_SIZE; probe++) {
        if (discovery->index[position] == 0) {
            discovery->index[position] = (uint16_t)(slot + 1U);
            return;
        }
        position = (position + 1U) & (tt_DISCOVERY_INDEX_SIZE - 1U);
    }
    TT_LOG_ERROR("Discovery index full - node %u endpoint %08x not indexed", entity->context_id, entity->endpoint_id);
}

void tt_Discovery_reindex(struct tt_Discovery* discovery) {
    if (discovery == NULL) {
        return;
    }
    memset(discovery->index, 0, sizeof(discovery->index));
    for (uint32_t slot = 0; slot < tt_MAX_DISCOVERED_ENTITIES; slot++) {
        if (discovery->entities[slot].context_id != tt_CONTEXT_ID_INVALID) {
            discovery_index_add(discovery, slot);
        }
    }
    discovery->free_cursor = 0;
}

// An empty slot, searched from the cursor, or else the first tombstoned one (reclaimed, so its old key leaves the
// index); NULL when there is neither.
static struct tt_DiscoveredEntity* discovery_free_slot(struct tt_Discovery* discovery, bool* reclaimed) {
    *reclaimed = false;
    for (uint32_t step = 0; step < tt_MAX_DISCOVERED_ENTITIES; step++) {
        uint32_t slot = (discovery->free_cursor + step) % tt_MAX_DISCOVERED_ENTITIES;
        if (discovery->entities[slot].context_id == tt_CONTEXT_ID_INVALID) {
            discovery->free_cursor = (uint16_t)((slot + 1U) % tt_MAX_DISCOVERED_ENTITIES);
            return &discovery->entities[slot];
        }
    }
    for (uint32_t slot = 0; slot < tt_MAX_DISCOVERED_ENTITIES; slot++) {
        if (!discovery->entities[slot].alive) {
            *reclaimed = true;
            return &discovery->entities[slot];
        }
    }
    return NULL;
}

#endif
// Records one remote entity into node->discovery (tt_Context_set_discovery(), rmw_tickle/PLAN.md's
// Milestone 0(c)), refreshing its existing slot or claiming the first empty one, then fires the
// appear/refresh callback. An entity is its (context_id, entity_id): two publishers of one topic in one
// remote process share the endpoint_id, and keyed on that the second overwrote the first, so the graph
// listed one of them and every count came out short (2026-10-08). No-op (not even the callback) if no discovery cache
// is attached - every caller below calls this unconditionally rather than checking node->discovery first, the same way
// logging macros check their own level instead of every call site checking it.
static void upsert_discovered_entity(struct tt_Context* node, uint8_t node_id, uint32_t endpoint_id, uint32_t entity_id,
                                     uint8_t kind, uint8_t node_index, uint8_t qos, uint64_t deadline_duration_ns,
                                     uint64_t liveliness_lease_duration_ns, const char* type, const char* name) {
    if (node->discovery == NULL) {
        return;
    }

#if tt_DISCOVERY_INDEXED
    struct tt_Discovery* discovery = node->discovery;
    int32_t found = discovery_entity_slot_of(discovery, node_id, endpoint_id, entity_id);
    struct tt_DiscoveredEntity* slot = found >= 0 ? &discovery->entities[found] : NULL;
    bool is_new = slot == NULL;
    bool reclaimed = false;
    if (is_new) {
#else
    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    struct tt_DiscoveredEntity* slot = NULL;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (entities[i].context_id == node_id && entities[i].endpoint_id == endpoint_id &&
            entities[i].entity_id == entity_id) {
            slot = &entities[i];
            break;
        }
    }
    if (slot == NULL) {
#endif
        // Prefer a truly-empty slot; fall back to reclaiming the first tombstoned one (struct
        // tt_DiscoveredEntity.alive's own doc comment, tickle.h) rather than dropping a genuinely
        // new entity on the floor while the table still has room for it in spirit, just not in a
        // never-used slot - tombstones are remembered on a best-effort basis, not guaranteed.
#if tt_DISCOVERY_INDEXED
        slot = discovery_free_slot(discovery, &reclaimed);
#else
        struct tt_DiscoveredEntity* tombstone_slot = NULL;
        for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
            if (entities[i].context_id == tt_CONTEXT_ID_INVALID) {
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
#endif
    }
    if (slot == NULL) {
        // Counted every time and said once: a full table is not only an introspection gap (see struct tt_Discovery),
        // so it must never pass for a quiet one, and a log line per entity per announce would bury everything else.
        node->discovery->entities_dropped++;
        if (!node->discovery->full_warned) {
            node->discovery->full_warned = true;
            TT_LOG_WARNING("Discovery table full (%d entities): dropping node %u endpoint %08x and every later new one "
                           "- raise tt_MAX_DISCOVERED_ENTITIES. RxO, KEEP_ALL and liveliness are not checked for "
                           "entities not in it",
                           tt_MAX_DISCOVERED_ENTITIES, node_id, endpoint_id);
        }
        return;
    }

    slot->context_id = node_id;
    slot->endpoint_id = endpoint_id;
    // Part of the key: a restarted publisher keeps its endpoint_id (same topic, same name) and gets a NEW
    // entity_id, and lands in a slot of its own - its predecessor's goes with the new announce generation
    // (forget_discovered_entities_from_source()), which is what a restart announces.
    slot->entity_id = entity_id;
    slot->kind = kind;
    slot->node_index = node_index;
    slot->qos = qos;
    slot->deadline_duration_ns = deadline_duration_ns;
    slot->liveliness_lease_duration_ns = liveliness_lease_duration_ns;
    slot->alive = true;
#if tt_DISCOVERY_INDEXED
    if (reclaimed) {
        tt_Discovery_reindex(discovery); // the tombstone's old key leaves the index with it
    } else if (is_new) {
        discovery_index_add(discovery, (uint32_t)(slot - discovery->entities));
    }
#endif
    uint64_t now = tt_get_ns();
    slot->last_asserted_ns = now; // being announced is a sign of life
#if tt_DISCOVERY_INDEXED
    // The source's liveliness flags are refreshed once per announce, after its entities (decode_update_entities()),
    // not per entity: each refresh scans the table.
#else
    refresh_liveliness_flags(node, node_id);
#endif
    if (liveliness_lease_duration_ns != 0) {
        arm_liveliness_check(node, now + liveliness_lease_duration_ns + 1);
    }
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
// supersedes old state" reasoning (a fresh announce means decode_update_entities() is about to
// re-add whatever `node_id` still actually hosts, so anything not re-added here first must have
// been dropped). A real removal (the slot is freed, not tombstoned) - this is a normal,
// intentional departure (an explicit farewell, or the node simply not listing this entity
// anymore), not a liveliness failure, and struct tt_DiscoveredEntity.alive's own doc comment (QoS
// roadmap #3, RMW_EVENT_LIVELINESS_CHANGED.not_alive_count) explicitly excludes normal deletion
// from "not alive" - see tombstone_discovered_entities_from_source() below for the liveliness-
// timeout counterpart that keeps the entity instead. No-op if no discovery cache is attached.
static void forget_discovered_entities_from_source(struct tt_Context* node, uint8_t node_id) {
    if (node->discovery == NULL) {
        return;
    }

    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (entities[i].context_id != node_id) {
            continue;
        }
        uint32_t endpoint_id = entities[i].endpoint_id;
        uint8_t kind = entities[i].kind;
        entities[i].context_id = tt_CONTEXT_ID_INVALID;
        if (node->discovery_callback != NULL) {
            node->discovery_callback(node, node_id, endpoint_id, kind, /*departed=*/true,
                                     node->discovery_callback_param);
        }
    }
#if tt_DISCOVERY_INDEXED
    tt_Discovery_reindex(node->discovery); // the forgotten keys leave the index
#endif
    refresh_liveliness_flags(node, node_id);
}

// check_liveliness()'s own counterpart to forget_discovered_entities_from_source() just above -
// same trigger (a source presumed dead) but tombstones instead of freeing the slot (alive =
// false, entity otherwise left intact) so QoS roadmap #3's own RMW_EVENT_LIVELINESS_CHANGED.
// not_alive_count (a live snapshot, rmw_tickle/PLAN.md) has a real "known but not currently alive"
// set to count instead of always reporting 0. Still fires the discovery callback with
// departed=true - an existing plain appear/depart consumer doesn't need to know about the
// tombstone distinction, only rmw_tickle_c's own count_not_alive_matching_locked() (rmw_graph.c)
// needs to see the .alive flag directly. No-op if no discovery cache is attached.
static void tombstone_discovered_entities_from_source(struct tt_Context* node, uint8_t node_id) {
    if (node->discovery == NULL) {
        return;
    }

    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (entities[i].context_id != node_id || !entities[i].alive) {
            continue; // not from this source, or already tombstoned - nothing new to report
        }
        entities[i].alive = false;
        if (node->discovery_callback != NULL) {
            node->discovery_callback(node, node_id, entities[i].endpoint_id, entities[i].kind, /*departed=*/true,
                                     node->discovery_callback_param);
        }
    }
}

// The context's nodes as announce entries (stage 3), after its endpoints: every node created explicitly, and the
// default node only while it owns an endpoint - an empty implicit node would be a phantom in every peer's graph.
static uint32_t announced_nodes(struct tt_Context* context, struct tt_Endpoint** out) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < tt_MAX_NODES; index++) {
        struct tt_Node* node = context->nodes[index];
        if (node == NULL) {
            continue;
        }
        if (node == &context->default_node) {
            bool owns_one = false;
            for (uint32_t i = 0; i < context->endpoint_count && !owns_one; i++) {
                owns_one = context->endpoints[i]->node_index == index;
            }
            if (!owns_one) {
                continue;
            }
        }
        out[count++] = &node->entry;
    }
    return count;
}

// Milestone 35 (rmw_tickle/PLAN.md) - deliberately does NOT reject a second endpoint sharing an
// already-registered (kind, id): real DDS lets multiple independent entities (Publishers,
// Subscribers, Clients, or Servers) share one topic/service name, and rmw_tickle/PLAN.md's own
// Milestone 34 (multiple ROS 2 nodes per process, sharing one tt_Context) made this reachable within
// a single process for the first time - two rmw nodes in one process each creating a Publisher
// for the same topic, or a Server for the same service, is a normal pattern this used to reject
// outright as "Duplicate endpoint", which was never really a wire-protocol requirement, only a
// side effect of this table's own single-entry-per-id assumption (see find_endpoint()'s and
// for_each_endpoint()'s own doc comments for how lookups now handle more than one match). Still
// guards against the one thing that IS always a real bug: registering the exact same struct
// pointer twice (a double-create without an intervening destroy).
static bool build_and_send_update(struct tt_Context* node, const struct tt_Peer* peers, uint8_t peer_count);

// struct tt_Context.announce_soon_scheduled: the announce a new endpoint is owed, sent once the burst it came
// in has gone quiet for tt_CONTEXT_TX_INTERVAL. Rescheduling itself rather than unscheduling on every creation
// keeps creation to a clock read and a store. The periodic node_update() is untouched - this is one extra
// announce per burst of changes, not a faster cadence. Since tt_VERSION 8 this broadcast is how a change
// is pushed; a peer that misses it pulls the list when the next summary shows the new generation.
static void announce_soon(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(param);
    uint64_t quiet_at = node->endpoints_changed_ns + tt_CONTEXT_TX_INTERVAL;
    if (time < quiet_at) {
        if (tt_Context_schedule(node, quiet_at, announce_soon, NULL)) {
            return;
        }
        TT_LOG_ERROR("Cannot reschedule announce_soon"); // peers still pull it on the next summary
    }
    node->announce_soon_scheduled = false;
    build_and_send_update(node, NULL, 0);
    reschedule_summary_for_leases(node, time);
}

static void arm_announce_soon(struct tt_Context* node) {
    node->endpoints_changed_ns = tt_get_ns();
    if (node->announce_soon_scheduled) {
        return;
    }
    if (tt_Context_schedule(node, node->endpoints_changed_ns + tt_CONTEXT_TX_INTERVAL, announce_soon, NULL)) {
        node->announce_soon_scheduled = true;
    }
}

static struct tt_Node* default_node_locked(struct tt_Context* context);

// `owner`: the node the endpoint is created on; NULL for the context's default node, which is brought into use here,
// once the endpoint is known to be registered - a shorthand call that fails leaves it as it was.
#if tt_LOCAL_DELIVERY
// (g9) A Subscriber joining (delta 1) or leaving (-1) changes the local Subscriber count of every Publisher of this
// context on its endpoint id; a Publisher joining counts the Subscribers already there. Linear over the endpoints, on
// creation and destruction only.
static void count_local_pairs(struct tt_Context* node, struct tt_Endpoint* endpoint, int delta) {
    if (endpoint->kind != tt_KIND_TOPIC_SUBSCRIBER && endpoint->kind != tt_KIND_TOPIC_PUBLISHER) {
        return;
    }
    uint16_t subscribers = 0;
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* other = node->endpoints[i];
        if (other == endpoint || other->id != endpoint->id) {
            continue;
        }
        if (endpoint->kind == tt_KIND_TOPIC_SUBSCRIBER && other->kind == tt_KIND_TOPIC_PUBLISHER) {
            struct tt_Publisher* pub = (struct tt_Publisher*)other;
            pub->local_subscriber_count = (uint16_t)(pub->local_subscriber_count + delta);
        } else if (endpoint->kind == tt_KIND_TOPIC_PUBLISHER && other->kind == tt_KIND_TOPIC_SUBSCRIBER) {
            subscribers++;
        }
    }
    if (endpoint->kind == tt_KIND_TOPIC_PUBLISHER) {
        ((struct tt_Publisher*)endpoint)->local_subscriber_count = delta > 0 ? subscribers : 0;
    }
}
#endif

static tt_ret_t add_endpoint_to_node(struct tt_Context* node, struct tt_Endpoint* endpoint, struct tt_Node* owner) {
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

    // The default node when no owner is given. Nothing below can fail, so a call that fails leaves it unused; it
    // cannot come into being when an explicit node took index 0 (a context without a default node may use all
    // tt_MAX_NODES indices).
    struct tt_Node* owner_node = owner != NULL ? owner : default_node_locked(node);
    if (owner_node == NULL) {
        TT_LOG_ERROR("No default node: index 0 is taken by node %s", node->nodes[0]->name);
        return tt_RET_OUT_OF_BUFFER;
    }

    // Milestone 47 - every entity kind gets its own entity_id here, the single shared
    // registration point for all four (Publisher/Subscriber/Client/Server) - see struct tt_
    // Endpoint.entity_id's own doc comment (tickle.h) for what this is and struct tt_Context.
    // entity_id_base/next_entity_id's own doc comment for the generation scheme.
    endpoint->entity_id = node->entity_id_base + node->next_entity_id++;
    if (endpoint->entity_id == tt_DISCOVERY_ENTITY_ID) {
        // Reserved for the node's own discovery announce, which a fragment names by entity_id alone
        // (tt_DISCOVERY_ENDPOINT_ID, tickle.h). Reached only when the launch-drawn base lands next to it.
        endpoint->entity_id = node->entity_id_base + node->next_entity_id++;
    }

    endpoint->node_index = owner_node->index;
    node->endpoints[node->endpoint_count++] = endpoint;
    node->endpoint_index_valid = false;
    arm_announce_soon(node);
#if tt_LOCAL_DELIVERY
    count_local_pairs(node, endpoint, 1);
#endif

    return tt_RET_OK;
}

static bool remove_endpoint_from_node(struct tt_Context* node, struct tt_Endpoint* endpoint) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        if (node->endpoints[i] == endpoint) {
#if tt_LOCAL_DELIVERY
            count_local_pairs(node, endpoint, -1);
#endif
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
    case tt_KIND_NODE: // a node's announce entry (stage 3), which is a struct tt_Node's first member
        return ((struct tt_Node*)endpoint)->namespace_name;
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

// Threading (tt_THREAD_SAFE, config.h) - see "Threading" at tt_Context_lock() in tickle.h for the contract.
//
// One lock per node, the state lock, and it tracks its own owner: a plain mutex plus the owning thread and a
// depth, rather than a recursive mutex. Callbacks run with it held and routinely call back into core, and
// on the publish path of a max-rate publisher that happens several times per sample; re-entry by the owner
// is then a thread-id compare instead of an atomic. The rig showed why that matters: with a recursive mutex
// and a separate scheduler lock, about five lock operations per sample cost ~215 ns on the Raspberry Pi -
// uncontended, and in place about three times what a tight-loop microbenchmark of the same lock suggested.
//
// Taken with a try first so the uncontended case costs one atomic and the contended one is counted, with
// how long it waited: whether the lock is worth splitting is a measurement, not something to decide here.
// tt_lock_*/tt_thread_self: from the platform header hal.h selects (hal_linux.h, hal_freertos.h), which
// include-cleaner cannot see through - the same reason struct tt_Context.hal carries a NOLINT.
static bool state_lock_owned(struct tt_Context* node) {
    return __atomic_load_n(&node->state_owner, __ATOMIC_RELAXED) == tt_thread_self(); // NOLINT(misc-include-cleaner)
}

// Called with the mutex just taken.
static void state_lock_taken(struct tt_Context* node) {
    __atomic_store_n(&node->state_owner, tt_thread_self(), __ATOMIC_RELAXED); // NOLINT(misc-include-cleaner)
    node->state_depth = 1;
    node->state_lock_stats.acquisitions++;
}

// Counts a wait for the state lock that began at start and has just ended with the lock held.
static void state_lock_waited(struct tt_Context* node, uint64_t start) {
    uint64_t waited = tt_get_ns() - start;
    node->state_lock_stats.contended++;
    node->state_lock_stats.wait_ns += waited;
    if (__atomic_load_n(&node->poller_thread, __ATOMIC_RELAXED) == tt_thread_self()) { // NOLINT(misc-include-cleaner)
        node->state_lock_stats.poller_contended++;
        node->state_lock_stats.poller_wait_ns += waited;
    }
}

// The poller goes first (2026-10-05). The mutex has no hand-off: a thread that releases it and asks again - an rmw
// publish loop, every sample - takes it back before the waiter it just woke has run, so the poll thread could wait
// tens of milliseconds for its turn while ACKNACKs and announces sat unread on its sockets. A KEEP_ALL writer then
// matched its reader 0.1-0.35 s late on the rig and evicted what it broadcast meanwhile; in a netns the poller's wait
// was 11 ms of an 11.5 ms window and 59 ms of a 60 ms one (experiments/rmw_keepall_evict_repro.sh). So a poller that
// has to wait says so (poller_waiting), and any other thread asking for the lock yields until the poller has it.
// Only the blocking path defers: state_try_lock() still answers at once, which tt_Context_schedule() relies on to
// fall back to its inbox instead of waiting while it may already hold the lock in a callback (poll_wait_io()).
static void state_lock(struct tt_Context* node) {
    if (state_lock_owned(node)) {
        node->state_depth++; // re-entry from a callback, or a public call made inside another
        return;
    }
    bool is_poller =
        __atomic_load_n(&node->poller_thread, __ATOMIC_RELAXED) == tt_thread_self(); // NOLINT(misc-include-cleaner)
#if tt_HAL_THREAD_YIELD // NOLINT(misc-include-cleaner) - from the platform header hal.h selects
    if (!is_poller) {
        while (__atomic_load_n(&node->poller_waiting, __ATOMIC_ACQUIRE) != 0) {
            tt_thread_yield(); // NOLINT(misc-include-cleaner)
        }
    }
#endif
    if (!tt_lock_try(&node->state_lock)) { // NOLINT(misc-include-cleaner)
        uint64_t start = tt_get_ns();
        if (is_poller) {
            __atomic_add_fetch(&node->poller_waiting, 1U, __ATOMIC_RELEASE);
        }
        tt_lock_acquire(&node->state_lock); // NOLINT(misc-include-cleaner)
        if (is_poller) {
            __atomic_sub_fetch(&node->poller_waiting, 1U, __ATOMIC_RELEASE);
        }
        state_lock_waited(node, start);
    }
    state_lock_taken(node);
}

// state_lock() without waiting: true if it is now held (by this thread, possibly re-entered).
static bool state_try_lock(struct tt_Context* node) {
    if (state_lock_owned(node)) {
        node->state_depth++;
        return true;
    }
    if (!tt_lock_try(&node->state_lock)) { // NOLINT(misc-include-cleaner)
        return false;
    }
    state_lock_taken(node);
    return true;
}

static void state_unlock(struct tt_Context* node) {
    if (--node->state_depth == 0) {
        __atomic_store_n(&node->state_owner, 0, __ATOMIC_RELAXED);
        tt_lock_release(&node->state_lock); // NOLINT(misc-include-cleaner)
    }
}

void tt_Context_lock(struct tt_Context* node) {
    if (node != NULL) {
        state_lock(node);
    }
}

bool tt_Context_lock_timed(struct tt_Context* node, uint64_t timeout_ns) {
    if (node == NULL) {
        return false;
    }
    if (state_lock_owned(node)) {
        node->state_depth++;
        return true;
    }
    if (!tt_lock_try(&node->state_lock)) { // NOLINT(misc-include-cleaner)
        uint64_t start = tt_get_ns();
        if (!tt_lock_acquire_timed(&node->state_lock, timeout_ns)) { // NOLINT(misc-include-cleaner)
            return false;
        }
        state_lock_waited(node, start);
    }
    state_lock_taken(node);
    return true;
}

void tt_Context_unlock(struct tt_Context* node) {
    if (node != NULL) {
        state_unlock(node);
    }
}

// scheduler[] is a binary min-heap keyed on TCB.time (heap[0] = earliest), sized by
// scheduler_tail. schedule/pop/unschedule are all O(log N) sift operations with no array
// memmove. Equal-time entries no longer keep strict FIFO order (heaps don't) - nothing in this
// codebase's scheduling depends on that.
static void sched_sift_up(struct tt_Context* node, int32_t index) {
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

static void sched_sift_down(struct tt_Context* node, int32_t index) {
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

static void sched_heap_insert(struct tt_Context* node, uint64_t time,
                              void (*function)(struct tt_Context* node, uint64_t time, void* param), void* param) {
    int32_t index = node->scheduler_tail;
    node->scheduler[index].time = time;
    node->scheduler[index].function = function;
    node->scheduler[index].param = param;
    node->scheduler_tail++;
    sched_sift_up(node, index);
}

// struct tt_Context.wait_until, as a seqlock over two 32-bit halves. A 64-bit atomic would be simpler, but a
// 32-bit target has none in hardware - rv32 builds of this file failed to link on __atomic_store_8, and
// picolibc brings no libatomic - so the value is written under a sequence counter instead. Only the poller
// writes it. The final store of the counter is sequentially consistent, and it is that store, paired with
// the reader's first load, that carries the ordering poll_wait_io() and wake_if_waiting_past() rely on.
static void wait_until_store(struct tt_Context* node, uint64_t value) {
    uint32_t seq = __atomic_load_n(&node->wait_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&node->wait_seq, seq + 1, __ATOMIC_RELAXED); // odd: a write is in progress
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&node->wait_until_hi, (uint32_t)(value >> 32U), __ATOMIC_RELAXED);
    __atomic_store_n(&node->wait_until_lo, (uint32_t)value, __ATOMIC_RELAXED);
    __atomic_store_n(&node->wait_seq, seq + 2, __ATOMIC_SEQ_CST);
}

static uint64_t wait_until_load(struct tt_Context* node) {
    uint32_t before = 0;
    uint32_t after = 0;
    uint32_t high = 0;
    uint32_t low = 0;
    do {
        before = __atomic_load_n(&node->wait_seq, __ATOMIC_SEQ_CST);
        high = __atomic_load_n(&node->wait_until_hi, __ATOMIC_RELAXED);
        low = __atomic_load_n(&node->wait_until_lo, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = __atomic_load_n(&node->wait_seq, __ATOMIC_RELAXED);
    } while ((before & 1U) != 0 || before != after);
    return ((uint64_t)high << 32U) | low;
}

// Whether a poll blocked in tt_receive() is waiting for something later than `time`, and must be woken.
// Paired with poll_wait_io()'s store of wait_until and its re-check of the inbox: both sides write, then
// read the other's variable, all sequentially consistent, so at least one of them sees the other.
static void wake_if_waiting_past(struct tt_Context* node, uint64_t time) {
    uint64_t until = wait_until_load(node);
    if (until != 0 && time < until) {
        tt_wake_signal(node);
    }
}

// The scheduler inbox: how a thread that does not hold the state lock schedules without taking it - the
// user's own design, "put it in the scheduler and interrupt", made lock-free. A fixed ring of slots, each
// claimed by compare-and-swap, the same way tt_Server_send_response() hands a response to the poll thread
// (struct tt_Server.slot_state). The heap itself belongs to whoever holds the state lock; the poll thread
// moves inbox entries into it (sched_drain_inbox()) each time it looks at the heap.
static bool sched_inbox_push(struct tt_Context* node, uint64_t time,
                             void (*function)(struct tt_Context* node, uint64_t time, void* param), void* param) {
    for (int i = 0; i < tt_SCHED_INBOX_LENGTH; i++) {
        uint8_t expected = tt_SCHED_SLOT_EMPTY;
        if (!__atomic_compare_exchange_n(&node->sched_inbox_state[i], &expected, tt_SCHED_SLOT_WRITING, false,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            continue;
        }
        node->sched_inbox[i].time = time;
        node->sched_inbox[i].function = function;
        node->sched_inbox[i].param = param;
        __atomic_store_n(&node->sched_inbox_state[i], tt_SCHED_SLOT_READY, __ATOMIC_RELEASE);
        __atomic_fetch_add(&node->sched_inbox_pending, 1, __ATOMIC_SEQ_CST);
        return true;
    }
    return false; // every slot busy: the caller falls back to the state lock
}

// Called with the state lock held. Costs one atomic load when the inbox is empty, which is almost always.
static void sched_drain_inbox(struct tt_Context* node) {
    if (__atomic_load_n(&node->sched_inbox_pending, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    for (int i = 0; i < tt_SCHED_INBOX_LENGTH; i++) {
        if (__atomic_load_n(&node->sched_inbox_state[i], __ATOMIC_ACQUIRE) != tt_SCHED_SLOT_READY) {
            continue;
        }
        if (node->scheduler_tail + 1 >= tt_MAX_SCHEDULER_LENGTH) {
            break; // heap full: leave the rest queued, the next drain tries again
        }
        struct tt_TCB tcb = node->sched_inbox[i];
        __atomic_store_n(&node->sched_inbox_state[i], tt_SCHED_SLOT_EMPTY, __ATOMIC_RELEASE);
        __atomic_fetch_sub(&node->sched_inbox_pending, 1, __ATOMIC_RELEASE);
        sched_heap_insert(node, tcb.time, tcb.function, tcb.param);
    }
}

// Straight into the heap whenever the state lock can be had without waiting - held already (the poll
// thread inside a callback, which for a self-rescheduling publisher is every sample, or any thread inside
// a core call or tt_Context_lock()), or free. Only when another thread holds it does the entry go through the
// inbox, without a lock and without waiting behind whatever that thread is doing; a full inbox, rare, then
// waits for the lock after all. Either way a poll waiting for something later than `time` is woken
// (wake_if_waiting_past()), so no caller has to remember tt_Context_interrupt().
bool tt_Context_schedule(struct tt_Context* node, uint64_t time,
                         void (*function)(struct tt_Context* node, uint64_t time, void* param), void* param) {
    if (!state_try_lock(node)) {
        if (sched_inbox_push(node, time, function, param)) {
            wake_if_waiting_past(node, time);
            return true;
        }
        state_lock(node);
    }
    bool room = node->scheduler_tail + 1 < tt_MAX_SCHEDULER_LENGTH;
    if (room) {
        sched_heap_insert(node, time, function, param);
    }
    state_unlock(node);
    if (room) {
        wake_if_waiting_past(node, time);
    }
    return room;
}

static bool unschedule_locked(struct tt_Context* node,
                              void (*function)(struct tt_Context* node, uint64_t time, void* param), void* param);

// Under the state lock, which entries run inside: an entry already taken out of the heap to run is waited
// out, so after this returns `function` is neither pending nor running for `param` and it may be freed.
// Entries still in the inbox are cancelled too. One being written into the inbox by another thread at this
// very moment is not - that insert and this cancel are concurrent, and neither is ordered before the other.
bool tt_Context_unschedule(struct tt_Context* node,
                           void (*function)(struct tt_Context* node, uint64_t time, void* param), void* param) {
    state_lock(node);
    sched_drain_inbox(node);
    bool removed = unschedule_locked(node, function, param);
    // The drain above cannot always empty the inbox - not into a full heap - so what is still there is
    // cancelled where it sits. Claimed the same way the drain claims a slot, so the two never both take it.
    // Only READY slots are read: a WRITING one is still being filled by its producer. And a READY slot
    // stays READY while the state lock is held, because only the drain (which needs the lock) empties one,
    // so the fields read here are still the slot's when it is claimed below.
    for (int i = 0; i < tt_SCHED_INBOX_LENGTH; i++) {
        if (__atomic_load_n(&node->sched_inbox_state[i], __ATOMIC_ACQUIRE) != tt_SCHED_SLOT_READY ||
            node->sched_inbox[i].function != function || node->sched_inbox[i].param != param) {
            continue;
        }
        uint8_t expected = tt_SCHED_SLOT_READY;
        if (__atomic_compare_exchange_n(&node->sched_inbox_state[i], &expected, tt_SCHED_SLOT_WRITING, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            __atomic_store_n(&node->sched_inbox_state[i], tt_SCHED_SLOT_EMPTY, __ATOMIC_RELEASE);
            __atomic_fetch_sub(&node->sched_inbox_pending, 1, __ATOMIC_RELEASE);
            removed = true;
        }
    }
    state_unlock(node);
    return removed;
}

static bool unschedule_locked(struct tt_Context* node,
                              void (*function)(struct tt_Context* node, uint64_t time, void* param), void* param) {
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

// Callers hold the state lock: the heap belongs to it.
static struct tt_TCB* peek_scheduler(struct tt_Context* node) {
    if (node->scheduler_tail > 0) {
        return &node->scheduler[0];
    }

    return NULL;
}

static void pop_scheduler(struct tt_Context* node) {
    node->scheduler_tail--;
    if (node->scheduler_tail > 0) {
        node->scheduler[0] = node->scheduler[node->scheduler_tail];
        sched_sift_down(node, 0);
    }
}

// When the earliest entry is due, or false when nothing is scheduled. A copy, taken under the lock.
static bool sched_next_time(struct tt_Context* node, uint64_t* time) {
    state_lock(node);
    sched_drain_inbox(node);
    const struct tt_TCB* tcb = peek_scheduler(node);
    if (tcb != NULL) {
        *time = tcb->time;
    }
    state_unlock(node);
    return tcb != NULL;
}

// Runs the earliest entry if it is due at `now` and returns true; otherwise returns false and reports when
// the next one is due (*has_next false when nothing is scheduled). One call answers both, because this sits
// on the per-sample path of a max-rate publisher: one lock acquisition per entry, and the entry's own
// reschedule inside it goes straight into the heap.
//
// The entry is copied out and popped before it runs. It used to run in place at scheduler[0] and be popped
// afterwards, which was safe only while nothing else could reorder the heap during the call; with other
// threads scheduling it no longer is. The state lock is held across the run, so tt_Context_unschedule() on
// another thread either removes the entry before it is taken or waits until it has finished.
static bool run_due_entry(struct tt_Context* node, uint64_t now, bool* has_next, uint64_t* next) {
    state_lock(node);
    sched_drain_inbox(node);
    const struct tt_TCB* head = peek_scheduler(node);
    *has_next = head != NULL;
    bool due = head != NULL && head->time <= now;
    struct tt_TCB tcb = {0};
    if (due) {
        tcb = *head;
        pop_scheduler(node);
        tcb.function(node, now, tcb.param);
    } else if (head != NULL) {
        *next = head->time;
    }
    state_unlock(node);
    return due;
}

static void node_update(struct tt_Context* node, uint64_t time, void* param);
// Milestone 47 "goodbye" - see its own definition's doc comment.
static void broadcast_goodbye(struct tt_Context* node);
#if tt_LOCAL_DELIVERY
static void deliver_locally(struct tt_Context* node, struct tt_Publisher* pub, const uint8_t* payload, uint32_t length,
                            uint32_t seq_no, uint64_t timestamp);
#endif
static void check_liveliness(struct tt_Context* node, uint64_t time, void* param);
static void server_cache_clean(struct tt_Context* node, uint64_t time, void* param);
static void clear_server_cache_slot(struct tt_Server* server, int slot);
// QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - see each definition's own comment.
static void acknack_retry(struct tt_Context* node, uint64_t time, void* param);
static uint16_t reliable_cache_depth(const struct tt_ReliableCache* cache);
static uint64_t reliable_retry_interval(const struct tt_Context* node, const struct tt_WriterProxy* proxy);
static uint64_t reliable_retry_configured(void);
static uint64_t reliable_retry_interval_publisher(void);
static void note_watermark_requested(struct tt_WriterProxy* proxy, uint64_t now);
static void note_recovery_sample(struct tt_WriterProxy* proxy, uint64_t sample_ns);
static void rtt_estimate_fold(uint32_t* srtt_ns, uint32_t* rttvar_ns, uint64_t sample_ns);
static void send_acknack(struct tt_Context* node, struct tt_WriterProxy* proxy);
static void advance_ack_seq_no(struct tt_WriterProxy* proxy);
static void maybe_arm_acknack_retry(struct tt_Context* node, struct tt_WriterProxy* proxy);
static void answer_ack_request(struct tt_Context* node, struct tt_WriterProxy* proxy);
static bool update_reliable_ack(struct tt_Context* node, struct tt_Subscriber* sub, uint32_t seq_no,
                                uint8_t sender_node_id, uint32_t sender_entity_id, uint32_t sender_ip,
                                uint16_t sender_port);
static void jump_ack_baseline(struct tt_WriterProxy* proxy, uint32_t seq_no);
// RELIABLE in-order delivery - release any samples this Subscriber is holding for a writer that
// has gone away, so its slots do not stay occupied for a stream that will never resume.
static void release_reorder_slots_for_writer(struct tt_Context* node, struct tt_Subscriber* sub, uint8_t node_id,
                                             uint32_t entity_id, bool match_any_entity);
// Releases held samples the watermark has passed. Declared up here because acknack_retry()'s
// give-up moves the watermark too, and it is defined long before the delivery code.
static void drain_reorder(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_WriterProxy* proxy);
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
static void send_heartbeat(struct tt_Context* node, uint64_t time, void* param);
static void send_initial_heartbeat(struct tt_Context* node, struct tt_Publisher* pub, struct tt_Peer* target);
// QoS roadmap #5 (RELIABILITY) follow-up - periodic ACK solicitation, see struct tt_Publisher.
// ack_solicit_period_ns's own doc comment (tickle.h).
static void send_ack_solicit(struct tt_Context* node, uint64_t time, void* param);
static bool process_heartbeat(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                              uint32_t tail, uint32_t sender_ip, uint16_t sender_port);
// QoS roadmap #4 (DURABILITY/TRANSIENT_LOCAL, rmw_tickle/PLAN.md) - see its own definition's comment.
static void deliver_durability_backlog(struct tt_Context* node, struct tt_Publisher* pub, struct tt_Peer* target);

// The node's lock and scheduler inbox, ready and empty. tt_Context_create() calls it; so does every unit test
// that builds a node by hand on the mock HAL instead - a zeroed node is not ready for FreeRTOS, and a
// zeroed inbox state array only happens to mean "empty".
static void node_init_locks(struct tt_Context* node) {
    tt_lock_init(&node->state_lock, /*recursive=*/false); // NOLINT(misc-include-cleaner) - re-entry: state_owner
    __atomic_store_n(&node->state_owner, 0, __ATOMIC_RELAXED);
    node->state_depth = 0;
    node->state_lock_stats = (struct tt_LockStats) {0};
    __atomic_store_n(&node->poller_thread, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&node->poller_waiting, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < tt_SCHED_INBOX_LENGTH; i++) {
        __atomic_store_n(&node->sched_inbox_state[i], tt_SCHED_SLOT_EMPTY, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&node->sched_inbox_pending, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&node->poller_active, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&node->wait_seq, 0, __ATOMIC_RELAXED);
    node->rx_clock_ns = 0;
    __atomic_store_n(&node->wait_until_hi, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&node->wait_until_lo, 0, __ATOMIC_RELAXED);
    // G (timer_lateness_fold()) starts cold, at tt_TIMER_LATENESS_INITIAL, and never falls below what this
    // platform's waits can resolve. Here and not in reset_node_state(): it is the host's lateness, not the node's.
    node->timer_lateness_mean_ns = 0;
    node->timer_lateness_var_ns = 0;
    __atomic_store_n(&node->timer_lateness_ns, 0, __ATOMIC_RELAXED);
    uint64_t resolution = tt_timer_resolution_ns();
    node->timer_resolution_ns = resolution > UINT32_MAX ? UINT32_MAX : (uint32_t)resolution;
}

// Whether any fragment of an announce from `source` is in progress, and forgetting them (see update_parts_complete()).
static bool update_parts_any(const struct tt_Context* node, uint8_t source) {
    for (uint32_t word = 0; word < tt_UPDATE_PART_WORDS; word++) {
        if (node->update_part_received[source][word] != 0) {
            return true;
        }
    }
    return false;
}

static void update_parts_clear(struct tt_Context* node, uint8_t source) {
    memset(node->update_part_received[source], 0, sizeof(node->update_part_received[source]));
}

static void reset_node_state(struct tt_Context* node) {
    node->id = tt_CONTEXT_ID_INVALID;
#if tt_LOCAL_DELIVERY
    node->local_delivery_depth = 0;
#endif
    node->endpoint_count = 0;
    node->endpoint_index_valid = false;

    for (int i = 0; i < tt_MAX_ENDPOINT_COUNT; i++) {
        node->endpoints[i] = NULL;
    }
    // No nodes hosted, and the default node not in use (CONTEXT_NODE_PLAN.md stage 2).
    for (int i = 0; i < tt_MAX_NODES; i++) {
        node->nodes[i] = NULL;
    }
    memset(&node->default_node, 0, sizeof(node->default_node));

    node->last_modified = 0;
    node->entity_id_base = 0; // real value assigned by tt_Context_create() itself, after this call
    node->next_entity_id = 0;
    node->call_seq_no = 0;

    for (int i = 0; i < tt_MAX_CONTEXT_IDS; i++) {
        node->update_generation[i] = 0;
        node->update_seen[i] = false;
        node->update_reprocess[i] = false;
        node->update_last_seen[i] = 0;
        node->update_part_generation[i] = 0;
        memset(node->update_part_received[i], 0, sizeof(node->update_part_received[i]));
        node->update_part_count[i] = 0;
        node->traffic_last_seen[i] = 0;
    }

    node->discovery = NULL;
    node->discovery_callback = NULL;
    node->discovery_callback_param = NULL;

    node->tx_datagrams = 0;
    // The per-transport pair beside their scalars, because they were added without being reset and every read of them
    // was stack garbage plus increments (found 2026-09-29 while verifying BenchStats' new tx_udp=/tx_shm= fields: a run
    // that really sent 949,078 datagrams reported tx_udp=140723338891185, a stack address). tt_Context is caller-owned
    // and this function initialises field by field rather than memset-ing, so a new field that is only ever incremented
    // is never zero - and a counter that starts from garbage cannot be told from one the seam failed to reach, which is
    // exactly the false negative SHM_PLAN's S2 exists to catch.
    for (int transport = 0; transport < tt_TRANSPORT_COUNT; transport++) {
        node->tx_datagrams_by_transport[transport] = 0;
        node->rx_datagrams_by_transport[transport] = 0;
    }
    // Reset with them, for the reason the pair above exists: a field added to this struct and not to
    // this function is never zero, and a counter that starts from garbage cannot be told from one
    // the code never reached.
    for (int reason = 0; reason < tt_SEGMENT_ATTACH_COUNT; reason++) {
        node->segment_attach[reason] = 0;
    }
    node->segment_broadcast_to_udp = 0;
    node->segment_oversized_to_udp = 0;
    node->segment_unattached_to_udp = 0;
    node->segment_full_dropped = 0;
#if tt_SEGMENT_ENABLED
    // The segment state, for the reason this function already gives twice over: tt_Context is
    // caller-owned and this initialises field by field, so a field added to the struct and not to
    // this function is never zero. It was a lie about a counter the first time. This time it is
    // POINTERS - segment_peers[].mapping held whatever was on the caller's stack, and the teardown
    // walked all 256 entries and called munmap() on 0x3, 0x40, 0x10 and the rest, unmapping parts of
    // the process at random. Every node in the integration suite segfaulted at exit, and no unit
    // test could see it: they memset their context before use, which is exactly what a real caller
    // is not required to do.
    // Not the table itself: segment_peer_live[] says which entries were ever set up, and segment_peer() zeroes an entry
    // the first time it is wanted. Zeroing all 14 KB here wrote every page of it in every context, including the ones
    // that never attach anything (stage 1 / S1's "on but unused" cost). The hazard above is closed as before - no
    // entry is read until it has been zeroed - only lazily.
    memset(node->segment_peer_live, 0, sizeof(node->segment_peer_live));
    node->own_segment = NULL;
    node->whole_refusals_logged = 0;
    // The same reasoning one step further, and with more riding on it than on a counter: with the
    // segment built on demand, same_host_peer[] is READ to decide whether to build one and
    // same_host_peer_count to decide whether to give it up. Left as stack garbage, a context would
    // believe in peers it has never heard from - building a segment at the first announce from
    // anywhere, or, worse, never releasing one because the count never reaches zero.
    memset(node->same_host_peer, 0, sizeof(node->same_host_peer));
#endif
    node->same_host_peer_count = 0;
    node->segments_created = 0;
    node->segments_released = 0;
    node->segment_full_warnings = 0;
    node->segment_head_stalls = 0;
    node->segment_head_stall_passes = 0;
    node->segment_stall_warnings = 0;
    node->liveliness_deferrals = 0;
    node->liveliness_deferrals_total = 0;
    node->segment_doorbells_sent = 0;
    node->segment_bells_rung = 0;
    node->segment_sleep_generation = 0;
    node->segment_slot_ceiling = 0;
    node->segment_doorbells_received = 0;
    node->segment_generation_unspent = 0;
    node->segment_unspent_doorbells = 0;
    node->segment_generations_kept = 0;
    node->segment_sleeps = 0;
    node->segment_sleep_cost_ns = 0;
    node->segment_sleep_cost_n = 0;
    node->segment_sleep_on_claim = 0;
    node->segment_claim_waits = 0;
    node->segment_claim_waits_published = 0;
    node->segment_gap_ns = 0;
    node->segment_stretch_ns = 0;
    node->segment_stretch_index = 0;
    node->segment_watches = 0;
    node->segment_watch_hits = 0;
    node->segment_watching = 0;
    node->segment_preferred = 0;
    node->segment_epoch_index = 0;
    node->segment_epoch_ns = 0;
    node->segment_epoch_cpu_ns = 0;
    node->segment_epoch_thread = 0;
    node->segment_epoch_settling = 0;
    node->segment_cost_n[0] = 0;
    node->segment_cost_n[1] = 0;
    node->segment_cost_mean_ns[0] = 0;
    node->segment_cost_mean_ns[1] = 0;
    node->segment_cost_m2[0] = 0;
    node->segment_cost_m2[1] = 0;
    node->segment_probe_every = 1;
    node->segment_probe_in = 0;
    node->segment_epochs[0] = 0;
    node->segment_epochs[1] = 0;
    node->segment_encoded_in_slot = 0;
    node->rx_drain_ring_turns = 0;
    // Counters that only ever increment, and therefore only ever reported whatever was on the
    // caller's stack. Found by the structural check Plan built after `segment_peers` shipped
    // uninitialised: this function is where a field is initialised, and the three below had been
    // added to the struct without being added here. Two of them are what a reader consults when a
    // node delivered nothing - "how many datagrams did we throw away, and why" - so answering that
    // from garbage is worse than not answering it.
    node->rx_malformed_drops = 0;
    node->rx_shm_only_on_socket = 0;
    node->rx_span_absorbed = 0;
    node->rx_shm_skipped_superseded = 0;
    node->segment_plan_base = 0;
    node->segment_plan_count = 0;
    node->segment_plan_mark = 0;
    node->rx_keep_last_delivered = 0;
    memset(node->segment_plan_skip, 0, sizeof(node->segment_plan_skip));
    memset(&node->segment_plan_run, 0, sizeof(node->segment_plan_run));
    memset(node->segment_skipping, 0, sizeof(node->segment_skipping));
    node->version_mismatch_drops = 0;
    // The span of the record currently being processed. 1 is its resting value, not a zero: a
    // datagram that carries no span consumes one seq_no, and process_datagram_locked() re-states it
    // on every arrival anyway. Garbage here would be read as "this record consumed N seq_nos" and
    // would skip a reader's watermark past samples that do exist - the opposite of the bug it was
    // added to fix, and silent in the same way.
    node->rx_seq_span = 1;
    // The rate limiter that decides whether a wire-version mismatch is EVER logged, and the worst of
    // this family because it is READ to make a decision rather than merely reported. Its only two
    // appearances in this file are a read and the write beside it, and nothing initialised it - so a
    // caller-owned context began with 256 bytes of stack garbage in a latch whose whole job is "log
    // this source once per version it speaks". One chance in 256 per source that the garbage byte
    // equals that source's version, and the single log line about a peer whose every datagram is
    // being dropped is suppressed. version_mismatch_drops still counts it, but a counter is what you
    // read once you suspect something and the log line is what tells you to suspect.
    memset(node->version_mismatch_logged, 0, sizeof(node->version_mismatch_logged));
#if tt_DISCOVERY_OPTIONS
    node->rx_out_of_range = 0; // the field itself only exists under this flag, as does every read of it
#endif
    node->summaries_skipped = 0;
    node->summaries_ridden = 0;
    node->tx_dropped_oversize = 0;
    node->rx_datagrams = 0;
    node->rx_self_sent = 0;
    node->rx_self_sent_data = 0;
    node->rx_self_sent_data_unicast = 0;
    node->rx_via_data_port = false;
    node->rx_via_data_datagrams = 0;
    node->rx_via_well_known_datagrams = 0;
#if tt_FRAG_ENABLED
    for (int i = 0; i < tt_FRAG_REASSEMBLY_SLOTS; i++) {
        node->frag_slots[i].received = 0;
        node->frag_slots[i].done = false;
    }
    node->frag_clock = 0;
    node->frag_fast_sub = NULL;
    node->frag_fast_timestamp = 0;
    node->frag_fast_entity_id = 0;
    node->frag_fast_seq_no = 0;
    node->frag_fast_length = 0;
    node->frag_fast_first_length = 0;
    node->frag_fast_cont_length = 0;
    node->frag_fast_context_id = 0;
    node->frag_fast_count = 0;
    node->frag_fast_placed = 0;
    node->frag_fast_is_native = false;
    node->frag_fast_via_data_port = false;
    node->frag_reassembled = 0;
    node->frag_abandoned = 0;
    node->frag_dropped = 0;
    node->frag_duplicate = 0;
#endif

    memset(node->tx_buffer, 0, sizeof(node->tx_buffer));
    set_tx_tail(node, sizeof(struct tt_Header));
    node->tx_size = sizeof(node->tx_buffer);
    node->tx_has_pending_update = false;
    node->flush_scheduled = false;
    node->announce_soon_scheduled = false;
    node->endpoints_changed_ns = 0;
    node->discovery_reply_tick = 0;
    node->discovery_reply_count = 0;
    memset(node->discovery_requests, 0, sizeof(node->discovery_requests));
    node->discovery_retry_scheduled = false;
    node->discovery_retry_ns = 0;
    memset(node->discovery_rtt_ns, 0, sizeof(node->discovery_rtt_ns));
    node->liveliness_check_scheduled = false;
    node->liveliness_check_ns = 0;
    memset(node->liveliness_flags, 0, sizeof(node->liveliness_flags));
    node->next_summary_ns = 0;
    node->summary_sent_ns = 0;
    node->tx_summary_alone_len = 0;
    node->reached_everyone = 0;
    node->summary_skip_armed = 0;
    node->summary_rides = 0;
    for (int i = 0; i < tt_MAX_CONTEXT_IDS / 32; i++) {
        node->reached_nodes[i] = 0;
    }

    memset(node->rx_buffer, 0, (long)tt_MAX_BUFFER_LENGTH * 2);
    node->rx_tail = 0;
    node->rx_size = tt_MAX_BUFFER_LENGTH * 2;
#if tt_SAMPLE_LENDING
    // No pool, nothing held, the socket reading into rx_buffer (landing 0), every counter zero.
    memset(&node->lend, 0, sizeof(node->lend));
#endif

    memset(node->scheduler, 0, sizeof(struct tt_TCB) * tt_MAX_SCHEDULER_LENGTH);
    node->scheduler_tail = 0;
}

// Arms node_update()'s and check_liveliness()'s first run, aligned to the next tt_CONTEXT_CYCLE boundary.
static tt_ret_t schedule_periodic_tasks(struct tt_Context* node) {
    uint64_t basetime = tt_get_ns();
    uint64_t rem = basetime % tt_CONTEXT_CYCLE;
    basetime = basetime - rem + tt_CONTEXT_CYCLE;

    if (!tt_Context_schedule(node, basetime, node_update, NULL)) {
        TT_LOG_ERROR("Cannot schedule node_update");
        tt_close(node);
        return tt_RET_OUT_OF_SCHEDULE;
    }
    node->next_summary_ns = basetime;

    // node_flush() is not armed here: nothing is waiting to be sent yet, and it is armed on demand the
    // moment something is (ensure_flush_scheduled()).

    node->liveliness_check_scheduled = false;
    arm_liveliness_check(node, basetime + tt_CONTEXT_UPDATE_INTERVAL);
    if (!node->liveliness_check_scheduled) {
        tt_close(node);
        return tt_RET_OUT_OF_SCHEDULE;
    }

    return tt_RET_OK;
}

#if tt_CONTEXT_ID_CLAIM
// ---- (g8, config.h's tt_CONTEXT_ID_CLAIM) keeping a context's id its own on the link.

// How long a context counts as new: it yields on a collision within this of its creation, and a collision that
// outlasts it is between two established contexts.
#define ID_SETTLE_NS (2 * tt_CONTEXT_UPDATE_INTERVAL)

static void set_id_bit(uint8_t* set, uint32_t id) {
    set[id / 8] |= (uint8_t)(1U << (id % 8));
}

// The id at creation: an explicit one as it is (claimed in the registry only if free there), otherwise the preferred
// id - the address's last octet - or, when another context on this host holds it, a free one.
static bool claim_initial_id(struct tt_Context* node) {
    node->hal.claimed_id = tt_CONTEXT_ID_INVALID;
    node->id_explicit = _tt_CONFIG.context_id != tt_CONTEXT_ID_INVALID;
    node->id_muted = false;
    node->id_muted_drops = 0;
    node->created_ns = tt_get_ns();
    node->collision_since_ns = 0;
    node->collision_last_ns = 0;
    memset(node->ids_seen, 0, sizeof(node->ids_seen));
    node->collision_logged_ip = 0;
    node->collision_logged_port = 0;
    int32_t preferred = node->id_explicit ? _tt_CONFIG.context_id : tt_get_node_id();
    if (preferred <= tt_CONTEXT_ID_INVALID || preferred >= tt_CONTEXT_ID_BROADCAST) {
        TT_LOG_ERROR("Invalid node id: %d", preferred);
        return false;
    }
    if (node->id_explicit) {
        // Every other id avoided: the registry records this one if it is free, and nothing else.
        uint8_t others[tt_MAX_CONTEXT_IDS / 8];
        memset(others, 0xff, sizeof(others));
        others[preferred / 8] &= (uint8_t)~(1U << (preferred % 8));
        if (tt_claim_context_id(node, (uint8_t)preferred, others, 0) != preferred) {
            TT_LOG_ERROR("Context id %d was set explicitly, but another context on this host holds it", preferred);
        }
        node->id = (uint8_t)preferred;
        return true;
    }
    uint8_t claimed = tt_claim_context_id(node, (uint8_t)preferred, NULL, 0);
    if (claimed == tt_CONTEXT_ID_INVALID) {
        TT_LOG_ERROR("No free context id: every id from 1 to 254 is held by a context on this host");
        return false;
    }
    if (claimed != preferred) {
        TT_LOG_INFO("Context id %d is held by another context on this host; this one takes %u", preferred, claimed);
    }
    node->id = claimed;
    return true;
}

// A full announce, sent at once: after a move, under the new id; for the context that keeps an id, so its peers
// replace whatever the other holder's packets left under it.
static void announce_id_now(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    node->last_modified = time;
    broadcast_goodbye(node);
}

static void announce_after_id_change(struct tt_Context* node) {
    tt_Context_unschedule(node, announce_id_now, NULL);
    (void)tt_Context_schedule(node, tt_get_ns(), announce_id_now, NULL);
}

// Moves this context to an id nobody on the link has used and no context on this host holds. No farewell goes out
// under the old id: its other holder is still there, and a farewell from that id would make peers drop it. The
// entries this context left under the old id are replaced by the other holder's next announce, which it sends as soon
// as it sees the collision.
static void move_id(struct tt_Context* node, uint32_t other_ip, uint16_t other_port) {
    uint8_t avoid[tt_MAX_CONTEXT_IDS / 8];
    _tt_memcpy(avoid, node->ids_seen, sizeof(avoid));
    set_id_bit(avoid, node->id);
    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(node, &own_ip, &own_port);
    uint8_t next = tt_claim_context_id(node, tt_CONTEXT_ID_INVALID, avoid, (uint32_t)own_port + 1U);
    if (next == tt_CONTEXT_ID_INVALID) {
        TT_LOG_ERROR("Context id %u is also held by %u.%u.%u.%u:%u and no id is free on the link: this context stops "
                     "sending",
                     node->id, (other_ip >> 24) & 0xffU, (other_ip >> 16) & 0xffU, (other_ip >> 8) & 0xffU,
                     other_ip & 0xffU, other_port);
        node->id_muted = true;
        return;
    }
    TT_LOG_WARNING("Context id %u is also held by %u.%u.%u.%u:%u: this context moves to %u", node->id,
                   (other_ip >> 24) & 0xffU, (other_ip >> 16) & 0xffU, (other_ip >> 8) & 0xffU, other_ip & 0xffU,
                   other_port, next);
    node->id = next;
    node->collision_since_ns = 0;
    announce_after_id_change(node);
}

// A packet carrying this context's id from another context: two hold one id. A context in its startup window
// yields; an established one keeps its id and announces at once. When the collision outlasts the window, both
// sides are established, and the one with the higher (address, port) moves. An explicit id never moves: each
// foreign holder is reported once.
static void handle_id_collision(struct tt_Context* node, uint32_t ip, uint16_t port) {
    if (node->id_explicit) {
        if (node->collision_logged_ip != ip || node->collision_logged_port != port) {
            node->collision_logged_ip = ip;
            node->collision_logged_port = port;
            TT_LOG_ERROR("Context id %u was set explicitly, and %u.%u.%u.%u:%u uses it too: the two cannot tell each "
                         "other's packets apart",
                         node->id, (ip >> 24) & 0xffU, (ip >> 16) & 0xffU, (ip >> 8) & 0xffU, ip & 0xffU, port);
        }
        return;
    }
    if (node->id_muted) {
        return;
    }
    uint64_t now = tt_get_ns();
    bool established = now - node->created_ns >= ID_SETTLE_NS;
    if (node->collision_since_ns == 0 || now - node->collision_last_ns > ID_SETTLE_NS) {
        node->collision_since_ns = now; // a new collision
        if (established) {
            announce_after_id_change(node);
        }
    }
    node->collision_last_ns = now;
    if (!established) {
        move_id(node, ip, port);
        return;
    }
    if (now - node->collision_since_ns >= ID_SETTLE_NS) {
        uint32_t own_ip = 0;
        uint16_t own_port = 0;
        tt_own_address(node, &own_ip, &own_port);
        if (own_ip > ip || (own_ip == ip && own_port > port)) {
            move_id(node, ip, port);
        }
    }
}
#endif

tt_ret_t tt_Context_create(struct tt_Context* node) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
#if tt_SEGMENT_ENABLED
    // A slot size the user set and this build cannot use is their configuration and their error, so it is
    // refused here rather than quietly replaced by the default - a value silently ignored reads exactly like
    // a value that had no effect, which is the shape config.h's own #ifndef guards were in until 2026-09-29.
    if (_tt_CONFIG.segment_slot_bytes != 0 && !valid_slot_bytes(_tt_CONFIG.segment_slot_bytes)) {
        TT_LOG_ERROR("Configured segment slot of %u bytes is outside [%u, %u] and cannot hold a datagram",
                     _tt_CONFIG.segment_slot_bytes, (unsigned)sizeof(struct tt_Header), (unsigned)tt_MAX_SAMPLE_LENGTH);
        return tt_RET_INVALID_ARGUMENT;
    }
#endif
    reset_node_state(node);
    // Before anything else can reach the node. Not concurrent-safe itself ("Threading", tickle.h): no
    // other thread may hold a node that is being created.
    node_init_locks(node);

    // Milestone 47 - this launch's own random entity_id base (struct tt_Context.entity_id_base's own
    // doc comment, tickle.h): tt_get_ns()'s low 32 bits, no separate RNG primitive needed - this
    // node's own launch instant already is one, and this is exactly the kind of "coarse, no
    // cryptographic requirement" randomness every other sentinel/hash choice in this file already
    // accepts (e.g. tt_hash_id() itself).
    // Before anything can send: flush_tx() addresses every datagram through the link table, so it
    // has to exist first. Idempotent, so a second node in this process costs one getifaddrs.
    tt_ret_t link_ret = resolve_links();
    if (link_ret != tt_RET_OK) {
        return link_ret;
    }

    node->entity_id_base = (uint32_t)tt_get_ns();

#if tt_CONTEXT_ID_CLAIM
    if (!claim_initial_id(node)) {
        return tt_RET_IILEGAL_NODE_ID;
    }
#else
    // _tt_CONFIG.context_id (see its own comment) skips auto-detection when set explicitly.
    node->id = (uint8_t)(_tt_CONFIG.context_id != tt_CONTEXT_ID_INVALID ? _tt_CONFIG.context_id : tt_get_node_id());
    if (node->id == tt_CONTEXT_ID_INVALID || node->id == tt_CONTEXT_ID_BROADCAST) {
        TT_LOG_ERROR("Invalid node id: %u", node->id);
        return tt_RET_IILEGAL_NODE_ID;
    }
#endif

    tt_ret_t bound = tt_bind(node);
    if (bound != tt_RET_OK) {
        TT_LOG_ERROR("Cannot bind");
        // tt_RET_UNSUPPORTED is kept: unlike a socket error it says the build and the system disagree, and retrying
        // cannot help (hal.h).
        return bound == tt_RET_UNSUPPORTED ? bound : tt_RET_IO_ERROR;
    }

    TT_LOG_INFO("Node open at %d", _tt_CONFIG.port);

    // The segment is NOT created here, though this is the first moment its name could be built. It is
    // built when something can use it: when an announce shows a peer at this same address
    // (note_same_host_peer()), or when this context first sends to itself (peer_segment()). A context
    // whose peers are all on other hosts never builds one, which is the whole point - it was paying
    // for a ring nobody could open. A failure to build is still not fatal: peers reach this context
    // over UDP, counted as unattached on their side.

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

// The same for a topic's sample, which fragmentation lets go up to tt_MAX_SAMPLE_LENGTH (config.h) - equal
// to the datagram bound unless fragmentation is compiled in. Services do not fragment and keep the
// datagram bound above.
static bool valid_sample_size(uint32_t size) {
    return size > 0 && size <= tt_MAX_SAMPLE_LENGTH;
}

// A new local Publisher or Client learns its peers from remote announces (register_subscriber_peer_on_
// publisher(), register_server_peer_on_client()) - but process_announce() skips the periodic resend of an
// announce it has already acted on, and a remote node whose endpoints do not change resends the same one
// forever. So a matching endpoint announced BEFORE this one was created was never matched to it, and never
// would be: its Publisher broadcast every sample. Seen on the rig (2026-09-26): 4 of 7 rmw_tickle ping
// runs registered no peer in 10 s and broadcast all 100 pings, the other 3 unicast from the first second.
//
// Marks every remote node's last acted-on announce as not acted on (update_reprocess[]), so its next periodic
// resend, within tt_CONTEXT_UPDATE_INTERVAL, is decoded in full and matched against the new endpoint. update_seen[] is
// left alone: this is not a first contact, and nothing is replied. A partial announce in progress is unaffected.
//
// A flag, set however many endpoints are created before that resend. Until 2026-10-09 the mark was the stored
// generation inverted, and a second Publisher or Client created before the resend inverted it back to the real
// generation: the resend then read as a duplicate and neither endpoint ever learned its peer - in rmw, a talker that
// creates two publishers after its listener's subscriptions were announced never matched them (test_loaned_messages,
// the held cases, about 1 run in 10 under load).
static void reprocess_known_announces(struct tt_Context* node) {
    for (int i = 0; i < tt_MAX_CONTEXT_IDS; i++) {
        if (node->update_seen[i]) {
            node->update_reprocess[i] = true;
        }
    }
}

// The smallest client_tag in 1..255 that no Client of service `endpoint_id` already in `node` holds, or 0 when all
// are. Clients of one service in one context share the endpoint_id; the tag is what tells their CallRequests apart
// at the server (tt_CallRequestHeader.client_tag), so it must differ among them - and only among them.
static uint8_t free_client_tag(const struct tt_Context* node, uint32_t endpoint_id) {
    uint32_t taken[8] = {0}; // a bit per tag value
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        const struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint != NULL && endpoint->kind == tt_KIND_SERVICE_CLIENT && endpoint->id == endpoint_id) {
            uint8_t tag = ((const struct tt_Client*)endpoint)->client_tag;
            taken[tag / 32] |= 1U << (tag % 32);
        }
    }
    for (uint32_t tag = 1; tag <= UINT8_MAX; tag++) {
        if ((taken[tag / 32] & (1U << (tag % 32))) == 0) {
            return (uint8_t)tag;
        }
    }
    return 0; // 255 Clients of this service already: this one shares the untold tag, as every Client once did
}

static tt_ret_t node_create_client_locked(struct tt_Context* node, struct tt_Client* client, struct tt_Service* service,
                                          const char* endpoint_name, tt_CLIENT_CALLBACK callback,
                                          struct tt_Node* owner) {
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
    client->cache = NULL;
    client->cache_storage = NULL; // inline - see client_cache_area()
    client->cache_length = 0;
    client->cache_time = 0;
    client->latency = 0;
    client->latency_var = 0;
    client->latency_backed_off = false;
    client->client_tag = free_client_tag(node, endpoint->id);
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        client->peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }

    tt_ret_t result = add_endpoint_to_node(node, endpoint, owner);
    if (result != tt_RET_OK) {
        return result;
    }

    node->last_modified = tt_get_ns();
    reprocess_known_announces(node);

    return tt_RET_OK;
}

tt_ret_t tt_Context_create_client(struct tt_Context* node, struct tt_Client* client, struct tt_Service* service,
                                  const char* endpoint_name, tt_CLIENT_CALLBACK callback) {
    struct tt_Context* locked_node = node;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = node_create_client_locked(node, client, service, endpoint_name, callback, NULL);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

static tt_ret_t node_create_server_locked(struct tt_Context* node, struct tt_Server* server, struct tt_Service* service,
                                          const char* endpoint_name, tt_SERVER_CALLBACK callback,
                                          struct tt_Node* owner) {
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
        server->cache_sent_at[i] = 0;
        server->cache_client_entity[i] = 0;
        server->cache_client_tag[i] = 0;
        server->slot_state[i] = tt_SERVER_SLOT_EMPTY;
        server->pending_timeout_scheduled[i] = false;
    }
    server->cache_storage = NULL; // inline - see server_cache_entry()
    server->cache_entry_length = 0;
    server->client_retry_gap = 0;

    tt_ret_t result = add_endpoint_to_node(node, endpoint, owner);
    if (result != tt_RET_OK) {
        return result;
    }

    node->last_modified = tt_get_ns();

    return tt_RET_OK;
}

tt_ret_t tt_Context_create_server(struct tt_Context* node, struct tt_Server* server, struct tt_Service* service,
                                  const char* endpoint_name, tt_SERVER_CALLBACK callback) {
    struct tt_Context* locked_node = node;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = node_create_server_locked(node, server, service, endpoint_name, callback, NULL);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

// Caller storage must hold aligned structs: a tt_SubmessageHeader in a cache entry.
static bool storage_aligned(const uint8_t* storage, uint32_t entry_length) {
    return ((uintptr_t)storage % 8U) == 0 && (entry_length % 8U) == 0;
}

static tt_ret_t server_set_storage_locked(struct tt_Server* server, uint8_t* cache_storage,
                                          uint32_t cache_entry_length) {
    if (server == NULL || server->service == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    // A cache entry must at least hold the smallest response (headers, empty body).
    const uint32_t smallest_response = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallResponseHeader);
    if (cache_storage != NULL &&
        (cache_entry_length < smallest_response || !storage_aligned(cache_storage, cache_entry_length))) {
        return tt_RET_INVALID_ARGUMENT;
    }
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (server->cache[i] != NULL) {
            return tt_RET_ILLEGAL_STATUS; // an entry already lives in the storage being replaced
        }
    }
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        server->cache_sent_at[i] = 0; // an expired entry's key lived in the old storage
    }
    server->cache_storage = cache_storage; // NULL = inline, see server_cache_entry()
    server->cache_entry_length = cache_storage != NULL ? cache_entry_length : 0;
    return tt_RET_OK;
}

tt_ret_t tt_Server_set_storage(struct tt_Server* server, uint8_t* cache_storage, uint32_t cache_entry_length) {
    struct tt_Context* locked_node = server != NULL ? server->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = server_set_storage_locked(server, cache_storage, cache_entry_length);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

static tt_ret_t client_set_storage_locked(struct tt_Client* client, uint8_t* cache_storage, uint32_t cache_length) {
    if (client == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (cache_storage != NULL && !storage_aligned(cache_storage, cache_length)) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (client->cache != NULL) {
        return tt_RET_ILLEGAL_STATUS; // a call is outstanding in the storage being replaced
    }
    client->cache_storage = cache_storage; // NULL = inline, see client_cache_area()
    client->cache_length = cache_storage != NULL ? cache_length : 0;
    return tt_RET_OK;
}

tt_ret_t tt_Client_set_storage(struct tt_Client* client, uint8_t* cache_storage, uint32_t cache_length) {
    struct tt_Context* locked_node = client != NULL ? client->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = client_set_storage_locked(client, cache_storage, cache_length);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

static tt_ret_t node_create_publisher_locked(struct tt_Context* node, struct tt_Publisher* pub, struct tt_Topic* topic,
                                             const char* endpoint_name, struct tt_Node* owner) {
    if (node == NULL || pub == NULL || topic == NULL || endpoint_name == NULL || topic->name == NULL ||
        !valid_sample_size(topic->data_size) || topic->data_encode_size == NULL || topic->data_encode == NULL) {
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
    pub->heartbeat_piggyback_every = 0; // no piggybacked Heartbeat by default - see its own doc comment
    pub->heartbeat_piggyback_count = 0;
    pub->retransmitted = 0;
    pub->ack_solicit_period_ns = 0;     // no periodic ACK solicitation by default - see its own doc comment
    pub->ack_solicit_watermark_pct = 0; // no watermark-triggered solicitation either (Phase 3 (d))
    pub->last_ack_solicit_ns = 0;
    pub->ack_solicit_outstanding = false;
    pub->ack_solicit_seq_no = 0;
    pub->resolicit_armed = false;
    // Every field an announce or the reliability path reads, not only the ones above: these
    // used to be left as found, and a caller whose struct was not already zero (a stack or reused
    // allocation) announced whatever QoS bits the garbage made, and could start with ack slots that
    // looked occupied. Found 2026-09-24 by UBSan ("load of value 69 ... for type '_Bool'").
    pub->keep_all = false;
    pub->liveliness_manual = false;
    pub->lifespan_duration_ns = 0;
    pub->deadline_duration_ns = 0;
    pub->liveliness_lease_duration_ns = 0;
    pub->liveliness_asserted_ns = 0;
    pub->writable_callback = NULL;
    pub->writable_callback_param = NULL;
    pub->writable_pending = false;
    pub->blocked_record_bytes = 0;
    pub->blocked_datagrams = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        pub->peer_acks[i].context_id = tt_CONTEXT_ID_INVALID; // empty - see claim_peer_ack()
        pub->peer_acks[i].entity_id = 0;
        pub->peer_acks[i].ack_seq_no = 0;
        pub->peer_acks[i].tracking_words = 0;
        pub->peer_acks[i].first_owed_seq_no = 0;
        pub->peer_acks[i].match_heartbeats_left = 0;
    }
    pub->match_heartbeat_pending = false;
    for (int i = 0; i < tt_DEPARTED_ACKS; i++) {
        pub->departed_acks[i].context_id = tt_CONTEXT_ID_INVALID;
        pub->departed_acks[i].entity_id = 0;
        pub->departed_acks[i].at_ns = 0;
    }
    // The ring's cursor, which forget_peer_ack() uses as an index before it reduces it modulo tt_DEPARTED_ACKS: left
    // as found, the first departure wrote 16 bytes at departed_acks[garbage], up to 4 KB past the array - into this
    // Publisher's own fields after it (departed_next = 8 overwrites batch and keep_all) or past it. Found 2026-10-08
    // by MemorySanitizer, after CI's gcc 13 left an 8 there on the stack of a test and a liveliness assertion sent
    // one datagram too many (tests/test_create_defaults.c, test_peer_discovery.c).
    pub->departed_next = 0;
    pub->keep_all_unmatched_until_ns = 0; // 0 = the pre-match window not yet opened (keep_all_room_before_match())
    pub->cache_grow = NULL;               // the caller's hook, set after creation (rmw_tickle's g10)

    tt_ret_t result = add_endpoint_to_node(node, endpoint, owner);
    if (result != tt_RET_OK) {
        return result;
    }

    node->last_modified = tt_get_ns();
    reprocess_known_announces(node);

    return tt_RET_OK;
}

tt_ret_t tt_Context_create_publisher(struct tt_Context* node, struct tt_Publisher* pub, struct tt_Topic* topic,
                                     const char* endpoint_name) {
    struct tt_Context* locked_node = node;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = node_create_publisher_locked(node, pub, topic, endpoint_name, NULL);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

static tt_ret_t node_create_subscriber_locked(struct tt_Context* node, struct tt_Subscriber* sub,
                                              struct tt_Topic* topic, const char* endpoint_name,
                                              tt_SUBSCRIBER_CALLBACK callback, struct tt_Node* owner) {
    if (node == NULL || sub == NULL || topic == NULL || endpoint_name == NULL || callback == NULL ||
        topic->name == NULL || !valid_sample_size(topic->data_size) || topic->data_decode == NULL ||
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
    // As for tt_Context_create_publisher(): everything an announce or the receive path reads. The
    // tracking pair matters most - tickle.h documents NULL/0 as this function's default, meaning
    // "use builtin_tracking[]", and a caller that did not zero its struct otherwise handed the
    // reliable path a garbage pointer to write through.
    sub->durable = false;
    sub->liveliness_manual = false;
    sub->deadline_duration_ns = 0;
    sub->liveliness_lease_duration_ns = 0;
    sub->tracking_bitmaps = NULL;
    sub->tracking_words = 0;
    sub->seq_no = 0;
    sub->accept_callback = NULL; // g13 - accept everything, which is what every caller did before it existed
    sub->accept_callback_param = NULL;
    sub->keep_last_depth = 0; // never skip: what every caller written before the segment drain could skip expects
    sub->superseded = 0;
    sub->superseded_pending = 0;
    sub->delivering_superseded = 0;
    sub->accept_declines = 0;
    sub->rxo_drops = 0;
    sub->delivered = 0;
    sub->writer_switches = 0;
    sub->out_of_order = 0;
    sub->timestamp_not_newer = 0;
    sub->via_socket_flips = 0;
    sub->out_of_order_discarded = 0;
    sub->reorder_storage = NULL;
    sub->reorder_slots = 0;
    sub->reorder_slot_bytes = 0;
    sub->reorder_held = 0;
    sub->reorder_held_peak = 0;
    sub->reorder_delivered = 0;
    sub->reorder_overflow = 0;
    sub->gap_abandoned = 0;
    sub->gap_evicted = 0;
    sub->reorder_abandoned = 0;
    sub->last_seq_no = 0;
    sub->last_source = 0;
    sub->last_entity_id = 0;
    sub->last_timestamp = 0;
    sub->last_via_data_port = false;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].context_id = tt_CONTEXT_ID_INVALID; // all empty - see struct tt_WriterProxy
    }

    tt_ret_t result = add_endpoint_to_node(node, endpoint, owner);
    if (result != tt_RET_OK) {
        return result;
    }

    node->last_modified = tt_get_ns();

    return tt_RET_OK;
}

tt_ret_t tt_Context_create_subscriber(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_Topic* topic,
                                      const char* endpoint_name, tt_SUBSCRIBER_CALLBACK callback) {
    struct tt_Context* locked_node = node;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = node_create_subscriber_locked(node, sub, topic, endpoint_name, callback, NULL);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

// ---- Nodes (CONTEXT_NODE_PLAN.md stage 2) - see struct tt_Node (tickle.h).

// A node's announce entry (struct tt_Node.entry, stage 3): its identity on the wire, set as it comes into use - its
// own entity id from the context's sequence, as an endpoint's is - and the announce armed, as for an endpoint.
static void node_entry_init(struct tt_Context* context, struct tt_Node* node) {
    node->entry.kind = tt_KIND_NODE;
    node->entry.node_index = node->index;
    node->entry.id = tt_hash_id(node->namespace_name, node->name);
    node->entry.name = node->name;
    node->entry.entity_id = context->entity_id_base + context->next_entity_id++;
    if (node->entry.entity_id == tt_DISCOVERY_ENTITY_ID) {
        node->entry.entity_id = context->entity_id_base + context->next_entity_id++;
    }
    context->last_modified = tt_get_ns();
    arm_announce_soon(context);
}

static struct tt_Node* default_node_locked(struct tt_Context* context) {
    if (context == NULL) {
        return NULL;
    }
    struct tt_Node* node = &context->default_node;
    if (context->nodes[0] != NULL && context->nodes[0] != node) {
        return NULL; // an explicit node took index 0, which a context without a default node may do
    }
    if (node->context == NULL) {
        (void)snprintf(context->default_node_name, sizeof(context->default_node_name), "tickle_%u",
                       (unsigned)context->id);
        node->name = context->default_node_name;
        node->namespace_name = "/";
        node->index = 0;
        node->context = context;
        context->nodes[0] = node;
        node_entry_init(context, node);
    }
    return node;
}

struct tt_Node* tt_Context_default_node(struct tt_Context* context) {
    if (context == NULL) {
        return NULL;
    }
    state_lock(context);
    struct tt_Node* node = default_node_locked(context);
    state_unlock(context);
    return node;
}

static tt_ret_t node_register_locked(struct tt_Context* context, struct tt_Node* node, const char* name,
                                     const char* namespace_name) {
    if (node->context != NULL) {
        return tt_RET_ILLEGAL_STATUS;
    }
    // Index 0 is the default node's, so it is taken last - and only while the default node is not in use: a context
    // whose endpoints are all on its own nodes, as rmw_tickle's are, can host tt_MAX_NODES of them.
    for (uint32_t i = 1; i <= tt_MAX_NODES; i++) {
        uint32_t index = i % tt_MAX_NODES;
        if (context->nodes[index] == NULL) {
            node->context = context;
            node->name = name;
            node->namespace_name = namespace_name;
            node->index = (uint8_t)index;
            context->nodes[index] = node;
            node_entry_init(context, node);
            return tt_RET_OK;
        }
    }
    return tt_RET_OUT_OF_BUFFER;
}

// A function, not an inline comparison: at tt_MAX_NODES 256 a uint8_t index is always in range, and the comparison
// written against the uint8_t would draw -Wtype-limits.
static bool node_index_in_range(uint32_t index) {
    return index < tt_MAX_NODES;
}

struct tt_Node* tt_Endpoint_node(const struct tt_Context* context, const struct tt_Endpoint* endpoint) {
    if (context == NULL || endpoint == NULL || !node_index_in_range(endpoint->node_index)) {
        return NULL;
    }
    return context->nodes[endpoint->node_index];
}

tt_ret_t tt_Node_create(struct tt_Context* context, struct tt_Node* node, const char* name,
                        const char* namespace_name) {
    if (context == NULL || node == NULL || name == NULL || namespace_name == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    state_lock(context);
    tt_ret_t result = node_register_locked(context, node, name, namespace_name);
    state_unlock(context);
    return result;
}

tt_ret_t tt_Node_destroy(struct tt_Node* node) {
    if (node == NULL || node->context == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Context* context = node->context;
    state_lock(context);
    tt_ret_t result = tt_RET_OK;
    for (uint32_t i = 0; i < context->endpoint_count; i++) {
        if (context->endpoints[i]->node_index == node->index) {
            result = tt_RET_ILLEGAL_STATUS; // it still owns an endpoint, which would be left pointing at it
            break;
        }
    }
    if (result == tt_RET_OK) {
        context->nodes[node->index] = NULL;
        node->context = NULL;
        context->last_modified = tt_get_ns(); // it leaves the announce (stage 3)
        arm_announce_soon(context);
    }
    state_unlock(context);
    return result;
}

tt_ret_t tt_Node_create_publisher(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Topic* topic,
                                  const char* endpoint_name) {
    if (node == NULL || node->context == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    state_lock(node->context);
    tt_ret_t result = node_create_publisher_locked(node->context, pub, topic, endpoint_name, node);
    state_unlock(node->context);
    return result;
}

tt_ret_t tt_Node_create_subscriber(struct tt_Node* node, struct tt_Subscriber* sub, struct tt_Topic* topic,
                                   const char* endpoint_name, tt_SUBSCRIBER_CALLBACK callback) {
    if (node == NULL || node->context == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    state_lock(node->context);
    tt_ret_t result = node_create_subscriber_locked(node->context, sub, topic, endpoint_name, callback, node);
    state_unlock(node->context);
    return result;
}

tt_ret_t tt_Node_create_client(struct tt_Node* node, struct tt_Client* client, struct tt_Service* service,
                               const char* endpoint_name, tt_CLIENT_CALLBACK callback) {
    if (node == NULL || node->context == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    state_lock(node->context);
    tt_ret_t result = node_create_client_locked(node->context, client, service, endpoint_name, callback, node);
    state_unlock(node->context);
    return result;
}

tt_ret_t tt_Node_create_server(struct tt_Node* node, struct tt_Server* server, struct tt_Service* service,
                               const char* endpoint_name, tt_SERVER_CALLBACK callback) {
    if (node == NULL || node->context == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    state_lock(node->context);
    tt_ret_t result = node_create_server_locked(node->context, server, service, endpoint_name, callback, node);
    state_unlock(node->context);
    return result;
}

// A send that will go unicast - an immediate publish or a call request with 1..tt_UNICAST_PEER_THRESHOLD known
// peers - is sent from an empty tx_buffer: whatever is batched there, a batched announce typically, goes out
// first, as the broadcast it was going to be. It used to decide the other way: anything pending made the
// submessage join it and go by broadcast, so a sample published within ~1 ms of an announce was broadcast
// although its peers were known - about one sample a run in a 10 ms ping-pong, and more once endpoints
// announce as soon as they are created (2026-09-26). The pending datagram only leaves up to one
// tt_CONTEXT_TX_INTERVAL early. peers is the sender's own peer table (tt_Publisher.peers, tt_Client.peers).
static void flush_pending_before_unicast(struct tt_Context* node, const struct tt_Peer* peers) {
    if (node->tx_tail == sizeof(struct tt_Header)) {
        return;
    }
    uint8_t count = count_peers(peers);
    if (count >= 1 && count <= tt_UNICAST_PEER_THRESHOLD) {
        (void)flush_tx(node, node->tx_tail, NULL, 0); // a failure logs; the send then broadcasts as before
    }
}

// Re-sends the still-outstanding call request verbatim. Failing to encode/flush isn't fatal here
// - the retry timer armed by the caller will just try again.
static void resend_call_request(struct tt_Context* node, struct tt_Client* client,
                                struct tt_SubmessageHeader* submessage_header) {
    flush_pending_before_unicast(node, client->peers);
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

// G, the one absolute term of both retry timers' RFC 6298 form, srtt + max(G, 4 x rttvar) (config.h, DESIGN.md 6
// and 7): how late this host runs a timer. Measured since 2026-10-08 rather than assumed (it was a fixed 100 us).
//
// A sample is one timed wait in tt_Context_poll() for a retry timer (is_retry_timer()) that ran to its deadline: the
// clock on waking minus that deadline (poll_wait_io()). A wait that ended early - a datagram, a wake, a signal - says
// nothing about the timer and is not one. Nor is a wait for any other deadline, because how late a wait ends depends
// on how long it was: Linux lets poll()-family timeouts end up to max(50 us, 0.1% of the timeout) late (its timer
// slack), so the context's own 500 ms announce and budget waits return ~520 us late on the dev PC where 1 ms waits
// return ~60 us late, and they made G ~1.2 ms; and the sub-microsecond waits of a busy loop return ~0.5 us "late" -
// the system call's cost - and drove G to ~1 us within three seconds of traffic (2026-10-08, c5 shape in netns). A
// retry timer's own waits are the lateness the retry suffers, whatever the platform does with the others.
//
// Of those, a wait that came back later than it was long is not one either. Under traffic the loop's wait for a
// retry is mostly a fraction of a microsecond, which ends before the thread sleeps, and its ~0.5 us is the system
// call's cost: those samples held G at 1-3 us throughout the c5 and c6 shapes, against ~60 us for the same timer
// when the thread sleeps for it, and cost +0.03-0.07% wire bytes a sample (PC, 5 reps, t 5-8). A wait preempted
// for longer than it lasted (the 2-3 ms p99.9 outliers seen on the dev PC) is passed over by the same rule. The samples
// are folded as RFC 6298 folds round trips (rtt_estimate_fold(): mean and mean deviation, gains 1/8 and 1/4), and G is
// mean + 4 x deviation: the lateness a timer seldom exceeds, not the lateness it reaches half the time, which is what
// the retry needs, as srtt + 4 x rttvar is for the round trip. Its floor is the finest step a wait can end on
// (tt_timer_resolution_ns()), so a mock clock or an exact timer cannot drive it to 0. A wait cut short by a signal
// after its deadline is a genuine late wake and counts.
static void call_retry(struct tt_Context* node, uint64_t time, void* param);
static struct tt_Client* find_calling_client(struct tt_Context* node, uint32_t endpoint_id, uint16_t seq_no);

// The timers G is the granularity of: the reliable reader's ACKNACK retry and the client's call retry.
static bool is_retry_timer(void (*function)(struct tt_Context* node, uint64_t time, void* param)) {
    return function == acknack_retry || function == call_retry;
}

static void timer_lateness_fold(struct tt_Context* node, uint64_t lateness_ns) {
    rtt_estimate_fold(&node->timer_lateness_mean_ns, &node->timer_lateness_var_ns, lateness_ns);
    uint64_t granularity = (uint64_t)node->timer_lateness_mean_ns + (4ULL * node->timer_lateness_var_ns);
    uint64_t least = node->timer_resolution_ns != 0 ? node->timer_resolution_ns : 1U;
    if (granularity < least) {
        granularity = least;
    }
    __atomic_store_n(&node->timer_lateness_ns, granularity > UINT32_MAX ? UINT32_MAX : (uint32_t)granularity,
                     __ATOMIC_RELAXED);
}

// The measured G, or tt_TIMER_LATENESS_INITIAL - the constant it replaced - until the first sample (or with no
// context to ask, as a test's bare proxy has none).
static uint64_t timer_lateness_ns(const struct tt_Context* node) {
    uint32_t measured = node != NULL ? __atomic_load_n(&node->timer_lateness_ns, __ATOMIC_RELAXED) : 0U;
    return measured != 0 ? (uint64_t)measured : (uint64_t)tt_TIMER_LATENESS_INITIAL;
}

// The reliable retry's G: a fixed tt_RELIABLE_RETRY_GRANULARITY if the build sets one, else the measured lateness.
// The measured path takes the setting as a parameter for the same reason retry_interval_for() does: a test can reach
// it in a build whose default is fixed.
static uint64_t granularity_for(uint64_t configured, const struct tt_Context* node) {
    return configured != 0 ? configured : timer_lateness_ns(node);
}

static uint64_t reliable_retry_granularity(const struct tt_Context* node) {
    return granularity_for((uint64_t)tt_RELIABLE_RETRY_GRANULARITY, node);
}

// A call's G: a fixed tt_CALL_RETRY_GRANULARITY, else twice the reliable retry's - two late-running events in a
// call's round trip (config.h), the server's stood in for by this host's own.
static uint64_t call_retry_granularity(const struct tt_Context* node) {
    uint64_t configured = (uint64_t)tt_CALL_RETRY_GRANULARITY;
    return configured != 0 ? configured : 2U * reliable_retry_granularity(node);
}

// The sum of a call's waits: `waits` of them, the first `first` and each twice the one before, none above `ceiling`.
// Saturates rather than wraps. The client's whole retry schedule - and, for a server, how long that client may still
// be retrying (server_client_window()).
static uint64_t call_retry_window(uint64_t first, uint64_t ceiling, uint32_t waits) {
    uint64_t total = 0;
    uint64_t wait = first < ceiling ? first : ceiling;
    for (uint32_t k = 0; k < waits; k++) {
        if (wait >= ceiling) { // every wait from here on is the ceiling
            uint64_t left = (uint64_t)(waits - k);
            return ceiling != 0 && left > (UINT64_MAX - total) / ceiling ? UINT64_MAX : total + (left * ceiling);
        }
        total += wait;
        wait = wait > ceiling / 2 ? ceiling : wait * 2;
    }
    return total;
}

// The first wait of a call on the auto path, and the ceiling of each: srtt + max(G, 4 * rttvar) and
// tt_CALL_RETRY_MAX_SRTT_MULTIPLE * max(srtt, G), with the seed tt_CALL_RETRY_INTERVAL standing in for srtt until a
// first answer. See config.h for why the bounds are relative to srtt and why the one absolute term remains.
static void call_retry_bounds(const struct tt_Client* client, uint64_t* first, uint64_t* ceiling) {
    if (client->latency == 0) {
        *first = (uint64_t)tt_CALL_RETRY_INTERVAL;
        *ceiling = (uint64_t)tt_CALL_RETRY_INTERVAL * tt_CALL_RETRY_MAX_SRTT_MULTIPLE;
        return;
    }
    uint64_t srtt = client->latency;
    uint64_t spread = 4ULL * client->latency_var;
    uint64_t granularity = call_retry_granularity(client->node);
    if (spread < granularity) {
        spread = granularity;
    }
    *first = srtt + spread;
    // A multiple of srtt or of G, whichever is larger. Of srtt alone, a same-host srtt of 1-2 us put the ceiling at
    // 64-128 us, below G itself: every wait was cut to it, the doubling never happened, and a call gave up within
    // ~0.5 ms - one scheduler hiccup (2026-10-09). The ceiling is there to bound a pathological variance term and a
    // large retry count, not to undo the G the first wait is guaranteed.
    uint64_t unit = srtt > granularity ? srtt : granularity;
    *ceiling = unit * tt_CALL_RETRY_MAX_SRTT_MULTIPLE;
}

// The wait after a call's `retry`-th send (0: the call itself). An explicit call_retry_interval is used as given, every
// time. The auto path (0) doubles the first wait at each retry, up to the ceiling (call_retry_bounds()).
static uint64_t compute_retry_interval(const struct tt_Client* client, uint32_t retry) {
    if (client->service->call_retry_interval != 0) {
        return client->service->call_retry_interval;
    }
    uint64_t first = 0;
    uint64_t ceiling = 0;
    call_retry_bounds(client, &first, &ceiling);
    uint64_t wait = first < ceiling ? first : ceiling;
    for (uint32_t k = 0; k < retry && wait < ceiling; k++) {
        wait = wait > ceiling / 2 ? ceiling : wait * 2;
    }
    return wait;
}

static uint32_t call_retry_count(const struct tt_Client* client) {
    // 0 means tt_CALL_RETRY_COUNT, as struct tt_Service documents. It used to be compared as given,
    // so 0 - what every caller in the tree passes, core's own examples included, and what a
    // zero-allocated rmw_tickle client holds - meant no retries at all: the call was abandoned at
    // its first retry interval (~7.5 ms), and a response arriving after that was ignored as
    // "CallResponse with no outstanding call". Found 2026-09-24 by the first rmw test to make a real
    // service call, where the deferred response took longer than that to be sent.
    return client->service->call_retry_count != 0 ? client->service->call_retry_count : (uint32_t)tt_CALL_RETRY_COUNT;
}

// How long after it was made an auto-path call reports failure at the latest: (count + 1) x tt_CALL_DEADLINE_PER_SEND,
// the worst case the old fixed 250 ms ceiling gave (config.h). An explicit call_retry_interval has no deadline: its
// (count + 1) waits are used as given, as before.
static uint64_t call_deadline(const struct tt_Client* client) {
    if (client->service->call_retry_interval != 0) {
        return UINT64_MAX;
    }
    uint64_t sends = (uint64_t)call_retry_count(client) + 1;
    return sends > UINT64_MAX / (uint64_t)tt_CALL_DEADLINE_PER_SEND ? UINT64_MAX
                                                                    : sends * (uint64_t)tt_CALL_DEADLINE_PER_SEND;
}

// When the wait after the call's `retry`-th send, starting at `now`, ends: the retry interval, cut short at the
// call's deadline.
static uint64_t call_next_wake(const struct tt_Client* client, uint32_t retry, uint64_t now) {
    uint64_t deadline = call_deadline(client);
    uint64_t deadline_at = deadline > UINT64_MAX - client->cache_time ? UINT64_MAX : client->cache_time + deadline;
    uint64_t wait = compute_retry_interval(client, retry);
    uint64_t wake = wait > UINT64_MAX - now ? UINT64_MAX : now + wait;
    return wake < deadline_at ? wake : deadline_at;
}

static bool call_past_deadline(const struct tt_Client* client, uint64_t now) {
    uint64_t deadline = call_deadline(client);
    return deadline != UINT64_MAX && now >= client->cache_time && now - client->cache_time >= deadline;
}

// A call on the auto path timed out: double the latency estimate (from the seed if there is none yet), so the next
// call waits longer (Karn's / TCP's RTO backoff). It stops at the call's deadline: no wait can be longer than that,
// so growing the estimate past it would change nothing but how far back down an answer has to bring it. An estimate
// already above it, measured, is kept. The estimate is otherwise learnt only from accepted answers, so without this a
// server that became slower than the budget timed out every call from then on. The next accepted answer replaces the
// estimate outright (latency_backed_off), so a server that is fast again gets its budget back at once.
static void back_off_retry_interval(struct tt_Client* client) {
    if (client->service->call_retry_interval != 0) {
        return;
    }
    uint64_t limit = call_deadline(client);
    limit = limit > UINT32_MAX ? UINT32_MAX : limit;
    uint64_t latency = client->latency == 0 ? (uint64_t)tt_CALL_RETRY_INTERVAL : client->latency;
    if (latency < limit) {
        latency *= 2;
        latency = latency > limit ? limit : latency;
    }
    client->latency = (uint32_t)latency;
    client->latency_backed_off = true;
}

static void call_retry(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(time);

    struct tt_Client* client = param;

    struct tt_SubmessageHeader* submessage_header = client->cache;
    if (submessage_header == NULL) {
        // Already respond.
        return;
    }

    struct tt_CallRequestHeader* callrequest_header =
        (struct tt_CallRequestHeader*)((void*)submessage_header + sizeof(struct tt_SubmessageHeader));

    uint64_t now = tt_get_ns();
    if (++callrequest_header->retry > call_retry_count(client) || call_past_deadline(client, now)) {
        back_off_retry_interval(client);
        client->callback(client, tt_CALL_TIMEOUT, NULL); // every retry went unanswered, or the deadline passed

        client->cache = NULL;
        return;
    }

    resend_call_request(node, client, submessage_header);

    if (!tt_Context_schedule(node, call_next_wake(client, callrequest_header->retry, now), call_retry, client)) {
        TT_LOG_ERROR("Cannot schedule call_retry");
        client->callback(client, tt_CALL_TIMEOUT, NULL); // can't arm another retry - treat as no answer

        client->cache = NULL;
    }
}

static tt_ret_t client_call_locked(struct tt_Client* client, struct tt_Request* request) {
    if (client == NULL || request == NULL || client->node == NULL || client->service == NULL ||
        client->service->request_encode_size == NULL || client->service->request_encode == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    if (client->cache != NULL) {
        return tt_RET_ILLEGAL_STATUS; // waiting response
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)client;
    struct tt_Context* node = client->node;
    flush_pending_before_unicast(node, client->peers);
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

    // The context's counter, not the client's (struct tt_Context.call_seq_no), past any value another Client of this
    // service here still has outstanding: the counter comes round after 65,536 calls, and an answer reaches its Client
    // by seq_no alone (find_calling_client() - a CallResponse has no room for the client_tag), so two outstanding
    // calls of one service in one context must never share one. At most one step per such Client.
    uint16_t seq_no = node->call_seq_no;
    while (find_calling_client(node, endpoint->id, seq_no) != NULL) {
        seq_no++;
    }
    callrequest_header->endpoint_id = endpoint->id;
    callrequest_header->seq_no = seq_no;
    callrequest_header->retry = 0;
    callrequest_header->client_tag = client->client_tag; // which of the context's Clients of this service

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
    uint32_t cache_length = 0;
    uint8_t* cache_area = client_cache_area(client, &cache_length);
    if (length > cache_length) {
        // No room to keep it for a retry (tt_Client_set_storage() sized for smaller requests). Not
        // sent either: a call that cannot be retried would fail silently on the first lost packet.
        TT_LOG_ERROR("Request of %u bytes exceeds the client's %u-byte cache - not sent", (unsigned)length,
                     (unsigned)cache_length);
        rollback(node, old_tx_tail);
        return tt_RET_OUT_OF_BUFFER;
    }
    struct tt_SubmessageHeader* cache = (struct tt_SubmessageHeader*)cache_area;
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
    // The round trip starts here, before the send, not when the send returns: a send that wakes a server on the same
    // CPU is preempted by it, and the answer can be waiting before the send system call is back. Read after it, that
    // call measured a round trip of ~200 ns, srtt collapsed below any real one, and the retry schedule built on it
    // ended inside the next ordinary round trip - 306 timeouts in 1,000,000 calls (service_window_a, 2026-10-09).
    uint64_t sent_at = tt_get_ns();
    if (!end_encode(node, submessage_header, true, unicast ? client->peers : NULL, unicast ? peer_count : 0)) {
        rollback(node, old_tx_tail);
        return tt_RET_IO_ERROR;
    }

    client->cache = cache;
    client->cache_time = sent_at;
    node->call_seq_no = (uint16_t)(seq_no + 1U);

    if (!tt_Context_schedule(node, call_next_wake(client, 0, client->cache_time), call_retry, client)) {
        TT_LOG_ERROR("Cannot schedule call_retry");
        client->cache = NULL;
        return tt_RET_OUT_OF_SCHEDULE;
    }

    return tt_RET_OK;
}

tt_ret_t tt_Client_call(struct tt_Client* client, struct tt_Request* request) {
    struct tt_Context* locked_node = client != NULL ? client->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = client_call_locked(client, request);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

static tt_ret_t client_destroy_locked(struct tt_Client* client) {
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
        tt_Context_unschedule(client->node, call_retry, client);
        client->cache = NULL;
    }

    client->node->last_modified = tt_get_ns();
    broadcast_goodbye(client->node);

    return tt_RET_OK;
}

tt_ret_t tt_Client_destroy(struct tt_Client* client) {
    struct tt_Context* locked_node = client != NULL ? client->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = client_destroy_locked(client);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

static tt_ret_t server_destroy_locked(struct tt_Server* server) {
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

tt_ret_t tt_Server_destroy(struct tt_Server* server) {
    struct tt_Context* locked_node = server != NULL ? server->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = server_destroy_locked(server);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

// A DATA's timestamp on the wire (tt_VERSION 10, WIRE_PLAN.md W2): the low 32 bits of the clock in
// microseconds - see struct tt_DataHeader.timestamp.
static uint32_t timestamp_to_wire(uint64_t time_ns) {
    return (uint32_t)(time_ns / tt_MICROSECOND);
}

// ... and back, in nanoseconds: the sender's microseconds taken as the ones nearest the receiver's clock,
// within half the 32-bit range (+-35.8 min) of it either way. 0 if that would fall before the clock's epoch.
// The receiver's clock is the one the running poll already read (tt_Context.rx_clock_ns): a clock read per
// received sample cost the v10 campaign 19-70 ns of user time per sample on the Pi (WIRE_PLAN.md 8).
// The receive path's "now": the running poll's reading (tt_Context.rx_clock_ns), which the poll refreshes when
// a wait returns and every tt_RX_CLOCK_REFRESH datagrams of a drain, or the clock itself outside a poll.
static uint64_t rx_now(const struct tt_Context* node) {
    return node->rx_clock_ns != 0 ? node->rx_clock_ns : tt_get_ns();
}

static uint64_t timestamp_from_wire(const struct tt_Context* node, uint32_t sent_us) {
    int64_t now_us = (int64_t)(rx_now(node) / tt_MICROSECOND);
    int64_t rebuilt_us = now_us + (int32_t)(sent_us - (uint32_t)now_us);
    return rebuilt_us < 0 ? 0 : (uint64_t)rebuilt_us * tt_MICROSECOND;
}

// Zero-copy standalone-packet publish: framing (Header + SubmessageHeader + DataHeader) built in
// a stack buffer, the CDR sent straight from the publisher's own memory via one sendmsg() - no
// staging copy into tx_buffer. Only reachable when tx_buffer is empty (nothing to coalesce with),
// so it makes the same broadcast-vs-unicast destination choice node_flush() would for a batched
// flush. body_len must be 4-aligned (the caller checks) so the single submessage needs no pad.
static tt_ret_t publish_zerocopy(struct tt_Publisher* pub, const uint8_t* body, uint32_t body_len) {
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    struct tt_Context* node = pub->node;

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
    data_header->timestamp = timestamp_to_wire(tt_get_ns());
    data_header->entity_id = endpoint->entity_id; // Milestone 47 - this Publisher's own identity

    uint8_t peer_count = count_peers(pub->peers);
#if tt_FRAG_ENABLED
    if (sizeof(framing) + body_len > FRAG_WHOLE_DATA_LIMIT) {
        bool unicast = peer_count >= 1 && peer_count <= tt_UNICAST_PEER_THRESHOLD;
        if (!send_fragments(node, data_header, body, body_len, unicast ? pub->peers : NULL, unicast ? peer_count : 0,
                            tt_SUBMESSAGE_ID_ALL)) {
            return tt_RET_IO_ERROR;
        }
        pub->seq_no += frag_count_for(body_len); // one seq_no per datagram
        return tt_RET_OK;
    }
#endif
    uint32_t skip = to_single_form(framing, (uint32_t)sizeof(framing), body_len);
    const uint8_t* head = framing + skip;
    uint32_t head_len = (uint32_t)sizeof(framing) - skip;
    if (peer_count >= 1 && peer_count <= tt_UNICAST_PEER_THRESHOLD) {
        note_reached(node, pub->peers, peer_count);
        for (uint8_t i = 0; i < peer_count; i++) {
            if (seam_send_iov(node, head, head_len, body, body_len, pub->peers[i].ip, pub->peers[i].port,
                              pub->peers[i].context_id) < 0) {
                return tt_RET_IO_ERROR;
            }
        }
    } else {
        if (link_count() <= 1) {
            note_reached(node, NULL, 0); // the HAL's one broadcast address; with several links it is not every link
        }
        // Broadcast: no single peer, so tt_CONTEXT_ID_INVALID and therefore UDP.
        if (seam_send_iov(node, head, head_len, body, body_len, 0, 0, tt_CONTEXT_ID_INVALID) < 0) {
            return tt_RET_IO_ERROR;
        }
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
        if (pub->peer_acks[i].context_id == tt_CONTEXT_ID_INVALID) {
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

// Defined with the rest of the cache arithmetic below; keep_all_writable() needs it here.
static bool reliable_cache_admits(const struct tt_ReliableCache* cache, uint16_t depth, uint32_t length,
                                  uint32_t acked_through);
static uint32_t keep_all_acked_through(const struct tt_Publisher* pub);

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
        if (pub->peer_acks[i].context_id != tt_CONTEXT_ID_INVALID) {
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

    uint32_t acked_through = keep_all_acked_through(pub);
    // The next sample takes one seq_no, or - one that was refused for its datagram count - that many.
    uint32_t unacked_after_this =
        pub->seq_no + (pub->blocked_datagrams > 1 ? pub->blocked_datagrams : 1) - acked_through;
    if (unacked_after_this > keep_all_bound(pub)) {
        return false;
    }
    // The byte half of the same promise (2026-09-25). The count above is not enough on its own: the
    // arena holds (depth + 1) records of whatever size it was sized for, so a Publisher whose
    // samples are larger than that fills it long before the count bound, and cache_reliable_sample()
    // would then evict an unacknowledged sample to fit. blocked_record_bytes is the record a publish
    // was already refused for (0 before any refusal), so this asks the question the caller will
    // actually ask again rather than guessing a size.
    if (pub->blocked_record_bytes != 0 &&
        !reliable_cache_admits(pub->reliable_cache, reliable_cache_depth(pub->reliable_cache),
                               pub->blocked_record_bytes, acked_through)) {
        return false;
    }
    return true;
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
static uint64_t ack_solicit_min_gap(const struct tt_Publisher* pub) {
    return pub->ack_solicit_period_ns > reliable_retry_interval_publisher() ? pub->ack_solicit_period_ns
                                                                            : reliable_retry_interval_publisher();
}

static void solicit_ack_throttled(struct tt_Publisher* pub) {
    uint64_t now = tt_get_ns();
    uint64_t min_gap = ack_solicit_min_gap(pub);
    // Self-clocked (2026-10-04): one solicitation in flight at a time, and the next one as soon as an ACKNACK
    // answers it. min_gap is then only how long an unanswered one waits before it is assumed lost and repeated.
    // A fixed gap alone set the RELIABLE ceiling at one acknowledgement per millisecond - on one host, a KEEP_ALL
    // window of 256 every 1 ms is the 241k samples/s the cell measured on two different machines alike.
    if (pub->ack_solicit_outstanding && now - pub->last_ack_solicit_ns < min_gap) {
        RSTAT_INC(ack_solicit_suppressed);
        return;
    }
    pub->ack_solicit_outstanding = true;
    pub->ack_solicit_seq_no = pub->seq_no; // where the watermark path counts the next half-window from
    pub->last_ack_solicit_ns = now;
    RSTAT_INC(ack_solicit_sent);
    (void)tt_Publisher_request_ack(pub);
}

// A refused Publisher's own retry of its solicitation (2026-10-05). The refusal asks once; if that request or its
// answer is lost, nothing else asks while the Publisher is stopped - seq_no no longer moves, so the watermark cannot
// fire, and a caller that waits for writable_callback (rmw_tickle's publish_blocking() does) never retries the
// publish that would. The cross-host A/B found exactly that at p4 under 5% loss: 4.5 s of a 5 s run spent waiting,
// -87% throughput. So the Publisher arms this itself while it is refused, as a retransmission timer for the request:
// it fires one retry interval after the last solicitation, asks again if still refused, and ends when it is not.
static void keep_all_resolicit(struct tt_Context* node, uint64_t time, void* param);

static void arm_keep_all_resolicit(struct tt_Publisher* pub) {
    if (pub->resolicit_armed) {
        return;
    }
    if (tt_Context_schedule(pub->node, pub->last_ack_solicit_ns + ack_solicit_min_gap(pub), keep_all_resolicit, pub)) {
        pub->resolicit_armed = true;
    }
}

static void keep_all_resolicit(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(node);
    UNUSED(time);
    struct tt_Publisher* pub = param;
    pub->resolicit_armed = false;
    if (!pub->writable_pending) {
        return; // writable again: there is nothing left to chase
    }
    solicit_ack_throttled(pub);
    arm_keep_all_resolicit(pub);
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
    // One solicitation per `threshold` new seq_nos (2026-10-05). The answer clocks the next one
    // (solicit_ack_throttled()), but on a link whose round trip is shorter than the retry interval that alone asks
    // more often than before: the cross-host A/B measured 1.6x the ACKNACKs per sample and +2% CPU on cells that
    // never blocked. Counting data instead of time bounds it at two per window whatever the round trip.
    if (pub->ack_solicit_seq_no != 0 && pub->seq_no - pub->ack_solicit_seq_no < threshold) {
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
static void reliable_cache_evict_one(struct tt_ReliableCache* cache, uint16_t depth);

// Whether the retained record under seq_no is a fragment continuing a sample rather than starting one.
static bool reliable_cache_record_continues(const struct tt_ReliableCache* cache, uint16_t depth, uint32_t seq_no) {
#if tt_FRAG_ENABLED
    if (seq_no == 0 || cache->oldest_seq_no == 0 || seq_no < cache->oldest_seq_no || seq_no > cache->newest_seq_no) {
        return false;
    }
    const struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, seq_no);
    return entry->len != 0 && entry->seq_no == seq_no &&
           ((const struct tt_SubmessageHeader*)(cache->arena + entry->offset))->type == tt_SUBMESSAGE_TYPE_FRAG_CONT;
#else
    UNUSED(cache);
    UNUSED(depth);
    UNUSED(seq_no);
    return false;
#endif
}

// Whether the oldest retained record is a fragment continuing a sample rather than starting one.
static bool reliable_cache_oldest_continues(const struct tt_ReliableCache* cache, uint16_t depth) {
#if tt_FRAG_ENABLED
    if (cache->oldest_seq_no == 0) {
        return false;
    }
    const struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, cache->oldest_seq_no);
    return entry->len != 0 && entry->seq_no == cache->oldest_seq_no &&
           ((const struct tt_SubmessageHeader*)(cache->arena + entry->offset))->type == tt_SUBMESSAGE_TYPE_FRAG_CONT;
#else
    UNUSED(cache);
    UNUSED(depth);
    return false;
#endif
}

// KEEP_ALL's "acknowledged through", rounded down to a whole sample (2026-10-05). Readers acknowledge datagrams, and a
// fragmented sample is several, so the acknowledgement can stop inside one; but the cache evicts whole samples
// (reliable_cache_evict_oldest()), so evicting the acknowledged first fragment takes the unacknowledged rest with it.
// The admission checks counted records and passed; the reader then heard those continuations had gone. Rig, Array4k
// (4 fragments) at 5% loss with the reader matched: gap_evicted 751-1912 per 20 s run, every logged eviction at the
// reader's ack and the one after it (experiments/rmw_keepall_evict_repro.sh). Every KEEP_ALL check that decides
// whether something may be evicted asks this instead of min_ack - 1.
static uint32_t keep_all_acked_through(const struct tt_Publisher* pub) {
    uint32_t min_ack = min_peer_ack_seq_no(pub);
    uint32_t acked_through = min_ack > 0 ? min_ack - 1 : 0; // ack_seq_no means "everything below it"
#if tt_FRAG_ENABLED
    const struct tt_ReliableCache* cache = pub->reliable_cache;
    uint16_t depth = reliable_cache_depth(cache);
    while (acked_through != 0 && cache != NULL && depth != 0 &&
           reliable_cache_record_continues(cache, depth, acked_through + 1)) {
        acked_through--; // the first unacknowledged record continues a sample: none of that sample is evictable
    }
#endif
    return acked_through;
}

// Evicts the oldest retained sample - every datagram of it. A fragmented sample is cached one record per
// datagram (each its own seq_no, DATAFRAG_PLAN.md section 13), and evicting only its first would leave
// continuations no subscriber can use under a sequence range the Heartbeat would still advertise, so the
// oldest retained record is always the first datagram of a sample.
static void reliable_cache_evict_oldest(struct tt_ReliableCache* cache, uint16_t depth) {
    reliable_cache_evict_one(cache, depth);
    while (reliable_cache_oldest_continues(cache, depth)) {
        reliable_cache_evict_one(cache, depth);
    }
}

// Whether the live record in this entry is the first of its sample - a whole DATA or a FRAG_FIRST - rather
// than a continuation. What tt_ReliableCache.retained_samples counts.
static bool reliable_cache_record_starts_sample(const struct tt_ReliableCache* cache,
                                                const struct tt_ReliableCacheIndex* entry) {
    return entry->len != 0 &&
           ((const struct tt_SubmessageHeader*)(cache->arena + entry->offset))->type != tt_SUBMESSAGE_TYPE_FRAG_CONT;
}

static void reliable_cache_evict_one(struct tt_ReliableCache* cache, uint16_t depth) {
    if (cache->oldest_seq_no == 0) {
        return;
    }
    struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, cache->oldest_seq_no);
    if (entry->seq_no == cache->oldest_seq_no) {
        if (reliable_cache_record_starts_sample(cache, entry) && cache->retained_samples > 0) {
            cache->retained_samples--;
        }
        entry->len = 0; // tombstone: the bytes are gone, the slot may still be named by an ACKNACK
    }
    if (cache->oldest_seq_no == cache->newest_seq_no) {
        cache->oldest_seq_no = 0; // nothing retained any more; newest_seq_no stays (it's "last
        cache->tail = 0;          // published", what first_resendable falls back to)
        cache->retained_samples = 0;
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
            cache->retained_samples = 0;
            return;
        }
        cache->oldest_seq_no++;
    }
}

// Where a `length`-byte record may be written right now, or UINT32_MAX when the oldest retained
// record has to be evicted first. Records are never split (struct tt_ReliableCache.arena's own doc
// comment): when the space before the end of the arena is too small, the write wraps to offset 0
// and the tail fragment is simply wasted until the ring passes it.
// How many bytes of arena the submessage now sitting at submessage_header will need. Shared with
// tt_Publisher_publish()'s KEEP_ALL admission check, so the two cannot disagree about the size of
// the record one of them is deciding about and the other is writing.
// Bytes of cache arena one encoded sample occupies: its raw length rounded to end_encode()'s 4-byte
// submessage alignment, since a retransmission is cut from the cached record and must match what was sent.
//
// The raw length is passed in rather than derived from node->tx_tail, which is what this did until
// 2026-10-02. "The distance from this submessage to tx_buffer's current end" is only the sample's length
// while the sample is the last thing in tx_buffer - true today and the first thing SHM_PLAN 6e breaks, since
// its encoder writes into a segment slot where that subtraction is between two unrelated addresses. The
// equivalence was checked before the derivation was removed: a temporary assert compared the two on every
// path the suite walks, 44 tests green, and a mutant four bytes out failed it in test_data_frag.
static uint32_t record_length_checked(const struct tt_Context* node,
                                      const struct tt_SubmessageHeader* submessage_header, uint32_t raw_len) {
    (void)node;
    (void)submessage_header;
    return (uint32_t)ROUNDUP(raw_len);
}

// Where a length-byte record goes in an arena whose live records occupy [head, tail) - or
// UINT32_MAX when something has to be evicted first. Takes the two fields eviction moves as plain
// arguments rather than reading the cache, so reliable_cache_admits() below can ask the same
// question about a state it only simulates. One definition on purpose: the byte bound and the count
// bound already disagreed once (2026-09-25) because they were decided in two different places.
static uint32_t reliable_cache_offset_in(uint32_t arena_size, bool empty, uint32_t head, uint32_t tail,
                                         uint32_t length) {
    if (empty) {
        return 0; // the whole arena is free (callers reject length > arena_size before asking)
    }
    if (tail == head) {
        return UINT32_MAX; // the live records fill the arena exactly - evict before anything fits
                           // (tail == head reads as "empty" everywhere else, hence this first)
    }
    if (tail > head) { // live bytes are one contiguous [head, tail) run
        if (length <= arena_size - tail) {
            return tail;
        }
        return length <= head ? 0 : UINT32_MAX; // wrap to the front if the free head fragment fits
    }
    return length <= head - tail ? tail : UINT32_MAX; // live run wraps: free is [tail, head)
}

static uint32_t reliable_cache_write_offset(const struct tt_ReliableCache* cache, uint16_t depth, uint32_t length) {
    bool empty = cache->oldest_seq_no == 0;
    uint32_t head = empty ? 0 : reliable_cache_slot(cache, depth, cache->oldest_seq_no)->offset;
    return reliable_cache_offset_in(cache->arena_size, empty, head, cache->tail, length);
}

// Whether a length-byte record can be cached without evicting a sample that nobody has acknowledged
// yet - the question KEEP_ALL's promise actually turns on, and the half keep_all_writable() used to
// miss (2026-09-25): it bounds the unacknowledged run by COUNT while the arena bounds it by BYTES,
// and cache_reliable_sample() evicts to make room without knowing the policy, because it takes the
// cache and not the Publisher.
//
// Mirrors that function's own "ask for an offset, evict the oldest, ask again" loop against a copy
// of the two fields eviction moves, so it answers about the arithmetic the real write will use.
static bool reliable_cache_admits(const struct tt_ReliableCache* cache, uint16_t depth, uint32_t length,
                                  uint32_t acked_through) {
    if (length > cache->arena_size) {
        return true; // B1: a sample this large is sent without being cached at all, so it evicts
                     // nothing - a different loss (it can never be retransmitted), reported by
                     // not_cached_oversize, and not one refusing the publish forever would fix
    }
    uint32_t oldest = cache->oldest_seq_no;
    uint32_t tail = cache->tail;
    for (;;) {
        if (oldest == 0) {
            return true; // nothing retained: the whole arena is free
        }
        uint32_t head = reliable_cache_slot(cache, depth, oldest)->offset;
        if (reliable_cache_offset_in(cache->arena_size, false, head, tail, length) != UINT32_MAX) {
            return true;
        }
        if (oldest > acked_through) {
            return false; // the next eviction would take a sample nobody has acknowledged
        }
        // What reliable_cache_evict_oldest() would do next, without doing it.
        if (oldest == cache->newest_seq_no) {
            oldest = 0;
            tail = 0;
            continue;
        }
        oldest++;
        while (oldest != 0 && !reliable_cache_slot_live(cache, depth, oldest)) {
            if (oldest == cache->newest_seq_no) {
                oldest = 0;
                tail = 0;
                break;
            }
            oldest++;
        }
    }
}

// "cache first, then flush" order tt_Client_call() already uses for its own single-slot cache.
// Both QoS policies share this one cache (struct tt_ReliableCache's own doc comment, tickle.h): a
// later incoming ACKNACK (process_acknack()) resends this exact copy verbatim, and a newly-
// discovered Subscriber (deliver_durability_backlog()) gets every currently-retained entry pushed
// straight to it, whichever of the two (or both) this Publisher opted into. KEEP_LAST eviction
// once `depth` slots are full (mirrors rmw_subscription.c's own queue eviction), not an error.
// Split out of tt_Publisher_publish() below purely to keep that function's own cognitive
// complexity under clang-tidy's threshold.
//
// reliable_cache_reserve() is the cache half: room for one record of `length` bytes under seq_no, or
// NULL when there is none to be had, the slot then left a tombstone. length 0 asks for the tombstone
// outright, for a record the caller has decided not to keep. The caller writes the bytes.
static uint8_t* reliable_cache_reserve(struct tt_ReliableCache* cache, uint32_t seq_no, uint32_t length) {
    uint16_t depth = reliable_cache_depth(cache);
    if (depth == 0) {
        return NULL; // index[]/capacity/arena never set up (struct tt_ReliableCache's own doc comment) -
                     // nothing to cache into, same safe no-op every other clamp site below shares
    }
    // Records start 4-aligned in the arena; a fragment's own length is not a multiple of 4 and is kept
    // exact in entry->len, since a fragment is resent unpadded.
    uint32_t footprint = ROUNDUP(length);

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

    if (length == 0) {
        reliable_cache_drop_leading_tombstones(cache, depth);
        return NULL;
    }
    if (footprint > cache->arena_size) {
        // B1 - this one sample can never fit, however much is evicted: publish it (the caller
        // already encoded it into tx_buffer) but retain nothing, and leave the rest of the cache
        // alone rather than evicting for room that could never exist. The slot stays a tombstone,
        // so an ACKNACK naming it gets Phase 1-c's eviction Heartbeat and the Subscriber skips it
        // immediately instead of retrying; DURABILITY's backlog simply doesn't contain it.
        TT_LOG_WARNING("Reliable sample %u (%u bytes) exceeds the %u-byte cache arena - sent, not cached", seq_no,
                       length, cache->arena_size);
        RSTAT_INC(not_cached_oversize);
        reliable_cache_drop_leading_tombstones(cache, depth);
        return NULL;
    }

    // Contiguous byte room, evicting oldest-first until this record fits (the byte bound, DDS's
    // own RESOURCE_LIMITS). reliable_cache_write_offset() returns where it would go, or UINT32_MAX
    // while something still has to be evicted first.
    uint32_t offset = reliable_cache_write_offset(cache, depth, footprint);
    while (offset == UINT32_MAX) {
        RSTAT_INC(evicted_by_bytes);
        reliable_cache_evict_oldest(cache, depth);
        offset = reliable_cache_write_offset(cache, depth, footprint);
    }

    entry->offset = offset;
    entry->len = (uint16_t)length;
    cache->tail = offset + footprint;
    if (cache->oldest_seq_no == 0) {
        cache->oldest_seq_no = seq_no;
    }
    return cache->arena + offset;
}

// Makes room for one more sample under tt_ReliableCache.sample_depth, KEEP_LAST-evicting whole samples
// oldest first. Called before a sample's first record is reserved; a no-op when sample_depth is 0.
static void reliable_cache_admit_sample(struct tt_ReliableCache* cache) {
    uint16_t depth = reliable_cache_depth(cache);
    while (depth != 0 && cache->sample_depth != 0 && cache->oldest_seq_no != 0 &&
           cache->retained_samples >= cache->sample_depth) {
        RSTAT_INC(evicted_by_count);
        reliable_cache_evict_oldest(cache, depth);
    }
}

// Counts a sample whose first record reliable_cache_reserve() just placed.
static void reliable_cache_count_sample(struct tt_ReliableCache* cache, const uint8_t* first_record) {
    if (first_record != NULL) {
        cache->retained_samples++;
    }
}

static void cache_reliable_sample(struct tt_Context* node, struct tt_SubmessageHeader* submessage_header,
                                  struct tt_ReliableCache* cache, uint32_t seq_no, uint32_t encoded_len) {
    uint32_t length = record_length_checked(node, submessage_header, encoded_len);
    reliable_cache_admit_sample(cache);
    uint8_t* record = reliable_cache_reserve(cache, seq_no, length);
    if (record != NULL) {
        _tt_memcpy(record, submessage_header, length);
    }
    reliable_cache_count_sample(cache, record);
}

// QoS roadmap #6 (LIFESPAN) - see tt_Publisher.lifespan_duration_ns's own doc comment (tickle.h).
// lifespan_duration_ns == 0 means "no LIFESPAN requested" - never expired, matching every other
// disabled-by-zero convention this struct already uses (heartbeat_period_ns, etc).
static bool reliable_cache_entry_expired(const struct tt_ReliableCacheIndex* entry, uint64_t lifespan_duration_ns) {
    return lifespan_duration_ns != 0 && (tt_get_ns() - entry->timestamp) >= lifespan_duration_ns;
}

// Milestone 58 (rmw_tickle/PLAN.md) - true if durable_delivered[] already records this exact
// (node_id, announce generation) pair, i.e. this announce is a re-announce from a peer that already has
// this Publisher's current backlog, not a genuinely new match. See struct tt_DurableDeliveryRecord's
// own doc comment (tickle.h) for why the generation, not node_id alone, is the right key.
static bool durable_delivered_get(const struct tt_ReliableCache* cache, uint8_t node_id, uint32_t generation) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (cache->durable_delivered[i].context_id == node_id) {
            return cache->durable_delivered[i].generation == generation;
        }
    }
    return false;
}

// Records that node_id has now received the backlog as of its announce generation - refreshes an existing
// slot for that node_id, or claims the first empty one, mirroring upsert_peer()'s own style. A full
// table (durable_delivered_upsert() finding neither) is a safe no-op: the worst case is one
// redundant re-delivery next time, never a correctness problem (struct tt_DurableDeliveryRecord's
// own doc comment).
static void durable_delivered_upsert(struct tt_ReliableCache* cache, uint8_t node_id, uint32_t generation) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (cache->durable_delivered[i].context_id == node_id) {
            cache->durable_delivered[i].generation = generation;
            return;
        }
    }
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (cache->durable_delivered[i].context_id == tt_CONTEXT_ID_INVALID) {
            cache->durable_delivered[i].context_id = node_id;
            cache->durable_delivered[i].generation = generation;
            return;
        }
    }
}

static uint32_t reliable_cache_oldest_seq_no(struct tt_ReliableCache* cache);
static void encode_and_send_heartbeat(struct tt_Context* node, struct tt_Publisher* pub, uint32_t first_seq_no,
                                      const struct tt_Peer* peers, uint8_t peer_count, uint8_t flags);

// Whether this publish should carry a piggybacked Heartbeat (tt_Publisher.heartbeat_piggyback_every).
// Only when it flushes anyway: a batching publisher leaves the send to node_flush(), and a
// Heartbeat buried in a batch arrives no sooner than the batch does.
// The zero-copy path, taken when the topic offers it, tx_buffer is empty (nothing batched to
// coalesce with) and the message is big enough that a second one could not share the packet anyway -
// i.e. batching has nothing to gain. True when it published, leaving the result in *result; false
// when this publish must fall through to the staging copy path, having changed nothing. A reliable
// or durable Publisher always falls through: it needs the encoded bytes at a known tx_buffer
// location to retain, and this path publishes straight from the caller's own tt_Data. Split out of
// tt_Publisher_publish() to keep its cognitive complexity under clang-tidy's threshold.
static bool try_publish_zerocopy(struct tt_Publisher* pub, struct tt_Data* data, uint32_t old_tx_tail,
                                 tt_ret_t* result) {
    if (pub->topic->data_encode_inplace == NULL || old_tx_tail != sizeof(struct tt_Header) ||
        pub->reliable_cache != NULL) {
        return false;
    }
    const uint8_t* body = NULL;
    int32_t body_len = pub->topic->data_encode_inplace(data, &body);
    TT_TRACE(tt_TRACE_ENCODED);
    uint32_t standalone_len = sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) +
                              sizeof(struct tt_DataHeader) + (body_len >= 0 ? (uint32_t)body_len : 0);
    bool fills_packet =
        standalone_len + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader) > tt_MAX_BUFFER_LENGTH;
    if (body_len >= 0 && (body_len % 4) == 0 && fills_packet &&
        (!tt_FRAG_ENABLED || body_len <= tt_MAX_SAMPLE_LENGTH)) {
#if tt_LOCAL_DELIVERY
        uint32_t local_seq_no = pub->seq_no + 1;
        uint64_t local_timestamp = tt_get_ns();
#endif
        *result = publish_zerocopy(pub, body, (uint32_t)body_len);
#if tt_LOCAL_DELIVERY
        // (g9) The caller's own bytes, still valid: the zero-copy path sends from them.
        if (*result == tt_RET_OK && pub->local_subscriber_count != 0) {
            deliver_locally(pub->node, pub, body, (uint32_t)body_len, local_seq_no, local_timestamp);
        }
#endif
        return true;
    }
    return false; // declined, unaligned, or small enough to want batching
}

// How many seq_no - and datagrams - the encoded DATA submessage at submessage_header takes: 1, or one
// per fragment when no datagram can carry it (DATAFRAG_PLAN.md section 13).
// The peers this publish will unicast to, or none when it will broadcast - mirrors tt_Client_call()'s own peer
// decision. Unicast only when there is a small enough known count AND nothing else is already sitting unflushed
// ahead of this DATA submessage, since unicasting would reach these peers and not whatever else needs the whole
// segment. Extracted when 6e(a) moved this above the datagram count and publisher_publish_locked() went one over
// the cognitive-complexity gate; it is one decision and now has one name.
// Why a whole-record send was refused. One bit each in tt_Context.whole_refusals_logged, so every cause gets
// said once and no cause can hide behind another: the first version of this was a single bool, and a single
// bool reports whichever cause fires first and then stays silent for the life of the node - which, for a
// publisher that batches, would have been "batched" forever and never the segment answer being looked for.
enum tt_WholeRefusal {
    tt_WHOLE_REFUSE_BATCHED = 0,   // the publisher batches; this publish is not a flush
    tt_WHOLE_REFUSE_NO_PEERS,      // no known unicast peer - a broadcast
    tt_WHOLE_REFUSE_PEER_SPREAD,   // more destinations than tt_UNICAST_PEER_THRESHOLD
    tt_WHOLE_REFUSE_BUFFER_IN_USE, // something else is already unflushed ahead of this DATA
    tt_WHOLE_REFUSE_UNATTACHED,    // a destination has no segment of ours attached
    tt_WHOLE_REFUSE_ADDRESS,       // a destination's context id is behind a different (ip, port)
    tt_WHOLE_REFUSE_PEER_COUNT,    // the array yielded fewer live entries than count_peers() promised
    tt_WHOLE_REFUSE_SLOT_ALIGN,    // a destination's slot_bytes would put payloads off 4-alignment
};

// Says a cause once per node. Separate from the tests above so each of them stays a single line and the
// cognitive-complexity gate keeps its grip on the decision rather than on the reporting.
#if tt_SEGMENT_ENABLED
static void note_whole_refusal(struct tt_Context* node, enum tt_WholeRefusal why, const char* detail) {
    const uint16_t bit = (uint16_t)(1U << (unsigned)why);
    if ((node->whole_refusals_logged & bit) != 0) {
        return;
    }
    node->whole_refusals_logged |= bit;
    TT_LOG_INFO("Whole-record send refused: %s", detail);
}
#else
// The bitmask it writes lives inside the same guard, and with no segment there is no whole-record send to
// refuse. A no-op here rather than a guard around each of the four call sites, which is how whole_record_limit_for()
// below already handles the same split - and it keeps the callers reading as one decision.
static void note_whole_refusal(struct tt_Context* node, enum tt_WholeRefusal why, const char* detail) {
    UNUSED(node);
    UNUSED(why);
    UNUSED(detail);
}
#endif

static uint8_t unicast_destinations_for(struct tt_Context* node, const struct tt_Publisher* pub, uint32_t old_tx_tail,
                                        bool is_flush, const struct tt_Peer** out_peers) {
    *out_peers = NULL;
    if (!is_flush) {
        note_whole_refusal(node, tt_WHOLE_REFUSE_BATCHED, "this publisher batches, so the publish is not a flush");
        return 0;
    }
    uint8_t count = count_peers(pub->peers);
    if (count < 1) {
        note_whole_refusal(node, tt_WHOLE_REFUSE_NO_PEERS, "no known unicast peer, so this publish broadcasts");
        return 0;
    }
    if (count > tt_UNICAST_PEER_THRESHOLD) {
        note_whole_refusal(node, tt_WHOLE_REFUSE_PEER_SPREAD, "more destinations than the unicast threshold");
        return 0;
    }
    if (old_tx_tail != sizeof(struct tt_Header)) {
        note_whole_refusal(node, tt_WHOLE_REFUSE_BUFFER_IN_USE, "another submessage is already unflushed ahead");
        return 0;
    }
    *out_peers = pub->peers;
    return count;
}

// The largest record every destination of this send could take whole, or 0 when any of them could not.
//
// SHM_PLAN 6e(a): a sample bound only for shared memory need not be split, because a segment has no MTU. The
// conditions are all necessary and each rules out a way of producing a record with nowhere to go - an
// unfragmented record is larger than one datagram by construction, so UDP is not a fallback for it:
//
//   peer_count == 0   a broadcast, whose destinations are not known here and include every remote node.
//   not attached      we have no segment for that peer, so its datagrams take the socket.
//   a different (ip, port) behind the same context id   a different peer, whose segment we have not opened.
//   slot_bytes too small   it would be refused at the ring and fall back to the socket, oversized.
//
// A FULL ring is deliberately not among them. A full ring already drops rather than rerouting - sending that
// datagram over UDP would overtake the records already in the ring and make the reader discard everything older
// behind it, which cost CI's same-host cell 97.6% of its traffic when it was tried - so an unfragmented record
// changes only the size of what a full ring drops, not whether it has somewhere to go.
//
// The minimum across peers, because one record is built and every destination has to take it.
// Not const: it records, once per cause, why it refused. See tt_Context.whole_refusals_logged.
static uint32_t whole_record_limit_for(struct tt_Context* node, const struct tt_Peer* peers, uint8_t peer_count) {
#if tt_SEGMENT_ENABLED
    if (peers == NULL || peer_count == 0) {
        return 0; // unicast_destinations_for() already said which of its four reasons this was
    }
    uint32_t smallest = UINT32_MAX;
    uint8_t seen = 0;
    // i < peer_count, which is what link_destinations() and every other peer walk in this file do: the
    // array holds the caller's peer_count entries and nothing says it is longer. This loop originally
    // walked tt_MAX_PEER_COUNT skipping holes, which is safe for pub->peers and reads off the end of a
    // single-peer stack object - end_encode(node, hdr, true, &target, 1), the retransmit path. That path
    // only began calling this function when record_size_limit() was added, so the call sites widened and
    // the contract did not. The fuzzer found it in under a second as an ASan stack-buffer-overflow.
    for (uint8_t i = 0; i < peer_count; i++) {
        if (peers[i].context_id == tt_CONTEXT_ID_INVALID) {
            continue;
        }
        seen++;
        const struct tt_SegmentPeer* entry = segment_peer_if_live(node, peers[i].context_id);
        if (entry == NULL || entry->mapping == NULL || entry->ip != peers[i].ip || entry->port != peers[i].port) {
            // Said once per cause, because four rig campaigns were spent inferring why this refused from
            // throughput numbers that look identical whether the mechanism is absent or merely never granted.
            // An answer in the log costs one line and ends the guessing; reasoning about it cost a night.
            if (entry == NULL || entry->mapping == NULL) {
                note_whole_refusal(node, tt_WHOLE_REFUSE_UNATTACHED, "a destination has no attached segment");
            } else {
                note_whole_refusal(node, tt_WHOLE_REFUSE_ADDRESS, "a destination is at a different address");
            }
            return 0;
        }
        if ((entry->mapping->slot_bytes % 4U) != 0U) {
            // Refused before the path that would need it exists, rather than after. A record encoded
            // into a slot must land 4-aligned, as it does in tx_buffer; with both segment structs a
            // multiple of 4 (asserted at the top of this file) that is exactly slot_bytes % 4 == 0.
            // Today's whole-record send is a copy and would not care, so this costs nothing now and
            // cannot be forgotten later - the order last night's defects arrived in was always the
            // other one.
            note_whole_refusal(node, tt_WHOLE_REFUSE_SLOT_ALIGN, "a destination's slot_bytes is not a multiple of 4");
            return 0;
        }
        uint32_t usable = entry->mapping->slot_bytes;
        if (usable > (uint32_t)tt_MAX_SAMPLE_LENGTH) {
            usable = (uint32_t)tt_MAX_SAMPLE_LENGTH; // tx_buffer holds one sample of this size and no more
        }
        if (usable < smallest) {
            smallest = usable;
        }
    }
    if (seen != peer_count) {
        // The silent exit. It had no diagnostic at all until 2026-10-03, which is exactly the shape of defect
        // the other six were added to end: a refusal that leaves no trace reads, from outside, as a mechanism
        // that does not work.
        note_whole_refusal(node, tt_WHOLE_REFUSE_PEER_COUNT, "fewer live peer entries than the peer count");
        return 0;
    }
    return smallest;
#else
    UNUSED(node);
    UNUSED(peers);
    UNUSED(peer_count);
    return 0;
#endif
}

// How many datagrams this encoded sample goes as. `raw_len` is the length the encode produced, passed in for
// the same reason record_length_checked() takes it: deriving it from tx_tail is only the sample's length while
// the sample is the last thing in tx_buffer, and SHM_PLAN 6e's encoder step puts it somewhere else.
//
// This one was missed by the pass that fixed the cache's consumers, and it is the one that matters most to 6e:
// the fragmentation decision is what 6e changes for an all-local publish, so a length derived from the send
// buffer here would have been the first thing to go wrong.
static uint32_t sample_datagram_count(const struct tt_Context* node,
                                      const struct tt_SubmessageHeader* submessage_header, uint32_t raw_len,
                                      uint32_t whole_limit) {
#if tt_FRAG_ENABLED
    size_t length = raw_len;
    if (sizeof(struct tt_Header) + ROUNDUP(length) > whole_limit) {
        return frag_count_for(sample_cdr_length(node, submessage_header));
    }
#else
    UNUSED(node);
    UNUSED(submessage_header);
    UNUSED(raw_len);
    UNUSED(whole_limit);
#endif
    return 1;
}

#if tt_FRAG_ENABLED
static uint32_t frag_header_length(uint32_t index) {
    return (uint32_t)(sizeof(struct tt_SubmessageHeader) +
                      (index == 0 ? sizeof(struct tt_FragFirstHeader) : sizeof(struct tt_FragContHeader)));
}
#endif

// Bytes of arena the encoded sample at submessage_header takes: its one record, or one per fragment.
static uint32_t sample_cache_footprint(const struct tt_Context* node,
                                       const struct tt_SubmessageHeader* submessage_header, uint32_t encoded_len,
                                       uint32_t whole_limit) {
    uint32_t count = sample_datagram_count(node, submessage_header, encoded_len, whole_limit);
    if (count == 1) {
        return record_length_checked(node, submessage_header, encoded_len);
    }
    uint32_t total = 0;
#if tt_FRAG_ENABLED
    uint32_t cdr_len = sample_cdr_length(node, submessage_header);
    for (uint32_t index = 0; index < count; index++) {
        total += ROUNDUP(frag_header_length(index) + frag_payload_length(index, cdr_len));
    }
#endif
    return total;
}

// 0 when the encoded sample at submessage_header may be cached, or its record size when caching it
// would evict a sample nobody has acknowledged - which KEEP_ALL promises not to do. The count-based
// refusal in tt_Publisher_publish() cannot answer this: it runs before anything is encoded, and it
// is the arena rather than the index that a sample larger than the reserved record overflows.
// Returning the size rather than setting it keeps this a question; the caller decides. Split out of
// tt_Publisher_publish() to keep its cognitive complexity under clang-tidy's threshold, the same
// reasoning check_and_cache_sample() below was split out for.
static uint32_t keep_all_refused_record_bytes(const struct tt_Publisher* pub, const struct tt_Context* node,
                                              const struct tt_SubmessageHeader* submessage_header, uint32_t encoded_len,
                                              uint32_t whole_limit) {
    if (!pub->keep_all || pub->reliable_cache == NULL || !any_peer_ack_matched(pub)) {
        return 0; // KEEP_LAST may evict, an unretained Publisher has nothing to evict, and with no
                  // matched Subscriber there is nobody whose acknowledgement could ever arrive
    }
    uint32_t record = sample_cache_footprint(node, submessage_header, encoded_len, whole_limit);
    if (reliable_cache_admits(pub->reliable_cache, reliable_cache_depth(pub->reliable_cache), record,
                              keep_all_acked_through(pub))) {
        return 0;
    }
    return record;
}

// Whether KEEP_ALL refuses the encoded sample at submessage_header, on either half of its promise that
// needs the sample encoded first: the arena's bytes (keep_all_refused_record_bytes()), or - for a sample
// that goes as several datagrams, each taking a seq_no - the unacknowledged count, which the check before
// encoding could only make for one. Records what was refused, so tt_Publisher_writable() and the writable
// callback answer about the sample the caller will retry.
static bool keep_all_refuses_encoded(struct tt_Publisher* pub, const struct tt_Context* node, uint32_t encoded_len,
                                     const struct tt_SubmessageHeader* submessage_header, uint32_t whole_limit) {
    uint32_t datagrams = sample_datagram_count(node, submessage_header, encoded_len, whole_limit);
    if (datagrams > 1 && pub->keep_all && reliable_cache_depth(pub->reliable_cache) != 0 && any_peer_ack_matched(pub)) {
        uint32_t min_ack = min_peer_ack_seq_no(pub);
        uint32_t acked_through = keep_all_acked_through(pub);
        if (pub->seq_no + datagrams - acked_through > keep_all_bound(pub)) {
            pub->blocked_datagrams = (uint16_t)datagrams;
            return true;
        }
    }
    uint32_t refused_record = keep_all_refused_record_bytes(pub, node, submessage_header, encoded_len, whole_limit);
    if (refused_record != 0) {
        pub->blocked_record_bytes = refused_record; // what keep_all_writable() asks about from now on
        RSTAT_INC(publish_refused_bytes);
        return true;
    }
    return false;
}

#if tt_FRAG_ENABLED
// Retains a fragmented sample one record per datagram, each under its own seq_no and exactly as it is
// sent, so an ACKNACK naming one datagram resends that datagram and nothing else (DATAFRAG_PLAN.md
// section 13). A sample with more fragments than the cache has index slots, or more bytes than its
// arena, is sent and not retained: its seq_no are left as tombstones, which an ACKNACK is answered for
// with the eviction Heartbeat. Keeping part of a sample would advertise datagrams no reader could use.
static void cache_sample_fragments(struct tt_Context* node, struct tt_ReliableCache* cache, uint32_t raw_len,
                                   uint32_t whole_limit, struct tt_SubmessageHeader* submessage_header,
                                   uint32_t first_seq_no, uint32_t count) {
    uint16_t depth = reliable_cache_depth(cache);
    if (depth == 0) {
        return;
    }
    uint32_t length = raw_len;
    memset(node->tx_buffer + node->tx_tail, 0, ROUNDUP(length) - length); // sent padded; cached the same
    const struct tt_DataHeader* data_header = (const struct tt_DataHeader*)(submessage_header + 1);
    const uint8_t* cdr = (const uint8_t*)(data_header + 1);
    uint32_t cdr_len = sample_cdr_length(node, submessage_header);
    bool keep =
        count <= depth && sample_cache_footprint(node, submessage_header, raw_len, whole_limit) <= cache->arena_size;
    if (!keep) {
        TT_LOG_WARNING("Reliable sample %u (%u fragments) exceeds the cache (%u slots, %u bytes) - sent, not cached",
                       first_seq_no, count, depth, cache->arena_size);
        RSTAT_INC(not_cached_oversize);
    }
    reliable_cache_admit_sample(cache);
    for (uint32_t index = 0; index < count; index++) {
        uint32_t payload_length = frag_payload_length(index, cdr_len);
        uint32_t header_length = frag_header_length(index);
        uint8_t* record =
            reliable_cache_reserve(cache, first_seq_no + index, keep ? header_length + payload_length : 0);
        if (record != NULL) {
            frag_write_header(record, data_header, index, count, payload_length, tt_SUBMESSAGE_ID_ALL);
            _tt_memcpy(record + header_length, cdr + frag_payload_offset(index), payload_length);
        }
        if (index == 0) {
            reliable_cache_count_sample(cache, record);
        }
    }
}
#endif

// The encoded DATA submessage at submessage_header, checked and then retained. False (logged and
// counted by submessage_fits_datagram()) when no datagram could ever carry it - checked before
// caching, not only at end_encode(): a sample that can never be sent must not be retained either,
// or the cache would hold, and later offer to retransmit, a sample no reader was ever sent, under a
// sequence number the next publish then reuses. Split out of tt_Publisher_publish() to keep its
// cognitive complexity under clang-tidy's threshold.
// (g10) Whether caching a sample of `footprint` arena bytes now keeps the newest sample_depth - 1 samples beside it:
// what the write below would do, simulated on copies of the fields eviction moves. The count bound evicts the oldest
// sample first when sample_depth are already held, which KEEP_LAST allows; any further eviction, for bytes, would
// take a sample inside the depth.
static bool reliable_cache_keeps_depth(const struct tt_ReliableCache* cache, uint32_t footprint) {
    if (cache->sample_depth == 0) {
        return true; // no depth promised in samples
    }
    if (footprint > cache->arena_size) {
        return false;
    }
    uint16_t depth = reliable_cache_depth(cache);
    uint32_t oldest = cache->oldest_seq_no;
    uint32_t tail = cache->tail;
    if (oldest != 0 && cache->retained_samples >= cache->sample_depth) {
        do { // the oldest sample, every record of it, goes by count
            if (oldest == cache->newest_seq_no) {
                oldest = 0;
                tail = 0;
                break;
            }
            oldest++;
        } while (!reliable_cache_slot_live(cache, depth, oldest) ||
                 !reliable_cache_record_starts_sample(cache, reliable_cache_slot(cache, depth, oldest)));
    }
    if (oldest == 0) {
        return true; // nothing left retained: the whole arena is free
    }
    uint32_t head = reliable_cache_slot(cache, depth, oldest)->offset;
    return reliable_cache_offset_in(cache->arena_size, false, head, tail, footprint) != UINT32_MAX;
}

// KEEP_ALL's admission refuses only on behalf of matched Subscribers, so until the first one is matched nothing guards
// the arena - yet what the Publisher broadcasts meanwhile reaches Subscribers it has not matched, and each asks again
// for what it missed as soon as it acknowledges. Evicting those samples for bytes lost them for good: rmw KEEP_ALL
// Array4k under 5% loss, 5-8 samples per run, all in the first second (rig, 2026-10-05). So in that window the arena
// grows through the Publisher's hook rather than evicts. The window is one tt_CONTEXT_UPDATE_INTERVAL from the first
// publish: every live node announces at least once an interval, so a Subscriber that exists is matched by its end,
// and a Publisher still unmatched then has nobody to keep samples for and evicts as before rather than take its
// whole budget. The index ring's count bound is not grown: it holds far more datagrams than one match takes.
static void keep_all_room_before_match(struct tt_Context* node, struct tt_Publisher* pub,
                                       const struct tt_SubmessageHeader* submessage_header, uint32_t encoded_len,
                                       uint32_t whole_limit) {
    if (any_peer_ack_matched(pub)) {
        return;
    }
    uint64_t now = tt_get_ns();
    if (pub->keep_all_unmatched_until_ns == 0) {
        pub->keep_all_unmatched_until_ns = now + tt_CONTEXT_UPDATE_INTERVAL;
    }
    if (now > pub->keep_all_unmatched_until_ns || pub->cache_grow == NULL) {
        return;
    }
    struct tt_ReliableCache* cache = pub->reliable_cache;
    uint32_t footprint = sample_cache_footprint(node, submessage_header, encoded_len, whole_limit);
    // acked_through 0: room without evicting anything, since nobody has acknowledged anything.
    while (!reliable_cache_admits(cache, reliable_cache_depth(cache), footprint, 0)) {
        if (!pub->cache_grow(pub)) {
            return;
        }
    }
}

// (g10) Room for this KEEP_LAST sample without evicting inside the depth: grown through the Publisher's hook while it
// can, else counted.
static void make_depth_room(struct tt_Context* node, struct tt_Publisher* pub,
                            const struct tt_SubmessageHeader* submessage_header, uint32_t encoded_len,
                            uint32_t whole_limit) {
    struct tt_ReliableCache* cache = pub->reliable_cache;
    if (pub->keep_all) {
        keep_all_room_before_match(node, pub, submessage_header, encoded_len, whole_limit);
        return; // after the first match KEEP_ALL's own admission refuses rather than evicts
    }
    if (cache->sample_depth == 0) {
        return;
    }
    uint32_t footprint = sample_cache_footprint(node, submessage_header, encoded_len, whole_limit);
    while (!reliable_cache_keeps_depth(cache, footprint)) {
        if (pub->cache_grow == NULL || !pub->cache_grow(pub)) {
            cache->depth_shortfalls++;
            return;
        }
    }
}

static bool check_and_cache_sample(struct tt_Context* node, struct tt_Publisher* pub,
                                   struct tt_SubmessageHeader* submessage_header, uint32_t encoded_len,
                                   uint32_t whole_limit) {
    // With fragmentation every sample within tt_MAX_SAMPLE_LENGTH can be sent, and the caller has
    // already refused anything larger.
    if (!tt_FRAG_ENABLED && !submessage_fits_datagram(node, submessage_header, encoded_len, whole_limit)) {
        return false;
    }
    // QoS roadmap #5 (RELIABILITY) / #4 (DURABILITY) - see cache_reliable_sample()'s own doc
    // comment above; one shared write serves both, whichever (or both) this Publisher opted into.
    if (pub->reliable_cache != NULL) {
        make_depth_room(node, pub, submessage_header, encoded_len, whole_limit); // (g10)
#if tt_FRAG_ENABLED
        uint32_t datagrams = sample_datagram_count(node, submessage_header, encoded_len, whole_limit);
        if (datagrams > 1) {
            cache_sample_fragments(node, pub->reliable_cache, encoded_len, whole_limit, submessage_header,
                                   pub->seq_no + 1, datagrams);
            return true;
        }
#endif
        cache_reliable_sample(node, submessage_header, pub->reliable_cache, pub->seq_no + 1, encoded_len);
    }
    return true;
}

static bool piggyback_due(struct tt_Publisher* pub, bool is_flush) {
    if (!is_flush || pub->reliable_cache == NULL || pub->heartbeat_piggyback_every == 0) {
        return false;
    }
    pub->heartbeat_piggyback_count++;
    return pub->heartbeat_piggyback_count >= pub->heartbeat_piggyback_every;
}

// Append a Heartbeat behind the DATA the caller left pending, and send both in one datagram to the
// DATA's own peers - so the unicast decision the DATA made is the one that holds.
//
// If the Heartbeat cannot be appended - no room behind this DATA, or an encode failure it rolled
// back - the DATA is still sitting unsent, and goes on its own. A piggyback is an optimisation; it
// must never be the reason a sample is not published. Returns false only if that send fails.
static bool append_piggybacked_heartbeat(struct tt_Context* node, struct tt_Publisher* pub, const struct tt_Peer* peers,
                                         uint8_t peer_count) {
    pub->heartbeat_piggyback_count = 0;
    pub->retransmitted = 0;
    encode_and_send_heartbeat(node, pub, reliable_cache_oldest_seq_no(pub->reliable_cache), peers, peer_count,
                              tt_HEARTBEAT_FLAG_FINAL);
    if (node->tx_tail != sizeof(struct tt_Header)) {
        return flush_tx(node, node->tx_tail, peers, peer_count);
    }
    return true;
}

// end_encode() for the DATA submessage at submessage_header, or - when no datagram can carry it and
// fragmentation is compiled in - its fragments, sent at once whatever is_flush says, since a fragment
// never shares a datagram. Rolls back to old_tx_tail on failure where that is still meaningful.
static bool end_encode_sample(struct tt_Context* node, struct tt_SubmessageHeader* submessage_header, bool is_flush,
                              const struct tt_Peer* peers, uint8_t peer_count, uint32_t old_tx_tail,
                              uint32_t record_len) {
#if tt_FRAG_ENABLED
    // The length the encode produced, passed rather than reached for - the last site that still derived it
    // as "the distance from this submessage to tx_buffer's current end". record_length_checked() stopped
    // doing that on 2026-10-02 for the reason its comment gives: the subtraction mixes tx_buffer's base
    // with a submessage_header that SHM_PLAN 6e's encoder puts in a segment slot, where the two are
    // unrelated addresses. It is also the reason to pass it rather than recompute it here - this decision
    // and sample_datagram_count()'s must not be able to disagree about one sample's length.
    size_t length = record_len;
    if (sizeof(struct tt_Header) + ROUNDUP(length) >
        record_size_limit(node, FRAG_WHOLE_DATA_LIMIT, peers, peer_count)) {
        return send_tail_as_fragments(node, submessage_header, peers, peer_count);
    }
#else
    UNUSED(record_len); // the fragmentation decision is the only reader, and it is compiled out
#endif
    if (!end_encode(node, submessage_header, is_flush, peers, peer_count)) {
        rollback(node, old_tx_tail);
        return false;
    }
    return true;
}

static void put_match_heartbeat(struct tt_Context* node, struct tt_Publisher* pub);
#if tt_SEGMENT_ENABLED && tt_SEGMENT_ENCODE_IN_SLOT
// SHM_PLAN 6e(b), "encode into the slot": the one destination this publish has, when it would go as a DATA alone in its
// datagram to a single same-host peer - the only shape whose bytes in the slot are a function of this sample alone.
// False for anything else, and the staging path below then runs exactly as it did before 6e(b):
//   batch                a batching publisher leaves the send to node_flush(), with whatever else is in tx_buffer.
//   tx_buffer not empty  something is already waiting ahead of this DATA and would share its datagram.
//   summary skip armed   flush_tx() may send a summary just ahead of the datagram (note_reached_armed()).
//   piggyback due        a Heartbeat rides behind this DATA in the same datagram (piggyback_due()).
//   match point owed     a match Heartbeat goes ahead of this DATA (put_match_heartbeat(), first_owed_seq_no).
//   local subscribers    deliver_locally() wants its own copy of the CDR (g9) - left to the path that makes one.
//   not one peer's slot  a broadcast, several destinations, or one whose segment we hold nothing of.
// Only the conditions no later step would refuse are asked here; each is one a test removes (test_encode_in_slot.c).
// The segment itself is not looked up here: peer_segment() counts down its revalidation on every call, and this is
// asked before the cheaper size checks that can still send the publish down the staging path. Whether this context
// has ever attached a segment at all is asked first, from segment_slot_ceiling, which counts nothing down.
static bool encode_in_slot_destination(struct tt_Publisher* pub, uint32_t old_tx_tail, struct tx_destination* out) {
    struct tt_Context* node = pub->node;
    // An unempty tx_buffer is refused by unicast_destinations_for() below, as it refuses a whole record for it.
    if (pub->batch || node->summary_skip_armed || pub->match_heartbeat_pending) {
        return false;
    }
    if (pub->reliable_cache != NULL && pub->heartbeat_piggyback_every != 0 &&
        pub->heartbeat_piggyback_count + 1U >= pub->heartbeat_piggyback_every) {
        return false;
    }
#if tt_LOCAL_DELIVERY
    if (pub->local_subscriber_count != 0) {
        return false;
    }
#endif
    // A context that has never attached a peer's segment cannot publish into a slot - and one whose peers are all on
    // other hosts never will. segment_slot_ceiling is raised by every attach and by nothing else, so 0 says exactly
    // that, in one load. Asked first: the destination work below and try_publish_into_slot()'s size and KEEP_ALL checks
    // otherwise ran for every publish and then ran again on the staging path. Stage 1 / S1, 2026-10-09
    // (experiments/stage1_payg.sh): +790 user instructions a sample on a p1-p4 RELIABLE KEEP_ALL publisher whose one
    // peer was on another host, against the same commit with the module compiled out. A context that has attached a
    // segment asks the rest as before, and the first segment the staging path's own send attaches is used from the
    // next publish on.
    if (node->segment_slot_ceiling == 0) {
        return false;
    }
    // No peers is a broadcast, and a broadcast's one destination has no context id: peer_segment() answers NULL for
    // it, which is where it is refused rather than here as well.
    const struct tt_Peer* peers = NULL;
    uint8_t peer_count = unicast_destinations_for(node, pub, old_tx_tail, true, &peers);
    struct tx_destination destinations[TX_MAX_DESTINATIONS];
    if (tx_destinations(peers, peer_count, destinations) != 1) {
        return false;
    }
    *out = destinations[0];
    return true;
}

// The two-copy path, for comparison, encodes into tx_buffer as tt_Header + submessage header + DataHeader + CDR, pads
// it in end_encode(), and flush_tx() turns the first two headers into a tt_SingleHeader (to_single_form()) before
// segment_write() copies the rest into the slot. This writes the same record where that copy would have put it: the
// submessage header first, because check_and_cache_sample() retains the record in that form, and the single header
// over it once the cache has its copy. tests/test_encode_in_slot.c holds the slot to the two-copy path's bytes.
//
// A CLAIMED SLOT IS ALWAYS PUBLISHED (segment_claim()). Everything that can refuse this publish without a failure -
// a sample that would fragment or not fit the slot, KEEP_ALL's arena bound, a full ring - is asked before the claim,
// and answers by returning false, so the staging path handles it exactly as before: a full ring drops the sample and
// counts it there, never reroutes it. KEEP_ALL's encoded-sample checks depend only on the sample's length for a
// sample that goes as one datagram, which is why they can be asked before anything is encoded. What can still fail
// after the claim - the topic's encoder, or the cache refusing the record - publishes a zero-length record into the
// slot and returns the error the staging path would have returned, having sent nothing and advanced nothing.
//
// WHAT IT SAVES IS NOT THE COPY'S BYTES BUT ONE L1-TO-L1 PASS. The expensive part of the old segment_write() memcpy is
// taking the slot's lines from the reader's cache, and that cost does not go away: it moves into the topic's encoder,
// which now writes those lines. So the result depends on how the encoder writes. On the PC (2026-10-06, p3 same-host
// best-effort, 6 interleaved reps, a PC figure only) the bench's generated encoder, whose 1412-byte array copy gcc
// inlines as `rep movsq`, made the publisher 12% slower per sample (user 0.511 -> 0.570 us, 19% fewer instructions,
// 10% more cycles); the same code with that copy left to glibc's memcpy made it 10% faster (0.462 -> 0.414 us).
// tt_SEGMENT_ENCODE_IN_SLOT=0 is the control arm; the rig A/B decides.
static bool try_publish_into_slot(struct tt_Publisher* pub, struct tt_Data* data, uint32_t old_tx_tail,
                                  tt_ret_t* result) {
    struct tx_destination dest;
    if (!encode_in_slot_destination(pub, old_tx_tail, &dest)) {
        return false;
    }
    struct tt_Context* node = pub->node;
    int32_t cdr_len = pub->topic->data_encode_size(data);
    if (cdr_len < 0) {
        return false; // the staging path reports it; a length too large for a datagram is refused just below
    }
    const uint32_t framing = (uint32_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader));
    const uint32_t raw_len = framing + (uint32_t)cdr_len;
    const uint32_t record_len = ROUNDUP(raw_len);
    // One datagram on the wire, so one seq_no - a sample that fragments, or goes as a whole record wider than a
    // datagram, stays with the staging path. tx_buffer is at least twice that (tt_TX_BUFFER_LENGTH), so the staging
    // path could always have encoded what passes here.
    if (sizeof(struct tt_Header) + record_len > FRAG_WHOLE_DATA_LIMIT) {
        return false;
    }
    struct tt_SubmessageHeader probe = {tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, 0};
    if (keep_all_refused_record_bytes(pub, node, &probe, raw_len, FRAG_WHOLE_DATA_LIMIT) != 0) {
        return false; // the staging path refuses it and records what was refused
    }
    struct tt_SegmentHeader* segment = peer_segment(node, dest.context_id, dest.ip, dest.port);
    if (segment == NULL || record_len > segment->slot_bytes) {
        return false;
    }
    // Everything that need not be in the slot is computed before the claim. Between the claim and the publish the
    // reader stops at this slot - it takes records in index order - so that window is kept to the encode itself.
    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(node, &own_ip, &own_port);
    const uint32_t timestamp = timestamp_to_wire(tt_get_ns());
    uint32_t claimed = 0;
    struct tt_SegmentSlot* slot = segment_claim(segment, &claimed);
    if (slot == NULL) {
        return false; // full: the staging path drops it and counts it, as it always has
    }

    // The slot is ours from here, and every return below publishes it.
    uint8_t* record = segment_slot_payload(slot);
    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)record;
    *submessage_header = probe; // as start_encode() leaves it: the length is end_encode()'s, and the cache copies 0
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)(record + sizeof(struct tt_SubmessageHeader));
    data_header->endpoint_id = pub->endpoint.id;
    data_header->seq_no = pub->seq_no + 1;
    data_header->timestamp = timestamp;
    data_header->entity_id = pub->endpoint.entity_id;
    int32_t encoded_len = pub->topic->data_encode(data, record + framing, (uint32_t)cdr_len);
    TT_TRACE(tt_TRACE_ENCODED);
    // The padding, zeroed as end_encode() zeroes it - before the cache's copy here, which only makes it tidier.
    memset(record + raw_len, 0, record_len - raw_len);
    if (encoded_len < 0 || !check_and_cache_sample(node, pub, submessage_header, raw_len, FRAG_WHOLE_DATA_LIMIT)) {
        segment_publish(slot, claimed, 0, own_ip, own_port, 1); // the harmless record: nothing to read, span unused
        *result = tt_RET_PROTOCOL_ERROR;
        return true;
    }
    pub->blocked_record_bytes = 0;
    pub->blocked_datagrams = 0;
    (void)piggyback_due(pub, true); // its cadence counts this publish; encode_in_slot_destination() ruled out a firing

    struct tt_SingleHeader single = {native_single_marker(), tt_VERSION, node->id, tt_SUBMESSAGE_TYPE_DATA};
    _tt_memcpy(record, &single, sizeof(single));
    segment_publish(slot, claimed, record_len, own_ip, own_port, 1);
#ifdef tt_RELIABLE_STATS
    g_rstats.datagrams++; // flush_tx()'s accounting for the one DATA this datagram carries
    g_rstats.datagrams_with_data++;
    g_rstats.data_in_datagrams++;
    if (g_rstats.max_data_per_datagram < 1) {
        g_rstats.max_data_per_datagram = 1;
    }
#endif
    node->segment_encoded_in_slot++;
    segment_note_written(node, dest.context_id, segment, dest.ip, dest.port, true);

    pub->seq_no += 1;
    maybe_solicit_ack_at_watermark(pub);
    *result = tt_RET_OK;
    return true;
}
#endif

static tt_ret_t publisher_publish_locked(struct tt_Publisher* pub, struct tt_Data* data) {
    if (pub == NULL || data == NULL || pub->node == NULL || pub->topic == NULL ||
        pub->topic->data_encode_size == NULL || pub->topic->data_encode == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    struct tt_Context* node = pub->node;
    if (!pub->batch) {
        flush_pending_before_unicast(node, pub->peers);
    }
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
        arm_keep_all_resolicit(pub);
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
    tt_ret_t zerocopy_result = tt_RET_OK;
    if (try_publish_zerocopy(pub, data, old_tx_tail, &zerocopy_result)) {
        return zerocopy_result;
    }
#if tt_SEGMENT_ENABLED && tt_SEGMENT_ENCODE_IN_SLOT
    if (try_publish_into_slot(pub, data, old_tx_tail, &zerocopy_result)) {
        return zerocopy_result;
    }
#endif

    // A match Heartbeat ahead of the DATA while a Subscriber is still owed one (tt_PeerAck.first_owed_seq_no).
    // old_tx_tail stays where this publish began - the unicast decision and every rollback are about the whole
    // datagram - and data_tail is where the sample itself starts, which its length is measured from.
    if (pub->match_heartbeat_pending) {
        put_match_heartbeat(node, pub);
    }
    uint32_t data_tail = node->tx_tail;

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
    data_header->timestamp = timestamp_to_wire(tt_get_ns());
    data_header->entity_id = endpoint->entity_id; // Milestone 47 - this Publisher's own identity

    // DataBody
    int32_t cdr_len = pub->topic->data_encode_size(data);
    if (cdr_len < 0 || cdr_len > tt_MAX_SAMPLE_LENGTH) {
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
    TT_TRACE(tt_TRACE_ENCODED);
    if (encoded_len < 0) {
        rollback(node, old_tx_tail);
        return tt_RET_PROTOCOL_ERROR;
    }
#if tt_LOCAL_DELIVERY
    // (g9) This context's own Subscribers get the sample once it has gone out, from a copy: sending reuses tx_buffer.
    // The context's scratch, or - for a sample published from inside a local delivery, whose bytes the scratch still
    // holds - a stack copy of its own. A publish with no local Subscriber pays the branch.
    bool local = pub->local_subscriber_count != 0;
    bool nested = local && node->local_delivery_depth != 0;
    uint8_t nested_copy[nested ? (uint32_t)encoded_len : 1U];
    uint8_t* local_copy = nested ? nested_copy : node->local_scratch;
    uint32_t local_seq_no = pub->seq_no + 1;
    uint64_t local_timestamp = tt_get_ns();
    if (local) {
        _tt_memcpy(local_copy, cdr, (size_t)encoded_len);
    }
#endif

    // The halves of KEEP_ALL's promise that need the encoded sample (2026-09-25, 2026-09-26).
    if (keep_all_refuses_encoded(pub, node, node->tx_tail - data_tail, submessage_header, FRAG_WHOLE_DATA_LIMIT)) {
        rollback(node, old_tx_tail);
        pub->writable_pending = true;
        RSTAT_INC(publish_refused);
        solicit_ack_throttled(pub); // same bounded-stall reasoning as the count-based refusal above
        arm_keep_all_resolicit(pub);
        return tt_RET_WOULD_BLOCK;
    }

    // The length the encode produced, from the tail this publish saved before it began, rather than derived
    // inside the cache from tx_tail. 6e's encoder step writes into a slot, where "distance to tx_buffer's
    // current end" is a difference between two unrelated addresses. Raw: the cache rounds it, and the
    // fragment path's padding needs it unrounded.
    uint32_t record_len = node->tx_tail - data_tail;
    // The size a sample may reach before it has to be split. One datagram's worth today, for every path;
    // SHM_PLAN 6e(a) raises it to the destination's slot when every destination is a same-host peer whose
    // segment is already attached, which is the only case where an unfragmented record - larger than the
    // MTU by construction - has somewhere to go. Passed rather than reached for, so the cache's footprint
    // and the sent datagram count cannot disagree about it.
    uint32_t whole_limit = FRAG_WHOLE_DATA_LIMIT;
    // Hoisted above the datagram count, which used to be decided before it. 6e(a) needs to know WHERE this
    // sample is going before it can say how many pieces it goes in, and deciding the destination twice is how
    // the byte bound and the count bound came apart on 2026-09-25.
    bool is_flush = !pub->batch;
    const struct tt_Peer* peers = NULL;
    uint8_t peer_count = unicast_destinations_for(node, pub, old_tx_tail, is_flush, &peers);
    uint32_t whole_to_peers = whole_record_limit_for(node, peers, peer_count);
    if (whole_to_peers > whole_limit) {
        whole_limit = whole_to_peers;
    }
    // One seq_no per datagram the sample goes as - counted now, while it still sits in tx_buffer.
    // The sample's SEQ SPAN: how many seq_nos it consumes. Counted against the NETWORK form -
    // FRAG_WHOLE_DATA_LIMIT, not whole_limit - so it is the same number whatever path the sample takes
    // (SHM_PLAN 6e). Until 2026-10-03 this used whole_limit, which comes from the destinations, so a
    // publisher's seq_no advanced at a rate that depended on who was listening and whether their segment
    // happened to be attached: the same sample consumed one seq_no for an all-local publisher and two
    // when a remote subscriber was present. That is why 6e(a) had to forbid mixed destinations, and the
    // prohibition was not even sufficient - it is evaluated at publish while retransmission happens
    // later, so a segment detaching in between left a whole record that had to go out over UDP, where it
    // exceeds a datagram by construction and was dropped (the measured tx_dropped_oversize).
    uint32_t seq_span = sample_datagram_count(node, submessage_header, record_len, FRAG_WHOLE_DATA_LIMIT);
    if (!check_and_cache_sample(node, pub, submessage_header, record_len, whole_limit)) {
        rollback(node, old_tx_tail);
        return tt_RET_PROTOCOL_ERROR;
    }
    pub->blocked_record_bytes = 0; // this one was admitted; nothing outstanding to re-ask about
    pub->blocked_datagrams = 0;

    // pub->batch (default false, tt_Context_create_publisher() - see tickle.h's own doc comment on
    // it for why immediate is the default now): mirrors tt_Client_call()'s own peer decision and
    // shared-tx_buffer guard exactly - unicast to pub->peers when there's a small enough known
    // count (tt_UNICAST_PEER_THRESHOLD) *and* nothing else (e.g. a still-batched announce
    // from node_update()) was already sitting unflushed ahead of this DATA submessage, since
    // unicasting would only reach these peers, not whatever else needs the whole segment.
    // pub->batch == true keeps today's behavior unconditionally: never flush here, let
    // node_flush()'s own tt_CONTEXT_TX_INTERVAL tick decide broadcast vs. unicast for the whole
    // accumulated buffer at once.
    // Piggybacked Heartbeat (heartbeat_piggyback_every, tickle.h). Decided here, before the DATA's
    // end_encode(), because a piggyback changes whether that end_encode() flushes: the DATA is left
    // pending so the Heartbeat can be appended behind it and one flush sends both. Only when this
    // publish flushes anyway - a batching publisher leaves the send to node_flush(), and a
    // Heartbeat buried in a batch arrives no sooner than the batch does.
    bool piggyback = piggyback_due(pub, is_flush);

    // This datagram's seq footprint, for the segment path to record. Set here because this is where the
    // seq_nos are allocated, and the slot's reader must advance by the same number this publisher is
    // about to add. set_tx_tail() returns it to 1 when the buffer empties, so it cannot ride out on a
    // later record; segment_write() checks it against tt_FRAG_MAX_COUNT rather than trusting it.
    node->tx_seq_span = (uint16_t)seq_span;
    if (!end_encode_sample(node, submessage_header, is_flush && !piggyback, peers, peer_count, old_tx_tail,
                           record_len)) {
        return tt_RET_IO_ERROR;
    }

    pub->seq_no += seq_span;

    // After seq_no++, so the Heartbeat's last_seq_no includes the DATA it travels with.
    if (piggyback && !append_piggybacked_heartbeat(node, pub, peers, peer_count)) {
        return tt_RET_IO_ERROR;
    }

    // Phase 3 prerequisite (d) - after the sample is out and counted, ask for an ACK if the cache
    // is now watermark-full of unacknowledged samples. No-op unless a caller opted in.
    maybe_solicit_ack_at_watermark(pub);

#if tt_LOCAL_DELIVERY
    if (local) {
        deliver_locally(node, pub, local_copy, (uint32_t)encoded_len, local_seq_no, local_timestamp);
    }
#endif
    return tt_RET_OK;
}

tt_ret_t tt_Publisher_publish(struct tt_Publisher* pub, struct tt_Data* data) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
#if tt_CONTEXT_ID_CLAIM
    // (g8) A context left without an id of its own sends nothing, and says so: an error, never a silent success.
    if (locked_node != NULL && locked_node->id_muted) {
        locked_node->id_muted_drops++;
        state_unlock(locked_node);
        return tt_RET_IO_ERROR;
    }
#endif
    tt_ret_t result = publisher_publish_locked(pub, data);
    if (result == tt_RET_OK && pub->liveliness_lease_duration_ns != 0) {
        // The DATA asserts the writer's liveliness (tt_Publisher_assert_liveliness() rate-limits on this).
        // Only for a leased Publisher: the clock read is not free on the publish path (veth A/B, 2026-09-26).
        pub->liveliness_asserted_ns = tt_get_ns();
    }
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

tt_ret_t tt_Publisher_assert_liveliness(struct tt_Publisher* pub) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Context* node = pub->node;
    state_lock(node);
    uint64_t now = tt_get_ns();
    uint64_t lease = pub->liveliness_lease_duration_ns;
    if (lease != 0 && (pub->liveliness_asserted_ns == 0 ||
                       now - pub->liveliness_asserted_ns >= lease / tt_LIVELINESS_LEASE_DIVISOR)) {
        encode_and_send_heartbeat(node, pub, 0, NULL, 0, tt_HEARTBEAT_FLAG_FINAL | tt_HEARTBEAT_FLAG_LIVELINESS);
        pub->liveliness_asserted_ns = now;
    }
    state_unlock(node);
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
static void encode_heartbeat_range(struct tt_Context* node, struct tt_Publisher* pub, uint32_t first_seq_no,
                                   uint32_t last_seq_no, bool is_flush, const struct tt_Peer* peers, uint8_t peer_count,
                                   uint8_t flags) {
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
    heartbeat_header->last_seq_no = last_seq_no;
    heartbeat_header->entity_id = endpoint->entity_id; // Milestone 47 - this Publisher's own identity
    heartbeat_header->flags = flags;
    heartbeat_header->reserved[0] = 0;
    heartbeat_header->reserved[1] = 0;
    heartbeat_header->reserved[2] = 0;

    if (!end_encode(node, submessage_header, is_flush, peers, peer_count)) {
        rollback(node, old_tx_tail);
    }
}

static void encode_and_send_heartbeat(struct tt_Context* node, struct tt_Publisher* pub, uint32_t first_seq_no,
                                      const struct tt_Peer* peers, uint8_t peer_count, uint8_t flags) {
    encode_heartbeat_range(node, pub, first_seq_no, pub->seq_no, true, peers, peer_count, flags);
}

// Whether any of this Publisher's peers has an attached segment (SHM_PLAN 6e): a record bound only for segments may
// be larger than a datagram, and only one alone in tx_buffer is granted that (end_encode()), so the match Heartbeat
// goes as its own datagram there rather than ahead of the DATA.
static bool any_peer_on_segment(const struct tt_Context* node, const struct tt_Publisher* pub) {
#if tt_SEGMENT_ENABLED
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (pub->peers[i].context_id == tt_CONTEXT_ID_INVALID) {
            continue;
        }
        const struct tt_SegmentPeer* entry = segment_peer_if_live(node, pub->peers[i].context_id);
        if (entry != NULL && entry->mapping != NULL) {
            return true;
        }
    }
#else
    UNUSED(node);
    UNUSED(pub);
#endif
    return false;
}

// The match Heartbeat (tt_PeerAck.first_owed_seq_no) for the publish about to be encoded: left in tx_buffer, so the
// DATA's own flush carries both in one datagram to the DATA's own peers and a Subscriber cannot receive the DATA
// without it. Only where that holds - an unbatched publish into an empty buffer, to the unicast peers the DATA will
// go to; otherwise it goes now as its own datagram, as the periodic Heartbeat does (send_heartbeat()). A fragmented
// sample flushes it ahead of its fragments (send_tail_as_fragments()).
static void put_match_heartbeat(struct tt_Context* node, struct tt_Publisher* pub) {
    uint32_t owed = take_match_heartbeat(pub);
    if (owed == 0) {
        return;
    }
    uint32_t oldest = reliable_cache_oldest_seq_no(pub->reliable_cache);
    uint32_t first = oldest != 0 ? oldest : owed;
    uint8_t count = count_peers(pub->peers);
    bool empty = node->tx_tail == sizeof(struct tt_Header);
    bool unicast = count >= 1 && count <= tt_UNICAST_PEER_THRESHOLD && empty;
    if (unicast && !pub->batch && !any_peer_on_segment(node, pub)) {
        encode_heartbeat_range(node, pub, first, owed - 1, false, NULL, 0, tt_HEARTBEAT_FLAG_FINAL);
        return;
    }
    encode_heartbeat_range(node, pub, first, owed - 1, true, unicast ? pub->peers : NULL, unicast ? count : 0,
                           tt_HEARTBEAT_FLAG_FINAL);
}

// QoS roadmap #5 (RELIABILITY) follow-up - runs once per pub->heartbeat_period_ns (armed by tt_
// Publisher_set_heartbeat_period()), announcing pub->reliable_cache's own currently-retained
// range - see struct tt_HeartbeatHeader's own doc comment (tickle.h) for what this buys over the
// purely-reactive gap detection RELIABILITY already had on its own. Skips sending (but still
// reschedules) when nothing has been published yet - entries[] is still entirely empty, nothing
// to announce, same "nothing retained yet" short-circuit deliver_durability_backlog() already has.
static void send_heartbeat(struct tt_Context* node, uint64_t time, void* param) {
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

    if (!tt_Context_schedule(node, time + pub->heartbeat_period_ns, send_heartbeat, pub)) {
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
static void send_ack_solicit(struct tt_Context* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;
    pub->last_ack_solicit_ns = tt_get_ns(); // shared throttle - see ack_solicit_watermark_pct (tickle.h)
    (void)tt_Publisher_request_ack(pub);

    if (!tt_Context_schedule(node, time + pub->ack_solicit_period_ns, send_ack_solicit, pub)) {
        TT_LOG_ERROR("Cannot schedule send_ack_solicit");
    }
}

// QoS roadmap #5 (RELIABILITY) follow-up - fires once, the instant decode_update_entities()'s own
// upsert_peer() claims a previously-empty slot for this exact Publisher (a genuinely new - or
// forgotten-then-rejoined - peer, not every periodic announce refresh), mirroring deliver_
// durability_backlog()'s own identical trigger exactly. Closes the race a purely periodic
// Heartbeat can't: a newly-matched Subscriber's very first few samples are also the ones a slow
// periodic period is most likely to arrive too late to save (by the time it fires, reliable_
// cache's own tiny KEEP_LAST window has already evicted them) - an immediate, one-off greeting
// reaches the new peer as soon as discovery itself completes instead. No-op unless pub->reliable
// is set (see its own doc comment - this is exactly the unprompted-traffic case that flag exists
// to gate, so a durable-only Publisher doesn't also start emitting Heartbeats nobody asked for)
// and pub->reliable_cache is non-NULL and non-empty (nothing published yet) - fires regardless of
// whether periodic Heartbeat (tt_Publisher_set_heartbeat_period()) was ever separately enabled.
static void send_initial_heartbeat(struct tt_Context* node, struct tt_Publisher* pub, struct tt_Peer* target) {
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
static uint32_t publisher_unacked_bound_locked(const struct tt_Publisher* pub) {
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
        if (pub->peer_acks[i].context_id == tt_CONTEXT_ID_INVALID) {
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

uint32_t tt_Publisher_unacked_bound(const struct tt_Publisher* pub) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    uint32_t result = publisher_unacked_bound_locked(pub);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

uint64_t tt_reliable_retry_interval_configured(void) {
    return reliable_retry_configured();
}

static uint32_t publisher_min_acked_seq_no_locked(const struct tt_Publisher* pub) {
    return min_peer_ack_seq_no(pub);
}

uint32_t tt_Publisher_min_acked_seq_no(const struct tt_Publisher* pub) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    uint32_t result = publisher_min_acked_seq_no_locked(pub);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

static bool publisher_writable_locked(const struct tt_Publisher* pub) {
    if (pub == NULL) {
        return false;
    }
    return keep_all_writable(pub);
}

bool tt_Publisher_writable(const struct tt_Publisher* pub) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    bool result = publisher_writable_locked(pub);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

static bool publisher_is_acked_by_all_peers_locked(const struct tt_Publisher* pub, uint32_t seq_no) {
    // Phase 2 - every matched Subscriber entity must have got this far, not merely every matched
    // node: two Subscriptions of one topic in one remote process each have their own entry.
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].context_id == tt_CONTEXT_ID_INVALID) {
            continue;
        }
        if (pub->peer_acks[i].ack_seq_no <= seq_no) {
            return false; // never acked anything, or not this far yet
        }
    }
    return true;
}

bool tt_Publisher_is_acked_by_all_peers(const struct tt_Publisher* pub, uint32_t seq_no) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    bool result = publisher_is_acked_by_all_peers_locked(pub, seq_no);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
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

tt_ret_t tt_ReliableCache_grow(struct tt_ReliableCache* cache, uint8_t* new_arena, uint32_t new_arena_size) {
    if (cache == NULL || new_arena == NULL || cache->arena == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (new_arena_size < cache->arena_size) {
        return tt_RET_INVALID_ARGUMENT; // shrinking would have to drop retained samples
    }
    if (cache->arena_limit != 0 && new_arena_size > cache->arena_limit) {
        return tt_RET_INVALID_ARGUMENT; // the limit is the caller's own, set once at init
    }

    uint16_t depth = reliable_cache_depth(cache);
    if (depth == 0) {
        // Nothing usable to migrate - the index was never set up. Take the arena anyway, so a
        // caller that grows before its first publish is not a special case.
        cache->arena = new_arena;
        cache->arena_size = new_arena_size;
        return tt_RET_OK;
    }

    // Repack in sequence order rather than copying the ring as it lies: the live records may be two
    // runs with a gap between them, and the wrap fragment at the end is dead space this recovers.
    // Only offsets change - each sample keeps its slot, because depth does not change.
    uint32_t offset = 0;
    if (cache->oldest_seq_no != 0) {
        for (uint32_t seq_no = cache->oldest_seq_no; seq_no != 0 && seq_no <= cache->newest_seq_no; seq_no++) {
            struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, seq_no);
            if (entry->len == 0 || entry->seq_no != seq_no) {
                continue; // a tombstone, or a slot already taken over by a later sample
            }
            _tt_memcpy(new_arena + offset, cache->arena + entry->offset, entry->len);
            entry->offset = offset;
            offset += entry->len;
        }
    }

    cache->arena = new_arena;
    cache->arena_size = new_arena_size;
    cache->tail = offset;
    if (offset == 0) {
        cache->oldest_seq_no = 0; // everything retained turned out to be a tombstone
        cache->retained_samples = 0;
    }
    return tt_RET_OK;
}

static tt_ret_t publisher_set_heartbeat_period_locked(struct tt_Publisher* pub, uint64_t period_ns) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (period_ns != 0 && pub->reliable_cache == NULL) {
        return tt_RET_INVALID_ARGUMENT; // nothing for a Heartbeat to announce without one
    }

    if (pub->heartbeat_period_ns != 0) {
        tt_Context_unschedule(pub->node, send_heartbeat, pub); // re-arming or disabling either way
    }
    pub->heartbeat_period_ns = period_ns;
    if (period_ns == 0) {
        return tt_RET_OK; // disabled
    }

    if (!tt_Context_schedule(pub->node, tt_get_ns() + period_ns, send_heartbeat, pub)) {
        pub->heartbeat_period_ns = 0;  // failed to arm - stay disabled rather than claim it's on
        return tt_RET_OUT_OF_SCHEDULE; // tt_MAX_SCHEDULER_LENGTH exhausted
    }
    return tt_RET_OK;
}

tt_ret_t tt_Publisher_set_heartbeat_period(struct tt_Publisher* pub, uint64_t period_ns) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = publisher_set_heartbeat_period_locked(pub, period_ns);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

// See struct tt_Publisher.ack_solicit_period_ns's own doc comment (tickle.h) for why this needs an
// explicit call rather than just setting that field directly - same reasoning as tt_Publisher_
// set_heartbeat_period() above.
static tt_ret_t publisher_set_ack_solicit_period_locked(struct tt_Publisher* pub, uint64_t period_ns) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (period_ns != 0 && pub->reliable_cache == NULL) {
        return tt_RET_INVALID_ARGUMENT; // nothing for a solicited ACKNACK to confirm without one
    }

    if (pub->ack_solicit_period_ns != 0) {
        tt_Context_unschedule(pub->node, send_ack_solicit, pub); // re-arming or disabling either way
    }
    pub->ack_solicit_period_ns = period_ns;
    if (period_ns == 0) {
        return tt_RET_OK; // disabled
    }

    if (!tt_Context_schedule(pub->node, tt_get_ns() + period_ns, send_ack_solicit, pub)) {
        pub->ack_solicit_period_ns = 0; // failed to arm - stay disabled rather than claim it's on
        return tt_RET_OUT_OF_SCHEDULE;  // tt_MAX_SCHEDULER_LENGTH exhausted
    }
    return tt_RET_OK;
}

tt_ret_t tt_Publisher_set_ack_solicit_period(struct tt_Publisher* pub, uint64_t period_ns) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = publisher_set_ack_solicit_period_locked(pub, period_ns);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

// See its own doc comment (tickle.h) for what this is for. Builds its own dense peer list from
// pub->peers[] rather than passing pub->peers/count_peers(pub->peers) straight through the way
// send_heartbeat()/tt_Publisher_publish() do. Those rely on peers[] having no gap before the first
// count_peers() slots, which forget_peer() did not guarantee when this was written - it cleared a
// departed peer in place - and the gap this comment flagged is what failed CI's interfaces check
// (fixed 2026-10-08: forget_peer() compacts). The filter is kept: it costs O(tt_MAX_PEER_COUNT) on
// a call that is not per sample, and solicitation must reach every matched peer.
static tt_ret_t publisher_request_ack_locked(struct tt_Publisher* pub) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    if (pub->reliable_cache == NULL) {
        return tt_RET_INVALID_ARGUMENT; // best-effort - no ack state to solicit
    }

    struct tt_Peer live_peers[tt_MAX_PEER_COUNT];
    uint8_t live_count = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (pub->peers[i].context_id != tt_CONTEXT_ID_INVALID) {
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

tt_ret_t tt_Publisher_request_ack(struct tt_Publisher* pub) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = publisher_request_ack_locked(pub);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

// Milestone 47 "goodbye" - broadcasts the node's own now-reduced entity list right away, instead
// of waiting for node_update()'s own next periodic tick (up to tt_CONTEXT_UPDATE_INTERVAL later).
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
// its own periodic reschedule on top of the one already pending - safe inside tt_Context_destroy()
// only because that function wipes the whole scheduler right after, not true for a per-entity
// destroy that leaves the node running).
static void broadcast_goodbye(struct tt_Context* node) {
    build_and_send_update(node, NULL, 0);
    if (!flush_tx(node, node->tx_tail, NULL, 0)) {
        TT_LOG_WARNING("Could not send farewell announce on entity destroy");
    }
}

static tt_ret_t publisher_destroy_locked(struct tt_Publisher* pub) {
    if (pub == NULL || pub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)pub;
    struct tt_Context* node = pub->node;

    // Cancel a still-armed Heartbeat before this Publisher (its own schedule param) goes away -
    // same reasoning as tt_Subscriber_destroy()'s own acknack_retry cancellation just below.
    if (pub->heartbeat_period_ns != 0) {
        tt_Context_unschedule(node, send_heartbeat, pub);
    }
    // Same reasoning, for a still-armed periodic ACK solicitation.
    if (pub->ack_solicit_period_ns != 0) {
        tt_Context_unschedule(node, send_ack_solicit, pub);
    }
    // ...and for a refused Publisher's own retry of its solicitation.
    if (pub->resolicit_armed) {
        tt_Context_unschedule(node, keep_all_resolicit, pub);
        pub->resolicit_armed = false;
    }

    if (!remove_endpoint_from_node(node, endpoint)) {
        return tt_RET_IILEGAL_ENDPOINT_ID;
    }
    node->last_modified = tt_get_ns();
    broadcast_goodbye(node);
    return tt_RET_OK;
}

tt_ret_t tt_Publisher_destroy(struct tt_Publisher* pub) {
    struct tt_Context* locked_node = pub != NULL ? pub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = publisher_destroy_locked(pub);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

// The complete delivery-order numbers for one Subscriber, emitted exactly once, whichever way it
// goes away.
//
// Why it is called from two places (2026-09-24): this used to be emitted only from the loop in
// tt_Context_destroy() that walks node->endpoints, and under rmw_tickle it therefore never fired at
// all. rmw_destroy_subscription() calls tt_Subscriber_destroy() before the node is destroyed, so
// by the time that loop runs the Subscriber has already been removed from the table and its
// counters go with it. The counters were being computed correctly for an entire benchmark and
// then discarded in silence.
//
// The two paths are mutually exclusive rather than merely usually-not-both:
// tt_Subscriber_destroy() removes the endpoint from node->endpoints before it returns, so a
// Subscriber reported here cannot still be in the table the node teardown walks. Nothing needs a
// "already reported" flag.
//
// What this cost is worth recording, because the verification looked sound: these counters were
// checked on a real two-node loopback run and reported real numbers. That run destroys its node
// with endpoints still attached, which is not the shape rmw uses - so the test exercised the one
// path that worked. A number that appears on the bench and never in production is not a weaker
// version of a working instrument, it is a missing one.
static void report_delivery_counters(const struct tt_Subscriber* sub, uint32_t endpoint_id) {
    // The throttled WARNINGs during a run fire on the 1st, 10th, 100th ... occurrence and
    // undercount by design. These are the complete numbers, and a run's conclusion should be read
    // from them rather than from how many log lines appeared.
    TT_LOG_INFO("Subscriber %u delivery: delivered=%lu out_of_order=%lu timestamp_not_newer=%lu "
                "writer_switches=%lu via_socket_flips=%lu out_of_order_discarded=%lu rxo_drops=%lu "
                "reorder_held_peak=%lu reorder_delivered=%lu reorder_overflow=%lu reorder_abandoned=%lu "
                "gap_abandoned=%lu gap_evicted=%lu superseded=%lu",
                endpoint_id, (unsigned long)sub->delivered, (unsigned long)sub->out_of_order,
                (unsigned long)sub->timestamp_not_newer, (unsigned long)sub->writer_switches,
                (unsigned long)sub->via_socket_flips, (unsigned long)sub->out_of_order_discarded,
                (unsigned long)sub->rxo_drops, (unsigned long)sub->reorder_held_peak,
                (unsigned long)sub->reorder_delivered, (unsigned long)sub->reorder_overflow,
                (unsigned long)sub->reorder_abandoned, (unsigned long)sub->gap_abandoned,
                (unsigned long)sub->gap_evicted, (unsigned long)sub->superseded);
}

static tt_ret_t subscriber_destroy_locked(struct tt_Subscriber* sub) {
    if (sub == NULL || sub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)sub;
    struct tt_Context* node = sub->node;
#if tt_FRAG_ENABLED
    if (node->frag_fast_sub == sub) {
        node->frag_fast_sub = NULL; // its part-assembled sample goes with it, as its reorder buffer does
    }
#endif

    // Cancel every outstanding per-writer acknack_retry before this Subscriber's own writers[]
    // table (each entry's own schedule param) goes away - same reasoning as tt_Client_destroy()'s
    // own call_retry cancellation, just once per still-armed WriterProxy instead of once for the
    // whole Subscriber (Milestone 47 - acknack_retry() is now scheduled per struct tt_WriterProxy,
    // not per Subscriber, see its own doc comment).
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (sub->writers[i].acknack_scheduled) {
            tt_Context_unschedule(node, acknack_retry, &sub->writers[i]);
            sub->writers[i].acknack_scheduled = false;
        }
    }

    if (remove_endpoint_from_node(node, endpoint)) {
        report_delivery_counters(sub, endpoint->id);
        node->last_modified = tt_get_ns();
        broadcast_goodbye(node);
        return tt_RET_OK;
    }

    return tt_RET_IILEGAL_ENDPOINT_ID;
}

tt_ret_t tt_Subscriber_destroy(struct tt_Subscriber* sub) {
    struct tt_Context* locked_node = sub != NULL ? sub->node : NULL;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = subscriber_destroy_locked(sub);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
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

static void bitmap_clear_bit(uint64_t* bitmap, uint32_t offset) {
    bitmap[offset / tt_RELIABLE_BITMAP_WORD_BITS] &= ~(1ULL << (offset % tt_RELIABLE_BITMAP_WORD_BITS));
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
static enum tt_WriterKeepAll writer_announced_keep_all(struct tt_Context* node, uint8_t node_id, uint32_t endpoint_id,
                                                       uint32_t entity_id) {
    if (node == NULL || node->discovery == NULL) {
        return tt_WRITER_KEEP_ALL_UNKNOWN;
    }
    // This writer's own announce, not the first of its topic on that context: two writers there may differ.
    const struct tt_DiscoveredEntity* writer =
        tt_Discovery_find_entity(node->discovery, node_id, endpoint_id, entity_id);
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
        if (sub->writers[i].context_id == node_id && sub->writers[i].entity_id == entity_id) {
            return &sub->writers[i];
        }
    }
    return NULL;
}

// Milestone 47 - find_writer_proxy() above, but claims and initializes the first empty slot on a
// miss instead of returning NULL (every DATA/HEARTBEAT-driven call site below wants this). A new
// entry starts at ack_seq_no 1 (a Publisher's first sample is always seq_no 1, never 0), matching
// tt_Context_create_subscriber()'s own former up-front default - now applied lazily, per writer, the
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
        if (sub->writers[i].context_id == tt_CONTEXT_ID_INVALID) {
            proxy = &sub->writers[i];
            proxy->context_id = node_id;
            proxy->entity_id = entity_id;
            proxy->sender_ip = 0;
            proxy->sender_port = 0;
            proxy->ack_seq_no = 1;
            proxy->reorder_cursor = 1;
            proxy->highest_delivered = 0;
            proxy->superseded_pending = 0;
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
            proxy->keep_all = writer_announced_keep_all(sub->node, node_id, ((struct tt_Endpoint*)sub)->id, entity_id);
            proxy->presence_acked = false;
            // Phase 2 - this slot's own window inside the Subscriber's tracking storage: the
            // caller-provided buffer when it gave one, otherwise the builtin default.
            uint64_t* tracking = sub->tracking_bitmaps != NULL ? sub->tracking_bitmaps : sub->builtin_tracking;
            proxy->received_bitmap = tracking + ((size_t)i * subscriber_tracking_words(sub));
            bitmap_clear(proxy->received_bitmap, proxy_words(proxy));
            proxy->retry = 0;
            proxy->acknack_scheduled = false;
            proxy->heartbeat_last_seq_no = 0;
            // A reused slot must not inherit a departed writer's recovery estimate: a new writer
            // may be on a different path entirely.
            proxy->recovery_srtt_ns = 0;
            proxy->recovery_rttvar_ns = 0;
            proxy->probe_seq_no = 0;
            proxy->probe_ns = 0;
            for (int slot = 0; slot < tt_RELIABLE_REQUEST_HISTORY; slot++) {
                proxy->requests[slot].sent_ns = 0;
            }
            proxy->request_next = 0;
            proxy->transit_srtt_ns = 0;
            proxy->transit_rttvar_ns = 0;
            if (out_created != NULL) {
                *out_created = true;
            }
            // Logged so that "this Subscriber never matched anyone" can be told apart from "it
            // matched and then received nothing" - the two look identical from outside, and the
            // rmw_tickle zero-delivery failure under investigation on 2026-09-23 is total and
            // silent either way. Once per writer, so the volume is bounded by tt_MAX_PEER_COUNT.
            TT_LOG_INFO("Writer proxy created: node %u entity %u for endpoint %u", node_id, entity_id,
                        ((struct tt_Endpoint*)sub)->id);
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

// How long a repair this reader asked for is taken to be on its way: the measured repair transit's srtt +
// max(G, 4 * rttvar) - the retry timer's own formula, on a sample that counts only the
// transit - or tt_RELIABLE_RETRY_INITIAL before there is one. srtt alone (d603d369) was too short at c6 on the rig: a
// repair queued behind ~850 Mbps of the writer's own data arrives with a spread as wide as its mean (rttvar ~ srtt),
// and every one later than srtt was asked for again. The transit is not the recovery estimate the timer runs on,
// which a lost repair or a bounded reader's declines lengthen: f3451cd8 was about a reader whose timer had grown to
// ~130 ms while its writer, stopped and refused, would have answered within a round trip.
static uint64_t repair_in_flight_ns(const struct tt_Context* node, const struct tt_WriterProxy* proxy) {
    if (proxy->transit_srtt_ns == 0) {
        return (uint64_t)tt_RELIABLE_RETRY_INITIAL;
    }
    uint64_t spread = 4ULL * proxy->transit_rttvar_ns;
    uint64_t granularity = reliable_retry_granularity(node);
    return (uint64_t)proxy->transit_srtt_ns + (spread > granularity ? spread : granularity);
}

// An ACKNACK just went out naming seq_nos first..last (and possibly fewer in between). See tt_WriterProxy.requests.
static void note_requested(struct tt_WriterProxy* proxy, uint32_t first_seq_no, uint32_t last_seq_no, uint64_t now) {
    struct tt_RepairRequest* request = &proxy->requests[proxy->request_next];
    request->sent_ns = now != 0 ? now : 1U;
    request->first_seq_no = first_seq_no;
    request->last_seq_no = last_seq_no;
    proxy->request_next = (uint8_t)((proxy->request_next + 1U) % tt_RELIABLE_REQUEST_HISTORY);
}

// A repair of seq_no (a copy addressed to this node, tt_Context.rx_targeted) arrived, at or above ack_seq_no; if
// seq_no was still missing, one transit sample, from the oldest remembered request that named it. The oldest and not
// the latest, the same choice note_watermark_requested() makes: when a sample was asked for twice, the copy that
// arrived may answer either, and timing it from the later request would shorten the estimate - the window - and ask for
// repairs in flight again, which shortens it further. From the older one it errs long, which costs a lost repair one
// late re-request instead.
static void note_repair_arrival(struct tt_WriterProxy* proxy, uint32_t seq_no, uint64_t now) {
    bool missing =
        seq_no == proxy->ack_seq_no || ((uint64_t)seq_no - proxy->ack_seq_no < proxy_window_bits(proxy) &&
                                        !bitmap_test_bit(proxy->received_bitmap, seq_no - proxy->ack_seq_no));
    if (!missing) {
        return; // a second copy of something already here: it says nothing about the request it answers
    }
    uint64_t oldest = 0;
    for (int slot = 0; slot < tt_RELIABLE_REQUEST_HISTORY; slot++) {
        const struct tt_RepairRequest* request = &proxy->requests[slot];
        if (request->sent_ns != 0 && seq_no >= request->first_seq_no && seq_no <= request->last_seq_no &&
            (oldest == 0 || request->sent_ns < oldest)) {
            oldest = request->sent_ns;
        }
    }
    if (oldest == 0 || now < oldest) {
        return; // named by no request still remembered - nothing to time it from
    }
    rtt_estimate_fold(&proxy->transit_srtt_ns, &proxy->transit_rttvar_ns, now - oldest);
}

// A copy of seq_no arrived and this reader declined it (tt_Subscriber.accept_callback: a bounded KEEP_ALL queue with
// nowhere to put it). Every request that named seq_no has had its answer - the repair came, it is not on its way - so
// each remembered request naming it is forgotten. Kept, they did two things wrong (2026-10-09, rmw_samehost.sh's PC
// preflight, tput Array1k RELIABLE KEEP_ALL on one host, no loss, perf_test's reader slower than the writer):
//   - answer_ack_request() left the declined sample out of every answer for the whole repair-transit window, so the
//     refused writer, asking about once a millisecond, was told "on its way" while the reader had room again: blocks
//     of 23-40 ms carried 20-36 answers and one window of resends (~960 named bits) between them;
//   - the copy finally kept was timed from the request before the decline (note_repair_arrival()), so the decline's
//     wait went into the transit estimate, which widened the window, which lengthened the next wait. Past rmw_tickle's
//     100 ms publish bound the writer gave up ("blocked 100ms ... gave up") with nothing lost.
// Forgetting the whole remembered range can name again a repair of another sample in it that is still in flight - a
// duplicate, only while this reader is declining.
static void forget_requests_answered_by(struct tt_Subscriber* sub, uint8_t writer_node_id, uint32_t writer_entity_id,
                                        uint32_t seq_no) {
    struct tt_WriterProxy* proxy = sub->reliable ? find_writer_proxy(sub, writer_node_id, writer_entity_id) : NULL;
    if (proxy == NULL) {
        return; // BEST_EFFORT asks for nothing; a reliable reader with no proxy for this writer has asked for nothing
    }
    for (int slot = 0; slot < tt_RELIABLE_REQUEST_HISTORY; slot++) {
        struct tt_RepairRequest* request = &proxy->requests[slot];
        if (request->sent_ns != 0 && seq_no >= request->first_seq_no && seq_no <= request->last_seq_no) {
            request->sent_ns = 0;
        }
    }
}

// The bits of bitmap word `word` that lie in positions low..high (both inclusive); 0 when none do, or low > high.
static uint64_t bitmap_range_in_word(int word, int low, int high) {
    int base = word * tt_RELIABLE_BITMAP_WORD_BITS;
    int first = low > base ? low - base : 0;
    int last = high - base < tt_RELIABLE_BITMAP_WORD_BITS - 1 ? high - base : tt_RELIABLE_BITMAP_WORD_BITS - 1;
    if (low > high || first > last) {
        return 0;
    }
    uint64_t upto_last = last == tt_RELIABLE_BITMAP_WORD_BITS - 1 ? ~0ULL : (1ULL << (last + 1)) - 1;
    return upto_last & ~((1ULL << first) - 1);
}

// A run of bit positions (relative to ack_seq_no, both inclusive) an ACKNACK leaves out.
struct acknack_skip {
    int low_bit;
    int high_bit;
};

// Requests (ACKNACK "please resend" bits) only positions low_bit..high_bit relative to ack_seq_no,
// further masked to what's still missing in received_bitmap, less the skip_count runs in skips
// (answer_ack_request() leaves out repairs still on their way). send_acknack() below is the usual
// full-range form; update_reliable_ack() uses a narrow range for Phase 1-a's per-new-gap NACK.
static void send_acknack_skipping(struct tt_Context* node, struct tt_WriterProxy* proxy, int low_bit, int high_bit,
                                  const struct acknack_skip* skips, int skip_count) {
    struct tt_Subscriber* sub = proxy->sub;
    struct tt_Endpoint* endpoint = (struct tt_Endpoint*)sub;
    struct tt_Peer target = {proxy->context_id, proxy->sender_ip, proxy->sender_port};
    uint32_t old_tx_tail = node->tx_tail;

    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_ACKNACK, target.context_id);
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
    int first_named = -1;
    int last_named = -1;
    for (uint16_t word = 0; word < wire_words; word++) {
        uint64_t request = ~proxy->received_bitmap[word] & request_mask[word] & ~below_low_mask[word];
        for (int skip = 0; skip < skip_count; skip++) {
            request &= ~bitmap_range_in_word(word, skips[skip].low_bit, skips[skip].high_bit);
        }
        acknack_header->bitmap[word] = request;
        if (request != 0) {
            int base = word * tt_RELIABLE_BITMAP_WORD_BITS;
            first_named = first_named < 0 ? base + __builtin_ctzll(request) : first_named;
            last_named = base + (tt_RELIABLE_BITMAP_WORD_BITS - 1 - __builtin_clzll(request));
        }
    }
    bool names_watermark = first_named == 0;
    // Milestone 47 - the *target* Publisher's own entity_id, learned from whichever WriterProxy
    // this ACKNACK answers - see struct tt_AckNackHeader.entity_id's own doc comment (tickle.h).
    acknack_header->entity_id = proxy->entity_id;

    // Unicast straight back to whoever's DATA this acks - same "nothing else queued" guard as
    // process_callrequest()'s own CallResponse. Falling back to broadcast when something else is
    // already staged is still correct here: the submessage's own receiver field (target.context_id,
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
        return;
    }
    // Only a request that names the watermark itself can time its recovery: a narrow request for a
    // newly opened gap further ahead (low_bit > 0) does not ask for ack_seq_no at all.
    if (names_watermark) {
        note_watermark_requested(proxy, tt_get_ns());
    }
    if (first_named >= 0) {
        note_requested(proxy, proxy->ack_seq_no + (uint32_t)first_named, proxy->ack_seq_no + (uint32_t)last_named,
                       tt_get_ns());
    }
#ifdef tt_RELIABLE_STATS
    {
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

static void send_acknack_range(struct tt_Context* node, struct tt_WriterProxy* proxy, int low_bit, int high_bit) {
    send_acknack_skipping(node, proxy, low_bit, high_bit, NULL, 0);
}

static void send_acknack(struct tt_Context* node, struct tt_WriterProxy* proxy) {
    send_acknack_range(node, proxy, 0, highest_relevant_bit(proxy));
}

// The reliable Subscriber's ACKNACK retry cadence - tt_RELIABLE_DEADLINE when set, otherwise
// tt_RELIABLE_RETRY_INTERVAL (config.h, Phase 1-b). Shared by acknack_retry() and
// maybe_arm_acknack_retry() so the first retry and every later one use the same interval.
// The configured interval: tt_RELIABLE_DEADLINE when set, else tt_RELIABLE_RETRY_INTERVAL - which
// may be 0, meaning dynamic.
// Chosen by the preprocessor rather than a conditional expression: with the dynamic default both
// macros are 0, and `0 != 0 ? 0 : 0` is a conditional with identical branches.
static uint64_t reliable_retry_configured(void) {
#if tt_RELIABLE_DEADLINE != 0
    return (uint64_t)tt_RELIABLE_DEADLINE;
#else
    return (uint64_t)tt_RELIABLE_RETRY_INTERVAL;
#endif
}

// One proxy's interval, given the configured value. A non-zero configured value is the caller's
// explicit choice and wins outright. 0 derives it from this proxy's own recovery estimate (see
// tt_WriterProxy.recovery_srtt_ns): srtt + max(G, 4 * rttvar) - G being `granularity`, the context's
// reliable_retry_granularity() - at most
// tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE * srtt, or tt_RELIABLE_RETRY_INITIAL until there is a first
// sample. See config.h for why the bounds are relative to srtt and why the one absolute term remains.
//
// The configured value is a parameter rather than read here so the dynamic path can be exercised
// by tests in a build whose default is fixed - otherwise the branch this whole feature is would be
// untestable in the default build.
static uint64_t retry_interval_for(uint64_t configured, uint64_t granularity, const struct tt_WriterProxy* proxy) {
    if (configured != 0) {
        return configured;
    }
    if (proxy == NULL || proxy->recovery_srtt_ns == 0) {
        return (uint64_t)tt_RELIABLE_RETRY_INITIAL;
    }
    uint64_t srtt = proxy->recovery_srtt_ns;
    uint64_t spread = 4ULL * proxy->recovery_rttvar_ns;
    if (spread < granularity) {
        spread = granularity;
    }
    uint64_t interval = srtt + spread;
    uint64_t ceiling = srtt * (uint64_t)tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE;
    return interval > ceiling ? ceiling : interval;
}

static uint64_t reliable_retry_interval(const struct tt_Context* node, const struct tt_WriterProxy* proxy) {
    return retry_interval_for(reliable_retry_configured(), reliable_retry_granularity(node), proxy);
}

// A Publisher has no recovery estimate of its own - it is the Subscriber that times recoveries - so
// in dynamic mode its ACK-solicitation throttle keeps the fixed starting value it always had.
static uint64_t reliable_retry_interval_publisher(void) {
    uint64_t configured = reliable_retry_configured();
    return configured != 0 ? configured : (uint64_t)tt_RELIABLE_RETRY_INITIAL;
}

// Folds one request-to-recovery time into proxy's estimate, RFC 6298-style: on the first sample
// srtt = R and rttvar = R/2, then rttvar = 3/4 rttvar + 1/4 |srtt - R| and srtt = 7/8 srtt + 1/8 R.
// R is clamped to at least 1ns so a first sample can never leave srtt at the 0 that means "none".
// RFC 6298's gains, as the shifts they are: srtt moves 1/8 of the way to each sample and rttvar 1/4
// of the way to each deviation. Named so the arithmetic below reads as the RFC does.
#define RECOVERY_SRTT_KEEP 7U // srtt = (7 * srtt + R) / 8
#define RECOVERY_SRTT_DIV 8U
#define RECOVERY_RTTVAR_KEEP 3U // rttvar = (3 * rttvar + |srtt - R|) / 4
#define RECOVERY_RTTVAR_DIV 4U

static void rtt_estimate_fold(uint32_t* srtt_ns, uint32_t* rttvar_ns, uint64_t sample_ns) {
    uint32_t sample = UINT32_MAX;
    if (sample_ns == 0) {
        sample = 1U;
    } else if (sample_ns < UINT32_MAX) {
        sample = (uint32_t)sample_ns;
    }
    if (*srtt_ns == 0) {
        *srtt_ns = sample;
        *rttvar_ns = sample / 2U;
        return;
    }
    uint32_t err = *srtt_ns > sample ? *srtt_ns - sample : sample - *srtt_ns;
    *rttvar_ns = (uint32_t)((((uint64_t)RECOVERY_RTTVAR_KEEP * *rttvar_ns) + err) / RECOVERY_RTTVAR_DIV);
    *srtt_ns = (uint32_t)((((uint64_t)RECOVERY_SRTT_KEEP * *srtt_ns) + sample) / RECOVERY_SRTT_DIV);
}

static void note_recovery_sample(struct tt_WriterProxy* proxy, uint64_t sample_ns) {
    rtt_estimate_fold(&proxy->recovery_srtt_ns, &proxy->recovery_rttvar_ns, sample_ns);
}

// An ACKNACK naming the watermark just went out. Starts a probe on it unless one is already running
// for this same watermark - a retry keeps the first request's timestamp, which is the whole point
// (see tt_WriterProxy.probe_ns). A probe left behind by a watermark that moved on without its sample
// arriving (a give-up, a jump, an eviction) no longer matches ack_seq_no, and is simply replaced.
static void note_watermark_requested(struct tt_WriterProxy* proxy, uint64_t now) {
    if (proxy->probe_ns != 0 && proxy->probe_seq_no == proxy->ack_seq_no) {
        return;
    }
    proxy->probe_seq_no = proxy->ack_seq_no;
    proxy->probe_ns = now != 0 ? now : 1U;
}

// Scheduled (tt_Context_schedule()) while proxy has an outstanding gap (proxy->received_bitmap !=
// 0), re-sending the ACKNACK on a timer for the case where no further DATA ever arrives to
// re-trigger update_reliable_ack() itself. Mirrors call_retry()'s own schedule/reschedule/give-up
// shape. Scheduled against proxy's own stable address (not the owning Subscriber) so several
// writers' independent retry timers on the same Subscriber never collide - see struct tt_
// WriterProxy.sub's own doc comment for why this can still reach node/endpoint from just `proxy`.
static void acknack_retry(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(time);

    struct tt_WriterProxy* proxy = param;

    if (highest_relevant_bit(proxy) < 0) {
        // A DATA arrival already closed the gap since this timer was armed. By either signal, as
        // maybe_arm_acknack_retry() reads it: an empty received_bitmap is not enough, because a gap only a Heartbeat
        // reveals - a lost or declined tail, with nothing out of order recorded - has one. Testing the bitmap alone
        // (until 2026-10-09) gave such a gap one ACKNACK and then stopped, and if its repair was lost or declined
        // again, nothing asked until the writer sent something more; a writer that had stopped never did (rmw
        // test_loaned_messages, held case).
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
        proxy->sub->gap_abandoned++; // ack_seq_no itself, which is missing by definition
        proxy->acknack_scheduled = false;
        // Give up on ack_seq_no itself - the same "advance past it" advance_ack_seq_no() already
        // does for a real receipt, since from here on it makes no difference *why* nothing more
        // is waiting on it. A different, still-outstanding gap further ahead in the window (if
        // any) is untouched - it gets its own full tt_RELIABLE_RETRY budget against whatever
        // ack_seq_no ends up being next (advance_ack_seq_no()'s own reset of retry is what
        // actually grants that fresh budget) - and, unlike leaving it to the next DATA arrival to
        // notice, maybe_arm_acknack_retry() below starts requesting it immediately.
        advance_ack_seq_no(proxy);
        // advance_ack_seq_no() does not only step past the abandoned sample - it absorbs every
        // received sample contiguous behind it, which with ordered delivery are samples still
        // HELD in the reorder buffer. The watermark is now past them and they have not been
        // handed up. This path never drained, so they waited for the next in-order arrival, and
        // on a stream that had stopped they waited forever - while their slots stayed occupied
        // and a sample arriving a window later collided with one and was re-requested.
        drain_reorder(node, proxy->sub, proxy);
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
                       proxy->ack_seq_no, proxy->context_id, proxy->retry,
                       proxy->keep_all == tt_WRITER_KEEP_ALL_YES ? "KEEP_ALL" : "policy not yet known");
    }

    if (!tt_Context_schedule(node, tt_get_ns() + reliable_retry_interval(node, proxy), acknack_retry, proxy)) {
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
static void maybe_arm_acknack_retry(struct tt_Context* node, struct tt_WriterProxy* proxy) {
    if (highest_relevant_bit(proxy) < 0) {
        // No outstanding gap by either signal (received_bitmap or the last Heartbeat) - a healthy
        // stream needs no ACKNACK at all.
        if (proxy->acknack_scheduled) {
            tt_Context_unschedule(node, acknack_retry, proxy);
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
        if (tt_Context_schedule(node, tt_get_ns() + reliable_retry_interval(node, proxy), acknack_retry, proxy)) {
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
// How many of the first `span` positions of proxy's window - ack_seq_no and the span-1 after it -
// have already arrived. Used to count what a watermark move gives up on: span minus this is the
// number of samples skipped without ever being delivered. Positions past the window cannot have
// been recorded at all, so they count as not arrived, which is what they are.
static uint32_t received_in_first(const struct tt_WriterProxy* proxy, uint32_t span) {
    if (span == 0) {
        return 0;
    }
    uint16_t words = proxy_words(proxy);
    uint32_t window = proxy_window_bits(proxy);
    uint64_t mask[tt_RELIABLE_BITMAP_MAX_WORDS];
    bitmap_low_mask(mask, words, span >= window ? (int)window - 1 : (int)span - 1);
    uint32_t count = 0;
    for (uint16_t word = 0; word < words; word++) {
        count += (uint32_t)__builtin_popcountll(proxy->received_bitmap[word] & mask[word]);
    }
    return count;
}

static void jump_ack_baseline(struct tt_WriterProxy* proxy, uint32_t seq_no) {
#ifdef tt_RELIABLE_STATS
    if (seq_no > proxy->ack_seq_no) {
        uint64_t span = (uint64_t)seq_no - proxy->ack_seq_no;
        uint64_t received = rstat_popcount_bitmap(proxy->received_bitmap, proxy_words(proxy));
        g_rstats.jump_abandoned_seq += span > received ? span - received : 0;
    }
#endif
    if (seq_no > proxy->ack_seq_no) {
        uint32_t span = seq_no - proxy->ack_seq_no;
        proxy->sub->gap_abandoned += span - received_in_first(proxy, span);
    }
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
// The seq_nos a whole record's span covers beyond its own. A sample consumes the number of seq_nos
// its NETWORK form would need whatever path it takes (SHM_PLAN 6e), so a record that travelled
// whole over shared memory leaves the positions after its own permanently empty: the publisher
// allocated them to this same sample and no datagram will ever carry them. The reader has to stop
// waiting for them, and this is the only thing it may do with them.
//
// Set directly rather than through record_out_of_order_arrival(), on purpose: these are not
// arrivals. Routing them through it would move prev_highest and swallow the new_gap report for the
// genuinely missing samples below, which is how the reader asks for what it actually lost.
//
// Returns how many positions were absorbed, for the counter - a span that is being carried but not
// applied, and a span that is not being carried at all, are the same shape from outside.
static uint32_t absorb_seq_span(struct tt_WriterProxy* proxy, uint32_t first_offset, uint16_t span) {
    uint32_t absorbed = 0;
    for (uint16_t i = 1; i < span; i++) {
        uint64_t offset = (uint64_t)first_offset + i;
        if (offset >= proxy_window_bits(proxy)) {
            break; // past the window - the ordinary watermark machinery reaches it by itself
        }
        if (!bitmap_test_bit(proxy->received_bitmap, (uint32_t)offset)) {
            bitmap_set_bit(proxy->received_bitmap, (uint32_t)offset);
            absorbed++;
        }
    }
    return absorbed;
}

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
// A pure acknowledgement (no resend bits, "everything below ack_seq_no") that tells a writer this reader exists - once
// per writer (2026-10-05). A writer that has not received this reader's announce learns of it only from an ACKNACK
// (claim_from_acknack()), and a healthy reader otherwise never sends one, so a max-rate KEEP_ALL writer ran
// unmatched, evicting what it had broadcast, until the first loss made the reader ask (rig: 180-1230 samples per 20 s
// run at 5% loss). Sent at first contact for a writer announced KEEP_ALL, or not yet announced at all - waiting for
// its announce, which queues behind its DATA on this reader's socket, still left 1-10 lost per run; and when an
// announce turns a writer KEEP_ALL later. Never for a writer announced without KEEP_ALL: it evicts by design, and a
// healthy reader of one stays silent, as before. Requests nothing, so nothing is resent.
static void ack_writer_presence(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_WriterProxy* proxy) {
    if (proxy->presence_acked || !sub->reliable || proxy->sender_ip == 0) {
        return;
    }
    proxy->presence_acked = true;
    send_acknack_range(node, proxy, 0, -1);
}

static bool update_reliable_ack(struct tt_Context* node, struct tt_Subscriber* sub, uint32_t seq_no,
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

    if (first_contact && !sub->durable) {
        // Milestone 60 - RELIABLE+VOLATILE DDS-parity fix, mirrors inform_subscriber_of_heartbeat()'s
        // own identical first-contact branch (its own doc comment: "the actual DDS-parity fix this
        // whole follow-up is for"). DATA can legally win the race against the discovery-triggered
        // initial Heartbeat (send_initial_heartbeat(), unicast and never itself retried/acked)
        // arriving first, especially under loss injection - without this, first contact via DATA
        // fell through to the offset-based gap logic below starting from the stale ack_seq_no==1
        // default, misreading "everything before this first sample" as a recoverable in-flight gap
        // and ACKNACK-requesting a VOLATILE Publisher's own pre-match history it was never
        // obligated to keep - re-deriving the exact bug the Heartbeat-first path already fixed, any
        // time DATA happened to win that race instead.
        //
        // Phase 3 step 4 follow-up (2026-09-23) - and it applies to a VOLATILE Subscriber only,
        // which is what `!sub->durable` above is for. This branch used to run unconditionally, and
        // its own comment called the result an accepted residual: "an earlier backlog sample lost
        // in flight while a later one wins the race here". It isn't accepted any more, because it
        // was measured. On the HIL rig at 50% injected loss, a DURABLE KEEP_ALL stream started at
        // seq 3 in one run of three - the Publisher had pushed its whole retained range on match
        // (deliver_durability_backlog()), seq 1 and 2 were lost in flight, and this line then
        // pinned the baseline to whichever sample happened to survive, discarding the rest as
        // late_below_ack. A Subscriber that asked for TRANSIENT_LOCAL had the history it asked for
        // thrown away by the race that delivered it.
        //
        // Same RxO reasoning inform_subscriber_of_heartbeat() already spells out: THIS Subscriber's
        // requested durability decides the baseline, not the remote Publisher's offered one. A
        // volatile Subscriber explicitly does not want pre-match history, so pinning the baseline
        // to the first sample it sees is exactly right. A durable one does want it, so leaving
        // ack_seq_no where it is - at the default 1 - is what lets the ordinary ACKNACK exchange
        // go and fetch the range the Publisher is still holding for it.
        //
        // The obvious worry about that, stated rather than left implicit: a durable Subscriber
        // whose Heartbeat never arrives then sits at ack_seq_no == 1 and requests history. That is
        // what it asked for and what a durable Publisher is retaining anyway, so it is not a
        // spurious request - and it cannot hang, because a Publisher that no longer holds the
        // range answers with Phase 1-c's eviction Heartbeat and advance_past_unavailable() skips
        // exactly what is genuinely gone. tests/test_reliable_pubsub.c pins that termination.
        proxy->ack_seq_no = seq_no;
        proxy->reorder_cursor = seq_no;
    }
    if (first_contact && (proxy->keep_all == tt_WRITER_KEEP_ALL_YES ||
                          (proxy->keep_all == tt_WRITER_KEEP_ALL_UNKNOWN && node->discovery != NULL))) {
        ack_writer_presence(node, sub, proxy);
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
    if (node->rx_targeted) {
        note_repair_arrival(proxy, seq_no, tt_get_ns());
    }
    struct new_gap_range new_gap = {-1, -1};
    bool is_new = true;
    if (seq_no == proxy->ack_seq_no) {
        // Karn's ambiguity, and the reason for rx_targeted: the requested sample can arrive as the
        // retransmission the ACKNACK caused, or as its original, which was only late - overtaken by a
        // later sample on the other socket, say - and then arrives microseconds after the request. Timing
        // the second as a recovery put 12.6 us into the estimate under 20 ms of injected delay on the rig,
        // and with the ceiling at 64 x srtt that would have clamped real recoveries hard. Only a copy
        // addressed to this node is a retransmission, so only that one is timed; the original just ends
        // the probe. (The sequence number does say which REQUEST a copy answers - it cannot say which copy
        // arrived. The first version of this estimator assumed the one covered the other.)
        if (proxy->probe_ns != 0 && proxy->probe_seq_no == seq_no) {
            if (node->rx_targeted) {
                note_recovery_sample(proxy, tt_get_ns() - proxy->probe_ns);
            }
            proxy->probe_ns = 0;
        }
        // Before the advance, not after: bit j is "received(ack_seq_no + j)" and ack_seq_no is
        // still this sample's own number here, so the span's positions are bits 1..span-1.
        // advance_ack_seq_no() steps one and then absorbs the contiguous run that follows, which
        // is exactly these, landing the watermark on seq_no + span in one go.
        node->rx_span_absorbed += absorb_seq_span(proxy, 0, node->rx_seq_span);
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
            // After, not before, and this is the half that differs from the in-order case: the gap
            // report above is computed against the highest bit set so far, so absorbing the span
            // first would hide the real gap below this arrival and the reader would never ask for
            // what it genuinely lost.
            node->rx_span_absorbed += absorb_seq_span(proxy, (uint32_t)offset, node->rx_seq_span);
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
// An announce entry's node index in bits kind and qos do not use (stage 3, wire v11; tt_UPDATE_NODE_INDEX, tickle.h):
// index bits 0-3 are qos bits 4-7, index bits 4-7 are kind bits 2, 3, 6 and 7.
#define NODE_INDEX_QOS_SHIFT 4U
#define NODE_INDEX_HIGH_SHIFT 4U
#define KIND_INDEX_BIT_4 0x04U
#define KIND_INDEX_BIT_5 0x08U
#define KIND_INDEX_BIT_6 0x40U
#define KIND_INDEX_BIT_7 0x80U

static uint8_t node_index_kind_bits(uint8_t index) {
    uint8_t high = (uint8_t)(index >> NODE_INDEX_HIGH_SHIFT);
    return (uint8_t)(((high & 1U) != 0 ? KIND_INDEX_BIT_4 : 0U) | ((high & 2U) != 0 ? KIND_INDEX_BIT_5 : 0U) |
                     ((high & 4U) != 0 ? KIND_INDEX_BIT_6 : 0U) | ((high & 8U) != 0 ? KIND_INDEX_BIT_7 : 0U));
}

static uint8_t node_index_qos_bits(uint8_t index) {
    return (uint8_t)((index & tt_UPDATE_QOS_MASK) << NODE_INDEX_QOS_SHIFT);
}

static uint8_t node_index_of_entry(uint8_t kind, uint8_t qos) {
    uint8_t high = (uint8_t)(((kind & KIND_INDEX_BIT_4) != 0 ? 1U : 0U) | ((kind & KIND_INDEX_BIT_5) != 0 ? 2U : 0U) |
                             ((kind & KIND_INDEX_BIT_6) != 0 ? 4U : 0U) | ((kind & KIND_INDEX_BIT_7) != 0 ? 8U : 0U));
    return (uint8_t)((high << NODE_INDEX_HIGH_SHIFT) | (qos >> NODE_INDEX_QOS_SHIFT));
}

static int encode_update_entities(struct tt_Context* node, struct tt_Endpoint* const* endpoints,
                                  uint32_t endpoint_count) {
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
        // The node's index in spare bits (stage 3): no bytes added.
        update_entity->kind = (uint8_t)(endpoint->kind | node_index_kind_bits(endpoint->node_index));
        update_entity->qos = (uint8_t)(endpoint_qos_bits(endpoint) | node_index_qos_bits(endpoint->node_index));
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

// Bytes one endpoint's UpdateEntity takes on the wire: the fixed record plus its two strings, each
// a uint16 length and the bytes including '\0' (tt_encode_string()). Entities are packed with no
// padding between them, so this is exact wherever the entity lands. 0 for an endpoint
// encode_update_entities() would not encode at all.
static uint32_t update_entity_wire_size(struct tt_Endpoint* endpoint) {
    if (endpoint == NULL) {
        return 0;
    }
    const char* type = endpoint_type_name(endpoint);
    if (type == NULL || endpoint->name == NULL) {
        return 0;
    }
    return (uint32_t)(sizeof(struct tt_UpdateEntity) + (2 * sizeof(uint16_t)) +
                      _tt_strnlen(type, tt_MAX_STRING_LENGTH) + 1 + _tt_strnlen(endpoint->name, tt_MAX_STRING_LENGTH) +
                      1);
}

// Whether an announce submessage whose header-plus-body is `bytes` fits one datagram. The control
// limit, not tt_MAX_BUFFER_LENGTH: an announce has to reach nodes built with the default buffer,
// whatever this node was built with (tt_CONTROL_MAX_LENGTH, config.h).
static bool submessage_bytes_fit_datagram(uint32_t bytes) {
    return sizeof(struct tt_Header) + ROUNDUP(bytes) <= tt_CONTROL_MAX_LENGTH;
}

// Whether this node's whole announce fits one DATA: within one datagram, and within the 255 entities
// tt_AnnounceHeader.entity_count can say.
static bool update_fits_single(struct tt_Endpoint* const* endpoints, uint32_t endpoint_count) {
    uint32_t bytes =
        sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader) + sizeof(struct tt_AnnounceHeader);
    uint32_t entities = 0;
    for (uint32_t i = 0; i < endpoint_count; i++) {
        uint32_t size = update_entity_wire_size(endpoints[i]);
        if (size != 0) {
            bytes += size;
            entities++;
        }
    }
    return entities < UINT8_MAX && submessage_bytes_fit_datagram(bytes);
}

// Splits endpoints[] into announce fragments that each fit one datagram, in order: fragment p covers
// endpoints[part_start[p]] up to endpoints[part_start[p + 1]]. An endpoint whose entity could not
// fit even a fragment of its own is dropped from the announce (NULLed in endpoints[], counted and
// logged) rather than holding everything else back. Returns the fragment count, or 0 when more than
// tt_UPDATE_MAX_PARTS would be needed. Every fragment is planned with FRAG_FIRST's larger header, so
// the plan holds wherever a fragment lands.
static uint8_t plan_update_parts(struct tt_Context* node, struct tt_Endpoint** endpoints, uint32_t endpoint_count,
                                 uint32_t part_start[tt_UPDATE_MAX_PARTS + 1]) {
    const uint32_t part_overhead =
        sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_FragFirstHeader) + sizeof(struct tt_AnnounceHeader);
    uint32_t parts = 0;
    uint32_t bytes = part_overhead;
    uint32_t entities = 0;
    part_start[0] = 0;
    for (uint32_t i = 0; i < endpoint_count; i++) {
        uint32_t size = update_entity_wire_size(endpoints[i]);
        if (size == 0) {
            continue;
        }
        if (!submessage_bytes_fit_datagram(part_overhead + size)) {
            TT_LOG_ERROR("Endpoint '%s' needs %u bytes to announce, more than a datagram - not announced",
                         endpoints[i]->name, size);
            node->tx_dropped_oversize++;
            endpoints[i] = NULL;
            continue;
        }
        if (entities == UINT8_MAX || !submessage_bytes_fit_datagram(bytes + size)) {
            if (++parts == tt_UPDATE_MAX_PARTS) {
                return 0;
            }
            part_start[parts] = i;
            bytes = part_overhead;
            entities = 0;
        }
        bytes += size;
        entities++;
    }
    part_start[++parts] = endpoint_count;
    return (uint8_t)parts;
}

// This node's announce DataHeader (tt_DISCOVERY_ENDPOINT_ID, tickle.h): the built-in endpoint, and the
// generation that names this version of the endpoint list.
static void fill_announce_header(const struct tt_Context* node, struct tt_DataHeader* data_header) {
    data_header->endpoint_id = tt_DISCOVERY_ENDPOINT_ID;
    data_header->seq_no = (uint32_t)node->last_modified;
    data_header->timestamp = timestamp_to_wire(node->last_modified); // not read: the generation is seq_no
    data_header->entity_id = tt_DISCOVERY_ENTITY_ID;
}

// Sends this node's announce as the part_count (>= 2) fragments plan_update_parts() laid out, each
// flushed as its own datagram to the same destination the single announce would have gone to. Split at
// entity boundaries rather than by bytes: every fragment is FRAG_FIRST/FRAG_CONT framing, its own
// tt_AnnounceHeader and whole entities, so a receiver processes each one on arrival with no reassembly
// memory (tt_DISCOVERY_ENDPOINT_ID, tickle.h).
static bool send_update_parts(struct tt_Context* node, struct tt_Endpoint* const* endpoints,
                              const uint32_t part_start[tt_UPDATE_MAX_PARTS + 1], uint8_t part_count,
                              const struct tt_Peer* peers, uint8_t peer_count) {
    for (uint8_t part_no = 0; part_no < part_count; part_no++) {
        uint32_t old_tx_tail = node->tx_tail;
        uint8_t type = part_no == 0 ? tt_SUBMESSAGE_TYPE_FRAG_FIRST : tt_SUBMESSAGE_TYPE_FRAG_CONT;
        struct tt_SubmessageHeader* submessage_header = start_encode(node, type, tt_SUBMESSAGE_ID_ALL);
        if (submessage_header == NULL) {
            return false;
        }
        bool header_ok;
        if (part_no == 0) {
            struct tt_FragFirstHeader* first = encode(node, sizeof(struct tt_FragFirstHeader));
            header_ok = first != NULL;
            if (header_ok) {
                fill_announce_header(node, &first->data);
                first->frag_count = part_count;
            }
        } else {
            struct tt_FragContHeader* cont = encode(node, sizeof(struct tt_FragContHeader));
            header_ok = cont != NULL;
            if (header_ok) {
                cont->entity_id = tt_DISCOVERY_ENTITY_ID;
                cont->seq_no = (uint32_t)node->last_modified;
                cont->frag_index = part_no;
                cont->frag_count = part_count;
            }
        }
        struct tt_AnnounceHeader* announce = header_ok ? encode(node, sizeof(struct tt_AnnounceHeader)) : NULL;
        if (announce == NULL) {
            rollback(node, old_tx_tail);
            return false;
        }
        int entity_count = encode_update_entities(node, endpoints + part_start[part_no],
                                                  part_start[part_no + 1] - part_start[part_no]);
        if (entity_count < 0) {
            rollback(node, old_tx_tail);
            return false;
        }
        announce->entity_count = (uint8_t)entity_count;
        if (!end_encode(node, submessage_header, true, peers, peer_count)) {
            rollback(node, old_tx_tail);
            return false;
        }
        // end_encode() flushes what was pending ahead of a fragment that would not fit behind it and
        // keeps the fragment for the next flush. Each fragment is meant to go now, as its own datagram.
        if (node->tx_tail != sizeof(struct tt_Header) && !flush_tx(node, node->tx_tail, peers, peer_count)) {
            return false;
        }
    }
    return true;
}

// Builds this node's current announce (its own endpoint list, a DATA of the built-in discovery endpoint)
// and sends it either way it is needed: peer_count == 0 broadcasts it, batched (is_flush=false - a change
// pushed by announce_soon(), or a request answered in bulk, no synchronous waiter, node_flush()'s own tick
// is fine); peer_count >= 1 unicasts it to that one peer, flushed immediately (a first-contact reply from
// process_announce(), or a request answered by answer_discovery_request() - the whole point is the other
// side learning us as fast as possible).
static bool build_and_send_update(struct tt_Context* node, const struct tt_Peer* peers, uint8_t peer_count) {
    uint32_t old_tx_tail = node->tx_tail;

    // start_encode()/encode() below already log their own reason when they fail (e.g. "Lack of
    // tx buffer"), so none of these failure branches log again on top of that.

    // Header and SubmessageHeader
    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL);
    if (submessage_header == NULL) {
        return false;
    }

    struct tt_DataHeader* data_header = encode(node, sizeof(struct tt_DataHeader));
    struct tt_AnnounceHeader* announce = data_header != NULL ? encode(node, sizeof(struct tt_AnnounceHeader)) : NULL;
    if (announce == NULL) {
        rollback(node, old_tx_tail);
        return false;
    }
    fill_announce_header(node, data_header);
    announce->entity_count = 0;

    struct tt_Endpoint* endpoints[tt_MAX_ENDPOINT_COUNT + tt_MAX_NODES];
    uint32_t endpoint_count = node->endpoint_count;
    for (uint32_t i = 0; i < endpoint_count; i++) {
        endpoints[i] = node->endpoints[i];
    }
    endpoint_count += announced_nodes(node, endpoints + endpoint_count);

    // An announce too large for one datagram goes in fragments; anything that fits is one DATA.
    if (!update_fits_single(endpoints, endpoint_count)) {
        uint32_t part_start[tt_UPDATE_MAX_PARTS + 1];
        uint8_t part_count = plan_update_parts(node, endpoints, endpoint_count, part_start);
        if (part_count == 0) {
            TT_LOG_ERROR("Announce of %u endpoints needs more than %d fragments - not announced", endpoint_count,
                         tt_UPDATE_MAX_PARTS);
            node->tx_dropped_oversize++;
            rollback(node, old_tx_tail);
            return false;
        }
        if (part_count >= 2) {
            rollback(node, old_tx_tail);
            return send_update_parts(node, endpoints, part_start, part_count, peers, peer_count);
        }
        // One fragment: what is left once plan_update_parts() dropped endpoints no datagram could
        // carry fits a single DATA, and a one-fragment sample is not a valid fragment at all.
    }

    int entity_count = encode_update_entities(node, endpoints, endpoint_count);
    if (entity_count < 0) {
        rollback(node, old_tx_tail);
        return false;
    }
    announce->entity_count = (uint8_t)entity_count;

    bool is_flush = peer_count > 0;
    if (!end_encode(node, submessage_header, is_flush, peers, peer_count)) {
        rollback(node, old_tx_tail);
        return false;
    }

    if (!is_flush) {
        // This announce is now sitting batched in tx_buffer (or, rarely, was already flushed on
        // its own by end_encode()'s own overflow handling above) - either way it's
        // broadcast-only content that must not get swept into a unicast flush; node_flush()
        // clears this once it's actually sent, see flush_tx().
        node->tx_has_pending_update = true;
    }

    return true;
}

// The periodic discovery summary (tt_VERSION 8, rmw_tickle/DISCOVERY_PLAN.md): a HEARTBEAT of the built-in
// discovery endpoint whose first and last seq_no are this node's announce generation - ~28 bytes whatever the
// endpoint count, where the periodic announce it replaces carried the whole endpoint list every interval. A
// receiver that has applied this generation takes it as liveliness only; one that has not asks for the list
// (process_discovery_summary()). The full list is still broadcast at once on every change (announce_soon(),
// broadcast_goodbye()). Batched, as the announce was: it is broadcast-only content.
static void fill_summary(const struct tt_Context* node, struct tt_HeartbeatHeader* summary) {
    uint32_t generation = (uint32_t)node->last_modified;
    summary->endpoint_id = tt_DISCOVERY_ENDPOINT_ID;
    summary->first_available_seq_no = generation;
    summary->last_seq_no = generation;
    summary->entity_id = tt_DISCOVERY_ENTITY_ID;
    summary->flags = tt_HEARTBEAT_FLAG_FINAL;
    memset(summary->reserved, 0, sizeof(summary->reserved));
}

// The summary as a datagram of its own, built apart from tx_buffer, which may hold the very send it goes
// ahead of (note_reached(), tt_Context.summary_rides). Broadcast, as the batched one is.
static void send_summary_ahead(struct tt_Context* node) {
    tt_ALIGNAS(4) uint8_t
        datagram[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_HeartbeatHeader)];
    struct tt_Header* header = (struct tt_Header*)datagram;
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = node->id;
    struct tt_SubmessageHeader* submessage = (struct tt_SubmessageHeader*)(datagram + sizeof(struct tt_Header));
    submessage->type = tt_SUBMESSAGE_TYPE_HEARTBEAT;
    submessage->receiver = tt_SUBMESSAGE_ID_ALL;
    submessage->length = (uint16_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_HeartbeatHeader));
    fill_summary(
        node, (struct tt_HeartbeatHeader*)(datagram + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader)));
    uint32_t skip = to_single_form(datagram, (uint32_t)sizeof(datagram), 0);
    struct tx_datagram dgram = {datagram + skip, (uint32_t)sizeof(datagram) - skip, NULL, 0};
    (void)send_datagram(node, &dgram, NULL, 0);
    node->summaries_ridden++;
}

static void send_discovery_summary(struct tt_Context* node) {
    uint32_t old_tx_tail = node->tx_tail;
    struct tt_SubmessageHeader* submessage_header =
        start_encode(node, tt_SUBMESSAGE_TYPE_HEARTBEAT, tt_SUBMESSAGE_ID_ALL);
    struct tt_HeartbeatHeader* summary =
        submessage_header != NULL ? encode(node, sizeof(struct tt_HeartbeatHeader)) : NULL;
    if (summary == NULL) {
        rollback(node, old_tx_tail);
        return; // start_encode()/encode() logged why; the next interval tries again
    }
    fill_summary(node, summary);
    if (!end_encode(node, submessage_header, false, NULL, 0)) {
        rollback(node, old_tx_tail);
        return;
    }
    node->tx_has_pending_update = true; // broadcast-only, like the announce it replaces
    bool alone = (uint32_t)((uint8_t*)submessage_header - node->tx_buffer) == sizeof(struct tt_Header);
    node->tx_summary_alone_len = alone && node->summary_skip_armed ? node->tx_tail : 0;
}

// How often this node's summary goes out: every tt_CONTEXT_UPDATE_INTERVAL, or a tt_LIVELINESS_LEASE_DIVISOR-th of
// the shortest lease any
// of its own endpoints announces if that is sooner (LIVELINESS_PLAN.md amendment 1) - an idle node's summary
// is its only sign of life, so it must outpace the leases peers hold it to. At least tt_CONTEXT_TX_INTERVAL.
static uint64_t summary_interval(const struct tt_Context* node) {
    uint64_t interval = tt_CONTEXT_UPDATE_INTERVAL;
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        uint64_t lease = endpoint_liveliness_lease_duration_ns(node->endpoints[i]);
        if (lease != 0 && lease / tt_LIVELINESS_LEASE_DIVISOR < interval) {
            interval = lease / tt_LIVELINESS_LEASE_DIVISOR;
        }
    }
    return interval < tt_CONTEXT_TX_INTERVAL ? tt_CONTEXT_TX_INTERVAL : interval;
}

// Whether every peer this node knows of has had a datagram from it since the last summary tick, and clears
// the record for the next one. Each such datagram asserted this node's AUTOMATIC liveliness at its receiver
// (source_last_heard()), as the summary would have; MANUAL writers assert with their own DATA and HEARTBEAT.
// False with no peer known, so a node alone keeps announcing itself.
static bool every_peer_reached(struct tt_Context* node) {
    bool everyone = node->reached_everyone != 0;
    node->reached_everyone = 0;
    uint32_t reached[tt_MAX_CONTEXT_IDS / 32];
    for (int i = 0; i < tt_MAX_CONTEXT_IDS / 32; i++) {
        reached[i] = node->reached_nodes[i];
        node->reached_nodes[i] = 0;
    }
    bool any_peer = false;
    for (int i = 0; i < tt_MAX_CONTEXT_IDS; i++) {
        if (i == node->id || (!node->update_seen[i] && !update_parts_any(node, (uint8_t)i))) {
            continue;
        }
        any_peer = true;
        if (!everyone && (reached[i / 32] & (1U << (i % 32))) == 0) {
            return false;
        }
    }
    return any_peer;
}

static void node_update(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(param);
#if tt_SAMPLE_LENDING && tt_SEGMENT_ENABLED
    // A segment release put off while a retained sample held a slot, finished on the polling thread (DESIGN.md 10).
    (void)lend_finish_release(node);
#endif

    // At the short-lease cadence (LIVELINESS_PLAN.md 10) a summary is skipped when the node's own traffic
    // has already reached every peer since the last one: under traffic the last sign of life is then the
    // data, as with DDS, rather than a summary tens of ms after it. The tt_CONTEXT_UPDATE_INTERVAL summary, which
    // also carries the discovery generation, always goes.
    // That summary, when the traffic reaches every peer, rides just ahead of the next send rather than going
    // on its own, so it is not the last datagram before a node that stops.
    uint64_t interval = summary_interval(node);
    bool reached = every_peer_reached(node) && node->summary_skip_armed; // always cleared; unarmed, it recorded nothing
    node->summary_skip_armed = interval < tt_CONTEXT_UPDATE_INTERVAL;
    // A rider still waiting found no send since the last tick, so `reached` is false and it goes below.
    node->summary_rides = 0;
    bool keeps_the_second = interval >= tt_CONTEXT_UPDATE_INTERVAL || node->summary_sent_ns == 0 ||
                            time - node->summary_sent_ns + interval > tt_CONTEXT_UPDATE_INTERVAL;
    if (!reached) {
        send_discovery_summary(node);
        node->summary_sent_ns = time;
    } else if (keeps_the_second) {
        if (interval < tt_CONTEXT_UPDATE_INTERVAL) {
            node->summary_rides = 1;
        } else {
            send_discovery_summary(node);
        }
        node->summary_sent_ns = time;
    } else {
        node->summaries_skipped++;
    }

    node->next_summary_ns = time + interval;
    if (!tt_Context_schedule(node, node->next_summary_ns, node_update, NULL)) {
        TT_LOG_ERROR("Cannot schedule node_update");
    }
}

// Brings the next summary forward when an endpoint with a short lease has just appeared, so the first gap is
// not a whole tt_CONTEXT_UPDATE_INTERVAL.
static void reschedule_summary_for_leases(struct tt_Context* node, uint64_t now) {
    uint64_t due = now + summary_interval(node);
    if (node->next_summary_ns == 0 || due >= node->next_summary_ns) {
        return; // not running (a unit test's bare node), or already soon enough
    }
    (void)tt_Context_unschedule(node, node_update, NULL);
    if (tt_Context_schedule(node, due, node_update, NULL)) {
        node->next_summary_ns = due;
    } else {
        TT_LOG_ERROR("Cannot schedule node_update");
    }
}

// Phase 3 (rmw_tickle/PLAN.md) - records whether a remote writer promises KEEP_ALL on every local
// Subscriber already tracking it, from that writer's own announce (tt_UPDATE_QOS_KEEP_ALL). A
// proxy claimed later reads the same thing from the discovery table at first contact, so both
// orderings converge.
static void update_writer_proxies_keep_all(struct tt_Context* node, uint32_t endpoint_id, uint8_t node_id,
                                           uint32_t entity_id, enum tt_WriterKeepAll keep_all) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_SUBSCRIBER || endpoint->id != endpoint_id) {
            continue;
        }
        struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
        for (int j = 0; j < tt_MAX_PEER_COUNT; j++) {
            if (sub->writers[j].context_id == node_id && sub->writers[j].entity_id == entity_id) {
                struct tt_WriterProxy* proxy = &sub->writers[j];
                proxy->keep_all = keep_all;
                if (keep_all == tt_WRITER_KEEP_ALL_YES) {
                    ack_writer_presence(node, sub, proxy); // its DATA came before this announce: either order, once
                }
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
static void forget_writer_proxies_for_endpoint(struct tt_Context* node, uint32_t endpoint_id, uint8_t node_id,
                                               uint32_t entity_id, bool match_any_entity) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_SUBSCRIBER || endpoint->id != endpoint_id) {
            continue;
        }
        struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
        for (int j = 0; j < tt_MAX_PEER_COUNT; j++) {
            struct tt_WriterProxy* proxy = &sub->writers[j];
            if (proxy->context_id != node_id) {
                continue;
            }
            if (!match_any_entity && proxy->entity_id != entity_id) {
                continue;
            }
            if (proxy->acknack_scheduled) {
                tt_Context_unschedule(node, acknack_retry, proxy);
                proxy->acknack_scheduled = false;
            }
            proxy->context_id = tt_CONTEXT_ID_INVALID; // frees the slot; a restart re-runs first contact
            proxy->entity_id = 0;
            sub->superseded_pending -= proxy->superseded_pending < sub->superseded_pending ? proxy->superseded_pending
                                                                                           : sub->superseded_pending;
            proxy->superseded_pending = 0;
            proxy->ack_seq_no = 1;
            proxy->reorder_cursor = 1;
            proxy->highest_delivered = 0;
            proxy->heartbeat_last_seq_no = 0;
            proxy->retry = 0;
            proxy->keep_all = false;
            if (proxy->received_bitmap != NULL) {
                bitmap_clear(proxy->received_bitmap, proxy_words(proxy));
            }
            // Anything still held for this writer is never going to be delivered: what it was
            // waiting for was a gap this writer alone could have filled, and this writer is gone.
            // Freeing the slots matters more than the samples - a dead writer's held samples
            // would otherwise occupy the buffer for the lifetime of the Subscriber, and the only
            // symptom would be reorder_overflow rising on the writers that are still alive.
            release_reorder_slots_for_writer(node, sub, node_id, entity_id, match_any_entity);
            RSTAT_INC(proxies_dropped_liveliness);
        }
    }
}

// --- LIVELINESS (rmw_tickle/LIVELINESS_PLAN.md, 2026-09-26) --------------------------------------------
//
// Rule 1, the lease runs from the last sign of life: for a MANUAL_BY_TOPIC Publisher its own DATA or asserted
// liveliness (tt_DiscoveredEntity.last_asserted_ns); for any other entity any datagram from its node.
// Rule 2, one timer at the earliest expiry, not a once-a-second sweep: check_liveliness() below.
// Rule 3, a node is presumed dead only once it has been silent for tt_LIVELINESS_SILENCE_NS and for the
// longest lease any of its entities announced - a lease longer than the node-level limit is not cut short -
// but never longer than tt_CONTEXT_MAX_LEASE_NS, as a DDS participant lease bounds its writers'.

static bool entity_asserts_manually(const struct tt_DiscoveredEntity* entity) {
    return entity->kind == tt_KIND_TOPIC_PUBLISHER && (entity->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0;
}

// The last datagram of any kind from `source`: its summaries and announces, and all its other traffic.
static uint64_t source_last_heard(const struct tt_Context* node, uint8_t source) {
    uint64_t update = node->update_last_seen[source];
    uint64_t traffic = node->traffic_last_seen[source];
    return update > traffic ? update : traffic;
}

static uint64_t entity_lease_anchor(const struct tt_Context* node, const struct tt_DiscoveredEntity* entity) {
    return entity_asserts_manually(entity) ? entity->last_asserted_ns : source_last_heard(node, entity->context_id);
}

// Whether a leased entity's lease still holds at `now`.
static bool entity_within_lease(const struct tt_Context* node, const struct tt_DiscoveredEntity* entity, uint64_t now) {
    uint64_t anchor = entity_lease_anchor(node, entity);
    return now <= anchor || now - anchor <= entity->liveliness_lease_duration_ns;
}

// Recomputes tt_Context.liveliness_flags[source] from the discovery table. Called whenever an entity of
// `source` is added, removed, lapses or revives - rare events - so the per-datagram checks are one byte.
static void refresh_liveliness_flags(struct tt_Context* node, uint8_t source) {
    uint8_t flags = 0;
    if (node->discovery != NULL) {
        const struct tt_DiscoveredEntity* entities = node->discovery->entities;
        for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
            const struct tt_DiscoveredEntity* entity = &entities[i];
            if (entity->context_id != source || entity->liveliness_lease_duration_ns == 0) {
                continue;
            }
            if (entity_asserts_manually(entity)) {
                flags |= tt_LIVELINESS_SOURCE_MANUAL;
            }
            if (!entity->alive && node->update_seen[source]) {
                flags |= tt_LIVELINESS_SOURCE_LAPSED;
            }
        }
    }
    node->liveliness_flags[source] = flags;
}

// Makes sure check_liveliness() runs by `due_ns`: moves the one scheduled entry earlier if needed.
static void arm_liveliness_check(struct tt_Context* node, uint64_t due_ns) {
    if (node->liveliness_check_scheduled) {
        if (due_ns >= node->liveliness_check_ns) {
            return;
        }
        (void)tt_Context_unschedule(node, check_liveliness, NULL);
        node->liveliness_check_scheduled = false;
    }
    if (tt_Context_schedule(node, due_ns, check_liveliness, NULL)) {
        node->liveliness_check_scheduled = true;
        node->liveliness_check_ns = due_ns;
    } else {
        TT_LOG_ERROR("Cannot schedule check_liveliness");
    }
}

// Whether `entity`'s context still has another live entity of its kind on its endpoint - a second subscriber of the
// topic in that process. Scanned, not indexed: a lapse is rare.
static bool sibling_alive(const struct tt_Context* node, const struct tt_DiscoveredEntity* entity) {
    const struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* other = &entities[i];
        if (other != entity && other->alive && other->context_id == entity->context_id &&
            other->endpoint_id == entity->endpoint_id && other->kind == entity->kind) {
            return true;
        }
    }
    return false;
}

// A leased entity whose lease ran out while its node is still heard: tombstoned, and whatever this node
// kept for it goes - for it alone, not for a sibling of its topic in the same process.
static void lapse_entity(struct tt_Context* node, struct tt_DiscoveredEntity* entity) {
    entity->alive = false;
    // Phase 3 prerequisite (a), rmw_tickle/PLAN.md - a remote Subscriber presumed dead by its
    // own announced lease must also leave the matching local Publishers' peer/ack sets right
    // here, or under KEEP_ALL blocking a writer waiting on exactly that ack would stall. Narrow on
    // purpose: only the Publishers whose own endpoint id this entity matched, and only this node_id.
    // A peer is a context, so it stays while a sibling subscriber there is alive; only this reader's ack entry goes.
    bool one_entity = entity->entity_id != 0 && sibling_alive(node, entity);
    if (entity->kind == tt_KIND_TOPIC_SUBSCRIBER) {
        if (one_entity) {
            forget_publisher_acks_for_entity(node, entity->endpoint_id, entity->context_id, entity->entity_id);
        } else {
            forget_publisher_peers_for_endpoint(node, entity->endpoint_id, entity->context_id);
        }
    } else if (entity->kind == tt_KIND_TOPIC_PUBLISHER) {
        // Phase 3 - the mirror case: a remote *Publisher* past its own lease stops being
        // something our Subscribers can still recover from, so its WriterProxy goes too - its own, by entity_id
        // (every one of that context only for a writer announced without one).
        forget_writer_proxies_for_endpoint(node, entity->endpoint_id, entity->context_id, entity->entity_id,
                                           /*match_any_entity=*/entity->entity_id == 0);
    }
    if (node->discovery_callback != NULL) {
        node->discovery_callback(node, entity->context_id, entity->endpoint_id, entity->kind, /*departed=*/true,
                                 node->discovery_callback_param);
    }
}

// A lapsed entity showing a sign of life again - liveliness regained, as DDS reports it. Its peers come
// back with the next announce or data, as for any new match.
static void revive_entity(struct tt_Context* node, struct tt_DiscoveredEntity* entity, uint64_t now) {
    entity->alive = true;
    if (node->discovery_callback != NULL) {
        node->discovery_callback(node, entity->context_id, entity->endpoint_id, entity->kind, /*departed=*/false,
                                 node->discovery_callback_param);
    }
    arm_liveliness_check(node, now + entity->liveliness_lease_duration_ns + 1);
}

// Traffic from `source` revives its AUTOMATIC entities that lapsed (tt_LIVELINESS_SOURCE_LAPSED). Only
// called when that flag is set, so ordinary traffic pays one byte test.
static void revive_lapsed_entities(struct tt_Context* node, uint8_t source, uint64_t now) {
    if (node->discovery == NULL || !node->update_seen[source]) {
        return;
    }
    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        struct tt_DiscoveredEntity* entity = &entities[i];
        if (entity->context_id == source && !entity->alive && entity->liveliness_lease_duration_ns != 0 &&
            !entity_asserts_manually(entity)) {
            revive_entity(node, entity, now);
        }
    }
    refresh_liveliness_flags(node, source);
}

// A MANUAL_BY_TOPIC Publisher's sign of life: its DATA, or its HEARTBEAT with tt_HEARTBEAT_FLAG_LIVELINESS.
// Found by (source, endpoint_id, entity_id): the writer that asserted, not every one of its topic on that context -
// a sibling's DATA is no sign of this one's life. Only looked up when `source` has
// such a Publisher (tt_LIVELINESS_SOURCE_MANUAL).
static void note_manual_assertion(struct tt_Context* node, uint8_t source, uint32_t endpoint_id, uint32_t entity_id) {
    if ((node->liveliness_flags[source] & tt_LIVELINESS_SOURCE_MANUAL) == 0 || node->discovery == NULL) {
        return;
    }
#if tt_DISCOVERY_INDEXED
    // One entry per (source, endpoint_id), found through the index (CONTEXT_NODE_PLAN.md 4b) - this runs per DATA.
    int32_t slot = discovery_entity_slot_of(node->discovery, source, endpoint_id, entity_id);
    if (slot < 0) {
        return;
    }
    struct tt_DiscoveredEntity* entity = &node->discovery->entities[slot];
    if (!entity_asserts_manually(entity)) {
        return;
    }
#endif
    uint64_t now = rx_now(node);
#if tt_DISCOVERY_INDEXED
    entity->last_asserted_ns = now;
    if (!entity->alive && entity->liveliness_lease_duration_ns != 0 && node->update_seen[source]) {
        revive_entity(node, entity, now);
#else
    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    bool revived = false;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        struct tt_DiscoveredEntity* entity = &entities[i];
        if (entity->context_id != source || entity->endpoint_id != endpoint_id || entity->entity_id != entity_id ||
            !entity_asserts_manually(entity)) {
            continue;
        }
        entity->last_asserted_ns = now;
        if (!entity->alive && entity->liveliness_lease_duration_ns != 0 && node->update_seen[source]) {
            revive_entity(node, entity, now);
            revived = true;
        }
    }
    if (revived) {
#endif
        refresh_liveliness_flags(node, source);
    }
}

// Longest lease among the entities `source` announced, 0 if none or no discovery table.
static uint64_t longest_lease_from(const struct tt_Context* node, uint8_t source) {
    uint64_t longest = 0;
    if (node->discovery == NULL) {
        return 0;
    }
    const struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (entities[i].context_id == source && entities[i].liveliness_lease_duration_ns > longest) {
            longest = entities[i].liveliness_lease_duration_ns;
        }
    }
    return longest;
}

// A remote node silent past its limit is presumed gone - same forget_peers_from_source() peer-table cleanup a
// farewell announce would do, and the same update_seen[]/update_generation[] reset so a later summary or
// announce from the same node id is treated as first contact again. Its discovery-cache cleanup tombstones
// (tombstone_discovered_entities_from_source()) rather than forgets: a liveliness timeout is a *failure*,
// not the normal deletion RMW_EVENT_LIVELINESS_CHANGED.not_alive_count must exclude. Doesn't distinguish
// "crashed" from "partitioned" from "just slow" - none of those are observable from here, and DDS-style
// liveliness has the same limitation.
static void presume_node_dead(struct tt_Context* node, uint8_t source, uint64_t silent_ns) {
    // liveliness_deferrals_total rides along because this line is where a reader asks the question
    // it answers: was this a real death, or were we too starved to hear? Its own declaration says
    // it is "for a reader of the log" and it was not in any log, so the thing it promised could
    // not be checked anywhere. A large count beside a death means the check kept standing down
    // while datagrams went unread, and the silence may be ours rather than the peer's.
    TT_LOG_WARNING("Node %d presumed dead (silent for %lu ms, liveliness deferred %lu times so far)", source,
                   (unsigned long)(silent_ns / tt_MILLISECOND), (unsigned long)node->liveliness_deferrals_total);
    forget_peers_from_source(node, source, /*preserve_ack=*/false);
#if tt_SEGMENT_ENABLED
    // The departing edge for the segment, and deliberately only this one. A graceful farewell arrives
    // as an announce, which is indistinguishable here from the periodic refresh that also calls
    // forget_peers_from_source(preserve_ack=true) while the node is still very much alive - hooking
    // that would release and rebuild the segment under its peers once a second. A node that says
    // goodbye and goes then falls silent, so this path collects it a few seconds later instead. The
    // cost of that choice is holding a segment slightly longer than necessary; the cost of the other
    // one would be dropping it while somebody was writing to it.
    forget_same_host_peer(node, source);
#endif
    tombstone_discovered_entities_from_source(node, source);
    node->update_seen[source] = false;
    node->update_reprocess[source] = false;
    node->update_generation[source] = 0;
    node->update_last_seen[source] = 0;
    node->traffic_last_seen[source] = 0;
    update_parts_clear(node, source);
    node->liveliness_flags[source] = 0;
}

// The node-level half of check_liveliness(): presumes dead each remote node silent past its limit, and
// lowers *next to the earliest limit still ahead.
static void check_node_silence(struct tt_Context* node, uint64_t time, uint64_t unobserved_ns, uint64_t* next) {
    for (int i = 0; i < tt_MAX_CONTEXT_IDS; i++) {
        if (!node->update_seen[i] && !update_parts_any(node, (uint8_t)i)) {
            continue; // never heard from this node id at all - nothing to expire
        }
        // A node heard only through fragments of an announce it never finished (update_seen still false)
        // has entities recorded all the same, so it expires by the same clock as one that completed.
        uint64_t last = source_last_heard(node, (uint8_t)i);
        // The limit plus the time we were not watching. A peer is presumed gone after
        // tt_LIVELINESS_SILENCE_NS of silence WE OBSERVED; a window this node spent descheduled is not
        // silence we observed, and counting it is how a live peer gets declared dead on a loaded
        // machine. Added once per run and not accumulated: the next run is late only by its own
        // lateness, so a peer that really has gone is reported one ordinary interval later at worst.
        uint64_t limit = tt_LIVELINESS_SILENCE_NS + unobserved_ns;
        if (time > last && time - last > limit) {
            // Only for a node already that quiet. Capped as a DDS participant lease caps its writers': an
            // entity's lease cannot keep a silent node alive past tt_CONTEXT_MAX_LEASE_NS.
            uint64_t longest = longest_lease_from(node, (uint8_t)i);
            longest = longest < tt_CONTEXT_MAX_LEASE_NS ? longest : tt_CONTEXT_MAX_LEASE_NS;
            limit = longest > limit ? longest : limit;
        }
        if (time > last && time - last > limit) {
            presume_node_dead(node, (uint8_t)i, time - last);
            continue;
        }
        *next = last + limit + 1 < *next ? last + limit + 1 : *next;
    }
}

// The entity half: lapses each leased entity past its lease, and lowers *next to the earliest expiry ahead.
static void check_entity_leases(struct tt_Context* node, uint64_t time, uint64_t* next) {
    if (node->discovery == NULL) {
        return;
    }
    struct tt_DiscoveredEntity* entities = node->discovery->entities;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        struct tt_DiscoveredEntity* entity = &entities[i];
        if (entity->context_id == tt_CONTEXT_ID_INVALID || !entity->alive ||
            entity->liveliness_lease_duration_ns == 0) {
            continue;
        }
        if (!entity_within_lease(node, entity, time)) {
            lapse_entity(node, entity);
            refresh_liveliness_flags(node, entity->context_id);
            continue;
        }
        uint64_t expiry = entity_lease_anchor(node, entity) + entity->liveliness_lease_duration_ns + 1;
        *next = expiry < *next ? expiry : *next;
    }
}

// The liveliness timer (rule 2): runs at the earliest expiry among the remote nodes and leased entities this
// node tracks, acts on whatever has expired, and re-arms at the next one. A refresh only moves an expiry
// later, so nothing on the receive path re-arms it; when it fires early for that reason it finds nothing
// expired and re-arms. It also runs at least every tt_CONTEXT_UPDATE_INTERVAL, which picks up nodes heard for
// the first time.
static void check_liveliness(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(param);
    // How late this run is against the moment it was armed for, and the whole of why that matters:
    // **it is time during which this node was not running, so it cannot be evidence that anyone else
    // was silent.** A healthy scheduler is late by microseconds and this is noise; a descheduled
    // process is late by seconds, and without this it wakes with its own clock far advanced and
    // declares every peer dead for a window it spent not listening. Both sides of a starved pair do
    // it to each other, which is the shape of the CI failures on 2026-09-30 and 2026-10-01.
    //
    // Measured, not inferred: the scheduler entry knows when it was due. No clock heuristic, no
    // threshold to tune, and it degrades to nothing exactly when the node is healthy - which is the
    // property that keeps it a guard rather than a blanket amnesty.
    uint64_t unobserved_ns = 0;
    if (node->liveliness_check_scheduled && time > node->liveliness_check_ns) {
        unobserved_ns = time - node->liveliness_check_ns;
    }

    // Nothing is judged while datagrams are still unread: evidence in hand that has not been looked at.
    //
    // **This does NOT cover the descheduled case, and the commit that added it said it did. It was
    // wrong.** On Linux tt_rx_buffered() returns rx_count - rx_next, which counts datagrams already
    // pulled into our own batch by a previous recvmmsg() - not what the kernel is holding. A starved
    // process wakes with its peers' datagrams in the SOCKET, with rx_count == rx_next == 0, so this
    // condition is false exactly when the starvation case needs it. The tests did not catch that
    // because test_mock.h's tt_rx_buffered() means kernel-side availability instead, so four mutants
    // died against a condition that cannot be true in production. unobserved_ns above is the fix for
    // that case; this one keeps a narrower job of its own, which is real: drain_rx() takes at most
    // tt_RX_LOCK_CHUNK per lock, so a long batch really can still be part-drained when this runs.
    //
    // Capped, because "defer while busy" with no bound would let a saturated socket postpone a real
    // death for ever.
    if (tt_rx_buffered(node) > 0 && node->liveliness_deferrals < tt_LIVELINESS_MAX_DEFERRALS) {
        node->liveliness_deferrals++;
        node->liveliness_deferrals_total++;
        if (node->liveliness_check_scheduled) {
            (void)tt_Context_unschedule(node, check_liveliness, NULL);
            node->liveliness_check_scheduled = false;
        }
        arm_liveliness_check(node, time + tt_LIVELINESS_DEFER_NS);
        return;
    }
    node->liveliness_deferrals = 0;
    if (node->liveliness_check_scheduled) {
        // Run early, by a test or by a re-arm that lost a race with the entry itself: take the entry out,
        // this run re-arms.
        (void)tt_Context_unschedule(node, check_liveliness, NULL);
        node->liveliness_check_scheduled = false;
    }
    uint64_t next = time + tt_CONTEXT_UPDATE_INTERVAL;
    check_node_silence(node, time, unobserved_ns, &next);
    check_entity_leases(node, time, &next);
    arm_liveliness_check(node, next);
}

// This periodic tick only ever flushes batched pub/sub content - a DATA submessage from
// tt_Publisher_publish() and/or an announce from node_update() - never a CallResponse (that always
// flushes immediately from process_callrequest() itself instead). Broadcast is always correct
// for that content; unicasting it to a short list of known peers is only correct when (a) no
// announce is currently batched in there (it must reach the whole segment, not just a couple of
// peers - see tx_has_pending_update) and (b) there's exactly one Publisher on this node to
// attribute the batched DATA to (tx_buffer is shared across every endpoint on a node - mixing
// two Publishers' data in one unicast flush could send one's data to the other's peers). Both
// conditions hold for every one of this codebase's own examples (one Publisher per node); a node
// with 0 or 2+ Publishers, or one with an announce still pending, simply keeps broadcasting exactly
// as before this feature existed.
static void node_flush(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(param);
    UNUSED(time);
    node->flush_scheduled = false;

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

    // Re-armed only if something is still waiting - a send that failed - never just to tick.
    ensure_flush_scheduled(node);
}

// Sends every currently-retained sample (oldest first) straight to a Subscriber this Publisher's
// topic just discovered - QoS roadmap #4 (DURABILITY/TRANSIENT_LOCAL, rmw_tickle/PLAN.md). Called
// only from decode_update_entities() below when upsert_peer() just claimed a previously-empty
// peer slot for this exact Publisher - a genuinely new (or forgotten-then-rejoined) peer, not
// every periodic announce refresh. No-op unless pub->durable is set (VOLATILE, today's default) and
// pub->reliable_cache is non-NULL (nothing to deliver from otherwise) - matches process_acknack()'s
// own retransmit loop exactly, just unicasting to a fixed target instead of reacting to a NACK
// bitmap, and reading from the same shared cache (struct tt_ReliableCache's own doc comment).
// Sends one cached DATA record to target alone: copied into tx_buffer and flushed, as before, when it
// fits a datagram; as fragments read straight out of the cache when it does not. addressed stamps the
// target's id into the submessage header in place of the cached tt_SUBMESSAGE_ID_ALL - see
// retransmit_one_sample() for why a retransmission is addressed. The one path by which the reliable
// cache is ever sent, for a retransmission and for a durability backlog alike.
static bool send_cached_record(struct tt_Context* node, const uint8_t* record, uint16_t len, bool addressed,
                               const struct tt_Peer* target) {
#if tt_FRAG_ENABLED
    uint8_t type = ((const struct tt_SubmessageHeader*)record)->type;
    if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST || type == tt_SUBMESSAGE_TYPE_FRAG_CONT) {
        // One fragment, its own datagram and never padded - a fragment's length is what places it
        // (DESIGN.md). The headers are copied out so the receiver can be stamped without touching the
        // cached copy, which a durability backlog also sends; the payload goes straight from the arena.
        uint32_t header_length = sizeof(struct tt_SubmessageHeader) + (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST
                                                                           ? sizeof(struct tt_FragFirstHeader)
                                                                           : sizeof(struct tt_FragContHeader));
        uint8_t head[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_FragFirstHeader)];
        struct tt_Header* header = (struct tt_Header*)head;
        header->magic_value = NATIVE_MAGIC_VALUE;
        header->version = tt_VERSION;
        header->source = node->id;
        _tt_memcpy(head + sizeof(struct tt_Header), record, header_length);
        if (addressed) {
            ((struct tt_SubmessageHeader*)(head + sizeof(struct tt_Header)))->receiver = target->context_id;
        }
        uint32_t framing_length = (uint32_t)sizeof(struct tt_Header) + header_length;
        uint32_t skip = to_single_form(head, framing_length, len - header_length);
        struct tx_datagram dgram = {head + skip, framing_length - skip, record + header_length, len - header_length};
        return send_datagram(node, &dgram, target, 1);
    }
#endif
#if tt_FRAG_ENABLED
    // A whole record this destination cannot take. It was cached whole because the destination it was
    // PUBLISHED to could take it - a same-host peer with a wide enough slot - and that peer's segment
    // may have gone since. SHM_PLAN 6e's own table says this case re-fragments; until 2026-10-03 nothing
    // did, and end_encode() below would refuse it for ever ("the protocol does not fragment, so there is
    // nothing else to do with it"), which is the same failure as the oversize drop in a later disguise.
    //
    // Sent straight from the arena rather than through tx_buffer: send_fragments() takes pointers, so the
    // copy below is not needed, and going through send_tail_as_fragments() instead would flush whatever
    // else is batched - a broadcast, in the middle of an addressed retransmission.
    //
    // The seq_nos come out right by construction, which is what the seq span bought: frag_write_header()
    // gives fragment i the base seq_no + i, and the span reserved exactly that many when the sample was
    // published. A receiver cannot tell these fragments from ones the first publish would have sent.
    // Compared here rather than through submessage_fits_datagram(), which answers the same question but
    // treats a no as a failure: it logs an error and counts tx_dropped_oversize. Here a record that does
    // not fit is the ordinary signal to re-fragment, not something that could never be sent.
    if (sizeof(struct tt_Header) + ROUNDUP(len) > record_size_limit(node, tt_MAX_BUFFER_LENGTH, target, 1)) {
        const struct tt_DataHeader* cached = (const struct tt_DataHeader*)(record + sizeof(struct tt_SubmessageHeader));
        uint32_t head_len = (uint32_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader));
        return send_fragments(node, cached, record + head_len, len - head_len, target, 1,
                              addressed ? target->context_id : tt_SUBMESSAGE_ID_ALL);
    }
#endif
    uint32_t old_tx_tail = node->tx_tail;
    void* buf = encode(node, len);
    if (buf == NULL) {
        rollback(node, old_tx_tail);
        return false;
    }
    _tt_memcpy(buf, record, len);
    if (addressed) {
        ((struct tt_SubmessageHeader*)buf)->receiver = target->context_id;
    }
    if (!end_encode(node, (struct tt_SubmessageHeader*)buf, true, target, 1)) {
        rollback(node, old_tx_tail);
        return false;
    }
    return true;
}

static void deliver_durability_backlog(struct tt_Context* node, struct tt_Publisher* pub, struct tt_Peer* target) {
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
        if (!send_cached_record(node, cache->arena + cache_entry->offset, cache_entry->len, false, target)) {
            TT_LOG_WARNING("Cannot deliver durability backlog seq_no %u", cache_entry->seq_no);
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
// (learned from an announce) against every local Publisher sharing the announced topic
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
    // Milestone 58 - the generation of the announce this entity came in (tt_DISCOVERY_ENDPOINT_ID,
    // tickle.h), threaded down through decode_update_entities().
    // Phase 2 - the announcing entity's own entity_id and, for a Subscriber, the RELIABLE tracking
    // window it announced (tt_UpdateEntity.tracking_words). Both unused by
    // register_server_peer_on_client().
    uint32_t entity_id;
    uint16_t tracking_words;
    // Unused by register_server_peer_on_client() (Clients/Servers have no durability concept), only
    // meaningful to register_subscriber_peer_on_publisher()'s own durable_delivered[] check below.
    uint32_t announce_generation;
};

// A reliable Publisher learns of a Subscriber from its ACKNACK when its announce never arrived (2026-10-05). A reader
// only ACKNACKs a writer it has matched, after its own RxO check, so the ACKNACK is as good a match as the announce
// - and on the rig it was the only one that got through: a max-rate publisher broadcasting before it knows any
// peer floods its own well-known socket with its looped-back datagrams (the Pis grant 425,984 bytes of receive
// buffer), every one of the reader's announces and summaries was lost there in 14 of 14 unregistered runs, and
// KEEP_ALL with nobody to wait for evicted ~5% of samples under 5% loss while the reader's ACKNACKs arrived by
// unicast, thousands of them, and were ignored. Claiming the entry makes KEEP_ALL wait for this reader - never less
// than it did - and the peer makes the Publisher unicast to it, which ends the flood. Not for a reader that has
// just left (departed_acks, or tombstoned in discovery): a straggling ACKNACK must not revive it, or KEEP_ALL would
// wait for a reader that is gone. The reader's announced window is not known
// yet, so the entry keeps the default bound until its announce, which replaces it, arrives.
static void claim_from_acknack(struct tt_Context* node, struct tt_Publisher* pub, uint8_t source, uint32_t endpoint_id,
                               uint32_t sender_entity_id, uint32_t sender_ip, uint16_t sender_port) {
    if (!pub->reliable || sender_entity_id == 0 || find_peer_ack(pub, source, sender_entity_id) != NULL) {
        return;
    }
    if (node->discovery != NULL) {
        const struct tt_DiscoveredEntity* seen =
            tt_Discovery_find_entity(node->discovery, source, endpoint_id, sender_entity_id);
        if (seen != NULL && !seen->alive) {
            return;
        }
    }
    uint64_t now = tt_get_ns();
    for (int i = 0; i < tt_DEPARTED_ACKS; i++) {
        const struct tt_DepartedAck* departed = &pub->departed_acks[i];
        if (departed->context_id == source && (departed->entity_id == 0 || departed->entity_id == sender_entity_id) &&
            now - departed->at_ns < tt_DEPARTED_ACK_NS) {
            return; // it has just left: an ACKNACK that was in flight, not a reader to wait for
        }
    }
    if (claim_peer_ack(pub, source, sender_entity_id) == NULL) {
        return; // a full table: the same refusal register_subscriber_peer_on_publisher() makes
    }
    (void)upsert_peer(pub->peers, source, sender_ip, sender_port);
    TT_LOG_INFO("Publisher peer claimed from its ACKNACK: node %u entity %08x for endpoint %u (its announce has not "
                "arrived)",
                source, sender_entity_id, endpoint_id);
}

static void register_subscriber_peer_on_publisher(struct tt_Context* node, struct tt_Endpoint* endpoint,
                                                  void* ctx_ptr) {
    struct update_peer_ctx* ctx = (struct update_peer_ctx*)ctx_ptr;
    struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;

    // QoS roadmap #1 (RxO matching, Milestone 31) / #2 (DEADLINE) / #3 (LIVELINESS, Milestone 49) -
    // a remote Subscriber requesting a policy this local Publisher doesn't offer never becomes a
    // peer at all: no unicast optimization, no durability backlog, no discovery-triggered
    // Heartbeat - matching real DDS's own "an incompatible pair simply never connects" semantics.
    // See process_data()'s own subscriber_incompatible_with_writer() for this check's own
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
        bool newly_matched = find_peer_ack(pub, ctx->header->source, ctx->entity_id) == NULL;
        struct tt_PeerAck* ack = claim_peer_ack(pub, ctx->header->source, ctx->entity_id);
        if (ack == NULL) {
            TT_LOG_WARNING("Ack table full (%d entries) - not matching Subscriber %08x on node %d", tt_MAX_ACK_ENTRIES,
                           ctx->entity_id, ctx->header->source);
            return;
        }
        if (newly_matched && requested_reliable && !requested_durable) {
            arm_match_heartbeat(pub, ack); // the match point - see tt_PeerAck.first_owed_seq_no
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
        // The Publisher's half of the same question the writer-proxy log above answers from the
        // Subscriber's side: did the two ever actually find each other? Once per newly-claimed
        // peer slot, so this does not fire per sample.
        TT_LOG_INFO("Publisher peer registered: node %u at %u.%u.%u.%u:%u for endpoint %u", ctx->header->source,
                    (ctx->sender_ip >> 24) & 0xFF, (ctx->sender_ip >> 16) & 0xFF, (ctx->sender_ip >> 8) & 0xFF,
                    ctx->sender_ip & 0xFF, ctx->sender_port, ((struct tt_Endpoint*)pub)->id);
        // Milestone 58 - skip a redundant backlog re-delivery when this "genuinely new" peer slot
        // (check_liveliness()'s own presumed-dead cleanup, not necessarily a real departure - see
        // struct tt_DurableDeliveryRecord's own doc comment, tickle.h) already received this exact
        // announce's backlog. A real process restart lands on a different announce_generation
        // (durable_delivered_get() returns false), so it still gets delivered as usual.
        bool tracks_durable_delivery = pub->durable && pub->reliable_cache != NULL;
        bool already_delivered =
            tracks_durable_delivery &&
            durable_delivered_get(pub->reliable_cache, ctx->header->source, ctx->announce_generation);
        if (!already_delivered) {
            deliver_durability_backlog(node, pub, &target);
            if (tracks_durable_delivery) {
                durable_delivered_upsert(pub->reliable_cache, ctx->header->source, ctx->announce_generation);
            }
        }
        send_initial_heartbeat(node, pub, &target);
    }
}

// Milestone 35 - the SERVICE_SERVER-side mirror of register_subscriber_peer_on_publisher() above:
// every local Client sharing the announced service name learns this remote Server as a peer, not
// just the first. No QoS compatibility gate here (Clients/Servers have no RELIABLE/DURABLE
// policy to negotiate the way topics do), matching the original inline code this was lifted from.
static void register_server_peer_on_client(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    UNUSED(node);
    struct update_peer_ctx* ctx = (struct update_peer_ctx*)ctx_ptr;
    struct tt_Client* client = (struct tt_Client*)endpoint;
    upsert_peer(client->peers, ctx->header->source, ctx->sender_ip, ctx->sender_port);
}

static bool decode_update_entities(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t* head,
                                   uint32_t tail, int entity_count, uint32_t sender_ip, uint16_t sender_port,
                                   uint32_t generation) {
    bool reverse = tt_is_reverse_endian(header);
    for (int i = 0; i < entity_count && *head + sizeof(struct tt_UpdateEntity) + (2 * sizeof(uint16_t)) < tail; i++) {
        struct tt_UpdateEntity* update_entity = decode(node, buffer, head, tail, sizeof(struct tt_UpdateEntity));
        uint32_t endpoint_id = rd32(header, update_entity->endpoint_id);
        uint32_t remote_entity_id = rd32(header, update_entity->entity_id); // Phase 2
        uint16_t remote_tracking_words = rd16(header, update_entity->tracking_words);

        uint64_t deadline_duration_ns = rd64(header, update_entity->deadline_duration_ns);
        uint64_t liveliness_lease_duration_ns = rd64(header, update_entity->liveliness_lease_duration_ns);

        // The node's index, out of the spare bits it rides in, which are then cleared: everything below reads kind
        // and qos as they were before stage 3.
        uint8_t node_index = node_index_of_entry(update_entity->kind, update_entity->qos);
        update_entity->kind &= tt_UPDATE_KIND_MASK;
        update_entity->qos &= tt_UPDATE_QOS_MASK;

        TT_LOG_DEBUG("UpdateEntity");
        TT_LOG_DEBUG("  endpoint_id: %08x", endpoint_id);
        TT_LOG_DEBUG("  kind: %d", update_entity->kind);
        TT_LOG_DEBUG("  node_index: %u", node_index);

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
                                          .announce_generation = generation};
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
        // (tt_Context_set_discovery()) lists every remote entity a node has heard of, not just ones
        // this node itself can talk to.
        upsert_discovered_entity(node, header->source, endpoint_id, remote_entity_id, update_entity->kind, node_index,
                                 update_entity->qos, deadline_duration_ns, liveliness_lease_duration_ns, type, name);
    }

    return true;
}

// Unicasts our own current announce straight back to a peer we've just heard from for the
// first time - see process_announce()'s own comment on why. Terminates rather than looping forever
// because it only ever fires on that first contact: by the time this reply reaches the peer and
// it processes it, node->update_seen[our own source] on ITS side is already true - either from
// whatever announce got us onto its radar in the first place, or, in the simultaneous-startup
// case, from this very reply - so replying to a reply never meets this same trigger condition
// again on either side. Skipped (not a correctness issue, just a missed optimization
// this one time - the peer pulls our list when our next summary reaches it) whenever tx_buffer
// already has something else pending: redirecting that to a single peer here could be wrong for
// whatever else it's for (same shared-tx_buffer reasoning as process_callrequest()'s own unicast).
static void reply_with_own_announce(struct tt_Context* node, uint8_t sender_node_id, uint32_t sender_ip,
                                    uint16_t sender_port) {
    if (node->tx_tail != sizeof(struct tt_Header)) {
        return;
    }

    struct tt_Peer reply_to = {sender_node_id, sender_ip, sender_port};
    build_and_send_update(node, &reply_to, 1);
}

// More fragments than this build tracks. A function, not an inline comparison: at tt_UPDATE_MAX_PARTS 255 a uint8_t
// count can never exceed it, and the comparison written against the uint8_t would draw -Wtype-limits.
static bool update_parts_too_many(uint32_t part_count) {
    return part_count > tt_UPDATE_MAX_PARTS;
}

// Which fragments of a source's announce in progress have arrived: a bitmap of tt_UPDATE_PART_WORDS words, one bit
// per fragment (CONTEXT_NODE_PLAN.md 4a - it was one 32-bit word, which capped an announce at 32 fragments).
static bool update_parts_complete(const struct tt_Context* node, uint8_t source, uint8_t part_count) {
    for (uint32_t word = 0; word < tt_UPDATE_PART_WORDS; word++) {
        uint32_t first = word * 32U;
        uint32_t want = UINT32_MAX;
        if (part_count <= first) {
            want = 0U;
        } else if (part_count < first + 32U) {
            want = ((uint32_t)1 << (part_count - first)) - 1U;
        }
        if (node->update_part_received[source][word] != want) {
            return false;
        }
    }
    return true;
}

// A discovery announce (tt_DISCOVERY_ENDPOINT_ID, tickle.h), whole or one fragment of it: buffer[head..tail)
// is its tt_AnnounceHeader and entities. generation is its DataHeader/FragContHeader seq_no; frag_index
// and frag_count are 0 and 1 for an announce in one datagram.
//
// Liveliness is refreshed first, before anything can return: every announce and every fragment is proof
// the sender is alive, as every discovery summary is (process_discovery_summary()). Only then is the
// generation compared: a resend of an already-seen generation changes nothing.
//
// A new generation replaces what the source announced before (forget, then apply), as soon as its first
// datagram - whole or any fragment - arrives. Each fragment's entities are applied as it arrives, and a
// repeated fragment re-applies the same entities, which upsert makes harmless. Once every fragment has
// arrived the announce is complete: it becomes the source's acted-on announce (update_generation /
// update_seen), unmatched ack state is dropped, and a first contact is answered with our own announce.
// A lost fragment leaves the announce incomplete - its generation not applied - so the source's next
// summary draws a request, and the reply resends every fragment under the same generation, which fills the
// gap without starting over; until then the source is known by the fragments that did arrive.
static void drop_cached_responses_from_source(struct tt_Context* node, uint8_t source, bool farewell);

static void note_discovery_round_trip(struct tt_Context* node, uint8_t source, uint32_t generation);

static bool discovery_generation_applied(const struct tt_Context* node, uint8_t source, uint32_t generation);

static bool process_announce(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                             uint32_t tail, uint32_t sender_ip, uint16_t sender_port, uint32_t generation,
                             uint8_t frag_index, uint8_t frag_count) {
    uint8_t source = header->source;
    node->update_last_seen[source] = tt_get_ns();
#if tt_SEGMENT_ENABLED
    // Before the announce is decoded, because it is the address this datagram came from that decides
    // whether a segment could serve this peer, and that is known whether or not the body parses.
    note_same_host_peer(node, source, sender_ip);
#endif

    struct tt_AnnounceHeader* announce = decode(node, buffer, &head, tail, sizeof(struct tt_AnnounceHeader));
    if (announce == NULL) {
        TT_LOG_ERROR("Illegal AnnounceHeader");
        return false;
    }
    if (frag_count < 1 || update_parts_too_many(frag_count) || frag_index >= frag_count) {
        TT_LOG_ERROR("Illegal announce fragment %u of %u", frag_index, frag_count);
        return false;
    }

    TT_LOG_DEBUG("Announce");
    TT_LOG_DEBUG("  generation: %u", generation);
    TT_LOG_DEBUG("  fragment: %u of %u", frag_index, frag_count);
    TT_LOG_DEBUG("  entity_count: %u", announce->entity_count);

    if (discovery_generation_applied(node, source, generation)) {
        return true; // the periodic resend of the announce we last acted on
    }

    bool whole = frag_count == 1;
    if (whole || !update_parts_any(node, source) || node->update_part_generation[source] != generation ||
        node->update_part_count[source] != frag_count) {
        // A new announce from this source: it supersedes what it announced before (it may have dropped
        // an endpoint, or left entirely - see tt_Context_destroy()'s farewell announce). Forget its old
        // peer-table entries; decode_update_entities() below re-adds whatever it still lists.
        forget_peers_from_source(node, source, /*preserve_ack=*/true);
        forget_discovered_entities_from_source(node, source);
        node->update_part_generation[source] = generation;
        node->update_part_count[source] = frag_count;
        update_parts_clear(node, source);
    }

#if tt_DISCOVERY_INDEXED
    bool decoded = decode_update_entities(node, header, buffer, &head, tail, announce->entity_count, sender_ip,
                                          sender_port, generation);
    // Once for this fragment's entities, whether or not all of them decoded (upsert_discovered_entity()).
    refresh_liveliness_flags(node, source);
    if (!decoded) {
#else
    if (!decode_update_entities(node, header, buffer, &head, tail, announce->entity_count, sender_ip, sender_port,
                                generation)) {
#endif
        return false;
    }

    node->update_part_received[source][frag_index / 32U] |= (uint32_t)1 << (frag_index % 32U);
    if (!update_parts_complete(node, source, frag_count)) {
        return true; // more fragments to come
    }
    update_parts_clear(node, source);

    // Phase 3 prerequisite (c) - the forget above preserved this source's ack state so a re-added
    // Subscriber keeps it; now drop it wherever this announce genuinely dropped the match (an
    // endpoint it no longer lists, or a farewell listing nothing at all), so a departed Subscriber
    // can't hold a KEEP_ALL writer's ack set forever.
    drop_ack_state_for_unmatched_source(node, source);
    // The same for a Server's cached responses: a client this announce shows gone or replaced has none to retry.
    drop_cached_responses_from_source(node, source, whole && announce->entity_count == 0);

    // First time we've ever heard from this node, as opposed to it changing its endpoints since -
    // captured before update_seen[] is set, because that's the state reply_with_own_announce() needs
    // to not reply forever (see its own comment).
    bool is_first_contact_from_sender = !node->update_seen[source];
    note_discovery_round_trip(node, source, generation);
    node->update_generation[source] = generation;
    node->update_seen[source] = true;
    node->update_reprocess[source] = false;
    // A changed announce that came by broadcast is answered too (2026-09-26): the node that changed may
    // have just created an endpoint that matches one of ours, and until it hears our announce it cannot
    // match it - a Publisher of its would broadcast every sample for up to tt_CONTEXT_UPDATE_INTERVAL. Only a
    // broadcast is answered: a reply arrives unicast, on the data socket, so replies are never answered
    // and two nodes cannot trade announces back and forth. At most one reply per peer per change.
    if (is_first_contact_from_sender || !node->rx_via_data_port) {
        reply_with_own_announce(node, source, sender_ip, sender_port);
    }
    return true;
}

// Throttle for diagnostics on a per-sample path: true on the 1st, 10th, 100th ... occurrence.
// Rate-limiting by count rather than by time, because the interesting fact is that the thing
// happened at all and what it was, and a stream at a thousand samples a second buries the run's
// own output otherwise.
static bool is_power_of_ten(uint32_t count) {
    for (uint32_t step = 1; step <= count; step *= 10) {
        if (step == count) {
            return true;
        }
        if (step > count / 10) {
            break;
        }
    }
    return false;
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
// tt_Context_set_discovery() sees no behavior change from this milestone), or this Publisher hasn't
// been discovered yet (DATA arriving before its own first announce - a narrow startup
// race, not a genuine incompatibility; giving the benefit of the doubt here is strictly better
// than dropping a legitimately compatible pair's very first samples).
//
// It checks the writer that sent the sample (its entity_id), never the endpoint's first publisher: two writers of one
// topic in one context share the endpoint_id and can offer different QoS, and judged by the endpoint the first of
// them in the table answers for both. That was the segment drain's skip until 2026-10-09 (subscriber_refuses_writer()).
static bool writer_incompatible(struct tt_Subscriber* sub, const struct tt_DiscoveredEntity* publisher,
                                uint8_t publisher_node_id, uint32_t publisher_endpoint_id);
static bool writer_qos_incompatible(const struct tt_Subscriber* sub, const struct tt_DiscoveredEntity* publisher);

// The same judgement as subscriber_incompatible_with_writer() without counting or logging a drop: for the segment
// drain's skip, which asks whether a Subscriber will drop a record, not drops it. Counted there, every record of a
// refused writer that the plan met was counted as a drop once more than it was dropped.
static bool subscriber_refuses_writer(const struct tt_Context* node, const struct tt_Subscriber* sub, uint8_t source,
                                      uint32_t endpoint_id, uint32_t entity_id) {
    return node->discovery != NULL &&
           writer_qos_incompatible(sub, tt_Discovery_find_entity(node->discovery, source, endpoint_id, entity_id));
}

static bool writer_qos_incompatible(const struct tt_Subscriber* sub, const struct tt_DiscoveredEntity* publisher) {
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

static bool subscriber_incompatible_with_writer(struct tt_Context* node, struct tt_Subscriber* sub,
                                                uint8_t publisher_node_id, uint32_t publisher_endpoint_id,
                                                uint32_t publisher_entity_id) {
    if (node->discovery == NULL) {
        return false;
    }
    return writer_incompatible(
        sub, tt_Discovery_find_entity(node->discovery, publisher_node_id, publisher_endpoint_id, publisher_entity_id),
        publisher_node_id, publisher_endpoint_id);
}

static bool writer_incompatible(struct tt_Subscriber* sub, const struct tt_DiscoveredEntity* publisher,
                                uint8_t publisher_node_id, uint32_t publisher_endpoint_id) {
    if (!writer_qos_incompatible(sub, publisher)) {
        return false;
    }
    bool offered_reliable = (publisher->qos & tt_UPDATE_QOS_RELIABLE) != 0;
    bool offered_durable = (publisher->qos & tt_UPDATE_QOS_DURABLE) != 0;
    bool offered_manual = (publisher->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0;

    // Logged, because the drop itself is silent by design and that silence is indistinguishable
    // from "nobody is publishing". Throttled to the 1st, 10th, 100th ... drop rather than rate-
    // limited by time: the interesting fact is that it happened at all and what the mismatch was,
    // and a stream at a thousand samples a second would otherwise bury the run's own output.
    sub->rxo_drops++;
    if (is_power_of_ten(sub->rxo_drops)) {
        TT_LOG_WARNING("RxO mismatch: dropping DATA from node %u endpoint %u (drop #%u) - requested "
                       "reliable=%d durable=%d manual=%d lease=%luns deadline=%luns, offered reliable=%d "
                       "durable=%d manual=%d lease=%luns deadline=%luns",
                       publisher_node_id, publisher_endpoint_id, sub->rxo_drops, sub->reliable ? 1 : 0,
                       sub->durable ? 1 : 0, sub->liveliness_manual ? 1 : 0,
                       (unsigned long)sub->liveliness_lease_duration_ns, (unsigned long)sub->deadline_duration_ns,
                       offered_reliable ? 1 : 0, offered_durable ? 1 : 0, offered_manual ? 1 : 0,
                       (unsigned long)publisher->liveliness_lease_duration_ns,
                       (unsigned long)publisher->deadline_duration_ns);
    }
    return true;
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
    // A fragment's place in its sample (DATAFRAG_PLAN.md section 13), frag_count 0 for a whole DATA.
    uint8_t frag_index;
    uint8_t frag_count;
    // A sample the node's reassembly pool put together, for best-effort Subscribers only: a reliable one
    // took its fragments one by one, each under its own seq_no.
    bool best_effort_only;
};

// Milestone 35 - the actual per-Subscriber body process_data() used to run once (against find_
// endpoint()'s single match) before more than one local Subscription on the same topic became
// legal; now for_each_endpoint()'s own visitor, so it runs once per match - each with its own
// independent RxO compatibility check, reliable-ack state, and callback delivery, exactly as if
// each Subscriber had received its own private copy of the packet (which, semantically, it has:
// this is the same fan-out real DDS pub/sub gives every matched Subscriber for one Publisher).
// Records what was handed to the application callback, in the order it was handed up, and reports
// the first/10th/100th ... time that order was not what a reader is entitled to assume. See
// tt_Subscriber.delivered (tickle.h) for why this exists and why the three counters are separate.
//
// Called immediately before the callback rather than after, so the "previous" it compares against
// is the previous *delivered* sample and not merely the previous received one - a sample dropped
// by RxO matching or by de-duplication was never seen by the application and cannot be what it
// compared against.
static void record_delivery_order(struct tt_Context* node, struct tt_Subscriber* sub, uint32_t seq_no,
                                  uint64_t timestamp, uint8_t source, uint32_t entity_id, bool via_data_port) {
    // The arrival socket is passed in, not read off the node - see tt_ReorderSlot.via_data_port.
    bool first = (sub->delivered == 0);
    bool same_writer = !first && sub->last_source == source && sub->last_entity_id == entity_id;
    // seq_no counts per writer, so it means nothing across a switch of speaker.
    bool seq_back = same_writer && seq_no <= sub->last_seq_no;
    // Strictly older: since tt_VERSION 10 timestamps have microsecond precision, and two samples in one
    // microsecond are not out of order.
    bool time_back = !first && timestamp < sub->last_timestamp;

    if (!first && !same_writer) {
        sub->writer_switches++;
    }
    if (!first && via_data_port != sub->last_via_data_port) {
        sub->via_socket_flips++;
    }
    if (seq_back) {
        sub->out_of_order++;
    }
    if (time_back) {
        sub->timestamp_not_newer++;
    }

    if ((seq_back && is_power_of_ten(sub->out_of_order)) || (time_back && is_power_of_ten(sub->timestamp_not_newer))) {
        TT_LOG_WARNING("Delivery order: sample %u/%u from node %u entity %u ts %lu via %s follows "
                       "%u from node %u entity %u ts %lu via %s (delivered #%u, out_of_order %u, "
                       "timestamp_not_newer %u, writer_switches %u)",
                       seq_no, sub->endpoint.id, source, entity_id, (unsigned long)timestamp,
                       via_data_port ? "data" : "well-known", sub->last_seq_no, sub->last_source, sub->last_entity_id,
                       (unsigned long)sub->last_timestamp, sub->last_via_data_port ? "data" : "well-known",
                       sub->delivered + 1, sub->out_of_order, sub->timestamp_not_newer, sub->writer_switches);
    }

    sub->last_seq_no = seq_no;
    sub->last_source = source;
    sub->last_entity_id = entity_id;
    sub->last_timestamp = timestamp;
    sub->last_via_data_port = via_data_port;
    sub->delivered++;
    if (sub->keep_last_depth != 0) {
        node->rx_keep_last_delivered++; // the segment drain's cue to hand back (segment_skip_head())
    }
    // The samples of this writer the segment drain passed over just before this one, handed over with it. Looked up
    // only when there are any, so a Subscriber that never skips pays one compare.
    sub->delivering_superseded = 0;
    if (sub->superseded_pending != 0) {
        struct tt_WriterProxy* proxy = find_writer_proxy(sub, source, entity_id);
        if (proxy != NULL && proxy->superseded_pending != 0) {
            sub->delivering_superseded = proxy->superseded_pending;
            sub->superseded_pending -= proxy->superseded_pending < sub->superseded_pending ? proxy->superseded_pending
                                                                                           : sub->superseded_pending;
            proxy->superseded_pending = 0;
        }
    }
}

// TT_ORDERING_DISABLED - an experiment arm that restores pre-2026-09-24 delivery, off by default
// and not a knob for a deployment.
//
// Exists because both halves of the ordering work are only observable when they fail. With the
// BEST_EFFORT discard and the RELIABLE reorder buffer working, the application never sees a sample
// out of order - so a detector for that condition never fires, and "the detector is correct" is
// indistinguishable from "the detector is dead code". The only way to tell the two apart is to put
// the defect back and check that something notices.
//
// Same instrument as the socket-interleaving arm (TT_RX_FIXED_PREFERENCE, hal_linux.c): one -D
// between two builds of the same commit, so the comparison cannot be confounded by anything else.
#ifndef TT_ORDERING_DISABLED
#define TT_ORDERING_DISABLED 0
#endif

// Decode one wire payload and hand it to the application, recording the delivery order first.
// Split out of deliver_data_to_subscriber() so a sample released from the reorder buffer takes
// exactly the same path as one delivered straight off the wire - including the zero-copy decode,
// which a held sample is still eligible for because its bytes were copied verbatim.
static void deliver_payload(struct tt_Context* node, struct tt_Subscriber* sub, uint32_t seq_no, uint64_t timestamp,
                            uint8_t source, uint32_t entity_id, const uint8_t* payload, uint32_t length, bool is_native,
                            bool via_data_port, bool* out_decode_failed) {
    struct tt_Topic* topic = sub->topic;
#if tt_SAMPLE_LENDING
    // What tt_Sample_retain() may keep, for the length of the callback: the payload, and whose callback it is. On this
    // thread's stack, and the outer delivery put back afterwards - a callback may publish, and a local delivery nests.
    struct tt_LendDelivery lend = {sub, payload, length, is_native};
    const struct tt_LendDelivery* outer = node->lend.delivery;
#endif

    // Zero-copy path: hand the callback a tt_Data* aliasing the payload directly, skipping the
    // decode-into-scratch copy and the matching data_free. Falls through to the copy path when
    // the topic doesn't offer it or it declines (e.g. byte-swapped wire).
    if (topic->data_decode_inplace != NULL) {
        struct tt_Data* inplace = topic->data_decode_inplace(payload, length, is_native);
        if (inplace != NULL) {
            record_delivery_order(node, sub, seq_no, timestamp, source, entity_id, via_data_port);
            sub->delivering_source = source;
            sub->delivering_entity_id = entity_id;
#if tt_SAMPLE_LENDING
            node->lend.delivery = &lend;
#endif
            sub->callback(sub, timestamp, (uint16_t)seq_no, inplace);
#if tt_SAMPLE_LENDING
            node->lend.delivery = outer;
#endif
            return;
        }
    }

    uint8_t data[topic->data_size];
    int32_t decoded = topic->data_decode((struct tt_Data*)data, payload, length, is_native);
    if (decoded < 0) {
        TT_LOG_ERROR("Cannot decode data for endpoint_id: %08x, seq_no: %u", sub->endpoint.id, seq_no);
        if (out_decode_failed != NULL) {
            *out_decode_failed = true;
        }
        return;
    }

    record_delivery_order(node, sub, seq_no, timestamp, source, entity_id, via_data_port);
    sub->delivering_source = source;
    sub->delivering_entity_id = entity_id;
#if tt_SAMPLE_LENDING
    node->lend.delivery = &lend;
#endif
    sub->callback(sub, timestamp, (uint16_t)seq_no, (struct tt_Data*)data);
#if tt_SAMPLE_LENDING
    node->lend.delivery = outer;
#endif
    topic->data_free((struct tt_Data*)data);
}

#if tt_LOCAL_DELIVERY
// ---- (g9, config.h's tt_LOCAL_DELIVERY) samples between endpoints of one context.

// Whether a local pair can never match: what the Subscriber requests and the Publisher does not offer, by the rule a
// remote pair is held to (subscriber_incompatible_with_writer()).
static bool local_pair_incompatible(const struct tt_Subscriber* sub, const struct tt_Publisher* pub) {
    return (sub->reliable && !pub->reliable) || (sub->durable && !pub->durable) ||
           deadline_liveliness_incompatible(sub->deadline_duration_ns, pub->deadline_duration_ns,
                                            sub->liveliness_manual, pub->liveliness_manual,
                                            sub->liveliness_lease_duration_ns, pub->liveliness_lease_duration_ns);
}

struct local_delivery {
    struct tt_Publisher* pub;
    const uint8_t* payload;
    uint32_t length;
    uint32_t seq_no;
    uint64_t timestamp;
    struct tt_Subscriber* only; // the one Subscriber to deliver to (the backlog), or NULL for every one
};

static void deliver_locally_to(struct tt_Context* node, struct tt_Endpoint* endpoint, void* param) {
    struct local_delivery* delivery = (struct local_delivery*)param;
    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
    if ((delivery->only != NULL && delivery->only != sub) || local_pair_incompatible(sub, delivery->pub)) {
        return;
    }
    bool decode_failed = false;
    deliver_payload(node, sub, delivery->seq_no, delivery->timestamp, node->id, delivery->pub->endpoint.entity_id,
                    delivery->payload, delivery->length, true, false, &decode_failed);
}

// A sample this context just published, handed to its own Subscribers on the topic - after it has gone to the link,
// from a copy the caller owns, so a callback may publish again. In publish order and once: the context's own
// datagrams are dropped as self when they come back, so this is the only way a local Subscriber gets it.
static void deliver_locally(struct tt_Context* node, struct tt_Publisher* pub, const uint8_t* payload, uint32_t length,
                            uint32_t seq_no, uint64_t timestamp) {
    struct local_delivery delivery = {pub, payload, length, seq_no, timestamp, NULL};
    node->local_delivery_depth++;
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, pub->endpoint.id, deliver_locally_to, &delivery);
    node->local_delivery_depth--;
}

// One cached sample of a durable Publisher, whole: a DATA record as it is, or a fragmented one gathered from its
// FRAG_FIRST and FRAG_CONT records into `out` (tt_MAX_SAMPLE_LENGTH). Returns the number of seq_nos it took (0: none
// here), and its payload through *payload / *length (NULL when a fragment is missing or expired).
static uint32_t cached_sample(struct tt_ReliableCache* cache, uint16_t depth, uint32_t seq_no, uint64_t lifespan_ns,
                              uint8_t* out, const uint8_t** payload, uint32_t* length, uint64_t* sent_us) {
    *payload = NULL;
    struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, seq_no);
    if (entry->len == 0 || entry->seq_no != seq_no || reliable_cache_entry_expired(entry, lifespan_ns)) {
        return 1;
    }
    const uint8_t* record = cache->arena + entry->offset;
    const struct tt_SubmessageHeader* submessage = (const struct tt_SubmessageHeader*)record;
    if (submessage->type == tt_SUBMESSAGE_TYPE_DATA) {
        const struct tt_DataHeader* data_header = (const struct tt_DataHeader*)(submessage + 1);
        uint32_t headers = (uint32_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader));
        *payload = record + headers;
        *length = submessage->length > headers ? submessage->length - headers : 0;
        *sent_us = data_header->timestamp;
        return 1;
    }
#if tt_FRAG_ENABLED
    if (submessage->type != tt_SUBMESSAGE_TYPE_FRAG_FIRST) {
        return 1; // a continuation whose first fragment is gone
    }
    const struct tt_FragFirstHeader* first = (const struct tt_FragFirstHeader*)(submessage + 1);
    uint32_t count = first->frag_count;
    uint32_t total = 0;
    for (uint32_t index = 0; index < count; index++) {
        struct tt_ReliableCacheIndex* part = reliable_cache_slot(cache, depth, seq_no + index);
        if (part->len == 0 || part->seq_no != seq_no + index) {
            return count; // a fragment is missing: the sample is not whole
        }
        const struct tt_SubmessageHeader* part_header =
            (const struct tt_SubmessageHeader*)(cache->arena + part->offset);
        uint32_t header_length = frag_header_length(index);
        uint32_t part_length = part_header->length - header_length;
        if (total + part_length > tt_MAX_SAMPLE_LENGTH) {
            return count;
        }
        _tt_memcpy(out + total, cache->arena + part->offset + header_length, part_length);
        total += part_length;
    }
    *payload = out;
    *length = total;
    *sent_us = first->data.timestamp;
    return count;
#else
    UNUSED(out);
    return 1;
#endif
}

static void deliver_backlog_from(struct tt_Context* node, struct tt_Endpoint* endpoint, void* param) {
    struct tt_Subscriber* sub = (struct tt_Subscriber*)param;
    struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
    if (!pub->durable || pub->reliable_cache == NULL || local_pair_incompatible(sub, pub)) {
        return;
    }
    struct tt_ReliableCache* cache = pub->reliable_cache;
    uint16_t depth = reliable_cache_depth(cache);
    if (depth == 0) {
        return;
    }
    uint8_t* whole = node->local_scratch; // a fragmented sample gathered, under the context's lock
    for (uint32_t seq_no = cache->oldest_seq_no; seq_no != 0 && seq_no <= cache->newest_seq_no;) {
        const uint8_t* payload = NULL;
        uint32_t length = 0;
        uint64_t sent_us = 0;
        uint32_t taken =
            cached_sample(cache, depth, seq_no, pub->lifespan_duration_ns, whole, &payload, &length, &sent_us);
        if (payload != NULL) {
            // The wire's 32-bit microseconds, placed as a receiver places them (timestamp_from_wire()), against now.
            int64_t now_us = (int64_t)(tt_get_ns() / tt_MICROSECOND);
            int64_t sent_at_us = now_us + (int32_t)((uint32_t)sent_us - (uint32_t)now_us);
            uint64_t sent_ns = sent_at_us < 0 ? 0 : (uint64_t)sent_at_us * tt_MICROSECOND;
            struct local_delivery delivery = {pub, payload, length, seq_no, sent_ns, sub};
            deliver_locally_to(node, &sub->endpoint, &delivery);
        }
        seq_no += taken;
    }
}

void tt_Subscriber_deliver_local_backlog(struct tt_Subscriber* sub) {
    if (sub == NULL || sub->node == NULL) {
        return;
    }
    struct tt_Context* node = sub->node;
    state_lock(node);
    if (sub->durable) {
        node->local_delivery_depth++; // a callback publishing now must not take the scratch a fragment is gathered in
        for_each_endpoint(node, tt_KIND_TOPIC_PUBLISHER, sub->endpoint.id, deliver_backlog_from, sub);
        node->local_delivery_depth--;
    }
    state_unlock(node);
}
#endif

// The stride is the caller's slot size rounded DOWN to 8, and it must round down.
//
// Every slot after the first has to stay aligned for its uint64_t header, so the stride has to be
// a multiple of 8. This first rounded UP - and that walked off the end of the caller's buffer.
// A caller sizes its storage as slots * reorder_slot_bytes; with a stride larger than
// reorder_slot_bytes, the last slots start past that end, and core both read them (treating
// whatever bytes it found as "occupied" held samples) and wrote held samples into them. The
// perf_hil examples used 116-byte slots, so core addressed 120, and 4096 slots overran their
// array by ~16KB into the adjacent globals. AddressSanitizer: global-buffer-overflow in
// drain_reorder(), located just before g_cpu_place.
//
// It surfaced as a "leak" first: a diagnostic found slots belonging to a writer with node 255,
// entity 0xffffffff and seq 0xffffffff - every byte 0xFF - which is not a writer, it is the
// neighbouring memory's own contents read as if it were a slot. rmw_tickle escaped by luck: its
// 1496-byte slots are already a multiple of 8, so rounding changed nothing.
//
// Rounding down keeps every access inside slots * reorder_slot_bytes by construction. It costs at
// most 7 bytes of payload capacity per slot, which reorder_payload_capacity() reports, so a
// payload that no longer fits is treated as a full buffer rather than overrunning.
#define REORDER_SLOT_ALIGN ((uint16_t)sizeof(uint64_t))

static uint16_t reorder_stride(const struct tt_Subscriber* sub) {
    return (uint16_t)(sub->reorder_slot_bytes & ~(uint16_t)(REORDER_SLOT_ALIGN - 1U));
}

static struct tt_ReorderSlot* reorder_slot_at(struct tt_Subscriber* sub, uint16_t index) {
    return (struct tt_ReorderSlot*)(sub->reorder_storage + ((size_t)index * reorder_stride(sub) / sizeof(uint64_t)));
}

// The slot a writer's seq_no maps to: its seq_no modulo the slots, offset per writer. Every writer counts
// from 1, so without the offset two writers at similar seq_no contend for the same slots - which for DATA
// only costs an out-of-order sample a re-request, but a fragment is held even in order, until its sample
// is whole (DATAFRAG_PLAN.md section 13), and two writers' in-order fragments would collide constantly.
// One writer's held datagrams still lie within one window, so a buffer a window wide never collides
// with itself.
static struct tt_ReorderSlot* reorder_writer_slot(struct tt_Subscriber* sub, uint8_t node_id, uint32_t entity_id,
                                                  uint32_t seq_no) {
    // Knuth's multiplicative hash constants: they spread consecutive ids across the whole ring.
    const uint32_t entity_spread = 2654435761U;
    const uint32_t node_spread = 40503U;
    uint32_t salt = (entity_id * entity_spread) ^ ((uint32_t)node_id * node_spread);
    return reorder_slot_at(sub, (uint16_t)((seq_no + salt) % sub->reorder_slots));
}

static uint8_t* reorder_slot_payload(struct tt_ReorderSlot* slot) {
    return (uint8_t*)slot + sizeof(struct tt_ReorderSlot);
}

// How many payload bytes one slot can hold, or 0 if this Subscriber has no usable buffer.
static uint32_t reorder_payload_capacity(const struct tt_Subscriber* sub) {
    if (sub->reorder_storage == NULL || sub->reorder_slots == 0 ||
        sub->reorder_slot_bytes <= sizeof(struct tt_ReorderSlot)) {
        return 0;
    }
    return (uint32_t)reorder_stride(sub) - (uint32_t)sizeof(struct tt_ReorderSlot);
}

#if tt_FRAG_ENABLED
// The in-order fast path's way back to the reorder buffer (struct tt_Context.frag_fast_sub). Moves the
// fragments already put together in frag_scratch into their own reorder slots, exactly as the ordinary path
// would have stored them, and points the writer's reorder_cursor at the sample's start so the next drain
// walks them. Called before anything that would disturb the sample: another use of frag_scratch, a store
// into its Subscriber's reorder buffer, or a watermark move the fast path did not make.
//
// It cannot fail, and it has to be unable to: these fragments are acknowledged, and an acknowledged
// fragment is never sent again. The sample's Subscriber had nothing held when the sample started
// (frag_fast_take()), and every store into its buffer since moved this sample out first - so every slot
// it needs is free, and frag_fast_take() checked each fragment against the slot size before taking it.
static void frag_fast_spill(struct tt_Context* node) {
    struct tt_Subscriber* sub = node->frag_fast_sub;
    if (sub == NULL) {
        return;
    }
    node->frag_fast_sub = NULL;
    const uint8_t* bytes = node->frag_scratch + 4;
    uint32_t offset = 0;
    for (uint32_t index = 0; index < node->frag_fast_placed; index++) {
        uint32_t length = index == 0 ? node->frag_fast_first_length : node->frag_fast_cont_length;
        uint32_t seq_no = node->frag_fast_seq_no + index;
        struct tt_ReorderSlot* slot =
            reorder_writer_slot(sub, node->frag_fast_context_id, node->frag_fast_entity_id, seq_no);
        if (slot->occupied) {
            // Unreachable by the argument above. If it is ever reached the sample is short one fragment
            // below the watermark, which the drain abandons and counts - never delivered torn.
            TT_LOG_ERROR("Subscriber %u: reorder slot for fragment seq_no %u already taken - its sample is lost",
                         sub->endpoint.id, seq_no);
            offset += length;
            continue;
        }
        slot->seq_no = seq_no;
        slot->timestamp = index == 0 ? node->frag_fast_timestamp : 0;
        slot->entity_id = node->frag_fast_entity_id;
        slot->context_id = node->frag_fast_context_id;
        slot->length = (uint16_t)length;
        slot->is_native = node->frag_fast_is_native;
        slot->via_data_port = node->frag_fast_via_data_port;
        slot->frag_index = (uint8_t)index;
        slot->frag_count = node->frag_fast_count;
        slot->occupied = true;
        _tt_memcpy(reorder_slot_payload(slot), bytes + offset, length);
        offset += length;
        sub->reorder_held++;
        if (sub->reorder_held > sub->reorder_held_peak) {
            sub->reorder_held_peak = sub->reorder_held;
        }
    }
    struct tt_WriterProxy* proxy = find_writer_proxy(sub, node->frag_fast_context_id, node->frag_fast_entity_id);
    if (proxy != NULL) {
        proxy->reorder_cursor = node->frag_fast_seq_no;
    }
}

// Whether the fast path holds part of a sample of this Subscriber's (from any writer, when proxy is NULL).
static bool frag_fast_holds(const struct tt_Context* node, const struct tt_Subscriber* sub,
                            const struct tt_WriterProxy* proxy) {
    return node->frag_fast_sub == sub && (proxy == NULL || (node->frag_fast_context_id == proxy->context_id &&
                                                            node->frag_fast_entity_id == proxy->entity_id));
}

// Before a drain: the watermark moved under a sample the fast path holds part of - a gap given up on, or a DATA
// where a fragment was expected (recorded before the drain) - so it can no longer complete there. In the buffer,
// the drain deals with it.
static void frag_fast_spill_if_overtaken(struct tt_Context* node, const struct tt_Subscriber* sub,
                                         const struct tt_WriterProxy* proxy) {
    if (frag_fast_holds(node, sub, proxy) && proxy->ack_seq_no != node->frag_fast_seq_no + node->frag_fast_placed) {
        frag_fast_spill(node);
    }
}
#endif

// Hold a sample that arrived ahead of a gap, or - if it cannot be held - un-receive it so the
// ordinary ACKNACK exchange fetches it again later.
//
// That fallback is what makes a Subscriber with no buffer at all still correct rather than lossy:
// clearing the bit is a deliberate statement that this sample has NOT been received, which is
// true from the application's point of view, since it was never delivered and nothing is keeping
// it. Leaving the bit set and dropping the payload would silently lose the sample forever, which
// is the one outcome a RELIABLE reader must never produce.
static void hold_for_reorder(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_WriterProxy* proxy,
                             struct data_delivery_ctx* ctx, bool is_native) {
#if tt_FRAG_ENABLED
    if (frag_fast_holds(node, sub, NULL)) {
        frag_fast_spill(node); // this buffer is about to hold something, and it must be empty for that
    }
#endif
    uint32_t length = ctx->tail - ctx->head;
    uint32_t capacity = reorder_payload_capacity(sub);

    // Direct index, O(1): a sample goes in slot seq % slots. Every other version of this walked
    // every slot for every out-of-order arrival, which made the cost of a received sample scale
    // with the buffer's CAPACITY rather than with what it held - so sizing the buffer to the widest
    // window, the one thing that makes overflow impossible, cut reliable throughput by 88% on the
    // HIL rig (84.6 -> 10.0 Mbps at 0% loss) while holding ~255 samples in 4096 slots.
    //
    // A writer's held samples all lie within one tracking window of reorder_cursor, so when the
    // buffer is at least a window wide no two of them share a slot. A slot already taken by a
    // different sample - another writer's, or this writer's when the buffer is narrower than the
    // window - is treated exactly like a full buffer: re-requested, never overwritten.
    if (capacity >= length) {
        struct tt_ReorderSlot* slot = reorder_writer_slot(sub, ctx->header->source, ctx->entity_id, ctx->seq_no);
        if (slot->occupied) {
            // Already holding this exact sample: a retransmit racing the original. Keep the copy
            // already held - same bytes, and a second copy would be delivered twice.
            if (slot->seq_no == ctx->seq_no && slot->context_id == ctx->header->source &&
                slot->entity_id == ctx->entity_id) {
                return;
            }
        } else {
            slot->seq_no = ctx->seq_no;
            slot->timestamp = ctx->timestamp;
            slot->entity_id = ctx->entity_id;
            slot->context_id = ctx->header->source;
            slot->length = (uint16_t)length;
            slot->is_native = is_native;
            slot->via_data_port = node->rx_via_data_port;
            slot->frag_index = 0;
            slot->frag_count = 0;
            slot->occupied = true;
            memcpy(reorder_slot_payload(slot), ctx->buffer + ctx->head, length);
            sub->reorder_held++;
            if (sub->reorder_held > sub->reorder_held_peak) {
                sub->reorder_held_peak = sub->reorder_held;
            }
            return;
        }
    }

    // No room, or no buffer at all. Un-receive it: clear the bit so the gap logic still counts
    // this sample as missing and asks for it again once the hole in front of it has filled.
    //
    // Said out loud the first time, and on every power of ten after, because the cost is not
    // small and it is otherwise invisible: on the HIL rig this fallback more than halved reliable
    // receive throughput under tc loss the moment ordered delivery landed, and the only symptom
    // was the number. A configuration problem that presents as a performance cliff is one people
    // debug for a day; the same problem with a line of log attached is one they fix in a minute.
    sub->reorder_overflow++;
    if (is_power_of_ten(sub->reorder_overflow)) {
        if (reorder_payload_capacity(sub) == 0) {
            TT_LOG_WARNING("Subscriber %u is RELIABLE with no usable reorder buffer (storage=%p slots=%u "
                           "slot_bytes=%u): sample %u arrived ahead of the gap at %u and will be requested again "
                           "rather than held (occurrence #%u). Ordering is still correct; set "
                           "reorder_storage/reorder_slots/reorder_slot_bytes to stop paying for it in "
                           "retransmissions.",
                           sub->endpoint.id, (const void*)sub->reorder_storage, (unsigned)sub->reorder_slots,
                           (unsigned)sub->reorder_slot_bytes, ctx->seq_no, proxy->ack_seq_no, sub->reorder_overflow);
        } else {
            TT_LOG_WARNING("Subscriber %u reorder buffer full or too narrow for a %u-byte payload (%u slots of %u "
                           "bytes): sample %u will be requested again rather than held (occurrence #%u).",
                           sub->endpoint.id, length, sub->reorder_slots, sub->reorder_slot_bytes, ctx->seq_no,
                           sub->reorder_overflow);
        }
    }
    uint64_t offset = (uint64_t)ctx->seq_no - proxy->ack_seq_no;
    if (offset < proxy_window_bits(proxy)) {
        bitmap_clear_bit(proxy->received_bitmap, (uint32_t)offset);
    }
}

static void release_reorder_slots_for_writer(struct tt_Context* node, struct tt_Subscriber* sub, uint8_t node_id,
                                             uint32_t entity_id, bool match_any_entity) {
#if tt_FRAG_ENABLED
    if (node->frag_fast_sub == sub && node->frag_fast_context_id == node_id &&
        (match_any_entity || node->frag_fast_entity_id == entity_id)) {
        node->frag_fast_sub = NULL; // the fast path's part of a sample, abandoned like a held one
        sub->reorder_abandoned += node->frag_fast_placed;
    }
#else
    (void)node;
#endif
    if (reorder_payload_capacity(sub) == 0) {
        return;
    }
    for (uint16_t i = 0; i < sub->reorder_slots; i++) {
        struct tt_ReorderSlot* slot = reorder_slot_at(sub, i);
        if (!slot->occupied || slot->context_id != node_id) {
            continue;
        }
        if (!match_any_entity && slot->entity_id != entity_id) {
            continue;
        }
        slot->occupied = false;
        sub->reorder_held--;
        sub->reorder_abandoned++;
    }
}

// Release everything now in order: every held sample below the watermark, lowest first.
//
// Called after the watermark moves for any reason - a gap filled by a new arrival, or a gap given
// up on by jump_ack_baseline()/advance_past_unavailable(). Giving up is a delivery event too: the
// samples behind an abandoned gap have been waiting for something that is never coming, and DDS
// hands over what it has rather than holding it forever.
// Hand one sample to the application if - and only if - it keeps this writer's delivery strictly
// ordered. The single place RELIABLE ordering is enforced for delivery, so the rule cannot differ
// between a sample that arrived in order and one released from the buffer.
static void deliver_in_order(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_WriterProxy* proxy,
                             uint32_t seq_no, uint64_t timestamp, const uint8_t* payload, uint32_t length,
                             bool is_native, bool via_data_port, bool* out_decode_failed) {
    if (proxy->highest_delivered != 0 && seq_no <= proxy->highest_delivered) {
        // A step backwards: the application already has something later from this writer. Only
        // reachable after a gap was abandoned and a sample from it turned up anyway.
        sub->out_of_order_discarded++;
        return;
    }
    proxy->highest_delivered = seq_no;
    deliver_payload(node, sub, seq_no, timestamp, proxy->context_id, proxy->entity_id, payload, length, is_native,
                    via_data_port, out_decode_failed);
}

// Release, in sequence order, every held sample the watermark has passed - merged with the sample
// that just arrived, when there is one.
//
// The merge is the point. Which of the two must go first depends on which side of the held samples
// the new one lies, and it can be either:
//
//   - a gap FILLS: the new sample is below everything held behind it (2 arrives, 3 4 5 were held),
//     so it goes first;
//   - a gap is ABANDONED: jump_ack_baseline() fired, and the new sample is far above everything
//     held (5 6 were held, 1000 arrives), so it goes last.
//
// Delivering it first unconditionally - which is what this did - handed 1000 to the application
// before 5 and 6, and with strict order then in force, 5 and 6 would have been discarded as steps
// backwards: correct data lost to a sequencing bug. Walking one ascending range and delivering each
// number from wherever it lives - the slot, or the packet in hand - gets both cases right.
//
// Walks [cursor, ack) once, capped at one window (nothing held lies beyond it); a new sample past
// the cap is larger than everything walked, so it goes after. O(1) when nothing is held.
static bool reorder_slot_holds(const struct tt_ReorderSlot* slot, const struct tt_WriterProxy* proxy, uint32_t seq_no) {
    return slot->occupied && slot->seq_no == seq_no && slot->context_id == proxy->context_id &&
           slot->entity_id == proxy->entity_id;
}

#if tt_FRAG_ENABLED
static void reorder_release(struct tt_Subscriber* sub, struct tt_ReorderSlot* slot) {
    slot->occupied = false;
    sub->reorder_held--;
}

// The sample whose datagram at seq_no is next in order, held in the reorder slots one datagram per slot
// (DATAFRAG_PLAN.md section 13). Delivered once every one of its datagrams is here, put together in the
// node's frag_scratch. Returns false to stop the drain there - a datagram of it has not arrived yet but
// still can - and otherwise sets *consumed to how many seq_no it has dealt with, from seq_no on.
//
// A sample that can never be whole is dropped, its held datagrams counted in reorder_abandoned: one of
// its datagrams lies below the watermark without having arrived (a gap given up on), or the datagram
// in order is a continuation whose first datagram is behind the cursor (given up on, or tracking began
// mid-sample). Delivering part of a sample is not an option; a torn sample would decode as garbage.
static bool drain_fragmented_sample(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_WriterProxy* proxy,
                                    uint32_t seq_no, uint32_t ack, uint32_t* consumed) {
    struct tt_ReorderSlot* first = reorder_writer_slot(sub, proxy->context_id, proxy->entity_id, seq_no);
    *consumed = 1;
    if (first->frag_index != 0) {
        reorder_release(sub, first);
        sub->reorder_abandoned++;
        return true;
    }
    uint32_t count = first->frag_count;
    uint32_t total = 0;
    for (uint32_t index = 0; index < count; index++) {
        struct tt_ReorderSlot* slot = reorder_writer_slot(sub, proxy->context_id, proxy->entity_id, seq_no + index);
        if (reorder_slot_holds(slot, proxy, seq_no + index) && slot->frag_index == index && slot->frag_count == count) {
            total += slot->length;
            continue;
        }
        if (seq_no + index >= ack) {
            return false; // not here yet, and still asked for
        }
        // Given up on below the watermark: this sample can never be whole.
        uint32_t below = ack - seq_no < count ? ack - seq_no : count;
        for (uint32_t drop = 0; drop < below; drop++) {
            struct tt_ReorderSlot* held = reorder_writer_slot(sub, proxy->context_id, proxy->entity_id, seq_no + drop);
            if (reorder_slot_holds(held, proxy, seq_no + drop)) {
                reorder_release(sub, held);
                sub->reorder_abandoned++;
            }
        }
        *consumed = below;
        return true;
    }

    *consumed = count;
    frag_fast_spill(node); // the scratch is about to be overwritten; whatever it holds goes to its own buffer
    uint8_t* out = node->frag_scratch + 4;
    bool fits = total <= sizeof(node->frag_scratch) - 8;
    uint64_t timestamp = first->timestamp;
    bool is_native = first->is_native;
    bool via_data_port = first->via_data_port;
    uint32_t offset = 0;
    for (uint32_t index = 0; index < count; index++) {
        struct tt_ReorderSlot* slot = reorder_writer_slot(sub, proxy->context_id, proxy->entity_id, seq_no + index);
        if (fits) {
            _tt_memcpy(out + offset, reorder_slot_payload(slot), slot->length);
            offset += slot->length;
        }
        reorder_release(sub, slot);
    }
    if (!fits) {
        node->frag_dropped++; // cannot happen from a sender within tt_MAX_SAMPLE_LENGTH
        return true;
    }
    sub->reorder_delivered++;
    node->frag_reassembled++;
    deliver_in_order(node, sub, proxy, seq_no, timestamp, out, total, is_native, via_data_port, NULL);
    return true;
}
#endif

static void drain_reorder_with(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_WriterProxy* proxy,
                               struct data_delivery_ctx* arriving, bool is_native) {
#if tt_FRAG_ENABLED
    frag_fast_spill_if_overtaken(node, sub, proxy);
#endif
    uint32_t ack = proxy->ack_seq_no;
    uint32_t stop = ack; // where the next drain starts: ack, unless a sample in order is not whole yet
    bool arriving_done = (arriving == NULL);

    if (sub->reorder_held != 0 && reorder_payload_capacity(sub) != 0) {
        uint32_t cursor = proxy->reorder_cursor;
        uint32_t window = proxy_window_bits(proxy);
        uint32_t span = (ack - cursor > window) ? window : ack - cursor;
        for (uint32_t step = 0; step < span; step++) {
            uint32_t seq = cursor + step;
            if (!arriving_done && seq == arriving->seq_no) {
                deliver_in_order(node, sub, proxy, arriving->seq_no, arriving->timestamp,
                                 arriving->buffer + arriving->head, arriving->tail - arriving->head, is_native,
                                 node->rx_via_data_port, &arriving->decode_failed);
                arriving_done = true;
                continue;
            }
            struct tt_ReorderSlot* slot = reorder_writer_slot(sub, proxy->context_id, proxy->entity_id, seq);
            if (!reorder_slot_holds(slot, proxy, seq)) {
                continue;
            }
#if tt_FRAG_ENABLED
            if (slot->frag_count != 0) {
                uint32_t consumed = 1;
                if (!drain_fragmented_sample(node, sub, proxy, seq, ack, &consumed)) {
                    stop = seq;
                    break;
                }
                step += consumed - 1;
                continue;
            }
#endif
            slot->occupied = false;
            sub->reorder_held--;
            sub->reorder_delivered++;
            deliver_in_order(node, sub, proxy, slot->seq_no, slot->timestamp, reorder_slot_payload(slot), slot->length,
                             slot->is_native, slot->via_data_port, NULL);
        }
    }
    if (!arriving_done) {
        if (stop == ack) {
            deliver_in_order(node, sub, proxy, arriving->seq_no, arriving->timestamp, arriving->buffer + arriving->head,
                             arriving->tail - arriving->head, is_native, node->rx_via_data_port,
                             &arriving->decode_failed);
        } else {
            hold_for_reorder(node, sub, proxy, arriving, is_native); // behind a sample not yet whole
        }
    }
    proxy->reorder_cursor = stop;
}

static void drain_reorder(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_WriterProxy* proxy) {
    drain_reorder_with(node, sub, proxy, NULL, false);
}

// A RELIABLE Subscriber just declined `ctx`'s sample (g13: nowhere to put it). Nothing about the sample is recorded -
// that is what keeps it unacknowledged - but the writer evidently has published it, so it counts as announced, as a
// Heartbeat would: the gap is then visible to highest_relevant_bit(), and the retry timer is armed - not fired, so
// the decline itself sends nothing - to ask for it once its interval has passed (a gap found meanwhile is still asked
// for at once, by update_reliable_ack()'s new-gap ACKNACK, as with any armed timer). Without this a declined sample was
// asked for again only if a Heartbeat or a later accepted sample revealed it, and a writer that had stopped sent
// neither: the samples declined after its last piggybacked Heartbeat were never asked for (rmw
// test_loaned_messages' held case, 2026-10-09). No proxy yet means no baseline to measure a gap from; the writer's
// first Heartbeat or accepted sample makes one.
static void note_declined(struct tt_Context* node, struct tt_Subscriber* sub, const struct data_delivery_ctx* ctx) {
    if (!sub->reliable) {
        return; // BEST_EFFORT: a decline is a counted drop, nothing will send it again
    }
    struct tt_WriterProxy* proxy = find_writer_proxy(sub, ctx->header->source, ctx->entity_id);
    if (proxy == NULL || ctx->seq_no < proxy->ack_seq_no) {
        return;
    }
    if (ctx->seq_no > proxy->heartbeat_last_seq_no) {
        proxy->heartbeat_last_seq_no = ctx->seq_no;
    }
    proxy->sender_ip = ctx->sender_ip;
    proxy->sender_port = ctx->sender_port;
    if (!proxy->acknack_scheduled) {
        if (tt_Context_schedule(node, tt_get_ns() + reliable_retry_interval(node, proxy), acknack_retry, proxy)) {
            proxy->acknack_scheduled = true;
            proxy->retry = 0;
        } else {
            TT_LOG_ERROR("Cannot schedule acknack_retry");
        }
    }
}

static void deliver_data_to_subscriber(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    struct data_delivery_ctx* ctx = (struct data_delivery_ctx*)ctx_ptr;
    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
    if (ctx->best_effort_only && sub->reliable && !TT_ORDERING_DISABLED) {
        return; // it took this sample's fragments itself, each under its own seq_no
    }

    // QoS roadmap #1 (RxO matching, Milestone 31) - an incompatible Publisher's DATA is dropped
    // before any reliable-tracking side effects too (update_reliable_ack() below), not just before
    // delivery - no point generating ACKNACKs a Publisher that could never honor them will never
    // answer (see this file's own pre-Milestone-31 history of exactly that silent-degradation bug).
    if (subscriber_incompatible_with_writer(node, sub, ctx->header->source, ctx->endpoint_id, ctx->entity_id)) {
        return;
    }

    // g13 (rmw_tickle/RMW_GAPS_PLAN.md) - flow control, and it belongs HERE, above
    // update_reliable_ack(), for the same reason the RxO drop just above does: this is the last
    // instant at which nothing has been recorded about this sample. update_reliable_ack() advances
    // the ack watermark AND can send an ACKNACK carrying it before it returns, and an ACKNACK's
    // watermark implicitly acks every sample below it - so a decline made after it ran would be
    // trying to take back something the writer had already been told. Declining before it removes
    // that instant instead of reasoning about how narrow it is.
    if (sub->accept_callback != NULL && !sub->accept_callback(sub, ctx->seq_no, sub->accept_callback_param)) {
        sub->accept_declines++;
        forget_requests_answered_by(sub, ctx->header->source, ctx->entity_id, ctx->seq_no);
        if (is_power_of_ten(sub->accept_declines)) {
            TT_LOG_WARNING("Subscriber %u declined sample %u from node %u (decline #%u): nowhere to put it. A "
                           "RELIABLE writer still holds it and will send it again, so nothing is lost yet - but a "
                           "reader that never accepts stalls its writers, which is what a no-overwrite history "
                           "means. A BEST_EFFORT stream has no retransmission, so there this sample is gone.",
                           sub->endpoint.id, ctx->seq_no, ctx->header->source, sub->accept_declines);
        }
        note_declined(node, sub, ctx);
        return;
    }

    bool is_native = tt_is_native_endian(ctx->header);

    // BEST_EFFORT ordering (2026-09-24): a sample no newer than the last one delivered from this
    // same writer is discarded rather than handed up.
    //
    // This is the DDS reader policy, adopted deliberately rather than invented. A BEST_EFFORT
    // reader gives up on missing samples but never hands the application one it has already moved
    // past - so an application may see gaps, and may never see a sample twice or out of order.
    // TickLE previously delivered everything in arrival order, which is a *weaker* guarantee than
    // any DDS implementation offers, and applications written against DDS semantics assert on it:
    // performance_test aborts with "Received sample with not strictly older timestamp", which is
    // what this whole investigation was about.
    //
    // Per writer, not globally, because seq_no counts per writer - comparing across writers would
    // discard a perfectly good sample because a different Publisher happened to be further along.
    // The WriterProxy table already exists for exactly this identity and is bounded by
    // tt_MAX_PEER_COUNT, so this needs no new storage; it just stops being reliable-only.
    //
    // A Publisher that restarts resets its seq_no to 1, which would otherwise be discarded forever
    // against a high watermark. It is not, because a restarted Publisher carries a new entity_id
    // (Milestone 47) and therefore claims a different proxy.
    if (!sub->reliable && !TT_ORDERING_DISABLED) {
        struct tt_WriterProxy* proxy = find_or_create_writer_proxy(sub, ctx->header->source, ctx->entity_id, NULL);
        // No proxy slot free: deliver rather than drop. Losing a sample because a *diagnostic-
        // sized* table is full would be a worse failure than delivering one out of order, and the
        // table is sized for the peers a node can talk to anyway.
        if (proxy != NULL) {
            if (ctx->seq_no < proxy->ack_seq_no) {
                sub->out_of_order_discarded++;
                return;
            }
            proxy->ack_seq_no = ctx->seq_no + 1;
        }
    }

    // QoS roadmap #5 (RELIABILITY/RELIABLE) - no-op (always "new") unless sub->reliable. Milestone
    // 60 (rmw_tickle/PLAN.md) - a sample update_reliable_ack() recognizes as already delivered (a
    // legitimate ACKNACK-driven retransmit racing the original, or a stale duplicate) is skipped
    // here instead of re-invoking the application callback a second time for it, matching real DDS
    // readers' own per-writer sequence-number de-duplication.
    if (!update_reliable_ack(node, sub, ctx->seq_no, ctx->header->source, ctx->entity_id, ctx->sender_ip,
                             ctx->sender_port)) {
        return;
    }

    // RELIABLE in-order delivery (2026-09-24). Ordering is now the reader's job, not the
    // application's: a sample ahead of an unfilled gap waits, and everything behind it waits with
    // it. That is head-of-line blocking by construction, which is what RELIABLE means.
    //
    // KNOWN GAP, measured rather than suspected: this does not make delivery totally ordered. A
    // sample arriving BELOW the watermark is still delivered, and after a give-up path has moved
    // the watermark past an abandoned gap that is a step backwards the application can see. Under
    // 8% injected loss at maximum rate it happened 2493 times in 1.28M samples - with the reorder
    // buffer working perfectly, 686192 samples held and released in order and zero overflows.
    //
    // It is not fixed by discarding below-watermark samples, which was tried and is wrong: after
    // jump_ack_baseline() abandons a range, samples from that range are genuinely NEW - the
    // watermark moved past them without delivering them - so discarding loses data the reader
    // could have had, and tests/test_reliable_pubsub.c pins exactly that. Closing it properly
    // needs a per-writer "highest actually delivered" separate from the ack watermark, because
    // the two diverge precisely when a gap is abandoned.
    //
    // Whether this sample was in order is read off the watermark rather than tracked separately:
    // update_reliable_ack() advances ack_seq_no past this sample if and only if it was the next
    // one expected, so ack_seq_no > seq_no means in-order and anything else means ahead of a gap.
    if (sub->reliable && !TT_ORDERING_DISABLED) {
        struct tt_WriterProxy* proxy = find_writer_proxy(sub, ctx->header->source, ctx->entity_id);
        if (proxy != NULL && proxy->ack_seq_no <= ctx->seq_no) {
            hold_for_reorder(node, sub, proxy, ctx, is_native);
            return;
        }
        if (proxy != NULL) {
            drain_reorder_with(node, sub, proxy, ctx, is_native);
            return;
        }
        // No WriterProxy slot free, so no ordering state to honour: deliver as it came, which is
        // what this path did before ordering existed.
        deliver_payload(node, sub, ctx->seq_no, ctx->timestamp, ctx->header->source, ctx->entity_id,
                        ctx->buffer + ctx->head, ctx->tail - ctx->head, is_native, node->rx_via_data_port,
                        &ctx->decode_failed);
        return;
    }

    deliver_payload(node, sub, ctx->seq_no, ctx->timestamp, ctx->header->source, ctx->entity_id,
                    ctx->buffer + ctx->head, ctx->tail - ctx->head, is_native, node->rx_via_data_port,
                    &ctx->decode_failed);
}

static bool process_data_for(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                             uint32_t tail, uint32_t sender_ip, uint16_t sender_port, bool best_effort_only) {
    struct tt_DataHeader* data_header = decode(node, buffer, &head, tail, sizeof(struct tt_DataHeader));
    if (data_header == NULL) {
        TT_LOG_ERROR("Illegal DataHeader");
        return false;
    }

    uint32_t endpoint_id = rd32(header, data_header->endpoint_id);
    uint32_t seq_no = rd32(header, data_header->seq_no);
    uint64_t timestamp = timestamp_from_wire(node, rd32(header, data_header->timestamp));
    uint32_t entity_id = rd32(header, data_header->entity_id);

    // The built-in discovery endpoint (tt_DISCOVERY_ENDPOINT_ID, tickle.h): no Subscriber, no reliable
    // tracking and no deduplication in front of it - process_announce() refreshes liveliness first.
    if (endpoint_id == tt_DISCOVERY_ENDPOINT_ID && entity_id == tt_DISCOVERY_ENTITY_ID) {
        return process_announce(node, header, buffer, head, tail, sender_ip, sender_port, seq_no, 0, 1);
    }

    note_manual_assertion(node, header->source, endpoint_id, entity_id);

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
        .best_effort_only = best_effort_only,
    };
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id, deliver_data_to_subscriber, &ctx);
    return !ctx.decode_failed;
}

static bool process_data(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                         uint32_t tail, uint32_t sender_ip, uint16_t sender_port) {
    return process_data_for(node, header, buffer, head, tail, sender_ip, sender_port, false);
}

// Whether slot `slot` names the call (receiver, client_tag, seq_no). Meaningful while the slot holds a live entry
// (cache[slot]) or an expired one not yet reused (cache_sent_at[slot] != 0). seq_no alone is unique per call within a
// context (the context's one counter, struct tt_Context.call_seq_no) until it wraps; the tag keeps a wrapped seq_no
// of another Client of that context from being answered with this one's response.
static bool server_cache_slot_names(struct tt_Server* server, int slot, uint8_t receiver, uint8_t client_tag,
                                    uint16_t seq_no) {
    const struct tt_SubmessageHeader* submessage_header =
        (const struct tt_SubmessageHeader*)server_cache_entry(server, slot);
    const struct tt_CallResponseHeader* callresponse_header =
        (const struct tt_CallResponseHeader*)((const uint8_t*)submessage_header + sizeof(struct tt_SubmessageHeader));
    return submessage_header->receiver == receiver && server->cache_client_tag[slot] == client_tag &&
           callresponse_header->seq_no == seq_no;
}

static int find_server_cache_slot(struct tt_Server* server, uint8_t receiver, uint8_t client_tag, uint16_t seq_no) {
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (server->cache[i] != NULL && server_cache_slot_names(server, i, receiver, client_tag, seq_no)) {
            return i;
        }
    }
    return -1;
}

// The entity_id under which discovery knows `receiver`'s client of this service, or 0 when it does not know one (no
// discovery table attached, or not announced yet). An entity_id is drawn per launch (tt_Context.entity_id_base), so a
// restarted client process, or a client re-created in one, announces a different one under the same context id.
static uint32_t server_client_entity(const struct tt_Server* server, uint8_t receiver) {
    const struct tt_DiscoveredEntity* client =
        server->node != NULL
            ? discovery_find_kind(server->node->discovery, receiver, server->endpoint.id, tt_KIND_SERVICE_CLIENT)
            : NULL;
    return client != NULL ? client->entity_id : 0;
}

// Whether discovery still knows `receiver`'s client `entity_id` of this service - one of them, when that context
// hosts several clients of it, which share the endpoint_id and so cannot be told apart by it.
static bool server_client_known(const struct tt_Server* server, uint8_t receiver, uint32_t entity_id) {
    const struct tt_DiscoveredEntity* client =
        server->node != NULL
            ? tt_Discovery_find_entity(server->node->discovery, receiver, server->endpoint.id, entity_id)
            : NULL;
    return client != NULL && client->kind == tt_KIND_SERVICE_CLIENT;
}

// Whether slot `slot`'s response was cached for an earlier incarnation of the client now asking: discovery knew the
// client then and knows it now, under different entity_ids. When either side is unknown it cannot tell, and the
// response stands - the old behaviour, which a farewell still bounds (drop_cached_responses_from_source()).
static bool server_cache_slot_is_stale(const struct tt_Server* server, int slot, uint8_t receiver) {
    uint32_t cached_for = server->cache_client_entity[slot];
    if (cached_for == 0) {
        return false;
    }
    return server_client_entity(server, receiver) != 0 && !server_client_known(server, receiver, cached_for);
}

static struct tt_SubmessageHeader* get_server_cache(struct tt_Server* server, uint8_t receiver, uint8_t client_tag,
                                                    uint16_t seq_no) {
    int slot = find_server_cache_slot(server, receiver, client_tag, seq_no);
    return slot >= 0 ? server->cache[slot] : NULL;
}

// How long the client of this service may go on retrying one call before it has any answer to estimate from: its
// seed schedule (call_retry_window() over tt_CALL_RETRY_INTERVAL), or, when the service sets call_retry_interval
// explicitly, (count + 1) waits of exactly that - what both ends of a service share without anything on the wire.
static uint64_t server_client_window(const struct tt_Server* server) {
    const struct tt_Service* service = server->service;
    uint32_t count = service->call_retry_count != 0 ? service->call_retry_count : (uint32_t)tt_CALL_RETRY_COUNT;
    uint32_t waits = count == UINT32_MAX ? count : count + 1;
    if (service->call_retry_interval != 0) {
        return call_retry_window(service->call_retry_interval, service->call_retry_interval, waits);
    }
    return call_retry_window((uint64_t)tt_CALL_RETRY_INTERVAL,
                             (uint64_t)tt_CALL_RETRY_INTERVAL * tt_CALL_RETRY_MAX_SRTT_MULTIPLE, waits);
}

// How long an answered response is kept for a retrying client: the client's seed schedule, or
// tt_SERVER_CACHE_GAP_MULTIPLE x the longest recent retry gap this server has seen, whichever is longer. See
// config.h for why the server derives it from these and not from the client's srtt.
static uint64_t server_cache_lifetime(const struct tt_Server* server) {
    uint64_t window = server_client_window(server);
    uint64_t learnt = server->client_retry_gap > UINT64_MAX / tt_SERVER_CACHE_GAP_MULTIPLE
                          ? UINT64_MAX
                          : server->client_retry_gap * tt_SERVER_CACHE_GAP_MULTIPLE;
    return learnt > window ? learnt : window;
}

// A retry arrived `gap` after this server last sent that call's response. A decaying maximum: a slower client
// counts at once, a faster population pulls it down an eighth per sample.
static void note_client_retry_gap(struct tt_Server* server, uint64_t gap) {
    uint64_t decayed = server->client_retry_gap - (server->client_retry_gap / 8);
    server->client_retry_gap = gap > decayed ? gap : decayed;
}

// (Re-)arms slot `slot`'s expiry one lifetime from `now`. False if the scheduler is full, with nothing armed.
static bool arm_server_cache_expiry(struct tt_Server* server, int slot, uint64_t now) {
    if (server->clean_scheduled[slot]) {
        tt_Context_unschedule(server->node, server_cache_clean, &server->clean_config[slot]);
        server->clean_scheduled[slot] = false;
    }
    uint64_t lifetime = server_cache_lifetime(server);
    uint64_t expires_at = lifetime > UINT64_MAX - now ? UINT64_MAX : now + lifetime;
    server->clean_config[slot].server = server;
    server->clean_config[slot].slot = slot;
    if (!tt_Context_schedule(server->node, expires_at, server_cache_clean, &server->clean_config[slot])) {
        return false;
    }
    server->clean_scheduled[slot] = true;
    return true;
}

// Cancels slot i's cleanup timer (if any) and frees it up for reuse. The timer must be
// cancelled before the slot is reused, otherwise it later fires and clears whatever
// unrelated entry ends up occupying the slot by then.
static void clear_server_cache_slot(struct tt_Server* server, int slot) {
    if (server->clean_scheduled[slot]) {
        tt_Context_unschedule(server->node, server_cache_clean, &server->clean_config[slot]);
        server->clean_scheduled[slot] = false;
    }
    server->cache[slot] = NULL;
    server->cache_sent_at[slot] = 0; // the client moved on, or the slot is wanted: nothing to learn from it
    server->cache_client_entity[slot] = 0;
    server->cache_client_tag[slot] = 0;
}

// A complete announce from `source` has just been applied. Every local Server drops the responses - live or expired
// - it holds for a client on `source` that announce shows to be gone or replaced: all of them on a farewell (an
// announce listing nothing, tt_Context_destroy()), and with discovery attached, each one whose client the source no
// longer lists, or lists under another entity_id. A restarted client reusing the context id starts its seq_no at 0
// again; without this its first calls could be answered with its predecessor's responses.
static void drop_cached_responses_from_source(struct tt_Context* node, uint8_t source, bool farewell) {
    for (uint32_t index = 0; index < node->endpoint_count; index++) {
        struct tt_Endpoint* endpoint = node->endpoints[index];
        if (endpoint == NULL || endpoint->kind != tt_KIND_SERVICE_SERVER) {
            continue;
        }
        struct tt_Server* server = (struct tt_Server*)endpoint;
        for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
            if (server->cache[i] == NULL && server->cache_sent_at[i] == 0) {
                continue; // holds nothing
            }
            if (((const struct tt_SubmessageHeader*)server_cache_entry(server, i))->receiver != source) {
                continue;
            }
            bool replaced = server->cache_client_entity[i] != 0 &&
                            (farewell || !server_client_known(server, source, server->cache_client_entity[i]));
            if (farewell || replaced) {
                clear_server_cache_slot(server, i);
            }
        }
    }
}

// Expiry. The slot keeps cache_sent_at and its bytes, so a retry that comes after this can still be matched to it
// (learn_from_expired_response()) until the slot is reused.
static void server_cache_clean(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(node);
    UNUSED(time);

    struct server_cache_clean_config* clean = param;
    clean->server->cache[clean->slot] = NULL;
    clean->server->clean_scheduled[clean->slot] = false;
}

// A retry served from the cache: what it took the client to ask again is a sample of its retry gap, and a client
// still retrying is the evidence the entry is wanted, so it is re-armed from now.
static void note_server_cache_hit(struct tt_Server* server, int slot) {
    uint64_t now = tt_get_ns();
    if (now > server->cache_sent_at[slot]) {
        note_client_retry_gap(server, now - server->cache_sent_at[slot]);
    }
    server->cache_sent_at[slot] = now;
    if (!arm_server_cache_expiry(server, slot, now)) {
        TT_LOG_ERROR("Cannot schedule server_cache_clean");
        clear_server_cache_slot(server, slot); // never an entry with no way to expire
    }
}

// A retry with no live entry: if its response did go out and has since expired, the client waits longer than this
// server kept it, and the gap says by how much. One such sample counts as at most twice the current lifetime, so a
// stale match - a restarted client reusing the id, long after - raises it by a bounded step and not to whatever it
// measured; a genuinely slow client gets there in a few misses.
static void learn_from_expired_response(struct tt_Server* server, uint8_t receiver, uint8_t client_tag,
                                        uint16_t seq_no) {
    uint64_t now = tt_get_ns();
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (server->cache[i] != NULL || server->cache_sent_at[i] == 0 ||
            !server_cache_slot_names(server, i, receiver, client_tag, seq_no)) {
            continue;
        }
        if (now > server->cache_sent_at[i] && !server_cache_slot_is_stale(server, i, receiver)) {
            uint64_t gap = now - server->cache_sent_at[i];
            uint64_t lifetime = server_cache_lifetime(server);
            uint64_t bound = lifetime > UINT64_MAX / 2 ? UINT64_MAX : 2 * lifetime;
            note_client_retry_gap(server, gap < bound ? gap : bound);
        }
        server->cache_sent_at[i] = 0; // learnt once
        return;
    }
}

// Caches the response just encoded at `submessage_header` for the Client `client_tag` of context `receiver`. That
// Client's previous answer goes: it calls again only once it has it (one outstanding call per Client). Another
// Client's answer, from the same context or not, stays until it expires or is evicted - one live answer per calling
// Client, the memory bound tt_MAX_SERVER_CACHE_COUNT (>= # of clients) was always sized by.
static bool set_server_cache(struct tt_Server* server, struct tt_SubmessageHeader* submessage_header, uint8_t receiver,
                             uint8_t client_tag) {
    size_t length = ROUNDUP((uintptr_t)server->node->tx_buffer + server->node->tx_tail - (uintptr_t)submessage_header);

    // A slot that never held anything first, then the expired entry sent longest ago (it can still teach a gap, so
    // it goes last among the free), and with every slot live, the live entry sent longest ago - evicted rather than
    // the new response not being sent at all, which is what a full cache used to mean.
    int empty_slot = -1;
    int expired_slot = -1;
    int oldest_live = -1;
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (server->cache[i] != NULL && server->cache[i]->receiver == receiver &&
            server->cache_client_tag[i] == client_tag) {
            clear_server_cache_slot(server, i);
        }

        if (server->cache[i] == NULL) {
            if (server->cache_sent_at[i] == 0) {
                empty_slot = empty_slot < 0 ? i : empty_slot;
            } else if (expired_slot < 0 || server->cache_sent_at[i] < server->cache_sent_at[expired_slot]) {
                expired_slot = i;
            }
        } else if (oldest_live < 0 || server->cache_sent_at[i] < server->cache_sent_at[oldest_live]) {
            oldest_live = i;
        }
    }

    if (length > server_cache_entry_length(server)) {
        // Larger than this server's cache entries (tt_Server_set_storage() sized them for this
        // service's responses, and this one is bigger). Sent, not cached: refusing to send it would
        // turn a size limit on a retry optimisation into a lost response. A retry re-runs the
        // callback, exactly as it does once a cached response has expired.
        TT_LOG_WARNING("Response of %u bytes exceeds the server's %u-byte cache entries - sent, not cached",
                       (unsigned)length, (unsigned)server_cache_entry_length(server));
        return true;
    }

    int free_slot = empty_slot >= 0 ? empty_slot : expired_slot;
    if (free_slot < 0) {
        TT_LOG_DEBUG("Server cache full - evicting the response sent longest ago");
        free_slot = oldest_live;
        clear_server_cache_slot(server, free_slot);
    }

    // Copy into this slot's own fixed buffer instead of malloc'ing one.
    struct tt_SubmessageHeader* cache = (struct tt_SubmessageHeader*)server_cache_entry(server, free_slot);
    _tt_memcpy(cache, submessage_header, length);
    cache->length = length;

    // Only publish `cache` into the slot once its cleanup timer is guaranteed to run;
    // otherwise the slot would hold an entry that never gets cleared.
    uint64_t now = tt_get_ns();
    if (!arm_server_cache_expiry(server, free_slot, now)) {
        TT_LOG_ERROR("Cannot schedule server_cache_clean");
        server->cache_sent_at[free_slot] = 0; // its bytes were just overwritten: no longer an expired entry either
        return false;
    }

    server->cache_sent_at[free_slot] = now;
    server->cache_client_entity[free_slot] = server_client_entity(server, receiver);
    server->cache_client_tag[free_slot] = client_tag;
    server->cache[free_slot] = cache;

    return true;
}

// Cache hit: re-encode the previously cached response verbatim (bumping its retry count) instead
// of re-running the service callback - the client is asking again because it hasn't seen the
// first response yet, not because it wants a fresh answer.
static struct tt_SubmessageHeader* resend_cached_response(struct tt_Context* node,
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
// deferred tt_SERVER_CALLBACK returns), or handed to us later by tt_Server_send_response() for a
// request whose callback returned tt_CALL_DEFERRED (Milestone 17, rmw_tickle/PLAN.md). Split out
// from what used to be one function (build_call_response()) precisely so the callback-invocation
// half (which decides whether an answer exists *yet* at all) stays separate from this, the
// encode-what-already-exists half - `receiver`/`response` come from a live just-decoded packet in
// the synchronous case, or from a pending slot's own stored fields in the deferred case, but this
// function itself doesn't need to know which. `old_tx_tail` is this submessage's start, for
// rolling back on a failure here.
static struct tt_SubmessageHeader* encode_call_response(struct tt_Context* node, uint8_t receiver, uint8_t client_tag,
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
    if (!set_server_cache(server, submessage_header, receiver, client_tag)) {
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
static int find_pending_slot(struct tt_Server* server, uint8_t receiver, uint8_t client_tag, uint16_t seq_no) {
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (__atomic_load_n(&server->slot_state[i], __ATOMIC_ACQUIRE) == tt_SERVER_SLOT_EMPTY) {
            continue;
        }
        if (server->pending_request_id[i].receiver == receiver && server->pending_request_id[i].seq_no == seq_no &&
            server->pending_client_tag[i] == client_tag) {
            return i;
        }
    }
    return -1;
}

// Timer callback (tt_Context_schedule(), Milestone 17): if a deferred request still hasn't been
// answered by the time it fires, reclaim its slot rather than let it leak forever.
// tt_Server_send_response()'s own compare-exchange against tt_SERVER_SLOT_PENDING loses cleanly
// if it races against this, exactly like server_cache_clean() already reclaims an *answered*
// slot's own retry-cache lifetime.
static void pending_response_timeout(struct tt_Context* node, uint64_t time, void* param) {
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
static bool defer_call_response(struct tt_Server* server, tt_RequestId request_id, uint8_t client_tag,
                                uint32_t sender_ip, uint16_t sender_port) {
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
    server->pending_client_tag[slot] = client_tag;
    server->pending_sender_ip[slot] = sender_ip;
    server->pending_sender_port[slot] = sender_port;
    server->pending_timeout_config[slot].server = server;
    server->pending_timeout_config[slot].slot = slot;

    // Only publish once the timeout is guaranteed to run - same "don't publish an entry with no
    // way to reclaim it" reasoning set_server_cache() already follows for its own timer.
    if (!tt_Context_schedule(server->node, tt_get_ns() + tt_SERVER_DEFERRED_RESPONSE_TIMEOUT, pending_response_timeout,
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

static void send_ready_slot(struct tt_Context* node, struct tt_Server* server, int slot, struct tt_Response* response);

tt_ret_t tt_Server_send_response(struct tt_Server* server, tt_RequestId request_id, int8_t return_code,
                                 struct tt_Response* response) {
    if (server == NULL || response == NULL || server->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    // Encoded straight from the caller's `response` and sent, before this returns, on the caller's thread and under
    // the state lock (re-entrant, so a callback may call this too). The slot keeps only its bookkeeping - request
    // id, sender, return code, state, timer - and never a copy of the struct (2026-09-27). It used to take a shallow
    // copy for the poll thread to encode later: a response pointing at data it does not own - a string, as every
    // generated TickLE struct holds one - was then encoded from whatever that data had become, and rmw_tickle's
    // service responses, which alias the ROS response rclcpp destroys as soon as rmw_send_response() returns,
    // arrived with every string past std::string's inline 15 bytes as garbage. The caller may free or reuse the
    // response and everything it points at as soon as this returns.
    struct tt_Context* node = server->node;
    state_lock(node);
    tt_ret_t result = tt_RET_NOT_FOUND;
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (__atomic_load_n(&server->slot_state[i], __ATOMIC_ACQUIRE) != tt_SERVER_SLOT_PENDING) {
            continue;
        }
        if (server->pending_request_id[i].receiver != request_id.receiver ||
            server->pending_request_id[i].seq_no != request_id.seq_no) {
            continue;
        }
        server->pending_return_code[i] = return_code;
        // Under the lock the timeout (poll thread, also under it) cannot run in between; the slot is now this
        // response's, and its timer goes with it, so it cannot reclaim the slot's next request early.
        __atomic_store_n(&server->slot_state[i], tt_SERVER_SLOT_READY, __ATOMIC_RELEASE);
        if (server->pending_timeout_scheduled[i]) {
            tt_Context_unschedule(node, pending_response_timeout, &server->pending_timeout_config[i]);
            server->pending_timeout_scheduled[i] = false;
        }
        send_ready_slot(node, server, i, response);
        result = tt_RET_OK;
        break;
    }
    state_unlock(node);
    return result;
}

// Encodes `response` - the caller's own, not a copy - as the answer to the request READY in `slot`, sends it, and
// empties the slot. Under the state lock, from tt_Server_send_response().
static void send_ready_slot(struct tt_Context* node, struct tt_Server* server, int slot, struct tt_Response* response) {
    tt_RequestId request_id = server->pending_request_id[slot];
    uint32_t sender_ip = server->pending_sender_ip[slot];
    uint16_t sender_port = server->pending_sender_port[slot];
    int8_t return_code = server->pending_return_code[slot];

    uint32_t old_tx_tail = node->tx_tail;
    struct tt_SubmessageHeader* submessage_header =
        encode_call_response(node, request_id.receiver, server->pending_client_tag[slot], server, request_id.seq_no,
                             return_code, response, old_tx_tail);

    // Reclaim the slot regardless of encode success - a failure here is already logged by
    // encode_call_response() itself, and retrying it from this same stale slot later would just fail
    // identically forever.
    __atomic_store_n(&server->slot_state[slot], tt_SERVER_SLOT_EMPTY, __ATOMIC_RELAXED);

    if (submessage_header == NULL) {
        return;
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

// Milestone 35 - deliberately still uses find_endpoint()'s single-match lookup, not for_each_
// endpoint(), even though more than one local Server may now share this service name: a
// CallRequest must be answered by exactly one Server, and the wire protocol has no per-instance
// id beyond the service-name hash to say which of several identically-named ones a Client meant
// to reach - genuinely ambiguous at the wire level, not a gap this function's own logic could
// close. Picking find_endpoint()'s own deterministic "oldest still-registered match" (its own doc
// comment) is a reasonable, documented choice, matching real DDS's own undefined-which-one
// semantics for redundant same-name Servers - not attempted to be made "correct" beyond that here.
static bool process_callrequest(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                                uint32_t tail, uint32_t sender_ip, uint16_t sender_port) {
    struct tt_CallRequestHeader* callrequest_header =
        decode(node, buffer, &head, tail, sizeof(struct tt_CallRequestHeader));
    if (callrequest_header == NULL) {
        TT_LOG_ERROR("Illegal CallRequestHeader");
        return false;
    }

    uint32_t endpoint_id = rd32(header, callrequest_header->endpoint_id);
    uint16_t seq_no = rd16(header, callrequest_header->seq_no);
    uint8_t client_tag = callrequest_header->client_tag; // which Client of the source context; 0 from an older one

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
    // ever gets *cached* once tt_Server_send_response() has actually sent it.
    if (find_pending_slot(server, header->source, client_tag, seq_no) >= 0) {
        TT_LOG_DEBUG("CallRequest retry for a still-deferred response, ignoring");
        return true;
    }

    // Check cache - for any request, a duplicated first transmission (retry 0) included: answering it again would run
    // a non-idempotent callback twice. What must not be answered from the cache is a request from a new incarnation of
    // the client - a restarted process reusing the context id, whose seq_no starts at 0 again. Discovery tells the two
    // apart where it can (server_cache_slot_is_stale()); a farewell drops the source's responses outright
    // (drop_cached_responses_from_source()).
    int cached_slot = find_server_cache_slot(server, header->source, client_tag, seq_no);
    if (cached_slot >= 0 && server_cache_slot_is_stale(server, cached_slot, header->source)) {
        TT_LOG_DEBUG("Cached response for node %d is from an earlier incarnation of its client - dropped",
                     header->source);
        clear_server_cache_slot(server, cached_slot);
        cached_slot = -1;
    }
    if (cached_slot >= 0) {
        note_server_cache_hit(server, cached_slot);
    } else if (callrequest_header->retry != 0) {
        learn_from_expired_response(server, header->source, client_tag, seq_no);
    }
    struct tt_SubmessageHeader* cached = cached_slot >= 0 ? server->cache[cached_slot] : NULL;
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
            // tt_SERVER_DEFERRED_RESPONSE_TIMEOUT from now) encodes and sends it.
            return defer_call_response(server, request_id, client_tag, sender_ip, sender_port);
        }

        submessage_header = encode_call_response(node, header->source, client_tag, server, seq_no, return_code,
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

// The Client of this service whose outstanding call is `seq_no`, or NULL when none is: nothing outstanding (a
// duplicate answer, or a late one after call_retry() gave up), or an answer to some earlier call. Several Clients of
// one service in one context share the endpoint_id; the seq_no tells them apart, because it is drawn from the
// context's one counter (struct tt_Context.call_seq_no). This took the first of them, find_endpoint()'s single match,
// until 2026-10-09, and the answers to every other one were dropped as answers to someone else's call.
static struct tt_Client* find_calling_client(struct tt_Context* node, uint32_t endpoint_id, uint16_t seq_no) {
    if (!node->endpoint_index_valid) {
        rebuild_endpoint_index(node);
    }
    uint32_t slot = endpoint_id & (tt_ENDPOINT_INDEX_SIZE - 1);
    for (uint32_t probe = 0; probe < tt_ENDPOINT_INDEX_SIZE; probe++) {
        struct tt_Endpoint* endpoint = node->endpoint_index[slot];
        if (endpoint == NULL) {
            break;
        }
        if (endpoint->kind == tt_KIND_SERVICE_CLIENT && endpoint->id == endpoint_id) {
            struct tt_Client* client = (struct tt_Client*)endpoint;
            const struct tt_CallRequestHeader* outstanding =
                client->cache != NULL ? (const struct tt_CallRequestHeader*)((const uint8_t*)client->cache +
                                                                             sizeof(struct tt_SubmessageHeader))
                                      : NULL;
            if (outstanding != NULL && outstanding->seq_no == seq_no) {
                return client;
            }
        }
        slot = (slot + 1) & (tt_ENDPOINT_INDEX_SIZE - 1);
    }
    return NULL;
}

static bool process_callresponse(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
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

    // Only the response to a call that's still outstanding counts. A duplicate (the server
    // answered both the original request and a retry that crossed it on the wire) or a late one
    // (arriving after call_retry() already gave up, see its own client->callback(0, NULL)) would
    // otherwise invoke client->callback a second time and pollute the latency EMA with a stale
    // cache_time.
    struct tt_Client* client = find_calling_client(node, endpoint_id, seq_no);
    if (client == NULL) {
        TT_LOG_DEBUG("CallResponse seq_no %u answers no outstanding call, ignoring", seq_no);
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
    tt_Context_unschedule(node, call_retry, client);

    // RFC 6298's estimator, with the reliable retry's gains (note_recovery_sample()): the first answer - or the first
    // since a backoff, see back_off_retry_interval() - sets srtt = R and rttvar = R / 2; each later one moves rttvar a
    // quarter of the way to |srtt - R| and srtt an eighth of the way to R. R is at least 1 ns, so an answer can never
    // leave srtt at the 0 that means "none yet".
    latency = latency == 0 ? 1 : latency;
    if (client->latency == 0 || client->latency_backed_off) {
        client->latency = latency;
        client->latency_var = latency / 2;
        client->latency_backed_off = false;
    } else {
        uint32_t deviation = client->latency > latency ? client->latency - latency : latency - client->latency;
        client->latency_var =
            (uint32_t)((((uint64_t)RECOVERY_RTTVAR_KEEP * client->latency_var) + deviation) / RECOVERY_RTTVAR_DIV);
        client->latency = (uint32_t)((((uint64_t)RECOVERY_SRTT_KEEP * client->latency) + latency) / RECOVERY_SRTT_DIV);
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
static bool retransmit_one_sample(struct tt_Context* node, struct tt_Publisher* pub, struct tt_ReliableCache* cache,
                                  uint16_t depth, uint32_t missing_seq_no, const struct tt_Peer* target) {
    bool gone = false;
    struct tt_ReliableCacheIndex* cache_entry =
        find_resendable_cache_entry(cache, depth, missing_seq_no, pub->lifespan_duration_ns, pub->keep_all, &gone);
    if (cache_entry == NULL) {
        return gone;
    }

    // Addressed to the node that asked, where the cached original is addressed to everyone. It goes to
    // that node alone anyway (unicast), so nothing else changes - but it is what lets that node tell
    // this copy from the original that was only late, which its retry-interval estimate depends on
    // (tt_Context.rx_targeted). No wire change: a node of an older build sees a submessage addressed to
    // itself and processes it exactly as before.
    if (!send_cached_record(node, cache->arena + cache_entry->offset, cache_entry->len, true, target)) {
        TT_LOG_WARNING("Cannot retransmit seq_no %u now", missing_seq_no);
        RSTAT_INC(retransmit_tx_fail);
    } else {
        RSTAT_INC(retransmitted);
        pub->retransmitted++;
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
static void retransmit_reliable_samples(struct tt_Context* node, struct tt_Publisher* pub,
                                        struct tt_ReliableCache* cache, uint16_t depth, uint32_t seq_no,
                                        const uint64_t* bitmap, uint16_t words, const struct tt_Peer* target) {
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
// A send addressed to one peer goes from an empty tx_buffer: anything batched there, broadcast-only, leaves
// first as the broadcast it was going to be (flush_pending_before_unicast()'s reasoning).
static void flush_pending_broadcast(struct tt_Context* node) {
    if (node->tx_tail != sizeof(struct tt_Header)) {
        (void)flush_tx(node, node->tx_tail, NULL, 0);
    }
}

// Asks `source` for its endpoint list (rmw_tickle/DISCOVERY_PLAN.md rule 3): an ACKNACK of the built-in
// discovery endpoint naming the generation its summary showed, unicast. request_discovery_list() decides
// when: on a summary showing a generation not yet applied, and again on a timer while the list is missing.
static void send_discovery_request(struct tt_Context* node, uint8_t source, uint32_t generation, uint32_t sender_ip,
                                   uint16_t sender_port) {
    flush_pending_broadcast(node);
    uint32_t old_tx_tail = node->tx_tail;
    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_ACKNACK, source);
    struct tt_AckNackHeader* request = submessage_header != NULL ? encode(node, sizeof(struct tt_AckNackHeader)) : NULL;
    if (request == NULL) {
        rollback(node, old_tx_tail);
        return;
    }
    request->endpoint_id = tt_DISCOVERY_ENDPOINT_ID;
    request->entity_id = tt_DISCOVERY_ENTITY_ID;
    request->sender_entity_id = tt_DISCOVERY_ENTITY_ID;
    request->seq_no = generation;
    request->bitmap_words = 0;
    request->reserved = 0;
    struct tt_Peer target = {source, sender_ip, sender_port};
    if (!end_encode(node, submessage_header, true, &target, 1)) {
        rollback(node, old_tx_tail);
    }
}

static bool discovery_generation_applied(const struct tt_Context* node, uint8_t source, uint32_t generation) {
    return node->update_seen[source] && !node->update_reprocess[source] &&
           node->update_generation[source] == generation;
}

static void discovery_request_retry(struct tt_Context* node, uint64_t time, void* param);

// How long a request to `source` waits for its list before it is sent again (ROADMAP.md 5a): the round trip
// measured to that node plus one tt_CONTEXT_TX_INTERVAL, the flush tick an answer can wait for when it is
// batched (rule 4); tt_DISCOVERY_REQUEST_RETRY, the seed, until a round trip has been measured.
static uint64_t discovery_retry_after(const struct tt_Context* node, uint8_t source) {
    uint32_t rtt = node->discovery_rtt_ns[source];
    return rtt != 0 ? (uint64_t)rtt + tt_CONTEXT_TX_INTERVAL : tt_DISCOVERY_REQUEST_RETRY;
}

// The list `source` announced as `generation` has just been applied: if it was asked for, that is a round
// trip, folded into tt_Context.discovery_rtt_ns the way RFC 6298 smooths srtt (gain 1/8; the first sample
// is taken whole). An announce nobody asked for - a broadcast change, a periodic one - times nothing.
static void note_discovery_round_trip(struct tt_Context* node, uint8_t source, uint32_t generation) {
    for (int i = 0; i < tt_DISCOVERY_PENDING_REQUESTS; i++) {
        const struct tt_DiscoveryRequest* request = &node->discovery_requests[i];
        if (request->attempts == 0 || request->source != source || request->generation != generation) {
            continue;
        }
        uint64_t elapsed = tt_get_ns() - request->first_sent_ns;
        // 1 ns at least: 0 means "not measured", and an answer delivered within the same clock reading is
        // a measured round trip of nothing (two nodes in one process on the mock clock).
        uint32_t sample = elapsed >= UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
        if (sample == 0) {
            sample = 1;
        }
        uint32_t srtt = node->discovery_rtt_ns[source];
        if (srtt == 0) {
            node->discovery_rtt_ns[source] = sample;
        } else {
            node->discovery_rtt_ns[source] = (uint32_t)((int64_t)srtt + (((int64_t)sample - (int64_t)srtt) / 8));
        }
        return;
    }
}

// Makes sure discovery_request_retry() runs by `due_ns`: moves the one scheduled entry earlier if needed. Each
// peer has its own retry delay (discovery_retry_after()), so a request to a near peer can fall due before the
// entry already armed for a far one - with one delay for all, a newer request never did.
static void arm_discovery_request_retry(struct tt_Context* node, uint64_t due_ns) {
    if (node->discovery_retry_scheduled) {
        if (due_ns >= node->discovery_retry_ns) {
            return;
        }
        (void)tt_Context_unschedule(node, discovery_request_retry, NULL);
        node->discovery_retry_scheduled = false;
    }
    if (tt_Context_schedule(node, due_ns, discovery_request_retry, NULL)) {
        node->discovery_retry_scheduled = true;
        node->discovery_retry_ns = due_ns;
    } else {
        TT_LOG_ERROR("Cannot schedule discovery_request_retry"); // the next summary asks again
    }
}

// Re-sends each open request whose list has not arrived within discovery_retry_after(), and closes the
// ones answered or out of attempts - the peer's next summary asks again after that. Runs only while some
// request is open.
static void discovery_request_retry(struct tt_Context* node, uint64_t time, void* param) {
    UNUSED(param);
    node->discovery_retry_scheduled = false;
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < tt_DISCOVERY_PENDING_REQUESTS; i++) {
        struct tt_DiscoveryRequest* request = &node->discovery_requests[i];
        if (request->attempts == 0) {
            continue;
        }
        if (discovery_generation_applied(node, request->source, request->generation)) {
            request->attempts = 0;
            continue;
        }
        uint64_t retry_after = discovery_retry_after(node, request->source);
        if (time - request->sent_ns >= retry_after) {
            if (request->attempts >= tt_DISCOVERY_REQUEST_ATTEMPTS) {
                request->attempts = 0;
                continue;
            }
            send_discovery_request(node, request->source, request->generation, request->ip, request->port);
            request->attempts++;
            request->sent_ns = time;
        }
        uint64_t due = request->sent_ns + retry_after;
        next = due < next ? due : next;
    }
    if (next != UINT64_MAX) {
        arm_discovery_request_retry(node, next);
    }
}

// Asks `source` for its list, and keeps the request open so discovery_request_retry() can ask again. A
// request already open for this source and generation is left to the retry: another summary adds nothing.
static void request_discovery_list(struct tt_Context* node, uint8_t source, uint32_t generation, uint32_t sender_ip,
                                   uint16_t sender_port) {
    struct tt_DiscoveryRequest* slot = NULL;
    for (int i = 0; i < tt_DISCOVERY_PENDING_REQUESTS; i++) {
        struct tt_DiscoveryRequest* request = &node->discovery_requests[i];
        if (request->attempts != 0 && request->source == source) {
            if (request->generation == generation) {
                return;
            }
            slot = request; // a newer generation replaces the one asked for
            break;
        }
        if (request->attempts == 0 && slot == NULL) {
            slot = request;
        }
    }
    uint64_t now = tt_get_ns();
    send_discovery_request(node, source, generation, sender_ip, sender_port);
    if (slot == NULL) {
        return; // every slot busy: sent, not retried - the next summary asks again
    }
    *slot = (struct tt_DiscoveryRequest) {generation, sender_ip, sender_port, source, 1, now, now};
    arm_discovery_request_retry(node, now + discovery_retry_after(node, source));
}

// A discovery summary from `source` (send_discovery_summary()). Liveliness first, whatever else it says -
// exactly what an announce refreshes (rule 1). A generation already applied needs nothing more (rule 2); any
// other - a change missed, a node never heard in full - is asked for (rule 3), and asked again within
// discovery_retry_after() if the list does not come.
static bool process_discovery_summary(struct tt_Context* node, uint8_t source, uint32_t generation, uint32_t sender_ip,
                                      uint16_t sender_port) {
    node->update_last_seen[source] = tt_get_ns();
    if (discovery_generation_applied(node, source, generation)) {
        return true;
    }
    request_discovery_list(node, source, generation, sender_ip, sender_port);
    return true;
}

// A peer asked for this node's endpoint list (rule 4): the whole announce, unicast to it. When more than
// tt_UNICAST_PEER_THRESHOLD ask within one tt_CONTEXT_TX_INTERVAL tick - a burst of new nodes, say - the next
// becomes one broadcast instead, and later requests in that tick are covered by it.
static void answer_discovery_request(struct tt_Context* node, uint8_t source, uint32_t sender_ip,
                                     uint16_t sender_port) {
    uint64_t tick = tt_get_ns() / tt_CONTEXT_TX_INTERVAL;
    if (tick != node->discovery_reply_tick) {
        node->discovery_reply_tick = tick;
        node->discovery_reply_count = 0;
    }
    if (node->discovery_reply_count > tt_UNICAST_PEER_THRESHOLD) {
        return; // a broadcast of the list is already on its way this tick
    }
    node->discovery_reply_count++;
    if (node->discovery_reply_count > tt_UNICAST_PEER_THRESHOLD) {
        build_and_send_update(node, NULL, 0); // batched broadcast, out on the next flush tick
        return;
    }
    flush_pending_broadcast(node);
    struct tt_Peer requester = {source, sender_ip, sender_port};
    build_and_send_update(node, &requester, 1);
}

static bool process_acknack(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
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
    if (endpoint_id == tt_DISCOVERY_ENDPOINT_ID) {
        answer_discovery_request(node, header->source, sender_ip, sender_port);
        return true;
    }

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
    claim_from_acknack(node, pub, header->source, endpoint_id, sender_entity_id, sender_ip, sender_port);
    record_peer_ack(pub, header->source, sender_entity_id, seq_no);
    pub->ack_solicit_outstanding = false; // answered: the next solicitation may go at once (solicit_ack_throttled())
    // Phase 3 - this ACKNACK may have freed room a refused publish was waiting on. Fired here, from
    // inside tt_Context_poll()'s own packet handling, so the callback runs on the node's thread like
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
    proxy->sub->gap_evicted += skipped - received_in_first(proxy, skipped);
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
static void inform_subscriber_of_heartbeat(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
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
        // by subscriber_incompatible_with_writer()), the Requested side defines what the
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

    // Giving up on a gap is a delivery event: whatever was held behind it has been waiting for
    // something the Publisher has just said is never coming, so it is released now, in order.
    //
    // AFTER both watermark moves in this function, not between them. It used to sit after
    // advance_past_unavailable() and before the oversized-gap jump_ack_baseline() above, so that
    // jump moved the watermark past held samples after they had last been checked - and nothing
    // released them until the next in-order arrival happened to drain, which on a stream that
    // has stopped is never. Every path that moves a watermark has to drain after its last move.
    drain_reorder(node, sub, proxy);

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
    bool timer_was_armed = proxy->acknack_scheduled;
    maybe_arm_acknack_retry(node, proxy);
    if (ctx->flags & tt_HEARTBEAT_FLAG_FINAL) {
        return;
    }
    if (!had_gap) {
        // Healthy, but tt_Publisher_request_ack() explicitly asked anyway - the only way a Publisher ever learns a
        // healthy Subscriber has fully caught up (see maybe_arm_acknack_retry()'s own "a healthy stream needs no
        // ACKNACK at all" comment for why nothing above already sent one).
        send_acknack(node, proxy);
    } else if (timer_was_armed) {
        answer_ack_request(node, proxy);
    }
}

// Whether positions 0..high_bit hold any still missing outside the skip_count runs in skips.
static bool names_any_open(const struct tt_WriterProxy* proxy, int high_bit, const struct acknack_skip* skips,
                           int skip_count) {
    for (int word = 0; word <= high_bit / tt_RELIABLE_BITMAP_WORD_BITS && word < (int)proxy_words(proxy); word++) {
        uint64_t open = ~proxy->received_bitmap[word] & bitmap_range_in_word(word, 0, high_bit);
        for (int skip = 0; skip < skip_count; skip++) {
            open &= ~bitmap_range_in_word(word, skips[skip].low_bit, skips[skip].high_bit);
        }
        if (open != 0) {
            return true;
        }
    }
    return false;
}

// A Heartbeat that asks for an answer (FINAL clear) reached a reader with a gap open and its retry timer armed - which
// maybe_arm_acknack_retry() does not answer: it sends only when it arms. Left unanswered, a refused KEEP_ALL Publisher,
// which sends nothing but these requests (keep_all_resolicit()), waited for the reader's timer: srtt + 4 * rttvar up
// to 64 * srtt, ~130 ms on the rig under 5% loss and past rmw_tickle's 100 ms publish bound (2026-10-07, f3451cd8).
//
// Answered, but leaving out what a remembered request named within the repair transit time (repair_in_flight_ns()):
// those repairs are on their way. The full answer f3451cd8 sent named them again - c6 (P4 KEEP_ALL, 5% loss) at
// +11.2% wire bytes a sample on the rig - and an srtt window still did, +6.5% (d603d369). What a request named longer
// ago is overdue (its repair was lost) and named again; what no remembered request named - a gap this Heartbeat
// revealed - is named too. Nothing left, the answer is a pure acknowledgement: the writer still learns how far this
// reader has got and may ask again at once. The timer is left as it is.
static void answer_ack_request(struct tt_Context* node, struct tt_WriterProxy* proxy) {
    int high_bit = highest_relevant_bit(proxy);
    if (high_bit < 0) {
        return; // no gap after all - the caller's had_gap says otherwise only if this were called without one
    }
    uint64_t now = tt_get_ns();
    uint64_t window = repair_in_flight_ns(node, proxy);
    struct acknack_skip skips[tt_RELIABLE_REQUEST_HISTORY];
    int skip_count = 0;
    for (int slot = 0; slot < tt_RELIABLE_REQUEST_HISTORY; slot++) {
        const struct tt_RepairRequest* request = &proxy->requests[slot];
        if (request->sent_ns == 0 || now - request->sent_ns >= window || request->last_seq_no < proxy->ack_seq_no) {
            continue;
        }
        int64_t low = (int64_t)request->first_seq_no - proxy->ack_seq_no;
        int64_t high = (int64_t)request->last_seq_no - proxy->ack_seq_no;
        skips[skip_count].low_bit = low < 0 ? 0 : (int)low;
        skips[skip_count].high_bit = high > high_bit ? high_bit : (int)high;
        skip_count++;
    }
    if (!names_any_open(proxy, high_bit, skips, skip_count)) {
        RSTAT_INC(ack_request_in_flight);
        send_acknack_range(node, proxy, 0, -1);
        return;
    }
    RSTAT_INC(ack_request_answered);
    send_acknack_skipping(node, proxy, 0, high_bit, skips, skip_count);
}

// QoS roadmap #5 (RELIABILITY) follow-up - process_submessage()'s own new HEARTBEAT case. See
// struct tt_HeartbeatHeader's own doc comment (tickle.h) for what this is; matches process_data()/
// process_acknack()'s own decode-then-dispatch shape and "silently no-op if nothing local
// matches" convention. Returns false only on a genuine decode failure (illegal header) - a
// Heartbeat with no local match, or for a non-reliable Subscriber, is a normal no-op, not an
// error, same as those two functions' own equivalent cases.
static bool process_heartbeat(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
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

    if ((flags & tt_HEARTBEAT_FLAG_LIVELINESS) != 0) {
        note_manual_assertion(node, header->source, endpoint_id, entity_id);
        return true; // an assertion only - see the flag's comment (tickle.h)
    }
    if (endpoint_id == tt_DISCOVERY_ENDPOINT_ID) {
        return process_discovery_summary(node, header->source, last_seq_no, sender_ip, sender_port);
    }
    struct heartbeat_ctx ctx = {header, sender_ip, sender_port, entity_id, first_available_seq_no, last_seq_no, flags};
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id, inform_subscriber_of_heartbeat, &ctx);
    return true;
}

#if tt_FRAG_ENABLED
// CDR bytes a reassembly slot can hold.
#define FRAG_SLOT_CAPACITY (sizeof(((struct tt_FragSlot*)0)->bytes) - tt_FRAG_DATA_HEADER_LENGTH)
_Static_assert(FRAG_SLOT_CAPACITY >= FRAG_MAX_CDR, "a reassembly slot must hold the largest fragmented sample");

// Where fragment `index` starts within the sample's CDR, given the payload of a full continuation.
static uint32_t frag_offset(uint32_t index, uint32_t cont_length) {
    return index == 0 ? 0 : (cont_length - (uint32_t)tt_FRAG_FIRST_SHORTFALL) + ((index - 1) * cont_length);
}

// Whether the count-th occurrence of a fragment event is logged: the 1st, 10th, 100th ... (is_power_of_ten()).
static bool frag_log_due(uint64_t count) {
    return count <= UINT32_MAX && is_power_of_ten((uint32_t)count);
}

static uint64_t frag_all_received(uint32_t count) {
    return count >= tt_FRAG_MAX_COUNT ? UINT64_MAX : (1ULL << count) - 1; // tt_FRAG_MAX_COUNT is the bitmap width
}

static bool frag_slot_is(const struct tt_FragSlot* slot, uint8_t source, uint32_t entity_id, uint32_t seq_no) {
    return slot->source == source && slot->entity_id == entity_id && slot->seq_no == seq_no;
}

static bool frag_claimed_before(const struct tt_FragSlot* slot, const struct tt_FragSlot* than) {
    return than == NULL || (int32_t)(slot->claimed - than->claimed) < 0;
}

// The slot already collecting this sample - or, marked done, the one that completed it (the caller drops
// the fragment as a duplicate) - or a newly claimed one: a slot never used, else the one whose completed
// sample is oldest, else the incomplete reassembly claimed longest ago, which is abandoned and counted.
// A claimed slot stays free (received == 0) until a fragment has actually been placed in it.
static struct tt_FragSlot* frag_slot_for(struct tt_Context* node, uint8_t source, uint32_t entity_id, uint32_t seq_no,
                                         uint8_t frag_count) {
    struct tt_FragSlot* unused = NULL;
    struct tt_FragSlot* oldest_done = NULL;
    struct tt_FragSlot* oldest = NULL;
    for (int i = 0; i < tt_FRAG_REASSEMBLY_SLOTS; i++) {
        struct tt_FragSlot* slot = &node->frag_slots[i];
        if ((slot->received != 0 || slot->done) && frag_slot_is(slot, source, entity_id, seq_no)) {
            return slot;
        }
        if (slot->received != 0) {
            oldest = frag_claimed_before(slot, oldest) ? slot : oldest;
        } else if (slot->done) {
            oldest_done = frag_claimed_before(slot, oldest_done) ? slot : oldest_done;
        } else if (unused == NULL) {
            unused = slot;
        }
    }
    struct tt_FragSlot* slot = unused != NULL ? unused : oldest_done;
    if (slot == NULL) {
        slot = oldest;
        node->frag_abandoned++;
        // Throttled like the RxO drop: under loss this is routine - a sample that lost a fragment holds its
        // slot until its retransmission, which carries every fragment again and so needs nothing the slot
        // held - and one line per event was 28,542 lines in a 5 s p4 run at 5% loss, each a write() of
        // its own on the receive path being measured. The counter keeps the exact figure.
        if (frag_log_due(node->frag_abandoned)) {
            TT_LOG_WARNING("Abandoning reassembly of seq_no %u from node %u for a newer sample (abandoned #%lu)",
                           slot->seq_no, slot->source, (unsigned long)node->frag_abandoned);
        }
    }
    slot->received = 0;
    slot->done = false;
    slot->source = source;
    slot->entity_id = entity_id;
    slot->seq_no = seq_no;
    slot->frag_count = frag_count;
    slot->cont_length = 0;
    slot->last_length = 0;
    slot->claimed = node->frag_clock++;
    return slot;
}

// Records the continuation payload size, learned from a non-last fragment, and moves a last fragment
// parked while it was unknown to where it belongs. False if the sizes cannot belong to one sample.
static bool frag_learn_cont_length(struct tt_FragSlot* slot, uint32_t cont_length) {
    if (slot->cont_length != 0) {
        return slot->cont_length == cont_length;
    }
    uint32_t last = (uint32_t)slot->frag_count - 1;
    if (cont_length <= tt_FRAG_FIRST_SHORTFALL || frag_offset(last, cont_length) >= FRAG_SLOT_CAPACITY) {
        return false;
    }
    uint8_t* cdr = slot->bytes + tt_FRAG_DATA_HEADER_LENGTH;
    if ((slot->received & (1ULL << last)) != 0) {
        uint32_t offset = frag_offset(last, cont_length);
        if (slot->last_length > cont_length || offset + slot->last_length > FRAG_SLOT_CAPACITY) {
            return false;
        }
        memmove(cdr + offset, cdr + FRAG_SLOT_CAPACITY - slot->last_length, slot->last_length);
    }
    slot->cont_length = (uint16_t)cont_length;
    return true;
}

// Copies one fragment's payload into its slot. False when it contradicts what the slot already knows.
static bool frag_place(struct tt_FragSlot* slot, uint32_t index, const uint8_t* payload, uint32_t length) {
    uint32_t last = (uint32_t)slot->frag_count - 1;
    if ((slot->received & (1ULL << index)) != 0) {
        return true; // a duplicate adds nothing
    }
    if (index != last && !frag_learn_cont_length(slot, index == 0 ? length + tt_FRAG_FIRST_SHORTFALL : length)) {
        return false;
    }
    uint8_t* cdr = slot->bytes + tt_FRAG_DATA_HEADER_LENGTH;
    uint32_t offset;
    if (index != last) {
        offset = frag_offset(index, slot->cont_length);
    } else if (slot->cont_length == 0) {
        offset = FRAG_SLOT_CAPACITY - length; // parked until the continuation size is known
    } else if (length <= slot->cont_length) {
        offset = frag_offset(last, slot->cont_length);
    } else {
        return false;
    }
    if (length > FRAG_SLOT_CAPACITY || offset > FRAG_SLOT_CAPACITY - length) {
        return false;
    }
    _tt_memcpy(cdr + offset, payload, length);
    if (index == last) {
        slot->last_length = (uint16_t)length;
    }
    slot->received |= 1ULL << index;
    return true;
}

// One user-data fragment for best-effort Subscribers, placed in the reassembly pool under its sample's
// seq_no. Once every fragment has arrived, the sample goes to process_data_for() exactly as a DATA carrying
// it would have, for best-effort Subscribers only. data_header is the FRAG_FIRST's DataHeader, NULL for a
// continuation.
//
// A partial sample is not dropped when a newer one from the same writer starts, although a seq_no gap
// would allow it: a gap from reordering is not a loss, and a sample whose last datagram was merely
// overtaken would be lost for nothing. The pool's own rule - the reassembly claimed longest ago goes
// first - clears what really was lost.
static bool reassemble_fragment(struct tt_Context* node, struct tt_Header* header, const uint8_t* payload,
                                uint32_t length, const struct tt_DataHeader* data_header, uint32_t entity_id,
                                uint32_t seq_no, uint32_t index, uint32_t count, uint32_t sender_ip,
                                uint16_t sender_port) {
    if (count < 2 || count > tt_FRAG_MAX_COUNT || index >= count || length == 0) {
        node->frag_dropped++;
        if (frag_log_due(node->frag_dropped)) {
            TT_LOG_ERROR("Illegal fragment %u of %u (%u bytes, dropped #%lu)", index, count, length,
                         (unsigned long)node->frag_dropped);
        }
        return false;
    }

    struct tt_FragSlot* slot = frag_slot_for(node, header->source, entity_id, seq_no, (uint8_t)count);
    if (slot->done) {
        node->frag_duplicate++; // the sample is already whole and delivered
        return true;
    }
    if (slot->frag_count != count || !frag_place(slot, index, payload, length)) {
        node->frag_dropped++;
        if (frag_log_due(node->frag_dropped)) {
            TT_LOG_WARNING("Fragment %u of seq_no %u from node %u is inconsistent with its sample - dropped (#%lu)",
                           index, seq_no, header->source, (unsigned long)node->frag_dropped);
        }
        return false;
    }
    if (data_header != NULL) {
        _tt_memcpy(slot->bytes, data_header, sizeof(struct tt_DataHeader));
    }
    if (slot->received != frag_all_received(count)) {
        return true;
    }

    uint32_t cdr_len = frag_offset(count - 1, slot->cont_length) + slot->last_length;
    node->frag_reassembled++;
    bool processed = process_data_for(node, header, slot->bytes, 0, tt_FRAG_DATA_HEADER_LENGTH + cdr_len, sender_ip,
                                      sender_port, true);
    slot->received = 0;
    slot->done = true;
    return processed;
}

// Whether the reassembly pool is already collecting this sample.
static bool frag_pool_has(const struct tt_Context* node, uint8_t source, uint32_t entity_id, uint32_t sample_seq_no) {
    for (int i = 0; i < tt_FRAG_REASSEMBLY_SLOTS; i++) {
        const struct tt_FragSlot* slot = &node->frag_slots[i];
        if (slot->received != 0 && frag_slot_is(slot, source, entity_id, sample_seq_no)) {
            return true;
        }
    }
    return false;
}

// Stores a fragment for a RELIABLE Subscriber in its reorder slot (seq_no % slots), where it waits until
// its whole sample is in order. False when there is no room - no buffer, a slot too small, or the slot
// taken by another writer's datagram; the caller then leaves it unrecorded, so it is asked for again.
static bool reorder_store_fragment(struct tt_Context* node, struct tt_Subscriber* sub,
                                   const struct data_delivery_ctx* ctx, bool is_native) {
    if (frag_fast_holds(node, sub, NULL)) {
        frag_fast_spill(node); // the fast path's sample needs this buffer empty to be moved into it
    }
    uint32_t length = ctx->tail - ctx->head;
    if (reorder_payload_capacity(sub) < length) {
        return false;
    }
    struct tt_ReorderSlot* slot = reorder_writer_slot(sub, ctx->header->source, ctx->entity_id, ctx->seq_no);
    if (slot->occupied) {
        return false;
    }
    slot->seq_no = ctx->seq_no;
    slot->timestamp = ctx->timestamp;
    slot->entity_id = ctx->entity_id;
    slot->context_id = ctx->header->source;
    slot->length = (uint16_t)length;
    slot->is_native = is_native;
    slot->via_data_port = node->rx_via_data_port;
    slot->frag_index = ctx->frag_index;
    slot->frag_count = ctx->frag_count;
    slot->occupied = true;
    _tt_memcpy(reorder_slot_payload(slot), ctx->buffer + ctx->head, length);
    sub->reorder_held++;
    if (sub->reorder_held > sub->reorder_held_peak) {
        sub->reorder_held_peak = sub->reorder_held;
    }
    return true;
}

// The in-order fast path (2026-10-06). A fragment that is the next datagram its writer's tracking expects,
// of a sample whose earlier fragments all came the same way, is put together in the node's frag_scratch and
// the sample delivered from there when its last fragment lands - without touching the reorder buffer, which
// the ordinary path writes every fragment into first. At the bench's 4096 slots of 2840 bytes that buffer is
// 11.6 MB, and each in-order sample wrote 5.7 KB of it that had never been touched: first-touch page faults
// on every sample of the first lap, and cold lines after. A DATA in order never touched it either.
//
// One sample at a time per node, since frag_scratch is one per node. It takes a sample only when its
// Subscriber holds nothing at all, the scratch is free, and a slot could have taken each fragment (so
// frag_fast_spill() can always hand it over). Anything else - a second writer, a second Subscriber of the
// same writer, a fragment out of order - goes the ordinary way, and moves this sample there first if it
// would disturb it. False: not taken, and the caller stores it as before.
static bool frag_fast_take(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_WriterProxy* proxy,
                           const struct data_delivery_ctx* ctx, bool is_native) {
    uint32_t length = ctx->tail - ctx->head;
    uint32_t index = ctx->frag_index;
    // In order: the next datagram the writer's tracking expects. While a sample is in the scratch the
    // watermark moves only as its fragments arrive (anything else moves it out first), so for a continuation
    // this also says it is the next fragment of that sample - given that its index is the next one.
    if (ctx->seq_no != proxy->ack_seq_no || reorder_payload_capacity(sub) < length) {
        return false;
    }
    if (index == 0) {
        if (node->frag_fast_sub != NULL || sub->reorder_held != 0) {
            return false;
        }
        node->frag_fast_context_id = ctx->header->source;
        node->frag_fast_entity_id = ctx->entity_id;
        node->frag_fast_seq_no = ctx->seq_no;
        node->frag_fast_count = ctx->frag_count;
        node->frag_fast_timestamp = ctx->timestamp;
        node->frag_fast_is_native = is_native;
        node->frag_fast_via_data_port = node->rx_via_data_port;
        node->frag_fast_first_length = (uint16_t)length;
        node->frag_fast_cont_length = 0;
        node->frag_fast_length = 0;
        node->frag_fast_placed = 0;
    } else if (!frag_fast_holds(node, sub, proxy) || index != node->frag_fast_placed ||
               ctx->frag_count != node->frag_fast_count ||
               // every continuation but the last is one size, which is what lets frag_fast_spill() cut them apart
               (index > 1 && index + 1U != node->frag_fast_count && length != node->frag_fast_cont_length)) {
        return false;
    }
    if (node->frag_fast_length + length > sizeof(node->frag_scratch) - 8) {
        return false; // only from a sender past tt_MAX_SAMPLE_LENGTH; the ordinary path drops its sample
    }
    _tt_memcpy(node->frag_scratch + 4 + node->frag_fast_length, ctx->buffer + ctx->head, length);
    node->frag_fast_sub = sub;
    // Always new: the writer is tracked and this is its watermark, which nothing has received yet.
    (void)update_reliable_ack(node, sub, ctx->seq_no, ctx->header->source, ctx->entity_id, ctx->sender_ip,
                              ctx->sender_port);
    if (index == 1) {
        node->frag_fast_cont_length = (uint16_t)length;
    }
    node->frag_fast_length += length;
    node->frag_fast_placed++;
    if (node->frag_fast_placed < node->frag_fast_count) {
        return true;
    }
    node->frag_fast_sub = NULL;
    node->frag_reassembled++;
    deliver_in_order(node, sub, proxy, node->frag_fast_seq_no, node->frag_fast_timestamp, node->frag_scratch + 4,
                     node->frag_fast_length, node->frag_fast_is_native, node->frag_fast_via_data_port, NULL);
    // Nothing is held, so this is where a drain would leave the cursor. Left behind, the first drain after a run
    // of fast samples would walk a window of cold slot headers to find nothing.
    proxy->reorder_cursor = proxy->ack_seq_no;
    return true;
}

// One fragment for a RELIABLE Subscriber, under its own seq_no (DATAFRAG_PLAN.md section 13): recorded in
// the writer's tracking exactly as a DATA is, so the ordinary ACKNACK names it if it goes missing and the
// writer resends that datagram alone.
//
// Stored first, recorded second. Once a seq_no is acknowledged the writer never sends it again, so a
// fragment must already be somewhere that keeps it by then - an acknowledged fragment discarded is a
// sample lost for good. One that cannot be stored is left unrecorded instead, which is the reorder
// buffer's own overflow rule: asked for again, never lost.
static void accept_reliable_fragment(struct tt_Context* node, struct tt_Subscriber* sub, struct data_delivery_ctx* ctx,
                                     bool is_native) {
    // A continuation only reaches a Subscriber already tracking its writer (deliver_user_fragment()), so
    // first contact is always a FRAG_FIRST or a DATA, and the baseline update_reliable_ack() pins to it is
    // a sample's start. A continuation that overtook its writer's very first datagram is not taken; it
    // stays unrecorded and the ordinary ACKNACK asks for it again.
    struct tt_WriterProxy* proxy = find_writer_proxy(sub, ctx->header->source, ctx->entity_id);
    if (proxy != NULL) {
        uint64_t offset = (uint64_t)ctx->seq_no - proxy->ack_seq_no;
        if (ctx->seq_no < proxy->ack_seq_no ||
            (offset < proxy_window_bits(proxy) && bitmap_test_bit(proxy->received_bitmap, (uint32_t)offset))) {
            node->frag_duplicate++; // already here, or already past
            return;
        }
        if (frag_fast_take(node, sub, proxy, ctx, is_native)) {
            return;
        }
    }
    if (!reorder_store_fragment(node, sub, ctx, is_native)) {
        sub->reorder_overflow++;
        if (is_power_of_ten(sub->reorder_overflow)) {
            TT_LOG_WARNING(
                "Subscriber %u cannot hold fragment seq_no %u (%u slots of %u bytes): it will be requested "
                "again (occurrence #%u). A RELIABLE Subscriber of a fragmented topic needs a reorder buffer.",
                sub->endpoint.id, ctx->seq_no, sub->reorder_slots, sub->reorder_slot_bytes, sub->reorder_overflow);
        }
        return;
    }
    struct tt_ReorderSlot* slot = reorder_writer_slot(sub, ctx->header->source, ctx->entity_id, ctx->seq_no);
    bool recorded = update_reliable_ack(node, sub, ctx->seq_no, ctx->header->source, ctx->entity_id, ctx->sender_ip,
                                        ctx->sender_port);
    proxy = find_writer_proxy(sub, ctx->header->source, ctx->entity_id);
    if (!recorded || proxy == NULL) {
        reorder_release(sub, slot); // a duplicate after all, or no tracking to order it by
        return;
    }
    drain_reorder(node, sub, proxy);
}

// for_each_endpoint()'s visitor for one fragment: a RELIABLE Subscriber takes it now, datagram by
// datagram; a best-effort one takes the whole sample from the reassembly pool (deliver_user_fragment()).
struct frag_route_ctx {
    struct data_delivery_ctx* data;
    bool best_effort_seen;
};

static void route_fragment_to_subscriber(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    struct frag_route_ctx* route = (struct frag_route_ctx*)ctx_ptr;
    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
    struct data_delivery_ctx* ctx = route->data;
    if (subscriber_incompatible_with_writer(node, sub, ctx->header->source, endpoint->id, ctx->entity_id)) {
        return;
    }
    if (!sub->reliable || TT_ORDERING_DISABLED) {
        route->best_effort_seen = true;
        return;
    }
    accept_reliable_fragment(node, sub, ctx, tt_is_native_endian(ctx->header));
}

// One user-data fragment (not discovery, which process_frag() routes first). A FRAG_FIRST names its
// endpoint; a FRAG_CONT does not, and goes to every Subscriber tracking its writer - or, when none is
// tracking it yet (a writer's first sample, its continuation overtaking its first fragment), to the pool
// if the node has any best-effort Subscriber at all. seq_no is this datagram's own; its sample is
// seq_no - index. Best-effort Subscribers get the sample whole from the node's reassembly pool.
static bool deliver_user_fragment(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                                  uint32_t tail, const struct tt_DataHeader* data_header, uint32_t entity_id,
                                  uint32_t seq_no, uint32_t index, uint32_t count, uint32_t sender_ip,
                                  uint16_t sender_port) {
    uint32_t length = tail - head;
    if (count < 2 || count > tt_FRAG_MAX_COUNT || index >= count || length == 0) {
        node->frag_dropped++;
        if (frag_log_due(node->frag_dropped)) {
            TT_LOG_ERROR("Illegal fragment %u of %u (%u bytes, dropped #%lu)", index, count, length,
                         (unsigned long)node->frag_dropped);
        }
        return false;
    }
    struct data_delivery_ctx ctx = {
        .header = header,
        .endpoint_id = data_header != NULL ? rd32(header, data_header->endpoint_id) : 0,
        .entity_id = entity_id,
        .seq_no = seq_no,
        .timestamp = data_header != NULL ? timestamp_from_wire(node, rd32(header, data_header->timestamp)) : 0,
        .buffer = buffer,
        .head = head,
        .tail = tail,
        .sender_ip = sender_ip,
        .sender_port = sender_port,
        .frag_index = (uint8_t)index,
        .frag_count = (uint8_t)count,
    };
    struct frag_route_ctx route = {&ctx, false};
    if (data_header != NULL) {
        for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, ctx.endpoint_id, route_fragment_to_subscriber, &route);
    } else {
        bool tracked = false;
        bool any_best_effort = false;
        for (uint32_t i = 0; i < node->endpoint_count; i++) {
            struct tt_Endpoint* endpoint = node->endpoints[i];
            if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_SUBSCRIBER) {
                continue;
            }
            struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
            any_best_effort = any_best_effort || !sub->reliable || TT_ORDERING_DISABLED;
            if (find_writer_proxy(sub, header->source, entity_id) != NULL) {
                tracked = true;
                route_fragment_to_subscriber(node, endpoint, &route);
            }
        }
        route.best_effort_seen = route.best_effort_seen || (!tracked && any_best_effort);
    }
    uint32_t sample_seq_no = seq_no - index;
    if (route.best_effort_seen || frag_pool_has(node, header->source, entity_id, sample_seq_no)) {
        return reassemble_fragment(node, header, buffer + head, length, data_header, entity_id, sample_seq_no, index,
                                   count, sender_ip, sender_port);
    }
    return true;
}
#endif

// One fragment, FRAG_FIRST or FRAG_CONT, from header->source. A discovery announce's fragment (its
// entity_id is tt_DISCOVERY_ENTITY_ID) is whole entities and goes straight to process_announce(), in
// every build; anything else is user data for the reassembly pool, which exists only when fragmentation
// is compiled in - without it, a sample over this node's limit could not be delivered anyway, and its
// fragments are passed over.
static bool process_frag(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                         uint32_t tail, uint8_t type, uint32_t sender_ip, uint16_t sender_port) {
    const struct tt_DataHeader* data_header = NULL;
    uint32_t entity_id;
    uint32_t seq_no;
    uint32_t index;
    uint32_t count;
    if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST) {
        struct tt_FragFirstHeader* first = decode(node, buffer, &head, tail, sizeof(struct tt_FragFirstHeader));
        if (first == NULL) {
            TT_LOG_ERROR("Illegal FragFirstHeader");
            return false;
        }
        data_header = &first->data;
        entity_id = rd32(header, first->data.entity_id);
        seq_no = rd32(header, first->data.seq_no);
        index = 0;
        count = first->frag_count;
    } else {
        struct tt_FragContHeader* cont = decode(node, buffer, &head, tail, sizeof(struct tt_FragContHeader));
        if (cont == NULL) {
            TT_LOG_ERROR("Illegal FragContHeader");
            return false;
        }
        entity_id = rd32(header, cont->entity_id);
        seq_no = rd32(header, cont->seq_no);
        index = cont->frag_index;
        count = cont->frag_count;
    }

    if (entity_id == tt_DISCOVERY_ENTITY_ID) {
        if (count < 2) {
            TT_LOG_ERROR("Illegal announce fragment %u of %u", index, count);
            return false;
        }
        return process_announce(node, header, buffer, head, tail, sender_ip, sender_port, seq_no, (uint8_t)index,
                                (uint8_t)count);
    }
#if tt_FRAG_ENABLED
    return deliver_user_fragment(node, header, buffer, head, tail, data_header, entity_id, seq_no, index, count,
                                 sender_ip, sender_port);
#else
    UNUSED(data_header);
    UNUSED(sender_ip);
    UNUSED(sender_port);
    TT_LOG_DEBUG("Fragment skipped: built without fragmentation");
    return true;
#endif
}

static bool process_submessage(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                               uint32_t body_tail, const struct tt_SubmessageHeader* submessage_header,
                               uint32_t sender_ip, uint16_t sender_port, bool self_sent, enum tt_Transport transport) {
    // Each process_X() below already logs its own specific reason on failure, so this switch
    // doesn't log again on top of that - only the type dispatch itself gets a message here.
    // sender_ip/sender_port (this packet's own source, from tt_receive() - see
    // handle_receive_result()) reach process_callrequest() (to unicast the CallResponse straight
    // back), process_announce() (to learn/refresh a peer table entry - see decode_update_
    // entities()'s own comment), process_data() (to remember where a reliable Subscriber's own
    // ACKNACK should go, QoS roadmap #5), and process_acknack() (to unicast a retransmit straight
    // back the same way CallResponse does); process_callresponse() doesn't need them.
    //
    // self_sent (this whole packet's own header->source == node->id) only suppresses the two
    // topic-shaped types below, not CALLREQUEST/CALLRESPONSE - rmw_tickle/PLAN.md's own Milestone
    // 17 finding: a client and its service can end up on the exact same tt_Context (the only
    // topology rmw_tickle's one-node-per-process model allows), and RPC has no separate in-
    // process delivery path the way "a node already has its own published data locally" is true
    // for pub/sub - the request/response datagrams *are* the only path, so suppressing them here
    // made a co-located client structurally unable to ever reach its own service.
    switch (submessage_header->type) {
    case tt_SUBMESSAGE_TYPE_DATA:
        if (!self_sent) {
            process_data(node, header, buffer, head, body_tail, sender_ip, sender_port);
        }
        return true;
    case tt_SUBMESSAGE_TYPE_ACKNACK:
        // QoS roadmap #5 (RELIABILITY/RELIABLE) - self_sent-guarded for the same reason as DATA/
        // DATA above: a reliable Subscriber never sees its own co-located Publisher's DATA in
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
    case tt_SUBMESSAGE_TYPE_FRAG_FIRST:
    case tt_SUBMESSAGE_TYPE_FRAG_CONT:
        if (!self_sent) {
            process_frag(node, header, buffer, head, body_tail, submessage_header->type, sender_ip, sender_port);
        }
        return true;
    case tt_SUBMESSAGE_TYPE_SHM_DATA:
        if (transport != tt_TRANSPORT_SHM) {
            // SHM_PLAN 6e's safety condition, which it names as not skippable. This record declares how
            // many seq_nos it covers, which only something with write access to a segment may say; from
            // the socket it is an injection or a bug, never a rolling upgrade. Counted apart from version
            // skew for that reason, and dropped rather than returned as an error - the default case below
            // explains why a peer must never be able to end a local poll loop.
            //
            // Logged at the 1st, 10th, 100th ... refusal, like the RxO drop: this is untrusted input, and a log line
            // per datagram would hand whoever can reach the port a way to fill the disk (and slow the poll loop with
            // it). Until 2026-10-09 every refusal was logged.
            node->rx_shm_only_on_socket++;
            node->rx_malformed_drops++;
            if (node->rx_shm_only_on_socket <= UINT32_MAX && is_power_of_ten((uint32_t)node->rx_shm_only_on_socket)) {
                TT_LOG_WARNING("Shared-memory-only submessage type %u arrived over the socket from %u.%u.%u.%u:%u, "
                               "refused (refusal #%lu)",
                               (unsigned)submessage_header->type, (unsigned)((sender_ip >> 24) & MASK_8BIT),
                               (unsigned)((sender_ip >> 16) & MASK_8BIT),
                               (unsigned)((sender_ip >> BITS_IN_1BYTE) & MASK_8BIT), (unsigned)(sender_ip & MASK_8BIT),
                               (unsigned)sender_port, (unsigned long)node->rx_shm_only_on_socket);
            }
            return true;
        }
        // A segment record of this type, which nothing produces yet - SHM_PLAN 6e's span step is what
        // will. Skipping is the honest handling until then, and it keeps "refused on the socket" apart
        // from "this build does not read spans", which are different answers.
        TT_LOG_WARNING("Shared-memory DATA record received, but this build does not read seq spans yet; skipping");
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
static bool validate_packet_header(struct tt_Context* node, struct tt_Header* header) {
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
        node->version_mismatch_drops++; // (g11) counted, and dropped by the caller - never fatal
        return false;
    }

    return true;
}

enum submessage_walk_result { SUBMSG_ERROR, SUBMSG_DONE, SUBMSG_CONTINUE };

// Whether the DATA submessage whose body is buffer[head..body_tail) is a discovery announce
// (tt_DISCOVERY_ENDPOINT_ID, tickle.h) rather than a sample.
static bool data_is_announce(struct tt_Header* header, const uint8_t* buffer, uint32_t head, uint32_t body_tail) {
    if (body_tail < head || body_tail - head < sizeof(struct tt_DataHeader)) {
        return false;
    }
    const struct tt_DataHeader* data_header = (const struct tt_DataHeader*)(buffer + head);
    return rd32(header, data_header->endpoint_id) == tt_DISCOVERY_ENDPOINT_ID &&
           rd32(header, data_header->entity_id) == tt_DISCOVERY_ENTITY_ID;
}

// Hands one submessage, its body buffer[head..body_tail), to its handler if it is addressed to this node - from
// the classic walk below or from a single-submessage datagram (process_packet()).
static bool dispatch_submessage(struct tt_Context* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                                uint32_t body_tail, struct tt_SubmessageHeader* submessage_header, uint32_t sender_ip,
                                uint16_t sender_port, bool self_sent, enum tt_Transport transport) {
    // Counted before the receiver filter below, deliberately: a node's own DATA is addressed to
    // whoever it was published to, not to itself, so filtering first would hide exactly the case
    // this counter exists to detect. A sample only: since tt_VERSION 7 an announce is a DATA too.
    if (self_sent && submessage_header->type == tt_SUBMESSAGE_TYPE_DATA &&
        !data_is_announce(header, buffer, head, body_tail)) {
        node->rx_self_sent_data++;
        if (node->rx_via_data_port) {
            node->rx_self_sent_data_unicast++;
        }
    }

    node->rx_targeted = submessage_header->receiver == node->id;
    if (submessage_header->receiver != tt_SUBMESSAGE_ID_ALL && submessage_header->receiver != node->id) {
        return true;
    }
    return process_submessage(node, header, buffer, head, body_tail, submessage_header, sender_ip, sender_port,
                              self_sent, transport);
}

// Decodes and dispatches one submessage starting at *head, advancing *head past it.
static enum submessage_walk_result process_one_submessage(struct tt_Context* node, struct tt_Header* header,
                                                          uint8_t* buffer, uint32_t* head, uint32_t tail,
                                                          uint32_t sender_ip, uint16_t sender_port, bool self_sent,
                                                          enum tt_Transport transport) {
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
    if (!dispatch_submessage(node, header, buffer, *head, body_tail, submessage_header, sender_ip, sender_port,
                             self_sent, transport)) {
        return SUBMSG_ERROR;
    }

    *head += sub_length - sizeof(struct tt_SubmessageHeader);
    return SUBMSG_CONTINUE;
}

// A datagram in the single-submessage form (struct tt_SingleHeader, tt_VERSION 10): its header read into the
// two classic ones, so the rest of the receive path is the same whichever form a datagram came in. `single`
// sits at buffer[*head]; *head is advanced past it and the submessage is the rest of the datagram.
static bool read_single_header(const struct tt_SingleHeader* single, uint32_t body_len, struct tt_Header* header,
                               struct tt_SubmessageHeader* submessage) {
    header->magic_value = single->marker == native_single_marker() ? NATIVE_MAGIC_VALUE : REVERSE_MAGIC_VALUE;
    header->version = single->version;
    header->source = single->source;
    if (body_len > UINT16_MAX - sizeof(struct tt_SubmessageHeader)) {
        return false;
    }
    submessage->type = single->type;
    submessage->receiver = tt_SUBMESSAGE_ID_ALL;
    submessage->length = 0; // not read on this path: the body runs to the end of the datagram
    return true;
}

#if tt_DISCOVERY_OPTIONS
#define LOOPBACK_NET 127U
// (g6) Whether a datagram from `ip` is inside the discovery range. LOCALHOST: this host - loopback, or this context's
// own link address - or a static peer (a peer link's address or subnet). OFF: nothing.
static bool sender_in_range(uint32_t ip) {
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return false;
    }
    if ((ip >> 24) == LOOPBACK_NET) {
        return true;
    }
    for (uint8_t i = 0; i < link_count(); i++) {
        const struct _tt_Link* link = &_tt_CONFIG.links[i];
        if (!link->resolved) {
            continue;
        }
        if (link->peer ? (ip & link->resolved_netmask) == (link->resolved_addr & link->resolved_netmask)
                       : ip == link->resolved_addr) {
            return true;
        }
    }
    return false;
}
#endif

static bool process_packet(struct tt_Context* node, uint8_t* buffer, uint32_t head, uint32_t tail, uint32_t sender_ip,
                           uint16_t sender_port, enum tt_Transport transport) {
    struct tt_Header single_header;
    struct tt_SubmessageHeader single_submessage;
    bool single = false;
    struct tt_Header* header = NULL;
    if (tail > head && (buffer[head] == tt_SINGLE_MARKER_LE || buffer[head] == tt_SINGLE_MARKER_BE)) {
        const struct tt_SingleHeader* single_raw = decode(node, buffer, &head, tail, sizeof(struct tt_SingleHeader));
        if (single_raw == NULL || !read_single_header(single_raw, tail - head, &single_header, &single_submessage)) {
            TT_LOG_ERROR("RX buffer underflow");
            return false;
        }
        header = &single_header;
        single = true;
    } else {
        header = decode(node, buffer, &head, tail, sizeof(struct tt_Header));
    }
    if (header == NULL) {
        TT_LOG_ERROR("RX buffer underflow");
        return false;
    }

    if (!validate_packet_header(node, header)) {
        return false;
    }

    // Self sent message - no longer short-circuited here: see process_submessage()'s own comment
    // on why this now only suppresses the topic-shaped types (DATA and its fragments, which carry announces too), not
    // CALLREQUEST/ CALLRESPONSE, and so has to be threaded down per-submessage rather than dropping the whole packet up
    // front.
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range != tt_DISCOVERY_RANGE_SUBNET && !sender_in_range(sender_ip)) {
        node->rx_out_of_range++;
        return true; // (g6) from outside the discovery range: not processed, as if never received
    }
#endif
    bool self_sent = header->source == node->id;
#if tt_CONTEXT_ID_CLAIM
    if (self_sent && !tt_is_own_address(node, sender_ip, sender_port)) {
        handle_id_collision(node, sender_ip, sender_port);
        return true; // another context's packet under this id: neither this context's own nor a peer's to process
    }
    set_id_bit(node->ids_seen, header->source);
#endif
    if (self_sent) {
        node->rx_self_sent++;
    }
    TT_LOG_DEBUG("source: %d%s", header->source, self_sent ? " (self)" : "");

    // Liveliness evidence (2026-09-23, at the user's own direction): ANY validated packet from a
    // node proves that node is alive, not only its periodic announce. A peer that is
    // sending DATA at full rate, or ACKNACKing every gap, is self-evidently running - declaring it
    // dead because its announces happened to be the packets that got dropped is a false positive
    // by construction, and injected loss attacks exactly the channel the old evidence relied on.
    //
    // Not hypothetical, and worse than latent: rmw_tickle's own perf comparison aborted both async
    // runs with "Data consistency violated. Received sample with not strictly higher id. Received
    // sample id 1 Prev. sample id : 7427", each abort immediately preceded by "Node N presumed
    // dead (no UPDATE for 3 consecutive intervals)" - seven such events in one run. Forgetting a
    // live peer makes its next announce read as fresh discovery, and the subscriber is handed the
    // stream from the beginning again.
    //
    // Placed here rather than in each process_X(): this is one site that cannot drift, it runs
    // after validate_packet_header() so a malformed or wrong-version packet extends nobody's
    // lease, and it covers every submessage type including ones added later. Both consumers of
    // this timestamp - check_liveliness()'s node-level sweep and tt_Context_entity_alive()'s
    // per-entity lease - therefore see the same evidence, rather than one of them still believing
    // only announces count.
    //
    // A retransmit or a duplicate counts, deliberately. It is not new information about the data,
    // but it is proof the peer's stack is alive and transmitting, which is the only question being
    // asked here - and under loss, retransmits may be most of what arrives.
    //
    // header->source indexes traffic_last_seen[tt_MAX_CONTEXT_IDS] unchecked, which is safe by
    // construction rather than by luck: source is a uint8_t and that array has exactly 256 entries.
    // Worth stating because a narrower array would make this an out-of-bounds write on a hostile
    // packet, and validate_packet_header() does not range-check the field.
    //
    // This is evidence only - it never shortens or lengthens a timeout on its own. It acts as a
    // veto: check_liveliness() and tt_Context_entity_alive() each still fire on their own announce-
    // based schedule and consult this to refuse to declare dead a node that is plainly still
    // transmitting. Keeping the schedule on the announce clock is what stops detection sliding
    // later, which using traffic as the single clock did measure at about +290ms.
    if (!self_sent) {
        uint64_t now = rx_now(node); // the poll's reading, not a clock read per datagram (D1)
        node->traffic_last_seen[header->source] = now;
        if ((node->liveliness_flags[header->source] & tt_LIVELINESS_SOURCE_LAPSED) != 0) {
            revive_lapsed_entities(node, header->source, now);
        }
    }

    if (single) {
        return dispatch_submessage(node, header, buffer, head, tail, &single_submessage, sender_ip, sender_port,
                                   self_sent, transport);
    }
    while (true) {
        enum submessage_walk_result result =
            process_one_submessage(node, header, buffer, &head, tail, sender_ip, sender_port, self_sent, transport);
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
// The one place a datagram enters core, whichever transport carried it - which is what lets the
// transport be a required parameter rather than something the counting infers. An arrival cannot be
// recorded without saying where it came from, the same way count_udp() makes a fallback impossible
// to count without naming its reason. That matters more here than on the send side: a sum like
// rx_udp + rx_shm agreeing with a packet count would look like proof while a misattributed arrival
// hid inside it.
static tt_ret_t process_datagram_locked(struct tt_Context* node, int32_t len, uint32_t ip, uint16_t port,
                                        enum tt_Transport transport, uint16_t seq_span);

// Where the socket reads its next datagram: the context's rx_buffer, or - once a sample retained from it keeps it - a
// free buffer of the lending pool (tt_Context_set_rx_pool()). Read on the polling thread only, which is also the only
// thread that changes it (lend_retain_locked()).
#if tt_SAMPLE_LENDING
static uint8_t* lend_buffer(struct tt_Context* node, uint32_t number) {
    return number == 0 ? node->rx_buffer : node->lend.pool + ((size_t)(number - 1U) * tt_RX_POOL_BUFFER_BYTES);
}
#define RX_LANDING(node) lend_buffer((node), (node)->lend.landing)
static tt_ret_t process_datagram_at(struct tt_Context* node, uint8_t* buffer, int32_t len, uint32_t ip, uint16_t port,
                                    enum tt_Transport transport, uint16_t seq_span, uint8_t kind, uint32_t index);
#else
#define RX_LANDING(node) ((node)->rx_buffer)
#endif

// One datagram counted as received, by transport and by socket - process_datagram_locked()'s counting, and the segment
// drain's for a record it passes over unread, so the totals agree whichever way a record was dealt with.
static void count_arrivals(struct tt_Context* node, enum tt_Transport transport, uint32_t datagrams) {
    node->rx_datagrams += datagrams;
    // The receive half of the seam (SHM_PLAN.md stage 0). One place, because there is one place a
    // datagram enters core - and stage 1's segment arrivals will be counted here too rather than
    // beside it, so the two transports are never counted by two different rules.
    node->rx_datagrams_by_transport[transport] += datagrams;
    if (node->rx_via_data_port) {
        node->rx_via_data_datagrams += datagrams;
    } else {
        node->rx_via_well_known_datagrams += datagrams;
    }
}

static void count_arrival(struct tt_Context* node, enum tt_Transport transport) {
    count_arrivals(node, transport, 1);
}

// Whether anything is outstanding in this context's own ring, published or claimed - drain_own_segment()'s own
// lock-free first look; both indices are free-running and equal means nothing is. Always false without a segment.
//
// drain_rx() asks it of a long pass only (ring_takes_its_turn()). The socket is read to exhaustion, and a socket
// refilled as fast as it is read is never exhausted - then nothing in the ring was read for as long as the stream
// lasted. On the rig a max-rate publisher's own broadcasts kept its socket full for a whole run, and the
// subscriber's announce, waiting in its segment, was never read (test_transport_seam.c,
// test_a_socket_that_never_empties_does_not_starve_the_ring). The poll that follows drains the ring first.
static bool segment_has_records(const struct tt_Context* node) {
#if tt_SEGMENT_ENABLED
    const struct tt_SegmentHeader* header = node->own_segment;
    return header != NULL && __atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE) !=
                                 __atomic_load_n(&header->read_index, __ATOMIC_RELAXED);
#else
    UNUSED(node);
    return false;
#endif
}

// segment_has_records() as drain_rx() asks it, counting each pass it ends (rx_drain_ring_turns, the A/B witness).
// Asked only where drain_rx() refreshes its clock, every tt_RX_CLOCK_REFRESH datagrams of one drain: the point the
// drain already treats as "this pass is long", so a drain that empties the socket sooner - every drain of a doorbell,
// an ACKNACK or a heartbeat - reads nothing of the ring's header. Asked every pass (56565720), the two loads of a line
// the writers keep writing cost best_effort_throughput p4 0.5% of its receive rate and its writer 0.5% CPU per sample
// on the rig (ab_samehost_ringturn3_20261007-235731). A record now waits at most tt_RX_CLOCK_REFRESH datagrams (and the
// rest of a chunk) behind a socket that never empties - the bound the drain's timestamps already accept.
static bool ring_takes_its_turn(struct tt_Context* node) {
    if (!segment_has_records(node)) {
        return false;
    }
    node->rx_drain_ring_turns++;
    return true;
}

// rx_buffer itself needs no lock - only the one poller touches it (struct tt_Context.poller_active) - but
// everything a datagram updates does, so each one is processed under the state lock.
#if tt_SEGMENT_ENABLED
// Everything waiting in this context's own segment, handed to the same acceptance path a socket
// arrival takes - the whole point of the seam being that a datagram is a datagram once it is
// inside. The transport is passed rather than inferred, so an arrival cannot be recorded without
// saying where it came from.
//
// Bounded per call so a writer that keeps the ring full cannot starve the socket: the poll returns
// and comes back, which is the same fairness the socket drain already has.
// The head of the ring when the drain found nothing to read. A claimed slot keeps the sequence it
// had while free, so "empty" and "claimed by a writer that never published" look identical from the
// slot; what separates them is write_index, which moves on the claim. Ahead of read_index with
// nothing readable means something was taken from the ring and not put back.
//
// A writer mid-memcpy looks exactly like a writer that died, and must, so this counts rather than
// concludes: the warning waits for tt_SEGMENT_STALL_PASSES consecutive passes, which no live writer
// survives. The reason it is worth detecting at all is that a wedged head is permanent - the reader
// cannot read past it, the ring then fills, and every peer falls back to UDP with only
// segment_full_dropped to show for it, pointing at the ring's size instead of at the dead writer.
// Whether this context is about to sleep on its socket, published in its own segment header so its
// peers can see it. Fenced both ways: a writer publishes a record and then reads this, the owner
// writes this and then drains, and it is that pairing - each side's store ordered before its own
// later load by a full fence, not either store alone - that makes it impossible for a record to sit
// in the ring with nobody coming for it (segment_ring_if_asleep() says what went wrong without one).
static void segment_reader_waiting(struct tt_Context* node, bool waiting) {
    if (node->own_segment == NULL) {
        return;
    }
    uint32_t value = 0;
    if (waiting) {
        // A new generation for every sleep, and never 0, which means awake. Writers ring each one once - except right
        // after a sleep that was called off (segment_sleep_called_off()), whose generation is announced again.
        if (node->segment_generation_unspent != 0 &&
            node->segment_unspent_doorbells == node->segment_doorbells_received) {
            node->segment_generations_kept++;
        } else {
            node->segment_sleep_generation++;
            if (node->segment_sleep_generation == 0) {
                node->segment_sleep_generation = 1;
            }
        }
        node->segment_generation_unspent = 0; // kept once at most: the wait that follows may take its ring
        value = node->segment_sleep_generation;
    }
    __atomic_store_n(&node->own_segment->reader_waiting, value, __ATOMIC_SEQ_CST);
    if (waiting) {
        // The other half of segment_ring_if_asleep()'s fence: the drain that follows reads the ring with acquire
        // loads, and C11 orders a seq_cst store before a later acquire load of another word no more than it orders a
        // release store before one. x86's locked store and Arm64's ldar happen to; an RCpc acquire (ldapr) need not.
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
}

// A sleep announced and then called off, because the drain after the announcement found a record (poll_wait_io()):
// its generation is announced again by the next sleep rather than a new one (2026-10-08).
//
// Why: the record that calls a sleep off was published while the announcement stood, and its writer reads
// reader_waiting right after publishing, so it has usually rung that generation already - for a reader that never
// waited. With the edge-triggered bell (hal_linux.c) that ring stays pending until the next wait, which returns on it
// at once; under a new generation its writer rings again as soon as it publishes, and that ring lands after the
// reader has already woken - pending for the sleep after, and so on. One called-off sleep started a chain of waits
// that each returned at once and each cost a ring. The old level-triggered bell was read on every wake, which
// swallowed most rings landing during it and ended a chain within a wait or two. On the PC with the two vCPUs ~260
// ns apart, max-rate best_effort_throughput p3 announced 2.6 times as often as with the old wait (763 k against
// 290 k a run), 23% of the announcements were called off, and the writer rang 0.073 times a sample against 0.025.
// With the generation kept, round 4 (8c1e6431) went from 1.463 to 1.367 us of CPU a sample there, against the old
// wait's 1.343, and rang 0.028 times a sample against its 0.033.
//
// Announced again, the generation is one its writers have rung or will ring: a writer that rang it does not ring it
// again, and its ring is still pending and ends the next wait - which is then the sleep it was for; one that has not
// rings when it sees it, as for any sleep. What would make the ring gone is something taking it in between, so the
// generation is kept only when no doorbell was received since it was called off (a UDP doorbell is a datagram, and
// a socket read outside a wait can take it) and only once: the wait that follows may take its ring, and the sleep
// after that is a new one. A new segment never inherits one (create_own_segment()).
static void segment_sleep_called_off(struct tt_Context* node) {
    segment_reader_waiting(node, false);
    node->segment_generation_unspent = 1;
    node->segment_unspent_doorbells = node->segment_doorbells_received;
}

// Whether anything is outstanding in this context's own ring - published and unread, or claimed and still being
// filled: the two indices drain_own_segment()'s fast path compares.
static bool segment_outstanding(const struct tt_Context* node) {
    const struct tt_SegmentHeader* header = node->own_segment;
    return header != NULL && __atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE) !=
                                 __atomic_load_n(&header->read_index, __ATOMIC_RELAXED);
}

// Whether the head of this context's own ring is claimed by a writer that has not published it yet.
static bool segment_head_in_flight(const struct tt_Context* node) {
    const struct tt_SegmentHeader* header = node->own_segment;
    if (header == NULL) {
        return false;
    }
    uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_RELAXED); // ours to move
    if (__atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE) == read_index) {
        return false; // nothing claimed: the ring is empty
    }
    const struct tt_SegmentSlot* head = (const struct tt_SegmentSlot*)segment_slot(header, read_index);
    return __atomic_load_n(&head->sequence, __ATOMIC_ACQUIRE) != read_index + 1U;
}

// The processor's own hint that this loop is a spin-wait (x86 PAUSE, Arm YIELD): the hardware paces the loop and may
// give the pipeline to an SMT sibling. Not a delay of this code's choosing.
static inline void spin_wait_hint(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

// When a wait for a writer should next look at the ring. A wait that reads the ring's shared words on every turn
// takes their cache lines from the writer that is about to write them, and the writer stalls for each one: on the
// PC with the two vCPUs ~260 ns apart, a reader that polled the head slot and write_index continuously while
// following its writer record by record doubled the writer's time per sample. So the wait looks once per writer
// gap (segment_gap_ns, measured while awake; segment_await_next()) and spins on the clock alone in between, which
// costs the writer nothing. Without a gap measured yet it looks on every turn. Never past `give_up`.
static uint64_t segment_next_look(const struct tt_Context* node, uint64_t now, uint64_t give_up) {
    uint64_t look = now + node->segment_gap_ns;
    if (look > give_up) {
        look = give_up;
    }
    uint64_t clock = tt_get_ns();
    while (clock < look) {
        spin_wait_hint();
        clock = tt_get_ns();
    }
    return clock;
}

// Waits, without announcing a sleep, for the record at the head of the ring that a writer has claimed and not yet
// published (2026-10-07). True when it was published - the caller goes round and drains it - and false when the wait
// gave up: the caller then sleeps on it exactly as before, and the writer rings when it publishes.
//
// Why: a writer claims a slot, fills it, publishes it and only then reads reader_waiting (segment_ring_if_asleep()).
// A reader that catches up with its writer while that writer is filling the head slot, and announces a sleep, has
// bought a doorbell for a record it is about to see anyway, and is woken one wake-up later with that record and little
// more. A reader faster than its writer catches up every few records, so at a writer's full rate the writer paid a
// doorbell every few records, and the sooner a sleeping reader is back the more often it catches up: the rig's
// same-host max-rate p3 pair rang 0.145 times a sample, and 0.39 once the sleep itself got cheaper (epoll, the
// edge-triggered bell) - +59% system time per sample, the writer's. On the PC the same pair rang 0.12-0.19 times a
// sample, and 0.01 with this wait (examples/perf_hil/experiments/be_wake_cost.sh). The record being filled is the one
// thing a reader can be sure of: it is published within one copy, sooner than any doorbell could report it.
//
// How long: no longer than what a sleep costs this reader (segment_sleep_cost_ns), and never past the
// poll's own deadline - the spin-then-block rule: wait at most what blocking would have cost, then block. It also
// bounds the wait on a writer that was preempted, or died, between claim and publish; the reader then sleeps as it
// always did. Only the head slot's sequence is read meanwhile, the word the publication writes; another thread's
// scheduler entry ends the wait too.
//
// What it costs, with segment_await_next() beside it: a reader that waits instead of sleeping follows its writer
// record by record instead of sleeping through a batch, and both keep pulling the ring's lines from each other. Where
// moving a line between the two cores is cheap that is a gain - the PC at ~100 ns a round trip, 8% less user + system
// time per sample than the old sleep, 25% less with a slower writer - and where it is dear it is not: at ~260 ns (the
// PC's vCPUs on distant host cores) about 50% more, because the old sleep's slow return batched the ring. Looking at
// the ring once per writer gap rather than continuously (segment_next_look()) halved that from 100%.
static bool segment_await_claim(struct tt_Context* node, uint64_t now, uint64_t until) {
    uint64_t budget = node->segment_sleep_cost_ns;
    if (budget == 0) {
        return false; // no sleep measured yet: nothing to weigh the wait against
    }
    if (until - now < budget) {
        budget = until - now;
    }
    struct tt_SegmentHeader* header = node->own_segment;
    uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_RELAXED);
    const struct tt_SegmentSlot* head = (const struct tt_SegmentSlot*)segment_slot(header, read_index);
    node->segment_claim_waits++;
    const uint64_t give_up = now + budget;
    for (uint64_t looked = now;; looked = segment_next_look(node, looked, give_up)) {
        if (__atomic_load_n(&head->sequence, __ATOMIC_ACQUIRE) == read_index + 1U) {
            node->segment_claim_waits_published++;
            return true;
        }
        if (__atomic_load_n(&node->sched_inbox_pending, __ATOMIC_SEQ_CST) != 0) {
            return true; // another thread scheduled an entry: go round and run it
        }
        if (looked >= give_up) {
            return false;
        }
    }
}

static uint32_t drain_own_segment(struct tt_Context* node, bool* emptied);

// Waits, without announcing a sleep, for the next record to be claimed in an empty ring - only when the writers'
// records have been arriving faster than a sleep costs (2026-10-07, round 3).
//
// Why: with a record being filled waited out (segment_await_claim()), the rig's same-host max-rate p3 pair still rang
// 0.18 times a sample, every ring ending a real sleep: the reader, faster than its writer, also catches up between
// two records, finds the ring empty with nothing claimed, sleeps, and is rung by the next record about one writer gap
// later. A sleep there cost the reader ~2.4 us of system time and the writer a doorbell, ~1.4 us, to wait out a gap
// of ~0.9 us.
//
// The criterion, both sides measured and neither tuned: the expected time to the next record - the records claimed
// since the current awake stretch began (the last resume, or the last time the ring was found empty) over its length,
// so time spent asleep is never in it and a burst after a long sleep does not pass for a fast writer - against the
// what a sleep costs (segment_sleep_cost_ns). Only when the next record is due sooner than a sleep would cost is
// the ring watched, and then for at most that cost - the spin-then-block rule again - and never past the poll's
// deadline. No record claimed while awake means nothing to expect, and the reader sleeps at once: a ping-pong's reader
// wakes to the record it was rung for, answers, and goes back to sleep exactly as before. Only write_index is read
// meanwhile, the word a claim moves.
static bool segment_await_next(struct tt_Context* node, uint64_t now, uint64_t until) {
    struct tt_SegmentHeader* header = node->own_segment;
    const uint32_t claimed = __atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE);
    const uint32_t arrivals = claimed - node->segment_stretch_index;
    const uint64_t awake_ns = now - node->segment_stretch_ns;
    node->segment_stretch_ns = now; // the ring is empty: a new stretch starts here
    node->segment_stretch_index = claimed;
    uint64_t budget = node->segment_sleep_cost_ns;
    if (arrivals == 0) {
        return false; // nothing claimed while awake: nothing to expect, sleep
    }
    node->segment_gap_ns = awake_ns / arrivals;
    if (node->segment_watching == 0 || budget == 0 || node->segment_gap_ns >= budget) {
        return false; // waiting does not pay here (segment_epoch_turn()), or the next record is not expected sooner
                      // than a sleep costs: sleep
    }
    if (until - now < budget) {
        budget = until - now;
    }
    node->segment_watches++;
    const uint64_t give_up = now + budget;
    for (uint64_t looked = now;; looked = segment_next_look(node, looked, give_up)) {
        if (__atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE) != claimed) {
            node->segment_watch_hits++;
            return true;
        }
        if (__atomic_load_n(&node->sched_inbox_pending, __ATOMIC_SEQ_CST) != 0) {
            return true; // another thread scheduled an entry: go round and run it
        }
        if (looked >= give_up) {
            return false;
        }
    }
}

// Whether the two waits above pay for themselves - measured, not assumed (2026-10-07, round 4).
//
// Why: each wait is right by the reader's own account - it waits at most what a sleep has cost it - and still made the
// pair dearer where moving a cache line between the two cores is slow. On the PC with the two vCPUs ~260 ns apart,
// a6ef471d's same-host max-rate p3 pair cost 1.99 us of CPU a sample against main's 1.39: the waits keep the reader
// at its writer's heels, so every record moves the head slot's line and write_index from the writer while it still
// needs them. The writer slowed from 0.75 to 1.00 us a record, and the reader, waiting instead of sleeping, spent all
// of that too. Where a line moves in ~100 ns the same waits cost about what they save, and on the rig's Pi 5 they
// saved 17.5%: the writer, no longer ringing, got faster (0.81 -> 0.66 us a record). The reader cannot see what its
// waits cost the writer except in the writer's pace, nor what sleeping costs itself except in its own CPU time, and
// no rule built from one side's figures separates those three cases.
//
// So both are measured, in each mode, over epochs of one ring's worth of records (header->slots): wall time per
// record, which is the writers' pace - for a writer never idle its CPU time per record, and for a paced one the same
// in both modes, so it cancels - plus this thread's CPU time per record (tt_thread_cpu_ns(), read once an epoch).
// The first epoch after a change of mode is not measured: it starts from what the other mode left (a backlog, a
// writer still slowed). Each mode keeps the mean and variance of its recent epochs (segment_cost_record()), and the
// preference moves only when the other mode is cheaper by more than twice the standard error of the difference - one
// epoch is ~0.4 ms, and on the PC a single slow wake-up moves one by a quarter, so comparing single epochs flipped the
// choice every few epochs and kept the dearer mode on for a third of them (measured at ~260 ns). The other mode is
// measured again after segment_probe_every epochs: 1 at first, doubling after each re-measure that does not change
// the choice, up to tt_SEGMENT_PROBE_EVERY_MAX, and back to 1 when the choice changes. Doubling whether or not the
// re-measure was conclusive matters: re-measures are short and start from the other mode's state, so they read the
// two modes closer together than they are (on the PC, 1.42 against 1.66 us a record at ~260 ns, where whole runs
// differ by 1.35 against 2.0), and an interval that shrank while the two could not be told apart kept the dearer mode
// on for a tenth of the epochs. A context starts sleeping, as it did before the waits existed,
// and measures the waits second.
//
// Called at a decision on an empty-looking ring, so an epoch ends within one decision of its last record.

// One epoch's cost per record into its mode's running mean and variance. Costs are clamped to 2^24 ns a record (60
// records a second), far slower than any stream where the waits can be taken, so the squares fit.
static void segment_cost_record(struct tt_Context* node, uint8_t mode, uint64_t cost_ns) {
    const uint64_t cost = cost_ns < (1ULL << 24) ? cost_ns : (1ULL << 24);
    const uint32_t count = ++node->segment_cost_n[mode];
    const int64_t mean = (int64_t)node->segment_cost_mean_ns[mode];
    const int64_t delta = (int64_t)cost - mean;
    const int64_t next = mean + (delta / (int64_t)count);
    node->segment_cost_mean_ns[mode] = (uint64_t)next;
    const int64_t product = delta * ((int64_t)cost - next);
    node->segment_cost_m2[mode] += product > 0 ? (uint64_t)product : 0U;
}

// -1 when `mode` costs less than the other by more than twice the standard error of the difference, +1 when
// it costs more by that much, 0 when the two cannot be told apart yet (or either has fewer than two epochs).
static int segment_cost_compare(const struct tt_Context* node, uint8_t mode) {
    const uint8_t other = (uint8_t)(mode ^ 1U);
    const uint64_t n_a = node->segment_cost_n[mode];
    const uint64_t n_b = node->segment_cost_n[other];
    if (n_a < 2 || n_b < 2) {
        return 0;
    }
    const int64_t diff = (int64_t)node->segment_cost_mean_ns[mode] - (int64_t)node->segment_cost_mean_ns[other];
    // The variance of each mean: m2 / (n - 1) / n.
    const uint64_t se2 =
        (node->segment_cost_m2[mode] / (n_a - 1U) / n_a) + (node->segment_cost_m2[other] / (n_b - 1U) / n_b);
    const uint64_t magnitude = (uint64_t)(diff < 0 ? -diff : diff);
    if (magnitude * magnitude <= 4U * se2) {
        return 0;
    }
    return diff < 0 ? -1 : 1;
}

// Which mode the next epoch runs in, given the one just measured.
static uint8_t segment_next_mode(struct tt_Context* node, uint8_t mode) {
    const uint8_t other = (uint8_t)(mode ^ 1U);
    if (node->segment_cost_n[mode] < 2) {
        return mode; // not enough of this mode yet to tell its spread
    }
    if (node->segment_cost_n[other] < 2) {
        return other; // nor of the other: measure it next
    }
    const uint8_t preferred = node->segment_preferred;
    const int verdict = segment_cost_compare(node, (uint8_t)(preferred ^ 1U));
    if (verdict < 0) {
        node->segment_preferred = (uint8_t)(preferred ^ 1U); // the other mode is cheaper, beyond doubt
        node->segment_probe_every = 1;
        node->segment_probe_in = 1;
        return node->segment_preferred;
    }
    if (mode != preferred) {
        if (node->segment_probe_every < tt_SEGMENT_PROBE_EVERY_MAX) {
            node->segment_probe_every *= 2; // a re-measure that did not change the choice: the next one later
        }
        node->segment_probe_in = node->segment_probe_every;
        return preferred;
    }
    if (node->segment_probe_in > 0) {
        node->segment_probe_in--;
        return preferred;
    }
    return other; // time to measure the other mode again
}

static void segment_epoch_turn(struct tt_Context* node, uint64_t now, uint32_t read_index) {
    const uint64_t cpu = tt_thread_cpu_ns();
    const uintptr_t thread = __atomic_load_n(&node->poller_thread, __ATOMIC_RELAXED);
    const uint32_t records = read_index - node->segment_epoch_index;
    const uint8_t mode = node->segment_watching;
    const bool settled = node->segment_epoch_settling == 0;
    node->segment_epoch_settling = 0;
    bool measured = false;
    // Another polling thread's CPU clock, or a clock that went backwards (the wall clock stepped), measures nothing.
    if (settled && node->segment_epoch_ns != 0 && records > 0 && thread == node->segment_epoch_thread &&
        now > node->segment_epoch_ns && cpu >= node->segment_epoch_cpu_ns) {
        const uint64_t spent = (now - node->segment_epoch_ns) + (cpu - node->segment_epoch_cpu_ns);
        segment_cost_record(node, mode, spent / records);
        node->segment_epochs[mode]++;
        measured = true;
        // The window: every tt_SEGMENT_PROBE_EVERY_MAX epochs, of either mode, the weight of both modes' past is
        // halved, never below two epochs (one alone has no spread to test against). So the mode not chosen, measured
        // about once in that many, keeps two or three epochs' weight and its next re-measure counts for a third of its
        // record or more. Halved by the count of its own epochs instead, it kept a whole run's re-measures, and after
        // the costs changed took over 30,000 epochs to follow them. Its wider standard error is the price: once the
        // re-measures are rare, the choice moves only on a clear difference - which a change of placement is.
        if ((node->segment_epochs[0] + node->segment_epochs[1]) % tt_SEGMENT_PROBE_EVERY_MAX == 0) {
            for (uint8_t each = 0; each < 2; each++) {
                if (node->segment_cost_n[each] >= 4) {
                    node->segment_cost_n[each] /= 2U;
                    node->segment_cost_m2[each] /= 2U;
                }
            }
        }
    }
    node->segment_epoch_ns = now;
    node->segment_epoch_cpu_ns = cpu;
    node->segment_epoch_thread = thread;
    node->segment_epoch_index = read_index;
    if (!measured) {
        return; // the next epoch runs in the same mode, and is measured
    }
    const uint8_t next = segment_next_mode(node, mode);
    if (next != mode) {
        node->segment_watching = next;
        node->segment_epoch_settling = 1;
    }
}

// poll_wait_io()'s first question at an empty-looking ring: is there something to drain instead of sleeping? A record
// in the ring is drained, not slept on, one a writer is still filling is waited for - for a while - rather than
// announced to (segment_await_claim()), and an empty ring is watched when the next record is due sooner than a sleep
// would cost (segment_await_next()); the two waits only in an epoch where they pay (segment_epoch_turn()). Either way
// no writer is told to ring for it. True when it drained, watched a record claimed or was interrupted; *took says
// whether it took a record.
static bool segment_drained_instead_of_sleeping(struct tt_Context* node, bool has_next, uint64_t next, uint64_t time,
                                                int64_t timeout, bool until_next_event, bool* took) {
    *took = false;
    node->segment_sleep_on_claim = 0;
    const struct tt_SegmentHeader* header = node->own_segment;
    if (header == NULL) {
        return false;
    }
    const uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_RELAXED); // ours to move
    if (node->segment_epoch_ns == 0 || read_index - node->segment_epoch_index >= header->slots) {
        segment_epoch_turn(node, time, read_index);
    }
    uint64_t deadline = has_next ? next : UINT64_MAX;
    if (!until_next_event && time + (uint64_t)timeout < deadline) {
        deadline = time + (uint64_t)timeout;
    }
    if (!segment_outstanding(node)) {
        // Empty: true only when a record was claimed while watching - the caller goes round and finds it outstanding.
        return deadline > time && segment_await_next(node, time, deadline);
    }
    if (segment_head_in_flight(node) &&
        (node->segment_watching == 0 || deadline <= time || !segment_await_claim(node, time, deadline))) {
        node->segment_sleep_on_claim = 1; // the one sleep whose length is all cost: segment_resumed() measures it
        return false;                     // the claim outlived what a sleep costs: sleep on it, and its writer rings
    }
    bool emptied = true;
    *took = drain_own_segment(node, &emptied) > 0;
    return true;
}

// After a sleep: a new awake stretch (segment_await_next()), and what choosing to sleep cost - but only from a sleep
// taken on a record already claimed, ended by its doorbell alone (tt_receive()'s zero-length datagram). Its writer was
// mid-copy when the reader decided, and rings as soon as it publishes, so decision to resume is the copy's remainder,
// the doorbell and the wake-up, with no idle time in it. Any other sleep lasts as long as nobody writes: the rig's
// round 2 kept the shortest of all doorbell-ended sleeps, and a run whose first such sleep came while the peer was
// still starting up and that never slept again after it was left with a 0.6 ms bound (seen on the PC). Until there is
// one, nothing is waited for.
//
// The mean of the recent clean ones is kept (segment_sleep_cost_ns), not the shortest (2026-10-07, round 4). The
// shortest is the one that never blocked - its doorbell rung between the announcement and the wait, so the wait
// returned at once - and every clean sleep is another chance to draw one. On the PC with an -O0 writer the shortest
// fell to 0.5-0.6 us in one run of five of a6ef471d (rebased) and in every run that also measured the sleeping mode
// (segment_epoch_turn()), against 8.5-17.7 us in the others: below the writer's ~1 us gap, so the empty ring was
// never watched, the claim waits ran out, and the writer rang 0.7 M times a run instead of 2.5 k (1.80 us a sample of
// CPU against 1.61). The window is the last tt_SEGMENT_PROBE_EVERY_MAX sleeps or so, as for the epochs' costs.
static void segment_resumed(struct tt_Context* node, int32_t len, uint64_t decided) {
    struct tt_SegmentHeader* header = node->own_segment;
    const bool clean = node->segment_sleep_on_claim != 0;
    node->segment_sleep_on_claim = 0;
    if (header == NULL) {
        return;
    }
    // A wait that timed out or was interrupted received nothing, and no clock was read after it: its stretch is
    // counted from the decision, which only lengthens it - the pace it gives is, if anything, slower.
    const uint64_t resumed = len >= 0 ? node->rx_clock_ns : decided;
    node->segment_stretch_ns = resumed;
    node->segment_stretch_index = __atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE);
    if (len != 0 || !clean) {
        return;
    }
    const uint64_t cost = resumed - decided;
    if (node->segment_sleep_cost_n >= tt_SEGMENT_PROBE_EVERY_MAX) {
        node->segment_sleep_cost_n /= 2U;
    }
    const uint32_t count = ++node->segment_sleep_cost_n;
    const int64_t mean = (int64_t)node->segment_sleep_cost_ns;
    node->segment_sleep_cost_ns = (uint64_t)(mean + (((int64_t)cost - mean) / (int64_t)count));
}

static void note_head_stall(struct tt_Context* node) {
    struct tt_SegmentHeader* header = node->own_segment;
    uint32_t write_index = __atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE);
    uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_RELAXED); // the owner's own
    if (write_index == read_index) {
        node->segment_head_stall_passes = 0; // nothing outstanding: the ring is genuinely empty
        return;
    }
    node->segment_head_stalls++;
    node->segment_head_stall_passes++;
    if (node->segment_head_stall_passes == tt_SEGMENT_STALL_PASSES && node->segment_stall_warnings == 0) {
        node->segment_stall_warnings++;
        TT_LOG_WARNING("Segment ring head stalled: slot %u was claimed by a peer that never published it, and %u "
                       "drain passes have found it so. Nothing can be read past it and the ring will fill; traffic "
                       "falls back to UDP from here, which is correct but will look like a sizing problem.",
                       (unsigned)(read_index & (header->slots - 1U)), (unsigned)node->segment_head_stall_passes);
    }
}

// segment_release() of `count` slots from `index` on, with read_index moved once, past the last: the slots the drain
// passes over without reading. A writer reads read_index only to see whether the ring is full, so one that looks
// between the stores sees it fuller than it is, never emptier; each slot is free for it only once its own sequence
// says so. read_index stays a release store for that: a writer that saw it move but not yet a slot's sequence would
// refuse the slot and drop the datagram (segment_full_dropped).
//
// One release fence and plain stores for the sequences, not a release store each. The guarantee is the same -
// whatever this reader did with a slot (the plan's look at its headers) happens before a writer that sees its new
// sequence reuses it - and on x86 so is the code: both forms are plain moves there. On AArch64 a release store is an
// STLR, an STLR is not seen before any store ahead of it, and the drain's next step - segment_read()'s acquire load of
// the record it hands over, an LDAR - waits until every earlier STLR is seen. Each sequence is on a line the publisher
// wrote, so the handed-over record waited for count + 1 ownership transfers, one after another: a cost that grows
// with the backlog passed over, ~10 records per record read on the rig (rmw Array1k BEST_EFFORT KEEP_LAST 1, -r 0)
// against ~4 on the PC. That is the likely rig-only part of skip-to-newest's extra sample age: on the rig 1fec27bb
// read the delivered sample 0.63 us older than main; on the PC, at the rig's depth, -0.36 and +0.05 us (two runs).
// Plain stores after the fence are taken in parallel, and the one STLR left waits for them together.
// examples/perf_hil/experiments/skip_age_pc.sh EMU=1 runs the arms on the PC under a model of the AArch64 ordering
// (skip_age_armorder.py): at depth 11-13 1fec27bb read the sample 1.10 us older than main and this 0.06 us (n=5).
// The rig decides.
static void segment_release_run(struct tt_SegmentHeader* header, uint32_t index, uint32_t count) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (uint32_t k = 0; k < count; k++) {
        struct tt_SegmentSlot* slot_header = (struct tt_SegmentSlot*)segment_slot(header, index + k);
        __atomic_store_n(&slot_header->sequence, index + k + header->slots, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&header->read_index, index + count, __ATOMIC_RELEASE); // after every sequence above
}

// Releases slot `index` back to the writers without reading it - segment_read()'s release, in its order: the slot
// is marked free one lap ahead before read_index moves past it.
static void segment_release(struct tt_SegmentHeader* header, uint32_t index) {
    segment_release_run(header, index, 1);
}

// SKIP TO NEWEST. Under overload a KEEP_LAST Subscriber's backlog in the ring is mostly samples its history will
// overwrite before anyone takes them: at BEST_EFFORT KEEP_LAST 1 the drain decoded and delivered 29.7M samples into
// rmw_tickle's queue and the application took 1.23M, so nearly all of the subscriber's CPU went into samples nobody
// saw. Reading only the first record per poll (origin/ab/drain-one-sample) cut the waste but read the ring in
// order, so the application saw samples ~1.4 ms old instead of the newest - which defeats KEEP_LAST.
//
// So the drain looks at the backlog first, header by header, and passes over a sample when its writer already has
// `keep_last_depth` newer complete samples queued behind it. What it never passes over:
//   - anything that is not one DATA, FRAG_FIRST or FRAG_CONT of user data (control, announces, batches);
//   - a sample of an endpoint with a KEEP_ALL Subscriber, or with any Subscriber whose depth is 0;
//   - a fragmented sample not wholly in the backlog - a partial one is never counted as newer, never skipped;
//   - anything from a writer whose records are not in strictly rising seq_no order in the backlog (a retransmit
//     among them): "newer" is read off the seq_no, so where it does not rise the drain does not judge.
// A RELIABLE Subscriber's skipped sample is recorded as received - its watermark moves past it exactly as if it
// had been delivered and then superseded - so the writer is acknowledged it and nothing asks for it again; and only
// a sample that is next in order for every RELIABLE Subscriber of it is skipped, so no gap is ever made out of it.
enum segment_record_kind { SEGMENT_RECORD_OTHER, SEGMENT_RECORD_WHOLE, SEGMENT_RECORD_FIRST, SEGMENT_RECORD_CONT };

struct segment_record {
    enum segment_record_kind kind;
    uint8_t source;
    uint8_t frag_index;
    uint8_t frag_count;
    uint16_t span;
    uint32_t endpoint_id; // WHOLE and FIRST only: a continuation does not name its endpoint
    uint32_t entity_id;
    uint32_t seq_no; // the record's own
    uint32_t sender_ip;
    uint16_t sender_port;
};

// Enough of a record to read its headers: the classic header with its submessage header, then the largest of the
// sample headers.
#define SEGMENT_RECORD_PEEK \
    (sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_FragFirstHeader))

// A record's framing, read from the copy of its first bytes: its header (in the classic form either way), the one
// submessage's type, and where that submessage's body starts and ends. False when it is not one submessage this
// context would process - a batch, another receiver's, a foreign magic or version, or our own.
struct segment_framing {
    struct tt_Header header;
    uint8_t type;
    uint32_t head;     // where the body starts
    uint32_t body_end; // where it ends
};

static bool segment_peek_framing(const struct tt_Context* node, const uint8_t* peek, uint32_t peeked, uint32_t length,
                                 struct segment_framing* out) {
    if (peek[0] == tt_SINGLE_MARKER_LE || peek[0] == tt_SINGLE_MARKER_BE) {
        const struct tt_SingleHeader* single = (const struct tt_SingleHeader*)peek;
        out->header.magic_value = single->marker == native_single_marker() ? NATIVE_MAGIC_VALUE : REVERSE_MAGIC_VALUE;
        out->header.version = single->version;
        out->header.source = single->source;
        out->type = single->type;
        out->head = sizeof(struct tt_SingleHeader);
        out->body_end = length;
    } else {
        if (peeked < sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader)) {
            return false;
        }
        memcpy(&out->header, peek, sizeof(out->header));
        if (!tt_is_native_endian(&out->header) && !tt_is_reverse_endian(&out->header)) {
            return false;
        }
        const struct tt_SubmessageHeader* submessage =
            (const struct tt_SubmessageHeader*)(peek + sizeof(struct tt_Header));
        uint32_t sub_length = rd16(&out->header, submessage->length);
        // Exactly one submessage, addressed to everyone or to us: a batch is read the ordinary way.
        if (sub_length < sizeof(struct tt_SubmessageHeader) || sizeof(struct tt_Header) + sub_length > length ||
            length - (sizeof(struct tt_Header) + sub_length) >= sizeof(struct tt_SubmessageHeader) ||
            (submessage->receiver != tt_SUBMESSAGE_ID_ALL && submessage->receiver != node->id)) {
            return false;
        }
        out->type = submessage->type;
        out->head = sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader);
        out->body_end = sizeof(struct tt_Header) + sub_length;
    }
    return out->header.version == tt_VERSION && out->header.source != node->id && out->body_end > out->head;
}

// A whole DATA's header: user data only, never an announce.
static void segment_peek_data(struct tt_Header* header, const uint8_t* body, struct segment_record* rec) {
    const struct tt_DataHeader* data = (const struct tt_DataHeader*)body;
    rec->endpoint_id = rd32(header, data->endpoint_id);
    rec->entity_id = rd32(header, data->entity_id);
    rec->seq_no = rd32(header, data->seq_no);
    if (rec->endpoint_id != tt_DISCOVERY_ENDPOINT_ID && rec->entity_id != tt_DISCOVERY_ENTITY_ID) {
        rec->kind = SEGMENT_RECORD_WHOLE;
    }
}

#if tt_FRAG_ENABLED
static bool frag_shape_valid(uint32_t index, uint32_t count) {
    return count >= 2 && count <= tt_FRAG_MAX_COUNT && index < count;
}

static void segment_peek_first(struct tt_Header* header, const uint8_t* body, struct segment_record* rec) {
    const struct tt_FragFirstHeader* first = (const struct tt_FragFirstHeader*)body;
    rec->endpoint_id = rd32(header, first->data.endpoint_id);
    rec->entity_id = rd32(header, first->data.entity_id);
    rec->seq_no = rd32(header, first->data.seq_no);
    rec->frag_count = first->frag_count;
    rec->span = 1; // each fragment is its own seq_no
    if (rec->entity_id != tt_DISCOVERY_ENTITY_ID && frag_shape_valid(0, rec->frag_count)) {
        rec->kind = SEGMENT_RECORD_FIRST;
    }
}

static void segment_peek_cont(struct tt_Header* header, const uint8_t* body, struct segment_record* rec) {
    const struct tt_FragContHeader* cont = (const struct tt_FragContHeader*)body;
    rec->entity_id = rd32(header, cont->entity_id);
    rec->seq_no = rd32(header, cont->seq_no);
    rec->frag_index = cont->frag_index;
    rec->frag_count = cont->frag_count;
    rec->span = 1;
    if (rec->entity_id != tt_DISCOVERY_ENTITY_ID && rec->frag_index != 0 &&
        frag_shape_valid(rec->frag_index, rec->frag_count)) {
        rec->kind = SEGMENT_RECORD_CONT;
    }
}
#endif

// What record `index` of the ring is, from its headers alone: no payload is copied or decoded. Anything this does
// not recognise as one sample record is OTHER, which the drain reads the ordinary way.
static void segment_peek_record(const struct tt_Context* node, struct tt_SegmentHeader* ring, uint32_t index,
                                struct segment_record* rec) {
    memset(rec, 0, sizeof(*rec));
    rec->kind = SEGMENT_RECORD_OTHER;
    const struct tt_SegmentSlot* slot_header = (const struct tt_SegmentSlot*)segment_slot(ring, index);
    uint32_t length = slot_header->length;
    if (length > ring->slot_bytes || length < sizeof(struct tt_SingleHeader)) {
        return;
    }
    // A copy of the headers, so every check below reads the bytes the decision is made on.
    tt_ALIGNAS(8) uint8_t peek[SEGMENT_RECORD_PEEK];
    uint32_t peeked = length < (uint32_t)sizeof(peek) ? length : (uint32_t)sizeof(peek);
    memcpy(peek, (const uint8_t*)slot_header + sizeof(*slot_header), peeked);
    struct segment_framing framing;
    if (!segment_peek_framing(node, peek, peeked, length, &framing)) {
        return;
    }
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range != tt_DISCOVERY_RANGE_SUBNET && !sender_in_range(slot_header->sender_ip)) {
        return;
    }
#endif
    rec->source = framing.header.source;
    rec->sender_ip = slot_header->sender_ip;
    rec->sender_port = slot_header->sender_port;
    uint16_t span = slot_header->seq_span;
    rec->span = (span >= 1 && span <= tt_FRAG_MAX_COUNT) ? span : 1;
    // A sample header must lie wholly in the copy and leave a payload after it.
    uint32_t body = framing.body_end - framing.head;
    uint32_t in_copy = peeked > framing.head ? peeked - framing.head : 0;
    const uint8_t* sample_header = peek + framing.head;
    if (framing.type == tt_SUBMESSAGE_TYPE_DATA && body >= sizeof(struct tt_DataHeader) &&
        in_copy >= sizeof(struct tt_DataHeader)) {
        segment_peek_data(&framing.header, sample_header, rec);
    }
#if tt_FRAG_ENABLED
    if (framing.type == tt_SUBMESSAGE_TYPE_FRAG_FIRST && body > sizeof(struct tt_FragFirstHeader) &&
        in_copy >= sizeof(struct tt_FragFirstHeader)) {
        segment_peek_first(&framing.header, sample_header, rec);
    }
    if (framing.type == tt_SUBMESSAGE_TYPE_FRAG_CONT && body > sizeof(struct tt_FragContHeader) &&
        in_copy >= sizeof(struct tt_FragContHeader)) {
        segment_peek_cont(&framing.header, sample_header, rec);
    }
#endif
}

// The depth the drain may rely on for an endpoint: the deepest of its Subscribers' KEEP_LAST depths, or 0 - never
// skip - when it has none, or any one of them keeps everything.
struct keep_last_depth_ctx {
    uint32_t depth;
    bool any;
    bool keeps_all;
};

static void visit_keep_last_depth(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    UNUSED(node);
    struct keep_last_depth_ctx* ctx = (struct keep_last_depth_ctx*)ctx_ptr;
    const struct tt_Subscriber* sub = (const struct tt_Subscriber*)endpoint;
    ctx->any = true;
    if (sub->keep_last_depth == 0) {
        ctx->keeps_all = true;
    } else if (sub->keep_last_depth > ctx->depth) {
        ctx->depth = sub->keep_last_depth;
    }
}

static uint32_t keep_last_depth_of(struct tt_Context* node, uint32_t endpoint_id) {
    struct keep_last_depth_ctx ctx = {0, false, false};
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id, visit_keep_last_depth, &ctx);
    return (ctx.any && !ctx.keeps_all) ? ctx.depth : 0;
}

// One writer's records in the backlog, as the plan walks it from the newest back.
#define SEGMENT_PLAN_WRITERS 8U
struct segment_plan_writer {
    uint32_t entity_id;
    uint32_t endpoint_id;
    uint32_t depth;
    uint32_t newer_complete; // complete samples of this writer later in the backlog than the walk has reached
    uint32_t lowest_seq_no;  // the lowest record seq_no later in the backlog
    uint32_t run_seq_no;     // the seq_no the next fragment of the sample being collected must carry
    uint8_t source;
    uint8_t run_count;      // 0: no sample being collected
    uint8_t run_next_index; // the fragment index expected next, walking back
    bool depth_known;
    bool disordered; // a seq_no that did not fall walking back: nothing earlier of this writer is judged
};

static bool seq_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static struct segment_plan_writer* plan_writer(struct segment_plan_writer* writers, uint32_t* writer_count,
                                               const struct segment_record* rec) {
    for (uint32_t idx = 0; idx < *writer_count; idx++) {
        if (writers[idx].source == rec->source && writers[idx].entity_id == rec->entity_id) {
            return &writers[idx];
        }
    }
    if (*writer_count == SEGMENT_PLAN_WRITERS) {
        return NULL; // a writer the table cannot follow is read the ordinary way, all of it
    }
    struct segment_plan_writer* writer = &writers[(*writer_count)++];
    memset(writer, 0, sizeof(*writer));
    writer->source = rec->source;
    writer->entity_id = rec->entity_id;
    writer->lowest_seq_no = rec->seq_no + 1U;
    return writer;
}

// Whether `rec`, met walking back, completes a sample of `writer`: a whole record always does; a FRAG_FIRST does
// when every continuation of its sample was met just before (later in the ring). A continuation never does - it
// extends the run being collected, or ends it.
static bool plan_completes_sample(struct segment_plan_writer* writer, const struct segment_record* rec) {
    if (rec->kind == SEGMENT_RECORD_CONT) {
        if (rec->frag_index + 1U == rec->frag_count) {
            writer->run_count = rec->frag_count; // a sample's last fragment: collect it from here back
        } else if (writer->run_count != rec->frag_count || rec->frag_index != writer->run_next_index ||
                   rec->seq_no != writer->run_seq_no) {
            writer->run_count = 0;
            return false;
        }
        writer->run_next_index = (uint8_t)(rec->frag_index - 1U);
        writer->run_seq_no = rec->seq_no - 1U;
        return false;
    }
    bool complete =
        rec->kind == SEGMENT_RECORD_WHOLE ||
        (writer->run_count == rec->frag_count && writer->run_next_index == 0 && writer->run_seq_no == rec->seq_no);
    writer->run_count = 0;
    return complete;
}

// How many records from `read_index` on are published now, up to `limit`: a claimed slot not yet written ends it.
//
// Plain loads and one acquire fence after them, not an acquire load each: the same guarantee for every header the
// plan then reads. On AArch64 no load may start before an earlier LDAR completes, so the walk took one cross-core miss
// after another, one per record of the backlog (segment_release_run() has the measurements); plain loads miss
// together. x86 compiles both forms to plain moves.
static uint32_t segment_published_run(struct tt_SegmentHeader* ring, uint32_t read_index, uint32_t limit) {
    uint32_t count = 0;
    while (count < limit) {
        const struct tt_SegmentSlot* slot_header = (const struct tt_SegmentSlot*)segment_slot(ring, read_index + count);
        if (__atomic_load_n(&slot_header->sequence, __ATOMIC_RELAXED) != read_index + count + 1U) {
            break;
        }
        count++;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return count;
}

// Whether any Subscriber of this context could have a sample skipped. None is the common case - every native
// Subscriber, KEEP_ALL, and a context with no Subscriber at all - and then the plan reads no record. Without this check
// the plan peeked at every record's headers for nothing, and the first PC run read the native best_effort_throughput
// p3 reader slower than main's (1379 vs 1633 k/s, n=2 and n=1: a signal, not a figure).
static bool any_keep_last_subscriber(const struct tt_Context* node) {
    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        const struct tt_Endpoint* endpoint = node->endpoints[i];
        if (endpoint != NULL && endpoint->kind == tt_KIND_TOPIC_SUBSCRIBER &&
            ((const struct tt_Subscriber*)endpoint)->keep_last_depth != 0) {
            return true;
        }
    }
    return false;
}

// One record of the plan's walk, met walking back: whether it is superseded. `rec` is what its headers say.
static bool plan_segment_record(struct tt_Context* node, uint32_t index, struct segment_plan_writer* writers,
                                uint32_t* writer_count, struct segment_record* rec) {
    segment_peek_record(node, node->own_segment, index, rec);
    if (rec->kind == SEGMENT_RECORD_OTHER) {
        return false;
    }
    struct segment_plan_writer* writer = plan_writer(writers, writer_count, rec);
    if (writer == NULL) {
        return false;
    }
    if (!seq_before(rec->seq_no, writer->lowest_seq_no)) {
        writer->disordered = true;
    }
    writer->lowest_seq_no = rec->seq_no;
    if (!plan_completes_sample(writer, rec)) {
        return false; // part of a sample is never newer than anything, and never skipped
    }
    if (!writer->depth_known || writer->endpoint_id != rec->endpoint_id) {
        writer->endpoint_id = rec->endpoint_id;
        writer->depth = keep_last_depth_of(node, rec->endpoint_id);
        writer->depth_known = true;
    }
    bool skip = !writer->disordered && writer->depth != 0 && writer->newer_complete >= writer->depth;
    if (writer->newer_complete < UINT32_MAX) {
        writer->newer_complete++;
    }
    return skip;
}

// Whether `rec` belongs to the run being collected: the same writer's samples of the same endpoint, which one
// permission check and one accounting can stand for.
static bool plan_run_extends(const struct tt_SegmentPlanRun* run, const struct segment_record* rec) {
    return run->count != 0 && run->source == rec->source && run->entity_id == rec->entity_id &&
           run->endpoint_id == rec->endpoint_id;
}

// Decides, for the records from `read_index` on that are published now, which ones are superseded, and sets their
// bits in segment_plan_skip. Walks from the newest back so that "how many complete samples of this writer are
// newer" is a running count. Under the state lock: it reads the endpoint table.
static void plan_segment_skips(struct tt_Context* node, uint32_t read_index) {
    struct tt_SegmentHeader* ring = node->own_segment;
    node->segment_plan_base = read_index;
    node->segment_plan_count = 0;
    node->segment_plan_run.count = 0;
    uint32_t limit = ring->slots < (uint32_t)tt_SEGMENT_SLOTS ? ring->slots : (uint32_t)tt_SEGMENT_SLOTS;
    uint32_t outstanding = __atomic_load_n(&ring->write_index, __ATOMIC_ACQUIRE) - read_index;
    if (outstanding < 2) {
        return; // one record cannot be superseded by anything
    }
    if (!any_keep_last_subscriber(node)) {
        // A plan with nothing to skip: as many records as are outstanding, read the ordinary way, no header read.
        node->segment_plan_count = outstanding < limit ? outstanding : limit;
        memset(node->segment_plan_skip, 0, (node->segment_plan_count + BITS_IN_1BYTE - 1U) / BITS_IN_1BYTE);
        return;
    }
    uint32_t count = segment_published_run(ring, read_index, outstanding < limit ? outstanding : limit);
    node->segment_plan_count = count;
    memset(node->segment_plan_skip, 0, (count + BITS_IN_1BYTE - 1U) / BITS_IN_1BYTE);

    struct segment_plan_writer writers[SEGMENT_PLAN_WRITERS];
    uint32_t writer_count = 0;
    struct tt_SegmentPlanRun* run = &node->segment_plan_run;
    for (uint32_t i = count; i-- > 0;) {
        struct segment_record rec;
        bool skip = plan_segment_record(node, read_index + i, writers, &writer_count, &rec);
        if (skip) {
            node->segment_plan_skip[i / BITS_IN_1BYTE] |= (uint8_t)(1U << (i % BITS_IN_1BYTE));
        }
        // The run of superseded whole samples of one writer that ends at record i; once the walk reaches record 0
        // it is the plan's leading run. Anything else met on the way ends it.
        if (skip && rec.kind == SEGMENT_RECORD_WHOLE) {
            if (!plan_run_extends(run, &rec)) {
                run->source = rec.source;
                run->entity_id = rec.entity_id;
                run->endpoint_id = rec.endpoint_id;
                run->count = 0;
            }
            run->count++;
        } else {
            run->count = 0;
        }
    }
}

// Whether a superseded sample can be passed over now without disturbing any Subscriber of it. The plan judged it by
// depth; this asks what only the moment of reading knows: every RELIABLE Subscriber must have it as the very next
// seq_no and hold nothing out of order, so skipping it moves a watermark and makes no gap.
struct segment_skip_ctx {
    const struct segment_record* rec;
    bool allowed;
    bool any;
    bool reliable; // a RELIABLE Subscriber of it, whose watermark each skipped seq_no must move one by one
};

static void visit_skip_allowed(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    struct segment_skip_ctx* ctx = (struct segment_skip_ctx*)ctx_ptr;
    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
    if (subscriber_refuses_writer(node, sub, ctx->rec->source, ctx->rec->endpoint_id, ctx->rec->entity_id)) {
        return; // it drops this writer's samples whatever happens here
    }
    ctx->any = true;
    if (sub->keep_last_depth == 0) {
        ctx->allowed = false;
        return;
    }
    if (sub->reliable && !TT_ORDERING_DISABLED) {
        ctx->reliable = true;
        struct tt_WriterProxy* proxy = find_writer_proxy(sub, ctx->rec->source, ctx->rec->entity_id);
        if (proxy == NULL || proxy->ack_seq_no != ctx->rec->seq_no || sub->reorder_held != 0) {
            ctx->allowed = false;
        }
#if tt_FRAG_ENABLED
        if (node->frag_fast_sub == sub) {
            ctx->allowed = false; // part of a sample is being put together for it
        }
#endif
    }
}

// Records skipped samples with every Subscriber of them: a RELIABLE one counts the record's seq_no(s) received,
// exactly as an arrival would, and releases whatever that puts in order. A BEST_EFFORT one has nothing to record -
// the newer sample it is about to get moves its watermark. `samples` is how many whole samples of the writer this
// is (0 for a continuation of one already counted); the RELIABLE half runs only for one record at a time.
struct segment_superseded_ctx {
    const struct segment_record* rec;
    uint32_t samples;
};

static void visit_superseded(struct tt_Context* node, struct tt_Endpoint* endpoint, void* ctx_ptr) {
    struct segment_superseded_ctx* ctx = (struct segment_superseded_ctx*)ctx_ptr;
    const struct segment_record* rec = ctx->rec;
    struct tt_Subscriber* sub = (struct tt_Subscriber*)endpoint;
    if (subscriber_refuses_writer(node, sub, rec->source, rec->endpoint_id, rec->entity_id)) {
        return;
    }
    if (ctx->samples != 0) {
        sub->superseded += ctx->samples;
        struct tt_WriterProxy* writer = find_or_create_writer_proxy(sub, rec->source, rec->entity_id, NULL);
        if (writer != NULL) {
            writer->superseded_pending += ctx->samples;
            sub->superseded_pending += ctx->samples;
        }
    }
    if (!sub->reliable || TT_ORDERING_DISABLED) {
        return;
    }
    node->rx_seq_span = rec->span;
    node->rx_targeted = false;
    (void)update_reliable_ack(node, sub, rec->seq_no, rec->source, rec->entity_id, rec->sender_ip, rec->sender_port);
    struct tt_WriterProxy* proxy = find_writer_proxy(sub, rec->source, rec->entity_id);
    if (proxy != NULL) {
        drain_reorder(node, sub, proxy);
    }
}

static void skip_superseded_record(struct tt_Context* node, const struct segment_record* rec, bool count_sample,
                                   uint32_t index) {
    struct segment_superseded_ctx ctx = {rec, count_sample ? 1U : 0U};
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, rec->endpoint_id, visit_superseded, &ctx);
    node->rx_seq_span = 1;
    if (count_sample) {
        node->rx_shm_skipped_superseded++;
    }
    count_arrival(node, tt_TRANSPORT_SHM); // received, like any record; only not read
    segment_release(node->own_segment, index);
}

// The plan's leading run, passed over whole: `segment_plan_run.count` superseded samples of one writer, each one
// record, at the head of the ring. Done record by record, every one of them cost a fresh look at its headers, two
// walks of the endpoint's Subscribers and two releases before the drain reached the sample it hands over - and that
// sample waited for all of it, so it was that much older when it was taken. On the PC (rmw Array1k BEST_EFFORT
// KEEP_LAST 1, -r 0, examples/perf_hil/experiments/skip_age_pc.sh) 5.1 records were passed over at 92 ns each before
// every delivered sample, which waited 1.07 us inside the drain before its decode began, and the age perf_test reads
// rose above main's (3.36 against 2.97 us); the rig's backlog is deeper (11.6 records per delivered sample). Judged,
// counted and released together they cost 17 ns each, the wait is 0.53 us and the age 2.80 us. Only where no RELIABLE
// Subscriber is in it: a RELIABLE watermark moves one seq_no at a time, and the record-by-record path does that.
// Returns how many records went; 0 leaves the head to the record-by-record path.
static uint32_t segment_skip_run(struct tt_Context* node, uint32_t index) {
    struct tt_SegmentPlanRun* run = &node->segment_plan_run;
    uint32_t count = run->count;
    run->count = 0; // spent, whatever is decided: the record-by-record path judges what is left
    struct segment_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = SEGMENT_RECORD_WHOLE;
    rec.source = run->source;
    rec.entity_id = run->entity_id;
    rec.endpoint_id = run->endpoint_id;
    rec.span = 1;
    struct segment_skip_ctx allowed = {&rec, true, false, false};
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, rec.endpoint_id, visit_skip_allowed, &allowed);
    if (!allowed.allowed || !allowed.any || allowed.reliable) {
        return 0;
    }
    struct segment_superseded_ctx ctx = {&rec, count};
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, rec.endpoint_id, visit_superseded, &ctx);
    node->rx_shm_skipped_superseded += count;
    count_arrivals(node, tt_TRANSPORT_SHM, count); // received, like any record; only not read
    segment_release_run(node->own_segment, index, count);
    return count;
}

static bool segment_skipping_any(const struct tt_Context* node) {
    for (uint32_t k = 0; k < tt_SEGMENT_SKIPPING_SAMPLES; k++) {
        if (node->segment_skipping[k].count != 0) {
            return true;
        }
    }
    return false;
}

// A continuation of a sample whose FRAG_FIRST was skipped goes with it. True when `rec` was one (and is gone).
static bool skip_continuation(struct tt_Context* node, struct segment_record* rec, uint32_t index) {
    for (uint32_t k = 0; k < tt_SEGMENT_SKIPPING_SAMPLES; k++) {
        struct tt_SegmentSkippedSample* sample = &node->segment_skipping[k];
        if (sample->count == 0 || sample->source != rec->source || sample->entity_id != rec->entity_id) {
            continue;
        }
        if (sample->next_seq_no != rec->seq_no || sample->next_index != rec->frag_index ||
            sample->count != rec->frag_count) {
            sample->count = 0; // not the sample it was following; cannot happen to a sample planned whole
            return false;
        }
        rec->endpoint_id = sample->endpoint_id;
        sample->next_seq_no++;
        sample->next_index++;
        if (sample->next_index == sample->count) {
            sample->count = 0;
        }
        skip_superseded_record(node, rec, false, index);
        return true;
    }
    return false;
}

// Starts following a skipped FRAG_FIRST's continuations. False when there is no room to, and then the sample is
// read whole rather than skipped in part.
static bool follow_skipped_sample(struct tt_Context* node, const struct segment_record* rec) {
    for (uint32_t k = 0; k < tt_SEGMENT_SKIPPING_SAMPLES; k++) {
        struct tt_SegmentSkippedSample* sample = &node->segment_skipping[k];
        if (sample->count == 0) {
            sample->source = rec->source;
            sample->entity_id = rec->entity_id;
            sample->endpoint_id = rec->endpoint_id;
            sample->next_seq_no = rec->seq_no + 1U;
            sample->next_index = 1;
            sample->count = rec->frag_count;
            return true;
        }
    }
    return false;
}

enum segment_head { SEGMENT_HEAD_READ, SEGMENT_HEAD_SKIPPED, SEGMENT_HEAD_HAND_BACK };

// The record at the head of the ring: SKIPPED when it was superseded and is gone from the ring, READ when it is to be
// read the ordinary way, HAND_BACK when the drain should return first. Under the state lock.
//
// HAND_BACK is the other half of skip-to-newest. A plan covers the records published when it was made, and hands a
// KEEP_LAST reader the newest of them; a writer faster than the reader has published more by the time that plan is
// spent, and planning again would hand the same reader a still newer sample in the same poll - overwriting, in its
// history, the one just delivered before anyone took it. Measured on the PC (rmw Array1k BEST_EFFORT KEEP_LAST 1,
// without this): 7.6M samples delivered into the rmw queue for 1.7M taken. So once a plan's records have handed a
// KEEP_LAST Subscriber a sample, the drain returns and the next poll plans afresh - newest again, and taken. A drain
// that handed nothing to a KEEP_LAST Subscriber (KEEP_ALL, control, every native bench reader) is not stopped.
//
// `passed` is how many records a SKIPPED head took out of the ring: one, or the plan's whole leading run.
static enum segment_head segment_skip_head(struct tt_Context* node, uint32_t* passed) {
    struct tt_SegmentHeader* ring = node->own_segment;
    uint32_t index = __atomic_load_n(&ring->read_index, __ATOMIC_RELAXED);
    if (index - node->segment_plan_base >= node->segment_plan_count) {
        if (node->rx_keep_last_delivered != node->segment_plan_mark) {
            return SEGMENT_HEAD_HAND_BACK;
        }
        plan_segment_skips(node, index);
    }
    uint32_t offset = index - node->segment_plan_base;
    bool planned = offset < node->segment_plan_count &&
                   (node->segment_plan_skip[offset / BITS_IN_1BYTE] & (uint8_t)(1U << (offset % BITS_IN_1BYTE))) != 0;
    bool skipping = segment_skipping_any(node);
    if (!planned && !skipping) {
        return SEGMENT_HEAD_READ;
    }
    *passed = 1;
    if (offset == 0 && node->segment_plan_run.count != 0 && !skipping) {
        uint32_t count = segment_skip_run(node, index);
        if (count != 0) {
            *passed = count;
            return SEGMENT_HEAD_SKIPPED;
        }
    }
    const struct tt_SegmentSlot* slot_header = (const struct tt_SegmentSlot*)segment_slot(ring, index);
    if (__atomic_load_n(&slot_header->sequence, __ATOMIC_ACQUIRE) != index + 1U) {
        return SEGMENT_HEAD_READ; // not published: segment_read() says so and counts the stall
    }
    struct segment_record rec;
    segment_peek_record(node, ring, index, &rec);
    if (rec.kind == SEGMENT_RECORD_CONT && skipping && skip_continuation(node, &rec, index)) {
        return SEGMENT_HEAD_SKIPPED;
    }
    if (!planned || (rec.kind != SEGMENT_RECORD_WHOLE && rec.kind != SEGMENT_RECORD_FIRST)) {
        return SEGMENT_HEAD_READ;
    }
    struct segment_skip_ctx allowed = {&rec, true, false, false};
    for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, rec.endpoint_id, visit_skip_allowed, &allowed);
    if (!allowed.allowed || !allowed.any) {
        return SEGMENT_HEAD_READ;
    }
    if (rec.kind == SEGMENT_RECORD_FIRST && !follow_skipped_sample(node, &rec)) {
        return SEGMENT_HEAD_READ;
    }
    skip_superseded_record(node, &rec, true, index);
    return SEGMENT_HEAD_SKIPPED;
}

// Returns how many records it delivered, and through `emptied` whether the ring is now empty. The
// caller must not read the socket while it is not: the socket is drained to exhaustion (drain_rx)
// while this used to stop after 64 records, so the ring ran permanently behind - and a single
// datagram of the same stream arriving by socket, a broadcast sample say, advances the subscriber's
// watermark past everything still queued in the ring, all of which is then discarded on arrival.
// A few hundred thousand socket arrivals invalidated thirty-six million ring records that way on
// CI's same-host cell. Ordering between two queues is not a property of either queue; it is a
// property of the order they are read in.
static uint32_t drain_own_segment(struct tt_Context* node, bool* emptied) {
    *emptied = true;
    if (node->own_segment == NULL) {
        return 0;
    }
    // Nothing published and nothing claimed means no records, and that is worth knowing WITHOUT the
    // state lock. Both indices are free-running counters and equal means nothing is outstanding; a
    // record arriving immediately after is taken on the next poll, exactly as one arriving a moment
    // later always was.
    //
    // This is the whole of a cross-host context's experience of the segment - its ring is always
    // empty, because no peer can ever attach to it - so without this fast path every such
    // deployment pays a lock acquire and release per poll to discover nothing, and pays it forever.
    // Plan raised it against the measured 1.6-2.9% the module costs cross-host, which is exactly the
    // budget a per-poll lock lands in. segment_head_stall_passes is the polling thread's own and is
    // touched nowhere else, so resetting it here needs no lock either.
    struct tt_SegmentHeader* header = node->own_segment;
    if (__atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE) ==
        __atomic_load_n(&header->read_index, __ATOMIC_RELAXED)) {
        node->segment_head_stall_passes = 0;
        return 0;
    }

    uint32_t delivered = 0;
    uint32_t drained = 0;
    bool hand_back = false;
    node->segment_plan_mark = node->rx_keep_last_delivered; // read by segment_skip_head() between plans
    while (drained < tt_SEGMENT_DRAIN_PER_POLL) {
        // The state lock, in chunks, exactly as drain_rx() takes it for the socket. It is required:
        // process_datagram_locked() says so in its name and every field a datagram touches is under
        // it. This drain called it **unlocked from the day the segment landed** (1c69658a) and
        // nothing caught it, because every test that drives the drain is single-threaded - the core
        // integration suite runs one thread per process, so the race has no second party. rmw_tickle
        // has an executor, and there it corrupted state until the publisher segfaulted: "Check all"
        // went red on 1c69658a and stayed red, and this is why.
        //
        // Chunked rather than one lock for the whole drain so a publishing thread waits at most
        // tt_RX_LOCK_CHUNK records, which is the bound drain_rx() already chose for the same reason.
        state_lock(node);
        uint32_t taken = 0;
        bool ran_dry = false;
        while (taken < tt_RX_LOCK_CHUNK && drained + taken < tt_SEGMENT_DRAIN_PER_POLL) {
            uint32_t len = 0;
            uint32_t sender_ip = 0;
            uint16_t sender_port = 0;
            uint16_t seq_span = 1;
            uint32_t passed = 0;
            enum segment_head head = segment_skip_head(node, &passed);
            if (head == SEGMENT_HEAD_HAND_BACK) {
                hand_back = true;
                break;
            }
            if (head == SEGMENT_HEAD_SKIPPED) {
                node->segment_head_stall_passes = 0;
                taken += passed;
                continue;
            }
#if tt_SAMPLE_LENDING
            // Read in place (DESIGN.md section 10, receive-buffer lending): the record is processed in its slot, which
            // is released after - or kept, if a sample in it was retained.
            struct tt_SegmentHeader* ring = node->own_segment;
            uint8_t* record = NULL;
            uint32_t index = 0;
            if (!segment_take(ring, &record, &index, &len, &sender_ip, &sender_port, &seq_span)) {
#else
            if (!segment_read(node->own_segment, node->rx_buffer, (uint32_t)sizeof(node->rx_buffer), &len, &sender_ip,
                              &sender_port, &seq_span)) {
#endif
                note_head_stall(node); // empty, or a head nobody is coming back for - the two look alike
                ran_dry = true;
                break;
            }
            node->segment_head_stall_passes = 0; // something was read, so the head is moving
            taken++;
            // The sender's own address, carried in the record. Not invented here and not inferred
            // from whose segment this is: several peers write into one segment, and discovery learns
            // where a peer lives from the address its announce arrived on.
            // The span travels with the record, not with the peer: the same writer's next record
            // may be a different size and consume a different number of seq_nos.
#if tt_SAMPLE_LENDING
            (void)process_datagram_at(node, record, (int32_t)len, sender_ip, sender_port, tt_TRANSPORT_SHM, seq_span,
                                      tt_LEND_SLOT, index);
            segment_done(node, ring, index);
            if (node->lend.segment_release_deferred && lend_finish_release(node)) {
                ran_dry = true; // the record was the last same-host peer's farewell: there is no ring any more
                break;
            }
#else
            (void)process_datagram_locked(node, (int32_t)len, sender_ip, sender_port, tt_TRANSPORT_SHM, seq_span);
#endif
        }
        state_unlock(node);
        delivered += taken;
        drained += taken;
        if (ran_dry || hand_back) {
            node->segment_plan_count = 0; // a plan is made and spent inside one drain
            // Ran dry: emptied, even past a wedged head (nothing can be read past it). Handed back: whether the ring
            // still holds records, so the caller does not read the socket ahead of them.
            *emptied = ran_dry || !segment_has_records(node);
            return delivered;
        }
    }
    // The bound was reached with records still there. Bounded rather than unbounded so one busy peer
    // cannot hold the caller inside this function; `emptied` is how the caller learns not to read the
    // socket yet.
    node->segment_plan_count = 0;
    *emptied = false;
    return delivered;
}
#endif

static tt_ret_t process_datagram(struct tt_Context* node, int32_t len, uint32_t ip, uint16_t port,
                                 enum tt_Transport transport) {
    state_lock(node);
    // Every caller of this wrapper is a socket path, where a sample's fragments are separate
    // datagrams that really do each carry their own seq_no. Span 1 is not a default here, it is
    // the right answer; the segment drain is the one path that reads a span off the record.
    tt_ret_t result = process_datagram_locked(node, len, ip, port, transport, 1);
    state_unlock(node);
    return result;
}

// The datagram of `len` bytes at `buffer` - the receive buffer the socket read it into, or (lending) the ring slot it
// lies in - decoded and dispatched.
static tt_ret_t process_datagram_in(struct tt_Context* node, uint8_t* buffer, int32_t len, uint32_t ip, uint16_t port,
                                    enum tt_Transport transport, uint16_t seq_span);

#if tt_SAMPLE_LENDING
// process_datagram_in() with the datagram's memory recorded for tt_Sample_retain(): `kind` and `index` say where it
// lives (tt_LEND_BUFFER and the buffer, tt_LEND_SLOT and the ring index), and only while it is being processed.
static tt_ret_t process_datagram_at(struct tt_Context* node, uint8_t* buffer, int32_t len, uint32_t ip, uint16_t port,
                                    enum tt_Transport transport, uint16_t seq_span, uint8_t kind, uint32_t index) {
    node->lend.rx_base = buffer;
    node->lend.rx_length = len > 0 ? (uint32_t)len : 0U;
    node->lend.rx_index = index;
    node->lend.rx_kind = kind;
    tt_ret_t result = process_datagram_in(node, buffer, len, ip, port, transport, seq_span);
    node->lend.rx_kind = tt_LEND_FREE;
    return result;
}
#endif

static tt_ret_t process_datagram_locked(struct tt_Context* node, int32_t len, uint32_t ip, uint16_t port,
                                        enum tt_Transport transport, uint16_t seq_span) {
#if tt_SAMPLE_LENDING
    return process_datagram_at(node, RX_LANDING(node), len, ip, port, transport, seq_span, tt_LEND_BUFFER,
                               node->lend.landing);
#else
    return process_datagram_in(node, node->rx_buffer, len, ip, port, transport, seq_span);
#endif
}

static tt_ret_t process_datagram_in(struct tt_Context* node, uint8_t* buffer, int32_t len, uint32_t ip, uint16_t port,
                                    enum tt_Transport transport, uint16_t seq_span) {
    // Set here and nowhere else, so no arrival path can forget to and none inherits the last
    // record's span. Read, not consumed: one DATA can match several Subscribers and each needs it.
    node->rx_seq_span = (seq_span >= 1) ? seq_span : 1;
#if tt_SEGMENT_ENABLED
    // A doorbell: somebody put a record in this context's segment while it was asleep on this socket,
    // and rang. There is nothing to parse - waking up was the message - and it is dropped here,
    // before the magic check, because a zero-length datagram would otherwise be logged as a malformed
    // one every time the module did its job.
    if (len == 0) {
        node->segment_doorbells_received++;
        return tt_RET_OK;
    }
#endif
    node->rx_tail = (uint32_t)len;
    count_arrival(node, transport);

    TT_LOG_DEBUG("Process packet from addr: %d.%d.%d.%d:%d len: %d", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                 (ip >> BITS_IN_1BYTE) & MASK_8BIT, (ip >> 0) & MASK_8BIT, port, len);

    // (g11, RMW_GAPS_PLAN.md) A datagram nothing could be made of is dropped and counted, never returned as an
    // error. The loops TickLE ships - and the ones its examples teach people to write - end on anything but OK or
    // TIMEOUT, and drain_rx() stops on the same condition, so returning an error here let any host that can reach
    // the port end a node with one UDP datagram: one v10 packet from a leftover process ended a v11 server on the
    // rig, twenty seconds into its run, and voided the measurement. An error return is for this node's own
    // failures - an encode that overflows, a socket that breaks - not for what a peer chose to send.
    if (!process_packet(node, buffer, 0, len, ip, port, transport)) {
        TT_LOG_ERROR("Cannot process packet");
        node->rx_malformed_drops++;
        return tt_RET_OK;
    }

    return tt_RET_OK;
}

// One datagram more of a drain_rx() pass: every tt_RX_CLOCK_REFRESH of them the receive clock is read again, so a long
// drain keeps its stamps within microseconds (D1). True when it was - the drain's "this pass is long" point, where
// drain_rx() also looks at the ring (ring_takes_its_turn()).
static inline bool drain_clock_tick(struct tt_Context* node, uint32_t* since_clock) {
    if (++*since_clock <= tt_RX_CLOCK_REFRESH) {
        return false;
    }
    node->rx_clock_ns = tt_get_ns();
    *since_clock = 1;
    return true;
}

// D4: datagrams the HAL already holds are processed under one lock, up to tt_RX_LOCK_CHUNK of them - a publishing
// thread waits at most one chunk (OPTIMIZATION_PLAN.md 11.4). *long_pass is set when the clock was read again.
static tt_ret_t drain_rx_chunk(struct tt_Context* node, uint32_t* since_clock, bool* long_pass) {
    tt_ret_t result = tt_RET_OK;
    uint32_t ip = 0;
    uint16_t port = 0;
    state_lock(node);
    for (uint32_t taken = 0; taken < tt_RX_LOCK_CHUNK && result == tt_RET_OK && tt_rx_buffered(node) > 0; taken++) {
        int32_t len = tt_try_receive(node, RX_LANDING(node), tt_MAX_BUFFER_LENGTH, &ip, &port);
        if (len < 0) {
            break;
        }
        if (drain_clock_tick(node, since_clock)) {
            *long_pass = true;
        }
        result = process_datagram_locked(node, len, ip, port, tt_TRANSPORT_UDP, 1);
    }
    state_unlock(node);
    return result;
}

// After tt_receive() hands tt_Context_poll() the first datagram, pull whatever else the kernel
// already has buffered without another poll() per packet - a saturated receiver otherwise pays
// poll()+recvfrom() per packet instead of one poll() per drain. Best-effort: stops on the first
// "nothing waiting", a protocol error, or an I/O error (the outer poll picks that back up).
static tt_ret_t drain_rx(struct tt_Context* node, tt_ret_t first_result) {
    if (first_result != tt_RET_OK) {
        return first_result;
    }

    uint32_t since_clock = 1; // the first datagram was stamped with the reading its caller took
    while (true) {
        bool long_pass = false; // the clock was read again: tt_RX_CLOCK_REFRESH more datagrams in this drain
        tt_ret_t result = tt_RET_OK;
        if (tt_rx_buffered(node) == 0) {
            // The next receive may read the socket: outside the lock, one datagram, as before D4.
            uint32_t ip = 0;
            uint16_t port = 0;
            int32_t len = tt_try_receive(node, RX_LANDING(node), tt_MAX_BUFFER_LENGTH, &ip, &port);
            if (len < 0) {
                break; // -1 nothing waiting, -2 I/O error - either way, done draining
            }
            long_pass = drain_clock_tick(node, &since_clock);
            result = process_datagram(node, len, ip, port, tt_TRANSPORT_UDP);
        } else {
            result = drain_rx_chunk(node, &since_clock, &long_pass);
        }
        if (result != tt_RET_OK) {
            return result;
        }
        // A socket that never empties must not starve the ring: a long pass ends when a record waits there, and the
        // poll that follows drains the ring first (ring_takes_its_turn()).
        if (long_pass && ring_takes_its_turn(node)) {
            break;
        }
    }

    return tt_RET_OK;
}

// Whether a scheduler entry is due at `now`.
static bool scheduler_entry_due(struct tt_Context* node, uint64_t now) {
    uint64_t next = 0;
    return sched_next_time(node, &next) && next <= now;
}

static bool handle_receive_result(struct tt_Context* node, int32_t len, uint32_t ip, uint16_t port,
                                  bool woke_for_scheduler, tt_ret_t* result) {
    if (len == -1) { // Timeout
        if (woke_for_scheduler) {
            return false;
        }
        *result = tt_RET_TIMEOUT;
        return true;
    }

    if (len == -3) { // tt_Context_interrupt() - always ends the poll, even if woke_for_scheduler:
                     // an explicit interrupt request must never be swallowed the way a plain
                     // short wait for a due scheduler entry is, or the caller that asked to be
                     // woken (tt_Context_interrupt()'s own caller, on another thread) could end up
                     // waiting for however much longer the scheduler-driven work takes instead.
        *result = tt_RET_INTERRUPTED;
        return true;
    }

    if (len < 0) { // I/O error
        *result = tt_RET_IO_ERROR;
        return true;
    }

    *result = process_datagram(node, len, ip, port, tt_TRANSPORT_UDP);
    return true;
}

// One non-blocking pass: run everything due now, take whatever is already received, return. See
// tt_Context_poll()'s timeout == 0.
static tt_ret_t poll_once_nonblocking(struct tt_Context* node, uint64_t time) {
    bool has_next = false;
    uint64_t next = 0;
    while (run_due_entry(node, time, &has_next, &next)) {
    }

#if tt_SEGMENT_ENABLED
    // Before the socket, and to empty. Cheapest-first is the lesser reason; the real one is that a
    // socket datagram read while records are still queued in the ring arrives ahead of them and
    // makes the reader discard them.
    bool emptied = true;
    if (drain_own_segment(node, &emptied) > 0 || !emptied) {
        return tt_RET_OK; // data is data: hand it back rather than reaching past it for the socket
    }
#endif

    uint32_t ip = 0;
    uint16_t port = 0;
    int32_t len = tt_try_receive(node, RX_LANDING(node), tt_MAX_BUFFER_LENGTH, &ip, &port);
    if (len < 0) {
        return tt_RET_TIMEOUT;
    }
    return drain_rx(node, process_datagram(node, len, ip, port, tt_TRANSPORT_UDP));
}

// A negative-timeout poll returns once what fell due has run - every entry due by now, not just the
// first, so a burst of simultaneous timers costs one return, not several - and, when entries keep
// falling due (a max-rate publisher rescheduling itself), after at most tt_RECEIVE_TIMEOUT of that
// work: the old slice. That keeps the only property of the old cadence anyone could legitimately
// depend on - a bounded time to return under load - and drops the part nobody wanted, the same bound
// when there is nothing to do.
//
// Both checks are placed where they cost nothing extra per entry. "Nothing more is due" is decided by
// the peek the next iteration does anyway: it lands in the I/O branch, which returns instead of waiting
// once work has been done. The slice is checked against the clock reading the loop takes anyway. The
// first version peeked and read the clock again after every entry, and that alone cost -1.5% of max-rate
// throughput on the rig and ~24% of the loop's own entries per second in a microbenchmark.

// How long the I/O wait in tt_Context_poll() may last, and whether it ends for a scheduler entry. A
// negative-timeout poll waits exactly until the next entry, or - with none - passes 0, which
// tt_receive() takes as "no timeout" (hal.h): block until a datagram, a signal or tt_wake_signal(). A
// positive one waits the rest of its budget, shortened to the next entry if that comes first.
static int64_t poll_wait_length(bool has_next, uint64_t next, uint64_t time, int64_t timeout, bool until_next_event,
                                bool* woke_for_scheduler) {
    if (until_next_event) {
        *woke_for_scheduler = has_next;
        return has_next ? (int64_t)(next - time) : 0;
    }
    if (has_next && next - time < (uint64_t)timeout) {
        *woke_for_scheduler = true;
        return (int64_t)(next - time);
    }
    *woke_for_scheduler = false;
    return timeout;
}

// tt_Context_poll()'s I/O step, when nothing is due: wait for a datagram, the next entry or an interrupt.
// Returns true with *result set when the poll should end.
static bool poll_wait_io(struct tt_Context* node, bool has_next, uint64_t next, uint64_t time, int64_t timeout,
                         bool until_next_event, bool did_work, tt_ret_t* result) {
    // A negative-timeout poll that has already run what fell due returns here instead of starting
    // another wait.
    if (until_next_event && did_work) {
        *result = tt_RET_TIMEOUT;
        return true;
    }

#if tt_SEGMENT_ENABLED
    // `time`, this iteration's clock reading, also stands for when the choice to sleep began: the sleep's cost below.
    bool took = false;
    if (segment_drained_instead_of_sleeping(node, has_next, next, time, timeout, until_next_event, &took)) {
        if (took) {
            // Handed back, as the last drain below hands back its own (cba66e30): the loop has no drain, so going
            // round would decide the next wait with the records already delivered and the caller not told.
            *result = tt_RET_OK;
            return true;
        }
        return false; // nothing taken (a claim watched, or an entry scheduled): go round and take it, or run it
    }
#endif

    // Re-read the heap under the state lock, and publish what this wait will wait until before looking at
    // the inbox one last time. A thread scheduling without the lock pushes into the inbox and then reads
    // wait_until (wake_if_waiting_past()); this side writes wait_until and then reads the inbox. Both
    // sequentially consistent, so either the insert is seen here and the wait is skipped, or wait_until is
    // seen there and the wait is woken. A thread holding the state lock inserts into the heap directly and
    // cannot interleave with the read below at all. Either way nothing earlier than the wait is missed -
    // which under an indefinite wait would be forever.
    state_lock(node);
    sched_drain_inbox(node);
    const struct tt_TCB* head = peek_scheduler(node);
    has_next = head != NULL;
    next = has_next ? head->time : next;
    const bool retry_deadline = has_next && is_retry_timer(head->function); // G's samples (timer_lateness_fold())
    // An entry another thread scheduled since the loop's run_due_entry(), at a time no later than this
    // iteration's clock reading, is due: it saw no wait to wake, and the wait length below would be 0 or
    // negative - no timeout at all - or, under a budget, wrap past it. Go round the loop and run it.
    if (has_next && next <= time) {
        state_unlock(node);
        return false;
    }
    uint64_t until = has_next ? next : UINT64_MAX;
    if (!until_next_event && time + (uint64_t)timeout < until) {
        until = time + (uint64_t)timeout;
    }
    wait_until_store(node, until);
    state_unlock(node);
    if (__atomic_load_n(&node->sched_inbox_pending, __ATOMIC_SEQ_CST) != 0) {
        wait_until_store(node, 0);
        return false; // an entry arrived while this was deciding: loop, drain it, decide again
    }

    bool woke_for_scheduler = false;
    int64_t rest = poll_wait_length(has_next, next, time, timeout, until_next_event, &woke_for_scheduler);

    uint32_t ip = 0;
    uint16_t port = 0;
#if tt_SEGMENT_ENABLED
    // Published before the wait, and then one more drain: a record written before the flag became
    // visible to its writer has no doorbell coming for it, so the only way not to sleep on top of it
    // is to look once more after saying we are about to sleep.
    //
    // What this drain takes is handed back HERE, as the drain at the top of node_poll() hands back its own. It used
    // to "go round the loop", but the loop has no drain of its own: the next pass came straight back to this wait,
    // found the ring empty, and slept with the record already delivered to its Subscriber - until the budget or the
    // next scheduler entry. On the rig that was a 500 ms round trip about once a second in every rmw_tickle block
    // cell (the ping's own deadline; rmw_samehost 7e6fe171, 2026-10-07), and the poll cell that lost every reply
    // after one of them (test_transport_seam.c, test_a_record_found_by_the_last_drain_before_sleeping_ends_the_poll).
    segment_reader_waiting(node, true);
    bool emptied_before_wait = true;
    if (drain_own_segment(node, &emptied_before_wait) > 0 || !emptied_before_wait) {
        segment_sleep_called_off(node); // its generation is announced again by the next sleep
        wait_until_store(node, 0);
        *result = tt_RET_OK; // data is data: hand it back rather than waiting
        return true;
    }
    node->segment_sleeps++;
#endif
    int32_t len = tt_receive(node, RX_LANDING(node), tt_MAX_BUFFER_LENGTH, &ip, &port, rest);
#if tt_SEGMENT_ENABLED
    segment_reader_waiting(node, false);
#endif

    wait_until_store(node, 0); // not waiting: an insert now is seen by the loop
    uint64_t woke = 0;
    if (len >= 0) {
        node->rx_clock_ns = tt_get_ns(); // the wait may have been long: what arrived is stamped from here
    } else if (len == -1) {
        // Timed out. A wait for a retry timer that ran to its deadline is a sample of how late this host runs that
        // timer: G, the retry timers' granularity (timer_lateness_fold()). Earlier than the deadline is a wait cut
        // short; a wait that ended at a budget or another entry's deadline says nothing about a retry timer's; and
        // one that came back later than it was long never slept on a timer at all.
        woke = tt_get_ns();
        if (retry_deadline && woke_for_scheduler && woke >= until && woke <= until + (uint64_t)rest) {
            timer_lateness_fold(node, woke - until);
        }
    }
#if tt_SEGMENT_ENABLED
    segment_resumed(node, len, time);
#endif

    // A wait that ended with nothing received BEFORE the entry it was waiting for fell due was cut short
    // by a signal (the HALs report EINTR as a timeout). Hand control back rather than wait again: under
    // an indefinite wait that is what lets Ctrl-C reach the caller's loop.
    if (until_next_event && len == -1 && !scheduler_entry_due(node, woke)) {
        *result = tt_RET_TIMEOUT;
        return true;
    }

    if (handle_receive_result(node, len, ip, port, woke_for_scheduler, result)) {
        *result = drain_rx(node, *result);
        return true;
    }
    return false;
}

static tt_ret_t node_poll(struct tt_Context* node, int64_t timeout);

tt_ret_t tt_Context_poll(struct tt_Context* node, int64_t timeout) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    // One poller at a time ("Threading", tickle.h): a second would share rx_buffer with the first.
    uint8_t idle = 0;
    if (!__atomic_compare_exchange_n(&node->poller_active, &idle, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return tt_RET_BUSY;
    }
    __atomic_store_n(&node->poller_thread, tt_thread_self(), __ATOMIC_RELAXED); // NOLINT(misc-include-cleaner)
    tt_ret_t result = node_poll(node, timeout);
    node->rx_clock_ns = 0; // outside a poll a receive reads the clock itself
    __atomic_store_n(&node->poller_thread, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&node->poller_active, 0, __ATOMIC_RELEASE);
    return result;
}

// The non-blocking socket check a busy poll loop squeezes in between scheduler entries. Asks tt_rx_maybe_ready()
// first because its answer is usually free: a HAL that can tell from memory that nothing arrived saves the read that
// would have said so (hal.h; 2026-10-04, two empty reads per eight samples were a third of a max-rate segment
// publisher's system time). True with *result set when something was read and the poll should return.
static bool busy_peek(struct tt_Context* node, tt_ret_t* result) {
    if (!tt_rx_maybe_ready(node)) {
        return false;
    }
    uint32_t ip = 0;
    uint16_t port = 0;
    int32_t len = tt_try_receive(node, RX_LANDING(node), tt_MAX_BUFFER_LENGTH, &ip, &port);
    if (len < 0) {
        return false;
    }
    *result = drain_rx(node, process_datagram(node, len, ip, port, tt_TRANSPORT_UDP));
    return true;
}

static tt_ret_t node_poll(struct tt_Context* node, int64_t timeout) {
    // Negative: wait exactly until the next scheduler entry is due, or indefinitely when there is none
    // (see tt_Context_poll() in tickle.h). The loop below always bounded a wait by the next due entry, but
    // that could only SHORTEN a fixed 100us slice, so an idle node woke ~10,000 times a second for
    // nothing. Now the scheduler sets the wait, and returning once the due work has run keeps the
    // caller's loop exactly as responsive: it regains control after every event, and only then.
    const bool until_next_event = timeout < 0;

    uint64_t time = tt_get_ns();
    const uint64_t poll_start = time;
    node->rx_clock_ns = time;

#if tt_SEGMENT_ENABLED
    // A segment arrival is not something poll() can wait on, so the ring is drained here, at the top
    // of the call, and to empty - and if anything came out of it this call returns with it rather
    // than going on to sleep on top of data it already has. Reaching the socket first would put a
    // newer datagram in front of what is still queued here, which is the whole of the defect this
    // ordering exists to prevent.
    bool ring_emptied = true;
    if (drain_own_segment(node, &ring_emptied) > 0 || !ring_emptied) {
        return tt_RET_OK;
    }
#endif

    // timeout == 0: one non-blocking pass - run everything due now, drain whatever RX is already
    // waiting, return. No poll()/select() wait at all. For a caller that just wants to make
    // progress and get straight back to its own work (a tight publish loop with -i 0), where a
    // sub-millisecond "wait" would otherwise round up to a full 1ms poll() and throttle it -
    // and where relying on broadcast self-receive to keep that poll() returning early breaks the
    // moment the publisher switches to unicast.
    if (timeout == 0) {
        return poll_once_nonblocking(node, time);
    }

    // EXPERIMENTAL (branch experiment/poll-loop-io-interleave, rmw_tickle/PLAN.md's own "Further
    // latency research" section) - counts scheduler entries run back-to-back without a receive
    // check, so a continuously-rescheduling task (a max-rate Publisher's own send loop) can't
    // starve tt_receive() for this whole call's own timeout budget. See tt_SCHEDULER_IO_INTERLEAVE's
    // own doc comment (config.h) for the full reasoning.
    uint32_t consecutive_scheduler_runs = 0;
    bool did_work = false; // a scheduler entry has run during this call

    while (until_next_event || timeout > 0) {
        bool has_next = false;
        uint64_t next = 0;
        bool ran = false;
        if (consecutive_scheduler_runs < tt_SCHEDULER_IO_INTERLEAVE) {
            ran = run_due_entry(node, time, &has_next, &next); // runs one if due, else says when
        } else {
            has_next = sched_next_time(node, &next);
        }

        if (ran) {
            // Run scheduler first
            consecutive_scheduler_runs++;
            did_work = true;
        } else if (has_next && next <= time) {
            // A scheduler entry is still due, but tt_SCHEDULER_IO_INTERLEAVE consecutive ones have
            // already run without a receive check - force one non-blocking peek before letting more
            // scheduler work run. Not the caller's own real wait (never blocks): if nothing's
            // there, fall straight back into scheduler processing next iteration.
            consecutive_scheduler_runs = 0;
            tt_ret_t peeked = tt_RET_TIMEOUT;
            if (busy_peek(node, &peeked)) {
                return peeked;
            }
        } else {
            consecutive_scheduler_runs = 0;
            tt_ret_t result;
            if (poll_wait_io(node, has_next, next, time, timeout, until_next_event, did_work, &result)) {
                return result;
            }
        }

        uint64_t new_time = tt_get_ns();
        if (!until_next_event) {
            timeout -= (int64_t)(new_time - time);
        }
        time = new_time;
        node->rx_clock_ns = time;
        if (until_next_event && did_work && time - poll_start >= (uint64_t)tt_RECEIVE_TIMEOUT) {
            return tt_RET_TIMEOUT; // the busy-node slice
        }
    }

    return tt_RET_TIMEOUT;
}

tt_ret_t tt_Context_interrupt(struct tt_Context* node) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    return tt_wake_signal(node);
}

static tt_ret_t node_set_discovery_locked(struct tt_Context* node, struct tt_Discovery* discovery,
                                          tt_DISCOVERY_CALLBACK callback, void* param) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    node->discovery = discovery;
    node->discovery_callback = discovery != NULL ? callback : NULL;
    node->discovery_callback_param = discovery != NULL ? param : NULL;
    return tt_RET_OK;
}

tt_ret_t tt_Context_set_discovery(struct tt_Context* node, struct tt_Discovery* discovery,
                                  tt_DISCOVERY_CALLBACK callback, void* param) {
    struct tt_Context* locked_node = node;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    tt_ret_t result = node_set_discovery_locked(node, discovery, callback, param);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

uint32_t tt_Discovery_count(const struct tt_Discovery* discovery) {
    if (discovery == NULL) {
        return 0;
    }
    uint32_t count = 0;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (discovery->entities[i].context_id != tt_CONTEXT_ID_INVALID && discovery->entities[i].alive) {
            count++;
        }
    }
    return count;
}

const struct tt_DiscoveredEntity* tt_Discovery_find(const struct tt_Discovery* discovery, uint8_t context_id,
                                                    uint32_t endpoint_id) {
    if (discovery == NULL) {
        return NULL;
    }
#if tt_DISCOVERY_INDEXED
    int32_t slot = discovery_slot_of(discovery, context_id, endpoint_id); // the index (4b), not a scan
    return slot >= 0 ? &discovery->entities[slot] : NULL;
#else
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (discovery->entities[i].context_id == context_id && discovery->entities[i].endpoint_id == endpoint_id) {
            return &discovery->entities[i];
        }
    }
    return NULL;
#endif
}

// The first entity of (context_id, endpoint_id) of `kind`: a publisher and a subscriber of one topic, or a client and
// a server of one service, in one context share the endpoint_id, and tt_Discovery_find() may return either.
static const struct tt_DiscoveredEntity* discovery_find_kind(const struct tt_Discovery* discovery, uint8_t context_id,
                                                             uint32_t endpoint_id, uint8_t kind) {
    if (discovery == NULL) {
        return NULL;
    }
#if tt_DISCOVERY_INDEXED
    int32_t slot = discovery_kind_slot_of(discovery, context_id, endpoint_id, kind);
    return slot >= 0 ? &discovery->entities[slot] : NULL;
#else
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* entity = &discovery->entities[i];
        if (entity->context_id == context_id && entity->endpoint_id == endpoint_id && entity->kind == kind) {
            return entity;
        }
    }
    return NULL;
#endif
}

const struct tt_DiscoveredEntity* tt_Discovery_find_entity(const struct tt_Discovery* discovery, uint8_t context_id,
                                                           uint32_t endpoint_id, uint32_t entity_id) {
    if (discovery == NULL) {
        return NULL;
    }
#if tt_DISCOVERY_INDEXED
    int32_t slot = discovery_entity_slot_of(discovery, context_id, endpoint_id, entity_id);
    return slot >= 0 ? &discovery->entities[slot] : NULL;
#else
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* entity = &discovery->entities[i];
        if (entity->context_id == context_id && entity->endpoint_id == endpoint_id && entity->entity_id == entity_id) {
            return entity;
        }
    }
    return NULL;
#endif
}

// See this function's own doc comment (tickle.h).
static bool node_entity_alive_locked(const struct tt_Context* node, const struct tt_DiscoveredEntity* entity,
                                     uint64_t now) {
    if (node == NULL || entity == NULL || entity->context_id == tt_CONTEXT_ID_INVALID) {
        return false;
    }
    if (entity->liveliness_lease_duration_ns == 0) {
        return entity->alive; // no specific lease requested - defer to the node-level sweep
    }
    if (!node->update_seen[entity->context_id]) {
        return false; // never heard from this node id at all
    }
    // The lease runs from the entity's last sign of life (LIVELINESS_PLAN.md rule 1) - see
    // entity_lease_anchor().
    return entity_within_lease(node, entity, now);
}

bool tt_Context_entity_alive(const struct tt_Context* node, const struct tt_DiscoveredEntity* entity, uint64_t now) {
    struct tt_Context* locked_node = (struct tt_Context*)node;
    if (locked_node != NULL) {
        state_lock(locked_node);
    }
    bool result = node_entity_alive_locked(node, entity, now);
    if (locked_node != NULL) {
        state_unlock(locked_node);
    }
    return result;
}

// ---- Receive-buffer lending (DESIGN.md section 10): tt_Sample_retain() / tt_Sample_release().
#if tt_SAMPLE_LENDING
// A handle is (generation << 8) | (entry + 1): the generation has the 24 bits above the entry's byte.
#define LEND_GENERATION_MASK 0xFFFFFFU

// Whether a retained sample still holds receive buffer `number`.
static bool lend_holds_buffer(const struct tt_Context* node, uint32_t number) {
    for (uint32_t k = 0; k < tt_SAMPLE_RETAIN_MAX; k++) {
        const struct tt_LendEntry* entry = &node->lend.entries[k];
        if (entry->kind == tt_LEND_BUFFER && entry->index == number) {
            return true;
        }
    }
    return false;
}

// A receive buffer no sample holds, other than `current` (the one the datagram being processed is in), or -1.
static int32_t lend_spare_buffer(const struct tt_Context* node, uint32_t current) {
    for (uint32_t number = 0; number <= node->lend.pool_count; number++) {
        if (number != current && !lend_holds_buffer(node, number)) {
            return (int32_t)number;
        }
    }
    return -1;
}

static int32_t lend_free_entry(const struct tt_Context* node) {
    for (uint32_t k = 0; k < tt_SAMPLE_RETAIN_MAX; k++) {
        if (node->lend.entries[k].kind == tt_LEND_FREE) {
            return (int32_t)k;
        }
    }
    return -1;
}

static tt_ret_t lend_retain_locked(struct tt_Context* node, const struct tt_Subscriber* sub, struct tt_Sample* out) {
    const struct tt_LendDelivery* delivery = node->lend.delivery;
    if (delivery == NULL || delivery->sub != sub) {
        return tt_RET_ILLEGAL_STATUS; // not inside this Subscriber's callback
    }
    // Lendable exactly when the payload lies in the datagram being processed: anything else is a copy core made into
    // its own storage (reassembly, reorder buffer, local delivery) and is not handed out.
    const uint8_t* base = node->lend.rx_base;
    uint8_t kind = node->lend.rx_kind;
    if (kind == tt_LEND_FREE || delivery->payload < base || delivery->length > node->lend.rx_length ||
        (size_t)(delivery->payload - base) > node->lend.rx_length - delivery->length) {
        node->lend.unlendable++;
        return tt_RET_UNSUPPORTED;
    }
    int32_t entry_index = lend_free_entry(node);
    if (entry_index < 0) {
        node->lend.exhausted++;
        return tt_RET_OUT_OF_BUFFER;
    }
    const void* region = NULL;
    if (kind == tt_LEND_BUFFER) {
        // The first sample kept in this buffer: the socket's next datagram needs another one to go to.
        if (!lend_holds_buffer(node, node->lend.rx_index)) {
            int32_t spare = lend_spare_buffer(node, node->lend.rx_index);
            if (spare < 0) {
                node->lend.exhausted++;
                return tt_RET_OUT_OF_BUFFER;
            }
            node->lend.landing = (uint8_t)spare;
        }
    } else {
#if tt_SEGMENT_ENABLED
        region = node->own_segment;
        node->lend.held_slots++;
#endif
    }
    struct tt_LendEntry* entry = &node->lend.entries[entry_index];
    entry->generation = (entry->generation + 1U) & LEND_GENERATION_MASK;
    if (entry->generation == 0) {
        entry->generation = 1;
    }
    entry->kind = kind;
    entry->index = node->lend.rx_index;
    entry->region = region;
    node->lend.held++;
    node->lend.retains++;
    out->payload = delivery->payload;
    out->length = delivery->length;
    out->is_native_endian = delivery->is_native;
    out->handle = (entry->generation << 8U) | ((uint32_t)entry_index + 1U);
    return tt_RET_OK;
}

tt_ret_t tt_Sample_retain(struct tt_Subscriber* sub, struct tt_Sample* out) {
    if (sub == NULL || out == NULL || sub->node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    struct tt_Context* node = sub->node;
    state_lock(node); // already held by the delivering thread, which is the only one that can succeed
    tt_ret_t result = lend_retain_locked(node, sub, out);
    state_unlock(node);
    return result;
}

// A slot no retained sample holds any more goes back to the writers - unless it is the record the drain is still
// processing (retained and released inside one callback): read_index is already past it, so a writer could claim it
// while the rest of the record (a batch) is still being read. segment_done() frees that one, after.
static void lend_release_slot(struct tt_Context* node, const struct tt_LendEntry* released) {
#if tt_SEGMENT_ENABLED
    struct tt_SegmentHeader* header = node->own_segment;
    if (header == NULL || released->region != header) {
        return; // not the segment it was read from: cannot happen while the release of a held one waits (DESIGN.md)
    }
    if (lend_holds_slot(node, header, released->index)) {
        return; // another sample of the same record (a batch) still holds it
    }
    if (node->lend.rx_kind == tt_LEND_SLOT && node->lend.rx_index == released->index) {
        return; // still being read
    }
    struct tt_SegmentSlot* slot_header = (struct tt_SegmentSlot*)segment_slot(header, released->index);
    // Release, after every read the holder made of the record: a writer that sees this reuses the slot.
    __atomic_store_n(&slot_header->sequence, released->index + header->slots, __ATOMIC_RELEASE);
#else
    UNUSED(node);
    UNUSED(released);
#endif
}

tt_ret_t tt_Sample_release(struct tt_Context* node, struct tt_Sample* sample) {
    if (node == NULL || sample == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    state_lock(node);
    uint32_t handle = sample->handle;
    uint32_t entry_index = (handle & 0xFFU) - 1U; // handle 0 wraps to a value past the table
    struct tt_LendEntry* entry = entry_index < tt_SAMPLE_RETAIN_MAX ? &node->lend.entries[entry_index] : NULL;
    if (entry == NULL || entry->kind == tt_LEND_FREE || entry->generation != (handle >> 8U)) {
        node->lend.bad_releases++;
        state_unlock(node);
        return tt_RET_INVALID_ARGUMENT; // not held: never released twice, never something else's slot or buffer
    }
    struct tt_LendEntry released = *entry;
    entry->kind = tt_LEND_FREE;
    entry->region = NULL;
    node->lend.held--;
    node->lend.releases++;
    if (released.kind == tt_LEND_SLOT) {
        node->lend.held_slots--;
        lend_release_slot(node, &released);
    }
    // A buffer needs nothing more: it is free once no entry names it, and the socket is pointed at a free one only
    // when a retain needs it to move (lend_retain_locked()).
    state_unlock(node);
    sample->payload = NULL;
    sample->length = 0;
    sample->handle = 0;
    return tt_RET_OK;
}

tt_ret_t tt_Context_set_rx_pool(struct tt_Context* node, uint64_t* storage, uint8_t count) {
    if (node == NULL || (storage == NULL && count != 0) || count > tt_RX_POOL_MAX) {
        return tt_RET_INVALID_ARGUMENT;
    }
    state_lock(node);
    bool busy = node->lend.landing != 0;
    for (uint32_t number = 1; number <= node->lend.pool_count && !busy; number++) {
        busy = lend_holds_buffer(node, number);
    }
    if (busy) {
        state_unlock(node);
        return tt_RET_ILLEGAL_STATUS; // a sample or the socket is in the pool being replaced
    }
    node->lend.pool = (uint8_t*)storage;
    node->lend.pool_count = storage == NULL ? 0 : count;
    state_unlock(node);
    return tt_RET_OK;
}

// Every retained sample forgotten, at tt_Context_destroy(): their bytes go with the context (the segment is unmapped
// next), and a release that comes later must be refused rather than write into a slot that is no longer mapped.
static void lend_forget_all(struct tt_Context* node) {
    if (node->lend.held != 0) {
        TT_LOG_WARNING("Context %u destroyed with %u retained sample(s) not released: their bytes are no longer valid",
                       node->id, (unsigned)node->lend.held);
    }
    for (uint32_t k = 0; k < tt_SAMPLE_RETAIN_MAX; k++) {
        node->lend.entries[k].kind = tt_LEND_FREE;
        node->lend.entries[k].region = NULL;
    }
    node->lend.held = 0;
    node->lend.held_slots = 0;
    node->lend.landing = 0;
    node->lend.segment_release_deferred = false;
}
#else
tt_ret_t tt_Sample_retain(struct tt_Subscriber* sub, struct tt_Sample* out) {
    return (sub == NULL || out == NULL) ? tt_RET_INVALID_ARGUMENT : tt_RET_UNSUPPORTED;
}

tt_ret_t tt_Sample_release(struct tt_Context* node, struct tt_Sample* sample) {
    UNUSED(node);
    UNUSED(sample);
    return tt_RET_INVALID_ARGUMENT; // nothing can have been retained
}

tt_ret_t tt_Context_set_rx_pool(struct tt_Context* node, uint64_t* storage, uint8_t count) {
    UNUSED(storage);
    UNUSED(count);
    return node == NULL ? tt_RET_INVALID_ARGUMENT : tt_RET_UNSUPPORTED;
}
#endif

static tt_ret_t node_destroy_locked(struct tt_Context* node);

tt_ret_t tt_Context_destroy(struct tt_Context* node) {
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }
    // Held across the teardown so a call already inside the node on another thread finishes first.
    // Not released into a destroyed lock afterwards: the locks are left initialised, because a late
    // tt_Context_interrupt() or a poll still unwinding must never touch a destroyed mutex, and on both
    // platforms an idle one costs nothing to keep. tt_Context_create() initialises them again.
    state_lock(node);
    tt_ret_t result = node_destroy_locked(node);
    state_unlock(node);
    return result;
}

#if tt_SAMPLE_LENDING
#define LEND_COUNT(node, field) ((node)->lend.field)
#else
#define LEND_COUNT(node, field) 0
#endif

static tt_ret_t node_destroy_locked(struct tt_Context* node) {
    // One line, at the one moment the whole run's traffic is known. Cheap enough to be
    // unconditional, and the question it answers - did anything arrive at all - is the first one
    // asked whenever a node delivered nothing.
    if (node == NULL) {
        return tt_RET_INVALID_ARGUMENT;
    }

    // After the null check, not before it: the first version of this line dereferenced node to
    // print the counters and only then asked whether node was NULL.
    // tx_udp/tx_shm/rx_udp/rx_shm are here as well as in the benchmark's RESULT line (SHM_PLAN
    // stage 0) because the two instruments reach different runs: the RESULT line covers the native
    // campaign, and this covers everything that goes through rmw - the acceptance suite runs rclpy
    // nodes, which have no RESULT line, so without this S2 could assert on the campaign and not on
    // the acceptance suite. Same names in both, so one grep reads either.
    //
    // tx_datagrams stays alongside deliberately rather than being replaced by the split: the two
    // are produced by the same seam and must agree, so a reader who sees tx_udp + tx_shm differ
    // from tx_datagrams is looking at a counting defect and not at a transport story.
    TT_LOG_INFO(
        "Node %u traffic: tx_datagrams=%lu rx_datagrams=%lu rx_self_sent=%lu rx_self_sent_data=%lu "
        "rx_self_sent_data_unicast=%lu rx_via_data=%lu rx_via_well_known=%lu tx_dropped_oversize=%lu "
        "tx_udp=%lu tx_shm=%lu rx_udp=%lu rx_shm=%lu rx_shm_skipped_superseded=%lu "
        // Why each UDP datagram went that way, on the same line as the totals. Without it a
        // split like tx_udp=6694744 tx_shm=6746571 says only "half and half" and the next
        // question - which half, and why - needs another run. It cost one on 2026-09-29.
        "tx_udp_broadcast=%lu tx_udp_oversize=%lu tx_udp_unattached=%lu shm_full_dropped=%lu "
        // How many peers this context gave up on, and how many doorbells it rang. The first
        // is the only counter that rises ONLY when a reader was judged dead, and without it
        // "the writer abandoned the corpse" cannot be told apart from the ordinary reasons
        // tx_udp_unattached rises - which is a test that cannot fail, found as one.
        "shm_gave_up=%lu shm_doorbells_sent=%lu shm_bells_rung=%lu shm_doorbells_received=%lu "
        // Whether this context ever built a segment, and whether it still has one. With
        // creation deferred until a same-host peer appears, tx_shm=0 has two entirely
        // different meanings - "no peer could have used one" and "one could, and it broke" -
        // and shm_segments_created is what separates them. A reader who sees created=0 on a
        // run that expected shared memory should look at same_host_peers before the ring.
        "shm_segments_created=%lu shm_segments_released=%lu shm_same_host_peers=%u "
        // How often a record a writer was still filling was waited for instead of slept on, how often it came
        // in time, and the bound (segment_await_claim()). A wait that seldom ends in a record is time spent
        // for nothing; only the two counts together say whether it pays.
        "shm_claim_waits=%lu shm_claim_waits_published=%lu shm_sleep_cost_ns=%lu shm_watches=%lu shm_watch_hits=%lu "
        // Whether waiting paid (segment_epoch_turn()): epochs measured in each mode, the recent mean cost per record
        // of each in ns (0: never measured), and the mode preferred at exit (1: waiting).
        "shm_epochs_sleeping=%lu shm_epochs_waiting=%lu shm_cost_sleeping_ns=%lu shm_cost_waiting_ns=%lu "
        "shm_waiting_preferred=%u shm_encoded_in_slot=%lu rx_drain_ring_turns=%lu "
        // How often this reader announced a sleep (generations, and of the announcements those that kept a called-off
        // sleep's generation; segment_sleep_called_off()) and how often it then waited: the announcements less the
        // waits are the sleeps called off, each of which a writer has usually rung for nothing.
        "shm_sleep_generations=%lu shm_generations_kept=%lu shm_sleeps=%lu "
        // Receive-buffer lending (DESIGN.md section 10): compiled in or not, and what it did. lending=1 with every
        // count 0 is the treatment check of an A/B that measures it unused.
        "lending=%u lend_retains=%lu lend_releases=%lu lend_unlendable=%lu lend_exhausted=%lu lend_bad_releases=%lu "
        "lend_held=%lu shm_full_retained=%lu "
        // Attaches refused because no live context owned the file: a dead one's segment, left in /dev/shm.
        "shm_attach_orphaned=%lu",
        node->id, (unsigned long)node->tx_datagrams, (unsigned long)node->rx_datagrams,
        (unsigned long)node->rx_self_sent, (unsigned long)node->rx_self_sent_data,
        (unsigned long)node->rx_self_sent_data_unicast, (unsigned long)node->rx_via_data_datagrams,
        (unsigned long)node->rx_via_well_known_datagrams, (unsigned long)node->tx_dropped_oversize,
        (unsigned long)node->tx_datagrams_by_transport[tt_TRANSPORT_UDP],
        (unsigned long)node->tx_datagrams_by_transport[tt_TRANSPORT_SHM],
        (unsigned long)node->rx_datagrams_by_transport[tt_TRANSPORT_UDP],
        (unsigned long)node->rx_datagrams_by_transport[tt_TRANSPORT_SHM],
        (unsigned long)node->rx_shm_skipped_superseded, (unsigned long)node->segment_broadcast_to_udp,
        (unsigned long)node->segment_oversized_to_udp, (unsigned long)node->segment_unattached_to_udp,
        (unsigned long)node->segment_full_dropped, (unsigned long)node->segment_attach[tt_SEGMENT_REFUSED],
        (unsigned long)node->segment_doorbells_sent, (unsigned long)node->segment_bells_rung,
        (unsigned long)node->segment_doorbells_received, (unsigned long)node->segments_created,
        (unsigned long)node->segments_released, node->same_host_peer_count, (unsigned long)node->segment_claim_waits,
        (unsigned long)node->segment_claim_waits_published, (unsigned long)node->segment_sleep_cost_ns,
        (unsigned long)node->segment_watches, (unsigned long)node->segment_watch_hits,
        (unsigned long)node->segment_epochs[0], (unsigned long)node->segment_epochs[1],
        (unsigned long)node->segment_cost_mean_ns[0], (unsigned long)node->segment_cost_mean_ns[1],
        (unsigned)node->segment_preferred, (unsigned long)node->segment_encoded_in_slot,
        (unsigned long)node->rx_drain_ring_turns, (unsigned long)node->segment_sleep_generation,
        (unsigned long)node->segment_generations_kept, (unsigned long)node->segment_sleeps, (unsigned)tt_SAMPLE_LENDING,
        (unsigned long)LEND_COUNT(node, retains), (unsigned long)LEND_COUNT(node, releases),
        (unsigned long)LEND_COUNT(node, unlendable), (unsigned long)LEND_COUNT(node, exhausted),
        (unsigned long)LEND_COUNT(node, bad_releases), (unsigned long)LEND_COUNT(node, held),
        (unsigned long)LEND_COUNT(node, full_retained), (unsigned long)node->segment_attach[tt_SEGMENT_ORPHANED]);
    // Said out loud rather than left for a reader to derive, because the derivation is exactly the
    // one nobody performs: a run that received on only one socket never interleaved them, so it
    // cannot be read as evidence either way about interleaving reordering delivery. It reads
    // identically to a run that interleaved and stayed in order.
    if (node->rx_datagrams > 0 && (node->rx_via_data_datagrams == 0 || node->rx_via_well_known_datagrams == 0)) {
        TT_LOG_WARNING("Node %u received on only one socket (%s) - this run did not interleave them, so its "
                       "delivery-order counters are void for that question, not negative",
                       node->id, node->rx_via_data_datagrams == 0 ? "well-known only" : "data only");
    }
    uint64_t time = tt_get_ns();

    for (uint32_t i = 0; i < node->endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node->endpoints[i];
        // A Subscriber still attached at node teardown is reported here; one destroyed earlier
        // reported itself in tt_Subscriber_destroy(). The two are mutually exclusive, because
        // that function removes the endpoint from this table before returning.
        if (endpoint != NULL && endpoint->kind == tt_KIND_TOPIC_SUBSCRIBER) {
            report_delivery_counters((struct tt_Subscriber*)endpoint, endpoint->id);
        }
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
                server->cache_sent_at[j] = 0;
            }
        }
    }
    node->endpoint_count = 0;

    // Broadcast a final, entity-less announce so peers can drop this node right away
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
    node->scheduler_tail = 0; // the state lock is held (tt_Context_destroy())
    for (int i = 0; i < tt_SCHED_INBOX_LENGTH; i++) {
        __atomic_store_n(&node->sched_inbox_state[i], tt_SCHED_SLOT_EMPTY, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&node->sched_inbox_pending, 0, __ATOMIC_RELAXED);

#if tt_SAMPLE_LENDING
    lend_forget_all(node); // before the segment is unmapped: a later release must find nothing to write to
#endif
#if tt_SEGMENT_ENABLED
    // Before the socket goes: the segment is named from this context's address, and the
    // teardown below is the last point at which that name is still this context's.
    release_segments(node);
#endif
    tt_close(node);

    return tt_RET_OK;
}

const char* tt_version(void) {
    return TICKLE_VERSION_STRING;
}
