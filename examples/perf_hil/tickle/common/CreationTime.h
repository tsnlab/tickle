/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Creation-time round trip (the user's decision of 2026-10-17): latency is the arrival time minus the data's CREATION
// time, the time the sample was scheduled to be sent, not the stamp the sender reads just before its write. A
// latency client paces its pings one interval after the previous reply; the creation time of a ping is that due time
// (the previous reply's time + the interval), and its stamp, send_ns, is read after the timer wakes, just before the
// write. Each client records both for every measured round trip: crt = arrival - due, and gap = send_ns - due (how
// late the sender stamped against its schedule, a timer wake when nothing else waits). The rtt_* fields stay the
// stamp's. Read by creation_latency_summary.py --rtt; nothing waits at a ping-pong sender, so the two definitions must
// agree within the noise there (its R4). Header-only plain C, included by all three frameworks' clients like
// RttQuantiles.h, so the three cannot drift apart.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "RttQuantiles.h"

// The RESULT-line fields BenchCreation_fields() writes fit this many bytes.
#define BENCH_CREATION_FIELDS_MAX 320

struct BenchCreation {
    struct BenchRtt crt; // arrival - due, ms
    struct BenchRtt gap; // send stamp - due, ms
    double crt_sum_ms;
    double gap_min_ms;
    double gap_max_ms;
    uint32_t no_due; // measured round trips whose ping had no due time recorded (must stay 0)
};

static inline void BenchCreation_init(struct BenchCreation* creation) {
    creation->crt.count = 0;
    creation->gap.count = 0;
    creation->crt_sum_ms = 0.0;
    creation->gap_min_ms = 0.0;
    creation->gap_max_ms = 0.0;
    creation->no_due = 0;
}

// One measured round trip: its arrival, the time its ping was due, and the stamp the ping carried. due_ns 0 means the
// client recorded no due time: counted in no_due and left out, never read as a gap of 0.
static inline void BenchCreation_add(struct BenchCreation* creation, uint64_t arrival_ns, uint64_t due_ns,
                                     uint64_t send_ns) {
    if (due_ns == 0) {
        creation->no_due++;
        return;
    }
    const double ns_per_ms = 1e6;
    double crt_ms = (double)(int64_t)(arrival_ns - due_ns) / ns_per_ms;
    double gap_ms = (double)(int64_t)(send_ns - due_ns) / ns_per_ms;
    if (creation->gap.count == 0 || gap_ms < creation->gap_min_ms) {
        creation->gap_min_ms = gap_ms;
    }
    if (creation->gap.count == 0 || gap_ms > creation->gap_max_ms) {
        creation->gap_max_ms = gap_ms;
    }
    creation->crt_sum_ms += crt_ms;
    BenchRtt_add(&creation->crt, crt_ms);
    BenchRtt_add(&creation->gap, gap_ms);
}

// A quantile in us for the RESULT line; -1 (no round trips) stays -1 rather than reading as -1000 us.
static inline double BenchCreation_quantile_us(struct BenchRtt* rtt, double q) {
    const double us_per_ms = 1e3;
    double value_ms = BenchRtt_quantile(rtt, q);
    return rtt->count > 0 ? value_ms * us_per_ms : value_ms;
}

// The RESULT-line fields, after the run (the quantiles sort in place). stamp is the client's send-stamp round trips
// (its rtt_* fields). rtt_p50_us and crt_p50_us repeat rtt_p50_ms and crt_p50_ms to 0.1 us (2026-10-18): the ms
// fields print to 1 us, and the R4 control (creation_latency_summary.py rtt_r4) compares d = crt_p50 - rtt_p50 with
// the gap at a 1 us print resolution, so three reps printing the same p50 failed it on rounding alone (same-host S10c
// at 2579e822, |d - g| 1.1 us). The ms fields stay as they were: the summaries and gates read them.
static inline const char* BenchCreation_fields(struct BenchCreation* creation, struct BenchRtt* stamp, char* buf,
                                               size_t len) {
    const double us_per_ms = 1e3;
    uint32_t count = creation->crt.count;
    double avg = count > 0 ? creation->crt_sum_ms / (double)count : -1.0;
    (void)snprintf(buf, len,
                   "crt_p50_ms=%.3f crt_p99_ms=%.3f crt_avg_ms=%.6f crt_kept=%u gap_p50_us=%.1f gap_p99_us=%.1f "
                   "gap_min_us=%.1f gap_max_us=%.1f crt_no_due=%u rtt_p50_us=%.1f crt_p50_us=%.1f",
                   BenchRtt_quantile(&creation->crt, BENCH_RTT_P50), BenchRtt_quantile(&creation->crt, BENCH_RTT_P99),
                   avg, (unsigned)count, BenchRtt_quantile(&creation->gap, BENCH_RTT_P50) * us_per_ms,
                   BenchRtt_quantile(&creation->gap, BENCH_RTT_P99) * us_per_ms, creation->gap_min_ms * us_per_ms,
                   creation->gap_max_ms * us_per_ms, (unsigned)creation->no_due,
                   BenchCreation_quantile_us(stamp, BENCH_RTT_P50),
                   BenchCreation_quantile_us(&creation->crt, BENCH_RTT_P50));
    return buf;
}
