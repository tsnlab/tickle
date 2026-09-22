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
static const double discovery_margin_s = 2.0;
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
static const uint32_t default_ack_solicit_us = 200; // well under the time to send throttle_lag
                                                    // messages at max rate for every -T value
                                                    // this scenario tests (64-200)

// Largest "how far ahead of its own last confirmed ack" gap across every currently-matched peer
// that has sent at least one real ACKNACK so far (peer_ack_seq_no == 0 means "nothing confirmed
// yet", not "confirmed everything up to 0" - skipped, not treated as an enormous lag at startup
// before the first ACKNACK has even had a chance to arrive).
static uint32_t reliable_lag(const struct tt_Publisher* pub) {
    uint32_t max_lag = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (pub->peers[i].node_id == tt_NODE_ID_INVALID || pub->peer_ack_seq_no[i] == 0) {
            continue;
        }
        uint32_t lag = pub->seq_no - pub->peer_ack_seq_no[i];
        if (lag > max_lag) {
            max_lag = lag;
        }
    }
    return max_lag;
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
    struct BenchData msg = {.seq = ++seq, .send_ns = tt_get_ns()};
    tt_ret_t ret = tt_Publisher_publish(g_pub, (struct tt_Data*)&msg);
    if (ret == tt_RET_OK) {
        sent++;
    }
    uint64_t next = interval_s > 0.0 ? time + (uint64_t)(interval_s * (double)tt_SECOND) : time;
    tt_Node_schedule(node, next, send_one, NULL);
}

static void stop_draining(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
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
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            throttle_lag = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            ack_solicit_us = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
    }
    if (throttle_lag > 0 && ack_solicit_us == 0) {
        ack_solicit_us = default_ack_solicit_us;
    }
}

int main(int argc, char** argv) {
    parse_args(argc, argv);
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
    static struct tt_ReliableCacheEntry pub_cache_entries[MAX_RELIABLE_DEPTH];
    static struct tt_ReliableCache pub_cache = {0};
    pub_cache.entries = pub_cache_entries;
    pub_cache.capacity = (uint16_t)reliable_depth;
    pub_cache.depth = (uint16_t)reliable_depth;
    pub.reliable_cache = &pub_cache;
    pub.reliable = true;
    g_pub = &pub;

    if (ack_solicit_us > 0) {
        ret = tt_Publisher_set_ack_solicit_period(&pub, (uint64_t)ack_solicit_us * tt_MICROSECOND);
        if (ret != tt_RET_OK) {
            printf("Cannot arm ACK solicitation: %d\n", ret);
            return ret;
        }
    }

    uint64_t send_start = tt_get_ns() + (uint64_t)(discovery_margin_s * (double)tt_SECOND);
    g_deadline_ns = send_start + (uint64_t)(duration_s * (double)tt_SECOND);
    tt_Node_schedule(&node, send_start, send_one, NULL);

    ret = tt_RET_OK;
    while (!g_interrupted && !g_sending_done && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }
    // drain period - no blocking "wait for acks" API exists (this file's own doc comment above),
    // just keep polling so any in-flight retransmit can still land before teardown.
    tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(drain_s * (double)tt_SECOND), stop_draining, NULL);
    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    double mbps = duration_s > 0.0
                      ? ((double)sent * sizeof(struct BenchData) * bits_per_byte) / bits_per_megabit / duration_s
                      : 0.0;
    printf("RESULT: framework=tickle scenario=reliable_throughput role=client sent=%lu elapsed_s=%.3f "
           "send_mbps=%.3f reliable_depth=%u throttle_lag=%u ack_solicit_us=%u\n",
           (unsigned long)sent, duration_s, mbps, reliable_depth, throttle_lag, ack_solicit_us);
    print_reliable_stats("client");

    tt_Node_destroy(&node);
    return 0;
}
