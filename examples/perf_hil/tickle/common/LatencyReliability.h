/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// One latency bench, two reliabilities (2026-10-28). best_effort_latency is reliable_latency's client and server with
// BEST_EFFORT: each framework's best_effort_latency/{client,server} defines BENCH_LATENCY_BEST_EFFORT 1 and includes
// its reliable_latency twin, so the windowing (BenchWindow.h), the pacing (the 2026-10-05 fairness audit) and every
// statistic are the same code in both scenarios and cannot drift apart. Until then best_effort_latency was a separate,
// older client - 100 pings, no warm-up, no instrument= field - and every campaign row of it read VOID.
//
// Header-only and plain C, included by all three frameworks' latency clients and servers, like BenchWindow.h.
//
// What differs, and only this: the reliability kind of the writer and the reader on both sides, and in TickLE the
// reader's reorder buffer (none for BEST_EFFORT, which has nothing to hold a sample for). History stays as
// reliable_latency's: KEEP_LAST 8 on the writers (and the DDS readers), KEEP_ALL on the TickLE readers. A BEST_EFFORT
// VOLATILE TickLE Publisher retains nothing, so its depth is inert there.
//
// The treatment is reported, not assumed: every latency RESULT line carries reliability=, read back from the created
// endpoints (bench_reliability_name()), so an arm that silently ran RELIABLE says so, and campaign_sweep.sh voids a
// best_effort_latency row that does not say reliability=best_effort.

#pragma once

#ifndef BENCH_LATENCY_BEST_EFFORT
#define BENCH_LATENCY_BEST_EFFORT 0
#endif

#if BENCH_LATENCY_BEST_EFFORT
#define BENCH_LATENCY_SCENARIO "best_effort_latency"
#else
#define BENCH_LATENCY_SCENARIO "reliable_latency"
#endif

// The reliability one side's writer and reader run, each 1 RELIABLE, 0 BEST_EFFORT or -1 unreadable: "reliable",
// "best_effort", "mixed" when they differ, or "unreadable" when either could not be read, which must never pass for
// either kind.
static inline const char* bench_reliability_name(int writer_reliable, int reader_reliable) {
    if (writer_reliable < 0 || reader_reliable < 0) {
        return "unreadable";
    }
    if (writer_reliable != reader_reliable) {
        return "mixed";
    }
    return writer_reliable != 0 ? "reliable" : "best_effort";
}
