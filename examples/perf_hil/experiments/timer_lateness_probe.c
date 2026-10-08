/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// What G a context measures for itself (struct tt_Context.timer_lateness_ns, 2026-10-08), against the lateness its
// retry timer actually sees. One context and one retry timer - call_retry() on a client with no call outstanding,
// which does nothing - due every PERIOD_US, polled with tt_Context_poll(-1), the wait-to-the-next-entry path G is
// sampled on, for DURATION_S. After each run the probe records how late it came back (its clock minus the due time).
//
// Whitebox (it #includes src/tickle.c, as the unit tests do) because the samples come only from the retry timers'
// waits, and no public call schedules one without a server to talk to. Built by granularity_pc.sh:
//   gcc -O2 -D_GNU_SOURCE -Iinclude -Isrc timer_lateness_probe.c src/hal_linux.c src/encoding.c src/log.c -lpthread -lm
//
// Prints one line: G and the estimator's mean / deviation as the context left them, and the observed p50 / p90 / p99
// / p99.9 / max of the retry timer's lateness. Run in a private network namespace (CLAUDE.md).
//
// HOW TO READ IT, written before the first run: G is meant to be a lateness the timer seldom exceeds (mean + 4 x
// deviation, as srtt + 4 x rttvar is for a round trip). It READS AS INTENDED if p90 <= G <= 4 x p99 + the clock
// resolution. The probe's lateness includes the return from the poll, which the sample does not, so G a little under
// p90 is not by itself a contradiction; under p50 would mean the samples miss most of the lateness.
//
// Usage: timer_lateness_probe [period_us=1000] [duration_s=5]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h> // _tt_CONFIG
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) - whitebox: call_retry() is static

#define MAX_SAMPLES 200000
#define PER_MILLE 1000U
#define P50 500U
#define P90 900U
#define P99 990U
#define P999 999U
#define DEFAULT_PERIOD_US 1000UL
#define DEFAULT_DURATION_S 5UL
#define NS_PER_US 1000U
#define NS_PER_S 1000000000ULL

static uint64_t g_late[MAX_SAMPLES];
static size_t g_count;

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
    unsigned long period_us = argc > 1 ? strtoul(argv[1], NULL, 10) : DEFAULT_PERIOD_US;
    unsigned long duration_s = argc > 2 ? strtoul(argv[2], NULL, 10) : DEFAULT_DURATION_S;
    const uint64_t period_ns = (uint64_t)period_us * NS_PER_US;

    static struct tt_Context node;
    static struct tt_Client idle_client; // cache NULL: call_retry() returns at once
    memset(&node, 0, sizeof(node));
    _tt_CONFIG.broadcast = "192.168.10.255"; // the harnesses' link, as their servers and clients set it
    if (tt_Context_create(&node) != tt_RET_OK) {
        fprintf(stderr, "tt_Context_create failed\n");
        return 1;
    }
    uint64_t end = tt_get_ns() + ((uint64_t)duration_s * NS_PER_S);
    uint64_t due = tt_get_ns() + period_ns;
    (void)tt_Context_schedule(&node, due, call_retry, &idle_client);
    while (tt_get_ns() < end) {
        (void)tt_Context_poll(&node, -1);
        uint64_t now = tt_get_ns();
        if (now < due) {
            continue; // another entry (an announce) ended this poll; the retry timer is still pending
        }
        if (g_count < MAX_SAMPLES) {
            g_late[g_count++] = now - due;
        }
        due = now + period_ns;
        (void)tt_Context_schedule(&node, due, call_retry, &idle_client);
    }
    qsort(g_late, g_count, sizeof(g_late[0]), cmp_u64);
    printf("PROBE period_us=%lu duration_s=%lu resolution_ns=%u g_ns=%u mean_ns=%u var_ns=%u entries=%zu "
           "late_p50_ns=%llu late_p90_ns=%llu late_p99_ns=%llu late_p999_ns=%llu late_max_ns=%llu\n",
           period_us, duration_s, node.timer_resolution_ns, node.timer_lateness_ns, node.timer_lateness_mean_ns,
           node.timer_lateness_var_ns, g_count, (unsigned long long)quantile(P50), (unsigned long long)quantile(P90),
           (unsigned long long)quantile(P99), (unsigned long long)quantile(P999),
           (unsigned long long)(g_count ? g_late[g_count - 1] : 0));
    tt_Context_destroy(&node);
    return 0;
}
