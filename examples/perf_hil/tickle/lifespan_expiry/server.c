/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "lifespan_expiry" (docs/RESULTS.md row 47) - TickLE core native
// subscriber role, built to the CycloneDDS/FastDDS twins' shape: match, then stall consumption for `-p` seconds, then
// take, and a sample older than its LIFESPAN when it is taken is never delivered.
//
// Until 2026-10-20 this side slept `-p` BEFORE creating its Subscriber. A VOLATILE late joiner is given nothing written
// before it joined, whatever the lifespan, so the "lost" it printed was the samples published before the join: the
// same count at -T 0.1 and at -T 1000, a scenario that could not fail (lifespan_pc_check.sh shows both). The
// "bimodal LIFESPAN pause" and "pauses differ per arm" residuals of COMPARISON to-do 6 and 8 were that join time.
//
// Core delivers on arrival and keeps no queue; LIFESPAN at the reader is the caller's to apply, from the delivering
// writer's lifespan (tt_Subscriber_delivering_lifespan()) and the sample's source stamp, as rmw_tickle does. So the
// callback is the reader cache: it queues each sample with the date it expires, judging its age once on arrival as
// rmw_tickle's lifespan_expiry() does; main() keeps polling (the reader keeps receiving, as a DDS reader cache does
// while nothing takes) and takes from the queue only once the stall is over, dropping what has expired. The stall
// starts at the first arrival, the first moment this side can see the match: the client publishes only once it has
// matched a reader (client.c), as the DDS writers do. The client's generous depth keeps the loss LIFESPAN's alone.

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
static uint64_t expired = 0;
static uint32_t last_seq = 0;

// The reader cache: a sample's seq and the tt_get_ns() from which it is expired (0: never). Larger than any count
// this scenario sends (-n 250); a run that overflows it says so on its RESULT line and is void.
#define LIFESPAN_QUEUE_SLOTS 4096
static struct {
    uint32_t seq;
    uint64_t expires_ns;
} queue[LIFESPAN_QUEUE_SLOTS];
static size_t queue_count = 0;
static uint64_t queue_overflow = 0;
static uint64_t first_arrival_ns = 0;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)seq_no;
    uint64_t now = tt_get_ns();
    if (first_arrival_ns == 0) {
        first_arrival_ns = now;
    }
    if (queue_count == LIFESPAN_QUEUE_SLOTS) {
        queue_overflow++;
        return;
    }
    // The age is judged once, on arrival, against the writer's stamp (the realtime clock, as the wire's), and from
    // there runs on the monotonic clock; a stamp ahead of this host's date is age 0.
    uint64_t lifespan = tt_Subscriber_delivering_lifespan(sub);
    uint64_t expires_ns = 0;
    if (lifespan != 0) {
        uint64_t wall = tt_get_wall_ns();
        uint64_t age = wall > timestamp ? wall - timestamp : 0;
        expires_ns = now + (age < lifespan ? lifespan - age : 0);
    }
    queue[queue_count].seq = data->seq;
    queue[queue_count].expires_ns = expires_ns;
    queue_count++;
}

// Takes every queued sample, oldest first: one past its expiry is dropped as if it had never been sent, the rest are
// delivered and gap-counted. last_seq starts at 0, so a sample never delivered counts as lost wherever it falls.
static void take_all(void) {
    uint64_t now = tt_get_ns();
    for (size_t i = 0; i < queue_count; i++) {
        if (queue[i].expires_ns != 0 && now >= queue[i].expires_ns) {
            expired++;
            continue;
        }
        if (queue[i].seq > last_seq + 1) {
            lost += (queue[i].seq - last_seq - 1);
        }
        last_seq = queue[i].seq;
        received++;
    }
    queue_count = 0;
}

static const double default_pause_s = 0.3;
static const double default_lifespan_s = 0.1;
static const double default_safety_cap_s = 30.0;
static const double safety_cap_buffer_s = 15.0;

int main(int argc, char** argv) {
    double pause_s = default_pause_s;
    double lifespan_s = default_lifespan_s;
    double safety_cap_s = default_safety_cap_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pause_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            lifespan_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }
    // +15s buffer - see deadline_miss_detection/server.c's own doc comment for the real bug this
    // avoids (run_scenario.sh forwards the same -d to both sides, but it means "this side's own
    // send duration" on the client vs. "don't hang forever" here - taken verbatim, this side could
    // exit mid-stream, before the client - which starts several seconds later - had even finished).
    safety_cap_s += safety_cap_buffer_s;

    // real HIL link's own broadcast address - see best_effort_latency/server.c's own doc comment
    // for the real bug this avoids.
    _tt_CONFIG.broadcast = "192.168.10.255";

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
        // The reader's own LIFESPAN, -T as the writer's, as the CycloneDDS/FastDDS readers set theirs: a writer's
        // announced lifespan reaches a Subscriber only through a discovery table (tt_Subscriber_delivering_lifespan()),
        // and this bench context keeps none, so without it nothing would expire here (the first PC run lost 0).
        .lifespan = {.duration_ns = (uint64_t)(lifespan_s * (double)tt_SECOND)},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
    ret = BenchCreate_subscriber(&node, &sub, "stream", &sub_qos, (tt_SUBSCRIBER_CALLBACK)stream_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    uint64_t pause_ns = (uint64_t)(pause_s * (double)tt_SECOND);
    // 500ms at most (nanoseconds), so the deadline/g_interrupted check re-runs; shorter while the stall is ending.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    bool stalling = true;
    ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        int64_t timeout_ns = poll_timeout_ns;
        if (stalling && first_arrival_ns != 0) {
            uint64_t now = tt_get_ns();
            uint64_t stall_end = first_arrival_ns + pause_ns;
            if (now >= stall_end) {
                stalling = false;
                printf("Subscriber: stalled %.3fs after the first arrival, now taking\n", pause_s);
            } else if ((int64_t)(stall_end - now) < timeout_ns) {
                timeout_ns = (int64_t)(stall_end - now);
            }
        }
        if (!stalling) {
            take_all();
        }
        ret = tt_Context_poll(&node, timeout_ns);
    }
    if (!stalling) {
        take_all();
    }

    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;

    // lost counts every sample never delivered, expired included; expired is the part LIFESPAN dropped at take.
    // stalled=0 means no sample ever arrived, queue_overflow>0 that the cache was too small: either run is void.
    printf("RESULT: framework=tickle scenario=lifespan_expiry role=server pause_s=%.2f recv=%lu lost=%lu "
           "loss_pct=%.6f expired=%lu stalled=%d queue_overflow=%lu\n",
           pause_s, (unsigned long)received, (unsigned long)lost, loss_pct, (unsigned long)expired, stalling ? 0 : 1,
           (unsigned long)queue_overflow);

    tt_Context_destroy(&node);
    return 0;
}
