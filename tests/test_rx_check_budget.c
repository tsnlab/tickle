/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// When a busy tt_Context_poll() looks at the socket between scheduler entries (tt_RX_CHECK_RATIO, config.h;
// ROADMAP.md 5a, 2026-10-05). A scheduler entry that reschedules itself forever - a max-rate publisher's send
// loop - is always due, so only this check lets the poll hear an ACKNACK at all. It used to come after a fixed
// count of entries (tt_SCHEDULER_IO_INTERLEAVE, 8), whose period in time is that count times whatever an entry
// costs on the hardware at hand. It is now decided in time, from two quantities the loop measures itself:
// - the gap between checks never exceeds tt_RECEIVE_TIMEOUT, however slow an entry is
//   (test_slow_entries_cannot_starve_receive);
// - empty checks take about 1 / (tt_RX_CHECK_RATIO + 1) of a busy loop's time, however dear a check is
//   (test_dear_checks_keep_their_share).
//
// Through the mock HAL: each run of the entry moves the mock clock by ENTRY_NS, each empty tt_try_receive() by
// test_mock_try_receive_empty_advance_ns, and the hook records when the socket was looked at. The count-based
// loop fails both: 8 entries of 20 us are a 160 us gap, and 8 entries of 100 ns against a 2 us read spend 71% of
// the loop reading nothing. Both are what a slower or a faster platform than the one 8 was picked on does.
// - a read the clock cannot see leaves a budget of 0, and the loop then alternates a look with an entry
//   (test_a_free_read_alternates_with_entries).
// Mutants, each killed here: the cap on the gap removed (the slow-entry test fails); the measured cost dropped
// from the budget, so a check follows every entry (the share test fails); the check removed altogether (every
// test fails on the gap); the "an entry ran since the last look" guard removed (the free-read test fails).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) - whitebox, as test_poll_wait.c
#include "test_mock.h"

static uint64_t entry_ns;       // what one run of the entry costs
static uint64_t entry_total_ns; // time spent in the entry
static uint64_t entries;        // runs of the entry
static uint64_t checks;         // tt_try_receive() calls, each one an empty read
static uint64_t last_check_end; // when the socket was last finished with
static uint64_t longest_gap;    // longest stretch without a look at the socket
static bool counting;

// A loop that peeks forever with a clock that does not move would hang the binary; past this many checks the hook
// jumps the clock past any poll's budget instead, so the failure is an assertion.
#define CHECK_BACKSTOP 1000000U

// The max-rate publisher: costs entry_ns and is due again the moment it returns.
static void spin(struct tt_Context* node, uint64_t time, void* param) {
    (void)time;
    test_mock_now += entry_ns;
    if (counting) {
        entry_total_ns += entry_ns;
        entries++;
    }
    EXPECT_TRUE(tt_Context_schedule(node, test_mock_now, spin, param));
}

static void note_gap(void) {
    uint64_t gap = test_mock_now - last_check_end;
    if (gap > longest_gap) {
        longest_gap = gap;
    }
}

static void saw_check(void) {
    if (!counting) {
        return;
    }
    checks++;
    if (checks > CHECK_BACKSTOP) {
        test_mock_now += tt_SECOND;
    }
    note_gap();
    last_check_end = test_mock_now + test_mock_try_receive_empty_advance_ns;
}

// The stretch from the last look to the end of the poll counts too: a loop that never looks has one gap, all of it.
static void stop_counting(void) {
    note_gap();
    counting = false;
}

static void setup(struct tt_Context* node, uint64_t entry, uint64_t check) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    test_mock_reset();
    test_mock_now = tt_SECOND;
    test_mock_try_receive_empty_advance_ns = check;
    test_mock_try_receive_hook = saw_check;
    entry_ns = entry;
    counting = false;
    EXPECT_TRUE(tt_Context_schedule(node, test_mock_now, spin, NULL));
}

static void start_counting(void) {
    entry_total_ns = 0;
    entries = 0;
    checks = 0;
    longest_gap = 0;
    last_check_end = test_mock_now;
    counting = true;
}

// 20 us entries - a slow core, or a send that blocks - and a 50 us read. Whatever the loop has learned about the
// cost of a read, no stretch of scheduler work between two looks at the socket is longer than tt_RECEIVE_TIMEOUT,
// give or take the one entry that was already running when it ran out.
static void test_slow_entries_cannot_starve_receive(void) {
    struct tt_Context node;
    setup(&node, 20 * tt_MICROSECOND, 50 * tt_MICROSECOND);
    start_counting();

    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Context_poll(&node, 5 * tt_MILLISECOND));
    stop_counting();

    EXPECT_TRUE(entries > 10); // the run did scheduler work for the checks to interleave with
    const uint64_t bound = (uint64_t)tt_RECEIVE_TIMEOUT + entry_ns;
    if (longest_gap > bound) {
        fprintf(stderr, "longest gap without a receive check %llu ns, bound %llu ns\n", (unsigned long long)longest_gap,
                (unsigned long long)bound);
    }
    EXPECT_TRUE(longest_gap <= bound);
}

// 100 ns entries - a segment publish - and a 2 us read that finds nothing, as recvmmsg() without the io_uring hint.
// After a warm-up in which the loop learns what a read costs, empty reads take no more than their share:
// tt_RX_CHECK_RATIO units of scheduler work per unit of reading, with 10% for the entry that crosses the budget.
static void test_dear_checks_keep_their_share(void) {
    struct tt_Context node;
    setup(&node, 100, 2 * tt_MICROSECOND);
    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Context_poll(&node, tt_MILLISECOND)); // warm-up
    start_counting();

    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Context_poll(&node, 5 * tt_MILLISECOND));
    stop_counting();

    const uint64_t ratio = tt_RX_CHECK_RATIO;
    const uint64_t check_total_ns = checks * test_mock_try_receive_empty_advance_ns;
    EXPECT_TRUE(checks > 10);
    EXPECT_TRUE(entry_total_ns > 0);
    if (10 * ratio * check_total_ns > 11 * entry_total_ns) {
        fprintf(stderr, "empty reads %llu ns against %llu ns of scheduler work, ratio %llu\n",
                (unsigned long long)check_total_ns, (unsigned long long)entry_total_ns, (unsigned long long)ratio);
    }
    EXPECT_TRUE(10 * ratio * check_total_ns <= 11 * entry_total_ns);
    // and the check still happens: a gap no longer than the budget the measured cost allows, plus one entry
    EXPECT_TRUE(longest_gap <= (ratio * test_mock_try_receive_empty_advance_ns) + entry_ns);
}

// A read the clock cannot see - free in the mock, below the tick of a coarse clock - measures 0 and leaves a budget
// of 0: the loop then looks after every entry, and never twice with no entry between, which with a clock that does
// not move would be a loop that never ends.
static void test_a_free_read_alternates_with_entries(void) {
    struct tt_Context node;
    setup(&node, tt_MICROSECOND, 0);
    start_counting();

    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Context_poll(&node, tt_MILLISECOND));
    stop_counting();

    EXPECT_TRUE(entries >= 900); // a millisecond of 1 us entries, not a loop stuck on the socket
    EXPECT_TRUE(checks <= entries + 1);
    EXPECT_TRUE(longest_gap <= entry_ns);
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_slow_entries_cannot_starve_receive();
    test_dear_checks_keep_their_share();
    test_a_free_read_alternates_with_entries();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_rx_check_budget: all tests passed\n");
    return 0;
}
