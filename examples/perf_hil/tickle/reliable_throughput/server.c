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

#include "Bench.h"

// DATA_FRAG's reassembly pool, reported so that c6 can be read as a number rather than inferred: how
// many slots this build has, how many samples were put back together, how many partial reassemblies
// were given up to make room (tt_Node.frag_abandoned), how many fragments were refused, and how many
// arrived for a sample already whole (frag_duplicate). All zero, and
// frag_slots=0, in a build without fragmentation - sample_path= on the same line says which it was.
#if tt_FRAG_ENABLED
#define BENCH_FRAG_SLOTS tt_FRAG_REASSEMBLY_SLOTS
#define BENCH_FRAG_COUNT(node, field) ((unsigned long)(node).field)
#else
#define BENCH_FRAG_SLOTS 0
#define BENCH_FRAG_COUNT(node, field) 0UL
#endif
#include "BenchStats.h" // shared instrumentation - see its own header

static struct BenchStats g_bench_stats;
static char g_bench_fields[BENCH_STATS_FIELDS_MAX];
#include "../common/CpuFreq.h"
#include "../common/CpuPlace.h"
#include "../common/reliable_stats_print.h"

// Reorder-buffer geometry for this example's RELIABLE Subscriber - see where it is assigned.
//
// Sized to the widest tracking window this build allows, not to a round number. A Subscriber can
// hold at most as many samples as it can track ahead of its oldest gap, so a buffer that covers
// the widest window makes overflow impossible by construction rather than unlikely - the same
// argument rmw_tickle's RMW_TICKLE_REORDER_SLOTS rests on. This was 512 at first, which is
// narrower than the -w a reliable_throughput run is allowed to request (up to 4096), so any run
// that asked for a wide window would have overflowed into the re-request fallback and measured
// that instead of the protocol.
#define BENCH_REORDER_SLOTS tt_RELIABLE_BITMAP_MAX_BITS
// tt_REORDER_SLOT_SIZE rounds up to a multiple of 8. This was a bare sizeof(...) + 16 = 116, which
// core then addressed with a 120-byte stride - so the last slots of the array were past its end.
#define BENCH_REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(sizeof(struct BenchData) + 16)

// The tracking window a Subscriber actually uses, in samples, as reorder slots - core's own width
// rule (subscriber_tracking_words(), tickle.c): the caller's tracking_words when it supplied
// bitmaps, otherwise the built-in tt_RELIABLE_BITMAP_WORDS. Capped at the array's capacity.
static uint16_t reorder_slots_for_window(const struct tt_Subscriber* sub) {
    const uint32_t words =
        (sub->tracking_bitmaps != NULL && sub->tracking_words > 0) ? sub->tracking_words : tt_RELIABLE_BITMAP_WORDS;
    const uint32_t slots = words * tt_RELIABLE_BITMAP_WORD_BITS;
    return (uint16_t)(slots < BENCH_REORDER_SLOTS ? slots : BENCH_REORDER_SLOTS);
}

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
// When the last sample arrived, for the idle-based lifetime in main(). 0 = none yet.
static uint64_t g_last_rx_ns = 0;

// Whether main()'s receive loop should stop - see the lifetime comment there. An absolute deadline
// until the first sample, then an idle cap measured from the last one.
static bool lifetime_over(uint64_t now, uint64_t deadline, uint64_t idle_cap_ns) {
    if (g_last_rx_ns == 0) {
        return now >= deadline;
    }
    return now - g_last_rx_ns >= idle_cap_ns;
}

static struct BenchCpuFreq g_cpu_freq;
static struct BenchCpuPlace g_cpu_place;
static const uint64_t cpu_freq_period_ns = 100ULL * 1000 * 1000;

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    BenchCpuFreq_sample(&g_cpu_freq, tt_get_ns(), cpu_freq_period_ns);
    BenchCpuPlace_sample(&g_cpu_place, tt_get_ns(), cpu_freq_period_ns);
    if (data->seq == 0 || data->seq > MAX_TRACKED_SEQ) {
        return; // out of this scenario's own realistic tracked range - never expected in practice
    }
    uint32_t idx = data->seq - 1;
    if (received_bitmap[idx / 8] & (1U << (idx % 8))) {
        return; // genuine duplicate re-delivery (already counted on first arrival)
    }
    received_bitmap[idx / 8] |= (uint8_t)(1U << (idx % 8));
    received++;
    g_last_rx_ns = tt_get_ns();
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
// -N <samples>: KEEP_ALL's history bound, the same flag as the client's and the DDS harnesses'
// (2026-09-26, COMPARISON.MD 4.4). A Publisher can hold no more unacknowledged than this Subscriber's
// window, which counts datagrams, so without -w the window is widened to N x the datagrams one sample
// takes, rounded up to whole bitmap words - otherwise the reader's default window, not N, would bound the
// run. The DDS readers take N as their own max_samples; this side has no history of its own to bound.
// Reported as keepall_samples= and window= on the RESULT line. -w, if given, still wins.
static uint32_t keepall_samples = 0;
static const double safety_cap_buffer_s = 15.0;

// The first writer proxy in use, for reporting its recovery estimate. This scenario has one writer;
// falls back to slot 0, whose zeroed estimate then reads as "no sample", if none was ever matched.
static const struct tt_WriterProxy* first_live_writer(const struct tt_Subscriber* sub) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (sub->writers[i].node_id != tt_NODE_ID_INVALID) {
            return &sub->writers[i];
        }
    }
    return &sub->writers[0];
}

// Split out of main() to keep its cognitive complexity under the project's clang-tidy threshold.
static void parse_args(int argc, char** argv, bool* durable, double* safety_cap_s) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            *safety_cap_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-D") == 0) {
            *durable = true;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            // Phase 2 (rmw_tickle/PLAN.md) - RELIABLE tracking window in samples, so one HIL
            // sweep can compare them. 0/absent keeps TickLE core's own embedded-first default
            // (tt_RELIABLE_BITMAP_BITS), i.e. exactly what earlier runs measured.
            //
            // Keep it at or below the *client's* own -K depth: tracking further back than the
            // Publisher retains can only ever be skipped, never recovered. Measured at -K 1024:
            // -w 1024 lost nothing over 6 runs, -w 4096 lost 191-368 per run.
            window_samples = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
            keepall_samples = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
    }
    // -N without -w: the window KEEP_ALL's N samples need (keepall_samples' own comment above).
    if (keepall_samples > 0 && window_samples == 0) {
        const uint32_t datagrams = keepall_samples * tt_sample_datagrams((uint32_t)sizeof(struct BenchData));
        window_samples = ((datagrams + tt_RELIABLE_BITMAP_WORD_BITS - 1) / tt_RELIABLE_BITMAP_WORD_BITS) *
                         tt_RELIABLE_BITMAP_WORD_BITS;
    }
}

int main(int argc, char** argv) {
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&g_bench_stats);
    bool durable = false; // -D, see sub.durable below
    double safety_cap_s = default_safety_cap_s;
    parse_args(argc, argv, &durable, &safety_cap_s);
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
    //
    // The ARRAY is sized for the widest window this build allows (-w up to 4096), so any -w runs.
    // The SLOT COUNT handed to core is set below, after the tracking window is known, to that
    // window - see the comment there.
    static uint64_t reorder[BENCH_REORDER_SLOTS * BENCH_REORDER_SLOT_BYTES / sizeof(uint64_t)];
    sub.reorder_storage = reorder;
    sub.reorder_slot_bytes = BENCH_REORDER_SLOT_BYTES;
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
    // Reorder slots = the tracking window this Subscriber actually uses (2026-09-26), mirroring
    // rmw_tickle's own default (RMW_TICKLE_REORDER_SLOTS, "defaulting to the window bound",
    // rmw_subscription.c) so the memory figure is what a user of the product gets.
    //
    // This used to hand core all BENCH_REORDER_SLOTS (4096) regardless of the window, which is 256
    // in every campaign run. Core picks a slot as seq % reorder_slots, so under loss the
    // out-of-order samples land all round a 4096-slot ring as seq climbs, until every page of it
    // is resident: c6's server read 13,061 KB against c4's 1,731, the difference being the ~11.4 MB
    // ring, while CycloneDDS read 5,841. Slots beyond the window can never be legitimately needed:
    // the Subscriber cannot hold a sample further than one window ahead of its oldest gap.
    // Fewer slots than the window is what storms (COMPARISON.MD to-do 17); exactly the window is
    // the floor that does not. The width below is core's own rule, subscriber_tracking_words().
    sub.reorder_slots = reorder_slots_for_window(&sub);

    BenchCpuFreq_init(&g_cpu_freq);
    BenchCpuPlace_init(&g_cpu_place);
    // Lifetime (2026-09-25): an absolute cap only until the first sample arrives, then an idle
    // cap. It was absolute throughout - -d + 15 s from start - and that truncated the one cell that
    // needed longer: P4 under 5% loss needs at least 16.8 s of wire time, so this server exited while
    // the client was still retransmitting, leaving 200 samples undelivered and peer_acks_end=0 in
    // what read as a protocol result. Any fixed figure only moves that cliff to a worse condition.
    // "Don't hang forever" means "stop when nothing is arriving", so that is what it now checks.
    // run_scenario.sh still ends the normal case with SIGINT as soon as the client finishes; these
    // are only the backstops. Identical in all three frameworks' servers - a lifetime rule that
    // differed would hand whichever lived longest the samples the other was cut off from.
    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    const uint64_t idle_cap_ns = (uint64_t)(safety_cap_buffer_s * (double)tt_SECOND);
    // 500ms (nanoseconds), so the lifetime/g_interrupted check re-runs.
    const int64_t poll_timeout_ns = 500LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && !lifetime_over(tt_get_ns(), deadline, idle_cap_ns) &&
           (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
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

    bench_stats_end(&g_bench_stats);

    // retry_interval_cfg_ns= is the interval libtickle itself was built with, 0 meaning dynamic -
    // asked of the library rather than read from this file's own config.h, so a harness built with
    // one -D against a library built with another reports the mode that ran, not the one it asked
    // for. In a fixed build the recovery_* pair is an upper bound on what a dynamic build would settle
    // at, not a prediction of it - see tt_WriterProxy.probe_ns (tickle.h). The recovery_* pair is the
    // estimate the matched writer's proxy learned - read raw, so the interval
    // it implies (srtt + 4 * rttvar, clamped) can be checked rather than trusted. The single writer
    // this scenario has is the first live slot.
    const struct tt_WriterProxy* first_writer = first_live_writer(&sub);
    // gap_abandoned=/gap_evicted= (2026-09-25) are the samples this Subscriber stopped waiting for
    // without delivering - core counts them in every build now, where before a RELIABLE Subscriber
    // could drop samples with every production counter at zero. They separate "never arrived"
    // from "given up on", which recv against the client's sent cannot.
    printf("RESULT: framework=tickle scenario=reliable_throughput role=server recv=%lu lost=%lu loss_pct=%.1f "
           "post_match_lost=%lu post_match_loss_pct=%.1f prematch_window=%u first_seq=%u window_samples=%u "
           "keepall_samples=%u "
           "reorder_slots=%u frag_slots=%d frag_reassembled=%lu frag_abandoned=%lu frag_dropped=%lu "
           "frag_duplicate=%lu "
           "cpu_mhz_mean=%.1f cpu_mhz_min=%.1f cpu_mhz_max=%.1f cpu_samples=%u cpu_main=%d cpu_main_share=%.2f "
           "cpu_migrations=%u gap_abandoned=%u gap_evicted=%u retry_interval_cfg_ns=%llu recovery_srtt_ns=%u "
           "recovery_rttvar_ns=%u %s\n",
           (unsigned long)received, (unsigned long)lost, loss_pct, (unsigned long)post_match_lost, post_match_loss_pct,
           prematch_window, first_seq_seen, window_samples > 0 ? window_samples : (uint32_t)tt_RELIABLE_BITMAP_BITS,
           keepall_samples, (unsigned)sub.reorder_slots, BENCH_FRAG_SLOTS, BENCH_FRAG_COUNT(node, frag_reassembled),
           BENCH_FRAG_COUNT(node, frag_abandoned), BENCH_FRAG_COUNT(node, frag_dropped),
           BENCH_FRAG_COUNT(node, frag_duplicate), BenchCpuFreq_mean_mhz(&g_cpu_freq),
           BenchCpuFreq_min_mhz(&g_cpu_freq), BenchCpuFreq_max_mhz(&g_cpu_freq), g_cpu_freq.samples,
           BenchCpuPlace_main_cpu(&g_cpu_place), BenchCpuPlace_main_share(&g_cpu_place), g_cpu_place.migrations,
           sub.gap_abandoned, sub.gap_evicted, (unsigned long long)tt_reliable_retry_interval_configured(),
           first_writer->recovery_srtt_ns, first_writer->recovery_rttvar_ns,
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_RECEIVER, received, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));
    print_reliable_stats("server");

    tt_Node_destroy(&node);
    return 0;
}
