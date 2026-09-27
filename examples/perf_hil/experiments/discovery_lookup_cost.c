/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// discovery_lookup_cost.c - what the per-sample discovery lookup costs (CONTEXT_NODE_PLAN.md 4b, 2026-09-27): every
// received DATA runs subscriber_incompatible_with_publisher() for each matching subscriber, which finds the
// publisher's entry with tt_Discovery_find(). The table is filled with tt_MAX_DISCOVERED_ENTITIES - 1 entities of
// other contexts first, the publisher's last, so it sits at the end; then the check is timed over CALLS calls, the
// median of ROUNDS rounds. Also printed: the same with the publisher's entry first.
//
// Build from the repo root, at a table size (16: core's default; 2048: rmw_tickle's):
//   gcc -O2 -I include -I src -Dtt_MAX_DISCOVERED_ENTITIES=2048 -o /tmp/lookup
//       examples/perf_hil/experiments/discovery_lookup_cost.c src/encoding.c src/log.c -lm

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define TEST_MOCK_DEFINE_STORAGE
#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: the check is static
#include "../../../tests/test_mock.h"

#define ROUNDS 21
#define CALLS 100000
#define PUBLISHER_SOURCE 2
#define PUBLISHER_ENDPOINT 0xabcdef01U
#define OTHER_SOURCE 3
#define NS_PER_S 1000000000ULL

static struct tt_Context context;
static struct tt_Discovery discovery;
static struct tt_Subscriber sub;

static uint64_t wall_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner) - <time.h>, via its own internal header
    return ((uint64_t)now.tv_sec * NS_PER_S) + (uint64_t)now.tv_nsec;
}

static int compare(const void* a, const void* b) {
    double lhs = *(const double*)a;
    double rhs = *(const double*)b;
    return (lhs > rhs) - (lhs < rhs);
}

// The publisher's entry at the end of a full table (last) or at its start (!last).
static void fill(bool last) {
    memset(&discovery, 0, sizeof(discovery));
    if (!last) {
        upsert_discovered_entity(&context, PUBLISHER_SOURCE, PUBLISHER_ENDPOINT, tt_KIND_TOPIC_PUBLISHER, 0, 0, 0, 0,
                                 "t", "p");
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES - 1U; i++) {
        upsert_discovered_entity(&context, OTHER_SOURCE, i + 1U, tt_KIND_TOPIC_PUBLISHER, 0, 0, 0, 0, "t", "o");
    }
    if (last) {
        upsert_discovered_entity(&context, PUBLISHER_SOURCE, PUBLISHER_ENDPOINT, tt_KIND_TOPIC_PUBLISHER, 0, 0, 0, 0,
                                 "t", "p");
    }
}

static double per_call_ns(void) {
    double samples[ROUNDS];
    volatile int incompatible = 0;
    for (int round = 0; round < ROUNDS; round++) {
        uint64_t start = wall_ns();
        for (int call = 0; call < CALLS; call++) {
            incompatible +=
                subscriber_incompatible_with_publisher(&context, &sub, PUBLISHER_SOURCE, PUBLISHER_ENDPOINT) ? 1 : 0;
        }
        samples[round] = (double)(wall_ns() - start) / CALLS;
    }
    qsort(samples, ROUNDS, sizeof(samples[0]), compare);
    return samples[ROUNDS / 2];
}

int main(void) {
    test_mock_reset();
    context.id = 1;
    node_init_locks(&context);
    context.discovery = &discovery;
    fill(true);
    double at_end = per_call_ns();
    fill(false);
    double at_start = per_call_ns();
    printf("RESULT: table=%d entry_last_ns=%.1f entry_first_ns=%.1f\n", tt_MAX_DISCOVERED_ENTITIES, at_end, at_start);
    return 0;
}
