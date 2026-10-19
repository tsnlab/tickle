/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (docs/RESULTS.md rows 43-44) - TickLE core
// native subscriber role, built to the CycloneDDS/FastDDS twins' shape: match, then stall consumption for `-p` seconds
// (default 3.0) while the writer keeps publishing, then take. The reader cache is KEEP_LAST HISTORY_DEPTH (8, as the
// twins' readers): a sample arriving while it is full evicts the oldest, so a deeper cache keeps more of the stall.
//
// Until 2026-10-20 this side slept `-p` BEFORE creating its Subscriber. A VOLATILE late joiner is given nothing written
// before it joined, whatever the depth, so the "taken of 160" it printed was the samples published after the join: 139
// of 160 with the writer at depth 8 and 139 at depth 64 on the PC (history_depth_pc_check.sh, DEPTH_BLIND) - the
// lifespan_expiry defect (b393bf90) in this scenario.
//
// Core delivers on arrival and keeps no queue, so the callback is the reader cache, as rmw_tickle's KEEP_LAST queue:
// main() keeps polling through the stall (the reader keeps receiving, as a DDS reader cache does while nothing takes)
// and takes from the cache only once the stall is over. The stall starts at the first arrival, the first moment this
// side can see the match: the client publishes only once it has matched a reader (client.c), as the DDS writers do.
// The freshness flags (--fresh, --fresh-rt, BenchFreshness.h / BenchFreshRt.h) stamp the TAKE, as the twins' do at
// dds_take(), not the arrival.
//
// last_seq starts at 0, not the first-taken sample's own seq (2026-09-20, real measurement bug - already found and
// fixed once on the CycloneDDS twin, see its own doc comment): treating the first take as the new baseline would hide
// exactly the leading-gap loss this scenario exists to observe.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "Bench.h"
#include "BenchCreate.h"
#include "BenchFreshRt.h"
#include "BenchFreshness.h"

// Reorder-buffer geometry for this example's RELIABLE Subscriber - see where it is assigned.
//
// Sized to the widest tracking window this build allows, not to a round number. A Subscriber can
// hold at most as many samples as it can track ahead of its oldest gap, so a buffer that covers
// the widest window makes overflow impossible by construction rather than unlikely - the same
// argument rmw_tickle's RMW_TICKLE_REORDER_SLOTS rests on. This was 512 at first, which is
// narrower than the -w a reliable_throughput run is allowed to request (up to 4096), so any run
// that asked for a wide window would have overflowed into the re-request fallback and measured
// that instead of the protocol.
// 4096: tt_RELIABLE_BITMAP_MAX_BITS until large-message stage 2 raised it to 8192 (docs/DESIGN.md section 8), pinned
// so these cells stay the experiment they were - the same ring, warmed the same way.
#define BENCH_REORDER_SLOTS 4096
// The slot stride is core's since the QoS redesign: tt_Subscriber_storage_size() sizes it from BenchTopic's
// max_encoded_size, a datagram's payload at most (docs/DESIGN.md section 9).

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint64_t received = 0;
static uint64_t lost = 0;
static uint32_t last_seq = 0;
// KEEP_LAST freshness (BenchFreshness.h), with --fresh: the age of each sample when this side takes it.
static struct bench_fresh_receiver g_fresh;
static char g_fresh_fields[BENCH_FRESH_FIELDS_MAX];
// Freshness by round trip (BenchFreshRt.h): --fresh-rt HOST:PORT echoes every take to the sender.
static struct bench_rt_reader g_rt;
static char g_rt_fields[BENCH_RT_FIELDS_MAX];

// The reader cache, KEEP_LAST HISTORY_DEPTH: a ring of the newest samples not yet taken, oldest at cache_head. 8, as
// the twins' readers and the writer (client.c); history_depth_pc_check.sh builds a copy at 64.
#define HISTORY_DEPTH 8
static struct {
    uint32_t seq;
    uint8_t payload[sizeof(((struct BenchData*)0)->payload)];
} cache[HISTORY_DEPTH];
static size_t cache_head = 0;
static size_t cache_count = 0;
// Samples the cache evicted unread (a newer one arrived while it was full) and the most it ever held.
static uint64_t evicted = 0;
static size_t cache_peak = 0;
static uint64_t first_arrival_ns = 0;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    if (first_arrival_ns == 0) {
        first_arrival_ns = tt_get_ns();
    }
    if (cache_count == HISTORY_DEPTH) {
        cache_head = (cache_head + 1) % HISTORY_DEPTH;
        cache_count--;
        evicted++;
    }
    size_t slot = (cache_head + cache_count) % HISTORY_DEPTH;
    cache[slot].seq = data->seq;
    memcpy(cache[slot].payload, data->payload, sizeof cache[slot].payload);
    cache_count++;
    if (cache_count > cache_peak) {
        cache_peak = cache_count;
    }
}

// Takes every cached sample, oldest first, as one dds_take() of the twins: gap-counted, stamped and echoed at the take.
static void take_all(void) {
    if (cache_count == 0) {
        return;
    }
    int64_t take_wall = g_fresh.on ? bench_fresh_wall_ns() : 0;
    while (cache_count > 0) {
        const uint32_t seq = cache[cache_head].seq;
        if (seq > last_seq + 1) {
            lost += (seq - last_seq - 1);
        }
        last_seq = seq;
        received++;
        bench_fresh_take(&g_fresh, seq, cache[cache_head].payload, sizeof cache[cache_head].payload, take_wall);
        bench_rt_echo(&g_rt, seq, cache[cache_head].payload, sizeof cache[cache_head].payload);
        cache_head = (cache_head + 1) % HISTORY_DEPTH;
        cache_count--;
    }
}

// Polls until SIGINT or the safety cap: the cache fills from the first arrival for pause_s, then every poll's arrivals
// are taken. Returns whether the stall ended (false: no sample ever arrived).
static bool receive_stall_take(struct tt_Context* node, double pause_s, double safety_cap_s) {
    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    uint64_t pause_ns = (uint64_t)(pause_s * (double)tt_SECOND);
    // 500ms at most (nanoseconds), so the deadline/g_interrupted check re-runs; shorter while the stall is ending.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    bool stalling = true;
    tt_ret_t ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        int64_t timeout_ns = poll_timeout_ns;
        if (stalling && first_arrival_ns != 0) {
            uint64_t now = tt_get_ns();
            uint64_t stall_end = first_arrival_ns + pause_ns;
            if (now >= stall_end) {
                stalling = false;
                printf("Subscriber: stalled %.3fs after the first arrival, now taking (%lu evicted meanwhile)\n",
                       pause_s, (unsigned long)evicted);
            } else if ((int64_t)(stall_end - now) < timeout_ns) {
                timeout_ns = (int64_t)(stall_end - now);
            }
        }
        if (!stalling) {
            take_all();
        }
        ret = tt_Context_poll(node, timeout_ns);
    }
    if (!stalling) {
        take_all();
    }
    return !stalling;
}

static const double default_pause_s = 3.0;
static const double default_safety_cap_s = 30.0;
static const double safety_cap_buffer_s = 15.0;

int main(int argc, char** argv) {
    double pause_s = default_pause_s;
    double safety_cap_s = default_safety_cap_s;
    bench_fresh_receiver_init(&g_fresh);
    for (int i = 1; i < argc; i++) {
        if (bench_fresh_receiver_parse_arg(&g_fresh, argv[i]) ||
            bench_rt_reader_parse_arg(&g_rt, &g_fresh, argc, argv, &i)) {
            continue;
        }
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pause_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }
    // +15s buffer - see deadline_miss_detection/server.c's own doc comment for the real bug this
    // avoids (run_scenario.sh forwards the same -d to both sides, but it means "this side's own
    // send duration" on the client vs. "don't hang forever" here - taken verbatim, this side could
    // exit mid-stream, before the client - which starts several seconds later - had even finished).
    // Not directly hit this pass (no -d was passed testing this scenario), fixed preemptively.
    safety_cap_s += safety_cap_buffer_s;

    // real HIL link's own broadcast address - see best_effort_latency/server.c's own doc comment
    // for the real bug this avoids.
    _tt_CONFIG.broadcast = "192.168.10.255";
    // Before the match and the stall: the side channel's control (PING/PONG) runs through the whole run.
    bench_rt_reader_start(&g_rt);

    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Context node;
    tt_ret_t ret = tt_Context_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    struct tt_Subscriber sub;
    // RELIABLE and VOLATILE, the builtin tracking window, BENCH_REORDER_SLOTS reorder slots (below), no DEADLINE or
    // LIVELINESS lease. KEEP_ALL keeps keep_last_depth 0, never skip (BenchCreate.h).
    //
    // Ordered delivery (2026-09-24) - somewhere to hold a sample that arrives ahead of a gap.
    //
    // Not optional for a benchmark. A RELIABLE Subscriber with no reorder buffer is still correct
    // - it declines to record an out-of-order sample as received, so the ACKNACK exchange fetches
    // it again once the gap has filled - but under real tc loss that turns one lost datagram into
    // a re-request for everything behind it. On the HIL rig that more than halved reliable receive
    // throughput and tripped the regression gate, which is how this was found.
    //
    // Sized to hold a full tracking window of samples: the window is exactly how far ahead of its
    // oldest missing sample this Subscriber is allowed to get, so it is also the most it can ever
    // need to hold at once.
    const struct tt_QosProfile sub_qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_ALL},
        .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .tracking_words = 0, .reorder_slots = BENCH_REORDER_SLOTS},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
    ret = BenchCreate_subscriber(&node, &sub, "stream", &sub_qos, (tt_SUBSCRIBER_CALLBACK)stream_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    bool stalled = receive_stall_take(&node, pause_s, safety_cap_s);

    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;

    bench_rt_reader_finish(&g_rt);
    // recv counts the samples taken, lost the seqs never taken (evicted included); evicted is the part the KEEP_LAST
    // cache dropped. stalled=0 means no sample ever arrived: the run is void.
    printf("RESULT: framework=tickle scenario=history_depth_burst_loss role=server pause_s=%.1f "
           "recv=%lu lost=%lu loss_pct=%.6f depth=%d evicted=%lu cache_peak=%lu stalled=%d%s%s\n",
           pause_s, (unsigned long)received, (unsigned long)lost, loss_pct, HISTORY_DEPTH, (unsigned long)evicted,
           (unsigned long)cache_peak, stalled ? 1 : 0,
           bench_fresh_receiver_fields(&g_fresh, g_fresh_fields, sizeof g_fresh_fields),
           bench_rt_reader_fields(&g_rt, g_rt_fields, sizeof g_rt_fields));

    tt_Context_destroy(&node);
    return 0;
}
