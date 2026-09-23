/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/COMPARISON.MD) -
// TickLE core native client/sender role (no rmw). Same one-way max-rate stream as
// best_effort_throughput's own client.c, plus RELIABLE + a caller-configurable HISTORY depth
// (rmw_tickle/PLAN.md's own "DDS semantic-parity backlog" row 2 - struct tt_ReliableCache's
// entries[]/capacity are caller-owned now, tickle.h, no longer a single build-wide
// tt_MAX_RELIABLE_HISTORY=64 ceiling every Publisher was capped by alike) - defaults to that same
// 64 for continuity with earlier measurements, -K raises it up to MAX_RELIABLE_DEPTH below without
// a rebuild, for directly re-measuring whether a deeper Publisher-side cache actually improves
// RELIABLE's own tc-loss recovery at TickLE's own real max throughput, or - per struct tt_
// ReliableCache's own doc comment's honest answer - the Subscriber-side received_bitmap window
// (widened 64 -> 256 bits, rmw_tickle/PLAN.md's "TickLE-native performance" plan, once real HIL
// pointed at it directly) is the real bottleneck regardless of how deep this side's own cache
// reaches back - a deeper Publisher-side cache alone still can't help past whatever that window
// currently is. No
// blocking "wait for all acks" API exists in tickle.h (only tt_Publisher_request_ack(), which
// solicits but doesn't block) - a fixed drain period after the send loop substitutes for that,
// polling so any in-flight retransmits can still land before teardown.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "../common/Bench.h"
#include "../common/reliable_stats_print.h"

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static const double default_duration_s = 10.0;
static const double default_drain_s = 3.0;
// Phase 3 step 4 - how long to wait for a Subscriber to actually match before publishing, and the
// interval at which that's rechecked. This used to be a blind 2-second sleep, which is what the
// residual loss under KEEP_ALL turned out to be: at 50% injected loss the missing sequence numbers
// were always 1 and 2, never the tail and never mid-stream. Samples published before any Subscriber
// matched, whose loss nothing can report - a WriterProxy is claimed on the first DATA that arrives,
// so a Subscriber that never saw seq 1 has no way to know it existed, and every counter on both
// sides is legitimately zero while the harness counts the gap as transport loss.
//
// That is also ordinary DDS semantics rather than a TickLE shortfall: a VOLATILE writer owes
// nothing to a reader that had not yet matched. The DDS harnesses this scenario is compared against
// structurally cannot make the error - cyclonedds/reliable_throughput/client.c calls
// wait_for_writer_match(..., 10.0) and bails if no reader appears - so the fixed sleep here was the
// asymmetry, not the result.
static const double match_wait_cap_s = 10.0; // matches the DDS harnesses' own match timeout
static const double match_poll_s = 0.01;
static const double bits_per_byte = 8.0;
static const double bits_per_megabit = 1e6;

// Compile-time backing-array ceiling for this scenario's own -K flag below - deliberately far
// past the old tt_MAX_RELIABLE_HISTORY=64 default, since re-measuring at a genuinely deeper depth
// is this flag's entire purpose (struct tt_ReliableCache's own doc comment, tickle.h).
#define MAX_RELIABLE_DEPTH 8192

static double interval_s = 0.0; // 0 = as fast as possible, matching best_effort_throughput's own default
static double duration_s = default_duration_s;
static double drain_s = default_drain_s;
static uint32_t reliable_depth = tt_MAX_RELIABLE_HISTORY; // -K overrides; 0 stays the historical default
static bool depth_explicit = false; // ...and whether -K actually said so - see -Q's own default below
static uint64_t sent = 0;
static uint32_t seq = 0;
static struct tt_Publisher* g_pub;
static uint64_t g_deadline_ns;
static bool g_sending_done = false;

// Self-throttle (-T <lag>, 0 = disabled/default) - rmw_tickle/PLAN.md's own "self-throttle"
// pattern for a RELIABLE Publisher that wants to actually honor RELIABLE's own delivery
// guarantee, not just maximize raw throughput (the user's own explicit direction: apply this
// specifically for RELIABLE, where DDS's own philosophy is correctness over rate - a BEST_EFFORT
// Publisher has no such guarantee to protect and shouldn't pay this cost). Uses only existing,
// already-public fields (tt_Publisher.peer_ack_seq_no[]/.peers[], tickle.h's own "caller-owned,
// plain field access" convention) - no core change needed at all.
static uint32_t throttle_lag = 0;
static const double throttle_retry_s = 0.00005; // 50us - short enough to resume promptly once a
                                                // peer catches up, without spinning the CPU raw

// Proactive ACK solicitation period (-A <period_us>, 0 = disabled). rmw_tickle/PLAN.md's own real
// HIL finding: without this, peer_ack_seq_no[] only ever advances via a loss-reactive ACKNACK
// paced at tt_CALL_RETRY_INTERVAL=5ms (config.h) - at this scenario's ~180-190K msg/s max send
// rate that's 900+ new sequence numbers per retry window, so any -T threshold in the low hundreds
// is blown past within a fraction of one window and the Publisher livelocks in the throttle
// branch. tt_Publisher_set_ack_solicit_period() (tickle.c) closes that gap by having the core
// periodically call tt_Publisher_request_ack() on this Publisher's own behalf, keeping
// peer_ack_seq_no[] fresh even on a healthy (gap-free) stream. When -T is given without an
// explicit -A, default_ack_solicit_us below is used instead of leaving it disabled, since a
// throttle with no proactive solicitation is the exact configuration already shown broken.
static uint32_t ack_solicit_us = 0;
// Phase 3 prerequisite (d) (rmw_tickle/PLAN.md) - -W <pct> sets tt_Publisher.ack_solicit_watermark_
// pct, i.e. solicit an ACK once this percent of the retained cache is unacknowledged. 0 (the
// default, matching core's own) leaves it off, so a plain run measures exactly what Phase 1 did.
static uint32_t ack_watermark_pct = 0;

// Phase 3 step 4 (rmw_tickle/PLAN.md) - -Q turns on HISTORY.KEEP_ALL, i.e. core refuses a write
// (tt_RET_WOULD_BLOCK) rather than evicting a sample no matched Subscriber has acknowledged.
// Default off, so every number measured before this flag existed is still reproducible by running
// the same command line.
static bool keep_all = false;

// -B <ms>: how long to keep retrying a refused write before counting it as failed, emulating DDS's
// own RELIABILITY max_blocking_time. Default 100ms to match FastDDS's own default, which is what
// examples/perf_hil/fastdds/reliable_throughput/client.cpp measures against - the whole point of
// the flag is that the three harnesses' write_fail columns mean the same thing.
static double max_blocking_ms = 100.0;

// Writes core refused for longer than -B. The sequence number is still consumed, exactly as in the
// DDS harnesses ("seq is still consumed, so the server counts each one as lost too; write_fail lets
// the two be told apart" - cyclonedds/reliable_throughput/client.c), so the comparable figure is
// net loss = lost - write_fail: samples the transport lost, as opposed to ones the application was
// told up front were never sent.
static uint64_t write_fail = 0;

// The sample currently being retried, if any. Held across send_one() invocations so a refused write
// retries the *same* sequence number rather than skipping ahead - a skip would be indistinguishable
// from transport loss at the Subscriber, which is exactly the distinction this scenario measures.
static struct BenchData pending_msg;
static bool have_pending = false;
static uint64_t pending_since_ns = 0;

// How long to wait before retrying a refused write. Matches throttle_retry_s below - the governing
// constraint is the same one: this client is single-threaded and driven entirely by the scheduler
// inside tt_Node_poll(), so a retry must go back through tt_Node_schedule() rather than loop in
// place. Spinning here would starve the poll that receives the very ACKNACKs the retry is waiting
// for, and the write would then never be accepted no matter how large -B is.
static const double keep_all_retry_s = 0.00005; // 50us

// Drain bookkeeping - see drain_tick(). The poll interval matches tt_RELIABLE_RETRY_INTERVAL: there
// is no point asking again faster than the recovery it is waiting on can answer.
static const double drain_poll_s = 0.001;
static uint64_t g_drain_deadline_ns = 0;
static bool g_drain_fully_acked = false;
static uint32_t g_peer_acks_min = UINT32_MAX; // low-water mark of matched Subscriber entities
// Defined next to drain_tick() below, where the reasoning for watching this lives; used by
// send_one() above it as well, so declared here.
static void sample_peer_acks(void);

// Depth -Q uses when -K doesn't say otherwise: twice the 1024-sample window the Phase 3 HIL matrix
// announces, so the Subscriber's window is what bounds blocking rather than this cache. Costs
// nothing extra - the backing arrays below are statically sized for MAX_RELIABLE_DEPTH regardless.
static const uint32_t keep_all_default_depth = 2048;
static const uint32_t default_ack_solicit_us = 200; // well under the time to send throttle_lag
                                                    // messages at max rate for every -T value
                                                    // this scenario tests (64-200)

// How far ahead of the slowest currently-matched peer this Publisher has run. 0 while nothing is
// confirmed yet - tt_Publisher_min_acked_seq_no() returns 0 both when no peer is matched and
// before the first ACKNACK has arrived, which must not read as an enormous lag at startup
// (Phase 3 prerequisite (c) moved this bookkeeping behind that accessor; it used to index
// pub->peer_ack_seq_no[] directly, which no longer exists).
static uint32_t reliable_lag(const struct tt_Publisher* pub) {
    uint32_t min_ack = tt_Publisher_min_acked_seq_no(pub);
    if (min_ack == 0) {
        return 0;
    }
    return pub->seq_no - (min_ack - 1);
}

static uint64_t max_blocking_ns_value(void) {
    return (uint64_t)(max_blocking_ms * (double)tt_MILLISECOND);
}

static void send_one(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (g_interrupted || tt_get_ns() >= g_deadline_ns) {
        g_sending_done = true;
        return;
    }
    if (throttle_lag > 0 && reliable_lag(g_pub) >= throttle_lag) {
        // The slowest-acking peer has fallen too far behind - pause a short beat rather than
        // publish another sample on top of an already-open gap, so this Publisher doesn't keep
        // burying that gap deeper (PLAN.md's own "Follow-up v2/v3" scheduler research - the
        // mechanism this throttle protects against is real, not hypothetical).
        tt_Node_schedule(node, time + (uint64_t)(throttle_retry_s * (double)tt_SECOND), send_one, NULL);
        return;
    }
    // A refused write (-Q only) leaves the sample pending and comes back to this same one; a fresh
    // sequence number is taken only once the previous sample has been either accepted or given up
    // on. send_ns is stamped once, at creation, so a retried sample reports the latency the
    // application actually experienced rather than the one the last attempt did - the same thing
    // dds_write() reports when it blocks internally.
    sample_peer_acks();
    if (!have_pending) {
        pending_msg.seq = ++seq;
        pending_msg.send_ns = tt_get_ns();
        have_pending = true;
        pending_since_ns = pending_msg.send_ns;
    }

    tt_ret_t ret = tt_Publisher_publish(g_pub, (struct tt_Data*)&pending_msg);
    if (ret == tt_RET_OK) {
        sent++;
        have_pending = false;
    } else if (ret == tt_RET_WOULD_BLOCK && tt_get_ns() - pending_since_ns < max_blocking_ns_value()) {
        // Still inside the budget: come back to this same sample through the scheduler, which is
        // what lets tt_Node_poll() run (and ACKNACKs arrive) between attempts. See
        // keep_all_retry_s' own comment for why this can't be a loop.
        tt_Node_schedule(node, time + (uint64_t)(keep_all_retry_s * (double)tt_SECOND), send_one, NULL);
        return;
    } else if (ret == tt_RET_WOULD_BLOCK) {
        // Budget expired. Drop the sample and move on, counting it - the Subscriber will see this
        // sequence number missing and count it lost, and write_fail is what lets the two be
        // separated afterwards.
        write_fail++;
        have_pending = false;
    } else {
        have_pending = false; // a real failure, not back-pressure - nothing to retry
    }

    uint64_t next = interval_s > 0.0 ? time + (uint64_t)(interval_s * (double)tt_SECOND) : time;
    tt_Node_schedule(node, next, send_one, NULL);
}

// Phase 3 step 4 - the drain is what decides whether a sample counts as delivered, so a blind
// fixed-duration one measures the wrong thing: the Publisher exits on a timer regardless of whether
// its peers ever confirmed the tail of the stream, and anything still in recovery at that instant
// is reported as transport loss. At 50% injected loss that showed up as a residual 0-3 samples per
// run with every abandonment counter at zero on both sides - not data anyone dropped, just data the
// run stopped waiting for. The tail is the exposed part: a Subscriber notices a gap when a *higher*
// seq_no arrives, and after the last publish no higher one ever does, so nothing reveals a lost
// final sample unless the Publisher asks.
//
// So the drain now asks, and ends when every matched peer has confirmed the last accepted sample -
// tt_Publisher_is_acked_by_all_peers(), which is what a DDS writer's own wait_for_acknowledgments()
// does at teardown. drain_s becomes the cap rather than the plan. This file's own doc comment used
// to say no such API existed; it does now.
// Phase 3 step 4 diagnostic - how many matched Subscriber entities this Publisher currently has
// acknowledgement state for. Worth watching because both tt_Publisher_is_acked_by_all_peers() and
// core's own keep_all_writable() treat "no matched peers" as vacuously satisfied - correctly, since
// there is then nobody left to wait for - which means losing the peer entry silently converts both
// "everyone has confirmed the stream" and "KEEP_ALL is holding this writer back" into no-ops. Under
// sustained injected loss a Subscriber's own periodic announce can be lost often enough to matter,
// so this records the value at the end and the low-water mark across the run.
static uint32_t count_peer_acks(const struct tt_Publisher* pub) {
    uint32_t live = 0;
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub->peer_acks[i].node_id != tt_NODE_ID_INVALID) {
            live++;
        }
    }
    return live;
}

static void sample_peer_acks(void) {
    uint32_t live = count_peer_acks(g_pub);
    if (live < g_peer_acks_min) {
        g_peer_acks_min = live;
    }
}

static void drain_tick(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    sample_peer_acks();
    if (g_pub->seq_no > 0 && tt_Publisher_is_acked_by_all_peers(g_pub, g_pub->seq_no)) {
        g_drain_fully_acked = true;
        g_interrupted = 1; // ends main()'s own drain poll loop, same as stop_draining() below
        return;
    }
    if (tt_get_ns() >= g_drain_deadline_ns) {
        g_interrupted = 1;
        return;
    }
    // Nothing else solicits here: publishing has stopped, so neither the KEEP_ALL watermark nor a
    // refusal can fire, and a healthy Subscriber sends no ACKNACK unprompted.
    (void)tt_Publisher_request_ack(g_pub);
    tt_Node_schedule(node, time + (uint64_t)(drain_poll_s * (double)tt_SECOND), drain_tick, NULL);
}

static void stop_draining(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

// Phase 3 step 4 - waits for a Subscriber to actually match before publishing, rather than the
// fixed sleep this used to do. Returns false (having explained itself on stderr) if none appears
// within match_wait_cap_s, which is the same failure cyclonedds/reliable_throughput/client.c takes
// via wait_for_writer_match(): better no numbers at all than numbers from a run that was publishing
// into the void.
//
// Polls through tt_Node_poll() rather than sleeping, since matching happens by processing the
// Subscriber's own announce, which only arrives while the node is being polled. Note this narrows
// the pre-match window but cannot close it: a peer_acks entry proves only that *we* heard the
// Subscriber, not that it is tracking us - see the server's own first_seq_seen comment for what
// that costs and how it is now reported.
//
// Split out of main() to keep its cognitive complexity under the project's clang-tidy threshold.
static bool wait_for_matched_subscriber(struct tt_Node* node, struct tt_Publisher* pub) {
    uint64_t match_deadline = tt_get_ns() + (uint64_t)(match_wait_cap_s * (double)tt_SECOND);
    tt_ret_t ret = tt_RET_OK;
    while (count_peer_acks(pub) == 0 && tt_get_ns() < match_deadline && !g_interrupted &&
           (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(node, (int64_t)(match_poll_s * (double)tt_SECOND));
    }
    if (count_peer_acks(pub) == 0) {
        fprintf(stderr, "timed out waiting for a matched subscriber after %.1fs\n", match_wait_cap_s);
        return false;
    }
    return true;
}

// Split out of main() to keep its own cognitive complexity under the project's clang-tidy
// threshold - this branch chain was the tipping point once -A joined -i/-d/-K/-T.
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-K") == 0 && i + 1 < argc) {
            reliable_depth = (uint32_t)strtoul(argv[++i], NULL, 10);
            depth_explicit = true;
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            throttle_lag = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            ack_solicit_us = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
            ack_watermark_pct = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-Q") == 0) {
            keep_all = true;
        } else if (strcmp(argv[i], "-B") == 0 && i + 1 < argc) {
            max_blocking_ms = atof(argv[++i]);
        }
    }
    if (throttle_lag > 0 && ack_solicit_us == 0) {
        ack_solicit_us = default_ack_solicit_us;
    }
}

int main(int argc, char** argv) {
    parse_args(argc, argv);
    // -Q without an explicit -K: the historical default of 64 is far below any window this scenario
    // announces (-w 1024 in the Phase 3 matrix), and KEEP_ALL blocks at min(depth, window), so
    // leaving it at 64 would measure a 64-deep cache rather than KEEP_ALL. -K still wins if given,
    // including deliberately pairing a shallow depth with a wide window.
    if (keep_all && !depth_explicit) {
        reliable_depth = keep_all_default_depth;
    }
    if (reliable_depth == 0 || reliable_depth > MAX_RELIABLE_DEPTH) {
        printf("Requested reliable cache depth %u out of range (1..%u); clamping to %u.\n", reliable_depth,
               MAX_RELIABLE_DEPTH, (unsigned)tt_MAX_RELIABLE_HISTORY);
        reliable_depth = tt_MAX_RELIABLE_HISTORY;
    }

    // real HIL link's own broadcast address - see best_effort_latency/client.c's own doc comment
    // for the real bug this avoids.
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    struct tt_Publisher pub;
    ret = tt_Node_create_publisher(&node, &pub, &BenchTopic, "stream");
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }
    // scenario "reliable_throughput" - RELIABLE + a caller-chosen HISTORY depth (-K, default 64 -
    // see this file's own doc comment above). entries[]/capacity are this file's own backing
    // array now, not an embedded tt_MAX_RELIABLE_HISTORY-sized one (struct tt_ReliableCache's own
    // doc comment, tickle.h) - sized to MAX_RELIABLE_DEPTH so -K can actually reach past 64.
    // B1 (rmw_tickle/PLAN.md) - index slots plus a byte arena, both sized for MAX_RELIABLE_DEPTH
    // samples of this scenario's own fixed BenchData shape, so -K still reaches 8192 - at ~820KB
    // of static storage now instead of ~12MB.
    static struct tt_ReliableCacheIndex pub_cache_index[MAX_RELIABLE_DEPTH];
    static uint8_t pub_cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(MAX_RELIABLE_DEPTH,
                                                                 tt_RELIABLE_RECORD_BYTES(sizeof(struct BenchData)))];
    static struct tt_ReliableCache pub_cache = {0};
    pub_cache.index = pub_cache_index;
    pub_cache.capacity = (uint16_t)reliable_depth;
    pub_cache.depth = (uint16_t)reliable_depth;
    pub_cache.arena = pub_cache_arena;
    // Only the part of the arena this run's own -K depth can use, so a smaller -K really is a
    // smaller retention window in bytes too, not just in slots.
    pub_cache.arena_size =
        tt_RELIABLE_CACHE_ARENA_BYTES(reliable_depth, tt_RELIABLE_RECORD_BYTES(sizeof(struct BenchData)));
    pub.reliable_cache = &pub_cache;
    pub.reliable = true;
    // Phase 3 step 4 - KEEP_ALL's back-pressure. Blocking is bounded by min(cache depth, the
    // narrowest window any matched Subscriber announced), so a depth below that window would make
    // this side the binding limit and quietly measure something narrower than the run asked for -
    // hence keep_all_default_depth above.
    pub.keep_all = keep_all;
    // Phase 3 prerequisite (d) - off unless -W asked for it, so the default run is byte-for-byte
    // the Phase 1 experiment.
    if (ack_watermark_pct > 0) {
        const uint32_t max_pct = 100;
        pub.ack_solicit_watermark_pct = (uint8_t)(ack_watermark_pct > max_pct ? max_pct : ack_watermark_pct);
    }
    g_pub = &pub;

    if (ack_solicit_us > 0) {
        ret = tt_Publisher_set_ack_solicit_period(&pub, (uint64_t)ack_solicit_us * tt_MICROSECOND);
        if (ret != tt_RET_OK) {
            printf("Cannot arm ACK solicitation: %d\n", ret);
            return ret;
        }
    }

    if (!wait_for_matched_subscriber(&node, &pub)) {
        tt_Node_destroy(&node);
        return 1;
    }

    uint64_t send_start = tt_get_ns();
    g_deadline_ns = send_start + (uint64_t)(duration_s * (double)tt_SECOND);
    tt_Node_schedule(&node, send_start, send_one, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && !g_sending_done && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }
    // Drain until every matched peer has confirmed the last accepted sample, or drain_s elapses -
    // see drain_tick()'s own comment for why a fixed-duration drain measured the wrong thing.
    g_drain_deadline_ns = tt_get_ns() + (uint64_t)(drain_s * (double)tt_SECOND);
    tt_Node_schedule(&node, tt_get_ns(), drain_tick, NULL);
    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    double mbps = duration_s > 0.0
                      ? ((double)sent * sizeof(struct BenchData) * bits_per_byte) / bits_per_megabit / duration_s
                      : 0.0;
    // write_fail= and max_blocking_ms= are spelled exactly as the cyclonedds/fastdds harnesses
    // spell them, so one parser reads all three frameworks' RESULT lines (Phase 3 step 4).
    printf("RESULT: framework=tickle scenario=reliable_throughput role=client sent=%lu write_fail=%lu "
           "elapsed_s=%.3f send_mbps=%.3f max_blocking_ms=%.3f keep_all=%d reliable_depth=%u "
           "throttle_lag=%u ack_solicit_us=%u ack_watermark_pct=%u drained=%s peer_acks_end=%u "
           "peer_acks_min=%u\n",
           (unsigned long)sent, (unsigned long)write_fail, duration_s, mbps, max_blocking_ms, keep_all ? 1 : 0,
           reliable_depth, throttle_lag, ack_solicit_us, ack_watermark_pct, g_drain_fully_acked ? "acked" : "timeout",
           count_peer_acks(&pub), g_peer_acks_min == UINT32_MAX ? 0 : g_peer_acks_min);
    print_reliable_stats("client");

    tt_Node_destroy(&node);
    return 0;
}
