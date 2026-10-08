/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// What G a context measures for itself (struct tt_Context.timer_lateness_ns, 2026-10-08), against the lateness
// distribution its scheduler entries actually see. One context, one entry rescheduled every PERIOD_US, polled with
// tt_Context_poll(-1) - the wait-to-the-next-entry path G is sampled on - for DURATION_S. Each run of the entry
// records how late it ran (its own clock reading minus the time it was due).
//
// Prints one line: G and the estimator's mean / deviation as the context left them, and the observed p50 / p90 / p99
// / p99.9 / max of the entry lateness. Run by timer_lateness_pc.sh, in a private network namespace (CLAUDE.md).
//
// HOW TO READ IT, written before the first run: G is meant to be a lateness the timer seldom exceeds (mean + 4 x
// deviation, as srtt + 4 x rttvar is for a round trip). It READS AS INTENDED if p90 <= G, and G <= 4 x p99 + the
// clock resolution (a G far above every observed lateness would make every retry wait for nothing). The entry's
// lateness includes the dispatch after the wake, which the sample does not, so a G a little below p50 of it would not
// be a contradiction on its own; below p50 by more than 20% would mean the sample misses most of the lateness.
//
// Usage: timer_lateness_probe [period_us=1000] [duration_s=5]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/hal.h>
#include <tickle/tickle.h>

#define MAX_SAMPLES 200000
#define PER_MILLE 1000U

static uint64_t g_due;
static uint64_t g_period_ns;
static uint64_t g_late[MAX_SAMPLES];
static size_t g_count;

static void tick(struct tt_Context* node, uint64_t time, void* param) {
    (void)time;
    (void)param;
    uint64_t now = tt_get_ns();
    if (g_count < MAX_SAMPLES && now >= g_due) {
        g_late[g_count++] = now - g_due;
    }
    g_due = now + g_period_ns;
    (void)tt_Context_schedule(node, g_due, tick, NULL);
}

static int cmp_u64(const void* lhs, const void* rhs) {
    uint64_t left = *(const uint64_t*)lhs;
    uint64_t right = *(const uint64_t*)rhs;
    return (left > right) - (left < right);
}

static uint64_t quantile(unsigned per_mille) {
    if (g_count == 0) {
        return 0;
    }
    size_t index = (g_count * per_mille) / PER_MILLE;
    return g_late[index < g_count ? index : g_count - 1];
}

int main(int argc, char** argv) {
    unsigned long period_us = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000UL;
    unsigned long duration_s = argc > 2 ? strtoul(argv[2], NULL, 10) : 5UL;
    g_period_ns = (uint64_t)period_us * 1000U;

    static struct tt_Context node;
    memset(&node, 0, sizeof(node));
    if (tt_Context_create(&node) != tt_RET_OK) {
        fprintf(stderr, "tt_Context_create failed\n");
        return 1;
    }
    uint64_t start = tt_get_ns();
    uint64_t end = start + ((uint64_t)duration_s * 1000000000ULL);
    g_due = start + g_period_ns;
    (void)tt_Context_schedule(&node, g_due, tick, NULL);
    while (tt_get_ns() < end) {
        (void)tt_Context_poll(&node, -1);
    }
    qsort(g_late, g_count, sizeof(g_late[0]), cmp_u64);
    printf("PROBE period_us=%lu duration_s=%lu resolution_ns=%u g_ns=%u mean_ns=%u var_ns=%u entries=%zu "
           "late_p50_ns=%llu late_p90_ns=%llu late_p99_ns=%llu late_p999_ns=%llu late_max_ns=%llu\n",
           period_us, duration_s, node.timer_resolution_ns, node.timer_lateness_ns, node.timer_lateness_mean_ns,
           node.timer_lateness_var_ns, g_count, (unsigned long long)quantile(500U), (unsigned long long)quantile(900U),
           (unsigned long long)quantile(990U), (unsigned long long)quantile(999U),
           (unsigned long long)(g_count ? g_late[g_count - 1] : 0));
    tt_Context_destroy(&node);
    return 0;
}
