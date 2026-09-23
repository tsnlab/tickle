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
// TickLE core native server/receiver role (no rmw). Same one-way stream shape as
// best_effort_throughput/server.c (this side is the authoritative one for loss/throughput),
// `sub.reliable = true` to match the client's own RELIABLE Publisher - no cache is needed on the
// Subscriber side (tickle.h: retransmission state lives on the Publisher's own reliable_cache).

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

// Order-independent receive tracking (2026-09-22, at the user's own direction, following the
// real ACKNACK-flood fix in tickle.c). RELIABLE only adds a delivery guarantee via
// retransmission, not an ordering one (deliver_data_to_subscriber()'s own doc comment, tickle.c;
// struct tt_WriterProxy's own doc comment makes the identical point) - a successfully recovered
// retransmit can legitimately arrive *after* later, in-order samples, once one real ACKNACK round
// trip (a handful of ms) has elapsed. The strict-order counting this file used before
// (`data->seq <= last_seq -> ignore`) could never credit that: by the time any retransmit could
// possibly land, `last_seq` had already advanced past it from later arrivals, so the gap was
// already counted "lost" the moment it was first noticed, and the retransmit's own actual arrival
// changed nothing - a genuine measurement bug, not a core bug, found investigating why loss_pct
// stayed far above the injected tc rate even after the flood fix made recovery attempts routine
// (PLAN.md's real-HIL record for this scenario, 2026-09-22).
//
// Fixed with a distinct-sequence-number bitmap instead: any sample that ever arrives, in any
// order, is counted as received exactly once (first arrival only - a true duplicate re-delivery,
// same doc-commented core residual as before, is silently ignored the same way). Genuine loss is
// computed once at the end as "how many sequence numbers up to the highest one ever seen were
// never received at all" - this correctly credits an out-of-order recovery regardless of when it
// arrives, rather than penalizing it twice (once for the transient gap, once more by discarding
// the recovery itself).
#define MAX_TRACKED_SEQ 20000000u // ~100s worth of headroom at this scenario's own ~200K msg/s max rate
static uint8_t received_bitmap[(MAX_TRACKED_SEQ / 8) + 1];
static uint64_t received = 0;
static uint32_t max_seq_seen = 0;
// Phase 3 step 4 - the lowest sequence number this Subscriber ever saw. A WriterProxy is created by
// the first DATA that actually arrives and takes its ack baseline from that sample, so anything
// published before it is not merely lost but unobservable: the Subscriber cannot know a sequence
// number it never saw was ever sent, and so never requests it. Measured at 50% injected loss, that
// is the entire residual under KEEP_ALL - the missing seq_nos were always 1..3, never the tail and
// never mid-stream, with every abandonment counter on both sides at zero.
static uint32_t first_seq_seen = 0;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    if (data->seq == 0 || data->seq > MAX_TRACKED_SEQ) {
        return; // out of this scenario's own realistic tracked range - never expected in practice
    }
    uint32_t idx = data->seq - 1;
    if (received_bitmap[idx / 8] & (1U << (idx % 8))) {
        return; // genuine duplicate re-delivery (already counted on first arrival)
    }
    received_bitmap[idx / 8] |= (uint8_t)(1U << (idx % 8));
    received++;
    if (first_seq_seen == 0 || data->seq < first_seq_seen) {
        first_seq_seen = data->seq;
    }
    if (data->seq > max_seq_seen) {
        max_seq_seen = data->seq;
    }
}

// Phase 3 step 4 (rmw_tickle/PLAN.md) - which sequence numbers are missing, not just how many.
// With KEEP_ALL the residual is small enough that the count alone says nothing: one number in the
// final handful is a teardown boundary, the same number at the very start is the pre-match window,
// and the same number mid-stream would be a delivery failure. They need different explanations, and
// this is what told them apart - the answer turned out to be always 1..3. Prints the first and last
// few so both ends are visible without dumping thousands of lines at KEEP_LAST loss rates.
//
// Split out of main() to keep its own cognitive complexity under the project's clang-tidy
// threshold, same reason parse_args() exists in the client.
static void print_missing_seqs(uint64_t lost) {
    if (lost == 0) {
        return;
    }
    uint32_t first[8];
    uint32_t last[8];
    unsigned first_n = 0;
    unsigned last_n = 0;
    for (uint32_t missing_seq = 1; missing_seq <= max_seq_seen; missing_seq++) {
        uint32_t idx = missing_seq - 1;
        if ((received_bitmap[idx / 8] & (1U << (idx % 8))) != 0) {
            continue;
        }
        if (first_n < 8) {
            first[first_n++] = missing_seq;
        }
        last[last_n % 8] = missing_seq;
        last_n++;
    }
    printf("MISSING: count=%lu max_seq_seen=%u first=", (unsigned long)lost, max_seq_seen);
    for (unsigned i = 0; i < first_n; i++) {
        printf("%u,", first[i]);
    }
    printf(" last=");
    unsigned last_shown = last_n < 8 ? last_n : 8;
    for (unsigned i = 0; i < last_shown; i++) {
        printf("%u,", last[last_n < 8 ? i : (last_n + i) % 8]);
    }
    printf("\n");
}

static const double default_safety_cap_s = 40.0;
// Phase 2 - -w <samples>: the RELIABLE tracking window this Subscriber asks for; 0 = core default.
static uint32_t window_samples = 0;
static const double safety_cap_buffer_s = 15.0;

int main(int argc, char** argv) {
    bool durable = false; // -D, see sub.durable below
    double safety_cap_s = default_safety_cap_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-D") == 0) {
            durable = true;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            // Phase 2 (rmw_tickle/PLAN.md) - RELIABLE tracking window in samples, so one HIL
            // sweep can compare them. 0/absent keeps TickLE core's own embedded-first default
            // (tt_RELIABLE_BITMAP_BITS), i.e. exactly what earlier runs measured.
            //
            // Keep it at or below the *client's* own -K depth: tracking further back than the
            // Publisher retains can only ever be skipped, never recovered. Measured at -K 1024:
            // -w 1024 lost nothing over 6 runs, -w 4096 lost 191-368 per run.
            window_samples = (uint32_t)strtoul(argv[++i], NULL, 10);
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

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "stream", (tt_SUBSCRIBER_CALLBACK)stream_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }
    sub.reliable = true;
    // -D: request TRANSIENT_LOCAL, matching the client's own -D. The RxO fix (PLAN.md Milestone 60)
    // keys the first-contact baseline on sub->durable: a durable Subscriber syncs to the
    // Publisher's first_available_seq_no rather than to "whatever arrives next", which is exactly
    // the pre-match window COMPARISON.MD §3b documents for the volatile case.
    sub.durable = durable;
    // Phase 2 - a wider caller-owned tracking window, one per simultaneously tracked Publisher.
    // Sized for the widest this build allows; only the requested prefix is actually used.
    static uint64_t tracking[tt_MAX_PEER_COUNT * tt_RELIABLE_BITMAP_MAX_WORDS];
    if (window_samples > 0) {
        uint32_t words = window_samples / tt_RELIABLE_BITMAP_WORD_BITS;
        if (words > tt_RELIABLE_BITMAP_MAX_WORDS) {
            words = tt_RELIABLE_BITMAP_MAX_WORDS;
        }
        if (words > 0) {
            sub.tracking_bitmaps = tracking;
            sub.tracking_words = (uint16_t)words;
        }
    }

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    // 500ms (nanoseconds), so the deadline/g_interrupted check re-runs.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, poll_timeout_ns);
    }

    // Both numbers, always, never one silently standing in for the other (rmw_tickle/PLAN.md Phase
    // 3 step 4, at Plan's direction):
    //   lost / loss_pct       - counted from seq 1, exactly as before. The raw figure.
    //   post_match_lost       - counted from the first sequence number this Subscriber ever saw,
    //                           i.e. excluding the window it could not observe at all.
    //   prematch_window       - how many samples that window swallowed, so it is visible rather
    //                           than inferred from the difference.
    // Reported side by side because the DDS harnesses this scenario is compared against do not have
    // this artefact - their reader match is symmetric and the writer waits for it - so quietly
    // switching to the post-match figure would normalise away a TickLE-specific effect and flatter
    // the comparison. Both columns keep it honest in both directions.
    uint64_t lost = max_seq_seen > received ? (uint64_t)max_seq_seen - received : 0;
    double loss_pct = max_seq_seen > 0 ? (100.0 * (double)lost / (double)max_seq_seen) : 0.0;
    uint32_t prematch_window = first_seq_seen > 0 ? first_seq_seen - 1 : 0;
    uint64_t observable =
        (max_seq_seen >= first_seq_seen && first_seq_seen > 0) ? (uint64_t)max_seq_seen - first_seq_seen + 1 : 0;
    uint64_t post_match_lost = observable > received ? observable - received : 0;
    double post_match_loss_pct = observable > 0 ? (100.0 * (double)post_match_lost / (double)observable) : 0.0;

    print_missing_seqs(lost);

    printf("RESULT: framework=tickle scenario=reliable_throughput role=server recv=%lu lost=%lu loss_pct=%.1f "
           "post_match_lost=%lu post_match_loss_pct=%.1f prematch_window=%u first_seq=%u window_samples=%u\n",
           (unsigned long)received, (unsigned long)lost, loss_pct, (unsigned long)post_match_lost, post_match_loss_pct,
           prematch_window, first_seq_seen, window_samples > 0 ? window_samples : (uint32_t)tt_RELIABLE_BITMAP_BITS);
    print_reliable_stats("server");

    tt_Node_destroy(&node);
    return 0;
}
