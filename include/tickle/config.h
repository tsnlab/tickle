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

// Every setting below is wrapped in #ifndef so a build can override it from the command line
// (-Dtt_UNICAST_PEER_THRESHOLD=0) or from a project's own prefix header, without patching this
// file. That is how a tunable is normally exposed, and it is not how this file used to be: the
// defines were unconditional, so a -D on the command line was accepted by the compiler and then
// silently discarded when this header redefined the name. On 2026-09-23 that cost two full A/B
// runs which appeared to refute a correct hypothesis - the flag reached the compiler, a check
// confirmed exactly that, and the header threw it away afterwards. A guard that can be overridden
// is also a guard whose override can be *verified*, by checking the value rather than the flag.
//
// Four kinds of name here are deliberately NOT overridable, because they are not settings:
//   - tt_SECOND / tt_MILLISECOND / tt_MICROSECOND are unit definitions.
//   - tt_CONTEXT_ID_INVALID / tt_CONTEXT_ID_BROADCAST are wire sentinels; changing one on a single node
//     breaks interoperability with every other node rather than tuning anything.
//   - tt_RELIABLE_BITMAP_WORDS / _MAX_WORDS are derived from the bit counts above them, and must
//     stay derived.
//   - tt_RELIABLE_RECORD_BYTES / tt_RELIABLE_CACHE_ARENA_BYTES are function-like macros.
// Override the inputs to those, not the results.
//
// Overriding a size does not suspend the invariants between sizes. The _Static_asserts at the end
// of this file fail the build on a combination that cannot work, which is the point of allowing
// the override at all - a silently broken configuration would be worse than an unoverridable one.

#include <assert.h>  // static_assert in C11 - see the invariant asserts at the end of this file
#include <stdbool.h> // struct _tt_Link.resolved
#include <stdint.h>  // UINT8_MAX, for those same asserts

// The tt_NODE_* settings became tt_CONTEXT_* on 2026-09-27 (rmw_tickle/CONTEXT_NODE_PLAN.md stage 1): they belong to
// the context, which owns the sockets, the scheduler and discovery. An old name set with -D would otherwise be ignored
// without a word behind the #ifndef defaults below, so each one stops the build instead.
#ifdef tt_NODE_CYCLE
#error "tt_NODE_CYCLE is now tt_CONTEXT_CYCLE (CONTEXT_NODE_PLAN.md stage 1)"
#endif
#ifdef tt_NODE_UPDATE_INTERVAL
#error "tt_NODE_UPDATE_INTERVAL is now tt_CONTEXT_UPDATE_INTERVAL (CONTEXT_NODE_PLAN.md stage 1)"
#endif
#ifdef tt_NODE_TX_INTERVAL
#error "tt_NODE_TX_INTERVAL is now tt_CONTEXT_TX_INTERVAL (CONTEXT_NODE_PLAN.md stage 1)"
#endif
#ifdef tt_NODE_MAX_LEASE_NS
#error "tt_NODE_MAX_LEASE_NS is now tt_CONTEXT_MAX_LEASE_NS (CONTEXT_NODE_PLAN.md stage 1)"
#endif
#ifdef tt_NODE_ID_INVALID
#error "tt_NODE_ID_INVALID is now tt_CONTEXT_ID_INVALID (CONTEXT_NODE_PLAN.md stage 1)"
#endif
#ifdef tt_NODE_ID_BROADCAST
#error "tt_NODE_ID_BROADCAST is now tt_CONTEXT_ID_BROADCAST (CONTEXT_NODE_PLAN.md stage 1)"
#endif
#ifdef _tt_NODE_ADDRESS
#error "_tt_NODE_ADDRESS is now _tt_CONTEXT_ADDRESS (CONTEXT_NODE_PLAN.md stage 1)"
#endif
#ifdef _tt_NODE_PORT
#error "_tt_NODE_PORT is now _tt_CONTEXT_PORT (CONTEXT_NODE_PLAN.md stage 1)"
#endif
#ifdef _tt_NODE_BROADCAST
#error "_tt_NODE_BROADCAST is now _tt_CONTEXT_BROADCAST (CONTEXT_NODE_PLAN.md stage 1)"
#endif

#define tt_SECOND 1000000000ULL
#define tt_MILLISECOND 1000000ULL
#define tt_MICROSECOND 1000ULL

#ifndef tt_CONTEXT_CYCLE
#define tt_CONTEXT_CYCLE tt_MILLISECOND // nanosecond
#endif
// How often a node re-broadcasts its endpoint list (discovery announce). The first announce goes
// out ~tt_CONTEXT_CYCLE after tt_Context_create(), and a node that hears a peer's announce for the
// first time replies with its own straight away (see reply_with_own_announce() in tickle.c), so
// mutual discovery is effectively immediate on a healthy link - this interval is the recovery
// cadence for an announce lost to packet loss, or for a node that was already up when this one
// started. 1s keeps that recovery quick while costing one small packet per node per second.
#ifndef tt_CONTEXT_UPDATE_INTERVAL
#define tt_CONTEXT_UPDATE_INTERVAL (1 * tt_SECOND) // nanosecond
#endif
#ifndef tt_CONTEXT_TX_INTERVAL
#define tt_CONTEXT_TX_INTERVAL tt_MILLISECOND // nanosecond
#endif
// QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - a reliable Subscriber's ACKNACK
// re-send interval override (0 = auto, i.e. tt_RELIABLE_RETRY_INTERVAL below; same convention as
// struct tt_Service.call_retry_interval) and the max retransmit attempts a reliable Publisher
// makes for one cached sample before giving up on it (mirrors tt_CALL_RETRY_COUNT). A Topic's
// deadline_duration/lifespan_duration (tickle.h) are still reserved for #2/#6, not this.
#ifndef tt_RELIABLE_DEADLINE
#define tt_RELIABLE_DEADLINE 0 // nanosecond, 0 is auto
#endif
#ifndef tt_RELIABLE_RETRY
#define tt_RELIABLE_RETRY 3 // count
#endif
// Phase 1-b (rmw_tickle/PLAN.md, H3) - the reliable Subscriber's own default ACKNACK retry
// interval, separate from RPC's tt_CALL_RETRY_INTERVAL (5ms, which it used to share). Real HIL
// (Phase 0-c/1-a counters) put every successful recovery under 256us after its ACKNACK, while a
// retry for a lost ACKNACK/retransmit waited the full 5ms - long enough for a depth-64 Publisher
// cache (~4.8ms at 13K msg/s, ~0.34ms at max rate) to evict the sample first. 1ms keeps a ~4x
// margin over that RTT.
//
// 0 means dynamic (2026-09-25, the user's decision): each writer proxy derives its own interval
// from how long its recoveries actually take, so the timer stops firing before a recovery could
// have completed. Any other value is the caller's explicit choice and is used as-is. The measured
// case for it: at 250us recoveries the fixed 1ms barely fires (4% of ACKNACKs were timer-driven),
// while at the 2-4ms recoveries of a collapsing link it fired about three times per recovery, and
// every firing re-requested samples already in flight - 92% of ACKNACKs there were timer-driven.
// See the three constants below for how the dynamic value is bounded.
//
// Dynamic is the shipped default since 2026-09-25, the user's decision: if our own estimate is good
// enough, deriving the value beats any fixed one a caller would have to guess. What the rig showed,
// against the old fixed 1ms, same QoS on every arm: at a healthy link (c5) 1.75% more throughput for
// 0.62% more bytes per sample - 1.12% more delivered per byte, so not a regression; under high RTT,
// 42.6% less retransmit bandwidth for 14.0% less throughput, a genuine trade. A build that needs the
// old behaviour sets 1ms here explicitly.
#ifndef tt_RELIABLE_RETRY_INTERVAL
#define tt_RELIABLE_RETRY_INTERVAL 0 // nanosecond, 0 = dynamic (the default); any other value is used as-is
#endif
// Dynamic retry interval (tt_RELIABLE_RETRY_INTERVAL 0) - an RFC 6298-style estimate, srtt + 4 *
// rttvar, over request-to-recovery times. INITIAL is used until a proxy has a first sample, so
// dynamic mode starts from exactly the fixed default and moves only on evidence.
//
// Both bounds are relative to the link since 2026-09-26 (the user's question: can the absolute
// constants be expressed relatively, the way RFC 6298's srtt + 4 * rttvar is?). They used to be 250us
// and 10ms, both calibrated to the rig's ~200us recoveries - and a fixed ceiling is already wrong on
// this project's own target link: at 10BASE-T1S speeds srtt is ~2ms, so 10ms sits at 5x srtt and
// clamps genuine recoveries, and a ceiling below the true recovery time turns backoff into a retry
// storm exactly when the link is worst. (examples/perf_hil/CONSTANTS_AUDIT.md has the whole sweep.)
//
// The interval is srtt + max(GRANULARITY, 4 * rttvar) - RFC 6298's own form, RTO = SRTT + max(G, K *
// RTTVAR). The G term is what the old floor was really for. On a steady link rttvar decays to zero and
// srtt + 4 * rttvar converges on srtt itself: a retry at the MEAN recovery time, while about half of
// all recoveries are still in flight. A floor of "1 * srtt" cannot help, because the interval is never
// below srtt anyway. G stays absolute on purpose, and for a stated reason, as it does in the RFC: it is
// how late this host actually runs a timer - a property of the host, not of the link. 100us is
// provisional until the rig measures the p99 lateness of a scheduled entry; it is not a link figure.
//
// The ceiling is MAX_SRTT_MULTIPLE * srtt. With the interval at srtt + 4 * rttvar, it binds only when
// rttvar reaches ~16x srtt - the pathological estimate the clamp exists for, not a healthy link. On the
// rig it lands at ~12.8ms, next to the ~12ms a KEEP_LAST Publisher there retains a sample for, which is
// what the old fixed 10ms was chosen against.
#ifndef tt_RELIABLE_RETRY_INITIAL
#define tt_RELIABLE_RETRY_INITIAL (1 * tt_MILLISECOND) // nanosecond
#endif
#ifndef tt_RELIABLE_RETRY_GRANULARITY
#define tt_RELIABLE_RETRY_GRANULARITY (100 * tt_MICROSECOND) // nanosecond - the host's timer lateness
#endif
#ifndef tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE
#define tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE 64 // the ceiling, in multiples of srtt
#endif
// How many of its latest ACKNACK requests a reliable Subscriber remembers per writer (2026-10-07,
// tt_WriterProxy.requests): what each named and when, so that an answer to the writer's own request leaves out
// repairs still on their way, and so that a repair's transit time can be measured from the request it answers. 16
// bytes each, in every writer proxy. Too few, and a request still in flight is forgotten and its repair asked for
// again: at 5% loss and ~850 Mbps a gap opens every ~0.3 ms and a repair queued behind the writer's data takes 1-2 ms.
// Measured on the PC (c6 shape, netem rate 1gbit + 5% loss, 2 reps): 8 sent 3061-3085 wire bytes a sample with 35-40k
// duplicate fragments, 32 sent 3007-3021 with 21-25k, the same as the build without the answer (3009-3019, 19-23k).
// 8 on FreeRTOS, where a writer proxy's 128 bytes is the cost that matters and the rates that overflow it are not
// reached; a shorter history only re-requests some repairs early, it never loses one.
#ifndef tt_RELIABLE_REQUEST_HISTORY
#if defined(TT_PLATFORM_FREERTOS)
#define tt_RELIABLE_REQUEST_HISTORY 8
#else
#define tt_RELIABLE_REQUEST_HISTORY 32
#endif
#endif
// Phase 3 (rmw_tickle/PLAN.md) - how often a Subscriber logs that a KEEP_ALL gap is still stuck.
// KEEP_ALL switches off the tt_RELIABLE_RETRY give-up, so without this a genuinely unrecoverable
// gap would retry silently forever; rate-limited by time, not retry count, so the cadence stays
// readable whatever tt_RELIABLE_RETRY_INTERVAL is.
#ifndef tt_RELIABLE_STUCK_WARN_INTERVAL
#define tt_RELIABLE_STUCK_WARN_INTERVAL (5 * tt_SECOND) // nanosecond
#endif
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
#ifndef tt_MAX_RELIABLE_HISTORY
#define tt_MAX_RELIABLE_HISTORY 64
#endif
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
    ((uint32_t)((((4u + 16u + (uint32_t)(payload_bytes)) + 3u) / 4u) * 4u)) // 4 = submessage header,
                                                                            // 16 = sizeof(struct tt_DataHeader)
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
// (COMPARISON.md §6 item 9/10's own re-measurement already ruled that out). See struct tt_
// WriterProxy.received_bitmap's own doc comment (tickle.h) for the honest "this raises the
// tolerable gap ~4x, not a guaranteed full fix - the real link's own ACKNACK RTT still sets the
// actual limit" caveat.
#ifndef tt_RELIABLE_BITMAP_BITS
#define tt_RELIABLE_BITMAP_BITS 256
#endif
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
#ifndef tt_RELIABLE_BITMAP_WORD_BITS
#define tt_RELIABLE_BITMAP_WORD_BITS 64
#endif
#define tt_RELIABLE_BITMAP_WORDS (tt_RELIABLE_BITMAP_BITS / tt_RELIABLE_BITMAP_WORD_BITS)
// Phase 2 (rmw_tickle/PLAN.md) - the widest tracking window a Subscriber may ask for, and the
// upper bound every decode path validates an incoming ACKNACK's own word count against (a
// malformed or hostile count must never index past a local buffer). tt_RELIABLE_BITMAP_BITS above
// stays the *default* window every Subscriber gets for free: TickLE core is embedded-first
// (PLAN.md's Project Goal 1), and 4096 bits is 512 bytes per tracked writer that a microcontroller
// nowhere near 190K msg/s would never use. A Linux-class caller (rmw_tickle, Goal 5, and the
// perf_hil examples via their own flag) opts into a wider one per Subscriber by handing
// tt_Context_create_subscriber()'s own caller-owned tracking buffer - see struct tt_Subscriber's own
// window doc comment (tickle.h).
#ifndef tt_RELIABLE_BITMAP_MAX_BITS
#define tt_RELIABLE_BITMAP_MAX_BITS 4096
#endif
#define tt_RELIABLE_BITMAP_MAX_WORDS (tt_RELIABLE_BITMAP_MAX_BITS / tt_RELIABLE_BITMAP_WORD_BITS)
// A Client's retries (struct tt_Service.call_retry_interval 0, the auto path) - the same RFC 6298 estimator as the
// reliable retry above, over call-to-answer times, with bounds relative to the link since 2026-10-05 (ROADMAP "Now"
// 5a, as 08e568af did for the reliable retry). They used to be a 5 ms floor and a 250 ms ceiling, both fitted to the
// rig: the floor held a 100 us link's retry 50 x past its round trip, and the ceiling retried a 400 ms link before
// any answer could arrive.
//
// The first wait of a call is srtt + max(GRANULARITY, 4 * rttvar), and each retry of the same call waits twice the
// one before (RFC 6298 5.5), each at most MAX_SRTT_MULTIPLE * srtt. The doubling is what the old floor was really
// for: a call gives up after (count + 1) waits, so with a constant interval one fast answer shrank the budget of
// every later call to a few round trips, and an answer slower than that timed out (CONTEXT_NODE_PLAN.md "Client
// retry fix", 2026-09-27: 85 us measured gave 0.5 ms). Doubling makes the budget (2^(count+1) - 1) first waits -
// 15 with the default count - so a retry can fire early without the call giving up early.
//
// INTERVAL is the seed: the first wait until a client has its first answer, standing in for srtt. GRANULARITY is
// twice the reliable retry's, because a call's round trip has two late-running events in it, not one: this host's
// retry timer and the server's dispatch of the request. Both are host properties, not link ones, and provisional
// as tt_RELIABLE_RETRY_GRANULARITY is.
//
// A timed-out call doubles srtt (Karn's / TCP's backoff), so a server that became slower than the budget is reached
// again; the next answer replaces the estimate outright.
//
// DEADLINE_PER_SEND bounds the call, not the estimate: an auto-path call reports failure no later than (count + 1) x
// it after it was made - 1 s by default, exactly the worst case the old fixed 250 ms ceiling gave, (count + 1) x 250
// ms. A wait that would run past it is cut short, and the call times out there. So how fast an application learns
// its server is gone does not change, and a measured srtt above 250 ms is still used as it is: a 400 ms link waits
// 400 ms for its answer, where the old ceiling resent at 250 ms. It is a policy bound - how long an application is
// kept waiting - not a link estimate; a service whose answers may take longer sets call_retry_interval (and count)
// explicitly, which is used as given with no deadline, as rmw_tickle does. The backoff stops growing srtt at the
// deadline, since no wait can be longer.
#ifndef tt_CALL_RETRY_INTERVAL
#define tt_CALL_RETRY_INTERVAL (5 * tt_MILLISECOND) // nanosecond - the seed, until a first answer
#endif
#ifndef tt_CALL_RETRY_COUNT
#define tt_CALL_RETRY_COUNT 3 // count
#endif
#ifndef tt_CALL_RETRY_GRANULARITY
#define tt_CALL_RETRY_GRANULARITY (2 * tt_RELIABLE_RETRY_GRANULARITY) // nanosecond - two hosts' event lateness
#endif
#ifndef tt_CALL_RETRY_MAX_SRTT_MULTIPLE
#define tt_CALL_RETRY_MAX_SRTT_MULTIPLE 64 // a wait's ceiling, in multiples of srtt
#endif
#ifndef tt_CALL_DEADLINE_PER_SEND
#define tt_CALL_DEADLINE_PER_SEND (250 * tt_MILLISECOND) // nanosecond - an auto call ends within (count + 1) x this
#endif
// How long a Server keeps an answered response for a retrying client (server_cache_lifetime(), tickle.c). It used to
// be a fixed 100 ms whose comment gave the formula - client latency x (count + 1) - that nothing computed, and which
// was already shorter than the 250 ms a client's retry interval could reach. The server cannot see the client's
// srtt (nothing on the wire carries it), so it keeps a response for the longest of what it can know: the client's
// retry schedule before any answer (the seed above, or this service's own explicit call_retry_interval x (count +
// 1)), and GAP_MULTIPLE x the longest recent gap between a response going out and the same client asking again. Each
// retry served re-arms the entry. 4 is the client's own doubling (the next gap is twice the last) times a margin of
// 2. A client slower than anything seen misses once, and its retry - matched against the expired entry, which still
// names it - teaches the server its gap. A response is never answered to a new incarnation of its client (a
// restarted process reusing the context id, seq_no back at 0): discovery's entity_id for the client, drawn per
// launch, is recorded with the response and compared, and a farewell announce drops the source's responses.
#ifndef tt_SERVER_CACHE_GAP_MULTIPLE
#define tt_SERVER_CACHE_GAP_MULTIPLE 4
#endif
// How long a tt_SERVER_CALLBACK that returned tt_CALL_DEFERRED has to eventually call
// tt_Server_send_response() before the slot reserved for it is reclaimed (Milestone 17,
// rmw_tickle/PLAN.md) - deliberately much longer than an answered response's cache lifetime above,
// which covers re-sending an *already-computed* answer to a retrying client, not waiting on the
// application to compute one in the first place. Matches rmw_tickle's own pre-existing
// RMW_TICKLE_SERVICE_RESPONSE_TIMEOUT_NS default (rmw_tickle.h) - not a coincidence, that value
// was standing in for this exact primitive not existing yet.
#ifndef tt_SERVER_DEFERRED_RESPONSE_TIMEOUT
#define tt_SERVER_DEFERRED_RESPONSE_TIMEOUT (5 * tt_SECOND)
#endif
// How many datagrams of one drain are stamped with the same reading of the clock (OPTIMIZATION_PLAN.md 11,
// D1). The receive path takes "now" - a peer's last sign of life (traffic_last_seen), a timestamp's rebuild -
// from the time the poll read when its wait returned, and reads it again every this many datagrams, instead
// of once per datagram: a clock read was ~27 ns of a ~85 ns received sample on the PC. A stamp is then at most
// this many datagrams' processing old, microseconds, against liveliness's milliseconds.
#ifndef tt_RX_CLOCK_REFRESH
#define tt_RX_CLOCK_REFRESH 16
#endif
// How many datagrams the HAL already holds (tt_rx_buffered()) core processes under one taking of the state lock
// (OPTIMIZATION_PLAN.md 11.4, D4): the lock costs ~13 ns a pair on the PC, and a thread publishing meanwhile
// waits for at most this many datagrams' processing. 1 is the behaviour before D4.
#ifndef tt_RX_LOCK_CHUNK
#define tt_RX_LOCK_CHUNK 8
#endif
// A positive poll slice some callers pass explicitly (rmw_tickle's poll thread), and the most back-to-
// back scheduler work a negative-timeout tt_Context_poll() runs before handing control back. It used to be
// what a negative timeout waited, too; since 2026-09-25 that waits for the scheduler instead - see
// tt_Context_poll() in tickle.h.
#ifndef tt_RECEIVE_TIMEOUT
#define tt_RECEIVE_TIMEOUT (100 * tt_MICROSECOND) // nanosecond
#endif
// EXPERIMENTAL (branch experiment/poll-loop-io-interleave, rmw_tickle/PLAN.md's own "Further
// latency research" section) - tt_Context_poll()'s own inner loop favors an already-due scheduler
// entry over ever calling tt_receive(), with no cap on how many may run consecutively before an
// I/O check happens. A continuously-rescheduling task (e.g. a max-rate Publisher's own send loop,
// interval_s=0) can then starve tt_receive() for a whole call's own tt_RECEIVE_TIMEOUT budget,
// meaning ACKNACK responsiveness ends up bounded by how rarely the scheduler queue goes idle, not
// by real network RTT. This bounds how many scheduler entries may run back-to-back before a
// forced, non-blocking tt_try_receive() peek is squeezed in between them - unvalidated on real HIL
// yet, this specific value (8) is a first guess, not yet tuned.
#ifndef tt_SCHEDULER_IO_INTERLEAVE
#define tt_SCHEDULER_IO_INTERLEAVE 8
#endif
// Whether TickLE core may be called from more than one thread (2026-09-25). 1: every public tt_*
// function is safe to call from any thread, concurrently with tt_Context_poll() on another - see
// "Threading" in tickle.h. 0: the original single-thread contract, and the locks compile to nothing,
// for a microcontroller build that only ever has one task touching the stack.
#ifndef tt_THREAD_SAFE
#define tt_THREAD_SAFE 1
#endif
// How many timers other threads may have in flight to a node at once before tt_Context_schedule() falls back
// from the lock-free inbox to taking the node's lock (struct tt_Context.sched_inbox). The poll thread empties
// it every time it looks at the scheduler, so it only has to cover a burst between two looks.
#ifndef tt_SCHED_INBOX_LENGTH
#define tt_SCHED_INBOX_LENGTH 32
#endif
// Requested SO_SNDBUF/SO_RCVBUF size. The kernel silently clamps this to whatever
// net.core.[rw]mem_max allows for an unprivileged process, so asking for more than that is
// harmless - it's cheap insurance against drops under bursty send/receive on systems where the
// ceiling is higher than the (often small, e.g. 208KB) distro default.
#ifndef tt_SOCKET_BUFFER_SIZE
#define tt_SOCKET_BUFFER_SIZE (1024 * 1024)
#endif

#ifndef tt_MAX_ENDPOINT_COUNT
#define tt_MAX_ENDPOINT_COUNT 256 // Local endpoints (data or services) one context holds; rmw_tickle builds set 2048
#endif
// Size of tt_Context.endpoint_index (power of two, >= 2 * tt_MAX_ENDPOINT_COUNT so load stays
// <= 0.5 for linear-probe lookups).
#ifndef tt_ENDPOINT_INDEX_SIZE
#define tt_ENDPOINT_INDEX_SIZE 512
#endif
#ifndef tt_MAX_NAME_LENGTH
#define tt_MAX_NAME_LENGTH 255 // Maximum length of endpoint name
#endif
#ifndef tt_MAX_STRING_LENGTH
#define tt_MAX_STRING_LENGTH 65535 // Maximum length of string
#endif
// RX/TX buffering size to flush: the largest UDP payload a standard 1500-byte Ethernet MTU
// can carry without IP fragmentation. 1500 (MTU) - 20 (IPv4 header) - 8 (UDP header) = 1472.
// Previously 1480, which is 8 bytes *larger* than that limit - a packet in the 1473-1480
// byte range would pass this check yet still fragment at the IP layer on a standard network.
#define tt_ETHERNET_UDP_PAYLOAD 1472  // the above, for a standard Ethernet MTU
#define tt_IPV4_UDP_MAX_PAYLOAD 65507 // 65535 - 20 (IPv4 header) - 8 (UDP header): one datagram, fragmented
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH tt_ETHERNET_UDP_PAYLOAD
#endif

// The largest datagram a node builds out of more than one submessage, and the size a discovery
// announce is split to (DESIGN.md's "Discovery announce") - kept at a standard Ethernet
// datagram even when tt_MAX_BUFFER_LENGTH is raised, as rmw_tickle does (up to 65507, left to the
// OS to fragment - the user's decision, 2026-09-24).
//
// tt_MAX_BUFFER_LENGTH bounds a single sample; this bounds everything a node sends that the other
// side did not ask to be large. A node on core defaults (tt_MAX_BUFFER_LENGTH 1472, an MCU say)
// can only receive 1472 bytes, and it has to be able to discover and be discovered by a node built
// with the larger buffer: a batch or an announce that grew to the larger size would be truncated
// on its side and silently lost. So submessages that share a datagram never take it past this,
// and only one submessage sent on its own - a sample that is large because its type is - may use
// the full tt_MAX_BUFFER_LENGTH.
//
// Chosen by the preprocessor rather than a ?: so the result is a plain constant: at the default
// the two candidates are the same value, which a conditional expression would carry into every
// use site as a branch with identical arms.
#ifndef tt_CONTROL_MAX_LENGTH
#if tt_MAX_BUFFER_LENGTH < tt_ETHERNET_UDP_PAYLOAD
#define tt_CONTROL_MAX_LENGTH tt_MAX_BUFFER_LENGTH
#else
#define tt_CONTROL_MAX_LENGTH tt_ETHERNET_UDP_PAYLOAD
#endif
#endif

// The largest sample - encoded CDR bytes, framing excluded - a Publisher may send and a Subscriber can
// receive (DATA_FRAG, rmw_tickle/DATAFRAG_PLAN.md). tt_MAX_BUFFER_LENGTH keeps meaning the largest
// datagram; a sample that does not fit a control datagram (tt_CONTROL_MAX_LENGTH) goes out as fragments of
// at most that size, each its own datagram, and is put back together on the receiving side
// (tt_SUBMESSAGE_TYPE_FRAG_FIRST, tickle.h).
//
// Fragmentation is compiled in exactly when a sample can be larger than a control datagram. At core's
// defaults the three are equal, which compiles it out altogether: no reassembly pool, no larger
// tx_buffer, and a sample that does not fit one datagram is refused exactly as before - an MCU that only
// sends small samples pays nothing for a feature it does not use. A build that raises
// tt_MAX_BUFFER_LENGTH (rmw_tickle, to 65507) gets it too: its samples then fragment at the control
// datagram, while service requests and responses, which do not fragment, keep the large datagram.
// -Dtt_FRAG_ENABLED=0 keeps such a build on the OS's IP fragmentation instead - the benchmark's ipfrag arm.
// (SHM_PLAN.md) The shared-memory transport. 1 everywhere it can be built, 0 on FreeRTOS, which has no shm_open
// and where the segment is a HAL-provided region rather than a file - stage 1 does not carry that form, so the
// module compiles out there entirely and a target that cannot use it pays nothing for it. Same shape as
// tt_CONTEXT_ID_CLAIM above, and for the same reason: a module the user can turn off has to actually leave when it
// is off, not merely be skipped at runtime.
#ifndef tt_SEGMENT_ENABLED
#if defined(TT_PLATFORM_FREERTOS)
#define tt_SEGMENT_ENABLED 0
#else
#define tt_SEGMENT_ENABLED 1
#endif
#endif

// SHM_PLAN.md stage 1: the shared-memory segment's own capacity, as a byte budget with the slot
// count derived - the idiom g13's reader queue already uses, because it is the bound that stays
// meaningful when the datagram size changes underneath it.
//
// A slot holds one datagram, not one sample, and it is sized by tt_CONTROL_MAX_LENGTH rather than
// by tt_MAX_BUFFER_LENGTH. Those differ where it matters: an rmw build raises the buffer to 65507
// while its topics still fragment at the control datagram, so sizing slots by the buffer would make
// a 64-slot ring 4.2 MB against 94 KB in a native build - most of the peak-RSS margin rmw_tickle
// wins on, spent on slack in slots that never fill.
//
// The cost of that choice, stated because it is a real scope limit and not an oversight: **service
// requests and responses do not fragment** (see tt_MAX_SAMPLE_LENGTH below and valid_msg_size() in
// tickle.c), so a service message larger than a slot goes over UDP. That is deliberate, it is
// counted rather than silent (struct tt_Context.segment_oversized_to_udp), and S2 expects `udp` for
// such a shape by design. The fix if measurement ever asks for it is variable-length records
// spanning contiguous slots, which is not stage 1: complexity added before a measurement asks for
// it cannot be attributed to anything.
// **Choose this from your own system, not from this default - README.md has the formula.** In short:
// the ring has to hold what the publisher produces while the subscriber is not running, so
//
//     slots >= publish rate (datagrams/s) x the subscriber's worst-case stall (s)
//     bytes  = slots x (16 + tt_SEGMENT_SLOT_BYTES)
//
// and if that stall cannot be characterised, `shm_full_dropped` on the node traffic line is the
// feedback signal: non-zero under your real load means the ring is smaller than your burst.
//
// **The slot count rounds DOWN to a power of two, so a careless value wastes up to half of it.** At
// 1472-byte slots the stride is 1488 bytes:
//
//     512 KiB -> 352 raw slots -> 256 used, 372 KiB mapped, 27% of the value unusable
//     768 KiB -> 528 raw slots -> 512 used, 744 KiB mapped,  3% unusable
//
// 768 KiB is the default for that reason (raised from 512 KiB on 2026-10-02): it doubles the slots
// and drops the waste from 27% to 3%, which is the only change to this value justified without a
// measurement. It is a per-context cost - a host running N contexts with a same-host peer each pays
// N times it - though a context with no same-host peer and no self-delivery creates no segment at
// all and pays nothing.
#ifndef tt_SEGMENT_BYTES
#define tt_SEGMENT_BYTES (768 * 1024)
#endif
// Every constant below is #ifndef-guarded, and that is not decoration. Until 2026-09-29 they were
// bare #defines, so -Dtt_SEGMENT_ATTACH_RETRY_SENDS=... was silently overridden by this header and
// changed nothing. Plan found it by trying to measure the retry's cost directly: the two arms came
// out byte-identical and the harness refused to report rather than telling us the retry was free.
// A tunable that cannot be tuned reads exactly like a tunable whose value does not matter.
#ifndef tt_SEGMENT_SLOT_BYTES
#define tt_SEGMENT_SLOT_BYTES tt_CONTROL_MAX_LENGTH
#endif
// SHM_PLAN 6e(b): a publish whose one destination is an attached same-host segment encodes the sample straight into
// the claimed slot instead of into tx_buffer and then copying it into the slot - one copy of the sample fewer. The
// slot receives the same bytes either way (tests/test_encode_in_slot.c). 0 keeps the two-copy path, which is what an
// A/B of the change builds as its control arm; it has no effect where tt_SEGMENT_ENABLED is 0.
#ifndef tt_SEGMENT_ENCODE_IN_SLOT
#define tt_SEGMENT_ENCODE_IN_SLOT 1
#endif
// Most records taken from a segment in one poll, so a writer that keeps its ring full cannot starve
// the socket - the poll returns and comes back, which is the fairness the socket drain already has.
#ifndef tt_SEGMENT_DRAIN_PER_POLL
#define tt_SEGMENT_DRAIN_PER_POLL (tt_SEGMENT_SLOTS * 4)
#endif
// The most epochs (one ring's worth of records each) a reader spends in the mode it measured cheaper before it
// measures the other again (segment_epoch_turn(), tickle.c), and the window of epochs - and of sleeps - its means are
// over. Not fitted to any measurement: it bounds what re-measuring costs - two epochs in the dearer mode (the one
// that settles and the one measured) per this many, so at most 2/514 of the difference between the two - against how
// long a change in that difference (the host moving a vCPU, a writer slowing) goes unnoticed: 512 epochs of 512
// records is 0.2 s at 1.3 M records/s. At 64 the re-measures alone cost the PC's ~260 ns pair 3% (2026-10-07).
#ifndef tt_SEGMENT_PROBE_EVERY_MAX
#define tt_SEGMENT_PROBE_EVERY_MAX 512
#endif

// Consecutive drain passes with the head of the ring claimed but never published, before the owner
// says so. A publish takes a memcpy, so a handful of passes over a claimed head is ordinary
// concurrency; a thousand is a writer that is not coming back. High enough that a busy segment
// never warns, low enough that a wedged one is reported in well under a second of polling.
// Whether a segment's doorbell is a FIFO beside it (1, the default) or only the zero-length UDP datagram it was
// before 2026-10-04 (0). The FIFO is what a peer uses when it finds one; a peer without one is rung over UDP either
// way, so this is a choice of footprint and of A/B arm, not of compatibility (tt_segment_bell_create(), hal.h).
#ifndef tt_SEGMENT_BELL_FIFO
#define tt_SEGMENT_BELL_FIFO 1
#endif
#ifndef tt_SEGMENT_STALL_PASSES
#define tt_SEGMENT_STALL_PASSES 1000
#endif

// Sends to a peer with no segment before asking /dev/shm about it again. The answer for a peer on
// another host never changes, so asking per datagram is pure cost - measured at roughly 87,000
// failed open() calls a second per sender, which halved cross-host throughput. It must not become
// permanent either: a peer that binds after we first sent to it, or that restarts, has to become
// attachable. 256 sends costs about 0.4% of the failed calls and bounds the delay at 256 datagrams,
// which on a link busy enough for the cost to matter is well under a millisecond.
#ifndef tt_SEGMENT_ATTACH_RETRY_SENDS
#define tt_SEGMENT_ATTACH_RETRY_SENDS 256
#endif

// Sends over an attached segment before the peer asks the name again. This is not paranoia about
// the mapping going bad: an owner killed between one datagram and the next leaves its region mapped
// and unchanged, incarnation included, so re-reading the header can never reveal it. Re-attaching
// by name can - the file is either gone or has been replaced by the successor's. Larger than the
// negative interval because a working segment is the common case and this costs an open() and a
// remap, not just an open().
#ifndef tt_SEGMENT_REVALIDATE_SENDS
#define tt_SEGMENT_REVALIDATE_SENDS 4096
#endif

// How long a reader may take nothing at all from its ring, while we have records for it, before the
// writer gives the segment up and reaches that peer over UDP until the next recheck.
//
// Time rather than a count of refusals, because a count measures the writer. At the rates the
// same-host cell runs at, a perfectly healthy reader goes thousands of our sends between two of its
// own poll passes, and a count-based rule abandoned it constantly - then re-attached, then abandoned
// again, putting one logical stream on two paths all by itself.
//
// The limit is one summary interval of liveliness silence: tt_LIVELINESS_SILENCE_NS is
// tt_LIVELINESS_MISS_THRESHOLD intervals and a half, and this is the silence of one of them. A reader
// whose node is alive sends a summary every interval from the same poll loop that drains its ring, so
// one whole interval without a take is far longer than any scheduling delay a live reader suffers; and
// it is shorter than the silence after which that node would be presumed dead (which drops the segment
// anyway, forget_same_host_peer()), so this rule decides first and is not dead code. The cost of being
// wrong is UDP until the next recheck. It was a literal 1 s until 2026-10-06 (ROADMAP.md 5a) - the same
// value at the default tt_CONTEXT_UPDATE_INTERVAL, but a build with an interval below ~286 ms put the
// liveliness silence under it, and the rule could no longer fire before the peer was presumed dead.
// Derived now, and the ordering is a static_assert at the end of this file.
#ifndef tt_SEGMENT_DEAD_READER_NS
#define tt_SEGMENT_DEAD_READER_NS (tt_LIVELINESS_SILENCE_NS * 2U / (((uint64_t)tt_LIVELINESS_MISS_THRESHOLD * 2U) + 1U))
#endif
// 16 bytes of per-slot header (struct tt_SegmentSlot: length, sender address, port, sequence) - spelled out
// rather than sizeof() because this has to be a preprocessor constant. A static_assert in tickle.h
// checks the two agree, since nothing else would notice them drifting apart.
#define tt_SEGMENT_SLOT_STRIDE (16 + tt_SEGMENT_SLOT_BYTES)
#define tt_SEGMENT_RAW_SLOTS (tt_SEGMENT_BYTES / tt_SEGMENT_SLOT_STRIDE)

// Rounded down to a power of two so the ring indexes with a mask, by an explicit ladder rather than
// preprocessor arithmetic - the value is worth being able to read off the page.
#if tt_SEGMENT_RAW_SLOTS >= 1024
#define tt_SEGMENT_SLOTS 1024
#elif tt_SEGMENT_RAW_SLOTS >= 512
#define tt_SEGMENT_SLOTS 512
#elif tt_SEGMENT_RAW_SLOTS >= 256
#define tt_SEGMENT_SLOTS 256
#elif tt_SEGMENT_RAW_SLOTS >= 128
#define tt_SEGMENT_SLOTS 128
#elif tt_SEGMENT_RAW_SLOTS >= 64
#define tt_SEGMENT_SLOTS 64
#elif tt_SEGMENT_RAW_SLOTS >= 32
#define tt_SEGMENT_SLOTS 32
#elif tt_SEGMENT_RAW_SLOTS >= 16
#define tt_SEGMENT_SLOTS 16
#elif tt_SEGMENT_RAW_SLOTS >= 8
#define tt_SEGMENT_SLOTS 8
#else
#define tt_SEGMENT_SLOTS 4
#endif

#ifndef tt_MAX_SAMPLE_LENGTH
#define tt_MAX_SAMPLE_LENGTH tt_MAX_BUFFER_LENGTH
#endif
#ifndef tt_FRAG_ENABLED
#if tt_MAX_SAMPLE_LENGTH > tt_CONTROL_MAX_LENGTH
#define tt_FRAG_ENABLED 1
#else
#define tt_FRAG_ENABLED 0
#endif
#endif
// Samples a node can be reassembling at once, from any mix of senders. When all are busy and a fragment
// of another sample arrives, the reassembly started longest ago is abandoned and counted
// (tt_Context.frag_abandoned) - never dropped silently, since a silent drop here looks exactly like loss.
// Each slot holds one whole sample (tt_MAX_SAMPLE_LENGTH plus a DataHeader); nothing is allocated.
#ifndef tt_FRAG_REASSEMBLY_SLOTS
#define tt_FRAG_REASSEMBLY_SLOTS 8
#endif
// Fragments one sample may be split into: the width of a reassembly slot's bitmap.
#define tt_FRAG_MAX_COUNT 64
// tx_buffer: room for a full batch plus one submessage overshooting it, which is what end_encode()'s
// deferral needs - and, with fragmentation, room for a whole sample behind a batch that is still pending,
// because the sample is encoded in one piece and only split as it is sent.
#if tt_FRAG_ENABLED
#define tt_TX_BUFFER_LENGTH (tt_MAX_BUFFER_LENGTH + tt_MAX_SAMPLE_LENGTH + 64)
#else
#define tt_TX_BUFFER_LENGTH (tt_MAX_BUFFER_LENGTH * 2)
#endif

// Node ID values are the last byte of the IPv4 address on the local network.
// Valid node IDs are 1..254, because 0 is reserved for invalid/unassigned and
// 255 is reserved for the broadcast address.
#define tt_CONTEXT_ID_INVALID 0x00
#define tt_CONTEXT_ID_BROADCAST 0xff
// (g8, rmw_tickle/RMW_GAPS_PLAN.md, 2026-09-28) Several contexts on one host, each with its own id. 0, the default: a
// context takes the id above - the address's last octet, or _tt_CONFIG.context_id - as it always has, so two
// processes on one address share it and drop each other's every packet as their own. 1, rmw_tickle's build:
// - a context claims its id in a host registry (the HAL's tt_claim_context_id()): the preferred id when no other
//   live process holds it, else a free one;
// - a packet is its own only when it comes from its own data socket (tt_is_own_address()) - one carrying its id from
//   anywhere else is a collision, which the newer of the two contexts resolves by moving to an id nobody on the link
//   uses (tickle.c, handle_id_collision());
// - an id set explicitly never moves.
// The wire is unchanged: a source is still the sending context's id, unique on the link.
//
// **On by default since 2026-09-29 (the user's decision): several processes on one host is what a Linux deployment
// normally is, so it should work without being asked for, and a build that does not want it turns it off.** It costs
// about 1 KB of text where it is compiled in, measured the way README.md's optimisation section describes.
//
// Off by default on FreeRTOS, and not because of size: that HAL has no host registry to claim an id in - there are no
// processes to tell apart - and `struct tt_hal` there has no `claimed_id`, so tt_CONTEXT_ID_CLAIM=1 does not compile
// (checked 2026-09-29). The conditionality is what the platform can supply, not a size preference. A HAL that grows the
// primitive can flip this with a -D.
#ifndef tt_CONTEXT_ID_CLAIM
#if defined(TT_PLATFORM_FREERTOS)
#define tt_CONTEXT_ID_CLAIM 0
#else
#define tt_CONTEXT_ID_CLAIM 1
#endif
#endif
// (g9, rmw_tickle/RMW_GAPS_PLAN.md, 2026-09-28) Delivery between endpoints of one context. 0, the default: a context
// drops its own DATA as self-sent, so its own Subscribers never receive its own Publishers' samples. 1, rmw_tickle's
// build, whose every node in a process shares one context: a publish also hands the sample to the context's own
// matching Subscribers, in-process (tickle.c, deliver_locally()), with the QoS rules a remote pair has, and a late
// durable Subscriber can take the local durable backlog (tt_Subscriber_deliver_local_backlog()).
#ifndef tt_LOCAL_DELIVERY
#define tt_LOCAL_DELIVERY 0
#endif
// (g6, rmw_tickle/RMW_GAPS_PLAN.md, 2026-09-28) How far a context may discover and be discovered - ROS 2's
// ROS_AUTOMATIC_DISCOVERY_RANGE and ROS_STATIC_PEERS. 0, the default: the configured links only, as always. 1,
// rmw_tickle's build: _tt_CONFIG.discovery_range limits it (tt_DISCOVERY_RANGE_* below), and a link marked `peer` is
// one remote address rather than an interface - announces and every broadcast-class datagram also go to it.
#ifndef tt_DISCOVERY_OPTIONS
#define tt_DISCOVERY_OPTIONS 0
#endif
#define tt_DISCOVERY_RANGE_SUBNET 0    // the links, as always
#define tt_DISCOVERY_RANGE_LOCALHOST 1 // only this host (loopback) and the peer links: other senders are dropped
#define tt_DISCOVERY_RANGE_OFF 2       // nothing sent to the link, nothing received processed
// Every context id the wire can name (a uint8_t): the size of struct tt_Context's per-peer tables, which are indexed
// by a remote context's id. Not a setting. They were sized by tt_MAX_ENDPOINT_COUNT, which was 256 only by
// coincidence and so could not grow (CONTEXT_NODE_PLAN.md 4a, 2026-09-27).
#define tt_MAX_CONTEXT_IDS (UINT8_MAX + 1)
#ifndef tt_MAX_SCHEDULER_LENGTH
#define tt_MAX_SCHEDULER_LENGTH 128 // Scheduling queue
#endif
// Nodes one context can host (CONTEXT_NODE_PLAN.md stage 2), its default node included when in use. At most 256: a
// node's index is a uint8_t, and stage 3 carries it in 8 spare bits of an announce entry. rmw_tickle builds set 256
// (its CMakeLists.txt) - a composed ROS 2 bringup puts 15-20 nodes in one container.
#ifndef tt_MAX_NODES
#define tt_MAX_NODES 16
#endif
#ifndef tt_MAX_SERVER_CACHE_COUNT
#define tt_MAX_SERVER_CACHE_COUNT 64 // >= # of client
#endif

// The storage a server and a client carry inline, per slot. Sized for any message by default, which
// is what core's own users - the examples, FreeRTOS - get without doing anything. A caller that
// knows its types can instead attach storage sized for them after creation
// (tt_Server_set_storage()/tt_Client_set_storage()) and define these small: rmw_tickle does, because
// at tt_MAX_BUFFER_LENGTH 65507 the inline defaults would make every tt_Server 12.6 MB (the storage
// design the user approved on 2026-09-24). Multiples of 8, so each slot can hold an aligned struct.
#ifndef tt_SERVER_CACHE_ENTRY_LENGTH
#define tt_SERVER_CACHE_ENTRY_LENGTH (tt_MAX_BUFFER_LENGTH * 2) // one cached, already-encoded response
#endif
// tt_SERVER_PENDING_ENTRY_LENGTH sized a deferred-response copy that is no longer kept (2026-09-27); setting it
// would do nothing, so it stops the build instead.
#ifdef tt_SERVER_PENDING_ENTRY_LENGTH
#error "tt_SERVER_PENDING_ENTRY_LENGTH was removed: deferred responses are no longer copied (CONTEXT_NODE_PLAN.md)"
#endif
#ifndef tt_CLIENT_CACHE_LENGTH
#define tt_CLIENT_CACHE_LENGTH (tt_MAX_BUFFER_LENGTH * 2) // the outstanding request, encoded
#endif

// Threshold for how many known recipient nodes a Publisher/Client sends to individually before
// switching to one broadcast instead. <= this many known peers -> unicast (tt_send_to() once per
// peer); more than this many -> broadcast (tt_send() once). Zero known peers (nobody has
// announced a matching endpoint yet) always broadcasts too, regardless of this threshold -
// there's nothing to unicast to yet, so it falls back to today's discovery-by-broadcast behavior.
// How many links (interfaces) one node can be configured to talk on. Fixed, because TickLE is
// malloc-free: a node's links are part of its static configuration, not something it discovers.
// Four is a guess sized for the cases in front of us - a wired test link plus a management
// network is two, and a gateway bridging two segments is three - not a measured limit.
// 255.255.255.255, as a number. Not a tunable - it is what the limited broadcast *is*, and a
// deployment that wants a different broadcast sets one per link rather than redefining this.
#define tt_LIMITED_BROADCAST 0xFFFFFFFFU

#ifndef tt_MAX_LINK_COUNT
#define tt_MAX_LINK_COUNT 4
#endif

#ifndef tt_UNICAST_PEER_THRESHOLD
#define tt_UNICAST_PEER_THRESHOLD 2
#endif

// Fixed capacity of each Publisher's/Client's peers[] table (struct tt_Peer, tickle.h) - the
// known set of remote nodes (IP:port) hosting a matching Subscriber/Server, learned from their
// periodic UPDATE announces. Must stay > tt_UNICAST_PEER_THRESHOLD: once full, a never-seen
// peer is silently dropped rather than tracked (see upsert_peer() in tickle.c) - safe only
// because a full table already implies "more than the threshold", i.e. already broadcasting,
// which still reaches that dropped peer too.
#ifndef tt_MAX_PEER_COUNT
#define tt_MAX_PEER_COUNT 8
#endif
// Phase 2 (rmw_tickle/PLAN.md) - how many remote Subscriber *entities* one Publisher tracks ack
// state for (struct tt_PeerAck, tickle.h). Deliberately its own constant rather than reusing
// tt_MAX_PEER_COUNT above, which counts remote *nodes*: one node can host several Subscriptions of
// the same topic, and each needs its own ack watermark for Phase 3's KEEP_ALL blocking to be
// correct. 12 bytes per entry.
#ifndef tt_MAX_ACK_ENTRIES
#define tt_MAX_ACK_ENTRIES 16
#endif

// Liveliness: a remote node is considered gone once this many *consecutive* tt_CONTEXT_UPDATE_
// INTERVAL windows pass with no UPDATE announce heard from it at all - not merely no *change*
// (see tt_Context's own update_last_seen[], tracked separately from update_last_modified[]/
// update_seen[], which only move when the announced content itself changes). A single announce
// lost to UDP packet loss is common and shouldn't immediately declare an otherwise-healthy node
// dead; too high a value delays noticing a real departure (a crash, a pulled cable - anything
// that skips tt_Context_destroy()'s own farewell UPDATE). 3 matches the conventional heartbeat-miss
// default other discovery protocols use for the same reason.
// How many summaries a node sends per lifetime of the shortest liveliness lease its own endpoints announce,
// when that makes them more frequent than tt_CONTEXT_UPDATE_INTERVAL (LIVELINESS_PLAN.md amendment 1) - an
// idle node's summary is its only sign of life. With n summaries lost in a row the peer hears nothing for
// (n + 1) / tt_LIVELINESS_LEASE_DIVISOR of a lease, so at six, four losses leave a sixth of the lease to
// spare and the fifth lands exactly on it - where two nodes' drifting schedulers decide it by a coin toss,
// the boundary tt_LIVELINESS_SILENCE_NS moved off. At 5% loss that is ~1e-4 false lapses per 120 s; five
// would put the coin toss at four losses (~2e-3), and three failed L3 once in three 120 s runs.
// tt_Publisher_assert_liveliness() allows an assertion this often too.
#ifndef tt_LIVELINESS_LEASE_DIVISOR
#define tt_LIVELINESS_LEASE_DIVISOR 6
#endif

// The longest a silent node is kept alive for the sake of a long liveliness lease one of its entities
// announced (LIVELINESS_PLAN.md rule 3). DDS bounds a writer's lease by its participant's the same way:
// CycloneDDS's participant lease is 10 s, Fast DDS's 20 s. Below tt_LIVELINESS_SILENCE_NS it has no effect.
#ifndef tt_CONTEXT_MAX_LEASE_NS
#define tt_CONTEXT_MAX_LEASE_NS (10 * tt_SECOND)
#endif

// A request for a peer's endpoint list (DISCOVERY_PLAN.md rule 3) that has not brought the list within the
// retry delay is sent again, up to tt_DISCOVERY_REQUEST_ATTEMPTS times in all; after that the peer's next
// summary starts over. A lost request or reply then costs a round trip, not the rest of a summary interval:
// M5 (5% loss, 2026-09-26) saw one node wait 2 s for a list, two losses in a row. The answer goes out at
// once, so the delay only has to cover a round trip and a flush tick, and since 2026-10-06 it is exactly
// that: the round trip measured to that peer (tt_Context.discovery_rtt_ns, smoothed, timed from each
// request's first send) plus tt_CONTEXT_TX_INTERVAL (discovery_retry_after(), tickle.c; ROADMAP.md 5a).
// tt_DISCOVERY_REQUEST_RETRY is only the seed, used for a peer no answer has been timed from yet - 10 ms,
// the value the delay had before, fitted to nothing but generous for a LAN. Requests to at most
// tt_DISCOVERY_PENDING_REQUESTS peers are tracked at a time; one more is still sent, just not retried.
#ifndef tt_DISCOVERY_REQUEST_RETRY
#define tt_DISCOVERY_REQUEST_RETRY (10 * tt_MILLISECOND)
#endif
#ifndef tt_DISCOVERY_REQUEST_ATTEMPTS
#define tt_DISCOVERY_REQUEST_ATTEMPTS 4
#endif
#ifndef tt_DISCOVERY_PENDING_REQUESTS
#define tt_DISCOVERY_PENDING_REQUESTS 8
#endif

// Liveliness must not be judged from a window this node spent descheduled. The check runs as a
// scheduler entry and poll_once_nonblocking() runs every due entry BEFORE reading the socket, so a
// process starved for longer than tt_LIVELINESS_SILENCE_NS wakes with its own clock far advanced and
// its peers' datagrams still queued - and would declare them dead without reading one of them. A peer
// whose datagram is in our buffer was not silent; we had not looked.
//
// Found on 2026-09-30 in CI (run 36787898559) on a commit that changed no code: on a loaded runner both
// nodes declared EACH OTHER dead 3.5 s apart while 100 samples were published, and delivery stopped at
// 5. Raising tt_LIVELINESS_SILENCE_NS does not address it - that only changes how much starvation is
// needed, and a shared runner can always supply more.
//
// So the check defers while anything is unread, and these two bound that deferral. The socket is
// drained by the very next receive pass, so one deferral is normally all it takes; the cap exists only
// so a permanently saturated socket cannot postpone detecting a real death for ever. At these values a
// genuine death is reported at most 4 ms late.
#ifndef tt_LIVELINESS_DEFER_NS
#define tt_LIVELINESS_DEFER_NS (1 * tt_MILLISECOND)
#endif
#ifndef tt_LIVELINESS_MAX_DEFERRALS
#define tt_LIVELINESS_MAX_DEFERRALS 4
#endif

#ifndef tt_LIVELINESS_MISS_THRESHOLD
#define tt_LIVELINESS_MISS_THRESHOLD 3
#endif

// How long a node must be silent to be presumed dead: tt_LIVELINESS_MISS_THRESHOLD intervals and half of
// one more. The half is what makes it mean "that many summaries missed" (DISCOVERY_PLAN.md M5, 2026-09-26).
// A whole number of intervals put the limit exactly where the next summary lands after one fewer loss, and
// every node's periodic tasks run a little late and reschedule from when they ran, so the two drift across
// each other: with two summaries lost in a row, whether the check or the third summary came first was a
// coin toss, and 5% loss on eight nodes produced false deaths within 40 s. Reproduced in
// test_peer_discovery.c (two lost summaries, schedulers 100/170 us late: 12 false deaths in 40 trials).
#define tt_LIVELINESS_SILENCE_NS \
    ((((uint64_t)tt_LIVELINESS_MISS_THRESHOLD * 2U) + 1U) * (uint64_t)tt_CONTEXT_UPDATE_INTERVAL / 2U)

// Fixed capacity of an opt-in struct tt_Discovery (tickle.h, tt_Context_set_discovery()) - the
// number of distinct remote entities (across every node it's ever heard an UPDATE from) it can
// track at once for graph introspection. Unrelated to tt_MAX_PEER_COUNT (that's a *local*
// endpoint's own known-unicast-destinations table; this is one shared cache of *every* remote
// entity a node has opted into recording, regardless of whether it matches anything local).
// A new entity past this limit is dropped, counted (tt_Discovery.entities_dropped) and warned about once
// (upsert_discovered_entity() in tickle.c). Correctness depends on it, not only introspection (CONTEXT_NODE_PLAN.md
// 4a): RxO checks on received DATA, a reliable subscriber's KEEP_ALL classification of a writer and per-entity
// liveliness leases read it, and for an entity not in it RxO fails open. Delivery between compatible endpoints and
// unicast peer selection do not. Size it to the remote entities a context will see. Each entry costs roughly 2 *
// (tt_MAX_NAME_LENGTH + 1) bytes (552 in all), so the default is small for FreeRTOS; rmw_tickle builds set 2048.
// Fragments one discovery announce may be split into, and so how many endpoints a context can announce: each fragment
// is one datagram of at most tt_CONTROL_MAX_LENGTH (1472 bytes on Ethernet), which holds ~15 ROS-sized endpoints, so 32
// announce ~480 (CONTEXT_NODE_PLAN.md 4a, 2026-09-27). At most 255: a fragment's index and count are uint8_t on the
// wire. rmw_tickle builds set 255, ~3800 endpoints, past its tt_MAX_ENDPOINT_COUNT of 2048. Each context tracks the
// fragments received from every peer in tt_UPDATE_PART_WORDS 32-bit words.
#ifndef tt_UPDATE_MAX_PARTS
#define tt_UPDATE_MAX_PARTS 32
#endif
#define tt_UPDATE_PART_WORDS ((tt_UPDATE_MAX_PARTS + 31) / 32)

#ifndef tt_MAX_DISCOVERED_ENTITIES
#define tt_MAX_DISCOVERED_ENTITIES 16
#endif

// The discovery table's index (CONTEXT_NODE_PLAN.md 4b): open addressing over (context id, endpoint id). Compiled only
// for a table large enough to need it (tt_DISCOVERY_INDEXED): at core's default 16 a scan costs no more than a hash
// (8.9/4.2 ns against 6.8/5.6 per lookup), so that build keeps the scan and its code as it was; rmw_tickle's 2048 gets
// the index (a full scan was 1.2 us per received sample). The index is a power of two at least twice the table.
#define tt_DISCOVERY_INDEXED (tt_MAX_DISCOVERED_ENTITIES > 64)
#if tt_DISCOVERY_INDEXED
#ifndef tt_DISCOVERY_INDEX_SIZE
#if tt_MAX_DISCOVERED_ENTITIES <= 256
#define tt_DISCOVERY_INDEX_SIZE 512
#elif tt_MAX_DISCOVERED_ENTITIES <= 1024
#define tt_DISCOVERY_INDEX_SIZE 2048
#elif tt_MAX_DISCOVERED_ENTITIES <= 2048
#define tt_DISCOVERY_INDEX_SIZE 4096
#elif tt_MAX_DISCOVERED_ENTITIES <= 8192
#define tt_DISCOVERY_INDEX_SIZE 16384
#else
#define tt_DISCOVERY_INDEX_SIZE 65536
#endif
#endif
#endif

#ifndef _tt_CONTEXT_ADDRESS
#define _tt_CONTEXT_ADDRESS "0.0.0.0"
#endif
#ifndef _tt_CONTEXT_PORT
#define _tt_CONTEXT_PORT 8282
#endif
#ifndef _tt_CONTEXT_BROADCAST
// The destination every announce and every broadcast-mode send is addressed to.
//
// This default reaches further than it looks, and on 2026-09-23 that put benchmark traffic onto a
// shared lab network for a day. 255.255.255.255 is the *limited* broadcast: it has no subnet to be
// scoped by, so the kernel sends it out whatever the default route points at. On the HIL rig that
// is the management Wi-Fi, not the wired test link. A *directed* broadcast - 192.168.10.255 for a
// node on 192.168.10.0/24 - is scoped by the routing table with no socket binding involved
// (`ip route get 192.168.10.255` naming eth0 is what run_perf.sh has always relied on to find the
// interface to apply tc to), which is why the perf_hil harnesses, which set one, never leaked.
//
// So: set this to the directed broadcast of the link you mean. Leaving it at the default does not
// mean "this machine's network", it means "wherever this machine's default route goes", and those
// are the same thing only by accident.
#define _tt_CONTEXT_BROADCAST "255.255.255.255"
#endif

struct _tt_Config {
    // The address the data socket binds to. 0.0.0.0 (the default) means "any local address",
    // which is almost always what is wanted; setting a specific local address scopes this node's
    // *sends* to the link that owns it, which is the unprivileged way to pin a limited broadcast
    // to one interface (SO_BINDTODEVICE would be the obvious tool and needs CAP_NET_RAW).
    //
    // It binds the data socket only, never the well-known one, and that is not an implementation
    // detail to tidy up later: measured on Linux, a socket bound to a unicast address receives no
    // broadcasts at all, directed or limited. Binding the well-known socket to a specific address
    // would therefore stop discovery dead while leaving unicast working - a node that hears
    // nobody and is heard by nobody, with every send succeeding. See tt_bind().
    char* addr;
    int port;
    char* broadcast;
    // tt_CONTEXT_ID_INVALID (0, the default) = auto-detect via tt_get_node_id() (the last byte of
    // the local address matching broadcast's subnet, per the comment above); any other value
    // overrides it. Auto-detection needs each node to have its own distinct address in that
    // subnet, which real separate hosts (or namespaces) give for free but a single shared network
    // namespace can't - two processes on the same host/interface would otherwise both detect the
    // same id and start silently dropping each other's packets as "self sent" (see
    // process_packet() in tickle.c). An explicit override sidesteps that: e.g.
    // platform/linux/test.sh runs both sides of a pair in one namespace over loopback, each
    // started with a different id, without needing root for network namespaces at all.
    int32_t context_id;

    // One link this node talks on: where its broadcasts go, which local address it sends them
    // from, and how many peers on *this* link it will unicast to before switching to one
    // broadcast. Per link rather than per node because the answer genuinely differs by medium -
    // five subscribers on a 10Base-T1S segment and one on Ethernet want opposite decisions, and a
    // single count across both loses on whichever it is not sized for.
    //
    // `resolved_*` are filled in at tt_bind() from the operating system, not from the strings:
    // matching a peer to a link needs the link's netmask, which a broadcast address does not
    // carry (x.y.z.255 implies /24 only by convention). The OS already knows every interface's
    // address, netmask and broadcast together, so the link is resolved against the interface
    // whose broadcast is the configured one. A link therefore has to correspond to a real local
    // interface - which is what "per interface" means.
    struct _tt_Link {
        char* broadcast;
        char* addr; // NULL or "0.0.0.0" = any local address
        // Deliberately still 2, and deliberately still a guess. Making the threshold per-link
        // does not make its value better-founded than the single global one was - see
        // tt_UNICAST_PEER_THRESHOLD's own comment. It is now a guess per link.
        uint8_t unicast_threshold;

        uint32_t resolved_addr;      // host byte order, from the OS at bind time
        uint32_t resolved_netmask;   // host byte order
        uint32_t resolved_broadcast; // host byte order
        bool resolved;
        // The resolved interface's MTU (tt_link_mtu(), hal.h), -1 when unknown, and whether it is
        // below the 1500 bytes tt_ETHERNET_UDP_PAYLOAD assumes. Recorded rather than only logged, so
        // a caller - or a test - can see it.
        int32_t resolved_mtu;
        bool mtu_below_assumed;
#if tt_DISCOVERY_OPTIONS
        // (g6) One remote peer, not an interface: `broadcast` is its IPv4 address (a unicast one, or a subnet's
        // directed broadcast), reached on the well-known port. Resolved as a /32 without asking the OS.
        bool peer;
#endif
    } links[tt_MAX_LINK_COUNT];
    // 0 means "no links configured explicitly": tt_bind() then synthesises exactly one from the
    // addr/broadcast/tt_UNICAST_PEER_THRESHOLD fields above, so every existing caller - and every
    // single-link deployment - keeps working without knowing links[] exists at all. The
    // single-link case is the degenerate one, not a special case.
    uint8_t link_count;
#if tt_DISCOVERY_OPTIONS
    // (g6) tt_DISCOVERY_RANGE_*: SUBNET (0, the default) is the links as always.
    uint8_t discovery_range;
#endif
    // Bytes of payload one shared-memory slot holds, or 0 (the default) for tt_SEGMENT_SLOT_BYTES.
    //
    // Runtime rather than only -D, for three reasons measured on 2026-10-02. The hot path never used the
    // constant - segment_slot() takes its stride and its mask from the header - so moving it costs nothing
    // where it would be felt. One binary can then serve deployments whose messages differ. And the value
    // appears in the header a peer reads, so a results file can say which geometry produced it rather than
    // leaving it to be inferred from a build.
    //
    // **Zero is not a size, it is the absence of a choice**, and the two are answered differently. A user who
    // set this and then has a type that does not fit gets an error from tt_Context_create_publisher(): they
    // chose a value that does not match their data, their intended behaviour does not happen, and that is
    // correctness. A user on the default gets the segment_oversized_to_udp counter and one log line per
    // topic, because the sample is still delivered over UDP and that is performance. Reporting both the same
    // way would make the alarm unreadable in the case where it matters.
    uint32_t segment_slot_bytes;
};

extern struct _tt_Config _tt_CONFIG;

// Invariants between the settings above.
//
// static_assert, not _Static_assert: this is a public header and rmw_tickle includes it from C++.
// _Static_assert is C-only, so the first version of these broke every C++ translation unit that
// reached this file - "expected constructor, destructor, or type conversion" at each one - and
// took CI's rmw_tickle compile_commands step red for eight commits before anyone looked. C11 gives
// static_assert as a macro in <assert.h>, C++11 as a keyword, so this spelling works in both. These hold for the
// defaults; they are asserted because the defaults are now overridable and an override that breaks one would otherwise
// fail at runtime, as a silent misbehaviour, far from the line that caused it.
static_assert(tt_MAX_PEER_COUNT > tt_UNICAST_PEER_THRESHOLD,
              "tt_MAX_PEER_COUNT must exceed tt_UNICAST_PEER_THRESHOLD - see tt_MAX_PEER_COUNT's own "
              "comment: a full peer table is only safe because it already implies broadcasting");
static_assert(tt_RELIABLE_BITMAP_BITS % tt_RELIABLE_BITMAP_WORD_BITS == 0,
              "tt_RELIABLE_BITMAP_BITS must be a whole number of words");
static_assert(tt_RELIABLE_BITMAP_MAX_BITS % tt_RELIABLE_BITMAP_WORD_BITS == 0,
              "tt_RELIABLE_BITMAP_MAX_BITS must be a whole number of words");
static_assert(tt_RELIABLE_REQUEST_HISTORY >= 1 && tt_RELIABLE_REQUEST_HISTORY <= 255,
              "a Subscriber remembers at least one request, and its ring index is a uint8_t");
static_assert(tt_RELIABLE_BITMAP_MAX_BITS >= tt_RELIABLE_BITMAP_BITS,
              "tt_RELIABLE_BITMAP_MAX_BITS is the ceiling for tt_RELIABLE_BITMAP_BITS");
static_assert(tt_ENDPOINT_INDEX_SIZE >= tt_MAX_ENDPOINT_COUNT, "the endpoint index must have room for every endpoint");
// Structural, not a performance depth: a ring of fewer than four slots cannot usefully separate a
// writer from a reader at all. The depth stage 1 actually needs is a measured criterion (about one
// poll period of output - roughly 25 datagrams at the 4.1 us a datagram S2 measured), and asserting
// that here would break the `ipfrag` diagnostic arm, which legitimately overrides
// tt_CONTROL_MAX_LENGTH upwards and must still build.
static_assert(tt_SEGMENT_RAW_SLOTS >= 4, "tt_SEGMENT_BYTES is too small to hold four datagram slots");
static_assert(tt_SEGMENT_DEAD_READER_NS < tt_LIVELINESS_SILENCE_NS,
              "a stopped same-host reader must be given up before its node is presumed dead - see "
              "tt_SEGMENT_DEAD_READER_NS");
static_assert(tt_MAX_BUFFER_LENGTH <= tt_IPV4_UDP_MAX_PAYLOAD,
              "tt_MAX_BUFFER_LENGTH above 65507 cannot be one IPv4 UDP datagram, and "
              "the protocol's uint16 lengths could not describe it either");
static_assert(tt_CONTROL_MAX_LENGTH <= tt_MAX_BUFFER_LENGTH,
              "tt_CONTROL_MAX_LENGTH cannot exceed tt_MAX_BUFFER_LENGTH - nothing larger could be received");
static_assert(tt_MAX_SAMPLE_LENGTH >= tt_MAX_BUFFER_LENGTH,
              "tt_MAX_SAMPLE_LENGTH below tt_MAX_BUFFER_LENGTH would refuse samples one datagram can carry");
// SubmessageHeader + DataHeader (24) + the sample + up to 3 bytes of padding, in a uint16 length: 65507,
// the largest UDP payload, just fits.
#define tt_FRAG_SUBMESSAGE_OVERHEAD 27
static_assert(!tt_FRAG_ENABLED || tt_MAX_SAMPLE_LENGTH + tt_FRAG_SUBMESSAGE_OVERHEAD <= UINT16_MAX,
              "a fragmented sample is encoded as one submessage first, whose length is a uint16");
static_assert(tt_FRAG_REASSEMBLY_SLOTS >= 1, "fragmentation needs at least one reassembly slot");
static_assert((tt_ENDPOINT_INDEX_SIZE & (tt_ENDPOINT_INDEX_SIZE - 1)) == 0,
              "tt_ENDPOINT_INDEX_SIZE must be a power of two - for_each_endpoint() masks with it");
static_assert(tt_MAX_CONTEXT_IDS % 32 == 0, "reached_nodes[] packs the per-peer bits 32 to a word");
static_assert(tt_CALL_DEADLINE_PER_SEND >= tt_CALL_RETRY_INTERVAL,
              "the seed's first wait fits inside the deadline of a call");
static_assert(tt_CALL_RETRY_MAX_SRTT_MULTIPLE >= 1, "a wait's ceiling is at least srtt");
static_assert(tt_SERVER_CACHE_GAP_MULTIPLE >= 1, "a response is kept at least one observed retry gap");
static_assert(tt_MAX_NODES >= 1 && tt_MAX_NODES <= (UINT8_MAX + 1),
              "a node's index is a uint8_t, 8 bits on the wire (stage 3)");
static_assert(tt_UPDATE_MAX_PARTS >= 1 && tt_UPDATE_MAX_PARTS <= UINT8_MAX,
              "an announce fragment's count is a uint8_t");
#if tt_DISCOVERY_INDEXED
static_assert((tt_DISCOVERY_INDEX_SIZE & (tt_DISCOVERY_INDEX_SIZE - 1)) == 0 &&
                  tt_DISCOVERY_INDEX_SIZE >= 2 * tt_MAX_DISCOVERED_ENTITIES && tt_MAX_DISCOVERED_ENTITIES < UINT16_MAX,
              "the discovery index is a power of two, at least twice the table, of uint16_t slot numbers");
#endif
