/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Warm-up and cool-down for every framework's bench (the user, 2026-10-06: "모든 프레임워크에 warm-up과 cool-down은
// 필요해"; docs/TESTING.md section 5). Every published statistic is taken over a measured window that excludes an
// equal warm-up at the start and cool-down at the end, so no figure is a start-up or teardown transient. Found by
// ROADMAP item 6: TickLE's same-host p4 latency was a first-lap figure - first-touch page faults on the 11.6 MB
// reorder ring cost ~16 us per round trip for the first ~2048 samples (p4_reorder_firsttouch.sh).
//
// Header-only and plain C, included by all three frameworks' clients and servers like RttQuantiles.h, so the defaults
// and the window rule cannot drift apart between them.
//
// LATENCY (reliable_latency clients): counted in round trips. -W warm-up pings come first, then -d seconds of measured
// pings at the run's -i spacing, then -C cool-down pings. Only the measured pings enter the RTT statistics. Warm-up
// and cool-down pings are spaced -I apart (the edge interval): their job is to run the round trip through every slot
// once, which takes the same number of pings at any spacing, and at the campaign's 0.1 s spacing 4096 of them would
// otherwise take seven minutes. The idle before the first measured ping is still the measured -i.
//
// THROUGHPUT (best_effort_throughput, reliable_throughput, client and server): counted in seconds of the SENDER's
// clock, read from each sample's own send_ns. A sample is counted into a bucket of BENCH_WINDOW_BUCKET_NS by
// send_ns - (the first sample's send_ns); the measured window is every bucket lying wholly inside
// [first + warm-up, last - cool-down], and its rate is samples / (buckets x bucket width). The client applies it to
// what it sent and the server to what it received, by the same rule and the same clock, so the two windows cover the
// same samples (up to the samples lost at either edge) even cross-host, where the two hosts' clocks differ.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Latency: round trips at each end. 4096 is TickLE's largest per-sample ring - BENCH_REORDER_SLOTS =
// tt_RELIABLE_BITMAP_MAX_BITS reorder slots on each side - so the warm-up writes every slot at least once for any
// shape (p4 takes two slots per sample, so it gets two laps' worth of seq_nos and one full lap of samples). The same
// count for every framework, and the same at both ends, so whichever reading of "equal" a reader takes holds.
#define BENCH_WARMUP_ROUND_TRIPS 4096U
#define BENCH_COOLDOWN_ROUND_TRIPS 4096U
#define BENCH_EDGE_INTERVAL_S 0.001

// Throughput: seconds at each end, the same for every framework and at both ends. Two seconds covers the ondemand
// governor's ramp (tens of ms) many times over, and a lap of TickLE's 4096-slot ring at any rate above 2,048 samples/s,
// which every lossless cell exceeds by two orders of magnitude.
#define BENCH_WARMUP_S 2.0
#define BENCH_COOLDOWN_S 2.0

// 2^22 ns = 4.194304 ms buckets, 2^17 of them = 549 s of sender time. A sample past that sets window=fail:too_long
// rather than being folded into the last bucket.
#define BENCH_WINDOW_BUCKET_SHIFT 22U
#define BENCH_WINDOW_BUCKETS (1U << 17U)
#define BENCH_WINDOW_NS_PER_S 1e9
#define BENCH_WINDOW_FIELDS_MAX 256

struct BenchWindow {
    double warmup_s;
    double cooldown_s;
    uint64_t first_ns;
    uint64_t last_ns;
    uint64_t total;
    bool started;
    bool overflow;
    uint32_t count[BENCH_WINDOW_BUCKETS];
};

// What the window measured. state is "ok", or "fail:<why>" when the window holds nothing to report.
struct BenchWindowResult {
    uint64_t samples;
    double seconds;
    const char* state;
};

// The struct is meant to be a static (zeroed) object; this sets the defaults without touching the bucket array, whose
// pages then stay unmapped until the run reaches them.
static inline void BenchWindow_init(struct BenchWindow* window) {
    window->warmup_s = BENCH_WARMUP_S;
    window->cooldown_s = BENCH_COOLDOWN_S;
}

// --warmup-s <s> and --cooldown-s <s>: long options, because -W and -C already mean something else in
// reliable_throughput (the ACK watermark and the drain cap). Returns true if argv[*idx] was one of them, having
// consumed its value.
static inline bool BenchWindow_parse_arg(struct BenchWindow* window, int argc, char** argv, int* idx) {
    if (*idx + 1 >= argc) {
        return false;
    }
    if (strcmp(argv[*idx], "--warmup-s") == 0) {
        window->warmup_s = atof(argv[++*idx]);
        return true;
    }
    if (strcmp(argv[*idx], "--cooldown-s") == 0) {
        window->cooldown_s = atof(argv[++*idx]);
        return true;
    }
    return false;
}

// One sample, by its sender's own timestamp. A sample stamped before the first one seen (a BEST_EFFORT reorder) counts
// into the first bucket, which is warm-up whenever warm-up is not zero.
static inline void BenchWindow_add(struct BenchWindow* window, uint64_t send_ns) {
    if (!window->started) {
        window->started = true;
        window->first_ns = send_ns;
        window->last_ns = send_ns;
    }
    uint64_t offset = send_ns > window->first_ns ? send_ns - window->first_ns : 0;
    uint64_t bucket = offset >> BENCH_WINDOW_BUCKET_SHIFT;
    if (bucket >= BENCH_WINDOW_BUCKETS) {
        window->overflow = true;
        return;
    }
    window->count[bucket]++;
    window->total++;
    if (send_ns > window->last_ns) {
        window->last_ns = send_ns;
    }
}

static inline struct BenchWindowResult BenchWindow_measure(const struct BenchWindow* window) {
    struct BenchWindowResult result = {0, 0.0, "ok"};
    if (window->overflow) {
        result.state = "fail:too_long";
        return result;
    }
    if (!window->started) {
        result.state = "fail:no_samples";
        return result;
    }
    const double bucket_ns = (double)(1ULL << BENCH_WINDOW_BUCKET_SHIFT);
    const double span_ns = (double)(window->last_ns - window->first_ns);
    const double begin_ns = window->warmup_s * BENCH_WINDOW_NS_PER_S;
    const double end_ns = span_ns - (window->cooldown_s * BENCH_WINDOW_NS_PER_S);
    if (begin_ns < 0.0 || end_ns <= begin_ns) {
        result.state = "fail:shorter_than_warmup_plus_cooldown";
        return result;
    }
    // Whole buckets only: the first bucket starting at or after begin, up to the last one ending at or before end.
    uint64_t first_bucket = (uint64_t)((begin_ns + bucket_ns - 1.0) / bucket_ns);
    uint64_t end_bucket = (uint64_t)(end_ns / bucket_ns); // exclusive
    if (end_bucket > BENCH_WINDOW_BUCKETS) {
        end_bucket = BENCH_WINDOW_BUCKETS;
    }
    if (end_bucket <= first_bucket) {
        result.state = "fail:shorter_than_warmup_plus_cooldown";
        return result;
    }
    for (uint64_t bucket = first_bucket; bucket < end_bucket; bucket++) {
        result.samples += window->count[bucket];
    }
    result.seconds = (double)(end_bucket - first_bucket) * bucket_ns / BENCH_WINDOW_NS_PER_S;
    if (result.samples == 0) {
        result.state = "fail:no_samples_in_window";
    }
    return result;
}

// The RESULT-line fields: "warmup_s= cooldown_s= win_s= win_<what>= win_<what>_mbps= window=", with <what> "sent"
// and "send" on a sender, "recv" and "recv" on a receiver. win_<what>_mbps is the windowed twin of the whole-run
// send_mbps/recv_mbps, which stay on the line for comparison.
static inline const char* BenchWindow_fields(const struct BenchWindow* window, const char* count_name,
                                             const char* rate_name, size_t sample_bytes, char* out, size_t out_len) {
    const double bits_per_byte = 8.0;
    const double bits_per_megabit = 1e6;
    struct BenchWindowResult result = BenchWindow_measure(window);
    double mbps = result.seconds > 0.0 ? (double)result.samples * (double)sample_bytes * bits_per_byte /
                                             bits_per_megabit / result.seconds
                                       : 0.0;
    snprintf(out, out_len, "warmup_s=%.3f cooldown_s=%.3f win_s=%.3f win_%s=%llu win_%s_mbps=%.3f window=%s",
             window->warmup_s, window->cooldown_s, result.seconds, count_name, (unsigned long long)result.samples,
             rate_name, mbps, result.state);
    return out;
}
