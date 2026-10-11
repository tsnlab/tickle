/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Receive coalescing while a large sample arrives (DESIGN.md section 2, "Receive path"): what core asks the HAL to
// defer before a wait (large_expect(), struct tt_Context.rx_defer_ns), from the fragments placed between waits, on the
// mock clock. Each condition of large_expect() has a test here that its mutant in tests/mutants_rx_defer.py must turn
// red. The HAL's half - the deferral itself, and what ends it - is platform/linux/rx_defer_check.c, against real
// sockets.
//
// Built as rmw_tickle builds core: a 65507-byte datagram and large samples.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: large_expect() is static
#include "test_mock.h"

#define SOURCE 1
#define ENTITY 7
#define BUDGET 10U
#define INTERVAL_NS 1000U // between two fragments, unless a test says otherwise
#define START_NS 1000000ULL

static struct tt_Context node;
static uint8_t payload[LARGE_CONT_PAYLOAD];
static struct tt_Header header;

static void* test_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void test_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}

static void setup(void) {
    test_mock_reset();
    for (uint32_t i = 0; i < tt_LARGE_ASSEMBLIES; i++) {
        free(node.large.assemblies[i].buffer);
    }
    memset(&node, 0, sizeof(node));
    node.large.acquire = test_acquire;
    node.large.release = test_release;
    node.rx_defer_budget = BUDGET; // as a HAL that defers sets it at tt_bind()
    header.source = SOURCE;
    test_mock_now = START_NS;
}

// A best-effort assembly for the sample at seq_no, of count fragments.
static struct tt_LargeAssembly* claim(uint32_t seq_no, uint32_t count) {
    struct tt_LargeAssembly* assembly = large_assembly_claim(&node, NULL, NULL, SOURCE, ENTITY, seq_no, count);
    EXPECT_TRUE(assembly != NULL);
    return assembly;
}

// Fragments first..last (continuations, full) placed in that order.
static void place(struct tt_LargeAssembly* assembly, uint32_t first, uint32_t last) {
    for (uint32_t index = first; index <= last; index++) {
        struct large_fragment fragment = {
            &header, NULL, payload, LARGE_CONT_PAYLOAD, ENTITY, assembly->seq_no + index, index, assembly->frag_count,
            0,       0,    false};
        EXPECT_EQ_INT(1, large_place(&node, assembly, &fragment));
    }
}

// A wait at `now`: what core asks the HAL to defer.
static uint64_t wait_at(uint64_t now) {
    test_mock_now = now;
    large_expect(&node);
    return node.rx_defer_ns;
}

// A pass of `n` fragments arriving INTERVAL_NS apart from `*now`, then the wait after it.
static uint64_t pass(struct tt_LargeAssembly* assembly, uint32_t first, uint32_t n, uint64_t* now) {
    place(assembly, first, first + n - 1U);
    *now += (uint64_t)n * INTERVAL_NS;
    return wait_at(*now);
}

// Mid-sample: k = the budget, for k intervals as the pass measured them.
static void test_a_steady_sample_defers_the_budget(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim(100, 200);
    uint64_t now = START_NS;
    EXPECT_EQ_U64(0, pass(assembly, 1, 5, &now)); // the sample's first pass: nothing measured yet
    EXPECT_EQ_U64((uint64_t)BUDGET * INTERVAL_NS, pass(assembly, 6, 5, &now));
}

// The last stretch: half of what is still to come, so the reader catches up before the last one arrives.
static void test_the_last_stretch_defers_half_of_it(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim(100, 20);
    uint64_t now = START_NS;
    (void)pass(assembly, 1, 5, &now);
    // next_index 11: 9 to come, half of them 4.
    EXPECT_EQ_U64(4ULL * INTERVAL_NS, pass(assembly, 6, 5, &now));
}

// Fewer than four to come: k would be 1, a timer in place of the one wake it saves.
static void test_a_single_one_is_not_deferred_for(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim(100, 14);
    uint64_t now = START_NS;
    (void)pass(assembly, 1, 5, &now);
    EXPECT_EQ_U64(0, pass(assembly, 6, 5, &now)); // next_index 11: 3 to come, half 1
}

// Repairs are not waited for: what is to come counts from the highest index landed, not from what has landed.
static void test_holes_behind_the_front_are_not_waited_for(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim(100, 120);
    uint64_t now = START_NS;
    (void)pass(assembly, 1, 5, &now);
    // 100..104 land, 6..99 still missing: the front is at 105, 15 to come, half 7 - not min(110 missing, budget).
    EXPECT_EQ_U64(7ULL * INTERVAL_NS, pass(assembly, 100, 5, &now));
}

// Another sample's first pass is not measured against the last one's.
static void test_a_new_sample_starts_unmeasured(void) {
    setup();
    struct tt_LargeAssembly* first = claim(100, 200);
    struct tt_LargeAssembly* second = claim(300, 200);
    uint64_t now = START_NS;
    (void)pass(first, 1, 5, &now);
    EXPECT_EQ_U64(0, pass(second, 1, 5, &now));
}

// The same entry claimed again is another sample.
static void test_a_reused_entry_is_another_sample(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim(100, 200);
    uint64_t now = START_NS;
    (void)pass(assembly, 1, 5, &now);
    large_assembly_free(&node, assembly);
    struct tt_LargeAssembly* again = claim(300, 200);
    EXPECT_TRUE(again == assembly); // the same entry - only its claim tells the two apart
    EXPECT_EQ_U64(0, pass(again, 1, 5, &now));
}

// A sample given up between two waits is not deferred for.
static void test_an_abandoned_sample_is_not_deferred_for(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim(100, 200);
    uint64_t now = START_NS;
    (void)pass(assembly, 1, 5, &now);
    place(assembly, 6, 10);
    large_assembly_abandon(&node, assembly);
    now += 5ULL * INTERVAL_NS;
    EXPECT_EQ_U64(0, wait_at(now));
}

// A wait that followed nothing breaks the chain: the next pass is not measured across the idle gap.
static void test_an_idle_wait_breaks_the_measurement(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim(100, 200);
    uint64_t now = START_NS;
    (void)pass(assembly, 1, 5, &now);
    now += 1000000ULL; // a millisecond with nothing
    EXPECT_EQ_U64(0, wait_at(now));
    EXPECT_EQ_U64(0, pass(assembly, 6, 5, &now));
    EXPECT_EQ_U64((uint64_t)BUDGET * INTERVAL_NS, pass(assembly, 11, 5, &now)); // and the one after is measured
}

// Capped at twice the budget at the fastest interval measured: a pass that measures slow - a sender that paused -
// does not stretch the wait past what the buffer holds at the speed the link has shown it can reach.
static void test_the_fastest_rate_seen_caps_the_wait(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim(100, 400);
    uint64_t now = START_NS;
    (void)pass(assembly, 1, 5, &now);
    (void)pass(assembly, 6, 5, &now); // INTERVAL_NS: the fastest so far
    place(assembly, 11, 15);
    now += 5ULL * 100U * INTERVAL_NS; // a pass at a hundredth of the rate
    EXPECT_EQ_U64(2ULL * BUDGET * INTERVAL_NS, wait_at(now));
}

// A HAL that does not defer (no budget) costs the wait no clock read.
static void test_no_budget_reads_no_clock(void) {
    setup();
    node.rx_defer_budget = 0;
    struct tt_LargeAssembly* assembly = claim(100, 200);
    uint64_t now = START_NS;
    test_mock_clock_step_ns = 1; // every clock read moves the mock clock: a read shows
    (void)pass(assembly, 1, 5, &now);
    place(assembly, 6, 10);
    test_mock_now = now;
    large_expect(&node);
    EXPECT_EQ_U64(0, node.rx_defer_ns);
    EXPECT_EQ_U64(now, test_mock_now);
}

int main(void) {
    memset(payload, 0x5C, sizeof(payload));
    test_a_steady_sample_defers_the_budget();
    test_the_last_stretch_defers_half_of_it();
    test_a_single_one_is_not_deferred_for();
    test_holes_behind_the_front_are_not_waited_for();
    test_a_new_sample_starts_unmeasured();
    test_a_reused_entry_is_another_sample();
    test_an_abandoned_sample_is_not_deferred_for();
    test_an_idle_wait_breaks_the_measurement();
    test_the_fastest_rate_seen_caps_the_wait();
    test_no_budget_reads_no_clock();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_rx_defer: all tests passed\n");
    return 0;
}
