/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The median and p99 of a latency client's round trips, beside the mean it already prints (fairness audit,
// 2026-10-05). The published figure was the mean, and one ~112 ms FastDDS round trip among 2,000 made up most of an
// S7/S8 mean: a mean alone penalises whichever framework has the noisier tail, which is a reading the table should
// state rather than bake in. Header-only and plain C, included by all three frameworks' clients like CpuFreq.h.
//
// Keeps every round trip up to BENCH_RTT_MAX; past that the quantiles describe the first BENCH_RTT_MAX and the RESULT
// line says how many were kept (rtt_kept), so a truncated quantile cannot pass for a whole one.

#pragma once

#include <stdint.h>
#include <stdlib.h>

// The two quantiles every client prints, named once so the three cannot drift apart.
#define BENCH_RTT_P50 0.50
#define BENCH_RTT_P99 0.99

#ifndef BENCH_RTT_MAX
#define BENCH_RTT_MAX 65536
#endif

struct BenchRtt {
    double ms[BENCH_RTT_MAX];
    uint32_t count;
};

static inline void BenchRtt_add(struct BenchRtt* rtt, double rtt_ms) {
    if (rtt->count < BENCH_RTT_MAX) {
        rtt->ms[rtt->count++] = rtt_ms;
    }
}

static inline int BenchRtt_compare(const void* left, const void* right) {
    double lhs = *(const double*)left;
    double rhs = *(const double*)right;
    return (lhs > rhs) - (lhs < rhs);
}

// Nearest-rank quantile, q in (0, 1]; -1 with no round trips. Sorts in place, so call it after the run.
static inline double BenchRtt_quantile(struct BenchRtt* rtt, double q) {
    if (rtt->count == 0) {
        return -1.0;
    }
    qsort(rtt->ms, rtt->count, sizeof(rtt->ms[0]), BenchRtt_compare);
    uint32_t rank = (uint32_t)((q * (double)rtt->count) + 0.999999);
    if (rank < 1) {
        rank = 1;
    }
    if (rank > rtt->count) {
        rank = rtt->count;
    }
    return rtt->ms[rank - 1];
}
