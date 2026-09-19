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
// re-send interval (mirrors tt_CALL_RETRY_INTERVAL's role for RPC; 0 = auto, same convention as
// struct tt_Service.call_retry_interval) and the max retransmit attempts a reliable Publisher
// makes for one cached sample before giving up on it (mirrors tt_CALL_RETRY_COUNT). A Topic's
// deadline_duration/lifespan_duration (tickle.h) are still reserved for #2/#6, not this.
#define tt_RELIABLE_DEADLINE 0 // nanosecond, 0 is auto
#define tt_RELIABLE_RETRY 3    // count
// Max retained-sample cache depth for a RELIABLE Publisher's opt-in struct tt_ReliableCache
// (tickle.h) - the fixed array dimension backing whatever depth a caller actually requests
// (clamped to this at setup time, e.g. rmw_tickle from qos_profile->depth). Same "small hard
// cap, caller picks a real value within it" trade-off as tt_MAX_PEER_COUNT/tt_MAX_SERVER_CACHE_COUNT.
//
// This is a *compile-time ceiling*, not the depth any given Publisher actually uses at runtime -
// struct tt_ReliableCache.depth (tickle.h) is the real, freely-configurable knob (clamped to this
// constant), exactly mirroring real DDS's own split between a resource-limit ceiling
// (RESOURCE_LIMITS.max_samples_per_instance) and the actual requested value (HISTORY.depth) - a
// caller picks whatever it wants at or below this cap without needing a rebuild. Fixed at 64,
// tt_RELIABLE_BITMAP_BITS's own hard ceiling below - a single ACKNACK can never name a gap wider
// than that, so raising this constant past it would only add slots update_reliable_ack() could
// never selectively recover anyway, regardless of what any caller's own depth requests.
//
// History: 8 -> 10 -> 64, chasing a real cache-eviction-vs-ACKNACK-round-trip-recovery race under
// run_perf.sh's own loss-injection scenarios (LOSS_TEST_INTERVAL_SEC's own comment) - the retained
// window (depth * send interval) has to outlast a real retransmit round trip before an unacked
// sample gets evicted, or genuine, otherwise-recoverable loss gets written off too early. Settled
// at 64 (this constant's own hard ceiling) once PLAN.md's Milestone 25 traced the residual ~0.1%
// loss_pct floor this whole tuning history was chasing to an unrelated measurement bug in
// examples/linux/perf/perf_server.c's own drop-counting, not real TickLE-core loss or this
// constant's own value at all - every earlier loss_pct figure this constant was ever tuned against
// (the 5.2%/9.6%/8.0% cliff at depth 8, etc.) was measured through that same buggy counter, so none
// of it was trustworthy calibration data regardless. With the counter fixed, this constant no
// longer needs to double as a *tuning* knob at all - run_perf.sh's own loss-injection client passes
// an explicit -K <depth> (examples/linux/perf/perf_client.c, cli_opts.h/.c) to set struct tt_
// ReliableCache.depth directly for that experiment, leaving this constant free to just be the
// structural ceiling it always should have been.
//
// Also now the *only* retained-sample cache depth in this file - QoS roadmap #4 (DURABILITY)
// used to have its own separate tt_MAX_DURABLE_HISTORY constant and struct tt_DurableCache
// (tickle.h) sized independently of this one, requiring a _Static_assert (tickle.c) to keep the
// two in sync whenever either changed. Unified into this one constant/cache instead (PLAN.md's
// Milestone 24) - matching real DDS/RTPS, where DURABILITY (at the TRANSIENT_LOCAL level this
// package implements) isn't a separately-sized cache at all: a late-joining reader just gets
// whatever's currently sitting in the Writer's own single History Cache, which HISTORY.depth (and
// RESOURCE_LIMITS) already govern for RELIABILITY's own retransmission - there's no independent
// "durability depth" concept to keep in sync with anything, because there's only ever one cache.
#define tt_MAX_RELIABLE_HISTORY 64
// Width of tt_AckNackHeader.bitmap/tt_WriterProxy.received_bitmap - inherent to their uint64_t
// wire/in-memory type, not a tunable, but named anyway so update_reliable_ack()/process_acknack()
// (tickle.c) don't compare against a bare 64. tt_MAX_RELIABLE_HISTORY above must never exceed
// this (tickle.c's own _Static_assert enforces it) - a gap this wide can never be named in a
// single ACKNACK bitmap in the first place, so a deeper cache couldn't be selectively recovered
// from anyway.
#define tt_RELIABLE_BITMAP_BITS 64
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
