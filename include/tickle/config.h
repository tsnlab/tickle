/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

#include <stdint.h>

#define tt_SECOND 1000000000ULL
#define tt_MILLISECOND 1000000ULL
#define tt_MICROSECOND 1000ULL

#define tt_NODE_CYCLE tt_MILLISECOND // nanosecond
// How often a node re-broadcasts its endpoint list (discovery announce). The first announce goes
// out ~tt_NODE_CYCLE after tt_Node_create(), and a node that hears a peer's announce for the
// first time replies with its own straight away (see reply_with_own_announce() in tickle.c), so
// mutual discovery is effectively immediate on a healthy link - this interval is the recovery
// cadence for an announce lost to packet loss, or for a node that was already up when this one
// started. 1s keeps that recovery quick while costing one small packet per node per second.
#define tt_NODE_UPDATE_INTERVAL (1 * tt_SECOND) // nanosecond
#define tt_NODE_TX_INTERVAL tt_MILLISECOND      // nanosecond
// QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - a reliable Subscriber's ACKNACK
// re-send interval override (0 = auto, i.e. tt_RELIABLE_RETRY_INTERVAL below; same convention as
// struct tt_Service.call_retry_interval) and the max retransmit attempts a reliable Publisher
// makes for one cached sample before giving up on it (mirrors tt_CALL_RETRY_COUNT). A Topic's
// deadline_duration/lifespan_duration (tickle.h) are still reserved for #2/#6, not this.
#define tt_RELIABLE_DEADLINE 0 // nanosecond, 0 is auto
#define tt_RELIABLE_RETRY 3    // count
// Phase 1-b (rmw_tickle/PLAN.md, H3) - the reliable Subscriber's own default ACKNACK retry
// interval, separate from RPC's tt_CALL_RETRY_INTERVAL (5ms, which it used to share). Real HIL
// (Phase 0-c/1-a counters) put every successful recovery under 256us after its ACKNACK, while a
// retry for a lost ACKNACK/retransmit waited the full 5ms - long enough for a depth-64 Publisher
// cache (~4.8ms at 13K msg/s, ~0.34ms at max rate) to evict the sample first. 1ms keeps a ~4x
// margin over that RTT.
#define tt_RELIABLE_RETRY_INTERVAL (1 * tt_MILLISECOND) // nanosecond
// rmw_tickle/PLAN.md's "DDS semantic-parity backlog" row 2 - struct tt_ReliableCache (tickle.h)
// no longer embeds a fixed-size array sized by this constant: entries[]/capacity are now caller-
// owned (any size the caller's own backing array happens to be - stack, static, or, for a caller
// that already accepts dynamic allocation elsewhere like rmw_tickle, heap), so a specific
// Publisher's own real ceiling is whatever array it was actually given, not a single build-wide
// constant every Publisher was capped by or paid for alike. This constant is now only the reference
// value examples/tests default a Publisher's own array size to when they have no other reason to
// pick something different (tt_ReliableCache's own doc comment) - not a limit on what a caller
// *may* choose, just a reasonable, historically-tuned starting point.
//
// It used to have a second, Subscriber-side role: skip_unrecoverable_backlog()'s (tickle.c) guess at
// how deep a *remote* Publisher's cache reaches back, used to bulk-skip on an ACKNACK give-up. Phase
// 1-c (rmw_tickle/PLAN.md, B2) removed that guess - a Publisher now answers an ACKNACK naming an
// evicted sample with an eviction Heartbeat carrying its real first_available_seq_no, so the
// Subscriber skips exactly what's gone instead of assuming a depth of 64 (which threw away samples a
// deeper cache, e.g. -K 1024, still held).
//
// Whether raising this value past tt_RELIABLE_BITMAP_BITS (below) would help
// anything: see struct tt_ReliableCache's own doc comment (tickle.h) for the fuller answer - a
// deeper *Publisher*-side cache alone was never the fix for RELIABLE's own ACKNACK-driven recovery
// under sustained loss (only DURABILITY's own one-shot backlog push benefits from that); the real
// bottleneck was always the Subscriber's own received_bitmap width, which this default already
// respects via the _Static_assert (tickle.c) keeping it at or under
// whatever that width currently is. That width was widened 64 -> 256 bits (rmw_tickle/PLAN.md's
// "TickLE-native performance" plan) once real HIL confirmed it, not just the cache depth, was the
// actual ceiling - see tt_RELIABLE_BITMAP_BITS's own doc comment for the honest "raises the
// tolerable gap, doesn't remove the ceiling entirely" framing.
//
// History: 8 -> 10 -> 64, chasing a real cache-eviction-vs-ACKNACK-round-trip-recovery race under
// run_perf.sh's own loss-injection scenarios (LOSS_TEST_INTERVAL_SEC's own comment) - the retained
// window (depth * send interval) has to outlast a real retransmit round trip before an unacked
// sample gets evicted, or genuine, otherwise-recoverable loss gets written off too early. Settled
// at 64 once PLAN.md's Milestone 25 traced the residual ~0.1% loss_pct floor this whole tuning
// history was chasing to an unrelated measurement bug in examples/linux/perf/perf_server.c's own
// drop-counting, not real TickLE-core loss or this constant's own value at all - every earlier
// loss_pct figure this constant was ever tuned against (the 5.2%/9.6%/8.0% cliff at depth 8, etc.)
// was measured through that same buggy counter, so none of it was trustworthy calibration data
// regardless. run_perf.sh's own loss-injection client passes an explicit -K <depth> (examples/
// linux/perf/perf_client.c, cli_opts.h/.c) to set struct tt_ReliableCache.depth directly for that
// experiment, independent of whatever this constant's own default is used for elsewhere.
//
// Also still the only retained-sample cache depth concept in this file - QoS roadmap #4
// (DURABILITY) used to have its own separate tt_MAX_DURABLE_HISTORY constant and struct tt_
// DurableCache (tickle.h) sized independently of this one, requiring a _Static_assert (tickle.c)
// to keep the two in sync whenever either changed. Unified into this one constant/cache instead
// (PLAN.md's Milestone 24) - matching real DDS/RTPS, where DURABILITY (at the TRANSIENT_LOCAL
// level this package implements) isn't a separately-sized cache at all: a late-joining reader
// just gets whatever's currently sitting in the Writer's own single History Cache, which HISTORY.
// depth (and RESOURCE_LIMITS) already govern for RELIABILITY's own retransmission - there's no
// independent "durability depth" concept to keep in sync with anything, only ever one cache.
#define tt_MAX_RELIABLE_HISTORY 64
// B1 (rmw_tickle/PLAN.md) - sizing helpers for struct tt_ReliableCache's own caller-provided byte
// arena (tickle.h). tt_RELIABLE_RECORD_BYTES(payload) is what one cached sample actually costs on
// the wire and in the arena: the submessage header, the DATA header, and the payload itself,
// rounded up to the 4-byte submessage alignment tickle.c's own end_encode() applies.
//
// tt_RELIABLE_CACHE_ARENA_BYTES(depth, max_record) is `depth + 1` records, not `depth`: records
// are never split across the end of the arena, so a record that doesn't fit before the end wraps
// to offset 0 and wastes up to max_record - 1 bytes. Without that one record of slack, the byte
// bound could evict a sample before `depth` of them are retained - i.e. silently break
// KEEP_LAST-by-count, which is what `depth` promises.
#define tt_RELIABLE_RECORD_BYTES(payload_bytes) \
    ((uint32_t)((((4u + 20u + (uint32_t)(payload_bytes)) + 3u) / 4u) * 4u)) // 4 = submessage header,
                                                                            // 20 = sizeof(struct tt_DataHeader)
#define tt_RELIABLE_CACHE_ARENA_BYTES(depth, max_record) (((uint32_t)(depth) + 1u) * (uint32_t)(max_record))
// Width of tt_AckNackHeader.bitmap/tt_WriterProxy.received_bitmap, both now tt_RELIABLE_BITMAP_
// WORDS-word uint64_t arrays (256 bits total - widened from a single, bare-uint64_t 64 bits,
// rmw_tickle/PLAN.md's "TickLE-native performance" plan, tt_VERSION bumped 4 -> 5 for the wire
// layout change). tt_MAX_RELIABLE_HISTORY above must never exceed this (tickle.c's own
// _Static_assert enforces it) - a gap this wide can never be named in a single ACKNACK bitmap in
// the first place, so a deeper cache couldn't be selectively recovered from anyway. Matches real
// RTPS's own practical SequenceNumberSet width (typically up to 256) - not a novel choice.
// Real-HIL-measured motivation: at TickLE's own real throughput (~1.2M msg/s), the old 64-bit
// window represented only ~53us of send time - almost certainly shorter than one real ACKNACK
// round trip on any physical link, capping RELIABLE's own tc-loss recovery well below 100% even
// though the Publisher's own retained-cache depth (Milestone 61) was never the actual bottleneck
// (COMPARISON.MD §6 item 9/10's own re-measurement already ruled that out). See struct tt_
// WriterProxy.received_bitmap's own doc comment (tickle.h) for the honest "this raises the
// tolerable gap ~4x, not a guaranteed full fix - the real link's own ACKNACK RTT still sets the
// actual limit" caveat.
#define tt_RELIABLE_BITMAP_BITS 256
// Word count backing the tt_RELIABLE_BITMAP_BITS-wide bitmap arrays above - every bit-manipulation
// site (highest_received_bit()/update_reliable_ack()/jump_ack_baseline()/send_acknack()/
// process_heartbeat(), tickle.c) operates in units of this many uint64_t words, not raw bits, to
// stay O(word-count) rather than O(bit-count) wherever the access pattern allows it (the same
// "O(1)/O(word-count), not O(size)" lesson Milestone 61's own find_resendable_cache_entry()
// regression - a naive per-element scan across a widened fixed structure measurably regressing
// performance - already taught this codebase once).
// Bits per word in the arrays tt_RELIABLE_BITMAP_WORDS sizes - the width of the uint64_t this
// codebase's own bitmap_*() helpers (tickle.c, next to highest_received_bit()) shift/index within,
// named so those helpers don't compare against a bare 64 (clang-tidy's own readability-magic-
// numbers check, matching this file's own established "name it" convention for every other fixed
// width here).
#define tt_RELIABLE_BITMAP_WORD_BITS 64
#define tt_RELIABLE_BITMAP_WORDS (tt_RELIABLE_BITMAP_BITS / tt_RELIABLE_BITMAP_WORD_BITS)
// Phase 2 (rmw_tickle/PLAN.md) - the widest tracking window a Subscriber may ask for, and the
// upper bound every decode path validates an incoming ACKNACK's own word count against (a
// malformed or hostile count must never index past a local buffer). tt_RELIABLE_BITMAP_BITS above
// stays the *default* window every Subscriber gets for free: TickLE core is embedded-first
// (PLAN.md's Project Goal 1), and 4096 bits is 512 bytes per tracked writer that a microcontroller
// nowhere near 190K msg/s would never use. A Linux-class caller (rmw_tickle, Goal 5, and the
// perf_hil examples via their own flag) opts into a wider one per Subscriber by handing
// tt_Node_create_subscriber()'s own caller-owned tracking buffer - see struct tt_Subscriber's own
// window doc comment (tickle.h).
#define tt_RELIABLE_BITMAP_MAX_BITS 4096
#define tt_RELIABLE_BITMAP_MAX_WORDS (tt_RELIABLE_BITMAP_MAX_BITS / tt_RELIABLE_BITMAP_WORD_BITS)
#define tt_CALL_RETRY_INTERVAL (5 * tt_MILLISECOND)    // Default value
#define tt_CALL_RETRY_COUNT 3                          // count
#define tt_SERVER_CACHE_TIMEOUT (100 * tt_MILLISECOND) // (Client server latency) * (CALL_RETRY_COUNT + 1)
// How long a tt_SERVER_CALLBACK that returned tt_CALL_DEFERRED has to eventually call
// tt_Server_send_response() before the slot reserved for it is reclaimed (Milestone 17,
// rmw_tickle/PLAN.md) - deliberately much longer than tt_SERVER_CACHE_TIMEOUT above, which times
// out re-sending an *already-computed* answer to a retrying client, not waiting on the
// application to compute one in the first place. Matches rmw_tickle's own pre-existing
// RMW_TICKLE_SERVICE_RESPONSE_TIMEOUT_NS default (rmw_tickle.h) - not a coincidence, that value
// was standing in for this exact primitive not existing yet.
#define tt_SERVER_DEFERRED_RESPONSE_TIMEOUT (5 * tt_SECOND)
#define tt_RECEIVE_TIMEOUT (100 * tt_MICROSECOND) // Network socket default receive timeout
// EXPERIMENTAL (branch experiment/poll-loop-io-interleave, rmw_tickle/PLAN.md's own "Further
// latency research" section) - tt_Node_poll()'s own inner loop favors an already-due scheduler
// entry over ever calling tt_receive(), with no cap on how many may run consecutively before an
// I/O check happens. A continuously-rescheduling task (e.g. a max-rate Publisher's own send loop,
// interval_s=0) can then starve tt_receive() for a whole call's own tt_RECEIVE_TIMEOUT budget,
// meaning ACKNACK responsiveness ends up bounded by how rarely the scheduler queue goes idle, not
// by real network RTT. This bounds how many scheduler entries may run back-to-back before a
// forced, non-blocking tt_try_receive() peek is squeezed in between them - unvalidated on real HIL
// yet, this specific value (8) is a first guess, not yet tuned.
#define tt_SCHEDULER_IO_INTERLEAVE 8
// Requested SO_SNDBUF/SO_RCVBUF size. The kernel silently clamps this to whatever
// net.core.[rw]mem_max allows for an unprivileged process, so asking for more than that is
// harmless - it's cheap insurance against drops under bursty send/receive on systems where the
// ceiling is higher than the (often small, e.g. 208KB) distro default.
#define tt_SOCKET_BUFFER_SIZE (1024 * 1024)

#define tt_MAX_ENDPOINT_COUNT 256 // Maximum number of endpoints (data or services)
// Size of tt_Node.endpoint_index (power of two, >= 2 * tt_MAX_ENDPOINT_COUNT so load stays
// <= 0.5 for linear-probe lookups).
#define tt_ENDPOINT_INDEX_SIZE 512
#define tt_MAX_NAME_LENGTH 255     // Maximum length of endpoint name
#define tt_MAX_STRING_LENGTH 65535 // Maximum length of string
// RX/TX buffering size to flush: the largest UDP payload a standard 1500-byte Ethernet MTU
// can carry without IP fragmentation. 1500 (MTU) - 20 (IPv4 header) - 8 (UDP header) = 1472.
// Previously 1480, which is 8 bytes *larger* than that limit - a packet in the 1473-1480
// byte range would pass this check yet still fragment at the IP layer on a standard network.
#define tt_MAX_BUFFER_LENGTH 1472

// Node ID values are the last byte of the IPv4 address on the local network.
// Valid node IDs are 1..254, because 0 is reserved for invalid/unassigned and
// 255 is reserved for the broadcast address.
#define tt_NODE_ID_INVALID 0x00
#define tt_NODE_ID_BROADCAST 0xff
#define tt_MAX_SCHEDULER_LENGTH 128  // Scheduling queue
#define tt_MAX_SERVER_CACHE_COUNT 64 // >= # of client

// Threshold for how many known recipient nodes a Publisher/Client sends to individually before
// switching to one broadcast instead. <= this many known peers -> unicast (tt_send_to() once per
// peer); more than this many -> broadcast (tt_send() once). Zero known peers (nobody has
// announced a matching endpoint yet) always broadcasts too, regardless of this threshold -
// there's nothing to unicast to yet, so it falls back to today's discovery-by-broadcast behavior.
#define tt_UNICAST_PEER_THRESHOLD 2

// Fixed capacity of each Publisher's/Client's peers[] table (struct tt_Peer, tickle.h) - the
// known set of remote nodes (IP:port) hosting a matching Subscriber/Server, learned from their
// periodic UPDATE announces. Must stay > tt_UNICAST_PEER_THRESHOLD: once full, a never-seen
// peer is silently dropped rather than tracked (see upsert_peer() in tickle.c) - safe only
// because a full table already implies "more than the threshold", i.e. already broadcasting,
// which still reaches that dropped peer too.
#define tt_MAX_PEER_COUNT 8
// Phase 2 (rmw_tickle/PLAN.md) - how many remote Subscriber *entities* one Publisher tracks ack
// state for (struct tt_PeerAck, tickle.h). Deliberately its own constant rather than reusing
// tt_MAX_PEER_COUNT above, which counts remote *nodes*: one node can host several Subscriptions of
// the same topic, and each needs its own ack watermark for Phase 3's KEEP_ALL blocking to be
// correct. 12 bytes per entry.
#define tt_MAX_ACK_ENTRIES 16

// Liveliness: a remote node is considered gone once this many *consecutive* tt_NODE_UPDATE_
// INTERVAL windows pass with no UPDATE announce heard from it at all - not merely no *change*
// (see tt_Node's own update_last_seen[], tracked separately from update_last_modified[]/
// update_seen[], which only move when the announced content itself changes). A single announce
// lost to UDP packet loss is common and shouldn't immediately declare an otherwise-healthy node
// dead; too high a value delays noticing a real departure (a crash, a pulled cable - anything
// that skips tt_Node_destroy()'s own farewell UPDATE). 3 matches the conventional heartbeat-miss
// default other discovery protocols use for the same reason.
#define tt_LIVELINESS_MISS_THRESHOLD 3

// Fixed capacity of an opt-in struct tt_Discovery (tickle.h, tt_Node_set_discovery()) - the
// number of distinct remote entities (across every node it's ever heard an UPDATE from) it can
// track at once for graph introspection. Unrelated to tt_MAX_PEER_COUNT (that's a *local*
// endpoint's own known-unicast-destinations table; this is one shared cache of *every* remote
// entity a node has opted into recording, regardless of whether it matches anything local).
// Silently drops a new entity past this limit (see upsert_discovered_entity() in tickle.c) -
// introspection is a best-effort aid, not something correctness depends on. Each entry costs
// roughly 2 * (tt_MAX_NAME_LENGTH + 1) bytes for its type/name strings alone, so this is
// deliberately much smaller than tt_MAX_ENDPOINT_COUNT.
#define tt_MAX_DISCOVERED_ENTITIES 16

#define _tt_NODE_ADDRESS "0.0.0.0"
#define _tt_NODE_PORT 8282
#define _tt_NODE_BROADCAST "255.255.255.255"

struct _tt_Config {
    char* addr;
    int port;
    char* broadcast;
    // tt_NODE_ID_INVALID (0, the default) = auto-detect via tt_get_node_id() (the last byte of
    // the local address matching broadcast's subnet, per the comment above); any other value
    // overrides it. Auto-detection needs each node to have its own distinct address in that
    // subnet, which real separate hosts (or namespaces) give for free but a single shared network
    // namespace can't - two processes on the same host/interface would otherwise both detect the
    // same id and start silently dropping each other's packets as "self sent" (see
    // process_packet() in tickle.c). An explicit override sidesteps that: e.g.
    // platform/linux/test.sh runs both sides of a pair in one namespace over loopback, each
    // started with a different id, without needing root for network namespaces at all.
    int32_t node_id;
};

extern struct _tt_Config _tt_CONFIG;
